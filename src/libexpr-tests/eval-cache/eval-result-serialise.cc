#include "helpers.hh"
#include "nix/expr/eval-result-serialise.hh"

#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"

namespace nix::eval_cache {

using namespace nix::eval_cache::test;

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

// ── computeDepStructHash tests ───────────────────────────────────────

TEST_F(DepHashTest, DepStructHash_DeterministicOrdering)
{
    // Deps in different order should produce the same struct hash
    std::vector<Dep> deps1 = {
        makeEnvVarDep("B", "val1"),
        makeContentDep("/a.nix", "content1"),
    };
    std::vector<Dep> deps2 = {
        makeContentDep("/a.nix", "content2"),
        makeEnvVarDep("B", "val2"),
    };
    EXPECT_EQ(computeDepStructHash(deps1), computeDepStructHash(deps2));
}

TEST_F(DepHashTest, DepStructHash_SameKeysDifferentValues)
{
    // Same keys, different expectedHash values -> same struct hash
    std::vector<Dep> deps1 = {makeContentDep("/a.nix", "hello")};
    std::vector<Dep> deps2 = {makeContentDep("/a.nix", "world")};
    EXPECT_EQ(computeDepStructHash(deps1), computeDepStructHash(deps2));
}

TEST_F(DepHashTest, DepStructHash_DifferentKeys)
{
    // Different dep keys -> different struct hash
    std::vector<Dep> deps1 = {makeContentDep("/a.nix", "x")};
    std::vector<Dep> deps2 = {makeContentDep("/b.nix", "x")};
    EXPECT_NE(computeDepStructHash(deps1), computeDepStructHash(deps2));
}

TEST_F(DepHashTest, DepStructHash_EmptyDeps)
{
    // Empty deps -> consistent hash
    auto h1 = computeDepStructHash({});
    auto h2 = computeDepStructHash({});
    EXPECT_EQ(h1, h2);
}

TEST_F(DepHashTest, DepContentHashWithParent_DifferentParents)
{
    // Same deps + different parent dep content hashes -> different hashes
    std::vector<Dep> deps = {makeContentDep("/a.nix", "val")};
    auto parentHash1 = hashString(HashAlgorithm::SHA256, "parent-deps-1");
    auto parentHash2 = hashString(HashAlgorithm::SHA256, "parent-deps-2");
    auto h1 = computeDepContentHashWithParent(deps, parentHash1);
    auto h2 = computeDepContentHashWithParent(deps, parentHash2);
    EXPECT_NE(h1, h2);
}

TEST_F(DepHashTest, DepContentHashWithParent_SameParent)
{
    // Same deps + same parent dep content hash -> same hash
    std::vector<Dep> deps = {makeContentDep("/a.nix", "val")};
    auto parentHash = hashString(HashAlgorithm::SHA256, "parent-deps");
    auto h1 = computeDepContentHashWithParent(deps, parentHash);
    auto h2 = computeDepContentHashWithParent(deps, parentHash);
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
    // Sorted by (type, source, key)
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
    // Same (type, source, key) but different expectedHash -> deduped
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

// ── Pre-sorted variant equivalence tests ─────────────────────────────

TEST_F(DepHashTest, PreSorted_ContentHashMatchesUnsorted)
{
    std::vector<Dep> deps = {
        makeEnvVarDep("B", "val"),
        makeContentDep("/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(computeDepContentHash(deps), computeDepContentHashFromSorted(sorted));
}

TEST_F(DepHashTest, PreSorted_StructHashMatchesUnsorted)
{
    std::vector<Dep> deps = {
        makeEnvVarDep("B", "val"),
        makeContentDep("/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(computeDepStructHash(deps), computeDepStructHashFromSorted(sorted));
}

TEST_F(DepHashTest, PreSorted_ParentHashMatchesUnsorted)
{
    std::vector<Dep> deps = {
        makeEnvVarDep("B", "val"),
        makeContentDep("/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    auto parentHash = hashString(HashAlgorithm::SHA256, "parent-deps");
    EXPECT_EQ(
        computeDepContentHashWithParent(deps, parentHash),
        computeDepContentHashWithParentFromSorted(sorted, parentHash));
}

} // namespace nix::eval_cache
