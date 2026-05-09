#include "nix/flake/eval-trace-session-open-adapter.hh"

#include "canonical-fetcher-attrs.hh"
#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/fetchers/attrs.hh"

namespace nix::flake {

namespace {

static EvalTraceHash computeResolvedGraphDigest(const LockedFlake & lockedFlake)
{
    auto builder = eval_trace::makeDomainBuilder<eval_trace::hash_domain::FlakeResolvedGraphDigest>();
    builder.field("root-key", lockedFlake.resolvedGraph.rootKey.value.value);
    for (const auto & [key, node] : lockedFlake.resolvedGraph.nodes) {
        const bool isRoot =
            asFlakeGraphNodeKey(key) == asFlakeGraphNodeKey(lockedFlake.resolvedGraph.rootKey);
        builder.field("node-key", asFlakeGraphNodeKey(key).value);
        builder.field("node-source-info-key", node.sourceInfoKey.value.value);
        builder.field("node-locked-version-identity", node.lockedVersionIdentity);

        const bool excludeNarHash =
            isRoot || (node.lockedInput.getType() == "path");
        if (!excludeNarHash) {
            if (auto narHash = node.lockedInput.getNarHash())
                builder.field("node-nar-hash", narHash->to_string(HashFormat::SRI, true));
        }

        builder.field("node-evaluation-root", node.evaluationRoot.value.path.abs());
        builder.field("node-carrier-root", node.carrierRoot.value.path.abs());
        builder.field("node-relative-path", node.relativePath.value);
        builder.field("node-subdir", node.subdir.value);
        builder.field("node-is-flake", node.isFlake);
        if (node.parentKey)
            builder.field("node-parent-key", node.parentKey->value.value);

        for (const auto & [inputName, inputSpec] : node.inputSpecs) {
            builder.field("input-spec-name", inputName);
            if (auto targetKey = inputSpec.targetNodeKey()) {
                builder.field("input-spec-kind", "target-key");
                builder.field("input-spec-target-key", targetKey->value.value);
            } else {
                const auto & followsPath = *inputSpec.followsPath();
                builder.field("input-spec-kind", "follows-path");
                builder.field("input-spec-follows-count", static_cast<uint64_t>(followsPath.size()));
                for (const auto & component : followsPath)
                    builder.field("input-spec-follows-component", component);
            }
        }

        for (const auto & [inputName, targetKey] : node.resolvedInputs) {
            builder.field("resolved-input-name", inputName);
            builder.field("resolved-input-target-key", targetKey.value.value);
        }
    }

    return builder.finish();
}

static EvalTraceHash computeOriginalSourceIdentityHash(
    eval_trace::CanonicalHashBuilder && builder,
    const LockedFlake & lockedFlake)
{
    appendFetcherAttrs(builder, lockedFlake.flake.originalRef.value.input.toAttrs());
    builder.field("original-subdir", lockedFlake.flake.originalRef.value.subdir);
    return builder.finish();
}

static StableRecoveryKey computeStableRecoveryKey(
    const LockedFlake & lockedFlake)
{
    return StableRecoveryKey{
        computeOriginalSourceIdentityHash(
            eval_trace::makeDomainBuilder<eval_trace::hash_domain::FlakeStableRecoveryKeyDomain>(),
            lockedFlake),
    };
}

static FlakeSourceIdentity computeFlakeSourceIdentity(
    const LockedFlake & lockedFlake,
    const std::optional<Fingerprint> &)
{
    return FlakeSourceIdentity{
        computeOriginalSourceIdentityHash(
            eval_trace::makeDomainBuilder<eval_trace::hash_domain::FlakeSourceIdentityDomain>(),
            lockedFlake),
    };
}

std::vector<FlakeGraphAuthorityNodeSpec> buildAuthorityNodeSpecs(
    const LockedFlake & lockedFlake)
{
    std::vector<FlakeGraphAuthorityNodeSpec> nodes;
    nodes.reserve(lockedFlake.resolvedGraph.nodes.size());

    for (const auto & [key, node] : lockedFlake.resolvedGraph.nodes) {
        nodes.push_back(FlakeGraphAuthorityNodeSpec{
            .nodeKey = asFlakeGraphNodeKey(key),
            .evaluationRoot = EvalTraceFlakeEvaluationRootPath{node.flakePath()},
            .carrierRoot = EvalTraceFlakeCarrierRootPath{node.carrierPath()},
            .mountSubdir = RegistryMountSubdir{
                node.relativePath.value.empty()
                    ? CanonPath::root
                    : CanonPath("/" + node.relativePath.value),
            },
        });
    }

    return nodes;
}

} // namespace

FlakeTraceSessionConfigRequest buildTraceSessionConfigRequest(
    const LockedFlake & lockedFlake,
    std::optional<Fingerprint> lockedFlakeFingerprint)
{
    return FlakeTraceSessionConfigRequest{
        .graphDigest = computeResolvedGraphDigest(lockedFlake),
        .sourceIdentity = computeFlakeSourceIdentity(lockedFlake, lockedFlakeFingerprint),
        .stableRecoveryKey = computeStableRecoveryKey(lockedFlake),
    };
}

std::vector<FlakeGraphAuthorityNodeSpec> buildFlakeAuthorityNodeSpecs(
    const LockedFlake & lockedFlake)
{
    return buildAuthorityNodeSpecs(lockedFlake);
}

} // namespace nix::flake
