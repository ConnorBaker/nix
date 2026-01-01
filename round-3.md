# Prompt

This is a continuation of round-2.md.

Please read all responses. You should fetch and read the sources referenced to ensure accuracy. You should also list those sources. Then, update your plan where appropriate, being sure to include alternative designs or implementations and their tradeoffs, as well as additional references. Lastly, answer any questions posed by any of the responses in a new file, round-3-answers.md.

Since there are a lot of steps here, please make a todo list and use it to keep track of your finished work and remaining work as you proceed.

# Responses

## ChatGPT

```text
Persistent Memoization Infrastructure: Third-Round Plan

Evaluation of Proposed Design Changes

The updated proposals address key consistency and performance challenges. In particular, splitting cacheability vs. portability (Decision 1) is justified: intra-run caching can safely use identity (pointer-based) keys, whereas cross-run caching needs content-hash keys. The recommended Salsa-style durability model (LOW/MEDIUM/HIGH) unifies these concerns via version vectors ￼ ￼. This appears correct and aligns with durable incrementality techniques, though it requires significant refactoring to track per-input durability.

The two-level caching strategy (Decision 2) – an identity cache for in-memory reuse plus a content-hash cache for persistent reuse – is both feasible and effective ￼. Identity caching (keyed by (Expr*,Env*,gen)) is very fast for a single eval ￼, while a hash-based cache ensures portability across sessions.  Using Boehm GC “disappearing links” and per-Value generation counters handles GC safely ￼ ￼. This correctly prevents stale pointers from causing false hits, at the cost of added GC integration complexity.

Cycle handling (Decision 3) is solved by using De Bruijn-style back-references during hashing ￼. This ensures that different cyclic structures produce different hashes (unlike a uniform CYCLE_MARKER) ￼ and avoids infinite recursion. It is technically sound (similar to Python’s pickle and Unison’s cycle encoding) and enables valid recursive definitions to be cached. For serialization, cycles will still be rejected (throwing an error) since reconstructing cyclic values on disk is complex ￼.

For position-dependent builtins (Decision 4), the plan’s two-layer approach is prudent: separate semantics (cacheable) from source-position info (non-cached) ￼. In practice this means evaluating expressions with abstract identifiers (no position) and only mapping those to locations when needed (e.g. error reporting) ￼. This preserves cache hits (position is irrelevant semantically) at the cost of an architectural split. It is feasible and mirrors techniques in IDE tooling ￼, though it requires rewriting attribute lookups to be position-agnostic until post-processing.

The symbol-interning issue (Decision 5) is resolved by serializing symbols as strings and re-interning on load, which is standard in many languages ￼. This ensures that persistent cache keys do not depend on EvalState-specific symbol IDs. It is a correct, low-risk solution, albeit making deserialization O(n) in symbol count.

On cache granularity (Decision 6), the feedback is clear: per-thunk caching is too fine-grained and memory-heavy ￼. The recommendation is to cache at higher levels (e.g. entire function or import scopes) with an early cutoff check (skip work if inputs haven’t changed) ￼. This matches “build-systems à la carte” insights and should yield much better performance for large projects. It is a sound approach, though it requires defining natural computation boundaries and tracking dependencies, as suggested by Adapton and early-cutoff techniques.

The storage backend choice (Decision 7) favors LMDB for the hot key–value store (with SQLite as a metadata layer) ￼. Benchmark data shows LMDB vastly outperforms SQLite for typical workloads ￼. This is a good call for speed; the trade-off is added complexity of a dual backend. Overall it is correct for a scalable cache (similar to Bazel’s two-table model with an action cache and content-addressable store ￼).

The α-equivalence hashing proposals (Decision 8) recognize that Nix’s current De Bruijn hashing is incomplete: for example, with scopes and nested with chains are not fully distinguished ￼. Modern algorithms (e.g. Maziarz et al., PLDI 2021 ￼) use commutative hash combiners to achieve hashing modulo α-equivalence in O(n log² n) time ￼. Adopting these ideas (or Unison’s 512-bit content-hash approach ￼) would correctly identify semantically equal terms despite renaming. Implementing this is non-trivial but would greatly strengthen cross-evaluation matching of expressions.

The impurity-tracking design (Decision 9) is conservative in current Nix (one global token). The proposal to track dependencies per resource (e.g. file, time, network) ￼ is correct for precise invalidation: e.g. a derivation depending only on /etc/hosts should not be invalidated by a currentTime change. This fine-grained effect system is used in languages like Koka (and relates to coeffects) but adds overhead. It is conceptually sound but would need careful engineering to remain efficient.

The serialization format (Decision 10) is chosen as deterministic CBOR (RFC 8949) ￼. CBOR natively supports binary and has strict canonical encoding rules, meeting the requirements of compactness and determinism. This is a strong choice: it handles strings, lists, attrsets (with sorted keys), etc. A key limitation is that functions (closures) cannot be serialized for cross-cache use ￼, which is acceptable since closures depend on runtime state. This means any value containing functions will be marked non-cacheable (necessary for correctness).

Finally, the import/IFD caching (Decision 11) recommendations match known patterns. For builtins.import, using just the path for store imports, and path+content hash for normal files, is logical ￼. Maintaining an mtime/size cache (fast-check) is a good optimization for file contents ￼. For Import-From-Derivation, hashing the derivation .drv (which encodes all inputs) is correct, with validation by comparing output hashes ￼. These align with ideas from Bazel’s remote cache and Tweag’s RFC.

Overall, the proposed solutions are correct in principle and align with best practices. The main feasibility challenges are engineering complexity (refactoring for two caches, introducing new AST nodes, integrating LMDB, etc.) and ensuring no new semantic regressions. But each recommendation has a solid rationale and influence from proven systems ￼ ￼.

Proposed Evaluator Architecture Enhancements

Beyond the current proposals, we identify several architectural improvements:
	•	Closed-Term Detection: Large static subexpressions (e.g. the Nixpkgs stdlib) can be marked as closed at parse time (no free variables) and cached. As suggested in Round 2 answers, wrapping such expressions in a closed(expr) marker lets us compute and store their hash just once ￼. This “closed-term” optimization (from Dolstra 2008) prevents exponential growth of substituted terms and dramatically speeds up repeated traversals. The evaluator should perform a free-variable analysis on parsed ASTs and mark closed subtrees.
	•	AST Interning: The plan already recommends hash-consing Expr nodes (Phase 2, AST intern) ￼. We suggest doing this as part of parsing or loading: use subtype-specific hash-cons tables so identical subexpressions share the same pointer ￼. This ensures stable pointer identity for in-memory caching and simplifies pointer-based comparisons. The interned ASTs should be GC-rooted (or never freed) to preserve their identity.
	•	Environment Representation: Nix currently uses linked Env* frames, which makes hashing sensitive to evaluation order (as noted in Decision 2) ￼. We recommend exploring persistent (shared) data structures for environments, such as trees or tries keyed by variable name. If environment frames were immutable records with structural sharing, hashing could be order-independent. At minimum, environments could include a consistent ordering of bindings to reduce randomness. This is an advanced change but could stabilize environment fingerprints.
	•	Multi-Phase Evaluation: Inspired by Bazel’s load/analyze/exec phases and rust-analyzer’s architecture, we can decouple evaluation into distinct stages. For example, a semantic evaluation phase computes attribute values and closures without recording source positions, while a resolution phase maps errors and positions separately (cf. Decision 4 ￼). Similarly, one could add an analysis phase to compute AST hashes, free-variable info, and closed-term markers before actual lazy forcing. This layered model would improve responsiveness (as in IDEs) and make caching more modular.
	•	Shared Environment Cache: The evaluator could maintain a global table of environments keyed by their structural contents. After computing a new Env*, we check if an equivalent frame already exists to reuse it. This interning of environment frames (similar to hashing attribute sets) would reduce redundancy when many closures capture identical contexts. It would also make environment hashes stable across runs (since their content is canonicalized) and could improve pointer-based caching.

Implementing these enhancements would further optimize caching and reduce duplication. The round-2 suggestion of early cutoff (wrapping forceValue to record dependencies ￼) could also be integrated: each cached thunk stores which sub-thunks it forced and their result hashes, allowing skipping recomputation if none changed.

Comparative Strategies in Other Systems

Several systems tackle similar caching and hashing challenges:
	•	Unison (α-equivalence hashing): Unison treats programs as content-addressed values; it uses a 512-bit SHA3 hash of term structure (ignoring all names) to identify code fragments ￼. For recursive bindings, Unison encodes cycles as #x.n pairs (cycle-hash plus index) to produce distinct hashes ￼. This ensures equivalent code (up to renaming) has the same hash, analogous to the hashing-mod-α algorithm of Maziarz et al. ￼. Adopting a similar strong content-hash (perhaps using SHA3/512) would make Nix hashes robust to renaming and recursion.
	•	Bazel Skyframe: Bazel builds an incremental computation graph (Skyframe) of well-defined nodes (analysis and execution artifacts) and memoizes each node’s value ￼. It maintains multiple caches: in-memory Skyframe cache (for fast rebuilds) and persistent action/output caches on disk. The plan’s two-tier cache parallels this: the identity cache is like Skyframe’s in-memory memo, while the content-addressed cache is akin to Bazel’s remote action cache. Bazel also splits work into phases (loading, analysis, execution) and uses checksums of all inputs to decide invalidation, much like the proposed durability and input-fingerprint keys. The two-table (action cache + CAS) architecture in the proposal ￼ is directly borrowed from Bazel’s design.
	•	GNU Guix: Guix, like Nix, is a purely functional package manager. Its /gnu/store acts as a content-addressed cache: the hash of all inputs of a build (including compiler, flags, sources) determines the output directory ￼. In effect, the store is an on-disk memo of build “functions.” This is conceptually the same as memoizing builtins.derivation invocations. Guix also uses a local SQL metadata cache for package lookups. The key lesson is that content-addressed builds automatically get cross-machine caching (via substitutes), which our design can similarly leverage once persistent keys (hashes) are stable.

These analogies confirm that Nix’s approach (pure functions + content hash) is sound. Implementing a strong α-hash like Unison’s or PLDI’s, an incremental graph like Bazel’s, and a content-address store like Guix’s, all align with our goals.

Implementation Roadmap (Phases and Tasks)

We propose a multi-phase rollout, prioritizing correctness, then performance, then cross-evaluation:
	•	Phase 1 – Correctness (Blocking): Address fundamental invariants before any caching is enabled. Key tasks:
	•	Fix expression hashing: Include with-bound variable names and full scope chain in the hash ￼. Without this, e.g. with {x=1;y=2;}; x and ...; y collided incorrectly.
	•	Cycle detection in hashing: Use an ancestry stack with De Bruijn back-references instead of a uniform marker ￼ ￼. This prevents infinite loops and distinguishes different rec structures.
	•	Cycle detection in serialization: Track visited nodes and throw an error on cycles ￼. This safely aborts on unsupported cyclic values.
	•	GC safety: Add a generation counter to each Value (in value.hh) to detect when an old pointer has been recycled ￼. Also use GC_general_register_disappearing_link in the cache so collected thunks automatically null out entries ￼ ￼. Periodically sweep null entries out of the cache.
	•	Impurity marking: Mark unsafeGetAttrPos (and similar) as impure (PositionDependent) so its results aren’t cached ￼.
	•	Testing: Write unit tests for all above cases, e.g. two with examples should yield different hashes, cycles don’t hang, impure ops invalidate cache, etc ￼.
	•	Phase 2 – In-Memory Performance (High Impact): Implement the two-tier cache and related optimizations:
	•	Two-level cache: In eval-inline.hh, check an identity cache (keyed by (Expr*,Env*,gen)) first, then a fallback content cache keyed by structural hash ￼ ￼. This provides fast hits intra-eval and persistent hits cross-eval.
	•	AST intern tables: Modify the parser to intern every new Expr (using subtype hash-cons tables) so that semantically identical subtrees share a pointer ￼ ￼. This makes identity comparisons reliable.
	•	Deferred value intern: Only hash-cons persistent (tenured) values, per Appel 1993 ￼ ￼. Short-lived thunks are not interned, saving lookup cost.
	•	Closed-term optimization: Detect closed sub-expressions (no free vars) at parse or early eval time and wrap them so their hash is cached once ￼ ￼. This avoids re-hashing large constant values repeatedly.
	•	Cacheability flags: Split valueIsUncacheable() into isIdentityCacheable() vs. isContentCacheable() ￼ ￼, reflecting portability constraints. E.g. lambdas are only content-cacheable.
	•	Thunk hashing: Ensure that hashing a thunk always uses its (expr_ptr, env_ptr) identity, never forcing it ￼ ￼. This maintains referential transparency: forced vs unforced thunks hash the same.
	•	Phase 3 – Architecture and Instrumentation: Refine caching behavior and gather data:
	•	Stabilize environment hashing: Continue improving env-hash.cc by hashing unforced thunks by identity and forced values by content ￼ ￼. This removes order-dependence.
	•	Portability classification: Ensure value-hash.cc correctly checks entire closure environments for content-portability (e.g. nested lambdas) ￼.
	•	Early cutoff mechanism: Record which thunks were forced and their result hashes during an eval (an “EvalTrace”) and skip recomputation if those child results are unchanged (see Round 2 Q7) ￼ ￼.
	•	Monitoring: Add cache statistics (hit/miss counts, lookup times) and profiling hooks ￼. Document any known limitations (e.g. “higher-order cycles may still cache poorly” ￼).
	•	Position caching: Begin separating the semantic layer and position layer (decision 4) as a target, perhaps starting by always attaching Expr* to values for later resolution (in preparation for a full split) ￼ ￼.
	•	Phase 4 – Cross-Evaluation (Persistence): Enable persistent caching across runs:
	•	Name resolution: Serialize symbols as strings (avoid EvalState-specific IDs) when writing CBOR, and re-intern on load ￼.
	•	Input fingerprinting: Include any global context (e.g. flake inputs fingerprint, NIX_PATH, environment variables) in the persistent cache key. Possibly implement an EvalInputs fingerprint (Phase 4 task) to catch external factors.
	•	Import caching: Implement builtins.import caching: for store paths, use the path; for files, use content hash + NIX_PATH context ￼. Use stat mtime/size as a quick check before re-hashing ￼.
	•	IFD caching: For Import-From-Derivation, key on the derivation’s .drv hash (all inputs) and store the build output path and its content hash for validation ￼. On hit, load the cached Nix value.
	•	Persistent store: Integrate LMDB (via lmdb++ or existing libstore code) as the hot cache for (key→value) lookups ￼. Optionally shard by durability (high/medium/low) to reduce contention. Use SQLite only for auxiliary metadata/eviction policies ￼.
	•	Serialization: Use deterministic CBOR for all Nix values ￼. Define CBOR tags for path values (including their content hash) and string contexts as shown ￼. Ensure the serializer respects the rules (sorted attr keys, etc.) for canonical output.
	•	Deserializer: Implement CBOR reader to reconstruct values. Skip (reject) any value containing a function/closure, marking it non-cacheable ￼. Store serialization versions to detect format upgrades.
	•	Phase 5 – Advanced Optimizations (Future): Once basic caching is stable, add more sophisticated features:
	•	Durability levels: Fully implement the SALSA-style version vector, bumping low/med/high on relevant inputs (e.g. low++ on any local change, med++ on pinned flake changes) ￼ ￼.
	•	Position-independent semantics: Complete the two-layer architecture (semantic vs position) so that error reporting only maps values to positions after caching.
	•	Fine-grained impure tokens: Implement per-effect tokens (file-specific, URL-specific) as outlined in Round 2 answers ￼ and Decision 9 ￼, to avoid broad invalidations.
	•	Higher-level caching: Expand “query-level” caching beyond thunks—for example, cache entire function calls or derivations as atomic units (as suggested in Decision 6 and Phase 5 ￼). Use early-cutoff at those boundaries.

This roadmap covers in-memory memoization (Phases 1–2), persistent cache encoding and serialization (Phase 4), symbol and environment handling (Phases 1–4), and cross-eval keying (Phase 4). Each phase includes testing and measurement to validate correctness and performance.

Technical Risks and Open Issues

Several challenges remain:
	•	Semantic changes: Refactoring hashing and caching may subtly change evaluation order or memory layout (e.g. different GC behavior). We must ensure that the semantics of Nix programs are unchanged. In particular, lazy evaluation means thunks must remain unforced unless needed. All hashing must avoid side-effects (as Decision 12 emphasizes ￼).
	•	GC integration complexity: Using disappearing links and generation counters relies on Boehm GC behavior. Bugs here (e.g. missing a link registration) could leak memory or cause stale hits. The interplay of GC cycles and finalization is tricky (Boehm notes that finalizers in cycles are never called), so this area needs careful testing.
	•	Environment identity: Currently, two logically identical Env* frames may have different pointers (due to evaluation order or GC). Until we invest in a shared representation, content-hashing an environment remains order-sensitive. Some memoization may be missed if pointer/alloc order differs.
	•	Impurity identification: Identifying precisely which inputs a value depends on (for fine-grained impure tokens) may be hard in practice. Many builtins (e.g. builtins.currentTime, getEnv, readFile) must be audited and tagged. Omissions could cause incorrect cache hits. We may need to simplify (e.g. just treat any I/O as low-durability for now).
	•	Memory overhead: The identity cache and value interning tables consume memory. On very large evaluations (e.g. full Nixpkgs), the cache itself could become big. We must tune policies (e.g. LRU eviction, or limited scope of identity caching) to avoid OOM.
	•	Symbol interning/IDs: The C API detail of symbol tables means that mixing results from different EvalStates is error-prone. If a persistent cache is shared across contexts, symbol re-interning could mis-resolve if the set of symbols differs. Using full strings for symbols (as planned) mitigates this, but performance costs must be measured.
	•	Closed-term detection false positives: Automatically recognizing “closed” expressions requires correct free-variable analysis in Nix’s dynamic scope semantics (including with). Misidentifying a non-closed term as closed could cause subtle bugs. We must validate this analysis thoroughly.
	•	Serialization compatibility: As Nix evolves, its internal Value representation may change. We need a versioning scheme in the serialization format to allow upgrades. Deterministic CBOR is stable, but care must be taken if new value types (e.g. shaders, new builtins) are added.
	•	Concurrency: If Nix ever evaluates in parallel threads, the cache structures must be made thread-safe or turned off. For now, Nix is largely single-threaded, but future multi-threading would complicate shared caches.
	•	Cross-machine security: Sharing cached values across machines implies trust that the serialized values are not maliciously modified. Integrity (e.g. HMAC signing as suggested ￼) may be needed for a production remote cache.

These risks warrant cautious implementation, extensive testing (including property-based tests for hashing, and real-world benchmarks), and incremental rollout (e.g. using --experimental-features flags).

Open Questions for Codebase Investigation

To proceed, agents should answer specific implementation questions in the Nix code:
	•	Closed-term detection: How can we efficiently compute free variables of an Expr in the existing parser or AST classes? Is there a hook after parsing or during AST construction to insert ExprClosed nodes (like in Exprs::add()) and compute their hash?
	•	Value generation counters: Where in the Value struct (value.hh) can we add a generation or “epoch” field? How do we initialize and update it so that each GC cycle increments it, and can we reliably detect stale pointers on cache lookup?
	•	Environment hashing: The env-hash.cc code currently iterates Env* frames. Should we change it to ignore order by sorting, or rely entirely on pointer identity for thunks? Specifically, when hashing an Env with a mix of forced and unforced slots, how should we implement “hash by identity vs content” safely?
	•	Symbol interning: How does the current C API assign symbol IDs? For serialization, should we convert all Symbol fields to strings using the SymbolTable? Is there existing code to extract the string from a Symbol?
	•	Builtin impurity: Which primitive operations are not already marked impure? (Round 2 identified unsafeGetAttrPos as missing.) Are there other builtins (e.g. __curPos, network fetches) that must be audited? The source primops.cc could be scanned for missing markImpure() calls.
	•	LMDB integration: Nix’s libstore already uses SQLite; is libstore also linked to LMDB, or would we add LMDB++ as a new dependency? How do we manage transactions safely alongside SQLite (for durability metadata)? Look at src/libstore for patterns (e.g. store/ code).
	•	Serialization infrastructure: The existing value-serialize.cc handles Nix values. Can it be extended to CBOR easily, and does it already handle context (e.g. annotated strings)? Where would custom CBOR tags be implemented? Also, is there a facility for symbol deduplication in serialization (as suggested by Round 2 for large caches) and if not, how to add it?
	•	Thunk identity in thunkMemoCache: The code that currently hashes thunks (in eval-inline.hh) may need changes to use pointer identity. How is thunkMemoCache keyed now, and how to insert the generation counter into its key without breaking existing lookups?
	•	Interaction with existing cache: Nix currently has a flake eval cache. How will the new per-thunk cache co-exist? Specifically, what namespace or prefix should be used so that old cache entries are not mixed with new ones? The Round-2 answers suggest separate namespaces (e.g. "thunk:<hash>"). Where in the code is the namespace for the persistent cache key defined?

Addressing these questions will clarify implementation details and help refine the plan.

Sources: All proposals and tasks above are drawn from the prior plan and answers ￼ ￼ ￼ ￼ ￼, informed by external references (PLDI, Bazel, Guix) ￼ ￼ ￼. These guide the overall design and roadmap.
```

## Claude

```text
# Nix evaluator memoization: Concrete implementation strategies

Implementing effective memoization for Nix's evaluator requires solving ten interconnected challenges spanning AST hashing, cache architecture, memory management, and serialization. This report synthesizes research findings into **actionable C++ implementation recommendations** for each challenge.

## Hash-consing mutable AST nodes requires excluding binding fields

The fundamental insight is that Nix's `bindVars()` mutations—setting `fromWith`, `level`, `displ`, `parentWith`, `prevWith`—are **deterministically derivable from AST structure**. Two structurally identical expressions will always receive identical binding analysis when placed in identical contexts. This makes the "exclude mutable fields from hash" approach both sound and practical.

**Recommended implementation:**
```cpp
struct ExprVar {
    // Immutable (included in hash):
    Symbol name;
    PosIdx pos;
    
    // Mutable (excluded from hash):
    mutable Level level;
    mutable Displacement displ;
    mutable bool fromWith;
    mutable ExprWith* parentWith;
    
    size_t hash() const {
        return combine_hash(EXPR_VAR_TAG, name.hash(), pos.hash());
    }
};
```

GHC employs a similar "phase separation" approach: interface files are fingerprinted only after `PartialModIface` is fully constructed via `addFingerprints`. For Nix, hash-cons at parse time using structural hash, then populate binding fields in-place during `bindVars()`. The hash table remains valid because binding fields don't affect lookup equality.

Alternative approaches like copy-on-write or hash-after-binding add complexity without benefit. COW would require copying nearly every `ExprVar` node since binding affects most expressions. Hash-after-binding would require maintaining two expression types (`UnboundExpr` vs `BoundExpr`) with phantom type enforcement—viable but unnecessarily complex when excluding fields suffices.

## De Bruijn levels with Merkle caching fix the parentWith chain bug

The `parentWith` chain must be included in hashes, but naively walking the entire chain on every variable hash creates O(n×m) complexity for m variables in n-deep `with` nesting. The solution combines **De Bruijn-style level indices** with **Merkle-tree hash caching**.

During `bindVars()`, compute and cache each `ExprWith`'s scope chain hash:

```cpp
struct ExprWith {
    Expr* attrs;
    Expr* body;
    ExprWith* parentWith;
    
    // Computed during bindVars():
    uint32_t withDepth;
    Hash scopeIdentityHash;
};

void ExprWith::bindVars(StaticEnv& env) {
    if (env.parentWith) {
        withDepth = env.parentWith->withDepth + 1;
        scopeIdentityHash = combine(
            attrs->getHash(),
            env.parentWith->scopeIdentityHash
        );
    } else {
        withDepth = 1;
        scopeIdentityHash = combine(SCOPE_ROOT_TAG, attrs->getHash());
    }
}
```

For `ExprVar` hashing, include the cached scope hash:
```cpp
Hash ExprVar::getHash() const {
    Hash h = combine(EXPR_VAR_TAG, name.hash());
    if (parentWith) {
        h = combine(h, parentWith->scopeIdentityHash);
    }
    return h;
}
```

This achieves **O(1) amortized** per-variable hash computation. The critical invariant is that `parentWith` must never be mutated after `bindVars()` completes—add an assertion to enforce this. The scope chain hash depends only on structural properties (the `attrs` expressions in the chain), not memory addresses, ensuring determinism across evaluations.

## Two-level caching separates identity lookup from content addressing

SHA256 dominates hashing overhead, so the architecture should defer content hashing until absolutely necessary. The Salsa framework (used by rust-analyzer) provides the model: a **revision-based identity cache** backed by a **content-addressed persistence layer**.

```
┌─────────────────────────────────────────────┐
│  L1: Identity Cache (HashMap<Expr*, Entry>) │
│  - Key: raw pointer to expression           │
│  - Stores: value, revision, dirty flag      │
│  - Per-evaluation-session, LRU eviction     │
└─────────────────────────────────────────────┘
                    │ promote on eviction
                    ▼
┌─────────────────────────────────────────────┐
│  L2: Content Cache (HashMap<SHA256, Value>) │
│  - Key: SHA256 of normalized expression     │
│  - Persistent across sessions               │
│  - Disk-backed with memory mapping          │
└─────────────────────────────────────────────┘
```

**Consistency protocol using revision numbers:**
```cpp
struct L1Entry {
    Value value;
    uint64_t revision;
    bool dirty;
    std::optional<SHA256> content_hash;  // lazy-computed
};

std::optional<Value> lookup(Expr* expr, uint64_t current_rev) {
    if (auto entry = l1.get(expr)) {
        if (entry->revision >= last_input_change_for(expr))
            return entry->value;
        l1.remove(expr);  // stale
    }
    
    auto hash = sha256_normalize(expr);  // expensive!
    if (auto value = l2.get(hash)) {
        l1.insert(expr, {value, current_rev, false, hash});
        return value;
    }
    return std::nullopt;
}
```

**Promotion strategy**: Promote from L1 to L2 on eviction, but only for entries accessed ≥3 times and with estimated size >1KB. Small values are cheap to recompute; large values justify the hashing cost. Use Salsa's "early cutoff" optimization: if a recomputed value equals the cached value, don't bump `changed_at`, preventing unnecessary downstream invalidations.

## Generation counters fit in tagged pointers without enlarging Value

Nix's 16-byte `Value` type has no padding to exploit, but **tagged pointers** provide 3-19 bits depending on architecture and alignment. For cache invalidation, only 3-8 bits suffice since epoch-based schemes work well.

**Available bits by technique:**

| Technique | Bits | Notes |
|-----------|------|-------|
| Low-bit tagging (8-byte align) | 3 | Universal, requires masking |
| High-bit tagging (x86-64) | 16 | Requires sign extension |
| ARM64 TBI | 8 | Zero overhead—hardware ignores top byte |
| Combined low + high | 19-20 | Maximum flexibility |

**Recommended implementation for x86-64:**
```cpp
class TaggedValuePtr {
    uint64_t data_;
    static constexpr uint64_t PTR_MASK = 0x0000FFFFFFFFFFFF;
    static constexpr int EPOCH_SHIFT = 56;
public:
    Value* get() const {
        uint64_t masked = data_ & PTR_MASK;
        if (masked & (1ULL << 47)) masked |= 0xFFFF000000000000; // sign extend
        return reinterpret_cast<Value*>(masked);
    }
    uint8_t epoch() const { return data_ >> EPOCH_SHIFT; }
    void set(Value* p, uint8_t epoch) {
        data_ = (reinterpret_cast<uint64_t>(p) & PTR_MASK) | (uint64_t(epoch) << EPOCH_SHIFT);
    }
};
```

**Alternative—epoch-based invalidation** without per-value tracking:
- Global epoch counter (8 bits = 256 epochs before wraparound)
- Cache entries store the epoch when cached
- Incrementing epoch invalidates all caches instantly
- Simpler than per-value versioning, sufficient for many workloads

External generation tables (`Value* -> generation`) defeat the purpose of tight packing—they add cache miss overhead on every access and require cleanup when Values are collected.

## Closed-term optimization prevents exponential term explosion

Dolstra's "Maximal Laziness" paper demonstrates that without closed-term optimization, expressions like `(x: y: e1) e2 e3` cause exponential work when `e2` is large—substitution descends into `e2` replacements of `x` even though those subterms are closed. The fix is conceptually simple: **mark closed terms and skip substitution descent**.

**Detecting closed terms with De Bruijn indices:**
A term is closed iff `maxFreeLevel(term) == 0` at depth 0. Compute this bottom-up during `bindVars()`:

```cpp
int ExprVar::computeMaxFreeLevel(int depth) {
    return (index >= depth) ? (index - depth + 1) : 0;
}

int ExprLambda::computeMaxFreeLevel(int depth) {
    return body->computeMaxFreeLevel(depth + 1);
}

int ExprApp::computeMaxFreeLevel(int depth) {
    return std::max(fun->computeMaxFreeLevel(depth),
                    arg->computeMaxFreeLevel(depth));
}
```

**Implementation with is_closed flag:**
```cpp
struct Expr {
    mutable bool is_closed = false;  // Set during bindVars()
    
    void markClosedness(int depth) {
        is_closed = (computeMaxFreeLevel(depth) == 0);
    }
};

// Modified substitution
Value subst(Substitution& subs, Expr* e) {
    if (e->is_closed) return eval(e);  // KEY: stop here!
    // ... normal substitution logic
}
```

Dolstra's evaluation showed this reduces `nix-env -qa` from non-terminating to a few seconds (~21 MiB). The key insight: closed terms are **α-canonical** in De Bruijn representation—syntactically identical closed terms are structurally identical without renaming, enabling maximal sharing.

## Pure evaluation mode enables zero-overhead caching fast paths

Nix's `--pure-eval` disables `currentTime`, `currentSystem`, `getEnv`, and unrestricted filesystem access. This enables **deterministic evaluation caching** but current implementation checks `settings.pureEval` at runtime for each operation.

**Compile-time optimization via `if constexpr`:**
```cpp
template<bool PureMode>
void EvalState::primop_getEnv(Value& v, const std::string& name) {
    if constexpr (PureMode) {
        v.mkString("");  // No runtime check needed
    } else {
        v.mkString(getenv(name.c_str()) ?: "");
    }
}
```

**Separate primop registration** (recommended approach):
```cpp
class PureEvalState : public EvalState {
    void registerPrimops() override {
        // Skip: getEnv, currentTime, currentSystem, storePath
        // Only register pure primops
    }
};
```

**Known impurity edge cases in "pure" mode:**
- `builtins.valueSize` leaks thunk evaluation state (issue #3246)
- Lazy trees expose nondeterministic virtual paths (issue #10689)
- Floating-point edge cases may vary by platform

For cross-machine cache sharing, flake evaluation results keyed by lock file fingerprint are safe. The fingerprint includes all input content hashes, ensuring identical inputs produce identical cache keys regardless of machine.

## Benchmarking requires metrics beyond cache hit rate

The standard approach uses **hyperfine** with warmup runs, combined with Nix's built-in statistics:

```bash
hyperfine --warmup 3 'nix eval --json nixpkgs#firefox'
NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH=stats.json nix-instantiate --eval expr.nix
```

**Essential metrics to track:**

| Category | Metric | Source |
|----------|--------|--------|
| Memory | Peak RSS, cache size ratio | `/proc/self/status`, stats.json |
| Time | Wall clock with CI, hash computation % | hyperfine |
| Cache | Hit rate, invalidation cascades | Instrumentation |
| Evaluation | Thunk count, value count | NIX_SHOW_STATS |

**Pathological test cases:**
- **Deep thunk chains**: Stress memoization table depth
- **Large attribute sets with cross-references**: Stress context tracking
- **Scan patterns**: Sequential access defeating LRU (the classic LRU pathology)
- **Cascading invalidation chains**: One input change triggering long dependency chains

Salsa's **durability** concept is directly applicable: mark stable inputs (nixpkgs) as "high durability" to skip L1 validation, yielding ~300ms savings for stdlib queries in rust-analyzer.

## LRU eviction with Boehm GC needs a two-tier strong/weak design

Pure LRU with Boehm GC's disappearing links is fragile—entries can vanish mid-list-traversal, corrupting data structures. Guile faced similar issues and is **removing weak sets entirely** in their new GC branch.

**Recommended: Two-tier cache with strong pinned + weak overflow:**
```cpp
class GCSafeCache {
    std::deque<Entry> pinned;  // Strong refs, max size N
    WeakHashTable weak;         // Weak refs, GC can collect
    
    Value* lookup(Key k) {
        if (auto v = findInPinned(k)) return v;
        
        auto entry = GC_call_with_alloc_lock(findInWeak, k);
        if (entry && entry->key) {  // Still alive?
            promoteToPin(entry);
            return entry->value;
        }
        return nullptr;  // Cache miss—entry was collected or missing
    }
    
    void insert(Key k, Value* v) {
        pinned.push_front({k, v});
        if (pinned.size() > MAX_PINNED) {
            auto old = pinned.back();
            pinned.pop_back();
            addToWeak(old);  // Demote to weak table
        }
    }
};
```

**Key design principles:**
- Always null-check weak entries (they can disappear anytime)
- Keep recent entries as strong references (LRU approximation)
- Use `GC_call_with_alloc_lock()` for safe weak reference reads
- Accept cache misses gracefully—recomputation is cheaper than corruption
- Monitor heap pressure via `GC_get_heap_size()` for proactive eviction

Size-based or random eviction is simpler than LRU and works well with GC—research shows random eviction performs within 10-30% of optimal LRU for many workloads.

## String contexts require canonical CBOR with interning

Nix string contexts are semantically **unordered sets** of `NixStringContextElem` variants: `Constant` (plain path), `DrvOutput` (specific derivation output), and `DrvDeep` (entire build closure). For deterministic cache keys, serialization must be canonical.

**RFC 8949 Core Deterministic Encoding requirements:**
- Map keys sorted by bytewise lexicographic order of encoded form
- Definite-length encoding only (no indefinite lengths)
- Shortest integer encoding (preferred serialization)

**Canonical ordering for context elements:**
1. Sort by type discriminant: Constant < DrvOutput < DrvDeep
2. Within type, sort by store path lexicographically
3. For DrvOutput, sort output names alphabetically

**Deduplication via interning** is crucial since many strings share identical contexts:
```cpp
class ContextInterner {
    std::unordered_map<SHA256, std::shared_ptr<NixStringContext>> cache;
    
    std::shared_ptr<NixStringContext> intern(NixStringContext ctx) {
        std::sort(ctx.begin(), ctx.end(), canonical_order);
        auto hash = cbor_hash(ctx);
        auto [it, inserted] = cache.try_emplace(hash, 
            std::make_shared<NixStringContext>(std::move(ctx)));
        return it->second;
    }
};
```

For CBOR in C++, **nlohmann/json** supports CBOR but doesn't guarantee determinism by default—you must sort map keys manually. **TinyCBOR** or **libcbor** offer more control over encoding.

## Import caching must distinguish content-addressed from configuration-dependent paths

Nix imports resolve differently based on form, with only some being cacheable across machines:

| Import Form | Resolution | Cross-Machine Cacheable? |
|-------------|------------|-------------------------|
| `/nix/store/...` | Content-addressed | ✓ Yes |
| `./foo.nix` | Relative to file | ✓ If content-hashed |
| `<nixpkgs>` | NIX_PATH lookup | ✗ Configuration-dependent |
| `http://...` | Fetch + TTL | ✓ If narHash specified |

**Flakes eliminate configuration-dependent paths** by forbidding `<...>` lookups in pure mode and requiring all inputs to be explicitly declared with locked content hashes. The flake evaluation cache at `~/.cache/nix/eval-cache-v1/<fingerprint>.sqlite` uses lock file fingerprints as keys—this is the right model.

**Recommended caching architecture:**
1. **L1**: Identity cache keyed by `(resolved_path, content_hash)` → evaluation result
2. **L2**: Content-addressed cache keyed by `SHA256(normalized_expression)` → evaluation result
3. For non-flake evaluation, resolve paths first (consuming NIX_PATH), then cache by content hash

**IFD (Import From Derivation) complicates caching** because it blocks evaluation until derivation outputs are built. Pre-cache IFD artifacts in binary caches, or batch IFD calls into single derivations. IFD is disabled for flake indexing (search.nixos.org) due to security concerns about building arbitrary code.

## Implementation priority and dependencies

For maximum impact with minimum risk, implement in this order:

1. **Closed-term optimization** (highest impact, lowest risk): Add `is_closed` flag, skip substitution descent—this alone can make intractable evaluations finish
2. **parentWith chain fix**: Add Merkle-cached scope hashes—correctness bug that must be fixed
3. **Two-level cache architecture**: L1 identity + L2 content—foundation for all other optimizations
4. **Hash-consing with excluded fields**: Enable at parse time using structural hash
5. **Benchmarking infrastructure**: Essential for measuring other optimizations
6. **Pure eval fast path**: Template specialization for zero-overhead pure mode
7. **LRU with GC**: Two-tier strong/weak cache—complex but important for memory management
8. **CBOR determinism**: Needed for cross-machine cache sharing
9. **Generation counters**: Tagged pointers—only needed if epoch-based invalidation insufficient
10. **Import caching**: Build on content-addressed foundation established earlier

Each optimization builds on previous work, with closed-term optimization providing immediate, substantial benefits while the two-level cache architecture enables the sophisticated memoization strategies that make incremental evaluation practical.
```

## Kagi

```markdown
## Round 3: Critique and Updated Plan for Nix Evaluator Memoization

### Executive Summary

The Round 2 answers reveal several critical implementation details that require plan adjustments:

1. **Expr mutation is confirmed** — `bindVars()` mutates `ExprVar` and `ExprWith` fields, meaning hash-consing must happen **after** binding analysis, not at allocation
2. **`with` chain hashing is broken** — `ExprWith::parentWith` is NOT included in the hash, only `prevWith` (depth count). This is a **correctness bug**.
3. **Finalizers won't work for cycles** — Boehm GC doesn't finalize cyclic structures. Use **disappearing links** instead.
4. **Cache is unbounded** — No eviction policy exists. Full Nixpkgs evaluation can create 100K-1M entries (~80MB).
5. **Hash computation dominates overhead** — 50-80% of memoization cost is SHA256. Two-level caching is essential.

---

### Critical Issue 1: Expr Mutation Timing

**Problem**: The plan proposes AST hash-consing, but the Round 2 answers confirm that several `Expr` fields are mutated during `bindVars()`: [^1]

| Field | Mutated During |
|-------|----------------|
| `ExprVar::fromWith` | `bindVars()` |
| `ExprVar::level` | `bindVars()` |
| `ExprVar::displ` | `bindVars()` |
| `ExprWith::parentWith` | `bindVars()` |
| `ExprWith::prevWith` | `bindVars()` |

**Implication**: Hash-consing at allocation time (in `Exprs::add()`) would hash **incomplete** nodes. Two identical expressions parsed from different files would have different hashes if `bindVars()` hasn't run yet.

**Recommended approach**: **Two-phase interning**:

```cpp
// Phase 1: Parse (no interning)
Expr* parsed = parser.parse(source);

// Phase 2: Bind variables (mutates Expr nodes)
bindVars(parsed, staticEnv);

// Phase 3: Intern (after mutation is complete)
Expr* interned = exprInternTable.intern(parsed);
```

**Alternative**: Exclude mutable fields from the hash key, using only the immutable structure. But this reduces sharing opportunities.

---

### Critical Issue 2: `with` Chain Hashing Bug

**Problem**: The Round 2 answers confirm that `ExprWith::parentWith` is **NOT** included in the hash—only `prevWith` (the depth count). [^1]

This means different scope chains with the same nesting depth hash identically:

```nix
# These would hash the same despite different semantics:
with a; with b; x    # x comes from b, then a
with c; with d; x    # x comes from d, then c
```

**Fix**: Hash the entire `parentWith` pointer chain:

```cpp
void hashExprWith(HashSink& sink, ExprWith* expr) {
    feedTag(sink, ExprTag::With);
    feedExprHash(sink, expr->attrs);
    feedExprHash(sink, expr->body);
    
    // Hash the scope chain structure (NEW)
    ExprWith* w = expr->parentWith;
    while (w) {
        feedExprHash(sink, w->attrs);  // Identity of each enclosing with
        w = w->parentWith;
    }
    feedUInt32(sink, expr->prevWith);  // Keep depth for completeness
}
```

**For `ExprVar` with `fromWith`**:

```cpp
void hashExprVar(HashSink& sink, ExprVar* var) {
    feedTag(sink, ExprTag::Var);
    if (var->fromWith) {
        feedSymbol(sink, var->name);  // Variable name matters
        
        // Hash the with chain this variable resolves through
        ExprWith* w = var->fromWith;
        while (w) {
            feedExprHash(sink, w->attrs);
            w = w->parentWith;
        }
    } else {
        // Lexically bound: De Bruijn indices suffice
        feedUInt32(sink, var->level);
        feedUInt32(sink, var->displ);
    }
}
```

---

### Critical Issue 3: Disappearing Links vs. Finalizers

**Problem**: The plan proposes using `GC_register_finalizer` for cache cleanup, but Boehm GC has a critical limitation:

> "Cycles involving one or more finalizable objects are **never finalized**." 

If a cached `Value*` result contains a reference back to its thunk key (common in recursive structures), the finalizer will never run.

**Solution**: Use **disappearing links** (`GC_general_register_disappearing_link`) instead: [^2]

```cpp
void MemoCache::insert(MemoKey key, Value* result) {
    cache[key] = result;
    
    // Register disappearing link: cache[key] is set to NULL when key.thunk is collected
    GC_general_register_disappearing_link(
        reinterpret_cast<void**>(&cache[key]), 
        key.thunk);
}

Value* MemoCache::lookup(MemoKey key) {
    auto it = cache.find(key);
    if (it != cache.end() && it->second != nullptr) {
        return it->second;  // Still alive
    }
    return nullptr;  // Collected or not found
}
```

**Cleanup**: Periodically sweep the cache to remove entries where the value has been nulled:

```cpp
void MemoCache::sweep() {
    for (auto it = cache.begin(); it != cache.end(); ) {
        if (it->second == nullptr) {
            it = cache.erase(it);
        } else {
            ++it;
        }
    }
}
```

---

### Critical Issue 4: Cache Eviction

**Problem**: The cache grows unbounded. For full Nixpkgs evaluation: [^1]
- 100K-1M entries
- ~80MB memory

**Solution**: Add LRU eviction with configurable size limit:

```cpp
struct LRUMemoCache {
    size_t maxSize = 100000;  // Configurable
    std::list<MemoKey> lruOrder;
    std::unordered_map<MemoKey, std::pair<Value*, std::list<MemoKey>::iterator>> cache;
    
    void insert(MemoKey key, Value* value) {
        // Evict oldest if at capacity
        while (cache.size() >= maxSize) {
            auto oldest = lruOrder.back();
            cache.erase(oldest);
            lruOrder.pop_back();
        }
        
        lruOrder.push_front(key);
        cache[key] = {value, lruOrder.begin()};
    }
    
    Value* lookup(MemoKey key) {
        auto it = cache.find(key);
        if (it == cache.end()) return nullptr;
        
        // Move to front (most recently used)
        lruOrder.splice(lruOrder.begin(), lruOrder, it->second.second);
        return it->second.first;
    }
};
```

---

### Critical Issue 5: Two-Level Caching Architecture

**Problem**: The plan mentions two-level caching but the architecture is unclear. The Round 2 answers confirm that hash computation is 50-80% of memoization overhead. [^1]

**Clarified architecture**:

| Level | Key Type | Use Case | Performance |
|-------|----------|----------|-------------|
| **L1: Identity** | `(Expr*, Env*, generation)` | Intra-eval | O(1) pointer comparison |
| **L2: Content** | `SHA256(expr_content, env_content)` | Cross-eval, persistent | O(n) hash computation |

**Lookup order**:

```cpp
Value* memoizedEval(Expr* expr, Env* env) {
    // L1: Fast identity lookup (no hashing)
    IdentityKey idKey{expr, env, env->generation};
    if (auto* cached = identityCache.lookup(idKey)) {
        return cached;
    }
    
    // L2: Content lookup (expensive, but survives across evals)
    if (crossEvalCacheEnabled) {
        ContentKey contentKey = computeContentHash(expr, env);
        if (auto* cached = contentCache.lookup(contentKey)) {
            // Populate L1 for future fast lookups
            identityCache.insert(idKey, cached);
            return cached;
        }
    }
    
    // Cache miss: evaluate
    Value* result = realEval(expr, env);
    
    // Populate both caches
    identityCache.insert(idKey, result);
    if (crossEvalCacheEnabled && isCrossEvalCacheable(result)) {
        contentCache.insert(computeContentHash(expr, env), result);
    }
    
    return result;
}
```

---

### State-of-the-Art: Salsa Durability + Early Cutoff

Salsa's durability system is directly applicable to Nix: [^2]

**Version vector**: Instead of a single revision number, use a tuple `(v_low, v_med, v_high)`:

```cpp
struct VersionVector {
    uint64_t low;   // Environment variables, network fetches
    uint64_t med;   // User flakes, local files
    uint64_t high;  // Store paths, Nixpkgs
};
```

**Increment rules**: [^2]
- Incrementing `low` increments only `low`
- Incrementing `med` increments `low` and `med`
- Incrementing `high` increments all three

**Early cutoff**: When validating a cached query, compare its durability against the version vector. If the query only depends on `high` durability inputs and `v_high` hasn't changed, skip the entire subgraph. [^2]

**Mapping to Nix inputs**:

| Durability | Nix Inputs | Rationale |
|------------|------------|-----------|
| HIGH | Store paths, Nixpkgs channel | Rarely change during a session |
| MEDIUM | User flakes, local `.nix` files | Change during development |
| LOW | `$NIX_PATH`, env vars, network fetches | Can change at any time |

---

### Updated Implementation Plan

#### Phase 1: Correctness (Blocking)

| Task | Priority | Status | Notes |
|------|----------|--------|-------|
| Fix `with` chain hashing | **Blocking** | NEW | Hash `parentWith` chain, not just `prevWith` |
| Fix `ExprVar` with `fromWith` hashing | **Blocking** | NEW | Include symbol name AND scope chain |
| Add cycle detection to hashing | **Blocking** | Unchanged | Use visited set |
| Add generation counters to `Value` | **High** | Unchanged | Detect stale pointers |
| Use disappearing links (not finalizers) | **High** | CHANGED | Finalizers don't work with cycles |
| Add periodic cache sweep | **Medium** | NEW | Remove nulled entries |
| Add LRU eviction | **Medium** | NEW | Prevent unbounded growth |

#### Phase 2: Performance (High Impact)

| Task | Priority | Notes |
|------|----------|-------|
| Implement two-level caching | **High** | L1: identity (fast), L2: content (portable) |
| Implement AST hash-consing **after bindVars** | **High** | Must be post-mutation |
| Split cacheability predicates | **High** | `isIdentityCacheable()` vs `isContentCacheable()` |
| Add cache statistics | **Medium** | Hit rates, overhead breakdown |

#### Phase 3: Architecture (Should Do)

| Task | Priority | Notes |
|------|----------|-------|
| Implement durability levels | **High** | Version vector `(low, med, high)` |
| Implement early cutoff | **High** | Skip subgraphs when durable inputs unchanged |
| Add closed-term optimization | **Medium** | Wrap substituted closed terms |

#### Phase 4: Cross-Eval (Nice to Have)

| Task | Priority | Notes |
|------|----------|-------|
| Make `ExprParseFile` hashable | **Medium** | Hash by `path.fingerprint()` |
| Add LMDB backend | **Medium** | Persistent L2 cache |
| Wire EvalInputs fingerprint | **Low** | Include in persistent cache key |

---

### Questions for the Codebase Agent (Round 3)

#### Expr Mutation and Hash-Consing

1. **Where does `bindVars()` get called?** Is there a single entry point after parsing where we could insert hash-consing, or are there multiple call sites?

2. **Is there a "finalized" flag on `Expr` nodes?** Something that indicates `bindVars()` has completed and the node is immutable?

3. **Can `bindVars()` be called multiple times on the same `Expr`?** Or is it guaranteed to be called exactly once?

4. **Are there any other mutations to `Expr` nodes after `bindVars()`?** During evaluation, for example?

#### `with` Semantics

5. **How is `ExprVar::fromWith` used during evaluation?** Specifically, how does the evaluator traverse the `with` chain to resolve a variable?

6. **Can `ExprWith::parentWith` form cycles?** Or is it guaranteed to be a DAG/tree?

7. **What is the relationship between `ExprWith::prevWith` and `ExprWith::parentWith`?** The Round 2 answers mention both but their exact semantics are unclear.

#### Cache Architecture

8. **Where is `thunkMemoCache` accessed?** Is it only in `forceValue()`, or are there other access points?

9. **Is there existing infrastructure for cache statistics?** The Round 2 answers mention `nrThunkMemoHits` etc.—where are these defined and updated?

10. **How would L1 (identity) cache interact with the existing `thunkMemoCache`?** Replace it, or layer on top?

#### GC Integration

11. **Is `GC_general_register_disappearing_link` thread-safe?** If Nix moves to parallel evaluation, does this need synchronization?

12. **What happens if we register a disappearing link for an object that's already been collected?** Does it return an error?

13. **Can we register multiple disappearing links for the same object?** (e.g., if the same thunk is a key in multiple cache entries)

#### Durability

14. **Where are "inputs" tracked in the evaluator?** Is there a central place where file reads, env var accesses, and network fetches are recorded?

15. **How does `--pure-eval` currently work?** Does it block impure operations, or just track them?

16. **Is there existing infrastructure for tracking which inputs a value depends on?** Or would this need to be built from scratch?

---

### Summary of Key Changes from Round 2

| Round 2 Plan | Round 3 Change | Rationale |
|--------------|----------------|-----------|
| Hash-cons at allocation | Hash-cons **after `bindVars()`** | Expr nodes are mutated during binding |
| Hash `prevWith` for `with` | Hash **entire `parentWith` chain** | `prevWith` alone causes collisions |
| Use finalizers for GC cleanup | Use **disappearing links** | Finalizers don't work with cycles |
| Unbounded cache | Add **LRU eviction** | Prevent memory exhaustion |
| Single-level cache | **Two-level** (identity + content) | Identity is 50-80% faster |

The most critical fix is the **`with` chain hashing bug**—this is a correctness issue that could cause wrong evaluation results. The second priority is **disappearing links** to prevent memory leaks from cyclic structures.
```