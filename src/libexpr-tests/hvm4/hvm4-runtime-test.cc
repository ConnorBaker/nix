/**
 * HVM4 Runtime Tests
 *
 * Low-level tests for HVM4 term construction and evaluation.
 * These tests work directly with the HVM4 runtime without Nix parsing.
 *
 * Test Categories:
 * - Term Construction: CreateNum, CreateVar, CreateLam, CreateApp, etc.
 * - Evaluation: Arithmetic, comparison, lambda application
 * - Operator Tests: Division, modulo, bitwise operations
 */

#include "hvm4-test-common.hh"

#if NIX_USE_HVM4

namespace nix {
namespace hvm4 {

// =============================================================================
// Term Construction Tests
// =============================================================================

TEST_F(HVM4RuntimeTest, CreateNum) {
    Term t = HVM4Runtime::termNewNum(42);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(t), 42u);
}

TEST_F(HVM4RuntimeTest, CreateNumNegative) {
    Term t = HVM4Runtime::termNewNum(static_cast<uint32_t>(-1));
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(static_cast<int32_t>(HVM4Runtime::termVal(t)), -1);
}

TEST_F(HVM4RuntimeTest, CreateVar) {
    Term t = HVM4Runtime::termNewVar(5);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_VAR);
    EXPECT_EQ(HVM4Runtime::termVal(t), 5u);
}

TEST_F(HVM4RuntimeTest, CreateLam) {
    Term body = HVM4Runtime::termNewNum(1);
    Term t = runtime.termNewLam(body);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_LAM);
}

TEST_F(HVM4RuntimeTest, CreateApp) {
    Term fun = runtime.termNewLam(HVM4Runtime::termNewVar(0));
    Term arg = HVM4Runtime::termNewNum(42);
    Term t = runtime.termNewApp(fun, arg);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_APP);
}

TEST_F(HVM4RuntimeTest, CreateSup) {
    Term t1 = HVM4Runtime::termNewNum(1);
    Term t2 = HVM4Runtime::termNewNum(2);
    Term t = runtime.termNewSup(1, t1, t2);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_SUP);
    EXPECT_EQ(HVM4Runtime::termExt(t), 1u);
}

TEST_F(HVM4RuntimeTest, CreateOp2) {
    Term a = HVM4Runtime::termNewNum(3);
    Term b = HVM4Runtime::termNewNum(4);
    Term t = runtime.termNewOp2(HVM4Runtime::OP_ADD, a, b);
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_OP2);
    EXPECT_EQ(HVM4Runtime::termExt(t), HVM4Runtime::OP_ADD);
}

TEST_F(HVM4RuntimeTest, CreateEra) {
    Term t = HVM4Runtime::termNewEra();
    EXPECT_EQ(HVM4Runtime::termTag(t), HVM4Runtime::TAG_ERA);
}

TEST_F(HVM4RuntimeTest, HeapAllocation) {
    size_t initial = runtime.getAllocatedBytes();

    // LAM with body does allocate
    runtime.termNewLam(HVM4Runtime::termNewNum(1));
    EXPECT_GT(runtime.getAllocatedBytes(), initial);
}

// =============================================================================
// Basic Evaluation Tests
// =============================================================================

TEST_F(HVM4RuntimeTest, EvalSimpleNum) {
    Term t = HVM4Runtime::termNewNum(42);
    Term result = runtime.evaluateSNF(t);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 42u);
}

TEST_F(HVM4RuntimeTest, EvalIdentity) {
    // (\x. x) 42 -> 42
    // Note: VAR must reference the lambda's heap location, not de Bruijn index 0
    uint32_t lamLoc = runtime.allocateLamSlot();
    Term identity = runtime.finalizeLam(lamLoc, HVM4Runtime::termNewVar(lamLoc));
    Term app = runtime.termNewApp(identity, HVM4Runtime::termNewNum(42));
    Term result = runtime.evaluateSNF(app);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 42u);
}

TEST_F(HVM4RuntimeTest, EvalAdd) {
    // 3 + 4 -> 7
    Term a = HVM4Runtime::termNewNum(3);
    Term b = HVM4Runtime::termNewNum(4);
    Term add = runtime.termNewOp2(HVM4Runtime::OP_ADD, a, b);
    Term result = runtime.evaluateSNF(add);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 7u);
}

TEST_F(HVM4RuntimeTest, EvalAddNested) {
    // (1 + 2) + (3 + 4) -> 10
    Term add1 = runtime.termNewOp2(HVM4Runtime::OP_ADD, HVM4Runtime::termNewNum(1), HVM4Runtime::termNewNum(2));
    Term add2 = runtime.termNewOp2(HVM4Runtime::OP_ADD, HVM4Runtime::termNewNum(3), HVM4Runtime::termNewNum(4));
    Term add3 = runtime.termNewOp2(HVM4Runtime::OP_ADD, add1, add2);
    Term result = runtime.evaluateSNF(add3);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 10u);
}

TEST_F(HVM4RuntimeTest, EvalMul) {
    // 6 * 7 -> 42
    Term a = HVM4Runtime::termNewNum(6);
    Term b = HVM4Runtime::termNewNum(7);
    Term mul = runtime.termNewOp2(HVM4Runtime::OP_MUL, a, b);
    Term result = runtime.evaluateSNF(mul);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 42u);
}

TEST_F(HVM4RuntimeTest, EvalEq) {
    // 5 == 5 -> 1 (true)
    Term a = HVM4Runtime::termNewNum(5);
    Term b = HVM4Runtime::termNewNum(5);
    Term eq = runtime.termNewOp2(HVM4Runtime::OP_EQ, a, b);
    Term result = runtime.evaluateSNF(eq);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

TEST_F(HVM4RuntimeTest, EvalNeq) {
    // 5 == 6 -> 0 (false)
    Term a = HVM4Runtime::termNewNum(5);
    Term b = HVM4Runtime::termNewNum(6);
    Term eq = runtime.termNewOp2(HVM4Runtime::OP_EQ, a, b);
    Term result = runtime.evaluateSNF(eq);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 0u);
}

TEST_F(HVM4RuntimeTest, EvalLessThan) {
    // 3 < 5 -> 1
    Term a = HVM4Runtime::termNewNum(3);
    Term b = HVM4Runtime::termNewNum(5);
    Term lt = runtime.termNewOp2(HVM4Runtime::OP_LT, a, b);
    Term result = runtime.evaluateSNF(lt);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

TEST_F(HVM4RuntimeTest, EvalLessThanFalse) {
    // 5 < 3 -> 0
    Term a = HVM4Runtime::termNewNum(5);
    Term b = HVM4Runtime::termNewNum(3);
    Term lt = runtime.termNewOp2(HVM4Runtime::OP_LT, a, b);
    Term result = runtime.evaluateSNF(lt);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 0u);
}

TEST_F(HVM4RuntimeTest, EvalSub) {
    // 10 - 3 -> 7
    Term a = HVM4Runtime::termNewNum(10);
    Term b = HVM4Runtime::termNewNum(3);
    Term sub = runtime.termNewOp2(HVM4Runtime::OP_SUB, a, b);
    Term result = runtime.evaluateSNF(sub);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 7u);
}

TEST_F(HVM4RuntimeTest, EvalConstLambda) {
    // (λx. 100) 42 -> 100  (constant body, argument is unused)
    Term body = HVM4Runtime::termNewNum(100);
    Term lam = runtime.termNewLam(body);
    Term app = runtime.termNewApp(lam, HVM4Runtime::termNewNum(42));
    Term result = runtime.evaluateSNF(app);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 100u);
}

TEST_F(HVM4RuntimeTest, EvalLambdaWithOp) {
    // (λx. x + 1) 5 -> 6
    // Note: VAR must reference the lambda's heap location
    uint32_t lamLoc = runtime.allocateLamSlot();
    Term body = runtime.termNewOp2(HVM4Runtime::OP_ADD, HVM4Runtime::termNewVar(lamLoc), HVM4Runtime::termNewNum(1));
    Term lam = runtime.finalizeLam(lamLoc, body);
    Term app = runtime.termNewApp(lam, HVM4Runtime::termNewNum(5));
    Term result = runtime.evaluateSNF(app);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 6u);
}

// =============================================================================
// Extended Operator Tests (Session 14)
// =============================================================================

// Division by non-zero
TEST_F(HVM4RuntimeTest, EvalDivision) {
    Term a = HVM4Runtime::termNewNum(20);
    Term b = HVM4Runtime::termNewNum(4);
    Term div = runtime.termNewOp2(HVM4Runtime::OP_DIV, a, b);
    Term result = runtime.evaluateSNF(div);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 5u);
}

// Modulo operation
TEST_F(HVM4RuntimeTest, EvalModulo) {
    Term a = HVM4Runtime::termNewNum(17);
    Term b = HVM4Runtime::termNewNum(5);
    Term mod = runtime.termNewOp2(HVM4Runtime::OP_MOD, a, b);
    Term result = runtime.evaluateSNF(mod);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 2u);
}

// Bitwise AND
TEST_F(HVM4RuntimeTest, EvalBitwiseAnd) {
    Term a = HVM4Runtime::termNewNum(0b1010);
    Term b = HVM4Runtime::termNewNum(0b1100);
    Term andOp = runtime.termNewOp2(HVM4Runtime::OP_AND, a, b);
    Term result = runtime.evaluateSNF(andOp);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 0b1000u);
}

// Bitwise OR
TEST_F(HVM4RuntimeTest, EvalBitwiseOr) {
    Term a = HVM4Runtime::termNewNum(0b1010);
    Term b = HVM4Runtime::termNewNum(0b1100);
    Term orOp = runtime.termNewOp2(HVM4Runtime::OP_OR, a, b);
    Term result = runtime.evaluateSNF(orOp);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 0b1110u);
}

// Bitwise XOR
TEST_F(HVM4RuntimeTest, EvalBitwiseXor) {
    Term a = HVM4Runtime::termNewNum(0b1010);
    Term b = HVM4Runtime::termNewNum(0b1100);
    Term xorOp = runtime.termNewOp2(HVM4Runtime::OP_XOR, a, b);
    Term result = runtime.evaluateSNF(xorOp);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 0b0110u);
}

// Greater-than-or-equal
TEST_F(HVM4RuntimeTest, EvalGreaterOrEqual) {
    Term a = HVM4Runtime::termNewNum(5);
    Term b = HVM4Runtime::termNewNum(5);
    Term ge = runtime.termNewOp2(HVM4Runtime::OP_GE, a, b);
    Term result = runtime.evaluateSNF(ge);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

// Less-than-or-equal
TEST_F(HVM4RuntimeTest, EvalLessOrEqual) {
    Term a = HVM4Runtime::termNewNum(3);
    Term b = HVM4Runtime::termNewNum(5);
    Term le = runtime.termNewOp2(HVM4Runtime::OP_LE, a, b);
    Term result = runtime.evaluateSNF(le);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

// Greater-than
TEST_F(HVM4RuntimeTest, EvalGreaterThan) {
    Term a = HVM4Runtime::termNewNum(7);
    Term b = HVM4Runtime::termNewNum(3);
    Term gt = runtime.termNewOp2(HVM4Runtime::OP_GT, a, b);
    Term result = runtime.evaluateSNF(gt);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

// =============================================================================
// Edge Case Tests (Session 15)
// =============================================================================

// Equality of zero
TEST_F(HVM4RuntimeTest, Session15EvalEqualityZero) {
    Term a = HVM4Runtime::termNewNum(0);
    Term b = HVM4Runtime::termNewNum(0);
    Term eq = runtime.termNewOp2(HVM4Runtime::OP_EQ, a, b);
    Term result = runtime.evaluateSNF(eq);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

// Inequality of different values
TEST_F(HVM4RuntimeTest, Session15EvalInequalityDiff) {
    Term a = HVM4Runtime::termNewNum(5);
    Term b = HVM4Runtime::termNewNum(10);
    Term ne = runtime.termNewOp2(HVM4Runtime::OP_NE, a, b);
    Term result = runtime.evaluateSNF(ne);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

// Addition with large values
TEST_F(HVM4RuntimeTest, Session15EvalAddLarge) {
    Term a = HVM4Runtime::termNewNum(1000000);
    Term b = HVM4Runtime::termNewNum(2000000);
    Term add = runtime.termNewOp2(HVM4Runtime::OP_ADD, a, b);
    Term result = runtime.evaluateSNF(add);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 3000000u);
}

// Subtraction resulting in zero
TEST_F(HVM4RuntimeTest, Session15EvalSubToZero) {
    Term a = HVM4Runtime::termNewNum(42);
    Term b = HVM4Runtime::termNewNum(42);
    Term sub = runtime.termNewOp2(HVM4Runtime::OP_SUB, a, b);
    Term result = runtime.evaluateSNF(sub);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 0u);
}

// Multiplication by one
TEST_F(HVM4RuntimeTest, Session15EvalMulByOne) {
    Term a = HVM4Runtime::termNewNum(99);
    Term b = HVM4Runtime::termNewNum(1);
    Term mul = runtime.termNewOp2(HVM4Runtime::OP_MUL, a, b);
    Term result = runtime.evaluateSNF(mul);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 99u);
}

// Multiplication by zero
TEST_F(HVM4RuntimeTest, Session15EvalMulByZero) {
    Term a = HVM4Runtime::termNewNum(999);
    Term b = HVM4Runtime::termNewNum(0);
    Term mul = runtime.termNewOp2(HVM4Runtime::OP_MUL, a, b);
    Term result = runtime.evaluateSNF(mul);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 0u);
}

// =============================================================================
// Complex Operation Tests (Session 16)
// =============================================================================

// Chained operations
TEST_F(HVM4RuntimeTest, Session16ChainedOps) {
    // (5 + 3) * 2 = 16
    Term a = HVM4Runtime::termNewNum(5);
    Term b = HVM4Runtime::termNewNum(3);
    Term c = HVM4Runtime::termNewNum(2);
    Term add = runtime.termNewOp2(HVM4Runtime::OP_ADD, a, b);
    Term mul = runtime.termNewOp2(HVM4Runtime::OP_MUL, add, c);
    Term result = runtime.evaluateSNF(mul);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 16u);
}

// Comparison chain
TEST_F(HVM4RuntimeTest, Session16ComparisonChain) {
    // (5 < 10) == 1
    Term a = HVM4Runtime::termNewNum(5);
    Term b = HVM4Runtime::termNewNum(10);
    Term one = HVM4Runtime::termNewNum(1);
    Term lt = runtime.termNewOp2(HVM4Runtime::OP_LT, a, b);
    Term eq = runtime.termNewOp2(HVM4Runtime::OP_EQ, lt, one);
    Term result = runtime.evaluateSNF(eq);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

// Division with remainder check
TEST_F(HVM4RuntimeTest, Session16DivisionRemainder) {
    // 17 / 5 = 3, 17 % 5 = 2
    Term a = HVM4Runtime::termNewNum(17);
    Term b = HVM4Runtime::termNewNum(5);
    Term div = runtime.termNewOp2(HVM4Runtime::OP_DIV, a, b);

    Term divResult = runtime.evaluateSNF(div);
    EXPECT_EQ(HVM4Runtime::termVal(divResult), 3u);

    runtime.reset();
    Term a2 = HVM4Runtime::termNewNum(17);
    Term b2 = HVM4Runtime::termNewNum(5);
    Term mod = runtime.termNewOp2(HVM4Runtime::OP_MOD, a2, b2);
    Term modResult = runtime.evaluateSNF(mod);
    EXPECT_EQ(HVM4Runtime::termVal(modResult), 2u);
}

// =============================================================================
// Session 25: Extended Runtime Tests
// =============================================================================

// Division by one (identity)
TEST_F(HVM4RuntimeTest, Session25DivisionByOne) {
    Term a = HVM4Runtime::termNewNum(42);
    Term b = HVM4Runtime::termNewNum(1);
    Term div = runtime.termNewOp2(HVM4Runtime::OP_DIV, a, b);
    Term result = runtime.evaluateSNF(div);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 42u);
}

// Self-division
TEST_F(HVM4RuntimeTest, Session25SelfDivision) {
    Term a = HVM4Runtime::termNewNum(100);
    Term b = HVM4Runtime::termNewNum(100);
    Term div = runtime.termNewOp2(HVM4Runtime::OP_DIV, a, b);
    Term result = runtime.evaluateSNF(div);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

// Modulo with result zero
TEST_F(HVM4RuntimeTest, Session25ModuloResultZero) {
    Term a = HVM4Runtime::termNewNum(20);
    Term b = HVM4Runtime::termNewNum(5);
    Term mod = runtime.termNewOp2(HVM4Runtime::OP_MOD, a, b);
    Term result = runtime.evaluateSNF(mod);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 0u);
}

// Left shift
TEST_F(HVM4RuntimeTest, Session25LeftShift) {
    Term a = HVM4Runtime::termNewNum(1);
    Term b = HVM4Runtime::termNewNum(4);
    Term shl = runtime.termNewOp2(HVM4Runtime::OP_SHL, a, b);
    Term result = runtime.evaluateSNF(shl);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 16u);  // 1 << 4 = 16
}

// Right shift
TEST_F(HVM4RuntimeTest, Session25RightShift) {
    Term a = HVM4Runtime::termNewNum(64);
    Term b = HVM4Runtime::termNewNum(3);
    Term shr = runtime.termNewOp2(HVM4Runtime::OP_SHR, a, b);
    Term result = runtime.evaluateSNF(shr);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 8u);  // 64 >> 3 = 8
}

// Not equal operator
TEST_F(HVM4RuntimeTest, Session25NotEqualTrue) {
    Term a = HVM4Runtime::termNewNum(5);
    Term b = HVM4Runtime::termNewNum(10);
    Term ne = runtime.termNewOp2(HVM4Runtime::OP_NE, a, b);
    Term result = runtime.evaluateSNF(ne);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

// Not equal operator (same values)
TEST_F(HVM4RuntimeTest, Session25NotEqualFalse) {
    Term a = HVM4Runtime::termNewNum(42);
    Term b = HVM4Runtime::termNewNum(42);
    Term ne = runtime.termNewOp2(HVM4Runtime::OP_NE, a, b);
    Term result = runtime.evaluateSNF(ne);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 0u);
}

// Nested lambda application
TEST_F(HVM4RuntimeTest, Session25NestedLambda) {
    // (λx. (λy. x + y) 3) 5 -> 8
    // Inner: λy. x + y where x is captured from outer
    uint32_t outerLoc = runtime.allocateLamSlot();
    uint32_t innerLoc = runtime.allocateLamSlot();

    // Inner body: x + y (x from outer, y from inner)
    Term innerBody = runtime.termNewOp2(HVM4Runtime::OP_ADD,
                                         HVM4Runtime::termNewVar(outerLoc),
                                         HVM4Runtime::termNewVar(innerLoc));
    Term innerLam = runtime.finalizeLam(innerLoc, innerBody);

    // Inner app: inner 3
    Term innerApp = runtime.termNewApp(innerLam, HVM4Runtime::termNewNum(3));

    // Outer: λx. innerApp
    Term outerLam = runtime.finalizeLam(outerLoc, innerApp);

    // Outer app: outer 5
    Term outerApp = runtime.termNewApp(outerLam, HVM4Runtime::termNewNum(5));

    Term result = runtime.evaluateSNF(outerApp);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 8u);
}

// Multi-use variable with duplication
TEST_F(HVM4RuntimeTest, Session25MultiUseVariable) {
    // (λx. x + x) 7 -> 14
    uint32_t lamLoc = runtime.allocateLamSlot();
    Term body = runtime.termNewOp2(HVM4Runtime::OP_ADD,
                                    HVM4Runtime::termNewVar(lamLoc),
                                    HVM4Runtime::termNewVar(lamLoc));
    Term lam = runtime.finalizeLam(lamLoc, body);
    Term app = runtime.termNewApp(lam, HVM4Runtime::termNewNum(7));
    Term result = runtime.evaluateSNF(app);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 14u);
}

// Large number operations
TEST_F(HVM4RuntimeTest, Session25LargeNumberAdd) {
    Term a = HVM4Runtime::termNewNum(1000000);
    Term b = HVM4Runtime::termNewNum(1000000);
    Term add = runtime.termNewOp2(HVM4Runtime::OP_ADD, a, b);
    Term result = runtime.evaluateSNF(add);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 2000000u);
}

// Power of 2 operations
TEST_F(HVM4RuntimeTest, Session25PowerOfTwoOps) {
    // 256 * 256 = 65536
    Term a = HVM4Runtime::termNewNum(256);
    Term b = HVM4Runtime::termNewNum(256);
    Term mul = runtime.termNewOp2(HVM4Runtime::OP_MUL, a, b);
    Term result = runtime.evaluateSNF(mul);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 65536u);
}

// Chained comparisons
TEST_F(HVM4RuntimeTest, Session25ChainedComparisons) {
    // (5 < 10) == (10 < 20) -> 1 == 1 -> 1
    Term lt1 = runtime.termNewOp2(HVM4Runtime::OP_LT,
                                   HVM4Runtime::termNewNum(5),
                                   HVM4Runtime::termNewNum(10));
    Term lt2 = runtime.termNewOp2(HVM4Runtime::OP_LT,
                                   HVM4Runtime::termNewNum(10),
                                   HVM4Runtime::termNewNum(20));
    Term eq = runtime.termNewOp2(HVM4Runtime::OP_EQ, lt1, lt2);
    Term result = runtime.evaluateSNF(eq);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

// Complex arithmetic expression
TEST_F(HVM4RuntimeTest, Session25ComplexArithmetic) {
    // ((10 + 5) * 2) - 10 = 20
    Term add = runtime.termNewOp2(HVM4Runtime::OP_ADD,
                                   HVM4Runtime::termNewNum(10),
                                   HVM4Runtime::termNewNum(5));
    Term mul = runtime.termNewOp2(HVM4Runtime::OP_MUL,
                                   add,
                                   HVM4Runtime::termNewNum(2));
    Term sub = runtime.termNewOp2(HVM4Runtime::OP_SUB,
                                   mul,
                                   HVM4Runtime::termNewNum(10));
    Term result = runtime.evaluateSNF(sub);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 20u);
}

// Deeply nested operations
TEST_F(HVM4RuntimeTest, Session25DeepNesting) {
    // (((1 + 1) + 1) + 1) + 1 = 5
    Term n1 = runtime.termNewOp2(HVM4Runtime::OP_ADD,
                                  HVM4Runtime::termNewNum(1),
                                  HVM4Runtime::termNewNum(1));
    Term n2 = runtime.termNewOp2(HVM4Runtime::OP_ADD, n1, HVM4Runtime::termNewNum(1));
    Term n3 = runtime.termNewOp2(HVM4Runtime::OP_ADD, n2, HVM4Runtime::termNewNum(1));
    Term n4 = runtime.termNewOp2(HVM4Runtime::OP_ADD, n3, HVM4Runtime::termNewNum(1));
    Term result = runtime.evaluateSNF(n4);
    EXPECT_EQ(HVM4Runtime::termTag(result), HVM4Runtime::TAG_NUM);
    EXPECT_EQ(HVM4Runtime::termVal(result), 5u);
}

// Comparison edge cases
TEST_F(HVM4RuntimeTest, Session25ComparisonEdgeCases) {
    // Zero comparisons
    Term zeroLtOne = runtime.termNewOp2(HVM4Runtime::OP_LT,
                                         HVM4Runtime::termNewNum(0),
                                         HVM4Runtime::termNewNum(1));
    Term result = runtime.evaluateSNF(zeroLtOne);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);

    runtime.reset();

    // Equal zeros
    Term zeroEqZero = runtime.termNewOp2(HVM4Runtime::OP_EQ,
                                          HVM4Runtime::termNewNum(0),
                                          HVM4Runtime::termNewNum(0));
    result = runtime.evaluateSNF(zeroEqZero);
    EXPECT_EQ(HVM4Runtime::termVal(result), 1u);
}

} // namespace hvm4
} // namespace nix

#endif // NIX_USE_HVM4
