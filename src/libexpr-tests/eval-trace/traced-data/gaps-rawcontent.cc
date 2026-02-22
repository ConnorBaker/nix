#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Gap 4 (FIXED): Raw + parsed readFile — RawContent dep ───────────
// Fix: stringLength (and other raw-observing string builtins) now record a
// RawContent dep (DepKind::Normal, not overrideable) via maybeRecordRawContentDep.
// This prevents StructuredContent two-level override from covering raw observations.

TEST_F(TracedDataTest, TracedJSON_RawAndParsedReadFile_ContentChanges)
{
    // [FIXED] Two readFile calls to same file: one raw (stringLength), one parsed
    // (fromJSON). Change "extra" field (.name unchanged) → must re-evaluate because
    // stringLength recorded a RawContent dep that is Normal (not overrideable).
    TempJsonFile file(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let raw = builtins.readFile )" + file.path.string()
        + R"(; j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in toString (builtins.stringLength raw) + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_TRUE(std::string(str).find("-foo") != std::string::npos);
    }

    // Change extra (name stays same, file size changes)
    file.modify(R"({"name":"foo","extra":"longer!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // raw stringLength changed → must re-evaluate
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_TRUE(std::string(str).find("-foo") != std::string::npos);
    }
}

TEST_F(TracedDataTest, RawContent_PureFromJSON_OverrideStillWorks)
{
    // Pure fromJSON (no raw observation) — SC override should still work.
    // No RawContent dep recorded since no raw-observing builtin touches the string.
    TempJsonFile file(R"({"x": 1, "y": "hello"})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"()).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Change unrelated value y — SC(.x) passes → cache hit
    file.modify(R"({"x": 1, "y": "world"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(TracedDataTest, RawContent_PureRawReadFile_AlwaysReEval)
{
    // Pure raw readFile (stringLength only, no fromJSON) — any byte change re-evaluates.
    TempJsonFile file(R"({"data": "short"})");
    auto expr = R"(builtins.stringLength (builtins.readFile )" + file.path.string()
        + R"())";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nInt);
    }

    file.modify(R"({"data": "longer!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // RawContent dep is Normal → re-eval
    }
}

TEST_F(TracedDataTest, RawContent_DoubleFromJSON_BothStructural)
{
    // Two fromJSON calls on same file, both structural → no RawContent recorded.
    // Change unrelated value → cache hit (both SC deps pass).
    TempJsonFile file(R"({"x": 1, "y": 2, "z": 3})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in j.x + j.y)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    // Change z (unrelated to x, y) → SC override applies
    file.modify(R"({"x": 1, "y": 2, "z": 999})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(TracedDataTest, RawContent_SingleReadFileBothWays)
{
    // Single readFile result used both raw (stringLength) and parsed (fromJSON).
    // RawContent dep from stringLength prevents SC override.
    TempJsonFile file(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let s = builtins.readFile )" + file.path.string()
        + R"(; in toString (builtins.stringLength s) + "-" + (builtins.fromJSON s).name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_TRUE(std::string(str).find("-foo") != std::string::npos);
    }

    // Change extra (name stays same, file size changes)
    file.modify(R"({"name":"foo","extra":"longer!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // RawContent blocks override
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_TRUE(std::string(str).find("-foo") != std::string::npos);
    }
}

} // namespace nix::eval_trace
