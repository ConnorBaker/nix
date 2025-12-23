# HVM4 Backend Implementation Status

*Last updated: 2025-12-23*

This document describes the current implementation status of the HVM4 evaluator backend for Nix.

## Overview

The HVM4 backend is an alternative evaluator for Nix expressions that compiles Nix AST to HVM4 (Higher-order Virtual Machine) terms for parallel evaluation. When an expression cannot be compiled by HVM4, evaluation falls back to the standard Nix evaluator transparently.

## Architecture

```
Nix Expression (AST)
        │
        ▼
┌─────────────────┐
│  HVM4Compiler   │  Compiles Nix AST to HVM4 terms
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  HVM4Runtime    │  Executes HVM4 terms (calls libhvm4)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ ResultExtractor │  Converts HVM4 results back to Nix Values
└─────────────────┘
```

## File Organization

| File | Purpose |
|------|---------|
| `hvm4-backend.cc` | Main entry point, `HVM4Backend` class |
| `hvm4-compiler.cc` | Core compiler, `emit()` dispatch |
| `hvm4-compiler-analysis.cc` | `canCompileWithScope()`, usage counting, dependency analysis |
| `hvm4-compiler-emit-basic.cc` | Emitters for Int, Float, String, Path, Var, List |
| `hvm4-compiler-emit-lambda.cc` | Lambda and pattern-matching lambda emitters |
| `hvm4-compiler-emit-control.cc` | If, Let, With, Assert, Call emitters |
| `hvm4-compiler-emit-ops.cc` | Operator emitters (comparison, boolean, etc.) |
| `hvm4-compiler-emit-attrs.cc` | Attribute set emitters (including recursive) |
| `hvm4-compile-context.cc` | `CompileContext` for tracking bindings during compilation |
| `hvm4-runtime.cc` | Wrapper around libhvm4 C API |
| `hvm4-result.cc` | `ResultExtractor` for converting HVM4 terms to Nix Values |
| `hvm4-bigint.cc` | 64-bit integer and float encoding/decoding |
| `hvm4-list.cc` | List encoding (`#Lst{len, spine}`) |
| `hvm4-string.cc` | String encoding (`#Str{id}`, `#SCat{l,r}`) |
| `hvm4-attrs.cc` | Attribute set encoding (`#Ats{spine}`) |
| `hvm4-path.cc` | Path encoding (`#Pth{accessor, path}`) |

## Data Type Encodings

HVM4 provides native 32-bit integers (`NUM`) and constructors (`C00`-`C16`). All other Nix types are encoded using constructors:

| Nix Type | HVM4 Encoding | Notes |
|----------|---------------|-------|
| Integer (small) | `NUM` | Native 32-bit signed |
| Integer (large) | `#Pos{lo, hi}` / `#Neg{lo, hi}` | 64-bit via two 32-bit words |
| Float | `#Flt{lo, hi}` | IEEE 754 double split into two words |
| Boolean | `NUM` | 0 = false, non-zero = true |
| Null | `#Nul{}` | Arity-0 constructor |
| String | `#Str{id}` | String table lookup |
| String (concat) | `#SCat{left, right}` | Lazy concatenation |
| String (from int) | `#SNum{value}` | Runtime int-to-string |
| List | `#Lst{length, spine}` | O(1) length |
| List spine | `#Con{head, tail}` / `#Nil{}` | Standard cons list |
| Attrs | `#Ats{spine}` | Sorted list of `#Atr{key, value}` |
| Path | `#Pth{accessor_id, path_string_id}` | References to registries |

## Implemented Features

### Literals and Basic Types
- [x] Integer literals (32-bit and 64-bit)
- [x] Float literals
- [x] String literals
- [x] Path literals
- [x] Null literal
- [x] Boolean literals (`true`, `false`)

### Arithmetic Operations
- [x] Addition (`+`) - integers only, 32-bit
- [x] Subtraction (`-`) - integers only, 32-bit
- [x] Multiplication (`*`) - integers only, 32-bit
- [x] Division (`/`) - integers only, 32-bit
- [ ] Float arithmetic (falls back)

### Comparison Operations
- [x] Less than (`<`) - integers, including BigInt via MAT
- [x] Less than or equal (`<=`)
- [x] Greater than (`>`)
- [x] Greater than or equal (`>=`)
- [x] Equality (`==`) - all types via EQL
- [x] Inequality (`!=`)
- [ ] Float comparisons (falls back)

### Boolean Operations
- [x] Logical AND (`&&`)
- [x] Logical OR (`||`)
- [x] Logical NOT (`!`)
- [x] Implication (`->`)

### Control Flow
- [x] If-then-else
- [x] Assert expressions

### Bindings
- [x] Let bindings (non-recursive)
- [x] Let bindings (acyclic recursive)
- [x] With expressions (basic)
- [x] With expressions (nested) - partial, outer scope access limited

### Functions
- [x] Simple lambdas (`x: body`)
- [x] Pattern-matching lambdas (`{ a, b }: body`)
- [x] Default arguments (`{ a, b ? 0 }: body`)
- [x] Ellipsis (`{ a, ... }: body`)
- [x] @-patterns (`{ a, b } @ args: body`)
- [x] Function application
- [x] Closures
- [x] Multi-argument functions (curried)

### Lists
- [x] List literals (`[1 2 3]`)
- [x] List concatenation (`++`)
- [ ] `builtins.head`, `builtins.tail`, etc. (falls back)

### Strings
- [x] String literals (`"hello"`)
- [x] Constant string concatenation (`"a" + "b"`)
- [x] String interpolation with constants (`"${x}"` where x is string)
- [ ] String interpolation with `toString` (falls back)
- [ ] Path-to-string coercion (falls back)

### Attribute Sets
- [x] Attribute set literals (`{ a = 1; b = 2; }`)
- [x] Attribute access (`.`)
- [x] Has-attribute (`?`)
- [x] Attribute update (`//`)
- [x] Recursive attribute sets (acyclic `rec { }`)
- [x] Inherit (`inherit a b;`)
- [x] Inherit from (`inherit (x) a b;`)
- [ ] Dynamic attributes (falls back)
- [ ] `builtins.attrNames`, etc. (falls back)

## Known Limitations

### BigInt Arithmetic Overflow
HVM4's arithmetic operators (`OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`) operate on 32-bit values. Operations on 64-bit BigInt constructors produce incorrect results:
```nix
2147483647 + 1  # Overflows to -2147483648
65536 * 65536   # Overflows to 0
```
BigInt comparisons work correctly via MAT pattern matching.

### Float Operations
Float literals are supported, but float arithmetic and comparisons are not:
```nix
3.14      # Works - returns 3.14
1.0 + 2.0 # Falls back to standard evaluator
1.0 < 2.0 # Falls back to standard evaluator
```

### Division by Zero
HVM4 does not detect division by zero. Instead of throwing an error, it produces undefined results.

### Nested With Expressions
Basic `with` works, but accessing attributes from outer `with` scopes in nested `with` expressions may fail:
```nix
with { a = 1; }; a                    # Works
with { a = 1; }; with { b = 2; }; b   # Works
with { a = 1; }; with { b = 2; }; a   # May fail (outer scope)
```

### Cyclic Recursive Attributes
Only acyclic recursive attribute sets are supported:
```nix
rec { a = 1; b = a + 1; }  # Works (acyclic)
rec { a = b; b = a; }      # Falls back (cyclic)
```

## Not Implemented (Falls Back)

- **Imports**: `import ./file.nix`
- **Builtins**: All `builtins.*` functions
- **Derivations**: `derivation { ... }`
- **String context**: Store path tracking
- **Thunks returning to Nix**: HVM4 results must be fully evaluated

## Test Coverage

The HVM4 backend has comprehensive test coverage in `src/libexpr-tests/hvm4/`:

| Test File | Coverage |
|-----------|----------|
| `hvm4-capability-test.cc` | Basic capability checks |
| `hvm4-arithmetic-test.cc` | Integer arithmetic |
| `hvm4-comparison-test.cc` | Comparison operators |
| `hvm4-boolean-test.cc` | Boolean operations |
| `hvm4-lambda-test.cc` | Lambdas and closures |
| `hvm4-let-test.cc` | Let bindings |
| `hvm4-list-test.cc` | List operations |
| `hvm4-string-test.cc` | String operations |
| `hvm4-attrs-test.cc` | Attribute sets |
| `hvm4-float-test.cc` | Float literals |
| `hvm4-null-test.cc` | Null handling |
| `hvm4-path-test.cc` | Path handling |
| `hvm4-pattern-lambda-test.cc` | Pattern matching |
| `hvm4-with-test.cc` | With expressions |
| `hvm4-known-limitations-test.cc` | Documented limitations |
| `hvm4-runtime-test.cc` | Low-level runtime tests |

**Total: 1102 HVM4 tests passing**

## Performance Notes

The HVM4 backend is designed for parallel evaluation of pure computations. Performance benefits are most visible for:
- Large recursive computations
- Map/fold operations over lists (when builtins are implemented)
- Parallel attribute set construction

For simple expressions, the compilation overhead may exceed evaluation time. The backend automatically falls back for unsupported expressions.

## Future Work

Priority items for expanding HVM4 coverage:
1. **Builtins**: `map`, `filter`, `foldl'`, `length`, `head`, `tail`
2. **Float arithmetic**: Implement via custom HVM4 functions
3. **BigInt arithmetic**: Multi-word arithmetic operations
4. **Imports**: Static import resolution
5. **Derivations**: Pure derivation record construction

## References

- [HVM4 Repository](https://github.com/HigherOrderCO/HVM)
- Plan file: `~/.claude/plans/velvety-mapping-dawn.md`
- Test common utilities: `src/libexpr-tests/hvm4/hvm4-test-common.hh`
