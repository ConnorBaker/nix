#pragma once
///@file

#include "nix/expr/eval-hash.hh"
#include "nix/util/hash.hh"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace nix {

/**
 * Captures all inputs that affect evaluation semantics.
 *
 * This fingerprint is used as part of persistent cache keys to ensure
 * that cached results are only reused when all relevant configuration
 * matches. Two evaluations with different EvalInputs MUST NOT share
 * cached results.
 *
 * See Decision 6 in the hash-consing plan for rationale.
 */
struct EvalInputs
{
    /**
     * Nix version string (affects builtin behavior).
     */
    std::string nixVersion;

    /**
     * Whether pure evaluation mode is enabled.
     * Affects: builtins.currentTime, builtins.getEnv, etc.
     */
    bool pureEval = false;

    /**
     * Whether --impure flag was passed.
     * Allows impure operations even in flake evaluation.
     */
    bool impureMode = false;

    /**
     * Whether import-from-derivation is allowed.
     */
    bool allowImportFromDerivation = true;

    /**
     * Whether restrict-eval is enabled.
     */
    bool restrictEval = false;

    /**
     * The resolved NIX_PATH / nix-path setting.
     * Affects: <nixpkgs>, builtins.nixPath
     */
    std::vector<std::string> nixPath;

    /**
     * The eval-system / current system for builtins.currentSystem.
     */
    std::string currentSystem;

    /**
     * Flake lock file hash (if evaluating a flake).
     * Captures the entire locked dependency tree.
     */
    std::optional<Hash> flakeLockHash;

    /**
     * Allowed URIs for network access (restrict-eval).
     */
    std::set<std::string> allowedUris;

    /**
     * Root accessor fingerprint for source path stability.
     * Two different checkouts of the same content should have the same fingerprint.
     */
    std::optional<Hash> rootAccessorFingerprint;

    /**
     * Compute a content hash of all inputs.
     *
     * Two EvalInputs with the same fingerprint are semantically equivalent
     * for caching purposes.
     */
    ContentHash fingerprint() const;

    /**
     * Create EvalInputs from an EvalState and its settings.
     * This captures the current configuration at a point in time.
     */
    static EvalInputs fromSettings(
        const std::string & nixVersion,
        bool pureEval,
        bool restrictEval,
        bool impureMode,
        bool allowImportFromDerivation,
        const std::vector<std::string> & nixPath,
        const std::string & currentSystem,
        const std::set<std::string> & allowedUris,
        const std::optional<Hash> & flakeLockHash = std::nullopt,
        const std::optional<Hash> & rootAccessorFingerprint = std::nullopt);
};

} // namespace nix
