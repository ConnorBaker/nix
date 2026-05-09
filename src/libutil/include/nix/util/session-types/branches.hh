#pragma once
///@file
///
/// Branches type for session type offer matching.
///
/// Branches holds the received choice index + tx/rx, deferring Chan
/// construction until visit() dispatches to the matching branch.
///

#include <tuple>
#include <type_traits>
#include <utility>

#include "nix/util/linear.hh"
#include "nix/util/session-types/backend.hh"
#include "nix/util/session-types/protocol.hh"
#include "nix/util/session-types/select.hh"

namespace nix::session {

// Forward declaration — Chan is defined in chan.hh.
// Template uses the same concept constraints as the definition.
template<typename S, Transmitter Tx, Receiver Rx> class Chan;

// Compute the common return type of visiting all branch types.
// (Separate detail block — the Chan forward declaration above must be
// outside detail, splitting the namespace.)
namespace detail {

template<typename OfferType, typename Tx, typename Rx, typename... Fs>
struct VisitReturnImpl;

template<typename... Ps, typename Tx, typename Rx, typename... Fs>
struct VisitReturnImpl<Offer<Ps...>, Tx, Rx, Fs...> {
    using type = std::common_type_t<
        std::invoke_result_t<Fs, Chan<Ps, Tx, Rx>>...>;
};

} // namespace detail

template<typename OfferType, typename Tx, typename Rx, typename... Fs>
using VisitReturn_t =
    typename detail::VisitReturnImpl<OfferType, Tx, Rx, Fs...>::type;

/// Deferred-construction offer result.
///
/// Constructed by Chan::offer(). Holds the received choice index and
/// the tx/rx halves, constructing the specific Chan<Pi, Tx, Rx> only
/// when visit() dispatches to the matching branch.
template<typename OfferType, Transmitter Tx, Receiver Rx>
class [[nodiscard("Branches must be consumed via visit()")]] Branches
    : public nix::Linear<Branches<OfferType, Tx, Rx>>
{
public:
    static constexpr const char * linearName = "Branches";

private:
    unsigned index_;
    Tx tx_;
    Rx rx_;

    template<typename, Transmitter, Receiver> friend class Chan;

    Branches(unsigned index, Tx tx, Rx rx)
        : index_(index), tx_(std::move(tx)), rx_(std::move(rx)) {}

    /// Recursive dispatch: try each branch index until the match.
    template<unsigned I, typename R, typename Tuple>
    R visit_dispatch(Tuple && fns) {
        using P = SelectBranch_t<OfferType, I>;
        if constexpr (I + 1 == BranchCount<OfferType>::value) {
            // Last branch — must be this one.
            return std::get<I>(std::forward<Tuple>(fns))(
                Chan<P, Tx, Rx>(std::move(tx_), std::move(rx_)));
        } else {
            if (index_ == I) {
                return std::get<I>(std::forward<Tuple>(fns))(
                    Chan<P, Tx, Rx>(std::move(tx_), std::move(rx_)));
            }
            return visit_dispatch<I + 1, R>(std::forward<Tuple>(fns));
        }
    }

public:
    Branches(Branches && o) noexcept
        : nix::Linear<Branches>(std::move(o))
        , index_(o.index_), tx_(std::move(o.tx_)), rx_(std::move(o.rx_))
    {}

    Branches(const Branches &) = delete;
    Branches & operator=(const Branches &) = delete;
    Branches & operator=(Branches &&) = delete;

    /// Exhaustive match: one callable per branch.
    /// Each Fi receives Chan<Pi, Tx, Rx> and must return the same type R.
    ///
    /// The requires clause checks:
    ///   1. Exactly one callable per branch (sizeof... == BranchCount).
    ///   2. All callables are invocable with their respective Chan<Pi, Tx, Rx>
    ///      and their return types share a common type (VisitReturn_t is well-formed).
    template<typename... Fs>
    auto visit(Fs &&... fs) &&
        -> VisitReturn_t<OfferType, Tx, Rx, Fs...>
        requires (sizeof...(Fs) == BranchCount<OfferType>::value)
              && requires { typename VisitReturn_t<OfferType, Tx, Rx, Fs...>; }
    {
        this->markConsumed();
        using R = VisitReturn_t<OfferType, Tx, Rx, Fs...>;
        return visit_dispatch<0, R>(
            std::forward_as_tuple(std::forward<Fs>(fs)...));
    }
};

} // namespace nix::session
