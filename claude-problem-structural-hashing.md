# Structural Hashing for Lazy Evaluation Memoization in Nix

## Executive Summary

This document describes the technical challenges of implementing function call memoization in the Nix evaluator to enable sharing across multiple instantiations of the same package set. The core problem is computing structural hashes of lazy values (thunks) without forcing evaluation, while maintaining correctness in the presence of garbage collection and hash collisions.

---

## 1. Nix's Lazy Evaluation Model

### 1.1 Value Representation

Nix values are represented by a discriminated union type `Value` with the following internal types:

```cpp
enum InternalType {
    tUninitialized = 0,
    tInt = 1,
    tBool = 2,
    tString = 3,
    tPath = 4,
    tNull = 5,
    tAttrs = 6,
    tList = 7,        // Small list (≤2 elements, inline storage)
    tListN = 8,       // Large list (heap-allocated array)
    tThunk = 9,       // Unevaluated closure
    tApp = 10,        // Unevaluated function application
    tLambda = 11,     // Lambda function
    tPrimOp = 12,     // Built-in primitive operation
    tPrimOpApp = 13,  // Partial application of primop
    tExternal = 14,   // External C++ value
    tFloat = 15,
};
```

Each `Value` is 16 bytes on 64-bit systems (8-byte payload + 8-byte type tag and flags).

### 1.2 Thunk Representation

A thunk represents an unevaluated computation. There are two thunk types:

**tThunk (Closure Thunk)**:
```cpp
struct Thunk {
    Env * env;    // Lexical environment
    Expr * expr;  // Expression to evaluate
};
```

**tApp (Application Thunk)**:
```cpp
struct App {
    Value * left;   // Function value
    Value * right;  // Argument value
};
```

When a thunk is forced (evaluated), the `Value` is **mutated in-place** to hold the result. The internal type changes from `tThunk`/`tApp` to the result type (e.g., `tInt`, `tAttrs`). This mutation is observable to all references to that `Value*`.

### 1.3 Environment Structure

Environments (`Env`) form a linked chain of lexical scopes:

```cpp
struct Env {
    Env * up;           // Parent environment (lexical scope chain)
    Value * values[];   // Flexible array of bound values (variable bindings)
};
```

The size of the `values` array is determined at allocation time based on the number of bindings in the scope. There is no stored size field—the evaluator tracks this statically from the AST.

### 1.4 Lambda Representation

Lambda functions are represented as:

```cpp
struct Lambda {
    ExprLambda * fun;  // Pointer to AST node (contains parameter info, body expr)
    Env * env;         // Closure environment (captured variables)
};
```

Two lambdas with the same `(fun, env)` pair are semantically identical—they will produce the same result for the same arguments.

### 1.5 Attribute Sets

Attribute sets are the primary data structure in Nix:

```cpp
struct Bindings {
    size_t size_;
    Attr attrs[];  // Sorted by symbol for binary search
};

struct Attr {
    Symbol name;    // Interned string identifier
    Value * value;  // May be a thunk (lazy attribute)
    PosIdx pos;     // Source position
};
```

Attributes are stored sorted by `Symbol` (interned string ID) for O(log n) lookup. Attribute values can be thunks—accessing an attribute may trigger evaluation.

### 1.6 Strings with Context

Nix strings carry **dependency context** for build reproducibility:

```cpp
struct StringWithContext {
    const StringData * str;     // The string content
    const Context * context;    // Store path dependencies (may be null)
};

struct Context {
    size_t size_;
    const StringData * elems[];  // Serialized context elements
};
```

Context elements are one of:
- **Opaque**: A plain store path (e.g., `/nix/store/xxx-foo`)
- **DrvDeep**: A derivation and all its outputs (serialized as `=<drvPath>`)
- **Built**: A specific derivation output (serialized as `!<output>!<drvPath>`)

Two strings with identical text but different contexts are **semantically different**—they have different build dependencies.

### 1.7 Thunk Avoidance Optimization

For certain expression types, Nix avoids creating thunks entirely:

```cpp
// String literals return a pointer to the Value stored in the AST node
Value * ExprString::maybeThunk(EvalState & state, Env & env) {
    return &v;  // Same Value* for all uses of this literal
}

// Variable references return the bound value directly if already evaluated
Value * ExprVar::maybeThunk(EvalState & state, Env & env) {
    Value * v = state.lookupVar(&env, *this, true);
    if (v) return v;  // Direct pointer to bound value
    return Expr::maybeThunk(state, env);  // Create thunk if lookup fails
}
```

This means the same `Value*` may be passed to multiple function calls for the same literal or variable reference.

---

## 2. The Memoization Goal

### 2.1 Problem Statement

Consider a NixOS flake that defines multiple system configurations:

```nix
{
  nixosConfigurations.system1 = nixpkgs.lib.nixosSystem {
    system = "x86_64-linux";
    modules = [...];
  };
  nixosConfigurations.system2 = nixpkgs.lib.nixosSystem {
    system = "x86_64-linux";
    modules = [...];
  };
}
```

Currently, each configuration:
1. Calls `lib.systems.elaborate "x86_64-linux"` → creates fresh `localSystem` thunks
2. Calls `import nixpkgs { localSystem = ...; ... }` → evaluates the nixpkgs body separately
3. Creates an independent package set with its own thunks

**Result**: Two configurations with the same `system` cannot share package thunks. Evaluating both requires roughly 2x the work and memory.

### 2.2 Desired Outcome

With memoization:
1. `lib.systems.elaborate "x86_64-linux"` returns the **same `Value*`** for both configurations
2. Since `localSystem` is pointer-identical, the import arguments hash identically
3. The import returns the **same package set thunks** for both configurations
4. Evaluation work is shared; forcing a package thunk in one configuration forces it for both

### 2.3 Why Import-Level Memoization Is Insufficient

Nix already caches file imports via `fileEvalCache`:

```cpp
// Simplified: import caching
if (auto cached = getConcurrent(*fileEvalCache, path)) {
    return *cached;  // Return cached lambda
}
// ... evaluate file, cache result
```

However, this only caches the **lambda** returned by evaluating a file, not its **applications**. The arguments to `import nixpkgs { ... }` differ in pointer identity even when structurally identical, causing cache misses.

The insight is that **all lambda calls** need memoization, not just imports. Functions like `lib.systems.elaborate` must return the same `Value*` for identical arguments so that downstream caches can hit.

---

## 3. Current Memoization Implementation

### 3.1 Cache Key Structure

```cpp
struct LambdaCallMemoKey {
    const ExprLambda * fun;  // Lambda's AST node (code identity)
    const Env * env;         // Lambda's closure environment
    size_t argsHash;         // Structural hash of argument

    bool operator==(const LambdaCallMemoKey & other) const {
        return fun == other.fun && env == other.env && argsHash == other.argsHash;
    }
};
```

The key identifies:
- **Which function**: `(fun, env)` pair identifies the lambda (code + captured variables)
- **Which argument**: `argsHash` distinguishes different argument values

### 3.2 Cache Storage

```cpp
boost::concurrent_flat_map<
    LambdaCallMemoKey,
    Value *,
    LambdaCallMemoKeyHash,
    std::equal_to<LambdaCallMemoKey>,
    traceable_allocator<std::pair<const LambdaCallMemoKey, Value *>>>
    lambdaCallMemoCache;
```

Uses Boost's concurrent hash map with `traceable_allocator` for Boehm GC integration.

### 3.3 Memoization Logic in callFunction()

```cpp
// In callFunction(), before evaluating lambda body:
size_t argsHash = hashValueLazy(*args[0]);
bool canMemoize = (argsHash != 0);  // 0 means "contains thunks, skip"
LambdaCallMemoKey memoKey{&lambda, vCur.lambda().env, argsHash};

if (canMemoize) {
    if (auto cached = getConcurrent(*lambdaCallMemoCache, memoKey)) {
        vCur = **cached;  // Cache hit: use cached result
        continue;
    }
}

// ... evaluate lambda body ...

if (canMemoize) {
    auto * cachedResult = allocValue();
    *cachedResult = vCur;
    lambdaCallMemoCache->emplace(memoKey, cachedResult);
}
```

### 3.4 Current Hash Function

```cpp
size_t hashValueLazy(Value & v) {
    // For thunks, return 0 to signal "don't memoize"
    if (v.isThunk() || v.isApp()) {
        return 0;
    }

    size_t hash = std::hash<int>{}(static_cast<int>(v.type()));

    switch (v.type()) {
    case nInt:
        hashCombine(hash, std::hash<NixInt::Inner>{}(v.integer().value));
        break;
    case nString:
        hashCombine(hash, std::hash<std::string_view>{}(v.string_view()));
        break;  // BUG: context not hashed
    case nAttrs: {
        auto * attrs = v.attrs();
        hashCombine(hash, attrs->size());
        for (auto & attr : *attrs) {
            hashCombine(hash, std::hash<Symbol>{}(attr.name));
            size_t valHash = hashValueLazy(*attr.value);
            if (valHash == 0) return 0;  // Contains thunk, abort
            hashCombine(hash, valHash);
        }
        break;
    }
    case nList: {
        auto list = v.listView();
        hashCombine(hash, list.size());
        for (auto * elem : list) {
            size_t elemHash = hashValueLazy(*elem);
            if (elemHash == 0) return 0;  // Contains thunk, abort
            hashCombine(hash, elemHash);
        }
        break;
    }
    case nFunction:
        if (v.isLambda()) {
            auto lambda = v.lambda();
            hashCombine(hash, hashPointer(lambda.fun));
            hashCombine(hash, hashPointer(lambda.env));
        }
        // ... other function types use pointer hashing
        break;
    // ... other types
    }
    return hash;
}
```

### 3.5 Performance Results

On NixOS `release.nix closures` evaluation:

| Metric | Value |
|--------|-------|
| Function calls | 85,003,674 |
| Cache hits | 3,104,684 (~3.5%) |
| Cache entries | 21,611,539 |
| CPU time (no GC) | 25.5s |
| CPU time (with GC) | 93.3s |

The low hit rate is because many function arguments contain thunks, which the current implementation cannot hash.

---

## 4. Identified Problems

### 4.1 Problem 1: String Context Not Hashed

**Current code**:
```cpp
case nString:
    hashCombine(hash, std::hash<std::string_view>{}(v.string_view()));
    break;
```

**Issue**: Two strings with identical text but different `NixStringContext` (store path dependencies) produce the same hash.

**Example**:
```nix
let
  a = "${drv1}";  # Context: {drv1.outPath}
  b = "${drv2}";  # Context: {drv2.outPath}
in
  # If drv1 and drv2 have same output path text, a and b hash identically
  # but have different build dependencies
```

**Impact**: Functions receiving these strings would incorrectly share cached results, potentially producing builds with wrong dependencies.

### 4.2 Problem 2: No Hash Collision Defense

**Current code**:
```cpp
struct LambdaCallMemoKey {
    // ...
    bool operator==(const LambdaCallMemoKey & other) const {
        return fun == other.fun && env == other.env && argsHash == other.argsHash;
    }
};
```

**Issue**: Equality is based solely on hash comparison. If two different arguments produce the same hash (collision), the cache returns incorrect results.

**Risk**: With 64-bit hashes and ~85 million function calls, birthday paradox suggests collisions become likely around √(2^64) ≈ 4 billion calls. However, hash quality and input distribution affect this.

**Impact**: Silent incorrect results—functions receive cached outputs from different arguments.

### 4.3 Problem 3: Thunks Cannot Be Hashed Safely

**The fundamental tension**: To maximize sharing, we want to memoize calls with thunk arguments. But thunks are problematic:

**Approach A: Skip thunks entirely (current)**
```cpp
if (v.isThunk() || v.isApp()) {
    return 0;  // Don't memoize
}
```

**Problem**: Most function arguments contain thunks (attribute sets with lazy values). Only ~25% of calls can be memoized.

**Approach B: Hash thunk structure (Env*, Expr*)**
```cpp
case tThunk:
    hashCombine(hash, hashPointer(thunk.env));
    hashCombine(hash, hashPointer(thunk.expr));
    break;
```

**Problem**: **GC pointer reuse**. The Boehm garbage collector can reclaim a thunk's memory and reallocate it for a different thunk with the same `(Env*, Expr*)` pointer values but different semantic content.

**Observed failure**: During NixOS evaluation, this caused "expected a set but found null" errors—the cache returned results from calls with completely different (but pointer-identical after GC) thunk arguments.

**Approach C: Force thunks before hashing**

**Problem**: This destroys laziness. Forcing thunks may trigger arbitrary computation, including side effects like network access (for fetchers) or infinite loops.

### 4.4 Problem 4: Value Mutation After Thunk Forcing

When a thunk is forced, the `Value` is mutated in-place:

```cpp
// Before forcing:
v.type() == tThunk
v.thunk() == {env, expr}

// After forcing:
v.type() == tInt  // (or whatever the result type is)
v.integer() == 42
```

**Issue**: If we hash a value before forcing, then force it, then hash again, we get different hashes. This breaks the invariant that the same logical value produces the same hash.

**Scenario**:
1. Call `f(x)` where `x` is a thunk → compute hash H1
2. `f` forces `x`, mutating it to an integer
3. Call `f(x)` again → compute hash H2 (now hashing the integer)
4. H1 ≠ H2, cache miss even though it's semantically the same call

### 4.5 Problem 5: Structural Equality for Collision Detection

To defend against hash collisions, we need to verify structural equality on cache hits. This requires `equalValueLazy(Value& a, Value& b)` that:

1. Returns `true` if values are structurally equal
2. Returns `false` if values differ
3. Returns `nullopt` or equivalent if comparison is impossible (thunks present)

**Challenges**:
- Must not force thunks (same constraints as hashing)
- Must handle all value types including recursive structures
- Must be efficient (called on every potential cache hit)

---

## 5. Proposed Solutions

### 5.1 Fix String Context Hashing

Hash the context after the string content:

```cpp
case nString:
    hashCombine(hash, std::hash<std::string_view>{}(v.string_view()));
    if (auto * ctx = v.context()) {
        hashCombine(hash, ctx->size());
        for (auto * elem : *ctx) {
            // elem->view() returns serialized context element
            hashCombine(hash, std::hash<std::string_view>{}(elem->view()));
        }
    }
    break;
```

The context is stored in sorted order, so iteration is deterministic.

### 5.2 Add Collision Defense with Stored Arguments

Change cache to store the original argument:

```cpp
struct LambdaCallMemoEntry {
    Value * arg;     // Original argument for equality verification
    Value * result;  // Cached result
};
```

On cache hit, verify equality:

```cpp
if (auto cached = getConcurrent(*cache, key)) {
    auto eq = equalValueLazy(*args[0], *cached->arg);
    if (eq && *eq) {
        return cached->result;  // Verified hit
    }
    // Collision or incomparable: treat as miss
}
```

### 5.3 Thunk Handling Strategies

**Conservative (current)**: Skip memoization when thunks present. Safe but limits sharing.

**Optimistic with validation**: Hash thunks by pointer, but require structural equality check on hit. If cached argument has been GC'd and reallocated, equality check fails, treating as miss.

**Semantic hashing**: For thunks, hash the expression structure (AST) instead of pointers. This is pointer-stable but expensive and may still miss equivalences due to alpha-renaming.

**Forcing with bounds**: Force thunks up to a depth limit or computation budget. Risky due to side effects.

---

## 6. Open Research Questions

### 6.1 Can We Hash Thunks Safely?

Is there a way to assign stable identities to thunks that:
- Survives garbage collection
- Doesn't require forcing the thunk
- Provides meaningful equality (same thunk = same result when forced)

Potential approaches:
- **Allocation-time unique IDs**: Assign monotonic IDs to thunks at creation. Survives GC but requires modifying the allocator.
- **Expression fingerprinting**: Hash the AST structure. Stable but expensive and may not capture environment differences.
- **Weak references with resurrection**: Keep thunks alive if referenced by cache. Changes GC semantics.

### 6.2 Can We Use Incremental/Demand-Driven Hashing?

Instead of hashing the entire argument upfront:
- Start with a cheap approximation (type, size, shallow structure)
- Refine hash as values are forced during evaluation
- Update cache key incrementally

This aligns with lazy evaluation semantics but complicates cache management.

### 6.3 Can We Memoize at a Different Granularity?

Instead of memoizing individual function calls:
- **Memoize at module/file boundaries**: Cache entire file evaluation results
- **Memoize known-expensive functions**: Whitelist functions like `lib.systems.elaborate`
- **User-controlled memoization**: Add `builtins.memoize` primitive

### 6.4 Can We Avoid Hashing Entirely?

Alternative approaches to detect argument equivalence:
- **Pointer-based with GC cooperation**: Ensure GC never reuses pointers for semantically different values during evaluation
- **Linear types / uniqueness types**: Track value provenance through the type system
- **Content-addressed values**: Like Nix store paths, assign hashes at creation time and store them with values

---

## 7. Codebase Reference

Key files for implementation:

| File | Purpose |
|------|---------|
| `src/libexpr/eval.cc` | Main evaluator, `callFunction()`, thunk forcing |
| `src/libexpr/include/nix/expr/eval.hh` | `EvalState`, caches, `Env` structure |
| `src/libexpr/include/nix/expr/value.hh` | `Value` type, internal types, accessors |
| `src/libexpr/value-hash.cc` | Current `hashValueLazy()` implementation |
| `src/libexpr/include/nix/expr/value/context.hh` | `NixStringContext`, `NixStringContextElem` |
| `src/libexpr/nixexpr.hh` | Expression AST types (`ExprLambda`, etc.) |

Key functions:
- `EvalState::callFunction()` (~line 1535): Lambda call dispatch and memoization point
- `EvalState::forceValue()`: Thunk forcing
- `EvalState::forceValueDeep()`: Recursive thunk forcing
- `EvalState::eqValues()`: Existing equality (forces thunks, not suitable for lazy hashing)

---

## 8. Summary of Constraints

Any solution must satisfy:

1. **Correctness**: Never return wrong cached results
2. **Laziness preservation**: Never force thunks just for cache key computation
3. **GC safety**: Handle pointer reuse after garbage collection
4. **Performance**: Overhead must not exceed memoization benefit
5. **Determinism**: Same logical value must produce same hash across runs

The current implementation achieves (1), (2), and (5) but with limited sharing due to skipping thunks. The string context bug violates (1). The lack of collision defense violates (1) probabilistically.

---

## 9. Appendix: Value Type Details

### Integer
```cpp
struct { NixInt value; }  // NixInt wraps int64_t
```

### Float
```cpp
struct { NixFloat value; }  // NixFloat is double
```

### Boolean
```cpp
struct { bool value; }
```

### String
```cpp
struct StringWithContext {
    const StringData * str;
    const Context * context;  // null if no context
};
```

### Path
```cpp
struct Path {
    SourceAccessor * accessor;  // Filesystem accessor
    const StringData * path;    // Path string
};
```

### Attribute Set
```cpp
struct { Bindings * attrs; }  // Pointer to sorted attribute array
```

### List (small, ≤2 elements)
```cpp
struct { Value * elems[2]; size_t size; }  // Inline storage
```

### List (large)
```cpp
struct { Value * const * elems; size_t size; }  // Heap array
```

### Thunk
```cpp
struct { Env * env; Expr * expr; }
```

### Application
```cpp
struct { Value * left; Value * right; }
```

### Lambda
```cpp
struct { ExprLambda * fun; Env * env; }
```

### Primitive Operation
```cpp
struct { PrimOp * primOp; }
```

### Primitive Operation Application
```cpp
struct { Value * left; Value * right; }
```

### External
```cpp
struct { ExternalValueBase * value; }
```
