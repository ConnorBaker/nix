#include "nix/expr/eval-trace/store/session-identity.hh"
#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/expr/eval-trace/hash-spec.hh"

namespace nix::eval_trace {

SemanticSessionKey SemanticSessionKey::fromSerialized(std::string_view serialized)
{
    auto builder = makeDomainBuilder<hash_domain::SemanticSessionKeySeed>();
    builder.field("serialized-seed", serialized);
    return SemanticSessionKey::fromDigest(builder.finish());
}

SemanticSessionKey SessionConfig::buildSemanticSessionKey(EvalTraceHashAlgorithm algorithm) const
{
    auto builder = makeDomainBuilder<hash_domain::SemanticSessionKey>(algorithm);
    builder.field("schema-epoch", kSchemaEpoch);
    builder.field("provider-epoch", kProviderEpoch);
    builder.field("hash-algorithm", evalTraceHashAlgorithmSlug(algorithm));
    builder.field("hash-size", static_cast<uint64_t>(kEvalTraceDigestSize));
    builder.field("policy-digest", policyDigest);
    builder.optionalField("graph-digest", graphDigest);
    builder.field("source-identity", sourceIdentity);
    builder.field("external-root-count", static_cast<uint64_t>(externalRoots.size()));
    for (const auto & root : externalRoots)
        builder.field("external-root", root);
    return SemanticSessionKey::fromDigest(builder.finish());
}

SemanticSessionKey SessionConfig::buildSemanticSessionKey() const
{
    return buildSemanticSessionKey(getEvalTraceHashAlgorithm());
}

SessionConfig SessionConfig::forTest(EvalTraceHash policyDigest, std::string_view stableRecoveryKey)
{
    auto builder = makeDomainBuilder<hash_domain::TestStableRecoveryKey>();
    if (!stableRecoveryKey.empty())
        builder.field("seed", stableRecoveryKey);
    else
        builder.field("seed", policyDigest);

    return SessionConfig{
        .policyDigest = policyDigest,
        .sourceIdentity = SessionSourceDigest{policyDigest},
        .stableRecoveryKey = SessionRecoveryKey{builder.finish()},
    };
}

} // namespace nix::eval_trace
