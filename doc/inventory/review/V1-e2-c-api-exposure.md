# V1 — verify E2 C-API settings-exposure claim

## Charter recap

E2 (`doc/inventory/review/E2-c-api-settings.md`) concludes:

> "no C ABI symbol exposes the *type* of any settings struct. Field paths are
> entirely an implementation detail."

and:

> "All three [opaque types] are forward-declared as opaque structs in the public
> headers; the internal layout lives in `*_internal.h(h)` files that C consumers
> do not include."

V1's job is to verify whether that claim holds under closer reading. The hole
in E2's method is that it grepped for `nix::settings.<field>` reads (correct
for "what's used") but did not check whether `*_internal.h(h)` files are
installed alongside the public headers, which would expose layout transitively.

## Method

Files read end-to-end:

- All `meson.build` files in the six C-API library directories
  (`src/libutil-c`, `src/libstore-c`, `src/libexpr-c`, `src/libfetchers-c`,
  `src/libflake-c`, `src/libmain-c`).
- All `nix_api_*.h` and `nix_api_*_internal.h(h)` files in the same six
  directories.
- Implementation files (`*.cc`) for each library, focusing on functions whose
  signatures touch settings types or whose bodies dereference settings-typed
  members.

The crucial discovery, made before walking the headers, is what each
`meson.build` actually installs. That determines whether `*_internal.h(h)`
files are part of the public ABI surface (they are, in five of the six
libraries — see next section).

## Public-header installation set per library

Per `install_headers(headers, preserve_path : true)` in each `meson.build`:

| Library      | Public `.h` headers installed                                                          | Internal headers installed                                       |
| ------------ | -------------------------------------------------------------------------------------- | ---------------------------------------------------------------- |
| `libutil-c`  | `nix_api_util.h`                                                                       | `nix_api_util_internal.h` (`# TODO don't install this once tests don't use it.`)        |
| `libstore-c` | `nix_api_store.h`, `nix_api_store/store_path.h`, `nix_api_store/derivation.h`          | `nix_api_store_internal.h` (`# TODO don't install ... or move the header into libstore, non-c`) |
| `libexpr-c`  | `nix_api_expr.h`, `nix_api_external.h`, `nix_api_value.h`                              | `nix_api_expr_internal.h` (no TODO comment — installed unconditionally)  |
| `libfetchers-c` | `nix_api_fetchers.h` (added twice; second install with `# TODO move this header to libexpr...` comment but is not a removal note) | `nix_api_fetchers_internal.hh` |
| `libflake-c` | `nix_api_flake.h` (added twice — same note)                                             | `nix_api_flake_internal.hh`    |
| `libmain-c`  | `nix_api_main.h`                                                                       | (none)                          |

**Five of six libraries install at least one `*_internal.h(h)` header into
the public include path.** This was missed by E2's "opaque struct"
characterisation. The next section walks each public-symbol surface and
classifies what is actually exposed to a downstream C consumer who has the
installed headers and nothing else.


## Per-public-header settings-type inventory

Functions are listed only if their signature, behaviour, or struct definition
touches a settings type (directly or through one of the wrapper types above).
Functions with no settings interaction (e.g. `nix_value_force`,
`nix_store_realise`, `nix_alloc_value`, etc.) are intentionally omitted —
they are clean and irrelevant to this audit.

### `libutil-c/nix_api_util.h` (key/value name-keyed only)

| Function | Settings type touched | Exposure class |
| --- | --- | --- |
| `nix_setting_get(ctx, key, callback, user_data)` | none in signature; body uses `nix::globalConfig.getSettings(map)` | name-keyed (no type exposed) |
| `nix_setting_set(ctx, key, value)` | none in signature; body uses `nix::globalConfig.set(key, value)` | name-keyed (no type exposed) |

These are the only two settings-related functions in this header. Both are
fully name-keyed; no settings *type* appears in the signature. E2's
characterisation of these two functions is correct.

### `libutil-c/nix_api_util_internal.h` (installed; tests use it)

Defines `struct nix_c_context { nix_err last_err_code; std::optional<std::string> last_err; std::optional<nix::ErrorInfo> info; std::string name; };`.
Layout exposed, but **no settings type touched.** Outside this audit's
scope.

### `libstore-c/nix_api_store.h` (no settings types)

All public functions take `Store *`, `StorePath *`, `nix_derivation *`. No
settings type appears in any signature. E2's characterisation here is
correct.

### `libstore-c/nix_api_store_internal.h` (installed)

Defines `struct Store { nix::ref<nix::Store> ptr; }`,
`struct StorePath { nix::StorePath path; }`,
`struct nix_derivation { nix::Derivation drv; }`.

**No settings type appears in this internal header**, but the layouts of
`Store`, `StorePath`, and `nix_derivation` are exposed. The implementation
of `nix_add_derivation` reads `nix::settings.readOnlyMode` (a *field on
the global*, not a field of any struct exposed here). The `Store` C wrapper
does not embed a settings struct.

### `libexpr-c/nix_api_expr.h` (opaque types in signatures)

| Function | Signature settings-touch | Exposure class |
| --- | --- | --- |
| `nix_eval_state_builder_new(ctx, store)` | returns `nix_eval_state_builder *` | opaque-pointer |
| `nix_eval_state_builder_load(ctx, builder)` | takes `nix_eval_state_builder *` | opaque-pointer |
| `nix_eval_state_builder_set_lookup_path(ctx, builder, lookupPath)` | takes `nix_eval_state_builder *` | opaque-pointer |
| `nix_eval_state_build(ctx, builder)` | takes `nix_eval_state_builder *`, returns `EvalState *` | opaque-pointer |
| `nix_eval_state_builder_free(builder)` | takes `nix_eval_state_builder *` | opaque-pointer |
| `nix_state_create(ctx, lookupPath, store)` | returns `EvalState *` | opaque-pointer |
| `nix_state_free(state)` | takes `EvalState *` | opaque-pointer |

The public header forward-declares both `nix_eval_state_builder` and
`EvalState` as opaque (`typedef struct nix_eval_state_builder nix_eval_state_builder;`,
`typedef struct EvalState EvalState;`). **Read in isolation, the public
header gives no layout information.**

### `libexpr-c/nix_api_expr_internal.h` (installed; this is the load-bearing finding)

Definitions, verbatim:

```cpp
struct nix_eval_state_builder
{
    nix::ref<nix::Store> store;
    nix::EvalSettings settings;
    nix::fetchers::Settings fetchSettings;
    nix::LookupPath lookupPath;
    nix::ref<bool> readOnlyMode;
};

struct EvalState
{
    nix::EvalState & state;
    std::unique_ptr<nix::fetchers::Settings> ownedFetchSettings;
    std::unique_ptr<nix::EvalSettings> ownedSettings;
    std::shared_ptr<nix::EvalState> ownedState;
};
```

Plus `#include "nix/fetchers/fetch-settings.hh"` and
`#include "nix/expr/eval-settings.hh"` at the top, which further pull in the
full layouts of `nix::fetchers::Settings` and `nix::EvalSettings`.

This is a complete-type definition of `nix_eval_state_builder` and
`EvalState` in the installed include path. Any downstream consumer can:

- compute `sizeof(struct nix_eval_state_builder)` and
  `sizeof(struct EvalState)`;
- read or write `builder->settings`, `builder->fetchSettings`,
  `builder->lookupPath`, `builder->readOnlyMode`, etc., directly;
- `memcpy` the structs;
- inherit from them in C++.

Therefore the *layout* of `nix::EvalSettings`, `nix::fetchers::Settings`,
and `nix::LookupPath` is **transitively part of the public include set**.
Any out-of-tree consumer that links against `nixexprc` and includes the
installed `nix_api_expr_internal.h` (e.g. Nixpkgs C++ tooling and the
Nix-internal `libexpr-tests` setup, which the build system makes available
to external consumers via the same install rule) is sensitive to layout
changes in those settings types.

### `libfetchers-c/nix_api_fetchers.h` and `_internal.hh`

Public header forward-declares `typedef struct nix_fetchers_settings nix_fetchers_settings;`.
Internal header (installed) defines:

```cpp
struct nix_fetchers_settings { nix::ref<nix::fetchers::Settings> settings; };
```

and includes `nix/fetchers/fetch-settings.hh`.

| Function (header) | Signature settings-touch | Exposure class |
| --- | --- | --- |
| `nix_fetchers_settings_new(ctx)` | returns `nix_fetchers_settings *` | opaque-pointer in public header; complete type in internal header (installed) |
| `nix_fetchers_settings_free(settings)` | takes `nix_fetchers_settings *` | opaque-pointer in public header; complete type in internal header (installed) |

So `nix_fetchers_settings` is opaque if you only include `nix_api_fetchers.h`,
but the internal header — which IS installed — exposes `nix::ref<nix::fetchers::Settings>`
as the wrapper's sole field, and pulls in the full `nix::fetchers::Settings`
layout. This is the same exposure pattern as `nix_eval_state_builder`.

### `libflake-c/nix_api_flake.h` and `_internal.hh`

Public header forward-declares `typedef struct nix_flake_settings nix_flake_settings;`,
plus several flake-specific opaque types (`nix_flake_reference_parse_flags`,
`nix_flake_reference`, `nix_flake_lock_flags`, `nix_locked_flake`).

Internal header (installed) defines `nix_flake_settings`,
`nix_flake_reference_parse_flags`, `nix_flake_reference`,
`nix_flake_lock_flags`, `nix_locked_flake` with complete types, plus
`#include "nix/flake/settings.hh"`, `#include "nix/flake/flake.hh"`,
`#include "nix/flake/flakeref.hh"`.

| Function (header) | Signature settings-touch | Exposure class |
| --- | --- | --- |
| `nix_flake_settings_new(ctx)` | returns `nix_flake_settings *` | opaque/internal-defined |
| `nix_flake_settings_free(settings)` | takes `nix_flake_settings *` | opaque/internal-defined |
| `nix_flake_settings_add_to_eval_state_builder(ctx, settings, builder)` | takes `nix_flake_settings *` and `nix_eval_state_builder *` | opaque/internal-defined |
| `nix_flake_reference_parse_flags_new(ctx, settings)` | takes `nix_flake_settings *` (not used in body, only kept for future use) | opaque/internal-defined |
| `nix_flake_lock_flags_new(ctx, settings)` | takes `nix_flake_settings *` | opaque/internal-defined |
| `nix_flake_reference_and_fragment_from_string(...)` | takes `nix_fetchers_settings *` and `nix_flake_settings *` | opaque/internal-defined |
| `nix_flake_lock(...)` | takes `nix_fetchers_settings *` and `nix_flake_settings *` | opaque/internal-defined |
| `nix_locked_flake_get_output_attrs(ctx, settings, evalState, lockedFlake)` | takes `nix_flake_settings *` | opaque/internal-defined |

Same pattern: opaque if you only have the public header, fully exposed if
you have the installed internal header.

### `libmain-c/nix_api_main.h`

`nix_init_plugins(ctx)` and `nix_set_log_format(ctx, format)`. **No
settings type touched.** No internal header is installed for `libmain-c`.

## Layout-opacity verification

Test: would a downstream C consumer who installs `pkgconfig --cflags`'s
include path see complete struct definitions for any settings wrapper?

**Result:** YES, in five of six libraries.

- `libutil-c`: installs `nix_api_util_internal.h` → no settings type, but
  exposes `nix_c_context` layout (irrelevant to settings audit).
- `libstore-c`: installs `nix_api_store_internal.h` → no settings type
  exposed, but exposes `Store`, `StorePath`, `nix_derivation` layouts.
- `libexpr-c`: installs `nix_api_expr_internal.h` → exposes
  `nix_eval_state_builder` and `EvalState` complete types. Both contain
  `nix::EvalSettings` and `nix::fetchers::Settings` *by value* (the builder)
  or via `std::unique_ptr` (the eval state). The `#include` directives pull
  in the full settings-struct layouts.
- `libfetchers-c`: installs `nix_api_fetchers_internal.hh` → exposes
  `nix_fetchers_settings` complete type and `nix::fetchers::Settings`
  layout via include.
- `libflake-c`: installs `nix_api_flake_internal.hh` → exposes
  `nix_flake_settings` complete type and `nix::flake::Settings` layout via
  include.
- `libmain-c`: no internal header installed; nothing settings-related.

Therefore E2's claim that "the internal layout lives in `*_internal.h(h)`
files that C consumers do not include" is **factually incorrect as
written**. The internal headers ARE installed; out-of-tree C++ consumers
(in particular Nix's own tests, which depend on the same install rule that
external consumers depend on) DO include them.

The mitigating distinction is that the `*_internal.h(h)` files are only
includable from C++ (they use `nix::ref`, `nix::EvalSettings`,
`std::unique_ptr`, etc.) — they are *not* C-consumable. Pure C consumers
would not be able to compile against them. In that narrow sense, the C
ABI (C-only consumers) does not see the layout. But C++ consumers of the
`nix_api_*` family — which include the in-tree test suite and any external
C++ tool linking against `libnixexprc`, `libnixflakec`, etc. — DO see the
layout. The "C ABI" qualifier in E2's conclusion is doing more work than
the prose admits.

## Lifetime-contract verification (the explicit pointer alias)

Found one explicit pointer-aliasing of a settings-global field by a C-API
implementation, in `nix_eval_state_builder_load`
(`src/libexpr-c/nix_api_expr.cc`):

```cpp
nix_err nix_eval_state_builder_load(nix_c_context * context, nix_eval_state_builder * builder)
{
    if (context)
        context->last_err_code = NIX_OK;
    try {
        // TODO: load in one go?
        builder->settings.readOnlyMode = &nix::settings.readOnlyMode;
        loadConfFile(builder->settings);
        loadConfFile(builder->fetchSettings);
    }
    NIXC_CATCH_ERRS
}
```

The aliased field is `nix::EvalSettings::readOnlyMode`, a `bool *` member
of `nix::EvalSettings` (defined in `src/libexpr/include/nix/expr/eval-settings.hh`):

```cpp
EvalSettings(bool & readOnlyMode, LookupPathHooks lookupPathHooks = {});

/* FIXME: This really shouldn't be public. The C API should have non-global settings instead. */
bool * readOnlyMode = nullptr;

bool isReadOnly() const
{
    assert(readOnlyMode);
    return *readOnlyMode;
}
```

After `nix_eval_state_builder_load` runs, the eval-state-builder's
`settings.readOnlyMode` points into the *global* `nix::settings.readOnlyMode`
field. The lifetime contract is therefore: **the global `nix::settings`
must outlive every `EvalState` constructed from a builder that was
`_load`-ed**. The global has process-static lifetime, so this is
trivially satisfied — but the contract is implicit and undocumented in
the public API.

`nix_eval_state_builder_new` initialises `builder->settings.readOnlyMode`
to point at a per-builder `nix::ref<bool> readOnly` (held in
`builder->readOnlyMode`), so before `_load` runs the lifetime contract is
"the builder owns the bool". The `_load` call silently swaps the pointer
to the global, so the per-builder bool becomes dead storage (E2's open
question 3). This swap is undocumented in the public-header
documentation of `nix_eval_state_builder_load`.

`nix_eval_state_build` in `nix_api_expr.cc`:

```cpp
auto fetchSettings = std::make_unique<nix::fetchers::Settings>(std::move(builder->fetchSettings));
auto settings = std::make_unique<nix::EvalSettings>(std::move(builder->settings));
auto ownedState =
    std::make_shared<nix::EvalState>(builder->lookupPath, builder->store, *fetchSettings, *settings);
auto & stateRef = *ownedState;
void * p = ::operator new(sizeof(EvalState), static_cast<std::align_val_t>(alignof(EvalState)));
return new (p) EvalState{stateRef, std::move(fetchSettings), std::move(settings), std::move(ownedState)};
```

The `nix::EvalSettings` is *moved* from the builder into a fresh
`std::unique_ptr<nix::EvalSettings>` owned by the resulting `EvalState`.
The `bool *` pointer-aliasing is preserved verbatim by the move (the
pointer value is copied), so the constructed `nix::EvalState` continues to
alias `&nix::settings.readOnlyMode` if `_load` ran, or the per-builder
`bool` if it didn't.

## Function-by-function: setter / getter style and ownership

Inspecting setter-shaped functions to confirm whether they copy or alias:

- `nix_eval_state_builder_set_lookup_path(ctx, builder, lookupPath_c)` —
  copies the C string array into a `nix::Strings` and parses into
  `builder->lookupPath`. Copying setter; **no lifetime contract on
  caller**. Settings type touched: `nix::LookupPath` (held by value in
  `nix_eval_state_builder`).

- `nix_flake_reference_parse_flags_set_base_directory(ctx, flags, baseDir, len)`
  — emplaces a `std::string(baseDirectory, len)` into
  `flags->baseDirectory`. Copying setter; no lifetime contract.

- `nix_flake_lock_flags_set_mode_*` — all four mutate the
  `flags->lockFlags->{updateLockFile,writeLockFile,...}` booleans
  in-place. Pure value setters; no lifetime contract.

- `nix_flake_lock_flags_add_input_override(ctx, flags, inputPath, flakeRef)`
  — emplaces parsed `(inputPath, *flakeRef->flakeRef)` into
  `flags->lockFlags->inputOverrides`. The dereference of `flakeRef` is
  copied (value semantics), so no lifetime contract on the caller's
  `flakeRef` post-call. Confirmed by reading the implementation.

- `nix_flake_settings_add_to_eval_state_builder(ctx, settings, builder)` —
  calls `settings->settings->configureEvalSettings(builder->settings)`,
  which is a member-function call that mutates fields of
  `builder->settings`. The function does not store a pointer back to
  `settings`, so the caller can free `nix_flake_settings` immediately
  after this call. **No lifetime contract carried.**

So among the explicit setters, the only one that creates a long-lived
pointer alias to caller-controlled (or global) storage is
`nix_eval_state_builder_load` (implicit, not explicit), and the alias is
into the *global* `nix::settings`, not into caller storage.

## C-API extern-symbol and `globalConfig` aggregator

Confirmed by reading `nix_api_util.cc` end-to-end:

- `nix_setting_get` calls `nix::globalConfig.getSettings(map)`, then
  string-lookup by key.
- `nix_setting_set` calls `nix::globalConfig.set(key, value)`.

These are the only two C ABI symbols that interact with any settings
registry, and neither exposes a settings type to the caller. E2's
characterisation here is correct and unaffected by the internal-header
installation issue.

## Decision

**Confirmed-with-caveats.** The strict E2 claim — "no C ABI symbol
exposes the *type* of any settings struct" — holds for pure-C consumers
of the C-only public headers (`nix_api_*.h`), where the only
settings-touching C ABI is the name-keyed `nix_setting_get`/`nix_setting_set`
pair. The opaque-pointer settings wrappers (`nix_fetchers_settings`,
`nix_flake_settings`, `nix_eval_state_builder`) are forward-declared
opaque in their public C-only headers; pure C consumers cannot inspect
their layout.

However, E2's secondary claim — "the internal layout lives in
`*_internal.h(h)` files that C consumers do not include" — is
**incorrect as written** for the install set. Five of six C-API libraries
install the `*_internal.h(h)` files into the public include path. These
internal headers contain complete C++ struct definitions plus `#include`
directives that pull in the full settings-struct layouts (`nix::EvalSettings`,
`nix::fetchers::Settings`, `nix::flake::Settings`). The opacity is
maintained only with respect to **C-only consumers**, not C++ consumers.

Per-instance evidence:

1. `nix_eval_state_builder` — opaque in `nix_api_expr.h`, complete-type
   defined in `nix_api_expr_internal.h` (installed). Holds
   `nix::EvalSettings settings;` and `nix::fetchers::Settings fetchSettings;`
   by value. Layout of those two settings types is implicitly part of the
   ABI seen by C++ consumers of the install set.

2. `EvalState` — opaque in `nix_api_expr.h`, complete-type defined in
   `nix_api_expr_internal.h` (installed). Holds
   `std::unique_ptr<nix::fetchers::Settings>`,
   `std::unique_ptr<nix::EvalSettings>`, `std::shared_ptr<nix::EvalState>`
   directly. Layouts of all three referenced types implicitly visible to
   C++ consumers.

3. `nix_fetchers_settings` — opaque in `nix_api_fetchers.h`, complete-type
   defined in `nix_api_fetchers_internal.hh` (installed). Holds
   `nix::ref<nix::fetchers::Settings>`. Layout of
   `nix::fetchers::Settings` implicitly part of the C++ ABI.

4. `nix_flake_settings` — opaque in `nix_api_flake.h`, complete-type
   defined in `nix_api_flake_internal.hh` (installed). Holds
   `nix::ref<nix::flake::Settings>`. Layout of `nix::flake::Settings`
   implicitly part of the C++ ABI.

5. `nix_eval_state_builder_load` (lifetime contract) — explicitly
   pointer-aliases `&nix::settings.readOnlyMode` into
   `builder->settings.readOnlyMode`. The aliased field is
   `nix::EvalSettings::readOnlyMode` (a `bool *` member, see
   `src/libexpr/include/nix/expr/eval-settings.hh`). The lifetime
   contract is "global outlives builder", trivially satisfied because
   `nix::settings` has process-static lifetime, but the contract is
   undocumented in the public-header doc-comment of
   `nix_eval_state_builder_load`. The pre-existing FIXME on
   `nix::EvalSettings::readOnlyMode` itself ("This really shouldn't be
   public. The C API should have non-global settings instead.") flags
   that the maintainers are already aware of this oddity.

## Implications for #134, #138, #144

### #134 (Settings virtual-base diamond → composition)

The C ABI surface (C-only consumers) is unaffected: no C function takes
or returns any of the `Local/Worker/LogFile/NarInfoDiskCache` sub-config
types, and the only `Settings` field path in the C-API translation units
is `nix::settings.readOnlyMode`, which lives directly on `Settings`
(unaffected by composition).

The C++-consumer-visible **layout** of `nix::EvalSettings`,
`nix::fetchers::Settings`, and `nix::flake::Settings` is part of the
installed include set via `*_internal.h(h)`. #134 does not change those
three types' diamond-base shape (the diamond at issue is the
`Settings`/`LocalSettings`/`WorkerSettings`/etc. tree, not the eval/flake/
fetchers settings). So #134 does not change layout for any settings type
exposed via `*_internal.h(h)`.

**#134 verdict for V1:** unchanged from E2. Source-compatible across the C
ABI.

### #138 (`readOnlyMode` relocation to `Store`/`StoreConfig`)

E2 already identifies the two `readOnlyMode` reader sites and the migration
strategy. V1 adds the following:

- The pointer-aliasing line
  `builder->settings.readOnlyMode = &nix::settings.readOnlyMode;` is the
  only explicit settings-field pointer alias in the entire C-API
  source. Removing the `bool *` field on `nix::EvalSettings` (which #138
  proposes, replacing it with an `EvalState::store->config.readOnly()`
  accessor) makes this line stop compiling and must be deleted in
  lockstep.

- After deletion, `nix_eval_state_builder` no longer needs the
  `nix::ref<bool> readOnlyMode;` field (it currently exists only to back
  the per-builder `bool` that `..._new` uses to construct
  `nix::EvalSettings{*readOnly}`, before `..._load` overwrites the
  pointer). If `EvalSettings` no longer takes a `bool &` in its
  constructor, this whole field becomes dead storage and should be
  removed from `nix_api_expr_internal.h` — which IS a layout change to
  the publicly-installed internal header. **This is a C++-ABI break** for
  any consumer holding a `nix_eval_state_builder` by value, but no such
  consumer should exist (the public header's docstring says to obtain
  pointers via `nix_eval_state_builder_new`).

- The pre-existing FIXME on `nix::EvalSettings::readOnlyMode` already
  flags the `bool *` aliasing as smelly; #138 is the natural cleanup.

**#138 verdict for V1:** confirmed E2's source-compatible analysis for
C-only consumers. For C++ consumers including `nix_api_expr_internal.h`,
the layout of `nix_eval_state_builder` will change (loss of the
`nix::ref<bool> readOnlyMode;` field and possibly of the
`nix::EvalSettings settings;` field semantics) — call this out in #138's
release notes.

### #144 (per-subsystem ownership of globals)

E2 correctly identifies the load-bearing constraint: `nix::globalConfig`
must remain queryable by name, otherwise `nix_setting_get` /
`nix_setting_set` regress.

V1 adds: because `nix_eval_state_builder` and `EvalState` hold
`nix::EvalSettings` and `nix::fetchers::Settings` *by value* (or as
`std::unique_ptr` / `nix::ref`), the per-builder settings instances are
already *not* the globals. They are owned by the builder/eval-state and
populated via `loadConfFile(builder->settings)` (which reads from the
ambient environment / config files, *not* from the global registry,
unless `loadConfFile` happens to consult `globalConfig` internally). So
#144's per-subsystem ownership reorganisation does not affect the C-API's
relationship with these per-instance settings — it only affects the
`nix_setting_get`/`nix_setting_set` aggregator path, which #144 must
preserve.

**#144 verdict for V1:** unchanged from E2. The aggregator-preservation
constraint is the load-bearing requirement; nothing else in the C-API
shifts.

## Open questions

1. Should the `*_internal.h(h)` headers be uninstalled? The TODO comments
   in `libutil-c` and `libstore-c` meson.build's say yes, "once tests
   don't use [them]". `libexpr-c` and the fetchers/flake variants have no
   such TODO — the install of internal headers is not labelled as
   transitional there, even though the same logic should apply. If the
   long-term direction is "C-only ABI surface", the internal headers
   should be moved into the corresponding non-`-c` libraries (which is
   what the libstore TODO suggests) and then included only by the C-API
   `.cc` and the in-tree tests, not installed.

2. Are external (non-Nix) consumers of the installed `*_internal.h(h)`
   files known to exist? `libstore-test-support`'s
   `nix_api_store.hh` includes `nix_api_store_internal.h`, and
   `libstore-test-support` is itself installed (as a test-support library
   for downstream projects' test suites). So at least via that path,
   external consumers can be assumed to depend on the layout. Removing
   the install for `*_internal.h(h)` will require replacing
   `libstore-test-support`'s reliance on the layout with a public C-only
   API, OR moving `libstore-test-support` to consume the C++ API
   directly.

3. Is the `nix_eval_state_builder_load`-time pointer swap from the
   per-builder bool to `&nix::settings.readOnlyMode` intentional? E2's
   open-question 3 notes the same. V1 confirms: the per-builder `bool`
   becomes dead after `_load` runs, which is dubious. #138 should resolve
   this by deleting the field entirely.

4. Does the `nix_state_create` convenience function (which always calls
   `nix_eval_state_builder_load`) inherit the same global-aliasing
   behaviour? Yes — `nix_state_create` chains `..._new` →
   `..._load` → `..._set_lookup_path` → `..._build`, and `..._load` is the
   one that installs the global pointer. Confirmed by reading
   `nix_state_create` in `nix_api_expr.cc`.
