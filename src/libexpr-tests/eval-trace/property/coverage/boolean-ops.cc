// Boolean operations on traced booleans from fromJSON.
//
// Tests that dep tracking correctly propagates through boolean operators
// (&&, ||, !, ->) operating on values sourced from fromJSON (readFile f).
//
// Each soundness test changes an operand and expects a cache miss (the
// boolean result changes).  The precision test changes an unrelated key
// in the same JSON file and expects a cache hit (the accessed operands
// are unchanged so the SC deps still verify).
//
// The fixture uses TempJsonFile with {"a": true, "b": false, "z": 42} as
// the base content.

#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;

// ── Fixture ─────────────────────────────────────────────────────────────────

class EvalTraceProperty_BooleanOps : public TraceCacheFixture {
public:
    EvalTraceProperty_BooleanOps() {
        testFingerprint = hashString(HashAlgorithm::SHA256,
                                    "prop-boolean-ops-traced-data");
    }
};

// ── Helper ───────────────────────────────────────────────────────────────────

// Build `(fromJSON (readFile PATH)).KEY` expression fragment.
static std::string fj(const std::filesystem::path & path, const std::string & key)
{
    return "(builtins.fromJSON (builtins.readFile " + path.string() + "))." + key;
}

// ── a) And_Soundness ─────────────────────────────────────────────────────────
//
// Expression:  (fromJSON (readFile f)).a && (fromJSON (readFile f)).b
//
// Initial: true && false = false.
// Soundness: change a from true to false → result unchanged (false && false = false),
// so we instead change b from false to true → result changes (true && true = true) → miss.
TEST_F(EvalTraceProperty_BooleanOps, And_Soundness)
{
    TempJsonFile file(R"({"a": true, "b": false, "z": 42})");
    auto expr = fj(file.path, "a") + " && " + fj(file.path, "b");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_EQ(v.boolean(), false);
    }

    // Warm hit (pre-condition).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change b from false to true: true && true = true (result changes).
    file.modify(R"({"a": true, "b": true, "z": 42})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: b changed, && result must invalidate";
        EXPECT_EQ(v.boolean(), true);
    }
}

// ── b) And_Precision ─────────────────────────────────────────────────────────
//
// Expression:  (fromJSON (readFile f)).a && (fromJSON (readFile f)).b
//
// Precision: change unrelated key z → a and b deps unchanged → cache hit.
TEST_F(EvalTraceProperty_BooleanOps, And_Precision)
{
    TempJsonFile file(R"({"a": true, "b": false, "z": 42})");
    auto expr = fj(file.path, "a") + " && " + fj(file.path, "b");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change z only — a and b are unchanged.
    file.modify(R"({"a": true, "b": false, "z": 9999})");
    invalidateFileCache(file.path);

    // Warm eval: must remain a cache hit (precision).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0)
            << "cache hit expected: only unrelated key z changed";
        EXPECT_EQ(v.boolean(), false);
    }
}

// ── c) Or_Soundness ──────────────────────────────────────────────────────────
//
// Expression:  (fromJSON (readFile f)).a || (fromJSON (readFile f)).b
//
// Initial: true || false = true.
// Soundness: change a from true to false → false || false = false → miss.
TEST_F(EvalTraceProperty_BooleanOps, Or_Soundness)
{
    TempJsonFile file(R"({"a": true, "b": false, "z": 42})");
    auto expr = fj(file.path, "a") + " || " + fj(file.path, "b");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_EQ(v.boolean(), true);
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change a from true to false: false || false = false (result changes).
    file.modify(R"({"a": false, "b": false, "z": 42})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: a changed, || result must invalidate";
        EXPECT_EQ(v.boolean(), false);
    }
}

// ── d) Not_Soundness ─────────────────────────────────────────────────────────
//
// Expression:  ! (fromJSON (readFile f)).a
//
// Initial: !true = false.
// Soundness: change a from true to false → !false = true → miss.
TEST_F(EvalTraceProperty_BooleanOps, Not_Soundness)
{
    TempJsonFile file(R"({"a": true, "b": false, "z": 42})");
    auto expr = "! " + fj(file.path, "a");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_EQ(v.boolean(), false);
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change a from true to false: !false = true (result changes).
    file.modify(R"({"a": false, "b": false, "z": 42})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: a changed, ! result must invalidate";
        EXPECT_EQ(v.boolean(), true);
    }
}

// ── e) Implication_Soundness ──────────────────────────────────────────────────
//
// Expression:  (fromJSON (readFile f)).a -> (fromJSON (readFile f)).b
//
// a -> b is equivalent to !a || b.
// Initial: true -> false = false.
// Soundness: change a from true to false → false -> false = true → miss.
TEST_F(EvalTraceProperty_BooleanOps, Implication_Soundness)
{
    TempJsonFile file(R"({"a": true, "b": false, "z": 42})");
    auto expr = fj(file.path, "a") + " -> " + fj(file.path, "b");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool);
        EXPECT_EQ(v.boolean(), false);  // true -> false = false
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change a from true to false: false -> false = true (result changes).
    file.modify(R"({"a": false, "b": false, "z": 42})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: a changed, -> result must invalidate";
        EXPECT_EQ(v.boolean(), true);  // false -> false = true
    }
}

} // namespace nix::eval_trace::proptest
