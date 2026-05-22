# Refactoring candidates

Split per-section from the previous monolithic `CANDIDATES.md`. Each section
file lists the relevant candidates with the validation verdict, effort class,
and any corrections discovered during the verification pass. The verification
pass read the cited source end-to-end; where the original candidate text
drifted from current source, the entry has a `**Validation:**` paragraph
with the corrected detail.

## What this catalog targets

The catalog tracks **duplication, complexity, and generalisation
opportunities** — places where C++23, current Boost, or simple
restructuring lets us replace bespoke or hand-rolled code with cleaner
abstractions. Concretely, the kinds of debt collected here:

- **Macro-driven boilerplate that should be a template.** CRTP,
  deducing-this, concept-constrained templates, or X-macros each replace
  a recurring `#define`-based pattern. Examples: `MakeError`,
  `MAKE_WRAPPER_CONSTRUCTOR`, the three protocol macro families,
  `MakeBinOp`, the `Setting<T>{this, ...}` initialiser pattern.
- **Two parallel implementations that are really one knob.** A pair
  differing in a single mutex, flag, or type parameter is one template
  parameter, not two classes. Examples: `Sync<T>` vs `SharedSync<T>`,
  `Pipe::create` Unix vs Windows non-blocking, `Counter`
  enabled/disabled, the `WorkerProto` / `ServeProto` / `CommonProto`
  trio.
- **Diamond inheritance dressed up as polymorphism.** When the type
  hierarchy is really a Cartesian product (platform × strategy), prefer
  a strategy/policy split with composition. Example: the
  `DerivationBuilder` diamond (chroot strategy × Linux/FreeBSD/Darwin).
- **Stdlib uplift made possible by C++23.** Defaulted `operator<=>` for
  the libc++16 workaround sites, `std::move_only_function` for
  `nix::fun`, `std::ranges::append_range` for the `append` shim,
  `std::format` for `boost::format` + `HintFmt`, `std::unreachable()`
  for genuinely-unreachable switch fall-through.
- **Hand-rolled abstractions where Boost or stdlib already has it.**
  Bespoke `Goal::Co` coroutine machinery vs `boost::asio::awaitable`,
  ad-hoc concurrent-map insert-on-miss vs
  `boost::concurrent_flat_map::try_emplace_and_cvisit`, manual
  `nlohmann::json_sax` parser vs `nlohmann::json::parse + walker`.
- **Globals-as-architecture.** Process-wide singletons or `extern`
  globals that are really per-subsystem state, where dependency
  injection or per-`Store` / per-`EvalState` ownership belongs.
  Examples: the 19 `Config`-derived globals, `drvHashes` shared across
  stores, the four logging globals.
- **Latent bugs hiding inside duplication.** Each cluster of "the same
  thing five times" tends to have one site that drifted. Surfacing the
  duplication and consolidating it forces these into the open. Already
  surfaced examples: `SQLiteSettings::useWAL` UB, `getCustomRegistry`
  first-call-wins, `CollectGarbage`'s unconditional `ignoreLiveness`
  throw, `AttrDb::getAttr` bypassing the `doSQLite` failed-flag
  protocol.
- **Dead code.** Declarations without definitions, parameters never
  read, `#if 0` blocks, leaked macros, vestigial `extern` globals.
  Smallest tier; usually first to land because it's pure deletion.

What the catalog does **not** target: cosmetic changes (formatting,
naming preferences), feature additions, performance work without
duplication or complexity to fix, or "modernise for its own sake" if the
existing code is clear and the proposed replacement isn't simpler.

## Verdict legend

- **VALID** — claim corroborated by source; refactor is genuinely available.
- **PARTIALLY VALID** — core claim holds but a detail (file path, count,
  proposed mechanism) needs adjustment. The validation paragraph spells out
  what.
- **INVALID** — claim does not hold under closer reading; do not pursue.
- **OBSOLETE** — duplication is real but too small to justify a refactor.

## Effort classes

`trivial` (one-line edit) → `small` → `medium` → `large` → `structural`
(multi-PR; reshapes module boundaries).

## Sections

| File | Range | Title |
| ---- | ----- | ----- |
| [01-wire-serialisation.md](01-wire-serialisation.md) | 1-10 | Duplicated wire / serialisation logic |
| [02-parallel-stores.md](02-parallel-stores.md) | 11-16 | Parallel store implementations |
| [03-repeated-boilerplate.md](03-repeated-boilerplate.md) | 17-25 | Repeated boilerplate |
| [04-per-platform-symmetry.md](04-per-platform-symmetry.md) | 26-29 | Per-platform symmetry |
| [05-inheritance-flattening.md](05-inheritance-flattening.md) | 30-35 | Inheritance chains worth flattening |
| [06-multi-impl-base.md](06-multi-impl-base.md) | 36-47 | Multi-implementation patterns that could share a base |
| [07-dead-stale-code.md](07-dead-stale-code.md) | 48-60 | Dead or stale code |
| [08-duplicated-parsers.md](08-duplicated-parsers.md) | 61-68 | Duplicated parsers / regexes |
| [09-cache-keys.md](09-cache-keys.md) | 69-71 | Cache-key construction scattered across modules |
| [10-other.md](10-other.md) | 72-88 | Other distinct candidates |
| [11-legacy-cli.md](11-legacy-cli.md) | 89-95 | Legacy CLI duplication |
| [12-libutil-libstore-core-extras.md](12-libutil-libstore-core-extras.md) | 96-104 | libutil + libstore-core extras |
| [13-libexpr-extras.md](13-libexpr-extras.md) | 105-120 | libexpr extras |
| [14-vestigial-stdlib.md](14-vestigial-stdlib.md) | 121-130 | Vestigial code, dead workarounds, stdlib replacements |
| [15-globals-settings.md](15-globals-settings.md) | 131-149 | Globals, settings architecture, and dependency-injection |
| [16-libstore-build-audit.md](16-libstore-build-audit.md) | 150-160 | libstore/build deep audit |
| [17-cross-cutting.md](17-cross-cutting.md) | 161-175 | Cross-cutting: platforms, headers, magic numbers |
| [18-daemon-protocol-audit.md](18-daemon-protocol-audit.md) | 176-188 | Daemon and protocol-dispatch deep audit |
| [19-eval-core-fetcher-lookup-json.md](19-eval-core-fetcher-lookup-json.md) | 189-200 | libexpr eval-core: fetcher primops, lookup-path, JSON |
| [20-eval-core-cache-attrset-profiler.md](20-eval-core-cache-attrset-profiler.md) | 201-209 | libexpr eval-core: cache, attr-set, profiler |
| [21-eval-core-evalstate-value.md](21-eval-core-evalstate-value.md) | 210-217 | libexpr eval-core: EvalState and Value |

## Cross-references

Candidates reference one another by number (e.g. "compounds with #34"). The
range column above is the lookup table — find the section file by number,
then the heading inside that file.

## Related

- [`../INVENTORY.md`](../INVENTORY.md) — navigation map and preserved
  invariants across the 19 verified shards.
- [`../verified/`](../verified/) — the per-shard verified inventories that
  the candidates cite.
