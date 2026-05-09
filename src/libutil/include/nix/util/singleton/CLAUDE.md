# Singleton Dispatch Library

C++ analog of Haskell singletons (Eisenberg & Weirich, "Dependently Typed
Programming with Singletons," Haskell Symposium 2012). Bridges runtime
values to compile-time template parameters via exhaustive switch.

## Header

| Header | Purpose |
|--------|---------|
| `dispatch.hh` | `Tag<V>` — compile-time tag carrying a non-type template parameter |

## API

### `Tag<V>`
A type carrying a compile-time value `V` (any NTTP: `int`, `enum class`,
`char`, `bool`, etc.). Provides:
- `static constexpr decltype(V) value = V` — the carried value
- `using value_type = decltype(V)` — the value's type
- `constexpr operator value_type()` — implicit conversion

### Dispatch pattern
The library provides only the `Tag` type. The dispatch switch is
user-written so that `-Wswitch-enum` catches missing variants:

```cpp
template<typename F>
decltype(auto) dispatch(MyEnum e, F && f) {
    switch (e) {
    case MyEnum::A: return f(singleton::Tag<MyEnum::A>{});
    case MyEnum::B: return f(singleton::Tag<MyEnum::B>{});
    }
    unreachable();
}
```

Combined with a `= delete` primary template, this ensures every enum
variant has an implementation:

```cpp
template<MyEnum E> void handle() = delete;
template<> void handle<MyEnum::A>() { ... }
// Missing handle<MyEnum::B> → compile error when dispatch calls it
```

## Comparison

| Haskell singletons | This library |
|-------------------|-------------|
| `data Sing (a :: k) where ...` | `Tag<V>` |
| `SingI a => ...` constraint | `decltype(tag)::value` in generic lambda |
| `withSomeSing val (\s -> ...)` | `dispatch(val, [](auto tag) { ... })` |
| Type family eliminators | `= delete` primary + specializations |

| Rust equivalent | Notes |
|----------------|-------|
| `match` on enum + generic trait dispatch | Rust's exhaustive match is the equivalent. No `Tag` needed because `match` arms can bind typed patterns directly |

## Maintenance

When modifying this library, update this file and:

**This CLAUDE.md must be updated when:**
- Changing `Tag<V>` API or semantics
- Adding new eval-trace dispatch instances

**Cross-reference with:**
- `src/libexpr/eval-trace/CLAUDE.md` Section 2 (Singleton Dispatch) — describes
  `HashProvenance` dispatch and the `= delete` primary template pattern
- `src/libexpr/include/nix/expr/eval-trace/store/dep-resolution-service.cc` —
  `ProvenancedHash<HP>` / `ComputedHash` / `VerifiedHash` using `singleton::Tag<HP>`
- `doc/eval-trace/implementation.md` Section 3.6 (Singleton Dispatch)

**Verification after changes:**
```bash
# All singleton dispatch switches in eval-trace
grep -rn 'singleton::Tag' src/libexpr/include/nix/expr/eval-trace/ --include='*.hh'
```
