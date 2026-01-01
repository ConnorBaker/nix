# Prompt

This is the prompt which spawned the responses in Responses:

```text
I'm currently working on enabling memoization in the Nix evaluator. Please read the attached file to understand the goal, the current status, and the current implementation plan. Then, research the current state of the art for achieving the goal and critique the plan. Suggest potential problems, alternative implementations, or improvements. Create an updated version of the plan and optionally include as many questions as you like about the Nix codebase. The updated plan and your questions will be provided to an agent with access to the codebase and the ability to reason about it, so you can task it to help guide your research.
```

Please read all responses. You should fetch and read the sources referenced to ensure accuracy. You should also list those sources. Then, update your plan where appropriate, being sure to include alternative designs or implementations and their tradeoffs, as well as additional references. Lastly, answer any questions posed by any of the responses in a new file, round-1-answers.md.

Since there are a lot of steps here, please make a todo list and use it to keep track of your finished work and remaining work as you proceed.

# Responses

## ChatGPT

```text
Critique of the Current Design

The staged Nix memoization prototype relies on structural hashing of thunks (closure code + environment) and caching based on that hash ￼.  However, several correctness and architectural issues have been identified.  For example, the current expression hash fails to include symbol names for with-bound variables, so with {x=1; y=2;}; x and with {x=1; y=2;}; y currently hash the same despite evaluating to different values ￼.  Similarly, hashing thunks by recursively hashing their contents is order-dependent: if two closures differ only by which sub-thunk has been forced, they will hash to different values ￼.  These bugs can cause incorrect cache hits or misses.

Other correctness risks include missing impurity signals: some primops (e.g. unsafeGetAttrPos) return dynamic information (source positions) without marking themselves impure, so caching them could yield stale or wrong results ￼.  Cyclic values from rec expressions are not handled by the serializer, leading to infinite loops ￼.  More generally, the design conflates intra-evaluation caching (within one nix eval) with cross-evaluation (persistent) caching ￼.  This single predicate (currently valueIsUncacheable()) is too coarse: for example, a closure (lambda) is cacheable during one run but not content-portable to another run (its captured Env* pointer is meaningless across processes) ￼.

Performance and integration are also concerns. Structural hashing of deep values or large environments can be expensive, and naïvely using pointer identity is fragile: Boehm GC may reuse addresses after collection.  Hash-caching strategies (pointer key vs content key) present trade-offs: pointer keys are fast and stable in one run but nonportable, while content keys are correct but may be expensive or cyclic ￼.  Symbol interning adds complexity: Nix’s symbol IDs are session-local ￼, so two runs would never agree on a symbol ID. Finally, the current prototype lacks a clear strategy for persistent storage: no backing store or serialization format is yet defined beyond an in-memory hashmap, making cross-run or cross-machine sharing impossible in practice.

Figure: Key issues in the current Nix memo design. Content-based hashes must be lazy-safe (never force thunks) ￼, yet also account for merged attrsets and string contexts; cycles must be detected (or rejected) to avoid infinite loops ￼; and pointer-based schemes must guard against GC reuse. (The table below summarizes major issues noted in the prototype.)
	•	Variable scoping: with-bound names were omitted from the hash ￼, causing collisions.
	•	Environment hashing: The thunk cache key uses a hash of (expr_hash, env_hash), but env_hash changes if any contained thunk is forced ￼.
	•	Impurity: Builtins like unsafeGetAttrPos are not marked impure ￼, so cached results may omit dynamic data (e.g. file positions).
	•	Alpha-equivalence: Nix uses de Bruijn indices to handle α-renaming ￼, which is good for lambda binders, but dynamic scopes (e.g. names in a with) need special handling (the proposed fix is to include the name in the hash ￼).
	•	Pointer reuse: Hashing by pointer (address) is fast but non-deterministic across runs and unsafe if GC relocates or reuses memory.
	•	Symbol table: Symbol IDs differ per EvalState ￼, so any key containing a symbol_id is not portable.
	•	No cycle check: Cyclic values cause serialization or hashing to never terminate ￼.

In summary, the current design has demonstrated correctness bugs (hash misses/hits due to scope/impurity issues) and architectural challenges (mixing intra vs cross-eval, GC/pointer safety, persistent storage).  These must be addressed before a reliable persistent memoization can be deployed.

Related Work and State of the Art

Many systems explore techniques for memoization, content-addressing, or AST sharing. Key examples include:
	•	Content-addressed code (Unison):  Unison identifies every function by the hash of its AST ￼.  Dependencies are likewise replaced by their hashes ￼, so code is globally immutable and shareable.  This ensures that semantically identical definitions (up to α-renaming) map to the same name.  Nix could adopt a similar strategy: hash each ExprLambda with de Bruijn normalization (like it already does ￼) so that equivalent functions get the same key.
	•	GHC / Lazy languages: In Haskell’s GHC runtime, thunks are updated in place on evaluation, so that repeated forces reuse the computed result.  This is a form of memoization internal to one process.  GHC also has Constant Applicative Forms (CAFs) that are shared globally within a program.  However, GHC does not provide a persistent, cross-process cache of arbitrary expression results (beyond interface file hashing for builds).  The lesson is that in-place updates (like Nix’s blackholing strategy ￼) naturally avoids duplicate work, but they vanish when the process ends.  For long-term caching, external stores are needed.
	•	Nominal Adapton (Foster et al., OOPSLA 2015):  Standard incremental systems (including Adapton ￼) use structural comparison: references/pointers are compared by address.  This is fast but brittle: as Foster et al. note, a single new pointer can cascade and cause missed reuse ￼.  Nominal Adapton instead assigns names to computations.  Programmers can give a deterministic name to each subcomputation so that the system knows which results correspond to which inputs ￼.  In our context, this suggests a hybrid: we could let the Nix evaluator optionally tag thunks or closures with a stable name (e.g. a GUID or a hash of some canonical form) and use that in the cache key.  Unlike raw pointers, these names can survive GC and even program restarts if based on content.
	•	Build Systems (Dune, Bazel, Shake):  Systems like Dune ￼ and Bazel ￼ ￼ maintain persistent caches of build results.  Bazel, for instance, hashes each “action” (build step) and its declared inputs.  If one machine has already built a target with those inputs, a second machine can pull the outputs from a remote cache ￼.  The outputs themselves are stored in a content-addressable store (CAS) keyed by their file hashes ￼.  Similarly, Dune uses a shared cache across workspaces to skip running unchanged build rules ￼.  The takeaway is a two-level cache: an action key (analogous to our thunk key) and a CAS for outputs.  For Nix evaluator caching, we could treat each forced thunk as an “action” and the computed Value as the output.  The persistent backend might be a SQLite or Git-based CAS, where each value is stored and retrievable by its hash.
	•	Functional Package Managers (Nix/Guix):  These systems themselves use on-disk memoization.  In Nix (and Guix) every derivation (build action) is hashed with all its inputs and placed in /nix/store so that identical inputs yield the same directory ￼.  This is effectively a build-time cache: if an identical build has run elsewhere, the output can be substituted (and GC’d) like any other store item ￼.  However, this mechanism only caches fully realized derivations, not intermediate language values.  Our goal goes further: caching Nix expression evaluation results themselves across runs.  Nonetheless, the success of Nix’s content-addressed store confirms that a cryptographic hash of inputs can safely name outputs on disk ￼.
	•	Persistent Data Structures and Caches:  More broadly, persistent (immutable) data structures in functional languages rely on structural sharing to avoid copies【19†】.  Memoization libraries (e.g. Data.Memoize in Haskell or memoizee in JS) offer optional persistence via files or databases.  The Crux concept (Guet et al., NSDI 2011) uses content hashes to dedupe build artifacts across machines.  The unifying theme is content-based identifiers: if we can express a Nix thunk’s identity purely in terms of its code and input values, we can reuse its result anywhere.

Summary of Lessons:  From this survey, important ideas emerge:
	•	Content-addressing: Use cryptographic hashes of AST/value content (α-normalized) as cache keys ￼ ￼.
	•	Two-level caching: Like Bazel, combine a fast in-process key (maybe pointer or cached hash) with a slower global key (full content) ￼.
	•	Naming: Consider explicit naming (Nominal Adapton) to stabilize identities and avoid pointer fragility ￼.
	•	Pure functions: Ensure cached expressions are truly pure in context (track all impurity sources such as currentTime or path environments).
	•	Cycle handling: Decide whether to forbid cyclic values or support a DAG encoding (e.g. with back-references).

Suggestions and Alternative Designs

Based on the above, we propose the following refinements and alternatives for Nix evaluator memoization:
	1.	Separate Intra- vs Cross-Evaluation Caches:  Define two predicates, e.g. isCacheableIntraEval(v) and isCacheableCrossEval(v), instead of one ￼【2†L125-133】.  Intra-eval caching can safely include closures and even path values as long as the same SourceAccessor* is used.  Cross-eval caching must be more strict: closures should typically be excluded (an Env* is not reproducible), and paths must be reduced to content-addressed references (see below).  This unifies approach B from the doc ￼, at the cost of a second code path, but avoids leaking non-portable keys into the persistent cache.
	2.	Hybrid Keying (Pointer + Content):  Implement a two-layer key (option E in [2]) ￼.  For an active thunk, use (ExprPtr, EnvPtr) as a fast hash to check the in-memory cache (intra-eval).  If that lookup misses, fall back to a content-based hash (expr_hash, env_fingerprint, args_hash).  The content key is more expensive but portable.  A similar strategy is used by e.g. GHC (pointer compare, then structural compare) or by “string-interner” caches.  This avoids recomputing content hashes on every force while still supporting cross-run lookup.
	3.	Canonicalize Environments:  Instead of hashing an Env* pointer chain directly, compute an environment fingerprint that is order-independent.  For example, flatten the lexical chain of an environment into a sorted list of (varName, valueHash) pairs (lexically sorted by name or de Bruijn index) and hash that.  This makes closures with the same bindings produce the same key, regardless of creation order.  It also naturally ignores unused slots: if a variable was captured but never used, it could be omitted from the fingerprint (since it shouldn’t affect the function).  Care must be taken that generating this fingerprint does not force thunks in the environment (see next point).
	4.	Lazy-Safe Hashing:  Any hashing routine must refuse to traverse or force thunks.  The common solution is: if a sub-value is a thunk or tApp, stop hashing and mark the whole key as uncacheable ￼.  Equivalently, maintain the invariant that cache keys contain only values already in WHNF.  As the ChatGPT report notes: forcing a thunk to compute its hash would break laziness and can lead to unintended evaluation or side-effects ￼.  In practice, this may mean we only cache calls whose argument structure has no deferred parts, or use a sentinel hash (e.g. zero) to denote “do not cache” when encountering a thunk.
	5.	Handle α-Equivalence Correctly:  The existing hashing uses de Bruijn for lambda-bound variables ￼, which is good.  The missing piece was dynamic scopes (with).  We should incorporate the actual symbol name for with-bindings as recommended ￼.  More generally, any binder not tracked by de Bruijn must feed its name into the hash to distinguish cases that would otherwise collide.  Alternatively, we could canonicalize with {x=…} into an AST form where each binding is explicitly named in the hash.
	6.	Path Values and Accessors:  Paths carry a SourceAccessor* plus content.  For cross-machine reuse, we must use content-only keys.  We should replace the pointer (accessor identity) in the hash with a fingerprint of the actual content (e.g. a SHA256 of the file or git-tree).  This matches “resolving” the path: two path values that point to the same file content but via different mounts must hash the same for portability.  (Within one eval, we could allow same-accessor pointer optimization.)
	7.	Impurity and Deterministic Inputs:  Any side-effects or implicit inputs (current time, environment variables, impurity tokens) must be included in the key or marked “uncacheable.”  For cross-eval caching, currentTime should probably be treated as an unstable input: either always treat it as impurity (never cache calls that involve it) or include a user-defined “seed” for it.  Similarly, Nix’s impureToken should be combined with the key so that two calls separated by a trace get different keys.  The implementation should audit all primitives: those that depend on external state (time, environment, random, file system state beyond SourceAccessor, etc.) must set valueIsUncacheable().
	8.	Cycle Detection or Rejection:  Because Nix values can be cyclic (via rec attrsets or lists), serialization or hashing must not loop forever.  The simplest safe choice is to reject cycles at persistent serialization: if the cache encoder sees a back-edge, it throws an error (Option A from [2]) ￼.  This means cyclic values simply won’t be stored cross-eval; callers should detect this and disable caching for that value.  An alternative (Option B) is to use back-references (like pickle) ￼, but that complicates the on-disk format.  For now, rejecting cycles is safer, especially since many derivations and data in Nix are acyclic by nature.
	9.	Persistent Backend (SQLite/Git/Store):  For cross-evaluation caching we need a durable store of (key → value).  A SQLite database is a reasonable choice (fast lookups, ACID, easy to ship).  Each entry could store the key (as blob of 256-bit hash) and a serialized Value* (perhaps in NAR or JSON form).  For large results (big lists or strings), consider storing them in the Nix store or Git and keeping only a reference in SQLite.  Another idea: emulate Bazel’s CAS by making each cached value an immutable store path keyed by the same hash, and store the path in SQLite.  This leverages Nix’s existing content store ￼.  If using Git, one could commit each value (or its derivation) under its hash, though serialization format needs care.  In any case, the backend must be deterministic and avoid pointers – e.g. use canonical stable names for objects.
	10.	Garbage Collection & Interning:  To avoid dangling keys or GC hazards, we should intern values when caching: once a value is stored in the persistent cache, it should not be reclaimed by GC (at least until it is expired from the cache).  This could be done by keeping a strong reference from the cache to the value.  Conversely, when an entry is evicted (e.g. by cache size limit), we can drop that reference so it can be collected.  This ensures pointer addresses in keys remain valid as long as needed.
	11.	Symbol Table Unification:  The C-API issue (Decision 5) highlights that Symbol IDs are EvalState-local ￼.  For cross-run caching, we must treat a symbol by its name text, not by its numeric ID.  Concretely, any cache key that currently includes a symbol ID (e.g. as part of attribute lookup) should instead include the string.  Alternatively, one could unify symbol tables across runs (difficult) or bake a global generation counter into symbols ￼.  At minimum, we should forbid using raw symbol IDs in cross-eval keys.
	12.	Testing and Validation:  Add regressions for all corner cases: cycles, with scope, position-sensitive builtins, impure operations, etc.  For example, verify hashing distinguishes with {x=1; y=2;}; x vs. ...; y ￼, that forcing order doesn’t change the key of unforced thunks, and that cache hits always correspond to structurally equal values (e.g. do a full == on collision).

By combining these strategies, we can make memoization correct (no false hits/misses), safe (not break GC or laziness), and portable (identical runs share results).  In particular, moving to content-addressed keys (with carefully curated exceptions) and providing a persistent store aligns with proven designs in build systems and content-based languages ￼ ￼ ￼.

Revised Implementation Plan

We outline a multi-phase plan incorporating the above recommendations, with concrete tasks and questions for the Nix codebase.
	•	Phase 1: Fix Correctness Bugs (Immediate)
	•	Fix with-binding hash: Include each with-binder’s name in the hash stream as proposed ￼.  Test: assert(hash(with {x=...}; x) != hash(with {x=...}; y)).*
	•	Cycle detection: Implement a visited set during serialization/hashing so that recursive values trigger an error (Option A) ￼.  Test: hashing a rec attrset fails.
	•	Mark unsafeGetAttrPos impure: Add ImpureReason::PositionDependent in valueIsUncacheable() ￼.  Test: caching a function returning __curPos should not hit.
	•	Symbol key inclusion: Ensure expression hashes (e.g. for functions) include all relevant metadata: e.g. for import expressions, hash the content ID (path fingerprint) instead of a pointer【1†L95-100】.
	•	Test suite: Add test cases for all fixed bugs and edge cases mentioned above (empty closures, impure calls, etc.).
	•	Phase 2: Split Cacheability and Improve Hashing (Major Architecture)
	•	Two cache predicates: Split valueIsUncacheable() into isValueCacheableIntraEval(v) and isValueCacheableCrossEval(v) ￼. Use the intra-eval predicate for the in-memory hash table, and the cross-eval predicate for persistent caching decisions.
	•	Environment fingerprinting: Replace env_hash with a canonical fingerprint: iterate over bound variables (via Bindings::iterator) in deterministic order, concatenating their symbol name and the child value’s cached hash (if forced) or a sentinel if unforced. Alternatively, for cross-eval hashing, require that all captured values be themselves cacheable and hashed. This addresses the ordering issue ￼.
	•	Pointer vs content key: Implement a hybrid key (option E from [2]): in the fast path, use (expr_ptr, env_ptr) to lookup the cache (stable for this run) ￼. On a miss, compute the full content hash (expr_hash, env_hash, args_hash).  This gives quick hits for repeated forcing of the exact same thunk object, while still enabling content-based lookup for others.
	•	Alpha/β normalization: Verify that the existing de Bruijn hashing covers all lambda-bound scopes ￼. Ensure that any additional binding forms (e.g. let/rec) are incorporated in expr_hash. For with, include the symbol name as noted.  If necessary, canonicalize ASTs (e.g. sort attribute sets by key) when hashing.
	•	String context: Ensure string values include their entire dependency context array in the hash ￼. This may require hashing each context element’s store path/fingerprint in order.
	•	GC safety: Avoid using raw pointers in persistent keys. If pointers are used for intra-eval keys, guard them by storing an object identity (e.g. a unique integer tag) that survives GC.  Boehm GC allows assigning IDs via GC_call_with_alloc_lock or using weak pointers. Alternatively, skip pointer-based keys if too hard, and rely only on content hashes (at some performance cost).
	•	Phase 3: Persistent Cache Backend (Cross-Eval Storage)
	•	SQLite integration: Embed a SQLite (or RocksDB) store in the evaluator. Define tables for (key: BLOB, value: BLOB) and for managing indices or eviction. Use a fixed key format, e.g. first 32 bytes for an action-hash, next for an expr-hash.
	•	Value serialization: Decide on a portable format (e.g. NAR or JSON). Serialization must be acyclic (enforced from Phase 1) and include enough type tags to reconstruct Value (null, bool, int, string with context, list of child IDs, etc.).  Possibly use existing value-serialize.cc with the cycle check. Ensure the format is architecture-independent.
	•	Integration with Nix store: For large or complex values (e.g. large strings or closures), consider writing the serialized BLOB to the Nix store under its hash.  Then the SQLite entry can just record the store path.  This piggybacks on Nix’s existing content-addressable storage ￼.  However, caution: values can contain closures or runtime-only data, so only use store for pure outputs (like fully-realized outputs).
	•	Cache schema: Include in the key any EvalInputs that affect purity: NIX_PATH sources, impure token state, current time (if treated as stable seed), etc.  (Decision 3 suggests hashing imports by content fingerprint ￼.)  Also, make a field for “cache version” so future changes can invalidate old entries.
	•	Testing cross-run: After implementing persistent store, test simple reuse: e.g. run nix eval on a pure function twice with the same inputs and verify the second run reads from the SQLite cache instead of recomputing (we can detect by adding a debug print in the forcing code).
	•	Phase 4: Cross-Machine and Advanced Sharing (Optional/Nice-to-have)
	•	Remote caching/Git: For multi-machine sharing, expose a way to sync the SQLite DB or CAS between machines (e.g. via a Git repo or a networked DB).  This could piggyback on Nix’s existing mechanisms (e.g. flake caches, which already fetch NARs by URL).
	•	Crash recovery: Ensure the cache is safe against crashes or partial failures. Use transactions in SQLite or robust append-only logs.
	•	Eviction/GC: Implement a policy to trim the persistent cache (e.g. LRU or size limit).  This prevents unbounded growth. Provide a tool or flag to prune or back up the cache.
	•	User control: Add options to disable caching for specific builds or modules (similar to Dune’s enabled-except-user-rules ￼) to avoid subtle breaks in ill-behaved code.
	•	Technical Questions for Exploration
	1.	Value identity: How to generate a stable ID for a Value* that survives GC?  Should we add an extra field (like a UUID or counter) to Value objects?  Or use GC_malloc_atomic zones to track them?
	2.	Hash collision defense: If we rely on 256-bit hashes, collisions are unlikely but possible. Should we implement a final structural equality check on cache hits to verify the values really match?  That entails a lazy-safe equivalence function.
	3.	Serialization format: Is the current NAR-based serializer adequate for caching arbitrary Values?  It may need extension to encode functions/thunks (likely by hashing the code pointer and skipping the env), or we may decide only to serialize WHNF results (no thunks).
	4.	Impurity tokens: Should the cache key include impureToken?  One risk: a trace call might increment it, changing the environment for subsequent calls.  If two runs have different side-effect order, should they be considered distinct?
	5.	currentTime and side effects: currentTime is constant per run but not cross-run.  Should we add a pseudo-argument (like an “EvalSeed”) that is hashed for each run?  Or simply mark any expression depending on time as uncacheable.
	6.	Partial forcing vs caching: When a thunk is forced, it partially updates the Value.  If that updated state is cached, what if another reference to the unforced version tries to use it?  We need to ensure caching only happens after a successful full evaluation to WHNF, or use a locking protocol.
	7.	Thread safety: The current evaluator is single-threaded, but if ever parallelized, we must guard cache mutations.  If not, we can skip locking.
	8.	Symbol tables: Could we embed a pointer to the symbol table in each nix_symbol (per Decision 5) ￼?  Or is it easier to disallow sharing closures/functions across EvalStates entirely?
	9.	Integration with existing Nix features: How will memoization interact with flakes (which have their own cache) or with NAR flattening?  For example, will nix flake eval --json skip the new cache?
	10.	Backward compatibility: Since we assume experimental mode, we can break cache formats and keys freely.  But how do we enable/disable caching at runtime?  A command-line flag --cache with modes (off, in-memory, persistent) might be needed.

In summary, the path forward is to make the Nix evaluator’s memoization both sound and practical.  We must combine content hashing (with α-renaming support ￼), careful impurity tracking, and a robust storage layer (e.g. SQLite/CAS) to achieve portable persistent caching.  By learning from systems like Unison ￼ and Nominal Adapton ￼, and by fixing the specific bugs noted above ￼ ￼, we can evolve the prototype into a resilient cache.  This will allow repeated Nix evaluations – even across machines – to share results, greatly speeding up large and repetitive Nixpkgs instantiations.

Sources: Based on the Nix memoization design document ￼ ￼ ￼ and literature on incremental computation and build caching ￼ ￼ ￼ ￼.
```

## Claude

```text
# Memoization in Lazy Evaluators: Critiquing the Nix Caching Plan

The proposed three-phase Nix evaluator memoization plan addresses a genuinely hard problem—caching in lazy functional languages where evaluation order affects observable state. This report synthesizes state-of-the-art research across ten critical dimensions, revealing that while the plan's foundations are sound, several design choices warrant reconsideration based on lessons from GHC, rust-analyzer, Adapton, and production build systems.

## Thunk identity requires syntax-based, not pointer-based, hashing

The plan's central challenge—that environment slots contain thunks whose "content" changes when forced—has a well-established solution. GHC's `System.Mem.StableName` module provides O(1) pointer-based identity, but with a **critical limitation**: `makeStableName` may return different stable names before versus after forcing a thunk. The documentation explicitly warns that thunk forcing changes memory representation, making pointer identity unstable.

Eelco Dolstra's own "Maximal Laziness" paper (2008) provides the canonical solution for Nix: **hash-cons all syntax terms using the ATerm library**, where syntactically equal terms occupy identical memory locations. Cache lookup becomes O(1) pointer equality, and the cache maps expressions to normal forms rather than tracking thunk identity. The key insight is that Nix terms are always closed at memoization points, enabling "a straight-forward, substitution-based reduction scheme" that achieves maximal sharing without closure updating.

The proposed `(expr_ptr, env_ptr)` scheme is **unsound** with standard thunk-in-place updates. Three alternatives exist: (1) indirection cells maintaining stable pointers to thunk/value pairs, (2) Nominal Adapton's first-class names that survive structural changes, or (3) content-addressed hashing on expression syntax. Given Nix's existing ATerm heritage, option 3 aligns best with proven architecture.

## The `with` expression fix is necessary but incomplete

Adding the symbol name to the hash correctly distinguishes `with {x=1; y=2;}; x` from `with {x=1; y=2;}; y`. However, nested `with` expressions reveal **additional incompleteness**. Consider `with a; with b; x` where both `a` and `b` define `x`—the result depends on evaluation order and shadowing rules, not just which symbol is accessed.

Research on dynamic scoping in Emacs Lisp and Common Lisp confirms that memoization fundamentally conflicts with dynamic binding because the binding value depends on the runtime call stack. The complete fix requires hashing **the ordered chain of `with` scopes** that could provide each symbol, not just the symbol name. Specifically:

- Track each `with` scope's identity (hash of the attribute set)
- Maintain the ordered list of enclosing `with` scopes for each lookup
- Include both the symbol name AND the scope chain in cache keys

Languages that successfully memoize with dynamic scoping do so by separating lexical and dynamic namespaces, allowing caching of lexical computations while treating dynamic lookups as inherently volatile.

## Cycle rejection may be viable if evaluated values are acyclic

The plan proposes rejecting cycles (Option A) for cross-evaluation serialization. This initially seems overly restrictive given Nix's pervasive `rec` patterns, but a crucial distinction emerges: `rec { a = 1; b = a; }` creates self-referential *bindings during evaluation* but produces the acyclic value `{ a = 1; b = 1; }` after evaluation.

Production serialization systems fall into three camps:

| Approach | Systems | Trade-off |
|----------|---------|-----------|
| Full cycle support via memo IDs | Python Pickle, OCaml Marshal | Automatic but format-specific |
| DAGs only, cycles via indices | FlatBuffers, Cap'n Proto | Fast but requires indirection |
| Reject cycles entirely | Protobuf | Simple but restrictive |

If Nix evaluation truly produces acyclic *values* (not just acyclic intermediate structures), rejection is sound. However, patterns like `let x = { y = x; }; x` do create cyclic values. The recommended approach is **detect-and-encode**: use ID-based references (JSON-LD `@id`/`@ref` style) when cycles exist, falling back to direct serialization for acyclic structures. This matches OCaml Marshal's opt-in sharing, which preserves cycles by default but allows `No_sharing` for known-acyclic data.

## Salsa's durability model outperforms separate predicates

Splitting `valueIsUncacheable()` into two predicates addresses a real need—distinguishing intra-evaluation volatility from cross-evaluation persistence. However, rust-analyzer's Salsa framework demonstrates a **more elegant unified model** with durability levels.

Salsa uses a **version vector** rather than a single version number. For three durability levels (volatile, normal, durable), the version is a tuple `(v_low, v_med, v_high)`. Incrementing a durability component also increments all less-durable components. When validating cached queries, Salsa checks only the relevant durability-specific version—queries depending solely on "durable" inputs (standard library code) skip revalidation when only "volatile" inputs (user code) change.

The durability model provides **300ms savings** in rust-analyzer by avoiding stdlib revalidation on user edits. For Nix, this maps naturally to:

- **HIGH durability**: Nixpkgs standard library, rarely-changing channels
- **MEDIUM durability**: User flakes, project dependencies  
- **LOW durability**: Local development files, impure inputs

A unified model with capability flags (`CacheLevel::IntraEval | CrossEval | CrossMachine`) is superior to separate predicates because durability is orthogonal to storage location. The same durability annotation controls both in-memory and persistent cache behavior.

## Position tracking needs architectural separation from evaluation

Marking `unsafeGetAttrPos` as impure is correct but blunt. rust-analyzer's architecture reveals a superior approach: **separate position-independent semantic caching from on-demand position computation**.

The rust-analyzer blog explicitly warns: "AST computation 'shields' the code higher in the stack from changes in source code. That is, of course, if you don't store positions in the AST." Their solution uses `AstId`—position-independent identifiers for AST nodes—with a separate `AstIdMap` query that translates to actual positions when needed.

For Nix, this suggests a two-layer architecture:

1. **Semantic layer (cacheable)**: Evaluation results using position-independent attribute identifiers
2. **Position layer (computed on-demand)**: Maps semantic entities to source positions for error reporting

This preserves cache hit rates for evaluation while supporting position-dependent tooling. The alternative—treating all position-dependent code as impure—will **devastate Nixpkgs cache hit rates** since error reporting paths are common.

## Query-level granularity beats per-thunk memoization

The plan must choose memoization granularity: per-thunk, per-expression, or per-import. Evidence from rust-analyzer and the Build Systems à la Carte paper strongly favors **query/function-level granularity** over fine-grained per-thunk caching.

Per-thunk hashing incurs prohibitive overhead:
- Hash computation for every cached result
- Memory for key storage, value storage, dependency lists, version numbers
- LRU tracking overhead per entry
- rust-analyzer reports **8GB+ memory usage** on large projects despite LRU limits

The key optimization is **early cutoff**: when a computation produces the same result as before, downstream dependents don't need re-evaluation. This requires intermediate representations that act as "shields"—changes to source code that don't change semantic structure don't propagate.

Shake implements multi-level early cutoff by recording actual input hashes rather than just timestamps. When a task re-executes and produces an identical result, the system stops propagating invalidation. For Nix, natural cutoff boundaries exist at:

- Import boundaries (`.nix` file level)
- Function application boundaries  
- Derivation boundaries

GHC's module-level granularity (50-200 lines per module being optimal) suggests Nix imports are the right abstraction level for primary caching, with function-level caching as a secondary optimization.

## Hash modulo alpha-equivalence enables syntax-based caching

Lazy-safe hashing without forcing thunks requires hashing syntax rather than semantics. Recent advances since Dolstra's 2008 paper provide efficient algorithms:

**Maziarz et al. (PLDI 2021)** achieve O(n log² n) alpha-equivalence detection by building "e-summaries" combining structure with variable position trees, using commutative hash combiners for map merging. Their follow-up (PLDI 2024) handles de Bruijn indices natively.

**Unison's content-addressed model** demonstrates practical large-scale deployment: 512-bit SHA3 digests of syntax trees with names excluded. Alpha-equivalent terms get identical hashes, and mutual recursion is handled by:

1. Replace recursive calls with de Bruijn indices into the cycle
2. Hash each element individually
3. Sort for canonical order independent of source
4. Hash the sorted structure as the cycle identifier

The collision probability for b-bit hashes is bounded by `4(|e₁| + |e₂|)/2^b`—negligible for 256+ bit hashes. This approach enables Nix to cache based on expression structure without forcing any thunks.

## Symbol interning requires re-interning on deserialize

Symbol IDs local to an `EvalState` cannot appear in cross-evaluation caches. Every major interpreter—Lua, Python, Erlang, Ruby—handles this identically: **serialize symbols as strings and re-intern on deserialize**.

Erlang's distributed protocol provides the most sophisticated approach: frequently-used atoms are cached per connection with local indices, optimizing wire format while maintaining correctness through re-interning. Ruby 2.2+ distinguishes "immortal" symbols (method names, constants) from "mortal" symbols (dynamically created), enabling garbage collection of the latter.

For Nix cross-evaluation caching:

1. Store original strings alongside numeric IDs in cache entries
2. Use content-addressed hashes (SHA256 of string) as cache keys
3. Re-intern into local symbol table on cache load
4. Consider Lua 5.3's optimization: don't intern strings longer than 40 characters

The performance cost of re-interning (O(n) lookup per symbol on load) is acceptable given typical evaluation profiles where symbol operations are not the bottleneck.

## Fine-grained impurity tracking enables partial invalidation

The proposed `impureToken` monotonic counter is **maximally conservative**: any impure call invalidates all results that transitively depend on any impurity. The coeffects literature suggests a more precise alternative.

Coeffects (Petricek, Orchard, Mycroft, ICFP 2014) track what computations need *from* their environment—directly applicable to caching requirements. A coeffect-based approach would track *which* impure operation each result depends on:

- `readFile "/etc/hosts"` → depends on file system token for `/etc/hosts`
- `currentTime` → depends on time token  
- `unsafeGetAttrPos` → depends on source positions token
- `fetchurl` → depends on network token

When only `/etc/hosts` changes, results depending solely on `currentTime` remain valid. Koka's effect system demonstrates practical fine-grained tracking where every function's type includes its effect annotations, enabling composition and selective invalidation.

The implementation cost is higher—tracking effect sets rather than a single counter—but the cache hit rate improvement could be substantial for Nixpkgs, where different impure operations are called in different contexts.

## LMDB for synchronous lookups, SQLite for complex queries

Persistent caching infrastructure should follow Bazel's two-table architecture:

1. **Action Cache**: Maps computation hash → result metadata
2. **Content-Addressable Store (CAS)**: Maps content hash → actual data

For the action cache requiring sub-millisecond lookups, **LMDB** outperforms SQLite dramatically:
- Sequential reads: 47x faster than SQLite
- Random reads: 9x faster  
- Zero-copy via memory mapping
- No WAL checkpointing pauses

For complex metadata queries (cache statistics, eviction policies), SQLite's query capability is valuable. A hybrid approach uses LMDB for the hot path and SQLite for management operations.

Key schema design principles from production systems:

- Use BLOB for hashes (50% space savings over hex strings)
- Include hash+size pairs in digests to detect corruption (Bazel's lesson)
- Track `last_accessed` timestamps for LRU eviction
- Enable WAL mode for concurrent access: `PRAGMA journal_mode=WAL`
- Sign artifacts with HMAC-SHA256 for integrity (Turborepo's approach)

Content-addressable caches are **self-invalidating**: changed inputs produce different hashes, creating new cache keys. Old entries become unreferenced and can be garbage collected without explicit invalidation logic.

## Conclusion: Architectural recommendations

The Nix evaluator memoization plan addresses genuine challenges but should incorporate these refinements:

**Sound foundations to preserve**: The three-phase approach (intra-eval, cross-eval, cross-machine) aligns with Salsa's durability model. Content-addressable storage provides natural deduplication and invalidation.

**Critical changes to consider**: Replace pointer-based thunk identity with syntax-based hashing following Dolstra's proven ATerm approach. Extend the `with` fix to track ordered scope chains. Adopt Salsa's durability levels rather than separate predicates. Implement early cutoff at import and function boundaries.

**Architectural improvements**: Separate position tracking from semantic evaluation using position-independent identifiers. Use LMDB for cache storage with Bazel's two-table schema. Consider coeffect-style fine-grained impurity tracking for better Nixpkgs hit rates.

The most significant risk in the current plan is per-thunk granularity overhead—rust-analyzer's experience suggests query-level caching with early cutoff achieves better performance than fine-grained memoization. The most significant opportunity is Salsa's durability optimization, which could provide substantial speedups by avoiding revalidation of stable Nixpkgs code when only user files change.
```

## Kagi

```text
# Critique and Updated Plan for Nix Evaluator Memoization

## Executive Summary

The current plan correctly identifies the key challenges but has several gaps that could lead to correctness bugs or missed optimization opportunities. The most critical issues are:

1. **Env pointer instability** — `(expr_ptr, env_ptr)` pairs are not stable identifiers
2. **Boehm GC pointer reuse** — no concrete mitigation strategy
3. **Missing the "closed term optimization"** — essential per Dolstra's paper
4. **Order-dependence is deeper than acknowledged** — affects env construction, not just thunk content

---

## Critical Issues with the Current Plan

### Issue 1: Env Pointer Instability

The plan proposes hashing thunks by `(expr_ptr, env_ptr)` for intra-eval caching.  However, this assumes that semantically equivalent environments always have the same `Env*` pointer, which is **false**.

**Problem**: When a closure is created, it captures the current `Env*`. But the same logical environment can have different `Env*` pointers depending on:
- Which thunks in parent environments have been forced
- The order in which `let` bindings were evaluated
- Whether intermediate environments were GC'd and recreated

**Example**:
```nix
let
  a = expensive1;
  b = expensive2;
  f = x: a + x;  # Captures Env containing {a, b}
in
  f 1 + f 2
```

If `a` is forced before creating `f` vs. after, the `Env*` captured by `f` may differ (because forcing `a` mutates the env slot in-place). Two calls to `f` with the same argument could have different `(expr_ptr, env_ptr)` keys despite being semantically identical.

**Recommendation**: Either:
- **Hash-cons environments** so semantically equal envs share the same pointer
- **Use structural env hashing** but only for the *shape* (which slots exist), not content
- **Accept reduced hit rates** and document this limitation

---

### Issue 2: Boehm GC Pointer Reuse

The plan acknowledges that "pointers can be reused after collection"  but doesn't provide a concrete mitigation.

**Problem**: After GC, a new `Value*` could be allocated at the same address as a previously collected one. If the old value was a cache key, the cache now returns wrong results for the new value.

**Evidence**: The document mentions observing "expected a set but found null" errors from cache returning results for different (but pointer-identical after GC) thunk arguments.

**State-of-the-art solutions**:

1. **Generation counters** (simple): Add a monotonic counter to each `Value`. Cache keys become `(ptr, generation)`. 

2. **Weak references with finalizers** (GHC's approach): Use `GC_register_finalizer` to remove cache entries when keys are collected. The `stable-memo` package does this: "Finalizers are put in place to remove the corresponding entries from the memo table if this happens."
3. **Disappearing links** (Boehm-specific): Use `GC_general_register_disappearing_link` to null cache entries when values are collected.
**Recommendation**: Implement option 2 (finalizers) for correctness, with option 1 as a simpler fallback:

```cpp
struct MemoKey {
    Value* ptr;
    uint64_t generation;  // Monotonic counter assigned at allocation
};

// On Value allocation:
value->generation = nextGeneration++;

// On cache insert:
GC_register_finalizer(value, remove_from_cache, &memoCache, nullptr, nullptr);
```

---

### Issue 3: Missing Closed-Term Optimization

Dolstra's "Maximal Laziness" paper emphasizes that the **closed-term optimization is essential**: "This optimization alone makes maximal laziness feasible for real-world Nix expressions. Without it, `nix-env -qa` doesn't finish; with it, it runs in 2.75 seconds."

**What it does**: When substituting a closed term (no free variables) into a function body, wrap it in a `closed()` marker. The substitution function skips descending into `closed()` terms, preventing term explosion.

**Why it matters for memoization**: Without this, substitution creates many syntactically different but semantically equivalent terms, reducing cache hit rates.

**Recommendation**: Add to Phase 2 or create a new phase for this optimization.

---

### Issue 4: Cycle Detection in Hashing

The plan mentions cycle detection for serialization  but not for hashing. Recursive environments (`rec { ... }`) can cause infinite loops during hash computation.

**Example**:
```nix
rec {
  a = { inherit b; };
  b = { inherit a; };
}
```

Hashing `a` requires hashing `b`, which requires hashing `a` → infinite loop.

**Solutions**:

1. **Visited set with identity**: Track visited `Value*` pointers during hashing. Return a sentinel hash for cycles.

2. **Depth-limited hashing** (Racket's approach): Stop recursion at a fixed depth, return 0.

3. **Lazy hash with cycle marker**: When a cycle is detected, include a unique cycle marker in the hash.

**Recommendation**: Use option 1 (visited set) for correctness:

```cpp
size_t hashValue(Value* v, std::unordered_set<Value*>& visited) {
    if (!visited.insert(v).second) {
        return CYCLE_MARKER;  // Already visiting this value
    }
    // ... recursive hashing ...
}
```

---

### Issue 5: Order-Dependence is Deeper Than Acknowledged

The plan correctly identifies that "Env hash order-dependent" is a problem , but the issue is deeper than thunk content changing when forced.

**The deeper problem**: The *structure* of environments depends on evaluation order:

1. **Env allocation timing**: When is a new `Env` frame created vs. reusing an existing one?
2. **Thunk-to-value mutation**: Nix mutates thunks in-place when forced (like GHC). This changes the env's "content" even if the "shape" is the same.
3. **`with` scope resolution**: `with` expressions create dynamic scopes that depend on which attributes have been accessed.

**Implication**: Even `(expr_ptr, env_ptr)` identity-based caching will have order-dependent hit rates. This isn't a correctness bug, but it makes performance unpredictable.

---

## State-of-the-Art Alternatives

### Alternative 1: Nominal Adapton's First-Class Names

Nominal Adapton uses **explicit names** instead of structural hashing to identify computations.
**Key insight**: "Structural matching is too conservative—it was designed for immutable inputs... our aim should be to return a result that requires only a little work to repair."

**How it works**:
- Programmers associate names with allocations: `ref(name, value)`
- Names are generated deterministically via `fork`: `fork n → (n1, n2)`
- Matching uses name equality, not structural equality

**Applicability to Nix**: This would require language changes (adding names to thunks), but could provide stable identity across evaluation orders. Consider for long-term roadmap.

### Alternative 2: GHC's StableName Approach

GHC provides `StableName` for identity-based memoization that survives GC.
**Key properties**:
- `makeStableName :: a -> IO (StableName a)` returns a stable identifier
- "If `sn1 == sn2` then `sn1` and `sn2` were created by calls to `makeStableName` on the same object"
- **Caveat**: "makeStableName may return a different StableName after an object is evaluated"

**Applicability to Nix**: Could implement a similar mechanism using Boehm GC's object addresses + generation counters. The caveat about evaluation changing the StableName is relevant—Nix thunks mutate in-place.

### Alternative 3: Hash-Consing at AST Level (Dolstra's Original Approach)

Instead of hashing runtime values, **hash-cons the AST** so that syntactically equal expressions share the same `Expr*` pointer.

**Benefits**:
- `Expr*` identity is stable (AST doesn't mutate)
- Memoization key becomes `(Expr*, Env*)` where `Expr*` is reliable
- Reduces memory usage from AST deduplication

**Implementation**:
```cpp
class ExprInternTable {
    std::unordered_map<ExprKey, Expr*> table;
    
    Expr* intern(ExprNode node) {
        auto key = makeKey(node);
        auto it = table.find(key);
        if (it != table.end()) return it->second;
        Expr* e = allocExpr(node);
        table[key] = e;
        return e;
    }
};
```

**Recommendation**: This is the most promising approach for Phase 2. It addresses the `Expr*` half of the key stability problem.

---

## Updated Implementation Plan

### Phase 1: Correctness Fixes (Must Do)

| Task | Priority | Notes |
|------|----------|-------|
| Fix `with`-bound variable hash | **Blocking** | Add `feedSymbol(sink, expr->name)` |
| Add cycle detection to hashing | **High** | Use visited set, return `CYCLE_MARKER` |
| Add cycle detection to serialization | **High** | Track visited set, throw on cycle |
| Mark `unsafeGetAttrPos` impure | **High** | Add `ImpureReason::PositionDependent` |
| Add generation counters to `Value` | **High** | Mitigate GC pointer reuse |
| Add finalizers for cache cleanup | **High** | Remove entries when keys are GC'd |

### Phase 2: Architectural Improvements (Should Do)

| Task | Priority | Notes |
|------|----------|-------|
| Split cacheability predicates | **High** | `isValueCacheableIntraEval()` vs `isValueCacheableCrossEval()` |
| Implement AST hash-consing | **High** | Intern `Expr*` nodes for stable identity |
| Implement closed-term optimization | **Medium** | Wrap substituted closed terms |
| Document order-dependence limitations | **Medium** | Set expectations for hit rates |

### Phase 3: Environment Canonicalization (Experimental)

| Task | Priority | Notes |
|------|----------|-------|
| Investigate env hash-consing | **Medium** | Would make `Env*` identity reliable |
| Prototype env shape hashing | **Low** | Hash structure without content |
| Evaluate Nominal Adapton approach | **Low** | Long-term, requires language changes |

### Phase 4: Cross-Eval Caching (Nice to Have)

| Task | Priority | Notes |
|------|----------|-------|
| Make `ExprParseFile` hashable | **Medium** | Hash by `path.fingerprint()` |
| Implement content-based value hashing | **Medium** | For persistence only, not intra-eval |
| Add SQLite backend | **Low** | Persistent storage |
| Wire EvalInputs fingerprint | **Low** | Include in persistent cache key |

---

## Questions for the Codebase Agent

### Environment Structure

1. **How is `Env` allocated?** Is there a single allocator, or are envs allocated in multiple places? Where would generation counters be added?

2. **When are new `Env` frames created?** Specifically:
   - Does `let { a = 1; b = 2; } in ...` create one frame or two?
   - Does `with { ... }; ...` create a new frame or modify the existing one?
   - Are `Env` frames ever shared between closures?

3. **How does thunk forcing mutate the environment?** When a thunk in an env slot is forced, is the slot mutated in-place? Does this affect the `Env*` pointer or just the slot content?

### Expression Hashing

4. **Where is `Expr` allocated?** Is there already any interning/deduplication? Where would AST hash-consing be integrated?

5. **How are De Bruijn indices computed?** The document mentions `(level, displ)` for variables. Are these computed during parsing or in a separate pass? Would hash-consing interfere with this?

6. **What is `ExprWith::parentWith`?** The document mentions `with` expressions have a `parentWith` pointer. How does this interact with environment lookup?

### Memoization Infrastructure

7. **Where is `thunkMemoCache` defined and used?** What is its current key type? How is it integrated with `forceValue`?

8. **How does `impureToken` work?** Is it a simple counter? How is it checked during cache lookup?

9. **What is the current behavior when a cached value is retrieved?** Is there any validation that the cached value is still valid?

### GC Integration

10. **Are there existing uses of `GC_register_finalizer` in the codebase?** If so, what patterns are used?

11. **How are `Value*` pointers currently allocated?** Is there a central allocator that could be modified to add generation counters?

12. **Is there any existing weak reference infrastructure?** Or would this need to be built from scratch?

### Performance

13. **What are the hot paths in evaluation?** Where would memoization overhead be most costly?

14. **Are there existing benchmarks for evaluation performance?** What workloads should be used to measure memoization effectiveness?

15. **How much memory does a typical Nixpkgs evaluation use?** This affects cache sizing decisions.

---

## Summary

The current plan is a solid foundation but needs:

1. **Concrete GC safety measures** (generation counters + finalizers)
2. **Cycle detection in hashing** (not just serialization)
3. **AST hash-consing** for stable `Expr*` identity
4. **Closed-term optimization** for practical performance
5. **Documentation of order-dependence limitations**

The most impactful change would be **AST hash-consing** (Phase 2), which addresses half of the key stability problem and aligns with Dolstra's original approach. Combined with generation counters for GC safety, this would provide a solid foundation for both intra-eval and cross-eval caching.
```