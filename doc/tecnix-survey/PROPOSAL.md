# A Lazy, Composable, Transparent SourceAccessor

This document turns the survey in [README.md](./README.md) into an implementation proposal for upstream Nix. The goal is source materialisation that is lazy, content-keyed, and composable enough for monorepos, without adding builtin functions, depending on nixpkgs library code, invoking system `git`, or forking libgit2.

The proposal has one internal model: a source is a view with content identity and, where relevant, local provenance. Clean subtrees, arbitrary Git trees, filtered subsets, dirty overlays, sparse checkout roots, and physical checkout paths are represented by that same model. Existing public Nix expressions continue to use `builtins.path`, `builtins.fetchTree`, `builtins.filterSource`, and `readFile`/`fromJSON`.

The implementation tracks are: subtree/blob fingerprints, deferred `narHash` and virtual source mounts, a Nix-side protocol-v2 promisor provider, Git-shaped store registration, filtered-shape caching, content-keyed parse caching, and lock-free Git object reads. Each track is independently useful; together they replace tecnix's performance-oriented internal builtins and remove the repeated NAR walks that dominate cold evaluation.

---

## Contents

- [Review status](#review-status)
- [Framing: the workarounds encode the architecture](#framing-the-workarounds-encode-the-architecture)
- [0. The underlying pattern: content-addressed eval-time projection](#0-the-underlying-pattern-content-addressed-eval-time-projection)
  - [What the algebra is and what it is not](#what-the-algebra-is-and-what-it-is-not)
  - [Projection partitions](#projection-partitions)
  - [The cache itself is a join-semilattice (with caveats)](#the-cache-itself-is-a-join-semilattice-with-caveats)
  - [What the algebra is *not*](#what-the-algebra-is-not)
  - [Vocabulary](#vocabulary)
- [1. Libgit2 Constraints](#1-libgit2-constraints)
- [2. Projection Typeclass](#2-projection-typeclass)
  - [The refactor](#the-refactor)
- [3. Lazy Clone and Promisor Provider](#3-lazy-clone-and-promisor-provider)
  - [3.1 What libgit2 must not do: fetch from ODB callbacks](#31-what-libgit2-must-not-do-fetch-from-odb-callbacks)
  - [3.2 Protocol-v2 object provider over `FileTransfer`](#32-protocol-v2-object-provider-over-filetransfer)
  - [3.3 Repository opening and pack ingestion](#33-repository-opening-and-pack-ingestion)
  - [3.4 Split `GitSourceAccessor` lookup from object materialisation](#34-split-gitsourceaccessor-lookup-from-object-materialisation)
  - [3.5 Coalescing: prefetch on `SourceAccessor`, trigger in `EvalState`](#35-coalescing-prefetch-on-sourceaccessor-trigger-in-evalstate)
  - [3.6 Choice of v2 implementation](#36-choice-of-v2-implementation)
  - [3.7 Expected envelope (qualitative)](#37-expected-envelope-qualitative)
  - [3.8 The user sees nothing change](#38-the-user-sees-nothing-change)
- [4. Subtree Fetching](#4-subtree-fetching)
  - [What `${input}/sub` looks like to C++ (Layer 2 access pattern)](#what-inputsub-looks-like-to-c-layer-2-access-pattern)
  - [Why the parent walk is unavoidable at Layer 2](#why-the-parent-walk-is-unavoidable-at-layer-2)
  - [`getFingerprint` is the canonical content-identity surface](#getfingerprint-is-the-canonical-content-identity-surface)
    - [Suffix schema (versioned, documented)](#suffix-schema-versioned-documented)
    - [Soundness invariant](#soundness-invariant)
    - [Composition rule](#composition-rule)
  - [Subtree-Aware Fingerprints](#subtree-aware-fingerprints)
  - [Lazy Mounting and Deferred NAR Hash](#lazy-mounting-and-deferred-nar-hash)
  - [Source Views: Subsets, Overlays, Trees, and Checkout Capabilities](#source-views-subsets-overlays-trees-and-checkout-capabilities)
  - [What about tecnix's eleven builtins?](#what-about-tecnixs-eleven-builtins)
    - [Per-builtin migration mapping (tecnix-specific)](#per-builtin-migration-mapping-tecnix-specific)
    - [Nixpkgs-wide migration scope (out of frame)](#nixpkgs-wide-migration-scope-out-of-frame)
- [4.5 Content-Keyed Parse Cache](#45-content-keyed-parse-cache)
  - [Why a side-table, not a `NixStringContext` variant](#why-a-side-table-not-a-nixstringcontext-variant)
  - [Side-table design](#side-table-design)
  - [StringData identity preservation through `callFunction`](#stringdata-identity-preservation-through-callfunction)
  - [Address-reuse hazard and mitigation](#address-reuse-hazard-and-mitigation)
  - [Encoded-value format](#encoded-value-format)
  - [`parse-cache-v1.sqlite` is its own database, not a table in the eval cache](#parse-cache-v1sqlite-is-its-own-database-not-a-table-in-the-eval-cache)
  - [Why bytes-only caching is not enough](#why-bytes-only-caching-is-not-enough)
  - [Per-`readFile` cost](#per-readfile-cost)
  - [Cache invalidation](#cache-invalidation)
  - [What this gives users](#what-this-gives-users)
  - [Implementation size estimate](#implementation-size-estimate)
- [5. Filtered-Shape Cache](#5-filtered-shape-cache)
  - [Filtered-shape cache](#filtered-shape-cache)
  - [Implementation sketch](#implementation-sketch)
  - [Soundness](#soundness)
  - [What this gives users](#what-this-gives-users-1)
- [6. Non-Blocking Evaluation IO](#6-non-blocking-evaluation-io)
  - [What blocks today](#what-blocks-today)
  - [Split the State Lock from the ODB Read](#split-the-state-lock-from-the-odb-read)
  - [Lazy Attribute Scope, Not Lazy Bytes](#lazy-attribute-scope-not-lazy-bytes)
  - [What this gives users](#what-this-gives-users-2)
- [7. Unified Picture](#7-unified-picture)
  - [How composition works (and where it stops working)](#how-composition-works-and-where-it-stops-working)
  - [Tree-SHA to NAR-Hash Bridge](#tree-sha-to-nar-hash-bridge)
  - [Git-Shaped Store Registration](#git-shaped-store-registration)
  - [CLI Devirtualisation](#cli-devirtualisation)
  - [Implementation Track Map](#implementation-track-map)
  - [Concrete user-visible outcomes](#concrete-user-visible-outcomes)
- [8. What the user writes](#8-what-the-user-writes)
- [9. Implementation order and risk](#9-implementation-order-and-risk)
  - [Risks](#risks)
- [10. What this proposal does *not* do](#10-what-this-proposal-does-not-do)
- [11. Summary](#11-summary)

---

## Review status

This proposal is written for review under [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md). Design claims should be checked with the discipline's rules: simulate the implementation path, test two-process determinism, enumerate adjacent consumers, assess blast radius, account for latency, and compare out-of-frame alternatives. Source comments and documentation are useful only after the implementation path has been traced.

The scope is the named implementation tracks: [projection typeclass](#2-projection-typeclass), [lazy clone and promisor provider](#3-lazy-clone-and-promisor-provider), [subtree-aware fingerprints](#subtree-aware-fingerprints), [content-keyed parse cache](#45-content-keyed-parse-cache), [source views](#source-views-subsets-overlays-trees-and-checkout-capabilities), and [filtered-shape cache](#5-filtered-shape-cache). Subtree-as-Input, where the subtree itself is the lock-file identity with its own `narHash` and store path, remains a separate lock-file migration. This proposal accelerates existing source-path workflows and designs the internal model so the later lock-file migration does not require a rewrite.

---

## Framing: the workarounds encode the architecture

The survey's [§3.1](./README.md#31-eleven-new-builtins) tecnix table, [§6.6](./README.md#66-filtered-sources-bypass-the-fetcher-cache--confirmed) filtered-source reproduction, and [§5](./README.md#5-asymmetry-catalogue) `SourceAccessor` asymmetry catalogue point at the same missing abstraction: Nix lacks a reusable internal representation for source views and the content-keyed projections derived from them.

- `__unsafeTectonixInternalTreeSha` is a downstream computation of "the git tree-SHA of an already-mounted virtual input" — a projection that should be a first-class fetcher-cache domain alongside `treeHashToNarHash` and `gitRevToTreeHash`.
- `__unsafeTectonixInternalZoneSrc` mints a content-addressed store path keyed on tree-SHA via tecnix's own `tectonixZoneCache_` table (in tecnix's `EvalState`, an in-process `SharedSync<std::map<Hash, StorePath>>`). That cache belongs in shared projection infrastructure as a `Projection<TreeSha, StorePath>` instance, not as a private map.
- The fileset cache bypass at `fetch-to-store.cc` exists because a `PathFilter` changes the output tree while the accessor fingerprint still describes the unfiltered source. Downstream filter-using libraries therefore re-run the filter and re-walk/re-hash the accepted content on every cold eval.
- Tecnix's `worldRepo` setting and lazy-mount machinery exist because `mountInput`'s eager `narHash` walk does not match the workload — an evaluator that touches one subtree of a 5 GB monorepo pays for fetching, hashing, and walking the whole thing.

The replacement is shared C++ machinery:

- `tectonixZoneCache_` becomes content-aware accessor fingerprints plus shared fetcher-cache projections.
- `DirtyOverlaySourceAccessor`, arbitrary tree mounting, sparse checkout roots, and checkout-backed zone roots become `SourceViewAccessor` recipes with separate content identity and local provenance.
- Filtered source materialisation uses a post-filter shape cache instead of trying to identify the filter function.
- Lazy monorepo fetching uses a Nix-side protocol-v2 object provider plus deferred source materialisation, so mounting an input does not force a full-tree NAR walk.
- Manifest parsing uses a content-keyed parse cache, so `builtins.fromJSON (builtins.readFile manifestPath)` can share parsed values across processes.

The constraints are part of the design:

1. **Existing public APIs stay compatible; no new builtin functions.** `builtins.path`, `builtins.fetchTree`, `builtins.filterSource`, and existing flake-input attributes keep their current meaning. `SourceViewAccessor` is an internal C++ abstraction, not a public evaluator function. Functionality that lacks an upstream spelling today — arbitrary tree handles and checkout provenance queries — is modelled internally so the implementation generalises, but public exposure must reuse existing surfaces or be a separate no-new-builtin API decision.
2. **No dependency on nixpkgs.** Nixpkgs `lib.fileset`, `lib.cleanSource`, `lib.sources` etc. are downstream library code; Nix cannot condition behaviour on them.
3. **No shelling out to system `git`.** Git object storage and pack ingestion stay on libgit2's public APIs; protocol-v2 fetching is either Nix-owned over `FileTransfer` or, if it lands upstream, native libgit2 support. No vendored libgit2 fork.
4. **No one-off flake-input attributes that change Input identity implicitly.** `dir` already exists at the `FlakeRef` layer as a subpath pointer with whole-repo `narHash`; `call-flake.nix` appends it to `outPath`. A future change to the lock-file identity unit must be explicit, versioned, and scoped to a subtree-as-Input design.

The survey's [§8.0](./README.md#80-the-natural-conclusion-finish-the-lazy-filesystem) identifies gaps in in-process caching, persistent caching, predicate composition, and devirtualisation. This proposal turns that work into implementation tracks:

- **[Projection typeclass](#2-projection-typeclass).** A CRTP `Projection<Derived, From, To>` refactors the cache domains that share a simple lookup-or-compute shape. It centralises domain names, key encoding, value encoding, and soundness checks. Domains with TTL, retry, or store-GC semantics keep specialised wrappers.
- **[Lazy clone and promisor provider](#3-lazy-clone-and-promisor-provider).** Lazy, on-demand object fetch via a Nix-owned protocol-v2 `upload-pack` client over `FileTransfer`, using libgit2's public ODB/indexer APIs only outside ODB callbacks. `GitSourceAccessor` first resolves a path to object IDs, then a `GitPromisorProvider::ensureObjects`-style layer fetches missing objects with `filter blob:none` and coalesced wants before libgit2 is asked to read them. The old "fetching ODB backend" seam is rejected: libgit2 invokes `read` / `read_header` / `exists` / `refresh` callbacks while holding the ODB mutex, so network fetch and pack ingestion there can re-enter libgit2 under its own lock ([`odb.c#L1094-L1106`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L1094-L1106), [`#L989-L1005`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L989-L1005), [`#L687-L700`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L687-L700), [`#L1693-L1702`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L1693-L1702)).
- **[Subtree-aware fingerprints](#subtree-aware-fingerprints).** Content-aware fingerprints in `GitSourceAccessor::getFingerprint`: for tree-rooted subpaths return `(CanonPath::root, "tree:<sha>...")`, for file-rooted subpaths return `(CanonPath::root, "blob:<sha>;m=<git-mode>...")`. The mode/type suffix is required because Git blob OIDs do not encode executable bit or symlink-vs-regular mode, but NAR serialisation does. LFS and export-ignore identity share an attribute-context digest; plain `;l` is sound only on rev-keyed fingerprints because smudge decisions are `.gitattributes`-dependent. Cross-rev dedup hits the existing `sourcePathToHash` cache for subtrees and individual files; cross-pipeline reuse through `treeHashToNarHash` needs an explicit bridge in `fetchToStore2`, because that domain is not consulted automatically.
- **[Content-keyed parse cache](#45-content-keyed-parse-cache).** Content-keyed parse-cache for `builtins.fromJSON` over context-free, non-empty `readFile` results from content-addressed sources: a `Projection<(content-fingerprint, format, parser-key), serialised-Value>`-shaped cache persisted in a new per-machine `parse-cache-v1.sqlite` (separate from the per-fingerprint eval cache, whose row-per-attribute schema is unsuited to whole-document caching). The string-to-fingerprint association lives in a per-`EvalState` side-table keyed on `StringData *` — *not* in `NixStringContext`, because parse-cache hints are not derivation-hashing references and an extra context variant would force surgery across every `forceStringNoCtx` caller. `fromTOML` is a second stage unless the key includes parser feature state such as `Xp::ParseTomlTimestamps`.
- **[Source views](#source-views-subsets-overlays-trees-and-checkout-capabilities).** `SourceViewAccessor` and `ViewRecipe`, the internal abstraction for subtrees, arbitrary tree handles, post-filter subsets, dirty overlays, sparse checkout provenance, and physical checkout roots. Content-addressed identities persist only when the recipe is content-determined; local checkout facts are capabilities/provenance and never masquerade as globally-shareable cache rows. This is the piece that prevents a clean-tree-only implementation from baking in a model that cannot later support tecnix's dirty/sparse/local-checkout semantics. It does **not** add a builtin.
- **[Filtered-shape cache](#filtered-shape-cache).** A cache key for `builtins.path { filter = ...; }` based on the source fingerprint plus the observed filtered output shape. It still runs the filter, but on a hit it skips the later content NAR/hash/copy work when the reconstructed store path is valid or substitutable; it safely shares rows between different filters only when their current run accepts the same canonical NAR shape.

Subtree-as-Input has two meanings. The existing `dir` attribute is a subtree pointer at the `FlakeRef` layer; the locked `Input` still has a whole-repo `narHash`. This proposal accelerates that existing pattern through subtree-aware fingerprints and lazy materialisation. Making the subtree itself the lock-file identity is feasible, but it requires a lock-file migration and a derivation-hash transition for existing `${input}/sub` string-interpolation users. [§4](#4-subtree-fetching) separates those two concerns.

The proposal is grounded in three concrete codebases: upstream Nix at [NixOS/nix@b66763439](https://github.com/NixOS/nix/tree/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4), tecnix at [Shopify/tecnix@45d9c6f4](https://github.com/Shopify/tecnix/tree/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f), and libgit2 at [libgit2/libgit2@e490b18b7](https://github.com/libgit2/libgit2/tree/e490b18b70e3d9d870d4affeb177218d684c47e3) (cloned locally). Every dependency on libgit2 has been verified against the actual API.

---

## 0. The underlying pattern: content-addressed eval-time projection

[Projection typeclass](#2-projection-typeclass), [subtree-aware fingerprints](#subtree-aware-fingerprints), [content-keyed parse cache](#45-content-keyed-parse-cache), the content-identity side of [source views](#source-views-subsets-overlays-trees-and-checkout-capabilities), and the [§5](#5-filtered-shape-cache) filtered-source cache share a single shape. Naming it makes the rest of the proposal read as instances of one structure rather than parallel inventions.

**The projection algebra.** Use "algebra" here in the small engineering sense: a typed family of partial, deterministic, content-keyed functions plus substitution and cache-merge laws. A projection is a morphism

`P : I₁ × ... × Iₙ ⇀ O`

with:

- `keyP(i₁, ..., iₙ)`: canonical, versioned, fingerprintable bytes for the content-determining inputs.
- `computeP(i₁, ..., iₙ)`: deterministic when it returns.
- `encodeO(o)`: the content-addressed identity or serialised value stored for the output.
- cache row: `(domain(P), keyP(...)) → encodeO(computeP(...))`.

The laws are the useful part:

1. **Soundness**: equal projection keys imply equal encoded results. If two clients disagree on `(domain, key)`, at least one of Nix version, fetcher policy, serialisation policy, or the implementation is outside the domain's law.
2. **Substitution / composition**: if `P : A → B` and `Q : B × C → D`, then a cached `P(a) = b` can be substituted into `Q` without remembering how `b` was obtained. Cache sharing is by the content key/value, not by the path taken through previous cache rows.
3. **Product / parallelism**: independent projections over the same source commute because their dependencies are statically named by the input tuple, not discovered by a monadic bind.
4. **Cache join**: for content-keyed domains, cache merge is set union on `(domain, key, value)` rows, provided equal keys have equal values. Time-bound domains are excluded or merged with explicit domain policy.

Concrete instances:

```text
GitTreeToNarHash:
  (treeOid) -> narHash

SourcePathToHash:
  (sourceFingerprint, ContentAddressMethod, subpath) -> contentHash

FilteredSource:
  (sourceFingerprint, observedAcceptedPathSetHash, method) -> contentHash

ParseJSON:
  (blobFingerprint, "json", parserVersion) -> serialised Value

SourceViewSubset:
  (baseTreeOid, acceptedPathTrie, modePolicy) -> syntheticTreeOid
```

If two revisions of a monorepo contain the same `/sub` tree-OID, `GitTreeToNarHash(subTreeOid)` is the same row for both revisions. If the same content hash is later used under different store path names, the cache row still shares: master already keys `sourcePathToHash` by `(fingerprint, method, path)` and stores only the hash, then derives the caller-named store path from that hash with `makeFixedOutputPathFromCA` on lookup ([`fetch-to-store.cc#L8-L13`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/fetch-to-store.cc#L8-L13), [`#L44-L50`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/fetch-to-store.cc#L44-L50)). If a filter accepts the same set of paths for the same source root, `FilteredSource` is the same row even when the predicate implementation that produced the set differs; the cache key is the measured output shape, not the filter value.

**Build-side analogues: `CAFixed` plus realisations, not a floating-CA claim.** The closest build-side analogue for input-keyed eval projections is fixed-output CA: `DerivationOutput::CAFixed` carries the expected content address, and its output path is computed from that address via `makeFixedOutputPathFromCA` ([`derivations.hh#L39-L58`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/include/nix/store/derivations.hh#L39-L58), [`derivations.cc#L20-L40`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/derivations.cc#L20-L40)). The proposal's projections are the eval-time equivalent of "the address is determined by the declared inputs and method"; the work may be lazy, but the result is not free to vary.

`CAFloating` is different. It records only method + hash algorithm, has no output path before the build (`DerivationOutput::path` returns `std::nullopt`), computes the content address from the produced output, then registers a `Realisation` mapping `DrvOutput{drvPath, outputName}` to the realised path ([`derivations.hh#L64-L82`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/include/nix/store/derivations.hh#L64-L82), [`derivation-builder.cc#L1725-L1748`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/unix/build/derivation-builder.cc#L1725-L1748), [`#L2035-L2038`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/unix/build/derivation-builder.cc#L2035-L2038)). That is an output-realisation mechanism, not the main projection algebra in this proposal.

**What floating CA at the evaluation layer would look like.** A true eval-layer floating CA design would introduce a stable handle before the evaluator knows the content address, e.g. `EvalOutputId{inputIdentity, recipeId, outputName}`. Values, string contexts, or virtual store mounts could carry that handle. At a force boundary (`toString`, `derivationStrict`, lock write, explicit `narHash`, store copy), Nix would compute the bytes/tree/NAR for the recipe, derive the content address, write or substitute the result, and register `EvalOutputId → {narHash, storePath, metadata}` in a realisation table. Consumers would resolve the handle later, analogous to build downstream placeholders resolving through realisations.

Deferred NAR hashing is the narrow version of that idea for source materialisation: `mountInput` can hand out a virtual/lazy path and postpone computing `narHash` and the final CA store path until a NAR-required boundary. It is "floating-like" in representation because the evaluator carries an unresolved handle, but semantically it is still usually a lazy fixed projection for locked Git inputs: the tree and serialisation method determine the eventual NAR hash. Dirty checkouts, local provenance, and post-filter subsets are the cases where the eval-output handle model matters more, because the final content address is only known after reading the local state or running the filter.

**Build-side memoisation parallels.** `hashDerivationModulo` / `pathDerivationModulo` are the closest build-side precedent for replacing provenance with content identity. For fixed-output CA dependencies, consumers key on the dependency output's content hash instead of the upstream derivation path. The in-process `DrvHashes` table is the build-side memo for that computation; tecnix's `tectonixZoneCache_` has the same shape at evaluation time, but without shared infrastructure.

The analogy has a boundary. `Realisation` rows can be signed and transferred across stores; projection rows are local cache entries. A projection cache merge must therefore rely on the projection soundness law and on Nix-version / fetcher-policy compatibility. There is no signature fallback for a disagreeing projection row.

`DerivationOutput::Impure` is the build-side analogue of the proposal's time-bound domains: `Derivation::shouldResolve` returns true and `queryRealisation` is not used. Fetcher domains such as moving refs and TTL-gated tarballs have the same property at evaluation time and remain outside the content-keyed projection subset.

### What the algebra is and what it is not

It is a memoisation discipline with a soundness law. The [projection typeclass](#2-projection-typeclass) names the common implementation shape; [subtree-aware fingerprints](#subtree-aware-fingerprints), [content-keyed parse cache](#45-content-keyed-parse-cache), and [filtered-shape cache](#5-filtered-shape-cache) are concrete instances.

In Build Systems à la Carte terms, the projection layer is the static-dependency part of `Task` ([Mokhov, Mitchell & Peyton Jones, ICFP 2018](https://arxiv.org/abs/1803.10527)). The evaluator remains monadic because Nix expressions choose dependencies at runtime. The projection layer sits below that: each projection names its inputs in the key before `compute` runs. That gives two implementation properties: the touched cache rows are knowable from the key type, and independent projections can run without monadic re-entrancy.

The same structure appears in Salsa, Skyframe, and Adapton. Nix's content-addressed keys remove the dirty-propagation part those systems need: when content changes, the key changes. The old row is not invalidated; it simply stops being queried.

Several projections are n-ary. The C++ spelling can start with `From = struct { ... }`; a variadic `Projection<Output, Inputs...>` removes boilerplate once enough domains need it. This is an encoding choice, not a separate theory.

### Projection partitions

The soundness law separates the fetcher cache into orthogonal concerns. Cross-tree verification against master, DetSys's `nix-src`, and tecnix yields the same partition in all three trees.

**Concern 1 — `Store`-aware**: the cached value carries a `StorePath` whose validity is checked against the live store through `Cache::lookupStorePath`. This affects `hgRev`, `file`, and tecnix's `gitRevUrl`.

**Concern 2 — `Timed` / non-content-keyed**: the producer reads wall-clock time or live remote state, so the same key can legitimately produce a different value later. This affects `tarball`, `file`, and `hgRefToRev`.

**Concern 3 — `Workdir`**: master's `workdirInfoCache_` caches `git_status` results whose answer changes on local edits and index changes. The cache is per-evaluation and is cleared on evaluator reset; it is not persisted to SQLite.

**Concern 4 — per-evaluation session identity**: tecnix's `tectonixCheckoutZoneCache_` maps a zone path to a random store path minted for one `EvalState`. That is a session cache, not a content cache. This proposal replaces the content side with source views; random session paths remain local implementation detail.

The resulting implementation split is:

- `Projection<O, Is...>` is the base (six domains: `treeHashToNarHash`, `sourcePathToHash`, `gitRevCount`, `gitRevToTreeHash`, `gitRevToLastModified`, `gitLastModified`).
- `Store` concern adds a `StorePath` to the value with GC-validity gating (one domain: `hgRev`; plus tecnix's `gitRevUrl`).
- `Timed` concern adds TTL/retry semantics that violate `Task Applicative` (one domain: `hgRefToRev`).
- `Store + Timed` is `file` and `tarball` — the value carries `etag`/`lastModified` *and* the row has TTL+retry. **`file` is in this intersection, not pure `Store`.** Two clients hitting the same URL at different times will record different `etag`s even for byte-identical content, so two-cache merge requires a domain-specific resolution policy (prefer rows with `immutableUrl` set, then max(timestamp)).

DetSys and tecnix add producers and domains, but they do not move existing domains between these categories. That makes the partition a property of the domain semantics rather than a local implementation accident.

### The cache itself is a join-semilattice (with caveats)

The persistent fetcher cache can be treated as a `Set<(domain, key, value)>` for content-keyed domains. The SQLite schema in master, DetSys, and tecnix is the same: `Cache(domain TEXT, key TEXT, value TEXT, timestamp INTEGER)` with primary key `(domain, key)`. For content-keyed domains, the soundness law makes `INSERT OR IGNORE` merge idempotent, commutative, and associative.

Three caveats define the safe merge boundary:

1. **The in-tree upsert uses `INSERT OR REPLACE`, not `INSERT OR IGNORE`.** A merge tool must use `INSERT OR IGNORE` explicitly.

2. **`tarball` and `file` carry time-bound metadata.** Their rows need a domain policy such as `immutableUrl` preference and `max(timestamp)`.

3. **Equal-key mismatches need version and policy context.** They can indicate a soundness bug, Nix-version skew, fetcher-policy differences, or serialisation differences.

Bazel RE CAS is a stronger structure because its keys are digests of the value bytes. Nix fetcher-cache keys are requests, so conflict-freeness depends on the soundness law plus version and policy alignment. A future cache-merge command can still exist, but it must pin schema version, use `INSERT OR IGNORE` only for content-keyed rows, apply explicit policy for time-bound rows, and reject cross-version merges.

### What the algebra is *not*

This proposal does not claim a lattice over source trees, a bidirectional lens, or a new categorical structure. The only lattice claim used operationally is the cache-row merge property for content-keyed rows. The projection API is read-only: it derives identities and store paths from sources; it does not splice derived values back into parent sources.

The engineering pattern is also not novel. Prior art includes:

- **Bazel's Remote Execution v2 protocol** ([`build/bazel/remote/execution/v2`](https://github.com/bazelbuild/remote-apis/blob/main/build/bazel/remote/execution/v2/remote_execution.proto)) defines `Directory`/`FileNode`/`DirectoryNode` Merkle messages where each `DirectoryNode` references a child by `(name, digest)` independently of position. Identical subtrees deduplicate automatically across mount points and across actions — the [subtree-aware fingerprint](#subtree-aware-fingerprints) cross-rev-subtree-dedup property, in production since 2018. Bazel's "Build without the Bytes" mode (default since Bazel 7) is the same materialisation strategy as [lazy clone](#3-lazy-clone-and-promisor-provider): fetch bytes lazily by content identity.
- **Git's `--filter` family** ([`git-rev-list(1)`](https://git-scm.com/docs/git-rev-list)) is itself a documented Merkle-projection algebra. `--filter=blob:none + tree:0 + sparse:oid=X` composes by intersection — the *meet* of the filter predicates. The proposal's [lazy clone](#3-lazy-clone-and-promisor-provider) path uses only `blob:none`; richer composition (`combine:` form) is plausible future work. Git's deprecated `sparse:path=` was removed for security reasons (arbitrary filesystem-path filters); the proposal's filtered-shape cache avoids this anti-pattern by measuring the actual accepted output shape for the current source.

The contribution is applying this pattern inside Nix evaluation while preserving existing public APIs, avoiding nixpkgs cooperation, avoiding system `git`, and keeping local checkout facts separate from globally shareable content identity.

### Vocabulary

- **Content-addressed projection**: a deterministic, content-keyed computation whose cached value can be reused across call sites and processes when the key matches.
- **Source view**: an internal `SourceAccessor` view with a recipe, content identity, and optional local provenance.
- **Provenance**: local facts such as physical checkout path, sparse checkout roots, and workdir delta. Provenance may guide evaluation but is not a substituter key.

---

## 1. Libgit2 Constraints

These constraints determine the lazy-clone design.

**Supported, used directly:**

- **Custom ODB backends exist, but not as the lazy-fetch seam.** libgit2 orders ODB backends via [`git_odb_add_backend`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/odb.h#L583), and alternates via [`git_odb_add_alternate`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/odb.h#L604). However, `git_odb` calls backend `exists`, `read_header`, `read`, `writepack`, and `refresh` while holding the ODB mutex ([`odb.c#L687-L700`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L687-L700), [`#L989-L1005`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L989-L1005), [`#L1094-L1106`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L1094-L1106), [`#L1610-L1613`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L1610-L1613), [`#L1693-L1702`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L1693-L1702)). A backend that does network I/O, pack ingestion, `git_odb_write_pack`, or `git_odb_refresh` from those callbacks risks re-entering the same lock. ODB backends remain useful for passive storage composition; they are not where missing-object fetch belongs.
- **Per-remote transport callbacks, not global scheme takeover.** libgit2 supports global transports via [`git_transport_register`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/sys/transport.h#L201), but custom transports are selected before built-ins and `git_transport_new` does not fall back after a selected factory fails ([`transport.c#L51-L70`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/transport.c#L51-L70), [`#L121-L132`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/transport.c#L121-L132)). Per-remote callbacks are safer: `remote.c` asks the caller-supplied transport factory first and then falls back to global registrations only if no transport was produced ([`remote.c#L955-L964`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/remote.c#L955-L964)). The primary [lazy clone](#3-lazy-clone-and-promisor-provider) path below avoids global scheme registration entirely: a Nix-owned upload-pack client speaks protocol-v2 over `FileTransfer`, receives packs, indexes them, refreshes ODB state, and then lets ordinary libgit2 object lookup proceed.
- **Whitelisting unknown extensions** via [`GIT_OPT_SET_EXTENSIONS`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/common.h#L522). Adding `"partialclone"` to the user-extensions vector ([`repository.c#L1925-L1944`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/repository.c#L1925-L1944)) makes `git_repository_open` accept partial-clone repos that today error out per [libgit2#6880](https://github.com/libgit2/libgit2/issues/6880).
- **Thread-safe ODB** — `git_odb` uses internal locking and is safe to share across threads ([`docs/threading.md#L24-L25`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/docs/threading.md#L24-L25)). Most other libgit2 objects (`git_repository`, `git_tree`, working trees) are *not* thread-safe and must be accessed from one thread at a time ([`#L4-L18`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/docs/threading.md#L4-L18)). References and configuration snapshots are immutable and shareable.
- **Shallow clone** with `git_fetch_options.depth` ([`remote.h#L820-L827`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/remote.h#L820-L827)).
- **Bare clone, no checkout** via `git_clone_options.bare = 1` and `checkout_strategy = GIT_CHECKOUT_NONE` ([`clone.h#L110-L171`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/clone.h#L110-L171)).
- **Pathspec matching** — `git_pathspec_new`, `git_pathspec_matches_path`, `git_pathspec_match_tree` ([`pathspec.h`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/pathspec.h)).
- **Tree construction** — `git_treebuilder_new`, `git_treebuilder_filter`, `git_treebuilder_write` ([`tree.h`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/tree.h)).
- **Indexer** — `git_indexer_new` / `git_indexer_append` / `git_indexer_commit` ([`indexer.h`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/indexer.h)). Streams a packfile (received from any source) into the ODB.
- **Subtree access** — `git_tree_entry_bypath`, `git_tree_walk`.
- **Smart-protocol caps libgit2 sends in `want` lines** (via `buffer_want_with_caps` in [`smart_pkt.c`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/transports/smart_pkt.c)): `multi_ack[_detailed]`, `side_band[_64k]`, `include_tag`, `thin_pack`, `ofs_delta`, `shallow`. Note that `want_tip_sha1` / `want_reachable_sha1` are *server-advertised* capabilities libgit2 *detects* (via `git_smart__detect_caps` in [`smart_protocol.c`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/transports/smart_protocol.c)) — never client-sent.

**NOT supported in libgit2:**

- **Native partial-clone fetch.** Issue [libgit2#5564](https://github.com/libgit2/libgit2/issues/5564) open since 2020. The built-in smart transport's want-list serialiser [`git_pkt_buffer_wants`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/transports/smart_pkt.c#L762-L843) emits `want <oid>` lines, `shallow` lines, and `deepen` lines — but never `filter`. The function is not `GIT_EXTERN`-exported; `smart_pkt.c` has zero `GIT_EXTERN` symbols, so Nix cannot reuse it from outside libgit2.
- **Wrapping the smart subtransport to inject `filter`.** The subtransport interface ([`sys/transport.h#L322-L438`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/sys/transport.h#L322-L438)) is byte-stream level — `read`/`write` of opaque bytes for `UPLOADPACK_LS` / `UPLOADPACK` / etc. The doc-comment for `git_smart_subtransport_definition` is explicit: "The smart transport knows how to speak the git protocol… For this, a subtransport interface is declared, and the smart transport delegates this work to the subtransports." Capability negotiation happens *above* the subtransport. By the time `write()` runs, `git_pkt_buffer_wants` has already serialised wants without `filter`.
- **Sparse checkout / sparse-index.** Issue [libgit2#2263](https://github.com/libgit2/libgit2/issues/2263) open.

**The path forward.** We compose libgit2's ODB/indexer/repository APIs with a Nix-owned protocol-v2 object provider:

1. Implement a small protocol-v2 upload-pack client over Nix's existing `FileTransfer` HTTPS layer. Nix cannot reuse libgit2's smart HTTP subtransport as a protocol shim because capability negotiation sits above the subtransport; by that point libgit2 has already serialised a v0/v1 fetch without `filter`. The request includes `filter blob:none`, matching Git's documented partial-clone protocol (`filter` is a fetch request argument in [protocol-v2](https://git-scm.com/docs/protocol-v2), and `blob:none` is the standard blob-omission filter in [partial-clone](https://git-scm.com/docs/partial-clone.html)).
2. Stream the resulting packfile into the ODB via `git_indexer_*`, outside any ODB backend callback. Keep connectivity verification compatible with promisor packs: `git_indexer_options.verify` performs full connectivity checks ([`indexer.h#L98-L100`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/indexer.h#L98-L100)), and missing expected objects fail indexer commit ([`indexer.c#L1310-L1313`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/indexer.c#L1310-L1313)).
3. Whitelist `partialclone` via `GIT_OPT_SET_EXTENSIONS` so libgit2 opens the resulting repo.
4. In `GitSourceAccessor`, resolve paths to object IDs under its existing repository lock, release the lock, call `ensureObjects` to fetch any missing objects, then retry the normal libgit2 lookup. That keeps network and pack writes outside both Nix's `GitSourceAccessor::state_` lock and libgit2's ODB mutex.

No system `git` invocation. No libgit2 source modifications. The seam is a Nix-side object provider plus libgit2's public ODB/indexer APIs. Long-term, the principled fix is to upstream partial-clone to libgit2 ([#5564](https://github.com/libgit2/libgit2/issues/5564)); when that lands, the provider can retire.

---

## 2. Projection Typeclass

The survey's [§2.1](./README.md#21-three-layers) identifies `SourceAccessor`, `storeFS`, the real store, and the fetcher cache as separate concerns. The existing C++ types already represent those layers. The missing piece is a common implementation shape for the cross-layer mappings that currently live as separate cache domains.

### The refactor

The fetcher cache today has ten domains, each with its own attr-set encoding (survey [§7.3](./README.md#73-content-addressed-identity-is-over-pluralised) footnote). The projection typeclass abstracts the shared lookup-or-compute shape. It is not a new runtime layer.

Use CRTP. Each projection is a stateless function of `fetchers::Settings`; the cache-row domain identifier is a static `constexpr std::string_view` on the derived class. That binds the string storage at compile time and avoids virtual dispatch at statically-known call sites.

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

The compute closure carries the accessor, repository, and settings context. The template enforces the lookup-or-upsert discipline and the cache-key shape; it does not constrain what `compute` reads.

The existing ten domains (`treeHashToNarHash`, `sourcePathToHash`, `gitRevCount`, `gitRevToTreeHash`, `gitRevToLastModified`, `gitLastModified`, `hgRev`, `hgRefToRev`, `tarball`, `file`) become concrete instantiations where their semantics fit. New projections needed by [filtered-shape caching](#filtered-shape-cache), [lazy clone](#3-lazy-clone-and-promisor-provider), [subtree-aware fingerprints](#subtree-aware-fingerprints), and [content-keyed parse cache](#45-content-keyed-parse-cache) use the same substrate. Tecnix's `tectonixZoneCache_` and `tectonixManifestJson` become persistent projection/cache entries instead of per-`EvalState` special cases.

**Shape-fit caveat.** Of the ten existing domains, six (`treeHashToNarHash`, `sourcePathToHash`, `gitRevCount`, `gitRevToTreeHash`, `gitRevToLastModified`, `gitLastModified`) fit the simple `lookup-or-compute` pattern cleanly. Two (`hgRev`, `file`) require the store-aware variant `lookupStorePath` because their values reference a `StorePath` that the GC must track; introduce a sibling `StoreProjection<Derived, From, To>` for those rather than retrofitting Store-awareness into the base. One (`tarball`) needs `lookupExpired`-style retry-on-failure semantics that does not fit either base; leave its call site bespoke. One (`hgRefToRev`) uses `lookupWithTTL`. The typeclass is therefore a refactor of most of the cache machinery, not all of it.

**Concurrent-miss caveat.** N threads calling `lookup(...)` with the same `from` can all miss, run `compute()`, and upsert. The SQLite layer uses `INSERT OR REPLACE`; for content-determined results this is safe, but it is wasteful under `eval-cores=N`. Expensive computes such as NAR walks and filtered-shape collection need an in-memory compute-in-progress registry to avoid duplicate work.

The registry has two implementation constraints:

- **`Cache` is abstract** (`fetchers/cache.hh` declares it with pure-virtual `lookup`/`upsert`/etc.), so the in-flight map and the `lookupOrCompute` helper live on the concrete `CacheImpl` (declared in `cache.cc`); subclasses inherit through the `Cache` interface.
- **`Cache::Key = pair<string_view, Attrs>`** and `Attrs = map<string, Attr>` is *not* `std::hash`-able (no `std::hash<map<...>>` specialisation; `Attr` is a `std::variant<string, uint64_t, Explicit<bool>, LazyAttr>` which would also need a custom hash). Using `Key` directly as the concurrent-map key requires writing a custom hasher. The simpler approach is to key on the same JSON-stringified form `CacheImpl::lookupExpired` already constructs (`attrsToJSON(key.second).dump()`, line 88) — that is the byte form the SQLite layer uses for row identity, so wait/wake align with what a `lookup` would observe.

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
// equivalent registry call lookup/upsert directly.
template<typename ComputeFn>
Attrs CacheImpl::lookupOrCompute(const Key & key, ComputeFn && compute) {
    if (auto cached = lookup(key)) return *cached;

    auto fk = flightKey(key);
    std::promise<Attrs> ours;
    auto ourFuture = ours.get_future().share();
    std::shared_future<Attrs> winnerFuture;
    bool weAreTheWinner = false;

    // try_emplace_and_cvisit is atomic: either this caller wins and the
    // on_emplace visitor reads this future back, or another caller won
    // and the on_existing visitor provides its future. Both visitors run
    // while holding the bucket lock — there is no race window where the
    // entry is observable without a future.
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

1. **`try_emplace_and_cvisit` is atomic.** A naive `if (inFlight.count(k)) wait; else insert` is racy. Boost's `try_emplace_and_cvisit` either inserts and visits the inserted entry or visits the existing entry. Master's existing uses in `eval.cc::evalFile`, `primops.cc::RegexCache::get`, and `aws-creds.cc::getCredentialsRaw` use this pattern.
2. **`set_value` before `erase`.** Loser threads hold `winnerFuture` (a `shared_future` copy that survives the map entry's destruction); fulfilling the promise wakes them up. Erasing the in-flight slot only releases the winner's reference; the losers' copies remain valid. Reversing the order (erase first, then set_value) is *also* correct — `shared_future` is value-semantic — but ordering set_value first matches the intuition "fulfil obligations, then clean up."
3. **Exception propagation.** `try { ... } catch (...) { set_exception; erase; throw; }` — *not* a RAII guard. The guard form (`Finally` setting an exception on destruction) is invalid here because it would set the exception even on the success path if `upsert` happens to throw after `set_value`. The explicit try/catch keeps success and failure paths distinct.

**LazyAttr re-entrance caveat.** `attrsToJSON(key.second)` calls `forceAttr` per attr. If a key contains a `LazyAttr` whose computation re-enters the same cache key, bucket-lock deadlock is possible. Projection call sites must pass force-clean primitive keys into `lookupOrCompute`; the content-keyed paths proposed here already satisfy that constraint.

This registry should land with the first track that makes duplicate expensive computes visible under `eval-cores`, such as subtree fingerprinting plus lock-free reads.

---

## 3. Lazy Clone and Promisor Provider

Target behaviour: a user runs `nix build .#packages.x86_64-linux.foo` against a large monorepo, and Nix downloads only the commits, trees, and blobs read by evaluation. The implementation does not invoke system `git` and does not modify libgit2.

The construction is not "a fetching ODB backend." The R1/R5 trace rules in [REVIEW-DISCIPLINE.md](./REVIEW-DISCIPLINE.md) matter here: libgit2 has ODB backend hooks, but the hooks run under libgit2's own ODB mutex. The correct seam is a Nix-side object provider that fetches before libgit2 is asked to read the object, not a libgit2 callback that fetches after lookup has already entered the ODB.

The construction is four pieces:

1. A protocol-v2 upload-pack client over Nix's `FileTransfer`.
2. A repository/object provider (`ensureObjects`) that coalesces missing object IDs, indexes returned packs, and refreshes the ODB outside ODB callbacks.
3. A `GitSourceAccessor` read split: path-to-OID lookup under `state_`, object materialisation outside `state_`, then normal libgit2 lookup/streaming.
4. `SourceAccessor::prefetchSubtree` triggers that batch wants before the evaluator gets to one-blob-at-a-time reads.

### 3.1 What libgit2 must not do: fetch from ODB callbacks

A rejected implementation is a low-priority writable `git_odb_backend` that fetches a missing blob from inside `read`, `read_header`, `exists`, or `refresh`. That seam is unsafe.

libgit2 invokes those callbacks under the `git_odb` mutex: `odb_exists_1` locks before calling `b->exists` ([`odb.c#L687-L700`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L687-L700)), `odb_read_header_1` locks before `b->read_header` ([`#L989-L1005`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L989-L1005)), `odb_read_1` locks before `b->read` ([`#L1094-L1106`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L1094-L1106)), `git_odb_write_pack` takes the same lock before selecting a writepack backend ([`#L1602-L1613`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L1602-L1613)), and `git_odb_refresh` locks before calling backend `refresh` ([`#L1693-L1702`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/odb.c#L1693-L1702)). The lock is not a scheduler primitive for long-running network work; it is an internal ODB consistency lock.

The pack path re-enters the ODB too. Thin-pack repair calls `git_odb_read` for missing delta bases ([`indexer.c#L1015-L1017`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/indexer.c#L1015-L1017)), and full connectivity verification fails if expected objects remain missing ([`indexer.c#L1310-L1313`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/indexer.c#L1310-L1313)). A callback-local fetch that writes a pack through libgit2's normal ODB/indexer path can therefore re-enter the same mutex by multiple routes. The [lazy clone](#3-lazy-clone-and-promisor-provider) design forbids network I/O, pack indexing, `git_odb_write_pack`, and `git_odb_refresh` from ODB backend callbacks.

### 3.2 Protocol-v2 object provider over `FileTransfer`

The provider speaks enough upload-pack protocol v2 to fetch specific missing objects. Git's protocol-v2 fetch request has explicit `want <oid>` arguments and a `filter <filter-spec>` argument ([`protocol-v2`](https://git-scm.com/docs/protocol-v2)); Git partial clone uses promisor packs and filters such as `blob:none` to omit blobs until demanded ([`partial-clone`](https://git-scm.com/docs/partial-clone.html)). That is the wire shape the [lazy clone](#3-lazy-clone-and-promisor-provider) track needs.

`GitPromisorProvider::ensureObjects(std::span<const git_oid> wants, FetchFilter filter)`:

1. groups and deduplicates wants across evaluator threads;
2. serialises one stateless `command=fetch` request over `FileTransfer` with `want <oid>` lines, `filter blob:none` for initial skeleton fetches, and no `thin-pack` for on-demand backfills unless the needed bases are known locally;
3. feeds returned pack bytes to `git_indexer_*` outside any ODB callback;
4. writes `.promisor` sidecars if Git compatibility with ordinary partial-clone repositories is desired;
5. calls `git_odb_refresh` after indexing, again outside callbacks; and
6. retries ordinary libgit2 object lookup.

The HTTPS byte-stream layer itself stays in Nix: `FileTransfer` already has POST request bodies and streaming response callbacks. The latent prerequisite remains: `FileTransfer` must preserve POST on redirects by setting `CURLOPT_POSTREDIR` for `HttpMethod::Post`, otherwise a 301/302/303 can turn upload-pack POST into GET and drop the request body. That is still the stage-0 patch in [§9](#9-implementation-order-and-risk).

The provider should not globally register `"https://"` / `"git://"` transports unless it implements a complete delegating transport. libgit2 checks global custom transports before built-ins and does not try the built-in transport after a custom factory errors ([`transport.c#L51-L70`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/transport.c#L51-L70), [`#L121-L132`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/transport.c#L121-L132)). Per-remote callbacks or a direct provider-owned upload-pack client keep fallback local to the repository being fetched.

### 3.3 Repository opening and pack ingestion

After an initial `--filter=blob:none` fetch, a Git-compatible partial-clone repository records `extensions.partialClone = <remote>`. libgit2 rejects unknown extensions unless they are allowed by `GIT_OPT_SET_EXTENSIONS`: `partialclone` is not in the built-in extension list ([`repository.c#L1904-L1911`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/repository.c#L1904-L1911)), user extensions are checked first ([`#L1925-L1944`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/repository.c#L1925-L1944)), and unknown entries error out ([`#L1957-L1959`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/repository.c#L1957-L1959)). Nix should call:

```cpp
const char * exts[] = { "partialclone" };
git_libgit2_opts(GIT_OPT_SET_EXTENSIONS, exts, 1);
```

That whitelist only lets libgit2 open the repository. It does **not** install a promisor-aware fetch path. `GitPromisorProvider` owns that path and receives the promisor remote URL from repo config (`extensions.partialClone` -> `remote.<name>.url`) or from the fetcher input when creating a fresh repo.

Pack ingestion uses `git_indexer_*`, but with two rules:

- Run the indexer outside ODB backend callbacks and outside `GitSourceAccessor::state_`.
- Do not enable full connectivity verification for filtered promisor packs unless the verifier is made promisor-aware. `git_indexer_options.verify` exists for full connectivity checks ([`indexer.h#L98-L100`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/include/git2/indexer.h#L98-L100)), but partial-clone packs are allowed to omit promised objects by design.

For on-demand backfills, omit `thin-pack` unless base availability is known. libgit2's thin-pack repair reads missing bases from the ODB ([`indexer.c#L1015-L1017`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/indexer.c#L1015-L1017)); a single-blob backfill has no reason to assume those bases are present.

### 3.4 Split `GitSourceAccessor` lookup from object materialisation

Master currently enters libgit2 object lookup while holding the accessor state lock. `GitSourceAccessor::readBlob` locks `state_`, calls `getBlob`, and then streams/smudges the blob under that same lock ([`git-utils.cc#L807-L837`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/git-utils.cc#L807-L837)); `getBlob` calls `git_tree_entry_to_object` ([`#L1055-L1057`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/git-utils.cc#L1055-L1057)), which goes through libgit2 object lookup and ultimately ODB read.

[Lazy clone](#3-lazy-clone-and-promisor-provider) changes the shape to:

```cpp
GitEntry entry;
{
    auto state(state_.lock());
    entry = lookupEntryOidAndMode(*state, path, expectedType);
}

provider.ensureObjects({entry.oid});

Blob blob;
{
    auto state(state_.lock());
    blob = lookupBlobByOid(*state, entry.oid, entry.expectedMode);
}
streamBlob(blob);
```

The first phase reads trees and copies cheap identity data (`git_oid`, object type, file mode, attribute path context if needed). The second phase may perform network I/O and pack ingestion, but it happens outside `state_` and outside any ODB backend callback. The final phase asks libgit2 for the now-local object and streams it. This is also the prerequisite for [§6](#6-non-blocking-evaluation-io)'s non-blocking read split: path lookup and content streaming have different lock requirements.

### 3.5 Coalescing: prefetch on `SourceAccessor`, trigger in `EvalState`

The unmitigated cold-eval latency floor of partial-clone is severe: a naive `ensureObjects({oneOid})` call per blob still issues one HTTPS round-trip per missing blob. On a nixpkgs-scale cold eval that is tens of thousands of round-trips; at a typical 50 ms RTT, hours of wall-clock. Without coalescing, the [lazy clone](#3-lazy-clone-and-promisor-provider) v1 would be slower than today's full clone for any cold cache. Coalescing is therefore a v1 prerequisite, not a future-work optimisation.

Coalescing inside an ODB backend callback is structurally not available, and the backend seam is rejected above. The design splits mechanism from trigger:

- **Mechanism (on `SourceAccessor`).** Add `SourceAccessor::prefetchSubtree(const CanonPath & subpath, unsigned depth = 1, PathFilter & filter = defaultPathFilter)`, defaulting to no-op. `GitSourceAccessor` overrides it for partial-clone repositories: it walks the already-local tree objects, enumerates missing blob OIDs whose paths satisfy the composed filter, and dispatches one multi-want `command=fetch` through `GitPromisorProvider::ensureObjects`. Depth defaults to `1` for directory-neighbour prefetch; single-file callers can pass `0`. Wider prefetches are caller-selected because deep traversal of wide directories can fetch unrelated blobs.

- **Wrapper forwarding.** `MountedSourceAccessor::prefetchSubtree` resolves through mounts and forwards to the resolved accessor. `FilteringSourceAccessor::prefetchSubtree` composes the caller's filter with its own `isAllowed` after translating paths back into wrapper-relative form. This keeps the common `MountedSourceAccessor -> GitExportIgnoreSourceAccessor -> GitSourceAccessor` stack from stopping at the first wrapper's no-op default.

- **`.gitattributes` prefetch.** `GitExportIgnoreSourceAccessor::isAllowed` consults `.gitattributes` along the path. Those files are blobs and may be absent under `--filter=blob:none`. Before the main filtered prefetch, enumerate ancestor `.gitattributes` OIDs from local trees and issue one no-filter multi-want fetch for those exact blobs. Do not express this as `combine:blob:none+sparse:oid=...`; Git combines filters by intersection, so `blob:none` would still omit the blobs.

- **Trigger (in `EvalState`).** Call `prefetchSubtree` from evaluator sites that are about to read blob content: `EvalState::evalFile`, `EvalState::coerceToPath`, `prim_readDir`, and `prim_readFile`. Do not trigger from `prim_pathExists`; existence checks read only tree metadata. `prim_readFile` is the manifest/config case: it can prefetch the file's parent directory at depth `1`, fetching the target blob and neighbours in one round-trip. `evalFile` can use depth `2` to amortise nested import chains. A per-`EvalState` set of prefetched parent-tree OIDs bounds repeated triggers.

If a `read(oid)` fires without a prior `prefetchSubtree`, a fallback can speculatively widen the fetch to siblings reachable from the OID's parent tree (over-fetches when the eval really wanted only one file in a wide directory, but bounded). The fallback is a quality-of-implementation detail; the `prefetchSubtree`-driven path is what gives bounded latency on the dominant workloads.

On a typical nixpkgs flake metadata workload, the trigger-driven path should use a small number of round-trips instead of one round-trip per missing blob. Cross-tree audit also rules out an extra `FilteringSourceAccessor::readDirectory(path, callback)` override: the existing default passes the wrapper as the scoped accessor and preserves prefix/filter semantics for subsequent reads.

### 3.6 Choice of v2 implementation

libgit2 has no protocol-v2 support today and the upstream issue tracking it (libgit2#5564) has been stalled since 2020 (see [§10](#10-what-this-proposal-does-not-do)). Three viable paths exist for getting `command=fetch` with `filter blob:none` into Nix:

1. **From-scratch v2 layer in C++** (the proposal's primary path). Reuse libgit2's public packfile indexer (`git_indexer_*`) and ODB stack for ingest, but implement pkt-line framing, sideband demultiplexing, and v2 fetch request/response parsing in Nix. The useful libgit2 smart-protocol helpers live in internal source files and are not exported. The v2-specific surface is: detect `version 2` in the capability advertisement, emit `command=fetch` arguments with `want <oid>` / `filter blob:none` / `done`, parse sideband response data, and feed pack bytes to the indexer.

2. **`gitoxide` (Rust) via FFI.** `gix-protocol` ships v2 fetch machinery and an explicit `fetch::Arguments::filter` method today ([docs.rs](https://docs.rs/gix-protocol/latest/gix_protocol/fetch/struct.Arguments.html#method.filter)). Vendoring it would replace the from-scratch v2 layer entirely. Cost: a Rust toolchain dependency in Nix's bootstrap closure — affects bootstrap, cross-compilation, and the existing C++/meson build pipeline. The trade is release-engineering and governance complexity in exchange for avoiding a Nix-owned C++ protocol-v2 fetch implementation.

3. **Vendor and patch libgit2's smart transport.** Rejected for the structural reasons in [§1](#1-libgit2-constraints): v2 changes are new functions next to v0/v1, not edits, and the dependency surface is not self-contained.

The recommendation is **path 1 for the first implementation, with an explicit escalation gate to path 2 if prototyping reveals hidden complexity comparable to libgit2's full v0/v1 transport surface.** The gitoxide comparison keeps the implementation choice explicit: own a small C++ v2 provider now, or accept a Rust dependency in the Nix bootstrap path.

### 3.7 Expected envelope (qualitative)

The wire and disk reduction is **one to two orders of magnitude on workloads where blobs dominate repo size** — i.e., monorepos where the commit and tree objects are a small fraction of the packed total. For a `--filter=blob:none` fetch the wire and disk costs reduce from "everything" to "commits + trees + the blobs the evaluator reads," which for typical evaluation accessing a single package's source is a small subset. The exact ratio depends on repo structure (blob-to-tree ratio, packfile compression, history depth) and is not measured here. Concrete benchmarks against a real monorepo are required before committing to specific numbers.

Wire-mechanism details, all verified against master / libgit2 source:

- **Omit `thin-pack` on on-demand backfills.** v2 `command=fetch` accepts `thin-pack` as an opt-in argument; sending it instructs the server it may emit a thin pack with REF_DELTA bases the client is expected to have locally. For a single-OID backfill the client often has only the commit/tree skeleton, no blob bases, so a thin-pack response can force `git_indexer_append` into missing-base repair via `git_odb_read` ([`indexer.c#L1015-L1017`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/src/libgit2/indexer.c#L1015-L1017)). *Not sending* `thin-pack` instructs the server to send a self-contained pack. Initial skeleton fetches may keep `thin-pack` when base availability is known; on-demand blob backfills omit it.

- **Half-duplex POST is structural in `FileTransfer`.** `TransferItem::init` sets `CURLOPT_POSTFIELDSIZE_LARGE = request.data->sizeHint` up-front, so the upload size is committed before the request opens. The provider must therefore serialise the entire `command=fetch want want filter blob:none done` block to a `std::string` before opening the request. v2 stateless-RPC matches this shape — each `command=` is self-contained — so this is a documentation requirement, not a blocker. Streaming the want-list while reading the response is not possible with the current `FileTransfer` shape.

- **Fallback for servers without v2/filter support.** If the server does not advertise protocol v2 and the `filter` capability, the provider records the remote as "no partial clone" and the fetcher falls back to the existing full-fetch path. This is per-remote and cached in a TTL-keyed set of known-bad hosts, not a global flag-flip — which keeps it race-free against other `GitRepoImpl` instances mid-fetch.

- **`partialclone` extension whitelist is process-global state.** `git_libgit2_opts(GIT_OPT_SET_EXTENSIONS, ...)` mutates a `static git_vector user_extensions` in libgit2's `repository.c`. If Nix is embedded in a host that later calls the same opt with a different list, the `"partialclone"` entry is dropped. Document for embedders; do not attempt to merge with another consumer's list at runtime.

- **The extension whitelist does not install a fetcher.** libgit2 does not auto-wire promisor fetching in response to `extensions.partialclone` — the whitelist's only job is to keep `git_repository_open` from rejecting the config. Provider creation is `GitRepoImpl` / fetcher integration's responsibility ([§3.3](#33-repository-opening-and-pack-ingestion)).

- **`flock` on `objects/pack/.lock`** during `git_indexer` operations prevents concurrent `nix` invocations against the same repo from corrupting each other's pack writes. Small but mandatory.

For workloads where [lazy clone](#3-lazy-clone-and-promisor-provider) is rejected as inappropriate (e.g. CI runners with plentiful bandwidth and ephemeral disks), a documented fallback is `git_fetch_options.depth = 1` (single-commit shallow clone, supported by libgit2 today). Strictly worse than `filter blob:none` on wire bytes (you receive every blob in the tip commit before any local filtering), but requires zero new transport code, no v2 implementation, no `partialclone` extension — the path of least resistance for "give me less than a full history clone, no other changes."

### 3.8 The user sees nothing change

Nothing in the Nix expression changes. `builtins.fetchTree { type = "git"; url = X; rev = Y; }` works the same. The decision of whether to fetch lazily is made at the C++ level: if the server advertises `filter` in its protocol-v2 capabilities, Nix uses partial clone; otherwise it falls back to the existing full clone. **No flake-input attribute, no setting, no user opt-in.** Servers that advertise the protocol-v2 filter capability automatically benefit; servers that do not are unaffected.

If a later prototype uses per-remote libgit2 transport callbacks rather than a direct upload-pack client, the callback must be able to delegate cleanly to libgit2's normal transport when v2/filter is unavailable. Global `git_transport_register("https://", ...)` is an unsafe default because it preempts built-ins process-wide and has no automatic fallback after factory failure.

---

## 4. Subtree Fetching

([Subtree-aware fingerprints](#subtree-aware-fingerprints) and [content-keyed parse cache](#45-content-keyed-parse-cache) are concrete instances of the algebra in [§0](#0-the-underlying-pattern-content-addressed-eval-time-projection): the cache-key shape `(parentTreeSha, method, name) → StorePath` is the same content-addressed-projection shape Bazel RE's `Directory` / `DirectoryNode` digests have used in production since 2018, and the soundness law — equal tree-SHAs ⇒ equal narrowed StorePaths — is the Merkle-content-equality property the git ODB already guarantees structurally.)

**Scope: tree-shaped inputs, not only `git://`.** Nix already unpacks tarballs into a shared bare git ODB at `$XDG_CACHE_HOME/nix/tarball-cache-v2`. `Settings::getTarballCache()` returns a `GitRepoImpl` constructed with `packfilesOnly = true`; `unpackTarfileToSink` walks tar entries and feeds them to `GitFileSystemObjectSinkImpl`; the sink writes blobs and trees into the ODB via libgit2. The result is a tree-OID exposed through a `GitSourceAccessor` rooted at that tree.

`tarball`, `github`, `gitlab`, and `sourcehut` inputs share `tarball-cache-v2`. `git://` inputs use a separate per-URL `gitv3/<hashed-url>/` repository, and the protocol-v2 fetch lands there rather than in the shared tarball cache. The accessor type is still `GitSourceAccessor` in both pipelines, so the [projection typeclass](#2-projection-typeclass), [subtree-aware fingerprints](#subtree-aware-fingerprints), and [§5](#5-filtered-shape-cache)'s filtered-shape cache apply uniformly. The ODB sharing boundary differs.

[Lazy clone](#3-lazy-clone-and-promisor-provider) is git-protocol-specific because tarballs have no protocol-level partial-fetch primitive. Partial-clone wins on `git://` inputs land in the per-URL `gitv3` repo, so they do not propagate to later tarball fetches. Cross-fetcher blob dedup comes from independent unpacks producing identical tree/blob OIDs, not from ODB sharing. The `file` input scheme is the only fetcher outside the tree-shaped optimisations: it is a single blob that goes straight to NAR/store through `addToStore` and a plain `getFSAccessor`. There is no garbage-collection story for either `tarball-cache-v2` or per-URL `gitv3` repositories today; both grow unbounded and are user-cleaned via `rm -rf`.

**The split has historical, domain-specific reasons.** Per-URL `gitv3` was introduced in 2018 (Graham Christensen, commit `02098d207` "fetchGit: use a better caching scheme"): a single shared git ODB held multiple unrelated repos and `git fetch` was "maddeningly slow" trying to find common commits between large unrelated histories. The shared `tarball-cache-v2` was introduced in 2023 (Eelco Dolstra, commit `b36857ac8` "Add a Git-based content-addressed tarball cache") explicitly for cross-rev dedup of the same repo. The two decisions were never reconciled. A user with `inputs.nixpkgs.url = "github:NixOS/nixpkgs/<rev>"` plus a local-clone override (`--override-input nixpkgs git+file:///home/me/nixpkgs`) duplicates the snapshot tree at `<rev>` in two places. The current worst-case example is around 7% of nixpkgs disk: roughly 150 MB for the tarball-cache snapshot versus roughly 2 GB for a full-history clone.

**The unification is a small future ODB-alternates track.** libgit2's `git_odb_add_alternate` and the standard `objects/info/alternates` file form let a per-URL `gitv3/<hash>/` repo *read* objects from `tarball-cache-v2` without copying them. One write of `gitv3/<hash>/objects/info/alternates` containing the absolute path to `tarball-cache-v2/objects` is the whole change. Importantly, **the alternate is read-only and consulted only after main backends miss**, so the 2018 fetch-time-performance regression that drove the per-URL split does *not* recur — alternates do not trigger common-commit walks during `git fetch`. **Without [lazy clone](#3-lazy-clone-and-promisor-provider), the dedup benefit is read-only** (objects already in the alternate are visible to `git_odb_read` but the per-URL fetch still receives a full pack — disk duplication unchanged). **With the [lazy clone](#3-lazy-clone-and-promisor-provider) provider**, the alternate becomes a real bandwidth win: `--filter=blob:none` clones backfill blobs from the network only when *neither* the per-URL pack *nor* the `tarball-cache-v2` alternate has them. ODB alternates therefore compose naturally with lazy clone and are left as future work because the bandwidth payoff is gated on lazy clone shipping.

**The proposal targets one of two distinct questions.** Cross-tree verification shows that subtree pointers already exist, but at a different layer from subtree identity:

- **Layer 1 (existing): `dir` at the FlakeRef layer.** `inputs.foo.url = "github:owner/repo?dir=areas/foo"` works today. `FlakeRef` is `(Input, std::string subdir)`; the Input's narHash is whole-repo; `call-flake.nix` resolves user-visible `outPath = sourceInfo.outPath + "/" + dir` by string concatenation; the lock records `dir` in the locked entry. Subtree-as-pointer is already user-visible at a layer above where narHash lives.
- **Layer 2 (this proposal): accelerate the access pattern that `dir` produces.** The whole repo is mounted once; `${input}/sub` (or `dir = "sub"` after `call-flake.nix` glues it on) is the dominant evaluation pattern. [Subtree-aware fingerprints](#subtree-aware-fingerprints) make access through this subpath as cheap as if the subtree were a first-class Input — without changing what the lock records or what narHash means.

**Subtree-as-Input** means a subtree-rooted accessor, subtree-rooted `narHash`, isolated subtree store path, and a lock entry whose `narHash` hashes only the subtree. The `mountInput` narHash gate is scope-symmetric: it compares the hash of the supplied accessor root against the locked `narHash`. A subtree-rooted accessor therefore fits the invariant. `Input::computeStorePath` is content-addressed and scope-blind, and substituters serve NARs by hash without knowing whether the bytes represent a whole repository or a subtree. The work is feasible but belongs in a separate lock-file RFC: either redefine `dir` at the fetcher layer, or add a versioned `subPath` / `narHashScope` attribute to locked entries. Tecnix's `tectonixZoneCache_` is an application-level existence proof; generalising it to flake inputs is separate from this proposal.

**This proposal stops at Layer 2** — accelerating access through `dir`-style subpaths via [subtree-aware fingerprints](#subtree-aware-fingerprints) for the `builtins.path` / `builtins.filterSource` code path — and stages Layer 1 (subtree-as-Input first-class) as future work in [§11](#11-summary). The remainder of this section explains the Layer-2 mechanism.

### What `${input}/sub` looks like to C++ (Layer 2 access pattern)

Consider `t = builtins.fetchTree {...}; src = t.outPath + "/sub";` (or equivalently a flake input with `dir = "sub"`, which `call-flake.nix` resolves into the same string-with-context shape). The execution order is:

1. `fetchTree` runs `EvalState::mountInput` ([`paths.cc`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libexpr/paths.cc#L63-L88)). `mountInput` documents the current behaviour: it uses a dryRun, computes the narHash for mismatch checking, and computes the store path before mounting. The dryRun walks the entire tree to compute its narHash. **The accessor mounted is whole-repo** — the input scheme's `getAccessor` returns it rooted at the commit's full root tree-OID, and `mountInput` does not narrow.
2. `outPath` becomes the string `"/nix/store/HASH-source"`. The `+ "/sub"` (or `call-flake.nix`'s `+ "/" + dir` glue) is plain Nix string concatenation. From C++'s perspective, the result is a string with attached store-path context — there is no structured "user accessed `/sub`" event the fetcher can observe at this point.
3. Materialisation happens later, via `EvalState::ensureLazyPathsCopied`, which iterates `Opaque{path}` context elements. It calls `ensureLazyPathCopied(o.path)` with the **whole input's store path**, not a subpath.

So by the time C++ knows `/sub` was the user's interest, the entire tree has already been NAR-walked. **Layer 2's job** is to short-circuit this walk on second eval (and on cross-rev sharing) — without changing what `mountInput` does to the parent.

### Why the parent walk is unavoidable at Layer 2

The `mountInput` narHash gate (in `EvalState::mountInput`, `src/libexpr/paths.cc`) calls `fetchToStore2(fetchSettings, *store, accessor, FetchMode::DryRun, input.getName())`, producing `(storePath, narHash)` from a dryRun walk of the supplied accessor, then mounts that same accessor on `storeFS`, then compares `narHash != *originalInput.getNarHash()`. At Layer 2, the supplied accessor is whole-repo, so the dryRun's narHash is whole-repo. [Lazy clone](#3-lazy-clone-and-promisor-provider) reduces wire cost by deferring blobs, but an eager dryRun still reads every blob. The deferral menu in [Lazy Mounting and Deferred NAR Hash](#lazy-mounting-and-deferred-nar-hash) relaxes eagerness, not scope. Layer 1 is the path to subtree-scoped lock identity; lazy `narHash` is the Layer-2 path to avoid a whole-repo walk when no consumer observes the full input hash.

### `getFingerprint` is the canonical content-identity surface

The canonical pattern: **`SourceAccessor::getFingerprint(path) → (CanonPath, optional<string>)` is the single place content-keyed identity is computed for a source.** Every accessor-level concern that affects a path's content identity — subtree narrowing, dirty workdir state, `.gitattributes` filtering, LFS smudging — is encoded as a suffix on the fingerprint string, with the path possibly re-anchored to root. The `fetchToStore` cache key is then `(domain="sourcePathToHash", fingerprint, method, path)`. The [filtered-shape cache](#filtered-shape-cache) is intentionally separate: it keys after running the user filter and observing the accepted output shape.

**Cheap-content-keyed where available, coarse otherwise.** Not every accessor can produce a content-keyed fingerprint cheaply. `GitSourceAccessor` can (the git ODB has OIDs in hand); `PosixSourceAccessor` cannot (computing per-file content hashes would double disk IO since `addPath` re-reads the file). The canonical pattern is *opportunistic*: where the accessor implementation has a cheap content identity, return the content-keyed form (`tree:<sha>` / `blob:<sha>;m=<git-mode>` from [subtree-aware fingerprints](#subtree-aware-fingerprints)); otherwise return the inherited whole-accessor fingerprint (master's existing default — `{path, fingerprint}`). Wrappers always forward and append, regardless of whether the inner fingerprint is content-keyed or coarse. Coarse fingerprints lose cross-rev sharing for the inner identity but retain cross-eval sharing for wrapper concerns. Filtered sources stay out of the suffix schema in this proposal; [§5](#5-filtered-shape-cache) handles them in a separate post-filter cache.

This canonicalisation is a precondition for the [§0](#0-the-underlying-pattern-content-addressed-eval-time-projection) algebra to compose. Without it, every new concern that affects content identity becomes a fresh special-case in `fetchToStore`'s key-construction; with it, every concern is a `getFingerprint` override and they compose by string concatenation under a documented suffix schema.

#### Suffix schema (versioned, documented)

The fingerprint string is a sequence: `<root-tag>[;<suffix>]*`, where:

- **Root tags** identify what the fingerprint refers to:
  - `git:<rev>` — a git commit (whole-input, parent-rev-keyed). Existing master form.
  - `tree:<sha>` — a git tree (subtree, content-keyed by tree-SHA). Added by [subtree-aware fingerprints](#subtree-aware-fingerprints).
  - `blob:<sha>;m=<git-mode>` — a git blob plus the tree-entry mode that determines executable bit and symlink-vs-regular interpretation. Added by [subtree-aware fingerprints](#subtree-aware-fingerprints)' file branch.
  - `nar:<hash>` — a NAR-content-addressed source (tarball pipeline, etc.). Existing.
- **Suffixes** modify identity by some additional concern, in canonical order:
  - `;s` — submodules enabled at the input/root level. Existing. For paths under a mounted submodule, identity should delegate through `MountedSourceAccessor` to the inner submodule accessor; `;s` distinguishes parent-tree materialisation with populated submodules from the default raw-gitlink-as-empty-directory behaviour.
  - `;a=<hash>` — attribute-context digest for concerns whose semantics depend on `.gitattributes` along the queried path. Export-ignore and LFS both use this context; the digest includes enough path context to distinguish identical attribute blobs applied under different prefixes.
  - `;e` — exportIgnore enabled. Its path-dependent content identity is carried by `;a=<hash>`.
  - `;l` — LFS smudging enabled at the input level. On `git:<rev>` fingerprints this remains the existing coarse input-policy flag; on `tree:<sha>` / `blob:<sha>;m=<mode>` fingerprints it is sound only together with the `;a=<hash>` attribute context, because LFS smudge decisions are attribute-driven.
  - `;d=<hash>` — dirty workdir, with deleted+modified files content-hashed. Existing master form.
  - Future: `;lfs=<hash>` (per-blob LFS-smudge-cache key — see [§6](#6-non-blocking-evaluation-io)), `;<concern-tag>=<hash>` for new concerns.

Suffixes are ordered alphabetically by tag character to give a canonical form (`;d` before `;e` before `;l` before `;s`); two clients computing the same fingerprint produce byte-identical strings. **Versioned domain prefix** (`fingerprint-v1\0` baked into the cache row's domain string) lets future revisions coexist if the schema changes.

**NAR canonicalisation version is *not* in the schema today.** Master's NAR format has evolved over Nix releases; two clients on different Nix versions might produce different NAR bytes from the same tree-SHA + fingerprint. The cache rows for `sourcePathToHash` therefore inherit the cross-version-skew caveat from [§0](#0-the-underlying-pattern-content-addressed-eval-time-projection)'s join-semilattice analysis: equal keys produce equal values *modulo NAR-canonicalisation version*. Master accepts this risk (its existing cache rows do not tag NAR version either). The future fix is a `cache_v<N>` schema bump that includes the NAR version; out of scope for this proposal.

#### Soundness invariant

The return type is `(CanonPath, optional<string>)`: a *(returned-path, fingerprint)* pair that identifies a content-anchored cache cell. The two components are read together — `returned-path` is the cell's name *at the level the fingerprint anchors at*. For a tree-rooted `tree:T` fingerprint, `returned-path` is `CanonPath::root` (the tree-SHA's namespace anchors at root). For a parent-rev-keyed `git:R` fingerprint, `returned-path` is the in-input subpath. The pair always reads as "the cache cell named `returned-path` under the namespace identified by `fingerprint`."

The soundness invariant: **two `(returned-path, fingerprint)` pairs that match must produce the same NAR contents under any consumer (`addPath`, `addToStore`, `dumpPath`).** This is the projection-pattern soundness law ([§0](#0-the-underlying-pattern-content-addressed-eval-time-projection)) made concrete for the source accessor. The path component is part of the cell identity, not a redundant return — `MountedSourceAccessor::getFingerprint` strips the mount prefix; [subtree-aware fingerprints](#subtree-aware-fingerprints) re-anchor to root for content-keyed cases; both legitimately return a path that differs from the input.

A new suffix is sound iff the concern it represents is *content-determined*: the suffix value is computable from the source content alone, and adding/removing it changes the NAR contents in a determinable way. `;a=<hash>;e` is sound because exportIgnore is determined by `.gitattributes` content plus the queried path context. `;d=<hash>` is sound because dirty-file content is hashed in.

A concern that is *not* content-determined cannot get a suffix and must bail out of the cache (return `nullopt` for the fingerprint). [Filtered-shape cache](#filtered-shape-cache) avoids trying to make the filter itself a suffix by measuring the current filtered shape instead.

#### Composition rule

Wrapper accessors compose by appending suffixes. `FilteringSourceAccessor::getFingerprint` (base class) currently does:

```cpp
if (fingerprint) return {path, fingerprint};   // master: short-circuits
return next->getFingerprint(prefix / path);
```

**The short-circuit is incompatible with [subtree-aware fingerprints](#subtree-aware-fingerprints).** When `Input::getAccessorUnchecked` (`fetchers.cc:367`) sets `accessor->fingerprint = result.getFingerprint(store)` on the *outermost* wrapper (the master pattern), the wrapper's `getFingerprint` returns the coarse `git:<rev>+flags` form for *every* path, and the inner `GitSourceAccessor`'s path-aware override never runs. Two evaluations of `${input}/sub` against different revs of the same monorepo with the same subtree-SHA would both return `git:<rev1>` and `git:<rev2>` respectively — no cross-rev sharing.

**`computeOwnSuffix` semantics**:
- Return `nullopt` when the wrapper's concern is *legitimately unhashable*. Caller bypasses the cache.
- Return non-empty string when the wrapper has a suffix to add. The suffix's content-determined value must come from data the wrapper can compute deterministically.
- Return empty string when the wrapper has nothing to add (e.g., `MountedSourceAccessor`'s default routes through unchanged).
- *Throw* if the wrapper's data is not available (e.g., a network failure fetching `.gitattributes` under partial-clone). Network failures propagate as errors; they are not "unhashable" — they are "unreachable."

**The canonical pattern requires changing the base class to "forward and append, falling back to the wrapper's field if the inner has no content-keyed identity":**

```cpp
// New FilteringSourceAccessor::getFingerprint:
auto [innerPath, innerFp] = next->getFingerprint(prefix / path);
if (!innerFp && fingerprint) {
    // Inner has no content-keyed identity (e.g. PosixSourceAccessor for
    // a workdir input); the wrapper's `fingerprint` field carries the
    // input-level identity (master's existing assignment at
    // fetchers.cc:367). Return the field; this is master's short-circuit
    // behaviour, preserved for the case where it is correct.
    return {path, fingerprint};
}
if (!innerFp)
    return {innerPath, std::nullopt};
// Inner has content-keyed identity (tree:T / blob:B for the
// subtree-aware fingerprint branch on
// commit-rooted git accessors). Forward and append this wrapper's suffix.
//
// computeOwnSuffix returns:
//   nullopt — wrapper signals "this concern is unhashable; bypass";
//   empty   — wrapper has no suffix to add at this path (default base);
//   non-empty — the suffix (e.g. ";a=<hash>;e")
//               to splice into innerFp.
auto suffix = computeOwnSuffix(path);
if (!suffix)
    return {innerPath, std::nullopt};   // bypass
if (suffix->empty())
    return {innerPath, innerFp};
return {innerPath, mergeSuffix(*innerFp, *suffix)};   // alphabetical splice
```

**Two distinct cases handled by one virtual.** Workdir inputs (`AllowListSourceAccessor` over `PosixSourceAccessor`): inner returns `{path, nullopt}` because `PosixSourceAccessor` has no content-keyed identity; wrapper's `fingerprint` field (set by `Input::getAccessorUnchecked`) carries the input-level fingerprint `git:R;d=H`; the wrapper returns `{path, "git:R;d=H"}`. Commit-rooted git inputs (`GitExportIgnoreSourceAccessor` over `GitSourceAccessor` with [subtree-aware fingerprints](#subtree-aware-fingerprints)): inner returns `{root, "blob:B;m=100644"}` content-keyed; wrapper appends `;a=<hash>;e` → `{root, "blob:B;a=<hash>;e;m=100644"}` after canonical suffix ordering. Both cases produce sound cache keys for their respective use cases.

This is the structural change that lets [subtree-aware fingerprints](#subtree-aware-fingerprints) compose correctly with master's wrapper-fingerprint assignment, while preserving cache-keyed access for workdir inputs, which the content-keyed subtree branch does not accelerate.

**Corollary: LFS is an attribute concern; submodules are a mount concern.** The allocation of which flags live on the leaf vs which on a wrapper follows a pragmatic rule: **a flag is allocated to whichever accessor implements the concern**. If a wrapper class exists for the concern, the wrapper allocates the flag via `computeOwnSuffix`; otherwise the leaf carries it in its `fingerprint` / `flagSuffix` fields.

- `;e=H` (export-ignore gitattributes hash): allocated by `GitExportIgnoreSourceAccessor::computeOwnSuffix`, the existing wrapper for export-ignore. Path-dependent (gitattributes vary per directory).
- `;l` (LFS): upstream already supports LFS smudging when the input's `lfs` flag is set. `GitSourceAccessor::readBlob` checks `state->lfsFetch`, asks `lfs::Fetch::shouldFetch(path)`, and substitutes the LFS object bytes for a pointer file when the path's committed `.gitattributes` says `filter=lfs` ([`git-utils.cc#L807-L831`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/git-utils.cc#L807-L831), [`git-lfs-fetch.cc#L193-L202`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/git-lfs-fetch.cc#L193-L202); Git LFS also documents that tracking is `.gitattributes`-based in its [FAQ](https://github.com/git-lfs/git-lfs/blob/main/docs/man/git-lfs-faq.adoc#L252-L255)). Therefore `;l` alone is sound for coarse `git:<rev>` fingerprints, but content-keyed `tree:<sha>` / `blob:<sha>;m=<mode>` fingerprints need the path's `;a=<attribute-context>` as well. A future `LfsSmudgingSourceAccessor` ([§6](#6-non-blocking-evaluation-io)'s LFS-smudge-cache design) can move the suffix allocation from the leaf to a wrapper; the semantics are the same.
- `;s` (submodules): upstream already supports submodules by fetching each recorded submodule and returning a `MountedSourceAccessor` whose root is the parent repo and whose submodule mount points resolve to the submodule accessors ([`git.cc#L937-L973`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/git.cc#L937-L973)). `MountedSourceAccessor::resolve` picks the nearest mounted parent and strips the mount prefix before forwarding ([`mounted-source-accessor.cc#L60-L67`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libutil/mounted-source-accessor.cc#L60-L67)); its `getFingerprint` already delegates to the resolved accessor when no outer fingerprint short-circuits ([`#L100-L105`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libutil/mounted-source-accessor.cc#L100-L105)). After the short-circuit removal described above, paths inside a submodule get the inner accessor's own `git:<rev>` / `tree:<sha>` / `blob:<sha>;m=<mode>` identity. The `;s` flag remains on the parent/root fingerprint because materialising the parent tree with submodules enabled differs from materialising the raw gitlink as an empty directory.

When a future wrapper for `;l` lands, the flag migrates from leaf to wrapper. **This is a structural refactor, not an algebraic change** — the schema, the soundness invariant, and the wrapper-stack composition rule are unchanged. Submodules do not need a special content wrapper for paths below the mount point; the existing mounted-accessor abstraction already provides the generalisation.

The orthogonal question of *path-dependence* — whether the suffix value varies with the queried path — is independent: `;l` and `;s` are path-independent (compute once, use everywhere); `;e=H` is path-dependent (cached per `(rev, path)` on the wrapper). Path-dependence affects memoisation strategy, not which accessor allocates the flag.

`GitSourceAccessor::fingerprint` is set inside `getAccessorFromCommit` at construction time to `git:<rev>` plus input-level flags such as `;l` and `;s`. The new `getFingerprint` override returns `{path, fingerprint}` for `subpath.isRoot()` and content-keyed `{root, "tree:T;..."}` / `{root, "blob:B;m=<mode>;..."}` for non-root subpaths. **The wrapper then merges in attribute and export-ignore suffixes**: `tree:T` under export-ignore becomes `tree:T;a=H;e`, and an LFS-smudged file becomes `blob:B;m=100644;a=H;l` if the accessor will turn an LFS pointer into payload bytes. A bare `blob:B;l` is not sound: the blob OID does not include the ancestor `.gitattributes` files that decide whether LFS smudging applies to this path. **For workdir inputs**, the wrapper-fingerprint pattern from `fetchers.cc:367` continues to work: the inner `PosixSourceAccessor`'s `fingerprint` field is unset, so the wrapper's "fall back to field" branch fires, returning `{path, "git:R;d=H"}` — master's existing form (the input-scheme's `getFingerprint` includes the `;d` for dirty workdirs and is set on the wrapper). **For substituted accessors** (the `requireStoreObjectAccessor` path at lines 318-345), the accessor is not wrapped and the base class returns `{path, fingerprint}` directly — that path's `accessor->fingerprint = getFingerprint(store)` assignment at line 329 sets the *input-scheme-level* fingerprint with all flags pre-appended, which is correct for the un-wrapped case. **All three cases — commit-rooted, workdir, substituted — produce sound cache keys** under the unified `getFingerprint` virtual.

**Migration note for the cache.** [Subtree-aware fingerprints](#subtree-aware-fingerprints) keep `Input::getFingerprint`'s input-level form unchanged (`git:R;e` flag-only, as master). Only the *accessor-level* form changes: master returns `(path, "git:R;e")` for any subpath; the content-keyed branch returns `(/, "tree:T;e=H")` or `(/, "blob:B;e=H")` for non-root subpaths through commit-rooted accessors. Old `sourcePathToHash` rows keyed on `("git:R;e", subpath)` continue to be hit when something queries them; new rows accumulate at `("tree:T;e=H", /)` for content-keyed access. **There is no mass orphaning** — the old and new rows occupy disjoint key spaces and master's existing semantics are preserved for any caller that still queries by `("git:R+flags", subpath)`. The eval cache files (named after `LockedFlake::getFingerprint`, which calls `Input::getFingerprint`) are unaffected — `git:R;e` continues to name the same eval cache file across the upgrade. The two form choices (input-level flag-only, accessor-level value-form) are deliberate: the input level cannot afford to compute the gitattributes hash before path is known (would force a full input walk), and the accessor level has the path in hand and computes per-`(rev, path)`.

`GitExportIgnoreSourceAccessor` overrides `computeOwnSuffix` to return `;a=<attribute-context>;e`; an LFS-smudging wrapper would reuse the same attribute-context helper and append `;a=<attribute-context>;l` when smudging can affect bytes.

`mergeSuffix(inner, "<;tag=value>")` or `mergeSuffix(inner, "<;tag>")` parses `inner` into root + sorted suffixes, inserts the new suffix at the alphabetically-correct position, and re-serialises. A suffix is `;<tag>` or `;<tag>=<value>`; the parser splits on `;` first, then on the first `=` within each suffix to separate tag from value. The parser maintains an ordered list of known tags with a flag per tag indicating whether `=value` is required: `a=required`, `d=required`, `e=forbidden`, `l=forbidden`, `lfs=required`, `m=required`, `s=forbidden`. Unknown tags are schema errors and throw — adding a new tag requires updating the schema ([§4 "Suffix schema"](#suffix-schema-versioned-documented)) and the ordered list together. Lexicographic comparison handles `l` vs `lfs` correctly (`"l" < "lfs"` byte-wise). This rejects accidental schema drift at the implementation site rather than letting two clients silently produce different orderings for the same tag set.

**Wrapper-stack-order independence is required.** Two wrappers `A` and `B` that append distinct tags commute: `A(B(x)) = B(A(x))`, because both `mergeSuffix` calls insert into the same alphabetically-sorted suffix list at fixed positions (`;d` < `;e` < `;l` < `;lfs` < `;s`). Without alphabetical canonicalisation, the two stacks would produce different strings for the same content, breaking the soundness invariant. If two wrappers in a stack both append the same tag, `mergeSuffix` throws because the wrappers are competing for the same identity dimension.

The result: [subtree-aware fingerprints](#subtree-aware-fingerprints), [content-keyed parse cache](#45-content-keyed-parse-cache), and future accessor-level identity concerns flow through one virtual, share one cache namespace, and compose without per-concern code in `fetchToStore`. The [filtered-shape cache](#filtered-shape-cache) is separate because it observes the post-filter shape. Removing master's current wrapper short-circuit is required; otherwise the content-keyed subtree branch never fires through the wrapper chain.

`MountedSourceAccessor::getFingerprint` has the same short-circuit (`mounted-source-accessor.cc:100-105`). It needs the same "forward without short-circuit" treatment, with `computeOwnSuffix` defaulting to empty because the mounted accessor adds nothing of its own. Concretely: `mounted->getFingerprint(path)` resolves the mount, calls `accessor->getFingerprint(subpath)`, and returns the inner result unmodified. The structural change is the same; the default suffix differs.

Together, removing both short-circuits is prerequisite to [subtree-aware fingerprints](#subtree-aware-fingerprints) firing under the dominant accessor stack. [§9](#9-implementation-order-and-risk) includes this work in the subtree-fingerprint track.

### Subtree-Aware Fingerprints

The `addPath` primop in [`src/libexpr/primops.cc`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libexpr/primops.cc#L2840-L2864), called by `builtins.path` and `builtins.filterSource`, calls `state.store->toStorePath(path.path.abs())` inside its in-store gate. That call returns `std::pair<StorePath, CanonPath>` (per `StoreDirConfig::toStorePath`); the `CanonPath` second element is the subpath under the parent store-path. Today only the `StorePath` is consumed (for `queryPathInfo(storePath)->references`); the subpath is discarded.

The cause of cross-rev subtree dedup failure (the survey's [§6.1](./README.md#61-tree-sha-cache-miss-across-revisions--confirmed) reproduction) is in `GitInputScheme::getFingerprint`, which returns `rev.gitRev() + flags`: two distinct revs of the same repo produce distinct fingerprint strings even for an identical subtree. The `sourcePathToHash` key is `(fingerprint, method, subpath)`; identical `/sub` content under different parent revs keys on different rows.

The fix is to extend `GitSourceAccessor::getFingerprint(subpath)` so that for tree-rooted *and* file-rooted subpaths, the returned fingerprint is content-determined by the subtree-SHA or blob-SHA respectively, not by the parent rev:

```cpp
// In GitSourceAccessor, store both forms at construction:
//   fingerprint: "git:R;l;s" — full input-level form, used for root reads
//   flagSuffix:  ";l;s"      — input-policy flags appended to tree/blob OIDs

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
    if (type == GIT_OBJECT_BLOB) {
        auto mode = renderGitMode(git_tree_entry_filemode(entry));
        return {CanonPath::root, "blob:" + oid + ";m=" + mode + flagSuffix};
    }
    return {subpath, fingerprint};       // unmounted gitlink (submodules=false) or other: parent-rev-keyed
}
```

This re-anchors the cache key at root with an OID-derived fingerprint string for tree- *and* blob-rooted subpaths. Two evaluations against revs `r1` and `r2` with identical `/sub` subtree-SHA `T` both produce the cache key `("sourcePathToHash", {fingerprint: "tree:T", method: "nar", path: "/"})` and hit the same row. **Two evaluations reading the same byte-identical file from different revs produce `blob:B;m=<mode>` and hit the same row only when mode/type also match.** The mode is required because Git blob OIDs do not encode executable bit or symlink-vs-regular interpretation, while `GitSourceAccessor::maybeLstat` distinguishes normal, executable, and symlink entries and NAR serialisation emits the executable bit. The blob branch makes the [content-keyed parse cache](#45-content-keyed-parse-cache) cross-process and cross-rev — without it, file-rooted fingerprints fall back to parent-rev-keyed and parse-cache rows miss across revs of the same monorepo with byte-identical content.

**Re-anchoring the path to root is sound.** The type signature `pair<CanonPath, optional<string>>` already permits the returned `CanonPath` to differ from the input — `MountedSourceAccessor::getFingerprint` does exactly this when stripping the mount prefix. Reusing the existing `sourcePathToHash` domain (no new cache table) keeps the cache namespace flat. Old rows keyed on `git:<rev>+flags` for file-rooted subpaths continue to coexist; they accumulate and age out naturally.

**Attribute-dependent concerns require an attribute-context suffix.** When `options.exportIgnore` is set, `GitExportIgnoreSourceAccessor` wraps the raw `GitSourceAccessor`. Per [§4](#4-subtree-fetching)'s canonical pattern, the wrapper's `computeOwnSuffix` returns `;a=<attribute-context>;e`, where the attribute context covers every relevant `.gitattributes` file and the path context under which libgit2 applies it. The composition rule then produces `tree:T;a=H;e` or `blob:B;m=<mode>;a=H;e` as the cache key. The blob branch needs the suffix too: `.gitattributes` can mark an individual file as `export-ignore`, in which case `addPath` against the file produces an empty NAR rather than a NAR containing its bytes. Two evaluations against the same `blob:<sha>;m=<mode>` under different `.gitattributes` ancestors — one where the file is included, one where it is excluded — produce different `addPath` results, so the suffix is mandatory for soundness on the blob branch.

The same helper is required for LFS. LFS smudging is attribute-driven; ancestor `.gitattributes` outside a subtree are not covered by the subtree tree SHA. A plain `;l` on `tree:T` or `blob:B;m=<mode>` says only "LFS was enabled for this input," not "this path's attributes smudge this blob." Tecnix's existing `getGitAttributesAlongPath` virtual on `GitRepo` (commit-rooted, walks the tree picking up `.gitattributes` blobs at each ancestor) is the working primitive to port, but the generalized version must include enough path/prefix context, not only the `.gitattributes` blob OIDs.

Submodules are supported, but not by treating the raw gitlink entry's commit-OID as a tree fingerprint. With `submodules=false`, upstream `GitSourceAccessor::maybeLstat` deliberately treats `GIT_FILEMODE_COMMIT` as an empty directory ([`git-utils.cc#L861-L877`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/git-utils.cc#L861-L877)); the parent-rev-keyed fallback above is correct for that unmounted case. With `submodules=true`, the path should normally be resolved by `MountedSourceAccessor` before the raw `GIT_OBJECT_COMMIT` branch is seen, and the cache identity is the submodule accessor's identity after mount-prefix stripping. The current upstream limitation is narrower: `exportIgnore` and `submodules` together throw `UnimplementedError`, because `git archive` has no submodule behaviour to copy and it is unclear whether parent export-ignore rules should affect the submodule ([`git.cc#L1081-L1087`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/git.cc#L1081-L1087)). This proposal should preserve that limitation until upstream defines semantics; it is not a reason to exclude submodules generally.

**Cross-pipeline reuse** does not fall out of `getFingerprint` by itself. `fetchToStore2` consults `sourcePathToHash`; `treeHashToNarHash` is a separate domain currently used by tarball input metadata. If the proposal wants tarball/git sharing by tree SHA, `fetchToStore2` needs an explicit bridge: for root path, NAR method, no filter, and empty refs, a `tree:<sha>` fingerprint can consult/upsert `treeHashToNarHash` in addition to `sourcePathToHash`. After that bridge, a tarball downloaded today and a git input fetched tomorrow that unpacks to the same tree-SHA share a row. Without the bridge, they do not.

**Cache disk-usage growth.** Master's `sourcePathToHash` rows are rev-keyed (one per unique rev × method × subpath). [Subtree-aware fingerprints](#subtree-aware-fingerprints) shift to content-keyed rows (one per unique tree-SHA or blob-SHA × method). For a long-lived cache against a monorepo with churn (nixpkgs, tecnix's world), the row count grows roughly with the count of unique tree-SHAs and blob-SHAs ever encountered — order-of-magnitude more rows than master, not fewer. At millions of rows, the cache can reach hundreds of megabytes. Master users already periodically clean `~/.cache/nix/`; the content-keyed branch increases the growth rate. The trade-off is more disk usage for a much higher hit rate. A future `nix fetcher-cache gc` operation that evicts least-recently-used rows beyond a configurable size budget would help; currently cache is uncapped. Out of scope for this proposal; flagged for the same RFC as cross-machine cache merge ([§10](#10-what-this-proposal-does-not-do)).

The implementation is local: the `getFingerprint` override covers tree and blob branches, `fetchToStore2` gets an explicit `treeHashToNarHash` bridge, and `GitExportIgnoreSourceAccessor` gets a `.gitattributes`-along-path memo. Header impact is one virtual override on `GitSourceAccessor`; no new cache domain, no new in-process state, no new public API.

**What this solves transparently.**

- **Cross-rev subtree dedup** for code paths that go through `builtins.path` / `builtins.filterSource` against an in-store input subpath. Two different revs of the same monorepo whose `/sub` tree-SHA is identical share a single store path. This is the survey's [§6.1](./README.md#61-tree-sha-cache-miss-across-revisions--confirmed) "store-path equivalence under content equivalence" applied to subtrees, with no public API change and no narHash gate violation (because `mountInput` for the parent input still computed the parent's full narHash; the cache hit is for the *child* `addPath` only).
- **CI matrices** evaluating N branches that share a subtree pay one NAR walk total (the first eval to populate the row), not N.
- **Cross-fetcher reuse**: a `tarball://` input and a `git://` input that produce the same tree-SHA share `treeHashToNarHash` rows.

**What this does not solve.**

- **Workdir inputs do not benefit from the content-keyed branch.** The `tree:<sha>` / `blob:<sha>;m=<git-mode>` branch fires only for `GitSourceAccessor` (commit-rooted, OIDs in hand). Workdir inputs go through `getAccessorFromWorkdir` → `AllowListSourceAccessor` over `makeFSSourceAccessor` (`PosixSourceAccessor`); `PosixSourceAccessor::getFingerprint` returns the whole-input fingerprint with master's existing `;d=<hash>` suffix. Computing per-file `blob:<hash>` for FS-backed paths would mean reading each file twice (once to fingerprint, once to add), which is a poor cost trade. Workdir-rooted `${input}/sub` reads remain parent-fingerprint-keyed under [subtree-aware fingerprints](#subtree-aware-fingerprints). The `;d=<hash>` suffix correctly invalidates the cache when anything in the workdir changes, but it does not share rows across workdir revisions where a particular subtree happens to be unchanged. Dirty workdirs are handled by [source views](#source-views-subsets-overlays-trees-and-checkout-capabilities), where a dirty overlay can synthesize its own content identity after local state is read.
- **String-interpolation references.** `nativeBuildInputs = [ "${bigrepo}/areas/tools/foo" ]` does *not* invoke `addPath`. The string-with-context value flows through `derivation`'s `inputSrcs` machinery and `EvalState::ensureLazyPathsCopied`, which iterates `Opaque{path}` context elements and copies the **whole** parent input's storePath. The fingerprint extension only fires when the user wraps the subpath in `builtins.path { path = bigrepo + "/sub"; }` or equivalent.
- **First-fetch wire savings.** The parent input is still fully fetched on first contact to compute `mountInput`'s narHash. The fingerprint extension hits at the `addPath` step, *after* the parent has been mounted. First-eval wire savings depend on lazy `narHash` and deferred mount verification in [Lazy Mounting and Deferred NAR Hash](#lazy-mounting-and-deferred-nar-hash) below.
- **Subtree-as-first-class-Input.** The lock still records the parent input. `flake.lock` does not gain a "subtree was the unit" entry. If the user wants to pin a subtree independently of its parent, that still requires the deferred RFC — see [§4](#4-subtree-fetching).

**Caveats and side fixes.**

- `fetch-to-store.cc`'s existing filter-bypass ternary is narrowed by [§5](#5-filtered-shape-cache)'s filtered-shape helper rather than removed entirely. Filtered paths still pass a `PathFilter`, but the expensive content walk becomes cacheable under `(source-fingerprint, method, root, filtered-shape-hash)`.
- **`addPath`'s refs-bypass must be fixed with subtree fingerprints.** When `refs` (the parent store path's transitive references) is non-empty, `addPath` currently calls `addToStore` directly instead of `fetchToStore`, bypassing the cache regardless of fingerprint. This fires when `${input}/sub` references a derivation output's store path, such as `builtins.path { path = pkgs.stdenv.outPath + "/lib"; }`. Extend `fetchToStore` to accept `StorePathSet refs`, thread it into cache-hit store-path reconstruction and the inner `addToStore` call, and route refs-bearing paths through `fetchToStore`. The cache key remains `(domain, fingerprint, method, path)` because refs are content-derived from the path. Existing refs-empty callers keep their current store paths.
- **Restricted / pure-eval**: `EvalState::allowPath(StorePath)` casts `rootFS` to `AllowListSourceAccessor` and calls `allowPrefix`; the parent storePath was registered when `mountInput` ran, so the subtree's narrowed store path is already covered by the parent's allowed prefix. No additional `allowPath` call is needed at the `addPath` site.

### Lazy Mounting and Deferred NAR Hash

[Lazy clone](#3-lazy-clone-and-promisor-provider) closes the wire-cost gap from a different direction: instead of trying to know a subtree at first contact, fetch the commit/tree skeleton but defer blobs. When the evaluator reads `/sub`, only those blobs come over the wire. From the user's perspective the cost looks like "subtree-only fetching" even though the full tree skeleton is present.

That only helps first evaluation if mounting does not force the full input NAR hash:

- **First contact**: full clone of commits+trees, no blobs.
- **First eval reading `/sub`**: blobs reachable from `/sub` are fetched on-demand via `GitPromisorProvider::ensureObjects`.
- **`narHash`**: still scoped to the full locked input unless mount defers verification. If `mountInput` runs today's dryRun, that walk reads every blob and triggers the provider to fetch all blobs on the first evaluation.

Upstream `mountInput` already identifies the relaxation points: `outPath` and `narHash` can become lazy because many consumers use the path only for relative imports and never inspect the final store-path string ([`paths.cc#L63-L88`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libexpr/paths.cc#L63-L88)). DetSys demonstrates virtual mounting and devirtualisation: with `lazyTrees`, it mounts a random virtual store path ([`paths.cc#L75-L84`](https://github.com/DeterminateSystems/nix-src/blob/35cd98d10b3605d8d7e350460ae5c7dc60262f80/src/libexpr/paths.cc#L75-L84)) and later rewrites hash parts during `devirtualize` ([`#L23-L35`](https://github.com/DeterminateSystems/nix-src/blob/35cd98d10b3605d8d7e350460ae5c7dc60262f80/src/libexpr/paths.cc#L23-L35)). The part that must not be copied is the eager mismatch path: DetSys still calls `getNarHash()` for locked-hash comparison ([`#L107-L113`](https://github.com/DeterminateSystems/nix-src/blob/35cd98d10b3605d8d7e350460ae5c7dc60262f80/src/libexpr/paths.cc#L107-L113)), which can force the full dryRun walk.

**The floating-CA parallel is representation-level, not semantic identity.** The Nix manual's fixed/floating distinction is exactly whether the output store path is known before the output exists: fixed content-addressing knows the content address up front; floating content-addressing does not ([Nix derivation-output manual](https://nix.dev/manual/nix/2.29/store/derivation/outputs/index.html), [floating content-addressing manual](https://nix.dev/manual/nix/2.32/store/derivation/outputs/content-address)). Build-side floating CA does three things:

1. During evaluation, a content-addressed derivation output has no static store path. Nix emits a placeholder string instead (`DownstreamPlaceholder::fromSingleDerivedPathBuilt`) while the string context carries `SingleDerivedPath::Built` ([`eval.cc#L999-L1004`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libexpr/eval.cc#L999-L1004), [`downstream-placeholder.cc#L36-L49`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/downstream-placeholder.cc#L36-L49)).
2. During the build, the output bytes are hashed after they exist; `DerivationOutput::CAFloating` becomes a `ValidPathInfo` whose path is derived from the computed content address ([`derivation-builder.cc#L1740-L1783`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/unix/build/derivation-builder.cc#L1740-L1783)).
3. The store registers `DrvOutput{drvPath, outputName} -> outPath` as a `Realisation`; later consumers resolve through `queryRealisation` ([`derivation-builder.cc#L2024-L2038`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/unix/build/derivation-builder.cc#L2024-L2038), [`outputs-query.cc#L163-L180`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/outputs-query.cc#L163-L180), [`local-store.cc#L651-L680`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/local-store.cc#L651-L680)). The manual's design note about future output-hash assertions is the closest published Nix analogue to "download/register first, check assertion only when used" ([floating content-addressing manual](https://nix.dev/manual/nix/2.32/store/derivation/outputs/content-address)).

Lazy source mounting should copy that *handle / force / resolve* structure:

1. `mountInput` creates an `EvalSourceOutput` handle and mounts it at a virtual store path. The handle contains the accessor/view recipe, serialisation method, name, optional expected `narHash`, and a shared force state. It is the eval-layer analogue of `SingleDerivedPath::Built` plus a downstream placeholder.
2. At a force boundary, Nix resolves the handle to `(storePath, narHash)`: use an already valid/substituted CA path if available; otherwise copy/hash through `fetchToStore2`; then record the result in the handle and rewrite virtual hash parts to the final hash part exactly once. DetSys's `devirtualize` proves the rewrite shape.
3. If the recipe is content-determined, upsert the ordinary projection row (`sourcePathToHash`, `treeHashToNarHash`, etc.). Do **not** persist a `Realisation` keyed by the random virtual store path; that key is session-local. A persistent eval realisation table only makes sense for stable handles such as `EvalOutputId{sourceFingerprint, recipeId, outputName}` from [§0](#0-the-underlying-pattern-content-addressed-eval-time-projection)'s hypothetical eval-layer floating CA.

Lazy mount is floating-like operationally because consumers can hold an unresolved path handle. For locked Git inputs it is still semantically a lazy fixed projection: the Git commit/tree plus NAR serialisation method determine the eventual hash. Dirty overlays, local checkouts, and post-filter subsets are closer to true eval-layer floating CA because the content address is known only after reading local state or running the filter.

**Authenticity boundary.** Deferring the NAR walk is safe only when the accessor has another content-authentic identity before evaluator reads occur. A locked Git rev has that property: Git objects are named by the hash of their contents, and object consistency is independently checkable ([Git object database docs](https://git.github.io/htmldocs/git.html), [Git user manual object validation](https://git.github.io/htmldocs/user-manual.html)). For a Git-protocol input locked by `rev`, `narHash` is the store/NAR identity and substitution key; the source bytes read by the evaluator are already pinned by Git object IDs. For tarballs, GitHub/GitLab archive downloads, and other `narHash`-only inputs, the Nix manual explicitly says `narHash` verifies tree contents and enables substitution ([`builtins.fetchTree` manual](https://nix.dev/manual/nix/2.30/language/builtins.html#fetchTree-input)); those inputs must either substitute a valid store path or eagerly verify before exposing bytes to evaluation. Otherwise a mismatching archive could be evaluated before the mismatch is reported.

This gives the first real partition for lazy `narHash`:

- **Git-protocol rev/tree identity**: may defer full-NAR verification to materialisation/force boundaries.
- **Existing valid/substituted CA path**: no evaluator risk; the store path is already verified by `queryPathInfo` / `ensurePath`.
- **NAR-hash-only remote archives**: cannot defer verification across evaluator reads; use the known-hash substitution fast path or keep today's eager dryRun.
- **Dirty/local development without locked `narHash`**: can be lazy because there is no remote authenticity claim to protect; the output is local capability state and should not be persisted as a cross-process content row until hashed.

**Lazy `narHash` requires a materialisation object.** Master already has generic fetcher lazy attrs: `LazyAttrComputation` is an attr variant, `forceAttr` evaluates it, and `fetchTree` emits `revCount` as a Nix thunk via an internal primop ([`attrs.hh#L17-L34`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/include/nix/fetchers/attrs.hh#L17-L34), [`attrs.cc#L7-L16`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/attrs.cc#L7-L16), [`fetchTree.cc#L24-L111`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libexpr/primops/fetchTree.cc#L24-L111), [`#L145-L148`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libexpr/primops/fetchTree.cc#L145-L148)). `narHash` cannot use that generic attr shape directly:

- `emitTreeAttrs` currently calls `input.getNarHash()` and emits an eager string, so a lazy `narHash` stored in `input.attrs` would be forced before the result attrset is returned ([`fetchTree.cc#L123-L128`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libexpr/primops/fetchTree.cc#L123-L128)).
- `Input::getNarHash()` calls `maybeGetStrAttr`, which calls `forceAttr`; using it as a "does this input have a narHash?" predicate would unexpectedly force the computation ([`fetchers.cc#L423-L430`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/fetchers.cc#L423-L430), [`attrs.cc#L64-L70`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/attrs.cc#L64-L70)).
- `attrsToJSON` forces every lazy attr, which is correct for lock-file writing but incorrect for fetcher lock keys, debug rendering, or any code path that only wants to preserve an unevaluated input ([`attrs.cc#L37-L51`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/attrs.cc#L37-L51)).
- URL rendering for tarball/file inputs includes `narHash` by calling `getNarHash`, so putting a lazy hash in ordinary attrs can make `input.to_string()` or error/debug text force a full NAR walk ([`tarball.cc#L385-L392`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/tarball.cc#L385-L392)).

Therefore the implementation should introduce a shared materialisation object, not simply a lazy attr value in the generic map:

```cpp
struct LazyInputMaterialisation : ExternalValueBase {
    StorePath virtualPath;
    ref<SourceAccessor> accessor;
    std::string name;
    ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive;
    std::optional<Hash> expectedNarHash;
    Sync<State> state; // pending | running | done{storePath,narHash} | failed

    std::pair<StorePath, Hash> force(FetchMode mode);
};
```

`mountInput` stores this object in `EvalState`'s virtual-path table and in the emitted `narHash` thunk. `emitTreeAttrs` must check for the materialisation object before `input.getNarHash()`; if present, emit a thunk exactly like `revCount` rather than forcing. Existing generic lazy attrs still force through `attrsToJSON`; the `narHash` side-channel should instead be forced explicitly by lock writing and then serialised as an ordinary SRI string. `Input::getNarHash()` should stay forceful, but call sites that only need to test presence need a non-forcing helper (`peekNarHashAttr` / `hasNarHashAttr`) so pure-eval checks, URL rendering, and cache-lock construction do not accidentally materialise a monorepo.

The implementation has four stages, from local to invasive:

1. **Known-`narHash` substitution fast path.** If a final/locked input has a concrete `narHash`, compute the fixed-output store path and try the store before fetching. Master already does this with `computeStorePath` + `ensurePath` + `requireStoreObjectAccessor` ([`fetchers.cc#L319-L343`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/fetchers.cc#L319-L343)); DetSys reorders the same idea around a fetcher-fast path ([`fetchers.cc#L308-L393`](https://github.com/DeterminateSystems/nix-src/blob/35cd98d10b3605d8d7e350460ae5c7dc60262f80/src/libfetchers/fetchers.cc#L308-L393)). This is a strict correctness win. For monorepos it is also a policy knob: remote substitution may download the full source NAR and defeat partial clone, while a local valid-path hit is always cheap. Preserve today's `fetchTree` contract, but make this trade explicit in measurements.
2. **Virtual mount path + deferred verification for Git-shaped authenticated inputs.** Relax master's invariant that emitted `outPath` is already the final CA store path. Mount at a virtual store path, remember the accessor and expected locked hash, and devirtualize at store-path sinks. Verification of a locked `narHash` happens when the path is materialized, when a consumer explicitly forces `narHash`, or when lock writing needs the value — not during first mount. This is the DetSys shape, but with the mismatch check moved out of the mount hot path and guarded by the authenticity partition above.
3. **Lazy `narHash` output attr backed by `LazyInputMaterialisation`.** For inputs whose hash is not yet computed, emit `narHash` as a Nix thunk instead of forcing during `emitTreeAttrs`. Do not put this thunk blindly in the generic `Input::attrs` map. Force on explicit `input.narHash`, lock-file serialisation, metadata commands that print `narHash`, and devirtualisation; do not force on debug rendering, fetcher lock-key construction, or URL stringification.
4. **Lazy `outPath` via `LazyStorePathString`.** Requires a value/thunk shape understood by string interpolation, derivation hashing, JSON printing, and context handling. Once landed, `${input}/sub` can avoid computing the parent's store path until a consumer observes it. Combined with the [§4](#4-subtree-fetching) subtree-promotion seam at `addPath`, this unlocks full first-eval wire savings.

The virtual-mount + lazy-`narHash` pair is the first implementation step after the already-present known-hash substitution path, because it is what makes partial clone meaningful for monorepos. The implementation sketch is:

```cpp
// paths.cc::mountInput, sketch:
auto mat = make_ref<LazyInputMaterialisation>(
    fetchSettings, store, accessor, input.getName(), originalInput.peekNarHashAttr());
auto virtualPath = mat->virtualPath;
storeFS->mount(CanonPath(store->printStorePath(virtualPath)), accessor);
virtualInputs.emplace(virtualPath, mat);
input.lazyMaterialisation = mat; // side-channel, not generic attrs
return virtualPath;
```

The implementation captures only stable inputs (`FetchSettings`, `ref<Store>`, `ref<SourceAccessor>`, input name, expected hash), matching existing fetcher lazy computations. The main call sites are:

- derivation input collection: devirtualize opaque store paths and apply hash-part rewrites before hashing/writing the derivation, as DetSys does in `derivationStrictInternal`;
- `emitTreeAttrs`: emit a thunk for `narHash` from `LazyInputMaterialisation`, analogous to `revCount`; do not call forceful `Input::getNarHash()` first;
- `Input::toAttrs()` / lock-file JSON serialisation: force when writing the lock, matching the flake manual's guarantee that locked nodes include the expected tree contents hash and that `narHash` allows store-path computation/substitution ([flake manual](https://nix.dev/manual/nix/2.34/command-ref/new-cli/nix3-flake.html#lock-files));
- `Input::to_string()` / URL rendering: avoid forcing lazy `narHash` for diagnostics; force only when producing a canonical locked URL that intentionally includes `narHash`;
- pure-eval and lockability checks: distinguish "concrete user-supplied narHash" from "lazy computed narHash not yet known";
- substituted-input fast path: if a locked input already has concrete `narHash`, reconstruct/check from the locked value and try substitution; do not force a fresh full-tree walk merely to mount the virtual accessor.

Locked hashes are still verified, but verification moves from mount time to materialisation or explicit force time. With [lazy clone](#3-lazy-clone-and-promisor-provider), blobs outside the read subtree stay unfetched on the first evaluation. Lazy `outPath` unlocks the full first-eval savings of [§4](#4-subtree-fetching) plus [lazy clone](#3-lazy-clone-and-promisor-provider) when the parent store-path string is never observed.

**Locked hashes are assertions, not cache evidence.** Per cache key, the row state forms a chain `⊥ ≤ running ≤ done x`, with `done x ⊑ done y` iff `x = y`. Cache state across all keys is the product (a Scott domain). Deferred verification is monotone: it moves the row from `⊥` to `done x` exactly when something computes or substitutes the value. Asserting `done x` from a lock's `narHash` without either a valid substituted store path or a later materialisation check lives outside this dcpo. If the user-supplied lock disagrees with the eventually-computed `narHash`, there is no monotone path to recover; the row was committed `done` against a stale assertion. Known hashes should therefore derive candidate store paths and drive substitution, but they should not seed content-hash caches without verification.

**Cross-tree caveat on the dcpo claim**: master's `mountInput` produces content-determined `(storePath, narHash)` (the storePath is computed via `makeFixedOutputPathFromCA`), so the dcpo holds globally — same input across processes maps to the same `done x`. **DetSys's lazy-trees branch** does *not* satisfy this: when `settings.lazyTrees` is true, `mountInput` uses `StorePath::random(input.getName())`, so the same input maps to *different* store paths across `EvalState` instances. Tecnix's `tectonixZoneCache_` has the same `StorePath::random` semantics. In dcpo terms: the per-row chain `⊥ ≤ running ≤ done x` holds *within one `EvalState` lifetime*, but the global "same input ⇒ same `done x`" property is broken across processes. **The proposal targets master**, where this issue does not arise. Implementations that adopt DetSys/tecnix-style random-storePath laziness would need to qualify the dcpo claim to "monotone within a single eval session" and accept that cross-process cache sharing requires an additional canonicalisation step.

**Eviction in master** is real: `EvalState::resetFileCache()` clears `inputCache`, `fileEvalCache`, etc. on REPL `:reload`, and the SQLite cache's `lookupExpired` returns expired entries with a flag (the caller can choose to re-run or use the expired value). Both are `done x → ⊥` moves and break monotonicity in principle. In practice they happen on user signal (`:reload`) or for time-bound domains (`tarball`/`file`/`hgRefToRev`), neither of which is in the content-keyed Scott domain the dcpo claim describes. The dcpo claim should be read as "for content-keyed rows under continuous evaluator lifetime."

The `addPath` subtree-promotion seam from the previous subsection composes specifically with deferred `narHash` *or* `LazyStorePathString`: with lazy narHash, the subtree cache hit at `addPath` means nothing forces the parent's narHash, so the dryRun's tree walk never runs; with `LazyStorePathString`, even the parent's store path is never computed.

### Source Views: Subsets, Overlays, Trees, and Checkout Capabilities

Dirty overlays, sparse checkout roots, physical checkout paths, and arbitrary tree-SHA materialisation must be represented by the same internal model. Tecnix already demonstrates the common shape: `mountZoneByTreeSha` mounts a tree-rooted accessor at a virtual store path, while `DirtyOverlaySourceAccessor` overlays checkout files on top of a clean git tree for dirty zones ([`eval.cc#L841-L1045`](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/eval.cc#L841-L1045)). `getTectonixSparseCheckoutRoots` and `getTectonixDirtyZones` are provenance queries over the same local checkout ([`#L598-L759`](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/eval.cc#L598-L759)), and `__unsafeTectonixInternalTree` materialises an arbitrary tree object through `repo->getAccessor(hash, ...)` then `fetchToStore` ([`tectonix.cc#L135-L159`](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/primops/tectonix.cc#L135-L159)). These are four views over a source, not four unrelated features.

Master's current split explains why this needs an explicit abstraction. `GitInputScheme::getAccessorUnchecked` chooses either a rev/ref-backed commit accessor or a workdir accessor at the *Input* level ([`git.cc#L1090-L1092`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/git.cc#L1090-L1092)). Its dirty fingerprint is a suffix over the whole workdir input, hashing modified files and deleted paths ([`#L1107-L1122`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/git.cc#L1107-L1122)). That is sound for "this input is the current workdir", but it cannot express "evaluate clean tree R, then overlay exactly these dirty checkout paths" because the decision has already been made before a zone/subtree is known.

The missing upstream abstraction is:

```cpp
struct ViewRecipe {
    enum Kind {
        Root,
        Subtree,        // base / path
        Subset,         // base + accepted path trie / shape
        Overlay,        // base + overlay entries + whiteouts
        TreeHandle,     // object-format + tree OID + provider/ODB
        LocalCheckout,  // base + checkout metadata/provenance
    };
};

struct SourceViewIdentity {
    std::optional<std::string> fingerprint;  // persistent cache key, if content-determined
    std::optional<Hash> gitTree;             // real or synthetic tree OID, if Git-shaped
    std::optional<LazyAttr> narHash;         // eventual NAR hash, not forced at mount
    enum class Purity { ContentAddressed, LocalCapability } purity;
};

struct SourceViewAccessor : SourceAccessor {
    ref<SourceAccessor> base;
    ViewRecipe recipe;

    SourceViewIdentity getIdentity(const CanonPath & path);
    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;
    SourceProvenance getProvenance();        // sparse roots, workdir delta, checkout root
};
```

`SourceViewAccessor` still implements the normal `SourceAccessor` surface (`maybeLstat`, `readDirectory`, `readFile`, `readLink`, `getFingerprint`, `getPhysicalPath`). The new part is that it records the *recipe* for the view and classifies identity separately from provenance. A content-determined recipe may persist a cache row. A local checkout fact — physical path, sparse-root file, index/worktree generation, dirty status — is a capability/provenance value and must not be written as if it were globally content-addressed.

The algebraic laws are simple:

1. **Content rows require content-determined recipes.** `Root`, `Subtree`, `TreeHandle`, `Subset` over a Git tree, and `Overlay` after overlay blob contents/whiteouts are hashed can produce persistent fingerprints. `LocalCheckout` by itself cannot.
2. **Provenance is queryable but not substitutable.** `physicalPath`, sparse roots, and dirty-zone membership are valid answers for the current checkout/session. They are not valid substituter keys and do not survive being copied to another machine.
3. **Git-shaped views prefer Git identity over NAR identity.** If a view can produce a real or synthetic Git tree OID, that is the cheap Merkle identity. NAR hash becomes a lazy compatibility boundary, forced only when a NAR-addressed store path or daemon/substituter protocol requires it.

This directly addresses the "arbitrary subsets of any tree" question:

- For a Git-backed tree or tarball-unpacked-into-Git tree, arbitrary subsets can avoid reading blob bytes. The subset recipe walks tree objects, builds an accepted-path trie, and uses `git_treebuilder_*` (cited in [§1](#1-libgit2-constraints)) to synthesize a tree from the accepted entries' names, modes, and object IDs. Under `filter blob:none`, tree objects are present and blob objects are not needed for this identity. The key is `(base tree OID, accepted trie/shape, attribute policy)`, not the accepted path list alone.
- For an overlay view, unchanged files inherit their base tree entries without blob reads; dirty/added files are hashed once into Git blobs; deleted files become whiteouts. Rename is delete + add. The synthetic tree ID is then built from base entries plus overlay entries/whiteouts.
- For a plain POSIX accessor, there is no cheap Merkle identity. The subset is still implementable, but the first content-addressed key requires reading file bytes. After that first read, the filtered-shape cache in [§5](#5-filtered-shape-cache) can reuse the result for the same content fingerprint/shape.

This also clarifies "avoid a NAR hash." There are two different costs:

- **Avoid the eager evaluation-time NAR walk.** This proposal does that by lazy `narHash`, virtual mounts, Git tree/subset identities, and `SourceViewIdentity::gitTree`. Master's `mountInput` comment already points at lazy `outPath` and `narHash` as the relaxation point ([`paths.cc#L66-L72`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libexpr/paths.cc#L66-L72)), but the current code still calls `fetchToStore2(... DryRun ...)` immediately ([`#L73`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libexpr/paths.cc#L73)).
- **Avoid NAR as the final store serialisation.** Nix already has a Git ingestion hash path: `hashPath(..., FileIngestionMethod::Git, ...)` calls `git::dumpHash` rather than NAR hashing ([`file-content-address.cc#L97-L107`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libutil/file-content-address.cc#L97-L107)), and `StoreDirConfig::computeStorePath` uses the ingestion method when computing fixed-output paths ([`store-dir-config.cc#L161-L169`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/store-dir-config.cc#L161-L169)). But `Store::addToStore` still maps `FileIngestionMethod::Git` to `FileSerialisationMethod::NixArchive` with the comment "Git is not a serialization method" ([`store-api.cc#L170-L181`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/store-api.cc#L170-L181)), and remote/daemon paths have the same fallback comments. Therefore arbitrary Git-shaped subsets can avoid NAR during evaluation and for fixed-output identity, but fully avoiding NAR for store transfer requires [Git-shaped store registration](#git-shaped-store-registration) to become real: a Git serialisation/registration path or a daemon protocol extension that can carry Git-tree objects instead of NAR bytes.

Sparse checkout support is also implementable under this model. Git's own sparse-checkout command records working-tree selection in checkout metadata, using the sparse-checkout file and skip-worktree/index state rather than committed tree content ([`git-sparse-checkout(1)`](https://git-scm.com/docs/git-sparse-checkout)). Git's object filter language has a separate `sparse:oid` object-filter form for fetching only objects needed by a sparse spec ([`git-rev-list(1)`](https://git-scm.com/docs/git-rev-list)); that is content/object selection, not the same as "what does this user's local checkout currently contain." The internal model should represent the former as `SourceProvenance::sparseCheckoutRoots` and optionally feed the latter into the [lazy clone](#3-lazy-clone-and-promisor-provider) provider as a prefetch/filter optimisation. It must not mix the two into one cache key.

The implementation sketch is staged:

1. Add `SourceViewAccessor` and `ViewRecipe` with `Root`, `Subtree`, `TreeHandle`, `Subset`, `Overlay`, and `LocalCheckout` recipes. Existing `MountedSourceAccessor` and `FilteringSourceAccessor` become degenerate/adapter instances where possible; no behaviour change yet.
2. Add `GitTreeView` identity: `toGitTree()` returns a real tree for `Root`/`Subtree`/`TreeHandle`, synthesizes one for `Subset`/`Overlay`, and returns `nullopt` for POSIX-only views. This is where arbitrary subsets avoid blob fetches and avoid eager NAR.
3. Add `WorkdirDelta` as a libgit2-backed source of overlay entries and whiteouts. This replaces tecnix's shell-out `git status --porcelain -z` path ([`eval.cc#L688-L750`](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/eval.cc#L688-L750)) with a core Nix implementation consistent with the "no system git" constraint. Untracked-file policy is explicit: tecnix marks new untracked files dirty in its testing plan ([`testing.md#L212-L217`](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/plans/tectonix/testing.md#L212-L217)), so the upstream view recipe needs a policy bit (`IncludeUntracked::Yes/No`) rather than inheriting master's current tracked-file-only workdir accessor by accident.
4. Add `SourceProvenance` internally for sparse roots, workdir deltas, and physical checkout paths. Do not expose it by adding a builtin. Any user-visible provenance surface is a separate API decision constrained to reuse existing mechanisms or non-builtin metadata.
5. Wire existing paths (`builtins.path` / `fetchToStore` / mounted input materialisation) to prefer `SourceViewIdentity::gitTree` and [Git-shaped store registration](#git-shaped-store-registration) when available, force lazy `narHash` only at NAR-required boundaries, and fall back to today's `dumpPath`/NAR path for non-Git-shaped views.

### What about tecnix's eleven builtins?

This subsection separates two scopes:

- **Tecnix-specific migration scope** — what does tecnix have to change in its own Nix expressions to retire its builtins? Tecnix controls one library wrapper (`worldZone`); the clean source path (`ZoneSrc`/`ZonePath`) and manifest path can move to ordinary Nix once `worldRoot` is declared as a flake input rather than reached via the `--tectonix-checkout-path` setting. [Source views](#source-views-subsets-overlays-trees-and-checkout-capabilities) keep the dirty/sparse/local-checkout cases in the implementation model: they become provenance and overlay views over the same `worldRoot`, not bespoke internal mechanisms.
- **Nixpkgs-wide migration scope** — what would it take for *every existing nixpkgs derivation* using `${input}/sub` interpolation to reach the same fast path? That requires either Layer-1 subtree-as-Input (lock-file identity changes) or rewriting the bare-string-interpolation pattern across nixpkgs (derivation-hash transition). Both are out of frame here; both are real release-coordination events. **The proposal does not commit to nixpkgs-wide migration.**

The migration target is to replace tecnix-specific internal machinery with shared `SourceViewAccessor` machinery, use ordinary `builtins.path` / `readFile` where existing public APIs already express the operation, and leave any remaining provenance/tree-handle exposure to a separate no-new-builtin API design. It is not a promise that every tecnix builtin disappears behind `builtins.path`, and it does not add a new builtin to replace tecnix's builtins.

#### Per-builtin migration mapping (tecnix-specific)

| # | Builtin | Replacement | Mechanism |
|---|---|---|---|
| 1 | `__unsafeTectonixInternalManifest` | `builtins.fromJSON (builtins.readFile (worldRoot + "/.meta/manifest.json"))` | [Content-keyed parse cache](#45-content-keyed-parse-cache) fires on the content-fingerprintable `readFile`; per-process re-parse cost collapses to one cache lookup |
| 2 | `__unsafeTectonixInternalManifestInverted` | Pure-Nix inversion of #1, e.g. `lib.mapAttrs' (path: { id, ... }: { name = id; value = path; }) manifest` | [Content-keyed parse cache](#45-content-keyed-parse-cache) hit on the source |
| 3 | `__unsafeTectonixInternalTreeSha` | Internal identity query in `worldZone`; ordinary users usually do not need this value | [Subtree-aware fingerprints](#subtree-aware-fingerprints) return `tree:<sha>` automatically for tree-rooted subpaths; [source views](#source-views-subsets-overlays-trees-and-checkout-capabilities) carry the same tree identity internally |
| 4 | `__unsafeTectonixInternalTree` | Internal `TreeHandle` view; no new upstream builtin proposed | [Source views](#source-views-subsets-overlays-trees-and-checkout-capabilities) `TreeHandle` recipe: `(objectFormat, treeOid, provider/ODB, attr policy)` is enough to mount or materialise an arbitrary tree. [Lazy clone](#3-lazy-clone-and-promisor-provider) supplies missing objects; [Git-shaped store registration](#git-shaped-store-registration) avoids NAR serialisation where the store supports Git-shaped objects. Public exposure, if still needed, is a separate no-new-builtin API question |
| 5 | `__unsafeTectonixInternalZoneSrc` | `builtins.path { path = (worldZone zonePath).path; }` where `worldZone` returns an existing path value | `worldZone` becomes a library wrapper over paths backed internally by `SourceViewAccessor`: clean zones are `Subtree`/`TreeHandle` views keyed by tree OID; dirty zones are `Overlay(cleanTree, workdirDelta, whiteouts)` views |
| 6 | `__unsafeTectonixInternalZonePath` | `(worldZone zonePath).path` | Same view as #5; the wrapper returns a path value backed by a `SourceViewAccessor` |
| 7 | `__unsafeTectonixInternalSparseCheckoutRoots` | Internal `SourceProvenance` query; public replacement deferred unless expressible through existing metadata/config surfaces | [Source views](#source-views-subsets-overlays-trees-and-checkout-capabilities) `LocalCheckout` provenance. The sparse roots are local checkout metadata, not content-addressed source content; they are queryable but not substitutable |
| 8 | `__unsafeTectonixInternalDirtyZones` | Pure Nix over `manifest` plus an existing or downstream-provided provenance value | [Source views](#source-views-subsets-overlays-trees-and-checkout-capabilities) `WorkdirDelta` reports modified/added/deleted/renamed/untracked paths under an explicit policy. Dirty-zone projection is a manifest path-prefix query, not a separate builtin |
| 9 | `__unsafeTectonixInternalZoneIsDirty` | `(worldZone zonePath).dirty` | Same data as #8, scoped to one zone |
| 10 | `__unsafeTectonixInternalZoneRoot` | `(worldZone zonePath).physicalPath or null` | [Source views](#source-views-subsets-overlays-trees-and-checkout-capabilities) keep `getPhysicalPath` as a local capability. Store-backed or remote-only views return `null`; source-available checkout views return the checkout path if allowed by eval mode |
| 11 | `__unsafeTectonixInternalGitSha` | `worldRoot.rev` where available, or a normal tectonix setting read through existing configuration introspection | Commit selection is input/config metadata, not source materialisation infrastructure |

Two notes on the table:

- **[Content-keyed parse cache](#45-content-keyed-parse-cache) is what makes #1 and #2 transparent.** It is the new addition introduced for this purpose.
- **[Source views](#source-views-subsets-overlays-trees-and-checkout-capabilities) change the old answer for #7-#10 at the implementation layer.** Master's workdir accessor partially overlaps tecnix's `DirtyOverlaySourceAccessor` but does not replace it because master dispatches at Input granularity ([`git.cc#L1090-L1092`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/git.cc#L1090-L1092)). The proposal therefore adds the missing overlay/view layer instead of forcing a UX split between "rev-pinned clean" and "unpinned workdir dirty." The content-addressed part of a dirty zone is the synthetic overlay tree; the local facts used to decide whether to build that view are provenance/capabilities. It does not prescribe a new builtin for exposing those facts.

#### Nixpkgs-wide migration scope (out of frame)

For *every existing nixpkgs derivation* that uses `${input}/sub` interpolation in `mkDerivation { src = ...; }` to reach the same fast path, the same `addPath` seam must fire. That means rewriting `${input}/sub` to `builtins.path { path = input + "/sub"; }`, which is a derivation-hash transition for thousands of derivations — a release-coordination event, not a localised optimisation. The bare-string-interpolation idiom flows through `derivation`'s `inputSrcs` and `EvalState::ensureLazyPathsCopied` against the parent's whole storePath: by the time the context iteration in `derivationStrictInternal` sees the `Opaque{parent}` element, the `${zoneSrc}/sub` suffix exists only as bytes inside `drv.env["src"]`; the input set `inputSrcs = {parent}` is what the build sandbox bind-mounts. Narrowing `inputSrcs` to the child without rewriting the env-string gives the build a path the sandbox does not bind-mount; rewriting (via `BasicDerivation::applyRewrites`) recomputes every derivation hash. Storing the narrowing annotation out-of-band is also closed: `Derivation::unparse` is a fixed ATerm-shaped serialisation `(outputs, inputDrvs, inputSrcs, platform, builder, args, env, ...)` with no out-of-band slot.

Eliminating the bare-string-interpolation cost across nixpkgs requires Layer-1 subtree-as-Input — the subtree as the unit of lock identity rather than a pointer under a whole-repo input. The existing `dir` attribute (FlakeRef layer) provides a user-visible subpath pointer with whole-repo narHash; making the subtree itself the lock unit requires either redefining `dir` at the fetcher layer (back-incompat) or adding a `version: 8` lockfile + `subPath`/`narHashScope` attribute on locked entries (back-compat, opt-in). Tecnix's `tectonixZoneCache_` is an existence proof for the latter at the application layer. The migration design is its own RFC. **Not in scope here, and not a prerequisite for tecnix retiring its builtins.**

---

## 4.5 Content-Keyed Parse Cache

(This section is a concrete instance of the algebra in [§0](#0-the-underlying-pattern-content-addressed-eval-time-projection): it is `Projection<(content-fingerprint, format, parser-key), serialised-Value>`, with the content-fingerprint inherited from `SourcePath::getFingerprint` ([subtree-aware fingerprints](#subtree-aware-fingerprints)' `blob:<sha>;m=<git-mode>` form for the headline workload) and the value serialised in a custom row-per-document binary blob format (described below). The proposal lives in a *new* per-machine SQLite database (`parse-cache-v1.sqlite` next to `fetcher-cache-v4.sqlite`); it does **not** ride the eval cache, whose schema is per-fingerprint, row-per-attribute, and unsuited to whole-document caching. No new public API; no Nix-language extension.)

The survey at [§7.8](./README.md#78-lift-this-c-data-structure-to-a-nix-attrset) names a missing abstraction: there is no parse-cache for pure expressions over input contents. `builtins.fromJSON` re-parses on every cold eval; `builtins.readFile` of a content-addressed input followed by `fromJSON` yields a string whose content-fingerprint is known C++-side but never used as a cache key. Tecnix's `__unsafeTectonixInternalManifest` exists exactly to side-step this: it parses `.meta/manifest.json` once per `EvalState` and memoises the `nlohmann::json` in tecnix's `EvalState` (`tectonixManifestJson` under `std::once_flag`).

The fix is a `Projection<>`-shaped cache keyed on `(content-fingerprint, format-tag, parser-key)` whose value is a serialised parsed `Value` tree, persisted in a per-machine SQLite database (`parse-cache-v1.sqlite`). The cache fires at the call sites that already know they have a content-fingerprintable string:

- `prim_fromJSON` already receives a string. Today the string is opaque; the proposal records a content-fingerprint *out-of-band* (in a per-`EvalState` side-table keyed on the string's `StringData *` allocation identity) when the string originates from `builtins.readFile` against a content-addressed source. `prim_fromTOML` can use the same substrate later, but its cache key must include parser-mode state such as `Xp::ParseTomlTimestamps`.
- When `prim_fromJSON` runs against such a string, it looks up the side-table, computes the cache key, and consults `parse-cache-v1.sqlite`. Hit returns the cached parsed value (deserialise into the output `Value &`); miss parses, upserts, returns.
- The parse cache uses a custom call shape rather than the generic `Projection<>::lookup` because Nix `Value` is not pass-by-value-friendly: `lookup-or-deserialise-into-output-parameter` plus `compute-and-serialise-from-output-parameter` avoids intermediate copies and GC bookkeeping. Same algebraic discipline as the typeclass; different signature for `Value`'s ownership idiom. Master's `tarball` and `hgRefToRev` domains already use bespoke `lookupExpired` / `lookupWithTTL` shapes for the same kind of reason — the typeclass is *one* expression of the cache discipline, not the only one.

### Why a side-table, not a `NixStringContext` variant

A rejected design extended `NixStringContext` with a fourth `Fingerprint{path, fp-string}` element carrying the originating accessor's `getFingerprint` result. Cross-tree audit rules that approach out:

- **`forceStringNoCtx` has 11 callers** in master (`prim_fromJSON`, `prim_fromTOML`, `prim_throw`, several path-coercion sites). All of them throw on any context element. Adding a `Fingerprint` variant would either break every caller until each gains a new "is this a real reference or metadata?" check, or require introducing `forceStringPureContent` and migrating each call site individually.
- **`unsafeDiscardStringContext` and string operations would silently drop fingerprints.** Concatenation drops context conservatively; `replaceStrings` drops it; the JSON serialiser writes context as a structured field. Each of these would need a new "Fingerprint passes through as metadata, not a reference" arm — at minimum a comment, often code.
- **Fall-through `std::get_if<Opaque>` call sites would silently lose fingerprints** in a way that is invisible at the call site. The "Fingerprint is metadata, not a reference" framing only holds if the variant has the right semantics in every visitor; a single missed visitor is a soundness bug.
- **The `Fingerprint` element does not behave like context anyway.** Real `NixStringContext` elements affect derivation hashing; the fingerprint is a parse-cache hint that can disappear without breaking soundness. Encoding hint-data in a hashing-relevant type misclassifies its semantics.

The side-table design avoids all of this by keeping the fingerprint *out of* `NixStringContext` entirely. The cost is one extra hashtable lookup per `prim_fromJSON` call (microseconds) and the `StringData *` keying constraint, both bounded. Do not register empty strings by allocation identity: `StringData::make` returns the static empty string for empty input, so an empty `readFile` result needs either a special-case key or should bypass the side-table.

### Side-table design

Add to `EvalState`:

```cpp
// libexpr/include/nix/expr/eval.hh
struct StringFingerprint {
    CanonPath returnedPath;          // from getFingerprint's first component
    std::string fingerprint;         // the suffix-bearing form, e.g. "blob:B;m=100644;a=H;e"
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
// storage); address-of recovers the pointer. mkStringNoCopy expects
// (const StringData &, const Value::StringWithContext::Context *), where the
// Context * is materialised from a NixStringContext via .fromBuilder(...).
auto fp = path.accessor->getFingerprint(path.path);
const StringData & sd = StringData::make(state.mem, s);
auto * ctx = Value::StringWithContext::Context::fromBuilder(context, state.mem);
v.mkStringNoCopy(sd, ctx);
if (fp.second)
    state.stringFingerprints.try_emplace(&sd, StringFingerprint{fp.first, *fp.second});

// Option B (simpler, equivalent): call mkString and recover the pointer.
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
// prim_fromJSON, with the same shape available to fromTOML only after
// parser-mode state is added to the cache key.
auto str = state.forceStringNoCtx(*args[0], pos, /* error context */);
// Recover the StringData * the prim_readFile call registered. The payload
// is reached via the public string_data() accessor, which dereferences
// `getStorage<StringWithContext>().str`. That `str` field is a `const
// StringData *` set by mkString/mkStringNoCopy and copied bitwise through
// callFunction's vRes = vCur step (see "StringData identity preservation"
// below).
const StringData * sd = &args[0]->string_data();
if (auto fp = state.stringFingerprints.find(sd)) {
    auto cached = state.parseCache->lookup(fp->fingerprint, "json", "json-v1", fp->returnedPath);
    if (cached) {
        deserialiseValue(*cached, v, state);
        return;
    }
    // miss: parse, then serialise + upsert under the same key
    parseJSON(state, str, v);
    state.parseCache->upsert(fp->fingerprint, "json", "json-v1", fp->returnedPath, serialiseValue(v));
    return;
}
parseJSON(state, str, v);   // uncached fallback
```

`forceStringNoCtx` is unchanged. The side-table is invisible to existing callers; it changes no public behaviour and breaks no existing plugin.

### StringData identity preservation through `callFunction`

The required constraint: when `prim_fromJSON` looks up `&args[0]->string_data()`, that `const StringData *` must equal the one `prim_readFile` registered. Cross-tree verification against master at `nix/src/libexpr/eval.cc::callFunction` and `nix/src/libexpr/include/nix/expr/value.hh` confirms this works:

- **`mkString` allocates a fresh `StringData`** via `StringData::make(mem, sv)` (`value.hh::StringData::make` allocates on the GC-managed heap and returns a reference to the just-allocated object — no interning of string contents).
- **`callFunction` copies `Value` payloads bitwise**. Master's local is `vCur`: `Value vCur(fun);` near the top of `callFunction`, `fn->impl(*this, vCur.determinePos(noPos), args.data(), vCur);` for primop dispatch, then `vRes = vCur;` at the bottom. The `vRes = vCur` copy carries the `StringWithContext { const StringData * str; const Context * context; }` payload through unchanged.
- **`prim_readFile` writes its result to the stack-local `Value & v` parameter** (which aliases `vCur` from `callFunction`'s frame). After the primop returns, `vRes = vCur` copies the `StringWithContext` (including the `str` pointer) bitwise into the caller's `Value`.
- **By the time `prim_fromJSON` runs** with `args[0]` pointing at a Value whose payload was set by that copy chain, `args[0]->string_data()` returns the same underlying `StringData` that `StringData::make` allocated inside `prim_readFile`'s `mkString` call.

The `Value *` is *not* preserved across this chain (different stack frames hold different `Value` instances, and `vCur` itself goes out of scope when `callFunction` returns); only the `StringData *` payload pointer is. `Value *` keying is unsound because `prim_readFile` writes to `vCur` (not directly to `args[0]`), and the result is copied through `vRes = vCur`. Keying on `const StringData *` works because the payload pointer survives the copy.

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
    fingerprint TEXT NOT NULL,            -- e.g. "blob:abc...;m=100644;a=def;e"
    format      TEXT NOT NULL,            -- "json" now; "toml" later
    parser_key  TEXT NOT NULL,            -- parser/schema version, plus TOML feature state later
    path        TEXT NOT NULL,            -- the (canonical) returned-path
    body        BLOB NOT NULL,            -- recursively-encoded Value tree
    PRIMARY KEY (fingerprint, format, parser_key, path)
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

Symbol *strings* (not symbol-IDs) are stored for attrset keys, so two cold processes serialise/deserialise identical bytes for the same parsed tree regardless of intern order. **Cycles cannot occur** in `fromJSON` parse output, so no sentinel is needed for that case; functions/thunks similarly cannot occur (JSON produces only data values). The `0xFF` sentinel is defensive against future format extensions.

### `parse-cache-v1.sqlite` is its own database, not a table in the eval cache

Master's eval cache is per-fingerprint (`eval-cache-v6/<input-fingerprint>.sqlite`, with `<input-fingerprint>` being the *flake-input* fingerprint). [Content-keyed parse cache](#45-content-keyed-parse-cache) keys are *file-content* fingerprints (`blob:<sha>;m=<git-mode>` from [subtree-aware fingerprints](#subtree-aware-fingerprints)); a single `flake.lock` typically references many such files, and partitioning on flake-input fingerprint loses cross-input sharing — the same `manifest.json` content read from two distinct flakes would land in two separate eval cache files, defeating the parse-cache's purpose.

Per-machine `parse-cache-v1.sqlite` (next to `fetcher-cache-v4.sqlite`) is the correct shape:

- One file per machine, all parse-cached documents share it.
- Schema versioning via the filename suffix (parallels `fetcher-cache-v4`).
- WAL mode + a 2-second `busy_timeout` (matches `fetcher-cache-v4`'s pattern) for concurrent process access.
- Row-per-document, not row-per-attribute. The eval cache's `AttrType`-discriminated row-per-attr encoding is unsuited to whole-document parse output — reconstructing a thousand-entry attrset would need a thousand SQL round-trips, dominated by `sqlite3_step` overhead. Row-per-document is one read per call.

### Why bytes-only caching is not enough

Caching the original bytes keyed on content fingerprint is insufficient. Parsing dominates the headline workload: a manifest with thousands of zone entries. Caching bytes saves a disk read that the OS file cache already handles; caching the parsed tree skips the expensive work.

### Per-`readFile` cost

Master's `prim_readFile` does not call `getFingerprint`; the parse-cache path adds one `getFingerprint` call plus one `try_emplace` into the side-table. For commit-rooted git inputs, `getFingerprint` is a tree walk through the wrapper chain, bounded by a brief State-lock acquisition. For workloads with thousands of `prim_readFile` calls, the cumulative overhead is still small relative to the parse-cache hits saved on `fromJSON`.

### Cache invalidation

The fingerprint is content-derived; rows never need invalidating. The `cache_v1` filename suffix prevents cross-version aliasing if `fromJSON`'s parser semantics change (e.g. integer overflow handling) — bump to `parse-cache-v2.sqlite` and old rows are orphaned. If TOML is added, the parser feature/mode state is part of the key rather than relying only on the format string. Within a process, the side-table is cleared on `EvalState::resetFileCache()` (REPL `:reload`) — same hook that clears `fileEvalCache`.

### What this gives users

`let manifest = builtins.fromJSON (builtins.readFile (worldRoot + "/.meta/manifest.json")); in manifest."//areas/tools/foo".id` becomes O(parse-skip) on second cold eval. Tecnix's `__unsafeTectonixInternalManifest` becomes redundant; deleting it is the migration. The pattern applies to any user-side `prim_fromJSON (prim_readFile X)` — manifest files, configuration files, generated metadata, lock-file *fragments* parsed by user-level Nix code. **It does not apply to the *core* flake-input resolution path**: `libflake/flake.cc` parses `flake.lock` via dedicated `readLockFile` machinery (not `prim_fromJSON`), and that path is unaffected by the parse cache. **The parse cache saves the parse, not the file read** — `prim_readFile` always reads the file before `prim_fromJSON` runs (no lazy-strings yet), so file I/O happens but is bounded by the OS file cache. For tecnix's manifest with thousands of zone entries, parsing dominates: tens of milliseconds parse → microseconds cache lookup + deserialise.

### Implementation size estimate

| Component | Estimated LOC |
|---|---|
| `parse-cache-v1.sqlite` open + WAL + busy-timeout + schema + prepared statements | 120 |
| `lookup`/`upsert` methods on `ParseCacheImpl` (custom call shape, not generic typeclass) | 80 |
| Row-per-document `Value`-tree binary serialisation/deserialisation (per "Encoded-value format" above; symbol-strings rather than symbol-IDs for cross-process determinism; recursive) | 250 |
| `EvalState::stringFingerprints` side-table (declaration, traceable_allocator wiring, clear hook in `resetFileCache`) | 40 |
| `prim_readFile` `getFingerprint` + side-table `try_emplace` | 30 |
| `prim_fromJSON` cache-lookup + deserialise + miss-path serialise+upsert wiring (TOML later with parser-mode key) | 60 |
| Tests (round-trip, cross-process, cache-miss, cache-hit, side-table-clear-on-reload) | 30 |
| **Total** | **610** |

The JSON-first implementation is approximately 610 LOC. No new public API; no `NixStringContext` change; no `forceStringNoCtx` change.

---

## 5. Filtered-Shape Cache

(See survey [§6.6](./README.md#66-filtered-sources-bypass-the-fetcher-cache--confirmed) for the bypass this section closes.)

This proposal does not try to identify the filter function. It identifies the **observed filtered output shape**:

```text
(source-fingerprint, root-subpath, ingestion-method, filtered-shape-hash) -> output hash
```

This still runs the filter and traverses directory metadata, but a cache hit skips the expensive part: reading file contents and building/hashing the filtered NAR. It skips copying too only when the reconstructed fixed-output store path is already valid or can be substituted; otherwise the accepted-set filter is reused for the normal add path. It also safely shares rows between different filters that accept the same output shape. For `lib.cleanSource` / `lib.fileset.toSource`, this is the useful win without evaluator surgery.

### Filtered-shape cache

The filtered-shape collector must mirror `SourceAccessor::dumpPath` exactly. The root is included unconditionally; the user filter is called only for child entries. This root rule preserves today's `builtins.path` filter semantics: a wrapper built directly on `FilteringSourceAccessor` would incorrectly call `isAllowed("/")`.

The shape hash records canonical output structure, not file bytes:

- root-relative path
- node type (`regular`, `directory`, `symlink`)
- executable bit for regular files
- symlink target
- empty directories
- a schema/version tag and the NAR case-hack policy that affects directory entry names

Regular file contents are deliberately not read; the source fingerprint covers bytes. If the source accessor cannot produce a fingerprint for the root, filtered-shape caching bypasses exactly as today's filtered path does.

Two existing semantics stay fixed. First, `addPath`'s expected-hash valid-path shortcut stays before this helper; if `builtins.path { sha256 = ...; }` already names a valid store path, today's code returns it without resolving symlinks or invoking the filter. Second, `recursive = false` flat ingestion keeps ignoring filters, as it does today; filtered-shape caching is only for recursive/NAR ingestion.

### Implementation sketch

Add a helper near `primops.cc`:

```cpp
struct FilteredShape {
    Hash shapeHash;
    boost::unordered_flat_set<CanonPath> accepted;
};

FilteredShape collectFilteredShape(
    EvalState & state,
    Value * filterFun,
    const SourcePath & root,
    PosIdx pos);
```

The traversal is a sibling of `SourceAccessor::dumpPath`, not a wrapper around it:

```cpp
void walk(const SourcePath & root, const CanonPath & rel) {
    auto st = root.accessor->lstat(root.path / rel);
    shapeSink << rel.abs() << st.type;
    if (st.type == tRegular)
        shapeSink << st.isExecutable;
    else if (st.type == tSymlink)
        shapeSink << root.accessor->readLink(root.path / rel);
    else if (st.type == tDirectory) {
        auto entries = root.accessor->readDirectory(root.path / rel);
        for (auto & [name, _] : canonicaliseLikeDumpPath(entries)) {
            auto child = rel / name;
            if (state.callPathFilter(filterFun, {root.accessor, root.path / child}, pos)) {
                accepted.insert(child);
                walk(root, child);
            }
        }
    }
}
```

`collectFilteredShape` starts with `accepted.insert(CanonPath::root)` and calls `walk(root, CanonPath::root)` without calling the user filter on `/`. The path string passed to `callPathFilter` is the same absolute path the current `PathFilter` closure passes in `primops.cc`.

Then route the filtered branch in `addPath` through a small helper rather than widening `fetchToStore2` with evaluator types:

```cpp
StorePath fetchFilteredToStore(
    EvalState & state,
    Store & store,
    const SourcePath & path,
    Value * filterFun,
    const StorePathSet & refs,
    ContentAddressMethod method,
    std::string_view name,
    PosIdx pos);
```

That helper:

1. calls `path.accessor->getFingerprint(path.path)`;
2. bypasses if the fingerprint is absent;
3. collects the filtered shape;
4. looks up `filteredSourcePathToHash` with `{ fingerprint, method, path, shape }`;
5. reconstructs the fixed-output store path from `method + hash + refs` on hit, adds a temp root before validity checks, and returns only for DryRun, valid local paths, or successful substitution;
6. builds a cheap `PathFilter` from the accepted set on miss, so the user predicate is not run again during `computeStorePath` / `addToStore`;
7. upserts the resulting hash.

The refs fix from [subtree-aware fingerprints](#subtree-aware-fingerprints) is mandatory here too: cache-hit store path reconstruction and the miss path both need the same `StorePathSet refs`.

### Soundness

The result path set alone is not sound. The source fingerprint is mandatory:

```text
same source fingerprint + same root + same method + same filtered shape
    => same filtered NAR/hash
```

This remains sound for nondeterministic filters because the current run's output shape is measured. Side effects (`trace`, `warn`, IFD, file reads inside the filter) still happen because the filter still runs. The cache only avoids the later content walk/copy when the measured output shape matches an existing row. If filter evaluation itself dominates, that cost remains in scope for downstream library design or future work, not this proposal.

### What this gives users

- `mkDerivation { src = lib.cleanSource ./.; }` becomes cache-hit-able on second cold eval for the expensive content phase. The filter still runs, but a repeated accepted shape skips NAR construction, hashing, and copying.
- `mkDerivation { src = lib.fileset.toSource {...}; }` gets the same benefit, and two different filters that accept the same path set for the same source share a row.
- Nondeterministic or side-effecting filters keep today's observable behaviour because the filter still runs on every eval.
- No public API changes. No nixpkgs-side cooperation required. Old code paths keep working; only the filtered branch's post-filter cache lookup gets richer.

---

## 6. Non-Blocking Evaluation IO

The survey's lazy metadata and lock-free-read tracks together address evaluation-time IO. This section refines both with details from the libgit2 thread-safety constraints.

### What blocks today

Every `lstat`, `readDirectory`, `readFile` call from a Nix primop (e.g. `builtins.readFile`, `builtins.pathExists`) goes through `SourceAccessor` synchronously. For a `GitSourceAccessor`, master's [`readBlob` (git-utils.cc#L807-L837)](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/git-utils.cc#L807-L837) takes `state_.lock()` on its first line (a `Sync<State>` exclusive mutex per the survey's [§7.5](./README.md#75-synchronisation-primitives-dont-form-a-coherent-menu)) and holds it for the entire read — tree walk, OID lookup, and blob streaming all under one lock. With `eval-cores > 1`, this serialises every read through every parallel evaluator thread.

### Split the State Lock from the ODB Read

libgit2's threading model ([`docs/threading.md#L4-L25`](https://github.com/libgit2/libgit2/blob/e490b18b70e3d9d870d4affeb177218d684c47e3/docs/threading.md#L4-L25)) is the lever: `git_odb` is internally thread-safe and shareable; `git_repository`, `git_tree`, and most other handles are not. So a blob read decomposes into two phases with different locking requirements:

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
        // it is safe outside the State lock.
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

**The borrowed-pointer pattern requires `state->lfsFetch` to be stable for the lifetime of `readBlob`.** That holds: `Sync<State>` owns the State by value, the accessor owns the Sync, and `readBlob` does not outlive the accessor. Releasing the lock between Phase 1 and Phase 2 does not destroy the State; it only allows concurrent access.

Phase 1 is brief (one tree-walk per read) and stays under the State lock. Phase 2 is the actual byte transfer and runs lock-free against the ODB; Phase 2b (the LFS smudge HTTPS fetch) also runs lock-free, because `lfs::Fetch::fetch` does not touch `git_repository`/`git_tree` — it is a self-contained HTTPS+SHA-256 verification routine. Concurrent readers contend only on the brief Phase 1; throughput scales with `eval-cores` instead of being capped by one mutex. This is the survey's lock-free-read track with concrete code.

**The [subtree-aware fingerprint](#subtree-aware-fingerprints) `getFingerprint` path also contends on the State lock** because it calls `lookup(*state, subpath)` which mutates `state->lookupCache` (the tree-entry memo). Same Phase-1-shaped cost: microseconds per call, contention bounded by `eval-cores`. The State lock now covers two distinct call types (tree-walks for `readBlob` and tree-walks for `getFingerprint`), but neither holds the lock for byte transfer. **The `lookupCache` is the shared resource; making it a `boost::concurrent_flat_map` instead of a `Sync<unordered_flat_map>`-protected member would remove the contention entirely** (the cache is content-keyed, so concurrent insertions are safe). Stage as a follow-on optimisation if measurement shows the State lock contention is the bottleneck.

**Required refactor.** Master's `readBlob` does not call `git_odb_read` directly; it goes through `getBlob` -> `git_tree_entry_to_object`, which calls object lookup and ODB read internally. The phase split requires `getBlob` and the surrounding lookup machinery to return the OID from Phase 1, then call `git_odb_read` against the shared `git_odb` in Phase 2. The current `lookupCache` remains a brief contention point until it is moved to a concurrent map, but the lock no longer covers blob streaming.

**Future LFS-smudge projection.** Phase 2b fetches the same LFS payload for the same blob OID across processes. A later `Projection<git_oid, smudged-bytes>` can cache that result as a store-aware projection. The suffix schema reserves `;lfs=<smudge-hash>` for per-blob LFS smudge identity when `lfs=true` and the relevant blob triggers smudging.

### Lazy Attribute Scope, Not Lazy Bytes

The survey's lazy-metadata track proposes a unified `Lazy<T>` for "per-attribute and per-byte laziness." Per-attribute laziness already exists on master via `LazyAttr` (see survey [§7.4](./README.md#74-three-laziness-primitives-scoped-differently) and the [`LazyAttr` definition](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libfetchers/include/nix/fetchers/attrs.hh#L20-L34)) — it defers `revCount`, `lastModified`, etc. The natural extension is to make every L1 metadata projection (`getFingerprint`, `getProvenance`, `getLastModified`) optionally `LazyAttr`-backed.

**Per-byte laziness for `readFile` is out of scope** for this proposal: it would require a new Nix value kind ("lazy string") that ripples through every consumer (string interpolation, derivation hashing, JSON printing). The survey's existing `makeLazyNarAccessor` defers byte materialisation of NAR archives specifically — useful for substituters, not for arbitrary `readFile` calls.

What this proposal *does* deliver for non-blocking reads:

- **[Phase 1/Phase 2 split](#split-the-state-lock-from-the-odb-read)** removes the State-lock bottleneck for parallel evaluation.
- **[Lazy clone](#3-lazy-clone-and-promisor-provider)** means reads that are never triggered never fetch from network; the survey's [§2.4](./README.md#24-the-cli-always-devirtualizes) "paths that do not appear in final output stay virtual" generalises to "unread blobs are never fetched."
- **[`LazyAttr` extension](#lazy-attribute-scope-not-lazy-bytes)** defers per-attribute computation — useful for `getLastModified()` aggregation (survey [§5.6](./README.md#56-getlastmodified-aggregation--absent)) that today returns `nullopt` from combinators.

### What this gives users

- `eval-cores > 1` can speed up monorepo evaluation instead of serialising reads through the State mutex.
- Speculative reads (e.g. an IDE LSP eagerly evaluating multiple attributes in parallel) do not stall on each other.
- Combined with [lazy clone](#3-lazy-clone-and-promisor-provider), blobs that do not end up affecting any final output are never fetched from the remote, let alone read from disk.

---

## 7. Unified Picture

### How composition works (and where it stops working)

The proposal's composition story is **transparent through key independence, not through chained lookups.** Each `Projection<>` instance is keyed on its own content-derived `From`; composed projections share rows with single projections automatically because each step's content-addressed output is the next step's content-addressed input. There is no chain of lookups that has to walk through intermediate caches.

For the dominant `builtins.path { path = input + "/sub"; }` use case under [subtree-aware fingerprints](#subtree-aware-fingerprints), the asymptotics are:

| Workload | Today | After subtree-aware fingerprint cache hit |
|---|---|---|
| Same rev, repeated `${input}/sub` | O(N) re-NAR each time | O(1) SQLite lookup on `sourcePathToHash` keyed on `tree:<sha>` |
| Different rev, *unchanged* subtree | O(N) re-NAR (parent fingerprint differs today) | O(D · log F) tree-SHA lookup + O(1) SQLite hit — the `tree:<sha>` key is independent of parent rev |
| Different rev, *changed* subtree | O(N) full re-NAR | O(S) NAR walk of subtree only + cache write |
| Tarball and git input that yield same tree-SHA | Two separate NAR walks | Cross-pipeline reuse via `treeHashToNarHash`: one walk total |

(*N* = parent tree size; *S* = subtree size; *D* = path depth; *F* = fanout.)

**Where Merkle structure helps**: subtree extraction is O(D · log F) because the git ODB indexes tree entries by name; cross-rev subtree dedup is automatic because tree-SHAs collide structurally on identical content.

**Where Merkle structure does not help**: the [filtered-shape cache](#filtered-shape-cache) is content-addressed but only after running the filter; narHash in `mountInput` is over a NAR encoding distinct from the git tree-SHA encoding (so first-contact subtree-only fetching cannot be transparent until lazy-narHash ships); per-derivation context-string rewrites in `derivationStrictInternal` are O(input-string × context-cardinality), not Merkle-amortised.

**Practical floor**: until the virtual-mount plus lazy-`narHash` work ships ([Lazy Mounting and Deferred NAR Hash](#lazy-mounting-and-deferred-nar-hash), staged first in [§9](#9-implementation-order-and-risk)), the first eval of any newly-locked input pays the O(N) dryRun walk regardless of subtree caching. The asymptotic story is "second eval onward is O(1) when content unchanged"; the *first* eval is gated by the narHash invariant unless that hash is lazy.

### Tree-SHA to NAR-Hash Bridge

This bridge reuses the existing `treeHashToNarHash` fetcher-cache domain on git-mounted-subtree write/read paths. It is the explicit cross-pipeline connection between a Git tree identity and a NAR identity; without it, a tarball and a git input that produce the same tree-SHA still populate separate rows because `fetchToStore2` normally consults `sourcePathToHash`, not `treeHashToNarHash`. This closes the survey's [§7.1](./README.md#71-the-internal-tree-sha--nar-hash-projection-that-already-exists-but-isnt-surfaced) gap.

### Git-Shaped Store Registration

Git-shaped store registration is the store/daemon half of avoiding NAR for Git-shaped content. The eval path can avoid eager NAR with lazy `narHash` and synthetic tree IDs, but final no-NAR registration requires `FileSerialisationMethod::Git` (or an equivalent Git-tree registration path) because `Store::addToStore` maps Git ingestion to NAR serialisation today. This is what lets [source views](#source-views-subsets-overlays-trees-and-checkout-capabilities)' Git-shaped subtrees, subsets, and overlays keep Git identity through final store registration instead of falling back to NAR.

### CLI Devirtualisation

CLI devirtualisation is the opt-in presentation path for commands that print values containing paths. It covers `nix/eval.cc`, `nix-instantiate.cc`, and `app.cc` print sites and closes the survey's [§2.4](./README.md#24-the-cli-always-devirtualizes) invisibility gap without making every evaluation force virtual paths by default.

### Implementation Track Map

The survey tracks and this proposal's added tracks compose as follows. Each ships independently; wins compound.

| Track | Touches | Closes |
|---|---|---|
| [Projection Typeclass](#2-projection-typeclass) | new `src/libfetchers/include/nix/fetchers/projection.hh` — refactor of the ten separate cache domains | survey [§7.3](./README.md#73-content-addressed-identity-is-over-pluralised) (content-identity over-pluralised) |
| [Filtered-Shape Cache](#filtered-shape-cache) | New `collectFilteredShape` helper mirroring `dumpPath` root semantics; new `filteredSourcePathToHash` lookup keyed on `(source-fingerprint, method, root, filtered-shape-hash)`; miss path reuses the accepted-set filter so the user predicate is not run twice during the later content walk/copy. | survey [§5.1](./README.md#51-getfingerprint-component-composition--three-different-rules), [§6.6](./README.md#66-filtered-sources-bypass-the-fetcher-cache--confirmed), [§7.12](./README.md#712-filter--uncached-the-largest-single-performance-hole) |
| [Tree-SHA to NAR-Hash Bridge](#tree-sha-to-nar-hash-bridge) | existing `treeHashToNarHash` domain reused on the git-mounted-subtree write/read paths | survey [§7.1](./README.md#71-the-internal-tree-sha--nar-hash-projection-that-already-exists-but-isnt-surfaced) |
| [Git-Shaped Store Registration](#git-shaped-store-registration) | `Store::addToStore` (`store-api.cc`), new `FileSerialisationMethod::Git` / Git-shaped store registration | Lets [source views](#source-views-subsets-overlays-trees-and-checkout-capabilities)' Git-shaped subtrees/subsets/overlays use Git identity through final store registration instead of falling back to NAR serialisation |
| [Lazy Attribute Scope](#lazy-attribute-scope-not-lazy-bytes) | `LazyAttr`, `getLastModified`, `getProvenance` aggregation | survey [§5.6](./README.md#56-getlastmodified-aggregation--absent), [§7.4](./README.md#74-three-laziness-primitives-scoped-differently) |
| [Lock-Free Concurrent Reads](#split-the-state-lock-from-the-odb-read) | `GitSourceAccessor::readBlob` (split State-lock from ODB-lock) | survey [§7.5](./README.md#75-synchronisation-primitives-dont-form-a-coherent-menu) |
| [CLI Devirtualisation](#cli-devirtualisation) | `nix/eval.cc`, `nix-instantiate.cc`, `app.cc` print sites | survey [§2.4](./README.md#24-the-cli-always-devirtualizes) invisibility |
| [Lazy Clone and Promisor Provider](#3-lazy-clone-and-promisor-provider) | new protocol-v2 upload-pack client over `FileTransfer`; new `GitPromisorProvider::ensureObjects`; `GIT_OPT_SET_EXTENSIONS` whitelist at startup; `GitSourceAccessor` path-to-OID / materialisation split; `SourceAccessor::prefetchSubtree` virtual + `EvalState` triggers | Network cost for large monorepos, without fetching from libgit2 ODB callbacks |
| [Subtree-Aware Fingerprints](#subtree-aware-fingerprints) | `GitSourceAccessor::getFingerprint` override returning `tree:<sha>` for tree-rooted and `blob:<sha>;m=<git-mode>` for file-rooted subpaths; `;a=<attrs-context>` composition for export-ignore/LFS; explicit `treeHashToNarHash` bridge where applicable; ships with the `addPath` refs-bypass fix and the `FilteringSourceAccessor`/`MountedSourceAccessor` short-circuit removal | Cross-rev subtree *and* cross-rev byte-identical-file dedup for `builtins.path` / `builtins.filterSource`; cross-pipeline (tarball ↔ git) reuse only through the explicit bridge; no public API change |
| [Content-Keyed Parse Cache](#45-content-keyed-parse-cache) | New per-machine `parse-cache-v1.sqlite`; row-per-document custom binary blob format for the value tree; per-`EvalState` `StringData *`-keyed side-table records non-empty context-free `prim_readFile` fingerprints; JSON consumes it first, TOML later with parser-mode key. **No `NixStringContext` change, no `forceStringNoCtx` change.** | Re-parse cost on cold eval reduces to one cache lookup; survey [§7.8](./README.md#78-lift-this-c-data-structure-to-a-nix-attrset) closed for JSON; no public API change |
| [Source Views](#source-views-subsets-overlays-trees-and-checkout-capabilities) | Internal accessor/view layer with `Root`, `Subtree`, `TreeHandle`, `Subset`, `Overlay`, and `LocalCheckout` recipes; `SourceViewIdentity` separates `fingerprint`, `gitTree`, lazy `narHash`, and purity; `SourceProvenance` records sparse roots, workdir delta, and physical checkout paths | Generalises tecnix's dirty overlays, sparse-checkout roots, `ZoneRoot`, and arbitrary tree materialisation at the implementation layer without adding an upstream builtin; arbitrary Git-backed subsets avoid blob fetches and eager NAR through synthetic tree identity |

### Concrete user-visible outcomes

| Workload | Today | After proposal |
|---|---|---|
| `mkDerivation { src = lib.cleanSource ./.; }` | NAR walk every cold eval (survey [§6.6](./README.md#66-filtered-sources-bypass-the-fetcher-cache--confirmed)) | First eval records the observed filtered output shape; second eval still runs the filter but hits `filteredSourcePathToHash` and skips content NAR/hash/copy |
| `mkDerivation { src = lib.fileset.toSource {...}; }` | NAR walk every cold eval | Same — cache key is the accepted output shape for the current source; the filter still runs |
| CI matrix evaluating N branches sharing a subtree | N NAR walks (survey [§6.1](./README.md#61-tree-sha-cache-miss-across-revisions--confirmed)) | 1 walk total, all branches hit `Projection<TreeSha, NarHash>` |
| Large monorepo where evaluation touches a small subset of blobs | Full pack clone + per-eval read | Commits + trees fetched eagerly; only blobs the evaluator reads come over the wire (via the [lazy clone](#3-lazy-clone-and-promisor-provider) provider), provided mount/devirtualization defers the full-tree narHash walk. Quantitative reduction depends on repo blob-to-tree ratio; see [§3.5](#35-coalescing-prefetch-on-sourceaccessor-trigger-in-evalstate). |
| Parallel evaluator threads reading the same monorepo `GitSourceAccessor` | Serialised on the per-accessor `Sync<State>` (every `readBlob` holds the lock for the entire body) | Phase 1 holds the State lock briefly for the tree-walk; Phase 2 reads the blob lock-free against `git_odb` ([lock-free-read refinement](#split-the-state-lock-from-the-odb-read)) |
| `nix eval --json` of nested attrset with paths | Devirtualises every printed path | [CLI devirtualisation](#cli-devirtualisation) |
| Tecnix's monorepo zones (clean-tree access) | `ZoneSrc`, `ZonePath`, and `TreeSha` as internal identity queries | **Retired/inline conditional on declaring `worldRoot` as a flake input.** `worldZone` returns a `SourceView` path; clean zones are `Subtree`/`TreeHandle` recipes keyed by tree OID, and ordinary `builtins.path` materialises them |
| Tecnix's arbitrary tree materialisation | `__unsafeTectonixInternalTree treeSha` | `SourceView` `TreeHandle` recipe. The tree SHA, object format, provider/ODB, and attr policy are explicit; [lazy clone](#3-lazy-clone-and-promisor-provider) supplies missing objects and [Git-shaped store registration](#git-shaped-store-registration) is the path to no-NAR final registration |
| Tecnix's workdir/local-checkout state | `SparseCheckoutRoots`, `DirtyZones`, `ZoneIsDirty`, `ZoneRoot` + `DirtyOverlaySourceAccessor` | **Implementation substrate becomes [source views](#source-views-subsets-overlays-trees-and-checkout-capabilities), not new upstream builtins.** Sparse roots and physical roots are `SourceProvenance`; dirty-zone data is a manifest query over `WorkdirDelta`; dirty source paths are `Overlay(cleanTree, dirtyEntries, whiteouts)` views. The local facts remain capabilities, not persistent content cache keys |
| Tecnix's manifest parse-caching | 2 of 11 builtins (`Manifest`, `ManifestInverted`) + `tectonixManifestJson` per-`EvalState` memo | **Retired.** [Content-keyed parse cache](#45-content-keyed-parse-cache) makes `builtins.fromJSON (builtins.readFile (worldRoot + "/.meta/manifest.json"))` content-keyed and persisted across processes; the per-`EvalState` `nlohmann::json` memo collapses to a single SQLite row keyed on `(file-fingerprint, "json", "json-v1")`. Inversion (`ManifestInverted`) is pure Nix on top. |
| Tecnix's git-sha exposure | 1 of 11 builtins (`GitSha`) | Out of frame — CLI configuration, not infrastructure. Read via existing setting-introspection. |

---

## 8. What the user writes

For ordinary fetches, subtrees, filtered sources, and manifest reads, the user writes the same Nix they write today. No setting is needed to opt into lazy clone, subtree fingerprints, filtered-shape caching, or parse caching:

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
        # Pull one subtree of a 5 GB monorepo into the build closure.
        # Wrapping in builtins.path is what routes through addPath and
        # exposes the subpath to subtree-aware getFingerprint;
        # bare string interpolation of `${bigrepo}/...` routes through
        # ensureLazyPathsCopied with the parent's whole storePath instead
        # (see §4, "What about tecnix's eleven builtins?").
        toolFoo = builtins.path { path = bigrepo + "/areas/tools/foo"; };
      };
    };
}
```

For features that cannot be expressed in upstream Nix today — arbitrary tree handles and checkout provenance — this proposal does **not** add a builtin. The C++ implementation should still use `SourceViewAccessor` internally so tree handles, subsets, overlays, and checkout facts share one semantics. Any user-visible replacement for tecnix's remaining provenance/tree-handle builtins must be designed separately under the same "no new builtin functions" constraint, for example by reusing existing flake/input metadata or downstream library plumbing.

What happens at each layer, transparently:

- **Lazy clone of `bigrepo`**: when `git+https://example.com/...` resolves and the server advertises protocol-v2 `fetch=filter`, the [lazy clone](#3-lazy-clone-and-promisor-provider) provider issues `ls-refs` and `fetch` requests over `FileTransfer` and asks for `filter blob:none` (Git documents the `filter` fetch argument in [protocol-v2](https://git-scm.com/docs/protocol-v2#_fetch), and `blob:none` in [`rev-list --filter`](https://git-scm.com/docs/git-rev-list#Documentation/git-rev-list.txt-code--filterltfilter-specgt-code)). Initial fetch is commits + trees only — a fraction of the full pack, with the exact ratio depending on repo blob-to-tree weight. If the remote does not advertise filter support, the fetcher falls back to today's full-fetch behaviour.
- **Reading `bigrepo + "/areas/tools/foo"` via `builtins.path`**: when evaluation forces this path, `addPath` calls `fetchToStore`, which in turn calls `path.accessor->getFingerprint(path.path)`. The chain reaches `GitSourceAccessor::getFingerprint` with the subpath stripped to `/areas/tools/foo`; [subtree-aware fingerprints](#subtree-aware-fingerprints) return `(CanonPath::root, "tree:<sha>")` for the subtree, and the `sourcePathToHash` cache row is keyed on the subtree-SHA — independent of parent rev. If any previous evaluation cached the same subtree-SHA (under any rev, or via an explicit `treeHashToNarHash` bridge from a tarball pipeline that produced the same tree-SHA), the cache hits without re-NARing. On a miss, `prefetchSubtree` enumerates missing blobs reachable from `/areas/tools/foo` and calls `GitPromisorProvider::ensureObjects` outside libgit2 ODB callbacks and outside `GitSourceAccessor::state_`; other subtrees are never touched.
- **`lib.cleanSource ./.`**: ends up calling `builtins.path { filter = ...; }`. [Filtered-shape cache](#filtered-shape-cache) still runs the filter, records the accepted output shape, and on a repeated shape skips the expensive content NAR/hash/copy.
- **Concurrent reads of `bigrepo` from parallel evaluator threads**: contend only on the brief Phase 1 of `readBlob` (the State-lock-protected tree walk); the Phase 2 ODB read runs lock-free against the thread-safe `git_odb` ([§6](#6-non-blocking-evaluation-io)).

What does **not** happen transparently and remains user-visible:

- **First-eval narHash walk.** `mountInput` runs a dry-run to compute the input's narHash for the lock check. Today this walks the full tree; under [lazy clone](#3-lazy-clone-and-promisor-provider), that walk forces all tree blobs to be fetched on the first eval because the hash is over full content. To avoid this, `mountInput`'s dry-run needs to become a lazy `narHash` computation (the relaxation [`paths.cc`'s `mountInput` comment](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libexpr/paths.cc#L63-L88) anticipates). The [Lazy Mounting and Deferred NAR Hash](#lazy-mounting-and-deferred-nar-hash) staging — virtual mount, `LazyInputMaterialisation`-backed `narHash`, and later `LazyStorePathString` — covers the chain of work needed; until lazy `narHash` lands, the first eval pays the full-tree NAR cost and lazy clone's first-eval wire win collapses to later evaluations or paths that never mount the input.
- **String-interpolated subpaths of inputs.** `nativeBuildInputs = [ "${bigrepo}/areas/tools/foo" ]` (string concatenation in a build-input list, no `builtins.path` wrapper) routes through `EvalState::ensureLazyPathsCopied` with the parent input's whole storePath, not through `addPath`. [Subtree-aware fingerprints](#subtree-aware-fingerprints) do not fire there; the parent input is materialised in full. Accelerating this pattern at the derivation level changes every existing nixpkgs derivation hash that uses such interpolation, and is deferred to the subtree-as-Input RFC (see [§4](#4-subtree-fetching)).
- **Subtree-as-Input (Layer 1).** The lock file still records the parent input's whole-repo `narHash`; the existing `dir` attribute provides a user-visible subpath pointer at the `FlakeRef` layer. The user gets lazy parent fetching and per-subpath dedup at materialisation time, both riding on the current `dir`/Layer-2 mechanism. Making the subtree itself the lock-file identity is feasible but belongs in its own RFC; [§4](#4-subtree-fetching) sketches the design.

---

## 9. Implementation order and risk

Recommended order, prioritising the monorepo path first:

0. **`CURLOPT_POSTREDIR` prerequisite**. Set `CURLOPT_POSTREDIR = CURL_REDIR_POST_301 | CURL_REDIR_POST_302 | CURL_REDIR_POST_303 | CURL_REDIR_POST_307` for `HttpMethod::Post` requests in `src/libstore/filetransfer.cc`. Curl can otherwise convert POST to GET on redirect and drop the upload-pack request body. This is independently correct and required for [lazy clone](#3-lazy-clone-and-promisor-provider).
1. **[Lazy `narHash` in `mountInput`](#lazy-mounting-and-deferred-nar-hash)**. This is the smallest change that makes partial clone useful on first eval: without it, `mountInput`'s dryRun walks the whole tree and fetches every blob anyway. Emit `narHash` as a thunk backed by `LazyInputMaterialisation`, not as an ordinary generic attr-map `LazyAttr`; force at lock-write, explicit hash checks, devirtualisation, and canonical locked-URL generation. Locked expected-hash mismatches still fail deterministically, but the comparison moves to the first point that needs the actual NAR hash instead of every mount.
2. **[Lazy clone](#3-lazy-clone-and-promisor-provider), phase 1: repository opening + Nix-side protocol-v2 provider.** Whitelist `partialclone` with `GIT_OPT_SET_EXTENSIONS`, tolerate promisor config when opening repositories, implement protocol-v2 `ls-refs` and `command=fetch` over `FileTransfer`, request `filter blob:none`, feed returned pack bytes through `git_indexer_*`, and refresh the ODB after pack ingestion. No global `git_transport_register`; no ODB backend that fetches while libgit2 holds ODB mutexes.
3. **[Lazy clone](#3-lazy-clone-and-promisor-provider), phase 2: `ensureObjects` + `prefetchSubtree` coalescing + eval triggers.** Add `GitPromisorProvider::ensureObjects(span<Oid>)`, add `SourceAccessor::prefetchSubtree(path, depth, filter)` with wrapper forwarding, implement `GitSourceAccessor` enumeration of missing blob OIDs, and trigger from `evalFile`, `coerceToPath`, `prim_readFile`, and `prim_readDir`. Git's own partial-clone design warns that one-by-one dynamic fetch is slow and bulk prefetch is the escape hatch ([partial-clone "Handling Missing Objects"](https://git-scm.com/docs/partial-clone.html#_handling_missing_objects)); this phase is therefore part of lazy clone, not a later polish pass.
4. **[Source views](#source-views-subsets-overlays-trees-and-checkout-capabilities), phase 1: SourceView core + Git tree identity.** Add `SourceViewAccessor`, `ViewRecipe`, `SourceViewIdentity`, and `toGitTree()` for `Root`, `Subtree`, `TreeHandle`, and Git-backed `Subset`. This is the no-rewrite foundation for arbitrary subsets and tree handles; it should land before more special cases accrete.
5. **[Git-shaped store registration](#git-shaped-store-registration).** The evaluation path can avoid eager NAR with lazy `narHash` and synthetic tree IDs, but final no-NAR store registration requires this step because `Store::addToStore` maps Git ingestion to NAR serialisation today ([`store-api.cc#L178-L180`](https://github.com/NixOS/nix/blob/b6676343999cf9ec21aea0ac60d4d5d4d13d4fb4/src/libstore/store-api.cc#L178-L180)).
6. **[Source views](#source-views-subsets-overlays-trees-and-checkout-capabilities), phase 2: `Overlay` + `WorkdirDelta` + provenance.** Add libgit2-backed workdir delta with explicit untracked policy, overlay entries and whiteouts, `getPhysicalPath`/`SourceProvenance`, and internal provenance plumbing. Do not add a builtin; any public exposure is a separate no-new-builtin API question.
7. **[Lock-free reads](#split-the-state-lock-from-the-odb-read).** This is still important for `eval-cores`, but it follows the lazy-fetch substrate rather than blocking it.
8. **[Subtree-aware fingerprints](#subtree-aware-fingerprints).** Ship with the refs-bypass fix, wrapper `getFingerprint` composition, leaf `fingerprint`/`flagSuffix`, `blob:<sha>;m=<git-mode>` file identity, attribute-context `;a=<hash>` composition for export-ignore/LFS, and the explicit `treeHashToNarHash` bridge. This gives cross-rev subtree/blob dedup and makes the [content-keyed parse cache](#45-content-keyed-parse-cache) cross-rev.
9. **[Filtered-shape cache](#filtered-shape-cache).** Route filtered `addPath` through `collectFilteredShape`, key by `(source-fingerprint, method, root, shape)`, and reuse cached output hashes when the observed accepted shape matches. Git-backed accepted shapes can also feed [source views](#source-views-subsets-overlays-trees-and-checkout-capabilities)' `Subset` recipe to synthesize a tree without blob reads.
10. **[Content-keyed parse cache](#45-content-keyed-parse-cache).** Benefits from [subtree-aware fingerprints](#subtree-aware-fingerprints)' `blob:<sha>;m=<git-mode>` branch and [lazy clone](#3-lazy-clone-and-promisor-provider) because manifest blobs are fetched on demand. `fromTOML` follows only once the key includes parser feature state such as `Xp::ParseTomlTimestamps`.
11. **[Projection typeclass](#2-projection-typeclass).** Still a good refactor, but no longer a hard prerequisite for the monorepo path. Port domains incrementally after the behaviour-changing substrate is in place.
12. **Compute-in-progress registry** ([projection/cache enhancement](#2-projection-typeclass), "Concurrent-miss caveat"). Needed when `eval-cores>1` makes duplicate NAR walks or duplicate blob prefetches measurable.
13. **[Lazy attribute metadata](#lazy-attribute-scope-not-lazy-bytes).**
14. (Stretch) `LazyStorePathString` thunk from [Lazy Mounting and Deferred NAR Hash](#lazy-mounting-and-deferred-nar-hash). Rippling through `Value`-consumers (string interpolation, derivation hashing, JSON printing). Unlocks the full first-eval wire savings of [lazy clone](#3-lazy-clone-and-promisor-provider) + [subtree-aware fingerprints](#subtree-aware-fingerprints) combined when nothing forces the parent input's `outPath` string.

### Risks

- **Protocol-v2 provider surface area.** [Lazy clone](#3-lazy-clone-and-promisor-provider) requires implementing protocol-v2's `command=fetch` over Nix-owned HTTP framing. Pkt-line, sideband, stateless-RPC, and the `filter` argument have well-known shapes documented in upstream Git's [protocol-v2](https://git-scm.com/docs/protocol-v2), but Nix still owns the parser. Mitigation: integration tests against the major server implementations (GitHub, GitLab, Gitea, cgit, libgit2's own test fixtures) on every change; conformance tests against `git fetch` ground truth.
- **libgit2 ships native partial-clone.** Low likelihood (see [§10](#10-what-this-proposal-does-not-do)), but if it lands before this implementation stabilises, duplicated fetch paths could conflict. Mitigation: keep [lazy clone](#3-lazy-clone-and-promisor-provider) behind a small provider interface (`ensureObjects`, `initialFetch`) so native libgit2 support can replace only that implementation. Lock files are still compatible if both paths materialise the same content and therefore the same NAR hash.
- **`Projection<>` typeclass migration.** Existing ten cache domains need to be ported. Mitigation: incremental — port one domain at a time, old code-path remains until last domain is ported. This is now a refactor track, not a prerequisite for lazy clone.
- **Source-view API shape.** [Source views](#source-views-subsets-overlays-trees-and-checkout-capabilities) model concepts that upstream Nix cannot currently express, but this proposal keeps that model internal. Risk: later public exposure could leak tecnix-specific nouns or confuse local provenance with content identity. Mitigation: keep the C++ `ViewRecipe` / `SourceViewIdentity` semantics independent of any future spelling, require public API review before exposing it, and test the same core through tecnix's arbitrary-tree, dirty-zone, sparse-root, and physical-root cases.
- **Git-shaped store registration.** [Git-shaped store registration](#git-shaped-store-registration) crosses from eval into store/daemon semantics. Risk: Git tree identity and NAR identity diverge in tooling assumptions, binary caches, and protocol serving. Mitigation: first use Git identity only as an eval-side/lazy-store-path key; gate final no-NAR registration behind explicit `FileSerialisationMethod::Git` tests and compatibility checks for local, remote, binary-cache, and daemon paths.
- **Filtered-shape cache still runs filters.** If filter evaluation itself dominates runtime, [filtered-shape cache](#filtered-shape-cache) only removes the subsequent content walk/copy. That limitation is accepted in this proposal; the design deliberately avoids trying to identify filter functions.
- **Lazy fetch + offline evaluation.** If a user goes offline mid-eval and a blob is missing, the eval fails with a network error. Mitigation: same as today's "rev not reachable" failure mode; surface a clear error.
- **Lazy `mountInput` dryRun is a real semantic change.** Skipping newly-discovered eager `narHash` computation until something forces it has knock-on effects on lock-file write timing. The mitigation is to preserve the same mismatch check at the first point the actual NAR hash is required (lock write, explicit `narHash`, URL rendering, or devirtualisation), while deferring only the unconditional "compute a fresh narHash for this mounted input" path. Until this lands, [lazy clone](#3-lazy-clone-and-promisor-provider) is bottlenecked by the dryRun walk on first eval — it saves blobs the evaluator never looks at, but the dryRun does look at every blob.

---

## 10. What this proposal does *not* do

Scope boundaries:

- **Does not contribute partial-clone to libgit2.** [Lazy clone](#3-lazy-clone-and-promisor-provider) is a Nix-side implementation. Upstream libgit2 partial-clone remains the long-term path, but [libgit2#5564](https://github.com/libgit2/libgit2/issues/5564) has been open since 2020, related PRs have not landed, and local source inspection shows no protocol-v2 fetch implementation. Nix should therefore own a small provider interface that can be replaced if libgit2 later grows native support.
- **Does not introduce async IO.** The evaluator remains thread-per-task; this proposal removes the State-lock bottleneck. True async (`io_uring`, futures) would require an evaluator rewrite far beyond this proposal.
- **Does not require a daemon protocol change for eval-side lazy clone.** [Lazy clone](#3-lazy-clone-and-promisor-provider)'s missing-object fetches happen on the eval side; the daemon can still receive fully-materialised store paths. Full no-NAR transfer for Git-shaped store objects is [Git-shaped store registration](#git-shaped-store-registration) and may require a store/daemon protocol extension; that is part of making "Git is a serialization method" real, not part of the protocol-v2 fetcher.
- **Does not change existing public API semantics or add builtin functions.** `builtins.path`, `builtins.fetchTree`, `builtins.filterSource`, and existing flake-input attributes keep their current meaning. `SourceViewAccessor` is internal substrate; arbitrary tree handles, dirty overlays on clean revs, sparse checkout provenance, and physical checkout roots are modelled so the implementation can generalise, but this proposal does not introduce a new builtin function.
- **Does not depend on nixpkgs library code.** [Filtered-shape cache](#filtered-shape-cache) observes the output of any `builtins.path { filter = ...; }` call, regardless of whether the filter came from `lib.cleanSource`, `lib.fileset.toSource`, or user code.
- **Does not change the unit of Input identity.** Layer 1 of subtree-as-Input — the subtree as the unit of lock-file identity, with subtree narHash and isolated subtree store path — is feasible. The `mountInput` narHash gate is scope-symmetric and admits a subtree-rooted accessor producing a subtree-rooted narHash; substituters serve NARs by hash without opinion on whole-vs-subtree. The required work is a lock-file migration, fetcher plumbing, a migration story for `${input.outPath}/sub` patterns, and a derivation-hash transition for every existing nixpkgs derivation that uses such interpolation. That is a separate RFC. Layer 2 (the existing `dir` user-visible pointer with whole-repo narHash, accelerated by [subtree-aware fingerprints](#subtree-aware-fingerprints) on the `builtins.path` code path) is what this proposal delivers. See [§4](#4-subtree-fetching).
- **Does not make local checkout provenance content-addressed.** `SourceView` deliberately separates identity from provenance. A dirty overlay may synthesize a content-addressed tree after dirty blobs and whiteouts are known; sparse checkout roots, physical checkout paths, and "is this path present in my checkout?" remain local capabilities. They can be exposed and cached within an eval session, but they cannot become substituter keys.
- **Does not invent a new memoisation algebra.** The substrate is Build Systems à la Carte's `Task c k v` + `Store i k v` plus Adapton/Salsa/Skyframe's articulation-point-with-demand-driven-force model, restricted to content-addressed keys (so the dirty-propagation engine those systems carry is unnecessary). The proposal's `Projection<From, To>` is structurally a SkyFunction / Salsa query / Adapton articulation; master's existing `fetchers::Cache` rows and `CAFixed` output paths are the nearest in-tree instances. `SingleDerivedPath`/`Realisation` is adjacent output-realisation machinery: relevant to a true eval-layer floating handle or deferred NAR materialisation, but not the main input-keyed projection algebra. The contribution is the *Nix-side constraints* (compatibility with existing APIs, no nixpkgs cooperation, no system git, explicit source-local capabilities) under which the substrate has to fit, not the substrate itself. See [§0](#0-the-underlying-pattern-content-addressed-eval-time-projection) for the BSàlC selective-task classification and the prior-art citations.
- **Does not claim Merkle-category or Merkle-lattice structure.** Direct literature search surfaces no published categorical formalisation of Merkle DAGs; the literature treats them operationally. The proposal adopts a pragmatic engineering stance.
- **Does not implement cross-machine cache merge.** The persistent fetcher-cache is structurally a bounded join-semilattice for content-keyed domains under content-determinism ([§0](#0-the-underlying-pattern-content-addressed-eval-time-projection)). A `nix fetcher-cache merge` operation would: pin the cache schema version (currently `v4`); use `INSERT OR IGNORE` on the `(domain, key)` primary key for content-keyed rows; apply `max(timestamp)` and prefer rows with `immutableUrl` for `tarball`/`file` time-bound rows; report value mismatches with Nix-version / fetcher-policy context (since not all mismatches prove soundness-law violations — some are version skew or LFS-opt-in differences). Bazel RE's CAS sharing is the partial production precedent — but Bazel CAS keys are content hashes (digests are guaranteed unique-by-construction), whereas Nix fetcher-cache keys are *requests* (URLs, refs, treeShas), so conflict-freeness for Nix depends on the soundness law plus version alignment. Out of scope for this proposal; future RFC.
- **Does not unify the `tarball-cache-v2` and `gitv3` ODBs.** The split between the shared tarball ODB and per-URL git ODBs is historical and quantifiable; for users with both `github:NixOS/nixpkgs/<rev>` and a local-clone override, the worst-case duplication is around 7% of nixpkgs disk. The unification is small (one `objects/info/alternates` file write per `gitv3/<hash>/` repo, pointing at `tarball-cache-v2/objects`); libgit2's alternates are read-only and consulted after main backends, so the 2018 fetch-time-performance regression that drove the per-URL split does not recur. Without [lazy clone](#3-lazy-clone-and-promisor-provider), the dedup benefit is read-only (the per-URL fetch still receives a full pack; disk duplication unchanged). With [lazy clone](#3-lazy-clone-and-promisor-provider)'s provider, the alternate becomes a bandwidth win: `--filter=blob:none` clones backfill blobs only when neither the per-URL pack nor the `tarball-cache-v2` alternate has them. ODB alternates are therefore staged as future work after lazy clone ships. See [§3](#3-lazy-clone-and-promisor-provider)'s Scope paragraph for the historical context.

---

## 11. Summary

The proposal is organised around named implementation tracks rather than lettered labels: [projection typeclass](#2-projection-typeclass), [lazy clone and promisor provider](#3-lazy-clone-and-promisor-provider), [subtree-aware fingerprints](#subtree-aware-fingerprints), [content-keyed parse cache](#45-content-keyed-parse-cache), [source views](#source-views-subsets-overlays-trees-and-checkout-capabilities), [filtered-shape cache](#filtered-shape-cache), [tree-SHA to NAR-hash bridge](#tree-sha-to-nar-hash-bridge), [Git-shaped store registration](#git-shaped-store-registration), [lock-free reads](#split-the-state-lock-from-the-odb-read), [lazy attribute metadata](#lazy-attribute-scope-not-lazy-bytes), and [CLI devirtualisation](#cli-devirtualisation).

The monorepo-critical path is **`CURLOPT_POSTREDIR` → lazy narHash → Nix-side protocol-v2 provider → `ensureObjects`/`prefetchSubtree` coalescing → SourceView core/Git tree identity → readBlob lock split → subtree/blob fingerprints**, with [Git-shaped store registration](#git-shaped-store-registration), [filtered-shape caching](#filtered-shape-cache), and [parse-cache](#45-content-keyed-parse-cache) as the next user-visible cache wins.

What this proposal **does not** solve: nixpkgs-wide migration off `${input}/sub` interpolation in `mkDerivation { src = ...; }` ([§4](#4-subtree-fetching) — separate RFC because the rebuild scope at the derivation level is a release-coordination event for nixpkgs, not a localised optimisation, and the bare-string-interpolation idiom routes through `ensureLazyPathsCopied` rather than `addPath`). The proposal now does address tecnix's dirty/sparse/local-checkout/arbitrary-tree surface through [source views](#source-views-subsets-overlays-trees-and-checkout-capabilities), but with an explicit semantic split: persistent content identities are shareable; local checkout provenance is a capability and stays local. Full first-eval wire savings under [lazy clone](#3-lazy-clone-and-promisor-provider) depend first on lazy `narHash` in `mountInput`; `LazyStorePathString` remains the later stretch for cases that also avoid forcing `outPath`.

The user writes idiomatic Nix for existing fetch/path/filter/read workloads. Lazy clone comes from the git remote path; internal source views, subtree/blob fingerprints, filtered-shape caching, parse-caching, and concurrent reads apply where the accessor exposes content identity. Cross-rev subtree dedup uses the `builtins.path` code path with [subtree-aware fingerprints](#subtree-aware-fingerprints), and the tecnix migration is conditional on declaring `worldRoot` as a flake input so `getAccessorFromCommit` returns a `GitSourceAccessor` and source views can see tree/blob identity internally. FS-path interpolation through `PosixSourceAccessor` does not benefit until bytes are read once. Per-builtin mapping is in [§4](#per-builtin-migration-mapping-tecnix-specific). Each stage ships independently; wins compound.
