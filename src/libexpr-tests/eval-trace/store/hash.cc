#include "eval-trace/helpers.hh"
#include "nix/expr/trace-hash.hh"

#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepHashTest : public LibExprTest
{
public:
    DepHashTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {}
};

// ── computeTraceStructHash tests (BSàlC: structural hash of trace dep keys) ──

TEST_F(DepHashTest, DepStructHash_DeterministicOrdering)
{
    // Deps in different order should produce the same structural hash (order-independent)
    std::vector<Dep> deps1 = {
        makeEnvVarDep("B", "val1"),
        makeContentDep("/a.nix", "content1"),
    };
    std::vector<Dep> deps2 = {
        makeContentDep("/a.nix", "content2"),
        makeEnvVarDep("B", "val2"),
    };
    EXPECT_EQ(computeTraceStructHash(deps1), computeTraceStructHash(deps2));
}

TEST_F(DepHashTest, DepStructHash_SameKeysDifferentValues)
{
    // Same dep keys, different expectedHash values -> same structural hash (hashes dep structure, not content)
    std::vector<Dep> deps1 = {makeContentDep("/a.nix", "hello")};
    std::vector<Dep> deps2 = {makeContentDep("/a.nix", "world")};
    EXPECT_EQ(computeTraceStructHash(deps1), computeTraceStructHash(deps2));
}

TEST_F(DepHashTest, DepStructHash_DifferentKeys)
{
    // Different dep keys -> different structural hash
    std::vector<Dep> deps1 = {makeContentDep("/a.nix", "x")};
    std::vector<Dep> deps2 = {makeContentDep("/b.nix", "x")};
    EXPECT_NE(computeTraceStructHash(deps1), computeTraceStructHash(deps2));
}

TEST_F(DepHashTest, DepStructHash_EmptyDeps)
{
    // Empty deps -> consistent structural hash (empty trace structure)
    auto h1 = computeTraceStructHash({});
    auto h2 = computeTraceStructHash({});
    EXPECT_EQ(h1, h2);
}

// ── sortAndDedupDeps tests ───────────────────────────────────────────

TEST_F(DepHashTest, SortAndDedup_RemovesDuplicates)
{
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "a"),
        makeContentDep("/a.nix", "a"), // exact duplicate
        makeContentDep("/b.nix", "b"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(sorted.size(), 2u);
}

TEST_F(DepHashTest, SortAndDedup_SortsCorrectly)
{
    // Sorted by (type, source, key) for deterministic trace hash computation
    std::vector<Dep> deps = {
        makeEnvVarDep("Z", "val"),     // type=4
        makeContentDep("/z.nix", "c"), // type=1
        makeContentDep("/a.nix", "c"), // type=1
    };
    auto sorted = sortAndDedupDeps(deps);
    ASSERT_EQ(sorted.size(), 3u);
    // Content (type=1) before EnvVar (type=4)
    EXPECT_EQ(sorted[0].type, DepType::Content);
    EXPECT_EQ(sorted[0].key, "/a.nix");
    EXPECT_EQ(sorted[1].type, DepType::Content);
    EXPECT_EQ(sorted[1].key, "/z.nix");
    EXPECT_EQ(sorted[2].type, DepType::EnvVar);
}

TEST_F(DepHashTest, SortAndDedup_DedupByKeyNotHash)
{
    // Same dep key (type, source, key) but different expectedHash -> deduped (first wins)
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "content-v1"),
        makeContentDep("/a.nix", "content-v2"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(sorted.size(), 1u);
}

TEST_F(DepHashTest, SortAndDedup_EmptyInput)
{
    auto sorted = sortAndDedupDeps({});
    EXPECT_TRUE(sorted.empty());
}

// ── Pre-sorted variant equivalence tests (trace hash from sorted deps) ──

TEST_F(DepHashTest, PreSorted_ContentHashMatchesUnsorted)
{
    std::vector<Dep> deps = {
        makeEnvVarDep("B", "val"),
        makeContentDep("/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(computeTraceHash(deps), computeTraceHashFromSorted(sorted));
}

TEST_F(DepHashTest, PreSorted_StructHashMatchesUnsorted)
{
    std::vector<Dep> deps = {
        makeEnvVarDep("B", "val"),
        makeContentDep("/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(computeTraceStructHash(deps), computeTraceStructHashFromSorted(sorted));
}

} // namespace nix::eval_trace
