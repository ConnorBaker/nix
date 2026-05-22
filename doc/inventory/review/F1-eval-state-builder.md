# F1 — `nix_eval_state_builder` dead-storage verification

**Charter:** Verify E2's flagged "dead-storage oddity" in
`nix_eval_state_builder_load`. Decide whether it is (a) dead code,
(b) latent bug, or (c) intentional defensive behaviour, and add it to
the catalog or fix it as appropriate.

**Verdict (TL;DR).** **Defensive default — but the per-builder storage
is materially load-bearing, not dead.** The "dead-storage" framing in
E2's open-question prose is wrong: the `bool` initialised by `_new` is
**only** dead on the `_new`+`_load` path (i.e. inside
`nix_state_create` and any embedder that mirrors that pipeline). On
the `_new`-without-`_load` path — which is exercised by **four
in-tree tests** in `libexpr-tests` and `libflake-tests` — the
per-builder `bool` is the *sole* source of `EvalSettings::isReadOnly()`
truth, and removing it would reduce a non-null pointer to a dangling
one (or worse, swap a documented `true` default for a silent `false`
under #138's proposed rewiring).

---

## The pattern

### What `_new` does

`nix_eval_state_builder_new(context, store)` allocates a fresh builder
with the following members initialised:

```cpp
auto readOnly = nix::make_ref<bool>(true);
return new nix_eval_state_builder{
    .store         = nix::ref<nix::Store>(store->ptr),
    .settings      = nix::EvalSettings{/* &bool */ *readOnly},
    .fetchSettings = nix::fetchers::Settings{},
    .readOnlyMode  = readOnly,
};
```

Two things to note here:

1. The `nix::make_ref<bool>(true)` is a heap-allocated `bool` whose
   *value* is `true`. The `nix::ref<bool>` is a non-null
   reference-counted pointer to that storage. The builder keeps it
   alive in `builder->readOnlyMode`.
2. `nix::EvalSettings{*readOnly}` invokes
   `EvalSettings::EvalSettings(bool & readOnlyMode, …)`, whose body
   is `: readOnlyMode{&readOnlyMode}` — i.e., it stores `&readOnly` in
   the `bool * EvalSettings::readOnlyMode` field.

After `_new`, `builder->settings.readOnlyMode` aliases the
heap-allocated `true` owned by `builder->readOnlyMode`. The aliasing
is intentional: `EvalSettings` *only* exposes `bool * readOnlyMode`
plus an `isReadOnly()` accessor that asserts non-null and dereferences
it. Owning the storage outside `EvalSettings` is how the existing
design avoids `EvalSettings` itself owning the bool (because the
intent is for the bool to live elsewhere — in `Store::config` per
candidate #138).

### What `_load` does

`nix_eval_state_builder_load(context, builder)` then:

```cpp
// TODO: load in one go?
builder->settings.readOnlyMode = &nix::settings.readOnlyMode;
loadConfFile(builder->settings);
loadConfFile(builder->fetchSettings);
```

The first line repoints the `EvalSettings::readOnlyMode` *pointer*
away from `*builder->readOnlyMode` (the per-builder `true`) and onto
`&nix::settings.readOnlyMode` (the global `Settings::readOnlyMode`,
default `false`).

After `_load` runs, `builder->readOnlyMode` (the `nix::ref<bool>`
heap cell holding `true`) is **no longer aliased by anything that
reads it**. That's the "dead storage" E2 flagged: the heap-allocated
`bool` cell still exists for the lifetime of the builder, but
`EvalSettings::isReadOnly()` no longer dereferences it.

### Where the storage *is* read

`EvalSettings::isReadOnly() const`
(`src/libexpr/include/nix/expr/eval-settings.hh`):

```cpp
bool isReadOnly() const
{
    assert(readOnlyMode);
    return *readOnlyMode;
}
```

`EvalState::store` callers gate on `isReadOnly()` to decide whether
to mutate the store. That's the only place the `bool *` is
dereferenced.

In `nix_eval_state_build`, the builder's `settings` is moved into a
heap-allocated `EvalSettings` that the resulting `EvalState` owns:

```cpp
auto settings = std::make_unique<nix::EvalSettings>(std::move(builder->settings));
```

Critical detail: `EvalSettings` is a `Config`-derived class with
member `Setting<…>` fields whose `this` pointers refer back to the
parent `Config` — the move constructor for these fields is
implementation-defined, but the `bool *` member is a plain pointer,
so it is bit-copied across the move. **The pointer survives the move
unchanged.** That's what makes the `_load`-overwrites-the-pointer
pattern actually work: after `_build`, the `EvalState`'s
`EvalSettings` still points at `&nix::settings.readOnlyMode` (if
`_load` ran) or at `*builder->readOnlyMode` (if it didn't).

But the `nix::ref<bool> readOnlyMode` member of the *builder* is a
field of `nix_eval_state_builder`, not of `EvalSettings`. It is
**dropped on the floor when `nix_eval_state_builder_free` runs**.
Which means: if `_load` did not run, the `EvalState` is now holding
an `EvalSettings` whose `bool *` points to *freed memory*. **This is
a use-after-free in the `_new`-without-`_load` path.**

…unless `nix::ref<bool>` keeps the pointee alive past
`nix_eval_state_builder_free`. Let me verify.

### `nix::ref` lifetime

<see `src/libutil/include/nix/util/ref.hh`>

`nix::ref<T>` wraps `std::shared_ptr<T>` (rejects nullptr at
construction). So `nix::ref<bool>` is shared ownership: the heap
cell stays alive while *any* `ref<bool>` (or the underlying
`shared_ptr`) references it. When `nix_eval_state_builder_free` runs
and deletes the builder, the builder's `nix::ref<bool>` member is
destroyed — its refcount drops by one. If no other ref holds the
cell, the cell is freed.

Now: does anyone else hold a ref/shared_ptr to that bool? Let me
re-read `_build`:

```cpp
auto fetchSettings = std::make_unique<nix::fetchers::Settings>(std::move(builder->fetchSettings));
auto settings = std::make_unique<nix::EvalSettings>(std::move(builder->settings));
auto ownedState = std::make_shared<nix::EvalState>(builder->lookupPath, builder->store, *fetchSettings, *settings);
auto & stateRef = *ownedState;
void * p = ::operator new(sizeof(EvalState), static_cast<std::align_val_t>(alignof(EvalState)));
return new (p) EvalState{stateRef, std::move(fetchSettings), std::move(settings), std::move(ownedState)};
```

`builder->readOnlyMode` (the `nix::ref<bool>`) is **not moved**, not
copied, not transferred into the `EvalState`. The `EvalState`
captures `settings` (the moved-from `EvalSettings`), which contains
the *raw `bool *`* — but the *only* shared owner of the heap-allocated
bool is the builder. **When `nix_eval_state_builder_free` runs, the
heap bool is freed, and the `EvalSettings::readOnlyMode` pointer in
the resulting `EvalState` becomes dangling.**

This is a **latent use-after-free** on the `_new`-without-`_load`
path. The four in-tree tests that do `_new` + `_build` + `_free` (no
`_load`) hit this: any subsequent call to `EvalSettings::isReadOnly()`
reads from freed memory.

In practice, the four tests probably don't trigger
`isReadOnly()`-gated code paths during their `nix_expr_eval_from_string`
calls (e.g., simple expressions like `1 + 1`, `builtins.nixVersion`,
or pure flake-eval that doesn't hit `addToStore`/`buildPaths`), so
the bug is silent. ASan/Valgrind would catch it under any test that
does cause an `isReadOnly()` query.

---

## Categorisation

The original three options are inadequate. The actual situation is:

- **Not dead code.** The per-builder `bool` is read on the
  `_new`-without-`_load` path (exercised by four in-tree tests).
  Deleting it changes observable behaviour (and the API contract).
- **Not a latent bug in the way E2's prose suggests.** The
  `_load`-overwrites-the-pointer is intentional: the documentation
  says `_load` "Read[s] settings from the ambient environment", and
  routing `readOnlyMode` to the global `nix::settings.readOnlyMode`
  is consistent with the embedder asking for env+nix.conf-driven
  configuration.
- **Defensive default — but with a hidden lifetime bug.** The `_new`
  defaults to `true` (read-only), which is the safe choice for
  embedders who never call `_load`: a CLI-style embedder that wants
  side effects calls `_load` (which clobbers the pointer); an
  embedder that skips `_load` keeps the safe default… *but* loses
  the storage backing the pointer when it frees the builder.

So the categorisation is **(c) defensive default that the design
needs**, but **with an adjacent latent UAF** that the catalog has not
previously surfaced.

The defensive-default story:

- The C-API documentation (`nix_api_expr.h`) says of `_new`: "The
  settings are initialized to their default value. Values can be
  sourced elsewhere with nix_eval_state_builder_load." That's a
  promise that `_load` is **optional** — the embedder gets a working
  default if they skip it.
- Defaulting to `readOnlyMode = true` is a defensible safety choice:
  an embedder who hasn't yet plumbed in any settings should not
  accidentally write to a Nix store. Nothing in the public contract
  says the default must be `false`.

The latent UAF story:

- The sole owner of the heap-allocated `bool` is the builder's
  `nix::ref<bool> readOnlyMode`. The resulting `EvalState` does not
  share ownership.
- After `nix_eval_state_builder_free`, if `_load` did not run, the
  `EvalSettings::readOnlyMode` raw pointer in the live `EvalState`
  is dangling.
- If `_load` *did* run, the pointer aliases `&nix::settings.readOnlyMode`,
  which is a static global with program-lifetime, so no UAF on that
  path.

---

## Public-API contract

From `nix_api_expr.h`:

> `nix_eval_state_builder * nix_eval_state_builder_new(...)`
> The settings are initialized to their default value. Values can be
> sourced elsewhere with nix_eval_state_builder_load.

> `nix_err nix_eval_state_builder_load(...)`
> Read settings from the ambient environment.
> Settings are sourced from environment variables and configuration
> files, as documented in the Nix manual.

The header explicitly does **not** require `_load` after `_new`. The
embedder can construct a builder, call `_set_lookup_path`, and call
`_build` without ever invoking `_load`. The
`nix_state_create` convenience wrapper *does* call `_load`, but that
is a convenience composition, not a contract that pins the order.

The `_load` contract is "ambient env+nix.conf". Pointing
`readOnlyMode` at `&nix::settings.readOnlyMode` is consistent with
that contract — it is, after all, the global setting that ambient
env (`NIX_REMOTE`, etc.) and `nix.conf` actually mutate.

---

## Caller survey

Searched all in-tree consumers of `nix_eval_state_builder_new` /
`_load` / `_build` / `nix_state_create`:

| File | Pattern |
| --- | --- |
| `src/libexpr-c/nix_api_expr.cc` :: `nix_state_create` | new + load + set_lookup_path + build + free |
| `src/libexpr-tests/nix_api_expr.cc` :: `nix_eval_state_lookup_path` | **new + set_lookup_path + build + free (no load)** |
| `src/libexpr-tests/nix_api_expr.cc` :: `nix_expr_eval_drv` (and others) | `nix_state_create` (= new+load+...) |
| `src/libexpr-tests/nix_api_external.cc` (×2) | `nix_state_create` |
| `src/libexpr-test-support/include/nix/expr/tests/nix_api_expr.hh` | `nix_state_create` |
| `src/libflake-tests/nix_api_flake.cc` :: `nix_api_init_getFlake_exists` | **new + flake_settings_add_to_eval_state_builder + build + free (no load)** |
| `src/libflake-tests/nix_api_flake.cc` :: `nix_api_load_flake` | **new + build + free (no load)** |
| `src/libflake-tests/nix_api_flake.cc` :: `nix_api_load_flake` (third test, line 220) | **new + build + free (no load)** |
| Documentation example in `doc/manual/source/release-notes/rl-2.28.md` | new + flake_settings_add_to_eval_state_builder + ... (no `_load` shown) |
| Documentation example in `src/external-api-docs/README.md` | `nix_state_create` |

Four of nine in-tree call sites take the `_new`-without-`_load`
path. The release-notes example also documents that pattern (with
`flake_settings_add_to_eval_state_builder`, which does not call
`_load`). So the `_new`-without-`_load` path is **not** an
unintentional caller mistake; it is an **intentional, documented**
usage shape.

---

## Decision

**Three independent issues, three different fixes.**

### Issue 1: latent UAF on `_new`-without-`_load`

**This is a real bug** — call it a forwardable defect on top of the
existing settings design. Fix: make the resulting `EvalState`
co-own the `nix::ref<bool>` storage. Concretely, in `_build`:

```cpp
auto readOnlyMode = std::move(builder->readOnlyMode);  // shared_ptr<bool>
auto settings = std::make_unique<nix::EvalSettings>(std::move(builder->settings));
// keep readOnlyMode alive alongside settings in the EvalState handle
```

…and add a `std::shared_ptr<bool> ownedReadOnlyMode;` (or, given that
`EvalSettings` is the only consumer, store it adjacent to
`ownedSettings`) to `struct EvalState`.

This costs one extra `shared_ptr` field and one move. It is a
mechanical fix.

Note that `_load`-having-run does not break this fix: after `_load`,
`builder->settings.readOnlyMode` aliases `&nix::settings.readOnlyMode`
(a static global), and the `nix::ref<bool>` storage is no longer
referenced by anyone — `_build` would still keep the now-orphan
`ref<bool>` alive in `EvalState::ownedReadOnlyMode`, which is
harmless (one byte of dead heap storage that lives until the
`EvalState` is freed). That's a small price for closing the UAF on
the other path.

**Alternative:** drop the `_load`-rewrites-pointer pattern entirely,
and have `_load` write *into* `*builder->readOnlyMode` (i.e.,
`*builder->readOnlyMode = nix::settings.readOnlyMode;` — read once
at load time). That keeps the `nix::ref<bool>` as the sole owner and
eliminates the rewrites. But it changes the **observable semantics**:
under the current code, if the embedder mutates
`nix::settings.readOnlyMode` *after* `_load` runs and *before*
`_build` finalises, the `EvalState`-side `isReadOnly()` reflects the
post-`_load` value. Under the alternative, it would not.

For a #138-aware refactor (where `readOnlyMode` migrates onto
`Store`/`StoreConfig`), Issue 1's fix becomes moot: `EvalSettings`
loses the `bool *` field entirely and queries the `Store`. So the
mechanical "co-own the ref<bool>" fix is an interim cleanup that the
#138 refactor will sweep away. **Recommendation:** fold the fix into
the same patch as #138, not as standalone churn.

### Issue 2: the `_load`-rewrites-pointer pattern

This is the "dead-storage oddity" E2 flagged. It is **intentional**
and **load-bearing** for the documented `_load` contract ("read from
the ambient environment"). The fact that it makes the per-builder
storage dead *after* `_load` runs is an artifact of the current
`bool *`-aliasing design of `EvalSettings`, not a defect in itself.

**Recommendation:** no standalone change. The pattern is
self-consistent given the current `EvalSettings::readOnlyMode`
design. #138 deletes the line entirely.

### Issue 3: the implicit `_new`-default of `true`

The C-API doc comment says the settings get "their default value"
without saying which value `readOnlyMode` defaults to.

`nix::Settings::readOnlyMode` defaults to `false` (in `globals.hh`).
The C-API builder defaults `readOnlyMode` to `true`. **The two
defaults disagree.**

So an embedder who does `_new + _build` (no `_load`) gets read-only;
an embedder who does `_new + _load + _build` gets the global default
(`false`, unless `nix.conf` or env says otherwise). A subtle behaviour
gap for what looks like the same builder.

The `true` default is *defensible* (safer for naive embedders), but
the discrepancy with the global default is not documented in
`nix_api_expr.h`. **Recommendation:** document the default in the
`_new` doxygen comment. One-line addition.

### Catalog placement

The **defensive default + lifetime bug** is a new catalog entry that
the existing C-API debt cluster (N24/N25/N29 in
`23-c-api-debt.md`) does not cover. The cluster covers structural
boilerplate (opaque-pointer wrappers, init idempotency, error-catch
macros) — it does not catalog **semantics**. Adding a new candidate
keeps the structural cluster clean and surfaces the substantive bug.

**Proposed candidate (new entry in `candidates/23-c-api-debt.md`):**

> N30. **`nix_eval_state_builder` storage lifetime for `readOnlyMode`
> is unsound on the `_new`-without-`_load` path.** The builder owns
> a `nix::ref<bool> readOnlyMode` that backs `EvalSettings::readOnlyMode`
> (a raw `bool *`). On `_build`, the `EvalSettings` is moved into the
> resulting `EvalState`, but the `nix::ref<bool>` is **not** moved —
> it is destroyed when `nix_eval_state_builder_free` runs, leaving
> the `EvalState`'s `EvalSettings::readOnlyMode` raw pointer
> dangling. The `_load`-rewrites-pointer code in
> `nix_eval_state_builder_load` happens to mask the bug for callers
> that go through `_new + _load + _build`: after `_load`, the
> pointer aliases `&nix::settings.readOnlyMode` (a static global,
> program-lifetime), so freeing the `nix::ref<bool>` does not
> dangle anything in that case. Four in-tree tests
> (`libexpr-tests/nix_api_expr.cc::nix_eval_state_lookup_path`,
> `libflake-tests/nix_api_flake.cc::nix_api_init_getFlake_exists`,
> ::`nix_api_load_flake` ×2) and the
> `rl-2.28.md` documented usage exercise the unsound path.
>
> **Validation:** VALID. Two fix options:
> 1. (Mechanical, interim) Move the `nix::ref<bool>` from the
>    builder into `struct EvalState` so it co-owns the storage.
>    Adds one `shared_ptr` member to `EvalState`. Does not change
>    the public C ABI.
> 2. (Strategic, ties into #138) Eliminate the `bool *` field on
>    `EvalSettings` by routing `isReadOnly()` through
>    `EvalState::store->config->readOnly()`. Deletes the
>    `nix::ref<bool>` field on the builder, deletes the
>    `_load`-rewrites-pointer line, deletes the `bool *
>    readOnlyMode` field on `EvalSettings` and its constructor
>    parameter. This is exactly what candidate #138 already
>    proposes; this candidate ties #138 to the C-API soundness
>    motivation.
>
> Also: document the `_new`-default of `readOnlyMode = true` in
> `nix_api_expr.h`'s `_new` doxygen comment, since it disagrees
> with the global `nix::settings.readOnlyMode = false` default.
> One-line doc addition. Effort: small (option 1 + doc) or none
> (folded into #138).
>
> **Compounds with #138** (relocate `readOnlyMode` to Store), and
> with the existing E2 audit's open question 3.

---

## Cross-references

- **Candidate #50** (`nix_value_incref/_decref` are pure forwarders).
  Different debt class: structural duplication, not lifetime
  unsoundness. No overlap.
- **Candidate #87** (`get_*_byidx{,_lazy}` lazy/forced pairs).
  Different debt class. No overlap.
- **Candidate #88** (`nix_<libname>_init` idempotency family).
  Different debt class. No overlap.
- **Candidate N24** (opaque-pointer wrappers).
  *Tangential.* `nix_eval_state_builder` is one of N24's "outliers
  with multiple fields" — and the `nix::ref<bool>` is one of those
  fields. N24's proposed `OpaqueRef`/`OpaqueValue` template would
  *not* simplify `nix_eval_state_builder` because of its multi-field
  shape; the lifetime bug is independent of the template question.
- **Candidate N25** (per-library init idempotency).
  Different debt class (init vs. settings lifetime). No overlap.
- **Candidate #138** (relocate `readOnlyMode` from
  `EvalSettings` / global `Settings` to `Store`/`StoreConfig`).
  **Direct overlap.** #138's plan to delete the `bool *
  readOnlyMode` field on `EvalSettings` *automatically* fixes the
  UAF described here (no field, no dangling pointer). The new N30
  candidate above ties the soundness argument to #138's motivation:
  even if you do not care about the diamond/composition cleanup,
  the C-API has a UAF that #138 fixes for free. This is a stronger
  argument than the "tidy up the settings hierarchy" framing in the
  current #138 prose.
- **E2 (this file's predecessor) "open question 3"** explicitly
  asked whether the `_load`-rewrites-pointer was intentional. The
  answer documented here: **the rewrite is intentional, but the
  underlying lifetime is unsound on the path that does not run
  `_load`**. E2's suggestion that "either keep the per-builder
  `bool` as the source of truth (and let the embedder set it
  explicitly), or route through the `Store`'s `readOnly()` and
  drop the per-builder `bool` altogether" is correct in spirit;
  this report concludes the latter is the right path because #138
  is already moving in that direction.

---

## Open questions

1. **Is the `readOnlyMode = true` default for the
   `_new`-without-`_load` path the *intended* default, or is it an
   artifact of how the C-API author happened to write
   `make_ref<bool>(true)`?** Git blame on
   `nix_eval_state_builder_new` might tell us. If the original
   author thought "let me pick the safer default", documenting it
   is the right move. If they typed `true` arbitrarily, **changing
   it to `false`** to match the global default is a candidate
   behaviour change — though that is a soft API change (any
   existing embedder that relied on the builder defaulting to
   read-only would silently flip to writable).

2. **Does the `EvalState` move constructor / move assignment
   propagate the `EvalSettings`'s `bool *` correctly?** The current
   code stores the `EvalSettings` inside a `unique_ptr` (so the
   `EvalSettings` itself does not move further once placed in the
   `EvalState`), which sidesteps the question. But if a future
   refactor inlines `EvalSettings` by value into `EvalState`, the
   move semantics need a re-check — `Setting<T>` fields hold
   parent-pointers and can break if moved without explicit
   move-rebinding logic.

3. **Are there ASan/UBSan coverage gaps that hide this UAF in CI?**
   Verifying with a sanitizer run on
   `libflake-tests::nix_api_init_getFlake_exists` (and the other
   three offending tests) under ASan would either confirm the UAF
   is reachable (sanitizer fires) or confirm the four tests never
   trigger an `isReadOnly()` query (sanitizer is silent). Either
   outcome strengthens the catalog entry. Not done as part of this
   investigation; flagged for a follow-up agent that has a build
   environment with sanitizers wired in.

4. **Does any out-of-tree consumer rely on the `_load` rewrite
   targeting `&nix::settings.readOnlyMode` specifically (vs. a
   read-once-at-load value)?** This matters if option 1 of the fix
   is replaced by the "alternative" mentioned above
   (`*builder->readOnlyMode = nix::settings.readOnlyMode;`). If any
   consumer mutates `nix::settings.readOnlyMode` post-`_load` and
   expects the change to propagate to the live `EvalState`, the
   alternative breaks them. Likely zero such consumers exist, but
   the C-API is a stable surface and "likely zero" is not the same
   as "verified zero". The mechanical fix (option 1) avoids this
   risk.
