# Overlooked patterns and abstractions

23 new candidates (N1-N23) plus four pattern clusters, a C++23/Boost
uplift series, seven architectural-debt themes, and ten
redundant-abstraction pairs. Numbered N1+ to avoid collision with the
existing 1-217.

## Methodology

The catalog organises 217 candidates into 21 themed sections, each derived
from one or two verified shards. The per-shard origin is the catalog's
greatest strength (deep coverage of any single shard) and its greatest
blind spot (patterns that span shards rarely surface as a single
candidate). This pass holds many shards in view simultaneously, reading
end-to-end, to surface those patterns.

The discovery is held to the catalog's stated criteria: the eight debt
shapes listed in `candidates/README.md`. Cosmetic, performance-only, or
modernisation-for-its-own-sake findings are excluded.

Each new candidate cites at least three concrete in-tree sites, names the
abstraction the sites point toward, and lists which existing candidate(s)
it compounds with or supersedes.

Sources read end-to-end:
- All 21 candidate section files.
- The INVENTORY.md cross-shard topic index.
- `src/libutil/include/nix/util/{util,serialise,configuration,fs-sink,
  pool,args}.hh` (libutil core types).
- `src/libutil/include/nix/util/source-accessor.hh` and the
  `make*SourceAccessor` factories.
- `src/libstore/include/nix/store/{store-api,store-registration,
  build/goal,build/worker,legacy-ssh-store,remote-store,
  derived-path,derivation-options}.hh`.
- `src/libfetchers/{fetchers.hh, attrs.hh, github.cc, git.cc, path.cc}`
  (the InputScheme structural pattern).
- `src/libexpr/include/nix/expr/{eval,counter,nixexpr,attr-set,
  primops}.hh` plus `src/libexpr/eval.cc` for the `force*` family.
- `src/libstore-test-support/include/nix/store/tests/protocol.hh` and
  the GTest characterization macros (for the production-mirror
  finding).
- Selected files in `src/nix/` for the JSON-vs-text dual-output
  pattern.

## New candidates

### N1. Two complementary "concurrent insert-on-miss" idioms

**Pattern:** the codebase uses two different concurrent-cache machinery
for the *same* problem (memoise a pure computation across threads), with
no shared abstraction:

- `boost::concurrent_flat_map<K, V>` plus `getConcurrent`/`try_emplace_and_cvisit`
  (existing cand. #120 names this), used in `CachingSourceAccessor`,
  `MountedSourceAccessorImpl`, `WindowsSourceAccessor::cachedLstat`,
  `DummyStoreImpl::contents` / `derivations` / `buildTrace`,
  `EvalState::srcToStore` / `importResolutionCache` / `fileEvalCache` /
  `resolvedPaths` / `lookupPathResolved` / `positionToDocComment`,
  `nix_api_expr.cc::nix_refcounts`, `drvHashes` (cand. #139).
- `Sync<std::map<K, V>>` (a coarse-grained mutex-protected map), used in
  `input-cache.cc::cache_`, `git-utils.cc::workdirInfoCache_`,
  `pos-table.hh::origins_`, `gc.cc::connections`,
  `remote-store.hh::connectionFds`, `filtering-source-accessor.cc::allowedPrefixes`
  (`SharedSync<std::set<...>>`).

Both shapes are insert-on-miss caches; there is no documented criterion
for choosing between them. `boost::concurrent_flat_map` is read-mostly
fast; `Sync<std::map>` is fine-grained-write friendlier and supports
ordering. Each writer rolled their own.

**Sites:**

- `src/libutil/include/nix/util/util.hh` — `getConcurrent` (the only
  shared concurrent-map helper).
- `src/libutil/posix-source-accessor.cc` (cache global), `src/libutil/caching-source-accessor.cc`,
  `src/libutil/mounted-source-accessor.cc`.
- `src/libfetchers/input-cache.cc` (`Sync<std::map<Input, CachedInput>>`).
- `src/libstore/dummy-store.cc`, `src/libstore/include/nix/store/dummy-store-impl.hh`.
- `src/libfetchers/git-utils.cc` — `workdirInfoCache_` is `Sync<std::map>`,
  but the surrounding `repoPool` is `Pool<GitRepoImpl>`; same scope, two
  storage shapes.

**Proposed abstraction:** a `Memo<K, V, Policy>` (or simply two thin
`getOrInsertConcurrent` helpers, one per backend) plus a one-paragraph
decision rubric in `util.hh`: use `boost::concurrent_flat_map` when keys
are flat and reads dominate; use `Sync<std::map>` only when ordered
iteration or fine-grained-cv-wait is required. Compounds with cand.
#120 (the lookup half) by adding the *insert* half and the
*backend-choice* half.

**Why it was missed:** each per-shard cataloger saw only `getConcurrent`'s
half (covered by #120) or only their shard's `Sync<std::map>` (e.g.
input-cache in shard 14). Neither half names that the choice itself is
ad-hoc. A meta-cross-cutting section is missing.

**Effort:** small (rubric + helper) / medium (migrate the
six `Sync<std::map>` writers that don't actually need ordering).

**Cross-references:** compounds with #120 (lookup helper), #139 (`drvHashes`
location), #147 (`getFileTransfer`'s leaked-singleton `Sync` — same axis).

---

### N2. The `<algo>:<base-encoded>` family is wider than #8 names

**Pattern:** existing cand. #8 (Hash, Signature, Key) names three
encoders for the `<algo>:<base>` shape but misses two siblings already in
the tree:

- `Realisation::fingerprint(key)` is JSON-rendered and then signed; the
  resulting `<sigType>:<base64>` shape goes through `Signature` (covered
  by #8). But the *call site* in `UnkeyedRealisation::sign` builds the
  fingerprint string by hand each time.
- `ValidPathInfo::fingerprint` produces `1;<path>;<narHash>;<size>;<refs>`
  — a different shape but with the same shape-pattern (versioned-prefix +
  fields), used universally for binary-cache signatures (see INVENTORY
  guarantee on `fingerprint`).
- `UnkeyedRealisation::fingerprint` uses `JSON-with-signatures-dropped` —
  a parallel pattern.

Beyond that, two more colon-separated fields exist with no shared
parser:

- `<flake-id>:<flake-hash>` in `LegacyArgs::lookupPathHooks` URL-style
  path-and-rev splits.
- `<drv>!<output>` (legacy `DerivedPath::to_string_legacy`) and the
  modern `<drv>^<output>` separator (`to_string`/`render`).

**Sites:**

- `src/libutil/hash.cc` — `Hash::parseAny`, `Hash::to_string`.
- `src/libutil/signature/local-keys.cc` — `parseColonBase64` /
  `serializeColonBase64`.
- `src/libstore/path-info.cc` — `ValidPathInfo::fingerprint`,
  `UnkeyedRealisation::fingerprint`.
- `src/libstore/include/nix/store/derived-path.hh` — `to_string`
  (`^`-separator) vs `to_string_legacy` (`!`-separator).
- `src/libstore/include/nix/store/path-with-outputs.hh` — `StorePathWithOutputs`
  variant of the same.

**Proposed abstraction:** rather than one universal helper, declare a
`SeparatorEncoded<Sep, Codec>` namespace of small free functions and
collect them. Specifically: a generic `parseColonPrefixed(s, allowedAlgos,
codec)` plus `formatColonPrefixed(name, codec, bytes)` shared by hash and
signature; a generic `parseSeparated(s, sep)` that all four
`<drv>(^|!)<out>`-style splits use; and a `KeyedFingerprint<Body>` mixin
(see N5 below) for the `1;<...>;<...>` versioned-prefix shape. The
catalog's #8 framing as "one helper" oversimplifies; the right factoring
is a *family of two-three* helpers.

**Why it was missed:** #8 noticed three encoders; the verifier scoped
them as cross-cutting (libutil/data) but didn't notice the `^`/`!`
separator pair (shard 05) or the `1;...` fingerprint shape (shard 05
again, where the separator question doesn't recur). The pattern crosses
shards 02 and 05 but the cataloger of either shard saw only their own
half.

**Effort:** small (formatColonPrefixed pair) / medium (full family).

**Cross-references:** extends #8, compounds with #102 (Keyed/Unkeyed
fingerprint), #104 (Renderable).

---

### N3. Five static-registration registries with no `Registry<Key, Factory>` template

**Pattern:** cand. #86 names the `OnStartup` lambda registration shape
across libfetchers/libstore/libcmd/libexpr/libstore-build but does not
articulate the *type-shaped duplication*. Each registry is a separate
struct with the same skeleton:

| Registry | Key type | Value type | Storage |
| -------- | -------- | ---------- | ------- |
| `RegisterPrimOp` | sorted by `name` | `PrimOp` | `static PrimOps & primOps()` |
| `RegisterCommand` | `vector<string>` | `fun<ref<Command>()>` | `static Commands & commands()` |
| `RegisterLegacyCommand` | `string` | `fun<int(int, char**)>` | static map |
| `RegisterStoreImplementation<TConfig>` | `TConfig::name()` | `StoreFactory` | `Implementations::registered()` |
| `RegisterBuiltinBuilder` | name | `fun<void(BuiltinBuilderContext)>` | static map |
| `registerInputScheme` | `schemeName()` | `unique_ptr<InputScheme>` | global vector |

Each is a Meyers singleton with a different signature, a different
storage container, a different "duplicate registration" policy
(`PrimOp` re-registration is silently allowed; `Implementations::add`
throws on duplicate; `RegisterCommand` overwrites silently).

**Sites:**

- `src/libexpr/include/nix/expr/primops.hh` — `RegisterPrimOp`.
- `src/libcmd/include/nix/cmd/command.hh` — `RegisterCommand` /
  `registerCommand` / `registerCommand2`.
- `src/libcmd/include/nix/cmd/legacy.hh` — `RegisterLegacyCommand`.
- `src/libstore/include/nix/store/store-registration.hh` —
  `RegisterStoreImplementation<TConfig>` + `Implementations`.
- `src/libstore/include/nix/store/builtins.hh` — `RegisterBuiltinBuilder`.
- `src/libfetchers/include/nix/fetchers/fetchers.hh` —
  `registerInputScheme` (free function).
- *plus* the per-`Setting<T>` constructor self-registration into
  `Config::_settings` (cand. #136), which is morally another registry —
  same SIOF hazard, same need for an explicit "register all" call.

**Proposed abstraction:** a `Registry<Key, Value, DuplicatePolicy>`
template owning the static map, with a `Registrar` RAII type that
inserts/removes. Standardise the duplicate-registration policy
(throwing is the safer default; `RegisterCommand` should adopt it).
Replaces six bespoke registries with one parameterised template. This is
the abstraction #86 hinted at without naming.

**Why it was missed:** #86 sits in section 10 ("other distinct
candidates") and was framed as a *boilerplate* simplification. The
deeper claim — that the same datum (key/value/duplicate-policy) is
encoded six times — is a cross-cutting, structural pattern. None of the
per-shard catalogers saw enough registries to spot it.

**Effort:** medium (template) / structural (migrating all six
registrations consistently).

**Cross-references:** compounds with #86 (registration shape), #136
(Setting constructor self-register, the seventh "registry"), #137
(GlobalConfig::Register; the eighth), #166 (forward-decl pattern; same
SIOF concerns).

---

### N4. The `<TYPE>Settings` structs are 19+ near-identical Cartesian products

**Pattern:** cand. #144 names "19 separate Config-derived globals" and
proposes per-subsystem ownership. What the candidate does *not* say is
that those 19 structs are themselves a uniform shape:

```
struct XxxSettings : Config { Setting<T0> k0{this, ...}; Setting<T1> k1{this, ...}; ... };
extern XxxSettings xxxSettings;
static GlobalConfig::Register r(&xxxSettings);
```

Every one of them — `Settings`, `LocalSettings`, `WorkerSettings`,
`LogFileSettings`, `NarInfoDiskCacheSettings`, `EvalSettings`,
`fetchers::Settings`, `flake::Settings`, `CompatibilitySettings`,
`FileTransferSettings`, `ArchiveSettings`, `RestoreSinkSettings`,
`LoggerSettings`, `PluginSettings`, `DevelopSettings`, `EnvSettings`,
`AuthorizationSettings`, `UpgradeSettings`, `ExperimentalFeatureSettings`
— is the same record, instantiated 19 times. (The diamond-mixin shape
in `Settings` is a per-instance complication, cand. #134, not a
cross-instance one.)

This is a *macro-driven boilerplate that should be a template* (debt
shape #1). The boilerplate is not just `Setting<T>{this, …}` (cand. #19)
but the entire surrounding "struct + extern + Register" triplet. A
single `NIX_DECLARE_SETTINGS_STRUCT(Name)` plus a member list (or a
`SETTINGS_STRUCT(Name, ((bool, foo, false, "doc"))((int, bar, 1, "doc")))`
X-macro) collapses each definition.

**Sites:**

- `src/libstore/include/nix/store/globals.hh` —
  `Settings`, `WorkerSettings`, `LogFileSettings`, `NarInfoDiskCacheSettings`.
- `src/libstore/include/nix/store/local-settings.hh` —
  `LocalSettings`, `GCSettings`, `AutoAllocateUidSettings`.
- `src/libfetchers/include/nix/fetchers/fetch-settings.hh` —
  `Settings` (libfetchers).
- `src/libflake/include/nix/flake/settings.hh` — `Settings` (libflake).
- `src/libexpr/include/nix/expr/eval-settings.hh` — `EvalSettings`,
  `EvalProfilerSettings`.
- `src/libstore/filetransfer.cc`, `archive.cc`, `fs-sink.cc`, `logging.cc`,
  `plugin.cc`, `nix/develop.cc`, `nix/env.cc`, `nix/upgrade-nix.cc`,
  `nix-c/nix_api_util.cc`.
- The same shape for the 15 `GlobalConfig::Register` instances (cand. #137).

**Proposed abstraction:** a single header (`config-struct.hh`) with an
X-macro driver:

```
NIX_SETTINGS_STRUCT(LoggerSettings,
    ((bool,    showTrace,         false, "..."))
    ((bool,    jsonLogPath,       false, "...")))
```

The macro emits the struct, the `Setting<T>{this, default, name, doc}`
list, and the global-config registration. Compounds with #19, #21 (per-T
specialisations) and #43 (X-macro setting list); collapses #137 by
generating the registration too.

**Why it was missed:** each verifier saw the duplication of *their*
settings struct and noted it (#19, #134, #135, #144). None had all 19 in
view at once, so the *shape-itself-is-a-template* observation didn't
land.

**Effort:** medium (X-macro driver + migrate one struct as proof) /
large (migrate all 19).

**Cross-references:** compounds with #19, #21, #43, #134, #135, #136,
#137, #144, N3.

---

### N5. `Keyed<Body>` / `Signable<Body>` mixin pattern hides in three places

**Pattern:** cand. #102 articulates that `UnkeyedValidPathInfo` →
`ValidPathInfo`, `UnkeyedRealisation` → `Realisation`, and
`UnkeyedNarInfo` → `NarInfo` form a "diamond-of-keying" pattern. Beyond
the diamond, the deeper duplication is the `fingerprint` + `sign` +
`checkSignature` + `checkSignatures` *protocol*: each pair re-implements
a four-method bundle parametrised on what `Body::fingerprint(Key)` looks
like.

The catalog's #102 framing is "type-pair shape". The deeper observation:
this is the *same protocol* with different fingerprint formats. A
`Signable<Body>` mixin lifting the four sign/verify members onto any
type that provides `Body::fingerprint() -> string` would replace ~120
lines across `path-info.cc` and `realisation.cc`. The diamond inherit
question (cand. #102) is orthogonal to the protocol-mixin question.

**Sites:**

- `src/libstore/path-info.cc` — `ValidPathInfo::fingerprint` / `sign` /
  `checkSignature` / `checkSignatures`.
- `src/libstore/realisation.cc` — `UnkeyedRealisation::fingerprint` /
  `sign` / `checkSignature` / `checkSignatures`.
- `src/libstore/nar-info.cc` — inherits from `ValidPathInfo` so reuses,
  but `NarInfo::sign` carries an additional `narInfoExtra` payload that
  could express via mixin policy.

**Proposed abstraction:** a `Signable<Derived>` CRTP/mixin that requires
`Derived::fingerprint() -> string` (or the keyed variant
`Derived::fingerprint(Key) -> string`) and provides
`sign`/`checkSignature`/`checkSignatures` for free. Pairs with the
versioned-prefix `<int>;<field>;<field>...` formatter mentioned in N2.
Independent of cand. #102's diamond-flattening; both should land
together.

**Why it was missed:** the cataloger of shard 05 (libstore-core) saw
the unkeyed/keyed pair and proposed #102, but they framed it as a
*type-pair* refactor and didn't isolate the *protocol-mixin* layer that
sits above it.

**Effort:** medium. Touches public ABI of `path-info.hh` /
`realisation.hh` / `nar-info.hh` headers; coordinated with #102.

**Cross-references:** compounds with #102; N2 supplies the underlying
fingerprint formatter.

---

### N6. The "wrap a Source/Sink to add a side-channel" pattern recurs without abstraction

**Pattern:** the `Source`/`Sink` zoo in `serialise.hh` plus
`references.hh` plus `make-content-addressed.cc` plus `archive.cc`
contains many adapters whose only job is to *observe* bytes flowing
through, sometimes mutating them:

- `TeeSink` / `TeeSource` — copy bytes to a second sink/source.
- `LengthSink` / `LengthSource` — count bytes.
- `HashSink` / `HashedSink` (in `hash.{hh,cc}`) — hash bytes as they
  flow.
- `RefScanSink` (`references.cc`) — substring-match for hash refs.
- `PathRefScanSink` (`path-references.cc`) — substring-match + map back.
- `RewritingSink` (`references.cc`) — substring substitute.
- `HashModuloSink` (`references.cc`) — compose `RewritingSink` +
  `HashSink` so the hash is taken over the rewritten bytes.
- `LogSink` (`derivation-building-goal.cc`) — split on `\n`.
- `BuildLog` (`build-log.cc`) — split on `\n` + JSON-frame extract.
- `NullSink` — discard.
- `StringSink` — collect into string.
- `ChainSource` — chain two sources (cand. #38 lists it).
- The compression sinks — write into compressor.

The catalog has #38 (sink/source wrapper hierarchy, "leave as-is unless
touching") and #103 (scan-and-rewrite pipeline, "factor the
tail-buffered scanning loop"). What neither names is the *side-channel
observer* abstraction: a `ObserverSink<Tag>` (or
`Sink::andThen(observer)` adapter) that lets an arbitrary side-effect
run on every byte without subclassing.

The *real* duplication is the boilerplate: every observer subclass
spells out `void operator()(string_view data) override { ... do_thing;
inner(data); }`. C++23 deducing-this on a base would let the
side-channel be a function-shaped argument:

```
auto sink = makeObserverSink(innerSink, [&](string_view bytes) { length += bytes.size(); });
```

Replacing `LengthSink` with `makeObserverSink(NullSink{}, lengthCounter)`
loses zero efficiency.

**Sites:**

- `src/libutil/include/nix/util/serialise.hh` — `TeeSink`, `LengthSink`,
  `LambdaSink`.
- `src/libutil/hash.cc` / `hash.hh` — `HashSink`.
- `src/libstore/references.cc` — `RefScanSink`, `RewritingSink`,
  `HashModuloSink`.
- `src/libstore/path-references.cc` — `PathRefScanSink`.
- `src/libstore/build/derivation-building-goal.cc` — `LogSink`.
- `src/libstore/build/build-log.cc` — `BuildLog`.
- `src/libstore/make-content-addressed.cc` — composes `HashModuloSink` +
  `RewritingSink`.

**Proposed abstraction:** a `WrappingSink<Inner, Observer>` template
plus a `splitOnDelimiter(sink, delim, callback)` adapter. The
line-buffering family (`LogSink`, `BuildLog` line-buffer half)
collapses into one template. The substring-scan family already shares a
`RefScanSink` base, but a templated `ScanSink<MatchPolicy>` with a
`MatchPolicy::onMatch(pos, ref)` member would let `RefScanSink` and
`PathRefScanSink` differ only in the match-output type.

**Why it was missed:** #38 was already validated as PARTIALLY VALID with
"leave as-is unless touching"; the verifier conservatively dismissed the
sink-subclass simplification because it sees the named types as ABI. But
the deeper observation is that *new* sites continue to grow new
single-purpose subclasses (`LogSink`, the Brotli sinks); a
`makeObserverSink` factory would prevent that growth without renaming
existing types.

**Effort:** small (factory) / medium (migrate `LogSink` /
`HashModuloSink` / `BuildLog` line-buffer half).

**Cross-references:** compounds with #38, #44, #103, #157.

---

### N7. Three "open a SourceAccessor over a thing" entry points reimplement the same shape

**Pattern:** the codebase has many sites that *open* a `SourceAccessor`
over a thing — a NAR file, a store path, a tarball, a git repo, a
filesystem path. Each does its own (1) detect-the-format, (2)
allocate-the-accessor, (3) wrap-with-filtering, (4) wrap-with-caching
sequence. The catalog has #92 (store/nar cat-and-ls reimplements
construction four times) but doesn't name the broader pattern.

Inventory of `SourceAccessor` factories from shard 01 + 07 + 08 + 14:

- `makeFSSourceAccessor` (filesystem root).
- `makeMountedSourceAccessor` (compose accessors at paths).
- `makeUnionSourceAccessor` (overlay).
- `makeCachingSourceAccessor` (memoise lstat/readLink; cand. #40).
- `makeMemorySourceAccessor`, `MemorySourceAccessor` (in-memory tree).
- `makeEmptySourceAccessor`.
- `makeLazyNarAccessor` (NAR streaming).
- `makeNarAccessor` (NAR parsed).
- `LocalStoreAccessor` (store-rooted FS).
- `RemoteFSAccessor` (per-path NAR fetch + NAR cache).
- `GitSourceAccessor` (libgit2-backed).
- `FilteringSourceAccessor` (libfetchers).
- `AllowListSourceAccessor` (subclass of filtering).
- `RestrictedStoreAccessor` (recursive-nix).
- `TarSourceAccessor` (tar/zip via libarchive — actually not in tree as
  named; tarball.cc unpacks instead).

Cross-cutting consumer code:

- `nix store cat`, `nix store ls`, `nix nar cat`, `nix nar ls` (cand.
  #92).
- `nix path-info` (which uses `requireStoreObjectAccessor`).
- `EvalState::storeFS` / `rootFS` / `corepkgsFS` construction (cand.
  #212).
- `Input::getAccessor` for every fetcher.

Each consumer reimplements the "given some URL-or-path-or-blob, give me
an accessor" mapping inline. There is no `openAccessor(spec)` entry
point.

**Sites:**

- `src/libutil/source-accessor.cc` — base class plus the make* helpers.
- `src/libutil/posix-source-accessor.cc`, `mounted-source-accessor.cc`,
  `caching-source-accessor.cc`, `union-source-accessor.cc`,
  `memory-source-accessor.cc`.
- `src/libstore/local-fs-store.cc` — `LocalStoreAccessor`.
- `src/libstore/remote-fs-accessor.cc` — `RemoteFSAccessor`.
- `src/libfetchers/git-utils.cc` — `GitSourceAccessor`.
- `src/libfetchers/filtering-source-accessor.cc`.
- `src/nix/store/cat.cc`, `src/nix/nar/{cat,ls}.cc`,
  `src/nix/store/dump-path.cc` (cand. #92).
- `src/libexpr/eval.cc` — the `storeFS` lambda (cand. #212).

**Proposed abstraction:** an `AccessorSpec` variant
(`StorePath{}`, `NarFile{Path}`, `LocalPath{Path}`, `Tar{Path,
TarOptions}`, `Empty{}`, ...) plus an `openAccessor(spec, store)` free
function returning `ref<SourceAccessor>`. The four `MixCat`/`MixLs` run
bodies in #92 collapse onto one. `EvalState`'s `rootFS` lambda becomes
a one-line `openAccessor(SpecForEval{...})`. Compounds with #11, #12
("delegating store" abstraction is the matching layer in store-space).

**Why it was missed:** the per-shard catalog noticed the factories in
shard 01 (libutil-io), the LocalStore/Remote variants in shards 07/08,
and the libfetchers wrappers in shard 14, but no shard has the four
together. #92 is the cataloger's only attempt and is scoped to four
commands, not the whole pattern.

**Effort:** medium.

**Cross-references:** compounds with #11, #12, #14, #15, #40, #92, #212.

---

### N8. Async-callback / sync-future / coroutine has three parallel idioms

**Pattern:** the codebase has three concurrency idioms for the same
problem (express "compute X, may take a while, may fail"):

1. `Callback<T>` (in `callback.hh`) — explicit continuation-passing.
   Used in `Store::queryPathInfo` (the uncached half;
   `queryPathInfoUncached`), `Store::queryRealisation`, and many other
   `*Uncached` methods. The synchronous wrapper is hand-rolled (cand.
   #22).
2. `boost::asio::awaitable<T>` (the in-tree alias `nix::asio`) — used in
   `HttpBinaryCacheStore::topoSortPaths` via `callbackToAwaitable` and
   `forEachAsync` (libutil); marginal in-tree adoption.
3. `Goal::Co` (the bespoke build-system coroutine; cand. #160) — used
   only by libstore/build.

The same logical operation (`Store::queryPathInfo`) is exposed via *all
three*: the sync wrapper, the callback uncached version, and (when it
grows in libstore/build) sometimes via a `Co` step. The catalog has
#22 (async/sync pair pattern) and #160 (Co → asio) but does not name
the unified question: should the codebase have one async idiom?

**Sites:**

- `src/libutil/include/nix/util/callback.hh` — `Callback<T>`.
- `src/libutil/async.hh` / `async.cc` — `nix::asio` namespace,
  `callbackToAwaitable`, `forEachAsync`.
- `src/libstore/include/nix/store/store-api.hh` — `*Uncached` callback
  methods (~10 of them).
- `src/libstore/include/nix/store/build/goal.hh` — `Goal::Co`,
  `promise_type`, the three custom awaiters.
- `src/libstore/http-binary-cache-store.cc` — the awaitable/callback
  bridge.
- `src/libstore/build/derivation-goal.cc` — `Goal::Co` consumer.

**Proposed abstraction:** pick one. `boost::asio::awaitable<T>` is the
right target (already in tree, already documented as the asio of choice
by INVENTORY's threading-primitives section). Three coordinated changes:

(a) Convert all `*Uncached(args, Callback<T>)` to
`asio::awaitable<T> *Uncached(args)`. The sync wrappers (#22) collapse
to `asio::co_spawn(std::execution::sync, fn).get()`.

(b) Convert `Goal::Co` to `asio::awaitable<T>` per cand. #160 (keeping
the bespoke `final_awaiter` per the validation note).

(c) Delete `Callback<T>` once the last consumer migrates.

This is structural and large but the *unification claim* belongs in the
catalog. The catalog currently presents #22 and #160 as independent
items; they're not.

**Why it was missed:** #22 is a libutil consolidation; #160 is a
build-system structural refactor. The verifiers correctly assessed
each in isolation. The cross-cutting observation (the codebase has
three async idioms; pick one) is missing.

**Effort:** structural.

**Cross-references:** subsumes #22; sequences with #160 (Co → asio
first); compounds with #147 (file-transfer; the curl thread is its own
fourth idiom; see N17).

---

### N9. Per-platform "Pid"/"Pipe"/"Process" symmetry has no shared interface

**Pattern:** the catalog has #27 (file-system / file-descriptor /
processes per-platform parallel), #81 (Pid Unix vs Windows divergence),
#163 (`Pipe::create` arity divergence), #164 (`#ifdef _WIN32` switches
in headers). All four are aspects of the same underlying observation:
the platform layer has *no shared interface header*. Each header
declares "one of two parallel APIs" with the choice made at preprocessor
time.

The pattern is the same as #28/#34 (DerivationBuilder diamond): a
Cartesian product (operation × platform) is being expressed as a
hand-coded switch on `_WIN32` instead of as polymorphism or
template-parameterised types.

What's missing is a meta-observation that platform-conditional code in
libutil is its own *category* and should have one consistent strategy.
The current state mixes:

- `#ifdef _WIN32` blocks in public headers (`file-descriptor.hh`,
  `file-system.hh`, `processes.hh`).
- Parallel `unix/` and `windows/` source dirs with same-named files
  (`pathlocks.cc`, `users.cc`, `environment-variables.cc`,
  `file-descriptor.cc`).
- Platform-only headers in `windows/include/` (cand. #162) and
  `unix/`-only namespaces in shared headers (`nix::unix::SelfPipe`).
- A `Descriptor` typedef in `file-descriptor.hh` that's `int` on Unix
  and `HANDLE` on Windows — but `Pid` does *not* follow the same shape
  (`pid_t` on Unix, `AutoCloseFD` on Windows, per #81).

**Sites:**

- `src/libutil/include/nix/util/file-descriptor.hh` (#163, #164).
- `src/libutil/include/nix/util/file-system.hh` (#164).
- `src/libutil/include/nix/util/processes.hh` (#27, #81).
- `src/libutil/include/nix/util/muxable-pipe.hh` (#162).
- `src/libutil/unix/`, `src/libutil/windows/` (the parallel impl dirs).
- `src/libstore/build/worker.cc` (Worker::Waker self-pipe vs IOCP, #152).

**Proposed abstraction:** explicit "platform interface" headers
(`file-descriptor-iface.hh`) carrying *only* the API; per-platform
implementations include the iface and provide implementations. No
`#ifdef _WIN32` in any public header. `Pid` becomes a class with two
implementation files (one per platform) selected by build system. The
catalog has #163, #164 individually, but the *meta-rule* "no `#ifdef
_WIN32` in public headers" is not stated.

**Why it was missed:** #27, #81, #163, #164, #162, #29, #161 are all
slices of one elephant. Each slice is in its own per-shard or
per-section bucket; the elephant has no name.

**Effort:** structural (multi-PR; touches every libutil platform header).

**Cross-references:** subsumes #27, #29, #81, #161, #162, #163, #164;
overlaps with #152 (Worker::Waker).

---

### N10. Per-protocol `Connection` shapes have no `BasicConnection<Proto>`

**Pattern:** cand. #185 names the worker-vs-serve `BasicClientConnection`
struct duplication. Compounding observation: there are *five* connection
classes in libstore that each carry their own `to`/`from`/`version`
trio, plus a `Pool<Connection>::Handle`-style wrapper layer:

- `WorkerProto::BasicConnection` — base.
- `WorkerProto::BasicClientConnection` (extends).
- `WorkerProto::BasicServerConnection` (extends).
- `ServeProto::BasicClientConnection` (parallel; no shared base; #185).
- `ServeProto::BasicServerConnection` (parallel).
- `RemoteStore::Connection` (extends `WorkerProto::BasicClientConnection`
  + `ClientHandshakeInfo` + a `startTime`).
- `LegacySSHStore::Connection` (extends nothing; carries
  `unique_ptr<SSHMaster::Connection>`, `FdSink to`, `FdSource from`,
  `ServeProto::Version remoteVersion`, `bool good = true`).
- `SSHStore::Connection : RemoteStore::Connection`.
- `UDSRemoteStore::Connection : RemoteStore::Connection`.

Plus the wrappers:
- `RemoteStore::ConnectionHandle` (cand. #184).
- `LegacySSHStore`'s private pool handle.

**Sites:**

- `src/libstore/include/nix/store/worker-protocol-connection.hh`.
- `src/libstore/include/nix/store/serve-protocol-connection.hh`.
- `src/libstore/include/nix/store/remote-store-connection.hh`.
- `src/libstore/include/nix/store/legacy-ssh-store.hh`.
- `src/libstore/ssh-store.cc`, `src/libstore/uds-remote-store.cc`,
  `src/libstore/legacy-ssh-store.cc`.

**Proposed abstraction:** a `BasicConnection<Proto>` template
parameterised on `WorkerProto`/`ServeProto` carrying the `to`/`from`/
`version` trio plus a `markBad`/`good` state machine. Plus a
`PooledClientStore<ConcreteConnection>` non-template base owning the
`Pool<ConcreteConnection>` and the `getConnection`/`processStderr`
plumbing. `RemoteStore`, `LegacySSHStore`, and (eventually) any future
RPC-like store all inherit from `PooledClientStore<...>`.

This is what cand. #16 hinted at ("legacy-ssh-store rolls its own
Pool<Connection>") and cand. #185 hinted at (`BasicConnection<Proto>`
template) — one abstraction unifies both.

**Why it was missed:** the cataloger of shard 09 (protocols) saw
`BasicClientConnection` parallels (#185); the cataloger of shard 08
(remote stores) saw the LegacySSHStore pool duplication (#16). Neither
saw `RemoteStore::ConnectionHandle` (#184) plus `BasicClientConnection`
(#185) plus `LegacySSHStore::Connection` (#16) as one composite.

**Effort:** medium. Combine #16, #184, #185 into one staged refactor.

**Cross-references:** subsumes #185; compounds with #16, #184.

---

### N11. The "iterate-attrset, validate by name" idiom recurs in seven places

**Pattern:** the catalog has #79 (per-fetcher attrset iteration) and
mentions it compounds with #190 (fetchTree's loop). The pattern is
much wider than two fetchers:

The shape is:
```
for (auto & attr : *args[0]->attrs()) {
    if (n == "x") handle_x(state, attr);
    else if (n == "y") handle_y(state, attr);
    ...
    else throw EvalError(... "unsupported argument %s ...");
}
```

Sites that follow this shape:

- `src/libexpr/primops.cc`'s many primops accepting attrset args:
  `prim_derivation`, `prim_fromTOML` (no, that's a parser).
- `src/libexpr/primops/fetchClosure.cc::prim_fetchClosure`.
- `src/libexpr/primops/fetchMercurial.cc::prim_fetchMercurial`.
- `src/libexpr/primops/fetchTree.cc::fetchTree`.
- `src/libstore/derivation-options.cc::derivationOptionsFromStructuredAttrs`
  (a sibling: iterates a JSON object for `__structuredAttrs` derivation
  options).
- `src/libfetchers/path.cc::PathInputScheme::inputFromURL` (iterates
  `url.query`; same shape, different source).
- Similar iterations in `src/libfetchers/{git,github,mercurial,tarball,
  indirect}.cc`'s `inputFromURL` / `inputFromAttrs`.

The catalog's #79 is small and fetcher-scoped. The cross-cutting
candidate is: this *iterate + dispatch + validate* shape is the same
every time — it should be a `dispatchAttrs` helper.

```
attrSwitch(*args[0]->attrs(), state, primopName)
    .on("type", [&](auto & a) { ... })
    .on("path", [&](auto & a) { ... })
    .otherwise([&](auto & name) { throw ...; });
```

The shape is also identical to `derivationOptionsFromStructuredAttrs`
which uses `getStringAttr`/`getBoolAttr`/`getStringSetAttr` (cand.
#114) — both are "look up a key by name with a typed coercion + diag".

**Sites:**

- `src/libexpr/primops.cc` (multi sites).
- `src/libexpr/primops/{fetchClosure,fetchTree,fetchMercurial}.cc`.
- `src/libstore/derivation-options.cc::getStringAttr` /
  `getBoolAttr` / `getStringSetAttr`.
- `src/libfetchers/path.cc`, `git.cc`, `github.cc`, `tarball.cc`,
  `mercurial.cc`, `indirect.cc`.
- `src/libfetchers/include/nix/fetchers/attrs.hh` —
  `getStrAttr`/`getBoolAttr`/`getIntAttr`/`maybeGetStrAttr`/etc.

**Proposed abstraction:** a generic `AttrSetBuilder` (or
`attr_dispatch`) helper that takes a name, a context-string for diagnostics,
and a typed handler. Could absorb the JSON-side equivalents (`getString`,
`getBoolean`, `getStringSet` in `json-utils.hh`) into a single typed
attribute-extraction layer that's polymorphic over JSON-vs-AttrSet-vs-StringMap
backends. Compounds with #65 (primop `force*` arg validation), #114
(derivation-options getX helpers), #190 (fetchTree's loop).

**Why it was missed:** #79 is in section 10 ("other"), #114 is in
section 12 ("libutil + libstore-core extras"), #190 is in section 19
(eval-core fetcher). Three section homes for one pattern.

**Effort:** medium.

**Cross-references:** subsumes #79, #114; compounds with #65, #190.

---

### N12. The "registered fetcher" Cartesian product is over-flat

**Pattern:** every concrete `InputScheme` re-implements the same flow:

1. `inputFromURL`: detect scheme, iterate `url.query`, collect into `Attrs`.
2. `inputFromAttrs`: accept-or-reject keys.
3. `allowedAttrs`: hand-write a static `std::map<string, AttributeInfo>`
   table.
4. `toURL`: the inverse of (1).
5. `getAccessor`: open a SourceAccessor (per N7).
6. `getFingerprint`: build a deterministic identifier.

The catalog has #32 (per-scheme rewrite, "BaseInputScheme<T> CRTP") but
is too narrowly scoped. The structural problem is that the
`InputScheme` is *also* a registry record — it has both polymorphic
runtime methods (`getAccessor`, `getFingerprint`) AND essentially
static metadata (`schemeName`, `allowedAttrs`, `experimentalFeature`,
the per-key documentation). The static half should be a `consteval`
descriptor; the runtime half should be a small set of
function-pointers.

Look at the `allowedAttrs` doc-strings for `git.cc` (~140 LoC), or the
9 attrs declared on `path.cc`, or the 12 on `github.cc`. Each is hand-
maintained; each attr has the same `{type, required, doc}` triple; each
scheme inserts/extracts the attr in `inputFromURL` and `inputFromAttrs`
*by hand*. Define the schema once (e.g. via `boost::describe` or an
X-macro of attrs), and `inputFromURL`/`inputFromAttrs`/`allowedAttrs`
all derive from it.

This is the *same* shape as the daemon protocol (cand. #176 — one giant
`switch` over opcodes; #182 — every `RemoteStore::*` opcode method
hand-rolls "open conn / write op / write args / processStderr / read
response"). In both cases the table-vs-code split is wrong: the
table is the source of truth and the code is an interpretation; right
now both halves are written by hand.

**Sites:**

- `src/libfetchers/include/nix/fetchers/fetchers.hh` — `InputScheme`
  base.
- `src/libfetchers/git.cc`, `github.cc`, `tarball.cc`, `mercurial.cc`,
  `path.cc`, `indirect.cc` — each scheme's own
  inputFromURL/inputFromAttrs/allowedAttrs trio.
- `src/libstore/daemon.cc` — `performOp` switch (#176).
- `src/libstore/remote-store.cc` — opcode methods (#182).

**Proposed abstraction:** an `attribute_schema<Scheme>` constexpr
descriptor (vector of `(name, type, required, doc, getter, setter)`)
declared per-scheme; the base class's `inputFromAttrs` /
`inputFromURL` / `allowedAttrs` are template'd over the descriptor.
Same shape applied to opcodes: an `opcode_schema<Op>` descriptor that
the daemon and the client both consume. When the catalog speaks of
"declarative dispatch table" (#177, #178, #181, #182), this is the
underlying common abstraction: each is an instance of "one schema,
multiple consumers" applied to different domains.

**Why it was missed:** #32 sits in section 5 (inheritance flattening);
#176, #177, #181, #182 sit in section 18 (daemon protocol audit). The
*meta-claim* that both subsystems have the same shape — two-sided
dispatch where the schema is hand-coded twice — is missing.

**Effort:** structural. Phased: (a) write the descriptor for one
scheme; (b) generate the three methods from it; (c) repeat per scheme;
(d) apply the same shape to opcodes once schemes settle.

**Cross-references:** subsumes #32 partially; compounds with #176,
#177, #178, #181, #182, N3 (registries), N11 (attrset iteration).

---

### N13. The five SQLite caches share no `SqliteCache<Schema>` base

**Pattern:** the codebase has five SQLite-backed caches, each with its
own filename version, schema, init logic, transaction discipline, and
purge strategy:

| Cache | File | Schema | Where |
| ----- | ---- | ------ | ----- |
| Local store | `db.sqlite` | `local-store/schema.sql` | `LocalStore` |
| Eval cache | `eval-cache-v6/<fp>.sqlite` | `attrDb.cc::schema` | `eval-cache.cc` |
| Binary cache (NarInfo) | `binary-cache-v8.sqlite` | `nar-info-disk-cache.cc` | `nar-info-disk-cache.cc` |
| Fetcher cache | `fetcher-cache-v4.sqlite` | `cache.cc::schema` | `libfetchers/cache.cc` |
| Tarball cache | `tarball-cache-v2/...` (libgit2) | (libgit2 packfile) | `git-utils.cc` |

Each cache:
- Encodes its schema version in the *filename*, not in `PRAGMA
  user_version` (cand. #168, #201).
- Has a `try { ... } catch (SQLiteError &) { ... disable }` "doSQLite"
  pattern, but the disabling protocol is not consistent (#203 names
  the read-vs-write asymmetry in `eval-cache`).
- Implements its own transaction-and-prepared-statement state (`State`
  inner struct, members for each prepared `SQLiteStmt`).
- Re-runs the `create table if not exists` on every open.
- Has bespoke purge logic (`narinfo`'s ad-hoc `LastPurge` row).

Cand. #168 names the magic-number aspect; #201 names the schema
version-marker; #203 names the asymmetric error-handling. None names
the *meta-claim*: there is no shared `SqliteCache<Schema>` template that
would centralise (a) version-marker via `user_version`, (b) doSQLite
protocol, (c) prepared-statement caching, (d) atomic-init.

**Sites:**

- `src/libstore/sqlite.{cc,hh}` — the wrapper.
- `src/libstore/local-store.cc` — `LocalStore::State`.
- `src/libstore/nar-info-disk-cache.cc` — `NarInfoDiskCacheImpl::State`.
- `src/libfetchers/cache.cc` — `Cache::State`.
- `src/libexpr/eval-cache.cc` — `AttrDb::State`.
- `src/libstore/include/nix/store/sqlite.hh` — `SQLite` /
  `SQLiteSettings`.

**Proposed abstraction:** an `SqliteCache<SchemaSql, UserVersion>`
template providing constructor (path, settings), version check via
`PRAGMA user_version`, doSQLite protocol (with consistent read+write
guarding), and a `prepare<>` accessor for static prepared statements.
Each concrete cache provides only `using Stmts = ...` plus its own
methods. Compounds with #168, #201, #203 by providing the substrate
on which all three become single-line policy choices.

**Why it was missed:** #168 (magic numbers) and #201 (schema version)
sit in section 17 (cross-cutting) and 20 (eval-core); #203 (read/write
asymmetry) sits in 20 too. The *cache base class* is implicit but
unnamed.

**Effort:** medium.

**Cross-references:** compounds with #54 (SQLiteSettings::useWAL),
#71 (TTL+present-bit shape), #168, #201, #203.

---

### N14. The `expectedX` / `runningX` / `nrX` worker counters are 14 parallel uint64s

**Pattern:** the `Worker` class (libstore/build) carries 14 `uint64_t`
counters tracked via `MaintainCount<uint64_t>` smart-RAII members on
goals:

```
expectedBuilds, doneBuilds, failedBuilds, runningBuilds,
expectedSubstitutions, doneSubstitutions, failedSubstitutions,
runningSubstitutions, expectedNarSize, doneNarSize, expectedDownloadSize,
doneDownloadSize
```

(plus `nrLocalBuilds`, `nrSubstitutions`)

Each goal manages a `unique_ptr<MaintainCount<uint64_t>>` member for
each counter it influences:

- `DerivationGoal::mcExpectedBuilds`.
- `DerivationBuildingGoal::mcRunningBuilds`.
- `PathSubstitutionGoal::maintainExpectedSubstitutions`,
  `maintainRunningSubstitutions`, `maintainExpectedDownload`,
  `maintainExpectedNar`.

Cand. #129 (replace `MaintainCount` with `Finally`) names the type;
cand. #33/#47 names the duplication of `doneSuccess`/`doneFailure`
that uses these. None names the *progress reporter* abstraction:

The 14 counters are essentially three orthogonal axes (expected /
running / done) × two activity types (build / substitution) × two units
(count / size). Plus `failedX`. The `MaintainCount<uint64_t>` shape
treats them as opaque counters; the progress reporter
(`worker.updateProgress()`) reads them all en bloc.

**Sites:**

- `src/libstore/include/nix/store/build/worker.hh` — the 14 counters.
- `src/libstore/include/nix/store/build/derivation-goal.hh`,
  `derivation-building-goal.hh`, `substitution-goal.hh`,
  `drv-output-substitution-goal.hh` — the per-goal `MaintainCount`
  members.
- `src/libstore/build/worker.cc::updateProgress` — the en-bloc reader.

**Proposed abstraction:** a `ProgressReporter` collaborator owning a
`std::array<atomic<uint64_t>, N>` plus a typed enum-keyed
`incCount(Stage::Build, Phase::Running)` API. Each goal holds a
`ProgressReporter::Activity` RAII handle that adds-on-construct,
removes-on-destruct. Same shape as `MaintainCount` but typed and
centralised. Compounds with #129 (`MaintainCount` deletion target).

**Why it was missed:** #33, #47, #129 each saw one slice of the
`MaintainCount` smell. None looked at the 14 counters as a single
group.

**Effort:** medium.

**Cross-references:** compounds with #33, #47, #129.

---

### N15. The "version tag + struct" wire pattern recurs three times

**Pattern:** the codebase has three places where a wire format encodes
a *version-tag-then-payload* pattern, each implemented by hand:

1. `WorkerProto::Version` is `(uint16_t major, uint16_t minor,
   FeatureSet)` with **partial ordering**.
2. `ServeProto::Version` is `(uint8_t major, uint8_t minor)` with
   **total ordering** and no FeatureSet.
3. `nixSchemaVersion = 10` (LocalStore on-disk integer file) plus the
   `SchemaMigrations` named-migration table.
4. Per-output-cache schema versions encoded in *filenames* (#168, N13).
5. `expectedJsonVersionDerivation = 4` (derivation JSON serialiser).
6. `narVersionMagic1 = "nix-archive-1"` (a string-magic).
7. `exportMagic = NIXE` (a 4-byte tag).

Each follows the same pattern (encode a version, validate it, route
accordingly), but they share no `Versioned<Body>` base. Cand. #179
notes version-gating in `performOp` is hand-rolled at six sites with
diverging styles; the candidate stops one layer short of saying
"versioning itself is hand-rolled across the codebase."

**Sites:**

- `src/libstore/include/nix/store/worker-protocol.hh`,
  `serve-protocol.hh`, `common-protocol.hh`.
- `src/libstore/local-store.cc` — `nixSchemaVersion` + `upgradeDBSchema`.
- `src/libutil/archive.cc` — `narVersionMagic1`.
- `src/libstore/export-import.cc` (or equivalent) — `exportMagic`.
- `src/libstore/derivations.cc` — JSON version field.
- The five SQLite cache schemas (N13).

**Proposed abstraction:** a `WireVersion<Cmp>` template where `Cmp` is
total or partial ordering policy, with serialisers; plus a
`SchemaVersion<UserVersion>` for SQLite caches; plus a
`MagicHeader<Tag>` for fixed-tag wire formats. Three small templates
collectively replace the hand-coded version-tag-then-payload protocol.

**Why it was missed:** #179 noticed version-gating *inside* one
protocol. The cross-cutting observation that the codebase has 7+
versioned wire formats with no shared base is missing.

**Effort:** medium.

**Cross-references:** compounds with #168, #179, #201, N13.

---

### N16. Test boilerplate: `JsonCharacterizationTest<T>` mirrors a missing production reflection

**Pattern:** the test-side `JsonCharacterizationTest<T>` and
`VersionedProtoTest<Proto>` produce read+write tests from a `T` value
plus a golden master file. The macro `VERSIONED_CHARACTERIZATION_TEST`
writes 4 separate `TEST_F`s (`name_read`, `name_write`,
`name_json_read`, `name_json_write`) — implying that for every type
that the protocol/JSON layer serialises, there *exist* at least 4
tests but the harness only fires when the cataloger remembers to
write `VERSIONED_CHARACTERIZATION_TEST(...)`.

The dual side: in production, every type that has a JSON serialiser
*also* tends to have a wire serialiser (the `WorkerProto`/`ServeProto`
specialisations). The cross-cutting observation: production has
hand-written `Serialise<T>` for both wire protocols and `to_json`/
`from_json` for JSON; the test side has parallel tests; *neither
side* uses reflection. With `boost::describe` (not currently in tree;
cand. #9, #176 reference it) every (type, format) pair could be
auto-derived.

The test infrastructure mirrors the missing production reflection
exactly: `JsonCharacterizationTest<T>` is a rectangular grid (T ×
{read, write}); `VersionedProtoTest<Proto>` is (Proto, T, Version) ×
{read, write}. Both templates are *evidence* that the pattern is
mechanical and the implementations of `to_json`/`Serialise` are
hand-coded boilerplate.

**Sites:**

- `src/libutil-test-support/include/nix/util/tests/{characterization,
  json-characterization}.hh`.
- `src/libstore-test-support/include/nix/store/tests/protocol.hh`.
- `src/libstore-tests/serve-protocol.cc`, `worker-protocol.cc` (130+
  tests via `VERSIONED_CHARACTERIZATION_TEST`).
- For *every* type involved, the production sides are in
  `src/libstore/build-result.cc::adl_serializer`,
  `path-info.cc::adl_serializer`, `realisation.cc::adl_serializer`,
  plus the matching `WorkerProto::Serialise<T>` /
  `ServeProto::Serialise<T>` specialisations.

**Proposed abstraction:** introduce `boost::describe` (header-only,
already mentioned by #9 and #176), use it for the bulk of (T × format)
pairs. The catalog's #9 names this as a cleanup; the cross-cutting
observation here is the *test side* makes the same shape, so adopting
reflection is a 2x win (reduces both production *and* test
duplication). Once production is generated, tests can drive from
typelists directly.

**Why it was missed:** test code was out of scope for the per-shard
catalog (the README explicitly excludes tests). But test-side
duplication is *evidence* of production-side patterns, and several
test patterns map 1:1 to production patterns this way.

**Effort:** medium-large (introduce `boost::describe`; migrate
record-shaped types).

**Cross-references:** compounds with #9 (JSON adl_serializer
scaffolding); evidence for #176, #182 reflection refactoring.

---

### N17. The "leaked Sync<T*>" idiom recurs three times for SIOF avoidance

**Pattern:** the codebase uses a "leaked global Sync<T>" pattern in
three places to dodge static-init / dtor order issues:

1. `getFileTransfer()` — `static auto * const _fileTransfer = new
   Sync<std::shared_ptr<curlFileTransfer>>;` (cand. #147).
2. `windowSize` (terminal.cc) — leaked unique_ptr; cand. #148 (and the
   #148 *attempted-cleanup-was-reverted* note documents the exact
   reason: dtor ordering vs `~Logger`).
3. `getInterruptCallbacks()` — same hazard class per the #148 note.

Plus the SIOF mitigation patterns:

4. The `Counter::enabled` static (counter.hh) reads `NIX_SHOW_STATS`
   once at startup.
5. `nix::regex_init_once` (a `std::call_once` in `EvalState`'s ctor; #212).

These are all manifestations of "process-wide singletons that need to
outlive other process-wide singletons." Cand. #148 documents the
hazard and cand. #147 documents the `getFileTransfer` slot. Neither
names that the *fix* is the same: don't use process-wide singletons
when a per-`Store` / per-`EvalState` reference would do (DI). And when
DI is impractical — as for `windowSize` (the *terminal* is genuinely
process-wide), `getInterruptCallbacks` (signals are process-wide), and
`logger` (cand. #143) — wrap the leak in a small `LeakedSingleton<T>`
helper that documents the pattern and the rationale.

**Sites:**

- `src/libstore/filetransfer.cc` — `_fileTransfer`.
- `src/libutil/terminal.cc` — `windowSize`.
- `src/libutil/signals.cc` — `getInterruptCallbacks`.
- `src/libutil/logging.cc` — `logger` (cand. #143; not leaked, but
  same SIOF hazard).

**Proposed abstraction:** a `LeakedSingleton<T>` template (or a
documented coding rule) plus a clear default to *not* use it; every
new use must justify why DI is impractical. Cand. #147 already
proposes per-Store ownership for `getFileTransfer`; cand. #143
proposes per-`EvalState`/per-`Store` `LoggingContext` for `logger`.
This candidate is a meta-rule above those: when the singleton is
genuinely process-wide, name the pattern.

**Why it was missed:** the catalog has #143, #147, #148 each in their
own per-shard context. The cross-cutting rule is undocumented.

**Effort:** small (helper + comment) / medium (rationalise existing
sites).

**Cross-references:** compounds with #143, #147, #148.

---

### N18. The 14+ `_NIX_TEST_*` env vars are the bottom of an iceberg of "hidden config"

**Pattern:** cand. #146 names the 14-15 `_NIX_TEST_*` / `_NIX_FORCE_*`
env-var protocol. Two adjacent observations:

(a) The *production* code has its *own* env-var protocol that the
catalog doesn't tabulate. From a quick `getEnv("...")` scan:
`NIX_SHOW_STATS`, `NIX_PROFILE`, `NIX_PATH`, `NIX_REMOTE`,
`NIX_BUILD_HOOK`, `NIX_LOG_FD`, `NIX_NICE_LEVEL`, `NIX_USER_CONF_FILES`,
`NIX_CONF_DIR`, `NIX_DAEMON_SOCKET_PATH`, `NIX_CONFIG`, `XDG_CACHE_HOME`,
`XDG_DATA_HOME`, `XDG_CONFIG_HOME`, `HOME`. None of these are
declared in any settings struct; they live as ad-hoc `getEnv` calls
sprinkled throughout. (Many *correspond* to settings — `NIX_PATH` →
`Setting<Strings> nixPath` — but the env-var → setting mapping is
hand-coded too.)

(b) The catalog's section 15 (globals/settings) has 19 *config*
structs. The env-var-only configs are an additional 14+ knobs that
*should* be in the settings system but aren't. So the real per-process
configuration surface is `19 * (avg 5) + 14` ≈ 100+ knobs, of which
the catalog so far names ~95.

**Sites:**

- All env-var sites: `src/libstore/globals.cc`, `src/libutil/users.cc`,
  `src/libutil/environment-variables.cc`, `src/libfetchers/git.cc`,
  `src/libstore/http-binary-cache-store.cc`, etc.

**Proposed abstraction:** a `Setting<T>` flag `EnvVarOnly` that
declares an env-var-driven setting alongside its config (would land
*after* cand. #136's two-phase Setting refactor). Each existing
`getEnv("FOO")` call site becomes a one-liner reading
`fooSetting.get()`. The env-var-only settings are auto-documented
alongside the rest. The 14 `_NIX_TEST_*` and the 14+ production
`NIX_*` env-vars become one tabulated, documented surface.

**Why it was missed:** #146 saw the test side; the production side
was not enumerated. The catalog's general focus on `Setting<T>` /
`Config` made env-var-only knobs invisible.

**Effort:** medium (enumerate + migrate); large (full settings
unification).

**Cross-references:** subsumes #146; compounds with #136, #144.

---

### N19. The `EvalState::*` "primop helpers" cluster matches a pattern: "context object too big"

**Pattern:** cand. #210 names the 13 (or 20) `EvalState::*` methods
that should be a `PrimOpHelpers` collaborator. The pattern recurs
elsewhere in the codebase:

- `Worker` (libstore/build) carries the 14 progress counters (N14),
  the six per-goal-kind weak-pointer maps (#150), the `Waker` self-pipe
  (#152), the goal scheduling queue, and the `dynamic_pointer_cast` chain
  in `removeGoal` (#151).
- `Store` (the abstract base) carries 30+ virtual methods spanning
  capability traits (`addToStore`, `signPathInfo`, `repairPath`, ...);
  cand. #23 names the `unsupported` smell.
- `LocalStore` carries the SQLite layer plus the GC layer plus the
  upgrade-schema layer plus the path-canonicalisation layer.
- `Args` (libutil) carries flag parsing plus positional parsing plus
  doc generation plus JSON conversion.

The catalog has #23 (`unsupported` → capability traits), #210
(EvalState primop helpers), #176 (daemon `performOp` switch), #169
(EvalState friend list), #217 (EvalState friend list, identifier
breakdown). These are all *symptoms* of one pattern: *context types
that have grown to the point of needing decomposition into
collaborators*.

A meta-section in the catalog could enumerate these "too big" types
and rationalise them together, rather than treating each as its own
candidate.

**Sites (the fat types):**

- `src/libexpr/include/nix/expr/eval.hh` — `EvalState`.
- `src/libstore/include/nix/store/build/worker.hh` — `Worker`.
- `src/libstore/include/nix/store/store-api.hh` — `Store`.
- `src/libstore/include/nix/store/local-store.hh` — `LocalStore`.
- `src/libutil/include/nix/util/args.hh` — `Args`.
- `src/libutil/include/nix/util/configuration.hh` — `Config`.

**Proposed abstraction:** one new catalog section named "fat types
and their collaborators" enumerating the targets and the
collaborator-decomposition direction for each (with a per-target
checklist of what must move). Per-target work is what the existing
candidates already specify; what's missing is the *rationale and
sequencing*.

**Why it was missed:** each candidate was scoped to its shard. The
sequencing question — "if you do #210 first, you unblock #169; if you
do #150/#151 first, you unblock #170" — needs an across-shards view.

**Effort:** structural (it's a meta-coordination, not a refactor).

**Cross-references:** subsumes #23, #150, #151, #169, #176, #210, #217;
compounds with #144 (settings architecture is another fat-types
pattern).

---

### N20. The `InternalType`/`nValueType`/`nValueType`-rendering trio replicates a tag-table

**Pattern:** the `Value` discriminated-union has two enums:

- `InternalType` (the storage-level tag; ~15 values per layout).
- `nValueType` (the "external" public tag; 9 values: `nThunk`, `nInt`,
  `nFloat`, `nBool`, `nString`, `nPath`, `nNull`, `nAttrs`, `nList`,
  `nFunction`, `nExternal`, `nFailed`).

Five renderers each switch on `nValueType`:
- `Printer::print` (cand. #36).
- `printAmbiguous` (#36).
- `printValueAsJSON` (#36, #200).
- `printValueAsXML` (#36).
- `Expr::show` (#36).

Plus the `force*` family (cand. #211) switches on `InternalType` /
`nValueType` (`forceInt`, `forceFloat`, `forceBool`, `forceFunction`,
`forceString`, `forceAttrs`, `forceList`, `forceStringNoCtx`).

Plus the 8 "type predicate" primops (`prim_isInt`, `prim_isFloat`, ...
cand. #66) switch on the same.

Plus `showType` (in `eval.cc`).

That's *eleven* `nValueType`-driven dispatch sites with no shared
table. The natural representation is a `static constexpr` table of
`(nValueType, name, accessor, jsonCoerce, xmlCoerce, printNixForm)`
tuples — "type traits for value types" — that the eleven dispatch
sites consume. This is the same shape as cand. #66 ("table-driven
type predicates") and cand. #211 ("template `forceTyped<T>`") and
cand. #36 ("shared visitor scaffold for renderers") in unified form.

The catalog has the three candidates separately; the unifying
abstraction (a `value-type-traits.hh` table) is missing.

**Sites:**

- `src/libexpr/include/nix/expr/value.hh` — `nValueType`,
  `InternalType`, `NIX_VALUE_STORAGE_FOR_EACH_FIELD` X-macro.
- `src/libexpr/print.cc`, `print-ambiguous.cc`, `value-to-json.cc`,
  `value-to-xml.cc`, `nixexpr.cc`.
- `src/libexpr/eval.cc` — `forceInt`/`forceFloat`/`forceBool`/`forceFunction`/
  `forceString`/`forceStringNoCtx` (#211).
- `src/libexpr/primops.cc` — `prim_isInt`/`prim_isFloat`/... (#66).

**Proposed abstraction:** a single table:
```
constexpr ValueTypeTraits valueTypeTraits[] = {
    { nInt, "an integer", &Value::integer, ... },
    { nFloat, "a float", &Value::fpoint, ... },
    ...
};
```
Then `forceTyped<nInt>(...)` (#211), `prim_isInt` (#66), and the per-
renderer switches (#36) all consume it.

**Why it was missed:** #36, #66, #211 each saw one slice. The
cataloger of #211 noticed it compounds with #65 (one layer up); the
cataloger of #36 noticed it compounds with #110 (the seen-set). None
saw all three as instances of the same "table missing" pattern.

**Effort:** medium.

**Cross-references:** subsumes #36, #66, #211; compounds with #110,
#200.

---

### N21. The "Counter" pattern is not isolated to libexpr

**Pattern:** cand. #205 names `Counter` (libexpr) — cache-line-aligned
atomic uint64 with `enabled` flag, used 13 times per `EvalState`.
Outside libexpr, the same conceptual entity exists with different
implementations:

- `MaintainCount<T>` (libutil) — RAII + plain `++`/`--` on `T &`,
  used 14 times in libstore/build (N14).
- `expectedBuilds`, `runningBuilds`, etc. — plain `uint64_t` members
  on `Worker` updated *via* `MaintainCount`.
- `Logger`'s `nextId` — `std::atomic<uint64_t>` (#143).
- Each store's bytes-received/bytes-sent counters
  (`LegacySSHStore::ConnectionStats`).
- `SQLite::stmt.use().exec()` counters (no central place).
- `nrLocalBuilds`, `nrSubstitutions` on `Worker` — also plain.

There's no shared "counter" abstraction. Each subsystem rolls its own.
The catalog has #205 (libexpr `Counter`), #129 (libutil
`MaintainCount`), #143 (logging globals), #14/N14 (worker counters);
the meta-claim "the codebase has multiple counter patterns; pick one"
is missing.

**Sites:**

- `src/libexpr/include/nix/expr/counter.hh` — `Counter`.
- `src/libutil/include/nix/util/util.hh` — `MaintainCount<T>`.
- `src/libstore/include/nix/store/build/worker.hh` — the 14 counters.
- `src/libstore/include/nix/store/legacy-ssh-store.hh` —
  `ConnectionStats`.
- `src/libutil/logging.cc` — `nextId`.

**Proposed abstraction:** a `MetricsRegistry` (or `Counter<Tag>`) per
subsystem plus a unified `MaintainMetric<Counter &>` RAII handle.
Replaces the three concurrent patterns (`Counter`, `MaintainCount<T>`,
ad-hoc atomics) with one type. Compounds with cand. #205 (libexpr-only
Counter) by generalising it across subsystems.

**Why it was missed:** counters are in shards 03 (libutil),
10 (libstore-build), 11 (libexpr), 03 (logging). The slice-by-shard
view doesn't surface the meta-pattern.

**Effort:** medium.

**Cross-references:** compounds with #129, #143, #205, N14.

---

### N22. The four `MakeBinOp` AST nodes share fate with `MakeError` and `MAKE_WRAPPER_CONSTRUCTOR`

**Pattern:** the catalog has #20 (`MAKE_WRAPPER_CONSTRUCTOR` for variant
types), #105 (`MakeBinOp`/`MakeBinOpMembers`), #18 (`MakeError`/
`CloneableError`). Three macro families spanning shards 03 (libutil),
05/06 (libstore-core), 12 (libexpr-parse). Each is the same
*kind* of entity: a `#define name(parent, ...) struct name : parent {
... }` macro that emits a class declaration with conventional members.

The catalog candidates the macro families separately. The
cross-cutting candidate is "nix-tree macros that emit a struct
declaration" — this is the *category*, and the codebase has 4-5 of
them:

- `MakeError(name, parent)` — exception declaration.
- `MakeBinOp(name, sym)` / `MakeBinOpMembers(name, sym)` — AST
  binop declaration.
- `MAKE_WRAPPER_CONSTRUCTOR(T)` — variant-shaped wrapper.
- `COMMON_METHODS` — `Expr*` standard virtual methods.
- `NIX_DECLARE_CONFIG_SERIALISER(T)` — `BaseSetting<T>::parse` /
  `to_string` declarations.

C++23's deducing-this enables CRTP-shaped replacements for several of
these without functional change. `MakeError` is already noted by
#18; `MakeBinOp` by #105. The remaining ones are not yet candidate-
ised:

- `COMMON_METHODS` is not in the catalog as a refactor target.
- `NIX_DECLARE_CONFIG_SERIALISER` is mentioned in the inventory
  (shard 03) but not in candidates.

**Sites:**

- `src/libutil/include/nix/util/error.hh` — `MakeError`.
- `src/libexpr/include/nix/expr/nixexpr.hh` — `MakeBinOp`,
  `MakeBinOpMembers`, `COMMON_METHODS`.
- `src/libstore/include/nix/store/content-address.hh`,
  `derived-path.hh`, `outputs-spec.hh`, `path-with-outputs.hh`,
  `derivations.hh`, `derivation-options.hh`, `realised-path.hh`,
  `build-result.hh`, `realisation.hh` — `MAKE_WRAPPER_CONSTRUCTOR`
  call sites.
- `src/libutil/include/nix/util/configuration.hh` —
  `NIX_DECLARE_CONFIG_SERIALISER`.

**Proposed abstraction:** a meta-section: "macros that emit struct
declarations". Each fitness-tested for replacement by CRTP /
deducing-this / concept-constrained template. Compounds the existing
candidate triplet (#18, #20, #105) with the missing two (`COMMON_METHODS`,
`NIX_DECLARE_CONFIG_SERIALISER`).

**Why it was missed:** each macro is in its own shard's per-shard
candidates. The category "macros that emit declarations" has no home.

**Effort:** medium.

**Cross-references:** subsumes part of #18, #20, #105, N4 (settings X-
macro); compounds with #43.

---

### N23. Per-protocol opcode dispatch and per-`Store` virtual surface mirror each other

**Pattern:** there are two parallel dispatcher patterns, both with
the same shape:

(A) Daemon side: `WorkerProto::Op` → switch in `daemon.cc::performOp`
(cand. #176, 31 cases). Each case reads typed args from the wire,
calls `store->method(...)`, writes typed response.

(B) Client side: `RemoteStore::*` methods (cand. #182, ~25 methods).
Each opens a connection, writes the opcode, writes typed args, reads
typed response.

(C) Local side: `LocalStore::*` and `BinaryCacheStore::*` — same set
of methods as (A) and (B) consume.

Each site (A, B, C) hand-codes the same logical operation 3 times.
Each operation has a "wire protocol" half (in A, B) and a "local
implementation" half (in C). The catalog has #176 (the daemon switch),
#182 (the client side), and the implicit per-`Store` virtual surface
(#23 + #134). The *meta-claim* — "these three sites are different
projections of one record-of-operations" — is missing.

**Sites:**

- `src/libstore/daemon.cc::performOp` (A).
- `src/libstore/remote-store.cc` (~25 methods) (B).
- `src/libstore/include/nix/store/store-api.hh` — virtual surface (C).
- `src/libstore/local-store.cc` (concrete C).
- `src/libstore/binary-cache-store.cc` (concrete C).

**Proposed abstraction:** a single `OpRegistry` (declarative table of
opcodes; each entry has args type, return type, version constraint,
trust constraint, server-side handler in terms of `Store::method`).
Both client and daemon are then *generated* from the table. The
`Store` virtual surface remains, but the wire-protocol code is
generated. This is the single largest *structural* win in the
codebase if achievable; it ties together cand. #176, #177, #178,
#179, #181, #182 into one phased refactor.

**Why it was missed:** #176, #182 are in section 18 (daemon protocol
audit). They're framed as separate dispatch refactors. The
table-driven *unification* across both sites needs a meta-section.

**Effort:** structural.

**Cross-references:** subsumes #176, #182 partially; compounds with
#177, #178, #179, #181, N12.

---

## Pattern clusters worth a section of their own

The existing 21 sections are organised by shard or library. The
following cross-cutting clusters touch enough sections that they
deserve their own section in a future inventory revision:

### Cluster A: Concurrent state and globals (cross-cutting)

The catalog's section 15 (globals/settings) is the closest fit but is
focused on *settings*-globals. There's a parallel set of *non-settings*
process-wide state that the catalog touches at the per-candidate
level:

- `getFileTransfer()` curl pool (#147).
- `windowSize` terminal cache (#148, INVALID-as-fix).
- `getInterruptCallbacks()` (#148 mentions).
- `logger` global (#143).
- `drvHashes` per-process map (#139).
- `Counter::enabled` (cand. #205).
- `PosTable::origins_` (`Sync<map>`).
- The `Sync<map>` vs `concurrent_flat_map` choice (N1).
- `ThreadPool` vs `nix::asio` vs `Goal::Co` (N8).

These are not settings; they are *runtime state* tied to the process
lifetime. A "global runtime state" section would name them and the
DI direction together, with a sequencing plan.

### Cluster B: Type erasure and variant-shaped types

The catalog has #20 (`MAKE_WRAPPER_CONSTRUCTOR`), #94 (`BuiltPath`/
`SingleBuiltPath`), #102 (Unkeyed/Keyed pair), #194 (`LookupPath::Elem`).
All are *variant types over a fixed alternative-set*. The ad-hoc
typedefs (`SingleDerivedPath`, `DerivedPath`, `OutputsSpec`,
`ContentAddressMethod`, `ContentAddressWithReferences`, ...) total
~13 in the libstore-core shard alone. A `TaggedUnion<Tag,
Alternatives...>` template is the obvious abstraction; #20's
validation already names it.

A "variant-shaped types" section would gather these and the surrounding
serialisation/parsing/printing duplication.

### Cluster C: SourceAccessor and Store factories

Cand. N7 above identifies that `SourceAccessor`-opening code is
duplicated; #11/#12 identifies that "store wrapper" code is duplicated.
A "factory consolidation" section would note:

- `openStore(uri)` already exists, parameterised by registered
  `StoreFactory`.
- `openAccessor(spec)` does not exist (N7).
- `openInputScheme(scheme, attrs)` does not exist (it lives behind
  `Input::fromAttrs`).
- `RegisterCommand`/`RegisterPrimOp`/`RegisterBuiltinBuilder` are
  parallel registries (N3).

The shared theme is "registry of factories"; consolidating across all
five would unify the registration pattern. N3 names this.

### Cluster D: Settings-tied configuration

Cand. N4 points out the 19 settings structs are a Cartesian product;
cand. #144 names the same. Together with the `_NIX_TEST_*` env-var
protocol (N18) and the per-`Setting<T>` self-registration (#136),
this is the *configuration* axis of the codebase. A "configuration
architecture" section would gather:

- The 19 `*Settings` structs.
- The `_NIX_TEST_*` and `NIX_*` env-vars.
- The 4-5 SQLite cache schema versions encoded in filenames (N13,
  N15).
- The 11+ trust-check and version-check ad-hoc dispatches (#178, #179).

## C++23 / Boost uplift series

The catalog has scattered C++23 / Boost uplift candidates (#121, #122,
#125, #129, #130, #160, #172). They form a coherent staged series:

### Stage U1 (libc++ 16 cleanup; cand. #121)

- Replace 17 `// TODO libc++ 16` workarounds with defaulted
  `operator<=>`.
- Effort: small. Risk: minimal (workaround removal).
- Files: `derived-path-map.{hh,cc}`, `content-address.hh`,
  `derivations.hh`, `derived-path.{hh,cc}`, `outputs-spec.hh`,
  `nar-info.hh`, `built-path.{hh,cc}`, `nix/profile.cc`.

### Stage U2 (stdlib uplift; cand. #130)

- Delete `append` shim → use `std::ranges::append_range`.
- Effort: trivial.
- File: `libutil/include/nix/util/util.hh`.

### Stage U3 (Finally-as-MaintainCount; cand. #129)

- Replace `MaintainCount<T>` stack-scoped uses with `Finally`.
- Defer Goal-hierarchy `MaintainCount` (heap-scoped) per the
  validation note.
- Effort: small (stack sites).
- Files: `nix/verify.cc`, `libstore/store-api.cc`, `libexpr/primops.cc`
  (`primTryEval`).
- Compounds with N14 (full counter rationalisation; medium).

### Stage U4 (`std::unreachable()` for dead-after-exhaustive switches; cand. #172)

- 4 of the 64 `nix::unreachable()` sites are dead after exhaustive
  switch — convert to `std::unreachable()`.
- Effort: trivial per site.

### Stage U5 (`std::move_only_function`; cand. #122)

- Replace `nix::fun<Sig>` with `std::move_only_function<Sig>` at fixed
  call sites.
- For shared-callback cases (e.g. `stackOverflowHandler`,
  `Callback`-copies), continue to use `std::function`.
- Effort: medium (~60+ sites).

### Stage U6 (`gsl::not_null` evaluation; cand. #123, INVALID-as-recommended)

- Per validation note: keep `nix::ref<T>`, only drop `bad_ref_cast`
  in favour of `std::bad_cast`.
- Effort: trivial.

### Stage U7 (Boost `concurrent_flat_map` vs `Sync<map>` rationalisation; cand. N1)

- Rubric + `getOrInsertConcurrent` helper.
- Migrate `Sync<map>` writers that don't need ordering.
- Effort: medium.

### Stage U8 (`boost::asio::awaitable` migration; cand. #160, N8)

- Replace `Goal::Co::SuspendAwaiter` and `ChildEventAwaiter` with asio
  primitives, keeping the bespoke `final_awaiter` (per #160 validation).
- Migrate `Callback<T>` consumers to `awaitable<T>` (N8).
- Effort: structural; needs prior #150/#151 (cache rationalisation).

### Stage U9 (`std::format` migration from `boost::format` + `HintFmt`; cand. #125)

- 226 `boost::format` / `boost/format` / `HintFmt` matches.
- Effort: large (multi-PR; mechanical but huge).
- Should be done last because it touches every error message.

### Stage U10 (`boost::describe` reflection; cand. #9, #176, N12, N16)

- Introduce `boost::describe` (header-only).
- Auto-derive `nlohmann::json` adl_serializer for record-shaped types.
- Eventually auto-derive `WorkerProto::Serialise<T>` /
  `ServeProto::Serialise<T>`.
- Eventually auto-derive `*Settings` struct surface (#19, N4).
- Effort: medium-to-large, but unblocks several other refactors.

### Sequencing

```
U1 (small)   ──── U4 (trivial)
   │              │
   v              v
U2 (trivial) ──── U6 (trivial)
   │              │
   v              v
U3 (small)        U7 (medium)
   │              │
   v              v
U5 (medium) ──── U10 (medium-large) ──── (unblocks parts of #9, #176, N12, N16, N4)
                  │
                  v
U8 (structural; depends on #150/#151)
   │
   v
U9 (large; last, low priority)
```

The recommended order is U1 → U4 → U2 → U3 → U6 → U5 → U7 → U10 → U8 → U9.
U10 is the highest-leverage middle-of-graph step; it unblocks
generation-of-protocol-code and generation-of-JSON-serialisers.

## Big-picture architectural debt

The catalog touches several architectural themes at the per-candidate
level but doesn't articulate them as named architectural debt. This
section names them.

### A1. No `EvalContext` separating eval state from eval *settings* and primop helpers

Cand. #131, #132, #133, #138, #209, #210, #212 each touch a slice of
this. The architectural problem: `EvalState` is currently
`{settings + state + primop helpers + eval cache + ... }` in one
class. The fix is splitting on the natural seams (cand. #210's
`PrimOpHelpers`; cand. #209's `EvalDebugSettings`; cand. #138's
`readOnlyMode → StoreConfig`). The catalog has the parts; the
architectural rationale is:

- `EvalState`: "the language interpreter's mutable state" (only).
- `EvalSettings`: "what does the language do" — language-level knobs
  only (cand. #209).
- `EvalDebugSettings`: "how does the evaluator report itself" (cand.
  #209).
- `PrimOpHelpers`: "operations primops perform that aren't core eval"
  (cand. #210).
- `EvalCacheManager`: "the SQLite memoisation layer" (currently
  baked in; could be a collaborator).

### A2. No `IOContext` / `Executor`

Cand. #147 hints at this for the curl thread; #160 hints at it for
the Goal coroutine machinery; N8 unifies both. The architectural
direction: a process-wide `IOContext` (`asio::io_context` wrapped) that
owns the curl thread, the worker poll loop, and the Goal coroutine
scheduler. Per-Store / per-EvalState consumers borrow from it.

### A3. No "store wrapper" abstraction

Cand. #11, #12 hint at this. `LocalOverlayStore`, `RestrictedStore`,
`MountedSSHStore` (sibling of `RemoteStore`+`LocalFSStore`) all have
"delegates to inner Store" semantics; there is no
`DelegatingStore<Inner>` base. This is N7's store-side counterpart.

### A4. No `LoggingContext`

Cand. #143 hints at this. The four orthogonal logging globals
(`verbosity`, `nextId`, `curActivity`, `logger`) are one logical
subsystem. Move to per-`EvalState`/per-`Store` reference, with TLS
fallback for backward compat.

### A5. No unified Configuration architecture

Cand. #134, #135, #136, #144, N4, N18. The 19 `*Settings` structs
plus the env-var protocol are one logical "configuration" subsystem.
The keystone is #136 (split `Setting<T>` self-registration into a
two-phase `Config::registerSettings()`).

### A6. No declarative protocol schema

Cand. #176, #177, #178, #179, #181, #182, N12, N23. The daemon
protocol is hand-coded on both sides. A declarative schema (one
table per opcode) would generate both sides.

### A7. No `SignedFingerprint` / `Keyed` / `Renderable` traits

Cand. #102, #104, N2, N5. The three "format-this-thing" patterns
(fingerprint, render, parse) recur on every variant-shaped type with
no shared trait.

### Architectural debt summary

Of the 217 candidates, roughly ~60 are *symptoms* of these seven
architectural themes. A future inventory revision could group them
under named architectural roadmap entries rather than per-shard
sections, and use the per-shard sections as the *to-do list per
theme*.

## Redundant abstractions

Cases where the codebase has 2+ in-tree abstractions solving
overlapping problems and the choice is not consistent:

### R1. `nix::Sync<T>` vs leaked-pointer + `std::mutex`

`Sync<T>` (libutil) is the canonical mutex+state pair. But three
sites use `static auto * const _x = new Sync<T>;` (leaked) instead
(cand. #147, #148; N17). Plus two sites use `Sync<map>` where
`boost::concurrent_flat_map` would do (N1). The choice rule should
be:
- ordered iteration / cv wait → `Sync<T>`.
- read-mostly flat lookup → `boost::concurrent_flat_map`.
- process-wide singleton with SIOF concerns → leaked `Sync<T>` with
  `LeakedSingleton<T>` wrapper (N17).

### R2. `nix::ref<T>` vs `gsl::not_null<shared_ptr<T>>` vs `std::shared_ptr<T>`

Cand. #123 names this. The current state: `ref<T>` is in tree;
`gsl::not_null` is not; `std::shared_ptr<T>` is used wherever ref<T>
is unsuitable. Rule (per #123 validation): keep `ref<T>`, drop
`bad_ref_cast` in favour of `std::bad_cast`.

### R3. `boost::concurrent_flat_map` vs `Sync<unordered_map>` vs `Sync<map>`

Cand. N1. Choose explicitly per-site; document the rule.

### R4. `Pool<R>` vs hand-rolled connection cache

Cand. #16: `LegacySSHStore::connections` is a `Pool<Connection>`;
`RemoteStore::connections` is also a `Pool<Connection>`; the
`getFileTransfer()` curl thread is a singleton (N17, #147). Three
shapes for "pool of clients to a remote service". Cand. #16 partially
unifies #16 + #184; N10 generalises further.

### R5. `nix::fun<Sig>` vs `std::function<Sig>` vs `std::move_only_function<Sig>` vs templated functor

Cand. #122. Currently, `fun<Sig>` is the codebase-wide non-nullable
wrapper; per the validation note, the right targets are: stable-call-
site lambdas → templated parameter; one-shot move-only → `std::move_only_function<Sig>`;
shared/copyable → `std::function<Sig>`. Currently mixed.

### R6. `std::list<std::string>` vs `std::vector<std::string>` vs `boost::container::small_vector<std::string, N>`

Cand. #171. `Strings` is `std::list<std::string>` (the wrong default
for token streams). Some sites use `boost::container::small_vector`
explicitly (e.g. `concatStringsSep`'s 64-element variant). The codebase
has three competing string-list types with no documented choice rule.

### R7. `Callback<T>` vs `boost::asio::awaitable<T>` vs `Goal::Co` vs `promise/future`

Cand. N8. Pick one (`asio::awaitable<T>`).

### R8. `Counter` (libexpr) vs `MaintainCount<T>` vs raw `std::atomic<uint64_t>` vs raw `uint64_t`

Cand. N21. Four counter shapes; pick one.

### R9. `MakeError(name, parent)` vs `class X final : public CloneableError<X, BaseError>`

Cand. #18. Two error-declaration patterns; the catalog notes both
should converge on a single shape.

### R10. `nix::asio` (`boost::asio` alias) vs `std::async` vs raw `std::thread`

Cand. N8 mentions. The codebase uses `std::thread` explicitly in
many places (`local-gc.cc`, `current-process.cc`, `gc.cc`, the
`Worker` poll loop), `boost::asio::awaitable` in a few
(`HttpBinaryCacheStore::topoSortPaths`), and `std::async` not at all
that I found. The same async-discipline gap as N8.

## Summary

This pass adds **23 new candidates** (N1-N23) and identifies four
**pattern clusters worth their own section** (A-D), a coherent
**C++23/Boost uplift series** (U1-U10), seven **architectural debt
themes** (A1-A7), and ten **redundant-abstraction pairs** (R1-R10).

### Recommended new catalog sections

1. **Cross-cutting / meta-debt** — house for N1, N3, N15, N17, N19,
   N21, N23, plus existing #168, #137, #143, #147, #148.
2. **Variant-shaped types** — house for cluster B; absorbs #20, #94,
   #102, #104, #194; adds N5, N20.
3. **Macro-driven boilerplate (sub-section)** — house for N4, N22;
   compounds #18, #20, #105, #43.
4. **Configuration architecture** — house for cluster D; absorbs
   #19, #21, #134, #135, #136, #137, #144; adds N4, N18.
5. **Concurrency / async architecture** — house for clusters A and
   sub-cluster of A; absorbs #22, #160; adds N1, N8, N17, N21.
6. **Test-mirror patterns** — house for N16; new section since
   tests are out of catalog scope.

### Highest-leverage refactors (subset of new candidates)

1. **N3 (`Registry<Key, Value>` template)** — unblocks N4, N12, #86,
   #137. Effort: medium.
2. **N4 (X-macro `*Settings` structs)** — unblocks N3 partially,
   #19, #21, #137, #144. Effort: medium.
3. **N7 (`openAccessor(spec)`)** — collapses #92, complements N12,
   compounds with #11, #12. Effort: medium.
4. **N12 (declarative attribute schemas)** — unblocks N3, N23, #176,
   #181, #182. Effort: structural.
5. **N20 (`value-type-traits.hh`)** — unblocks #36, #66, #211. Effort:
   medium.
6. **U10 (`boost::describe`)** — unblocks #9, N12, N16, parts of
   N4. Effort: medium-large.

### Highest-impact corrections of catalog claims

These are noted briefly here for the adversarial-existing agent to
route. (Note: this agent's scope is *new* candidates, but a few
existing-candidate observations surfaced incidentally.)

#### Handoff to adversarial-existing agent

- **#86 ("OnStartup lambda registration pattern")** undersells the
  pattern. It calls out 5 registries; there are at least 7 (add the
  `Setting<T>` self-registration of #136 and the
  `GlobalConfig::Register` of #137 as the 6th and 7th — both
  registries, both with the same SIOF concerns). N3 is the missing
  generalisation. The original #86's "shared `Registry<Key, Factory>`
  template would consolidate" stops one step short of saying *which
  template* and *which 7 registries*. Recommend reframing #86 around
  N3.

- **#144 ("19 separate Config-derived globals")** undersells the
  duplication. The 19 structs share a *uniform shape* that is itself
  template-able (N4). The candidate's recommendation ("collapse the
  small ones") is right but doesn't name the X-macro driver. Suggest
  re-validating with N4 in view.

- **#147 ("getFileTransfer leaked singleton")** is a slice of N17 —
  the leaked-Sync pattern recurs in three places, with one
  attempted-and-reverted cleanup (cand. #148). The fix direction is
  the same in each: per-Store / per-EvalState ownership where
  practical, `LeakedSingleton<T>` wrapper otherwise.

- **#176 ("`performOp` 700-line switch")** correctly identifies the
  daemon-side switch but stops short of N12/N23: the daemon switch
  *and* the 25 `RemoteStore::*` opcode methods *and* the `Store`
  virtual surface are three projections of one logical "opcode
  schema". The right refactor is declarative; #176's "per-op `void
  handle*` member" suggestion is one step toward this.

- **#205 ("Counter cache-line-aligned alignment cost")**'s "13
  Counter members per EvalState/EvalMemory = ~832 bytes" finding
  is correct, but N21 broadens to the codebase-wide counter pattern
  which the candidate doesn't name. Cross-reference N14, N21.

- **#160 ("Goal::Co coroutine machinery")** — the validation note
  correctly preserves the bespoke `final_awaiter` but doesn't name
  the broader async-architecture question (N8: 3-4 async idioms;
  pick one).

- **#22 ("Async/sync pair pattern")** is a libutil-shaped trivial
  refactor *as written*, but the deeper observation is N8: the codebase
  has three async idioms (`Callback<T>`, `awaitable<T>`, `Goal::Co`).
  Recommend re-reading #22 in the context of N8 before committing
  to a tactical fix.

- **#143 ("Logging globals")** — the 8 `logger` reassignment sites
  cited are real, but the *DI direction* is the same as N17's
  conclusion: `LoggingContext` is the right abstraction; per-EvalState/
  per-Store reference is the right ownership; TLS-stack fallback for
  free-function callers. Compounds with N17.



