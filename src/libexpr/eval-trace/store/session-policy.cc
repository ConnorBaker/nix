#include "nix/expr/eval-trace/store/session-policy.hh"
#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/util/environment-variables.hh"

#include <cstdint>

namespace nix::eval_trace {

EvalTraceHash computePolicyDigest(const EvalSettings & settings)
{
    auto builder = makeDomainBuilder<hash_domain::PolicyDigestSettings>();
    builder.field("pure-eval", static_cast<bool>(settings.pureEval));
    builder.field("restrict-eval", static_cast<bool>(settings.restrictEval));
    builder.field("current-system", settings.getCurrentSystem());
    builder.field("enable-import-from-derivation", static_cast<bool>(settings.enableImportFromDerivation));

    // In impure mode, NIX_PATH affects lookup-path resolution.
    // Include it so sessions with different lookup paths don't share a key.
    // Note: NIX_PATH is not an EvalSettings field — the env var overrides
    // the nix-path config key in impure mode, so it must be read here.
    if (!settings.pureEval) {
        builder.optionalField("nix-path-env", getEnv("NIX_PATH"));
        // Also include nix-path config setting / --nix-path CLI flag,
        // which takes precedence over NIX_PATH env var.
        builder.field("nix-path-count", static_cast<uint64_t>(settings.nixPath.get().size()));
        for (auto & p : settings.nixPath.get())
            builder.field("nix-path-entry", p);
    } else {
        builder.optionalField("nix-path-env", std::optional<std::string>{});
        builder.field("nix-path-count", uint64_t{0});
    }

    // allowed-uris gates URI resolution under restrict-eval. Two sessions
    // with the same purity/restriction/NIX_PATH but different allowed-uris
    // may produce different results (fetch* accepts/rejects different
    // URIs), so include it unconditionally in the digest.
    builder.field("allowed-uris-count", static_cast<uint64_t>(settings.allowedUris.get().size()));
    for (auto & uri : settings.allowedUris.get())
        builder.field("allowed-uri", uri);

    return builder.finish();
}

} // namespace nix::eval_trace
