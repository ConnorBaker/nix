/* Inc3 S4-b-a0 white-box tests: the fan-out-independent, reclamation-OFF
   machinery for the parallel memory-reclaiming wave barrier.

     F1  -- the GLOBAL executor-idle counter tracks spawns/completions and
            returns to zero (`outstandingWork`/`awaitExecutorIdle`).
     §2.5 -- a wave `requestCancel()` unwinds a running wave item (the worker
            `interruptCheck` consults `currentWave->cancel`).
     F3  -- `barrierThenReclaim` runs the reclamation AFTER the barrier, even
            when the barrier rethrows (Finally-ordered).
     F4b -- `waveStripeOf`/`youngW`/`waveStripeSpan` classify correctly for a
            SPARSE set of global stripe ids (the exactness-indexing fix).

   These exercise the join PRIMITIVE directly (Executor + WaveScope, and the
   unforce.cc classification hook); no drv-set fan-out, no reclamation, no
   evaluated values -- so they are deterministic and inert. */

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/parallel-eval.hh"
#include "nix/util/signals.hh"

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace nix {

/* The F4b classification hook, defined in unforce.cc (test-only; saves/restores
   the file-local stripe globals so it does not disturb production state). */
namespace unforce_test {
void beginSyntheticWave(const void * base, size_t n);
void endSyntheticWave();
size_t waveStripeOfForTest(const void * p);
bool youngWForTest(const void * p);
size_t waveStripeSpanForTest();
size_t stripeShiftForTest();
} // namespace unforce_test

class WaveS4baTest : public LibExprTest
{};

/* F1: the global outstanding-work count tracks spawns and completions and
   returns to zero. Items block until released so the count can be observed up. */
TEST_F(WaveS4baTest, globalIdleCounterTracksAndReturnsToZero)
{
    auto & ex = *state.executor;
    ex.awaitExecutorIdle(); /* clean baseline */
    ASSERT_EQ(ex.outstandingWork(), 0u);

    std::atomic<bool> release{false};
    std::atomic<uint64_t> ran{0};
    constexpr uint64_t N = 64;

    Executor::WorkItems work;
    for (uint64_t i = 0; i < N; ++i)
        work.emplace_back(
            [&]() {
                while (!release.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                ran.fetch_add(1, std::memory_order_relaxed);
            },
            uint8_t(0));

    WaveScope wave(ex);
    wave.spawn(std::move(work));

    /* All N were counted at spawn (increment-before-queue) and none can finish
       while blocked, so the global count is exactly N. */
    EXPECT_EQ(ex.outstandingWork(), N);

    release.store(true, std::memory_order_release);
    wave.barrier();          /* wave pending -> 0 */
    ex.awaitExecutorIdle();  /* global -> 0 */

    EXPECT_EQ(ex.outstandingWork(), 0u);
    EXPECT_EQ(ran.load(), N);
}

/* §2.5: requestCancel() unwinds a running wave item -- the worker's
   interruptCheck consults currentWave->cancel, so a polling item throws
   Interrupted, which the barrier re-raises. */
TEST_F(WaveS4baTest, requestCancelUnwindsRunningWaveItem)
{
    auto & ex = *state.executor;
    WaveScope wave(ex);

    std::atomic<bool> itemStarted{false};
    Executor::WorkItems work;
    work.emplace_back(
        [&]() {
            itemStarted.store(true, std::memory_order_release);
            /* Poll until cancelled (or the whole pool is torn down). */
            while (true) {
                checkInterrupt();
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        },
        uint8_t(0));
    wave.spawn(std::move(work));

    /* Wait until the item is actually running on a worker (so currentWave is set
       for it), then cancel. */
    while (!itemStarted.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::microseconds(50));

    wave.requestCancel();

    /* The item observes the cancel at its next checkInterrupt poll, throws
       Interrupted, and the barrier re-raises it. */
    EXPECT_THROW(wave.barrier(), Interrupted);
}

/* F3: barrierThenReclaim runs the reclamation after a NORMAL barrier. */
TEST_F(WaveS4baTest, barrierThenReclaimRunsReclaimAfterBarrier)
{
    auto & ex = *state.executor;
    WaveScope wave(ex);

    std::atomic<bool> ran{false}, reclaimed{false};
    Executor::WorkItems work;
    work.emplace_back([&]() { ran.store(true, std::memory_order_release); }, uint8_t(0));
    wave.spawn(std::move(work));

    wave.barrierThenReclaim([&]() noexcept { reclaimed.store(true, std::memory_order_release); });

    EXPECT_TRUE(ran.load());
    EXPECT_TRUE(reclaimed.load());
    EXPECT_EQ(wave.completion->pending.load(), 0u);
}

/* F3: the reclamation runs EVEN IF the barrier rethrows (Finally-ordered) -- a
   thrown wave must not skip reclamation (no stripe leak). */
TEST_F(WaveS4baTest, barrierThenReclaimRunsReclaimEvenOnRethrow)
{
    auto & ex = *state.executor;
    WaveScope wave(ex);

    std::atomic<bool> reclaimed{false};
    Executor::WorkItems work;
    work.emplace_back([]() { throw Interrupted("boom"); }, uint8_t(0));
    wave.spawn(std::move(work));

    EXPECT_THROW(
        wave.barrierThenReclaim([&]() noexcept { reclaimed.store(true, std::memory_order_release); }), Interrupted);
    EXPECT_TRUE(reclaimed.load()) << "reclaim must run even when the barrier rethrows (F3 ordering)";
}

/* F4b: waveStripeOf recovers the correct GLOBAL id, and youngW/waveStripeSpan
   classify correctly, for a SPARSE set of participating stripe ids {0,3,7}. The
   synthetic wave uses a fake base and nStripes=8; no real arena/reclamation. */
TEST_F(WaveS4baTest, sparseStripeIdClassification)
{
    using namespace unforce_test;
    const size_t shift = stripeShiftForTest();
    /* A fake, suitably-aligned base; waveStripeOf/youngW only do arithmetic. */
    auto base = (const char *) (uintptr_t(1) << (shift + 4));
    const size_t n = 8; /* ids 0..7 */

    beginSyntheticWave(base, n);
    Finally end([]() { endSyntheticWave(); });

    /* The span covers the FULL reservation (max id 7), not a dense count. */
    EXPECT_EQ(waveStripeSpanForTest(), n << shift);

    for (size_t id : {size_t(0), size_t(3), size_t(7)}) {
        const char * p = base + (id << shift);          /* start of stripe `id` */
        const char * mid = p + (size_t(1) << (shift - 1)); /* mid-stripe */
        EXPECT_EQ(waveStripeOfForTest(p), id) << "id=" << id;
        EXPECT_EQ(waveStripeOfForTest(mid), id) << "id=" << id << " (mid)";
        EXPECT_TRUE(youngWForTest(p)) << "id=" << id;
        EXPECT_TRUE(youngWForTest(mid)) << "id=" << id << " (mid)";
    }

    /* Just below the base and just past the last stripe are OLD (not young). */
    EXPECT_FALSE(youngWForTest(base - 1));
    EXPECT_FALSE(youngWForTest(base + (n << shift)));
    /* A high participating id (7) is classified young -- the bug the span fix
       closes (a stripes.size()-based span would call it OLD). */
    EXPECT_TRUE(youngWForTest(base + (size_t(7) << shift) + 123));
}

} // namespace nix
