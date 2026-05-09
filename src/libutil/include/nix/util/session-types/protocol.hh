#pragma once
///@file
///
/// Session type protocol constructors and TypeList utilities.
///

#include <cstddef>
#include <tuple>
#include <type_traits>

namespace nix::session {

// -- Protocol constructors ----------------------------------------------------

struct Done {};

template<typename T, typename P> struct Send {};
template<typename T, typename P> struct Recv {};

template<typename... Choices> struct Choose {};
template<typename... Choices> struct Offer {};

template<typename P> struct Loop {};
template<unsigned I> struct Continue {};

template<typename P, typename Q> struct Call {};
template<typename P, typename Q, typename R> struct Split {};

// -- TypeList -----------------------------------------------------------------

template<typename... Ts>
struct TypeList {
    static constexpr unsigned size = sizeof...(Ts);
};

// Head<TypeList<T, Ts...>> = T
template<typename List> struct Head;
template<typename T, typename... Ts>
struct Head<TypeList<T, Ts...>> { using type = T; };
template<typename List>
using Head_t = typename Head<List>::type;

// Tail<TypeList<T, Ts...>> = TypeList<Ts...>
template<typename List> struct Tail;
template<typename T, typename... Ts>
struct Tail<TypeList<T, Ts...>> { using type = TypeList<Ts...>; };
template<typename List>
using Tail_t = typename Tail<List>::type;

// At<List, N> = Nth element
template<typename List, unsigned N> struct At;
template<typename... Ts, unsigned N>
struct At<TypeList<Ts...>, N> {
    static_assert(N < sizeof...(Ts), "TypeList index out of range");
    using type = std::tuple_element_t<N, std::tuple<Ts...>>;
};
template<typename List, unsigned N>
using At_t = typename At<List, N>::type;

// Map<F, TypeList<Ts...>> = TypeList<F<Ts>::type...>
template<template<typename> class F, typename List> struct Map;
template<template<typename> class F, typename... Ts>
struct Map<F, TypeList<Ts...>> { using type = TypeList<typename F<Ts>::type...>; };
template<template<typename> class F, typename List>
using Map_t = typename Map<F, List>::type;

// All<Pred, TypeList<Ts...>> = (Pred<Ts>::value && ...)
template<template<typename> class Pred, typename List> struct All;
template<template<typename> class Pred, typename... Ts>
struct All<Pred, TypeList<Ts...>> : std::bool_constant<(Pred<Ts>::value && ...)> {};

// -- Protocol type traits -----------------------------------------------------

template<typename P> struct IsChoose : std::false_type {};
template<typename... Ps> struct IsChoose<Choose<Ps...>> : std::true_type {};

template<typename P> struct IsOffer : std::false_type {};
template<typename... Ps> struct IsOffer<Offer<Ps...>> : std::true_type {};

template<typename P> struct BranchCount;
template<typename... Ps>
struct BranchCount<Choose<Ps...>> : std::integral_constant<unsigned, sizeof...(Ps)> {};
template<typename... Ps>
struct BranchCount<Offer<Ps...>> : std::integral_constant<unsigned, sizeof...(Ps)> {};

// -- ToTypeList: extract parameter pack from Choose/Offer ---------------------

template<typename P> struct ToTypeList;
template<typename... Ps>
struct ToTypeList<Choose<Ps...>> { using type = TypeList<Ps...>; };
template<typename... Ps>
struct ToTypeList<Offer<Ps...>> { using type = TypeList<Ps...>; };
template<typename P>
using ToTypeList_t = typename ToTypeList<P>::type;

// -- Choice: unified branch selection value -----------------------------------

struct Choice { unsigned index; };

// -- Action decomposition traits ----------------------------------------------

template<typename P> struct IsSend : std::false_type {};
template<typename T, typename P> struct IsSend<Send<T, P>> : std::true_type {};

template<typename P> struct IsRecv : std::false_type {};
template<typename T, typename P> struct IsRecv<Recv<T, P>> : std::true_type {};

template<typename P> struct IsDone : std::false_type {};
template<> struct IsDone<Done> : std::true_type {};

template<typename P> struct IsCall : std::false_type {};
template<typename P, typename Q> struct IsCall<Call<P, Q>> : std::true_type {};

template<typename P> struct IsSplit : std::false_type {};
template<typename P, typename Q, typename R> struct IsSplit<Split<P, Q, R>> : std::true_type {};

// Sentinel type and defaults for decomposition traits.
//
// GCC eagerly evaluates Chan method signatures (parameter types, return
// types) during class template instantiation, even when requires clauses
// would prevent calling the method. Without defaults, Payload_t<Choose<...>>
// is an incomplete type, causing a hard error when Chan<Choose<...>, Tx, Rx>
// is instantiated.
//
// DecomposeDefault is used for Payload (function parameter position — void
// would be illegal). Done is used for Continuation/CallTarget/CallCont
// (protocol type parameter to Chan — Done is a valid protocol type).
struct DecomposeDefault {};

// Payload_t<Send<T,P>> = T, Payload_t<Recv<T,P>> = T
template<typename A> struct Payload { using type = DecomposeDefault; };
template<typename T, typename P> struct Payload<Send<T, P>> { using type = T; };
template<typename T, typename P> struct Payload<Recv<T, P>> { using type = T; };
template<typename A> using Payload_t = typename Payload<A>::type;

// Continuation_t<Send<T,P>> = P, Continuation_t<Recv<T,P>> = P
template<typename A> struct Continuation { using type = Done; };
template<typename T, typename P> struct Continuation<Send<T, P>> { using type = P; };
template<typename T, typename P> struct Continuation<Recv<T, P>> { using type = P; };
template<typename A> using Continuation_t = typename Continuation<A>::type;

// CallTarget_t<Call<P,Q>> = P (the sub-protocol)
template<typename A> struct CallTarget { using type = Done; };
template<typename P, typename Q> struct CallTarget<Call<P, Q>> { using type = P; };
template<typename A> using CallTarget_t = typename CallTarget<A>::type;

// CallCont_t<Call<P,Q>> = Q (the continuation after sub-protocol completes)
template<typename A> struct CallCont { using type = Done; };
template<typename P, typename Q> struct CallCont<Call<P, Q>> { using type = Q; };
template<typename A> using CallCont_t = typename CallCont<A>::type;

// SplitTx_t<Split<P,Q,R>> = P (the tx-only sub-protocol)
template<typename A> struct SplitTx { using type = Done; };
template<typename P, typename Q, typename R> struct SplitTx<Split<P, Q, R>> { using type = P; };
template<typename A> using SplitTx_t = typename SplitTx<A>::type;

// SplitRx_t<Split<P,Q,R>> = Q (the rx-only sub-protocol)
template<typename A> struct SplitRx { using type = Done; };
template<typename P, typename Q, typename R> struct SplitRx<Split<P, Q, R>> { using type = Q; };
template<typename A> using SplitRx_t = typename SplitRx<A>::type;

// SplitCont_t<Split<P,Q,R>> = R (the continuation after split completes)
template<typename A> struct SplitCont { using type = Done; };
template<typename P, typename Q, typename R> struct SplitCont<Split<P, Q, R>> { using type = R; };
template<typename A> using SplitCont_t = typename SplitCont<A>::type;

} // namespace nix::session
