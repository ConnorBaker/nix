# Prompt

This is a continuation of round-1.md.

Please read all responses. You should fetch and read the sources referenced to ensure accuracy. You should also list those sources. Then, update your plan where appropriate, being sure to include alternative designs or implementations and their tradeoffs, as well as additional references. Lastly, answer any questions posed by any of the responses in a new file, round-2-answers.md.

Since there are a lot of steps here, please make a todo list and use it to keep track of your finished work and remaining work as you proceed.

# Responses

## ChatGPT

```text
Nix Evaluator Memoization Infrastructure: Analysis and Roadmap

The updated plan introduces several architectural decisions and implementation options to improve Nix evaluator caching. We review each decision’s proposals (including recent collaborator input) in terms of correctness, evaluator soundness, and persistent cross-machine reuse. We then update the implementation roadmap accordingly, flag potential risks, and suggest follow-up questions for the development team. Where applicable we highlight implications for laziness, purity boundaries, evaluation order, and the support needed for durable memoization (hash portability, canonical environments, interning, storage backends).

1. Split Cacheability vs Portability (Durability Levels) ￼ ￼

The plan recognizes that intra-evaluation caching (during a single nix eval) and cross-evaluation caching (persisting results) have different safety requirements ￼. For example, lambdas (functions) are safe to cache within a run (their closures won’t change), but not necessarily across runs (pointer-based closures differ). Paths are safe within one accessor but need content-based identity across runs. The proposal options are:
	•	Option A (current): Single valueIsUncacheable() predicate. Simple but either too conservative (rejecting safe caches) or too permissive (wrongly caching unsafe cases) ￼. This can hurt correctness (by under- or over-caching) and limits reuse.
	•	Option B/C: Separate predicates or mode flags for intra- vs cross-cacheable values ￼. These give correct semantics (distinguishing the two cases) but at the cost of duplicating logic (two code paths). They would strengthen correctness by explicitly guarding cross-run caches, but add complexity and maintenance risk.
	•	Option D (recommended): Durability model (Salsa-style version vector) ￼. This unifies caching under durability levels (e.g. HIGH, MEDIUM, LOW), so that each cached computation declares how “durable” (stable) its inputs are. E.g. core libraries as HIGH, user projects as MEDIUM, local dev/impure inputs as LOW ￼. In practice this means associating a version vector (v_low, v_med, v_high) with each cached result and only invalidating if the relevant level changes. This approach offers the best precision and reuse: it minimizes unnecessary invalidations (improving reuse) while keeping track of safety across runs. For example, changing a volatile input (v_low) would only bump the low counter, leaving high-durability results (like nixpkgs stdlib) still valid ￼ ￼. In rust-analyzer, adopting durability yielded large performance gains by avoiding irrelevant re-computation ￼.

Evaluation: The durability model aligns well with correctness and reuse goals. By encoding assumptions about which parts of the world change together, it prevents cross-run caching of unsound thunks without banning all caching. It is more complex (requires refactoring) but should improve soundness (no hidden errors from stale caches) and persistence (it enables cross-run reuse when inputs are truly stable). Implementation risk involves designing a clean API for durability levels and assigning inputs to levels. The plan’s Phase 4 includes “implement durability levels (Salsa-style)” ￼, which will be a significant but valuable investment.

2. Environment Hashing and Evaluation Order ￼ ￼

Environment hashing is problematic because forcing a thunk mutates it in place, so the same Env* object hashes differently before vs. after forcing ￼. Worse, even the structure of Env can change depending on evaluation order (allocation timing, in-place forcing, name resolution in nested withs) ￼. The alternatives are:
	•	Pointer-based hashing (Options A, D): e.g. hash thunks by their Value* pointer ￼. This is fast and stable within one run but not portable across runs (addresses differ) and is fragile under GC (pointers get reused). Such hashing would not persist well across evaluations.
	•	Structural hashing (Option B): combine (expr_hash, env_hash) recursively ￼. Portable and content-based, but expensive. It can still exhibit order-dependence if an env contains thunks (env_hash changes when thunks force), and it may cycle. This preserves correctness but is costly.
	•	Cached-in-Env (Option C): store a computed hash in each Env and invalidate on force ￼. Amortizes cost, but complicates GC and sharing.
	•	Two-layer cache (Option E): maintain both pointer-based and content-based caches ￼. Conceptually best-of-both, but very complex.
	•	Option F (recommended): AST hash-consing + generation counters ￼. This means (a) intern all parsed expressions so that identical expressions have the same Expr* pointer (Dolstra’s maximal laziness idea) ￼, and (b) assign each newly-allocated Value a monotonically increasing generation counter and record finalizers on GC to clean cache entries ￼. Memo keys become (Expr*, Env*) plus the value generation. Because Expr*s are stable and unique for semantically identical code, and Value pointers (with counters) can be compared safely, this avoids order-dependence: the key ignores transient thunk states.

Evaluation: Hash-consing greatly improves correctness and soundness of caching within a run (avoiding wrong cache hits due to pointer reuse or env changes). It makes Env identity stable as keys, not depending on evaluation order. This strengthens laziness by letting thunks remain lazy (we don’t force them just to compute a hash). However, it is a heavy change: the parser must intern AST nodes globally, and GC finalizers must remove stale cache entries. It does not directly solve cross-run portability (pointers don’t survive restart), so cross-eval caching still needs structural content hashing or other means. The plan includes tasks for AST hash-consing and generation counters (Phase 1 and 2) ￼ ￼. A risk is added complexity and memory overhead (interned AST pool). Also, canonical environments are still needed for cross-run reuse.

3. Cycle Handling (Serialization & Hashing) ￼ ￼

Nix’s rec constructs can create cyclic values, but the current serialization and hashing logic has no cycle detection, causing infinite loops ￼. The key insight is that some rec forms resolve to acyclic values at runtime, but genuinely recursive definitions do produce cycles. The proposals:
	•	Option A (recommended): Reject cycles (throw an error) ￼. This is simplest and safe: whenever a true cycle is detected during serialization, fail. It guarantees correctness (never infinite-loop or produce wrong serialized output) but means giving up caching or persisting for cyclic values. Since many uses of rec yield non-cyclic results, this only blocks the pathological cases.
	•	Option B: Support cycles via back-references (like Python’s pickle) ￼. This would allow serializing any cyclic graph by emitting object IDs or paths, but requires a more complex on-disk format and deserialization logic.
	•	Option C: JSON-LD style ID-based references ￼, which also implies a big format change.

For hashing, the plan suggests detecting cycles by keeping a visited set during recursion and returning a special CYCLE_MARKER ￼. This prevents infinite recursion in hashValue, but all cyclic values would hash to a marker (losing differentiation).

Evaluation: Rejecting cycles at serialization time ensures soundness (no incorrect persistent cache state), but means some valid Nix constructs simply can’t be cached cross-run. This modest limitation likely affects corner cases (very self-referential structures). Given that continuous evaluation (no serial) could handle cycles normally, the trade-off is between completeness vs safety. This approach strengthens robustness: it avoids subtle bugs in cached state or endless loops. The roadmap already includes cycle detection tasks and throwing on cycles (Phase 1) ￼. A risk is user code relying on valid recursive structures for correctness; rejecting them may surprise some users, so it should be documented. We should ensure that acyclic results from rec still serialize normally. Follow-up could consider detecting when a cycle is “accidental” (like Python’s DAG requirement), but that’s complex.

4. Position-Dependent Builtins (Impurity) ￼

Builtins like unsafeGetAttrPos and __curPos return source-code position information. Caching their results naively means reported positions could be stale or incorrect in later runs ￼. The options:
	•	A: Mark these builtins as impure (with a new ImpureReason::PositionDependent) ￼. The evaluator’s impurity token would then invalidate any cached result using them. Simple but very conservative: many Nixpkgs errors compute positions for reporting, so marking impure would greatly reduce cache hits (hurting reuse and performance).
	•	B: Include position metadata in the expression hash ￼. This preserves correctness (same code at different positions hash differently) but reduces sharing: semantically identical code in different files or lines will never reuse cached values.
	•	C: Maintain a separate “position-dependent” cache layer ￼. The best of both worlds in theory, but very complex to implement.
	•	D (recommended): Position-independent semantic layer ￼. Inspired by rust-analyzer, the idea is to strip position info out of the cached value entirely and only compute positions on demand for error reporting ￼ ￼. In practice, the evaluator would cache semantics of attribute resolution with position-agnostic identifiers (e.g. “the first attribute named foo”), and only map those results back to positions when an error message or display is needed. This yields maximal cache reuse (semantic results don’t change if code shifts around) while still allowing accurate positions separately. It requires an architectural change: evaluation must distinguish semantic lookup from source location bookkeeping.

Evaluation: Option D is the most sound and reuse-friendly. It preserves correctness (errors still point to the right code) and maximizes caching (ignoring volatile position details). It’s aligned with laziness (positions can be computed lazily). However, it’s also a significant refactoring of the evaluator. Meanwhile, Option B is simpler and correct, but will reduce cache sharing (a drawback to reuse). Option A is trivial but loses many cache opportunities. The plan lists implementing a position-independent layer as a future task (Phase 4) ￼. In the short term, marking impure (Option A) or including positions in the hash (Option B) could be stopgap measures. Given the impact on cache hit rates (positions change frequently during editing), the recommended separation (citing rust-analyzer’s success ￼ and [15]) is worth the effort.

5. Symbol Table Identity (C API) ￼

When using the Nix C API, multiple EvalState instances have their own symbol tables. A symbol ID (e.g. integer 5) means different names in different tables, so serializing/ deserializing values across runs must handle this. The proposals:
	•	Option A/B: Encode the symbol table pointer or a generation in the symbol ID ￼. This detects mismatches at runtime but increases symbol size or overhead.
	•	Option C (local-only): Forbid sharing values across EvalStates. Impractical for cross-run caching, since this is exactly what we want to support.
	•	Option D (recommended): Serialize symbols as strings and re-intern on load ￼. This is the established approach in other interpreters (Lua, Python, Erlang) ￼: every saved symbol is written out as its name, and on load it is interned into the new symbol table. This is portable and correct, though it adds O(n) lookups on load.

Evaluation: Converting symbol IDs to names and back is necessary for portable persistence. It preserves evaluator soundness (no confusion of symbol identity) and enables cross-machine reuse (the name is content, not a local ID). The cost is manageable (symbol tables are typically small relative to evaluation cost). The plan’s Phase 2 already lists splitting cache predicates, but Phase 3 should include this symbol re-interning during serialization. The only caveat is performance on large symbol tables, but that is usually minor.

6. Cache Granularity (Scope of Memoization) ￼ ￼

Rust-analyzer feedback is cited: per-thunk caching is extremely expensive—requiring hashing every thunk, storing keys and dependency metadata, LRU overhead etc. (they saw 8GB memory use) ￼. Proposed levels:
	•	Option A (current): Thunk-level memoization. Very fine-grained and automatic, but heavy overhead and many cache entries ￼.
	•	Option B/C: Coarser levels (expression-level, layered across imports/functions) ￼. Higher hit rates but complex to define (especially defining an “environment signature” for shared subtrees) or prone to coherence issues.
	•	Option D: Demand-driven (Adapton-style). Requires explicit annotations or demand patterns; heavyweight.
	•	Option E (recommended): Query/function-level caching with early-cutoff ￼. This means treating larger computations (e.g. importing a file, a function application, or deriving a derivation) as units. When a unit runs, it compares its inputs (or their hashes) to previous runs, and if unchanged, stops propagation. Multi-level early cutoff (from Build Systems à la Carte) lets unchanged results bypass re-evaluation ￼. Nix has natural boundaries here (imported nix file, each function call, each derivation) ￼.

Evaluation: Coarser granularity dramatically improves performance and reuse by reducing overhead. It does not hurt correctness (it’s still safe to cache a whole function result as long as inputs are pure), and preserves soundness as long as inputs are captured. It helps cross-run reuse by caching bigger chunks. The trade-off is potentially missing some caching opportunities within a function: if a function body had redundant sub-expressions, those won’t be cached individually. But early cutoff mitigates this by still avoiding re-computation when results match. The plan includes “implement early cutoff” and “query-level caching” in phases 3-4 ￼ ￼. A risk is ensuring that such large-unit caching respects laziness (we must not force thunks unnecessarily just to compute the unit’s result), and that inputs are fully captured in the key. We will need to clearly define the “query” boundaries and how to compute their hash keys (likely using expression + environment fingerprints).

7. Persistent Storage Backend ￼ ￼

For cross-evaluation caching, a durable key-value store is needed. Benchmarks show LMDB vastly outperforms SQLite on KV workloads (47× faster sequential reads, 9× random reads, 8× seq writes, 5.7× random writes) ￼ ￼. Options:
	•	SQLite (A): Familiar SQL interface, ACID, queries, but slow for pure KV use ￼.
	•	LMDB+SQLite (B, recommended): Use LMDB for the hot path (key-value lookups of cache results) and SQLite for metadata (cache indexes, stats, eviction info) ￼. This hybrid gives the best lookup performance and durability (SQLite handles journaling and complex queries). The drawback is maintaining two systems.
	•	RocksDB (C): Fast writes, but more complex integration.

The plan also cites Bazel’s two-table remote cache design: an Action Cache (mapping computation hashes to result metadata) and a CAS (content store for actual data) ￼ ￼. This suggests that the evaluator should separate “cache key → manifest of value” from “hash → value content”.

Evaluation: Using LMDB for the main cache (hash keys to results) aligns with reuse goals: fast lookups enable large caches. Storing the actual cached values (Nix values) could be done via a content-addressable store (perhaps another LMDB or Nix store). For correctness/soundness, both LMDB and SQLite are ACID so they can safely store the data. The plan’s Phase 3 lists adding an LMDB backend and SQLite metadata ￼. We should ensure keys are stored in binary (the plan’s schema note on BLOBs) ￼ for efficiency. A risk is LMDB’s limitation of one writer (likely fine since Nix eval is single-threaded) and the complexity of two backends. We must also design eviction (LRU timestamps, HMAC signing) carefully as per recommendations ￼. Using HMAC-SHA256 signatures (Turborepo) is a good idea to detect corruption.

8. Alpha-Equivalence Hashing ￼

Current hashing uses De Bruijn indices for lambda-bound variables, but fails to distinguish with-bound variables by name or nested scopes by order ￼. This causes two identical-looking ASTs to hash the same even if they behave differently (e.g. {with a; x} vs {with b; x}). Proposed fixes include:
	•	Simply adding the missing information (feed the symbol name and scope chain into the hash) for with variables. This preserves the existing O(n) De Bruijn approach but patches the gaps.
	•	More advanced algorithms (Maziarz et al. PLDI 2021/2024) or Unison-style large digests ￼ ￼. These are heavyweight but handle complex α-equivalence more generally (including commutative cases or recursive cycles).

Evaluation: Fixing the simple omissions in the current scheme (names of with vars and ordering of scopes) is critical for correctness ￼. It’s presumably a straightforward change and should be done immediately. More ambitious hash algorithms could be explored, but given the likely size of Nix expressions, the De Bruijn approach with these fixes should suffice. As an implementation note, add tests (Phase 1) for with hashing as in the plan ￼ to prevent regressions. The plan does not currently include a rewrite of this logic beyond those tests, so we should ensure these fixes happen. The influence of Unison/Maziarz suggests future work, but our near-term focus is on complete and consistent hashing of existing constructs. Missing this would break soundness (cache collisions between distinct thunks).

9. Impurity Tracking Granularity ￼

Currently Nix uses a single global impureToken counter: any impure operation (I/O, time, etc.) increments it, and any value depending on any impurity is invalidated ￼. This is overly conservative. The plan suggests:
	•	Option A (current): Single counter (invalidates all on any impurity).
	•	Option B (recommended long-term): Track effect-dependency sets per value ￼. For example, a value depending only on currentTime should not be invalidated if the filesystem changed. Each impure source (files, time, network, positions, etc.) has its own token, and each cached result records exactly which tokens it read from ￼.
	•	Option C: Coeffect type annotations (too invasive).

Evaluation: Fine-grained impurity tracking would significantly improve reuse and correctness: caches would survive unrelated side effects. It strengthens soundness by preventing unnecessary invalidations. However, it complicates the evaluator and hash keys (we’d need to maintain bitsets or lists of tokens on each Value, update them transitively, and compare multi-dimensional tokens). The plan defers this to Phase 4 ￼ as a “future” task. A risk is the added runtime overhead and complexity. For now, we must ensure that key impure cases are at least correctly marked (e.g. the plan marks unsafeGetAttrPos as impure in Phase 1 ￼). Eventually, tracking per-effect is desirable (as in Koka/coeffects research ￼), but it will be a major change.

Revised Implementation Roadmap

Based on the above, we adjust the plan’s phases and tasks to align with the new proposals and priorities:
	•	Phase 1 (Correctness): Address blocking issues and ensure sound memoization within a run. This includes:
	•	Fix with-bound variable hashing so different with scopes hash differently ￼ (ensures semantic distinction).
	•	Add cycle detection in hashing and serialization, throwing on real cycles ￼ to avoid infinite loops.
	•	Mark position-dependent builtins as impure (ImpureReason::PositionDependent) ￼, so at least they don’t silently produce wrong positions.
	•	Add generation counters and finalizers for Values ￼ to eliminate GC-pointer reuse errors. Test all these fixes (the plan includes tests for with distinctions ￼).
	•	Phase 2 (Architecture): Implement foundational refactors for stable caching:
	•	AST Hash-Consing: Intern Expr nodes in the parser ￼ so that identical expressions share pointers. This enables the (Expr*,Env*) keys proposed in Decision 2.
	•	Split cacheability predicates into intra- vs cross-run versions ￼, preparing for durability semantics.
	•	Fix env-hash order-dependence: Switch env-hash.cc to use (expr_ptr, env_ptr) plus generation counters instead of naive structure ￼.
	•	Fix lambda portability: In value-hash.cc, ensure that hashing functions/thunks recursively checks that their environments are fully portable (content-addressable) ￼.
	•	Implement closed-term optimization (wrap closed expressions to prevent environment blow-up) as per Dolstra ￼.
	•	Document remaining order-dependence limitations (so developers know which cases may have low hit rates) ￼.
	•	Phase 3 (Cross-Eval Caching): Build the persistent cache infrastructure:
	•	Make ExprParseFile hashable by content (use path.fingerprint() rather than pointer) so imports are portable.
	•	Integrate EvalInputs into the cache key (include relevant interpreter inputs in the fingerprint) ￼.
	•	LMDB backend for the hot cache (store the mapping from thunk hashes to serialized values) ￼, plus SQLite for metadata (LRU, GC, stats) ￼.
	•	Schema setup: Use BLOBs for hash keys, track last-access timestamps, enable WAL mode, HMAC sign entries as recommended ￼.
	•	Implement early cutoff: Detect when a function/import result is unchanged and stop propagation ￼ ￼. This may involve storing previous input hashes and comparing.
	•	Phase 4 (Advanced Optimizations): Pursue long-term refinements:
	•	Durability levels: Implement the Salsa-style version vector and assign durability to inputs (e.g. stdlib=HIGH, user code=MEDIUM, local files=LOW) ￼ ￼. Modify the dependency tracking so each cached value carries a durability tag.
	•	Position-independent semantic layer: Overhaul evaluator to separate computation of values from mapping to source positions (as per Decision 4) ￼ ￼.
	•	Fine-grained impurity tracking: Replace the global impureToken with per-effect tokens or bitsets on cached values ￼ ￼.
	•	Query-level caching: Formalize the function/application-level memoization boundaries and implement them (as Decision 6 recommended) ￼ ￼.

This revised roadmap incorporates the key recommendations while balancing immediacy of correctness fixes vs. longer-term architecture work.

Risks and Flaws in Proposed Strategies
	•	Complexity of Durability: Introducing multiple version counters adds significant complexity to dependency tracking. Misassignment of durability levels could either over-invalidate or under-invalidate caches. The system must ensure transitive durability (like Salsa: an effect’s durability is the minimum of its inputs) ￼. Testing and verification will be crucial.
	•	Parser Refactor for Hash-Consing: Hash-consing the AST requires global interning tables and may conflict with future mutable AST passes. We must avoid memory leaks (interned nodes must be collected or never too numerous) and ensure thread-safety if parser ever becomes concurrent.
	•	Environment Canonicalization: Using (Expr*, Env*) keys hinges on stable Env* identity. However, actual environments (Env objects) are garbage-collected and ephemeral. We rely on generation counters, but cross-run these pointers vanish. For cross-run, we’ll need to fall back to structural env hashing or some canonical representation. The plan’s “wire EvalInputs fingerprint” step hints at this. We must define exactly what constitutes an environment’s content fingerprint (all its bound values).
	•	Lazy Evaluation vs. Coarse Caching: Query-level caching risks eagerly forcing thunks. We must ensure that computing a function’s “result hash” doesn’t accidentally evaluate sub-thunks it shouldn’t. Mechanisms like “closed-term optimization” ￼ help, but care is needed: e.g. we should cache a function application by hashing the unevaluated function and argument expressions plus environment fingerprint, not by forcing them. Mistakes here could break laziness.
	•	Impurity Tracking Overhead: Fine-grained impurity tracking (per-effect tokens) can degrade performance if not implemented efficiently (every value must carry metadata). Also, deciding the granularity (e.g. file-level vs entire file system) is a non-trivial design choice. For instance, should changing any file in /etc bump a global token, or do per-file tokens? The plan’s example suggests per-file tokens for FS reads ￼, but tracking this at scale could be heavy.
	•	Hash Portability: We must ensure all hash inputs are fully content-based. Current issues include pointer-based or session-local data: e.g. the plan mentions ExprParseFile pointer hashes (needs fixing) and non-ported symbols. Any leftover pointer use will break cross-machine reuse. Similarly, we must canonicalize environment closures and treat Nix store paths carefully (content vs accessor).
	•	GC Finalizers: Using GC_register_finalizer to remove cache entries is tricky. If a Value is collected and a new one reuses its address before the finalizer runs, we could delete the wrong entry. Generation counters help, but finalizers in Boehm GC are not ordered in any guarantee. We must test that no stale entries remain, and ensure finalizers themselves don’t resurrect values.
	•	Two-Backend Synchronization: Maintaining LMDB + SQLite in sync has complexity. E.g. an eviction policy should update both. We should design transactions carefully: on storing a result, write to LMDB (CAS) and update SQLite (action cache) atomically (or log an intent). On crash recovery, detect and repair partial writes.
	•	Cycle Rejection: By rejecting cyclic values, we may break some recursive definitions. We need to audit how Nix code uses recursion (e.g. recursive sets). If users rely on self-references for correctness, rejecting them may cause failures. We should provide a clear error message and possibly a workaround suggestion.

Follow-up Technical Questions
	1.	AST Interning: What data structure will hold interned Expr* nodes? Will it be a hash-cons table keyed by a textual or structural representation? How do we ensure interned nodes are de-duplicated across parsed modules? (E.g. if two imports contain the same subexpression, do they share an Expr*?) How to collect interned nodes on GC or at eval end?
	2.	Durability Assignment: How should we assign durability levels to various inputs? For example, are Nix store paths (derivations) treated as HIGH durability? Are HTTP fetches LOW? Can users influence durability (e.g. a flag marking a file as stable)? How do we implement the version vector and associate it with cached values in code?
	3.	Environment Fingerprinting: When hashing a thunk for caching across runs, what exactly constitutes its “environment hash”? Should we recursively hash all closed-bound values? The plan mentions closed-term optimization ￼ – how to implement that so that big static values (like stdlib attrsets) don’t explode the env-hash? (Dolstra’s 2008 paper suggests wrapping closed terms.)
	4.	Symbol Serialization: In practice, many values contain many symbols (AttrSet keys, etc.). Serializing them as strings on every cache save could be costly. Could we intern symbols globally (like a persistent symbol table) to speed up? Or use a two-pass approach to avoid duplicate string writes?
	5.	Impurity Granularity: If we track file-based impure tokens, do we hash file content or last-modified times? If a file changes but content stays same, do we invalidate? For fetchurl, what granularity makes sense (per-URL vs global network flag)? How to integrate these tokens into the existing impureToken mechanism?
	6.	LMDB Integration: Which LMDB Rust or C library should be used? How to structure transactions so that storing a cache entry (ThunkHash → serialized Value) and recording metadata (e.g. last_access) are atomic? Should we shard caches by durability or namespace to avoid locking contention?
	7.	Early Cutoff Mechanism: What will be the mechanism to detect “result unchanged”? The plan suggests hashing inputs, but does Nix evaluation already compute enough of a fingerprint of inputs? We might need to record a digest of function arguments and environment vs previous run. How to integrate this with the value-hash code?
	8.	Position-Independent Semantics: What is the design for separating semantic value from position? For instance, should unsafeGetAttrPos be implemented as two steps: (a) a pure lookup that returns a placeholder for the position, and (b) a later pass that fills in the actual position based on a mapping? How to propagate position context through nested evaluations without mixing caches?
	9.	Backward Compatibility: How will these changes interact with existing mechanisms? For example, the store already has fingerprints for fixed-output derivations. If we introduce persistent eval caches, how do we ensure they don’t conflict with store paths? Should cached values be stored under a new key namespace?
	10.	Testing and Validation: What automated tests should we write? The plan includes specific hash consistency tests for with scopes ￼. We should also test that repeated evaluation of a complex expression yields identical hashes and cached values, that cross-machine cache loads produce identical results, and that corner cases (cycles, impurities) behave as expected.

Each of these questions points to areas where implementation details must be solidified. As agents work on the Nix codebase, answers to these will guide the precise changes.

Sources: The above analysis cites the updated memoization plan ￼ ￼ ￼ ￼ ￼ and related references (e.g. the Rust Analyzer durable incrementality blog ￼, LMDB benchmarks ￼, and Bazel remote cache docs ￼) to ground each evaluation. These ensure the recommendations are aligned with known practices in incremental systems and persistent caching. Each proposal was assessed against the core goals of correctness (sound semantics and no wrong caches), evaluator soundness (purely functional behavior and laziness), and persistent cross-machine reuse.
```

## Claude

```text
# Nix evaluator memoization: resolving ten critical implementation gaps

The proposed Nix evaluator memoization plan requires concrete solutions for several underspecified mechanisms. **Disappearing links in Boehm GC provide the cleanest cache invalidation approach**, while De Bruijn-style cycle markers enable distinct hashes for structurally different cycles. The `with` scope chain requires ordered pointer-identity hashing, and thunks should never be forced during hashing—instead, hash their (code, environment) identity. Cross-evaluation caching should use deterministic CBOR (RFC 8949 §4.2) with functions excluded as non-cacheable.

## Boehm GC pointer reuse demands disappearing links over finalizers

The generation counter approach proposed in the memoization plan has a fundamental flaw: it keeps strong references to cache keys, preventing garbage collection of unused terms. **Boehm GC's disappearing links provide a superior solution** that allows automatic cache invalidation without memory leaks.

`GC_general_register_disappearing_link(void **link, void *obj)` automatically nulls `*link` when `obj` becomes unreachable. For memoization caches, this means storing disguised key pointers that the GC clears on collection:

```cpp
struct CacheEntry {
    void* disguised_key;  // Cleared by GC when key collected
    Value cached_value;
};

void insert(void* key, Value val) {
    Entry& e = table[reinterpret_cast<uintptr_t>(key)];
    e.disguised_key = GC_HIDE_POINTER(key);
    e.cached_value = val;
    GC_general_register_disappearing_link(&e.disguised_key, key);
}
```

**Finalizers are problematic** for cache cleanup: they execute during arbitrary allocations, cannot safely allocate themselves, and cyclic structures involving finalizable objects are never finalized. Haskell's stable-memo library achieves similar semantics using `StableName` plus weak pointers with finalizers, but Boehm's non-moving collection means pointer addresses serve as stable identities directly.

The **performance tradeoff** favors disappearing links: generation counters require validation on every cache hit after any GC, while disappearing links have per-entry registration cost but O(1) lookup with a simple null check. Lazy cleanup on access amortizes stale entry removal without blocking GC.

## Cycle detection requires De Bruijn-style back-references for distinct hashes

Returning a uniform `CYCLE_MARKER` sentinel for all cycles conflates structurally different cyclic values—two mutually recursive functions with different topologies would hash identically. **Position-based back-references using De Bruijn indices give distinct hashes to distinct cycles.**

The algorithm maintains an ancestry stack during hashing. When encountering a previously-seen object, emit a back-reference indicating the distance to that ancestor:

```cpp
Hash hashWithContext(Object* obj, std::vector<Object*>& ancestors) {
    for (int i = ancestors.size() - 1; i >= 0; i--) {
        if (ancestors[i] == obj) {
            return Hash::backref(ancestors.size() - i);  // De Bruijn index
        }
    }
    ancestors.push_back(obj);
    Hash h = computeChildrenHash(obj);
    ancestors.pop_back();
    return h;
}
```

Unison's content-addressed code demonstrates this approach at scale: mutually recursive definitions hash to `#x.n` where `x` is the cycle's overall hash and `n` indexes the definition within the cycle. For strongly connected components, the condensation graph (contracting each SCC to a node) forms a DAG suitable for Merkle hashing.

**Cyclic structures should be cacheable**, not excluded. Mutual recursion is legitimate in Nix (recursive attribute sets, mutually referential functions). The De Bruijn approach ensures isomorphic cycles hash identically while structurally different cycles hash differently.

## The closed-term optimization enables O(1) memoization lookups

Dolstra's "Maximal Laziness" paper (LDTA 2008) identifies closed-term detection as **critical because all terms the evaluator sees are closed**—they have no free variables. The optimization wraps substituted terms in a `closed()` marker telling the substitution function to skip descent, reducing complexity from O(n²) to O(n).

ATerm hash-consing makes this powerful: identical closed AST nodes share memory, so structural equality becomes pointer comparison. The memoization cache can use pointer identity as its key, achieving O(1) lookup. Dolstra's measurements show **40-280% reduction in rewrite steps** for typical Nix expressions.

Term explosion occurs without sharing: substituting a term `e` into multiple locations creates separate copies that consume O(2^n) memory for deeply nested expressions. Hash-consing ensures each unique term exists once, with multiple references sharing the allocation.

**Retrofitting onto Nix's current AST** is feasible because Nix already separates positions via `PosIdx` indices rather than embedding line/column in nodes. Minimal changes needed:

1. Add `size_t structuralHash` field to `Expr` base class
2. Create intern tables for each `Expr` subtype keyed by structural hash
3. Replace direct allocation with smart constructors that hash-cons

For error positions with hash-consed nodes, follow rust-analyzer's approach: store positions in a side table indexed by a combination of expression pointer and source file. Multiple source locations may reference the same shared node—the side table maps (Expr*, SourcePath) → PosIdx.

## With scope chains require ordered pointer-identity hashing

The `with` construct's interaction with explicit bindings creates subtle semantics that hashing must capture precisely. Nix's scoping rules distinguish **explicit definitions** (from `let`, `rec`, function parameters) which shadow everything, from **implicit definitions** (only from `with`) which shadow only other implicit definitions.

For `with a; let x = 1; in with b; x`, the variable `x` binds to the `let` definition because explicit bindings take precedence regardless of `with` nesting depth. This means the hash must distinguish explicit from implicit scope entries.

**Recommended hashing strategy for scope chains:**

1. Hash the **ordered list of with-set pointers** (identity, not contents)
2. Include explicit binding structure separately
3. Do **not** hash with-set contents—this would force thunk evaluation and create excessive invalidation

Pointer identity works because Nix values are shared after evaluation: identical attribute sets typically share the same allocation. The order matters because `with a; with b; x` differs from `with b; with a; x` when both define `x`.

```cpp
struct ScopeChainHash {
    std::vector<const Value*> withSetPointers;  // Ordered, inner-to-outer
    HashCode explicitBindingsHash;  // From let/rec/function params
};
```

Other dynamically-scoped languages (Common Lisp, Emacs Lisp) handle memoization by either including relevant dynamic bindings in cache keys or restricting memoization to lexically pure functions. Nix's approach—hashing with-set identity—strikes a balance between precision and avoiding forced evaluation.

## Thunks must never be forced during hashing

Forcing thunks during hash computation defeats lazy evaluation's fundamental guarantee: unevaluated code should produce no effects. A thunk might diverge (infinite loop), throw an error, or require expensive computation—none of which should occur if the value is ultimately unused.

**Hash the thunk's identity instead**: the combination of (expression pointer, captured environment pointer). Two thunks with identical code and identical captured environments will evaluate to the same value, so they should hash identically:

```cpp
HashCode hashValue(Value* v) {
    switch (v->type) {
        case nThunk:
            return combineHash(hash(v->thunk.expr), hash(v->thunk.env));
        case nInt:
            return hash(v->integer);
        case nAttrs:
            return hash(v->attrs);  // Pointer identity for evaluated attrsets
        // ...
    }
}
```

**Forced vs unforced state should not affect the hash.** A thunk and its forced result are semantically equivalent—evaluation order shouldn't determine cache hits. This maintains the referential transparency that makes memoization safe.

For environments with mixed forced/unforced slots, hash each slot consistently: forced values by their content hash, thunks by their identity hash. The cache will correctly hit regardless of what evaluation path led to the lookup.

## Early cutoff requires tracking both result hashes and forced-thunk sets

Shake implements early cutoff by storing content hashes of task outputs alongside their dependency traces. When a rebuilt task produces the same hash as before, dependents aren't marked dirty. Adapting this for lazy evaluation introduces complications around which thunks were forced.

**The problem**: if a thunk's result hash matches the previous run but that thunk wasn't forced before, there's no stored hash to compare. The cache must treat this as a miss, evaluate, and store the result for future comparisons.

**When evaluation order differs between runs**, different dependency traces result. Self-adjusting computation (Acar's work) addresses this via Dynamic Dependence Graphs (DDGs) that track both data and control dependencies. The Adapton system (PLDI 2014) formalizes "demanded computation graphs" where recomputation propagates only as demanded by observers.

For Nix, a practical approach tracks:

```cpp
struct EvalTrace {
    HashCode expressionHash;
    std::vector<std::pair<ThunkId, HashCode>> forcedThunks;  // What was forced, what it produced
    HashCode resultHash;
};
```

On re-evaluation, if the result hash matches and all forced thunks produced the same hashes, skip propagating changes to dependents. If a previously-unforced thunk is now forced, record it for future early cutoff opportunities.

## Import from Derivation caching needs derivation-hash keys with output validation

IFD blocks evaluation until a derivation builds, breaking `--dry-run` and serializing the single-threaded evaluator. Caching helps, but correctness requires careful key design.

**Cache key**: the derivation path (`.drv` content hash) captures all build inputs deterministically. **Validation**: store the output hash and verify it matches the current store content on cache hit, since builds can be non-reproducible.

```cpp
struct IFDCacheEntry {
    StorePath derivationPath;  // Content-addressed key
    StorePath outputPath;
    HashCode outputHash;       // For validation
    SerializedValue evalResult;
};

Value lookupIFD(Derivation& drv) {
    auto entry = cache.get(drv.path);
    if (entry && store.validPath(entry->outputPath) 
              && store.hashPath(entry->outputPath) == entry->outputHash) {
        return deserialize(entry->evalResult);
    }
    // Rebuild and update cache...
}
```

**Impurity implications**: IFD results depend on builder behavior which may vary. Content-addressed derivations (CA-derivations) help because the output hash becomes the cache key's basis, making results self-validating.

## builtins.import caching must include content hashes for non-store paths

The cache key design depends on path type:

- **Store paths** (`/nix/store/...`): path alone suffices—content is immutable by definition
- **Regular paths** (`./file.nix`): must include content hash; files can change between evaluations
- **Lookup paths** (`<nixpkgs>`): must include NIX_PATH value and resolved path's content hash

**Recommended hybrid approach** following Bazel's pattern:

```cpp
struct ImportCacheKey {
    HashCode contentHash;      // File content
    HashCode argsHash;         // For parameterized imports
    HashCode nixpathHash;      // Environment context for <name> lookups
};

// Fast validation using mtime+size, rehash only if changed
struct FileHashCache {
    std::unordered_map<Path, std::tuple<time_t, size_t, HashCode>> entries;
    
    HashCode getHash(Path p) {
        auto [mtime, size, hash] = entries[p];
        struct stat st;
        stat(p, &st);
        if (st.st_mtime == mtime && st.st_size == size) return hash;
        hash = computeHash(p);
        entries[p] = {st.st_mtime, st.st_size, hash};
        return hash;
    }
};
```

The Tweag `builtins.cachedImport` RFC proposes requiring pure evaluation mode and serializable arguments, which simplifies correctness guarantees at the cost of flexibility.

## Serialization should use deterministic CBOR with functions excluded

For cross-evaluation persistence, **RFC 8949 §4.2 deterministic CBOR** provides the best combination of compactness, speed, and guaranteed reproducibility. Key requirements:

- **Attribute set keys sorted** lexicographically by UTF-8 bytes
- **Shortest integer encoding** (preferred serialization)
- **Floating-point canonicalization**: use smallest precision preserving the value exactly, normalize NaN representations

CBOR custom tags can represent Nix-specific types:

```cbor
39([                              // Tag 39: contextual string
  "result /nix/store/abc...",     // Content
  [["drv", "xyz.drv", ["out"]]]   // String context
])
```

**Functions cannot be meaningfully serialized** for cross-evaluation caching. Lambdas capture lexical environments that may contain thunks, other functions, or references to builtins. Instead, mark values containing functions as non-cacheable:

```cpp
CacheResult serialize(Value& v) {
    if (containsFunction(v)) {
        return CacheResult::NonCacheable("contains_functions");
    }
    return CacheResult::Serialized(toCBOR(v));
}
```

String contexts require serialization alongside string content—they track derivation dependencies essential for build correctness. A deduplication table at the serialization root avoids repeating the same derivation paths across many strings.

## Implementation summary for Nix's C++ codebase

| Challenge | Recommendation | Key Change |
|-----------|---------------|------------|
| GC pointer reuse | `GC_general_register_disappearing_link` | Weak cache keys |
| Cycle hashing | De Bruijn back-references | Ancestry stack during hash |
| Closed-term opt | Hash-cons closed Expr nodes | Intern table per Expr type |
| Error positions | Side table (Expr*, Path) → PosIdx | Out-of-band storage |
| With scope hashing | Ordered with-set pointer list | Include explicit/implicit distinction |
| Thunk hashing | Hash (code ptr, env ptr), never force | Consistent identity hash |
| Early cutoff | Track forced thunks + result hashes | EvalTrace structure |
| IFD caching | Derivation hash key + output validation | Content verification on hit |
| Import caching | Content hash for regular paths, path for store | Hybrid mtime fast-path |
| Serialization | Deterministic CBOR, exclude functions | RFC 8949 §4.2 encoding |

These solutions address the identified gaps while maintaining compatibility with Nix's existing architecture. The disappearing-links approach integrates cleanly with Boehm GC, hash-consing builds on the existing `PosIdx` separation, and CBOR serialization can leverage NAR format's proven determinism principles.
```

## Kagi

```text
# Round 2: Critique and Updated Plan for Nix Evaluator Memoization

## Executive Summary

The updated plan is significantly improved, with good coverage of architectural decisions and state-of-the-art references. However, several critical implementation details need refinement:

1. **Boehm GC cycle limitation** — Finalizers won't work for cyclic structures
2. **Hash-consing timing** — Should happen during GC promotion, not at allocation
3. **Two-level caching conflation** — Identity-based (intra-eval) vs content-based (cross-eval) need clearer separation
4. **`with` scope chain hashing** — More complex than the current fix suggests

---

## Critical Issues Identified

### Issue 1: Boehm GC Cycles and Finalizers

The plan proposes using `GC_register_finalizer` to clean up cache entries when keys are collected.  However, Boehm GC has a critical limitation:

> "Cycles involving one or more finalizable objects are **never finalized**." [^3]

**Problem scenario**: If a cached `Value*` result contains a reference back to its thunk key (common in recursive structures), the finalizer will never run, causing memory leaks.

**Recommended fix**: Use **disappearing links** (`GC_general_register_disappearing_link`) instead of finalizers for the primary cleanup mechanism:

```cpp
// When inserting into cache:
void MemoCache::insert(ThunkKey key, Value* result) {
    cache[key] = result;
    // Null the cache entry when the key's thunk is collected
    GC_general_register_disappearing_link(
        reinterpret_cast<void**>(&cache[key]), 
        key.thunk_ptr);
}

Value* MemoCache::lookup(ThunkKey key) {
    auto it = cache.find(key);
    if (it != cache.end() && it->second != nullptr) {
        return it->second;  // Still alive
    }
    return nullptr;  // Collected or not found
}
```

Disappearing links don't have the cycle problem because they don't prevent collection—they just null the pointer when collection happens.

**Additionally**: Use periodic cache sweeps to remove nulled entries, rather than relying on finalizers.

---

### Issue 2: Hash-Consing Timing

The plan proposes AST hash-consing but doesn't specify **when** to hash-cons. Appel's key insight (1993) is that hash-consing at every allocation is expensive: [^2]

> "Only records that survive a garbage collection are 'hash-consed,' thus avoiding the cost of a table lookup for short-lived records."

**For Nix**: Most thunks are short-lived (forced once, then discarded). Hash-consing every `Expr` allocation adds overhead that may not pay off.

**Recommended approach**: Two-tier strategy:

1. **Immediate interning** for `Expr` nodes (AST is long-lived, parsed once)
2. **Deferred interning** for `Value`/thunk results (only intern survivors)

For the deferred approach, integrate with Boehm GC's promotion:

```cpp
// Hook into GC promotion (if possible) or use explicit "tenure" points
void tenureValue(Value* v) {
    if (v->generation < TENURE_THRESHOLD) return;
    
    // Now worth hash-consing
    auto key = computeValueKey(v);
    auto it = valueInternTable.find(key);
    if (it != valueInternTable.end()) {
        // Deduplicate: redirect to existing value
        *v = *it->second;  // Copy content
    } else {
        valueInternTable[key] = v;
    }
}
```

---

### Issue 3: Two-Level Caching Conflation

The plan correctly identifies that intra-eval and cross-eval caching have different requirements , but the implementation still uses a single `StructuralHash` (SHA256) for both. [^1]

**The problem**: SHA256 hashing is expensive. For intra-eval caching, **pointer identity is sufficient and much faster**.

**Recommended architecture** (following `stable-memo`'s approach [^4]):

```cpp
// Level 1: Identity-based (intra-eval)
// Key: (Expr*, Env*, generation_counter)
// Fast O(1) lookup, no hashing of content
struct IdentityKey {
    Expr* expr;
    Env* env;
    uint64_t generation;  // Prevents stale pointer reuse
};
std::unordered_map<IdentityKey, Value*> identityCache;

// Level 2: Content-based (cross-eval, persistent)
// Key: SHA256(expr_content, env_content)
// Expensive but portable
std::unordered_map<StructuralHash, SerializedValue> contentCache;
```

**Lookup order**:
1. Check identity cache first (fast)
2. On miss, check content cache (slower, but survives across evals)
3. On miss, evaluate and populate both caches

---

### Issue 4: `with` Scope Chain Hashing

The plan identifies that `with`-bound variables need their name hashed , but the fix is incomplete. The issue is deeper:

```nix
with a; with b; x
```

The lookup for `x` depends on the **ordered chain** of `with` scopes, not just the variable name. The current fix adds the symbol name, but doesn't capture the scope chain structure.

**From the codebase answers** [^1]:

> "The `with` lookup depends on the **ordered chain** of enclosing `with` expressions, not just the variable name. This is why nested `with` expressions need their scope chain hashed."

**Recommended fix**: Hash the entire `with` chain structure:

```cpp
void hashExprVar(HashSink& sink, ExprVar* var) {
    if (var->fromWith) {
        feedSymbol(sink, var->name);
        
        // Hash the with chain structure
        ExprWith* w = var->fromWith;
        uint32_t chainDepth = 0;
        while (w) {
            chainDepth++;
            // Hash each with's expression identity
            feedExprHash(sink, w->attrs);
            w = w->parentWith;
        }
        feedUInt32(sink, chainDepth);
    } else {
        // Lexically bound: De Bruijn indices suffice
        feedUInt32(sink, var->level);
        feedUInt32(sink, var->displ);
    }
}
```

---

### Issue 5: Generation Counters vs. Finalizers

The plan lists both generation counters and finalizers as solutions to GC pointer reuse , but they serve **different purposes**:

| Mechanism | Purpose | When to Use |
|-----------|---------|-------------|
| Generation counters | **Detect** stale pointers | Cache lookup validation |
| Finalizers/Disappearing links | **Clean up** stale entries | Cache maintenance |

**Both are needed**:

```cpp
struct MemoKey {
    Value* ptr;
    uint64_t generation;
    
    bool operator==(const MemoKey& o) const {
        return ptr == o.ptr && generation == o.generation;
    }
};

// On allocation:
Value* allocValue() {
    Value* v = GC_MALLOC(sizeof(Value));
    v->generation = nextGeneration++;
    return v;
}

// On cache insert:
void insert(Value* thunk, Value* result) {
    MemoKey key{thunk, thunk->generation};
    cache[key] = result;
    
    // Also register disappearing link for cleanup
    GC_general_register_disappearing_link(
        reinterpret_cast<void**>(&cache[key]), thunk);
}

// On cache lookup:
Value* lookup(Value* thunk) {
    MemoKey key{thunk, thunk->generation};
    auto it = cache.find(key);
    if (it != cache.end() && it->second != nullptr) {
        return it->second;
    }
    return nullptr;
}
```

---

## Updated Implementation Plan

### Phase 1: Correctness (Blocking)

| Task | Priority | Notes |
|------|----------|-------|
| Fix `with`-bound variable hash | **Blocking** | Add symbol name AND scope chain structure |
| Add cycle detection to hashing | **Blocking** | Use visited set, return `CYCLE_MARKER` |
| Add cycle detection to serialization | **Blocking** | Track visited set, throw on cycle |
| Add generation counters to `Value` | **High** | Modify `allocValue()` |
| Use disappearing links (not finalizers) | **High** | Avoids cycle problem |
| Add periodic cache sweep | **Medium** | Remove nulled entries |
| Mark `unsafeGetAttrPos` impure | **Medium** | Add `ImpureReason::PositionDependent` |

### Phase 2: Performance (High Impact)

| Task | Priority | Notes |
|------|----------|-------|
| **Implement two-level caching** | **High** | Identity (fast) + Content (portable) |
| Implement AST hash-consing | **High** | Intern `Expr` nodes at parse time |
| Defer value interning to tenure | **Medium** | Only hash-cons long-lived values |
| Implement closed-term optimization | **Medium** | Wrap substituted closed terms |

### Phase 3: Architecture (Should Do)

| Task | Priority | Notes |
|------|----------|-------|
| Split cacheability predicates | **High** | `isIdentityCacheable()` vs `isContentCacheable()` |
| Implement early cutoff | **Medium** | Stop propagation when result unchanged |
| Add cache statistics/profiling | **Medium** | Measure hit rates, overhead |

### Phase 4: Cross-Eval (Nice to Have)

| Task | Priority | Notes |
|------|----------|-------|
| Make `ExprParseFile` hashable | **Medium** | Hash by `path.fingerprint()` |
| Add LMDB backend | **Medium** | Fast persistent KV store |
| Wire EvalInputs fingerprint | **Low** | Include in persistent cache key |

---

## Questions for the Codebase Agent (Round 2)

### GC Integration

1. **Where is `GC_general_register_disappearing_link` available?** Is it already included in Nix's Boehm GC headers, or does it need to be added?

2. **Can we hook into Boehm GC's promotion events?** Is there a callback mechanism when objects are promoted from young to old generation? This would enable deferred hash-consing.

3. **What is the current `Value` struct layout?** Specifically, is there padding where a generation counter could fit without increasing struct size?

### Hashing Implementation

4. **How is `computeThunkStructuralHash` currently implemented?** What is its performance profile (time per call, allocation)?

5. **Is there a fast path for identity-based lookup?** Or does every cache check currently compute a full SHA256 hash?

6. **How are `with` expressions currently hashed?** Specifically, is `ExprWith::parentWith` included in the hash?

### Cache Architecture

7. **What is the current cache eviction policy?** Is there any LRU or size limit, or does the cache grow unbounded?

8. **How large does `thunkMemoCache` typically grow?** For a Nixpkgs evaluation, how many entries are typical?

9. **What is the cache hit rate in practice?** Are there statistics available from `NIX_SHOW_STATS`?

### AST Structure

10. **Is `Expr` allocation centralized?** The round-1 answers mention `Exprs::add()` — is this the only allocation point, or are there others?

11. **Are there any `Expr` nodes that are mutated after creation?** Hash-consing requires immutability.

12. **How are source positions currently associated with `Expr` nodes?** Are they stored inline or in a side table?

### `with` Semantics

13. **Can `with` expressions be nested arbitrarily deep?** Is there a practical limit?

14. **How does `ExprWith::prevWith` differ from `ExprWith::parentWith`?** The round-1 answers mention both but their relationship is unclear.

15. **When is `ExprVar::fromWith` set?** Is it during `bindVars()` or later?

### Performance

16. **What is the overhead of the current memoization?** Is there profiling data showing time spent in hash computation vs. cache lookup vs. actual evaluation?

17. **Are there known pathological cases?** Expressions that cause excessive cache misses or hash collisions?

18. **How does memoization interact with `--pure-eval`?** Are there additional optimizations possible in pure mode?

---

## Summary of Key Changes

| Original Plan | Recommended Change | Rationale |
|---------------|-------------------|-----------|
| Use finalizers for cleanup | Use disappearing links | Finalizers don't work with cycles |
| Single `StructuralHash` cache | Two-level (identity + content) | Identity is faster for intra-eval |
| Hash-cons at allocation | Hash-cons at tenure/promotion | Avoids overhead for short-lived objects |
| Add symbol name to `with` hash | Add full scope chain structure | Nested `with` depends on chain order |
| Generation counters OR finalizers | Generation counters AND disappearing links | They serve different purposes |

The most impactful changes are:
1. **Two-level caching** — will dramatically improve intra-eval performance
2. **Disappearing links** — fixes the GC cycle problem
3. **Full `with` chain hashing** — fixes correctness for nested `with`
```