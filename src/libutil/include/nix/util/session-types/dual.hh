#pragma once
///@file
///
/// Duality transform for session type protocols.
///

#include "nix/util/session-types/protocol.hh"

namespace nix::session {

template<typename P> struct Dual;
template<typename P> using Dual_t = typename Dual<P>::type;

template<>
struct Dual<Done> { using type = Done; };

template<typename T, typename P>
struct Dual<Send<T, P>> { using type = Recv<T, Dual_t<P>>; };

template<typename T, typename P>
struct Dual<Recv<T, P>> { using type = Send<T, Dual_t<P>>; };

template<typename... Ps>
struct Dual<Choose<Ps...>> { using type = Offer<Dual_t<Ps>...>; };

template<typename... Ps>
struct Dual<Offer<Ps...>> { using type = Choose<Dual_t<Ps>...>; };

template<typename P>
struct Dual<Loop<P>> { using type = Loop<Dual_t<P>>; };

template<unsigned I>
struct Dual<Continue<I>> { using type = Continue<I>; };

template<typename P, typename Q>
struct Dual<Call<P, Q>> { using type = Call<Dual_t<P>, Dual_t<Q>>; };

// Split: P and Q flip
template<typename P, typename Q, typename R>
struct Dual<Split<P, Q, R>> { using type = Split<Dual_t<Q>, Dual_t<P>, Dual_t<R>>; };

} // namespace nix::session
