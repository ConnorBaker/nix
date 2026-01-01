# The Environment Size Problem in Nix Evaluation Caching

---

## ✅ RESOLVED (2025-12-30)

**This problem has been fixed.** The Env struct now stores its size:

```cpp
struct Env {
    Env * up;
    uint32_t size;      // NEW: tracks number of Value* slots
    Value * values[0];
};
```

The size is populated by `allocEnv()` at allocation time. Environment hashing
now uses content-based hashing throughout the parent chain, making env hashes:
- Stable within evaluations
- Stable across evaluations
- Portable across machines
- Suitable for persistent caching

The remaining blocker for enabling memoization is **impure expression detection**
to prevent caching of side-effectful builtins like `trace`, `currentTime`, etc.

---

## Goal (Original Problem Description Below)

Enable **content-addressed cross-evaluation caching** so that:

1. Evaluation results are keyed by **semantic content**, not memory addresses
2. Cache entries can be shared across:
   - Different evaluation sessions (time)
   - Different machines
   - Cosmetic code changes (variable renaming, reformatting)
   - Different nixpkgs commits (for unchanged code)

The target is **90%+ cache hit rate** when evaluating nixpkgs across commits, since most packages don't change between versions.

```
nixpkgs@commit1: evaluate pkgs.hello → hash = ABC123 → cache result
nixpkgs@commit2: evaluate pkgs.hello → hash = ABC123 → CACHE HIT (code unchanged)
```

## The Problem

### Environment Structure

The `Env` struct in Nix is minimal:

```cpp
struct Env {
    Env * up;           // Parent environment pointer
    Value * values[0];  // Flexible array member (FAM)
};
```

The `values[0]` is a C flexible array member - memory for N values is allocated after the struct, but **the size N is not stored anywhere**. When `allocEnv(size)` is called, the size is known at allocation time but immediately discarded.

### Why This Matters for Hashing

To compute a content-based hash of an environment, we need to:

1. Hash the number of values (size)
2. Hash each value's content: `values[0], values[1], ..., values[size-1]`
3. Recursively hash the parent environment

**Without knowing the size, we cannot iterate over the values array.** The code falls back to hashing the pointer address instead:

```cpp
// From thunk-hash.cc - what we're forced to do without size
auto envPtr = reinterpret_cast<uintptr_t>(env);
feedBytes(sink, &envPtrLE, sizeof(envPtrLE));
```

### Two Consequences

#### 1. Within-Evaluation Instability (Immediate Blocker)

Boehm GC can reclaim memory and reallocate it at the same address:

```
Time T1: Env A allocated at 0x1000, hash(thunk1) includes 0x1000, result R1 cached
Time T2: GC reclaims Env A
Time T3: Env B allocated at 0x1000, hash(thunk2) includes 0x1000 → COLLISION
         Cache returns R1 for thunk2 (WRONG!)
```

**Result**: Memoization causes incorrect results and infinite hangs. Currently disabled.

#### 2. Cross-Evaluation Non-Portability (Blocks the Goal)

Even without GC reuse, pointer values differ between:

| Scenario | Same Pointer? | Same Content Hash? |
|----------|---------------|-------------------|
| Same machine, second run | No | Yes |
| Different machine | No | Yes |
| Same code, different nixpkgs commit | No | Yes |

Pointer-based hashes cannot be shared across evaluations, defeating the entire goal.

### The Parent Chain Problem

Even if we knew the current env's size, we face the same problem recursively:

```cpp
struct Env {
    Env * up;           // Parent - what's ITS size?
    Value * values[0];
};
```

To hash an env by content, we must hash its parent by content, which requires knowing the parent's size, and so on up the chain. **Every env in the chain needs its size tracked.**

## Potential Solutions

### Option 1: Add Size Field to Env

```cpp
struct Env {
    Env * up;
    uint32_t size;      // NEW: 4 bytes
    Value * values[0];
};
```

**Pros**:
- Simple, direct access: `env.size`
- No additional data structures
- O(1) lookup

**Cons**:
- Memory overhead: 4 bytes per env
- Large evaluations create millions of envs
- Estimated overhead: ~40-80 MB for nixpkgs evaluation

**Implementation**:
- Modify `allocEnv()` to store size
- Update all env creation sites
- Minimal code changes

### Option 2: External Size Table

```cpp
// In EvalState or EvalMemory
std::unordered_map<const Env*, uint32_t> envSizes;

// Or with Boehm GC integration
GC_map<const Env*, uint32_t> envSizes;  // Weak keys
```

**Pros**:
- No change to Env struct
- No per-env memory overhead (only for envs we actually hash)
- Can be added incrementally

**Cons**:
- Hash table lookup overhead on every env hash
- Must handle GC: when env is collected, entry must be removed
- Boehm GC doesn't have built-in weak references (requires custom weak map or finalizers)
- Potential memory leaks if entries aren't cleaned up

**Implementation**:
- Add side table to `EvalMemory`
- Populate in `allocEnv()`
- Use GC finalizers or periodic cleanup to remove stale entries

### Option 3: Size-Segregated Allocation Pools

```cpp
// Different pools for different sizes
Pool<Env, 1> env1Pool;   // Size-1 envs
Pool<Env, 2> env2Pool;   // Size-2 envs
// ... etc, plus overflow for large sizes

size_t getEnvSize(const Env* env) {
    if (env1Pool.contains(env)) return 1;
    if (env2Pool.contains(env)) return 2;
    // ...
}
```

**Pros**:
- Zero storage overhead per env
- Size lookup is O(1) pointer range check
- Good cache locality for same-size envs

**Cons**:
- Complex implementation
- Fixed number of size classes
- Large envs need fallback mechanism
- Major architectural change

### Option 4: Encode Size in Pointer (Tagged Pointers)

```cpp
// Use low bits of pointer for small sizes (if aligned)
// Or high bits on 64-bit systems with <48-bit address space
Env* taggedPtr = encodeSize(env, size);
size_t size = decodeSize(taggedPtr);
Env* env = stripTag(taggedPtr);
```

**Pros**:
- Zero memory overhead
- O(1) access

**Cons**:
- Platform-specific (alignment, address space assumptions)
- Limited size range in tag bits
- Requires careful pointer handling everywhere
- Easy to introduce bugs

### Option 5: Hybrid - Size Field + Lazy Population

```cpp
struct Env {
    Env * up;
    mutable int32_t size = -1;  // -1 = unknown, lazily populated
    Value * values[0];
};
```

**Pros**:
- Only compute/store size when needed for hashing
- Compatible with existing code (unknown size = use pointer fallback)
- Gradual migration path

**Cons**:
- Still has per-env memory overhead
- `mutable` for lazy init in const contexts
- Some envs may never get size populated (need fallback)

## Recommendation

**Option 1 (Add Size Field)** is recommended for several reasons:

1. **Simplicity**: Minimal code changes, easy to understand and maintain
2. **Reliability**: No GC interaction complexity, no weak reference issues
3. **Performance**: O(1) direct field access, no hash table lookup
4. **Memory**: ~4 bytes/env is acceptable given the benefits
   - Even 10M envs = 40 MB overhead
   - Modern systems have plenty of RAM
   - The cache hit benefits far outweigh memory cost

**Implementation sketch**:

```cpp
// eval.hh
struct Env {
    Env * up;
    uint32_t size;
    Value * values[0];
};

// eval-inline.hh
Env & EvalMemory::allocEnv(size_t size) {
    // ... allocation code ...
    env->size = static_cast<uint32_t>(size);
    return *env;
}

// env-hash.cc
StructuralHash computeEnvStructuralHash(const Env & env, const SymbolTable & symbols) {
    // Now we can use env.size directly!
    for (size_t i = 0; i < env.size; ++i) {
        // Hash values[i] by content
    }
    // Recursively hash parent (which also has .size now)
    if (env.up) {
        auto parentHash = computeEnvStructuralHash(*env.up, symbols);
        // ...
    }
}
```

## Impact on Caching Goals

| With Env Sizes | Within-Eval Memo | Cross-Eval Cache | Notes |
|----------------|------------------|------------------|-------|
| Not tracked | Unsafe (GC reuse) | Impossible | Current state |
| Tracked | Safe | Possible | Enables content-based hashing |

Tracking env sizes is the **critical enabler** for both:
1. **Safe within-evaluation memoization** (fixes GC reuse problem)
2. **Cross-evaluation persistent caching** (enables content-based env hashes)

Without it, we're stuck with pointer-based hashes that are neither stable nor portable.
