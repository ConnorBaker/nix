# HVM4 Backend for Nix - Overview

This directory consolidates the HVM4 backend planning docs.

## Source Documents

- `./plan.claude.md` (research notes + implementation plan)
- `./plan-future-work.claude.md` (full future-work plan and options)

## Document Index

### Core Data Types
1. [Attribute Sets](./01-attribute-sets.md)
2. [Lists](./02-lists.md)
3. [Strings](./03-strings.md)
4. [Paths](./04-paths.md)

### Encodings (HVM4)
- [Booleans](./encodings/boolean.hvm4)
- [Integers](./encodings/integer.hvm4)
- [Strings](./encodings/string.hvm4)
- [Lists](./encodings/list.hvm4)
- [Attribute Sets](./encodings/attrs.hvm4)

### Language Features
5. [Recursive Let / Rec](./05-recursive-let.md)
6. [With Expressions](./06-with-expressions.md)
7. [Imports](./07-imports.md)
8. [Derivations](./08-derivations.md)

### Operations
9. [Arithmetic Primops](./09-arithmetic-primops.md)
10. [Other Primops](./10-other-primops.md)
11. [Floats](./11-floats.md)
12. [Pattern-Matching Lambdas](./12-pattern-matching-lambdas.md)

### Reference
- [Debugging and Troubleshooting](./13-debugging.md)
- [Summary and Priority](./14-summary.md)
- [Error Handling Strategy](./15-error-handling.md)
- [HVM4 Patterns Reference](./16-appendix-patterns.md)
- [Integration Tests](./17-appendix-integration-tests.md)
- [Translation Examples](./18-appendix-translation-examples.md)
- [Stress Tests](./19-appendix-stress-tests.md)
- [Glossary](./20-appendix-glossary.md)

---

## Critical Semantic Constraints

**These Nix semantics MUST be preserved in the HVM4 backend. Violating these will cause incorrect evaluation results.**

### Strictness Requirements

| Data Type | Keys/Length | Values/Elements |
|-----------|-------------|-----------------|
| **Attribute Sets** | **STRICT** - keys are Symbol IDs, forced at construction | **LAZY** - values are thunks until accessed |
| **Lists** | **STRICT** - length is known at construction (O(1)) | **LAZY** - elements are thunks until accessed |
| **Strings** | **STRICT** - content is fully evaluated | Context not implemented (tracked externally in Nix) |

### Key Implications

1. **Attribute Sets**: When you write `{ a = 1; b = throw "x"; }`, the keys `a` and `b` are immediately known, but evaluating `.b` will throw. The `//` operator must preserve this: `{ a = 1; } // { b = throw "x"; }` should not throw until `.b` is accessed.

2. **Lists**: `builtins.length [1 (throw "x") 3]` must return `3` without throwing. Elements are only evaluated when accessed via `builtins.elemAt` or iteration.

3. **Strings**: String content is always strict. `"hello ${throw "x"}"` would throw during construction (but variable interpolation currently falls back to the standard evaluator).

---

## HVM4 Encoding Strategy

> Status (2025-12-28): This overview reflects the current encoding choices in
> `src/libexpr/hvm4/`. Some constructors below are still planned (not yet used).
> String context tracking is not implemented; only fully-constant string concatenations
> are compiled by the HVM4 backend today.

All encodings use HVM4 constructors (C00-C16) with base-64 encoded names:

| Constructor | Encoding | Arity | Purpose |
|-------------|----------|-------|---------|
| `#Nil` | 166118 | 0 | Empty list |
| `#Con` | 121448 | 2 | Cons cell (head, tail) |
| `#Lst` | CTR_LST | 2 | List wrapper (length, spine) |
| `#Ats` | CTR_ATS | 1 | Attrset wrapper (spine) |
| `#Atr` | CTR_ATR | 2 | Attr node (key_id, value) |
| `#Str` | CTR_STR | 1 | String table id |
| `#SCat` | CTR_SCAT | 2 | String concatenation |
| `#SNum` | CTR_SNUM | 1 | Int-to-string wrapper |
| `#Pth` | CTR_PTH | 2 | Path (accessor_id, path_string_id) |
| `#Som` | CTR_SOM | 1 | Option some (value) |
| `#Non` | CTR_NON | 0 | Option none |
| `#Nul` | NIX_NULL | 0 | Null value |
| `#Pos` | BIGINT_POS | 2 | BigInt positive (lo, hi) |
| `#Neg` | BIGINT_NEG | 2 | BigInt negative (lo, hi) |
| `#Err` | TBD | 1 | Error value (planned) |
| `#Opq` | TBD | 1 | Opaque context element (planned) |
| `#Drv` | TBD | 1 | Derivation context element (planned) |
| `#Blt` | TBD | 2 | Built output context element (planned) |

**Note:** booleans are represented as NUM (0/1), not constructors.

## HVM4 Operator Reference

| Operator | Code | Description | Nix Equivalent |
|----------|------|-------------|----------------|
| `OP_ADD` | 0 | Addition | `+` (integers) |
| `OP_SUB` | 1 | Subtraction | `-` |
| `OP_MUL` | 2 | Multiplication | `*` |
| `OP_DIV` | 3 | Integer division | `/` |
| `OP_MOD` | 4 | Modulo | N/A (use primop) |
| `OP_AND` | 5 | Bitwise AND | N/A |
| `OP_OR` | 6 | Bitwise OR | N/A |
| `OP_XOR` | 7 | Bitwise XOR | N/A |
| `OP_LSH` | 8 | Left shift | N/A |
| `OP_RSH` | 9 | Right shift | N/A |
| `OP_NOT` | 10 | Bitwise NOT | N/A |
| `OP_EQ` | 11 | Equality | `==` |
| `OP_NE` | 12 | Not equal | `!=` |
| `OP_LT` | 13 | Less than | `<` |
| `OP_LE` | 14 | Less or equal | `<=` |
| `OP_GT` | 15 | Greater than | `>` |
| `OP_GE` | 16 | Greater or equal | `>=` |

**Note**: Logical `&&` and `||` in Nix are short-circuiting and are implemented as conditionals, not OP2.
