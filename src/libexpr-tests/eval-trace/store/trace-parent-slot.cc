/**
 * Tests for TraceParentSlot dep type (A-7).
 *
 * A-7: hash-match warm hit, parent-change invalidation,
 *      routing through resolveTraceContextHash (same path for both
 *      TraceValueContext and TraceParentSlot).
 *
 * Construction recipe:
 *   db->record(ea, parentPath, ..., {});
 *   auto ph = db->getCurrentTraceHash(ea, parentPath);
 *   Dep slot = Dep::makeParentSlot(ParentSlot{parentPath}, DepHashValue(DepHash{ph->value}));
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

TEST_F(TraceStoreTest, TraceParentSlot_HashMatch_WarmHit)
{
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record parent with no deps
        db->record(ea, vpath({"parent"}), string_t{"pv", {}}, {});
        auto ph = db->getCurrentTraceHash(ea, vpath({"parent"}));
        ASSERT_TRUE(ph.has_value());

        // Child depends on parent via TraceParentSlot
        db->record(ea, vpath({"child"}), string_t{"cv", {}},
            {Dep::makeParentSlot(ParentSlot{vpath({"parent"})}, DepHashValue(DepHash{ph->value}))});
    });

    recreateDb(db);
    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"child"}), state);
    EXPECT_TRUE(result.has_value());
}

TEST_F(TraceStoreTest, TraceParentSlot_ParentChanged_Invalidates)
{
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record parent v1 with no deps
        db->record(ea, vpath({"parent"}), string_t{"pv1", {}}, {});
        auto h1 = db->getCurrentTraceHash(ea, vpath({"parent"}));
        ASSERT_TRUE(h1.has_value());

        // Child points at parent hash H1
        db->record(ea, vpath({"child"}), string_t{"cv", {}},
            {Dep::makeParentSlot(ParentSlot{vpath({"parent"})}, DepHashValue(DepHash{h1->value}))});
    });

    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"child"}), state);
        EXPECT_TRUE(result.has_value());
    }

    // Re-record parent with a new content dep → new trace hash H2 ≠ H1
    TempTextFile f("t");
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"parent"}), string_t{"pv2", {}},
            {makeContentDep(pools(), f.path.string(), "t")});
    });

    recreateDb(db);
    // Child's stored hash H1 no longer matches parent's new hash H2 → miss
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"child"}), state);
        EXPECT_FALSE(result.has_value());
    }
}

TEST_F(TraceStoreTest, TraceParentSlot_BothContextTypes_SameInvalidation)
{
    // Both CQK types (TraceValueContext, TraceParentSlot) share
    // resolveTraceContextHash; one parent mutation invalidates both.
    TempTextFile f("v1");
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record parent with a content dep
        db->record(ea, vpath({"parent"}), string_t{"pv", {}},
            {makeContentDep(pools(), f.path.string(), "v1")});
        auto ph = db->getCurrentTraceHash(ea, vpath({"parent"}));
        ASSERT_TRUE(ph.has_value());

        // Child has BOTH a TraceValueContext dep AND a TraceParentSlot dep
        // pointing at the same parent
        db->record(ea, vpath({"child"}), string_t{"cv", {}}, {
            Dep::makeValueContext(vpath({"parent"}), DepHashValue(DepHash{ph->value})),
            Dep::makeParentSlot(ParentSlot{vpath({"parent"})}, DepHashValue(DepHash{ph->value})),
        });
    });

    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"child"}), state);
        EXPECT_TRUE(result.has_value());
    }

    // Mutate file → parent trace hash changes → both deps become stale
    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"child"}), state);
        EXPECT_FALSE(result.has_value());
    }
}

} // namespace nix::eval_trace
