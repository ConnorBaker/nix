#pragma once
///@file
///
/// Type-level branch selection for Choose/Offer protocols.
///

#include <cstddef>
#include <tuple>
#include <utility>

#include "nix/util/session-types/protocol.hh"

namespace nix::session {

// -- RemoveAt: TypeList with Nth element removed ------------------------------

namespace detail {

// Concatenate TypeLists
template<typename... Lists> struct Concat;
template<> struct Concat<> { using type = TypeList<>; };
template<typename... Ts> struct Concat<TypeList<Ts...>> { using type = TypeList<Ts...>; };
template<typename... Ts, typename... Us, typename... Rest>
struct Concat<TypeList<Ts...>, TypeList<Us...>, Rest...>
    : Concat<TypeList<Ts..., Us...>, Rest...> {};

template<typename List, unsigned N, typename Seq>
struct RemoveAtImpl;

template<typename... Ts, unsigned N, std::size_t... Is>
struct RemoveAtImpl<TypeList<Ts...>, N, std::index_sequence<Is...>> {
    template<std::size_t I, typename T>
    using MaybeKeep = std::conditional_t<(I != N), TypeList<T>, TypeList<>>;

    using type = typename Concat<
        MaybeKeep<Is, std::tuple_element_t<Is, std::tuple<Ts...>>>...
    >::type;
};

} // namespace detail

template<typename List, unsigned N>
struct RemoveAt;

template<typename... Ts, unsigned N>
struct RemoveAt<TypeList<Ts...>, N> {
    static_assert(N < sizeof...(Ts), "RemoveAt index out of range");
    using type = typename detail::RemoveAtImpl<
        TypeList<Ts...>, N, std::make_index_sequence<sizeof...(Ts)>>::type;
};

template<typename List, unsigned N>
using RemoveAt_t = typename RemoveAt<List, N>::type;

// -- SelectBranch: Nth protocol from a Choose/Offer ---------------------------

template<typename Branching, unsigned N>
struct SelectBranch;

template<typename... Ps, unsigned N>
struct SelectBranch<Choose<Ps...>, N> {
    static_assert(N < sizeof...(Ps), "Branch index out of range");
    using type = std::tuple_element_t<N, std::tuple<Ps...>>;
};

template<typename... Ps, unsigned N>
struct SelectBranch<Offer<Ps...>, N> {
    static_assert(N < sizeof...(Ps), "Branch index out of range");
    using type = std::tuple_element_t<N, std::tuple<Ps...>>;
};

template<typename B, unsigned N>
using SelectBranch_t = typename SelectBranch<B, N>::type;

} // namespace nix::session
