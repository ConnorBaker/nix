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
#include "nix/util/finally.hh"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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
/* F4a re-home (Inc3 S4-b-a1) test hook. */
void clearSyntheticWorkers();
void injectSyntheticWorker(uint32_t id, size_t bytes);
void runRehomeForTest();
size_t lastRehomeStripes();
bool rehomeRegistryHasStripe(uint32_t id);
const void * rehomeRegistryStripeBase(uint32_t id);
/* Inc3 S4-b-b1 (F4c) delimitation hooks. */
void appendSyntheticUndoEntry(uint32_t id, size_t off);
void setSyntheticRegionCur(uint32_t id, size_t bytes);
void setWaveStartMarkerForTest(uint32_t id, size_t startCurOff, size_t undoMark);
void captureWaveStartForTest(bool executorIdle);
size_t lastRehomeSliceEntries();
bool lastRehomeDelimitationValid();
size_t rehomeSliceSize(uint32_t id);
size_t waveSegStartOff(uint32_t id);
void promoteSplitForTest(
    const std::vector<size_t> & targetOffs, const std::vector<uint32_t> & counts, uint32_t stripeId,
    size_t & promotedCount, size_t & revertibleCount);
uint32_t promoteAfterForTest();
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

/* F4a (Inc3 S4-b-a1): the cross-worker re-home indexes the registry by GLOBAL
   stripe id (sparse). Inject synthetic participating workers at sparse ids
   {1,4,7}, run the re-home, and check each landed at its global index with the
   right stripe base, and non-participating ids are empty slots. (Validates the
   same `rehomeWorkerStripesImpl` the real barrier runs; the real-eval run also
   asserts `region.base == stripeBase(id)` for the actual M workers.) */
TEST_F(WaveS4baTest, f4aRehomeSparseWorkers)
{
    using namespace unforce_test;
    const size_t shift = stripeShiftForTest();
    auto base = (const char *) (uintptr_t(1) << (shift + 4));
    const size_t n = 8; /* ids 0..7 */

    beginSyntheticWave(base, n);
    Finally end([]() {
        endSyntheticWave();
        clearSyntheticWorkers();
    });
    clearSyntheticWorkers();

    injectSyntheticWorker(1, size_t(1) << 20);
    injectSyntheticWorker(4, size_t(2) << 20);
    injectSyntheticWorker(7, size_t(3) << 20);

    runRehomeForTest();

    EXPECT_EQ(lastRehomeStripes(), 3u);
    for (uint32_t id : {uint32_t(1), uint32_t(4), uint32_t(7)}) {
        EXPECT_TRUE(rehomeRegistryHasStripe(id)) << "id=" << id;
        EXPECT_EQ(rehomeRegistryStripeBase(id), (const void *) (base + (size_t(id) << shift))) << "id=" << id;
    }
    for (uint32_t id : {uint32_t(0), uint32_t(2), uint32_t(3), uint32_t(5), uint32_t(6)})
        EXPECT_FALSE(rehomeRegistryHasStripe(id)) << "non-participating id=" << id;
}

/* ===== Inc3 S4-b-b1 (F4c): per-wave foam delimitation =====================
   A worker's scope is lifetime, so its undoLog/region accumulate across waves.
   These tests drive the real `captureWaveStartImpl` + `rehomeWorkerStripesImpl`
   on synthetic workers: the wave-open mark delimits this wave's slice `[mark,
   current)`, and `[base, mark)` (prior waves) is excluded by construction. The
   synthetic targets are opaque addresses carved from the stripe (never
   dereferenced by JobTargets / the delimitation). Granule = 16 bytes/cell. */

struct F4cFixture : WaveS4baTest
{
    static constexpr size_t G = 16; /* bytes per synthetic cell */
    const char * base = nullptr;
    size_t shift = 0;
    void begin()
    {
        shift = unforce_test::stripeShiftForTest();
        base = (const char *) (uintptr_t(1) << (shift + 4));
        unforce_test::beginSyntheticWave(base, 8);
        unforce_test::clearSyntheticWorkers();
    }
    void end()
    {
        unforce_test::endSyntheticWave();
        unforce_test::clearSyntheticWorkers();
    }
};

/* The slice is EXACTLY this wave's entries; the prior-wave prefix is excluded. */
TEST_F(F4cFixture, delimitsThisWaveSlice)
{
    using namespace unforce_test;
    begin();
    Finally fin([&]() { end(); });

    /* 10 prior-wave cells already forced: region.cur at 10*G, undoLog has 10. */
    injectSyntheticWorker(1, 10 * G);
    for (size_t k = 0; k < 10; ++k)
        appendSyntheticUndoEntry(1, k * G);

    /* wave-open mark (quiescent): startCur = 10*G, undoMark = 10. */
    captureWaveStartForTest(/*executorIdle=*/true);

    /* 5 this-wave cells forced: cur -> 15*G, 5 more undo entries at [10*G, 15*G). */
    setSyntheticRegionCur(1, 15 * G);
    for (size_t k = 10; k < 15; ++k)
        appendSyntheticUndoEntry(1, k * G);

    runRehomeForTest();

    EXPECT_TRUE(lastRehomeDelimitationValid());
    EXPECT_EQ(rehomeSliceSize(1), 5u);        /* only this wave's 5 entries */
    EXPECT_EQ(lastRehomeSliceEntries(), 5u);
    EXPECT_EQ(waveSegStartOff(1), 10 * G);    /* segment begins after the prior wave */
}

/* TEETH: a wrong delimitation (undoMark forgotten -> 0, startCur still correct)
   sweeps prior-wave young cells into the slice -> the young-address invariant
   must trip (delimitation reported INVALID). */
TEST_F(F4cFixture, priorWaveInclusionTripsInvariant)
{
    using namespace unforce_test;
    begin();
    Finally fin([&]() { end(); });

    injectSyntheticWorker(1, 15 * G);
    for (size_t k = 0; k < 15; ++k)
        appendSyntheticUndoEntry(1, k * G);

    /* Inconsistent mark: startCur says this wave began at 10*G, but undoMark=0
       claims the whole log -> the 10 prior young cells (< startCur) sneak in. */
    setWaveStartMarkerForTest(1, /*startCurOff=*/10 * G, /*undoMark=*/0);

    runRehomeForTest();

    EXPECT_FALSE(lastRehomeDelimitationValid()); /* teeth bite */
    EXPECT_EQ(rehomeSliceSize(1), 15u);          /* the (wrong) whole-log slice */
}

/* MULTI-WAVE: wave K+1's slice contains NONE of wave K's cells (the coordinator's
   crux -- a later wave's barrier must not reclaim an earlier wave's retained
   foam). The second wave's mark is captured at the first wave's end. */
TEST_F(F4cFixture, multiWaveSlicesAreDisjoint)
{
    using namespace unforce_test;
    begin();
    Finally fin([&]() { end(); });

    injectSyntheticWorker(1, 0); /* fresh worker, nothing forced yet */

    /* Wave 1: mark at START (empty), force 4 cells, barrier. */
    captureWaveStartForTest(true); /* startCur = 0, undoMark = 0 */
    setSyntheticRegionCur(1, 4 * G);
    for (size_t k = 0; k < 4; ++k)
        appendSyntheticUndoEntry(1, k * G);
    runRehomeForTest();
    EXPECT_TRUE(lastRehomeDelimitationValid());
    EXPECT_EQ(rehomeSliceSize(1), 4u);
    EXPECT_EQ(waveSegStartOff(1), 0u);

    /* Wave 2: mark at wave 1's END, force 3 more cells, barrier. */
    captureWaveStartForTest(true); /* startCur = 4*G, undoMark = 4 */
    setSyntheticRegionCur(1, 7 * G);
    for (size_t k = 4; k < 7; ++k)
        appendSyntheticUndoEntry(1, k * G);
    runRehomeForTest();

    EXPECT_TRUE(lastRehomeDelimitationValid());
    EXPECT_EQ(rehomeSliceSize(1), 3u);     /* wave 2 only, NOT 7 */
    EXPECT_EQ(waveSegStartOff(1), 4 * G);  /* disjoint: starts where wave 1 ended */
}

/* PROMOTED/SHARED exclusion: a cell forced in >= promoteAfter jobs is nominated
   (promoted -> copied to OLD, EXCLUDED from the revertible foam); a once-forced
   cell is revertible. Validates the real `countTargets` split the barrier uses. */
TEST_F(F4cFixture, promotedCellsExcludedFromRevertible)
{
    using namespace unforce_test;
    begin();
    Finally fin([&]() { end(); });

    const uint32_t pa = promoteAfterForTest();
    ASSERT_GE(pa, 2u);

    /* counts: {1, pa, 1, pa+1, 1} -> promoted at indices 1 and 3 (shared). */
    std::vector<size_t> offs{0 * G, 1 * G, 2 * G, 3 * G, 4 * G};
    std::vector<uint32_t> counts{1, pa, 1, pa + 1, 1};
    size_t promoted = 0, revertible = 0;
    promoteSplitForTest(offs, counts, 1, promoted, revertible);
    EXPECT_EQ(promoted, 2u);
    EXPECT_EQ(revertible, 3u);

    /* all private -> all revertible, none promoted. */
    std::vector<uint32_t> allOnce{1, 1, 1, 1, 1};
    promoteSplitForTest(offs, allOnce, 1, promoted, revertible);
    EXPECT_EQ(promoted, 0u);
    EXPECT_EQ(revertible, 5u);

    /* all shared -> all promoted, none revertible. */
    std::vector<uint32_t> allShared{pa, pa, pa, pa, pa};
    promoteSplitForTest(offs, allShared, 1, promoted, revertible);
    EXPECT_EQ(promoted, 5u);
    EXPECT_EQ(revertible, 0u);
}

} // namespace nix
