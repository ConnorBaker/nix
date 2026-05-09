/**
 * Concurrency tests for SqliteTraceStorage (D-1, D-3, G-6).
 *
 * D-1: Parallel attribute recording from N std::threads with no data race.
 *      Exercises the `SqliteTraceStorage::withExclusiveAccess` serialization path:
 *      each thread mints its own `bs`/`ea` and the top-level `storeMutex_`
 *      serializes writes. Real race coverage requires TSAN
 *      (-fsanitize=thread) in CI.
 *
 * D-3: Async write before destructor — orderly case: fut.get() before
 *      db.reset(). The actual race (destructor while write is in-flight)
 *      requires TSAN plus delay injection to expose reliably.
 *
 * G-6: Multi-process concurrent writes require a fork/subprocess harness.
 *      Documented as a TODO here; skipped at runtime with GTEST_SKIP().
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"

#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── D-1: Parallel attr recording, no data race ───────────────────────
//
// Spin up N threads each recording a distinct attr-path, then verify all.
// Each thread mints its own `Proof<BlockingTag>` via `withBs` (legitimate:
// the test thread IS a blocking thread) and then acquires the SqliteTraceStorage-wide
// `storeMutex_` via `withExclusiveAccess`. The mutex serializes the
// record/verify calls — this is the shared-store + mutex strategy, not the
// per-thread-store WAL strategy (see the original GTEST_SKIP history in
// src/libexpr-tests/eval-trace/CLAUDE.md).
//
// TSAN coverage: running this test under `-fsanitize=thread` confirms that
// the mutex eliminates the data race the old test was exposed to (16
// threads calling record() concurrently on the same SQLite connection).

TEST_F(TraceStoreTest, Concurrency_ParallelAttrs_NoDataRace)
{
    auto db = makeDb();
    constexpr int kThreads = 16;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; i++) {
        threads.emplace_back([this, &db, i]() {
            withBs([&](const auto & bs) {
                db->withExclusiveAccess(bs, [&](const auto & ea) {
                    auto p = vpath({"par", "attr-" + std::to_string(i)});
                    db->record(ea, p, string_t{"v-" + std::to_string(i), {}}, {});
                });
            });
        });
    }
    for (auto & t : threads)
        t.join();

    // Verify every attr-path records successfully and is retrievable.
    for (int i = 0; i < kThreads; i++) {
        auto p = vpath({"par", "attr-" + std::to_string(i)});
        auto result = test::TraceStorageTestAccess::verify(*db, p, state);
        EXPECT_TRUE(result.has_value())
            << "attr-" << i << " not verifiable after parallel record";
    }
}

// ── D-1b: Re-entrant `withExclusiveAccess` aborts (debug builds only) ──
//
// The `thread_local` depth counter in trace-store-lifecycle.cc asserts
// if a thread enters `withExclusiveAccess` on the same store twice. The
// recursive `storeMutex_` would otherwise silently permit re-entry,
// masking the call-graph bug. This test confirms the detector fires.
//
// EXPECT_DEATH re-executes the closure in a child process and expects
// it to terminate abnormally with the given regex in the message. Debug
// builds assert; release builds (NDEBUG) disable the assert, so the test
// is skipped on release.
#ifndef NDEBUG
TEST_F(TraceStoreTest, ReEntrantWithExclusiveAccess_Asserts)
{
    auto db = makeDb();
    EXPECT_DEATH({
        withExclusiveStore(*db, [&](const auto & ea) {
            db->withExclusiveAccess(ea.blockingProof(), [](auto &) {});
        });
    }, "re-entrant");
}
#endif

// ── D-3: Async write before destructor — orderly case ────────────────
//
// Launch an async task that holds a shared_ptr to the DB and records an
// entry. We call fut.get() before db.reset() so the write completes
// before the destructor runs. The test verifies the write is durable
// after the destructor/flush.
//
// The actual race (destructor invoked while write is still in-flight)
// requires TSAN plus injected delays to expose reliably. That is not
// tested here.
//
// makeDb() returns unique_ptr<SqliteTraceStorage>; move into shared_ptr for
// shared ownership with the async lambda.

TEST_F(TraceStoreTest, FlushTiming_AsyncWriteBeforeDestruct_Durable)
{
    auto db = std::shared_ptr<SqliteTraceStorage>(makeDb().release());
    auto fut = std::async(std::launch::async, [&] {
        withBs([&](const auto & bs) {
            db->withExclusiveAccess(bs, [&](const auto & ea) {
                auto p = vpath({"async-path"});
                db->record(ea, p, string_t{"av", {}}, {});
            });
        });
    });
    fut.get();   // join before reset so write completes
    db.reset();  // triggers destructor / flush
    auto db2 = makeDb();
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db2, vpath({"async-path"}), state).has_value());
}

// ── G-6: Multi-process concurrent writes (TODO) ──────────────────────
//
// Spawn a child process that writes to the same DB file concurrently.
// SQLite ON CONFLICT and INSERT OR IGNORE must prevent corruption under
// concurrent writers. Both processes must complete without DB corruption
// or assertion.
//
// Implementation requires a subprocess harness (posix_spawn + waitpid,
// or a helper binary called via std::system). Skipped until multi-process
// CI is available.
//
// TODO: implement when subprocess harness is available.

TEST_F(TraceStoreTest, ConcurrentUpserts_ParallelWrites_NoCorruption)
{
    GTEST_SKIP() << "Requires subprocess harness — implement when multi-process CI available";
}

} // namespace nix::eval_trace
