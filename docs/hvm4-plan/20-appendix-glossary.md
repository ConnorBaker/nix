# Appendix: Glossary

## HVM4 Terms

| Term | Definition |
|------|------------|
| **Constructor** | Tagged data structure with fixed arity (e.g., `#Con{head, tail}`). Used for encoding all Nix values. |
| **DUP/SUP** | Duplication/Superposition - HVM4's mechanism for optimal sharing and parallel reduction. |
| **Interaction Calculus** | The computational model underlying HVM4, based on symmetric interaction combinators. |
| **LAM/APP** | Lambda abstraction and application - core HVM4 terms for functions. |
| **MAT** | Pattern matching construct that dispatches on constructor tags. |
| **NUM** | Native 32-bit number in HVM4. Larger values use BigInt encoding. |
| **OP2** | Binary operation (arithmetic, comparison, bitwise). |
| **REF** | Reference to a defined function (`@name`). |
| **Term** | Basic unit of computation in HVM4, representing a value or expression. |
| **Thunk** | Unevaluated expression that will be computed on demand (lazy evaluation). |

## Nix Terms (HVM4 Context)

| Term | Definition |
|------|------------|
| **Bindings** | Internal Nix representation of attribute set contents. |
| **Context** | Metadata tracking store path dependencies in strings (for derivation inputs). |
| **Derivation** | Build recipe describing how to produce outputs from inputs. |
| **Symbol/Symbol ID** | Interned string identifier for attribute names. Symbol IDs are integers. |
| **Value** | Evaluated Nix expression result (integer, string, list, attrset, lambda, etc.). |

## Encoding Terms

| Term | Definition |
|------|------------|
| **`#ABs`** | Attrs Base - base layer of an attribute set as a sorted list. |
| **`#ALy`** | Attrs Layer - overlay layer for // optimization. |
| **`#Atr`** | Attribute node storing key_id and value. |
| **`#Chr`** | Character (Unicode codepoint) - HVM4's native string element. |
| **`#Con`** | Cons cell for list spine (head, tail). |
| **`#Ctx`** | Context present - wrapper containing context elements. |
| **`#Drv`** | Derivation context element. |
| **`#Err`** | Error value for propagating evaluation errors. |
| **`#Fls`** | Boolean false. |
| **`#Lst`** | List wrapper with cached length. |
| **`#Neg`** | Negative BigInt (lo, hi words). |
| **`#Nil`** | Empty list terminator. |
| **`#NoC`** | No Context - strings without store path dependencies. |
| **`#Non`** | Option none (no value). |
| **`#Nul`** | Nix null value. |
| **`#Opq`** | Opaque context element. |
| **`#Pos`** | Positive BigInt (lo, hi words). |
| **`#Pth`** | Path value (accessor_id, path_string). |
| **`#Som`** | Option some (has value). |
| **`#Str`** | String with context (chars, context). |
| **`#Tru`** | Boolean true. |

## Implementation Terms

| Term | Definition |
|------|------------|
| **BigInt Encoding** | Two-word (lo, hi) representation for 64-bit integers exceeding 32-bit range. |
| **Layer Flattening** | Collapsing multiple // overlay layers when depth exceeds threshold (8). |
| **Pre-Import Resolution** | Collecting and evaluating all imports before HVM4 compilation. |
| **Pure Derivation Record** | In-memory representation of derivation that defers store writes. |
| **Static Resolution** | Compile-time determination of variable bindings (for `with` expressions). |
| **Topological Sort** | Ordering `rec` bindings by dependency to avoid unnecessary Y-combinator. |
| **Y-Combinator** | Fixed-point combinator for implementing true mutual recursion. |

## Semantic Terms

| Term | Definition |
|------|------------|
| **Lazy** | Evaluation deferred until value is actually needed. |
| **Strict** | Evaluation happens immediately when the expression is encountered. |
| **Spine-Strict** | List structure (cons cells) is evaluated, but elements remain lazy. |
| **Key-Strict** | Attribute set keys (Symbol IDs) are evaluated, but values remain lazy. |
| **Content-Strict** | String characters are fully evaluated (no lazy chars). |

## Operator Codes

| Code | Name | Description |
|------|------|-------------|
| 0 | `OP_ADD` | Addition |
| 1 | `OP_SUB` | Subtraction |
| 2 | `OP_MUL` | Multiplication |
| 3 | `OP_DIV` | Integer division |
| 4 | `OP_MOD` | Modulo |
| 5 | `OP_EQ` | Equality |
| 6 | `OP_NE` | Not equal |
| 7 | `OP_LT` | Less than |
| 8 | `OP_LE` | Less or equal |
| 9 | `OP_GT` | Greater than |
| 10 | `OP_GE` | Greater or equal |
| 11 | `OP_AND` | Bitwise AND |
| 12 | `OP_OR` | Bitwise OR |
| 13 | `OP_XOR` | Bitwise XOR |
