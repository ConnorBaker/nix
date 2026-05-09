/**
 * Soundness test for source-tree copy invalidation.
 *
 * Reproduces a real-world soundness failure observed in the nixpkgs-release
 * benchmark: when evaluating `nix eval -f <dir>/entry.nix` across multiple
 * git checkouts, the eval-trace cache can serve a stale store path for the
 * source tree copy when files in the directory change between checkouts.
 *
 * Root cause: the eval-trace tracks per-file deps (FileBytes,
 * DirectoryEntries) for files that are explicitly read during evaluation.
 * When a directory is copied to the store (via string interpolation
 * `"${./dir}"`), a DerivedStorePath dep is recorded with the resulting
 * store path. However, if a file within the directory changes but is NOT
 * directly read by the expression, the per-file deps all pass verification
 * and the cached result (containing the stale store path) is served.
 *
 * Concrete scenario from the benchmark:
 *   - Commit N: eval closures.gnome -> copies nixpkgs to store -> hash A
 *   - Commit N-1: nixos files change, but x86_64-linux's trace deps
 *     don't include those files -> trace verifies -> serves hash A (stale)
 *   - Reference (no eval-trace): correctly produces hash B
 */

#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class SourceTreeSoundnessTest : public TraceCacheFixture
{
public:
    SourceTreeSoundnessTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "source-tree-soundness-test");
    }

protected:
    /**
     * Resolve symlinks in the temp directory path so that
     * directory-to-store copy (which calls resolveSymlinks on ancestors)
     * doesn't fail on macOS where /tmp -> /private/tmp.
     */
    static std::filesystem::path resolvedTempBase()
    {
        auto base = std::filesystem::canonical(std::filesystem::temp_directory_path()) / "nix-test-eval-trace-soundness";
        createDirs(base);
        return std::filesystem::canonical(base);
    }

    /**
     * RAII temp directory at a symlink-resolved path.
     * Unlike the standard TempDir, this resolves /tmp symlinks so that
     * directory-to-store copy works on macOS.
     */
    struct ResolvedTempDir {
        std::filesystem::path dir;
        ResolvedTempDir() {
            static std::atomic<int> counter = 0;
            dir = resolvedTempBase()
                / ("dir-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
            std::filesystem::create_directory(dir);
        }
        ~ResolvedTempDir() {
            try {
                deletePath(dir);
            } catch (...) {
            }
        }
        const std::filesystem::path & path() const { return dir; }
        void addFile(const std::string & name, const std::string & content) {
            std::ofstream ofs(dir / name);
            ofs << content;
        }
        ResolvedTempDir(const ResolvedTempDir &) = delete;
        ResolvedTempDir & operator=(const ResolvedTempDir &) = delete;
    };
};

/**
 * Core soundness test: a directory is copied to the store via "${./.}",
 * and the expression reads only one file in the directory. When a
 * different file in the directory changes, the store path changes, and
 * the cached result must invalidate.
 */
TEST_F(SourceTreeSoundnessTest, DirCopy_UnreadFileChange_Invalidates)
{
    ResolvedTempDir dir;
    dir.addFile("read.txt", "I am read by the expression");
    dir.addFile("unread.txt", "I am NOT read but AM in the directory copy");

    // Expression: reads one file AND coerces directory to string (store copy).
    // builtins.substring 0 0 extracts empty prefix — avoids /tmp symlink issues.
    auto dirStr = dir.path().string();
    auto expr =
        "let dir = " + dirStr + "; in "
        "(builtins.readFile (dir + \"/read.txt\")) + (builtins.substring 0 0 \"${dir}\")";

    // Cold eval -- records trace, copies dir to store
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Warm verify -- should hit cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "warm verify should hit cache";
    }

    // Modify the UNREAD file -- this changes the directory NAR hash and
    // thus the store path, but the expression never calls readFile on it.
    dir.addFile("unread.txt", "CHANGED CONTENT THAT SHOULD INVALIDATE THE STORE PATH!!!");
    invalidateFileCache(dir.path());

    // Warm verify -- the trace MUST invalidate because the store path changed.
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "SOUNDNESS: must re-evaluate when unread file in copied dir changes";
    }
}

/**
 * Variant: adding a NEW file to the directory should also invalidate.
 */
TEST_F(SourceTreeSoundnessTest, DirCopy_NewFileAdded_Invalidates)
{
    ResolvedTempDir dir;
    dir.addFile("read.txt", "I am read by the expression");

    auto dirStr = dir.path().string();
    auto expr =
        "let dir = " + dirStr + "; in "
        "(builtins.readFile (dir + \"/read.txt\")) + (builtins.substring 0 0 \"${dir}\")";

    // Cold eval
    {
        auto cache = makeCache(expr);
        forceRoot(*cache);
    }

    // Warm verify -- hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Add a new file -- directory content changes, store path should change
    dir.addFile("brand-new.txt", "this file did not exist during cold eval");
    invalidateFileCache(dir.path());

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "SOUNDNESS: must re-evaluate when new file added to copied dir";
    }
}

/**
 * Control: modifying the file that IS read should always invalidate.
 * This tests existing dep tracking and validates the test infrastructure.
 */
TEST_F(SourceTreeSoundnessTest, DirCopy_ReadFileChange_Invalidates)
{
    ResolvedTempDir dir;
    dir.addFile("read.txt", "original content");
    dir.addFile("unread.txt", "stable unread content");

    auto dirStr = dir.path().string();
    auto expr =
        "let dir = " + dirStr + "; in "
        "(builtins.readFile (dir + \"/read.txt\")) + (builtins.substring 0 0 \"${dir}\")";

    // Cold eval
    {
        auto cache = makeCache(expr);
        forceRoot(*cache);
    }

    // Warm verify -- hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify the read file
    dir.addFile("read.txt", "MODIFIED READ CONTENT WITH DIFFERENT SIZE!!!");
    invalidateFileCache(dir.path());

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "must re-evaluate when read file changes";
    }
}

/**
 * Minimal reproduction: directory copy only, no readFile.
 * If this fails, the DerivedStorePath dep itself is not being verified.
 */
TEST_F(SourceTreeSoundnessTest, DirCopy_OnlyCopy_FileChange_Invalidates)
{
    ResolvedTempDir dir;
    dir.addFile("content.txt", "some file content");

    auto dirStr = dir.path().string();
    auto expr = "builtins.substring 0 0 \"${" + dirStr + "}\"";

    // Cold eval
    std::string coldResult;
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        coldResult = std::string(v.string_view());
    }

    // Warm verify -- hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_EQ(std::string(v.string_view()), coldResult) << "warm hit should return same result";
    }

    // Modify file in dir
    dir.addFile("content.txt", "MODIFIED FILE CONTENT WITH DIFFERENT SIZE!!!");
    invalidateFileCache(dir.path());

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "must re-evaluate when file in copied dir changes";
    }
}

/**
 * Filtered copy: builtins.path with a filter copies the directory
 * and records NarIdentity deps for each file visited by the filter.
 * When a file changes, the NarIdentity dep should invalidate.
 */
TEST_F(SourceTreeSoundnessTest, FilteredCopy_UnreadFileChange_Invalidates)
{
    ResolvedTempDir dir;
    dir.addFile("read.nix", "42");
    dir.addFile("unread.nix", "99");

    auto dirStr = dir.path().string();
    // Use builtins.path with filter to simulate cleanSource.
    // The filter accepts all .nix files. import reads read.nix.
    // unread.nix passes the filter (gets NarIdentity dep) but
    // is never imported/readFile'd.
    auto expr =
        "let src = builtins.path { path = " + dirStr + "; "
        "  filter = path: type: true; name = \"test-src\"; }; "
        "in (import (src + \"/read.nix\")) + 0";

    // Cold eval
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(42));
    }

    // Warm verify -- hit
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify unread.nix -- filter visits it, NarIdentity dep should catch this
    dir.addFile("unread.nix", "12345678901234567890");
    invalidateFileCache(dir.path());

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "SOUNDNESS: must re-evaluate when filtered-but-unread file changes";
    }
}

/**
 * Two separate evaluations with the same filtered source.
 * The second eval should also record NarIdentity deps (not skip
 * because of fetchToStore caching).
 */
TEST_F(SourceTreeSoundnessTest, FilteredCopy_TwoTraces_BothInvalidate)
{
    ResolvedTempDir dir;
    dir.addFile("shared.nix", "1");
    dir.addFile("extra.nix", "2");

    auto dirStr = dir.path().string();
    auto expr1 =
        "let src = builtins.path { path = " + dirStr + "; "
        "  filter = path: type: true; name = \"test-src\"; }; "
        "in import (src + \"/shared.nix\")";
    auto expr2 =
        "let src = builtins.path { path = " + dirStr + "; "
        "  filter = path: type: true; name = \"test-src\"; }; "
        "in import (src + \"/extra.nix\")";

    // Cold eval both
    {
        auto cache = makeCache(expr1);
        forceRoot(*cache);
    }

    auto fp2 = hashString(HashAlgorithm::SHA256, "source-tree-soundness-test-2");
    auto origFp = testFingerprint;
    testFingerprint = fp2;
    {
        auto cache = makeCache(expr2);
        forceRoot(*cache);
    }

    // Warm hit both
    testFingerprint = origFp;
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr1, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
    testFingerprint = fp2;
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr2, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Modify extra.nix (only read by expr2)
    dir.addFile("extra.nix", "99999999");
    invalidateFileCache(dir.path());

    // Both should invalidate because the filtered copy includes extra.nix
    testFingerprint = origFp;
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr1, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "trace1 must re-evaluate when any filtered file changes";
    }
    testFingerprint = fp2;
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr2, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "trace2 must re-evaluate when any filtered file changes";
    }
}

/**
 * Per-leaf lazy verification contract (pins the cache's actual behavior).
 *
 * Shape: parent attrset's root has NO source-tree deps. Children hold
 * DerivedStorePath / FileBytes deps individually. When a source file
 * changes:
 *   - `forceRoot` warm-hits root (root's verify checks only root's own
 *     deps, which are empty w.r.t. the source tree) → rootLoader does
 *     NOT re-fire. This is CORRECT: root's cached attrset shape (the
 *     set of attr names) has not changed.
 *   - Forcing a specific child whose trace has the DSP/FileBytes dep
 *     triggers that child's verify, which fails, and the child
 *     re-evaluates to reflect the current source.
 *   - Forcing a sibling child whose own deps are unaffected by the
 *     source change continues to warm-hit and serve the cached value.
 *     This is precision: no over-invalidation.
 *
 * This test empirically pins that contract. Historical note: this test
 * was previously named `DISABLED_ParentChild_ChildInheritsStaleStorePath`
 * and asserted `EXPECT_EQ(loaderCalls, 1)` AFTER forceRoot alone, which
 * failed (==0) because the root's trace correctly has no source deps.
 * That assertion over-specified the cache's contract: it demanded that
 * forceRoot eagerly re-evaluate on any source change, which would
 * require either hoisting child source-copy deps up to the root (lost
 * precision: any source change invalidates all siblings via ParentSlot)
 * or eagerly forcing every child on root force (lost performance: defeats
 * laziness). Both violate the project's hard constraint on zero
 * precision/performance loss. The correct contract is per-leaf laziness,
 * enforced below.
 *
 * The benchmark failure at `closures.gnome.x86_64-linux` that OR-3
 * originally pointed at (CLAUDE.md:342-450) is a DIFFERENT shape: it
 * claims one leaf (`x86_64-linux`) is missing a dep another leaf
 * (`aarch64-linux`) correctly has. That asymmetry would be a real
 * per-leaf soundness bug, but it is NOT what this test reproduces and
 * it remains unreproduced in-process (CLAUDE.md §OR-3 "Benchmark still
 * needs confirmation").
 */
TEST_F(SourceTreeSoundnessTest, ParentChild_PerLeafLazyVerification)
{
    ResolvedTempDir dir;
    dir.addFile("common.nix", "{ val = 42; }");
    dir.addFile("extra.nix", "{ data = 99; }");

    auto dirStr = dir.path().string();
    auto expr =
        "let src = \"${" + dirStr + "}\"; "
        "in { "
        "  childA = (import (" + dirStr + " + \"/extra.nix\")).data + 0; "
        "  childB = (import (" + dirStr + " + \"/common.nix\")).val + 0; "
        "  srcPath = builtins.substring 0 46 src; "
        "}";

    // Cold eval: record traces for root + all children.
    std::string originalSrcPath;
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        ASSERT_EQ(v.type(), nAttrs);
        auto * sp = v.attrs()->get(state.symbols.create("srcPath"));
        ASSERT_NE(sp, nullptr);
        state.forceValue(*sp->value, noPos);
        originalSrcPath = std::string(sp->value->string_view());
    }

    // Warm no-change: root and all children hit cache.
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "No source change: full warm hit.";
        auto * sp = v.attrs()->get(state.symbols.create("srcPath"));
        ASSERT_NE(sp, nullptr);
        state.forceValue(*sp->value, noPos);
        EXPECT_EQ(std::string(sp->value->string_view()), originalSrcPath);
    }

    // Mutate extra.nix. Expected per-leaf invalidation behavior:
    //   - srcPath (DSP on dir): invalidates when forced.
    //   - childA (FileBytes on extra.nix): invalidates when forced.
    //   - childB (FileBytes on common.nix — unchanged): still hits.
    //   - root itself: warm-hits (no source-tree dep on root).
    dir.addFile("extra.nix", "{ data = 12345678; }");
    invalidateFileCache(dir.path());

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        // forceRoot alone: root warm-hits, loaderCalls stays 0.
        EXPECT_EQ(loaderCalls, 0)
            << "forceRoot must warm-hit: root's trace has no source-tree "
            << "deps, so its verify trivially passes. This is the lazy "
            << "verification contract.";

        // Force srcPath: its trace's DSP dep fails verify → re-eval.
        auto * sp = v.attrs()->get(state.symbols.create("srcPath"));
        ASSERT_NE(sp, nullptr);
        state.forceValue(*sp->value, noPos);
        EXPECT_EQ(loaderCalls, 1)
            << "Forcing srcPath with a changed DSP dep must trigger "
            << "re-eval via navigateToReal → getRealRoot → rootLoader.";
        EXPECT_NE(std::string(sp->value->string_view()), originalSrcPath)
            << "srcPath's value must reflect the new store path.";

        // Force childA: its trace's FileBytes(extra.nix) fails.
        auto * a = v.attrs()->get(state.symbols.create("childA"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        ASSERT_EQ(a->value->type(), nInt);
        EXPECT_EQ(a->value->integer().value, 12345678)
            << "childA must reflect mutated extra.nix content.";

        // Force childB: its trace's FileBytes(common.nix) still matches.
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        ASSERT_EQ(b->value->type(), nInt);
        EXPECT_EQ(b->value->integer().value, 42)
            << "childB must return 42 (common.nix unchanged).";
    }

    // Precision check in a fresh session: force ONLY childB. Because
    // childA and srcPath are not forced, their re-eval can't advance
    // loaderCalls; if childB correctly warm-hits (common.nix
    // unchanged), loaderCalls stays 0. If childB incorrectly
    // re-evaluates, loaderCalls becomes 1 (fresh realRoot via
    // rootLoader). This is the assertion that isolates precision from
    // the previous session's re-eval noise.
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_EQ(loaderCalls, 0)
            << "PRECISION: childB alone must warm-hit when common.nix "
            << "is unchanged, even though extra.nix (unrelated to "
            << "childB's deps) was mutated.";
        ASSERT_EQ(b->value->type(), nInt);
        EXPECT_EQ(b->value->integer().value, 42);
    }
}


/**
 * Thunk memoization soundness test.
 *
 * When two sibling attrset children force the same thunk that does a
 * directory copy (like nixpkgs.outPath = cleanSource ./..),
 * the FIRST child gets the NarIdentity/DerivedStorePath deps from
 * the copy. The SECOND child gets the cached thunk result without
 * re-running the copy, so it has NO source tree deps.
 *
 * When a file in the source tree changes:
 * - First child's trace FAILS (has the source deps)
 * - Second child's trace PASSES (has no source deps)
 * - Second child serves a stale result with the old store path
 *
 * This is the exact pattern from the nixpkgs-release benchmark:
 *   cleanSource is forced first by aarch64-linux, which gets the deps.
 *   x86_64-linux gets the memoized result with no deps.
 */
TEST_F(SourceTreeSoundnessTest, ThunkMemo_SecondChild_StaleResult)
{
    ResolvedTempDir dir;
    dir.addFile("read.nix", "42");
    dir.addFile("other.nix", "99");

    auto dirStr = dir.path().string();
    // Both children force the same let-binding (thunk) that does a dir copy.
    // childA is forced first → gets the DerivedStorePath dep.
    // childB is forced second → gets memoized result, no dep.
    auto expr =
        "let src = \"${" + dirStr + "}\"; in "
        "{ childA = builtins.substring 0 0 src; "
        "  childB = builtins.substring 0 0 src; }";

    // Cold eval — force both children
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto * a = v.attrs()->get(state.symbols.create("childA"));
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
    }

    // Warm verify — both should hit
    {
        int c = 0;
        auto cache = makeCache(expr, &c);
        auto v = forceRoot(*cache);
        auto * a = v.attrs()->get(state.symbols.create("childA"));
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        EXPECT_EQ(c, 0) << "both children should hit cache";
    }

    // Modify other.nix — changes the dir store path
    dir.addFile("other.nix", "12345678901234567890");
    invalidateFileCache(dir.path());

    // Warm verify — force childB FIRST (before childA).
    // In the benchmark, x86_64-linux (no source deps) is verified after
    // aarch64-linux. Here we force childB first to see if it independently
    // detects the change. childB has no DerivedStorePath dep (thunk was
    // memoized by childA during cold), so it should be the one that fails
    // to detect the change.
    {
        int c = 0;
        auto cache = makeCache(expr, &c);
        auto v = forceRoot(*cache);

        // Force childB first — it should NOT have the dir copy dep
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);

        // Check: did childB alone trigger re-evaluation?
        int cAfterB = c;

        // Now force childA — it should have the dep and trigger re-eval
        auto * a = v.attrs()->get(state.symbols.create("childA"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);

        // The soundness check: childB should have invalidated.
        // If cAfterB == 0, childB served a stale result.
        EXPECT_GE(cAfterB, 1) << "SOUNDNESS: childB (forced first, no dir dep from thunk memo) "
            "must also detect the dir content change. cAfterB=" << cAfterB
            << " cTotal=" << c;
    }
}

/**
 * Reproduces the benchmark pattern: builtins.path is called inside
 * an imported file (like nixpkgs release.nix calling cleanSource).
 * Two sibling children both import the same file that does the copy.
 * The first child gets the NarIdentity deps; the second may not.
 */
TEST_F(SourceTreeSoundnessTest, ImportedDirCopy_SecondChild_Soundness)
{
    ResolvedTempDir dir;
    dir.addFile("read.txt", "hello");
    dir.addFile("unread.txt", "world");

    // Create a .nix file that does the dir copy via builtins.path
    auto dirStr = dir.path().string();
    dir.addFile("copier.nix", "builtins.path { path = " + dirStr + "; filter = p: t: true; name = \"test-src\"; }");

    auto expr =
        "let src = import (" + dirStr + " + \"/copier.nix\"); in "
        "{ childA = builtins.substring 0 0 src; "
        "  childB = builtins.substring 0 0 src; }";

    // Cold eval — force both
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto * a = v.attrs()->get(state.symbols.create("childA"));
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
    }

    // Warm hit
    {
        int c = 0;
        auto cache = makeCache(expr, &c);
        auto v = forceRoot(*cache);
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        state.forceValue(*b->value, noPos);
        EXPECT_EQ(c, 0);
    }

    // Modify unread.txt — changes the filtered copy's store path
    dir.addFile("unread.txt", "CHANGED WORLD CONTENT MUCH LONGER!!!");
    invalidateFileCache(dir.path());

    // Force childB first (the second child that may lack deps)
    {
        int c = 0;
        auto cache = makeCache(expr, &c);
        auto v = forceRoot(*cache);
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_GE(c, 1) << "SOUNDNESS: childB must detect dir content change even when "
            "the builtins.path call was inside an imported file and the thunk "
            "was memoized by childA during cold recording.";
    }
}

/**
 * Shared-thunk replay regression guard (historically DISABLED; now enabled
 * 2026-04-30 after empirical verification that it passes).
 *
 * Shape: two sibling attrs both consume a shared `let`-bound thunk that
 * imports a common file (mirrors how nixpkgs systems share a package
 * definition via `genAttrs`). Cold recording: the first sibling forces
 * the shared thunk, records `FileBytes(common)` in its scope AND grows
 * the global epoch log. `recordThunkDeps(shared_value, epochStart)`
 * populates `epochMap[&shared_value]`. The second sibling forces
 * `shared_value` (now a non-thunk string); `replayMemoizedDeps` finds
 * the epochMap entry; `SiblingReplayCaptureScope::maybeCapture` fires
 * (sibling scope is active); a `TraceValueContext` dep linking to the
 * first sibling's trace is recorded into the second sibling's scope.
 *
 * After mutating the shared file: first sibling's FileBytes dep fails,
 * its trace invalidates. Second sibling's TraceValueContext dep triggers
 * recursive re-verification via `resolveTraceContextHash`
 * (trace-store-verify.cc:222), catches the first sibling's new trace
 * hash, and invalidates. Both siblings correctly reflect the new content.
 *
 * This test was previously DISABLED because in-process
 * `EvalEnvironmentSharedState::fileTraceCache` (which memoizes `import`
 * results by path+origin without content hash) would cause both
 * children to serve the stale pre-mutation value across `makeCache`
 * calls. `TraceCacheFixture::invalidateFileCache` now calls
 * `state.resetFileCache()` which clears `fileTraceCache`, making the
 * test pass.
 *
 * IMPORTANT: this test is NOT the reproducer for the
 * `closures.gnome.x86_64-linux` benchmark failure at
 * `nixpkgs@6b74cf77bde9`. CLAUDE.md §OR-3 notes the benchmark uses
 * subprocess-per-commit eval (so `fileTraceCache` is naturally empty)
 * yet still stale-serves, implying a root cause distinct from this
 * shape. Benchmark reproduction remains OR-3's blocker.
 */
TEST_F(SourceTreeSoundnessTest, SharedThunk_Siblings_TraceValueContext_InvalidatesBoth)
{
    ResolvedTempDir dir;
    dir.addFile("version.nix", "\"2.0.0\"");
    dir.addFile("first.nix", "\"aarch64\"");
    dir.addFile("second.nix", "\"x86_64\"");

    auto dirStr = dir.path().string();
    // Shared thunk: reads version.nix. Both children consume the result
    // through a per-system suffix — mirrors how systems reuse a shared
    // package definition via `genAttrs`.
    //
    //  - `first` child concatenates the shared version with its own marker
    //  - `second` child does the same with a different marker
    //
    // Cold recording order: `first` is forced first (alphabetical), which
    // is when the shared `version` thunk is evaluated. Current instrumentation
    // showed `second` replays the memoized dep range and records the same
    // version.nix deps, so this fixture should not produce the benchmark's
    // asymmetric stale-serve pattern.
    auto expr = fmt(R"(
        let version = import (%1$s + "/version.nix"); in {
          first  = version + "-" + import (%1$s + "/first.nix");
          second = version + "-" + import (%1$s + "/second.nix");
        }
    )", dirStr);

    std::string origFirst, origSecond;

    // Cold eval — force in attrset order (first, then second)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        ASSERT_EQ(v.type(), nAttrs);
        auto * a = v.attrs()->get(state.symbols.create("first"));
        auto * b = v.attrs()->get(state.symbols.create("second"));
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        origFirst = std::string(a->value->string_view());
        origSecond = std::string(b->value->string_view());
        ASSERT_EQ(origFirst, "2.0.0-aarch64");
        ASSERT_EQ(origSecond, "2.0.0-x86_64");
    }

    // Modify version.nix — both children's output must change
    dir.addFile("version.nix", "\"2.1.0\"");
    invalidateFileCache(dir.path());

    // Warm verify — force in attrset order (first, then second),
    // mirroring `nix eval --json` JSON serialization which enumerates
    // attrs alphabetically.
    {
        int c = 0;
        auto cache = makeCache(expr, &c);
        auto v = forceRoot(*cache);
        ASSERT_EQ(v.type(), nAttrs);

        auto * a = v.attrs()->get(state.symbols.create("first"));
        auto * b = v.attrs()->get(state.symbols.create("second"));
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);

        auto newFirst = std::string(a->value->string_view());
        auto newSecond = std::string(b->value->string_view());

        // Both children's VALUES must reflect the version change.
        // If this disabled fixture fails, investigate the local
        // replay/cache interaction; do not treat it as proof that the
        // subprocess-per-commit benchmark failure was reproduced.
        EXPECT_EQ(newFirst, "2.1.0-aarch64")
            << "first child (records deps through shared thunk) must "
            << "reflect the version.nix change";
        EXPECT_EQ(newSecond, "2.1.0-x86_64")
            << "SOUNDNESS: second child must also reflect the version.nix "
            << "change; a stale value here would be a local repro-adjacent "
            << "failure, not the confirmed nixpkgs-release benchmark root cause.";
    }
}

// ══════════════════════════════════════════════════════════════════════
// Warm-hit epochMap asymmetry probes (OR-3 follow-on)
// ──────────────────────────────────────────────────────────────────────
//
// Background: the cold-record path through `forceThunkValue`
// (eval.cc:1677-1705) populates `MemoReplayStore::epochMap[&v]` via
// `ReplayPublishScope`'s `recordThunkDeps` when the thunk's eval
// grows the global epoch log. Warm-hit through `TracedExpr::eval`
// (trace-session.cc:609-642) does NOT grow the log (replayTrace is
// gated on an active DepCaptureScope; `materializeResult` does not
// append to epochLog). `recordThunkDeps` then sees
// `epochStart == epochEnd` and creates no epochMap entry
// (memo-replay-store.hh:70 guard).
//
// Downstream consequence: `replayMemoizedDeps(v)` returns early if
// `replayBloom.test(&v)` is false (context.cc:821), which skips
// BOTH the direct replay path AND
// `SiblingReplayCaptureScope::maybeCapture`. A cold-recording trace
// that forces a warm-materialized Value thus records NEITHER a
// replayed dep set NOR a `TraceValueContext` dep for it.
//
// The tests below establish:
//   1. The mechanism-level asymmetry is real
//      (`WarmHit_Child_NoEpochMapEntry`).
//   2. Under the current architecture, cold re-eval traverses
//      `realRoot`'s fresh thunks via `navigateToReal`, not the
//      materialized tree; warm-materialized Values are unreachable
//      from cold re-eval in the tested shapes
//      (`WarmHit_ThenSourceMutation_SiblingsStillInvalidate`).
//
// These are regression guards. If a future refactor causes cold
// re-eval to reach a warm-materialized Value, (2) will fail and
// the asymmetry becomes a real soundness hole.
// ══════════════════════════════════════════════════════════════════════

/// Probe 1 — mechanism: warm-hit force of a TracedExpr thunk does
/// not populate epochMap for that Value.
TEST_F(SourceTreeSoundnessTest, WarmHit_Child_NoEpochMapEntry)
{
    ResolvedTempDir dir;
    dir.addFile("content.txt", "body");

    auto dirStr = dir.path().string();
    auto expr =
        "{ childA = builtins.substring 0 0 \"${" + dirStr + "}\"; }";

    // Cold-force populates epochMap for the thunk Value slot.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto * a = v.attrs()->get(state.symbols.create("childA"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_TRUE(state.mayHaveMemoizedDeps(*a->value))
            << "Cold-force must populate epochMap for the thunk Value.";
    }

    // Warm-hit force does not. `invalidateFileCache` clears replayStore,
    // so the warm phase starts with an empty epochMap.
    invalidateFileCache(dir.path());
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto * a = v.attrs()->get(state.symbols.create("childA"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_FALSE(state.mayHaveMemoizedDeps(*a->value))
            << "Warm-hit force must leave epochMap empty for the "
            << "materialized Value. If this fails, some code path "
            << "populates epochMap on warm-hit — re-examine "
            << "trace-session.cc's warm-hit branch and "
            << "materialize.cc's string_t materialization.";
    }
}

/// Probe 2 — soundness: even with the asymmetry, sibling invalidation
/// still works across sessions when each sibling records its own
/// source dep directly.
///
/// This is the shape where cold re-eval via `navigateToReal` enters
/// through `getRealRoot`'s fresh tree; warm-materialized Values are
/// not on that walk. Regression guard against architectural refactors
/// that might share materialized Values with the cold re-eval walk.
TEST_F(SourceTreeSoundnessTest,
       WarmHit_ThenSourceMutation_SiblingsStillInvalidate)
{
    ResolvedTempDir dir;
    dir.addFile("a.txt", "alpha");
    dir.addFile("b.txt", "bravo");

    auto dirStr = dir.path().string();
    // Each child has its own ${dir} coercion, so each records DSP
    // directly during its cold force.
    auto expr =
        "{ childA = builtins.substring 0 46 \"${" + dirStr + "}\"; "
        "  childB = builtins.substring 0 46 \"${" + dirStr + "}\"; }";

    std::string originalA;
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto * a = v.attrs()->get(state.symbols.create("childA"));
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        originalA = std::string(a->value->string_view());
    }

    dir.addFile("b.txt", "bravo-mutated-to-a-much-longer-string-for-nar-change");
    invalidateFileCache(dir.path());

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        auto * a = v.attrs()->get(state.symbols.create("childA"));
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);

        state.forceValue(*a->value, noPos);
        EXPECT_NE(std::string(a->value->string_view()), originalA)
            << "childA must invalidate on dir change.";

        state.forceValue(*b->value, noPos);
        EXPECT_NE(std::string(b->value->string_view()), originalA)
            << "childB must invalidate on dir change.";

        EXPECT_GE(loaderCalls, 1);
    }
}

/// Probe 3 — soundness: rec-attrset sibling consumption via the
/// `SiblingReplayCaptureScope::maybeCapture` path. When childB's
/// cold-recording body forces childA (already materialized in this
/// cold recording pass), `forceValue` hits `replayMemoizedDeps`,
/// which fires `maybeCapture` and records a `TraceValueContext`
/// dep in childB's scope. Verification of childB then recursively
/// re-verifies childA, catching source changes.
///
/// This test guards the TraceValueContext recording path. If a
/// refactor breaks it (e.g., by disabling sibling capture or
/// changing scope pushing order), childB would record no dep on
/// childA and would serve stale.
TEST_F(SourceTreeSoundnessTest,
       RecAttrsetSibling_TraceValueContext_InvalidatesOnSourceChange)
{
    ResolvedTempDir dir;
    dir.addFile("content.txt", "version-1");

    auto dirStr = dir.path().string();
    auto expr =
        "rec { "
        "  childA = \"${" + dirStr + "}\"; "
        "  childB = childA + \" suffix\"; "
        "}";

    std::string originalB;
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto * a = v.attrs()->get(state.symbols.create("childA"));
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        state.forceValue(*a->value, noPos);
        state.forceValue(*b->value, noPos);
        originalB = std::string(b->value->string_view());
    }

    dir.addFile("content.txt", "version-2-completely-different-content");
    invalidateFileCache(dir.path());

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        auto * b = v.attrs()->get(state.symbols.create("childB"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_NE(std::string(b->value->string_view()), originalB)
            << "childB's TraceValueContext dep on childA must "
            << "trigger recursive re-verification and catch the "
            << "source change. If this fails, either "
            << "SiblingReplayCaptureScope::maybeCapture isn't firing "
            << "during childB's cold force, or the stored "
            << "TraceValueContext dep isn't driving recursive verify.";
        EXPECT_GE(loaderCalls, 1);
    }
}

/// Probe 4 — soundness: nested attrset with only the innermost
/// trace carrying a source dep. Warm-hit chain traverses root→outer
/// (both warm-hit with no source dep) to inner (verify-fails). The
/// chain of warm-hits must not swallow the invalidation signal.
TEST_F(SourceTreeSoundnessTest,
       NestedAttrset_WarmHitChain_InnerInvalidatesOnSourceChange)
{
    ResolvedTempDir dir;
    dir.addFile("content.txt", "initial");

    auto dirStr = dir.path().string();
    auto expr =
        "{ outer = { inner = builtins.substring 0 46 \"${" + dirStr + "}\"; }; }";

    std::string originalInner;
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto * outer = v.attrs()->get(state.symbols.create("outer"));
        ASSERT_NE(outer, nullptr);
        state.forceValue(*outer->value, noPos);
        auto * inner = outer->value->attrs()->get(state.symbols.create("inner"));
        ASSERT_NE(inner, nullptr);
        state.forceValue(*inner->value, noPos);
        originalInner = std::string(inner->value->string_view());
    }

    dir.addFile("content.txt", "mutated-content-different-length");
    invalidateFileCache(dir.path());

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        auto * outer = v.attrs()->get(state.symbols.create("outer"));
        ASSERT_NE(outer, nullptr);
        state.forceValue(*outer->value, noPos);
        auto * inner = outer->value->attrs()->get(state.symbols.create("inner"));
        ASSERT_NE(inner, nullptr);
        state.forceValue(*inner->value, noPos);
        EXPECT_NE(std::string(inner->value->string_view()), originalInner);
        EXPECT_EQ(loaderCalls, 1);
    }
}

} // namespace nix::eval_trace
