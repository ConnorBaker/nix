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

/* `constinit`: guaranteed constant initialization, so no dynamic-init guard is
   emitted and cross-TU users (notably `EvalState::addCallDepth`) access it with a
   plain initial-exec TLS load, no `_ZTH`/TLS-wrapper call on the serial path. */
[[gnu::tls_model("initial-exec")]] constinit thread_local bool Executor::amWorkerThread = false;

/* Inc3 S4-a: the ambient wave scope for this thread (see parallel-eval.hh).
   `constinit` initial-exec, like `amWorkerThread`, so the serial path never
   emits a TLS-init guard; read only in the cold `prim_parallel`. */
[[gnu::tls_model("initial-exec")]] constinit thread_local WaveScope * Executor::currentWave = nullptr;

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
    , interruptCallback(createInterruptCallback([this]() { wakeAllWaiterDomains(); }))
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

    /* Wake idle workers waiting for queue items (they observe `quit` and drop
       the remaining queued-but-not-started work). */
    wakeup.notify_all();

    /* Belt-and-suspenders: wake every blackhole waiter domain so no worker is
       left parked in `waitOnThunk` at teardown. Normally a cancelled producer
       DOES wake its waiters -- `mkFailed` -> `publishOrStorePayload` ->
       `publishCompletionSlow` release-exchanges the pending cell and notifies an
       awaited waiter. This one-shot wake covers the residual case where that
       path can't fire promptly: a waiter parked on a cell whose producer is
       stuck in a long, non-pollable builtin (so the producer has not reached a
       `checkInterrupt` poll to be cancelled and republish the cell). After the
       wake the waiter re-checks, sees the cell still pending, and polls
       `checkInterrupt` -> `quit` -> throws `Interrupted`, unwinding cooperatively.
       (The Executor's own interrupt callback wakes these domains too, but only on
       an actual SIGINT; teardown is the no-signal case.) */
    wakeAllWaiterDomains();

    for (auto & thr : threads)
        pthread_join(thr, nullptr);
}

void Executor::wakeAllWaiterDomains() noexcept
{
    for (auto & domain : waiterDomainsStorage())
        domain.lock()->cv.notify_all();
}

void Executor::releaseOutstanding(uint64_t n) noexcept
{
    if (n == 0)
        return;
    /* On the edge that empties the pool, take the lock before notifying so a
       waiter that saw a non-zero count observes the notify (no lost wakeup). */
    if (globalOutstanding.fetch_sub(n, std::memory_order_acq_rel) == n) {
        std::lock_guard<std::mutex> lk(idleMutex);
        idleCv.notify_all();
    }
}

void Executor::awaitExecutorIdle()
{
    /* Inc3 S4-b-a0 (F1): block until the pool is globally quiescent. The
       `idleMutex` makes a waiter that saw a non-zero count observe the 1->0
       notify (no lost wakeup), mirroring `Completion::finish`. Nothing calls
       this on a live path in a0; it is the primitive the S4-b-a1 barrier uses. */
    std::unique_lock<std::mutex> lk(idleMutex);
    idleCv.wait(lk, [this]() { return globalOutstanding.load(std::memory_order_acquire) == 0; });
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

    /* Structured cancellation (Item 1): make this worker's `checkInterrupt`
       polls consult the Executor's `quit` flag. `interruptCheck` is
       thread-local (`nix::unix::interruptCheck`), so installing it here NEVER
       changes the main thread's interrupt semantics -- it only lets outstanding
       fire-and-forget "warm" work running on THIS worker observe a cooperative
       cancel. Abandoning warm work is always safe: it is a pure optimisation
       hint (it only pre-forces values the serial/driver pass re-forces on
       demand; forcing is idempotent), so cancelling it never changes a result.

       On teardown, `~Executor` sets `quit`; the worker-gated poll in
       `EvalState::addCallDepth` (and the existing polls in `waitOnThunk` and the
       value-to-json recursion) then throws `Interrupted`, unwinding the warm
       work via the ordinary force-exception path so the teardown join returns
       promptly instead of waiting out a runaway worker. A SIGTERM/SIGINT
       arriving while the main thread is blocked in that join independently sets
       the GLOBAL interrupt flag (`unix::_isInterrupted`), which the same
       `checkInterrupt` observes -- so signals abort the worker too.

       This is the reusable cancellation primitive Inc3's wave-scoped quiescence
       join builds on: a per-scope cancel token consulted here the same way lets
       a wave (and all work it transitively spawned) be cancelled/awaited at the
       scope boundary without tearing down the whole pool. */
    /* Inc3 S4-b-a0 (§2.5): also consult the ambient wave's cancel token, so a
       wave (and all work it transitively spawned) can be cancelled without
       tearing down the whole pool. `currentWave` is this worker's thread_local,
       set per item (F2); when running a wave item it points at that wave, else
       null. Reads are acquire to pair with `requestCancel`'s release store.
       S4-b-b: a wave-cancel Interrupted should NOT set `quit` (that tears down
       the pool); the worker's Interrupted catch below will be refined to
       distinguish a wave cancel from a teardown/signal. */
    unix::interruptCheck = [this]() noexcept {
        return quit.load(std::memory_order_acquire)
               || (currentWave && currentWave->cancel.load(std::memory_order_acquire));
    };
    Finally clearInterruptCheck([]() noexcept { unix::interruptCheck = {}; });

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
                size_t dropped = state->queue.size();
                for (auto & item : state->queue)
                    if (item.second.completion)
                        item.second.completion->finish(ex);
                state->queue.clear();
                /* Inc3 S4-b-a0 (F1): these items were counted at spawn but will
                   never run -- release them from the global count (and signal
                   idle if that empties the pool). */
                releaseOutstanding(dropped);
                return;
            }
            if (!state->queue.empty()) {
                item = std::move(state->queue.begin()->second);
                state->queue.erase(state->queue.begin());
                break;
            }
            state.wait(wakeup);
        }

        /* Inc3 S4-a (F2): make the wave this item belongs to ambient for the
           item's duration, so a nested spawn (`prim_parallel`) on this worker
           inherits it via `currentWave`. The wave rides `Completion::wave`
           (threaded into every Item at spawn), NOT a separate `Item::wave` each
           producer must remember to stamp. Restore the PREVIOUS value (not
           unconditionally null) so items that run while this worker was already
           inside a wave nest correctly. */
        WaveScope * prevWave = currentWave;
        currentWave = item.completion ? item.completion->wave : nullptr;
        Finally restoreWave([&]() noexcept { currentWave = prevWave; });

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
        /* Inc3 S4-b-a0 (F1): the item ran (or threw) -- release it from the
           global count. For EVERY item, not just completion-bearing ones. */
        releaseOutstanding(1);
    }
}

void Executor::spawn(WorkItems && items, std::shared_ptr<Completion> completion)
{
    if (items.empty())
        return;

    /* Inc3 S4-b-a0 (F1): count these items in the GLOBAL outstanding work BEFORE
       queueing (increment-before-queue, so a worker can't dequeue + decrement
       before the increment is visible). The one spawn chokepoint every producer
       funnels through, so the count is self-enforcing. */
    globalOutstanding.fetch_add(items.size(), std::memory_order_relaxed);

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
 * WaveScope (Inc3 S4-a)
 * ------------------------------------------------------------------ */

void WaveScope::barrier()
{
    /* Await transitive quiescence. Mirrors `FutureVector::finishAll`'s ordering
       exactly (acquire load of `pending` under the state lock, wait on the cv),
       which is what makes `pending == 0` a genuine quiescence point: a new wave
       item can only be created by an already-counted running wave item, which
       increments before it queues (`spawn`) and whose own `finish()` runs only
       after its `work()` returns -- so while any transitive descendant exists
       `pending >= 1`. */
    std::vector<std::exception_ptr> exceptions;
    {
        auto state(completion->state_.lock());
        while (completion->pending.load(std::memory_order_acquire) != 0)
            state.wait(completion->cv);
        std::swap(exceptions, state->exceptions);
    }

    /* S4-a: the only work bound to a wave is warm `__parallel` pre-force -- a
       pure hint whose errors the fire-and-forget producers always dropped.
       Swallow recorded warm-work exceptions (preserving that semantics), except
       re-raise an interrupt so SIGTERM / teardown still unwinds the driver.
       S4-b adds the real element-task error/cancel propagation. */
    for (auto & ex : exceptions) {
        try {
            std::rethrow_exception(ex);
        } catch (...) {
            ignoreExceptionExceptInterrupt();
        }
    }
}

void WaveScope::requestCancel()
{
    /* Inc3 S4-b-a0 (§2.5): set the token (release, paired with the worker's
       acquire read in `interruptCheck`), THEN wake every waiter domain so a
       worker parked in `waitOnThunk` on a cell produced by a cancelled sibling
       re-checks and unwinds at its next `checkInterrupt` poll. Order matters:
       set-then-wake, so a woken worker sees the token. */
    cancel.store(true, std::memory_order_release);
    executor.wakeAllWaiterDomains();
}

void WaveScope::barrierThenReclaim(const std::function<void()> & reclaim)
{
    /* Inc3 S4-b-a0 (F3): the reclamation runs AFTER the barrier and EVEN IF the
       barrier rethrows -- a `Finally`, not a straight-line `barrier(); reclaim();`
       (which would skip reclaim on a rethrow, leaking the wave's stripes). The
       caller must have set `cancel` (via `requestCancel`) before this on the
       error path so the barrier does not block on a runaway sibling.
       // S4-b-b: `reclaim` carries the single-writer reclamation (munmap/revert +
       // the F4a registry copy-in / TLS write-back + the F4c per-wave region
       // reset). It is an empty placeholder until revert flips on; in a0 the only
       // caller is a unit test. It must be noexcept (it runs from the Finally). */
    Finally doReclaim([&]() noexcept {
        if (reclaim)
            reclaim();
    });
    barrier();
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
        /* S4-a: `prim_parallel` is observably IDENTICAL to base -- warm
           deep-forces are spawned fire-and-forget and NOT awaited here, so a
           throwing or diverging un-demanded hint element does not change the
           result ("it does not change the result", below): the warm work's
           errors are dropped (its spawn carries no completion) and its
           divergence is cut at `~Executor` teardown (the Item-1 cancel poll).
           This holds for ALL inputs, not just the drv-set gate.
           // S4-b: when revert turns ON (a wave barrier munmaps stripes), a
           // fire-and-forget warm item with `currentWave == nullptr` is the F1
           // UAF. The correct fix is NOT inline forcing here (it changes
           // throw-vs-value / terminate-vs-hang -- an L2 break) but a GLOBAL
           // executor-idle barrier precondition (Gate-1 review option 2): the
           // barrier blocks on a global outstanding-work count so ANY spawn --
           // including this one, and any future producer -- is awaited before a
           // stripe is freed. The wave-ambient routing (spawn into
           // `currentWave->completion`) is then the per-wave accounting on top. */
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
