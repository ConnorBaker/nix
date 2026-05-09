#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;

// P-STR — String builtins on traced data (readFile / fromJSON)
//
// Tests soundness (and basic precision) of string builtins whose inputs are
// traced via builtins.readFile or builtins.fromJSON.  Each test follows the
// standard cold → warm-hit → mutate → warm-miss pattern.
//
// Builtins covered:
//   match, split, hashString, toJSON (on fromJSON round-trip), baseNameOf,
//   splitVersion, compareVersions

class EvalTraceProperty_StringBuiltins : public TraceCacheFixture {
public:
    EvalTraceProperty_StringBuiltins() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-string-builtins");
    }
};

// ── builtins.match ───────────────────────────────────────────────────────────

// P-STR-a: match Soundness — changing the file content so that the match
// result changes forces re-evaluation.
//
// builtins.match is anchored: the entire string must match the regex.
// "([0-9]+)" against "123" matches and returns ["123"].
// Changing to "abc" makes the match return null (no match).
TEST_F(EvalTraceProperty_StringBuiltins, Match_Soundness)
{
    // Initial content: "123" is a whole-string match for ([0-9]+).
    TempTextFile file("123");
    auto const expr =
        "builtins.match \"([0-9]+)\" (builtins.readFile " + file.path.string() + ")";

    // Cold eval: records FileBytes dep; match returns ["123"].
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nList) << "expected list (match result) for '123'";
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate: "abc" does not match "([0-9]+)" — result changes from list to null.
    file.modify("abc");
    invalidateFileCache(file.path);

    // Warm eval: match result changed (list → null) — must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u) << "expected cache miss after file content change (match result changed)";
    }
}

// ── builtins.split ───────────────────────────────────────────────────────────

// P-STR-c: split Soundness — changing the file content changes the split result.
TEST_F(EvalTraceProperty_StringBuiltins, Split_Soundness)
{
    TempTextFile file("a:b:c");
    auto const expr =
        "builtins.split \":\" (builtins.readFile " + file.path.string() + ")";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nList) << "expected list from split";
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate: "x:y:z" produces a different split result.
    file.modify("x:y:z");
    invalidateFileCache(file.path);

    // Warm eval: split tokens changed — must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u) << "expected cache miss after file content change (split result changed)";
    }
}

// ── builtins.hashString ──────────────────────────────────────────────────────

// P-STR-d: hashString Soundness — changing the file content changes the hash.
TEST_F(EvalTraceProperty_StringBuiltins, HashString_Soundness)
{
    TempTextFile file("hello world");
    auto const expr =
        "builtins.hashString \"sha256\" (builtins.readFile " + file.path.string() + ")";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString) << "expected string from hashString";
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate: different content produces a different hash.
    file.modify("goodbye world!");
    invalidateFileCache(file.path);

    // Warm eval: hash changes — must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u) << "expected cache miss after file content change (hash changes)";
    }
}

// P-STR-e: hashString unchanged file — cache hit.
TEST_F(EvalTraceProperty_StringBuiltins, HashString_FileUnchanged_Hit)
{
    TempTextFile file("stable content");
    auto const expr =
        "builtins.hashString \"sha256\" (builtins.readFile " + file.path.string() + ")";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Warm eval: nothing changed — must be a cache hit.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit when file is unchanged";
    }
}

// ── builtins.toJSON on fromJSON round-trip ───────────────────────────────────

// P-STR-f: toJSON(fromJSON(readFile)) Soundness — changing the JSON content
// changes the toJSON output.
TEST_F(EvalTraceProperty_StringBuiltins, ToJSON_Soundness)
{
    TempJsonFile file(R"({"x":1,"y":"hello"})");
    auto const expr =
        "builtins.toJSON (builtins.fromJSON (builtins.readFile " + file.path.string() + "))";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString) << "expected string from toJSON";
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate: change both keys — toJSON serializes the full attrset.
    file.modify(R"({"x":99,"y":"changed"})");
    invalidateFileCache(file.path);

    // Warm eval: serialized output changes — must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u) << "expected cache miss after JSON content change (toJSON output changes)";
    }
}

// ── builtins.baseNameOf ──────────────────────────────────────────────────────

// P-STR-h: baseNameOf Soundness — changing the path string in the file changes
// the basename result.
TEST_F(EvalTraceProperty_StringBuiltins, BaseNameOf_Soundness)
{
    TempTextFile file("/foo/bar");
    auto const expr =
        "builtins.baseNameOf (builtins.readFile " + file.path.string() + ")";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString) << "expected string from baseNameOf";
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate: different basename "/foo/baz" → "baz".
    file.modify("/foo/baz");
    invalidateFileCache(file.path);

    // Warm eval: basename changed — must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u) << "expected cache miss after path content change (basename changes)";
    }
}

// ── builtins.splitVersion ────────────────────────────────────────────────────

// P-STR-i: splitVersion Soundness — changing the version string changes the
// split result.
TEST_F(EvalTraceProperty_StringBuiltins, SplitVersion_Soundness)
{
    TempTextFile file("1.2.3");
    auto const expr =
        "builtins.splitVersion (builtins.readFile " + file.path.string() + ")";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nList) << "expected list from splitVersion";
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate: "2.0.0" produces a different split list.
    file.modify("2.0.0");
    invalidateFileCache(file.path);

    // Warm eval: version parts changed — must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u) << "expected cache miss after version string change (splitVersion result changes)";
    }
}

// ── builtins.compareVersions ─────────────────────────────────────────────────

// P-STR-j: compareVersions Soundness — changing one of the two version files
// changes the comparison result.
TEST_F(EvalTraceProperty_StringBuiltins, CompareVersions_Soundness)
{
    TempTextFile fileA("1.0.0");
    TempTextFile fileB("2.0.0");
    auto const expr =
        "builtins.compareVersions"
        " (builtins.readFile " + fileA.path.string() + ")"
        " (builtins.readFile " + fileB.path.string() + ")";

    // Cold eval: 1.0.0 < 2.0.0 → result is -1.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt) << "expected int from compareVersions";
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate fileA to "3.0.0" so that fileA > fileB and the result flips to 1.
    fileA.modify("3.0.0");
    invalidateFileCache(fileA.path);

    // Warm eval: comparison result changed — must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u) << "expected cache miss after version file change (compareVersions result changes)";
    }
}

} // namespace nix::eval_trace::proptest
