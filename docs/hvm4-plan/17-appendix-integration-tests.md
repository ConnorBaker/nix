# Appendix: Integration Tests

These tests verify that multiple features work correctly together, testing the interactions between different data types and language constructs.

Status (2025-12-28): Most of these tests are aspirational and will fail with the
current HVM4 backend because many builtins, imports, derivations, and error
handling paths are not implemented. Treat this as a future test plan.

## Attribute Sets + Lists

```cpp
TEST_F(HVM4BackendTest, IntegrationAttrsWithLists) {
    auto v = eval("{ xs = [1 2 3]; ys = [4 5 6]; }.xs ++ { xs = [1 2 3]; ys = [4 5 6]; }.ys", true);
    ASSERT_EQ(v.listSize(), 6);
}

TEST_F(HVM4BackendTest, IntegrationListOfAttrs) {
    auto v = eval("builtins.map (x: x.a) [{ a = 1; } { a = 2; } { a = 3; }]", true);
    ASSERT_EQ(v.listSize(), 3);
}
```

## Strings + Interpolation + Attribute Sets

```cpp
TEST_F(HVM4BackendTest, IntegrationStringInterpolationAttrs) {
    auto v = eval("let x = { name = \"world\"; }; in \"hello ${x.name}\"", true);
    ASSERT_EQ(v.string_view(), "hello world");
}

TEST_F(HVM4BackendTest, IntegrationAttrNamesAsStrings) {
    auto v = eval("let name = \"foo\"; in { ${name} = 42; }.foo", true);
    ASSERT_EQ(v.integer().value, 42);
}
```

## Functions + Pattern Matching + Attribute Sets

```cpp
TEST_F(HVM4BackendTest, IntegrationPatternMatchingPipeline) {
    auto v = eval(R"(
        let
          f = { a, b ? 0 }: { c = a + b; };
          g = { c }: c * 2;
        in g (f { a = 5; b = 3; })
    )", true);
    ASSERT_EQ(v.integer().value, 16);
}

TEST_F(HVM4BackendTest, IntegrationOverlay) {
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
    auto v = eval(R"(
        let
          lib = { add = a: b: a + b; mul = a: b: a * b; };
          nums = { x = 3; y = 4; };
        in with lib; with nums; mul (add x y) x
    )", true);
    ASSERT_EQ(v.integer().value, 21);
}
```

## Complex NixOS-like Patterns

```cpp
TEST_F(HVM4BackendTest, IntegrationModuleSystem) {
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

## Higher-Order Functions

```cpp
TEST_F(HVM4BackendTest, IntegrationHigherOrderFunctions) {
    auto v = eval(R"(
        let
          compose = f: g: x: f (g x);
          double = x: x * 2;
          inc = x: x + 1;
          doubleThenInc = compose inc double;
        in doubleThenInc 5
    )", true);
    ASSERT_EQ(v.integer().value, 11);
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

## Error Propagation

```cpp
TEST_F(HVM4BackendTest, IntegrationErrorInNestedExpr) {
    EXPECT_THROW(eval(R"(
        let
          a = { x = { y = { z = throw "deep"; }; }; };
        in a.x.y.z
    )", true), EvalError);
}

TEST_F(HVM4BackendTest, IntegrationErrorMessagePreserved) {
    try {
        eval("{ a = 1; }.b", true);
        FAIL() << "Expected exception";
    } catch (const EvalError& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("b") != std::string::npos);
    }
}
```
