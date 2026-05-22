#pragma once
/**
 * @file
 *
 * Template implementations (as opposed to mere declarations).
 *
 * This file is an example of the "impl.hh" pattern. See the
 * contributing guide.
 */

#include "nix/store/common-protocol.hh"
#include "nix/store/length-prefixed-protocol-helper.hh"

namespace nix {

/* protocol-agnostic templates */

#define COMMON_USE_LENGTH_PREFIX_SERIALISER(TEMPLATE, T)                                                 \
    TEMPLATE T CommonProto::Serialise<T>::read(const StoreDirConfig & store, CommonProto::ReadConn conn) \
    {                                                                                                    \
        return LengthPrefixedProtoHelper<CommonProto, T>::read(store, conn);                             \
    }                                                                                                    \
    TEMPLATE void CommonProto::Serialise<T>::write(                                                      \
        const StoreDirConfig & store, CommonProto::WriteConn conn, const T & t)                          \
    {                                                                                                    \
        LengthPrefixedProtoHelper<CommonProto, T>::write(store, conn, t);                                \
    }

COMMON_USE_LENGTH_PREFIX_SERIALISER(template<typename T>, std::vector<T>)
#define COMMON_USE_LENGTH_PREFIX_SERIALISER_COMMA ,
COMMON_USE_LENGTH_PREFIX_SERIALISER(
    template<typename T COMMON_USE_LENGTH_PREFIX_SERIALISER_COMMA typename Compare>,
    std::set<T COMMON_USE_LENGTH_PREFIX_SERIALISER_COMMA Compare>)
COMMON_USE_LENGTH_PREFIX_SERIALISER(template<typename... Ts>, std::tuple<Ts...>)

COMMON_USE_LENGTH_PREFIX_SERIALISER(
    template<typename K COMMON_USE_LENGTH_PREFIX_SERIALISER_COMMA typename V COMMON_USE_LENGTH_PREFIX_SERIALISER_COMMA
             typename Compare>
    ,
    std::map<K COMMON_USE_LENGTH_PREFIX_SERIALISER_COMMA V COMMON_USE_LENGTH_PREFIX_SERIALISER_COMMA Compare>)
#undef COMMON_USE_LENGTH_PREFIX_SERIALISER_COMMA

/* protocol-specific templates */

} // namespace nix
