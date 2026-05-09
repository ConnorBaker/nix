#pragma once
/// fiber-scheduler.hh — Eval task scheduler for eval-trace.
///
/// Manages per-task evaluation contexts (DepRecordingContext, TraceFrame).
/// Uses stackless coroutines (C++20 via asio::awaitable) for async I/O:
///
///   - The eval thread runs synchronously. When verification I/O is
///     needed, syncAwait (eval-context.hh) posts the coroutine to
///     io_context and polls inline until done.
///
///   - No boost::coroutine2 stacks — no 8MB allocations, no GC root
///     registration for fiber stacks.
///
///   - The outermost TracedExpr creates a task context (run()).
///     Nested TracedExprs detect insideTask() and skip context creation.

#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/util/finally.hh"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>

#include <cassert>
#include <exception>
#include <memory>
#include <thread>
#include <type_traits>

namespace nix::eval_trace {

struct SiblingReplayCaptureScope;
template<typename> class EvalContext;
struct Suspendable;
class FiberScheduler;        // forward declare for FiberThreadLocals
class SuspendableCtxScope;   // forward declare for FiberThreadLocals

/// Per-task evaluation context. Each task owns its own
/// DepRecordingContext for dependency isolation between tasks,
/// and its own TraceFrame (Lifetime 2 cache) for helper-facing
/// state (provenance, scanned bindings, etc.).
///
/// The TraceFrame's thread_local pointer is set/restored on task
/// entry/exit so TraceAccess::current() finds the right frame.
struct FiberEvalContext {
    DepRecordingContext depCtx;
    /// True only when this instance created fiberTraceFrameStorage.
    /// Nested FiberEvalContext instances that found a TraceFrame already
    /// active must NOT reset it on destruction — only the creator must.
    bool ownsTraceFrame = false;

    /// Construct with an explicit epoch log reference.
    /// Production callers pass MemoReplayStore::epochLog_.
    FiberEvalContext(InterningPools & pools, std::vector<Dep> & epochLog);
    ~FiberEvalContext();

    FiberEvalContext(const FiberEvalContext &) = delete;
    FiberEvalContext & operator=(const FiberEvalContext &) = delete;
};

/// Get the current task's DepRecordingContext (nullptr if not in a task).
DepRecordingContext * currentFiberDepCtx();

/// Standalone context for non-task paths (tests, DepCaptureScope fallback).
/// Mutation is only available via StandaloneDepCtxGuard (dep-capture-scope.hh):
/// no public setter exists, so callers can't accidentally bypass the
/// save/restore RAII shape.
DepRecordingContext * currentStandaloneDepCtx();

/// Compile-time thread-local registry for fiber context.
///
/// Bundles ALL fiber-context thread-locals so that save/restore
/// is structured. Adding a field without updating saveThreadLocals()
/// or restoreThreadLocals() is a compile error — both functions
/// reference every field via designated initializers / assignment.
struct FiberThreadLocals {
    bool insideTask;
    FiberScheduler * current;
    FiberEvalContext * currentEvalCtx;
    DepRecordingContext * standaloneDepCtx;
    SuspendableCtxScope * suspendableCtxScope;
    SiblingReplayCaptureScope * captureScope;
};

// Invariant: FiberScheduler::current() != nullptr on a thread ⟹
// onOwnerThread() on that thread.  run() is only called from the
// owner thread; violation is blocked at the TracedExpr::eval
// outermost-entry check.

/// Snapshot all fiber-context thread-locals into a struct.
FiberThreadLocals saveThreadLocals();

/// Restore all fiber-context thread-locals from a snapshot.
void restoreThreadLocals(const FiberThreadLocals & saved);

/// Eval task scheduler.
///
/// Runs on the eval thread. run() creates a FiberEvalContext for
/// dependency recording, calls the function directly (synchronous),
/// and restores the previous context on exit.
///
/// Async I/O (verification, dep resolution) is handled by syncAwait
/// in eval-context.hh, which polls the io_context inline on the eval
/// thread. No fiber stacks, no interleaving between tasks.
class FiberScheduler {
public:
    /// FiberScheduler must be constructed on the thread that will drive
    /// its run() loop — typically the eval thread.  The ctor captures
    /// std::this_thread::get_id() for thread-affinity checks in
    /// SuspendableCtxScope and EvalContext<Suspendable>.  If
    /// FiberScheduler construction ever moves to a different thread,
    /// every affinity check will abort on the legitimate eval thread.
    explicit FiberScheduler(boost::asio::io_context & ioc);

    /// Run func with a fresh eval context. Calls func directly
    /// (synchronous — no fiber, no coroutine). If pools and epochLog
    /// are non-null, a FiberEvalContext is created for dependency
    /// recording using the provided epoch log vector (typically
    /// MemoReplayStore::epochLog_).
    ///
    /// Nested TracedExpr::eval calls detect insideTask() and skip
    /// context creation — they run in the outermost task's context.
    template<typename F>
    auto run(F && func, InterningPools * pools = nullptr,
             std::vector<Dep> * epochLog = nullptr) -> std::invoke_result_t<F>;

    /// True if executing inside a managed task (run() is on the call stack).
    static bool insideTask() { return insideTask_; }

    /// Get the scheduler for the current task.
    static FiberScheduler * current() { return current_; }

    /// Get the current task's eval context.
    static FiberEvalContext * currentEvalCtx() { return currentEvalCtx_; }

    /// Get an executor for posting async work.
    boost::asio::any_io_executor executor() { return ioc_.get_executor(); }

    /// True if the calling thread is the thread that constructed this
    /// scheduler (= the eval thread, by construction).
    bool onOwnerThread() const noexcept {
        return std::this_thread::get_id() == ownerThreadId_;
    }

    /// The thread id captured at ctor time — the thread that owns this
    /// scheduler and drives its run() loop.
    std::thread::id ownerThreadId() const noexcept { return ownerThreadId_; }

private:
    boost::asio::io_context & ioc_;
    // Declaration order: ioc_ first so the init-list runs in
    // declaration order; ownerThreadId_ follows.
    const std::thread::id ownerThreadId_;

    static thread_local bool insideTask_;
    static thread_local FiberScheduler * current_;
    static thread_local FiberEvalContext * currentEvalCtx_;

    friend void restoreThreadLocals(const FiberThreadLocals &);
};

// ── Template implementation ──────────────────────────────────────────

template<typename F>
auto FiberScheduler::run(F && func, InterningPools * pools,
                         std::vector<Dep> * epochLog) -> std::invoke_result_t<F>
{
    using R = std::invoke_result_t<F>;

    auto saved = saveThreadLocals();
    Finally restore{[&] { restoreThreadLocals(saved); }};

    current_ = this;
    insideTask_ = true;

    // Create per-task DepRecordingContext if pools and epochLog provided.
    std::unique_ptr<FiberEvalContext> evalCtx;
    if (pools && epochLog) {
        evalCtx = std::make_unique<FiberEvalContext>(*pools, *epochLog);
    }
    currentEvalCtx_ = evalCtx.get();

    if constexpr (std::is_void_v<R>) {
        std::forward<F>(func)();
    } else {
        return std::forward<F>(func)();
    }
}

} // namespace nix::eval_trace
