// PathExists × if-then-else property tests — the CRITICAL gap.
//
// This is the canonical nixpkgs pattern:
//   if builtins.pathExists <file> then builtins.readFile <file> else "missing"
//
// The dep set must include:
//   1. An ExistenceCheck dep for the pathExists call.
//   2. A FileBytes dep for the readFile call (only when the file exists and
//      the then-branch is taken).
//
// Soundness requires that:
//   - Deleting the file invalidates (existence changes; branch changes).
//   - Recreating the file with different content invalidates (readFile result changes).
//
// Precision requires that:
//   - When nothing changes, the cache hits.
//   - When an unrelated file changes, the cache still hits.
//
// Test strategy: deterministic, hand-crafted tests (no RapidCheck).
// All tests use the standard cold→warm-hit→mutate→warm-miss cycle.

#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ──────────────────────────────────────────────────────────────────

class EvalTraceProperty_PathExistsIf : public TraceCacheFixture {
public:
    EvalTraceProperty_PathExistsIf() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-pathexists-if-test");
    }

protected:
    /// Write content to path (creates or overwrites).
    static void writeFile(const std::filesystem::path & p, std::string_view content)
    {
        std::ofstream ofs(p, std::ios::trunc);
        ofs << content;
    }

    /// Delete a file, ignoring errors.
    static void deleteFile(const std::filesystem::path & p)
    {
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }
};

// ── Test a: PathExists_IfThenElse_Soundness ───────────────────────────────
//
// Expression: if builtins.pathExists <file> then builtins.readFile <file> else "missing"
//
// Phase 1: File exists with content "hello" → cold eval → "hello".
// Phase 2: Warm eval → cache hit (nothing changed).
// Phase 3: Delete file → invalidateFileCache → warm eval → must miss ("missing" now).
// Phase 4: Recreate file with content "world" → warm eval → must miss (content changed).
//
TEST_F(EvalTraceProperty_PathExistsIf, PathExists_IfThenElse_Soundness)
{
    TempTextFile file("hello");
    auto const filePath = file.path.string();
    auto const nixCode =
        "if builtins.pathExists " + filePath
        + " then builtins.readFile " + filePath
        + " else \"missing\"";

    // Phase 1: Cold eval — file exists, returns "hello".
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "hello");
    }

    // Phase 2: Warm eval — nothing changed, must hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: nothing changed";
    }

    // Phase 3: Delete file → existence changes → branch flips → result changes.
    deleteFile(file.path);
    invalidateFileCache(file.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: file deleted, result changes to 'missing'";
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "missing");
    }

    // Phase 4: Recreate file with different content → must miss again.
    writeFile(file.path, "world");
    invalidateFileCache(file.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: file recreated with different content";
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "world");
    }
}

// ── Test b: PathExists_IfThenElse_Precision ───────────────────────────────
//
// File exists, cold eval → warm hit (nothing changed).
// Create a DIFFERENT (unrelated) file → warm eval must still hit.
TEST_F(EvalTraceProperty_PathExistsIf, PathExists_IfThenElse_Precision)
{
    TempTextFile file("content");
    auto const filePath = file.path.string();
    auto const nixCode =
        "if builtins.pathExists " + filePath
        + " then builtins.readFile " + filePath
        + " else \"missing\"";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "content");
    }

    // Warm eval — confirm cache hit baseline.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: nothing changed";
    }

    // Create an unrelated file — the expression does NOT reference it.
    TempTextFile unrelated("unrelated content");
    invalidateFileCache(unrelated.path);

    // Warm eval — the unrelated file is not in the dep set → must hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0)
            << "cache hit expected: unrelated file created, does not affect pathExists/readFile deps";
    }
}

// ── Test c: PathExists_IfThenElse_Toggle ──────────────────────────────────
//
// Full toggle sequence: missing → exists → missing.
//
// Phase 1: File absent → "no".
// Phase 2: Create file → invalidateFileCache → "yes" (must miss).
// Phase 3: Delete file → invalidateFileCache → "no" again (must miss).
//
TEST_F(EvalTraceProperty_PathExistsIf, PathExists_IfThenElse_Toggle)
{
    // Start with a path that does NOT exist yet.  We need a path in the temp
    // directory but we will NOT create the file initially.
    auto tempDir = std::filesystem::canonical(std::filesystem::temp_directory_path()) / "nix-test-eval-trace";
    createDirs(tempDir);
    static std::atomic<int> counter = 0;
    auto togglePath = tempDir
        / ("toggle-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + ".txt");

    // Ensure the file does not exist.
    deleteFile(togglePath);

    auto const pathStr = togglePath.string();
    auto const nixCode =
        "if builtins.pathExists " + pathStr
        + " then \"yes\" else \"no\"";

    // Phase 1: Cold eval — file absent → "no".
    // Uses its own fingerprint so Phase 1's historical trace cannot be
    // recovered during Phase 3 (which would mask the exists→missing miss).
    testFingerprint = hashString(HashAlgorithm::SHA256, "prop-pathexists-if-toggle-phase1");

    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "no");
    }

    // Warm eval — confirm cache hit baseline.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: file still absent";
    }

    // Phase 2+3 share a separate fingerprint.  Phase 2 records
    // ExistenceCheck="type:N" (file exists).  Phase 3 must fail verification
    // of that trace because the file is now absent — no historical trace
    // in this namespace can mask the miss.
    testFingerprint = hashString(HashAlgorithm::SHA256, "prop-pathexists-if-toggle-phase2");

    // Phase 2: Create file → existence flips → result changes to "yes".
    writeFile(togglePath, "exists now");
    invalidateFileCache(togglePath);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: file created, pathExists now true";
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "yes");
    }

    // Warm eval after re-recording — confirm hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: file exists, no change since last eval";
    }

    // Phase 3: Delete file → existence flips back → result changes to "no".
    deleteFile(togglePath);
    invalidateFileCache(togglePath);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: file deleted, pathExists now false again";
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "no");
    }
}

} // namespace nix::eval_trace::proptest
