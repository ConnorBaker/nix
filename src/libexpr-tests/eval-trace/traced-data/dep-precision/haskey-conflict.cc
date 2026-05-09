#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ═══════════════════════════════════════════════════════════════════════
// Scope stability: contradictory has:key observations on same origin
//
// When a JSON-derived attrset is used both directly (key exists) and via
// a subset/derivative (key absent), the same (source, file, dataPath,
// hasKey) dep key was observed with both sentinel(SentinelHash::One) and sentinel(SentinelHash::Zero),
// which marked the DepRecordingContext scope unstable and suppressed trace
// recording.
//
// Root cause: ProvenanceRecord identifies the data SOURCE, not the
// specific Nix Value* being checked. Derived attrsets (from
// intersectAttrs, removeAttrs, //) retain origin PosIdx from their
// source attrs, so forEachTracedDataOrigin finds the original origin
// even in subsets.
//
// Fixed by kHashOne-wins-over-kHashZero in observeDep (recording.cc):
// sentinel(SentinelHash::One) is a proof of existence in the source; sentinel(SentinelHash::Zero) from a
// derivative doesn't prove absence in the source.
// ═══════════════════════════════════════════════════════════════════════

class HasKeyConflictTest : public DepPrecisionTest
{
protected:
    struct EvalResult {
        std::vector<SqliteTraceStorage::ResolvedDep> deps;
        bool stable;
    };

    eval_trace::SemanticRegistry testRegistry;
    EvalResult evalAndCheckStability(const std::string & nixExpr)
    {
        auto & pools = state.tracingPools();
        DepCaptureScope depCapture(pools, testRegistry);
        TraceActivationScope traceActivation(state);
        (void) eval(nixExpr, /* forceValue */ true);
        bool stable = depCapture.isStable();
        auto deps = resolveDeps(pools, depCapture.finalizeAndTakeDeps());
        return {std::move(deps), stable};
    }
};

// ── Core conflict: hasAttr on original vs hasAttr on intersectAttrs subset ──

TEST_F(HasKeyConflictTest, IntersectAttrs_Subset_HasKey_Conflict)
{
    // JSON has keys {a, b, c}. intersectAttrs with {a, c} produces subset {a, c}.
    // Checking "b" on original → exists=true (kHashOne).
    // Checking "b" on subset → exists=false (kHashZero).
    // Same dep key (same provenance), different hashes → scope unstable.
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = std::format(R"(
        let
          json = {};
          subset = builtins.intersectAttrs {{ a = null; c = null; }} json;
        in (json ? b) && !(subset ? b)
    )", fj(file.path));

    auto [deps, stable] = evalAndCheckStability(expr);

    // The expression evaluates correctly (true).
    // But the scope must be stable — contradictory has:key observations
    // on the same provenance should not poison the scope.
    EXPECT_TRUE(stable)
        << "Scope should be stable: contradictory has:key on same provenance "
        << "from original vs intersectAttrs subset must not poison the scope.\n"
        << dumpDeps(deps);
}

// ── Same pattern with removeAttrs ──

TEST_F(HasKeyConflictTest, RemoveAttrs_Subset_HasKey_Conflict)
{
    // JSON has keys {x, y, z}. removeAttrs removes "y".
    // Checking "y" on original → exists=true.
    // Checking "y" on removeAttrs result → exists=false.
    TempJsonFile file(R"({"x": 1, "y": 2, "z": 3})");
    auto expr = std::format(R"(
        let
          json = {};
          without_y = builtins.removeAttrs json ["y"];
        in (json ? y) && !(without_y ? y)
    )", fj(file.path));

    auto [deps, stable] = evalAndCheckStability(expr);

    EXPECT_TRUE(stable)
        << "Scope should be stable: contradictory has:key from original vs "
        << "removeAttrs result must not poison the scope.\n"
        << dumpDeps(deps);
}

// ── intersectAttrs with two different JSON files, cross-conflict ──

TEST_F(HasKeyConflictTest, IntersectAttrs_TwoJsonFiles_NoConflict)
{
    // fileA has {a, b}, fileB has {a, c}.
    // intersectAttrs(fileA, fileB) = {a} — result attr comes from fileB (right operand).
    // Checking "b" on fileA → exists=true → kHashOne against fileA.
    // Checking "b" on result → exists=false → provenance scan finds fileB's
    // origin (from the "a" attr) → kHashZero against fileB, not fileA.
    // Different provenances → no conflict.
    TempJsonFile fileA(R"({"a": 1, "b": 2})");
    TempJsonFile fileB(R"({"a": 10, "c": 30})");
    auto expr = std::format(R"(
        let
          jsonA = {};
          jsonB = {};
          shared = builtins.intersectAttrs jsonA jsonB;
        in (jsonA ? b) && !(shared ? b)
    )", fj(fileA.path), fj(fileB.path));

    auto [deps, stable] = evalAndCheckStability(expr);

    EXPECT_TRUE(stable)
        << "No conflict expected: different provenances (fileA vs fileB).\n"
        << dumpDeps(deps);
}

// ── Conflict via // (update) operator ──

TEST_F(HasKeyConflictTest, Update_Subset_HasKey_Conflict)
{
    // JSON has {a, b, c}. Nix set has {a}. Update: nixSet // json = {a, b, c}.
    // intersectAttrs {a} (nixSet // json) = {a} — only "a" is in both.
    // Then check "b" on json (exists=true) and "b" on intersectAttrs result
    // (exists=false, but provenance scan finds json origin).
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = std::format(R"(
        let
          json = {};
          merged = {{ a = 99; }} // json;
          subset = builtins.intersectAttrs {{ a = null; }} merged;
        in (json ? b) && !(subset ? b)
    )", fj(file.path));

    auto [deps, stable] = evalAndCheckStability(expr);

    EXPECT_TRUE(stable)
        << "Scope should be stable: update + intersectAttrs conflict "
        << "must not poison the scope.\n"
        << dumpDeps(deps);
}

// ── Verify soundness is preserved: the trace must still be recordable ──

TEST_F(HasKeyConflictTest, IntersectSubset_Conflict_TraceStillRecorded)
{
    // Same conflict pattern, but verify the trace is actually recorded
    // and can be verified on re-evaluation (cache hit when file unchanged,
    // cache miss when key is removed).
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = std::format(R"(
        let
          json = {};
          subset = builtins.intersectAttrs {{ a = null; c = null; }} json;
        in (json ? b) && !(subset ? b)
    )", fj(file.path));

    // First eval: record trace.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    // Second eval, file unchanged: cache hit (no re-evaluation).
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "Unchanged file should produce a cache hit — "
            << "the trace must be recorded despite contradictory has:key.";
        EXPECT_THAT(v, IsTrue());
    }

    // Third eval, "b" removed from JSON: cache miss (must re-evaluate).
    file.modify(R"({"a": 1, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1)
            << "Removing 'b' from JSON should invalidate the trace.";
        EXPECT_THAT(v, IsFalse());
    }
}

// ── Edge: conflict from `builtins.hasAttr` (primop) vs `?` (ExprOpHasAttr) ──

TEST_F(HasKeyConflictTest, HasAttrPrimop_vs_QuestionMark_SameKey)
{
    // Both builtins.hasAttr and ? check the same key on different views.
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format(R"(
        let
          json = {};
          subset = builtins.intersectAttrs {{ x = null; }} json;
        in builtins.hasAttr "y" json && !(subset ? y)
    )", fj(file.path));

    auto [deps, stable] = evalAndCheckStability(expr);

    EXPECT_TRUE(stable)
        << "Scope should be stable: hasAttr primop vs ? operator conflict "
        << "on same provenance must not poison the scope.\n"
        << dumpDeps(deps);
}

// ── recordIntersectAttrsDeps: same pattern via disjoint-key recording ──

TEST_F(HasKeyConflictTest, IntersectAttrs_DisjointKey_RecordsAbsenceAgainstOrigin)
{
    // intersectAttrs(left, right) records sentinel(SentinelHash::Zero) for keys in left
    // that are absent from right. If right is a derivative of the same
    // JSON (e.g., via removeAttrs), the dep key is the same as a direct
    // hasAttr check on the original, producing a conflict.
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = std::format(R"(
        let
          json = {};
          withoutB = builtins.removeAttrs json ["b"];
          # intersectAttrs sees "b" in json but not in withoutB
          shared = builtins.intersectAttrs json withoutB;
        in (json ? b) && !(shared ? b)
    )", fj(file.path));

    auto [deps, stable] = evalAndCheckStability(expr);

    EXPECT_TRUE(stable)
        << "Scope should be stable: intersectAttrs disjoint-key recording "
        << "against shared origin must not poison the scope.\n"
        << dumpDeps(deps);
}

// ── Soundness: genuinely absent key must still invalidate on addition ──

TEST_F(HasKeyConflictTest, GenuineAbsence_KeyAdded_CacheMiss)
{
    // When a key is genuinely absent from the source (no conflict),
    // adding it must trigger a cache miss.
    TempJsonFile file(R"({"a": 1, "c": 3})");
    auto expr = std::format(R"(
        let json = {}; in json ? b
    )", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    // Add "b" to the file.
    file.modify(R"({"a": 1, "b": 2, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1)
            << "Adding 'b' to JSON should invalidate the trace.";
        EXPECT_THAT(v, IsTrue());
    }
}

// ── Soundness: kHashOne recorded via conflict must invalidate on removal ──

TEST_F(HasKeyConflictTest, ConflictResolved_KeyRemoved_CacheMiss)
{
    // After a conflict where sentinel(SentinelHash::One) wins, removing the key from the
    // source must trigger a cache miss (verifier computes kHashZero ≠ kHashOne).
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = std::format(R"(
        let
          json = {};
          subset = builtins.intersectAttrs {{ a = null; c = null; }} json;
        in (json ? b) && !(subset ? b)
    )", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    // Remove "b" — the conflict was resolved as kHashOne,
    // so this MUST invalidate.
    file.modify(R"({"a": 1, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1)
            << "Removing 'b' must invalidate even though the conflict "
            << "resolved to kHashOne.";
        EXPECT_THAT(v, IsFalse());
    }
}

// ── No conflict when key is genuinely absent from source ──

TEST_F(HasKeyConflictTest, NoConflict_KeyAbsent_FromSource)
{
    // When the key doesn't exist in the JSON at all, both the original
    // and derivative agree: sentinel(SentinelHash::Zero). No conflict.
    TempJsonFile file(R"({"a": 1, "c": 3})");
    auto expr = std::format(R"(
        let
          json = {};
          subset = builtins.intersectAttrs {{ a = null; }} json;
        in !(json ? b) && !(subset ? b)
    )", fj(file.path));

    auto [deps, stable] = evalAndCheckStability(expr);

    EXPECT_TRUE(stable)
        << "No conflict expected: key absent from source and derivative.\n"
        << dumpDeps(deps);
}

} // namespace nix::eval_trace
