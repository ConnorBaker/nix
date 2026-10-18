#include "nix/expr/eval.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/store/globals.hh"
#include "nix/util/finally.hh"

#include <chrono>

namespace nix {

/* ------------------------------------------------------------------ *
 * Armed flag + per-eval-thread id (shared with value.hh's publish path
 * and the inlined force site).
 * ------------------------------------------------------------------ */

namespace detail {
std::atomic<bool> parallelForcingArmed{false};
}

/* The per-eval-thread id, handed out in TLS-access order. It is used ONLY for
   self-cycle detection: it is written transiently into a cell's pending word
   (`fvMarkPending`) and compared against the forcing thread's own id, never
   persisted or printed, so each thread only needs a distinct id and the handout
   order is unobservable (byte-identical).

   inc2c-3: this id NO LONGER picks the unforce stripe. Because workers are
   spawned in the Executor constructor and open their unforce scopes at entry --
   before the main thread's first force -- the id handout order does not match
   thread role (the main thread can get a higher id than the workers). The
   stripe is therefore assigned by thread role in `beginUnforceScope`
   (`Executor::amWorkerThread`): the main thread always gets stripe 0 (the whole
   arena at eval-cores == 1, byte-identical), and each worker a distinct stripe
   in [1, evalCores]. */
static std::atomic<uint32_t> nextEvalThreadId{0};
[[gnu::tls_model("initial-exec")]] thread_local uint32_t myEvalThreadId(nextEvalThreadId++);

/* ------------------------------------------------------------------ *
 * Blackhole waiter domains (defined before the Executor, whose interrupt
 * callback wakes them all).
 * ------------------------------------------------------------------ */

// cache-line aligned to prevent false sharing between domains
struct alignas(64) WaiterDomain
{
    std::condition_variable cv;
};

static std::array<Sync<WaiterDomain>, 128> & waiterDomainsStorage()
{
    static std::array<Sync<WaiterDomain>, 128> domains;
    return domains;
}

static Sync<WaiterDomain> & getWaiterDomain(const void * valueAddr)
{
    auto & domains = waiterDomainsStorage();
    auto domain = (((size_t) valueAddr) >> 5) % domains.size();
    return domains[domain];
}

namespace detail {
void notifyValueWaiters(void * valueAddr) noexcept
{
    auto domain = getWaiterDomain(valueAddr).lock();
    domain->cv.notify_all();
}
} // namespace detail

/* ------------------------------------------------------------------ *
 * Executor
 * ------------------------------------------------------------------ */

[[gnu::tls_model("initial-exec")]] thread_local bool Executor::amWorkerThread{false};

unsigned int Executor::getEvalCores(const EvalSettings & evalSettings)
{
    /* Note: the default number of cores is currently limited to 32 due to
       scalability bottlenecks. The eval-cores setting defaults to 1 in inc2b-1
       (serial); 0 means "auto". */
    return evalSettings.evalProfilerMode != EvalProfilerMode::disabled ? 1
           : evalSettings.evalCores == 0UL                             ? std::min(32U, nix::Settings::getDefaultCores())
                                                                       : evalSettings.evalCores;
}

Executor::Executor(EvalState & evalState)
    : evalCores(getEvalCores(evalState.settings))
    , enabled(evalCores > 1)
    , evalState(&evalState)
    , interruptCallback(createInterruptCallback([&]() {
        for (auto & domain : waiterDomainsStorage())
            domain.lock()->cv.notify_all();
    }))
{
    debug("executor using %d threads", evalCores);

    /* Arm the concurrent-forcing publish path iff more than one worker can run
       (inc2b-1 keeps this disarmed at the default eval-cores == 1, so every
       value write stays a plain store, byte-identical to inc2a). */
    if (enabled)
        detail::parallelForcingArmed.store(true, std::memory_order_relaxed);

    /* inc2c-3: reserve the striped unforce arena on this (main) thread, before
       any worker is spawned. A worker opens its unforce scope at entry
       (`worker()`), so without an up-front reservation the first workers would
       race the lazy `stripesBase` reservation / `nStripes` binding. Done only
       when unforcing actually runs under threads (eval-cores > 1 + unforce); at
       eval-cores == 1 the single stripe is still reserved lazily by the main
       thread's first scope, byte-identical to inc2c-2. The main thread's write
       here happens-before each worker's reads via pthread_create. */
    if (enabled && evalState.settings.unforce)
        evalState.reserveUnforceStripes(evalCores);

    auto state(state_.lock());
    // FIXME: create worker threads on demand?
    for (size_t n = 0; n < evalCores; ++n)
        try {
            createWorker(*state);
        } catch (Error & e) {
            if (n == 0)
                throw;
            warn("could only create %d evaluator worker threads: %s", n, e.what());
            break;
        }
}

Executor::~Executor()
{
    std::vector<pthread_t> threads;
    {
        auto state(state_.lock());
        quit = true;
        std::swap(threads, state->threads);
        debug("executor shutting down with %d items left", state->queue.size());
    }

    wakeup.notify_all();

    for (auto & thr : threads)
        pthread_join(thr, nullptr);
}

/* pthread entry trampoline: run the worker loop, swallowing any exception that
   escapes it (it never should -- worker() catches work-item exceptions -- but a
   C++ exception must not unwind out of a pthread start routine). */
static void * executorWorkerTrampoline(void * self) noexcept
{
    try {
        static_cast<Executor *>(self)->worker();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
    return nullptr;
}

void Executor::createWorker(State & state)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        throw SysError("could not initialise thread attributes for an evaluator worker");
    Finally destroyAttr([&]() { pthread_attr_destroy(&attr); });

    if (pthread_attr_setstacksize(&attr, evalStackSize) != 0)
        throw SysError("could not set the stack size for an evaluator worker thread");

    pthread_t thread;
    if (int err = pthread_create(&thread, &attr, executorWorkerTrampoline, this))
        throw SysError(err, "could not create an evaluator worker thread");

    state.threads.push_back(thread);
}

void Executor::worker()
{
    ReceiveInterrupts receiveInterrupts;

    amWorkerThread = true;

    /* inc2c-2: per-worker unforce scope (Inc3 scaffolding). A worker that
       forces under unforcing binds its own stripe + owner timeline here
       (beginUnforceScope -> myStripe = myEvalThreadId) and RETAINS the whole
       stripe at exit (retain-only; revert stays OFF until Inc3). One scope per
       worker lifetime suffices while everything is retained -- per-item
       segmentation is only needed once revert turns on.

       INERT until 2c-3: `enabled` implies eval-cores > 1, which the EvalState
       ctor guard forbids together with unforce, so `enabled && unforce` is
       never true here yet; and at eval-cores == 1 `enabled` is false and this
       (single, idle) worker never pulls work, so it never forces. */
    const bool workerScope = enabled && evalState && evalState->settings.unforce;
    if (workerScope)
        evalState->beginUnforceScope();
    Finally closeWorkerScope([&]() {
        if (workerScope)
            evalState->endUnforceScopeRetaining();
    });

    while (true) {
        Item item;

        while (true) {
            auto state(state_.lock());
            if (quit) {
                /* Record an `Interrupted` exception for all queued items so
                   that any `FutureVector::finishAll()` wakes up and reports a
                   nice error. */
                auto ex = std::make_exception_ptr(Interrupted("interrupted by the user"));
                for (auto & item : state->queue)
                    if (item.second.completion)
                        item.second.completion->finish(ex);
                state->queue.clear();
                return;
            }
            if (!state->queue.empty()) {
                item = std::move(state->queue.begin()->second);
                state->queue.erase(state->queue.begin());
                break;
            }
            state.wait(wakeup);
        }

        std::exception_ptr ex;
        try {
            item.work();
        } catch (const Interrupted &) {
            quit = true;
            ex = std::current_exception();
        } catch (...) {
            ex = std::current_exception();
        }
        if (item.completion)
            item.completion->finish(std::move(ex));
    }
}

void Executor::spawn(WorkItems && items, std::shared_ptr<Completion> completion)
{
    if (items.empty())
        return;

    {
        auto state(state_.lock());
        for (auto & item : items) {
            /* Note: this uses a cheap PRNG rather than std::random_device,
               since the latter costs hundreds of cycles per call (RDRAND or
               /dev/urandom), which adds up when spawning many work items. The
               key only needs to spread items of the same priority around the
               queue, not be cryptographically random. */
            [[gnu::tls_model("initial-exec")]] static thread_local std::mt19937_64 rng{std::random_device{}()};
            [[gnu::tls_model("initial-exec")]] static thread_local std::uniform_int_distribution<uint64_t> dist(
                0, 1ULL << 48);
            auto key = (uint64_t(item.second) << 48) | dist(rng);
            state->queue.emplace(key, Item{.work = std::move(item.first), .completion = completion});
        }
    }

    if (items.size() == 1)
        wakeup.notify_one();
    else
        wakeup.notify_all();
}

FutureVector::~FutureVector()
{
    try {
        finishAll();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void FutureVector::spawn(Executor::WorkItems && work)
{
    /* Increment the pending count before queueing the items, since they may
       finish immediately. */
    completion->pending.fetch_add(work.size(), std::memory_order_relaxed);
    executor.spawn(std::move(work), completion);
}

void FutureVector::finishAll()
{
    std::vector<std::exception_ptr> exceptions;

    {
        auto state(completion->state_.lock());
        while (completion->pending.load(std::memory_order_acquire) != 0)
            state.wait(completion->cv);
        std::swap(exceptions, state->exceptions);
    }

    std::exception_ptr ex;
    for (auto & ex2 : exceptions) {
        if (ex) {
            if (!getInterrupted())
                try {
                    std::rethrow_exception(ex2);
                } catch (...) {
                    ignoreExceptionExceptInterrupt();
                }
        } else
            ex = ex2;
    }
    if (ex)
        std::rethrow_exception(ex);
}

/* ------------------------------------------------------------------ *
 * Blackhole wait / notify
 * ------------------------------------------------------------------ */

void EvalState::waitOnThunk(Value & v, uint64_t expectedP0)
{
    nrThunksAwaited++;

    auto domain = getWaiterDomain(&v).lock();

    if (Value::fvIsAwaited(expectedP0)) {
        /* Already flagged awaited by another waiter. Re-check under the domain
           lock that the value is still pending. */
        auto w0 = v.fvWord0Acquire();
        if (!Value::fvIsPendingTag(w0))
            /* Finished in the meantime. */
            return;
    } else {
        /* Flag the value as awaited so the finishing thread knows to wake us. */
        auto w0 = expectedP0;
        if (!v.fvCasAwaited(w0)) {
            if (!Value::fvIsPendingTag(w0))
                /* Finished in the meantime. */
                return;
            /* Otherwise another waiter already set awaited; fall through and
               wait too. */
        }
    }

    nrThunksAwaitedSlow++;
    currentlyWaiting++;
    maxWaiting = std::max<uint64_t>(maxWaiting.load(std::memory_order_acquire), currentlyWaiting.load());

    auto now1 = std::chrono::steady_clock::now();

    while (true) {
        domain.wait(domain->cv);
        auto w0 = v.fvWord0Acquire();
        if (!Value::fvIsPendingTag(w0)) {
            auto now2 = std::chrono::steady_clock::now();
            microsecondsWaiting += std::chrono::duration_cast<std::chrono::microseconds>(now2 - now1).count();
            currentlyWaiting--;
            return;
        }
        nrSpuriousWakeups++;
        checkInterrupt();
    }
}

/* ------------------------------------------------------------------ *
 * builtins.__parallel
 * ------------------------------------------------------------------ */

static void prim_parallel(EvalState & state, CallSite callSite, Value * const * args, Value & v)
{
    auto pos = callSite.pos;

    state.forceList(*args[0], pos, "while evaluating the first argument passed to builtins.__parallel");

    if (state.executor->enabled) {
        Executor::WorkItems work;
        for (auto value : args[0]->listView())
            if (!value->isFinished())
                work.emplace_back(
                    [rv(allocRootValue(value)), &state, pos]() { state.forceValue(**rv, pos); }, uint8_t(0));
        state.executor->spawn(std::move(work));
    }

    state.forceValue(*args[1], pos);
    v = *args[1];
}

static RegisterPrimOp r_parallel({
    .name = "__parallel",
    .args = {"xs", "x"},
    .arity = 2,
    .doc = R"(
      Start evaluation of the values in the list `xs` in the background
      (when `eval-cores > 1`) and return `x`. This is a hint used to
      parallelise evaluation; it does not change the result.
    )",
    .impl = prim_parallel,
    .experimentalFeature = Xp::ParallelEval,
});

} // namespace nix
