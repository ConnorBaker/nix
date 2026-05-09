#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;

// P-TOML — builtins.fromTOML property tests
//
// builtins.fromTOML has zero property coverage in the existing suite.
// These tests exercise fromTOML through the eval-trace cache pipeline,
// covering soundness and precision for attribute access, attrNames, and
// hasAttr operations on traced TOML data.
//
// TOML content used throughout:
//
//   key_a = "hello"
//   key_b = 42
//   key_c = true
//
// All tests follow the standard cold → warm-hit → mutate → warm-miss/hit
// pattern.  Precision tests assert loaderCalls == 0 after mutating a key
// that was NOT accessed by the expression (SC dep override).

class EvalTraceProperty_FromTOML : public TraceCacheFixture {
public:
    EvalTraceProperty_FromTOML() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-fromtoml");
    }
};

// Base TOML content shared by all tests.
static constexpr std::string_view kTomlBase =
    "key_a = \"hello\"\nkey_b = 42\nkey_c = true\n";

// ── Attribute access ──────────────────────────────────────────────────────────

// P-TOML-a: AttrAccess Soundness — changing the value of key_a invalidates
// the cached result of accessing key_a.
TEST_F(EvalTraceProperty_FromTOML, FromTOML_AttrAccess_Soundness)
{
    TempExtFile file("toml", kTomlBase);
    auto const expr =
        "(builtins.fromTOML (builtins.readFile " + file.path.string() + ")).key_a";

    // Cold eval: records SC dep on key_a and evaluates to "hello".
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString) << "expected string for key_a";
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate: change key_a's value — the accessed key's SC dep hash changes.
    file.modify("key_a = \"world\"\nkey_b = 42\nkey_c = true\n");
    invalidateFileCache(file.path);

    // Warm eval: key_a changed — must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u) << "expected cache miss after key_a value change";
    }
}

// P-TOML-b: AttrAccess Precision — changing key_b (unaccessed) does NOT
// invalidate the cached result of accessing key_a.
// The SC dep override records only the accessed key (key_a), so a change
// to an unaccessed key (key_b) must not cause re-evaluation.
TEST_F(EvalTraceProperty_FromTOML, FromTOML_AttrAccess_Precision)
{
    TempExtFile file("toml", kTomlBase);
    auto const expr =
        "(builtins.fromTOML (builtins.readFile " + file.path.string() + ")).key_a";

    // Cold eval: records SC dep on key_a only.
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate key_b (NOT accessed by the expression).
    file.modify("key_a = \"hello\"\nkey_b = 999\nkey_c = true\n");
    invalidateFileCache(file.path);

    // Warm eval: key_a is unchanged — SC dep for key_a still passes → cache hit.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit after mutating unaccessed key_b (SC dep override)";
    }
}

// ── attrNames ─────────────────────────────────────────────────────────────────

// P-TOML-c: AttrNames Soundness — adding a key changes the key set and must
// invalidate the attrNames result.
TEST_F(EvalTraceProperty_FromTOML, FromTOML_AttrNames_Soundness)
{
    TempExtFile file("toml", kTomlBase);
    auto const expr =
        "builtins.attrNames (builtins.fromTOML (builtins.readFile "
        + file.path.string() + "))";

    // Cold eval: records #keys SC dep on the key set {key_a, key_b, key_c}.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nList) << "expected list from attrNames";
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate: add a new key — the key set changes.
    file.modify("key_a = \"hello\"\nkey_b = 42\nkey_c = true\nkey_d = \"new\"\n");
    invalidateFileCache(file.path);

    // Warm eval: #keys dep fails — must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u) << "expected cache miss after adding a key (attrNames result changes)";
    }
}

// P-TOML-d: AttrNames Precision — changing a value (no key add/remove) does
// not invalidate the attrNames result.  The #keys dep covers only the key set,
// not the values.
TEST_F(EvalTraceProperty_FromTOML, FromTOML_AttrNames_Precision)
{
    TempExtFile file("toml", kTomlBase);
    auto const expr =
        "builtins.attrNames (builtins.fromTOML (builtins.readFile "
        + file.path.string() + "))";

    // Cold eval.
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate: change key_b's value — keys are unchanged.
    file.modify("key_a = \"hello\"\nkey_b = 100\nkey_c = false\n");
    invalidateFileCache(file.path);

    // Warm eval: #keys dep hash is still valid (same key set) → cache hit.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit after value-only change (key set unchanged)";
    }
}

// ── hasAttr ───────────────────────────────────────────────────────────────────

// P-TOML-e: HasAttr Soundness — removing key_a makes hasAttr return false and
// must invalidate the cached result.
TEST_F(EvalTraceProperty_FromTOML, FromTOML_HasAttr_Soundness)
{
    TempExtFile file("toml", kTomlBase);
    auto const expr =
        "builtins.hasAttr \"key_a\" (builtins.fromTOML (builtins.readFile "
        + file.path.string() + "))";

    // Cold eval: records #has:key_a dep, result is true.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nBool) << "expected bool from hasAttr";
        EXPECT_TRUE(v.boolean()) << "expected true initially";
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate: remove key_a — presence changes from true to false.
    file.modify("key_b = 42\nkey_c = true\n");
    invalidateFileCache(file.path);

    // Warm eval: #has:key_a dep fails (key removed) — must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u) << "expected cache miss after removing key_a (hasAttr result changes)";
    }
}

// P-TOML-f: HasAttr Precision — changing key_b's value does not affect whether
// key_a is present.  The #has:key_a dep does not cover key_b's value.
TEST_F(EvalTraceProperty_FromTOML, FromTOML_HasAttr_Precision)
{
    TempExtFile file("toml", kTomlBase);
    auto const expr =
        "builtins.hasAttr \"key_a\" (builtins.fromTOML (builtins.readFile "
        + file.path.string() + "))";

    // Cold eval: records #has:key_a dep, result is true.
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Warm hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit before mutation";
    }

    // Mutate key_b's value — key_a is still present.
    file.modify("key_a = \"hello\"\nkey_b = 777\nkey_c = true\n");
    invalidateFileCache(file.path);

    // Warm eval: key_a still present → #has:key_a dep still passes → cache hit.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly()) << "expected cache hit after mutating key_b value (key_a still present)";
    }
}

} // namespace nix::eval_trace::proptest
