/* Inc3 S4-a wave-scoped quiescence join (plumbing, revert-OFF).

   White-box proof that the barrier reaches transitive quiescence and that the
   ambient-wave (`currentWave`) identity propagates across the spawn / pick-up
   boundary -- the two load-bearing properties of the join:

     - `WaveScope::spawn` increments `pending` before queueing (the same
       increment-before-queue as `FutureVector::spawn`), so `pending == 0` at
       `barrier()` means every item bound to the wave AND everything those items
       transitively spawned into it has run. The barrier blocks until then.

     - A worker running a wave item recovers the wave from `item.completion->wave`
       and makes it ambient (`Executor::currentWave`) for the item's duration, so
       a NESTED spawn on that worker (the `prim_parallel`-inside-a-wave-element
       case) inherits the SAME wave and is counted + awaited transitively.

   These are exercised directly on the Executor (one worker drains the queue even
   at eval-cores == 1, where `enabled` is false -- `enabled` only gates the
   builtins' dispatch, not the worker loop), so no values are evaluated and the
   test is deterministic. Teeth: if `barrier()` returned before `pending == 0`,
   or if the nested item did not inherit the wave (so its spawn was never
   counted), the observed counter would be short of the expected total and the
   EXPECT_EQ below would fail. */

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/parallel-eval.hh"

#include <atomic>

#include <gtest/gtest.h>

namespace nix {

class WaveQuiescenceTest : public LibExprTest
{};

/* A flat wave: N items spawned into the wave; the barrier must await all N. */
TEST_F(WaveQuiescenceTest, barrierAwaitsAllFlatItems)
{
    WaveScope wave(*state.executor);

    std::atomic<uint64_t> ran{0};
    constexpr uint64_t N = 256;

    Executor::WorkItems work;
    for (uint64_t i = 0; i < N; ++i)
        work.emplace_back([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); }, uint8_t(0));
    wave.spawn(std::move(work));

    wave.barrier();

    EXPECT_EQ(ran.load(), N);
    EXPECT_EQ(wave.completion->pending.load(), 0u);
}

/* Transitive quiescence: each parent item, while running on a worker, inherits
   the wave via `currentWave` and spawns a child into it. The barrier must await
   the children too (they were created by an already-counted running item, which
   increments before it can queue, so `pending` never transiently hits 0 while a
   descendant is outstanding). Also proves the propagation: a parent that did not
   see the wave ambient records a miss. */
TEST_F(WaveQuiescenceTest, barrierAwaitsTransitivelySpawnedItems)
{
    WaveScope wave(*state.executor);

    std::atomic<uint64_t> ran{0};
    std::atomic<uint64_t> propagationMisses{0};
    constexpr uint64_t N = 128;

    Executor::WorkItems work;
    for (uint64_t i = 0; i < N; ++i)
        work.emplace_back(
            [&]() {
                ran.fetch_add(1, std::memory_order_relaxed);
                /* The wave must be ambient for this item (F2 propagation). */
                WaveScope * w = Executor::currentWave;
                if (w != &wave) {
                    propagationMisses.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                /* Nested spawn, inheriting the wave -- the
                   `prim_parallel`-inside-an-element shape. */
                Executor::WorkItems child;
                child.emplace_back([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); }, uint8_t(0));
                w->spawn(std::move(child));
            },
            uint8_t(0));
    wave.spawn(std::move(work));

    wave.barrier();

    EXPECT_EQ(propagationMisses.load(), 0u) << "a wave item did not inherit its wave via currentWave";
    EXPECT_EQ(ran.load(), 2 * N) << "the barrier returned before transitively-spawned children finished";
    EXPECT_EQ(wave.completion->pending.load(), 0u);
}

/* The ambient wave is set per item and restored to the PREVIOUS value after it,
   so a worker that runs wave A's item and later wave B's item sees each item
   under its own wave -- never a stale one. If the Finally restored to a fixed
   value (or not at all), B's items would observe A. */
TEST_F(WaveQuiescenceTest, ambientWavePerItemNotStale)
{
    std::atomic<uint64_t> wrongA{0}, wrongB{0};
    constexpr uint64_t N = 64;

    WaveScope waveA(*state.executor);
    {
        Executor::WorkItems work;
        for (uint64_t i = 0; i < N; ++i)
            work.emplace_back(
                [&]() {
                    if (Executor::currentWave != &waveA)
                        wrongA.fetch_add(1, std::memory_order_relaxed);
                },
                uint8_t(0));
        waveA.spawn(std::move(work));
    }
    waveA.barrier();

    WaveScope waveB(*state.executor);
    {
        Executor::WorkItems work;
        for (uint64_t i = 0; i < N; ++i)
            work.emplace_back(
                [&]() {
                    if (Executor::currentWave != &waveB)
                        wrongB.fetch_add(1, std::memory_order_relaxed);
                },
                uint8_t(0));
        waveB.spawn(std::move(work));
    }
    waveB.barrier();

    EXPECT_EQ(wrongA.load(), 0u) << "wave A item did not run under wave A";
    EXPECT_EQ(wrongB.load(), 0u) << "wave B item ran under a stale ambient wave (restore broken)";
}

} // namespace nix
