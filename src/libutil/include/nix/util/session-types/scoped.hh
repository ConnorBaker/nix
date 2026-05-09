#pragma once
///@file
///
/// De Bruijn well-scopedness check for session type protocols.
///

#include <type_traits>

#include "nix/util/session-types/protocol.hh"

namespace nix::session {

namespace detail {

template<typename P, unsigned N>
struct IsScopedImpl : std::false_type {};

template<unsigned N>
struct IsScopedImpl<Done, N> : std::true_type {};

template<typename T, typename P, unsigned N>
struct IsScopedImpl<Send<T, P>, N> : IsScopedImpl<P, N> {};

template<typename T, typename P, unsigned N>
struct IsScopedImpl<Recv<T, P>, N> : IsScopedImpl<P, N> {};

template<typename P, unsigned N>
struct IsScopedImpl<Loop<P>, N> : IsScopedImpl<P, N + 1> {};

template<unsigned I, unsigned N>
struct IsScopedImpl<Continue<I>, N> : std::bool_constant<(I < N)> {};

template<typename... Ps, unsigned N>
struct IsScopedImpl<Choose<Ps...>, N> : std::bool_constant<(IsScopedImpl<Ps, N>::value && ...)> {};

template<typename... Ps, unsigned N>
struct IsScopedImpl<Offer<Ps...>, N> : std::bool_constant<(IsScopedImpl<Ps, N>::value && ...)> {};

template<typename P, typename Q, unsigned N>
struct IsScopedImpl<Call<P, Q>, N>
    : std::bool_constant<IsScopedImpl<P, N>::value && IsScopedImpl<Q, N>::value> {};

template<typename P, typename Q, typename R, unsigned N>
struct IsScopedImpl<Split<P, Q, R>, N>
    : std::bool_constant<IsScopedImpl<P, N>::value && IsScopedImpl<Q, N>::value
                          && IsScopedImpl<R, N>::value> {};

} // namespace detail

/// P is well-scoped: every Continue<I> satisfies I < its enclosing Loop depth.
template<typename P, unsigned N = 0>
concept Scoped = detail::IsScopedImpl<P, N>::value;

} // namespace nix::session
