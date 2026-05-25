# Other distinct candidates

Candidates 72-88. All seventeen VALID.

| # | Verdict | Effort |
| - | ------- | ------ |
| 72 | VALID | small |
| 73 | VALID | medium |
| 74 | VALID | small |
| 75 | OBSOLETE (trivial half) / VALID (small full helper) | small |
| 76 | VALID | small |
| 77 | VALID | trivial |
| 78 | VALID | small |
| 79 | VALID | small |
| 80 | VALID | trivial / small |
| 81 | VALID | medium / structural |
| 82 | VALID | trivial |
| 83 | VALID | small |
| 84 | VALID | small / medium |
| 85 | VALID | small |
| 86 | VALID | medium |
| 87 | VALID | trivial |
| 88 | VALID | trivial |

---

72. **`getDefaultFlakeAttrPaths` / `getDefaultFlakeAttrPathPrefixes` is copy-pasted across modern commands.** The `apps.<system>.default` + `defaultApp.<system>` shape appears verbatim in `CmdRun` and `CmdBundle`; `Common` (develop) has the analogous `devShells.<system>.default` + `devShell.<system>`; `MixFormatter` returns `formatter.<system>`; `CmdSearch` returns `packages.<system>` + `legacyPackages.<system>`. Each command also re-walks `SourceExprCommand::getDefaultFlakeAttrPaths()` to merge in base prefixes. A small `MixFlakeAttrPaths` helper would deduplicate.
    - ../verified/17-nix-modern-1.md
    - **Validation:** VALID. `bundle.cc` even has a literal `// FIXME: cut&paste from CmdRun.` comment. Effort: small.
    - **Branch:** `vibe-coding/cleanup/nix-cli` (deviates from the candidate's `MixFlakeAttrPaths` mixin form — a mixin attempt failed because `SourceExprCommand` is non-virtually inherited via `InstallableCommand`/`InstallableValueCommand`, producing diamond errors. Shipped as free helpers in `src/nix/flake-attr-paths.hh`; three sites converted: `CmdRun`, `CmdBundle`, `Common` (develop). `CmdSearch` and `MixFormatter` retain their direct overrides because they intentionally replace rather than augment the base list — asymmetry documented in the helper header).

73. **JSON-vs-text dual output paths in commands.** A dozen commands (`CmdPathInfo`, `CmdFlakeMetadata`, `CmdFlakeShow`, `CmdFlakePrefetch`, `CmdFlakeArchive`, `CmdRealisationInfo`, `CmdConfigShow`, `CmdStorePrefetchFile`, `CmdProfileList`, `CmdSearch`, `CmdEval`, `CmdBuild`) all branch on `if (json) { printJSON(...) } else { logger->cout(...) }` with identical surrounding control flow.
    - ../verified/17-nix-modern-1.md
    - **Validation:** VALID. A `MixOutputFormat` mixin that owns the branch and a `std::format`-based emitter pair would consolidate. **Caveat:** `CmdSearch` and `CmdFlakeShow` interleave evaluation with output, so they need a streaming variant (`std::generator<RenderedRow>`) rather than a single render return. Per-command landing recommended; don't lift all twelve at once. Effort: medium.

74. **GC dispatch logic duplicated three times.** `nix-store.cc opGC`, `nix-collect-garbage.cc main_nix_collect_garbage`, and `store-gc.cc CmdStoreGC::run` all open a `GcStore`, set `pathsToDelete = GCOptions::WholeStore{}`, wrap `collectGarbage` in `Finally`. The result-printing differs (always `printFreed` vs path-by-path).
    - ../verified/18-nix-modern-2-legacy.md
    - **Validation:** VALID — confirmed verbatim. Only difference: `opGC` branches its `Finally printer` on `options.action`. Effort: small.
    - **Branch:** `vibe-coding/cleanup/nix-cli` (header-only `runWholeStoreGC` helper in `src/nix/whole-store-gc.hh`, templated on the printer callback type — `template<class Printer> inline void runWholeStoreGC(Store &, GCOptions &, const Printer &)` — so the lambda is captured inline rather than through `std::function` type-erasure indirection. Three call sites consolidated. The `nix-store --gc --print-roots` branch keeps its local `require<GcStore>` because it doesn't run `collectGarbage`; the `else` branch uses the helper with a printer lambda that branches on `options.action` to preserve the existing return-live/dead vs `printFreed` asymmetry).

75. **Three NAR streaming entry points.** `CmdDumpPath::run` (`store dump-path`), `CmdDumpPath2::run` (`nar pack`), `nix-store.cc opDump`. The first two route through `dump-path.cc:getNarSink()`; the third constructs the `FdSink` directly and skips the TTY check. All three then call `narFromPath` or `dumpPath`.
    - ../verified/18-nix-modern-2-legacy.md
    - **Validation:** VALID — `dump-path.cc:getNarSink` adds the `isTTY` guard that `nix-store.cc opDump` skips. **Trivial half OBSOLETE:** the TTY-check sharing is a one-line consolidation; not a debt entry, just a single boolean call — drop. **Small half kept:** the full `runNarDump(SourceLike)` helper covers genuine duplication across the three entry points. Effort: small (full helper).
    - **Branch:** `vibe-coding/cleanup/nix-cli` (header-only template helper `runNarDump` in `src/nix/run-nar-dump.hh`; three sites consolidated. Asymmetry preserved via `bool checkTTY`: modern commands pass `true`, legacy `nix-store --dump` passes `false` with an explanatory comment at the call site).

76. **Closure-walk helpers (BFS over `references`).** `nix-store/dotgraph.cc` and `nix-store/graphml.cc` both implement the same `StorePathSet workList`/`doneSet` BFS over `references`. They differ only in edge-direction and per-node emission. A shared `walkClosure(start, visit)` helper would consolidate.
    - ../verified/18-nix-modern-2-legacy.md
    - **Validation:** VALID — byte-identical iteration scaffold. Edge direction differs (dotgraph reverses `p -> path`; graphml keeps `path -> p`). A `std::generator<StorePath> walkClosure(ref<Store>, StorePathSet roots)` coroutine in `store-api.hh` (next to `computeFSClosure`) is the cleanest fit. Effort: small.
    - **Branch:** `vibe-coding/cleanup/nix-cli` (added a header-only template helper `walkClosure(store, roots, onPath, onEdge)` in a new `src/nix/nix-store/closure-walk.hh` — file-local to the `nix-store/` directory rather than promoted to `store-api.hh` per the validation's original sketch, because the latter would be a cross-shard libstore change. The helper takes per-path and per-edge callbacks so each caller emits its own format; queries `queryPathInfo` once per visited path and passes the `ValidPathInfo &` through (dotgraph ignores most fields, graphml uses them). Both consumer files now call `walkClosure(...)` with their own lambdas; node-emission and edge-direction divergence preserved. Output is byte-for-byte identical pre/post.)

77. **Eval-cache release before `exec*` is duplicated four times.** `CmdRun::run`, `CmdDevelop::run`, `CmdShell::run`, `CmdFormatterRun::run` each call `state->evalCaches.clear()` immediately before exec'ing out of the process; the comment is identical at all four sites.
    - ../verified/17-nix-modern-1.md
    - **Validation:** VALID. The verbatim comment is "Release our references to eval caches to ensure they are persisted to disk, because we are about to exec out of this process without running C++ destructors." A `releaseEvalCachesBeforeExec(state)` helper or RAII guard would consolidate. Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/nix-cli` (helper in `src/nix/release-eval-caches.hh`; replaces the verbatim comment + `evalCaches.clear()` pair at four sites)

78. **Lock-file walks in libflake.** `LockFile::isUnlocked`, `LockFile::getAllInputs`, `doFind` each implement a custom DFS over `Node::inputs` with their own visited-set; only `getAllInputs` is reused. A shared `forEachNode`/`forEachReachableEdge` helper would simplify all three.
    - ../verified/15-libflake-libmain.md
    - **Validation:** VALID. Effort: small.
    - **Branch:** `vibe-coding/cleanup/libflake` (extracted `forEachReachableNode` shared by `isUnlocked` and `getAllInputs` only; `doFind` was excluded after walking all three sites because its DFS is structurally different — name-based path-walk through `node->inputs` that recurses only on `follows` indirections, keys its visited-set on `InputAttrPath` rather than Node identity, and throws on revisit with a rendered cycle path).

79. **Per-fetcher attrset-iteration with `if (n == "x") ... else if (n == "y") ... else error`.** Every fetcher primop iterates `*args[0]->attrs()` with this chain. `fetchTree`, `fetchClosure`, `fetchMercurial`, `fetch` are ripe for a helper that takes a `{ name → handler }` table and yields a uniform "unsupported argument" error.
    - ../verified/13-libexpr-primops.md
    - **Validation:** VALID. Compounds with #190 (which calls out the per-flavor URL rewrite tangling the helper). **Caveat:** `prim_fetchTree` mutates `nameAttrPassed` and special-cases `url` resolution after the loop; the helper needs a "post-validation" hook. Effort: small.
    - **Branch:** `vibe-coding/cleanup/libexpr` (internal `iterateFetcherAttrs` + `FetcherAttrHandler { name, handler }` in non-installed `src/libexpr/primops/fetcher-attr-iter.hh`. Three of the four cited sites migrated: `prim_fetchClosure`, `prim_fetchMercurial`, and the shared `fetch` helper behind `prim_fetchurl`/`prim_fetchTarball`. `prim_fetchTree` was deliberately excluded after walking the site individually (Rule 6) — its loop dispatches on the value's *type* (`nString`/`nBool`/`nInt`/...) to forward arbitrary scheme-specific attribute names into a `fetchers::Attrs` map, not on names; the candidate body's mention of `fetchTree` is a misclassification, and the validation paragraph's "post-validation hook" caveat actually applies to `fetch` (`nameAttrPassed` mutation, captured naturally by the handler closure). User-visible diagnostic changes: `prim_fetchClosure` wording moves from "attribute 'X' isn't supported in call to 'fetchClosure'" (positioned at the call site) to the uniform "unsupported argument 'X' to 'fetchClosure'" positioned at the offending attribute; `fetchurl`/`fetchTarball` position moves from the call site to the offending attribute. No functional test asserts on either.)

80. **`logFD` Setting<int> on Unix vs plain `Descriptor logFD` on Windows is a latent bug.** In `LegacySSHStoreConfig`, the Windows side bypasses the settings system entirely; the field cannot be set at all from user config (only mutated programmatically in tests) — silently ignores the `log-fd` setting. Cross-class with "Per-platform symmetry" (#27, #29, #81, #161-#164) and "Globals/settings architecture": this is a configuration-surface inconsistency that fits best under "Latent bugs hiding inside duplication". Cleanest fix is a portable `Setting<Descriptor>` specialisation.
    - ../verified/08-libstore-remote.md
    - **Validation:** VALID. **Latent bug:** the Windows side cannot be set at all from user config (only mutated programmatically in tests) — silently ignores the `log-fd` setting. Cleanest fix is a portable `Setting<Descriptor>` specialisation. **See also:** N9 (per-platform `Pid`/`Pipe`/`Process` symmetry — same elephant, no shared interface header), N44 (the per-T `BaseSetting<T>::trait` specialisation pattern). Effort: trivial / small.
    - **Branch:** `vibe-coding/cleanup/libstore` (replaced `logFD` with a unified `Setting<Descriptor>`. Added a Windows-only `BaseSetting<Descriptor>::parse`/`to_string` specialisation declared in `legacy-ssh-store.hh` (which ships via `install_headers`) that bridges integer FD → HANDLE via `toDescriptor`. **Behavioural change on Windows:** store URLs may now carry `log-fd=N`; previously silently dropped. No existing tests exercised the Windows side.).

81. **`Pid` holds `pid_t` on Unix vs `AutoCloseFD` on Windows.** Mostly fine, but the `release()` method exists only on Unix; `setSeparatePG`/`setKillSignal`/`setKillTimeout` are Unix-only; `wait`'s `allowInterrupts` is unused on Windows.
    - ../verified/04-libutil-misc.md
    - **Validation:** VALID. The two reps are semantically different (process handle vs PID), so genuine deduplication is structural. Minimal fix: split into a portable `Pid` interface (kill/wait/dtor) and a Unix-only `UnixProcess : Pid` adding the process-group/timeout knobs; document `allowInterrupts` as ignored on Windows. Effort: medium / structural.

82. **`AutoUserLock`/`SimpleUserLock` `acquire` skeletons.** Both implementations open a per-slot lock file, try non-blocking exclusive lock via `lockFile(ltWrite, false)`, populate the lock object on success. The lock-acquisition skeleton could be shared.
    - ../verified/07-libstore-local.md
    - **Validation:** VALID — but the win is small (two call sites with one shared body of ~5 lines each). Extract `tryAcquireSlotLock(path) -> std::optional<AutoCloseFD>`; both call sites become a one-liner preceded by per-implementation prelude. Marginal as a debt entry; keep but accept that the duplication factored out is small. Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libstore` (file-static helper in `unix/user-lock.cc`. Deliberately bypasses `pathlocks.hh::FdLock` because `FdLock` is non-movable/non-copyable and the lock is handed off to the `UserLock`'s `AutoCloseFD` member for the duration of the build, not RAII-released at scope exit.)

83. **`/nix/store` GC roots and runtime roots have three layers of similar logic.** `local-gc.cc::findRuntimeRootsUnchecked`, `gc.cc::requestRuntimeRoots`, and `gc.cc::LocalStore::findRuntimeRoots` form three layers; the first synthesises roots from `/proc` (or `lsof`), the second reads them from a Unix-domain socket, the third dispatches between the two. The `Roots` typedef and the file-local `UncheckedRoots` map use different key types (`StorePath` vs `std::string`).
    - ../verified/07-libstore-local.md
    - **Validation:** VALID. The three layers do meaningfully different work (synthesising, transporting, validating); collapsing them obscures trusted-vs-untrusted root sources. Deduplicable slice is the post-collection validation loop in `LocalStore::findRuntimeRoots` → `validateRuntimeRoots(LocalStore &, UncheckedRoots, bool censor)`. Aligning `Roots`/`UncheckedRoots` key types to `StorePath` (parse once at source) is a small win. Effort: small.
    - **Branch:** `vibe-coding/cleanup/libstore` (extracted the post-collection validation loop into a file-scope `validateRuntimeRoots` helper. The key-alignment half was already done — both `findRuntimeRootsUnchecked` and `requestRuntimeRoots` already parse store paths at source and return `Roots` (StorePath-keyed); the string-keyed `UncheckedRoots` typedef is internal to `local-gc.cc`.).

84. **`narHash` / `references` parser duplicates between `path-info.cc` JSON and `nar-info.cc` text.** Both reconstruct the same `UnkeyedValidPathInfo` fields with their own per-format error handling.
    - ../verified/05-libstore-core.md, ../verified/08-libstore-remote.md
    - **Validation:** VALID. Wire formats genuinely differ (JSON vs header-style lines, SRI vs Nix32 hash format), so unification at the parsing layer is awkward. Cleanest factoring extracts a `UnkeyedValidPathInfoBuilder` with named setters (`setNarHash`, `addReference`, …) and a `build()` finaliser; both parsers call into the builder. Centralises invariants (e.g. self-reference rejection) without forcing a shared schema. Effort: small / medium.
    - **Skip-with-reason (libstore session, May 2026):** read both parsers end-to-end; the prescription doesn't actually shrink either body. The "shared" data is just simple struct field assignments where each setter (`setNarHash`, `addReference`, etc.) is a one-liner equivalent to `info.X = …`. The only invariant the candidate hints at — self-reference rejection — does not currently exist in either parser, so adding it would be a new behaviour change orthogonal to a mechanical refactor. Each parser also handles format-specific concerns (V1/V2 JSON dispatch in `path-info.cc`; URL/Compression/FileHash/missing-field validation in `nar-info.cc`) that don't live in the builder either way. Effort downgraded to **medium pending a real invariant to centralise** rather than the cosmetic builder-pattern shape.

85. **Setting<T> serialisation specialisations duplicated for `StoreReference`.** `BaseSetting<StoreReference>::parse`/`to_string`, `BaseSetting<std::vector<StoreReference>>::parse`/`to_string`/`appendOrSet`, `BaseSetting<std::set<StoreReference>>::parse`/`to_string`/`appendOrSet`. Three near-cut-and-pasted families.
    - ../verified/07-libstore-local.md
    - **Validation:** VALID. Compounds with #21, #43. Effort: small.
    - **Branch:** `vibe-coding/cleanup/libstore` (deduplicated the vector and set `parse`/`to_string` via two file-scope templates `parseStoreReferenceCollection<C>` and `renderStoreReferenceCollection<C>`, using a `requires` expression to pick `push_back` vs `insert`. The container-specific `appendOrSet` bodies stay inline because vector end-extend and set merge-insert genuinely differ. The singular `BaseSetting<StoreReference>` parse/to_string also stay inline — they are one-liners that don't share the tokenize-and-collect shape.).

86. **`OnStartup` lambda registration pattern.** Used in libfetchers (`OnStartup([] { registerInputScheme(...) })`), libstore (`RegisterStoreImplementation<TConfig>` with a static instance), libcmd (`RegisterCommand` static instance), libexpr (`RegisterPrimOp` static instance), libstore/build (`RegisterBuiltinBuilder`). Each registry is a Meyers singleton with a different signature. A shared `Registry<Key, Factory>` template would consolidate.
    - ../verified/04-libutil-misc.md, ../verified/05-libstore-core.md, ../verified/14-libfetchers.md, ../verified/16-libcmd.md, ../verified/13-libexpr-primops.md
    - **Validation:** VALID. Compounds with #137 (consolidating the static-init registrations). Effort: medium.

87. **`Forced vs lazy` value access in C bindings.** `nix_get_list_byidx{,_lazy}`, `nix_get_attr_byname{,_lazy}`, `nix_get_attr_byidx{,_lazy}` are nearly-identical pairs differing only by the presence of `forceValue` and slight error-message variations. The duplication is the most obvious internal symmetry in the C ABI.
    - ../verified/19-c-bindings-misc.md
    - **Validation:** VALID. Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libexpr` (three file-static private impl helpers — `get_list_byidx_impl`, `get_attr_byname_impl`, `get_attr_byidx_impl` — each carrying the shared body with a `bool force` parameter; the six public symbols become one-line wrappers selecting `force=true` or `force=false`. ABI and signature unchanged. Error wording preserved exactly: `nix_get_attr_byidx_lazy`'s "(Nix C API contract violation)" suffix on the bounds-check error message — divergent from `nix_get_attr_byidx`'s plain "attribute index out of bounds" — is threaded through the impl helper as a parameter rather than collapsed.)

88. **`nix_<libname>_init` family.** Each library exposes a parallel idempotent init. Could be a single template macro; currently each is a hand-rolled wrapper.
    - ../verified/19-c-bindings-misc.md
    - **Validation:** VALID. Effort: trivial.
