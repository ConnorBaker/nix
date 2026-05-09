#pragma once
/// eval-context.hh — Mode-parameterized evaluation context.
///
/// EvalContext<Mode> is a capability token that controls whether code
/// can call syncAwait (blocking the eval thread to wait for an async
/// coroutine). The Mode phantom type parameter determines the
/// capability:
///
///   EvalContext<Suspendable>  — may call syncAwait (eval thread main stack)
///   EvalContext<Critical>     — must NOT call syncAwait (inside lock or handler)
///
/// Narrowing (Suspendable → Critical) is free and irreversible.
/// Widening (Critical → Suspendable) does not exist.
///
/// TYPE-LEVEL SAFETY:
///
/// The context stores an any_io_executor (not an io_context *).
/// any_io_executor can POST work (co_spawn) but cannot DRIVE the
/// event loop (run, run_one, poll). This makes it structurally
/// impossible for syncAwait to re-enter the io_context — the
/// method simply doesn't exist on the executor type. The eval
/// thread blocks on a std::future while worker threads drive the
/// io_context. This is the same principle as Haskell's STM monad:
/// the operations that would cause deadlock don't exist in the API.
///
/// See doc/design/eval-context-coloring.md for the full design.

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>

#include <concepts>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace nix {
class EvalState;
}

namespace nix::eval_trace {

/// Phantom type tag: this context may call syncAwait.
struct Suspendable {};

/// Phantom type tag: this context must NOT call syncAwait.
/// Used inside critical sections (lock scope, handler context).
struct Critical {};

/// Unconditional check — fires in all build modes including release.
/// Same pattern as nix::detail::unconditionalAbort (linear.hh).
[[noreturn]] inline void evalContextViolation(const char * msg) noexcept {
    std::fprintf(stderr, "eval context violation: %s\n", msg);
    std::abort();
}

// Forward declarations for the friend grants and parameter types on
// SuspendableCtxScope.  Canonical definitions live in fiber/fiber-
// scheduler.hh (FiberThreadLocals, saveThreadLocals, restoreThreadLocals,
// FiberScheduler).  These forward decls exist only so SuspendableCtxScope
// can friend the fiber save/restore pair and name FiberScheduler in its
// public ctor.
struct FiberThreadLocals;
FiberThreadLocals saveThreadLocals();
void restoreThreadLocals(const FiberThreadLocals &);
class FiberScheduler;
class SuspendableCtxScope;

template<typename Mode>
class EvalContext {
    EvalState & state_;

    /// Executor for posting async work. any_io_executor is type-erased
    /// but all construction paths use io_context::executor_type (from
    /// FiberScheduler::executor()). Strand executors would also work
    /// for co_spawn but are not used — coroBlock takes io_context &
    /// directly for timer completions, preventing strand deadlocks.
    ///
    /// Null (default-constructed) in Critical mode as defense-in-depth.
    boost::asio::any_io_executor executor_;

    /// Thread that owns this ctx (Suspendable only; default-
    /// constructed no-thread sentinel for Critical).  Captured at
    /// ctor time from the FiberScheduler's owner thread id and
    /// consulted by thread-sensitive operations (syncAwait) to
    /// detect ctx smuggling across threads.
    std::thread::id ownerThreadId_;

    template<typename> friend class EvalContext;
    // Friendship used only for placement-new access to the private
    // Suspendable ctor in SuspendableCtxScope's ctor body.  No other
    // private-state access is intended; the narrow use is deliberate
    // (see the "RAII-only thread-local scope types" Closure note in
    // src/libexpr/eval-trace/CLAUDE.md for the pass-key vs friend
    // trade-off analysis).
    friend class SuspendableCtxScope;

    /// Suspendable ctor: private.  Only SuspendableCtxScope mints
    /// these via placement-new in its ctor body.
    EvalContext(EvalState & state,
                boost::asio::any_io_executor executor,
                std::thread::id ownerThreadId) noexcept
        requires std::same_as<Mode, Suspendable>
        : state_(state), executor_(std::move(executor)),
          ownerThreadId_(ownerThreadId) {}

    /// Precondition check shared by both syncAwait overloads.
    /// Pattern: thread-sensitive EvalContext<Suspendable> operations
    /// begin with this check (or an equivalent ownerThreadId_
    /// comparison).  Today the population is {syncAwait}; future
    /// additions must apply the same pattern.
    void checkSyncAwaitPreconditions() const noexcept
        requires std::same_as<Mode, Suspendable>
    {
        if (!executor_) [[unlikely]]
            evalContextViolation(
                "eval-trace/dispatch: syncAwait called without executor");
        if (std::this_thread::get_id() != ownerThreadId_) [[unlikely]]
            evalContextViolation(
                "eval-trace/dispatch: syncAwait called from non-owner "
                "thread (EvalContext<Suspendable> smuggled across threads)");
    }

public:
    /// Construct without an io_context. Only valid for Critical contexts.
    /// executor_ and ownerThreadId_ are explicitly default-initialized
    /// as defense-in-depth — Critical contexts never call syncAwait, so
    /// neither is consulted, but zero-init keeps the fields in a
    /// well-defined state.
    explicit EvalContext(EvalState & state) noexcept
        requires std::same_as<Mode, Critical>
        : state_(state), executor_{}, ownerThreadId_{} {}

    EvalContext(const EvalContext &) = delete;
    EvalContext(EvalContext &&) = delete;
    EvalContext & operator=(const EvalContext &) = delete;
    EvalContext & operator=(EvalContext &&) = delete;

    EvalState & state() { return state_; }
    const EvalState & state() const { return state_; }

    /// The thread on which this Suspendable ctx was minted.  Used by
    /// SuspendableCtxScope's adopt-path consistency check.
    std::thread::id ownerThreadId() const noexcept
        requires std::same_as<Mode, Suspendable>
    { return ownerThreadId_; }

    /// Executor reference, exposed only for scope-level identity
    /// checks in SuspendableCtxScope's adopt-path consistency check.
    /// Callers that want to post work should go through syncAwait.
    const boost::asio::any_io_executor & executor() const noexcept
        requires std::same_as<Mode, Suspendable>
    { return executor_; }

    // ── Suspension ──────────────────────────────────────────────

    /// Block the eval thread until the coroutine completes.
    ///
    /// Posts the coroutine to the executor. Worker threads drive the
    /// io_context (non-blocking handlers only — blocking I/O is in
    /// BlockingThreadPool via coroBlock). The eval thread blocks on
    /// future.get() until the completion handler fires.
    template<typename T>
    T syncAwait(boost::asio::awaitable<T> coro)
        requires (std::same_as<Mode, Suspendable> && !std::is_void_v<T>)
    {
        checkSyncAwaitPreconditions();

        std::promise<T> promise;
        auto future = promise.get_future();

        boost::asio::co_spawn(
            executor_,
            std::move(coro),
            [&promise](std::exception_ptr ep, T val) {
                if (ep)
                    promise.set_exception(ep);
                else
                    promise.set_value(std::move(val));
            });

        return future.get();
    }

    /// void specialization.
    void syncAwait(boost::asio::awaitable<void> coro)
        requires std::same_as<Mode, Suspendable>
    {
        checkSyncAwaitPreconditions();

        std::promise<void> promise;
        auto future = promise.get_future();

        boost::asio::co_spawn(
            executor_,
            std::move(coro),
            [&promise](std::exception_ptr ep) {
                if (ep)
                    promise.set_exception(ep);
                else
                    promise.set_value();
            });

        future.get();
    }

    // ── Narrowing ───────────────────────────────────────────────

    /// Narrow this context to Critical mode. The returned context
    /// cannot call syncAwait. Irreversible, zero-cost.
    ///
    /// The executor is default-constructed (null) as defense-in-depth.
    EvalContext<Critical> critical() const
        requires std::same_as<Mode, Suspendable>
    {
        return EvalContext<Critical>(state_);
    }

    // ── Critical sections ───────────────────────────────────────

    /// Lock a mutex and execute body with a Critical context.
    /// The body cannot call syncAwait — compile error.
    template<typename Mutex, typename F>
    auto withLock(Mutex & mtx, F && body)
        -> std::invoke_result_t<F, EvalContext<Critical> &>
    {
        std::lock_guard guard(mtx);
        if constexpr (std::same_as<Mode, Critical>) {
            return std::forward<F>(body)(*this);
        } else {
            auto crit = critical();
            return std::forward<F>(body)(crit);
        }
    }
};

// ── SuspendableCtxScope: RAII thread-local bridge ─────────────────
//
// The colored verify() interface requires an EvalContext<Suspendable>,
// but TracedExpr::eval is reached through the uncolored virtual
// Expr::eval(EvalState&, Env&, Value&) interface.  SuspendableCtxScope
// bridges the gap:
//
//   * Ownership.  SuspendableCtxScope owns the EvalContext<Suspendable>
//     via placement-new into aligned storage.  The ctx's ctor is
//     private; SuspendableCtxScope is friended and is the only minter.
//   * Tracking.  A private thread_local pointer (current_) tracks the
//     innermost scope on the current thread's stack.  Writes are
//     confined to the scope's ctor/dtor and the fiber snapshot/restore
//     pair; current_ has no public setter.
//   * Ref-implies-scope.  Because the ctx's ctor is private, any
//     EvalContext<Suspendable>& in the codebase comes from some
//     SuspendableCtxScope::ctx() call, which means the scope is live
//     somewhere on this thread's stack.
//
// Four abort points via evalContextViolation close the OR-9 worker-
// thread scenario:
//   * TracedExpr::eval outermost entry: off-owner-thread dispatch.
//   * SuspendableCtxScope ctor: direct off-owner-thread construction.
//   * SuspendableCtxScope adopt path: nested across mismatched state,
//     scheduler thread id, or executor.
//   * EvalContext<Suspendable>::syncAwait: ctx smuggled across threads.
//
// See the "RAII-only thread-local scope types" Closure note in
// src/libexpr/eval-trace/CLAUDE.md for the complete enforcement model.

class SuspendableCtxScope {
    // Raw placement-new buffer.  Intentionally left uninitialized —
    // the adopt and mint ctor paths both write it (mint: placement-new
    // into `storage_`; adopt: never touches `storage_`, `owned_` stays
    // null).  Zero-initializing here would be wasted work on the hot
    // mint path.  NOLINT: cppcoreguidelines-pro-type-member-init.
    alignas(EvalContext<Suspendable>) std::byte storage_[sizeof(EvalContext<Suspendable>)];  // NOLINT(cppcoreguidelines-pro-type-member-init)
    EvalContext<Suspendable> * owned_ = nullptr;   // non-null iff minted
    EvalContext<Suspendable> * ctx_   = nullptr;   // owned or adopted
    SuspendableCtxScope * prev_;

    static thread_local SuspendableCtxScope * current_;

    // Snapshot/restore is the structured save/restore path used by
    // FiberScheduler::run via FiberThreadLocals.  Narrow grants:
    // one read, one write.
    friend FiberThreadLocals saveThreadLocals();
    friend void restoreThreadLocals(const FiberThreadLocals &);

    static SuspendableCtxScope * snapshotPointer() noexcept { return current_; }
    static void restorePointer(SuspendableCtxScope * s) noexcept { current_ = s; }

public:
    /// Construct a scope on the scheduler's owner thread.
    ///   - Mint path (no enclosing scope): placement-new an
    ///     EvalContext<Suspendable> into storage_, set current_ = this.
    ///   - Adopt path (enclosing scope exists): share the enclosing
    ///     ctx after a consistency check (same state, same
    ///     ownerThreadId, same executor); make no thread-local
    ///     change.  state and fiber are used only for the
    ///     consistency check on this path.
    /// Aborts via evalContextViolation on:
    ///   - Construction off the scheduler's owner thread.
    ///   - Adopt across mismatched state, scheduler thread id, or
    ///     executor.
    SuspendableCtxScope(EvalState & state, FiberScheduler & fiber);
    ~SuspendableCtxScope();
    SuspendableCtxScope(const SuspendableCtxScope &)             = delete;
    SuspendableCtxScope(SuspendableCtxScope &&)                  = delete;
    SuspendableCtxScope & operator=(const SuspendableCtxScope &) = delete;
    SuspendableCtxScope & operator=(SuspendableCtxScope &&)      = delete;

    /// The returned reference is tied to *this on the mint path
    /// (into storage_), or to the enclosing scope on the adopt path
    /// (which outlives *this by LIFO).  [[clang::lifetimebound]]
    /// catches escape patterns; the adopt-path annotation is
    /// conservatively tight — false warnings on valid adopt-path
    /// uses after *this's destruction are rare and almost always
    /// indicate a genuine dangle.
    EvalContext<Suspendable> & ctx() noexcept [[clang::lifetimebound]]
    { return *ctx_; }

    /// Innermost active scope on this thread, or nullptr.  Debug /
    /// introspection only; no production callers today.  Production
    /// adopt-vs-mint decision happens inside the ctor via current_.
    /// Exercised by the Innermost_TracksScope unit test.
    static SuspendableCtxScope * innermost() noexcept { return current_; }
};

} // namespace nix::eval_trace
