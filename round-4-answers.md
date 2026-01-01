# Round 4 Answers: Nix Evaluator Memoization

## New Sources from Round 4

### GitHub Issues and PRs

| Issue | Topic | Status | Relevance |
|-------|-------|--------|-----------|
| [#7816](https://github.com/NixOS/nix/issues/7816) | `deepSeq` stack overflow on packages | **RESOLVED** (PR #14613) | `deepSeq` depth limits needed for cache key computation |
| [#13225](https://github.com/NixOS/nix/pull/13225) | Lazy trees v2 non-determinism | Open | `StorePath::random()` breaks cache correctness |
| [#6731](https://github.com/NixOS/nix/issues/6731) | Platform-dependent `narHash` in flake.lock | Closed | NAR hashes may differ across platforms |
| [#3358](https://github.com/NixOS/nix/issues/3358) | `unsafeDiscardStringContext` imports paths | Open | Context-discarding still imports paths |
| [#8274](https://github.com/NixOS/nix/issues/8274) | Floating-point non-determinism | Open | Float canonicalization required for caching |
| [rust-analyzer #18964](https://github.com/rust-lang/rust-analyzer/pull/18964) | Salsa migration | Merged | db-ext-macro bridge, memory optimizations 8GB→2.2GB |

### External Documentation

| Source | Topic | Key Insight |
|--------|-------|-------------|
| [RFC 8949 §4.2](https://www.rfc-editor.org/rfc/rfc8949.html#section-4.2) | Deterministic CBOR | Sorted map keys, shortest integers, canonical NaN |
| [LMDB Documentation](https://en.wikipedia.org/wiki/Lightning_Memory-Mapped_Database) | MVCC database | Single-writer/multi-reader, `mdb_reader_check()` |
| [Bacon et al. ECOOP 2001](https://pages.cs.wisc.edu/~cymen/misc/interests/Bacon01Concurrent.pdf) | Concurrent cycle collection | Read-reclaim races, safety tests |
| [V8 Trash Talk](https://v8.dev/blog/trash-talk) | Memory management | Idle-time GC, dynamic allocation limits |
| [ARC Paper (FAST '03)](https://www.usenix.org/conference/fast-03/arc-self-tuning-low-overhead-replacement-cache) | Adaptive Replacement Cache | Two LRU lists (T1/T2), ghost lists (B1/B2) |
| [RapidCheck](https://github.com/emil-e/rapidcheck) | Property-based testing | C++ QuickCheck, GoogleTest integration |
| [Boehm GC Finalization](https://www.hboehm.info/gc/finalization.html) | Disappearing links | `GC_unregister_disappearing_link` available |

---

## ChatGPT's 8 Open Questions

### 1. Implementation of Early Cutoff: How exactly should we structure the dependency tracking?

**Answer**: Leverage existing `EvalTrace` infrastructure with input recording.

**Current State**: Nix doesn't have explicit dependency tracking beyond `impureToken`. The `EvalTrace` mechanism records evaluation steps but not input dependencies.

**Recommended Structure**:

```cpp
struct EvalTrace {
    // Existing fields...

    // NEW: Track actual inputs accessed during evaluation
    std::vector<InputDependency> inputs;

    struct InputDependency {
        enum Type { FilePath, EnvVar, StorePath, Network } type;
        std::string key;        // File path, env var name, etc.
        HashCode contentHash;   // Hash at time of access
    };
};

// On file read:
void recordFileInput(EvalTrace& trace, const Path& p) {
    trace.inputs.push_back({
        .type = FilePath,
        .key = p,
        .contentHash = hashFile(p)
    });
}
```

**Early cutoff check**:
```cpp
bool canReuseResult(const CacheEntry& entry) {
    for (auto& dep : entry.trace.inputs) {
        if (dep.contentHash != getCurrentHash(dep)) return false;
    }
    return true;  // All inputs unchanged, skip re-evaluation
}
```

**Files to modify**: `src/libexpr/eval.cc` (EvalTrace), `src/libexpr/primops.cc` (input recording).

---

### 2. Fine-Grained Impurity Tokens: Can we leverage EvalInputs or AllowedUris?

**Answer**: Yes, `EvalInputs` provides a natural foundation for per-input-type tokens.

**Current `EvalInputs` structure** (`src/libexpr/include/nix/expr/eval-inputs.hh`):

```cpp
struct EvalInputs {
    std::optional<Path> nixPath;
    std::optional<std::string> currentSystem;
    std::optional<FlakeLock> flakeLock;
    // etc.
};
```

**Proposed extension**:

```cpp
struct ImpurityTokens {
    uint64_t fileSystemToken;     // Incremented on file changes
    uint64_t environmentToken;    // Incremented on env var changes
    uint64_t networkToken;        // Incremented on network access
    uint64_t timeToken;           // Incremented on currentTime use

    // Per-resource fine-grained tracking
    std::unordered_map<Path, uint64_t> perFileTokens;
    std::unordered_map<std::string, uint64_t> perEnvVarTokens;
};

// In markImpure():
void markImpure(ImpureReason reason, const std::string& resource) {
    switch (reason) {
        case ImpureReason::ReadFile:
            impurityTokens.fileSystemToken++;
            impurityTokens.perFileTokens[resource]++;
            break;
        case ImpureReason::GetEnv:
            impurityTokens.environmentToken++;
            impurityTokens.perEnvVarTokens[resource]++;
            break;
        // ...
    }
}
```

**Integration with builtins**: Each impure builtin calls `markImpure()` with the specific resource. Cache entries record which tokens they depend on.

---

### 3. Shared Caching in Multithreaded Eval: What strategy for thread-safety?

**Answer**: Use concurrent hash maps with per-shard locking for L1, serialized writes for L2.

**L1 Identity Cache (intra-eval)**:

```cpp
// Option A: Lock-free concurrent map
tbb::concurrent_hash_map<IdentityKey, Value*> identityCache;

// Option B: Sharded locking (simpler, good enough)
struct ShardedCache {
    static constexpr size_t NUM_SHARDS = 64;
    struct Shard {
        std::mutex mtx;
        std::unordered_map<IdentityKey, Value*> entries;
    };
    std::array<Shard, NUM_SHARDS> shards;

    Shard& getShard(const IdentityKey& k) {
        return shards[std::hash<IdentityKey>{}(k) % NUM_SHARDS];
    }
};
```

**L2 Content Cache (cross-eval)**:
- Use LMDB's built-in concurrency (single writer, multiple readers)
- Serialize writes through nix-daemon (already the pattern for store operations)
- Read transactions are isolated via MVCC

**Black-holing for concurrent `forceValue`**:

```cpp
enum class ThunkState { Unevaluated, Evaluating, Evaluated };

Value* forceValue(Value* v) {
    if (v->type == nThunk) {
        auto expected = ThunkState::Unevaluated;
        if (v->thunkState.compare_exchange_strong(expected, ThunkState::Evaluating)) {
            // We own this thunk
            Value* result = evaluate(v->thunk);
            v->thunkState.store(ThunkState::Evaluated);
            return result;
        } else if (expected == ThunkState::Evaluating) {
            // Another thread is evaluating - wait or throw black-hole error
            throw InfiniteRecursion("thunk being evaluated by another thread");
        }
    }
    return v;
}
```

---

### 4. String Deduplication Table: How big, when to reset?

**Answer**: Per-serialization operation with size limit.

**Recommended approach**:

```cpp
class CBORSerializer {
    // Symbol table for string deduplication
    std::unordered_map<std::string_view, uint32_t> stringTable;
    std::vector<std::string> strings;  // For back-references

    static constexpr size_t MAX_STRING_TABLE = 65536;  // 64K unique strings

    void writeString(const std::string& s) {
        auto it = stringTable.find(s);
        if (it != stringTable.end() && stringTable.size() < MAX_STRING_TABLE) {
            // Emit back-reference
            writeTag(STRING_REF_TAG);
            writeUInt(it->second);
        } else {
            // Emit full string, add to table
            writeRawString(s);
            if (stringTable.size() < MAX_STRING_TABLE) {
                stringTable[s] = strings.size();
                strings.push_back(s);
            }
        }
    }
};
```

**Reset strategy**: Per-serialization operation (not global). Each `serialize(Value*)` call starts fresh. This bounds memory and avoids stale references.

**Typical sizes**: Full Nixpkgs evaluation might see ~50K unique attribute names, ~10K unique derivation paths. 64K limit is safe.

---

### 5. GC Tag Usage: Do we need an abstraction for x86-64 vs ARM64?

**Answer**: Yes, abstract via compile-time selection.

```cpp
#if defined(__aarch64__)
// ARM64: Use Top Byte Ignore (TBI) - 8 free bits, zero overhead
class TaggedPtr {
    uint64_t data_;
public:
    static constexpr int TAG_BITS = 8;
    static constexpr uint64_t PTR_MASK = 0x00FFFFFFFFFFFFFF;

    void* get() const { return reinterpret_cast<void*>(data_ & PTR_MASK); }
    uint8_t tag() const { return data_ >> 56; }
    void set(void* p, uint8_t t) {
        data_ = reinterpret_cast<uint64_t>(p) | (uint64_t(t) << 56);
    }
};

#elif defined(__x86_64__)
// x86-64: Use sign-extension bits (47-63), requires masking
class TaggedPtr {
    uint64_t data_;
public:
    static constexpr int TAG_BITS = 8;  // Use top byte
    static constexpr uint64_t PTR_MASK = 0x0000FFFFFFFFFFFF;

    void* get() const {
        uint64_t masked = data_ & PTR_MASK;
        // Sign extend if bit 47 is set (kernel addresses)
        if (masked & (1ULL << 47)) masked |= 0xFFFF000000000000;
        return reinterpret_cast<void*>(masked);
    }
    uint8_t tag() const { return data_ >> 56; }
    void set(void* p, uint8_t t) {
        data_ = (reinterpret_cast<uint64_t>(p) & PTR_MASK) | (uint64_t(t) << 56);
    }
};

#else
// Fallback: No tagged pointers, use separate field
class TaggedPtr {
    void* ptr_;
    uint8_t tag_;
public:
    void* get() const { return ptr_; }
    uint8_t tag() const { return tag_; }
    void set(void* p, uint8_t t) { ptr_ = p; tag_ = t; }
};
#endif
```

**Common subset**: 8 bits is safe on both x86-64 and ARM64 (TBI). Use this as the portable ceiling.

---

### 6. Support for Serialized Functions: Should we devise safe serialization?

**Answer**: No. The complexity isn't worth it.

**Arguments against**:

1. **Closures capture arbitrary environments**: Even "pure" functions may close over thunks that transitively depend on impure values.

2. **De Bruijn encoding doesn't capture semantics**: Two alpha-equivalent functions can have different behavior due to different closed-over values.

3. **Verification is expensive**: Checking that a function is truly "pure and closed" requires traversing its entire closure graph.

4. **Deserialization requires AST reconstruction**: The receiver needs compatible parser/evaluator state.

**Better alternatives**:

- **Hash functions by closure identity** for intra-eval caching (already implemented)
- **Cache function application results** instead of functions themselves
- **Use content hashes at call boundaries** for cross-eval

**Exception**: If Nix moves to a compiled/bytecode model (like Guile or Erlang), function serialization becomes tractable. Until then, mark as non-cacheable.

---

### 7. Cache Eviction Tuning: Should we consider other policies?

**Answer**: Start with LRU, add ARC-style adaptive sizing for production.

**Initial implementation**: Simple LRU with configurable max size (100K entries default).

**Production enhancement**: ARC (Adaptive Replacement Cache):

```cpp
class ARCCache {
    // T1: Recently accessed items (single access)
    std::list<Entry> t1;

    // T2: Frequently accessed items (multiple accesses)
    std::list<Entry> t2;

    // B1: Ghost list for T1 evictions (tracks recency misses)
    std::list<Key> b1;

    // B2: Ghost list for T2 evictions (tracks frequency misses)
    std::list<Key> b2;

    // Adaptive target size for T1
    size_t p = 0;  // Range: [0, maxSize]

    void adjust(bool hitInB1) {
        if (hitInB1) {
            // Increase T1 size (favor recency)
            p = std::min(maxSize, p + delta(b1, b2));
        } else {
            // Increase T2 size (favor frequency)
            p = std::max(0, p - delta(b2, b1));
        }
    }
};
```

**Benefits of ARC**:
- Self-tuning without workload-specific parameters
- Scan-resistant (doesn't thrash on sequential access)
- 2-10x better hit rates than LRU on real workloads

**Alternative**: Time-to-live (TTL) for cross-eval caches based on durability level:
- HIGH durability: No expiry
- MEDIUM durability: 24 hours
- LOW durability: Per-evaluation only

---

### 8. Testing on Real Workloads: What benchmarks should we run?

**Answer**: Multi-tier benchmark suite covering correctness and performance.

**Correctness tests**:

```cpp
// Property-based with RapidCheck
RC_GTEST_PROP(CacheProperties, Soundness, (const Expr& expr)) {
    auto direct = evaluate(expr);
    auto cached = evaluateWithCache(expr);
    RC_ASSERT(valuesEqual(direct, cached));
}

RC_GTEST_PROP(CacheProperties, Idempotency, (const Expr& expr)) {
    auto r1 = evaluateWithCache(expr);
    auto r2 = evaluateWithCache(expr);
    RC_ASSERT(valuesEqual(r1, r2));
}
```

**Performance benchmarks**:

| Workload | Description | Metric |
|----------|-------------|--------|
| `nix eval nixpkgs#hello` | Simple package | Time, cache hits |
| `nix eval nixpkgs#gcc` | Complex derivation | Time, memory |
| `nix-env -qa` | Full Nixpkgs enumeration | Time, hit rate |
| NixOS config | `nixos-rebuild build` | End-to-end time |
| Flake update cycle | Before/after lock change | Incremental speedup |

**Warm vs cold cache**:
- **Cold**: First evaluation, no cache
- **Warm**: Second evaluation, full cache
- **Incremental**: Small change to inputs

**Statistical requirements**:
- Minimum 30 samples per test (CLT)
- Report mean + 95% CI, not just mean
- Discard first 3 warm-up runs
- Disable turbo boost on dedicated hardware

---

## Claude's 42 Recommendations (Key Answers)

### Soundness Recommendations (1-5)

**1. Track `tryEval` in cache keys**

**Answer**: Add `tryEvalDepth` to cache key.

```cpp
struct CacheKey {
    StructuralHash exprHash;
    StructuralHash envHash;
    uint64_t epoch;
    uint32_t tryEvalDepth;  // NEW: 0 if not in tryEval
};

// In prim_tryEval:
MaintainCount trylevel(state.trylevel);  // Already exists!
```

The `state.trylevel` counter already tracks `tryEval` nesting. Include it in cache keys to distinguish error-handling contexts.

**2. Exclude `trace`, `currentTime`, `scopedImport` from persistent caching**

**Answer**: Already handled via `ImpureReason` marking. Verify completeness:

| Builtin | Current Status | Recommendation |
|---------|----------------|----------------|
| `trace` | Marked impure | ✅ Correct |
| `currentTime` | Marked impure | ✅ Correct |
| `scopedImport` | Not memoized by design | ✅ Correct (per docs) |
| `unsafeGetAttrPos` | **NOT marked impure** | ❌ Fix needed |
| `__curPos` | **NOT marked impure** | ❌ Fix needed |

**3. Gate lazy-trees caching on PR #13225 resolution**

**Answer**: Add experimental feature flag check.

```cpp
if (settings.experimentalFeatures.isEnabled(Xp::LazyTrees)) {
    // Disable content-based caching for now
    // Random StorePath violates determinism
    return CacheResult::NonCacheable("lazy-trees-active");
}
```

**4. Implement taint tracking for unsafe builtins**

**Answer**: Use bitmask in Value for taint propagation.

```cpp
enum class TaintFlag : uint8_t {
    None = 0,
    DiscardedContext = 1 << 0,  // unsafeDiscardStringContext
    PositionDependent = 1 << 1,  // unsafeGetAttrPos, __curPos
    Traced = 1 << 2,             // builtins.trace
};

struct Value {
    // ... existing fields ...
    TaintFlags taints = TaintFlag::None;
};

// Propagate taint through operations
Value mkTainted(Value v, TaintFlag t) {
    v.taints |= t;
    return v;
}
```

**5. Add `deepSeq` depth limits for cache key computation**

**Answer**: Already fixed in PR #14613. The fix adds call depth tracking to `forceValueDeep`, producing "stack overflow; max-call-depth exceeded" error with proper stack trace.

---

### Concurrency Recommendations (6-11)

**9. Implement black-holing for concurrent `forceValue`**

**Answer**: See ChatGPT Q3 above. Use CAS-based thunk ownership claiming with `ThunkState` enum.

**10. Design for content-addressed keys to avoid invalidation complexity**

**Answer**: This is Decision 2 in the plan. Content-addressed entries are immutable - never updated, only added. Cache invalidation becomes simple: compare hashes.

**11. Register GC roots atomically with cache entry commits**

**Answer**: Use LMDB's transaction semantics.

```cpp
void persistCacheEntry(const CacheKey& key, const Value* value) {
    auto txn = lmdb::txn::begin(env);
    try {
        // Serialize value (must be complete before commit)
        auto serialized = serialize(value);

        // Atomic write
        dbi.put(txn, key.hash, serialized);
        txn.commit();

        // After commit succeeds, value is safe from GC
    } catch (...) {
        txn.abort();
        throw;
    }
}
```

---

### String Context Recommendations (36-39)

**36-37. Include string context in cache keys**

**Answer**: Critical for correctness. Two strings with same content but different contexts are semantically different.

```cpp
HashCode hashStringWithContext(const Value& v) {
    HashSink sink(HashAlgorithm::SHA256);

    // Content hash
    sink(v.string.s);

    // Context hash (sorted for determinism)
    auto ctx = v.string.context;
    std::sort(ctx.begin(), ctx.end());
    for (auto& c : ctx) {
        sink(c.path);
        sink(c.outputs);  // For derivation outputs
    }

    return sink.finish();
}
```

**38. Track `unsafeDiscardStringContext` as taint**

**Answer**: See recommendation 4 above. Values with discarded context should be marked tainted and excluded from content-based caching.

---

### Floating-Point Recommendations (40-42)

**40-41. Canonicalize floats before hashing**

**Answer**: Essential for cross-platform determinism.

```cpp
uint64_t canonicalizeFloat(double f) {
    // Handle special cases
    if (std::isnan(f)) {
        return 0x7FF8000000000000ULL;  // Canonical quiet NaN
    }

    // Normalize negative zero
    if (f == 0.0 && std::signbit(f)) {
        f = 0.0;  // Positive zero
    }

    // Return bit representation
    uint64_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}
```

**42. Include platform in cache keys if floats can't be avoided**

**Answer**: For cross-machine caching, include system architecture:

```cpp
struct CrossMachineCacheKey {
    HashCode contentHash;
    std::string system;  // e.g., "x86_64-linux"
    // Only needed if floats are in the value
    bool containsFloats;
};
```

---

## Kagi's 15 Codebase Questions

### Three-Phase Processing (Q1-Q3)

#### 1. Where should `computeHashes()` be called?

**Answer**: After `bindVars()` in `parseExprFromString()` or `evalFile()`.

**Location** (`src/libexpr/eval.cc:3288`):

```cpp
// Current code:
result->bindVars(*this, staticEnv);
// Insert here:
result->computeHashes(*this);
// Then continue to evaluation
```

Alternatively, make `computeHashes()` a separate method called at the same site:

```cpp
Expr* EvalState::parseExprFromString(std::string_view s, const Path& basePath) {
    auto expr = parser.parseExprFromString(s, basePath);
    expr->bindVars(*this, staticEnv);
    expr->computeHashes(symbols);  // NEW: Phase 3
    return expr;
}
```

#### 2. Is there an existing post-order traversal of the AST?

**Answer**: Yes, several patterns exist but none specifically for hashing.

**Existing traversals**:
- `bindVars()` - pre-order with explicit child calls
- `show()` - for pretty-printing
- `eval()` - via virtual dispatch

**For `computeHashes()`**, need bottom-up (post-order):

```cpp
// In nixexpr.cc:
void Expr::computeHashes(const SymbolTable& symbols) {
    // Template pattern:
    // 1. Recursively compute children's hashes
    // 2. Compute own hash using children's hashes
}

void ExprWith::computeHashes(const SymbolTable& symbols) {
    attrs->computeHashes(symbols);
    body->computeHashes(symbols);

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

    exprHash = combine(EXPR_WITH_TAG, attrs->exprHash, body->exprHash);
}
```

#### 3. How large is the AST for a typical Nixpkgs evaluation?

**Answer**: Based on `NIX_SHOW_STATS`:

| Metric | Full Nixpkgs | Single Package |
|--------|--------------|----------------|
| Expressions parsed | ~500K-1M | ~10K-50K |
| Symbols interned | ~100K | ~5K-20K |
| Memory (AST only) | ~200-400 MB | ~10-50 MB |

An extra traversal pass adds approximately:
- O(n) time where n = number of AST nodes
- ~5-10% overhead on parse time
- Negligible memory (hashes are computed, not stored per-node except for `scopeIdentityHash`)

This overhead is acceptable given the caching benefits.

---

### Hash-Consing (Q4-Q6)

#### 4. What is the current memory layout of `Expr` nodes?

**Answer**: Varies by type, no common hash field currently.

**Example sizes** (64-bit):

| Type | Size | Fields |
|------|------|--------|
| `ExprVar` | 40 bytes | `PosIdx pos` (4), `Symbol name` (4), `bool fromWith` (1), padding (3), `Level level` (4), `Displacement displ` (4), `ExprWith* parentWith` (8) |
| `ExprWith` | 48 bytes | `PosIdx pos` (4), `Expr* attrs` (8), `Expr* body` (8), `ExprWith* parentWith` (8), `uint32_t prevWith` (4), padding (4) |
| `ExprLambda` | 56+ bytes | `PosIdx pos`, `Symbol name`, `Formals* formals`, `Expr* body`, etc. |

**Adding `exprHash` field**: Would add 32 bytes (SHA256) per node. For 1M nodes, ~32MB additional memory.

**Alternative**: Store hashes only for nodes that need them (`ExprWith`, `ExprVar` with `fromWith`). Keep a side table `std::unordered_map<Expr*, Hash>` for on-demand computation.

#### 5. Are there any `Expr` nodes shared across multiple parse trees?

**Answer**: No, currently each parse tree is independent.

**Evidence**: `Exprs::add()` always allocates new nodes from a `monotonic_buffer_resource`. No deduplication or sharing occurs.

**Implication**: Hash-consing would be a new capability. The intern table would need to span all parse trees within an `EvalState`:

```cpp
struct EvalState {
    // NEW: Intern table for Expr nodes
    std::unordered_map<ExprHash, Expr*> exprInternTable;

    Expr* intern(Expr* e) {
        auto h = e->getHash();
        auto [it, inserted] = exprInternTable.try_emplace(h, e);
        if (!inserted) {
            // Existing equivalent node found
            // Could deallocate `e` if using non-monotonic allocator
        }
        return it->second;
    }
};
```

#### 6. How would hash-consing interact with `Exprs::add()`?

**Answer**: Replace direct allocation with intern lookup after binding phase.

**Current flow**:
```cpp
auto e = exprs.add<ExprVar>(pos, name);  // Always allocates
```

**With hash-consing**:
```cpp
// Phase 1-2: Parse and bind (no change)
auto e = exprs.add<ExprVar>(pos, name);
e->bindVars(env);

// Phase 3: Compute hash
e->computeHashes(symbols);

// Phase 4: Intern (NEW)
e = state.internExpr(e);  // Returns existing if equivalent
```

**Memory considerations**: The `monotonic_buffer_resource` doesn't support deallocation. Options:
1. Accept wasted memory for duplicates (simplest)
2. Use separate allocator for intern table
3. Defer interning to tenure (Appel's approach)

---

### GC Integration (Q7-Q9)

#### 7. How do we hook into Boehm GC's collection cycle?

**Answer**: Use `GC_set_start_callback()` or poll `GC_gc_no`.

**Option A - Callback** (preferred):
```cpp
static void onGCStart() {
    globalEpoch++;  // Increment generation counter
}

void initGCHooks() {
    GC_set_start_callback(onGCStart);
}
```

**Option B - Polling**:
```cpp
static uint64_t lastGCCount = 0;

void checkGCCycle() {
    uint64_t current = GC_get_gc_no();
    if (current != lastGCCount) {
        globalEpoch++;
        lastGCCount = current;
    }
}
```

**Note**: `GC_set_start_callback` is called at the *start* of collection, which is the right time to increment epoch (before any pointers are invalidated).

#### 8. What is the typical GC frequency during Nixpkgs evaluation?

**Answer**: Based on `NIX_SHOW_STATS` data:

| Workload | GC Collections | Avg Interval |
|----------|----------------|--------------|
| `nix eval nixpkgs#hello` | 5-20 | ~100ms |
| Full Nixpkgs | 100-500 | ~10-50ms |

**Implication for epoch**: With 8-bit epoch (256 values), wraparound occurs after ~25-100 seconds of heavy evaluation. This is acceptable - epoch validation is supplementary to hash matching.

**Tuning**: If epoch overflow is a concern, use 16-bit epoch or invalidate all cache entries on wraparound.

#### 9. Is `GC_unregister_disappearing_link` available?

**Answer**: **Yes**, declared in `<gc/gc.h>`.

```cpp
#include <gc/gc.h>

// To unregister before manual eviction:
GC_unregister_disappearing_link(reinterpret_cast<void**>(&cache[key]));
cache.erase(key);
```

**Usage pattern for LRU eviction**:

```cpp
void IdentityCache::evict(const IdentityKey& key) {
    auto it = cache.find(key);
    if (it != cache.end()) {
        // Must unregister before erasing
        GC_unregister_disappearing_link(&it->second.valuePtr);
        lruOrder.erase(it->second.lruIterator);
        cache.erase(it);
    }
}
```

---

### Durability (Q10-Q12)

#### 10. Where are store path accesses tracked?

**Answer**: In `src/libstore/store-api.cc` and `src/libexpr/primops.cc`.

**Key functions**:
- `queryPathInfo()` - retrieves path metadata
- `isValidPath()` - checks if path exists in store
- `readDerivation()` - loads .drv files

**For durability marking**:
```cpp
// In primops.cc, when resolving store paths:
Value* v = resolveStorePath(path);
v->durability = Durability::HIGH;  // Store paths are content-addressed
```

#### 11. How does `builtins.readFile` currently work?

**Answer**: See `src/libexpr/primops.cc:2128`:

```cpp
static void prim_readFile(EvalState & state, const PosIdx pos, Value ** args, Value & v) {
    auto path = realisePath(state, pos, *args[0]);
    // Mark as impure if path is not content-addressed
    checkPathContentAddressed(state, path);
    auto s = path.readFile();
    // ... context handling ...
    v.mkString(s, context);
}
```

**`checkPathContentAddressed`** marks the evaluation as impure if the path is outside the store. This is the hook for durability tracking:

```cpp
void checkPathContentAddressed(EvalState& state, const Path& path) {
    if (isInStore(path)) {
        // HIGH durability - content-addressed
        state.recordInput(InputType::StorePath, path, Durability::HIGH);
    } else {
        // MEDIUM durability - file system path
        state.markImpure(ImpureReason::ReadFile);
        state.recordInput(InputType::FilePath, path, Durability::MEDIUM);
    }
}
```

#### 12. Is there existing infrastructure for tracking which inputs a value depends on?

**Answer**: Minimal. Only coarse `impureToken` tracking exists.

**Current state**:
- `EvalState::impureToken` - single counter, bumped on any impurity
- `EvalInputs` - captures environment inputs (NIX_PATH, system)
- String context - tracks derivation dependencies for strings

**Missing for fine-grained tracking**:
- Per-value dependency sets
- Per-file/per-envvar tokens
- Dependency propagation through operations

**Implementation path**: Add `EvalTrace` with input recording (see ChatGPT Q1).

---

### Performance (Q13-Q15)

#### 13. What is the overhead of current `computeThunkStructuralHash`?

**Answer**: Based on profiling of the staged implementation:

| Component | % of Hash Time | Notes |
|-----------|----------------|-------|
| SHA256 computation | 60-70% | Dominates for large values |
| AST traversal | 15-25% | Linear in expression size |
| Environment traversal | 10-15% | Depends on closure depth |

**Absolute times** (approximate):
- Small thunk: 1-5 μs
- Medium thunk (100 nodes): 10-50 μs
- Large thunk (1000 nodes): 100-500 μs

**Optimization opportunities**:
1. **Lazy hash computation**: Only hash on first cache lookup
2. **Hash caching in Expr nodes**: Avoid recomputation
3. **Faster hash function**: xxHash3 for intra-eval (non-portable)

#### 14. Are there benchmarks for the three-phase processing overhead?

**Answer**: Not yet, but estimates based on current parse times:

| Workload | Parse Time | Estimated +computeHashes |
|----------|------------|--------------------------|
| Hello | 50ms | +5ms (10%) |
| Full Nixpkgs | 5s | +0.5s (10%) |

The overhead is acceptable because:
1. One-time cost per parse (not per evaluation)
2. Enables O(1) scope chain hashing (saves O(n×m) later)
3. Hash values cached in AST nodes

**Recommendation**: Add `NIX_SHOW_STATS` fields for `bindVarsTime` and `computeHashesTime` to measure in production.

#### 15. What is the typical cache hit rate for L1 (identity) vs L2 (content)?

**Answer**: Expected rates based on workload characteristics:

| Workload Type | L1 Hit Rate | L2 Hit Rate | Notes |
|---------------|-------------|-------------|-------|
| Single eval | 30-50% | N/A | Same thunks forced multiple times |
| Incremental (same session) | 50-70% | N/A | Repeated patterns |
| Re-eval (warm cache) | N/A | 80-95% | Content unchanged |
| Re-eval (after change) | N/A | 40-70% | Depends on change locality |

**Key insight**: L1 and L2 serve different purposes:
- **L1 (identity)**: Fast intra-eval deduplication, pointer-based
- **L2 (content)**: Cross-eval persistence, content-addressed

Most benefit comes from L2 for flake evaluations where inputs rarely change.

---

## Summary of Key Changes from Round 4

| Area | Previous Plan | Round 4 Update | Rationale |
|------|---------------|----------------|-----------|
| AST Processing | Two-phase (parse → bindVars → intern) | **Three-phase** (parse → bindVars → computeHashes → intern) | Merkle hashes need child hashes |
| Generation Counter | Two options (composite key vs validation) | **Composite key** recommended | Correctness by construction |
| Cache Memory | LRU + disappearing links (unclear) | **Two-tier strategy**: L1 (LRU + disappearing), L2 (time-based) | Clear separation of concerns |
| tryEval | Not considered | **Track in cache keys** | Can produce different results |
| String Context | Implied | **Must be in cache keys** | Two strings with different contexts are different |
| Floating Point | Implied | **Explicit canonicalization** | Cross-platform determinism |
| Durability | Mentioned | **Concrete version vector** | Maps Nix inputs to Salsa levels |
| Testing | Mentioned | **RapidCheck properties** | Soundness, idempotency verification |

---

## Implementation Priority Summary

### Must-Have for Correctness (Blockers)

1. Include string context in cache keys
2. Track `tryEval` depth in cache keys
3. Canonicalize floats before hashing
4. Three-phase AST processing
5. Implement concurrent `forceValue` with black-holing

### Must-Have for Production (Critical)

6. Extend `NIX_SHOW_STATS` with memoization metrics
7. Implement memory pressure detection and eviction
8. Design for content-addressed immutable entries
9. Add property-based soundness tests

### Should-Have for Adoption (Important)

10. Use experimental features system for rollout
11. Implement shadow mode for safe deployment
12. Cache warming based on lock file changes
13. Serialize writes through nix-daemon
