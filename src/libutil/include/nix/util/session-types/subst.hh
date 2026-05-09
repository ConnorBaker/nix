#pragma once
///@file
///
/// Continue substitution for session type protocols (De Bruijn).
///

#include <type_traits>

#include "nix/util/session-types/protocol.hh"

namespace nix::session {

template<typename Self, typename P, unsigned N = 0>
struct Subst;
template<typename Self, typename P, unsigned N = 0>
using Subst_t = typename Subst<Self, P, N>::type;

template<typename P, unsigned N>
struct Subst<Done, P, N> { using type = Done; };

template<typename T, typename Q, typename P, unsigned N>
struct Subst<Send<T, Q>, P, N> { using type = Send<T, Subst_t<Q, P, N>>; };

template<typename T, typename Q, typename P, unsigned N>
struct Subst<Recv<T, Q>, P, N> { using type = Recv<T, Subst_t<Q, P, N>>; };

template<typename Q, typename P, unsigned N>
struct Subst<Loop<Q>, P, N> { using type = Loop<Subst_t<Q, P, N + 1>>; };

template<unsigned I, typename P, unsigned N>
struct Subst<Continue<I>, P, N> {
    using type = std::conditional_t<I == N, P, Continue<I>>;
};

template<typename... Qs, typename P, unsigned N>
struct Subst<Choose<Qs...>, P, N> { using type = Choose<Subst_t<Qs, P, N>...>; };

template<typename... Qs, typename P, unsigned N>
struct Subst<Offer<Qs...>, P, N> { using type = Offer<Subst_t<Qs, P, N>...>; };

template<typename A, typename B, typename P, unsigned N>
struct Subst<Call<A, B>, P, N> { using type = Call<Subst_t<A, P, N>, Subst_t<B, P, N>>; };

template<typename A, typename B, typename C, typename P, unsigned N>
struct Subst<Split<A, B, C>, P, N> { using type = Split<Subst_t<A, P, N>, Subst_t<B, P, N>, Subst_t<C, P, N>>; };

} // namespace nix::session
