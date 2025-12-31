# HVM4 Backend for Nix - Future Work Implementation Options

> Note: This is the source document. The consolidated, split plan lives under `docs/hvm4-plan` starting at [00-overview.md](./00-overview.md).
>
> Status (2025-12-28): This is an archival planning document and does not reflect
> the current HVM4 backend implementation. See `docs/hvm4-plan/plan.claude.md`
> and `src/libexpr/hvm4/STATUS.md` for the current state.

This document details multiple implementation approaches for features not included in the minimal prototype, along with design tradeoffs for each approach.

---

# Table of Contents

## Core Data Types
1. [Attribute Sets](#1-attribute-sets)
2. [Lists](#2-lists)
3. [Strings](#3-strings)
4. [Paths](#4-paths)

## Language Features
5. [Recursive Let / Rec](#5-recursive-let--rec)
6. [With Expressions](#6-with-expressions)
7. [Imports](#7-imports)
8. [Derivations](#8-derivations)

## Operations
9. [Arithmetic Primops](#9-arithmetic-primops)
10. [Other Primops](#10-other-primops)
11. [Floats](#11-floats)
12. [Pattern-Matching Lambdas](#12-pattern-matching-lambdas)

## Reference
- [Debugging and Troubleshooting](#debugging-and-troubleshooting)
- [Summary: Implementation Priority and Chosen Approaches](#summary-implementation-priority-and-chosen-approaches)
- [Error Handling Strategy](#error-handling-strategy)
- [Appendix: HVM4 Patterns Reference](#appendix-hvm4-patterns-reference)
- [Appendix: Integration Tests](#appendix-integration-tests)
- [Appendix: Nix-to-HVM4 Translation Examples](#appendix-nix-to-hvm4-translation-examples)
- [Appendix: Stress Tests](#appendix-stress-tests)
- [Appendix: Glossary](#appendix-glossary)

---

# Critical Semantic Constraints

**These Nix semantics MUST be preserved in the HVM4 backend. Violating these will cause incorrect evaluation results.**

## Strictness Requirements

| Data Type | Keys/Length | Values/Elements |
|-----------|-------------|-----------------|
| **Attribute Sets** | **STRICT** - keys are Symbol IDs, forced at construction | **LAZY** - values are thunks until accessed |
| **Lists** | **STRICT** - length is known at construction (O(1)) | **LAZY** - elements are thunks until accessed |
| **Strings** | **STRICT** - content is fully evaluated | Context is a strict set |

## Key Implications

1. **Attribute Sets**: When you write `{ a = 1; b = throw "x"; }`, the keys `a` and `b` are immediately known, but evaluating `.b` will throw. The `//` operator must preserve this: `{ a = 1; } // { b = throw "x"; }` should not throw until `.b` is accessed.

2. **Lists**: `builtins.length [1 (throw "x") 3]` must return `3` without throwing. Elements are only evaluated when accessed via `builtins.elemAt` or iteration.

3. **Strings**: String content is always strict. `"hello ${throw "x"}"` will throw during construction, not when the string is used.

> **See also:** [Error Handling Strategy](#error-handling-strategy) for how errors are encoded and propagated in HVM4.

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

---

# 1. Attribute Sets

Nix attribute sets are central to the language. They are **sorted maps from symbols to values** with:
- O(log n) lookup via binary search
- Lazy values (thunks)
- The `//` (update) operator with layering optimization
- Dynamic attribute names (`"${expr}" = value`)

## Nix Implementation Details

```cpp
// Bindings: sorted array with layering for efficient //
class Bindings {
    Attr attrs[0];              // Flexible array, sorted by Symbol
    const Bindings* baseLayer;  // For // optimization (up to 8 layers)
};

struct Attr { Symbol name; Value* value; PosIdx pos; };
```

## Option A: Sorted List Encoding

Encode attribute sets as sorted linked lists of key-value pairs.

```hvm4
// AttrSet = #Empty{} | #Attr{key, value, rest}
// Keys are Symbol IDs (32-bit integers)

@empty_attrs = #Empty{}
@singleton = λkey. λval. #Attr{key, val, #Empty{}}

@lookup = λkey. λ{
  #Empty: #None{}
  #Attr: λk. λv. λrest.
    (key == k) .&. #Some{v} .|. @lookup(key, rest)
}

@insert = λkey. λval. λattrs. λ{
  #Empty: #Attr{key, val, #Empty{}}
  #Attr: λk. λv. λrest.
    (key < k) .&. #Attr{key, val, attrs} .|.
    (key == k) .&. #Attr{key, val, rest} .|.
    #Attr{k, v, @insert(key, val, rest)}
}(attrs)

// Update operator: O(n + m) merge
@update = λbase. λoverlay. ...
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Lookup | O(n) - must traverse list |
| Insert | O(n) - maintain sorted order |
| Update (`//`) | O(n + m) - merge two sorted lists |
| Memory | Efficient - cons cells only |
| Laziness | Natural - values stay as thunks |
| Implementation | Simple |

**Best for:** Small attribute sets, prototyping

## Option B: Binary Search Tree (BST)

Encode as a balanced BST for O(log n) operations.

```hvm4
// BST = #Leaf{} | #Node{key, value, left, right}
// Could use AVL or Red-Black for balance

@lookup_bst = λkey. λ{
  #Leaf: #None{}
  #Node: λk. λv. λl. λr.
    (key == k) .&. #Some{v} .|.
    (key < k) .&. @lookup_bst(key, l) .|.
    @lookup_bst(key, r)
}

// AVL rotation for balance
@rotate_left = λ{#Node: λk. λv. λl. λ{
  #Node: λrk. λrv. λrl. λrr.
    #Node{rk, rv, #Node{k, v, l, rl}, rr}
}}

@insert_avl = λkey. λval. λtree. ...  // With rebalancing
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Lookup | O(log n) with balance |
| Insert | O(log n) with rebalancing |
| Update (`//`) | O(n log n) or O(n) with merge |
| Memory | More nodes than list |
| Complexity | Rebalancing logic needed |
| Laziness | Natural |

**Best for:** Medium-sized attribute sets, frequent lookups

## Option C: Hash Array Mapped Trie (HAMT)

Use a HAMT for near-O(1) operations with structural sharing.

```hvm4
// HAMT with 32-way branching (5 bits per level)
// Node = #Leaf{key, value} | #Branch{bitmap, children}
// bitmap: 32 bits indicating which children exist
// children: array of child nodes (sparse)

// Would need to encode bitmap and sparse array
// Complex but very efficient for large sets
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Lookup | O(log32 n) ≈ O(1) for practical sizes |
| Insert | O(log32 n) with structural sharing |
| Update (`//`) | O(m log32 n) |
| Memory | Excellent sharing |
| Complexity | High - bitmap manipulation |
| HVM4 fit | Poor - needs efficient arrays |

**Best for:** Large attribute sets, many updates

## Option D: Hybrid / Layered Approach

Mirror Nix's layering optimization: small overlays reference base sets.

```hvm4
// Layered = #Base{sorted_list} | #Layer{overlay, base}
// overlay is small sorted list, base is another Layered

@lookup_layered = λkey. λ{
  #Base: λlist. @lookup_list(key, list)
  #Layer: λoverlay. λbase.
    @lookup_list(key, overlay) .or. @lookup_layered(key, base)
}

// // operator just creates a new layer
@update_layered = λbase. λoverlay.
  #Layer{@to_sorted_list(overlay), base}

// Flatten when layers get too deep (> 8)
@flatten_if_needed = λlayered. ...
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Lookup | O(layers × n_overlay) |
| Update (`//`) | O(1) - just wrap |
| Memory | Excellent for chains of updates |
| Flattening | Needed to bound lookup time |
| Nix compat | Matches Nix behavior closely |

**Best for:** Chains of `//` operations (common in NixOS)

## CHOSEN: Hybrid Layered Approach (Option D)

**Rationale:**
- Matches Nix's `//` layering optimization (up to 8 layers before flattening)
- Keys are Symbol IDs (strict, forced at construction time - this is critical for Nix semantics)
- Values remain lazy (HVM4 thunks until forced)
- O(1) for `//` operation (just wraps layers)
- O(layers × n) for lookup (acceptable for typical use with layer bound)

**Related Features:**
- Uses [Lists](#2-lists) encoding for sorted attribute lists
- Used by [Pattern-Matching Lambdas](#12-pattern-matching-lambdas) for destructuring
- Used by [Recursive Let](#5-recursive-let--rec) for cyclic bindings
- Used by [With Expressions](#6-with-expressions) for dynamic scope

**Encoding:**
```hvm4
// AttrSet = #ABs{sorted_list} | #ALy{overlay, base}
// sorted_list = list of #Atr{key_id, value}
// key_id = Symbol ID (32-bit, already strict in Nix)

// Example: { a = 1; b = 2; }
#ABs{#Con{#Atr{sym_a, 1}, #Con{#Atr{sym_b, 2}, #Nil{}}}}

// Example: { a = 1; } // { b = 2; }
#ALy{
  #Con{#Atr{sym_b, 2}, #Nil{}},    // overlay (right-hand side)
  #ABs{#Con{#Atr{sym_a, 1}, #Nil{}}}  // base (left-hand side)
}
```

### Detailed Implementation Steps

**New Files:**
- `src/libexpr/hvm4/hvm4-attrs.cc`
- `src/libexpr/include/nix/expr/hvm4/hvm4-attrs.hh`

#### Step 1: Define Encoding Constants and API

```cpp
// hvm4-attrs.hh
namespace nix::hvm4 {

// Constructor names (base-64 encoded)
constexpr uint32_t ATTRS_BASE = /* encode "#ABs" */;
constexpr uint32_t ATTRS_LAYER = /* encode "#ALy" */;
constexpr uint32_t ATTR_NODE = /* encode "#Atr" */;

// Maximum layers before forcing a flatten
constexpr size_t MAX_LAYERS = 8;

// Create empty attrset: #ABs{#Nil{}}
Term makeEmptyAttrs(HVM4Runtime& runtime);

// Create base attrset: #ABs{sorted_list}
Term makeBaseAttrs(Term sortedList, HVM4Runtime& runtime);

// Create layer: #ALy{overlay, base}
Term makeLayerAttrs(Term overlay, Term base, HVM4Runtime& runtime);

// Create attribute node: #Atr{key_id, value}
Term makeAttrNode(uint32_t symbolId, Term value, HVM4Runtime& runtime);

// Lookup attribute by symbol ID (returns #Som{value} or #Non{})
Term lookupAttr(uint32_t symbolId, Term attrs, HVM4Runtime& runtime);

// Check if attrset has attribute
Term hasAttr(uint32_t symbolId, Term attrs, HVM4Runtime& runtime);

// Update operator: base // overlay
Term updateAttrs(Term base, Term overlay, HVM4Runtime& runtime);

// Get layer depth
size_t getLayerDepth(Term attrs, const HVM4Runtime& runtime);

// Flatten to base (merge all layers)
Term flattenAttrs(Term attrs, HVM4Runtime& runtime);

// Check if term is our attrset encoding
bool isNixAttrs(Term term);

}  // namespace nix::hvm4
```

#### Step 2: Implement Construction Functions

```cpp
// hvm4-attrs.cc
Term makeEmptyAttrs(HVM4Runtime& runtime) {
    Term nil = runtime.makeCtr(LIST_NIL, 0, nullptr);
    Term args[1] = { nil };
    return runtime.makeCtr(ATTRS_BASE, 1, args);
}

Term makeBaseAttrs(Term sortedList, HVM4Runtime& runtime) {
    Term args[1] = { sortedList };
    return runtime.makeCtr(ATTRS_BASE, 1, args);
}

Term makeLayerAttrs(Term overlay, Term base, HVM4Runtime& runtime) {
    Term args[2] = { overlay, base };
    return runtime.makeCtr(ATTRS_LAYER, 2, args);
}

Term makeAttrNode(uint32_t symbolId, Term value, HVM4Runtime& runtime) {
    Term args[2] = { runtime.makeNum(symbolId), value };
    return runtime.makeCtr(ATTR_NODE, 2, args);
}

// Build sorted list from vector of (symbolId, value) pairs
Term buildSortedAttrList(std::vector<std::pair<uint32_t, Term>>& attrs,
                         HVM4Runtime& runtime) {
    // Sort by symbol ID (keys must be strict)
    std::sort(attrs.begin(), attrs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Build cons list from back to front
    Term list = runtime.makeCtr(LIST_NIL, 0, nullptr);
    for (int i = attrs.size() - 1; i >= 0; i--) {
        Term node = makeAttrNode(attrs[i].first, attrs[i].second, runtime);
        list = makeCons(node, list, runtime);
    }
    return list;
}
```

#### Step 3: Implement O(n) Lookup with Layer Traversal

```cpp
// Lookup in a sorted list
Term lookupInList(uint32_t symbolId, Term list, const HVM4Runtime& runtime) {
    Term current = list;
    while (term_tag(current) == CTR && term_ext(current) == LIST_CONS) {
        uint32_t loc = term_val(current);
        Term node = runtime.getHeapAt(loc);      // #Atr{key, value}
        Term tail = runtime.getHeapAt(loc + 1);

        // Get key from node
        uint32_t nodeLoc = term_val(node);
        Term keyTerm = runtime.getHeapAt(nodeLoc);
        uint32_t key = term_val(keyTerm);

        if (key == symbolId) {
            Term value = runtime.getHeapAt(nodeLoc + 1);
            return makeSome(value, runtime);  // #Som{value}
        }
        if (key > symbolId) {
            // Sorted list - key not present
            return makeNone(runtime);  // #Non{}
        }
        current = tail;
    }
    return makeNone(runtime);  // #Non{}
}

// Lookup with layer traversal
Term lookupAttr(uint32_t symbolId, Term attrs, HVM4Runtime& runtime) {
    Term current = attrs;

    while (true) {
        uint32_t tag = term_ext(current);

        if (tag == ATTRS_BASE) {
            // #ABs{sorted_list}
            uint32_t loc = term_val(current);
            Term sortedList = runtime.getHeapAt(loc);
            return lookupInList(symbolId, sortedList, runtime);
        }
        else if (tag == ATTRS_LAYER) {
            // #ALy{overlay, base}
            uint32_t loc = term_val(current);
            Term overlay = runtime.getHeapAt(loc);
            Term base = runtime.getHeapAt(loc + 1);

            // Check overlay first
            Term result = lookupInList(symbolId, overlay, runtime);
            if (isSome(result)) {
                return result;  // Found in overlay
            }
            // Continue to base
            current = base;
        }
        else {
            // Invalid attrset encoding
            return makeNone(runtime);
        }
    }
}
```

#### Step 4: Implement `//` as Layer Wrapping with Flatten-When-Deep

```cpp
size_t getLayerDepth(Term attrs, const HVM4Runtime& runtime) {
    size_t depth = 0;
    Term current = attrs;

    while (term_ext(current) == ATTRS_LAYER) {
        depth++;
        uint32_t loc = term_val(current);
        current = runtime.getHeapAt(loc + 1);  // base
    }
    return depth;
}

Term updateAttrs(Term base, Term overlay, HVM4Runtime& runtime) {
    // If overlay is empty, return base unchanged
    if (isEmptyAttrs(overlay, runtime)) {
        return base;
    }

    // If base is empty, return overlay
    if (isEmptyAttrs(base, runtime)) {
        return overlay;
    }

    // Check layer depth - flatten if too deep
    size_t depth = getLayerDepth(base, runtime);
    if (depth >= MAX_LAYERS) {
        base = flattenAttrs(base, runtime);
    }

    // Extract overlay's sorted list
    Term overlayList = getAttrsList(overlay, runtime);

    // Create new layer
    return makeLayerAttrs(overlayList, base, runtime);
}

Term flattenAttrs(Term attrs, HVM4Runtime& runtime) {
    // Collect all key-value pairs from all layers
    std::map<uint32_t, Term> merged;  // Sorted by key

    collectAllAttrs(attrs, merged, runtime);

    // Build new sorted list
    std::vector<std::pair<uint32_t, Term>> pairs(merged.begin(), merged.end());
    Term sortedList = buildSortedAttrList(pairs, runtime);

    return makeBaseAttrs(sortedList, runtime);
}

void collectAllAttrs(Term attrs, std::map<uint32_t, Term>& out,
                     const HVM4Runtime& runtime) {
    if (term_ext(attrs) == ATTRS_BASE) {
        uint32_t loc = term_val(attrs);
        Term list = runtime.getHeapAt(loc);
        collectFromList(list, out, runtime);
    }
    else if (term_ext(attrs) == ATTRS_LAYER) {
        uint32_t loc = term_val(attrs);
        Term overlay = runtime.getHeapAt(loc);
        Term base = runtime.getHeapAt(loc + 1);

        // Collect base first, overlay overrides
        collectAllAttrs(base, out, runtime);
        collectFromList(overlay, out, runtime);  // Later entries override
    }
}
```

#### Step 5: Add ExprAttrs and ExprSelect to Compiler

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprAttrs*>(&expr)) {
    if (e->recursive) return false;  // rec { } not yet supported
    for (auto& [symbol, attrDef] : e->attrs) {
        if (!canCompileWithScope(*attrDef.e, scope)) return false;
    }
    for (auto& [expr, attrDef] : e->dynamicAttrs) {
        return false;  // Dynamic attrs not yet supported
    }
    return true;
}

if (auto* e = dynamic_cast<const ExprSelect*>(&expr)) {
    if (!canCompileWithScope(*e->e, scope)) return false;
    if (e->def && !canCompileWithScope(*e->def, scope)) return false;
    return true;
}

// In emit():
if (auto* e = dynamic_cast<const ExprAttrs*>(&expr)) {
    return emitAttrs(*e, ctx);
}

if (auto* e = dynamic_cast<const ExprSelect*>(&expr)) {
    return emitSelect(*e, ctx);
}
```

#### Step 6: Implement Emitters

```cpp
Term HVM4Compiler::emitAttrs(const ExprAttrs& e, CompileContext& ctx) {
    // Collect symbol IDs and values (keys are forced to symbols at construction)
    std::vector<std::pair<uint32_t, Term>> attrs;

    for (auto& [symbol, attrDef] : e.attrs) {
        uint32_t symbolId = symbol.id;  // Key is strict (already resolved)
        Term value = emit(*attrDef.e, ctx);  // Value remains lazy thunk
        attrs.push_back({symbolId, value});
    }

    // Build sorted list
    Term sortedList = buildSortedAttrList(attrs, ctx.runtime());

    // Wrap as base attrset
    return makeBaseAttrs(sortedList, ctx.runtime());
}

Term HVM4Compiler::emitSelect(const ExprSelect& e, CompileContext& ctx) {
    Term attrs = emit(*e.e, ctx);

    // For now, handle simple single-attribute case
    // attrPath is a vector of AttrName (symbol or expression)
    if (e.attrPath.size() != 1) {
        throw CompileError("nested attribute paths not yet supported");
    }

    Symbol symbol = e.attrPath[0].symbol;
    uint32_t symbolId = symbol.id;

    // Generate lookup code
    Term result = lookupAttr(symbolId, attrs, ctx.runtime());

    // Handle default value
    if (e.def) {
        Term defaultVal = emit(*e.def, ctx);
        // Generate: (result is Some) ? unwrap(result) : default
        return emitWithDefault(result, defaultVal, ctx);
    }
    else {
        // Generate: unwrap(result) or throw
        return emitUnwrapOrThrow(result, symbol, ctx);
    }
}
```

#### Step 7: Add ExprOpHasAttr and ExprOpUpdate

```cpp
// In canCompileWithScope:
if (auto* e = dynamic_cast<const ExprOpHasAttr*>(&expr)) {
    if (!canCompileWithScope(*e->e, scope)) return false;
    return true;
}

if (auto* e = dynamic_cast<const ExprOpUpdate*>(&expr)) {
    if (!canCompileWithScope(*e->e1, scope)) return false;
    if (!canCompileWithScope(*e->e2, scope)) return false;
    return true;
}

// Emitters:
Term HVM4Compiler::emitOpHasAttr(const ExprOpHasAttr& e, CompileContext& ctx) {
    Term attrs = emit(*e.e, ctx);

    if (e.attrPath.size() != 1) {
        throw CompileError("nested hasAttr not yet supported");
    }

    Symbol symbol = e.attrPath[0].symbol;
    Term result = hasAttr(symbol.id, attrs, ctx.runtime());
    return result;  // Returns #Tru{} or #Fls{}
}

Term HVM4Compiler::emitOpUpdate(const ExprOpUpdate& e, CompileContext& ctx) {
    Term base = emit(*e.e1, ctx);
    Term overlay = emit(*e.e2, ctx);
    return updateAttrs(base, overlay, ctx.runtime());
}
```

#### Step 8: Implement Result Extraction

```cpp
// In hvm4-result.cc:
void ResultExtractor::extractAttrs(Term term, Value& result) {
    // Flatten all layers to get complete attribute list
    std::map<uint32_t, Term> allAttrs;
    collectAllAttrs(term, allAttrs, runtime);

    // Allocate Nix Bindings
    auto attrs = state.buildBindings(allAttrs.size());

    for (auto& [symbolId, valueTerm] : allAttrs) {
        Symbol symbol = state.symbols.fromId(symbolId);

        // Allocate and extract value
        Value* value = state.mem.allocValue();
        Term evaluated = runtime.evaluateSNF(valueTerm);
        extract(evaluated, *value);

        attrs.insert(symbol, value);
    }

    result.mkAttrs(attrs.finish());
}
```

#### Step 9: Add Primop Support (attrNames, attrValues, hasAttr, getAttr)

```cpp
// Primops are compiled as special function calls
// In emitCall for ExprCall:
if (isPrimop(fun, "attrNames")) {
    Term attrs = emit(*args[0], ctx);
    return emitAttrNames(attrs, ctx);
}

Term emitAttrNames(Term attrs, CompileContext& ctx) {
    // Returns list of attribute names (strings)
    // Generated at runtime:
    // 1. Collect all keys from all layers
    // 2. Sort and dedupe
    // 3. Convert symbol IDs to strings
    // 4. Return as #Lst{length, spine}

    // For now, emit as external call
    return emitExternalPrimop("attrNames", {attrs}, ctx);
}

Term emitAttrValues(Term attrs, CompileContext& ctx) {
    // Returns list of values in name-sorted order
    return emitExternalPrimop("attrValues", {attrs}, ctx);
}
```

#### Step 10: Add Comprehensive Tests

```cpp
// In hvm4.cc test file
TEST_F(HVM4BackendTest, AttrsEmpty) {
    auto v = eval("{}", true);
    ASSERT_EQ(v.type(), nAttrs);
    ASSERT_EQ(v.attrs()->size(), 0);
}

TEST_F(HVM4BackendTest, AttrsSingleton) {
    auto v = eval("{ a = 1; }", true);
    ASSERT_EQ(v.type(), nAttrs);
    ASSERT_EQ(v.attrs()->size(), 1);
    auto a = v.attrs()->get(state.symbols.create("a"));
    ASSERT_NE(a, nullptr);
    state.forceValue(*a->value, noPos);
    ASSERT_EQ(a->value->integer().value, 1);
}

TEST_F(HVM4BackendTest, AttrsMultiple) {
    auto v = eval("{ a = 1; b = 2; c = 3; }", true);
    ASSERT_EQ(v.attrs()->size(), 3);
}

TEST_F(HVM4BackendTest, AttrsSelect) {
    auto v = eval("{ a = 1; b = 2; }.a", true);
    ASSERT_EQ(v.type(), nInt);
    ASSERT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, AttrsSelectWithDefault) {
    auto v = eval("{ a = 1; }.b or 42", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, AttrsHasAttr) {
    auto v = eval("{ a = 1; } ? a", true);
    ASSERT_EQ(v.type(), nBool);
    ASSERT_TRUE(v.boolean());

    v = eval("{ a = 1; } ? b", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, AttrsUpdate) {
    auto v = eval("{ a = 1; } // { b = 2; }", true);
    ASSERT_EQ(v.attrs()->size(), 2);

    auto a = v.attrs()->get(state.symbols.create("a"));
    ASSERT_NE(a, nullptr);

    auto b = v.attrs()->get(state.symbols.create("b"));
    ASSERT_NE(b, nullptr);
}

TEST_F(HVM4BackendTest, AttrsUpdateOverride) {
    auto v = eval("{ a = 1; } // { a = 2; }", true);
    ASSERT_EQ(v.attrs()->size(), 1);

    auto a = v.attrs()->get(state.symbols.create("a"));
    state.forceValue(*a->value, noPos);
    ASSERT_EQ(a->value->integer().value, 2);  // Overlay wins
}

TEST_F(HVM4BackendTest, AttrsNestedUpdate) {
    // Test layer flattening at MAX_LAYERS
    auto v = eval(R"(
        {} // {a=1;} // {b=2;} // {c=3;} // {d=4;}
           // {e=5;} // {f=6;} // {g=7;} // {h=8;} // {i=9;}
    )", true);
    ASSERT_EQ(v.attrs()->size(), 9);
}

TEST_F(HVM4BackendTest, AttrsLazyValues) {
    // Values should not be forced until accessed
    auto v = eval("{ a = 1; b = throw \"not forced\"; }.a", true);
    ASSERT_EQ(v.integer().value, 1);  // Should not throw
}

TEST_F(HVM4BackendTest, AttrsInLet) {
    auto v = eval("let x = 1; in { a = x; b = x + 1; }", true);
    ASSERT_EQ(v.attrs()->size(), 2);
}

// === Edge Cases ===

TEST_F(HVM4BackendTest, AttrsNestedAccess) {
    // Nested attribute access: a.b.c
    auto v = eval("{ a = { b = { c = 42; }; }; }.a.b.c", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, AttrsDuplicateKeys) {
    // Later definition wins (Nix semantics)
    auto v = eval("{ a = 1; a = 2; }.a", true);
    ASSERT_EQ(v.integer().value, 2);
}

TEST_F(HVM4BackendTest, AttrsSpecialCharKeys) {
    // Keys with special characters (quoted)
    auto v = eval("{ \"foo-bar\" = 1; \"with spaces\" = 2; }", true);
    ASSERT_EQ(v.attrs()->size(), 2);
}

TEST_F(HVM4BackendTest, AttrsUpdateEmpty) {
    // Update with empty sets
    auto v = eval("{} // { a = 1; }", true);
    ASSERT_EQ(v.attrs()->size(), 1);

    v = eval("{ a = 1; } // {}", true);
    ASSERT_EQ(v.attrs()->size(), 1);

    v = eval("{} // {}", true);
    ASSERT_EQ(v.attrs()->size(), 0);
}

TEST_F(HVM4BackendTest, AttrsUpdateChainedOverride) {
    // Chained updates with same key - rightmost wins
    auto v = eval("{ a = 1; } // { a = 2; } // { a = 3; }", true);
    auto a = v.attrs()->get(state.symbols.create("a"));
    state.forceValue(*a->value, noPos);
    ASSERT_EQ(a->value->integer().value, 3);
}

TEST_F(HVM4BackendTest, AttrsInherit) {
    // inherit keyword
    auto v = eval("let x = 1; y = 2; in { inherit x y; }", true);
    ASSERT_EQ(v.attrs()->size(), 2);
    auto x = v.attrs()->get(state.symbols.create("x"));
    state.forceValue(*x->value, noPos);
    ASSERT_EQ(x->value->integer().value, 1);
}

TEST_F(HVM4BackendTest, AttrsInheritFrom) {
    // inherit (expr) names
    auto v = eval("let s = { a = 1; b = 2; }; in { inherit (s) a b; }", true);
    ASSERT_EQ(v.attrs()->size(), 2);
}

// === Error Handling ===

TEST_F(HVM4BackendTest, AttrsSelectMissing) {
    // Accessing missing attribute should throw
    EXPECT_THROW(eval("{ a = 1; }.b", true), EvalError);
}

TEST_F(HVM4BackendTest, AttrsSelectOnNonAttrs) {
    // Selecting from non-attrset should throw
    EXPECT_THROW(eval("42.a", true), EvalError);
    EXPECT_THROW(eval("[1 2 3].a", true), EvalError);
}

TEST_F(HVM4BackendTest, AttrsHasAttrOnNonAttrs) {
    // ? on non-attrset should throw
    EXPECT_THROW(eval("42 ? a", true), EvalError);
}

TEST_F(HVM4BackendTest, AttrsUpdateNonAttrs) {
    // // with non-attrset should throw
    EXPECT_THROW(eval("{ a = 1; } // 42", true), EvalError);
    EXPECT_THROW(eval("42 // { a = 1; }", true), EvalError);
}

// === Laziness Verification ===

TEST_F(HVM4BackendTest, AttrsKeyStrictValueLazy) {
    // Keys are strict, values are lazy
    // This should succeed - throw is in value, not accessed
    auto v = eval("{ a = 1; b = throw \"lazy\"; }", true);
    ASSERT_EQ(v.attrs()->size(), 2);  // Keys are known

    // Accessing a should work
    auto a = v.attrs()->get(state.symbols.create("a"));
    state.forceValue(*a->value, noPos);
    ASSERT_EQ(a->value->integer().value, 1);

    // Accessing b should throw
    auto b = v.attrs()->get(state.symbols.create("b"));
    EXPECT_THROW(state.forceValue(*b->value, noPos), EvalError);
}

TEST_F(HVM4BackendTest, AttrsUpdatePreservesLaziness) {
    // // should not force values
    auto v = eval("{ a = throw \"a\"; } // { b = throw \"b\"; }", true);
    ASSERT_EQ(v.attrs()->size(), 2);  // Neither value forced yet
}

// === Ordering Tests ===

TEST_F(HVM4BackendTest, AttrsSortedByName) {
    // attrNames returns sorted list
    auto v = eval("builtins.attrNames { z = 1; a = 2; m = 3; }", true);
    ASSERT_EQ(v.listSize(), 3);
    // Should be ["a", "m", "z"]
}
```

---

# 2. Lists

Nix lists are **arrays of lazy values** with:
- O(1) length and element access
- O(n) concatenation and tail
- Small list optimization (size 1-2 inlined)

## Nix Implementation Details

```cpp
// SmallList for size 1-2, BigList for 0 or 3+
using SmallList = std::array<Value*, 2>;
struct List { size_t size; Value* const* elems; };
```

## Option A: Cons List (Standard Functional)

```hvm4
// List = #Nil{} | #Con{head, tail}

@length = λ{#Nil: 0; #Con: λh.λt. 1 + @length(t)}
@head = λ{#Nil: @error("empty"); #Con: λh.λt. h}
@tail = λ{#Nil: @error("empty"); #Con: λh.λt. t}
@elemAt = λn.λ{#Nil: @error("index"); #Con: λh.λt. (n == 0) .&. h .|. @elemAt(n - 1, t)}

@concat = λxs.λys. λ{#Nil: ys; #Con: λh.λt. #Con{h, @concat(t, ys)}}(xs)
@map = λf.λ{#Nil: #Nil{}; #Con: λh.λt. !F&=f; #Con{F₀(h), @map(F₁, t)}}
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| length | O(n) - must traverse |
| elemAt | O(n) - must traverse |
| head/tail | O(1) |
| concat | O(n) |
| map | O(n) with optimal sharing |
| Memory | Efficient cons cells |
| Laziness | Natural |

**Best for:** Functional patterns, streaming

## Option B: Finger Tree

Amortized O(1) access at both ends, O(log n) concatenation.

```hvm4
// FingerTree = #Empty{} | #Single{elem}
//            | #Deep{prefix, middle, suffix}
// prefix/suffix: 1-4 elements (digits)
// middle: FingerTree of nodes

// Complex but excellent performance characteristics
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| length | O(1) if cached |
| elemAt | O(log n) |
| head/tail | O(1) amortized |
| concat | O(log(min(n,m))) |
| Complexity | Very high |

**Best for:** Heavy list manipulation

## Option C: Chunked Rope

Balanced tree of array chunks.

```hvm4
// Rope = #Leaf{array} | #Node{left, right, total_len}
// Leaf contains small fixed-size array (e.g., 32 elements)

// Good for large lists with random access
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| length | O(1) if cached |
| elemAt | O(log n) |
| concat | O(log n) |
| HVM4 fit | Needs array support |

## Option D: Difference List (for building)

For efficient list building, then convert to cons list.

```hvm4
// DList = λtail. ... (function that prepends to tail)
@dnil = λtail. tail
@dcons = λh.λdl. λtail. #Cons{h, dl(tail)}
@dconcat = λdl1.λdl2. λtail. dl1(dl2(tail))
@to_list = λdl. dl(#Nil{})

// Build in O(1) per element, convert in O(n) total
```

**Best for:** Building lists incrementally

## CHOSEN: Spine-Strict Cons List with Cached Length (Modified Option A)

**Rationale:**
- Nix requires O(1) length - we cache it in a wrapper constructor
- HVM4's #Con{head, tail} provides natural lazy elements
- Elements remain as thunks until forced (preserves Nix laziness)
- `tail` being O(n) in Nix means we lose nothing with cons lists

**Encoding:**
```hvm4
// List = #Lst{length, spine}
// Spine = #Nil{} | #Con{head, tail}

// Example: [1, 2, 3]
#Lst{3, #Con{1, #Con{2, #Con{3, #Nil{}}}}}

// Empty list
#Lst{0, #Nil{}}
```

### Detailed Implementation Steps

**New Files:**
- `src/libexpr/hvm4/hvm4-list.cc`
- `src/libexpr/include/nix/expr/hvm4/hvm4-list.hh`

#### Step 1: Define Encoding Constants and API

```cpp
// hvm4-list.hh
namespace nix::hvm4 {

// Constructor names (base-64 encoded from HVM4)
constexpr uint32_t LIST_NIL = 166118;   // #Nil
constexpr uint32_t LIST_CONS = 121448;  // #Con
constexpr uint32_t LIST_WRAPPER = /* encode "#Lst" */;

// Create empty list: #Lst{0, #Nil{}}
Term makeEmptyList(HVM4Runtime& runtime);

// Create cons cell: #Con{head, tail}
Term makeCons(Term head, Term tail, HVM4Runtime& runtime);

// Create wrapped list: #Lst{length, spine}
Term makeList(uint32_t length, Term spine, HVM4Runtime& runtime);

// Decode list length O(1)
uint32_t listLength(Term listTerm, const HVM4Runtime& runtime);

// Get list spine for iteration
Term listSpine(Term listTerm, const HVM4Runtime& runtime);

// Check if term is our list encoding
bool isNixList(Term term);

}  // namespace nix::hvm4
```

#### Step 2: Implement Construction Functions

```cpp
// hvm4-list.cc
Term makeEmptyList(HVM4Runtime& runtime) {
    Term nil = runtime.makeCtr(LIST_NIL, 0, nullptr);
    Term args[2] = { runtime.makeNum(0), nil };
    return runtime.makeCtr(LIST_WRAPPER, 2, args);
}

Term makeCons(Term head, Term tail, HVM4Runtime& runtime) {
    Term args[2] = { head, tail };
    return runtime.makeCtr(LIST_CONS, 2, args);
}

Term makeList(uint32_t length, Term spine, HVM4Runtime& runtime) {
    Term args[2] = { runtime.makeNum(length), spine };
    return runtime.makeCtr(LIST_WRAPPER, 2, args);
}

uint32_t listLength(Term listTerm, const HVM4Runtime& runtime) {
    // #Lst{length, spine} - length is first field
    uint32_t loc = term_val(listTerm);
    Term lengthTerm = runtime.getHeapAt(loc);
    return term_val(lengthTerm);
}

Term listSpine(Term listTerm, const HVM4Runtime& runtime) {
    uint32_t loc = term_val(listTerm);
    return runtime.getHeapAt(loc + 1);
}
```

#### Step 3: Add ExprList to Compiler

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprList*>(&expr)) {
    for (auto* elem : e->elems) {
        if (!canCompileWithScope(*elem, scope)) return false;
    }
    return true;
}

// In emit():
if (auto* e = dynamic_cast<const ExprList*>(&expr)) {
    return emitList(*e, ctx);
}

// New emitter:
Term HVM4Compiler::emitList(const ExprList& e, CompileContext& ctx) {
    uint32_t length = e.elems.size();

    // Build spine from back to front
    Term spine = ctx.runtime().makeCtr(LIST_NIL, 0, nullptr);
    for (int i = length - 1; i >= 0; i--) {
        Term elem = emit(*e.elems[i], ctx);
        spine = makeCons(elem, spine, ctx.runtime());
    }

    // Wrap with length
    return makeList(length, spine, ctx.runtime());
}
```

#### Step 4: Implement Result Extraction

```cpp
// In hvm4-result.cc:
void ResultExtractor::extractList(Term term, Value& result) {
    uint32_t length = listLength(term, runtime);
    Term spine = listSpine(term, runtime);

    // Allocate Nix list
    state.mkList(result, length);

    // Extract each element
    Term current = spine;
    for (uint32_t i = 0; i < length; i++) {
        // #Con{head, tail}
        uint32_t loc = term_val(current);
        Term head = runtime.getHeapAt(loc);
        Term tail = runtime.getHeapAt(loc + 1);

        // Evaluate and extract head
        head = runtime.evaluateSNF(head);
        result.listElems()[i] = state.mem.allocValue();
        extract(head, *result.listElems()[i]);

        current = tail;
    }
}
```

#### Step 5: Add List Operation Primops

```cpp
// builtins.length - O(1)
Term primopLength(Term list, CompileContext& ctx) {
    // Extract length field from #Lst wrapper
    return ctx.runtime().makeOp2(/* extract first field */);
}

// builtins.head - O(1)
Term primopHead(Term list, CompileContext& ctx) {
    // Get spine, then first element of cons
}

// builtins.tail - O(1) to create, but needs new wrapper
Term primopTail(Term list, CompileContext& ctx) {
    // Create #Lst{length-1, tail_of_spine}
}

// builtins.elemAt - O(n) traversal
Term primopElemAt(Term list, Term idx, CompileContext& ctx) {
    // Recursive traversal of spine
}

// builtins.map - O(n) lazy
Term primopMap(Term fn, Term list, CompileContext& ctx) {
    // Create new #Lst with mapped spine (lazy application to each elem)
}
```

#### Step 6: Add Comprehensive Tests

```cpp
// In hvm4.cc test file
TEST_F(HVM4BackendTest, ListEmpty) {
    auto v = eval("[]", true);
    ASSERT_EQ(v.type(), nList);
    ASSERT_EQ(v.listSize(), 0);
}

TEST_F(HVM4BackendTest, ListSingleton) {
    auto v = eval("[1]", true);
    ASSERT_EQ(v.type(), nList);
    ASSERT_EQ(v.listSize(), 1);
    state.forceValue(*v.listElems()[0], noPos);
    ASSERT_EQ(v.listElems()[0]->integer().value, 1);
}

TEST_F(HVM4BackendTest, ListMultiple) {
    auto v = eval("[1, 2, 3]", true);
    ASSERT_EQ(v.listSize(), 3);
}

TEST_F(HVM4BackendTest, ListWithExpressions) {
    auto v = eval("[1 + 1, 2 + 2, 3 + 3]", true);
    ASSERT_EQ(v.listSize(), 3);
    state.forceValue(*v.listElems()[0], noPos);
    ASSERT_EQ(v.listElems()[0]->integer().value, 2);
}

TEST_F(HVM4BackendTest, ListInLet) {
    auto v = eval("let x = 1; in [x, x + 1, x + 2]", true);
    ASSERT_EQ(v.listSize(), 3);
}

TEST_F(HVM4BackendTest, ListNested) {
    auto v = eval("[[1], [2, 3]]", true);
    ASSERT_EQ(v.listSize(), 2);
}

// === Laziness Verification ===

TEST_F(HVM4BackendTest, ListLengthDoesNotForceElements) {
    // CRITICAL: length is O(1) and must not force elements
    auto v = eval("[1, throw \"not forced\", 3]", true);
    ASSERT_EQ(v.listSize(), 3);  // Should not throw
}

TEST_F(HVM4BackendTest, ListElementsLazyUntilAccessed) {
    auto v = eval("let xs = [1, throw \"lazy\", 3]; in builtins.elemAt xs 0", true);
    ASSERT_EQ(v.integer().value, 1);  // Should not throw
}

TEST_F(HVM4BackendTest, ListForceThrowsOnBadElement) {
    auto v = eval("[1, throw \"forced\", 3]", true);
    EXPECT_THROW(state.forceValue(*v.listElems()[1], noPos), EvalError);
}

// === List Primops ===

TEST_F(HVM4BackendTest, ListLength) {
    auto v = eval("builtins.length [1 2 3 4 5]", true);
    ASSERT_EQ(v.integer().value, 5);
}

TEST_F(HVM4BackendTest, ListHead) {
    auto v = eval("builtins.head [1 2 3]", true);
    ASSERT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, ListHeadEmpty) {
    EXPECT_THROW(eval("builtins.head []", true), EvalError);
}

TEST_F(HVM4BackendTest, ListTail) {
    auto v = eval("builtins.tail [1 2 3]", true);
    ASSERT_EQ(v.listSize(), 2);
    state.forceValue(*v.listElems()[0], noPos);
    ASSERT_EQ(v.listElems()[0]->integer().value, 2);
}

TEST_F(HVM4BackendTest, ListTailEmpty) {
    EXPECT_THROW(eval("builtins.tail []", true), EvalError);
}

TEST_F(HVM4BackendTest, ListElemAt) {
    auto v = eval("builtins.elemAt [10 20 30] 1", true);
    ASSERT_EQ(v.integer().value, 20);
}

TEST_F(HVM4BackendTest, ListElemAtOutOfBounds) {
    EXPECT_THROW(eval("builtins.elemAt [1 2 3] 5", true), EvalError);
    EXPECT_THROW(eval("builtins.elemAt [1 2 3] (-1)", true), EvalError);
}

TEST_F(HVM4BackendTest, ListConcat) {
    auto v = eval("[1 2] ++ [3 4]", true);
    ASSERT_EQ(v.listSize(), 4);
}

TEST_F(HVM4BackendTest, ListConcatEmpty) {
    auto v = eval("[] ++ [1 2]", true);
    ASSERT_EQ(v.listSize(), 2);

    v = eval("[1 2] ++ []", true);
    ASSERT_EQ(v.listSize(), 2);

    v = eval("[] ++ []", true);
    ASSERT_EQ(v.listSize(), 0);
}

TEST_F(HVM4BackendTest, ListMap) {
    auto v = eval("builtins.map (x: x * 2) [1 2 3]", true);
    ASSERT_EQ(v.listSize(), 3);
    state.forceValue(*v.listElems()[0], noPos);
    ASSERT_EQ(v.listElems()[0]->integer().value, 2);
    state.forceValue(*v.listElems()[2], noPos);
    ASSERT_EQ(v.listElems()[2]->integer().value, 6);
}

TEST_F(HVM4BackendTest, ListFilter) {
    auto v = eval("builtins.filter (x: x > 2) [1 2 3 4 5]", true);
    ASSERT_EQ(v.listSize(), 3);  // [3, 4, 5]
}

TEST_F(HVM4BackendTest, ListFoldl) {
    auto v = eval("builtins.foldl' (a: b: a + b) 0 [1 2 3 4]", true);
    ASSERT_EQ(v.integer().value, 10);
}

TEST_F(HVM4BackendTest, ListConcatLists) {
    auto v = eval("builtins.concatLists [[1 2] [3] [4 5 6]]", true);
    ASSERT_EQ(v.listSize(), 6);
}

TEST_F(HVM4BackendTest, ListGenList) {
    auto v = eval("builtins.genList (x: x * x) 5", true);
    ASSERT_EQ(v.listSize(), 5);  // [0, 1, 4, 9, 16]
    state.forceValue(*v.listElems()[4], noPos);
    ASSERT_EQ(v.listElems()[4]->integer().value, 16);
}

TEST_F(HVM4BackendTest, ListElem) {
    auto v = eval("builtins.elem 3 [1 2 3 4]", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.elem 5 [1 2 3 4]", true);
    ASSERT_FALSE(v.boolean());
}

// === Edge Cases ===

TEST_F(HVM4BackendTest, ListLarge) {
    // Test with larger list
    auto v = eval("builtins.genList (x: x) 100", true);
    ASSERT_EQ(v.listSize(), 100);
}

TEST_F(HVM4BackendTest, ListMixedTypes) {
    // Lists can contain mixed types
    auto v = eval("[1, \"hello\", true, { a = 1; }]", true);
    ASSERT_EQ(v.listSize(), 4);
}

TEST_F(HVM4BackendTest, ListSort) {
    auto v = eval("builtins.sort builtins.lessThan [3 1 4 1 5 9 2 6]", true);
    ASSERT_EQ(v.listSize(), 8);
    state.forceValue(*v.listElems()[0], noPos);
    ASSERT_EQ(v.listElems()[0]->integer().value, 1);
}
```

---

# 3. Strings

Nix strings have:
- Arbitrary length content
- **String context** tracking store path dependencies
- Interpolation (`"${expr}"`)
- Path conversion with context tracking

## Nix Implementation Details

```cpp
struct StringWithContext {
    const StringData* str;      // The string content
    const Context* context;     // Store path dependencies (may be null)
};

// Context elements: Opaque (path), DrvDeep (drv+closure), Built (drv output)
```

## Option A: List of Character Codes

Simple encoding as list of integers.

```hvm4
// String = List of Char (32-bit Unicode codepoints)
// Already supported by HVM4 syntax: "hello" → cons list

@str_length = @length  // Reuse list length
@str_concat = @concat
@substring = λstart.λlen.λstr. @take(len, @drop(start, str))
@str_eq = λs1.λs2. @list_eq(s1, s2)
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Memory | 64 bits per character (inefficient) |
| Operations | O(n) for most |
| Unicode | Full support |
| Implementation | Trivial |

**Best for:** Prototyping, small strings

## Option B: Chunked String (Rope)

Tree of string chunks for efficient operations.

```hvm4
// Rope = #Leaf{chars} | #Node{left, right, total_len}
// chars is small list of characters

@rope_concat = λr1.λr2. #Node{r1, r2, @rope_len(r1) + @rope_len(r2)}
@rope_substring = λstart.λlen.λrope. ...  // O(log n) split operations
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| concat | O(1) or O(log n) |
| substring | O(log n) |
| Complexity | Medium |

## Option C: Packed Chunks

Pack multiple characters into 32-bit words.

```hvm4
// Pack 4 ASCII chars or 2 UTF-16 code units per NUM
// String = #Str{encoding, chunks} where chunks is list of packed NUMs

// Requires bit manipulation
@pack_ascii = λc1.λc2.λc3.λc4. c1 + (c2 << 8) + (c3 << 16) + (c4 << 24)
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Memory | 4x more efficient for ASCII |
| Operations | More complex |
| Unicode | Needs variable encoding |

## String Context Handling

**Critical**: String context must be threaded through all string operations.

```hvm4
// StringWithContext = #Str{chars, context}
// Context = #NoC{} | #Ctx{elements}
// CtxElem = #Opq{path} | #Drv{drvPath} | #Blt{drvPath, output}

@str_concat = λs1.λs2.
  #Str{@char_concat(s1.chars, s2.chars), @ctx_merge(s1.context, s2.context)}

@ctx_merge = λc1.λc2. λ{
  #NoC: c2
  #Ctx: λelems1. λ{
    #NoC: c1
    #Ctx: λelems2. #Ctx{@set_union(elems1, elems2)}
  }(c2)
}(c1)
```

**Key Insight**: Context is a **set** - use same encoding as attribute set keys.

## CHOSEN: HVM4 Native #Chr List + Context Wrapper (Option A with Context)

**Rationale:**
- HVM4 already represents strings as `#Con{#Chr{n}, ...}` natively
- Wrap with context to preserve Nix's store path tracking
- Context is a sorted set (reuse same encoding as attribute set keys)
- String content is strict in Nix (not lazy), so char list is natural
- Memory cost (64 bits per char) acceptable for most Nix strings

**Encoding:**
```hvm4
// StringWithContext = #Str{chars, context}
// chars = list of #Chr{codepoint}
// context = #NoC{} | #Ctx{elements}
// elements = sorted list of context element hashes

// Example: "hello" with no context
#Str{#Con{#Chr{104}, #Con{#Chr{101}, #Con{#Chr{108},
     #Con{#Chr{108}, #Con{#Chr{111}, #Nil{}}}}}}, #NoC{}}

// Example: "${drv}" with context
#Str{chars, #Ctx{#Con{hash_of_drv, #Nil{}}}}
```

### Detailed Implementation Steps

**New Files:**
- `src/libexpr/hvm4/hvm4-string.cc`
- `src/libexpr/include/nix/expr/hvm4/hvm4-string.hh`

#### Step 1: Define Encoding Constants and API

```cpp
// hvm4-string.hh
namespace nix::hvm4 {

// Constructor names (base-64 encoded)
constexpr uint32_t STRING_WRAPPER = /* encode "#Str" */;
constexpr uint32_t CHAR_NODE = /* encode "#Chr" */;   // HVM4 native
constexpr uint32_t CONTEXT_NONE = /* encode "#NoC" */;
constexpr uint32_t CONTEXT_SET = /* encode "#Ctx" */;

// Create string with no context
Term makeString(std::string_view content, HVM4Runtime& runtime);

// Create string with context
Term makeStringWithContext(std::string_view content,
                           const NixStringContext& context,
                           HVM4Runtime& runtime);

// Create empty string
Term makeEmptyString(HVM4Runtime& runtime);

// Concatenate two strings (merges contexts)
Term concatStrings(Term s1, Term s2, HVM4Runtime& runtime);

// Get string length (character count)
uint32_t stringLength(Term strTerm, const HVM4Runtime& runtime);

// Extract chars list from string wrapper
Term stringChars(Term strTerm, const HVM4Runtime& runtime);

// Extract context from string wrapper
Term stringContext(Term strTerm, const HVM4Runtime& runtime);

// Check if term is our string encoding
bool isNixString(Term term);

}  // namespace nix::hvm4
```

#### Step 2: Implement String Construction

```cpp
// hvm4-string.cc
Term makeChar(uint32_t codepoint, HVM4Runtime& runtime) {
    Term args[1] = { runtime.makeNum(codepoint) };
    return runtime.makeCtr(CHAR_NODE, 1, args);
}

Term makeCharList(std::string_view content, HVM4Runtime& runtime) {
    // Build from back to front (cons list)
    Term list = runtime.makeCtr(LIST_NIL, 0, nullptr);

    // Handle UTF-8 → codepoints
    auto it = content.rbegin();
    while (it != content.rend()) {
        // For now, assume ASCII (1 byte = 1 codepoint)
        // TODO: Full UTF-8 decoding
        uint32_t codepoint = static_cast<uint8_t>(*it);
        Term chr = makeChar(codepoint, runtime);
        list = makeCons(chr, list, runtime);
        ++it;
    }
    return list;
}

Term makeNoContext(HVM4Runtime& runtime) {
    return runtime.makeCtr(CONTEXT_NONE, 0, nullptr);
}

Term makeContextSet(Term elements, HVM4Runtime& runtime) {
    Term args[1] = { elements };
    return runtime.makeCtr(CONTEXT_SET, 1, args);
}

Term makeString(std::string_view content, HVM4Runtime& runtime) {
    Term chars = makeCharList(content, runtime);
    Term ctx = makeNoContext(runtime);
    Term args[2] = { chars, ctx };
    return runtime.makeCtr(STRING_WRAPPER, 2, args);
}

Term makeStringWithContext(std::string_view content,
                           const NixStringContext& nixCtx,
                           HVM4Runtime& runtime) {
    Term chars = makeCharList(content, runtime);
    Term ctx = encodeContext(nixCtx, runtime);
    Term args[2] = { chars, ctx };
    return runtime.makeCtr(STRING_WRAPPER, 2, args);
}
```

#### Step 3: Implement Context Encoding

```cpp
// Context is a set of context elements, encoded as sorted list of hashes
Term encodeContext(const NixStringContext& nixCtx, HVM4Runtime& runtime) {
    if (nixCtx.empty()) {
        return makeNoContext(runtime);
    }

    // Collect and sort context element hashes
    std::vector<uint32_t> hashes;
    for (const auto& elem : nixCtx) {
        // Hash the context element for compact representation
        // Store full elements in a side table if needed for extraction
        uint32_t hash = hashContextElement(elem);
        hashes.push_back(hash);
    }
    std::sort(hashes.begin(), hashes.end());

    // Build sorted list
    Term list = runtime.makeCtr(LIST_NIL, 0, nullptr);
    for (int i = hashes.size() - 1; i >= 0; i--) {
        Term hashTerm = runtime.makeNum(hashes[i]);
        list = makeCons(hashTerm, list, runtime);
    }

    return makeContextSet(list, runtime);
}

// Merge two contexts (set union)
Term mergeContext(Term ctx1, Term ctx2, HVM4Runtime& runtime) {
    // If either is NoContext, return the other
    if (term_ext(ctx1) == CONTEXT_NONE) return ctx2;
    if (term_ext(ctx2) == CONTEXT_NONE) return ctx1;

    // Both have context - merge sorted lists
    Term list1 = getContextElements(ctx1, runtime);
    Term list2 = getContextElements(ctx2, runtime);
    Term merged = mergeSortedLists(list1, list2, runtime);

    return makeContextSet(merged, runtime);
}
```

#### Step 4: Add ExprString Support

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprString*>(&expr)) {
    return true;  // String literals always compile
}

// In emit():
if (auto* e = dynamic_cast<const ExprString*>(&expr)) {
    return emitString(*e, ctx);
}

Term HVM4Compiler::emitString(const ExprString& e, CompileContext& ctx) {
    // ExprString contains the string content
    std::string_view content = e.s;
    return makeString(content, ctx.runtime());
}
```

#### Step 5: Update ExprConcatStrings for String Interpolation

```cpp
// In canCompileWithScope:
if (auto* e = dynamic_cast<const ExprConcatStrings*>(&expr)) {
    for (auto& [pos, subExpr] : *e->es) {
        if (!canCompileWithScope(*subExpr, scope)) return false;
    }
    return true;
}

// Emitter:
Term HVM4Compiler::emitConcatStrings(const ExprConcatStrings& e,
                                      CompileContext& ctx) {
    if (e.es->empty()) {
        return makeEmptyString(ctx.runtime());
    }

    // Compile first element
    Term result = emit(*(*e.es)[0].second, ctx);

    // If forceString, ensure it's converted to string
    if (e.forceString) {
        result = emitCoerceToString(result, ctx);
    }

    // Concatenate remaining elements
    for (size_t i = 1; i < e.es->size(); i++) {
        Term elem = emit(*(*e.es)[i].second, ctx);
        if (e.forceString) {
            elem = emitCoerceToString(elem, ctx);
        }
        result = concatStrings(result, elem, ctx.runtime());
    }

    return result;
}

// String concatenation with context merge
Term concatStrings(Term s1, Term s2, HVM4Runtime& runtime) {
    // Extract parts
    Term chars1 = stringChars(s1, runtime);
    Term ctx1 = stringContext(s1, runtime);
    Term chars2 = stringChars(s2, runtime);
    Term ctx2 = stringContext(s2, runtime);

    // Concat char lists (append chars2 to chars1)
    Term mergedChars = appendLists(chars1, chars2, runtime);

    // Merge contexts
    Term mergedCtx = mergeContext(ctx1, ctx2, runtime);

    // Build result
    Term args[2] = { mergedChars, mergedCtx };
    return runtime.makeCtr(STRING_WRAPPER, 2, args);
}
```

#### Step 6: Implement String Operations

```cpp
// String length - count chars in list
uint32_t stringLength(Term strTerm, const HVM4Runtime& runtime) {
    Term chars = stringChars(strTerm, runtime);
    uint32_t count = 0;

    Term current = chars;
    while (term_ext(current) == LIST_CONS) {
        count++;
        uint32_t loc = term_val(current);
        current = runtime.getHeapAt(loc + 1);  // tail
    }
    return count;
}

// Substring operation
Term substringOp(Term str, uint32_t start, uint32_t len,
                 HVM4Runtime& runtime) {
    Term chars = stringChars(str, runtime);
    Term ctx = stringContext(str, runtime);  // Context preserved

    // Skip 'start' characters
    Term current = chars;
    for (uint32_t i = 0; i < start && term_ext(current) == LIST_CONS; i++) {
        uint32_t loc = term_val(current);
        current = runtime.getHeapAt(loc + 1);
    }

    // Take 'len' characters
    Term resultChars = takeN(current, len, runtime);

    Term args[2] = { resultChars, ctx };
    return runtime.makeCtr(STRING_WRAPPER, 2, args);
}

// String equality (for comparisons)
Term stringEqual(Term s1, Term s2, HVM4Runtime& runtime) {
    Term chars1 = stringChars(s1, runtime);
    Term chars2 = stringChars(s2, runtime);

    // Compare character by character
    return listEqual(chars1, chars2, runtime);
}
```

#### Step 7: Implement Result Extraction

```cpp
// In hvm4-result.cc:
void ResultExtractor::extractString(Term term, Value& result) {
    Term chars = stringChars(term, runtime);
    Term ctx = stringContext(term, runtime);

    // Extract characters to std::string
    std::string content;
    Term current = chars;
    while (term_ext(current) == LIST_CONS) {
        uint32_t loc = term_val(current);
        Term chrTerm = runtime.getHeapAt(loc);

        // #Chr{codepoint}
        uint32_t chrLoc = term_val(chrTerm);
        Term cpTerm = runtime.getHeapAt(chrLoc);
        uint32_t codepoint = term_val(cpTerm);

        // For now, assume ASCII
        content.push_back(static_cast<char>(codepoint));

        current = runtime.getHeapAt(loc + 1);  // tail
    }

    // Extract context
    NixStringContext nixCtx;
    if (term_ext(ctx) == CONTEXT_SET) {
        uint32_t loc = term_val(ctx);
        Term elements = runtime.getHeapAt(loc);
        decodeContextElements(elements, nixCtx, runtime);
    }

    // Create Nix string value
    if (nixCtx.empty()) {
        result.mkString(content);
    } else {
        result.mkStringWithContext(content, nixCtx);
    }
}
```

#### Step 8: Add Comprehensive Tests

```cpp
// In hvm4.cc test file
TEST_F(HVM4BackendTest, StringEmpty) {
    auto v = eval("\"\"", true);
    ASSERT_EQ(v.type(), nString);
    ASSERT_EQ(v.string_view(), "");
}

TEST_F(HVM4BackendTest, StringSimple) {
    auto v = eval("\"hello\"", true);
    ASSERT_EQ(v.type(), nString);
    ASSERT_EQ(v.string_view(), "hello");
}

TEST_F(HVM4BackendTest, StringWithSpaces) {
    auto v = eval("\"hello world\"", true);
    ASSERT_EQ(v.string_view(), "hello world");
}

TEST_F(HVM4BackendTest, StringInterpolation) {
    auto v = eval("let x = \"world\"; in \"hello ${x}\"", true);
    ASSERT_EQ(v.string_view(), "hello world");
}

TEST_F(HVM4BackendTest, StringIntInterpolation) {
    auto v = eval("let n = 42; in \"number: ${toString n}\"", true);
    // Requires toString primop
    ASSERT_EQ(v.string_view(), "number: 42");
}

TEST_F(HVM4BackendTest, StringConcat) {
    auto v = eval("\"hello\" + \" \" + \"world\"", true);
    ASSERT_EQ(v.string_view(), "hello world");
}

TEST_F(HVM4BackendTest, StringInLet) {
    auto v = eval("let s = \"test\"; in s", true);
    ASSERT_EQ(v.string_view(), "test");
}

TEST_F(HVM4BackendTest, StringNoContext) {
    auto v = eval("\"plain string\"", true);
    ASSERT_TRUE(v.context().empty());
}

TEST_F(HVM4BackendTest, StringMultiline) {
    auto v = eval("''\n  line1\n  line2\n''", true);
    ASSERT_EQ(v.string_view(), "line1\nline2\n");
}

TEST_F(HVM4BackendTest, StringEscape) {
    auto v = eval("\"hello\\nworld\"", true);
    ASSERT_EQ(v.string_view(), "hello\nworld");
}

TEST_F(HVM4BackendTest, StringLength) {
    // Requires stringLength primop
    auto v = eval("builtins.stringLength \"hello\"", true);
    ASSERT_EQ(v.integer().value, 5);
}

// === Escape Sequences ===

TEST_F(HVM4BackendTest, StringEscapeTab) {
    auto v = eval("\"a\\tb\"", true);
    ASSERT_EQ(v.string_view(), "a\tb");
}

TEST_F(HVM4BackendTest, StringEscapeCarriageReturn) {
    auto v = eval("\"a\\rb\"", true);
    ASSERT_EQ(v.string_view(), "a\rb");
}

TEST_F(HVM4BackendTest, StringEscapeBackslash) {
    auto v = eval("\"a\\\\b\"", true);
    ASSERT_EQ(v.string_view(), "a\\b");
}

TEST_F(HVM4BackendTest, StringEscapeDollar) {
    auto v = eval("\"a\\${b}\"", true);  // Escaped interpolation
    ASSERT_EQ(v.string_view(), "a${b}");
}

TEST_F(HVM4BackendTest, StringEscapeQuote) {
    auto v = eval("\"a\\\"b\"", true);
    ASSERT_EQ(v.string_view(), "a\"b");
}

// === Unicode ===

TEST_F(HVM4BackendTest, StringUnicodeBasic) {
    auto v = eval("\"héllo wörld\"", true);
    ASSERT_EQ(v.string_view(), "héllo wörld");
}

TEST_F(HVM4BackendTest, StringUnicodeEmoji) {
    auto v = eval("\"hello 🌍\"", true);
    ASSERT_EQ(v.string_view(), "hello 🌍");
}

TEST_F(HVM4BackendTest, StringUnicodeCJK) {
    auto v = eval("\"你好世界\"", true);
    ASSERT_EQ(v.string_view(), "你好世界");
}

TEST_F(HVM4BackendTest, StringLengthUnicode) {
    // Note: Nix counts bytes, not codepoints
    auto v = eval("builtins.stringLength \"héllo\"", true);
    // "héllo" is 6 bytes in UTF-8 (é is 2 bytes)
    ASSERT_EQ(v.integer().value, 6);
}

// === Multiline Strings ===

TEST_F(HVM4BackendTest, StringMultilineIndent) {
    auto v = eval("''\n    line1\n    line2\n  ''", true);
    // Indentation stripping
    ASSERT_EQ(v.string_view(), "  line1\n  line2\n");
}

TEST_F(HVM4BackendTest, StringMultilineEscape) {
    auto v = eval("''\\n''", true);  // Literal \n in multiline
    ASSERT_EQ(v.string_view(), "\\n");
}

TEST_F(HVM4BackendTest, StringMultilineInterpolation) {
    auto v = eval("let x = \"test\"; in ''\n  ${x}\n''", true);
    ASSERT_EQ(v.string_view(), "test\n");
}

// === String Primops ===

TEST_F(HVM4BackendTest, StringSubstring) {
    auto v = eval("builtins.substring 0 5 \"hello world\"", true);
    ASSERT_EQ(v.string_view(), "hello");
}

TEST_F(HVM4BackendTest, StringSubstringMiddle) {
    auto v = eval("builtins.substring 6 5 \"hello world\"", true);
    ASSERT_EQ(v.string_view(), "world");
}

TEST_F(HVM4BackendTest, StringSubstringBeyondEnd) {
    // Nix clamps to string length
    auto v = eval("builtins.substring 6 100 \"hello world\"", true);
    ASSERT_EQ(v.string_view(), "world");
}

TEST_F(HVM4BackendTest, StringReplaceStrings) {
    auto v = eval("builtins.replaceStrings [\"o\"] [\"0\"] \"hello world\"", true);
    ASSERT_EQ(v.string_view(), "hell0 w0rld");
}

TEST_F(HVM4BackendTest, StringSplit) {
    auto v = eval("builtins.split \",\" \"a,b,c\"", true);
    ASSERT_EQ(v.listSize(), 5);  // ["a", [","], "b", [","], "c"]
}

TEST_F(HVM4BackendTest, StringMatch) {
    auto v = eval("builtins.match \"a(.)c\" \"abc\"", true);
    ASSERT_EQ(v.listSize(), 1);  // ["b"]
}

TEST_F(HVM4BackendTest, StringMatchNoMatch) {
    auto v = eval("builtins.match \"xyz\" \"abc\"", true);
    ASSERT_EQ(v.type(), nNull);  // null when no match
}

TEST_F(HVM4BackendTest, StringHashString) {
    auto v = eval("builtins.hashString \"sha256\" \"hello\"", true);
    // SHA256 of "hello"
    ASSERT_EQ(v.string_view(),
        "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

// === Context Handling ===

TEST_F(HVM4BackendTest, StringContextMerge) {
    // Context should merge when concatenating
    // Note: This test requires derivation support to create context
    // For now, test that context is preserved through operations
    auto v = eval("\"${./test}\"", true);
    ASSERT_FALSE(v.context().empty());  // Path adds context
}

TEST_F(HVM4BackendTest, StringContextPreserved) {
    // Context preserved through substring
    auto v = eval("builtins.substring 0 3 \"${./test}\"", true);
    ASSERT_FALSE(v.context().empty());
}

// === Error Handling ===

TEST_F(HVM4BackendTest, StringConcatNonString) {
    // Concatenating non-coercible type should throw
    EXPECT_THROW(eval("\"hello\" + 42", true), EvalError);
}

TEST_F(HVM4BackendTest, StringSubstringNegativeStart) {
    // Negative start should throw
    EXPECT_THROW(eval("builtins.substring (-1) 5 \"hello\"", true), EvalError);
}

// === Strictness ===

TEST_F(HVM4BackendTest, StringInterpolationStrict) {
    // String interpolation forces the interpolated expression
    EXPECT_THROW(eval("\"${throw \\\"error\\\"}\"", true), EvalError);
}

TEST_F(HVM4BackendTest, StringContentStrict) {
    // Unlike lists, string content is always strict
    // This should throw during construction
    EXPECT_THROW(eval("\"hello ${throw \\\"x\\\"}\"", true), EvalError);
}
```

---

# 4. Paths

Nix paths have:
- A `SourceAccessor` for virtual filesystem access
- Automatic copying to store when coerced to string
- Context tracking for store dependencies

## Nix Implementation Details

```cpp
struct Path {
    SourceAccessor* accessor;  // Virtual FS
    const StringData* path;    // Path string
};
```

## Option A: Pure Path (No Store Interaction)

Represent paths as strings, defer store operations.

```hvm4
// Path = #Pth{accessor_id, path_string}
// accessor_id identifies which SourceAccessor (0 = real FS)

@path_concat = λp1.λs2. #Pth{p1.accessor, @str_concat(p1.path, s2)}
@path_to_string = λpath. path.path  // Just extract string (no store copy)
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Store copy | Not handled - deferred |
| Simplicity | High |
| Compatibility | Partial - no derivation inputs |

## Option B: Effect-Based Store Interaction

Model store operations as effects for external handling.

```hvm4
// Effect = #Pure{value} | #CopyToStore{path, continuation}

@coerce_path_to_string = λpath. λcopy_to_store.
  copy_to_store .&. #CopyToStore{path, λstore_path. #Pure{store_path}}
              .|. #Pure{path.path}

// External interpreter handles CopyToStore effects
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Store interaction | Explicit, handled externally |
| Purity | Maintained in HVM4 |
| Complexity | Effect interpreter needed |

## Option C: Precomputed Store Paths

Pre-copy all referenced paths before HVM4 evaluation.

```hvm4
// During compilation, identify all path literals
// Copy them to store and substitute store paths
// No runtime store interaction needed

// Compilation phase:
// /foo/bar → "/nix/store/abc123-bar" with context
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Store interaction | At compile time only |
| Runtime | Pure |
| Limitation | Can't handle dynamic paths |

## CHOSEN: Pure Path Representation (Option A)

**Rationale:**
- Defer store operations to post-HVM4 phase (at evaluation boundary)
- Paths as tagged strings with accessor ID
- Store copying happens during result extraction, not during HVM4 evaluation
- Keeps HVM4 evaluation pure and deterministic
- Effect-based approach (Option B) can be added later for IFD support

**Encoding:**
```hvm4
// Path = #Pth{accessor_id, path_string}
// accessor_id = 0 for real filesystem, other IDs for virtual accessors

// Example: ./foo/bar.nix
#Pth{0, #Str{chars, #NoC{}}}  // chars = "foo/bar.nix"

// With base path (after resolution)
#Pth{0, #Str{"/home/user/project/foo/bar.nix", #NoC{}}}
```

### Detailed Implementation Steps

**New Files:**
- `src/libexpr/hvm4/hvm4-path.cc`
- `src/libexpr/include/nix/expr/hvm4/hvm4-path.hh`

#### Step 1: Define Encoding Constants and API

```cpp
// hvm4-path.hh
namespace nix::hvm4 {

// Constructor names (base-64 encoded)
constexpr uint32_t PATH_NODE = /* encode "#Pth" */;

// Create path from resolved path string
Term makePath(uint32_t accessorId, Term pathString, HVM4Runtime& runtime);

// Create path from SourcePath
Term makePathFromSource(const SourcePath& path, HVM4Runtime& runtime);

// Get accessor ID from path
uint32_t pathAccessorId(Term pathTerm, const HVM4Runtime& runtime);

// Get path string from path
Term pathString(Term pathTerm, const HVM4Runtime& runtime);

// Path concatenation (path + string suffix)
Term concatPath(Term path, Term suffix, HVM4Runtime& runtime);

// Check if term is our path encoding
bool isNixPath(Term term);

}  // namespace nix::hvm4
```

#### Step 2: Implement Construction Functions

```cpp
// hvm4-path.cc
Term makePath(uint32_t accessorId, Term pathString, HVM4Runtime& runtime) {
    Term args[2] = { runtime.makeNum(accessorId), pathString };
    return runtime.makeCtr(PATH_NODE, 2, args);
}

Term makePathFromSource(const SourcePath& path, HVM4Runtime& runtime) {
    // Resolve path to absolute string
    std::string resolved = path.abs().string();

    // Get accessor ID (0 for root filesystem)
    uint32_t accessorId = getAccessorId(path.accessor);

    // Create string (no context for raw path)
    Term pathStr = makeString(resolved, runtime);

    return makePath(accessorId, pathStr, runtime);
}

uint32_t pathAccessorId(Term pathTerm, const HVM4Runtime& runtime) {
    uint32_t loc = term_val(pathTerm);
    Term idTerm = runtime.getHeapAt(loc);
    return term_val(idTerm);
}

Term pathString(Term pathTerm, const HVM4Runtime& runtime) {
    uint32_t loc = term_val(pathTerm);
    return runtime.getHeapAt(loc + 1);
}
```

#### Step 3: Add ExprPath Support

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprPath*>(&expr)) {
    return true;  // Path literals always compile
}

// In emit():
if (auto* e = dynamic_cast<const ExprPath*>(&expr)) {
    return emitPath(*e, ctx);
}

Term HVM4Compiler::emitPath(const ExprPath& e, CompileContext& ctx) {
    // ExprPath contains a SourcePath
    return makePathFromSource(e.path, ctx.runtime());
}
```

#### Step 4: Path-to-String Coercion with Context

```cpp
// When path is coerced to string, it adds store path context
Term coercePathToString(Term path, CompileContext& ctx) {
    Term pathStr = pathString(path, ctx.runtime());

    // At this point, we're still in pure HVM4 evaluation
    // The actual store copy happens during result extraction

    // Create context element for this path
    // Context will be resolved when extracting result
    Term contextElem = makePathContext(path, ctx.runtime());
    Term context = makeContextSet(makeCons(contextElem,
                                   makeNil(ctx.runtime()), ctx.runtime()),
                                   ctx.runtime());

    // Return string with path context
    Term chars = stringChars(pathStr, ctx.runtime());
    Term args[2] = { chars, context };
    return ctx.runtime().makeCtr(STRING_WRAPPER, 2, args);
}
```

#### Step 5: Implement Result Extraction with Store Copy

```cpp
// In hvm4-result.cc:
void ResultExtractor::extractPath(Term term, Value& result) {
    uint32_t accessorId = pathAccessorId(term, runtime);
    Term pathStr = pathString(term, runtime);

    // Extract path string
    std::string pathContent = extractStringContent(pathStr);

    // Resolve accessor
    SourceAccessor* accessor = getAccessorById(accessorId);

    // Create SourcePath
    SourcePath sourcePath = SourcePath(accessor, pathContent);

    // Create Nix path value
    result.mkPath(sourcePath);
}

// When extracting string with path context, copy to store
void ResultExtractor::extractStringWithPathContext(Term term, Value& result) {
    Term chars = stringChars(term, runtime);
    Term ctx = stringContext(term, runtime);

    std::string content = extractCharList(chars);
    NixStringContext nixCtx;

    // Process context elements
    if (term_ext(ctx) == CONTEXT_SET) {
        Term elements = getContextElements(ctx, runtime);

        // For path contexts, copy to store and get store path
        processContextElements(elements, nixCtx, [&](Term elem) {
            if (isPathContext(elem)) {
                // Copy path to store
                SourcePath srcPath = extractSourcePath(elem);
                StorePath storePath = state.store->copyToStore(srcPath);

                // Add to context
                nixCtx.insert(NixStringContextElem::Opaque{storePath});

                return storePath.to_string();
            }
            return extractContextString(elem);
        });
    }

    result.mkStringWithContext(content, nixCtx);
}
```

#### Step 6: Add Comprehensive Tests

```cpp
// In hvm4.cc test file
TEST_F(HVM4BackendTest, PathLiteral) {
    // Need to set up a real path for testing
    auto v = eval("./.", true);
    ASSERT_EQ(v.type(), nPath);
}

TEST_F(HVM4BackendTest, PathInLet) {
    auto v = eval("let p = ./.; in p", true);
    ASSERT_EQ(v.type(), nPath);
}

TEST_F(HVM4BackendTest, PathCoerceToString) {
    // Path coercion adds context
    auto v = eval("\"${./.}\"", true);
    ASSERT_EQ(v.type(), nString);
    // Context should be non-empty (contains path reference)
}

TEST_F(HVM4BackendTest, PathConcat) {
    auto v = eval("./. + \"/foo\"", true);
    ASSERT_EQ(v.type(), nPath);
}

// === Additional Path Tests ===

TEST_F(HVM4BackendTest, PathConcatMultiple) {
    auto v = eval("./. + \"/foo\" + \"/bar\"", true);
    ASSERT_EQ(v.type(), nPath);
}

TEST_F(HVM4BackendTest, PathInAttrset) {
    auto v = eval("{ path = ./.; }", true);
    auto path = v.attrs()->get(state.symbols.create("path"));
    state.forceValue(*path->value, noPos);
    ASSERT_EQ(path->value->type(), nPath);
}

TEST_F(HVM4BackendTest, PathInList) {
    auto v = eval("[./. ./foo ./bar]", true);
    ASSERT_EQ(v.listSize(), 3);
    for (size_t i = 0; i < v.listSize(); i++) {
        state.forceValue(*v.listElems()[i], noPos);
        ASSERT_EQ(v.listElems()[i]->type(), nPath);
    }
}

TEST_F(HVM4BackendTest, PathEquality) {
    auto v = eval("./. == ./.", true);
    ASSERT_TRUE(v.boolean());
}

TEST_F(HVM4BackendTest, PathInequality) {
    auto v = eval("./foo == ./bar", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, PathAsFunction) {
    // Path can be passed to functions
    auto v = eval("(p: p) ./.", true);
    ASSERT_EQ(v.type(), nPath);
}

TEST_F(HVM4BackendTest, PathBaseNameDir) {
    // Using builtins with paths
    auto v = eval("builtins.baseNameOf ./foo/bar.nix", true);
    ASSERT_EQ(v.type(), nString);
    ASSERT_EQ(v.string_view(), "bar.nix");
}

TEST_F(HVM4BackendTest, PathDirOf) {
    auto v = eval("builtins.dirOf ./foo/bar.nix", true);
    ASSERT_EQ(v.type(), nPath);
}

TEST_F(HVM4BackendTest, PathToString) {
    // toString on path
    auto v = eval("builtins.toString ./.", true);
    ASSERT_EQ(v.type(), nString);
}

TEST_F(HVM4BackendTest, PathInterpolation) {
    // Path in string interpolation
    auto v = eval("\"prefix-${./.}-suffix\"", true);
    ASSERT_EQ(v.type(), nString);
    // Should have context from the path
}

TEST_F(HVM4BackendTest, PathLazy) {
    // Path should be lazy (not accessed until needed)
    auto v = eval("let p = ./nonexistent; in 42", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, PathExists) {
    auto v = eval("builtins.pathExists ./.", true);
    ASSERT_TRUE(v.boolean());
}
```

---

# 5. Recursive Let / Rec

Nix supports mutually recursive bindings with cycle detection.

## Nix Implementation Details

```cpp
// rec { a = ...; b = a + 1; }
// Creates environment where all bindings are visible

// Cycle detection via "black hole":
v.mkBlackhole();  // Mark as being evaluated
expr->eval(...);  // If we hit this value again → infinite recursion
```

## Option A: Y-Combinator Encoding

Encode mutual recursion via fixpoint combinators.

```hvm4
// Y combinator
@Y = λf. (λx. f(x(x))) (λx. f(x(x)))

// For: rec { a = b + 1; b = 10; }
// Encode as: Y(λself. { a = self.b + 1; b = 10; })

// Mutual recursion with record:
@rec_attrs = @Y(λself. #Attrs{
  #Attr{"a", self.b + 1, ...},
  #Attr{"b", 10, ...}
})
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Cycle detection | None - infinite loop on cycles |
| Implementation | Simple |
| Performance | Extra indirection |

## Option B: Lazy References with Blackhole

Mirror Nix's approach using explicit thunks and blackhole markers.

```hvm4
// Thunk = #Thunk{env, expr} | #Blackhole{} | #Evaluated{value}

@force = λthunk. λ{
  #Evaluated: λv. v
  #Blackhole: @error("infinite recursion")
  #Thunk: λenv.λexpr.
    // Would need mutable state to mark blackhole
    // Not directly expressible in pure HVM4
}
```

**Problem:** HVM4 is pure - can't mutate thunk to blackhole.

## Option C: Static Cycle Detection

Detect cycles at compile time via dependency analysis.

```hvm4
// During compilation:
// 1. Build dependency graph of rec bindings
// 2. Detect strongly connected components
// 3. Reject or specially handle cycles

// For non-cyclic rec:
// Topologically sort and compile as nested lets

// let b = 10; in let a = b + 1; in { a; b; }
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Cycle detection | At compile time |
| False positives | Rejects some valid Nix (lazy cycles) |
| Runtime | No overhead |

## Option D: Fuel-Based Evaluation

Add "fuel" parameter to bound recursion depth.

```hvm4
@eval_rec = λfuel.λenv.λexpr.
  (fuel == 0) .&. #Error{"recursion limit"} .|.
  // Evaluate with decremented fuel
  @eval_expr(fuel - 1, env, expr)
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Cycle detection | Eventually - via fuel exhaustion |
| False positives | May reject deep recursion |
| Configurability | Fuel limit tunable |

## CHOSEN: Static Topo-Sort + Y-Combinator Fallback (Options C + A)

**Rationale:**
- Most `rec` usages in real Nix code are acyclic (can be topologically sorted)
- Y-combinator handles true mutual recursion when needed
- Static analysis at compile time avoids runtime overhead for simple cases
- HVM4's optimal lazy evaluation works well with Y-combinator approach

**Strategy:**
1. Build dependency graph from rec bindings
2. Attempt topological sort
3. If acyclic: emit as nested lets in dependency order (fast path)
4. If cyclic: use Y-combinator to create fixpoint (correct but slower)

### Detailed Implementation Steps

**Files Modified:**
- `src/libexpr/hvm4/hvm4-compiler.cc` (add rec handling)

#### Step 1: Build Dependency Graph

```cpp
// In hvm4-compiler.cc
struct RecBindingInfo {
    Symbol name;
    const Expr* expr;
    std::set<Symbol> dependencies;  // Other rec bindings this depends on
};

std::vector<RecBindingInfo> analyzeRecBindings(const ExprAttrs& attrs,
                                                const SymbolTable& symbols) {
    std::vector<RecBindingInfo> bindings;

    for (auto& [name, attrDef] : attrs.attrs) {
        RecBindingInfo info;
        info.name = name;
        info.expr = attrDef.e;

        // Find free variables in the expression
        std::set<Symbol> freeVars;
        collectFreeVars(*attrDef.e, freeVars);

        // Filter to only other rec bindings
        for (auto& [otherName, _] : attrs.attrs) {
            if (freeVars.count(otherName) && otherName != name) {
                info.dependencies.insert(otherName);
            }
        }

        bindings.push_back(std::move(info));
    }

    return bindings;
}
```

#### Step 2: Attempt Topological Sort

```cpp
std::optional<std::vector<Symbol>> topologicalSort(
    const std::vector<RecBindingInfo>& bindings) {

    std::map<Symbol, int> inDegree;
    std::map<Symbol, std::vector<Symbol>> dependents;

    // Initialize
    for (const auto& b : bindings) {
        inDegree[b.name] = b.dependencies.size();
        for (const auto& dep : b.dependencies) {
            dependents[dep].push_back(b.name);
        }
    }

    // Kahn's algorithm
    std::queue<Symbol> ready;
    for (const auto& b : bindings) {
        if (inDegree[b.name] == 0) {
            ready.push(b.name);
        }
    }

    std::vector<Symbol> order;
    while (!ready.empty()) {
        Symbol current = ready.front();
        ready.pop();
        order.push_back(current);

        for (const auto& dependent : dependents[current]) {
            if (--inDegree[dependent] == 0) {
                ready.push(dependent);
            }
        }
    }

    // Check for cycles
    if (order.size() != bindings.size()) {
        return std::nullopt;  // Has cycles
    }

    return order;
}
```

#### Step 3: Emit Acyclic Rec as Nested Lets

```cpp
Term HVM4Compiler::emitAcyclicRec(const ExprAttrs& attrs,
                                   const std::vector<Symbol>& order,
                                   CompileContext& ctx) {
    // Build mapping from names to expressions
    std::map<Symbol, const Expr*> bindings;
    for (auto& [name, attrDef] : attrs.attrs) {
        bindings[name] = attrDef.e;
    }

    // Emit as nested lets in dependency order
    // let b = 10; in let a = b + 1; in { a; b; }

    // Start with innermost body (the attrset result)
    Term body = emitFinalAttrs(attrs, ctx);

    // Wrap with lets from back to front
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        Symbol name = *it;
        const Expr* expr = bindings[name];

        Term value = emit(*expr, ctx);
        body = emitLet(name, value, body, ctx);
    }

    return body;
}
```

#### Step 4: Emit Cyclic Rec with Y-Combinator

```cpp
// Y combinator definition (pre-compiled as @def)
// @Y = λf. (λx. f(x(x))) (λx. f(x(x)))

Term HVM4Compiler::emitCyclicRec(const ExprAttrs& attrs,
                                  CompileContext& ctx) {
    // Transform: rec { a = f(b); b = g(a); }
    // To: Y(λself. { a = f(self.b); b = g(self.a); })

    // Create lambda parameter for 'self'
    Symbol selfSym = ctx.symbols().create("$self");
    ctx.pushScope();
    ctx.addBinding(selfSym, makeSelfRef(ctx));

    // Build attrset body with self-references replaced by self.name lookups
    std::vector<std::pair<uint32_t, Term>> attrTerms;

    for (auto& [name, attrDef] : attrs.attrs) {
        // In this scope, references to other rec bindings become self.name
        Term value = emitWithSelfLookups(*attrDef.e, attrs, selfSym, ctx);
        attrTerms.push_back({name.id, value});
    }

    Term attrsBody = buildAttrsFromTerms(attrTerms, ctx);

    ctx.popScope();

    // Wrap in lambda
    Term recLambda = emitLambda(selfSym, attrsBody, ctx);

    // Apply Y combinator
    Term yCombinator = getYCombinatorRef(ctx);
    return emitApp(yCombinator, recLambda, ctx);
}

Term emitWithSelfLookups(const Expr& expr, const ExprAttrs& recAttrs,
                          Symbol selfSym, CompileContext& ctx) {
    // When we encounter a variable reference to another rec binding,
    // emit self.name instead of direct reference

    if (auto* var = dynamic_cast<const ExprVar*>(&expr)) {
        // Check if this variable is one of our rec bindings
        if (recAttrs.attrs.count(var->name)) {
            // Emit: self.name
            Term self = ctx.lookupBinding(selfSym);
            return emitSelect(self, var->name, ctx);
        }
    }

    // Recursively process subexpressions
    return emitDefault(expr, ctx);
}
```

#### Step 5: Add Support in canCompileWithScope

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprAttrs*>(&expr)) {
    if (e->recursive) {
        // Analyze dependency structure
        auto bindings = analyzeRecBindings(*e, symbols);
        auto order = topologicalSort(bindings);

        if (order) {
            // Acyclic - all bindings must be compilable
            for (auto& [name, attrDef] : e->attrs) {
                if (!canCompileWithScope(*attrDef.e, scope)) return false;
            }
            return true;
        } else {
            // Cyclic - need Y-combinator, still check bindings
            for (auto& [name, attrDef] : e->attrs) {
                if (!canCompileWithScope(*attrDef.e, scope)) return false;
            }
            return true;  // Y-combinator handles cycles
        }
    }
    // ... non-recursive attrs handling
}
```

#### Step 6: Add Comprehensive Tests

```cpp
// In hvm4.cc test file

// Acyclic rec (should use fast path)
TEST_F(HVM4BackendTest, RecAcyclic) {
    auto v = eval("rec { b = 10; a = b + 1; }", true);
    ASSERT_EQ(v.type(), nAttrs);
    auto a = v.attrs()->get(state.symbols.create("a"));
    state.forceValue(*a->value, noPos);
    ASSERT_EQ(a->value->integer().value, 11);
}

TEST_F(HVM4BackendTest, RecAcyclicChain) {
    auto v = eval("rec { c = b + 1; b = a + 1; a = 1; }", true);
    auto c = v.attrs()->get(state.symbols.create("c"));
    state.forceValue(*c->value, noPos);
    ASSERT_EQ(c->value->integer().value, 3);
}

// Cyclic rec (needs Y-combinator)
TEST_F(HVM4BackendTest, RecCyclicMutual) {
    // Mutual recursion: even/odd
    auto v = eval(R"(
        rec {
            even = n: if n == 0 then true else odd (n - 1);
            odd = n: if n == 0 then false else even (n - 1);
        }
    )", true);

    auto even = v.attrs()->get(state.symbols.create("even"));
    // Call even(4) - should be true
    // (Requires function call support)
}

TEST_F(HVM4BackendTest, RecSelfReference) {
    // Self-referential binding (lazy)
    auto v = eval(R"(
        rec {
            xs = [1 2 3] ++ xs;  # Infinite list
        }
    )", true);
    // Only valid because Nix is lazy - don't force xs fully
}

// Let rec equivalence
TEST_F(HVM4BackendTest, LetRecEquivalent) {
    auto v = eval("let a = b + 1; b = 10; in a", true);
    ASSERT_EQ(v.integer().value, 11);
}

// === Additional Recursive Let Edge Cases ===

TEST_F(HVM4BackendTest, RecMultipleDependencies) {
    auto v = eval("rec { sum = a + b + c; a = 1; b = 2; c = 3; }", true);
    auto sum = v.attrs()->get(state.symbols.create("sum"));
    state.forceValue(*sum->value, noPos);
    ASSERT_EQ(sum->value->integer().value, 6);
}

TEST_F(HVM4BackendTest, RecWithFunction) {
    auto v = eval(R"(
        rec {
            double = x: x * 2;
            result = double 21;
        }
    )", true);
    auto result = v.attrs()->get(state.symbols.create("result"));
    state.forceValue(*result->value, noPos);
    ASSERT_EQ(result->value->integer().value, 42);
}

TEST_F(HVM4BackendTest, RecFactorial) {
    auto v = eval(R"(
        rec {
            fact = n: if n <= 1 then 1 else n * fact (n - 1);
        }
    )", true);
    auto fact = v.attrs()->get(state.symbols.create("fact"));
    // Would need to call fact 5 and check result is 120
}

TEST_F(HVM4BackendTest, RecFibonacci) {
    auto v = eval(R"(
        rec {
            fib = n: if n <= 1 then n else fib (n - 1) + fib (n - 2);
        }
    )", true);
    // Self-recursive function
}

TEST_F(HVM4BackendTest, RecEmptyAttrs) {
    auto v = eval("rec { }", true);
    ASSERT_EQ(v.type(), nAttrs);
    ASSERT_EQ(v.attrs()->size(), 0);
}

TEST_F(HVM4BackendTest, RecSingleBinding) {
    auto v = eval("rec { x = 42; }", true);
    auto x = v.attrs()->get(state.symbols.create("x"));
    state.forceValue(*x->value, noPos);
    ASSERT_EQ(x->value->integer().value, 42);
}

TEST_F(HVM4BackendTest, RecLazyThunkNotForced) {
    // rec should create lazy thunks
    auto v = eval("rec { a = 1; b = throw \"lazy\"; }", true);
    auto a = v.attrs()->get(state.symbols.create("a"));
    state.forceValue(*a->value, noPos);
    ASSERT_EQ(a->value->integer().value, 1);  // Should not throw
}

TEST_F(HVM4BackendTest, RecWithInherit) {
    auto v = eval("let x = 1; in rec { inherit x; y = x + 1; }", true);
    auto y = v.attrs()->get(state.symbols.create("y"));
    state.forceValue(*y->value, noPos);
    ASSERT_EQ(y->value->integer().value, 2);
}

TEST_F(HVM4BackendTest, RecAccessDuringConstruction) {
    // Access a rec attr during construction of another
    auto v = eval("rec { list = [a b]; a = 1; b = 2; }", true);
    auto list = v.attrs()->get(state.symbols.create("list"));
    state.forceValue(*list->value, noPos);
    ASSERT_EQ(list->value->listSize(), 2);
}
```

---

# 6. With Expressions

`with e; body` adds all attributes of `e` to lexical scope dynamically.

## Nix Implementation Details

```cpp
// Cannot be resolved statically - requires runtime lookup
// Chain of 'with' expressions walked at runtime
Value* lookupVar(Env* env, const ExprVar& var) {
    if (var.fromWith) {
        // Walk 'with' chain until found
        forceAttrs(env->values[0]);
        if (auto j = env->values[0]->attrs()->get(var.name))
            return j->value;
        // Try parent 'with'...
    }
}
```

## Option A: Inline Expansion

At compile time, determine all possible variables and generate lookups.

```hvm4
// For: with attrs; x + y + z
// Compile to: attrs.x + attrs.y + attrs.z

// Problem: Can't know if x comes from 'with' or outer scope statically
// when 'with' shadows or there are nested 'with' expressions
```

**Limitation:** Only works for simple cases where all variables are known.

## Option B: Dynamic Lookup at Runtime

Pass the 'with' attrset and generate runtime lookups.

```hvm4
// Compile: with attrs; body
// To: let $with = attrs; in body'
// Where body' has variable references compiled to:
//   @lookup_with_chain(name, $with, outer_env)

@lookup_with_chain = λname.λwith_attrs.λouter.
  @lookup(name, with_attrs) .or. outer.name  // if outer has it

// Problem: "outer.name" requires knowing outer's structure
```

## Option C: Environment as First-Class Value

Pass entire environment as an attrset, 'with' merges attrsets.

```hvm4
// Environment = AttrSet of bindings
// with attrs; body  →  body evaluated with (env // attrs)

@eval_with = λenv.λattrs.λbody.
  @eval(env // attrs, body)

@eval_var = λenv.λname.
  @lookup(name, env)  // All lookups go through env attrset
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Semantics | Correct |
| Performance | Every var lookup is attrset lookup |
| Compilation | Must thread env through everything |

## Option D: Partial Evaluation + Fallback

Resolve what can be resolved statically, generate fallback for dynamic.

```hvm4
// Static analysis determines:
// - Variables definitely from lexical scope
// - Variables definitely from 'with'
// - Variables that could be either (need runtime check)

// For ambiguous: generate (hasAttr name with_attrs) ? with_attrs.name : lexical.name
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Performance | Good for unambiguous cases |
| Complexity | Medium |
| Correctness | Full |

## CHOSEN: Partial Evaluation + Runtime Lookup (Option D)

**Rationale:**
- Static analysis resolves unambiguous cases efficiently (most common)
- Runtime lookup only for truly dynamic/ambiguous access
- Preserves correct Nix semantics for all cases
- Good performance for typical code patterns

**Strategy:**
1. Static analysis to classify each variable reference:
   - **Definitely lexical**: bound in outer scope, emit normal VAR
   - **Definitely from with**: only exists in with attrset, emit attrs.name lookup
   - **Ambiguous**: could be either, emit runtime hasAttr check
2. Handle nested `with` expressions by chaining lookups

### Detailed Implementation Steps

**Files Modified:**
- `src/libexpr/hvm4/hvm4-compiler.cc` (add with handling)

#### Step 1: Variable Classification Analysis

```cpp
// In hvm4-compiler.cc
enum class VarSource {
    Lexical,      // Definitely from lexical scope
    FromWith,     // Definitely from with attrset
    Ambiguous     // Could be either - needs runtime check
};

struct WithAnalysisContext {
    std::set<Symbol> lexicalScope;     // Variables bound lexically
    std::vector<Term> withChain;       // Stack of with attrsets (innermost first)
    std::map<Symbol, std::set<Symbol>> withAttrs;  // Known attrs in each with (if static)
};

VarSource classifyVariable(Symbol name, const WithAnalysisContext& ctx) {
    bool inLexical = ctx.lexicalScope.count(name) > 0;

    // Check if any with could have this attribute
    bool possiblyInWith = false;
    for (const auto& withAttrs : ctx.withAttrs) {
        if (withAttrs.second.empty()) {
            // With attrset is dynamic - could contain anything
            possiblyInWith = true;
            break;
        }
        if (withAttrs.second.count(name)) {
            possiblyInWith = true;
            break;
        }
    }

    if (inLexical && !possiblyInWith) {
        return VarSource::Lexical;
    }
    if (!inLexical && possiblyInWith) {
        return VarSource::FromWith;
    }
    if (inLexical && possiblyInWith) {
        return VarSource::Ambiguous;
    }

    // Not found anywhere - will be an error
    return VarSource::Lexical;  // Let normal lookup handle error
}
```

#### Step 2: Emit Code Based on Classification

```cpp
Term HVM4Compiler::emitVarInWithContext(const ExprVar& var,
                                         const WithAnalysisContext& withCtx,
                                         CompileContext& ctx) {
    VarSource source = classifyVariable(var.name, withCtx);

    switch (source) {
        case VarSource::Lexical:
            // Normal lexical lookup
            return emitVar(var, ctx);

        case VarSource::FromWith:
            // Lookup in with chain (try innermost first)
            return emitWithChainLookup(var.name, withCtx.withChain, ctx);

        case VarSource::Ambiguous:
            // Runtime check: (hasAttr name with_attrs) ? with_attrs.name : lexical
            return emitAmbiguousLookup(var, withCtx, ctx);
    }
}

Term emitWithChainLookup(Symbol name, const std::vector<Term>& withChain,
                          CompileContext& ctx) {
    // Try each with in order (innermost first)
    // with a; with b; x  →  b.x or (a.x or error)

    if (withChain.empty()) {
        return emitLookupError(name, ctx);
    }

    // Start from outermost (end of chain) as base case
    Term result = emitLookupError(name, ctx);

    // Work inward, wrapping with hasAttr checks
    for (auto it = withChain.rbegin(); it != withChain.rend(); ++it) {
        Term withAttrs = *it;
        // (hasAttr name withAttrs) ? withAttrs.name : result
        Term lookup = emitSelect(withAttrs, name, ctx);
        Term hasAttr = emitHasAttr(withAttrs, name, ctx);
        result = emitIfThenElse(hasAttr, lookup, result, ctx);
    }

    return result;
}

Term emitAmbiguousLookup(const ExprVar& var,
                          const WithAnalysisContext& withCtx,
                          CompileContext& ctx) {
    // (hasAttr name with_chain) ? with_chain.name : lexical.name
    Term lexicalVal = emitVar(var, ctx);
    Term withVal = emitWithChainLookup(var.name, withCtx.withChain, ctx);

    // Check if any with has the attr
    Term anyWithHas = emitAnyWithHasAttr(var.name, withCtx.withChain, ctx);

    return emitIfThenElse(anyWithHas, withVal, lexicalVal, ctx);
}
```

#### Step 3: Handle Nested With Expressions

```cpp
Term HVM4Compiler::emitWith(const ExprWith& e, CompileContext& ctx) {
    // Compile the with attrset
    Term withAttrs = emit(*e.attrs, ctx);

    // Analyze the attrset if it's static (ExprAttrs)
    std::set<Symbol> staticAttrs;
    if (auto* attrs = dynamic_cast<const ExprAttrs*>(e.attrs)) {
        for (auto& [name, _] : attrs->attrs) {
            staticAttrs.insert(name);
        }
    }  // If not static, staticAttrs stays empty (means "unknown")

    // Push to with context
    WithAnalysisContext newCtx = ctx.withContext();
    newCtx.withChain.insert(newCtx.withChain.begin(), withAttrs);
    newCtx.withAttrs[ctx.freshWithId()] = staticAttrs;

    // Compile body with updated context
    ctx.pushWithContext(newCtx);
    Term body = emit(*e.body, ctx);
    ctx.popWithContext();

    return body;
}
```

#### Step 4: Add ExprWith to canCompileWithScope

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprWith*>(&expr)) {
    // Check both the with attrset and the body
    if (!canCompileWithScope(*e->attrs, scope)) return false;
    if (!canCompileWithScope(*e->body, scope)) return false;
    return true;
}
```

#### Step 5: Optimize Common Patterns

```cpp
// Optimization: If with attrset is a simple ExprVar, track it
Term HVM4Compiler::emitWithOptimized(const ExprWith& e, CompileContext& ctx) {
    // Common pattern: with pkgs; ...
    if (auto* varExpr = dynamic_cast<const ExprVar*>(e.attrs)) {
        // The with attrset is a variable - we can track its attrs
        // if we've seen it defined elsewhere
        Term withAttrs = emitVar(*varExpr, ctx);

        // If we know the static structure, use that for classification
        // Otherwise, all variables in body are Ambiguous
    }

    return emitWith(e, ctx);
}
```

#### Step 6: Add Comprehensive Tests

```cpp
// In hvm4.cc test file

TEST_F(HVM4BackendTest, WithSimple) {
    auto v = eval("with { x = 1; }; x", true);
    ASSERT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, WithMultipleAttrs) {
    auto v = eval("with { x = 1; y = 2; }; x + y", true);
    ASSERT_EQ(v.integer().value, 3);
}

TEST_F(HVM4BackendTest, WithShadowsLexical) {
    auto v = eval("let x = 1; in with { x = 2; }; x", true);
    ASSERT_EQ(v.integer().value, 2);  // with wins
}

TEST_F(HVM4BackendTest, WithLexicalFallback) {
    auto v = eval("let y = 1; in with { x = 2; }; x + y", true);
    ASSERT_EQ(v.integer().value, 3);  // x from with, y from lexical
}

TEST_F(HVM4BackendTest, WithNested) {
    auto v = eval("with {a=1;}; with {b=2;}; a + b", true);
    ASSERT_EQ(v.integer().value, 3);
}

TEST_F(HVM4BackendTest, WithNestedShadow) {
    auto v = eval("with {x=1;}; with {x=2;}; x", true);
    ASSERT_EQ(v.integer().value, 2);  // Inner with wins
}

TEST_F(HVM4BackendTest, WithDynamic) {
    // Dynamic with - can't know attrs statically
    auto v = eval("let attrs = {x = 1;}; in with attrs; x", true);
    ASSERT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, WithMissing) {
    // Missing attr should error
    EXPECT_THROW(eval("with { x = 1; }; y", true), EvalError);
}

// === Additional With Expression Edge Cases ===

TEST_F(HVM4BackendTest, WithEmptyAttrs) {
    // Empty with should just use outer scope
    auto v = eval("let x = 1; in with { }; x", true);
    ASSERT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, WithInFunction) {
    auto v = eval(R"(
        let f = attrs: with attrs; x + y;
        in f { x = 10; y = 20; }
    )", true);
    ASSERT_EQ(v.integer().value, 30);
}

TEST_F(HVM4BackendTest, WithRecAttrs) {
    // with a rec attrset
    auto v = eval("with rec { a = 1; b = a + 1; }; a + b", true);
    ASSERT_EQ(v.integer().value, 3);
}

TEST_F(HVM4BackendTest, WithDeeplyNested) {
    auto v = eval(R"(
        with { a = 1; };
        with { b = 2; };
        with { c = 3; };
        a + b + c
    )", true);
    ASSERT_EQ(v.integer().value, 6);
}

TEST_F(HVM4BackendTest, WithLexicalPreferred) {
    // When not shadowed, lexical scope preferred
    auto v = eval(R"(
        let x = 1; in
        let y = 2; in
        with { z = 3; }; x + y + z
    )", true);
    ASSERT_EQ(v.integer().value, 6);
}

TEST_F(HVM4BackendTest, WithChainOverride) {
    // Inner with overrides outer with
    auto v = eval("with { x = 1; }; with { x = 2; }; x", true);
    ASSERT_EQ(v.integer().value, 2);
}

TEST_F(HVM4BackendTest, WithInLet) {
    auto v = eval(R"(
        let
            attrs = { x = 10; y = 20; };
            result = with attrs; x * y;
        in result
    )", true);
    ASSERT_EQ(v.integer().value, 200);
}

TEST_F(HVM4BackendTest, WithInConditional) {
    auto v = eval(R"(
        if true
        then with { x = 1; }; x
        else with { x = 2; }; x
    )", true);
    ASSERT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, WithLazyEvaluation) {
    // with attrset should be lazy
    auto v = eval(R"(
        with { x = 1; y = throw "not used"; }; x
    )", true);
    ASSERT_EQ(v.integer().value, 1);  // y never forced
}

TEST_F(HVM4BackendTest, WithBuiltinsSimulation) {
    // Simulating common pattern: with builtins; ...
    auto v = eval(R"(
        let builtins = { add = a: b: a + b; mul = a: b: a * b; };
        in with builtins; mul (add 1 2) (add 3 4)
    )", true);
    ASSERT_EQ(v.integer().value, 21);
}
```

---

# 7. Imports

`import path` loads and evaluates a Nix file, with memoization.

## Nix Implementation Details

```cpp
// import is memoized in fileEvalCache
// scopedImport is NOT memoized
// Import From Derivation (IFD) requires building derivations during eval
```

## Option A: Pre-Import Resolution

Resolve all imports before HVM4 compilation.

```hvm4
// 1. Parse main file, collect all import paths
// 2. Recursively parse imported files
// 3. Inline imported expressions into single AST
// 4. Compile combined AST to HVM4

// No import at HVM4 runtime - all resolved at compile time
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| IFD | Not supported |
| Dynamic imports | Not supported |
| Memoization | Implicit via inlining |
| Compilation | May be slow for large closures |

## Option B: Module System

Compile each file to named HVM4 definitions, resolve at load time.

```hvm4
// file: foo.nix → @foo_nix_main = ...
// import ./foo.nix → @foo_nix_main

// Build dependency graph, load all modules, then evaluate
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Memoization | Natural - each file is one @def |
| Dynamic imports | Not supported |
| Modularity | Good |

## Option C: Effect-Based Import

Model import as an effect.

```hvm4
// Import = #Import{path, continuation}

@eval_import = λpath. #Import{path, λcontent. @eval(@parse(content))}

// External interpreter:
// 1. Receives Import effect
// 2. Reads file (with memoization)
// 3. Parses and compiles to HVM4
// 4. Continues with compiled term
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| IFD | Possible with external handler |
| Dynamic imports | Supported |
| Complexity | Effect system needed |
| Memoization | In effect handler |

## Option D: Two-Phase Evaluation

Phase 1: Evaluate to collect import requests. Phase 2: Resolve and re-evaluate.

```hvm4
// Phase 1: Return set of required imports
@collect_imports = λexpr. ...  // Returns list of paths

// Phase 2: With imports resolved, full evaluation
@eval_with_imports = λimport_map.λexpr. ...
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| IFD | Limited - no nested IFD |
| Iterations | Multiple passes |

## CHOSEN: Pre-Import Resolution (Phase 1: Option A)

**Rationale:**
- Simplest approach that works for most use cases
- No runtime import handling needed in HVM4
- Natural memoization through AST deduplication
- Effect-based approach (Option C) can be added later for IFD

**Strategy:**
1. Parse main expression and collect all static import paths
2. Recursively parse and compile imported files
3. Build combined AST with imports resolved
4. Compile complete AST to HVM4

**Limitations:**
- Dynamic import paths not supported (e.g., `import (./. + filename)`)
- Import From Derivation (IFD) not supported in Phase 1
- Expressions must fall back to standard evaluator if they contain dynamic imports

### Detailed Implementation Steps

**Files Modified:**
- `src/libexpr/hvm4/hvm4-compiler.cc` (import resolution)
- `src/libexpr/hvm4/hvm4-import-resolver.cc` (new file)

#### Step 1: Collect Import Paths

```cpp
// hvm4-import-resolver.cc
class ImportResolver {
public:
    struct ImportInfo {
        SourcePath path;
        Expr* resolvedExpr;
    };

    std::vector<SourcePath> collectImports(Expr* expr) {
        std::vector<SourcePath> imports;
        collectImportsRecursive(expr, imports);
        return imports;
    }

private:
    void collectImportsRecursive(Expr* expr, std::vector<SourcePath>& imports) {
        if (auto* call = dynamic_cast<ExprCall*>(expr)) {
            if (isImportBuiltin(call->fun)) {
                if (auto* pathExpr = dynamic_cast<ExprPath*>(call->args[0])) {
                    imports.push_back(pathExpr->path);
                } else {
                    // Dynamic import - not supported
                    throw CompileError("dynamic imports not supported in HVM4 backend");
                }
            }
        }
        // Recurse into children
        for (auto* child : expr->children()) {
            collectImportsRecursive(child, imports);
        }
    }
};
```

#### Step 2: Resolve and Compile Imports

```cpp
std::map<SourcePath, Term> resolveAllImports(
    Expr* mainExpr,
    EvalState& state,
    HVM4Compiler& compiler) {

    std::map<SourcePath, Term> resolved;
    std::set<SourcePath> pending;
    std::set<SourcePath> processing;  // For cycle detection

    ImportResolver resolver;
    auto imports = resolver.collectImports(mainExpr);

    for (const auto& path : imports) {
        pending.insert(path);
    }

    while (!pending.empty()) {
        SourcePath path = *pending.begin();
        pending.erase(pending.begin());

        if (resolved.count(path)) continue;
        if (processing.count(path)) {
            throw CompileError("circular import detected: " + path.to_string());
        }

        processing.insert(path);

        // Parse the file
        Expr* importedExpr = state.parseExprFromFile(path);

        // Collect nested imports
        auto nestedImports = resolver.collectImports(importedExpr);
        for (const auto& nested : nestedImports) {
            if (!resolved.count(nested)) {
                pending.insert(nested);
            }
        }

        // Compile to HVM4
        Term compiled = compiler.compile(*importedExpr);
        resolved[path] = compiled;

        processing.erase(path);
    }

    return resolved;
}
```

#### Step 3: Replace Import Calls with Resolved Terms

```cpp
Term HVM4Compiler::emitImport(const ExprCall& call, CompileContext& ctx) {
    // Import should have been resolved during pre-processing
    auto* pathExpr = dynamic_cast<ExprPath*>(call.args[0]);
    if (!pathExpr) {
        throw CompileError("import requires static path");
    }

    auto it = ctx.resolvedImports.find(pathExpr->path);
    if (it == ctx.resolvedImports.end()) {
        throw CompileError("import not resolved: " + pathExpr->path.to_string());
    }

    return it->second;
}
```

#### Step 4: Add Tests

```cpp
// In hvm4.cc test file

TEST_F(HVM4BackendTest, ImportSimple) {
    // Would need test file infrastructure
    // For now, test that import detection works
}

TEST_F(HVM4BackendTest, ImportCircularDetection) {
    // Circular imports should be detected
}

TEST_F(HVM4BackendTest, ImportNested) {
    // A imports B imports C should all be resolved
}

TEST_F(HVM4BackendTest, ImportDynamicRejected) {
    // Dynamic imports should fail compilation
    EXPECT_THROW(
        canCompile("import (./. + \"/foo.nix\")", true),
        CompileError
    );
}

TEST_F(HVM4BackendTest, ImportMemoization) {
    // Same file imported twice should use same term
}
```

**Related Features:**
- Uses [Paths](#4-paths) for path resolution
- Related to [Derivations](#8-derivations) for IFD (future work)

---

# 8. Derivations

Derivations are the core of Nix - they define build actions.

## Nix Implementation Details

```cpp
// derivationStrict:
// 1. Collect all attributes
// 2. Coerce to strings (accumulating context)
// 3. Process context → inputDrvs, inputSrcs
// 4. Compute output paths based on derivation type
// 5. Write .drv file to store
// 6. Return attrset with drvPath, out, etc.
```

## Option A: Pure Derivation Records

Derivations as pure data structures, no store writes during eval.

```hvm4
// Derivation = #Drv{name, builder, args, env, outputs, inputDrvs, inputSrcs}

@derivation_strict = λattrs.
  // Collect and validate attributes
  // Return Drv record, don't write to store
  #Drv{
    name: @get_attr("name", attrs),
    builder: @get_attr("builder", attrs),
    // ...
  }

// Store writing happens after HVM4 evaluation completes
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Purity | Preserved |
| IFD | Not supported (no store paths during eval) |
| Post-processing | Need to traverse result for Drv records |

## Option B: Effect-Based Derivations

Model derivation creation as an effect.

```hvm4
// Effect = #CreateDrv{drv_attrs, continuation}

@derivation_strict = λattrs.
  #CreateDrv{attrs, λdrv_result. drv_result}

// External handler:
// 1. Processes attrs, writes .drv
// 2. Returns attrset with paths
// 3. Continues evaluation
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| IFD | Supported via effect handler |
| Complexity | Effect system required |
| Semantics | Matches Nix |

## Option C: Staged Compilation

Separate derivation computation from evaluation.

```hvm4
// Stage 1: Identify derivation calls, extract to separate phase
// Stage 2: Compute all derivations (in Nix/external)
// Stage 3: Substitute results and continue HVM4 evaluation

// Requires multiple passes but keeps HVM4 pure
```

## CHOSEN: Pure Derivation Records (Phase 1: Option A)

**Rationale:**
- Keeps HVM4 evaluation pure and deterministic
- Derivation records can be collected and processed after evaluation
- Store writes happen in a single post-processing phase
- Effect-based approach (Option B) can be added later for IFD

**Strategy:**
1. Compile `derivationStrict` to create pure `#Drv{...}` records
2. HVM4 evaluates without side effects
3. After evaluation, traverse result to find all Drv records
4. Write .drv files to store and update references

**Encoding:**
```hvm4
// Derivation = #Drv{name, system, builder, args, env, outputs}
// name: String
// system: String
// builder: String (path to builder)
// args: List of Strings
// env: AttrSet of Strings
// outputs: List of output names

#Drv{
  #Str{"hello", #NoC{}},           // name
  #Str{"x86_64-linux", #NoC{}},    // system
  #Str{"/bin/sh", #NoC{}},         // builder
  #Lst{2, #Con{"-c", #Con{"echo hello", #Nil{}}}},  // args
  #ABs{...},                        // env
  #Lst{1, #Con{"out", #Nil{}}}     // outputs
}
```

### Detailed Implementation Steps

**New Files:**
- `src/libexpr/hvm4/hvm4-derivation.cc`
- `src/libexpr/include/nix/expr/hvm4/hvm4-derivation.hh`

#### Step 1: Define Derivation Encoding

```cpp
// hvm4-derivation.hh
namespace nix::hvm4 {

// Constructor for derivation record
constexpr uint32_t DRV_RECORD = /* encode "#Drv" */;

// Create a derivation record from attributes
Term makeDerivationRecord(
    Term name,
    Term system,
    Term builder,
    Term args,
    Term env,
    Term outputs,
    HVM4Runtime& runtime);

// Check if term is a derivation record
bool isDerivationRecord(Term term);

// Extract derivation fields
Term getDrvName(Term drv, const HVM4Runtime& runtime);
Term getDrvSystem(Term drv, const HVM4Runtime& runtime);
// ... etc

}  // namespace nix::hvm4
```

#### Step 2: Compile derivationStrict

```cpp
Term HVM4Compiler::emitDerivationStrict(const ExprCall& call, CompileContext& ctx) {
    // derivationStrict takes one argument: the attribute set
    Term attrsArg = emit(*call.args[0], ctx);

    // Extract required attributes
    Term name = emitGetAttr(attrsArg, "name", ctx);
    Term system = emitGetAttrOr(attrsArg, "system",
                                 makeString(settings.thisSystem, ctx.runtime()), ctx);
    Term builder = emitGetAttr(attrsArg, "builder", ctx);
    Term args = emitGetAttrOr(attrsArg, "args", makeEmptyList(ctx.runtime()), ctx);
    Term outputs = emitGetAttrOr(attrsArg, "outputs",
                                  makeSingletonList(makeString("out", ctx.runtime()),
                                                    ctx.runtime()), ctx);

    // Create derivation record (env is the whole attrset minus special attrs)
    return makeDerivationRecord(name, system, builder, args, attrsArg, outputs,
                                ctx.runtime());
}
```

#### Step 3: Post-Evaluation Processing

```cpp
// In hvm4-result.cc
class DerivationProcessor {
public:
    struct PendingDrv {
        Term drvTerm;
        std::string name;
        // ... other fields
    };

    std::vector<PendingDrv> collectDerivations(Term result, HVM4Runtime& runtime) {
        std::vector<PendingDrv> drvs;
        collectRecursive(result, drvs, runtime);
        return drvs;
    }

    void processDerivations(std::vector<PendingDrv>& drvs,
                            Store& store,
                            HVM4Runtime& runtime) {
        for (auto& drv : drvs) {
            // Build Derivation object
            Derivation d;
            d.name = extractString(getDrvName(drv.drvTerm, runtime));
            // ... populate other fields

            // Write to store
            auto drvPath = store.writeDerivation(d);

            // Update term to include drvPath
            // (This is tricky - may need to return mapping instead)
        }
    }

private:
    void collectRecursive(Term term, std::vector<PendingDrv>& drvs,
                          HVM4Runtime& runtime) {
        if (isDerivationRecord(term)) {
            PendingDrv pending;
            pending.drvTerm = term;
            pending.name = extractString(getDrvName(term, runtime));
            drvs.push_back(pending);
        }

        // Recurse into children (attrs, lists, etc.)
        // Be careful not to force thunks unnecessarily
    }
};
```

#### Step 4: Add Tests

```cpp
// In hvm4.cc test file

TEST_F(HVM4BackendTest, DerivationBasic) {
    auto v = eval(R"(
        derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
        }
    )", true);

    // Should return attrset with type = "derivation"
    ASSERT_EQ(v.type(), nAttrs);
    auto type = v.attrs()->get(state.symbols.create("type"));
    state.forceValue(*type->value, noPos);
    ASSERT_EQ(type->value->string_view(), "derivation");
}

TEST_F(HVM4BackendTest, DerivationWithArgs) {
    auto v = eval(R"(
        derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
            args = ["-c" "echo hello"];
        }
    )", true);
    ASSERT_EQ(v.type(), nAttrs);
}

TEST_F(HVM4BackendTest, DerivationOutputs) {
    auto v = eval(R"(
        let drv = derivation {
            name = "multi-output";
            system = "x86_64-linux";
            builder = "/bin/sh";
            outputs = ["out" "dev" "doc"];
        };
        in drv.dev
    )", true);
    // Should access the dev output
}

TEST_F(HVM4BackendTest, DerivationContext) {
    // Derivation should add context to dependent strings
    auto v = eval(R"(
        let drv = derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
        };
        in "${drv}"
    )", true);
    ASSERT_EQ(v.type(), nString);
    // Should have context containing drv reference
}

TEST_F(HVM4BackendTest, DerivationLazy) {
    // Derivation should be lazy
    auto v = eval(R"(
        let drv = derivation {
            name = "never-used";
            system = throw "not evaluated";
            builder = "/bin/sh";
        };
        in 42
    )", true);
    ASSERT_EQ(v.integer().value, 42);  // Should not throw
}
```

**Related Features:**
- Uses [Strings](#3-strings) with context for store path references
- Related to [Imports](#7-imports) for IFD (Import From Derivation)
- Uses [Attribute Sets](#1-attribute-sets) for derivation attributes

---

# 9. Arithmetic Primops

`-`, `*`, `/` are primops in Nix, not syntax.

## Nix Implementation Details

```cpp
// builtins.sub, builtins.mul, builtins.div
// Take two arguments, force to numbers, compute
// Handle int/float coercion
```

## Option A: Direct HVM4 OP2

Map to HVM4's built-in operators.

```hvm4
// Already available:
// OP_SUB (1), OP_MUL (2), OP_DIV (3)

@builtin_sub = λa.λb. (a - b)
@builtin_mul = λa.λb. (a * b)
@builtin_div = λa.λb. (a / b)  // Integer division in HVM4

// Division by zero: HVM4 behavior (likely crash)
```

**Issues:**
- HVM4 has 32-bit integers, Nix has 64-bit
- Integer division semantics may differ
- Need to handle BigInt encoding

## Option B: BigInt-Aware Operations

Implement operations that work with BigInt encoding.

```hvm4
// Recall: BigInt = #Pos{lo, hi} | #Neg{lo, hi}

@bigint_sub = λa.λb.
  @is_small(a) .&. @is_small(b) .&. @small_sub(a, b) .|.
  @full_bigint_sub(a, b)

@full_bigint_sub = λa.λb.
  // Convert to 64-bit representation
  // Perform subtraction
  // Re-encode result
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Correctness | Full 64-bit support |
| Performance | Slower for large numbers |
| Complexity | Medium |

## CHOSEN: Direct OP2 with BigInt Fast Path (Option B)

**Rationale:**
- HVM4 has native OP_ADD, OP_SUB, OP_MUL, OP_DIV, and comparison operators
- BigInt encoding (already implemented) handles 64-bit overflow
- Fast path for common case (32-bit values that fit in NUM)
- Slow path for large numbers using BigInt encoding

**Encoding:**
```hvm4
// Small integers: raw NUM (32-bit signed)
// Large integers: #Pos{lo, hi} or #Neg{lo, hi}

// Fast path: use native OP2 when both are NUM
// Slow path: BigInt arithmetic for overflow protection
```

### Detailed Implementation Steps

**Files Modified:**
- `src/libexpr/hvm4/hvm4-compiler.cc` (detect arithmetic primop calls)
- `src/libexpr/hvm4/hvm4-bigint.cc` (BigInt operations, already exists)

#### Step 1: Detect Primop Calls at Compile Time

```cpp
// In hvm4-compiler.cc
// Arithmetic primops: __sub, __mul, __div, __lessThan, etc.

bool isArithmeticPrimop(const Expr& fun, Symbol& primopSym) {
    if (auto* var = dynamic_cast<const ExprVar*>(&fun)) {
        std::string name = var->name.c_str();
        if (name == "__sub" || name == "__mul" || name == "__div" ||
            name == "__lessThan" || name == "__add") {
            primopSym = var->name;
            return true;
        }
    }
    return false;
}

// In canCompileWithScope for ExprCall:
if (auto* e = dynamic_cast<const ExprCall*>(&expr)) {
    Symbol primopSym;
    if (isArithmeticPrimop(*e->fun, primopSym)) {
        // Check arguments are compilable
        for (auto* arg : e->args) {
            if (!canCompileWithScope(*arg, scope)) return false;
        }
        return true;
    }
    // ... other call handling
}
```

#### Step 2: Add Primop Symbols to Whitelist

```cpp
// In HVM4Compiler constructor or init:
void initArithmeticPrimops(SymbolTable& symbols) {
    arithmeticPrimops = {
        symbols.create("__add"),
        symbols.create("__sub"),
        symbols.create("__mul"),
        symbols.create("__div"),
        symbols.create("__lessThan"),
        symbols.create("__le"),
        symbols.create("__negate"),
    };
}
```

#### Step 3: Emit OP2 Terms for Small Integers

```cpp
Term HVM4Compiler::emitArithmeticPrimop(Symbol primop,
                                         const std::vector<Expr*>& args,
                                         CompileContext& ctx) {
    if (args.size() != 2) {
        throw CompileError("arithmetic primop requires 2 arguments");
    }

    Term left = emit(*args[0], ctx);
    Term right = emit(*args[1], ctx);

    // Map primop to HVM4 OP2 code
    uint32_t opCode = getOpCode(primop);

    // Generate: both small ? OP2(left, right) : bigint_op(left, right)
    return emitArithmeticWithOverflow(opCode, left, right, ctx);
}

uint32_t getOpCode(Symbol primop) {
    std::string name = primop.c_str();
    if (name == "__add") return OP_ADD;  // 0
    if (name == "__sub") return OP_SUB;  // 1
    if (name == "__mul") return OP_MUL;  // 2
    if (name == "__div") return OP_DIV;  // 3
    if (name == "__lessThan") return OP_LT;   // comparison ops
    if (name == "__le") return OP_LE;
    throw CompileError("unknown arithmetic primop");
}
```

#### Step 4: Generate BigInt-Aware Code

```cpp
Term emitArithmeticWithOverflow(uint32_t opCode, Term left, Term right,
                                 CompileContext& ctx) {
    // For simple case where we know both are small at compile time:
    if (isSmallIntLiteral(left) && isSmallIntLiteral(right)) {
        // Just use OP2 directly
        return ctx.runtime().makeOp2(opCode, left, right);
    }

    // General case: runtime check
    // Generate:
    //   let l = left in
    //   let r = right in
    //   if (isSmall(l) && isSmall(r))
    //     then checkOverflow(OP2(l, r))
    //     else bigint_op(l, r)

    Term lVar = emitLet(ctx.freshSym(), left, ctx);
    Term rVar = emitLet(ctx.freshSym(), right, ctx);

    Term isSmallL = emitIsSmallInt(lVar, ctx);
    Term isSmallR = emitIsSmallInt(rVar, ctx);
    Term bothSmall = emitAnd(isSmallL, isSmallR, ctx);

    Term fastPath = emitOp2WithOverflowCheck(opCode, lVar, rVar, ctx);
    Term slowPath = emitBigIntOp(opCode, lVar, rVar, ctx);

    return emitIfThenElse(bothSmall, fastPath, slowPath, ctx);
}

Term emitOp2WithOverflowCheck(uint32_t opCode, Term left, Term right,
                               CompileContext& ctx) {
    // For add/sub/mul, check if result overflows 32-bit
    Term result = ctx.runtime().makeOp2(opCode, left, right);

    if (opCode == OP_ADD || opCode == OP_SUB || opCode == OP_MUL) {
        // Check overflow and convert to BigInt if needed
        return emitOverflowCheck(result, ctx);
    }

    // Division can't overflow (result smaller than inputs)
    return result;
}
```

#### Step 5: Handle Division by Zero

```cpp
Term emitDivision(Term left, Term right, CompileContext& ctx) {
    // Check for division by zero - match Nix semantics (throw error)
    Term isZero = emitEqual(right, makeSmallInt(0, ctx.runtime()), ctx);
    Term error = emitThrow("division by zero", ctx);
    Term divResult = emitArithmeticWithOverflow(OP_DIV, left, right, ctx);

    return emitIfThenElse(isZero, error, divResult, ctx);
}
```

#### Step 6: Add Comparison Primops

```cpp
// Comparisons return boolean, no overflow possible
Term emitComparisonPrimop(uint32_t opCode, Term left, Term right,
                           CompileContext& ctx) {
    // For comparisons, need to handle BigInt case specially
    Term isSmallL = emitIsSmallInt(left, ctx);
    Term isSmallR = emitIsSmallInt(right, ctx);
    Term bothSmall = emitAnd(isSmallL, isSmallR, ctx);

    // Small comparison: direct OP2
    Term fastPath = ctx.runtime().makeOp2(opCode, left, right);

    // BigInt comparison: compare hi first, then lo
    Term slowPath = emitBigIntCompare(opCode, left, right, ctx);

    Term result = emitIfThenElse(bothSmall, fastPath, slowPath, ctx);

    // Convert to Nix boolean
    return emitNumToBool(result, ctx);
}
```

#### Step 7: Add Comprehensive Tests

```cpp
// In hvm4.cc test file

// Basic arithmetic
TEST_F(HVM4BackendTest, ArithmeticAdd) {
    auto v = eval("1 + 2", true);
    ASSERT_EQ(v.integer().value, 3);
}

TEST_F(HVM4BackendTest, ArithmeticSub) {
    auto v = eval("5 - 3", true);
    ASSERT_EQ(v.integer().value, 2);
}

TEST_F(HVM4BackendTest, ArithmeticMul) {
    auto v = eval("4 * 5", true);
    ASSERT_EQ(v.integer().value, 20);
}

TEST_F(HVM4BackendTest, ArithmeticDiv) {
    auto v = eval("10 / 3", true);
    ASSERT_EQ(v.integer().value, 3);  // Integer division
}

TEST_F(HVM4BackendTest, ArithmeticNegative) {
    auto v = eval("3 - 5", true);
    ASSERT_EQ(v.integer().value, -2);
}

// Comparison operators
TEST_F(HVM4BackendTest, CompareLessThan) {
    auto v = eval("1 < 2", true);
    ASSERT_TRUE(v.boolean());

    v = eval("2 < 1", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, CompareLessEqual) {
    auto v = eval("2 <= 2", true);
    ASSERT_TRUE(v.boolean());
}

// BigInt overflow
TEST_F(HVM4BackendTest, ArithmeticBigInt) {
    // Values that overflow 32-bit
    auto v = eval("2147483647 + 1", true);  // INT32_MAX + 1
    ASSERT_EQ(v.integer().value, 2147483648L);
}

TEST_F(HVM4BackendTest, ArithmeticBigIntMul) {
    auto v = eval("100000 * 100000", true);  // 10^10
    ASSERT_EQ(v.integer().value, 10000000000L);
}

// Division by zero
TEST_F(HVM4BackendTest, DivisionByZero) {
    EXPECT_THROW(eval("1 / 0", true), EvalError);
}

// Complex expressions
TEST_F(HVM4BackendTest, ArithmeticComplex) {
    auto v = eval("(1 + 2) * (3 + 4)", true);
    ASSERT_EQ(v.integer().value, 21);
}

TEST_F(HVM4BackendTest, ArithmeticInLet) {
    auto v = eval("let x = 5; y = 3; in x * y + x - y", true);
    ASSERT_EQ(v.integer().value, 17);
}

// === Additional Edge Cases ===

TEST_F(HVM4BackendTest, ArithmeticNegation) {
    auto v = eval("0 - 42", true);
    ASSERT_EQ(v.integer().value, -42);
}

TEST_F(HVM4BackendTest, ArithmeticNegativeNegative) {
    auto v = eval("(-5) * (-3)", true);
    ASSERT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, ArithmeticZeroMul) {
    auto v = eval("0 * 999999", true);
    ASSERT_EQ(v.integer().value, 0);
}

TEST_F(HVM4BackendTest, ArithmeticDivTruncation) {
    auto v = eval("7 / 2", true);
    ASSERT_EQ(v.integer().value, 3);  // Truncates toward zero

    v = eval("(-7) / 2", true);
    ASSERT_EQ(v.integer().value, -3);  // Nix truncates toward zero
}

TEST_F(HVM4BackendTest, ArithmeticChainedOps) {
    auto v = eval("1 + 2 + 3 + 4 + 5", true);
    ASSERT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, ArithmeticPrecedence) {
    auto v = eval("2 + 3 * 4", true);
    ASSERT_EQ(v.integer().value, 14);  // Mul before add

    v = eval("(2 + 3) * 4", true);
    ASSERT_EQ(v.integer().value, 20);  // Parens override
}

// === Comparison Edge Cases ===

TEST_F(HVM4BackendTest, CompareEqual) {
    auto v = eval("5 == 5", true);
    ASSERT_TRUE(v.boolean());

    v = eval("5 == 6", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, CompareNotEqual) {
    auto v = eval("5 != 6", true);
    ASSERT_TRUE(v.boolean());

    v = eval("5 != 5", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, CompareGreaterThan) {
    auto v = eval("5 > 3", true);
    ASSERT_TRUE(v.boolean());

    v = eval("3 > 5", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, CompareGreaterEqual) {
    auto v = eval("5 >= 5", true);
    ASSERT_TRUE(v.boolean());

    v = eval("4 >= 5", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, CompareNegatives) {
    auto v = eval("(-5) < (-3)", true);
    ASSERT_TRUE(v.boolean());

    v = eval("(-3) < (-5)", true);
    ASSERT_FALSE(v.boolean());
}

// === BigInt Edge Cases ===

TEST_F(HVM4BackendTest, BigIntNegative) {
    auto v = eval("0 - 2147483648", true);  // -INT32_MIN
    ASSERT_EQ(v.integer().value, -2147483648L);
}

TEST_F(HVM4BackendTest, BigIntComparison) {
    auto v = eval("2147483648 < 2147483649", true);
    ASSERT_TRUE(v.boolean());
}

TEST_F(HVM4BackendTest, BigIntDivision) {
    auto v = eval("10000000000 / 3", true);
    ASSERT_EQ(v.integer().value, 3333333333L);
}

// === Arithmetic in Conditionals ===

TEST_F(HVM4BackendTest, ArithmeticInIf) {
    auto v = eval("if 3 > 2 then 10 + 5 else 10 - 5", true);
    ASSERT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, ArithmeticLazyBranch) {
    // Non-taken branch should not evaluate
    auto v = eval("if true then 42 else 1/0", true);
    ASSERT_EQ(v.integer().value, 42);
}
```

---

# 10. Other Primops

Nix has ~100+ primops. Key categories:

## Type Checking Primops

Type checking in HVM4 uses pattern matching on the actual data constructors:

```hvm4
// Types are distinguished by their constructors:
// - Integers: NUM (small) or #Pos/#Neg (BigInt)
// - Booleans: #Tru{} or #Fls{} (or 0/1 as NUM)
// - Strings: #Str{chars, context}
// - Lists: #Lst{length, spine}
// - Attrs: #ABs{list} or #ALy{overlay, base}
// - Paths: #Pth{accessor, path}
// - Functions: LAM

@isString = λv. λ{#Str: λ_.λ_. 1; _: 0}(v)
@isList = λv. λ{#Lst: λ_.λ_. 1; _: 0}(v)
@isAttrs = λv. λ{#ABs: λ_. 1; #ALy: λ_.λ_. 1; _: 0}(v)
@isPath = λv. λ{#Pth: λ_.λ_. 1; _: 0}(v)
// Note: isInt, isBool, isFunction require special handling
```

## List Primops

See Section 2. Most map naturally to HVM4.

```hvm4
@builtins_map = @map
@builtins_filter = @filter
@builtins_foldl = λf.λz.λlist. ...  // Strict fold
@builtins_length = @length
@builtins_head = @head
@builtins_tail = @tail
@builtins_elemAt = @elemAt
@builtins_concatLists = λlists. @foldl(@concat, #Nil{}, lists)
```

## Attribute Set Primops

```hvm4
@builtins_attrNames = λattrs. @map(λattr. attr.name, @to_list(attrs))
@builtins_attrValues = λattrs. @map(λattr. attr.value, @to_list(attrs))
@builtins_hasAttr = λname.λattrs. @is_some(@lookup(name, attrs))
@builtins_getAttr = λname.λattrs.
  @lookup(name, attrs) .or_else. @error("attribute not found")
@builtins_removeAttrs = λattrs.λnames. ...
@builtins_intersectAttrs = λa.λb. ...
```

## String Primops

```hvm4
@builtins_stringLength = @str_length
@builtins_substring = λstart.λlen.λs. @substring(start, len, s.string)
@builtins_toString = λv. @coerce_to_string(v)
@builtins_hashString = λtype.λs. ...  // Would need hash implementation
```

## Type Coercion Rules

Nix has specific type coercion rules that MUST be preserved in the HVM4 backend. These rules determine how values are converted between types implicitly and explicitly.

### String Coercion

The `toString` and string interpolation coercions follow these rules:

```hvm4
@coerce_to_string = λv. λ{
  // Already a string - return as-is (preserving context)
  #Str: λchars.λctx. #Str{chars, ctx}

  // Integer - convert to decimal string
  #Pos: λlo.λhi. @int_to_str(@bigint_to_int(#Pos{lo, hi}))
  #Neg: λlo.λhi. @str_concat(@make_string("-"), @int_to_str(@bigint_abs(#Neg{lo, hi})))

  // For small integers (NUM):
  // This case would be caught by @isSmallInt check

  // Boolean - "true" or "false"
  #Tru: @make_string("true")
  #Fls: @make_string("false")

  // Null - empty string
  #Nul: @make_string("")

  // Path - convert to store path (adds context!)
  #Pth: λacc.λpath. @coerce_path_to_string(#Pth{acc, path})

  // List - error (can't coerce list to string)
  #Lst: λ_.λ_. #Err{@make_string("cannot coerce list to string")}

  // Attrs - check for __toString or outPath
  #ABs: λlist. @coerce_attrs_to_string(#ABs{list})
  #ALy: λov.λbase. @coerce_attrs_to_string(#ALy{ov, base})

  // Function - error
  _: #Err{@make_string("cannot coerce this value to string")}
}(v)

// Special case for attrsets with __toString or outPath
@coerce_attrs_to_string = λattrs.
  @if (@hasAttr(attrs, "__toString"))
      (@coerce_to_string((@getAttr(attrs, "__toString"))(attrs)))
      (@if (@hasAttr(attrs, "outPath"))
           (@coerce_to_string(@getAttr(attrs, "outPath")))
           (#Err{@make_string("cannot coerce set to string")}))
```

### Coercion to Path

```hvm4
@coerce_to_path = λv. λ{
  // Already a path
  #Pth: λacc.λpath. #Pth{acc, path}

  // String - convert to path (absolute or relative)
  #Str: λchars.λctx.
    @if (@str_starts_with(chars, "/"))
        (#Pth{0, #Str{chars, #NoC{}}})  // Absolute path
        (#Err{@make_string("cannot coerce relative string to path")})

  // Other types - error
  _: #Err{@make_string("cannot coerce to path")}
}(v)
```

### Coercion to Integer

```hvm4
@coerce_to_int = λv. λ{
  // Already an integer (small or big)
  #Pos: λlo.λhi. #Pos{lo, hi}
  #Neg: λlo.λhi. #Neg{lo, hi}

  // For small integers, NUM passes through
  // (handled by isSmallInt check)

  // Other types - error
  _: #Err{@make_string("cannot coerce to integer")}
}(v)
```

### Context Preservation During Coercion

When coercing values to strings, context MUST be preserved and merged:

```hvm4
// Path to string adds store path to context
@coerce_path_to_string = λpath.
  let store_path = @copy_to_store(path.path);
  #Str{@path_chars(store_path), #Ctx{#Con{@hash_path(store_path), #Nil{}}}}

// Attrset with outPath inherits its context
@coerce_attrs_to_string = λattrs.
  let out = @getAttr(attrs, "outPath");
  // outPath's context is inherited
  @coerce_to_string(out)
```

### Coercion Tests

```cpp
// === String Coercion Tests ===

TEST_F(HVM4BackendTest, CoerceIntToString) {
    auto v = eval("toString 42", true);
    ASSERT_EQ(v.string_view(), "42");
}

TEST_F(HVM4BackendTest, CoerceNegativeIntToString) {
    auto v = eval("toString (-42)", true);
    ASSERT_EQ(v.string_view(), "-42");
}

TEST_F(HVM4BackendTest, CoerceBoolToString) {
    auto v = eval("toString true", true);
    ASSERT_EQ(v.string_view(), "true");

    v = eval("toString false", true);
    ASSERT_EQ(v.string_view(), "false");
}

TEST_F(HVM4BackendTest, CoerceNullToString) {
    auto v = eval("toString null", true);
    ASSERT_EQ(v.string_view(), "");
}

TEST_F(HVM4BackendTest, CoerceStringToString) {
    auto v = eval("toString \"hello\"", true);
    ASSERT_EQ(v.string_view(), "hello");
}

TEST_F(HVM4BackendTest, CoerceListToStringError) {
    EXPECT_THROW(eval("toString [1 2 3]", true), EvalError);
}

TEST_F(HVM4BackendTest, CoerceAttrsWithToString) {
    auto v = eval(R"(
        let x = { __toString = self: "custom"; };
        in toString x
    )", true);
    ASSERT_EQ(v.string_view(), "custom");
}

TEST_F(HVM4BackendTest, CoerceAttrsWithOutPath) {
    auto v = eval(R"(
        let x = { outPath = "/nix/store/abc123"; };
        in toString x
    )", true);
    ASSERT_EQ(v.string_view(), "/nix/store/abc123");
}

TEST_F(HVM4BackendTest, CoerceAttrsWithBothPreferToString) {
    // __toString takes precedence over outPath
    auto v = eval(R"(
        let x = {
            __toString = self: "toString";
            outPath = "/nix/store/abc";
        };
        in toString x
    )", true);
    ASSERT_EQ(v.string_view(), "toString");
}

TEST_F(HVM4BackendTest, CoerceFunctionToStringError) {
    EXPECT_THROW(eval("toString (x: x)", true), EvalError);
}

TEST_F(HVM4BackendTest, InterpolationCoercion) {
    auto v = eval("\"num: ${toString 42}\"", true);
    ASSERT_EQ(v.string_view(), "num: 42");
}

TEST_F(HVM4BackendTest, InterpolationDirectString) {
    auto v = eval("let s = \"world\"; in \"hello ${s}\"", true);
    ASSERT_EQ(v.string_view(), "hello world");
}

// === BigInt Coercion Tests ===

TEST_F(HVM4BackendTest, CoerceBigIntToString) {
    auto v = eval("toString 9999999999", true);
    ASSERT_EQ(v.string_view(), "9999999999");
}

TEST_F(HVM4BackendTest, CoerceNegativeBigIntToString) {
    auto v = eval("toString (-9999999999)", true);
    ASSERT_EQ(v.string_view(), "-9999999999");
}

// === Deep Coercion Tests ===

TEST_F(HVM4BackendTest, DeepToString) {
    // Nested __toString calls
    auto v = eval(R"(
        let
            inner = { __toString = self: "inner"; };
            outer = { __toString = self: "outer-${toString inner}"; };
        in toString outer
    )", true);
    ASSERT_EQ(v.string_view(), "outer-inner");
}
```

## Context Propagation

String context tracks store path dependencies and MUST be correctly propagated through all string operations.

### Context Types

```hvm4
// Context element types:
// #Opq{path}       - Opaque context (general store path dependency)
// #Drv{drvPath}    - Derivation context (input derivation)
// #Blt{drv, output} - Built output context (derivation output)

// Context is a sorted set for efficient merging
// #NoC{}           - Empty context
// #Ctx{elements}   - Non-empty context (elements is sorted list)
```

### Context Merging Rules

```hvm4
@merge_context = λctx1.λctx2. λ{
  #NoC: ctx2  // Empty + anything = anything
  #Ctx: λelems1. λ{
    #NoC: #Ctx{elems1}  // Anything + empty = anything
    #Ctx: λelems2. #Ctx{@set_union(elems1, elems2)}
  }(ctx2)
}(ctx1)

// All string operations must merge contexts:
@str_concat = λs1.λs2.
  #Str{
    @char_concat(s1.chars, s2.chars),
    @merge_context(s1.context, s2.context)
  }
```

### Context Propagation Tests

```cpp
// === Context Propagation Tests ===

TEST_F(HVM4BackendTest, ContextPreservedInConcat) {
    // When concatenating strings with context, context is merged
    auto v = eval(R"(
        let
            p1 = builtins.placeholder "out";
            p2 = builtins.placeholder "dev";
        in p1 + p2
    )", true);
    // Result should have context from both placeholders
    auto ctx = v.context();
    ASSERT_GE(ctx.size(), 2);
}

TEST_F(HVM4BackendTest, ContextPreservedInInterpolation) {
    auto v = eval(R"(
        let p = builtins.placeholder "out";
        in "prefix-${p}-suffix"
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextMergeInComplexExpr) {
    auto v = eval(R"(
        let
            drv1 = derivation { name = "a"; system = "x"; builder = "/bin/sh"; };
            drv2 = derivation { name = "b"; system = "x"; builder = "/bin/sh"; };
        in "${drv1}" + "${drv2}"
    )", true);
    auto ctx = v.context();
    ASSERT_GE(ctx.size(), 2);
}

TEST_F(HVM4BackendTest, NoContextForLiteral) {
    auto v = eval("\"hello world\"", true);
    auto ctx = v.context();
    ASSERT_TRUE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextPreservedThroughLet) {
    auto v = eval(R"(
        let
            p = builtins.placeholder "out";
            x = p;
            y = x;
        in y
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextPreservedInListToString) {
    auto v = eval(R"(
        let
            p = builtins.placeholder "out";
            parts = [p "-" "suffix"];
        in builtins.concatStringsSep "" parts
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextPreservedInSubstring) {
    auto v = eval(R"(
        let p = builtins.placeholder "out";
        in builtins.substring 0 5 p
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextPreservedInReplaceStrings) {
    auto v = eval(R"(
        let p = builtins.placeholder "out";
        in builtins.replaceStrings ["x"] ["y"] p
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextFromDerivation) {
    auto v = eval(R"(
        let drv = derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
        };
        in "${drv}"
    )", true);
    auto ctx = v.context();
    ASSERT_FALSE(ctx.empty());
    // Should contain derivation context
}

TEST_F(HVM4BackendTest, ContextUnsafeDiscardWorks) {
    auto v = eval(R"(
        let
            p = builtins.placeholder "out";
            noCtx = builtins.unsafeDiscardStringContext p;
        in noCtx
    )", true);
    auto ctx = v.context();
    ASSERT_TRUE(ctx.empty());
}

TEST_F(HVM4BackendTest, ContextGetAndAdd) {
    auto v = eval(R"(
        let
            p = builtins.placeholder "out";
            ctx = builtins.getContext p;
        in builtins.length (builtins.attrNames ctx)
    )", true);
    ASSERT_GE(v.integer().value, 1);
}
```

## Boolean Primops

```hvm4
@builtins_true = #Tru{}
@builtins_false = #Fls{}

// Short-circuit AND (implemented as conditional, not OP2)
@bool_and = λa.λb. λ{#Tru: b; #Fls: #Fls{}}(a)

// Short-circuit OR
@bool_or = λa.λb. λ{#Tru: #Tru{}; #Fls: b}(a)

// Negation
@bool_not = λa. λ{#Tru: #Fls{}; #Fls: #Tru{}}(a)

// Implication: a -> b = !a || b
@bool_impl = λa.λb. λ{#Tru: b; #Fls: #Tru{}}(a)
```

## Comparison Primops

```hvm4
// Deep equality (structural)
@builtins_eq = λa.λb.
  @typeOf(a) == @typeOf(b) .&.
  λ{
    #Str: @str_eq(a, b)
    #Lst: @list_eq(a, b)
    #ABs: @attrs_eq(a, b)
    // ... other types
    _: a == b  // Primitive comparison
  }(@typeOf(a))

// Note: Functions are not comparable (throw error)
@list_eq = λas.λbs.
  @length(as) == @length(bs) .&.
  @all(@zipWith(@builtins_eq, as, bs))
```

## Debugging Primops

```hvm4
// trace: prints first arg, returns second
@builtins_trace = λmsg.λval.
  // Would require effect-based I/O
  #Trace{msg, λ_. val}

// seq: force first arg, return second
@builtins_seq = λa.λb.
  // Force evaluation of a, then return b
  @force(a);
  b

// deepSeq: recursively force first arg
@builtins_deepSeq = λa.λb.
  @deepForce(a);
  b
```

## JSON/TOML Primops

```hvm4
// These would require effect-based parsing or pre-processing

// builtins.fromJSON requires parsing JSON → Nix values
// Map JSON types to Nix:
//   JSON object → Nix attrs
//   JSON array → Nix list
//   JSON string → Nix string
//   JSON number → Nix int (if no decimal) or float (if decimal)
//   JSON true/false → Nix true/false
//   JSON null → Nix null

// Option: Pre-parse during compilation if argument is literal
// Option: Effect-based parsing at runtime
```

## Primop Implementation Strategy

**Phase 1:** Implement essential primops inline
**Phase 2:** Create primop registry with HVM4 implementations

```hvm4
// Registry approach:
@primop = λname. λ{
  "add": @builtin_add
  "sub": @builtin_sub
  "map": @builtin_map
  // ...
  λ_. @error("unknown primop")
}(name)
```

## Primop Tests

```cpp
// === Type Checking Primops ===

TEST_F(HVM4BackendTest, IsInt) {
    auto v = eval("builtins.isInt 42", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isInt \"hello\"", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsString) {
    auto v = eval("builtins.isString \"hello\"", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isString 42", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsBool) {
    auto v = eval("builtins.isBool true", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isBool 1", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsList) {
    auto v = eval("builtins.isList [1 2 3]", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isList { }", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsAttrs) {
    auto v = eval("builtins.isAttrs { a = 1; }", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isAttrs [1 2 3]", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsPath) {
    auto v = eval("builtins.isPath ./.", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isPath \"/path\"", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, IsFunction) {
    auto v = eval("builtins.isFunction (x: x)", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isFunction 42", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, TypeOf) {
    auto v = eval("builtins.typeOf 42", true);
    ASSERT_EQ(v.string_view(), "int");

    v = eval("builtins.typeOf \"hello\"", true);
    ASSERT_EQ(v.string_view(), "string");

    v = eval("builtins.typeOf true", true);
    ASSERT_EQ(v.string_view(), "bool");

    v = eval("builtins.typeOf [1 2]", true);
    ASSERT_EQ(v.string_view(), "list");

    v = eval("builtins.typeOf { }", true);
    ASSERT_EQ(v.string_view(), "set");
}

// === Boolean Primops ===

TEST_F(HVM4BackendTest, BoolAnd) {
    auto v = eval("true && true", true);
    ASSERT_TRUE(v.boolean());

    v = eval("true && false", true);
    ASSERT_FALSE(v.boolean());

    v = eval("false && true", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, BoolOr) {
    auto v = eval("true || false", true);
    ASSERT_TRUE(v.boolean());

    v = eval("false || false", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, BoolNot) {
    auto v = eval("!true", true);
    ASSERT_FALSE(v.boolean());

    v = eval("!false", true);
    ASSERT_TRUE(v.boolean());
}

TEST_F(HVM4BackendTest, BoolImplication) {
    auto v = eval("true -> true", true);
    ASSERT_TRUE(v.boolean());

    v = eval("true -> false", true);
    ASSERT_FALSE(v.boolean());

    v = eval("false -> false", true);
    ASSERT_TRUE(v.boolean());  // Implication with false antecedent is true
}

TEST_F(HVM4BackendTest, BoolShortCircuit) {
    // Short-circuit: second arg not evaluated
    auto v = eval("false && (throw \"not evaluated\")", true);
    ASSERT_FALSE(v.boolean());

    v = eval("true || (throw \"not evaluated\")", true);
    ASSERT_TRUE(v.boolean());
}

// === Debugging Primops ===

TEST_F(HVM4BackendTest, Seq) {
    auto v = eval("builtins.seq 1 2", true);
    ASSERT_EQ(v.integer().value, 2);
}

TEST_F(HVM4BackendTest, SeqForces) {
    // seq should force first argument
    EXPECT_THROW(eval("builtins.seq (throw \"forced\") 2", true), EvalError);
}

TEST_F(HVM4BackendTest, DeepSeq) {
    auto v = eval("builtins.deepSeq { a = 1; b = 2; } 42", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, DeepSeqForces) {
    // deepSeq should force nested values
    EXPECT_THROW(
        eval("builtins.deepSeq { a = throw \"deep\"; } 42", true),
        EvalError
    );
}

// === Miscellaneous Primops ===

TEST_F(HVM4BackendTest, Null) {
    auto v = eval("null", true);
    ASSERT_EQ(v.type(), nNull);
}

TEST_F(HVM4BackendTest, IsNull) {
    auto v = eval("builtins.isNull null", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isNull 0", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, Throw) {
    EXPECT_THROW(eval("throw \"error message\"", true), EvalError);
}

TEST_F(HVM4BackendTest, TryEval) {
    auto v = eval("builtins.tryEval 42", true);
    ASSERT_EQ(v.type(), nAttrs);
    auto success = v.attrs()->get(state.symbols.create("success"));
    state.forceValue(*success->value, noPos);
    ASSERT_TRUE(success->value->boolean());
}

TEST_F(HVM4BackendTest, TryEvalCatch) {
    auto v = eval("builtins.tryEval (throw \"error\")", true);
    auto success = v.attrs()->get(state.symbols.create("success"));
    state.forceValue(*success->value, noPos);
    ASSERT_FALSE(success->value->boolean());
}

TEST_F(HVM4BackendTest, Assert) {
    auto v = eval("assert true; 42", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, AssertFails) {
    EXPECT_THROW(eval("assert false; 42", true), EvalError);
}

TEST_F(HVM4BackendTest, Abort) {
    EXPECT_THROW(eval("builtins.abort \"stopped\"", true), EvalError);
}
```

---

# 11. Floats

Nix has double-precision floats. HVM4 only has 32-bit integers.

## Option A: IEEE 754 Encoding

Encode doubles as pairs of 32-bit integers.

```hvm4
// Float = #Float{lo, hi}
// lo, hi are the low/high 32 bits of IEEE 754 double

// Operations require reimplementing float arithmetic
// Very complex, probably not worth it
```

## Option B: Fixed-Point

Use fixed-point representation for limited precision.

```hvm4
// Fixed = #Fixed{mantissa, scale}
// value = mantissa / 10^scale

@fixed_add = λa.λb.
  // Align scales, add mantissas
```

## Option C: External Float Handling

Defer float operations to external handler.

```hvm4
// FloatOp = #FloatAdd{a, b, continuation} | ...

// Float operations become effects handled outside HVM4
```

## CHOSEN: Reject Floats (Not Supported)

**Rationale:**
- Implementing IEEE 754 double-precision in HVM4's 32-bit integers is extremely complex
- Would require reimplementing all floating-point operations in software
- Float usage in Nix is rare (mostly for builtins.fromJSON parsing)
- Can fall back to standard evaluator for expressions containing floats

**Implementation:**

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprFloat*>(&expr)) {
    return false;  // Floats not supported - fallback to standard evaluator
}

// All float-related primops also return false:
// - builtins.ceil, builtins.floor
// - Any arithmetic with float operands
```

**Future Option (Phase 2):**
Could add effect-based external handling if needed:
```hvm4
// FloatOp = #FloatAdd{a, b, continuation} | ...
// External handler pauses HVM4, computes in native floats, continues
```

---

# 12. Pattern-Matching Lambdas

Nix supports `{ a, b, ... }: body` style lambdas.

## Nix Implementation Details

```cpp
struct ExprLambda {
    Symbol arg;        // Simple arg name (may be empty)
    Formals* formals;  // Pattern: list of Formal{name, default}
    bool ellipsis;     // Has ...?
};

// Calling: extract attrs, bind to formals, check for extra attrs if no ...
```

## Option A: Desugar to Attrset Destructuring

```hvm4
// { a, b ? 1, ... }: body
// →
// λ__arg. let a = __arg.a; b = __arg.b or 1; in body

@compile_formals = λformals.λellipsis.λbody.
  λ__arg.
    // Validate: if not ellipsis, check for extra attrs
    @when(not ellipsis, @check_no_extra(__arg, formals));
    // Bind each formal
    let a = @get_attr_or("a", __arg, default_a);
    let b = @get_attr_or("b", __arg, default_b);
    // ...
    body
```

## Option B: Native Pattern Match

Use HVM4's pattern matching if we encode attrs as constructors.

```hvm4
// Requires knowing attr layout at compile time
// Not practical for general case
```

## CHOSEN: Desugar to Attribute Destructuring (Option A)

**Rationale:**
- Simple transformation at compile time
- Leverages attribute set support once implemented
- Correctly handles defaults, ellipsis, and @-patterns
- No special runtime support needed

**Desugaring Example:**
```nix
# Input:
{ a, b ? 1, ... } @ args: body

# Desugars to:
__arg: let
  a = __arg.a;
  b = if __arg ? b then __arg.b else 1;
  args = __arg;
in body
```

### Detailed Implementation Steps

**Files Modified:**
- `src/libexpr/hvm4/hvm4-compiler.cc` (pattern lambda desugaring)

#### Step 1: Detect Pattern Lambdas

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprLambda*>(&expr)) {
    if (e->hasFormals()) {
        // Pattern lambda - check if we can compile the desugared form
        // Need attribute set support
        return canCompileDesugaredFormals(*e, scope);
    }
    // Simple lambda
    return canCompileWithScope(*e->body, extendedScope);
}

bool canCompileDesugaredFormals(const ExprLambda& e, const Scope& scope) {
    // Check body is compilable
    Scope bodyScope = scope;

    // Add formal parameters to scope
    for (const auto& formal : e.formals->formals) {
        bodyScope.insert(formal.name);
    }

    // Add @-pattern if present
    if (e.arg) {
        bodyScope.insert(e.arg);
    }

    return canCompileWithScope(*e.body, bodyScope);
}
```

#### Step 2: Generate Anonymous Arg Binding

```cpp
Term HVM4Compiler::emitPatternLambda(const ExprLambda& e, CompileContext& ctx) {
    // Create fresh symbol for anonymous arg
    Symbol argSym = ctx.freshSymbol("__arg");

    ctx.pushScope();
    ctx.addBinding(argSym, /* runtime arg */);

    // Generate body with formals bound
    Term body = emitFormalsAndBody(e, argSym, ctx);

    ctx.popScope();

    // Wrap in lambda
    return emitLambda(argSym, body, ctx);
}
```

#### Step 3: For Each Formal, Generate Lookup or Conditional

```cpp
Term emitFormalsAndBody(const ExprLambda& e, Symbol argSym,
                         CompileContext& ctx) {
    Term argRef = ctx.lookupBinding(argSym);
    std::vector<std::pair<Symbol, Term>> bindings;

    for (const auto& formal : e.formals->formals) {
        Term binding;

        if (formal.def) {
            // Has default: if __arg ? name then __arg.name else default
            Term hasAttr = emitHasAttr(argRef, formal.name, ctx);
            Term lookup = emitSelect(argRef, formal.name, ctx);
            Term defaultVal = emit(*formal.def, ctx);
            binding = emitIfThenElse(hasAttr, lookup, defaultVal, ctx);
        } else {
            // Required: __arg.name (throws if missing)
            binding = emitSelect(argRef, formal.name, ctx);
        }

        bindings.push_back({formal.name, binding});
        ctx.addBinding(formal.name, binding);
    }

    // Handle @-pattern
    if (e.arg) {
        bindings.push_back({e.arg, argRef});
        ctx.addBinding(e.arg, argRef);
    }

    // Compile body
    Term body = emit(*e.body, ctx);

    // Wrap in let bindings
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
        body = emitLet(it->first, it->second, body, ctx);
    }

    return body;
}
```

#### Step 4: Handle Ellipsis (No Extra Attr Check)

```cpp
Term emitFormalsAndBody(const ExprLambda& e, Symbol argSym,
                         CompileContext& ctx) {
    // ... (previous code)

    // If no ellipsis, check for unexpected attributes
    if (!e.formals->ellipsis) {
        Term extraCheck = emitCheckNoExtraAttrs(argRef, e.formals, ctx);
        // Prepend check to body (evaluates first, throws if extra attrs)
        body = emitSeq(extraCheck, body, ctx);
    }

    // ... rest
}

Term emitCheckNoExtraAttrs(Term arg, const Formals* formals,
                            CompileContext& ctx) {
    // Get list of expected names
    std::set<Symbol> expectedNames;
    for (const auto& f : formals->formals) {
        expectedNames.insert(f.name);
    }

    // For each attr in arg, check if it's expected
    // This can be done at runtime or partially at compile time
    return emitExtraAttrCheck(arg, expectedNames, ctx);
}
```

#### Step 5: Add Comprehensive Tests

```cpp
// In hvm4.cc test file

TEST_F(HVM4BackendTest, PatternLambdaSimple) {
    auto v = eval("({ a, b }: a + b) { a = 1; b = 2; }", true);
    ASSERT_EQ(v.integer().value, 3);
}

TEST_F(HVM4BackendTest, PatternLambdaDefault) {
    auto v = eval("({ a, b ? 10 }: a + b) { a = 1; }", true);
    ASSERT_EQ(v.integer().value, 11);
}

TEST_F(HVM4BackendTest, PatternLambdaDefaultOverride) {
    auto v = eval("({ a, b ? 10 }: a + b) { a = 1; b = 2; }", true);
    ASSERT_EQ(v.integer().value, 3);
}

TEST_F(HVM4BackendTest, PatternLambdaEllipsis) {
    // Extra attrs allowed with ...
    auto v = eval("({ a, ... }: a) { a = 1; b = 2; c = 3; }", true);
    ASSERT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, PatternLambdaNoEllipsisError) {
    // Extra attrs without ... should error
    EXPECT_THROW(
        eval("({ a }: a) { a = 1; b = 2; }", true),
        EvalError
    );
}

TEST_F(HVM4BackendTest, PatternLambdaAtPattern) {
    auto v = eval("({ a, b, ... } @ args: args.c) { a = 1; b = 2; c = 3; }", true);
    ASSERT_EQ(v.integer().value, 3);
}

TEST_F(HVM4BackendTest, PatternLambdaMissingRequired) {
    // Missing required attr should error
    EXPECT_THROW(
        eval("({ a, b }: a) { a = 1; }", true),
        EvalError
    );
}

TEST_F(HVM4BackendTest, PatternLambdaNested) {
    auto v = eval(R"(
        let f = { a }: { b }: a + b;
        in f { a = 1; } { b = 2; }
    )", true);
    ASSERT_EQ(v.integer().value, 3);
}

// === Additional Pattern Lambda Edge Cases ===

TEST_F(HVM4BackendTest, PatternLambdaEmptyPattern) {
    auto v = eval("({ }: 42) { }", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, PatternLambdaOnlyEllipsis) {
    // Just ellipsis, accepts anything
    auto v = eval("({ ... }: 42) { a = 1; b = 2; }", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, PatternLambdaAllDefaults) {
    auto v = eval("({ a ? 1, b ? 2, c ? 3 }: a + b + c) { }", true);
    ASSERT_EQ(v.integer().value, 6);
}

TEST_F(HVM4BackendTest, PatternLambdaPartialDefaults) {
    auto v = eval("({ a, b ? 2, c ? 3 }: a + b + c) { a = 10; }", true);
    ASSERT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, PatternLambdaDefaultReferencesOther) {
    // Default can reference another formal (from the same attrset)
    auto v = eval("({ a, b ? a * 2 }: a + b) { a = 5; }", true);
    ASSERT_EQ(v.integer().value, 15);  // 5 + 10
}

TEST_F(HVM4BackendTest, PatternLambdaDefaultReferencesOtherProvided) {
    // When both are provided, default isn't used
    auto v = eval("({ a, b ? a * 2 }: a + b) { a = 5; b = 3; }", true);
    ASSERT_EQ(v.integer().value, 8);  // 5 + 3
}

TEST_F(HVM4BackendTest, PatternLambdaAtPatternAccess) {
    auto v = eval(R"(
        let f = { a, ... } @ args: builtins.attrNames args;
        in f { a = 1; b = 2; c = 3; }
    )", true);
    // args should have a, b, c
}

TEST_F(HVM4BackendTest, PatternLambdaInMap) {
    auto v = eval(R"(
        builtins.map ({ x }: x * 2) [{ x = 1; } { x = 2; } { x = 3; }]
    )", true);
    // Should be [2, 4, 6]
}

TEST_F(HVM4BackendTest, PatternLambdaLazyDefault) {
    // Default should only be evaluated if not provided
    auto v = eval(R"(
        ({ a ? throw "not used" }: 42) { a = 1; }
    )", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, PatternLambdaChained) {
    auto v = eval(R"(
        let
            f = { a }: { b }: { c }: a + b + c;
        in f { a = 1; } { b = 2; } { c = 3; }
    )", true);
    ASSERT_EQ(v.integer().value, 6);
}

TEST_F(HVM4BackendTest, PatternLambdaMixedWithSimple) {
    // Mixing pattern and simple lambdas
    auto v = eval(R"(
        let f = { a }: x: a + x;
        in f { a = 10; } 5
    )", true);
    ASSERT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, PatternLambdaReturnsFunction) {
    auto v = eval(R"(
        let mkAdder = { x }: y: x + y;
            add5 = mkAdder { x = 5; };
        in add5 10
    )", true);
    ASSERT_EQ(v.integer().value, 15);
}
```

---

# Debugging and Troubleshooting

This section provides guidance for debugging the HVM4 backend during development.

## Common Issues and Solutions

### 1. Incorrect Laziness Behavior

**Symptom:** Values are being forced when they shouldn't be, causing unexpected errors.

**Diagnosis:**
```cpp
// Add logging to see when values are forced
void HVM4Runtime::force(Term term) {
    std::cerr << "Forcing term at " << term_val(term) << std::endl;
    // ... force logic
}
```

**Common Causes:**
- List length computation forcing elements
- Attribute key sorting forcing values
- String context merging forcing string content

**Solution:** Ensure constructors are being used correctly:
- `#Lst{length, spine}` - length is computed from spine structure, not by forcing elements
- `#Atr{key, value}` - key is a strict NUM, value is a lazy thunk

### 2. Wrong Constructor Tags

**Symptom:** Pattern matching fails or produces wrong results.

**Diagnosis:**
```cpp
// Print constructor tag
std::cerr << "Tag: " << term_ext(term) << " expected: " << EXPECTED_TAG << std::endl;
```

**Solution:** Verify base-64 encoding of constructor names matches the table in the encoding section.

### 3. Stack Overflow in Recursion

**Symptom:** Deep recursive evaluation crashes.

**Diagnosis:**
```cpp
// Add recursion depth tracking
thread_local int recursionDepth = 0;
struct RecursionGuard {
    RecursionGuard() { ++recursionDepth; if (recursionDepth > 10000) abort(); }
    ~RecursionGuard() { --recursionDepth; }
};
```

**Solution:**
- Ensure Y-combinator is correctly applied
- Check for unintended infinite loops in cyclic rec
- Consider fuel-based evaluation for debugging

### 4. Context Loss in Strings

**Symptom:** String context is empty when it should contain path references.

**Diagnosis:**
```cpp
// Print context during operations
void debugContext(Term strTerm, HVM4Runtime& runtime) {
    Term ctx = stringContext(strTerm, runtime);
    if (term_ext(ctx) == NO_CONTEXT) {
        std::cerr << "No context" << std::endl;
    } else {
        std::cerr << "Has context elements" << std::endl;
    }
}
```

**Solution:** Ensure context merging is performed during:
- String concatenation
- String interpolation
- Path-to-string coercion

### 5. Attribute Lookup Returning Wrong Values

**Symptom:** `attrs.name` returns wrong value or fails when it shouldn't.

**Diagnosis:**
```cpp
// Debug attribute lookup
Term debugLookup(uint32_t symbolId, Term attrs, HVM4Runtime& runtime) {
    std::cerr << "Looking up symbol " << symbolId << std::endl;
    // Print all attrs in the set
    Term current = getAttrList(attrs, runtime);
    while (!isNil(current)) {
        Term node = getCar(current);
        uint32_t key = getAttrKey(node, runtime);
        std::cerr << "  Found key " << key << std::endl;
        current = getCdr(current);
    }
    return lookupAttr(symbolId, attrs, runtime);
}
```

**Common Causes:**
- Symbol IDs not matching (different SymbolTable instances)
- Layers not being searched in correct order
- Binary search bounds incorrect

## Debug Logging

Enable verbose logging by setting environment variable:
```bash
export NIX_HVM4_DEBUG=1
```

Or compile with debug flags:
```cpp
#ifdef HVM4_DEBUG
#define HVM4_LOG(msg) std::cerr << "[HVM4] " << msg << std::endl
#else
#define HVM4_LOG(msg)
#endif
```

## Testing Strategies

### Unit Test Template
```cpp
TEST_F(HVM4BackendTest, DebugSpecificIssue) {
    // 1. Minimal reproduction
    auto v = eval("minimal expression", true);

    // 2. Step-by-step verification
    ASSERT_EQ(v.type(), expectedType);

    // 3. Force nested values
    if (v.type() == nAttrs) {
        for (auto& [name, attr] : *v.attrs()) {
            state.forceValue(*attr.value, noPos);
            // Check each value
        }
    }
}
```

### Comparison Testing
```cpp
TEST_F(HVM4BackendTest, CompareWithStandardEvaluator) {
    std::string expr = "complex expression";

    // Evaluate with standard evaluator
    auto stdResult = evalStandard(expr);

    // Evaluate with HVM4
    auto hvm4Result = eval(expr, true);

    // Compare results
    ASSERT_TRUE(valuesEqual(stdResult, hvm4Result));
}
```

## Practical Debugging Examples

### Example 1: Debugging Attribute Set Construction

When attribute sets aren't working correctly, use this pattern:

```cpp
// Debug helper to print attribute set structure
void dumpAttrs(Term attrs, HVM4Runtime& runtime, int depth = 0) {
    std::string indent(depth * 2, ' ');
    uint32_t tag = term_ext(attrs);

    if (tag == ATTRS_BASE) {
        std::cerr << indent << "#ABs{" << std::endl;
        Term list = getField(attrs, 0, runtime);
        dumpAttrList(list, runtime, depth + 1);
        std::cerr << indent << "}" << std::endl;
    } else if (tag == ATTRS_LAYER) {
        std::cerr << indent << "#ALy{" << std::endl;
        std::cerr << indent << "  overlay:" << std::endl;
        dumpAttrList(getField(attrs, 0, runtime), runtime, depth + 2);
        std::cerr << indent << "  base:" << std::endl;
        dumpAttrs(getField(attrs, 1, runtime), runtime, depth + 2);
        std::cerr << indent << "}" << std::endl;
    }
}

void dumpAttrList(Term list, HVM4Runtime& runtime, int depth) {
    std::string indent(depth * 2, ' ');
    while (!isNil(list, runtime)) {
        Term node = getCar(list, runtime);
        uint32_t key = term_val(getField(node, 0, runtime));
        std::cerr << indent << "key=" << key << std::endl;
        list = getCdr(list, runtime);
    }
}

// Usage in test:
TEST_F(HVM4BackendTest, DebugAttrConstruction) {
    // Enable HVM4 compilation
    auto result = compileExpr("{ a = 1; b = 2; }");
    dumpAttrs(result, runtime);
    // Check output matches expected structure
}
```

### Example 2: Tracing Evaluation Steps

```cpp
// Wrap HVM4 reduction to trace steps
class TracingRuntime : public HVM4Runtime {
public:
    Term reduce(Term term) override {
        std::cerr << "REDUCE: ";
        printTerm(term);
        Term result = HVM4Runtime::reduce(term);
        std::cerr << " -> ";
        printTerm(result);
        std::cerr << std::endl;
        return result;
    }

private:
    void printTerm(Term t) {
        uint32_t tag = term_ext(t);
        uint32_t val = term_val(t);
        std::cerr << "[" << tag << ":" << val << "]";
    }
};

// Usage:
TracingRuntime tracer;
Term result = tracer.eval(compiledExpr);
```

### Example 3: Isolating Laziness Issues

```cpp
// Test that ensures laziness is preserved
TEST_F(HVM4BackendTest, IsolateLazinessIssue) {
    // This should NOT throw, because element 1 is never accessed
    auto v = eval(R"(
        let list = [1 (throw "should not be forced") 3];
        in builtins.elemAt list 0
    )", true);
    ASSERT_EQ(v.integer().value, 1);

    // This SHOULD throw when element 1 is accessed
    EXPECT_THROW({
        eval(R"(
            let list = [1 (throw "forced") 3];
            in builtins.elemAt list 1
        )", true);
    }, EvalError);
}

// Verify length doesn't force elements
TEST_F(HVM4BackendTest, LengthDoesntForce) {
    auto v = eval(R"(
        builtins.length [1 (throw "forced") 3]
    )", true);
    ASSERT_EQ(v.integer().value, 3);  // Should succeed without throwing
}
```

### Example 4: Debugging BigInt Edge Cases

```cpp
TEST_F(HVM4BackendTest, DebugBigIntBoundary) {
    // Test exact boundary values
    struct TestCase {
        std::string expr;
        int64_t expected;
    };

    std::vector<TestCase> cases = {
        {"2147483647", INT32_MAX},           // Max 32-bit
        {"2147483648", INT32_MAX + 1LL},     // Just over 32-bit
        {"-2147483648", INT32_MIN},          // Min 32-bit
        {"-2147483649", INT32_MIN - 1LL},    // Just under 32-bit
        {"4294967295", UINT32_MAX},          // Max unsigned 32-bit
        {"4294967296", UINT32_MAX + 1LL},    // Just over unsigned 32-bit
    };

    for (const auto& tc : cases) {
        std::cerr << "Testing: " << tc.expr << std::endl;
        auto v = eval(tc.expr, true);
        ASSERT_EQ(v.integer().value, tc.expected)
            << "Failed for: " << tc.expr;
    }
}
```

### Example 5: Debugging Context Propagation

```cpp
// Helper to visualize context
void dumpContext(const NixStringContext& ctx) {
    std::cerr << "Context elements: " << ctx.size() << std::endl;
    for (const auto& elem : ctx) {
        std::visit(overloaded {
            [](const NixStringContextElem::Opaque& x) {
                std::cerr << "  Opaque: " << x.path << std::endl;
            },
            [](const NixStringContextElem::DrvDeep& x) {
                std::cerr << "  DrvDeep: " << x.drvPath << std::endl;
            },
            [](const NixStringContextElem::Built& x) {
                std::cerr << "  Built: " << x.drvPath << "!" << x.output << std::endl;
            },
        }, elem.raw);
    }
}

TEST_F(HVM4BackendTest, DebugContextPropagation) {
    auto v = eval(R"(
        let
            drv = derivation {
                name = "test";
                system = "x86_64-linux";
                builder = "/bin/sh";
            };
        in "prefix-${drv}-suffix"
    )", true);

    std::cerr << "Result string: " << v.string_view() << std::endl;
    dumpContext(v.context());

    // Context should contain the derivation
    ASSERT_FALSE(v.context().empty());
}
```

## Common Debugging Patterns

### Pattern: Binary Search on Test Cases

When a test fails, use binary search to find minimal reproduction:

```cpp
// Instead of testing the whole expression at once:
auto v = eval("large complex expression with many parts", true);

// Break it down:
auto v1 = eval("first half", true);  // Does this work?
auto v2 = eval("second half", true); // Or this?
// Continue subdividing until you find the minimal failing case
```

### Pattern: Verify Constructor Encoding

```cpp
// Verify your constructor encoding matches documentation
void verifyEncodings() {
    // From the encoding table:
    ASSERT_EQ(encodeConstructor("#Nil"), 166118);
    ASSERT_EQ(encodeConstructor("#Con"), 121448);
    // Add checks for all constructors you use
}
```

### Pattern: Compare Term Structures

```cpp
// Compare two terms structurally for debugging
bool termsEqual(Term a, Term b, HVM4Runtime& runtime) {
    if (term_ext(a) != term_ext(b)) {
        std::cerr << "Tag mismatch: " << term_ext(a) << " vs " << term_ext(b) << std::endl;
        return false;
    }
    // Recursively compare fields...
}
```

## Performance Profiling

### HVM4 Statistics
```cpp
struct HVM4Stats {
    size_t heapAllocations = 0;
    size_t reductions = 0;
    size_t maxStackDepth = 0;

    void report() {
        std::cerr << "Heap: " << heapAllocations << std::endl;
        std::cerr << "Reductions: " << reductions << std::endl;
        std::cerr << "Max stack: " << maxStackDepth << std::endl;
    }
};
```

### Benchmarking
```cpp
TEST_F(HVM4BackendTest, BenchmarkLargeEval) {
    auto start = std::chrono::high_resolution_clock::now();

    auto v = eval("large expression", true);

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cerr << "Evaluation time: " << ms.count() << "ms" << std::endl;
}
```

---

# Summary: Implementation Priority and Chosen Approaches

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

---

# Error Handling Strategy

This section documents how errors should be propagated and handled in the HVM4 backend
to maintain Nix's evaluation semantics.

## Error Encoding

All errors use the `#Err{message}` constructor from the encoding table:

```hvm4
// Error representation
#Err{message}  // message is a #Str{chars, #NoC{}}

// Error creation helper
@error = λmsg. #Err{@make_string(msg)}
```

## Error Categories

### 1. Attribute Access Errors

| Error | When | Nix Behavior | HVM4 Implementation |
|-------|------|--------------|---------------------|
| Missing attribute | `{ a = 1; }.b` | Immediate throw | Return `#Err{"missing attribute"}` |
| Select on non-attrset | `42.a` | Immediate throw | Return `#Err{"not an attrset"}` |
| hasAttr on non-attrset | `42 ? a` | Immediate throw | Return `#Err{"not an attrset"}` |
| Update with non-attrset | `42 // {}` | Immediate throw | Return `#Err{"not an attrset"}` |

**Key semantics**: Error only occurs when the attribute is *accessed*, not when the attrset is constructed:
```nix
{ a = 1; b = throw "x"; }.a  # Returns 1, no error
{ a = 1; b = throw "x"; }.b  # Throws "x"
```

### 2. List Operation Errors

| Error | When | Nix Behavior | HVM4 Implementation |
|-------|------|--------------|---------------------|
| head/tail on empty | `builtins.head []` | Immediate throw | Return `#Err{"empty list"}` |
| elemAt out of bounds | `builtins.elemAt [1] 5` | Immediate throw | Return `#Err{"index out of bounds"}` |
| elemAt negative index | `builtins.elemAt [1] (-1)` | Immediate throw | Return `#Err{"negative index"}` |

**Key semantics**: List length is strict but elements are lazy:
```nix
builtins.length [1, throw "x", 3]  # Returns 3, no error
builtins.elemAt [1, throw "x", 3] 0  # Returns 1, no error
builtins.elemAt [1, throw "x", 3] 1  # Throws "x"
```

### 3. String Operation Errors

| Error | When | Nix Behavior | HVM4 Implementation |
|-------|------|--------------|---------------------|
| Concat non-string | `"a" + 42` | Immediate throw | Return `#Err{"expected string"}` |
| Negative substring start | `builtins.substring (-1) 5 s` | Immediate throw | Return `#Err{"negative start"}` |
| Invalid interpolation | `"${throw \"x\"}"` | Immediate throw | Error during construction |

**Key semantics**: Strings are fully strict; interpolation errors occur during construction:
```nix
"hello ${throw \"x\"}"  # Throws immediately, not when used
let s = "hello ${throw \"x\"}"; in 42  # Still throws (construction happens in let)
```

### 4. Arithmetic Errors

| Error | When | Nix Behavior | HVM4 Implementation |
|-------|------|--------------|---------------------|
| Division by zero | `1 / 0` | Immediate throw | Return `#Err{"division by zero"}` |
| Type mismatch | `1 + "a"` | Immediate throw | Return `#Err{"expected number"}` |
| Overflow (64-bit) | Large result | Wrap around | Use BigInt encoding |

### 5. Function Application Errors

| Error | When | Nix Behavior | HVM4 Implementation |
|-------|------|--------------|---------------------|
| Wrong argument type | Pattern mismatch | Immediate throw | Return `#Err{"pattern mismatch"}` |
| Missing required attr | `({a}: a) {}` | Immediate throw | Return `#Err{"missing required: a"}` |
| Unexpected attr | `({a}: a) {a=1; b=2;}` | Immediate throw if no `...` | Return `#Err{"unexpected attr: b"}` |
| Call non-function | `42 1` | Immediate throw | Return `#Err{"not a function"}` |

### 6. Path and Import Errors

| Error | When | Nix Behavior | HVM4 Implementation |
|-------|------|--------------|---------------------|
| Path not found | `./missing.nix` | Throw at access | Defer to result extraction |
| Parse error | `import ./bad.nix` | Throw at import | Pre-compile error |
| Circular import | `A imports B imports A` | Throw | Detect during pre-resolution |

## Error Propagation Strategy

### Option A: Immediate Throw (Current Nix)
Throw exceptions immediately and unwind stack. **Not suitable for HVM4** as it
doesn't support exceptions natively.

### Option B: Error Values (CHOSEN)
Use error values that propagate through computation:

```hvm4
// Error-aware operations
@add = λa.λb. λ{
  #Err: λmsg. #Err{msg}   // Propagate error from a
  _: λ{
    #Err: λmsg. #Err{msg} // Propagate error from b
    _: a + b              // Actual addition
  }(b)
}(a)

// Or using monadic style:
@bind_err = λma.λf. λ{
  #Err: λmsg. #Err{msg}
  _: f(ma)
}(ma)
```

### Error Extraction at Boundary

When extracting results from HVM4 back to C++:

```cpp
// In hvm4-result.cc
Value ResultExtractor::extractToNix(Term term) {
    uint32_t tag = term_ext(term);

    if (tag == ERR_CONSTRUCTOR) {
        uint32_t loc = term_val(term);
        Term msgTerm = runtime.getHeapAt(loc);
        std::string msg = extractString(msgTerm);
        throw EvalError(msg);  // Convert to Nix exception
    }

    // Normal extraction...
}
```

## Error Handling Tests Summary

Each feature section includes error handling tests. Here's a consolidated reference:

| Feature | Test Category | Example Test |
|---------|---------------|--------------|
| Attrs | Missing attr | `EXPECT_THROW(eval("{ a = 1; }.b"), EvalError)` |
| Attrs | Type mismatch | `EXPECT_THROW(eval("42.a"), EvalError)` |
| Lists | Empty access | `EXPECT_THROW(eval("builtins.head []"), EvalError)` |
| Lists | Index bounds | `EXPECT_THROW(eval("builtins.elemAt [1] 5"), EvalError)` |
| Strings | Type mismatch | `EXPECT_THROW(eval("\"a\" + 42"), EvalError)` |
| Strings | Bad substring | `EXPECT_THROW(eval("builtins.substring (-1) 1 \"x\""), EvalError)` |
| Arithmetic | Div by zero | `EXPECT_THROW(eval("1 / 0"), EvalError)` |
| Functions | Missing arg | `EXPECT_THROW(eval("({a}: a) {}"), EvalError)` |

---

# Appendix: HVM4 Patterns Reference

> **Note:** This section shows generic HVM4 programming patterns. For the specific
> constructor names used in the Nix HVM4 encoding, see the [HVM4 Encoding Strategy](#hvm4-encoding-strategy) table.

## Pattern: Sum Types

```hvm4
// Option (Nix uses #Som/#Non)
#Non{}
#Som{value}

// Boolean (Nix uses #Tru/#Fls)
#Tru{}
#Fls{}

// Null
#Nul{}

// Either
#Lft{value}
#Rgt{value}

// Result
#Ok_{value}
#Err{error}
```

## Pattern: Recursive Data

```hvm4
// List (HVM4 native)
#Nil{}
#Con{head, tail}

// Tree
#Lef{value}
#Nod{left, right}

// With function
@length = λ{#Nil: 0; #Con: λh.λt. 1 + @length(t)}
```

## Pattern: Mutual Recursion

```hvm4
@even = λ{#Z: #Tru{}; #S: λn. @odd(n)}
@odd = λ{#Z: #Fls{}; #S: λn. @even(n)}
```

## Pattern: Effects via Continuation

```hvm4
// Effect type
#Pur{value}
#Eff{op, arg, continuation}

// Handler
@run = λ{
  #Pur: λv. v
  #Eff: λop.λarg.λk.
    // Handle op(arg), call k with result
}
```

## Pattern: State Threading

```hvm4
// State s a = s -> (a, s)
@return = λa. λs. #Par{a, s}
@bind = λma.λf. λs.
  let #Par{a, s'} = ma(s);
  f(a)(s')
```

---

# Appendix: Integration Tests

These tests verify that multiple features work correctly together, testing the interactions between different data types and language constructs.

## Attribute Sets + Lists

```cpp
TEST_F(HVM4BackendTest, IntegrationAttrsWithLists) {
    // Attribute values are lists
    auto v = eval("{ xs = [1 2 3]; ys = [4 5 6]; }.xs ++ { xs = [1 2 3]; ys = [4 5 6]; }.ys", true);
    ASSERT_EQ(v.listSize(), 6);
}

TEST_F(HVM4BackendTest, IntegrationListOfAttrs) {
    // List of attribute sets
    auto v = eval("builtins.map (x: x.a) [{ a = 1; } { a = 2; } { a = 3; }]", true);
    ASSERT_EQ(v.listSize(), 3);
    state.forceValue(*v.listElems()[0], noPos);
    ASSERT_EQ(v.listElems()[0]->integer().value, 1);
}

TEST_F(HVM4BackendTest, IntegrationAttrNamesValues) {
    // attrNames and attrValues
    auto v = eval(R"(
        let attrs = { b = 2; a = 1; c = 3; };
        in builtins.zipAttrsWith (n: vs: builtins.head vs) [attrs]
    )", true);
    ASSERT_EQ(v.attrs()->size(), 3);
}
```

## Strings + Interpolation + Attribute Sets

```cpp
TEST_F(HVM4BackendTest, IntegrationStringInterpolationAttrs) {
    // String interpolation with attribute access
    auto v = eval("let x = { name = \"world\"; }; in \"hello ${x.name}\"", true);
    ASSERT_EQ(v.string_view(), "hello world");
}

TEST_F(HVM4BackendTest, IntegrationAttrNamesAsStrings) {
    // Using strings as attribute names
    auto v = eval("let name = \"foo\"; in { ${name} = 42; }.foo", true);
    ASSERT_EQ(v.integer().value, 42);
}
```

## Functions + Pattern Matching + Attribute Sets

```cpp
TEST_F(HVM4BackendTest, IntegrationPatternMatchingPipeline) {
    // Common NixOS pattern: function composition with attrsets
    auto v = eval(R"(
        let
          f = { a, b ? 0 }: { c = a + b; };
          g = { c }: c * 2;
        in g (f { a = 5; b = 3; })
    )", true);
    ASSERT_EQ(v.integer().value, 16);
}

TEST_F(HVM4BackendTest, IntegrationOverlay) {
    // NixOS overlay pattern
    auto v = eval(R"(
        let
          base = { a = 1; b = 2; };
          overlay = self: super: { a = super.a + 10; c = 3; };
          fixed = let self = base // overlay self base; in self;
        in fixed.a
    )", true);
    ASSERT_EQ(v.integer().value, 11);
}
```

## Recursive Structures

```cpp
TEST_F(HVM4BackendTest, IntegrationRecursiveAttrset) {
    // Recursive attribute set with self-reference
    auto v = eval(R"(
        let
          tree = rec {
            value = 1;
            left = null;
            right = null;
            sum = value + (if left == null then 0 else left.sum)
                       + (if right == null then 0 else right.sum);
          };
        in tree.sum
    )", true);
    ASSERT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, IntegrationFixpoint) {
    // Fixpoint computation (factorial)
    auto v = eval(R"(
        let
          fix = f: let x = f x; in x;
          factorial = self: n: if n <= 1 then 1 else n * self (n - 1);
        in (fix factorial) 5
    )", true);
    ASSERT_EQ(v.integer().value, 120);
}
```

## With Expressions + Attribute Sets

```cpp
TEST_F(HVM4BackendTest, IntegrationWithNested) {
    // Nested with expressions
    auto v = eval(R"(
        let
          lib = { add = a: b: a + b; mul = a: b: a * b; };
          nums = { x = 3; y = 4; };
        in with lib; with nums; mul (add x y) x
    )", true);
    ASSERT_EQ(v.integer().value, 21);  // (3 + 4) * 3 = 21
}

TEST_F(HVM4BackendTest, IntegrationWithShadowing) {
    // With shadowing outer scope correctly
    auto v = eval(R"(
        let x = 1;
        in with { x = 2; }; x
    )", true);
    ASSERT_EQ(v.integer().value, 2);  // with wins
}
```

## Complex NixOS-like Patterns

```cpp
TEST_F(HVM4BackendTest, IntegrationModuleSystem) {
    // Simplified NixOS module pattern
    auto v = eval(R"(
        let
          evalModules = modules:
            let
              merged = builtins.foldl' (a: b: a // b) {} modules;
            in merged;

          moduleA = { config = { a = 1; }; };
          moduleB = { config = { b = 2; }; };
        in (evalModules [moduleA.config moduleB.config]).a
    )", true);
    ASSERT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, IntegrationDerivationLike) {
    // Derivation-like pattern (without actual derivation)
    auto v = eval(R"(
        let
          mkDerivation = { name, buildInputs ? [], ... } @ args:
            args // {
              type = "derivation";
              outPath = "/nix/store/fake-${name}";
            };
          pkg = mkDerivation {
            name = "test";
            version = "1.0";
          };
        in pkg.outPath
    )", true);
    ASSERT_EQ(v.string_view(), "/nix/store/fake-test");
}
```

## Laziness Preservation Across Features

```cpp
TEST_F(HVM4BackendTest, IntegrationLazinessChain) {
    // Verify laziness is preserved through multiple operations
    auto v = eval(R"(
        let
          xs = [1 (throw "lazy1") 3];
          ys = builtins.map (x: x) xs;
          attrs = { inherit xs ys; z = throw "lazy2"; };
          result = attrs // { w = throw "lazy3"; };
        in builtins.length result.xs
    )", true);
    ASSERT_EQ(v.integer().value, 3);  // No throws
}

TEST_F(HVM4BackendTest, IntegrationLazyAttrValues) {
    // Lazy attr values through update chain
    auto v = eval(R"(
        let
          a = { x = throw "a"; };
          b = { y = throw "b"; };
          c = a // b // { z = 42; };
        in c.z
    )", true);
    ASSERT_EQ(v.integer().value, 42);  // No throws
}
```

## Real-World Patterns

```cpp
TEST_F(HVM4BackendTest, IntegrationPackageSet) {
    // Simplified package set pattern
    auto v = eval(R"(
        let
          callPackage = path: overrides:
            let fn = path; args = builtins.functionArgs fn;
            in fn (builtins.mapAttrs (n: v: overrides.${n} or v) args);

          hello = { name ? "hello", version ? "1.0" }:
            { inherit name version; };

          pkgs = {
            hello = callPackage hello {};
            helloCustom = callPackage hello { name = "hi"; };
          };
        in pkgs.helloCustom.name
    )", true);
    ASSERT_EQ(v.string_view(), "hi");
}

TEST_F(HVM4BackendTest, IntegrationListComprehension) {
    // List comprehension pattern using map and filter
    auto v = eval(R"(
        let
          xs = builtins.genList (x: x) 10;
          evens = builtins.filter (x: builtins.mod x 2 == 0) xs;
          squares = builtins.map (x: x * x) evens;
        in builtins.foldl' builtins.add 0 squares
    )", true);
    // 0^2 + 2^2 + 4^2 + 6^2 + 8^2 = 0 + 4 + 16 + 36 + 64 = 120
    ASSERT_EQ(v.integer().value, 120);
}
```

## Error Propagation

```cpp
TEST_F(HVM4BackendTest, IntegrationErrorInNestedExpr) {
    // Error in deeply nested expression
    EXPECT_THROW(eval(R"(
        let
          a = { x = { y = { z = throw "deep"; }; }; };
        in a.x.y.z
    )", true), EvalError);
}

TEST_F(HVM4BackendTest, IntegrationErrorMessagePreserved) {
    // Verify error message is meaningful
    try {
        eval("{ a = 1; }.b", true);
        FAIL() << "Expected exception";
    } catch (const EvalError& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("b") != std::string::npos);  // Should mention missing attr
    }
}
```

## Conditionals + All Types

```cpp
TEST_F(HVM4BackendTest, IntegrationConditionalTypes) {
    // Conditionals returning different types
    auto v = eval("if true then 42 else \"hello\"", true);
    ASSERT_EQ(v.integer().value, 42);

    v = eval("if false then 42 else \"hello\"", true);
    ASSERT_EQ(v.string_view(), "hello");
}

TEST_F(HVM4BackendTest, IntegrationConditionalLaziness) {
    // Both branches should be lazy
    auto v = eval(R"(
        let
            choose = cond: a: b: if cond then a else b;
        in choose true 42 (throw "not evaluated")
    )", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, IntegrationNestedConditionals) {
    auto v = eval(R"(
        let
            classify = n:
                if n < 0 then "negative"
                else if n == 0 then "zero"
                else if n < 10 then "small"
                else "large";
        in classify 5
    )", true);
    ASSERT_EQ(v.string_view(), "small");
}
```

## Closures and Scope

```cpp
TEST_F(HVM4BackendTest, IntegrationClosureCapture) {
    auto v = eval(R"(
        let
            x = 10;
            f = y: x + y;
        in f 5
    )", true);
    ASSERT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, IntegrationClosureNested) {
    auto v = eval(R"(
        let
            mkCounter = start:
                let
                    count = start;
                    inc = n: count + n;
                in { inherit count inc; };
            counter = mkCounter 10;
        in counter.inc 5
    )", true);
    ASSERT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, IntegrationHigherOrderFunctions) {
    auto v = eval(R"(
        let
            compose = f: g: x: f (g x);
            double = x: x * 2;
            inc = x: x + 1;
            doubleThenInc = compose inc double;
        in doubleThenInc 5
    )", true);
    ASSERT_EQ(v.integer().value, 11);  // (5 * 2) + 1
}

TEST_F(HVM4BackendTest, IntegrationCurrying) {
    auto v = eval(R"(
        let
            add = a: b: c: a + b + c;
            add5 = add 5;
            add5and3 = add5 3;
        in add5and3 2
    )", true);
    ASSERT_EQ(v.integer().value, 10);
}
```

## String Building

```cpp
TEST_F(HVM4BackendTest, IntegrationStringBuilder) {
    auto v = eval(R"(
        let
            items = ["a" "b" "c"];
            joined = builtins.concatStringsSep ", " items;
        in "Items: ${joined}"
    )", true);
    ASSERT_EQ(v.string_view(), "Items: a, b, c");
}

TEST_F(HVM4BackendTest, IntegrationMultilineString) {
    auto v = eval(R"(
        let
            name = "test";
        in ''
            Hello ${name}!
            Line 2
        ''
    )", true);
    // Should handle multiline with proper indentation stripping
}
```

## Performance-Critical Patterns

```cpp
TEST_F(HVM4BackendTest, IntegrationLargeList) {
    auto v = eval("builtins.length (builtins.genList (x: x) 1000)", true);
    ASSERT_EQ(v.integer().value, 1000);
}

TEST_F(HVM4BackendTest, IntegrationLargeAttrset) {
    auto v = eval(R"(
        let
            mkAttrs = n: builtins.listToAttrs (
                builtins.genList (i: { name = "a${toString i}"; value = i; }) n
            );
        in builtins.length (builtins.attrNames (mkAttrs 100))
    )", true);
    ASSERT_EQ(v.integer().value, 100);
}

TEST_F(HVM4BackendTest, IntegrationDeepRecursion) {
    auto v = eval(R"(
        let
            sum = n: if n <= 0 then 0 else n + sum (n - 1);
        in sum 100
    )", true);
    ASSERT_EQ(v.integer().value, 5050);  // 1 + 2 + ... + 100
}
```

---

# Appendix: Nix-to-HVM4 Translation Examples

This section shows how common Nix patterns translate to HVM4 terms, useful for understanding the compilation process.

## Simple Values

```
Nix: 42
HVM4: 42  (raw NUM)

Nix: "hello"
HVM4: #Str{#Con{#Chr{104}, #Con{#Chr{101}, #Con{#Chr{108}, #Con{#Chr{108}, #Con{#Chr{111}, #Nil{}}}}}}, #NoC{}}

Nix: true
HVM4: #Tru{}

Nix: null
HVM4: #Nul{}
```

## Let Bindings

```
Nix:
  let x = 1; y = 2; in x + y

HVM4 (simplified):
  (λx. (λy. (OP2 ADD x y)) 2) 1

Or with @let syntax:
  @let x = 1;
  @let y = 2;
  (OP2 ADD x y)
```

## Attribute Sets

```
Nix:
  { a = 1; b = 2; }

HVM4:
  #ABs{#Con{#Atr{sym_a, 1}, #Con{#Atr{sym_b, 2}, #Nil{}}}}

Note: sym_a and sym_b are symbol IDs, sorted numerically
```

## Attribute Access

```
Nix:
  attrs.name

HVM4:
  @lookup(sym_name, attrs)

Expands to searching the sorted list:
  @lookup = λkey.λattrs. λ{
    #ABs: λlist. @lookup_list(key, list)
    #ALy: λoverlay.λbase.
      @lookup_list(key, overlay) .or. @lookup(key, base)
  }(attrs)
```

## Functions

```
Nix:
  x: x + 1

HVM4:
  λx. (OP2 ADD x 1)

Nix:
  { a, b ? 0 }: a + b

HVM4 (desugared):
  λ__arg.
    @let a = @select(__arg, sym_a);
    @let b = @if (@hasAttr(__arg, sym_b))
                 (@select(__arg, sym_b))
                 (0);
    (OP2 ADD a b)
```

## Lists

```
Nix:
  [1 2 3]

HVM4:
  #Lst{3, #Con{1, #Con{2, #Con{3, #Nil{}}}}}

Note: Length 3 is cached, elements are thunks
```

## Conditionals

```
Nix:
  if cond then a else b

HVM4:
  λ{#Tru: a; #Fls: b}(cond)

Or using MAT:
  (MAT cond #Tru a #Fls b)
```

## Recursion (rec)

```
Nix:
  rec { a = b + 1; b = 10; }

HVM4 (acyclic, after topo-sort):
  @let b = 10;
  @let a = (OP2 ADD b 1);
  #ABs{#Con{#Atr{sym_a, a}, #Con{#Atr{sym_b, b}, #Nil{}}}}

Nix:
  rec { even = n: ...; odd = n: ...; }

HVM4 (cyclic, using Y-combinator):
  @Y(λself. #ABs{
    #Atr{sym_even, λn. ... (@select(self, sym_odd)) ...},
    #Atr{sym_odd, λn. ... (@select(self, sym_even)) ...}
  })
```

## With Expressions

```
Nix:
  with { x = 1; }; x

HVM4 (static resolution):
  @let $with = #ABs{#Con{#Atr{sym_x, 1}, #Nil{}}};
  @select($with, sym_x)

Nix (ambiguous):
  let x = 1; in with { x = 2; }; x

HVM4:
  @let x = 1;
  @let $with = #ABs{#Con{#Atr{sym_x, 2}, #Nil{}}};
  @if (@hasAttr($with, sym_x))
      (@select($with, sym_x))
      (x)
```

## String Interpolation

```
Nix:
  "hello ${name}!"

HVM4:
  @str_concat(
    @str_concat(
      #Str{[h,e,l,l,o, ], #NoC{}},
      @coerce_to_string(name)
    ),
    #Str{[!], #NoC{}}
  )

Note: Context from 'name' is merged into result
```

---

# Appendix: Stress Tests

These tests verify the HVM4 backend handles edge cases, large inputs, and pathological patterns correctly.

## Memory and Scale Tests

```cpp
// Large list stress tests
TEST_F(HVM4BackendTest, StressLargeListLength) {
    // Test O(1) length on large lists
    auto v = eval("builtins.length (builtins.genList (x: x) 10000)", true);
    ASSERT_EQ(v.integer().value, 10000);
}

TEST_F(HVM4BackendTest, StressLazyListElements) {
    // Only first element should be evaluated
    auto v = eval(R"(
        builtins.head (builtins.genList (x:
            if x == 0 then 42 else throw "should not evaluate"
        ) 10000)
    )", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, StressListConcat) {
    // Concatenating many lists
    auto v = eval(R"(
        let
            small = [1 2 3];
            concat100 = builtins.foldl' (acc: _: acc ++ small) [] (builtins.genList (x: x) 100);
        in builtins.length concat100
    )", true);
    ASSERT_EQ(v.integer().value, 300);
}

TEST_F(HVM4BackendTest, StressLargeAttrset) {
    // Large attribute set lookup
    auto v = eval(R"(
        let
            attrs = builtins.listToAttrs (
                builtins.genList (i: { name = "key${toString i}"; value = i; }) 1000
            );
        in attrs.key500
    )", true);
    ASSERT_EQ(v.integer().value, 500);
}

TEST_F(HVM4BackendTest, StressAttrsetUpdate) {
    // Many // operations (tests layer flattening)
    auto v = eval(R"(
        let
            base = { a = 1; };
            update = i: { "b${toString i}" = i; };
            result = builtins.foldl' (acc: i: acc // update i) base (builtins.genList (x: x) 50);
        in builtins.length (builtins.attrNames result)
    )", true);
    ASSERT_EQ(v.integer().value, 51);  // 'a' + 50 'b*' keys
}

TEST_F(HVM4BackendTest, StressLongString) {
    // Long string operations
    auto v = eval(R"(
        let
            repeat = n: s:
                if n <= 0 then ""
                else s + repeat (n - 1) s;
        in builtins.stringLength (repeat 1000 "x")
    )", true);
    ASSERT_EQ(v.integer().value, 1000);
}
```

## Deep Recursion Tests

```cpp
TEST_F(HVM4BackendTest, StressDeepRecursion) {
    // Test stack safety with deep recursion
    auto v = eval(R"(
        let
            count = n: if n <= 0 then 0 else 1 + count (n - 1);
        in count 500
    )", true);
    ASSERT_EQ(v.integer().value, 500);
}

TEST_F(HVM4BackendTest, StressMutualRecursion) {
    // Deeply nested mutual recursion
    auto v = eval(R"(
        let
            isEven = n: if n == 0 then true else isOdd (n - 1);
            isOdd = n: if n == 0 then false else isEven (n - 1);
        in isEven 200
    )", true);
    ASSERT_EQ(v.type(), nBool);
    ASSERT_TRUE(v.boolean());
}

TEST_F(HVM4BackendTest, StressFibonacci) {
    // Exponential recursion (tests memoization/sharing)
    auto v = eval(R"(
        let
            fib = n:
                if n <= 1 then n
                else fib (n - 1) + fib (n - 2);
        in fib 20
    )", true);
    ASSERT_EQ(v.integer().value, 6765);
}

TEST_F(HVM4BackendTest, StressNestedLet) {
    // Many nested let bindings
    auto v = eval(R"(
        let a1 = 1;
            a2 = a1 + 1;
            a3 = a2 + 1;
            a4 = a3 + 1;
            a5 = a4 + 1;
            a6 = a5 + 1;
            a7 = a6 + 1;
            a8 = a7 + 1;
            a9 = a8 + 1;
            a10 = a9 + 1;
        in a10
    )", true);
    ASSERT_EQ(v.integer().value, 10);
}
```

## Edge Case Tests

```cpp
TEST_F(HVM4BackendTest, StressEmptyList) {
    auto v = eval("builtins.length []", true);
    ASSERT_EQ(v.integer().value, 0);
}

TEST_F(HVM4BackendTest, StressEmptyAttrset) {
    auto v = eval("builtins.attrNames {}", true);
    ASSERT_EQ(v.type(), nList);
    ASSERT_EQ(v.listSize(), 0);
}

TEST_F(HVM4BackendTest, StressEmptyString) {
    auto v = eval("builtins.stringLength \"\"", true);
    ASSERT_EQ(v.integer().value, 0);
}

TEST_F(HVM4BackendTest, StressSingleElement) {
    auto v = eval("builtins.head [42]", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, StressSingleAttr) {
    auto v = eval("{ a = 1; }.a", true);
    ASSERT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, StressNullPropagation) {
    // null should propagate correctly
    auto v = eval("null", true);
    ASSERT_EQ(v.type(), nNull);

    v = eval("builtins.isNull null", true);
    ASSERT_TRUE(v.boolean());

    v = eval("builtins.isNull 0", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, StressNestedNull) {
    auto v = eval("{ a = null; }.a", true);
    ASSERT_EQ(v.type(), nNull);
}

TEST_F(HVM4BackendTest, StressListWithNull) {
    auto v = eval("[null null null]", true);
    ASSERT_EQ(v.listSize(), 3);
    ASSERT_EQ(v.listElem(0)->type(), nNull);
}
```

## Function Application Tests

```cpp
TEST_F(HVM4BackendTest, StressHigherOrderFunctions) {
    // Map over list
    auto v = eval("builtins.map (x: x * 2) [1 2 3 4 5]", true);
    ASSERT_EQ(v.listSize(), 5);
    ASSERT_EQ(v.listElem(0)->integer().value, 2);
    ASSERT_EQ(v.listElem(4)->integer().value, 10);
}

TEST_F(HVM4BackendTest, StressFilter) {
    auto v = eval("builtins.filter (x: x > 3) [1 2 3 4 5]", true);
    ASSERT_EQ(v.listSize(), 2);
}

TEST_F(HVM4BackendTest, StressFoldl) {
    auto v = eval("builtins.foldl' (acc: x: acc + x) 0 [1 2 3 4 5]", true);
    ASSERT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, StressCurriedFunction) {
    auto v = eval(R"(
        let
            add = a: b: a + b;
            add5 = add 5;
        in add5 10
    )", true);
    ASSERT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, StressPartialApplication) {
    auto v = eval(R"(
        let
            f = a: b: c: a + b + c;
            g = f 1;
            h = g 2;
        in h 3
    )", true);
    ASSERT_EQ(v.integer().value, 6);
}

TEST_F(HVM4BackendTest, StressFunctionComposition) {
    auto v = eval(R"(
        let
            compose = f: g: x: f (g x);
            double = x: x * 2;
            inc = x: x + 1;
        in (compose double inc) 5
    )", true);
    ASSERT_EQ(v.integer().value, 12);  // (5 + 1) * 2
}

TEST_F(HVM4BackendTest, StressLazyFunctionArg) {
    // Unused argument should not be evaluated
    auto v = eval(R"(
        let
            const = a: b: a;
        in const 42 (throw "should not evaluate")
    )", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, StressRecursiveLambda) {
    auto v = eval(R"(
        let
            factorial = n: if n <= 1 then 1 else n * factorial (n - 1);
        in factorial 10
    )", true);
    ASSERT_EQ(v.integer().value, 3628800);
}

TEST_F(HVM4BackendTest, StressIdentityFunction) {
    auto v = eval("(x: x) 42", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, StressNestedLambdas) {
    auto v = eval(R"(
        ((a: (b: (c: a + b + c))) 1) 2) 3
    )", true);
    ASSERT_EQ(v.integer().value, 6);
}
```

## Pathological Pattern Tests

```cpp
TEST_F(HVM4BackendTest, StressDeepNesting) {
    // Deeply nested attribute access
    auto v = eval(R"(
        let
            nest = n: if n <= 0 then 42 else { inner = nest (n - 1); };
            deep = nest 20;
        in deep.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner
    )", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, StressWideAttrset) {
    // Very wide attribute set
    auto v = eval(R"(
        let
            attrs = builtins.listToAttrs (
                builtins.genList (i: { name = "a${toString i}"; value = i; }) 500
            );
        in attrs.a250
    )", true);
    ASSERT_EQ(v.integer().value, 250);
}

TEST_F(HVM4BackendTest, StressManyWith) {
    // Nested with expressions
    auto v = eval(R"(
        with { a = 1; };
        with { b = 2; };
        with { c = 3; };
        a + b + c
    )", true);
    ASSERT_EQ(v.integer().value, 6);
}

TEST_F(HVM4BackendTest, StressWithShadowing) {
    // with shadowing behavior
    auto v = eval(R"(
        let x = 1;
        in with { x = 2; };
           x
    )", true);
    // In Nix, let bindings shadow with - result should be 1
    ASSERT_EQ(v.integer().value, 1);
}

TEST_F(HVM4BackendTest, StressComplexInterpolation) {
    auto v = eval(R"(
        let
            a = "hello";
            b = "world";
            c = 42;
        in "${a} ${b} ${toString c}!"
    )", true);
    ASSERT_EQ(v.string_view(), "hello world 42!");
}
```

## BigInt Edge Cases

```cpp
TEST_F(HVM4BackendTest, StressBigIntBoundary) {
    // Test at 32-bit boundary
    auto v = eval("2147483647 + 1", true);  // INT32_MAX + 1
    ASSERT_EQ(v.integer().value, 2147483648LL);
}

TEST_F(HVM4BackendTest, StressBigIntNegative) {
    auto v = eval("-2147483648 - 1", true);  // INT32_MIN - 1
    ASSERT_EQ(v.integer().value, -2147483649LL);
}

TEST_F(HVM4BackendTest, StressBigIntMultiply) {
    auto v = eval("1000000 * 1000000", true);  // 1e12
    ASSERT_EQ(v.integer().value, 1000000000000LL);
}

TEST_F(HVM4BackendTest, StressBigIntDivision) {
    auto v = eval("1000000000000 / 1000000", true);
    ASSERT_EQ(v.integer().value, 1000000LL);
}
```

---

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
