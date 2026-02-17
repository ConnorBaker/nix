#include "helpers.hh"
#include "nix/expr/eval-cache-store.hh"
#include "nix/expr/stat-hash-cache.hh"

#include <gtest/gtest.h>
#include <filesystem>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/canon-path.hh"

namespace nix::eval_cache {

using namespace nix::eval_cache::test;

/**
 * Tests for the full end-to-end dependency tracking pipeline:
 *
 *   Nix builtin → recordDep / FileLoadTracker::record
 *     → evaluateCold collects deps → coldStore creates Text CA trace blob
 *       → index.upsert(contextHash, attrPath, tracePath)
 *
 *   Warm path: index.lookup → validateEvalDrv → compare stored hash
 *     with current hash → serve from cache or invalidate
 *
 * Each test verifies that:
 * - Cold eval records the correct dependencies
 * - Warm eval validates deps and serves from cache (loaderCalls == 0)
 * - After dep invalidation, warm eval falls back to cold (loaderCalls == 1)
 */
class DepTrackingTest : public LibExprTest
{
public:
    DepTrackingTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {}

protected:
    ScopedCacheDir cacheDir;
    Hash testFingerprint = hashString(HashAlgorithm::SHA256, "dep-tracking-test");

    /**
     * Create an EvalCache with a rootLoader that evaluates a Nix expression.
     * Optionally tracks how many times the rootLoader is called:
     *   loaderCalls == 0  → warm path served from cache
     *   loaderCalls == 1  → cold path (rootLoader called)
     */
    std::unique_ptr<EvalCache> makeCache(
        const std::string & nixExpr,
        int * loaderCalls = nullptr)
    {
        auto loader = [this, nixExpr, loaderCalls]() -> Value * {
            if (loaderCalls) (*loaderCalls)++;
            Value v = eval(nixExpr);
            auto * result = state.allocValue();
            *result = v;
            return result;
        };
        return std::make_unique<EvalCache>(
            testFingerprint, state, std::move(loader));
    }

    Value forceRoot(EvalCache & cache)
    {
        auto * v = cache.getRootValue();
        state.forceValue(*v, noPos);
        return *v;
    }

    /**
     * Invalidate all caches that could mask a file modification:
     * - PosixSourceAccessor's singleton lstat cache
     * - StatHashCache's L1 in-memory cache (process singleton)
     */
    void invalidateFileCache(const std::filesystem::path & path)
    {
        getFSSourceAccessor()->invalidateCache(CanonPath(path.string()));
        StatHashCache::instance().clearMemoryCache();
    }
};

// ── Group 1: Content Deps via builtins.readFile ──────────────────────

TEST_F(DepTrackingTest, ContentDep_ReadFile_WarmHit)
{
    TempTestFile file("hello");
    auto expr = "builtins.readFile " + file.path.string();

    // Cold eval — reads file, records Content dep
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Warm eval — file unchanged, dep valid → serve from cache
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

    // Cold eval
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Verify warm hit first
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify file content → Content dep hash changes.
    // Use clearly different size to ensure stat metadata changes
    // (avoids flakiness from same-size modifications within timestamp granularity).
    file.modify("world!!");
    invalidateFileCache(file.path);

    // Warm eval should fail validation → cold re-eval
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("world!!"));
    }
}

// ── Group 2: Content Deps via import ─────────────────────────────────

TEST_F(DepTrackingTest, ContentDep_Import_WarmHit)
{
    TempTestFile file("42");
    auto expr = "import " + file.path.string();

    // Cold eval — imports file, records Content dep
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(42));
    }

    // Warm eval — file unchanged → serve from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(42));
    }
}

// ── Group 3: EnvVar Deps via builtins.getEnv ─────────────────────────

TEST_F(DepTrackingTest, EnvVarDep_WarmHit)
{
    ScopedEnvVar env("NIX_TEST_DEP_TRACK", "hello");
    auto expr = R"(builtins.getEnv "NIX_TEST_DEP_TRACK")";

    // Cold eval — reads env var, records EnvVar dep
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Warm eval — env var unchanged → serve from cache
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

    // Cold eval
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Verify warm hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Change env var → EnvVar dep hash changes
    setenv("NIX_TEST_DEP_TRACK_INV", "world", 1);

    // Warm eval should fail validation → cold re-eval
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("world"));
    }
}

// ── Group 4: Existence Deps via builtins.pathExists ──────────────────

TEST_F(DepTrackingTest, ExistenceDep_WarmHit)
{
    TempTestFile file("dummy");
    auto expr = "builtins.pathExists " + file.path.string();

    // Cold eval — file exists, records Existence dep with file type
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    // Warm eval — file still exists → serve from cache
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

    // Cold eval — file exists
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    // Verify warm hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Delete file → Existence dep changes (type:N → missing)
    std::filesystem::remove(file.path);
    invalidateFileCache(file.path);

    // Warm eval should fail validation → cold re-eval
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

// ── Group 5: System Dep via builtins.currentSystem ───────────────────

TEST_F(DepTrackingTest, SystemDep_AlwaysValid)
{
    auto expr = "builtins.currentSystem";

    // Cold eval — records System dep with current system string
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsString());
    }

    // Warm eval — system never changes mid-process → always valid
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsString());
    }
}

// ── Group 6: Volatile Dep (currentTime) ──────────────────────────────

TEST_F(DepTrackingTest, VolatileDep_AlwaysInvalidates)
{
    auto expr = "builtins.currentTime";

    // Cold eval — records CurrentTime dep (always volatile)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
    }

    // Warm eval — CurrentTime dep always fails validation → cold re-eval
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.type(), nInt);
    }
}

// ── Group 7: Nested Attrs with Independent Deps ──────────────────────

TEST_F(DepTrackingTest, NestedAttrs_ChildDepInvalidation)
{
    TempTestFile file("alpha");
    auto expr = "{ a = builtins.readFile " + file.path.string() + "; b = 42; }";

    // Cold eval — force root and all children
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

    // Warm eval — everything should be cached
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

    // Modify file — only child 'a' depends on it.
    // Different size ensures stat metadata changes.
    file.modify("beta-modified");
    invalidateFileCache(file.path);

    // Warm eval after modification:
    // - Root has no direct file deps → warm hit
    // - Child 'a' has Content dep on modified file → invalidated → cold re-eval
    // - Child 'b' has no file deps → warm hit
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

        // rootLoader called once (for child 'a' cold re-eval via navigateToReal)
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 8: Dep Isolation (Sibling Independence) ────────────────────

TEST_F(DepTrackingTest, DepIsolation_SiblingUnaffected)
{
    TempTestFile fileA("aaa");
    TempTestFile fileB("bbb");
    auto expr = "{ a = builtins.readFile " + fileA.path.string()
              + "; b = builtins.readFile " + fileB.path.string() + "; }";

    // Cold eval — force root and both children
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

    // Warm eval — everything cached
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

    // Modify only fileA → child 'a' dep invalidates, child 'b' unaffected.
    // Different size ensures stat metadata changes.
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

        // rootLoader called once (for child 'a' cold re-eval via navigateToReal)
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 9: Deep Nesting with Leaf-Only Dep ─────────────────────────

TEST_F(DepTrackingTest, DeepNesting_LeafOnlyDep)
{
    TempTestFile file("leaf-val");
    auto expr = "{ l1 = { l2 = { leaf = builtins.readFile "
              + file.path.string() + "; }; }; }";

    // Cold eval — force all 4 levels (root → l1 → l2 → leaf)
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

    // Warm eval — all levels cached
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

    // Modify file → only the leaf dep changes.
    // Different size ensures stat metadata changes.
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

        // rootLoader called once (for leaf cold re-eval via navigateToReal chain)
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 10: Mixed Dep Types in Single Expression ───────────────────

TEST_F(DepTrackingTest, MixedDeps_WarmHit)
{
    TempTestFile file("hello");
    ScopedEnvVar env("NIX_TEST_MIXED", "world");
    auto expr = "(builtins.readFile " + file.path.string()
              + R"() + (builtins.getEnv "NIX_TEST_MIXED"))";

    // Cold eval — records both Content dep and EnvVar dep
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("helloworld"));
    }

    // Warm eval — both deps valid → serve from cache
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

    // Cold eval
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("helloworld"));
    }

    // Verify warm hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify file → Content dep invalidates (EnvVar dep still valid).
    // Different size ensures stat metadata changes.
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

    // Cold eval
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("helloworld"));
    }

    // Verify warm hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify env var → EnvVar dep invalidates (Content dep still valid)
    setenv("NIX_TEST_MIXED_E", "WORLD", 1);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("helloWORLD"));
    }
}

// ── Group 11: Parent Dep Cascade via Import ──────────────────────────

TEST_F(DepTrackingTest, ParentDepCascade_ViaImport)
{
    TempTestFile dataFile("hello");
    std::string nixContent = "{ x = builtins.readFile " + dataFile.path.string() + "; }";
    TempTestFile nixFile(nixContent);
    auto expr = "import " + nixFile.path.string();

    // Cold eval — root has Content dep on nixFile (via import),
    // child 'x' has Content dep on dataFile (via readFile)
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        EXPECT_THAT(root, IsAttrsOfSize(1));

        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("hello"));
    }

    // Warm eval — both root and child cached
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

    // Modify nixFile (add Nix comment — changes file hash but same semantics).
    // Use clearly different content size to ensure stat metadata changes.
    nixFile.modify(nixContent + " # this comment changes the file hash");
    invalidateFileCache(nixFile.path);
    // Clear import resolution cache so evalFile re-reads the file
    state.resetFileCache();

    // Warm eval after nixFile change:
    // - Root dep (Content on nixFile) invalidates → root cold re-eval
    // - Child dep chain (inputDrvs → parent) also invalidates → child cold re-eval
    // - But dataFile unchanged → child result still "hello"
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);

        auto * x = root.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsStringEq("hello"));

        // rootLoader called once (root cold re-eval due to nixFile Content dep)
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Group 12: Directory Deps via builtins.readDir ────────────────────

TEST_F(DepTrackingTest, DirectoryDep_WarmHit)
{
    // Create a temp directory with two files
    auto dir = std::filesystem::temp_directory_path()
             / ("nix-test-readdir-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "a.txt") << "a";
    std::ofstream(dir / "b.txt") << "b";

    auto expr = "builtins.attrNames (builtins.readDir " + dir.string() + ")";

    // Cold eval — records Directory dep on dir
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nList);
    }

    // Warm eval — dir unchanged → serve from cache
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

    // Cold eval — records Directory dep
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Verify warm hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Add another file → Directory dep hash changes
    std::ofstream(dir / "b.txt") << "b";
    invalidateFileCache(dir);

    // Warm eval should fail validation → cold re-eval
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(2));
    }

    std::filesystem::remove_all(dir);
}

} // namespace nix::eval_cache
