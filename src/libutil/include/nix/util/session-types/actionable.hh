#pragma once
///@file
///
/// Next-action extraction for session type protocols.
///

#include <type_traits>

#include "nix/util/session-types/protocol.hh"
#include "nix/util/session-types/subst.hh"

namespace nix::session {

template<typename P> struct Action;
template<typename P> using Action_t = typename Action<P>::type;

template<>
struct Action<Done> { using type = Done; };

template<typename T, typename P>
struct Action<Send<T, P>> { using type = Send<T, P>; };

template<typename T, typename P>
struct Action<Recv<T, P>> { using type = Recv<T, P>; };

template<typename... Ps>
struct Action<Choose<Ps...>> { using type = Choose<Ps...>; };

template<typename... Ps>
struct Action<Offer<Ps...>> { using type = Offer<Ps...>; };

template<typename P, typename Q>
struct Action<Call<P, Q>> { using type = Call<P, Q>; };

template<typename P, typename Q, typename R>
struct Action<Split<P, Q, R>> { using type = Split<P, Q, R>; };

// Loop: unroll once via Subst, then recurse
template<typename P>
struct Action<Loop<P>> { using type = Action_t<Subst_t<P, Loop<P>>>; };

// -- Actionable concept -------------------------------------------------------

namespace detail {

template<typename P, typename = void>
struct IsActionable : std::false_type {};

template<typename P>
struct IsActionable<P, std::void_t<Action_t<P>>> : std::true_type {};

} // namespace detail

template<typename P>
concept Actionable = detail::IsActionable<P>::value;

} // namespace nix::session
