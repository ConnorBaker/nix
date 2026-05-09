#pragma once
///@file
///
/// CRTP mixin for linear-use enforcement.
///
/// Linear<Derived> adds a consumed_ flag and unconditional destructor
/// abort. Each transition method must call markConsumed(). Dropping
/// without consuming aborts in ALL build modes (not assert).
///
/// Unlike a value wrapper, Linear is a CRTP mixin inherited by the
/// state machine class itself.

#include "nix/util/move-only.hh"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace nix {

namespace detail {

/// Unconditional linearity violation — fires in all build modes.
[[noreturn]] inline void unconditionalAbort(const char * msg) noexcept
{
    std::fprintf(stderr, "linearity violation: %s\n", msg);
    std::abort();
}

} // namespace detail

/// CRTP base for linear-use enforcement.
///
/// Usage:
///   template<typename State>
///   class Pipeline : public Linear<Pipeline<State>> {
///       static constexpr const char * linearName = "Pipeline";
///       // ^^ optional; used in error message if present
///
///       [[nodiscard]] Pipeline<Next> advance() && {
///           this->markConsumed();
///           return Pipeline<Next>{...};
///       }
///   };
template<typename Derived>
class Linear : private MoveOnly {
    bool consumed_ = false;

protected:
    /// Derived transition methods must call this before accessing fields.
    /// Forgetting is caught by the destructor (runtime abort, all build modes).
    void markConsumed() noexcept { consumed_ = true; }

    /// Replace the current state with another, aborting if the current
    /// state has not been consumed yet. Derived move-assignment
    /// operators delegate here instead of using a defaulted operator=.
    void reassign(Linear && o) noexcept {
        if (!consumed_) [[unlikely]]
            detail::unconditionalAbort("reassigning over unconsumed value");
        consumed_ = o.consumed_;
        o.consumed_ = true;
    }

public:
    ~Linear()
    {
        if (!consumed_) [[unlikely]] {
            if constexpr (requires { Derived::linearName; })
                detail::unconditionalAbort(Derived::linearName);
            else
                detail::unconditionalAbort(
                    "linear value dropped without transition");
        }
    }

    Linear() = default;

    /// Move: source becomes consumed (moved-from is terminal).
    Linear(Linear && other) noexcept
        : MoveOnly(std::move(other))
        , consumed_(other.consumed_)
    {
        other.consumed_ = true;
    }

    Linear & operator=(Linear &&) = delete;
};

} // namespace nix
