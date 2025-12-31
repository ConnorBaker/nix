# 8. Derivations

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).
>
> Status (2025-12-28): Derivations are not implemented in the HVM4 backend.
> `derivationStrict` and related primops fall back to the standard evaluator.

Derivations are the core of Nix - they define build actions.

## Nix Implementation Details

```cpp
// derivationStrict:
// 1. Collect all attributes
// 2. Coerce to strings (accumulating context)
// 3. Process context → inputDrvs, inputSrcs
// 4. Compute output paths based on derivation type
// 5. Write .drv file to store
// 6. Return attrset with drvPath, out, etc.
```

## Option A: Pure Derivation Records

Derivations as pure data structures, no store writes during eval.

```hvm4
// Derivation = #Drv{name, builder, args, env, outputs, inputDrvs, inputSrcs}

@derivation_strict = λattrs.
  // Collect and validate attributes
  // Return Drv record, don't write to store
  #Drv{
    name: @get_attr("name", attrs),
    builder: @get_attr("builder", attrs),
    // ...
  }

// Store writing happens after HVM4 evaluation completes
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Purity | Preserved |
| IFD | Not supported (no store paths during eval) |
| Post-processing | Need to traverse result for Drv records |

## Option B: Effect-Based Derivations

Model derivation creation as an effect.

```hvm4
// Effect = #CreateDrv{drv_attrs, continuation}

@derivation_strict = λattrs.
  #CreateDrv{attrs, λdrv_result. drv_result}

// External handler:
// 1. Processes attrs, writes .drv
// 2. Returns attrset with paths
// 3. Continues evaluation
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| IFD | Supported via effect handler |
| Complexity | Effect system required |
| Semantics | Matches Nix |

## Option C: Staged Compilation

Separate derivation computation from evaluation.

```hvm4
// Stage 1: Identify derivation calls, extract to separate phase
// Stage 2: Compute all derivations (in Nix/external)
// Stage 3: Substitute results and continue HVM4 evaluation

// Requires multiple passes but keeps HVM4 pure
```

## Original Plan (not implemented): Pure Derivation Records (Phase 1: Option A)

**Rationale:**
- Keeps HVM4 evaluation pure and deterministic
- Derivation records can be collected and processed after evaluation
- Store writes happen in a single post-processing phase
- Effect-based approach (Option B) can be added later for IFD

**Strategy:**
1. Compile `derivationStrict` to create pure `#Drv{...}` records
2. HVM4 evaluates without side effects
3. After evaluation, traverse result to find all Drv records
4. Write .drv files to store and update references

**Encoding:**
```hvm4
// Derivation = #Drv{name, system, builder, args, env, outputs}
// name: String
// system: String
// builder: String (path to builder)
// args: List of Strings
// env: AttrSet of Strings
// outputs: List of output names

#Drv{
  #Str{"hello", #NoC{}},           // name
  #Str{"x86_64-linux", #NoC{}},    // system
  #Str{"/bin/sh", #NoC{}},         // builder
  #Lst{2, #Con{"-c", #Con{"echo hello", #Nil{}}}},  // args
  #ABs{...},                        // env
  #Lst{1, #Con{"out", #Nil{}}}     // outputs
}
```

### Detailed Implementation Steps

**New Files:**
- `src/libexpr/hvm4/hvm4-derivation.cc`
- `src/libexpr/include/nix/expr/hvm4/hvm4-derivation.hh`

#### Step 1: Define Derivation Encoding

```cpp
// hvm4-derivation.hh
namespace nix::hvm4 {

// Constructor for derivation record
constexpr uint32_t DRV_RECORD = /* encode "#Drv" */;

// Create a derivation record from attributes
Term makeDerivationRecord(
    Term name,
    Term system,
    Term builder,
    Term args,
    Term env,
    Term outputs,
    HVM4Runtime& runtime);

// Check if term is a derivation record
bool isDerivationRecord(Term term);

// Extract derivation fields
Term getDrvName(Term drv, const HVM4Runtime& runtime);
Term getDrvSystem(Term drv, const HVM4Runtime& runtime);
// ... etc

}  // namespace nix::hvm4
```

#### Step 2: Compile derivationStrict

```cpp
Term HVM4Compiler::emitDerivationStrict(const ExprCall& call, CompileContext& ctx) {
    // derivationStrict takes one argument: the attribute set
    Term attrsArg = emit(*call.args[0], ctx);

    // Extract required attributes
    Term name = emitGetAttr(attrsArg, "name", ctx);
    Term system = emitGetAttrOr(attrsArg, "system",
                                 makeString(settings.thisSystem, ctx.runtime()), ctx);
    Term builder = emitGetAttr(attrsArg, "builder", ctx);
    Term args = emitGetAttrOr(attrsArg, "args", makeEmptyList(ctx.runtime()), ctx);
    Term outputs = emitGetAttrOr(attrsArg, "outputs",
                                  makeSingletonList(makeString("out", ctx.runtime()),
                                                    ctx.runtime()), ctx);

    // Create derivation record (env is the whole attrset minus special attrs)
    return makeDerivationRecord(name, system, builder, args, attrsArg, outputs,
                                ctx.runtime());
}
```

#### Step 3: Post-Evaluation Processing

```cpp
// In hvm4-result.cc
class DerivationProcessor {
public:
    struct PendingDrv {
        Term drvTerm;
        std::string name;
        // ... other fields
    };

    std::vector<PendingDrv> collectDerivations(Term result, HVM4Runtime& runtime) {
        std::vector<PendingDrv> drvs;
        collectRecursive(result, drvs, runtime);
        return drvs;
    }

    void processDerivations(std::vector<PendingDrv>& drvs,
                            Store& store,
                            HVM4Runtime& runtime) {
        for (auto& drv : drvs) {
            // Build Derivation object
            Derivation d;
            d.name = extractString(getDrvName(drv.drvTerm, runtime));
            // ... populate other fields

            // Write to store
            auto drvPath = store.writeDerivation(d);

            // Update term to include drvPath
            // (This is tricky - may need to return mapping instead)
        }
    }

private:
    void collectRecursive(Term term, std::vector<PendingDrv>& drvs,
                          HVM4Runtime& runtime) {
        if (isDerivationRecord(term)) {
            PendingDrv pending;
            pending.drvTerm = term;
            pending.name = extractString(getDrvName(term, runtime));
            drvs.push_back(pending);
        }

        // Recurse into children (attrs, lists, etc.)
        // Be careful not to force thunks unnecessarily
    }
};
```

#### Step 4: Add Tests

```cpp
// In hvm4.cc test file

TEST_F(HVM4BackendTest, DerivationBasic) {
    auto v = eval(R"(
        derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
        }
    )", true);

    // Should return attrset with type = "derivation"
    ASSERT_EQ(v.type(), nAttrs);
    auto type = v.attrs()->get(state.symbols.create("type"));
    state.forceValue(*type->value, noPos);
    ASSERT_EQ(type->value->string_view(), "derivation");
}

TEST_F(HVM4BackendTest, DerivationWithArgs) {
    auto v = eval(R"(
        derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
            args = ["-c" "echo hello"];
        }
    )", true);
    ASSERT_EQ(v.type(), nAttrs);
}

TEST_F(HVM4BackendTest, DerivationOutputs) {
    auto v = eval(R"(
        let drv = derivation {
            name = "multi-output";
            system = "x86_64-linux";
            builder = "/bin/sh";
            outputs = ["out" "dev" "doc"];
        };
        in drv.dev
    )", true);
    // Should access the dev output
}

TEST_F(HVM4BackendTest, DerivationContext) {
    // Derivation should add context to dependent strings
    auto v = eval(R"(
        let drv = derivation {
            name = "test";
            system = "x86_64-linux";
            builder = "/bin/sh";
        };
        in "${drv}"
    )", true);
    ASSERT_EQ(v.type(), nString);
    // Should have context containing drv reference
}

TEST_F(HVM4BackendTest, DerivationLazy) {
    // Derivation should be lazy
    auto v = eval(R"(
        let drv = derivation {
            name = "never-used";
            system = throw "not evaluated";
            builder = "/bin/sh";
        };
        in 42
    )", true);
    ASSERT_EQ(v.integer().value, 42);  // Should not throw
}
```

**Related Features:**
- Uses [Strings](./03-strings.md) with context for store path references
- Related to [Imports](./07-imports.md) for IFD (Import From Derivation)
- Uses [Attribute Sets](./01-attribute-sets.md) for derivation attributes

---
