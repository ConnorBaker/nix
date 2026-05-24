#pragma once
///@file

#include "nix/util/error.hh"
#include "nix/util/serialise.hh"

#include <algorithm>
#include <string_view>
#include <variant>

namespace nix {

struct StoreDirConfig;
struct Source;

/**
 * Bounds-check `remoteVersion` against `localMin` and return
 * `min(remoteVersion, localLatest)`.
 *
 * Both `WorkerProto::BasicClientConnection::handshake` and
 * `ServeProto::BasicClientConnection::handshake` perform exactly this
 * lower-bound + take-min step. The version type is templated because
 * `WorkerProto` uses `Version::Number` (major/minor only, total order)
 * here while `ServeProto` uses its full `Version` (also major/minor
 * only).
 *
 * @param localMin the minimum `Version` this side supports.
 * @param localLatest the latest `Version` this side supports; used as
 * the upper bound returned.
 * @param remoteVersion the version reported by the remote peer.
 * @param tooOldMsg the error message if `remoteVersion < localMin`.
 * The protocols use different protocol-flavored phrasing so this is
 * passed in by the caller. Format-string substitution is the caller's
 * responsibility.
 *
 * Pre-checks unique to one protocol (e.g. WorkerProto's "major must
 * equal latest.major" check) stay at the caller, since their error
 * messages and conditions differ.
 */
template<class VersionT>
inline VersionT negotiateVersion(
    const VersionT & localMin,
    const VersionT & localLatest,
    const VersionT & remoteVersion,
    std::string_view tooOldMsg)
{
    if (remoteVersion < localMin)
        throw Error("%s", tooOldMsg);
    return std::min(remoteVersion, localLatest);
}

// items being serialized
class StorePath;
struct ContentAddress;
struct DrvOutput;
struct Realisation;
struct Signature;
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
};

/**
 * Declare a canonical serialiser pair for `Proto::Serialise<T>`.
 *
 * Used by the worker, serve, and common protocol headers via the
 * `DECLARE_PROTO_SERIALISER_COMMA` token to embed commas inside `T`.
 *
 * Some sort of `template<...>` must precede the invocation for the
 * struct specialisation to be legal C++ syntax.
 */
#define DECLARE_PROTO_SERIALISER(Proto, T)                                            \
    struct Proto::Serialise<T>                                                        \
    {                                                                                 \
        static T read(const StoreDirConfig & store, Proto::ReadConn conn);            \
        static void write(const StoreDirConfig & store, Proto::WriteConn conn, const T & str); \
    }

#define DECLARE_PROTO_SERIALISER_COMMA ,

template<>
DECLARE_PROTO_SERIALISER(CommonProto, std::string);
template<>
DECLARE_PROTO_SERIALISER(CommonProto, StorePath);
template<>
DECLARE_PROTO_SERIALISER(CommonProto, ContentAddress);
template<>
DECLARE_PROTO_SERIALISER(CommonProto, DrvOutput);
template<>
DECLARE_PROTO_SERIALISER(CommonProto, Realisation);
template<>
DECLARE_PROTO_SERIALISER(CommonProto, Signature);

template<typename T>
DECLARE_PROTO_SERIALISER(CommonProto, std::vector<T>);
template<typename T, typename Compare>
DECLARE_PROTO_SERIALISER(CommonProto, std::set<T DECLARE_PROTO_SERIALISER_COMMA Compare>);
template<typename... Ts>
DECLARE_PROTO_SERIALISER(CommonProto, std::tuple<Ts...>);

template<typename K, typename V, typename Compare>
DECLARE_PROTO_SERIALISER(
    CommonProto,
    std::map<K DECLARE_PROTO_SERIALISER_COMMA V DECLARE_PROTO_SERIALISER_COMMA Compare>);

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
DECLARE_PROTO_SERIALISER(CommonProto, std::optional<StorePath>);
template<>
DECLARE_PROTO_SERIALISER(CommonProto, std::optional<ContentAddress>);

/**
 * The success and failure codes never overlay in enum tag values in the wire formats
 */
using BuildResultStatus = std::variant<BuildResultSuccessStatus, BuildResultFailureStatus>;

template<>
DECLARE_PROTO_SERIALISER(CommonProto, BuildResultStatus);

} // namespace nix
