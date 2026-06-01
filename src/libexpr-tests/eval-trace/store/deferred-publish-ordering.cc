/**
 * Layer 2a deferred-publish ordering tests (async-producer-recording-plan §7,
 * redesign-plan follow-up #24).
 *
 * Layer 1 (`deferFlush`) batches the per-record entity `flush()`. Layer 2a
 * additionally defers the `publishStateChange` Sessions/History SQL write into
 * `pendingCurrentNodes`, drained by `flush()` STRICTLY AFTER the Traces rows.
 * This closes the #24 crash-consistency hazard: with Layer 1 alone, a producer's
 * durable Sessions/History(trace_id=N) row could be written while N's Traces row
 * stayed buffered, so a crash + trace-id reuse (`nextTraceId = MAX(Traces.id)`)
 * could alias the stale History row to a different trace. Layer 2a writes Traces
 * and Sessions/History in the same flush txn, Traces first — so they become
 * durable together (or are lost together on crash).
 *
 * These are DETERMINISTIC unit tests (no SIGKILL). They assert the observable
 * guarantees through the public `ea`-gated API:
 *   1. Within-session lookup works under deferral (currentNodeIndex stays
 *      synchronous even though the SQL write defers) — the soundness pivot that
 *      lets a later consumer resolve a just-recorded producer.
 *   2. After a clean flush (recreateDb), the deferred trace is durable (batched,
 *      not dropped) and the reopened DB is mutually consistent — every persisted
 *      current-node verifies, including across deferred/non-deferred interleaving.
 *
 * SCOPE — what these do NOT cover (verified, not assumed). These are REGRESSION
 * GUARDS for the deferred path's within-session + durability semantics; they do
 * NOT discriminate the #24 crash-ordering fix. All three PASS under Layer-1-alone
 * (defer flush, synchronous publish — the hazardous state) because a CLEAN flush
 * drains Traces and Sessions/History either way, so post-flush state is identical.
 * The #24 fix is only observable on the CRASH path (kill before teardown → with
 * Layer-1-alone the durable Sessions/History rows reference unflushed Traces;
 * with Layer 2a they are lost together, no dangling ref). That is proven
 * out-of-process: a mid-eval SIGKILL + reopen + dangling-trace_id query shows
 * Layer-1-alone leaves hundreds of dangling refs and Layer 2a leaves zero
 * (async-producer-recording-plan §7 validation log). An in-process unit test
 * cannot reach it — the destructor always flushes cleanly, so there is no
 * unflushed-Traces window to observe.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// Within-session: a trace recorded with deferFlush=true is immediately
// observable via the in-memory currentNodeIndex (attrExists / getCurrentTraceHash)
// even though its SQL writes are buffered. This is the property that lets a
// later consumer in the SAME session resolve a just-recorded producer.
TEST_F(TraceStoreTest, DeferredPublish_WithinSession_LookupSucceedsBeforeFlush)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        auto dep = makeContentDep(pools(), "/a.nix", "a");
        // deferFlush=true → entity flush AND publishStateChange both deferred.
        auto result = db->record(ea, rootPath(), string_t{"deferred", {}}, {dep},
                                 /*deferFlush=*/true);
        EXPECT_GT(result.traceId.value, 0u);

        // Despite the deferred SQL write, the in-memory current-node index must
        // already reflect the record: within-session lookup works.
        EXPECT_TRUE(db->attrExists(ea, rootPath()))
            << "deferred record must be visible within the session via currentNodeIndex";
        auto hash = db->getCurrentTraceHash(ea, rootPath());
        EXPECT_TRUE(hash.has_value())
            << "getCurrentTraceHash reads in-memory caches, must work under defer";
    });
}

// Cross-session: after a clean flush (destructor drains pending* including
// pendingCurrentNodes), the deferred trace is durable and verifies in a fresh
// store. Pins that deferral BATCHES the write rather than dropping it.
TEST_F(TraceStoreTest, DeferredPublish_CrossSession_DurableAfterFlush)
{
    ScopedEnvVar guard("DEFERRED_PUBLISH_VAR", "v0");
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        auto dep = makeEnvVarDep(pools(), "DEFERRED_PUBLISH_VAR", "v0");
        db->record(ea, rootPath(), string_t{"durable", {}}, {dep},
                   /*deferFlush=*/true);
    });
    // recreateDb destroys the store (flushExclusive drains the buffers) then
    // reopens against the same DB file — the standard clean-restart simulation.
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value())
        << "deferred trace must survive the flush+reopen (batched, not dropped)";
    assertCachedResultEquals(string_t{"durable", {}}, result->value, state.symbols);
}

// The #24 ordering invariant, checked deterministically: a mix of deferred and
// non-deferred records, drained by a clean flush, must leave the reopened DB
// mutually consistent — every persisted current-node resolves to a present
// trace. A Sessions/History row referencing a missing Traces row (the #24
// dangling reference) would make verify() miss or the load assert. Recording
// several paths, deferred and non-deferred interleaved, then verifying ALL of
// them after reopen exercises the same-txn drain ordering (Traces before
// Sessions/History) across the buffered set.
TEST_F(TraceStoreTest, DeferredPublish_InterleavedRecords_AllConsistentAfterFlush)
{
    constexpr int N = 6;
    std::vector<std::unique_ptr<ScopedEnvVar>> envGuards;
    envGuards.reserve(N);
    for (int i = 0; i < N; ++i)
        envGuards.push_back(std::make_unique<ScopedEnvVar>(
            "DEFPUB_VAR_" + std::to_string(i), "v" + std::to_string(i)));

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        for (int i = 0; i < N; ++i) {
            auto dep = makeEnvVarDep(pools(),
                "DEFPUB_VAR_" + std::to_string(i), "v" + std::to_string(i));
            // Alternate deferred / non-deferred. Non-deferred records flush(),
            // draining earlier deferred producers' Traces AND Sessions/History
            // together — the realistic interleaving the #24 fix must keep ordered.
            bool defer = (i % 2 == 0);
            db->record(ea, vpath({std::to_string(i)}),
                       string_t{std::to_string(i), {}}, {dep}, /*deferFlush=*/defer);
        }
    });
    recreateDb(db);

    // Every path — deferred or not — must persist with its own value. If the
    // drain ever wrote a Sessions row ahead of (or without) its Traces row,
    // the reopened verify for that path would miss.
    auto verifyDb = makeDb();
    for (int i = 0; i < N; ++i) {
        auto result = test::TraceStorageTestAccess::verify(
            *verifyDb, vpath({std::to_string(i)}), state);
        ASSERT_TRUE(result.has_value())
            << "path " << i << " must persist consistently (no dangling trace_id)";
        assertCachedResultEquals(
            string_t{std::to_string(i), {}}, result->value, state.symbols);
    }
}

} // namespace nix::eval_trace
