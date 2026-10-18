#pragma once
///@file

#include <functional>
#include <queue>
#include <map>
#include <vector>
#include <memory>
#include <atomic>
#include <random>
#include <condition_variable>
#include <mutex>

#include <pthread.h>

#include "nix/util/sync.hh"
#include "nix/util/logging.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/util.hh"
#include "nix/util/signals.hh"

namespace nix {

struct EvalSettings;
struct EvalState;
struct WaveScope;

/**
 * Worker-thread stack size (parallel-eval line, inc2b-1).
 *
 * The main evaluator thread bumps its RLIMIT_STACK to 60 MiB in the
 * `EvalState` constructor (`ensureStackSizeAtLeast`). Worker threads do not
 * inherit that via rlimit reliably, so we size each worker's stack explicitly
 * so a worker overflows at (at least) the same depth as the main thread --
 * i.e. a worker never overflows *sooner* than base (the parallel-line
 * stack-parity contract). 64 MiB >= the main thread's 60 MiB.
 */
static constexpr size_t evalStackSize = 64 * 1024 * 1024;

/**
 * A fixed pool of worker threads that evaluate Nix thunks concurrently.
 *
 * Ported (inc2b-1) from DetSys's `parallel-eval.{hh,cc}`
 * (origin/parallel-eval-improvements @ 3d3f2810a), slimmed to the machinery
 * our base already supports. At `eval-cores = 1` (the default, and the inc2b-1
 * gate) the pool holds a single worker and `enabled` is false, so
 * `builtins.__parallel` never dispatches work -- the evaluator stays
 * effectively serial and the concurrent-forcing protocol (blackhole CAS +
 * release-exchange publish + wait/notify) is present but unexercised.
 */
struct Executor
{
    using work_t = std::function<void()>;

    /**
     * Completion state shared between a set of work items and a
     * `FutureVector` waiting for them. This is used instead of a
     * `std::promise`/`std::future` per work item, since fulfilling a
     * promise does an unconditional futex wake syscall (and its shared
     * state is a heap allocation), which is wasteful when spawning
     * hundreds of thousands of work items whose completion is only
     * ever awaited in aggregate.
     */
    struct Completion
    {
        std::atomic<uint64_t> pending{0};

        /* Inc3 S4-a: the wave this completion belongs to, or null. The sole
           channel that carries the wave identity across the spawn / pick-up
           boundary: `Executor::spawn` already threads this completion into every
           queued `Item`, so a worker picking up the item recovers the wave as
           `item.completion->wave` and makes it ambient (`currentWave`) for the
           item's duration. Set once when the wave's completion is created
           (`WaveScope`'s constructor) and never mutated after, so no separate
           `Item::wave` that each producer would have to remember to stamp. */
        WaveScope * wave = nullptr;

        struct State
        {
            /**
             * Exceptions thrown by work items.
             */
            std::vector<std::exception_ptr> exceptions;
        };

        Sync<State> state_;

        std::condition_variable cv;

        /**
         * Record the completion of one work item, waking up any
         * thread waiting for the pending count to drop to zero.
         */
        void finish(std::exception_ptr ex)
        {
            if (ex)
                state_.lock()->exceptions.push_back(std::move(ex));
            if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                /* Take the lock to make sure that a waiter that has
                   seen a non-zero pending count has started waiting on
                   the condition variable before we notify it. */
                auto state(state_.lock());
                cv.notify_all();
            }
        }
    };

    struct Item
    {
        work_t work;
        std::shared_ptr<Completion> completion;
    };

    struct State
    {
        std::multimap<uint64_t, Item> queue;
        /* Worker threads, created with an explicit stack size (`evalStackSize`)
           via pthreads so a worker overflows no sooner than the main thread
           (the parallel-line stack-parity contract). pthreads is used directly
           rather than boost::thread to avoid a new linked boost component. */
        std::vector<pthread_t> threads;
    };

    std::atomic_bool quit{false};

    const unsigned int evalCores;

    const bool enabled;

    /* inc2c-2/2c-3: the EvalState that owns this executor (1:1). Set in the
       constructor BEFORE workers are spawned (2c-3), so a worker reading it at
       entry to open its per-worker unforce scope never sees a null. Used by
       `worker()` to open that scope when unforcing runs under threads. */
    EvalState * evalState = nullptr;

    const std::unique_ptr<InterruptCallback> interruptCallback;

    Sync<State> state_;

    std::condition_variable wakeup;

    /* Inc3 S4-b-a0 (F1): the GLOBAL outstanding-work count across the whole pool
       -- the self-enforcing executor-idle guard. Incremented at the ONE spawn
       chokepoint (`Executor::spawn`, by items.size(), before queueing) and
       decremented when a work item finishes (the worker loop, after `work()`)
       OR is dropped unrun on teardown -- for EVERY item, fire-and-forget or
       completion-bearing. The (S4-b-b) wave barrier blocks on `awaitExecutorIdle`
       (global == 0) IN ADDITION to the wave's own `pending`, so ANY spawn --
       including a future producer that forgot to register into a wave -- is
       awaited before a stripe is freed. SPAWN-SITE + COMPLETION ONLY: nothing on
       the inlined force path (forceValue/allocValue/ownerOf/youngW/ownerOfW)
       touches it. Nothing blocks on it in a0; it is infra + a unit test. */
    std::atomic<uint64_t> globalOutstanding{0};
    std::mutex idleMutex;        /* guards the idle-cv notify (no lost wakeup) */
    std::condition_variable idleCv;

    static unsigned int getEvalCores(const EvalSettings & evalSettings);

    Executor(EvalState & evalState);

    ~Executor();

    void createWorker(State & state);

    void worker();

    using WorkItems = std::vector<std::pair<Executor::work_t, uint8_t>>;

    /**
     * Queue the given work items for execution. If `completion` is
     * non-null, it is notified as the items finish; otherwise the
     * items are fire-and-forget.
     */
    void spawn(WorkItems && items, std::shared_ptr<Completion> completion = {});

    /* Inc3 S4-b-a0 (F1): the global executor-idle query + wait primitive the
       wave barrier will block on. `outstandingWork() == 0` means the pool is
       quiescent (no item queued or running). `awaitExecutorIdle()` blocks until
       then. Nothing in a0 calls `awaitExecutorIdle` on a live path -- it is the
       infra S4-b-a1's barrier uses. */
    uint64_t outstandingWork() const
    {
        return globalOutstanding.load(std::memory_order_acquire);
    }
    void awaitExecutorIdle();

    /* Inc3 S4-b-a0 (F1): release `n` items from the global count (decrement),
       signalling idle if it reaches zero. Called from the worker loop (1 per
       finished item; `n` for items dropped unrun on teardown). */
    void releaseOutstanding(uint64_t n) noexcept;

    /* Wake every blackhole waiter domain (the teardown pattern). Factored out so
       `~Executor`, the SIGINT interrupt callback, AND the Inc3 wave cancel
       (§2.5, `WaveScope::requestCancel`) share one implementation: a worker
       parked in `waitOnThunk` on a cell whose producer was cancelled re-checks
       and observes the cancel at its next `checkInterrupt` poll. */
    void wakeAllWaiterDomains() noexcept;

    /* constinit so cross-TU users (EvalState::addCallDepth) see it is
       constant-initialized and elide the TLS-init guard on the serial path. */
    [[gnu::tls_model("initial-exec")]] static constinit thread_local bool amWorkerThread;

    /* Inc3 S4-a: the wave scope ambient for work running on THIS thread, or
       null. It is set for the duration of a worker's `work()` to the wave the
       item belongs to (`item.completion->wave`), and by the drv-set driver
       around its wave. A wave-aware spawn producer (`prim_parallel`) reads it to
       find the wave to register into; null means no ambient wave. Read ONLY in
       the cold spawn builtin and written only around a worker's `work()` (and by
       the driver) -- nothing on the inlined hot path. initial-exec + constinit
       so no TLS-init guard is ever emitted. */
    [[gnu::tls_model("initial-exec")]] static constinit thread_local WaveScope * currentWave;
};

struct FutureVector
{
    Executor & executor;

    std::shared_ptr<Executor::Completion> completion{std::make_shared<Executor::Completion>()};

    ~FutureVector();

    void spawn(Executor::WorkItems && work);

    void spawn(uint8_t prioPrefix, Executor::work_t && work)
    {
        spawn({{std::move(work), prioPrefix}});
    }

    /**
     * Wait for all spawned work items (including ones spawned by
     * other work items after this call) to finish, then rethrow the
     * first recorded exception, if any.
     */
    void finishAll();
};

/**
 * Inc3 S4-a: a per-wave structured-concurrency scope -- the join PLUMBING for
 * parallel memory-reclaiming revert, built with reclamation OFF.
 *
 * Carries the wave's pending-count (`completion`, REUSING the Executor's
 * `Completion` quiescence machinery -- not a second hand-rolled counter) plus a
 * per-wave cancel token. A work item spawned while this wave is ambient
 * (`Executor::currentWave == this`) is bound to `completion` with
 * increment-before-queue (`spawn`, the exact `FutureVector::spawn` primitive),
 * so a warm item spawned deep inside a wave element -- and anything IT spawns --
 * is counted in the wave's pending and awaited by `barrier()`.
 *
 * In S4-a `barrier()` ONLY awaits transitive quiescence (pending == 0); it frees
 * / reverts / munmaps NOTHING. The driver's per-wave reclamation, the
 * cross-worker TLS registry re-home, the Finally-ordered reclaim and the
 * wake-all-domains cancel wiring are all S4-b.
 */
struct WaveScope
{
    Executor & executor;

    std::shared_ptr<Executor::Completion> completion{std::make_shared<Executor::Completion>()};

    /* The per-wave cancel token (Item-1 generalized). Inc3 S4-b-a0 (§2.5) wires
       it: the worker `interruptCheck` consults `currentWave->cancel` in addition
       to `quit`, and `requestCancel()` sets it + wakes all waiter domains, so a
       worker parked in `waitOnThunk` on a cancelled sibling's cell is woken and
       unwinds at its next `checkInterrupt` poll. Nothing sets it in a0 (no
       cancel happens); it is wiring + a unit test. */
    std::atomic<bool> cancel{false};

    explicit WaveScope(Executor & executor)
        : executor(executor)
    {
        completion->wave = this;
    }

    /**
     * Spawn work bound to THIS wave, increment-before-queue -- the load-bearing
     * happens-before, identical to `FutureVector::spawn` (fetch_add before
     * `executor.spawn`). Used by the wave-aware producer (`prim_parallel`) when
     * this wave is ambient, so the warm work is counted in `pending` and
     * `barrier()` awaits it.
     */
    void spawn(Executor::WorkItems && work)
    {
        completion->pending.fetch_add(work.size(), std::memory_order_relaxed);
        executor.spawn(std::move(work), completion);
    }

    /**
     * Await transitive quiescence: block until every item bound to this wave --
     * and everything those items transitively spawned into it -- has finished
     * (pending == 0). S4-a does NOTHING else here (no reclamation). Warm-work
     * exceptions are swallowed (the warm `__parallel` pre-force is a pure hint
     * whose errors the fire-and-forget producers always dropped), except an
     * interrupt is re-raised so SIGTERM / teardown still unwinds. S4-b replaces
     * this with the finishAll-style element-task error/cancel propagation and
     * the single-writer reclamation.
     */
    void barrier();

    /**
     * Inc3 S4-b-a0 (§2.5): request cancellation of this wave. Sets `cancel` (so
     * the worker `interruptCheck` observes it) and wakes all waiter domains (so a
     * worker parked in `waitOnThunk` on a cancelled sibling's cell re-checks and
     * unwinds at its next poll). Abandoning a wave's warm work is safe (pure
     * hint). Nothing calls this in a0; it is wiring + a unit test.
     */
    void requestCancel();

    /**
     * Inc3 S4-b-a0 (F3): the reclamation-ORDERING shell. Runs `reclaim` AFTER the
     * barrier, and EVEN IF the barrier rethrows (a `Finally`, not a straight-line
     * statement), so a thrown wave never leaks its stripes / leaves reclamation
     * state dirty. The caller sets `cancel` (via `requestCancel`) BEFORE calling
     * this on the error path so the barrier does not block on a runaway sibling.
     * In S4-b-a0 the only caller is a unit test; the drv-set driver wires it in
     * S4-b-a1 and `reclaim` stays an empty placeholder until S4-b-b flips revert
     * on. `reclaim` must be noexcept (it runs from a `Finally`).
     */
    void barrierThenReclaim(const std::function<void()> & reclaim);
};

} // namespace nix
