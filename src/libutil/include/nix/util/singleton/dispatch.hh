#pragma once
///@file
///
/// Compile-time tag for singleton dispatch (C++ analog of Haskell singletons).
///
/// Tag<V> carries a compile-time value as a type. Used with exhaustive
/// switch + deleted primary template to bridge runtime enum values to
/// compile-time template parameters.
///
/// Pattern:
///   1. Write dispatch switch (user-written for -Wswitch-enum):
///        template<typename F>
///        decltype(auto) dispatch(MyEnum e, F && f) {
///            switch (e) {
///            case MyEnum::A: return f(singleton::Tag<MyEnum::A>{});
///            case MyEnum::B: return f(singleton::Tag<MyEnum::B>{});
///            }
///            unreachable();
///        }
///   2. Write handler with = delete primary:
///        template<MyEnum E> Result handle(Args...) = delete;
///        template<> Result handle<MyEnum::A>(Args...) { ... }
///   3. Dispatch:
///        dispatch(val, [&](auto tag) {
///            return handle<decltype(tag)::value>(args...);
///        });
///
/// The switch MUST be user-written so -Wswitch-enum catches missing variants.

namespace nix::singleton {

/// Compile-time tag carrying a non-type template parameter.
/// Within a generic lambda receiving Tag<V>, decltype(tag)::value
/// is a compile-time constant usable as a template argument.
template<auto V>
struct Tag {
    /// The carried value, accessible at compile time.
    static constexpr decltype(V) value = V;

    /// The type of the carried value.
    using value_type = decltype(V);

    /// Implicit conversion for switch/comparison convenience.
    constexpr operator value_type() const noexcept { return V; }
};

} // namespace nix::singleton
