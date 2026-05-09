#pragma once
///@file
/// DepCaptureScope: shared dependency-capture RAII wrapper.
///
/// Uses the fiber's DepRecordingContext when available. When no fiber
/// context exists (tests, non-fiber paths), creates a standalone
/// DepRecordingContext as fallback with a thread_local pointer so
/// TraceAccess::current() can find it.

#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/util/gdp/proof.hh"

#include <memory>
#include <optional>

namespace nix::eval_trace {

// Forward declaration — defined in fiber-scheduler.hh.
DepRecordingContext * currentFiberDepCtx();

void appendActiveReplayCaptureDeps(std::vector<Dep> & deps);

/// Thread-local pointer to the active standalone DepRecordingContext.
/// Set by DepCaptureScope when no fiber context exists.
/// Used by TraceAccess::current() as a fallback.
///
/// StandaloneDepCtxGuard is the only mutation interface — the compiler
/// enforces save/set/restore structurally because the thread-local has
/// no public setter. Nested guards correctly save and restore the prior
/// value instead of unconditionally clearing to nullptr.
DepRecordingContext * currentStandaloneDepCtx();

/// RAII guard that sets a DepRecordingContext as the standalone context.
/// Saves the prior value in the constructor and restores it in the
/// destructor, so nesting is safe (outer scope is preserved).
struct StandaloneDepCtxGuard {
    explicit StandaloneDepCtxGuard(DepRecordingContext & ctx);
    ~StandaloneDepCtxGuard();
    StandaloneDepCtxGuard(const StandaloneDepCtxGuard &) = delete;
    StandaloneDepCtxGuard & operator=(const StandaloneDepCtxGuard &) = delete;
    StandaloneDepCtxGuard(StandaloneDepCtxGuard &&) = delete;
    StandaloneDepCtxGuard & operator=(StandaloneDepCtxGuard &&) = delete;
private:
    DepRecordingContext * prev_;
};

struct DepCaptureScope
    : private gdp::Certifier<DepRecordingContext::DepCaptureScopeTag>
{
    /// GDP-guarded mount resolution for dep provenance.
    /// Pointer to session's SemanticRegistry
    /// (production path via evaluateResolvedTarget).
    /// Null for test code that doesn't need mount resolution.
    const eval_trace::SemanticRegistry * registry = nullptr;

    /// The context to use for recording (fiber or standalone).
    DepRecordingContext * ctx = nullptr;
    /// Epoch log for the fallback standalone context. Declared before
    /// ownedCtx so it outlives the context (reverse destruction order).
    std::vector<Dep> fallbackEpochLog_;
    /// Standalone context (owned, created when no fiber context).
    std::unique_ptr<DepRecordingContext> ownedCtx;
    /// RAII guard for the standalone-ctx thread-local. Declared after
    /// ownedCtx so it is destroyed first, restoring the prior thread-
    /// local value before ownedCtx's storage is freed.
    std::optional<StandaloneDepCtxGuard> standaloneGuard;
    bool isRoot = false;
    bool ownsTraceFrame = false;
    bool scopePopped = false;

    /// Production constructor: with GDP-guarded mount resolution.
    explicit DepCaptureScope(InterningPools & pools, const eval_trace::SemanticRegistry & registry);
    /// Test constructor: no mount resolution (deps get <absolute> source).
    explicit DepCaptureScope(InterningPools & pools);
    ~DepCaptureScope();

    std::vector<Dep> finalizeAndTakeDeps()
    {
        std::vector<Dep> deps;
        Certifier::withProof([&](const auto & proof) {
            deps = ctx->takeDeps(proof);
            ctx->popScope(proof);
        });
        scopePopped = true;
        appendActiveReplayCaptureDeps(deps);
        return deps;
    }

    bool isStable() const
    {
        return ctx->isStable();
    }

    DepCaptureScope(const DepCaptureScope &) = delete;
    DepCaptureScope & operator=(const DepCaptureScope &) = delete;
};

// ── GDP guard for replayTrace ─────────────────────────────────────────
//
// replayTrace must only be called when a dep recording scope is active.
// Without an active scope, replayed deps go to the shared epoch log but
// no recording captures them — they change memoization behavior for
// later recordings, altering trace hashes (session 91 cold regression).
//
// RecordingScopeGuard is the ONLY entry point that mints an
// EpochLogWriteProof<ReplayActive>. It checks the fiber-local dep context
// and creates a proof only when recording is active. replayTrace requires
// the proof.

class RecordingScopeGuard {
public:
    /// Check if a recording scope is active and call f with a proof if so.
    /// This is the ONLY way to get an EpochLogWriteProof<ReplayActive>.
    // isActive() requires a non-empty scope stack.
    // Replaying deps into a scopeless context would pollute the epoch log
    // with unreachable child deps that no recording scope captures.
    // RecordingScopeGuard::ifActive gates replay writes to prevent this.
    template<typename F>
    static auto ifActive(F && f) {
        auto * ctx = currentFiberDepCtx();
        if (!ctx) ctx = currentStandaloneDepCtx();
        return EpochLogWriteCertifier::ifReplayActive(
            ctx && ctx->isActive(), std::forward<F>(f));
    }
};

} // namespace nix::eval_trace
