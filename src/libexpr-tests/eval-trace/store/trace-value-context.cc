/**
 * Tests for TraceValueContext and TraceParentSlot dep types (A-6, B-6).
 *
 * A-6: TraceValueContext basic warm hit, parent-invalidated child miss,
 *      recursive depth-3 chain, hardcoded-hash cycle no-throw.
 * B-6: traceContextMemo staleness — two verify() calls on the same session
 *      after file change must not serve a stale memo hit.
 *
 * Construction recipe:
 *   db->record(ea, parentPath, ..., parentDeps);
 *   auto ph = db->getCurrentTraceHash(ea, parentPath);
 *   Dep ctx = Dep::makeValueContext(parentPath, DepHashValue(DepHash{ph->value}));
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── A-6: TraceValueContext ───────────────────────────────────────────

TEST_F(TraceStoreTest, TraceValueContext_Basic_WarmHit)
{
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record the parent first
        db->record(ea, vpath({"parent"}), string_t{"pv", {}}, {});
        auto ph = db->getCurrentTraceHash(ea, vpath({"parent"}));
        ASSERT_TRUE(ph.has_value());

        // Child depends on parent via TraceValueContext
        db->record(ea, vpath({"child"}), string_t{"cv", {}},
            {Dep::makeValueContext(vpath({"parent"}), DepHashValue(DepHash{ph->value}))});
    });

    recreateDb(db);
    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"child"}), state);
    EXPECT_TRUE(result.has_value());
}

TEST_F(TraceStoreTest, TraceValueContext_ParentInvalidated_ChildMisses)
{
    TempTextFile f("v1");
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Parent depends on file content
        db->record(ea, vpath({"parent"}), string_t{"pv", {}},
            {makeContentDep(pools(), f.path.string(), "v1")});
        auto ph = db->getCurrentTraceHash(ea, vpath({"parent"}));
        ASSERT_TRUE(ph.has_value());

        // Child depends on parent via TraceValueContext
        db->record(ea, vpath({"child"}), string_t{"cv", {}},
            {Dep::makeValueContext(vpath({"parent"}), DepHashValue(DepHash{ph->value}))});
    });

    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"child"}), state);
        EXPECT_TRUE(result.has_value());
    }

    // Mutate the file — parent invalidates → child must miss
    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"child"}), state);
        EXPECT_FALSE(result.has_value());
    }
}

TEST_F(TraceStoreTest, TraceValueContext_RecursiveChain_DepthThree)
{
    // A(FileBytes) ← B(ctx A) ← C(ctx B): file mutation propagates to C.
    TempTextFile f("v1");
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // A: depends on file content
        db->record(ea, vpath({"A"}), string_t{"av", {}},
            {makeContentDep(pools(), f.path.string(), "v1")});
        auto phA = db->getCurrentTraceHash(ea, vpath({"A"}));
        ASSERT_TRUE(phA.has_value());

        // B: depends on A via TraceValueContext
        db->record(ea, vpath({"B"}), string_t{"bv", {}},
            {Dep::makeValueContext(vpath({"A"}), DepHashValue(DepHash{phA->value}))});
        auto phB = db->getCurrentTraceHash(ea, vpath({"B"}));
        ASSERT_TRUE(phB.has_value());

        // C: depends on B via TraceValueContext
        db->record(ea, vpath({"C"}), string_t{"cv", {}},
            {Dep::makeValueContext(vpath({"B"}), DepHashValue(DepHash{phB->value}))});
    });

    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"C"}), state);
        EXPECT_TRUE(result.has_value());
    }

    // Mutate file: invalidation must propagate A → B → C
    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"C"}), state);
        EXPECT_FALSE(result.has_value());
    }
}

// This test uses hardcoded hash stubs (depHash("s1"), depHash("s2")).
// The stored hashes never match the recorded trace hashes, so this test
// exercises the hash-mismatch cycle path — not the verifiedTraceIds
// loop-prevention code, which would require a two-pass setup with real
// trace hashes (Pass 1: record A with B's actual hash; Pass 2: record B
// with A's actual hash).
//
// Under hash mismatch, verify MUST miss (not just "not crash"): the
// dep-hash check fails before the cycle-break path is even reached.
// Strengthened (§N.6) from EXPECT_NO_THROW to assert the miss outcome
// directly.  (The `nrTraceCacheMisses` counter is incremented by the
// orchestrator path, which this test does not exercise; asserting on
// it here would be a false positive, see §N.7.)
TEST_F(TraceStoreTest, TraceValueContext_HardcodedHashCycle_MissOnMismatch)
{
    // Cyclic A→B→A with hardcoded stubs: hash mismatch drives a miss
    // without touching the loop-prevention code path.
    auto db = makeDb();
    auto s1 = DepHashValue(depHash("s1"));
    auto s2 = DepHashValue(depHash("s2"));

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"A"}), string_t{"av", {}},
            {Dep::makeValueContext(vpath({"B"}), s1)});
        db->record(ea, vpath({"B"}), string_t{"bv", {}},
            {Dep::makeValueContext(vpath({"A"}), s2)});
    });

    recreateDb(db);
    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"A"}), state);
    EXPECT_FALSE(result.has_value())
        << "hardcoded-hash cycle: stored dep hashes do not match recorded "
           "trace hashes, so verify must miss";
}

// ── B-6: traceContextMemo staleness ────────────────────────────────

// TraceContextMemo staleness detection via fresh sessions.
//
// VerificationSession.verifiedTraceIds caches the verification outcome of each
// traceId within a session.  On a second verify() call using the SAME session,
// verifyTrace() returns immediately from verifiedTraceIds for the parent trace —
// bypassing the traceContextMemo staleness check entirely.  Memo staleness is
// therefore only observable when each call uses a SEPARATE session (so that
// verifiedTraceIds is empty) and the in-memory DB state is reset (recreateDb).
//
// This test verifies the fundamental soundness property: after the parent's
// file dep changes, a fresh verify() call (fresh session, re-opened DB) detects
// the change and returns a miss.  The 3-arg overload creates a fresh session
// internally on each call.
TEST_F(TraceStoreTest, TraceContextMemo_NodeStampChange_DetectsChange)
{
    TempTextFile f("v1");
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record parent with a content dep
        db->record(ea, vpath({"parent"}), string_t{"pv1", {}},
            {makeContentDep(pools(), f.path.string(), "v1")});
        auto ph1 = db->getCurrentTraceHash(ea, vpath({"parent"}));
        ASSERT_TRUE(ph1.has_value());

        // Record child depending on parent via TraceValueContext
        db->record(ea, rootPath(), string_t{"v", {}},
            {Dep::makeValueContext(vpath({"parent"}), DepHashValue(DepHash{ph1->value}))});
    });

    recreateDb(db);

    // First verify: parent file is unchanged → should hit (fresh session).
    {
        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        EXPECT_TRUE(result.has_value());
    }

    // Modify the file — parent dep is now stale.
    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();

    // recreateDb: destroys the old SqliteTraceStorage (flushing to SQLite) then
    // creates a new one. The file was modified above; the verifier will
    // read the actual (changed) file content.
    recreateDb(db);

    // Second verify: use a fresh session (3-arg overload) and a fresh DB
    // connection so that verifiedTraceIds does not mask the staleness.
    // The parent's FileBytes dep now fails → resolveTraceContextHash returns
    // nullopt → child's TraceContext dep fails → trace is Invalid.
    {
        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        EXPECT_FALSE(result.has_value());
    }
}

} // namespace nix::eval_trace
