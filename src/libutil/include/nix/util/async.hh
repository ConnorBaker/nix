#pragma once
///@file

#include "nix/util/callback.hh"
#include "nix/util/ref.hh"
#include "nix/util/signals.hh"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>

#include <atomic>
#include <concepts>
#include <tuple>
#include <utility>

namespace nix {

namespace asio = boost::asio;

template<typename T, std::invocable<Callback<T>> F, typename CompletionToken>
auto callbackToAwaitable(F && initiate, CompletionToken && token)
{
    return asio::async_initiate<CompletionToken, void(typename Callback<T>::Result)>(
        [initiate = std::forward<F>(initiate)](auto handler) mutable {
            auto executor = asio::get_associated_executor(handler);
            auto h = std::make_shared<std::pair<decltype(handler), std::atomic<bool>>>(
                std::piecewise_construct, std::forward_as_tuple(std::move(handler)), std::tuple{false});

            if (auto slot = asio::get_associated_cancellation_slot(h->first); slot.is_connected()) {
                std::weak_ptr wh = h; /* To handle the cyclic ownership. */
                slot.assign([executor, wh](asio::cancellation_type /*don't care*/) {
                    auto h = wh.lock();
                    if (!h || h->second.exchange(true))
                        return; /* Gracefully die. */
                    typename Callback<T>::Result result(
                        std::in_place_type<std::exception_ptr>,
                        std::make_exception_ptr(Interrupted("interrupted by user")));
                    asio::post(executor, [h, result = std::move(result)]() mutable {
                        std::move(h->first)(std::move(result));
                    });
                });
            }

            initiate(
                Callback<T>([executor,
                             h,
                             /* Ugly, but we need a work guard for the actual executor to ensure that the io_context
                                stays alive until the callback has finished on possibly another thread. */
                             guard = asio::make_work_guard(
                                 static_cast<asio::io_context &>(asio::query(executor, asio::execution::context)))](
                                typename Callback<T>::Result && fut) mutable {
                    auto releaseOwnership = [&]() {
                        /* Release our ownership. If the callback is owned by another thread, we don't want to be
                           responsible for tearing down the last reference to any of the captured objects. */
                        executor = {};
                        h.reset();
                        guard.reset();
                    };

                    if (h->second.exchange(true)) {
                        releaseOwnership();
                        /* Early return for cooperative cancellation. The callback has been called
                           later than we've been cancelled. In practice we'll get an error, the handler
                           has already been posted by the cancellation handler. */
                        return;
                    }

                    asio::post(executor, [h, fut = std::move(fut)]() mutable { std::move(h->first)(std::move(fut)); });
                    releaseOwnership();
                }));
        },
        std::forward<CompletionToken>(token));
}

/**
 * Convert a completion handler callback into a stackless coroutine. The
 * callback can be invoked on any thread and the completion handler will be
 * marshalled to the coroutines executer.
 */
template<typename T, std::invocable<Callback<T>> F>
asio::awaitable<T> callbackToAwaitable(F && initiate)
{
    auto fut = co_await callbackToAwaitable<T>(std::forward<F>(initiate), asio::use_awaitable);
    co_return std::move(fut).value();
}

template<typename Range, typename F>
asio::awaitable<void> forEachAsync(Range && range, const F & f)
{
    /* This code only runs on a strand - we don't do multithreaded executors, so
       no need for synchronisation. */
    auto pending = std::ranges::size(range);
    if (pending == 0)
        co_return;
    else if (pending == 1)
        co_return co_await f(*range.begin());

    auto executor = co_await asio::this_coro::executor;
    std::exception_ptr err;

    /* TODO: Handle cancellation on first error. Not very useful for now since we
       do all-or-nothing cancellation typically. */
    co_await asio::async_initiate<decltype(asio::use_awaitable), void(std::exception_ptr)>(
        [&](auto handler) {
            auto h = std::make_shared<decltype(handler)>(std::move(handler));
            for (auto && elt : std::forward<Range>(range)) {
                asio::co_spawn(executor, f(elt), [executor, h, &err, &pending](std::exception_ptr ex) {
                    if (ex && !err)
                        err = ex;
                    if (--pending == 0)
                        /* Post the coroutine resumption to the executor. */
                        asio::post(executor, [h = std::move(h), &err]() mutable { std::move (*h)(err); });
                });
            }
        },
        asio::use_awaitable);
}

} // namespace nix
