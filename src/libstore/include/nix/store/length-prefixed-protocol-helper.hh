#pragma once
/**
 * @file
 *
 * Reusable serialisers for length-prefixed container types.
 *
 * Used by the worker, serve, and common protocols. Each protocol
 * declares (via `DECLARE_PROTO_SERIALISER`) the four container
 * specialisations of `Proto::Serialise<T>` for `std::vector<T>`,
 * `std::set<T, Compare>`, `std::tuple<Ts...>`, and `std::map<K, V, Compare>`,
 * then includes this header to define the bodies as forwards to
 * `LengthPrefixedProtoHelper<Proto, T>::read`/`write`.
 */

#include <map>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "nix/util/types.hh"

namespace nix {

struct StoreDirConfig;

namespace length_prefixed_detail {

template<typename T>
struct is_vector : std::false_type
{};

template<typename T, typename A>
struct is_vector<std::vector<T, A>> : std::true_type
{};

template<typename T>
struct is_set : std::false_type
{};

template<typename T, typename C, typename A>
struct is_set<std::set<T, C, A>> : std::true_type
{};

template<typename T>
struct is_map : std::false_type
{};

template<typename K, typename V, typename C, typename A>
struct is_map<std::map<K, V, C, A>> : std::true_type
{};

template<typename T>
struct is_tuple : std::false_type
{};

template<typename... Ts>
struct is_tuple<std::tuple<Ts...>> : std::true_type
{};

} // namespace length_prefixed_detail

/**
 * Concept matching the four container shapes that have length-prefixed
 * serialisers (`std::vector`, `std::set`, `std::map`, `std::tuple`).
 */
template<typename T>
concept LengthPrefixContainer = length_prefixed_detail::is_vector<T>::value
                             || length_prefixed_detail::is_set<T>::value
                             || length_prefixed_detail::is_map<T>::value
                             || length_prefixed_detail::is_tuple<T>::value;

/**
 * Reusable read/write helper for length-prefixed container types.
 *
 * `Inner` is the protocol class (e.g. `WorkerProto`, `ServeProto`,
 * `CommonProto`) and `T` is one of the four container specialisations
 * matching `LengthPrefixContainer`. The element-level (re)dispatch goes
 * back through `Inner::Serialise<element_type>` so each protocol's
 * conventions are honoured.
 */
template<class Inner, LengthPrefixContainer T>
struct LengthPrefixedProtoHelper
{
private:
    template<typename U>
    using S = typename Inner::template Serialise<U>;

public:
    static T read(const StoreDirConfig & store, typename Inner::ReadConn conn)
    {
        if constexpr (length_prefixed_detail::is_vector<T>::value) {
            T result;
            auto size = readNum<size_t>(conn.from);
            while (size--)
                result.push_back(S<typename T::value_type>::read(store, conn));
            return result;
        } else if constexpr (length_prefixed_detail::is_set<T>::value) {
            T result;
            auto size = readNum<size_t>(conn.from);
            while (size--)
                result.insert(S<typename T::value_type>::read(store, conn));
            return result;
        } else if constexpr (length_prefixed_detail::is_map<T>::value) {
            T result;
            auto size = readNum<size_t>(conn.from);
            while (size--) {
                auto k = S<typename T::key_type>::read(store, conn);
                auto v = S<typename T::mapped_type>::read(store, conn);
                result.insert_or_assign(std::move(k), std::move(v));
            }
            return result;
        } else /* tuple */ {
            return [&]<size_t... Is>(std::index_sequence<Is...>) {
                return T{S<std::tuple_element_t<Is, T>>::read(store, conn)...};
            }(std::make_index_sequence<std::tuple_size_v<T>>{});
        }
    }

    static void write(const StoreDirConfig & store, typename Inner::WriteConn conn, const T & value)
    {
        if constexpr (length_prefixed_detail::is_vector<T>::value
                      || length_prefixed_detail::is_set<T>::value) {
            conn.to << value.size();
            for (auto & elem : value)
                S<typename T::value_type>::write(store, conn, elem);
        } else if constexpr (length_prefixed_detail::is_map<T>::value) {
            conn.to << value.size();
            for (auto & [k, v] : value) {
                S<typename T::key_type>::write(store, conn, k);
                S<typename T::mapped_type>::write(store, conn, v);
            }
        } else /* tuple */ {
            std::apply(
                [&]<typename... Us>(const Us &... args) { (S<Us>::write(store, conn, args), ...); },
                value);
        }
    }
};

} // namespace nix

/**
 * Define `Proto::Serialise<T>::read`/`write` bodies for the four
 * length-prefixed container specialisations (`std::vector<T>`,
 * `std::set<T, Compare>`, `std::tuple<Ts...>`, `std::map<K, V, Compare>`)
 * as thin forwards to `nix::LengthPrefixedProtoHelper<Proto, T>`.
 *
 * Used by `worker-protocol-impl.hh`, `serve-protocol-impl.hh`, and
 * `common-protocol-impl.hh`. Each impl header invokes this macro once
 * with its protocol class to instantiate the four pairs at namespace
 * scope. The corresponding declarations are emitted by
 * `DECLARE_PROTO_SERIALISER` in the protocol headers.
 *
 * Must be invoked at `nix` namespace scope (not inside `namespace nix
 * { ... }`).
 */
#define NIX_DEFINE_LENGTH_PREFIX_SERIALISERS(Proto)                                                             \
    namespace nix {                                                                                             \
    template<typename T>                                                                                        \
    std::vector<T>                                                                                              \
    Proto::Serialise<std::vector<T>>::read(const StoreDirConfig & store, Proto::ReadConn conn)                  \
    {                                                                                                           \
        return LengthPrefixedProtoHelper<Proto, std::vector<T>>::read(store, conn);                             \
    }                                                                                                           \
    template<typename T>                                                                                        \
    void Proto::Serialise<std::vector<T>>::write(                                                               \
        const StoreDirConfig & store, Proto::WriteConn conn, const std::vector<T> & v)                          \
    {                                                                                                           \
        LengthPrefixedProtoHelper<Proto, std::vector<T>>::write(store, conn, v);                                \
    }                                                                                                           \
                                                                                                                \
    template<typename T, typename Compare>                                                                      \
    std::set<T, Compare>                                                                                        \
    Proto::Serialise<std::set<T, Compare>>::read(const StoreDirConfig & store, Proto::ReadConn conn)            \
    {                                                                                                           \
        return LengthPrefixedProtoHelper<Proto, std::set<T, Compare>>::read(store, conn);                       \
    }                                                                                                           \
    template<typename T, typename Compare>                                                                      \
    void Proto::Serialise<std::set<T, Compare>>::write(                                                         \
        const StoreDirConfig & store, Proto::WriteConn conn, const std::set<T, Compare> & v)                    \
    {                                                                                                           \
        LengthPrefixedProtoHelper<Proto, std::set<T, Compare>>::write(store, conn, v);                          \
    }                                                                                                           \
                                                                                                                \
    template<typename... Ts>                                                                                    \
    std::tuple<Ts...>                                                                                           \
    Proto::Serialise<std::tuple<Ts...>>::read(const StoreDirConfig & store, Proto::ReadConn conn)               \
    {                                                                                                           \
        return LengthPrefixedProtoHelper<Proto, std::tuple<Ts...>>::read(store, conn);                          \
    }                                                                                                           \
    template<typename... Ts>                                                                                    \
    void Proto::Serialise<std::tuple<Ts...>>::write(                                                            \
        const StoreDirConfig & store, Proto::WriteConn conn, const std::tuple<Ts...> & v)                       \
    {                                                                                                           \
        LengthPrefixedProtoHelper<Proto, std::tuple<Ts...>>::write(store, conn, v);                             \
    }                                                                                                           \
                                                                                                                \
    template<typename K, typename V, typename Compare>                                                          \
    std::map<K, V, Compare>                                                                                     \
    Proto::Serialise<std::map<K, V, Compare>>::read(const StoreDirConfig & store, Proto::ReadConn conn)         \
    {                                                                                                           \
        return LengthPrefixedProtoHelper<Proto, std::map<K, V, Compare>>::read(store, conn);                    \
    }                                                                                                           \
    template<typename K, typename V, typename Compare>                                                          \
    void Proto::Serialise<std::map<K, V, Compare>>::write(                                                      \
        const StoreDirConfig & store, Proto::WriteConn conn, const std::map<K, V, Compare> & v)                 \
    {                                                                                                           \
        LengthPrefixedProtoHelper<Proto, std::map<K, V, Compare>>::write(store, conn, v);                       \
    }                                                                                                           \
    } /* namespace nix */
