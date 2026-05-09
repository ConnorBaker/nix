#pragma once
///@file
///
/// Index shifting (lifting) for free Continue variables in session type protocols.
///

#include "nix/util/session-types/protocol.hh"

namespace nix::session {

template<typename Self, unsigned N, unsigned Level = 0>
struct Lift;
template<typename Self, unsigned N, unsigned Level = 0>
using Lift_t = typename Lift<Self, N, Level>::type;

template<unsigned N, unsigned Level>
struct Lift<Done, N, Level> { using type = Done; };

template<unsigned I, unsigned N, unsigned Level>
struct Lift<Continue<I>, N, Level> {
    using type = Continue<(I >= Level) ? (I + N) : I>;
};

template<typename P, unsigned N, unsigned Level>
struct Lift<Loop<P>, N, Level> { using type = Loop<Lift_t<P, N, Level + 1>>; };

template<typename T, typename P, unsigned N, unsigned Level>
struct Lift<Send<T, P>, N, Level> { using type = Send<T, Lift_t<P, N, Level>>; };

template<typename T, typename P, unsigned N, unsigned Level>
struct Lift<Recv<T, P>, N, Level> { using type = Recv<T, Lift_t<P, N, Level>>; };

template<typename... Ps, unsigned N, unsigned Level>
struct Lift<Choose<Ps...>, N, Level> { using type = Choose<Lift_t<Ps, N, Level>...>; };

template<typename... Ps, unsigned N, unsigned Level>
struct Lift<Offer<Ps...>, N, Level> { using type = Offer<Lift_t<Ps, N, Level>...>; };

template<typename A, typename B, unsigned N, unsigned Level>
struct Lift<Call<A, B>, N, Level> { using type = Call<Lift_t<A, N, Level>, Lift_t<B, N, Level>>; };

template<typename A, typename B, typename C, unsigned N, unsigned Level>
struct Lift<Split<A, B, C>, N, Level> { using type = Split<Lift_t<A, N, Level>, Lift_t<B, N, Level>, Lift_t<C, N, Level>>; };

} // namespace nix::session
