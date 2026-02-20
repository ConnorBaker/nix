/**
 * Tests for CopiedPath dep recording and path coercion dep recording.
 *
 * CopiedPath deps are recorded by copyPathToStore() when a path value is
 * coerced to a string (e.g., string interpolation `"${./dir}"`). The dep
 * hash is the store path string, which is deterministic for a given source.
 *
 * BUG FIX TESTED HERE: Before the fix, copyPathToStore() only recorded a
 * CopiedPath dep on the FIRST call per source path per process (guarded by
 * `!dstPathCached`). Subsequent traces in the same process that copied the
 * same source got no dep, causing stale results after source modification.
 *
 * PATH COERCION FIX: ExprConcatStrings::eval produces path values (e.g.,
 * `./dir + "/file.nix"`) without recording any dep. The fix records a
 * Content dep on the final concatenated path for files.
 *
 * NOTE ON CONSERVATIVE OVER-APPROXIMATION: The path coercion Content dep
 * may duplicate a dep recorded at the consumption point (import, readFile,
 * copyPathToStore). This is harmless — DependencyTracker::record() deduplicates
 * by (type, source, key) within a single tracker scope.
 * The trade-off is one extra readFile + BLAKE3 hash per path concatenation
 * result, in exchange for soundness (no stale results when file changes).
 * For directories, no dep is recorded here; it will be recorded at the
 * consumption point (readDir, copyPathToStore, etc.).
 */

#include "helpers.hh"

#include "nix/expr/trace-cache.hh"
#include "nix/expr/eval.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepCopiedPathTest : public TraceCacheFixture
{
public:
    DepCopiedPathTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "dep-copied-path-test");
    }
};

// ═══════════════════════════════════════════════════════════════════════
// Path coercion tests (ExprConcatStrings path result dep recording)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepCopiedPathTest, PathConcat_FileContent_WarmHit)
{
    // Create a directory with a file
    TempDir dir;
    dir.addFile("file.txt", "original content here");

    // Expression: ./dir + "/file.txt" — produces a path value
    auto expr = dir.path().string() + " + \"/file.txt\"";

    // Fresh evaluation — records Content dep on the concatenated path
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nPath);
    }

    // Verification — file unchanged, dep valid -> serve from trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.type(), nPath);
    }
}

TEST_F(DepCopiedPathTest, PathConcat_FileContent_Invalidation)
{
    TempDir dir;
    dir.addFile("file.txt", "original content here");

    auto expr = dir.path().string() + " + \"/file.txt\"";

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nPath);
    }

    // Verify warm hit first
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify file content — different size for reliable stat invalidation
    dir.addFile("file.txt", "MODIFIED content with different size!!!");
    invalidateFileCache(dir.path() / "file.txt");

    // Verification should fail (Content dep invalid) -> fresh re-evaluation
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.type(), nPath);
    }
}

TEST_F(DepCopiedPathTest, PathConcat_Directory_NoDep)
{
    // When the concatenated path resolves to a directory (not a file),
    // no Content dep is recorded here — it will be recorded at the
    // consumption point (readDir, copyPathToStore, etc.).
    TempDir dir;
    dir.addSubdir("subdir");

    auto expr = dir.path().string() + " + \"/subdir\"";

    // Fresh evaluation — produces a path value; no Content dep
    // (directory, caught by try-catch in ExprConcatStrings::eval)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nPath);
    }

    // Verification — still serves from trace (no deps to invalidate)
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(v.type(), nPath);
    }
}

TEST_F(DepCopiedPathTest, PathConcat_ReadFile_DupDep)
{
    // The path coercion dep may duplicate the dep recorded by readFile.
    // This test verifies both work together without issues.
    TempDir dir;
    dir.addFile("data.txt", "hello from data");

    // Path concat produces a path, then readFile reads it — both attempt to record
    // Content dep. DependencyTracker::record() deduplicates by (type, source, key).
    auto pathExpr = dir.path().string() + " + \"/data.txt\"";
    auto expr = "builtins.readFile (" + pathExpr + ")";

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello from data"));
    }

    // Warm hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello from data"));
    }

    // Modify file — both deps should detect the change
    dir.addFile("data.txt", "modified data content!!!");
    invalidateFileCache(dir.path() / "data.txt");

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("modified data content!!!"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// CopiedPath dep recording tests (copyPathToStore)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepCopiedPathTest, CopiedPath_SingleTrace_WarmHit)
{
    // String interpolation of a path triggers copyPathToStore
    TempDir dir;
    dir.addFile("content.txt", "some file content");

    // "${./dir}" coerces the path to string, copying to store
    auto expr = "builtins.substring 0 0 \"${" + dir.path().string() + "}\"";

    // Fresh evaluation — records CopiedPath dep
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Warm hit — CopiedPath dep still valid (dir unchanged)
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

TEST_F(DepCopiedPathTest, CopiedPath_SingleTrace_Invalidation)
{
    TempDir dir;
    dir.addFile("content.txt", "original file content");

    auto expr = "builtins.substring 0 0 \"${" + dir.path().string() + "}\"";

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Verify warm hit first
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify file in dir — changes store path -> CopiedPath dep invalid
    dir.addFile("content.txt", "MODIFIED FILE CONTENT WITH DIFFERENT SIZE");
    INVALIDATE_DIR(dir);

    // Verification should fail -> fresh re-evaluation
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(DepCopiedPathTest, CopiedPath_MultipleTraces_AllRecordDep)
{
    // REGRESSION TEST for the srcToStore caching bug:
    // Before the fix, only the first trace to copy a given source path
    // would get a CopiedPath dep. Subsequent traces would miss it because
    // the `!dstPathCached` guard prevented recording when the store path
    // was already cached in the process-wide `srcToStore` map.
    //
    // After the fix, the CopiedPath dep is recorded for every active
    // DependencyTracker regardless of srcToStore caching.

    TempDir dir;
    dir.addFile("shared.txt", "shared content here");

    // Two separate TraceCaches that both interpolate the same directory
    auto expr = "builtins.substring 0 0 \"${" + dir.path().string() + "}\"";

    // Fresh evaluation of trace 1
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Use a different fingerprint for trace 2 so it gets a separate trace
    auto origFingerprint = testFingerprint;
    testFingerprint = hashString(HashAlgorithm::SHA256, "dep-copied-path-test-2");

    // Fresh evaluation of trace 2 — same source path, already in srcToStore cache
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Both traces should serve warm hits
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "trace 2 should have warm hit";
    }

    testFingerprint = origFingerprint;
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "trace 1 should have warm hit";
    }

    // Modify file in dir — both traces should re-evaluate
    dir.addFile("shared.txt", "MODIFIED SHARED CONTENT WITH DIFFERENT SIZE");
    INVALIDATE_DIR(dir);

    // Trace 1 should fail verification and re-evaluate
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "trace 1 must re-evaluate after modification";
    }

    // Trace 2 should also fail verification and re-evaluate
    testFingerprint = hashString(HashAlgorithm::SHA256, "dep-copied-path-test-2");
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "trace 2 must re-evaluate after modification (regression: was 0 before fix)";
    }
}

TEST_F(DepCopiedPathTest, CopiedPath_ThreeTraces_AllInvalidate)
{
    TempDir dir;
    dir.addFile("triple.txt", "triple content data");

    auto expr = "builtins.substring 0 0 \"${" + dir.path().string() + "}\"";

    // Evaluate three traces with different fingerprints
    auto fp1 = hashString(HashAlgorithm::SHA256, "triple-test-1");
    auto fp2 = hashString(HashAlgorithm::SHA256, "triple-test-2");
    auto fp3 = hashString(HashAlgorithm::SHA256, "triple-test-3");

    for (auto & fp : {fp1, fp2, fp3}) {
        testFingerprint = fp;
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Verify all three serve warm hits
    for (auto & fp : {fp1, fp2, fp3}) {
        testFingerprint = fp;
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        (void) forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify source
    dir.addFile("triple.txt", "CHANGED TRIPLE CONTENT DATA WITH BIGGER SIZE");
    INVALIDATE_DIR(dir);

    // All three traces should re-evaluate
    for (auto & fp : {fp1, fp2, fp3}) {
        testFingerprint = fp;
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "all three traces must re-evaluate after modification";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Known soundness issues — documented as commented stubs
// ═══════════════════════════════════════════════════════════════════════

// KNOWN ISSUE 2: ParentContext non-transitive verification
// getCurrentTraceHash() returns stored hash without recursive parent verification.
// A parent trace may have stale deps while its trace_hash is unchanged,
// meaning child traces with ParentContext deps on the parent won't detect
// the staleness. Separate fix needed in trace-store.cc.

// KNOWN ISSUE 3: Empty sibling traces always verify as valid
// recordSiblingTrace() records zero-dep traces for already-forced siblings.
// These empty traces always pass verification (no deps to check), even when
// the sibling's actual value would differ. Separate fix needed in trace-cache.cc.

// KNOWN ISSUE 5: Parent overlay gap
// Parent structural change can invalidate child without changing child's deps.
// For example, adding/removing attributes in the parent attrset doesn't change
// the child's trace deps, but the child's attr path may resolve to a different
// position. Fundamental design limitation, needs parent-child dep edge.

// KNOWN ISSUE 7: prim_path expectedHash skips dep recording
// When expectedHash matches and store path is valid, fetchToStore is skipped
// entirely. No CopiedPath or NARContent dep is recorded for the source path.
// Separate fix needed in primops.cc (prim_path).

} // namespace nix::eval_trace
