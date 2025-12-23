# 9. Arithmetic Primops

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).

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
