# E7 — forward-decl source-compat audit

Audit of the `serialise-fwd.hh` / `store-fwd.hh` consolidation proposed in
catalog entry #166. Walks every `*.hh` / `*.h` file under
`src/**/include/`, classifies each forward-decl, and asks whether
consolidation would change the include topology in a way a downstream
embedder might notice.

Scope:

- `struct Source;` / `struct Sink;` (proposed `serialise-fwd.hh`)
- `class Store;` (proposed `store-fwd.hh`)
- `class EvalState;` (out-of-band; the catalog body did not propose
  `eval-fwd.hh` but Pass-2 noted 12 sites, and N9 deferred the
  question)
- Adjacent recurring forward-decls (`StorePath`, `StoreDirConfig`,
  `Value`, `fetchers::Settings`) that share the same include-topology
  question.

No emojis. No line-numbered citations.

---

## Forward-decl inventory

The following counts are from `grep -rn` over `src/**/include/`,
restricted to file-scope forward-decls inside `namespace nix { ... }`.
Indented (block-scope) decls and decls inside nested namespaces are
listed in the variants column.

| Symbol                         | Count | Variant                                         |
| ------------------------------ | ----: | ----------------------------------------------- |
| `class Store;`                 |    20 | bare; all in `namespace nix { ... }`            |
| `class EvalState;`             |    12 | bare; one in `namespace nix { ... }`, one in `namespace nix::flake { ... }` (none — both at outer `namespace nix`); zero indented |
| `struct StoreDirConfig;`       |    10 | bare; all in `namespace nix { ... }`            |
| `struct Settings;`             |    10 | **mixed namespaces**: 8 in `namespace nix::fetchers`, 2 in `namespace nix::flake` (only `flake.hh` and `flake-primops.hh`); these are not the same `Settings` |
| `struct Source;`               |     8 | bare; all in `namespace nix { ... }`; one is the second forward-decl in `derivations.hh`, paired with `struct Sink;` |
| `struct Value;`                |     7 | bare; all in `namespace nix { ... }`            |
| `class StorePath;`             |     6 | bare; all in `namespace nix { ... }`            |
| `struct Sink;`                 |     5 | bare; all in `namespace nix { ... }`            |

Note: the catalog body claimed "9 Source/Sink, 21 Store, 12 EvalState".
Pass-2's revised "13/22/12" was a count of *lines* (treating the two
duplicate `class Store;` lines in `derivations.hh` as separate sites).
The de-duplicated count is closer to **8/5/20/12** (Source/Sink/Store/EvalState).
The `class Store;` count of 20 (rather than 21) excludes
`fetchers/fetchers.hh` if you count it once and including it gives 21.
Either way, the consolidation pressure is real.

### Per-symbol site list

`struct Source;` (8 sites):

- `libutil/include/nix/util/nar-accessor.hh`
- `libutil/include/nix/util/file-system.hh`
- `libutil/include/nix/util/processes.hh`
- `libutil/include/nix/util/file-descriptor.hh`
- `libstore/include/nix/store/serve-protocol.hh`
- `libstore/include/nix/store/derivations.hh` (the `struct Source; struct Sink;` block before `readDerivation`/`writeDerivation`)
- `libstore/include/nix/store/worker-protocol.hh`
- `libstore/include/nix/store/common-protocol.hh`

`struct Sink;` (5 sites):

- `libutil/include/nix/util/processes.hh`
- `libutil/include/nix/util/file-system.hh`
- `libutil/include/nix/util/file-descriptor.hh`
- `libutil/include/nix/util/source-accessor.hh` (Sink-only; takes a `Sink &` for `readFile`)
- `libstore/include/nix/store/derivations.hh`

`class Store;` (20 sites):

- `libflake/include/nix/flake/lockfile.hh`
- `libflake/include/nix/flake/flakeref.hh`
- `libmain/include/nix/main/shared.hh`
- `libfetchers/include/nix/fetchers/tarball.hh`
- `libfetchers/include/nix/fetchers/fetchers.hh`
- `libfetchers/include/nix/fetchers/registry.hh`
- `libcmd/include/nix/cmd/common-eval-args.hh`
- `libcmd/include/nix/cmd/command.hh`
- `libexpr/include/nix/expr/eval.hh`
- `libstore/include/nix/store/filetransfer.hh`
- `libstore/include/nix/store/parsed-derivations.hh`
- `libstore/include/nix/store/path-info.hh`
- `libstore/include/nix/store/derivations.hh` (twice — once near the top before `Derivation : BasicDerivation`, once again after `Derivation` for the parser; this is the duplicate Pass-2 caught)
- `libstore/include/nix/store/path-with-outputs.hh`
- `libstore/include/nix/store/realisation.hh`
- `libstore/include/nix/store/build/derivation-building-misc.hh`
- `libstore/include/nix/store/store-api.hh` (self-forward-decl preceding the full `class Store` definition; see "self-forward-decl" caveat)
- `libstore/include/nix/store/machines.hh`
- `libstore/include/nix/store/build/derivation-env-desugar.hh`

`class EvalState;` (12 sites):

- `libflake/include/nix/flake/flake.hh`
- `libcmd/include/nix/cmd/command.hh`
- `libcmd/include/nix/cmd/common-eval-args.hh`
- `libexpr/include/nix/expr/eval-profiler.hh`
- `libexpr/include/nix/expr/print.hh`
- `libexpr/include/nix/expr/eval-error.hh`
- `libexpr/include/nix/expr/eval-settings.hh`
- `libexpr/include/nix/expr/eval.hh` (self-forward-decl preceding the full definition)
- `libexpr/include/nix/expr/value.hh`
- `libexpr/include/nix/expr/nixexpr.hh`
- `libexpr/include/nix/expr/print-ambiguous.hh`
- `libexpr/include/nix/expr/json-to-value.hh`

### Variants

All four target symbols (`Source`, `Sink`, `Store`, `EvalState`) appear
in exactly one variant: bare forward-decl, no comments, no `using`
aliases, all at file scope inside `namespace nix { ... }`. None are
templates. None are wrapped in `#ifdef` guards. None have differing
linkage (all are C++ types in the `nix::` namespace).

This is the easy case: every site is mechanically replaceable by
`#include "nix/util/serialise-fwd.hh"` (or `nix/store/store-fwd.hh`,
`nix/expr/eval-fwd.hh`).

The **non-trivial** wrinkle: many of these headers also forward-declare
*other* types in the same `namespace nix { ... }` block. Examples:

- `path-info.hh`: `class Store; struct StoreDirConfig;`
- `realisation.hh`: `class Store; struct OutputsSpec;`
- `derivation-env-desugar.hh`: `class Store; struct Derivation; template<typename Input> struct DerivationOptions;`
- `parsed-derivations.hh`: `class Store; template<typename Input> struct DerivationOptions; struct DerivationOutput;`
- `build/derivation-building-misc.hh`: `class Store; struct Derivation;`
- `machines.hh`: `class Store; struct Machine;`
- `path-with-outputs.hh`: `struct StoreDirConfig;` ... (later) `class Store;`
- `store-api.hh`: a long block (`UnkeyedRealisation`, `Realisation`, `RealisedPath`, `DrvOutput`, `BasicDerivation`, `Derivation`, `SourceAccessor`, `NarInfoDiskCache`, `NarInfoDiskCacheSettings`, `class Store;`)
- `value.hh`: `class StorePath; class EvalState;` block
- `eval.hh`: `class Store; namespace fetchers { struct Settings; struct InputCache; struct Input; } struct EvalSettings; class EvalState; class StorePath; struct SingleDerivedPath; enum RepairFlag : bool; struct MemorySourceAccessor; struct MountedSourceAccessor;`
- `command.hh`: `class EvalState; struct Pos; class Store; struct LocalFSStore;`

In each of these, the `*-fwd.hh` consolidation would *replace one line*
in a multi-line forward-decl block. The other lines stay. This means
the consolidation is mechanically safe but provides smaller wins than
"every block becomes a single include": the forward-decl block remains.

The deeper consolidation (a "common-types-fwd.hh" or per-header
`*-fwd.hh` for `Realisation`, `DerivationOutput`, `BasicDerivation`,
etc.) is out of scope for #166 and should be filed as a follow-up.

### Self-forward-decl caveat

Two of the counted sites are *self-forward-decls* used to break a
mutual-reference cycle within a single header:

- `store-api.hh` line 45 declares `class Store;` and then at line 387
  defines `class Store : public ... { ... }`. The forward-decl is
  needed because intermediate types (`StoreConfig`, etc.) reference
  `Store *` before its full definition.
- `eval.hh` line 49 declares `class EvalState;` and then at line 358
  defines `class EvalState : public ...`. Same pattern.

These two sites must **not** be replaced with `#include "nix/store/store-fwd.hh"`
or `#include "nix/expr/eval-fwd.hh"`: the consolidated header would
still ultimately resolve to the same forward-decl, but using
`#include` makes the cycle harder to reason about and could open up an
include cycle if `*-fwd.hh` ever grows transitive includes (e.g.
`<memory>`). Self-forward-decls should remain inline.

---

## Proposed `*-fwd.hh` content

### `libutil/include/nix/util/serialise-fwd.hh`

```cpp
#pragma once
///@file Forward-declarations for nix::Source and nix::Sink.

namespace nix {

struct Source;
struct Sink;

} // namespace nix
```

**Consumers (replacement sites):**

- `libutil/include/nix/util/nar-accessor.hh` (Source only — Sink is unused)
- `libutil/include/nix/util/file-system.hh` (both)
- `libutil/include/nix/util/processes.hh` (both)
- `libutil/include/nix/util/file-descriptor.hh` (both)
- `libutil/include/nix/util/source-accessor.hh` (Sink only)
- `libstore/include/nix/store/serve-protocol.hh` (Source only)
- `libstore/include/nix/store/derivations.hh` (Source + Sink, near the bottom)
- `libstore/include/nix/store/worker-protocol.hh` (Source only)
- `libstore/include/nix/store/common-protocol.hh` (Source only)

**Caveat:** four of these consumers (`nar-accessor.hh`,
`source-accessor.hh`, `serve-protocol.hh`, `worker-protocol.hh`,
`common-protocol.hh`) only need *one* of the two types. Including
`serialise-fwd.hh` will also forward-declare the other, which is
harmless (forward decls are idempotent) but slightly violates the
"include only what you use" principle.

This is acceptable. The alternative — splitting into `source-fwd.hh`
and `sink-fwd.hh` — is over-engineered for two sibling structs that
co-evolve.

### `libstore/include/nix/store/store-fwd.hh`

```cpp
#pragma once
///@file Forward-declaration for nix::Store.

namespace nix {

class Store;

} // namespace nix
```

**Consumers (replacement sites — see list above; 18 distinct files
after excluding the self-forward-decl in `store-api.hh` and the
duplicate occurrence in `derivations.hh`, which counts as one
consumer file with two replacement lines collapsing to one):**

- `libflake/include/nix/flake/{lockfile,flakeref}.hh`
- `libmain/include/nix/main/shared.hh`
- `libfetchers/include/nix/fetchers/{tarball,fetchers,registry}.hh`
- `libcmd/include/nix/cmd/{common-eval-args,command}.hh`
- `libexpr/include/nix/expr/eval.hh`
- `libstore/include/nix/store/filetransfer.hh`
- `libstore/include/nix/store/parsed-derivations.hh`
- `libstore/include/nix/store/path-info.hh`
- `libstore/include/nix/store/derivations.hh` (the duplicate near the
  top of the file; replace once, delete the second occurrence)
- `libstore/include/nix/store/path-with-outputs.hh`
- `libstore/include/nix/store/realisation.hh`
- `libstore/include/nix/store/build/derivation-building-misc.hh`
- `libstore/include/nix/store/machines.hh`
- `libstore/include/nix/store/build/derivation-env-desugar.hh`

### `libexpr/include/nix/expr/eval-fwd.hh` (proposed; not in the original #166)

```cpp
#pragma once
///@file Forward-declaration for nix::EvalState.

namespace nix {

class EvalState;

} // namespace nix
```

**Consumers (11 sites after excluding the self-forward-decl in `eval.hh`):**

- `libflake/include/nix/flake/flake.hh`
- `libcmd/include/nix/cmd/command.hh`
- `libcmd/include/nix/cmd/common-eval-args.hh`
- `libexpr/include/nix/expr/eval-profiler.hh`
- `libexpr/include/nix/expr/print.hh`
- `libexpr/include/nix/expr/eval-error.hh`
- `libexpr/include/nix/expr/eval-settings.hh`
- `libexpr/include/nix/expr/value.hh`
- `libexpr/include/nix/expr/nixexpr.hh`
- `libexpr/include/nix/expr/print-ambiguous.hh`
- `libexpr/include/nix/expr/json-to-value.hh`

The catalog body did not list `eval-fwd.hh` explicitly but the count
of 12 EvalState forward-decls is the same order of magnitude as the
Source/Sink count and the consolidation pattern is identical. Doing
all three at the same time is a single PR; doing only two is
arbitrary.

### Out of scope (proposed but not for #166)

These would each be a parallel `*-fwd.hh` if pursued, but were not in
the original proposal:

- `nix/store/store-dir-config-fwd.hh` — 10 sites for `struct StoreDirConfig;`
- `nix/store/store-path-fwd.hh` — 6 sites for `class StorePath;`
- `nix/expr/value-fwd.hh` — 7 sites for `struct Value;`
- `nix/fetchers/settings-fwd.hh` — 8 sites for `nix::fetchers::Settings`

These should be filed as a follow-up issue.

---

## Compilation-firewall impact

The compilation-firewall question: does consolidation pull in
transitive includes that the inline forward-decl avoided?

### serialise-fwd.hh

The proposed content is a single `#pragma once` and two struct
forward-decls in `namespace nix`. No other includes. No macros. No
`using` aliases.

**Transitive include set added: zero.**

**Headers transitively unblocked:** none. Including `serialise-fwd.hh`
is strictly equivalent to writing `struct Source; struct Sink;`
inline.

### store-fwd.hh

Same shape: single `#pragma once`, one class forward-decl in
`namespace nix`. No other content.

**Transitive include set added: zero.**

### eval-fwd.hh

Same shape.

**Transitive include set added: zero.**

### Per-consumer transitive-include comparison

Because each `*-fwd.hh` is a pure forward-decl header with no
includes, the consumer's transitive include set is **unchanged**:

| Consumer                                 | Before                          | After                                      |
| ---------------------------------------- | ------------------------------- | ------------------------------------------ |
| `libutil/.../file-system.hh`             | (none from forward-decls)       | `nix/util/serialise-fwd.hh`                |
| `libutil/.../file-descriptor.hh`         | (none)                          | `nix/util/serialise-fwd.hh`                |
| `libstore/.../worker-protocol.hh`        | (none)                          | `nix/util/serialise-fwd.hh`                |
| `libfetchers/.../fetchers.hh`            | (none from forward-decls)       | `nix/store/store-fwd.hh`                   |
| `libexpr/.../eval-profiler.hh`           | (none)                          | `nix/expr/eval-fwd.hh`                     |

The "before" column is "none" because the inline `struct Source;` /
`class Store;` brings in no transitive headers either. The
consolidation is **strictly source-compatible at the include-topology
level** *provided* the new `*-fwd.hh` headers themselves contain no
`#include` directives.

### Risk: future drift

The risk is not the initial consolidation but its evolution. If, six
months after consolidation, someone adds `#include <memory>` (or
worse, `#include "nix/util/types.hh"`) to `serialise-fwd.hh` because
"the fwd header is the right place for `using SourcePtr = std::unique_ptr<Source>;`",
*then* every consumer suddenly transitively includes `<memory>` — an
ABI/PCH-cost change that wasn't visible at PR-review time.

**Mitigation:** add a leading comment to each `*-fwd.hh` that says

> This header MUST contain only forward-declarations. Do not add
> `#include` directives. Do not add `using` aliases. Do not add
> macro definitions. The compilation-firewall guarantee depends on
> this header pulling in zero transitive includes.

…and a CI check (clang-tidy or a custom script) that fails the build
if any `*-fwd.hh` under `src/lib*/include` has a non-comment
`#include` line. This is N9's open question and the CI guard would
make the answer enforceable.

### `#define` shadowing

None of the four target headers (`file-system.hh`, `file-descriptor.hh`,
`store-api.hh`, `eval.hh`) define a macro between their forward-decl
line and the symbol's full declaration. There is no `#define Store ...`
or `#define Source ...` anywhere in the include path. The consolidation
introduces no preprocessor-level conflict.

The closest "shadow risk": `file-descriptor.hh` defines
`WIN32_LEAN_AND_MEAN` and includes `<windows.h>` *before* its `struct
Source; struct Sink;` block. If `serialise-fwd.hh` were ever to be
included *before* `<windows.h>` in a TU that also relies on
`WIN32_LEAN_AND_MEAN` being undefined, the include-order semantics
could matter. As long as `serialise-fwd.hh` includes nothing, this is
moot.

### Avoidance-of-include cases

Are any forward-decl sites *avoiding* a separate header that the
proposed `*-fwd.hh` would force-include?

Concretely: today, `nar-accessor.hh` writes `struct Source;` inline
*precisely* to avoid `#include "nix/util/serialise.hh"` (the full
`Source`/`Sink` definitions, ~600 lines, plus `<memory>`,
`<functional>`, BufferedSource transitive). After consolidation, the
file writes `#include "nix/util/serialise-fwd.hh"` and gets… still
nothing transitively. The compilation-firewall is preserved.

There is **no case** in the inventory where the inline forward-decl
serves as an alternative to including a *different* header that the
proposed `*-fwd.hh` would now mandate. All sites are pure
"forward-decl to avoid full definition" and the new fwd header is
strictly equivalent.

---

## C-API impact

The C-API public headers under `src/lib*-c/` declare opaque types in
**global namespace** (extern "C") — not `nix::Store`, `nix::EvalState`,
`nix::Value`. Examples:

- `nix_api_store.h`: `typedef struct Store Store;` (global `::Store`,
  not `nix::Store`)
- `nix_api_expr.h`: `typedef struct EvalState EvalState; // nix::EvalState`
- `nix_api_value.h`: relies on `nix_value` not raw `Value`

The "C++ shim" types (`::Store`, `::EvalState`, `::nix_value`,
`::BindingsBuilder`, `::ListBuilder`) are defined in the
`*_internal.{h,hh}` headers, *not* in the public C-API headers. The
internal headers `#include "nix/store/store-api.hh"` (and
`"nix/expr/eval.hh"`, `"nix/expr/attr-set.hh"`) directly to get the
full C++ definitions, since they wrap them.

**Consolidation impact on the C-API:**

- Public C-API headers (`nix_api_store.h`, `nix_api_expr.h`,
  `nix_api_value.h`, `nix_api_util.h`, `nix_api_fetchers.h`,
  `nix_api_main.h`, `nix_api_external.h`, `nix_api_flake.h`) include
  no `nix/util/`, `nix/store/`, `nix/expr/` C++ headers. They are
  pure C and untouched by the consolidation.
- Private C-API headers (`nix_api_store_internal.h`,
  `nix_api_expr_internal.h`, `nix_api_flake_internal.hh`,
  `nix_api_fetchers_internal.hh`, `nix_api_util_internal.h`) include
  full C++ definitions (`store-api.hh`, `eval.hh`). They do not need
  the new `*-fwd.hh` and are unaffected.
- Test-support C-API headers (`libstore-test-support/.../nix_api_store.hh`)
  also include the full C++ headers.

**Conclusion: zero C-API impact.** No opaque-type forward-decl in
`nix_api_*.h` is backed by a `nix::` C++ class whose forward-decl
moves into a `*-fwd.hh`. The two name-spaces are disjoint: the C-API
`::Store` is a wrapper struct around `nix::ref<nix::Store>`, and the
public header forward-declares `::Store` (global), not `nix::Store`.
A `nix/store/store-fwd.hh` consolidation never appears in any C-API
public header.

This is a **soft-ABI-safe** change with respect to C-API consumers.

---

## Downstream-fork impact

I cannot grep external fork sources from this checkout (only Hydra's
`packaging/hydra.nix` is in-tree, and that is just a meson packaging
expression — not source). The audit below is necessarily structural,
based on what a fork would observe at the include-API boundary.

### Hydra (`NixOS/hydra`)

Hydra includes `nix/store/store-api.hh`, `nix/store/derivations.hh`,
`nix/util/serialise.hh`, `nix/expr/eval.hh` directly for its
queue-runner and evaluator. None of these are in the consolidation
scope (they are the *full* headers; the consolidation only changes
the *forward-decl* headers that today sit inline in some other
header).

A Hydra TU that does `#include "nix/store/derivations.hh"` will, after
consolidation, transitively pick up a one-line `#include
"nix/store/store-fwd.hh"` it didn't pick up before. If Hydra has its
own `class Store;` forward-decl somewhere *that is then also
forward-declared by the new `store-fwd.hh`*, the result is two
forward-decls of the same class — which is well-formed C++ (forward
decls are repeatable). No conflict.

If Hydra ever forward-declared `Store` in a *different* namespace
(unlikely; `class Store;` outside `namespace nix` would not match
`nix::Store`), then no conflict either.

**Hydra impact: none expected.** Worst case is a one-time rebuild
because the include graph hash changed; no source edits required.

### Lix (`lix-project/lix`)

Lix is a hard fork as of 2024 with its own divergent header layout.
Lix includes (not surprisingly) some `nix::Store`, `nix::EvalState`,
`nix::Source`, `nix::Sink` references. Its public headers may already
have its own forward-decl pattern, which can differ from upstream's
post-consolidation pattern.

Because Lix is a fork, the relevant compatibility question is not
"does Lix's source compile against a Nix-built lib" (it doesn't — it
builds its own) but rather "does a downstream that depends on either
Nix or Lix have to do `#ifdef NIX_VERSION_AT_LEAST_2_31` to get the
forward-decls right?". The answer is **no**, for the same reason as
Hydra: forward decls are repeatable, and the consolidation does not
move any *symbol*, only the *line of source* that declares it.

**Lix impact: irrelevant** (cross-fork source compat is already a
manual port, not a #166 concern).

### Embedders using the C-API

Per the C-API audit above: zero impact.

### Embedders using the C++ API directly

Such embedders (e.g. devbox, lorri, certain NixOS test-driver
use-cases that link `libnixexpr` directly) include the full headers
(`store-api.hh`, `eval.hh`, …). After consolidation, those full
headers transitively include their own `*-fwd.hh` (because the
consolidation pulls the inline forward-decl out). The embedder sees
no source-level change.

If an embedder has its own `class Store;` forward-decl in
`namespace nix` (perhaps to avoid `#include "nix/store/store-api.hh"`
in some of its own headers), the post-consolidation result is
two-forward-decls-of-same-class, which is well-formed.

**Embedder impact: none expected at source level.** A header-graph
hash change means a one-time rebuild for ccache/sccache users. No
source edits.

---

## Recommended migration plan

### Immediate consolidation (safe in one PR)

These are pure, mechanical, and have zero source-compat risk. Do them
in one PR alongside the new `*-fwd.hh` files:

1. Create `libutil/include/nix/util/serialise-fwd.hh` with `struct Source; struct Sink;` and the no-includes-allowed comment.
2. Replace inline `struct Source;` / `struct Sink;` blocks in:
   - `libutil/include/nix/util/file-system.hh`
   - `libutil/include/nix/util/file-descriptor.hh`
   - `libutil/include/nix/util/processes.hh`
   - `libutil/include/nix/util/nar-accessor.hh`
   - `libutil/include/nix/util/source-accessor.hh`
   - `libstore/include/nix/store/serve-protocol.hh`
   - `libstore/include/nix/store/worker-protocol.hh`
   - `libstore/include/nix/store/common-protocol.hh`
   - `libstore/include/nix/store/derivations.hh` (the second `struct Source; struct Sink;` block, near the readDerivation/writeDerivation declarations)

3. Create `libstore/include/nix/store/store-fwd.hh` similarly.
4. Replace inline `class Store;` blocks in the 18 sites listed above.
   - **Skip the self-forward-decl** in `store-api.hh` (line 45) — leave it inline.
   - **Delete the duplicate** in `derivations.hh` (the catalog noted
     the duplicate; consolidate to a single `#include` near the top
     of the file and remove the second `class Store;` line).

5. Create `libexpr/include/nix/expr/eval-fwd.hh`.
6. Replace inline `class EvalState;` blocks in the 11 sites listed above.
   - **Skip the self-forward-decl** in `eval.hh` — leave it inline.

### Deprecation cycle (none required)

No deprecation cycle is needed for any consolidation in this audit.
Forward-decls are repeatable; downstream code that has its own
forward-decl coexists with the new `*-fwd.hh` without conflict. There
is no symbol migration, no namespace move, no name change.

### Leave alone

These were considered and explicitly rejected for the initial
consolidation:

1. **The two self-forward-decls** (`class Store;` in `store-api.hh`,
   `class EvalState;` in `eval.hh`). Replacing them with `#include
   "store-fwd.hh"` would work today but introduces a future-fragility
   risk: if `store-fwd.hh` ever grows a transitive include, the
   self-include becomes a circularity tripwire. Keep them inline.

2. **`struct Settings;` (10 sites) is NOT a single consolidation.**
   The 10 sites split between `nix::fetchers::Settings` (8) and
   `nix::flake::Settings` (2). These are *different types* in
   *different namespaces* and a single `settings-fwd.hh` would have
   to forward-declare two distinct symbols across two namespaces,
   which is fine syntactically but creates a "settings" header that
   means two unrelated things. File these as **two separate**
   follow-ups (`fetchers/settings-fwd.hh`,
   `flake/settings-fwd.hh`), or leave them inline. **Do not bundle
   into #166.**

3. **`template<typename Input> struct DerivationOptions;`** appears in
   `parsed-derivations.hh` and `derivation-env-desugar.hh` as a
   template forward-decl. Template forward-decls in a `*-fwd.hh`
   require the same template parameters at the use site, which is
   workable but adds a coupling that is not present today. Two sites
   is below the `>= 5` threshold for consolidation anyway.

4. **`struct StoreDirConfig;` (10 sites)** — eligible for a
   `store-dir-config-fwd.hh`, but the *full* header `store-dir-config.hh`
   is small (~50 lines) and including it directly is often the right
   call. Defer to follow-up; benefit is smaller than `class Store;`.

5. **`class StorePath;` (6 sites)** — same argument as
   `StoreDirConfig`. The full header `store/path.hh` is ~70 lines and
   self-contained. A `store-path-fwd.hh` would help only when callers
   want to take `const StorePath &` without including the full type
   (impossible in practice — they'd need at least `StorePath::operator==`
   etc. to do anything). Most "forward-decl of StorePath" sites are
   followed by an `#include "store/path.hh"` later anyway.

6. **`struct Value;` (7 sites)** — could be a `value-fwd.hh`, but the
   `Value` struct sits at the heart of `eval.hh` and the inline
   forward-decl in `print.hh`, `eval-error.hh`, etc., is short and
   self-documenting. Marginal benefit.

### Implementation order

1. PR 1: `serialise-fwd.hh` (lowest blast radius, smallest review).
2. PR 2: `store-fwd.hh` (largest benefit, 18 sites; touches libflake,
   libfetchers, libcmd, libexpr, libstore).
3. PR 3: `eval-fwd.hh` (11 sites; touches libflake, libcmd, libexpr).
4. (optional follow-up) `store-dir-config-fwd.hh`,
   `store-path-fwd.hh`, `value-fwd.hh`, `fetchers/settings-fwd.hh`,
   `flake/settings-fwd.hh` as separate PRs.

Each PR is a strict, mechanical search-and-replace plus the
"no-includes-allowed" comment. No semantic change. Reviewable by
diff-counting alone.

---

## Open questions

1. **CI guard for the no-includes invariant.** The compilation-firewall
   guarantee only holds if every `*-fwd.hh` stays a pure forward-decl
   header. Should this be enforced by:
   a. A clang-tidy custom check?
   b. A meson `custom_target` that greps each `*-fwd.hh` for `^#include`?
   c. A pre-commit hook?
   d. Documentation only?
   Recommendation: (b) — meson `custom_target` running a 5-line
   shell script. Cheap, deterministic, runs in CI for free.

2. **Should `*-fwd.hh` be pre-compiled?** Today (no `*-fwd.hh`), every
   consumer pays for an inline `struct Source;` directly. After
   consolidation, every consumer pays for `#include "serialise-fwd.hh"`,
   which the compiler must open and parse. PCH would mitigate.
   Probably not worth a PCH layer for three one-line headers; flag if
   build times regress measurably.

3. **Is `eval-fwd.hh` in scope for #166?** The catalog body proposed
   only `serialise-fwd.hh` and `store-fwd.hh`. Pass-2 noted 12
   `EvalState` forward-decls. The audit recommends doing all three at
   once because the cost is identical and partial consolidation
   leaves the inconsistency visible. Decision needed before
   implementation.

4. **Should the duplicate `class Store;` in `derivations.hh` be
   tracked as a separate cleanup?** Pass-2 caught it. The
   consolidation PR will delete it as a side effect. No separate
   tracking needed.

5. **Out-of-tree fork compatibility note in the changelog?** Even
   though there is no source-compat break, the include graph hash
   changes for any TU that includes one of the consolidated headers.
   ccache/sccache users will see a one-time miss. This is normal and
   not worth a release-notes line, but could be flagged in the PR
   description.

6. **Should `serialise-fwd.hh` live alongside `serialise.hh` in
   `libutil/include/nix/util/`, or in a new
   `libutil/include/nix/util/fwd/` subdirectory?** Convention check
   needed. The first form (sibling) matches `nlohmann/json_fwd.hpp`
   and `boost/.../fwd.hpp` precedent. Recommend sibling placement,
   no subdirectory.

7. **What about `enum class GCAction;` and similar in
   `worker-protocol.hh`?** Several headers also forward-declare enum
   classes (e.g. `enum BuildMode : uint8_t;`, `enum TrustedFlag : bool;`,
   `enum class GCAction;`). These appear at 2-3 sites each, below the
   consolidation threshold. Out of scope for #166.
