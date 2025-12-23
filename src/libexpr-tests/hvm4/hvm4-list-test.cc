/**
 * HVM4 List Tests
 *
 * Comprehensive tests for list functionality in the HVM4 backend.
 *
 * List support is implemented using the encoding:
 *   #Lst{length, spine} where spine = #Nil{} | #Con{head, tail}
 *
 * Test Categories:
 * - Basic List Construction: Empty, single, multiple elements
 * - List Expressions: Lists containing expressions
 * - Nested Lists: Lists of lists
 * - Lists in Let Bindings: Variable scoping with lists
 * - Laziness Verification: Length should not force elements
 * - List Primops: builtins.length, head, tail, elemAt, map, etc.
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Basic List Construction Tests
// =============================================================================
// These tests verify list literal parsing and evaluation.

TEST_F(HVM4BackendTest, ListEmpty) {
    // Empty list: []
    // Expected encoding: #Lst{0, #Nil{}}
    auto* expr = state.parseExprFromString("[]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nList);
    EXPECT_EQ(result.listSize(), 0);
}

TEST_F(HVM4BackendTest, ListSingleElement) {
    // Single element list: [1]
    // Expected encoding: #Lst{1, #Con{1, #Nil{}}}
    auto* expr = state.parseExprFromString("[1]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nList);
    EXPECT_EQ(result.listSize(), 1);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 1);
}

TEST_F(HVM4BackendTest, ListMultipleElements) {
    // Multiple element list: [1 2 3]
    // Expected encoding: #Lst{3, #Con{1, #Con{2, #Con{3, #Nil{}}}}}
    auto* expr = state.parseExprFromString("[1 2 3]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.type(), nList);
    EXPECT_EQ(result.listSize(), 3);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 1);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 2);
    state.forceValue(*result.listView()[2], noPos);
    EXPECT_EQ(result.listView()[2]->integer().value, 3);
}

TEST_F(HVM4BackendTest, ListWithSpaceSeparators) {
    // Nix uses space separators in lists
    auto* expr = state.parseExprFromString("[10 20 30]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
}

// =============================================================================
// List Expression Tests
// =============================================================================
// Lists containing arithmetic and other expressions as elements.

TEST_F(HVM4BackendTest, ListWithExpressions) {
    // List with arithmetic expressions: [(1+1) (2+2)]
    // Elements are evaluated during extraction
    auto* expr = state.parseExprFromString("[(1+1) (2+2)]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 2);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 2);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 4);
}

TEST_F(HVM4BackendTest, ListWithMultiplication) {
    auto* expr = state.parseExprFromString("[(2*3) (4*5) (6*7)]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 6);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 20);
    state.forceValue(*result.listView()[2], noPos);
    EXPECT_EQ(result.listView()[2]->integer().value, 42);
}

TEST_F(HVM4BackendTest, ListWithNestedExpressions) {
    // More complex expressions in list elements
    auto* expr = state.parseExprFromString("[((1+2)*3) ((4-1)*(5-2))]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 2);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 9);  // (1+2)*3 = 9
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 9);  // (4-1)*(5-2) = 9
}

TEST_F(HVM4BackendTest, ListWithBooleanExpressions) {
    // Note: true/false are integers (1/0) in HVM4
    auto* expr = state.parseExprFromString("[true false (1 < 2) (3 > 4)]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 4);
}

TEST_F(HVM4BackendTest, ListWithConditionals) {
    auto* expr = state.parseExprFromString("[(if true then 1 else 2) (if false then 3 else 4)]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 2);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 1);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 4);
}

// =============================================================================
// Nested List Tests
// =============================================================================
// Lists containing other lists as elements.

TEST_F(HVM4BackendTest, ListNestedSingle) {
    // Nested list: [[1]]
    auto* expr = state.parseExprFromString("[[1]]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 1);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->type(), nList);
    EXPECT_EQ(result.listView()[0]->listSize(), 1);
}

TEST_F(HVM4BackendTest, ListNestedMultiple) {
    // Nested lists: [[1] [2 3]]
    auto* expr = state.parseExprFromString("[[1] [2 3]]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 2);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->listSize(), 1);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->listSize(), 2);
}

TEST_F(HVM4BackendTest, ListNestedEmpty) {
    // Nested empty lists: [[] []]
    auto* expr = state.parseExprFromString("[[] []]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 2);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->listSize(), 0);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->listSize(), 0);
}

TEST_F(HVM4BackendTest, ListDeeplyNested) {
    // Deeply nested: [[[[1]]]]
    auto* expr = state.parseExprFromString("[[[[1]]]]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 1);
}

TEST_F(HVM4BackendTest, ListMixedNesting) {
    // Mixed: [1 [2 3] 4]
    auto* expr = state.parseExprFromString("[1 [2 3] 4]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 1);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->type(), nList);
    state.forceValue(*result.listView()[2], noPos);
    EXPECT_EQ(result.listView()[2]->integer().value, 4);
}

// =============================================================================
// Lists in Let Bindings
// =============================================================================
// Variable scoping with list expressions.

TEST_F(HVM4BackendTest, ListInLetBinding) {
    // List in let binding body
    auto* expr = state.parseExprFromString("let x = 1; in [x (x + 1) (x + 2)]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 1);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 2);
    state.forceValue(*result.listView()[2], noPos);
    EXPECT_EQ(result.listView()[2]->integer().value, 3);
}

TEST_F(HVM4BackendTest, ListBoundToVariable) {
    // List bound to a variable
    auto* expr = state.parseExprFromString("let xs = [1 2 3]; in xs", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
}

TEST_F(HVM4BackendTest, ListWithMultipleLetBindings) {
    // Multiple let bindings used in list
    auto* expr = state.parseExprFromString("let a = 1; b = 2; c = 3; in [a b c]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 1);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 2);
    state.forceValue(*result.listView()[2], noPos);
    EXPECT_EQ(result.listView()[2]->integer().value, 3);
}

TEST_F(HVM4BackendTest, ListNestedLetBindings) {
    // Nested let with list
    auto* expr = state.parseExprFromString("let x = 1; in let y = 2; in [x y (x + y)]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 1);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 2);
    state.forceValue(*result.listView()[2], noPos);
    EXPECT_EQ(result.listView()[2]->integer().value, 3);
}

TEST_F(HVM4BackendTest, ListWithLambdaElements) {
    // List containing lambda expressions - lambdas need to be applied to be extracted
    auto* expr = state.parseExprFromString("[((x: x) 42) ((x: x + 1) 10)]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 2);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 42);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 11);
}

TEST_F(HVM4BackendTest, ListWithAppliedLambda) {
    // List with applied lambda
    auto* expr = state.parseExprFromString("[((x: x + 1) 5) ((x: x * 2) 3)]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 2);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 6);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 6);
}

// =============================================================================
// Laziness Verification Tests
// =============================================================================
// These tests verify that list length is O(1) due to cached length.
// Full laziness tests require builtins.throw which is not implemented.

TEST_F(HVM4BackendTest, ListLengthIsCached) {
    // List length should be O(1) using the cached length in #Lst{length, spine}
    auto* expr = state.parseExprFromString("[1 2 3 4 5 6 7 8 9 10]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 10);
}

TEST_F(HVM4BackendTest, ListElementsEvaluatedOnAccess) {
    // Elements with expressions are evaluated when extracted
    auto* expr = state.parseExprFromString(
        "let xs = [(1+1) (2+2) (3+3)]; in xs",
        state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
    // Elements should be evaluated
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 2);
}

TEST_F(HVM4BackendTest, ListComplexExpressionEvaluated) {
    // Complex expressions in lists are evaluated properly
    auto* expr = state.parseExprFromString("[((x: x * x) 5)]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 1);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 25);
}

TEST_F(HVM4BackendTest, ListLazyEvaluationStructure) {
    // The list structure is created correctly even with unevaluated elements
    auto* expr = state.parseExprFromString(
        "let f = x: x + 1; in [1 (f 2) (f (f 3))]",
        state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 1);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 3);
    state.forceValue(*result.listView()[2], noPos);
    EXPECT_EQ(result.listView()[2]->integer().value, 5);
}

// =============================================================================
// List Primop Tests (Future)
// =============================================================================
// These tests are placeholders for when list primops are implemented.
// Currently they just verify the expressions cannot be compiled.

TEST_F(HVM4BackendTest, ListBuiltinLength) {
    // builtins.length should be O(1) due to cached length
    auto* expr = state.parseExprFromString("builtins.length [1 2 3 4 5]", state.rootPath(CanonPath::root));

    // NOT YET IMPLEMENTED: Lists and builtins.length cannot be compiled yet
    EXPECT_FALSE(backend.canEvaluate(*expr));

    // TODO: Once implemented:
    // Value result;
    // bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    // ASSERT_TRUE(success);
    // EXPECT_EQ(result.integer().value, 5);
}

TEST_F(HVM4BackendTest, ListBuiltinHead) {
    // builtins.head returns first element
    auto* expr = state.parseExprFromString("builtins.head [1 2 3]", state.rootPath(CanonPath::root));

    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListBuiltinTail) {
    // builtins.tail returns all but first element
    auto* expr = state.parseExprFromString("builtins.tail [1 2 3]", state.rootPath(CanonPath::root));

    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListBuiltinElemAt) {
    // builtins.elemAt for indexed access
    auto* expr = state.parseExprFromString("builtins.elemAt [10 20 30] 1", state.rootPath(CanonPath::root));

    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListConcatOperator) {
    // ++ operator for list concatenation
    auto* expr = state.parseExprFromString("[1 2] ++ [3 4]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 4);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 1);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, 2);
    state.forceValue(*result.listView()[2], noPos);
    EXPECT_EQ(result.listView()[2]->integer().value, 3);
    state.forceValue(*result.listView()[3], noPos);
    EXPECT_EQ(result.listView()[3]->integer().value, 4);
}

TEST_F(HVM4BackendTest, ListBuiltinMap) {
    // builtins.map applies function to each element
    auto* expr = state.parseExprFromString("builtins.map (x: x * 2) [1 2 3]", state.rootPath(CanonPath::root));

    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListBuiltinFilter) {
    // builtins.filter selects elements matching predicate
    auto* expr = state.parseExprFromString("builtins.filter (x: x > 2) [1 2 3 4 5]", state.rootPath(CanonPath::root));

    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListBuiltinFoldl) {
    // builtins.foldl' for left fold
    auto* expr = state.parseExprFromString("builtins.foldl' (a: b: a + b) 0 [1 2 3 4]", state.rootPath(CanonPath::root));

    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListBuiltinConcatLists) {
    // builtins.concatLists flattens list of lists
    auto* expr = state.parseExprFromString("builtins.concatLists [[1 2] [3] [4 5 6]]", state.rootPath(CanonPath::root));

    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListBuiltinGenList) {
    // builtins.genList generates list from function
    auto* expr = state.parseExprFromString("builtins.genList (x: x * x) 5", state.rootPath(CanonPath::root));

    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListBuiltinElem) {
    // builtins.elem checks membership
    auto* expr = state.parseExprFromString("builtins.elem 3 [1 2 3 4]", state.rootPath(CanonPath::root));

    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TEST_F(HVM4BackendTest, ListLargeSize) {
    // Test with a larger list
    auto* expr = state.parseExprFromString(
        "[1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20]",
        state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 20);
    state.forceValue(*result.listView()[19], noPos);
    EXPECT_EQ(result.listView()[19]->integer().value, 20);
}

TEST_F(HVM4BackendTest, ListNegativeNumbers) {
    // List with negative numbers
    auto* expr = state.parseExprFromString("[(0-1) (0-2) (0-3)]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, -1);
    state.forceValue(*result.listView()[1], noPos);
    EXPECT_EQ(result.listView()[1]->integer().value, -2);
    state.forceValue(*result.listView()[2], noPos);
    EXPECT_EQ(result.listView()[2]->integer().value, -3);
}

TEST_F(HVM4BackendTest, ListZeros) {
    // List of zeros
    auto* expr = state.parseExprFromString("[0 0 0]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 3);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 0);
}

TEST_F(HVM4BackendTest, ListSameValue) {
    // List with same value repeated (tests sharing)
    auto* expr = state.parseExprFromString("let x = 42; in [x x x x]", state.rootPath(CanonPath::root));

    EXPECT_TRUE(backend.canEvaluate(*expr));
    Value result;
    bool success = backend.tryEvaluate(expr, state.baseEnv, result);
    ASSERT_TRUE(success);
    EXPECT_EQ(result.listSize(), 4);
    state.forceValue(*result.listView()[0], noPos);
    EXPECT_EQ(result.listView()[0]->integer().value, 42);
    state.forceValue(*result.listView()[3], noPos);
    EXPECT_EQ(result.listView()[3]->integer().value, 42);
}

// =============================================================================
// Error Case Tests
// =============================================================================
// These tests verify that list operations produce appropriate errors
// for invalid inputs.

TEST_F(HVM4BackendTest, ListHeadEmptyError) {
    // builtins.head [] should produce an error
    auto* expr = state.parseExprFromString("builtins.head []", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED: builtins not supported
    EXPECT_FALSE(backend.canEvaluate(*expr));

    // TODO: Once implemented, this should either:
    // - Return false from tryEvaluate, or
    // - The error should be caught in extraction
}

TEST_F(HVM4BackendTest, ListTailEmptyError) {
    // builtins.tail [] should produce an error
    auto* expr = state.parseExprFromString("builtins.tail []", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListElemAtOutOfBounds) {
    // builtins.elemAt [1 2 3] 5 should produce an error (index out of bounds)
    auto* expr = state.parseExprFromString("builtins.elemAt [1 2 3] 5", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListElemAtNegativeIndex) {
    // builtins.elemAt [1 2 3] (-1) should produce an error (negative index)
    auto* expr = state.parseExprFromString("builtins.elemAt [1 2 3] (0 - 1)", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListConcatNonList) {
    // [1 2] ++ 3 should be rejected (second operand not a list)
    auto* expr = state.parseExprFromString("[1 2] ++ 3", state.rootPath(CanonPath::root));
    // Currently ++ only works with direct list literals on both sides
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListMapNonFunction) {
    // builtins.map 42 [1 2 3] should produce an error (first arg not a function)
    auto* expr = state.parseExprFromString("builtins.map 42 [1 2 3]", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListFilterNonFunction) {
    // builtins.filter 42 [1 2 3] should produce an error
    auto* expr = state.parseExprFromString("builtins.filter 42 [1 2 3]", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

TEST_F(HVM4BackendTest, ListLengthNonList) {
    // builtins.length 42 should produce an error
    auto* expr = state.parseExprFromString("builtins.length 42", state.rootPath(CanonPath::root));
    // NOT YET IMPLEMENTED
    EXPECT_FALSE(backend.canEvaluate(*expr));
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
