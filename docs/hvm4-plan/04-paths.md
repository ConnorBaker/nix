# 4. Paths

> Source: `plan-future-work.claude.md` (extracted into `docs/hvm4-plan`).

Nix paths have:
- A `SourceAccessor` for virtual filesystem access
- Automatic copying to store when coerced to string
- Context tracking for store dependencies

## Nix Implementation Details

```cpp
struct Path {
    SourceAccessor* accessor;  // Virtual FS
    const StringData* path;    // Path string
};
```

## Option A: Pure Path (No Store Interaction)

Represent paths as strings, defer store operations.

```hvm4
// Path = #Pth{accessor_id, path_string}
// accessor_id identifies which SourceAccessor (0 = real FS)

@path_concat = λp1.λs2. #Pth{p1.accessor, @str_concat(p1.path, s2)}
@path_to_string = λpath. path.path  // Just extract string (no store copy)
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Store copy | Not handled - deferred |
| Simplicity | High |
| Compatibility | Partial - no derivation inputs |

## Option B: Effect-Based Store Interaction

Model store operations as effects for external handling.

```hvm4
// Effect = #Pure{value} | #CopyToStore{path, continuation}

@coerce_path_to_string = λpath. λcopy_to_store.
  copy_to_store .&. #CopyToStore{path, λstore_path. #Pure{store_path}}
              .|. #Pure{path.path}

// External interpreter handles CopyToStore effects
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Store interaction | Explicit, handled externally |
| Purity | Maintained in HVM4 |
| Complexity | Effect interpreter needed |

## Option C: Precomputed Store Paths

Pre-copy all referenced paths before HVM4 evaluation.

```hvm4
// During compilation, identify all path literals
// Copy them to store and substitute store paths
// No runtime store interaction needed

// Compilation phase:
// /foo/bar → "/nix/store/abc123-bar" with context
```

**Tradeoffs:**
| Aspect | Assessment |
|--------|------------|
| Store interaction | At compile time only |
| Runtime | Pure |
| Limitation | Can't handle dynamic paths |

## CHOSEN: Pure Path Representation (Option A)

**Rationale:**
- Defer store operations to post-HVM4 phase (at evaluation boundary)
- Paths as tagged strings with accessor ID
- Store copying happens during result extraction, not during HVM4 evaluation
- Keeps HVM4 evaluation pure and deterministic
- Effect-based approach (Option B) can be added later for IFD support

**Encoding:**
```hvm4
// Path = #Pth{accessor_id, path_string}
// accessor_id = 0 for real filesystem, other IDs for virtual accessors

// Example: ./foo/bar.nix
#Pth{0, #Str{chars, #NoC{}}}  // chars = "foo/bar.nix"

// With base path (after resolution)
#Pth{0, #Str{"/home/user/project/foo/bar.nix", #NoC{}}}
```

### Detailed Implementation Steps

**New Files:**
- `src/libexpr/hvm4/hvm4-path.cc`
- `src/libexpr/include/nix/expr/hvm4/hvm4-path.hh`

#### Step 1: Define Encoding Constants and API

```cpp
// hvm4-path.hh
namespace nix::hvm4 {

// Constructor names (base-64 encoded)
constexpr uint32_t PATH_NODE = /* encode "#Pth" */;

// Create path from resolved path string
Term makePath(uint32_t accessorId, Term pathString, HVM4Runtime& runtime);

// Create path from SourcePath
Term makePathFromSource(const SourcePath& path, HVM4Runtime& runtime);

// Get accessor ID from path
uint32_t pathAccessorId(Term pathTerm, const HVM4Runtime& runtime);

// Get path string from path
Term pathString(Term pathTerm, const HVM4Runtime& runtime);

// Path concatenation (path + string suffix)
Term concatPath(Term path, Term suffix, HVM4Runtime& runtime);

// Check if term is our path encoding
bool isNixPath(Term term);

}  // namespace nix::hvm4
```

#### Step 2: Implement Construction Functions

```cpp
// hvm4-path.cc
Term makePath(uint32_t accessorId, Term pathString, HVM4Runtime& runtime) {
    Term args[2] = { runtime.makeNum(accessorId), pathString };
    return runtime.makeCtr(PATH_NODE, 2, args);
}

Term makePathFromSource(const SourcePath& path, HVM4Runtime& runtime) {
    // Resolve path to absolute string
    std::string resolved = path.abs().string();

    // Get accessor ID (0 for root filesystem)
    uint32_t accessorId = getAccessorId(path.accessor);

    // Create string (no context for raw path)
    Term pathStr = makeString(resolved, runtime);

    return makePath(accessorId, pathStr, runtime);
}

uint32_t pathAccessorId(Term pathTerm, const HVM4Runtime& runtime) {
    uint32_t loc = term_val(pathTerm);
    Term idTerm = runtime.getHeapAt(loc);
    return term_val(idTerm);
}

Term pathString(Term pathTerm, const HVM4Runtime& runtime) {
    uint32_t loc = term_val(pathTerm);
    return runtime.getHeapAt(loc + 1);
}
```

#### Step 3: Add ExprPath Support

```cpp
// In hvm4-compiler.cc canCompileWithScope:
if (auto* e = dynamic_cast<const ExprPath*>(&expr)) {
    return true;  // Path literals always compile
}

// In emit():
if (auto* e = dynamic_cast<const ExprPath*>(&expr)) {
    return emitPath(*e, ctx);
}

Term HVM4Compiler::emitPath(const ExprPath& e, CompileContext& ctx) {
    // ExprPath contains a SourcePath
    return makePathFromSource(e.path, ctx.runtime());
}
```

#### Step 4: Path-to-String Coercion with Context

```cpp
// When path is coerced to string, it adds store path context
Term coercePathToString(Term path, CompileContext& ctx) {
    Term pathStr = pathString(path, ctx.runtime());

    // At this point, we're still in pure HVM4 evaluation
    // The actual store copy happens during result extraction

    // Create context element for this path
    // Context will be resolved when extracting result
    Term contextElem = makePathContext(path, ctx.runtime());
    Term context = makeContextSet(makeCons(contextElem,
                                   makeNil(ctx.runtime()), ctx.runtime()),
                                   ctx.runtime());

    // Return string with path context
    Term chars = stringChars(pathStr, ctx.runtime());
    Term args[2] = { chars, context };
    return ctx.runtime().makeCtr(STRING_WRAPPER, 2, args);
}
```

#### Step 5: Implement Result Extraction with Store Copy

```cpp
// In hvm4-result.cc:
void ResultExtractor::extractPath(Term term, Value& result) {
    uint32_t accessorId = pathAccessorId(term, runtime);
    Term pathStr = pathString(term, runtime);

    // Extract path string
    std::string pathContent = extractStringContent(pathStr);

    // Resolve accessor
    SourceAccessor* accessor = getAccessorById(accessorId);

    // Create SourcePath
    SourcePath sourcePath = SourcePath(accessor, pathContent);

    // Create Nix path value
    result.mkPath(sourcePath);
}

// When extracting string with path context, copy to store
void ResultExtractor::extractStringWithPathContext(Term term, Value& result) {
    Term chars = stringChars(term, runtime);
    Term ctx = stringContext(term, runtime);

    std::string content = extractCharList(chars);
    NixStringContext nixCtx;

    // Process context elements
    if (term_ext(ctx) == CONTEXT_SET) {
        Term elements = getContextElements(ctx, runtime);

        // For path contexts, copy to store and get store path
        processContextElements(elements, nixCtx, [&](Term elem) {
            if (isPathContext(elem)) {
                // Copy path to store
                SourcePath srcPath = extractSourcePath(elem);
                StorePath storePath = state.store->copyToStore(srcPath);

                // Add to context
                nixCtx.insert(NixStringContextElem::Opaque{storePath});

                return storePath.to_string();
            }
            return extractContextString(elem);
        });
    }

    result.mkStringWithContext(content, nixCtx);
}
```

#### Step 6: Add Comprehensive Tests

```cpp
// In hvm4.cc test file
TEST_F(HVM4BackendTest, PathLiteral) {
    // Need to set up a real path for testing
    auto v = eval("./.", true);
    ASSERT_EQ(v.type(), nPath);
}

TEST_F(HVM4BackendTest, PathInLet) {
    auto v = eval("let p = ./.; in p", true);
    ASSERT_EQ(v.type(), nPath);
}

TEST_F(HVM4BackendTest, PathCoerceToString) {
    // Path coercion adds context
    auto v = eval("\"${./.}\"", true);
    ASSERT_EQ(v.type(), nString);
    // Context should be non-empty (contains path reference)
}

TEST_F(HVM4BackendTest, PathConcat) {
    auto v = eval("./. + \"/foo\"", true);
    ASSERT_EQ(v.type(), nPath);
}

// === Additional Path Tests ===

TEST_F(HVM4BackendTest, PathConcatMultiple) {
    auto v = eval("./. + \"/foo\" + \"/bar\"", true);
    ASSERT_EQ(v.type(), nPath);
}

TEST_F(HVM4BackendTest, PathInAttrset) {
    auto v = eval("{ path = ./.; }", true);
    auto path = v.attrs()->get(state.symbols.create("path"));
    state.forceValue(*path->value, noPos);
    ASSERT_EQ(path->value->type(), nPath);
}

TEST_F(HVM4BackendTest, PathInList) {
    auto v = eval("[./. ./foo ./bar]", true);
    ASSERT_EQ(v.listSize(), 3);
    for (size_t i = 0; i < v.listSize(); i++) {
        state.forceValue(*v.listElems()[i], noPos);
        ASSERT_EQ(v.listElems()[i]->type(), nPath);
    }
}

TEST_F(HVM4BackendTest, PathEquality) {
    auto v = eval("./. == ./.", true);
    ASSERT_TRUE(v.boolean());
}

TEST_F(HVM4BackendTest, PathInequality) {
    auto v = eval("./foo == ./bar", true);
    ASSERT_FALSE(v.boolean());
}

TEST_F(HVM4BackendTest, PathAsFunction) {
    // Path can be passed to functions
    auto v = eval("(p: p) ./.", true);
    ASSERT_EQ(v.type(), nPath);
}

TEST_F(HVM4BackendTest, PathBaseNameDir) {
    // Using builtins with paths
    auto v = eval("builtins.baseNameOf ./foo/bar.nix", true);
    ASSERT_EQ(v.type(), nString);
    ASSERT_EQ(v.string_view(), "bar.nix");
}

TEST_F(HVM4BackendTest, PathDirOf) {
    auto v = eval("builtins.dirOf ./foo/bar.nix", true);
    ASSERT_EQ(v.type(), nPath);
}

TEST_F(HVM4BackendTest, PathToString) {
    // toString on path
    auto v = eval("builtins.toString ./.", true);
    ASSERT_EQ(v.type(), nString);
}

TEST_F(HVM4BackendTest, PathInterpolation) {
    // Path in string interpolation
    auto v = eval("\"prefix-${./.}-suffix\"", true);
    ASSERT_EQ(v.type(), nString);
    // Should have context from the path
}

TEST_F(HVM4BackendTest, PathLazy) {
    // Path should be lazy (not accessed until needed)
    auto v = eval("let p = ./nonexistent; in 42", true);
    ASSERT_EQ(v.integer().value, 42);
}

TEST_F(HVM4BackendTest, PathExists) {
    auto v = eval("builtins.pathExists ./.", true);
    ASSERT_TRUE(v.boolean());
}
```

---
