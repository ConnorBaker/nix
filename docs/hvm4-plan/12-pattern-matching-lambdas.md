# 12. Pattern-Matching Lambdas

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).
>
> Status (2025-12-28): Pattern lambdas are implemented by desugaring to attr
> lookups. Defaults and `@`-patterns are supported. Extra-attribute checks for
> missing ellipsis are not implemented, and missing required attrs yield ERA
> rather than a structured error.
>
> Note: This file focuses on the pattern-lambda implementation only.

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

## Current Implementation: Desugar to Attribute Destructuring (Option A, partial)

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

    if (tag == CTR_ATS) {
        std::cerr << indent << "#Ats{" << std::endl;
        Term list = getField(attrs, 0, runtime);
        dumpAttrList(list, runtime, depth + 1);
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


## Future Work

- Implement extra-attribute checks when `...` is not present.
- Replace ERA-based failures with structured errors.
- Add tests for nested `with` + pattern lambdas and missing-attr behavior.
