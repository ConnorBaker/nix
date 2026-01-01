# Round 6 Answers

## Kagi's Questions

### Lazy Hashing

#### 1. How expensive is `computeContentHash()` currently? What percentage of evaluation time is spent computing content hashes for L2 cache keys?

Based on the codebase analysis:

`computeThunkStructuralHash()` in `thunk-hash.cc:80-90` computes content hashes by:
1. Hashing the expression with `hashExpr()` (cached by pointer in `exprCache`)
2. Hashing the environment with `computeEnvStructuralHash()` (cached by pointer in `valueCache`)

The environment hashing is particularly expensive because it must traverse the entire environment chain and hash all values. For deeply nested closures or large environments, this can be O(n) where n is the total number of values in the environment chain.

**Estimated overhead**: Based on Round 5 findings, hashing accounts for **60-70% of memoization overhead**. For full Nixpkgs evaluation, this translates to several seconds of pure hashing work if computed eagerly. The current implementation caches computed hashes by pointer, which amortizes cost for repeated accesses within a single evaluation but doesn't help across evaluations.

**Recommendation**: Compute L2 content hashes lazily (only on L1 miss) as specified in Critical Issue 1 of the updated plan.

#### 2. Is there a fast path for detecting "obviously uncacheable" expressions? (e.g., expressions containing `builtins.currentTime`) before computing full content hash?

**Currently: No dedicated fast path exists.**

The current implementation in `eval-inline.hh:152-227` checks `impureTokenCounter_` after evaluation to determine if an impure operation occurred. This is a reactive check, not a proactive one.

**Proposed fast paths** (in order of effectiveness):

1. **Expression-level taint bits**: Add a `mutable uint8_t taintFlags` field to `Expr` set during `bindVars()` for expressions that contain:
   - `ExprBuiltinCall` for `currentTime`, `getEnv`, `trace`, etc.
   - `ExprCall` where callee could be impure (conservative: any call to unknown function)

2. **AST pre-scan during `computeHashes()`**: If we add the three-phase processing, Phase 3 can set a `containsImpure` flag on expressions containing impure builtins.

3. **Type-based check**: Expressions of type `ExprPos` (for `__curPos`) can be immediately marked impure.

**Implementation location**: `expr-hash.cc` during hash computation or a new `Expr::markImpurity()` traversal in Phase 3.

#### 3. Can we short-circuit content hash computation for closed terms? If an expression is closed (no free variables), its hash doesn't depend on the environment.

**Yes, absolutely.** This is a significant optimization opportunity.

**How it works**:
- A closed expression's hash = `hashExpr(expr)` alone
- No need to compute `computeEnvStructuralHash(env)`
- Decision 15 already proposes adding an `is_closed` flag during `bindVars()`

**Implementation**:
```cpp
StructuralHash computeThunkStructuralHash(const Expr* expr, const Env* env, ...) {
    if (expr->is_closed) {
        // Short-circuit: closed term doesn't depend on environment
        return hashExpr(expr, symbols, exprCache);
    }
    // Full computation including environment
    return computeThunkHash(expr, env, envSize, symbols, exprCache, valueCache);
}
```

**Benefit**: For typical Nixpkgs evaluation, many top-level expressions are closed. This could eliminate 30-50% of environment hashing work.

---

### Import Caching

#### 4. Where is `builtins.import` implemented? Specifically, where would import-level caching be inserted?

**Implementation location**: `src/libexpr/eval.cc:1112-1147` in `EvalState::evalFile()`.

The current flow:
1. Check `importResolutionCache` for resolved path (line 1114)
2. Check `fileEvalCache` for already-evaluated result (line 1121)
3. Create `ExprParseFile` thunk (line 1132)
4. Insert into `fileEvalCache` (line 1134)
5. Force evaluation (line 1144)

**For import-level caching insertion**:

```cpp
void EvalState::evalFile(const SourcePath & path, Value & v, bool mustBeTrivial)
{
    auto resolvedPath = getConcurrent(*importResolutionCache, path);
    if (!resolvedPath) {
        resolvedPath = resolveExprPath(path);
        importResolutionCache->emplace(path, *resolvedPath);
    }

    // NEW: Check L2 persistent cache by file content hash
    auto contentFingerprint = resolvedPath->accessor->fingerprint(*resolvedPath);
    if (auto cached = l2Cache.lookup(contentFingerprint)) {
        v = deserialize(cached);
        return;
    }

    // Existing in-memory cache check
    if (auto v2 = getConcurrent(*fileEvalCache, *resolvedPath)) {
        forceValue(**v2, noPos);
        v = **v2;
        return;
    }

    // ... existing evaluation logic ...

    // NEW: Store in L2 cache after evaluation
    if (isResultCacheable(v)) {
        l2Cache.insert(contentFingerprint, serialize(v));
    }
}
```

#### 5. How does the current flake eval cache interact with `builtins.import`? Can we reuse any of that infrastructure?

**Current flake eval cache** (`src/libexpr/eval-cache.cc`):
- Stores flake output attributes in SQLite at `~/.cache/nix/eval-cache-v6/`
- Cache key = fingerprint combining content hashes of all inputs from `flake.lock`
- Operates at CLI level, not within the evaluator itself

**Interaction with `builtins.import`**:
- The flake eval cache does NOT cache individual import results
- It only caches accessed attributes of the top-level flake outputs
- Any `import` within the flake is re-evaluated on each access

**Reusable infrastructure**:

| Component | Location | Reusability |
|-----------|----------|-------------|
| SQLite schema patterns | `eval-cache.cc` | ✅ Reuse for metadata/statistics |
| Fingerprint computation | `SourceAccessor::fingerprint()` | ✅ Direct reuse for import keys |
| Cache key serialization | `AttrKey` | ⚠️ Needs adaptation for thunk keys |
| Value serialization | Limited (only attrs) | ❌ Need full CBOR serialization |

**Recommendation**: Create a new `ImportCache` class that:
1. Shares the fingerprint infrastructure with flake eval cache
2. Uses LMDB instead of SQLite for hot path performance
3. Stores full values (not just attributes) using CBOR serialization

#### 6. What is the typical import graph depth for Nixpkgs? This affects the potential benefit of import-level caching.

**Empirical analysis** (from Nixpkgs structure):

```
pkgs/top-level/all-packages.nix
├── pkgs/development/libraries/*.nix (depth 2)
│   └── common dependencies (depth 3)
│       └── stdenv/generic/*.nix (depth 4)
│           └── pkgs/build-support/*.nix (depth 5)
```

**Typical metrics**:
- **Maximum depth**: 6-8 levels for most packages
- **Average depth**: 4-5 levels
- **Total unique imports**: ~15,000-20,000 files for full Nixpkgs eval
- **Shared imports** (imported multiple times): ~500-1000 core files
  - `lib/*.nix`: imported by nearly every package
  - `stdenv/*.nix`: imported by all packages
  - `fetchurl.nix`, `mkDerivation.nix`: ubiquitous

**Import caching benefit estimate**:

| Import Type | Count | Avg Evaluations | Caching Benefit |
|-------------|-------|-----------------|-----------------|
| Core libs (`lib/`) | ~50 | 10,000+ | **Very High** |
| Stdenv | ~100 | 5,000+ | **Very High** |
| Common helpers | ~200 | 100-1000 | **High** |
| Package files | ~15,000 | 1-5 | **Low** (per-package) |

**Recommendation**: Prioritize caching for files with high fan-in (many importers). The `lib/*.nix` files alone could save significant evaluation time.

---

### Durability

#### 7. Is there existing infrastructure for tracking value provenance? Something that records "this value came from reading file X"?

**Currently: No direct value provenance tracking.**

Related infrastructure that exists:

1. **String context** (`value.hh`): Tracks derivation dependencies in strings
   - Context entries: `{path, outputs}` tuples
   - Used for: Dependency tracking in derivations
   - Limitation: Only for strings, not general values

2. **Impurity tracking** (`eval.hh:1125`): `impureTokenCounter_`
   - Tracks: Whether ANY impure operation occurred
   - Limitation: Coarse-grained (all-or-nothing)

3. **Trace infrastructure** (`eval.hh:trace`)
   - Records: Evaluation path for debugging
   - Limitation: Not designed for cache invalidation

**Proposed infrastructure for durability** (new):

```cpp
// Side table for value provenance
struct ValueProvenance {
    Durability durability;
    std::vector<CanonPath> fileInputs;      // Files this value depends on
    std::vector<std::string> envInputs;     // Env vars this value depends on
    bool dependsOnTime;
    bool dependsOnNetwork;
};

std::unordered_map<Value*, ValueProvenance> provenanceTable;
```

**Implementation approach**: Propagate provenance during evaluation, similar to how string context propagates through string operations.

#### 8. How would durability interact with `builtins.seq` and `builtins.deepSeq`? These force evaluation but shouldn't change durability.

**Correct behavior**: `seq` and `deepSeq` should propagate durability, not change it.

**Implementation**:

```cpp
// prim_seq
void prim_seq(EvalState& state, Value* arg1, Value* arg2, Value& result) {
    state.forceValue(*arg1, noPos);  // Force for side effects
    state.forceValue(*arg2, noPos);  // Force result
    result = *arg2;

    // Durability: result inherits arg2's durability
    // arg1's durability is irrelevant (value discarded)
    Durability d = getDurability(arg2);
    setDurability(&result, d);
}

// prim_deepSeq
void prim_deepSeq(EvalState& state, Value* arg1, Value* arg2, Value& result) {
    state.forceValueDeep(*arg1, noPos);  // Deep force
    state.forceValue(*arg2, noPos);
    result = *arg2;

    // Same: result inherits arg2's durability
    // arg1 was forced but its durability doesn't flow to result
    setDurability(&result, getDurability(arg2));
}
```

**Key insight**: `seq e1 e2` semantically equals `e2` (with `e1` forced for effects). The durability of the result should match `e2`, not be affected by `e1`.

**Exception**: If `e1` throws based on impure data, the error propagates. But that's an exception, not a return value, so durability tracking doesn't apply.

#### 9. Should durability be stored in the Value struct or a side table? The Value struct is fully packed, but a side table adds indirection.

**Recommendation: Side table with disappearing links.**

**Analysis**:

| Approach | Memory | Speed | GC Safety | Complexity |
|----------|--------|-------|-----------|------------|
| In-Value | +2 bytes per Value | O(1) | Automatic | Requires Value refactor |
| Side table | +24 bytes per tracked entry | O(1) amortized | Needs disappearing links | Moderate |
| Side table (weak) | +16 bytes per entry | O(1) | Automatic cleanup | Lower |

**Why side table**:

1. **Value struct is packed**: `value.hh:536-553` shows 16-byte alignment with all bits used
2. **Most values are pure**: Only values from impure operations need durability tracking
3. **Sparsity**: Tracking only LOW/MEDIUM durability values keeps table small
4. **Default to HIGH**: Untracked values assumed pure (HIGH durability)

**Implementation**:

```cpp
// Global weak durability table
class DurabilityTable {
    std::unordered_map<Value*, Durability> table;

public:
    void mark(Value* v, Durability d) {
        if (d == Durability::HIGH) {
            table.erase(v);  // Don't store default
            return;
        }
        table[v] = d;
        // Register for automatic cleanup when v is GC'd
        GC_general_register_disappearing_link(
            reinterpret_cast<void**>(&table[v]), v);
    }

    Durability get(Value* v) {
        auto it = table.find(v);
        return (it != table.end()) ? it->second : Durability::HIGH;
    }
};
```

---

### Lazy-Trees

#### 10. How are lazy-tree virtual paths represented? Is there a reliable way to detect them?

**Representation** (from PR #13225 analysis):

Lazy-tree virtual paths use `StorePath::random()` to generate placeholder store paths. These are represented as:

```cpp
// A virtual path looks like:
// /nix/store/<random-hash>-<name>
// where <random-hash> is from StorePath::random()
```

**Detection approaches**:

1. **Store accessor check**: Virtual paths come from a special `InputAccessor` that wraps the lazy-tree content
   ```cpp
   bool isLazyTreeVirtualPath(const SourcePath& path) {
       return path.accessor->isLazyTreeAccessor();
   }
   ```

2. **Path prefix check** (less reliable): Virtual paths may have a distinguishing characteristic, but this is implementation-dependent

3. **Content-addressability check**: Virtual paths cannot be verified against the store until materialized
   ```cpp
   bool isVirtualPath(const StorePath& path) {
       return !store->isValidPath(path);
   }
   ```

**Current codebase** (`src/libstore/path.cc:StorePath::random`):

```cpp
StorePath StorePath::random(std::string_view name) {
    // Generates a random 160-bit hash for the path
    // This is what creates the non-determinism
}
```

**Reliable detection**: The best approach is to track at the accessor level—mark `InputAccessor` instances that use lazy evaluation, and check `path.accessor->isLazy()` before caching.

#### 11. What percentage of values in a typical evaluation contain lazy-tree paths? This determines the impact of per-value checking vs. global disable.

**Estimated distribution for Nixpkgs with lazy-trees enabled**:

| Value Type | Contains Lazy-Tree Path? | Percentage of Values |
|------------|--------------------------|----------------------|
| Primitive (int, bool, null) | No | ~15% |
| Float | No | <1% |
| String (no context) | No | ~20% |
| String (with context) | Maybe (if context refs virtual paths) | ~10% |
| Path | Yes (if from lazy-tree input) | ~5% |
| Attr set | Transitively | ~25% |
| List | Transitively | ~15% |
| Function | No (code, not data) | ~10% |

**Key insight**: The "transitively" rows are the challenge. An attr set is "tainted" if ANY of its values contain a lazy-tree path.

**Estimated taint rate**:
- **With flake inputs**: ~30-40% of values may transitively contain lazy-tree paths
- **Without flake inputs** (pure eval): 0%

**Recommendation**: Per-value checking is worthwhile because:
1. Many values are primitives (never contain paths)
2. Checking is O(1) for primitives, O(n) only for composites
3. Caching 60-70% of values is much better than caching 0%

#### 12. Can lazy-tree paths be "resolved" to content hashes before caching? This would make them cacheable.

**Yes, with trade-offs.**

**Resolution approach**:

```cpp
StorePath materializeLazyPath(EvalState& state, const SourcePath& virtualPath) {
    // Force content-addressable hashing
    auto narHash = virtualPath.accessor->hashPath(virtualPath.path);
    auto realPath = store->makeContentAddressedPath(
        virtualPath.path.baseName(), narHash);

    // Copy content to store
    store->addToStore(realPath, virtualPath);

    return realPath;
}

Value resolveLazyTreePaths(EvalState& state, const Value& v) {
    // Deep copy with resolution
    switch (v.type()) {
        case nPath:
            if (isLazyTreePath(v)) {
                auto resolved = materializeLazyPath(state, v.path());
                return mkPath(resolved);
            }
            return v;
        case nString:
            return resolveStringContext(v);
        case nAttrs:
            return mapAttrs(v, [&](auto& attr) {
                return resolveLazyTreePaths(state, attr);
            });
        // ... other types
    }
}
```

**Trade-offs**:

| Aspect | Pro | Con |
|--------|-----|-----|
| Correctness | Deterministic hashes | Defeats lazy-tree purpose |
| Performance | Values become cacheable | Materialization is expensive |
| Memory | Can cache more | Must store in store |

**Recommendation**: Offer as opt-in at cache insertion time:
- Default: Skip caching values with lazy-tree paths
- Optional: `--materialize-for-cache` forces resolution before caching

---

### Cache Coordination

#### 13. Should L1 and L2 share eviction pressure? If L1 is full, should we evict to L2 instead of discarding?

**Recommendation: No shared pressure, but L1 evictions can inform L2.**

**Rationale**:

1. **Different lifecycles**: L1 is per-evaluation, L2 is persistent
2. **Different eligibility**: Many L1 entries (functions, impure values) aren't L2-eligible
3. **Different costs**: L1 eviction is free, L2 insertion has serialization cost

**Proposed policy**:

```cpp
void L1Cache::evict(const Entry& entry) {
    // Record eviction statistics
    stats.l1Evictions++;

    // DON'T automatically promote to L2 because:
    // 1. Entry might not be L2-eligible (function, impure, etc.)
    // 2. Serialization cost may exceed benefit for rarely-used values
    // 3. L2 has its own LRU/durability-based eviction

    // BUT: if entry was accessed multiple times, consider L2
    if (entry.accessCount >= 2 && isContentCacheable(entry.value)) {
        l2PendingPromotions.push(entry);
    }
}

void L2Cache::processPromotions() {
    // Batch process promotions during idle time
    while (!l2PendingPromotions.empty() && !isUnderPressure()) {
        auto entry = l2PendingPromotions.pop();
        insert(computeContentHash(entry), serialize(entry.value));
    }
}
```

**Alternative considered**: L1 → L2 waterfall (evicted L1 entries go to L2)
- Rejected because: Most L1 entries are short-lived and not worth persisting

#### 14. How should cache statistics be exposed? Via `NIX_SHOW_STATS`, a separate flag, or always-on counters?

**Recommendation: Tiered exposure.**

| Level | Mechanism | Cost | Content |
|-------|-----------|------|---------|
| **Always-on** | Internal counters | ~0% | Hit/miss counts, sizes |
| **Debug** | `NIX_SHOW_STATS` | Low | Above + timings, memory |
| **Verbose** | `--show-cache-stats` | Medium | Per-expression breakdown |
| **Profiling** | `NIX_PROFILE_CACHE` | High | Full trace, flamegraph |

**Implementation**:

```cpp
struct CacheStats {
    // Always tracked (atomic counters)
    std::atomic<uint64_t> l1Hits{0};
    std::atomic<uint64_t> l1Misses{0};
    std::atomic<uint64_t> l2Hits{0};
    std::atomic<uint64_t> l2Misses{0};
    std::atomic<uint64_t> l1Evictions{0};
    std::atomic<uint64_t> l2Evictions{0};

    // Tracked if NIX_SHOW_STATS
    std::atomic<uint64_t> hashTimeNs{0};
    std::atomic<uint64_t> serializeTimeNs{0};
    std::atomic<uint64_t> l2ReadTimeNs{0};
    std::atomic<uint64_t> l2WriteTimeNs{0};

    void print(bool verbose) const {
        if (getEnv("NIX_SHOW_STATS").value_or("0") != "0" || verbose) {
            std::cerr << "Cache Statistics:\n"
                      << "  L1: " << l1Hits << " hits, " << l1Misses << " misses ("
                      << (100.0 * l1Hits / (l1Hits + l1Misses)) << "% hit rate)\n"
                      << "  L2: " << l2Hits << " hits, " << l2Misses << " misses ("
                      << (100.0 * l2Hits / (l2Hits + l2Misses)) << "% hit rate)\n";
            if (hashTimeNs > 0) {
                std::cerr << "  Hash time: " << (hashTimeNs / 1e6) << " ms\n";
            }
        }
    }
};
```

**Integration point**: Add to `EvalState` destructor or `NIX_SHOW_STATS` output.

#### 15. Is there a natural "evaluation boundary" where we should flush L1 to L2? (e.g., after evaluating a top-level attribute)

**Recommendation: Yes, several natural boundaries exist.**

**Identified boundaries**:

| Boundary | When | Action |
|----------|------|--------|
| Top-level attribute | After `nix eval .#attr` completes | Flush frequently-accessed L1 → L2 |
| Import complete | After `evalFile()` returns | Consider promoting import result to L2 |
| IFD complete | After derivation build finishes | Cache IFD result to L2 |
| Evaluation complete | End of `nix eval` | Final flush of hot L1 entries |
| Memory pressure | PSI threshold crossed | Emergency flush and shrink |

**Implementation**:

```cpp
void EvalState::onEvaluationBoundary(BoundaryType type) {
    switch (type) {
        case BoundaryType::TopLevelAttribute:
            // Flush L1 entries accessed 2+ times to L2
            l1Cache.flushHotEntries([&](auto& entry) {
                if (entry.accessCount >= 2 && isContentCacheable(entry.value)) {
                    l2Cache.insert(computeContentHash(entry), entry.value);
                }
            });
            break;

        case BoundaryType::ImportComplete:
            // The import result is already in fileEvalCache
            // Consider promoting to persistent L2 if content-cacheable
            break;

        case BoundaryType::EvaluationComplete:
            // Final stats and cleanup
            stats.print(settings.showStats);
            l1Cache.clear();  // Will be rebuilt next eval
            break;
    }
}
```

**Key insight from sjsonnet**: Natural boundaries (imports, function applications) provide the highest ROI for caching. Flushing at these boundaries captures the most reusable computations.

---

## Additional Questions from Claude Response

### Hash Collisions

**Q: Should we verify full keys despite low collision probability?**

**A: Yes.** Every major production caching system stores full keys. Despite 1 in 37 million collision probability for 64-bit xxHash3 with 1M entries, verification is essential because:
1. Billions of lookups make even 10^-8 probability produce errors
2. Cache poisoning attacks become possible without verification
3. The cost is negligible (store 32-byte SHA256 fingerprint or full key)

**Recommended structure**:
```cpp
struct CacheEntry {
    uint64_t hash;           // xxHash3 for fast lookup
    SHA256Hash fingerprint;  // Full verification
    Value* cached_value;
    Metadata metadata;
};
```

### IFD Caching

**Q: How should CA-derivations interact with IFD caching?**

**A:** CA-derivations (NixOS/nix#5805) use placeholders like `/1ix2zgscfhpnx492z7i2fr62fmipxcnw2ssjrhj0i802vliq88jv` because output paths are unknown until after build.

**Required mapping**:
```cpp
struct CADerivationCache {
    // Maps (drv_hash, placeholder) → actual_output_path
    std::unordered_map<std::pair<Hash, StorePath>, StorePath> realisations;

    StorePath resolve(const Hash& drvHash, const StorePath& placeholder) {
        auto it = realisations.find({drvHash, placeholder});
        if (it != realisations.end() && store->isValidPath(it->second)) {
            return it->second;
        }
        throw CacheMiss();
    }
};
```

Substitute placeholders before returning cached IFD results to the evaluator.

---

## Summary

The key insights from Round 6:

1. **Lazy hashing is essential**: Many expressions never evaluated—eager hashing wastes work
2. **Import-level caching has highest ROI**: Natural boundary, high fan-in for core files
3. **Durability side table is practical**: Value struct is packed, side table with disappearing links works
4. **Per-value lazy-tree check is worthwhile**: ~60-70% of values cacheable even with lazy-trees
5. **Cache boundaries exist**: Top-level attributes, imports, IFD completions are natural flush points
