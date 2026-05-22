# libexpr eval-core: cache, attr-set, profiler

Candidates 201-209. Seven VALID; #205, #206, #208 PARTIALLY VALID.

**Latent bug surfaced:** #203 — `AttrDb::getAttr` does not honour the
`doSQLite` failed-flag protocol that every writer uses. Asymmetric error
handling between read and write paths.

| # | Verdict | Effort |
| - | ------- | ------ |
| 201 | VALID | small |
| 202 | VALID | small |
| 203 | VALID | small (latent bug) |
| 204 | VALID | small |
| 205 | PARTIALLY VALID | medium |
| 206 | PARTIALLY VALID | medium |
| 207 | VALID | small |
| 208 | PARTIALLY VALID | small |
| 209 | VALID | medium |

---

201. **`eval-cache.cc` SQLite schema carries no version marker; mismatch is detected only by the directory name `eval-cache-v6`.** [HIGH] The `static const char * schema` in `eval_cache::AttrDb` declares one `Attributes` table and is fed verbatim to `state->db.exec(schema)` with `create table if not exists`. There is no `PRAGMA user_version`, no `SchemaVersion` table, no migration logic, and no compatibility check against an existing file. A user who downgrades Nix and writes against the same `eval-cache-v6/<fingerprint>.sqlite` with an incompatible schema gets silent corruption. Sharpens #168: all five SQLite caches (eval-cache, binary-cache, fetcher-cache, tarball-cache, narinfo-cache) encode their schema version in the filename rather than in `PRAGMA user_version`.
    - ../verified/11-libexpr-eval.md
    - **Validation:** VALID. Confirmed no `PRAGMA user_version` anywhere in `eval-cache.cc`. Effort: small (per cache; medium across all five).

202. **`AttrCursor::forceValue` caches evaluation failures via `failed_t`, then has to round-trip through a custom `CachedEvalError::force()` to re-evaluate.** [DESIGN] `forceValue` catches `EvalError`, calls `root->db->setFailed(getKey())` to persist `AttrType::Failed`, and rethrows. On the next access, `fetchCachedValue` reads the `failed_t` row and throws a `CachedEvalError`, which carries the cursor and the attr name; callers that want to actually re-run evaluation must call `CachedEvalError::force()`, which calls back into `forceValue` against the parent. The design caches the *fact* of failure but never the diagnostic, so every consumer that wants the original message has to force the cursor again. A simpler design would be: don't insert `Failed` rows at all, let the next eval re-run, and rely on the rest of the cache to limit re-eval scope.
    - ../verified/11-libexpr-eval.md
    - **Validation:** VALID. Drop the `Failed` row insertion entirely; rely on typed/`Placeholder`/`Missing` cache entries to scope re-eval. Cache hit saves nothing in the common path (consumers wanting the original message must `force()` to re-evaluate anyway). Effort: small.

203. **`AttrDb::doSQLite` is applied to every writer but the reader `getAttr` bypasses it, so a read-side `SQLiteError` propagates to the caller instead of disabling the cache.** [HIGH] All ten "set*" methods (`setAttrs`, `setString`, `setBool`, `setInt`, `setListOfStrings`, `setPlaceholder`, `setMissing`, `setMisc`, `setFailed`) wrap their bodies in `doSQLite`, which catches `SQLiteError`, calls `ignoreExceptionExceptInterrupt`, sets `failed = true`, and returns `0`. `getAttr` does not — it directly calls `state->queryAttribute.use()(...)` and surfaces any `SQLiteError` to the caller. The asymmetry means a write fault disables the cache silently while a read fault aborts evaluation. `~AttrDb` skips committing the txn when `failed`, but `getAttr`'s exception escapes before that flag is set. Either the read path should also be guarded (returning `std::nullopt` and setting `failed = true`), or the write path should propagate too — the current state is a half-implemented invariant.
    - ../verified/11-libexpr-eval.md
    - **Validation:** VALID — and a **latent bug**. Effort: small.

204. **`MultiEvalProfiler` exists to fan out to a `vector<ref<EvalProfiler>>`, but `SampleStack` is the only in-tree profiler implementation.** [MEDIUM] `EvalProfiler` is the abstract base, `MultiEvalProfiler` holds a `vector<ref<EvalProfiler>>` and dispatches `pre/postFunctionCallHook` per element while OR-ing `getNeededHooks()` across the vector, and `SampleStack` (the only concrete subclass) implements stack-sampling. `EvalState` always installs the profiler via the `Multi`, even when only `SampleStack` is registered.
    - ../verified/11-libexpr-eval.md
    - **Validation:** VALID. **Correction:** `SampleStack` is not the only `EvalProfiler` subclass — `FunctionCallTrace` (in `function-trace.hh`) is the second; both registered conditionally in `EvalState` ctor. Multi is over-engineered for an effectively 0/1/2-element vector either way. Replace vector with `std::array<ref<EvalProfiler>, 2>` plus `std::bitset<2>` mask, or drop Multi and inline. Effort: small.

205. **`Counter` reserves a full cache line per atomic counter and gates increment on `Counter::enabled`, but the alignment cost is paid even when disabled.** [MEDIUM] `alignas(std::hardware_destructive_interference_size) struct Counter` (in `expr/counter.hh`) wraps a `std::atomic<uint64_t>` and conditions every `++`/`--`/`+=`/`-=` on a global `static bool enabled` (set when `NIX_SHOW_STATS` is in the environment). The 64-byte alignment is a real cost — every `EvalState` member that's a `Counter` consumes a cache line on its own. The `enabled` gate is a runtime branch, not a compile-time switch. Two simpler shapes are possible: (a) make the type a `Counter<bool Enabled = NIX_DEBUG_STATS>` template parameter so the disabled case compiles to a no-op, or (b) drop the per-counter alignment.
    - ../verified/11-libexpr-eval.md
    - **Validation:** PARTIALLY VALID. **Correction:** 13 `Counter` members per `EvalState`/`EvalMemory` (6 in `EvalMemory::Statistics` + 7 in `EvalState`) at 64 bytes each = ~832 bytes pinned by alignment. The compile-time-switch route via template parameter is clean; the alignment-drop route needs benchmarking under multi-threaded eval. Effort: medium.

206. **`Bindings::maxLayers = 8` is a magic constant with no benchmark cited, and the fallback path on overflow is a flat copy in the hot `ExprOpUpdate::eval`.** [DESIGN] `static constexpr unsigned maxLayers = 8` (in `expr/attr-set.hh`) caps the linked-list depth that `ExprOpUpdate` will build before falling back to a flat `buildBindings(bindings1.size() + bindings2.size())` copy in `eval.cc`'s `ExprOpUpdate::evalForUpdate` path. The `static_vector<BindingsCursor, maxLayers>` priority queue in `Bindings::iterator` is sized off the same constant. The doc comment on `bindingsUpdateLayerRhsSizeThreshold` says "tunes the maximum size of an attribute set that ... uses a more space-efficient linked-list representation", but `maxLayers` itself is not exposed as a setting and has no benchmark log in the tree. When the cap is hit, `ExprOpUpdate::eval` falls back to copying both sides flat — this *is* the hot path for derivations that chain many `//` updates (nixpkgs lib `recursiveUpdate`, NixOS module evaluation). Either the cap should be a setting alongside `bindingsUpdateLayerRhsSizeThreshold`, or there should be a regression benchmark pinning `maxLayers = 8` as the optimum.
    - ../verified/11-libexpr-eval.md
    - **Validation:** PARTIALLY VALID. The cap is real but no benchmark in tree confirms "this is hot path" empirically. Exposing `maxLayers` as a `Setting<unsigned>` (option a) breaks the `static_vector<..., maxLayers>` compile-time sizing — would need `boost::container::small_vector` with heap fallback, or a `static_vector<..., kHardCap>` plus a runtime soft-cap. Recommendation: add a regression benchmark before changing the default. Effort: medium.

207. **`SymbolStr::SymbolValueStore` chunked-vector path beyond one chunk is exercised only by the small-chunk `chunked-vector.cc` unit test, never by an integration test that creates >65536 symbols.** [MEDIUM] `chunkSize = 65536` and `MaxChunks = numeric_limits<uint32_t>::max() / 65536`. The only tests that hit chunk boundaries are in `libutil-tests/chunked-vector.cc` with `ChunkedVector<int, 2, 100>` — i.e. they exercise the chunk-overflow logic with 2-element chunks, not the production layout. No integration test, evaluator test, or benchmark in the tree creates a `SymbolTable` with more than 65536 distinct identifiers. Given that `SymbolTable` interns on insert and never deletes, a `std::deque<SymbolValue>` would offer the same pointer-stability guarantee that `ChunkedVector` provides.
    - ../verified/11-libexpr-eval.md
    - **Validation:** VALID. `std::deque<SymbolValue>` provides the same pointer-stability guarantee that `ChunkedVector`'s production geometry needs; the only in-tree consumer of `ChunkedVector` is `SymbolStr::SymbolValueStore`. Effort: small.

208. **`StaticSymbolTable` and `SymbolTable` are kept in lockstep by a runtime assertion in `copyIntoSymbolTable`, not by a compile-time guarantee.** [MEDIUM] `StaticSymbolTable::create` is `consteval`-friendly and assigns ids `1..N` at compile time. `SymbolTable(const StaticSymbolTable &)` then walks the static entries and calls `symtab.create(str)`, asserting via `unreachable()` that the dynamic id matches the static id. This works only because the runtime constructor sees an empty store at the moment the static entries are copied; if anything ever interns a symbol before the static-table copy, the ids drift silently except for the assertion.
    - ../verified/11-libexpr-eval.md
    - **Validation:** PARTIALLY VALID. Drift is **impossible by construction** with the current ctor signature (the runtime table is empty when the copy happens). The runtime assert is belt-and-suspenders, not load-bearing. The proposed initialise-from-array refactor is still a clarity win. Effort: small.

209. **`EvalSettings` mixes 9 tracing/debug toggles with 7 eval-semantics settings on one struct; carving out an `EvalDebugSettings` would isolate the debug surface.** [DESIGN] Of the 23 settings on `EvalSettings`, the eval-semantics core is `nixPath`, `currentSystem`, `restrictEval`, `pureEval`, `allowedUris`, `useEvalCache`, `maxCallDepth` (7); the IFD pair is `traceImportFromDerivation`, `enableImportFromDerivation` (2); the lint family is `lintShortPathLiterals`, `lintAbsolutePathLiterals`, `lintUrlLiterals`, plus the deprecated `warnShortPathLiterals` alias (4); the tuning knob is `bindingsUpdateLayerRhsSizeThreshold` (1); and the rest — `traceFunctionCalls`, `traceVerbose`, `builtinsTraceDebugger`, `builtinsDebuggerOnWarn`, `builtinsAbortOnWarn`, `ignoreExceptionsDuringTry`, `evalProfilerMode`, `evalProfileFile`, `evalProfilerFrequency` — are all debug/trace/profiler toggles (9). The debug nine could move to a `EvalDebugSettings` struct hung off `EvalState::debugSettings`. The win is that `EvalSettings` becomes a stable "what does the language do" surface vs `EvalDebugSettings` becomes a "how does the evaluator report itself" surface — relevant for the C API ambition called out in the `// FIXME: This really shouldn't be public` comment on `readOnlyMode`.
    - ../verified/11-libexpr-eval.md
    - **Validation:** VALID. Compounds with #138, #144. Effort: medium.
