# Prompt

This is a continuation of round-7.md.

Please read all responses. You should fetch and read the sources referenced to ensure accuracy. You should also list those sources. Then, update your plan where appropriate, being sure to include alternative designs or implementations and their tradeoffs, as well as additional references. Lastly, answer any questions posed by any of the responses in a new file, round-8-answers.md.

Since there are a lot of steps here, please make a todo list and use it to keep track of your finished work and remaining work as you proceed.

# Responses

## ChatGPT

```text
Nix Evaluator Memoization Infrastructure

Goal

Implement a robust memoization system for the Nix evaluator to enable:
	1.	Within-evaluation caching – Reuse results when the same thunk is forced multiple times during a single evaluation.
	2.	Cross-evaluation caching – Persist evaluation results to reuse across separate invocations of Nix, avoiding repeated work.
	3.	Cross-machine caching – Long-term goal to share cached results between machines, by ensuring cache keys are purely content-addressed and independent of local Nix store paths or machine-specific details.

Core requirements: The new design must prioritize correctness and portability. Cache keys should be content-based so that any two machines given identical Nix input produce the same keys (allowing future sharing). We accept that significant refactoring of Nix’s evaluator internals (AST processing, environment handling, thunk forcing mechanism, etc.) may be needed to achieve these goals given lessons learned since Nix’s original implementation.

Current status: An initial memoization prototype exists for within-evaluation reuse, but it has correctness bugs and architectural limitations. No persistent or cross-machine caching is implemented yet.

⸻

Codebase Verification (Round 5)

The following assumptions about the current Nix codebase were verified:

Verified Correct ✅

Assumption	Location	Status
Env has up, size, values[]	eval.hh:222-227	✅ Correct
EvalState has thunkMemoCache	eval.hh:1163	✅ Correct
EvalState has trylevel (tryEval depth)	eval.hh:481	✅ Correct
EvalState has exprHashCache, valueHashCache	eval.hh:1172,1184	✅ Correct
Value uses 3-bit type discrimination	value.hh:555-556	✅ Correct
SourceAccessor has fingerprint, getFingerprint(), hashPath()	source-accessor.hh:185,203,146	✅ Correct
Expression hash functions exist	expr-hash.cc, value-hash.cc, env-hash.cc	✅ Correct
Cycle detection in hashing implemented	All hash files	✅ Uses De Bruijn back-references
forceValue memoization with impurity tracking	eval-inline.hh:152-227	✅ Implemented (checks impureTokenCounter_)
Boehm GC is thread-safe (GC_THREADS defined)	eval-gc.hh:12	✅ Correct (parallel marking enabled)
Concurrent map data structure available	eval.hh:526-543	✅ Correct (boost::concurrent_flat_map used for caches)

Issues Found (Corrections Needed) ⚠️

Issue	Finding / Status	Impact
with-bound variable hashing bug	Confirmed in expr-hash.cc:332-341: when hashing an identifier introduced by a with statement, the code uses only a withDepth counter and ignores the variable’s name ￼. Two different names at the same depth produce the same hash, causing collisions where with {x=1; y=2;}; x and with {x=1; y=2;}; y hash identically but evaluate differently.	Blocking correctness bug – must be fixed to safely memoize results involving with.
GC callback hooks not used	The Boehm GC provides GC_set_start_callback and disappearing links for finalization, but Nix’s code does not use them ￼.	Cache entries are never invalidated on GC, allowing stale pointers to remain in caches (risking incorrect hits if memory is reused).
Lazy hashing vs AST mutation	Nix currently performs hashing lazily during evaluation, not as a distinct phase ￼. However, AST nodes are mutated during bindVars() (e.g. setting parentWith pointers), so hashing content too early is unsafe. This confirms the need for a separate hash computation phase after binding (as proposed in the plan).	Without a redesign, content hashes can be inconsistent or require re-computation, complicating memoization correctness.
Portability classification	In value-hash.cc:850-858, a comment mentions a “pointer fallback” for hashing non-portable values, but the code actually uses content-based hashing with structural env hashing. The comment is stale ￼.	Minor – documentation needs updating. It also highlights potential confusion in distinguishing truly portable values vs ones that embed environment pointers.
Name mismatch – impureToken	The field tracking impurities is actually named impureTokenCounter_, not impureToken ￼.	Minor naming discrepancy – plan/documentation should use correct name.
ExprWith prevWith type	The ExprWith::prevWith field is a uint32_t distance, not a pointer ￼ (the plan assumed distance correctly, but wording can be clarified).	Minor – no functional impact, just clarity.
Tagged pointer plan	A previous plan idea to repurpose pointer tag bits for caching metadata conflicts with Decision 14 (tagged pointers not reliably usable on all platforms) ￼.	Must avoid implementing that approach – we will use alternative means (e.g. global cache epoch) for generation counters.
No concurrency guard in forceValue	Nix’s evaluator is not thread-safe for parallel evaluation: it lacks black-holing (thunk state locking) and relies on the CLI being single-threaded ￼. A parallel evaluation mode is being developed externally.	Blocking for future parallel eval – Without changes, forcing the same thunk from multiple threads can duplicate work or even cause race conditions. We need to add locking (CAS or mutex) around thunk evaluation and make caches thread-safe.
Lazy Trees non-determinism	The experimental lazyTrees feature (Nix 2.4+) introduces random store paths (StorePath::random()), meaning expressions can evaluate to different results across runs if this feature is enabled (e.g. for fixed-output derivations without specified hash) ￼.	Determinism issue – We must disable or special-case caching when lazyTrees is active (otherwise cache keys would be non-reproducible). This is addressed by gating memoization behind a feature flag unless the nondeterminism is resolved.


⸻

Codebase Context

Key Data Structures

Structure	File	Description
Value	src/libexpr/include/nix/expr/value.hh	Runtime value (16 bytes on 64-bit). A tagged union with primary types: nNull, nBool, nInt, nFloat, nString, nPath, nAttrs, nList, nFunction, nThunk, nExternal. A thunk Value contains {Env*, Expr*} pointers (environment and expression to compute). A path contains {SourceAccessor*, string} (pointer to filesystem accessor and the path string).
Expr	src/libexpr/include/nix/expr/nixexpr.hh	Abstract syntax tree node (~30 subclasses). Key subclasses for variable binding: see ExprVar and ExprWith below.
Env	src/libexpr/include/nix/expr/eval.hh	Evaluation environment (closure context). Implemented as a linked list of frames via Env* up (parent pointer). Each frame contains an array Value* values[size] for bound variables in that scope ￼. Envs are created for each function application, let block, with, and rec expression.
EvalState	src/libexpr/include/nix/expr/eval.hh	Global evaluation state. Holds the symbol table, allocator, and caches. Notably contains thunkMemoCache (a hash map for memoization) and impureTokenCounter_ (global counter for impure operations) ￼. Also includes other caches like fileEvalCache (which uses boost::concurrent_flat_map for thread-safe import caching).
SourceAccessor	src/libutil/include/nix/util/source-accessor.hh	Abstraction for filesystem access (e.g. local disk, git archives, etc.). Nix Path values carry a pointer to a SourceAccessor. Important: Path equality and hashing consider the accessor – a path from different accessors (e.g. nix store vs local FS) are considered different even if the strings match.

ExprVar and ExprWith (Handling with scopes)

Proper hashing of with expressions is critical. The AST provides information to distinguish variables bound by a with:
	•	ExprVar (nixexpr.hh:227-261): Represents a variable reference. Key fields: Symbol name (interned variable name), and ExprWith* fromWith – this points to the enclosing with expression if the variable is unbound in the normal environment (otherwise nullptr). Additionally level and displ give the De Bruijn index (lexical depth) and position within its environment frame ￼.

struct ExprVar : Expr {
    PosIdx pos;
    Symbol name;
    ExprWith* fromWith;   // Non-null if this var comes from a `with` scope
    Level level;
    Displacement displ;
};


	•	ExprWith (nixexpr.hh:640-657): Represents a with <attrs>; <body> expression. It has pointers to its attribute set expression and body, plus two fields for linking nested with scopes: prevWith (a uint32_t distance to the previous outer with in the AST, set during binding) and parentWith (a pointer to the actual enclosing ExprWith, set during binding) ￼.

struct ExprWith : Expr {
    PosIdx pos;
    uint32_t prevWith;      // Distance to previous `with` (in static chain)
    Expr* attrs;            // The attribute set provided by this `with`
    Expr* body;             // The expression that can use those attrs
    ExprWith* parentWith;   // Pointer to enclosing `with` (if any)
};

Hashing insight: After variable binding (bindVars()), each ExprVar from a with has its fromWith pointer set to the corresponding ExprWith. We can thus identify a chain of active with scopes via the parentWith links on the ExprWith nodes ￼. A correct content hash must account for both the name of a with-provided variable and the chain of with scopes it is resolved from. (The current bug is due to using only a depth count.)

Value Memory Layout (Implications for Memoization)

On 64-bit systems, Nix uses a packed representation for Value for efficiency ￼. Each Value occupies 16 bytes (aligned to 16). The layout is essentially two 64-bit words (called Payload). The low 3 bits of the first word act as a primary tag (discriminator) indicating how to interpret the rest:
	•	If the discriminator is pdPairOfPointers (low bits ...011 for example), then the Value contains two pointers (packed into the two 64-bit slots) – this is used for thunks, functions, applications, and other composite structures ￼. For a thunk, these two pointers are Env* and Expr* (both tagged to use the low bits for type tag).
	•	Other discriminator values indicate other layouts (integers and booleans fit entirely in the 64-bit payload, strings and paths use one pointer to a context and one to data, etc.) ￼.

Memoization impact:
	•	There are no spare bits in the Value representation to store metadata like cache generation or durability flags – all low bits are used for type tags ￼. This means approaches like tagging pointer addresses with extra information are not feasible without major changes (hence the plan to use a separate global epoch for GC generation, and a side-table for durability/taint info).
	•	Thunks being a fixed small size (16 bytes) means we cannot directly attach large memoization data to the thunk object itself. Instead, caches must be external (e.g., a hash map).
	•	However, updating a thunk in place is possible (since we can mutate the Value payload). In theory, once a thunk is evaluated we could overwrite its payload to directly store the result (similar to GHC’s in-place update of thunks). Nix currently does not do this – it leaves the thunk and uses an external cache to avoid recomputation. We will continue with external caching for now (to preserve semantics of impure operations like trace), but this is a point where future refactoring could improve performance (see Phase 5).
	•	Global GC epoch: Because pointer addresses can be reused after garbage collection, any identity-based cache needs a mechanism to distinguish stale entries. We adopt a global epoch counter that increments on each GC cycle, and incorporate it into cache keys ￼. This way, a cached entry from before a GC will not match a new object allocated at the same address after GC.

Current Hashing Implementation (Status Quo)

Before detailing the new design, it’s useful to summarize how Nix currently computes hashes (for content-addressing derivations and value comparisons):
	•	Expression hashing (expr-hash.cc) – Computes a structural hash of the AST (using SHA-256), treating different variable bindings as different and normalizing variable names via De Bruijn indices for alpha-equivalence ￼. (This hashing is used for deduplication of build results, not for memoizing runtime values yet.)
	•	Value hashing (value-hash.cc) – Computes a hash of fully evaluated values. Primitive types (ints, bools, strings, etc.) hash to a digest of their content. Importantly, function closures and thunks are hashed by a combination of the hash of their code (exprHash) and the hash of their environment (envHash) ￼. This gives a content-based fingerprint of a closure.
	•	Environment hashing (env-hash.cc) – Hashes an Env (environment frame and its chain) by recursively hashing the parent env and each value in the current frame ￼. If a value is a thunk or function, its hash will incorporate its own environment, so this can recursively traverse lots of data.
	•	Thunk (closure) hash (thunk-hash.cc) – Essentially just combines the exprHash of the code with the envHash of the environment to produce a unique hash for the thunk’s content.

These hashes are computed on demand. In the current memoization prototype, when a thunk is about to be evaluated (forceValue), the code:
	1.	Computes the thunk’s hash (by calling computeThunkStructuralHash, which triggers the above hashing of expr and env).
	2.	Looks in thunkMemoCache (an std::unordered_map<Hash, Value*>) to see if a result is already cached for that hash.
	3.	If not found, it proceeds to evaluate the thunk. After evaluation, if no impure operations occurred during that evaluation, it inserts the result into the cache for that hash ￼.

Key invariants / assumptions in current design:
	1.	Laziness – Thunks may never be forced; evaluation is demand-driven. The caching must not force anything that wouldn’t have been forced anyway (to preserve laziness and termination characteristics).
	2.	Purity tracking – The impureTokenCounter_ is incremented whenever an operation with side-effects occurs (e.g., trace, reading an environment variable, currentTime primop). Cached results are only reused if no side-effects happened during their computation (ensuring referential transparency for cached results).
	3.	Path context – Paths carry a SourceAccessor and possibly a context of other paths (for derivations). Two paths that point to different files or are from different stores should not be considered equal. The hashing and equality of path values respects this by including the accessor and using content hashes of files.
	4.	Symbol interning – Variable names (for attrsets, etc.) are interned and represented as integers (Symbol) that are only meaningful within a single evaluation. This means we cannot naively treat a symbol ID as stable across runs (we’ll rely on actual strings for cross-run keys where needed).
	5.	Garbage Collection – The Boehm GC can relocate or collect values unpredictably. In particular, a pointer address can be reallocated to a new object after the old one is collected. This is why identity-based caches must incorporate generation info or use GC hooks to avoid false hits.

⸻

Issues to Address

From the above, the primary issues we must solve can be summarized:

Blocking
	•	Incorrect hashing of with-bound variables – Needs a fix so that the content hash distinguishes variables introduced by different with expressions ￼. This bug currently prevents any sound memoization in presence of with.
	•	No cache invalidation on GC – We need a mechanism (e.g., generation counter or automatic removal) to avoid returning stale pointers from the cache after GC frees the underlying value ￼ ￼.
	•	Potential infinite recursion in hashing – The environment and value hashing code does not guard against cyclical structures (e.g., recursive sets) at runtime ￼. This can lead to non-termination during hash computation. We must add cycle detection for value serialization and hashing (the AST hashing already uses back-references for recursion).
	•	Impure results caching – Currently, the impurity check is all-or-nothing (valueIsUncacheable() covers both “uncacheable within a single eval” and “uncacheable across evals” reasons) ￼. We need to refine this. Some operations (like trace) should allow reuse within the same evaluation (so we don’t recompute a value twice in memory), but should not be persisted across evaluations. We need a clearer classification of durability levels for cached values.
	•	Parallel evaluation safety – With the prospect of parallel Nix evaluation, the evaluator must handle concurrent forcing of thunks. Right now, nothing prevents two threads from evaluating the same thunk concurrently, which could produce duplicate work or inconsistent state. We consider adding black-holing (marking a thunk as “in progress” and having others wait or use the same result) as essential for thread-safe memoization. We also must ensure our caching data structures are thread-safe (leveraging boost::concurrent_flat_map or similar).
	•	Lazy Trees nondeterminism – As noted, we must treat the lazyTrees experimental feature as incompatible with content-based caching for now. This likely means disabling cross-evaluation caching (and possibly even within-eval caching for certain paths) when this feature is active, or marking results that involve StorePath::random() as “volatile.”

High Priority
	•	Cache key completeness – We need to incorporate missing pieces into the cache key:
	•	String context: Nix strings can carry a context (list of derivations they were built from). Two strings with identical characters but different contexts are not interchangeable. The cache key must include a representation of string context ￼.
	•	Floating-point canonicalization: Floating point values can have platform-dependent representations (especially NaNs and -0.0). We should canonicalize these (e.g., always treat all NaNs as a single canonical NaN bit pattern, and treat -0.0 as 0.0) before hashing ￼.
	•	Exception handling context: The result of tryEval (which captures evaluation errors) should not be confused with a normal evaluation. We should include the tryEval depth level in the key so that expressions inside a tryEval are considered distinct ￼.
	•	Evaluation epoch: As discussed, incorporate a generation/epoch counter to distinguish values created before vs. after a GC cycle (to prevent pointer reuse issues).
	•	Environment sensitivity – The current environment hashing can change based on evaluation order (if a thunk in an env is forced, its spot in the env changes from a thunk to a value, altering the hash). We plan to hash thunks by identity (address) rather than forcing them for the hash ￼. This will make the hash of an environment stable regardless of whether sub-thunks have been evaluated yet (order-independence).
	•	Serialization safety – Currently, serializing a value (for caching to disk) does not guard against cycles, potentially looping forever on a self-referential structure ￼. We need to implement cycle detection in the serializer as well.
	•	Closed-term optimization – Many Nix expressions are closed (they do not reference any free variables from an outer scope). Dolstra (2008) shows a massive performance gain by avoiding re-evaluating closed subexpressions in each use ￼. We should mark Expr nodes that are closed during binding, and later leverage that to speed up evaluation and caching (closed expressions can be cached aggressively since they depend on nothing external).
	•	Test coverage – We should add property-based tests (e.g., with RapidCheck) to ensure that evaluation with caching yields the same results as without caching, and that repeated evaluation yields consistent results (idempotency). In particular, create tests for tricky cases: the with bug scenario, nested with ordering, unsafeGetAttrPos usage, etc., to prevent regressions ￼ ￼.

Medium Priority
	•	Impure builtins marking – A few builtins that depend on external state are not properly marked as impure:
	•	unsafeGetAttrPos (to get source location of an attr) and __curPos (current file position) currently do not bump the impurity counter ￼ ￼. They should be treated similarly to trace (so their results aren’t cached across evals or perhaps even within eval, since they produce contextual data).
	•	We might introduce a finer-grained notion of “taint” on values for things like this (e.g., a bit on a value indicating it depends on source positions), but initially simply marking them impure is a straightforward fix.
	•	Taint propagation – Beyond impurity, some values carry hidden context that should not be ignored by caching:
	•	Values produced by unsafeDiscardStringContext (which strips the context from a string) should probably be treated as tainted – if you discard context, you might not want to cache that result as if it were pure, since it deliberately loses information. One approach is to tag such values (in a side table or extra field) so that the memoization system knows not to persist them.
	•	Similarly, values coming from builtins.trace (though we mark the evaluation impure to avoid caching them at all in current design) could be given a low-durability tag if we wanted to cache their result within the same eval.
	•	Stability of hash across machines – We must verify that, after applying all the above fixes (string context, float canonicalization, etc.), a given Nix expression will produce the same content hash on any machine of the same architecture. If not, we may consider including a machine identifier in the persistent cache key as a last resort (e.g. include the system string like x86_64-linux if floating-point or other platform-sensitive data is involved ￼). The goal is to avoid this by full normalization, but it’s a consideration.
	•	Documentation and stale comments – Ensure that we update documentation, both inline (comments in code that may have become outdated, e.g. the portability comment) and user-facing (Nix manual sections about caching, if any, and writing new docs for this feature).

Low Priority
	•	Current time / date – The currentTime primop is marked impure (so results won’t be cached across evals), but if we implement cross-eval caching, we might want to incorporate a timestamp into the durability checking so that even if someone accidentally persisted a value that depended on time, it could be invalidated after some TTL. For now, it’s enough that it’s treated as impure (never cached persistently).
	•	Order-dependent cache misses – Even with improvements, some cache misses will depend on evaluation order (e.g., if two thunks reference each other, who forces what first can affect caching). We will document these cases and possibly add metrics to understand their impact, rather than try to solve them completely (which would require a more radical change to evaluation strategy).

⸻

Architectural Decisions

(Summary of key design decisions guiding the solution, incorporating latest insights and trade-offs.)
	•	Decision 1: Three-phase AST processing – We will introduce a new phase after parsing and binding for computing hashes of AST nodes (bottom-up) before evaluation. This ensures that each AST node knows its structural hash (and for with nodes, their scope identity hash) prior to runtime. Rationale: This allows content-hash caching of thunks without needing to evaluate them and resolves the ordering issues with hashing with scopes. It does add ~5–10% overhead to parsing (see Q14 analysis) but is a one-time cost per file and is offset by the caching gains ￼ ￼.
	•	Decision 2: Content-addressed cache keys – The cache key for a thunk’s value will be primarily a content hash of that thunk (its code and its closure environment). We will not attempt any invalidation logic that isn’t based on content changes. In other words, if anything that contributes to a thunk’s computation changes (its AST, any data it reads, etc.), the content hash changes and it’s a cache miss. This “correctness-by-construction” approach avoids needing complex dependency invalidation logic beyond hashing ￼. Note: We will augment the key with some runtime-specific components (see Decision 3 and 4) to handle GC and tryEval.
	•	Decision 3: Composite cache key with generation – To handle GC and identity reuse, the cache key will actually be a tuple: (contentHash, evalGeneration). evalGeneration (a global epoch counter) increments every time a GC cycle begins ￼ ￼. We store the current epoch when a value is cached, and on lookup, we only match if the stored epoch equals the current epoch. This means after a GC, all old cache entries are effectively invalid (they won’t match new keys). This is a simple way to avoid use-after-free. It does mean we “lose” the cache after a GC cycle, but since GC runs periodically, we may mitigate this by using a two-tier cache (see Decision 6).
	•	Decision 4: Track tryEval context in keys – Include the current trylevel (try evaluation nesting depth) in the cache key structure ￼. This prevents a value computed inside a builtins.tryEval (which might produce an error value) from being reused outside of that context where an error would have propagated (or vice versa). It’s a small addition (32 bits in the key) for correctness in error-handling logic.
	•	Decision 5: Separate pure vs impure caching (durability levels) – Introduce the concept of durability for cached values, inspired by Salsa’s durability levels (HIGH, MEDIUM, LOW) ￼ ￼:
	•	HIGH durability: fully deterministic and pure results (e.g. a fixed pure function on literals, or a package set that depends only on immutable store paths). These can be cached across machines and persisted indefinitely.
	•	MEDIUM durability: results that are deterministic given the same filesystem/environment but might change if external inputs change (e.g. reading a local file, using an environment variable, etc.). These can be persisted on the same machine, but on another machine or after significant changes they should be treated as invalid. (We might implement medium durability as a time-based TTL or tie it to a content fingerprint of known inputs.)
	•	LOW durability: results that are inherently one-time or context-dependent (e.g. containing currentTime or trace output). These should not be persisted across evaluations at all – only within the running evaluation (or not cached at all). Effectively, LOW durability means “do not reuse in a different invocation.”
We will assign durability to results at evaluation time based on what operations were performed (e.g., any impure operation yields LOW, reading a non-store file yields MEDIUM, pure yields HIGH). This influences whether a result is written to the persistent cache and for how long it remains valid. (For now, we likely implement this simply as flags on whether to write to disk cache or not, and possibly a simple TTL for medium.)
	•	Decision 6: Two-level caching (L1/L2) – Use a two-tier cache design:
	•	Level 1 (L1): An in-memory identity cache keyed by pointer identity (and perhaps some lightweight hash) for within a single evaluation. This provides maximum speed (no hashing of large structures needed on lookup) and catches repeated forcing of the exact same thunk pointer.
	•	Level 2 (L2): A content-addressed cache (persisted on disk) keyed by the structural content hash (plus epoch, etc.) for across evaluations. This uses cryptographic hashing and serialization to allow reuse even after the process exits or on other machines.
The evaluator will first check L1 (fast path); on an L1 miss, it computes the content hash and checks L2 (disk); if that misses, it actually evaluates the thunk. On storing results: store in L2 (if durable enough) and optionally also keep in L1 for the remainder of the current run. This design follows the established pattern in build systems (e.g., Bazel’s Skyframe has in-memory vs disk caches). It also means that if the cache is cold, we pay the cost of hashing/serialization once (for the L2 lookup) and thereafter get O(1) pointer-based hits for duplicates in that run.
	•	Decision 7: Use existing concurrency primitives – Rather than inventing new concurrency structures, we will leverage boost::concurrent_flat_map (already used in Nix) for any new caches that need thread-safety ￼. This gives us lock-free reads and safe concurrent writes with internal sharding. For coordinating thunk evaluation, we will implement a simple black-holing: mark a thunk as “evaluating” when a thread starts forcing it, so if another thread attempts to force the same thunk, it will either wait or see that it’s already in progress. We can implement this by adding an atomic state flag to the thunk (e.g., repurposing one of the tag bits during evaluation or an external table of “in progress” thunks). This prevents duplicate work and ensures shared results in parallel mode.
	•	Decision 8: Experimental rollout – Guard the entire memoization system behind experimental feature flags until it’s proven stable. We will introduce flags such as:
	•	--experimental-features eval-memoization for enabling basic in-memory caching,
	•	--experimental-features eval-memoization-persist for enabling cross-evaluation persistent caching,
	•	--experimental-features eval-memoization-share for any future cross-machine sharing features.
This allows gradual adoption. We will also implement a shadow mode (optional): where caching is enabled and used internally, but the evaluator still computes results normally in parallel to verify correctness without relying on cached values, logging any mismatches. This helps build confidence in the feature in production.
	•	Decision 9: ARC for cache eviction – To manage the size of the in-memory cache (and potentially a memory-bound portion of the persistent cache), we plan to use an Adaptive Replacement Cache (ARC) strategy ￼ ￼. ARC offers scan-resistance and adapts between LRU (recency) and LFU (frequency) behavior. It was patented, but that patent has expired (IBM 2004 patent expired in 2020), so it is now safe to use. If any concerns remain, a simpler 2Q policy (two queues) could be used as a fallback. We will maintain two lists for cache entries (recent and frequent) and two “ghost” lists for recently evicted entries, adjusting the balance dynamically. This should optimize hit rates for typical Nix workloads (which often have a mix of many unique values and a small hot set of repeated values) ￼ ￼. We will also expose configuration for maximum cache size (entries or memory) and implement eviction when exceeded.
	•	Decision 10: Deterministic serialization (CBOR) – For storing values on disk (L2 cache), we will use a deterministic encoding. We lean towards Canonical CBOR (as per RFC 8949 §4.2) ￼ for its balance of human-readability and efficiency, and because Nix’s own binary cache (.nar format) could be excessive for this purpose. The serialization will ensure maps have sorted keys, numeric values use shortest form, and no randomness is introduced. Additionally, we will incorporate a string deduplication mechanism during serialization: since Nix values (especially attrsets) often repeat strings (e.g., attribute names, derivation outputs), we can embed a string table in the CBOR stream and refer back to earlier strings by index ￼ ￼. We’ll limit this table (e.g., 64k entries) and reset per serialization so memory use is bounded. This can drastically shrink cache files for large attrsets.
	•	Decision 11: No general function serialization – We will not attempt to serialize function closures for cross-machine use beyond their content hash. While it’s tempting to persist a function’s value, functions can close over large parts of the environment including impure values, and their behavior can’t be reconstructed just from a hash without re-evaluating their contents on the target machine. Instead, our approach caches the application of functions (thunk results). If we ever move Nix to a compiled bytecode or a wholly content-addressed execution (like the Dhall language does for expressions), this could be revisited, but for now functions will be treated as non-persistable values (cache key exists to avoid re-evaluating the same function within one run, but we won’t serialize the function’s result for other runs) ￼ ￼.
	•	Decision 12: Fine-grained dependency tracking (future) – We acknowledge that a purely content-hash approach may do more work than necessary in some incremental scenarios. For example, if a large attrset is cached and only a small part changes, content-hash will treat it as entirely different. In the future, we may incorporate ideas from incremental computing (like Adapton or build systems à la carte) to track dependencies of values on inputs (files, environment variables, function arguments) more directly and achieve early cutoff (stop re-evaluation when inputs haven’t changed) ￼ ￼. In this design, we outline an early-cutoff mechanism using EvalTrace to record input hashes, but this will likely come after basic caching is in place.
	•	Decision 13: Closed terms as cache isolants – When an expression is marked is_closed (no free variables), we can optimize evaluation by not cloning environments or doing needless work for each instantiation. The plan is to mark such expressions during bindVars() and use that information in two ways:
	1.	If a closed expression is a function, it means it doesn’t use the environment it’s defined in – we can potentially hoist it or share one instance of it across all calls (maximal laziness optimization).
	2.	For caching, a closed expression’s result is inherently HIGH durability (assuming no impurity), because it can only depend on its explicit inputs. We will leverage this to cache aggressively – e.g., a fixed closed subexpression might be evaluated once and reused everywhere, similar to CSE (common subexpression elimination) but at the Nix evaluation level.
This echoes Dolstra’s observation that without closed-term optimization, many evaluations blow up, whereas with it, they finish quickly ￼. We will implement this by adding a bool is_closed flag to Expr nodes and setting it true when appropriate during binding ￼ ￼.
	•	Decision 14: Position-independence mode – Nix evaluation currently carries source location data through values (for error reporting). This means that normally, evaluating the same expression in two different files yields structurally identical values that differ in their embedded position info. For content-addressed caching across runs, this is a problem. We will introduce an option (--position-independent-eval) to strip position info (or treat it as taint) during evaluation for caching purposes. This would make evaluation results fully deterministic w.r.t file content, at the cost of losing some error context. It will be off by default initially, but it’s useful for hermetic builds or caching in CI systems where sources are fixed.
	•	Decision 15: Integration with Nix store / binary cache (future) – Cross-machine sharing will likely leverage Nix’s existing store and binary cache infrastructure. One idea is to treat cached evaluation results as derivation outputs in a content-addressed store (NAR) – essentially, when we serialize a value, we could store it in the Nix store with a name derived from the hash. However, differences in architecture (for functions that might embed a platform or path) complicate this. More straightforwardly, we can set up a separate distributed key-value cache (similar to Bazel remote cache) where keys are the content hashes and values are the serialized results. The user could specify a remote cache (HTTP, S3, etc.). This is beyond the initial implementation, but the design choices above (content-hash keys and deterministic serialization) lay the groundwork for it.

⸻

Action Plan

We organize the implementation into phased milestones, focusing first on correctness and then on performance and broader scope. Each phase builds on the previous. Note: Experimental flags will guard these features until we complete the roadmap.

Phase 0: AST Normalization & Infrastructure (Foundational)

Goal: Establish a stable foundation for hashing by normalizing the AST after parsing, and lay groundwork for optimizations like closed-term detection.
	•	Implement three-phase AST processing – Split evaluation into: Parse → Bind → Compute Hashes → then Evaluate ￼ ￼. Introduce a new method Expr::computeHashes(SymbolTable&) that performs a post-order traversal of the AST:
	•	Compute child exprHash for each subnode.
	•	For ExprWith, compute a scopeIdentityHash that combines the hash of its attr set with the parent scope’s scopeIdentityHash ￼. This gives each with a stable identifier for its chain of surrounding with scopes.
	•	Compute each node’s exprHash by hashing a tag for the node type and the hashes of important fields (e.g. for ExprWith, combine a EXPR_WITH_TAG, the attrs->exprHash, and body->exprHash ￼).
	•	We will add fields to the Expr structure (or use a side table) to store exprHash. Storing 32 bytes (256 bits) per node is heavy (~32 MB per million AST nodes) ￼, but we will at least store it for nodes that are thunks or relevant to hashing (we can skip storing for literals). Alternatively, we compute on the fly and immediately intern (see next item), freeing the AST.
	•	Update parser entry points (parseExprFromString, evalFile, etc.) to call computeHashes() after bindVars() ￼ ￼.
	•	Introduce AST interning (hash-consing) – Using the computed exprHash, implement an intern table for AST nodes (likely keyed by exprHash plus node type). This allows deduplication of identical ASTs (particularly useful for large Nixpkgs evaluations where many package expressions are identical across the tree). When computeHashes() is done for the root of an AST, look it up in a global EvalState::exprInternTable. If an existing node with the same hash (and structurally equal by a deep compare, to avoid hash collision issues) is found, reuse it; otherwise, insert the new one ￼ ￼.
	•	Note: We use a monotonic arena for AST allocation, which doesn’t free memory. If we intern and discard duplicates, those duplicates remain allocated. Initially, we accept this memory overhead as limited (Dolstra noted a few MB wastage is okay) ￼. Later, we could switch to a different allocator or free duplicates explicitly.
	•	Fix with-bound variable hashing – With the above in place, address the with hashing bug:
	•	In the new computeHashes, ensure that when hashing an ExprVar that has fromWith != nullptr, we incorporate the symbol name (so x vs y differ) and something to represent the chain of with scopes. We have two options:
	1.	Eager approach: Use the parentWith->scopeIdentityHash (computed as part of the ExprWith hashing) as part of the hash for ExprVar that comes from a with. This means each variable’s hash implicitly includes the hash of the attr set that introduced it and any outer scopes.
	2.	Simpler fix: In the old hashing code, just add feedSymbol(sink, expr->name) for ExprVar when fromWith is set ￼. This ensures different names yield different hashes. This doesn’t fully account for scope ordering, but it handles the immediate bug. However, since we’re doing scopeIdentityHash, we will implement the full solution (the chain hashing) which covers ordering as well (e.g., with A; with B; x vs with B; with A; x will differ, because the chain hashes differ).
	•	Write a regression test for this: e.g., ensure the hash of with {x=1; y=2;}; x is different from with {x=1; y=2;}; y ￼.
	•	Mark closed terms in AST – Extend bindVars() to determine if an Expr is closed (no free variables from an outer scope) ￼. We can do this by tracking the maximum scope level referenced:
	•	After binding, an expression can be considered closed if none of its ExprVar references refer to a level above 0 (the current top level of that expression). We can compute this by adding a method like Expr::computeFreeVariableLevel() that returns 0 if closed or >0 for the outermost free variable level, then mark is_closed = true if the result is 0 ￼ ￼.
	•	Each lambda (ExprLambda) can mark its body closed if the body has no references to anything outside the lambda’s own arguments.
	•	This flag will be used later for optimization (Phase 2 and Phase 3).
	•	Environment hash stability – Modify env-hash.cc to avoid forcing thunks during hashing:
	•	When computing an environment’s hash, instead of forcing every value in the environment (which is what currently can happen, leading to evaluating thunks just to hash them), we will do:
	•	If the value is a thunk or function (not yet evaluated), hash it by a tuple of its Expr* pointer’s hash and its Env* pointer’s hash (basically its identity). We have exprHash from AST now, and we can assign or look up an Env ID or use the address (with epoch as safeguard). Essentially, treat the closure as an opaque identifiable object for hashing. This prevents us from accidentally evaluating it during hashing.
	•	If the value is already a concrete value (e.g., an int or a realized list), we hash its content normally.
	•	This change means the hash of an environment won’t change just because a thunk inside it got evaluated (since we hash thunks by identity). It does mean if we evaluate a thunk, its environment’s hash still reflects the thunk as a thunk (which is fine for content-addressed keys – we consider the content different if a thunk wasn’t forced; that’s acceptable because forcing it might expose more dependencies, so we treat forced vs not forced as different states).
	•	We must be careful with this: a thunk’s identity includes the pointer which is only valid in one run, so this environment hash is not stable across runs for persistent caching. However, for cross-run caching we will rely on value hashes after forcing. This tweak is mainly to avoid order-dependent misses within one run. In cross-run keys, we will ensure the thunks are forced or considered differently.
	•	GC hooks for epoch – Install a callback with Boehm GC to increment our global evalGeneration on each collection cycle ￼. This uses GC_set_start_callback. Also use GC_register_disappearing_link for entries we put in the identity cache: when we insert a pointer in the cache, register its address so that if the object is collected, the GC will clear or notify us, allowing cleanup ￼. (We will implement cache cleanup in Phase 2, but the hooks are set up here.)
	•	Testing Phase 0: Ensure that after these changes, basic evaluation still works (no change in semantics). Add tests specifically for:
	•	AST hashing: two alpha-equivalent lambdas yield the same exprHash, etc.
	•	with hashing: the bug is fixed.
	•	Closed-term detection: some known closed and non-closed expressions are correctly flagged (we might expose this via a debugging interface or just infer via stats in Phase 2).

Phase 1: Correctness Fixes and Cache Key Completion (Blocking issues)

Goal: Fix all known correctness bugs and finalize the structure of cache keys so that caching will not change evaluation results. Enable within-evaluation caching under an experimental flag (without persistence) and ensure it doesn’t break anything.
	•	Include string context in cache keys – Modify value-hash.cc to incorporate the string context list into the hash of string values ￼ ￼. This means sorting the context (list of derivation outputs) deterministically and feeding each element into the hash. We will also adjust the equality check for strings such that context matters (if not already). Add a test where two strings differing only by context are treated as different keys.
	•	Canonicalize floats for hashing – Implement a helper (canonicalizeFloat(double)) to normalize floats ￼ ￼. Use it in value-hash.cc so that prior to hashing a float, we do:
	•	If NaN, use a fixed bit pattern (e.g., IEEE 754 quiet NaN 0x7FF8…).
	•	If negative zero, treat as positive zero.
	•	Otherwise, use the 64-bit representation as-is (which will be identical on all IEEE754-compliant platforms for the same numeric value).
	•	Also ensure our serialization uses the same function so that a float’s binary form in cache is canonical.
	•	Track tryEval depth – Add a field in the cache key for tryEvalDepth (just copy state.trylevel, which Nix already increments in prim_tryEval) ￼ ￼. This requires threading trylevel to the hashing code or storing it in EvalState accessible when computing a cache lookup key. We can store state.trylevel in a thread-local or global during hashing for simplicity.
	•	Generation (epoch) in key – Finalize the use of the global evalGeneration counter in the key. We will combine the 256-bit content hash with an 8-bit or 16-bit generation. Implementation details:
	•	Define a struct CacheKey { Hash256 contentHash; uint16_t epoch; uint8_t tryEvalDepth; } (sizes chosen to pack nicely).
	•	Always set epoch = currentEvalGeneration on creating a key.
	•	When matching keys in the L1 or L2 cache, require both contentHash and epoch to match (and tryEvalDepth).
	•	The epoch will be incremented via GC callback as set in Phase 0. If it overflows (0xFFFF -> 0x0000), we may choose to invalidate/clear caches to avoid confusion (since an overflow means old entries might appear to have a matching epoch again).
	•	Impurity and durability handling – Refine forceValue logic:
	•	Before forcing a thunk, record the current impureTokenCounter_. After forcing, if it changed, mark the result as impure. Currently, Nix simply doesn’t insert into cache if impure. We will continue that for persistent cache (don’t persist impure results). For the in-memory cache, we have a choice:
	•	We could still cache it within the run (so the second time you force the same impure thunk, you reuse the value). However, this can change semantics for things like trace (which the user might expect to run each time). In Nix’s current prototype, they do not cache if impure (so that e.g. a trace in a thunk will execute each time it’s forced). We will keep that behavior for now: no caching of impure computations even within one eval.
	•	But we will still tag the result with a durability: e.g. if impureTokenCounter_ changed, assign LOW durability to that result (meaning it won’t be persisted and it might not be reused).
	•	Mark unsafeGetAttrPos and __curPos as impure operations: i.e., call state.markImpure(ImpureReason::PositionDependent) when they are invoked ￼ ￼. This will ensure any thunk using them is not cached cross-evaluation. (Within one eval, since it’s marked impure, as per above, we skip caching entirely.)
	•	Implement basic durability assignment: After evaluating a thunk, we determine durability:
	•	If impureTokenCounter_ changed => LOW durability (don’t cache beyond this eval).
	•	Else if the result contains any non-store Paths or other environmental data => MEDIUM durability (cacheable on disk, but might need invalidation if those inputs change; for now just mark as such).
	•	Else => HIGH durability (pure, cache freely).
	•	We can refine these rules in Phase 3 with a proper version vector. For now, record durability in a side table mapping Value* to Durability (HIGH/MEDIUM/LOW) ￼ ￼. Use std::unordered_map initially (but see concurrency in Phase 2).
	•	Ensure that if a cached value is reused, its durability is downgraded appropriately if combined with other inputs (e.g., if a HIGH durability value and a LOW one are used to construct something, the result is LOW – we will propagate min durability).
	•	This durability info won’t be heavily used until we implement persistence and invalidation logic in Phase 3, but we gather it now.
	•	Concurrency: black-holing and thread safety – Implement a basic mechanism to prevent duplicate evaluation:
	•	Add an atomic flag or small enum to Value for thunks (maybe reuse the existing Value::type bits temporarily or maintain a separate ConcurrentMap<Value*, bool> for “in progress”). For instance, we could use a compare-and-swap on the thunkMemoCache entry: when a thread goes to evaluate a thunk, it inserts a placeholder indicating evaluation in progress. Another thread checking the cache or trying to force the same thunk can see this and wait (or in our case of single-process, maybe spin until done, or better use a condition variable).
	•	Simpler: Use a mutex per thunk address via a sharded locking scheme. But since we already have the ability to use a concurrent map, an approach is:
	•	Create a global std::unordered_set<Value*> evaluatingSet protected by a mutex. On entering forceValue, lock and check if the thunk is in the set; if yes, wait on a condition for it to finish; if no, add it and unlock. After evaluation, lock, remove it, notify waiters.
	•	This is not super fine-grained but thunks are usually quick. In Phase 2 we might refine it.
	•	Switch thunkMemoCache (L1) to boost::concurrent_flat_map<CacheKey, Value*> or wrap our existing map with a read-write lock. Since our L1 cache will be keyed by pointer (plus epoch), and boost::concurrent_flat_map expects keys that are copyable, we might use a wrapper struct containing the pointer and epoch.
	•	For now, mark this feature behind the experimental flag for parallel eval (since mainline Nix isn’t parallel yet). But implementing now ensures our design is thread-safe.
	•	Testing concurrency (if possible) – We might simulate parallel eval in tests by creating multiple threads manually forcing shared thunks. Ensure that with black-holing, the thunk is only computed once and both threads get the same result.
	•	Memoization feature flag and CLI – Add a config and CLI option for eval memoization. E.g., in Nix config experimental-features = nix-command eval-cache, or similar (depending on how features are toggled). For Phase 1, the default is off, so unless enabled, Nix behaves exactly as before. When enabled (especially in non-parallel mode), we expect no change in output, only performance (and those only in repeated computations scenarios).
	•	Add property-based tests – Integrate RapidCheck (or another property test framework):
	•	Soundness: For arbitrary (generated) Nix expressions, evaluate with caching and without, assert the results are equal ￼ ￼.
	•	Idempotency: Evaluate with caching twice in a row, assert the results are the same and that the second time does not produce different effects or errors.
	•	We will likely stub out or avoid truly impure things in these tests (or allow that if an expression produces a side effect, we ignore it as long as results match).
	•	Statistics collection – Begin collecting metrics to ensure caching isn’t misbehaving:
	•	Extend NIX_SHOW_STATS output to include: number of cache hits, misses, evictions, and perhaps average hash computation time. Already counters nrThunkMemoHits and nrThunkMemoMisses exist ￼; we will keep those updated. Add counters for L1 vs L2 hits once L2 is introduced.
	•	Count how many values are cached with HIGH/MEDIUM/LOW durability to see distribution (if trivial to add).

At the end of Phase 1, we should have a functional in-memory memoization that is correct for all known edge cases when enabled. It might not provide big speedups yet (due to overhead of hashing every thunk and the bug fixes overhead), but it sets the stage. We will verify that enabling it doesn’t change evaluation outputs (modulo performance and the intended effect of not repeating work).

Phase 2: Performance and In-Memory Caching Improvements

Goal: Make the caching efficient (reduce overheads, optimize hit rates) and solidify the in-memory caching layer (L1). This phase focuses on within-evaluation performance and preparing the structure needed for persistent caching.
	•	Optimize cache data structures (L1) – Replace the simple thunkMemoCache with a two-level structure:
	•	Identity cache (L1): Key = IdentityKey (we can define this as the raw pointer of the thunk or a combination of Env* and Expr* pointers, plus maybe the tryEvalDepth). In fact, since an Expr* can be shared by multiple thunks (same code but different environment), using just pointers might collide if the same code is evaluated in different contexts. To avoid confusion with content cache, here we truly want an identity of the thunk instance. The simplest is the Value* address of the thunk itself (which is unique per evaluation for that thunk).
	•	We will use boost::concurrent_flat_map<IdentityKey, Value*> for thread-safe reads/writes ￼ ￼. This gives lock-free reads. For insertion, we can use try_emplace_and_cvisit pattern as seen in fileEvalCache usage ￼ ￼ to atomically insert and possibly get a handle to update.
	•	The identity cache will store both ongoing evaluations (to implement black-holing) and completed results. E.g., we can insert a dummy entry when a thunk evaluation begins, and then update it with the result when done.
	•	We will also integrate an eviction policy for L1: since L1 lives only for the duration of an eval (which typically doesn’t run out of memory for a single evaluation of Nixpkgs, but could for a long-lived REPL or daemon), we still want a safeguard. Use an LRU or ARC structure on top of the map. Perhaps maintain doubly-linked lists for T1 and T2 (for ARC) with ghost lists B1, B2 ￼ ￼. Implement adjustments on cache hit/miss as per ARC algorithm. This is complex to do fully lock-free; we might simplify initially to a coarse-grained lock around eviction operations (since evictions happen relatively infrequently when thresholds exceed).
	•	If ARC is too much initially, implement an LRU with a simple list and a max size, noting in documentation the limitations (thrashing on large scans).
	•	Decide on default size: e.g., 100k entries or 500MB memory, whichever hit first (configurable via nix.conf: eval-cache-max-entries, eval-cache-max-size) ￼ ￼.
	•	Content cache (in-memory part): Even before we have disk persistence, we can maintain a second map in memory keyed by the content hash (256-bit) to Value*. Why do this if we have identity cache? Two use cases:
	1.	If the same equivalent thunk is created twice (different pointer but same content), we could reuse the result after the first is computed. This situation can happen if you have duplicated code; however, due to our AST interning from Phase 0, identical AST should map to the same Expr* now, but if they occur in different environments, we might still consider them same content if those environments are equal. That’s a rare scenario though (exactly equal env content).
	2.	It sets up the logic for persistent cache: essentially, this in-memory content cache will later be backed by disk.
	•	We might delay implementing a full in-memory content cache until Phase 3, as its real benefit comes with persistence. Within one eval, it’s usually cheaper to just compute once per pointer (identity cache) because identical content thunks typically share structure and thus share sub-thunks rather than appear twice independently. However, to be thorough, we can include it.
	•	If implemented, use another concurrent_flat_map<ContentHash, Value*>. The content hash here should incorporate tryEvalDepth and epoch for safety (so effectively the full CacheKey minus pointer). But if epoch changed (GC happened), those entries could be invalid – we might flush content cache on GC same as identity.
	•	The content cache in memory would effectively do what persistent cache does but without writing to disk. We might skip this and go directly to disk cache implementation, to avoid maintaining two similar structures.
	•	Integrate closed-term fast path – Use the is_closed flags from Phase 0:
	•	When evaluating an application of a function (ExprApp forcing a thunk that is a function), if the function’s AST is marked is_closed and it has HIGH durability (no impure actions inside), we can memoize the function’s result the first time and reuse that exact value for subsequent calls with the same arguments. This is essentially memoizing pure function calls. However, implementing a full call memoization might be beyond our current scope, but note that closed-term optimization classically means if a function doesn’t use its environment, you don’t need to clone the environment for each call – in Nix, that translates to not needing to create a new Env for the function, you can reuse one.
	•	Concretely, we can skip environment copying for closed functions: currently, calling a function creates a new Env frame and inserts default closures, etc. If the function is closed, it doesn’t need anything from outside, so the evaluation of the body can in theory ignore the outer Env. We could optimize that by evaluating the body once and caching it. However, since functions can be called with different arguments, we can’t reuse the result unless arguments are also the same (which might rarely be the case except for nullary functions).
	•	Instead, the main benefit is avoiding repeated work for constant expressions (e.g., large static lists or attrsets defined at top level). Those will be forced only once and then cached.
	•	We will ensure that if an Expr is closed and not a function (i.e., it’s a constant or attrset), when it’s first forced we mark it somehow so that subsequent forces just reuse the same value (maybe treat it as a memoized constant).
	•	This might already naturally happen if it’s a thunk with no env dependencies: its hash will be unique and we’ll cache it once in L1 and/or L2. So the system already covers this without special-casing, as long as the hashing is content-based. We will verify that is_closed indeed correlates with fewer misses in cache.
	•	Avoid repeated hashing – Profiling the prototype showed hashing itself can be costly (dominant cost on large thunks is computing SHA-256 over possibly big data structures) ￼ ￼. To mitigate:
	•	Use xxHash3 (a very fast non-cryptographic hash) for the in-memory identity cache. Specifically, we can hash pointers and maybe some constant values with xxHash for quick indexing in L1 (although L1 primarily uses pointer keys, which might not need hashing at all if we use pointer as key directly in the unordered_map, which uses a trivial hash).
	•	For content hashing (L2), we stick to SHA-256 (for collision resistance, especially if we ever share across machines).
	•	Cache intermediate hashes: with Phase 0, each AST node now has exprHash. We should use those cached exprHash values instead of recomputing AST hashes for every thunk each time. The thunk’s exprHash can be retrieved quickly from its Expr*. Similarly, we can compute an envHash once per environment structure and cache it. Nix already had exprHashCache and valueHashCache in EvalState – we will use/extend those. Possibly, we add an envHashCache too if not present (or repurpose valueHashCache for environment values).
	•	Never hash the same thunk twice in one eval: ensure that when a thunk is forced and we compute its content hash for caching, we store that mapping so if by some chance later we need the hash again, we reuse it. This might be a micro-optimization, but given content hashing is heavy, it helps.
	•	Our earlier AST interning also helps because identical sub-expressions share exprHash, saving time on hashing large repeated AST portions.
	•	Partial serialization for large values – If a thunk returns a huge value (say a list of 100k elements), computing its hash for caching is expensive. But if we are only doing in-memory caching, we might decide not to hash very large values at all and just treat them as uncacheable (or only identity-cache them). Because hashing a 100k-element structure might take more time than recomputing it, depending on what it is. However, for cross-eval caching it’s needed.
	•	A compromise: perhaps skip content-hash for values above a certain size for now (just don’t cache them persistently, still allow identity cache).
	•	We can measure sizes and times with stats first, then decide if needed.
	•	Memory pressure heuristics – Optionally, integrate with the GC to drop cache entries if memory is low. This could be an advanced step; for now, rely on the eviction policy (ARC) and user-configured size limits to manage memory. We note it for documentation.
	•	Finish test & debug instrumentation – Extend NIX_SHOW_STATS=1 to print: cacheHits, cacheMisses, cacheHitRate, maybe L2HitRate (when Phase 3 is done), and number of entries in cache at end of eval. This will help users tune the cache size and see effectiveness.
	•	Documentation – Write up a section in Nix’s manual (if appropriate, or as a design doc for now) explaining how to enable the feature, its current limitations (no cross-machine yet, etc.), and how to interpret stats.

By the end of Phase 2, we expect:
	•	Within one evaluation, repeated computations are significantly faster (e.g., forcing the same expensive thunk twice should show a near-100% speed improvement the second time).
	•	The overhead for computing hashes when nothing is reused is ideally minimized (through caching of hashes, etc.), so that in worst-case scenarios (no repeats) the user doesn’t see a large slowdown (our goal is <5% overhead on evaluation when caching yields no hits).
	•	Memory usage is under control via eviction.
	•	Concurrency: if enabled, the evaluator can run in parallel mode (if that branch is merged in Nix, we’re prepared for it).

Phase 3: Persistent Cross-Evaluation Caching & Durability

Goal: Introduce a disk-backed cache (L2) to persist results between evaluations, and implement the durability/invalidation logic to ensure correctness across runs and across different input states.
	•	LMDB backing store – Integrate an embedded database (Lightning Memory-Mapped DB) for storing the content-addressed cache. LMDB offers very fast read performance and is used in similar contexts (Mozilla’s rkv, etc.) ￼ ￼. Plan:
	•	On Nix startup (or first use of eval cache), open an LMDB environment in $XDG_CACHE_HOME/nix/eval-cache (for example). Set a reasonable size (e.g., 1 GiB default, configurable) ￼ ￼.
	•	Use one database within it for the mapping of CacheKey.contentHash -> serialized Value.
	•	Since LMDB is single-writer, and Nix evaluations are typically short, we can get away with committing each insertion immediately (but see below for daemon mode).
	•	Wrap LMDB access in a small C++ module for ease (or use an existing C++ binding if available).
	•	Serialization to CBOR – Implement serialization of Nix Values to a binary form:
	•	Use a streaming approach (e.g., a CBORSerializer class). Serialize primitives directly (ints, bools, strings with context). For collections (list, attrset), serialize an opening tag or array length then each element.
	•	Maintain a string deduplication table as discussed to reduce size (especially for attr names which repeat a lot in Nixpkgs) ￼ ￼.
	•	Pay attention to special values: cycles (shouldn’t appear in fully evaluated normal Nix values except via self-referential attrsets – we need to break those with markers), functions (we decided not to serialize, so what do we do if a cached value includes a function? We likely skip caching it persistently if it contains a function closure, or we serialize a placeholder that indicates “function” and perhaps store its hash for identity, but not attempt to restore it).
	•	We might choose to not store any value that isn’t fully serializable. For now, treat functions and thunks as non-persistable: mark those results as not cached to disk (or only store their hash but not the actual value).
	•	The output of serialization is a byte array along with maybe a top-level description of the type.
	•	Ensure the serialization is deterministic (we will test by serializing the same value twice and comparing bytes).
	•	Possibly use the existing Nix .nar serialization for parts of it (nar is deterministic and handles derivations etc.), but .nar is oriented toward store paths.
	•	For our use, CBOR we can fine-tune and perhaps directly memory-map when reading.
	•	Deserialization – The inverse of above: given bytes, reconstruct a Nix Value. This requires allocating Values and possibly creating Env frames, etc., if the value is complex:
	•	We have to be careful if the value contains references to store paths or strings with context – those need to be properly interned or linked to current EvalState. For example, if a cached value is a derivation output path, on deserialization we should ideally ensure that if the store path isn’t present on this machine or has changed, we handle that (though if content hash matches, the store path contents should be same – but cross-machine caches might send store paths that don’t exist locally).
	•	This gets into cache invalidation: if a value includes a store path that the target machine doesn’t have, using that cached value might be invalid. We might include the store path fingerprint in the key to avoid that (the EvalInputs::fingerprint() covers current store state in a sense).
	•	Implement deserialization initially assuming same machine, same Nix version, etc.
	•	Integrate L2 lookup on evaluation:
	•	Modify forceValue logic: After computing the content hash (CacheKey), if not found in L1:
	•	Look up in LMDB (L2) by the content hash. If found:
	•	Deserialize the stored result into a Value. This value may point to newly allocated data not yet managed by GC (we must allocate it with GC allocation so Boehm can see it).
	•	Before returning it, consider its durability: if the cached entry was stored with a durability flag, check if it’s still valid. For example, if it was MEDIUM and the EvalInputs (Nix version, system, etc.) changed, we might invalidate. We incorporate EvalInputs::fingerprint() into the key or as part of the stored metadata to help with this ￼ ￼. For now, assume if content hash matches, it’s valid (since content hash would include any environmental differences if properly done).
	•	Insert the resulting Value into L1 cache (so any further uses in this eval use the same pointer).
	•	Return it, skipping the actual computation.
	•	If not found in L2, proceed to evaluate as before.
	•	After evaluation, if the value’s durability is HIGH or MEDIUM, serialize it and store in LMDB:
	•	Use an LMDB write transaction. LMDB ensures atomic commit – either the entry is stored or not, no half measures ￼ ￼. This also helps if multiple processes write (which they shouldn’t concurrently, but multiple sequential processes can append).
	•	We might need to prune the LMDB if it grows too large (see eviction).
	•	If durability is LOW, do not store it to disk (skip).
	•	Durability and invalidation rules – Implement the logic fully:
	•	We’ve been marking durability on values. Now use it:
	•	For HIGH: no invalidation needed except content changes. Cache forever (or until manual clear).
	•	For MEDIUM: we need to define what could invalidate these. Likely, the EvalInputs::fingerprint (which includes Nixpkgs version, flake lock, etc.) should be part of the key for these. Actually, EvalInputs::fingerprint() already covers things like the current system, NIX_PATH, flake locks, etc ￼ ￼. We should incorporate that into the persistent cache key for any value that depends on those (which is arguably any evaluation).
	•	Possibly incorporate a hash of EvalInputs into the cache key globally for persistent cache. This means caches are only reused if the Nix “evaluation context” is the same. This might be too coarse (would treat different machines or user environments as completely separate caches, which might be fine initially).
	•	Alternatively, store it as metadata with each entry and check on retrieval.
	•	For values that depend on specific files (like an import of a file outside the store), we can record the file’s content hash when caching (we have path.fingerprint() via SourceAccessor for imports). We should extend EvalState to record InputDependencies during evaluation (like a list of “read file X with hash H” events, similar to how build systems track input files) ￼ ￼. In caching a result, store those dependencies. On retrieval, compare the current hash of those files to the stored ones – if any differ, invalidate (skip using cache). This is the early cutoff mechanism. Implementation:
	•	Add a vector EvalState::inputDeps where primops like readFile push the file path and its hash when called ￼. Similarly for any other external reads (getEnv could record the env var name and value).
	•	After evaluating a thunk, if we want to store it, attach this dependency list to the cache entry (LMDB can store it as part of the value or a separate DB for metadata).
	•	On lookup from cache, if an entry has dependencies, verify each (e.g., re-hash the file to see if it matches; this is only for MEDIUM durability entries likely).
	•	This prevents using stale results if files changed.
	•	Propagation: Ensure that when combining values, durability propagates as the minimum (if one part is LOW, whole is LOW, etc.) ￼ ￼. This was conceptually done, but now in code: e.g., implement functions or macros to combine durability easily.
	•	Primops special cases: Implement durability rules for builtins:
	•	trace: we mark impure (so LOW, not persisted) and in Phase 1 we decided not to cache it at all, so fine.
	•	fetchurl without fixed hash: should be LOW (non-deterministic content). If with hash, it’s actually deterministic (HIGH) because content is verified – but Nix typically imports those as store paths, which are content-addressed (HIGH).
	•	getEnv: yields LOW (depends on process environment).
	•	readFile: if path is in store (content-addressed), treat as HIGH (or at least Medium but practically content-addressed store paths we can consider immutable = HIGH) ￼. If path is regular filesystem, Medium (depends on that file’s content – track its hash).
	•	import: importing a nix file – that essentially triggers parsing and eval of another file. Nix already caches imports via fileEvalCache for the process. For cross-eval, we want to cache those results too. We will handle that by caching the top-level expressions as well; essentially, the entire result of an import can be cached like any other thunk result. But we need to incorporate the file’s fingerprint into the key (which our content hash of the AST + env will do if the file content changes, the AST hash changes).
	•	For extra safety, we ensure that ExprParseFile AST nodes (which represent an import) are hashed using the file’s content fingerprint, not the pointer address ￼. Plan: when hashing an ExprParseFile (if not already handled), use SourceAccessor->fingerprint(path) which gives a content hash of the file ￼ ￼. This way, the expression hash for an import is content-addressed (the Nix code in that file).
	•	Then the normal thunk caching will apply to the imported file’s contents.
	•	builtins.derivation (if we consider import-from-derivation or fixed-output derivations): these produce derivation outputs that might not be deterministic unless fixed-output. We won’t dive into that here; we can mark IFD results as Medium (depending on derivation output existing in store).
	•	Other builtins like addErrorContext should propagate durability from the inner expression (we add that in primops: after evaluating the context and the value, mark the result’s durability = value’s durability) ￼ ￼.
	•	We will also propagate position-independence: if --position-independent-eval is on, we might strip out any positional info values (or treat them as impure/tainted so they don’t get cached).
	•	Env pointer instability – With content cache we rely on content hash rather than pointer addresses for cross-run, so environment pointer differences between runs don’t matter as long as the content is the same. However, one subtlety: if an environment contains references to values that were not forced in one run but forced in another, the content hashes might diverge unless we always treat thunks uniformly. Our earlier fix (hashing thunks by identity for envHash) means if a thunk wasn’t forced, the envHash includes its identity (pointer) which is not stable across runs. This suggests that for persistent cache, we cannot rely on envHash that includes raw pointers. Instead, we should ensure that any thunk present in an environment either gets forced (and hence replaced by its value hash) before forming the persistent key, or we find another scheme.
	•	A possible solution is evaluate all thunks to weak head normal form for caching: i.e., for the purpose of computing the content hash of a top-level thunk, recursively force any thunks in its environment (essentially, ensure we have normal form). But that breaks laziness and could do a lot of work unnecessarily.
	•	Alternatively, we include in the content hash the full description of any thunks in the env: since each thunk has an exprHash and an envHash of its own, we could incorporate those. This is essentially what we already do by combining expr and env hashes. But if envHash of a sub-thunk had pointer identities, that’s not stable.
	•	Perhaps the correct approach: treat the content hash of a thunk (to be persisted) as if all sub-thunks are content-hashed as well. That’s a deep content hash that would require a fixed point computation or forcing everything.
	•	We might settle for a practical approach: declare that caching a value with unevaluated sub-thunks is not safe for cross-run reuse unless those thunks are closed and context-insensitive (HIGH durable anyway).
	•	Concretely: for Phase 3, we might decide to not persist values that still contain thunks or functions in them (i.e., we only store fully evaluated data structures). Since a top-level evaluation of a Nix expression typically either returns a concrete value (like a derivation or attrset or list of derivations) or we don’t cache it. That could be acceptable: e.g., caching the result of nix-env -qa which is a big attrset of attrsets (some might still have thunks for lazy attributes…). Actually, Nixpkgs evaluation does intentionally leave many things lazy for performance. Caching a partially lazy structure might be tricky. This is a known challenge.
	•	To not overcomplicate: we will persist such structures but note that the cache key might not perfectly capture the differences if some thunks forced vs not. We will document that order-dependent issue as a limitation.
	•	Eviction and size management (L2) – Implement policies for the disk cache:
	•	If the cache file (LMDB) grows beyond a limit (e.g., >1 GiB), start evicting least-recently used or lowest durability entries. LMDB doesn’t have built-in eviction, so we might keep metadata of usage. Perhaps maintain a separate LRU list for persistent entries in memory (only updated on access within the program run).
	•	Or simpler: provide a command nix eval-cache gc that the user (or nix-collect-garbage) can run to prune the cache according to some policy (e.g., drop all Medium entries older than 7 days, etc.).
	•	Also integrate with nix-collect-garbage by adding an option to clear the eval cache (by default, perhaps do so unless --keep-eval-cache is specified) ￼ ￼.
	•	Possibly use SQLite for storing metadata (timestamps, usage counts) if LMDB usage tracking is burdensome – but that adds complexity. We can also store metadata in LMDB by using a separate key space or encoding.
	•	CLI and UI – Provide user-facing ways to inspect cache:
	•	nix eval-cache status or similar to print size, number of entries, hit/miss stats (if persistent daemon or so).
	•	As mentioned, nix eval-cache gc, nix eval-cache clear to manage it.
	•	Ensure these commands are documented and behind the experimental flag.

After Phase 3, we will have a working persistent evaluation cache. This means running a command like nix eval nixpkgs.foo twice in a row will skip the actual evaluation the second time (if nothing relevant changed), retrieving the result from disk almost instantly. We will test scenarios:
	•	Re-running nix-env -qa (which queries all packages) to see the speedup.
	•	Changing a file that is imported and ensuring that invalidates the cache for things depending on it.
	•	Flipping an impure setting (like an env var) and seeing that cached results that depended on the old value are not reused.
	•	Ensure that cross-user or cross-machine doesn’t accidentally reuse things it shouldn’t (we might simulate cross-machine by changing the EvalInputs.fingerprint and see that those entries don’t match).
	•	Check that memory usage is reasonable (not retaining too many values in RAM after eval – maybe drop L1 at end of eval or when EvalState is destroyed).
	•	Performance: measure overhead of writing to LMDB (should be minor compared to eval, given LMDB is very fast for our sizes).

Phase 4: Cross-Machine Sharing and Adoption

Goal: Outline steps to leverage the content-addressed cache for multiple machines and ensure the feature is robustly integrated and adopted.
	•	Content-addressable store integration – Investigate storing evaluation results in the Nix binary cache or substitutable store:
	•	One idea: treat the serialized eval results as store “derivations” outputs. Possibly create a fake derivation whose output is the eval result, where the output name or hash is the content hash. Then existing Nix binary cache infra could distribute it. This is complex and not a priority for now.
	•	Instead, set up a dedicated network cache for eval results. Possibly as simple as a directory that can be rsynced or an HTTP endpoint:
	•	For example, if an organization has a CI that pre-evaluates Nixpkgs, it could upload the eval cache to a shared location. Developers could download it to speed up their evals.
	•	We can implement a command nix eval-cache export and nix eval-cache import to dump the entire cache or portions (maybe just the keys and values as CBOR) for synchronization between machines. This is an explicit step rather than automatic.
	•	In the future, this could be automated with a narinfo-like system for eval results, but that’s speculative.
	•	Position independence mode – Finalize and test the --position-independent-eval flag:
	•	When on, any operation that would generate a value containing a source position (like __trace or evaluation error traces) should either exclude those or mark the value as LOW durability so it’s not persisted. Essentially, this mode sacrifices some debugging info for cacheability. We ensure that when this flag is set, our content hashes truly don’t incorporate any ephemeral positions.
	•	Write tests: Evaluate an expression with and without position-independence and ensure that with position-independence, the results have no references to the original file paths or line numbers (maybe check string outputs of derivations or traces).
	•	Tuning and thresholding – Based on usage, adjust default limits:
	•	If the cache hit rate is high and memory is abundant, maybe increase default size.
	•	If some workloads see slowdown due to hashing, consider enabling/disabling parts by heuristics (e.g. a thunk that took <1ms to compute might not be worth hashing – but detecting that requires measuring execution time which is intrusive).
	•	Possibly allow users to mark certain functions or expressions as “do not cache” via an annotation, if they know it’s not beneficial (not in this phase, but an idea).
	•	Gradual rollout plan:
	•	Initially, keep the feature experimental. Encourage adventurous users to enable it and provide feedback.
	•	Define graduation criteria for making it default: e.g., after 6 months of testing, no known correctness issues, performance overhead in worst-case is negligible, significant wins in best-case, and maybe caching of evaluations is used in a few major projects successfully ￼ ￼.
	•	Possibly do a “shadow rollout” in the Nix daemon or CLI where it computes results with caching but also without and compares, logging any discrepancies – to catch any corner-case differences in the wild without affecting users.
	•	Community and docs – Write a comprehensive section in Nix manual about evaluator caching:
	•	How it works conceptually (so users understand when a change will or won’t invalidate something).
	•	How to enable/disable, configure sizes.
	•	How to diagnose issues (maybe an env var to turn on verbose logging of cache decisions).
	•	Encourage reporting any expression that behaves differently with caching (that would be a bug).
	•	Future improvements (beyond Phase 4) – Though not implemented yet, outline to the community what might come:
	•	More fine-grained invalidation (maybe tying into flake system or file watching to auto-drop stale cache).
	•	Possibly share caches with builds (some overlap with derivation outputs? Though eval is separate).
	•	Potential integration with language servers (where Nix evaluation caching could greatly speed up IDE features, as rust-analyzer uses Salsa – mention that as a direction).
	•	And cross-machine fully automated sharing if a design is settled.

By the end of Phase 4, the evaluator memoization should be a usable, albeit experimental, feature. It addresses the original goals: within-eval caching is done by Phase 2, cross-eval persistence by Phase 3, and while cross-machine sharing is not fully automatic, the content addressing groundwork is laid such that it’s feasible via external means.

Phase 5: Advanced Optimizations and Refactoring (Future Ideas)

(These are potential improvements that go beyond the initial implementation – they might be implemented after the core functionality is proven, or only if needed.)
	•	In-place Thunk Update – Revisit the idea of updating thunks with their results directly (like Haskell). This could eliminate the need for an identity cache entirely: once a thunk is evaluated, all future forces naturally see it as a value. This is tricky in Nix due to side-effect semantics (we wouldn’t want to skip a trace the second time, as discussed). But perhaps for pure thunks we could do it. This would require careful changes to how Value is structured (maybe introducing an Indirection Value type that points to the result).
	•	Generational caching / tenuring – We incorporated Appel’s idea of deferring interning (hash-consing) until values survive some time ￼. We could apply similar to caching: maybe don’t immediately persist every computed value, but only those that appear to be used frequently or survive many evaluations. This avoids cluttering the disk cache with one-off computations.
	•	Fine-grained impurity tokens – We started on this by splitting reasons and tracking per resource. We can go further by maintaining separate counters for, say, file system reads, network access, etc., so that a change in one doesn’t invalidate caches dependent on another. This ties into context-dependent invalidation (coeffects) ￼ ￼. For example, if only network operations occurred, maybe you only invalidate things that care about network, not everything.
	•	Version vectors for durability – A more sophisticated durability mechanism (from Salsa) uses a vector of version numbers for each durability level ￼ ￼. E.g., one component increments on any LOW event, another on MEDIUM, etc. Then a cached query has an associated durability requirement and only compares relevant parts. We decided this was too invasive initially ￼, but in future, implementing this could allow caching results even when, say, time changed, if they didn’t depend on time.
	•	Query-level (sub-eval) caching – Currently, our cache unit is a thunk (which corresponds roughly to an expression in the source, often one that is a function body or a let binding). We could consider higher-level caching: e.g., caching an entire derivation or even a fixed-point of the NixOS module system. This is more algorithmic and might be better handled by restructuring Nix evaluation into an explicit dependency graph. If that happens (maybe in Nix 3?), our memoization infrastructure should adapt to store those results.
	•	IDE integration and incremental eval – With position-independent evaluation and caching, one can imagine an IDE or long-running service that holds the state of an evaluation and only re-evaluates parts that changed (like incremental compilation). This would require Nix to expose an interface to reuse the cache between runs programmatically. Our work could be the foundation of such a Nix interpreter mode.
	•	Alternate GC or memory model – If performance of Boehm GC becomes an issue (with lots of cached values hanging around), we might explore a custom allocator for cached values (possibly reference-counted or arena-based since cached values might be long-lived). Also, if we go with in-place thunk updates, a copying collector or generational GC might be more suitable to handle updated graph structures. These are deep changes and would need careful consideration.

⸻

Will This Meet the Goals?

Finally, we evaluate our progress toward the original goals:

Goal	Status after Phase 3-4	Notes/Blockers
Within-evaluation caching (no repeated work in one run)	✅ Achieved (Phases 0–2) – The evaluator will reuse results on multiple force attempts of the same thunk in a single evaluation.	Ensure black-holing is in place for concurrency. Some expressions with side-effects (trace, etc.) are intentionally not cached to preserve semantics, but that’s acceptable.
Cross-evaluation caching (persist results between runs)	⚠️ Partial (Phase 3 implemented the core persistent cache).	The basic mechanism works: repeated invocations reuse previous results, vastly speeding up operations like re-evaluating Nixpkgs. However, some edge cases (changes in external files, environment, etc.) rely on our invalidation heuristics which need real-world testing. Also, by Phase 3 we do not yet automatically distribute the cache across machines.
Cross-machine caching (share cache across systems)	❌ Not fully realized – foundations are laid (content-addressed keys, deterministic serialization) by Phase 3.	To actually share between machines, additional infrastructure is needed (e.g., a way to exchange cache entries, or using Nix binary cache). The current plan doesn’t implement a network sync. That said, if two machines independently evaluate the same expressions, they will end up with the same keys, so one could manually copy the LMDB file or we could later integrate with the binary cache. This remains future work beyond Phase 4.

In summary, the planned memoization infrastructure should drastically improve Nix evaluation performance for large evaluations and repeated runs, without compromising correctness. By addressing known bugs and carefully designing the cache keys and invalidation, we aim to make the evaluator’s behavior with caching as transparent (i.e. same results) as without caching. The biggest payoff will be seen in scenarios like: querying large portions of Nixpkgs, evaluating NixOS modules repeatedly, or interactive use of Nix (REPLs or IDE integration) where results can be reused. Cross-machine sharing, while not immediate, is on the horizon given the content-addressed approach.

⸻

Sources

Core References
	•	Dolstra, E. “Maximal Laziness in Build Languages” (LDTA 2008) – Describes Nix’s existing lazy evaluation optimizations (function short-circuiting, closed term optimization) and motivation for memoization.
	•	Nix Source Code (particularly eval.cc, eval.hh, eval-inline.hh, expr-hash.cc, value-hash.cc) – Implementation details of current Nix evaluator and hashing.
	•	Determinate Systems blog: “Parallel Nix Evaluation” (2023) – Shows an external effort to parallelize Nix evaluation, which our design considers for thread safety.
	•	Bazel Remote Caching – Documentation on how Bazel separates an in-memory (local) cache and a remote persistent cache, analogous to our L1/L2 separation.

Incremental and Build Systems
	•	Build Systems à la Carte (ICFP 2018) – A framework for understanding incremental build semantics, which influenced our durability concept.
	•	Salsa (Rust analyzer incremental engine) – Blog posts on “Durable memoization” (2023) and “Three architectures for responsive IDEs” (2020) – provided inspiration for durability levels and position-independent evaluation ￼.
	•	Adapton (PLDI 2014) and Nominal Adapton (OOPSLA 2015) – Research on demand-driven incremental computation with fine-grained dependency tracking, which underlies our early cutoff idea.

Hashing and Equivalence
	•	Maziarz et al. “Hashing Modulo Alpha-Equivalence” (PLDI 2021) – Advanced techniques for hashing ASTs modulo renaming. Nix uses a simpler De Bruijn approach, but this paper ensures we handle with scopes correctly.
	•	Unison language content-addressed code – Demonstrates a practical system where code (including functions) is content-addressed and cached, highlighting the difficulty of hashing function closures with environment (they handle cycles via hash mix-ins).

GC and Memoization
	•	GHC StableName / stable-memo libraries – Haskell’s approach to identity-based caching under GC (we drew parallels in using a generation number).
	•	Boehm GC documentation on finalization – Noting pitfalls with finalizers and why we prefer disappearing links (to avoid finalizer cycles) ￼ ￼.
	•	Guile “whippet” branch – Guile Scheme’s attempt at a new GC removed weak tables due to unpredictability, which informed our cautious approach to GC interactions with caches.

Performance and Caching
	•	Adaptive Replacement Cache (ARC) paper (IBM Research, 2003) – Introduced ARC policy we use for cache eviction ￼ ￼.
	•	LMDB benchmarks – Illustrate LMDB’s speed (especially read performance) vs alternatives, justifying its use for persistent cache.
	•	Mozilla’s RKV (LMDB wrapper) analysis – Gave us confidence in using LMDB over something like SQLite for this use-case ￼.

(This concludes the updated plan. We have integrated answers from prior rounds and incorporated allowances for deeper architectural changes as discussed. The next steps would involve prototyping Phase 0 and Phase 1 changes, running the test suite, and iterating on any issues discovered.)
```

## Claude

```markdown
# Nix evaluator memoization: concrete implementation guidance

The Nix evaluator's parallel execution model requires carefully coordinated memoization to avoid duplicate work while maintaining thread safety. This guide provides concrete implementation patterns drawn from GHC's runtime, Boost's concurrent containers, Salsa's incremental computation framework, and Determinate Systems' existing parallel evaluation work.

## Boost concurrent_flat_map delivers fine-grained concurrency

Boost's `concurrent_flat_map` uses **two-level synchronization**: a container-level read-write mutex for structural operations like rehashing, and per-group spinlocks for the 15-slot buckets that actually hold elements. This architecture enables concurrent readers and writers to operate on different parts of the map simultaneously, with contention only when threads access the same bucket group.

The **try_emplace_and_cvisit pattern** from Nix's existing `fileEvalCache` provides the canonical memoization approach:

```cpp
fileEvalCache->try_emplace_and_cvisit(
    resolvedPath,
    nullptr,  // placeholder for new entries
    [&](auto& entry) {  // called on successful insertion (f1)
        Value* v = allocValue();
        v->mkThunk(&baseEnv, expr);
        entry.second = v;
        result = v;
    },
    [&](const auto& entry) {  // called if element exists (f2)
        result = entry.second;
    }
);
```

Three critical constraints apply: visitation functions **cannot call operations on the same container** (detected in debug builds via reentrancy checks), references obtained during visitation become **invalid after the visitation ends** due to potential rehashing, and heavy computation should be avoided inside visitation blocks to prevent holding group locks. The pattern for expensive operations is to copy data out, compute outside the visitation, then update with a second visit.

Memory ordering guarantees are strong: any operation that inserts or modifies an element **synchronizes-with** subsequent visitation functions on that element, establishing happens-before relationships that ensure memory writes are visible.

## Black-holing prevents duplicate evaluation across threads

GHC's black-holing mechanism translates directly to Nix's thunk lifecycle. The key insight is using an **atomic type field** to coordinate concurrent thunk forcing through compare-and-swap operations:

```cpp
enum ThunkState { tThunk, tPending, tAwaited, tFailed, tValue };

void forceValue(Value* v) {
    auto type = v->type.load(std::memory_order_acquire);
    if (type == tThunk) {
        if (v->type.compare_exchange_strong(type, tPending)) {
            // Won race - this thread evaluates the thunk
            try {
                auto result = evaluate(v->thunk);
                v->payload = result;
                auto prev = v->type.exchange(tValue, std::memory_order_release);
                if (prev == tAwaited) wakeWaiters(v);
            } catch (...) {
                v->exception = std::current_exception();
                v->type.store(tFailed, std::memory_order_release);
                wakeWaiters(v);
            }
        } else {
            // Lost race - another thread is evaluating
            goto wait;
        }
    }
wait:
    if (type == tPending || type == tAwaited) {
        registerAndWait(v);  // block until evaluation completes
    }
    if (v->type.load() == tFailed) std::rethrow_exception(v->exception);
}
```

GHC distinguishes **eager black-holing** (mark thunk immediately on entry) from **lazy black-holing** (mark only when thread yields to scheduler). Eager black-holing has ~1-2% overhead but eliminates the window for duplicate evaluation—essential for parallel code. Lazy black-holing checks blackhole status during GC stack walks, catching most cases with lower per-thunk cost.

The **tAwaited state** is crucial: when a second thread attempts to force a tPending thunk, it atomically transitions the state to tAwaited and registers itself for notification. The evaluating thread checks after completion whether state is tAwaited (indicating waiters exist) and wakes them. Storing `std::exception_ptr` in the tFailed state ensures all threads forcing a failing thunk receive the same exception.

## Closed-term detection enables safe caching

Only **closed terms**—expressions with no free variables—can be cached context-independently. Eelco Dolstra's maximal laziness optimization wraps substituted terms to avoid unnecessary traversal:

```python
def subst(subs, expr):
    if expr is Var(x):
        if (x, e) in subs:
            return Closed(e)  # mark as closed, prevent re-traversal
        return expr
    if expr is Closed(e):
        return expr  # don't descend into closed terms
    # continue recursively for other node types
```

This optimization reduced Nix's GCC build evaluation from **2.8 seconds to 0.2 seconds** in original benchmarks. The `Closed` wrapper acts as a barrier: the evaluator unwraps it only at evaluation time, and the wrapped expression is never re-traversed during substitution.

For efficient free variable analysis with De Bruijn indices, a term is closed if all indices are less than the current binding depth—achievable in O(n) with a single traversal:

```python
def is_closed_at_depth(expr, depth):
    match expr:
        case Index(n): return n < depth
        case Lambda(body): return is_closed_at_depth(body, depth + 1)
        case App(e1, e2): return is_closed_at_depth(e1, depth) and is_closed_at_depth(e2, depth)
```

Annotating AST nodes with precomputed `is_closed` flags during construction enables O(1) cache eligibility checks during evaluation.

## Determinate Systems' parallel evaluation architecture

The parallel Nix evaluator achieves **3-4x speedups** through several key design decisions. The symbol table was rewritten to use a **lock-free contiguous arena** where symbol IDs are offsets into memory whose base never moves, eliminating lock contention on the highly-accessed symbol table. File parse and eval caches use read-write locks via `SharedSync` wrappers.

Work distribution uses a **randomized task queue** rather than work-stealing:

```cpp
// Executor randomizes work queue to reduce probability of concurrent 
// tasks blocking on common dependencies (e.g., libfoo and libfoo_variant 
// both needing stdenv)
```

This design acknowledges that Nix's evaluation graph has heavy shared dependencies (stdenv, common libraries), making random scheduling more effective than topological ordering at reducing blocking.

**Boehm GC integration** remains challenging. Stop-the-world collection pauses all threads, making `GC_INITIAL_HEAP_SIZE=8G` essential for parallel workloads. Recent work enabled parallel marking (`GC_allow_register_threads()`) with increased mark stack sizes, improving `nix search nixpkgs` from 24.3s to 18.9s. The main blocker for upstream adoption is **disabled blackhole detection**—infinite recursion causes hangs instead of errors under parallel evaluation.

| Operation | Single-threaded | Parallel (12 threads) | Speedup |
|-----------|-----------------|----------------------|---------|
| `nix flake show` | 23.70s | 5.77s | **4.1x** |
| `nix search nixpkgs` | 11.82s | 3.88s | **3.0x** |
| IFD test | 50s | 10s | **5.0x** |

## Deterministic CBOR serialization for cache persistence

RFC 8949 §4.2 specifies deterministic encoding rules: integers use minimum bytes (no leading zeros), floating-point uses shortest representation, and **map keys are sorted by bytewise lexicographic order** of their deterministic encodings. The definite-length encoding (not indefinite) ensures reproducible serialization.

For cyclic structures, CBOR tags 28 (shareable) and 29 (sharedref) enable reference sharing:

```
// Cyclic structure: data = []; data[0] = data;
d8 1c 81 d8 1d 00  // tag 28 marks array, tag 29(0) references it
```

However, library support for tags 28/29 is limited. For Nix values, consider explicit ID-based references instead:

```cddl
nix-value-or-ref = nix-value / #6.1000(uint)  // custom tag for reference by ID
```

**libcbor** is recommended for C++ implementation—RFC 8949 compliant, MIT licensed, used by Yubico's libfido2 and AWS's C SDK. A deterministic encoding wrapper is needed on top since libcbor doesn't enforce canonical ordering automatically.

For lambda serialization, don't serialize functions directly. Instead, serialize `(function-hash, captured-environment)` pairs, following Haskell's distributed-closure approach where function IDs point to statically-compiled code.

## Incremental hashing with Merkle-style propagation

**XXH3** (64-bit or 128-bit) provides the best balance of speed and collision resistance for non-cryptographic memoization keys—approximately 10x faster than SHA-256 at 6.2 GB/s. For cache keys where collision resistance matters more, **BLAKE3** offers cryptographic strength at 3.3 GB/s with native incremental update support.

Merkle-style bottom-up hashing enables **O(log n) updates** when a single leaf changes:

```python
class ASTNode:
    def __init__(self, type, children):
        self.type = type
        self.children = children
        self._hash_dirty = True
        self._hash = None
        self.parent = None
    
    def mark_dirty(self):
        if not self._hash_dirty:
            self._hash_dirty = True
            if self.parent: self.parent.mark_dirty()
    
    def get_hash(self):
        if self._hash_dirty:
            child_hashes = [c.get_hash() for c in self.children]
            self._hash = xxh3_64(self.type.encode() + b''.join(
                h.to_bytes(8, 'little') for h in child_hashes))
            self._hash_dirty = False
        return self._hash
```

**Hash-consing** provides maximal structural sharing: syntactically identical subexpressions share the same memory address, enabling O(1) equality testing via pointer comparison. The hash serves simultaneously as the deduplication key and memoization cache key.

## LMDB multi-process cache coordination

LMDB's **single-writer multiple-reader** model uses copy-on-write shadow paging—data pages are never overwritten, ensuring the database is always consistent even after crashes. Multiple Nix processes can share an LMDB evaluation cache with these configuration recommendations:

```c
mdb_env_set_mapsize(env, 50UL * 1024 * 1024 * 1024);  // 50GB virtual (cheap)
mdb_env_set_maxreaders(env, 512);  // accommodate parallel evaluations
mdb_env_open(env, path, MDB_NOTLS, 0644);  // NOTLS for thread pool usage

// Always check for stale readers on open
int dead;
mdb_reader_check(env, &dead);
```

The **stale reader problem** occurs when a process crashes while holding a read transaction—pages referenced by that transaction cannot be reclaimed, causing database growth. Call `mdb_reader_check()` on environment open and periodically during long-running operations. For write contention, batch multiple cache entries per transaction since `mdb_txn_begin` blocks when another process holds a write transaction.

The **MDB_NOTLS flag** is essential for Nix's potential thread pool usage, decoupling transactions from threads. Error handling must address `MDB_MAP_RESIZED` (another process grew the map) and `MDB_READERS_FULL` (clear stale readers and retry).

## Property-based testing with RapidCheck

Generate random Nix expressions using size-controlled recursion:

```cpp
rc::Gen<std::unique_ptr<Expr>> genExpr() {
    return rc::gen::withSize([](int size) {
        if (size <= 1) {
            return rc::gen::oneOf(genInt(), genString(), genVar());
        }
        auto subtree = rc::gen::resize(size / 2, genExpr());
        return rc::gen::oneOf(
            genInt(), genString(), genVar(),
            genLambda(subtree), genApp(subtree), genAttrSet(subtree)
        );
    });
}
```

**Key properties to test**:

- **Soundness**: `eval(expr) == eval_after_clear_cache(expr)` for all expressions
- **Idempotency**: Repeated evaluation returns identical results
- **Context sensitivity**: Different bindings produce different results for open terms
- **Thread safety**: Concurrent evaluation produces correct results (verify with ThreadSanitizer)

For thread safety testing, compile with `-fsanitize=thread` and run stress tests that force shared thunks from multiple threads simultaneously. CHESS-style systematic testing explores thread interleavings through preemption bounding.

AST shrinking should use **subterm shrinking** (replace expression with any subexpression) and **constructor simplification** (replace if-then-else with one branch). Integrated shrinking that respects generator constraints prevents invalid shrink candidates.

## Benchmarking methodology for incremental systems

Track these metrics following Salsa and Bazel patterns:

| Metric | Formula | Purpose |
|--------|---------|---------|
| Hit rate | hits / (hits + misses) | Cache effectiveness |
| Time-saved ratio | (miss_time - request_time) / miss_time | Accounts for cache overhead |
| Speedup factor | baseline_time / cached_time | End-to-end improvement |
| Early cutoff rate | unchanged_results / total_revalidations | Propagation efficiency |

Salsa's **durability levels** partition queries by change frequency—volatile (user code), normal (dependencies), durable (stdlib). Version vectors instead of single numbers allow skipping entire query subgraphs when only user code changes, reducing overhead from ~300ms to near-zero for typical edits.

Representative Nix workloads include `nix-env -qa` (full nixpkgs), `nix-instantiate '<nixpkgs/nixos>' -A system` (NixOS config), and `nix flake check` (CI operation). Use `NIX_SHOW_STATS=1` for built-in statistics collection. For statistical rigor, run 10-30 iterations after warm-up, report 95% confidence intervals, and use geometric mean for speedup aggregation.

## Tooling integration recommendations

**Immediate CLI additions**:

```bash
nix eval-cache stats [--json]     # hit rate, size, entry counts
nix eval-cache clear [--all | <flake-ref>]
nix eval-cache gc --max-age 30d --max-size 5G
nix eval-cache verify [--all] [--repair]
```

**Configuration** (nix.conf):

```
eval-cache = true              # already exists
eval-cache-max-size = 5G       # new
eval-cache-max-age = 90d       # new  
eval-cache-gc-strategy = lru   # new
```

The evaluation cache should remain **independent of store GC**—it's a performance optimization rather than a dependency graph component. TTL-based eviction by last access time and total size limits provides appropriate lifecycle management. Cache verification should check SQLite integrity (`PRAGMA integrity_check`), validate that cached store paths still exist, and confirm flake fingerprints still match locked versions.

For debugging, add `--eval-cache-trace` to show cache decisions during evaluation, and expose hit/miss statistics in build output following Bazel's format: `eval: 847 cached, 12 evaluated`. This visibility helps users understand cache behavior and diagnose unexpected re-evaluation.
```

## Kagi

```markdown
## Round 8: Critique and Updated Plan for Nix Evaluator Memoization

### Executive Summary

The Round 7 plan is architecturally mature and addresses most critical concerns. However, several implementation details need concrete specification before coding can begin:

1. **Black-holing implementation** — GHC's lock-free approach provides the blueprint, but Nix-specific adaptations are needed
2. **LZ4 compression for large imports** — 10-100MB serialized imports need compression for LMDB storage
3. **Closed-term computation algorithm** — Concrete implementation for `is_closed` flag
4. **Cache CLI commands** — New `nix eval-cache` subcommands need specification
5. **Experimental feature flags** — Granular flags for staged rollout
6. **Graduation criteria** — Formalized metrics for stabilization

---

### Critical Issue 1: Black-Holing Implementation

**Problem**: The plan mentions "add black-holing for concurrent forceValue" but doesn't specify the implementation. [^2]

**GHC's approach** (from "Haskell on a Shared-Memory Multiprocessor" [^1]):

> "The key idea of this paper is this: evaluate thunks with no locking instructions whatsoever. This lock-free approach is based on the following observations: Because a thunk always represents a pure expression, semantically it doesn't matter if two threads evaluate the same thunk, because they will both return equivalent values."

**GHC uses three mechanisms**:

| Mechanism | Purpose | Overhead |
|-----------|---------|----------|
| **Grey-holing** | Write sentinel immediately on entry | 1 instruction |
| **Lazy black-holing** | Periodically scan stack, CAS to claim thunks | ~1-2% |
| **Global blocking queue** | Threads blocked on black holes wait here | Rare |

**Recommended implementation for Nix**:

Since Nix's `Value` struct is fully packed (16 bytes), we cannot add a result word like GHC. Instead, use the memo cache itself for coordination:

```cpp
// In forceValue():
Value* forceValue(Value& v, const PosIdx pos) {
    if (v.isThunk()) {
        Expr* expr = v.thunk.expr;
        Env* env = v.thunk.env;
        
        // Grey-holing: Try to claim this thunk
        // Use try_emplace_and_cvisit for atomic insertion
        bool claimed = false;
        Value* result = nullptr;
        
        thunkMemoCache.try_emplace_and_cvisit(
            ThunkKey{expr, env, globalEpoch},
            [&](auto& entry) {
                // Insertion succeeded: we claimed it
                entry.second = EVALUATING_SENTINEL;
                claimed = true;
            },
            [&](const auto& entry) {
                // Already exists: check state
                if (entry.second == EVALUATING_SENTINEL) {
                    // Another thread is evaluating - wait
                    result = waitForResult(expr, env);
                } else {
                    // Already evaluated - use cached result
                    result = entry.second;
                }
            }
        );
        
        if (!claimed) {
            return result;  // Cache hit or waited for result
        }
        
        // We claimed it - evaluate
        try {
            callFunction(v, 0, nullptr, v, pos);
            
            // Update cache with result
            thunkMemoCache.visit(ThunkKey{expr, env, globalEpoch},
                [&](auto& entry) { entry.second = &v; });
            
            // Wake any waiting threads
            notifyWaiters(expr, env);
            
        } catch (...) {
            // On exception, remove sentinel so others can retry
            thunkMemoCache.erase(ThunkKey{expr, env, globalEpoch});
            throw;
        }
    }
    return &v;
}
```

**Waiting mechanism** (using condition variables):

```cpp
// Global wait infrastructure
struct WaitEntry {
    std::mutex mutex;
    std::condition_variable cv;
    Value* result = nullptr;
    bool ready = false;
};

std::unordered_map<ThunkKey, std::shared_ptr<WaitEntry>> waitTable;
std::shared_mutex waitTableMutex;

Value* waitForResult(Expr* expr, Env* env) {
    ThunkKey key{expr, env, globalEpoch};
    
    std::shared_ptr<WaitEntry> entry;
    {
        std::unique_lock lock(waitTableMutex);
        auto& e = waitTable[key];
        if (!e) e = std::make_shared<WaitEntry>();
        entry = e;
    }
    
    std::unique_lock lock(entry->mutex);
    entry->cv.wait(lock, [&] { return entry->ready; });
    return entry->result;
}

void notifyWaiters(Expr* expr, Env* env, Value* result) {
    ThunkKey key{expr, env, globalEpoch};
    
    std::shared_lock lock(waitTableMutex);
    auto it = waitTable.find(key);
    if (it != waitTable.end()) {
        std::unique_lock entryLock(it->second->mutex);
        it->second->result = result;
        it->second->ready = true;
        it->second->cv.notify_all();
    }
}
```

**Key insight from GHC**: "Many thunks are cheap, so duplicate evaluation often doesn't matter." [^1] For expensive thunks, the waiting mechanism kicks in. For cheap thunks, occasional duplicate evaluation is acceptable.

---

### Critical Issue 2: LZ4 Compression for Large Imports

**Problem**: Round 7 answers confirm that serialized import results can be 10-100MB for large files like `all-packages.nix`. [^2] This is too large for efficient LMDB storage without compression.

**LZ4 characteristics** [^3]:
- Compression speed: >500 MB/s per core
- Decompression speed: Multiple GB/s per core (often RAM-limited)
- Compression ratio: ~2-3x for typical data

**Recommended implementation**:

```cpp
#include <lz4.h>

struct CompressedCacheEntry {
    uint32_t magic = 0x4E495843;  // "NIXC"
    uint16_t schema_version = 1;
    uint16_t flags;  // bit 0: compressed
    uint32_t uncompressed_size;
    uint32_t compressed_size;
    uint64_t checksum;  // xxHash64 of uncompressed data
    // followed by compressed data
};

std::vector<char> compressForCache(const std::vector<char>& data) {
    // Only compress if above threshold (e.g., 4KB)
    if (data.size() < 4096) {
        return data;  // Store uncompressed
    }
    
    int maxCompressedSize = LZ4_compressBound(data.size());
    std::vector<char> compressed(sizeof(CompressedCacheEntry) + maxCompressedSize);
    
    auto* header = reinterpret_cast<CompressedCacheEntry*>(compressed.data());
    header->flags = 0x01;  // Compressed
    header->uncompressed_size = data.size();
    header->checksum = XXH64(data.data(), data.size(), 0);
    
    int compressedSize = LZ4_compress_default(
        data.data(), 
        compressed.data() + sizeof(CompressedCacheEntry),
        data.size(),
        maxCompressedSize
    );
    
    header->compressed_size = compressedSize;
    compressed.resize(sizeof(CompressedCacheEntry) + compressedSize);
    
    return compressed;
}

std::vector<char> decompressFromCache(const std::vector<char>& compressed) {
    auto* header = reinterpret_cast<const CompressedCacheEntry*>(compressed.data());
    
    if (header->magic != 0x4E495843) {
        throw CacheCorruptionError("Invalid magic number");
    }
    
    if (!(header->flags & 0x01)) {
        // Not compressed
        return std::vector<char>(
            compressed.begin() + sizeof(CompressedCacheEntry),
            compressed.end()
        );
    }
    
    std::vector<char> decompressed(header->uncompressed_size);
    
    int result = LZ4_decompress_safe(
        compressed.data() + sizeof(CompressedCacheEntry),
        decompressed.data(),
        header->compressed_size,
        header->uncompressed_size
    );
    
    if (result != header->uncompressed_size) {
        throw CacheCorruptionError("Decompression size mismatch");
    }
    
    // Verify checksum
    if (XXH64(decompressed.data(), decompressed.size(), 0) != header->checksum) {
        throw CacheCorruptionError("Checksum mismatch");
    }
    
    return decompressed;
}
```

**Storage impact**: With 2-3x compression, a 100MB `all-packages.nix` result becomes ~35-50MB. Combined with the 1GB LMDB limit, this allows caching ~20-30 large imports.

---

### Critical Issue 3: Closed-Term Computation Algorithm

**Problem**: Round 7 answers confirm `is_closed` is not currently computed and needs ~200 LOC across ~10 `Expr` subclasses. [^2]

**Algorithm**: An expression is closed if it has no free variables (all variables are bound within the expression itself).

```cpp
// Add to Expr base class
class Expr {
public:
    bool is_closed = false;  // Computed during bindVars
    
    // Returns the maximum De Bruijn level of free variables
    // 0 means closed (no free variables)
    virtual unsigned computeMaxFreeLevel(unsigned currentLevel) = 0;
};

// ExprVar: The key case
unsigned ExprVar::computeMaxFreeLevel(unsigned currentLevel) {
    if (fromWith) {
        // With-bound variables are resolved at runtime
        // Conservatively mark as not closed
        return UINT_MAX;
    }
    // De Bruijn index: level is how many binders up
    // If level >= currentLevel, it's a free variable
    return (level >= currentLevel) ? level + 1 : 0;
}

// ExprLambda: Introduces a binder
unsigned ExprLambda::computeMaxFreeLevel(unsigned currentLevel) {
    // Body is evaluated with one more binder
    return body->computeMaxFreeLevel(currentLevel + 1);
}

// ExprLet: Introduces binders
unsigned ExprLet::computeMaxFreeLevel(unsigned currentLevel) {
    unsigned maxLevel = 0;
    
    // Each binding can reference previous bindings
    for (size_t i = 0; i < attrs->attrs.size(); i++) {
        maxLevel = std::max(maxLevel, 
            attrs->attrs[i].e->computeMaxFreeLevel(currentLevel + i));
    }
    
    // Body sees all bindings
    maxLevel = std::max(maxLevel,
        body->computeMaxFreeLevel(currentLevel + attrs->attrs.size()));
    
    return maxLevel;
}

// ExprWith: Introduces dynamic scope
unsigned ExprWith::computeMaxFreeLevel(unsigned currentLevel) {
    // attrs is evaluated in current scope
    unsigned attrsLevel = attrs->computeMaxFreeLevel(currentLevel);
    
    // body may reference with-bound variables (conservatively not closed)
    // But if body doesn't use any with-bound vars, it could be closed
    unsigned bodyLevel = body->computeMaxFreeLevel(currentLevel);
    
    return std::max(attrsLevel, bodyLevel);
}

// Binary operations, function application, etc.
unsigned ExprOpAnd::computeMaxFreeLevel(unsigned currentLevel) {
    return std::max(
        e1->computeMaxFreeLevel(currentLevel),
        e2->computeMaxFreeLevel(currentLevel)
    );
}

// After bindVars completes:
void Expr::finalizeClosed() {
    is_closed = (computeMaxFreeLevel(0) == 0);
}
```

**Integration point**: Call `finalizeClosed()` at the end of `bindVars()` for the root expression:

```cpp
void EvalState::parse(...) {
    Expr* e = parser.parse(source);
    e->bindVars(*this, staticEnv);
    e->finalizeClosed();  // NEW: Compute is_closed
    return e;
}
```

---

### Critical Issue 4: Cache CLI Commands

**Problem**: Round 7 answers propose new CLI commands but they're not in the implementation plan. [^1]

**Recommended commands**:

```bash
# Garbage collection
nix eval-cache gc                    # Clean eval cache (default: keep last 7 days)
nix eval-cache gc --older-than 30d   # Clean entries older than 30 days
nix eval-cache gc --max-size 500M    # Shrink to 500MB

# Verification
nix eval-cache verify                # Check integrity of all entries
nix eval-cache verify --repair       # Remove corrupted entries

# Statistics
nix eval-cache stats                 # Show usage statistics
nix eval-cache stats --json          # JSON output for scripting

# Management
nix eval-cache clear                 # Remove all entries
nix eval-cache export <path>         # Export cache to file
nix eval-cache import <path>         # Import cache from file
```

**Implementation sketch**:

```cpp
// In nix/src/nix/eval-cache.cc
struct CmdEvalCache : virtual NixMultiCommand {
    CmdEvalCache() {
        addCommand<CmdEvalCacheGc>("gc");
        addCommand<CmdEvalCacheVerify>("verify");
        addCommand<CmdEvalCacheStats>("stats");
        addCommand<CmdEvalCacheClear>("clear");
    }
};

struct CmdEvalCacheStats : Command {
    void run() override {
        auto& cache = getEvalCache();
        
        nlohmann::json stats;
        stats["entries"] = cache.entryCount();
        stats["size_bytes"] = cache.sizeBytes();
        stats["l1_hits"] = cache.l1Hits();
        stats["l1_misses"] = cache.l1Misses();
        stats["l2_hits"] = cache.l2Hits();
        stats["l2_misses"] = cache.l2Misses();
        stats["hit_rate"] = cache.hitRate();
        stats["compression_ratio"] = cache.compressionRatio();
        
        if (json) {
            std::cout << stats.dump(2) << std::endl;
        } else {
            printTable(stats);
        }
    }
};
```

---

### Critical Issue 5: Experimental Feature Flags

**Problem**: Round 7 answers propose granular flags but they're not formalized. [^1]

**Recommended flags**:

```nix
experimental-features = [
  eval-memoization           # Basic intra-eval memoization (L1 cache)
  eval-memoization-persist   # Cross-eval persistence (L2 cache, LMDB)
  eval-memoization-share     # Cross-machine sharing (signatures, trust)
]
```

**Flag dependencies**:
- `eval-memoization-persist` requires `eval-memoization`
- `eval-memoization-share` requires `eval-memoization-persist`

**Implementation**:

```cpp
// In experimental-features.hh
enum struct ExperimentalFeature {
    // ... existing features ...
    EvalMemoization,
    EvalMemoizationPersist,
    EvalMemoizationShare,
};

// In eval.cc
void EvalState::initMemoization() {
    if (!experimentalFeatureSettings.isEnabled(ExperimentalFeature::EvalMemoization)) {
        return;  // Memoization disabled
    }
    
    // Initialize L1 (identity) cache
    l1Cache = std::make_unique<IdentityCache>(settings.evalCacheL1Size);
    
    if (experimentalFeatureSettings.isEnabled(ExperimentalFeature::EvalMemoizationPersist)) {
        // Initialize L2 (content) cache with LMDB
        l2Cache = std::make_unique<ContentCache>(
            settings.evalCachePath,
            settings.evalCacheL2Size
        );
    }
    
    if (experimentalFeatureSettings.isEnabled(ExperimentalFeature::EvalMemoizationShare)) {
        // Initialize signature verification
        signatureVerifier = std::make_unique<CacheSignatureVerifier>(
            settings.trustedEvalCacheKeys
        );
    }
}
```

---

### Critical Issue 6: Graduation Criteria

**Problem**: Round 7 answers define criteria but they're not formalized in the plan. [^1]

**Formalized graduation criteria**:

| Criterion | Metric | Threshold | Measurement |
|-----------|--------|-----------|-------------|
| **Correctness** | Differential test failures | 0 over 6 months | CI: empty vs warm cache comparison |
| **Performance (miss)** | Overhead on cache miss | <5% | Benchmark: `nix-env -qa` with empty cache |
| **Performance (hit)** | Speedup on cache hit | >80% | Benchmark: repeated `nix-env -qa` |
| **Stability** | Breaking format changes | 0 for 2 releases | Manual review |
| **Adoption** | Production users | ≥3 major projects | Survey/telemetry |
| **Documentation** | Coverage | 100% of public API | Doc review |

**Graduation process**:

1. **Alpha** (current): Behind `eval-memoization` flag, no stability guarantees
2. **Beta**: Enabled by default in Determinate Nix, format stability begins
3. **Stable**: Enabled by default in upstream Nix, format frozen

---

### Updated Implementation Plan

#### Phase 1: Correctness (Blocking) — Updated

| Task | Priority | Change |
|------|----------|--------|
| All existing Phase 1 tasks | **Blocking** | Unchanged |
| **Implement black-holing with `try_emplace_and_cvisit`** | **Blocking** | **NEW**: Concrete implementation |
| **Add waiting mechanism for blocked threads** | **Blocking** | **NEW**: Condition variables |

#### Phase 2: Performance — Updated

| Task | Priority | Change |
|------|----------|--------|
| All existing Phase 2 tasks | As specified | Unchanged |
| **Add LZ4 compression for L2 cache** | **High** | **NEW**: Compress entries >4KB |
| **Implement `is_closed` computation** | **High** | **CLARIFIED**: Concrete algorithm |
| **Add compression ratio to statistics** | **Medium** | **NEW**: Track compression effectiveness |

#### Phase 3: Architecture — Updated

| Task | Priority | Change |
|------|----------|--------|
| All existing Phase 3 tasks | As specified | Unchanged |
| **Implement `nix eval-cache` CLI commands** | **High** | **NEW**: gc, verify, stats, clear |
| **Add experimental feature flags** | **High** | **NEW**: Granular flags for staged rollout |
| **Formalize graduation criteria** | **Medium** | **NEW**: Metrics and thresholds |

#### Phase 4: Cross-Eval — Updated

| Task | Priority | Change |
|------|----------|--------|
| All existing Phase 4 tasks | As specified | Unchanged |
| **Add cache export/import commands** | **Low** | **NEW**: For migration and backup |

---

### Questions for the Codebase Agent (Round 8)

#### Black-Holing

1. **Is there existing infrastructure for thread waiting in Nix?** (e.g., condition variables, futures)

2. **How does `boost::concurrent_flat_map::try_emplace_and_cvisit` handle exceptions?** If the insertion callback throws, is the entry rolled back?

3. **What is the typical thunk evaluation time distribution?** (e.g., median, 99th percentile) This determines whether waiting is worthwhile.

#### Compression

4. **Is LZ4 already a dependency of Nix?** If not, what's the process for adding dependencies?

5. **What is the typical compression ratio for serialized Nix values?** Has anyone measured this?

6. **Should compression be configurable?** (e.g., `eval-cache-compression = lz4 | zstd | none`)

#### Closed-Term Optimization

7. **Are there expressions that appear closed but have hidden impurity?** Beyond `import`, `fetchurl`, etc.

8. **How does `with` interact with closed-term detection?** The algorithm above conservatively marks `with`-bound variables as not closed.

9. **What is the memory overhead of adding `is_closed` to every `Expr`?** (1 byte per node)

#### CLI Commands

10. **Where should `nix eval-cache` be implemented?** In `src/nix/` alongside other commands?

11. **Should `nix-collect-garbage` automatically clean eval caches?** Or require explicit `--eval-cache` flag?

12. **How should cache statistics be exposed in `NIX_SHOW_STATS`?** Separate section or integrated?

#### Graduation

13. **What is the current release cadence for Nix?** This affects the "2 releases" stability criterion.

14. **Are there existing differential tests in the Nix test suite?** Or would this be new infrastructure?

15. **How should telemetry for adoption tracking be implemented?** Opt-in? Anonymous?

---

### Summary of Key Changes from Round 7

| Round 7 Plan | Round 8 Change | Rationale |
|--------------|----------------|-----------|
| "Add black-holing" (unspecified) | **Concrete implementation with `try_emplace_and_cvisit`** | GHC paper provides blueprint |
| Large imports mentioned | **LZ4 compression for L2 cache** | 10-100MB imports need compression |
| `is_closed` mentioned | **Concrete computation algorithm** | ~200 LOC implementation specified |
| CLI commands proposed | **Formalized `nix eval-cache` subcommands** | gc, verify, stats, clear, export, import |
| Experimental flags proposed | **Granular flags with dependencies** | Staged rollout capability |
| Graduation criteria listed | **Formalized metrics and thresholds** | Clear stabilization path |

The most critical additions are:
1. **Black-holing implementation** — enables safe parallel evaluation
2. **LZ4 compression** — makes L2 cache practical for large imports
3. **`is_closed` algorithm** — enables significant caching optimization

The plan is now implementation-ready for Phase 1 and Phase 2 tasks.

[^1]: [round-7-answers](round-7-answers.md) (52%)
[^2]: [nix-evaluator-memoization-infrastructure-update-7](nix-evaluator-memoization-infrastructure-update-7.md) (45%)
[^3]: [4.14. Using SMP parallelism - Haskell](https://downloads.haskell.org/~ghc/7.0.3/docs/html/users_guide/using-smp.html) (3%)
```