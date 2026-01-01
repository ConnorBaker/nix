# Round 7 Answers

Answers to Kagi's 16 questions from round-7.md, plus additional questions from Claude's response.

---

## Thread Safety

### Q1: Is Nix currently built with thread-safe Boehm GC? What compilation flags are used?

**Answer**: **Yes**, Nix is built with thread-safe Boehm GC.

From `src/libexpr/include/nix/expr/eval-gc.hh:12`:
```cpp
#  define GC_THREADS 1
```

This is defined before including Boehm GC headers in `eval.hh:24`:
```cpp
// For `NIX_USE_BOEHMGC`, and if that's set, `GC_THREADS`
```

The `-DGC_THREADS` flag enables:
- Thread-safe garbage collection (stops all threads during GC)
- Parallel marking with N-1 dedicated threads (N = number of processors)
- Support for `THREAD_LOCAL_ALLOC` for reduced lock contention

**Status**: Thread-safe GC is already enabled. No changes needed for parallel evaluation compatibility at the GC level.

---

### Q2: How does the parallel evaluation branch handle concurrent `forceValue()` calls? Is there existing synchronization we can reuse?

**Answer**: The current codebase uses `boost::concurrent_flat_map` for thread-safe containers but does **not** have explicit synchronization in `forceValue()` itself.

**Evidence from codebase**:

1. **Concurrent containers exist** (`eval.hh:526-537`):
   ```cpp
   const ref<boost::concurrent_flat_map<SourcePath, StorePath>> srcToStore;
   const ref<boost::concurrent_flat_map<SourcePath, SourcePath>> importResolutionCache;
   const ref<boost::concurrent_flat_map<...>> fileEvalCache;
   ```

2. **No black-holing in current `forceValue`**: The current implementation does not mark thunks as "being evaluated" to prevent duplicate work.

3. **Comment acknowledges limitation** (`eval.cc:2930`):
   ```cpp
   // probably won't encounter it in practice, because the CLI isn't concurrent
   ```

**For memoization**, we need to add:
- Black-holing mechanism (atomic CAS to claim thunk ownership)
- `std::shared_mutex` wrappers around `thunkMemoCache`
- Or migrate to `boost::concurrent_flat_map` for the memo cache

---

### Q3: Are there existing concurrent data structures in the Nix codebase?

**Answer**: **Yes**, Nix uses `boost::concurrent_flat_map` extensively.

From `eval.hh:28`:
```cpp
#include <boost/unordered/concurrent_flat_map_fwd.hpp>
```

**Usage locations**:
- `srcToStore` — maps source paths to store paths
- `importResolutionCache` — caches import path resolution
- `fileEvalCache` — caches file evaluations
- `primops.cc:4785` — fetchURL cache

**No usage of**:
- `folly::ConcurrentHashMap`
- `tbb::concurrent_hash_map`
- `std::shared_mutex` (for custom locking)

**Recommendation**: Use `boost::concurrent_flat_map` for consistency with existing code, rather than introducing folly or TBB dependencies.

---

### Q4: How is `fileEvalCache` currently protected in parallel evaluation? Is it thread-local or shared with synchronization?

**Answer**: `fileEvalCache` is **shared with built-in synchronization** via `boost::concurrent_flat_map`.

From `eval.hh:537-543`:
```cpp
const ref<boost::concurrent_flat_map<
    SourcePath, Value *, boost::hash<SourcePath>, std::equal_to<>
>> fileEvalCache;
```

From `eval.cc:1121`:
```cpp
if (auto v2 = getConcurrent(*fileEvalCache, *resolvedPath)) {
    // ...
}
```

And `eval.cc:1134`:
```cpp
fileEvalCache->try_emplace_and_cvisit(
    // ...
);
```

**Key insight**: The `try_emplace_and_cvisit` pattern ensures atomic insertion with a visitor callback—this pattern should be reused for `thunkMemoCache`.

---

## Closed-Term Optimization

### Q5: Is `is_closed` currently computed during `bindVars()`? If not, what would be required to add it?

**Answer**: **No**, `is_closed` is **not currently computed** in the codebase.

**Search results**: No matches for `is_closed`, `isClosed`, or `closed.*term` in `src/libexpr`.

**To add it**, we need:

1. Add field to `Expr` base class:
   ```cpp
   struct Expr {
       mutable bool is_closed = false;  // Computed during bindVars
   };
   ```

2. Compute during `bindVars()`:
   ```cpp
   void ExprLambda::bindVars(EvalState& state, StaticEnv& env) {
       StaticEnv bodyEnv(false, &env, formals);
       body->bindVars(state, bodyEnv);

       // Closed if body has no free variables from outer scope
       is_closed = (computeMaxFreeLevel(0) == 0);
   }
   ```

3. Use De Bruijn index tracking to detect free variables:
   ```cpp
   int ExprVar::computeMaxFreeLevel(int depth) {
       return (level >= depth) ? (level - depth + 1) : 0;
   }
   ```

**Estimated effort**: ~200 lines of code, touching ~10 `Expr` subclasses.

---

### Q6: What percentage of expressions in a typical Nixpkgs evaluation are closed terms?

**Answer**: **Unknown** — no existing profiling data. However, from Dolstra (2008):

> "This optimization alone makes maximal laziness feasible. Without it, `nix-env -qa` doesn't finish; with it, it runs in 2.75 seconds."

**Hypothesis**: A significant percentage of expressions are closed, especially:
- Functions in `lib/` (pure utilities)
- Constants and configuration defaults
- Derivation builders (fixed inputs)

**Recommendation**: Add profiling counter:
```cpp
Counter nrClosedTerms;
Counter nrNonClosedTerms;
```

And measure with `NIX_SHOW_STATS=1` on real Nixpkgs evaluations.

---

### Q7: Are there any expressions that appear closed but have hidden dependencies?

**Answer**: **Yes**, several cases:

| Expression | Hidden Dependency |
|------------|-------------------|
| `builtins.import` | Opens a file → depends on file system |
| `builtins.scopedImport` | Injects variables → creates implicit env dependency |
| `builtins.fetchurl` | Network access → non-deterministic |
| `builtins.currentTime` | Clock → impure |
| `builtins.getEnv` | Environment variables → impure |

**Solution**: A closed term can still be impure. The caching strategy should:
1. Check `is_closed` for cache key simplification (skip env hashing)
2. Still check impurity token before caching result
3. Assign appropriate durability based on primops used

**Code pattern**:
```cpp
if (expr->is_closed && !wasImpure) {
    // Cache with simplified key + HIGH durability
} else if (expr->is_closed && wasImpure) {
    // Cache with simplified key + LOW durability (or don't cache)
}
```

---

## Import Caching

### Q8: How does `resolvedPath.accessor->fingerprint()` work? Is it content-based or metadata-based?

**Answer**: Fingerprinting is implemented in `EvalInputs::fingerprint()` (`eval-inputs.cc:28`), and it is **content-based**.

The fingerprint includes:
- Nix version
- Pure eval mode flags
- Current system
- NIX_PATH entries (order-preserving)
- Allowed URIs
- Flake lock hash (if present)
- Root accessor fingerprint (if present)

For file-level fingerprints, `SourceAccessor::fingerprint()` uses **content hashing**:
- Store paths: content-addressed by definition
- Regular files: hash of file contents
- Git trees: tree hash

**Key code** (`eval-inputs.cc:28-100`):
```cpp
ContentHash EvalInputs::fingerprint() const {
    HashSink sink(evalHashAlgo);
    writeString(nixVersion);
    // ... flags, system, NIX_PATH, allowedUris, flakeLockHash ...
    return ContentHash{sink.finish().hash};
}
```

---

### Q9: What is the typical size of a serialized import result?

**Answer**: **Varies widely**, but estimates:

| Import Type | Typical Size |
|-------------|--------------|
| Simple module (e.g., `lib.nix`) | 1-10 KB |
| Package set (e.g., `python-packages.nix`) | 100 KB - 1 MB |
| NixOS module (e.g., `services/web-servers/nginx/default.nix`) | 5-50 KB |
| Top-level `all-packages.nix` | 10-100 MB (contains unevaluated thunks) |

**LMDB considerations**:
- Default max DB size: 10 GB (configurable)
- Default max readers: 126
- Entry overhead: ~16 bytes per entry + key size

**Recommendation**: Start with a 1 GB limit, measure actual usage:
```cpp
MDB_env* env;
mdb_env_create(&env);
mdb_env_set_mapsize(env, 1ULL * 1024 * 1024 * 1024);  // 1 GB
```

---

### Q10: Are there imports that should NOT be cached?

**Answer**: **Yes**, several categories:

| Import Type | Reason | Solution |
|-------------|--------|----------|
| `builtins.fetchurl` without hash | Non-deterministic content | Don't cache unless `sha256` specified |
| `builtins.currentTime` in scope | Time-dependent | Mark as LOW durability |
| `builtins.getEnv` usage | Environment-dependent | Mark as LOW durability |
| Flakes with `follows` | Resolution depends on lockfile | Include lockfile hash in key |
| `builtins.path { filter = ...; }` | Filter function may be non-deterministic | Check filter for impurity |

**Existing mechanism**: `state.markImpure(ImpureReason::X)` already tracks impurity. Extend to durability levels:

```cpp
if (state.impureTokenCounter_ != impureTokenBefore) {
    // Something impure happened during import
    setDurability(result, Durability::LOW);
} else if (containsNonStorePath(result)) {
    setDurability(result, Durability::MEDIUM);
} else {
    setDurability(result, Durability::HIGH);
}
```

---

## Taint Propagation

### Q11: Where is `builtins.trace` implemented? Specifically, where would durability propagation be added?

**Answer**: `builtins.trace` is implemented at `primops.cc:1319-1335`.

```cpp
static void prim_trace(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    // Mark as impure BEFORE the side effect to prevent memoization
    state.markImpure(ImpureReason::Trace);

    state.forceValue(*args[0], pos);
    if (args[0]->type() == nString)
        printError("trace: %1%", args[0]->string_view());
    else
        printError("trace: %1%", ValuePrinter(state, *args[0]));

    state.forceValue(*args[1], pos);
    v = *args[1];
}
```

**Durability propagation point**: After `v = *args[1]`, add:
```cpp
// Inherit durability from result, not message
setDurability(&v, getDurability(args[1]));
```

**Note**: `trace` already calls `state.markImpure(ImpureReason::Trace)` which prevents memoization. This is correct for intra-eval caching, but for cross-eval we might want to allow caching the *result* with LOW durability.

---

### Q12: How does `builtins.addErrorContext` interact with exceptions?

**Answer**: It **wraps** the exception with additional context. From `primops.cc:1052-1070`:

```cpp
static void prim_addErrorContext(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    try {
        state.forceValue(*args[1], pos);
        v = *args[1];  // Success path: just return inner result
    } catch (Error & e) {
        NixStringContext context;
        auto message = state.coerceToString(pos, *args[0], context, ...);
        e.addTrace(nullptr, HintFmt(message), TracePrint::Always);
        throw;  // Re-throw with added context
    }
}
```

**Durability propagation**: On success, inherit from `args[1]`:
```cpp
try {
    state.forceValue(*args[1], pos);
    v = *args[1];
    setDurability(&v, getDurability(args[1]));  // ADD THIS
} catch (Error & e) {
    // Durability irrelevant—exception thrown
}
```

---

### Q13: Is there existing infrastructure for tracking "which primop produced this value"?

**Answer**: **Partially**. The codebase has:

1. **Impurity tracking** via `ImpureReason` enum and `markImpure()`:
   - `ImpureReason::Trace`
   - `ImpureReason::Warn`
   - (others as needed)

2. **Position tracking** via `PosIdx` in every primop

3. **No value provenance tracking** — there's no field indicating which primop produced a value

**For durability assignment**, we have two options:

**Option A**: Side table (current recommendation):
```cpp
std::unordered_map<Value*, Durability> durabilityTable;
```

**Option B**: Tagged values (requires memory layout changes):
```cpp
struct Value {
    // ... existing fields
    uint8_t durability : 2;  // 0=LOW, 1=MEDIUM, 2=HIGH
};
```

**Recommendation**: Use Option A (side table) to avoid touching the packed `Value` layout.

---

## Cache Eviction

### Q14: What are the typical access patterns for the thunk cache?

**Answer**: Based on analysis of Nix evaluation patterns, the access pattern is likely:

| Pattern | Evidence | Implication |
|---------|----------|-------------|
| **Temporal locality** | Recent thunks often re-accessed (e.g., `lib` functions) | LRU component beneficial |
| **Frequency skew** | Some thunks accessed thousands of times (hot), most accessed once (cold) | Frequency tracking beneficial |
| **Working set phases** | Package evaluation, then module system, then derivation | Phase-aware eviction helps |
| **Scans** | `nix search` scans all packages once | Scan resistance critical |

**Best fit**: **2Q** or **ARC** (both scan-resistant with frequency awareness).

**Not recommended**: Pure LRU (thrashes on scans), pure LFU (doesn't adapt to phase changes).

---

### Q15: Is there existing profiling data for cache hit rates?

**Answer**: **Yes**, basic statistics exist. From `eval.hh:1106` and `eval.cc:3032-3038`:

```cpp
Counter nrThunkMemoHits;
Counter nrThunkMemoMisses;

// In showStats():
{"hits", nrThunkMemoHits.load()},
{"misses", nrThunkMemoMisses.load()},
{"hitRate", nrThunkMemoHits.load() / (nrThunkMemoHits.load() + nrThunkMemoMisses.load())},
```

**Available via**: `NIX_SHOW_STATS=1 nix eval ...`

**Missing metrics** (should add):
- Hit rate by cache level (L1 vs L2)
- Average entry size
- Eviction count
- Time spent hashing vs evaluating
- Closed-term hit rate vs non-closed

---

### Q16: How does cache size affect evaluation time? Is there a point of diminishing returns?

**Answer**: **Unknown precisely**, but theoretical analysis suggests:

| Cache Size | Expected Effect |
|------------|-----------------|
| 10K entries | Captures hot working set; significant speedup |
| 100K entries | Covers most of Nixpkgs single-package eval |
| 1M entries | Full NixOS system evaluation; diminishing returns |
| 10M entries | Likely over-provisioned; memory waste |

**From rust-analyzer experience**: 8GB+ memory usage with aggressive caching; they implemented memory limits and LRU eviction.

**Recommendation**: Start with configurable limit:
```nix
# nix.conf
eval-cache-max-entries = 100000
eval-cache-max-size = 500M
```

And measure with real workloads to find optimal size.

---

## Additional Questions from Claude's Response

### Q17: What experimental feature flags should memoization use?

**Recommendation**: Use granular flags:
```nix
experimental-features = eval-memoization           # Basic memoization
experimental-features = eval-memoization-persist   # Cross-eval persistence
experimental-features = eval-memoization-share     # Cross-machine sharing
```

This allows independent stabilization of each capability.

### Q18: What graduation criteria should be defined?

Based on Claude's recommendations:
1. **Correctness**: Zero differential test failures over 6 months
2. **Performance**: <5% overhead on cache miss, >80% speedup on cache hit
3. **Stability**: No breaking changes to cache format for 2 releases
4. **Adoption**: Used in production by at least 3 major projects
5. **Documentation**: Complete user guide and troubleshooting docs

### Q19: Should `nix-collect-garbage` also clean evaluation caches?

**Answer**: **Yes**, with opt-out:
```bash
nix-collect-garbage           # Cleans both binary and eval caches
nix-collect-garbage --keep-eval-cache  # Keeps eval cache
```

Also add independent cache management:
```bash
nix eval-cache gc             # Clean eval cache only
nix eval-cache verify         # Check integrity
nix eval-cache stats          # Show usage
```

---

## Summary of Action Items from Q&A

| Question | Action Item | Priority |
|----------|-------------|----------|
| Q2 | Add black-holing to `forceValue()` | Blocking |
| Q3 | Use `boost::concurrent_flat_map` for `thunkMemoCache` | Blocking |
| Q5 | Implement `is_closed` computation in `bindVars()` | High |
| Q6 | Add profiling counters for closed terms | Medium |
| Q11 | Add durability propagation to `prim_trace` | High |
| Q12 | Add durability propagation to `prim_addErrorContext` | High |
| Q15 | Add detailed cache statistics to `NIX_SHOW_STATS` | Medium |
| Q16 | Add configurable cache size limits | Medium |
| Q17 | Define granular experimental feature flags | High |
