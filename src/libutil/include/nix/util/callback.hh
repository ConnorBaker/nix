#pragma once
///@file

#include <cassert>
#include <future>
#include <boost/outcome/result.hpp>
#include <functional>

#include "nix/util/fun.hh"

namespace nix {

/**
 * A callback delivers one ready value of type T or an exception.
 * Invocation and ownership transfer require sequencing or external synchronization; invoke at most once.
 */
template<typename T>
class Callback
{
public:
    using Result = boost::outcome_v2::result<T, std::exception_ptr>;

private:
    nix::fun<void(Result &&)> fun;
    bool done = false;

public:

    Callback(nix::fun<void(Result &&)> fun)
        : fun(std::move(fun))
    {
    }

    // NOTE: std::function is noexcept move-constructible since C++20.
    Callback(Callback && callback) noexcept(std::is_nothrow_move_constructible_v<decltype(fun)>)
        : fun(std::move(callback.fun))
        , done(std::exchange(callback.done, true))
    {
    }

    void operator()(T && t) noexcept
    {
        auto prev = std::exchange(done, true);
        assert(!prev);
        fun(Result(std::in_place_type<T>, std::move(t)));
    }

    void rethrow(const std::exception_ptr & exc = std::current_exception()) noexcept
    {
        auto prev = std::exchange(done, true);
        assert(!prev);
        fun(Result(std::in_place_type<std::exception_ptr>, exc));
    }
};

} // namespace nix
