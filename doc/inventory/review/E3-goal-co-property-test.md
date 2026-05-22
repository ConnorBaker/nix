# E3 — Goal::Co property test specification

Status: complete.

This note specifies a property test that pins `Goal::Co`'s O(1)
frame-depth invariant so that any partial migration of catalog entry
#160 (replacing `Goal::Co` with `boost::asio::awaitable<T>`) cannot
silently regress the HALO tail-call optimisation. The test belongs in
`src/libstore-tests/`. Implementation is intentionally not provided —
this is a contract for the implementer.

## Mechanism review

I read `src/libstore/include/nix/store/build/goal.hh`,
`src/libstore/build/goal.cc`, `src/libstore/build/goal-impl.hh`,
`src/libstore/build/derivation-trampoline-goal.cc`,
`src/libstore/build/derivation-goal.cc`, and the relevant blocks of
`src/libstore/build/derivation-building-goal.cc` end-to-end.

### What the pieces do

`Goal::Co` is a move-only RAII wrapper around
`std::coroutine_handle<promise_type>`. The handle is destroyed when
`Co` goes out of scope (`Co::~Co` flips `promise().alive = false` then
calls `handle.destroy()`). `Co` never suspends caller-side eagerly —
its `await_ready` returns `false`.

`Goal::promise_type` carries three pieces of state:

- `std::optional<Co> continuation` — at any one time this is either
  the caller we will return to OR the callee we are about to tail-call
  into. The `return_value(Co &&)` overload is what flips its meaning:
  see below.
- `Goal * goal` — the back-pointer set by `Co::await_suspend` (when a
  `co_await another_co_function()` happens) or by `return_value(Co &&)`
  (when a `co_return another_co_function()` happens) or by the
  `Goal(Worker &, Co)` constructor for the root coroutine.
- `bool alive` — sentinel used by `Co::operator=`/destructor and
  asserted in `final_awaiter::await_suspend` so we never double-free.

`InitialSuspend` is a thin wrapper around `std::suspend_always` that
captures the handle in `await_suspend` and asserts in `await_resume`
that `goal`, `goal->top_co`, and the identity
`goal->top_co->handle == handle` are all consistent. It runs once per
coroutine, before the body starts.

`SuspendAwaiter` is the awaiter produced by
`await_transform(Suspend)`. It always suspends (`await_ready` returns
`false`) and asserts no child events are pending. `await_suspend` is a
no-op — control simply falls back out of the `coroutine_handle::resume`
that drove us here, which lands in `Goal::work` and ultimately in
`Worker::run`.

`ChildEventAwaiter` is the awaiter produced by
`await_transform(WaitForChildEvent)`. It is `await_ready`-true if a
child event is already queued (eager dequeue, no suspension); otherwise
it suspends and pops the queued event in `await_resume` once
`Worker::wakeUp` re-resumes the goal.

`final_awaiter` is the awaiter `final_suspend()` returns. Its
`await_suspend` is the entire mechanism this property test exists to
pin. Reproduced inline:

```
std::coroutine_handle<>
promise_type::final_awaiter::await_suspend(handle_type h) noexcept {
    auto & p = h.promise();
    auto goal = p.goal;
    auto c = std::move(p.continuation);
    if (c) {
        // We are tail-calling: hand `c` to the goal as the new top_co.
        // This destroys the previous top_co (which is `h`, us).
        goal->top_co = std::move(c);
        return goal->top_co->handle;          // symmetric transfer
    } else {
        // No continuation: goal must be done.
        p.goal->top_co = {};
        return std::noop_coroutine();
    }
}
```

The `goal->top_co = std::move(c)` line is what makes this O(1). Moving
into `top_co` *destroys the previous `top_co`* (which is `h`, the
coroutine that is finishing). After the move the only live coroutine
on the goal is `c` itself — the predecessor's frame is freed before
the symmetric-transfer return takes effect. C++20 symmetric transfer
(returning a `coroutine_handle<>` from `await_suspend`) means the
compiler emits a tail-jump; no extra stack frame is laid down for the
resumption.

### Two ways control transfers between coroutines

There are two distinct mechanisms, and they have different stack /
heap profiles:

1. **`co_await another_co_function()`** routes through
   `Goal::Co::await_suspend(handle_type caller)` (defined in
   `goal.cc`). The caller's `top_co` is moved into the callee's
   `continuation`, and the callee's handle becomes the new `top_co`.
   When the callee finishes, its `final_awaiter` fires, sees a
   continuation (the original caller), swaps it back, and returns.
   **Both frames are live simultaneously while the callee runs.**

2. **`co_return another_co_function()`** routes through
   `promise_type::return_value(Co && next)`. This sets `next` as our
   *own* continuation, pushing our previous continuation down to be
   `next`'s continuation. `final_suspend` then runs immediately, and
   `final_awaiter::await_suspend` swaps `next` into `top_co`,
   destroying the current frame in the move-assign and returning the
   callee's handle for symmetric transfer. **Only one of the two
   frames is live at a time.**

Mechanism (2) is the HALO tail-call. It is the entire reason the
codebase can express deep recursive build-orchestration logic
(`tryToBuild` -> `buildLocally` -> `tryToBuild` retry; `init` ->
`haveDerivation` -> `amDone`; `gaveUpOnSubstitution` -> `tryToBuild`;
etc.) without growing the heap one frame per chained call.

### What the file's own `@todo` says

`goal.hh`'s comment on `Co`:

> @todo Allocate explicitly on stack since HALO thing doesn't really
> work, specifically, there's no way to uphold the requirements when
> trying to do tail-calls without using a trampoline AFAICT.

This refers to the C++ standard's HALO ([Coroutine Heap-Allocation
Elision Optimisation](https://wg21.link/p0981)) which lets the
compiler put a coroutine frame on the stack when the lifetime is
provably bounded. The `final_awaiter` pattern here is *not* HALO; it
is symmetric transfer with manual frame destruction. The comment is
saying "we wish HALO would put these on the stack, it doesn't, so we
do the next best thing: free the previous frame in `final_awaiter`
before resuming the next." The property test must pin the *next best
thing*, not HALO itself.

## Tail-call call-site survey

Across all five goal files I grepped (`derivation-trampoline-goal.cc`,
`derivation-goal.cc`, `derivation-resolution-goal.cc`,
`derivation-building-goal.cc`, `drv-output-substitution-goal.cc`,
`substitution-goal.cc`) the `co_return` calls fall into three classes:

1. **Terminal**: `co_return Return{}`, `co_return Done{}` (rare; only
   constructed via `amDone`/`doneSuccess`/`doneFailure`),
   `co_return amDone(...)`, `co_return doneSuccess(...)`,
   `co_return doneFailure(...)`. These return a non-`Co` value to
   `return_value(Return)` / `return_value(Done)`, so `final_awaiter`
   sees no continuation set by the body and either resumes the prior
   caller (if `co_await`-chained) or finishes.

2. **Tail-call to another `Co`-returning method on the same goal**:
   the moves that exercise the HALO optimisation. From the survey:

   - `DerivationTrampolineGoal::init` ->
     `co_return haveDerivation(std::move(drvPath), std::move(drv));`
     (in `derivation-trampoline-goal.cc`).
   - `DerivationGoal::haveDerivation` ->
     `co_return repairClosure();` (in `derivation-goal.cc`).
   - `DerivationBuildingGoal::gaveUpOnSubstitution` ->
     `co_return tryToBuild(std::move(inputPaths));`
     (in `derivation-building-goal.cc`).
   - `DerivationBuildingGoal::tryToBuild` ->
     `co_return buildWithHook(...)` (two sites: inside `tryHookLoop`
     and the second invocation after the wait loop) and
     `co_return buildLocally(...)` (in `tryBuildLocally`). These are
     **inside lambdas with `Goal::Co` return type** — the inner
     lambda's `co_return next()` jumps to `next`, but the lambda was
     itself reached via `co_await acquireResources(...)` followed by
     more inner logic, and the lambda is `co_await`ed by the body. So
     the tail-call here is at lambda granularity, not method
     granularity.
   - `DerivationBuildingGoal::buildLocally` ->
     `co_return tryToBuild(std::move(inputPaths));` inside the
     `curBuilds >= maxBuildJobs` retry path. **This is the
     unbounded-retry chain**: each time max jobs is hit,
     `buildLocally` tail-calls back into `tryToBuild`, which can
     tail-call back into `buildLocally`. Without the HALO
     optimisation each `co_await waitForBuildSlot()` cycle would
     accumulate two frames forever.

3. **Convenience helpers**: `co_await yield()`,
   `co_await waitForAWhile()`, `co_await waitForBuildSlot()`,
   `co_await waitUntilWoken()`, `co_await await({...waitees...})`.
   These are `co_await`ed not `co_return`ed, and their own bodies
   `co_return Return{}`. They contribute the
   `co_await another_coroutine()` overhead (mechanism 1 above), one
   extra frame for the duration of the call.

The unbounded chain that worries us most is
`tryToBuild -> buildLocally -> waitForBuildSlot -> tryToBuild -> ...`
which under heavy contention produces an N-deep tail-call chain where
N = number of times the build slot was unavailable. Today this is
O(1)-bounded in heap. A naive #160 migration that replaces
`co_return f()` with `co_await f()` would make it O(N).

## Invariant to pin

**Heap-bounded tail-call invariant.** Let `N` be the number of times
a `Goal::Co`-returning method tail-calls another `Goal::Co`-returning
method on the same goal via `co_return f();`. Across the entire chain
the number of *live* coroutine frames at any single point in time is
exactly **1** plus a constant for any in-flight sub-`co_await`s. It is
*not* a function of `N`.

Formal statement, in two equivalent forms.

**(Heap form.)** If `M` is the number of distinct coroutine frames
allocated by `Goal::promise_type::operator new` (or whatever allocator
the promise routes to) over the lifetime of a tail-call chain of
length `N`, then `M = O(N)` *as a cumulative count* but the number of
*simultaneously live* frames is `O(1)` in `N`. Equivalently:
`live_frame_max - first_frame_count <= K` for some small `K`
independent of `N` (`K = 1` for pure `co_return next()` chains, plus a
fixed offset for the helper coroutines such as `waitForBuildSlot` that
the chain transiently `co_await`s).

**(Stack form.)** Let `S(d)` be the runtime stack depth observed at
the point of execution inside the d-th tail-callee in the chain.
`S(d) - S(0) <= K` for some small `K` independent of d — symmetric
transfer must compile down to a tail-jump, not a recursive call.

The property test must pin **the heap form**. The stack form is
subordinate (a regression in heap necessarily lifts stack too, but the
heap form is observable without architecture-specific stack
inspection).

### What invariant a partial #160 migration would break

A partial migration that converts `SuspendAwaiter` and
`ChildEventAwaiter` to `asio::awaitable<T>` while leaving
`final_awaiter` in place is at first glance the safe choice. But:

- `asio::awaitable<T>` requires `co_await sub()` for chaining.
  `co_return sub();` is not supported. Anywhere a goal's body
  becomes asio-shaped, the `co_return another_co_function();` form
  must mechanically rewrite to `co_await another_co_function();
  co_return Return{};`. Each such rewrite converts a tail-call site
  from heap-form-O(1) to heap-form-O(depth-of-chain).

- The migration is locally semantics-preserving (same observable
  outputs, same goal completion). It is globally heap-O(N) instead
  of O(1). On long retry chains in `buildLocally` the daemon now
  accumulates frames for the entire retry history.

- Existing tests in `src/libstore-tests/` (e.g.
  `WorkerSubstitutionTest`) drive at most a handful of nested calls.
  They will pass after the partial migration. Production at scale
  will not.

## Test design

Two candidate measurement strategies:

**Strategy A — heap allocation count via `operator new` interception.**
Override `Goal::promise_type::operator new` (a member operator new on
`promise_type` is found by the compiler ahead of the global one) so
that it counts allocations into a `thread_local` or test-controlled
counter. After running a chain of N tail-calls, assert that the peak
count of *live* allocations stayed below a small constant. Pair with
`operator delete` to track liveness, not just cumulative count.

**Strategy B — stack pointer measurement.** Capture
`__builtin_frame_address(0)` (or `&local_var` cast to `uintptr_t`) at
the deepest tail-callee and compare to the same address captured at
the top of the chain. Assert the delta is bounded by a small constant.
This directly observes "did the compiler emit a tail-jump?"

**Strategy A is more practical for this codebase.** Reasons:

- Heap is what the catalog entry is about. Cumulative allocations are
  a direct measurement of the property under test, not a proxy.
- `operator new` interception is portable across compilers and
  optimisation levels. Stack depth depends on `-O0` vs `-O2`, frame
  pointer omission, ASan padding, sanitiser thunks, and platform ABI.
- The `Co` class already destroys handles in its destructor and via
  `final_awaiter`. Pairing `operator new`/`operator delete` overrides
  with the existing destruction discipline gives a clean live-count.
- `promise_type` is a member type of `Goal`, defined in a header. A
  member `operator new`/`operator delete` overload on
  `promise_type` is found before the global one by C++'s lookup rules
  for coroutine promise allocation (`[dcl.fct.def.coroutine]/9`). So
  the test can install an `operator new` shim by *defining*
  `promise_type::operator new`/`delete` in a translation unit linked
  ahead of the production one — but that requires conditional
  compilation. **Better:** parameterise the production code (now or as
  part of landing the test) so the allocator is configurable, or use
  a test-only friend that observes via a side channel.

I recommend a hybrid: define a `thread_local` allocation counter in
`promise_type` (compiled in only under a test macro, or always but
gated behind `#ifndef NDEBUG`), increment it in a member
`operator new`/`operator delete` on `promise_type`, and have the test
read it. This minimises the risk that the test has different code
paths from production.

The fallback if instrumenting `promise_type` is unacceptable is
Strategy B; document the `-O` flag the test must run at and accept
that `-O0` may legitimately fail.

## Test skeleton

File: `src/libstore-tests/goal-co-tail-call.cc` (new — add to
`sources` in `src/libstore-tests/meson.build`).

Minimal scaffolding:

```
namespace nix {

/**
 * Test goal that recursively tail-calls itself N times before
 * settling. Pure unit-of-test surface; does no real I/O.
 */
struct TailCallProbeGoal : Goal {
    size_t remaining;
    size_t * peakLiveFrames;        // updated by promise_type allocator
    size_t * cumulativeAllocs;

    TailCallProbeGoal(Worker & w, size_t depth, size_t * peak, size_t * total);

    Co step();                       // body: co_return step() if remaining > 0
                                     // else co_return amDone(ecSuccess);

    JobCategory jobCategory() const override { return JobCategory::Administration; }
    std::string key() override { return "tail-call-probe"; }
};

class GoalCoTailCallTest : public LibStoreTest {
    // reuse the WorkerSubstitutionTest fixture pattern: dummy store, no
    // substituters, no real builds; the goal does no I/O.
};

TEST_F(GoalCoTailCallTest, tailCallChainHasBoundedLiveFrames) {
    constexpr size_t N = 1000;
    size_t peakLive = 0;
    size_t cumulative = 0;

    Worker worker{*store, *store};
    auto g = std::make_shared<TailCallProbeGoal>(worker, N, &peakLive, &cumulative);

    Goals goals;
    goals.insert(g);
    worker.run(goals);

    EXPECT_EQ(g->exitCode, Goal::ecSuccess);

    // Cumulative allocations must scale with chain length: each tail-callee
    // is a freshly-constructed coroutine frame.
    EXPECT_GE(cumulative, N);

    // The HALO invariant: live frames at any point are O(1), not O(N).
    // Today the bound is ~2 (current + the helper-coroutine slack).
    // Pick a generous constant that catches the regression cleanly.
    constexpr size_t LIVE_FRAME_BOUND = 8;
    EXPECT_LE(peakLive, LIVE_FRAME_BOUND)
        << "Live coroutine frames grew with chain length. "
        << "Either final_awaiter's symmetric-transfer destroy step "
        << "regressed (see Goal::promise_type::final_awaiter::await_suspend "
        << "in src/libstore/build/goal.cc) or a co_return f() site got "
        << "rewritten to co_await f(); co_return Return{};.";
}

TEST_F(GoalCoTailCallTest, coAwaitChainScalesWithDepth) {
    // Sanity check: confirm the test instrumentation works by exercising
    // the OPPOSITE path — co_await chain — and asserting peak live frames
    // *does* scale with N. If both this test and the previous one show
    // the same peak, the instrumentation is broken.
    ...
}

} // namespace nix
```

Body of `TailCallProbeGoal::step()`:

```
Goal::Co TailCallProbeGoal::step() {
    if (remaining == 0) {
        co_return doneSuccess(BuildResult::Success{
            .status = BuildResult::Success::AlreadyValid });
    }
    --remaining;
    co_return step();   // tail-call
}
```

Constructor delegates to `Goal(worker, step())` so that the initial
suspend lands inside `step` with `goal` set. `peakLiveFrames` and
`cumulativeAllocs` are stored on the goal so the
`promise_type::operator new` instrumentation can locate them via
`promise.goal` once `goal` is set on the second-and-onward frame —
or, if first-frame counting matters, via a `thread_local`.

The instrumentation hook (sketch — install via a member overload):

```
struct Goal::promise_type {
    // ... existing members ...

    // test-only counters (gated under #ifdef NIX_GOAL_CO_INSTRUMENTED
    // or always-on if cheap; both work)
    static thread_local size_t liveFrames;
    static thread_local size_t * peakSink;
    static thread_local size_t * cumulativeSink;

    void * operator new(std::size_t n) {
        ++liveFrames;
        if (cumulativeSink) ++*cumulativeSink;
        if (peakSink && liveFrames > *peakSink) *peakSink = liveFrames;
        return ::operator new(n);
    }

    void operator delete(void * p, std::size_t n) noexcept {
        --liveFrames;
        ::operator delete(p, n);
    }
};
```

Test sets `peakSink` / `cumulativeSink` to point at the `peakLive` /
`cumulative` locals before calling `worker.run`, and clears them
after. `liveFrames` resets to zero across tests via fixture teardown.

## Failure-mode documentation

Today the test passes because:

- `final_awaiter::await_suspend` performs the
  `goal->top_co = std::move(c)` move, which assigns through
  `Co::operator=(Co &&)`, which destroys the previous handle (the one
  finishing) before installing the new continuation. So in a chain
  `step(N) -> step(N-1) -> ... -> step(0)` only one frame is live at
  any point: the previous frame is freed by the time the next one
  starts.
- The C++20 symmetric-transfer protocol guarantees that returning a
  `coroutine_handle<>` from `await_suspend` is a tail-jump, not a
  call, so **no stack growth either**.

A partial #160 migration that converts the `co_return next();` sites
to `co_await next(); co_return Return{};` (because the migrated
machinery — be it `asio::awaitable<T>` or `std::generator` — does not
support the tail-call `return_value(Co &&)` overload) will fail the
property test in three observable ways:

1. **`peakLive` grows with `N`.** Each `co_await next()` keeps the
   parent's frame alive while the child runs. After `N` levels of
   nesting, all `N+1` frames are simultaneously live on the heap.
   `EXPECT_LE(peakLive, LIVE_FRAME_BOUND)` fails with `peakLive`
   approximately equal to `N`. This is the primary signal.

2. **`cumulative` count is unchanged from the passing case.** Both
   tail-call and `co_await` chains allocate exactly one frame per
   coroutine invocation. So cumulative-count alone would not catch
   the regression; the live-count is what matters. The
   `EXPECT_GE(cumulative, N)` check exists only to confirm the
   instrumentation is wired up.

3. **Stack overflow at large N.** If the migration also breaks
   symmetric transfer (e.g., the new awaiter returns `void` from
   `await_suspend` instead of a `coroutine_handle<>`), each chained
   call pushes a real stack frame. At `N = 1000` with default
   thread-stack size, this is borderline; at `N = 10000` it segfaults.
   This is detectable by a separate stack-pointer assertion
   (Strategy B) and is worth including as a secondary check. The
   `tryToBuild`/`buildLocally` retry under heavy build-slot pressure
   is the production scenario where this would surface — chains of
   thousands of retries are not implausible on a heavily contended
   builder.

## Open questions

- **Where to put `liveFrames` / counters.** The cleanest hook is a
  member `operator new`/`operator delete` on `Goal::promise_type`,
  added to `goal.hh`/`goal.cc` and made unconditional (the cost is one
  thread-local increment per coroutine creation, negligible). Doing so
  bakes test-only state into production. The alternative is a global
  `operator new`/`operator delete` shim in a test-only TU, but that
  intercepts *all* allocations and forces the test to filter by size
  or address range, which is brittle. **Recommendation:** add the
  member overloads to `Goal::promise_type` unconditionally, document
  them as instrumentation, and accept the negligible cost.

- **What is the allowed `LIVE_FRAME_BOUND` constant?** Today the
  steady-state of `tryToBuild` -> `buildLocally` -> `tryToBuild` is
  one or two live frames depending on whether the test exercises the
  `co_await waitForBuildSlot()` path. Empirically determine the
  bound by running the passing test with high N, take the observed
  peak, multiply by 2 for slack, and lock that in. Without
  empirical measurement I would guess `8` is a generous-but-tight
  bound; reviewers should require the implementer to print the
  observed peak and justify the chosen bound.

- **Should the chain test the `co_await waitForBuildSlot()` retry
  pattern specifically?** A second test with body
  `co_await yield(); co_return step();` would exercise the
  `co_await another_co()` -> `co_return another_co()` interaction —
  the case where the helper coroutine adds a transient frame and the
  tail-call frees it. Recommendation: include both as separate
  `TEST_F` cases.

- **Does the test need to run under sanitisers?** `Co::~Co` already
  has UB-clean ordering (set `alive = false` then `handle.destroy()`).
  Running the test under ASan is a free correctness check on the
  instrumentation hook itself; if the `operator new`/`operator
  delete` pairing is wrong (e.g., counts become negative on
  exception unwind), ASan would surface it. Recommendation: ensure
  the test fixture works under the existing CI sanitiser builds.

- **Does the heap-form invariant suffice, or must the test also pin
  symmetric transfer (the stack form)?** The catalog text says "same
  number of frames are alive at the same call depth (equivalent stack
  depth in micro-benchmark)" — both. I argue heap-form alone is
  sufficient for the property test (it is a strictly stronger signal
  in practice — a stack-form regression without a heap-form
  regression would require simultaneously breaking and preserving
  symmetric transfer, which is incoherent). But adding a `__builtin_
  frame_address(0)`-based stack-form sanity check at low cost is
  defensible if reviewers want belt-and-braces.

- **What happens if a future migration supports `co_return f();` but
  via a different mechanism that allocates an extra small bookkeeping
  frame per tail-call (e.g., a trampoline thunk)?** The bound check
  would fire even though the asymptotic property is preserved. The
  generous constant `LIVE_FRAME_BOUND = 8` should absorb a constant
  trampoline overhead; if a future migration needs more, the test is
  the right place to negotiate the new bound.
