#pragma once
/// store/verifier.hh — Async verification pipeline orchestrator.
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
/// Orchestrates direct typed verification and recovery calls by
/// delegating I/O to other services:
///   - `TraceStorage` (via the shared blocking pool) for trace
///     loading/publishing
///   - direct dep-resolution calls for dep hash computation
///
/// VerificationSession state is accessed from a single eval thread at
/// a time; concurrency safety comes from `syncAwait`'s future.get()
/// barrier, not from strand serialization (the original design used
/// boost::asio strands; those members were removed).

#include "../cache/prefetch-pool.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/strand-local.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/ids.hh"

#include "nix/expr/eval-trace/store/semantic-registry.hh"

#include <boost/asio/awaitable.hpp>

#include <optional>
#include <vector>

namespace nix {
class EvalState;
}

namespace nix::eval_trace {

namespace asio = boost::asio;

class BlockingThreadPool;

/// Async verification pipeline orchestrator.
///
/// VerificationSession state is accessed from a single eval thread at
/// a time; `syncAwait`'s future.get() provides the sequencing barrier
/// between the eval thread and the blocking pool workers.
///
/// The verifier delegates blocking store I/O to `SqliteTraceStorage` via the
/// shared blocking pool. Dep hash computation (`resolveDepHash`)
/// happens synchronously inside `verifyTrace` on the blocking pool.
///
class Verifier : private gdp::Certifier<VerificationAccessTag> {
public:
    /// Configuration for the verifier.
    struct Config {
        /// Maximum outstanding prefetch hints per eval coroutine.
        uint32_t maxPrefetchHints;
    };

    static constexpr Config defaultConfig() { return {16}; }

    Verifier(
        SqliteTraceStorage & store,
        BlockingThreadPool & blockingPool,
        Config config = defaultConfig());

    ~Verifier();

    // Non-copyable, non-movable (state ownership via raw pointers into
    // EvalState / SemanticRegistry that must not be aliased).
    Verifier(const Verifier &) = delete;
    Verifier & operator=(const Verifier &) = delete;

    /// Access the verification session (for testing only — production
    /// code should go through the typed verify/recovery interface).
    VerificationSession & sessionForTest() { return session_; }

    /// Bind per-session state used by subsequent verify/prefetch calls.
    void bindSession(const SemanticRegistry & registry, EvalState & state);

    /// Reset verification-only mutable state for a fresh logical session open.
    /// Keeps the bound registry/state pointers intact.
    void resetVerificationState();

    /// Verify an attr path using the currently bound session state.
    asio::awaitable<std::optional<SqliteTraceStorage::VerifyResult>>
    verifyAttr(AttrPathId pathId);

    /// Submit prefetch hints for a batch of sibling attr paths.
    void submitPrefetchHints(const std::vector<AttrPathId> & pathIds);

private:
    // ── State ───────────────────────────────────────────────────────

    SqliteTraceStorage & store_;
    BlockingThreadPool & blockingPool_;
    VerificationSession session_;
    Config config_;

    /// Bound per-session state. Set once via bindSession(), used by all
    /// subsequent verifyAttr/prefetch calls. Eliminates per-call reference
    /// parameters and the lifetime bugs they cause.
    const SemanticRegistry * registry_ = nullptr;
    EvalState * state_ = nullptr;

    StrandLocal<PrefetchPool, VerificationAccessTag> prefetchPool_;

    /// Internal implementation — shared by the public verify entry point.
    asio::awaitable<std::optional<SqliteTraceStorage::VerifyResult>> verifyAttrImpl(
        AttrPathId pathId);
};

} // namespace nix::eval_trace
