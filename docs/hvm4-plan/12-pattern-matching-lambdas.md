# 12. Pattern-Matching Lambdas

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).

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
> constructor names used in the Nix HVM4 encoding, see the [HVM4 Encoding Strategy](./00-overview.md#hvm4-encoding-strategy) table.

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
