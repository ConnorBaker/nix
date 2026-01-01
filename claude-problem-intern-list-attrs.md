# Problem Analysis: Interning Lists and Attribute Sets

## Executive Summary

Interning lists and attribute sets in the Nix evaluator fails due to **circular references** in value graphs. The naive recursive approach causes stack overflow. This document analyzes the problem in depth and explores solutions that enable full interning of all value types, including cyclic structures.

## The Failed Approach

### Implementation

```cpp
Value * EvalState::internValue(Value & v)
{
    // ...
    case nList: {
        auto view = v.listView();
        std::vector<Value *> internedElems;
        internedElems.reserve(view.size());
        for (auto * elem : view) {
            internedElems.push_back(internValue(*elem));  // RECURSIVE CALL
        }
        return internList(internedElems.data(), internedElems.size());
    }

    case nAttrs: {
        auto * bindings = v.attrs();
        auto builder = mem.buildBindings(symbols, bindings->size());
        for (const auto & attr : *bindings) {
            Value * internedVal = internValue(*attr.value);  // RECURSIVE CALL
            builder.insert(attr.name, internedVal, attr.pos);
        }
        return internAttrs(builder.finish());
    }
    // ...
}
```

### Failure Mode

Test `ca/duplicate-realisation-in-closure` failed with:
```
error: stack overflow (possible infinite recursion)
```

### Reproduction

Any Nix expression with cyclic values triggers the issue:

```nix
# Self-reference
let x = { a = x; }; in x

# Mutual recursion
let
  a = { b = b; };
  b = { a = a; };
in a

# Recursive attribute set
rec { self = { inherit self; }; }

# Function capturing its own binding
let f = { fn = f; }; in f
```

## Problem Analysis

### The Interning Invariant

The goal of interning is to establish:

> **Two values are semantically equal ⟺ Their interned pointers are equal**

This enables O(1) equality checking via pointer comparison, which is the foundation for efficient memoization.

### Current Key Design

The interning tables use structural keys:

```cpp
struct ListValueKey {
    std::vector<Value *, traceable_allocator<Value *>> elems;
    // Two lists equal iff same element pointers
};

struct AttrsValueKey {
    std::vector<std::pair<Symbol, Value *>, ...> bindings;
    // Two attrs equal iff same (name, value*) pairs
};
```

**Critical assumption**: Child pointers must already be interned (canonical) for the key to correctly identify structural equality.

### Why Cycles Break This

Consider:
```nix
let x = { a = 1; self = x; }; in x
```

The value graph:
```
x (attrs) ──┬── a: 1 (int)
            └── self: ───┐
                    ▲    │
                    └────┘
```

When interning `x`:
1. Start interning `x` (attrs with 2 bindings)
2. Intern child `a` → returns interned `Int(1)` ✓
3. Intern child `self` → this IS `x`
4. Recursively try to intern `x` → goto step 1
5. **Infinite recursion**

### The Fundamental Tension

To intern a compound value, we need its children's interned pointers.
But a child might BE (or transitively reference) the parent.
We can't have the interned parent before we have the interned children.
**Circular dependency.**

## Constraints

1. **Must handle arbitrary cycles**: Nix semantics allow cyclic data structures via laziness
2. **Must preserve semantic equality**: `let x = {a=1; s=x;}; y = {a=1; s=y;}; in x == y` should intern to the same pointer
3. **Must be GC-safe**: All Value pointers in intern tables must be traceable
4. **Must be thread-safe**: Concurrent evaluation requires safe concurrent access
5. **Performance**: Should not significantly degrade evaluation speed

## Solution Approaches

### Approach 1: Two-Phase Interning with Forwarding Table

**Concept**: Separate allocation from initialization. Allocate all interned values first, then fill in children using a forwarding table.

**Algorithm**:

```
Phase 1: Allocation
  visited = {}  // Value* → interned Value*
  worklist = [root]

  while worklist not empty:
    v = worklist.pop()
    if v in visited: continue

    internedV = allocateUninitializedValue()
    visited[v] = internedV

    for each child c of v:
      if c not in visited:
        worklist.push(c)

Phase 2: Initialization
  for (original, interned) in visited:
    initialize interned with:
      - same type as original
      - children looked up via visited[] table
```

**Handling semantic equality**:

The challenge is that two structurally-equal cyclic values should map to the same interned value:
```nix
let x = { a = 1; self = x; };
    y = { a = 1; self = y; };
in [x y]  # Should both intern to same Value*
```

After Phase 1, we'd have two different interned values. We need structural comparison.

**Extended algorithm**:

```
Phase 1: Allocation (as above)

Phase 2: Compute structural signatures
  For each allocated value, compute a signature that:
  - For acyclic parts: hash of content
  - For back-references: encode as "ref to ancestor at depth N"

Phase 3: Unification
  Group values by signature
  For each group, pick canonical representative
  Update forwarding table to point all group members to canonical

Phase 4: Initialization
  Initialize canonical representatives using forwarding table
```

**Complexity**: O(n) allocation, O(n) signature computation, O(n log n) or O(n) unification depending on signature comparison.

### Approach 2: Tarjan's Algorithm for Value SCCs

**Concept**: Treat the value graph as a directed graph. Use Tarjan's algorithm to find strongly connected components (SCCs). Each SCC is a maximal set of mutually-reachable values.

**Key insight**: Values in the same SCC must be interned together as a unit because they reference each other.

**Algorithm**:

```
1. Run Tarjan's SCC algorithm on value graph
2. Process SCCs in reverse topological order (leaves first)
3. For each SCC:
   a. If singleton with no self-loop: intern normally
   b. If has cycles: compute canonical form and intern as unit
```

**Canonical form for cyclic SCC**:

Represent the SCC as a graph structure:
```
SCC {
  nodes: [(type, content_without_refs), ...]
  edges: [(from_idx, label, to_idx), ...]
  root: idx
}
```

Two SCCs are equal if their graph representations are isomorphic.

**Challenge**: Graph isomorphism is computationally expensive (GI-complete). However, for practical Nix values:
- SCCs are typically small
- We can use heuristics (degree sequence, type sequence) to quickly reject non-isomorphic graphs
- For small graphs, brute-force isomorphism is acceptable

### Approach 3: Hash Consing with Cycle Detection

**Concept**: Classic hash-consing extended with explicit cycle handling.

**Data structures**:

```cpp
// During interning of a single root value:
thread_local std::unordered_map<Value*, InternState> internState;

enum class InternState {
    NotVisited,
    InProgress,   // Currently being interned (cycle detection)
    Complete      // Interning finished
};

// For cycle representation:
struct CyclicRef {
    Value* target;  // Points to a value with InProgress state
    int depth;      // Depth from current position to target
};
```

**Algorithm**:

```cpp
Value* internValueWithCycleDetection(Value& v, int depth = 0) {
    auto it = internState.find(&v);
    if (it != internState.end()) {
        if (it->second == InternState::Complete) {
            return getInternedValue(&v);
        }
        if (it->second == InternState::InProgress) {
            // Cycle detected! Record back-reference
            return createCyclicRef(&v, depth);
        }
    }

    internState[&v] = InternState::InProgress;

    // Intern based on type, recursing into children
    Value* result = internByType(v, depth);

    internState[&v] = InternState::Complete;
    recordInternedValue(&v, result);

    return result;
}
```

**Challenge**: How to handle `createCyclicRef`? The returned value isn't a real `Value*`, it's a placeholder. We need a fixup phase.

**Extended with fixup**:

```cpp
struct InternContext {
    std::unordered_map<Value*, InternState> state;
    std::unordered_map<Value*, Value*> forwarding;
    std::vector<std::pair<Value**, Value*>> fixups;  // (location, target)
};

Value* internValueImpl(Value& v, InternContext& ctx, int depth) {
    // ... cycle detection as above ...

    if (/* cycle detected */) {
        // Allocate placeholder, record fixup needed
        Value* placeholder = allocatePlaceholder();
        ctx.fixups.push_back({&placeholder->payload, &v});
        return placeholder;
    }

    // ... normal interning ...
}

Value* internValue(Value& v) {
    InternContext ctx;
    Value* result = internValueImpl(v, ctx, 0);

    // Apply fixups
    for (auto& [location, target] : ctx.fixups) {
        *location = ctx.forwarding[target];
    }

    return result;
}
```

### Approach 4: Structural Hashing with De Bruijn-like Indexing

**Concept**: Inspired by lambda calculus, use indices instead of pointers for back-references.

**Representation**:

```cpp
struct ValueSignature {
    enum class Tag { Int, Float, Bool, Null, String, List, Attrs, Lambda, BackRef };
    Tag tag;

    union {
        NixInt intVal;
        NixFloat floatVal;
        bool boolVal;
        std::string stringVal;
        std::vector<ValueSignature> listElems;
        std::vector<std::pair<Symbol, ValueSignature>> attrsBindings;
        struct { ExprLambda* fun; /* env signature */ } lambda;
        int backRefDepth;  // For cycles: how many levels up
    };
};
```

**Example**:

```nix
{ a = 1; self = <current>; }
```

Signature:
```
Attrs([
    (a, Int(1)),
    (self, BackRef(0))  // 0 = this attrs itself
])
```

```nix
{ x = { y = <outer>; }; }
```

Signature:
```
Attrs([
    (x, Attrs([
        (y, BackRef(1))  // 1 = one level up = outer attrs
    ]))
])
```

**Interning**:

Two values have the same signature ⟺ they are structurally identical (including cycle structure).

Use signatures as keys in the intern table:
```cpp
std::unordered_map<ValueSignature, Value*, ValueSignatureHash> internTable;
```

**Building signatures**:

```cpp
ValueSignature computeSignature(Value& v,
                                std::vector<Value*>& ancestors) {
    // Check for back-reference
    for (int i = ancestors.size() - 1; i >= 0; i--) {
        if (ancestors[i] == &v) {
            return ValueSignature::backRef(ancestors.size() - 1 - i);
        }
    }

    ancestors.push_back(&v);

    ValueSignature sig;
    switch (v.type()) {
        case nInt:
            sig = ValueSignature::integer(v.integer());
            break;
        case nAttrs:
            std::vector<std::pair<Symbol, ValueSignature>> bindings;
            for (auto& attr : *v.attrs()) {
                bindings.push_back({
                    attr.name,
                    computeSignature(*attr.value, ancestors)
                });
            }
            std::sort(bindings.begin(), bindings.end());  // Canonical order
            sig = ValueSignature::attrs(std::move(bindings));
            break;
        // ... other types ...
    }

    ancestors.pop_back();
    return sig;
}
```

**Reconstructing values from signatures**:

```cpp
Value* signatureToValue(const ValueSignature& sig,
                        std::vector<Value*>& constructed) {
    if (sig.tag == Tag::BackRef) {
        return constructed[constructed.size() - 1 - sig.backRefDepth];
    }

    Value* v = allocValue();
    constructed.push_back(v);  // Register before recursing (for cycles)

    switch (sig.tag) {
        case Tag::Attrs:
            auto builder = buildBindings(sig.attrsBindings.size());
            for (auto& [name, childSig] : sig.attrsBindings) {
                Value* childVal = signatureToValue(childSig, constructed);
                builder.insert(name, childVal);
            }
            v->mkAttrs(builder.finish());
            break;
        // ... other types ...
    }

    constructed.pop_back();
    return v;
}
```

## Recommended Approach

**Approach 4 (Structural Hashing with De Bruijn Indexing)** is recommended because:

1. **Correctness**: De Bruijn indices correctly capture cycle structure
2. **Semantic equality**: Two values with the same signature are semantically equal, regardless of how cycles are formed
3. **No mutation**: Doesn't require modifying existing values or complex fixup phases
4. **Deterministic**: Signature computation is deterministic given canonical child ordering
5. **Debuggable**: Signatures can be printed/compared for debugging

### Implementation Plan

1. **Define `ValueSignature` type** with proper equality and hashing
2. **Implement `computeSignature()`** with ancestor tracking
3. **Implement `signatureToValue()`** with forward reference tracking
4. **Create signature-based intern table**: `std::unordered_map<ValueSignature, Value*>`
5. **Update `internValue()`** to use signature-based approach for lists and attrs
6. **Add statistics** for signature computation and cache hits

### Complexity Analysis

- **Signature computation**: O(n) where n = total nodes in value graph
- **Signature hashing**: O(n)
- **Signature comparison**: O(n) worst case, but often early-exit
- **Value reconstruction**: O(n)

Total: O(n) per interning operation, which matches the recursive approach but handles cycles correctly.

### Memory Considerations

Signatures can be large for complex values. Optimizations:
- **Lazy signature computation**: Only compute when needed for interning
- **Signature interning**: Intern signatures themselves to share substructures
- **Streaming hash**: Compute hash incrementally during signature construction without storing full signature

## Alternative: Hybrid Approach

For performance, consider a hybrid:

1. **Fast path**: For values with no cycles (common case), use simple recursive interning
2. **Slow path**: When cycle detected (via depth limit or explicit check), fall back to signature-based approach

Cycle detection heuristic:
```cpp
Value* internValue(Value& v, int depth = 0) {
    if (depth > MAX_DEPTH) {
        // Likely cycle, use signature-based approach
        return internValueWithSignature(v);
    }

    // Try fast recursive approach
    // If we detect cycle (same pointer seen), switch to signature approach
    // ...
}
```

## Testing Strategy

1. **Unit tests for cycle handling**:
   ```nix
   let x = { a = x; }; in x
   let x = { a = { b = x; }; }; in x
   let x = [x]; in x
   ```

2. **Semantic equality tests**:
   ```nix
   let x = { a = 1; s = x; }; y = { a = 1; s = y; }; in x == y  # true
   ```

3. **Deep nesting without cycles** (should not trigger slow path):
   ```nix
   let f = n: if n == 0 then {} else { child = f (n - 1); }; in f 1000
   ```

4. **Complex mutual recursion**:
   ```nix
   let
     a = { b = b; c = c; };
     b = { a = a; c = c; };
     c = { a = a; b = b; };
   in [a b c]
   ```

## Conclusion

The current failure is due to unbounded recursion when interning cyclic value structures. The recommended solution is **signature-based interning with De Bruijn indices** for cycle representation. This approach correctly handles arbitrary cycles while preserving the semantic equality property required for effective memoization.

Implementation complexity is moderate (estimated 200-400 lines of code), and the approach integrates cleanly with the existing interning infrastructure.
