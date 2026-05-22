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

73. **JSON-vs-text dual output paths in commands.** A dozen commands (`CmdPathInfo`, `CmdFlakeMetadata`, `CmdFlakeShow`, `CmdFlakePrefetch`, `CmdFlakeArchive`, `CmdRealisationInfo`, `CmdConfigShow`, `CmdStorePrefetchFile`, `CmdProfileList`, `CmdSearch`, `CmdEval`, `CmdBuild`) all branch on `if (json) { printJSON(...) } else { logger->cout(...) }` with identical surrounding control flow.
    - ../verified/17-nix-modern-1.md
    - **Validation:** VALID. A `MixOutputFormat` mixin that owns the branch and a `std::format`-based emitter pair would consolidate. **Caveat:** `CmdSearch` and `CmdFlakeShow` interleave evaluation with output, so they need a streaming variant (`std::generator<RenderedRow>`) rather than a single render return. Per-command landing recommended; don't lift all twelve at once. Effort: medium.

74. **GC dispatch logic duplicated three times.** `nix-store.cc opGC`, `nix-collect-garbage.cc main_nix_collect_garbage`, and `store-gc.cc CmdStoreGC::run` all open a `GcStore`, set `pathsToDelete = GCOptions::WholeStore{}`, wrap `collectGarbage` in `Finally`. The result-printing differs (always `printFreed` vs path-by-path).
    - ../verified/18-nix-modern-2-legacy.md
    - **Validation:** VALID — confirmed verbatim. Only difference: `opGC` branches its `Finally printer` on `options.action`. Effort: small.

75. **Three NAR streaming entry points.** `CmdDumpPath::run` (`store dump-path`), `CmdDumpPath2::run` (`nar pack`), `nix-store.cc opDump`. The first two route through `dump-path.cc:getNarSink()`; the third constructs the `FdSink` directly and skips the TTY check. All three then call `narFromPath` or `dumpPath`.
    - ../verified/18-nix-modern-2-legacy.md
    - **Validation:** VALID — `dump-path.cc:getNarSink` adds the `isTTY` guard that `nix-store.cc opDump` skips. **Trivial half OBSOLETE:** the TTY-check sharing is a one-line consolidation; not a debt entry, just a single boolean call — drop. **Small half kept:** the full `runNarDump(SourceLike)` helper covers genuine duplication across the three entry points. Effort: small (full helper).

76. **Closure-walk helpers (BFS over `references`).** `nix-store/dotgraph.cc` and `nix-store/graphml.cc` both implement the same `StorePathSet workList`/`doneSet` BFS over `references`. They differ only in edge-direction and per-node emission. A shared `walkClosure(start, visit)` helper would consolidate.
    - ../verified/18-nix-modern-2-legacy.md
    - **Validation:** VALID — byte-identical iteration scaffold. Edge direction differs (dotgraph reverses `p -> path`; graphml keeps `path -> p`). A `std::generator<StorePath> walkClosure(ref<Store>, StorePathSet roots)` coroutine in `store-api.hh` (next to `computeFSClosure`) is the cleanest fit. Effort: small.

77. **Eval-cache release before `exec*` is duplicated four times.** `CmdRun::run`, `CmdDevelop::run`, `CmdShell::run`, `CmdFormatterRun::run` each call `state->evalCaches.clear()` immediately before exec'ing out of the process; the comment is identical at all four sites.
    - ../verified/17-nix-modern-1.md
    - **Validation:** VALID. The verbatim comment is "Release our references to eval caches to ensure they are persisted to disk, because we are about to exec out of this process without running C++ destructors." A `releaseEvalCachesBeforeExec(state)` helper or RAII guard would consolidate. Effort: trivial.

78. **Lock-file walks in libflake.** `LockFile::isUnlocked`, `LockFile::getAllInputs`, `doFind` each implement a custom DFS over `Node::inputs` with their own visited-set; only `getAllInputs` is reused. A shared `forEachNode`/`forEachReachableEdge` helper would simplify all three.
    - ../verified/15-libflake-libmain.md
    - **Validation:** VALID. Effort: small.

79. **Per-fetcher attrset-iteration with `if (n == "x") ... else if (n == "y") ... else error`.** Every fetcher primop iterates `*args[0]->attrs()` with this chain. `fetchTree`, `fetchClosure`, `fetchMercurial`, `fetch` are ripe for a helper that takes a `{ name → handler }` table and yields a uniform "unsupported argument" error.
    - ../verified/13-libexpr-primops.md
    - **Validation:** VALID. Compounds with #190 (which calls out the per-flavor URL rewrite tangling the helper). **Caveat:** `prim_fetchTree` mutates `nameAttrPassed` and special-cases `url` resolution after the loop; the helper needs a "post-validation" hook. Effort: small.

80. **`logFD` Setting<int> on Unix vs plain `Descriptor logFD` on Windows is a latent bug.** In `LegacySSHStoreConfig`, the Windows side bypasses the settings system entirely; the field cannot be set at all from user config (only mutated programmatically in tests) — silently ignores the `log-fd` setting. Cross-class with "Per-platform symmetry" (#27, #29, #81, #161-#164) and "Globals/settings architecture": this is a configuration-surface inconsistency that fits best under "Latent bugs hiding inside duplication". Cleanest fix is a portable `Setting<Descriptor>` specialisation.
    - ../verified/08-libstore-remote.md
    - **Validation:** VALID. **Latent bug:** the Windows side cannot be set at all from user config (only mutated programmatically in tests) — silently ignores the `log-fd` setting. Cleanest fix is a portable `Setting<Descriptor>` specialisation. **See also:** N9 (per-platform `Pid`/`Pipe`/`Process` symmetry — same elephant, no shared interface header), N44 (the per-T `BaseSetting<T>::trait` specialisation pattern). Effort: trivial / small.

81. **`Pid` holds `pid_t` on Unix vs `AutoCloseFD` on Windows.** Mostly fine, but the `release()` method exists only on Unix; `setSeparatePG`/`setKillSignal`/`setKillTimeout` are Unix-only; `wait`'s `allowInterrupts` is unused on Windows.
    - ../verified/04-libutil-misc.md
    - **Validation:** VALID. The two reps are semantically different (process handle vs PID), so genuine deduplication is structural. Minimal fix: split into a portable `Pid` interface (kill/wait/dtor) and a Unix-only `UnixProcess : Pid` adding the process-group/timeout knobs; document `allowInterrupts` as ignored on Windows. Effort: medium / structural.

82. **`AutoUserLock`/`SimpleUserLock` `acquire` skeletons.** Both implementations open a per-slot lock file, try non-blocking exclusive lock via `lockFile(ltWrite, false)`, populate the lock object on success. The lock-acquisition skeleton could be shared.
    - ../verified/07-libstore-local.md
    - **Validation:** VALID — but the win is small (two call sites with one shared body of ~5 lines each). Extract `tryAcquireSlotLock(path) -> std::optional<AutoCloseFD>`; both call sites become a one-liner preceded by per-implementation prelude. Marginal as a debt entry; keep but accept that the duplication factored out is small. Effort: trivial.

83. **`/nix/store` GC roots and runtime roots have three layers of similar logic.** `local-gc.cc::findRuntimeRootsUnchecked`, `gc.cc::requestRuntimeRoots`, and `gc.cc::LocalStore::findRuntimeRoots` form three layers; the first synthesises roots from `/proc` (or `lsof`), the second reads them from a Unix-domain socket, the third dispatches between the two. The `Roots` typedef and the file-local `UncheckedRoots` map use different key types (`StorePath` vs `std::string`).
    - ../verified/07-libstore-local.md
    - **Validation:** VALID. The three layers do meaningfully different work (synthesising, transporting, validating); collapsing them obscures trusted-vs-untrusted root sources. Deduplicable slice is the post-collection validation loop in `LocalStore::findRuntimeRoots` → `validateRuntimeRoots(LocalStore &, UncheckedRoots, bool censor)`. Aligning `Roots`/`UncheckedRoots` key types to `StorePath` (parse once at source) is a small win. Effort: small.

84. **`narHash` / `references` parser duplicates between `path-info.cc` JSON and `nar-info.cc` text.** Both reconstruct the same `UnkeyedValidPathInfo` fields with their own per-format error handling.
    - ../verified/05-libstore-core.md, ../verified/08-libstore-remote.md
    - **Validation:** VALID. Wire formats genuinely differ (JSON vs header-style lines, SRI vs Nix32 hash format), so unification at the parsing layer is awkward. Cleanest factoring extracts a `UnkeyedValidPathInfoBuilder` with named setters (`setNarHash`, `addReference`, …) and a `build()` finaliser; both parsers call into the builder. Centralises invariants (e.g. self-reference rejection) without forcing a shared schema. Effort: small / medium.

85. **Setting<T> serialisation specialisations duplicated for `StoreReference`.** `BaseSetting<StoreReference>::parse`/`to_string`, `BaseSetting<std::vector<StoreReference>>::parse`/`to_string`/`appendOrSet`, `BaseSetting<std::set<StoreReference>>::parse`/`to_string`/`appendOrSet`. Three near-cut-and-pasted families.
    - ../verified/07-libstore-local.md
    - **Validation:** VALID. Compounds with #21, #43. Effort: small.

86. **`OnStartup` lambda registration pattern.** Used in libfetchers (`OnStartup([] { registerInputScheme(...) })`), libstore (`RegisterStoreImplementation<TConfig>` with a static instance), libcmd (`RegisterCommand` static instance), libexpr (`RegisterPrimOp` static instance), libstore/build (`RegisterBuiltinBuilder`). Each registry is a Meyers singleton with a different signature. A shared `Registry<Key, Factory>` template would consolidate.
    - ../verified/04-libutil-misc.md, ../verified/05-libstore-core.md, ../verified/14-libfetchers.md, ../verified/16-libcmd.md, ../verified/13-libexpr-primops.md
    - **Validation:** VALID. Compounds with #137 (consolidating the static-init registrations). Effort: medium.

87. **`Forced vs lazy` value access in C bindings.** `nix_get_list_byidx{,_lazy}`, `nix_get_attr_byname{,_lazy}`, `nix_get_attr_byidx{,_lazy}` are nearly-identical pairs differing only by the presence of `forceValue` and slight error-message variations. The duplication is the most obvious internal symmetry in the C ABI.
    - ../verified/19-c-bindings-misc.md
    - **Validation:** VALID. Effort: trivial.

88. **`nix_<libname>_init` family.** Each library exposes a parallel idempotent init. Could be a single template macro; currently each is a hand-rolled wrapper.
    - ../verified/19-c-bindings-misc.md
    - **Validation:** VALID. Effort: trivial.
