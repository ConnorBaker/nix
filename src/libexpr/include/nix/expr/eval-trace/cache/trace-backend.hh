#pragma once
///@file

#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/eval-context.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/source-path.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nix {
class EvalState;
class Store;
}

namespace nix::eval_trace {

// Flat-owned async infrastructure — declared in trace-backend.cc.
// Forward-declared here so public callers don't need the io_context /
// thread-pool includes.
struct BackendAsyncInfra;

class Verifier;

// ── Trace backend ─────────────────────────────────────────────────

using TraceInputAccessors = boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash>;

/// Rearchitecture-proposal.md §2.4.  Concrete trace backend backed by
/// `TraceStorage`.
///
/// TraceSession owns one `TraceBackend` when a cache is active; when no
/// cache is active, TraceSession's pointer is null and all dispatches
/// at call sites bypass the backend.
///
/// Async infrastructure (io_context, worker pools, scheduler,
/// BlockingThreadPool, Verifier) is owned directly by this class via
/// a single `BackendAsyncInfra` member (declared in trace-backend.cc
/// so the header doesn't leak the asio headers).  Previously split
/// across `StoreTraceBackend` and a nested `AsyncRuntime` struct; the
/// split was a §14 step 6 TODO, absorbed here.
class TraceBackend final : private gdp::Certifier<BlockingTag>
{
    std::shared_ptr<SqliteTraceStorage> store;

    /// Flat-owned async + verifier infrastructure.  Constructed +
    /// destroyed in the out-of-line ctor/dtor in context.cc (where
    /// `BackendAsyncInfra` is defined) so the teardown sequence
    /// (blocking pool stopped before io_context workers joined) stays
    /// structural.
    std::unique_ptr<BackendAsyncInfra> infra_;

    /// Whether bindSession() has been called for this backend instance.
    bool sessionBound_ = false;

public:
    /// Result of runtime root verification at session open.
    struct RuntimeRootResult {
        /// Canonical typed result of runtime-root verification.
        std::vector<SqliteTraceStorage::VerifiedRuntimeRootRecord> verifiedRoots;
        /// Total expected count of runtime roots.
        size_t expectedCount = 0;
        /// Malformed persisted rows ignored during load.
        size_t rejectedCount = 0;
    };

    explicit TraceBackend(std::shared_ptr<SqliteTraceStorage> store, InterningPools & pools);
    ~TraceBackend();

    TraceBackend(const TraceBackend &) = delete;
    TraceBackend & operator=(const TraceBackend &) = delete;

    /// Get the fiber scheduler for this backend.
    /// Used by TracedExpr::eval() to create a long-lived fiber that
    /// wraps the entire evaluation subtree — all nested verify() calls
    /// yield the same fiber instead of creating one per call.
    class FiberScheduler * getScheduler();

    /// Verify a cached trace for the given attr path.
    ///
    /// Requires EvalContext<Suspendable> — proof that the caller is on
    /// the eval thread's main stack and may block via syncAwait.
    /// Code inside handlers, strand coroutines, or lock bodies does not
    /// have a Suspendable context and cannot call this method.
    std::optional<SqliteTraceStorage::VerifyResult> verify(
        EvalContext<Suspendable> & ctx, AttrPathId pathId);

    std::optional<SqliteTraceStorage::RecordResult> record(
        EvalContext<Suspendable> & ctx,
        AttrPathId pathId,
        const CachedResult & value,
        const std::vector<Dep> & allDeps);

    std::shared_ptr<const std::vector<Dep>> loadFullTrace(
        EvalContext<Suspendable> & ctx, TraceId traceId);

    /// getCurrentTraceHash takes EvalContext<Suspendable> & — ctx is
    /// carried by the active SiblingReplayCaptureScope (the only caller
    /// context where this method is reachable).
    std::optional<TraceHash> getCurrentTraceHash(
        EvalContext<Suspendable> & ctx, AttrPathId pathId);

    /// Flush any pending backend state (durable writes).
    /// Safe to call from any thread without an EvalContext.
    void flush();

    /// Submit prefetch hints for speculative verification of sibling attrs.
    /// Called from materialization after creating child TracedExpr thunks.
    void submitPrefetchHints(const std::vector<AttrPathId> & pathIds);

    /// Bind the per-session eval state to the verifier.
    /// Called once after construction when the TraceSession is set up.
    void bindSession(
        const SemanticRegistry & registry, EvalState & state);

    /// Set the session fingerprint on the backing store.
    /// Called once after construction when a fingerprint is available.
    void setSessionConfig(SessionConfig config);

    /// Persist a runtime root identity as session metadata.
    /// Called from fetchTree when a locked input is fetched during cold eval.
    void recordRuntimeRoot(
        const SqliteTraceStorage::RuntimeRootRecord & record,
        Store & store);

    /// Load runtime roots from session metadata and verify their narHashes.
    /// Returns verified entries, expected count, and any store paths that were
    /// confirmed missing.
    RuntimeRootResult loadAndVerifyRuntimeRoots(EvalState & state);

    // ── Read-through accessors (replace raw-store reach-through in callers) ─
    //
    // `nix eval-info` and similar diagnostic tools route through these
    // instead of reaching a raw `SqliteTraceStorage &`. A future
    // `unique_ptr<TraceStorage>` ownership switch then won't require every
    // caller to change.

    /// Semantic session key of the bound trace storage.
    /// Forwards to the base-owned accessor on `TraceStorage`. Inexpensive;
    /// no synchronization needed (the field is read-only after
    /// `setSessionConfig`).
    [[nodiscard]] const SemanticSessionKey & currentSemanticSessionKey() const noexcept;

    /// Query the cached evaluation record for an attr path without
    /// invoking verification. Thin forward to
    /// `SqliteTraceStorage::queryEvalInfoExclusive`.
    [[nodiscard]] std::optional<SqliteTraceStorage::EvalInfoRecord> queryEvalInfo(
        AttrPathId pathId, bool allowHistoryFallback);

    /// Raw-row loader for `nix eval-info` diagnostics. Unlike
    /// `loadAndVerifyRuntimeRoots`, this does NOT verify narHashes or
    /// materialize source paths — callers get the persisted entries
    /// verbatim so the CLI can render rejected-count diagnostics.
    [[nodiscard]] SqliteTraceStorage::RuntimeRootLoadResult loadRuntimeRoots(Store & storeDirConfig);
};

} // namespace nix::eval_trace
