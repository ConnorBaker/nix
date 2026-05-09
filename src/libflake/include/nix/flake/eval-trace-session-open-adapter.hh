#pragma once
///@file
///
/// Flake-local adapter for conversion from `LockedFlake` / `ResolvedFlakeGraph`
/// into eval-environment session-open inputs. This remains outside libexpr
/// because the graph types are flake-specific, but it centralizes the
/// extraction so callers do not duplicate the normalization logic.

#include "nix/expr/eval-environment/session-types.hh"
#include "nix/flake/flake.hh"

namespace nix::flake {

FlakeTraceSessionConfigRequest buildTraceSessionConfigRequest(
    const LockedFlake & lockedFlake,
    std::optional<Fingerprint> lockedFlakeFingerprint);

std::vector<FlakeGraphAuthorityNodeSpec> buildFlakeAuthorityNodeSpecs(
    const LockedFlake & lockedFlake);

} // namespace nix::flake
