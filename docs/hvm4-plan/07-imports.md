# 7. Imports

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).

`import path` loads and evaluates a Nix file, with memoization.

## Nix Implementation Details

```cpp
// import is memoized in fileEvalCache
// scopedImport is NOT memoized
// Import From Derivation (IFD) requires building derivations during eval
```

## Option A: Pre-Import Resolution

Resolve all imports before HVM4 compilation.

```hvm4
// 1. Parse main file, collect all import paths
// 2. Recursively parse imported files
// 3. Inline imported expressions into single AST
// 4. Compile combined AST to HVM4

// No import at HVM4 runtime - all resolved at compile time
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| IFD | Not supported |
| Dynamic imports | Not supported |
| Memoization | Implicit via inlining |
| Compilation | May be slow for large closures |

## Option B: Module System

Compile each file to named HVM4 definitions, resolve at load time.

```hvm4
// file: foo.nix → @foo_nix_main = ...
// import ./foo.nix → @foo_nix_main

// Build dependency graph, load all modules, then evaluate
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Memoization | Natural - each file is one @def |
| Dynamic imports | Not supported |
| Modularity | Good |

## Option C: Effect-Based Import

Model import as an effect.

```hvm4
// Import = #Import{path, continuation}

@eval_import = λpath. #Import{path, λcontent. @eval(@parse(content))}

// External interpreter:
// 1. Receives Import effect
// 2. Reads file (with memoization)
// 3. Parses and compiles to HVM4
// 4. Continues with compiled term
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| IFD | Possible with external handler |
| Dynamic imports | Supported |
| Complexity | Effect system needed |
| Memoization | In effect handler |

## Option D: Two-Phase Evaluation

Phase 1: Evaluate to collect import requests. Phase 2: Resolve and re-evaluate.

```hvm4
// Phase 1: Return set of required imports
@collect_imports = λexpr. ...  // Returns list of paths

// Phase 2: With imports resolved, full evaluation
@eval_with_imports = λimport_map.λexpr. ...
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| IFD | Limited - no nested IFD |
| Iterations | Multiple passes |

## CHOSEN: Pre-Import Resolution (Phase 1: Option A)

**Rationale:**
- Simplest approach that works for most use cases
- No runtime import handling needed in HVM4
- Natural memoization through AST deduplication
- Effect-based approach (Option C) can be added later for IFD

**Strategy:**
1. Parse main expression and collect all static import paths
2. Recursively parse and compile imported files
3. Build combined AST with imports resolved
4. Compile complete AST to HVM4

**Limitations:**
- Dynamic import paths not supported (e.g., `import (./. + filename)`)
- Import From Derivation (IFD) not supported in Phase 1
- Expressions must fall back to standard evaluator if they contain dynamic imports

### Detailed Implementation Steps

**Files Modified:**
- `src/libexpr/hvm4/hvm4-compiler.cc` (import resolution)
- `src/libexpr/hvm4/hvm4-import-resolver.cc` (new file)

#### Step 1: Collect Import Paths

```cpp
// hvm4-import-resolver.cc
class ImportResolver {
public:
    struct ImportInfo {
        SourcePath path;
        Expr* resolvedExpr;
    };

    std::vector<SourcePath> collectImports(Expr* expr) {
        std::vector<SourcePath> imports;
        collectImportsRecursive(expr, imports);
        return imports;
    }

private:
    void collectImportsRecursive(Expr* expr, std::vector<SourcePath>& imports) {
        if (auto* call = dynamic_cast<ExprCall*>(expr)) {
            if (isImportBuiltin(call->fun)) {
                if (auto* pathExpr = dynamic_cast<ExprPath*>(call->args[0])) {
                    imports.push_back(pathExpr->path);
                } else {
                    // Dynamic import - not supported
                    throw CompileError("dynamic imports not supported in HVM4 backend");
                }
            }
        }
        // Recurse into children
        for (auto* child : expr->children()) {
            collectImportsRecursive(child, imports);
        }
    }
};
```

#### Step 2: Resolve and Compile Imports

```cpp
std::map<SourcePath, Term> resolveAllImports(
    Expr* mainExpr,
    EvalState& state,
    HVM4Compiler& compiler) {

    std::map<SourcePath, Term> resolved;
    std::set<SourcePath> pending;
    std::set<SourcePath> processing;  // For cycle detection

    ImportResolver resolver;
    auto imports = resolver.collectImports(mainExpr);

    for (const auto& path : imports) {
        pending.insert(path);
    }

    while (!pending.empty()) {
        SourcePath path = *pending.begin();
        pending.erase(pending.begin());

        if (resolved.count(path)) continue;
        if (processing.count(path)) {
            throw CompileError("circular import detected: " + path.to_string());
        }

        processing.insert(path);

        // Parse the file
        Expr* importedExpr = state.parseExprFromFile(path);

        // Collect nested imports
        auto nestedImports = resolver.collectImports(importedExpr);
        for (const auto& nested : nestedImports) {
            if (!resolved.count(nested)) {
                pending.insert(nested);
            }
        }

        // Compile to HVM4
        Term compiled = compiler.compile(*importedExpr);
        resolved[path] = compiled;

        processing.erase(path);
    }

    return resolved;
}
```

#### Step 3: Replace Import Calls with Resolved Terms

```cpp
Term HVM4Compiler::emitImport(const ExprCall& call, CompileContext& ctx) {
    // Import should have been resolved during pre-processing
    auto* pathExpr = dynamic_cast<ExprPath*>(call.args[0]);
    if (!pathExpr) {
        throw CompileError("import requires static path");
    }

    auto it = ctx.resolvedImports.find(pathExpr->path);
    if (it == ctx.resolvedImports.end()) {
        throw CompileError("import not resolved: " + pathExpr->path.to_string());
    }

    return it->second;
}
```

#### Step 4: Add Tests

```cpp
// In hvm4.cc test file

TEST_F(HVM4BackendTest, ImportSimple) {
    // Would need test file infrastructure
    // For now, test that import detection works
}

TEST_F(HVM4BackendTest, ImportCircularDetection) {
    // Circular imports should be detected
}

TEST_F(HVM4BackendTest, ImportNested) {
    // A imports B imports C should all be resolved
}

TEST_F(HVM4BackendTest, ImportDynamicRejected) {
    // Dynamic imports should fail compilation
    EXPECT_THROW(
        canCompile("import (./. + \"/foo.nix\")", true),
        CompileError
    );
}

TEST_F(HVM4BackendTest, ImportMemoization) {
    // Same file imported twice should use same term
}
```

**Related Features:**
- Uses [Paths](./04-paths.md) for path resolution
- Related to [Derivations](./08-derivations.md) for IFD (future work)

---
