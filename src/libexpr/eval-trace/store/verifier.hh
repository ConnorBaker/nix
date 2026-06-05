#pragma once
/// store/verifier.hh — Synchronous verification pipeline.
///
/// Rearchitecture-proposal.md §14 step 7. Renamed from
/// `VerificationOrchestrator`. The verify/recovery phase functions
/// (`VerifyImpl`, `RecoveryState<Stage>`, `OriginScopeFactory`) now
/// live alongside this class in `verifier.cc` — the two files were
/// merged when `sqlite-trace-storage-verify.cc` was folded in. The
/// phase functions are still friends of `SqliteTraceStorage` (not
/// `Verifier` private helpers) because they reach the backend's
/// private helpers directly; moving them onto `Verifier` would
/// require the full §2.1 virtual surface on `TraceStorage`, which is
/// deferred until a second verification-capable backend materialises.
///
/// Drives direct typed verification and recovery calls:
///   - `SqliteTraceStorage` for trace loading/publishing
///   - direct dep-resolution calls for dep hash computation
///
/// VerificationSession state is accessed from a single eval thread at a
/// time; verification runs synchronously on the eval thread under the
/// store's `ExclusiveTraceStorageAccess` (the original async io_context /
/// strand orchestrator was removed — see context.cc `verify`).

#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/ids.hh"

#include "nix/expr/eval-trace/store/semantic-registry.hh"

#include <optional>
#include <vector>

namespace nix {
class EvalState;
}

namespace nix::eval_trace {

/// Synchronous verification pipeline.
///
/// VerificationSession state is accessed from a single eval thread at a
/// time; verification and dep hash computation (`resolveDepHash`) run
/// synchronously inside `verifyTrace` under the store's exclusive access.
///
class Verifier {
public:
    Verifier(SqliteTraceStorage & store);

    ~Verifier();

    // Non-copyable, non-movable (state ownership via raw pointers into
    // EvalState / SemanticRegistry that must not be aliased).
    Verifier(const Verifier &) = delete;
    Verifier & operator=(const Verifier &) = delete;

    /// Access the verification session (for testing only — production
    /// code should go through the typed verify/recovery interface).
    VerificationSession & sessionForTest() { return session_; }

    /// The production verify path (driven by `TraceBackend::verify` ->
    /// `verifySync`, context.cc). Caller (TraceBackend) supplies the
    /// exclusive-access capability (since `Verifier` doesn't inherit
    /// `Certifier<BlockingTag>`). Uses the bound `registry_`/`state_` (set via
    /// `bindSession`) and the existing `session_`. Returns nullopt if unbound.
    std::optional<SqliteTraceStorage::VerifyResult> verifyAttrSync(
        const ExclusiveTraceStorageAccess & ea, AttrPathId pathId);

    /// Bind per-session state used by subsequent verify/prefetch calls.
    void bindSession(const SemanticRegistry & registry, EvalState & state);

    /// H1b: populate the persisted file-content-hash cache from a trace's
    /// just-recorded deps, so the FIRST warm verify hits H1 instead of
    /// recomputing (cold recording does not run the verify path, so without
    /// this H1 only pays off from the 2nd warm verify). For each FileBytes/
    /// RawBytes dep on a store-resident Registered source (the same
    /// `h1StorePathKey` gate the verify path uses), stores
    /// (resolved-store-path, dep.hash). Sound: the dep's hash is already
    /// `depHash(readFile())` (the byte-identical value the verify path would
    /// compute), and the key is derived by the SAME function the verify path
    /// keys on — so a populated entry matches by construction. No-op if the
    /// session is unbound (no registry). Caller holds `ea`.
    void populateFileContentCacheFromRecordedDeps(
        const ExclusiveTraceStorageAccess & ea, const std::vector<Dep> & deps);

    /// Reset verification-only mutable state for a fresh logical session open.
    /// Keeps the bound registry/state pointers intact.
    void resetVerificationState();

private:
    // ── State ───────────────────────────────────────────────────────

    SqliteTraceStorage & store_;
    VerificationSession session_;

    /// Bound per-session state. Set once via bindSession(), used by all
    /// subsequent verifyAttrSync calls. Eliminates per-call reference
    /// parameters and the lifetime bugs they cause.
    const SemanticRegistry * registry_ = nullptr;
    EvalState * state_ = nullptr;

};

} // namespace nix::eval_trace
