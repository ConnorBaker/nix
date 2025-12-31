# Summary: Implementation Priority and Chosen Approaches

## Feature Matrix (Current Status)

| Feature | Status | Current Approach | Notes |
|---------|--------|------------------|-------|
| **Lists** | Implemented (basic) | `#Lst{len, spine}` | `++` only for list literals; list primops missing |
| **Attribute Sets** | Implemented (basic) | `#Ats{spine}` sorted list | No layering; O(n) lookup; dynamic keys unsupported |
| **Strings** | Implemented (basic) | String table + `#SCat/#SNum` | No context tracking; only constant concat compiled |
| **Pattern Lambdas** | Implemented (partial) | Desugar to attr lookups | No extra-attr check; missing attrs yield ERA |
| **Arithmetic Primops** | Partial | OP2 for `__sub/__mul/__div`; BigInt compare for `__lessThan` | No overflow handling |
| **Recursive Let** | Partial | Topo-sort acyclic `rec` | Cycles rejected; `let rec` unsupported |
| **Paths** | Partial | `#Pth{accessor_id, path_string_id}` | No coercion or store interaction |
| **With Expressions** | Partial | Innermost lookup only | No outer fallback |
| **Imports** | Not implemented | - | Falls back |
| **Derivations** | Not implemented | - | Falls back |
| **Floats** | Partial | `#Flt{lo, hi}` literals | Arithmetic/comparison falls back |

## Implementation Order (Completed vs Planned)

Completed in some form: Lists, Attrs, Strings, Pattern Lambdas, Paths, basic ops.
Planned work remains for imports, derivations, full primops, error handling, and
proper `with`/`rec` semantics.

## Key Design Decisions

### Laziness Preservation
- List length is cached, but elements are lazy thunks
- Attribute keys are strict (Symbol IDs), values are lazy
- String content is strict (Nix semantics)

### Layering Optimization
- Not implemented (attrsets are merged eagerly with O(n+m) cost)

### BigInt Encoding
- Small integers (32-bit) use native HVM4 NUM
- Large integers use #Pos{lo, hi} or #Neg{lo, hi}
- Overflow checking is not implemented for add/sub/mul

### Pure Evaluation
- HVM4 evaluation is pure, but derivations/imports are not implemented yet

## Phase 1 vs Phase 2

### Phase 1 (Current)
- No import/derivation handling in HVM4 yet (fallback)
- Float literals supported; arithmetic/comparison fall back

### Phase 2 (Future)
- Effect-based imports for IFD
- Effect-based derivation creation
- Float handling via external effects
- Dynamic import path resolution
