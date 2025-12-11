# HVM-Based Nix Evaluator Investigation

This document investigates how the Nix evaluator (libexpr) would need to be rewritten to use HVM's C implementation as its evaluation substrate.

## Executive Summary

Rewriting the Nix evaluator to use HVM is a substantial undertaking that would require fundamental architectural changes. While HVM offers potential benefits like automatic parallelization and optimal beta-reduction, the mismatch between Nix's lazy attribute set semantics and HVM's interaction net model presents significant challenges.

---

## 1. Understanding the Current Systems

### 1.1 Nix Evaluator (libexpr) Architecture

The Nix evaluator (`src/libexpr/`) is a tree-walking interpreter with lazy evaluation:

**Core Components:**
- **`eval.cc`**: Main evaluation logic (~3000 lines)
- **`value.hh`**: Value representation using a discriminated union (`InternalType`)
- **`nixexpr.hh`**: AST node definitions for Nix expressions
- **`primops.cc`**: Built-in functions (derivation, import, map, etc.)
- **`eval-inline.hh`**: Hot path functions like `forceValue()`

**Evaluation Model:**
```
Expression AST → Thunks → Forced Values
                   ↑
              Lazy Evaluation
```

**Key Characteristics:**
1. **Lazy evaluation**: Thunks are created for deferred computation
2. **Call-by-need**: Values are forced only when accessed
3. **Memoization**: Forced thunks are updated in-place (blackholing)
4. **Attribute sets**: Efficient O(log n) lookup with sorted bindings
5. **Boehm GC**: Garbage collection for all values

### 1.2 HVM C Implementation Architecture

HVM (`src/hvm.c`, `src/run.c`) is an interaction net evaluator:

**Core Data Structures:**
```c
typedef u32 Port;  // Tag (3 bits) + Val (29 bits)
typedef u64 Pair;  // Two ports packed together
typedef struct Net {
    APair node_buf[G_NODE_LEN];  // Global node buffer
    APort vars_buf[G_VARS_LEN];  // Global vars buffer (wires)
    APair rbag_buf[G_RBAG_LEN];  // Redex bag (work queue)
    a64 itrs;                     // Interaction count
    a32 idle;                     // Idle thread counter
} Net;
```

**Node Types (Tags):**
- `VAR` (0x0): Variable (wire endpoint)
- `REF` (0x1): Reference to a definition
- `ERA` (0x2): Eraser (for garbage collection)
- `NUM` (0x3): 24-bit number
- `CON` (0x4): Constructor (lambda abstraction)
- `DUP` (0x5): Duplicator (for copying)
- `OPR` (0x6): Operator (numeric operations)
- `SWI` (0x7): Switch (pattern matching on numbers)

**Evaluation Model:**
```
HVM Source → Interaction Net → Normalize (parallel interactions) → Result
```

**Key Characteristics:**
1. **Optimal sharing**: Implements Lamping's optimal reduction
2. **Parallel by default**: All active pairs can reduce in parallel
3. **Lock-free**: Atomic operations for thread safety
4. **Strong confluence**: Order of reductions doesn't matter
5. **Explicit duplication**: DUP nodes handle copying

---

## 2. Mapping Nix Constructs to HVM Primitives

### 2.1 Values

| Nix Type | HVM Encoding | Notes |
|----------|--------------|-------|
| `Int` | `NUM` node (24-bit limitation!) | HVM only supports 24-bit integers |
| `Float` | `NUM` with F24 encoding | Precision loss (24-bit mantissa) |
| `Bool` | Scott encoding or `NUM` (0/1) | `true = λt.λf.t`, `false = λt.λf.f` |
| `Null` | Scott encoding | `null = λn.n` |
| `String` | Constructor list of bytes | Very inefficient for large strings |
| `Path` | Constructor list + accessor ref | Complex encoding needed |
| `List` | Scott-encoded cons list | `[a,b,c] = cons a (cons b (cons c nil))` |
| `Attrs` | **MAJOR CHALLENGE** | See section 2.2 |
| `Lambda` | `CON` node (constructor) | Direct mapping |
| `Thunk` | Deferred net / `REF` node | Needs explicit handling |

### 2.2 Attribute Sets: The Critical Challenge

Nix's attribute sets are fundamental and heavily optimized:
- O(log n) lookup using sorted bindings
- Lazy evaluation of attribute values
- Support for `rec` (recursive attribute sets)
- Dynamic attribute names

**HVM has no native associative data structure.** Options:

**Option A: Association Lists**
```hvm
@attrs = (("name" value1) (("age" value2) nil))
```
- Pros: Simple encoding
- Cons: O(n) lookup - unacceptable for nixpkgs (thousands of packages)

**Option B: Binary Search Trees**
```hvm
@attrs = (Node "key" value left right)
```
- Pros: O(log n) lookup
- Cons: Complex to encode, tree operations need helper functions

**Option C: Hash Array Mapped Tries (HAMT)**
- Pros: Near O(1) lookup, functional/persistent
- Cons: Extremely complex to encode in HVM

**Option D: External Attribute Set Handling**
- Keep attribute sets in C, use FFI for lookups
- Breaks HVM's parallelism model

### 2.3 Functions and Application

| Nix Construct | HVM Encoding |
|---------------|--------------|
| `x: body` | `CON` node: `(body x)` |
| `f arg` | Application: `f ~ arg` redex |
| `{ x, y }: body` | Destructuring: Extract from attrset |
| `{ x ? default }` | Default args: Conditional selection |

### 2.4 Lazy Evaluation

Nix's lazy evaluation maps to HVM as follows:

**Nix Thunks → HVM REF nodes:**
```nix
let x = expensiveComputation; in ...
```
Becomes:
```hvm
@thunk_x = ... # Only expanded when needed
```

HVM's `REF` nodes provide call-by-need semantics through the CALL interaction rule.

### 2.5 Recursion

**Let bindings:**
```nix
let x = e1; in e2
```
Maps to lambda application and REF definitions.

**Recursive attrs (`rec`):**
```nix
rec { x = y + 1; y = 5; }
```
Requires careful handling of self-references through HVM variables.

---

## 3. Architectural Changes Required

### 3.1 New Components to Build

1. **Nix → HVM Compiler**
   - Parse Nix AST (`nixexpr.hh`)
   - Generate HVM interaction net representation
   - Handle all Nix constructs (let, with, inherit, etc.)

2. **HVM Runtime Integration**
   - Replace `EvalState` evaluation loop with HVM's `normalize()`
   - Manage HVM's `Net` and `Book` structures
   - Handle HVM's fixed memory allocation model

3. **Primop FFI Layer**
   - Implement Nix primops as HVM foreign functions
   - Handle effects (file I/O, derivations, import)
   - Convert between HVM and Nix representations

4. **Attribute Set System**
   - Implement efficient attribute sets (likely hybrid approach)
   - Handle dynamic attribute names
   - Support attribute selection (`a.b.c`)

5. **String Handling**
   - Implement string operations efficiently
   - Handle string contexts (for derivation dependencies)
   - Support string interpolation

6. **Error Handling**
   - Map HVM evaluation errors to Nix errors
   - Preserve source positions for error messages
   - Handle infinite recursion (blackhole detection)

### 3.2 Components to Replace

| Current Component | Replacement Strategy |
|-------------------|---------------------|
| `forceValue()` | HVM `normalize()` + result extraction |
| `callFunction()` | HVM redex creation + normalization |
| `Value` union | HVM `Port` + readback functions |
| `Env` (environments) | HVM scoping via `CON`/`VAR` |
| `Bindings` (attrs) | New hybrid attr system |
| Boehm GC | HVM's implicit GC via ERA nodes |

### 3.3 Components to Keep/Adapt

- **Parser** (`parser.y`, `lexer.l`): Keep, modify output to HVM nets
- **Symbol table**: Keep for identifier interning
- **Position tracking**: Keep for error messages
- **Settings/configuration**: Keep
- **Store integration**: Keep, adapt interface

---

## 4. Technical Challenges

### 4.1 Integer Precision

**Problem:** HVM uses 24-bit integers, Nix uses 64-bit.

**Solutions:**
1. Pair of HVM numbers for 48-bit integers
2. External bignum handling via FFI
3. Accept precision limitations (likely unacceptable)

### 4.2 String Context

**Problem:** Nix strings carry context for derivation dependencies.

**Solution:** Encode context as separate attribute/structure, propagate through string operations.

### 4.3 Imperative Primops

**Problem:** Many Nix primops have side effects (import, readFile, derivation).

**Solution:** Use HVM's IO monad pattern (seen in `run.c`):
```c
Port io_read(Net* net, Book* book, Port argm);
Port io_open(Net* net, Book* book, Port argm);
```

### 4.4 Debugging and Error Messages

**Problem:** HVM's interaction nets lose source position information.

**Solution:**
- Maintain position table separate from HVM net
- Embed position references in REF node names
- Custom error extraction from HVM state

### 4.5 Memory Model

**Problem:** HVM has fixed-size memory pools.

```c
#define G_NODE_LEN (1ul << 29)  // max 536m nodes
#define G_VARS_LEN (1ul << 29)  // max 536m vars
```

**Solution:**
- Dynamic pool resizing (requires HVM modifications)
- Incremental evaluation for large expressions
- Memory pressure monitoring

---

## 5. Potential Benefits

### 5.1 Automatic Parallelization
- HVM parallelizes independent computations automatically
- Could speed up evaluation of large package sets
- Lock-free execution model

### 5.2 Optimal Sharing
- HVM implements optimal beta-reduction
- Avoids exponential blowups in certain cases
- Efficient handling of `let` bindings

### 5.3 GPU Potential
- HVM has CUDA backend
- Could offload computation-heavy evaluations
- Massive parallelism for suitable workloads

---

## 6. Estimated Complexity

### 6.1 Core Rewrite Effort

| Component | Estimated Effort | Risk |
|-----------|------------------|------|
| Nix→HVM compiler | Very High | Medium |
| Attribute set system | Very High | High |
| Primop implementation | High | Medium |
| String handling | Medium | Low |
| Type conversions | Medium | Low |
| Error handling | Medium | Medium |
| Integration testing | Very High | High |

### 6.2 Risk Assessment

**High Risks:**
- Attribute set performance: Make-or-break for nixpkgs
- 24-bit integer limitation: May require HVM modifications
- String context propagation: Complex semantic preservation

**Medium Risks:**
- Primop compatibility: Some may need redesign
- Memory usage: HVM's fixed pools vs. Nix's dynamic allocation
- Error message quality: Maintaining good UX

**Low Risks:**
- Basic lambda calculus: Direct HVM mapping
- Lazy evaluation: HVM REF nodes work well
- List operations: Straightforward encoding

---

## 7. Recommended Approach

### Phase 1: Proof of Concept
1. Implement minimal Nix subset (no attrs, basic types)
2. Benchmark against current evaluator
3. Validate parallelization benefits

### Phase 2: Attribute Set Design
1. Design and benchmark attr encoding options
2. Possibly modify HVM for native assoc structures
3. Ensure nixpkgs-scale performance

### Phase 3: Full Implementation
1. Complete type system
2. Implement all primops
3. Handle all edge cases

### Phase 4: Optimization
1. Profile and optimize hot paths
2. Tune HVM parameters
3. Explore GPU acceleration

---

## 8. Conclusion

Rewriting the Nix evaluator to use HVM is technically feasible but presents significant challenges. The primary obstacle is efficient attribute set handling - Nix's fundamental data structure has no efficient mapping to HVM's interaction net model.

A practical path forward might involve:
1. Using HVM for the lambda calculus core (functions, applications)
2. Keeping native C implementations for attribute sets and strings
3. Using FFI bridges between the two systems

This hybrid approach would capture some parallelization benefits while avoiding the performance pitfalls of encoding complex data structures in pure interaction nets.

---

## Appendix A: Key File Locations

### Nix Evaluator
- `src/libexpr/eval.cc` - Main evaluation logic
- `src/libexpr/eval-inline.hh` - Hot path functions
- `src/libexpr/include/nix/expr/value.hh` - Value types
- `src/libexpr/include/nix/expr/nixexpr.hh` - AST definitions
- `src/libexpr/primops.cc` - Built-in functions

### HVM
- `src/hvm.c` - Core interpreter and data structures
- `src/hvm.h` - Public API
- `src/run.c` - IO operations and readback
- `paper/HVM2.typst` - Formal specification

## Appendix B: HVM Interaction Rules

| Rule | When | Effect |
|------|------|--------|
| LINK | VAR ~ any | Substitute variable |
| CALL | REF ~ non-VAR | Expand definition |
| VOID | nullary ~ nullary | Erase both |
| ERAS | nullary ~ binary | Propagate erasure |
| ANNI | same-type binary | Annihilate (beta) |
| COMM | diff-type binary | Commute (copy) |
| OPER | NUM ~ OPR | Numeric operation |
| SWIT | NUM ~ SWI | Pattern match |
