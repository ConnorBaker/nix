## Implementing Memoization in the Nix Evaluator: Research Synthesis

### Executive Summary

The core challenge is that **standard memoization requires equality checking and hashing, but comparing lazy values forces evaluation, destroying laziness semantics**. [^1] The solution lies in **canonicalizing representations** so that pointer equality implies semantic equality, making memoization cheap and safe.

Two complementary approaches emerge from the research:

1. **Maximal Sharing** (hash-consing AST, environments, and thunks) — makes semantically-equal closures pointer-equal [^1]
2. **Deferred Structural Hashing** — uses identity-based keying with lazy hash computation for cross-invocation caching [^1]

---

### Foundational Work: Dolstra's Maximal Laziness (2008)

Eelco Dolstra's 2008 paper "Maximal Laziness" is directly relevant—it describes the technique originally used in Nix with the ATerm library: [^2]

> **Maximal laziness**: if two terms `e1` and `e2` arising during evaluation have the same abstract syntax representation, only one is evaluated, and the other reuses the result.

**Key implementation insight**: In a term rewriting system with **maximal sharing** (hash-consing), where equal terms occupy the same memory location, memoization becomes trivial: [^2]

```
cache : ATerm → ATerm  // Maps expressions to normal forms
```

The lookup `cache[e]` is O(1) because it's just a pointer lookup in a hash table.

**Performance results** from the paper show maximal laziness with closed-term optimization reduced evaluation time by 40-280% for typical Nix expressions. [^2]

**Why this matters**: The current Nix evaluator moved away from ATerms to a custom C++ representation, losing maximal sharing. Re-introducing hash-consing at the AST/Env/Thunk level would restore this capability.

---

### Approach 1: Maximal Sharing (Hash-Consing)

The most robust approach is to **make semantically-equal closures become pointer-equal**, then memoization becomes cheap and safe. [^1]

#### 1.1 Hash-Cons the AST (Expr Interning)

Build an intern table keyed by `(node_tag, children Expr*, literal payload, symbol ids)` and return canonical `Expr*` nodes: [^1]

```cpp
// Conceptual structure
struct ExprKey {
    ExprType tag;
    std::vector<Expr*> children;  // Already interned
    // ... literal payloads
};

std::unordered_map<ExprKey, Expr*, ExprKeyHash> exprInternTable;

Expr* internExpr(ExprKey key) {
    auto it = exprInternTable.find(key);
    if (it != exprInternTable.end()) return it->second;
    Expr* e = allocExpr(key);
    exprInternTable[key] = e;
    return e;
}
```

**Important**: Do not include source position (`Pos`) in the interning key—keep location metadata separately. [^1]

#### 1.2 Canonicalize Environments (Env Interning)

Represent environments as hash-consed frames: [^1]

```cpp
struct EnvKey {
    Env* parent;  // Already interned
    std::vector<std::pair<Symbol, Value*>> bindings;  // Canonical order by symbol
};

std::unordered_map<EnvKey, Env*, EnvKeyHash> envInternTable;
```

Now, if two closures capture equivalent environments (same parent, same bindings), they share the same `Env*`.

#### 1.3 Canonicalize Thunks (Thunk Interning)

When allocating a thunk `(expr, env)`, look up in the intern table: [^1]

```cpp
Value* mkThunk(Expr* e, Env* env) {
    auto key = std::make_pair(e, env);
    auto it = thunkInternTable.find(key);
    if (it != thunkInternTable.end()) return it->second;
    
    Value* thunk = allocValue();
    thunk->mkThunk(env, e);
    thunkInternTable[key] = thunk;
    return thunk;
}
```

**Result**: "Same syntax + same env" translates to pointer equality of the thunk. [^1]

#### 1.4 Memoization Becomes Trivial

With thunk interning, Nix's existing call-by-need updating automatically provides: [^1]

- "Evaluate this closure once globally" (within the sharing universe)
- Deep sharing of results (e.g., a single Nixpkgs evaluation feeding multiple consumers)

For function application, add an `AppMemoTable` keyed by identity: [^1]

```cpp
// Key: (lambda_closure_ptr, arg_thunk_ptr)
std::unordered_map<std::pair<Value*, Value*>, Value*> appMemoTable;
```

This short-circuits repeated applications without forcing arguments.

---

### Approach 2: Deferred Structural Hashing

For cases where full hash-consing is too invasive, **deferred structural hashing** provides an alternative: [^1]

#### 2.1 Two-Level Keying

- **Primary key**: `(Env*, Expr*)` pointer pair for O(1) identity lookup [^1]
- **Secondary key**: Structural hash computed lazily on force, enabling cross-invocation caching [^2]

#### 2.2 Lazy Hash Computation

Add a mutable hash cache to `Value`: [^1]

```cpp
struct Value {
    ValueType type;
    union { /* payload */ };
    mutable std::optional<size_t> cached_hash;  // Computed on first access
    
    size_t structural_hash() const {
        if (!cached_hash) {
            assert(type != tThunk);  // Only callable on forced values
            cached_hash = compute_structural_hash(*this);
        }
        return *cached_hash;
    }
};
```

**Critical**: Thunks within forced values are hashed by identity, not structure—preserving laziness for nested unevaluated components. [^1]

#### 2.3 Cycle Handling

Use **depth-limited traversal** (Racket's approach) for cycle safety: [^3]

```cpp
size_t hash(Value& v, int depth = 16) {
    if (depth <= 0) return 0;  // Depth limit reached
    // ... recurse with depth-1
}
```

Or use a **visited set with identity** (GHC's observable sharing): [^2]

```cpp
size_t hash(Value& v, std::unordered_set<Value*>& visited) {
    if (!visited.insert(&v).second) {
        return CYCLE_MARKER;  // Already visited
    }
    // ... recurse
}
```

---

### GHC's Approach: StableName + Weak Pointers

GHC's memo table implementation (from "Stretching the Storage Manager" by Peyton Jones, Marlow, and Elliott) uses: [^3]

1. **StableName for keys**: `makeStableName :: a -> IO (StableName a)` returns an integer ID stable across GC
2. **Weak pointers for values**: `mkWeak :: k -> v -> Maybe (IO ()) -> IO (Weak v)` creates a reference where `v` stays alive only while `k` is alive
3. **Finalization for cleanup**: When the key is collected, run a cleanup action removing the memo entry

**Key insight**: The cached *result* stays alive as long as the *input* is reachable. When the input becomes garbage, the result can be collected too. [^3]

**Note on StableName**: `makeStableName` may return a different StableName after an object is evaluated—this is relevant for Nix where thunks mutate in-place. [^1]

---

### Filliâtre's Type-Safe Hash-Consing

The OCaml hash-consing library by Conchon and Filliâtre provides a clean model: [^4]

```ocaml
type α hash_consed = private {
    node : α;      (* The value itself *)
    tag : int;     (* Unique integer *)
    hkey : int;    (* Cached hash key *)
}
```

**Key features**:
- **Unique tags** enable O(1) equality: `x = y ⟺ x == y ⟺ x.tag = y.tag` [^4]
- **Weak pointers** in the hash table allow GC to reclaim unreferenced values [^4]
- **Cached hash keys** avoid recomputation during table resizing [^4]

The `hashcons` operation is a single lookup-or-insert: [^4]

```ocaml
let hashcons t d =
    let hkey = H.hash d in
    let index = hkey mod (Array.length t.table) in
    (* lookup in bucket for value v such that H.equal v.node d *)
    (* if found then return v else *)
    let n = { hkey = hkey; tag = newtag(); node = d } in
    add t n;
    n
```

---

### Existing Nix Work: builtins.memoise Patch

The commit `0395b9b` adds a `builtins.memoise` primop: 

**Performance**: For a 10-machine NixOps network, evaluation time dropped from 17.1s to 9.6s, memory from 4486 MiB to 3089 MiB. 

**Implementation**: Uses a two-level map: [^1]

```cpp
// Key: (Env*, ExprLambda*) identifies the lambda
typedef std::pair<Env*, ExprLambda*> LambdaKey;

// Per-lambda cache: maps argument Value* to result Value
typedef std::map<Value*, Value, MemoArgComparator> PerLambdaMemo;

std::map<LambdaKey, PerLambdaMemo> memos;
```

**Critical limitation**: The `MemoArgComparator` **forces values** during comparison: [^1]

```cpp
bool MemoArgComparator::operator()(Value* v1, Value* v2) {
    state.forceValue(*v1);  // ← Destroys laziness!
    state.forceValue(*v2);
    // ... structural comparison
}
```

This is acceptable for an explicit primop but **poisonous as default evaluator behavior** if you want to preserve laziness. [^2]

---

### Boehm GC Considerations

Boehm GC provides mechanisms for weak references via **disappearing links**: [^1][^2]

```cpp
// Register weak reference: slot nulled when val collected
GC_general_register_disappearing_link(
    reinterpret_cast<void**>(&cache[key]), val);
```

**For hash-consing tables**: [^3]

```cpp
class WeakMemoCache {
    absl::flat_hash_map<MemoKey, Value*, MemoKeyHash> cache;
    
    void insert(const MemoKey& key, Value* val) {
        cache[key] = val;
        GC_general_register_disappearing_link(
            reinterpret_cast<void**>(&cache[key]), val);
    }
    
    Value* lookup(const MemoKey& key) {
        auto it = cache.find(key);
        if (it != cache.end() && it->second != nullptr) {
            return it->second;  // Still alive
        }
        return nullptr;  // Not found or collected
    }
};
```

**Finalization for cleanup**: [^3]

```cpp
void memo_finalizer(void* obj, void* cache_ptr) {
    auto* cache = static_cast<WeakMemoCache*>(cache_ptr);
    cache->remove_entry_for(obj);
}

void insert_with_cleanup(const MemoKey& key, Value* thunk, Value* result) {
    cache[key] = result;
    GC_register_finalizer(thunk, memo_finalizer, this, nullptr, nullptr);
}
```

**Important**: Boehm GC is non-moving by default, so raw pointers are stable. However, pointer reuse after collection is a concern—the current implementation observed "expected a set but found null" errors from cache returning results for different (but pointer-identical after GC) thunk arguments. [^1]

---

### Correctness Guarantees

#### Purity Barriers

Nix evaluation is not fully pure—operations observing the outside world (filesystem, env vars, network fetches) make "same expr+env" insufficient. [^3]

**Options**:
1. Enable maximal-sharing memoization only under `--pure-eval` conditions [^3]
2. Include an "effect token" or "world hash" in keys for impure thunks

#### Blackhole Preservation

Memoization must not interfere with infinite recursion detection—never cache a value that's currently being forced. [^2][^3]

#### String Context

Nix strings carry **context metadata** tracking store path dependencies. Two strings with identical characters but different contexts are semantically different. [^1]

```cpp
size_t hash_with_context(const char* str, const PathSet& context) {
    size_t h = std::hash<std::string_view>{}(str);
    for (const auto& path : context) {
        h ^= std::hash<std::string>{}(path) * 31;
    }
    return h;
}
```

---

### Recommended Implementation Strategy

Based on the research, here's a phased approach: [^2]

#### Phase 1: AST Hash-Consing (Expr Interning)
- Modify parser/AST builder to return interned `Expr*`
- Decouple source positions from interning key
- Low risk, immediate memory savings

#### Phase 2: Environment Interning
- Build binding lists in canonical order
- Return interned `Env*` from environment extension points
- Gate behind a flag initially

#### Phase 3: Thunk Interning
- Replace `new Thunk(expr, env)` with `mkThunk(expr, env)` lookup
- Gate behind `--pure-eval` mode initially
- This is where the "deduplicate Nixpkgs instances" win materializes

#### Phase 4: Application Memo Table
- Add `AppMemoTable` keyed by `(lambda_closure_ptr, arg_thunk_ptr)` (identity)
- Similar to `builtins.memoise` but non-strict

#### Phase 5: Memory Management
- Make all tables bounded first (LRU, size caps)
- Upgrade to weak-key + finalizers/disappearing links
- Use `traceable_allocator` for Boehm GC integration

---

### Key Papers and References

| Topic | Reference |
|-------|-----------|
| Maximal Laziness | Dolstra, "Maximal Laziness" (LDTA 2008) [^2] |
| Hash-Consing | Conchon & Filliâtre, "Type-Safe Modular Hash-Consing" (ML Workshop 2006) [^4] |
| Weak Pointers & Stable Names | Peyton Jones, Marlow, Elliott, "Stretching the Storage Manager" (IFL 1999) [^3] |
| Lazy Evaluation Semantics | Launchbury, "Natural Semantics for Lazy Evaluation" (1993) [^2] |
| STG Machine | Peyton Jones, "Implementing Lazy Functional Languages on Stock Hardware" (1992) [^2] |

---

### Summary

The path to effective memoization in Nix is **not** structural hashing of runtime values (which forces thunks), but rather **canonicalizing the representation** so that pointer equality implies semantic equality. This is exactly what Dolstra's original ATerm-based implementation achieved, and what the current C++ evaluator lost.

The recommended approach combines:
1. **Hash-consing** at the AST, Env, and Thunk levels for maximal sharing
2. **Identity-based memoization** using pointer pairs as keys
3. **Weak references** via Boehm GC's disappearing links for memory safety
4. **Purity barriers** to ensure correctness in the presence of impure operations

This approach achieves the goal of deduplicating Nixpkgs instances while maintaining maximum correctness and preserving laziness semantics.

[^1]: [claude-problem-structural-hashing](claude-problem-structural-hashing.md) (48%)
[^2]: [claude-problem-structural-hashing-research](claude-problem-structural-hashing-research.md) (24%)
[^3]: [chatgpt-problem-structural-hashing](chatgpt-problem-structural-hashing.md) (18%)
[^4]: [chatgpt-problem-structural-hashing-research](chatgpt-problem-structural-hashing-research.md) (10%)
