#pragma once
/// verification-protocol.hh — Verification outcome and recovery-stage tags.
///
/// Bundle 4 removed the old in-process session-typed verification protocols.
/// The live runtime now uses direct typed method calls plus `Linear`
/// typestate for recovery staging. This header keeps the shared runtime
/// enums and stage tags that still need a stable public definition.

namespace nix::eval_trace {

// -- Verification outcome ----------------------------------------------------
//
// Classification of trace verification outcome. Shared between the
// protocol definition (Pass2Complete message) and the verification
// implementation (VerificationState::determineOutcome).

/**
 * Classification of trace verification outcome. Replaces the ad-hoc boolean
 * combination (allValid, hasContentFailure, hasImplicitShapeOnlyOverride).
 * -Wswitch ensures every consumer handles all cases.
 */
enum class VerifyOutcome {
    /** All deps match current state. No hash recomputation needed. */
    Valid,
    /** Content dep(s) failed but StructuredContent deps cover all failures.
     *  Value-aware: accessed scalars verified. No hash recomputation needed. */
    ValidViaStructuralOverride,
    /** Content dep(s) failed, covered by ImplicitShape-only (no SC coverage).
     *  Value-blind: key set unchanged but values may differ. Requires
     *  trace_hash recomputation so trace-context deps detect potential change. */
    ValidViaImplicitShapeOverride,
    /** Unrecoverable verification failure. */
    Invalid,
};

// -- Recovery typestate -------------------------------------------------------
//
// These tags define the stages of the `RecoveryState` linear typestate in
// trace-store-verify.cc. They remain public because the stage names are part
// of the subsystem's documented design vocabulary.

struct RecoveryUntried {};
struct RecoveryGitMissed {};
struct RecoveryDirectMissed {};

} // namespace nix::eval_trace
