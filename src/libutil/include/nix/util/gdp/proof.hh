#pragma once
///@file
///
/// GDP (Ghosts of Departed Proofs) scoped proof library.
///
/// Proof<Tag> is a zero-size, non-constructible, non-copyable, non-movable
/// token existing only as const& inside a continuation. Proves a precondition
/// (identified by Tag) holds.
///
/// Creation path: Certifier<Tag> (parameterized private inheritance).
/// Subclass privately, call the protected withProof(f) or withProofIf(cond, f)
/// from domain-specific methods. No bare proof objects — proofs exist only
/// inside continuations.
///
/// C++ approximation of Haskell's GDP pattern (rank-2 scoped phantom proof).
/// See: Noonan, "Ghosts of Departed Proofs," Haskell Symposium 2018.

#include <optional>
#include <type_traits>
#include <utility>

namespace nix::gdp {

template<typename Tag> class Certifier;


/// Unforgeable proof that a precondition identified by Tag holds.
/// Zero runtime cost. Cannot be constructed, copied, or moved except
/// via Certifier<Tag> (protected, requires subclassing).
template<typename Tag>
class Proof {
    struct Key {};
    explicit Proof(Key) {}

    Proof(const Proof &) = delete;
    Proof(Proof &&) = delete;
    Proof & operator=(const Proof &) = delete;
    Proof & operator=(Proof &&) = delete;

    template<typename> friend class Certifier;

public:
    ~Proof() = default;
};


/// Base for custom certifiers. Subclass privately and call the protected
/// withProof() or withProofIf() from domain-specific methods.
/// Private inheritance restricts proof creation to the inheriting class.
///
/// Usage:
///   struct MyTag {};
///   class MyVerifier : private Certifier<MyTag> {
///   public:
///       template<typename F>
///       bool withVerified(bool ok, F && f) {
///           return Certifier::withProofIf(ok, std::forward<F>(f));
///       }
///   };
template<typename Tag>
class Certifier {
protected:
    /// Unconditionally create a scoped Proof<Tag> and pass it to f.
    /// The proof exists only inside f and cannot escape.
    template<typename F>
    static decltype(auto) withProof(F && f)
    {
        return std::forward<F>(f)(
            static_cast<const Proof<Tag> &>(Proof<Tag>{typename Proof<Tag>::Key{}}));
    }

    /// Conditionally create a scoped Proof<Tag>. Calls f only if cond is true.
    /// Non-void f: returns std::optional<R> (nullopt when !cond).
    /// Void f: returns bool (false when !cond).
    template<typename F>
    static auto withProofIf(bool cond, F && f)
    {
        using R = std::invoke_result_t<F, const Proof<Tag> &>;
        if constexpr (std::is_void_v<R>) {
            if (cond)
                withProof(std::forward<F>(f));
            return cond;
        } else {
            return cond
                ? std::optional<R>(withProof(std::forward<F>(f)))
                : std::optional<R>();
        }
    }
};


} // namespace nix::gdp
