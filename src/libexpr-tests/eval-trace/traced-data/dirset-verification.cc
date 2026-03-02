/**
 * End-to-end verification tests for aggregated DirSet deps (Fix 2).
 *
 * Tests that aggregated directory deps correctly invalidate when a file
 * matching the absent key appears in any shard, and correctly validate
 * when no shard contains the key.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── Verification: aggregated DirSet deps ─────────────────────────────

TEST_F(DepPrecisionTest, DirSet_AbsentKey_NoChange_CacheHit)
{
    // Multiple dirs merged, absent key probed → cache should hit when nothing changes.
    TempDir d1, d2, d3;
    d1.addFile("a", "");
    d2.addFile("b", "");
    d3.addFile("c", "");

    auto expr = std::format(
        R"(builtins.hasAttr "missing" ({} // {} // {}))",
        rd(d1.path()), rd(d2.path()), rd(d3.path()));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "No change → cache hit (no re-evaluation)";
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(DepPrecisionTest, DirSet_AbsentKey_FileAddedToOneShard_CacheMiss)
{
    // Add a file named "missing" to one shard → the aggregated dep should fail.
    TempDir d1, d2, d3;
    d1.addFile("a", "");
    d2.addFile("b", "");
    d3.addFile("c", "");

    auto expr = std::format(
        R"(builtins.hasAttr "missing" ({} // {} // {}))",
        rd(d1.path()), rd(d2.path()), rd(d3.path()));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    // Add "missing" to d2
    d2.addFile("missing", "");
    INVALIDATE_DIR(d2);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "File matching absent key added → cache miss";
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepPrecisionTest, DirSet_AbsentKey_UnrelatedFileAdded_CacheHit)
{
    // Add a file with a DIFFERENT name → should NOT invalidate the absent-key dep.
    TempDir d1, d2;
    d1.addFile("a", "");
    d2.addFile("b", "");

    auto expr = std::format(
        R"(builtins.hasAttr "missing" ({} // {}))",
        rd(d1.path()), rd(d2.path()));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    // Add "unrelated" to d1 — different key, shouldn't affect "missing" dep
    d1.addFile("unrelated", "");
    INVALIDATE_DIR(d1);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "Adding unrelated file should NOT invalidate absent-key dep";
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(DepPrecisionTest, DirSet_AbsentKey_ShardRemoved_StillAbsent_CacheHit)
{
    // Remove a file from a shard (but not the absent key) → cache hit.
    TempDir d1, d2;
    d1.addFile("a", "");
    d1.addFile("extra", "");
    d2.addFile("b", "");

    auto expr = std::format(
        R"(builtins.hasAttr "missing" ({} // {}))",
        rd(d1.path()), rd(d2.path()));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    // Remove "extra" from d1 — key "missing" is still absent everywhere
    d1.removeEntry("extra");
    INVALIDATE_DIR(d1);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "Removing unrelated file should NOT invalidate absent-key dep";
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(DepPrecisionTest, DirSet_MultipleAbsentKeys_IndependentAggregation)
{
    // Two different absent keys on the same merged dirs → each gets its own
    // aggregated dep. Changing one shouldn't affect the other.
    TempDir d1, d2;
    d1.addFile("a", "");
    d2.addFile("b", "");

    auto expr = std::format(
        R"(let m = {} // {}; in {{ x = m ? "keyX"; y = m ? "keyY"; }})",
        rd(d1.path()), rd(d2.path()));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * x = root.attrs()->get(state.symbols.create("x"));
        auto * y = root.attrs()->get(state.symbols.create("y"));
        ASSERT_NE(x, nullptr);
        ASSERT_NE(y, nullptr);
        state.forceValue(*x->value, noPos);
        state.forceValue(*y->value, noPos);
        EXPECT_THAT(*x->value, IsFalse());
        EXPECT_THAT(*y->value, IsFalse());
    }

    // Add "keyX" to d1 → x becomes true, y should still be cached
    d1.addFile("keyX", "");
    INVALIDATE_DIR(d1);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * x = root.attrs()->get(state.symbols.create("x"));
        auto * y = root.attrs()->get(state.symbols.create("y"));
        ASSERT_NE(x, nullptr);
        ASSERT_NE(y, nullptr);
        state.forceValue(*x->value, noPos);
        state.forceValue(*y->value, noPos);

        // x should now be true (re-evaluated)
        EXPECT_THAT(*x->value, IsTrue());
        // y should still be false (keyY still absent)
        EXPECT_THAT(*y->value, IsFalse());
    }
}

// ── Dep count verification ───────────────────────────────────────────

TEST_F(DepPrecisionTest, DirSet_DepCountReduction_ManyDirs)
{
    // 10 directories merged, absent key → should produce exactly 1 aggregated dep
    // instead of 10 individual deps.
    std::vector<std::unique_ptr<TempDir>> dirs;
    std::string mergeExpr;
    for (int i = 0; i < 10; i++) {
        dirs.push_back(std::make_unique<TempDir>());
        dirs.back()->addFile("entry" + std::to_string(i), "");
        if (i > 0) mergeExpr += " // ";
        mergeExpr += rd(dirs.back()->path());
    }

    auto expr = std::format(R"(builtins.hasAttr "absent" ({}))", mergeExpr);
    auto deps = evalAndCollectDeps(expr);

    // Count NON-aggregated directory-format has-key deps for "absent"
    size_t dirHasKeyDeps = countJsonDeps(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) {
            return j.value("t", "") == "d" && j.contains("h") && j["h"] == "absent"
                && !j.contains("ds");
        });
    size_t aggregatedDeps = countJsonDeps(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) { return j.contains("ds"); });

    // After Fix 2: 0 individual dir deps + 1 aggregated dep
    EXPECT_EQ(dirHasKeyDeps, 0u)
        << "Should have 0 individual dir has-key deps (all aggregated)\n"
        << dumpDeps(deps);
    EXPECT_EQ(aggregatedDeps, 1u)
        << "10 directories should produce exactly 1 aggregated DirSet dep\n"
        << dumpDeps(deps);
}

// ── Stored deps exact comparison ─────────────────────────────────────

TEST_F(MaterializationDepTest, DirSet_StoredDeps_ContainAggregateDep)
{
    // Verify that the stored trace contains the aggregated dep, not individual ones.
    TempDir d1, d2;
    d1.addFile("a", "");
    d2.addFile("b", "");

    auto expr = std::format(
        R"(builtins.hasAttr "absent" ({} // {}))",
        rd(d1.path()), rd(d2.path()));

    {
        auto cache = makeCache(expr);
        forceRoot(*cache);
    }

    auto storedDeps = getStoredDeps("");
    ASSERT_FALSE(storedDeps.empty()) << "Root trace should have stored deps";

    // Check stored deps for aggregated DirSet dep
    size_t aggregated = 0;
    size_t individualDirHasKey = 0;
    for (auto & d : storedDeps) {
        if (d.type != DepType::StructuredContent) continue;
        auto j = DepPrecisionTest::parseDepKey(d);
        if (!j) continue;
        if (j->contains("ds") && j->value("h", "") == "absent")
            aggregated++;
        if (j->value("t", "") == "d" && j->contains("h") && j->value("h", "") == "absent" && !j->contains("ds"))
            individualDirHasKey++;
    }

    EXPECT_EQ(aggregated, 1u)
        << "Stored trace should contain 1 aggregated DirSet dep\n"
        << dumpDeps(storedDeps);
    EXPECT_EQ(individualDirHasKey, 0u)
        << "Stored trace should NOT contain individual dir has-key deps\n"
        << dumpDeps(storedDeps);
}

} // namespace nix::eval_trace::test
