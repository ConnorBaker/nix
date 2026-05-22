# E2 — C-API *Settings audit

**Charter:** Audit the C-API `*Settings` surface in lockstep before any of #134
(virtual-base diamond → composition), #138 (`readOnlyMode` to `Store`), #144
(per-subsystem ownership) lands, to confirm none of the proposed refactors
breaks the C ABI in invisible ways.

**Scope:** All six C-API libraries — `src/libutil-c`, `src/libstore-c`,
`src/libexpr-c`, `src/libfetchers-c`, `src/libflake-c`, `src/libmain-c`.
Read every `.cc`/`.h`/`internal.h(h)` in those directories end-to-end.

## Per-config field reads from `src/*-c/`

### `nix::settings.*` (the global `Settings` instance)

| File | Identifier | Field read | Defined on (current diamond) |
| --- | --- | --- | --- |
| `src/libstore-c/nix_api_store.cc` | `nix_add_derivation` | `nix::settings.readOnlyMode` (read as condition) | `Settings` directly (not a diamond base) |
| `src/libexpr-c/nix_api_expr.cc` | `nix_eval_state_builder_load` | `&nix::settings.readOnlyMode` (address-taken alias into `EvalSettings::readOnlyMode`) | `Settings` directly |

Those are the only two `nix::settings.*` reads in any of the six C-API
libraries. Confirmed via `grep -rn "nix::settings"` over all six directories;
no other matches.

`grep` also confirms there are **no `nix::settings ` (non-field) usages** —
i.e., no `nix::settings::staticMember` calls, no `&nix::settings`
address-taken-as-whole, and no `extern Settings settings;` re-declared inside
the C-API translation units.

### `evalSettings`, `flakeSettings`, `fetchSettings`, `compatibilitySettings`, `experimentalFeatureSettings`, `loggerSettings`, `pluginSettings`

`grep -rEn "(evalSettings|flakeSettings|fetchSettings|compatibilitySettings|experimentalFeatureSettings|loggerSettings|pluginSettings)\."`
in all six C-API directories returns **zero hits**.

The C-API does **not** read any global `*Settings` singleton other than
`nix::settings.readOnlyMode`. Every other settings interaction goes through a
caller-owned object that the embedder constructs explicitly:

- `nix_eval_state_builder` holds an owned `nix::EvalSettings settings;` and
  `nix::fetchers::Settings fetchSettings;` by value
  (`src/libexpr-c/nix_api_expr_internal.h`).
- `nix_fetchers_settings` holds a `nix::ref<nix::fetchers::Settings>`
  (`src/libfetchers-c/nix_api_fetchers_internal.hh`).
- `nix_flake_settings` holds a `nix::ref<nix::flake::Settings>`
  (`src/libflake-c/nix_api_flake_internal.hh`).

There is no `extern nix::flake::Settings flakeSettings;` reader, no
`extern nix::fetchers::Settings fetchSettings;` reader, etc. — those externs
are entirely a `libcmd` concern (per `15-globals-settings.md` candidate #133)
and the C-API never touches them.

### Settings-related symbols passed through (not field reads)

For completeness, the following are settings-typed values in the C-API but
are passed through opaque-pointer wrappers — they are **not** field reads of
a global, so the proposed refactors do not affect them:

| File | Identifier | Operation |
| --- | --- | --- |
| `src/libexpr-c/nix_api_expr.cc` | `nix_eval_state_builder_new` | constructs `nix::EvalSettings{*readOnly}` with a per-builder `bool`, **not** the global |
| `src/libexpr-c/nix_api_expr.cc` | `nix_eval_state_builder_load` | calls `loadConfFile(builder->settings)` — `loadConfFile` takes `AbstractConfig &`, base-class API |
| `src/libexpr-c/nix_api_expr.cc` | `nix_eval_state_builder_load` | calls `loadConfFile(builder->fetchSettings)` |
| `src/libflake-c/nix_api_flake.cc` | `nix_flake_settings_add_to_eval_state_builder` | calls `settings->settings->configureEvalSettings(builder->settings)` — member function |
| `src/libflake-c/nix_api_flake.cc` | `nix_flake_reference_and_fragment_from_string` | `*fetchSettings->settings` deref, passed to `parseFlakeRefWithFragment` |
| `src/libflake-c/nix_api_flake.cc` | `nix_flake_lock` | `*flakeSettings->settings` deref, passed to `lockFlake` |
| `src/libfetchers-c/nix_api_fetchers.cc` | `nix_fetchers_settings_new` | constructs a fresh `fetchers::Settings` |
| `src/libutil-c/nix_api_util.cc` | `nix_setting_get` | iterates `nix::globalConfig.getSettings(...)` — name-keyed lookup, not field syntax |
| `src/libutil-c/nix_api_util.cc` | `nix_setting_set` | `nix::globalConfig.set(key, value)` — name-keyed dispatch |

The only place where a *field path* on a settings struct is referenced is the
two `readOnlyMode` sites listed earlier. Everything else is a base-class API
call (`loadConfFile`), a member function (`configureEvalSettings`), or a
key-string dispatch (`globalConfig.set`/`getSettings`). Those are immune to
the diamond → composition refactor.

## Per-proposal C-API impact

### #134 — Settings virtual-base diamond → composition

**Touched fields in C-API:** `readOnlyMode`.

**Where `readOnlyMode` lives in the diamond:** `Settings::readOnlyMode` is a
plain `bool` member declared **directly on `Settings`** (in `globals.hh`,
right after `keepFailed`), **not** on `LocalSettings` / `LogFileSettings` /
`WorkerSettings` / `NarInfoDiskCacheSettings`. Composition that moves the
four sub-config groups into held-by-value members therefore leaves
`Settings::readOnlyMode` exactly where it is — the field path
`settings.readOnlyMode` survives unchanged.

**C-API impact: source-compatible.**

**Caveats:**
- Other field reads of `LocalSettings`/`WorkerSettings` fields via
  `settings.someLocalField` would break — but **the C-API has zero such
  reads.** The candidate's pitfall ("external code using
  `settings.someLocalField` directly") does not apply within these six
  libraries; any concern there belongs to consumers (e.g. Hydra), not to
  the C-API surface itself.
- `loadConfFile(builder->settings)` is unaffected: `loadConfFile` takes
  `AbstractConfig &`, and any composition refactor preserves
  `EvalSettings : Config`.

### #138 — `EvalSettings::readOnlyMode` `bool *` aliasing → `Store`/`StoreConfig`

**Touched fields in C-API:** `readOnlyMode` (both reads).

#### Reader 1: `nix_add_derivation` (`src/libstore-c/nix_api_store.cc`)

```
auto ret = nix::settings.readOnlyMode ? nix::computeStorePath(*store->ptr, derivation->drv)
                                      : store->ptr->writeDerivation(derivation->drv, nix::NoRepair);
```

The function already has `Store * store` as a parameter. Migration is
**mechanical**: replace `nix::settings.readOnlyMode` with the new accessor on
`store->ptr` (`store->ptr->config.readOnly()` or whatever spelling #138
chooses). No new C-API helper needed for this reader.

The existing comment (`Quite dubious that users would want this to silently
succeed without actually writing the derivation if this setting is set, but
it was that way already...`) flags pre-existing semantic doubt about this
read, but #138 should preserve the current behaviour.

#### Reader 2: `nix_eval_state_builder_load` (`src/libexpr-c/nix_api_expr.cc`)

```
builder->settings.readOnlyMode = &nix::settings.readOnlyMode;
loadConfFile(builder->settings);
```

This is the load-bearing pointer alias the candidate calls out. The builder
was constructed earlier in `nix_eval_state_builder_new` with a per-builder
`nix::ref<bool> readOnly` and `EvalSettings{*readOnly}` — i.e., the eval
state initially owns its own `readOnlyMode` storage; then `..._load`
overwrites the pointer to alias the global.

**Migration paths:**

1. **If `EvalSettings::readOnlyMode` field is kept as a deprecated shim
   (always-true alias) for one release:** the existing
   `builder->settings.readOnlyMode = &nix::settings.readOnlyMode;` line stays
   compiling but becomes dead (the shim ignores it). Source-compatible. This
   is the path #138's validation paragraph already proposes.
2. **If the `bool *` field is removed in the same release:** the line stops
   compiling and must be deleted. Source-only break (no ABI symbol exposed),
   but a definite source change in the C-API TU.

The `builder->settings` is *not* `nix::settings` — it is the per-builder
`EvalSettings` instance that the embedder owns. So the relevant question for
#138 is: how does `EvalSettings` continue to know whether the active store
is read-only when `EvalSettings` no longer holds a `bool *`?

**Required upstream fix:** `EvalSettings::isReadOnly()` (or its successor)
needs to query the active `Store` via the `EvalState` link, **not** via a
field on `EvalSettings` itself. Once `isReadOnly()` is wired through
`EvalState::store`, the C-API line above becomes redundant and is deleted in
the same patch.

**C-API impact: source-only break (one deleted line in one TU).** No ABI
symbol changes, no public-header changes, no embedder visible behaviour
shift (the embedder already passes `Store *` into `nix_eval_state_builder_new`,
which gives `EvalState` the store reference it needs to query `readOnly()`).

### #144 — 19+ `Config`-derived globals → per-subsystem ownership

**Touched fields in C-API:** none via field syntax.

**Touched globals in C-API:**

- `extern nix::Settings settings;` — read via `nix::settings.readOnlyMode`
  (covered above by #138; once #138 lands, the C-API has zero reads of the
  `nix::settings` global).
- `nix::globalConfig` — read via `nix::globalConfig.getSettings(...)` and
  `nix::globalConfig.set(...)` in `nix_api_util.cc`. **This is the C ABI
  surface for settings (see next section).** Any #144 redesign must keep a
  process-global registry that `globalConfig.getSettings()` can iterate,
  otherwise `nix_setting_get` and `nix_setting_set` cease to function.

**C-API impact: source-compatible**, *provided* that:

1. `globalConfig` (or a successor with the same `getSettings`/`set` API)
   continues to exist and aggregates every subsystem's settings.
2. The per-subsystem owners *register* with `globalConfig` (or the
   successor) so that `nix_setting_get("eval-cores", ...)` still resolves
   regardless of which subsystem owns the underlying setting.

If #144 splinters `globalConfig` into per-subsystem registries with no
aggregator, then `nix_setting_get`/`nix_setting_set` either become
incomplete (silently returning `NIX_ERR_KEY` for valid settings) or break
the C ABI semantics. **This is the load-bearing constraint for #144.**

The good news: per the candidate text, "the long-term direction is a
per-subsystem config object owned by the corresponding subsystem object",
not the elimination of the registry. As long as registration is preserved,
the C-API is fine.

The `extern Settings settings;` symbol itself is **not part of the C ABI** —
it has C++ name mangling (it's in `namespace nix` and the type is a C++
class with virtual bases), so no C consumer can link against it. Removing
or relocating this symbol would only break C++ consumers (Hydra, Nixpkgs
C++ tooling), which is candidate-15 in-scope.

## C-API extern symbol exposure

**The settings surface as part of the documented C ABI is exclusively
key/value, name-keyed, string-typed.** Specifically:

```c
nix_err nix_setting_get(nix_c_context * context, const char * key,
                        nix_get_string_callback callback, void * user_data);
nix_err nix_setting_set(nix_c_context * context, const char * key,
                        const char * value);
```

Both functions live in `src/libutil-c/nix_api_util.h` under the
`@defgroup settings` group. They are implemented against
`nix::globalConfig.getSettings(...)` and `nix::globalConfig.set(key, value)`
respectively (`src/libutil-c/nix_api_util.cc`).

**Consequence:** no C ABI symbol exposes the *type* of any settings struct.
Field paths are entirely an implementation detail. The diamond → composition
refactor (#134), the `readOnlyMode` relocation (#138), and the per-subsystem
ownership refactor (#144) can all proceed without an ABI version bump *as
long as*:

1. `nix_setting_get` / `nix_setting_set` keep working (i.e. the global
   aggregator survives).
2. The two source-level `readOnlyMode` reads in `nix_api_store.cc` and
   `nix_api_expr.cc` are updated in lockstep with #138.

There are also opaque-pointer settings types in the public headers:

| Public type | Header | What it wraps |
| --- | --- | --- |
| `nix_fetchers_settings` | `src/libfetchers-c/nix_api_fetchers.h` | `nix::ref<nix::fetchers::Settings>` |
| `nix_flake_settings` | `src/libflake-c/nix_api_flake.h` | `nix::ref<nix::flake::Settings>` |
| `nix_eval_state_builder` | `src/libexpr-c/nix_api_expr.h` | holds `nix::EvalSettings` and `nix::fetchers::Settings` by value |

All three are forward-declared as opaque structs in the public headers; the
internal layout lives in `*_internal.h(h)` files that C consumers do not
include. Therefore changing the layout of `EvalSettings`, `fetchers::Settings`,
or `flake::Settings` does **not** break the C ABI — only the C++ ABI of the
internal structs, which is rebuilt with the rest of the tree.

There is no `nix_store_settings` opaque type, no `nix_setting_get_typed`
function, no inline functions in C-API public headers that read settings
fields. The C-API headers also do not document any specific setting key as
load-bearing — `key` is a plain string passed to `globalConfig`.

## Recommended migration order

The candidate text already proposes **#136 → #137 → #134/#135/#144/#149**
plus **#138 → #131** for the broader refactor chain. Restricted to the
C-API hazards in this report, the keystone ordering is:

1. **#138 first** — eliminate the two `readOnlyMode` reads in the C-API
   (one source-only break in `nix_api_expr.cc`, one mechanical replacement
   in `nix_api_store.cc`). After this, the C-API has **zero** field-syntax
   reads of any `*Settings` global. This is the highest-leverage step
   because it removes the only load-bearing field path in the C ABI.
2. **#144 next** — once #138 has cleared the C-API of field reads, #144
   can splinter the globals freely *provided* the `globalConfig`
   aggregator (or a successor that `nix_setting_get`/`nix_setting_set`
   route through) is preserved. The constraint is purely on
   `globalConfig.getSettings()` / `globalConfig.set()`, not on any field
   layout.
3. **#134 last (or any time after #138)** — purely structural; the C-API
   has no field reads on `LocalSettings`/`WorkerSettings`/`LogFileSettings`/
   `NarInfoDiskCacheSettings`, so composition vs. inheritance is invisible
   to the six libraries. Could land before #138 if convenient — there is
   no C-API ordering constraint between #134 and #138.

The candidate text proposes #134 *after* #144 ("after #144 collapses
LogFileSettings/NarInfoDiskCacheSettings/etc. into Settings directly, the
diamond shrinks to just LocalSettings"). The C-API does not constrain that
ordering either way — both orderings are source-compatible from the C ABI
side.

## Required C-API helpers to add before each refactor

### Before #138

**No new public C function is strictly required.** `nix_add_derivation`
already has `Store * store` in scope, so the migration is internal. No
`nix_store_is_read_only(Store *)` C export is needed for the existing
two readers.

However, **if** future C-API code outside of `nix_api_store.cc` and
`nix_api_expr.cc` needs to query read-only state at a site that does **not**
hold a `Store *`, then a `nix_store_is_read_only(nix_c_context *, Store *)`
C export would be the clean way to expose `StoreConfig::readOnly()`. None of
the current C-API call sites need this; flagging it as forward-looking only.

The line to delete in `nix_api_expr.cc` — `builder->settings.readOnlyMode =
&nix::settings.readOnlyMode;` — depends on `EvalSettings::isReadOnly()`
being rewired to query the store via `EvalState`. That rewiring is upstream
of the C-API, not a C-API helper.

### Before #134

No new public C function required. The diamond → composition refactor is
invisible to the C-API as long as `Settings::readOnlyMode` keeps its
top-level field path (it does — see #134 analysis above) and `loadConfFile`
keeps its `AbstractConfig &` signature (it does).

### Before #144

**Hard requirement:** preserve `globalConfig.getSettings(map)` and
`globalConfig.set(key, value)` semantics, regardless of how subsystem
ownership is reorganised. The simplest way is to keep `globalConfig` as a
process-global aggregator that subsystems register their `Config` instance
with at init time (the existing pattern), even if each subsystem also holds
a strong reference to its own `Config`.

If #144's design eliminates the aggregator, then **a new C export is
required before #144 lands**, e.g.:

```c
nix_err nix_setting_get_in(nix_c_context *, const char * subsystem,
                           const char * key, nix_get_string_callback,
                           void * user_data);
```

…plus a deprecation cycle for `nix_setting_get`/`nix_setting_set`. This
would be an ABI addition (not a break), but the *behavioural* semantics of
the existing two functions would silently change (some keys disappear),
which is itself a soft ABI break. **Strongly preferred:** keep the
aggregator.

## Open questions

1. **Does #138 plan to delete `EvalSettings::readOnlyMode` outright, or
   keep it as a deprecated shim for one release?** The C-API change differs
   between the two paths (delete one line vs. leave it as a no-op). The
   candidate text is ambiguous — it says "keep the field in `EvalSettings`
   for one release as a deprecated shim (always-true alias)". An
   "always-true alias" is a weird semantic because the C-API code currently
   sets the alias to point to the *global* — under a deprecated shim, the
   field would presumably be ignored entirely. The candidate prose
   conflates two ideas; #138's implementation must pick one.

2. **Does #144 plan to keep `globalConfig` as the aggregator, or
   eliminate it?** The C ABI surface (`nix_setting_get`, `nix_setting_set`)
   depends critically on the aggregator. The candidate says "long-term
   direction is a per-subsystem config object owned by the corresponding
   subsystem object — not a global registry"; the C-API forces a
   compromise: per-subsystem ownership *with* a registration step into
   `globalConfig` (or successor), so the registry remains queryable by name
   even though ownership lives elsewhere.

3. **`nix_eval_state_builder_load` in `nix_api_expr.cc` reassigns
   `builder->settings.readOnlyMode` from the per-builder `bool` (set by
   `nix_eval_state_builder_new`) to `&nix::settings.readOnlyMode`.** That
   is an existing oddity — the per-builder `bool` (held in the
   `nix::ref<bool> readOnlyMode` field of the builder struct) becomes dead
   storage after `..._load` runs. Was that intentional (load-from-config
   should override the local default), or accidental? #138 has an
   opportunity to clean this up: either keep the per-builder `bool` as the
   source of truth (and let the embedder set it explicitly), or route
   through the `Store`'s `readOnly()` and drop the per-builder `bool`
   altogether. The current code does neither cleanly.

4. **`nix_add_derivation`'s comment ("Quite dubious that users would want
   this to silently succeed without actually writing the derivation if
   this setting is set, but it was that way already...")** flags
   pre-existing semantic doubt. If #138 changes the source of `readOnly`
   from "global setting" to "per-store config", the *value* read here may
   change for embedders who previously set `nix::settings.readOnlyMode =
   true;` but did not configure their store as read-only. That is a soft
   behavioural change visible to C-API consumers. **Should be called out
   in #138's release notes.**
