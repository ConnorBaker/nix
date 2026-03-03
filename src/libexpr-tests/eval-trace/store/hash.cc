#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"

#include <gtest/gtest.h>
#include <set>

#include "nix/expr/tests/libexpr.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepHashTest : public LibExprTest
{
public:
    InterningPools pools;

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
        makeEnvVarDep(pools, "B", "val1"),
        makeContentDep(pools, "/a.nix", "content1"),
    };
    std::vector<Dep> deps2 = {
        makeContentDep(pools, "/a.nix", "content2"),
        makeEnvVarDep(pools, "B", "val2"),
    };
    EXPECT_EQ(computeTraceStructHash(pools,deps1), computeTraceStructHash(pools,deps2));
}

TEST_F(DepHashTest, DepStructHash_SameKeysDifferentValues)
{
    // Same dep keys, different expectedHash values -> same structural hash (hashes dep structure, not content)
    std::vector<Dep> deps1 = {makeContentDep(pools, "/a.nix", "hello")};
    std::vector<Dep> deps2 = {makeContentDep(pools, "/a.nix", "world")};
    EXPECT_EQ(computeTraceStructHash(pools,deps1), computeTraceStructHash(pools,deps2));
}

TEST_F(DepHashTest, DepStructHash_DifferentKeys)
{
    // Different dep keys -> different structural hash
    std::vector<Dep> deps1 = {makeContentDep(pools, "/a.nix", "x")};
    std::vector<Dep> deps2 = {makeContentDep(pools, "/b.nix", "x")};
    EXPECT_NE(computeTraceStructHash(pools,deps1), computeTraceStructHash(pools,deps2));
}

TEST_F(DepHashTest, DepStructHash_EmptyDeps)
{
    // Empty deps -> consistent structural hash (empty trace structure)
    auto h1 = computeTraceStructHash(pools,{});
    auto h2 = computeTraceStructHash(pools,{});
    EXPECT_EQ(h1, h2);
}

// ── sortAndDedupDeps tests ───────────────────────────────────────────

TEST_F(DepHashTest, SortAndDedup_RemovesDuplicates)
{
    std::vector<Dep> deps = {
        makeContentDep(pools, "/a.nix", "a"),
        makeContentDep(pools, "/a.nix", "a"), // exact duplicate
        makeContentDep(pools, "/b.nix", "b"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(sorted.size(), 2u);
}

TEST_F(DepHashTest, SortAndDedup_SortsCorrectly)
{
    // Sorted by (type, sourceId, keyId) for deterministic trace hash computation.
    // Note: intra-type order is by interned integer ID, not lexicographic string.
    std::vector<Dep> deps = {
        makeEnvVarDep(pools, "Z", "val"),     // type=4
        makeContentDep(pools, "/z.nix", "c"), // type=1
        makeContentDep(pools, "/a.nix", "c"), // type=1
    };
    auto sorted = sortAndDedupDeps(deps);
    ASSERT_EQ(sorted.size(), 3u);
    // Content (type=1) before EnvVar (type=4)
    EXPECT_EQ(sorted[0].key.type, DepType::Content);
    EXPECT_EQ(sorted[1].key.type, DepType::Content);
    EXPECT_EQ(sorted[2].key.type, DepType::EnvVar);
    // Both Content keys present (order depends on intern IDs)
    std::set<std::string_view> contentKeys{
        pools.resolve(sorted[0].key.keyId), pools.resolve(sorted[1].key.keyId)};
    EXPECT_TRUE(contentKeys.count("/a.nix"));
    EXPECT_TRUE(contentKeys.count("/z.nix"));
}

TEST_F(DepHashTest, SortAndDedup_DedupByKeyNotHash)
{
    // Same dep key (type, source, key) but different expectedHash -> deduped (first wins)
    std::vector<Dep> deps = {
        makeContentDep(pools, "/a.nix", "content-v1"),
        makeContentDep(pools, "/a.nix", "content-v2"),
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
        makeEnvVarDep(pools, "B", "val"),
        makeContentDep(pools, "/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(computeTraceHash(pools,deps), computeTraceHashFromSorted(pools,sorted));
}

TEST_F(DepHashTest, PreSorted_StructHashMatchesUnsorted)
{
    std::vector<Dep> deps = {
        makeEnvVarDep(pools, "B", "val"),
        makeContentDep(pools, "/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(computeTraceStructHash(pools,deps), computeTraceStructHashFromSorted(pools,sorted));
}

} // namespace nix::eval_trace
