#pragma once
///@file

#include "nix/store/path.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/util/serialise.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/std-hash.hh"
#include "nix/util/singleton/dispatch.hh"
#include "nix/util/tagged.hh"
#include "nix/expr/eval-trace/eval-trace-hash.hh"
#include "nix/expr/eval-trace/hash-spec.hh"
#include "nix/expr/eval-trace/ids.hh"
#include "nix/util/util.hh"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace nix {

struct Value;
struct SourcePath;

// ── CanonicalQueryKind ──────────────────────────────────────────────
//
// The primary dep identity. Each variant maps 1:1 to a QueryDescriptor
// via makeQueryDescriptor(). Adding a new variant without updating the
// switch triggers -Wswitch.

enum class CanonicalQueryKind : uint8_t {
    FileBytes = 0,
    DirectoryEntries,
    ExistenceCheck,
    EnvironmentLookup,
    SessionSystemValue,
    RuntimeFetchIdentity,
    DerivedStorePath,
    VolatileExec,
    NarIdentity,
    StructuredProjection,
    ImplicitStructure,
    RawBytes,
    StorePathAvailability,
    GitRevisionIdentity,
    TraceValueContext,
    TraceParentSlot,
    VolatileTime,
};

// ── QueryBehavior ───────────────────────────────────────────────────

enum class QueryBehavior : uint8_t {
    Normal = 0,
    Volatile = 1,
    ContentOverrideable = 2,
    Structural = 3,
    TraceContext = 4,
    ImplicitStructural = 5,
};

// ── QueryDomain ─────────────────────────────────────────────────────

enum class QueryDomain : uint32_t {
    Data = 1u << 0,
    Structure = 1u << 1,
    Metadata = 1u << 2,
    Locator = 1u << 3,
    Identity = 1u << 4,
    Availability = 1u << 5,
    Resolution = 1u << 6,
};

using QueryDomainMask = uint32_t;

inline constexpr QueryDomainMask operator|(QueryDomain lhs, QueryDomain rhs)
{
    return std::to_underlying(lhs) | std::to_underlying(rhs);
}

inline constexpr QueryDomainMask operator|(QueryDomainMask lhs, QueryDomain rhs)
{
    return lhs | std::to_underlying(rhs);
}

inline constexpr QueryDomainMask queryDomainMaskOf(QueryDomain domain)
{
    return std::to_underlying(domain);
}

inline constexpr bool queryDomainContains(QueryDomainMask mask, QueryDomain domain)
{
    return (mask & std::to_underlying(domain)) != 0;
}

// ── RepoRootAddressingKind ──────────────────────────────────────────
//
// How a dep's canonical query addresses filesystem paths for repo-root
// coverage and GitIdentity-based recovery.
//
// - DirectPath: the dep key's primary path is stored directly in keyId.
// - StructuredPath: the dep key addresses a structured file path or dir-set.
// - None: the dep is not repo-root addressable.

enum class RepoRootAddressingKind : uint8_t {
    None = 0,
    DirectPath,
    StructuredPath,
};

// ── QueryDescriptor ─────────────────────────────────────────────────

struct QueryDescriptor {
    QueryBehavior behavior;
    CanonicalQueryKind kind;
    QueryDomainMask observedDomains;
    bool isDigest;
    bool isOverrideable;
    bool isVolatile;
    const char * name;
};

/**
 * Build the descriptor for a single CanonicalQueryKind. Constexpr;
 * -Wswitch on the CQK switch ensures all variants are covered.
 */
inline constexpr QueryDescriptor makeQueryDescriptor(CanonicalQueryKind kind)
{
    switch (kind) {
    case CanonicalQueryKind::FileBytes:
        return {QueryBehavior::ContentOverrideable, kind,
            queryDomainMaskOf(QueryDomain::Data), true, true, false, "fileBytes"};
    case CanonicalQueryKind::DirectoryEntries:
        return {QueryBehavior::ContentOverrideable, kind,
            queryDomainMaskOf(QueryDomain::Structure) | QueryDomain::Metadata, true, true, false, "directoryEntries"};
    case CanonicalQueryKind::ExistenceCheck:
        return {QueryBehavior::Normal, kind,
            queryDomainMaskOf(QueryDomain::Metadata), false, false, false, "existenceCheck"};
    case CanonicalQueryKind::EnvironmentLookup:
        return {QueryBehavior::Normal, kind,
            queryDomainMaskOf(QueryDomain::Resolution), true, false, false, "environmentLookup"};
    case CanonicalQueryKind::VolatileTime:
        return {QueryBehavior::Volatile, kind,
            queryDomainMaskOf(QueryDomain::Resolution), false, false, true, "volatileTime"};
    case CanonicalQueryKind::SessionSystemValue:
        return {QueryBehavior::Normal, kind,
            queryDomainMaskOf(QueryDomain::Resolution), true, false, false, "sessionSystemValue"};
    case CanonicalQueryKind::RuntimeFetchIdentity:
        return {QueryBehavior::Normal, kind,
            queryDomainMaskOf(QueryDomain::Availability) | QueryDomain::Resolution, false, false, false, "runtimeFetchIdentity"};
    case CanonicalQueryKind::TraceValueContext:
        return {QueryBehavior::TraceContext, kind,
            queryDomainMaskOf(QueryDomain::Identity), true, false, false, "traceValueContext"};
    case CanonicalQueryKind::TraceParentSlot:
        return {QueryBehavior::TraceContext, kind,
            queryDomainMaskOf(QueryDomain::Identity), true, false, false, "traceParentSlot"};
    case CanonicalQueryKind::DerivedStorePath:
        return {QueryBehavior::Normal, kind,
            queryDomainMaskOf(QueryDomain::Data) | QueryDomain::Metadata | QueryDomain::Locator, false, false, false, "derivedStorePath"};
    case CanonicalQueryKind::VolatileExec:
        return {QueryBehavior::Volatile, kind,
            queryDomainMaskOf(QueryDomain::Resolution), false, false, true, "volatileExec"};
    case CanonicalQueryKind::NarIdentity:
        return {QueryBehavior::Normal, kind,
            queryDomainMaskOf(QueryDomain::Data) | QueryDomain::Metadata, true, false, false, "narIdentity"};
    case CanonicalQueryKind::StructuredProjection:
        return {QueryBehavior::Structural, kind,
            queryDomainMaskOf(QueryDomain::Data) | QueryDomain::Structure, true, false, false, "structuredProjection"};
    case CanonicalQueryKind::ImplicitStructure:
        return {QueryBehavior::ImplicitStructural, kind,
            queryDomainMaskOf(QueryDomain::Structure), true, false, false, "implicitStructure"};
    case CanonicalQueryKind::RawBytes:
        return {QueryBehavior::Normal, kind,
            queryDomainMaskOf(QueryDomain::Data), true, false, false, "rawBytes"};
    case CanonicalQueryKind::StorePathAvailability:
        return {QueryBehavior::Normal, kind,
            queryDomainMaskOf(QueryDomain::Availability), false, false, false, "storePathAvailability"};
    case CanonicalQueryKind::GitRevisionIdentity:
        return {QueryBehavior::ImplicitStructural, kind,
            queryDomainMaskOf(QueryDomain::Identity), true, false, false, "gitRevisionIdentity"};
    }
    unreachable();
}

/// Look up the descriptor for a CanonicalQueryKind. O(1) switch dispatch.
inline constexpr QueryDescriptor describe(CanonicalQueryKind kind)
{
    return makeQueryDescriptor(kind);
}

/**
 * Human-readable name for a CanonicalQueryKind.
 */
inline constexpr std::string_view queryKindName(CanonicalQueryKind kind)
{
    return describe(kind).name;
}

/**
 * Returns true if the query kind is volatile (always fails verification).
 */
inline constexpr bool isVolatile(CanonicalQueryKind kind)
{
    return describe(kind).isVolatile;
}

/**
 * Returns true if the query kind is a coarse content/directory dep
 * that can be overridden by fine-grained StructuredProjection deps.
 */
inline constexpr bool isContentOverrideable(CanonicalQueryKind kind)
{
    return describe(kind).isOverrideable;
}

/**
 * Returns true if the query kind stores an eval-trace digest (not a string).
 */
inline constexpr bool isDigestDep(CanonicalQueryKind kind)
{
    return describe(kind).isDigest;
}

/**
 * Returns true if the query kind is a trace context dep.
 */
inline constexpr bool isTraceContext(CanonicalQueryKind kind)
{
    return describe(kind).behavior == QueryBehavior::TraceContext;
}

/**
 * Get the query behavior for a query kind.
 */
inline constexpr QueryBehavior queryBehavior(CanonicalQueryKind kind)
{
    return describe(kind).behavior;
}

/**
 * Returns true when this dep kind contributes material to trace_hash and
 * struct_hash. ImplicitStructural deps are recorded for verification/recovery
 * guards, but canonical trace hashes intentionally exclude them.
 */
inline constexpr bool contributesToTraceHash(CanonicalQueryKind kind)
{
    return queryBehavior(kind) != QueryBehavior::ImplicitStructural;
}

/**
 * Human-readable name for a QueryBehavior.
 */
inline constexpr std::string_view queryBehaviorName(QueryBehavior behavior)
{
    switch (behavior) {
    case QueryBehavior::Normal: return "normal";
    case QueryBehavior::Volatile: return "volatile";
    case QueryBehavior::ContentOverrideable: return "contentOverrideable";
    case QueryBehavior::Structural: return "structural";
    case QueryBehavior::TraceContext: return "traceContext";
    case QueryBehavior::ImplicitStructural: return "implicitStructural";
    }
    unreachable();
}

/**
 * Returns true if the query kind is a structured dep type (StructuredProjection or ImplicitStructure).
 */
inline constexpr bool isStructuredQueryKind(CanonicalQueryKind kind)
{
    return kind == CanonicalQueryKind::StructuredProjection || kind == CanonicalQueryKind::ImplicitStructure;
}

/**
 * Returns true if the query kind is a trace context dep type.
 */
inline constexpr bool isTraceContextQueryKind(CanonicalQueryKind kind)
{
    return isTraceContext(kind);
}

inline constexpr RepoRootAddressingKind repoRootAddressingKind(CanonicalQueryKind kind)
{
    switch (kind) {
    case CanonicalQueryKind::FileBytes:
    case CanonicalQueryKind::DirectoryEntries:
    case CanonicalQueryKind::ExistenceCheck:
    case CanonicalQueryKind::NarIdentity:
    case CanonicalQueryKind::RawBytes:
        return RepoRootAddressingKind::DirectPath;
    case CanonicalQueryKind::StructuredProjection:
    case CanonicalQueryKind::ImplicitStructure:
        return RepoRootAddressingKind::StructuredPath;
    case CanonicalQueryKind::EnvironmentLookup:
    case CanonicalQueryKind::SessionSystemValue:
    case CanonicalQueryKind::RuntimeFetchIdentity:
    case CanonicalQueryKind::DerivedStorePath:
    case CanonicalQueryKind::VolatileExec:
    case CanonicalQueryKind::StorePathAvailability:
    case CanonicalQueryKind::GitRevisionIdentity:
    case CanonicalQueryKind::TraceValueContext:
    case CanonicalQueryKind::TraceParentSlot:
    case CanonicalQueryKind::VolatileTime:
        return RepoRootAddressingKind::None;
    }
    unreachable();
}

// ── CQK-based lattice predicates ────────────────────────────────────

/**
 * True when this dep's identity domain is covered by any session fingerprint.
 * SessionParam-equivalent deps (GitRevisionIdentity, SessionSystemValue,
 * DerivedStorePath) never need per-dep verification when a fingerprint is present.
 */
inline constexpr bool isCoveredBySessionFingerprint(CanonicalQueryKind kind)
{
    // TraceContext deps (TraceValueContext, TraceParentSlot) observe the Identity
    // domain but depend on upstream traces which may have non-file deps -- they
    // must NOT be covered by session fingerprint.
    if (kind == CanonicalQueryKind::TraceValueContext
        || kind == CanonicalQueryKind::TraceParentSlot)
        return false;
    return queryDomainContains(describe(kind).observedDomains, QueryDomain::Identity)
        || kind == CanonicalQueryKind::SessionSystemValue
        || kind == CanonicalQueryKind::DerivedStorePath
        || kind == CanonicalQueryKind::GitRevisionIdentity;
}

/**
 * True when this dep MAY be covered by a session fingerprint, depending
 * on whether the dep's source path is under a covered root.
 */
inline constexpr bool isFileContentDep(CanonicalQueryKind kind)
{
    switch (kind) {
    case CanonicalQueryKind::FileBytes:
    case CanonicalQueryKind::DirectoryEntries:
    case CanonicalQueryKind::ExistenceCheck:
    case CanonicalQueryKind::RawBytes:
    case CanonicalQueryKind::NarIdentity:
    case CanonicalQueryKind::StructuredProjection:
    case CanonicalQueryKind::ImplicitStructure:
    case CanonicalQueryKind::DerivedStorePath:
        return true;
    case CanonicalQueryKind::EnvironmentLookup:
    case CanonicalQueryKind::VolatileTime:
    case CanonicalQueryKind::SessionSystemValue:
    case CanonicalQueryKind::RuntimeFetchIdentity:
    case CanonicalQueryKind::VolatileExec:
    case CanonicalQueryKind::StorePathAvailability:
    case CanonicalQueryKind::GitRevisionIdentity:
    case CanonicalQueryKind::TraceValueContext:
    case CanonicalQueryKind::TraceParentSlot:
        return false;
    }
    unreachable();
}

// EvalTraceHash, EvalTraceHasher, and the phantom-typed hash aliases
// (DepHash, TraceHash, StructHash, ResultHash, FullTraceHash,
// DepKeySetHash, StoredGitIdentityHash, CurrentGitIdentityHash) are
// defined in `eval-trace/eval-trace-hash.hh`, included above.
// Lightweight consumers (session-identity.hh) can include that header
// without pulling the rest of deps/types.hh.

/**
 * A dep's expected hash value: either a fixed-size eval-trace dep hash
 * (for content, directory, NAR, envvar, system, and parent context deps)
 * or a variable-length
 * string (for existence checks like "type:1"/"missing", and store paths for
 * CopiedPath/UnhashedFetch).
 */
using DepHashValue = std::variant<DepHash, std::string>;

/** Get (data, size) for SQLite BLOB binding. */
inline std::pair<const unsigned char *, size_t> blobData(const DepHashValue & v) {
    return std::visit(overloaded{
        [](const DepHash & h) -> std::pair<const unsigned char *, size_t> {
            return {h.value.data(), h.value.size()};
        },
        [](const std::string & s) -> std::pair<const unsigned char *, size_t> {
            return {reinterpret_cast<const unsigned char *>(s.data()), s.size()};
        },
    }, v);
}

/// Hash provenance: how a dep hash was obtained. Each provenance is a
/// singleton::Tag used as a phantom type on Tagged<Tag, DepHashValue>.
/// VerificationSession exposes two typed write methods: cacheComputedHash
/// (any caller) and cacheVerifiedHash (gated by the VerifiedSubsumption
/// capability).  L1 stores only the DepHashValue; provenance is type-erased
/// at read time because the L1 invariant makes every entry safe to return.
enum class HashProvenance : uint8_t {
    /// Freshly computed from current filesystem state.
    Computed,
    /// Returned via trace-scoped subsumption (this trace's file Content dep
    /// verified unchanged). The stored hash IS the current hash — sound
    /// because the trace id is carried by CurrentTraceDep, and subsumption
    /// writes are gated by VerifiedSubsumption (CurrentTrace origin only).
    Verified,
};

/// Phantom-typed dep hash with provenance tracked at the type level.
/// resolveDepHash creates ComputedHash on its compute path and
/// VerifiedHash on its subsumption path. Both can enter the L1 cache.
template<HashProvenance HP>
using ProvenancedHash = Tagged<singleton::Tag<HP>, DepHashValue>;

using ComputedHash = ProvenancedHash<HashProvenance::Computed>;
using VerifiedHash = ProvenancedHash<HashProvenance::Verified>;

/**
 * Format tag for StructuredContent deps identifying the data source type.
 * Persisted as the single-byte format field in the structured dep key payload.
 */
enum class StructuredFormat : char {
    Json      = 'j',
    Toml      = 't',
    Directory = 'd',
    Nix       = 'n',
};

/**
 * Convert a StructuredFormat to its wire-format character.
 */
inline constexpr char structuredFormatChar(StructuredFormat f)
{
    return static_cast<char>(f);
}

/**
 * Parse a wire-format character to a StructuredFormat, or nullopt if invalid.
 */
inline constexpr std::optional<StructuredFormat> parseStructuredFormat(char c)
{
    switch (c) {
    case 'j': return StructuredFormat::Json;
    case 't': return StructuredFormat::Toml;
    case 'd': return StructuredFormat::Directory;
    case 'n': return StructuredFormat::Nix;
    default: return std::nullopt;
    }
}

/**
 * Human-readable name for a StructuredFormat (for diagnostics/logging).
 */
inline constexpr std::string_view structuredFormatName(StructuredFormat f)
{
    switch (f) {
    case StructuredFormat::Json: return "json";
    case StructuredFormat::Toml: return "toml";
    case StructuredFormat::Directory: return "directory";
    case StructuredFormat::Nix: return "nix";
    }
    unreachable();
}

/**
 * Shape suffix for StructuredContent deps on containers.
 * Appended to the data path in dep keys.
 */
enum class ShapeSuffix : uint8_t {
    None = 0,  ///< Scalar leaf access — no suffix
    Len  = 1,  ///< List/array length (#len)
    Keys = 2,  ///< Object/attrset key set (#keys)
    Type = 3,  ///< Container type — "object" or "array" (#type)
};

/**
 * Display name for a ShapeSuffix (for diagnostics).
 * Returns "", "len", "keys", or "type".
 */
inline constexpr std::string_view shapeSuffixName(ShapeSuffix s)
{
    switch (s) {
    case ShapeSuffix::None: return "";
    case ShapeSuffix::Len: return "len";
    case ShapeSuffix::Keys: return "keys";
    case ShapeSuffix::Type: return "type";
    }
    unreachable();
}

// ── DepSourceKind ───────────────────────────────────────────────────

enum class DepSourceKind : uint8_t {
    Absolute = 0,
    Registered = 1,
};

/**
 * Sentinel inputName for deps on absolute filesystem paths that are outside
 * any flake input tree. Validated directly against the real filesystem,
 * not through any input accessor.
 */
inline constexpr std::string_view absolutePathDep = "<absolute>";

using RuntimeRootSourceKey = Tagged<struct RuntimeRootSourceKeyTag_, EvalTraceHash>;
using RuntimeRootNarHash = Tagged<struct RuntimeRootNarHashTag_, Hash>;
using RuntimeRootStorePath = Tagged<struct RuntimeRootStorePathTag_, StorePath>;
using GraphNodeDepSourceKey = Tagged<struct GraphNodeDepSourceKeyTag_, std::string>;
using GitRepoRoot = Tagged<struct GitRepoRootTag_, CanonPath>;
using RegistryMountSubdir = Tagged<struct RegistryMountSubdirTag_, CanonPath>;

inline RuntimeRootSourceKey runtimeRootSourceKeyFromDebugString(std::string_view value)
{
    HashSink sink{eval_trace::toHashAlgorithm(eval_trace::getEvalTraceHashAlgorithm())};
    sink(value);
    return RuntimeRootSourceKey{EvalTraceHash::fromHash(sink.finish().hash)};
}

struct AbsoluteDepSource {
    bool operator==(const AbsoluteDepSource &) const = default;
};

struct DepSource {
    std::variant<AbsoluteDepSource, GraphNodeDepSourceKey, RuntimeRootSourceKey> value;

    static DepSource makeAbsolute()
    {
        return DepSource{AbsoluteDepSource{}};
    }

    /// Create a registered dep source from a ResolvedFlakeGraph node key.
    ///
    /// This is the primary factory for flake input sources. The node key
    /// is the lock-file node name (e.g., "root", "nixpkgs", "sub0") from
    /// LockFile::toJSON(). The SemanticRegistry's forward entries and
    /// mount points are both keyed by node keys, so deps recorded with
    /// this identity are always resolvable during verification.
    static DepSource fromNodeKey(GraphNodeDepSourceKey nodeKey)
    {
        return DepSource{std::move(nodeKey)};
    }

    static DepSource fromNodeKey(std::string nodeKey)
    {
        return fromNodeKey(GraphNodeDepSourceKey{std::move(nodeKey)});
    }

    /// Create a registered dep source from a canonical runtime-root identity.
    ///
    /// Used for runtime-fetched inputs (builtins.fetchTree) that are NOT
    /// part of the flake's ResolvedFlakeGraph. These sources are resolved
    /// by entries added from SessionRuntimeRoots at session open, not by
    /// the graph-derived registry entries.
    static DepSource fromRuntimeRoot(const RuntimeRootSourceKey & sourceKey)
    {
        return DepSource{sourceKey};
    }

    DepSourceKind kind() const noexcept
    {
        return std::visit([](const auto & source) -> DepSourceKind {
            using T = std::decay_t<decltype(source)>;
            if constexpr (std::same_as<T, AbsoluteDepSource>)
                return DepSourceKind::Absolute;
            else
                return DepSourceKind::Registered;
        }, value);
    }

    const GraphNodeDepSourceKey * graphNodeKey() const noexcept
    {
        return std::get_if<GraphNodeDepSourceKey>(&value);
    }

    const RuntimeRootSourceKey * runtimeRootKey() const noexcept
    {
        return std::get_if<RuntimeRootSourceKey>(&value);
    }

    bool operator==(const DepSource &) const = default;

    struct Hash {
        size_t operator()(const DepSource & source) const noexcept
        {
            return std::visit([](const auto & value) -> size_t {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<T, AbsoluteDepSource>)
                    return hashValues(uint8_t{0});
                else if constexpr (std::same_as<T, GraphNodeDepSourceKey>)
                    return hashValues(uint8_t{1}, value.value);
                else
                    return hashValues(uint8_t{2}, EvalTraceHash::Hasher{}(value.value));
            }, source.value);
        }
    };
};

using EncodedDepSourceBlob = Tagged<struct EncodedDepSourceBlobTag_, std::string>;

inline std::optional<DepSource> parseDepSource(std::string_view source)
{
    if (source == absolutePathDep)
        return DepSource::makeAbsolute();
    if (source.starts_with("node:"))
        return DepSource{GraphNodeDepSourceKey{std::string(source.substr(5))}};
    if (source.starts_with("runtime:")) {
        try {
            auto payload = source.substr(8);
            auto algorithm = eval_trace::getEvalTraceHashAlgorithm();
            auto separator = payload.find(':');
            if (separator != std::string_view::npos) {
                auto algorithmName = payload.substr(0, separator);
                if (algorithmName == "blake3")
                    algorithm = eval_trace::EvalTraceHashAlgorithm::Blake3;
                else if (algorithmName == "sha256")
                    algorithm = eval_trace::EvalTraceHashAlgorithm::Sha256;
                else
                    return std::nullopt;
                payload.remove_prefix(separator + 1);
            }
            auto hash = Hash::parseNonSRIUnprefixed(
                payload,
                eval_trace::toHashAlgorithm(algorithm));
            return DepSource::fromRuntimeRoot(
                RuntimeRootSourceKey{EvalTraceHash::fromHash(hash)});
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

inline std::string serializeDepSource(const DepSource & source)
{
    return std::visit([](const auto & value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<T, AbsoluteDepSource>)
            return std::string(absolutePathDep);
        else if constexpr (std::same_as<T, GraphNodeDepSourceKey>)
            return "node:" + value.value;
        else
            return "runtime:"
                + std::string(eval_trace::evalTraceHashAlgorithmSlug(
                    eval_trace::getEvalTraceHashAlgorithm()))
                + ":" + value.value.toHex();
    }, source.value);
}

inline EncodedDepSourceBlob encodeDepSourceBlob(const DepSource & source)
{
    std::string encoded("dsrc2", 5);
    std::visit([&](const auto & value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<T, AbsoluteDepSource>) {
            encoded.push_back('\x00');
        } else if constexpr (std::same_as<T, GraphNodeDepSourceKey>) {
            encoded.push_back('\x01');
            encoded.append(value.value);
        } else {
            encoded.push_back('\x02');
            encoded.push_back(eval_trace::evalTraceHashAlgorithmTag(
                eval_trace::getEvalTraceHashAlgorithm()));
            encoded.append(value.value.view());
        }
    }, source.value);
    return EncodedDepSourceBlob{std::move(encoded)};
}

inline std::optional<DepSource> decodeDepSourceBlob(std::string_view encoded)
{
    // The current eval-trace schema (see kSchemaEpoch in trace-store.hh)
    // persists runtime-root source digests with an explicit hash algorithm
    // tag. Rejecting non-prefixed rows here keeps legacy string compatibility
    // at explicit JSON/debug surfaces only, not in persistence.
    if (!encoded.starts_with("dsrc2"))
        return std::nullopt;

    encoded.remove_prefix(5);
    if (encoded.empty())
        return std::nullopt;

    auto tag = encoded.front();
    encoded.remove_prefix(1);
    switch (tag) {
    case '\x00':
        if (!encoded.empty())
            return std::nullopt;
        return DepSource::makeAbsolute();
    case '\x01':
        return DepSource{GraphNodeDepSourceKey{std::string(encoded)}};
    case '\x02': {
        if (encoded.empty())
            return std::nullopt;
        eval_trace::EvalTraceHashAlgorithm algorithm;
        try {
            algorithm = eval_trace::parseEvalTraceHashAlgorithmTag(encoded.front());
        } catch (...) {
            return std::nullopt;
        }
        encoded.remove_prefix(1);
        if (algorithm != eval_trace::getEvalTraceHashAlgorithm())
            return std::nullopt;
        if (encoded.size() != eval_trace::kEvalTraceDigestSize)
            return std::nullopt;
        return DepSource::fromRuntimeRoot(
            RuntimeRootSourceKey{EvalTraceHash::fromBlob(encoded.data(), encoded.size())});
    }
    default:
        return std::nullopt;
    }
}

inline constexpr bool isAbsoluteDepSource(const DepSource & source)
{
    return std::holds_alternative<AbsoluteDepSource>(source.value);
}

/**
 * Typed component of a structured data path.
 *
 * Object-key access stores the key in `key` with `index == -1`.
 * Array-index access stores the index in `index` with an empty `key`.
 */
struct StructuredPathComponent {
    std::string key;
    int32_t index = -1;

    static StructuredPathComponent makeKey(std::string key)
    {
        return StructuredPathComponent{
            .key = std::move(key),
            .index = -1,
        };
    }

    static StructuredPathComponent makeIndex(int32_t index)
    {
        return StructuredPathComponent{
            .key = "",
            .index = index,
        };
    }

    bool isIndex() const { return index >= 0; }
    bool isKey() const { return index < 0; }

    bool operator==(const StructuredPathComponent &) const = default;
};

using StructuredPath = std::vector<StructuredPathComponent>;

inline std::string displayStructuredPath(const StructuredPath & path)
{
    std::string result = "$";
    for (auto & component : path) {
        if (component.isIndex()) {
            result += "[";
            result += std::to_string(component.index);
            result += "]";
            continue;
        }

        result += "[\"";
        result += component.key;
        result += "\"]";
    }
    return result;
}

/**
 * A single interned dependency: stores StringInternTable indices instead of
 * owned strings. DepSourceId and DepKeyId share the same index space as
 * StringId — all three are uint32_t indices into InterningPools::strings.
 * Zero per-dep heap allocation; string data lives in the arena.
 * Resolve dep sources via pools.resolveDepSource(key.sourceId) and other
 * strings via pools.resolve(...).
 *
 * For file deps (FileBytes/DirectoryEntries/ExistenceCheck), simpleKeyId()
 * resolves to:
 *   - relative path for flake/runtime dep sources
 *   - absolute path for absolute dep sources
 *
 * For non-file deps, the typed dep-key accessor resolves to a descriptive
 * identifier:
 *   - EnvironmentLookup: the environment variable name (e.g., "HOME")
 *   - VolatileTime: "currentTime"
 *   - SessionSystemValue: "currentSystem"
 *   - RuntimeFetchIdentity: canonical fetcher attrs
 */
struct Dep {
    struct Key {
    private:
        struct InitTag {};

        explicit Key(InitTag, CanonicalQueryKind kind)
            : kind(kind)
        {
        }

        TypedDepKeyId typedKeyId_{SimpleDepKeyId{}};

    public:
        Key() = delete;

        CanonicalQueryKind kind;
        DepSourceId sourceId{};     ///< Simple/structured dep source.
        FilePathId filePathId{};    ///< Structured dep file path atom.
        DataPathId dataPathId{};    ///< Structured dep data-path trie node.
        StringId hasKeyId{};        ///< Structured has-key query atom.
        StringId dirSetHashId{};    ///< Aggregated DirSet hash atom.
        AttrPathId attrPathId{};    ///< Trace-context path payload.
        /// The git repo root this dep's file lives under, interned at
        /// recording time. Only meaningful for file-content deps
        /// (repoRootAddressingKind != None). Value 0 means "no governing
        /// repo" (file is outside any git repo, or dep kind is not
        /// repo-addressable). The verifier uses this to restrict the
        /// git-identity skip optimization to deps whose OWN governing
        /// repo had a matching GitRevisionIdentity in the same trace,
        /// preventing cross-trace poisoning of verifiedGitRepos_.
        RepoRootId governingRepoId{};
        ShapeSuffix suffix = ShapeSuffix::None;
        uint8_t format = 0;

        static Key makeSimple(CanonicalQueryKind kind, DepSourceId sourceId, SimpleDepKeyId keyId)
        {
            assert(kind != CanonicalQueryKind::DerivedStorePath);
            assert(kind != CanonicalQueryKind::StorePathAvailability);
            assert(kind != CanonicalQueryKind::RuntimeFetchIdentity);
            assert(!isStructuredQueryKind(kind) && !isTraceContextQueryKind(kind));
            Key key(InitTag{}, kind);
            key.sourceId = sourceId;
            key.typedKeyId_ = keyId;
            return key;
        }

        static Key makeDerivedStorePath(DepSourceId sourceId, DerivedStorePathDepKeyId keyId)
        {
            Key key(InitTag{}, CanonicalQueryKind::DerivedStorePath);
            key.sourceId = sourceId;
            key.typedKeyId_ = keyId;
            return key;
        }

        static Key makeStorePathAvailability(DepSourceId sourceId, StorePathAvailabilityDepKeyId keyId)
        {
            Key key(InitTag{}, CanonicalQueryKind::StorePathAvailability);
            key.sourceId = sourceId;
            key.typedKeyId_ = keyId;
            return key;
        }

        static Key makeRuntimeFetchIdentity(DepSourceId sourceId, RuntimeFetchIdentityDepKeyId keyId)
        {
            Key key(InitTag{}, CanonicalQueryKind::RuntimeFetchIdentity);
            key.sourceId = sourceId;
            key.typedKeyId_ = keyId;
            return key;
        }

        static Key makeStructured(
            CanonicalQueryKind kind,
            DepSourceId sourceId,
            FilePathId filePathId,
            StructuredFormat format,
            DataPathId dataPathId,
            ShapeSuffix suffix = ShapeSuffix::None,
            StringId hasKeyId = StringId(),
            StringId dirSetHashId = StringId())
        {
            assert(isStructuredQueryKind(kind));
            Key key(InitTag{}, kind);
            key.sourceId = sourceId;
            key.filePathId = filePathId;
            key.dataPathId = dataPathId;
            key.hasKeyId = hasKeyId;
            key.dirSetHashId = dirSetHashId;
            key.suffix = suffix;
            key.format = static_cast<uint8_t>(structuredFormatChar(format));
            return key;
        }

        static Key makeTraceContext(
            CanonicalQueryKind kind,
            AttrPathId attrPathId)
        {
            assert(isTraceContextQueryKind(kind));
            Key key(InitTag{}, kind);
            key.attrPathId = attrPathId;
            return key;
        }

        bool isSimple() const { return !isStructuredQueryKind(kind) && !isTraceContextQueryKind(kind); }
        bool isStructured() const { return isStructuredQueryKind(kind); }
        bool isTraceContext() const { return isTraceContextQueryKind(kind); }

        StructuredFormat structuredFormat() const
        {
            assert(isStructured());
            auto parsed = parseStructuredFormat(static_cast<char>(format));
            assert(parsed && "invalid structured dep format");
            return *parsed;
        }

        SimpleDepKeyId simpleKeyId() const
        {
            assert(isSimple());
            assert(kind != CanonicalQueryKind::DerivedStorePath);
            assert(kind != CanonicalQueryKind::StorePathAvailability);
            assert(kind != CanonicalQueryKind::RuntimeFetchIdentity);
            return std::get<SimpleDepKeyId>(typedKeyId_);
        }

        DepKeyId depKeyId() const
        {
            assert(!isStructured());
            assert(!isTraceContext());
            return eraseDepKeyType(typedKeyId_);
        }

        DerivedStorePathDepKeyId derivedStorePathKeyId() const
        {
            assert(kind == CanonicalQueryKind::DerivedStorePath);
            return std::get<DerivedStorePathDepKeyId>(typedKeyId_);
        }

        StorePathAvailabilityDepKeyId storePathAvailabilityKeyId() const
        {
            assert(kind == CanonicalQueryKind::StorePathAvailability);
            return std::get<StorePathAvailabilityDepKeyId>(typedKeyId_);
        }

        RuntimeFetchIdentityDepKeyId runtimeFetchIdentityKeyId() const
        {
            assert(kind == CanonicalQueryKind::RuntimeFetchIdentity);
            return std::get<RuntimeFetchIdentityDepKeyId>(typedKeyId_);
        }

        TypedDepKeyId typedKeyId() const
        {
            assert(isSimple());
            return typedKeyId_;
        }

        // NOTE: `governingRepoId` is deliberately EXCLUDED from identity.
        // It is a verification hint set at recording time, not part of the
        // logical "what was observed" identity.  Including it would desync
        // from `feedCanonicalDepKeyMaterial` (which correctly excludes it),
        // would cause dedup in `seenDeps` to miss duplicate deps when
        // filesystem state flickers between two recordings of the same
        // file, and would make two deps that represent the same
        // observation hash differently.  Keep identity based on kind +
        // source + key (what was queried), not recording-side metadata.
        auto operator<=>(const Key & other) const
        {
            auto lhsKind = isStructured() ? 1u : (isTraceContext() ? 2u : 0u);
            auto rhsKind = other.isStructured() ? 1u : (other.isTraceContext() ? 2u : 0u);
            if (auto cmp = lhsKind <=> rhsKind; cmp != 0)
                return cmp;
            if (auto cmp = kind <=> other.kind; cmp != 0)
                return cmp;

            if (isStructured()) {
                return std::tie(sourceId, filePathId, dataPathId, hasKeyId, dirSetHashId, suffix, format)
                    <=> std::tie(other.sourceId, other.filePathId, other.dataPathId,
                                 other.hasKeyId, other.dirSetHashId, other.suffix, other.format);
            }
            if (isTraceContext())
                return attrPathId <=> other.attrPathId;
            return std::tuple{sourceId, eraseDepKeyType(typedKeyId_)}
                <=> std::tuple{other.sourceId, eraseDepKeyType(other.typedKeyId_)};
        }

        bool operator==(const Key & other) const
        {
            return (*this <=> other) == 0;
        }

        struct Hash {
            // No `is_avalanching` marker.  `hashValues` is
            // `hash_combine` over `std::hash<size_t>`; on libstdc++
            // `std::hash<size_t>` is identity, so the combine does
            // not avalanche.  Let Boost apply its post-mixer.
            std::size_t operator()(const Key & k) const noexcept {
                if (k.isStructured()) {
                    return hashValues(
                        std::to_underlying(k.kind),
                        k.sourceId.value,
                        k.filePathId.value,
                        k.dataPathId.value,
                        k.hasKeyId.value,
                        k.dirSetHashId.value,
                        std::to_underlying(k.suffix),
                        k.format);
                }
                if (k.isTraceContext())
                    return hashValues(std::to_underlying(k.kind), k.attrPathId.value);
                return hashValues(
                    std::to_underlying(k.kind),
                    k.sourceId.value,
                    eraseDepKeyType(k.typedKeyId_).value);
            }
        };
    };

    Key key;
    DepHashValue hash;

    auto operator<=>(const Dep & o) const { return key <=> o.key; }
    bool operator==(const Dep & o) const { return key == o.key; }

    /// Create a ValueContext dep.
    static Dep makeValueContext(AttrPathId pathId, DepHashValue hash) {
        return {Key::makeTraceContext(CanonicalQueryKind::TraceValueContext, pathId), std::move(hash)};
    }

    /// Create a ParentSlot dep.
    static Dep makeParentSlot(ParentSlot pathId, DepHashValue hash) {
        return {Key::makeTraceContext(CanonicalQueryKind::TraceParentSlot, pathId.value), std::move(hash)};
    }

    /// Extract the AttrPathId from a trace-context dep.
    AttrPathId traceContextPath() const {
        assert(key.isTraceContext());
        return key.attrPathId;
    }
};

/**
 * A half-open range [start, end) into the epoch log dep vector,
 * representing the deps recorded during a single thunk/app evaluation.
 */
struct DepRange {
    std::vector<Dep> * deps;
    uint32_t start;
    uint32_t end;
};

// ═══════════════════════════════════════════════════════════════════════
// Provenance — eval-trace origin data for PosTable positions
// ═══════════════════════════════════════════════════════════════════════

/**
 * Full provenance record for a position originating from traced data
 * (JSON, TOML, directory listings). Stored in a ProvenanceTable indexed
 * by the opaque Pos::ProvenanceRef::id.
 */
struct ProvenanceRecord {
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;
    StructuredFormat format;
};

/**
 * Flat table of provenance records. Append-only within a session.
 * Owned by InterningPools (Lifetime 1).
 */
struct ProvenanceTable {
    std::vector<ProvenanceRecord> records;

    uint32_t allocate(DepSourceId srcId, FilePathId fpId, DataPathId dpId, StructuredFormat fmt) {
        uint32_t id = static_cast<uint32_t>(records.size());
        records.push_back({srcId, fpId, dpId, fmt});
        return id;
    }

    const ProvenanceRecord & resolve(uint32_t id) const {
        assert(id < records.size());
        return records[id];
    }

    void clear() { records.clear(); }
};

} // namespace nix
