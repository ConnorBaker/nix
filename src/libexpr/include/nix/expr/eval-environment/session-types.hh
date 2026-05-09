#pragma once

#include "nix/expr/eval-environment/capabilities.hh"
#include "nix/expr/eval-environment/domains.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/util/hash.hh"
#include "nix/util/linear.hh"
#include "nix/util/source-path.hh"
#include "nix/util/tagged.hh"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nix {

using SessionSourceIdentity = std::variant<
    FlakeSourceIdentity,
    CurrentGitIdentityHash,
    FileEvalLogicalSourceIdentity>;

enum class EvalPurityMode : uint8_t {
    Pure = 0,
    Impure,
};

enum class EvalRestrictionMode : uint8_t {
    Restricted = 0,
    Unrestricted,
};

enum class ImportFromDerivationMode : uint8_t {
    Disabled = 0,
    Enabled,
};

struct EvalTraceInputAccessorBinding
{
    DepSource source;
    SourcePath path;
};

struct EvalTraceMountBinding
{
    CanonPath mountPoint;
    DepSource source;
    RegistryMountSubdir subdir;
};

struct EvalTraceSessionConfigInput
{
    EvalTraceHash policyDigest;
    std::optional<EvalTraceHash> graphDigest;
    SessionSourceIdentity sourceIdentity;
    std::vector<ExternalRootIdentity> externalRoots;
    StableRecoveryKey stableRecoveryKey;
};

struct EvalPolicySnapshot
{
    bool useTraceCache = false;
    EvalPurityMode purityMode = EvalPurityMode::Pure;
    EvalRestrictionMode restrictionMode = EvalRestrictionMode::Restricted;
    ImportFromDerivationMode ifdMode = ImportFromDerivationMode::Disabled;
    SessionCurrentSystem currentSystem;
    std::string nixPathEnv;
    std::vector<std::string> nixPathSetting;
    std::vector<std::string> allowedUris;

private:
    EvalPolicySnapshot(
        bool useTraceCache,
        EvalPurityMode purityMode,
        EvalRestrictionMode restrictionMode,
        ImportFromDerivationMode ifdMode,
        SessionCurrentSystem currentSystem,
        std::string nixPathEnv,
        std::vector<std::string> nixPathSetting,
        std::vector<std::string> allowedUris)
        : useTraceCache(useTraceCache)
        , purityMode(purityMode)
        , restrictionMode(restrictionMode)
        , ifdMode(ifdMode)
        , currentSystem(std::move(currentSystem))
        , nixPathEnv(std::move(nixPathEnv))
        , nixPathSetting(std::move(nixPathSetting))
        , allowedUris(std::move(allowedUris))
    {}

    friend class EvalEnvironment;
};

static_assert(!std::is_aggregate_v<EvalPolicySnapshot>,
    "EvalPolicySnapshot must be non-aggregate (sealed construction)");

struct FlakeTraceSessionConfigRequest
{
    EvalTraceHash graphDigest;
    FlakeSourceIdentity sourceIdentity;
    StableRecoveryKey stableRecoveryKey;
};

struct FileEvalGitIdentitySnapshot
{
    ExternalRootIdentity repoRoot;
    CurrentGitIdentityHash gitIdentityHash;
};

struct NoSessionReuseRequested
{
};

using FlakeSessionReuseDecision = std::variant<
    NoSessionReuseRequested,
    FlakeSessionReuseKey>;

using FileEvalReuseSource = std::variant<
    FileEvalAbsoluteFilePath,
    FileEvalExpressionHash,
    std::monostate>;

enum class FileEvalReuseKeyUncacheableReason : uint8_t {
    StdinInput = 0,
    MissingAutoArgsIdentity,
};

struct FileEvalTraceSessionReuseKey
{
    FileEvalSessionReuseKeyValue value;
};

using FileEvalSessionReuseDecision = std::variant<
    NoSessionReuseRequested,
    FileEvalTraceSessionReuseKey,
    FileEvalReuseKeyUncacheableReason>;

class EvalTraceSessionAuthority;
using FlakeEvalSessionOpen = std::tuple<EvalTraceSessionAuthority, FlakeSessionReuseDecision>;
using FileEvalSessionOpen = std::tuple<EvalTraceSessionAuthority, FileEvalSessionReuseDecision>;

struct FileEvalTraceSessionReuseKeyInputs
{
    FileEvalReuseSource source;
    std::optional<FileEvalAutoArgsHash> autoArgsIdentity;
    StoreDirIdentity storeDir;
    SessionCurrentSystem currentSystem;
};

struct RootLoadDepObservation
{
    CanonicalQueryKind kind;
    DepSource source;
    SimpleDepKeyAtom key;
    DepHashValue hash;
};

using EvalTraceFlakeEvaluationRootPath =
    Tagged<struct EvalTraceFlakeEvaluationRootPathTag_, SourcePath>;
using EvalTraceFlakeCarrierRootPath =
    Tagged<struct EvalTraceFlakeCarrierRootPathTag_, SourcePath>;

struct TraceSessionReuseSlotKey
{
    std::variant<FlakeSessionReuseKey, FileEvalSessionReuseKeyValue> value;

    bool operator==(const TraceSessionReuseSlotKey &) const = default;

    struct Hash {
        // No `is_avalanching` marker.  `hashValues` is `hash_combine`
        // over identity hashes on libstdc++; it does not avalanche.
        size_t operator()(const TraceSessionReuseSlotKey & key) const noexcept
        {
            return std::visit([](const auto & value) -> size_t {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<T, FlakeSessionReuseKey>)
                    return hashValues(uint8_t{0}, typename FlakeSessionReuseKey::Hash{}(value));
                else
                    return hashValues(uint8_t{1}, typename FileEvalSessionReuseKeyValue::Hash{}(value));
            }, key.value);
        }
    };
};

class CapturedSessionOpenInputs;
class FlakeGraphTraceSessionAuthorityRequest;

class CommonTraceSessionAuthorityInputs final : public Linear<CommonTraceSessionAuthorityInputs>
{
    static constexpr const char * linearName = "CommonTraceSessionAuthorityInputs";

public:
    struct AssemblyPayload {
        RootLoaderCapability rootLoader;
        std::vector<EvalTraceInputAccessorBinding> inputAccessors;
        std::vector<EvalTraceMountBinding> mountedInputs;
        std::vector<RootLoadDepObservation> rootLoadDeps;
        eval_trace::SemanticRegistry registrySeed;
    };

private:
    AssemblyPayload payload_;

    explicit CommonTraceSessionAuthorityInputs(AssemblyPayload payload)
        : payload_(std::move(payload))
    {
    }

private:
    friend FlakeEvalSessionOpen assembleFlakeTraceSessionOpen(
        CapturedSessionOpenInputs sessionInputs,
        FlakeGraphTraceSessionAuthorityRequest authorityRequest,
        const FlakeTraceSessionConfigRequest & sessionConfigRequest);
    friend FileEvalSessionOpen assembleFileEvalTraceSessionOpen(
        CapturedSessionOpenInputs sessionInputs,
        CommonTraceSessionAuthorityInputs inputs,
        const std::optional<FileEvalGitIdentitySnapshot> & sessionGitIdentity,
        const FileEvalTraceSessionReuseKeyInputs & reuseKeyInputs);

public:
    static CommonTraceSessionAuthorityInputs create(
        RootLoaderCapability rootLoader,
        std::vector<EvalTraceInputAccessorBinding> inputAccessors,
        std::vector<EvalTraceMountBinding> mountedInputs,
        std::vector<RootLoadDepObservation> rootLoadDeps,
        eval_trace::SemanticRegistry registrySeed)
    {
        return CommonTraceSessionAuthorityInputs(AssemblyPayload{
            .rootLoader = std::move(rootLoader),
            .inputAccessors = std::move(inputAccessors),
            .mountedInputs = std::move(mountedInputs),
            .rootLoadDeps = std::move(rootLoadDeps),
            .registrySeed = std::move(registrySeed),
        });
    }

private:
    [[nodiscard]] AssemblyPayload consumeForAssembly() &&
    {
        this->markConsumed();
        return std::move(payload_);
    }
};

struct FlakeGraphAuthorityNodeSpec
{
    FlakeGraphNodeKey nodeKey;
    EvalTraceFlakeEvaluationRootPath evaluationRoot;
    EvalTraceFlakeCarrierRootPath carrierRoot;
    RegistryMountSubdir mountSubdir;
};

class FlakeGraphTraceSessionAuthorityRequest final : public Linear<FlakeGraphTraceSessionAuthorityRequest>
{
    static constexpr const char * linearName = "FlakeGraphTraceSessionAuthorityRequest";

public:
    struct AssemblyPayload {
        RootLoaderCapability rootLoader;
        std::vector<FlakeGraphAuthorityNodeSpec> nodes;
        std::vector<RootLoadDepObservation> rootLoadDeps;
    };

private:
    AssemblyPayload payload_;

    explicit FlakeGraphTraceSessionAuthorityRequest(AssemblyPayload payload)
        : payload_(std::move(payload))
    {
    }

    friend FlakeEvalSessionOpen assembleFlakeTraceSessionOpen(
        CapturedSessionOpenInputs sessionInputs,
        FlakeGraphTraceSessionAuthorityRequest authorityRequest,
        const FlakeTraceSessionConfigRequest & sessionConfigRequest);

public:
    static FlakeGraphTraceSessionAuthorityRequest create(
        RootLoaderCapability rootLoader,
        std::vector<FlakeGraphAuthorityNodeSpec> nodes,
        std::vector<RootLoadDepObservation> rootLoadDeps)
    {
        return FlakeGraphTraceSessionAuthorityRequest(AssemblyPayload{
            .rootLoader = std::move(rootLoader),
            .nodes = std::move(nodes),
            .rootLoadDeps = std::move(rootLoadDeps),
        });
    }

private:
    [[nodiscard]] AssemblyPayload consumeForAssembly() &&
    {
        this->markConsumed();
        return std::move(payload_);
    }
};

class CapturedSessionOpenInputs final : public Linear<CapturedSessionOpenInputs>
{
    static constexpr const char * linearName = "CapturedSessionOpenInputs";

public:
    struct AssemblyPayload {
        EvalPolicySnapshot policySnapshot;
        std::vector<UnrealizedFullLookupPathEntry> lookupPathEntries;
    };

private:
    AssemblyPayload payload_;

    explicit CapturedSessionOpenInputs(AssemblyPayload payload)
        : payload_(std::move(payload))
    {
    }

    CapturedSessionOpenInputs(EvalPolicySnapshot policySnapshot, std::vector<UnrealizedFullLookupPathEntry> lookupPathEntries)
        : CapturedSessionOpenInputs(AssemblyPayload{
            .policySnapshot = std::move(policySnapshot),
            .lookupPathEntries = std::move(lookupPathEntries),
        })
    {
    }

    friend class EvalEnvironment;
    friend FlakeEvalSessionOpen assembleFlakeTraceSessionOpen(
        CapturedSessionOpenInputs sessionInputs,
        FlakeGraphTraceSessionAuthorityRequest authorityRequest,
        const FlakeTraceSessionConfigRequest & sessionConfigRequest);
    friend FileEvalSessionOpen assembleFileEvalTraceSessionOpen(
        CapturedSessionOpenInputs sessionInputs,
        CommonTraceSessionAuthorityInputs inputs,
        const std::optional<FileEvalGitIdentitySnapshot> & sessionGitIdentity,
        const FileEvalTraceSessionReuseKeyInputs & reuseKeyInputs);

    [[nodiscard]] AssemblyPayload consumeForAssembly() &&
    {
        this->markConsumed();
        return std::move(payload_);
    }
};

class EvalTraceSessionAuthority final : public Linear<EvalTraceSessionAuthority>
{
    static constexpr const char * linearName = "EvalTraceSessionAuthority";

public:
    struct OpenPayload {
        RootLoaderCapability rootLoader;
        std::vector<EvalTraceInputAccessorBinding> inputAccessors;
        std::vector<EvalTraceMountBinding> mountedInputs;
        std::vector<RootLoadDepObservation> rootLoadDeps;
        eval_trace::SemanticRegistry registrySeed;
        std::optional<EvalTraceSessionConfigInput> sessionConfig;
    };

private:
    OpenPayload payload_;

    explicit EvalTraceSessionAuthority(OpenPayload payload)
        : payload_(std::move(payload))
    {
    }

    friend class EvalEnvironment;
    friend class TraceSessionFactory;
    friend FlakeEvalSessionOpen assembleFlakeTraceSessionOpen(
        CapturedSessionOpenInputs sessionInputs,
        FlakeGraphTraceSessionAuthorityRequest authorityRequest,
        const FlakeTraceSessionConfigRequest & sessionConfigRequest);
    friend FileEvalSessionOpen assembleFileEvalTraceSessionOpen(
        CapturedSessionOpenInputs sessionInputs,
        CommonTraceSessionAuthorityInputs inputs,
        const std::optional<FileEvalGitIdentitySnapshot> & sessionGitIdentity,
        const FileEvalTraceSessionReuseKeyInputs & reuseKeyInputs);

public:
    void discardForSessionReuse() &&
    {
        std::move(payload_.rootLoader).discardUnused();
        payload_.inputAccessors.clear();
        payload_.mountedInputs.clear();
        payload_.rootLoadDeps.clear();
        payload_.registrySeed = eval_trace::SemanticRegistry{};
        payload_.sessionConfig.reset();
        this->markConsumed();
    }

    [[nodiscard]] OpenPayload consumeForTraceSessionOpen() &&
    {
        this->markConsumed();
        return std::move(payload_);
    }
};

} // namespace nix
