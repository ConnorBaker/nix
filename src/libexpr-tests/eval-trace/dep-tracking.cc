#include "helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/**
 * Tests for the full end-to-end dependency tracking pipeline (Adapton DDG):
 *
 *   Nix builtin -> DependencyTracker::record (Adapton: record edge in DDG)
 *     -> fresh evaluation collects deps -> record creates trace (BSàlC: trace recording)
 *       -> index.upsert(contextHash, attrPath, traceId)
 *
 *   Verification path (BSàlC: verifying trace): index.lookup -> verifyTrace
 *     -> compare stored dep hash with current oracle hash
 *       -> serve from trace or invalidate and re-evaluate
 *
 * Each test verifies that:
 * - Fresh evaluation records the correct dependencies (BSàlC: trace recording)
 * - Verification validates deps and serves from trace (loaderCalls == 0)
 * - After dep invalidation, verification falls back to fresh evaluation (loaderCalls == 1)
 */
class DepTrackingTest : public TraceCacheFixture
{
public:
    DepTrackingTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "dep-tracking-test");
    }
};

// ── Group 1: Content oracle deps via builtins.readFile ───────────────

TEST_F(DepTrackingTest, ContentDep_ReadFile_WarmHit)
{
    TempTestFile file("hello");
    auto expr = "builtins.readFile " + file.path.string();

    // Fresh evaluation — reads file, records Content dep in trace (BSàlC: trace recording)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Verification — file unchanged, dep valid -> serve from trace (BSàlC: verify trace)
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello"));
    }
}

TEST_F(DepTrackingTest, ContentDep_ReadFile_Invalidation)
{
    TempTestFile file("hello");
    auto expr = "builtins.readFile " + file.path.string();

    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Verify trace verification hit first
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify file content -> Content oracle hash changes (Shake: input changed).
    // Use clearly different size to ensure stat metadata changes
    // (avoids flakiness from same-size modifications within timestamp granularity).
    file.modify("world!!");
    invalidateFileCache(file.path);

    // Verification should fail (Shake: dirty input) -> fresh re-evaluation
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("world!!"));
    }
}

// ── Group 2: Content oracle deps via import ──────────────────────────

TEST_F(DepTrackingTest, ContentDep_Import_WarmHit)
{
    TempTestFile file("42");
    auto expr = "import " + file.path.string();

    // Fresh evaluation — imports file, records Content dep in trace
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(42));
    }

    // Verification — file unchanged -> serve from trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(42));
    }
}

// ── Group 3: EnvVar oracle deps via builtins.getEnv ──────────────────

TEST_F(DepTrackingTest, EnvVarDep_WarmHit)
{
    ScopedEnvVar env("NIX_TEST_DEP_TRACK", "hello");
    auto expr = R"(builtins.getEnv "NIX_TEST_DEP_TRACK")";

    // Fresh evaluation — reads env var, records EnvVar dep in trace
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Verification — env var unchanged -> serve from trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello"));
    }
}

TEST_F(DepTrackingTest, EnvVarDep_Invalidation)
{
    ScopedEnvVar env("NIX_TEST_DEP_TRACK_INV", "hello");
    auto expr = R"(builtins.getEnv "NIX_TEST_DEP_TRACK_INV")";

    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Verify trace verification hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Change env var -> EnvVar oracle hash changes (Shake: dirty input)
    setenv("NIX_TEST_DEP_TRACK_INV", "world", 1);

    // Verification should fail (Shake: dirty input) -> fresh re-evaluation
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("world"));
    }
}

// ── Group 4: Existence oracle deps via builtins.pathExists ───────────

TEST_F(DepTrackingTest, ExistenceDep_WarmHit)
{
    TempTestFile file("dummy");
    auto expr = "builtins.pathExists " + file.path.string();

    // Fresh evaluation — file exists, records Existence dep with file type in trace
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    // Verification — file still exists -> serve from trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(DepTrackingTest, ExistenceDep_Invalidation)
{
    TempTestFile file("dummy");
    auto expr = "builtins.pathExists " + file.path.string();

    // Fresh evaluation — file exists, record Existence dep
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    // Verify trace verification hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Delete file -> Existence oracle changes (type:N -> missing, Shake: dirty input)
    std::filesystem::remove(file.path);
    invalidateFileCache(file.path);

    // Verification should fail (Shake: dirty input) -> fresh re-evaluation
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsFalse());
    }

    // Recreate the file so TempTestFile destructor doesn't fail
    std::ofstream(file.path) << "recreated";
}

// ── Group 5: System oracle dep via builtins.currentSystem ────────────

TEST_F(DepTrackingTest, SystemDep_AlwaysValid)
{
    auto expr = "builtins.currentSystem";

    // Fresh evaluation — records System dep with current system string in trace
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsString());
    }

    // Verification — system oracle never changes mid-process -> always valid
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsString());
    }
}

// ── Group 6: Volatile Dep (currentTime — Shake: always-dirty rule) ───

TEST_F(DepTrackingTest, VolatileDep_AlwaysInvalidates)
{
    auto expr = "builtins.currentTime";

    // Fresh evaluation — records CurrentTime dep (always volatile — Shake: always-dirty)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
    }

    // Verification — CurrentTime dep always fails -> fresh re-evaluation
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.type(), nInt);
    }
}

// ── Group 7: Nested Attrs with Independent Deps (Adapton: per-node traces) ──

TEST_F(DepTrackingTest, NestedAttrs_ChildDepInvalidation)
{
    TempTestFile file("alpha");
    auto expr = "{ a = builtins.readFile " + file.path.string() + "; b = 42; }";

    // Fresh evaluation — force root and all children, recording traces at each level
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        EXPECT_THAT(root, IsAttrsOfSize(2));

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("alpha"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsIntEq(42));
    }

    // Verification — all traces should be verified and served from cache.
    // Children with zero own deps have a ParentContext dep linking them
    // to the parent's result hash, enabling cache hits on warm path.
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("alpha"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsIntEq(42));

        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify file — only child 'a' has a dep on it in its trace.
    // Different size ensures stat oracle metadata changes.
    file.modify("beta-modified");
    invalidateFileCache(file.path);

    // Verification after modification:
    // - Root has no direct file deps -> trace verified (BSàlC: clean)
    // - Child 'a' has Content dep on modified file -> trace invalid -> fresh re-evaluation
    // - Child 'b' has no file deps -> trace verified (BSàlC: clean)
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("beta-modified"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsIntEq(42));

        // rootLoader called once (for child 'a' fresh re-evaluation via navigateToReal)
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 8: Dep Isolation (Adapton: independent DDG subgraphs) ──────

TEST_F(DepTrackingTest, DepIsolation_SiblingUnaffected)
{
    TempTestFile fileA("aaa");
    TempTestFile fileB("bbb");
    auto expr = "{ a = builtins.readFile " + fileA.path.string()
              + "; b = builtins.readFile " + fileB.path.string() + "; }";

    // Fresh evaluation — force root and both children, recording independent traces
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        EXPECT_THAT(root, IsAttrsOfSize(2));

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("aaa"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsStringEq("bbb"));
    }

    // Verification — all traces verified
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("aaa"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsStringEq("bbb"));

        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify only fileA -> child 'a' trace invalidates, child 'b' unaffected (Adapton: minimal re-evaluation).
    // Different size ensures stat oracle metadata changes.
    fileA.modify("AAA-modified");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * a = root.attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsStringEq("AAA-modified"));

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsStringEq("bbb"));

        // rootLoader called once (for child 'a' fresh re-evaluation via navigateToReal)
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 9: Deep Nesting with Leaf-Only Dep (Adapton: deep DDG chain) ──

TEST_F(DepTrackingTest, DeepNesting_LeafOnlyDep)
{
    TempTestFile file("leaf-val");
    auto expr = "{ l1 = { l2 = { leaf = builtins.readFile "
              + file.path.string() + "; }; }; }";

    // Fresh evaluation — force all 4 levels (root -> l1 -> l2 -> leaf), recording traces at each
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        EXPECT_THAT(root, IsAttrsOfSize(1));

        auto * l1 = root.attrs()->get(createSymbol("l1"));
        ASSERT_NE(l1, nullptr);
        state.forceValue(*l1->value, noPos);

        auto * l2 = l1->value->attrs()->get(createSymbol("l2"));
        ASSERT_NE(l2, nullptr);
        state.forceValue(*l2->value, noPos);

        auto * leaf = l2->value->attrs()->get(createSymbol("leaf"));
        ASSERT_NE(leaf, nullptr);
        state.forceValue(*leaf->value, noPos);
        EXPECT_THAT(*leaf->value, IsStringEq("leaf-val"));
    }

    // Verification — all level traces verified.
    // Intermediates have ParentContext deps; leaf has Content dep.
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * l1 = root.attrs()->get(createSymbol("l1"));
        ASSERT_NE(l1, nullptr);
        state.forceValue(*l1->value, noPos);

        auto * l2 = l1->value->attrs()->get(createSymbol("l2"));
        ASSERT_NE(l2, nullptr);
        state.forceValue(*l2->value, noPos);

        auto * leaf = l2->value->attrs()->get(createSymbol("leaf"));
        ASSERT_NE(leaf, nullptr);
        state.forceValue(*leaf->value, noPos);
        EXPECT_THAT(*leaf->value, IsStringEq("leaf-val"));

        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify file -> only the leaf trace's dep changes (Adapton: minimal dirty propagation).
    // Different size ensures stat oracle metadata changes.
    file.modify("new-leaf-value");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * l1 = root.attrs()->get(createSymbol("l1"));
        ASSERT_NE(l1, nullptr);
        state.forceValue(*l1->value, noPos);

        auto * l2 = l1->value->attrs()->get(createSymbol("l2"));
        ASSERT_NE(l2, nullptr);
        state.forceValue(*l2->value, noPos);

        auto * leaf = l2->value->attrs()->get(createSymbol("leaf"));
        ASSERT_NE(leaf, nullptr);
        state.forceValue(*leaf->value, noPos);
        EXPECT_THAT(*leaf->value, IsStringEq("new-leaf-value"));

        // rootLoader called once (for leaf fresh re-evaluation via navigateToReal chain)
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 10: Mixed Oracle Dep Types in Single Expression ────────────

TEST_F(DepTrackingTest, MixedDeps_WarmHit)
{
    TempTestFile file("hello");
    ScopedEnvVar env("NIX_TEST_MIXED", "world");
    auto expr = "(builtins.readFile " + file.path.string()
              + R"() + (builtins.getEnv "NIX_TEST_MIXED"))";

    // Fresh evaluation — records both Content and EnvVar oracle deps in trace
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("helloworld"));
    }

    // Verification — both oracle deps valid -> serve from trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("helloworld"));
    }
}

TEST_F(DepTrackingTest, MixedDeps_InvalidateFile)
{
    TempTestFile file("hello");
    ScopedEnvVar env("NIX_TEST_MIXED_F", "world");
    auto expr = "(builtins.readFile " + file.path.string()
              + R"() + (builtins.getEnv "NIX_TEST_MIXED_F"))";

    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("helloworld"));
    }

    // Verify trace verification hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify file -> Content oracle dep invalidates (EnvVar oracle dep still valid).
    // Different size ensures stat oracle metadata changes.
    file.modify("HELLO-MODIFIED");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("HELLO-MODIFIEDworld"));
    }
}

TEST_F(DepTrackingTest, MixedDeps_InvalidateEnvVar)
{
    TempTestFile file("hello");
    ScopedEnvVar env("NIX_TEST_MIXED_E", "world");
    auto expr = "(builtins.readFile " + file.path.string()
              + R"() + (builtins.getEnv "NIX_TEST_MIXED_E"))";

    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("helloworld"));
    }

    // Verify trace verification hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify env var -> EnvVar oracle dep invalidates (Content oracle dep still valid)
    setenv("NIX_TEST_MIXED_E", "WORLD", 1);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("helloWORLD"));
    }
}

// ── Group 11: Parent Trace Cascade via Import (Shake: transitive dirty) ──

TEST_F(DepTrackingTest, ParentDepCascade_ViaImport)
{
    TempTestFile dataFile("hello");
    std::string nixContent = "{ x = builtins.readFile " + dataFile.path.string() + "; }";
    TempTestFile nixFile(nixContent);
    auto expr = "import " + nixFile.path.string();

    // Fresh evaluation — root trace has Content dep on nixFile (via import),
    // child 'x' trace has Content dep on dataFile (via readFile)
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        EXPECT_THAT(root, IsAttrsOfSize(1));

        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("hello"));
    }

    // Verification — both root and child traces verified
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("hello"));

        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify nixFile (add Nix comment — changes file oracle hash but same semantics).
    // Use clearly different content size to ensure stat oracle metadata changes.
    nixFile.modify(nixContent + " # this comment changes the file hash");
    invalidateFileCache(nixFile.path);
    // Clear import resolution cache so evalFile re-reads the file
    state.resetFileCache();

    // Verification after nixFile change (Shake: transitive dirty propagation):
    // - Root trace dep (Content on nixFile) invalidates -> root fresh re-evaluation
    // - Child trace chain (parent link) also invalidates -> child fresh re-evaluation
    // - But dataFile oracle unchanged -> child result still "hello"
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("hello"));

        // rootLoader called once (root fresh re-evaluation due to nixFile Content oracle dep change)
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 12: Directory oracle deps via builtins.readDir ─────────────

TEST_F(DepTrackingTest, DirectoryDep_WarmHit)
{
    // Create a temp directory with two files
    auto dir = std::filesystem::temp_directory_path()
             / ("nix-test-readdir-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "a.txt") << "a";
    std::ofstream(dir / "b.txt") << "b";

    auto expr = "builtins.attrNames (builtins.readDir " + dir.string() + ")";

    // Fresh evaluation — records Directory oracle dep on dir in trace
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nList);
    }

    // Verification — dir oracle unchanged -> serve from trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.type(), nList);
    }

    std::filesystem::remove_all(dir);
}

TEST_F(DepTrackingTest, DirectoryDep_Invalidation)
{
    // Create a temp directory with one file
    auto dir = std::filesystem::temp_directory_path()
             / ("nix-test-readdir-inv-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "a.txt") << "a";

    auto expr = "builtins.length (builtins.attrNames (builtins.readDir " + dir.string() + "))";

    // Fresh evaluation — records Directory oracle dep in trace
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Verify trace verification hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Add another file -> Directory oracle hash changes (Shake: dirty input)
    std::ofstream(dir / "b.txt") << "b";
    invalidateFileCache(dir);

    // Verification should fail (Shake: dirty input) -> fresh re-evaluation
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(2));
    }

    std::filesystem::remove_all(dir);
}

} // namespace nix::eval_trace
