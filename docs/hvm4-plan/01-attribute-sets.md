# 1. Attribute Sets

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).

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
- Uses [Lists](./02-lists.md) encoding for sorted attribute lists
- Used by [Pattern-Matching Lambdas](./12-pattern-matching-lambdas.md) for destructuring
- Used by [Recursive Let](./05-recursive-let.md) for cyclic bindings
- Used by [With Expressions](./06-with-expressions.md) for dynamic scope

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
