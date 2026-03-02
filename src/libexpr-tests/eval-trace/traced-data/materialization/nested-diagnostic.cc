/**
 * Diagnostic test for nested cross-scope cache invalidation.
 * Dumps the exact deps stored for each TracedExpr to identify
 * why d\0inner's verify incorrectly passes after file modification.
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── Baseline: non-cached Nix eval always gets fresh results ──

TEST_F(MaterializationDepTest, Nested_NoCacheBaseline)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
    auto expr = std::format("builtins.attrNames ({}).inner", fj(f.path));

    auto v1 = eval(expr);
    ASSERT_EQ(v1.listSize(), 1u);

    f.modify(R"({"inner":{"x":1,"y":2}})");
    invalidateFileCache(f.path);

    auto v2 = eval(expr);
    EXPECT_EQ(v2.listSize(), 2u) << "Baseline: pure Nix eval should see new file";
}

// ── Dump deps for every attr path after first evaluation ──

TEST_F(MaterializationDepTest, Nested_DumpDepsAfterFirstEval)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.inner; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        // Force both children to populate all traces
        auto * dAttr = root.attrs()->get(state.symbols.create("d"));
        ASSERT_NE(dAttr, nullptr);
        state.forceValue(*dAttr->value, noPos);
        // Force d.inner too
        if (dAttr->value->type() == nAttrs) {
            auto * innerAttr = dAttr->value->attrs()->get(state.symbols.create("inner"));
            if (innerAttr) state.forceValue(*innerAttr->value, noPos);
        }
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    // Dump deps for every attr path
    auto dumpPath = [&](const std::string & label, const std::string & attrPath) {
        auto deps = getStoredDeps(attrPath);
        std::string msg = label + " deps (" + std::to_string(deps.size()) + "):\n";
        for (auto & d : deps) {
            msg += "  [" + std::string(depTypeName(d.type)) + "] src=\"" + d.source + "\" key=\"";
            // Show key with non-printable chars escaped
            for (char c : d.key) {
                if (c == '\0') msg += "\\0";
                else if (c == '\t') msg += "\\t";
                else if (c == '\n') msg += "\\n";
                else msg += c;
            }
            msg += "\"\n";
        }
        // Use SCOPED_TRACE so all dumps appear regardless of pass/fail
        SCOPED_TRACE(msg);
        // Always "pass" — we just want the output
        EXPECT_TRUE(true);
    };

    dumpPath("root (\"\")", "");
    dumpPath("d", "d");
    dumpPath("d.inner", "d.inner");
    dumpPath("names", "names");
}

// ── Verify behavior for d\0inner after file change ──

TEST_F(MaterializationDepTest, Nested_DInnerVerifyAfterChange)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.inner; }}",
        fj(f.path));

    // First eval — populate all traces
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * dAttr = root.attrs()->get(state.symbols.create("d"));
        ASSERT_NE(dAttr, nullptr);
        state.forceValue(*dAttr->value, noPos);
        if (dAttr->value->type() == nAttrs) {
            auto * innerAttr = dAttr->value->attrs()->get(state.symbols.create("inner"));
            if (innerAttr) state.forceValue(*innerAttr->value, noPos);
        }
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
    }

    // Dump d\0inner deps BEFORE file change
    {
        auto deps = getStoredDeps("d.inner");
        std::string msg = "d\\0inner deps BEFORE change (" + std::to_string(deps.size()) + "):\n";
        for (auto & d : deps)
            msg += "  [" + std::string(depTypeName(d.type)) + "] src=\"" + d.source
                + "\" key=\"" + d.key + "\"\n";
        SCOPED_TRACE(msg);
        EXPECT_GT(deps.size(), 0u) << "d\\0inner should have deps stored";
    }

    f.modify(R"({"inner":{"x":1,"y":2}})");
    invalidateFileCache(f.path);

    // Check d\0inner verify AFTER change — should FAIL because
    // standalone ImplicitShape(inner#keys) detects the key set change
    {
        auto db = makeQueryDb();
        auto result = db.verify(pathFromDotted("d.inner"), {}, state);
        EXPECT_FALSE(result.has_value())
            << "d.inner verify should fail after nested keys changed";
    }

    // Also check d's verify (should still pass — top-level keys unchanged)
    {
        auto db = makeQueryDb();
        auto result = db.verify(pathFromDotted("d"), {}, state);
        EXPECT_TRUE(result.has_value()) << "d verify should pass (top-level keys unchanged)";
    }
}

// ── Manual force through cache to observe stale behavior ──

TEST_F(MaterializationDepTest, Nested_SecondEvalManualForce)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.inner; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        ASSERT_EQ(namesAttr->value->listSize(), 1u);
    }

    f.modify(R"({"inner":{"x":1,"y":2}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");

        auto * dAttr = root.attrs()->get(state.symbols.create("d"));
        ASSERT_NE(dAttr, nullptr);
        state.forceValue(*dAttr->value, noPos);
        ASSERT_EQ(dAttr->value->type(), nAttrs);

        auto * innerAttr = dAttr->value->attrs()->get(state.symbols.create("inner"));
        ASSERT_NE(innerAttr, nullptr);
        state.forceValue(*innerAttr->value, noPos);
        ASSERT_EQ(innerAttr->value->type(), nAttrs);

        size_t innerKeyCount = 0;
        for (auto & attr : *innerAttr->value->attrs()) { (void)attr; innerKeyCount++; }
        EXPECT_EQ(innerKeyCount, 2u)
            << "d.inner should have 2 keys after file change";

        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        EXPECT_EQ(namesAttr->value->listSize(), 2u)
            << "names should have 2 elements after file change";
    }
}

// ── Full reproduction ──

TEST_F(MaterializationDepTest, Nested_CrossScope_CacheMiss_Full)
{
    TempJsonFile f(R"({"inner":{"x":1}})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d.inner; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        ASSERT_EQ(namesAttr->value->listSize(), 1u);
    }

    f.modify(R"({"inner":{"x":1,"y":2}})");
    invalidateFileCache(f.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * namesAttr = root.attrs()->get(state.symbols.create("names"));
        ASSERT_NE(namesAttr, nullptr);
        state.forceValue(*namesAttr->value, noPos);
        EXPECT_EQ(namesAttr->value->listSize(), 2u)
            << "Nested cross-scope: should have 2 keys after file change";
    }
}

} // namespace nix::eval_trace::test
