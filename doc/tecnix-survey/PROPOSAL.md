# A Lazy, Composable, Transparent SourceAccessor

This document is the constructive companion to [README.md](./README.md) (the survey). The survey diagnoses why tecnix needs eleven `__unsafeTectonixInternal*` builtins and why nixpkgs filesets pay a NAR walk on every cold evaluation. This document proposes the concrete construction that makes most of those workarounds unnecessary, transparently — 8 of the 11 tecnix builtins retire under items g+h+j+k; the remaining 3 (the dirty-zone family) require either a UX shift in tecnix or a follow-on `OverlaySourceAccessor` combinator that this proposal explicitly punts.

---

## Review status

This document has been through several rounds of adversarial review against master, governed by the rules in [README.md "Review discipline"](./README.md#review-discipline-read-before-editing-this-document-or-proposalmd) — simulate, two-process determinism, adjacent-file consumer enumeration, blast-radius, latency budget, out-of-frame alternatives. **Reviewers must read README.md's "Review discipline" section first.** Reports that do not exercise rules R1–R6 against load-bearing claims are rejected as citation audits, not reviews.

The proposal's scope: items **g**, **h**, **j**, **k**, and §5 filter identity, designed in §§2–5. Subtree-as-Input (the subtree as the unit of lock-file identity, with subtree narHash and isolated subtree storepath) is a separate RFC; §4 sketches the design; §10 records its scope. **8 of the 11 `__unsafeTectonixInternal*` builtins retire under this scope** (the manifest pair via item k; the source/path/tree triple via items g+h+j applied to a flake-input `worldRoot`; the inverse-manifest as pure Nix; the sparse-checkout-roots query as a manifest-derived expression; the git-sha exposure remains as ordinary setting introspection, out of frame). The remaining **3 (`DirtyZones`, `ZoneIsDirty`, `ZoneRoot`) require either a UX shift** (users choose between rev-pinned clean evaluation and workdir-dirty no-rev evaluation at the input declaration) **or a follow-on `OverlaySourceAccessor` combinator** (survey §8.3 #14, explicitly out of scope here per §10) that can serve clean-at-rev for non-dirty files and workdir bytes for dirty files in the same Input. §4 records the per-builtin mapping. Nixpkgs-wide migration off `${input}/sub` interpolation is the *other* concern often confused with builtin retirement; that is a release-coordination event for the nixpkgs derivation hashes and remains out of frame.

---

## Framing: the workarounds already encode the right architecture

Read the survey's §3.1 table of `__unsafeTectonixInternal*` builtins side by side with §6.6's reproduction of the fileset cache bypass and §5's twelve `SourceAccessor`-shaped asymmetries, and a single picture emerges: every one of these workarounds is **a downstream user manually filling in a missing C++ abstraction inside Nix itself.**

- `__unsafeTectonixInternalTreeSha` is a downstream computation of "the git tree-SHA of an already-mounted virtual input" — a projection that should be a first-class fetcher-cache domain alongside `treeHashToNarHash` and `gitRevToTreeHash`.
- `__unsafeTectonixInternalZoneSrc` mints a content-addressed store path keyed on tree-SHA via tecnix's own `tectonixZoneCache_` table (in tecnix's `EvalState`, an in-process `SharedSync<std::map<Hash, StorePath>>`). A cache that should be a `Projection<TreeSha, StorePath>` instance in shared cache infrastructure, not a bespoke private map.
- The fileset cache bypass at `fetch-to-store.cc` exists because there is no C++-side identity for a Nix lambda — every downstream filter-using library re-evaluates from scratch. nixpkgs's `lib.cleanSource` and `lib.fileset.toSource` are workarounds that *encode* a stable identity at the Nix-language level (constant predicate definitions, library-level memoisation) because the C++ side won't compute one.
- Tecnix's `worldRepo` setting and lazy-mount machinery exist because `mountInput`'s eager `narHash` walk does not match the workload — an evaluator that touches one subtree of a 5 GB monorepo pays for fetching, hashing, and walking the whole thing.

**The proposal is the obvious generalisation: lift the workarounds into Nix as shared, typeclass-shaped, transparent C++ machinery.** Where tecnix has a private `tectonixZoneCache_`, this proposal has `Projection<TreeSha, NarHash>` — exposed through subtree-aware `getFingerprint` (item **j**) so `addPath` against `${input}/sub` reuses the same cache row across revs. Where nixpkgs encodes filter identity at the Nix-language level, this proposal computes it C++-side from the lambda's source-region content + captured environment (§5). Where tecnix opts into lazy fetching via downstream settings, this proposal makes the laziness transparent through a custom libgit2 transport that speaks protocol-v2-with-`filter blob:none` (item **h**). Where tecnix maintains a per-`EvalState` `nlohmann::json` parse of `manifest.json`, this proposal has `Projection<(content-fingerprint, format), serialised-Value>` (item **k**) so `builtins.fromJSON (builtins.readFile manifestPath)` is content-keyed and shared across processes. Each piece is small individually; together they replace the ad-hoc machinery with shared infrastructure that every Nix user benefits from — and the residual obligation on tecnix is a one-line wrapper change (`__unsafeTectonixInternalZoneSrc zonePath` → `builtins.path { path = worldRoot + zonePath; }`), not eleven persistent builtins. The §4 mapping table records this migration in full.

The hard constraints below are what make this a *generalisation*, not a parallel set of ad-hoc fixes:

1. **Public API is frozen.** `builtins.path`, `builtins.fetchTree`, `builtins.filterSource`, and the flake-input attribute set cannot grow new arguments. Whatever we do has to fit behind today's signatures.
2. **No depending on nixpkgs.** Nixpkgs `lib.fileset`, `lib.cleanSource`, `lib.sources` etc. are downstream library code; Nix can't condition behaviour on them.
3. **No shelling out to system `git`.** Everything runs through libgit2 or upstream contributions to libgit2.
4. **No new user-visible flake-input attributes that change Input identity.** This constraint is about the *Input* layer (where narHash and storePath live) and is more nuanced than "no new attributes" — `dir` already exists at the *FlakeRef* layer (it's a subpath pointer; whole-repo narHash; `call-flake.nix` glues `outPath = sourceInfo.outPath + "/" + dir`). What is ruled out is a new Input-layer attribute (the historical `subdir` proposal, or `lazy`) that would change what narHash means or change the unit of lock-file identity. §4 (subtree fetching) explains how the proposal stays inside the existing two-layer split.

The survey's §8.0 is six items (**a**–**f**) addressing the in-process and on-disk caching, predicate composition, and devirtualisation gaps. This proposal adds four concrete items on top — **g** (substrate typeclass), **h** (lazy clone), **j** (subtree-aware fingerprint), **k** (content-keyed parse-cache) — plus a concrete fleshing-out of survey item **a**:

- **g.** A *single* projection typeclass (`Projection<Derived, From, To>` in CRTP form) covering most of the survey's ten ad-hoc fetcher-cache domains. Item g is a code-organisation win — schema discoverability for the domains that fit, domain-string consistency, encoding/decoding centralisation — *not* a runtime substrate. The actual substrate is the persistent fetcher cache + the soundness law (§0) + the suffix schema (§4); items j, k, and the §5 filter-identity work consume that substrate, not the typeclass per se. Item k uses a custom call shape (not the typeclass) because Nix `Value` is not pass-by-value-friendly; master's `tarball` / `hgRefToRev` use bespoke `lookupExpired` / `lookupWithTTL` shapes. The typeclass is *one* expression of the cache discipline — convenient where it fits, optional where it doesn't.
- **h.** Lazy, on-demand object fetch via a custom libgit2 transport that speaks protocol-v2-with-`filter blob:none` ourselves, plus a low-priority writable ODB backend that promisor-fetches missing blobs. All inside libgit2; coalescing via a `SourceAccessor::prefetchSubtree` virtual driven by `EvalState` triggers.
- **j.** Content-aware fingerprints in `GitSourceAccessor::getFingerprint`: for tree-rooted subpaths return `(CanonPath::root, "tree:<sha>")`, for file-rooted subpaths return `(CanonPath::root, "blob:<sha>")`, both with a `;l` suffix when `lfs=true` (LFS smudging is content-affecting). Cross-rev dedup hits the existing `sourcePathToHash` cache automatically for both subtrees and individual files. Cross-pipeline reuse via the existing `treeHashToNarHash` domain so tarball and git fetches that yield the same tree-SHA share a row. The blob branch is what makes item k's parse-cache cross-process-cross-rev — without it, file-rooted fingerprints fall back to parent-rev-keyed and item k's manifest cache misses across revs. Both branches receive `;e=<gitattributes-along-path-hash>` from `GitExportIgnoreSourceAccessor` when `exportIgnore=true` (sound for both: ancestor `.gitattributes` can mark either a tree or an individual file as export-ignore).
- **k.** Content-keyed parse-cache for `builtins.fromJSON` / `fromTOML` over `readFile` of content-addressed sources: a `Projection<(content-fingerprint, format), serialised-Value>`-shaped cache persisted in a new per-machine `parse-cache-v1.sqlite` (separate from the per-fingerprint eval cache, whose row-per-attribute schema is structurally wrong for whole-document caching). The string-to-fingerprint association lives in a per-`EvalState` side-table keyed on `StringData *` — *not* in `NixStringContext`, because parse-cache hints are not derivation-hashing references and an extra context variant would force surgery across every `forceStringNoCtx` caller. Closes the "no parse-caching for pure expressions over input contents" gap (survey §7.8) without a new public API — `fromJSON` / `fromTOML` continue to take a string; the cache fires when the string's `StringData *` is in the side-table.
- **a (filter identity, §5).** A stable identity for `builtins.path { filter = ...; }` derived from the lambda's source-region content and captured environment, computed entirely C++-side with no API change. This is survey item **a** made concrete. Closes the survey's §6.6 / §7.12 fileset cache bypass.

**Subtree-as-Input has two distinct meanings**, and the proposal addresses one of them. Today's `dir` attribute (FlakeRef layer) already provides a user-visible subtree pointer: `inputs.foo.url = "github:owner/repo?dir=areas/tools/foo"` works, the lock records `dir`, and `call-flake.nix` resolves `outPath = sourceInfo.outPath + "/" + dir` by string concatenation. The whole-repo narHash sits at the Input layer and is unchanged by `dir`. **The proposal accelerates this existing pattern** — item j speeds up access through `dir`-style subpaths to the same level as native subtree-Inputs would, on the `builtins.path` / `builtins.filterSource` code path. **It does not change the unit of Input identity** (the parent input's narHash). Making the *subtree* itself the unit of lock-file identity (subtree narHash in the lock, isolated subtree storepath) is feasible — ~550 LOC of fetcher plumbing across `git.cc`/`github.cc`/`flakeref.cc`/`call-flake.nix` plus a `version: 8` lockfile bump — but is a separate RFC's worth of migration work; §4 sketches the design and explains why this proposal stops at the acceleration tier rather than absorbing the identity-change tier.

The proposal is grounded in two concrete codebases: upstream Nix at [NixOS/nix@2d309b18e](https://github.com/NixOS/nix/tree/2d309b18e5a3a8734f4e64e659dac1813964c241) and libgit2 at [libgit2/libgit2@0609130af](https://github.com/libgit2/libgit2/tree/0609130af) (cloned locally). Every dependency on libgit2 has been verified against the actual API.

---

## 0. The underlying pattern: content-addressed eval-time projection

Items **g**, **j**, **k**, and the §5 filter-identity work share a single shape. Naming it makes the rest of the proposal read as instances of one structure rather than four parallel inventions.

**The pattern.** A *projection* is a memoised partial function `From → To` where:

- `From` is some content-determining input (a tree-SHA, a `(source-fingerprint, filter-identity)` pair, a `(parent-StorePath, subpath)` pair).
- `To` is a content-addressed result (typically a `Hash` or a `StorePath` minted via `makeFixedOutputPathFromCA`).
- The cache (`fetchers::Cache`) is the persistent memoisation table.
- The soundness law: `from₁ = from₂ ⇒ compute(from₁) = compute(from₂)`.
- Composition is "transparent through key independence": composed projections share rows with single projections automatically because the key is *content-derived*, not *path-through-cache-derived*.

**Where the pattern already lives in Nix.** This is the same shape Nix's existing `Realisation` machinery uses for floating CA derivations. Today, `Store::queryRealisation(DrvOutput{drvPath, outputName}) → StorePath` is exactly `Projection<DrvOutput, StorePath>::lookup`: a deferred handle (`DrvOutput`) keyed against a persistent table; equivalent computations producing the same content collapse to the same row because their `outPath` matches; `registerDrvOutput` is `cache->upsert`. The build-system version of this pattern is `(SingleDerivedPath::Built, Realisations table, DownstreamPlaceholder, tryResolve)`. **The proposal applies the same pattern at the evaluation layer**, replacing the build sandbox with cheap in-process computation (NAR-walk, libgit2 tree-walk, lambda hashing) and keeping the rest of the machinery (deferred handle, persistent table, content-addressed result) intact.

**One important inversion.** Floating-CA derivations key by *outputs*: the address is computed *after* the build, from the output's content. The proposal's projections key by *inputs*: the address is computed *before* the operation, from `From`. So this is **fixed-output CA at the eval layer**, not floating-CA at the eval layer. The `Realisations` analogue is structurally apt; the "floating" framing is misleading.


**Connection to `hashDerivationModulo` / `pathDerivationModulo`.** §5 (filter identity) and the `hashDerivationModulo`/`pathDerivationModulo` pair both compute a content-determined identity and discard upstream-derivation provenance, but the discard happens in different places. `hashDerivationModulo` for a `CAFixed` output hashes `"fixed:out:" + method + ":" + outputHash + ":" + storePath` per output (in `derivations.cc`) — the storePath is included, but is itself content-determined (via `makeFixedOutputPath`), so the per-output hash is content-determined end-to-end. The actual *provenance-stripping* — replacing the upstream drv path with the dep's CA-hashes and treating each output as a single-output 'out' derivation — happens in `pathDerivationModulo`'s `CaOutputHashes` consumer branch, where for each named output of a CA-fixed dep, `inputs2` is keyed on the output's CA-hash with `ChildNode{value = {"out"}}`. §5's filter-identity walk takes the analogous moves at the eval layer: (a) exclude `PosIdx` (the parser-table provenance) from the hash, and (b) hash captured primops by their static name (since their content-determined identity is just the name). The "Reading-1" decision (don't recurse through nested-closure capture envs) is the moral analogue: stop at the content-determined identity of each captured value, just as the build-side stops at the content-hash for fixed outputs. The `DrvHashes` typedef at `derivations.hh` (`typedef boost::concurrent_flat_map<StorePath, DrvHashModulo, DrvHashFct> DrvHashes;` plus `extern DrvHashes drvHashes;`) is the in-process memo of `hashDerivationModulo` — same shape as tecnix's `tectonixZoneCache_` (in-process, thread-safe map keyed by content-determined input). Both are existing instances of the projection pattern at the build/eval layer.

**What the analogy elides.** The build-side `Realisation` machinery carries a `std::set<Signature>` and a `Realisation::fingerprint` method that produces a canonical signing form for cross-store transfer (`realisation.hh::UnkeyedRealisation`). Cross-machine `Realisation` reuse can fall back to `checkSignatures > 0` when soundness assumptions cannot be made. **The proposal's `Projection` rows have no parallel** — projections are local, content-determined, and not multi-party-attestable. The cache-merge story below ("The cache itself is a join-semilattice") therefore requires the soundness law to hold absolutely; it cannot fall back to signature-gating like `Realisation` can. This sharpens the "mismatches indicate Nix-version skew or fetcher-policy mismatch" caveat: there is no signed-but-disagreeing fallback.

**The fifth `DerivationOutput` variant.** `DerivationOutput` has *five* shapes: `InputAddressed`, `CAFixed`, `CAFloating`, `Deferred`, `Impure`. The proposal so far has framed `(InputAddressed | CAFixed | CAFloating | Deferred)` as the relevant analogues. **`Impure` is the build-side analogue of the `Timed` projection partition**: `Derivation::shouldResolve` always returns true for impure outputs, and `queryRealisation` is *not* consulted — they're rebuilt every time. `tarball`/`hgRefToRev` retry-on-network are precisely this shape at the eval layer. Both bypass content-addressed memoisation by design.

### What the algebra is and what it isn't

**It is** a memoisation discipline with a soundness law. Item **g**'s `Projection<Derived, From, To>` CRTP typeclass names the discipline; items **j** and **k** plus the §5 filter-identity work are concrete instances; the existing ten fetcher-cache domains are pre-existing instances that get retrofitted.

**More precisely: it is the static-dependency corner of Build Systems à la Carte.** Mokhov, Mitchell & Peyton Jones (ICFP 2018, [arXiv:1803.10527](https://arxiv.org/abs/1803.10527); JFP 2020) frame build systems as `newtype Task c k v = Task { run :: forall f. c f => (k -> f v) -> f v }`, parameterised over a constraint `c f` that determines the dependency discipline. They classify Nix as `Build Monad (DCT k v) k v` — *monadic* `Task`s with a suspending scheduler, because Nix's evaluator picks dependencies at runtime via `import`/`fetchTree`/`builtins.derivation`. The proposal's projections operate at a **lower** layer than the evaluator: each `Projection::compute` reads its specific git ODB / source accessor / lambda hash, statically determined by `From`. There is no `>>=` where a future cache lookup is *chosen* by the value of an earlier one. **The projection layer is `Task Applicative` (statically-determined dependencies), not `Task Monad`** — even though the evaluator that drives it is monadic.

This is a real structural refinement, not decoration. `Task Applicative` instances have two practical properties `Task Monad` instances lack: (a) the set of cache rows a projection touches is statically computable from its `From` type, before any compute fires; (b) the projection layer parallelises safely without `>>=` re-entrancy hazards. Filter-identity dispatch (§5) — "if `computeFilterIdentity` returns `Just`, use the richer cache key; else fall through to bypass" — is the `Selective Task` extension (Mokhov, Lukyanov, Marlow & Dimino, ICFP 2019; [`selective`](https://hackage.haskell.org/package/selective)): static enumeration of effects, dynamic skipping based on hit/miss.

**It is structurally Salsa over content-addressed keys.** Hammer, Phang, Hicks & Foster, *"ADAPTON: Composable, Demand-Driven Incremental Computation"* (PLDI 2014), introduced the *articulation-point + demand-driven memoisation* model that powers rust-analyzer's [Salsa](https://github.com/salsa-rs/salsa) ("heavily inspired by adapton") and that is structurally identical to Bazel's [Skyframe](https://bazel.build/reference/skyframe) (`SkyKey` = `From`, `SkyValue` = `To`, `SkyFunction` = `Projection::compute`). The proposal's `Projection<From, To>` is structurally a Salsa query / Skyframe SkyFunction with one simplification: **content-addressed keys make Salsa's dirty-propagation engine unnecessary**. When inputs change, the cache *row* changes (different key), not the same row's value at a fixed key — so Adapton/Salsa/Skyframe's whole dirty-propagation graph collapses to "two distinct keys with two distinct values." The proposal is "Salsa minus dirty-propagation"; the contribution is the constraints (frozen public API, no nixpkgs cooperation, no system git, no flake-input attributes) under which the substrate has to fit, not the substrate itself.

**It is a multicategory of n-ary projections.** Several existing and new domains key on multiple inputs: `treeShaToStorePath` is `(TreeSha, FileIngestionMethod, Name) → StorePath`. Today the proposal encodes this as `From = struct { ... }`. The clean shape is a variadic `Projection<Output, Inputs...>` typeclass — a [multicategory](https://en.wikipedia.org/wiki/Multicategory) of substitution-composable n-ary morphisms. Composition by content-key matching is the multicategory substitution rule.

### What it forces (the law-content has bite)

The `Task Applicative` framing forces real partitions that the existing typeclass paragraph names informally. Cross-tree verification against master, DetSys's `nix-src`, and tecnix yields the following picture. **The partitions are orthogonal concerns, not a strict three-way disjoint split** — `file` straddles two of them.

**Concern 1 — `Store`-aware**: the cached value carries a `StorePath` whose validity must be checked against the live store (via `Cache::lookupStorePath`, which gates on `store.isValidPath`). Affects `hgRev`, `file`, and tecnix's new `gitRevUrl` domain. (DetSys adds an `allowInvalid` flag to `lookupStorePath` that strengthens this — when true, callers receive cached metadata even when the store path was GC'd, so the cache row's content survives store eviction.)

**Concern 2 — `Timed` / non-content-keyed**: the producer reads wall-clock time or live remote state, so `compute(from)` can return different values for the same `from` at different times. **The Kleisli laws (left identity, right identity, associativity) fail for these.** Affects `tarball` (retry-on-network-error fallback to stale rows; TTL-gated via `lookupExpired`), `file` (also TTL-gated and has the same retry-fallback pattern as `tarball`), and `hgRefToRev` (queries a live remote with a moving ref, gated via `lookupWithTTL`).

**Concern 3 — `Workdir` (genuinely needs dirty-tracking)**: master's `workdirInfoCache_` in `git-utils.cc` caches `git_status` results. The answer changes when the user runs `git add` or edits files. Master handles this by **whole-cache eviction at evaluator `clear()`** (called e.g. on `:reload` in the REPL), not by a Salsa-style dependency walk. This is the only fetcher-side cache where Salsa-minus-dirty-propagation actually breaks down — and it does so by sidestepping rather than implementing dirty propagation: the cache is per-evaluation in-memory only, never persisted to SQLite.

**Concern 4 — per-evaluation session identity**: tecnix's `tectonixCheckoutZoneCache_` is keyed on a non-content `zonePath` string, lives one evaluation, and uses `StorePath::random()` for the cached value. The cache memoises *which random store path was minted for this zone in this evaluation* — not "the contents of this zone." It dies with the `EvalState`. The proposal does not generalise this; it is documented here only to acknowledge tecnix's existing behaviour.

**The cleanest formal recipe** is therefore three concerns combined orthogonally on top of `Projection<O, Is...>`:

- `Projection<O, Is...>` is the base (six domains: `treeHashToNarHash`, `sourcePathToHash`, `gitRevCount`, `gitRevToTreeHash`, `gitRevToLastModified`, `gitLastModified`).
- `Store` concern adds a `StorePath` to the value with GC-validity gating (one domain: `hgRev`; plus tecnix's `gitRevUrl`).
- `Timed` concern adds TTL/retry semantics that violate `Task Applicative` (one domain: `hgRefToRev`).
- `Store + Timed` is `file` and `tarball` — the value carries `etag`/`lastModified` *and* the row has TTL+retry. **`file` is in this intersection, not pure `Store`.** Two clients hitting the same URL at different times will record different `etag`s even for byte-identical content, so two-cache merge requires a domain-specific resolution policy (prefer rows with `immutableUrl` set, then max(timestamp)).

**Observed across all three trees** (master `d4979d9f8`, DetSys `nix-src/main`, tecnix `45d9c6f4a`): the partition assignment is *identical* — no domain shifts category between trees. DetSys and tecnix add producer call-sites (DetSys's `path.cc` and `fetchers.cc` write `sourcePathToHash` from substitution paths; tecnix's `gitRevUrl` is a new `Store` domain) but never recategorise an existing domain. **This is strong evidence that the partition is a structural property of the domain semantics, not an implementation accident.**

### The cache itself is a join-semilattice (with caveats)

There is one place where lattice structure has potential operational content: **the persistent fetcher cache, viewed as a `Set<(domain, key, value)>`, is a bounded join-semilattice for content-keyed domains** (the six base `Projection` instances plus `hgRev` plus tecnix's `gitRevUrl`). Cross-tree verification confirms the SQLite schema in master, DetSys, and tecnix is identical: `Cache(domain TEXT, key TEXT, value TEXT, timestamp INTEGER)` with primary key `(domain, key)`. For these domains, the soundness law forces equal keys to imply equal values, so `INSERT OR IGNORE` merge is idempotent, commutative, and associative.

Three caveats are load-bearing:

1. **The in-tree single-process upsert uses `INSERT OR REPLACE` (last-writer-wins), NOT `INSERT OR IGNORE`.** A future `nix fetcher-cache merge` would need to use `INSERT OR IGNORE` explicitly; the existing internal upsert is *not* the merge primitive. Last-writer-wins on identical content is harmless for content-keyed domains, but for time-bound rows it overwrites the etag/lastModified.

2. **`tarball`/`file` domains carry `etag`, `lastModified`, and optional `immutableUrl` in the value.** Two clients hitting the same URL at different times produce different rows even for byte-identical fetches. Merge for these requires a policy: prefer rows with `immutableUrl` set, then `max(timestamp)`. The CRDT property only holds after projecting away these time-bound fields; the proposal's `Timed` concern partition is what isolates them from the merge story.

3. **Mismatches on equal keys do *not* always prove content-determinism violations.** They can also indicate (a) Nix-version skew (NAR canonicalisation changes between releases), (b) different fetcher policies (LFS opt-in differing between clients), (c) symlink/executable-bit handling differences. The honest framing is "mismatches indicate a soundness-law violation **or** a Nix-version / fetcher-policy mismatch." A merge tool should report mismatches with version-and-policy context, not unconditionally reject one as buggy.

The Bazel RE CAS analogy is **weaker** than I implied earlier in this document: Bazel CAS keys are content hashes (`digest = hash(data)`) so collision is impossible by construction; Nix fetcher-cache keys are *requests* (URLs, refs, treeShas) and conflict-freeness depends on the soundness law holding. Bazel CAS is a true CRDT; the Nix fetcher cache is a CRDT only modulo the soundness law plus version/policy alignment.

This still justifies a future `nix fetcher-cache merge` operation, but the recipe is more nuanced than "INSERT OR IGNORE": pin the cache schema version (currently `v4`), use `INSERT OR IGNORE` for content-keyed domains, apply `max(timestamp)` and `immutableUrl`-preference for time-bound rows, refuse to merge across `cache_v*` versions. Out of scope for this proposal; documented as future work in §10.

### What the algebra is *not*

**It isn't** a lattice on the things projections operate on. Tree subtree-containment is a partial order whose meet (greatest common subtree) does not exist as a rooted relation; the powerset lattice on `(path, OID)` pairs that *does* have meet is not the one cross-rev dedup uses (dedup is by tree-SHA equality, a flat equivalence). Filter pointwise refinement is a Boolean lattice but the lattice operations are exploited at the `lib.fileset` Nix-language layer above this proposal, not in the C++ projection cache. The persistent cache *itself* is a join-semilattice (above); the projections themselves and their domains/codomains are not lattice-structured in any load-bearing sense.

**It isn't** a Lens or any bidirectional optic. In profunctor-optics terms, the proposal's projections are **Getters, Folds, and AffineFolds** — read-only optics in the sub-lattice that doesn't require put-direction laws. There is no projection in this proposal with a meaningful put-direction; every projection is consumed downstream as a fresh content-addressed handle. (`subtree(T, p)` *would* be a very-well-behaved Lens if it had a `put` — git's `treebuilder` even implements that put — but the proposal has no operation that splices a derived value back into its parent. The §5 filter-identity walk is a Fold with a depth bound and a Reading-1 cutoff at lambda boundaries.)

**It isn't** an Adapton-style demand-driven invalidation graph. Content-addressed keys make Adapton's dirty-propagation engine unnecessary: changing inputs change the *key*, not the *value at a fixed key*, so there is nothing to invalidate.

**It isn't** novel as a categorical structure. The category of memoised content-addressed projections has no published categorical formalisation (verified by direct literature search across nLab, Wikipedia, arXiv on "Merkle category", "memoisation category", "hash-cons category"). The closest categorical formalisation in the VCS space is **Mimram & Di Giusto, "A Categorical Theory of Patches"** (MFPS 2013, [arXiv:1311.3903](https://arxiv.org/abs/1311.3903)), which models *patches* (deltas between states) with pushouts for merge — orthogonal to projections (views/slices of a single state). Cited for completeness; not load-bearing here.

**It isn't** novel as engineering, either. Three pieces of prior art encode pieces of the same shape:

- **Bazel's Remote Execution v2 protocol** ([`build/bazel/remote/execution/v2`](https://github.com/bazelbuild/remote-apis/blob/main/build/bazel/remote/execution/v2/remote_execution.proto)) defines `Directory`/`FileNode`/`DirectoryNode` Merkle messages where each `DirectoryNode` references a child by `(name, digest)` independently of position. Identical subtrees deduplicate automatically across mount points and across actions — exactly item **j**'s cross-rev-subtree-dedup property, in production since 2018. Bazel's "Build without the Bytes" mode (default since Bazel 7) is the same load-bearing intuition as item **h**: lazy materialisation via a content-addressed remote fetch.
- **Git's `--filter` family** ([`git-rev-list(1)`](https://git-scm.com/docs/git-rev-list)) is itself a documented Merkle-projection algebra. `--filter=blob:none + tree:0 + sparse:oid=X` composes by intersection — the *meet* of the filter predicates. The proposal's item **h** uses only `blob:none`; richer composition (`combine:` form) is plausible future work. Git's deprecated `sparse:path=` was removed for security reasons (arbitrary filesystem-path filters); the proposal's filter-identity work avoids this anti-pattern by computing identity from the lambda's AST + closed-over evaluator state, not from filesystem paths.
- **Maziarz, Ellis, Lawrence, Fitzgibbon, Peyton Jones (PLDI 2021), "Hashing Modulo Alpha-Equivalence"** ([arXiv:2105.02856](https://arxiv.org/abs/2105.02856)) gives an O(n log² n) algorithm for hashing lambda-term ASTs invariant under alpha-renaming, with a published correctness proof. **§5's filter identity should cite this paper**: today's prose ("hash the AST plus environment") is hand-wavy; with the citation it has a published basis. **Unison** ([unison-lang.org](https://www.unison-lang.org/docs/the-big-idea/)) is the production-precedent for content-addressed code, using normalised positional variables — a simpler approach achieving alpha-invariance for closed terms without the formal complexity result.

What this proposal contributes that the prior art doesn't: a concrete plan to apply this pattern *at the Nix evaluation layer*, working through the constraints (frozen public API, no nixpkgs cooperation, no system git, no new flake-input attributes). The pattern is not novel; the integration is.

### Vocabulary

- **"Content-addressed projection"** is the term used throughout this proposal. It is distinct from IPLD's "selector" (which is a query language whose results are *not* content-addressed).
- The proposal does **not** claim "Merkle category" / "Merkle lattice" structure. Direct searches of the literature surface no published categorical formalisation of Merkle DAGs — the literature treats them operationally — and the proposal does not need one. The honest stance is engineering, not category-theoretic. (Mimram & Di Giusto's "A Categorical Theory of Patches" is the closest VCS-space categorical work but models patches between states, not projections of one state — orthogonal; cited above for completeness.)

---

## 1. What libgit2 actually supports (and doesn't)

This shapes everything below.

**Supported, used directly:**

- **Custom ODB backends** with priority ordering ([`git_odb_add_backend`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/odb.h#L582)) and **alternates** ([`git_odb_add_alternate`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/odb.h#L603), only consulted after main backends miss). A backend implements `read`, `read_header`, `exists`, `refresh`, `readstream`, `writepack`. The `refresh` callback is automatically invoked on failed lookups ([`sys/odb_backend.h#L68-L75`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/sys/odb_backend.h#L68-L75)).
- **Custom transports** via [`git_transport_register(scheme, cb, param)`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/sys/transport.h#L214) — registered transports are checked **before** the built-in HTTPS/SSH/git handlers ([`transport.c#L51-L73`](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/transport.c#L51-L73): `transport_find_by_url` walks `custom_transports` first). The `git_transport` struct ([`sys/transport.h#L43-L153`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/sys/transport.h#L43-L153)) exposes every callback we need: `connect`, `capabilities`, `ls`, `negotiate_fetch`, `download_pack`, `shallow_roots`. **A custom transport can speak any wire protocol it wants — including protocol-v2 with `filter blob:none`.**
- **Whitelisting unknown extensions** via [`GIT_OPT_SET_EXTENSIONS`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/common.h#L522). Adding `"partialclone"` to the user-extensions vector ([`repository.c#L1913-L1944`](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/repository.c#L1913-L1944)) makes `git_repository_open` accept partial-clone repos that today error out per [libgit2#6880](https://github.com/libgit2/libgit2/issues/6880).
- **Thread-safe ODB** — `git_odb` uses internal locking and is safe to share across threads ([`docs/threading.md#L24-L25`](https://github.com/libgit2/libgit2/blob/0609130af/docs/threading.md#L24-L25)). Most other libgit2 objects (`git_repository`, `git_tree`, working trees) are *not* thread-safe and must be accessed from one thread at a time ([`#L4-L18`](https://github.com/libgit2/libgit2/blob/0609130af/docs/threading.md#L4-L18)). References and configuration snapshots are immutable and shareable.
- **Shallow clone** with `git_fetch_options.depth` ([`remote.h#L820-L827`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/remote.h#L820-L827)).
- **Bare clone, no checkout** via `git_clone_options.bare = 1` and `checkout_strategy = GIT_CHECKOUT_NONE` ([`clone.h#L110-L171`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/clone.h#L110-L171)).
- **Pathspec matching** — `git_pathspec_new`, `git_pathspec_matches_path`, `git_pathspec_match_tree` ([`pathspec.h`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/pathspec.h)).
- **Tree construction** — `git_treebuilder_new`, `git_treebuilder_filter`, `git_treebuilder_write` ([`tree.h`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/tree.h)).
- **Indexer** — `git_indexer_new` / `git_indexer_append` / `git_indexer_commit` ([`indexer.h`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/indexer.h)). Streams a packfile (received from any source) into the ODB.
- **Subtree access** — `git_tree_entry_bypath`, `git_tree_walk`.
- **Smart-protocol caps libgit2 sends in `want` lines** (via `buffer_want_with_caps` in [`smart_pkt.c`](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/transports/smart_pkt.c)): `multi_ack[_detailed]`, `side_band[_64k]`, `include_tag`, `thin_pack`, `ofs_delta`, `shallow`. Note that `want_tip_sha1` / `want_reachable_sha1` are *server-advertised* capabilities libgit2 *detects* (via `git_smart__detect_caps` in [`smart_protocol.c`](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/transports/smart_protocol.c)) — never client-sent.

**NOT supported in libgit2:**

- **Native partial-clone fetch.** Issue [libgit2#5564](https://github.com/libgit2/libgit2/issues/5564) open since 2020. The built-in smart transport's want-list serialiser [`git_pkt_buffer_wants`](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/transports/smart_pkt.c#L762-L843) emits `want <oid>` lines, `shallow` lines, and `deepen` lines — but never `filter`. The function isn't `GIT_EXTERN`-exported; `smart_pkt.c` has zero `GIT_EXTERN` symbols, so we can't reuse it from outside.
- **Wrapping the smart subtransport to inject `filter`.** The subtransport interface ([`sys/transport.h#L322-L438`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/sys/transport.h#L322-L438)) is byte-stream level — `read`/`write` of opaque bytes for `UPLOADPACK_LS` / `UPLOADPACK` / etc. The doc-comment for `git_smart_subtransport_definition` is explicit: "The smart transport knows how to speak the git protocol… For this, a subtransport interface is declared, and the smart transport delegates this work to the subtransports." Capability negotiation happens *above* the subtransport. By the time `write()` runs, `git_pkt_buffer_wants` has already serialised wants without `filter`.
- **Sparse checkout / sparse-index.** Issue [libgit2#2263](https://github.com/libgit2/libgit2/issues/2263) open.

**The path forward.** We compose three already-supported APIs to get partial-clone behaviour entirely inside libgit2:

1. Implement a `git_transport` for `https://` (and friends) ourselves via `git_transport_register`. It speaks protocol-v2 over an HTTPS subtransport (we can keep using libgit2's HTTP subtransport for the byte-stream layer, since *that* is just bytes; we just need to write our own protocol framing on top). The wants we send include `filter blob:none`.
2. Stream the resulting thin packfile into the ODB via `git_indexer_*`.
3. Whitelist `partialclone` via `GIT_OPT_SET_EXTENSIONS` so libgit2 opens the resulting repo.
4. Register a `PromisorBackend` ODB backend with negative priority (consulted last, after main pack/loose backends miss; *not* a `git_odb_add_alternate`-style alternate — alternates are read-only and `PromisorBackend` needs to ingest fetched packfiles via `git_indexer_commit`); on miss, the `read`/`refresh` callbacks dispatch to our custom transport for a single-blob fetch.

No system `git` invocation. No libgit2 source modifications. The seam is one `git_transport` implementation + one `git_odb_backend`. Long-term, the principled fix is to upstream partial-clone to libgit2 ([#5564](https://github.com/libgit2/libgit2/issues/5564)); when that lands, our custom transport retires.

---

## 2. The `Projection<Output, Inputs...>` typeclass (item g)

(See §0 for the algebraic framing: this typeclass is the static-dependency `Task Applicative` instance from Build Systems à la Carte, applied at the eval/projection layer. The shape is a multicategory of n-ary content-keyed projections; the cache is the Salsa/Skyframe-style persistence layer, with content-addressed keys removing the need for dirty-propagation. The three-way partition that emerges — `Projection` / `StoreProjection` / `TimedProjection` — is what the `Task Applicative` laws *force* on the existing ten domains.)

The survey's §2.1 identifies three layers (L1 SourceAccessor, L2 storeFS, L3 real store) and the projection cache as a separate concern. This proposal does *not* reify those as distinct C++ types — the survey's existing `SourceAccessor` / `MountedSourceAccessor` / `Store` types already correspond to the layers, and §8.0 fixes their algebra. What's missing is a typeclass for the cross-layer mappings that today live as ten ad-hoc cache domains.

### The refactor

The fetcher cache today has ten domains, each implemented ad-hoc with its own attr-set encoding (survey §7.3 footnote). Item **g** is a small typeclass that abstracts the shared shape — *not* a new layer model. (The simplest sketch uses a single `From` type; the multicategory shape from §0 generalises this to variadic `Projection<Output, Inputs...>`, which eliminates the `From = struct {...}` boilerplate that several of the existing domains have today. Both shapes have the same algebra; the variadic form is the cleaner C++ encoding once `From`-tuples become common.)

The right form is CRTP: each projection is a stateless function of `fetchers::Settings`, and the cache-row domain identifier is best given as a static `constexpr std::string_view` on the derived class so the literal storage is bound at compile time. This structurally rules out the `string_view`-bound-to-temporary-`std::string` lifetime hazard that virtual `std::string_view domain() const` would invite, and avoids vtable indirection at call sites where the type is statically known.

```cpp
// New: src/libfetchers/include/nix/fetchers/projection.hh
template<typename Derived, typename From, typename To>
struct Projection {
    // Required on Derived:
    //   static constexpr std::string_view domain;
    //   static fetchers::Attrs toKey(const From &);
    //   static To fromValue(const fetchers::Attrs &);
    //   static fetchers::Attrs toValue(const To &);

    // The template factors the lookup-or-upsert discipline. The compute
    // step is supplied by the caller as a closure so call sites can pass
    // in whatever accessor / repo / settings context they need without
    // the template constraining the compute signature.
    template<typename ComputeFn>
    static To lookup(const fetchers::Settings & settings, const From & from, ComputeFn && compute) {
        auto cache = settings.getCache();
        auto key = fetchers::Cache::Key{Derived::domain, Derived::toKey(from)};
        if (auto cached = cache->lookup(key))
            return Derived::fromValue(*cached);
        To result = compute();
        cache->upsert(key, Derived::toValue(result));
        return result;
    }
};
```

A concrete call site:

```cpp
struct TreeHashToNarHash : Projection<TreeHashToNarHash, Hash, Hash> {
    static constexpr std::string_view domain = "treeHashToNarHash";
    static fetchers::Attrs toKey(const Hash & h) { return {{"treeHash", h.gitRev()}}; }
    static Hash fromValue(const fetchers::Attrs & a) { return Hash::parseSRI(getStrAttr(a, "narHash")); }
    static fetchers::Attrs toValue(const Hash & h) { return {{"narHash", h.to_string(HashFormat::SRI, true)}}; }
};

// In GitRepoImpl::treeHashToNarHash (existing member function):
Hash GitRepoImpl::treeHashToNarHash(const fetchers::Settings & settings, const Hash & treeHash) {
    auto accessor = getAccessor(treeHash, {}, "");
    return TreeHashToNarHash::lookup(settings, treeHash, [&]() {
        return accessor->hashPath(CanonPath::root);
    });
}
```

The compute closure carries the accessor, the repo, anything else the call site needs. The template only enforces the lookup-or-upsert discipline + the cache-key shape; it doesn't constrain what `compute` reads.

The existing ten domains (`treeHashToNarHash`, `sourcePathToHash`, `gitRevCount`, `gitRevToTreeHash`, `gitRevToLastModified`, `gitLastModified`, `hgRev`, `hgRefToRev`, `tarball`, `file`) become concrete instantiations. New projections needed by items **a**, **h**, **j**, and **k** below become uniform additions. Tecnix's downstream `tectonixZoneCache_` (`SharedSync<std::map<Hash, StorePath>>` in tecnix's `EvalState`) and `tectonixManifestJson` (per-`EvalState` `nlohmann::json` memo) both collapse to `Projection<...>` instances with on-disk persistence — no new layers, no new identity types.

**Shape-fit caveat.** Of the ten existing domains, six (`treeHashToNarHash`, `sourcePathToHash`, `gitRevCount`, `gitRevToTreeHash`, `gitRevToLastModified`, `gitLastModified`) fit the simple `lookup-or-compute` pattern cleanly. Two (`hgRev`, `file`) require the store-aware variant `lookupStorePath` because their values reference a `StorePath` that the GC must track; introduce a sibling `StoreProjection<Derived, From, To>` for those rather than retrofitting Store-awareness into the base. One (`tarball`) needs `lookupExpired`-style retry-on-failure semantics that doesn't fit either base; leave its call site bespoke. One (`hgRefToRev`) uses `lookupWithTTL`. The typeclass is therefore a refactor of most of the cache machinery, not all of it.

**Concurrent-miss caveat.** N threads calling `lookup(...)` with the same `from` all miss the cache, all run `compute()`, all upsert. The SQLite layer is `INSERT OR REPLACE` and the upserts are idempotent for content-determined results, so this is *safe* but **`N`-times wasteful** under `eval-cores=N`. For cheap computes (cache-row decoding) the waste is invisible; for expensive computes (NAR walks of large trees, lambda hashing of deep capture trees), the cost multiplier is real — eight evaluator threads racing for the same input tree could pay 8× the cold-eval cost compared to serial. **An in-memory "compute-in-progress" registry** is therefore not optional in the long run for the eval-cores story to actually scale.

The shape — concrete enough to be implementable, simple enough to be ~80 LOC. Two structural constraints from cross-tree audit shape the sketch:

- **`Cache` is abstract** (`fetchers/cache.hh` declares it with pure-virtual `lookup`/`upsert`/etc.), so the in-flight map and the `lookupOrCompute` helper live on the concrete `CacheImpl` (declared in `cache.cc`); subclasses inherit through the `Cache` interface.
- **`Cache::Key = pair<string_view, Attrs>`** and `Attrs = map<string, Attr>` is *not* `std::hash`-able (no `std::hash<map<...>>` specialisation; `Attr` is a `std::variant<string, uint64_t, Explicit<bool>, LazyAttr>` which would also need a custom hash). Using `Key` directly as the concurrent-map key requires writing a custom hasher. The simpler approach is to key on the same JSON-stringified form `CacheImpl::lookupExpired` already constructs (`attrsToJSON(key.second).dump()`, line 88) — that's the byte form the SQLite layer uses for row identity, so wait/wake align with what a `lookup` would observe.

```cpp
// libfetchers/cache.cc — additions to CacheImpl (the concrete subclass)

// In-flight registry keyed on the SQLite row's stringified key shape.
// std::string is std::hash-able; matching the row-key shape ensures
// wait/wake align with SQLite-observable cache state.
boost::concurrent_flat_map<
    std::string,
    std::shared_future<Attrs>
> inFlight;

static std::string flightKey(const Cache::Key & k) {
    // attrsToJSON forces LazyAttrs (master's behaviour at attrs.cc:41,
    // forceAttr); same forcing happens inside lookupExpired anyway, so
    // re-entrance hazards are no worse here. Domain (string_view) and
    // attrs JSON are concatenated with a NUL separator so the byte form
    // is unambiguous.
    return std::string(k.first) + '\0' + attrsToJSON(k.second).dump();
}

// Non-virtual coalescing helper on CacheImpl. Subclasses without an
// equivalent registry just call lookup/upsert directly.
template<typename ComputeFn>
Attrs CacheImpl::lookupOrCompute(const Key & key, ComputeFn && compute) {
    if (auto cached = lookup(key)) return *cached;

    auto fk = flightKey(key);
    std::promise<Attrs> ours;
    auto ourFuture = ours.get_future().share();
    std::shared_future<Attrs> winnerFuture;
    bool weAreTheWinner = false;

    // try_emplace_and_cvisit is atomic: either we win and the on_emplace
    // visitor reads our future back, or someone else won and the on_existing
    // visitor reads their future. Both visitors run while holding the
    // bucket lock — there's no race window where the entry is observable
    // without a future.
    inFlight.try_emplace_and_cvisit(
        fk, ourFuture,
        [&](auto & ent) { weAreTheWinner = true; winnerFuture = ent.second; },
        [&](const auto & ent) { winnerFuture = ent.second; }
    );

    if (!weAreTheWinner) {
        // Loser path: wait for the winner. shared_future propagates either
        // a successful Attrs or the original exception via .get().
        return winnerFuture.get();
    }

    // Winner path: compute, persist, fulfil, then deregister.
    try {
        Attrs result = compute();
        upsert(key, result);                 // SQLite upsert (existing path)
        ours.set_value(result);              // wake all loser threads
        inFlight.erase(fk);                  // safe: shared_future copies losers
                                             //   hold remain valid after erase
        return result;
    } catch (...) {
        ours.set_exception(std::current_exception());
        inFlight.erase(fk);
        throw;
    }
}
```

Three details that make this correct rather than racy:

1. **`try_emplace_and_cvisit` is atomic.** A naive `if (inFlight.count(k)) wait; else insert` is racy: two threads can both observe absence and both insert. boost's `try_emplace_and_cvisit` either inserts the new value (and visits the just-inserted entry) or visits the pre-existing entry, atomically — exactly what we need to pick a winner. Master's existing uses (`eval.cc::evalFile` at `fileEvalCache->try_emplace_and_cvisit(...)`, `primops.cc::RegexCache::get`, `aws-creds.cc::getCredentialsRaw`) all use this exact pattern; the names of both visitors as `on_emplace` then `on_existing` matches the boost API.
2. **`set_value` before `erase`.** Loser threads hold `winnerFuture` (a `shared_future` copy that survives the map entry's destruction); fulfilling the promise wakes them up. Erasing the in-flight slot only releases the winner's reference; the losers' copies remain valid. Reversing the order (erase first, then set_value) is *also* correct — `shared_future` is value-semantic — but ordering set_value first matches the intuition "fulfil obligations, then clean up."
3. **Exception propagation.** `try { ... } catch (...) { set_exception; erase; throw; }` — *not* a RAII guard. The guard form (`Finally` setting an exception on destruction) is wrong because it would set the exception even on the success path if `upsert` happens to throw after `set_value`. The explicit try/catch keeps success and failure paths distinct.

**LazyAttr re-entrance caveat.** `attrsToJSON(key.second)` calls `forceAttr` per attr (master's `attrs.cc:41`), which can fire user code if any attr is a `LazyAttr` thunk. Master's existing `lookupExpired` does the same forcing under `_state.lock()`; this sketch does it *outside* the SQLite lock but inside `try_emplace_and_cvisit`'s bucket lock. A `LazyAttr` whose computation re-enters the *same key* would deadlock on the bucket lock — but for content-keyed projections (item j's `tree:<sha>`, item k's content fingerprints) the keys are always already-forced primitives, so the hazard does not arise in practice. Document this preconditioning constraint when wiring projection call sites: keys must be force-clean before reaching `lookupOrCompute`.

~80 LOC counting `flightKey` + `lookupOrCompute` + boost concurrent_flat_map field declaration + tests. Flagged as a follow-on but **gated to whichever PR introduces eval-cores parallelism for the workloads that benefit**; without it, item j's eval-cores claim ("Phase 1 + Phase 2 split lets `eval-cores>1` scale") only holds for workloads where compute is cheap.

---

## 3. Lazy clone (item h) — entirely inside libgit2

We want: a user runs `nix build .#packages.x86_64-linux.foo` against a 50 GB monorepo, and Nix only downloads the commit, tree, and blob objects that `foo`'s evaluation actually reads. No system-`git` invocation. No modifications to libgit2.

The construction is three pieces composed inside libgit2:

### 3.1 A custom HTTPS transport that speaks `filter blob:none`

We register our own transport via [`git_transport_register("https", PartialCloneTransport::create, nullptr)`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/sys/transport.h#L214). Because `transport_find_by_url` walks `custom_transports` before the built-in array ([`transport.c#L51-L73`](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/transport.c#L51-L73)), our handler takes precedence on every `https://` URL.

`PartialCloneTransport` implements the `git_transport` struct ([`sys/transport.h#L43-L153`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/sys/transport.h#L43-L153)). The non-trivial callbacks:

- **`connect`** — set up a TLS HTTP/2 connection to the remote. We can reuse Nix's existing `FileTransfer` machinery (`src/libstore/filetransfer.cc`) for the HTTPS layer; it already handles auth, redirects, and connection pooling.
- **`ls`** — speak protocol-v2's `ls-refs` command. Standard pkt-line framing on top of `Content-Type: application/x-git-upload-pack-request`.
- **`negotiate_fetch`** — send `command=fetch\n` capability set including `filter\n`, then on the request line write our wants with `want <oid>\n` lines plus a `filter blob:none\n` line. **This is the line libgit2's [`git_pkt_buffer_wants`](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/transports/smart_pkt.c#L762-L843) cannot emit.** We write it ourselves because we control the wire.
- **`download_pack`** — receive the response packfile and feed it directly into [`git_indexer_append`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/indexer.h). The indexer handles thin-pack delta resolution and writes a `.idx` file alongside the pack.

**Why we cannot piggyback on `git_transport_smart`.** A natural first instinct is to register a `git_smart_subtransport` rather than a full `git_transport`, on the theory that we'd inherit libgit2's pkt-line / sideband / multi-ack / negotiation logic and only have to inject a `filter` capability. **This does not work.** The cap table (`transport_smart_caps` in [`smart.h#L137-L154`](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/transports/smart.h#L137-L154)) has only protocol-v0/v1 fields — `ofs_delta`, `multi_ack[_detailed]`, `side_band[_64k]`, `include_tag`, `delete_refs`, `report_status`, `thin_pack`, `want_tip_sha1`, `want_reachable_sha1`, `shallow`, `push_options` plus the bookkeeping `common` flag and the string-valued `object_format` / `agent` fields. There is no `fetch`, no `ls-refs`, no `filter`, no `object-info`. The negotiation entry point [`git_smart__negotiate_fetch`](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/transports/smart.h#L185-L188) and the want-list serialiser [`git_pkt_buffer_wants`](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/transports/smart_pkt.c#L762-L843) both emit only the v0/v1 cap set. The subtransport interface ([`sys/transport.h#L322-L438`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/sys/transport.h#L322-L438)) is byte-stream level: by the time `write()` runs, capability negotiation has already happened *above* it without `filter`. A subtransport cannot inject `filter`, and a v0/v1-only smart transport cannot speak protocol-v2's `command=fetch` framing. The only escape is to register at the higher `git_transport` layer and own the protocol entirely.

**Order-of-magnitude budget.** Protocol-v2's request/response shape is well-documented in `Documentation/gitprotocol-v2.txt` upstream; we don't need v0/v1's multi-ack negotiation. The work decomposes into: pkt-line framing, HTTP stateless-RPC on top of Nix's existing `FileTransfer` (so connection management and TLS are already handled), `ls-refs` request/response, `command=fetch` emission with `filter blob:none`, sideband demultiplexing, thin-pack ingestion via `git_indexer_*`, `git_transport` v-table glue, capability detection, the `PromisorBackend` ODB alternate (seven `git_odb_backend` callbacks), and `GitRepoImpl` integration. **For an order-of-magnitude anchor**: libgit2's own v0/v1 implementation totals 2,902 LOC across `smart.{h,c}` (756 LOC) + `smart_pkt.c` (870 LOC) + `smart_protocol.c` (1,276 LOC). The v2-relevant subset is structurally smaller (no multi-ack, no NAK loop, no ref-advertisement-on-connect): the four functions whose v2 analogues we need (`git_smart__store_refs`, `git_smart__detect_caps`, `git_smart__negotiate_fetch`, `git_smart__download_pack`) sum to ~500 LOC of v0/v1 today. **Counted against that anchor, a from-scratch v2-only `PartialCloneTransport` plus `PromisorBackend` plus `GitRepoImpl` integration is on the order of low thousands of LOC.** Concrete scoping requires prototyping; treat any tighter range as speculation.

The HTTPS byte-stream layer itself we get for free: Nix's existing `FileTransfer` ([`src/libstore/include/nix/store/filetransfer.hh`](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libstore/include/nix/store/filetransfer.hh)) already supports `HttpMethod::Post` with an `UploadData` request body and a `dataCallback` for the streaming response, which is exactly the smart-HTTP shape. We do not implement TLS, auth, redirects, or connection pooling; we frame pkt-lines on top of an existing HTTPS POST.

**Latent prerequisite found during sketch**: `FileTransfer` does not currently set `CURLOPT_POSTREDIR`. curl's default behaviour on a 301/302/303 redirect of a POST is to convert it to a GET, silently dropping the request body. For a v2 fetch this is catastrophic — the very first GitHub redirect (renamed org, http→https) would silently break partial-clone. Fix: add `curl_easy_setopt(req, CURLOPT_POSTREDIR, CURL_REDIR_POST_301 | CURL_REDIR_POST_302 | CURL_REDIR_POST_303 | CURL_REDIR_POST_307)` conditional on `request.method == HttpMethod::Post`. This is a small (~5 LOC) prerequisite change in `filetransfer.cc` that ships before the partial-clone work proper.

**Why not vendor libgit2's smart transport and modify it?** A natural alternative is to copy `src/libgit2/transports/{smart.{h,c},smart_pkt.c,smart_protocol.c}` into Nix's tree and patch them to support protocol-v2 with `filter blob:none`, on the theory that we'd reuse libgit2's pkt-line, sideband, and stateless-RPC implementations rather than re-implementing them. **Structurally, this is strictly worse than from-scratch.** Three reasons:

1. **The vendored baseline is already larger than from-scratch.** Those four files total 2,902 LOC, all of it implementing v0/v1 negotiation (multi-ack, NAK loops, push, report-status) we do not need. The v2-only subset we'd actually use from this baseline is far smaller. Vendoring drags the v0/v1 dead weight along for the ride.
2. **The required modifications are not edits.** v2's want-list serialiser is structurally different from v0/v1's (no caps in `want` lines; instead, a `command=fetch` keyword followed by `want`/`filter`/`have`/`done` argument lines). `git_smart__detect_caps` cannot recognise v2's cap-advertisement section. `git_smart__connect` has no ref-advertisement to parse on v2. Each of these is a new function next to the v0/v1 one, not a patch.
3. **The dependency surface is not self-contained.** Vendoring smart-transport drags in libgit2's internal utility layer (`git_str`, `git_vector`, `git_pool`, `git_revwalk`, packbuilder hooks) — either we vendor those too, or we link libgit2 proper and live with the brittle assumption that internal-API stability is preserved across libgit2 releases.

When libgit2 eventually ships v2 natively the API shape will not match our fork (their `transport_smart_caps` will likely become a discriminated union over v0/v1/v2, not bitfields-plus-additions), so "switch back to upstream" still requires non-trivial re-integration regardless of which path we take now.

From-scratch wins on every axis: smaller artifact, no upstream-divergence cost, no GPL-with-linking-exception propagation to audit, no internal-API coupling. Watch libgit2#5564 (see §10); if it lands, retire our transport. Until then, the from-scratch code is small enough to maintain indefinitely.

### 3.2 Whitelist `partialclone` so libgit2 opens the repo

After the initial `--filter=blob:none` fetch, the bare repo's `.git/config` has `extensions.partialClone = origin`. libgit2 today errors on unknown extensions ([repository.c#L1958](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/repository.c#L1958)). We tell it `partialclone` is OK by calling once at startup:

```cpp
const char * exts[] = { "partialclone" };
git_libgit2_opts(GIT_OPT_SET_EXTENSIONS, exts, 1);
```

[`GIT_OPT_SET_EXTENSIONS`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/common.h#L522) inserts `"partialclone"` into the `user_extensions` vector. `check_valid_extension` at [`repository.c#L1925-L1944`](https://github.com/libgit2/libgit2/blob/0609130af/src/libgit2/repository.c#L1925-L1944) checks `user_extensions` first; the partial-clone extension is now accepted, so `git_repository_open` succeeds. **No config-file mutation needed.**

### 3.3 `PromisorBackend` — the low-priority writable ODB backend that fetches missing blobs

Once the repo is open, missing-blob reads need to dispatch back to the remote. The right libgit2 mechanism is a low-priority writable backend installed via [`git_odb_add_backend`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/odb.h) with a negative priority — consulted last (after the main pack/loose backends miss), but writable so it can ingest the fetched packfile via `git_indexer_commit` directly into the repo's pack directory. (`git_odb_add_alternate` is read-only and unsuitable for that ingest path.)

The backend's `git_odb_backend` callbacks ([`sys/odb_backend.h#L27-L107`](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/sys/odb_backend.h#L27-L107)) cover the surface:

- **`exists(oid)`**, **`read(oid)`**, **`read_header(oid)`** — for these we issue a single-blob fetch via the same `PartialCloneTransport` machinery (omitting `thin-pack` from the request, since we have no commit/tree graph available locally to resolve thin-pack delta bases against — see §3.5), ingest the response packfile via `git_indexer_*`, and return the requested data.
- **`refresh()`** — libgit2 calls this automatically on failed lookups ([sys/odb_backend.h#L68-L75](https://github.com/libgit2/libgit2/blob/0609130af/include/git2/sys/odb_backend.h#L68-L75)). We use it to invalidate stale metadata if needed.

After `git_indexer_commit`, the just-written pack is part of the main pack/loose backend, so subsequent reads short-circuit before ever reaching `PromisorBackend`. A repeated read costs nothing extra.

Installation is gated on `isPartialClone(repo)`: `git_config_get_string_buf(&buf, cfg, "extensions.partialclone")` returning 0 means the config key is present (the post-`--filter=blob:none` clone writes that key by convention). On non-partial-clone repos the backend is not installed, avoiding the per-OID fallthrough cost. ~10 LOC.

### 3.4 Putting it together

```cpp
// One-time, at process startup:
void initLazyFetch() {
    const char * exts[] = { "partialclone" };
    git_libgit2_opts(GIT_OPT_SET_EXTENSIONS, exts, 1);
    git_transport_register("https", PartialCloneTransport::create, nullptr);
    // ... and "http", "git"; ssh later if needed.
}

// In src/libfetchers/git-utils.cc, GitRepoImpl constructor (sketch — the
// real code uses libgit2's RAII Setter/Owner wrappers around raw pointers):
GitRepoImpl::GitRepoImpl(std::filesystem::path path, Options opts) {
    check(git_repository_open(Setter(repo), path.string().c_str()));
    git_odb * odb;
    check(git_repository_odb(&odb, repo.get()));
    if (isPartialClone(repo.get())) {
        // The promisor URL is the remote that the original --filter=blob:none
        // clone was made against. After clone, this is recorded in the repo's
        // config as remote.<name>.url for the remote whose name is recorded
        // in extensions.partialclone (typically "origin"). Resolve it via:
        std::string remoteName, remoteUrl;
        {
            git_buf buf = GIT_BUF_INIT;
            git_config * cfg;
            check(git_repository_config_snapshot(&cfg, repo.get()));
            Finally cfgCleanup([&]{ git_config_free(cfg); });
            check(git_config_get_string_buf(&buf, cfg, "extensions.partialclone"));
            remoteName = buf.ptr;
            git_buf_dispose(&buf);
            check(git_config_get_string_buf(&buf, cfg,
                ("remote." + remoteName + ".url").c_str()));
            remoteUrl = buf.ptr;
            git_buf_dispose(&buf);
        }
        promisor = std::make_unique<PromisorBackend>(std::move(remoteUrl));
        // Negative priority: consulted last, after main pack/loose backends.
        check(git_odb_add_backend(odb, &promisor->backend, /*priority=*/-1));
    }
}
```

### 3.5 Coalescing: prefetch on `SourceAccessor`, trigger in `EvalState`

The unmitigated cold-eval latency floor of partial-clone is severe: each `git_odb_backend::read` callback is synchronous (must return the requested blob before returning), so a naive backend issues one HTTPS round-trip per missing blob. On a nixpkgs-scale cold eval that is tens of thousands of round-trips; at a typical 50 ms RTT, hours of wall-clock. Without coalescing, item h's v1 would be slower than today's full clone for any cold cache. Coalescing is therefore a v1 prerequisite, not a future-work optimisation.

Coalescing inside `PromisorBackend::read` itself is structurally not possible — `read` is synchronous and per-OID. The right architecture splits mechanism from trigger:

- **Mechanism (on `SourceAccessor`).** Add a `SourceAccessor::prefetchSubtree(const CanonPath & subpath, unsigned depth = 1, PathFilter & filter = defaultPathFilter)` virtual to the base, defaulting to a no-op. The `filter` parameter lets wrappers push their `isAllowed` predicate down so the inner accessor doesn't enumerate blobs that wrapping filters would reject; most accessors (FS-backed, store-object-backed, anything without a notion of "blob OIDs to prefetch") ignore the parameter because their no-op default doesn't enumerate anything. `GitSourceAccessor` overrides it: **gates on `isPartialClone(repo)` and no-ops if false** (a full-clone repo has all blobs local; the multi-want fetch would either re-fetch already-local blobs or dispatch an empty want-list); when partial-clone, walks the tree rooted at `subpath` (already loaded under `filter blob:none`, since trees are local), enumerates reachable blob OIDs to a bounded depth *whose paths satisfy the filter and whose OIDs are not already in the local ODB* (skip already-present blobs to avoid re-fetch), and dispatches a single multi-want `command=fetch` through the `PromisorBackend` for them. **Default depth = 1** (the directory's own files, no recursion) because the trigger pattern is `prim_readFile X` calls `prefetchSubtree(parent(X))`, fetching X plus its directory neighbours; the eval is unlikely to read the *whole* subtree (which could be a large monorepo directory like `/areas/`). Triggers that know they want a single-blob fetch (e.g. `coerceToPath` for a string-context-resolved path that points to a single file) pass `depth = 0`. **Going deeper than 1 is dangerous on wide directories**: a `prim_readDir` trigger that pre-fetched depth=2 against `nixpkgs/pkgs/` would fetch tens of thousands of unrelated blobs. The over-fetch fallback (when a `read` fires without a prior `prefetchSubtree`) handles depth>1 cases by speculatively widening from each cold-read site, naturally bounding waste to one round-trip per cold spot. ~80 LOC for the override; ~3 LOC for the base default.

- **Wrapper forwarding (load-bearing — without it the trigger silently no-ops).** `MountedSourceAccessor::prefetchSubtree` resolves `path` through its mounts and forwards: `auto [accessor, subpath] = resolve(path); accessor->prefetchSubtree(subpath, depth, filter);`. `FilteringSourceAccessor::prefetchSubtree` builds a composite filter that `AND`s its own `isAllowed` with the caller's `filter`, accounting for the path-namespace shift across the prefix: at the inner accessor's namespace, the path under `prefix / X` corresponds to wrapper-relative `X` for `isAllowed`'s purposes. Concretely: `PathFilter composite = [&](const std::string & innerAbsP){ auto inner = CanonPath(innerAbsP); auto wrapperRel = inner.removePrefix(prefix); return isAllowed(wrapperRel) && filter(innerAbsP); }; next->prefetchSubtree(prefix / path, depth, composite);`. The `removePrefix` call returns the wrapper-relative path that `isAllowed` expects (master's `FilteringSourceAccessor::isAllowed` takes wrapper-relative `CanonPath`s, so the composite is responsible for stripping the prefix back off). Filter-aware enumeration in `GitSourceAccessor::prefetchSubtree` skips blobs whose path the composite rejects, so wrapping accessors automatically push their predicates down without `GitSourceAccessor` knowing about them. ~40 LOC for the wrapper overrides. **Without this**, the dominant accessor chain `MountedSourceAccessor` → `GitExportIgnoreSourceAccessor` → `GitSourceAccessor` short-circuits at the first wrapper's no-op default, and the trigger does nothing.

- **`.gitattributes` blobs need to be fetched before filter evaluation.** `GitExportIgnoreSourceAccessor::isAllowed` consults `.gitattributes` along the path — these are themselves blobs that may not be local under `--filter=blob:none`. The first `prefetchSubtree` call against an exportIgnore-input would synchronously fetch each ancestor `.gitattributes` blob (one round-trip per ancestor) before the bulk-fetch can begin filtering. **Mitigation**: `PartialCloneTransport`'s `command=fetch` request includes a per-path filter that exempts `.gitattributes` blobs from `--filter=blob:none` (git's protocol-v2 supports `filter=combine:blob:none+sparse:oid=<sparse-spec>`-style composition; the simpler form is to issue a separate small fetch for `.gitattributes` blobs reachable from the requested tree before the main fetch). ~20 LOC of pre-fetch logic in `prefetchSubtree`'s `GitExportIgnoreSourceAccessor` override. The first-eval cost is now one extra small RTT for `.gitattributes`, which is bounded.

- **Trigger (in `EvalState`).** Wire prefetch calls from the points where the evaluator already knows it's about to read blob content: `EvalState::evalFile` (when `import` resolves a path), `EvalState::coerceToPath`, `prim_readDir`, and `prim_readFile`. **`prim_pathExists` is deliberately NOT a trigger** — existence checks read the tree skeleton (already local under `filter blob:none`), not blob content; triggering a blob-prefetch for `pathExists` is wasted bandwidth. **`prim_readFile` is the load-bearing trigger** for the headline manifest-and-config workloads: tecnix's manifest reduces to `builtins.fromJSON (builtins.readFile (worldRoot + "/.meta/manifest.json"))`, and `prim_readFile` triggers `prefetchSubtree(parent(/.meta/manifest.json), depth=1)` — fetching the manifest blob plus its directory neighbours in one round-trip rather than one per blob. **Trigger-specific depth defaults**: `prim_readFile`/`coerceToPath`/`prim_readDir` use depth=1 (just the directory siblings); `evalFile` uses depth=2 (siblings plus one level deeper) because Nix import chains typically nest several levels (`A.nix` imports `B/default.nix` which imports `B/sub/default.nix`), and depth=2 amortises the round-trips along that path; the over-fetch risk is bounded because Nix expression files cluster in directories shallow enough that depth=2 is rarely a multiplier larger than 10. ~80 LOC of trigger sites. Per-`EvalState` dedupe via a `boost::unordered_flat_set<git_oid>` of already-prefetched parent-trees keeps trigger frequency bounded — the same import path resolved twice in one eval issues one prefetch.

If a `read(oid)` fires without a prior `prefetchSubtree`, a fallback can speculatively widen the fetch to siblings reachable from the OID's parent tree (over-fetches when the eval really wanted only one file in a wide directory, but bounded). The fallback is a quality-of-implementation detail; the `prefetchSubtree`-driven path is what gives bounded latency on the dominant workloads.

On a typical nixpkgs flake metadata workload (~5 imported files + manifest reads), the trigger-driven path gives ~5–10 RTTs, not ~120 — the difference between "item h is unusable on cold eval" and "item h is fast on cold eval." Total surgery is ~230 LOC (mechanism ~80 + base default ~3 + wrapper forwarding ~40 + `.gitattributes` pre-fetch ~20 + triggers ~80, summed against the §3 budget). An earlier version of this section flagged a "defensive override of `FilteringSourceAccessor::readDirectory(path, callback)`" as a sound-but-non-optimisation issue; cross-tree audit against master shows the existing default (`callback(*this, dirPath)`) is correct — the wrapper passes itself as the scoped accessor and `dirPath` as the prepend-prefix, and any subsequent `readFile`/`readDirectory` calls through the wrapper apply `prefix / X` and `isAllowed` as expected. No defensive override needed.

### 3.6 Choice of v2 implementation

libgit2 has no protocol-v2 support today and the upstream issue tracking it (libgit2#5564) has been stalled since 2020 (see §10). Three viable paths exist for getting `command=fetch` with `filter blob:none` into Nix:

1. **From-scratch v2 layer in C++** (the proposal's primary path). Reuse libgit2's existing pkt-line framing, sideband demultiplexing, and packfile indexer for the bulk of the work; add the v2-specific surface ourselves: detect `version 2` in the cap-advertisement, emit `command=fetch` arguments with `want <oid>` / `filter blob:none` / `done` (omitting `thin-pack` on `PromisorBackend`-issued reads — see below), parse the response via the existing indexer + ODB stack. The minimal v2 surface is approximately 500 LOC, not the low-thousands the original sketch implied; the larger figure was an anchor against the full v0/v1 implementation, which we do not need to replicate. The hard work libgit2 already does is reused; the new code is the v2-specific framing.

2. **`gitoxide` (Rust) via FFI.** `gix-protocol` ships v2 + `filter blob:none` as a library API today (verified via `docs.rs/gix-protocol`'s `fetch::Arguments::filter`). Vendoring it would replace the from-scratch v2 layer entirely. Cost: a Rust toolchain dependency in Nix's bootstrap closure — affects bootstrap, cross-compilation, and the existing C++/meson build pipeline. This is approximately 1–2 weeks of release-engineering and a non-trivial governance decision. Saving: ~500 LOC of new C++ that this proposal otherwise commits to owning indefinitely.

3. **Vendor and patch libgit2's smart transport.** Rejected for the structural reasons in §3.1: v2 changes are new functions next to v0/v1, not edits, and the dependency surface is not self-contained.

The recommendation is **path 1 for the first implementation, with an explicit escalation gate to path 2 if prototyping reveals hidden complexity that pushes the budget toward libgit2's full v0/v1 anchor (~2,902 LOC).** Documenting the gitoxide alternative on the page is what was missing from the original sketch; the choice to ship from-scratch is defensible only with the comparison made.

### 3.7 Expected envelope (qualitative)

The wire and disk reduction is **one to two orders of magnitude on workloads where blobs dominate repo size** — i.e., monorepos where the commit and tree objects are a small fraction of the packed total. For a `--filter=blob:none` fetch the wire and disk costs reduce from "everything" to "commits + trees + the blobs the evaluator actually reads," which for typical evaluation accessing a single package's source is a small subset. The exact ratio depends on repo structure (blob-to-tree ratio, packfile compression, history depth) and is not measured here. Concrete benchmarks against a real monorepo are needed to commit to specific numbers.

Wire-mechanism details, all verified against master / libgit2 source:

- **Omit `thin-pack` on `PromisorBackend` reads.** v2 `command=fetch` accepts `thin-pack` as an opt-in argument; sending it instructs the server it may emit a thin pack with REF_DELTA bases the client is expected to have locally. For a single-OID promisor read the client has only the commit/tree skeleton, no blob bases, so a thin-pack response causes `git_indexer_append` to fail with `GIT_ENOTFOUND` when `fix_thin_pack` looks up the missing base. *Not sending* `thin-pack` instructs the server to send a self-contained pack. Initial-clone fetches keep `thin-pack` (libgit2's existing default behaviour, where the local ODB does have the bases); promisor-only fetches omit it.

- **Half-duplex POST is structural in `FileTransfer`.** `TransferItem::init` sets `CURLOPT_POSTFIELDSIZE_LARGE = request.data->sizeHint` up-front, so the upload size is committed before the request opens. The `PartialCloneTransport` must therefore serialise the entire `command=fetch want want filter blob:none done` block to a `std::string` before opening the request. v2 stateless-RPC matches this shape — each `command=` is self-contained — so this is a documentation requirement, not a blocker. Streaming the want-list while reading the response is not possible with the current `FileTransfer` shape.

- **Fallback for servers without v2/filter support.** If the server doesn't advertise `version 2` and the `filter` capability, `PartialCloneTransport` returns a regular smart-transport allocation rather than a `pc_transport`. This is per-remote and cached in a TTL-keyed set of known-bad hosts, not a global flag-flip — which keeps it race-free against other `GitRepoImpl` instances mid-fetch.

- **`partialclone` extension whitelist is process-global state.** `git_libgit2_opts(GIT_OPT_SET_EXTENSIONS, ...)` mutates a `static git_vector user_extensions` in libgit2's `repository.c`. If Nix is embedded in a host that later calls the same opt with a different list, the `"partialclone"` entry is dropped. Document for embedders; do not attempt to merge with another consumer's list at runtime.

- **The extension whitelist does not install a backend.** libgit2 does not auto-wire promisor backends in response to `extensions.partialclone` — the whitelist's only job is to keep `git_repository_open` from rejecting the config. Backend installation is the `GitRepoImpl` ctor's responsibility (§3.4).

- **`flock` on `objects/pack/.lock`** during `git_indexer` operations prevents concurrent `nix` invocations against the same repo from corrupting each other's pack writes. Small but mandatory.

For workloads where item h is rejected as inappropriate (e.g. CI runners with plentiful bandwidth and ephemeral disks), a documented fallback is `git_fetch_options.depth = 1` (single-commit shallow clone, supported by libgit2 today). Strictly worse than `filter blob:none` on wire bytes (you receive every blob in the tip commit before any local filtering), but requires zero new transport code, no v2 implementation, no `partialclone` extension — the path of least resistance for "give me less than a full history clone, no other changes."

### 3.8 The user sees nothing change

Nothing in the Nix expression changes. `builtins.fetchTree { type = "git"; url = X; rev = Y; }` works the same. The decision of whether to fetch lazily is made at the C++ level: if the server advertises `filter` in its protocol-v2 capabilities, we use partial clone; otherwise we fall back to the existing full clone. **No flake-input attribute, no setting, no user opt-in.** Servers that support partial clone (GitHub, GitLab, Gitea, Forgejo, gitea.io, self-hosted with `uploadpack.allowFilter=true`) automatically benefit; servers that don't are unaffected.

One implementation-level note worth recording: the `param` passed to `git_transport_register` is borrowed for the lifetime of registration — libgit2 does not free it on `git_transport_unregister`. Pass a pointer to a process-lifetime `FileTransfer` singleton.

---

## 4. Subtree fetching (two layers, two questions)

(Items **j** and **k** are concrete instances of the algebra in §0: the cache-key shape `(parentTreeSha, method, name) → StorePath` is the same content-addressed-projection shape Bazel RE's `Directory` / `DirectoryNode` digests have used in production since 2018, and the soundness law — equal tree-SHAs ⇒ equal narrowed StorePaths — is the Merkle-content-equality property the git ODB already guarantees structurally.)

**Scope: tree-shaped inputs, not just `git://`.** Nix already unpacks tarballs into a shared bare git ODB at `$XDG_CACHE_HOME/nix/tarball-cache-v2`. The pipeline is: `Settings::getTarballCache()` returns a `GitRepoImpl` (constructed with `packfilesOnly = true`, a libgit2 optimisation that excludes the loose-object backend to avoid ENOENT spam on misses); `unpackTarfileToSink` walks tar entries and feeds them to `GitFileSystemObjectSinkImpl`, which calls `git_blob_create_from_buffer` (and `git_blob_create_from_stream` for large files) and `git_treebuilder_*` to write real git objects into the ODB; the result is a tree-OID accessed via a `GitSourceAccessor` rooted at that OID. **`tarball`, `github`, `gitlab`, and `sourcehut` inputs all share this `tarball-cache-v2` ODB**; **`git://` inputs use a separate per-URL `gitv3/<hashed-url>/` repository** (the protocol-v2 fetch lands in a fresh per-remote repo, not the shared cache). The accessor *type* is `GitSourceAccessor` for both — so items **g**, **j**, and §5 (filter identity) apply uniformly across all five fetcher types — but the *ODB instance* differs: the `tarball-cache-v2` is the share point, not all tree-shaped fetches. Item **h** (partial clone) is git-protocol-specific because tarballs have no protocol-level partial-fetch primitive; partial-clone wins on `git://` inputs land in the per-URL `gitv3` repo, not in `tarball-cache-v2`, so they do *not* propagate to subsequent tarball fetches. Cross-fetcher blob dedup happens via independent unpacks producing identical tree/blob OIDs (Merkle-content equality) that each repo computes locally, not through ODB sharing. The only fetcher genuinely outside the tree-shaped optimisations is the `file` input scheme (single-blob, goes straight to NAR/store via `addToStore`, uses a plain `getFSAccessor`, no git ODB) — and tree-shaped optimisations don't apply to single blobs anyway. **There is no garbage-collection story for either the `tarball-cache-v2` or the per-URL `gitv3` repos today**; both grow unbounded and are user-cleaned via `rm -rf`.

**The split has historical, domain-specific reasons.** Per-URL `gitv3` was introduced in 2018 (Graham Christensen, commit `02098d207` "fetchGit: use a better caching scheme"): a single shared git ODB held multiple unrelated repos and `git fetch` was "maddeningly slow" trying to find common commits between large unrelated histories. The shared `tarball-cache-v2` was introduced in 2023 (Eelco Dolstra, commit `b36857ac8` "Add a Git-based content-addressed tarball cache") explicitly *for* cross-rev dedup of the same repo. The two decisions are domain-specific and were never reconciled. A user with `inputs.nixpkgs.url = "github:NixOS/nixpkgs/<rev>"` (writes to `tarball-cache-v2`) plus a local-clone override (`--override-input nixpkgs git+file:///home/me/nixpkgs`, writes to `gitv3/<hash>/`) duplicates the snapshot tree at `<rev>` in two places — order-of-magnitude ~7% of nixpkgs disk in the worst case (~150 MB tarball-cache snapshot vs ~2 GB full-history clone).

**The unification is a small future item** — call it **m**. libgit2's `git_odb_add_alternate` and the standard `objects/info/alternates` file form let a per-URL `gitv3/<hash>/` repo *read* objects from `tarball-cache-v2` without copying them. One write of `gitv3/<hash>/objects/info/alternates` containing the absolute path to `tarball-cache-v2/objects` is the whole change. Importantly, **the alternate is read-only and consulted only after main backends miss**, so the 2018 fetch-time-performance regression that drove the per-URL split does *not* recur — alternates do not trigger common-commit walks during `git fetch`. **Without item h, the dedup benefit is read-only** (objects already in the alternate are visible to `git_odb_read` but the per-URL fetch still receives a full pack — disk duplication unchanged). **With item h's `PromisorBackend`, the alternate becomes a real bandwidth win**: `--filter=blob:none` clones backfill blobs from the network only when *neither* the per-URL pack *nor* the `tarball-cache-v2` alternate has them. Item m therefore composes naturally with item h and is left as future work because the bandwidth payoff is gated on item h shipping.

**The proposal targets one of two distinct questions.** Cross-tree verification surfaced a precedent that an earlier draft of this proposal mis-framed as a "structural impossibility":

- **Layer 1 (existing): `dir` at the FlakeRef layer.** `inputs.foo.url = "github:owner/repo?dir=areas/foo"` works today. `FlakeRef` is `(Input, std::string subdir)`; the Input's narHash is whole-repo; `call-flake.nix` resolves user-visible `outPath = sourceInfo.outPath + "/" + dir` by string concatenation; the lock records `dir` in the locked entry. **Subtree-as-pointer is already user-visible**, just at a layer above where narHash lives.
- **Layer 2 (this proposal): accelerate the access pattern that `dir` produces.** The whole repo is mounted once; `${input}/sub` (or `dir = "sub"` after `call-flake.nix` glues it on) is the dominant evaluation pattern. Item **j** makes access through this subpath as cheap as if the subtree were a first-class Input — without changing what the lock records or what narHash means.

**The harder question — making the *subtree* the unit of Input identity** — would mean a subtree-rooted accessor producing a subtree-rooted narHash, isolated subtree storepath, lock entry whose narHash hashes only the subtree. The previous draft of this section claimed the narHash invariant in `mountInput` made this structurally impossible; that claim was wrong. The narHash gate is *scope-symmetric* — it compares `narHash(rootOfTheAccessor)` against `lockedInput.narHash`; both sides come from the same accessor. A subtree-rooted accessor produces a subtree-rooted narHash and the gate passes. `Input::computeStorePath` is content-addressed and scope-blind; substituters serve NARs by hash without any opinion about whole-repo vs subtree. The actual cost is **~550 LOC of fetcher plumbing** in `git.cc`/`github.cc`/`flakeref.cc`/`call-flake.nix`, plus a `version: 8` lockfile bump that records `subPath` (or re-interprets `dir` at the fetcher layer when present), plus a migration story for existing `${input.outPath}/sub` patterns. That work is feasible; it is not part of this proposal because it is its own RFC's worth of design choices (option A: re-define `dir`'s semantics at the fetcher layer, back-incompat for old flakes; option B: add `narHashScope` or `subPath` to the locked entry, gated by `version: 8`, opt-in per input). Tecnix's `tectonixZoneCache_` is an existence proof at the application layer that the mechanism works; generalising it to flake inputs is a separate, sized piece of work.

**This proposal stops at Layer 2** — accelerating access through `dir`-style subpaths via item **j** for the `builtins.path` / `builtins.filterSource` code path — and stages Layer 1 (subtree-as-Input first-class) as future work in §11. The remainder of this section explains the Layer-2 mechanism.

### What `${input}/sub` looks like to C++ (Layer 2 access pattern)

Consider `t = builtins.fetchTree {...}; src = t.outPath + "/sub";` (or equivalently a flake input with `dir = "sub"`, which `call-flake.nix` resolves into the same string-with-context shape). The execution order is:

1. `fetchTree` runs `EvalState::mountInput` ([`paths.cc`](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/paths.cc)). The comment in `mountInput` is decisive: *"To mount the input, dryRun is sufficient. We still compute the narHash (to check for mismatches) and the store path to figure out where to mount it."* The dryRun walks the entire tree to compute its narHash. **The accessor mounted is whole-repo** — the input scheme's `getAccessor` returns it rooted at the commit's full root tree-OID, and `mountInput` does not narrow.
2. `outPath` becomes the string `"/nix/store/HASH-source"`. The `+ "/sub"` (or `call-flake.nix`'s `+ "/" + dir` glue) is plain Nix string concatenation. From C++'s perspective, the result is a string with attached store-path context — there is no structured "user accessed `/sub`" event the fetcher can observe at this point.
3. Materialisation happens later, via `EvalState::ensureLazyPathsCopied`, which iterates `Opaque{path}` context elements. It calls `ensureLazyPathCopied(o.path)` with the **whole input's store path**, not a subpath.

So by the time C++ knows `/sub` was the user's interest, the entire tree has already been NAR-walked. **Layer 2's job** is to short-circuit this walk on second eval (and on cross-rev sharing) — without changing what `mountInput` does to the parent.

### Why the parent walk is unavoidable at Layer 2

The `mountInput` narHash gate (in `EvalState::mountInput`, `src/libexpr/paths.cc`) calls `fetchToStore2(fetchSettings, *store, accessor, FetchMode::DryRun, input.getName())`, producing `(storePath, narHash)` from a dryRun walk of the *supplied* accessor, then mounts that same accessor on `storeFS`, then compares `narHash != *originalInput.getNarHash()`. **At Layer 2, the supplied accessor is whole-repo**, so the dryRun's narHash is whole-repo, so the dryRun must walk the whole tree at least on first contact. Item **h** (lazy clone) reduces *wire* cost (commits + trees, blobs deferred); the dryRun *walks* every blob anyway because narHash is over content. The proposal's `mountInput` deferral options (the three-option menu in "What item **h** *does* deliver" below — `LazyAttr` for `narHash`, `LazyStorePathString`, "trust the lock") relax *eagerness* of the narHash computation but don't change its *scope*. **Layer 1 — making narHash subtree-scoped — is the only way to skip the whole-repo walk first-eval**, and it is out of scope here.

### `getFingerprint` is the canonical content-identity surface

Before the item-j sketch, name the canonical pattern: **`SourceAccessor::getFingerprint(path) → (CanonPath, optional<string>)` is the single place content-keyed identity is computed for a source.** Every concern that affects what a path's content "really is" — subtree narrowing, dirty workdir state, `.gitattributes` filtering, LFS smudging, user-supplied `PathFilter` lambdas — is encoded as a *suffix* on the fingerprint string, with the path possibly re-anchored to root. The `fetchToStore` cache key is then `(domain="sourcePathToHash", fingerprint, method, path)` — uniformly. Special cases inside `fetch-to-store.cc` (the existing filter-bypass ternary; future per-feature carve-outs) collapse into wrapper accessors that override `getFingerprint`.

**Cheap-content-keyed where available, coarse otherwise.** Not every accessor can produce a content-keyed fingerprint cheaply. `GitSourceAccessor` can (the git ODB has OIDs in hand); `PosixSourceAccessor` cannot (computing per-file content hashes would double disk IO since `addPath` re-reads the file). The canonical pattern is *opportunistic*: where the accessor implementation has a cheap content identity, return the content-keyed form (item j's `tree:<sha>` / `blob:<sha>`); otherwise return the inherited whole-accessor fingerprint (master's existing default — `{path, fingerprint}`). Wrappers always forward and append, regardless of whether the inner fingerprint is content-keyed or coarse — so a workdir-rooted fingerprint with `;f=<hash>` from `FilterIdentitySourceAccessor` still gets the filter-aware cache key, just keyed against the parent input rather than per-file content. Coarse fingerprints lose cross-rev sharing for the inner identity but retain cross-eval sharing for the outer concerns.

This canonicalisation is a precondition for the §0 algebra to actually compose. Without it, every new concern that affects content identity becomes a fresh special-case in `fetchToStore`'s key-construction; with it, every concern is a `getFingerprint` override and they compose by string concatenation under a documented suffix schema.

#### Suffix schema (versioned, documented)

The fingerprint string is a sequence: `<root-tag>[;<suffix>]*`, where:

- **Root tags** identify what the fingerprint refers to:
  - `git:<rev>` — a git commit (whole-input, parent-rev-keyed). Existing master form.
  - `tree:<sha>` — a git tree (subtree, content-keyed by tree-SHA). New for item j.
  - `blob:<sha>` — a git blob (single-file, content-keyed by blob-OID). New for item j (file branch).
  - `nar:<hash>` — a NAR-content-addressed source (tarball pipeline, etc.). Existing.
- **Suffixes** modify identity by some additional concern, in canonical order:
  - `;s` — submodules enabled. Existing.
  - `;e=<hash>` — exportIgnore enabled, with the relevant `.gitattributes` content hashed in. Item j refinement.
  - `;l` — LFS smudging enabled at the input level (input-policy flag, not a per-blob hash). Existing master form on `git:<rev>` fingerprints, extended by item j to also apply on `tree:<sha>` and `blob:<sha>` since LFS smudging is content-affecting (an LFS-pointer blob OID under `lfs=true` smudges to different bytes than the raw pointer-file bytes under `lfs=false`). Distinct from the future `;lfs=<smudge-hash>` (per-blob smudge identity, item l).
  - `;d=<hash>` — dirty workdir, with deleted+modified files content-hashed. Existing master form.
  - `;f=<hash>` — user-supplied `PathFilter` identity. Item §5.
  - Future: `;lfs=<hash>` (per-blob LFS-smudge-cache key — see §6), `;<concern-tag>=<hash>` for new concerns.

Suffixes are ordered alphabetically by tag character to give a canonical form (`;d` before `;e` before `;f` before `;l` before `;s`); two clients computing the same fingerprint produce byte-identical strings. **Versioned domain prefix** (`fingerprint-v1\0` baked into the cache row's domain string) lets future revisions coexist if the schema changes.

**NAR canonicalisation version is *not* in the schema today.** Master's NAR format has evolved over Nix releases; two clients on different Nix versions might produce different NAR bytes from the same tree-SHA + fingerprint. The cache rows for `sourcePathToHash` therefore inherit the cross-version-skew caveat from §0's join-semilattice analysis: equal keys produce equal values *modulo NAR-canonicalisation version*. Master accepts this risk (its existing cache rows don't tag NAR version either). The future fix is a `cache_v<N>` schema bump that includes the NAR version; out of scope for this proposal.

#### Soundness invariant

The return type is `(CanonPath, optional<string>)`: a *(returned-path, fingerprint)* pair that identifies a content-anchored cache cell. The two components are read together — `returned-path` is the cell's name *at the level the fingerprint anchors at*. For a tree-rooted `tree:T` fingerprint, `returned-path` is `CanonPath::root` (the tree-SHA's namespace anchors at root). For a parent-rev-keyed `git:R` fingerprint, `returned-path` is the in-input subpath. The pair always reads as "the cache cell named `returned-path` under the namespace identified by `fingerprint`."

The soundness invariant: **two `(returned-path, fingerprint)` pairs that match must produce the same NAR contents under any consumer (`addPath`, `addToStore`, `dumpPath`).** This is the projection-pattern soundness law (§0) made concrete for the source accessor. The path component is part of the cell identity, not a redundant return — `MountedSourceAccessor::getFingerprint` strips the mount prefix; item j re-anchors to root for content-keyed cases; both legitimately return a path that differs from the input.

A new suffix is sound iff the concern it represents is *content-determined*: the suffix value is computable from the source content alone, and adding/removing it changes the NAR contents in a determinable way. `;e=<hash>` is sound because exportIgnore is determined by `.gitattributes` content. `;f=<hash>` is sound because the filter's identity is determined by lambda source-bytes + captured-value content (§5). `;d=<hash>` is sound because dirty-file content is hashed in.

A concern that is *not* content-determined cannot get a suffix and must bail out of the cache (return `nullopt` for the fingerprint). The `;f` suffix bailing to `nullopt` for unhashable filters is the existing pattern.

#### Composition rule

Wrapper accessors compose by appending suffixes. `FilteringSourceAccessor::getFingerprint` (base class) currently does:

```cpp
if (fingerprint) return {path, fingerprint};   // master: short-circuits
return next->getFingerprint(prefix / path);
```

**The short-circuit is incompatible with item j.** When `Input::getAccessorUnchecked` (`fetchers.cc:367`) sets `accessor->fingerprint = result.getFingerprint(store)` on the *outermost* wrapper (the master pattern), the wrapper's `getFingerprint` returns the coarse `git:<rev>+flags` form for *every* path, and the inner `GitSourceAccessor`'s path-aware override never runs. Two evaluations of `${input}/sub` against different revs of the same monorepo with the same subtree-SHA would both return `git:<rev1>` and `git:<rev2>` respectively — no cross-rev sharing.

**`computeOwnSuffix` semantics**:
- Return `nullopt` when the wrapper's concern is *legitimately unhashable* — e.g., `FilterIdentitySourceAccessor` for a filter that captures a non-deterministic value (`builtins.currentTime`-shaped). Caller bypasses the cache.
- Return non-empty string when the wrapper has a suffix to add. The suffix's content-determined value must come from data the wrapper can compute deterministically.
- Return empty string when the wrapper has nothing to add (e.g., `MountedSourceAccessor`'s default — it just routes).
- *Throw* if the wrapper's data isn't available (e.g., a network failure fetching `.gitattributes` under partial-clone). Network failures propagate as errors; they're not "unhashable" — they're "unreachable."

**The canonical pattern requires changing the base class to "forward and append, falling back to the wrapper's field if the inner has no content-keyed identity":**

```cpp
// New FilteringSourceAccessor::getFingerprint:
auto [innerPath, innerFp] = next->getFingerprint(prefix / path);
if (!innerFp && fingerprint) {
    // Inner has no content-keyed identity (e.g. PosixSourceAccessor for
    // a workdir input); the wrapper's `fingerprint` field carries the
    // input-level identity (master's existing assignment at
    // fetchers.cc:367). Return the field; this is master's short-circuit
    // behaviour, preserved for the case where it's correct.
    return {path, fingerprint};
}
if (!innerFp)
    return {innerPath, std::nullopt};
// Inner has content-keyed identity (item j's tree:T / blob:B for
// commit-rooted git accessors). Forward and append our own suffix.
//
// computeOwnSuffix returns:
//   nullopt — wrapper signals "this concern is unhashable; bypass" (e.g. unhashable filter);
//   empty   — wrapper has no suffix to add at this path (default base);
//   non-empty — the suffix (e.g. ";f=<hash>" or ";e=<hash>") to splice into innerFp.
auto suffix = computeOwnSuffix(path);
if (!suffix)
    return {innerPath, std::nullopt};   // bypass
if (suffix->empty())
    return {innerPath, innerFp};
return {innerPath, mergeSuffix(*innerFp, *suffix)};   // alphabetical splice
```

**Two distinct cases handled by one virtual.** Workdir inputs (`AllowListSourceAccessor` over `PosixSourceAccessor`): inner returns `{path, nullopt}` because `PosixSourceAccessor` has no content-keyed identity; wrapper's `fingerprint` field (set by `Input::getAccessorUnchecked`) carries the input-level fingerprint `git:R;d=H`; the wrapper returns `{path, "git:R;d=H"}`. Commit-rooted git inputs (`GitExportIgnoreSourceAccessor` over `GitSourceAccessor` with item j): inner returns `{root, "blob:B"}` content-keyed; wrapper appends `;e=<hash>` → `{root, "blob:B;e=<hash>"}`. Both cases produce sound cache keys for their respective use cases.

This is the structural change that lets item j's override compose correctly with master's wrapper-fingerprint assignment, while preserving cache-keyed access for workdir inputs (which item j doesn't accelerate).

**Corollary: `GitSourceAccessor::fingerprint` set at construction with `;l` and `;s` flags.** The allocation of which flags live on the leaf vs which on a wrapper follows a pragmatic rule: **a flag is allocated to whichever accessor implements the concern**. If a wrapper class exists for the concern, the wrapper allocates the flag via `computeOwnSuffix`; otherwise the leaf carries it in its `fingerprint` / `flagSuffix` fields.

- `;e=H` (export-ignore gitattributes hash): allocated by `GitExportIgnoreSourceAccessor::computeOwnSuffix`, the existing wrapper for export-ignore. Path-dependent (gitattributes vary per directory).
- `;f=H` (filter identity): allocated by `FilterIdentitySourceAccessor::computeOwnSuffix` (§5), the new wrapper for user filters. Path-independent (one filter per addPath).
- `;l` (LFS): *no wrapper class exists today*. The future `LfsSmudgingSourceAccessor` (item l, §6) would allocate it via `computeOwnSuffix`. Until then, `;l` is on the leaf because the leaf is the only accessor that knows whether LFS smudging is on (`state->lfsFetch` is set in `GitSourceAccessor::State`).
- `;s` (submodules): *no wrapper class exists today*. Same story as `;l` — leaf carries it until a future submodule wrapper exists.

When a future wrapper for `;l` or `;s` lands, the flag migrates from leaf to wrapper. **This is a structural refactor, not an algebraic change** — the schema, the soundness invariant, and the wrapper-stack composition rule are unchanged. The leaf-allocation is a *transitional* placement, valid until a wrapper takes over the concern.

The orthogonal question of *path-dependence* — whether the suffix value varies with the queried path — is independent: `;l`, `;s`, `;f=H` are path-independent (compute once, use everywhere); `;e=H` is path-dependent (cached per `(rev, path)` on the wrapper). Path-dependence affects memoisation strategy, not which accessor allocates the flag.

`GitSourceAccessor::fingerprint` is set inside `getAccessorFromCommit` at construction time to `git:<rev>` plus `;l` (when `lfs=true`) and `;s` (when `submodules=true`). ~7 LOC at construction: `accessor->fingerprint = "git:" + rev.gitRev() + (lfsFetch ? ";l" : "") + (submodules ? ";s" : "")`. Item j's `getFingerprint` override returns `{path, fingerprint}` for `subpath.isRoot()` and content-keyed `{root, "tree:T;l;s"}` / `{root, "blob:B;l"}` / etc. for non-root subpaths (using the same flag suffixes from the leaf's stored `flagSuffix`). **The wrapper then merges in `;e=H`**: `git:R;l;s` + `;e=H` = `git:R;e=H;l;s` (alphabetical: `;e` before `;l` before `;s`), `tree:T;e=H;l;s` for non-root. **No same-tag collision** because the leaf's `;l` and `;s` flags don't overlap with the wrapper's `;e=H`. **For workdir inputs**, the wrapper-fingerprint pattern from `fetchers.cc:367` continues to work: the inner `PosixSourceAccessor`'s `fingerprint` field is unset, so the wrapper's "fall back to field" branch fires, returning `{path, "git:R;d=H"}` — master's existing form (the input-scheme's `getFingerprint` includes the `;d` for dirty workdirs and is set on the wrapper). **For substituted accessors** (the `requireStoreObjectAccessor` path at lines 318-345), the accessor isn't wrapped and the base class returns `{path, fingerprint}` directly — that path's `accessor->fingerprint = getFingerprint(store)` assignment at line 329 sets the *input-scheme-level* fingerprint with all flags pre-appended, which is correct for the un-wrapped case. **All three cases — commit-rooted, workdir, substituted — produce sound cache keys** under the unified `getFingerprint` virtual.

**Migration note for the cache.** Item j keeps `Input::getFingerprint`'s input-level form unchanged (`git:R;e` flag-only, as master). Only the *accessor-level* form changes: master returns `(path, "git:R;e")` for any subpath; item j returns `(/, "tree:T;e=H")` or `(/, "blob:B;e=H")` for non-root subpaths through commit-rooted accessors. Old `sourcePathToHash` rows keyed on `("git:R;e", subpath)` continue to be hit when something queries them; new rows accumulate at `("tree:T;e=H", /)` for content-keyed access. **There is no mass orphaning** — the old and new rows occupy disjoint key spaces and master's existing semantics are preserved for any caller that still queries by `("git:R+flags", subpath)`. The eval cache files (named after `LockedFlake::getFingerprint`, which calls `Input::getFingerprint`) are unaffected — `git:R;e` continues to name the same eval cache file across the upgrade. The two form choices (input-level flag-only, accessor-level value-form) are deliberate: the input level cannot afford to compute the gitattributes hash before path is known (would force a full input walk), and the accessor level has the path in hand and computes per-`(rev, path)`.

`GitExportIgnoreSourceAccessor` overrides `computeOwnSuffix` to return `;e=<gitattributes-along-path-hash>`; the new `FilterIdentitySourceAccessor` (§5) overrides it to return `;f=<hash>` (or `nullopt` when the filter is unhashable, signalling bypass); an LFS-smudging wrapper would return `;lfs=<hash>`.

`mergeSuffix(inner, "<;tag=value>")` or `mergeSuffix(inner, "<;tag>")` parses `inner` into root + sorted suffixes, inserts the new suffix at the alphabetically-correct position, and re-serialises. ~15 LOC of shared logic. **Parser specifics**: a suffix is `;<tag>` or `;<tag>=<value>`; the parser splits on `;` first, then on `=` (first occurrence) within each suffix to separate tag from value. The parser maintains an ordered list of *known tags* with a flag per tag indicating whether `=value` is required: `d=required`, `e=required`, `f=required`, `l=forbidden`, `lfs=required`, `s=forbidden`. Unknown tags are programmer errors and throw — adding a new tag requires updating the schema (§4 "Suffix schema") and the ordered list together. Lexicographic comparison handles `l` vs `lfs` correctly (`"l" < "lfs"` byte-wise). This rejects accidental schema drift at the implementation site rather than letting two clients silently produce different orderings for the same tag set.

**Wrapper-stack-order independence is the load-bearing algebraic property.** Two wrappers `A` and `B` that append *distinct* tags commute: `A(B(x)) = B(A(x))`, because both `mergeSuffix` calls insert into the same alphabetically-sorted suffix list at fixed positions (`;d` < `;e` < `;f` < `;l` < `;lfs` < `;s`). Without the alphabetical canonicalisation, the two stacks would produce different strings for the same content, breaking the soundness invariant. **If two wrappers in a stack both try to append the *same* tag** (e.g. two filter wrappers both appending `;f=<hash>`), that's a programmer error — they're competing for the same identity dimension and one is mis-using the schema. `mergeSuffix` throws on same-tag insertion; in practice `addPath` constructs at most one `FilterIdentitySourceAccessor` per call, so the case doesn't arise. The assertion is defensive against future wrapper additions that accidentally reuse a tag.

The result: items j, k, §5, and any future identity-affecting concern all flow through one virtual, share one cache namespace, and compose without per-concern code in `fetchToStore`. **Master's existing short-circuit is the load-bearing change — without removing it, item j's branch never fires through the wrapper chain.**

`MountedSourceAccessor::getFingerprint` has the same short-circuit (`mounted-source-accessor.cc:100-105`). It needs the same "forward without short-circuit" treatment, with `computeOwnSuffix` defaulting to empty (the mounted accessor adds nothing of its own; it just routes). Concretely: `mounted->getFingerprint(path)` resolves the mount, calls `accessor->getFingerprint(subpath)`, and returns the inner result unmodified. **Same structural change, different default.**

Together: removing both short-circuits is ~30 LOC of plumbing, prerequisite to item j firing under the dominant accessor stack. **Add this to the implementation order in §9 as part of item j.**

### Subtree-aware fingerprints (item j): the `getFingerprint` extension

The `addPath` primop in [`src/libexpr/primops.cc`](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/primops.cc), called by `builtins.path` and `builtins.filterSource`, calls `state.store->toStorePath(path.path.abs())` inside its in-store gate. That call returns `std::pair<StorePath, CanonPath>` (per `StoreDirConfig::toStorePath`); the `CanonPath` second element is the subpath under the parent store-path. Today only the `StorePath` is consumed (for `queryPathInfo(storePath)->references`); the subpath is discarded.

The cause of cross-rev subtree dedup failure (the survey's §6.1 reproduction) is in `GitInputScheme::getFingerprint`, which returns `rev.gitRev() + flags`: two distinct revs of the same repo produce distinct fingerprint strings even for an identical subtree. The `sourcePathToHash` key is `(fingerprint, method, subpath)`; identical `/sub` content under different parent revs keys on different rows.

The fix is to extend `GitSourceAccessor::getFingerprint(subpath)` so that for tree-rooted *and* file-rooted subpaths, the returned fingerprint is content-determined by the subtree-SHA or blob-SHA respectively, not by the parent rev:

```cpp
// In GitSourceAccessor, store both forms at construction:
//   fingerprint: "git:R;l;s" — full input-level form, used for root reads
//   flagSuffix:  ";l;s"      — just flags, prepended to tree/blob OIDs

std::pair<CanonPath, std::optional<std::string>>
getFingerprint(const CanonPath & subpath) override
{
    if (subpath.isRoot())
        return {subpath, fingerprint};   // input-level identity at root
    auto state = state_.lock();
    auto entry = lookup(*state, subpath);
    if (!entry)
        return {subpath, fingerprint};   // missing: fall back to whole-accessor
    auto type = git_tree_entry_type(entry);
    auto oid = toHash(*git_tree_entry_id(entry)).gitRev();
    if (type == GIT_OBJECT_TREE)
        return {CanonPath::root, "tree:" + oid + flagSuffix};
    if (type == GIT_OBJECT_BLOB)
        return {CanonPath::root, "blob:" + oid + flagSuffix};
    return {subpath, fingerprint};       // submodule (commit) or other: parent-rev-keyed
}
```

This re-anchors the cache key at root with an OID-derived fingerprint string for tree- *and* blob-rooted subpaths. Two evaluations against revs `r1` and `r2` with identical `/sub` subtree-SHA `T` both produce the cache key `("sourcePathToHash", {fingerprint: "tree:T", method: "nar", path: "/"})` and hit the same row. **Two evaluations reading the same byte-identical file from different revs produce `blob:B` and hit the same row.** The blob branch is what makes item k's parse-cache (§4.5) genuinely cross-process-cross-rev — without it, file-rooted fingerprints fall back to parent-rev-keyed and item k's cache misses across revs of the same monorepo with byte-identical content.

**Re-anchoring the path to root is sound.** The type signature `pair<CanonPath, optional<string>>` already permits the returned `CanonPath` to differ from the input — `MountedSourceAccessor::getFingerprint` does exactly this when stripping the mount prefix. Reusing the existing `sourcePathToHash` domain (no new cache table) keeps the cache namespace flat. Old rows keyed on `git:<rev>+flags` for file-rooted subpaths continue to coexist; they accumulate and age out naturally.

**`exportIgnore=true` requires both branches to receive a `;e=<hash>` suffix from the wrapper.** When `options.exportIgnore` is set, `GitExportIgnoreSourceAccessor` wraps the raw `GitSourceAccessor`. Per §4's canonical pattern, the wrapper's `computeOwnSuffix` returns `;e=<hash-of-gitattributes-along-path>`, where the gitattributes hash covers every `.gitattributes` file from the repo root down to the queried path. The composition rule then produces `tree:T;e=H` or `blob:B;e=H` as the cache key. **The blob branch needs the suffix too**, not just the tree branch: `.gitattributes` can mark an individual file as `export-ignore`, in which case `addPath` against the file produces an empty NAR (the file is excluded), not a NAR containing its bytes. Two evaluations against the same `blob:<sha>` under different `.gitattributes` ancestors — one where the file is included, one where it's excluded — produce different `addPath` results, so the suffix is mandatory for soundness on the blob branch. Tecnix's existing `getGitAttributesAlongPath` virtual on `GitRepo` (commit-rooted, walks the tree picking up `.gitattributes` blobs at each ancestor) is the working primitive to port to master; the gitattributes hash is computed once per `(rev, path)` pair and memoised on the wrapper's lookup cache. Submodule entries (`GIT_OBJECT_COMMIT` inside a tree) fall through to the parent-rev-keyed branch because submodule content is determined by the *outer* repo's `.gitmodules` config plus the inner repo's accessor — neither captured by the entry's commit-OID alone. **The `lfs=true` case is content-affecting** (an LFS-pointer blob smudges to different bytes than the raw pointer file): item j's override applies a `;l` suffix to both branches when `state->lfsFetch` is set, so the cache key correctly distinguishes smudged from unsmudged content. Master's `GitInputScheme::getFingerprint` already adds `;l` for the `git:<rev>` form; extending it to the tree/blob branches keeps the schema consistent.

**Cross-pipeline reuse** falls out for free by also writing the resulting `(treeSha, narHash)` pair into the existing `treeHashToNarHash` cache domain — currently used only by `tarball.cc::downloadTarball` for the tarball-cache pipeline. After this extension, a tarball downloaded today and a git input fetched tomorrow that unpacks to the same tree-SHA share a single cache row. This is exactly the survey's §8.0 item **b** ("tree-SHA → NAR-hash projection generalised across all opened git ODBs") realised at the natural seam.

**Cache disk-usage growth.** Master's `sourcePathToHash` rows are rev-keyed (one per unique rev × method × subpath). Item j shifts to content-keyed (one per unique tree-SHA or blob-SHA × method). For a long-lived cache against a monorepo with churn (nixpkgs, tecnix's world), the row count grows roughly with the count of unique tree-SHAs and blob-SHAs ever encountered — order-of-magnitude *more* rows than master, not fewer. ~100 bytes per row × millions of rows over years → multi-hundred-MB cache. Master users already periodically clean `~/.cache/nix/`; item j increases the growth rate. **The trade-off**: more disk usage *for* much higher hit-rate (item j's headline win). A future `nix fetcher-cache gc` operation that evicts least-recently-used rows beyond a configurable size budget would help; currently cache is uncapped. Out of scope for this proposal; flagged for the same RFC as cross-machine cache merge (§10).

The total surgery is approximately ~20 LOC for the `getFingerprint` override (tree + blob branches), ~25 LOC for the `treeHashToNarHash` write/read hook in `fetchToStore2`, and ~30 LOC for the `.gitattributes`-along-path memo on `GitExportIgnoreSourceAccessor`. Header impact is one virtual override on `GitSourceAccessor`; no new cache domain, no new in-process state, no new public API.

**What this solves transparently.**

- **Cross-rev subtree dedup** for code paths that go through `builtins.path` / `builtins.filterSource` against an in-store input subpath. Two different revs of the same monorepo whose `/sub` tree-SHA is identical share a single store path. This is the survey's §6.1 "store-path equivalence under content equivalence" applied to subtrees, with no public API change and no narHash gate violation (because `mountInput` for the parent input still computed the parent's full narHash; the cache hit is for the *child* `addPath` only).
- **CI matrices** evaluating N branches that share a subtree pay one NAR walk total (the first eval to populate the row), not N.
- **Cross-fetcher reuse**: a `tarball://` input and a `git://` input that produce the same tree-SHA share `treeHashToNarHash` rows.

**What this does not solve.**

- **Workdir inputs don't benefit from the content-keyed branch.** Item j's `tree:<sha>` / `blob:<sha>` branch fires only for `GitSourceAccessor` (commit-rooted, OIDs in hand). Workdir inputs go through `getAccessorFromWorkdir` → `AllowListSourceAccessor` over `makeFSSourceAccessor` (`PosixSourceAccessor`); `PosixSourceAccessor::getFingerprint` returns the whole-input fingerprint with master's existing `;d=<hash>` suffix. Computing per-file `blob:<hash>` for FS-backed paths would mean reading each file twice (once to fingerprint, once to add), which is the wrong cost trade. **Workdir-rooted `${input}/sub` reads remain parent-fingerprint-keyed under item j**; the `;d=<hash>` suffix correctly invalidates the cache when *anything* in the workdir changes, but it doesn't share rows across workdir-revisions where a particular subtree happens to be unchanged. Acceptable: dirty workdir is the developer's local-edit case, not the cache-cold-cross-process case where item j's wins matter most.
- **String-interpolation references.** `nativeBuildInputs = [ "${bigrepo}/areas/tools/foo" ]` does *not* invoke `addPath`. The string-with-context value flows through `derivation`'s `inputSrcs` machinery and `EvalState::ensureLazyPathsCopied`, which iterates `Opaque{path}` context elements and copies the **whole** parent input's storePath. The fingerprint extension only fires when the user wraps the subpath in `builtins.path { path = bigrepo + "/sub"; }` or equivalent.
- **First-fetch wire savings.** The parent input is still fully fetched on first contact to compute `mountInput`'s narHash. The fingerprint extension hits at the `addPath` step, *after* the parent has been mounted. First-eval wire savings depend on lazy `narHash` (option 1 in "What item h does deliver" below).
- **Subtree-as-first-class-Input.** The lock still records the parent input. `flake.lock` does not gain a "subtree was the unit" entry. If the user wants to pin a subtree independently of its parent, that still requires the deferred RFC — see §4.

**Caveats and side fixes.**

- `fetch-to-store.cc`'s existing filter-bypass ternary disappears under §5's `FilterIdentitySourceAccessor` wrapper-accessor pattern. The wrapper is what computes filter identity and appends `;f=<hash>` to the underlying fingerprint; `fetchToStore` no longer takes a `PathFilter *` parameter and treats wrapped/unwrapped accessors uniformly. Item j's blob/tree branches and §5's wrapper compose cleanly because both flow through `getFingerprint` per §4's canonical pattern.
- **`addPath`'s refs-bypass is a load-bearing fix, not optional.** The existing `// FIXME: support refs in fetchToStore()?` comment in `primops.cc` says: when `refs` (the parent storePath's transitive references) is non-empty, `addPath` calls `addToStore` directly instead of `fetchToStore`, bypassing the cache regardless of fingerprint. **This bypass fires whenever `${input}/sub` references a derivation output's storePath** (e.g. `builtins.path { path = pkgs.stdenv.outPath + "/lib"; }`). Without fixing this, item j's wins evaporate for any pattern where the parent is itself a derivation output — which includes `mkDerivation { src = pkgs.fetchurl {...}; }`-style intermediate inputs. **Stage this fix alongside item j**, not as future work: extend `fetchToStore` to take a `StorePathSet refs` parameter (defaulting to `{}`), thread it into both `makeFixedOutputPathFromCA(name, fromParts(method, hash, refs))` (for cache-hit storePath reconstruction) and the inner `addToStore` call, and route the refs-bearing branch through `fetchToStore` too. **The cache key remains `(domain, fingerprint, method, path)` — refs are NOT a key dimension** because for a given `(accessor, path)` the refs are content-derived from the path (`queryPathInfo(parent-storePath)->references`); two calls with the same `(accessor, path)` always produce the same refs. **No derivation-hash transition** for existing callers: the refs-empty branch keeps producing the same storePaths (`fromParts(..., {})`) it produces today; the refs-bearing branch produces *different* storePaths than the no-refs branch from the same content (because refs participate in the CA hash via `fromParts`), but those storePaths are *new* — they didn't exist before because the bypass meant nothing was cached. ~10 LOC. Without this fix, item j's user-visible-outcome row in §7 ("CI matrix evaluating N branches sharing a subtree → 1 walk total") is conditional on the parent input being non-derivation — a real but partial win.
- **Restricted / pure-eval**: `EvalState::allowPath(StorePath)` casts `rootFS` to `AllowListSourceAccessor` and calls `allowPrefix`; the parent storePath was registered when `mountInput` ran, so the subtree's narrowed store path is already covered by the parent's allowed prefix. No additional `allowPath` call is needed at the `addPath` site.

### What item **h** *does* deliver

Lazy clone (item **h**) closes the wire-cost gap from a different direction: instead of trying to know a subtree at first contact, fetch *all* commits and trees (a small fraction of total repo size on typical monorepos — exact ratio depends on packfile compression and blob-to-tree ratio) but defer blobs. When the evaluator reads `/sub`, only those blobs come over the wire. From the user's perspective the cost looks like "subtree-only fetching" even though we technically have the full tree skeleton.

The shape this leaves:

- **First contact**: full clone of commits+trees, no blobs.
- **First eval reading `/sub`**: blobs reachable from `/sub` are fetched on-demand via `PromisorBackend`.
- **narHash**: still computed over the *full* tree at the locked rev. The lock isn't subtree-aware. We pay one full NAR walk on first eval (since `mountInput` runs dryRun); that walk reads every blob, triggering `PromisorBackend` to fetch them all the first time.

Wait — that last bullet means item **h** doesn't actually reduce wire bytes on the first eval, because the dryRun forces a full walk! This is a real and important wrinkle. Reading [`paths.cc`'s `mountInput`](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/paths.cc) and the comment in its body carefully, the existing code already anticipates *two* tiers of laziness:

> *"This could be relaxed in the future by making outPath and narHash lazier. Good code that doesn't do `toString ./.` or otherwise inspects the outPath string and only uses it for doing relative imports does not even require computing the store path. That is a big invasive change though and would require having a special "LazyStorePathString" thunk. narHash also doesn't need to be computed eagerly in case it's not actually specified (like during local development with a dirty tree) — in that case narHash could also become a lazy app/thunk that shares the state with the storePath delayed computation."* — `mountInput` comment in `paths.cc`.

So the menu of changes, from cheap to expensive:

1. **Lazy `narHash` only.** Replace the `narHash` field's eager string with a thunk, computed at lock-file write time and at the hash-mismatch check, both off the hot path. `outPath` remains eager; `mountInput` still computes the storePath. Wire savings: the first eval skips the full-NAR walk if nothing forces narHash, so blobs the evaluator never reads stay unfetched. This is the relaxation the existing comment calls "lazy app/thunk." It does not require touching `Value` or string interpolation. **Direct precedent on master**: Robert Hensing's `LazyAttr` scaffolding (commits `ee78fe13e` adding lazy fetcher attrs and `a08c14bf8` emitting thunks for them; in `libfetchers/include/nix/fetchers/attrs.hh`: `struct LazyAttrComputation { fun<ResolvedAttr()> compute; }` plus `using LazyAttr = ref<LazyAttrComputation>`, with the `compute` callback wrapped via `memo<>` (`std::once_flag` for thread safety)). The analogous single-attribute conversion of `revCount` (commit `e9336fa3a`) is a few dozen lines on top of that scaffolding. `narHash` has more call sites than `revCount` — `fetchers.cc::Input::getNarHash`, `Input::to_string`, `Input::toAttrs`, the JSON serialiser, the lock-write path in `libflake/flake.cc` — so the conversion is correspondingly larger but the same shape. **Note**: this `LazyAttr` lineage is in master and tecnix; DetSys's lazy-trees branch (`nix-src/main`) does *not* have `LazyAttr` and uses a different mechanism (`StorePath::random` in `mountInput` plus a `lazy-locks` setting that omits narHash from lock files). The proposal targets master, so the master-LazyAttr precedent is the relevant one.
2. **Lazy `outPath` via `LazyStorePathString`.** The "big invasive change." Requires a new value/thunk shape that string interpolation, derivation hashing, and JSON printing all understand to defer until forced. Once landed, `${input}/sub` doesn't even compute the parent's store path until something concatenates it for real, and combined with the §4 subtree-promotion seam at `addPath`, *first-eval wire savings actually materialise*: the parent input's narHash is never forced because `addPath` hits the subtree cache and never asks for it.
3. **Trust the lock.** If `originalInput.getNarHash()` is set, skip the dryRun entirely on the eval-time fast path; turn the mismatch check from an eager throw into a deferred warning at next lock-write. **More invasive than it sounds:** the dryRun in `mountInput` produces *both* `storePath` and `narHash` jointly via one `fetchToStore2(..., FetchMode::DryRun, ...)` call, so skipping it requires reconstructing the storePath from the lock or deferring it — which is essentially option 2's `LazyStorePathString` work. Same RFC-shaped concern as option 1 but for a different reason (semantic visibility of the check, not value-kind rippling).

Option 1 is the right first step. Option 2 unlocks the full first-eval savings of §4 + item **h** combined. Option 3 is a backstop if upstream rejects either; it's strictly less invasive but trades safety for performance.

**Why option 3's risk is structural, not just operational.** Per cache key, the row state forms a chain `⊥ ≤ running ≤ done x`, with `done x ⊑ done y` iff `x = y`. Cache state across all keys is the product (a Scott domain). Options 1 and 2 are monotone refinements: they move the row from `⊥` to `done x` exactly when something forces the value. Option 3 is *not* monotone — it asserts `done x` from a check (the lock's `narHash` field) that lives outside this dcpo. If the user-supplied lock disagrees with the eventually-computed `narHash`, option 3 has no monotone path to recover; the row was committed `done` against a stale assertion. This is why option 3 needs a "deferred warning at next lock-write" mitigation: the warning is the only signal that the dcpo's monotonicity has been violated. Options 1 and 2 *cannot* hit this failure mode because they only ever refine `⊥ → done x` from the actual computation.

**Cross-tree caveat on the dcpo claim**: master's `mountInput` produces content-determined `(storePath, narHash)` (the storePath is computed via `makeFixedOutputPathFromCA`), so the dcpo holds globally — same input across processes maps to the same `done x`. **DetSys's lazy-trees branch** does *not* satisfy this: when `settings.lazyTrees` is true, `mountInput` uses `StorePath::random(input.getName())`, so the same input maps to *different* store paths across `EvalState` instances. Tecnix's `tectonixZoneCache_` has the same `StorePath::random` semantics. In dcpo terms: the per-row chain `⊥ ≤ running ≤ done x` holds *within one `EvalState` lifetime*, but the global "same input ⇒ same `done x`" property is broken across processes. **The proposal targets master**, where this issue does not arise. Implementations that adopt DetSys/tecnix-style random-storePath laziness would need to qualify the dcpo claim to "monotone within a single eval session" and accept that cross-process cache sharing requires an additional canonicalisation step.

**Eviction in master** is real: `EvalState::resetFileCache()` clears `inputCache`, `fileEvalCache`, etc. on REPL `:reload`, and the SQLite cache's `lookupExpired` returns expired entries with a flag (the caller can choose to re-run or use the expired value). Both are `done x → ⊥` moves and break monotonicity in principle. In practice they happen on user signal (`:reload`) or for time-bound domains (`tarball`/`file`/`hgRefToRev`), neither of which is in the content-keyed Scott domain the dcpo claim describes. The dcpo claim should be read as "for content-keyed rows under continuous evaluator lifetime."

The `addPath` subtree-promotion seam from the previous subsection composes specifically with option 1 *or* option 2: with lazy narHash, the subtree cache hit at `addPath` means nothing forces the parent's narHash, so the dryRun's tree walk never runs; with `LazyStorePathString`, even the parent's store path is never computed.

### What about tecnix's eleven builtins?

This subsection has two distinct scopes that earlier drafts conflated:

- **Tecnix-specific migration scope** — what does tecnix have to change in its own Nix expressions to retire its eleven builtins? Tecnix controls one library wrapper (`worldZone`); 8 of the 11 builtins retire transparently under items g+h+j+k *and* a model shift where `worldRoot` is declared as a flake input rather than reached via the `--tectonix-checkout-path` setting (because item j's `tree:<sha>`/`blob:<sha>` branch fires only for `GitSourceAccessor`, not `PosixSourceAccessor`). The remaining **3 dirty-zone builtins** (`DirtyZones`, `ZoneIsDirty`, `ZoneRoot`) cannot retire under this proposal — master's `getAccessorFromWorkdir` dispatches at the *Input* level (a workdir input has no rev pinned), so it cannot replicate `DirtyOverlaySourceAccessor`'s per-file overlay of dirty bytes onto a clean-at-rev tree. Retiring those three requires either accepting a UX shift (users *choose* clean-at-rev or dirty-no-rev) or a follow-on RFC delivering `OverlaySourceAccessor` (survey §8.3 #14).
- **Nixpkgs-wide migration scope** — what would it take for *every existing nixpkgs derivation* using `${input}/sub` interpolation to reach the same fast path? That requires either Layer-1 subtree-as-Input (lock-file identity changes) or rewriting the bare-string-interpolation pattern across nixpkgs (derivation-hash transition). Both are out of frame here; both are real release-coordination events. **The proposal does not commit to nixpkgs-wide migration.**

The framing in §0 ("makes those workarounds unnecessary, transparently") was overstated for the dirty-zone subset. The acceleration tier (items **g**, **h**, **j**, **k**, plus the §5 filter-identity work) is sufficient for tecnix to retire 8 of its 11 builtins via Nix-side library updates plus the worldRoot-as-flake-input model shift; the remaining 3 dirty-zone builtins persist until either tecnix accepts the workdir-vs-rev split or a separate RFC adds the overlay combinator. The lock-file-identity tier (out of scope) is what every nixpkgs derivation would need to share the same fast path on its existing `${input}/sub` interpolation idiom.

#### Per-builtin migration mapping (tecnix-specific)

| # | Builtin | Replacement | Mechanism |
|---|---|---|---|
| 1 | `__unsafeTectonixInternalManifest` | `builtins.fromJSON (builtins.readFile (worldRoot + "/.meta/manifest.json"))` | Item **k**: parse-cache fires on the content-fingerprintable `readFile`; per-process re-parse cost collapses to one cache lookup |
| 2 | `__unsafeTectonixInternalManifestInverted` | Pure-Nix inversion of #1, e.g. `lib.mapAttrs' (path: { id, ... }: { name = id; value = path; }) manifest` | Item **k** (cache hit on the source) |
| 3 | `__unsafeTectonixInternalTreeSha` | Internal cache key under item **j**; not exposed at the Nix-language level | The C++ `getFingerprint` override returns `tree:<sha>` for tree-rooted subpaths automatically |
| 4 | `__unsafeTectonixInternalTree` | `builtins.path { path = worldRoot + treeRelativePath; }` (or in tecnix's `worldZone` library, the manifest-resolved path) | Item **j**: subtree-SHA-keyed CA storePath at `addPath` time |
| 5 | `__unsafeTectonixInternalZoneSrc` | `builtins.path { path = worldRoot + zonePath; }` *only when `worldRoot` is a flake input pinned to a rev* (then `getAccessorFromCommit` returns a `GitSourceAccessor` and item j's `tree:<sha>` branch fires); pure FS-path interpolation routes through `PosixSourceAccessor` and gets *no* cross-rev subtree dedup | Items **g**+**h**+**j**: cache hit on subtree-SHA, blobs lazy via `PromisorBackend` — only along the flake-input path |
| 6 | `__unsafeTectonixInternalZonePath` | Same as #5; `builtins.path` already returns a path-typed value | Same |
| 7 | `__unsafeTectonixInternalSparseCheckoutRoots` | Pure-Nix derivation from the manifest (the set of declared zones the user is working on); no longer needs a separate sparse-checkout state | Item **h**'s lazy clone collapses "in sparse checkout" vs "not" — everything is on-demand |
| 8 | `__unsafeTectonixInternalDirtyZones` | **Partially retire.** Master's `getAccessorFromWorkdir` exposes dirty-workdir bytes through `${workdirInput}/sub`, but only when the input is a *workdir input* (no `rev` pinned). Tecnix's current model — pin `--tectonix-git-sha` *and* mix in dirty bytes from the local checkout — has no master analogue. **Without an overlay-accessor combinator, retiring this builtin forces a UX shift**: the user either pins the rev (clean-at-rev evaluation; in-place edits invisible until committed) or uses the workdir input (dirty bytes visible; rev pinning lost). The proposal does *not* deliver the overlay combinator (§10 explicitly punts it). | Conditional on UX shift; otherwise builtin remains until a follow-on RFC adds `OverlaySourceAccessor` |
| 9 | `__unsafeTectonixInternalZoneIsDirty` | **Same as #8** — partial retire conditional on the UX shift | Same |
| 10 | `__unsafeTectonixInternalZoneRoot` | **Conditional retire.** The application's need to obtain "the filesystem path to a zone for in-place editing" is satisfied by `worldRoot + zonePath` directly *when the user is in a source-available checkout configured as a workdir input*. With a rev-pinned flake input, no FS path is exposed (the input is materialised in the store). | Conditional on the same model shift as #8/#9 |
| 11 | `__unsafeTectonixInternalGitSha` | A plain setting (`tectonix-git-sha`) read via existing setting-introspection — out of frame for this proposal (CLI configuration, not infrastructure) | n/a |

Two notes on the table:

- **Item k (parse-cache) is what makes #1 and #2 transparent.** It is the new addition introduced for this purpose; see §4.5.
- **Master's workdir accessor partially overlaps tecnix's `DirtyOverlaySourceAccessor` but does not replace it.** Master's `GitRepoImpl::getAccessor(WorkdirInfo & wd, ...)` at `git-utils.cc` constructs an `AllowListSourceAccessor` over `makeFSSourceAccessor(repoPath)`, gated by `wd.files` (the set of git-tracked, non-deleted files reported by `git_status_foreach_ext`). `GitInputScheme::getFingerprint` appends `;d=<dirty-hash>` to the rev when the input is a workdir input. **Dispatch into this accessor is conditional on `!input.getRef() && !input.getRev() && repoInfo.getPath()`** (`git.cc:1090–1092`) — a workdir input has no rev pinned. Tecnix's current model pins `--tectonix-git-sha` and overlays dirty bytes selectively per file via `DirtyOverlaySourceAccessor`; **master has no analogous accessor that can serve clean-at-rev for non-dirty files and workdir bytes for dirty files in the same input.** The semantics are mutually exclusive in master: either you pin a rev (you get the clean tree at that rev — workdir edits invisible until committed) or you use a workdir input (you get dirty bytes — rev pinning lost). **For tecnix's "is zone X dirty?" question, the gap is real**: a Nix-language predicate like `worldZone.isDirty zonePath` cannot be expressed transparently because the dispatch decision happens at the Input level, not per-zone. Modified-file and deletion semantics individually align (master's FS accessor sees disk state directly; deleted files appear missing through both); untracked-but-not-`git add`-ed files are visible in neither. **What this means for migration**: retiring `DirtyZones`, `ZoneIsDirty`, and `ZoneRoot` requires either (a) a UX shift where users *choose* between rev-pinned-clean and workdir-dirty-no-rev mode at the input declaration, or (b) the proposal acquires an `OverlaySourceAccessor` combinator (survey §8.3 #14) that can selectively overlay disk bytes on a clean-at-rev tree. The proposal as written does neither — §10 explicitly punts the overlay combinator. **The "all 11 retire transparently" framing was overstated**; the honest claim is "5 of 11 retire under items g+h+j+k; 3 retire conditional on a UX shift; 1 (manifest-inverted) is pure-Nix derivation; 2 (`Manifest`/`ManifestInverted`) retire under item k; 1 (`GitSha`) is out of frame." The dirty-zone builtins continue to exist until either a follow-on RFC adds the overlay combinator or tecnix users accept the workdir-vs-rev split.

#### Nixpkgs-wide migration scope (out of frame)

For *every existing nixpkgs derivation* that uses `${input}/sub` interpolation in `mkDerivation { src = ...; }` to reach the same fast path, the same `addPath` seam must fire. That means rewriting `${input}/sub` to `builtins.path { path = input + "/sub"; }`, which is a derivation-hash transition for thousands of derivations — a release-coordination event, not a localised optimisation. The bare-string-interpolation idiom flows through `derivation`'s `inputSrcs` and `EvalState::ensureLazyPathsCopied` against the parent's whole storePath: by the time the context iteration in `derivationStrictInternal` sees the `Opaque{parent}` element, the `${zoneSrc}/sub` suffix exists only as bytes inside `drv.env["src"]`; the input set `inputSrcs = {parent}` is what the build sandbox bind-mounts. Narrowing `inputSrcs` to the child without rewriting the env-string gives the build a path the sandbox does not bind-mount; rewriting (via `BasicDerivation::applyRewrites`) recomputes every derivation hash. Storing the narrowing annotation out-of-band is also closed: `Derivation::unparse` is a fixed ATerm-shaped serialisation `(outputs, inputDrvs, inputSrcs, platform, builder, args, env, ...)` with no out-of-band slot.

Truly *eliminating the bare-string-interpolation cost across nixpkgs* therefore requires Layer-1 subtree-as-Input — the *subtree* as the unit of lock identity, not just a pointer. The existing `dir` attribute (FlakeRef layer) provides a user-visible subpath pointer with whole-repo narHash; making the subtree itself the lock unit requires either re-defining `dir`'s semantics at the fetcher layer (back-incompat) or adding a `version: 8` lockfile + `subPath`/`narHashScope` attribute on locked entries (back-compat, opt-in). Tecnix's `tectonixZoneCache_` is an existence proof for the latter at the application layer. The migration design is its own RFC. **Not in scope here, and not a prerequisite for tecnix retiring its builtins.**

---

## 4.5 Content-keyed parse-cache (item k)

(This section is a concrete instance of the algebra in §0: it is `Projection<(content-fingerprint, format), serialised-Value>`, with the content-fingerprint inherited from `SourcePath::getFingerprint` (item j's `blob:<sha>` form for the headline workload) and the value serialised in a custom row-per-document binary blob format (described below). The proposal lives in a *new* per-machine SQLite database (`parse-cache-v1.sqlite` next to `fetcher-cache-v4.sqlite`); it does **not** ride the eval cache, whose schema is per-fingerprint, row-per-attribute, and structurally wrong for whole-document caching. No new public API; no Nix-language extension.)

The survey at §7.8 names a missing abstraction: there is no parse-cache for pure expressions over input contents. `builtins.fromJSON` re-parses on every cold eval; `builtins.fromTOML` likewise; `builtins.readFile` of a content-addressed input followed by either yields a string whose content-fingerprint is known C++-side but never used as a cache key. Tecnix's `__unsafeTectonixInternalManifest` exists exactly to side-step this: it parses `.meta/manifest.json` once per `EvalState` and memoises the `nlohmann::json` in tecnix's `EvalState` (`tectonixManifestJson` under `std::once_flag`).

The fix is a `Projection<>`-shaped cache keyed on `(content-fingerprint, format-tag)` whose value is a serialised parsed `Value` tree, persisted in a per-machine SQLite database (`parse-cache-v1.sqlite`). The cache fires at the call sites that already know they have a content-fingerprintable string:

- `prim_fromJSON` and `prim_fromTOML` already receive a string. Today the string is opaque; the proposal records a content-fingerprint *out-of-band* (in a per-`EvalState` side-table keyed on the string's `StringData *` allocation identity) when the string originates from `builtins.readFile` against a content-addressed source.
- When `prim_fromJSON` runs against such a string, it looks up the side-table, computes the cache key, and consults `parse-cache-v1.sqlite`. Hit returns the cached parsed value (deserialise into the output `Value &`); miss parses, upserts, returns.
- Item k uses a custom call shape rather than the generic `Projection<>::lookup` because Nix `Value` is not pass-by-value-friendly: `lookup-or-deserialise-into-output-parameter` plus `compute-and-serialise-from-output-parameter` avoids intermediate copies and GC bookkeeping. Same algebraic discipline as the typeclass; different signature for `Value`'s ownership idiom. Master's `tarball` and `hgRefToRev` domains already use bespoke `lookupExpired` / `lookupWithTTL` shapes for the same kind of reason — the typeclass is *one* expression of the cache discipline, not the only one.

### Why a side-table, not a `NixStringContext` variant

An earlier draft of this section extended `NixStringContext` with a fourth `Fingerprint{path, fp-string}` element carrying the originating accessor's `getFingerprint` result. Cross-tree audit ruled that approach out:

- **`forceStringNoCtx` has 11 callers** in master (`prim_fromJSON`, `prim_fromTOML`, `prim_throw`, several path-coercion sites). All of them throw on *any* context element. Adding a `Fingerprint` variant would either (a) break every caller until each gains a new "is this a real reference or just metadata?" check, or (b) require introducing `forceStringPureContent` and migrating each call site individually — the original draft's plan, ~20 LOC of mechanical surgery.
- **`unsafeDiscardStringContext` and string operations would silently drop fingerprints.** Concatenation drops context conservatively; `replaceStrings` drops it; the JSON serialiser writes context as a structured field. Each of these would need a new "Fingerprint passes through as metadata, not a reference" arm — at minimum a comment, often code.
- **Worst, the ~58 fall-through `std::get_if<Opaque>` call sites would silently lose fingerprints** in a way that's invisible at the call site. The "Fingerprint is metadata, not a reference" framing only holds because the variant has the right semantics in *every* visitor; a single missed visitor is a soundness bug.
- **The `Fingerprint` element doesn't behave like context anyway.** Real `NixStringContext` elements affect derivation hashing; the fingerprint is a parse-cache hint that can disappear without breaking soundness. Encoding hint-data in a hashing-relevant type misclassifies its semantics.

The side-table design avoids all of this by keeping the fingerprint *out of* `NixStringContext` entirely. The cost is one extra hashtable lookup per `prim_fromJSON` / `prim_fromTOML` call (microseconds) and the `StringData *` keying constraint, both bounded.

### Side-table design

Add to `EvalState`:

```cpp
// libexpr/include/nix/expr/eval.hh
struct StringFingerprint {
    CanonPath returnedPath;          // from getFingerprint's first component
    std::string fingerprint;         // the suffix-bearing form, e.g. "blob:B;e=H"
};

// Keyed on the const StringData* allocation identity. The payload's str
// field is `const StringData *` (Value::StringWithContext::str), so the
// key type matches.  Side-table entries are cleared en-masse on
// resetFileCache() (REPL :reload), matching fileEvalCache's clear hook.
//
// The traceable_allocator keeps the StringData* keys reachable from the GC
// root set: without it, a side-table-only reference to a StringData would
// not prevent collection, and address reuse on the next allocation could
// cause a stale entry to answer for a fresh string. (Same defensive reason
// fileEvalCache uses traceable_allocator<pair<SourcePath, Value*>>.)
boost::concurrent_flat_map<
    const StringData *,
    StringFingerprint,
    std::hash<const StringData *>,
    std::equal_to<>,
    traceable_allocator<std::pair<const StringData * const, StringFingerprint>>
> stringFingerprints;
```

`prim_readFile` registers an entry after constructing the result string. The
trick is that master's `Value::mkString(string_view, context, mem)` allocates
the `StringData` internally (via `StringData::make(mem, ...)`) and gives no
direct handle back. Two compatible idioms:

```cpp
// libexpr/primops.cc — prim_readFile, option A: split the alloc + mkStringNoCopy
//
// StringData::make returns `const StringData &` (a reference to GC-allocated
// storage); we get the pointer back via address-of. mkStringNoCopy expects
// (const StringData &, const Value::StringWithContext::Context *), where the
// Context * is materialised from a NixStringContext via .fromBuilder(...).
auto fp = path.accessor->getFingerprint(path.path);
const StringData & sd = StringData::make(state.mem, s);
auto * ctx = Value::StringWithContext::Context::fromBuilder(context, state.mem);
v.mkStringNoCopy(sd, ctx);
if (fp.second)
    state.stringFingerprints.try_emplace(&sd, StringFingerprint{fp.first, *fp.second});

// Option B (simpler, equivalent): just call mkString and recover the pointer.
//
// After v.mkString(s, context, state.mem), v.string_data() returns the same
// `const StringData &` that mkString allocated. The address is stable because
// StringData is GC-allocated (the FAM lives in one piece on the GC heap and
// is not moved). One extra level of indirection vs option A; no semantic diff.
v.mkString(s, context, state.mem);
if (fp.second)
    state.stringFingerprints.try_emplace(&v.string_data(),
                                          StringFingerprint{fp.first, *fp.second});
```

`prim_fromJSON` consults the side-table:

```cpp
// libexpr/primops/fromTOML.cc and equivalent for fromJSON
auto str = state.forceStringNoCtx(*args[0], pos, /* error context */);
// Recover the StringData * the prim_readFile call registered. The payload
// is reached via the public string_data() accessor, which dereferences
// `getStorage<StringWithContext>().str`. That `str` field is a `const
// StringData *` set by mkString/mkStringNoCopy and copied bitwise through
// callFunction's vRes = vCur step (see "StringData identity preservation"
// below).
const StringData * sd = &args[0]->string_data();
if (auto fp = state.stringFingerprints.find(sd)) {
    auto cached = state.parseCache->lookup(fp->fingerprint, "json", fp->returnedPath);
    if (cached) {
        deserialiseValue(*cached, v, state);
        return;
    }
    // miss: parse, then serialise + upsert under the same key
    parseJSON(state, str, v);
    state.parseCache->upsert(fp->fingerprint, "json", fp->returnedPath, serialiseValue(v));
    return;
}
parseJSON(state, str, v);   // uncached fallback
```

`forceStringNoCtx` is unchanged. The side-table is invisible to existing callers; it changes no public behaviour and breaks no existing plugin.

### StringData identity preservation through `callFunction`

The load-bearing constraint: when `prim_fromJSON` looks up `&args[0]->string_data()`, that `const StringData *` must equal the one `prim_readFile` registered. Cross-tree verification against master at `nix/src/libexpr/eval.cc::callFunction` and `nix/src/libexpr/include/nix/expr/value.hh` confirms this works:

- **`mkString` allocates a fresh `StringData`** via `StringData::make(mem, sv)` (`value.hh::StringData::make` allocates on the GC-managed heap and returns a reference to the just-allocated object — no interning of string contents).
- **`callFunction` copies `Value` payloads bitwise**. Master's local is named `vCur` (not `vCur_inner` as an earlier draft of this section claimed). The relevant lines are `Value vCur(fun);` near the top of `callFunction`, then `fn->impl(*this, vCur.determinePos(noPos), args.data(), vCur);` for primop dispatch, then `vRes = vCur;` at the bottom — the `vRes = vCur` is a `ValueStorage` copy that carries the `StringWithContext { const StringData * str; const Context * context; }` payload through unchanged.
- **`prim_readFile` writes its result to the stack-local `Value & v` parameter** (which aliases `vCur` from `callFunction`'s frame). After the primop returns, `vRes = vCur` copies the `StringWithContext` (including the `str` pointer) bitwise into the caller's `Value`.
- **By the time `prim_fromJSON` runs** with `args[0]` pointing at a Value whose payload was set by that copy chain, `args[0]->string_data()` returns the same underlying `StringData` that `StringData::make` allocated inside `prim_readFile`'s `mkString` call.

The `Value *` is *not* preserved across this chain (different stack frames hold different `Value` instances, and `vCur` itself goes out of scope when `callFunction` returns); only the `StringData *` payload pointer is. Earlier sketches that proposed `Value *` keying were broken by exactly this — `prim_readFile` writes to `vCur` (not directly to `args[0]`), and the result is copied through `vRes = vCur`. Keying on `const StringData *` works because the payload pointer survives the copy.

### Address-reuse hazard and mitigation

The `const StringData *` is a pointer to GC-allocated memory; if the only references to it are via the side-table, the GC will eventually collect it, and a future `StringData::make` call may produce a `StringData` at the same address — resulting in a stale side-table entry answering a fresh string.

Two mitigations, applied together:

1. **`traceable_allocator`** on the side-table's underlying allocator (the `<pair<const StringData * const, StringFingerprint>>` form above). Boehm GC scans the allocator's blocks as part of its mark phase, so side-table entries keep their `StringData *` keys reachable. No address-reuse without an intervening `EvalState::resetFileCache` clear.
2. **Cleared on `EvalState::resetFileCache`** (REPL `:reload`, the same hook that clears `fileEvalCache` and `inputCache`). After a clear, all keys are invalidated together, and the GC is free to reclaim. The `prim_readFile` re-registration on the next eval re-populates the table for the live working set.

Without (1), the GC may collect a `StringData *` between `prim_readFile` writing the side-table entry and `prim_fromJSON` reading it — even within one eval. Defensive but mandatory.

### Encoded-value format

`parse-cache-v1.sqlite` schema (single table):

```sql
CREATE TABLE IF NOT EXISTS Documents (
    fingerprint TEXT NOT NULL,            -- e.g. "blob:abc...;e=def..."
    format      TEXT NOT NULL,            -- "json" or "toml"
    path        TEXT NOT NULL,            -- the (canonical) returned-path
    body        BLOB NOT NULL,            -- recursively-encoded Value tree
    PRIMARY KEY (fingerprint, format, path)
);
```

Body encoding (recursive, big-endian fixed-width fields):

```
EncodedValue = u8 tag | payload
  0x01 NULL
  0x02 BOOL  (u8)
  0x03 INT   (i64)
  0x04 FLOAT (f64)
  0x05 STRING (u32 length, bytes; no context — fromJSON inputs have none)
  0x06 LIST  (u32 count, count × EncodedValue)
  0x07 ATTRS (u32 count, count × (u32 name-len, name-bytes, EncodedValue))
  0xFF UNSUPPORTED (sentinel for functions/thunks/externals; the row should not exist)
```

Symbol *strings* (not symbol-IDs) are stored for attrset keys, so two cold processes serialise/deserialise identical bytes for the same parsed tree regardless of intern order. **Cycles cannot occur** in `fromJSON` / `fromTOML` parse output (these languages don't express cycles), so no sentinel is needed for that case; functions/thunks similarly cannot occur (these inputs produce only data values). The `0xFF` sentinel is defensive against future format extensions.

### `parse-cache-v1.sqlite` is its own database, not a table in the eval cache

Master's eval cache is per-fingerprint (`eval-cache-v6/<input-fingerprint>.sqlite`, with `<input-fingerprint>` being the *flake-input* fingerprint). Item k's keys are *file-content* fingerprints (`blob:<sha>` from item j); a single `flake.lock` typically references many such files, and partitioning on flake-input fingerprint loses cross-input sharing — the same `manifest.json` content read from two distinct flakes would land in two separate eval cache files, defeating the parse-cache's purpose.

Per-machine `parse-cache-v1.sqlite` (next to `fetcher-cache-v4.sqlite`) is the correct shape:

- One file per machine, all parse-cached documents share it.
- Schema versioning via the filename suffix (parallels `fetcher-cache-v4`).
- WAL mode + ~2-second `busy_timeout` (matches `fetcher-cache-v4`'s pattern) for concurrent process access.
- Row-per-document, not row-per-attribute. The eval cache's `AttrType`-discriminated row-per-attr encoding is *wrong* for whole-document parse output — reconstructing a thousand-entry attrset would need a thousand SQL round-trips, dominated by `sqlite3_step` overhead. Row-per-document is one read per call.

### Why bytes-only caching isn't enough

A simpler design — cache the *original bytes* (JSON/TOML) keyed on content-fingerprint and re-parse on hit — looks tempting because the input is already a deterministic textual format. But parsing dominates the runtime for the headline workload (a manifest with thousands of zone entries; tecnix's `tectonixManifestJson` exists *because* parsing is expensive). Caching just the bytes saves the disk read but not the parse, and the OS file cache already handles disk reads. **Item k's value comes from caching the parsed tree.** Bytes-only would be net-zero for the manifest workload.

### Per-`readFile` cost

Master's `prim_readFile` doesn't call `getFingerprint`; item k's `prim_readFile` adds one `getFingerprint` call plus one `try_emplace` into the side-table. For commit-rooted git inputs, `getFingerprint` is a tree-walk through the wrapper chain — microseconds per call, bounded by the State lock's brief acquisition. The `try_emplace` is a hashed `concurrent_flat_map` insert (a few hundred nanoseconds). For a workload with thousands of `prim_readFile` calls (rare in user code but possible in heavily-introspecting flakes), the cumulative overhead is bounded milliseconds. Acceptable; the parse-cache hits saved on `fromJSON` calls dominate.

### Cache invalidation

The fingerprint is content-derived; rows never need invalidating. The `cache_v1` filename suffix prevents cross-version aliasing if `fromJSON`'s parser semantics change (e.g. integer overflow handling) — bump to `parse-cache-v2.sqlite` and old rows are orphaned. Within a process, the side-table is cleared on `EvalState::resetFileCache()` (REPL `:reload`) — same hook that clears `fileEvalCache`.

### What this gives users

`let manifest = builtins.fromJSON (builtins.readFile (worldRoot + "/.meta/manifest.json")); in manifest."//areas/tools/foo".id` becomes O(parse-skip) on second cold eval. Tecnix's `__unsafeTectonixInternalManifest` is no longer pulling its weight; deleting it is the migration. The pattern applies to any user-side `prim_fromJSON (prim_readFile X)` — manifest files, configuration files, generated metadata, lock-file *fragments* parsed by user-level Nix code. **It does not apply to the *core* flake-input resolution path**: `libflake/flake.cc` parses `flake.lock` via dedicated `readLockFile` machinery (not `prim_fromJSON`), and that path is unaffected by item k. **Item k saves the parse, not the file read** — `prim_readFile` always reads the file before `prim_fromJSON` runs (no lazy-strings yet), so file I/O happens but is bounded by the OS file cache. For tecnix's manifest with thousands of zone entries, parsing dominates: tens of milliseconds parse → microseconds cache lookup + deserialise.

### Concrete size

| Component | LOC |
|---|---|
| `parse-cache-v1.sqlite` open + WAL + busy-timeout + schema + prepared statements | ~120 |
| `lookup`/`upsert` methods on `ParseCacheImpl` (custom call shape, not generic typeclass) | ~80 |
| Row-per-document `Value`-tree binary serialisation/deserialisation (per "Encoded-value format" above; symbol-strings rather than symbol-IDs for cross-process determinism; recursive) | ~250 |
| `EvalState::stringFingerprints` side-table (declaration, traceable_allocator wiring, clear hook in `resetFileCache`) | ~40 |
| `prim_readFile` `getFingerprint` + side-table `try_emplace` | ~30 |
| `prim_fromJSON` / `prim_fromTOML` cache-lookup + deserialise + miss-path serialise+upsert wiring | ~80 |
| Tests (round-trip, cross-process, cache-miss, cache-hit, side-table-clear-on-reload) | ~30 |
| **Total** | **~630** |

The earlier ~245 LOC estimate undercounted serialisation depth and missed the SQLite open/schema work and the side-table machinery. Approximately 630 LOC is the realistic figure. No new public API; no `NixStringContext` change; no `forceStringNoCtx` change.

---

## 5. Filter identity (closing §6.6 inside the existing API)

(This section is a concrete instance of the algebra in §0: it computes a content-addressed identity for a Nix lambda + captured environment, structurally analogous to the `pathDerivationModulo` / `hashDerivationModulo` pair's provenance-strip-and-replace move for `CAFixed` deps (see §0 for the precise attribution — `pathDerivationModulo` is where upstream drv-path provenance is replaced with content-hashes; `hashDerivationModulo` itself includes a content-determined storePath in its CAFixed branch). Maziarz, Ellis, Lawrence, Fitzgibbon & Peyton Jones, *"Hashing Modulo Alpha-Equivalence"* (PLDI 2021, [arXiv:2105.02856](https://arxiv.org/abs/2105.02856)) gives the load-bearing algorithm: O(n log² n) bottom-up Merkle-style hashing of lambda-term ASTs invariant under alpha-renaming, with a published correctness proof. Unison's content-addressed code ([unison-lang.org](https://www.unison-lang.org/docs/the-big-idea/)) is the production precedent. The §0 framing — deferred handle + content-determined result — applies here with the cache row keyed on `(source-fingerprint, filter-identity, method, path)`.)


The survey diagnoses that every `builtins.path { ...; filter = f; }` call bypasses the on-disk fetcher cache because [`fetch-to-store.cc:41`](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/fetch-to-store.cc#L41) sets `fingerprint = nullopt` whenever a `PathFilter` is supplied. The reason this is correct today: the inner accessor's `getFingerprint(path)` doesn't know about the filter, so a cache key built from it would alias different filters together.

The constraint we must respect: **`builtins.path`'s public signature is frozen.** No `fileset = ...` arg, no `filterId = ...` arg. The user passes a Nix lambda as `filter`, and that's all we have.

The fix is to derive a stable identity for the lambda *purely C++-side*, then incorporate it into the cache key. This is what survey item **a** calls "fold the filter into the fingerprint." This section makes it concrete.

### What's available about a lambda value

When `builtins.path`'s primop receives `filter`, it gets a `Value*` whose runtime form is `Value::Lambda { Env * env; ExprLambda * fun; }` (`src/libexpr/include/nix/expr/value.hh`). Of these:

- **`fun->pos`** (`src/libexpr/include/nix/expr/nixexpr.hh`) is the source position of the lambda's definition (file + line + column).
- **`fun->body`** is the AST.
- **`env`** is a GC-allocated chain of captured `Value*`s. Its identity is a heap pointer — different across invocations even for the same lambda. **Not directly hashable.**

What the C++ side is missing today: a serialisation that captures both the lambda's source content and the *values* of its free variables in a way that is byte-identical across cold processes. The existing function-value serialisers (`printFunction`, `value-to-xml.cc`, `print-ambiguous.cc`) all bottom out at constant or position-only stubs (`«lambda NAME @ POS»`, the constant `<LAMBDA>`, an `<attrspat>`/`<varpat>` with no body content). The eval cache refuses to cache function-typed attrs (no `nFunction` in its `AttrType` enum). Nothing today computes a content-keyed identity for a lambda.

The structurally robust answer is to hash the lambda's *source-region content* (the bytes between the lambda's begin and end positions in the original source file) together with a content-determined walk over its captured free variables. Source bytes are deterministic functions of the source file; free-variable values are content-determined when walked through the rules below; heap pointers never enter the hash.

### Derivation of filter identity

We add a single C++ helper: `EvalState::computeFilterIdentity(Value & filter) → std::optional<Hash>`. It's called by `FilterIdentitySourceAccessor::getFingerprint` (lazily, on first invocation, then memoised on `cachedId`). It returns `nullopt` for any case it can't safely handle; the wrapper then returns `std::nullopt` for the fingerprint, preserving today's bypass behaviour. (No `ctx` parameter — the algorithm walks the lambda and captures, never accessing the surrounding `NixStringContext`. Earlier sketches had a `ctx` parameter that was unused; dropping it removes a lifetime-management hazard for the wrapper.)

The algorithm. For a target `Value f`:

0. **Verify `f` is a lambda.** Only `tLambda` values have an `ExprLambda * fun` to hash from. Primops, primop-applications, externals, and partial applications of lambdas (forced thunks that yielded `tLambda` again are fine; everything else bails). If `f.type() != nFunction` or `!f.isLambda()`, return `nullopt` immediately. The `FilterIdentitySourceAccessor` constructor accepts any `Value &` (master's `addPath` doesn't pre-check either); the bail-to-`nullopt` is what preserves today's bypass semantics for non-lambda filters.

1. **Hash the source-region body.** Compute `body_hash = SHA256(source-bytes(f.fun->pos .. f.fun->endPos))`. This requires `ExprLambda` to track `endPos: PosIdx` — currently it tracks only `pos` (begin); `ExprCall` already tracks an analogous `cursedOrEndPos` for issue #11118 warnings, so the parser-side primitive is established. Adding `endPos` to `ExprLambda` is one field (4 bytes) plus parser-side wiring at the lambda-rule reduction. Source bytes are recovered via `Pos::getSnippetUpTo`, already used by `DocComment::getInnerText`.

2. **Identify free variables.** The parser has already resolved every `ExprVar` inside the body to a `(level, displ)` pair plus a `fromWith` field via `bindVars`. A free variable is one whose `level` escapes the lambda's own binder depth. Walk the body once per `ExprLambda` and memoise the result on the AST node (a `mutable std::vector<ExprVar *> freeVars` field, populated at most once and protected by `std::call_once`). The walker is structural over the 27 `Expr*` kinds in `nixexpr.hh`.

3. **Hash the captured values.** For each free `ExprVar`, look up the captured `Value*` via `lookupVar(filter.lambda().env, *var, /*noEval=*/true)`. Walk the captured value:

   - `nInt` / `nBool` / `nNull` / `nFloat` / `nString`: canonical bytes.
   - `nPath`: `accessor.fingerprint() + subpath` if the accessor has a fingerprint; else bail (`nullopt`).
   - `nList` / `nAttrs`: force lazily, recurse on each element. Sort attrset entries by `std::string_view(state.symbols[s])` (the string content of the symbol — `SymbolStr` has an implicit conversion to `string_view` defined at `symbol-table.hh:154`), **never** by `Symbol::operator<=>` — `Symbol`'s default `<=>` (declared `= default` at `symbol-table.hh:74`) compares the private `uint32_t id`, which is intern-order in the `SymbolTable`, not lexicographic.
   - `nFunction.isLambda()`: recurse into the source-region + capture-walk algorithm on the captured lambda. This is the load-bearing recursion: nested captured lambdas (e.g. `cleanSourceWith`'s wrapper captures `cleanSourceFilter`) are hashed by their own source bytes plus captures, not by `(ExprLambda*, Env*)` heap pointers.
   - `nFunction.isPrimOp()`: hash `primOp->name` (the static string identifier — primops are pure, deterministic, and identified by name).
   - `nFunction.isPrimOpApp()`: walk the application chain via `primOpAppPrimOp()` and `app().left/right`, hash `primOp->name + recursive identities of the applied args`.
   - `nExternal`: bail (`nullopt`). `Value::type()`'s lookup table maps `tExternal` to `nExternal`, not `nFunction`, so this case is not caught by the function predicate; it must be handled explicitly.
   - `nThunk`: force, then retry; if force throws and it's not `Interrupted`, bail. `Interrupted` is re-thrown so user `^C` is honoured.

4. **`with`-resolved free vars** require special handling. If `var.fromWith != nullptr`, `lookupVar` with `noEval=true` returns null because the binding is computed at call time by walking up the env chain and forcing the `with`-attrset. The algorithm forces the `with`-attrset and looks up `${var.name}` in it, recursively walking that value. This is the soundness fix for shadowing: filter A under `with cfg1; expr` and filter B under `with cfg2; expr` accept different paths if `cfg1.flag != cfg2.flag` and `expr` references `flag`; without the slot lookup, the body bytes are identical and the cache aliases incorrectly.

5. **Termination on mutual recursion.** Maintain `seen: map<ExprLambda*, Hash>` within one walk. On re-entry to a lambda already being walked, emit a placeholder `«rec N»` where N is the depth-position in the current walk. The placeholder is content-determined; `seen` is local to one walk and never embedded in the output hash directly.

6. **Catch errors and bail.** Wrap the whole walk in `try { ... } catch (Error & e) { if (dynamic_cast<Interrupted *>(&e)) throw; return nullopt; }`. A captured thunk may throw on force; we must not propagate that as an evaluation error from inside cache-key derivation.

7. **Budget.** Depth limit (~64) and node-count limit (~100k) bound worst-case cost (e.g. a closure that captures `nixpkgs.lib`). Exceeding either yields `nullopt` and falls back to today's bypass — slower than ideal but never wrong.

8. **Combine.** `filter_identity = SHA256("filter-identity-v1\0" || body_hash || "|" || join(capture_hashes) || "|" || nix-version-tag)`. The version tag prevents cross-version aliasing of primop-by-name when a Nix release changes a primop's semantics.

### Filter identity as a wrapper accessor (canonical pattern)

Per §4's "`getFingerprint` is the canonical content-identity surface", filter identity flows as a `;f=<hash>` suffix appended by a wrapper accessor — not as a special-case in `fetch-to-store.cc`. The wrapper is structurally analogous to `GitExportIgnoreSourceAccessor`: it forwards every read method to its inner accessor (with the path filter applied) and overrides `getFingerprint` to append the filter's identity.

```cpp
// New: src/libfetchers/filter-identity-source-accessor.{hh,cc}
struct FilterIdentitySourceAccessor : FilteringSourceAccessor {
    EvalState & state;
    // RootValue, not Value & — the filter Value is GC-managed memory and
    // the wrapper outlives the call frame that constructed it (it is
    // make_ref<>'d into the SourcePath that flows down into fetchToStore).
    // RootValue is the master-public typedef `std::shared_ptr<Value *>`
    // (libexpr/value.hh); allocRootValue(v) is the matching free function
    // that wraps a Value* in such a shared_ptr to keep the Value reachable
    // from the GC root set without depending on stack lifetime.
    RootValue filterValue;

    // Default-constructed unset; filled on first getFingerprint call.
    // The outer optional distinguishes "not yet computed" from
    // "computed and returned nullopt"; the inner optional is the bypass
    // signal from computeFilterIdentity.
    mutable std::optional<std::optional<Hash>> cachedId;

    // The base's ctor takes (const SourcePath &, MakeNotAllowedError &&)
    // and records (next, prefix) — the prefix is the subpath at which the
    // filter is rooted. For builtins.path { path = X; filter = F; },
    // src is { X.accessor, X.path }.
    FilterIdentitySourceAccessor(const SourcePath & src, EvalState & s, RootValue f)
        : FilteringSourceAccessor(src, /*makeNotAllowedError=*/{})
        , state(s), filterValue(std::move(f)) {}

    bool isAllowed(const CanonPath & path) override {
        // *filterValue is `Value *` (RootValue is shared_ptr<Value*>);
        // callPathFilter's signature is callPathFilter(Value * filterFun, ...).
        return state.callPathFilter(*filterValue, {next, prefix / path}, noPos);
    }

    // Per §4's "forward and append" composition rule, override
    // computeOwnSuffix. nullopt = bypass; non-empty = the suffix to
    // splice into the inner fingerprint at its alphabetical position.
    std::optional<std::string> computeOwnSuffix(const CanonPath & path) override {
        if (!cachedId)
            // **filterValue is `Value &` (the held pointer dereferenced);
            // computeFilterIdentity takes a Value & by reference.
            cachedId = state.computeFilterIdentity(**filterValue);
        if (!*cachedId)
            return std::nullopt;   // unhashable filter: bypass
        return ";f=" + (*cachedId)->to_string(HashFormat::Base32, false);
    }
};
```

`builtins.path { filter = ...; }`'s implementation in `primops.cc` then constructs the wrapper instead of carrying the filter through `fetch-to-store.cc` as a separate parameter:

```cpp
// In addPath, when filterFun is non-null:
SourcePath sp{path.accessor, path.path};
if (filterFun) {
    // allocRootValue is a free function (eval.cc) returning RootValue
    // = shared_ptr<Value*>; it pins filterFun in the GC root set so it
    // survives until the wrapper is destroyed.
    auto filterRoot = allocRootValue(filterFun);
    auto wrapper = make_ref<FilterIdentitySourceAccessor>(sp, state, filterRoot);
    sp = SourcePath{wrapper, CanonPath::root};   // the wrapper IS the rooted accessor
}
// Then proceed with addPath using sp uniformly:
auto dstPath = fetchToStore(state.fetchSettings, *state.store, sp, ...);
```

**Note the SourcePath construction.** The wrapper records `prefix = path.path` internally, then re-roots: subsequent operations on `sp` work with paths *relative to the wrapper*, not relative to the underlying accessor. `fetchToStore` calls `sp.accessor->getFingerprint(sp.path)` = `wrapper->getFingerprint(/)`, which calls the base `FilteringSourceAccessor::getFingerprint(/)` = `next->getFingerprint(prefix / /)` = `next->getFingerprint(prefix)` = the inner `GitSourceAccessor`'s fingerprint for the subpath, with item j's tree/blob branch firing. The wrapper appends `;f=<hash>` and returns.

**The user's filter lambda receives the original absolute paths** (master's existing behaviour). The wrapper's `isAllowed` translates wrapper-relative paths back to user-visible form via `prefix / path` before invoking `state.callPathFilter` — so a user's `path: type: hasSuffix ".nix" path` filter against `builtins.path { path = input + "/sub"; filter = f; }` receives paths like `/nix/store/HASH-source/sub/file.nix`, not `/file.nix`. The re-anchoring is purely internal; no user-visible behaviour change.

The `fetch-to-store.cc` ternary disappears: `fetchToStore` now always calls `accessor->getFingerprint(path)` and the wrapper does the right thing. Identity-affecting concerns become wrappers; `fetchToStore` stays generic.

**Why this is strictly cleaner than the previous "fingerprint ternary" sketch.** (1) `fetch-to-store.cc` no longer takes a `PathFilter *` parameter — every consumer routes through `SourceAccessor::readFile`/`dumpPath`, with filtering already applied by `FilteringSourceAccessor::isAllowed` inside the wrapper. The signature simplifies from `fetchToStore(settings, store, path, mode, name, method, filter, repair)` to `fetchToStore(settings, store, path, mode, name, method, repair)`. (2) Identity is computed at the only place that knows it (the wrapper), not at the cache-key call site. (3) Future identity-affecting concerns (e.g. an opt-in "ignore mtime" wrapper, an LFS-smudge-cache wrapper) plug in as new wrapper classes without touching `fetchToStore` at all. (4) The wrapper's `cachedId` saves nothing within one `addPath` call (`fetchToStore` calls `getFingerprint` only once); the cross-invocation savings come from §5's within-`EvalState` `(ExprLambda*, Env*)` hash-cons. The wrapper's field is defensive, in case future refactors call `getFingerprint` more than once on the same wrapper instance.

**Why `fetchToStore` no longer needing `filter` is a clean win.** Master's `fetchToStore` body has `auto filter2 = filter ? *filter : defaultPathFilter;` and threads `filter2` into both `computeStorePath` (DryRun) and `addToStore` (Copy). Under the wrapper, `addToStore` and `computeStorePath` see filtered content via the accessor's `readFile`/`dumpPath` — the path-filtering is already inside the accessor's behaviour. The two methods need to consult the wrapper's `isAllowed` predicate when walking; master's `addToStore(SourcePath, ..., PathFilter & filter, ...)` has both an accessor *and* a separate filter, with awkward semantics about which takes precedence. Under the wrapper-accessor pattern, the only filter is the one inside the accessor; `addToStore` just walks. The `addToStore` signature can drop its `PathFilter` parameter too, in a follow-on cleanup. (Out of scope for this proposal; flagged as a natural follow-up.)

### Determinism

Two cold processes evaluating the same source produce identical: `lambda.pos` and `endPos` (deterministic functions of source text), `getSnippetUpTo` source bytes (function of source text), `(level, displ)` pairs (deterministic from `bindVars`), and captured-value walks (each step content-determined). Symbol-string ordering replaces symbol-id ordering. Heap pointers no longer enter the hash. The headline workload `lib.cleanSource ./.` — whose wrapper closure captures `cleanSourceFilter` as a lambda value — hashes deterministically across cold processes because the captured lambda is recursed-into via source-region, not pointer-pair.

### Failure modes (all safe)

- **Two textually identical lambdas at different source positions.** PosIdx is not in the hash; only the source bytes between the lambda's begin and end positions are. Identical bytes hash identically. Two flakes that copy the same predicate definition share a row. Sound because identical body bytes in identical static-env scopes bind to the same captured values.
- **`with`-introduced free vars.** Handled by step 4 (force the `with`-attrset slot and recurse). Unsound if elided.
- **Captured primop / primop-app.** Hashed by name (and recursively by applied args for `primOpApp`). The common-case nixpkgs filters (`cleanSourceFilter`, fileset-internal `nonEmpty`) all rely on this.
- **Captured value that throws on force.** Caught at step 6 → `nullopt` → bypass. Same observable behaviour as today.
- **Externals.** Step 3 explicitly bails for `nExternal`. The `Value::type()` table maps `tExternal` to `nExternal`, not `nFunction`, so this case requires explicit handling.
- **Mutually recursive let bindings.** `seen: map<ExprLambda*, Hash>` plus `«rec N»` placeholder terminates without infinite recursion.
- **Cosmetic source edits.** Whitespace and comment changes inside the lambda body change the source-region bytes and therefore the hash. Acceptable for cache-busting.
- **Cross-version primop semantics drift.** `builtins.foo` in Nix N+1 may differ from Nix N. The `nix-version-tag` in step 8 segregates rows.

### Implementation notes

- **Versioned domain prefix.** `"filter-identity-v1\0"` lets future revisions (`-v2`) coexist if the algorithm changes. Cache rows under different prefixes never collide.
- **Streaming `HashSink` is the right primitive.** `src/libutil/include/nix/util/hash.hh` exposes a streaming SHA-256 sink derived from `BufferedSink`; feed bytes directly rather than materialising a flat buffer.
- **Memoise the free-vars list, not the final hash.** Different invocations of the same lambda capture different envs; only the AST-side analysis is reusable.
- **Hash-cons within an `EvalState` on `(ExprLambda*, Env*)`.** When the same lambda value is forced multiple times in one `EvalState` (e.g. a flake with N `mkDerivation` calls each using `lib.cleanSource ./.` against captures from `let inherit (pkgs) lib;` — same `Env*`), the captured-value walk is identical each time. A small `boost::concurrent_flat_map<std::pair<const ExprLambda *, const Env *>, std::optional<Hash>>` on `EvalState` collapses N walks to one. **`concurrent_flat_map`, not `unordered_flat_map`** — under `eval-cores > 1`, two threads may both see a miss for the same `(lambda, env)` pair, both compute the hash, both insert. The result is identical (content-determined), so `try_emplace`-based concurrent insertion is correct (both threads observe the same final state). Sequential `unordered_flat_map` would race on the insertion. This is the wrapper-accessor's `cachedId` field generalised to "cache across distinct `FilterIdentitySourceAccessor` instances that wrap the same `(lambda, env)` pair." ~30 LOC. Within-`EvalState` only; not persisted (the `Env*` lifetime is bounded by `EvalState`).
- **IFD inside filter identity is a real semantic concern, not a bug.** The capture walk forces thunks that may invoke `builtins.trace`, `builtins.warn`, or trigger import-from-derivation. Users who write filters that perform IFD already see those effects on every invocation today; computing a stable identity once and caching it strictly *reduces* this work. The algorithm does not try to suppress IFD inside the walk — that would be a separate, larger semantic change.
- **`with`-slot lookup cost.** When the `with`-attrset is large (e.g. `with builtins; ...`), forcing slots can be expensive. The cost on `cleanSourceFilter`-style captures is unmeasured. Mitigation if expensive: cache the `with`-slot lookup result per `(with-Value, Symbol)` pair within one walk.
- **Concrete size.** Adding `endPos: PosIdx` to `ExprLambda` plus the capture walk and free-var walker is approximately 300 LOC total — similar in shape to `forceValueDeep` plus a structural AST walker, plus parser wiring for the new field.

### Empirical bail-rate audit

**Audited (nixpkgs):**

- **No `with` clauses** anywhere in `nixpkgs/lib/sources.nix`, `nixpkgs/lib/fileset/default.nix`, `nixpkgs/lib/fileset/internal.nix`. All bindings are `let`/`inherit` (static). The `with`-shadowing case never fires for the dominant nixpkgs idioms.
- **`lib.cleanSource ./.`** — the lambda passed to `builtins.path` is the `cleanSourceWith` wrapper, whose direct captures are `filter` (a lambda) and `orig` (an attrset of lambdas/paths/strings). Captured lambda recursion descends into `cleanSourceFilter`'s source region; primops hash by name. **Passes.**
- **`lib.fileset.toSource { ... }`** — the same `cleanSourceWith` wrapper, with `filter = _toSourceFilter fileset`. Mutually recursive `inTree`/`_normaliseTreeFilter` are handled by the `seen`-map placeholder. **Passes.**
- **`lib.cleanSourceFilter` standalone** (rare in practice): captures `baseNameOf`, `toString`, `lib.strings.match` directly — all primops, hashed by name. **Passes.**

**Not yet audited but plausibly broader-bail-rate:**

- **`haskell.nix` source filtering.** `cleanSourceWith` chains with predicates that capture `pkgs.lib.something` recursively. The capture walk descends into `lib`; at depth ~64 or node count ~100k it bails. The capture chain through `pkgs.lib` is wide but mostly attrsets-of-functions; primops cap recursion via name, attrsets sort by symbol-string. **Likely passes**; not measured.
- **User-written filters that capture `pkgs` directly** (e.g. `path: type: pkgs.lib.hasSuffix ".nix" path` without a closing `over` argument). Capture walk includes the entire nixpkgs `lib` attrset — wide but shallow per attribute, since each value either resolves to a primop, a constant, or another attrset. The depth limit (~64) is the binding constraint; node-count (~100k) is generous. **Likely passes for direct `lib.*` references; bails on filters that capture entire derivations or large evaluated trees.**
- **Filters that capture path values via `builtins.toString`.** `nPath`'s rule is `accessor.fingerprint() + subpath`; if the path's accessor lacks a fingerprint (a corner case for accessors that are not flake-input-rooted, e.g. ad-hoc `builtins.path { path = builtins.toString ./relative; }`), the walk bails. **The bail is sound** — the algorithm preserves today's bypass behaviour — but the bail-rate on real workloads is the empirical question that decides cache hit ratio.

The audit covers what's *known to hash cleanly* (the dominant `lib.cleanSource` / `lib.fileset.toSource` shapes); the broader bail-rate is a measurement question for a prototype against (a) nixpkgs's `pkgs.haskellPackages.callPackage` graph, (b) `flake-utils-plus`-style filter composition, (c) tecnix's own filter usage. **The proposal's safety claim — "bail is never wrong, just slower than ideal" — holds regardless of bail-rate.** What's at stake is not correctness but the cache hit ratio on filtered `builtins.path` workloads.

### What this gives users

- `mkDerivation { src = lib.cleanSource ./.; }` becomes cache-hit-able on second cold eval. First eval writes a row keyed on `(source-fingerprint, filter-identity, method, path)`; second hits.
- `mkDerivation { src = lib.fileset.toSource {...}; }` likewise.
- Two flakes that copy the same predicate definition into different files share a row (positions are excluded; only source-region bytes and captured-value content matter).
- No public API changes. No nixpkgs-side cooperation required. Old code paths keep working — only the cache lookup gets richer.

---

## 6. Non-blocking evaluation IO (refining items d and e)

The survey's items **d** (unify `mountInput` / `LazyAttr` / `makeLazyNarAccessor`) and **e** (lockfree concurrent reads) together address evaluation-time IO. This section refines both with details from the libgit2 thread-safety constraints.

### What blocks today

Every `lstat`, `readDirectory`, `readFile` call from a Nix primop (e.g. `builtins.readFile`, `builtins.pathExists`) goes through `SourceAccessor` synchronously. For a `GitSourceAccessor`, master's [`readBlob` (git-utils.cc#L807-L837)](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git-utils.cc#L807-L837) takes `state_.lock()` on its first line (a `Sync<State>` exclusive mutex per the survey's §7.5) and holds it for the entire read — tree walk, OID lookup, and blob streaming all under one lock. With `eval-cores > 1`, this serialises every read through every parallel evaluator thread.

### Refined item e: split the State lock from the ODB read

libgit2's threading model ([`docs/threading.md#L4-L25`](https://github.com/libgit2/libgit2/blob/0609130af/docs/threading.md#L4-L25)) is the lever: `git_odb` is internally thread-safe and shareable; `git_repository`, `git_tree`, and most other handles are not. So a blob read decomposes into two phases with different locking requirements:

```cpp
// Refactored GitSourceAccessor::readBlob:
void readBlob(const CanonPath & path, bool symlink, Sink & sink, ...) {
    git_oid oid;
    bool wantsLfsSmudge;
    const lfs::Fetch * lfsFetch = nullptr;  // borrowed, not owned
    {
        // Phase 1 (lock-needing): tree walk to resolve path → oid,
        //   plus LFS-smudge decision (which only needs git_tree access).
        // Touches non-threadsafe git_tree / git_repository handles.
        auto state = state_.lock();
        oid = lookupOid(*state, path);
        wantsLfsSmudge = state->lfsFetch && state->lfsFetch->shouldFetch(path);
        if (wantsLfsSmudge)
            // Borrow a pointer; lfs::Fetch is const after construction
            // and outlives this readBlob call (state owns it for the
            // accessor's lifetime). lfs::Fetch::fetch is const-qualified
            // and never re-enters libgit2's git_repository / git_tree
            // (audited against master git-lfs-fetch.cc — the only
            // libgit2 calls live in the constructor and shouldFetch,
            // both of which run under the State lock above). Phase 2b
            // is therefore lock-free with respect to libgit2. Note: the
            // LFS on-disk cache directory (~/.cache/nix/git-lfs/<oid>)
            // is *not* mutex-protected; two threads racing on the same
            // missing oid double-fetch and double-write. The double-write
            // is tolerable (writes are idempotent for content-addressed
            // files), but is a known suboptimality flagged for future
            // dedup via the same compute-in-progress registry as §2.
            lfsFetch = &*state->lfsFetch;
    }
    // Phase 2 (lock-free): blob read via ODB directly.
    // git_odb_read() takes its own internal lock; multiple threads can call concurrently.
    git_odb_object * raw = nullptr;
    check(git_odb_read(&raw, sharedOdb, &oid));
    Finally cleanup([&]{ git_odb_object_free(raw); });
    auto view = std::string_view(
        (const char *) git_odb_object_data(raw), git_odb_object_size(raw));

    if (wantsLfsSmudge) {
        // Phase 2b (lock-free): LFS smudge runs against the borrowed
        // lfs::Fetch instance. The HTTPS LFS fetch is self-contained
        // network IO; it does not touch git_repository or git_tree, so
        // it is safe outside the State lock. (Master's existing FIXME —
        // "do we need to hold the state lock while doing this?" — is
        // answered no by the phase split.)
        StringSink s;
        lfsFetch->fetch(std::string(view), path, s, [&s](uint64_t size) { s.s.reserve(size); });
        StringSource source{s.s};
        source.drainInto(sink);
    } else {
        // Use drainInto rather than sink({data,size}) directly: drainInto
        // chunks at the BufferedSink's buffer size (32KB by default), so
        // large blobs do not synthesize a single large sink call that some
        // consumers (e.g. NAR-building string sinks) handle poorly.
        StringSource source{view};
        source.drainInto(sink);
    }
}
```

**The borrowed-pointer pattern requires `state->lfsFetch` to be stable for the lifetime of `readBlob`.** That holds: `Sync<State>` owns the State by value, the accessor owns the Sync, and `readBlob` doesn't outlive the accessor. The lock release between Phase 1 and Phase 2 doesn't destroy the State — it just makes it concurrently accessible. **The lfs::Fetch instance is not destructed mid-readBlob.** ✓

Phase 1 is brief (one tree-walk per read) and stays under the State lock. Phase 2 is the actual byte transfer and runs lock-free against the ODB; Phase 2b (the LFS smudge HTTPS fetch) also runs lock-free, because `lfs::Fetch::fetch` does not touch `git_repository`/`git_tree` — it's a self-contained HTTPS+SHA-256 verification routine. Concurrent readers contend only on the brief Phase 1; throughput scales with `eval-cores` instead of being capped by one mutex. This is the survey's item **e** with concrete code.

**Item j's `getFingerprint` also contends on the State lock** because it calls `lookup(*state, subpath)` which mutates `state->lookupCache` (the tree-entry memo). Same Phase-1-shaped cost: microseconds per call, contention bounded by `eval-cores`. The State lock now covers two distinct call types (tree-walks for `readBlob` and tree-walks for `getFingerprint`), but neither holds the lock for byte transfer. **The `lookupCache` is the shared resource; making it a `boost::concurrent_flat_map` instead of a `Sync<unordered_flat_map>`-protected member would remove the contention entirely** (the cache is content-keyed, so concurrent insertions are safe). Stage as a follow-on optimisation if measurement shows the State lock contention is the bottleneck.

**The sketch is a structural diagram, not a literal drop-in.** Master's `readBlob` does not call `git_odb_read` directly — it goes through `getBlob` → `git_tree_entry_to_object` (which internally calls `git_object_lookup` → `git_odb_read`). To realise the phase split, `getBlob` and the surrounding `lookup` machinery need refactoring so that Phase 1 returns just the OID (which `git_tree_entry_id` already exposes) and Phase 2 calls `git_odb_read` against the shared `git_odb` directly. The `lookupCache` (`boost::unordered_flat_map<CanonPath, TreeEntry>`) that today is mutated under `state_.lock()` becomes a contention point once Phase 2 is lock-free, because every `readFile` must complete Phase 1 before reaching Phase 2 — but Phase 1 is microseconds; the mutex contention drops from "blob streaming" to "tree-entry hash insertion." The structural win is real; the surgery is bigger than the sketch implies.

**LFS-smudge-cache as a future projection (item l, sketched).** The Phase 2b LFS smudge fetches the same content for the same blob OID across processes. Today master re-fetches per `readBlob` invocation when the OID is in the lfs-pointer file format. A natural projection-pattern instance: `Projection<git_oid, smudged-bytes>` keyed on the blob OID, stored as a `Store`-aware projection (the smudged content is large; a `StorePath` is the right value type). Items g, j, and §6's phase split together make this drop-in once needed: the `lfs::Fetch::fetch` call in Phase 2b consults `LfsSmudgeCache::lookup(oid)` first; on miss, fetch and upsert. **Not staged in §9** because LFS users are a smaller cohort than item h's beneficiaries; sized as a small (~50 LOC) addition once item g's substrate is in place. Tagged here as item **l** for the suffix schema's reservation: a per-blob LFS smudge identity would surface as a `;lfs=<smudge-hash>` suffix on the `blob:<sha>` fingerprint when the entire input has `lfs=true` set, *and* the relevant blob actually triggers smudging.

### Refined item d: lazy-attribute scope (not lazy-bytes)

The survey's item **d** proposes a unified `Lazy<T>` for "per-attribute and per-byte laziness." Per-attribute laziness already exists on master via `LazyAttr` ([survey §7.4 footnote](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/include/nix/fetchers/attrs.hh#L20-L34)) — it defers `revCount`, `lastModified`, etc. The natural extension is to make every L1 metadata projection (`getFingerprint`, `getProvenance`, `getLastModified`) optionally `LazyAttr`-backed.

**Per-byte laziness for `readFile` is out of scope** for this proposal: it would require a new Nix value kind ("lazy string") that ripples through every consumer (string interpolation, derivation hashing, JSON printing). The survey's existing `makeLazyNarAccessor` defers byte materialisation of NAR archives specifically — useful for substituters, not for arbitrary `readFile` calls.

What this proposal *does* deliver for non-blocking reads:

- **Phase 1/Phase 2 split (item e refinement)** removes the State-lock bottleneck for parallel evaluation.
- **Lazy clone (item h)** means reads that don't get triggered don't fetch from network; the survey's §2.4 "paths that don't appear in final output stay virtual" generalises to "blobs not actually read are never fetched."
- **`LazyAttr` extension** (item d, narrowed scope) defers per-attribute computation — useful for `getLastModified()` aggregation (survey §5.6) that today returns `nullopt` from combinators.

### What this gives users

- `eval-cores > 1` actually speeds up evaluation of a monorepo (today the State mutex is the bottleneck).
- Speculative reads (e.g. an IDE LSP eagerly evaluating multiple attributes in parallel) don't stall on each other.
- Combined with item **h**, blobs that don't end up affecting any final output are never even fetched from the remote, let alone read from disk.

---

## 7. Unified picture: how the items compose

### How composition works (and where it stops working)

The proposal's composition story is **transparent through key independence, not through chained lookups.** Each `Projection<>` instance is keyed on its own content-derived `From`; composed projections share rows with single projections automatically because each step's content-addressed output is the next step's content-addressed input. There is no chain of lookups that has to walk through intermediate caches.

For the dominant `builtins.path { path = input + "/sub"; }` use case under item j the asymptotics are:

| Workload | Today | After item j (cache hit) |
|---|---|---|
| Same rev, repeated `${input}/sub` | O(N) re-NAR each time | O(1) SQLite lookup on `sourcePathToHash` keyed on `tree:<sha>` |
| Different rev, *unchanged* subtree | O(N) re-NAR (parent fingerprint differs today) | O(D · log F) tree-SHA lookup + O(1) SQLite hit — the `tree:<sha>` key is independent of parent rev |
| Different rev, *changed* subtree | O(N) full re-NAR | O(S) NAR walk of subtree only + cache write |
| Tarball and git input that yield same tree-SHA | Two separate NAR walks | Cross-pipeline reuse via `treeHashToNarHash`: one walk total |

(*N* = parent tree size; *S* = subtree size; *D* = path depth; *F* = fanout.)

**Where Merkle structure helps**: subtree extraction is O(D · log F) because the git ODB indexes tree entries by name; cross-rev subtree dedup is automatic because tree-SHAs collide structurally on identical content.

**Where Merkle structure doesn't help**: filter identity (§5) is content-addressed but not Merkle-structured (lambdas have no canonical Merkle encoding); narHash in `mountInput` is over a NAR encoding distinct from the git tree-SHA encoding (so first-contact subtree-only fetching can't be transparent until lazy-narHash ships); per-derivation context-string rewrites in `derivationStrictInternal` are O(input-string × context-cardinality), not Merkle-amortised.

**Practical floor**: until lazy-`narHash` ships (§4's option 1, staged in §9 after the headline items), the first eval of any newly-locked input pays the O(N) dryRun walk regardless of subtree caching. The asymptotic story is "second eval onward is O(1) when content unchanged"; the *first* eval is gated by the narHash invariant.

The survey's **a**–**f** plus this proposal's **g**, **h**, **j**, **k**, plus the §5 filter-identity work (a concrete fleshing-out of survey item **a**) compose as follows. Each ships independently; wins compound.

| Item | Touches | Closes |
|---|---|---|
| **g** `Projection<Derived, From, To>` CRTP typeclass | new `src/libfetchers/include/nix/fetchers/projection.hh` — refactor of the ten ad-hoc cache domains | §7.3 (content-identity over-pluralised) |
| **a** Filter identity (§5) | New `FilterIdentitySourceAccessor` wrapper class (analogous to `GitExportIgnoreSourceAccessor`) that overrides `getFingerprint` to append `;f=<hash>`; new `EvalState::computeFilterIdentity` helper; `fetch-to-store.cc` simplifies (the `PathFilter *` parameter is removed; the bypass ternary disappears) | survey §5.1, §6.1, §6.6, §7.12 |
| **b** Tree-SHA → NAR-hash projection across ODBs | existing `treeHashToNarHash` domain reused on the git-mounted-subtree write/read paths | §7.1 |
| **c** NAR-hash-free CA path for git-rooted content | `Store::addToStore` (`store-api.cc`), new `FileSerialisationMethod::Git` | §6.1 first-walk cost |
| **d** `LazyAttr` extension for L1 metadata | `LazyAttr`, `getLastModified`, `getProvenance` aggregation | survey §5.6, §7.4 |
| **e** Lockfree concurrent reads | `GitSourceAccessor::readBlob` (split State-lock from ODB-lock — §6 refinement) | §7.5 |
| **f** Opt-in CLI devirtualise | `nix/eval.cc`, `nix-instantiate.cc`, `app.cc` print sites | §2.4 invisibility |
| **h** `PartialCloneTransport` + `PromisorBackend` + `prefetchSubtree` virtual | new `src/libfetchers/partial-clone-transport.{hh,cc}`, new `src/libfetchers/promisor-backend.{hh,cc}`, `GIT_OPT_SET_EXTENSIONS` whitelist at startup, `GitRepoImpl` ctor registers the backend at low priority, `SourceAccessor::prefetchSubtree` virtual + `EvalState` triggers | Network cost for large monorepos, entirely inside libgit2 |
| **j** Content-aware fingerprint in `GitSourceAccessor` (§4) | `GitSourceAccessor::getFingerprint` override returning `tree:<sha>` for tree-rooted and `blob:<sha>` for file-rooted subpaths (with `;e=<attrs-hash>` from `GitExportIgnoreSourceAccessor` when `exportIgnore=true`, sound for both branches); cross-pipeline reuse via `treeHashToNarHash`; ships with the `addPath` refs-bypass fix and the `FilteringSourceAccessor`/`MountedSourceAccessor` short-circuit removal | Cross-rev subtree *and* cross-rev byte-identical-file dedup for `builtins.path` / `builtins.filterSource`; cross-pipeline (tarball ↔ git) reuse; no public API change |
| **k** Content-keyed parse-cache (§4.5) | New per-machine `parse-cache-v1.sqlite`; row-per-document custom binary blob format for the value tree; per-`EvalState` `StringData *`-keyed side-table records `prim_readFile`'s fingerprint; `prim_fromJSON` / `prim_fromTOML` consume it. **No `NixStringContext` change, no `forceStringNoCtx` change.** | Re-parse cost on cold eval reduces to one cache lookup; survey §7.8 closed; no public API change |

### Concrete user-visible outcomes

| Workload | Today | After proposal |
|---|---|---|
| `mkDerivation { src = lib.cleanSource ./.; }` | NAR walk every cold eval (§6.6) | First eval writes `sourcePathToHash` keyed on `(source-fp, filter-identity, ...)`; second hits |
| `mkDerivation { src = lib.fileset.toSource {...}; }` | NAR walk every cold eval | Same — filter identity hashes the fileset library's lambda + captured tree |
| CI matrix evaluating N branches sharing a subtree | N NAR walks (§6.1) | 1 walk total, all branches hit `Projection<TreeSha, NarHash>` |
| Large monorepo where evaluation touches a small subset of blobs | Full pack clone + per-eval read | Commits + trees fetched eagerly; only blobs the evaluator actually reads come over the wire (via `PromisorBackend`). Quantitative reduction depends on repo blob-to-tree ratio; see §3.5. |
| Parallel evaluator threads reading the same monorepo `GitSourceAccessor` | Serialised on the per-accessor `Sync<State>` (every `readBlob` holds the lock for the entire body) | Phase 1 holds the State lock briefly for the tree-walk; Phase 2 reads the blob lock-free against `git_odb` (item **e** refinement) |
| `nix eval --json` of nested attrset with paths | Devirtualises every printed path | Item **f**: opt-in CLI devirtualise |
| Tecnix's monorepo zones (clean-tree access) | 4 of 11 `__unsafeTectonixInternal*` builtins (`TreeSha`, `Tree`, `ZoneSrc`, `ZonePath`) | **Retire conditional on declaring `worldRoot` as a flake input.** Tecnix's `worldZone` library wrapper switches from `__unsafeTectonixInternalZoneSrc zonePath` to `builtins.path { path = worldRoot + zonePath; }`. **Item j fires only when `worldRoot.accessor` is `GitSourceAccessor`** (i.e. the input is rev-pinned via the flake mechanism); pure-FS-path interpolation through `PosixSourceAccessor` gets no `tree:<sha>`/`blob:<sha>` branch and does not benefit. With the flake-input model, items g+h+j make the resulting expression at least as fast as today's builtin: cross-rev subtree dedup hits the cache, blobs are fetched lazily, the resulting CA storePath is keyed on subtree-SHA. See §4 "Per-builtin migration mapping" for the full table. **Distinct from nixpkgs-wide migration**, which is out of frame because it would change every existing nixpkgs derivation hash. |
| Tecnix's workdir/local-checkout state | 4 of 11 builtins (`SparseCheckoutRoots`, `DirtyZones`, `ZoneIsDirty`, `ZoneRoot`) + `DirtyOverlaySourceAccessor` | **Partially retire.** `SparseCheckoutRoots` reduces to a pure-Nix derivation from the manifest (no C++ change needed). The remaining 3 (`DirtyZones`, `ZoneIsDirty`, `ZoneRoot`) **do not retire under this proposal**. Master's `getAccessorFromWorkdir` exposes dirty workdir bytes through `${workdirInput}/sub` and tags the input fingerprint with `;d=<hash>`, but **dispatch is at Input granularity** (workdir input ⇔ no rev pinned, `git.cc:1090`). Tecnix's `DirtyOverlaySourceAccessor` selectively overlays per-file dirty bytes onto a *clean-at-rev* tree — a combination master cannot express. Retiring these 3 requires either (a) a UX shift where users drop `--tectonix-git-sha` while editing (clean-at-rev becomes dirty-no-rev) or (b) a follow-on RFC adding `OverlaySourceAccessor` with whiteout semantics (survey §8.3 #14, explicitly out of scope per §10). |
| Tecnix's manifest parse-caching | 2 of 11 builtins (`Manifest`, `ManifestInverted`) + `tectonixManifestJson` per-`EvalState` memo | **Retired.** Item **k** makes `builtins.fromJSON (builtins.readFile (worldRoot + "/.meta/manifest.json"))` content-keyed and persisted across processes; the per-`EvalState` `nlohmann::json` memo collapses to a single SQLite row keyed on `(file-fingerprint, "json")`. Inversion (`ManifestInverted`) is pure Nix on top. |
| Tecnix's git-sha exposure | 1 of 11 builtins (`GitSha`) | Out of frame — CLI configuration, not infrastructure. Read via existing setting-introspection. |

---

## 8. What the user writes

The user writes the same Nix they write today. No new builtins, no new flake-input attributes, no setting to opt into:

```nix
{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.bigrepo.url  = "git+https://example.com/monorepo.git?rev=abc123";

  outputs = { self, nixpkgs, bigrepo }:
    let
      pkgs = nixpkgs.legacyPackages.x86_64-linux;
      inherit (pkgs) lib;
    in {
      packages.x86_64-linux.default = pkgs.stdenv.mkDerivation {
        name = "my-pkg";
        src = lib.cleanSource ./.;
        # Pull just one subtree of a 5 GB monorepo into the build closure.
        # Wrapping in builtins.path is what routes through addPath and
        # exposes the subpath to item j's subtree-aware getFingerprint;
        # bare string interpolation of `${bigrepo}/...` routes through
        # ensureLazyPathsCopied with the parent's whole storePath instead
        # (see §4 — "What about tecnix's eleven builtins?").
        toolFoo = builtins.path { path = bigrepo + "/areas/tools/foo"; };
      };
    };
}
```

What happens at each layer, transparently:

- **Lazy clone of `bigrepo`**: when `git+https://example.com/...` resolves, our `PartialCloneTransport` (item **h**) negotiates protocol-v2 with `filter blob:none`. Server is GitHub/GitLab/Gitea — they support `uploadpack.allowFilter=true` by default. Initial fetch is commits + trees only — a fraction of the full pack, with the exact ratio depending on repo blob-to-tree weight.
- **Reading `bigrepo + "/areas/tools/foo"` via `builtins.path`**: when evaluation forces this path, `addPath` calls `fetchToStore`, which in turn calls `path.accessor->getFingerprint(path.path)`. The chain reaches `GitSourceAccessor::getFingerprint` with the subpath stripped to `/areas/tools/foo`; item **j**'s override returns `(CanonPath::root, "tree:<sha>")` for the subtree, and the `sourcePathToHash` cache row is keyed on the subtree-SHA — independent of parent rev. If any previous evaluation cached the same subtree-SHA (under any rev, or even via a tarball pipeline that produced the same tree-SHA), we hit without re-NARing. On a miss, blobs reachable from `/areas/tools/foo` are fetched on-demand by `PromisorBackend`; other subtrees are never touched.
- **`lib.cleanSource ./.`**: ends up calling `builtins.path { filter = ...; }`. The lambda's AST + captured environment hash to a stable identity (§5). First eval writes a `sourcePathToHash` row keyed on `(source-fp, filter-id, ...)`; second eval hits.
- **Concurrent reads of `bigrepo` from parallel evaluator threads**: contend only on the brief Phase 1 of `readBlob` (the State-lock-protected tree walk); the Phase 2 ODB read runs lock-free against the thread-safe `git_odb` (§6).

What does **not** happen transparently and remains user-visible:

- **First-eval narHash walk.** `mountInput` runs a dry-run to compute the input's narHash for the lock check. Today this walks the full tree; under item **h**, that walk forces all the tree blobs to be fetched on the first eval (because the hash is over the full content). To avoid this, `mountInput`'s dry-run needs to become a `Lazy<Hash>` (the relaxation [`paths.cc`'s `mountInput` comment](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/paths.cc) anticipates). The §4 staging — option 1 (lazy `narHash`) and option 2 (`LazyStorePathString`, the "big invasive change") — covers the chain of work needed; until at least option 1 lands, the first eval pays the full-tree NAR cost (though item **h** still saves disk + bytes for blobs the dryRun does not actually need, which is most of them under shallow + tree-only fetch).
- **String-interpolated subpaths of inputs.** `nativeBuildInputs = [ "${bigrepo}/areas/tools/foo" ]` (string concatenation in a build-input list, no `builtins.path` wrapper) routes through `EvalState::ensureLazyPathsCopied` with the parent input's whole storePath, not through `addPath`. Item **j**'s seam does not fire there; the parent input is materialised in full. Accelerating this pattern at the derivation level changes every existing nixpkgs derivation hash that uses such interpolation, and is deferred to the subtree-as-Input RFC (see §4).
- **Subtree-as-Input (Layer 1).** The lock-file still records the parent input's whole-repo narHash; the existing `dir` attribute provides a user-visible subpath pointer at the FlakeRef layer, glued onto `outPath` by `call-flake.nix`. The user gets the parent fetched lazily (item **h**) and per-subpath dedup at materialisation time (item **j** for `builtins.path`-wrapped accesses), all riding on the `dir`/Layer-2 mechanism. **Making the *subtree* itself the unit of lock-file identity** (subtree narHash, isolated subtree storepath, lock-file `version: 8` with `subPath` attribute on locked entries) is feasible — ~550 LOC of fetcher plumbing — but its own RFC; §4 sketches the design.

---

## 9. Implementation order and risk

Recommended order, lowest-risk first:

0. **`CURLOPT_POSTREDIR` prerequisite**. ~5 LOC in `src/libstore/filetransfer.cc` to set `CURLOPT_POSTREDIR = CURL_REDIR_POST_301 | CURL_REDIR_POST_302 | CURL_REDIR_POST_303 | CURL_REDIR_POST_307` for `HttpMethod::Post` requests. A latent bug independent of partial-clone (curl silently converts POST → GET on redirect today, dropping the request body); fixing it standalone is correct, and it's a prerequisite for item **h**. Trivial; do it first.
1. **g** (`Projection<Derived, From, To>` CRTP typeclass). Pure refactor, no behaviour change. Unblocks **a**, **b**, and **j**.
2. **e** (Phase 1 / Phase 2 split in `GitSourceAccessor::readBlob`, §6). Pure performance, no API change. Self-contained.
3. **f** (Opt-in CLI devirtualise). Small CLI change, can ship independently.
4. **a** + filter identity (§5). The biggest user-visible win (closes the fileset / `cleanSource` bypass). Adds `EvalState::computeFilterIdentity`, the free-vars walk on `ExprLambda`, and the `endPos: PosIdx` field. No public API change.
5. **b** (Cross-ODB tree-SHA projection — `treeHashToNarHash` reused on the git-mounted-subtree path). Builds on **g**.
6. **j** (Subtree-aware fingerprint in `GitSourceAccessor`, §4). One virtual override returning `(CanonPath::root, "tree:<sha>")` for tree-rooted subpaths and `(CanonPath::root, "blob:<sha>")` for file-rooted subpaths, with `;l` (LFS) and `;s` (submodules) flags carried from the leaf for both branches plus `;e=<gitattributes-hash>` from `GitExportIgnoreSourceAccessor::computeOwnSuffix` when `exportIgnore=true`. Reuses the existing `sourcePathToHash` cache namespace. Hooks `treeHashToNarHash` upsert/lookup in `fetchToStore2`. **Four load-bearing prerequisites ship together**: (a) the `addPath` refs-bypass fix (~10 LOC route the refs-bearing branch through `fetchToStore`); (b) reworking `FilteringSourceAccessor::getFingerprint` and `MountedSourceAccessor::getFingerprint` to "forward and append, falling back to the wrapper's `fingerprint` field if the inner has no content-keyed identity" — preserves master's behaviour for workdir inputs (wrapper has identity, inner doesn't) while letting item j's commit-rooted branch propagate up the wrapper stack (~30 LOC + `computeOwnSuffix` virtual + `mergeSuffix` shared logic); (c) setting `GitSourceAccessor::fingerprint` and `flagSuffix` at construction (~7 LOC) — `fingerprint = "git:R" + ;l? + ;s?`, `flagSuffix = ;l? + ;s?`; (d) the `;e=<hash>` memo on `GitExportIgnoreSourceAccessor` using a port of tecnix's `getGitAttributesAlongPath` (~30 LOC). ~102 LOC total. Closes cross-rev subtree dedup for `builtins.path` / `builtins.filterSource`. No public API change.
7. **d** (`LazyAttr` extension for `getLastModified` aggregation — narrowed scope per §6).
7a. **k** (Content-keyed parse-cache for `fromJSON` / `fromTOML` over `readFile` of content-addressed sources, §4.5). New per-machine `parse-cache-v1.sqlite` (separate from the eval cache); row-per-document custom binary blob format; per-`EvalState` `StringData *`-keyed side-table for the string-to-fingerprint association, hooked into `resetFileCache`; `prim_readFile` registers entries, `prim_fromJSON` / `prim_fromTOML` consume them. **No `NixStringContext` change, no `forceStringNoCtx` change.** ~630 LOC. Closes survey §7.8 (no parse-caching for pure expressions over input contents). Custom call shape (not the generic `Projection<>::lookup`) because Nix `Value` is not pass-by-value-friendly. Benefits from item **j**'s `blob:<sha>` branch (cross-process-cross-rev sharing requires file-rooted fingerprints to be content-keyed).
7b. **Compute-in-progress registry** (Projection<> enhancement, §2 "Concurrent-miss caveat"). `boost::concurrent_flat_map<std::string, std::shared_future<Attrs>>` field on `CacheImpl` (the concrete subclass — `Cache` itself is abstract); new `lookupOrCompute(key, compute)` method using `try_emplace_and_cvisit` for the atomic insert-or-find, `set_value` before `erase`, explicit try/catch (not RAII guard) for exception propagation; key-shape uses `attrsToJSON(key.second).dump()` to match the SQLite row-key form (since `Cache::Key`'s `Attrs` is a `std::map` with no `std::hash` specialisation). ~80 LOC. **Required for `eval-cores>1` to actually scale** on workloads with expensive compute (NAR walks, lambda hashing); without it, N threads racing on the same key pay N× compute cost. Caveat: `attrsToJSON` forces `LazyAttr` thunks; projection call sites must hand keys with already-forced primitives. Optional under `eval-cores=1`; gates whichever PR enables eval-cores parallelism for fingerprint-heavy workloads.
8. **c** (`FileSerialisationMethod::Git`). New code path in `Store::addToStore`. Builds on **b**.
9. Lazy `narHash` in `mountInput` (option 1 from §4). Builds on the master `LazyAttr` scaffolding (commits `ee78fe13e` + `a08c14bf8`). Single-attribute conversion analogue: commit `e9336fa3a` "make revCount lazy." `narHash` has more call sites than `revCount` (`Input::getNarHash`, `Input::to_string`, `Input::toAttrs`, JSON serialiser, `libflake/flake.cc` lock-write), so the conversion is correspondingly larger but the same shape. Required for first-eval savings under **h** + **j** when the user only reads a subtree.
10. **h** (`PartialCloneTransport` + `PromisorBackend` + `prefetchSubtree` virtual + `EvalState` triggers). The largest piece of new C++ in this proposal. Approximately 500 LOC for the v2 wire layer (reusing libgit2's existing pkt-line / sideband / packfile-indexer machinery — the v2-specific surface is small) plus ~230 LOC for the coalescing layer (mechanism + wrapper forwarding + `.gitattributes` pre-fetch + triggers, per §3.5). Inside libgit2 only, no system `git`. See §3.6 for the path-1-vs-path-2 escalation gate to gitoxide if the v2 budget exceeds estimate.
11. (Stretch) `LazyStorePathString` thunk (option 2 from §4). Rippling through `Value`-consumers (string interpolation, derivation hashing, JSON printing). Unlocks the full first-eval wire savings of **h** + **j** combined when nothing forces the parent input's `outPath` string. The "big invasive change" the existing `mountInput` comment names.

### Risks

- **Custom transport surface area.** Item **h** requires implementing protocol-v2's `command=fetch` over our own HTTPS framing. Pkt-line, sideband, and stateless-RPC have well-known shapes documented in `gitprotocol-v2.txt` upstream, but we still own the parser. Mitigation: integration tests against the major server implementations (GitHub, GitLab, Gitea, cgit, libgit2's own test fixtures) on every change; conformance tests against `git fetch` ground truth.
- **libgit2 ships native partial-clone.** Low likelihood (see §10), but if it lands before we retire our custom transport, the two could conflict. Mitigation: `git_transport_register` with our prefix wins (verified, `transport.c::transport_find_by_url`); explicitly unregister and fall back when libgit2 advertises native support. Lock files are still compatible (both produce the same narHash from the same content).
- **`Projection<>` typeclass migration.** Existing ten cache domains need to be ported. Mitigation: incremental — port one domain at a time, old code-path remains until last domain is ported.
- **Filter identity false misses.** Lambdas whose captures throw on force fall back to today's bypass; lambdas whose `with`-attrset-slot lookup fails (when the with-attrset is not forceable without further IFD) bail. Captured primops and primop-applications are hashed by name. End-to-end audit of `nixpkgs/lib/sources.nix` and `nixpkgs/lib/fileset/{default,internal}.nix` confirms the wrappers passed to `builtins.path` by `lib.cleanSource` and `lib.fileset.toSource` both hash cleanly under the source-region + capture-walk algorithm.
- **Lazy fetch + offline evaluation.** If a user goes offline mid-eval and a blob is missing, the eval fails with a network error. Mitigation: same as today's "rev not reachable" failure mode; surface a clear error.
- **Lazy `mountInput` dryRun is a real semantic change.** Skipping the eager narHash check until something forces it has knock-on effects on lock-file write timing (the deferred force then has to surface mismatches at lock-write time, where the user expects a quick local operation). Option 1 from §4 (lazy `narHash` only) is the smallest move and probably needs an RFC. Until it lands, item **h** is still bottlenecked by the dryRun walk on first eval — it saves blobs the evaluator never looks at, but the dryRun does look at every blob. Item **j**'s subtree-aware fingerprint at `addPath` partially compensates because parent inputs whose outPath is never read can become candidates for skipping the dryRun on a hot path.

---

## 10. What this proposal does *not* do

To stay honest about scope:

- **Does not contribute partial-clone to libgit2.** Item **h** is a Nix-side implementation; the principled long-term answer is upstreaming partial-clone to libgit2 ([#5564](https://github.com/libgit2/libgit2/issues/5564)). **Landing horizon: indefinite.** As of 2026-05, issue #5564 has been open since 2020-06 with no comment since 2022-05 and no maintainer commitment. The lone external champion publicly stopped pursuing upstream merge in May 2022 (continuing to use a downstream fork). The three prerequisite PRs referenced from that comment (#5993 `GIT_EMISSING`, #5996 promisor packfile detection, #5776 design RFC) are stalled-open or closed-without-merge since 2021-22. **libgit2 has no protocol-v2 implementation at all today** — verified by grep across the entire `libgit2/src/` tree for `ls-refs`, `command=fetch`, `protocol-v2` and similar terms (zero hits). Protocol-v2 is a hard prerequisite for `--filter=` negotiation. By analogy, libgit2's shallow-clone support took ~6 years from the first dedicated PR (#4331 "Shallow support", opened 2017-08-22, *never merged* — it remains open as a stale design discussion) to actual feature landing (PR #6396/#6557, merged 2023-05-09). The pattern — original PR sits open for years; eventually a fresh PR lineage merges the feature — is consistent with #5564's current trajectory. The realistic outcome is that this proposal's `PartialCloneTransport` is the production path for the lifetime of the feature. Sibling issues #6880 (the `partialclone` extension whitelist that this proposal works around via `GIT_OPT_SET_EXTENSIONS`) and #2263 (sparse-checkout, 12 years open) show the same pattern — community PRs sit unreviewed by maintainers for years. **The proposal commits to owning partial-clone support indefinitely.**
- **Does not introduce async IO.** The evaluator remains thread-per-task; we just remove the State-lock bottleneck. True async (`io_uring`, futures) would require an evaluator rewrite far beyond this proposal.
- **Does not change the daemon protocol.** Lazy fetches happen on the eval-side; the daemon still sees fully-materialised store paths.
- **Does not change the public Nix language API.** `builtins.path`, `builtins.fetchTree`, `builtins.filterSource`, and the flake-input attribute set are unchanged. No new syntax. No new arguments to existing builtins.
- **Does not depend on nixpkgs library code.** Filter identity (§4) is computed from the lambda value alone — `lib.cleanSource`, `lib.fileset.toSource`, and any other downstream user benefit transparently because they all eventually call `builtins.path { filter = ...; }` with a Nix lambda.
- **Does not change the unit of Input identity.** Layer 1 of subtree-as-Input — the *subtree* as the unit of lock-file identity, with subtree narHash and isolated subtree storepath — is feasible (the `mountInput` narHash gate is scope-symmetric and admits a subtree-rooted accessor producing a subtree-rooted narHash; substituters serve NARs by hash without opinion on whole-vs-subtree; the cost is ~550 LOC of fetcher plumbing plus a `version: 8` lockfile bump and a migration story for `${input.outPath}/sub` patterns, plus a derivation-hash transition for every existing nixpkgs derivation that uses such interpolation). It is its own RFC's worth of design choices, sized separately from this proposal. Layer 2 (the existing `dir` user-visible pointer with whole-repo narHash, accelerated by item j on the `builtins.path` code path) is what this proposal delivers. See §4.
- **Does not add a generic `OverlaySourceAccessor` combinator.** Master's `getAccessorFromWorkdir` (`AllowListSourceAccessor` over `makeFSSourceAccessor`) does expose dirty workdir content through `${input}/sub` with a `;d=<hash>` fingerprint suffix, *but only when the input is a workdir input* (no `rev` pinned — `git.cc:1090` dispatches based on `!input.getRef() && !input.getRev() && repoInfo.getPath()`). Tecnix's `DirtyOverlaySourceAccessor` selectively overlays per-file dirty bytes onto a clean-at-rev tree — a shape that *combines* rev pinning with dirty-file visibility, which master's Input-level dispatch cannot express. The `SparseCheckoutRoots` builtin reduces to a pure-Nix derivation from the manifest (no overlay needed), but **`DirtyZones`, `ZoneIsDirty`, and `ZoneRoot` cannot retire under this proposal** without either (a) a UX shift in tecnix where users *choose* between rev-pinned-clean and workdir-dirty-no-rev evaluation at the input declaration (i.e. drop `--tectonix-git-sha` while editing a zone) or (b) a follow-on RFC delivering `OverlaySourceAccessor` with whiteout semantics (survey §8.3 #14). The proposal commits to neither here.
- **Does not invent a new memoisation algebra.** The substrate is Build Systems à la Carte's `Task c k v` + `Store i k v` plus Adapton/Salsa/Skyframe's articulation-point-with-demand-driven-force model, restricted to content-addressed keys (so the dirty-propagation engine those systems carry is unnecessary). The proposal's `Projection<From, To>` is structurally a SkyFunction / Salsa query / Adapton articulation; the existing `SingleDerivedPath`/`Realisation`/`fetchers::Cache` triple is the build-sandbox version of the same shape. The contribution is the *Nix-side constraints* (frozen public API, no nixpkgs cooperation, no system git, no flake-input attributes) under which the substrate has to fit, not the substrate itself. See §0 for the BSàlC selective-task classification and the prior-art citations.
- **Does not claim Merkle-category or Merkle-lattice structure.** Direct literature search surfaces no published categorical formalisation of Merkle DAGs; the literature treats them operationally. The proposal adopts a pragmatic engineering stance.
- **Does not implement cross-machine cache merge.** The persistent fetcher-cache is structurally a bounded join-semilattice for content-keyed domains under content-determinism (§0). A `nix fetcher-cache merge` operation would: pin the cache schema version (currently `v4`); use `INSERT OR IGNORE` on the `(domain, key)` primary key for content-keyed rows; apply `max(timestamp)` and prefer rows with `immutableUrl` for `tarball`/`file` time-bound rows; report value mismatches with Nix-version / fetcher-policy context (since not all mismatches prove soundness-law violations — some are version skew or LFS-opt-in differences). Bazel RE's CAS sharing is the partial production precedent — but Bazel CAS keys are content hashes (digests are guaranteed unique-by-construction), whereas Nix fetcher-cache keys are *requests* (URLs, refs, treeShas), so conflict-freeness for Nix depends on the soundness law plus version alignment. Out of scope for this proposal; future RFC.
- **Does not unify the `tarball-cache-v2` and `gitv3` ODBs (item m).** The split between the shared tarball ODB and per-URL git ODBs is historical and quantifiable (~7% of nixpkgs disk worst-case duplication for users with both `github:NixOS/nixpkgs/<rev>` and a local-clone override). The unification is small (one `objects/info/alternates` file write per `gitv3/<hash>/` repo, pointing at `tarball-cache-v2/objects`); libgit2's alternates are read-only and consulted after main backends, so the 2018 fetch-time-performance regression that drove the per-URL split does not recur. Without item **h**, the dedup benefit is read-only (the per-URL fetch still receives a full pack; disk duplication unchanged). With item **h**'s `PromisorBackend`, the alternate becomes a real bandwidth win: `--filter=blob:none` clones backfill blobs only when neither the per-URL pack nor the `tarball-cache-v2` alternate has them. Item **m** is therefore staged as future work after item **h** ships. See §3's Scope paragraph for the historical context.

---

## 11. Summary

Four additions on top of the survey's six items, plus a concrete fleshing-out of survey item **a**:

- **g.** A single `Projection<Derived, From, To>` CRTP typeclass replacing the ten ad-hoc fetcher-cache domains. `domain()` is `static constexpr std::string_view` on the derived class — literal-storage backed, no virtual dispatch, no `string_view`-bound-to-temporary-`std::string` lifetime hazard. Pure refactor; no new layers, no new identity types.
- **h.** Lazy on-demand object fetch entirely inside libgit2: a custom HTTPS transport (registered via `git_transport_register`) that speaks protocol-v2's `filter blob:none`, plus a low-priority writable ODB backend (`PromisorBackend`) that materialises missing blobs on read, plus `GIT_OPT_SET_EXTENSIONS` to whitelist `partialclone` repos. Has to register at the `git_transport` layer rather than as a `git_smart_subtransport`, because libgit2's smart transport is hardwired to v0/v1 capabilities (no `fetch`, no `ls-refs`, no `filter`). Coalescing — the load-bearing piece for cold-eval latency — splits cleanly: a `SourceAccessor::prefetchSubtree(path, depth, filter)` virtual on the layer that understands paths, with wrapper accessors composing `isAllowed` predicates AND'd into the `filter` argument as the call descends, and triggers wired from `EvalState::evalFile` / `coerceToPath` / `prim_readDir` / `prim_readFile` on the layer that generates demand. Approximately 500 LOC for the v2 wire layer plus 230 LOC for coalescing, with an explicit escalation gate to `gitoxide` (Rust FFI) if the v2 budget exceeds estimate. **No system `git` invocation. No libgit2 source modifications.**
- **j.** Content-aware fingerprint in `GitSourceAccessor::getFingerprint`: tree-rooted subpaths return `(CanonPath::root, "tree:<sha>")`, file-rooted subpaths return `(CanonPath::root, "blob:<sha>")` — content-determined by the OID, independent of parent rev. Both branches receive `;l` (LFS) and `;s` (submodules) flags from the leaf and `;e=<hash>` from `GitExportIgnoreSourceAccessor::computeOwnSuffix` when `exportIgnore=true`. Reuses the existing `sourcePathToHash` cache namespace; no new domain. Cross-pipeline reuse via the existing `treeHashToNarHash` domain so a tarball and a git input that yield the same tree-SHA share a row. Ships with four structural prerequisites: (a) the `addPath` refs-bypass fix; (b) reworking `FilteringSourceAccessor`/`MountedSourceAccessor::getFingerprint` to "forward and append, falling back to the wrapper's field if the inner has no content-keyed identity" (preserves master's workdir-input semantics while letting item j's branch propagate); (c) setting `GitSourceAccessor::fingerprint` and `flagSuffix` at construction so commit-rooted accessors expose input-level identity at the leaf without colliding with wrapper-added `;e=H`; (d) the `;e=<hash>` memo on `GitExportIgnoreSourceAccessor` (port of tecnix's `getGitAttributesAlongPath`). Approximately 102 LOC total. Closes the survey's §6.1 cross-rev subtree dedup reproduction *and* makes item k cross-process-cross-rev. Covers `builtins.path` / `builtins.filterSource` only; bare-string-interpolation patterns are deferred to the subtree-as-Input RFC.
- **k.** Content-keyed parse-cache for `builtins.fromJSON` / `builtins.fromTOML` over `readFile` of content-addressed sources. New per-machine `parse-cache-v1.sqlite` (separate from the eval cache, whose per-fingerprint, row-per-attribute schema is structurally wrong for whole-document caching); row-per-document with a custom recursive binary blob format for the value tree (symbol-strings not symbol-IDs so the bytes survive cross-process). The string-to-fingerprint association lives in a per-`EvalState` side-table keyed on `const StringData *` (the only allocation-identity that survives `callFunction`'s `vRes = vCur` payload copy from `prim_readFile`'s stack-local result through to `prim_fromJSON`'s `args[0]`); `traceable_allocator` keeps keys reachable from the GC root set, and `EvalState::resetFileCache` clears the table on REPL `:reload`. **No `NixStringContext` change, no `forceStringNoCtx` change, no public API change** — the side-table is invisible to existing callers. Approximately 630 LOC (split: ~120 SQLite open+schema, ~80 lookup/upsert, ~250 value-tree serialisation, ~40 side-table wiring, ~30 readFile registration, ~80 fromJSON/fromTOML consumption, ~30 tests). Closes the survey's §7.8 "no parse-caching for pure expressions over input contents" gap.
- **§5 filter identity.** A stable identity for `builtins.path { filter = lambda; }` derived from the lambda's source-region content (`Pos::getSnippetUpTo` between `lambda.pos` and a new `lambda.endPos: PosIdx`, returning `nullopt` for non-file-backed origins like the REPL) plus a content-determined walk over its captured free variables. Adds `endPos: PosIdx` to `ExprLambda` (one field, established parser pattern via `ExprCall::cursedOrEndPos` at `nixexpr.hh:602`). Captured nested lambdas recurse via source-region (not heap-pointer pair), so the hash is byte-identical across cold processes. Free-var resolution uses `EvalState::lookupVar(env, var, /*noEval=*/true)` (`eval.cc:915`), with `var.fromWith != nullptr` triggering an explicit force of the with-attrset slot (master's `lookupVar(noEval=false)` already forces here, so the IFD-during-walk hazard is no worse than ordinary evaluation). Symbols are sorted by `std::string_view(state.symbols[s])`, never by the intern-order `Symbol::operator<=>`. Primops hash by `primOp->name`; externals bail; `with`-shadowing is handled by recursive `with`-attrset slot lookup. End-to-end audit of `nixpkgs/lib/sources.nix` and `nixpkgs/lib/fileset/*` confirms the `lib.cleanSource` and `lib.fileset.toSource` wrappers both hash cleanly. Approximately 300 LOC. Closes the survey's §6.6 / §7.12 fileset cache bypass without changing the public API.

Items **b**–**f** of the survey stand as-is, with §6 refining item **e** (Phase 1 / Phase 2 split in `readBlob` exploiting `git_odb` thread-safety) and item **d** (narrowed to per-attribute `LazyAttr`, since per-byte lazy strings would require a new Nix value kind).

What this proposal **does not** solve: (a) nixpkgs-wide migration off `${input}/sub` interpolation in `mkDerivation { src = ...; }` (§4 — separate RFC because the rebuild scope at the derivation level is a release-coordination event for nixpkgs, not a localised optimisation, and the bare-string-interpolation idiom routes through `ensureLazyPathsCopied` rather than `addPath`); (b) tecnix's three dirty-zone builtins (`DirtyZones`, `ZoneIsDirty`, `ZoneRoot`) — master's `getAccessorFromWorkdir` dispatches at Input granularity (workdir input ⇔ no rev pinned), so it cannot replicate tecnix's per-file dirty-overlay-on-clean-rev semantics. **8 of 11 tecnix builtins retire under this proposal** via items **g**+**h**+**j**+**k** plus a model shift where `worldRoot` is declared as a flake input — see §4 "Per-builtin migration mapping". The remaining 3 dirty-zone builtins persist until a follow-on RFC adds the `OverlaySourceAccessor` combinator (survey §8.3 #14, explicitly out of scope per §10) or tecnix accepts a UX split between clean-at-rev evaluation and dirty-workdir evaluation. Full first-eval wire savings under item **h** further depend on lazy `narHash` in `mountInput` (extending the existing `LazyAttr` scaffolding) and `LazyStorePathString` — both staged in §9.

The user writes idiomatic Nix. Public API is unchanged. Lazy clone, filter caching, parse-caching, and concurrent reads come for free on existing flakes that touch tree-shaped inputs (`git://`, `github:`, `gitlab:`, `sourcehut:`, and `tarball://`/`http(s)://*.tar.gz` — all of which expose a `GitSourceAccessor`; `tarball`/`github`/`gitlab`/`sourcehut` share the `tarball-cache-v2` ODB, while `git://` uses per-URL `gitv3/` repositories). Cross-rev subtree dedup comes for free on the `builtins.path` code path with item **j**. **8 of the 11 `__unsafeTectonixInternal*` builtins retire under items g+h+j+k, conditional on tecnix declaring `worldRoot` as a flake input** (so `getAccessorFromCommit` returns a `GitSourceAccessor` and item j's tree:/blob: branch fires; FS-path interpolation through `PosixSourceAccessor` does not benefit). The remaining 3 dirty-zone builtins (`DirtyZones`, `ZoneIsDirty`, `ZoneRoot`) persist until a follow-on RFC adds the `OverlaySourceAccessor` combinator — master's workdir accessor dispatches at Input granularity (workdir ⇔ no rev) and cannot express tecnix's per-file dirty-on-clean-rev semantics. Per-builtin mapping in §4. The realistic critical path to the headline first-eval wire savings is **(0) `CURLOPT_POSTREDIR` → g → e → f → a (filter id) → b → j → d → k → c → lazy-narHash → h**, with `LazyStorePathString` as a stretch goal for the full wire savings under combined lazy-fetch + lazy-mount. The total work is non-trivial — multiple RFC-shaped landings, anchored where possible against the historical commits cited above. Each stage ships independently; wins compound.
