#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"

#include <gtest/gtest.h>
#include <set>

#include "nix/expr/tests/libexpr.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

ResultHash computeResultHash(
    ResultKind type,
    uint32_t encodingVersion,
    std::string_view payload,
    std::string_view auxContext);

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

TEST_F(DepHashTest, Hash_DepStructHash_DeterministicOrdering)
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

TEST_F(DepHashTest, Hash_DepStructHash_SameKeysDifferentValues)
{
    // Same dep keys, different expectedHash values -> same structural hash (hashes dep structure, not content)
    std::vector<Dep> deps1 = {makeContentDep(pools, "/a.nix", "hello")};
    std::vector<Dep> deps2 = {makeContentDep(pools, "/a.nix", "world")};
    EXPECT_EQ(computeTraceStructHash(pools,deps1), computeTraceStructHash(pools,deps2));
}

TEST_F(DepHashTest, Hash_DepStructHash_DifferentKeys)
{
    // Different dep keys -> different structural hash
    std::vector<Dep> deps1 = {makeContentDep(pools, "/a.nix", "x")};
    std::vector<Dep> deps2 = {makeContentDep(pools, "/b.nix", "x")};
    EXPECT_NE(computeTraceStructHash(pools,deps1), computeTraceStructHash(pools,deps2));
}

TEST_F(DepHashTest, Hash_DepStructHash_FramesSourceAndSimpleKey)
{
    auto srcAb = pools.intern<DepSourceId>(DepSource::fromNodeKey("ab"));
    auto srcA = pools.intern<DepSourceId>(DepSource::fromNodeKey("a"));
    auto keyC = pools.intern<SimpleDepKeyId>("c");
    auto keyBc = pools.intern<SimpleDepKeyId>("bc");
    auto value = DepHashValue(depHash("same"));

    std::vector<Dep> deps1 = {{
        Dep::Key::makeSimple(CanonicalQueryKind::FileBytes, srcAb, keyC),
        value,
    }};
    std::vector<Dep> deps2 = {{
        Dep::Key::makeSimple(CanonicalQueryKind::FileBytes, srcA, keyBc),
        value,
    }};

    EXPECT_NE(computeTraceStructHash(pools, deps1), computeTraceStructHash(pools, deps2));
    EXPECT_NE(computeTraceHash(pools, deps1), computeTraceHash(pools, deps2));
}

TEST_F(DepHashTest, Hash_DepStructHash_FramesStructuredPathComponentKinds)
{
    auto objectKeyDep = makeStructuredDepForTest(
        pools,
        CanonicalQueryKind::StructuredProjection,
        DepSource::fromNodeKey("root"),
        "data.json",
        StructuredFormat::Json,
        {StructuredPathComponent::makeKey("1")},
        DepHashValue(depHash("same")));
    auto arrayIndexDep = makeStructuredDepForTest(
        pools,
        CanonicalQueryKind::StructuredProjection,
        DepSource::fromNodeKey("root"),
        "data.json",
        StructuredFormat::Json,
        {StructuredPathComponent::makeIndex(1)},
        DepHashValue(depHash("same")));

    EXPECT_NE(computeTraceStructHash(pools, {objectKeyDep}), computeTraceStructHash(pools, {arrayIndexDep}));
    EXPECT_NE(computeTraceHash(pools, {objectKeyDep}), computeTraceHash(pools, {arrayIndexDep}));
}

TEST_F(DepHashTest, Hash_DepStructHash_EmptyDeps)
{
    // Empty deps -> consistent structural hash (empty trace structure)
    auto h1 = computeTraceStructHash(pools,{});
    auto h2 = computeTraceStructHash(pools,{});
    EXPECT_EQ(h1, h2);
}

// ── sortAndDedupDeps tests ───────────────────────────────────────────

TEST_F(DepHashTest, Hash_SortAndDedup_RemovesDuplicates)
{
    std::vector<Dep> deps = {
        makeContentDep(pools, "/a.nix", "a"),
        makeContentDep(pools, "/a.nix", "a"), // exact duplicate
        makeContentDep(pools, "/b.nix", "b"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(sorted.size(), 2u);
}

TEST_F(DepHashTest, Hash_SortAndDedup_SortsCorrectly)
{
    // Sorted by canonical dep key/value order for deterministic trace hashing.
    // Note: intra-type order is by interned integer IDs, not lexicographic strings.
    std::vector<Dep> deps = {
        makeEnvVarDep(pools, "Z", "val"),     // type=4
        makeContentDep(pools, "/z.nix", "c"), // type=1
        makeContentDep(pools, "/a.nix", "c"), // type=1
    };
    auto sorted = sortAndDedupDeps(deps);
    ASSERT_EQ(sorted.size(), 3u);
    // Content (type=1) before EnvVar (type=4)
    EXPECT_EQ(sorted[0].key.kind, CanonicalQueryKind::FileBytes);
    EXPECT_EQ(sorted[1].key.kind, CanonicalQueryKind::FileBytes);
    EXPECT_EQ(sorted[2].key.kind, CanonicalQueryKind::EnvironmentLookup);
    // Both Content keys present (order depends on intern IDs)
    std::set<std::string_view> contentKeys{
        pools.resolve(sorted[0].key.simpleKeyId()), pools.resolve(sorted[1].key.simpleKeyId())};
    EXPECT_TRUE(contentKeys.count("/a.nix"));
    EXPECT_TRUE(contentKeys.count("/z.nix"));
}

TEST_F(DepHashTest, Hash_SortAndDedup_KeepsConflictingHashes)
{
    // Conflicting observations must remain distinct at this layer; tracker-level
    // instability is what suppresses unsound publication.
    std::vector<Dep> deps = {
        makeContentDep(pools, "/a.nix", "content-v1"),
        makeContentDep(pools, "/a.nix", "content-v2"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(sorted.size(), 2u);
}

TEST_F(DepHashTest, Hash_SortAndDedup_EmptyInput)
{
    auto sorted = sortAndDedupDeps({});
    EXPECT_TRUE(sorted.empty());
}

// ── Pre-sorted variant equivalence tests (trace hash from sorted deps) ──

TEST_F(DepHashTest, Hash_PreSorted_ContentHashMatchesUnsorted)
{
    std::vector<Dep> deps = {
        makeEnvVarDep(pools, "B", "val"),
        makeContentDep(pools, "/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(computeTraceHash(pools,deps), computeTraceHashFromSorted(pools,sorted));
}

TEST_F(DepHashTest, Hash_TraceHash_FramesDepValueVariant)
{
    auto key = Dep::Key::makeSimple(
        CanonicalQueryKind::FileBytes,
        pools.intern<DepSourceId>(DepSource::fromNodeKey("root")),
        pools.intern<SimpleDepKeyId>("file.nix"));
    auto hashValue = depHash("same-bytes");
    std::string stringValue(hashValue.value.view());

    std::vector<Dep> depsWithDigest = {{key, DepHashValue(hashValue)}};
    std::vector<Dep> depsWithString = {{key, DepHashValue(stringValue)}};

    EXPECT_EQ(computeTraceStructHash(pools, depsWithDigest), computeTraceStructHash(pools, depsWithString));
    EXPECT_NE(computeTraceHash(pools, depsWithDigest), computeTraceHash(pools, depsWithString));
}

TEST_F(DepHashTest, Hash_ResultHash_FramesPayloadAndAuxContext)
{
    auto h1 = computeResultHash(ResultKind::String, 1, "aC", "b");
    auto h2 = computeResultHash(ResultKind::String, 1, "a", "Cb");
    EXPECT_NE(h1, h2);
}

TEST_F(DepHashTest, Hash_PreSorted_StructHashMatchesUnsorted)
{
    std::vector<Dep> deps = {
        makeEnvVarDep(pools, "B", "val"),
        makeContentDep(pools, "/a.nix", "a"),
    };
    auto sorted = sortAndDedupDeps(deps);
    EXPECT_EQ(computeTraceStructHash(pools,deps), computeTraceStructHashFromSorted(pools,sorted));
}

// ── ImplicitStructural deps excluded from hash computation ───────────

TEST_F(DepHashTest, Hash_TraceHash_ExcludesImplicitStructural)
{
    // Deps with and without GitIdentity should produce the same trace hash
    std::vector<Dep> depsWithout = {
        makeContentDep(pools, "/a.nix", "hello"),
    };
    std::vector<Dep> depsWith = {
        makeContentDep(pools, "/a.nix", "hello"),
        makeGitIdentityDep(pools, "/tmp/repo", "rev-abc"),
    };
    EXPECT_EQ(computeTraceHash(pools, depsWithout), computeTraceHash(pools, depsWith));
}

TEST_F(DepHashTest, Hash_StructHash_ExcludesImplicitStructural)
{
    std::vector<Dep> depsWithout = {
        makeContentDep(pools, "/a.nix", "hello"),
    };
    std::vector<Dep> depsWith = {
        makeContentDep(pools, "/a.nix", "hello"),
        makeGitIdentityDep(pools, "/tmp/repo", "rev-abc"),
    };
    EXPECT_EQ(computeTraceStructHash(pools, depsWithout), computeTraceStructHash(pools, depsWith));
}

TEST_F(DepHashTest, TraceHash_DifferentGitIdentity_SameHash)
{
    // Two dep sets differing only in GitIdentity fingerprint → same trace hash
    std::vector<Dep> deps1 = {
        makeContentDep(pools, "/a.nix", "hello"),
        makeGitIdentityDep(pools, "/tmp/repo", "commit-1"),
    };
    std::vector<Dep> deps2 = {
        makeContentDep(pools, "/a.nix", "hello"),
        makeGitIdentityDep(pools, "/tmp/repo", "commit-2"),
    };
    EXPECT_EQ(computeTraceHash(pools, deps1), computeTraceHash(pools, deps2));
}

} // namespace nix::eval_trace
