#include "nix/expr/eval-environment/authority-internal.hh"
#include "nix/expr/eval-environment/observation-types.hh"

#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/expr/search-path.hh"
#include "nix/expr/eval-trace/store/session-identity.hh"

namespace nix {

namespace {

EvalTraceHash computePolicyDigest(const EvalPolicySnapshot & snapshot)
{
    auto builder = eval_trace::makeDomainBuilder<eval_trace::hash_domain::PolicySnapshot>();
    builder.field("purity-mode", snapshot.purityMode);
    builder.field("restriction-mode", snapshot.restrictionMode);
    builder.field("current-system", snapshot.currentSystem.value);
    builder.field("ifd-mode", snapshot.ifdMode);

    if (snapshot.purityMode == EvalPurityMode::Impure) {
        builder.field("nix-path-env", snapshot.nixPathEnv);
        builder.field("nix-path-count", static_cast<uint64_t>(snapshot.nixPathSetting.size()));
        for (const auto & entry : snapshot.nixPathSetting)
            builder.field("nix-path-entry", entry);
    }

    // allowed-uris gates URI resolution under restrict-eval. Two sessions
    // with the same purity/restriction/NIX_PATH but different allowed-uris
    // may produce different evaluation results (fetch* accepts/rejects
    // different URIs), so include it unconditionally in the digest.
    builder.field("allowed-uris-count", static_cast<uint64_t>(snapshot.allowedUris.size()));
    for (const auto & uri : snapshot.allowedUris)
        builder.field("allowed-uri", uri);

    return builder.finish();
}

EvalTraceHash computeFileEvalLogicalIdentityHash(
    eval_trace::CanonicalHashBuilder && builder,
    const FileEvalTraceSessionReuseKeyInputs & request,
    const std::vector<UnrealizedLookupPathIdentity> & lookupPathEntries,
    const std::optional<EvalTraceHash> & policyDigest)
{
    std::visit([&](const auto & source) {
        using T = std::decay_t<decltype(source)>;
        if constexpr (std::same_as<T, FileEvalAbsoluteFilePath>) {
            builder.field("source-kind", "file");
            builder.field("absolute-file-path", source);
        } else if constexpr (std::same_as<T, FileEvalExpressionHash>) {
            builder.field("source-kind", "expr");
            builder.field("expression-hash", source);
        } else {
            builder.field("source-kind", "stdin");
        }
    }, request.source);

    builder.optionalField("auto-args-identity", request.autoArgsIdentity);
    builder.field("lookup-path-entry-count", static_cast<uint64_t>(lookupPathEntries.size()));
    for (const auto & entry : lookupPathEntries) {
        builder.optionalField("lookup-prefix", entry.prefix);
        builder.field("lookup-raw-value", entry.rawValue.value);
    }
    builder.field("store-dir", request.storeDir);
    builder.field("current-system", request.currentSystem.value);
    // Policy digest is folded in only when the caller asks for it. It is
    // needed for the stable recovery key (OR-5: distinct policy must
    // produce distinct History lineage to prevent cross-session recovery
    // from serving a trace that was recorded under different
    // allowed-uris / pure-eval / etc.). It is deliberately omitted from
    // the source-identity and session-reuse keys, which identify "what
    // is being evaluated" independent of policy.
    builder.optionalField("policy-digest", policyDigest);
    return builder.finish();
}

FileEvalLogicalSourceIdentity buildFileEvalLogicalSourceIdentity(
    const FileEvalTraceSessionReuseKeyInputs & request,
    const std::vector<UnrealizedLookupPathIdentity> & lookupPathEntries)
{
    return FileEvalLogicalSourceIdentity{
        computeFileEvalLogicalIdentityHash(
            eval_trace::makeDomainBuilder<eval_trace::hash_domain::FileEvalSourceIdentity>(),
            request, lookupPathEntries, /*policyDigest=*/std::nullopt),
    };
}

StableRecoveryKey buildFileEvalStableRecoveryKey(
    const FileEvalTraceSessionReuseKeyInputs & request,
    const std::vector<UnrealizedLookupPathIdentity> & lookupPathEntries,
    const EvalTraceHash & policyDigest)
{
    return StableRecoveryKey{
        computeFileEvalLogicalIdentityHash(
            eval_trace::makeDomainBuilder<eval_trace::hash_domain::FileEvalStableRecoveryKey>(),
            request, lookupPathEntries, policyDigest),
    };
}

}

eval_trace::SessionConfig toTraceSessionConfig(const EvalTraceSessionConfigInput & input)
{
    std::vector<eval_trace::SessionExternalRoot> externalRoots;
    externalRoots.reserve(input.externalRoots.size());
    for (const auto & root : input.externalRoots)
        externalRoots.push_back(eval_trace::SessionExternalRoot{root.value});

    return eval_trace::SessionConfig{
        .policyDigest = input.policyDigest,
        .graphDigest = input.graphDigest,
        .sourceIdentity = std::visit([](const auto & source) -> eval_trace::SessionSourceDigest {
            return eval_trace::SessionSourceDigest{source.value};
        }, input.sourceIdentity),
        .externalRoots = std::move(externalRoots),
        .stableRecoveryKey = eval_trace::SessionRecoveryKey{input.stableRecoveryKey.value},
    };
}

static EvalTraceSessionConfigInput buildFlakeSessionConfig(
    const EvalPolicySnapshot & policySnapshot,
    const FlakeTraceSessionConfigRequest & request)
{
    return EvalTraceSessionConfigInput{
        .policyDigest = computePolicyDigest(policySnapshot),
        .graphDigest = request.graphDigest,
        .sourceIdentity = SessionSourceIdentity{request.sourceIdentity},
        .externalRoots = {},
        .stableRecoveryKey = request.stableRecoveryKey,
    };
}

std::optional<FileEvalGitIdentitySnapshot> buildFileEvalSessionConfigInputs(
    const GitIdentityObservation & observation)
{
    if (!observation.hash)
        return {};

    return FileEvalGitIdentitySnapshot{
        .repoRoot = ExternalRootIdentity{CanonPath(observation.observedRepoRoot.path.abs())},
        .gitIdentityHash = *observation.hash,
    };
}

static EvalTraceSessionConfigInput buildFileEvalSessionConfig(
    const EvalPolicySnapshot & policySnapshot,
    const std::optional<FileEvalGitIdentitySnapshot> & gitIdentity,
    const FileEvalTraceSessionReuseKeyInputs & request,
    const std::vector<UnrealizedLookupPathIdentity> & lookupPathEntries)
{
    auto policyDigest = computePolicyDigest(policySnapshot);
    EvalTraceSessionConfigInput config{
        .policyDigest = policyDigest,
        .graphDigest = std::nullopt,
        .sourceIdentity = SessionSourceIdentity{buildFileEvalLogicalSourceIdentity(request, lookupPathEntries)},
        .externalRoots = {},
        .stableRecoveryKey = buildFileEvalStableRecoveryKey(request, lookupPathEntries, policyDigest),
    };

    if (gitIdentity) {
        config.sourceIdentity = SessionSourceIdentity{gitIdentity->gitIdentityHash};
        config.externalRoots = {gitIdentity->repoRoot};
    }

    return config;
}

static FlakeSessionReuseKey buildFlakeSessionReuseKey(
    const EvalTraceSessionConfigInput & sessionConfig)
{
    auto sessionKey = toTraceSessionConfig(sessionConfig).buildSemanticSessionKey();
    return FlakeSessionReuseKey{sessionKey.digest};
}

static FileEvalSessionReuseDecision buildFileEvalSessionReuseKey(
    const FileEvalTraceSessionReuseKeyInputs & request,
    const std::vector<UnrealizedLookupPathIdentity> & lookupPathEntries)
{
    if (std::holds_alternative<std::monostate>(request.source))
        return FileEvalReuseKeyUncacheableReason::StdinInput;

    if (!request.autoArgsIdentity)
        return FileEvalReuseKeyUncacheableReason::MissingAutoArgsIdentity;

    return FileEvalTraceSessionReuseKey{
        .value = FileEvalSessionReuseKeyValue{
            computeFileEvalLogicalIdentityHash(
                eval_trace::makeDomainBuilder<eval_trace::hash_domain::FileEvalSessionReuseKey>(),
                request, lookupPathEntries, /*policyDigest=*/std::nullopt),
        },
    };
}

std::tuple<EvalTraceSessionAuthority, FlakeSessionReuseDecision> assembleFlakeTraceSessionOpen(
    CapturedSessionOpenInputs sessionInputs,
    FlakeGraphTraceSessionAuthorityRequest authorityRequest,
    const FlakeTraceSessionConfigRequest & sessionConfigRequest)
{
    auto sessionInputsPayload = std::move(sessionInputs).consumeForAssembly();
    auto authorityRequestPayload = std::move(authorityRequest).consumeForAssembly();

    std::vector<EvalTraceInputAccessorBinding> inputAccessors;
    std::vector<EvalTraceMountBinding> mountedInputs;
    boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> registryEntries;
    inputAccessors.reserve(authorityRequestPayload.nodes.size());
    mountedInputs.reserve(authorityRequestPayload.nodes.size());

    for (const auto & node : authorityRequestPayload.nodes) {
        auto source = DepSource::fromNodeKey(GraphNodeDepSourceKey{node.nodeKey.value});
        inputAccessors.push_back(EvalTraceInputAccessorBinding{
            .source = source,
            .path = node.evaluationRoot.value,
        });
        mountedInputs.push_back(EvalTraceMountBinding{
            .mountPoint = node.carrierRoot.value.path,
            .source = source,
            .subdir = node.mountSubdir,
        });
        registryEntries.insert_or_assign(source, node.evaluationRoot.value);
    }

    std::optional<EvalTraceSessionConfigInput> sessionConfig;
    FlakeSessionReuseDecision sessionReuse = NoSessionReuseRequested{};
    if (sessionInputsPayload.policySnapshot.useTraceCache) {
        sessionConfig = buildFlakeSessionConfig(
            sessionInputsPayload.policySnapshot,
            sessionConfigRequest);
        sessionReuse = buildFlakeSessionReuseKey(*sessionConfig);
    }

    auto authority = EvalTraceSessionAuthority(
        EvalTraceSessionAuthority::OpenPayload{
            .rootLoader = std::move(authorityRequestPayload.rootLoader),
            .inputAccessors = std::move(inputAccessors),
            .mountedInputs = std::move(mountedInputs),
            .rootLoadDeps = std::move(authorityRequestPayload.rootLoadDeps),
            .registrySeed = eval_trace::SemanticRegistry(std::move(registryEntries)),
            .sessionConfig = std::move(sessionConfig),
        });

    return {
        std::move(authority),
        std::move(sessionReuse),
    };
}

std::tuple<EvalTraceSessionAuthority, FileEvalSessionReuseDecision> assembleFileEvalTraceSessionOpen(
    CapturedSessionOpenInputs sessionInputs,
    CommonTraceSessionAuthorityInputs inputs,
    const std::optional<FileEvalGitIdentitySnapshot> & sessionGitIdentity,
    const FileEvalTraceSessionReuseKeyInputs & reuseKeyInputs)
{
    auto sessionInputsPayload = std::move(sessionInputs).consumeForAssembly();
    auto inputsPayload = std::move(inputs).consumeForAssembly();

    std::optional<EvalTraceSessionConfigInput> sessionConfig;
    FileEvalSessionReuseDecision sessionReuse = NoSessionReuseRequested{};

    if (sessionInputsPayload.policySnapshot.useTraceCache) {
        std::vector<UnrealizedLookupPathIdentity> lookupPathIdentityEntries;
        lookupPathIdentityEntries.reserve(sessionInputsPayload.lookupPathEntries.size());
        for (const auto & entry : sessionInputsPayload.lookupPathEntries) {
            lookupPathIdentityEntries.push_back(entry.toIdentity());
        }

        sessionReuse = buildFileEvalSessionReuseKey(reuseKeyInputs, lookupPathIdentityEntries);

        if (std::holds_alternative<FileEvalTraceSessionReuseKey>(sessionReuse)) {
            sessionConfig = buildFileEvalSessionConfig(
                sessionInputsPayload.policySnapshot,
                sessionGitIdentity,
                reuseKeyInputs,
                lookupPathIdentityEntries);
        }
    }

    auto authority = EvalTraceSessionAuthority(
        EvalTraceSessionAuthority::OpenPayload{
            .rootLoader = std::move(inputsPayload.rootLoader),
            .inputAccessors = std::move(inputsPayload.inputAccessors),
            .mountedInputs = std::move(inputsPayload.mountedInputs),
            .rootLoadDeps = std::move(inputsPayload.rootLoadDeps),
            .registrySeed = std::move(inputsPayload.registrySeed),
            .sessionConfig = std::move(sessionConfig),
        });

    return {
        std::move(authority),
        std::move(sessionReuse),
    };
}

} // namespace nix
