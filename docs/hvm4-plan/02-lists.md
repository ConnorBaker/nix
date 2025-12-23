# 2. Lists

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).

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
