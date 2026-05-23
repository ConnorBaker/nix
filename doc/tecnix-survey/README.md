# Tecnix vs Determinate Systems Nix vs Upstream Nix

Tecnix's eleven `__unsafeTectonixInternal*` builtins are user-side compensation for three abstractions Nix already half-implements internally but doesn't expose: a tree-SHA → NAR-hash projection (used for tarballs but not arbitrary git subtrees), per-subpath mounting (used for git submodules but not language-visible), and overlays with whiteouts (re-implemented in tecnix's `DirtyOverlaySourceAccessor`). §8 is the surfacing wishlist; §4 maps each tecnix patch to the missing abstraction it compensates for; §5 catalogues twelve accessor-algebra asymmetries (plus four symmetric operations for completeness) that motivate parts of it; §6 is the empirical evidence.

Every claim is grounded in code I read end-to-end[^codebases] or behaviour I reproduced empirically.[^empirics] Limits of the analysis in §8.

[^codebases]: Trees surveyed (commit-pinned permalinks throughout): [NixOS/nix@2d309b18e](https://github.com/NixOS/nix/tree/2d309b18e5a3a8734f4e64e659dac1813964c241) for upstream master, with merge-base to tecnix at [NixOS/nix@39e6f6677](https://github.com/NixOS/nix/commit/39e6f667750a867a6f23f37a6b1fe6607fa60f01) (2026-03-02); [DeterminateSystems/nix-src@11f3aff90](https://github.com/DeterminateSystems/nix-src/tree/11f3aff904f84ae612e36e8bc578ac421fca74fa) for the DetSys fork (post-v3.21.0); [Shopify/tecnix@45d9c6f4a](https://github.com/Shopify/tecnix/tree/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f) for tecnix, branched from DetSys release [v3.20.0 = f0ccb960d](https://github.com/DeterminateSystems/nix-src/tree/f0ccb960d3ad5bff28acd9cabf8bdef885b5d52f). Tecnix is 109 commits behind current DetSys main. Citations of code below use commit-pinned URLs so they don't drift.

[^empirics]: Empirical evidence generated against system Nix `Determinate Nix 3.20.0` (tecnix's base) with `lazy-trees = true` set in `/etc/nix/nix.conf` (Determinate installer's default). Test repository at `/tmp/tecnix-experiment/repo`.

---

## 1. The question

Determinate Systems' lazy-trees is a lazy filesystem with materialised views over content-addressed sources. Tecnix is using it to back a monorepo. What would upstream Nix need to support these use-cases natively, without the bespoke `__unsafeTectonixInternal*` builtins?

The answer is structured as: §2 establishes the substrate (the layered model, the L2 design split, the `SourceAccessor` algebra, and a concrete map of where each cache and accessor lives) needed to read the rest. §3 inventories tecnix's patches and §4 maps each one to a missing abstraction — the document's payoff table. §5 catalogues sixteen accessor-algebra rows (twelve real asymmetries plus four symmetric operations for completeness) — the codified evidence behind the missing abstractions. §6 shows the empirical reproduction of five findings. §7 is the patterns-Nix-is-missing rollup. §8 is the priority-ranked wishlist. §9 is what I did not verify.

---

## 2. Setup: the layered model

### 2.1 Three layers

Nix's evaluator-and-build pipeline operates on three layers:

- **L1 — virtual filesystem.** A `SourceAccessor` is a read-only filesystem interface (`readFile`, `lstat`, `readDirectory`, `readLink`, plus metadata projections `getFingerprint`, `getProvenance`, `getLastModified`). Concrete sources include the real POSIX filesystem, an in-memory tree, a bare git ODB rooted at a tree or commit, an unpacked NAR archive, and the Nix store viewed as a directory.

- **L2 — virtual store.** `MountedSourceAccessor` is a `path → SourceAccessor` table. `EvalState::storeFS` mounts at `/nix/store` and the eval-time root accessor unions it with the real POSIX filesystem. When a `fetchTree` result needs a store path before its content has been materialised, `EvalState::mountInput` mounts the input's accessor at a *virtual store path* on `storeFS`. Reads to that path during evaluation route through the mount and the underlying L1 accessor. Two competing implementations of "what is a virtual store path" exist (see §2.3).

- **L3 — real store.** `/nix/store/<hash>-<name>` directories on disk, plus the daemon protocol for cross-machine operations. The daemon has no concept of L2; every L2 path must be materialised to L3 before it can cross the build, daemon, or `nix copy` boundary.

A virtual L2 path is "made real" by a *materialise on demand* function — `devirtualize` on DetSys/tecnix, `ensureLazyPathCopied` on master. Both look up the L2 mount and run `fetchToStore` to copy its content into L3.

### 2.2 The `SourceAccessor` algebra

The L1 layer has a small algebra. Three categories of accessor:

- **Sources** (no `next` pointer): `PosixSourceAccessor`, `MemorySourceAccessor`, `GitSourceAccessor`, `NarAccessor`, `RemoteFSAccessor`, `LocalStoreAccessor`, plus `WholeStoreViewAccessor` (test-only).
- **Wrappers** (one `next`): `FilteringSourceAccessor`, `AllowListSourceAccessor`, `CachingFilteringSourceAccessor`, `GitExportIgnoreSourceAccessor` are common to all three trees. `ForwardingSourceAccessor` and `OverrideProvenanceSourceAccessor` exist only in DetSys/tecnix.[^forwarding-detsys] `CachingSourceAccessor` exists only in master.[^caching-master] `DirtyOverlaySourceAccessor` exists only in tecnix.
- **Combinators** (multiple `next`): `MountedSourceAccessor` (a `path → accessor` table; nearest mount wins) and `UnionSourceAccessor` (first-with-path wins for files; directory listings *merge* with first-wins on conflicts).

[^forwarding-detsys]: [DetSys:forwarding-source-accessor.hh](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libutil/include/nix/util/forwarding-source-accessor.hh) and [DetSys:override-provenance-source-accessor.hh](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libutil/include/nix/util/override-provenance-source-accessor.hh) exist in DetSys main. Master has neither — verified by greppping `ForwardingSourceAccessor` and `OverrideProvenance` across [NixOS/nix@2d309b18e](https://github.com/NixOS/nix/tree/2d309b18e5a3a8734f4e64e659dac1813964c241) returning nothing.

[^caching-master]: [NixOS/nix:caching-source-accessor.cc](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil/caching-source-accessor.cc) exists in master, instantiated at [eval.cc#L281](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/eval.cc#L281) as part of the rootFS construction. Memoises `lstat` and `readLink` results in `boost::concurrent_flat_map`. Not present in DetSys main. Forwards `getFingerprint` but not `getProvenance` or `getLastModified` — see §5.11.

The composition operators are `mount` (table construction), `union` (list construction), `filter` (unary predicate gating), `prefix` (constant path translation, expressed as a field on `FilteringSourceAccessor`), and tecnix's `overlay` (per-file routing with whiteouts).

**Zero combinator tests.** Greppping [NixOS/nix:src/libutil-tests/](https://github.com/NixOS/nix/tree/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil-tests), [src/libfetchers-tests/](https://github.com/NixOS/nix/tree/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers-tests), [src/libstore-tests/](https://github.com/NixOS/nix/tree/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libstore-tests) for `MountedSource`, `UnionSource`, `FilteringSource`, `DirtyOverlaySource` returns no tests of any combinator's behaviour. (Same for [DetSys main](https://github.com/DeterminateSystems/nix-src/tree/11f3aff904f84ae612e36e8bc578ac421fca74fa).) The composition rules are hand-written without contract tests for symmetry. This alone explains many of the asymmetries in §5.

### 2.3 The L2 design split[^paths-bytes]

[^paths-bytes]: `git diff` between [DetSys:paths.cc](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libexpr/paths.cc) and [Shopify/tecnix:paths.cc](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/paths.cc) returns nothing — byte-identical. The lazy-trees mechanism in tecnix is *entirely inherited from DetSys*; tecnix's additions are in `eval.cc` and are zone-specific.

Master and DetSys disagree on what an L2 virtual store path *is*. Three modes have shipped:

| Tree | Store path returned | NAR hash | File copy |
| --- | --- | --- | --- |
| Master at merge-base `39e6f6677` | Real CA path | Eager (`Copy`) | Eager |
| DetSys + tecnix, `lazyTrees=false` | Real CA path | Eager (`Copy`) | Eager |
| Master post-`1d130492d` (Eelco, 2025-09)[^eelco] | Real CA path | Eager (`Copy`) | Eager |
| **Master post-`891ef140b` (Sergei, 2026-04)**[^sergei] | **Real CA path** | **Eager (`DryRun` walk)** | **Lazy** |
| **DetSys + tecnix, `lazyTrees=true`** | **`StorePath::random(name)`** | **Deferred** | **Deferred** |

[^eelco]: [NixOS/nix#14050](https://github.com/NixOS/nix/pull/14050) "Mount inputs on storeFS to restore fetchToStore() caching" (2025-09-22), commit [1d130492d](https://github.com/NixOS/nix/commit/1d130492d743345715107d24f0204fda19896db1). Per the commit message: with composite accessors becoming the norm, `fetchToStore` caching broke because the accessor's `fingerprint` field was empty. The fix added subpath-aware `getFingerprint(CanonPath)` and the mount-on-storeFS pattern. Not a laziness feature — pure cache-correctness.

[^sergei]: [NixOS/nix@891ef140b](https://github.com/NixOS/nix/commit/891ef140b8564a7848a3d75976e172c3e15ec14b) "Don't copy flakes to the store unnecessarily" (Sergei Zimmerman, 2026-04-19, co-authored by Eelco Dolstra). Verbatim from the commit message: *"This repurposes a slightly less lazy (but also more deterministic) approach than determinate nix has taken. We do still pay to cost of hashing an input once to compute the store path and narHash daemon-client-side."* Full patch flips `mountInput` from `Copy` to `DryRun`, adds `ensureLazyPathCopied`/`ensureLazyPathsCopied`, threads a `CopyLazyPaths` enum through `realisePath`, and adds backwards-compat for `getFlake` with discarded string context (similar to detnix). Implementation is small because Eelco's PR #14050 had already introduced `mountInput` as caching plumbing.

For tecnix's "thousands of zone mounts where most are never devirtualized":

- **Master (Sergei)**: every zone mount pays one full NAR-hash walk of the subtree, regardless of materialisation.
- **DetSys (`lazyTrees=true`) + tecnix**: zone mounts cost essentially nothing; NAR hash and copy both deferred.

Porting tecnix to current upstream master *without* a `lazyTrees`-equivalent mode would regress evaluation cost.

### 2.4 The CLI always devirtualizes

A subtle point: `nix eval` and `nix-instantiate` both materialise every L2 path before printing.[^cli-devirtualize] So virtual paths are nearly impossible to observe through normal CLI usage. The win for tecnix is at *eval time*: the `MountedSourceAccessor` serves reads through the underlying `GitSourceAccessor` without a copy, but only paths that don't appear in any final output stay virtual.

[^cli-devirtualize]: Both master and DetSys materialise at CLI output, with different function names: DetSys [eval.cc#L115](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/nix/eval.cc#L115) calls `state->devirtualize(...)`; [nix-instantiate.cc#L61](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/nix/nix-instantiate/nix-instantiate.cc#L61), [#L69](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/nix/nix-instantiate/nix-instantiate.cc#L69), [#L72](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/nix/nix-instantiate/nix-instantiate.cc#L72) do the same. Master uses `ensureLazyPathsCopied(context)`: [eval.cc#L126](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/nix/eval.cc#L126), [nix-instantiate.cc#L98](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/nix/nix-instantiate/nix-instantiate.cc#L98), [app.cc#L98](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/nix/app.cc#L98). So `nix eval -f x.nix --raw` of `someInput.outPath` always materialises that input. Confirmed empirically — the eval logs show `hashing` followed by `copying` even for a bare `.outPath` access.

This means: virtual paths are an *evaluator-internal* optimisation. They never escape to user-visible CLI output. Whether tecnix's "lazy" mode is faster than master's "lazy" mode depends on whether the saved NAR-hash walks are dominated by paths that *do* end up in final output (which would be hashed anyway).

### 2.5 Where the caches and accessors live

The L1/L2/L3 picture above is the type level. Concretely, Nix maintains several on-disk stores plus several in-process structures, each mapped to a layer:

| What | Where | Lifetime | Identity |
| --- | --- | --- | --- |
| Real working tree, build outputs | `/`, `/tmp/...` | Filesystem | Path on disk |
| **Tarball cache** (a bare git ODB)[^tarball-cache-loc] | `~/.cache/nix/tarball-cache-v2` | Filesystem | git tree-SHA |
| **Per-URL git fetch cache**[^gitv3-cache] | `~/.cache/nix/gitv3/<hashed-url>/` | Filesystem | URL → bare git repo |
| **Tecnix's worldRepo** | `~/world/git` (configurable) | Filesystem | Same as the above two: a bare git ODB |
| **Fetcher-cache projection memo** | `~/.cache/nix/fetcher-cache-v4.sqlite` | Filesystem | One key-attr-set domain per projection (10+) |
| **L1 `SourceAccessor` instance** | RAM | EvalState | Accessor pointer + opaque `fingerprint` field |
| **L2 `storeFS` mount table** | RAM (`MountedSourceAccessor`) | EvalState | Synthetic store path → L1 accessor |
| **Tecnix's `tectonixZoneCache_`** | RAM (`SharedSync<map<Hash, StorePath>>`) | EvalState | tree-SHA → synthetic store path |
| **L3 real store** | `/nix/store` | Filesystem | Content-addressed store path |
| **Eval cache** | `~/.cache/nix/eval-cache-v6/<flake-fp>.sqlite` | Filesystem | Flake fingerprint → cached evaluated values (orthogonal to L1/L2/L3) |

[^tarball-cache-loc]: Opened via `Settings::getTarballCache()` at [DetSys:git-utils.cc#L1514-L1525](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/git-utils.cc#L1514-L1525) (master [#L1493-L1502](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git-utils.cc#L1493-L1502)). Bare repo, packfiles-only, populated by `tarball.cc` on tarball download and by `github.cc` on archive-tarball fetches.

[^gitv3-cache]: `getCachePath(url, shallow)` returns `~/.cache/nix/gitv3/<hashed-url>/`: master at [git.cc#L36-L40](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git.cc#L36-L40), DetSys at [git.cc#L41-L45](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/git.cc#L41-L45). Used by the git fetcher when fetching by URL.

Two observations follow:

**Three on-disk bare-git ODBs of the same shape, three different lifetimes.** The tarball cache, the per-URL gitv3 cache, and tecnix's worldRepo are all bare git repositories accessed via `GitSourceAccessor` and identified by tree-SHA. They differ only in *where* they sit and *who* writes to them. The mechanism is uniform.

**The fetcher-cache sqlite is the L1↔L2 projection layer.** Each cache "domain" memoises one projection between identity schemes. `treeHashToNarHash` is the canonical example: tree-SHA → NAR hash. `sourcePathToHash` (used by `fetchToStore`) is the most general: `(fingerprint, method, subpath) → NAR hash`. The ten domains visible in §7.3's footnote are ten ad-hoc instances of the same projection pattern. **Tecnix's `tectonixZoneCache_` is a fourth instance — same shape, EvalState lifetime instead of persistent — because the persistent projection layer isn't reachable from user-space code.**

This is the bridge between the abstract L1/L2/L3 model in §2.1 and the missing-abstractions catalogue in §6/§7: the surfacing work in §8 Tier 1 #3 ("expose `treeHashToNarHash` for arbitrary git subtrees") is, mechanically, "let user-space tree-SHA → store-path projections write to the same fetcher-cache sqlite that the tarball cache already writes to."

---

## 3. Tecnix patch set

`git diff --stat detsys/main...tecnix-gcs` reports 5,600+ added / 635 removed lines across 54 files. Tecnix's two design documents — `plans/tectonix/lazy-trees.md` (582 lines) and `plans/tectonix/testing.md` (763 lines) — record architectural intent and the deviations from that intent that landed.

### 3.1 Eleven new builtins[^primops-count]

[^primops-count]: Verified count: `grep -c "RegisterPrimOp primop_" src/libexpr/primops/tectonix.cc` returns 11. Source: [Shopify/tecnix:src/libexpr/primops/tectonix.cc](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/primops/tectonix.cc).

In [Shopify/tecnix:src/libexpr/primops/tectonix.cc](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/primops/tectonix.cc) (418 LOC), parameterised by global settings, none exposed via `InputScheme`:[^no-scheme]

[^no-scheme]: `grep registerInputScheme [Shopify/tecnix:src/libfetchers/](https://github.com/Shopify/tecnix/tree/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libfetchers)` returns no tectonix-typed scheme. The world repo is identified by global settings, not as a flake input.

| Builtin | Returns | Effect |
| --- | --- | --- |
| `__unsafeTectonixInternalManifest` | attrset path → `{id}` | Parse `.meta/manifest.json` from world commit |
| `__unsafeTectonixInternalManifestInverted` | attrset id → path | Inverse; throws on duplicate ids |
| `__unsafeTectonixInternalTreeSha worldPath` | git tree SHA string | Walk to subdirectory, cache per component |
| `__unsafeTectonixInternalTree treeSha` | store path | Eager `fetchToStore` of arbitrary tree |
| `__unsafeTectonixInternalZoneSrc zonePath` | store path string | Lazy when `lazy-trees=true` |
| `__unsafeTectonixInternalZonePath zonePath` | path value | Same content, path-typed[^zonepath] |
| `__unsafeTectonixInternalSparseCheckoutRoots` | list of zone IDs | Reads `.git/info/sparse-checkout-roots` |
| `__unsafeTectonixInternalDirtyZones` | attrset zonePath → bool | `git status --porcelain -z` |
| `__unsafeTectonixInternalZoneIsDirty zonePath` | bool | Single-zone check |
| `__unsafeTectonixInternalZoneRoot zonePath` | string \| null | Path to zone in checkout |
| `__unsafeTectonixInternalGitSha` | string | Configured `--tectonix-git-sha` |

[^zonepath]: Returns a path-typed Nix value via `v.mkPath(state.storePath(storePath), state.mem)` rather than a string ([Shopify/tecnix:src/libexpr/primops/tectonix.cc#L225](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/primops/tectonix.cc#L225)). Significant because it points at an existing primitive — see §7.

### 3.2 EvalState additions

[Shopify/tecnix:src/libexpr/eval.cc](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/eval.cc) gains 623 LOC; [eval.hh](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/include/nix/expr/eval.hh) gains 114. Behind `std::call_once`:

- `worldRepo`, `worldGitAccessor` — the world git ODB and a cached commit accessor.
- `worldTreeShaCache` — `path → tree SHA`, populated component-by-component.
- `tectonixSparseCheckoutRoots`, `tectonixDirtyZones`, `tectonixManifestContent`/`tectonixManifestJson` — caches.
- `tectonixZoneCache_: SharedSync<map<Hash, StorePath>>` — **tree SHA → virtual store path**. The central caching mechanism.
- `tectonixCheckoutZoneCache_: SharedSync<map<string, StorePath>>` — zone path → virtual store path for dirty zones.

Three coordinator methods: `getZoneStorePath`, `mountZoneByTreeSha`, `getZoneFromCheckout`.

### 3.3 Settings

Three new (`tectonix-git-dir`, `tectonix-git-sha`, `tectonix-checkout-path`) and one default flip. Across the four trees:[^lazy-trees-defaults]

[^lazy-trees-defaults]: Setting locations: [DetSys main:eval-settings.hh#L470](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libexpr/include/nix/expr/eval-settings.hh#L470) defaults to `false`; [DetSys v3.20.0:eval-settings.hh](https://github.com/DeterminateSystems/nix-src/blob/f0ccb960d3ad5bff28acd9cabf8bdef885b5d52f/src/libexpr/include/nix/expr/eval-settings.hh) — same; [Shopify/tecnix:eval-settings.hh#L470](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/include/nix/expr/eval-settings.hh#L470) flips to `true` in commit [d1ea942f5](https://github.com/Shopify/tecnix/commit/d1ea942f554ff3d5339f9fbdf4c45b5863a81f42); upstream master has no `lazyTrees` setting at all (verified absent from [NixOS/nix:eval-settings.hh](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/include/nix/expr/eval-settings.hh)). On my system: `nix show-config | grep lazy-trees` reports `lazy-trees = true` because `/etc/nix/nix.conf` has `lazy-trees = true` set by the Determinate installer.

| Tree | source default for `lazy-trees` |
| --- | --- |
| DetSys main | `false` |
| DetSys v3.20.0 | `false` |
| Tecnix | `true` |
| Upstream master | (no setting) |

### 3.4 libfetchers/git extensions

Three new `GitRepo` virtuals: `getSubtreeSha`, `getCommitTree`, `getGitAttributesAlongPath`. `GitAccessorOptions` gained `attrCommitRev`, `attrPathPrefix`, `attrFingerprint` so tree-rooted accessors can honour `.gitattributes` (which needs commit context that a tree-rooted accessor lacks).

[Shopify/tecnix:git.cc](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libfetchers/git.cc) gained a **`gitRevUrl` cache** keyed on `(rev, url, submodules, exportIgnore, lfs)`. Master and DetSys differ on git accessor caching: DetSys caches the legacy `git archive` fallback path while master has no cache there at all.[^gitcache-comparison] Neither has a modern-libgit2 accessor cache — that's tecnix's addition.

[^gitcache-comparison]: Master has only `gitLastModified` ([NixOS/nix:git.cc#L719](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git.cc#L719)) and `gitRevCount` ([NixOS/nix:git.cc#L739](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git.cc#L739)) caches. DetSys main additionally caches the legacy `git archive` fallback path by reusing the `sourcePathToHash` domain with a `;legacy` fingerprint suffix: suffix construction at [DetSys:git.cc#L831](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/git.cc#L831), cache-key construction at [#L834](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/git.cc#L834), lookup at [#L842](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/git.cc#L842). Tecnix introduces a new `gitRevUrl` cache domain at [Shopify/tecnix:git.cc#L829](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libfetchers/git.cc#L829) keyed on `(rev, url, submodules, exportIgnore, lfs)`.

### 3.5 Other patches

GCS support (~880 LOC + tests, independent of lazy-trees), git LFS hardening, git env hygiene (strip `GIT_DIR`/`GIT_WORK_TREE`/`GIT_COMMON_DIR` before `git status`), tracked-files indexing optimisation, `CachingFilteringSourceAccessor::cache` wrapped in `SharedSync` to fix a real concurrency hole,[^fsa-race] and `BASE_ENV_SIZE` 128 → 140 to fit the eleven new builtins.[^baseenv]

[^fsa-race]: Plain `std::map<CanonPath, bool> cache` in [NixOS/nix:filtering-source-accessor.hh#L98](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/include/nix/fetchers/filtering-source-accessor.hh#L98) and [DetSys:filtering-source-accessor.hh#L96](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/include/nix/fetchers/filtering-source-accessor.hh#L96), with unprotected `cache.find` / `cache.emplace` in [NixOS/nix:filtering-source-accessor.cc#L110](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/filtering-source-accessor.cc#L110) and [DetSys:filtering-source-accessor.cc#L122](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/filtering-source-accessor.cc#L122). Tecnix wraps in `SharedSync<std::map<CanonPath, bool>>` at [Shopify/tecnix:filtering-source-accessor.hh#L97](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libfetchers/include/nix/fetchers/filtering-source-accessor.hh#L97). Real race under `eval-cores > 1`. In the *same struct*, `allowedPrefixes` and `allowedPaths` are already `SharedSync`-wrapped; only the predicate-result memo wasn't.

[^baseenv]: [DetSys:eval.cc#L251](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libexpr/eval.cc#L251) shows `BASE_ENV_SIZE = 128`; [Shopify/tecnix:eval.cc#L253](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/eval.cc#L253) shows `140`.

---

## 4. Per-patch mapping: what each tecnix patch is really asking for

Each tecnix change maps to a missing or unsurfaced upstream abstraction. This is the document's central claim, with the supporting evidence collected in §5 (asymmetries), §6 (empirical), §7 (larger patterns), and the recommendations enumerated in §8.

| Tecnix patch | What's missing upstream | §7 pattern row | §8 wishlist item(s) |
| --- | --- | --- | --- |
| `tectonixZoneCache_`, `__unsafeTectonixInternalTreeSha`/`Tree`/`ZoneSrc`/`ZonePath` | Tree-SHA → NAR-hash projection isn't surfaced for arbitrary git subtrees; `fetchTree` can't return a sub-tree handle | §7.1 internal projection unsurfaced; §7.2 subtree mounts already work twice; §7.3 content-identity over-pluralised | Tier 1 #1 + #3, Tier 3 #14 + #15 |
| `DirtyOverlaySourceAccessor`, `__unsafeTectonixInternalDirtyZones`/`ZoneIsDirty` | No generic `OverlaySourceAccessor` combinator; no dirty-introspection on workdir Inputs | §7.6 wrapper-ergonomics gap | Tier 3 #13 |
| `__unsafeTectonixInternalManifest`/`ManifestInverted` | No parse-caching for pure expressions over input contents | §7.8 lift-to-Nix-attrset pattern | Tier 3 #16 + #17 |
| `lazy-trees=true` default flip | Master and DetSys disagree on what an L2 virtual store path *is* (§2.3) | §7.4 two laziness primitives, scoped differently | (design choice, not a Tier item) |
| `gitRevUrl` cache | One of §7.3's missing identity projections, surfacing as ten ad-hoc cache domains | §7.3 content-identity over-pluralised | Independent #18 |
| `attrFingerprint` plumbing in `GitAccessorOptions` | Tree-rooted accessors can't honour `.gitattributes` without commit context | (enabling change for Tier 1 #1) | Independent #20 |
| `CachingFilteringSourceAccessor::cache` → `SharedSync` | No coherent menu of synchronisation primitives | §7.5 sync primitives don't form a menu | Independent #19 |
| `BASE_ENV_SIZE` 128 → 140 | Mechanical consequence of adding 11 builtins | — | — |

The structural critique is not that tecnix did anything wrong. The critique is that the *generalising abstractions don't exist*; tecnix had to specialise. The rest of the document is the evidence behind each row.

---

## 5. Asymmetry catalogue

The composition rules for the L1 algebra are inconsistent across virtual methods. Twelve real asymmetries follow, plus a closing summary of four operations that *are* symmetric. **L** marks an asymmetry a current caller can hit; **D** marks dormant.

### 5.1 `getFingerprint` component composition — three different rules

When the combinator's own fingerprint is null:

| Combinator | Composition |
| --- | --- |
| `MountedSourceAccessor` | resolve `path` to `(child, subpath)`; return `child->getFingerprint(subpath)` |
| `UnionSourceAccessor` | walk children in order, return the first non-null; **no folding** |
| `FilteringSourceAccessor` | `next->getFingerprint(prefix / path)`; **predicate not folded in** |

All three combinators do check `if (fingerprint) return {path, fingerprint};` first.[^getfingerprint-impls] The asymmetry is in the fallback rule.

[^getfingerprint-impls]: `MountedSourceAccessor::getFingerprint` at [DetSys:mounted-source-accessor.cc#L95-L101](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libutil/mounted-source-accessor.cc#L95-L101) and [NixOS/nix:mounted-source-accessor.cc#L100-L106](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil/mounted-source-accessor.cc#L100-L106); `UnionSourceAccessor::getFingerprint` at [DetSys:union-source-accessor.cc#L82-L92](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libutil/union-source-accessor.cc#L82-L92) and [NixOS/nix:union-source-accessor.cc#L88-L98](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil/union-source-accessor.cc#L88-L98); `FilteringSourceAccessor::getFingerprint` at [DetSys:filtering-source-accessor.cc#L58-L63](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/filtering-source-accessor.cc#L58-L63) (same in master).

**D** in current code: only one Union exists (the eval-time root with `[posixFS, storeFS]`); only one fingerprint-holding `FilteringSourceAccessor` exists (`AllowListSourceAccessor` over a workdir), and its fingerprint is set externally by the `Input` layer using `Input::getFingerprint()` which already digests dirty file state via the `;d=<sha512>` suffix.[^d-suffix]

[^d-suffix]: `GitInputScheme::getFingerprint` digests dirty files and deleted files into a `;d=<hex>` suffix on the fingerprint: [NixOS/nix:git.cc#L1122](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git.cc#L1122) and [DetSys:git.cc#L1277](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/git.cc#L1277). The L1-level filter-predicate-not-in-fingerprint asymmetry is plugged at the L0 (Input) layer. **Pattern instance**: every level has its own way to incorporate variant state into identity, and they don't share a common abstraction.

### 5.2 `getProvenance` self-check — asymmetric

| Combinator | Self-check first? |
| --- | --- |
| `MountedSourceAccessor` | **No** — `inner->getProvenance(subpath)` directly |
| `UnionSourceAccessor` | **No** — walks children for first non-null |
| `FilteringSourceAccessor` | **Yes** — checks `provenance` first |

**D**: verified by greppping `provenance =` across `src/` of DetSys. The two relevant assignment sites at [DetSys:fetchers.cc#L331](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/fetchers.cc#L331) and [#L355](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/fetchers.cc#L355) target the inner accessor returned by the fetcher scheme, never a Mounted/Union directly. Master does not assign provenance on accessors at all (the entire `Provenance`-on-accessor mechanism is DetSys-specific).

### 5.3 `pathExists` override — asymmetric, **L**

`FilteringSourceAccessor::pathExists` is overridden as `isAllowed && next->pathExists(prefix/p)`; Mounted and Union inherit the default `maybeLstat(p).has_value()`. This is **L**, not D: multiple concrete sources override `pathExists` with cheaper paths than building a full `Stat`. `GitSourceAccessor::pathExists` at [NixOS/nix:git-utils.cc#L844](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git-utils.cc#L844) does an OID lookup and returns a `bool` directly; `MemorySourceAccessor::pathExists` calls `open(path, std::nullopt)`; `LocalStoreAccessor` and `dummy-store.cc` also override. Through Filtering, the optimisation reaches the source. Through Mounted or Union, the default re-routes to `maybeLstat`, which builds a full `Stat` struct unnecessarily. Performance bug.

### 5.4 `lstat` override — asymmetric

`MountedSourceAccessor` and `FilteringSourceAccessor` override; `UnionSourceAccessor` inherits the default. Different error messages for missing paths through different combinators. **L**: observable to users.

### 5.5 `invalidateCache` — divergent across master and DetSys

Different signatures across trees:

- **Master**: signature is `invalidateCache()` (no path). All three combinators consistently invalidate everything reachable.
- **DetSys**: signature is `invalidateCache(const CanonPath & path)`. Three different rules: Mounted is selective, Union fans out, Filter forwards with prefix translation. **Asymmetric.**[^invalidate-divergence]

[^invalidate-divergence]: Master at [source-accessor.hh#L237](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil/include/nix/util/source-accessor.hh#L237): `virtual void invalidateCache() {}`. DetSys at [source-accessor.hh#L228](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libutil/include/nix/util/source-accessor.hh#L228): `virtual void invalidateCache(const CanonPath & path) {}`. Tecnix tracks DetSys. Master fixed the asymmetry by making the operation whole-accessor.

### 5.6 `getLastModified` aggregation — absent

No combinator overrides it. Composite accessors return `nullopt` even when components have lastModified. The natural "max over components" rule is not implemented. **L**: callers depend on it via the Input layer instead, which has its own aggregation.

### 5.7 `showPath` shape — asymmetric

| Combinator | Formula |
| --- | --- |
| `MountedSourceAccessor` | `displayPrefix + inner->showPath(subpath) + displaySuffix` |
| `UnionSourceAccessor` | `displayAccessor->showPath(p)` if set, else first component, else default |
| `FilteringSourceAccessor` | `displayPrefix + next->showPath(prefix/p) + displaySuffix` |

Mounted and Filtering have identical shape. Union is bespoke.

### 5.8 `mount` on duplicate key — silent drop

`MountedSourceAccessor::mount` calls `concurrent_flat_map::emplace`,[^mount-emplace] which is no-op on existing key. Neither POSIX-`mount` semantics nor fail-loudly. **L** in DetSys: two `storeFS->mount` call sites — flake-schemas[^flake-schemas-mount] and `mountInput` — both happen to be content-equivalent so the silent drop is harmless. A future caller mounting non-content-equivalent accessors at the same path would be silently broken.

[^mount-emplace]: [DetSys:mounted-source-accessor.cc#L84](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libutil/mounted-source-accessor.cc#L84) and [NixOS/nix:mounted-source-accessor.cc#L89](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil/mounted-source-accessor.cc#L89): `mounts.emplace(std::move(mountPoint), std::move(accessor));`. `boost::concurrent_flat_map::emplace` is no-op on existing key.

[^flake-schemas-mount]: [DetSys:flake-schemas.cc#L28](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libcmd/flake-schemas.cc#L28) mounts a `MemorySourceAccessor` for the built-in flake schemas at a CA path computed from the same accessor — content-deterministic, idempotent. This file does not exist in upstream master.

### 5.9 `getProvenance` path translation — three different rules

Mounted passes `subpath`; Union passes `p` unchanged; Filtering passes `prefix / p`. Each translation is consistent with its combinator's read-path translation, but a caller computing what path the inner accessor will see has to know the routing rule.

### 5.10 `ForwardingSourceAccessor` is incomplete (DetSys only)

DetSys's [`ForwardingSourceAccessor`](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libutil/include/nix/util/forwarding-source-accessor.hh) forwards 6 methods (`readFile`, `maybeLstat`, `readDirectory`, `readLink`, `showPath`, `getPhysicalPath`); does **not** forward `getFingerprint`, `getLastModified`, `getProvenance`, `invalidateCache`. So a wrapper extending it silently drops the inner accessor's fingerprint and provenance and ignores invalidation.

`OverrideProvenanceSourceAccessor` extends it and adds a `getProvenance` override but not the others. **D**: both real-world uses[^override-uses] happen to wrap accessors with no fingerprint, so the dropped fingerprint is invisible. Master doesn't have either class — this asymmetry is structurally absent there.

[^override-uses]: `OverrideProvenanceSourceAccessor` is DetSys-only (not present in master). Its two real-world uses: [DetSys:primops.cc#L2946](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libexpr/primops.cc#L2946) (in `builtins.path` with filter — additionally, when a filter is applied, [DetSys:fetch-to-store.cc#L41](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/fetch-to-store.cc#L41) explicitly sets fingerprint to `nullopt`, bypassing the cache anyway) and [DetSys:prefetch.cc#L152](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/nix/prefetch.cc#L152) (over a `PosixSourceAccessor` which has no fingerprint).

### 5.11 `CachingSourceAccessor` (master only) silently drops provenance

Forwards `getFingerprint` ([NixOS/nix:caching-source-accessor.cc#L91-L94](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil/caching-source-accessor.cc#L91-L94)); does **not** override `getProvenance` or `getLastModified`. Wrapping a fingerprinted+provenance-bearing accessor in a `CachingSourceAccessor` (which the eval root does at [NixOS/nix:eval.cc#L281](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/eval.cc#L281)) silently drops the provenance.[^caching-source] **L** for master only — DetSys lacks this wrapper entirely.

[^caching-source]: [NixOS/nix:caching-source-accessor.cc](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil/caching-source-accessor.cc). The eval-root accessor stack on master is `(AllowList?)(Caching(Union(posixFS, storeFS)))`, where AllowList wraps only in `restrictEval` or `pureEval` mode (see [eval.cc#L268-L293](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/eval.cc#L268-L293)). Provenance from union components is lost through the `Caching` layer regardless. Currently the union components don't set provenance, so dormant — but a real codified asymmetry.

### 5.12 The L2 random-store-path hides materialisation state

A `StorePath::random(name)` and a real CA path are visually indistinguishable. No `StorePath::isVirtual()` exists; the only signal is `storeFS->getMount(path).has_value()`. The store layer has *no concept* of virtual paths;[^store-no-virtual] the `realiseContext` primop and the derivation primop both special-case via the mount-table lookup,[^realise-context] and the daemon worker protocol simply fails closed on virtual paths because it has no mount table to consult.

[^store-no-virtual]: Greppping `isLazy`, `isVirtual`, `getMount`, `storeFS` in `src/libstore/` and `src/libfetchers/` of [DetSys main](https://github.com/DeterminateSystems/nix-src/tree/11f3aff904f84ae612e36e8bc578ac421fca74fa) returns nothing. The store layer treats all paths uniformly; only `EvalState::storeFS` knows about mounts. Daemon-side: [DetSys:remote-store.cc#L173](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libstore/remote-store.cc#L173) `RemoteStore::isValidPathUncached` queries the daemon, which has no concept of virtual paths and would simply return false.

[^realise-context]: [DetSys:primops.cc#L94-L101](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libexpr/primops.cc#L94-L101) and master at [primops.cc#L91-L99](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/primops.cc#L91-L99): in `realiseContext`, `Opaque{path}` skips `ensureValid(p)` if `storeFS->getMount(printStorePath(p))` returns a mount. Comment text differs between trees but the behaviour is the same. This is the L2/L3 boundary. Sergei's commit added this check in master.

DetSys has `NixStringContextElem::Path` (in addition to `Opaque`, `DrvDeep`, `Built`) for partial mitigation — flagging when a virtual path leaks into a string context where it shouldn't via `builtins.toString`/`builtins.toFile`. Master has only the three original variants.

**Pure-eval/restricted-eval interaction**: virtual paths are allowlisted via `EvalState::allowPath` ([DetSys:eval.cc#L407](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libexpr/eval.cc#L407), [NixOS/nix:eval.cc#L393](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/eval.cc#L393)), which adds the prefix to `AllowListSourceAccessor::allowPrefix`. In *non-restricted* eval, `rootFS` is *not* an `AllowListSourceAccessor`, so `allowPath` is a no-op. Virtual paths work in unrestricted mode only because everything is allowed; in restricted mode they work because every `mountInput` call adds an allowlist entry. If any future code path created a virtual path without `allowPath`, restricted-mode users would see "path not allowed" errors with no actionable diagnostic.

### Symmetric operations

For completeness, four operations are consistent across all three combinators:

| Method | Behaviour |
| --- | --- |
| `getFingerprint` self-check | All three return `(path, this->fingerprint)` if non-null before delegating. (Composition is asymmetric — see §5.1.) |
| `getPhysicalPath` | All three: "find the first child with a physical path." |
| `union` flatness | `union(A, union(B, C))` and `union(A, B, C)` give the same observable read behaviour. (Fingerprints can differ — see §5.1.) |
| `mount` ordering on overlap | For non-overlapping mountpoints, irrelevant. For overlapping, see §5.8. |

---

## 6. Empirical evidence

### 6.1 Tree-SHA cache miss across revisions — confirmed

Test repository at `/tmp/tecnix-experiment/repo` with two commits sharing a stable subtree[^test-repo]:

[^test-repo]: Test repo construction: `git init repo; mkdir sub other; echo "stable content" > sub/file.txt; echo "v1" > other/file.txt; git commit; echo "v2" > other/file.txt; git commit`. Verified `git rev-parse <rev>:sub` is identical across both commits: `47665c66...` (the subtree SHA is stable; only the parent commit's `other/` differs).

```nix
let
  url = "file:///tmp/tecnix-experiment/repo";
  t1 = builtins.fetchTree { type = "git"; url = url; rev = "<rev1>"; allRefs = true; };
  t2 = builtins.fetchTree { type = "git"; url = url; rev = "<rev2>"; allRefs = true; };
in {
  sub1 = builtins.path { path = t1.outPath + "/sub"; name = "sub"; };
  sub2 = builtins.path { path = t2.outPath + "/sub"; name = "sub"; };
}
```

Both store paths resolve to `/nix/store/08ig...sub` — same hash, identical CA hash, identical narHash. **Final store path is shared.**

But the fetcher cache shows *two distinct rows*:

```
{"fingerprint":"git:17b4ccef...","method":"nar","path":"/sub"} → sha256-pQ3coAxD...
{"fingerprint":"git:f6b03487...","method":"nar","path":"/sub"} → sha256-pQ3coAxD...
```

Same content, **two cache rows**. The NAR was hashed twice from disk because the cache key includes the per-rev fingerprint.

### 6.2 String-interpolation hole — confirmed

`derivation { src = "${t1.outPath}/sub"; ...; }` produces a derivation whose `inputSrcs` is the **whole repo**. `derivation { src = builtins.path { path = t1.outPath + "/sub"; name = "sub"; }; ...; }` produces a derivation with `inputSrcs = ['08ig...sub']` — subtree only.[^interp-master]

[^interp-master]: Master at [primops.cc#L1759-L1762](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/primops.cc#L1759-L1762) derivation primop calls `state.ensureLazyPathCopied(o.path); drv.inputSrcs.insert(o.path);` for each `Opaque{path}`. `ensureLazyPathCopied` is at [paths.cc#L25](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/paths.cc#L25) and looks up the mount, then runs `fetchToStore(SourcePath{ref(mount)})` from `CanonPath::root` of the accessor. Master and DetSys are isomorphic on this seam.

### 6.3 Whiteout correctness in `DirtyOverlaySourceAccessor` — verified

Tecnix's overlay correctly handles whiteouts. Verified by reading the porcelain parser, `maybeLstat`, and `readDirectory` end-to-end:

- The porcelain parser at [Shopify/tecnix:eval.cc#L705-L751](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/eval.cc#L705-L751) inserts every status code's path into `dirtyFiles` regardless of XY: `D `, ` D`, `M`, `A`, `??`, `R`, `C`. Renames/copies emit two paths (source and destination), both inserted.
- `maybeLstat` at [eval.cc#L928-L938](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/eval.cc#L928-L938): for root, returns `base->maybeLstat`. For dirty paths, routes to `disk->maybeLstat` (returns nullopt for absent files = whiteout).
- `readDirectory` at [eval.cc#L955-L989](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/eval.cc#L955-L989): iterates `dirtyFiles` whose direct parent is the requested path. `disk->maybeLstat` returning nullopt → `entries.erase(rest)` (whiteout). New files appear via the same loop. New directories appear via `dirtyDirs` reconstructed in the constructor.

### 6.4 lazy-trees default behaviour — observed

```
$ nix --version
nix (Determinate Nix 3.20.0) 2.34.6
$ nix show-config | grep lazy-trees
lazy-trees = true
$ grep lazy-trees /etc/nix/nix.conf
lazy-trees = true
```

Source default `false`; Determinate installer ships override.

### 6.5 `treeHashToNarHash` cache is alive in production

On my system, after normal `fetchTree` use:

```
$ sqlite3 ~/.cache/nix/fetcher-cache-v4.sqlite "SELECT COUNT(*) FROM Cache WHERE domain='treeHashToNarHash'"
9
```

Real entries from real `fetchTree` of tarball/GitHub inputs. (The count depends on how much the user has fetched; the point is the domain is populated, not the specific number.)

---

## 7. Larger patterns Nix is missing or duplicating

Beyond the §5 catalogue, the codebase has several broader patterns each implemented multiple times in slightly different ways.

### 7.1 The internal tree-SHA → NAR-hash projection that already exists, but isn't surfaced

`treeHashToNarHash`[^treehash-impl] is a tree-SHA → NAR-hash projection persisted in the fetcher cache, used internally for tarballs and GitHub archives. The flow:

[^treehash-impl]: `treeHashToNarHash` definition at [DetSys:git-utils.cc#L758-L771](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/git-utils.cc#L758-L771) and master at [git-utils.cc#L742-L756](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git-utils.cc#L742-L756). Caches `("treeHashToNarHash", {treeHash}) → {narHash}` in the persistent fetcher cache. Use sites: tarball at [DetSys:tarball.cc#L527](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/tarball.cc#L527) (master [#L511](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/tarball.cc#L511)); GitHub fetcher at [DetSys:github.cc#L302](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/github.cc#L302), [#L326](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/github.cc#L326), [#L366](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/github.cc#L366) (master [#L289](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/github.cc#L289), [#L309](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/github.cc#L309), [#L345](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/github.cc#L345)). The tarball cache itself is a bare git repository at `~/.cache/nix/tarball-cache-v2`, opened via `Settings::getTarballCache()` at [DetSys:git-utils.cc#L1514-L1525](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/git-utils.cc#L1514-L1525) (master [#L1493-L1502](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git-utils.cc#L1493-L1502)).

1. Download tarball.
2. Unpack into a *bare git repository* at `~/.cache/nix/tarball-cache-v2`.
3. Identify by tree-SHA.
4. Cache `treeHash → narHash`.

Nix already operates an internal git ODB as a content-addressed tarball cache. Tecnix's `tectonixZoneCache_` is a re-implementation of the same projection scoped to the world repo, in-process only because the existing projection isn't reachable from user-space.

### 7.2 Subtree mounts already work in two places

Two existing mechanisms in upstream prove that the abstraction tecnix needs is already present, just not surfaced.

**`SourcePath` is already a sub-tree handle.** [NixOS/nix:source-path.hh](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil/include/nix/util/source-path.hh) is `(accessor, path)` with operators `/(CanonPath)` and `/(string_view)` for descent. It supports `readFile`, `readDirectory`, `dumpPath`, `getProvenance`, etc. — every projection of the underlying accessor, but *anchored at a sub-path*. The Nix `nPath` value type carries a `SourcePath` internally. **`fetchTree` could return a `nPath` value** (instead of an attrset with `outPath` as a string) and `(fetchTree input) / "sub"` would Just Work via the existing `operator/`. Tecnix's `__unsafeTectonixInternalZonePath` recognises this — it returns a path-typed value via `v.mkPath(state.storePath(storePath), state.mem)`. But because `storePath` is already minted as a fresh CA path keyed on tree-SHA, what tecnix's path-typed return buys is the *deterministic* store path that subtree-routing through `SourcePath::operator/` alone wouldn't give you.

**Git submodules already mount per-subpath.**[^submodule-mount] When git fetches a submoduled repo, it builds a `MountedSourceAccessor` with the parent repo at root and each submodule at its relative path. Each submodule is a fresh `Input` with its own fingerprint, mounted at its parent path. The composite's fingerprint is `parent_fingerprint + ";s"`.

[^submodule-mount]: For each submodule, fetches it as its own `Input` with `submoduleInput.getAccessor(...)`, then `mounts.insert_or_assign(submodule.path, submoduleAccessor)`. Builds a `MountedSourceAccessor` with the parent at `CanonPath::root` and submodules at their relative paths. DetSys appends `;s` to the *accessor's* fingerprint after building the mount: [DetSys:git.cc#L1100-L1141](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/git.cc#L1100-L1141), with `;s` at [#L1136](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/git.cc#L1136). Master computes the `;s` suffix at the *Input* level via `Input::getFingerprint`: [NixOS/nix:git.cc#L1100](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git.cc#L1100); the commit-path mount construction is at [NixOS/nix:git.cc#L940-L980](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git.cc#L940-L980) without an accessor-level fingerprint append. This is *exactly* the pattern tecnix needs for zones, modulo where the fingerprint is set.

**Tecnix's "zone" concept is structurally identical to git submodules**, except submodules are external repos referenced by URL while zones are subtrees of the same repo. If subtree-as-input were generalised — `fetchTree { type = "git"; url = X; rev = Y; subdir = "//areas/tools/tec"; }` — every zone would be a regular flake input, lockable, with the existing mount machinery handling everything.

The reason tecnix didn't do this: there's no `Input` syntax for "subtree of this rev." The `subdir` attribute on flakes is for *flake-within-a-repo* lookup (where to find `flake.nix`), not for sub-tree-rooted accessor construction. Adding it as a primitive would let tecnix collapse most of their patches.

Why these two existing mechanisms don't fully solve the problem: `SourcePath` is per-EvalState (it doesn't survive across `nix eval` invocations); submodules are minted with `StorePath::random` (or real CA via `mountInput`), so each zone needs a *deterministic* store path keyed on tree-SHA, not random. The tree-SHA-aware fingerprint from §8 Tier 1 #1 is required.

### 7.3 Content-addressed identity is over-pluralised

A piece of content has *all* of:
- a `Hash` (raw bytes, 5 algorithms,[^hash-algos] 4 formats[^hash-formats])
- a `gitRev()` rendering
- a `StorePath` (32 base32 chars + name)
- an opaque `fingerprint: optional<string>` field on `SourceAccessor`
- an `Input::getNarHash()` and `Input::getFingerprint()` (two distinct identities at the L0 layer)
- a `treeHash` attribute (for tarballs unpacked into the tarball cache)
- a CA hash distinct from the NAR hash for non-NAR content addressing

[^hash-algos]: [NixOS/nix:hash.hh#L14](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil/include/nix/util/hash.hh#L14): `enum struct HashAlgorithm : char { MD5 = 42, SHA1, SHA256, SHA512, BLAKE3 };`

[^hash-formats]: [NixOS/nix:hash.hh#L42-L53](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil/include/nix/util/hash.hh#L42-L53): `enum struct HashFormat : int { Base64, Nix32, Base16, SRI };` — four formats.

These are all the same thing under different names and different scopes. The cache subsystem proves it[^cache-domains] — there are at least ten distinct `Cache::Key` domains, each with its own attrs schema, scattered across fetchers. The `treeHashToNarHash` projection is exactly "translate one identity scheme to another."

[^cache-domains]: Verified by `grep -h "Cache::Key" src/libfetchers/*.cc | grep -oE '"[a-zA-Z]+"' | sort -u` against [DetSys main](https://github.com/DeterminateSystems/nix-src/tree/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers): `gitLastModified`, `gitRevCount`, `gitRevToLastModified`, `gitRevToTreeHash`, `hgRefToRev`, `hgRev`, `tarball`, `treeHashToNarHash`, plus `file` (in [tarball.cc#L27](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/tarball.cc#L27)) and `sourcePathToHash` (in [fetch-to-store.cc#L8](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/fetch-to-store.cc#L8)). Ten distinct domains, each ad-hoc, with its own attrs schema. Tecnix adds an eleventh: `gitRevUrl`. Master has the same set as DetSys minus tecnix's addition.

The **missing abstraction**: a typeclass-shaped "ContentIdentity" with one canonical primary key (the raw content hash), and registered projections to/from the others.

### 7.4 Two laziness primitives, scoped differently

- **`mountInput`-style laziness** (lazy-trees): defers materialisation of *whole trees*.
- **`LazyAttr`** (master only, Sergei's path[^lazyattr-master]): defers computation of *individual attributes* like `revCount` of a git input. Different mechanism (a thunk plus a memoiser), different scope.

[^lazyattr-master]: [NixOS/nix:attrs.hh#L20-L34](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/include/nix/fetchers/attrs.hh#L20-L34) defines `ResolvedAttr`, `LazyAttrComputation`, `LazyAttr`, `Attr`. Used at [NixOS/nix:git.cc#L754](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libfetchers/git.cc#L754) for `lazyRevCount`. Plus [NixOS/nix:nar-accessor.hh#L56](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libutil/include/nix/util/nar-accessor.hh#L56) `makeLazyNarAccessor(NarListing, GetNarBytes)` — *third* laziness primitive that defers byte materialisation of NAR archives. (DetSys also has `makeLazyNarAccessor`; LazyAttr is master-only.)

A unified "lazy value" abstraction would let tecnix express `(fetchTree world).treeSha "//areas/tools/tec"` as a lazy attribute rather than a builtin call.

### 7.5 Synchronisation primitives don't form a coherent menu

Three primitives in heavy use:[^sync-primitives]

- `Sync<T>` — exclusive lock (despite having a misleadingly-named `readLock()` method that uses `unique_lock`).
- `SharedSync<T>` — shared+exclusive lock. *Actually* allows concurrent readers.
- `boost::concurrent_flat_map/set` — lockfree concurrent maps.

Plus `std::call_once` for one-shot init.

[^sync-primitives]: [DetSys:sync.hh#L162-L172](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libutil/include/nix/util/sync.hh#L162-L172): `Sync<T> = SyncBase<T, std::mutex, std::unique_lock<std::mutex>, std::unique_lock<std::mutex>, ...>` — both lock types are `unique_lock<mutex>`, so `Sync::readLock()` is *not* a shared lock despite the name. `SharedSync<T>` uses `std::shared_mutex` with `unique_lock` for write and `shared_lock` for read.

The choice between them is arbitrary. `FilteringSourceAccessor` proves it: its `allowedPrefixes` and `allowedPaths` are `SharedSync`-wrapped, but `cache` (the predicate memo) was plain `std::map` until tecnix wrapped it. Same struct, same access pattern, three different choices. The `Sync<T>::readLock()` naming is a bug-magnet — `InputCache` calls `cache_.readLock()` at [DetSys:input-cache.cc#L49](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libfetchers/input-cache.cc#L49) which under `Sync<T>` is exclusive, despite reading like a shared lookup.

### 7.6 The wrapper-ergonomics gap

Eight `SourceAccessor` wrappers exist; the `ForwardingSourceAccessor` superclass is supposed to be the "delegate everything by default" base, but it forwards only 6 of ~12 virtual methods (§5.10). Each non-forwarded method silently drops on extension. The **missing abstraction**: a complete delegating wrapper base class that forwards every virtual method.

### 7.7 Global settings vs per-input configuration

Tecnix configures the world repo via three global `EvalSettings` because the `Input` model can't express "subtree of this single repo." Every other source in Nix is per-input. Tecnix is the lone "this-is-the-environment, period" exception.[^no-tectonix-scheme]

[^no-tectonix-scheme]: `grep registerInputScheme [Shopify/tecnix:src/libfetchers](https://github.com/Shopify/tecnix/tree/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libfetchers)` returns the same set as DetSys: `IndirectInputScheme`, `GitArchiveInputScheme` with subclasses, `MercurialInputScheme`, `GitInputScheme`. No tectonix scheme. All access via three global `Setting<std::string>` declarations in [Shopify/tecnix:eval-settings.hh#L508-L545](https://github.com/Shopify/tecnix/blob/45d9c6f4a4e7b9457bb3fe5dab18427f5671735f/src/libexpr/include/nix/expr/eval-settings.hh#L508-L545).

### 7.8 "Lift this C++ data structure to a Nix attrset"

`__unsafeTectonixInternalManifest` exposes an `nlohmann::json` parse as a Nix attrset. `__unsafeTectonixInternalDirtyZones` exposes a C++ map. `__unsafeTectonixInternalSparseCheckoutRoots` exposes a set as a list. None reuse existing `parseJSON`/`printValueAsJSON`. The reason: parse-caching. `builtins.fromJSON` has no per-content cache.

The **missing abstraction**: parse-caching for pure expressions over input contents, e.g. `(fetchTree input).readFileJSON "/.meta/manifest.json"` lazy and content-keyed.

### 7.9 Composition leaks identity

§5.10 + §5.11 + §5.1 stack into one pattern: every wrapping/composing accessor is supposed to preserve the inner accessor's identity, but the composition rules for `getFingerprint`, `getProvenance`, `getLastModified` are inconsistently implemented. The result is silent identity-loss in some paths. **No tests** (§2.2) so the asymmetries persist.

### 7.10 "Materialise on demand" lacks a tag

§5.12. `StorePath::random` produces paths visually indistinguishable from real CA paths. Every cross-boundary site special-cases via `storeFS->getMount`. HSM systems handle this with explicit per-file state (online/dual-state/offline) — Nix's L2 needs the equivalent.

### 7.11 Path-prefix lookup is implemented three times

Three different "look up an inner accessor based on path prefix" implementations:

1. `MountedSourceAccessor::resolve` walks up paths to find nearest mount. Uses `boost::concurrent_flat_map<CanonPath, ref<SourceAccessor>>`.
2. `FilteringSourceAccessor::prefix` field — constant prefix translation, stored as a `CanonPath` field.
3. `RemoteFSAccessor::fetch` parses store paths via `store->toStorePath(storeDir + path)` to find the corresponding NAR accessor in `narCache`. Uses unprotected `std::map<string, Hash> narHashes`.[^remote-fs-narHashes]

[^remote-fs-narHashes]: Field declared at [DetSys:remote-fs-accessor.hh#L20](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libstore/include/nix/store/remote-fs-accessor.hh#L20) (master same), used at [DetSys:remote-fs-accessor.cc#L23](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libstore/remote-fs-accessor.cc#L23) and [#L30](https://github.com/DeterminateSystems/nix-src/blob/11f3aff904f84ae612e36e8bc578ac421fca74fa/src/libstore/remote-fs-accessor.cc#L30). Plain `std::map<std::string, Hash>` with no synchronisation. Same race-prone pattern as `CachingFilteringSourceAccessor::cache` had before tecnix's fix. Another instance of §7.5.

Three different solutions to the same problem. None share a base class.

---

## 8. Wishlist

### 8.1 Tier 1: address the actual pain points

1. **Tree-SHA-aware fingerprint on git accessors.** `getFingerprint("/sub")` should return the *subtree* SHA, not `git:C;e?;l?`. Collapses §6.1's duplicate cache rows. Tecnix's `getSubtreeSha` is the primitive; `attrCommitRev`/`attrPathPrefix`/`attrFingerprint` plumbing keeps `.gitattributes` correct.

2. **Subpath-aware `ensureLazyPathCopied` / `devirtualize`.** When `Opaque{worldStorePath}` is in context but the user-visible reference is `${worldStorePath}/sub`, materialise only `/sub`. Either a new `NixStringContextElem::SubpathOpaque{path, subpath}` variant or a tree-walk during devirtualize.

3. **Surface `treeHashToNarHash` for arbitrary git subtrees.** Currently scoped to the tarball-cache git ODB. Expanding it to consult the cache for any tree SHA across all opened git ODBs makes tecnix's `tectonixZoneCache_` redundant.

### 8.2 Tier 2: codify and fix the algebra

4. **Fold predicates into `FilteringSourceAccessor::getFingerprint`** (§5.1).
5. **Compose component fingerprints in `UnionSourceAccessor::getFingerprint`** (§5.1).
6. **`MountedSourceAccessor` and `UnionSourceAccessor` consult `this->provenance` first** (§5.2).
7. **Override `pathExists` on `MountedSourceAccessor` and `UnionSourceAccessor`** to forward to inner overrides (§5.3) so source-level optimisations propagate through composites.
8. **Define semantics for duplicate `mount`** (§5.8).
9. **Implement `getLastModified` aggregation** (§5.6).
10. **Forward `getFingerprint`, `getProvenance`, `getLastModified`, `invalidateCache` through `ForwardingSourceAccessor`** (§5.10, §5.11).
11. **Differentiate virtual from real CA paths** (§5.12, §7.10).
12. **Add contract tests for the combinator algebra** (§2.2). Zero tests today.
13. **Fix `Sync<T>::readLock()` naming** (§7.5) — it's not a shared lock under `Sync<T>`.

### 8.3 Tier 3: generalise tecnix's primitives

14. **Generic `OverlaySourceAccessor` combinator** with whiteouts (§7.6 / §6.3).
15. **Subtree-as-Input syntax** (§7.2 / §7.7). `fetchTree { ... subdir = "/areas/tools/tec"; }` returning a content-addressed sub-result, mounted via the existing submodule machinery.
16. **`fetchTree` returns `nPath` value** (§7.2), so `(fetchTree input) / "sub"` works via existing `SourcePath::operator/`.
17. **Promote `Provenance` from `Xp::Provenance`** (§7.9). Surface `(fetchTree input).provenance`.
18. **Parse-caching helpers on `fetchTree` results** (§7.8).

### 8.4 Independent fixes upstreamable as-is

19. Tecnix's `gitRevUrl` modern-path cache.
20. Tecnix's `SharedSync` wrap of `CachingFilteringSourceAccessor::cache`.
21. Tecnix's `.gitattributes` along-path fingerprint.
22. `RemoteFSAccessor::narHashes` synchronisation (same pattern as #20, distinct location).

### 8.5 Larger-pattern abstractions

23. **Unified content-identity primitive** (§7.3).
24. **Unified laziness primitive** spanning whole-tree, per-attribute, and per-byte (§7.4).
25. **Sync-primitive convention** — recipe for "what kind of lock for this kind of state" (§7.5).
26. **Path-prefix lookup combinator** generalising Mounted/Filtering's prefix and `RemoteFSAccessor::narHashes` (§7.11).

### 8.6 What does not need to change

The mount/devirtualize architecture itself. The `SourceAccessor` interface (modulo the asymmetry fixes). The store layer. The flake input model (additive subtree-handle is sufficient). The Nix language. None of this requires new syntax.

---

## 9. Limits of this analysis

What I have *not* done:

- **No benchmark of master's eager-NAR-hash cost vs DetSys's `StorePath::random`** at tecnix scale. §2.3 and §8 Tier 1 #2 are reasoned from code paths, not measured. Building Nix from source would dominate the session.
- **No reproducers for the dormant asymmetries in §5.** Each requires synthetic C++ accessors with custom fingerprints/provenance, not user-level Nix.
- **No exercising of tectonix builtins against a built tecnix Nix.** Their behaviour is fully determined by `eval.cc` and `tectonix.cc` source.
- **The Determinate-installer-overrides-default observation depends on what installer the user ran.** A different distribution might not flip the default.
- **Eval cache behaviour with virtual paths.** Verified that the eval cache ([NixOS/nix:eval-cache.cc](https://github.com/NixOS/nix/blob/2d309b18e5a3a8734f4e64e659dac1813964c241/src/libexpr/eval-cache.cc)) keys on a flake fingerprint, not on store paths. No special handling required because the eval cache caches *evaluated values*, not their store-path interpretation.
- **Parallelism stress.** Not stress-tested under `eval-cores > 1`.
- **Daemon boundary handling of virtual paths.** Not investigated in depth.
- **Substitution of virtual paths.** What happens when `nix copy` receives a virtual path was reasoned (it would fail — daemon has no concept) but not exercised.
- **The `LazyAttr` mechanism on master.** I read its definition and use sites but did not exercise its correctness under concurrent evaluation.
