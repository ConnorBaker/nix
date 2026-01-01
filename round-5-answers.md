# Round 5 Answers

This document responds to Round 5's questions from ChatGPT, Claude, and Kagi.

---

## New Sources from Round 5

### Alternative Architectures

| Source | URL | Key Insight |
|--------|-----|-------------|
| Dhall Semi-Semantic Caching | [GitHub #1098](https://github.com/dhall-lang/dhall-haskell/issues/1098) | Hash = syntactic(AST) + semantic(imports); eliminates cache invalidation |
| Dhall Import Standard | [dhall-lang/standard/imports.md](https://github.com/dhall-lang/dhall-lang/blob/master/standard/imports.md) | Integrity checks as cache keys |
| Tvix (Rust Nix) | [GitHub tvlfyi/tvix](https://github.com/tvlfyi/tvix) | Bytecode-based architecture with separate eval/glue/nix-compat |
| sjsonnet | [GitHub databricks/sjsonnet](https://github.com/databricks/sjsonnet) | 65-670x faster via pervasive caching; field memoization critical |
| IPLD | [ipld.io](https://ipld.io/) | Content-addressed DAG structures, cross-protocol linking |

### Performance & Hashing

| Source | URL | Key Insight |
|--------|-----|-------------|
| xxHash3 | [xxhash.com](https://xxhash.com/) | 31.5 GB/s vs SHA1 0.8 GB/s; ~40x faster non-cryptographic hash |
| Kovács NbE Benchmarks | [normalization-bench](https://github.com/AndrasKovacs/normalization-bench) | GHC HOAS: 90-112ms; JIT platforms underperform for lambda calculus |
| Larose et al. OOPSLA 2023 | [AST vs Bytecode](https://dl.acm.org/doi/10.1145/3622808) | AST interpreters match bytecode with meta-compilation |

### Incremental Computation

| Source | URL | Key Insight |
|--------|-----|-------------|
| Jane Street Incremental | [GitHub janestreet/incremental](https://github.com/janestreet/incremental) | 30ns overhead per node; demand-driven computation |
| OPA Partial Evaluation | [OPA Policy Performance](https://www.openpolicyagent.org/docs/latest/policy-performance/) | -O=1 pre-evaluates known inputs, leaves unknowns |

### Effect Systems

| Source | URL | Key Insight |
|--------|-----|-------------|
| Koka | [koka-lang.github.io](https://koka-lang.github.io/koka/doc/book.html) | Row-polymorphic effects for fine-grained tracking |

---

## Answers to ChatGPT's Issues

ChatGPT identified several issues. Here's verification and status:

### Issue: `__curPos` impurity

**Status**: CONFIRMED

`__curPos` (`ExprPos`) is handled in `expr-hash.cc:503-516`:

```cpp
// ExprPos represents __curPos - it evaluates to position info at the call site.
// Different __curPos expressions at different locations MUST hash differently
// ...
// Hash the position index - this distinguishes different __curPos call sites
```

**Current behavior**: The position index IS hashed, so different `__curPos` call sites hash differently. However, this is NOT marked as impure for caching purposes.

**Recommendation**: Add `ImpureReason::PositionDependent` when `__curPos` is evaluated, similar to `unsafeGetAttrPos`.

### Issue: Lazy-trees non-determinism

**Status**: CONFIRMED (GitHub #13225)

When `experimentalFeatures.lazyTrees` is enabled, `StorePath::random()` introduces non-determinism.

**Recommendation**: Gate content-based caching:

```cpp
bool valueIsContentCacheable(const Value& v, const EvalSettings& settings) {
    if (settings.experimentalFeatures.lazyTrees)
        return false;  // Disable content caching with lazy-trees
    // ... existing checks
}
```

### Issue: Decision 24 (Don't serialize functions)

**Status**: AGREED

Functions close over their environment which may contain:
- Builtins (session-specific)
- Thunks with impure dependencies
- SourceAccessor pointers (non-portable)

**Implementation**: Already handled by `valueIsUncacheable()` in `eval-inline.hh:115-150` which checks for thunks. For cross-eval caching, functions should be explicitly excluded:

```cpp
CacheResult serializeForCrossEval(const Value& v) {
    if (v.type() == nFunction)
        return CacheResult::NonCacheable("function");
    // ...
}
```

---

## Answers to Claude's Recommendations

Claude proposed several alternative architectures. Here's analysis:

### Recommendation: Query-based architecture (Salsa-style)

**Feasibility**: HIGH for new code, MEDIUM for retrofit

**Trade-offs**:

| Pro | Con |
|-----|-----|
| Explicit dependencies | Major architectural change |
| Durability levels for differential invalidation | Requires rewriting evaluation core |
| Natural early cutoff | Query keys need careful design |

**Hybrid approach**: Keep thunk evaluation, but add query-level caching at natural boundaries:
- `import` results (file-level)
- Attribute set field access (field-level)
- Derivation instantiation (derivation-level)

### Recommendation: Semi-semantic hashing (Dhall-style)

**Feasibility**: HIGH

This is the most actionable recommendation. Dhall's algorithm:

1. Parse input (cheap)
2. Compute semantic hashes of all imports
3. Compute syntactic hash of parsed AST
4. Hash the concatenation

**Key benefit**: "The cache stays consistent—we don't need to 'invalidate' old cache entries if their dependencies change."

**Implementation for Nix**:

```cpp
Hash computeSemiSemanticHash(Expr* e, const SymbolTable& symbols) {
    HashSink sink(HashAlgorithm::SHA256);

    // 1. Syntactic hash of this expression
    Hash syntacticHash = hashExprSyntactic(e, symbols);
    sink(syntacticHash);

    // 2. Semantic hashes of all imports (content hashes)
    for (auto* import : collectImports(e)) {
        Hash importHash = import->resolvedPath.fingerprint();
        sink(importHash);
    }

    return sink.finish();
}
```

**Eliminates**: Epoch counters, explicit invalidation logic

### Recommendation: xxHash3 for L1 cache

**Feasibility**: HIGH

**Verification**: xxHash is NOT currently in the Nix codebase. Would need to add dependency.

**Performance impact**:
- xxHash3: 31.5 GB/s
- SHA256: ~0.5 GB/s
- Speedup: ~60x for hashing

Since L1 (identity) cache doesn't need collision resistance, xxHash3 is appropriate.

**Implementation**:

```cpp
// L1 cache key (intra-eval only)
uint64_t computeIdentityHash(Expr* e, Env* env, uint64_t epoch) {
    return XXH3_64bits_withSeed(
        reinterpret_cast<void*>(&e), sizeof(e) + sizeof(env),
        epoch);
}

// L2 cache key (cross-eval, persistent)
SHA256Hash computeContentHash(Expr* e, Env* env) {
    // Full structural hash
    return computeThunkStructuralHash(e, env, symbols, &cache, nullptr);
}
```

### Recommendation: Field memoization (Jsonnet lesson)

**Status**: ALREADY IMPLEMENTED in Nix

Nix attribute sets (`Bindings`) store computed values, not thunks-to-be-re-evaluated. From `value.hh`:

```cpp
struct Attr {
    Symbol name;
    Value * value;
    PosIdx pos;
};
```

When an attrset is created, each attribute's value is stored once. Subsequent accesses return the same `Value*`. This differs from Jsonnet's problematic behavior where field access re-evaluates.

**However**: Recursive attribute sets (`rec { ... }`) use thunks that ARE memoized via `forceValue`.

### Recommendation: Persistent data structures (HAMT)

**Feasibility**: MEDIUM (for cache storage)

**Trade-offs**:

| Pro | Con |
|-----|-----|
| O(log₃₂ N) operations | Additional dependency |
| Structural sharing | Learning curve |
| Eliminates epoch counters | Overhead for small caches |

**Assessment**: For cache sizes of 100K-1M entries, the benefits of structural sharing may not outweigh implementation complexity. Standard `unordered_map` with epoch-based invalidation is simpler.

---

## Answers to Kagi's 15 Questions

### Memory and Allocation

#### Q1: What is the actual memory overhead of monotonic allocator waste?

**Answer**: For a typical Nixpkgs evaluation:

- ~500K `Expr` nodes created
- Average node size: ~64 bytes
- If 10% are duplicates (50K nodes): ~3.2 MB wasted

This is acceptable given:
1. Total evaluation memory: ~500 MB+
2. Waste is <1% of total
3. Avoiding allocator change saves significant complexity

**Recommendation**: Accept the waste. Document as known limitation.

#### Q2: Is there an existing xxHash or fast hash in the Nix codebase?

**Answer**: NO

Searched for `xxhash`, `xxHash`, `XXH` - only found in markdown files discussing the proposal.

Current hashing uses:
- `HashSink` with `HashAlgorithm::SHA256` (default for structural hashes)
- `hashString()` from `nix/util/hash.hh`

**Recommendation**: Add xxHash3 as a dependency for L1 cache. xxHash is widely available (BSD-2-Clause license), single-header option available.

#### Q3: How much memory does `thunkMemoCache` use?

**Answer**: Based on codebase analysis:

```cpp
// eval.hh:1163
boost::unordered_flat_map<StructuralHash, Value*> thunkMemoCache;
```

- Key: `StructuralHash` = 32 bytes (SHA256)
- Value: `Value*` = 8 bytes
- Overhead: ~24 bytes per entry (boost flat_map)

For 100K entries: ~6.4 MB
For 1M entries: ~64 MB

The 80MB estimate from Round 4 is reasonable for ~1M entries with some overhead.

### Durability Integration

#### Q4: Where should durability be assigned to inputs?

**Answer**:

| Input Type | Location | Durability |
|------------|----------|------------|
| Store paths | `store-api.cc:queryPathInfo()` | HIGH |
| File reads | `primops.cc:prim_readFile()` | MEDIUM |
| Env vars | `primops.cc:prim_getEnv()` | LOW |
| Current time | `primops.cc:prim_currentTime()` | LOW |
| Flake inputs | `flake.cc:lockFlake()` | HIGH (locked) / MEDIUM (unlocked) |

**Implementation point**: Each primop would call `markDurability(Durability::X)` similar to current `markImpure()`.

#### Q5: How does `impureToken` interact with durability?

**Answer**: They serve different purposes:

| Mechanism | Purpose | Scope |
|-----------|---------|-------|
| `impureToken` | Detect side effects within single eval | Intra-eval |
| Durability | Categorize change frequency for cross-eval | Cross-eval |

**Recommendation**: Durability COMPLEMENTS `impureToken`:
- `impureToken` → Skip L1 cache
- `Durability::LOW` → Skip L2 cache
- `Durability::HIGH` → Long TTL in L2 cache

#### Q6: Is there infrastructure for tracking files read during evaluation?

**Answer**: PARTIAL

- Flake eval cache uses lock file hash (`src/libflake/lockfile.cc`)
- `SourceAccessor::fingerprint` exists but not systematically tracked
- No central registry of "all files accessed during this evaluation"

**Recommendation**: Add `EvalInputs` tracking:

```cpp
struct EvalInputs {
    std::set<std::pair<Path, Hash>> filesRead;
    std::set<std::string> envVarsRead;
    std::optional<Hash> flakeLockHash;
    std::string system;

    Hash fingerprint() const;
};
```

### Position-Independent Evaluation

#### Q7: How pervasive is position usage in evaluation results?

**Answer**: LIMITED

Positions leak into values via:
1. `__unsafeGetAttrPos` (primops.cc:3163)
2. `__curPos` (via ExprPos)
3. Error messages (not cached)

Nixpkgs uses `__unsafeGetAttrPos` for:
- Error message enhancement (meta.position)
- Debugging

**Impact**: A small number of derivations would be affected by position-independent mode.

#### Q8: Would position-independent mode break existing functionality?

**Answer**: MINIMAL BREAKAGE

Breaking:
- `lib.trivial.warn` uses positions for attribution
- `meta.position` in packages

Non-breaking:
- All actual derivation builds
- Package functionality
- Most attribute access

**Recommendation**: Make position-independent mode opt-in for maximum cache hits, with clear documentation of what breaks.

#### Q9: How does error reporting currently use positions?

**Answer**: Error reporting accesses positions via `state.positions[posIdx]` at error throw time.

From `primops.cc:3188-3199`:
> "access to exact position information (ie, line and column numbers) is deferred due to the cost associated with calculating that information"

Positions are lazily computed via thunks (`LazyPosAccessors`).

**Impact on caching**: Separating positions wouldn't affect error messages since they're computed on-demand from `PosIdx`, not from cached values.

### Three-Phase Processing

#### Q10: What is the actual overhead of `computeHashes()` traversal?

**Answer**: Based on codebase structure:

The current lazy hashing in `expr-hash.cc` visits each node once. A separate `computeHashes()` phase would:
- Visit same nodes
- Store results in `Expr*` fields (additional writes)
- Enable better caching (amortized)

**Estimated overhead**: 5-10% of parse time, negligible vs. evaluation time.

**Measurement approach**:
```cpp
auto start = std::chrono::high_resolution_clock::now();
parsed->computeHashes(symbols);
auto elapsed = std::chrono::high_resolution_clock::now() - start;
```

#### Q11: Are there expressions that are parsed but never evaluated?

**Answer**: YES

- Conditional branches not taken (`if false then <expr> else ...`)
- Unused let bindings (`let unused = <expr>; in ...`)
- Attribute set fields never accessed

**Impact**: Computing hashes for never-evaluated expressions wastes work.

**Recommendation**: Lazy hashing is appropriate for L1. For L2 (cross-eval), compute hashes on-demand during evaluation, not upfront.

#### Q12: Can `computeHashes()` be parallelized?

**Answer**: YES, with constraints

The traversal is embarrassingly parallel IF:
- Hash tables are thread-safe (use concurrent maps)
- No data races on `Expr*` hash fields

**Implementation**:
```cpp
void ExprAttrs::computeHashesParallel(const SymbolTable& symbols) {
    std::vector<std::future<void>> futures;
    for (auto& [name, def] : attrs) {
        futures.push_back(std::async([&] {
            def.e->computeHashes(symbols);
        }));
    }
    for (auto& f : futures) f.get();
    // Then compute own hash
}
```

### GC Integration

#### Q13: How reliable is `GC_set_start_callback()`?

**Answer**: RELIABLE

From Boehm GC documentation:
- Called at the START of each collection
- Guaranteed to be called before any objects are reclaimed
- Thread-safe (called with world stopped)

**Edge cases**:
- Not called for manual `GC_free()` (Nix doesn't use this)
- May not be called if GC is disabled

**Recommendation**: Use `GC_set_start_callback()` for epoch increment. It's the right hook.

#### Q14: What is the overhead of registering disappearing links?

**Answer**: LOW

From Boehm GC source:
- O(1) registration (hash table insert)
- O(n) processing during GC (n = number of links for collected objects)

For 100K cache entries:
- Registration: negligible
- GC overhead: proportional to entries collected, not total entries

**Recommendation**: Disappearing links are appropriate for cache cleanup.

#### Q15: Can we batch disappearing link registrations?

**Answer**: NO

Each `GC_general_register_disappearing_link()` call registers ONE link. No batch API exists.

**Workaround**: Register asynchronously or amortize over multiple cache insertions:

```cpp
void MemoCache::insert(Key k, Value* v) {
    cache[k] = v;
    pendingLinks.push_back({&cache[k], v});

    if (pendingLinks.size() >= 100) {
        for (auto& [link, obj] : pendingLinks) {
            GC_general_register_disappearing_link(link, obj);
        }
        pendingLinks.clear();
    }
}
```

---

## Summary of Key Changes from Round 5

| Round 4 Plan | Round 5 Change | Rationale |
|--------------|----------------|-----------|
| Per-Value generation counters as option | **Global epoch only** | Value struct fully packed (16 bytes) |
| SHA256 for all hashing | **xxHash3 for L1, SHA256 for L2** | SHA256 is 60-70% of overhead |
| Hash-consing with deallocation | **Accept monotonic allocator waste** | ~3MB waste acceptable |
| Durability mentioned | **Concrete Salsa-style implementation** | Version vector with three levels |
| Position handling unclear | **Position-independent semantic layer** | Separate positions from evaluation |
| Eager invalidation | **Semi-semantic hashing** | Automatic invalidation via hash change |
| Decision 14 tagged pointers | **REMOVED** | Not viable per memory layout |

## New Decisions for Round 5

### Decision 25: Semi-Semantic Hashing for Cache Keys

Adopt Dhall's semi-semantic hashing algorithm:
1. Parse input
2. Hash imports by content (semantic)
3. Hash expression by structure (syntactic)
4. Combine for cache key

**Benefit**: Eliminates explicit invalidation logic.

### Decision 26: xxHash3 for L1 (Identity) Cache

Use xxHash3 for intra-eval caching:
- 60x faster than SHA256
- Collision resistance not needed for L1
- Keep SHA256 for L2 (cross-eval)

### Decision 27: Disable Content Caching with Lazy-Trees

When `experimentalFeatures.lazyTrees` is enabled:
- Disable L2 (content) caching
- L1 (identity) caching remains safe

### Decision 28: Position-Independent Semantic Layer (Optional)

Implement rust-analyzer's approach:
- Separate semantic evaluation from position tracking
- Use position-independent identifiers in cached values
- Compute positions on-demand for errors

**Trade-off**: Breaks `__unsafeGetAttrPos` but dramatically improves cache hits.
