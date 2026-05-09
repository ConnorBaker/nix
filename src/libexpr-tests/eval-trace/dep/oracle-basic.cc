#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/**
 * Tests for the full end-to-end dependency tracking pipeline (Adapton DDG):
 *
 *   Nix builtin -> DepRecordingContext::record (Adapton: record edge in DDG)
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

// ── P1 Precision: Comment-only .nix change causes re-evaluation ──────
// Documents the precision gap: whole-file Content dep means any byte change
// (even a comment edit) invalidates the trace, even when the Nix result
// is identical. The system is correct (re-evaluates), just imprecise.

TEST_F(DepTrackingTest, ContentDep_Import_CommentChange_ReEvaluates)
{
    TempTestFile file("42 # original comment");
    auto expr = "import " + file.path.string();

    // Fresh evaluation — imports file, records Content dep on whole file
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(42));
    }

    // Verify warm hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(42));
    }

    // Modify only the comment — same Nix result, different file bytes.
    file.modify("42 # different comment but longer");
    invalidateFileCache(file.path);

    // P1 precision gap: Content dep covers whole file, so comment change
    // invalidates trace even though the evaluation result is identical.
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "comment-only change causes re-evaluation (P1 precision gap)";
        EXPECT_THAT(v, IsIntEq(42));
    }
}

// ── Group 3: EnvVar oracle deps via builtins.getEnv ──────────────────

TEST_F(DepTrackingTest, EnvVarDep_GetEnv_WarmHit)
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

TEST_F(DepTrackingTest, EnvVarDep_GetEnv_Invalidation)
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

TEST_F(DepTrackingTest, ExistenceDep_PathExists_WarmHit)
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

TEST_F(DepTrackingTest, ExistenceDep_PathExists_Invalidation)
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

TEST_F(DepTrackingTest, SystemDep_CurrentSystem_AlwaysValid)
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

TEST_F(DepTrackingTest, VolatileDep_CurrentTime_AlwaysInvalidates)
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

// ── E-7: builtins.getEnv pure vs impure cross-contamination ──────────
//
// A trace recorded under one session key (simulated by one testFingerprint)
// must not be a cache hit under a different session key (different fingerprint).
// EnvironmentLookup deps carry the env value in the dep hash; combined with
// session-key isolation, a "pure" session and an "impure" session must each
// see only their own traces.
//
// Implementation note: makeDbWithPureMode [NEW HELPER] doesn't exist.
// We simulate session isolation via two separate DepTrackingTest instances
// with different testFingerprint values. Because GTest creates a fresh
// fixture instance per TEST_F and each uses a different fingerprint string,
// the underlying SqliteTraceStorage uses a different semantic session key and the
// traces are partitioned.
//
// Within a single test body we simulate this by changing testFingerprint
// between the two makeCache calls (the fixture uses testFingerprint at
// makeCache time to construct the TraceSession, so modifying it before
// the second makeCache changes the session namespace).

TEST_F(DepTrackingTest, GetEnv_PureVsImpure_NoCrossContamination)
{
    ScopedEnvVar env("MY_TEST_VAR_E7", "hello");
    auto expr = R"(builtins.getEnv "MY_TEST_VAR_E7")";

    // Cold eval with "pure-mode" fingerprint (simulates pure session)
    testFingerprint = hashString(HashAlgorithm::SHA256, "pure-mode-e7");
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cold eval: pure-mode session";
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Warm check under same fingerprint — must hit (same session namespace)
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit: same pure-mode session";
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Switch to "impure-mode" fingerprint (different session namespace)
    testFingerprint = hashString(HashAlgorithm::SHA256, "impure-mode-e7");
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        // Must be a miss: different session key → no cross-contamination
        EXPECT_EQ(calls, 1) << "miss expected: different session key, no cross-contamination";
        EXPECT_THAT(v, IsStringEq("hello"));
    }
}

} // namespace nix::eval_trace
