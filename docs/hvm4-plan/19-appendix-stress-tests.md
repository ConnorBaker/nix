# Appendix: Stress Tests

These tests verify the HVM4 backend handles edge cases, large inputs, and pathological patterns correctly.

Status (2025-12-28): Most tests below are aspirational and will not pass with the
current backend due to missing builtins, imports, derivations, and error handling.

## Memory and Scale Tests

```cpp
TEST_F(HVM4BackendTest, StressLargeListLength) {
    auto v = eval("builtins.length (builtins.genList (x: x) 10000)", true);
    ASSERT_EQ(v.integer().value, 10000);
}

TEST_F(HVM4BackendTest, StressLazyListElements) {
    auto v = eval(R"(
        builtins.head (builtins.genList (x:
            if x == 0 then 42 else throw "should not evaluate"
        ) 10000)
    )", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, StressListConcat) {
    auto v = eval(R"(
        let
            small = [1 2 3];
            concat100 = builtins.foldl' (acc: _: acc ++ small) [] (builtins.genList (x: x) 100);
        in builtins.length concat100
    )", true);
    ASSERT_EQ(v.integer().value, 300);
}

TEST_F(HVM4BackendTest, StressLargeAttrset) {
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
    auto v = eval(R"(
        let
            base = { a = 1; };
            update = i: { "b${toString i}" = i; };
            result = builtins.foldl' (acc: i: acc // update i) base (builtins.genList (x: x) 50);
        in builtins.length (builtins.attrNames result)
    )", true);
    ASSERT_EQ(v.integer().value, 51);
}

TEST_F(HVM4BackendTest, StressLongString) {
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
    auto v = eval(R"(
        let
            count = n: if n <= 0 then 0 else 1 + count (n - 1);
        in count 500
    )", true);
    ASSERT_EQ(v.integer().value, 500);
}

TEST_F(HVM4BackendTest, StressMutualRecursion) {
    auto v = eval(R"(
        let
            isEven = n: if n == 0 then true else isOdd (n - 1);
            isOdd = n: if n == 0 then false else isEven (n - 1);
        in isEven 200
    )", true);
    ASSERT_TRUE(v.boolean());
}

TEST_F(HVM4BackendTest, StressFibonacci) {
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
    auto v = eval("null", true);
    ASSERT_EQ(v.type(), nNull);
}
```

## BigInt Edge Cases

```cpp
TEST_F(HVM4BackendTest, StressBigIntBoundary) {
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

## Pathological Pattern Tests

```cpp
TEST_F(HVM4BackendTest, StressDeepNesting) {
    auto v = eval(R"(
        let
            nest = n: if n <= 0 then 42 else { inner = nest (n - 1); };
            deep = nest 20;
        in deep.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner.inner
    )", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, StressWideAttrset) {
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
    auto v = eval(R"(
        with { a = 1; };
        with { b = 2; };
        with { c = 3; };
        a + b + c
    )", true);
    ASSERT_EQ(v.integer().value, 6);
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

## Function Application Tests

```cpp
TEST_F(HVM4BackendTest, StressHigherOrderFunctions) {
    auto v = eval("builtins.map (x: x * 2) [1 2 3 4 5]", true);
    ASSERT_EQ(v.listSize(), 5);
}

TEST_F(HVM4BackendTest, StressFilter) {
    auto v = eval("builtins.filter (x: x > 3) [1 2 3 4 5]", true);
    ASSERT_EQ(v.listSize(), 2);
}

TEST_F(HVM4BackendTest, StressFoldl) {
    auto v = eval("builtins.foldl' (acc: x: acc + x) 0 [1 2 3 4 5]", true);
    ASSERT_EQ(v.integer().value, 15);
}

TEST_F(HVM4BackendTest, StressRecursiveLambda) {
    auto v = eval(R"(
        let
            factorial = n: if n <= 1 then 1 else n * factorial (n - 1);
        in factorial 10
    )", true);
    ASSERT_EQ(v.integer().value, 3628800);
}
```
