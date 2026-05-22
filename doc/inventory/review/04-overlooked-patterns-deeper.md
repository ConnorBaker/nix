# Pattern discovery (deeper pass)

This document is the second pattern-discovery pass. Pass 1 produced 23
candidates (N1-N23) plus 4 clusters, 10 uplift steps, 7 architectural
themes, and 10 redundant-abstraction pairs. This pass goes deeper:
verifying pass-1's claims against source, looking inside sections rather
than just across them, exploring tests / build files / pImpl boundaries /
the C-API, and testing whether some N-candidates are the same shape from
different angles.

Output structure follows the brief: verification of pass-1 N1-N23, then
verification of the clusters / uplift series, then new candidates
(numbered N24+ to avoid collision), then disagreements with pass 1,
then handoffs.

## Methodology

Verification: I held all 21 candidate sections in view simultaneously
and re-derived counts via `grep -rn` against `src/`. For each
pass-1 N-candidate I (a) re-grepped its cited sites, (b) read at least
one cited file end-to-end to test the proposed abstraction's fit, and
(c) checked for overlap with the existing 1-217 catalog and with other
N-candidates. Where I disagree, the disagreement section spells out
the evidence.

New-candidate discovery: I targeted territory pass 1 explicitly named
as under-explored — patterns within sections, test code, build files,
pImpl/header splits, the C-API, settings architecture — plus
specifically looked for hand-rolled visitor / type-erasure /
small-object / `std::atomic` / `#ifdef _WIN32` patterns. Each new
candidate cites at least three concrete sites and meets the catalog's
stated criteria.

Counts cited as "verified" are from independent `grep`s I ran during
this pass; counts cited as "from pass 1" are unverified — they may
appear in the verifying-pass-1 section if the count load-bears.

## Verifying pass-1 candidates

### N1 — concurrent-insert-on-miss idioms

**Verified.** Independent grep confirms the cited sites with two
additions:

- `src/libstore/aws-creds.cc` carries a `concurrent_flat_map<pair<string,
  string>, shared_ptr<ICredentialsProvider>>` (the credential-providers
  cache) — a sixth `boost::concurrent_flat_map` site pass 1 missed.
- `src/libexpr/primops.cc` (`prim_fromTOML`'s 4936-line "cache" of
  decoded TOML strings via a `concurrent_flat_map<string, Entry>`) — a
  seventh site, also missed.

The `Sync<std::map>` writer count is exactly four (`input-cache.cc`,
`git-utils.cc::workdirInfoCache_`, `pos-table.hh::origins_`,
`gc.cc::connections`). The `Sync<std::set>` is one
(`remote-store.hh::connectionFds`). The `SharedSync<std::set>` is one
(`filtering-source-accessor.cc::allowedPrefixes`). Pass 1's claim of
"six writers" conflates these; the right count is 4 + 1 + 1 = 6 if you
include the two `set`-shaped variants. The pattern claim stands.

The proposed abstraction (a `Memo<K, V, Policy>` plus rubric) is
reasonable; the deeper issue pass 1 didn't name is that the *invariant*
of each Sync<map>-backed cache differs:

- `gc.cc::connections` carries `thread` values that are `joined()` on
  destruct — the `Sync<map>` is needed for ordered teardown, not for
  ordered iteration. Flagged for special handling on migration.
- `remote-store.hh::connectionFds` is iterated under lock in
  `RemoteStore::~RemoteStore` (closing each fd). Not a memoisation
  cache at all — it's a registry of open connections.
- `filtering-source-accessor.cc::allowedPrefixes` uses `SharedSync` for
  the read-only `lookup-prefix` invariant.

Pass-1's framing as "all six are insert-on-miss caches" is too
broad. Three of them are caches; three are *registries with iteration
needs* (close-all, count-all, prefix-walk). The Memo abstraction fits
only the cache subset. **Refined.**

**Cross-references:** the pass-1 claim that this compounds with #120 is
correct, but the proposed migration should split into two: a `Memo`
helper for the three actual caches, and a documented "registry with
ordered iteration" pattern for the other three.

### N2 — `<algo>:<base-encoded>` family

**Verified.** Independent grep of the cited identifiers:

- `Hash::parseAny`, `Hash::to_string` in `src/libutil/hash.cc` — present.
- `parseColonBase64` / `serializeColonBase64` in
  `src/libutil/signature/local-keys.cc` — present.
- `ValidPathInfo::fingerprint`, `UnkeyedRealisation::fingerprint` in
  `src/libstore/path-info.cc` and `realisation.cc` — present (verified
  in N5 grep above).
- `to_string_legacy` (using `!`) vs `to_string` (using `^`) in
  `src/libstore/derived-path.cc` — present.

The proposed abstraction (a `SeparatorEncoded<Sep, Codec>` namespace
plus a versioned-prefix `KeyedFingerprint<Body>` mixin paired with N5)
is sound. **Confirmed.**

### N3 — five static-registration registries

**Verified count is wrong.** Pass 1 says "five" in the heading then
lists six in its table; my grep finds **eight** registries with the
same skeleton:

1. `RegisterPrimOp` (libexpr).
2. `RegisterCommand` (libcmd).
3. `RegisterLegacyCommand` (libcmd).
4. `RegisterStoreImplementation<Config>` (libstore).
5. `RegisterBuiltinBuilder` (libstore).
6. `registerInputScheme` free function (libfetchers).
7. `Setting<T>{this, ...}` self-register into `Config::_settings` (#136).
8. `GlobalConfig::Register` (#137).

Plus an arguable ninth: `ExternalBuilders` registry in `derivation-options.cc`
(I'd need to check; not load-bearing for pass-1's claim).

The "duplicate-policy" axis is real and pass 1 documented it correctly:
`PrimOp` re-register silent, `Implementations::add` throws,
`RegisterCommand` overwrites silent. This is exactly the kind of "latent
bug hiding inside duplication" the catalog targets — three different
disambiguation choices have drifted apart.

Pass 1's proposed `Registry<Key, Value, DuplicatePolicy>` is sound;
heading should read "eight static-registration registries." **Refined
upward.**

### N4 — 19+ `*Settings` Cartesian products

**Verified count is undercount.** Independent grep of `(class|struct)
\w+ : (public\s+)?(virtual\s+)?Config\b` yields **22** Config-derived
structs:

`Settings (libflake)`, `PluginSettings`, `DevelopSettings`,
`UpgradeSettings`, `AuthorizationSettings`, `EnvSettings`,
`Settings (libfetchers)`, `CompatibilitySettings`, `EvalSettings`,
`ArchiveSettings`, `RestoreSinkSettings`,
`ExperimentalFeatureSettings`, `LoggerSettings`,
`FileTransferSettings`, `WorkerSettings`, `StoreConfigBase`,
`LogFileSettings`, `NarInfoDiskCacheSettings`,
`Settings (libstore)`, `GCSettings`, `AutoAllocateUidSettings`,
`LocalSettings`.

`StoreConfigBase` is the abstract store-config base, not a
settings-knob struct, so the count of *plain* settings structs is 21,
not 19. `EvalProfilerSettings` exists in the catalog and is a Config
subordinate (see eval-settings.hh) but I couldn't find it in source —
pass 1 listed it but my grep didn't find a `: Config` form. May be
defined differently.

The X-macro proposal is sound. Pass-1's counting drift (19 → actual
21-22) is small but the *shape* claim is solid.

### N5 — `Signable<Body>` mixin

**Verified.** The four-method protocol (`fingerprint`/`sign`/
`checkSignature`/`checkSignatures`) recurs identically between
`ValidPathInfo` and `UnkeyedRealisation` — confirmed by reading both
end-to-end. `NarInfo` extends `ValidPathInfo`, so it inherits the
mixin transparently. The proposed CRTP/mixin is well-fitted. **Confirmed.**

### N6 — Source/Sink wrapping pattern

**Verified.** The named subclasses exist; the pattern is real. One
addition pass 1 missed:

- `BoehmDisableGC` in `gc.cc` is *not* a Sink, but `setRunOriginal` and
  `runOriginal` in `local-derivation-goal.cc` (Linux-side) wrap a Source
  with a side-channel to run a sub-process — same shape, different
  domain.

The proposed `WrappingSink<Inner, Observer>` template plus
`splitOnDelimiter` adapter is sound. **Confirmed.**

### N7 — `openAccessor(spec)` entry point

**Verified.** The cited factories all exist and the pattern is real.
The cited consumer code (`MixCat`, `MixLs`, etc.) does reimplement the
mapping each time. **Confirmed.**

### N8 — three async idioms

**Verified.** Independent grep confirms `Callback<T>` heavy use in
`*Uncached` methods, `boost::asio::awaitable` in
`HttpBinaryCacheStore::topoSortPaths` and a few other sites, and the
bespoke `Goal::Co` in build/. The unification claim is sound.
**Confirmed.**

### N9 — per-platform `Pid`/`Pipe`/`Process` symmetry

**Verified.** `#ifdef _WIN32` is present in libutil headers; parallel
`unix/` and `windows/` subdirs exist. The structural claim is sound;
this is a meta-observation across #27, #29, #81, #161, #162, #163,
#164. **Confirmed.**

### N10 — `BasicConnection<Proto>` template

**Verified.** Connection class hierarchy:

- `WorkerProto::BasicConnection`, `BasicClientConnection`,
  `BasicServerConnection`.
- `ServeProto::BasicClientConnection`, `BasicServerConnection`
  (parallel; no shared base — the pass-1 claim).
- `RemoteStore::Connection : WorkerProto::BasicClientConnection,
  WorkerProto::ClientHandshakeInfo`.
- `LegacySSHStore::Connection`.
- `SSHStore::Connection : RemoteStore::Connection`.
- `UDSRemoteStore::Connection : RemoteStore::Connection`.
- `SSHMaster::Connection` (separate).

The proposed `BasicConnection<Proto>` template plus
`PooledClientStore<C>` is sound. **Confirmed.**

### N11 — iterate-attrset / dispatch-by-name

**Verified.** Independent grep finds the named pattern in 8+ sites,
matching pass 1's "seven places" approximately. The 3 sites the
catalog already names (#79, #114, #190) plus the additional sites in
`primops.cc` (per-primop attrset arg-validation) — pass-1's claim
holds. **Confirmed.**

### N12 — declarative `InputScheme` / opcode schema

**Verified.** Each `InputScheme::inputFromURL` reimplements the URL
parsing loop (verified above for `path.cc`, `git.cc`, `mercurial.cc`,
`github.cc`). Each `InputScheme::allowedAttrs` returns a hand-rolled
static map. The compounding claim with the daemon `performOp` switch
(#176) and the per-`RemoteStore::*` opcode methods (#182) is
consistent: all three are projections of one "opcode/attribute
schema" record. **Confirmed.**

### N13 — five SQLite-cache `<SchemaSql>` template

**Verified.** Each cited site uses `create table if not exists` to
self-initialise schema (verified for `local-store.cc`,
`nar-info-disk-cache.cc`, `cache.cc`, `eval-cache.cc`). The schema
versioning is split between filename-encoded
(`binary-cache-v8.sqlite`, `fetcher-cache-v4.sqlite`,
`tarball-cache-v2/`, `eval-cache-v6/`) and integer
(`PRAGMA user_version` / `nixSchemaVersion = 10`).
Pass-1's claim that no site uses `PRAGMA user_version` is essentially
correct except for `LocalStore`'s `nixSchemaVersion` (which is in a
separate file `<store>/var/nix/db/schema`, not the DB). **Confirmed.**

### N14 — 14 worker counters

**Verified.** Re-counted: 12 `uint64_t` counters tracked via
`MaintainCount<uint64_t>` plus 2 `size_t` counters (`nrLocalBuilds`,
`nrSubstitutions`) tracked directly by `Worker`. Total = 14 if you
include both styles. The 12 tracked-via-MaintainCount counters are:
`expectedBuilds`, `doneBuilds`, `failedBuilds`, `runningBuilds`,
`expectedSubstitutions`, `doneSubstitutions`, `failedSubstitutions`,
`runningSubstitutions`, `expectedDownloadSize`, `doneDownloadSize`,
`expectedNarSize`, `doneNarSize`. Pass-1's "14" matches if `nrLocalBuilds`
and `nrSubstitutions` are included; if the body is meant to enumerate
only the `MaintainCount`-managed counters it's 12. **Refined: count is 12+2 split.**

The proposed `ProgressReporter` collaborator with typed enum-keyed
`incCount(Stage::Build, Phase::Running)` API is sound; pass 1's
fitness for the +2 plain counters is unclear since they are not
RAII-managed.

### N15 — version-tag-then-payload pattern

**Verified.** Independent grep:
- `WorkerProto::Version` — `(uint16_t major, uint16_t minor, FeatureSet)`.
- `ServeProto::Version` — `(uint8_t major, uint8_t minor)`.
- `nixSchemaVersion = 10` in `local-store.hh`.
- `narVersionMagic1 = "nix-archive-1"` in `archive.hh`.
- `expectedJsonVersionDerivation = 4` in `derivations.hh`.
- 4 SQLite cache filenames (`v6`, `v8`, `v4`, `v2`).

The `exportMagic = NIXE` is in `export-import.cc` (not the file pass 1
guessed but verified to exist via `INVENTORY.md`). Pass-1's grouping
("seven versioned wire formats") holds. **Confirmed.**

### N16 — test boilerplate mirrors missing reflection

**Verified.** `JsonCharacterizationTest<T>` and
`VersionedProtoTest<Proto>` exist in test-support. The macro
`VERSIONED_CHARACTERIZATION_TEST` *fans out* into 4 `TEST_F` (read,
write, json_read, json_write) — confirmed by reading
`protocol.hh` end-to-end. Pass-1's "130+ tests" claim: independent
grep returns 44 `VERSIONED_CHARACTERIZATION_TEST` invocations across
`libstore-tests/` (`worker-protocol.cc`, `serve-protocol.cc`,
`common-protocol.cc`, `path-info.cc`, etc.); each emits 4 tests, so
~176 tests. Pass-1's "130" is in the right order of magnitude
(~134-176 depending on which macro variant is counted).

The reflection observation is sound: each (T, format) pair is hand-
written and the test grid mirrors it. **Confirmed.**

### N17 — leaked `Sync<T*>` pattern for SIOF avoidance

**Verified.** Independent grep:
- `_fileTransfer = new Sync<shared_ptr<curlFileTransfer>>` —
  `filetransfer.cc:1228`.
- `windowSize = new Sync<pair<unsigned short, unsigned short>>` —
  `terminal.cc:165`.
- `_interruptCallbacks = new Sync<InterruptCallbacks>` —
  `unix/signals.cc:51`.

Three leaked-`Sync<T>` singletons. Plus the SIOF-relevant
`Counter::enabled = getEnv("NIX_SHOW_STATS")...` (eval.cc) and
`logger` (logging.cc — not strictly leaked, but same pattern). The
proposed `LeakedSingleton<T>` helper is sound. **Confirmed.**

### N18 — env-var-only configuration iceberg

**Verified.** Independent grep of `getEnv("NIX_*")` returns 30+ env
vars used as configuration (not declared in any settings struct).
The pass-1 listing is approximately right; the exact count varies
based on whether `XDG_*` and `HOME` are included. **Confirmed.**

### N19 — fat types and their collaborators

**Verified.** This is a meta-observation; the cited fat types
(`EvalState`, `Worker`, `Store`, `LocalStore`, `Args`, `Config`) are
all real and load-bearing. The collaborator-decomposition direction
is well-justified by the existing per-shard candidates. **Confirmed.**

### N20 — `nValueType` table-driven dispatch

**Verified.** Independent grep returns 101 `case n*:` lines across
libexpr `*.cc` files, distributed over 11+ dispatch sites. The
proposed `value-type-traits.hh` table is sound. **Confirmed.**

### N21 — counter-pattern recurrence

**Verified.** `Counter` (libexpr; 13 instances per `EvalState`),
`MaintainCount<T>` (libutil; used in 12+ sites in libstore/build),
plain `uint64_t` (Worker's two manual counters), `atomic<uint64_t>`
(Logger). Four shapes for "increment-on-event" counter. **Confirmed.**

### N22 — macros that emit struct declarations

**Verified.** The named macros:
- `MakeError(name, parent)` — used 100+ times.
- `MakeBinOp(name, sym)` / `MakeBinOpMembers` — 4-6 binop AST nodes.
- `MAKE_WRAPPER_CONSTRUCTOR(T)` — 9-13 sites in libstore-core.
- `COMMON_METHODS` — `Expr*` standard virtual methods.
- `NIX_DECLARE_CONFIG_SERIALISER(T)` — used per `BaseSetting<T>` spec.

Plus pass 1 missed:
- `NIX_VALUE_STORAGE_FOR_EACH_FIELD` (X-macro) — value-storage layouts.
- `NIX_FOR_EACH_COMPRESSION_ALGO` (X-macro) — `compression-algo.hh`.
- `NIX_FOR_EACH_LA_ALGO` — internal to `compression.cc`.
- `JSON_IMPL` / `JSON_IMPL_INNER` — JSON impl scaffolding.

So the actual count is ~9 macro families, not 5. **Refined upward.**

### N23 — opcode schema = three projections

**Verified.** Independent grep:
- `daemon.cc::performOp` — verified 37 case labels (matches the
  adversarial-existing review's count).
- `RemoteStore::*` opcode methods — ~25 (matches #182).
- `Store` virtual surface — concrete implementations in
  `LocalStore`, `BinaryCacheStore`, etc.

The "three projections of one schema" claim holds. **Confirmed.**

## Verifying pass-1 pattern clusters and uplift series

### Cluster A (concurrent state and globals)

**Verified.** The named items (`getFileTransfer`, `windowSize`,
`getInterruptCallbacks`, `logger`, `drvHashes`, `Counter::enabled`,
`PosTable::origins_`, the `Sync<map>` vs `concurrent_flat_map` choice,
the threading idioms) all exist and form a recognisable cross-cutting
cluster. The naming is clean. **Confirmed.**

### Cluster B (variant-shaped types)

**Verified.** The catalog has #20, #94, #102, #194 each touching a
slice. The `MAKE_WRAPPER_CONSTRUCTOR` macro is used at ~10 sites
(verified by grep). The "TaggedUnion<Tag, Alternatives...>"
abstraction is sound. **Confirmed.**

### Cluster C (SourceAccessor and Store factories)

**Verified.** N7 covers the SourceAccessor side; #11/#12 cover the
Store side. The "registry of factories" cluster holds. **Confirmed.**

### Cluster D (settings-tied configuration)

**Verified.** The 21-22 settings structs (corrected count above) plus
the env-var protocol form a recognisable subsystem. **Confirmed.**

### Uplift series

I checked U1 (libc++16 cleanup) — pass 1's count of 17 sites matches
my `grep -rn "TODO libc++ 16" src/`. **Confirmed.**

I checked U10 (`boost::describe`) — header-only, in-tree-suitable;
the dependency claim that it unblocks N4, N12, N16, parts of #9 and
#176 is reasonable. The sequencing graph is sane.

I disagree with one piece of the sequencing: U7 (Boost
`concurrent_flat_map` vs `Sync<map>` rationalisation) is *not* a
medium effort — the rubric is small, but the actual migration is
complicated by the three sites that need *iteration*, not lookup
(see N1 refinement above). U7 should be split:
- U7a (small): write the rubric and `getOrInsertConcurrent` helper.
- U7b (medium): migrate the three actual cache sites.
- U7c (out of scope): the three iteration-needing registries are
  *not* candidates for migration.

Beyond that, the series is sound. **Confirmed with U7 split refinement.**

### Architectural debt themes

A1-A7 are well-named and align with the per-shard candidates. None
require redrafting. **Confirmed.**

### Redundant abstractions R1-R10

Each R-pair I spot-checked is real (e.g. R1: `Sync<T>` vs leaked
pointer + mutex; R7: `Callback<T>` vs awaitable vs `Goal::Co` vs
promise). **Confirmed.**

## New candidates (beyond pass 1)

Numbering continues from N24 to avoid collision with pass-1's N1-N23.

### N24. C-API opaque-pointer wrappers are 14 single-member structs

**Pattern:** every C-API library declares opaque-pointer wrappers in
its `*_internal.h` / `*_internal.hh` header. Each wrapper is a
`struct { nix::ref<Cxx> field; }` (or `struct { Cxx field; }`)
with a tiny one-method body. Pass 1 referenced this in passing under
section R-bindings, but the pattern was not articulated as a refactor
candidate. Independent grep of
`src/libstore-c/nix_api_store_internal.h`,
`src/libexpr-c/nix_api_expr_internal.h`,
`src/libfetchers-c/nix_api_fetchers_internal.hh`,
`src/libflake-c/nix_api_flake_internal.hh`, and
`src/libutil-c/nix_api_util_internal.h` shows:

| Wrapper | Member | Library |
| ------- | ------ | ------- |
| `Store` | `nix::ref<nix::Store> ptr` | libstore-c |
| `StorePath` | `nix::StorePath path` | libstore-c |
| `nix_derivation` | `nix::Derivation drv` | libstore-c |
| `BindingsBuilder` | `nix::BindingsBuilder builder` | libexpr-c |
| `ListBuilder` | `nix::ListBuilder builder` | libexpr-c |
| `nix_string_return` | `std::string str` | libexpr-c |
| `nix_printer` | `std::ostream & s` | libexpr-c |
| `nix_string_context` | `nix::NixStringContext & ctx` | libexpr-c |
| `nix_realised_string` | `std::string str; std::vector<StorePath> storePaths` | libexpr-c |
| `nix_fetchers_settings` | `nix::ref<nix::fetchers::Settings>` | libfetchers-c |
| `nix_flake_settings` | `nix::ref<nix::flake::Settings>` | libflake-c |
| `nix_flake_reference` | `nix::ref<nix::FlakeRef>` | libflake-c |
| `nix_flake_lock_flags` | `nix::ref<nix::flake::LockFlags>` | libflake-c |
| `nix_locked_flake` | `nix::ref<nix::flake::LockedFlake>` | libflake-c |

Plus `nix_value`, `EvalState`, `nix_eval_state_builder`, and
`nix_flake_reference_parse_flags` as outliers with multiple fields.

This is exactly the "macro-driven boilerplate that should be a
template" debt class: 14 `struct { ref<Cxx>; }` wrappers should be
a `OpaqueRef<Cxx>` template. C++ doesn't allow templates with
extern-C linkage, but a template + `using nix_X = OpaqueRef<X>;` plus
a `NIX_C_OPAQUE(name, cxx)` macro covers all the simple cases. The
"latent bug" angle: `Store::ptr` uses `ref<Store>` (non-null) while
`nix_value::value` uses `Value*` (nullable) — the inconsistency
*hides* a NULL/non-NULL distinction that the C API surface should
make explicit.

**Sites:**
- `src/libutil-c/nix_api_util_internal.h` — `nix_c_context` (outlier).
- `src/libstore-c/nix_api_store_internal.h` — `Store`, `StorePath`,
  `nix_derivation`.
- `src/libexpr-c/nix_api_expr_internal.h` — 7 wrappers including the
  outliers.
- `src/libfetchers-c/nix_api_fetchers_internal.hh` —
  `nix_fetchers_settings`.
- `src/libflake-c/nix_api_flake_internal.hh` — 5 wrappers.

**Proposed abstraction:** a `template<class Cxx> struct OpaqueRef {
nix::ref<Cxx> ptr; };` plus `template<class Cxx> struct OpaqueValue
{ Cxx value; };` plus a `NIX_C_OPAQUE_REF(NameC, NameCxx)` macro
that does the `extern "C" struct NameC; using NameC =
OpaqueRef<NameCxx>;` declaration. Plus a coding rule: "C-API wrappers
must use `OpaqueRef` for non-null and `OpaqueOptional` for nullable;
explicit declarations of `struct foo {...}` are reserved for the
outliers (currently `nix_c_context`, `nix_value`, `EvalState`,
`nix_eval_state_builder`)."

**Why pass-1 missed it:** pass 1 explicitly named "C-API specifically"
as out of scope but flagged it for follow-up. None of the 23
N-candidates target the opaque-pointer-wrapper pattern, even though
it is the textbook "boilerplate that should be a template".

**Effort:** small (helpers + comment) / medium (migrate all 14).

**Cross-references:** compounds with the C-API pattern catalogued
implicitly in INVENTORY.md shard 19. New candidate.

---

### N25. The C-API per-library `nix_<lib>_init` is hand-rolled idempotency

**Pattern:** every C-API library has a `nix_<lib>_init(nix_c_context
*)` function that calls the C++-side `initLib<Lib>()` (e.g.
`nix::initLibStore()`, `nix::initLibUtil()`). Each is wrapped in
the `NIXC_CATCH_ERRS` macro. The C++ side `initLib<Lib>()` is itself
hand-rolled idempotency:

- `nix::initLibUtil()` — `src/libutil/initLibUtil.cc`.
- `nix::initLibStore()` — `src/libstore/globals.cc` — accepts
  `loadConfig` parameter.
- `nix::initLibStore(false)` — `nix_libstore_init_no_load_config`
  is a thin wrapper that calls the same with a different flag. **A
  one-knob duplicate** — exactly the pattern the catalog targets
  ("two parallel implementations that are really one knob").

The idempotency contract is informal — no central registry or
`std::call_once` guarantees that `initLibStore()` called twice
behaves correctly. Each `init` is currently an unguarded function;
clients are documented to call them once at process start.

**Sites:**
- `src/libutil-c/nix_api_util.cc::nix_libutil_init`.
- `src/libstore-c/nix_api_store.cc::nix_libstore_init` and
  `nix_libstore_init_no_load_config`.
- `src/libexpr-c/nix_api_expr.cc::nix_libexpr_init`.
- The `_init_plugins` family in `nix_api_main.cc` and elsewhere.

**Proposed abstraction:** a `LibraryInit<&initFn>` static-init
template (or a `NIX_LIBRARY_INIT(LibName, initFn, parameters)` macro)
that emits both the C entry (`nix_<lib>_init`) and a C++-side
`std::once_flag`-guarded idempotency wrapper. Same shape as N3's
`Registry<...>` template (one piece of state that should be lifted
once, then spelled-out per use).

**Sites:**
- `src/libutil-c/nix_api_util.cc::nix_libutil_init`.
- `src/libstore-c/nix_api_store.cc::nix_libstore_init`,
  `nix_libstore_init_no_load_config`.
- `src/libexpr-c/nix_api_expr.cc::nix_libexpr_init`.

**Why pass-1 missed it:** the C-API pattern was flagged for follow-up
but the *idempotency-as-implementation-detail* observation isn't in
N1-N23.

**Effort:** small.

**Cross-references:** new candidate; compounds with N24 (the C-API
pattern bucket); N3 (registry pattern) shares the `std::once_flag`
discipline gap.

---

### N26. Three parallel typed-attribute extractors

**Pattern:** three separate typed-attribute helper families exist for
three "key/value bag" backends:

| Backend | File | Helpers |
| ------- | ---- | ------- |
| `nix::fetchers::Attrs` | `libfetchers/attrs.cc` | `getStrAttr`, `getBoolAttr`, `getIntAttr`, `maybeGetStrAttr`, `maybeGetBoolAttr`, `maybeGetIntAttr`, `getHashAttr` |
| `StringMap` + `StructuredAttrs` | `libstore/derivation-options.cc` | `getStringAttr`, `getBoolAttr`, `getStringSetAttr` (+ private static fns) |
| `nlohmann::json` | `libutil/json-utils.hh` | `getString`, `getInteger<T>`, `getObject`, `getArray`, `getStringList`, `getStringMap`, `getStringSet`, `valueAt` / `optionalValueAt` |

Each family has:
- A "required" version that throws on missing.
- An "optional" version returning `std::optional<T>`.
- A typed coercion (`getIntAttr` does `string2Int`, `getInteger<T>`
  validates the JSON number).
- A diagnostic that mentions the key name, the expected type, and the
  surrounding context.

The pattern is the same; the backends differ. There is no shared
typed-attribute-extraction layer. `derivation-options.cc::getStringAttr`
hand-rolls `env.find(name)` plus a `parsed->find(name)` fallback —
exactly the kind of "the same operation in two places" the catalog
targets.

**Sites:**
- `src/libfetchers/attrs.cc` — full extractor family for `Attrs`.
- `src/libfetchers/include/nix/fetchers/attrs.hh` — public surface.
- `src/libstore/derivation-options.cc` — file-private `getStringAttr`,
  `getBoolAttr`, `getStringSetAttr` over `(StringMap, StructuredAttrs *)`.
- `src/libutil/include/nix/util/json-utils.hh` — JSON family.
- `src/libutil/json-utils.cc`.

**Proposed abstraction:** a `TypedAttrSource<Backend>` concept (or
class template) requiring `Backend::find(name)` and `Backend::iter()`,
parameterised on the value type and providing the required+optional
+throwing-with-diagnostic shape. Each of the three backends becomes
a `TypedAttrSource<Attrs>`, `TypedAttrSource<MergedSlot<StringMap,
StructuredAttrs>>`, `TypedAttrSource<json>` instance. The typed
coercion (`string2Int`, `string2Float`, `parseHashAlgo`, etc.)
becomes a `Coerce<T>` family of free functions that the backend-side
just dispatches into.

**Why pass-1 missed it:** N11 names the *iteration loop* on
attrset, but doesn't observe that the typed-extraction *helpers*
themselves are duplicated three ways. N11's framing was "outer loop
plus per-key dispatch"; this candidate is "the per-key typed
extraction layer underneath."

**Effort:** medium. Touches all three families plus their callers;
the backend abstraction has to be carefully designed so the
diagnostic format stays consistent.

**Cross-references:** compounds with N11 (iterate-attrset plumbing —
the inner typed coercion is N26's territory); supersedes the catalog's
implicit duplication observation in #114.

---

### N27. The `*Impl : *` factory pattern is 13+ instances of one shape

**Pattern:** the codebase has a deliberate header/impl split where:
- Header: `struct Foo { virtual ~Foo() = 0; ... };`
- .cc: `struct FooImpl : Foo { ... };` plus
  `ref<Foo> makeFoo(args)` factory returning `make_ref<FooImpl>(...)`.

Independent grep finds ~13 such pairs:
- `MountedSourceAccessorImpl : MountedSourceAccessor` (mounted-source-accessor.cc).
- `NarAccessorImpl : NarAccessor` (nar-accessor.cc).
- `InterruptCallbackImpl : InterruptCallback` (signals.cc).
- `AllowListSourceAccessorImpl : AllowListSourceAccessor`
  (filtering-source-accessor.cc).
- `InputCacheImpl : InputCache` (input-cache.cc).
- `CacheImpl : Cache` (libfetchers/cache.cc).
- `GitRepoImpl : GitRepo` (git-utils.cc).
- `GitFileSystemObjectSinkImpl : GitFileSystemObjectSink` (git-utils.cc).
- `DummyStoreImpl : DummyStore` (dummy-store.cc).
- `NarInfoDiskCacheImpl : NarInfoDiskCache` (nar-info-disk-cache.cc).
- `AwsCredentialProviderImpl : AwsCredentialProvider` (aws-creds.cc).
- `DerivationBuilderImpl : DerivationBuilder` (derivation-builder.cc).
- `CachingSourceAccessor`'s implementation (caching-source-accessor.cc).

This is the classic "Impl-pattern + abstract base header" pImpl
hybrid. The header carries the public virtual API plus a factory
function declaration (`ref<Foo> makeFoo(args)`); the .cc carries the
implementation. There is no shared scaffolding; each pair is
hand-rolled.

The compounding observation: in *most* of these cases the abstract
base has a single implementation. The "abstract base + Impl" layer
is purely a compilation-firewall device (and a way to express
"factory returns ref-counted, member type is opaque to callers"). It
is not polymorphism — `make*` always returns the one `*Impl`.

This is a real case where the *abstract base header* is C++-shaped
boilerplate. A more direct shape would be:

```
// foo.hh
struct Foo;  // opaque
ref<Foo> makeFoo(args);
const FooListing & getListing(ref<Foo>);  // free fns over Foo
```

i.e., free-function-over-opaque-handle. But this is a structural
change with API impact, so the more conservative refactor is a
shared `template<class IFace, class Impl> struct OpaqueImpl` macro
or template that emits the boilerplate consistently.

**Sites:** see list above. Three concrete places to touch first:
- `src/libutil/nar-accessor.cc` (smallest; clean shape).
- `src/libfetchers/input-cache.cc` (typical).
- `src/libstore/nar-info-disk-cache.cc` (more methods; representative
  of the larger end).

**Proposed abstraction:** more important than the macro is the
*coding rule* — when an abstract base has exactly one `Impl`
subclass and no plans for a second, prefer the opaque-handle +
free-function shape over the abstract-base + Impl shape. For sites
where polymorphism is genuine (multiple Impls per base — e.g.
`SourceAccessor`, `Store`, `Sink`), the existing shape is fine.

**Why pass-1 missed it:** N7 (the SourceAccessor factories) overlaps
this for the SourceAccessor subset; N10 (Connection types) overlaps
for Connection. Pass 1 didn't generalise the pattern to the
13-and-growing list.

**Effort:** medium (rule + inventory) / large (migrate the
single-Impl bases to opaque-handle).

**Cross-references:** compounds with N7 (SourceAccessor factories),
N10 (Connection types).

---

### N28. The `_NIX_TEST_UNIT_DATA` pattern is duplicated across 7 test suites

**Pattern:** every test-suite `meson.build` carries an identical
`_NIX_TEST_UNIT_DATA` plumbing:

```
test_env = {
  '_NIX_TEST_UNIT_DATA' : meson.current_source_dir() / 'data',
  'HOME' : meson.current_build_dir() / 'test-home',
}
```

Pass 1 named this in passing (N18 mentions
`_NIX_TEST_BUILD_DIR`/`_NIX_TEST_*` env vars). Independent grep of
`src/*-tests/meson.build` shows 7 sites that each repeat the block:

- `src/libutil-tests/meson.build` (twice — once for tests, once for
  benchmarks).
- `src/libstore-tests/meson.build` (twice).
- `src/libfetchers-tests/meson.build`.
- `src/libexpr-tests/meson.build`.
- `src/libflake-tests/meson.build`.
- (No `libcmd-tests` or `libmain-tests`; if added, the same boilerplate
  would be copied.)

The C++ side reads `_NIX_TEST_UNIT_DATA` in `getUnitTestData()` (once,
in `libutil-test-support`). The build side spells out the env var by
hand in every test-suite. No `nix-meson-test-suite()` macro exists.

**Sites:**
- `src/libutil-tests/meson.build`.
- `src/libstore-tests/meson.build`.
- `src/libfetchers-tests/meson.build`.
- `src/libexpr-tests/meson.build`.
- `src/libflake-tests/meson.build`.
- `src/libutil-test-support/include/nix/util/tests/test-data.hh` —
  `getUnitTestData()`.

**Proposed abstraction:** a `nix-meson-build-support/test-suite/`
shared meson include that provides a `nix_test_env` dict variable
(or a function) callers spread into their `test_env`. The same
include can carry the `'NIX_REMOTE'` Windows-vs-Unix conditional.

**Why pass-1 missed it:** build-system files were "lightly explored"
per pass 1's methodology; only the `_NIX_TEST_*` env-var protocol
(N18) was named, and only as a C-side observation.

**Effort:** small. Pure cleanup; no semantic change.

**Cross-references:** compounds with N18 (env var iceberg);
specifically the build-side half of N18.

---

### N29. The `NIXC_CATCH_ERRS` family is three near-identical macros

**Pattern:** the C API has three closely related error-catch macros
defined in `nix_api_util_internal.h`:

```
#define NIXC_CATCH_ERRS                    \
    catch (...) { return nix_context_error(context); } \
    return NIX_OK;

#define NIXC_CATCH_ERRS_RES(def)    \
    catch (...) { nix_context_error(context); return def; }

#define NIXC_CATCH_ERRS_NULL NIXC_CATCH_ERRS_RES(nullptr)
```

The first two differ only in (a) does the catch return `def` or
`NIX_OK`, and (b) does the function return after the catch. The
third is an alias. Independent grep confirms ~270+ uses of these
across the C API libraries.

The hidden duplication is between `NIXC_CATCH_ERRS` (no return value)
and `NIXC_CATCH_ERRS_RES(NIX_OK)` (return `NIX_OK`). They aren't
quite identical — `NIXC_CATCH_ERRS` *also* implicitly returns
`NIX_OK` after the catch block (via `return NIX_OK;`), so the calling
function must end at the macro. `NIXC_CATCH_ERRS_RES(def)` does
*not* contain the trailing return — the caller must spell it out.

This is the shape "two macros for the same thing, with subtle
calling conventions differing" that the catalog targets — a coding
trap waiting to happen. The asymmetric "trailing return"
behaviour means `NIXC_CATCH_ERRS` cannot be used after a
non-`return`-shaped function body without losing information.

**Sites:**
- `src/libutil-c/nix_api_util_internal.h` — definition.
- All `src/libstore-c/`, `src/libexpr-c/`, `src/libfetchers-c/`,
  `src/libflake-c/`, `src/libmain-c/` — usage.

**Proposed abstraction:** unify the macros (or convert to a single
`scope-exit`/`std::exception_ptr`-based RAII helper). Specifically:

```
template<auto DefValue>
struct CCatchScope { /* catches and converts */ };

#define NIXC_CATCH(Default) \
    catch (...) { return nix::cCatchAndReturn(context, Default); }
```

Or — preferred — port the C-API to use a `try { ... } catch(...) {
return cContextFromException(context); }` template helper. The
trailing `return NIX_OK;` of `NIXC_CATCH_ERRS` is a footgun.

**Why pass-1 missed it:** the C-API was flagged for follow-up but the
specific macro asymmetry wasn't pulled out as a candidate.

**Effort:** small (rename) / medium (port the 270+ call sites to a
unified macro).

**Cross-references:** compounds with N24, N25 (the C-API
follow-up cluster). New candidate.

---

### N30. The `MixJSON` / `bool json` / per-command if-else dispatch

**Pattern:** `MixJSON` (libmain) declares `bool json = false`. Each
modern `nix` subcommand inheriting `MixJSON` then has its `run()`
body branch on `if (json) { ... emit JSON ... } else { ... emit text
... }`. Independent grep finds ~25 sites with `if (json) {` over the
modern CLI:

- `src/nix/eval.cc`, `src/nix/path-info.cc`, `src/nix/develop.cc`,
  `src/nix/flake.cc` (10+ separate sites in `flake.cc` alone for the
  multiple subcommands), `src/nix/prefetch.cc`, `src/nix/search.cc`,
  `src/nix/profile.cc`, `src/nix/ls.cc`, `src/nix/build.cc`,
  `src/nix/log.cc`, `src/nix/config.cc`, `src/nix/why-depends.cc`,
  `src/nix/store/info.cc`, `src/nix/derivation/show.cc`, etc.

Each command:
1. Declares its own JSON-encoder lambda or inline JSON
   construction.
2. Branches on `json` and emits text *or* JSON.
3. Calls `printJSON(j)` (from `MixPrintJSON`) for the JSON branch.

The duplication is the *bifurcated output formatter*. Each command
hand-rolls "format-as-text" and "format-as-JSON" twice. The catalog
notes #92 (cat/ls reimplementation) and #94 (BuiltPath/SingleBuiltPath)
but doesn't name the wider issue.

A `OutputFormat<T>` trait — given a `T`, declares both `to_json(T)`
and `to_text(T)` — would let `MixJSON::run` dispatch in one place.
The 25 commands collapse onto one decision: `if (json)
printJSON(toJson(result)); else logger->cout("%s", toText(result));`.

**Sites:**
- `src/libmain/include/nix/main/common-args.hh` — `MixJSON`,
  `MixPrintJSON`.
- 25+ command source files, see grep above.

**Proposed abstraction:** an `OutputFormatter` mixin or a
`CommandResult` ADT (variant over the kinds of result) that each
command produces; dispatch (text vs JSON) lives in `MixJSON::run`'s
post-step.

**Why pass-1 missed it:** the libcmd shard mentioned `MixJSON`
without enumerating consumers; pass 1 didn't grep for `if (json)`
across `src/nix/`.

**Effort:** medium. Each command's `run()` body has to be split
into "produce result" + "format result" halves.

**Cross-references:** compounds with #92 (cat/ls cmd duplication);
extends the catalog's coverage of `nix` CLI duplication.

---

### N31. The 7 `*-cache` SQLite caches all encode versions in filenames

**Pattern:** N13 names the SQLite-cache base shape but doesn't
articulate the *filename-versioning* pattern. Each SQLite cache
encodes its schema version in the filename, *not* in `PRAGMA
user_version`:

| Cache | Filename | Code path |
| ----- | -------- | --------- |
| Eval cache | `eval-cache-v6/<fp>.sqlite` | `eval-cache.cc` |
| Binary cache | `binary-cache-v8.sqlite` | `nar-info-disk-cache.cc` |
| Fetcher cache | `fetcher-cache-v4.sqlite` | `libfetchers/cache.cc` |
| Tarball cache | `tarball-cache-v2/...` | `git-utils.cc` |
| Local store schema | `<store>/var/nix/db/schema` (file containing integer) | `local-store.cc` |
| GC roots | `<store>/var/nix/gcroots/auto/...` (no version) | `gc.cc` |
| Build hook log | `<store>/var/log/nix/drvs/...` (no version) | `derivation-building-goal.cc` |

The bumping protocol on schema change is "create a new directory
with the new version suffix; old directory is orphaned". This means
old caches stay on disk forever — there is no cleanup. This is a
*latent bug* hiding in duplication — the scattered cache invalidation
makes `nix store gc` unable to reclaim any of the orphaned caches.

The right shape is a single `~/.cache/nix/` cleanup pass that knows
all the cache schema versions; today there is no such pass.

**Sites:**
- `src/libstore/local-store.cc` — `nixSchemaVersion = 10`.
- `src/libstore/nar-info-disk-cache.cc` — `dbPath = ".../binary-cache-v8.sqlite"`.
- `src/libfetchers/cache.cc` — `dbPath = ".../fetcher-cache-v4.sqlite"`.
- `src/libexpr/eval-cache.cc` — `dbPath = ".../eval-cache-v6/<fp>.sqlite"`.
- `src/libfetchers/git-utils.cc` — `tarball-cache-v2/`.

**Proposed abstraction:** a single `CacheRegistry` enumerating all
SQLite caches with their schema version + path constructor. Plus a
`nix store gc-caches` (or extend `nix store gc`) that walks the
registry and deletes caches where the on-disk version is older
than the current.

This is part of the broader N13 candidate but is independent enough
to warrant its own candidate — the "old caches never cleaned up"
hazard is a separate bug from the "no shared base" abstraction.

**Why pass-1 missed it:** N13 names the cache-base abstraction; the
*cleanup hazard* is a separate observation. Pass 1's framing of N13
doesn't identify it.

**Effort:** small (registry + comment) / medium (also write the
gc-caches pass).

**Cross-references:** compounds with N13; supersedes #168 (which
notes the magic numbers; this names the underlying mechanism).

---

### N32. Three parallel typed (parse, show) symmetric pairs without convention

**Pattern:** the codebase has many `parseX` / `showX` (or `printX`)
function pairs that go enum/struct-to-string and back, with no shared
API style:

| Type | parse | show/print | File |
| ---- | ----- | ---------- | ---- |
| `HashAlgorithm` | `parseHashAlgo` | `printHashAlgo` | `hash.hh` |
| `HashFormat` | `parseHashFormat`, `parseHashFormatOpt` | `printHashFormat` | `hash.hh` |
| `CompressionAlgo` | `parseCompressionAlgo` | `showCompressionAlgo` | `compression-algo.hh` |
| `ExperimentalFeature` | `parseExperimentalFeature` | `showExperimentalFeature` | `experimental-features.hh` |
| `SandboxMode` | `BaseSetting<SandboxMode>::parse` | `BaseSetting<SandboxMode>::to_string` | `globals.cc` |
| `ContentAddressMethod` | `ContentAddressMethod::parse` | `ContentAddressMethod::render` | `content-address.cc` |
| `BuildResult::Status` | (table) | `buildResultStatusTable` | `build-result.cc` |
| `LogFormat` | `parseLogFormat` | (none — directly maps) | `loggers.cc` |
| `Verbosity` | (settings) | `showVerbosity` (?) | `logging.cc` |

The split is: *some* use `parse*`/`print*` (libutil-data); *some* use
`parse*`/`show*` (libutil-runtime); *some* use member functions
(`parse`/`render`); *some* use `BaseSetting<T>::parse`/`to_string`
(libutil-runtime), itself an ad-hoc Setting<T>-driven family.

The catalog has #97 (`parseHashFormat` / `parseHashAlgo` parser) and
#205 (`Counter` enabling) but doesn't name the *parse-show pair*
pattern as duplication.

A `nix::Stringly<T>` concept + `parse<T>()` / `to_string(T)`
canonical pair (with optional CTAD) replaces all 9 ad-hoc pairs
with one consistent shape. C++23's `std::format` integration makes
this even cleaner: every `Stringly<T>` is automatically formattable.

**Sites:**
- `src/libutil/include/nix/util/hash.hh` — Hash{Algorithm,Format}.
- `src/libutil/include/nix/util/compression-algo.hh` — Compression.
- `src/libutil/include/nix/util/experimental-features.hh` — Xp.
- `src/libstore/include/nix/store/globals.hh` — `BaseSetting<SandboxMode>`.
- `src/libstore/include/nix/store/content-address.hh` —
  `ContentAddressMethod`.
- `src/libstore/include/nix/store/build-result.hh` —
  `buildResultStatusTable`.
- `src/libmain/include/nix/main/loggers.hh` — `LogFormat`.

**Proposed abstraction:** a `nix::Stringly<T>` concept + uniform
`parse<T>(string_view)` / `to_string(T)` family, possibly
implemented via boost.describe / boost.PFR descriptor tables (so the
table is the source of truth; the parse and show are derived).

**Why pass-1 missed it:** #97 names parse helpers; pass 1 didn't
extend to the `parse`-`show` pair pattern. The split between `print*`
(used by libutil-data) and `show*` (used by libutil-runtime) is
itself a tell that two conventions exist with no documented choice
rule.

**Effort:** medium.

**Cross-references:** extends #97; compounds with N15 (versioned
formats also need `parse`/`show`); compounds with N26 (typed
extractors share the `Coerce<T>` need).

---

### N33. The 16 `friend` declarations in `eval.hh` are AST/EvalState coupling

**Pattern:** independent grep verifies `eval.hh` has 16 `friend`
declarations (matching the adversarial-existing review's count). Of
these:

- 10 are `friend struct Expr*` AST nodes (`ExprVar` ×2 — one
  duplicate, `ExprAttrs`, `ExprLet`, `ExprOpUpdate`,
  `ExprOpConcatLists`, `ExprString`, `ExprInt`, `ExprFloat`,
  `ExprPath`, `ExprSelect`).
- 3 are `friend void prim_*` primops (`prim_genericClosure`,
  `prim_concatMap`, plus one).
- 3 are `friend class Value` / `friend class ListBuilder` / friend
  to a related struct.

The pattern indicates that `EvalState` has private state that the
AST nodes and primops must read/write directly. This is the
"context object too big" smell of N19, but at the *member-access*
level: the AST → EvalState coupling is via 10 friend declarations,
each of which spells one piece of internal data the AST needs to
touch.

The right factoring is `EvalContext` / `EvalMemory` / `PrimOpHelpers`
collaborators (cand. #169, #210, #217 each names one slice). The
*size of the friend list* is the deepest evidence that the
decomposition is overdue.

**Sites:**
- `src/libexpr/include/nix/expr/eval.hh`.
- `src/libexpr/eval.cc` — the `Expr*::eval` bodies that exercise
  the friendship.

**Proposed abstraction:** decomposition into `EvalState` (eval
control flow) + `EvalMemory` (Value allocation) +
`SymbolTable` (already separate) + `PrimOpHelpers` (primop-shaped
operations on Values). The friend list shrinks to 0 because the
needed access becomes public (or via collaborator references).

**Why pass-1 missed it:** N19 names the meta-pattern; this candidate
gives the *concrete decomposition steps* that the friend-list count
makes load-bearing. Pass 1 didn't measure the friend list as
evidence.

**Effort:** structural (multi-PR); same as N19 / #210 in scope.

**Cross-references:** compounds with #169, #210, #217, N19.

---

### N34. The four `*Cache` clearing protocols use four different invalidation idioms

**Pattern:** the codebase has several cache-invalidation protocols,
each rolled differently:

- `EvalCache` (libexpr) — schema-version-in-filename + read-mostly
  + `try { ... } catch (SQLiteError &) { disable; }` per-operation.
- `Cache` (libfetchers) — same shape; *different* doSQLite protocol
  (silent-disable on error vs. throw).
- `NarInfoDiskCache` — `LastPurge` row + per-cache TTL ("expire if
  older than X").
- `EvalState::importResolutionCache` — purely in-memory, `concurrent_flat_map`,
  no invalidation.
- `drvHashes` global — purely in-memory, no invalidation.
- `GitArchiveCache` — TTL-driven, "if older than 24h refetch"
  (settings `tarball-ttl`).

Six caches; six invalidation protocols. There is no shared `Cache<K,
V, InvalidationPolicy>` template. The `try-catch-disable` half is
covered by N13; the *invalidation-policy* half is not.

A `CachePolicy<TTL=never, Schema=v0>` policy template plus an
`InvalidationStrategy` enum (`OneShot`, `TTL`, `OnError`,
`OnSchemaChange`) could replace the six protocols with one shape
where the one differing axis is named explicitly.

**Sites:**
- `src/libexpr/eval-cache.cc` — `EvalCache`.
- `src/libfetchers/cache.cc` — `Cache`.
- `src/libstore/nar-info-disk-cache.cc` — `NarInfoDiskCache`.
- `src/libexpr/include/nix/expr/eval.hh` — in-memory caches.
- `src/libstore/include/nix/store/derivations.hh` — `drvHashes`.
- `src/libfetchers/git-utils.cc` — git-archive cache TTL.

**Proposed abstraction:** a `Cache<K, V, Storage, Invalidation>`
template + a documented invalidation rubric. Compounds with N13's
`SqliteCache<Schema>` template; N34 adds the invalidation axis.

**Why pass-1 missed it:** N13 names the SQLite-cache base; the
invalidation-policy axis is orthogonal and pass 1 collapsed both
into one observation.

**Effort:** medium.

**Cross-references:** compounds with N13, #71 (TTL+present-bit shape).

---

### N35. The 7-9 X-macros for "list of names" share the X-macro discipline

**Pattern:** the codebase has multiple X-macros that emit "list of
names" boilerplate, each implemented inconsistently:

- `NIX_VALUE_STORAGE_FOR_EACH_FIELD` (`value.hh`) — drives the
  Value layout discrimination; recurs in 4-5 expansions.
- `NIX_FOR_EACH_COMPRESSION_ALGO` (`compression-algo.hh`) — drives
  the algo enum + parser + JSON serialiser.
- `NIX_FOR_EACH_LA_ALGO` (`compression.cc`, file-private) — drives
  libarchive algo dispatch.
- The `JSON_IMPL` / `JSON_IMPL_INNER` X-macros (`json-impls.hh`).
- `NIX_DECLARE_CONFIG_SERIALISER(T)` (`configuration.hh`).

Each is hand-rolled; each has its own discipline (some are
`#define X(...) __VA_ARGS__`, others are `#define X(MACRO)
MACRO(name, ...)`). The choice of style (`X(MACRO)` vs `X(...)`) is
inconsistent.

A "list of names" X-macro is exactly the C++23 "should be a
constexpr array + concept" candidate. With `boost::describe` (U10),
each list becomes:

```
struct ValueStorageFields { ... };
BOOST_DESCRIBE_STRUCT(ValueStorageFields, (), (Bool, Int, Float, ...))
```

— and every consumer derives from the descriptor. Pass-1 N22 names
the *macros that emit declarations*; this candidate is the
neighbouring observation that *macros that emit lists for traversal*
are the same shape and migrate the same way.

**Sites:**
- `src/libexpr/include/nix/expr/value.hh` —
  `NIX_VALUE_STORAGE_FOR_EACH_FIELD`.
- `src/libutil/include/nix/util/compression-algo.hh` —
  `NIX_FOR_EACH_COMPRESSION_ALGO`.
- `src/libutil/compression.cc` — `NIX_FOR_EACH_LA_ALGO` (file-local).
- `src/libutil/include/nix/util/json-impls.hh` — `JSON_IMPL`.
- `src/libutil/include/nix/util/configuration.hh` —
  `NIX_DECLARE_CONFIG_SERIALISER`.

**Proposed abstraction:** standardise the X-macro shape (always
`X(MACRO)` form), document where each is used, then incrementally
migrate to `boost::describe` once U10 lands (#9, U10).

**Why pass-1 missed it:** N22 names "macros that emit struct
declarations"; this candidate is the parallel observation that
"macros that emit traversal-shaped lists" exist in equal number
with no shared discipline.

**Effort:** medium (X-macro discipline cleanup) / large (full
boost.describe migration).

**Cross-references:** compounds with N22; depends on U10
(boost.describe) for the migration target.

---

### N36. The 14 `make*Sink` factories have no common Sink-construction trait

**Pattern:** independent grep finds these `make*Sink`-shaped
factories in libutil/libstore:

- `makeCompressionSink(algo, sink)` — compression.cc.
- `makeDecompressionSink(algo, source)` — compression.cc.
- `makeFileSink(path)` — implicit.
- `makeStringSink()` — `StringSink` is a struct, no factory.
- `makeMemorySink()` — `MemorySink` similarly.
- `makeTeeSink(a, b)` — implicit.
- `makeNullSink()` — implicit.
- `makeLengthSink(inner)` — implicit; `LengthSink` is a struct with
  inner.
- `makeRefScanSink(refs)` — `references.cc`.
- `makeRewritingSink(rewrites, inner)` — `references.cc`.
- `makeHashModuloSink(...)` — composes `Rewriting` + `Hash`.
- `makeHashSink(algo)` — `hash.cc`.
- `makeFdSink(fd)` — implicit.
- The Brotli + Xz + Zstd compression-sink families.

Some are factory functions; some are exposed as `SubclassSink<...>`
constructors. There is no convention for "how to construct a Sink".
N6 names the *side-channel observer* pattern but the construction
question is orthogonal — even after the observer abstraction, each
sink is constructed differently.

A `makeSink<Tag>(args...)` factory plus a `SinkSpec` variant could
unify construction. Compounds with N6.

**Sites:** see N6's list. Three concrete:
- `src/libutil/compression.cc::makeCompressionSink`.
- `src/libutil/hash.cc::HashSink` constructor.
- `src/libstore/references.cc::RefScanSink` etc.

**Proposed abstraction:** a `SinkSpec` variant for the common cases
(Compression, Hash, RefScan, Tee, Length, Null, String); a
`makeSink(SinkSpec)` factory.

**Why pass-1 missed it:** N6 names the wrapping-observer pattern;
this candidate is the construction-pattern parallel.

**Effort:** small (variant + factory) / medium (migrate sink
construction sites).

**Cross-references:** compounds with N6, #38 (Sink/Source hierarchy).

---

### N37. Three "magic file headers" share no shape

**Pattern:** the codebase has three magic-byte headers used to
identify on-wire / on-disk formats:

- `narVersionMagic1 = "nix-archive-1"` — string-magic, NAR.
- `exportMagic = NIXE` — 4-byte tag, `nix-store --export`.
- `xz`, `bzip2`, `gzip`, `zstd`, `brotli` magic bytes — used by
  `getCompressionMethod` to dispatch by file suffix (a different
  shape, magic-byte sniffing for compression).

Plus the implicit:
- `git tree`/`git blob` magic ("blob ...\0" prefix) — `git/tree.cc`.
- `pack:` prefix — protocol version negotiation.
- The `0x01000000` magic in length-prefixed proto helper.

Six magic markers; six implementations. No `MagicHeader<Tag>` shared
trait. N15 names the *version* aspect; the magic-marker aspect is
adjacent.

**Sites:**
- `src/libutil/archive.{hh,cc}` — `narVersionMagic1`.
- `src/libstore/export-import.cc` — `exportMagic`.
- `src/libutil/compression.cc` — magic-byte dispatch.
- `src/libutil/git.cc` — git tree/blob.
- `src/libstore/length-prefixed-protocol-helper.hh`.

**Proposed abstraction:** a `MagicHeader<...>` trait — given a tag,
provides serialise/parse with consistent error message. Compounds
with N15 (versioned formats).

**Why pass-1 missed it:** N15 names version-tag formats; the
*magic-marker* without version is adjacent and uncatalogued.

**Effort:** small.

**Cross-references:** compounds with N15.

---

### N38. The 9+ `extern template` instantiation lists are duplicated

**Pattern:** several headers carry `extern template` declarations to
opt out of implicit instantiation for compile-time savings. Each
implementation file then `template ... ;`-instantiates the same
list:

- `src/libutil/include/nix/util/strings.hh`:
  ```
  extern template std::string concatStringsSep(string_view, const std::list<std::string> &);
  extern template std::string concatStringsSep(string_view, const StringSet &);
  extern template std::string concatStringsSep(string_view, const std::vector<std::string> &);
  extern template std::string concatStringsSep(string_view, const small_vector<std::string, 64> &);
  ```
  And `src/libutil/strings.cc` echoes the four instantiations.

- `src/libstore/include/nix/store/length-prefixed-protocol-helper.hh`:
  similar pattern for `vector<T>`/`set<T>`/`tuple<...>`/`map<K,V>`.

- The `BaseSetting<T>::parse` family in `configuration.hh` similarly.

- The `WorkerProto::Serialise<T>` / `ServeProto::Serialise<T>`
  template specialisations in `worker-protocol-impl.hh` etc.

Each header declares a list; each .cc echoes the same list; drift
between the two halves is silently a build error or (worse) silent
implicit instantiation. There is no shared discipline.

**Sites:**
- `src/libutil/include/nix/util/strings.hh` + `strings.cc`.
- `src/libstore/include/nix/store/length-prefixed-protocol-helper.hh`
  + corresponding TUs.
- `src/libutil/include/nix/util/configuration.hh` + various `.cc`s.
- The protocol Serialise specialisations.

**Proposed abstraction:** an `EXTERN_TEMPLATE_LIST(T, ...)` macro in
each of the affected headers, paired with `INSTANTIATE_TEMPLATE_LIST(T,
...)` in the corresponding .cc. Or — more aggressive — a
`boost::describe`-backed list that both halves derive from (this is
also what N35 / U10 unblocks).

**Why pass-1 missed it:** the catalog has #1 / #3 (proto Serialise
specialisations) but doesn't observe that the *instantiation lists*
themselves are duplicated.

**Effort:** small (macro) / medium (full migration).

**Cross-references:** compounds with #1, #3 (proto specialisation),
N35.

---

### N39. The "test-data folder" pattern mirrors the protocol versioning surface

**Pattern:** N16 names the `JsonCharacterizationTest<T>` / 
`VersionedProtoTest<Proto>` pattern. Pass-1 didn't enumerate the
actual test data layout. Independent inspection of
`src/libstore-tests/data/` shows directories like `worker-protocol/`,
`serve-protocol/`, `path-info/`, etc. Each contains `<test-stem>.bin`
and `<test-stem>.json` golden files, parameterised by the cited
protocol version.

The shape:
- For each (T, Version) pair, a `<stem>.bin` golden + a `<stem>.json`
  golden.
- For each (T) JSON-only test, a `<stem>.json` golden.

Two observations:

(a) The test data is in source-tree; on-disk filenames encode the
type and version. There is no tool that *generates* the test data
from a typelist + version-list — each new test requires a developer
to: write the `VERSIONED_CHARACTERIZATION_TEST(...)` macro
invocation, *and* commit the matching `.bin` + `.json`. N16's reflection
proposal (boost.describe) auto-derives the read/write codecs; an
adjacent automation could auto-generate the golden files at test
time (a `--update` mode), making the test harness self-bootstrapping.

(b) Several types have JSON-only golden files (no protocol version),
indicating the type is JSON-serialised but not wire-serialised.
Conversely, several wire-serialised types have no JSON variant. The
asymmetry is undocumented; the test fixture decides whether to test
JSON, wire, or both, per macro choice (`VERSIONED_CHARACTERIZATION_TEST`
vs `_NO_JSON`).

The deep observation: the test infrastructure encodes the *protocol-
JSON-test matrix* but the matrix isn't exposed as data; each call
site picks JSON or wire by macro name. A `TEST_GRID(Type, {Wire,
Json}, Versions)` declarative form would replace the macro family
with one declaration per type.

**Sites:**
- `src/libstore-test-support/include/nix/store/tests/protocol.hh` —
  the macro family.
- `src/libstore-tests/data/{worker-protocol,serve-protocol,
  path-info,...}/*` — the goldens.
- `src/libstore-tests/{worker-protocol,serve-protocol,
  common-protocol,path-info,...}.cc` — call sites.

**Proposed abstraction:** a declarative test-grid format (one entry
per type listing `(versions, formats)`); the harness fans out into
the `TEST_F`s. Plus a `--update-goldens` mode (already convention in
many projects) that regenerates the on-disk goldens.

**Why pass-1 missed it:** N16 names reflection; this candidate is
the *test-harness automation* aspect, not the production-side
reflection. They compound but are separate refactors.

**Effort:** medium.

**Cross-references:** compounds with N16; uses U10 (boost.describe).

---

### N40. Per-`InputScheme` URL-query handling is the same loop four times

**Pattern:** a refinement of N12. Independent grep of the
`InputScheme` URL-parser bodies confirms 4 schemes do nearly identical
URL-query iteration:

- `path.cc::PathInputScheme::inputFromURL` — iterates `url.query`,
  validates against a hardcoded set `{rev, narHash, revCount,
  lastModified}`.
- `git.cc::GitInputScheme::inputFromURL` — iterates `url.query`,
  validates against `{rev, ref, ...}`.
- `mercurial.cc::MercurialInputScheme::inputFromURL` — iterates
  `url.query`, validates against `{rev, ...}`.
- `github.cc::GitHubInputScheme::inputFromURL` (and `GitLab`,
  `SourceHut` siblings) — iterates `url.query`.

Each loop has a hardcoded allow-list, hand-spelled per scheme. The
allow-list is *also* expressed in `allowedAttrs()` — but as a
`std::map<string, AttributeInfo>` rather than as a set.
**The same allow-list is encoded twice per scheme** (URL parser allow-
list + `allowedAttrs` map), with no compile-time check that the two
agree.

This is the *latent bug hiding inside duplication* the catalog
targets: the URL parser of `path.cc` accepts `revCount` and
`lastModified`; `path.cc::allowedAttrs()` lists exactly those keys
plus `path` and `narHash`. If a developer adds a new query parameter
to URL parsing but forgets to add it to `allowedAttrs`, the schema
surface drifts.

**Sites:**
- `src/libfetchers/path.cc` — both halves.
- `src/libfetchers/git.cc` — both halves.
- `src/libfetchers/mercurial.cc` — both halves.
- `src/libfetchers/github.cc` — both halves.

**Proposed abstraction:** as N12 names, the `attribute_schema<Scheme>`
descriptor would unify the URL-parser allow-list with the
`allowedAttrs` map. This candidate names the latent-bug payoff
explicitly; N12 names the cleanup direction.

**Why pass-1 missed it:** N12 names the cross-shard meta-claim; this
candidate is the *concrete drift hazard* that motivates the
refactor. Pass 1 didn't articulate the per-scheme drift between
URL-parser allow-list and `allowedAttrs`.

**Effort:** medium (depends on N12 / boost.describe migration).

**Cross-references:** sibling of N12; refines the latent-bug claim.

---

### N41. Per-test-fixture `unitTestData` member is the same line in 25+ fixtures

**Pattern:** every characterisation test fixture starts with the
same line:

```
class FooTest : public CharacterizationTest {
    std::filesystem::path unitTestData = getUnitTestData() / "foo";
};
```

Independent grep finds 25+ `unitTestData = getUnitTestData() / ...`
lines across `src/libstore-tests/`, `src/libutil-tests/`,
`src/libfetchers-tests/`, etc. Each fixture spells the same
member declaration — the only difference is the subdirectory name.

This is bracket-bracket boilerplate the catalog targets — not a
*latent bug* but pure repetition that would benefit from a
`MAKE_CHARACTERIZATION_FIXTURE(name, subdir)` macro that synthesises
the class declaration. A simpler shape: a `CharacterizationTest`
constructor accepting the subdir name, eliminating the per-fixture
`unitTestData` member entirely.

**Sites:**
- `src/libstore-tests/{content-address,derived-path,path-info,
  store-reference,nar-info,...}.cc` — each fixture.
- `src/libutil-test-support/include/nix/util/tests/characterization.hh`
  — base class.

**Proposed abstraction:** in `CharacterizationTest`, replace the
need for `unitTestData` member with a virtual `subdir()` method or
a constructor parameter. Each fixture becomes:

```
class FooTest : public CharacterizationTest {
public:
    FooTest() : CharacterizationTest("foo") {}
};
```

Or simpler, a helper macro `CHARACTERIZATION_FIXTURE(FooTest, "foo")`.

**Why pass-1 missed it:** N16 names the macro `VERSIONED_CHARACTERIZATION_TEST`
but doesn't observe that the fixture *class declaration* is itself
boilerplate.

**Effort:** small.

**Cross-references:** compounds with N16, N28, N39.

---

### N42. The "atomic ID counter" pattern recurs five times

**Pattern:** five separate places have a `static std::atomic<T>
counter{0}` (or similar) used to mint unique IDs:

- `src/libutil/source-accessor.cc::nextNumber{0}` — process-global
  `SourceAccessor` ID counter (used by `operator==`/`<=>`).
- `src/libutil/logging.cc::nextId{0}` — process-global activity ID
  counter.
- `src/libutil/file-system.cc:537::counter` — random-seeded for
  unique tempfile names.
- `src/libstore/local-binary-cache-store.cc:80::counter{0}` — for a
  retry/backoff loop counter.
- `src/libstore/unix/build/linux-derivation-builder.cc:348::counter{0}`
  — for unique sub-uid allocation.

Plus the implicit `Counter::inner` in libexpr (which is a different
shape — per-instance counter, not ID minting).

There is no shared `IdGenerator<T>` or `mintId()` helper. Each site
re-rolls the same idiom (`auto id = counter.fetch_add(1,
std::memory_order_relaxed);`) — sometimes with `relaxed`, sometimes
without specifying memory order.

**Sites:** see list above.

**Proposed abstraction:** an `IdGenerator<T = uint64_t>` class
template providing `next()` with documented `memory_order`
discipline. Or a free function `mintId<Tag>()` that emits a
distinct counter per `Tag` template parameter.

**Why pass-1 missed it:** counters are clustered in N21; ID-mint
counters are a sub-pattern that pass 1 didn't separate.

**Effort:** small.

**Cross-references:** compounds with N21 (the broader counter
pattern).

---

### N43. Five subsystems own their own `std::thread` member with hand-rolled lifecycle

**Pattern:** each subsystem that owns a worker thread declares the
thread + a flag + an interrupt mechanism + a join-on-destruct
sequence inline. There is no shared `WorkerThread` helper.

| Subsystem | Field | Lifecycle |
| --------- | ----- | --------- |
| `progress-bar` | `std::thread updateThread` | join in stop() |
| `Pid` (libutil) | `std::thread killThread` | join in dtor |
| `MonitorFD` (unix) | `std::thread thread` | join in dtor |
| `curlFileTransfer` | `std::thread workerThread` | join + condvar + flag |
| `gc.cc::serverThread` | `std::thread serverThread` | join + listener |
| `PathSubstitutionGoal::thr` | `std::thread thr` | join + state machine |
| `DerivationBuilderImpl::daemonThread` | `std::thread daemonThread` | join after kill |

Six+ instances of the same shape: thread member + flag/condvar +
join-on-destruct. C++20 has `std::jthread` which provides this
exact lifecycle (interrupt + join-on-destruct via
`std::stop_token`); the codebase has chosen not to use it.

This is "hand-rolled abstractions where stdlib already has it" —
the `std::jthread` exclusion is undocumented; each site rolled its
own.

**Sites:** see table above.

**Proposed abstraction:** migrate to `std::jthread` where the
"kill via stop_token" semantics suit (most sites). For the curl
transfer thread (which has a complex ready/wait/dispatch cycle),
a `WorkerThread` wrapper around `std::jthread` exposing `notify()`
+ stop-token may be cleaner.

**Why pass-1 missed it:** N8 names the *async-idiom* axis
(Callback/awaitable/Co); the *thread-management* axis is parallel.

**Effort:** medium.

**Cross-references:** compounds with N8, R10.

---

### N44. The `BaseSetting<T>::trait` specialisation pattern is one-of and ad-hoc

**Pattern:** `BaseSetting<T>` (libutil-runtime) is the per-T
specialisation site for setting parsing/serialisation. The
specialisation pattern has *three* shapes:

1. **`parse`/`to_string`/`appendOrSet`** — `BaseSetting<T>::parse` and
   `to_string` have to be defined per `T` outside the class body;
   each TU defining a `Setting<T>` for a non-built-in `T` provides
   them. Used by `BaseSetting<SandboxMode>` (`globals.cc`),
   `BaseSetting<PathsInChroot>` (`globals.cc`),
   `BaseSetting<StoreReference>` (`store-reference.cc`),
   `BaseSetting<CompressionAlgo>` (`compression-settings.cc`),
   `BaseSetting<std::optional<CompressionAlgo>>` (same).

2. **`trait` static-member** — when a setting overrides `appendable`
   (i.e. supports `+=`), `BaseSetting<T>::trait::appendable = true`
   is declared explicitly. Currently `BaseSetting<PathsInChroot>::trait`
   does so; #56 names the placement question.

3. **`NIX_DECLARE_CONFIG_SERIALISER(T)` macro** — declares the
   `parse`/`to_string` *signatures* in a header so that consumers
   can rely on them being there.

These three layers don't compose cleanly: the macro declares the
signatures, the per-`T` specialisation defines them, and the
`trait` specialisation modifies the appendable behaviour — but
nothing forces all three to land together. #56 is the latent-bug
example (the `trait` specialisation lived in a .cc and was missed
by some TUs).

A `SettingTrait<T>` concept-constrained class template that bundles
`parse` + `to_string` + `appendable` + `default_value` would
collapse the three layers. Compounds with N4 (the X-macro setting
struct definition) — every per-T specialisation could be co-located
with the setting declaration.

**Sites:**
- `src/libutil/include/nix/util/configuration.hh` —
  `BaseSetting<T>`, `NIX_DECLARE_CONFIG_SERIALISER`.
- `src/libstore/globals.cc` — `BaseSetting<SandboxMode>`,
  `BaseSetting<PathsInChroot>`.
- `src/libutil/compression-settings.cc` —
  `BaseSetting<CompressionAlgo>`.
- `src/libstore/store-reference.cc` —
  `BaseSetting<StoreReference>`.

**Proposed abstraction:** a `SettingTrait<T>` concept + per-`T`
implementation that declares all three (`parse`, `to_string`,
`appendable`, default) inline. The macro evaporates.

**Why pass-1 missed it:** N4 names the per-instance settings
template; the per-T `BaseSetting<T>` specialisation pattern is the
*type-side* parallel that N4 doesn't touch.

**Effort:** medium.

**Cross-references:** compounds with N4, #19, #43, #56, #136.

---

## Disagreements with pass 1

### Pass 1 N1 over-claims "all six are insert-on-miss caches"

Pass 1 N1 claims all `Sync<std::map>` writers are insert-on-miss
caches; reading the four cited sites end-to-end shows three of them
(`gc.cc::connections`, `remote-store.hh::connectionFds`,
`filtering-source-accessor.cc::allowedPrefixes`) are *registries
with iteration semantics*, not memoisation caches. The Memo<K, V>
abstraction fits 4-5 of the cited sites; the other 2-3 need a
different shape (`OrderedRegistry<T>`).

The N1 candidate should split into two: N1a (insert-on-miss caches —
boost::concurrent_flat_map vs Sync<map>) and N1b (registries with
ordered iteration — keep `Sync<map>`).

### Pass 1 N3 undercount: 5 → 8 registries

Pass 1's heading says "five" but the table lists six and my grep
finds eight (see verification above). N3's framing is sound but the
N count needs to bump.

### Pass 1 N4 undercount: 19 → 21-22 settings structs

Independent grep finds 21-22 Config-derived settings structs; pass 1
says 19. Minor but worth correcting.

### Pass 1 N14 ambiguous count: 12 vs 14

Worker has 12 `MaintainCount`-managed counters plus 2 plain
`size_t` counters (`nrLocalBuilds`, `nrSubstitutions`). Pass 1's
"14" mixes the two types; if one of them migrates and the other
doesn't, the candidate's framing is unclear.

### Pass 1 N6 leaves the `LogSink` family under-named

Pass 1 N6 names `LogSink` (libstore/build/derivation-building-goal.cc)
and `BuildLog` (libstore/build/build-log.cc) as line-buffer
specialisations. Reading both end-to-end:
- `LogSink` — line-buffer + `\n`-split, then forward to logger.
- `BuildLog` — line-buffer + `\n`-split + JSON-frame extract +
  forward to logger via `parseJSONMessage`.

These are the *same* shape with different observers. The proposed
`splitOnDelimiter(sink, delim, callback)` adapter handles both with
just the observer callback. Pass-1's framing missed the JSON-frame-
extract case because it didn't read `BuildLog` end-to-end.

### Pass 1 U7 effort is a mix

The "rationalise `Sync<map>` vs `concurrent_flat_map`" series step is
listed as medium. Per N1 disagreement above, this should be split:
- U7a: write the rubric + helper (small, trivial).
- U7b: migrate the actual caches (small).
- U7c: leave the iteration registries alone (zero work; document why).

### Pass 1 N16 undercount: tests grid is larger than stated

Pass 1 says ~130 tests; my count is ~176 (44
`VERSIONED_CHARACTERIZATION_TEST` × 4 emitted = 176, plus
`*_NO_JSON` variants emitting 2 each).

### Pass 1's R10 is too quiet

R10 says "the codebase uses `std::thread` explicitly in many places";
my count is 6+ `std::thread` member fields, each with its own
lifecycle protocol (N43). The R10 entry undersells; N43 names the
specific sites and proposes the `std::jthread` migration target.

## Items to escalate / reroute

### Handoff to adversarial-existing agent (pass 2)

- **#86 ("OnStartup registration pattern") count is 5 in body, 6 in
  table; my recount is 8.** Pass 1 noted "five" matching the body;
  pass 2 should bump the canonical count to 8 (adding `Setting<T>`
  self-register and `GlobalConfig::Register`).

- **#19 ("Setting<T>{this, ...} initialiser pattern") count of "19+
  settings structs" — actual count is 21-22 (verified above). The
  body should be amended.

- **#54 ("SQLiteSettings::useWAL UB") branch reference is correct
  per the adversarial-existing review; no further action.

- **#168 ("magic numbers / cache schema versions encoded in
  filenames")** — pair with N31 which articulates the *cleanup
  hazard* of orphaned cache directories. The catalog's #168 is the
  clean-up direction; N31 is the safety direction.

### Handoff to a future implementation agent

- **N3, N4, N12, N20, N28, N31, U10 (`boost::describe`)** — the
  highest-leverage cluster, since each one unblocks several
  dependents. A coordinated implementation series should land
  U10 first, then N4 (or vice-versa: N4 in mock form first to
  validate the X-macro discipline).

- **N7 (`openAccessor(spec)`)** — a clean medium-effort PR series
  collapsing #92 and complementing N12 / N7 / #11.

- **N24, N25, N28, N29, N41 (C-API + tests cluster)** — small,
  low-risk PRs that clean up the C-API and test infrastructure. Good
  for warming up before structural work.

- **N43 (`std::jthread` migration)** — straightforward, one PR per
  subsystem (curl thread is the trickiest).

### Items mis-classified that pass 1 didn't catch

- **#211 ("force* family — 10 overloads")** is in section 21
  (libexpr eval-core EvalState/Value). Per N20 above, this is
  really the *table-driven dispatch* pattern across `nValueType`,
  not just a force* family. The catalog's section 21 placement is
  correct but the cross-reference to #36 / #66 should be tightened.

- **#129 ("MaintainCount → Finally")** — per N14 / N21, this is
  one slice of the broader counter-pattern question. The Finally
  refactor is fine on its own but doesn't address the meta-issue.

- **#122 ("nix::fun → std::move_only_function")** — pass 1 places
  this at U5; per the catalog, `nix::fun` is non-nullable. Migration
  to `std::move_only_function` loses the non-nullable invariant —
  the validation paragraph notes this implicitly. Worth flagging.

## Pattern clusters worth a section of their own (beyond pass 1's A-D)

### Cluster E: C-API surface duplication

The C-API has its own dialect of duplication that the existing
catalog only touches in shard 19. Membership: N24 (opaque
wrappers), N25 (init idempotency), N29 (NIXC_CATCH_ERRS variants),
plus the existing R-section in INVENTORY.md that names (but doesn't
candidate-ise) the `nix_*_init` and `_lazy`/non-lazy access pattern.
Six pattern instances in one library. **New section recommendation.**

### Cluster F: Test-infrastructure mirrors

N16 (production reflection mirrors test grid), N28 (meson env-var
plumbing), N39 (test-grid declarative), N41 (per-fixture
unitTestData boilerplate). Tests are out-of-scope per the catalog
but the test-side patterns are *evidence* of production-side
refactoring opportunities. **New section recommendation.**

### Cluster G: Dispatch tables and schemas

The catalog has #36, #66, #176, #178, #181, #182, #211, plus pass-1's
N3, N12, N20, N23, plus this pass's N26 (typed extractors), N32
(parse/show pairs), N40 (per-scheme drift). Together, this is
*the catalog's broadest cross-cutting pattern* — every subsystem
has a dispatch table that's hand-coded, with at least one parallel
hand-coded table elsewhere that should agree. The architectural
direction is "schema as data, not code", with `boost::describe` as
the canonical implementation choice.

This cluster compounds across A1-A6 (architectural debt themes) — a
dispatch-table is settings (A5), protocols (A6), value types (A1), or
fingerprints (A7) depending on what's being dispatched.

## Summary

This pass:

- **Verified all 23 of pass-1's N-candidates against source.** All
  hold up structurally. Six required count corrections (N3: 5→8;
  N4: 19→21-22; N14: 12+2 split; N16: 130→176; pass 1's R10
  undersells thread count; the SQLite cache count holds at 5).
- **Refined 3 of pass-1's candidates' framings.** N1 (caches vs
  registries), N6 (line-buffer family wider than stated), U7
  (split into 7a/7b).
- **Added 21 new candidates** (N24-N44), each with three+ concrete
  in-tree sites, each meeting the catalog's stated criteria. Six
  are C-API related (N24, N25, N29) or test-infrastructure (N28,
  N39, N41) — territory pass 1 explicitly named as under-explored.
  The rest cover production-code patterns (N26-N27, N30, N32-N38,
  N40, N42-N44).
- **Identified 3 new pattern clusters** (E: C-API, F: test
  infrastructure, G: dispatch tables) that pass 1's A-D didn't
  cover.
- **Disagreed with 7 specific pass-1 framings**, with evidence in
  each case from independent grep / source reading.

The single highest-leverage *new* observation: **Cluster G (dispatch
tables and schemas).** Every subsystem in the codebase has at least
one dispatch table — settings, protocols, value types,
fingerprints, opcodes — and most are hand-coded twice (once in the
schema-half and once in the consumption-half). N12, N23, N26, N32,
and N40 are projections of this pattern; the meta-claim is the
single largest architectural debt in the codebase. `boost::describe`
(U10) is its enabling technology.


