#pragma once
///@file
///
/// Protocol sequencing: replace Done with a continuation protocol.
///

#include "nix/util/session-types/lift.hh"
#include "nix/util/session-types/protocol.hh"

namespace nix::session {

template<typename Self, typename P, unsigned N = 0>
struct Then;
template<typename Self, typename P, unsigned N = 0>
using Then_t = typename Then<Self, P, N>::type;

// Done -> splice in P, shifted by N for current depth
template<typename P, unsigned N>
struct Then<Done, P, N> { using type = Lift_t<P, N>; };

// Continue: unchanged (not a termination point)
template<unsigned I, typename P, unsigned N>
struct Then<Continue<I>, P, N> { using type = Continue<I>; };

// Loop: increment N
template<typename Q, typename P, unsigned N>
struct Then<Loop<Q>, P, N> { using type = Loop<Then_t<Q, P, N + 1>>; };

// Send/Recv: recurse through tail
template<typename T, typename Q, typename P, unsigned N>
struct Then<Send<T, Q>, P, N> { using type = Send<T, Then_t<Q, P, N>>; };

template<typename T, typename Q, typename P, unsigned N>
struct Then<Recv<T, Q>, P, N> { using type = Recv<T, Then_t<Q, P, N>>; };

// Choose/Offer: all branches get continuation
template<typename... Qs, typename P, unsigned N>
struct Then<Choose<Qs...>, P, N> { using type = Choose<Then_t<Qs, P, N>...>; };

template<typename... Qs, typename P, unsigned N>
struct Then<Offer<Qs...>, P, N> { using type = Offer<Then_t<Qs, P, N>...>; };

// Call<A, B>: only B gets the continuation
template<typename A, typename B, typename P, unsigned N>
struct Then<Call<A, B>, P, N> { using type = Call<A, Then_t<B, P, N>>; };

// Split<A, B, C>: only C gets the continuation
template<typename A, typename B, typename C, typename P, unsigned N>
struct Then<Split<A, B, C>, P, N> { using type = Split<A, B, Then_t<C, P, N>>; };

} // namespace nix::session
