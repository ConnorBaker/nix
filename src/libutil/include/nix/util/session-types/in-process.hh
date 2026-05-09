#pragma once
///@file
///
/// In-process async backend for session-typed channels.
///
/// Uses a shared queue pair with asio::steady_timer as a condition variable
/// for notification between coroutines on the same strand. std::any is the
/// internal type-erasure mechanism, hidden behind typed transmit(T) /
/// receive<T>() methods.
///
/// Both endpoints run on a single strand — no concurrent queue access,
/// no mutex needed. The timer-condvar pattern (timer set to time_point::max(),
/// cancel_one() to wake) is the standard asio approach for coroutine
/// synchronization without threads.
///

#include <any>
#include <memory>
#include <queue>
#include <stdexcept>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "nix/util/session-types/chan.hh"
#include "nix/util/session-types/dual.hh"
#include "nix/util/session-types/session.hh"

namespace nix::session {

namespace asio = boost::asio;

/// Error type for the in-process backend.
struct InProcessError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace detail {

/// Shared queue state between InProcessTx and InProcessRx.
/// Uses steady_timer as a condition variable: timer set to time_point::max()
/// means async_wait blocks indefinitely. cancel_one() in transmit wakes
/// exactly one waiting receive.
struct AsyncQueueState {
    std::queue<std::any> data;
    asio::steady_timer notify;

    explicit AsyncQueueState(asio::any_io_executor ex)
        : notify(ex, asio::steady_timer::time_point::max()) {}
};

} // namespace detail

/// In-process transmitter: pushes typed values into a shared queue.
class InProcessTx {
    std::shared_ptr<detail::AsyncQueueState> queue_;

public:
    using transmitter_tag = void;

    explicit InProcessTx(std::shared_ptr<detail::AsyncQueueState> queue)
        : queue_(std::move(queue)) {}

    /// cancel_one() is a no-op if nobody is waiting — the next receive()
    /// will find data in the queue directly (checked before waiting).
    template<typename T>
    asio::awaitable<void> transmit(T val) {
        queue_->data.push(std::any(std::move(val)));
        queue_->notify.cancel_one();
        co_return;
    }
};

/// In-process receiver: pops typed values from a shared queue.
class InProcessRx {
    std::shared_ptr<detail::AsyncQueueState> queue_;

public:
    using receiver_tag = void;

    explicit InProcessRx(std::shared_ptr<detail::AsyncQueueState> queue)
        : queue_(std::move(queue)) {}

    /// Uses as_tuple to handle timer cancellation as a value (error_code)
    /// rather than an exception — the cancellation is the expected notification
    /// signal, not an error. Timer reset to max() after each wakeup prepares
    /// for next potential wait. While-loop is defensive (re-checks queue
    /// emptiness after wakeup).
    template<typename T>
    asio::awaitable<T> receive() {
        while (queue_->data.empty()) {
            (void) co_await queue_->notify.async_wait(
                asio::as_tuple(asio::use_awaitable));
            queue_->notify.expires_at(
                asio::steady_timer::time_point::max());
        }
        auto val = std::move(queue_->data.front());
        queue_->data.pop();
        auto * p = std::any_cast<T>(&val);
        if (!p)
            throw InProcessError("type mismatch in receive");
        co_return std::move(*p);
    }
};

/// Create a channel pair for protocol S.
///
/// Returns (client, server) where:
///   client: Chan<S, InProcessTx, InProcessRx>
///   server: Chan<Dual_t<S>, InProcessTx, InProcessRx>
///
/// The executor parameter is needed for timer construction.
/// Both endpoints share a strand — no concurrent queue access.
template<typename S>
    requires Session<S>
auto channel(asio::any_io_executor exec)
    -> std::pair<Chan<S, InProcessTx, InProcessRx>,
                 Chan<Dual_t<S>, InProcessTx, InProcessRx>>
{
    static_assert(std::is_same_v<Dual_t<Dual_t<S>>, S>,
        "Dual must be an involution");

    auto a_to_b = std::make_shared<detail::AsyncQueueState>(exec);
    auto b_to_a = std::make_shared<detail::AsyncQueueState>(exec);
    return {
        makeChan<S, InProcessTx, InProcessRx>(
            InProcessTx{a_to_b}, InProcessRx{b_to_a}),
        makeChan<Dual_t<S>, InProcessTx, InProcessRx>(
            InProcessTx{b_to_a}, InProcessRx{a_to_b}),
    };
}

} // namespace nix::session
