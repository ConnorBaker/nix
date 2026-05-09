// Conditional Dep Inclusion property tests.
//
// Expression shape:
//   if builtins.pathExists <guard.txt> then builtins.readFile <data.txt> else "default"
//
// This models the NixOS pattern:
//   if pathExists ./config then import ./config else {}
//
// The guard file (guard.txt) controls which branch is taken.  The data file
// (data.txt) is only read when the guard exists (then-branch).
//
// Three properties are tested:
//
// 1. DataFile_Mutation_Invalidates (RC): When the guard exists and data.txt
//    changes, the cache must miss (soundness of the then-branch dep).
//
// 2. GuardDeletion_Invalidates (deterministic): Deleting guard.txt flips the
//    branch from then to else — the cache must miss.  Known file cache
//    interaction with exists→missing toggle; test may be skipped if flaky.
//
// 3. BranchNotTaken_DataMutation_NoInvalidation (deterministic): When the
//    guard does NOT exist (else-branch taken), mutating data.txt must NOT
//    invalidate — Nix lazy evaluation means readFile in the then-branch is
//    never forced.  If this fails (calls==1), the assertion documents eager
//    dep recording behavior instead.

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <unistd.h>  // getpid()

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ───────────────────────────────────────────────────────────────

class EvalTraceProperty_ConditionalDep : public TraceCacheFixture {
public:
    EvalTraceProperty_ConditionalDep() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-conditional-dep");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    params.maxDiscardRatio = 200;
    return params;
}

// ── Test 1: DataFile_Mutation_Invalidates (RapidCheck) ───────────────────
//
// Guard exists → cold eval → warm hit → mutate data.txt →
// invalidateFileCache(data.path) → warm eval must miss.
//
// RC_PRE guards:
//   - expectsSuccess(): only process well-formed expressions.
//   - depSlots[0].currentValue=="exists": guard file must be present so the
//     then-branch is taken and data.txt is actually read.
//   - depSlots[1].kind==File: data slot must be a plain file slot.
TEST_F(EvalTraceProperty_ConditionalDep, DataFile_Mutation_Invalidates)
{
    rc::detail::checkGTestWith(
        [this](TestExpr expr) {
            RC_PRE(expr.expectsSuccess());
            RC_PRE(expr.depSlots.size() == 2);
            RC_PRE(expr.depSlots[0].currentValue == "exists");
            RC_PRE(expr.depSlots[1].kind == DepSlot::Kind::File);

            auto & slot = expr.depSlots[1];  // data.txt slot

            // Cold eval — records trace (then-branch taken, data.txt read).
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Warm eval — confirm cache hit.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 0);
            }

            // Mutate data.txt with a new value.
            auto mutGen = slot.generateMutation();
            auto newValue = *mutGen;
            RC_PRE(newValue != slot.currentValue);
            slot.mutate(newValue);
            invalidateFileCache(slot.path);

            // Warm eval — must miss: data.txt changed, readFile result different.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 1);
            }

            // Restore for the next iteration.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// ── Test 2: GuardDeletion_Invalidates (deterministic) ────────────────────
//
// Guard exists, data has content.  Cold eval → warm hit → delete guard
// (std::filesystem::remove) → invalidateFileCache(guard.path) → warm eval
// must miss (branch flips from then to else, result changes from file content
// to "default").
//
// NOTE: The exists→missing toggle has a known file cache interaction
// where a stale entry may prevent the ExistenceCheck dep from observing
// the removal.  If this test proves flaky in CI, add GTEST_SKIP with a
// reference to the deferred-clear investigation (see pathexists-if.cc
// PathExists_IfThenElse_Soundness for the same known limitation).
TEST_F(EvalTraceProperty_ConditionalDep, GuardDeletion_Invalidates)
{
    // Build the expression manually: guard exists, data has known content.
    TempExtFile guardFile("txt", "");
    TempTextFile dataFile("hello from data");

    auto const nixCode =
        "if builtins.pathExists " + guardFile.path.string()
        + " then builtins.readFile " + dataFile.path.string()
        + " else \"default\"";

    // Cold eval — guard exists, then-branch taken, result = "hello from data".
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "hello from data");
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before guard deletion";
    }

    // Delete guard file — pathExists now returns false → branch flips.
    std::filesystem::remove(guardFile.path);
    invalidateFileCache(guardFile.path);

    // Warm eval — must miss: branch changed, result now "default".
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "cache miss expected: guard deleted, branch flips to else";
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "default");
    }
}

// ── Test 3: BranchNotTaken_DataMutation_NoInvalidation (deterministic) ───
//
// Guard does NOT exist (file absent from the start).
// Cold eval → result "default" → warm hit → modify data.txt →
// invalidateFileCache(data.path) → warm eval.
//
// Expected (lazy evaluation): calls==0 — readFile in the then-branch is
// never forced when pathExists returns false, so data.txt is not a dep.
//
// If the assertion fails (calls==1), the assertion below documents eager
// dep recording: Nix records readFile as a dep even when the branch is not
// taken.  The comment explains the observed behavior.
TEST_F(EvalTraceProperty_ConditionalDep, BranchNotTaken_DataMutation_NoInvalidation)
{
    // Build expression with a guard path that does NOT exist.
    // Use a unique path in the temp directory that is never created.
    auto tempDir = std::filesystem::canonical(std::filesystem::temp_directory_path()) / "nix-test-eval-trace";
    createDirs(tempDir);
    static std::atomic<int> counter{0};
    auto absentGuardPath = tempDir
        / ("absent-guard-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + ".txt");

    // Ensure the guard path does not exist.
    std::error_code ec;
    std::filesystem::remove(absentGuardPath, ec);

    // Create data.txt — it exists but the then-branch is never taken.
    TempTextFile dataFile("initial data content");

    auto const nixCode =
        "if builtins.pathExists " + absentGuardPath.string()
        + " then builtins.readFile " + dataFile.path.string()
        + " else \"default\"";

    // Cold eval — guard absent, else-branch taken, result = "default".
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "default");
    }

    // Warm eval — confirm cache hit baseline.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: nothing changed";
    }

    // Modify data.txt — the then-branch is NOT taken, so this should not
    // affect the cached result under lazy evaluation.
    {
        std::ofstream ofs(dataFile.path, std::ios::trunc);
        ofs << "mutated data content";
    }
    invalidateFileCache(dataFile.path);

    // Warm eval — assert lazy evaluation: readFile in then-branch not forced
    // when pathExists returns false, so data.txt is not a recorded dep.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        // Nix lazy evaluation: readFile in then-branch not forced when pathExists
        // returns false.
        EXPECT_EQ(calls, 0)
            << "cache hit expected: data.txt mutation should not invalidate "
               "when the then-branch is not taken (guard absent, else-branch active)";
        // If this assertion fails (calls==1), Nix records readFile as an eager
        // dep even when the branch is not taken.  In that case, change the
        // assertion to EXPECT_EQ(calls, 1) and document eager dep recording.
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "default");
    }
}

} // namespace nix::eval_trace::proptest
