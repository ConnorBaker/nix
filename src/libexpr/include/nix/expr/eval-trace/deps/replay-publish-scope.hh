#pragma once
///@file
/// RAII helper for replay-publication bookkeeping around thunk/app forcing.
///
/// Templated on the callable to avoid std::function heap allocation.
/// The lambda is stored inline (stack-allocated). CTAD deduces F at
/// the call site — no source changes needed at call sites.

#include <utility>

namespace nix {

template<typename F>
struct ReplayPublishScope
{
    F publishFn;
    bool committed = false;

    explicit ReplayPublishScope(F && f)
        : publishFn(std::move(f)) {}

    void commit() { committed = true; }

    ~ReplayPublishScope()
    {
        if (committed)
            publishFn();
    }

    ReplayPublishScope(const ReplayPublishScope &) = delete;
    ReplayPublishScope & operator=(const ReplayPublishScope &) = delete;
};

// CTAD guide: deduce F from constructor argument.
template<typename F>
ReplayPublishScope(F &&) -> ReplayPublishScope<std::decay_t<F>>;

} // namespace nix
