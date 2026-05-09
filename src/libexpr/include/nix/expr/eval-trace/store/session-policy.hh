#pragma once
/// session-policy.hh — Policy digest computation for session identity.
///
/// computePolicyDigest hashes the subset of EvalSettings that affects
/// evaluation results, producing the policy component of the 5-part
/// semantic session key.  Defined once here so flake.cc and
/// installable-attr-path.cc cannot diverge.
///
/// NOTE: Only the policyDigest is unified here.  The final session key
/// will differ between flake and file-eval paths because SessionConfig
/// construction diverges by design: the flake path omits externalRoots
/// while the file-eval path sets externalRoots = {repoRoot}.  This is
/// intentional — the two paths represent semantically distinct sessions.
///
/// NOTE: stableRecoveryKey is NOT computed here because it depends on
/// caller-specific source identity (flake original ref vs. logical file-eval
/// source identity). See SessionConfig construction in the eval-environment
/// flake and file-eval assembly paths.

#include "nix/expr/eval-trace/deps/types.hh"  // EvalTraceHash

namespace nix {
struct EvalSettings;
}

namespace nix::eval_trace {

/// Compute an eval-trace digest of the eval settings that affect cached results.
///
/// Fields hashed (in order, length-prefixed):
///   1. pureEval:                     "pure" | "impure"
///   2. restrictEval:                 "restricted" | "unrestricted"
///   3. currentSystem:                e.g. "x86_64-linux"
///   4. enableImportFromDerivation:   "ifd" | "no-ifd"
///   5. NIX_PATH env var (only when !pureEval): raw value or ""
///   6. nixPath config entries (only when !pureEval): --nix-path / nix-path
///   7. allowedUris entries (always): --allowed-uris / allowed-uris
///
/// This overload uses positional length-prefixed framing (length header +
/// bytes per field).  The production digest path uses the snapshot overload
/// in session-builders.cc, which uses CanonicalHashBuilder with tagged
/// fields; the two digests are NOT intended to be byte-equal.
///
/// Thread safety: safe to call concurrently as long as no other thread
/// calls putenv/setenv/unsetenv (Nix does not do this after startup).
/// If session opens ever become concurrent, NIX_PATH should be captured
/// once at EvalState construction time instead.
EvalTraceHash computePolicyDigest(const EvalSettings & settings);

} // namespace nix::eval_trace
