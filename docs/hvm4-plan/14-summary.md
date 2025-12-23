# Summary: Implementation Priority and Chosen Approaches

## Feature Matrix

| Feature | Priority | CHOSEN Approach | Complexity | Notes |
|---------|----------|-----------------|------------|-------|
| **Lists** | High | Spine-Strict Cons + Cached Length | Low | O(1) length, lazy elements |
| **Attribute Sets** | High | Hybrid Layered (up to 8 layers) | Medium | O(1) for //, O(layers×n) lookup |
| **Strings** | High | #Chr List + Context Wrapper | Medium | HVM4 native encoding |
| **Pattern Lambdas** | High | Desugar to Attr Destructure | Low | Compile-time transform |
| **Arithmetic Primops** | High | Direct OP2 + BigInt Fast Path | Medium | Native ops for 32-bit |
| **Recursive Let** | Medium | Topo-Sort + Y-Combinator | Medium | Fast path for acyclic |
| **Paths** | Medium | Pure String Wrapper | Low | Store ops at boundary |
| **With Expressions** | Medium | Partial Eval + Runtime Lookup | High | Static analysis first |
| **Imports** | Medium | Pre-resolution (Phase 1) | Medium | Resolve at compile-time |
| **Derivations** | Low | Pure records (Phase 1) | Medium | Store writes post-eval |
| **Floats** | Low | Reject (unsupported) | N/A | Fallback to std eval |

## Implementation Order

Based on dependencies and complexity:

1. **Lists** - Foundation for other data structures (attrs, strings use lists)
2. **Arithmetic Primops** - Enables more complex tests, low dependency
3. **Strings** - Required for attribute names and output
4. **Attribute Sets** - Core Nix data structure, depends on lists
5. **Pattern Lambdas** - Depends on attribute sets
6. **Recursive Let** - Depends on attribute sets
7. **Paths** - Depends on strings
8. **With Expressions** - Depends on attribute sets

## Key Design Decisions

### Laziness Preservation
- List length is cached, but elements are lazy thunks
- Attribute keys are strict (Symbol IDs), values are lazy
- String content is strict (Nix semantics)

### Layering Optimization
- Attribute sets use layered approach for O(1) `//` operations
- Maximum 8 layers before flattening
- Matches Nix's internal optimization

### BigInt Encoding
- Small integers (32-bit) use native HVM4 NUM
- Large integers use #Pos{lo, hi} or #Neg{lo, hi}
- Overflow checking at arithmetic boundaries

### Pure Evaluation
- No side effects during HVM4 evaluation
- Derivations are pure records, store writes happen post-eval
- Imports are pre-resolved before HVM4 compilation

## Phase 1 vs Phase 2

### Phase 1 (Current)
- Pre-import resolution (no IFD)
- Pure derivation records (no store writes during eval)
- No float support
- Static import paths only

### Phase 2 (Future)
- Effect-based imports for IFD
- Effect-based derivation creation
- Float handling via external effects
- Dynamic import path resolution
