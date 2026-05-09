// Type-check builtins on traced data from fromJSON.
//
// Tests that dep tracking correctly propagates through type-check builtins
// (typeOf, isAttrs, isInt, isList, isString) operating on values sourced
// from fromJSON (readFile f) or builtins.readFile f.
//
// Soundness tests change the observed type or value so the result changes.
// Precision tests keep the type (and hence the typeOf result) the same.
//
// Test content layout:
//   a) TypeOf_Soundness              — change x from int to string → miss
//   b) TypeOf_CorrectlyInvalidates   — change x value (same int type) → miss (Content dep)
//   c) IsAttrs_Soundness             — fromJSON attrset, change to JSON array → miss
//   d) IsInt_Soundness               — change x from int to string → miss
//   e) IsList_Soundness              — fromJSON array: add element → miss
//   f) IsString_OnTracedString       — readFile is always a string; content change → hit

#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;

// ── Fixture ─────────────────────────────────────────────────────────────────

class EvalTraceProperty_TypeChecks : public TraceCacheFixture {
public:
    EvalTraceProperty_TypeChecks() {
        testFingerprint = hashString(HashAlgorithm::SHA256,
                                    "prop-type-checks-traced-data");
    }
};

// ── Helper ───────────────────────────────────────────────────────────────────

// Build `(fromJSON (readFile PATH)).KEY` expression fragment.
static std::string fj(const std::filesystem::path & path, const std::string & key)
{
    return "(builtins.fromJSON (builtins.readFile " + path.string() + "))." + key;
}

// Build `fromJSON (readFile PATH)` (no key access — returns the whole value).
static std::string fjRoot(const std::filesystem::path & path)
{
    return "(builtins.fromJSON (builtins.readFile " + path.string() + "))";
}

// ── a) TypeOf_Soundness ───────────────────────────────────────────────────────
//
// Expression:  builtins.typeOf (fromJSON (readFile f)).x
//
// Initial: {"x": 42} → typeOf returns "int".
// Soundness: change x to a string → typeOf returns "string" → cache miss.
TEST_F(EvalTraceProperty_TypeChecks, TypeOf_Soundness)
{
    TempJsonFile file(R"({"x": 42})");
    auto expr = "builtins.typeOf " + fj(file.path, "x");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "int");
    }

    // Warm hit (pre-condition).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change x from int to string: typeOf changes from "int" to "string".
    file.modify(R"({"x": "hello"})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: x changed type from int to string";
        EXPECT_EQ(std::string_view(v.string_view()), "string");
    }
}

// ── b) TypeOf_CorrectlyInvalidates ───────────────────────────────────────────
//
// Expression:  builtins.typeOf (fromJSON (readFile f)).x
//
// Per-element SC deps cause correct over-invalidation; this is a soundness test, not precision.
// typeOf on a scalar (int) does NOT record a #type SC dep (see dep-precision/typeof.cc:
// TypeOf_Scalar_NoSCType). Instead, the scalar value itself is the dep (StructuredProjection
// Content dep). Changing x's VALUE (1 → 2) without changing its type still invalidates
// because the Content dep on the scalar value changes.
TEST_F(EvalTraceProperty_TypeChecks, TypeOf_CorrectlyInvalidates)
{
    TempJsonFile file(R"({"x": 1})");
    auto expr = "builtins.typeOf " + fj(file.path, "x");

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
        EXPECT_EQ(std::string_view(v.string_view()), "int");
    }

    // Warm hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Change x from 1 to 2 — same type (int), typeOf still returns "int",
    // but the scalar Content dep changes → cache correctly invalidates.
    file.modify(R"({"x": 2})");
    invalidateFileCache(file.path);

    // Warm eval correctly invalidates: typeOf on a scalar records a Content dep
    // (not a #type SC dep), so any value change causes a cache miss.
    // This is correct behavior (over-invalidation relative to the typeOf result,
    // but sound: no stale results are served).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: typeOf on scalar records Content dep — "
               "value change (1→2) invalidates even though type is still int";
        EXPECT_EQ(std::string_view(v.string_view()), "int");
    }
}

// ── c) IsAttrs_Soundness ──────────────────────────────────────────────────────
//
// Expression:  builtins.isAttrs (fromJSON (readFile f))
//
// A JSON object is always an attrset, so isAttrs returns true.
// Soundness: replace JSON object with a JSON array → fromJSON returns a list →
// isAttrs returns false → cache miss.
TEST_F(EvalTraceProperty_TypeChecks, IsAttrs_Soundness)
{
    TempJsonFile file(R"({"key": "value"})");
    auto expr = "builtins.isAttrs " + fjRoot(file.path);

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

    // Replace object with array: isAttrs returns false (result changes).
    file.modify(R"([1, 2, 3])");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: JSON object replaced by array, isAttrs changes";
        EXPECT_EQ(v.boolean(), false);
    }
}

// ── d) IsInt_Soundness ────────────────────────────────────────────────────────
//
// Expression:  builtins.isInt (fromJSON (readFile f)).x
//
// Initial: {"x": 42} → isInt returns true.
// Soundness: change x to a string → isInt returns false → cache miss.
TEST_F(EvalTraceProperty_TypeChecks, IsInt_Soundness)
{
    TempJsonFile file(R"({"x": 42})");
    auto expr = "builtins.isInt " + fj(file.path, "x");

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

    // Change x from int to string: isInt returns false (result changes).
    file.modify(R"({"x": "hello"})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: x changed from int to string, isInt changes";
        EXPECT_EQ(v.boolean(), false);
    }
}

// ── e) IsList_Soundness ───────────────────────────────────────────────────────
//
// Expression:  builtins.isList (fromJSON (readFile f))
//
// Initial: [1, 2] → isList returns true.
// Soundness: replace with JSON object → isList returns false → cache miss.
TEST_F(EvalTraceProperty_TypeChecks, IsList_Soundness)
{
    TempJsonFile file(R"([1, 2])");
    auto expr = "builtins.isList " + fjRoot(file.path);

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

    // Replace array with object: isList returns false (result changes).
    file.modify(R"({"key": "value"})");
    invalidateFileCache(file.path);

    // Warm eval: must re-evaluate (soundness).
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: JSON array replaced by object, isList changes";
        EXPECT_EQ(v.boolean(), false);
    }
}

// ── f) IsString_OnTracedString ────────────────────────────────────────────────
//
// Expression:  builtins.isString (builtins.readFile f)
//
// builtins.readFile always returns a string, so isString always returns true.
// Changing the file content (while keeping it a string read) does not change
// the result of isString — so the cache should hit.
//
// This tests precision: the SC dep for the type check should record only the
// type information, not the full content, so a content-only change leaves
// the typecheck result dep satisfied.
//
// NOTE: readFile records a FileBytes dep (not a StructuredContent dep), so the
// full file content is in the dep hash.  A content change WILL invalidate the
// FileBytes dep even though isString is constant-true.  This test therefore
// documents the known over-approximation: the cache misses because the whole
// readFile content is a dep, not just the type.
//
// The assertion is EXPECT_EQ(calls, 1) to document the current behavior.
// If a future optimization records only a type dep for isString(readFile f),
// this assertion should be updated to EXPECT_EQ(calls, 0) with a comment.
TEST_F(EvalTraceProperty_TypeChecks, IsString_OnTracedString)
{
    TempTextFile file("hello, world");
    auto expr = "builtins.isString (builtins.readFile " + file.path.string() + ")";

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

    // Change file content — readFile still returns a string, isString still true.
    file.modify("different content entirely — much longer than the original");
    invalidateFileCache(file.path);

    // Warm eval: documents current behavior.
    // readFile records a FileBytes dep; content change causes a miss even though
    // the isString result is unchanged.  This is the known over-approximation.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        // Document current behavior: FileBytes dep invalidates on content change.
        EXPECT_EQ(calls, 1)
            << "cache miss expected: readFile records FileBytes dep which "
               "invalidates on content change (known over-approximation for isString)";
        EXPECT_EQ(v.boolean(), true);
    }
}

} // namespace nix::eval_trace::proptest
