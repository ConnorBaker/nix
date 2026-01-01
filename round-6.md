# Prompt

This is a continuation of round-5.md.

Please read all responses. You should fetch and read the sources referenced to ensure accuracy. You should also list those sources. Then, update your plan where appropriate, being sure to include alternative designs or implementations and their tradeoffs, as well as additional references. Lastly, answer any questions posed by any of the responses in a new file, round-6-answers.md.

Since there are a lot of steps here, please make a todo list and use it to keep track of your finished work and remaining work as you proceed.

# Responses

## ChatGPT

```text
Nix Evaluator Memoization Infrastructure

Goal

Implement thunk memoization for the Nix evaluator to enable:
	1.	Within-evaluation caching: Reuse results when the same thunk is forced multiple times during a single nix eval.
	2.	Cross-evaluation caching: Persist results to reuse across separate nix eval invocations.
	3.	Cross-machine caching: Share cached results between machines using portable, content-addressed keys (i.e. cache entries independent of Nix store paths).

Current status: Staged changes implement within-evaluation memoization, but have correctness bugs and architectural issues that must be addressed before use.

⸻

Codebase Verification (Round 5)

The following assumptions were verified against the actual codebase:

Verified Correct ✅

Assumption	Location	Status
Env has up, size, values[]	eval.hh:222-227	✅ Correct
EvalState has thunkMemoCache	eval.hh:1163	✅ Correct
EvalState has trylevel	eval.hh:481	✅ Correct
EvalState has exprHashCache, valueHashCache	eval.hh:1172,1184	✅ Correct
Value uses 3-bit type discrimination	value.hh:555-556	✅ Correct
SourceAccessor has fingerprint, getFingerprint(), hashPath()	source-accessor.hh:185,203,146	✅ Correct
Expression hash caches exist	expr-hash.cc, value-hash.cc, env-hash.cc	✅ Correct
Cycle detection in hashing (De Bruijn back-refs)	All hash files	✅ Implemented
forceValue memoization with impure tracking	eval-inline.hh:152-227	✅ Implemented
ExprPos (__curPos) hashed by position index (distinct per call site)	expr-hash.cc:503-516	✅ Correct ￼

Issues Found (Corrections Needed) ⚠️

Issue	Finding	Impact
impureToken naming	Actual field is impureTokenCounter_ (not impureToken)	Minor naming discrepancy
ExprWith::prevWith type	Defined as uint32_t distance, not a pointer	Plan is correct but could be clearer
with-bound variable hash bug	Confirmed: expr-hash.cc:332-341 hashes only withDepth, not the symbol name	Blocking: scope collisions in cache
GC callbacks not used	GC_set_start_callback and GC_general_register_disappearing_link exist but are not invoked	Required for cache eviction (to implement)
Three-phase AST processing	Currently no separate computeHashes() phase – hashing is done lazily during eval	Plan describes adding this in future
Portability classification comment	Stale comment at value-hash.cc:850-858 claims “pointer fallback”; code actually uses content hashing	Minor documentation fix
Tagged pointers task (Phase 2)	Prior plan suggested tagged pointers; conflicts with Decision 14 (tagged pointers not viable under current memory layout)	Task will be removed
Position-dependent builtins	unsafeGetAttrPos and __curPos are not marked impure in caching logic	Could lead to cached values carrying incorrect position info ￼ ￼; must be fixed


⸻

Codebase Context

Key Data Structures

Structure	File	Description
Value	src/libexpr/include/nix/expr/value.hh	Tagged union of types: nNull, nBool, nInt, nFloat, nString, nPath, nAttrs, nList, nFunction, nThunk, nExternal. Thunks contain {Env*, Expr*}; Paths contain {SourceAccessor*, std::string}.
Expr	src/libexpr/include/nix/expr/nixexpr.hh	AST node base class (∼30 subclasses). See ExprVar and ExprWith details below.
Env	src/libexpr/include/nix/expr/eval.hh	Runtime lexical environment. Implemented as a linked list via Env* up; each frame contains Value* values[size]. Created by lambdas, let bindings, with, and rec expressions.
EvalState	src/libexpr/include/nix/expr/eval.hh	Evaluation context: symbol table, allocator, caches, settings. Holds thunkMemoCache (the memoization cache), impureTokenCounter_ (global side-effect counter at line 1125), etc.
SourceAccessor	src/libutil/include/nix/util/source-accessor.hh	Abstracts filesystem access (supports real FS, Git trees, in-memory files, archives). Path Values carry an accessor pointer—paths from different accessors are treated as distinct and cannot be directly compared or cached interchangeably.

ExprVar and ExprWith (Critical for with-scoping)

ExprVar (nixexpr.hh:227-261):

struct ExprVar : Expr {
    PosIdx pos;
    Symbol name;        // Variable name (interned symbol ID)
    ExprWith* fromWith; // Non-null if resolved via an enclosing `with` (otherwise null)
    Level level;        // De Bruijn level (0 = innermost scope)
    Displacement displ; // Offset within environment frame
};

ExprWith (nixexpr.hh:640-657):

struct ExprWith : Expr {
    PosIdx pos;
    uint32_t prevWith;   // ⚠️ Distance (in Env frames) to previous `with` (not a pointer)
    Expr* attrs;         // The attribute set expression
    Expr* body;          // Body expression
    ExprWith* parentWith; // Pointer to enclosing `with` (or nullptr if outermost)
};

Hashing insight: Each ExprVar that comes from a with (i.e. fromWith != nullptr) needs to incorporate the chain of enclosing with scopes into its hash. The code uses ExprWith::parentWith to traverse that chain (since prevWith is just a numeric distance). This ensures that two identical sub-expressions under different with scopes produce different hashes.

Value Memory Layout (Implications for Memoization)

On 64-bit systems with default 16-byte alignment, Value uses a bit-packed 16-byte representation:

// value.hh:536-553 (simplified)
template<std::size_t ptrSize>
class alignas(16) ValueStorage<ptrSize, ...> {
    using PackedPointer = uint64_t;
    using Payload = std::array<PackedPointer, 2>;  // total 16 bytes
    Payload payload = {};
    static constexpr int discriminatorBits = 3;
    static constexpr PackedPointer discriminatorMask = 0b111;
};

Low-level type tagging is achieved by using 3 bits in payload[0] as a primary discriminator:

Discriminator	Payload Layout	Used For
pdSingleDWord	Type tag in high bits of [0]; data in [1]	Small primitives (int, bool, null, float), small attr sets, primOps, externals
pdListN	Pointer (tagged) + length	Large lists (heap-allocated list nodes)
pdString	Context ptr (tagged) + StringData*	Strings (carry context for derivation outputs)
pdPath	Accessor ptr (tagged) + StringData*	Paths (carry SourceAccessor + path data)
pdPairOfPointers	Both 64-bit words used (type tag in low 3 bits of [1])	Thunks, lambdas, function applications, small lists (two pointers)

Implications: This compact layout means many Value types (including thunks and functions) are just 16 bytes, improving cache locality. However, pointer-based identity (for memoization) must be handled carefully because Nix uses a conservative GC (Boehm): after a GC cycle, a freed Value* might be reallocated for an unrelated value. The memoization strategy must account for this (see Decision 23 on generation counters below).

⸻

Architectural Decisions

(Decisions incorporate insights from prior rounds and external research. Each addresses a specific design problem for the caching infrastructure.)

Decision 1: Split Cacheability from Portability

Problem: Intra-evaluation and cross-evaluation caching have different requirements. Some values are safe to cache within a single evaluation but not safe to persist and reuse across evaluations (or across machines).
	•	Intra-eval caching: It’s safe to memoize pure functions/expressions as long as their closure (environment) remains valid. For example, lambdas can be cached within one eval because their environment pointer remains stable; paths can be cached if referring to the same SourceAccessor (same machine/repo).
	•	Cross-eval caching: To persist results, cache keys/values must be portable. No machine-specific or run-specific references (like raw pointers or un-hashable content) should be embedded, or else the cached result would be invalid on a different run or host.

Decision: Differentiate cache levels. We will implement a two-tier caching scheme (Decision 12 below) so that the Level 1 cache (within an eval) can use fast identity-based keys (including pointers), while the Level 2 cache (cross-eval) uses content-based keys only. This also means introducing separate predicates: e.g. isIdentityCacheable(value) vs isContentCacheable(value) ￼ ￼. A value like a function may be allowed in L1 (for memoizing repeated calls in one run) but not in L2.

Decision 2: Env Hashing Order-Dependence

Problem: The hash of an environment (Env) in the current implementation can change if any contained thunk is evaluated. Environments hold thunks by pointer, and a thunk’s hash may be computed differently before forcing versus after (when it yields a value). Thus, simply hashing the environment’s content in memory is order-dependent: caching could treat the same environment as different before/after forcing a member.

Plan: Modify environment hashing to be order-insensitive with respect to thunk forcing. Specifically, represent thunks in the env hash by a stable identity (e.g. a pointer + epoch, or an intrinsic identity hash) rather than by the possibly-changing content of the thunk ￼. For forced values, use their structural content hash. In practice, this means updating env-hash.cc to detect thunks and use an identity token for them (see also Decision 12 on thunk hashing strategy). This will ensure an environment’s hash remains consistent throughout evaluation, as long as its structure and unforced thunks remain the same.

Decision 3: Cycle Handling in Serialization and Hashing

Problem: Nix allows recursive data structures (e.g. rec { … } sets) which can introduce cycles in values. The current hashing and serialization logic does not robustly handle cycles – a naive traversal would loop infinitely.

Solution: Introduce cycle detection using a visited set or De Bruijn indices (for env cycles). When computing hashes (or serializing values), maintain an ancestor stack and if a cycle is detected, break it by inserting a special marker or back-reference instead of recursing indefinitely. This is similar to how the evaluator currently handles cyclic Env via back-pointers. Concretely, in value-hash.cc and env-hash.cc, implement detection (e.g. using an std::unordered_set of addresses) and ensure a cycle yields a unique hash that is stable but not dependent on traversal order ￼ ￼. Likewise, in value-serialize.cc, track visited values and throw an error if a cycle is encountered (for now, we won’t attempt to serialize cyclic structures across evals). This decision prevents infinite loops and ensures cyclic values can at least be recognized and handled gracefully.

Decision 4: Position-Dependent Builtins

Problem: Certain builtins leak evaluation position information into results (specifically builtins.unsafeGetAttrPos and the special __curPos expression). These return file/line positions of attributes or the current expression. If we memoize an expression that uses these, we risk reusing a cached value that contains a stale position (e.g. pointing to a previous file or line). Also, even if the code is identical, a change in whitespace or comments could shift line numbers and conceptually should not invalidate computation, yet a naive cache would treat it differently if positions are included.

Solution: Treat position-dependent builtins as impure for caching purposes. We will mark any evaluation that calls unsafeGetAttrPos or __curPos with a special impurity token (e.g. ImpureReason::PositionDependent) to prevent caching across evals. Within one eval, these can still be computed, but their results should not be persisted in the content-based cache. In practice, this means modifying primops.cc to call state.markImpure(PositionDependent) whenever these builtins are invoked, similar to how trace and currentTime are handled ￼ ￼. Additionally, we introduce an internal taint flag on values indicating they depend on positions (Decision 9 covers a general taint mechanism). This decision ensures that expressions returning position info will always be recomputed in new contexts, preserving correct behavior and not causing cache misses due purely to formatting changes.

Decision 5: Symbol Table Identity (Multiple Evaluators)

Problem: Nix’s C++ API allows multiple independent EvalState instances in one process, each with its own symbol table. Symbol IDs (the Symbol type used for variable names) are indices in a table, so the same numeric ID in two evaluators could refer to different strings. If we were to compute a hash of an expression that included symbol IDs directly, two identical expressions from different EvalState instances would hash differently (or worse, different symbols could hash the same just by ID collision).

Solution: Include the identity of the symbol table in the hashing scheme, or avoid relying on raw symbol integers for cross-eval hashing. A simple approach is to incorporate a per-EvalState salt in hash computations or to use the symbol string content for hashing exported cache keys. In practice, our plan is to re-intern symbols on cache load (see Decision 10 on serialization) so that persistent caches don’t rely on identical symbol table addresses. Additionally, when computing structural hashes for cross-eval use, we can mix in an identifier of the EvalState if necessary. This ensures that cache keys from one process’s symbol table won’t erroneously collide with those from another. The cost (slight increase in hashing work) is acceptable for cross-eval caching.

Decision 6: Cache Granularity

Problem: What should be the granularity of caching? Options range from fine-grained (cache every sub-expression) to coarse (only cache at certain boundaries like file imports or top-level expr). Fine-grained caching yields more reusability but costs more overhead (many cache lookups) and complexity (handling dependencies), while coarse caching might miss some opportunities but is simpler.

Decision: Cache at the thunk level, which corresponds roughly to lexical closures and delayed computations. Nix already represents most intermediates as thunks (due to laziness). By memoizing thunks, we cover function results, let-binding expressions, and recursive sets. We will not cache individual primitive operations or trivial expressions (their overhead doesn’t warrant caching). Additionally, we will add explicit caching for expensive operations that are not naturally thunks, notably file imports and import-from-derivation (IFD), via dedicated mechanisms (see Decision 11). This strikes a balance: we piggyback on Nix’s existing laziness (each thunk is a natural cache entry boundary) and add special-case caching for known heavy operations.

Decision 7: Persistent Storage Backend

Problem: For cross-evaluation (persistent) caching, we need a storage backend to save and load cached values. Requirements: fast key->value lookup, support for binary values up to possibly many megabytes (derivations), ACID semantics for concurrent access (since the Nix daemon or multiple CLI processes might access it), and portability.

Options: We considered LMDB (Lightning Memory-Mapped DB) and SQLite:
	•	LMDB offers key-value store semantics, memory-mapped file for fast reads, single-writer/multi-reader which fits Nix (one daemon writing, many readers). It would serve as the primary cache store.
	•	SQLite could be used for metadata or as an alternative, but its SQL overhead is unnecessary for simple key-value lookups. It might, however, be useful to track statistics or perform complex queries on cache entries (if needed for cleanup or analysis).

Decision: Use LMDB as the primary persistent cache store, and optionally use SQLite for auxiliary metadata and eviction policy if needed. We will integrate LMDB in Phase 4 for storing content-addressed cache entries (Decision 10 discusses serialization format). LMDB’s MVCC design aligns well: the daemon can handle writes in a transaction (ensuring that we only commit a cache entry when fully computed and serialized), and multiple nix clients can read concurrently without locks ￼ ￼. SQLite (or even just in-memory stats) can be added later if we want more sophisticated eviction metrics (see Decision 17 on eviction).

Decision 8: Alpha-Equivalence Hashing

Problem: Nix is not a lambda calculus in the pure sense (it has builtins, effects, etc.), but there is still the question of whether two functions that differ only in bound variable names should be considered the same for caching. In other words, do we want to hash modulo alpha-equivalence for lambdas? If we don’t, then trivial renamings would produce different cache keys.

Decision: For now, do not attempt full alpha-equivalence hashing beyond what Nix already does with De Bruijn indices. Nix’s binding structure (converted in bindVars) already ensures that variable references use levels and displacements, meaning the hash of an expression is insensitive to the original source variable names. This is essentially alpha-equivalence at the AST level (thanks to how ExprVar is represented). However, two functions with different closed-over values are not equivalent even if their code is alpha-equivalent, and our hashing must distinguish those (see Decision 12). Therefore, we will rely on the existing De Bruijn mechanism for name insensitivity and not implement a separate expensive alpha-equality check in the hashing. If future research (e.g. the 2021 PLDI algorithm for hashing modulo alpha ￼) becomes practical, we can revisit, but it’s low priority.

Decision 9: Impurity Tracking Granularity

Problem: Nix currently uses a single global impureToken counter to invalidate the cache whenever any impure operation occurs (like reading a file, getting current time, etc.). This is very coarse: any impurity invalidates all cached results that depend on any impurity. We would prefer a more fine-grained approach so that, for instance, reading an environment variable doesn’t invalidate caches that only depend on network access, etc.

Decision: Introduce per-effect impurity tokens. We will extend the evaluator to distinguish categories of side-effects: file system reads, environment variable reads, network access, time, etc. (Nix’s EvalSettings already has an EvalInputs structure that tracks some of these inputs). Instead of a single impureTokenCounter_, we will maintain a small set of counters (or a version vector) – for example, fsToken, envToken, netToken, timeToken – and increment the relevant one on each impure operation ￼ ￼. Cached values will record which token(s) they observed; when reusing a cache entry, we compare the current tokens to those recorded to decide if it’s still valid. This way, an unrelated impurity doesn’t flush the entire cache. In Phase 5, we may refine this further into durability levels (see Decision 16) to decide how long certain effects can be considered valid.

Decision 10: Serialization Format for Cross-Eval Caching

Problem: To persist a Nix Value (which can be a tree of attributes, lists, strings, etc.), we need a deterministic binary format. It must be portable across architectures and Nix store paths, and canonical (the same value always serializes to the exact same bytes, so that its hash can serve as a content hash). We also want it to be reasonably compact and fast to (de)serialize.

Options: We considered CBOR (Concise Binary Object Representation) with deterministic encoding, as well as simpler ad-hoc encodings:
	•	The advantage of CBOR is that there’s a standard for canonical encoding (RFC 8949 §4.2 ￼), including sorted map keys and normalized numeric representations (e.g. a canonical NaN value).
	•	Alternatively, Nix could define its own binary format (e.g. the NAR format used for store paths) but that might reinvent wheels.

Decision: Use Deterministically-Encoded CBOR for persistent cache values. Each Nix value type will be assigned a CBOR representation (somewhat like how the NAR format serializes store paths, but we include all value types). We will ensure maps (attrsets) have their keys sorted and numeric values canonicalized (for floats: canonical NaN, and +0 instead of -0) ￼ ￼. Strings will be emitted with a context if present (or a back-reference table for deduplication, see Round 4 Answer 4). This format will be used to compute content hashes (for the L2 cache key) and to actually store the values in LMDB. By making it canonical, any two identical values produce the same serialized bytes and thus the same hash, regardless of machine or evaluation session. This is crucial for cross-machine cache sharing. In implementation, we will likely write a custom serializer (value-serialize.cc) adhering to these rules, possibly leveraging an existing CBOR library for basic encoding but with careful control over ordering.

Decision 11: Import and IFD Caching

Problem: Nix expressions can use builtins.import to load another file, and “Import From Derivation” (IFD) to build a derivation during evaluation. These are notoriously expensive (import does I/O and parsing; IFD triggers a build) and are pure functions of their inputs, so we want to cache them. However, their keys and invalidation differ:
	•	A normal file import is identified by the file path and its content. If the file’s content (or the path fingerprint) hasn’t changed, re-importing should yield the same value.
	•	An IFD is identified by a derivation (which itself is content-addressed by its inputs); after building, the result is placed in the store. We should cache the result of an IFD by the derivation’s hash (and perhaps validate the store path exists with the expected hash on reuse).

Decision: Implement specialized caching for imports and IFD:
	•	Imports: We will hash ExprParseFile nodes by the fingerprint of the file path (using SourceAccessor::fingerprint). Currently, the evaluator treats each ExprParseFile as new each time (unless Nix’s own evaluator caches file reads internally). By hashing on file content, we can memoize file imports in the content cache (L2). This means if you import the same file in two different evaluations (or even twice in one eval), we reuse the parsed/evaluated result as long as the file’s content hasn’t changed ￼.
	•	IFD (derivations): For a derivation built during eval, use the derivation’s output hash as the cache key. Essentially, treat it as: if we’ve built this derivation before and got output path X, and X is still valid in the store, we can skip re-building it. We will store a mapping from derivation input hash -> realized output in the cache. On reuse, we might quickly check the store to ensure the path is present (and perhaps that its contents’ hash matches, for extra safety) ￼.
	•	We will incorporate EvalState::evalInputs (which tracks things like currentSystem, flake inputs, etc.) into cache keys as well, so that different system architectures or differing NIX_PATH don’t erroneously hit the same cache entry ￼ ￼.

This decision greatly speeds up scenarios where the same imports or IFDs are evaluated repeatedly (e.g., multiple modules importing common files, or many evaluations of the same flake).

Decision 12: Thunk Hashing Strategy

Problem: When computing a structural hash for a thunk (a deferred computation represented by {Env*, Expr*}), we have a choice:
	•	We could hash the thunk by its expression and environment content, essentially forcing it or hashing as if it were forced (which might involve recursively hashing a large sub-expression).
	•	Or we could hash it by a stable identity (like the pointer to the Expr and the pointer to the Env), deferring detailed hashing until needed.

For memoization, the tension is between precision (structural hashing ensures two equivalent thunks produce the same hash, enabling cross-eval deduplication) and performance (structural hashing can be expensive and even lead to forcing thunks during hashing, which might cause side effects or high overhead).

Decision: Use a two-layer approach (as foreshadowed in Decision 1). Concretely:
	•	L1 identity key: Use (Expr*, Env*, epoch) as a quick identity-based key (no deep hashing) for the in-memory cache. This treats each thunk by its memory identity (the code pointer and environment pointer), which is fast and ensures within one evaluation we never duplicate work on the same thunk. The epoch (a global generation count, see Decision 14/23) ensures that if those pointers get recycled after a GC, they won’t collide with a new thunk.
	•	L2 content key: For cross-eval caching, compute a structural hash of the thunk: essentially hash the AST of the expression plus a hash of the environment’s contents (excluding other thunks, which are handled by identity as noted in Decision 2). This will happen in a separate computeHashes() phase after parsing/binding (Decision 13) so that we cache the structural hashes for expressions. Two thunks that are extensionally the same (same code and same values for all free variables) will get the same structural hash ￼. This hash (likely 32 bytes from SHA-256) is used as the key in the persistent cache (L2).

Additionally, we will never force thunks during hashing ￼ – i.e., computing the structural hash of a thunk does not recursively evaluate the thunk’s expression; it only uses the stored hashes of the expression and environment. This avoids potential non-termination or side effects during hashing. If a thunk’s value is needed, normal evaluation will force it, but hashing is decoupled.

This two-tier strategy gives us the best of both: fast reuse within one run (even for impure or half-completed computations) and portable reuse across runs for pure computations. The complexity is manageable by implementing the caching logic carefully (Decision 16 discusses cache invalidation details).

Decision 13: Merkle-Cached Scope Chain Hashes (Round 3)

Problem: Hashing a with chain for each variable reference (ExprVar) can be expensive. In a worst case, if you have a deeply nested series of with expressions and many variables, computing each variable’s hash by walking up the chain is O(n) for chain length, and doing that for m variables gives O(n*m). We need to reduce this overhead.

Solution: Cache intermediate hashes of with scopes (a Merkle-like approach). During the proposed computeHashes() phase, when we compute hashes bottom-up, we can give each ExprWith (each with expression) a hash of the scope it introduces – basically a hash of its attributes combined with its parent’s scope hash. Then for an ExprVar, instead of traversing parentWith links one by one, we can just incorporate the already-computed hash of the entire chain of with up to that point ￼ ￼. We will add a field like scopeIdentityHash to ExprWith nodes to store this precomputed chain-hash. Thus, variable references can hash in O(1) by feeding the scopeIdentityHash plus the variable name into the sink. This is analogous to how content-addressed Merkle trees avoid re-hashing common prefixes repeatedly. The plan is to implement this in Phase 2 along with the three-phase processing change.

Decision 14: Generation Counters for GC (Round 3, updated Round 4)

Problem: As mentioned in the Value layout, pointer identity alone isn’t safe across garbage collections because memory can be reused. We need a way to detect if a cached pointer (Env* or Expr* or Value*) has been freed and reallocated for a different object, which would erroneously make an old cache entry seem valid for a new, unrelated object.

Original idea was to use tagged pointers: encode a small generation tag in spare pointer bits (e.g. using ARM64 TBI or x86-64 high bits) so that each allocation carries a generation count. However, analysis showed this isn’t portable or always feasible: Nix’s Value struct already tightly packs bits, and on some platforms (or with moving GC) this approach breaks. Indeed, we determined tagged pointers are not viable given Boehm GC and our constraints ￼.

Updated Decision: Use a global epoch counter (see Decision 23 for details). In short, maintain an epoch that increments on each major GC event, and incorporate this into cache keys. This way, any Value* allocated in a previous epoch will not collide with a new Value* in a later epoch because the key includes the epoch. We will leverage Boehm’s GC_set_start_callback to bump the epoch at the start of each GC cycle ￼ ￼. This approach is simpler and more portable than per-object generation counters, at the cost of invalidating all identity-based cache entries after a GC (which is acceptable, as those entries would be pointing to freed objects anyway). We consider this sufficient given the generational behavior of caches (many entries die young).

Decision 15: Closed-Term Optimization (Round 3)

Problem: Nix is lazily evaluated, but consider a scenario: (x: y: <large expression using x>) someValue someOtherValue. Normally, applying the first function to someValue would substitute x=someValue into the <large expression>, creating a potentially huge intermediate before applying it to someOtherValue. If <large expression> does not actually depend on x (i.e., it’s closed with respect to x), this substitution work is wasted – it could be done once and reused, or avoided entirely.

This is a known issue in functional languages (sometimes called maximal laziness or closed-term optimization ￼). Without an optimization, certain patterns cause exponential blowup in evaluation time due to repeated substitution.

Solution: Implement closed-term optimization: detect when a function’s body does not depend on one of its arguments, and avoid re-evaluating or re-copying the body for each application. In the example above, when applying (x: y: e1) to e2, if we know e1 does not depend on x, we can effectively transform it to let _ = e2 in (y: e1) – i.e., ignore the first argument application except for evaluating it for effects (or skip evaluating it at all if it’s pure and unused).

We can detect closed terms by augmenting the bindVars phase: when binding lambda parameters, compute whether the body actually uses them. One approach is to use De Bruijn levels: if the maximum free variable level in the body is less than the lambda’s level, then the lambda’s parameter is not used in the body ￼. We will add an isClosed flag to ExprLambda nodes during binding. At evaluation time, if isClosed is true for a function and it’s called, we can skip part of the work:
	•	Possibly don’t even evaluate the argument (if it’s pure and not needed – though in Nix, arguments might have side effects like throwing errors or using builtins, so we must be careful not to omit effects unless we’re in pure evaluation mode).
	•	More safely, we can evaluate the argument for its effects but then not substitute it into the body environment (or substitute a dummy since it won’t be used).

This optimization will be implemented in Phase 2. It should transparently speed up cases where large configuration sets are threaded through lambdas but never actually used.

Decision 16: Two-Tier Cache with Weak References (Round 3)

Problem: If we cache every thunk result, the cache could grow unbounded and hold onto large values, causing memory bloat. We need an eviction policy. However, with a garbage-collected language, typical eviction (like LRU) is tricky: if we simply remove an entry but some part of the program still holds a reference to that value, we must not free it. Conversely, if the GC frees a value, we should remove it from the cache to avoid a dangling pointer.

Solution: Use a two-tier cache:
	•	Strong LRU tier: a fixed-size cache (say 100k entries) where entries are strongly referenced, meaning they stay alive even if not referenced elsewhere, until evicted. This provides a bounded working set and ensures frequently used entries stay available.
	•	Weak cache (ephemeral tier): entries beyond the LRU capacity are kept only as weak references (using Boehm’s disappearing links or similar). If the GC collects a value, its weak entry will automatically disappear (Boehm supports registering a pointer that should be zeroed when the target is collected) ￼ ￼.

We will implement this by using Boehm’s GC_general_register_disappearing_link on cache pointers, rather than finalizers (finalizers in Boehm have known issues with cycles ￼). In practice, our thunkMemoCache (an unordered_map) can hold strong references for the most-recently-used subset and weak references for others. We’ll need to periodically sweep the cache to remove entries whose pointers have been collected (to avoid the cache holding dead links). This strategy, inspired by Guile’s approach and the ARC eviction policy (Decision 17), ensures we balance memory and speed: we get O(1) lookup for recent entries and automatically drop long-unused entries when memory is tight.

Decision 17: Adaptive Cache Eviction Policy (ARC) (Round 3)

Problem: Using a simple LRU for eviction can lead to suboptimal behavior in real workloads. For example, a one-time traversal of a large set of values (a “scan”) can evict many useful entries if you just use LRU. ARC (Adaptive Replacement Cache) is a policy that maintains two lists (recent and frequent) and two “ghost” lists to adapt to the workload pattern, providing better hit ratios without manual tuning ￼ ￼.

Decision: Implement ARC (or a simplified variant) for evicting entries from the strong cache tier. The idea is:
	•	Keep track of entries that have been seen only once recently (T1) and those seen multiple times (T2).
	•	When the cache is full, evict from T1 or T2 depending on the recent access pattern, and adjust the target sizes of T1 vs T2 using the ghost lists (B1, B2) as per the ARC algorithm ￼.
	•	This allows the cache to dynamically prefer recency or frequency based on which type of misses have been observed.

In implementation, we might use a simplified variant or at least ensure the eviction policy doesn’t degrade to thrashing. Because this is an in-memory cache for evaluator results (not disk cache), we want it to be low overhead. We will likely implement this in C++ manually (std::list for LRU lists and some bookkeeping counters). This will be integrated in Phase 2 or 3 when the cache is fully functional, with tuning parameters such as a default max size (e.g. 100k entries, configurable).

ARC’s benefit is mostly in long-running evaluations or daemons that cache across many operations. It ensures, for example, that iterating once over a very large list (touching many unique keys) doesn’t wipe out all frequently-used small keys from the cache. Instead, the ghost lists will detect a large stream of one-time uses and adapt by keeping the frequent list size larger.

Decision 18: Pure Evaluation Mode Optimization (Round 3)

Problem: Nix has a --pure-eval mode which disallows certain impure operations for stricter determinism. Currently, the evaluator checks settings.pureEval at many points (for example, to error out on builtins.fetchTarball or other impure actions). These checks are branches that incur slight overhead even in normal (impure) mode.

Decision: Provide a fast path for pure evaluation by using compile-time or initialization-time flags. For instance, we can template the evaluator on a PureEval parameter or have function pointers that either point to a “allowed” or “forbidden” version of certain operations. During startup, if pureEval is true, we select implementations of impure primitives that immediately throw errors; if false, we use normal ones. This avoids repeatedly checking the flag at runtime. It’s a micro-optimization (each check is cheap, but they add up given how often they might occur). We consider this a low-level performance tweak to do in Phase 3 if needed ￼. It won’t change functionality, just streamline the code for the common case (impure mode).

Decision 19: builtins.tryEval Context Tracking (Round 4)

Problem: builtins.tryEval wraps an evaluation and returns { success = true, value = X} if it succeeds or { success = false, value = <thrown> } if it throws. A subtlety is that an expression’s behavior inside tryEval can differ from outside it (because exceptions are caught). For caching, this means we must distinguish whether we are evaluating under a tryEval or not. Otherwise, an expression cached during a normal eval might be incorrectly reused inside a tryEval, or vice versa, leading to wrong success flags.

Decision: Include the current tryEval depth in the cache key. Nix’s EvalState already tracks a trylevel counter (incremented when entering tryEval) ￼ ￼. We will incorporate state.trylevel into our cache key for memoization. Concretely, the cache key structure gains a field like uint32_t tryEvalDepth. When tryEvalDepth > 0, it means the evaluation is within one or more nested tryEval calls. This field will differentiate cache entries so that we don’t mix results from inside vs outside tryEval. Implementation-wise, this is straightforward in the key construction (Phase 1). It slightly reduces cache hit potential (some expressions might be computed twice, once inside tryEval and once outside), but correctness demands it.

Decision 20: String Context in Cache Keys (Round 4)

Problem: Nix String values have an associated context: a list of derivations (and output names) that influenced the string (typically, in Nix, these come from interpolation of store paths and are used to track dependencies for derivations). Two strings with identical text but different contexts are not interchangeable in evaluation, because they lead to different derivations when used as builder paths. For example, "abc" and "abc" might be the same content but if one has context [{"path": /nix/store/…-pkg, "outputs": ["out"]}] and the other has context [], then using them in a derivation will produce different output hashes (one has a dependency, the other doesn’t).

Decision: Incorporate string context into the content-hash of values. When hashing a Value of type string for the content-based (L2) cache, we will hash not just the string’s characters, but also its context list (in a deterministic order) ￼ ￼. Specifically, sort the context entries by their store path (to avoid non-determinism in order) and hash each path and output name. The resulting hash ensures that two strings differ in the cache key if their contexts differ, preserving correctness. We will also treat strings that have had their context discarded via unsafeDiscardStringContext as tainted (so they don’t go into the persistent cache, because discarding context can import a path implicitly ￼). This way, our caches will respect Nix’s string dependency semantics.

Decision 21: Floating-Point Canonicalization (Round 4)

Problem: Floating point values in Nix can be a source of non-determinism. For instance, the representation of NaNs can vary (many bit patterns are NaN but they are considered equal by Nix), and the distinction between 0.0 and -0.0 is not significant in Nix’s semantics. Moreover, across different CPU architectures or compiler settings, certain floating operations (especially in debug vs release builds) might produce slightly different results or representations ￼. If we ignore these issues, the content hash of a float could differ across machines even when the “value” is conceptually the same.

Decision: Canonicalize floats before hashing for content cache:
	•	All NaNs are treated as a single canonical NaN bit pattern (e.g., IEEE 754 quiet NaN with a fixed payload of 0) ￼.
	•	+0.0 and -0.0 are treated as equal; we force -0.0 to +0.0 by setting the sign bit to 0 ￼.
	•	We will use a stable serialization of floats (for CBOR, likely 64-bit IEEE format), and ensure any platform-specific variations (80-bit extended precision, etc.) are handled (by rounding to 64-bit double precision for hashing).

By doing this in value-hash.cc when computing a float’s hash (and similarly when serializing), we guarantee that the same numeric value yields the same hash on all machines. In cases where floating-point computations truly differ across architectures (e.g. due to x87 precision), those will result in different bit patterns after canonicalization and thus different hashes – which is correct, since the results are not actually equal. If needed, in the future we might include a marker of architecture for floats if discrepancies remain (Decision 42 in Claude’s list suggested including platform in key if floats can’t be aligned ￼, but with canonicalization we hope to avoid that).

Decision 22: ARC-Inspired Cache Sizing (Round 4)

Problem: As discussed in Decision 17, eviction policy matters. Specifically, the size of the strong vs weak cache (or any LRU) should ideally adapt. ARC provides a self-tuning mechanism. Another aspect is we may want to give users or Nix configuration the ability to set durability/TTL for cache entries.

Decision: Implement a basic LRU for initial rollout, then consider an ARC-inspired adaptive policy for production use. We will implement the scaffolding for two LRU lists (recent, frequent) and ghost lists as per ARC ￼ ￼, but possibly not enable the adaptive adjustment until we gather some metrics. For now, the default max entries will be, say, 100k, and eviction will remove the least recently used. We’ll gather hit/miss metrics (Decision 17 covers adding profiling).

Additionally, support different durability levels for cache entries:
	•	Entries marked HIGH durability (e.g. pure, expensive computations) might never expire (or have a very long TTL).
	•	MEDIUM durability (e.g. file reads) might be given a time-based TTL (like 24 hours) in addition to LRU eviction ￼.
	•	LOW durability (impure stuff) could be per-evaluation only (not persisted at all).

This is an extension of Decision 9 and ties into the ARC strategy by possibly segregating or scoring entries by durability. Implementation will likely involve tagging each cache entry with a durability and periodically cleaning out expired ones. This decision ensures our cache doesn’t become stale or overly bloated with rarely-used data, and that it can adapt to real usage patterns automatically.

Decision 23: Global Epoch for GC (Round 4)

(This is the implementation of Decision 14’s updated strategy.)

Problem: Need to prevent cache collisions when pointers are recycled after GC.

Solution: Maintain a global atomic epoch counter, increment it at the start of each GC. Incorporate this epoch into every identity-based cache key ￼. We will implement:
	•	globalEpoch (64-bit) starting at 0.
	•	A GC start callback onGCStart() that does globalEpoch++ ￼.
	•	Use composite keys that include epoch for caches. For example, struct MemoKey { StructuralHash hash; uint64_t epoch; }; for the content cache, and for the identity cache, use { Expr*, Env*, uint64_t epoch } as the key ￼.

With this, any time a GC happens, the epoch changes and all existing identity keys effectively become invalid for future lookups (because new entries will have a newer epoch). Stale entries will remain in the cache until we choose to remove them (they won’t be found due to epoch mismatch, but we should eventually purge them to free memory). We’ll handle removal either on cache lookup miss or via the periodic sweep (Decision 16). This global epoch approach is simple and imposes very little overhead (just storing 8 extra bytes in keys and doing cheap integer comparisons). It’s also thread-safe as long as increment is atomic and reads are atomic, which is straightforward.

Decision 24: Exclude Functions from Persistent Caching (Round 5)

Problem: Nix function values (closures) capture an environment that may contain non-portable or context-specific data:
	•	Pointers to builtins (which are specific to the EvalState session),
	•	Thunks that might depend on impure state (like reading a file or env var),
	•	SourceAccessor pointers tied to a particular filesystem or store (not portable across machines) ￼.

Attempting to serialize and reuse function values across evaluations is both complex and unsafe. Two functions that are alpha-equivalent in code could close over different data, making them semantically different ￼. Verifying that a function is “pure” (has no impure or non-portable parts) would require traversing its entire closure graph ￼, which is expensive and error-prone.

Solution: Do not persist functions in the content-addressed cache. We will treat nFunction values as uncacheable across evals. In practice, when preparing a value for L2 storage, if its type is function, we will return a marker like CacheResult::NonCacheable("function") ￼. Such values will still be usable within one evaluation (they can reside in the L1 identity cache for memoizing multiple calls in the same run), but they won’t be written to LMDB or reused in future runs. This ensures we never restore a closure with dangling pointers or missing context. An implication is that any expression whose result is a function will not benefit from cross-evaluation caching – we accept this, as caching is most needed for concrete values like derivations, attrsets, lists, etc.

Exception: If Nix in the future moves to a compiled or bytecode VM where closures can be serialized (e.g., like Guile’s compiled procedures or Haskell’s interface files), this decision could be revisited ￼. But with the current AST interpreter, it’s safer to exclude functions entirely from persistent caching.

Decision 25: Semi-Semantic Hashing for Cache Keys (Round 5)

Problem: Explicitly tracking dependencies and invalidating cache entries when inputs change is complex and can be a source of bugs. The Dhall configuration language introduced an approach where cache keys incorporate both the expression and the hashes of all its imports, so that any change to an import automatically changes the key (and therefore stale cache entries are simply not looked up, obviating the need for an invalidation step).

Solution: Adopt semi-semantic hashing for top-level evaluation inputs (like Nix flake or expression files). Concretely:
	1.	Parse the input expression to an AST.
	2.	Compute a hash of each imported file’s content (semantic hashes of imports).
	3.	Compute a hash of the AST structure itself (a syntactic hash).
	4.	Combine these (e.g. by concatenating or hashing together) to form the final cache key ￼ ￼.

This means our cache key for an entry like “evaluate this Nix expression” automatically changes if any file it imports changes, or if the expression text itself changes. We no longer need to explicitly invalidate cache entries on file edits; the key won’t match and the entry will simply not be found (the new evaluation will store a new entry). We plan to implement this by extending the parser or evaluation initialization (Phase 4) to compute a top-level “semi-semantic hash” for the entrypoint expression. Internally, we have already added support for computing import content hashes via SourceAccessor::fingerprint (for import files) and will ensure that the parse -> bind -> computeHashes sequence yields both an exprHash (structural AST hash) and an aggregate including imports. The benefit is a greatly simplified coherence model: the cache is essentially content-addressed by the transitive closure of inputs, much like how build outputs are content-addressed by input hashes.

Decision 26: xxHash3 for L1 (Identity) Cache (Round 5)

Problem: Our hashing operations heavily use SHA-256 (for content hashing). But for the L1 identity cache, which is ephemeral and only within one process, collision resistance is not critical. The performance of SHA-256 can become a bottleneck – in fact, measurements showed hashing accounts for 60–70% of evaluation overhead in our prototype.

Solution: Use xxHash3 (a fast non-cryptographic hash) for identity cache keys. xxHash3 can process data on the order of 30+ GB/s in memory, versus SHA-256’s <1 GB/s throughput ￼ ￼. We will incorporate the xxhash library (it’s BSD-2 licensed and can be used as a single header). The identity key (which includes Expr*, Env*, and a few integers like epoch and tryEval depth) is small – we can hash those pointers and values with XXH3 to produce a 64-bit key in a few nanoseconds. This replaces the use of SHA-256 for L1. Meanwhile, we will keep using SHA-256 for L2 content hashes (where cryptographic collision resistance is mildly desirable to avoid malicious cache poisoning, and performance is less critical because it’s amortized by persistence and fewer in number).

To implement, we’ll add xxHash3 in meson.build as a dependency and use it in our computeIdentityHash function ￼. The result is that L1 cache insert/lookups become much faster, especially when keys are complex structural hashes (which we no longer need to compute fully for L1). This speeds up repeated evaluations significantly.

Decision 27: Disable Content Caching with Lazy Trees (Round 5)

Problem: Nix has an experimental feature “lazy trees” (lazy evaluation of NAR trees). A known issue (GitHub #13225) is that when lazy trees are enabled, StorePath::random() is called, introducing nondeterministic behavior in what should be content-addressed computations ￼. This breaks the assumption that content-based hashing is deterministic.

Solution: When lazy trees are active, turn off cross-eval (L2) caching. We will add a simple check: if settings.experimentalFeatures.lazyTrees is true, then valueIsContentCacheable returns false unconditionally ￼. This means we don’t attempt to store values in the persistent cache if lazy trees are involved. L1 caching (which is pointer-based and purely within the run) can remain on, since it doesn’t rely on content hashes. This is a temporary safety measure until the nondeterminism in lazy trees is resolved. We will also guard any content-hash based logic (like semi-semantic hashing) behind the same check if necessary. Essentially, enabling lazy-trees will fall back to behavior as if --disable-memoization for cross-run caching, to avoid incorrect reuse. This decision will be revisited once lazy trees are deterministic.

Decision 28: Position-Independent Semantic Layer (Round 5, Optional)

Problem: As noted in Decision 4, positions can reduce cache hits. Additionally, some users may want evaluation results that don’t embed any file/line info, for maximum reproducibility (like how build systems sometimes ignore debug info for caching). In rust-analyzer’s design for IDE responsiveness, they use a separation between a position-indexed layer and a pure semantic layer.

Solution: Provide an opt-in mode for position-independent evaluation. In this mode, we do not store actual file positions in values. For example, unsafeGetAttrPos could return a dummy value or an opaque ID instead of the real position. We could define a structure like:

struct SemanticAttrId {
    Symbol name;
    uint32_t index; // index in some global table for attr positions
};
std::unordered_map<SemanticAttrId, PosIdx> attrPositions;

In position-independent mode, calling unsafeGetAttrPos would return a SemanticAttrId value (which is just a pair of name and an index) that represents “the position of attribute X in some set,” rather than the actual file/line. The actual positions could be looked up on demand from attrPositions if needed (for debugging), but the cached value would not directly contain the file/line info ￼ ￼.

This is a significant change and would break expectations (e.g., meta.position fields in Nixpkgs would no longer show real positions). Thus, it’s optional and off by default. The idea is to maximize cache hit rates by ignoring differences that are purely positional. We would document clearly that enabling --position-independent-eval can cause loss of position data in outputs (with potential downstream effects on error messages or debugging info). If enabled, cache keys and values become oblivious to whitespace-only changes. Given the limited usage of __curPos and unsafeGetAttrPos in practice (mostly for error reporting and a few nixpkgs debug uses ￼ ￼), this mode could be acceptable for users prioritizing performance.

We plan to implement this in Phase 3 as an experimental feature flag. The influences include the rust-analyzer article on separating parsing and analysis to allow caching independent of positions ￼.

⸻

Alternative Architectures (Round 5 Analysis)

(In Round 5, we considered more radical design changes inspired by other systems. Below is an analysis and how we plan to incorporate (or not) these ideas.)

Recommendation: Query-Based Architecture (Salsa/Skyframe)

Description: Restructure the evaluator around an explicit dependency graph of queries (like Bazel’s Skyframe or Rust’s Salsa). In such an architecture, each “cell” (e.g., “the value of this attribute” or “the result of parsing this file”) is computed as a function of other cells and cached. Changes propagate by invalidating affected cells. This would make Nix’s evaluator incremental by construction.

Trade-offs:

Pro	Con (for Nix)
Explicit tracking of dependencies (fine-grained invalidation)	Major architectural rewrite of evaluator
Durability levels naturally fit (skip recompute parts that are high durability)	Requires reimplementing evaluation logic as demand-driven graph
Natural support for early cutoff (if dependencies unchanged, skip recompute)	Query key design is non-trivial (what constitutes the identity of a query result in Nix’s dynamic language?)
Huge potential speedups (Salsa reports 10–100× in some IDE scenarios)	Very high implementation cost and risk for an existing codebase

Assessment: This would be a long-term direction (essentially Nix 3.0 with a new evaluator core). For now, we opt for a hybrid approach: keep Nix’s lazy thunk-based evaluation, but introduce query-level caching at natural boundaries ￼. Those boundaries are:
	•	File imports: we already plan to cache these (Decision 11).
	•	Attribute set field access: Nix already memoizes field lookups by storing the evaluated value in the attrset (so this is effectively cached).
	•	Derivation instantiation: we plan to cache IFDs and possibly the construction of derivations (though the latter is cheap relative to building).
	•	Builtin function calls that are pure and expensive (we could memoize certain builtins internally, if needed).

This way, we get some benefits of the query model (reusing results of expensive sub-computations) without a full graph engine. If in the future we design a query graph, we’d likely do it in a new evaluator (perhaps in Rust, see Tvix below). For now, the plan focuses on improving caching granularity within the existing structure.

Recommendation: Content-Addressed Expression Storage (Unison/IPLD style)

Description: An extreme approach where every AST node and value is stored in a content-addressable store (like Merkle DAG). The entire evaluation could then refer to nodes by hash, enabling cache sharing globally. Unison language does this for code (each function is identified by a hash) and systems like IPFS/IPLD allow cross-system DAG sharing.

Trade-offs:

Pro	Con
Global deduplication: Identical sub-expressions or values across all evaluations (or even across different projects) could be stored once and reused everywhere.	Significant change to storage format – Nix’s current store is content-addressed for derivation outputs, but not for arbitrary values. We’d need a “Nix value store” possibly.
Renaming or reordering that doesn’t semantically matter wouldn’t change hashes (if done fully, includes alpha-equivalence etc.).	Would require storing every intermediate value persistently (or in a global cache), which raises storage and garbage collection questions.
Could enable incremental evaluation by hash: skip evaluating a node if you know its hash has been seen (and evaluated) before.	Requires a robust serialization of AST and values (which we are working on) and a mechanism to fetch subresults on-demand (network or local). Also introduces a distributed cache invalidation problem if not careful.
Cross-machine cache sharing would be trivial: just exchange DAG nodes by hash (IPLD could link to content on other machines).	Brings in additional dependencies (IPFS/IPLD) and complexity integrating with Nix’s security model (e.g., how to trust cached data from others?).

Assessment: This approach is intellectually appealing and aligns with content-addressed design. In fact, much of our plan (Decisions 10, 11, 25) is moving toward content-addressing values. By implementing deterministic serialization and structural hashes, we lay the groundwork for content-addressed value caching. However, fully storing the AST as a DAG and reusing sub-expression results might overshoot diminishing returns for Nix (which often doesn’t repeat identical large sub-expressions within a single eval, aside from imports).

Our recommendation is to achieve 90% of the benefit with less disruption: semi-semantic top-level hashing (Decision 25) already ensures the cache key accounts for all content. That, combined with import caching and memoization, yields similar end-result (no stale reuse) without a whole new storage format. We will keep an eye on projects like Guix (which is exploring content-addressed store items) and perhaps experiment with IPLD for specific use cases (like sharing flake input DAGs), but it’s not in the immediate plan.

Recommendation: Effect System for Impurity (Koka/Coeffects)

Description: Instead of using ad-hoc impurity tracking, introduce a language-level effect system that tracks which expressions use which side-effects. For example, a type-and-effect system could annotate a computation with FS[path] if it reads a file, or Network if it does network I/O, etc. This would allow caching/invalidation logic to be precise: only reevaluate when the specific resource used changes.

Trade-offs:

Pro	Con
Precise invalidation: We’d only invalidate cache entries exactly when the resource they depend on changes (no more, no less).	This would require changes to the Nix language (adding effect types or annotations), which is a massive endeavor and likely not backward-compatible.
Could enforce purity better (like a static analysis preventing certain ops in pure functions).	Nix’s user base expects dynamic, untyped behavior. An effect type system would complicate the language significantly.
Aligns with research on coeffects and fine-grained build systems.	Extremely high implementation effort for marginal gain given our other mechanisms (impurity tokens, durability levels) achieve much of this without changing the language.

Assessment: This is too invasive for Nix as it stands. While languages like Koka demonstrate the benefit of effect typing ￼, Nix’s charm is in being a simple, dynamically-typed DSL for configuration. Instead of adding a full effect system, we are achieving acceptable impurity handling via the impurity tokens (Decision 9) and upcoming durability levels (Decision 16). Those give us the ability to treat different effects differently (e.g., time vs file vs network) without exposing any new complexity to Nix users. Therefore, we will not pursue a Koka-style effect system in this plan.

Recommendation: Field Memoization (Jsonnet lesson)

Description: The Jsonnet configuration language (which is similar to Nix in some ways) had a performance pathology: accessing object fields would re-evaluate the field expression each time, leading to exponential blowups if not memoized. The recommendation was to memoize field values upon first evaluation.

Status in Nix: Already handled. Nix’s attribute sets evaluate their fields lazily once. Specifically, when an attrset is built, each attribute’s value is stored as a thunk (Value*). The first time you access that field, Nix forces the thunk and then stores the resulting Value* back into the attrset. Subsequent accesses return the memoized value directly ￼ ￼. This means Nix will not recompute an attribute value more than once per evaluation. For recursive sets (rec { ... }), Nix creates thunks for self-references, and those are also memoized via forceValue updates (so a recursive reference doesn’t loop indefinitely after the first evaluation).

Jsonnet’s 65× to 670× speedups in the optimized sjsonnet implementation came largely from adding this kind of memoization ￼, which Nix already has. So we do not need to change anything here. It’s a good validation that Nix’s design was ahead in this aspect. We will, however, add tests to ensure no regressions (that no field is evaluated twice).

No action needed. (This also reinforces that our caching efforts should focus on bigger grains like whole functions or files, since within an attrset Nix is optimal.)

Recommendation: Bytecode VM (Tvix)

Description: A suggestion to move away from the AST interpreter and use a bytecode virtual machine, possibly via a complete rewrite in a memory-safe language (Rust). The Tvix project on GitHub is an experimental Nix evaluator in Rust that uses a bytecode and separates evaluation, glue, and Nix compatibility layers ￼.

Trade-offs:

Pro	Con
Implementing in Rust could bring memory safety (no Boehm GC) and better concurrency by leveraging Rust’s ownership (potentially removing global interpreter lock issues).	Reimplementation effort is enormous: essentially writing Nix from scratch. Compatibility with all of Nix’s features (and edge cases) is a big challenge.
A bytecode VM might optimize certain operations and make future optimizations (JIT compiling hotspots, etc.) easier.	The performance gain of bytecode vs AST interpretation is not guaranteed to be large. Recent research shows an optimized AST interpreter can match a bytecode VM given good inlining and specialization ￼.
Could modularize the evaluator – e.g., separate concerns of evaluation and external I/O better. Tvix’s design separates pure eval from impure “glue”. This could yield cleaner impurity handling.	Divergence from upstream: a from-scratch evaluator would live outside NixOS/nix for some time, making it hard to merge improvements. Also, things like flakes, new builtins, etc., would need to be duplicated or kept in sync.

Assessment: While promising in the long term, adopting a bytecode VM is out of scope for our current plan. Instead, we focus on maximizing performance in the existing C++ AST interpreter. Many of our optimizations (memoization, better hashing, etc.) will benefit any implementation. And as noted, an AST interpreter with meta-tracing or just careful optimization can be as fast as a bytecode in many cases ￼. We will however take inspiration from Tvix and others:
	•	We may introduce an internal bytecode or opcode for specific hot builtins (like binary operations) to reduce dispatch overhead, but this can be done within the current interpreter.
	•	We ensure our design doesn’t preclude eventually integrating with a new evaluator. For example, using content-addressed caches means even a different implementation could share the cache if it agrees on the hashing/serialization format.

In summary, we’ll continue with the C++ evaluator for now, implementing memoization and caching there. We’ll monitor Tvix’s progress, and if it matures, one strategy could be to gradually swap out components (maybe using Tvix for pure evaluation while keeping parts of the Nix C++ code for certain builtins or effects). But that’s beyond this roadmap.

⸻

Action Plan

(The implementation will be broken into phases, with earlier phases focusing on correctness, then performance, then architectural improvements, and finally cross-evaluation and advanced features. Each task is labeled with a priority.)

Phase 1: Correctness (Blocking issues first)

These changes fix known bugs and ensure that caching does not change evaluation semantics.

Task	File(s)	Change/Addition	Priority
Fix with-bound variable hashing	expr-hash.cc:338	Incorporate the variable’s symbol name into the hash (add feedSymbol(sink, expr->name)) and include the full parentWith chain (using precomputed scope hashes if available) so that different with scopes yield different hashes ￼.	Blocking
Add cycle detection (hashing)	env-hash.cc, value-hash.cc	Implement detection of cycles in env/value hashing. Use an ancestry stack and back-references (instead of a naive marker) to safely break cycles ￼. Ensure that repeated traversal of a cycle yields a stable hash.	Blocking
Add cycle detection (serialization)	value-serialize.cc	Track visited values during serialization. If a cycle is encountered, throw an error (or emit a special marker if we decide to handle it). This prevents non-terminating serialization.	Blocking
Include string context in hashes	value-hash.cc	When hashing Value::nString, hash both the string content and its context list (sorted) ￼ ￼. This ensures strings with differing derivation contexts get distinct hashes.	Blocking
Canonicalize floats for hashing	value-hash.cc	Before hashing a float, normalize it: set sign of 0 to positive and replace any NaN with a canonical NaN bit pattern ￼ ￼. Do similarly in serialization.	Blocking
Track tryEval depth in cache key	eval.cc (key struct)	Add a tryEvalDepth field to the memoization key and set it from state.trylevel ￼ ￼. This differentiates evaluations inside builtins.tryEval.	Blocking
Generation counter (global epoch)	eval.cc	Implement global epoch (uint64_t). Hook GC_set_start_callback to increment it on each GC start ￼. Change cache key from just structural hash to a composite {structuralHash, epoch} (Decision 23) ￼ ￼. This replaces per-Value generation counters.	High
Use disappearing links for cache	eval.cc (cache init)	When inserting into thunkMemoCache (and any new caches), use GC_general_register_disappearing_link to tie the entry’s lifetime to the Value*. This ensures entries get removed when values are collected (prevents dangling pointers) ￼ ￼. Avoid Boehm finalizers due to cycle issues.	High
Periodic cache sweep	eval.cc	Implement a routine to periodically remove cache entries whose Value* have been nulled out (collected). For example, after each evaluation or on a schedule, scan cache buckets and erase entries with nullptr values. This prevents memory leak of empty cache nodes.	Medium
Mark unsafeGetAttrPos impure	eval.hh, primops.cc	Add a new ImpureReason::PositionDependent and call markImpure(PositionDependent) inside prim_unsafeGetAttrPos. Also, classify this impurity as non-cacheable in valueIsContentCacheable.	Medium
Mark __curPos impure	primops.cc	Similar to above, when evaluating ExprPos (__curPos), mark impurity. This may involve intercepting evaluation of that AST node in the evaluator loop to call markImpure(PositionDependent) (since __curPos isn’t a normal builtin function).	Medium
Tests for with hashing	expr-hash.test.cc (new or existing)	Add test cases: e.g. with {x=1; y=2;}; x vs with {x=1; y=2;}; y should yield different hashes (ensuring the variable name is included) ￼. Also test nested with: with A; with B; v vs with B; with A; v yield different hashes ￼.	High
RapidCheck property tests	libexpr/tests	Introduce property-based tests (using RapidCheck or similar) to ensure caching doesn’t change results. For example, generate random expressions and assert that evaluate(expr) == evaluateWithCache(expr) (where the latter enables our memoization) ￼ ￼. Also test idempotency: evaluating with cache twice yields same result ￼. These help catch subtle semantic differences.	High
Fix stale portability comment	value-hash.cc:850-858	Update the comment that mentions “pointer fallback” for env hashing to correctly reflect the current approach (content-based hashing with env recursion) ￼ ￼. Documentation fix (no functional change).	Low
Gate caching under lazy-trees flag	eval-inline.hh or eval.cc	Before using the persistent cache (L2), check if (settings.experimentalFeatures.lazyTrees) return /* no-cache */ ￼. This disables content-based caching when the lazyTrees feature is on (Decision 27).	High

Phase 2: Performance (High-Impact optimizations)

Once correctness issues are resolved, implement these to improve speed and memory usage for common scenarios.

Task	File(s)	Change/Addition	Priority
Two-level caching (L1/L2)	eval.cc, eval-inline.hh	Implement the two-tier cache structure: an L1 identity cache (e.g. identityCache: unordered_map<IdentityKey, Value*>) and an L2 content cache (contentCache: unordered_map<StructuralHash, SerializedValue>). L1 keys = {Expr*, Env*, epoch, tryEvalDepth}; L2 keys = structural hash (maybe wrapped with some context info). Ensure that forceThunk first checks L1, then if miss, computes L2 key (if value is cacheable) and checks L2. If found in L2, deserialize to a Value. Otherwise, compute result and insert into caches appropriately ￼ ￼.	High
Use xxHash3 for L1 keys	meson.build, eval.cc	Add xxhash dependency (probably via a single header). Use XXH3_64bits to hash the IdentityKey (which consists of a couple of pointers and an int) to an uint64_t for quick lookups ￼ ￼. This will replace using full SHA-256 on structural content for the L1 cache and significantly reduce overhead.	High
Three-phase AST processing	Parser/Eval code (nixexpr.cc, etc.)	Implement the parse → bind → computeHashes → eval pipeline. After parsing and bindVars, call a new Expr::computeHashes(symbols) on the AST root ￼ ￼. This traverses the AST bottom-up computing exprHash for each node. Store these hashes in new fields (e.g., extend Expr or use side data structures). This makes structural hashes available without deep recomputation each time. It’s especially needed for content hashing thunks efficiently.	High
Merkle scope hashes for with	nixexpr.hh, nixexpr.cc	As part of computeHashes(), compute a scopeIdentityHash for each ExprWith. For a with with parent scope hash P and attr-set hash A, define scopeHash = hash(P, A) (some stable combination). Store it in ExprWith. Then when hashing ExprVar in computeHashes, if fromWith != nullptr, just use fromWith->parentWith->scopeIdentityHash (or chain of them) instead of walking multiple steps ￼ ￼. This makes variable hash calc O(1).	High
ARC eviction policy	eval.cc (cache impl)	Replace or augment simple LRU in the cache with an ARC-like policy (T1, T2, B1, B2 lists as per Decision 17). Implement ghost lists and adjustment of target size p ￼ ￼. This will improve hit rates in mixed workloads. Initially, we might implement but not tune it aggressively until we have benchmarks.	High
Closed-term optimization	nixexpr.hh, nixexpr.cc	Add an unsigned isClosed : 1 flag to ExprLambda nodes. During bindVars, compute for each lambda if its body uses the bound variable (see Decision 15: use a function to compute max free level or similar ￼). If not used, mark isClosed. In the evaluator (eval.cc applying a lambda), if isClosed is true, you can skip environment setup for that variable or avoid copying a large expression for substitution. At minimum, skip traversing the body to substitute an unused var.	High
Defer value interning to tenure	eval.cc (allocator)	Nix currently interns (hash-conses) some values eagerly to save memory (sharing structure). This can cause overhead for short-lived values. Based on Appel’s 1993 idea ￼, we only intern values that survive a while. Implementation: perhaps allocate normally, and only when we sweep or on GC, intern those still around. Alternatively, maintain two allocators: one for short-lived (no interning) and if something moves to long-lived set (like gets into persistent cache or survives GC), then intern it. We document this approach and accept a bit of duplication (~3 MB waste for Nixpkgs eval) ￼ ￼. In fact, we might simply document that we won’t fix this (since the waste is small and complexity high).	Medium
Split cacheability predicates	eval-inline.hh (or new file)	Refactor valueIsUncacheable() into two functions: isIdentityCacheable(Value*) and isContentCacheable(Value*). The first returns false only for truly non-cacheable things even within eval (e.g. functions? Actually functions we can identity-cache within one eval, so that’s fine; main thing that stops identity caching is something like values depending on current time perhaps). The second is stricter (e.g. functions false, impure values false, anything with a non-portable accessor false, etc.). This makes logic clearer.	High
Never force thunks during hashing	value-hash.cc, env-hash.cc	Audit the hashing functions to ensure if they encounter a thunk, they do not call forceValue. Instead, hash thunks by their identity (expr ptr, env ptr, perhaps using the identity hash or an existing cached hash). In env hashing, represent thunks via a stable token as per Decision 2. This prevents evaluation during what should be a pure hashing phase ￼.	High
Blackhole detection (concurrency)	eval.cc (forceValue)	Implement black-holing for thunks being evaluated concurrently (or recursively). Use an atomic state in the Value (e.g. enum ThunkState { Unevaluated, Evaluating, Evaluated} with an atomic field). In forceValue, perform a CAS to set Evaluating -> if another thread already did, handle accordingly (throw an error or wait) ￼ ￼. This prevents duplicate evaluation and infinite recursion issues in multi-threaded contexts.	Medium
Accept monotonic allocation waste	Documentation (maybe code comments)	Acknowledge that by using a global epoch and not freeing interned expressions, we may accumulate some duplicate values. Based on calculations (~3 MB waste on Nixpkgs eval) ￼ ￼, we consider this acceptable. Add a note in the code or docs so future maintainers are aware this is intentional. If needed, provide a debug mode to count waste.	Low

Phase 3: Architecture (Improvements to design, non-critical but beneficial)

These tasks further refactor or improve the evaluator architecture for maintainability, scalability, or new use-cases.

Task	File(s)	Change/Addition	Priority
Fix env hash order-dependence	env-hash.cc	Implement the solution from Decision 2: hash environments such that the presence of thunks vs forced values doesn’t change the hash. Concretely, if an env slot is a thunk that hasn’t been forced, use an identity (e.g. pointer+epoch) in the hash; if it’s forced, use the content hash of the value. This way, forcing a thunk in an env doesn’t alter the env’s hash retrospectively ￼.	High
Fix portability classification	value-hash.cc:850	Update valueIsContentCacheable to properly classify lambdas as non-portable unless their entire closure is portable. Currently, a lambda’s portability might be misclassified. We will recursively check the env of functions: if any part is non-portable (contains store paths from different accessor, or impure thunks, etc.), treat the function as non-cacheable. This ties in with Decision 24 (functions generally not cached persistently) ￼.	Medium
Early cutoff (skip unchanged recompute)	Evaluator logic (no single file, perhaps EvalState or new cache manager)	Implement an early cutoff check: if evaluating an expression with a set of recorded dependencies that haven’t changed, return a cached result immediately. We can leverage the EvalTrace or an EvalInputs record of dependencies (file hashes, etc.) ￼ ￼. Specifically, after computing a result, store the list of file hashes, env vars, etc., in the content cache entry. On next eval, before recomputing, compare current hashes; if all match, just use the cached value. This effectively short-circuits evaluation when inputs identical (especially useful for incremental nixos-rebuild). This will be integrated with our content cache lookup.	Medium
Cache statistics & profiling	eval.cc (or separate)	Add instrumentation: counters for cache hits, misses at L1 and L2, time spent in hashing vs evaluating, etc. Possibly integrate with Nix’s --debug or tracing to output stats after evaluation (e.g., “Memoization: 80% hit rate, X entries, Y evicted”). Could also expose via a builtin. This helps validate performance gains and identify pathological cases.	Medium
Two-tier GC for cache	eval.cc (cache)	Implement the strong/weak cache tiers (Decision 16). Possibly maintain two containers or a structure where entries have a flag if they are in the “protected” (strong) set. Use GC_register_disappearing_link for weak ones only. The strong set can be LRU with size limit; evictions from strong go to weak. Ensure that when values in weak are collected, links disappear from cache. This reduces risk of dangling pointers.	Medium
Pure eval fast path	primops.cc or eval.cc	As per Decision 18, remove repeated if (settings.pureEval) checks. For example, wrap impure builtin functions in a conditional at startup: if pureEval, make them point to a stub that throws an error. Or template the evaluation loop on Purity. This will slightly complicate code but improve performance in pure or impure mode by avoiding checks. Start with obvious cases (like builtins.fetchTarball).	Medium
Document order-dependence limits	Documentation (e.g. Nix manual or code comments)	Clearly explain which scenarios can lead to cache misses due to evaluation order or laziness. For instance, mention that within one eval, forcing order might affect performance but not results, and across evals, our content hashing covers most cases except if side-effects or experimental features (like lazy trees) are involved. Setting expectations will help users understand cache hit/miss behavior (why some seemingly same eval might not hit cache if context differs slightly).	Low
Salsa-style durability levels	eval.cc	Integrate the durability concept: assign each impure operation a durability level (HIGH, MEDIUM, LOW as in Decision 16/ARC). Maintain a version vector or simply mark cache entries with the lowest durability of any input they depend on ￼ ￼. Then we can have policies: e.g., skip writing LOW durability results to disk cache, or evict MEDIUM after some time. Implementation: extend cache entry metadata to store durability and use it in eviction/TTL decisions.	High
Position-independent eval mode	eval-settings.hh, primops.cc	Add a global setting or CLI flag --position-independent-eval. If set, alter the behavior of unsafeGetAttrPos and __curPos as described in Decision 28: instead of returning real positions, return dummy semantic identifiers. Implement this by changing prim_unsafeGetAttrPos to check the flag and either call the usual implementation or a new one that returns a SemanticAttrId value. Store actual positions in a global map for later lookup. Also ensure that when this flag is on, we treat those dummy values as equal regardless of actual positions (to maximize cache hits). Document that this mode may break some error reporting or debugging info ￼ ￼.	Medium

Phase 4: Cross-Evaluation Caching (Persistent cache infrastructure – Nice to Have)

This phase introduces on-disk caching and other features to enable reuse across separate Nix invocations (and sets the stage for cross-machine sharing).

Task	File(s)	Change/Addition	Priority
Make ExprParseFile hashable	expr-hash.cc	When hashing a parse node representing an import (ExprParseFile), use the file’s content fingerprint (via SourceAccessor->fingerprint(path)) instead of the raw pointer or path string ￼. This ensures the structural hash of an import expression reflects the imported file’s content. (This ties into Decision 25 and import caching.)	Medium
Implement import caching	eval.cc (import logic)	Add caching for file imports: after parsing and evaluating an imported file, store its resulting value in the content cache keyed by the file’s content hash (or a combination of file hash and perhaps location if needed). Next time the same file (same content) is imported, short-circuit to the cached Value (deserializing if from disk). Nix might already do some of this internally for certain imports, but we will extend it to our memoization cache.	Medium
Implement IFD caching	N/A (new logic in eval or derivation code)	When builtins.derivation is called or when an IFD is triggered, compute a hash of the derivation (its inputs, output names, etc.). After building it, store the realized output path in a cache (perhaps in LMDB or just the Nix store DB). Next time the same derivation hash is encountered, if the output path exists and matches, we can reuse it without rebuilding. This likely integrates with Nix’s normal build cache (binary cache). We have to be careful to validate that the output is still present in the store (not GC’d) before skipping build.	Medium
Wire EvalInputs into cache key	eval.cc (CacheKey)	Incorporate EvalState.evalInputs (which includes things like current system, current NIX_PATH, flake lock hash, etc.) into the persistent cache key. For example, hash the EvalInputs struct (with stable serialization of its fields) and mix that into the content hash. This ensures that if you evaluate the same expression on two different systems (e.g. x86_64 vs aarch64), they won’t erroneously hit the same cache entry if the result could differ ￼ ￼. In particular, the currentSystem should be part of the key for derivations that include it.	Low
LMDB backend for cache	eval.cc (initialization)	Integrate LMDB: on startup, open an LMDB environment (file) in an appropriate location (e.g. $HOME/.cache/nix/evalcache or /nix/var/cache/eval). Use LMDB for the L2 cache storage. Each content-hash key maps to a byte string (the CBOR serialized Value). Implement put/get functions for the cache that wrap LMDB transactions. Use single-writer via the Nix daemon or a global lock if multiple processes might write. Also use LMDB’s reader check on startup to clear stale readers (in case of crashes) ￼. This gives us a durable on-disk store.	Medium
SQLite for metadata	Optional (cache-db.cc perhaps)	(Optional) Set up a SQLite database to track metadata: e.g. how often each entry is used, last access time, origin of entry, etc. This can help with eviction (we might implement a smarter eviction policy by issuing queries, or just for introspection via nix show-cache). This is low priority; initially, we can rely on in-memory stats and LMDB size limits.	Low
Deterministic CBOR serialization	value-serialize.cc	Implement serialization of Value to CBOR as per Decision 10. Ensure the following in the implementation: sort attrset keys, sort string contexts, canonicalize floats, represent lists and sets in a stable order, and include a version tag so we can evolve the format. Use the guidelines from RFC 8949 §4.2 for deterministic encoding ￼ ￼. We can use an existing CBOR library or write directly. Write test cases to serialize a value and deserialize it to ensure round-trip correctness. This serialized form’s hash will be our content key.	Medium
Exclude functions from persistent cache	eval-inline.hh or eval.cc	As per Decision 24, implement a check before writing to L2: if Value is of type nFunction (or contains functions in it), do not cache it. Concretely, in the code that prepares a Value for serialization, if v.type == nFunction return CacheResult::NonCacheable. Also consider functions inside lists/attrs: likely we treat the whole composite as non-cacheable if any part is a function, to be safe (alternatively, could serialize everything except functions, but that breaks referential integrity). Simpler: if the final result of an eval is or contains a function, we just don’t cache it across evals. Document this limitation.	Medium
Implement semi-semantic hashing	eval.cc (top-level eval)	Compute the Dhall-style combined hash (Decision 25) for top-level expressions. For nix eval of a given expression or file: after parsing and computing all sub-hashes (Phase 2/3 work), do: semiSemanticHash = hash(computeExprStructuralHash(ast), concat(importContentHashes)). Use this as the key to lookup in the persistent cache before doing actual evaluation. If found, skip evaluation and just deserialize the result. If not, proceed to evaluate, then store the result under that key. This eliminates explicit invalidation: changing any imported file or the expression itself automatically gives a new key ￼ ￼. This task ties together a lot of earlier pieces (import hashing, serialization, etc.).	High

Phase 5: Advanced Optimizations (Future plans)

These are forward-looking improvements that can be implemented once the above is stable, time permitting.

Task	Description	Priority
Cross-machine cache sharing	Implement a mechanism to share the persistent cache across machines. For example, integrate with Nix’s binary cache protocol: package cached evaluation results as store objects (e.g., a fixed-output derivation whose contents is the LMDB or individual CBOR blobs), so they can be uploaded to a cache server. Or use a peer-to-peer approach (IPFS/IPLD) to publish content-addressed results so other machines can fetch by hash ￼. Concretely, we might add commands like nix cache push <hash> and nix cache pull <hash> that interface with cache.nixos.org or a custom endpoint. This requires careful consideration of trust (only share cache for pure results). But achieving this means if one machine evaluates something, another can reuse it by content hash match.	Medium
Fine-grained impurity tracking	(Extending Decision 9) Possibly introduce even more granular tracking, e.g., per-path versioning. We could integrate with filesystem watchers or OS-specific file version APIs to know if a file has changed since last eval and only invalidate caches for that file. Or maintain a mapping of file -> last seen hash and auto-invalidate if changed. This ties into durability: e.g., mark file reads as MEDIUM and provide a way to invalidate those after 24h. Implementation could involve a background thread checking timestamps or a user command nix cache invalidate <path>.	Low
Query-level caching (incremental eval)	Revisit the query model: identify if there are opportunities to incorporate a mini query engine for some parts of Nix. For instance, we could treat the set of top-level attributes in Nixpkgs as queryable nodes and only re-evaluate those that change (similar to how nix-env -qa could be incremental). This is speculative. If feasible, implement a prototype where the evaluator records a dependency graph (like a crude Skyframe) and on subsequent runs, only evaluates nodes whose inputs changed. This might go beyond current Nix, so it’s a research direction rather than concrete plan.	Low
Parallel AST hashing	Use multiple threads to speed up the computeHashes() phase for large ASTs. For example, in an attrset with many attributes, spawn tasks to hash each attribute’s expression in parallel ￼ ￼. GHC’s interface file generation does similar parallelism for separate declarations. Implementation: use C++17 std::async or a thread pool to hash independent subnodes concurrently. Be careful with thread-safety (our global symbol table and caches need locks). This could reduce parsing/hash time significantly for huge expressions (though those are not the main bottleneck, it’s an easy win where applicable).	Low


⸻

Will This Meet the Goals?

Goal	Status	Blockers/Next Steps
Within-evaluation caching (no repeated work in one run)	✅ Achieved after Phase 1.	Fixing the with hash bug and adding cycle detection are critical for correctness. Once done, the L1 identity cache can be safely enabled. We also ensure GC safety (epoch, disappearing links) so it won’t crash.
Cross-evaluation caching (reuse results across runs)	⚠️ Foundation in place by Phase 2-3, full support after Phase 4.	Needs completion of persistent storage (LMDB), deterministic hashing/serialization, and import/IFD caching from Phases 2-4. After Phase 3, we have content hashes for values; Phase 4 writes them to disk and reads on next run. Until serialization and LMDB are implemented, cross-run reuse is not active.
Cross-machine caching (share cache between machines)	⚠️ Planned after Phase 5 (future work).	Requires content-addressed keys (Phase 4 provides) and a distribution mechanism (Phase 5). We need to integrate with Nix’s substituter or use IPFS to actually exchange cache entries. Once done, a cache entry computed on Machine A (e.g., a large attribute set or derivation evaluation) can be used on Machine B, provided both have the same store contents for dependencies. This remains future work but is a core requirement driving our design choices (everything is being made portable and deterministic) ￼.


⸻

Sources

Core References
	•	Dolstra, “Maximal Laziness in Nix” (LDTA 2008) – Introduces closed-world optimization and hashing consing with ATerm (influenced our Decision 15) ￼.
	•	Build Systems à la Carte (ICFP 2018) – Space of build caching strategies, early cutoff (helped inspire Decisions 11 and 25).
	•	Adapton: Composable Demand-Driven Incremental Computation (PLDI 2014) – Incremental computing theory (related to query-based architecture ideas).
	•	Nominal Adapton: Memoization by Evaluation (OOPSLA 2015) – Use of names for stable memoization (somewhat analogous to our approach of stable hashing with epochs and content).

Incremental Computation & Effect Tracking
	•	Salsa (Rust Analyzer incremental) – Blog posts (2020, 2023) on durable incrementality and position-independent semantics influenced our thinking on durability levels and position-independence ￼ ￼.
	•	Jane Street Incremental – An OCaml library achieving 30ns overhead per node ￼, shows that fine-grained incremental graphs can be efficient (but implementing similar in Nix would be a huge project).
	•	Coeffects (Orchard 2014) – Theoretical basis for effect systems ￼; we chose a pragmatic coarse approach instead (Impurity tokens).

Content-Addressing & Hashing
	•	Unison language – Content-addressed code, 512-bit hashes, approach to hashing recursive structures (gave us confidence in using content hashes widely) ￼.
	•	Maziarz et al. 2021 – Hashing modulo alpha-equivalence (we decided not to fully implement, but good reference if we ever do) ￼.
	•	IPLD / IPFS – Ideas for storing and linking content-addressed data across systems (relevant to cross-machine caching) ￼.
	•	RFC 8949 (CBOR Canonical Encoding) – Guidelines we follow for serialization format ￼.

Memory Management and Concurrency
	•	GHC StableName / stable-memo – Haskell’s solution for pointer identity in a GC world (inspired our epoch approach to some extent) ￼.
	•	Boehm GC finalization and disappearing links – Documentation on how finalizers don’t work with cycles, reinforcing our use of disappearing links ￼.
	•	ARM64 Top-Byte-Ignore (TBI) – We looked at tagged pointer hardware support (ARM64) which would allow 8-bit tags with zero overhead ￼ ￼, but since x86_64 support is limited and we moved to global epoch, we dropped this avenue.
	•	Rust crossbeam / multi-producer – If we move parts of this to Rust or threads, we’d consider using lock-free structures for caches. Our plan’s black-holing and concurrent map options were influenced by one of ChatGPT’s answers and by typical patterns in concurrent runtimes ￼ ￼.

Performance Benchmarks & Inspiration
	•	Kovács’s normalization benchmark – Showed GHC’s optimized AST eval outperforming some JITs for lambda calculus ￼, supporting our decision to stick with AST + specialization rather than a full JIT.
	•	sjsonnet – A reimplementation of Jsonnet in Scala that is much faster by aggressive caching; their 65-670× speedup number ￼ was a motivating example for us to pursue memoization aggressively.
	•	V8 “Trash Talk” – Insights into GC tuning (like doing GC at idle times) which could be applied if we find GC overhead significant. We might consider prompting a GC at strategic points (e.g., after a big eval) to avoid random pauses.

Alternate Implementations
	•	Tvix (Rust Nix evaluator) – Ongoing project to watch ￼. Its design influenced our optional ideas (e.g., maybe eventually separate the pure evaluation and side-effectful parts). Not directly integrated, but it validated some choices like possibly using a bytecode is not immediately necessary.
	•	GNU Guix – Guix uses Scheme (Guile) and has content-addressed store for derivations. We took note of their approach to see what might be generalized (like the idea of a separate serialization for derivations which we partially emulate in import/IFD caching).
```

## Claude

```markdown
# Nix Evaluator Memoization: Implementation Deep Dive

The proposed two-level caching system for Nix evaluation faces ten critical implementation gaps. This report provides concrete solutions drawn from production systems including Dhall, GHC, Salsa, Shake, Bazel, and Buck2, with specific code patterns, performance data, and architectural recommendations.

## Semi-semantic hashing: Dhall's proven approach

Dhall's semi-semantic caching provides the blueprint for Nix's hash strategy. The algorithm resolves imports to their **normalized forms** before computing the syntactic hash, automatically incorporating semantic content of all dependencies without explicit invalidation logic.

**The core algorithm:**
1. Parse the input expression
2. Recursively resolve all imports, replacing each with its **fully normalized form**
3. Hash the resulting expression syntactically (the AST with imports inlined)

This approach means semantically different inputs produce distinct hashes, while the cache key changes automatically when any transitive dependency's semantic content changes. Dhall stores cache entries at `~/.cache/dhall/1220${HASH}` where entries are CBOR-encoded, alpha-beta-normalized expressions keyed by SHA256.

**Adaptation for Nix requires modifications** for lazy evaluation (complicating "normal form" definition) and IFD (creating build-time dependencies). The recommended approach: compute syntactic hash of source combined with content hashes of all resolved imports, accepting that formatting changes create new cache entries. Dhall benchmarks show **~10% overhead on cold cache** but **3x+ speedup on warm cache** for the cpkg test case.

## Concurrent thunk forcing follows GHC's lock-free pattern

GHC's STG machine demonstrates that **lock-free thunk evaluation achieves only ~6% overhead** versus 50-300% for CAS-based locking. The key insight: for pure computations, accepting rare duplicate evaluation is semantically harmless and dramatically cheaper than synchronization.

**The state machine pattern:**
```
THUNK → GREYHOLE (claimed) → IND (indirection) → value
```

GHC's critical implementation detail: thunks contain an extra "result word" between header and payload, allowing the update sequence to be non-destructive. Write the result pointer first, then write the IND header. On x86, hardware guarantees ordering; on ARM/RISC-V, explicit write barriers are required between these steps.

**Memory ordering requirements for Nix:**
```cpp
// x86: Compiler barriers sufficient
__asm__ __volatile__ ("" : : : "memory");

// ARM64: Full fence required  
__asm__ __volatile__ ("dmb ish" : : : "memory");

// RISC-V
__asm__ __volatile__ ("fence w,w" : : : "memory");
```

The pattern for concurrent forceValue: use atomic header write to claim thunk ownership, allow duplicate evaluation when races occur (GHC observed only ~11 grey-hole entries on average during parallel compilation), and use a global wait queue rather than per-thunk queues to avoid synchronization costs.

## Hash collisions: store full keys despite low probability

For **1 million entries with 64-bit xxHash3**, collision probability is approximately **1 in 37 million**. At 10 million entries, still only ~1 in 370,000. The birthday paradox formula `P(collision) ≈ k²/2^65` yields negligible risk for typical L1 cache sizes.

**Yet every major production caching system stores full keys.** Memcached, Redis, V8, and RocksDB all use the "hash to locate, compare to verify" pattern. The rationale: over billions of lookups, even 10^-8 probability produces incorrect results, and cache poisoning attacks become possible without verification.

**Recommended L1 structure:**
```cpp
struct CacheEntry {
    uint64_t hash;           // xxHash3 for fast lookup
    key_t full_key;          // Full key OR SHA-256 fingerprint
    value_t cached_value;
    metadata_t metadata;
};
```

For collision resolution, **cuckoo hashing** offers guaranteed O(1) worst-case lookup (at most 2 probes) and is used at TikTok scale. Bucketized cuckoo hashing achieves **95%+ load factor** while maintaining worst-case guarantees.

## LMDB integration demands careful transaction management

LMDB delivers **80x faster sequential reads than SQLite** (0.032 μs vs 2.586 μs per operation) and scales linearly with concurrent readers. However, integration with garbage-collected runtimes requires specific patterns.

**Critical configuration:** Always use `MDB_NOTLS` flag when integrating with async runtimes or green threads—it allows reader slots to be tied to transaction objects rather than OS threads. Without this, Go, Python async, and similar patterns break.

**The fundamental challenge with GC runtimes:** LMDB returns pointers directly into memory-mapped pages that become invalid when transactions end. Mozilla's rkv and all major bindings solve this by **copying on return**:
```rust
fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>> {
    let txn = self.env.begin_ro_txn()?;
    match txn.get(self.db, key) {
        Ok(data) => Ok(Some(data.to_vec())), // COPY before txn ends
        Err(lmdb::Error::NotFound) => Ok(None),
        Err(e) => Err(e.into()),
    }
    // txn dropped here, releases reader slot
}
```

**Transaction lifetime is critical:** Long-lived read transactions prevent page reclamation, causing unbounded database growth. Use short-lived transactions with `mdb_txn_reset()`/`mdb_txn_renew()` for high-frequency reads. For Nix's L2 cache with read-heavy workloads, consider `MDB_NOSYNC` or `MDB_NOMETASYNC` since cached data can be regenerated.

## Flake eval cache: complement rather than replace

Nix's existing flake eval cache stores flake output attributes in SQLite databases at `~/.cache/nix/eval-cache-v6/`. Cache keys derive from a fingerprint combining content hashes of all inputs from `flake.lock`. This design works well for remote flakes with stable dependencies but **struggles fundamentally with local development**.

**The core limitations:**
- Operates at CLI level only, not within the evaluator
- Any file change in a dirty repository invalidates the entire cache
- No cross-flake caching between dependent flakes
- Only caches accessed attributes, not shared dependencies

A new memoization system should **operate at sub-expression level** and address local development workflows. The recommended architecture: local L1 in-process cache with daemon-mediated L2, storing in a separate cache directory from the flake eval cache. The existing flake cache's fingerprint infrastructure can be extended, and the SQLite patterns (WAL mode, similar schema structure) should be reused.

## IFD caching requires derivation-type-aware key selection

Import-From-Derivation creates eval→build→eval→build cycles where evaluation blocks entirely while derivations build. The correct cache key depends on derivation type:

| Derivation Type | Cache Key | Validation |
|-----------------|-----------|------------|
| Input-Addressed | `.drv` path hash | Output path exists |
| Fixed-Output | `outputHash` value | Content matches hash |
| CA-derivations | Input hash + realisation mapping | Content hash verification |

**CA-derivations present a critical compatibility issue** (NixOS/nix#5805): output paths are unknown until after build, so strings contain placeholders like `/1ix2zgscfhpnx492z7i2fr62fmipxcnw2ssjrhj0i802vliq88jv`. The cache must maintain a mapping `(drv_hash, placeholder) → actual_output_path` and substitute placeholders before returning to the evaluator.

**IFD caching should be separate from expression caching** due to different invalidation semantics—expression changes versus build output changes—and the fact that IFD results may be garbage collected while expressions remain valid. Register cache entries as GC roots or handle missing outputs gracefully with fallback re-evaluation.

## Property-based testing with RapidCheck validates correctness

Four key properties must be tested for memoization correctness:

**Soundness:** `cached(f(x)) == compute(f(x))` for all inputs
**Idempotency:** Multiple cache lookups return identical results  
**Consistency:** Concurrent access produces consistent results
**Transparency:** Cache presence doesn't affect observable behavior

RapidCheck integrates with Google Test via `RC_GTEST_PROP`. For recursive AST structures, use `gen::lazy` to avoid infinite recursion:

```cpp
namespace rc {
template<>
struct Arbitrary<nix::Expr> {
    static Gen<nix::Expr> arbitrary() {
        return gen::lazy([]() {
            return gen::oneOf(
                gen::map(gen::arbitrary<int>(), nix::mkInt),
                gen::map(gen::arbitrary<std::string>(), nix::mkString),
                gen::lazy([]() {
                    return gen::build<nix::ExprApp>(
                        gen::set(&nix::ExprApp::fn, gen::arbitrary<nix::Expr>()),
                        gen::set(&nix::ExprApp::arg, gen::arbitrary<nix::Expr>())
                    );
                })
            );
        });
    }
};
}
```

Compile with `-fsanitize=thread` for concurrent testing. Salsa tests incremental recomputation through from-scratch consistency checks and explicit memoization verification tracking whether queries were re-executed versus cache-hit.

## Memory pressure response uses platform-specific APIs

**Linux 4.20+** provides Pressure Stall Information at `/proc/pressure/memory` with trigger-based monitoring:
```c
int fd = open("/proc/pressure/memory", O_RDWR | O_NONBLOCK);
write(fd, "some 100000 1000000", 20);  // 100ms stall per second triggers
// Poll fd for POLLPRI events
```

**macOS** uses dispatch sources:
```c
dispatch_source_t src = dispatch_source_create(
    DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
    DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
    dispatch_get_global_queue(0, 0)
);
```

**Boehm GC** provides reactive callbacks through `GC_set_oom_fn` and `GC_set_warn_proc`, plus `GC_get_heap_size()` and `GC_get_free_bytes()` for monitoring.

**Implement tiered response:** moderate pressure shrinks cache to 50%, critical pressure shrinks to 25% and calls `GC_gcollect_and_unmap()`. Use an ARC-inspired algorithm tracking recent (accessed once) versus frequent (accessed 2+ times) entries, with parameter `p` adapting the split based on ghost list hits.

## Daemon integration follows Bazel's multi-layer pattern

The Nix daemon uses a custom binary protocol with 64-bit little-endian integers and length-prefixed byte buffers. Extending for memoization cache requires new worker operations:

```cpp
enum {
    wopQueryMemoCache = 50,      // Lookup cached result
    wopUpdateMemoCache = 51,     // Store result  
    wopBatchQueryMemo = 53,      // Batch lookup
    wopBatchUpdateMemo = 54,     // Batch store
};
```

**Bazel's architecture provides the blueprint:** Content Addressable Storage (CAS) stores blobs by digest, Action Cache maps action digests to results, and persistent workers enable internal caching. Key lesson: **batch operations reduce IPC overhead**—Bazel's `BatchReadBlobs` typically batches 100-1000 blobs per request.

**Recommended cache coherency:** Use content addressing (immutable entries keyed by hash) for lock-free reads, serialize writes through the daemon with SQLite WAL mode for durability. The architecture should be tiered: local L1 in-process for hot data, daemon-mediated L2 for cross-evaluation sharing.

## Dependency tracking adopts Salsa's durability system

Salsa's **durability optimization reduced "mostly no-op" builds from ~300ms to near-zero** in rust-analyzer. The insight: a version vector `(volatile, normal, durable)` allows skipping verification for queries whose durability level exceeds the changed level.

**Assign durability based on expected change frequency:**
- **Low (volatile):** User inputs, frequently changing
- **Medium:** Intermediate computations
- **High (durable):** Rarely changing dependencies (nixpkgs stdlib)

Changing user code (volatile) increments only the volatile counter; queries derived only from durable inputs skip verification entirely.

**The EvalTrace structure should use hash-based verifying traces:**
```rust
struct Trace {
    result_hash: Hash,
    dependencies: Vec<(Key, Hash)>,
    last_verified: Revision,
    last_changed: Revision,
    durability: Durability,
}
```

The rebuild algorithm: verify dependencies lazily on demand, use early cutoff (if recomputation produces identical result, don't propagate), and employ a suspending scheduler that recursively fetches dependencies and suspends on missing ones.

From Acar's self-adjusting computation research: **O(1) overhead per dependency** is achievable. The trade-off between fine-grained (per-value) and coarse-grained (per-file) tracking should start coarse and add granularity as needed—fine-grained tracking showed **8.5x slowdown** for wrapping every integer in 400x400 matrix multiplication, but **10-100x speedups** over naive recomputation for incremental changes.

## Conclusion

The implementation gaps in the Nix memoization plan have well-tested solutions from production systems. The critical patterns: Dhall's semi-semantic hashing for automatic invalidation, GHC's lock-free thunk evaluation accepting rare duplicates, hash-to-locate-compare-to-verify for collision safety, LMDB with MDB_NOTLS and copied returns for GC safety, Salsa's durability levels for minimal verification, and Bazel's batched protocol for daemon efficiency. The recommended approach layers local L1 caching with daemon-mediated L2, complements rather than replaces the existing flake cache, and implements tiered memory pressure response using platform-specific APIs.
```

## Kagi

```markdown
## Round 6: Critique and Updated Plan for Nix Evaluator Memoization

### Executive Summary

The plan has matured significantly through five rounds of iteration. The core architecture (two-level caching, disappearing links, generation counters, Merkle-cached scope hashes) is sound. However, several architectural conflicts and missing details need resolution before implementation:

1. **Lazy vs Eager Hashing Conflict** — Three-phase processing conflicts with the finding that many expressions are never evaluated
2. **Durability Propagation Rules** — Missing concrete implementation for how durability flows through evaluation
3. **L1/L2 Cache Interaction** — Underspecified promotion and coordination between cache tiers
4. **Lazy-Trees Limitation** — Disabling all L2 caching is too conservative
5. **sjsonnet Lesson** — Field memoization at natural boundaries matters more than sophisticated algorithms

---

### Critical Issue 1: Lazy vs Eager Hashing Conflict

**Problem**: The plan proposes three-phase processing (parse → bindVars → computeHashes → intern), but Round 5 answers confirm that many expressions are never evaluated: [^1]

> "Conditional branches not taken, unused let bindings, attribute set fields never accessed... Computing hashes for never-evaluated expressions wastes work."

This creates a conflict: eager `computeHashes()` traversal wastes work on dead code, but Merkle-cached scope hashes require bottom-up computation.

**Resolution**: **Separate AST interning from cache key computation**

| Purpose | When to Hash | Algorithm |
|---------|--------------|-----------|
| **AST Interning** | After `bindVars()` (eager) | Structural hash of syntax |
| **L1 Cache Key** | At evaluation (lazy) | Pointer identity + epoch |
| **L2 Cache Key** | At evaluation (lazy) | Content hash (on-demand) |

**Refined three-phase processing**:

```cpp
// Phase 1: Parse
Expr* parsed = parser.parse(source);

// Phase 2: Bind variables
parsed->bindVars(*this, staticEnv);

// Phase 3: Compute STRUCTURAL hashes for interning (NOT cache keys)
// This is cheap: just syntax structure, no env traversal
parsed->computeStructuralHash(symbols);

// Phase 4: Intern (deduplicates identical syntax)
Expr* interned = exprInternTable.intern(parsed);

// Cache keys computed LAZILY during evaluation:
// - L1: (expr_ptr, env_ptr, epoch) — no hashing needed
// - L2: computeContentHash(expr, env) — only when L1 misses
```

**Key insight**: AST structural hashes (for interning) are cheap because they don't traverse environments. Cache key hashes (for L2) are expensive because they include environment content—compute these lazily.

**Merkle scope hashes** can still be computed eagerly in Phase 3 because they only depend on AST structure (the `attrs` expressions), not runtime environments.

---

### Critical Issue 2: Durability Propagation Rules

**Problem**: The plan defines durability levels but doesn't specify how durability propagates through evaluation. 

**Concrete propagation rules**:

```cpp
// Rule 1: Derived values inherit MINIMUM durability of inputs
Durability derivedDurability(const std::vector<Durability>& inputs) {
    return *std::min_element(inputs.begin(), inputs.end());
}

// Rule 2: Primops assign durability at source
void prim_readFile(EvalState& state, Value& v, const Path& path) {
    v.mkString(readFile(path));
    
    if (isStorePath(path)) {
        state.markDurability(v, Durability::HIGH);  // Immutable
    } else {
        state.markDurability(v, Durability::MEDIUM);  // Can change
    }
}

void prim_getEnv(EvalState& state, Value& v, const std::string& name) {
    v.mkString(getenv(name.c_str()) ?: "");
    state.markDurability(v, Durability::LOW);  // Volatile
}

// Rule 3: Function application propagates durability
Value* apply(Value* fn, Value* arg) {
    Value* result = doApply(fn, arg);
    result->durability = std::min(fn->durability, arg->durability);
    return result;
}
```

**Storage**: Durability can be stored in a side table (not in `Value` struct, which is fully packed):

```cpp
// Global durability tracking (weak map)
std::unordered_map<Value*, Durability> durabilityTable;

void markDurability(Value* v, Durability d) {
    durabilityTable[v] = d;
    GC_general_register_disappearing_link(
        reinterpret_cast<void**>(&durabilityTable[v]), v);
}

Durability getDurability(Value* v) {
    auto it = durabilityTable.find(v);
    return (it != durabilityTable.end()) ? it->second : Durability::HIGH;
}
```

**Default durability**: HIGH (pure computations are maximally durable).

---

### Critical Issue 3: L1/L2 Cache Interaction

**Problem**: The plan describes two cache levels but doesn't specify their interaction. [^1]

**Concrete interaction rules**:

```cpp
Value* memoizedEval(Expr* expr, Env* env, EvalState& state) {
    // Step 1: L1 lookup (fast, pointer-based)
    IdentityKey l1Key{expr, env, globalEpoch};
    if (auto* cached = l1Cache.lookup(l1Key)) {
        stats.l1Hits++;
        return cached;
    }
    
    // Step 2: L2 lookup (slower, content-based)
    // Only if expression is content-cacheable
    ContentKey l2Key;
    bool l2Eligible = isContentCacheable(expr, env);
    if (l2Eligible) {
        l2Key = computeContentHash(expr, env);
        if (auto* cached = l2Cache.lookup(l2Key)) {
            stats.l2Hits++;
            // PROMOTE to L1 for future fast lookups
            l1Cache.insert(l1Key, cached);
            return cached;
        }
    }
    
    // Step 3: Evaluate
    Value* result = realEval(expr, env);
    
    // Step 4: Populate L1 (always)
    l1Cache.insert(l1Key, result);
    
    // Step 5: Populate L2 (if eligible and result is cacheable)
    if (l2Eligible && isResultCacheable(result)) {
        l2Cache.insert(l2Key, result, getDurability(result));
    }
    
    return result;
}
```

**Promotion policy**: L2 hits are promoted to L1 for subsequent fast lookups within the same evaluation.

**Eviction coordination**: L1 and L2 have independent eviction policies:
- L1: LRU with disappearing links (intra-eval lifetime)
- L2: Durability-based TTL (cross-eval persistence)

---

### Critical Issue 4: Lazy-Trees Limitation

**Problem**: Decision 27 disables ALL L2 caching when lazy-trees is enabled due to `StorePath::random()` non-determinism. [^1] This is too conservative—it throws away cross-eval caching entirely.

**Refined approach**: Disable L2 caching only for values that **contain** lazy-tree paths:

```cpp
bool isContentCacheable(const Value& v, const EvalSettings& settings) {
    if (settings.experimentalFeatures.lazyTrees) {
        // Check if value contains any lazy-tree virtual paths
        if (containsLazyTreePath(v)) {
            return false;  // Only exclude values with virtual paths
        }
    }
    // ... other checks
    return true;
}

bool containsLazyTreePath(const Value& v) {
    switch (v.type()) {
        case nPath:
            return isLazyTreeVirtualPath(v.path());
        case nString:
            return stringContextContainsLazyTree(v.context());
        case nAttrs:
            for (auto& [name, val] : *v.attrs())
                if (containsLazyTreePath(*val)) return true;
            return false;
        // ... other types
    }
}
```

**Benefit**: Most values don't contain lazy-tree paths, so L2 caching remains effective for the majority of evaluations.

---

### Lesson from sjsonnet: Natural Boundaries Matter

sjsonnet achieved 30-60x speedup over google/jsonnet primarily through **field memoization** (caching object fields) and **parse caching**, not sophisticated algorithms. [^1]

> "To remedy the issues that were causing poor performance in the google/jsonnet interpreter, Sjsonnet caches object fields to avoid unnecessary re-computation."

**Application to Nix**: Nix already does field memoization correctly (attrs store `Value*`, not `Expr*`). The lesson is that **caching at natural boundaries** (imports, function applications, attribute access) provides most of the benefit.

**Recommended priority order**:

1. **Import caching** (file-level) — highest impact, natural boundary
2. **Function application caching** — second highest, captures repeated calls
3. **Thunk-level caching** — fine-grained, diminishing returns

The current plan focuses heavily on thunk-level caching. Consider adding explicit import-level caching as a Phase 2 priority.

---

### Updated Implementation Plan

#### Phase 1: Correctness (Blocking) — No Changes

All Phase 1 tasks remain as specified. The `with` chain hashing fix, cycle detection, and GC safety are prerequisites.

#### Phase 2: Performance (Refined)

| Task | Priority | Change from Round 5 |
|------|----------|---------------------|
| **Import-level caching** | **High** | **NEW**: Add explicit file-level cache |
| Implement two-level caching | **High** | Unchanged |
| Add xxHash3 for L1 cache | **High** | Unchanged |
| **Lazy L2 hash computation** | **High** | **CHANGED**: Compute content hashes on-demand, not eagerly |
| Three-phase AST processing | **High** | **CLARIFIED**: For interning only, not cache keys |
| Merkle-cached scope hashes | **High** | Unchanged |
| ARC-inspired cache eviction | **High** | Unchanged |
| Closed-term optimization | **High** | Unchanged |
| **L1→L2 promotion logic** | **Medium** | **NEW**: Explicit promotion on L2 hit |

#### Phase 3: Architecture (Refined)

| Task | Priority | Change from Round 5 |
|------|----------|---------------------|
| **Durability propagation rules** | **High** | **NEW**: Concrete implementation |
| **Durability side table** | **High** | **NEW**: Weak map for tracking |
| Salsa-style durability levels | **High** | Unchanged |
| **Refined lazy-trees handling** | **Medium** | **CHANGED**: Per-value check, not global disable |
| Early cutoff | **Medium** | Unchanged |
| Position-independent mode | **Medium** | Unchanged |

#### Phase 4: Cross-Eval (Unchanged)

All Phase 4 tasks remain as specified.

---

### Questions for the Codebase Agent (Round 6)

#### Lazy Hashing

1. **How expensive is `computeContentHash()` currently?** What percentage of evaluation time is spent computing content hashes for L2 cache keys?

2. **Is there a fast path for detecting "obviously uncacheable" expressions?** (e.g., expressions containing `builtins.currentTime`) before computing full content hash?

3. **Can we short-circuit content hash computation for closed terms?** If an expression is closed (no free variables), its hash doesn't depend on the environment.

#### Import Caching

4. **Where is `builtins.import` implemented?** Specifically, where would import-level caching be inserted?

5. **How does the current flake eval cache interact with `builtins.import`?** Can we reuse any of that infrastructure?

6. **What is the typical import graph depth for Nixpkgs?** This affects the potential benefit of import-level caching.

#### Durability

7. **Is there existing infrastructure for tracking value provenance?** Something that records "this value came from reading file X"?

8. **How would durability interact with `builtins.seq` and `builtins.deepSeq`?** These force evaluation but shouldn't change durability.

9. **Should durability be stored in the Value struct or a side table?** The Value struct is fully packed, but a side table adds indirection.

#### Lazy-Trees

10. **How are lazy-tree virtual paths represented?** Is there a reliable way to detect them?

11. **What percentage of values in a typical evaluation contain lazy-tree paths?** This determines the impact of per-value checking vs. global disable.

12. **Can lazy-tree paths be "resolved" to content hashes before caching?** This would make them cacheable.

#### Cache Coordination

13. **Should L1 and L2 share eviction pressure?** If L1 is full, should we evict to L2 instead of discarding?

14. **How should cache statistics be exposed?** Via `NIX_SHOW_STATS`, a separate flag, or always-on counters?

15. **Is there a natural "evaluation boundary" where we should flush L1 to L2?** (e.g., after evaluating a top-level attribute)

---

### Summary of Key Changes from Round 5

| Round 5 Plan | Round 6 Change | Rationale |
|--------------|----------------|-----------|
| Eager `computeHashes()` for cache keys | **Lazy content hash computation** | Many expressions never evaluated |
| Durability levels defined | **Concrete propagation rules** | Missing implementation details |
| L1/L2 caches described | **Explicit interaction rules** | Promotion, coordination underspecified |
| Disable all L2 with lazy-trees | **Per-value lazy-tree check** | Too conservative |
| Thunk-level focus | **Add import-level caching** | sjsonnet lesson: natural boundaries matter |

The most impactful changes are:
1. **Lazy content hash computation** — avoids wasted work on dead code
2. **Import-level caching** — highest ROI based on sjsonnet experience
3. **Durability propagation rules** — enables differential invalidation

[^1]: [round-5-answers](round-5-answers.md) (100%)
```