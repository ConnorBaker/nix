# Prompt

This is a continuation of round-3.md.

Please read all responses. You should fetch and read the sources referenced to ensure accuracy. You should also list those sources. Then, update your plan where appropriate, being sure to include alternative designs or implementations and their tradeoffs, as well as additional references. Lastly, answer any questions posed by any of the responses in a new file, round-4-answers.md.

Since there are a lot of steps here, please make a todo list and use it to keep track of your finished work and remaining work as you proceed.

# Responses

## ChatGPT

```text
Fourth-Round Refinement: Nix Evaluator Memoization Roadmap

The update‑3 plan laid out a rich set of design decisions and phased tasks for within‑eval and cross‑eval thunk memoization.  We have re‑examined each proposal in light of the round‑3 Q&A responses (and the current Nix codebase) to confirm, adjust, or defer them.  In general, the major approaches remain sound but require refinement and reprioritization.  Below we summarize the status of each decision, then present a phased implementation roadmap.  We conclude with notes on architectural implications (closure serialization, CBOR tagging, cycles, etc.) and open questions for Nix developers.

Revisiting Key Proposals
	•	Cacheability vs. Portability (Decision 1): The update suggested a SALSA‑style durability model, separating values that are cacheable only intra‑evaluation (identity‑based) from those cacheable cross‑evaluation (content‑based) ￼ ￼.  This distinction is crucial: lambdas and closures cannot be safely hashed by content across runs, whereas basic values and paths (with content digests) can.  Round‑3 feedback confirms this two‑tier notion, so we keep it.  In practice, we implement two predicates: one for intra‑eval identity caching (using pointer stability) and one for cross‑eval content caching.  The more ambitious SALSA/durability layering is deferred (Phase 5) as a future optimization, focusing now on separate identity/content flags or functions.
	•	Environment Hashing (Decision 2): We must avoid order‑dependent hashes for nested let/with bindings.  The current code iterates Env.values[] in declaration order, but that can change if thunks are forced in different order.  The plan’s solution – a two-level cache (fast identity cache plus portable content cache) – still stands.  Intra‑eval, we key off (Expr*, Env*, generation) for O(1) lookups ￼.  For content keys, we use a structural hash: when hashing an Env, each thunk slot is hashed by identity (pointer to its expr and env) rather than by forcing it, while already-forced values use their content hash ￼.  This ensures consistency: the same logical environment always hashes the same, regardless of which thunks have been evaluated.
	•	Cycle Handling (Decision 3): Recursive Nix sets (via rec) can produce cycles.  For hashing, we adopt a De Bruijn back‑reference scheme: if an object is seen again during traversal, emit a backref index based on the stack distance ￼ ￼.  This way distinct cycle shapes get distinct hashes.  We do not drop this approach: cyclic attrsets and mutually recursive functions should be cacheable and have stable hashes.  For serialization (cross‑eval storage), however, we continue to reject cycles ￼.  Deserializing arbitrary cycles is complex, so a cycle in a value will cause a “non-cacheable” error or skip.  (In practice this means very few Nix expressions will fail — only pathological recursions.)
	•	Position‑Dependent Builtins (Decision 4): Builtins like unsafeGetAttrPos and __curPos leak source locations.  In update 3 we recommended separating semantic vs positional results (like rust-analyzer’s AstId approach).  Given round 3 findings, we at least must immediately mark these primops as impure (ImpureReason::PositionDependent) so their results aren’t cached across evaluations ￼.  The longer-term strategy of a semantic evaluation layer (with positions overlaid later) is very complex and remains future work (Phase 5).
	•	Symbol Serialization (Decision 5): Each EvalState has its own symbol table, so integer IDs aren’t portable.  We confirm: when serializing (for cross‑eval cache) we should write symbols as UTF-8 strings and re-intern on load ￼.  This is straightforward and aligns with standard practice in other VMs (Lua, Erlang, etc.) ￼.  No change needed other than implementation.
	•	Cache Granularity (Decision 6): Per-thunk caching (fine-grained hashing) was too costly.  We keep the recommendation: cache at natural query/function boundaries.  In Nix, good boundaries are whole imports, function calls, or derivation outputs.  This reduces overhead and memory usage.  We will implement early cutoff: if an expression’s inputs haven’t changed, dependent results needn’t re-run ￼.  This will be an evolving design (Phase 3).
	•	Persistent Storage Backend (Decision 7): The plan chose LMDB for the main cache (hot key–value lookups) and SQLite for metadata/stats ￼.  Round 3 confirms Nix currently uses SQLite, but we should introduce LMDB for speed (e.g. lmdb++ C++ binding) and keep SQLite for schema-like data (durability levels, LRU timestamps).  We retain this two-store design (Phase 4).
	•	Alpha-Equivalence Hashing (Decision 8): The current expr-hash misses with‑bound variable names and nested scopes.  The solution (introduced in round 3) is Merkle-cached scope chain hashes: each ExprWith stores a scopeIdentityHash computed at bindVars(), so ExprVar can hash in O(1) time ￼.  We keep this.  Any remaining research-level improvements (e.g. PLDI 2024 techniques) are long-term and not in scope.
	•	Impurity Tracking (Decision 9): Currently any impurity (trace, getEnv, currentTime, etc.) invalidates all cached values via a single counter.  Round 3 proposed fine-grained tokens per effect type.  We agree this is ideal (Option B) but very complex.  For now we keep the coarse impureToken but note this is a limitation.  (Phase 5 lists effect sets as a future goal.)
	•	Serialization Format (Decision 10): We continue to use deterministic CBOR (§4.2) for cross-eval storage ￼.  This meets determinism, compactness, and flexibility requirements.  In particular, we define custom CBOR tags for Nix-specific types (e.g. tag 39 for strings-with-context, tag 40 for store paths) as sketched ￼.  We will ensure canonical encoding (sorted map keys, shortest ints, NaN normalization) per RFC 8949 ￼.  Functions/lambdas remain non-serializable: any value containing a function will be marked “non-cacheable” in the persistent cache ￼.
	•	Import and IFD Caching (Decision 11): We keep the hybrid strategy: for builtins.import, key by content hash for non-store paths, and by path alone for fixed store paths.  A simple L1 check by mtime/size with L2 fallback full hash will accelerate this (as sketched) ￼.  For IFD (import-from-derivation), we key on the derivation’s .drv content hash and verify outputs on lookup (recording store path and output hash) ￼.  This design stands.
	•	Thunk Hashing Strategy (Decision 12): We affirm: never force a thunk during hashing.  Hash a thunk by its identity (pointer to expr and env) ￼.  This preserves laziness and referential transparency: the hash doesn’t change if/when the thunk is evaluated ￼.  Our two-level cache uses this rule: L1 keys use pointer identities (plus epoch), L2 content keys use the thunk’s structural hash (based on expr+env).
	•	Merkle Scope Hashes (Decision 13, Round 3): Implemented as noted, computing ExprWith::scopeIdentityHash in bindVars() ￼.  This fixes nested with hashing in O(1) per ExprVar.  We use the parent’s cached hash so each new scope increments a Merkle-like hash chain ￼.
	•	Generation Counters (Decision 14, Round 3): To prevent stale pointer reuse, we embed an epoch in Value* using pointer tagging ￼.  On x86-64, we reserve the top 8 bits of each pointer for an epoch count (via sign-extension) ￼.  This lets us stamp each value with the current GC-generation byte.  On cache lookup we compare epochs.  (An alternative global-epoch approach was noted, but tagged pointers are efficient if supported on target architectures.)
	•	Closed-Term Optimization (Decision 15, Round 3): We implement Dolstra’s trick: mark closed subterms and skip substitution descent.  By computing Expr::is_closed during binding (via max-free-variable check) ￼, the evaluator can short-circuit: if a term is closed, replace it once and stop (rather than duplicating it repeatedly) ￼.  This yields massive performance gains on patterns with nested lambdas.

Overall, no major proposal is dropped.  Instead, some advanced ideas (durability, semantic layer, fine-grained effects) are deferred to Phase 5.  For completeness, we ensure any newly identified bugs are captured as tasks (e.g. marking positional builtins impure, adding tests for with behavior, and guarding against double-bindVars()).

Phased Implementation Roadmap

We retain the multi-phase structure.  Each phase lists concrete tasks (with file/area hints) and priorities, updated with round‑3 insights:

Phase 1: Correctness (Blocking)
	•	Fix with-bound variable hashing – In expr-hash.cc, include the variable’s name and full with-scope chain in the hash.  (Round 3 found that ExprWith::scopeIdentityHash must be included.) Priority: Blocking ￼.
	•	Add cycle detection in hashing – In env-hash.cc and value-hash.cc, track a stack of visited values.  On encountering a back-edge, emit a back-reference (as in Decision 3) instead of recursing.  Do not use a uniform marker. Priority: Blocking ￼ ￼.
	•	Add cycle detection in serialization – In value-serialize.cc, maintain a visited set of addresses.  On a cycle, throw an error (reject rec).  Priority: Blocking ￼ ￼.
	•	Add generation counters – Modify Value or its pointers to include a generation/epoch field (e.g. via tagged pointers ￼).  Use this to reject stale cache hits.  Priority: High ￼.
	•	Use disappearing links for cache cleanup – In eval.cc (or eval-inline.hh), after inserting a cache entry, call GC_general_register_disappearing_link on the stored Value* pointer.  This nulls the entry when the thunk is collected, avoiding unsafe finalizers.  Priority: High ￼ ￼.
	•	Periodic cache sweep – Regularly scan the cache to remove entries whose key’s pointer has become NULL (cleared by GC).  This avoids memory leaks from dead L1 entries.  Priority: Medium ￼.
	•	Mark position-dependent builtins impure – In primops.cc/eval.hh, mark unsafeGetAttrPos, __curPos, and similar position-related ops with ImpureReason::PositionDependent.  This ensures their outputs don’t get cached cross‑eval.  Priority: Medium ￼ ￼.
	•	Add tests for with hash bugs – Unit tests in expr-hash (or similar) to verify that with {x=1;y=2;}; x and with {x=1;y=2;}; y hash differently, and that different nesting orders also differ.  Catch the old bug.  Priority: High ￼.
	•	Ensure single bindVars() call – (Enforcing update3/round3 notes) Add an assertion or flag so that bindVars() is only executed once per Expr (e.g. by setting a bindingComplete flag) ￼.  This avoids hashing incomplete ASTs.  Priority: High.

Phase 2: Performance (High Impact)
	•	Implement two-level caching – Replace the existing single thunkMemoCache with:
	1.	L1 identity cache: a hash map keyed by (Expr*, Env*, uint64_t epoch) → Value* (fast, pointer-based).
	2.	L2 content cache: a map from structural hash (e.g. SHA256 of (expr_hash, env_hash)) → serialized/cached Value* (portable).
On force, check L1 first, then L2.  Priority: High ￼ ￼.
	•	AST hash-consing after bindVars() – Only intern/hook up AST nodes once they have all bindVars() fields set (e.g. level, fromWith, prevWith, scope hashes).  Use a two-phase parse→bind→intern approach to avoid hashing incomplete Exprs.  Priority: High.
	•	Add Merkle-cached scope hashes – In ExprWith::bindVars(), compute scopeIdentityHash = combine(attrs->getHash(), parent->scopeIdentityHash) (or root seed) as shown ￼.  Then ExprVar::getHash() can incorporate parentWith->scopeIdentityHash instead of walking the chain.  Priority: High.
	•	Add LRU eviction – Allow a maximum cache size.  On insert beyond capacity, evict the least-recently-used entry (updating any disappearing links).  This prevents unbounded memory growth.  Priority: High ￼.
	•	Closed-term optimization – As soon as bindVars() runs, set a bool is_closed on each Expr by checking if it has no free vars (using De Bruijn levels) ￼.  Modify substitution/evaluation to skip descending into an Expr when is_closed is true ￼.  Priority: High.
	•	Defer value interning to tenure – Only add evaluated Value results to any hash-cons pool if they survive a garbage collection (Appel’s hash-cons GC).  Short-lived values are not added.  This amortizes hashing cost.  Priority: Medium.
	•	Split cacheability predicates – In eval-inline.hh, separate the old valueIsUncacheable() into two checks: one for identity cacheability (intra-eval safe) and one for content cacheability (cross-eval safe).  (E.g. closures and external paths fail the latter.)  This aligns with Decision 1.  Priority: High.
	•	Never force thunks in hashing – Audit value-hash.cc and env-hash.cc to ensure no thunk is ever evaluated during hashing.  Use identity-based hashing (expr_ptr, env_ptr) for thunks, as in Decision 12 ￼.  Priority: High.
	•	Add tagged pointers for epoch – Implement the tagged-pointer scheme (e.g. top 8 bits) in value.hh ￼.  Use it to quickly extract Value* and epoch.  Priority: Medium.
	•	Instrumentation and stats – Hook up metrics counters (hits, misses, purities, etc., already present) to get concrete feedback on cache performance.  Visualize or log to identify pathological cases (e.g. thrashing).  Priority: Medium.

Phase 3: Architecture (Should Do)
	•	Fix env hash order‑dependence – Ensure our env-hash (for content keys) is stable by hashing thunks by identity and forced values by content ￼.  With two-level caching, this primarily affects any structural hash of Env we use for content keys or portability checks.  Priority: High.
	•	Portability classification fix – When checking if a value is cross-eval portable, ensure functions are rejected if any enclosing environment contains non-portable data.  That is, recursively examine lambdas’ closed-over values.  Priority: Medium.
	•	Early cutoff mechanism – Implement a system to record actual input hashes (or dependencies) for evaluations so that unchanged inputs skip downstream work (as in Build Systems à la Carte).  This may involve augmenting the “EvalTrace” of each cache entry with its input fingerprint.  Priority: Medium.
	•	Cache profiling – Add tools to profile cache behavior (e.g. LRU depth, entry lifetimes, memory usage).  Use this to tune data structures (e.g. switch from std::unordered_map to flat_map if needed).  Priority: Medium.
	•	Two-tier GC cache (strong/weak) – Instead of relying solely on disappearing links, consider a hybrid: a small strong cache for most-recent results (pinned values that are hard to recompute), plus a weak overflow (allow GC to reclaim stale entries).  This borrows from Nominal Adapton ideas.  Priority: Medium.
	•	Pure-eval fast path – Optimize pure mode by specializing or inlining impure checks.  (Round 3 noted using template specialization to make markImpure() a no-op in pure mode.)  This removes overhead when --pure-eval.  Priority: Medium.
	•	Document limitations – Clearly document that even with memoization, evaluation order and GC may affect hit rates (e.g. if some thunks aren’t memoized because never visited).  Set expectations and guidelines.  Priority: Low.

Phase 4: Cross-Evaluation (Nice-to-Have)
	•	Hash imports by content – In ExprParseFile, use the file’s content fingerprint (or NAR hash for derivations) instead of pointer for hashing imports ￼.  (Round 3 also advised converting ExprParseFile to use path.fingerprint().)  Priority: Medium.
	•	Implement import caching – Use the content-based key for builtins.import as above (store vs non-store paths).  Cache the resulting Value (force it once per unique content).  Priority: Medium.
	•	Implement IFD caching – Using the IFD design from Decision 11, cache import-from-derivation results keyed by .drv hash and verify outputs on load ￼.  Priority: Medium.
	•	Include EvalInputs in cache key – Hash the EvalInputs (NIX_PATH, system, flake lock, etc.) and incorporate into the persistent key so that changes there invalidate old entries.  (Round 3 noted only coarse impure mode was tracked today.)  Priority: Low.
	•	LMDB backend (hot cache) – Set up LMDB (with lmdb++ or C API) for the main thunk/content cache store ￼.  Create eval-cache-lmdb.cc/hh modules.  Ensure proper transactions (possibly one per storeResult, with WAL for concurrency).  Priority: Medium ￼.
	•	SQLite backend (metadata/stats) – Use an SQLite DB (e.g. metadata.sqlite) to record LRU timestamps, usage counts, and durability tiers.  This pairs with LMDB as a two-table design (action cache + CAS) ￼.  Priority: Low.
	•	Deterministic CBOR serialization – Replace the ad-hoc value-serialize.cc format with a canonical CBOR encoder (e.g. TinyCBOR).  Use custom tag numbers for Nix-specific types (we already defined 39/40) and ensure sorted maps/shortest encoding ￼.  Priority: Medium.
	•	LCOV/regression testing – Add tests to ensure cross-eval cache invariants: e.g. serializing then deserializing a value yields an equivalent value, and that no latent cycles sneak through.  Priority: Medium.

Phase 5: Advanced Optimizations (Future)
	•	Durability levels (Salsa) – Revisit the SALSA-style multi-level version vector to support partial invalidation (e.g. treat Nixpkgs stdlib as “HIGH durability”).  This would let us skip re-evaluating truly unchanged inputs.  Priority: Low ￼.
	•	Position-independent semantic layer – Architect a split where evaluation produces semantic results without source info, then a separate pass attaches positions for errors.  This achieves better caching by ignoring spurious changes in source layout.  Priority: Low.
	•	Fine-grained impurity tracking – Implement effect tokens as discussed (per-file, per-env-var, per-network, time) so that only relevant cached results are invalidated on change ￼.  This may require a dependency-tracking overhaul.  Priority: Low.
	•	Query-level caching – Further aggregate caching to the level of entire expressions or modules (e.g. cache a function+arguments as a single key), similar to GHC’s module cache.  Priority: Low.

Architectural Implications of Recent Insights
	•	Closure Serialization: We explicitly treat functions/closures as uncacheable across runs.  A lambda’s closure includes its environment of thunks (which may contain uncacheable builtins or other functions), so we will always skip serialization on values containing nFunction.  In practice, this means two functions are only memoized intra-eval; cross-machine or persistent caches will not store them.  (A future approach like Nominal Adapton might assign stable names, but that is beyond scope.)
	•	Cycle Handling: Our hashing strategy now distinguishes cyclic structures.  For example, a set { x = rec; y = rec; } hashes differently from { y = rec; x = rec; } even though both are cyclic.  By contrast, serialization will not handle such cycles: if a value is cyclic, cross-eval caching will skip it.  This trade-off simplifies implementation (no need for graph reference notation) at the cost of “cache bypass” on pathological inputs.
	•	CBOR Tagging: We introduce custom CBOR tags (e.g. 39 for contextual string, 40 for path) to represent Nix-specific data in the binary format ￼.  Every attribute set is encoded with sorted keys; integers use minimal bytes; floats and NaNs are canonical.  String deduplication (a symbol table prefix or reference table) ensures large derivation paths aren’t repeatedly serialized ￼.  Deterministic CBOR ties directly into content-addressing: the serialized bytes become the fingerprint for the content store.  We must ensure our CBOR library (TinyCBOR or similar) produces canonical output per RFC 8949 ￼.
	•	Environment Hashing: With the two-level cache, intra-eval lookups use raw Env* pointer identities (plus generation), so insertion/eviction is constant-time.  Cross-eval hashing, on the other hand, uses the structural hash of each forced value, and pointer identity for pending thunks ￼.  This combination means that changing evaluation order (forcing a different thunk first) does not change the final content-key: any thunk still hashes by its original (expr,env) identity until it is forced.  Thus, we achieve order-independence in practice.
	•	Purity & Inputs: Currently, all impurity is recorded by a single integer that bumps whenever any impure op occurs.  This is coarse: e.g. reading /etc/hosts or calling currentTime both just mark “impure” and would invalidate the same cache entries.  Ideally, we would track which inputs (files, env vars, time) a result used ￼ and include those in the cache key.  That way, a change in one input type only invalidates dependent results.  This is a significant undertaking and remains future work (Phase 5).
	•	Caching Scope: Empirically (as in Rust Analyzer), caching every thunk is too expensive.  By raising the granularity to whole expressions or functions, we rely on the fact that many calls (especially pure ones) will reuse results.  For example, if a function body contains many thunks but its final result hasn’t changed, our early-cutoff mechanism can skip re-evaluating those internal thunks.  In practice, we expect caching at the derivation or import level to yield the biggest wins with acceptable overhead ￼.

Open Questions for Nix Developers
	•	Implementation of Early Cutoff: How exactly should we structure the dependency tracking for early cutoff?  E.g. does each Thunk need to record which inputs it read (file paths, env var keys)?  What existing infrastructure (e.g. EvalTrace) can we reuse or extend?
	•	Fine-Grained Impurity Tokens: Can we leverage EvalInputs or AllowedUris to break out impurity?  For example, track a separate token per file path or scheme.  How would that integrate with the markImpure() calls in builtins?
	•	Shared Caching in Multithreaded Eval: If Nix ever does parallel evaluation, the L1 and L2 caches will need thread-safety.  Should we use concurrent maps or a lock strategy?  How to minimize contention?  (Round 3 noted GC_call_with_alloc_lock for disappearing links; but the cache itself also needs guarding.)
	•	String Deduplication Table: In CBOR serialization, we plan a symbol table for common strings (derivation paths, attr names).  How big can this table grow, and how to reset it?  Is it per serialization operation, or globally cached?  (A poor choice could use too much memory for many small caches.)
	•	GC Tag Usage: The tagged-pointer epoch scheme uses high bits of pointers.  On x86_64 we relied on sign extension; on ARM64 this is free via TBI ￼.  Do we need an abstraction to handle differences, or assume a common subset (e.g. use only top 8 bits)?
	•	Support for Serialized Functions: We currently disallow serializing any function.  If some functions are pure and closed (no imports, no builtins), could we devise a safe serialization (e.g. via de Bruijn encoding)?  Is that worth the complexity, or does it violate Nix’s semantics?
	•	Cache Eviction Tuning: The plan uses LRU eviction.  Should we consider other policies (e.g. time-to-live, size-based, or multiple queues)?  How to choose defaults to balance freshness vs memory?
	•	Testing on Real Workloads: How will these changes affect common Nix workflows (nix-build, nix eval, flakes)?  What benchmarks or tests should we run to validate that memoization is correct (no false hits) and beneficial (hit rates, speedup)?

These questions aim to clarify and guide the detailed implementation. Each touches on tricky aspects of integrating memoization into Nix’s evaluator. Continued collaboration between architecture/design and code maintainers will be crucial as we refine and test the system.

Sources: The above plan incorporates proposals from Nix Evaluator Memoization Infrastructure (Update 3) ￼ ￼ and the Round 3 answers document. Key ideas (CBOR tags, De Bruijn backrefs, tagged pointers, etc.) are drawn from those sources and linked inline. Behavioral notes about Nix (e.g. current impurity handling) come from the Nix codebase as cited in the round‑3 answers ￼ ￼.
```

## Claude

```markdown
# Nix Evaluator Memoization: Implementation Gaps and Recommendations

The comprehensive plan in update 3 addresses many correctness issues, but significant soundness holes, edge cases, and production readiness gaps remain. This report identifies **42 concrete recommendations** across ten critical areas that should be added to the implementation plan.

## Soundness verification reveals five critical edge cases

The plan correctly identifies `builtins.trace` as problematic, but several other builtins can observe or affect evaluation order in ways that compromise cache correctness.

**`builtins.seq` and `builtins.deepSeq`** force thunk evaluation but are generally safe for caching since they don't introduce non-determinism—they only change *when* evaluation happens, not *what* evaluates to. However, `deepSeq` on large structures like nixpkgs packages can trigger stack overflow (GitHub #7816) and expose latent errors that wouldn't surface under lazy evaluation. A cached result assuming lazy semantics may behave differently after `deepSeq`.

**`builtins.tryEval` creates cache key collision potential.** The same expression under `tryEval` can produce different `success` values depending on evaluation context:

```nix
let
  x = if condition then throw "error" else value;
in builtins.tryEval x  # Result depends on thunk evaluation order
```

The Nix documentation explicitly notes that error messages are omitted from `tryEval` results because they "risk bringing non-determinism into the evaluation result." This design decision hints at the deeper cache correctness challenges.

**Lazy trees v2 (PR #13225) introduces non-determinism** that breaks cache correctness fundamentally. The PR uses `StorePath::random()` for placeholder paths, making the language impure. As @roberth noted: "Using randomness or even fingerprints for placeholders makes the language non-deterministic and impure." Cache keys based on these random paths will never match across evaluations.

**`builtins.scopedImport` is explicitly non-memoized** by design. Unlike `import`, each call produces a distinct value because cache keys would need to include function-valued overrides, which can't be compared for equality.

### Recommendations for soundness

1. **Track `tryEval` in cache keys** by recording whether an expression is being evaluated under `tryEval` context
2. **Exclude expressions using `builtins.trace`, `currentTime`, or `scopedImport`** from persistent caching
3. **Gate lazy-trees caching** on the resolution of the determinism issue in PR #13225
4. **Implement "taint tracking"** for expressions using unsafe builtins like `unsafeDiscardStringContext`
5. **Add `deepSeq` depth limits** when evaluating for cache key computation to prevent stack overflow

## Concurrency and multi-process safety requires careful design

The plan proposes LMDB for persistent caching. LMDB provides **serializable isolation via MVCC** with a single-writer/multi-reader model that's well-suited to Nix's use case.

**LMDB transaction characteristics** relevant to the implementation:
- Writers don't block readers (readers see consistent snapshots via copy-on-write)
- Only one write transaction can be active at a time across all processes
- Multiple processes can safely open the same LMDB store via shared memory coordination
- Data pages use copy-on-write, so active pages are never overwritten

**Important: Nix currently uses SQLite (not LMDB)** for its store database with WAL mode. Any LMDB integration would be new infrastructure alongside the existing SQLite-based store.

**Concurrent thunk forcing** requires a black-holing pattern similar to GHC's SMP runtime:

```cpp
enum ThunkState {
    Unevaluated(Expression),
    Evaluating { owner: ThreadId },  // Black hole
    Evaluated(Value),
};

// Use CAS to claim thunk ownership; if another thread is evaluating, spin/wait
```

This prevents duplicate evaluation and detects infinite recursion when a thread re-enters its own thunk.

**Race conditions in reference counting** (from Bacon et al.'s "Concurrent Cycle Collection") manifest as "read-reclaim races" where a thread gains a pointer just before another thread deletes the last reference. Solutions include deferred reference counting with buffered increments/decrements and safety tests (Σ-test, Δ-test) to detect concurrent mutations during cycle detection.

### Recommendations for concurrency

6. **Serialize writes through nix-daemon** for simplicity—design for read parallelism with single writer
7. **Keep LMDB transactions short-lived** to avoid blocking page reuse and database growth
8. **Implement periodic `mdb_reader_check()`** to clean up stale reader transactions
9. **Use black-holing for concurrent `forceValue`** with CAS-based thunk ownership claiming
10. **Design for content-addressed keys** to avoid cache invalidation complexity—immutable entries never updated, only added
11. **Register GC roots atomically** with cache entry commits to prevent disappearing references

## Cache warming strategies from Bazel and Buck2

Bazel maintains three caching layers: in-memory analysis cache (most critical), action cache, and content-addressable store. The analysis cache is volatile and easily invalidated by flag changes—it requires a persistent JVM server to maintain warmth.

**FireCracker microVMs** provide an advanced pattern: snapshot Bazel JVM memory state, clone snapshots into multiple VMs, and start within milliseconds with full analysis cache preserved. This is ideal for CI worker pools.

**Buck2's "bottom-up" model** differs: action digests are computed from actual inputs, meaning all dependencies must be built/cached first to check the cache. This creates different warming dynamics than Bazel's top-down approach.

**Nix flake evaluation caching** currently only works at the CLI level, not within the evaluator. The `path:` flake references don't use the cache at all—only `git+file:` with committed changes enables caching.

### Recommendations for cache warming

12. **Implement lock file-based predictive warming**: parse old and new `flake.lock` on update, identify changed inputs, pre-evaluate dependent attributes
13. **Use content-addressed prefetching** with `narHash` from lock file as cache key prefix
14. **Create a background warming daemon** that watches for `flake.lock` changes and warms `devShells` → `packages` → `nixosConfigurations` by priority
15. **Integrate with CI** using a pattern similar to Bazel: CI populates cache on main branch builds, developers read
16. **Implement "stale-while-revalidate"** where cached content is served immediately while background revalidation runs

## Migration path should follow rust-analyzer's salsa evolution

Rust-analyzer's migration to "new Salsa" (merged March 2025, PR #18964) demonstrates effective multi-phase migration for incremental computation frameworks.

**Key migration strategies used:**
- Created `db-ext-macro` as a compatibility layer mimicking old Salsa's query group pattern
- Phased memory optimization: 8GB → 4GB → 2.2GB through multiple optimization passes
- Introduced **durability tiers** dividing queries into "more durable" (stdlib) vs "more volatile" (user code)

**Salsa's durability system** uses version vectors instead of single version numbers, avoiding traversal of the entire query graph for changes in volatile code. This reduced incremental recomputation overhead from ~300ms to minimal.

**Nix's experimental features system** (`experimental-features = nix-command flakes`) provides an ideal mechanism for gradual rollout. The lifecycle is: idea → PR → experimental (with flag) → stable OR removed.

### Recommendations for migration

17. **Use Nix experimental features system** for cache migration with flag like `two-level-cache`
18. **Implement shadow mode first**: new cache runs in parallel, only reads, compares results, logs discrepancies
19. **Design salsa-inspired durability tiers**: HIGH for nixpkgs stdlib (cache indefinitely), MEDIUM for third-party flakes (TTL 24h), LOW for user code (per-evaluation only)
20. **Maintain rollback capability** via feature flag toggle with daemon restart
21. **Set automatic rollback triggers**: >5% correctness errors OR >20% performance regression

## Debugging and observability builds on NIX_SHOW_STATS

`NIX_SHOW_STATS=1` already outputs detailed evaluation statistics including thunk counts, primop calls, function calls, and Boehm GC heap information. The undocumented `NIX_SHOW_STATS_PATH` outputs to JSON.

**OpenTelemetry integration** would use:
- `Counter` for hits, misses, evictions (monotonically increasing)
- `Gauge` for current cache size, memory usage  
- `Histogram` for cache operation latency distribution
- Exemplars linking metrics to traces for debugging latency spikes

**Chrome DevTools-style profiling** in V8 exposes compilation events in Performance panel flamecharts. Similar infrastructure for Nix could provide thunk evaluation timelines and memoization hit/miss overlays.

### Recommendations for observability

22. **Extend NIX_SHOW_STATS** with memoization fields: `tableLookups`, `cacheHits`, `cacheMisses`, `hitRate`, `tableSize`, `evictions`
23. **Add NIX_MEMO_TRACE environment variable** for detailed structured trace logging of cache operations
24. **Implement thrashing detection heuristics**: alert if hit rate drops below 50% after warmup, log warning if eviction rate exceeds insertion rate
25. **Design metrics to be OpenTelemetry-exportable**: counters, gauges, histograms with standard naming
26. **Add debug assertions in non-NDEBUG builds** validating cache invariants (size ≤ capacity, no duplicate keys, LRU ordering consistent)

## Memory pressure handling adapts V8 and Boehm GC patterns

V8 implements **memory reduction mode** for low-memory devices with aggressive GC tuning: less slack in heap growing strategy, hard limits forcing incremental marking completion, and aggressive compaction. V8 reduced heap page size from 1MB to 512KB to reduce fragmentation by up to 2×.

**Boehm GC (used by Nix)** provides limited pressure signals compared to V8:
- `GC_get_heap_size()` for polling current heap size
- `GC_set_max_heap_size()` for hard limits
- `GC_register_finalizer()` for cleanup callbacks
- No direct memory pressure callback—must poll and detect manually

**ARC (Adaptive Replacement Cache)** from IBM provides self-tuning without workload-specific parameters by maintaining two LRU lists (recent and frequent) with ghost lists tracking recently evicted items. Used by OpenZFS, it outperforms simple LRU by 2-10× on real workloads.

### Recommendations for memory pressure

27. **Implement pressure detection layer** with soft limit (70-80% of max heap) and hard limit (85-90%)
28. **Define eviction strategy by priority**: reproducible entries first, then low access count, then LRU
29. **Scale eviction intensity by pressure level**: 10% at PRESSURE_LOW, 25% at PRESSURE_HIGH, 50% at PRESSURE_CRITICAL with forced `GC_gcollect()`
30. **Consider ARC-inspired dual-list design** with adaptive sizing based on ghost hit patterns
31. **Use `GC_malloc_atomic()`** for cache entry keys/metadata containing no pointers to reduce false retention

## Flake lock integration provides durability signals

The `flake.lock` structure contains rich durability information in the `locked` attribute:

```json
{
  "locked": {
    "narHash": "sha256-OnpEWzNxF/AU4KlqBXM2s5PWvfI5/BS6xQrPvkF5tO8=",
    "rev": "7f8d4b088e2df7fdb6b513bc2d6941f1d422a013",
    "lastModified": 1580555482
  }
}
```

**`narHash` is the authoritative content identifier**—a SHA-256 hash of the NAR serialization that includes file contents, permissions, and directory structure (but not timestamps). This provides content-based identity independent of git history or hosting location.

**Important caveat**: NAR hashes may differ between platforms in edge cases (GitHub #6731 reported system-dependent narHash in flake.lock).

### Recommendations for flake lock durability

32. **Use `narHash` as primary cache key** for flake inputs rather than `rev` (which can be mutable via force-push)
33. **Parse `flake.lock` early** in evaluation to extract durability information for all inputs
34. **Implement automatic HIGH durability** for inputs matching common nixpkgs patterns (owner: "NixOS", repo: "nixpkgs")
35. **Propagate durability transitively**: if input A (HIGH) depends on input B (unknown), B inherits HIGH

## String context requires explicit cache key inclusion

**Two strings with identical characters can have different contexts**, leading to different derivations when used in build scripts. The string `"/nix/store/abc-hello"` as a plain string behaves differently than the same path obtained from `"${pkgs.hello}"` because the latter has context tracking the hello derivation dependency.

**Critical for cache correctness**: Nix string equality (`==`) only compares character content, NOT context. This means standard equality checks are insufficient for cache key comparison.

**`builtins.unsafeDiscardStringContext`** creates a copy with empty context, breaking dependency tracking. Known bug: it imports paths into the store even though the result has no context (GitHub #3358).

### Recommendations for string contexts

36. **Include serialized string context in cache keys** using `builtins.getContext` output
37. **Hash must include both**: `hash(string_content) + hash(sorted(context_elements))`
38. **Track `unsafeDiscardStringContext` usage** as cache invalidation boundary—consider the result "tainted"
39. **For IFD results, key by derivation hash** (the `.drv` file hash) plus `builtins.currentSystem`

## Floating-point determinism requires canonicalization

Native CPU floating-point operations are **NOT portable deterministic** (GitHub #8274). Different results occur across platforms, compiler settings, and even debug vs release builds. Sources of non-determinism include: compiler rearranging associative operations, x87 using 80-bit extended precision internally, SIMD vs scalar giving different results, and fused multiply-add availability.

**NaN has 2^52 different bit patterns** in IEEE 754, and NaN ≠ NaN by definition. Content-addressed systems like Git and IPFS hash raw bytes, so representation differences produce different hashes.

### Recommendations for floating-point

40. **Canonicalize floats before hashing**: map all NaNs to canonical quiet NaN (`0x7FF8000000000000`), normalize -0.0 to +0.0
41. **Consider string representation** with fixed precision for cache keys, or convert to fixed-point integers
42. **Include platform/architecture in cache keys** if floating-point operations cannot be avoided

## Test suite design combines property testing, fuzzing, and benchmarks

**Nix already uses RapidCheck** (C++ QuickCheck) with GoogleTest integration. Key cache properties to test:

- **Soundness**: `evaluate(expr) == evaluateWithCache(expr)` for all expressions
- **Idempotency**: repeated cache lookups return same value
- **Consistency**: `cache.put(k, v); cache.get(k) == v`
- **Cycle-freedom**: no reference cycles in cached dependency graphs

**libFuzzer integration** should target:
- Expression parser (find crash-inducing inputs)
- Cache key computation (find inputs producing same hash with different semantics)
- Derivation serialization (find round-trip failures)

**Statistical rigor for benchmarks** requires at least 30 samples for CLT to apply, confidence intervals (not just means), warm-up iteration discarding, and dedicated hardware with turbo boost disabled.

### Test infrastructure structure

```
src/tests/
├── libexpr-tests/
│   ├── cache_properties.cc      # RapidCheck: soundness, idempotency
│   └── memoization_properties.cc
├── libstore-tests/
│   └── content_addressed_properties.cc
└── libexpr-fuzz/
    ├── expr_parser_fuzz.cc      # libFuzzer targets
    └── cache_key_fuzz.cc
```

**Example RapidCheck property**:

```cpp
RC_GTEST_PROP(CacheProperties, Soundness, (const Expr& expr)) {
    auto direct = evaluate(expr);
    auto cached = evaluateWithCache(expr);
    RC_ASSERT(direct == cached);
}
```

---

## Summary: Priority ranking of recommendations

**Must-have for correctness** (blockers):
- Include string context in cache keys (#36-38)
- Track `tryEval` evaluation context (#1)
- Canonicalize floats before hashing (#40-41)
- Implement concurrent forceValue with black-holing (#9)

**Must-have for production** (critical):
- Extend NIX_SHOW_STATS with memoization metrics (#22)
- Implement pressure detection and eviction (#27-29)
- Design for content-addressed immutable entries (#10)
- Add property-based soundness tests (#42)

**Should-have for adoption** (important):
- Use experimental features system for rollout (#17)
- Implement shadow mode for safe deployment (#18)
- Cache warming based on lock file changes (#12-14)
- Serialize writes through nix-daemon (#6)

**Nice-to-have for optimization** (desirable):
- ARC-inspired dual-list adaptive sizing (#30)
- OpenTelemetry-ready metrics design (#25)
- FireCracker-style cache state snapshots for CI
- Durability tier propagation (#35)

These 42 recommendations address the remaining implementation gaps for production-ready Nix evaluator memoization. The highest priority items—string context handling, floating-point canonicalization, and concurrent evaluation safety—should be resolved before any broader rollout.
```

## Kagi

```markdown
## Round 4: Critique and Updated Plan for Nix Evaluator Memoization

### Executive Summary

The Round 3 answers clarify critical implementation details, but reveal a **timing conflict** between Merkle-cached scope hashes and hash-consing. The plan proposes computing `scopeIdentityHash` during `bindVars()` using `attrs->getHash()`, but hash-consing happens *after* `bindVars()`, so child hashes aren't available yet.

Key issues requiring resolution:

1. **Three-phase processing required** — Parse → bindVars → computeHashes → intern
2. **Generation counter strategy** — Recommend composite key over validation-on-lookup
3. **Cache memory management** — Clarify interaction between LRU eviction and disappearing links
4. **Salsa durability mapping** — Concrete mapping of Nix inputs to durability levels

---

### Critical Issue 1: Merkle Scope Hash Timing Conflict

**Problem**: The plan proposes computing `scopeIdentityHash` during `bindVars()`: 

```cpp
void ExprWith::bindVars(StaticEnv& env) {
    scopeIdentityHash = combine(
        attrs->getHash(),  // ← Requires attrs to be hash-consed already!
        env.parentWith->scopeIdentityHash
    );
}
```

But hash-consing happens *after* `bindVars()` completes. [^1] So `attrs->getHash()` isn't available during `bindVars()`.

**Solution**: **Three-phase processing** instead of two-phase:

```cpp
// Phase 1: Parse (creates AST)
Expr* parsed = parser.parse(source);

// Phase 2: Bind variables (sets level, displ, fromWith, parentWith, prevWith, is_closed)
parsed->bindVars(*this, staticEnv);

// Phase 3: Compute hashes bottom-up (NEW)
parsed->computeHashes();  // Sets scopeIdentityHash, exprHash

// Phase 4: Intern (uses computed hashes)
Expr* interned = exprInternTable.intern(parsed);
```

**Phase 3 implementation**:

```cpp
void ExprWith::computeHashes() {
    // Children first (bottom-up)
    attrs->computeHashes();
    body->computeHashes();
    
    // Now compute scope hash
    if (parentWith) {
        scopeIdentityHash = combine(
            SCOPE_CHAIN_TAG,
            attrs->exprHash,
            parentWith->scopeIdentityHash
        );
    } else {
        scopeIdentityHash = combine(SCOPE_ROOT_TAG, attrs->exprHash);
    }
    
    // Compute own hash
    exprHash = combine(EXPR_WITH_TAG, attrs->exprHash, body->exprHash, scopeIdentityHash);
}

void ExprVar::computeHashes() {
    if (fromWith) {
        exprHash = combine(EXPR_VAR_WITH_TAG, name.hash(), fromWith->scopeIdentityHash);
    } else {
        exprHash = combine(EXPR_VAR_TAG, level, displ);
    }
}
```

**Why bottom-up**: Parent expressions need child hashes. The `computeHashes()` traversal visits children before computing the parent's hash.

---

### Critical Issue 2: Generation Counter Strategy

The Round 3 answers propose two options for generation counters: [^1]

| Option | Approach | Pros | Cons |
|--------|----------|------|------|
| A: Composite key | `MemoKey = {hash, epoch}` | Simple lookup, no validation | Larger keys, more memory |
| B: Validation on lookup | Store epoch in entry, check on lookup | Smaller keys | Extra branch on every lookup |

**Recommendation: Option A (Composite Key)**

Rationale:
1. **Correctness by construction** — Stale entries are simply not found (different key)
2. **No validation overhead** — Hash table lookup is already O(1)
3. **Memory overhead is minimal** — 8 bytes per key vs. 8 bytes per entry

**Implementation**:

```cpp
struct MemoKey {
    StructuralHash hash;
    uint64_t epoch;
    
    bool operator==(const MemoKey& o) const {
        return hash == o.hash && epoch == o.epoch;
    }
};

struct MemoKeyHash {
    size_t operator()(const MemoKey& k) const {
        return std::hash<StructuralHash>{}(k.hash) ^ (k.epoch * 0x9e3779b97f4a7c15ULL);
    }
};

// Global epoch, incremented on each GC cycle
std::atomic<uint64_t> globalEpoch{0};

// Hook into GC
void onGCComplete() {
    globalEpoch++;
}
```

**For identity cache (L1)**, use pointer + epoch:

```cpp
struct IdentityKey {
    Expr* expr;
    Env* env;
    uint64_t epoch;
};
```

---

### Critical Issue 3: Cache Memory Management Strategy

The plan mentions both **LRU eviction** and **disappearing links**, but their interaction is unclear.

**Recommended strategy: Two-tier with different policies**

| Tier | Purpose | Eviction Policy | GC Integration |
|------|---------|-----------------|----------------|
| **L1 (Identity)** | Fast intra-eval lookup | LRU with size limit | Disappearing links |
| **L2 (Content)** | Cross-eval persistence | Time-based expiry | None (serialized) |

**L1 Cache Implementation**:

```cpp
class IdentityCache {
    static constexpr size_t MAX_SIZE = 100000;
    
    // LRU list: front = most recent, back = oldest
    std::list<IdentityKey> lruOrder;
    
    // Map: key → (value, lru_iterator)
    boost::unordered_flat_map<IdentityKey, 
        std::pair<Value*, std::list<IdentityKey>::iterator>> cache;
    
public:
    void insert(IdentityKey key, Value* value) {
        // Evict oldest if at capacity
        while (cache.size() >= MAX_SIZE) {
            auto oldest = lruOrder.back();
            // Unregister disappearing link before removal
            GC_unregister_disappearing_link(&cache[oldest].first);
            cache.erase(oldest);
            lruOrder.pop_back();
        }
        
        lruOrder.push_front(key);
        cache[key] = {value, lruOrder.begin()};
        
        // Register disappearing link: null value when key's env is collected
        GC_general_register_disappearing_link(
            reinterpret_cast<void**>(&cache[key].first),
            key.env);
    }
    
    Value* lookup(IdentityKey key) {
        auto it = cache.find(key);
        if (it == cache.end()) return nullptr;
        if (it->second.first == nullptr) {
            // Value was collected, remove stale entry
            lruOrder.erase(it->second.second);
            cache.erase(it);
            return nullptr;
        }
        // Move to front (most recently used)
        lruOrder.splice(lruOrder.begin(), lruOrder, it->second.second);
        return it->second.first;
    }
};
```

**Periodic sweep** (triggered by GC or allocation count):

```cpp
void IdentityCache::sweep() {
    for (auto it = cache.begin(); it != cache.end(); ) {
        if (it->second.first == nullptr) {
            lruOrder.erase(it->second.second);
            it = cache.erase(it);
        } else {
            ++it;
        }
    }
}
```

---

### Critical Issue 4: Salsa Durability Mapping to Nix

Salsa's durability system uses **version vectors** to skip revalidation of stable subgraphs. [^1] This maps well to Nix's input categories.

**Durability levels for Nix**:

| Level | Nix Inputs | Rationale |
|-------|------------|-----------|
| **HIGH** | Store paths (`/nix/store/...`), Nixpkgs channel | Content-addressed, immutable |
| **MEDIUM** | User flakes, local `.nix` files | Change during development |
| **LOW** | `$NIX_PATH`, env vars, `builtins.currentTime`, network fetches | Can change at any time |

**Version vector implementation**:

```cpp
struct VersionVector {
    uint64_t low;    // Incremented on any impure operation
    uint64_t med;    // Incremented on local file change
    uint64_t high;   // Incremented on store path change (rare)
    
    void incrementLow()  { low++; }
    void incrementMed()  { low++; med++; }
    void incrementHigh() { low++; med++; high++; }
};

struct CacheEntry {
    Value* result;
    uint8_t durability;  // 0=LOW, 1=MED, 2=HIGH
    uint64_t version;    // Version at time of computation
};

bool isValid(const CacheEntry& entry, const VersionVector& current) {
    switch (entry.durability) {
        case 0: return entry.version == current.low;
        case 1: return entry.version == current.med;
        case 2: return entry.version == current.high;
    }
}
```

**Durability propagation**: A derived value's durability is the **minimum** of its inputs' durabilities. [^1]

```cpp
uint8_t computeDurability(const std::vector<CacheEntry*>& inputs) {
    uint8_t minDurability = 2;  // HIGH
    for (auto* input : inputs) {
        minDurability = std::min(minDurability, input->durability);
    }
    return minDurability;
}
```

---

### Critical Issue 5: `with` Chain Hashing Completeness

The Round 3 answers confirm that `parentWith` is acyclic (forms a strict tree). [^1] This simplifies hashing—no cycle detection needed for scope chains.

**Complete `ExprVar` hashing for `with`-bound variables**:

```cpp
void hashExprVar(HashSink& sink, ExprVar* var) {
    if (var->fromWith) {
        // With-bound: hash name + scope chain identity
        feedTag(sink, EXPR_VAR_WITH_TAG);
        feedSymbol(sink, var->name);
        feedBytes(sink, var->fromWith->scopeIdentityHash);  // Merkle-cached
    } else {
        // Lexically bound: De Bruijn indices
        feedTag(sink, EXPR_VAR_TAG);
        feedUInt32(sink, var->level);
        feedUInt32(sink, var->displ);
    }
}
```

**Why this is correct**: The `scopeIdentityHash` encodes the entire chain of `with` expressions (via Merkle hashing), so different scope chains produce different hashes even if they have the same nesting depth.

---

### Updated Implementation Plan

#### Phase 1: Correctness (Blocking)

| Task | Priority | Notes |
|------|----------|-------|
| Implement three-phase processing | **Blocking** | Parse → bindVars → computeHashes → intern |
| Fix `with`-bound variable hash | **Blocking** | Use `scopeIdentityHash` from Merkle chain |
| Add cycle detection with De Bruijn indices | **Blocking** | For value hashing, not scope chains |
| Add generation counters (composite key) | **High** | Global epoch incremented on GC |
| Use disappearing links for L1 cache | **High** | Register on insert, unregister on evict |
| Add LRU eviction to L1 cache | **High** | Configurable max size (default 100K) |
| Add periodic cache sweep | **Medium** | Triggered by GC or allocation count |

#### Phase 2: Performance (High Impact)

| Task | Priority | Notes |
|------|----------|-------|
| Implement two-level caching | **High** | L1: identity (fast), L2: content (portable) |
| Implement Merkle-cached scope hashes | **High** | Compute in Phase 3 (computeHashes) |
| Implement AST hash-consing | **High** | In Phase 4 (intern), after hashes computed |
| Implement closed-term optimization | **High** | Set `is_closed` flag during bindVars |
| Split cacheability predicates | **High** | `isIdentityCacheable()` vs `isContentCacheable()` |

#### Phase 3: Architecture (Should Do)

| Task | Priority | Notes |
|------|----------|-------|
| Implement durability levels | **High** | Version vector (low, med, high) |
| Implement early cutoff | **High** | Skip subgraphs when durable inputs unchanged |
| Add cache statistics | **Medium** | Hit rates, overhead, per-durability stats |
| Document order-dependence limitations | **Low** | Set expectations for hit rates |

#### Phase 4: Cross-Eval (Nice to Have)

| Task | Priority | Notes |
|------|----------|-------|
| Add LMDB backend for L2 cache | **Medium** | Persistent content-addressed storage |
| Wire EvalInputs fingerprint | **Low** | Include in L2 cache key |
| Implement import caching | **Medium** | Content hash for files, path for store |

---

### Questions for the Codebase Agent (Round 4)

#### Three-Phase Processing

1. **Where should `computeHashes()` be called?** After `bindVars()` in `parseExprFromString()`, or in a separate pass?

2. **Is there an existing post-order traversal of the AST?** Or would `computeHashes()` need to implement its own traversal?

3. **How large is the AST for a typical Nixpkgs evaluation?** This affects whether we can afford an extra traversal pass.

#### Hash-Consing

4. **What is the current memory layout of `Expr` nodes?** Is there room for an `exprHash` field without increasing allocation size?

5. **Are there any `Expr` nodes that are shared across multiple parse trees?** Or is each parse tree independent?

6. **How would hash-consing interact with `Exprs::add()`?** Would we replace it with an interning lookup?

#### GC Integration

7. **How do we hook into Boehm GC's collection cycle?** Is there a callback mechanism for incrementing the global epoch?

8. **What is the typical GC frequency during a Nixpkgs evaluation?** This affects how often the epoch increments.

9. **Is `GC_unregister_disappearing_link` available?** We need it for LRU eviction cleanup.

#### Durability

10. **Where are store path accesses tracked?** Is there a central point where we can mark values as HIGH durability?

11. **How does `builtins.readFile` currently work?** Does it record the file path anywhere we could use for durability tracking?

12. **Is there existing infrastructure for tracking which inputs a value depends on?** The Round 3 answers mention `impureToken` but not fine-grained tracking.

#### Performance

13. **What is the overhead of the current `computeThunkStructuralHash`?** Specifically, how much time is spent in SHA256 vs. traversal?

14. **Are there benchmarks for the three-phase processing overhead?** Adding an extra AST traversal has a cost.

15. **What is the typical cache hit rate for L1 (identity) vs L2 (content)?** This affects the relative importance of each cache tier.

---

### Summary of Key Changes from Round 3

| Round 3 Plan | Round 4 Change | Rationale |
|--------------|----------------|-----------|
| Two-phase (parse → bindVars → intern) | **Three-phase** (parse → bindVars → computeHashes → intern) | Merkle hashes need child hashes, which aren't available during bindVars |
| Generation counter: two options | **Composite key recommended** | Correctness by construction, no validation overhead |
| LRU + disappearing links (unclear interaction) | **Clear two-tier strategy** | L1: LRU + disappearing links, L2: time-based expiry |
| Durability mentioned | **Concrete version vector implementation** | Maps Nix inputs to Salsa-style durability levels |

The most critical change is the **three-phase processing**—without it, Merkle-cached scope hashes cannot be computed correctly. The second priority is the **composite key generation counter**, which provides correctness by construction rather than relying on validation.

[^1]: [round-3-answers](round-3-answers.md) (100%)
```