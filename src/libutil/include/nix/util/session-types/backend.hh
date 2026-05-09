#pragma once
///@file
///
/// Transmitter/Receiver concepts for session type backends.
///
/// Async-first design: backends return asio::awaitable<T> from their
/// transmit/receive methods. Errors propagate as exceptions.
///
/// Two-tier design:
///   - Transmitter / Receiver: base concepts requiring a tag type.
///     Used as template constraints on Chan and Branches class parameters.
///   - CanTransmit<Tx, T> / CanReceive<Rx, T>: per-type concepts checked
///     at each Chan method call site (e.g., send() requires CanTransmit<Tx, T>).
///
/// This split avoids requiring the backend to declare transmit/receive for
/// every possible type upfront — the per-type check is deferred to the
/// point where the type is actually used.
///
/// Tag types (transmitter_tag / receiver_tag) serve the same role as the
/// former Error associated type: they let the base concept constrain class
/// template parameters without requiring per-message-type method signatures.
/// Backends declare `using transmitter_tag = void;` / `using receiver_tag = void;`.
///

#include <concepts>

#include <boost/asio/awaitable.hpp>

namespace nix::session {

namespace asio = boost::asio;

/// A Transmitter declares itself via a tag type.
template<typename Tx>
concept Transmitter = requires { typename Tx::transmitter_tag; };

/// A Transmitter that can send values of type T.
template<typename Tx, typename T>
concept CanTransmit = Transmitter<Tx>
    && requires(Tx & tx, T val) {
        { tx.transmit(std::move(val)) }
            -> std::same_as<asio::awaitable<void>>;
    };

/// A Receiver declares itself via a tag type.
template<typename Rx>
concept Receiver = requires { typename Rx::receiver_tag; };

/// A Receiver that can receive values of type T.
template<typename Rx, typename T>
concept CanReceive = Receiver<Rx>
    && requires(Rx & rx) {
        { rx.template receive<T>() }
            -> std::same_as<asio::awaitable<T>>;
    };

/// Placeholder backend half for split(). Satisfies Transmitter and Receiver
/// at the class-template level (has both tag types) but provides no
/// transmit()/receive() methods, so CanTransmit and CanReceive are false
/// for all types. A Chan<P, Unavailable, Rx> can only call rx-side ops
/// (recv, offer); a Chan<P, Tx, Unavailable> can only call tx-side ops
/// (send, choose).
struct Unavailable {
    using transmitter_tag = void;
    using receiver_tag = void;
};

} // namespace nix::session
