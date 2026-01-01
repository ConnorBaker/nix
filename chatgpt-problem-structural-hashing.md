# Report: Nix lazy evaluation, memoization goals, and structural hashing challenges

This report summarizes how Nix implements lazy evaluation in the evaluator, why lambda-call memoization is being pursued (to enable sharing across repeated Nixpkgs instantiations), and the technical challenges of structural hashing without destroying laziness. It is written to be useful to an external researcher without direct access to the Nix codebase.

## 1) How lazy evaluation works in Nix (implementation overview)

Nix implements **call‑by‑need** using explicit thunks. The central runtime value type is `Value`, with an internal tagged representation (examples below) that can hold either an immediate value (int, string, etc.) or a thunk (deferred computation). In broad terms:

- **Thunks** are `Value`s that carry either `(Env*, Expr*)` (`tThunk`) or a function application `(Value* left, Value* right)` (`tApp`).
- **Lambdas** are `Value`s that carry `(Env*, ExprLambda*)`.
- **Attrsets** and **lists** are stored as pointer structures (no eager evaluation of contained values).

### 1.1) Value types (high-level)

The internal representation includes (simplified):

- `tThunk`, `tApp` → user‑facing `nThunk`
- `tLambda`, `tPrimOp`, `tPrimOpApp` → user‑facing `nFunction`
- `tInt`, `tBool`, `tFloat`, `tString`, `tPath`, `tNull`, `tAttrs`, `tListSmall`, `tListN`, `tExternal`

The public method `Value::type()` collapses internal types into a smaller `ValueType` enum (e.g. `nThunk`, `nFunction`, `nAttrs`, `nList`, etc.). The evaluator operates on the `ValueType` and on internal `isThunk()`, `isApp()`, `isLambda()`, etc.

### 1.2) Where thunks are created

- **Default path**: `Expr::maybeThunk()` creates a new `Value` and stores a thunk `(Env*, Expr*)` in it.
- **Literals and some nodes** override `maybeThunk` to return a **shared Value** stored in the AST node (e.g., string literals). This means multiple occurrences of the same literal can share a `Value*`.
- **Variables**: `ExprVar::maybeThunk()` returns the bound `Value*` directly (no new thunk), so references share the same value.

### 1.3) Forcing (turning a thunk into a value)

`EvalState::forceValue(Value & v, PosIdx pos)` is the critical forcing mechanism:

1. If `v` is a thunk, it reads `(env, expr)` from the thunk.
2. It replaces the value with a **blackhole sentinel** to detect recursion.
3. It evaluates `expr->eval(*this, *env, v)` and writes the result **in place** into the same `Value` object.
4. If evaluation fails, it restores the thunk and rethrows.

Because forcing rewrites the same `Value` in place, multiple references to the same thunk share the computed result (classic call‑by‑need).

### 1.4) Function calls and laziness

`EvalState::callFunction(Value & fun, std::span<Value *> args, ...)` implements function application:

- The function value is forced, but arguments are not forced eagerly.
- If the function is a lambda, a new `Env` is allocated and binds arguments (without forcing them). If the lambda expects attrset formals, the **attrset itself** is forced so attributes can be inspected; attribute values remain lazy.
- The lambda body is evaluated in the new environment, producing the result `vCur`.

This preserves laziness: arguments are only forced when required (e.g., attribute inspection, numeric operations, etc.).

---

## 2) Goal of memoization: sharing work across instantiations

A common pattern is evaluating Nixpkgs multiple times in the same evaluation (e.g., multiple `nixosSystem` calls with the same `system` or `config`). Today each instantiation creates a distinct evaluation graph, so thunks are **not** shared and expensive computations repeat.

The proposed optimization is **memoization of lambda calls**, so repeated calls of the same lambda with the same argument(s) can reuse a cached result. The intended key is roughly:

- `(ExprLambda*, Env*, argsHash)`

Where:
- `ExprLambda*` identifies the lambda body code.
- `Env*` identifies the closure (captured environment).
- `argsHash` represents the argument value structure.

When this hits, Nix can reuse the cached result and avoid recomputation, enabling effective sharing similar to deduplicating repeated Nixpkgs instances.

Because this memoization is applied **inside the evaluator**, it can share thunks across subgraphs even when the user did not explicitly share values at the language level.

---

## 3) Challenges of structural hashing without destroying laziness

Memoization hinges on hashing and comparing arguments **without forcing them**. This is technically subtle.

### 3.1) You cannot force thunks just to hash

Forcing a thunk to compute a hash would:

- Destroy laziness (turn evaluation eager).
- Change observable behavior (especially in impure expressions).
- Cause large unintended evaluation cost.

Therefore, the hash function must be **lazy‑safe**: if a value (or any nested value) is a thunk or application, hashing should **refuse** to proceed. A common pattern is:

- Return a sentinel hash (e.g., `0`) to mean “do not memoize this call.”

### 3.2) Values are graphs (not trees)

Nix values can contain cycles (recursive attrsets/lists). Structural hashing/equality must include **cycle detection** (e.g., a visited set of `Value*` pairs) to avoid infinite recursion.

### 3.3) Attrsets are layered and merged

Attrsets are implemented by `Bindings`, which can be layered with `//`. Iteration yields the **effective merged view** with overriding semantics. Structural hashing must iterate the merged view (the `Bindings::iterator`), not raw layers, or it will hash the wrong logical value.

### 3.4) String context must be included

Strings in Nix carry a **dependency context** (store paths and derivations). This context affects evaluation results (especially when strings are used in derivations). Two strings with the same content but different contexts are **not equivalent**. A hash function that ignores context risks returning cached results with incorrect dependency closures.

The string context is stored as `Value::StringWithContext::Context`, a length‑prefixed array of `StringData*` values representing canonicalized context elements. Hashing must incorporate this array in order (it is sorted for canonicity).

### 3.5) Hash collisions must be defended

If a memoization key uses only `argsHash` and does **not** check equality, then a collision yields a wrong result. Structural equality (no forcing) should be checked on cache hits to defend against collisions, or collisions should raise an explicit error.

### 3.6) Pointer identity is insufficient

Pointer identity is tempting, but unsafe as a hash key:

- GC can reuse addresses, causing unrelated values to match.
- Using pointer identity as a *hash* (without storing the pointer) makes collisions unavoidable.

Structural hashing and equality are necessary for correctness.

---

## 4) Memoization and currying

Nix functions are curried; a lambda can return another lambda that closes over its argument. Memoizing these intermediate results has consequences:

- **Increased sharing**: repeated partial applications (`f a`) reuse the same closure, which can be beneficial.
- **Memory retention**: cached closures keep captured environments alive longer, potentially retaining large graphs.
- **Evaluation timing changes**: if the first lambda body performs work or traces before returning a lambda, memoization can suppress those effects on subsequent calls. This is acceptable only if semantic change is intended.

Even if memoization is accepted in impure contexts, the above effects should be understood, as they can impact tracing, profiling, and memory usage.

---

## 5) Recommended strategy for lazy‑safe hashing + equality (sketch)

### 5.1) Lazy‑safe hash

- If a value is a thunk/app, return 0.
- Otherwise hash by structure:
  - primitives: value hash
  - strings: content hash + context hash
  - paths: path string + accessor pointer (identity)
  - attrsets: size + (name, value) recursively
  - lists: size + elements recursively
  - lambdas/primops/external: pointer identity (as a component in a structural hash)
- If any nested value is a thunk, propagate 0.

### 5.2) Lazy‑safe structural equality

- If either value is a thunk/app, return false (do not memoize).
- Otherwise compare structure by type:
  - strings: compare content + context
  - attrsets: compare names and corresponding values
  - lists: compare size and elements
  - lambdas/primops/external: compare pointer identity
- Use a visited set to avoid cycles.

### 5.3) Cache entries should retain arguments/results

To run structural equality on a hit, the cache needs access to the original argument value. Store a GC‑rooted `RootValue` for the argument and result in the cache entry:

```
struct LambdaCallMemoEntry {
  RootValue arg;
  RootValue result;
  size_t argsHash;
};
```

On lookup, compare `equalValueLazy(*entry.arg, *args[0])`. If false, either treat as miss or throw a collision error.

---

## 6) Key takeaways

- Nix laziness relies on **thunks stored as `(Env*, Expr*)`** and **in‑place forcing**.
- Memoization can enable significant sharing across repeated instantiations, but must be careful not to force evaluation.
- Structural hashing must respect **string context**, **layered attrsets**, and **cycles**, and avoid thunks.
- Hash collisions must be defended with lazy‑safe equality checks.
- Currying introduces memory retention and evaluation‑timing effects that should be acknowledged.

This should be sufficient for an external researcher to reason about advanced approaches without direct access to the codebase.
