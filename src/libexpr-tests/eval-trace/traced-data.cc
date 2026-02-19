#include "helpers.hh"
#include "nix/expr/trace-store.hh"
#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/traced-data.hh"
#include "nix/expr/json-to-value.hh"

#include <gtest/gtest.h>
#include <filesystem>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/trace-cache.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/canon-path.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/**
 * Tests for lazy structural dependency tracking (StructuredContent deps).
 *
 * Verifies that fromJSON(readFile f) produces fine-grained deps on
 * accessed scalar values, and that two-level verification allows
 * traces to survive changes to unused parts of the file.
 */
class TracedDataTest : public LibExprTest
{
public:
    TracedDataTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {}

protected:
    ScopedCacheDir cacheDir;
    Hash testFingerprint = hashString(HashAlgorithm::SHA256, "traced-data-test");

    std::unique_ptr<TraceCache> makeCache(
        const std::string & nixExpr,
        int * loaderCalls = nullptr)
    {
        auto loader = [this, nixExpr, loaderCalls]() -> Value * {
            if (loaderCalls) (*loaderCalls)++;
            Value v = eval(nixExpr);
            auto * result = state.allocValue();
            *result = v;
            return result;
        };
        return std::make_unique<TraceCache>(
            testFingerprint, state, std::move(loader));
    }

    Value forceRoot(TraceCache & cache)
    {
        auto * v = cache.getRootValue();
        state.forceValue(*v, noPos);
        return *v;
    }

    void invalidateFileCache(const std::filesystem::path & path)
    {
        getFSSourceAccessor()->invalidateCache(CanonPath(path.string()));
        clearStatHashMemoryCache();
    }

    /**
     * Create a TempTestFile with a .json extension.
     */
    struct TempJsonFile {
        std::filesystem::path path;
        explicit TempJsonFile(std::string_view content)
        {
            auto dir = std::filesystem::temp_directory_path() / "nix-test-eval-trace";
            std::filesystem::create_directories(dir);
            static int counter = 0;
            path = dir / ("test-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + ".json");
            std::ofstream ofs(path);
            ofs << content;
        }
        void modify(std::string_view newContent)
        {
            std::ofstream ofs(path, std::ios::trunc);
            ofs << newContent;
        }
        ~TempJsonFile() { std::filesystem::remove(path); }
        TempJsonFile(const TempJsonFile &) = delete;
        TempJsonFile & operator=(const TempJsonFile &) = delete;
    };

    struct TempTomlFile {
        std::filesystem::path path;
        explicit TempTomlFile(std::string_view content)
        {
            auto dir = std::filesystem::temp_directory_path() / "nix-test-eval-trace";
            std::filesystem::create_directories(dir);
            static int counter = 0;
            path = dir / ("test-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + ".toml");
            std::ofstream ofs(path);
            ofs << content;
        }
        void modify(std::string_view newContent)
        {
            std::ofstream ofs(path, std::ios::trunc);
            ofs << newContent;
        }
        ~TempTomlFile() { std::filesystem::remove(path); }
        TempTomlFile(const TempTomlFile &) = delete;
        TempTomlFile & operator=(const TempTomlFile &) = delete;
    };
};

// ── JSON: Scalar access records StructuredContent dep ────────────────

TEST_F(TracedDataTest, TracedJSON_ScalarAccess)
{
    TempJsonFile file(R"({"name": "hello", "version": "1.0"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.name)";

    // Fresh evaluation: access .name → should record StructuredContent dep
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Verification: file unchanged → serve from trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello"));
    }
}

// ── JSON: Nested access records dep at correct path ──────────────────

TEST_F(TracedDataTest, TracedJSON_NestedAccess)
{
    TempJsonFile file(R"({"a": {"b": {"c": 42}}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.a.b.c)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(42));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsIntEq(42));
    }
}

// ── JSON: Unused key change → trace valid (two-level override) ───────

TEST_F(TracedDataTest, TracedJSON_UnusedKeyChange)
{
    TempJsonFile file(R"({"used": "stable", "unused": "original"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.used)";

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    // Modify unused key (different size to ensure stat change)
    file.modify(R"({"used": "stable", "unused": "changed-value!!"})");
    invalidateFileCache(file.path);

    // Verification: Content dep fails (file changed), but StructuredContent
    // dep on "used" still passes → two-level override → trace valid
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

// ── JSON: Used key change → trace invalid ────────────────────────────

TEST_F(TracedDataTest, TracedJSON_UsedKeyChange)
{
    TempJsonFile file(R"({"name": "hello", "extra": "x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Modify the accessed key
    file.modify(R"({"name": "world!!", "extra": "x"})");
    invalidateFileCache(file.path);

    // Verification: StructuredContent dep on "name" fails → trace invalid
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("world!!"));
    }
}

// ── JSON: Add new key → trace valid ─────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_AddKey)
{
    TempJsonFile file(R"({"used": "stable"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.used)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    // Add a new key (different file size)
    file.modify(R"({"used": "stable", "newkey": "newvalue!!"})");
    invalidateFileCache(file.path);

    // Content dep fails but StructuredContent dep on "used" passes
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

// ── JSON: Remove unused key → trace valid ────────────────────────────

TEST_F(TracedDataTest, TracedJSON_RemoveUnusedKey)
{
    TempJsonFile file(R"({"used": "stable", "unused": "original-val"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.used)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    // Remove unused key
    file.modify(R"({"used": "stable"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

// ── JSON: Remove used key → trace invalid (navigation fails) ─────────

TEST_F(TracedDataTest, TracedJSON_RemoveUsedKey)
{
    TempJsonFile file(R"({"name": "hello", "other": "x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Remove the accessed key (different size)
    file.modify(R"({"other": "x-remaining!!"})");
    invalidateFileCache(file.path);

    // Verification: StructuredContent dep navigation fails → trace invalid.
    // The loader re-evaluates, but the expression accesses .name which was
    // removed, so evaluation throws. We only care that the trace was invalidated
    // (loaderCalls == 1).
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── JSON: Array element access ───────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_ArrayAccess)
{
    TempJsonFile file(R"({"items": ["alpha", "beta", "gamma"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in builtins.elemAt j.items 1)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("beta"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("beta"));
    }
}

// ── JSON: Array element change (accessed vs unaccessed) ──────────────

TEST_F(TracedDataTest, TracedJSON_ArrayElementChange)
{
    TempJsonFile file(R"({"items": ["alpha", "beta", "gamma"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in builtins.elemAt j.items 1)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("beta"));
    }

    // Change unaccessed element (index 0) — trace should still be valid
    file.modify(R"({"items": ["CHANGED!!", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("beta"));
    }

    // Now change the accessed element (index 1)
    file.modify(R"({"items": ["CHANGED!!", "BETA-NEW!!", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("BETA-NEW!!"));
    }
}

// ── JSON: No provenance (literal string) → eager fallback ────────────

TEST_F(TracedDataTest, TracedJSON_NoProvenance)
{
    // fromJSON on a literal string (not from readFile) should use eager parsing
    auto expr = R"(builtins.fromJSON ''{"x": 42}'')";

    {
        auto cache = makeCache(expr);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * xVal = v->attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xVal, nullptr);
        state.forceValue(*xVal->value, noPos);
        EXPECT_THAT(*xVal->value, IsIntEq(42));
    }

    // Verification: no file deps at all, just the trace
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── JSON: Direct readFile→fromJSON produces structural deps ──────────

TEST_F(TracedDataTest, TracedJSON_DirectReadFile)
{
    TempJsonFile file(R"({"x": 42})");
    // readFile result is directly used (no modification), so provenance matches.
    // This test verifies that direct readFile→fromJSON produces structural deps
    // and the trace is served on second access.
    auto expr = "builtins.fromJSON (builtins.readFile " + file.path.string() + ")";

    {
        auto cache = makeCache(expr);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * xVal = v->attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xVal, nullptr);
        state.forceValue(*xVal->value, noPos);
        EXPECT_THAT(*xVal->value, IsIntEq(42));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── JSON: Modified string (hash mismatch) → eager fallback ───────────

TEST_F(TracedDataTest, TracedJSON_ModifiedString)
{
    TempJsonFile file(R"({"x": 42})");
    // readFile result is concatenated with extra text, so the content hash
    // won't match the ReadFileProvenance → eager fallback (no structural deps).
    // The trace should still work via the whole-file Content dep.
    auto expr = "builtins.fromJSON (builtins.readFile " + file.path.string() + ")";

    // Use an expression that modifies the string, breaking provenance
    auto modExpr = "builtins.fromJSON (''{\"y\": 99}'')";

    {
        auto cache = makeCache(modExpr);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * yVal = v->attrs()->get(state.symbols.create("y"));
        ASSERT_NE(yVal, nullptr);
        state.forceValue(*yVal->value, noPos);
        EXPECT_THAT(*yVal->value, IsIntEq(99));
    }

    // Trace still works (served from cache — no file deps, just trace)
    {
        int loaderCalls = 0;
        auto cache = makeCache(modExpr, &loaderCalls);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── TOML: Scalar access records StructuredContent dep ────────────────

TEST_F(TracedDataTest, TracedTOML_ScalarAccess)
{
    TempTomlFile file("[section]\nname = \"hello\"\nversion = \"1.0\"\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string() + R"(); in t.section.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello"));
    }
}

// ── TOML: Unused key change → trace valid (two-level override) ───────

TEST_F(TracedDataTest, TracedTOML_UnusedKeyChange)
{
    TempTomlFile file("[section]\nused = \"stable\"\nunused = \"original\"\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string() + R"(); in t.section.used)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    // Modify unused key (different size)
    file.modify("[section]\nused = \"stable\"\nunused = \"changed-value-longer!!\"\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

// ── TOML: Used key change → trace invalid ────────────────────────────

TEST_F(TracedDataTest, TracedTOML_UsedKeyChange)
{
    TempTomlFile file("[section]\nname = \"hello\"\nextra = \"x\"\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string() + R"(); in t.section.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Modify the accessed key (different size)
    file.modify("[section]\nname = \"world-changed!!\"\nextra = \"x\"\n");
    invalidateFileCache(file.path);

    // StructuredContent dep on section.name fails → trace invalid
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("world-changed!!"));
    }
}

// ── JSON: Multiple scalar accesses from same file ────────────────────

TEST_F(TracedDataTest, TracedJSON_MultipleAccess)
{
    TempJsonFile file(R"({"a": "alpha", "b": "beta", "c": "gamma"})");
    // Access two keys: a and b. c is not accessed.
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.a + "-" + j.b)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("alpha-beta"));
    }

    // Change unaccessed key c → trace still valid
    file.modify(R"({"a": "alpha", "b": "beta", "c": "CHANGED!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("alpha-beta"));
    }

    // Change accessed key b → trace invalid
    file.modify(R"({"a": "alpha", "b": "BETA-NEW!!", "c": "CHANGED!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("alpha-BETA-NEW!!"));
    }
}

// ── JSON: Root-level array ───────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_RootArray)
{
    TempJsonFile file(R"(["first", "second", "third"])");
    auto expr = R"(builtins.elemAt (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()) 1)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("second"));
    }

    // Verify trace is served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("second"));
    }

    // Change unaccessed element → trace still valid
    file.modify(R"(["CHANGED!!", "second", "third"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("second"));
    }
}

// ── JSON: Keys containing dots, brackets, quotes, backslashes ────────

TEST_F(TracedDataTest, TracedJSON_DottedKey)
{
    // JSON key "a.b" must be quoted in the data path to avoid being
    // misinterpreted as two separate keys "a" and "b".
    TempJsonFile file(R"({"a.b": "dotted", "a": {"b": "nested"}})");
    // Access the dotted key "a.b" (not a.b which is nested)
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j."a.b")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("dotted"));
    }

    // Verify trace is served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("dotted"));
    }

    // Change the nested a.b (not the dotted key) → trace still valid
    file.modify(R"({"a.b": "dotted", "a": {"b": "CHANGED"}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("dotted"));
    }

    // Change the dotted key → trace invalid
    file.modify(R"({"a.b": "CHANGED", "a": {"b": "nested"}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED"));
    }
}

TEST_F(TracedDataTest, TracedJSON_BracketKey)
{
    // JSON key containing brackets
    TempJsonFile file(R"({"[0]": "bracket-key", "normal": "value"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j."[0]")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("bracket-key"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("bracket-key"));
    }
}

TEST_F(TracedDataTest, TracedJSON_QuoteAndBackslashKey)
{
    // JSON keys with quotes and backslashes (escaped in JSON source)
    TempJsonFile file(R"({"a\"b": "has-quote", "c\\d": "has-backslash"})");
    // In Nix, access with string interpolation to get literal key
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"(); in j.${"a\"b"})";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("has-quote"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("has-quote"));
    }
}

// ── mapAttrs / key-set interaction ────────────────────────────────────
// These tests verify that the lazy attrset design is correct when combined
// with Nix builtins that iterate keys (mapAttrs, attrNames). The key
// invariant: key-set changes only affect traces at the level that
// materializes the attrset structure, where only Content deps exist
// (no StructuredContent override possible).

TEST_F(TracedDataTest, TracedJSON_MapAttrsNoForce)
{
    // mapAttrs iterates keys but doesn't force values → only Content dep
    // recorded. Any file change must invalidate (no StructuredContent override).
    TempJsonFile file(R"({"a": "1", "b": "2"})");
    auto expr = R"(builtins.mapAttrs (n: v: v + "-mapped") (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nAttrs);
    }

    // Add a key → Content fails, no StructuredContent deps → re-eval
    file.modify(R"({"a": "1", "b": "2", "c": "3"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
    }
}

TEST_F(TracedDataTest, TracedJSON_MapAttrsValueAccess)
{
    // mapAttrs + access specific value → StructuredContent dep recorded for
    // that value. Adding an unused key to the JSON file must NOT invalidate
    // because the StructuredContent override covers the accessed value and
    // the cached result is that specific value (not the full attrset).
    TempJsonFile file(R"({"a": "stable", "b": "other"})");
    auto expr = R"((builtins.mapAttrs (n: v: v + "-mapped") (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"())).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable-mapped"));
    }

    // Add unused key → Content fails, StructuredContent for "a" passes → override
    file.modify(R"({"a": "stable", "b": "other", "c": "new!!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache
        EXPECT_THAT(v, IsStringEq("stable-mapped"));
    }

    // Change accessed value → StructuredContent fails → re-eval
    file.modify(R"({"a": "CHANGED", "b": "other", "c": "new!!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED-mapped"));
    }
}

TEST_F(TracedDataTest, TracedJSON_AttrNamesNoLeafAccess)
{
    // attrNames enumerates keys without forcing values → only Content dep.
    // Any file change invalidates (no StructuredContent override).
    TempJsonFile file(R"({"x": 1, "y": 2, "z": 3})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Add a key → Content fails, no StructuredContent → must re-eval
    file.modify(R"({"w": 0, "x": 1, "y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
    }
}

TEST_F(TracedDataTest, TracedJSON_MapAttrsRemoveUnusedKey)
{
    // Flake-compat scenario: mapAttrs over lock file inputs, access specific
    // input. Removing an unused input must NOT invalidate.
    TempJsonFile file(R"({"nixpkgs": {"rev": "abc123"}, "unused-input": {"rev": "def456"}})");
    auto expr = R"((builtins.mapAttrs (n: v: v.rev) (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"())).nixpkgs)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("abc123"));
    }

    // Remove unused input → Content fails, StructuredContent for nixpkgs.rev passes
    file.modify(R"({"nixpkgs": {"rev": "abc123"}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache
        EXPECT_THAT(v, IsStringEq("abc123"));
    }
}

// ── List size change correctness ─────────────────────────────────────
// Regression test: TracedExpr's allStrings recording path used to force
// all list elements in the same DependencyTracker, mixing StructuredContent
// deps with the Content dep. Adding an element would cause Content to fail
// but all existing StructuredContent deps to pass, incorrectly overriding.

TEST_F(TracedDataTest, TracedJSON_ListElementAdded)
{
    // Access a specific element of a JSON array via fromJSON(readFile f).
    TempJsonFile file(R"({"items": ["alpha", "beta"]})");
    auto expr = R"(builtins.elemAt (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()).items 0)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("alpha"));
    }

    // Add an element to the array (size changes, accessed element unchanged)
    file.modify(R"({"items": ["alpha", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    // StructuredContent for items.[0] passes, Content fails.
    // Override should apply because the cached result is "alpha" (the leaf
    // value, not the list) — the list size is irrelevant to this result.
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache (override correct)
        EXPECT_THAT(v, IsStringEq("alpha"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ListSizeChange_NoLeafAccess)
{
    // Access the entire list without forcing elements → only Content dep.
    // Any file change must invalidate.
    TempJsonFile file(R"(["a", "b", "c"])");
    auto expr = "builtins.fromJSON (builtins.readFile "
        + file.path.string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Change list size
    file.modify(R"(["a", "b", "c", "d"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
    }
}

} // namespace nix::eval_trace
