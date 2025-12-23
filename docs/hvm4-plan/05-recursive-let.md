# 5. Recursive Let / Rec

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).

Nix supports mutually recursive bindings with cycle detection.

## Nix Implementation Details

```cpp
// rec { a = ...; b = a + 1; }
// Creates environment where all bindings are visible

// Cycle detection via "black hole":
v.mkBlackhole();  // Mark as being evaluated
expr->eval(...);  // If we hit this value again → infinite recursion
```

## Option A: Y-Combinator Encoding

Encode mutual recursion via fixpoint combinators.

```hvm4
// Y combinator
@Y = λf. (λx. f(x(x))) (λx. f(x(x)))

// For: rec { a = b + 1; b = 10; }
// Encode as: Y(λself. { a = self.b + 1; b = 10; })

// Mutual recursion with record:
@rec_attrs = @Y(λself. #Attrs{
  #Attr{"a", self.b + 1, ...},
  #Attr{"b", 10, ...}
})
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Cycle detection | None - infinite loop on cycles |
| Implementation | Simple |
| Performance | Extra indirection |

## Option B: Lazy References with Blackhole

Mirror Nix's approach using explicit thunks and blackhole markers.

```hvm4
// Thunk = #Thunk{env, expr} | #Blackhole{} | #Evaluated{value}

@force = λthunk. λ{
  #Evaluated: λv. v
  #Blackhole: @error("infinite recursion")
  #Thunk: λenv.λexpr.
    // Would need mutable state to mark blackhole
    // Not directly expressible in pure HVM4
}
```

**Problem:** HVM4 is pure - can't mutate thunk to blackhole.

## Option C: Static Cycle Detection

Detect cycles at compile time via dependency analysis.

```hvm4
// During compilation:
// 1. Build dependency graph of rec bindings
// 2. Detect strongly connected components
// 3. Reject or specially handle cycles

// For non-cyclic rec:
// Topologically sort and compile as nested lets

// let b = 10; in let a = b + 1; in { a; b; }
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Cycle detection | At compile time |
| False positives | Rejects some valid Nix (lazy cycles) |
| Runtime | No overhead |

## Option D: Fuel-Based Evaluation

Add "fuel" parameter to bound recursion depth.

```hvm4
@eval_rec = λfuel.λenv.λexpr.
  (fuel == 0) .&. #Error{"recursion limit"} .|.
  // Evaluate with decremented fuel
  @eval_expr(fuel - 1, env, expr)
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Cycle detection | Eventually - via fuel exhaustion |
| False positives | May reject deep recursion |
| Configurability | Fuel limit tunable |

## CHOSEN: Static Topo-Sort + Y-Combinator Fallback (Options C + A)

**Rationale:**
- Most `rec` usages in real Nix code are acyclic (can be topologically sorted)
- Y-combinator handles true mutual recursion when needed
- Static analysis at compile time avoids runtime overhead for simple cases
- HVM4's optimal lazy evaluation works well with Y-combinator approach

**Strategy:**
1. Build dependency graph from rec bindings
2. Attempt topological sort
3. If acyclic: emit as nested lets in dependency order (fast path)
4. If cyclic: use Y-combinator to create fixpoint (correct but slower)

### Detailed Implementation Steps

**Files Modified:**
- `src/libexpr/hvm4/hvm4-compiler.cc` (add rec handling)

#### Step 1: Build Dependency Graph

```cpp
// In hvm4-compiler.cc
struct RecBindingInfo {
    Symbol name;
    const Expr* expr;
    std::set<Symbol> dependencies;  // Other rec bindings this depends on
};

std::vector<RecBindingInfo> analyzeRecBindings(const ExprAttrs& attrs,
                                                const SymbolTable& symbols) {
    std::vector<RecBindingInfo> bindings;

    for (auto& [name, attrDef] : attrs.attrs) {
        RecBindingInfo info;
        info.name = name;
        info.expr = attrDef.e;

        // Find free variables in the expression
        std::set<Symbol> freeVars;
        collectFreeVars(*attrDef.e, freeVars);

        // Filter to only other rec bindings
        for (auto& [otherName, _] : attrs.attrs) {
            if (freeVars.count(otherName) && otherName != name) {
                info.dependencies.insert(otherName);
            }
        }

        bindings.push_back(std::move(info));
    }

    return bindings;
}
```

#### Step 2: Attempt Topological Sort

```cpp
std::optional<std::vector<Symbol>> topologicalSort(
    const std::vector<RecBindingInfo>& bindings) {

    std::map<Symbol, int> inDegree;
    std::map<Symbol, std::vector<Symbol>> dependents;

    // Initialize
    for (const auto& b : bindings) {
        inDegree[b.name] = b.dependencies.size();
        for (const auto& dep : b.dependencies) {
            dependents[dep].push_back(b.name);
        }
    }

    // Kahn's algorithm
    std::queue<Symbol> ready;
    for (const auto& b : bindings) {
        if (inDegree[b.name] == 0) {
            ready.push(b.name);
        }
    }

    std::vector<Symbol> order;
    while (!ready.empty()) {
        Symbol current = ready.front();
        ready.pop();
        order.push_back(current);

        for (const auto& dependent : dependents[current]) {
            if (--inDegree[dependent] == 0) {
                ready.push(dependent);
            }
        }
    }

    // Check for cycles
    if (order.size() != bindings.size()) {
        return std::nullopt;  // Has cycles
    }

    return order;
}
```

#### Step 3: Emit Acyclic Rec as Nested Lets

```cpp
Term HVM4Compiler::emitAcyclicRec(const ExprAttrs& attrs,
                                   const std::vector<Symbol>& order,
                                   CompileContext& ctx) {
    // Build mapping from names to expressions
    std::map<Symbol, const Expr*> bindings;
    for (auto& [name, attrDef] : attrs.attrs) {
        bindings[name] = attrDef.e;
    }

    // Emit as nested lets in dependency order
    // let b = 10; in let a = b + 1; in { a; b; }

    // Start with innermost body (the attrset result)
    Term body = emitFinalAttrs(attrs, ctx);

    // Wrap with lets from back to front
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        Symbol name = *it;
        const Expr* expr = bindings[name];

        Term value = emit(*expr, ctx);
        body = emitLet(name, value, body, ctx);
    }

    return body;
}
```

#### Step 4: Emit Cyclic Rec with Y-Combinator

```cpp
// Y combinator definition (pre-compiled as @def)
// @Y = λf. (λx. f(x(x))) (λx. f(x(x)))

Term HVM4Compiler::emitCyclicRec(const ExprAttrs& attrs,
                                  CompileContext& ctx) {
    // Transform: rec { a = f(b); b = g(a); }
    // To: Y(λself. { a = f(self.b); b = g(self.a); })

    // Create lambda parameter for 'self'
    Symbol selfSym = ctx.symbols().create("$self");
    ctx.pushScope();
    ctx.addBinding(selfSym, makeSelfRef(ctx));

    // Build attrset body with self-references replaced by self.name lookups
    std::vector<std::pair<uint32_t, Term>> attrTerms;

    for (auto& [name, attrDef] : attrs.attrs) {
        // In this scope, references to other rec bindings become self.name
        Term value = emitWithSelfLookups(*attrDef.e, attrs, selfSym, ctx);
        attrTerms.push_back({name.id, value});
    }

    Term attrsBody = buildAttrsFromTerms(attrTerms, ctx);

    ctx.popScope();

    // Wrap in lambda
    Term recLambda = emitLambda(selfSym, attrsBody, ctx);

    // Apply Y combinator
    Term yCombinator = getYCombinatorRef(ctx);
    return emitApp(yCombinator, recLambda, ctx);
}

Term emitWithSelfLookups(const Expr& expr, const ExprAttrs& recAttrs,
                          Symbol selfSym, CompileContext& ctx) {
    // When we encounter a variable reference to another rec binding,
    // emit self.name instead of direct reference

    if (auto* var = dynamic_cast<const ExprVar*>(&expr)) {
        // Check if this variable is one of our rec bindings
        if (recAttrs.attrs.count(var->name)) {
            // Emit: self.name
            Term self = ctx.lookupBinding(selfSym);
            return emitSelect(self, var->name, ctx);
        }
    }

    // Recursively process subexpressions
    return emitDefault(expr, ctx);
}
```

#### Step 5: Add Support in canCompileWithScope

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprAttrs*>(&expr)) {
    if (e->recursive) {
        // Analyze dependency structure
        auto bindings = analyzeRecBindings(*e, symbols);
        auto order = topologicalSort(bindings);

        if (order) {
            // Acyclic - all bindings must be compilable
            for (auto& [name, attrDef] : e->attrs) {
                if (!canCompileWithScope(*attrDef.e, scope)) return false;
            }
            return true;
        } else {
            // Cyclic - need Y-combinator, still check bindings
            for (auto& [name, attrDef] : e->attrs) {
                if (!canCompileWithScope(*attrDef.e, scope)) return false;
            }
            return true;  // Y-combinator handles cycles
        }
    }
    // ... non-recursive attrs handling
}
```

#### Step 6: Add Comprehensive Tests

```cpp
// In hvm4.cc test file

// Acyclic rec (should use fast path)
TEST_F(HVM4BackendTest, RecAcyclic) {
    auto v = eval("rec { b = 10; a = b + 1; }", true);
    ASSERT_EQ(v.type(), nAttrs);
    auto a = v.attrs()->get(state.symbols.create("a"));
    state.forceValue(*a->value, noPos);
    ASSERT_EQ(a->value->integer().value, 11);
}

TEST_F(HVM4BackendTest, RecAcyclicChain) {
    auto v = eval("rec { c = b + 1; b = a + 1; a = 1; }", true);
    auto c = v.attrs()->get(state.symbols.create("c"));
    state.forceValue(*c->value, noPos);
    ASSERT_EQ(c->value->integer().value, 3);
}

// Cyclic rec (needs Y-combinator)
TEST_F(HVM4BackendTest, RecCyclicMutual) {
    // Mutual recursion: even/odd
    auto v = eval(R"(
        rec {
            even = n: if n == 0 then true else odd (n - 1);
            odd = n: if n == 0 then false else even (n - 1);
        }
    )", true);

    auto even = v.attrs()->get(state.symbols.create("even"));
    // Call even(4) - should be true
    // (Requires function call support)
}

TEST_F(HVM4BackendTest, RecSelfReference) {
    // Self-referential binding (lazy)
    auto v = eval(R"(
        rec {
            xs = [1 2 3] ++ xs;  # Infinite list
        }
    )", true);
    // Only valid because Nix is lazy - don't force xs fully
}

// Let rec equivalence
TEST_F(HVM4BackendTest, LetRecEquivalent) {
    auto v = eval("let a = b + 1; b = 10; in a", true);
    ASSERT_EQ(v.integer().value, 11);
}

// === Additional Recursive Let Edge Cases ===

TEST_F(HVM4BackendTest, RecMultipleDependencies) {
    auto v = eval("rec { sum = a + b + c; a = 1; b = 2; c = 3; }", true);
    auto sum = v.attrs()->get(state.symbols.create("sum"));
    state.forceValue(*sum->value, noPos);
    ASSERT_EQ(sum->value->integer().value, 6);
}

TEST_F(HVM4BackendTest, RecWithFunction) {
    auto v = eval(R"(
        rec {
            double = x: x * 2;
            result = double 21;
        }
    )", true);
    auto result = v.attrs()->get(state.symbols.create("result"));
    state.forceValue(*result->value, noPos);
    ASSERT_EQ(result->value->integer().value, 42);
}

TEST_F(HVM4BackendTest, RecFactorial) {
    auto v = eval(R"(
        rec {
            fact = n: if n <= 1 then 1 else n * fact (n - 1);
        }
    )", true);
    auto fact = v.attrs()->get(state.symbols.create("fact"));
    // Would need to call fact 5 and check result is 120
}

TEST_F(HVM4BackendTest, RecFibonacci) {
    auto v = eval(R"(
        rec {
            fib = n: if n <= 1 then n else fib (n - 1) + fib (n - 2);
        }
    )", true);
    // Self-recursive function
}

TEST_F(HVM4BackendTest, RecEmptyAttrs) {
    auto v = eval("rec { }", true);
    ASSERT_EQ(v.type(), nAttrs);
    ASSERT_EQ(v.attrs()->size(), 0);
}

TEST_F(HVM4BackendTest, RecSingleBinding) {
    auto v = eval("rec { x = 42; }", true);
    auto x = v.attrs()->get(state.symbols.create("x"));
    state.forceValue(*x->value, noPos);
    ASSERT_EQ(x->value->integer().value, 42);
}

TEST_F(HVM4BackendTest, RecLazyThunkNotForced) {
    // rec should create lazy thunks
    auto v = eval("rec { a = 1; b = throw \"lazy\"; }", true);
    auto a = v.attrs()->get(state.symbols.create("a"));
    state.forceValue(*a->value, noPos);
    ASSERT_EQ(a->value->integer().value, 1);  // Should not throw
}

TEST_F(HVM4BackendTest, RecWithInherit) {
    auto v = eval("let x = 1; in rec { inherit x; y = x + 1; }", true);
    auto y = v.attrs()->get(state.symbols.create("y"));
    state.forceValue(*y->value, noPos);
    ASSERT_EQ(y->value->integer().value, 2);
}

TEST_F(HVM4BackendTest, RecAccessDuringConstruction) {
    // Access a rec attr during construction of another
    auto v = eval("rec { list = [a b]; a = 1; b = 2; }", true);
    auto list = v.attrs()->get(state.symbols.create("list"));
    state.forceValue(*list->value, noPos);
    ASSERT_EQ(list->value->listSize(), 2);
}
```

---
