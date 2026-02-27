/**
 * Tests for Fix 2: Aggregated directory has-key-miss deps.
 *
 * When multiple readDir origins are merged (via //) and an absent key is probed
 * (hasAttr/intersectAttrs), the current code records one SC dep per directory.
 * After Fix 2, multiple directory origins should be aggregated into a single
 * DirSet dep with key "ds" in the JSON dep key.
 *
 * Non-directory origins (JSON/TOML) should still get individual deps.
 * exists=true always gets an individual dep (key found at specific origin).
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── Dep recording: aggregated directory has-key-miss ─────────────────

TEST_F(DepPrecisionTest, DirSet_HasKeyMiss_MultiDir_ProducesAggregateDep)
{
    // Three directories merged: d1 has "a", d2 has "b", d3 has "c".
    // Probing for absent key "missing" should produce ONE aggregated dep
    // instead of three individual deps.
    TempDir d1, d2, d3;
    d1.addFile("a", "");
    d2.addFile("b", "");
    d3.addFile("c", "");

    auto expr = std::format(
        R"(builtins.hasAttr "missing" ({} // {} // {}))",
        rd(d1.path()), rd(d2.path()), rd(d3.path()));

    auto deps = evalAndCollectDeps(expr);

    // After Fix 2: exactly 1 aggregated dep with "ds" key (not 3 individual deps)
    size_t dirSetDeps = countJsonDeps(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) { return j.contains("ds"); });
    EXPECT_EQ(dirSetDeps, 1u)
        << "Expected 1 aggregated DirSet dep, got " << dirSetDeps << "\n"
        << dumpDeps(deps);

    // Should NOT have individual per-dir has-key deps for "missing"
    size_t individualDirHasKey = countJsonDeps(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) {
            return j.contains("h") && j.value("t", "") == "d" && !j.contains("ds");
        });
    EXPECT_EQ(individualDirHasKey, 0u)
        << "Should not have individual per-dir has-key deps after aggregation\n"
        << dumpDeps(deps);

    // The aggregated dep should have the absent key name and hash("0")
    bool foundCorrectDep = hasJsonDep(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) {
            return j.contains("ds") && j.contains("h") && j["h"] == "missing";
        });
    EXPECT_TRUE(foundCorrectDep)
        << "Aggregated dep should contain key name 'missing'\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, DirSet_HasKeyMiss_MultiDir_DepValueIsHashZero)
{
    // The hash value for an absent key should be depHash("0")
    TempDir d1, d2;
    d1.addFile("a", "");
    d2.addFile("b", "");

    auto expr = std::format(
        R"(builtins.hasAttr "nope" ({} // {}))",
        rd(d1.path()), rd(d2.path()));

    auto deps = evalAndCollectDeps(expr);
    auto expectedHash = DepHashValue(depHash("0"));

    for (auto & d : deps) {
        auto j = parseDepKey(d);
        if (j && j->contains("ds") && j->value("h", "") == "nope") {
            EXPECT_EQ(d.expectedHash, expectedHash)
                << "Aggregated dep hash should be depHash(\"0\") for absent key";
            return;
        }
    }
    FAIL() << "Did not find aggregated DirSet dep for key 'nope'\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, DirSet_HasKeyHit_StillIndividualDep)
{
    // When a key IS found (exists=true), the dep should be individual, not aggregated.
    TempDir d1, d2;
    d1.addFile("found", "");
    d2.addFile("other", "");

    auto expr = std::format(
        R"(builtins.hasAttr "found" ({} // {}))",
        rd(d1.path()), rd(d2.path()));

    auto deps = evalAndCollectDeps(expr);

    // Should have an individual has-key dep for "found" with hash("1")
    bool foundHit = hasJsonDep(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) {
            return j.contains("h") && j["h"] == "found" && !j.contains("ds");
        });
    EXPECT_TRUE(foundHit)
        << "exists=true should produce individual dep (not aggregated)\n"
        << dumpDeps(deps);

    // Should NOT have aggregated dep for "found"
    bool foundAggregated = hasJsonDep(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) {
            return j.contains("ds") && j.value("h", "") == "found";
        });
    EXPECT_FALSE(foundAggregated)
        << "exists=true should NOT produce aggregated DirSet dep\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, DirSet_SingleDir_NoAggregation)
{
    // With only one directory origin, no aggregation should happen.
    TempDir d;
    d.addFile("a", "");
    d.addFile("b", "");

    auto expr = std::format(
        R"(builtins.hasAttr "missing" ({}))",
        rd(d.path()));

    auto deps = evalAndCollectDeps(expr);

    // Should have individual dep, not aggregated
    size_t dirSetDeps = countJsonDeps(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) { return j.contains("ds"); });
    EXPECT_EQ(dirSetDeps, 0u)
        << "Single directory should NOT produce aggregated dep\n"
        << dumpDeps(deps);

    // Should have individual has-key dep
    bool found = hasJsonDep(deps, DepType::StructuredContent, hasKeyPred("missing"));
    EXPECT_TRUE(found)
        << "Single directory should have individual has-key dep\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, DirSet_JsonOrigins_NeverAggregated)
{
    // JSON origins should never be aggregated, even if there are multiple.
    TempJsonFile f1(R"({"a":1})");
    TempJsonFile f2(R"({"b":2})");

    auto expr = std::format(
        R"(builtins.hasAttr "missing" ({} // {}))",
        fj(f1.path), fj(f2.path));

    auto deps = evalAndCollectDeps(expr);

    // Should NOT have any aggregated DirSet deps
    size_t dirSetDeps = countJsonDeps(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) { return j.contains("ds"); });
    EXPECT_EQ(dirSetDeps, 0u)
        << "JSON origins should never be aggregated\n"
        << dumpDeps(deps);

    // Should have individual has-key deps (one per JSON origin)
    size_t hasKeyDeps = countJsonDeps(deps, DepType::StructuredContent, hasKeyPred("missing"));
    EXPECT_EQ(hasKeyDeps, 2u)
        << "Should have 2 individual has-key deps for 2 JSON origins\n"
        << dumpDeps(deps);
}

TEST_F(DepPrecisionTest, DirSet_MixedDirAndJson_OnlyDirsAggregated)
{
    // Mixed origins: directories are aggregated, JSON gets individual dep.
    TempDir d1, d2;
    d1.addFile("a", "");
    d2.addFile("b", "");
    TempJsonFile f(R"({"c":3})");

    auto expr = std::format(
        R"(builtins.hasAttr "missing" ({} // {} // {}))",
        rd(d1.path()), rd(d2.path()), fj(f.path));

    auto deps = evalAndCollectDeps(expr);

    // Should have 1 aggregated DirSet dep for the 2 directories
    size_t dirSetDeps = countJsonDeps(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) { return j.contains("ds"); });
    EXPECT_EQ(dirSetDeps, 1u)
        << "2 directory origins should produce 1 aggregated dep\n"
        << dumpDeps(deps);

    // Should have 1 individual has-key dep for the JSON origin
    size_t jsonHasKey = countJsonDeps(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) {
            return j.contains("h") && j.value("t", "") == "j" && !j.contains("ds");
        });
    EXPECT_EQ(jsonHasKey, 1u)
        << "JSON origin should still have individual has-key dep\n"
        << dumpDeps(deps);
}

// ── IntersectAttrs absent recording ──────────────────────────────────

TEST_F(DepPrecisionTest, DirSet_IntersectAttrs_AbsentKeys_Aggregated)
{
    // intersectAttrs with multi-dir operand: absent keys should be aggregated.
    TempDir d1, d2;
    d1.addFile("a", "");
    d2.addFile("b", "");
    TempJsonFile f(R"({"x":1,"y":2})");

    auto expr = std::format(
        R"(builtins.intersectAttrs ({} // {}) ({}))",
        rd(d1.path()), rd(d2.path()), fj(f.path));

    auto deps = evalAndCollectDeps(expr);

    // "x" and "y" are absent from the dir merge → should produce aggregated deps
    // (each absent key gets its own aggregated dep but they share the same DirSet)
    size_t dirSetDeps = countJsonDeps(deps, DepType::StructuredContent,
        [](const nlohmann::json & j) { return j.contains("ds"); });
    EXPECT_GE(dirSetDeps, 1u)
        << "intersectAttrs absent keys should produce aggregated DirSet deps\n"
        << dumpDeps(deps);
}

// ── DirSet determinism ──────────────────────────────────────────────

TEST_F(DepPrecisionTest, DirSet_HashIsDeterministic)
{
    // The DirSet hash should be the same regardless of // operand order.
    TempDir d1, d2;
    d1.addFile("a", "");
    d2.addFile("b", "");

    auto expr1 = std::format(
        R"(builtins.hasAttr "missing" ({} // {}))",
        rd(d1.path()), rd(d2.path()));
    auto expr2 = std::format(
        R"(builtins.hasAttr "missing" ({} // {}))",
        rd(d2.path()), rd(d1.path()));

    auto deps1 = evalAndCollectDeps(expr1);
    auto deps2 = evalAndCollectDeps(expr2);

    // Extract the "ds" hash from each
    std::string dsHash1, dsHash2;
    for (auto & d : deps1) {
        auto j = parseDepKey(d);
        if (j && j->contains("ds")) { dsHash1 = (*j)["ds"].get<std::string>(); break; }
    }
    for (auto & d : deps2) {
        auto j = parseDepKey(d);
        if (j && j->contains("ds")) { dsHash2 = (*j)["ds"].get<std::string>(); break; }
    }

    ASSERT_FALSE(dsHash1.empty()) << "No DirSet dep found in expr1\n" << dumpDeps(deps1);
    ASSERT_FALSE(dsHash2.empty()) << "No DirSet dep found in expr2\n" << dumpDeps(deps2);
    EXPECT_EQ(dsHash1, dsHash2) << "DirSet hash should be deterministic regardless of // order";
}

} // namespace nix::eval_trace::test
