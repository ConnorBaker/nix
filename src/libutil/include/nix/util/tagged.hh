#pragma once
///@file
///
/// Generic phantom-tagged wrapper template.
///
/// Prevents cross-domain confusion at compile time with zero runtime
/// cost.  The inner value is accessible via `.value`; the tag exists
/// only in the type system.  Two Tagged values with different tags
/// are distinct, non-interconvertible types even if the inner type
/// is the same.
///
/// No default member initializer on `value`.  Tagged's implicit
/// default ctor is therefore:
///
///   * **Available** iff `T` is default-constructible.  `T value;`
///     default-initializes; for arithmetic `T` that leaves `value`
///     indeterminate.  **Callers MUST write `{}`** on scalar-valued
///     Tagged fields that need to read as zero — e.g.
///     `Tagged<_, uint32_t> nextId{};`.  A clang-tidy check
///     (`nix-uninitialized-tagged-scalar`) flags uninitialized scalar
///     Tagged fields in classes with user-provided ctors so this
///     isn't a footgun.
///
///   * **Deleted** iff `T` is not default-constructible (`CanonPath`,
///     `SourcePath`).  `std::__is_implicitly_default_constructible_v
///     <Tagged>` correctly reports false via SFINAE — `vector::
///     emplace_back` / pair machinery compiles cleanly without
///     transitively trying to instantiate `T()`.
///
/// Prior forms of this template introduced a third template parameter
/// (`bool DefaultConstructible = std::is_default_constructible_v<T>`)
/// to provide zero-init via partial specialization for scalar T while
/// keeping the non-default-constructible branch probe-safe.  That cost
/// ~30–50 KB of mangling bloat in `libnixexpr.so` (every downstream
/// template instantiated over Tagged re-instantiated once per bool
/// value).  Dropping the third parameter recovers that space; the
/// clang-tidy check plus explicit `{}` at use sites replaces the
/// compiler-enforced zero-init with a tool-enforced one.

#include <cstddef>
#include <functional>
#include <type_traits>

namespace nix {

/**
 * Phantom-typed wrapper.  Aggregate with one public member `value`.
 * Layout: identical to `T`; the tag has no runtime representation.
 */
template<typename Tag, typename T>
struct Tagged {
    T value;

    bool operator==(const Tagged &) const = default;
    auto operator<=>(const Tagged &) const = default;

    explicit operator bool() const
        requires requires(const T & t) { bool(t); }
    {
        return bool(value);
    }

    /// Hash functor — selects `std::hash<T>` if available, else
    /// `T::Hasher`, else `T::Hash`.
    ///
    /// Deliberately does NOT declare `is_avalanching = void`:
    /// libstdc++'s `std::hash<uint32_t>` (and every other integer
    /// specialisation) is the identity function.  Marking this
    /// avalanching would cause `boost::unordered_flat_map` to
    /// skip its internal `hash_mix` and use the raw ID as the
    /// bucket-index source, which clusters catastrophically for
    /// monotonic IDs (`[1..N]` all map to bucket 0 at bucket
    /// count 2^k).  Leaving the marker off is correct even for
    /// the composed-hasher case — callers that `hash_combine`
    /// multiple Tagged IDs don't inherit a false claim.
    struct Hash {
        size_t operator()(const Tagged & x) const noexcept
        {
            if constexpr (requires { std::hash<T>{}(x.value); })
                return std::hash<T>{}(x.value);
            else if constexpr (requires { typename T::Hasher; })
                return typename T::Hasher{}(x.value);
            else if constexpr (requires { typename T::Hash; })
                return typename T::Hash{}(x.value);
            else
                static_assert(sizeof(T) == 0,
                    "Tagged::Hash requires std::hash<T>, T::Hasher, or T::Hash");
        }
    };
};

} // namespace nix
