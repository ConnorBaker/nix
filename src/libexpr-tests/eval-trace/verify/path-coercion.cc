// E-3: Path coercion — fromRuntimeRoot, dirOf, Detach mode.
//
// These tests verify that path coercion builtins correctly record deps and
// produce cache hits / invalidations under the full TraceCacheFixture pipeline.
//
// E-3a: PathObject from runtime root via builtins.toPath + builtins.readFile.
//   The coerced path object must record the correct dep kind (FileBytes).
//
// E-3b: builtins.dirOf on a file path — must round-trip correctly in the
//   cache (warm verify hits, result matches cold eval).
//
// E-3c: Detach mode — using a plain string path via builtins.readFile must
//   produce a warm hit without bypassing the cache.
//   (evalCacheHitsInDetachMode [NEW HELPER] is not yet on the fixture; this
//    test uses the canonical loaderCalls pattern instead.)
//
// NOTE: "Detach mode" in the sketches refers to the case where a path value
// is passed as a plain string (not a path literal). builtins.readFile on a
// string path coerces it via coerceToPath, which still records a FileBytes
// dep in the current implementation.

#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class PathCoercionIntegrationTest : public TraceCacheFixture
{
public:
    PathCoercionIntegrationTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "path-coercion-integration-test");
    }
};

// ── E-3a: PathObject from runtime root ───────────────────────────────
//
// builtins.toPath coerces a string to a path; builtins.readFile on it
// records a FileBytes dep. The warm verify must hit.

TEST_F(PathCoercionIntegrationTest, PathCoercion_FromRuntimeRoot_WarmHit)
{
    TempTextFile f("runtime-value");
    auto expr = "builtins.readFile (builtins.toPath \"" + f.path.string() + "\")";

    // Cold eval (BSàlC: trace recording)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("runtime-value"));
    }

    // Warm eval — dep unchanged → cache hit
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: path dep unchanged";
        EXPECT_THAT(v, IsStringEq("runtime-value"));
    }
}

TEST_F(PathCoercionIntegrationTest, PathCoercion_FromRuntimeRoot_ContentChange_Invalidation)
{
    TempTextFile f("runtime-v1");
    auto expr = "builtins.readFile (builtins.toPath \"" + f.path.string() + "\")";

    // Cold eval
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("runtime-v1"));
    }

    // Confirm warm hit (precision pre-condition)
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0);
    }

    // Mutate the file
    f.modify("runtime-v2-longer");
    invalidateFileCache(f.path);

    // Must re-evaluate (soundness)
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "soundness: path content change must invalidate";
        EXPECT_THAT(v, IsStringEq("runtime-v2-longer"));
    }
}

// ── E-3b: builtins.dirOf + mergeSemanticHandle warm hit ─────────────
//
// builtins.dirOf on a string path returns the parent directory.
// The cached result must match the cold eval result on warm verify.
//
// dirOf has no mutable dep — soundness is vacuously satisfied.

TEST_F(PathCoercionIntegrationTest, DirOf_MergeSemanticHandle_WarmHit)
{
    TempTextFile f("v");
    // dirOf on a file path returns the containing directory
    auto expr = "builtins.dirOf \"" + f.path.string() + "\"";

    // Cold eval — records the expression; dirOf is pure (no file-read dep)
    Value coldResult;
    {
        auto cache = makeCache(expr);
        coldResult = forceRoot(*cache);
    }

    // Warm eval — must hit and return same result
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto warmResult = forceRoot(*cache);
        // dirOf has no file-read dep, so it always warm-hits after cold eval.
        EXPECT_EQ(calls, 0) << "warm hit expected for dirOf (pure expression)";
        ASSERT_EQ(coldResult.type(), warmResult.type());
        EXPECT_EQ(coldResult.string_view(), warmResult.string_view())
            << "warm hit must return identical string value";
    }
}

// ── E-3c: Detach mode / string path — warm hit without cache bypass ──
//
// builtins.readFile on a plain string (not a path literal) still coerces to
// a path and records a FileBytes dep. The warm verify must hit.

TEST_F(PathCoercionIntegrationTest, DetachMode_WarmHit_NoCacheBypass)
{
    TempTextFile f("detach-content");
    // Use plain string (not path literal) — this is the "detach mode" case
    auto expr = "builtins.readFile \"" + f.path.string() + "\"";

    // Cold eval
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("detach-content"));
    }

    // Warm eval — must hit (dep recorded via string-path coercion)
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: string-path dep unchanged";
        EXPECT_THAT(v, IsStringEq("detach-content"));
    }
}

TEST_F(PathCoercionIntegrationTest, DetachMode_FileChange_Invalidation)
{
    TempTextFile f("detach-v1");
    auto expr = "builtins.readFile \"" + f.path.string() + "\"";

    // Cold eval
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("detach-v1"));
    }

    // Confirm warm hit
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0);
    }

    // Mutate
    f.modify("detach-v2-longer");
    invalidateFileCache(f.path);

    // Must re-evaluate
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1);
        EXPECT_THAT(v, IsStringEq("detach-v2-longer"));
    }
}

} // namespace nix::eval_trace
