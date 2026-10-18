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

#include <pthread.h>

#include "nix/util/sync.hh"
#include "nix/util/logging.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/util.hh"
#include "nix/util/signals.hh"

namespace nix {

struct EvalSettings;
struct EvalState;

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

    /* inc2c-2: the EvalState that owns this executor (1:1), set right after
       construction. Used only by `worker()` to open a per-worker unforce scope
       when unforcing runs under threads (Inc3 scaffolding). INERT until 2c-3:
       `enabled` implies eval-cores > 1, which the EvalState ctor guard forbids
       together with unforce, so the worker-scope branch is never taken yet. */
    EvalState * evalState = nullptr;

    const std::unique_ptr<InterruptCallback> interruptCallback;

    Sync<State> state_;

    std::condition_variable wakeup;

    static unsigned int getEvalCores(const EvalSettings & evalSettings);

    Executor(const EvalSettings & evalSettings);

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

    [[gnu::tls_model("initial-exec")]] static thread_local bool amWorkerThread;
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

} // namespace nix
