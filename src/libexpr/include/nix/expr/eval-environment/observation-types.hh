#pragma once

#include "nix/fetchers/fetchers.hh"
#include "nix/expr/eval-environment/domains.hh"
#include "nix/expr/eval-environment/request-types.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/semantic-objects.hh"
#include "nix/store/path.hh"
#include "nix/util/conditional-base.hh"
#include "nix/util/linear.hh"

#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nix {

struct EnvVarObservation
{
    std::string name;
    std::string value;
};

enum class LookupPathOrigin : uint8_t {
    Existing,
    Downloaded,
    HookResolved,
    Missing,
};

/// Fields present on all non-missing lookup path resolutions.
struct LookupPathResolvedRootFields {
    SourcePath resolvedRoot;
    std::optional<SourcePath> allowlistedPath;
    std::optional<StorePath> allowlistedClosureRoot;
};

/// Field present only on downloaded pseudo-URL resolutions.
struct LookupPathMaterializedField {
    StorePath materializedStorePath;
};

/// Parameterized lookup path resolution.
///
/// Fields are physically present or absent depending on the origin:
/// - Missing: only `identity`
/// - Existing, HookResolved: `identity` + resolved root fields
/// - Downloaded: `identity` + resolved root fields + materializedStorePath
///
/// Accessing a field that is absent for the origin is a compile error.
template <LookupPathOrigin O>
struct LookupPathResolution
    : ConditionalBase<O != LookupPathOrigin::Missing, LookupPathResolvedRootFields>
    , ConditionalBase<O == LookupPathOrigin::Downloaded, LookupPathMaterializedField>
{
    static_assert(
        O == LookupPathOrigin::Existing || O == LookupPathOrigin::Downloaded
        || O == LookupPathOrigin::HookResolved || O == LookupPathOrigin::Missing);

    RealizedLookupPathIdentity identity;
};

/// Type alias naming convention: <Tag><Template>.
/// The tag prefix matches the enum enumerator name.
/// Example: DownloadedLookupPathResolution = LookupPathResolution<LookupPathOrigin::Downloaded>

using ExistingLookupPathResolution = LookupPathResolution<LookupPathOrigin::Existing>;
using DownloadedLookupPathResolution = LookupPathResolution<LookupPathOrigin::Downloaded>;
using HookResolvedLookupPathResolution = LookupPathResolution<LookupPathOrigin::HookResolved>;
using MissingLookupPathResolution = LookupPathResolution<LookupPathOrigin::Missing>;

static_assert(sizeof(MissingLookupPathResolution) == sizeof(RealizedLookupPathIdentity));
static_assert(sizeof(UnrealizedLookupPathIdentity) == sizeof(RealizedLookupPathIdentity));
static_assert(std::is_base_of_v<LookupPathResolvedRootFields, ExistingLookupPathResolution>);
static_assert(std::is_base_of_v<LookupPathResolvedRootFields, DownloadedLookupPathResolution>);
static_assert(std::is_base_of_v<LookupPathMaterializedField, DownloadedLookupPathResolution>);
static_assert(std::is_base_of_v<LookupPathResolvedRootFields, HookResolvedLookupPathResolution>);
static_assert(!std::is_base_of_v<LookupPathResolvedRootFields, MissingLookupPathResolution>);
static_assert(!std::is_base_of_v<LookupPathMaterializedField, MissingLookupPathResolution>);

using LookupPathResolvedEntry = std::variant<
    ExistingLookupPathResolution,
    DownloadedLookupPathResolution,
    HookResolvedLookupPathResolution,
    MissingLookupPathResolution>;

struct LookupPathObservation
{
    struct MatchedResolution
    {
        uint32_t matchedEntryIndex;
        SourcePath resolvedEntryRoot;
        std::optional<PathObject> matchedOrigin;
    };

    struct CorepkgsFallbackResolution
    {
    };

    LookupPathRequest request;
    std::vector<EnvVarObservation> envLookups;
    std::vector<LookupPathResolvedEntry> resolvedEntries;
    std::variant<MatchedResolution, CorepkgsFallbackResolution> resolution;
    SourcePath resolvedPath;

    const MatchedResolution * matchedResolution() const noexcept
    {
        return std::get_if<MatchedResolution>(&resolution);
    }

    bool usedCorepkgsFallback() const noexcept
    {
        return std::holds_alternative<CorepkgsFallbackResolution>(resolution);
    }

    const PathObject * matchedOrigin() const noexcept
    {
        if (auto * matched = matchedResolution(); matched && matched->matchedOrigin)
            return std::addressof(*matched->matchedOrigin);
        return nullptr;
    }
};

struct UriPolicyObservation
{
    UriPolicyRequest request;
};

struct PlaceholderRewrite
{
    std::string placeholder;
    StorePath storePath;
};

struct ContextRealisationObservation
{
    RealiseContextRequest request;
    std::vector<PlaceholderRewrite> rewrites;
    std::vector<StorePath> referencedStorePaths;
};

struct PathStatusObservation
{
    PathObservationRequest request;
    SourcePath observedPath;
    std::optional<SourceAccessor::Stat> stat;
    bool exists = false;
};

struct FileReadObservation
{
    ReadFileRequest request;
    SourcePath observedPath;
    std::string bytes;
    std::optional<TextObject> textObject;
};

struct DirectoryReadObservation
{
    ReadDirectoryRequest request;
    SourcePath observedPath;
    SourceAccessor::DirEntries entries;
    std::optional<StructuredObject> structuredObject;
};

struct StorePathObservation
{
    StorePath storePath;
};

struct StoreClosureObservation
{
    StorePath root;
};

class PublishedStorePathString final
{
    struct PreservedPublication
    {
        PathObject provenance;
    };

    struct DetachedPublication
    {
    };

    struct PlainPublication
    {
    };

    StorePath storePath_;
    std::string renderedPath_;
    NixStringContext context_;
    std::variant<PreservedPublication, DetachedPublication, PlainPublication> publication_;

    explicit PublishedStorePathString(
        StorePath storePath,
        std::string renderedPath,
        NixStringContext context,
        std::variant<PreservedPublication, DetachedPublication, PlainPublication> publication)
        : storePath_(std::move(storePath))
        , renderedPath_(std::move(renderedPath))
        , context_(std::move(context))
        , publication_(std::move(publication))
    {
    }

    static PublishedStorePathString preserve(
        StorePath storePath,
        std::string renderedPath,
        NixStringContext context,
        PathObject provenance)
    {
        return PublishedStorePathString(
            std::move(storePath),
            std::move(renderedPath),
            std::move(context),
            PreservedPublication{
                .provenance = std::move(provenance),
            });
    }

    static PublishedStorePathString detach(
        StorePath storePath,
        std::string renderedPath,
        NixStringContext context)
    {
        return PublishedStorePathString(
            std::move(storePath),
            std::move(renderedPath),
            std::move(context),
            DetachedPublication{});
    }

    static PublishedStorePathString plain(
        StorePath storePath,
        std::string renderedPath,
        NixStringContext context)
    {
        return PublishedStorePathString(
            std::move(storePath),
            std::move(renderedPath),
            std::move(context),
            PlainPublication{});
    }

    friend class EvalEnvironment;

public:
    const StorePath & storePath() const noexcept
    {
        return storePath_;
    }

    const std::string & renderedPath() const noexcept
    {
        return renderedPath_;
    }

    const NixStringContext & context() const noexcept
    {
        return context_;
    }

    const PathObject * provenance() const noexcept
    {
        if (auto * preserved = std::get_if<PreservedPublication>(&publication_))
            return std::addressof(preserved->provenance);
        return nullptr;
    }

    // appendSubpath intentionally omitted.  A PublishedStorePathString
    // represents one independently content-addressed store object.
    // Its context must match its rendered path exactly.  Appending a
    // subpath would make the rendered path diverge from the context
    // (which still names the root), producing wrong derivation hashes
    // when the string is used in a derivation's input set.
};

class ResolvedFetchIdentity final : public Linear<ResolvedFetchIdentity>
{
    struct MaterializationPayload {
        FetchIdentityRequest request;
        ResolvedLockedInput lockedInput;
        ref<SourceAccessor> accessor;
        std::optional<std::string> materializationFingerprint;
    };

    static constexpr const char * linearName = "ResolvedFetchIdentity";

private:
    MaterializationPayload payload_;

    explicit ResolvedFetchIdentity(MaterializationPayload payload)
        : payload_(std::move(payload))
    {
    }

    friend class EvalEnvironment;
    [[nodiscard]] MaterializationPayload consumeForMaterialization() &&
    {
        this->markConsumed();
        return std::move(payload_);
    }
};

struct FetchIdentityResolution
{
    fetchers::Input resolvedInput;
    fetchers::Attrs extraAttrs;
    SourcePath phase1Root;
    ResolvedFetchIdentity identity;
};

struct RuntimeRootCandidate
{
    DepSource source;
    RuntimeFetchIdentityDepKey fetchIdentity;
    RuntimeRootNarHash narHash;
    RuntimeRootStorePath storePath;
};

class FetchedInput final : public Linear<FetchedInput>
{
    struct MountPayload {
        OriginalFetchInput originalInput;
        ResolvedLockedInput lockedInput;
        ref<SourceAccessor> accessor;
        std::optional<std::string> materializationFingerprint;
    };

    static constexpr const char * linearName = "FetchedInput";

private:
    MountPayload payload_;

    explicit FetchedInput(MountPayload payload)
        : payload_(std::move(payload))
    {
    }

    friend class EvalEnvironment;
    [[nodiscard]] MountPayload consumeForMount() &&
    {
        this->markConsumed();
        return std::move(payload_);
    }
};

/// Core result of any mount operation — store path + provenance.
struct MountedStorePath
{
    StorePath storePath;
    std::optional<PathObject> provenance;
};

using DetachedMountedStorePath = MountedStorePath;

enum class MountMode : uint8_t {
    DetachedStandalone,
    DetachedGraph,
    BoundLocked,
    BoundUnlocked,
};

/// Field present only on DetachedGraph mounted inputs.
struct MountedInputGraphField {
    DepSource promotedGraphSource;
};

/// Field present only on BoundLocked mounted inputs.
struct MountedInputNarHashField {
    RuntimeRootNarHash narHash;
};

/// Field present on all mounted inputs except DetachedGraph.
struct MountedInputFinalizedLockedInputField {
    FinalizedLockedInput lockedInput;
};

namespace detail {
template <MountMode M> class MountedInput;
template <MountMode M>
using MountedInputLinearityBase = std::conditional_t<
    M == MountMode::DetachedStandalone,
    MoveOnly,
    Linear<MountedInput<M>>>;
} // namespace detail

struct GraphFetchCompletion : MountedStorePath
{
    DepSource promotedGraphSource;

private:
    GraphFetchCompletion(
        StorePath storePath,
        std::optional<PathObject> provenance,
        DepSource promotedGraphSource)
        : MountedStorePath{std::move(storePath), std::move(provenance)}
        , promotedGraphSource(std::move(promotedGraphSource))
    {}

    template <MountMode>
    friend class detail::MountedInput;
};

static_assert(!std::is_aggregate_v<GraphFetchCompletion>,
    "GraphFetchCompletion must be non-aggregate (sealed construction)");

/// Parameterized mounted input carrier.
///
/// Always-present fields (via MountedStorePath base): storePath, provenance.
/// Conditional fields are physically present or absent via EBO:
/// - lockedInput (FinalizedLockedInput): present on all modes except DetachedGraph
/// - promotedGraphSource: present only on DetachedGraph
/// - narHash: present only on BoundLocked
///
/// Removed from previous design: accessor (dead after mount — mount point owns
/// its own ref), originalInput (dead on MountedInput — passed separately where
/// needed in completion).
///
/// DetachedStandalone is terminal (MoveOnly — can be dropped without consuming).
/// All other modes are Linear (must be consumed or the destructor aborts).
template <MountMode M>
class detail::MountedInput final
    : public detail::MountedInputLinearityBase<M>
    , public MountedStorePath
    , public ConditionalBase<M != MountMode::DetachedGraph, MountedInputFinalizedLockedInputField>
    , public ConditionalBase<M == MountMode::DetachedGraph, MountedInputGraphField>
    , public ConditionalBase<M == MountMode::BoundLocked, MountedInputNarHashField>
{
    static constexpr const char * linearName = "MountedInput";

    static_assert(
        M == MountMode::DetachedStandalone || M == MountMode::DetachedGraph
        || M == MountMode::BoundLocked || M == MountMode::BoundUnlocked);

    // Conditional fields accessed through public bases:
    //   .lockedInput           — FinalizedLockedInput, absent on DetachedGraph
    //   .promotedGraphSource   — only on DetachedGraph
    //   .narHash               — only on BoundLocked

private:
    friend class nix::EvalEnvironment;

    // DetachedStandalone / BoundUnlocked: mount result + finalized locked input
    MountedInput(MountedStorePath mount, FinalizedLockedInput finalizedInput)
        requires (M == MountMode::DetachedStandalone || M == MountMode::BoundUnlocked)
        : MountedStorePath(std::move(mount))
        , MountedInputFinalizedLockedInputField{std::move(finalizedInput)}
    {}

    // DetachedGraph: mount result + graph source (no lockedInput)
    MountedInput(MountedStorePath mount, MountedInputGraphField graph)
        requires (M == MountMode::DetachedGraph)
        : MountedStorePath(std::move(mount))
        , MountedInputGraphField(std::move(graph))
    {}

    // BoundLocked: mount result + finalized locked input + nar hash
    MountedInput(MountedStorePath mount, FinalizedLockedInput finalizedInput, MountedInputNarHashField nar)
        requires (M == MountMode::BoundLocked)
        : MountedStorePath(std::move(mount))
        , MountedInputFinalizedLockedInputField{std::move(finalizedInput)}
        , MountedInputNarHashField(std::move(nar))
    {}

    [[nodiscard]] GraphFetchCompletion consumeForGraphCompletion() &&
        requires (M == MountMode::DetachedGraph)
    {
        this->markConsumed();
        return GraphFetchCompletion(
            std::move(storePath),
            std::move(provenance),
            std::move(this->promotedGraphSource));
    }

    struct LockedRuntimePayload : MountedStorePath {
        FinalizedLockedInput lockedInput;
        RuntimeRootNarHash narHash;
    };

    struct UnlockedRuntimePayload : MountedStorePath {
        FinalizedLockedInput lockedInput;
    };

    [[nodiscard]] auto consumeForRuntimeCompletion() &&
        requires (M == MountMode::BoundLocked || M == MountMode::BoundUnlocked)
    {
        this->markConsumed();
        if constexpr (M == MountMode::BoundLocked) {
            return LockedRuntimePayload{
                {std::move(storePath), std::move(provenance)},
                std::move(this->lockedInput),
                std::move(this->narHash),
            };
        } else {
            return UnlockedRuntimePayload{
                {std::move(storePath), std::move(provenance)},
                std::move(this->lockedInput),
            };
        }
    }

};

template <MountMode M>
using MountedInput = detail::MountedInput<M>;

using DetachedStandaloneMountedInput = MountedInput<MountMode::DetachedStandalone>;
using DetachedGraphMountedInput      = MountedInput<MountMode::DetachedGraph>;
using BoundLockedMountedInput        = MountedInput<MountMode::BoundLocked>;
using BoundUnlockedMountedInput      = MountedInput<MountMode::BoundUnlocked>;

// Linearity invariants
static_assert(std::is_base_of_v<MoveOnly, DetachedStandaloneMountedInput>);
static_assert(std::is_base_of_v<Linear<detail::MountedInput<MountMode::DetachedGraph>>, DetachedGraphMountedInput>);
static_assert(std::is_base_of_v<Linear<detail::MountedInput<MountMode::BoundLocked>>, BoundLockedMountedInput>);
static_assert(std::is_base_of_v<Linear<detail::MountedInput<MountMode::BoundUnlocked>>, BoundUnlockedMountedInput>);
// MountedStorePath base always present
static_assert(std::is_base_of_v<MountedStorePath, DetachedStandaloneMountedInput>);
static_assert(std::is_base_of_v<MountedStorePath, DetachedGraphMountedInput>);
static_assert(std::is_base_of_v<MountedStorePath, BoundLockedMountedInput>);
static_assert(std::is_base_of_v<MountedStorePath, BoundUnlockedMountedInput>);
// Conditional field presence
static_assert(std::is_base_of_v<MountedInputFinalizedLockedInputField, DetachedStandaloneMountedInput>);
static_assert(!std::is_base_of_v<MountedInputFinalizedLockedInputField, DetachedGraphMountedInput>);
static_assert(std::is_base_of_v<MountedInputFinalizedLockedInputField, BoundLockedMountedInput>);
static_assert(std::is_base_of_v<MountedInputFinalizedLockedInputField, BoundUnlockedMountedInput>);
static_assert(std::is_base_of_v<MountedInputGraphField, DetachedGraphMountedInput>);
static_assert(!std::is_base_of_v<MountedInputGraphField, DetachedStandaloneMountedInput>);
static_assert(std::is_base_of_v<MountedInputNarHashField, BoundLockedMountedInput>);
static_assert(!std::is_base_of_v<MountedInputNarHashField, BoundUnlockedMountedInput>);

enum class RuntimeFetchLockMode : uint8_t { Locked, Unlocked };

/// Field present only on locked runtime fetch completions.
/// Carries the narHash-based runtime root identity used for content-addressable
/// verification.
struct LockedRuntimeFetchField {
    RuntimeRootCandidate runtimeRootCandidate;
};

/// Field present only on unlocked runtime fetch completions.
/// Carries the input-attr-based runtime root identity used for
/// input-resolution verification.
struct UnlockedRuntimeFetchField {
    DepSource promotedSource;
};

namespace detail {
template <RuntimeFetchLockMode L> class PublishedRuntimeFetch;
} // namespace detail

/// Parameterized runtime fetch completion.
///
/// Always-present fields (via MountedStorePath base): storePath, provenance.
/// Always-present field: lockedInput (FinalizedLockedInput).
/// Conditional fields are physically present or absent via EBO:
/// - Locked: runtimeRootCandidate (RuntimeRootCandidate)
/// - Unlocked: promotedSource (DepSource)
///
/// The runtimeSource() accessor extracts the DepSource that matches the
/// runtime root registration, regardless of lock mode.
template <RuntimeFetchLockMode L>
class detail::PublishedRuntimeFetch final
    : public MountedStorePath
    , public ConditionalBase<L == RuntimeFetchLockMode::Locked, LockedRuntimeFetchField>
    , public ConditionalBase<L == RuntimeFetchLockMode::Unlocked, UnlockedRuntimeFetchField>
{
public:
    FinalizedLockedInput lockedInput;

    /// The runtime root DepSource for this fetch — used as originSource
    /// in emitTreeAttrs to ensure provenance matches the registered
    /// runtime root identity.
    DepSource runtimeSource() const
    {
        if constexpr (L == RuntimeFetchLockMode::Locked)
            return this->runtimeRootCandidate.source;
        else
            return this->promotedSource;
    }

private:
    friend class nix::EvalEnvironment;
    BoundEffectScope session_;

    PublishedRuntimeFetch(
        MountedStorePath mount,
        FinalizedLockedInput input,
        BoundEffectScope session,
        LockedRuntimeFetchField locked)
        requires (L == RuntimeFetchLockMode::Locked)
        : MountedStorePath(std::move(mount))
        , LockedRuntimeFetchField(std::move(locked))
        , lockedInput(std::move(input))
        , session_(std::move(session))
    {}

    PublishedRuntimeFetch(
        MountedStorePath mount,
        FinalizedLockedInput input,
        BoundEffectScope session,
        UnlockedRuntimeFetchField unlocked)
        requires (L == RuntimeFetchLockMode::Unlocked)
        : MountedStorePath(std::move(mount))
        , UnlockedRuntimeFetchField(std::move(unlocked))
        , lockedInput(std::move(input))
        , session_(std::move(session))
    {}
};

template <RuntimeFetchLockMode L>
using PublishedRuntimeFetch = detail::PublishedRuntimeFetch<L>;

using LockedPublishedRuntimeFetch = PublishedRuntimeFetch<RuntimeFetchLockMode::Locked>;
using UnlockedPublishedRuntimeFetch = PublishedRuntimeFetch<RuntimeFetchLockMode::Unlocked>;

using RuntimeFetchResult = std::variant<
    LockedPublishedRuntimeFetch,
    UnlockedPublishedRuntimeFetch>;

// MountedStorePath base always present
static_assert(std::is_base_of_v<MountedStorePath, LockedPublishedRuntimeFetch>);
static_assert(std::is_base_of_v<MountedStorePath, UnlockedPublishedRuntimeFetch>);
// Conditional field presence
static_assert(std::is_base_of_v<LockedRuntimeFetchField, LockedPublishedRuntimeFetch>);
static_assert(!std::is_base_of_v<LockedRuntimeFetchField, UnlockedPublishedRuntimeFetch>);
static_assert(!std::is_base_of_v<UnlockedRuntimeFetchField, LockedPublishedRuntimeFetch>);
static_assert(std::is_base_of_v<UnlockedRuntimeFetchField, UnlockedPublishedRuntimeFetch>);

struct RuntimeFetchIdentityObservation
{
    FetchIdentityRequest request;
    std::optional<StorePath> storePath;
};

struct GitIdentityObservation
{
    GitIdentityRequest request;
    SourcePath observedRepoRoot;
    std::optional<CurrentGitIdentityHash> hash;
};

struct DerivedStorePathObservation
{
    DerivedStorePathRequest request;
    SourcePath observedSourcePath;
    std::optional<StorePath> storePath;
};

struct SessionSystemObservation
{
    SessionCurrentSystem currentSystem;
};

} // namespace nix
