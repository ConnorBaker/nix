# V3 — F1 lifetime trace verification

**Charter.** Verify or refute F1's claim that the `_new`-without-`_load`
path of `nix_eval_state_builder` leaves a dangling `bool *` in the
resulting `EvalState`'s `EvalSettings::readOnlyMode`. Method: pure source
reading. No reproducer. No sanitizer.

**Verdict (TL;DR).** **Code-trace confirms F1.** Every step of the
lifetime claim is unambiguously established by the source. The
`_new`-without-`_load` path produces an `EvalState` whose
`EvalSettings::readOnlyMode` raw pointer points to memory freed by
`nix_eval_state_builder_free`. The pointer is dereferenced by
`EvalState::ensureLazyPathCopied` and the `addToStore` path in
`EvalState::copyPathToStore` (in `eval.cc`). The bug is real.

---

## Method

Files read end-to-end:

- `src/libexpr-c/nix_api_expr.cc` (entire file, 271 lines)
- `src/libexpr-c/nix_api_expr_internal.h` (entire file, 77 lines)
- `src/libexpr/include/nix/expr/eval-settings.hh` (entire file, 483 lines)
- `src/libutil/include/nix/util/ref.hh` (entire file, 173 lines)

Targeted reads (relevant regions only, with end-to-end review of the
referenced declarations):

- `src/libexpr/include/nix/expr/eval.hh` — `EvalState` class declaration
  and constructor signature (the `settings` member, the doc comment
  "Must outlive the lifetime of this EvalState!").
- `src/libexpr/eval-settings.cc` — `EvalSettings::EvalSettings(bool &)`
  constructor body.
- `src/libutil/include/nix/util/configuration.hh` — `Config` class
  declaration (no user-defined move ctor / move-assignment); confirms
  the implicit move bit-copies the `bool *` pointer.
- `src/libstore/include/nix/store/globals.hh` — `Settings::readOnlyMode`
  member (default `false`) and `extern nix::Settings settings`.
- `src/libexpr/eval.cc` and `src/libexpr/paths.cc` — call sites for
  `settings.isReadOnly()` (the dereference site).
- `src/libexpr-tests/nix_api_expr.cc` — the
  `nix_eval_state_lookup_path` test that exercises the no-load path.

---

## Source-level definitions

**Builder struct** (`src/libexpr-c/nix_api_expr_internal.h`, struct
`nix_eval_state_builder`):

```
struct nix_eval_state_builder
{
    nix::ref<nix::Store> store;
    nix::EvalSettings settings;
    nix::fetchers::Settings fetchSettings;
    nix::LookupPath lookupPath;
    nix::ref<bool> readOnlyMode;
};
```

The member `readOnlyMode` is a `nix::ref<bool>`, i.e. shared-ownership
of a heap-allocated `bool` cell.

**C-API EvalState wrapper** (same file, struct `EvalState`):

```
struct EvalState
{
    nix::EvalState & state;
    std::unique_ptr<nix::fetchers::Settings> ownedFetchSettings;
    std::unique_ptr<nix::EvalSettings> ownedSettings;
    std::shared_ptr<nix::EvalState> ownedState;
};
```

There is **no** `std::shared_ptr<bool>` or `nix::ref<bool>` member here.
The wrapper does not co-own the `bool` cell.

**`EvalSettings::readOnlyMode`**
(`src/libexpr/include/nix/expr/eval-settings.hh`, struct `EvalSettings`):

```
bool * readOnlyMode = nullptr;

bool isReadOnly() const
{
    assert(readOnlyMode);
    return *readOnlyMode;
}
```

A raw `bool *`. `isReadOnly()` asserts non-null and dereferences.

**`EvalSettings` constructor**
(`src/libexpr/eval-settings.cc`, `EvalSettings::EvalSettings(bool &
readOnlyMode, ...)`):

```
: readOnlyMode{&readOnlyMode}
```

Stores the address of the referenced `bool` in the `bool *` member.

**`nix::ref<T>`** (`src/libutil/include/nix/util/ref.hh`, class `ref`):

```
template<typename T>
class ref
{
    std::shared_ptr<T> p;
    ...
};

template<typename T, typename... Args>
inline ref<T> make_ref(Args &&... args)
{
    auto p = std::make_shared<T>(std::forward<Args>(args)...);
    return ref<T>(p);
}
```

`nix::ref<bool>` wraps `std::shared_ptr<bool>`. `nix::make_ref<bool>(true)`
heap-allocates a `bool` initialised to `true` and returns a
shared-owning handle.

**`nix::EvalState::settings`** (`src/libexpr/include/nix/expr/eval.hh`,
class `EvalState`):

```
const EvalSettings & settings;
```

A reference. Doc comment on the constructor's `settings` parameter:
"Must outlive the lifetime of this EvalState!" — the `EvalState` does
not own the `EvalSettings`; the caller is responsible for keeping it
alive.

**`nix::settings.readOnlyMode`** (`src/libstore/include/nix/store/globals.hh`,
class `Settings`):

```
bool readOnlyMode = false;
...
extern nix::Settings settings;
```

A non-static member of an `extern` global. Program-lifetime.

**`Config` move semantics** (`src/libutil/include/nix/util/configuration.hh`,
class `Config`): the class declares no user-defined move constructor or
move-assignment operator. The implicit moves apply, which bit-copy the
`bool *` member of the derived `EvalSettings`. F1's claim "the bool * is
bit-copied across the move" is correct.

---

## Trace: WITHOUT `_load`

### Step 1: `_new`

Source: `nix_eval_state_builder_new` in
`src/libexpr-c/nix_api_expr.cc`.

```
auto readOnly = nix::make_ref<bool>(true);
return new nix_eval_state_builder{
    .store = nix::ref<nix::Store>(store->ptr),
    .settings = nix::EvalSettings{/* &bool */ *readOnly},
    .fetchSettings = nix::fetchers::Settings{},
    .readOnlyMode = readOnly,
};
```

- A `bool` cell holding `true` is heap-allocated via `std::make_shared`
  inside `nix::make_ref<bool>(true)` (per `make_ref` in `ref.hh`).
- The local `readOnly` (a `nix::ref<bool>`) holds the **sole** owning
  handle at this instant.
- `nix::EvalSettings{*readOnly}` invokes
  `EvalSettings::EvalSettings(bool & readOnlyMode, ...)`. The reference
  parameter binds to the heap `bool`, and the constructor stores
  `&readOnlyMode` (i.e. the address of the heap cell) in
  `EvalSettings::readOnlyMode`.
- The aggregate initialiser places `readOnly` into
  `nix_eval_state_builder::readOnlyMode` (a copy of the
  `nix::ref<bool>`, increasing the underlying `shared_ptr`'s refcount).

After `_new` returns, the heap `bool` has refcount 1 (held by
`builder->readOnlyMode`). The local `readOnly` has gone out of scope.
The `bool *` in `builder->settings.readOnlyMode` aliases that heap
cell.

The storage-class of the `bool` is **heap, shared-owned by
`std::shared_ptr<bool>`**. The sole owner is the builder.

### Step 2: post-`_new` state

- `builder->readOnlyMode` (type `nix::ref<bool>`) holds the only
  shared-ownership handle.
- `builder->settings.readOnlyMode` (type `bool *`) aliases the same
  heap cell. This is a non-owning raw pointer.

### Step 3: `_build`

Source: `nix_eval_state_build` in `src/libexpr-c/nix_api_expr.cc`.

```
auto fetchSettings = std::make_unique<nix::fetchers::Settings>(std::move(builder->fetchSettings));
auto settings = std::make_unique<nix::EvalSettings>(std::move(builder->settings));
auto ownedState =
    std::make_shared<nix::EvalState>(builder->lookupPath, builder->store, *fetchSettings, *settings);
auto & stateRef = *ownedState;
void * p = ::operator new(sizeof(EvalState), static_cast<std::align_val_t>(alignof(EvalState)));
return new (p) EvalState{stateRef, std::move(fetchSettings), std::move(settings), std::move(ownedState)};
```

What is moved/copied/transferred:

- `builder->fetchSettings` is **moved** into a heap
  `nix::fetchers::Settings` owned by `unique_ptr<...> fetchSettings`.
- `builder->settings` (type `nix::EvalSettings`) is **moved** into a
  heap `nix::EvalSettings` owned by `unique_ptr<...> settings`. The
  move is implicit (no user-defined move ctor on `Config` or
  `EvalSettings`); the `bool * readOnlyMode` member is **bit-copied**
  to the new heap `EvalSettings`. The pointer still aliases the same
  heap `bool` that `builder->readOnlyMode` owns.
- `builder->store` is **copied** by value (it is a parameter to the
  `nix::EvalState` constructor; no `std::move` on `builder->store` in
  the `EvalState` ctor call). This is irrelevant to the bug but worth
  noting for completeness.
- `builder->lookupPath` is passed by const-reference into the
  `EvalState` constructor. Used only during construction.
- The `nix::EvalState` is constructed via `std::make_shared`. Its
  constructor stores `*settings` as a `const EvalSettings &` member
  (per `eval.hh`). The `nix::EvalState` does not own the
  `nix::EvalSettings`; it relies on the caller (the C-API layer) to
  keep the `EvalSettings` alive.
- The C-API `EvalState` struct is placement-new'd with:
  - `state = stateRef` (reference to the heap `nix::EvalState`)
  - `ownedFetchSettings = std::move(fetchSettings)` (the unique_ptr)
  - `ownedSettings = std::move(settings)` (the unique_ptr — this is
    what keeps the heap `EvalSettings` alive)
  - `ownedState = std::move(ownedState)` (the shared_ptr to the
    `nix::EvalState`)

**`builder->readOnlyMode` is not moved, not copied, not transferred.**
The `nix::ref<bool>` in the builder remains the sole owner of the
heap `bool` cell. The new C-API `EvalState` wrapper has no field for
it.

After `_build`:

- The heap `bool` cell still has refcount 1, owned solely by
  `builder->readOnlyMode`.
- The new heap `EvalSettings` (inside the C-API `EvalState` wrapper's
  `ownedSettings`) holds a `bool * readOnlyMode` that aliases the same
  cell.
- The returned C-API `EvalState` does not co-own the `bool`.

### Step 4: `_free`

Source: `nix_eval_state_builder_free` in
`src/libexpr-c/nix_api_expr.cc`.

```
void nix_eval_state_builder_free(nix_eval_state_builder * builder)
{
    delete builder;
}
```

`delete builder` runs the implicit `nix_eval_state_builder` destructor,
which destroys all members in reverse declaration order. The member
`nix::ref<bool> readOnlyMode` is destroyed; this destroys its embedded
`std::shared_ptr<bool>`, which decrements the refcount on the heap
`bool` cell.

Since `builder->readOnlyMode` was the **sole** owner (no other
`std::shared_ptr`/`nix::ref` was created from it during `_build`), the
refcount drops from 1 to 0, and the heap `bool` cell is deallocated by
the `shared_ptr` deleter. The other members of the builder
(`store`, `settings`, `fetchSettings`, `lookupPath`) are also destroyed
— note that `builder->settings` is an already-moved-from
`nix::EvalSettings`; its destruction is irrelevant to the bug.

### Step 5: post-`_free` state

The C-API `EvalState` wrapper is still live and points to:

- `ownedSettings` → heap `nix::EvalSettings` whose `bool * readOnlyMode`
  member is **dangling** (it points at the deallocated heap `bool`
  cell from Step 4).
- `ownedState` → live `nix::EvalState` whose `const EvalSettings &
  settings` references that same dangling-pointer-bearing heap
  `EvalSettings`.

The `bool *` is dangling. Any read through it is a use-after-free.

### Step 6: read site

`EvalSettings::isReadOnly()` (declared in
`src/libexpr/include/nix/expr/eval-settings.hh`) dereferences
`readOnlyMode`:

```
bool isReadOnly() const
{
    assert(readOnlyMode);
    return *readOnlyMode;
}
```

The `assert(readOnlyMode)` check only verifies non-null; a dangling
non-null pointer passes this assert. The `return *readOnlyMode;` then
reads from freed memory.

Call sites of `settings.isReadOnly()`:

- `EvalState::ensureLazyPathCopied`
  (`src/libexpr/paths.cc`):

  ```
  void EvalState::ensureLazyPathCopied(const StorePath & path)
  {
      if (settings.isReadOnly())
          return;
      ...
  }
  ```

- `EvalState::copyPathToStore` (or equivalent, in
  `src/libexpr/eval.cc`):

  ```
  auto dstPath = fetchToStore(
      fetchSettings,
      *store,
      path.resolveSymlinks(SymlinkResolution::Ancestors),
      settings.isReadOnly() ? FetchMode::DryRun : FetchMode::Copy,
      ...);
  ```

Either of these can be reached during ordinary `EvalState` usage that
involves coercing a path expression to a store path (which is the case
for any expression that touches a `<lookup>` or path literal that needs
to land in the store). On the no-load path, the read here is from
freed memory.

---

## Trace: WITH `_load`

### Step 1, 2: `_new`

Identical to the no-load trace.

### Intermediate: `_load`

Source: `nix_eval_state_builder_load` in
`src/libexpr-c/nix_api_expr.cc`.

```
builder->settings.readOnlyMode = &nix::settings.readOnlyMode;
loadConfFile(builder->settings);
loadConfFile(builder->fetchSettings);
```

The first line **rebinds** `builder->settings.readOnlyMode` to point to
`&nix::settings.readOnlyMode`. The previous aliasing (which pointed at
the heap `bool` cell owned by `builder->readOnlyMode`) is overwritten.

After this line:

- `builder->readOnlyMode` (the `nix::ref<bool>`) still owns the heap
  `bool`, but **nothing references the cell anymore**: the only reader
  was `builder->settings.readOnlyMode`, which has been rebound. The
  heap cell is "dead storage" alive only because of the
  `nix::ref<bool>` membership.
- `builder->settings.readOnlyMode` aliases the global
  `nix::settings.readOnlyMode`. That global is a non-static member of
  `extern nix::Settings settings` (from `globals.hh`), and so has
  program lifetime.

### Step 3: `_build` (load path)

Same code as before. The `EvalSettings` is moved (bit-copying the
`bool *` member); the bit-copied pointer now points at
`&nix::settings.readOnlyMode` (program-lifetime), not the heap cell.

### Step 4: `_free` (load path)

`delete builder` destroys `builder->readOnlyMode`. The heap `bool`
cell is deallocated. **Nothing dangles**: the C-API
`EvalState`'s `EvalSettings::readOnlyMode` already aliases
`&nix::settings.readOnlyMode`, which is independent of the now-gone
heap cell.

### Step 5, 6: post-`_free` reads (load path)

`isReadOnly()` reads `nix::settings.readOnlyMode` via the
program-lifetime global pointer. Safe.

**Conclusion for the load path:** the rebind in `_load` makes this
path safe. F1's framing of `_load` as "masking the bug" is correct.

---

## Verdict

**Code-trace confirms F1.** The unsoundness is unambiguously
established by the source; every step has identifier-level evidence:

- Step 1: `nix::make_ref<bool>(true)` and `nix_eval_state_builder`'s
  initialiser list (`nix_api_expr.cc :: nix_eval_state_builder_new`).
- Step 2: `EvalSettings::EvalSettings(bool &)` constructor body
  (`eval-settings.cc`) stores `&readOnlyMode` in `bool *
  EvalSettings::readOnlyMode` (`eval-settings.hh`).
- Step 3: `nix_eval_state_build` moves `builder->settings` and
  `builder->fetchSettings` but leaves `builder->readOnlyMode`
  untouched. The C-API `EvalState` wrapper has no `shared_ptr<bool>` /
  `ref<bool>` member (`nix_api_expr_internal.h`). `Config` has no
  user-defined move ctor (`configuration.hh`); the implicit move
  bit-copies `bool *`.
- Step 4: `nix_eval_state_builder_free` is `delete builder`
  (`nix_api_expr.cc`). The implicit destructor of
  `nix_eval_state_builder` destroys `nix::ref<bool> readOnlyMode`
  (which is the only owner; refcount goes to zero; heap cell is
  freed).
- Step 5: the surviving heap `EvalSettings` (in
  `EvalState::ownedSettings`) holds the same `bool *` (bit-copied in
  Step 3); it now points at freed memory.
- Step 6: `EvalSettings::isReadOnly()` (`eval-settings.hh`) dereferences
  it; `EvalState::ensureLazyPathCopied` (`paths.cc`) and the
  `addToStore`-like path inside `eval.cc` are the documented call
  sites.

Every link in the chain is established by code, not inference. There is
no "co-ownership transfer" hidden in `_build` — I read both
`nix_eval_state_build` and the `EvalState` wrapper struct end-to-end;
no member of the wrapper holds a `shared_ptr<bool>` or `nix::ref<bool>`.
There is no unconditional rebind in `_build` — the only place
`builder->settings.readOnlyMode` is rewritten anywhere in the file is
inside `nix_eval_state_builder_load`.

F1's verdict is correct: the no-load path is a latent UAF, and the
load path masks it via the program-lifetime global.

### Subtle caveats (not refutations)

- The bug is **latent**: it triggers only when an `EvalState` operation
  exercises `isReadOnly()`. The four no-load tests F1 lists (and the
  one I read at line 30 of `libexpr-tests/nix_api_expr.cc`,
  `nix_eval_state_lookup_path`) may evaluate expressions that never
  reach `ensureLazyPathCopied`/`addToStore` paths. The
  `nix_eval_state_lookup_path` test does evaluate
  `builtins.seq <nixos-config> <nixpkgs>` post-`_free`, which forces
  lookup-path resolution and may or may not reach a store-path
  coercion that calls `isReadOnly()`. Determining whether the latent
  read actually fires is a sanitizer-class question, **not** a source-
  reading question. F1 acknowledges this.
- The `assert(readOnlyMode)` check passes a dangling non-null pointer.
  In a release build (where the assert is compiled out), the
  dereference is unconditional. Either way, the dereference of a
  dangling pointer is the bug.
- The bool memory may, in many allocators, be "still mapped" after the
  freelist returns it, so the read may return a stable
  same-allocation value (most likely `true` if the slot wasn't
  reused). This is allocator-dependent behaviour, not a source-level
  guarantee. The bug is real even if the symptom is hard to observe.

### What would refute F1

Any of the following, none of which I found:

1. A `std::shared_ptr<bool>` or `nix::ref<bool>` member in the C-API
   `EvalState` struct that captures the builder's
   `readOnlyMode`. **Not present** (verified in
   `nix_api_expr_internal.h`).
2. A `std::move(builder->readOnlyMode)` or
   `std::shared_ptr<bool>` capture in `nix_eval_state_build`. **Not
   present** (verified in the function body).
3. An unconditional rebind of `builder->settings.readOnlyMode` to a
   program-lifetime address inside `_new` or `_build`. **Not
   present**: the only rebind is inside `_load`.
4. A user-defined move constructor on `EvalSettings` or `Config` that
   re-binds the pointer to point inside the new (heap) `EvalSettings`.
   **Not present**: `Config` declares no move ctor; `EvalSettings`
   declares only the `(bool &, LookupPathHooks)` constructor and uses
   the implicit move.

---

## Implications for catalog integration

F1's catalog framing (a new candidate, e.g. N30 in
`23-c-api-debt.md`) is **substantively correct** and the verification
strengthens it: every step of the lifetime claim has identifier-level
evidence from end-to-end source reads.

For the candidate text itself, two minor wording precisions worth
folding in:

1. F1 says "any subsequent call to `EvalSettings::isReadOnly()` reads
   from freed memory". More precisely: any call to `isReadOnly()` made
   *after* `nix_eval_state_builder_free` runs and *before* the
   `EvalState` is destroyed. If the embedder calls the
   `EvalState`-mutating APIs **before** freeing the builder, those
   reads are safe. None of the four in-tree no-load test sites
   demonstrate that pattern (they all free the builder first); but
   the catalog should note the temporal precondition for correctness
   to make the bug class crisp.
2. The mechanical fix proposed in F1 (option 1: add
   `std::shared_ptr<bool>` to the C-API `EvalState` struct, move from
   `builder->readOnlyMode` in `_build`) is correct. It is also the
   minimum change that closes the bug; option 2 (#138 strategic
   rewire) is a larger refactor that subsumes it. Both options are
   compatible.

No new catalog entry is needed beyond F1's proposed N30; F1 already
specified placement in `candidates/23-c-api-debt.md` and the
relationship to #138.

---

## Open questions

1. **Does any existing in-tree no-load test actually trigger
   `isReadOnly()`?** The verification doc cannot answer this from
   source alone — it requires either tracing every code path the
   test takes, or running under a sanitizer. F1 already flagged this
   as open question 3. The verification confirms the *class* of bug
   (UAF on path that runs `isReadOnly()` post-`_free` on the no-load
   path) but not the *coverage* (which tests exercise that path).
2. **`Setting<T>` parent-pointer rebind during the implicit move of
   `EvalSettings`.** F1 mentions this as a caveat (open question 2).
   The verification confirms the implicit move bit-copies the
   `bool *` correctly, which is the only point relevant to the F1
   claim. The `Setting<T>` parent-pointer rebind question is
   independent: each `Setting<T>` field stores a `Config *` pointing
   to its container; after a move, the new container's `Setting<T>`
   fields still point to the **old** container, not the new one.
   This is a separate bug class that F1 correctly distinguishes from
   the `bool *` issue. Not in scope here.
3. **Is `builder->lookupPath` left in a usable state for any code
   that runs after `_build`?** `nix_eval_state_build` passes
   `builder->lookupPath` by reference (no `std::move`); only the
   `nix::EvalState` constructor uses it during construction. After
   `_build`, the lookup path is captured into the `nix::EvalState`'s
   `LookupPath lookupPath` member, so `builder->lookupPath` is
   irrelevant post-`_build`. Confirmed safe; not a related bug.
4. **The `_new`-default of `true` vs. global default of `false`.**
   F1's "Issue 3" — disagreement between `make_ref<bool>(true)` in
   `_new` and `Settings::readOnlyMode = false` in
   `globals.hh`. Verification confirms both defaults exist as
   stated. Whether the discrepancy is documented in `nix_api_expr.h`
   is a documentation question, not a code-correctness one.
