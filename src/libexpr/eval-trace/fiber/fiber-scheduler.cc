/// fiber-scheduler.cc — Eval task scheduler implementation.

#include "fiber-scheduler.hh"
#include "nix/expr/eval-trace/eval-context.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/trace-frame.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"

namespace nix::eval_trace {

thread_local bool FiberScheduler::insideTask_ = false;
thread_local FiberScheduler * FiberScheduler::current_ = nullptr;
thread_local FiberEvalContext * FiberScheduler::currentEvalCtx_ = nullptr;

DepRecordingContext * currentFiberDepCtx()
{
    auto * ctx = FiberScheduler::currentEvalCtx();
    return ctx ? &ctx->depCtx : nullptr;
}

// ── Thread-locals for the uncolored→colored bridge ───────────────────
//
// The Suspendable thread-local lives inside SuspendableCtxScope as a
// private static thread_local member (current_).  Mutation is via the
// scope's ctor/dtor + the narrow snapshotPointer/restorePointer
// friend grant (used by save/restoreThreadLocals for structured
// save/restore across fiber task boundaries).  There is no file-
// static for the Suspendable ctx; placement-new into the scope's
// own aligned storage ensures the ctx's lifetime is tied to the
// scope's lifetime.
//
// The Standalone slot remains file-static here — it's written via
// StandaloneDepCtxGuard's ctor/dtor (no public setter) and bulk-
// restored via save/restoreThreadLocals, matching the structured
// contract enforced by FiberThreadLocals's designated-initializer
// coverage.

static thread_local DepRecordingContext * standaloneDepCtx_ = nullptr;

DepRecordingContext * currentStandaloneDepCtx()
{
    return standaloneDepCtx_;
}

// SuspendableCtxScope's thread-local.  Private; written only by
// the scope's ctor, dtor, and restorePointer().
thread_local SuspendableCtxScope * SuspendableCtxScope::current_ = nullptr;

// `storage_` is a raw placement-new buffer — zero-init would waste
// cycles on the hot mint path, and the adopt path never touches it.
// See the member declaration in `eval-context.hh`.
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
SuspendableCtxScope::SuspendableCtxScope(EvalState & state, FiberScheduler & fiber)
    : prev_(current_)
{
    // Thread-affinity check at construction.  Redundant with
    // TracedExpr::eval's outermost-entry check for the production
    // call path, but guards any direct-construction path (including
    // tests) against off-owner-thread misuse.
    if (std::this_thread::get_id() != fiber.ownerThreadId()) [[unlikely]]
        evalContextViolation(
            "eval-trace/dispatch: SuspendableCtxScope constructed off "
            "the scheduler's owner thread");

    if (prev_) {
        // Adopt path: enclosing scope's ctx must belong to the same
        // scheduler.  Identity by state + ownerThreadId + executor.
        if (&prev_->ctx_->state() != &state ||
            prev_->ctx_->ownerThreadId() != fiber.ownerThreadId() ||
            prev_->ctx_->executor() != fiber.executor()) [[unlikely]]
            evalContextViolation(
                "eval-trace/dispatch: SuspendableCtxScope nested with "
                "mismatched state, scheduler thread id, or executor");
        ctx_ = prev_->ctx_;
    } else {
        // Mint path: placement-new invokes EvalContext<Suspendable>'s
        // private ctor.  SuspendableCtxScope is friended, so the call
        // here has private-member access.
        owned_ = ::new (static_cast<void *>(storage_))
            EvalContext<Suspendable>(
                state, fiber.executor(), fiber.ownerThreadId());
        ctx_ = owned_;
    }
    current_ = this;
}

SuspendableCtxScope::~SuspendableCtxScope()
{
    // Restore current_ before destroying the ctx: if the ctx's dtor
    // ever reads current_, it sees the restored outer state, not
    // a self-pointer about to dangle.  (Today's dtor doesn't read;
    // this order is defense for future changes.)
    current_ = prev_;
    if (owned_) owned_->~EvalContext<Suspendable>();
}

StandaloneDepCtxGuard::StandaloneDepCtxGuard(DepRecordingContext & ctx)
    : prev_(standaloneDepCtx_)
{
    standaloneDepCtx_ = &ctx;
}

StandaloneDepCtxGuard::~StandaloneDepCtxGuard()
{
    standaloneDepCtx_ = prev_;
}

// ── FiberThreadLocals save/restore ───────────────────────────────────
//
// Structured save/restore for ALL fiber-context thread-locals.
// Adding a field to FiberThreadLocals without updating both functions
// is a compile error: designated initializers reference every field.

FiberThreadLocals saveThreadLocals()
{
    return FiberThreadLocals{
        .insideTask = FiberScheduler::insideTask(),
        .current = FiberScheduler::current(),
        .currentEvalCtx = FiberScheduler::currentEvalCtx(),
        .standaloneDepCtx = standaloneDepCtx_,
        .suspendableCtxScope = SuspendableCtxScope::snapshotPointer(),
        .captureScope = snapshotReplayCaptureScope(),
    };
}

void restoreThreadLocals(const FiberThreadLocals & saved)
{
    FiberScheduler::insideTask_ = saved.insideTask;
    FiberScheduler::current_ = saved.current;
    FiberScheduler::currentEvalCtx_ = saved.currentEvalCtx;
    standaloneDepCtx_ = saved.standaloneDepCtx;
    SuspendableCtxScope::restorePointer(saved.suspendableCtxScope);
    restoreReplayCaptureScope(saved.captureScope);
}

// ── FiberEvalContext ──────────────────────────────────────────────────

/// Thread-local storage for the task-owned TraceFrame.
/// Each FiberEvalContext creates a TraceFrame and sets this pointer.
/// TraceFrame::currentForAccess() reads it (via the existing thread_local).
struct FiberTraceFrameScope {
    TraceFrame * previous;
    TraceFrame frame;

    FiberTraceFrameScope()
        : previous(TraceFrame::swapCurrent(&frame))
    {
    }

    ~FiberTraceFrameScope()
    {
        TraceFrame::swapCurrent(previous);
    }
};

/// Per-task TraceFrame storage.
static thread_local std::optional<FiberTraceFrameScope> fiberTraceFrameStorage;

FiberEvalContext::FiberEvalContext(InterningPools & pools, std::vector<Dep> & epochLog)
    : depCtx(pools, epochLog)
{
    if (!TraceFrame::currentForAccess()) {
        fiberTraceFrameStorage.emplace();
        ownsTraceFrame = true;
    }
}

FiberEvalContext::~FiberEvalContext()
{
    if (ownsTraceFrame)
        fiberTraceFrameStorage.reset();
}

// ── FiberScheduler ───────────────────────────────────────────────────

FiberScheduler::FiberScheduler(boost::asio::io_context & ioc)
    : ioc_(ioc), ownerThreadId_(std::this_thread::get_id())
{
}

} // namespace nix::eval_trace
