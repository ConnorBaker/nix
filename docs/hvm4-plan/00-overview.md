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
| **Strings** | **STRICT** - content is fully evaluated | Context is a strict set |

### Key Implications

1. **Attribute Sets**: When you write `{ a = 1; b = throw "x"; }`, the keys `a` and `b` are immediately known, but evaluating `.b` will throw. The `//` operator must preserve this: `{ a = 1; } // { b = throw "x"; }` should not throw until `.b` is accessed.

2. **Lists**: `builtins.length [1 (throw "x") 3]` must return `3` without throwing. Elements are only evaluated when accessed via `builtins.elemAt` or iteration.

3. **Strings**: String content is always strict. `"hello ${throw "x"}"` will throw during construction, not when the string is used.

---

## HVM4 Encoding Strategy

All encodings use HVM4 constructors (C00-C16) with base-64 encoded names:

| Constructor | Encoding | Arity | Purpose |
|-------------|----------|-------|---------|
| `#Nil` | 166118 | 0 | Empty list |
| `#Con` | 121448 | 2 | Cons cell (head, tail) |
| `#Lst` | TBD | 2 | List wrapper (length, spine) |
| `#ABs` | TBD | 1 | Attrs base (sorted_list) |
| `#ALy` | TBD | 2 | Attrs layer (overlay, base) |
| `#Atr` | TBD | 2 | Attr node (key_id, value) |
| `#Str` | TBD | 2 | String (chars, context) |
| `#Chr` | Native | 1 | Character (codepoint) |
| `#Pth` | TBD | 2 | Path (accessor_id, path_string) |
| `#NoC` | TBD | 0 | No context (strings) |
| `#Ctx` | TBD | 1 | Context present (elements) |
| `#Pos` | Existing | 2 | BigInt positive (lo, hi) |
| `#Neg` | Existing | 2 | BigInt negative (lo, hi) |
| `#Tru` | TBD | 0 | Boolean true |
| `#Fls` | TBD | 0 | Boolean false |
| `#Som` | TBD | 1 | Option some (value) |
| `#Non` | TBD | 0 | Option none |
| `#Err` | TBD | 1 | Error value (message) |
| `#Opq` | TBD | 1 | Opaque context element |
| `#Drv` | TBD | 1 | Derivation context element |
| `#Blt` | TBD | 2 | Built output context element |
| `#Nul` | TBD | 0 | Null value |
| `#Imp` | TBD | 2 | Import result (path, value) |
| `#Lam` | TBD | 2 | Lambda (arg_pattern, body) |
| `#App` | TBD | 2 | Application (func, arg) |

## HVM4 Operator Reference

| Operator | Code | Description | Nix Equivalent |
|----------|------|-------------|----------------|
| `OP_ADD` | 0 | Addition | `+` (integers) |
| `OP_SUB` | 1 | Subtraction | `-` |
| `OP_MUL` | 2 | Multiplication | `*` |
| `OP_DIV` | 3 | Integer division | `/` |
| `OP_MOD` | 4 | Modulo | N/A (use primop) |
| `OP_EQ` | 5 | Equality | `==` |
| `OP_NE` | 6 | Not equal | `!=` |
| `OP_LT` | 7 | Less than | `<` |
| `OP_LE` | 8 | Less or equal | `<=` |
| `OP_GT` | 9 | Greater than | `>` |
| `OP_GE` | 10 | Greater or equal | `>=` |
| `OP_AND` | 11 | Bitwise AND | N/A |
| `OP_OR` | 12 | Bitwise OR | N/A |
| `OP_XOR` | 13 | Bitwise XOR | N/A |

**Note**: Logical `&&` and `||` in Nix are short-circuiting and are implemented as conditionals, not OP2.
