// tryEval error-path property tests.
//
// Deterministic tests for builtins.tryEval error handling.  Verifies:
//   1. Flag toggle invalidation: changing a flag from true→false causes the
//      throw path to activate, invalidating the cached success result.
//   2. No dep leakage: when the then-branch is never taken (flag=false),
//      files referenced only in the then-branch are NOT in the dep set.
//
// These complement the RapidCheck success-path tests in try-eval.cc.

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

class EvalTraceProperty_TryEvalError : public TraceCacheFixture {
public:
    EvalTraceProperty_TryEvalError() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-try-eval-error");
    }
};

// ── ErrorPath_FlagToggle_Invalidates ─────────────────────────────────────────
//
// Expression:
//   (builtins.tryEval
//     (if (builtins.fromJSON (builtins.readFile <json>))."flag"
//      then "ok"
//      else throw "disabled")).success
//
// JSON starts as {"flag": true, "data": "hello"}.
//   Cold eval: flag=true → "ok" → tryEval.success = true.
//   Mutate: flag → false → throw "disabled" → tryEval.success = false.
TEST_F(EvalTraceProperty_TryEvalError, FlagToggle_Invalidates)
{
    TempJsonFile file(R"({"flag": true, "data": "hello"})");
    auto nixCode =
        "(builtins.tryEval "
        "(if (builtins.fromJSON (builtins.readFile "
        + file.path.string()
        + ")).\"flag\" then \"ok\" else throw \"disabled\")).success";

    // Cold eval — flag=true → "ok" → tryEval.success = true.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_TRUE(v.boolean()) << "tryEval should succeed when flag=true";
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before flag mutation";
    }

    // Mutate: flip flag to false.
    file.modify(R"({"flag": false, "data": "hello"})");
    invalidateFileCache(file.path);

    // Cold eval after mutation — flag=false → throw → tryEval.success = false.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "flag change must invalidate the cache";
        EXPECT_EQ(v.type(), nBool);
        EXPECT_FALSE(v.boolean()) << "tryEval should fail when flag=false (throw path)";
    }

    // Warm eval — new result is cached; must hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected after flag mutation";
        EXPECT_EQ(v.type(), nBool);
        EXPECT_FALSE(v.boolean());
    }
}

// ── ErrorPath_NoDepsLeaked ────────────────────────────────────────────────────
//
// Expression:
//   (builtins.tryEval
//     (if (builtins.fromJSON (builtins.readFile <json>))."flag"
//      then builtins.readFile <other>
//      else throw "no")).success
//
// JSON = {"flag": false}.  The then-branch (readFile <other>) is never evaluated.
// Modifying <other> must NOT invalidate the cache (lazy dep recording).
TEST_F(EvalTraceProperty_TryEvalError, NoDepsLeaked)
{
    TempJsonFile jsonFile(R"({"flag": false})");
    TempTextFile otherFile("initial content of other file");

    auto nixCode =
        "(builtins.tryEval "
        "(if (builtins.fromJSON (builtins.readFile "
        + jsonFile.path.string()
        + ")).\"flag\" then builtins.readFile "
        + otherFile.path.string()
        + " else throw \"no\")).success";

    // Cold eval — flag=false → throw → tryEval.success = false.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_FALSE(v.boolean());
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0);
    }

    // Modify <other> — the then-branch file that was never read.
    otherFile.modify("mutated content — then-branch was never taken");
    invalidateFileCache(otherFile.path);

    // Warm eval — must still hit: <other> is not in the dep set.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0)
            << "cache must not depend on <other> because the then-branch was never evaluated";
        EXPECT_EQ(v.type(), nBool);
        EXPECT_FALSE(v.boolean());
    }
}

} // namespace nix::eval_trace::proptest
