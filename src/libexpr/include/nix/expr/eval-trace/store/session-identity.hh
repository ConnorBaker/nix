#pragma once
/// session-identity.hh — Session config, identity keys, and epoch constants.
///
/// Split from trace-store.hh so that TUs needing only session identity types
/// (flake.cc, eval-environment/session-builders.cc, libflake) don't transitively
/// pull in the SQLite-backed storage interface.
///
/// `EvalTraceHash` lives in its own lightweight header so this file
/// does NOT transitively pull the ~1200-line `deps/types.hh` or
/// anything that depends on it. Lightweight consumers
/// (flake.cc, session-builders.cc, libflake) now get a minimal
/// dep graph rooted at this header.

#include "nix/expr/eval-trace/eval-trace-hash.hh"
#include "nix/expr/eval-trace/hash-spec.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/error.hh"
#include "nix/util/tagged.hh"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nix::eval_trace {

/// Schema and provider epoch constants for session identity.
///
/// Epoch 25: schema cleanups (P-SK): Results.encoding column dropped
///           (encoding version is global per kSemanticResultEncodingVersion);
///           Strings.value UNIQUE replaced with an ordinary index.
inline constexpr uint32_t kSchemaEpoch = 25;
inline constexpr uint32_t kProviderEpoch = 1;

using SessionSourceDigest = Tagged<struct SessionSourceDigestTag_, EvalTraceHash>;
using SessionRecoveryKey = Tagged<struct SessionRecoveryKeyTag_, EvalTraceHash>;
using SessionExternalRoot = Tagged<struct SessionExternalRootTag_, CanonPath>;

struct SemanticSessionKey {
    EvalTraceHash digest;

    static SemanticSessionKey fromSerialized(std::string_view serialized);
    static SemanticSessionKey fromDigest(EvalTraceHash digest)
    {
        return SemanticSessionKey{.digest = digest};
    }

    std::string toHex() const
    {
        return digest.toHex();
    }
};

/// Immutable session configuration set once at session creation.
///
/// `buildSemanticSessionKey(algorithm)` is a pure function of its
/// inputs: the `SessionConfig` fields and the hash algorithm passed
/// in. No reads of process-global state — two calls with identical
/// inputs always produce identical digests. The overload without an
/// algorithm argument reads `getEvalTraceHashAlgorithm()` once at
/// call time as a compatibility hatch; prefer the explicit form in
/// new code (ref adversarial review #8).
struct SessionConfig {
    EvalTraceHash policyDigest;
    std::optional<EvalTraceHash> graphDigest;
    SessionSourceDigest sourceIdentity;
    std::vector<SessionExternalRoot> externalRoots;
    SessionRecoveryKey stableRecoveryKey;

    SemanticSessionKey buildSemanticSessionKey(EvalTraceHashAlgorithm algorithm) const;
    SemanticSessionKey buildSemanticSessionKey() const;

    EvalTraceHash semanticSessionDigest() const { return buildSemanticSessionKey().digest; }
    EvalTraceHash semanticSessionDigest(EvalTraceHashAlgorithm algorithm) const
    { return buildSemanticSessionKey(algorithm).digest; }

    static SessionConfig forTest(EvalTraceHash policyDigest, std::string_view stableRecoveryKey = "");
};

/// Value that can be set at most once per lifetime. The invariant is
/// enforced in all build modes (not just debug). Throws nix::Error on
/// second `set()` so callers catching the project-wide `nix::Error`
/// base see a consistent failure type.
template<typename T>
class SetOnce {
    std::optional<T> inner;
public:
    void set(T value) {
        if (inner.has_value())
            throw Error("SetOnce: value already set");
        inner = std::move(value);
    }

    explicit operator bool() const noexcept { return inner.has_value(); }
    bool has_value() const noexcept { return inner.has_value(); }

    const T & operator*() const { return *inner; }
    const T * operator->() const { return &*inner; }
};

} // namespace nix::eval_trace
