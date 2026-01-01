# Prompt

This is a continuation of round-6.md.

Please read all responses. You should fetch and read the sources referenced to ensure accuracy. You should also list those sources. Then, update your plan where appropriate, being sure to include alternative designs or implementations and their tradeoffs, as well as additional references. Lastly, answer any questions posed by any of the responses in a new file, round-7-answers.md.

Since there are a lot of steps here, please make a todo list and use it to keep track of your finished work and remaining work as you proceed.

# Responses

## ChatGPT

```text
Nix Evaluator Memoization Infrastructure – Revised Plan

Goal and Scope

The goal is to implement thunk memoization in the Nix evaluator to improve performance while preserving correctness. This involves enabling three levels of caching:
	1.	Within-evaluation caching – reuse results when the same thunk is forced multiple times during a single evaluation (intra-process) ￼.
	2.	Cross-evaluation caching – persist evaluation results to reuse across separate nix eval invocations (same machine, across processes) ￼.
	3.	Cross-machine caching – ultimately, share cached results between machines (portability across Nix store paths and platforms) ￼ ￼.

Current Status: Preliminary changes for within-evaluation caching exist, but there are correctness bugs and architectural limitations blocking wider use ￼. Cross-evaluation caching is not yet fully implemented, and cross-machine reuse remains a long-term objective. This plan treats portability (across machines and store paths) as a core requirement, influencing design decisions around content-addressed hashing and serialization.

Design Overview and Key Decisions

To achieve the above goals, the plan introduces several fundamental changes to Nix’s evaluator design, incorporating lessons learned from recent research and the community:
	•	Three-Phase AST Processing: Extend Nix’s evaluation pipeline from two phases (parse → bindVars) to three phases (parse → bindVars → computeHashes → intern) ￼. In the new computeHashes phase, each AST node computes a structural hash of its subtree and environment. This yields Merkle-style hash caching for scope chains (lexical environments) and expressions, enabling O(1) comparisons later ￼ ￼. The overhead of this extra phase is estimated to be ~10% of parse time (e.g. ~0.5s on a 5s parse of Nixpkgs) – a one-time cost per parse, not per evaluation, which is acceptable given the speedups it enables ￼. These hashes are stored in AST nodes for reuse. Rationale: Precomputing hashes bottom-up allows constant-time environment identity checks and avoids repeated costly traversals during caching.
	•	Comprehensive Cache Keys: The memoization key for each thunk will incorporate all factors that can affect its result. In addition to the expression and environment pointers, this includes the evaluation context and other dynamic parameters. Notably, the plan will include string context (the set of relevant derivations or context for strings) in cache keys, track tryEval depth (to distinguish evaluations inside tryEval which can yield different outcomes), and canonicalize floating-point values (e.g. treat NaNs uniformly, normalize -0 to +0) before hashing ￼. By canonicalizing floats and capturing string context, we ensure that cache hits are portable and correct across platforms – e.g. two strings differing only in context or two numerically equal floats differing in bit representation won’t erroneously collide ￼ ￼. These measures guarantee cross-platform determinism in hashing and prevent subtle cache mismatches.
	•	Identity vs Content Two-Tier Caching: We adopt a two-tier caching strategy ￼. L1 (identity cache) is an in-memory cache keyed by object identity (the exact Expr* and Env* pointer of a thunk) and is used within a single evaluation for fast deduplication. L2 (content cache) is a persistent, content-addressed cache keyed by a structural hash of the thunk (including its environment and dependencies), used across evaluations and even across machines. This separation is crucial: L1 provides O(1) lookups for identical thunks during one run (even if they appear in multiple places), while L2 allows reuse when the same computation is encountered in a new run or on another machine ￼ ￼. The design recognizes that L1 and L2 serve different purposes – L1 optimizes repeated work in one run, and L2 captures pure, deterministic computations for long-term reuse ￼. We expect typical L1 hit rates of 30–70% (many thunks forced repeatedly in large evaluations), and L2 hit rates of 80–95% for repeated evaluations with unchanged inputs ￼.
	•	Generation Counters for GC Safety: To safely use pointer identity in caching without leaking memory or incorrect hits after garbage collection, the plan uses a generation counter strategy. We introduce a global epoch that increments on each GC cycle, and incorporate this into cache keys (as a composite {hash, epoch} key) ￼ ￼. Each cache entry is thus qualified by the “generation” of the heap it was created in. If a Value is garbage-collected, its epoch will expire and entries with the old epoch become unreachable (they won’t be erroneously reused for a new object that happens to have the same pointer). This “correctness by construction” approach avoids explicit validity checks on each lookup ￼. Concretely, an IdentityKey will consist of (exprPtr, envPtr, epoch) for L1, and the content-hash keys for L2 will also include epoch or versioning as needed for impure entries ￼.
	•	Memoization Memory Model and GC Integration: Nix’s evaluator currently uses a custom garbage collector (Boehm GC) for Value allocation. We will integrate the memoization cache with the GC via “disappearing links” (weak references) so that cached results don’t prevent Values from being freed ￼. Before inserting a thunk’s result into the cache, we register its pointer as a disappearing link with Boehm GC. When the GC runs and is about to collect that Value, the link is zeroed, allowing us to detect stale cache entries and purge them. This approach is preferred over finalizers, because Boehm’s finalizers do not handle reference cycles well (cycles involving finalizable objects may never be collected) ￼ ￼. In essence, the cache will automatically invalidate entries whose values have been garbage-collected, using the GC’s callback hook to bump the generation counter ￼. We also plan to add memory pressure monitoring – e.g. if heap usage grows too large, aggressively evict cache entries – though specifics will depend on tuning (see Phase 2).
	•	Thread-Safety and Thunk Forcing Mechanism: In a multi-threaded evaluation scenario, forcing the same thunk concurrently could lead to duplicate work or race conditions. We address this by implementing black-holing in forceValue: when a thread starts evaluating a thunk, it atomically marks it as in-progress (e.g. by storing a special “black hole” token or using a CAS on a status flag) ￼. Any other thread attempting to force the same thunk will see this and wait (or get a cache miss) until the first thread finishes. Once a thunk is computed, the result is stored (memoized) so both threads can use it. This prevents duplicate evaluation and infinite loops in self-referential thunks. Furthermore, we will consider in-place thunk updates (similar to GHC’s approach of updating thunks with their value) to avoid extra indirection on subsequent forces ￼. Currently, Nix uses a separate memo cache mapping thunks to their results rather than altering the thunk’s Value in place. With careful GC coordination, we may update the thunk object itself to point to its value after evaluation (as long as type tagging and memory layout allow) to streamline future accesses. These changes to the thunk forcing mechanism and cache storage improve both correctness and performance in a concurrent setting.
	•	Durability and Impurity Tracking: To make cross-evaluation caching sound, we need to know when a cached result is valid to reuse. Nix evaluations can depend on external factors (filesystem, network, time, etc.). We plan to classify inputs by durability levels ￼, drawing inspiration from Salsa’s durable incrementality. For example, pure computations depending only on immutable Nix store paths or fixed-input derivations might be marked HIGH durability (cache indefinitely), whereas those depending on non-store file I/O could be MEDIUM (cache invalidated when the file changes) and those reading environment variables or current time as LOW (valid only within the same evaluation) ￼ ￼. Nix’s existing impureToken mechanism is too coarse (a single global counter) ￼, so we will extend it by recording fine-grained dependencies for each thunk’s value. Specifically, an EvalTrace structure will record all inputs a value depends on (store paths, file paths, env vars, etc. along with their durability) during evaluation ￼. The memoization cache can then use a version vector of relevant inputs as part of the cache key or invalidation criteria ￼. If any high-durability input (like a Nix store path content hash) changes – which should be rare for truly immutable inputs – or if medium-durability inputs have changed timestamps, we can invalidate or skip using the cached result. This approach avoids explicit manual invalidation in most cases: by incorporating the semantic hash of dependencies into the key (see semi-semantic hashing below), cached results naturally become invalid when their dependencies change ￼. In summary, we treat evaluation like a build with tracked inputs: if the inputs haven’t changed, we can reuse the result.
	•	Portability Across Store Paths and Machines: A core requirement for cross-machine caching is that cache keys do not embed machine-specific or store-path-specific identifiers that differ on another machine. To achieve this, all content-based keys rely on content fingerprints rather than raw pointers or store paths. Nix’s SourceAccessor provides a fingerprint() for paths, which we will use to hash file imports and store paths ￼ ￼. For example, an ExprParseFile (AST node representing an import) will be hashed by the content fingerprint of the file it points to, rather than by the pointer address of the AST node or a store path string ￼. This way, two machines importing identical files (or the same Nix store path copied between machines) produce the same hash. We also plan deterministic serialization of values (using CBOR with a fixed ordering) for storing cache entries, ensuring that a value serialized on one machine can be deserialized and recognized as the same on another ￼. Combined with the canonicalization of floats (to handle potential cross-platform floating point differences) and possibly including a platform tag in the cache key if truly necessary ￼, these measures ensure that a cached result is valid on any machine that has the same dependency content. In effect, the content-addressed L2 cache acts like a mini build artifact cache for evaluator results, paralleling how Nix store paths allow binaries to be shared between systems.
	•	Testing and Validation: Given the complexity of adding memoization to a lazy evaluator, rigorous testing is essential. We will add property-based tests using RapidCheck (a C++ QuickCheck library) to validate that evaluation with caching yields the same results as normal evaluation in all cases ￼. For example, a property evaluate(expr) == evaluateWithCache(expr) (for pure expressions) will be checked over a large space of random Nix expressions. We’ll also specifically test tricky cases: scoping differences (e.g. that with a; with b; x hashes differently than with b; with a; x as it should) ￼, tryEval interactions, string contexts, and floating-point edge cases. Performance benchmarks will be conducted to measure the overhead of caching logic. The plan includes extending NIX_SHOW_STATS to report memoization metrics (e.g. time spent in bindVars vs computeHashes, cache hit rates) ￼ ￼ so we can monitor impact in real-world use. The overhead of the AST hashing phase (~5–10% to parse time) and cache lookups is expected to be far outweighed by the savings for repeated computations, but these stats will help validate that assumption in production.
	•	Deployment Strategy: To safely roll out these changes, we will use feature flags and a gradual adoption strategy ￼. The new evaluator memoization can be introduced behind an experimental feature flag (e.g. experimental-features = nix(evalCache)), allowing users to opt-in. We also propose a “shadow mode” during testing: the evaluator would perform all the usual computations and the memoized caching in parallel (or in logging mode), but only use the traditional result, merely comparing and reporting any discrepancies. This allows us to battle-test the cache in real usage (including in NixOS tests or Hydra builds) without risk. We will also implement cache warming heuristics – e.g. if a flake’s lock file or Nix files haven’t changed, we can pre-populate the cache or trust the persisted cache to speed up command startup. Finally, to coordinate cache persistence in multi-user or multi-process scenarios, writes to the persistent cache will go through the Nix daemon (single writer), similar to how the Nix store is managed ￼. This prevents race conditions or corruption when multiple evaluations attempt to save results concurrently.

The following roadmap outlines the phased implementation plan, incorporating the above decisions. Each phase focuses on a set of improvements, building on previous ones. (The numbering continues from prior planning rounds, hence starting at Phase 0.)

Implementation Roadmap

Phase 0: AST Normalization & Hashing (Parse-Time Preparations)

Objective: Establish a robust foundation for hashing and caching by enhancing the AST and parse/evaluation pipeline.
	•	Implement Three-Phase AST Processing: Introduce a new Expr::computeHashes() pass that runs after variable binding and before interning the AST ￼ ￼. In this pass, each node computes two hashes:
	•	a scope identity hash capturing the structure of its lexical environment (e.g. for a with or let, the chain of scopes) and
	•	an expression structural hash for the subtree.
This requires storing additional fields in AST nodes (for example, a scopeIdentityHash in ExprWith or similar) and possibly refactoring the AST node types to hold hash values. The computeHashes implementation will traverse the AST bottom-up, computing child hashes first ￼. For with expressions, we use the parentWith pointers (which link to the enclosing with) to incorporate the entire chain of with scopes into the hash ￼. This corrects the current behavior where only a numeric depth was considered, missing the actual names and structure, and fixes the with-bound variable hashing bug (a critical issue where with x; body and with y; body were hashed identically if their with depths were equal) ￼. After this phase, every AST node will carry a cached hash of itself and its scope, enabling fast comparisons in the evaluator.
	•	Closed-Term Optimization Flag: During the binding phase (bindVars), mark lambdas or lets that do not close over any outer variables with an isClosed flag ￼. This indicates a closed term (no free variables from outer scopes), which can be memoized or copied without capturing a dynamic environment. In Phase 2 we will use this flag to skip environment hashing or environment allocation in certain cases for speed. It’s a minor AST annotation change: as we walk the AST, propagate whether an expression closes over any outer Env. This pairs well with computeHashes: truly closed sub-expressions can be given a special hash treatment or cached aggressively, knowing they’re pure.
	•	Integrate AST Hash Caching with Symbol Table: Ensure that computing hashes can leverage the existing symbol table for stable symbol identities. We may need to extend the SymbolTable or how symbols (variables) are interned so that the hash of a variable reference depends only on a stable symbol ID and the with chain hash. After Phase 0, the parser + binder + hasher pipeline should deterministically assign the same hash to identical code with identical scope, regardless of memory addresses. This phase effectively normalizes the program representation, making subsequent caching keyed purely on content.

(By the end of Phase 0, Nix will parse and preprocess expressions into a form ready for memoization: each thunk’s AST knows its own content hash and its lexical scope’s hash. The evaluator can then use these as needed for keys.)

Phase 1: Correctness and Core Cache-Key Fixes (Blocking Issues)

Objective: Fix all known correctness issues in the current memoization prototype and incorporate missing key elements so that within-evaluation caching produces exactly the same results as no caching. These are must-haves before enabling the cache.
	•	Include String Context in Hashing: Extend value hashing (value-hash.cc) to incorporate a string’s context. In Nix, strings can carry context (e.g. derivation outputs that influence how the string is used). Two otherwise identical strings with different contexts must hash differently ￼. We will hash the sorted context along with the string contents ￼. This prevents cache collisions where, for instance, "$out" in one derivation vs another would previously be considered the same. (This addresses item 1 in the correctness must-haves ￼.)
	•	Track tryEval Depth in Cache Keys: Modify the evaluator to record the current trylevel (a counter for nested tryEval calls) as part of the cache key ￼. This means a thunk evaluated inside a tryEval (which catches evaluation errors) will not hit a cache entry created outside a tryEval, and vice versa. Without this, expressions that error vs succeed could be memoized incorrectly across a tryEval boundary. We’ll incorporate state.trylevel into the hash/key generation for thunks (perhaps by mixing it into the content hash or storing alongside the structural hash) ￼. (Addresses item 2 in must-haves ￼.)
	•	Canonicalize Floating-Point Values: Implement a canonicalization function for double values used in Nix (nFloat). All NaNs will be treated as a single canonical NaN (e.g. using the IEEE 754 default quiet NaN bit pattern), and negative zero will be converted to positive zero ￼ ￼. This function will be applied before hashing floats in value-hash.cc. By doing so, any platform-specific idiosyncrasies (like x86 vs ARM NaN payloads) or trivial differences (sign of zero) are eliminated, ensuring the same numerical value hashes identically everywhere ￼. If cross-platform floating-point inconsistencies remain (e.g. if different architectures produce slightly different results in computations), we might include the system (platform identifier) in cache keys for any thunk that actually contains float operations ￼. The combination of canonical bits and selective platform tagging guarantees float-heavy evaluations are cacheable across machines. (Addresses item 3 in must-haves ￼.)
	•	Fix “with” Scope Hashing Bug: As discovered, the current implementation fails to include the actual variable name introduced by a with statement in the hash, only a relative depth ￼. We will fix this by feeding the Symbol (variable name) from ExprVar into the hash sink for with-bound variables ￼. Additionally, we ensure that the entire chain of nested with-scopes influences the hash (by using the parent scope hash computed in Phase 0). Concretely, in expr-hash.cc at the point where an ExprVar bound by a with is hashed, we will incorporate expr->name (the symbol) and the precomputed hash of the enclosing ExprWith chain ￼. New unit tests will be added to verify that swapping two different with scopes changes the hash and that two distinct names at the same with depth are distinguished ￼.
	•	Cycle Detection in Hashing: Introduce robust cycle detection in the hashing routines for values and environments. Currently, a uniform CYCLE_MARKER approach is used or planned, but a more structured solution is needed ￼. Using De Bruijn indices or back-references, we can detect cycles in recursive values (like a value that indirectly contains itself) and break the hashing recursion safely ￼. For example, maintain an ancestry stack of visited nodes during hashing; if we’re about to hash a value that’s already on the stack, instead of recursing infinitely, emit a special marker (or incorporate the position in the stack) into the hash. This ensures that cyclical data structures (which are possible in Nix values via lazy references) don’t hang or produce inconsistent hashes. We’ll implement this in env-hash.cc and value-hash.cc and also add cycle detection to serialization (throw an error or omit cycles when trying to persist a value graph) ￼. This is critical to avoid infinite loops in the cache and will resolve any potential for non-termination when hashing pathological values.
	•	Epoch-Based Composite Cache Keys: Implement the generation counter (epoch) scheme for cache invalidation on GC, as described earlier. We will add a global atomic EvalState::currentEpoch (or use the existing GC start callback) that increments every time a garbage collection happens ￼. All cache lookups and insertions will use keys augmented with the current epoch. This means defining new key structures: e.g. struct MemoKey { StructuralHash hash; uint64_t epoch; } for the content cache, and similarly an IdentityKey { Expr* expr; Env* env; uint64_t epoch; } for the identity cache ￼ ￼. We will adjust the hash table usage accordingly (providing custom hash and equality for these structs) ￼. When the GC callback fires, incrementing the epoch invalidates all existing entries – but importantly, those entries will likely be removed lazily when noticed or proactively via the disappearing links mechanism. This change closes a correctness hole where a pointer could be recycled by GC and erroneously hit an old cache entry. It aligns with the “composite key” recommendation from earlier reviews ￼.
	•	Garbage Collector Integration for Cache Entries: Use Boehm GC disappearing links for cache entries. For each new entry in the L1 cache, register the pointer to its value as a disappearing link tied to that Value object ￼. Boehm will clear that pointer when the object is collected. We’ll periodically sweep the cache to remove entries with null values. This requires storing entries in a structure we can iterate (e.g. a linked list or iterating the hash map). We will avoid Boehm finalizers (due to their cycle issues ￼) and instead rely on this approach. Combined with the epoch key and GC epoch increments, the cache will gracefully discard dead entries without impacting correctness. Implementing a periodic cache sweep (e.g. after each evaluation or when cache size grows) will ensure we prune out any null entries to free memory ￼.
	•	Impurity Marking Adjustments: Mark certain builtins as impure to prevent caching when they’re used. For example, unsafeGetAttrPos and the special __curPos (current file position) should be considered impure operations that depend on evaluation context ￼ ￼. We will tag these by calling state.markImpure(ImpureReason::PositionDependent) whenever they are invoked. This ensures that any thunk using these will carry an impurity token in its hash key, effectively making such evaluations unique (or of LOW durability) and not cacheable across different positions or runs. This is a minor but important tweak to avoid incorrect reuse of values that encode positional metadata.
	•	Basic Testing for Phase 1 Changes: Add targeted regression tests to confirm the above fixes. For instance, tests that two strings with different contexts do not collide in the cache; a test that uses tryEval twice on a failing expression and a succeeding one and ensures no incorrect cache hit; tests that floats of different sign bits don’t collide, etc. Also, enable NIX_SHOW_STATS and verify that it now reports timing for the new computeHashes phase (after implementing it in Phase 0) so we can observe parse vs hash time in practice ￼. Only once all Phase 1 tasks are done and tests are passing would we consider the within-evaluation (L1) cache safe to use.

(By the end of Phase 1, the evaluator’s memoization for a single run should be correct and deterministic. All known bugs are fixed, and the cache keys include every context needed to avoid false hits. Within one evaluation, forcing the same thunk twice will return a cached result with confidence in its correctness.)

Phase 2: Performance Improvements & Intra-Eval Caching Scalability

Objective: Implement the full caching mechanism for within-evaluation use (L1) and lay the groundwork for persistent caching (L2), focusing on performance and efficiency. This phase turns on the memoization for real and ensures it accelerates evaluations without undue overhead.
	•	Enable Two-Level Caching Infrastructure: Activate the identity cache (L1) and content cache (L2) in the code. In EvalState, we already have structures for thunkMemoCache (which will act as L1) and placeholders for content hashing caches ￼. We will formalize these into two cache objects or tables. L1 will map (Expr*, Env*, epoch) to a Value* result. L2 will map a content-hash (structural hash + context) to a serialized result or a Value* that can be reused across evals. Initially, L2 can be in-memory (to focus on the mechanism), but structured in a way that it can be persisted (Phase 4 will handle actual persistence). This likely means designing a ContentCacheKey struct that includes the structural hash and also the evaluation inputs version (e.g. an identifier for the flake or Nixpkgs revision, to avoid collisions across different top-level inputs – more on that in Phase 4). For now, we ensure that whenever a thunk is evaluated, we populate both caches: first check L1 (fast path), if miss then compute, then before returning result, optionally store in L2 (if the value is deemed cacheable and pure enough). Conversely, on forcing a thunk, we should check L2 as well in case the result was computed in a previous run (if we decide to allow cross-run reuse immediately). However, enabling cross-run reuse now may complicate things; we might defer actually querying L2 until Phase 4. In any case, the code paths and data structures for both caches will be in place after this task ￼.
	•	Use a Faster Hash for Pointer-Based Keys: Switch the hash function for L1 keys to a fast, non-cryptographic hash (xxHash3). Since L1 is not intended to be portable or secure – it’s within one process – we can use a high-speed hash for combining pointer values and small integers. We will include the xxhash3 library (as a dependency via Meson) ￼ and use it to hash the (exprPtr, envPtr, epoch) tuple to index the identity cache. This will reduce overhead compared to using SHA-256 or similar. (This implements Decision 26 from prior discussions.) For L2 content keys, we will continue to use a cryptographic hash (likely SHA-256) because collisions there would be persistent and cross-machine, so we need strong guarantees. The combination of xxHash3 for L1 and SHA-256 for L2 strikes a balance between speed and robustness ￼.
	•	Adaptive Cache Eviction (ARC): Introduce a replacement policy better than simple LRU for the in-memory caches. Real-world evaluations may alternate between phases of heavy churn (unique computations) and phases of reusing earlier computations, where a naive LRU might evict useful entries. We plan to implement an Adaptive Replacement Cache (ARC) algorithm for managing the cache size ￼ ￼. ARC maintains two LRU lists (one for recent entries, one for frequently used entries) and two “ghost” lists for tracking evictions, dynamically balancing between recency and frequency ￼ ￼. By incorporating ARC, the memo cache can self-tune to different workload patterns and avoid pathological thrashing. The implementation will involve tracking cache entries in these lists and adjusting a pivot parameter p as described in the literature ￼. If this proves too complex to implement immediately, a simpler stop-gap is to use a two-tier LRU: e.g. entries with High durability (pure, cross-run) can be kept until memory pressure (no expiry), whereas entries with Medium durability expire after a fixed time (like 24h), and Low durability entries are freed at end of evaluation ￼. However, since Phase 2 focuses on intra-eval caching, we primarily need to ensure the cache doesn’t grow unbounded in long-running evaluations. We will likely cap the L1 cache size (e.g. by number of entries or memory usage) and evict using ARC to keep frequently reused thunks. Coupling this with Boehm GC’s guidance (the disappearing links mean truly unreachable entries will be auto-removed) should keep memory overhead low. We will measure hit rates and adjustments as part of performance testing.
	•	Lazy vs Eager Hash Computation: Currently, our design does an eager computeHashes() on the entire AST (Phase 0). One might worry that we are hashing parts of the AST that might never be forced at runtime (dead code paths). However, we determined that computing the scope hashes eagerly is beneficial for correctness and those are relatively cheap (just structural info) ￼. Actual value hashing (for content keys) is done lazily on demand, not for every thunk upfront. We will confirm that thunk hashing is only done when needed (on cache lookup or insertion), never forcing thunks just to hash them ￼. The code will treat an unforced thunk’s hash as the pair of its expr pointer and env pointer (already captured in L1 key), and will only compute a full content hash of a thunk’s value if it’s forced to a normal value. This way, we avoid any performance hit of pre-hashing thunk results that might not be needed. We explicitly ensure that our hashing functions for a Value check for thunks and do not recursively force them – instead, they incorporate the thunk’s identity (expr/env) into the hash ￼. This is important to preserve laziness and not introduce unintended evaluation by the act of hashing.
	•	Concurrency and Black-Holing Implementation: Finalize the black-holing mechanism in forceValue. We’ll use an atomic compare-and-swap on a field in the Value (or an entry in the memo table) to mark a thunk as “being evaluated” ￼. The first thread that succeeds in this mark will evaluate the thunk’s expression; others will either block or skip using the cache. Once the evaluation finishes, the result is stored and the state updated so other threads can proceed to use it. If a thunk evaluation throws an exception, we need to un-mark it (so another attempt can be made, or so it doesn’t remain in a permanently evaluating state). We will carefully handle this in the exception path. Nix already has some concurrency support in place; we will leverage any existing std::mutex or atomic flag around forcing (the codebase hints at something like this in eval-inline.hh). After this, the evaluator should be thread-safe in terms of memoization. We will test this by forcing the same thunk from multiple threads in a controlled way (if the Nix evaluator can be run in parallel threads – typically Nix eval is mostly single-threaded except for IFDs, but we ensure we’re future-proof).
	•	Optimize Memory Usage of Interning: As an optimization (from Appel’s 1993 paper on hash-consing), we will defer interning of values to a later stage or only intern long-lived values ￼ ￼. Currently, Nix interns (hash-conses) certain values to save memory and identify duplicates. However, doing this eagerly for short-lived values (like intermediate list or attrset values that don’t escape a single eval) can actually cost time and space. We plan to apply a tenuring approach: only intern values that survive multiple evaluations or are of high durability. For example, content cache entries that are written to disk could be interned (to avoid duplicating them when loaded), but identity cache values within one run need not all be interned globally – they can be simply tracked by the memo cache. This reduces contention on any global structures and avoids building huge hash-cons pools for ephemeral data. We’ll document this as accepting some monotonic allocator waste – i.e. a value might be duplicated in memory a few times across separate evals, but boundedly so (estimated overhead is a few MB for a large Nixpkgs eval, which is acceptable) ￼. Essentially, we trade a bit of memory for simpler and faster caching logic.
	•	Distinguish Cacheable vs Non-cacheable Values: Implement helper predicates like isIdentityCacheable(Value*) and isContentCacheable(Value*) ￼. Certain values should bypass caching: e.g. values capturing external state or large data that is cheaper to recompute than store. We likely consider all pure values cacheable, but things like builtins with side effects or unstable results should not be cached. We already gate caching on impurity (if state.impureTokenCounter_ changed during evaluation of a thunk, we do not store it). Additionally, with Nix’s lazyTrees experimental feature (which introduces a different evaluation strategy), caching might interfere. We will disable L2 content caching when experimental-features.lazyTrees is active ￼, as that mode implies a non-standard evaluation order. The isContentCacheable() check will incorporate such global flags or perhaps check that state.lazyTree is false. This provides an easy off-switch for caching in scenarios known to conflict.
	•	Performance Testing: With caching fully enabled, measure performance on sample workloads. We will use scenarios like evaluating a large attribute set with repeated computations to see L1 hits, and doing repeated evaluations of an expression (without Nix process restart, if possible) to see L2 hits. Expectation: single-run evaluations of something like Nixpkgs should see significant speedups (especially if identical sub-expressions appear many times). We will tweak cache sizes if needed to maximize hits without blowing up memory. The NIX_SHOW_STATS outputs will be checked to ensure that, for example, cache hit rates and time spent in hashing are reasonable. We also ensure that enabling the cache doesn’t regress performance on trivial evaluations (the overhead should be minimal when there are no repeated thunks).

(By the end of Phase 2, the within-evaluation memoization (L1) is fully implemented and optimized. Nix can run with caching turned on by default in a safe way. Basic content hashing is also in place, though not yet persistent across runs. The evaluator is faster for large pure expressions, and groundwork is laid for cross-eval caching.)

Phase 3: Architectural Refinements & Durability Features

Objective: Improve the architecture for long-term maintainability and prepare the system for cross-evaluation caching. This includes refining how environments are handled, how durability is tracked, and adding instrumentation and opt-in features.
	•	Environment Hash Stability and Order-Dependence Fix: Revisit the environment (Env) hashing logic in env-hash.cc. The environment is essentially a list of variable bindings (thunks or values) in a frame plus a pointer to an up (parent env) ￼. The current hashing may be order-dependent or sensitive to evaluation order for thunks. For example, if an environment contains several thunks, and some have been forced and some not, hashing their values vs thunk identities might yield different results depending on evaluation history. We plan to define a consistent rule: hash thunks by their identity (Env, Expr pair) if they have not been forced**, and hash fully evaluated values by their content ￼. This way, hashing an environment yields the same result regardless of whether some of its components have been evaluated yet (assuming purity). We will implement this by modifying Env::hash() to iterate through the values array: if a slot is a thunk that is not yet evaluated, we incorporate its (exprPtr, envPtr) (maybe via pointer address or a precomputed AST hash) into the hash; if it’s a concrete value, we incorporate its content hash. We also include the up environment’s hash (already computed in that env’s struct during computeHashes phase for static env structure). This ensures referential transparency of environment hashing. After this, the portability classification of closures can be improved: for a function value (lambda), we can recursively check the environment it closes over to decide if it’s portable (all store paths content-addressed, etc.) ￼. We fix any stale comments in code that suggested a pointer-based fallback for env hashing – it is now fully content-based except for un-forced thunks which are identified by stable identities ￼.
	•	Early Cutoff Optimization: Introduce an early cutoff mechanism inspired by build systems ￼. In an incremental setting, if a thunk’s dependencies (as captured by its EvalTrace or input hashes) have not changed, we should be able to short-circuit re-evaluating that thunk. While our content-addressed cache largely provides this (we won’t re-evaluate if the content hash is the same and present), early cutoff can also refer to aborting an ongoing evaluation when a dependency’s hash matches a previous one. This is more relevant in an interactive setting (like Nix REPL or future incremental builds): if we start evaluating a function and notice an input hasn’t changed, we reuse the old result immediately. Implementing this fully may be complex, but we can prepare by storing not just values in the content cache, but also their hashes of dependencies. Possibly maintain a map from each thunk to the set of already forced sub-thunks and their hashes; if all match a prior run’s record, skip re-computation. This ties into the durability tracking – effectively, if none of the inputs’ version vectors changed, we can reuse. We mark this as a medium-priority architectural feature ￼ that can be added once basic caching is in place.
	•	Cache Statistics and Profiling: Add instrumentation to gather cache performance stats at runtime ￼. This includes counters for L1 and L2 cache hits, misses, evictions, and perhaps distribution of thunk sizes or hash computation times. We will extend NIX_SHOW_STATS and possibly introduce a new --debug option to dump cache profiling info after an eval. This will help users and developers understand when caching is effective and identify pathological cases (e.g., a certain thunk cache miss rate). We might integrate with an external profiler or simply log to stderr. This task ensures observability of the caching subsystem in production.
	•	Two-Tier Weak/Strong Cache (GC integration): Refine the interaction between caching and garbage collection by implementing a two-tier cache structure ￼. Idea: divide cached entries into strongly referenced and weakly referenced. The L1 identity cache might keep strong references to recent results to ensure they stay alive if still in use, whereas older entries could be moved to a weak table that allows GC to reclaim them if memory is needed. This is somewhat handled by our disappearing links mechanism, but we can make it more explicit: e.g., maintain a small L1 (strong) and a secondary structure (L1-ghost or L2-weak) that holds additional entries that can vanish. This is analogous to generational caching where recently used entries stay longer. We will consider using C++ weak pointers or Boehm’s APIs. This task is a “should-do” for memory efficiency ￼.
	•	Pure Evaluation Fast-Path: Introduce an opt-in mode or internal fast path for pure evaluations. If the user or Nix knows that an evaluation has no side effects (no impure actions like file reads or environment queries), we can bypass certain overhead. For instance, we could disable impureToken checks and related branches, and avoid capturing EvalState changes that are irrelevant. A possible implementation is a template specialization or flag in EvalState that, when set, assumes no impurity and omits those code paths (for example, not calling markImpure on file access – which we wouldn’t do because file access wouldn’t be allowed in pure mode) ￼. This is similar to a “sandboxed eval” mode and ties in with Nix’s pure-eval RFC ￼. For our caching, pure mode means everything is HIGH durability; we might be able to simplify logic or achieve better performance. We’ll prototype this as an internal feature (state.pureEval = true) and use it in tests (it could catch any code that tries an impure action by triggering an assertion).
	•	Documentation and Developer Notes: By this phase, update the Nix manual/developer docs to describe the memoization feature and its caveats ￼. We need to clearly document that while caching greatly speeds up evaluation, there are still order-dependent scenarios (like if a value’s computation depends on evaluation order of independent thunks, results might not always be reused if orders differ – though logically they shouldn’t differ for pure code). We also document the memory trade-offs (e.g., that some duplicate computations might still occur in certain edge cases, or that memory usage might increase up to X% with caching enabled). Setting expectations will help users trust but verify the feature.
	•	Durability Levels and Version Vector: Implement the Salsa-style durability tracking fully in the codebase ￼. We introduced the concept earlier; now we add a structure in EvalState to maintain a mapping from input type to a version. For instance, maintain counters or hashes for:
	•	Nix store paths seen (HIGH durability): since these are content-addressed, perhaps a global hash of their names could serve as part of a version vector (though content-addressed means they never change – so maybe the set of store paths doesn’t need a version as long as content hash included).
	•	File system paths (MEDIUM): maybe track an mtime or a hash of each file read.
	•	Environment variables/NIX_PATH (MEDIUM or LOW): incorporate a hash of relevant env vars.
	•	Network access or current time (LOW): if any network or clock was accessed, the evaluation gets a LOW durability token.
The evaluator will record these during execution (some hooks already exist: e.g. state.recordInput() as seen in primops.cc for readFile ￼). We will extend that to cover more builtins. At the end of an eval, we can store a concise version vector of all inputs. In caching, we can then decide that an L2 entry is valid only if the current eval’s inputs vector matches that stored with the entry. For example, if the cached result was computed with Nixpkgs commit X and we are now on commit Y, the version vector difference would prevent a reuse (or we treat it as a cache miss leading to recomputation). This approach complements content-hash keys: content hashing covers the expression and its immediate pure dependencies, while durability versioning covers external context changes (like new NIX_PATH). We’ll implement this such that each persistent cache entry is tagged with a small identifier representing the state of external inputs (for flakes, it could even be the flake’s own hash or lockfile hash). This is critical to avoid stale cross-run cache hits and achieves incremental invalidation robustly ￼.
	•	Position-Independent Evaluation (Opt-In): Implement a mode to ignore source locations in the AST for caching purposes ￼. Normally, Nix Values carry position info (for error messages). Two identical computations at different source locations would have different AST hashes due to differing Pos indices. This prevents caching across, say, the same expression written in two files or two locations. We introduce a flag (e.g. --position-independent-eval) that, when enabled, causes the evaluator to treat all positions as equal (or strip them out in hashing) ￼. Concretely, in nixexpr.cc or the hashing functions, if the flag is on, we do not feed pos into any hash. This mode is useful for scenarios like IDE support or if we want maximal cache hits at the cost of potentially confusing error messages. The default will remain position-sensitive (to avoid wrong results if code genuinely differs only by position-dependent behavior). However, this option shows our commitment to portability: in principle, one could copy a Nix expression to a new file and if all else is the same, caches could be reused. It’s also a stepping stone to a future where evaluation is more content-addressed at a semantic level (as seen in rust-analyzer’s approach to decouple parsing from analysis) ￼.

(By the end of Phase 3, the caching system is robust, well-instrumented, and equipped with mechanisms to handle changes in input and environment. Many of these changes are about future-proofing and ensuring that when we enable cross-run caching, it behaves correctly. At this point, within one machine, we have the capability to persist and reuse results across evaluations in principle, gated by durability checks.)

Phase 4: Cross-Evaluation Persistence & Portability

Objective: Implement persistent storage for the content cache and enable reuse of cached results across separate Nix invocations and across different machines. This phase makes the L2 cache truly functional and shareable.
	•	Persistent Cache Storage (LMDB Integration): Choose a high-performance storage for caching evaluation results on disk. We plan to use LMDB (Lightning Memory-Mapped DB) as a backing store for the content cache ￼. LMDB offers single-writer, multi-reader concurrency, which fits Nix’s pattern (nix-daemon can be the writer, eval processes are readers) ￼. It also allows zero-copy reads via memory mapping. We will design the cache so that when a thunk is fully evaluated and deemed cacheable (pure and above a certain size threshold perhaps), we serialize its resulting Value (and any needed closure data) into a CBOR byte string and store it in the LMDB under the key = content hash ￼. On a cache lookup miss in memory, we then check LMDB: if an entry exists for that hash and is still valid according to current durability vector, we can deserialize it to a Value and avoid recomputation. We will integrate this by expanding the L2 cache interface: first check in-memory (maybe recently used cross-run entries kept in memory), then fall back to LMDB lookup. The LMDB environment will be initialized by nix-daemon or on first use, likely stored in Nix’s state directory (with a size limit configurable). We also consider using SQLite for metadata (to track stats, or to store an index of what keys exist and maybe last-used times for eviction) ￼, but that may be overengineering – LMDB can store metadata as well. A simple approach: LMDB database with two tables: one for values (key → serialized value, possibly split if large), one for metadata (key → last accessed timestamp, durability vector, etc.). We mark this task as medium priority since it’s an engineering choice; the key point is persistent storage is needed for cross-eval caching.
	•	Deterministic Serialization (CBOR): Implement serialization of Nix values in a deterministic manner ￼. We will use the CBOR format (RFC 8949 §4.2) which supports a canonical encoding for data structures. This ensures that the bytes we store for a given value are consistent across platforms and independent of memory layout. We may need to introduce custom CBOR tags for Nix-specific types (like derivations or thunks) but only for completeness – ideally we don’t serialize thunks at all (only fully evaluated values). Critical details: when serializing sets or lists, ensure a deterministic order (attribute sets should maybe sort by key, though Nix attrs are unordered semantically; we might store them sorted by key name for reproducibility ￼). Also, be careful to not include memory addresses or non-deterministic data in the output. This task will produce a value-serialize.cc with a function to turn a Value into a CBOR byte array and a corresponding parse function. By using a standardized encoding, we make it possible for a machine of a different endianness or word size to still read the cached value (assuming float canonicalization took care of special cases). This is a cornerstone for cross-machine caching – combined with content-addressed keys, it means two machines given the same Nix expression and inputs can share the cache artifact.
	•	Content-Addressable Imports and IFD: Extend caching to Nix’s import mechanism and import-from-derivation (IFD). When Nix evaluates import <path>, currently it will read and evaluate that file anew each time (unless flakes or other higher-level caches circumvent it). We will make ExprParseFile hashable by file content: use the file’s fingerprint (content hash from the SourceAccessor) as part of the expression’s hash ￼. Then, the act of importing a file becomes cacheable: the content hash of the file combined with the fact of doing an import can serve as a key in L2. Concretely, after evaluating an import once, we store its result in the content cache keyed by the file’s hash. On the next eval, if the file’s content is unchanged, we can directly reuse the prior result (possibly after a quick check that none of its transitive imports changed, but that would be covered if those were cached too). This essentially extends Nix’s current “flake” caching (which operates on top-level outputs) down into the evaluator. For IFD (where a derivation is built during eval and then imported), we can similarly cache based on the derivation’s hash and build outputs. Specifically, when encountering an import (builtins.fetchTarball "...") or a derivation built and then read, we use the derivation’s hash and the output path’s content hash as a cache key for the resulting value. This might integrate with Nix’s existing cached import feature (the Tweag RFC) ￼. We’ll add logic so that if an import or IFD is encountered and the output’s hash was seen before, we skip re-building or re-importing and fetch the cached Value instead ￼. This is a medium priority because it can drastically speed up evals that use IFD or import large files repeatedly.
	•	Cache Key: Evaluation Inputs Fingerprint: Incorporate an EvalInputs fingerprint into cross-eval cache keys ￼. Nix’s EvalState often knows about high-level inputs like the NIX_PATH, system type, current git revision for flakes, etc. We will produce a single hash (e.g. SHA256) that aggregates the identity of all these global inputs. This could be as simple as concatenating: Nixpkgs revision, Nix version, system architecture, and perhaps a list of enabled experimental features. This fingerprint will be included in the content cache key (or stored alongside durability info) so that cache entries are segregated by incompatible inputs. For example, if you switch your Nixpkgs version, the fingerprint changes, and you won’t accidentally reuse cache entries from the old revision (unless those expressions truly didn’t depend on Nixpkgs, in which case it wouldn’t matter). This is a defensive measure to avoid subtle issues when the “universe” changes under the evaluator. We mark this as low priority to implement (since version vectors already cover a lot), but it’s straightforward and adds an extra safety net ￼.
	•	Semi-Semantic Hashing for Keys: Implement Dhall-style semi-semantic hashing for cache keys ￼. The idea, borrowed from the Dhall configuration language, is to incorporate the hashes of imported content (which represent semantic meaning) into the overall hash of an expression, rather than raw syntax alone ￼. In practice, this means when we compute the structural hash of an AST, if that AST has an import node, we combine the content-hash of the imported file into the AST’s hash. Similarly, for a derivation, we might incorporate the content (hash) of its inputs. This way, the hash of a thunk truly reflects its “semantic closure.” Dhall uses this to avoid invalidations – since the hash changes only if imported content changes, you never need an external invalidation step. We will apply this concept to Nix by ensuring our computeHashes phase (Phase 0) or later augmentation does this: e.g., after parsing, once we know the fingerprint of an import, we can store a combined hash in the AST node representing that import. This eliminates an entire class of cache invalidation logic because the key self-invalidates when dependencies update ￼. It’s a high-priority design improvement for cross-eval caching. We have to be careful to handle cycles (import cycles) and huge dependency graphs, but since we already avoid forcing thunks for hashing, it should be fine – we’ll treat imports similarly to thunks in hashing by not evaluating them during hash, instead use their known fingerprint.
	•	Network Distribution of Cache (Planning/Future): While not implemented in this phase, it’s worth noting that once content-addressed evaluation results exist, they could be shared similar to build artifacts. We anticipate integrating with Nix’s binary cache infrastructure to upload/download eval results by their hashes. For now, we ensure that our design keeps this possibility open: e.g., the content hash for caching could be based on a known cryptographic hash that can be used as a key in a binary cache (like a store path hash or something analogous). In Phase 4 we might not fully implement the networking, but we design the system such that an external tool or future enhancement can sync the LMDB or cache directory between machines, and thanks to deterministic hashes and serialization, those results would be recognized. This is mentioned because cross-machine caching is a goal, and after this phase the only missing piece to achieve it would be a transport mechanism (which might piggyback on existing Nix cache servers, or a simple rsync of the cache DB).

(By the end of Phase 4, Nix can cache evaluation results across runs. Running nix eval on the same inputs multiple times will reuse results from disk, substantially speeding up repeated evaluations (e.g. evaluating big flakes). The cache is content-addressed and machine-independent – if two machines have the same inputs, they can share cache files. This achieves the core requirement of portability. Cross-machine sharing is now feasible by copying or syncing the cache data. The evaluation caching is now analogous to how build outputs are cached, but at the level of the evaluator’s intermediate values.)

Phase 5: Advanced Optimizations and Future Ideas

Objective: Explore further optimizations and architectural shifts that could improve performance or maintainability in the long run. These are forward-looking and may or may not be implemented, but guide future development.
	•	Position-Independent Semantic Layer: Continue the work started with opt-in position independence to potentially create a fully position-agnostic evaluation model ￼. In a future Nix evaluator, we might separate parsing and evaluation such that the evaluator deals with anonymous ASTs where source locations are irrelevant. This could allow caching across codebases – e.g., if two different files contain identical functions, a single cached result could serve both, even if defined in different places. Achieving this requires careful handling of error messages and debugging (since positions are primarily for those), possibly by storing positions separately from the cache key. This idea connects to research in incremental IDE architectures (like rust-analyzer) where the semantic analysis is cached independent of file locations ￼.
	•	Fine-Grained Impurity/Coeffects: Implement more granular tracking of effects, inspired by coeffect types ￼. Instead of a single impure token or broad durability classes, we could tag evaluations with the exact effects they performed (e.g., “read file /etc/xyz”, “access network”, “used current time”). This would allow even more selective invalidation – for instance, if a thunk only read a specific file, and that file hasn’t changed, we could still reuse the thunk’s result even if other parts of the eval changed different files. Doing this in Nix might require a type-system extension or at least conventions around builtins. While we don’t plan to implement a full effect type system, we keep the concept in mind. As a small step, we already track store vs non-store file accesses (to mark HIGH vs MEDIUM) ￼ ￼. A future enhancement could introduce multiple impure tokens (one for network, one for env vars, etc.) or even extend the recordInput to note what kind of input. This way, changes in one type of input don’t invalidate cache entries that didn’t depend on that type. This is complex and marked low priority, but it’s a possible direction ￼.
	•	Query-Level / Structural Caching: Consider introducing caching at larger granularity than individual thunks. For example, caching the result of a whole function application or fixed-point computation. Nix evaluations often have natural boundaries (like the top-level of each package’s evaluation). We could allow caching subtrees of the eval graph by content hash, not just individual thunks. This resembles Bazel’s Skyframe or other build systems where each “node” (function, import) can be cached as a unit ￼. With our current approach, we’re partway there (imports are a node, derivations could be considered a node). A concrete future step: when evaluating an attrset of many packages, cache each package’s evaluation result by its name or by a hash of the expression for that package. Another idea is memoizing function calls: Nix functions are pure, so calling a function with the same argument should yield the same result – we could cache function application by hashing the function closure and argument value. This goes beyond our current thunk caching (which is mostly at the point of defining the thunk, not calls). It’s a larger undertaking and might require changes in how the evaluator represents function application. We list this as a future optimization (perhaps leveraging the “query” concept from Build Systems à la Carte) ￼.
	•	Alternate Memory Management Strategies: Evaluate the possibility of using a moving generational GC or reference counting for the evaluator, to reduce fragmentation and overhead. Boehm GC has served Nix well, but with caching we might allocate many long-lived structures (especially if a daemon process holds the cache). A moving GC could compact memory and improve cache locality, at the cost of needing to update pointers (which Boehm avoids). Alternatively, since cache entries are mostly immutable once created, a simple reference counting scheme could work (with a cycle collector for the rare cycles). Another approach is to allocate cache values in a separate arena that can be memory-mapped and perhaps shared between processes (this is speculative). These ideas are beyond the current scope, but we note them as potential improvements if memory usage becomes a bottleneck. In the near term, we focus on working with Boehm: tuning its parameters (maybe using GC_set_flow_alloc_fn or other APIs to better handle our patterns) and ensuring we don’t trigger GC too often unnecessarily (perhaps by adjusting GC_free_space_divisor or explicitly hinting when large caches can be freed). We’ll also monitor the memory overhead introduced by caching and adjust accordingly.
	•	Revisiting Tagged Pointers: Earlier, we considered using tagged pointers (taking advantage of unused low bits on 64-bit systems for identity tagging) to speed up certain operations. This was deemed not viable due to portability and complexity ￼. However, as hardware evolves (e.g. widespread pointer tagging support in CPUs), we might re-evaluate this. For instance, ARM’s Top-Byte-Ignore could allow storing a small cache ID directly in pointer addresses ￼. This could potentially make identity comparisons O(1) with certain guarantees. This is low priority and likely unnecessary given our other solutions, but it remains a curious avenue for performance hacks in the future.
	•	Comprehensive Benchmarking and Tuning: Finally, after all planned features, the evaluator should be rigorously benchmarked against real workloads (evaluating Nixpkgs, large flakes, etc.) and tuned. We expect dramatic improvements in repeated evaluations of nearly-unchanged inputs, and moderate improvements even in single-run evaluations of big expressions due to L1 hits. If any performance issues remain (for example, hashing overhead in pathological cases), we will explore solutions – such as selective hashing (avoid hashing parts of AST that are huge lists of constants, since they rarely repeat), or multi-threading the computeHashes phase for large ASTs, etc. Since this is an ongoing effort, this phase is about ensuring the memoization infrastructure lives up to its promise and adjusting any parameters or algorithms (like ARC’s balancing or GC frequency) to optimal values.

(Phase 5 and beyond involve exploratory and low-priority tasks that could yield further gains. They underscore our willingness to re-architect core pieces of Nix (evaluator, memory, dependency model) if it materially improves caching and performance. Many of these ideas draw on the latest research and could be incorporated as Nix continues to evolve.)

Will These Changes Meet the Goals?

By implementing the above phases, we expect to meet the memoization goals as follows:
	•	Within-Evaluation Caching: After Phase 1 (correctness fixes) and Phase 2 (performance), Nix will reliably avoid duplicate work within a single evaluation. The critical bug fixes (e.g. with hashing) and inclusion of context in keys ensure no incorrect reuse, while the L1 cache architecture with black-holing ensures thread-safe and efficient reuse. This goal is essentially achieved as soon as the blocking issues are resolved and caching is turned on ￼.
	•	Cross-Evaluation Caching: By Phase 4, the foundation for persistent caching is implemented (content-addressed keys, durable storage, import/IFD caching). At that point, separate Nix invocations can share results – e.g. evaluating the same flake twice will be significantly faster the second time, as most computations will be loaded from the cache. Some work in Phase 3 (durability tracking, position independence) is needed to make this reliable and safe. After Phase 4, cross-eval caching should be functional, with any remaining limitations documented (for example, if certain impure workloads don’t benefit) ￼.
	•	Cross-Machine Caching: The plan treats cross-machine reuse as a first-class concern. By using content hashes, canonicalization, and deterministic serialization throughout, we ensure that a cache entry computed on one machine is meaningful on another. After Phase 4, the only missing piece is distributing those entries. While this plan doesn’t implement a network cache server, it sets up everything needed for one. We note that truly seamless cross-machine caching may require integrating with Nix’s binary cache infrastructure or an out-of-band sync of the LMDB. Also, absolute paths and store paths are handled via fingerprints, so differences in store path prefixes won’t break hashing. Thus, once content-addressed paths and deterministic serialization are in place, the evaluation cache is essentially portable ￼. We rate cross-machine caching as achievable but not automatically enabled until a sharing mechanism is in place; still, power users could manually transfer cache data between machines to great effect.

In summary, the phased plan addresses immediate correctness and performance needs (Phases 0–2), introduces a robust architecture for sustainability (Phase 3), and then builds out the cross-process and cross-machine capabilities (Phase 4), with ideas for future enhancements (Phase 5). By following this roadmap, Nix’s evaluator will become much faster for repetitive tasks, leveraging memoization while preserving the purely functional behavior that is the cornerstone of Nix. All these changes will be rolled out carefully, guarded by testing and gradual adoption strategies ￼ to maintain user trust and system reliability.

Sources
	•	Dolstra, E. Maximal Laziness in Nix (LDTA 2008) – discusses lazy evaluation optimizations and closed-world assumptions in Nix ￼.
	•	Mokhov et al. Build Systems à la Carte (ICFP 2018) – provided inspiration for explicit dependency graph and early cutoff ideas ￼.
	•	Incremental computation research (Adapton, Nominal Adapton, Salsa) – influenced the durability level design and position-independent caching ￼ ￼.
	•	Nix issue trackers and RFCs – many real-world edge cases (floating-point hashing ￼, lazy tree non-determinism, etc.) informed the fixes (e.g., float canonicalization, impurity handling).
	•	Nix source code (particularly eval.hh, expr-hash.cc, value-hash.cc) – verified assumptions about existing data structures and guided the specific fixes (with-bound variable hashing, EvalState fields) ￼ ￼.
	•	Haskell GHC stable pointers and memoization techniques – influenced the approach to black-holing and potential thunk update-in-place ￼.
	•	Adaptive Replacement Cache (ARC) by Megiddo & Modha (FAST 2003) – chosen for cache eviction policy to improve upon simple LRU ￼.
	•	RapidCheck property-testing framework – to be used for validating the evaluator’s behavior under random inputs, ensuring soundness of memoization ￼.

(All code references (e.g., to expr-hash.cc or eval.cc) and decision numbers above correspond to the state of the Nix codebase as verified in Round 5 of this effort ￼ ￼. They highlight where changes will apply or have been confirmed. The plan is subject to adjustment as we integrate these changes and learn from testing, but it provides a clear path forward to a more efficient and robust Nix evaluator.)
```

## Claude

```markdown
# Implementation readiness for Nix evaluator memoization

Deploying memoization in Nix requires addressing **seven critical operational concerns**: experimental feature gating, cache versioning, corruption handling, performance measurement, security for shared caches, correctness testing, and CI/CD integration. Nix's own history with experimental features—particularly flakes remaining unstable for 4+ years despite 84% adoption—provides crucial lessons for avoiding similar pitfalls. The technical recommendations here draw from production patterns in Bazel, SQLite, LMDB, GHC, and Nix's existing binary cache infrastructure.

## Gate memoization with clear stabilization criteria and timeline

Nix's experimental features system, introduced in Nix 2.4 (November 2021), gates functionality behind explicit opt-in flags configured via `experimental-features = memoization` in nix.conf or `--extra-experimental-features 'memoization'` on the command line. This mechanism has enabled iteration but created problems when flakes was encouraged for production use without stability guarantees—the feature has remained experimental for over four years while becoming the de facto standard.

**Memoization should adopt granular feature flags** rather than bundling multiple capabilities under one gate. If memoization involves distinct components (cross-machine caching, parallel evaluation integration, persistent storage), each should have separate flags to allow independent stabilization. The original flakes RFC closed without resolving fundamental design issues, yet the feature shipped anyway—memoization must have design questions resolved before widespread deployment.

The recommended rollout timeline targets **6-12 months** for experimental status, with quarterly reviews. Define explicit graduation criteria upfront: evidence of real-world use across diverse workloads, confirmed absence of correctness bugs through differential testing, acceptable performance overhead (**<5% slowdown** on cache misses, **>80% speedup** on cache hits), and demonstrated backwards compatibility. Unlike flakes, communicate these criteria publicly and track progress visibly.

**Configuration syntax should follow established patterns**:
```nix
# nix.conf
experimental-features = memoization
eval-cache-path = /var/cache/nix/eval
eval-cache-max-size = 10G
```

## Design cache format with explicit versioning and migration paths

Production systems employ three distinct versioning strategies. **GHC uses strict version checking**: interface files contain a magic number (`0x1face`) and version field, with hard failure on any mismatch—full recompilation is required when GHC versions differ. **npm uses dual-write backwards compatibility**: lockfileVersion 2 writes both new `packages` and legacy `dependencies` sections, allowing npm v6 to read v7-generated files. **Bazel uses implicit versioning** through directory path hashing—different Bazel installations naturally isolate their caches by placing them in directories named by the installation manifest hash.

For Nix memoization, **combine header-based versioning with directory separation for major versions**:

```
~/.cache/nix-memoize/
├── v1/
│   └── <cache entries with v1 format>
└── v2/
    └── <cache entries with v2 format>
```

Each cache entry file should begin with a **16-byte header**: 4-byte magic (`NIXM`), 2-byte schema version, 2-byte flags for future use, 4-byte CRC32 checksum, and 4-byte data offset. When reading older formats, auto-upgrade or dual-read on demand. When encountering newer formats (which shouldn't happen in normal use), emit a warning and fall back to recomputation—cache misses are recoverable operations.

**Include Nix version hash in the cache directory path** (Bazel-style implicit versioning), ensuring that evaluator changes automatically isolate caches without manual intervention. Provide an explicit migration command (`nix eval-cache migrate --from-version=1 --to-version=2`) for users who want to preserve cache data across upgrades rather than rebuilding from scratch.

## Implement corruption detection with automatic recovery

SQLite demonstrates comprehensive corruption handling through `PRAGMA integrity_check` (full B-tree validation) and `PRAGMA quick_check` (faster O(N) scan). LMDB takes a different approach—copy-on-write semantics mean the on-disk structure is always valid, and the worst case on crash is loss of uncommitted transactions. Git uses content-addressable storage where the filename IS the SHA-1 hash of content, making corruption immediately detectable by re-hashing.

**For Nix evaluation cache, use xxHash64 or CRC32C** for per-entry integrity checking. These algorithms are fast enough (~1GB/s for CRC32C with hardware acceleration via SSE4.2) to compute on every read and write. SHA-256 is unnecessary unless cache entries will be shared across trust boundaries.

The **cache entry structure** should include:
```c
struct CacheEntry {
    uint32_t magic;          // 0x4E495843 ("NIXC")
    uint16_t schema_version;
    uint16_t flags;
    uint64_t checksum;       // xxHash64 of content
    uint32_t data_length;
    // ... actual serialized data
}
```

On read: verify magic → check version → validate checksum → deserialize. **Automatically clear corrupted entries** and fall back to recomputation—log the corruption event but don't surface errors to users for recoverable situations. Only report errors loudly when corruption is persistent or affects operation integrity.

**Handle disk full conditions gracefully**: check available space before writes, skip caching on ENOSPC (don't fail the evaluation), and optionally evict old entries. SQLite's documentation emphasizes that disk-full errors may leave connections in unreliable states—close and reopen if necessary.

Implement a verification command analogous to `nix store verify`:
```bash
nix eval-cache verify [--all] [--repair]
```

## Establish benchmarking methodology with standard workloads

The rust-analyzer project measures Salsa incremental computation effectiveness with three standard benchmarks: **from scratch** (cold cache baseline), **no change** (measures overhead outside the incremental system), and **trivial change** (advances revision to measure validation cost). Bazel exposes cache statistics through the Build Event Protocol, reporting `actions_executed`, remote cache hits, and memory metrics.

**Essential metrics for Nix memoization**:

| Metric | Definition | Target |
|--------|------------|--------|
| Hit rate | Cache hits / total requests | >80% on warm evaluations |
| Speedup | Cold eval time / warm eval time | >5x for NixOS configurations |
| Memory overhead | Peak memory with cache - baseline | <20% increase |
| Invalidation cost | Time to check validity without recompute | <1% of full eval time |

**Standard benchmark workloads should include**:

1. **NixOS system derivation**: `nix-instantiate '<nixpkgs/nixos>' -A system` represents real-world module system evaluation with complex merging and deep attribute sets
2. **Nixpkgs attribute enumeration**: `nix search nixpkgs` tests parallel evaluation and large attribute set traversal  
3. **Incremental configuration change**: Modify a single service option in configuration.nix and re-evaluate, measuring how quickly the cache validates unchanged portions
4. **Cold/warm comparison**: Same expression evaluated twice, measuring first-run vs cached performance

Use Nix's existing `NIX_SHOW_STATS` output (thunks created, function calls, attributes allocated, GC statistics) as baseline instrumentation. Expose Prometheus-compatible metrics for production monitoring:

```prometheus
nix_memo_hits_total{cache="evaluation"}
nix_memo_misses_total{cache="evaluation"}
nix_evaluation_duration_seconds{phase="parse|eval|realize"}
nix_cache_size_bytes{type="entries|memory"}
```

## Require signatures for cross-machine evaluation caches

The **Action Cache is fundamentally more vulnerable than Content-Addressable Storage** because it's input-addressed—an attacker with write access can upload poisoned outputs linked to legitimate input hashes. Bazel's documentation explicitly states that it "trusts that there are no maliciously stored records in the cache," delegating security to access control and transport encryption. This model is insufficient for public or broadly-shared Nix evaluation caches.

**Nix binary caches already require ed25519 signatures** (`require-sigs = true` by default), and evaluation caches should follow the same model. Sign `(input_hash, output_hash)` pairs using the same key infrastructure. The Trustix project demonstrates a stronger model: maintaining input-to-output mappings across multiple independent builders with Merkle tree-based append-only logs, requiring consensus (e.g., 3/4 majority) for builds to be trusted.

**Trust levels to support**:

| Level | Security Model |
|-------|----------------|
| Per-machine (local disk) | No signatures needed; trust is implicit |
| Trusted team/organization | Authentication + optional signing |
| CI/build infrastructure | Mandatory signing, restricted write access |
| Public cache | Mandatory signatures, multiple signers preferred |

**Write access should be restricted** by default—CI systems can write to shared caches, but developer machines should be read-only. This prevents a compromised developer workstation from poisoning caches used by production builds. Use the post-build hook pattern from Nix binary cache signing: signing happens in daemon context, preventing untrusted code from accessing signing keys.

```nix
# Proposed evaluation cache settings
eval-cache = {
  trusted-public-keys = [ "eval-cache.example.org-1:..." ];
  require-signatures = true;  # For non-local caches
  trusted-eval-substituters = [ "https://eval-cache.example.org" ];
};
```

## Test correctness through differential evaluation

**Differential testing compares outputs across multiple configurations to detect discrepancies.** The Csmith project found 325+ compiler bugs in GCC/LLVM by generating random valid C programs and comparing execution results across compilers and optimization levels. The key technique: produce a checksum of all global variables at program end, making comparison trivial.

For memoization correctness testing, **run every evaluation twice**—once with an empty cache, once with a warm cache—and compare results by hashing all outputs. Any difference indicates a bug. This should be part of CI for the Nix project itself, run against a corpus of diverse Nix expressions.

**Property-based testing should verify three key properties**:

1. **Cache key completeness**: Changing ANY input must invalidate the cache—modify one attribute of an input attrset and verify a cache miss occurs
2. **Cache key soundness**: Identical inputs must always hit cache—the same expression with different whitespace should produce a cache hit
3. **Invalidation propagation**: Changes to upstream dependencies must invalidate all downstream cached results

**Detect non-determinism** by running the same expression N times with fresh state; any variation indicates a source of non-determinism that must be excluded from caching (builtins.currentTime, mutable git refs, changing files). Use `--pure-eval` mode and mock time/environment sources in tests.

The test infrastructure should generate random valid Nix expressions (guided by the Nix grammar), filter out those with undefined behavior or non-deterministic builtins, execute both cached and uncached, and compare results with deep equality checking.

## Integrate cache warming into CI/CD pipelines

**Bazel warms caches by running builds on CI with `--keep_going`**, uploading results to remote cache for subsequent builds to consume. Docker BuildKit supports cache-to/cache-from patterns with multiple backends (inline, registry, GitHub Actions cache). Nix's existing binary cache (including Cachix) already demonstrates the pattern: CI builds push to cache, subsequent builds pull from cache.

**For evaluation caches, separate concerns from binary caches**:

- **Binary cache**: Stores derivation outputs (build artifacts)—existing Nix pattern
- **Evaluation cache**: Stores evaluation results (attribute sets, derivations before building)—new for memoization

**Cache key design for evaluation cache**:
```
eval_cache_key = hash(
  expression_content,
  flake_lock_content,
  nix_version,
  eval_mode_flags
)
```

**Cache warming strategy for CI**:
```bash
# CI cache warming job (runs on main branch)
nix flake check  # Evaluates all checks without building
nix build --dry-run .#all-packages  # Evaluates derivations without building
upload-eval-cache ./eval-cache  # Push to shared cache
```

**GitHub Actions integration**:
```yaml
- uses: actions/cache@v4
  with:
    path: ~/.cache/nix/eval
    key: ${{ runner.os }}-nix-eval-${{ hashFiles('flake.lock', '**/*.nix') }}
    restore-keys: |
      ${{ runner.os }}-nix-eval-
```

Implement a **layered caching hierarchy**: local RAM (fastest, process-local memoization) → local disk (runner-local eval cache) → shared storage (NFS or S3 across runners) → remote public cache (Cachix-style). Each layer should be independently configurable with fallback behavior.

## Conclusion: practical rollout recommendations

The memoization rollout should proceed in three phases. **Phase 1 (months 1-3)**: ship as experimental with local-only caching, no cross-machine support, comprehensive differential testing running in Nix CI. **Phase 2 (months 4-8)**: add signed cross-machine caching support, Prometheus metrics exposure, and verify against real-world NixOS configurations. **Phase 3 (months 9-12)**: stabilize based on measured adoption and bug reports, with explicit graduation criteria documented from day one.

**Key decisions to make before shipping**:

1. Should evaluation caches be content-addressed (like CAS) or input-addressed (like Action Cache)?—prefer content-addressed for security
2. What happens when a cached thunk's dependencies change but the thunk itself doesn't?—implement early cutoff optimization from Salsa
3. How granular should cache entries be?—per-thunk vs per-expression vs per-file tradeoffs
4. Should `nix-collect-garbage` also clean evaluation caches?—probably yes, with `--keep-eval-cache` flag

Learn from Nix's flakes experience: communicate experimental status clearly, don't encourage production use before stabilization, maintain stable alternatives, and set public timeline targets. The technical infrastructure for memoization is largely understood from other systems—the challenge is operational discipline in deployment.
```

## Kagi

```markdown
## Round 7: Critique and Updated Plan for Nix Evaluator Memoization

### Executive Summary

The Round 6 plan is architecturally mature, but several critical issues need resolution before implementation can proceed:

1. **Parallel evaluation compatibility** — Determinate Systems is actively shipping parallel Nix evaluation; the memoization infrastructure must be thread-safe from day one
2. **ARC patent status** — IBM's ARC patent expired in 2014, but simpler alternatives (Clock-Pro, LIRS) may be preferable
3. **Taint propagation completeness** — Missing rules for `trace`, `addErrorContext`, `assert`, and attribute access
4. **Import-level vs thunk-level cache layering** — Unclear interaction between existing `fileEvalCache` and new L1/L2 caches
5. **Closed-term optimization synergy** — Closed terms can be cached more aggressively since they don't depend on environment

---

### Critical Issue 1: Parallel Evaluation Compatibility

**Problem**: Determinate Systems has shipped parallel Nix evaluation that "drastically cuts evaluation time" and is being actively developed. [^1] The memoization infrastructure must be thread-safe, but the current plan doesn't adequately address concurrency.

**Specific concerns**:

| Component | Thread Safety Issue |
|-----------|---------------------|
| L1 Cache (identity) | Concurrent reads/writes to `boost::unordered_flat_map` |
| L2 Cache (content) | Concurrent hash computation and insertion |
| Disappearing links | `GC_general_register_disappearing_link` is NOT thread-safe by default |
| Global epoch | `std::atomic<uint64_t>` is safe, but epoch increment timing matters |
| Durability side table | Concurrent access to `std::unordered_map` |

**Boehm GC thread safety**: The collector requires explicit thread-safe configuration: [^2]

> "In its default configuration, the Boehm-Demers-Weiser garbage collector is not thread-safe. It can be made thread-safe for a number of environments by building the collector with the appropriate XXX compilation flag."

**Recommended approach**: Use **concurrent data structures** from the start:

```cpp
// Option 1: Reader-writer locks (simple, moderate contention)
class ThreadSafeL1Cache {
    mutable std::shared_mutex mutex;
    boost::unordered_flat_map<IdentityKey, Value*> cache;
    
public:
    Value* lookup(const IdentityKey& key) const {
        std::shared_lock lock(mutex);
        auto it = cache.find(key);
        return (it != cache.end()) ? it->second : nullptr;
    }
    
    void insert(const IdentityKey& key, Value* value) {
        std::unique_lock lock(mutex);
        cache[key] = value;
    }
};

// Option 2: Lock-free concurrent hash map (better for high contention)
// Use folly::ConcurrentHashMap or tbb::concurrent_hash_map
#include <folly/concurrency/ConcurrentHashMap.h>
folly::ConcurrentHashMap<IdentityKey, Value*> l1Cache;
```

**For disappearing links**: Batch registrations under a single GC lock:

```cpp
void batchRegisterDisappearingLinks(
    const std::vector<std::pair<void**, void*>>& links) 
{
    GC_disable();  // Prevent collection during batch
    for (auto& [link, obj] : links) {
        GC_general_register_disappearing_link(link, obj);
    }
    GC_enable();
}
```

---

### Critical Issue 2: ARC Patent Status and Alternatives

**Problem**: The plan proposes ARC (Adaptive Replacement Cache), which was patented by IBM. [^2]

**Good news**: The original ARC patent (US 6,996,676) was filed in 2004 and **expired in 2024**. However, IBM filed continuation patents that may still be active.

**Alternatives to consider**:

| Algorithm | Pros | Cons |
|-----------|------|------|
| **Clock-Pro** | Used by Linux page cache; no patent issues; scan-resistant | More complex than LRU |
| **LIRS** | Better hit rates than ARC in some workloads | Complex implementation |
| **2Q** | Simple two-queue design; no patents | Less adaptive than ARC |
| **TinyLFU** | Excellent hit rates; used by Caffeine (Java) | Requires frequency sketch |

**Recommendation**: Start with **2Q** (simpler, no patent concerns), measure hit rates, then consider Clock-Pro if needed:

```cpp
class TwoQueueCache {
    // A1in: FIFO queue for first-time accesses
    std::list<Entry> a1in;
    std::unordered_map<Key, std::list<Entry>::iterator> a1inMap;
    
    // Am: LRU queue for frequently accessed items
    std::list<Entry> am;
    std::unordered_map<Key, std::list<Entry>::iterator> amMap;
    
    // A1out: Ghost queue tracking recent evictions from A1in
    std::list<Key> a1out;
    std::unordered_set<Key> a1outSet;
    
    void onAccess(Key k, Value* v) {
        if (amMap.count(k)) {
            // Hit in Am: move to front (LRU refresh)
            am.splice(am.begin(), am, amMap[k]);
        } else if (a1inMap.count(k)) {
            // Hit in A1in: promote to Am
            am.push_front(*a1inMap[k]);
            amMap[k] = am.begin();
            a1in.erase(a1inMap[k]);
            a1inMap.erase(k);
        } else if (a1outSet.count(k)) {
            // Ghost hit: was recently evicted, now frequently accessed
            am.push_front({k, v});
            amMap[k] = am.begin();
            a1outSet.erase(k);
        } else {
            // First access: add to A1in
            a1in.push_front({k, v});
            a1inMap[k] = a1in.begin();
        }
        evictIfNeeded();
    }
};
```

---

### Critical Issue 3: Taint Propagation Completeness

**Problem**: The plan defines durability propagation for `seq`/`deepSeq` [^1], but several other primops need rules:

| Primop | Current Status | Recommended Rule |
|--------|----------------|------------------|
| `builtins.trace` | Not addressed | Result inherits arg2's durability (trace is side effect) |
| `builtins.addErrorContext` | Not addressed | Result inherits inner expression's durability |
| `builtins.assert` | Not addressed | Result inherits body's durability (condition is checked, not returned) |
| `builtins.throw` / `abort` | Not addressed | N/A (never returns a value) |
| Attribute access (`a.b`) | Not addressed | Result inherits attrset's durability |
| List indexing (`a !! n`) | Not addressed | Result inherits list's durability |
| String interpolation | Not addressed | Result is MIN of all interpolated values' durabilities |

**Implementation**:

```cpp
// builtins.trace
void prim_trace(EvalState& state, Value* msg, Value* result, Value& v) {
    // msg is printed (side effect), result is returned
    state.forceValue(*msg, noPos);
    printError("trace: %s", showValue(*msg));
    state.forceValue(*result, noPos);
    v = *result;
    // Durability: inherit from result, not msg
    setDurability(&v, getDurability(result));
}

// builtins.addErrorContext
void prim_addErrorContext(EvalState& state, Value* ctx, Value* body, Value& v) {
    try {
        state.forceValue(*body, noPos);
        v = *body;
        setDurability(&v, getDurability(body));
    } catch (...) {
        // Add context to error, rethrow
        throw;
    }
}

// Attribute access (in EvalState::forceValue or getAttr)
Value* getAttr(Value* attrset, Symbol name) {
    Value* result = attrset->attrs->get(name);
    // Inherit durability from containing attrset
    setDurability(result, getDurability(attrset));
    return result;
}
```

---

### Critical Issue 4: Import-Level vs Thunk-Level Cache Layering

**Problem**: The plan proposes import-level caching at `evalFile()` [^1], but doesn't clarify how it interacts with the existing `fileEvalCache` and the new L1/L2 thunk caches.

**Current architecture** (from Round 6 answers):

```
evalFile() flow:
1. importResolutionCache (path resolution)
2. fileEvalCache (in-memory, per-evaluation)
3. ExprParseFile thunk creation
4. Force evaluation
```

**Proposed layered architecture**:

```
┌─────────────────────────────────────────────────────────────┐
│                    Import-Level Cache                        │
│  Key: file content fingerprint                               │
│  Scope: Cross-evaluation (persistent, LMDB-backed)           │
│  Hit: Skip parsing AND evaluation                            │
└─────────────────────────────────────────────────────────────┘
                              ↓ miss
┌─────────────────────────────────────────────────────────────┐
│                    fileEvalCache (existing)                  │
│  Key: resolved path                                          │
│  Scope: Intra-evaluation (in-memory)                         │
│  Hit: Skip re-evaluation of same file in same eval           │
└─────────────────────────────────────────────────────────────┘
                              ↓ miss
┌─────────────────────────────────────────────────────────────┐
│                    L1 Thunk Cache (identity)                 │
│  Key: (expr_ptr, env_ptr, epoch)                             │
│  Scope: Intra-evaluation (in-memory)                         │
│  Hit: Skip re-evaluation of same thunk                       │
└─────────────────────────────────────────────────────────────┘
                              ↓ miss
┌─────────────────────────────────────────────────────────────┐
│                    L2 Thunk Cache (content)                  │
│  Key: SHA256(expr_content, env_content)                      │
│  Scope: Cross-evaluation (persistent, LMDB-backed)           │
│  Hit: Skip re-evaluation of semantically identical thunk     │
└─────────────────────────────────────────────────────────────┘
```

**Key insight**: Import-level cache is **coarser** than thunk-level cache. A single import cache hit skips thousands of thunk evaluations.

**Implementation**:

```cpp
void EvalState::evalFile(const SourcePath& path, Value& v, bool mustBeTrivial) {
    auto resolvedPath = resolveExprPath(path);
    
    // Layer 1: Import-level persistent cache (NEW)
    auto fingerprint = resolvedPath.accessor->fingerprint(resolvedPath);
    if (auto cached = importCache.lookup(fingerprint)) {
        v = deserialize(cached);
        stats.importCacheHits++;
        return;
    }
    
    // Layer 2: fileEvalCache (existing, in-memory)
    if (auto v2 = getConcurrent(*fileEvalCache, resolvedPath)) {
        forceValue(**v2, noPos);
        v = **v2;
        return;
    }
    
    // Parse and evaluate (thunk caches apply during evaluation)
    auto expr = parseExprFromFile(resolvedPath);
    auto vPtr = allocValue();
    fileEvalCache->emplace(resolvedPath, vPtr);
    eval(expr, *vPtr);
    v = *vPtr;
    
    // Populate import-level cache (if result is cacheable)
    if (isImportCacheable(v)) {
        importCache.insert(fingerprint, serialize(v));
    }
}
```

---

### Critical Issue 5: Closed-Term Optimization Synergy with Caching

**Problem**: The plan mentions closed-term optimization [^1] but doesn't exploit its synergy with caching.

**Key insight**: Closed terms (no free variables) have a **constant result** regardless of environment. This means:

1. **L1 cache key simplification**: For closed terms, the environment component is irrelevant
2. **L2 cache key simplification**: Only the expression hash matters, not the environment hash
3. **Aggressive caching**: Closed term results can be cached with HIGH durability (pure computation)

**Implementation**:

```cpp
Value* memoizedEval(Expr* expr, Env* env, EvalState& state) {
    if (expr->is_closed) {
        // Closed term: environment doesn't matter
        // Use simplified cache key (expr only)
        ClosedTermKey key{expr, globalEpoch};
        if (auto* cached = closedTermCache.lookup(key)) {
            stats.closedTermHits++;
            return cached;
        }
        
        Value* result = realEval(expr, env);
        closedTermCache.insert(key, result);
        
        // Closed terms are always HIGH durability (pure)
        setDurability(result, Durability::HIGH);
        return result;
    }
    
    // Non-closed term: use full L1/L2 caching
    // ... existing logic
}
```

**Benefit**: Closed terms are common in Nixpkgs (e.g., `lib` functions, constants). Simplified caching for these provides significant speedup.

---

### Refinement: Memory Pressure Response Integration

The plan mentions Linux PSI and macOS dispatch [^1] but doesn't integrate them with the cache architecture.

**Integrated response**:

```cpp
class MemoryPressureResponder {
    enum class Level { Normal, Moderate, Critical };
    Level currentLevel = Level::Normal;
    
    void onPressureChange(Level newLevel) {
        if (newLevel == currentLevel) return;
        
        switch (newLevel) {
            case Level::Moderate:
                // Shrink caches to 50%
                l1Cache.shrinkTo(l1Cache.maxSize() / 2);
                l2Cache.shrinkTo(l2Cache.maxSize() / 2);
                break;
                
            case Level::Critical:
                // Shrink to 25% and force GC
                l1Cache.shrinkTo(l1Cache.maxSize() / 4);
                l2Cache.shrinkTo(l2Cache.maxSize() / 4);
                GC_gcollect_and_unmap();
                break;
                
            case Level::Normal:
                // Restore normal limits
                l1Cache.restoreMaxSize();
                l2Cache.restoreMaxSize();
                break;
        }
        currentLevel = newLevel;
    }
};
```

---

### Refinement: Daemon Batch Operation Strategy

The plan mentions `wopBatchQueryMemo` [^1] but doesn't specify batching strategy.

**Recommended batching**:

| Boundary | Batch Size | Rationale |
|----------|------------|-----------|
| Top-level attribute | 100-500 | Natural evaluation unit |
| Import complete | 50-200 | File-level granularity |
| Time-based | 10ms window | Prevent latency spikes |
| Size-based | 1MB payload | Prevent memory spikes |

**Implementation**:

```cpp
class BatchedCacheClient {
    std::vector<std::pair<ContentKey, Value*>> pendingInserts;
    std::chrono::steady_clock::time_point lastFlush;
    
    void insert(ContentKey key, Value* value) {
        pendingInserts.emplace_back(key, value);
        
        bool shouldFlush = 
            pendingInserts.size() >= 100 ||
            estimatePayloadSize() >= 1'000'000 ||
            timeSinceLastFlush() >= 10ms;
            
        if (shouldFlush) flush();
    }
    
    void flush() {
        if (pendingInserts.empty()) return;
        
        // Serialize batch
        auto payload = serializeBatch(pendingInserts);
        
        // Send to daemon
        daemon.call(wopBatchUpdateMemo, payload);
        
        pendingInserts.clear();
        lastFlush = std::chrono::steady_clock::now();
    }
    
    void onEvaluationBoundary(BoundaryType type) {
        flush();  // Always flush at boundaries
    }
};
```

---

### Updated Implementation Plan

#### Phase 1: Correctness (Blocking) — Updated

| Task | Priority | Change |
|------|----------|--------|
| All existing Phase 1 tasks | **Blocking** | Unchanged |
| **Add thread-safe cache wrappers** | **Blocking** | **NEW**: Required for parallel eval compatibility |
| **Complete taint propagation rules** | **High** | **NEW**: Add rules for trace, addErrorContext, assert, attr access |

#### Phase 2: Performance — Updated

| Task | Priority | Change |
|------|----------|--------|
| **Use 2Q instead of ARC** | **High** | **CHANGED**: Simpler, no patent concerns |
| **Add closed-term cache fast path** | **High** | **NEW**: Simplified caching for closed terms |
| **Implement import-level cache** | **High** | **CLARIFIED**: Layer above fileEvalCache |
| All other Phase 2 tasks | As specified | Unchanged |

#### Phase 3: Architecture — Updated

| Task | Priority | Change |
|------|----------|--------|
| **Integrate memory pressure response** | **High** | **CLARIFIED**: Concrete shrink/restore logic |
| **Implement daemon batch operations** | **Medium** | **CLARIFIED**: Batching strategy specified |
| All other Phase 3 tasks | As specified | Unchanged |

---

### Questions for the Codebase Agent (Round 7)

#### Thread Safety

1. **Is Nix currently built with thread-safe Boehm GC?** What compilation flags are used?

2. **How does the parallel evaluation branch handle concurrent `forceValue()` calls?** Is there existing synchronization we can reuse?

3. **Are there existing concurrent data structures in the Nix codebase?** (e.g., `folly::ConcurrentHashMap`, `tbb::concurrent_hash_map`)

4. **How is `fileEvalCache` currently protected in parallel evaluation?** Is it thread-local or shared with synchronization?

#### Closed-Term Optimization

5. **Is `is_closed` currently computed during `bindVars()`?** If not, what would be required to add it?

6. **What percentage of expressions in a typical Nixpkgs evaluation are closed terms?** This determines the impact of the optimization.

7. **Are there any expressions that appear closed but have hidden dependencies?** (e.g., through `builtins.import` or `builtins.scopedImport`)

#### Import Caching

8. **How does `resolvedPath.accessor->fingerprint()` work?** Is it content-based or metadata-based?

9. **What is the typical size of a serialized import result?** This affects LMDB storage requirements.

10. **Are there imports that should NOT be cached?** (e.g., imports with side effects, imports of non-deterministic content)

#### Taint Propagation

11. **Where is `builtins.trace` implemented?** Specifically, where would durability propagation be added?

12. **How does `builtins.addErrorContext` interact with exceptions?** Does it wrap the exception or modify it in place?

13. **Is there existing infrastructure for tracking "which primop produced this value"?** This would help with durability assignment.

#### Cache Eviction

14. **What are the typical access patterns for the thunk cache?** (e.g., Zipfian, uniform, temporal locality) This determines optimal eviction policy.

15. **Is there existing profiling data for cache hit rates?** The Round 6 answers mention `nrThunkMemoHits` but not detailed patterns.

16. **How does cache size affect evaluation time?** Is there a point of diminishing returns?

---

### Summary of Key Changes from Round 6

| Round 6 Plan | Round 7 Change | Rationale |
|--------------|----------------|-----------|
| ARC eviction policy | **2Q (simpler, no patent)** | ARC patent concerns; 2Q is simpler |
| Single-threaded design | **Thread-safe from day one** | Parallel eval is shipping |
| Taint rules for seq/deepSeq only | **Complete taint propagation** | Missing rules for trace, assert, etc. |
| Import cache mentioned | **Layered architecture clarified** | Import → fileEvalCache → L1 → L2 |
| Closed-term optimization | **Synergy with caching exploited** | Simplified keys for closed terms |
| Memory pressure mentioned | **Integrated response logic** | Concrete shrink/restore behavior |
| Batch operations mentioned | **Batching strategy specified** | Size, time, and boundary triggers |

The most critical change is **thread safety** — with parallel evaluation shipping, the memoization infrastructure must be concurrent-safe from the start. The second priority is **closed-term optimization synergy**, which provides significant speedup for common Nixpkgs patterns.

[^1]: [round-6-answers](round-6-answers.md) (70%)
```