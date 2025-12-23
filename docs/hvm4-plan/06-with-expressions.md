# 6. With Expressions

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).

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
