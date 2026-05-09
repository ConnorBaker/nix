#pragma once
///@file
///
/// Runtime session-typed channel: Chan<S, Tx, Rx>.
///
/// Each method (send, recv, choose, offer, call, close) consumes the
/// channel by move and returns a new channel with the advanced protocol
/// type. Protocol violations don't compile.
///
/// Channel operations are coroutines returning asio::awaitable<T>.
/// Errors propagate as exceptions through co_await.
///
/// Enforcement strategy (layered, compile-time where possible):
///
///   Compile-time:
///     - Protocol type parameterization: wrong-order operations fail to compile
///       (requires clauses gate each method on Action_t<S> matching the right
///       protocol constructor).
///     - &&-qualified methods: calling without std::move fails to compile.
///     - Deleted copy: aliasing fails to compile.
///     - [[nodiscard]] on class: discarding a temporary Chan is a warning.
///     - Private constructor: only makeChan (public, requires Session<S>),
///       friend Chan instantiations, and Branches can construct a Chan.
///
///   Runtime (unconditional, never compiled out):
///     - Destructor (via Linear<>) aborts if the channel was not consumed.
///       This catches the one case C++ cannot prevent at compile time: a
///       named variable going out of scope without being consumed. Fires
///       in all build modes including release (NDEBUG).
///     - Move-assignment aborts (via reassign) if the target was not already
///       consumed.
///
///   Lazy coroutine semantics:
///     - asio::awaitable<T> is lazy — markConsumed() executes when
///       co_awaited, not at function call time.
///     - If awaitable is discarded: coroutine frame destroyed (body never ran),
///       but tx_/rx_ never moved either. Original Chan destructor fires
///       unconsumed → abort. Same safety as sync.
///     - During co_await: caller's coroutine frame lives on heap (suspended),
///       so `this` remains valid through any suspension point.
///     - Cancellation during co_await: markConsumed() already called before
///       any co_await. Exception from cancelled I/O propagates — channel
///       consumed by failure (terminal). No spurious linearity abort.
///

#include <concepts>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <boost/asio/awaitable.hpp>

#include "nix/util/linear.hh"
#include "nix/util/session-types/actionable.hh"
#include "nix/util/session-types/backend.hh"
#include "nix/util/session-types/branches.hh"
#include "nix/util/session-types/protocol.hh"
#include "nix/util/session-types/select.hh"
#include "nix/util/session-types/session.hh"

namespace nix::session {

namespace asio = boost::asio;

/// Public factory: construct a channel with explicit backend halves.
/// Use this when providing your own Transmitter/Receiver (custom backends,
/// test doubles). For paired InProcess channels, use channel<S>(exec).
template<typename S, Transmitter Tx, Receiver Rx>
    requires Session<S>
Chan<S, Tx, Rx> makeChan(Tx tx, Rx rx);

template<typename S, Transmitter Tx, Receiver Rx>
class [[nodiscard("Chan must be consumed — call send/recv/choose/offer/call/close")]]
Chan : public nix::Linear<Chan<S, Tx, Rx>> {
public:
    static constexpr const char * linearName = "Chan";
    using session_type = S;

private:
    Tx tx_;
    Rx rx_;

    // Self-friendship across different protocol instantiations (for call()).
    template<typename, Transmitter, Receiver> friend class Chan;
    // Branches constructs Chan in visit_dispatch.
    template<typename, Transmitter, Receiver> friend class Branches;
    // Public factory for custom backends.
    template<typename S2, Transmitter Tx2, Receiver Rx2>
        requires Session<S2>
    friend Chan<S2, Tx2, Rx2> makeChan(Tx2, Rx2);

    Chan(Tx tx, Rx rx)
        : tx_(std::move(tx)), rx_(std::move(rx)) {}

public:
    Chan(Chan && o) noexcept
        : nix::Linear<Chan>(std::move(o))
        , tx_(std::move(o.tx_)), rx_(std::move(o.rx_))
    {}

    Chan(const Chan &) = delete;
    Chan & operator=(const Chan &) = delete;

    /// Move assignment: only valid when the target was already consumed.
    /// This enables C++ loop patterns (Rust's variable rebinding equivalent).
    /// Assigning over an unconsumed Chan aborts in all build modes.
    Chan & operator=(Chan && o) noexcept {
        this->reassign(std::move(o));
        tx_ = std::move(o.tx_);
        rx_ = std::move(o.rx_);
        return *this;
    }

    // -- send: Action_t<S> is Send<T, P> ----------------------------------------

    /// markConsumed() is called before co_await deliberately — ensures
    /// cancellation or backend failure doesn't trigger linearity abort,
    /// matching the semantic "a failed operation is terminal for the channel."
    auto send(Payload_t<Action_t<S>> val) &&
        -> asio::awaitable<Chan<Continuation_t<Action_t<S>>, Tx, Rx>>
        requires IsSend<Action_t<S>>::value
              && CanTransmit<Tx, Payload_t<Action_t<S>>>
    {
        this->markConsumed();
        co_await tx_.transmit(std::move(val));
        co_return Chan<Continuation_t<Action_t<S>>, Tx, Rx>(
            std::move(tx_), std::move(rx_));
    }

    // -- recv: Action_t<S> is Recv<T, P> ----------------------------------------

    auto recv() &&
        -> asio::awaitable<
               std::pair<Payload_t<Action_t<S>>,
                         Chan<Continuation_t<Action_t<S>>, Tx, Rx>>>
        requires IsRecv<Action_t<S>>::value
              && CanReceive<Rx, Payload_t<Action_t<S>>>
    {
        this->markConsumed();
        auto val = co_await rx_.template receive<Payload_t<Action_t<S>>>();
        co_return std::pair(
            std::move(val),
            Chan<Continuation_t<Action_t<S>>, Tx, Rx>(
                std::move(tx_), std::move(rx_)));
    }

    // -- choose<N>: Action_t<S> is Choose<Ps...> --------------------------------

    template<unsigned N>
    auto choose() &&
        -> asio::awaitable<Chan<SelectBranch_t<Action_t<S>, N>, Tx, Rx>>
        requires IsChoose<Action_t<S>>::value
              && (N < BranchCount<Action_t<S>>::value)
              && CanTransmit<Tx, Choice>
    {
        this->markConsumed();
        co_await tx_.transmit(Choice{N});
        co_return Chan<SelectBranch_t<Action_t<S>, N>, Tx, Rx>(
            std::move(tx_), std::move(rx_));
    }

    // -- offer: Action_t<S> is Offer<Ps...> -------------------------------------

    /// Invalid branch index throws std::runtime_error — this is a protocol-level
    /// violation from the peer, not a backend I/O error.
    auto offer() &&
        -> asio::awaitable<Branches<Action_t<S>, Tx, Rx>>
        requires IsOffer<Action_t<S>>::value
              && CanReceive<Rx, Choice>
    {
        this->markConsumed();
        auto choice = co_await rx_.template receive<Choice>();
        if (choice.index >= BranchCount<Action_t<S>>::value)
            throw std::runtime_error("invalid branch index in offer");
        co_return Branches<Action_t<S>, Tx, Rx>(
            choice.index, std::move(tx_), std::move(rx_));
    }

    // -- call: Action_t<S> is Call<P, Q> ----------------------------------------
    //
    // f receives Chan<P, Tx, Rx> and must return awaitable<Chan<Done, Tx, Rx>>.
    // call() extracts tx/rx from the Done channel and constructs Chan<Q, Tx, Rx>.
    //
    // With an async backend, bidirectional sub-protocols (f both sends and
    // receives) now work because co_await suspends the coroutine, allowing
    // the scheduler to interleave the peer.

    template<typename F>
    auto call(F && f) &&
        -> asio::awaitable<Chan<CallCont_t<Action_t<S>>, Tx, Rx>>
        requires IsCall<Action_t<S>>::value
              && requires(F fn, Chan<CallTarget_t<Action_t<S>>, Tx, Rx> c) {
                     { std::forward<F>(fn)(std::move(c)) }
                         -> std::same_as<asio::awaitable<Chan<Done, Tx, Rx>>>;
                 }
    {
        using SubProto = CallTarget_t<Action_t<S>>;
        using ContProto = CallCont_t<Action_t<S>>;

        this->markConsumed();
        Chan<SubProto, Tx, Rx> sub_chan(std::move(tx_), std::move(rx_));

        Chan<Done, Tx, Rx> done_chan =
            co_await std::forward<F>(f)(std::move(sub_chan));

        done_chan.markConsumed();
        co_return Chan<ContProto, Tx, Rx>(
            std::move(done_chan.tx_), std::move(done_chan.rx_));
    }

    // -- split: Action_t<S> is Split<P, Q, R> -----------------------------------
    //
    // f receives two half-channels:
    //   Chan<P, Tx, Unavailable>  — tx-only (can send/choose, not recv/offer)
    //   Chan<Q, Unavailable, Rx>  — rx-only (can recv/offer, not send/choose)
    // and must return a pair of Done channels. split() reassembles Tx from the
    // tx-done channel and Rx from the rx-done channel into Chan<R, Tx, Rx>.
    //
    // The closure is where concurrency happens — typically the user co_spawns
    // two coroutines for each half and joins them.

    template<typename F>
    auto split(F && f) &&
        -> asio::awaitable<Chan<SplitCont_t<Action_t<S>>, Tx, Rx>>
        requires IsSplit<Action_t<S>>::value
              && requires(F fn,
                          Chan<SplitTx_t<Action_t<S>>, Tx, Unavailable> tx_chan,
                          Chan<SplitRx_t<Action_t<S>>, Unavailable, Rx> rx_chan) {
                     { std::forward<F>(fn)(std::move(tx_chan), std::move(rx_chan)) }
                         -> std::same_as<asio::awaitable<
                                std::pair<Chan<Done, Tx, Unavailable>,
                                          Chan<Done, Unavailable, Rx>>>>;
                 }
    {
        using TxProto = SplitTx_t<Action_t<S>>;
        using RxProto = SplitRx_t<Action_t<S>>;
        using ContProto = SplitCont_t<Action_t<S>>;

        this->markConsumed();
        Chan<TxProto, Tx, Unavailable> tx_chan(std::move(tx_), Unavailable{});
        Chan<RxProto, Unavailable, Rx> rx_chan(Unavailable{}, std::move(rx_));

        auto [tx_done, rx_done] =
            co_await std::forward<F>(f)(std::move(tx_chan), std::move(rx_chan));

        tx_done.markConsumed();
        rx_done.markConsumed();
        co_return Chan<ContProto, Tx, Rx>(
            std::move(tx_done.tx_), std::move(rx_done.rx_));
    }

    // -- close: Action_t<S> is Done ---------------------------------------------

    /// close() stays synchronous (void, not a coroutine) because it performs
    /// no I/O — just marks the channel consumed.
    void close() &&
        requires IsDone<Action_t<S>>::value
    {
        this->markConsumed();
    }
};

template<typename S, Transmitter Tx, Receiver Rx>
    requires Session<S>
Chan<S, Tx, Rx> makeChan(Tx tx, Rx rx) {
    return Chan<S, Tx, Rx>(std::move(tx), std::move(rx));
}

} // namespace nix::session
