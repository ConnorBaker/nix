#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ═════════════════════════════════════════════════════════════════════
// Integration tests for replay dedup-pollution fix.
// Uses TraceCacheFixture to verify full-stack behavior with TraceCache.
// ═════════════════════════════════════════════════════════════════════

class ReplayIntegrationTest : public TraceCacheFixture
{
public:
    ReplayIntegrationTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "replay-integration-test");
    }
};

// ── Test 7: Shared file dep present in parent trace after sibling replay ──

TEST_F(ReplayIntegrationTest, SharedFileDep_ParentTrace_ContainsSharedDep)
{
    TempTextFile sharedFile("original-shared-content");
    TempTextFile aOnlyFile("a-only-content-v1");

    auto expr = std::format(
        R"({{ a = builtins.readFile {} + builtins.readFile {};
              b = builtins.readFile {}; }})",
        sharedFile.path.string(), aOnlyFile.path.string(),
        sharedFile.path.string());

    // First eval — populate traces for both a and b
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        auto * b = root.attrs()->get(state.symbols.create("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
    }

    // Invalidate only the a-only file
    aOnlyFile.modify("a-only-content-v2");
    invalidateFileCache(aOnlyFile.path);

    // Second eval — b should cache-hit, a should miss and re-evaluate
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);

        // Force b (should still be valid — depends only on sharedFile)
        auto * b = root.attrs()->get(state.symbols.create("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsStringEq("original-shared-content"));

        // Force a (cache miss due to aOnlyFile change)
        auto * a = root.attrs()->get(state.symbols.create("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
    }

    // Verify stored trace for a contains the shared file dep
    // by checking that modifying the shared file invalidates a's trace
    sharedFile.modify("shared-content-changed");
    invalidateFileCache(sharedFile.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        // a's trace MUST include the shared file dep
        // so changing shared file MUST cause a cache miss and return new content
        auto str = state.forceStringNoCtx(*a->value, noPos, "");
        EXPECT_NE(std::string(str), "original-shared-contenta-only-content-v2")
            << "Shared file change must invalidate a's trace (dep must be present)";
    }
}

// ── Test 8: Both siblings invalidated by shared file change ──

TEST_F(ReplayIntegrationTest, SharedFileDep_BothSiblings_InvalidateOnSharedChange)
{
    TempTextFile sharedFile("shared-v1");

    auto expr = std::format(
        R"({{ a = builtins.readFile {}; b = builtins.readFile {}; }})",
        sharedFile.path.string(), sharedFile.path.string());

    // Populate cache
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("shared-v1"));
        auto * b = root.attrs()->get(state.symbols.create("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsStringEq("shared-v1"));
    }

    // Change shared file
    sharedFile.modify("shared-v2");
    invalidateFileCache(sharedFile.path);

    // Both must get fresh values
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * a = root.attrs()->get(state.symbols.create("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("shared-v2"))
            << "a must reflect changed shared file";
        auto * b = root.attrs()->get(state.symbols.create("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsStringEq("shared-v2"))
            << "b must reflect changed shared file";
    }
}

// ── Test 9: Three siblings — selective invalidation ──

TEST_F(ReplayIntegrationTest, ThreeSiblings_SharedDep_SelectiveInvalidation)
{
    TempTextFile sharedFile("shared");
    TempTextFile cFile("c-only");

    auto expr = std::format(
        R"({{ a = builtins.readFile {};
              b = builtins.readFile {};
              c = builtins.readFile {}; }})",
        sharedFile.path.string(),
        sharedFile.path.string(),
        cFile.path.string());

    // Populate cache
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        for (auto name : {"a", "b", "c"}) {
            auto * attr = root.attrs()->get(state.symbols.create(name));
            ASSERT_NE(attr, nullptr);
            state.forceValue(*attr->value, noPos);
        }
    }

    // Change shared file only
    sharedFile.modify("shared-v2");
    invalidateFileCache(sharedFile.path);

    // a and b must re-evaluate; c must cache-hit
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(state.symbols.create("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("shared-v2"))
            << "a must see updated shared file";

        auto * b = root.attrs()->get(state.symbols.create("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsStringEq("shared-v2"))
            << "b must see updated shared file";

        auto * c = root.attrs()->get(state.symbols.create("c"));
        ASSERT_NE(c, nullptr);
        state.forceValue(*c->value, noPos);
        EXPECT_THAT(*c->value, IsStringEq("c-only"))
            << "c must still serve cached value (independent dep)";
    }
}

// ── Test 10: Dep precision — two files both produce Content deps ──

class ReplayDepPrecisionTest : public DepPrecisionTest {};

TEST_F(ReplayDepPrecisionTest, TwoFiles_BothContentDepsRecorded)
{
    TempTextFile fileA("hello");
    TempTextFile fileB("world");

    auto expr = std::format(
        R"(builtins.readFile {} + builtins.readFile {})",
        fileA.path.string(), fileB.path.string());

    auto deps = evalAndCollectDeps(expr);

    // Must contain Content deps for BOTH files
    bool hasA = hasDep(deps, DepType::Content, fileA.path.filename().string());
    bool hasB = hasDep(deps, DepType::Content, fileB.path.filename().string());
    EXPECT_TRUE(hasA) << "Missing Content dep for fileA\n" << dumpDeps(deps);
    EXPECT_TRUE(hasB) << "Missing Content dep for fileB\n" << dumpDeps(deps);
}

TEST_F(ReplayDepPrecisionTest, SharedFile_NotDoubleCounted)
{
    TempTextFile sharedFile("shared-content");

    // Same file read twice in one expression
    auto expr = std::format(
        R"(builtins.readFile {} + builtins.readFile {})",
        sharedFile.path.string(), sharedFile.path.string());

    auto deps = evalAndCollectDeps(expr);

    // Should have exactly 1 Content dep for the shared file (dedup within tracker)
    size_t count = countDeps(deps, DepType::Content, sharedFile.path.filename().string());
    EXPECT_EQ(count, 1u)
        << "Same file read twice should produce exactly 1 Content dep\n"
        << dumpDeps(deps);
}

} // namespace nix::eval_trace
