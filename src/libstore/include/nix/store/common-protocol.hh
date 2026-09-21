#pragma once
///@file

#include "nix/util/serialise.hh"
#include "nix/util/object-hash.hh"

#include <functional>
#include <optional>
#include <utility>
#include <variant>

namespace nix {

struct StoreDirConfig;
struct Source;

// items being serialized
class StorePath;
struct ContentAddress;
struct DrvOutput;
struct Realisation;
struct Signature;
struct UnkeyedValidPathInfo;
enum struct BuildResultSuccessStatus : uint8_t;
enum struct BuildResultFailureStatus : uint8_t;

/**
 * Shared serializers between the worker protocol, serve protocol, and a
 * few others.
 *
 * This `struct` is basically just a `namespace`; We use a type rather
 * than a namespace just so we can use it as a template argument.
 */
struct CommonProto
{
    /**
     * A unidirectional read connection, to be used by the read half of the
     * canonical serializers below.
     */
    struct ReadConn
    {
        Source & from;
    };

    /**
     * A unidirectional write connection, to be used by the write half of the
     * canonical serializers below.
     */
    struct WriteConn
    {
        Sink & to;
    };

    template<typename T>
    struct Serialise;

    /**
     * Wrapper function around `CommonProto::Serialise<T>::write` that allows us to
     * infer the type instead of having to write it down explicitly.
     */
    template<typename T>
    static void write(const StoreDirConfig & store, WriteConn conn, const T & t)
    {
        CommonProto::Serialise<T>::write(store, conn, t);
    }

    /**
     * The hash slot of a path info, shared by the worker and serve
     * protocols.  In the object-hash form (worker `featureObjectHash`,
     * serve >= 2.9) it is two strings, the rendered object hash and the
     * NAR hash, either empty for absent; in the old form one string, the
     * NAR hash the sender asserts, empty for absent.  The NAR hash's text
     * differs between the protocols (and, in serve, between operations),
     * so its parse and render are the caller's.
     */
    static std::pair<std::optional<ObjectHash>, std::optional<Hash>> readPathInfoHashes(
        Source & from, bool objectHashForm, const std::function<Hash(const std::string &)> & parseNarHash);

    /**
     * In the old form the slot is `assertedNarHash`, else `lazyNarHash`
     * forced here -- the one place a peer's version costs anything.
     *
     * @throws Error in the old form when the info has neither.
     */
    static void writePathInfoHashes(
        Sink & to,
        bool objectHashForm,
        const UnkeyedValidPathInfo & info,
        const std::function<std::string(const Hash &)> & renderNarHash);
};

#define DECLARE_COMMON_SERIALISER(T)                                                                 \
    struct CommonProto::Serialise<T>                                                                 \
    {                                                                                                \
        static T read(const StoreDirConfig & store, CommonProto::ReadConn conn);                     \
        static void write(const StoreDirConfig & store, CommonProto::WriteConn conn, const T & str); \
    }

template<>
DECLARE_COMMON_SERIALISER(std::string);
template<>
DECLARE_COMMON_SERIALISER(StorePath);
template<>
DECLARE_COMMON_SERIALISER(ContentAddress);
template<>
DECLARE_COMMON_SERIALISER(DrvOutput);
template<>
DECLARE_COMMON_SERIALISER(Realisation);
template<>
DECLARE_COMMON_SERIALISER(Signature);

#define COMMA_ ,
template<typename T>
DECLARE_COMMON_SERIALISER(std::vector<T>);
template<typename T, typename Compare>
DECLARE_COMMON_SERIALISER(std::set<T COMMA_ Compare>);
template<typename... Ts>
DECLARE_COMMON_SERIALISER(std::tuple<Ts...>);

template<typename K, typename V, typename Compare>
DECLARE_COMMON_SERIALISER(std::map<K COMMA_ V COMMA_ Compare>);
#undef COMMA_

/**
 * These use the empty string for the null case, relying on the fact
 * that the underlying types never serialize to the empty string.
 *
 * We do this instead of a generic std::optional<T> instance because
 * ordinal tags (0 or 1, here) are a bit of a compatibility hazard. For
 * the same reason, we don't have a std::variant<T..> instances (ordinal
 * tags 0...n).
 *
 * We could the generic instances and then these as specializations for
 * compatibility, but that's proven a bit finnicky, and also makes the
 * worker protocol harder to implement in other languages where such
 * specializations may not be allowed.
 */
template<>
DECLARE_COMMON_SERIALISER(std::optional<StorePath>);
template<>
DECLARE_COMMON_SERIALISER(std::optional<ContentAddress>);

/**
 * The success and failure codes never overlay in enum tag values in the wire formats
 */
using BuildResultStatus = std::variant<BuildResultSuccessStatus, BuildResultFailureStatus>;

template<>
DECLARE_COMMON_SERIALISER(BuildResultStatus);
template<>
DECLARE_COMMON_SERIALISER(Verbosity);

#undef COMMA_

} // namespace nix
