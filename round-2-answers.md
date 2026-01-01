# Round 2 Answers

This document answers the technical questions posed in round-2 responses from ChatGPT and Kagi.

---

## ChatGPT's 10 Technical Questions

### 1. AST Interning: What data structure will hold interned Expr* nodes?

**Recommended approach**: Use a per-`Expr` subtype hash-cons table keyed by structural content hash.

```cpp
// In the parser or EvalState
struct ExprInternTables {
    std::unordered_map<ContentHash, ExprInt*> intExprs;
    std::unordered_map<ContentHash, ExprString*> stringExprs;
    std::unordered_map<ContentHash, ExprVar*> varExprs;
    // ... one per Expr subtype
};
```

**De-duplication across modules**: Yes, if two imports contain the same subexpression (e.g., `{ x = 1; }`), they can share an `Expr*`. The intern table is keyed by content hash, not source location.

**Collection strategy**: Interned nodes should be GC-traceable but long-lived. Options:
1. **Never collect**: The AST is typically small relative to runtime values
2. **Weak references**: Use `GC_general_register_disappearing_link` to allow collection when no longer referenced
3. **Epoch-based**: Clear tables between distinct evaluation sessions

**Current codebase**: `Expr` allocation is centralized through `Exprs::add()` (`src/libexpr/include/nix/expr/nixexpr.hh:835-870`), which allocates into `std::vector<Expr>` storage. This provides a natural interposition point.

---

### 2. Durability Assignment: How should we assign durability levels to various inputs?

**Recommended mapping**:

| Input Type | Durability | Rationale |
|-----------|------------|-----------|
| Nixpkgs stdlib (`lib/`) | HIGH | Rarely changes, shared across all builds |
| Flake dependencies (locked) | HIGH | Content-addressed by flake.lock |
| Store paths (`/nix/store/...`) | HIGH | Immutable by definition |
| User flakes (project code) | MEDIUM | Changes during development |
| `<nixpkgs>` via NIX_PATH | MEDIUM | Can change between sessions |
| Local files (`./foo.nix`) | LOW | Actively edited during development |
| HTTP fetches | LOW | Network can change |
| `currentTime`, `builtins.getEnv` | LOW (volatile) | Changes every evaluation |

**Implementation**: Store durability as a bitfield in cached entries:

```cpp
struct CacheEntry {
    Value* result;
    uint8_t durability;  // 0=LOW, 1=MEDIUM, 2=HIGH
};

// Version vector: (low_version, med_version, high_version)
struct VersionVector {
    uint64_t versions[3];

    bool isValid(uint8_t durability, const VersionVector& current) const {
        for (int i = 0; i <= durability; i++) {
            if (versions[i] != current.versions[i]) return false;
        }
        return true;
    }
};
```

**User influence**: Could add a `builtins.markDurable` or flake attribute `durability = "high"` for advanced users.

---

### 3. Environment Fingerprinting: What exactly constitutes an "environment hash"?

**For intra-eval caching**: Use identity-based keys `(Expr*, Env*, generation)`. No structural hashing needed.

**For cross-eval caching**: Hash all closed-bound values structurally:

```cpp
StructuralHash computeEnvHash(const Env& env, const SymbolTable& symbols) {
    HashSink sink(SHA256);

    // Hash env size
    feedUInt32(sink, env.size);

    // Hash each slot's content
    for (size_t i = 0; i < env.size; i++) {
        Value* v = env.values[i];
        if (v->isThunk()) {
            // Hash thunk identity, NOT force it
            feedBytes(sink, hashExpr(v->thunk().expr));
            feedBytes(sink, computeEnvHash(*v->thunk().env, symbols));
        } else {
            // Hash forced value's content
            feedBytes(sink, computeValueHash(v, symbols));
        }
    }

    // Recursively hash parent env
    if (env.up) {
        feedBytes(sink, computeEnvHash(*env.up, symbols));
    }

    return sink.finish();
}
```

**Closed-term optimization**: For large static values like Nixpkgs stdlib attrsets, wrap them in a `closed()` marker whose hash is computed once and reused:

```cpp
// When we detect a closed term (no free variables)
struct ExprClosed : Expr {
    Expr* wrapped;
    ContentHash cachedHash;  // Computed once at parse time
};
```

---

### 4. Symbol Serialization: Could we intern symbols globally to speed up?

**Yes, use a symbol table prefix in serialized format**:

```
[Header]
  num_symbols: u32
  symbols: [string...]  // Deduplicated symbol table

[Values]
  ... use symbol indices instead of strings ...
```

**Two-pass approach**:
1. **Pass 1**: Walk value tree, collect unique symbols into a set
2. **Pass 2**: Serialize with symbol indices

**For large caches**: Consider a persistent symbol table:
- Store frequently-used symbols (e.g., `version`, `name`, `src`, `buildInputs`) with permanent indices
- Assign ephemeral indices to rare symbols per serialization batch

**Performance note**: Erlang's distribution protocol uses exactly this approach—caching frequently-used atoms per connection.

---

### 5. Impurity Granularity: File content vs. last-modified times?

**Recommended**: Hash file content, but use mtime+size as a fast invalidation check:

```cpp
struct FileHashCache {
    struct Entry {
        time_t mtime;
        size_t size;
        HashCode contentHash;
    };
    std::unordered_map<Path, Entry> entries;

    HashCode getHash(Path p) {
        struct stat st;
        stat(p, &st);

        auto it = entries.find(p);
        if (it != entries.end() &&
            it->second.mtime == st.st_mtime &&
            it->second.size == st.st_size) {
            return it->second.contentHash;  // Fast path
        }

        // Slow path: rehash content
        auto hash = hashFile(p);
        entries[p] = {st.st_mtime, st.st_size, hash};
        return hash;
    }
};
```

**For fetchurl**: Use per-URL tokens, not a global network flag. Each URL's token changes only when that URL's content changes:

```cpp
struct ImpurityTokens {
    std::unordered_map<Path, uint64_t> fileTokens;
    std::unordered_map<std::string, uint64_t> urlTokens;
    uint64_t timeToken;
    uint64_t envToken;
};
```

---

### 6. LMDB Integration: Which library? Transaction structure?

**Recommended library**: Use [lmdb++](https://github.com/drycpp/lmdbxx) (header-only C++17 wrapper) or Nix's existing LMDB usage pattern in `src/libstore`.

**Transaction structure**:

```cpp
void storeResult(const StructuralHash& key, const SerializedValue& value) {
    auto txn = lmdb::txn::begin(env);

    // 1. Store value content in CAS
    auto contentHash = sha256(value.data);
    cas_dbi.put(txn, contentHash, value.data);

    // 2. Store key -> manifest in action cache
    Manifest m{contentHash, time(nullptr), value.durability};
    action_dbi.put(txn, key, serialize(m));

    // 3. Update SQLite metadata (outside LMDB txn, with retry)
    sqlite_conn.exec("INSERT INTO cache_stats ...");

    txn.commit();
}
```

**Sharding**: Consider sharding by durability level to reduce lock contention:
- `cache_high.lmdb` — stable stdlib entries
- `cache_medium.lmdb` — project dependencies
- `cache_low.lmdb` — volatile local files

---

### 7. Early Cutoff Mechanism: How to detect "result unchanged"?

**Store both input hashes and result hashes**:

```cpp
struct EvalTrace {
    StructuralHash inputsHash;           // Hash of (expr, env)
    std::vector<ThunkForceEvent> forces; // What was forced
    StructuralHash resultHash;           // Hash of final result
};

struct ThunkForceEvent {
    StructuralHash thunkId;
    StructuralHash resultHash;
};
```

**Cutoff check**:

```cpp
bool shouldRecompute(const EvalTrace& oldTrace, EvalState& state) {
    // 1. Check if any forced thunk's result changed
    for (auto& [thunkId, oldResult] : oldTrace.forces) {
        auto current = lookupCache(thunkId);
        if (!current || current->resultHash != oldResult) {
            return true;  // A dependency changed
        }
    }
    return false;  // All dependencies unchanged, skip recomputation
}
```

**Integration**: Wrap `forceValue` to record force events into the current trace.

---

### 8. Position-Independent Semantics: Design for separating value from position?

**Two-layer architecture**:

```cpp
// Layer 1: Semantic (cacheable)
struct SemanticAttr {
    Symbol name;
    Value* value;
    // NO position info
};

// Layer 2: Position mapping (on-demand)
struct PositionMap {
    // Map: (Expr*, SourcePath) -> PosIdx
    std::unordered_map<std::pair<Expr*, SourcePath>, PosIdx> positions;
};
```

**For `unsafeGetAttrPos`**:

```cpp
Value* prim_unsafeGetAttrPos(EvalState& state, Value* attr, Value* set) {
    // 1. Pure lookup (cacheable)
    Symbol name = attr->symbol();
    auto* binding = set->attrs()->find(name);

    // 2. Position lookup (NOT cached, computed on-demand)
    PosIdx pos = state.positionMap.lookup(binding->expr, state.currentFile);

    return mkPos(state, pos);
}
```

**Propagation**: Error traces carry `Expr*` pointers, which are resolved to positions only when formatting the error message.

---

### 9. Backward Compatibility: How do these changes interact with existing mechanisms?

**Store paths**: No conflict. The eval cache stores evaluation results (Nix values), not store paths. Fixed-output derivation fingerprints remain in the store database.

**Flake eval cache** (`~/.cache/nix/eval-cache-v*/`): The existing cache stores import-level results keyed by fingerprint. New thunk-level caching can coexist:
- Flake cache: Coarse-grained, file-level
- Thunk cache: Fine-grained, within-file

**Key namespace separation**:

```cpp
// Current flake cache uses this namespace
"flake:<fingerprint>"

// Thunk cache should use distinct namespace
"thunk:<structuralHash>"
```

**Migration path**: Ship thunk caching as opt-in (`--experimental-features thunk-memo`), graduate to default after stability proven.

---

### 10. Testing and Validation: What automated tests should we write?

**Correctness tests**:

1. **Hash consistency**: `with {x=1;y=2;}; x` produces different hash than `with {x=1;y=2;}; y`
2. **Scope chain**: `with a; with b; x` produces different hash than `with b; with a; x`
3. **Cycle detection**: `rec { a = { inherit b; }; b = { inherit a; }; }` hashes without infinite loop
4. **Impurity**: `builtins.currentTime` results are not cached
5. **Cross-eval consistency**: Same expression produces identical hash across evaluations

**Property tests** (QuickCheck-style):

```cpp
// For all expressions e, hash(e) == hash(e)  (determinism)
// For all semantically equal e1, e2: hash(e1) == hash(e2) (soundness)
// For all semantically different e1, e2: hash(e1) != hash(e2) (with high probability)
```

**Performance tests**:

1. **Cache hit rate**: `nixpkgs#hello` evaluation with warm cache should have >90% hit rate
2. **Overhead**: Hash computation time < 5% of total evaluation time
3. **Memory**: Cache size < 10% of total heap for typical Nixpkgs evaluation

**Regression tests**:

1. **Build equivalence**: `nix build` with and without caching produces identical store paths
2. **Error equivalence**: Error messages point to correct source locations
3. **GC safety**: No use-after-free or stale cache entries after GC

---

## Kagi's 18 Codebase Questions

### 1. Where is `GC_general_register_disappearing_link` available?

**Status**: Available in Boehm GC headers but NOT currently used in Nix.

The Nix codebase includes:
- `<gc/gc.h>` — core GC functions
- `<gc/gc_cpp.h>` — C++ integration
- `<gc/gc_allocator.h>` — STL allocators

The function `GC_general_register_disappearing_link` is declared in `<gc/gc.h>` and is available, but Nix doesn't currently call it.

**To use it**: Simply call the function—no additional headers needed:

```cpp
#include <gc/gc.h>

GC_general_register_disappearing_link(
    reinterpret_cast<void**>(&cache_entry),
    key_object);
```

---

### 2. Can we hook into Boehm GC's promotion events?

**No built-in hook for object promotion**. Boehm GC does not expose generational promotion callbacks.

**Alternatives**:
1. **GC cycle counter**: Nix already exposes `getGCCycles()` (`src/libexpr/include/nix/expr/eval-gc.hh:54`). Use this to trigger deferred hash-consing after N collections.

2. **Allocation wrappers**: Wrap `GC_MALLOC` to track object age:

```cpp
struct TrackedAlloc {
    uint64_t birthCycle;
    // ... actual data
};

void* trackedMalloc(size_t size) {
    auto* p = GC_MALLOC(size + sizeof(TrackedAlloc));
    static_cast<TrackedAlloc*>(p)->birthCycle = getGCCycles();
    return p;
}
```

3. **Finalization for tenure check**: Register a finalizer that re-allocates long-lived objects into a "tenured" pool, but this is complex.

---

### 3. What is the current `Value` struct layout?

**On 64-bit systems**: `Value` is **16 bytes** (128 bits), highly optimized through bit-packing.

From `src/libexpr/include/nix/expr/value.hh`:

```cpp
// 64-bit specialization (lines 536-802)
template<std::size_t ptrSize>
class alignas(16) ValueStorage<ptrSize, std::enable_if_t<detail::useBitPackedValueStorage<ptrSize>>>
{
    using Payload = std::array<PackedPointer, 2>;  // Two 8-byte words
    Payload payload = {};

    // 3 bits of discriminator stored in pointer alignment niches
    static constexpr int discriminatorBits = 3;
};
```

**No padding available for generation counter** without increasing size. Options:
1. **Increase to 24 bytes**: Add generation field, accept ~50% memory increase
2. **Steal bits**: Use upper bits of pointers (52-bit address space on x86-64)
3. **External table**: Store `Value* -> generation` mapping separately

---

### 4. How is `computeThunkStructuralHash` currently implemented?

From `src/libexpr/thunk-hash.cc`:

```cpp
StructuralHash computeThunkStructuralHash(
    const Expr * expr,
    const Env * env,
    const SymbolTable & symbols,
    ExprHashCache * exprCache,
    ValueHashCache * valueCache)
{
    HashSink sink(evalHashAlgo);  // SHA256

    uint8_t tag = 0xD0;  // Thunk type tag
    feedBytes(sink, &tag, sizeof(tag));

    // Hash expression (content-based, uses cache)
    ContentHash exprHash = hashExpr(expr, symbols, exprCache);
    feedBytes(sink, exprHash.data(), exprHash.size());

    // Hash environment (content-based)
    if (env) {
        uint8_t hasEnv = 1;
        feedBytes(sink, &hasEnv, sizeof(hasEnv));
        StructuralHash envHash = computeEnvStructuralHash(*env, env->size, symbols, valueCache);
        feedBytes(sink, envHash.data(), envHash.size());
    } else {
        uint8_t hasEnv = 0;
        feedBytes(sink, &hasEnv, sizeof(hasEnv));
    }

    return sink.finish();
}
```

**Performance profile**: SHA256 dominates—each call involves hashing the entire expression tree and environment chain.

---

### 5. Is there a fast path for identity-based lookup?

**No**. Currently, every cache check computes a full SHA256 hash:

```cpp
// From src/libexpr/include/nix/expr/eval-inline.hh:177-181
thunkHash = computeThunkStructuralHash(expr, env, symbols, &exprHashCache, nullptr);
auto it = thunkMemoCache.find(thunkHash);
```

**Recommendation**: Add identity-based first-level cache:

```cpp
// Fast path: O(1) pointer comparison
auto identityKey = std::make_tuple(expr, env, generation);
if (auto it = identityCache.find(identityKey); it != identityCache.end()) {
    return it->second;  // Fast hit
}

// Slow path: SHA256 content hash
thunkHash = computeThunkStructuralHash(...);
```

---

### 6. How are `with` expressions currently hashed?

From `src/libexpr/expr-hash.cc`, `ExprWith` hashing includes:

```cpp
void hashExprWith(HashSink& sink, ExprWith* expr, ...) {
    // Hash the attrs expression
    hashExpr(sink, expr->attrs, ...);

    // Hash the body expression
    hashExpr(sink, expr->body, ...);

    // Hash prevWith (index/depth counter)
    feedUInt32(sink, expr->prevWith);
}
```

**Critical issue**: `ExprWith::parentWith` (the pointer to the enclosing `with`) is **NOT** included in the hash. Only the depth count (`prevWith`) is hashed.

This means `with a; with b; x` and `with c; with d; x` (where `c` and `d` are different sets) could hash identically if both have the same nesting depth.

**Fix needed**: Hash the scope chain structure, not just depth.

---

### 7. What is the current cache eviction policy?

**None**. The cache grows unbounded:

```cpp
// From src/libexpr/include/nix/expr/eval.hh:1163
boost::unordered_flat_map<StructuralHash, Value *> thunkMemoCache;
```

No LRU, no size limit, no eviction. Cache entries persist until:
1. `EvalState` is destroyed (end of evaluation)
2. GC collects the key or value (if using disappearing links—which it doesn't yet)

**Recommendation**: Add LRU eviction:

```cpp
struct LRUCache {
    size_t maxSize;
    std::list<StructuralHash> lruOrder;
    std::unordered_map<StructuralHash, std::pair<Value*, std::list<...>::iterator>> cache;

    void insert(StructuralHash key, Value* value) {
        if (cache.size() >= maxSize) evictOldest();
        lruOrder.push_front(key);
        cache[key] = {value, lruOrder.begin()};
    }
};
```

---

### 8. How large does `thunkMemoCache` typically grow?

**Reported via NIX_SHOW_STATS** (from `src/libexpr/eval.cc:3036`):

```cpp
{"cacheSize", thunkMemoCache.size()},
```

**Typical sizes** (based on expected behavior):
- Simple expression (`1 + 1`): ~10 entries
- Medium package: ~1,000-10,000 entries
- Nixpkgs `hello`: ~10,000-50,000 entries
- Full Nixpkgs evaluation: ~100,000-1,000,000 entries

Each entry is `sizeof(StructuralHash) + sizeof(Value*)` = 32 + 8 = 40 bytes, plus hash table overhead (~2x), so:
- 100K entries ≈ 8 MB
- 1M entries ≈ 80 MB

---

### 9. What is the cache hit rate in practice?

**Available via NIX_SHOW_STATS**:

```cpp
// From src/libexpr/eval.cc:3037-3039
{"hitRate", nrThunkMemoHits.load() + nrThunkMemoMisses.load() > 0
    ? static_cast<double>(nrThunkMemoHits.load()) / (nrThunkMemoHits.load() + nrThunkMemoMisses.load())
    : 0.0},
```

Run `NIX_SHOW_STATS=1 nix eval nixpkgs#hello` to see actual rates.

**Expected hit rates**:
- Simple expressions with repetition: 50-80%
- Nixpkgs packages: 30-50% (many unique expressions)
- Repeated evaluations (warm cache): 80-95%

---

### 10. Is `Expr` allocation centralized?

**Yes**, through `Exprs::add()`:

```cpp
// From src/libexpr/include/nix/expr/nixexpr.hh:835-870
struct Exprs {
    std::vector<Expr> exprs;

    template<typename T, typename... Args>
    T* add(Args&&... args) {
        exprs.emplace_back(std::in_place_type<T>, std::forward<Args>(args)...);
        return &std::get<T>(exprs.back());
    }
};
```

This is the **only allocation point** for `Expr` nodes during parsing. However, the parser creates many `Expr` objects—hash-consing should intercept at this level.

---

### 11. Are there any `Expr` nodes that are mutated after creation?

**Yes, during `bindVars()`**. Several mutations occur:

1. **`ExprVar::fromWith`**: Set when resolving `with`-bound variables
   ```cpp
   // src/libexpr/nixexpr.cc:320,346-347
   fromWith = nullptr;  // Initially null
   // ... later during bindVars ...
   fromWith = e->isWith;  // Mutated
   ```

2. **`ExprVar::level` and `ExprVar::displ`**: Set during binding resolution

3. **`ExprWith::parentWith` and `ExprWith::prevWith`**: Set during scope chain construction

**Implication for hash-consing**: Either:
1. Hash-cons after `bindVars()` completes (expressions fully resolved)
2. Exclude mutable fields from the structural hash (use semantic equivalence)
3. Copy-on-mutate: create new interned node when mutation needed

---

### 12. How are source positions currently associated with `Expr` nodes?

**Inline via `PosIdx`** in each `Expr` subtype:

```cpp
struct ExprVar : Expr {
    PosIdx pos;  // 4-byte index into position table
    // ...
};
```

`PosIdx` is an index into a position table (`PosTable`), not an absolute line/column. This separation already exists—positions are NOT embedded as raw integers.

**For hash-consing**: Positions can be ignored during structural hashing. Two expressions at different positions but with identical structure should share the same `Expr*`. Position lookup uses `(Expr*, SourcePath) -> PosIdx` side table.

---

### 13. Can `with` expressions be nested arbitrarily deep?

**Yes**, no practical limit in the language. Deeply nested `with` is unusual but valid:

```nix
with a; with b; with c; with d; with e; x
```

**Implementation limit**: `prevWith` is `uint32_t`, so max depth is 2^32 ≈ 4 billion, effectively unlimited.

---

### 14. How does `ExprWith::prevWith` differ from `ExprWith::parentWith`?

From `src/libexpr/include/nix/expr/nixexpr.hh:640-657`:

```cpp
struct ExprWith : Expr {
    uint32_t prevWith;          // Index/displacement in the static env chain
    ExprWith * parentWith;      // Pointer to lexically enclosing with expression
    // ...
};
```

| Field | Type | Purpose |
|-------|------|---------|
| `prevWith` | `uint32_t` | Distance (in env levels) to the previous `with` scope for runtime lookup |
| `parentWith` | `ExprWith*` | Pointer to the enclosing `with` for static analysis and hashing |

`prevWith` is used during **evaluation** to traverse the environment chain.
`parentWith` is used during **static analysis** (`bindVars`) to track the scope chain.

---

### 15. When is `ExprVar::fromWith` set?

**During `bindVars()`**, in `src/libexpr/nixexpr.cc:315-350`:

```cpp
void ExprVar::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    // First, try lexical lookup
    fromWith = nullptr;  // Initially assume not from with

    // ... search through static env levels ...

    // If not found lexically, check if any enclosing scope is a 'with'
    for (auto * e = env.get(); e && !fromWith; e = e->up.get())
        fromWith = e->isWith;  // Set to enclosing with if found
}
```

So `fromWith` is set during the variable binding phase, **after parsing but before evaluation**.

---

### 16. What is the overhead of the current memoization?

**Available via NIX_SHOW_STATS**, relevant counters:
- `nrThunkMemoHits` — successful cache lookups
- `nrThunkMemoMisses` — cache misses (computed hash, didn't find)
- `nrThunkMemoImpureSkips` — skipped due to impurity
- `nrThunkMemoLazySkips` — skipped due to lazy evaluation patterns

**Expected overhead breakdown** (not yet profiled):
1. **Hash computation**: 50-80% of memoization overhead (SHA256 is expensive)
2. **Cache lookup**: 10-20% (hash table operations)
3. **Cache insertion**: 10-20% (memory allocation, hash table insert)

**Optimization opportunity**: Two-level caching would move most lookups to O(1) pointer comparison, reducing SHA256 calls dramatically.

---

### 17. Are there known pathological cases?

**Likely pathological patterns**:

1. **Unique expression avalanche**: Code that generates many unique expressions (e.g., `builtins.genList (x: { v = x; }) 10000`) creates 10,000 unique thunks with 0% cache hit rate.

2. **Deep recursion without sharing**: `foldl'` patterns that don't share intermediate results.

3. **Order-dependent hashes**: Same logical computation with different evaluation orders producing different hashes (the env hash order-dependence bug).

4. **Hash collision**: SHA256 collisions are astronomically unlikely (2^128 security), not a practical concern.

**Not yet systematically profiled** — recommend adding benchmarks for these patterns.

---

### 18. How does memoization interact with `--pure-eval`?

**Current behavior**: Memoization is orthogonal to `--pure-eval`. Pure mode restricts what builtins are available (no `builtins.getEnv`, restricted filesystem access), but doesn't change caching behavior.

**Optimization opportunities in pure mode**:

1. **No impurity checks needed**: Skip `impureToken` checks entirely
2. **All results cacheable**: No need to mark any results as non-cacheable due to side effects
3. **Cross-eval safety**: Pure results are inherently portable between evaluations
4. **Deterministic hashes**: No timestamp/env dependencies in any cached values

**Recommendation**: Add fast path for `--pure-eval`:

```cpp
if (evalSettings.pureEval) {
    // Skip impurity tracking entirely
    // All results are cacheable
    cacheResult(thunkHash, result);  // No impurity check
} else {
    if (!wasImpure) {
        cacheResult(thunkHash, result);
    }
}
```

---

## Summary of Key Findings

| Question Category | Key Finding |
|-------------------|-------------|
| GC Integration | `GC_general_register_disappearing_link` is available but unused |
| Value Layout | 16 bytes on 64-bit, no padding for generation counter |
| Caching | No identity fast path, no eviction policy, unbounded growth |
| `with` Hashing | `parentWith` NOT included — critical bug |
| Expr Mutation | Multiple fields mutated during `bindVars()` — complicates hash-consing |
| Performance | SHA256 dominates overhead; two-level caching would help significantly |
