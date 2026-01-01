# Memoization strategies for lazy evaluators: A Nix implementation guide

Implementing memoization in Nix's lazy evaluator requires solving a fundamental tension: **standard memoization demands equality checking and hashing, but comparing lazy values forces evaluation, destroying laziness semantics**. The solution lies in two-phase identity systems that defer structural comparison until values are naturally forced. This report synthesizes implementation strategies from GHC, OCaml, Racket, and academic literature, specifically targeting Nix's C++ codebase with Boehm GC.

## The core constraint shapes every design decision

Nix represents thunks as `(Env*, Expr*)` pairs—an environment pointer plus an AST expression pointer. The key invariant is that **memoization lookups must not force thunks**. This rules out naive structural hashing but opens three viable approaches: identity-based keying using pointer pairs, deferred structural hashing computed lazily on first force, and content-addressing at the expression level. GHC's StableName mechanism demonstrates this pattern can work at scale—it provides O(1) object identity without forcing evaluation by mapping heap addresses to stable integer indices through a runtime table that updates during garbage collection.

The Nix community has already identified this need. **Issue #6228** proposes a persistent evaluation cache primop for memoizing function calls across invocations, while the existing flake evaluation cache (Issue #2853) demonstrates the basic infrastructure but operates at too coarse a granularity for development workflows. Tvix, the Rust reimplementation, represents thunks as `Rc<RefCell<ThunkRepr>>` with explicit states (Suspended, Evaluated, Blackhole), providing a cleaner separation that could inform improvements to the C++ implementation.

## Identity-based keying enables lazy-safe memoization

The most direct approach uses pointer identity: a memo key of `(Env*, Expr*)` can be hashed and compared in O(1) without forcing anything. This works because Nix's expression AST is immutable after parsing, and environments form a stable tree structure during evaluation.

```cpp
struct MemoKey {
    Env* env;
    Expr* expr;
    
    bool operator==(const MemoKey& other) const {
        return env == other.env && expr == other.expr;
    }
};

struct MemoKeyHash {
    size_t operator()(const MemoKey& k) const {
        // Combine using absl::HashOf for quality mixing
        return absl::HashOf(k.env, k.expr);
    }
};
```

This approach has a critical limitation: **semantically equivalent expressions at different AST locations won't share cached results**. Two identical lambda bodies parsed from different files produce different `Expr*` pointers. For repeated Nixpkgs instantiations—the stated goal—this is actually acceptable because the same expression tree is reused within an evaluation, and cross-invocation caching requires persistence anyway.

GHC's StableName mechanism solves a related problem: objects move during garbage collection, invalidating address-based keys. The solution is a **stable name table** (SNT)—an array of entries indexed by stable integers, with a hash table mapping current addresses to SNT indices. When GC moves an object, the hash table is updated but the stable name index remains constant. For Nix with Boehm GC, which uses non-moving collection by default, raw pointers suffice, but a similar table would be needed if conservative GC ever caused pointer instability.

## Deferred structural hashing preserves laziness while enabling content-based sharing

For cross-expression sharing, values need structural comparison. The key insight from Filliâtre's hash-consing work is to **compute and cache structural hashes lazily at the moment values are forced**, then use these cached hashes for subsequent lookups.

Each Value gains a mutable hash cache slot:

```cpp
struct Value {
    ValueType type;
    union { /* payload */ };
    mutable std::optional<size_t> cached_hash;  // Computed on first access
    
    size_t structural_hash() const {
        if (!cached_hash) {
            // Only callable on forced values
            assert(type != tThunk);
            cached_hash = compute_structural_hash(*this);
        }
        return *cached_hash;
    }
};
```

The structural hash computation uses **depth-limited traversal** for cycle safety (borrowed from Racket's `equal-hash-code`). For attrsets, hash the sorted attribute names combined with children's hashes. For lists, combine element hashes with position encoding. Critically, **thunks within forced values are hashed by identity**, not structure—preserving laziness for nested unevaluated components:

```cpp
size_t compute_structural_hash(const Value& v, int depth = 16) {
    if (depth <= 0) return 0;  // Depth limit for cycles
    
    switch (v.type) {
        case tInt: return std::hash<int64_t>{}(v.integer);
        case tString: return hash_with_context(v.string, v.context);
        case tThunk: 
            // Hash thunk by identity, not content!
            return absl::HashOf(v.thunk.env, v.thunk.expr);
        case tAttrs:
            size_t h = 0;
            for (auto& [name, attr] : *v.attrs) {
                h ^= std::hash<Symbol>{}(name);
                h ^= compute_structural_hash(*attr.value, depth - 1) << 1;
            }
            return h;
        // ... other cases
    }
}
```

## String contexts require special handling in hashes

Nix strings carry **context metadata** tracking store path dependencies—a string `"/nix/store/abc..."` derived from a derivation carries that derivation's output reference. Two strings with identical characters but different contexts are semantically different for build purposes.

The hash must incorporate context:

```cpp
size_t hash_with_context(const char* str, const PathSet& context) {
    size_t h = std::hash<std::string_view>{}(str);
    for (const auto& path : context) {
        h ^= std::hash<std::string>{}(path) * 31;
    }
    return h;
}
```

Context also affects equality: strings compare equal only if both content and context match. This is already Nix's behavior but must be preserved in any memoization scheme.

## Weak references enable garbage-collectible memo tables

A memo cache that holds strong references to values prevents garbage collection of unused results, causing memory leaks during long evaluations. The solution uses **weak references** so cached values can be collected when unreachable elsewhere.

Boehm GC provides `GC_general_register_disappearing_link`:

```cpp
class WeakMemoCache {
    absl::flat_hash_map<MemoKey, Value*, MemoKeyHash> cache;
    
    void insert(const MemoKey& key, Value* val) {
        cache[key] = val;
        // Register weak reference: slot nulled when val collected
        GC_general_register_disappearing_link(
            reinterpret_cast<void**>(&cache[key]), val);
    }
    
    Value* lookup(const MemoKey& key) {
        auto it = cache.find(key);
        if (it != cache.end() && it->second != nullptr) {
            return it->second;  // Still alive
        }
        return nullptr;  // Not found or collected
    }
};
```

The challenge is that flat hash maps relocate values on rehash, invalidating registered weak links. Solutions include: using a node-based map (`std::unordered_map`), indirection through stable pointers, or registering/unregistering around rehashes. OCaml's hashcons library uses **weak arrays** as buckets specifically to handle this—Boehm's `GC_malloc_atomic` arrays with disappearing links per slot.

## GHC's full pattern combines stable names, weak pointers, and finalization

The definitive reference is Peyton Jones, Marlow, and Elliott's 1999 paper "Stretching the Storage Manager." GHC's memo table implementation uses:

1. **StableName for keys**: `makeStableName :: a -> IO (StableName a)` returns an integer ID stable across GC
2. **Weak pointers for values**: `mkWeak :: k -> v -> Maybe (IO ()) -> IO (Weak v)` creates a reference where `v` stays alive only while `k` is alive
3. **Finalization for cleanup**: When the key is collected, run a cleanup action removing the memo entry

The elegant insight is the **key-value semantic**: the cached *result* stays alive as long as the *input* is reachable. When the input becomes garbage, the result can be collected too. This matches memoization semantics perfectly.

For Nix, this translates to: memo entries are keyed by thunk identity (Env*, Expr*); entries become collectible when the thunk itself is no longer referenced. Using Boehm's finalization:

```cpp
void memo_finalizer(void* obj, void* cache_ptr) {
    auto* cache = static_cast<WeakMemoCache*>(cache_ptr);
    cache->remove_entry_for(obj);
}

void insert_with_cleanup(const MemoKey& key, Value* thunk, Value* result) {
    cache[key] = result;
    GC_register_finalizer(thunk, memo_finalizer, this, nullptr, nullptr);
}
```

## Handling cycles and infinite structures

Lazy evaluation permits cyclic and infinite structures—a list defined as `let xs = [1] ++ xs` creates a cycle. Hash computation must not diverge on these structures.

Three approaches from the literature:

**Depth limiting** (Racket's approach): Bound recursion depth during hashing. Simple but produces hash collisions for deep-but-different structures:
```cpp
size_t hash(Value& v, int depth = 16) {
    if (depth <= 0) return 0;
    // ... recurse with depth-1
}
```

**Visited set with identity** (GHC's observable sharing): Track visited nodes by pointer identity during traversal. If a node is revisited, return a fixed hash for "cycle here":
```cpp
size_t hash(Value& v, std::unordered_set<Value*>& visited) {
    if (!visited.insert(&v).second) {
        return CYCLE_MARKER;  // Already visited
    }
    // ... recurse
}
```

**Tarjan SCC + DAG hashing**: For structures where cycles matter semantically, compute strongly connected components, then hash the condensation DAG. This is expensive (graph isomorphism-hard in the general case) but produces correct distinct hashes for non-isomorphic cyclic structures.

For Nix, depth limiting is likely sufficient—deeply nested attrsets are rare, and hash collisions just mean slightly less cache sharing, not correctness issues.

## Attribute set merging affects memoization granularity

Nix's `//` operator and `pkgs/by-name` overlay pattern involve merging attribute sets. **PR #11290** proposes `mergeAttrsList` to optimize k-way merges, but memoization interacts with this: should the *merged* result be memoizable, or only the components?

The cleanest design treats merge as a pure operation with memoizable inputs:
```nix
# Memoization boundary at overlay application
final: prev: {
  hello = prev.hello.override { ... };
}
```

Each overlay function is independently memoizable by its inputs (the `final` and `prev` attrsets). The merge itself is cheap if inputs are cached. This matches Issue #6228's proposal for memoizing `f: j: import f j` where `f` is a store path.

## Practical hash map selection for C++ with Boehm GC

For the memo table itself, **`absl::flat_hash_map`** (Swiss Table) offers the best performance characteristics: SIMD-accelerated metadata filtering, flat memory layout with excellent cache locality, and **87.5% load factor** before resize. Integration with Boehm requires a custom allocator:

```cpp
template<typename T>
class BohmGCAllocator {
public:
    T* allocate(size_t n) {
        return static_cast<T*>(GC_MALLOC(n * sizeof(T)));
    }
    void deallocate(T*, size_t) { /* GC handles */ }
};

using MemoTable = absl::flat_hash_map<
    MemoKey, Value*,
    MemoKeyHash, std::equal_to<MemoKey>,
    BohmGCAllocator<std::pair<const MemoKey, Value*>>
>;
```

The table must be allocated as a GC root (`GC_MALLOC_UNCOLLECTABLE`) so the GC traces through to the stored pointers but doesn't collect the table itself.

For concurrent evaluation (PR #10938), consider **`parallel_hash_map`** (sharded Swiss Table) from the Abseil-derivatives that provide lock-free reads with per-shard locking on writes.

## Tvix's approach offers a comparison point

Tvix represents thunks with explicit state:
```rust
enum ThunkRepr {
    Suspended { lambda: Lambda, upvalues: Upvalues },
    Evaluated(Value),
    Blackhole,  // Infinite recursion detection
}
```

This state machine—Suspended → Evaluated—is cleaner than Nix's implicit thunk-or-value union. Memoization integrates naturally: check if evaluated, return cached value. Tvix uses Rust's `Rc<RefCell<>>` rather than GC, gaining deterministic cleanup but losing cycle collection.

The Nix C++ evaluator could adopt a similar explicit state without major refactoring by adding a `tEvaluated` variant that wraps a cached value while preserving the original thunk key for memoization purposes.

## A complete implementation strategy

Bringing together these components, the recommended approach for Nix:

1. **Two-level keying**: Primary key is `(Env*, Expr*)` pointer pair for O(1) identity lookup. Secondary key is structural hash computed lazily on force, enabling cross-invocation caching when serialized.

2. **Lazy hash computation**: Add a `mutable cached_hash` field to Value. Compute on first structural access. For thunks, hash by identity. Depth-limit to 16 for cycle safety.

3. **Weak memo entries**: Use `GC_general_register_disappearing_link` so cached results are collected when inputs become garbage.

4. **Swiss Table with GC allocator**: `absl::flat_hash_map` with `BohmGCAllocator`, table itself as GC root.

5. **String context inclusion**: Hash and equality must incorporate string contexts for correctness.

6. **Blackhole preservation**: Memoization must not interfere with infinite recursion detection—never cache a value that's currently being forced.

## Key papers for deeper understanding

The **foundational theoretical work** is Launchbury's 1993 "Natural Semantics for Lazy Evaluation" defining heap-based call-by-need, and the **implementation reference** is Peyton Jones' 1992 STG machine paper. For **weak references and stable names**, the 1999 IFL paper by Peyton Jones, Marlow, and Elliott is essential reading. **Hash-consing** details come from Conchon and Filliâtre's 2006 ML Workshop paper on type-safe modular hash-consing. **Optimal reduction theory**—showing when memoization is semantically sound—traces to Lévy's 1980 work and Lamping's 1990 POPL algorithm.

## Conclusion

Memoization in lazy evaluators requires respecting the laziness invariant: lookups must not force thunks. The solution is **identity-based primary keying** using pointer pairs, with **deferred structural hashing** computed as values are naturally forced. Weak references via Boehm's disappearing links prevent space leaks, while Swiss Tables provide the performance characteristics needed for large-scale evaluations. The Nix community has identified the need (Issue #6228) but implementation remains open—these techniques from GHC, OCaml, and Racket provide a proven foundation for that work.