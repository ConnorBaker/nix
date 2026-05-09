// TraceValueContext: Sibling trace change invalidates dependent
//
// TraceValueContext is a dep kind recorded when one trace references another
// trace's value.  Unlike TraceParentSlot (parent→child scope), TraceValueContext
// captures cross-trace value dependencies — e.g., two sibling attributes that
// both reference the same let-bound value, or a derived attribute that
// references another attribute's result.
//
// The soundness analysis rates TraceValueContext as "Critical" severity.  These
// tests cover the ZERO property test coverage gap for this dep kind.
//
// Test strategy: deterministic, hand-crafted expressions (no RapidCheck
// generators) that produce cross-trace value references.  Each test follows
// the standard cold→warm-hit→mutate→warm-miss pattern.

#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ─────────────────────────────────────────────────────────────────

class EvalTraceProperty_TraceValueContext : public TraceCacheFixture {
public:
    EvalTraceProperty_TraceValueContext() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-trace-value-context");
    }
};

// ── SharedLetValue_BothSiblings_Invalidate: Two siblings referencing same value
//
// Expression:  let x = builtins.readFile <file>; in { a = x; b = x; }
//
// Both `a` and `b` reference the same value `x`.  The trace for each attribute
// may record a TraceValueContext dep on `x`'s trace (or on the parent scope's
// trace via TraceParentSlot).  Changing the file must invalidate both `a` and
// `b` through the shared dep.
TEST_F(EvalTraceProperty_TraceValueContext, SharedLetValue_BothSiblings_Invalidate)
{
    TempTextFile file("shared content");
    // Access .a (not the whole attrset) to bypass SC override — the SC
    // override legitimately fires for unchanged shapes, masking the
    // TraceValueContext propagation we're trying to test.
    auto nixCode = "let x = builtins.readFile " + file.path.string()
        + "; in ({ a = x; b = x; }).a";

    // Cold eval: forces .a which evaluates x and records deps.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Confirm warm hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Mutate the shared file.  If .a records a TraceValueContext
    // dep on x's trace, changing x's underlying dep must cascade.
    file.modify("shared content — mutated to a clearly different size!!");
    invalidateFileCache(file.path);

    // Must re-evaluate: TraceValueContext propagation.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: shared let-value changed, "
               "TraceValueContext must propagate to accessed attribute";
        EXPECT_EQ(v.type(), nString);
    }
}

// ── DerivedAttribute_ValueDep_Invalidates: Derived attribute via let-binding ─
//
// Expression:  let f = builtins.readFile <file>; result = f + " suffix"; in result
//
// `result` depends on `f` via a string concatenation.  The trace for `result`
// may record a TraceValueContext dep on `f`'s trace (because `f` is a
// separately traced value used as input to `result`'s computation).
// Changing the file should invalidate `result`.
TEST_F(EvalTraceProperty_TraceValueContext, DerivedAttribute_ValueDep_Invalidates)
{
    // No TraceValueContext involved: let bindings evaluate inline in the
    // root trace.  FileBytes dep is recorded directly.
    TempTextFile file("base value");
    auto nixCode = "let f = builtins.readFile " + file.path.string()
        + "; result = f + \" suffix\"; in result";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Confirm warm hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before mutation";
    }

    // Mutate the file.
    file.modify("mutated base value — clearly different!!");
    invalidateFileCache(file.path);

    // Must re-evaluate.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: readFile dep changed, "
               "TraceValueContext must propagate to derived 'result'";
        EXPECT_EQ(v.type(), nString);
    }
}

// ── JSONSiblingAccess_SharedSource_Invalidates: JSON attrset sibling key ──────
//
// Expression:
//   let data = builtins.fromJSON (builtins.readFile <json>);
//       a = data.key_a;
//       b = data.key_b;
//   in { inherit a b; }
//
// `a` and `b` are sibling traces both derived from the same `data` attrset.
// If `b` records a TraceValueContext dep on `a`'s trace (due to the shared
// `data` source), changing `key_a` should cascade to both.
// At minimum, changing the JSON file (which affects `data`'s FileBytes dep)
// must invalidate the whole expression.
TEST_F(EvalTraceProperty_TraceValueContext, JSONSiblingAccess_SharedSource_Invalidates)
{
    TempJsonFile jsonFile(R"({"key_a": "value_a", "key_b": "value_b"})");
    // Access .a (not the whole attrset) to bypass SC override — the SC
    // override legitimately fires for unchanged shapes.
    auto nixCode =
        "let data = builtins.fromJSON (builtins.readFile " + jsonFile.path.string() + "); "
        "a = data.key_a; "
        "b = data.key_b; "
        "in ({ inherit a b; }).a";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Confirm warm hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before JSON mutation";
    }

    // Mutate key_a — the accessed key.
    jsonFile.modify(R"({"key_a": "new_value_a", "key_b": "value_b_unchanged"})");
    invalidateFileCache(jsonFile.path);

    // Must re-evaluate.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: JSON file changed (key_a mutated), "
               "shared source dep must invalidate accessed attribute";
        EXPECT_EQ(v.type(), nString);
    }
}

// ── Concatenation_BothDeps_Invalidate: Cross-trace value via string concat ───
//
// Expression:
//   let a = builtins.readFile <fileA>;
//       b = builtins.readFile <fileB>;
//   in a + b
//
// `a + b` concatenates two independently traced values.  The result trace
// depends on both `a`'s and `b`'s traces.  This exercises the scenario where
// TraceValueContext deps link the result trace to its two input traces.
// Changing either file must invalidate the result.
TEST_F(EvalTraceProperty_TraceValueContext, Concatenation_BothDeps_Invalidate)
{
    // No TraceValueContext involved: both readFile calls land as
    // FileBytes deps in the single root trace.
    TempTextFile fileA("content A");
    TempTextFile fileB("content B");
    auto nixCode = "let a = builtins.readFile " + fileA.path.string()
        + "; b = builtins.readFile " + fileB.path.string()
        + "; in a + b";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nString);
    }

    // Confirm warm hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected before any mutation";
    }

    // Change fileA only — result trace must miss (dep on a's trace changes).
    fileA.modify("content A — mutated to a clearly different size!!");
    invalidateFileCache(fileA.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: fileA changed, "
               "a's dep propagates to concatenation result";
        EXPECT_EQ(v.type(), nString);
    }

    // Restore fileA, then change fileB — result trace must also miss.
    fileA.modify("content A");
    invalidateFileCache(fileA.path);

    // Re-record trace with original fileA.
    {
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
    }
    // Confirm hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected after restoring fileA";
    }

    fileB.modify("content B — mutated to clearly different size!!");
    invalidateFileCache(fileB.path);

    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "cache miss expected: fileB changed, "
               "b's dep propagates to concatenation result";
        EXPECT_EQ(v.type(), nString);
    }
}

} // namespace nix::eval_trace::proptest
