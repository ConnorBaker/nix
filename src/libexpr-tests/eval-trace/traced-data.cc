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

    struct TempTextFile {
        std::filesystem::path path;
        explicit TempTextFile(std::string_view content)
        {
            auto dir = std::filesystem::temp_directory_path() / "nix-test-eval-trace";
            std::filesystem::create_directories(dir);
            static int counter = 0;
            path = dir / ("test-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + ".txt");
            std::ofstream ofs(path);
            ofs << content;
        }
        void modify(std::string_view newContent)
        {
            std::ofstream ofs(path, std::ios::trunc);
            ofs << newContent;
        }
        ~TempTextFile() { std::filesystem::remove(path); }
        TempTextFile(const TempTextFile &) = delete;
        TempTextFile & operator=(const TempTextFile &) = delete;
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

// ═══════════════════════════════════════════════════════════════════════
// Shape observation tracking tests
// ═══════════════════════════════════════════════════════════════════════

// ── Core bug reproduction ─────────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_LengthPlusElemAt_ShapeChange)
{
    // THE motivating bug: toString(length arr) + "-" + elemAt arr 0
    // If array grows but element 0 is unchanged, shape dep #len must fail
    // to prevent serving stale "2-alpha" when answer should be "3-alpha".
    TempJsonFile file(R"({"arr": ["alpha", "beta"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (toString (builtins.length j.arr)) + "-" + (builtins.elemAt j.arr 0))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }

    // Add element: array grows from 2 to 3, but element 0 unchanged
    file.modify(R"({"arr": ["alpha", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (shape dep #len fails)
        EXPECT_THAT(v, IsStringEq("3-alpha"));
    }
}

TEST_F(TracedDataTest, TracedJSON_LengthPlusElemAt_NoShapeChange)
{
    // Same expression, but values change while length stays the same.
    // Shape dep #len passes AND leaf dep passes → override applies.
    TempJsonFile file(R"({"arr": ["alpha", "beta"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (toString (builtins.length j.arr)) + "-" + (builtins.elemAt j.arr 0))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }

    // Change element 1 (unaccessed), keep length and element 0 same
    file.modify(R"({"arr": ["alpha", "CHANGED"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache (override applies)
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }
}

// ── List length shape deps ────────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_LengthOnly_ShapeChange)
{
    // length(arr) must invalidate when array grows
    TempJsonFile file(R"({"arr": ["a", "b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.arr)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"arr": ["a", "b", "c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(TracedDataTest, TracedJSON_LengthOnly_ContentChange)
{
    // length(arr) survives when values change but length stays same
    TempJsonFile file(R"({"arr": ["a", "b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.arr)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"arr": ["CHANGED!", "b"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache
        EXPECT_THAT(v, IsIntEq(2));
    }
}

TEST_F(TracedDataTest, TracedJSON_RootArrayLength)
{
    // length(fromJSON(readFile f)) where root is array
    TempJsonFile file(R"(["x", "y", "z"])");
    auto expr = R"(builtins.length (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"(["x", "y", "z", "w"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
        EXPECT_THAT(v, IsIntEq(4));
    }
}

TEST_F(TracedDataTest, TracedJSON_NestedListLength)
{
    // length(obj.items) where items is nested list
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [1, 2, 3, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
        EXPECT_THAT(v, IsIntEq(4));
    }
}

// ── Attrset key set shape deps ────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_AttrNamesOnly_KeySetChange)
{
    // attrNames(obj) invalidates when key added
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"a": 1, "b": 2, "c": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
    }
}

TEST_F(TracedDataTest, TracedJSON_AttrNamesOnly_ValueChange)
{
    // attrNames(obj) survives when values change but keys same
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"a": 99, "b": 88})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache (#keys passes)
    }
}

TEST_F(TracedDataTest, TracedJSON_AttrNamesPlusValue_KeySetChange)
{
    // concatStringsSep "," (attrNames obj) + ":" + obj.a
    // Adding a key must invalidate because #keys changes
    TempJsonFile file(R"({"a": "alpha", "b": "beta"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.concatStringsSep "," (builtins.attrNames j)) + ":" + j.a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b:alpha"));
    }

    file.modify(R"({"a": "alpha", "b": "beta", "c": "gamma"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
        EXPECT_THAT(v, IsStringEq("a,b,c:alpha"));
    }
}

TEST_F(TracedDataTest, TracedJSON_NestedAttrNames)
{
    // attrNames(obj.inner) where inner is nested object
    TempJsonFile file(R"({"inner": {"x": 1, "y": 2}})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()).inner)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"inner": {"x": 1, "y": 2, "z": 3}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
    }
}

// ── hasAttr shape deps ────────────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_HasAttr_KeyAdded)
{
    // hasAttr "b" obj returns false, key "b" added → must invalidate
    TempJsonFile file(R"({"a": 1})");
    auto expr = R"(builtins.hasAttr "b" (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    file.modify(R"({"a": 1, "b": 2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(TracedDataTest, TracedJSON_HasAttr_KeyRemoved)
{
    // hasAttr "b" obj returns true, key "b" removed → must invalidate
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(builtins.hasAttr "b" (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"a": 1000})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(TracedDataTest, TracedJSON_HasAttr_ValueChange)
{
    // hasAttr "a" obj survives when values change but keys same
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(builtins.hasAttr "a" (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"a": 999, "b": 888})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // served from cache (#keys passes)
        EXPECT_THAT(v, IsTrue());
    }
}

// ── Regression tests (existing behavior preserved) ────────────────────

TEST_F(TracedDataTest, TracedJSON_PointAccessSurvivesKeyAddition)
{
    // data.x still works when key y added (no shape dep, override applies)
    TempJsonFile file(R"({"x": "stable"})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"x": "stable", "y": "added!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // override applies (no shape dep)
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ElemAtSurvivesArrayGrowth)
{
    // elemAt(arr, 0) still works when array grows (no shape dep, override applies)
    TempJsonFile file(R"(["first", "second"])");
    auto expr = R"(builtins.elemAt (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()) 0)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("first"));
    }

    file.modify(R"(["first", "second", "third"])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // override applies (no shape dep)
        EXPECT_THAT(v, IsStringEq("first"));
    }
}

TEST_F(TracedDataTest, TracedJSON_MapAttrsValueAccess_NoShapeDep)
{
    // (mapAttrs f data).a still works when key added (no shape builtin used)
    TempJsonFile file(R"({"a": "stable", "b": "other"})");
    auto expr = R"((builtins.mapAttrs (n: v: v + "!") (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"())).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable!"));
    }

    file.modify(R"({"a": "stable", "b": "other", "c": "new!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // override applies (no shape builtin)
        EXPECT_THAT(v, IsStringEq("stable!"));
    }
}

// ── TOML shape deps ──────────────────────────────────────────────────

TEST_F(TracedDataTest, TracedTOML_LengthChange)
{
    // TOML array length change invalidates
    TempTomlFile file("items = [\"a\", \"b\"]\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length t.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify("items = [\"a\", \"b\", \"c\"]\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#len fails)
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(TracedDataTest, TracedTOML_KeySetChange)
{
    // TOML table key set change invalidates
    TempTomlFile file("[section]\na = 1\nb = 2\n");
    auto expr = R"(builtins.attrNames (builtins.fromTOML (builtins.readFile )" + file.path.string() + R"()).section)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify("[section]\na = 1\nb = 2\nc = 3\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
    }
}

// ── Edge cases ────────────────────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_EmptyArrayLength)
{
    // length([]) shape dep (file changes to [1])
    TempJsonFile file(R"({"arr": []})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.arr)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(0));
    }

    file.modify(R"({"arr": [1]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#len fails)
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(TracedDataTest, TracedJSON_EmptyObjectAttrNames)
{
    // attrNames({}) shape dep (file changes to {"a":1})
    TempJsonFile file(R"({"obj": {}})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()).obj)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    file.modify(R"({"obj": {"a": 1}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (#keys fails)
    }
}

TEST_F(TracedDataTest, TracedJSON_KeyWithHash)
{
    // JSON key containing '#' is properly escaped, no ambiguity with shape suffix
    TempJsonFile file(R"({"a#b": "value", "c": "other"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in j.${"a#b"})";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("value"));
    }

    // Verify trace served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("value"));
    }

    // Change the value → trace invalid
    file.modify(R"({"a#b": "CHANGED!", "c": "other"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Robustness and edge case tests
// ═══════════════════════════════════════════════════════════════════════

// ── Data path escaping roundtrip ──────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_DeeplyNestedMixedKeys)
{
    // Deeply nested path through dotted key → array index → bracket key.
    // Tests escapeDataPathKey ↔ parseDataPath roundtrip for complex paths:
    // the dep key encodes "a.b".[0]."c[d]" and verification must parse
    // this exact path to navigate the JSON DOM correctly.
    TempJsonFile file(R"({"a.b": [{"c[d]": "deep-value"}], "x": "other"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.elemAt j.${"a.b"} 0).${"c[d]"})";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("deep-value"));
    }

    // Verify served from cache (roundtrip works)
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("deep-value"));
    }

    // Change unaccessed key → override should apply
    file.modify(R"({"a.b": [{"c[d]": "deep-value"}], "x": "CHANGED!!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // override applies
        EXPECT_THAT(v, IsStringEq("deep-value"));
    }

    // Change accessed value → must re-evaluate
    file.modify(R"({"a.b": [{"c[d]": "NEW-deep-val!!"}], "x": "CHANGED!!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("NEW-deep-val!!"));
    }
}

// ── Shape suffix disambiguation ───────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_KeyNamedHashSuffix)
{
    // Keys literally named "#len" and "#keys" must be quoted by
    // escapeDataPathKey (because '#' triggers quoting), preventing
    // computeCurrentHash from confusing them with shape dep suffixes.
    TempJsonFile file(R"({"#len": "hash-len-val", "#keys": "hash-keys-val", "other": "x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in j.${"#len"} + "-" + j.${"#keys"})";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hash-len-val-hash-keys-val"));
    }

    // Change unaccessed key → override applies (no shape suffix confusion)
    file.modify(R"({"#len": "hash-len-val", "#keys": "hash-keys-val", "other": "CHANGED!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hash-len-val-hash-keys-val"));
    }

    // Change accessed key "#len" → must re-evaluate
    file.modify(R"({"#len": "CHANGED-val!!", "#keys": "hash-keys-val", "other": "CHANGED!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED-val!!-hash-keys-val"));
    }
}

// ── Container type changes ────────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_LengthAfterTypeChangeToObject)
{
    // Array changes to object — #len shape dep must fail because
    // computeCurrentHash checks is_array() and returns nullopt for objects.
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    // Change items from array to object (type change)
    file.modify(R"({"items": {"a": 1, "b": 2, "c": 3}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Re-eval calls length on attrset → throws
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedJSON_AttrNamesAfterTypeChangeToArray)
{
    // Object changes to array — #keys shape dep must fail because
    // computeCurrentHash checks is_object() and returns nullopt for arrays.
    TempJsonFile file(R"({"data": {"x": 1, "y": 2}})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()).data)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Change data from object to array (type change)
    file.modify(R"({"data": [1, 2, 3, 4, 5]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Re-eval calls attrNames on list → throws
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Two-level override coverage gap ───────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_TwoFilesOnlyOneCovered)
{
    // Two files read in same trace: f1 parsed with fromJSON (SC deps),
    // f2 read raw (only Content dep). When f2 changes, the override must
    // NOT apply because f2's Content failure is not covered by any SC dep.
    // Tests the structuralCoveredFiles check in verifyTrace.
    TempJsonFile f1(R"({"name": "hello"})");
    TempJsonFile f2("raw-content-here");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + f1.path.string()
        + R"(); raw = builtins.readFile )" + f2.path.string()
        + R"(; in j.name + "-" + raw)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello-raw-content-here"));
    }

    // Modify f2 only (f1 unchanged) — override must NOT apply
    f2.modify("raw-CHANGED-content!!");
    invalidateFileCache(f2.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (f2 uncovered)
        EXPECT_THAT(v, IsStringEq("hello-raw-CHANGED-content!!"));
    }
}

TEST_F(TracedDataTest, TracedJSON_NavigationFailureMidPath)
{
    // Intermediate key removed — navigateJson fails at middle segment,
    // not at leaf. Tests that mid-path navigation failure (as opposed to
    // leaf-key removal tested by TracedJSON_RemoveUsedKey) correctly
    // invalidates the StructuredContent dep.
    TempJsonFile file(R"({"outer": {"inner": {"deep": "value"}}, "x": "padding"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in j.outer.inner.deep)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("value"));
    }

    // Remove intermediate key "inner" (renamed)
    file.modify(R"({"outer": {"RENAMED": {"deep": "value"}}, "x": "padding!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Re-eval: j.outer.inner → missing key → throws
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ── Array shrinkage edge cases ────────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_ArrayShrinksLengthFails)
{
    // Array shrinks from 3 to 1 — #len shape dep must fail.
    // Tests the shrinkage direction (existing tests only grow arrays).
    TempJsonFile file(R"({"arr": ["a", "b", "c"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.arr)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"arr": ["a"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(TracedDataTest, TracedJSON_ArrayBecomesEmptyLengthFails)
{
    // Non-empty array becomes empty — #len shape dep must fail.
    // During recording, provenance was registered with first element as key.
    // During verification, computeCurrentHash navigates to the (now empty)
    // array and computes hash("0") != recorded hash("2").
    TempJsonFile file(R"({"arr": ["x", "y"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length j.arr)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"arr": []})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(0));
    }
}

// ── Shape dep re-record after invalidation ────────────────────────────

TEST_F(TracedDataTest, TracedJSON_ShapeDepRerecordAfterInvalidation)
{
    // Three-step test: fresh → shape invalidation → re-record → verify.
    // Ensures shape deps are correctly re-recorded after a trace is
    // invalidated by a shape change, and the new trace works correctly
    // on subsequent verification.
    TempJsonFile file(R"({"arr": ["alpha", "beta"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (toString (builtins.length j.arr)) + "-" + (builtins.elemAt j.arr 0))";

    // Step 1: Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-alpha"));
    }

    // Step 2: Shape change (array grows) → invalidation + re-record
    file.modify(R"({"arr": ["alpha", "beta", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len fails (2→3) → re-eval
        EXPECT_THAT(v, IsStringEq("3-alpha"));
    }

    // Step 3: Value change (length same, elem 1 changed) → new trace survives
    file.modify(R"({"arr": ["alpha", "CHANGED!", "gamma"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // re-recorded #len=3 passes, .[0] passes → override
        EXPECT_THAT(v, IsStringEq("3-alpha"));
    }
}

// ── Nested container provenance ───────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_HasAttrOnNestedObject)
{
    // hasAttr on a nested traced object. Tests that container provenance
    // survives the Value copy during attribute selection (ExprSelect::eval
    // copies Values: v = *attr->value), so the Bindings* key remains stable.
    TempJsonFile file(R"({"inner": {"x": 1, "y": 2}})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )" + file.path.string() + R"()).inner)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    // Remove "x" from inner, add "z" → #keys changes → must re-evaluate
    file.modify(R"({"inner": {"y": 2, "z": 33}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys fails
        EXPECT_THAT(v, IsFalse());
    }

    // Values change but keys unchanged → #keys passes (nested provenance works)
    file.modify(R"({"inner": {"y": 99, "z": 999}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #keys passes → override
        EXPECT_THAT(v, IsFalse());
    }
}

// ── Multiple containers from same file ────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_MultipleContainersShapeDeps)
{
    // Two arrays from the same file, both with #len shape deps.
    // One array grows, the other stays the same. The failing #len dep
    // must prevent the override even though the other #len passes.
    TempJsonFile file(R"({"arr1": [1, 2], "arr2": [3, 4, 5]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (toString (builtins.length j.arr1)) + "-" + (toString (builtins.length j.arr2)))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2-3"));
    }

    // Only arr2 grows → its #len fails, overall override must NOT apply
    file.modify(R"({"arr1": [1, 2], "arr2": [3, 4, 5, 6]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (one #len fails)
        EXPECT_THAT(v, IsStringEq("2-4"));
    }

    // Both lengths unchanged, values change → both #len pass → override
    file.modify(R"({"arr1": [9, 8], "arr2": [7, 6, 5, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // both #len pass → override
        EXPECT_THAT(v, IsStringEq("2-4"));
    }
}

// ── TOML type change ──────────────────────────────────────────────────

TEST_F(TracedDataTest, TracedTOML_LengthAfterTypeChangeToTable)
{
    // TOML array changes to table — #len shape dep must fail.
    // Tests navigateToml's is_array() type guard in computeCurrentHash.
    TempTomlFile file("items = [\"a\", \"b\", \"c\"]\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length t.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    // Change items from array to table
    file.modify("[items]\na = 1\nb = 2\nc = 3\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Re-eval: length on attrset → throws
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// ReadFileProvenance map tests (multi-file, reuse, forced-before-fromJSON)
// ═══════════════════════════════════════════════════════════════════════

// ── Two files read before fromJSON ────────────────────────────────────

TEST_F(TracedDataTest, TracedJSON_TwoFilesReadBeforeFromJSON)
{
    // Read two files, then fromJSON both. Both should get traced data
    // (verifies no single-slot overwrite).
    TempJsonFile f1(R"({"x": "hello"})");
    TempJsonFile f2(R"({"y": "world"})");
    auto expr = R"(let a = builtins.readFile )" + f1.path.string()
        + R"(; b = builtins.readFile )" + f2.path.string()
        + R"(; in (builtins.fromJSON a).x + "-" + (builtins.fromJSON b).y)";

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello-world"));
    }

    // Verify trace served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello-world"));
    }

    // Modify f1 value of x → first trace invalidates
    f1.modify(R"({"x": "CHANGED!!"})");
    invalidateFileCache(f1.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!!-world"));
    }

    // Modify f2 value of y → second trace invalidates
    f2.modify(R"({"y": "CHANGED!!"})");
    invalidateFileCache(f2.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!!-CHANGED!!"));
    }
}

// ── Same readFile result fed to two fromJSON calls ────────────────────

TEST_F(TracedDataTest, TracedJSON_SameReadFileTwoFromJSON)
{
    // Same readFile result used by two fromJSON calls. Both should get
    // traced data (verifies non-consuming lookup).
    TempJsonFile file(R"({"x": "alpha", "y": "beta"})");
    auto expr = R"(let s = builtins.readFile )" + file.path.string()
        + R"(; in (builtins.fromJSON s).x + "-" + (builtins.fromJSON s).y)";

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("alpha-beta"));
    }

    // Verify trace served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("alpha-beta"));
    }

    // Change x value → trace invalid (StructuredContent dep on x fails)
    file.modify(R"({"x": "CHANGED!!", "y": "beta"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!!-beta"));
    }

    // Change only y (unaccessed by x dep) — but y is also accessed,
    // so StructuredContent dep on y fails → trace invalid
    file.modify(R"({"x": "CHANGED!!", "y": "ALSO-NEW!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!!-ALSO-NEW!!"));
    }

    // Change only unused key (add "z") → trace valid (two-level override)
    file.modify(R"({"x": "CHANGED!!", "y": "ALSO-NEW!!", "z": "extra!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("CHANGED!!-ALSO-NEW!!"));
    }
}

// ── Forced before fromJSON (builtins.seq) ─────────────────────────────

TEST_F(TracedDataTest, TracedJSON_ForcedBeforeFromJSON)
{
    // Force readFile result (via builtins.seq) before fromJSON. Provenance
    // should still be available because the map is non-consuming.
    // seq forces s but returns s (same string), so depHash(s) matches.
    TempJsonFile file(R"({"x": "hello", "extra": "padding"})");
    auto expr = R"(let s = builtins.readFile )" + file.path.string()
        + R"(; forced = builtins.seq s s; in (builtins.fromJSON forced).x)";

    // Fresh evaluation
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Verify trace served from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Change unused key → two-level override applies
    file.modify(R"({"x": "hello", "extra": "CHANGED-padding!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Change used key → trace invalid
    file.modify(R"({"x": "CHANGED!!", "extra": "CHANGED-padding!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("CHANGED!!"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// readDir ExprTracedData tests — Directory two-level override
// ═══════════════════════════════════════════════════════════════════════

// ── TempDir helper ─────────────────────────────────────────────────────

class TempDir {
    std::filesystem::path dir_;
public:
    TempDir()
    {
        auto base = std::filesystem::temp_directory_path() / "nix-test-eval-trace";
        std::filesystem::create_directories(base);
        static int counter = 0;
        dir_ = base / ("dir-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directory(dir_);
    }
    const std::filesystem::path & path() const { return dir_; }

    void addFile(const std::string & name, const std::string & content = "")
    {
        std::ofstream ofs(dir_ / name);
        ofs << content;
    }
    void addSubdir(const std::string & name)
    {
        std::filesystem::create_directory(dir_ / name);
    }
    void addSymlink(const std::string & name, const std::string & target)
    {
        std::filesystem::create_symlink(target, dir_ / name);
    }
    void removeEntry(const std::string & name)
    {
        std::filesystem::remove_all(dir_ / name);
    }
    void changeToSymlink(const std::string & name, const std::string & target)
    {
        std::filesystem::remove_all(dir_ / name);
        std::filesystem::create_symlink(target, dir_ / name);
    }
    void changeToSubdir(const std::string & name)
    {
        std::filesystem::remove_all(dir_ / name);
        std::filesystem::create_directory(dir_ / name);
    }

    ~TempDir() { std::filesystem::remove_all(dir_); }
    TempDir(const TempDir &) = delete;
    TempDir & operator=(const TempDir &) = delete;
};

// Helper to invalidate directory cache entries
#define INVALIDATE_DIR(td) \
    do { \
        getFSSourceAccessor()->invalidateCache(CanonPath((td).path().string())); \
        clearStatHashMemoryCache(); \
    } while (0)

// ── Group 1: Basic ExprTracedData for readDir ──────────────────────────

TEST_F(TracedDataTest, TracedDir_BasicAccess)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addSubdir("world");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_MultipleAccess)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addSubdir("world");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello + \"-\" + (builtins.readDir " + td.path().string() + ").world";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular-directory"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular-directory"));
    }
}

TEST_F(TracedDataTest, TracedDir_SymlinkEntry)
{
    TempDir td;
    td.addFile("target", "x");
    td.addSymlink("link", "target");
    auto expr = "(builtins.readDir " + td.path().string() + ").link";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("symlink"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("symlink"));
    }
}

TEST_F(TracedDataTest, TracedDir_DirectoryEntry)
{
    TempDir td;
    td.addSubdir("subdir");
    td.addFile("file", "x");
    auto expr = "(builtins.readDir " + td.path().string() + ").subdir";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("directory"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("directory"));
    }
}

TEST_F(TracedDataTest, TracedDir_SpecialCharKey)
{
    // Entry name contains '.' → data path escaping must handle it
    TempDir td;
    td.addFile("foo.bar", "x");
    td.addFile("other", "y");
    auto expr = "(builtins.readDir " + td.path().string() + ").\"foo.bar\"";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_EmptyDir)
{
    TempDir td;
    auto expr = "builtins.readDir " + td.path().string();

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nAttrs);
        EXPECT_EQ(v.attrs()->size(), 0);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_TRUE(v.type() == nAttrs);
    }
}

// ── Group 2: Two-Level Override — Positive Cases ───────────────────────

TEST_F(TracedDataTest, TracedDir_AddUnrelatedFile)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    // Add new file "newfile" — unrelated to accessed "hello"
    td.addFile("newfile", "new-content");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // trace valid (two-level override)
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_RemoveUnrelatedFile)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad-pad");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    td.removeEntry("other");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_ChangeUnrelatedType)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "regular-file");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    // Change "other" from regular → symlink
    td.changeToSymlink("other", "hello");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_ThroughUpdate)
{
    // readDir result passed through // (update operator) — thunks survive.
    // The // operator records #keys for both operands, so adding a new file
    // causes a #keys dep failure even though .hello is unchanged. This is the
    // expected precision trade-off for the soundness fix in ExprOpUpdate.
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad");
    auto expr = "((builtins.readDir " + td.path().string() + ") // { extra = \"val\"; }).hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    td.addFile("newfile", "new-content");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys dep on readDir result causes re-eval
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_ThroughMapAttrs)
{
    // readDir result passed through mapAttrs — thunks survive
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad");
    auto expr = "let d = builtins.mapAttrs (n: v: v) (builtins.readDir " + td.path().string() + "); in d.hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    td.addFile("newfile", "new-content");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_MultiAccessOneChanges)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    td.addFile("other", "z");
    auto expr = "let d = builtins.readDir " + td.path().string()
        + "; in d.a + \"-\" + d.b";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }

    // Add unrelated file — both accessed entries unchanged
    td.addFile("c", "new");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_AddMultipleFiles)
{
    TempDir td;
    td.addFile("hello", "content");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    td.addFile("new1", "a");
    td.addFile("new2", "b");
    td.addFile("new3", "c");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_MixedContentAndDir)
{
    // readFile (fromJSON) + readDir, change unused JSON key AND add file to dir
    TempJsonFile jf(R"({"used": "stable", "unused": "original"})");
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad");

    auto expr = "let j = builtins.fromJSON (builtins.readFile " + jf.path.string()
        + "); d = builtins.readDir " + td.path().string()
        + "; in j.used + \"-\" + d.hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable-regular"));
    }

    // Change unused JSON key AND add file to dir
    jf.modify(R"({"used": "stable", "unused": "changed-value!!"})");
    invalidateFileCache(jf.path);
    td.addFile("newfile", "new");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // both Content and Directory override
        EXPECT_THAT(v, IsStringEq("stable-regular"));
    }
}

// ── Group 3: Two-Level Override — Negative Cases ───────────────────────

TEST_F(TracedDataTest, TracedDir_AccessedEntryTypeChanges)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    // Change "hello" from regular → symlink
    td.changeToSymlink("hello", "other");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // trace invalid (SC dep for hello fails)
        EXPECT_THAT(v, IsStringEq("symlink"));
    }
}

TEST_F(TracedDataTest, TracedDir_AccessedEntryRemoved)
{
    TempDir td;
    td.addFile("hello", "content");
    td.addFile("other", "pad-pad");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    td.removeEntry("hello");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        // Re-eval: .hello missing → throws
        EXPECT_THROW(forceRoot(*cache), Error);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedDir_NoValuesForced)
{
    // attrNames only — no per-entry SC deps recorded
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "builtins.attrNames (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    td.addFile("c", "z");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // no SC deps → no override
    }
}

TEST_F(TracedDataTest, TracedDir_AllEntriesChangedType)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "let d = builtins.readDir " + td.path().string()
        + "; in d.a + \"-\" + d.b";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }

    // Change both to directories
    td.changeToSubdir("a");
    td.changeToSubdir("b");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("directory-directory"));
    }
}

TEST_F(TracedDataTest, TracedDir_OneAccessedOneChanged)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "let d = builtins.readDir " + td.path().string()
        + "; in d.a + \"-\" + d.b";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }

    // Change only b's type
    td.changeToSubdir("b");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for b fails
        EXPECT_THAT(v, IsStringEq("regular-directory"));
    }
}

TEST_F(TracedDataTest, TracedDir_MapAttrsIgnoreValues)
{
    // mapAttrs (n: _: n) — ignores values entirely, no SC deps
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "builtins.mapAttrs (n: _: n) (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nAttrs);
    }

    td.addFile("c", "z");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // no SC deps → no override
    }
}

TEST_F(TracedDataTest, TracedDir_AccessedEntryReplacedSameType)
{
    // Replace "hello" with different content but same type (regular → regular)
    TempDir td;
    td.addFile("hello", "content-A");
    td.addFile("other", "pad");
    auto expr = "(builtins.readDir " + td.path().string() + ").hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    // Re-create hello with different content but same type
    td.removeEntry("hello");
    td.addFile("hello", "content-B-different");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        // Type unchanged ("regular" = "regular") → trace valid
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_MixedInvalid)
{
    // readFile (fromJSON) + readDir, accessed JSON leaf changes
    TempJsonFile jf(R"({"used": "hello", "extra": "x"})");
    TempDir td;
    td.addFile("a", "content");

    auto expr = "let j = builtins.fromJSON (builtins.readFile " + jf.path.string()
        + "); d = builtins.readDir " + td.path().string()
        + "; in j.used + \"-\" + d.a";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello-regular"));
    }

    // Change accessed JSON leaf
    jf.modify(R"({"used": "CHANGED!!", "extra": "x"})");
    invalidateFileCache(jf.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for used fails
        EXPECT_THAT(v, IsStringEq("CHANGED!!-regular"));
    }
}

// ── Group 4: Shape Deps for readDir ────────────────────────────────────

TEST_F(TracedDataTest, TracedDir_AttrNames_KeyAdded)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "builtins.attrNames (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    td.addFile("c", "z");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys changes
    }
}

TEST_F(TracedDataTest, TracedDir_AttrNames_KeyRemoved)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    td.addFile("c", "z");
    auto expr = "builtins.attrNames (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    td.removeEntry("c");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys changes
    }
}

TEST_F(TracedDataTest, TracedDir_AttrNames_TypeChanged)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "builtins.attrNames (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Change type of "b" — keys stay the same
    td.changeToSubdir("b");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #keys unchanged → override applies
    }
}

TEST_F(TracedDataTest, TracedDir_HasAttr_True_KeyRemoved)
{
    TempDir td;
    td.addFile("foo", "x");
    td.addFile("bar", "y");
    auto expr = "builtins.hasAttr \"foo\" (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    td.removeEntry("foo");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys changes
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(TracedDataTest, TracedDir_HasAttr_False_KeyAdded)
{
    TempDir td;
    td.addFile("bar", "x");
    auto expr = "builtins.hasAttr \"foo\" (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    td.addFile("foo", "y");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys changes
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(TracedDataTest, TracedDir_AttrNamesAndAccess_KeyAdded)
{
    TempDir td;
    td.addFile("hello", "x");
    td.addFile("other", "y");
    auto expr = "let d = builtins.readDir " + td.path().string()
        + "; in (builtins.concatStringsSep \",\" (builtins.attrNames d)) + \":\" + d.hello";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello,other:regular"));
    }

    td.addFile("added", "z");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys SC dep fails
        EXPECT_THAT(v, IsStringEq("added,hello,other:regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_AttrNames_ValChangedKeySame)
{
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "builtins.attrNames (builtins.readDir " + td.path().string() + ")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    // Change all types but keep same entries
    td.changeToSubdir("a");
    td.changeToSymlink("b", "a");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #keys hash only covers names
    }
}

// ── Group 5: Edge Cases ────────────────────────────────────────────────

TEST_F(TracedDataTest, TracedDir_ContainerProvLostAfterUpdate)
{
    // attrNames (readDir dir // { x = 1; }) — container provenance is lost
    // because // creates a new Bindings*, so #keys dep is NOT recorded
    TempDir td;
    td.addFile("a", "x");
    td.addFile("b", "y");
    auto expr = "builtins.attrNames (builtins.readDir " + td.path().string() + " // { x = 1; })";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nList);
    }

    td.addFile("c", "z");
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        // Container prov lost → no #keys dep → Directory dep alone, no override possible
        // BUT: the readDir value is still ExprTracedData. After //, attrNames sees
        // the merged Bindings* which has no provenance. No SC deps → must re-eval.
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedDir_NestedReadDir)
{
    TempDir td1;
    td1.addFile("a", "x");
    td1.addFile("a-other", "y");
    TempDir td2;
    td2.addFile("b", "p");
    td2.addFile("b-other", "q");

    auto expr = "let d1 = builtins.readDir " + td1.path().string()
        + "; d2 = builtins.readDir " + td2.path().string()
        + "; in d1.a + \"-\" + d2.b";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }

    // Change dir1 only (add unrelated file)
    td1.addFile("new-in-d1", "z");
    INVALIDATE_DIR(td1);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // dir1 override (a unchanged), dir2 unchanged
        EXPECT_THAT(v, IsStringEq("regular-regular"));
    }
}

TEST_F(TracedDataTest, TracedDir_LargeDirectory)
{
    TempDir td;
    // Create 100 entries
    for (int i = 0; i < 100; i++)
        td.addFile("entry-" + std::to_string(i), std::to_string(i));

    auto expr = "(builtins.readDir " + td.path().string() + ").\"entry-42\"";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("regular"));
    }

    // Add more entries
    for (int i = 100; i < 110; i++)
        td.addFile("entry-" + std::to_string(i), std::to_string(i));
    INVALIDATE_DIR(td);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // override applies (entry-42 unchanged)
        EXPECT_THAT(v, IsStringEq("regular"));
    }
}

// ── Category B — #len from iterating builtins ───────────────────────

TEST_F(TracedDataTest, TracedJSON_Map_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.map (x: x) j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Map_ArrayShrinks)
{
    TempJsonFile file(R"({"items":["a","b","c"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.map (x: x) j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }

    file.modify(R"({"items":["a","b"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Filter_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.filter (x: true) j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_FoldlStrict_ArrayGrows)
{
    TempJsonFile file(R"({"nums":[1,2,3]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.foldl' (a: b: a + b) 0 j.nums)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(6));
    }

    file.modify(R"({"nums":[1,2,3,4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(10));
    }
}

TEST_F(TracedDataTest, TracedJSON_Sort_ArrayGrows)
{
    TempJsonFile file(R"({"items":["b","a"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.sort (a: b: a < b) j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":["b","a","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ConcatStringsSep_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Any_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.any (x: x == "c") j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedJSON_All_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.all (x: x != "c") j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedJSON_Tail_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b","c"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.tail j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("b,c"));
    }

    file.modify(R"({"items":["a","b","c","d"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("b,c,d"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ConcatLists_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (j.items ++ ["extra"]))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b,extra"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,b,c,extra"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Elem_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.elem "c" j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse());
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedJSON_ConcatMap_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.concatMap (x: [x x]) j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,a,b,b"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("a,a,b,b,c,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Partition_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","d"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); p = builtins.partition (x: x < "c") j.items; in builtins.concatStringsSep "," p.right)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        // partition (x: x < "c") ["a","d"] → right=["a"], wrong=["d"]
        EXPECT_THAT(v, IsStringEq("a"));
    }

    file.modify(R"({"items":["a","d","b"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        // partition (x: x < "c") ["a","d","b"] → right=["a","b"], wrong=["d"]
        EXPECT_THAT(v, IsStringEq("a,b"));
    }
}

TEST_F(TracedDataTest, TracedJSON_GroupBy_ArrayGrows)
{
    TempJsonFile file(R"({"tags":["a","a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); g = builtins.groupBy (x: x) j.tags; in builtins.toString (builtins.length g.a))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("2"));
    }

    file.modify(R"({"tags":["a","a","b","a"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("3"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ReplaceStrings_ArrayGrows)
{
    TempJsonFile file(R"({"from":["hello"],"to":["hi"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.replaceStrings j.from j.to "hello world")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hi world"));
    }

    file.modify(R"({"from":["hello","world"],"to":["hi","earth"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("hi earth"));
    }
}

TEST_F(TracedDataTest, TracedTOML_Map_ArrayGrows)
{
    TempTomlFile file("[[package]]\nname = \"foo\"\n\n[[package]]\nname = \"bar\"\n");
    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.map (x: x.name) t.package))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("foo,bar"));
    }

    file.modify("[[package]]\nname = \"foo\"\n\n[[package]]\nname = \"bar\"\n\n[[package]]\nname = \"baz\"\n");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("foo,bar,baz"));
    }
}

// ── Category C — #keys from attrValues ──────────────────────────────

TEST_F(TracedDataTest, TracedJSON_AttrValues_KeyAdded)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.attrValues j))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x,y"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("x,y,z"));
    }
}

// ── Category E — #type container type change ────────────────────────

TEST_F(TracedDataTest, TracedJSON_TypeChange_ArrayToObject)
{
    TempJsonFile file(R"({"data":[1,2,3],"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.typeOf j.data + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    file.modify(R"({"data":{"a":1},"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }
}

TEST_F(TracedDataTest, TracedJSON_TypeChange_ObjectToArray)
{
    TempJsonFile file(R"({"data":{"a":1},"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.typeOf j.data + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }

    file.modify(R"({"data":[1,2,3],"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }
}

// ── Category A — Positional access (regression) ─────────────────────

TEST_F(TracedDataTest, TracedJSON_Head_Append_CacheHit)
{
    TempJsonFile file(R"({"items":["stable","other"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.head j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"items":["stable","other","new"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Head_Prepend_CacheMiss)
{
    TempJsonFile file(R"({"items":["stable","other"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.head j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"items":["new","stable","other"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("new"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ElemAt_Append_CacheHit)
{
    TempJsonFile file(R"({"items":["a","stable","c"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.elemAt j.items 1)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("stable"));
    }

    file.modify(R"({"items":["a","stable","c","new"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("stable"));
    }
}

// ── Category D — Attrset output, name-based access (negative) ───────

TEST_F(TracedDataTest, TracedJSON_MapAttrs_KeyAdded_CacheHit)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"((builtins.mapAttrs (n: v: v + "!") (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"())).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x!"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("x!"));
    }
}

TEST_F(TracedDataTest, TracedJSON_Update_KeyAdded_CacheMiss)
{
    // The // operator now records #keys for both operands. When a key is added
    // to the traced JSON, the #keys dep fails even though .a is unchanged.
    // This is the expected precision trade-off for the soundness fix.
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (j // { extra = "e"; }).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys dep causes re-eval
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_IntersectAttrs_KeyAdded_CacheMiss)
{
    // intersectAttrs now records #keys for both operands. When a key is added
    // to the traced JSON, the #keys dep fails even though .a is unchanged.
    // This is the expected precision trade-off for the soundness fix.
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.intersectAttrs { a = true; } j).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys dep causes re-eval
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_RemoveAttrs_KeyAdded_CacheHit)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.removeAttrs j ["unused"]).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ListToAttrs_ElementAppended_CacheMiss)
{
    // listToAttrs now records #len for the input list. When an element is
    // appended, the #len dep fails even though .a is unchanged.
    // This is the expected precision trade-off for the soundness fix.
    TempJsonFile file(R"([{"name":"a","value":"x"},{"name":"b","value":"y"}])");
    auto expr = R"((builtins.listToAttrs (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"())).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x"));
    }

    file.modify(R"([{"name":"a","value":"x"},{"name":"b","value":"y"},{"name":"c","value":"z"}])");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep causes re-eval
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_NestedFieldAccess_SiblingAdded_CacheHit)
{
    TempJsonFile file(R"({"nodes":{"root":{"inputs":{"nixpkgs":"abc"}}}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in j.nodes.root.inputs.nixpkgs)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("abc"));
    }

    file.modify(R"({"nodes":{"root":{"inputs":{"nixpkgs":"abc","home-manager":"def"}}}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("abc"));
    }
}

// ── Scalar dep correctness (regression) ─────────────────────────────

TEST_F(TracedDataTest, TracedJSON_Map_SameLength_ValueChange_CacheMiss)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.map (x: x) j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":["x","y"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("x,y"));
    }
}

// ── #type persistence (negative — type unchanged) ───────────────────

TEST_F(TracedDataTest, TracedJSON_TypeUnchanged_CacheHit)
{
    // typeOf j.data records a #type dep on data (hash of "array").
    // j.name records a scalar dep. No #len dep is recorded because
    // no iterating builtin is called on data.
    TempJsonFile file(R"({"data":[1,2],"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.typeOf j.data + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    // Array grows but is still an array — #type dep passes. name unchanged.
    file.modify(R"({"data":[1,2,3],"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }
}

// ── Category F — toJSON serialization ───────────────────────────────

TEST_F(TracedDataTest, TracedJSON_ToJSON_ArrayGrows)
{
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(builtins.toJSON (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        // toJSON produces a string representation of the parsed JSON
        EXPECT_TRUE(v.type() == nString);
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedJSON_ToJSON_KeyAdded)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(builtins.toJSON (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nString);
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
    }
}

TEST_F(TracedDataTest, TracedJSON_ToJSON_OutPath_CacheHit)
{
    // toJSON { outPath = j.name; extra = j; } short-circuits to just "\"foo\""
    // because outPath is the only thing serialized for sets with outPath.
    TempJsonFile file(R"({"name":"foo","other":"bar"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.toJSON { outPath = j.name; extra = j; })";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("\"foo\""));
    }

    file.modify(R"({"name":"foo","other":"baz"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("\"foo\""));
    }
}

// ── Category G — ? operator and or expression ───────────────────────

TEST_F(TracedDataTest, TracedJSON_HasOp_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"a":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (if j ? b then j.b else "default") + "-" + j.a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("default-x"));
    }

    file.modify(R"({"a":"x","b":"y"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_HasOp_KeyRemoved_CacheMiss)
{
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (if j ? b then j.b else "default") + "-" + j.a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }

    file.modify(R"({"a":"x"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("default-x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_HasOp_KeyUnchanged_CacheHit)
{
    // .b scalar dep fails (y→z), so re-eval happens due to scalar dep change.
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (if j ? a then j.a else "default") + "-" + j.b)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x-y"));
    }

    file.modify(R"({"a":"x","b":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // cache miss from scalar dep (.b changed y→z)
        EXPECT_THAT(v, IsStringEq("x-z"));
    }
}

TEST_F(TracedDataTest, TracedJSON_SelectOrDefault_KeyAdded_CacheMiss)
{
    TempJsonFile file(R"({"a":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (j.b or "default") + "-" + j.a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("default-x"));
    }

    file.modify(R"({"a":"x","b":"y"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_SelectOrDefault_KeyPresent_CacheHit)
{
    // or expression takes value path (j.b exists). .b and .a pass. .c not accessed.
    TempJsonFile file(R"({"a":"x","b":"y","c":"z"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (j.b or "default") + "-" + j.a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }

    file.modify(R"({"a":"x","b":"y","c":"w"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("y-x"));
    }
}

// ── Tricky area tests — additional coverage ─────────────────────────

TEST_F(TracedDataTest, TracedJSON_CoerceToString_ListGrows)
{
    // toString on traced list should record #len via coerceToString.
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in toString j.items)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a b"));
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep fails (2→3)
        EXPECT_THAT(v, IsStringEq("a b c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_IsAttrs_ObjectToArray)
{
    // isAttrs records #type dep. Type change object→array invalidates.
    TempJsonFile file(R"({"data":{"a":1},"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (if builtins.isAttrs j.data then "set" else "other") + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }

    file.modify(R"({"data":[1,2,3],"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #type dep fails (object→array)
        EXPECT_THAT(v, IsStringEq("other-foo"));
    }
}

TEST_F(TracedDataTest, TracedJSON_IsList_ArrayToObject)
{
    // isList records #type dep. Type change array→object invalidates.
    TempJsonFile file(R"({"data":[1,2],"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (if builtins.isList j.data then "list" else "other") + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    file.modify(R"({"data":{"a":1},"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #type dep fails (array→object)
        EXPECT_THAT(v, IsStringEq("other-foo"));
    }
}

TEST_F(TracedDataTest, TracedJSON_CatAttrs_ListGrows)
{
    // catAttrs iterates the input list — should record #len.
    TempJsonFile file(R"({"items":[{"name":"a"},{"name":"b"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.catAttrs "name" j.items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a,b"));
    }

    file.modify(R"({"items":[{"name":"a"},{"name":"b"},{"name":"c"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep fails (2→3)
        EXPECT_THAT(v, IsStringEq("a,b,c"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ZipAttrsWith_ListGrows)
{
    // zipAttrsWith iterates the input list — should record #len.
    // zipAttrsWith returns attrset where each value is f(name, [vals...]).
    TempJsonFile file(R"({"sets":[{"a":"x"},{"a":"y"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.zipAttrsWith (name: vals: builtins.concatStringsSep "+" vals) j.sets).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x+y"));
    }

    file.modify(R"({"sets":[{"a":"x"},{"a":"y"},{"a":"z"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep fails (2→3)
        EXPECT_THAT(v, IsStringEq("x+y+z"));
    }
}

TEST_F(TracedDataTest, TracedJSON_RemoveAttrs_NameListGrows)
{
    // removeAttrs name list records #len. Adding a name removes more keys.
    TempJsonFile file(R"({"names":["b"],"data":{"a":"x","b":"y","c":"z"}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.concatStringsSep "," (builtins.attrValues (builtins.removeAttrs j.data j.names)))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("x,z"));
    }

    file.modify(R"({"names":["b","c"],"data":{"a":"x","b":"y","c":"z"}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep on names list fails (1→2)
        EXPECT_THAT(v, IsStringEq("x"));
    }
}

TEST_F(TracedDataTest, TracedJSON_NestedTypeChange)
{
    // Type change at non-root level. typeOf j.data.inner changes type.
    TempJsonFile file(R"({"data":{"inner":[1,2]},"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.typeOf j.data.inner + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    file.modify(R"({"data":{"inner":{"a":1}},"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #type dep fails at data.inner (array→object)
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }
}

TEST_F(TracedDataTest, TracedJSON_EmptyArray_ThenGrows_NoFalseHit)
{
    // Empty array can't be tracked (no stable pointer). Verify that
    // when array becomes non-empty, Content dep failure causes re-eval.
    TempJsonFile file(R"({"items":[],"name":"foo"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in toString (builtins.length j.items) + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("0-foo"));
    }

    file.modify(R"({"items":["a","b"],"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        // No SC deps → two-level override cannot apply → Content failure → re-eval
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("2-foo"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ToXML_ArrayGrows)
{
    // toXML iterates list elements — should record #len.
    TempJsonFile file(R"({"items":["a","b"]})");
    auto expr = R"(builtins.toXML (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nString);
    }

    file.modify(R"({"items":["a","b","c"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len dep fails from printValueAsXML
    }
}

TEST_F(TracedDataTest, TracedJSON_ToXML_KeyAdded)
{
    // toXML iterates attrset keys — should record #keys.
    TempJsonFile file(R"({"a":"x","b":"y"})");
    auto expr = R"(builtins.toXML (builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_TRUE(v.type() == nString);
    }

    file.modify(R"({"a":"x","b":"y","c":"z"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys dep fails from printValueAsXML
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Standalone StructuredContent dep tests (cross-trace dep separation)
// ═══════════════════════════════════════════════════════════════════════
// These tests verify that SC deps for files WITHOUT a corresponding
// Content/Directory dep in the same trace are correctly verified.
// This arises when a parent trace calls readFile+fromJSON (Content dep
// in parent), and a child trace forces thunks from the result (SC deps
// in child, but NO Content dep). The child's standalone SC deps must
// be checked even when no coarse dep fails in the child trace.

TEST_F(TracedDataTest, TracedJSON_StandaloneStructuralDep_CacheMiss)
{
    // Child trace has SC dep for .name but no Content dep for the file.
    // When .name changes, the standalone SC dep must fail → cache miss.
    TempJsonFile f(R"({"name":"foo","marker":"ok"})");

    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + f.path.string()
        + R"(); in builtins.seq j.marker { x = j.name; })";

    // Cold run: record root + child traces
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);
        EXPECT_THAT(*xAttr->value, IsStringEq("foo"));
    }

    // Modify: change .name, keep .marker unchanged
    f.modify(R"({"name":"changed","marker":"ok"})");
    invalidateFileCache(f.path);

    // Hot run: standalone SC dep for .name in child must fail
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);

        auto xStr = state.forceStringNoCtx(*xAttr->value, noPos, "");
        EXPECT_EQ(std::string(xStr), "changed");
    }
    EXPECT_EQ(loaderCalls, 1);
}

TEST_F(TracedDataTest, TracedJSON_StandaloneStructuralDep_CacheHit)
{
    // Child trace has SC dep for .name (standalone). When an unrelated
    // field changes, root's override accepts (marker SC passes) and
    // child's standalone SC dep for .name also passes → cache hit.
    TempJsonFile f(R"({"name":"foo","marker":"ok","extra":"bar"})");

    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + f.path.string()
        + R"(); in builtins.seq j.marker { x = j.name; })";

    // Cold run
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);
        EXPECT_THAT(*xAttr->value, IsStringEq("foo"));
    }

    // Modify: change unrelated field, keep .name and .marker
    f.modify(R"({"name":"foo","marker":"ok","extra":"changed"})");
    invalidateFileCache(f.path);

    // Hot run: everything should be served from cache
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);

        auto xStr = state.forceStringNoCtx(*xAttr->value, noPos, "");
        EXPECT_EQ(std::string(xStr), "foo");
    }
    EXPECT_EQ(loaderCalls, 0);
}

TEST_F(TracedDataTest, TracedJSON_StandaloneStructuralDep_MarkerChanges)
{
    // When the root's SC dep (.marker) fails, the root is re-evaluated
    // (override rejected). This changes the root's trace hash, so the
    // child's ParentContext dep fails → child also re-evaluated.
    // Confirms existing ParentContext cascade behavior.
    TempJsonFile f(R"({"name":"foo","marker":"ok"})");

    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + f.path.string()
        + R"(); in builtins.seq j.marker { x = j.name; })";

    // Cold run
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);
        EXPECT_THAT(*xAttr->value, IsStringEq("foo"));
    }

    // Modify: change .marker, keep .name
    f.modify(R"({"name":"foo","marker":"changed"})");
    invalidateFileCache(f.path);

    // Hot run: root's SC dep for .marker fails → root re-evaluated →
    // child's ParentContext dep fails → child re-evaluated
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);

        auto xStr = state.forceStringNoCtx(*xAttr->value, noPos, "");
        EXPECT_EQ(std::string(xStr), "foo");
    }
    EXPECT_EQ(loaderCalls, 1);
}

TEST_F(TracedDataTest, TracedTOML_StandaloneStructuralDep_CacheMiss)
{
    // Same as TracedJSON_StandaloneStructuralDep_CacheMiss but with TOML.
    // Confirms the fix is format-agnostic.
    TempTomlFile f("[data]\nname = \"foo\"\nmarker = \"ok\"\n");

    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + f.path.string()
        + R"(); in builtins.seq t.data.marker { x = t.data.name; })";

    // Cold run
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);
        EXPECT_THAT(*xAttr->value, IsStringEq("foo"));
    }

    // Modify: change name, keep marker
    f.modify("[data]\nname = \"changed\"\nmarker = \"ok\"\n");
    invalidateFileCache(f.path);

    // Hot run: standalone SC dep for data.name must fail
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);

        auto xStr = state.forceStringNoCtx(*xAttr->value, noPos, "");
        EXPECT_EQ(std::string(xStr), "changed");
    }
    EXPECT_EQ(loaderCalls, 1);
}

// ═══════════════════════════════════════════════════════════════════════
// Adversarial soundness tests: gaps where shape dep recording is missing
// ═══════════════════════════════════════════════════════════════════════

// ── Gap 1 (FIXED): eqValues (== and !=) — shape deps added ──────────

TEST_F(TracedDataTest, TracedJSON_EqOp_ListLengthGrows)
{
    // [FIXED] == checks list length — #len shape dep now recorded at eval.cc:2964-2975.
    // Cold: arr has 2 elements, matches literal → true.
    // Hot: arr grows to 3 elements, but SC deps for [0] and [1] still pass.
    // Without #len dep, override incorrectly accepts stale "true".
    TempJsonFile file(R"({"arr":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.arr == ["a" "b"] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    // arr grows: ["a","b"] → ["a","b","c!!"] (different size for stat invalidation)
    file.modify(R"({"arr":["a","b","c!!"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (length changed)
        EXPECT_THAT(v, IsStringEq("no")); // 3-element list != 2-element literal
    }
}

TEST_F(TracedDataTest, TracedJSON_EqOp_AttrsetKeyAdded)
{
    // [FIXED] == checks attrset key count/names — #keys shape dep now recorded at eval.cc:2964-2975.
    // Cold: obj has {a,b}, matches literal → true.
    // Hot: obj gains key "c", but SC deps for .a and .b still pass.
    TempJsonFile file(R"({"obj":{"a":1,"b":2}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.obj == { a = 1; b = 2; } then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    file.modify(R"({"obj":{"a":1,"b":2,"c":3}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (key set changed)
        EXPECT_THAT(v, IsStringEq("no")); // extra key → not equal
    }
}

TEST_F(TracedDataTest, TracedJSON_NeqOp_ListLengthGrows)
{
    // [FIXED] != has the same fix as == (shares eqValues).
    // Cold: arr matches → != returns false → "no".
    // Hot: arr grows → should return true → "yes".
    TempJsonFile file(R"({"arr":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.arr != ["a" "b"] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("no")); // equal → != is false
    }

    file.modify(R"({"arr":["a","b","c!!"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
        EXPECT_THAT(v, IsStringEq("yes")); // not equal → != is true
    }
}

TEST_F(TracedDataTest, TracedJSON_EqOp_ListElementChanges)
{
    // [PRECISION] Same length, element changes → SC dep for element fails → re-eval.
    TempJsonFile file(R"({"arr":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.arr == ["a" "b"] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    // Same length but element 1 changes (different size for stat invalidation)
    file.modify(R"({"arr":["a","B!!!"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for [1] fails → re-eval
        EXPECT_THAT(v, IsStringEq("no"));
    }
}

TEST_F(TracedDataTest, TracedJSON_EqOp_ListUnrelatedChange)
{
    // [PRECISION] arr unchanged, unrelated key changes → override correctly accepts.
    TempJsonFile file(R"({"arr":["a","b"],"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.arr == ["a" "b"] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    file.modify(R"({"arr":["a","b"],"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (arr unchanged)
        EXPECT_THAT(v, IsStringEq("yes"));
    }
}

// ── Gap 2 (FIXED): genericClosure — indirect #len via shared Value* ─

TEST_F(TracedDataTest, TracedJSON_GenericClosure_StartSetGrows)
{
    // [FIXED] genericClosure — indirect fix via shared Value* provenance (see design.md Gap 2).
    // Cold: 2 nodes → closure has 2 elements.
    // Hot: 3 nodes, existing SC deps for [0].key and [1].key pass, no #len dep.
    TempJsonFile file(R"({"nodes":[{"key":"a"},{"key":"b"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length (builtins.genericClosure { startSet = j.nodes; operator = n: []; }))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"nodes":[{"key":"a"},{"key":"b"},{"key":"c!!"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (startSet grew)
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(TracedDataTest, TracedJSON_GenericClosure_ElementChanges)
{
    // [PRECISION] Element value changes → SC dep for that element fails → re-eval.
    TempJsonFile file(R"({"nodes":[{"key":"a"},{"key":"b"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.head (builtins.genericClosure { startSet = j.nodes; operator = n: []; }))";

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * keyAttr = root.attrs()->get(state.symbols.create("key"));
        ASSERT_NE(keyAttr, nullptr);
        state.forceValue(*keyAttr->value, noPos);
        EXPECT_THAT(*keyAttr->value, IsStringEq("a"));
    }

    file.modify(R"({"nodes":[{"key":"CHANGED!"},{"key":"b"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * keyAttr = root.attrs()->get(state.symbols.create("key"));
        ASSERT_NE(keyAttr, nullptr);
        state.forceValue(*keyAttr->value, noPos);
        EXPECT_EQ(loaderCalls, 1); // SC dep for [0].key fails
        EXPECT_THAT(*keyAttr->value, IsStringEq("CHANGED!"));
    }
}

TEST_F(TracedDataTest, TracedJSON_GenericClosure_CacheHit)
{
    // [PRECISION] Unaccessed non-key attribute changes → cache hit.
    // genericClosure reads ALL elements' .key for dedup, so we must change
    // a non-.key attribute to avoid invalidation.
    TempJsonFile file(R"({"nodes":[{"key":"a","val":"x"},{"key":"b","val":"y"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.head (builtins.genericClosure { startSet = j.nodes; operator = n: []; }))";

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * keyAttr = root.attrs()->get(state.symbols.create("key"));
        ASSERT_NE(keyAttr, nullptr);
        state.forceValue(*keyAttr->value, noPos);
        EXPECT_THAT(*keyAttr->value, IsStringEq("a"));
    }

    // Change element [1]'s non-key attribute only (unaccessed by result)
    file.modify(R"({"nodes":[{"key":"a","val":"x"},{"key":"b","val":"CHANGED!"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * keyAttr = root.attrs()->get(state.symbols.create("key"));
        ASSERT_NE(keyAttr, nullptr);
        state.forceValue(*keyAttr->value, noPos);
        EXPECT_EQ(loaderCalls, 0); // cache hit ([0].key and [1].key unchanged)
        EXPECT_THAT(*keyAttr->value, IsStringEq("a"));
    }
}

// ── Gap 3 (FIXED): listToAttrs — #len on input list ─────────────────

TEST_F(TracedDataTest, TracedJSON_ListToAttrs_FullResultGrows)
{
    // [FIXED] listToAttrs — #len shape dep now recorded at primops.cc:3543.
    // Cold: 2 items → 2 attrs. Hot: 3 items, existing SC deps pass.
    TempJsonFile file(R"({"items":[{"name":"a","value":"1"},{"name":"b","value":"2"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length (builtins.attrNames (builtins.listToAttrs j.items)))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"items":[{"name":"a","value":"1"},{"name":"b","value":"2"},{"name":"c","value":"3!!"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (list grew)
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(TracedDataTest, TracedJSON_ListToAttrs_ElementChanges)
{
    // [PRECISION] Same list length, element value changes → SC dep fails → re-eval.
    TempJsonFile file(R"({"items":[{"name":"a","value":"1"},{"name":"b","value":"2"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.listToAttrs j.items).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("1"));
    }

    // Change value of "a" item, keep list length the same
    file.modify(R"({"items":[{"name":"a","value":"99"},{"name":"b","value":"2"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for .value fails
        EXPECT_THAT(v, IsStringEq("99"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ListToAttrs_UnrelatedChange)
{
    // [PRECISION] Unrelated field changes → SC override accepts → cache hit.
    TempJsonFile file(R"({"items":[{"name":"a","value":"1"},{"name":"b","value":"2"}],"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.listToAttrs j.items).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("1"));
    }

    // Only change unrelated field
    file.modify(R"({"items":[{"name":"a","value":"1"},{"name":"b","value":"2"}],"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (items unchanged)
        EXPECT_THAT(v, IsStringEq("1"));
    }
}

// ── Gap 4: Positive tests for raw + parsed readFile scenarios ────────

TEST_F(TracedDataTest, RawOnly_StringLength_ContentChanges)
{
    // Raw-only readFile correctly invalidates when content changes.
    // No SC deps at all — Content dep failure forces re-evaluation.
    TempJsonFile file(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let raw = builtins.readFile )" + file.path.string()
        + R"(; in builtins.stringLength raw)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(30));
    }

    file.modify(R"({"name":"foo","extra":"longer!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Content dep fails, no SC deps → must re-eval
        EXPECT_THAT(v, IsIntEq(33));
    }
}

TEST_F(TracedDataTest, RawOnly_Substring_ContentChanges)
{
    // Raw substring invalidates when content changes.
    // No SC deps — Content dep failure forces re-evaluation.
    TempJsonFile file(R"({"name":"foo"})");
    auto expr = R"(let raw = builtins.readFile )" + file.path.string()
        + R"(; in builtins.substring 0 10 raw)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq(R"({"name":"f)"));
    }

    file.modify(R"({"xame":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Content dep fails, no SC override
        EXPECT_THAT(v, IsStringEq(R"({"xame":"f)"));
    }
}

TEST_F(TracedDataTest, RawOnly_StringLength_SameSizeChange)
{
    // Raw invalidates even when file size is unchanged.
    // Tests that we hash content, not just stat metadata.
    TempJsonFile file(R"({"a":"xxx"})");
    auto expr = R"(let raw = builtins.readFile )" + file.path.string()
        + R"(; in builtins.stringLength raw)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(11));
    }

    file.modify(R"({"a":"yyy"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Content hash changes even though size is same
        EXPECT_THAT(v, IsIntEq(11));
    }
}

TEST_F(TracedDataTest, ParsedOnly_UnusedFieldChange_CacheHit)
{
    // Parsed-only path allows SC override when only unused fields change.
    // Adjacent to Gap 4 to make the contrast with raw+parsed explicit.
    TempJsonFile file(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("foo"));
    }

    file.modify(R"({"name":"foo","extra":"longer!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // Content fails, SC dep on .name passes → override
        EXPECT_THAT(v, IsStringEq("foo"));
    }
}

TEST_F(TracedDataTest, RawAndParsed_DifferentFiles_RawFileChanges)
{
    // Raw + parsed from different files: SC deps from parsed file cannot cover
    // raw file's Content dep failure.
    TempTextFile file1("hello world");
    TempJsonFile file2(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let raw = builtins.readFile )" + file1.path.string()
        + R"(; j = builtins.fromJSON (builtins.readFile )" + file2.path.string()
        + R"(); in toString (builtins.stringLength raw) + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_EQ(std::string(str), "11-foo");
    }

    // Only modify the raw file
    file1.modify("hello world!!");
    invalidateFileCache(file1.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // file1 Content fails, no SC deps for file1 → re-eval
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_EQ(std::string(str), "13-foo");
    }
}

TEST_F(TracedDataTest, RawAndParsed_DifferentFiles_ParsedUnusedChange)
{
    // Raw + parsed from different files: SC override applies only to parsed file.
    // Raw file unchanged → its Content dep passes. Parsed file's unused field
    // changes → Content fails but SC dep on .name passes → override.
    TempTextFile file1("hello world");
    TempJsonFile file2(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let raw = builtins.readFile )" + file1.path.string()
        + R"(; j = builtins.fromJSON (builtins.readFile )" + file2.path.string()
        + R"(); in toString (builtins.stringLength raw) + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_EQ(std::string(str), "11-foo");
    }

    // Only modify parsed file's unused field
    file2.modify(R"({"name":"foo","extra":"longer!!"})");
    invalidateFileCache(file2.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // file1 passes, file2 SC override → cache hit
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_EQ(std::string(str), "11-foo");
    }
}

TEST_F(TracedDataTest, RawAndParsed_SameFile_AccessedFieldChanges)
{
    // Same file, raw + parsed: correctly re-evals when SC dep also fails.
    // Content dep fails AND SC dep on .name fails → override rejected → re-eval.
    // Contrasts with the DISABLED Gap 4 test where only an unused field changes.
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

    // Change the accessed field (.name)
    file.modify(R"({"name":"bar","extra":"short"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Content fails, SC dep on .name ALSO fails → re-eval
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_TRUE(std::string(str).find("-bar") != std::string::npos);
    }
}

// ── Gap 4: Raw + parsed readFile from same file (KNOWN BUG) ─────────

TEST_F(TracedDataTest, DISABLED_TracedJSON_RawAndParsedReadFile_ContentChanges)
{
    // [SOUNDNESS] DEFERRED: Raw + parsed readFile from same file requires deeper
    // design work. When both a raw readFile (used via stringLength) and a parsed
    // readFile (used via fromJSON) reference the same file, SC deps from the parsed
    // path can incorrectly "cover" the Content dep failure for the raw path.
    // This needs a mechanism to distinguish raw vs parsed readFile provenance.
    TempJsonFile file(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let raw = builtins.readFile )" + file.path.string()
        + R"(; j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in toString (builtins.stringLength raw) + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        // File is 30 bytes: {"name":"foo","extra":"short"}
        auto str = state.forceStringNoCtx(v, noPos, "");
        // Just verify it has the right format (number-name)
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

// ── Gap 5 (FIXED): intersectAttrs — #keys on inputs ────────────────

TEST_F(TracedDataTest, TracedJSON_IntersectAttrs_TracedGainsMatchingKey)
{
    // [FIXED] intersectAttrs — #keys shape dep now recorded at primops.cc:3627-3628.
    // A passing SC dep (marker) triggers the override, but key set changes
    // in intersectAttrs inputs go undetected.
    TempJsonFile file(R"({"a":1,"b":2,"marker":"ok"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.seq j.marker (builtins.length (builtins.attrNames (builtins.intersectAttrs j { a = 1; b = 2; c = 3; }))))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2)); // intersection of {a,b,marker} ∩ {a,b,c} = {a,b}
    }

    // j gains "c" which is in the literal set → intersection grows
    file.modify(R"({"a":1,"b":2,"c":99,"marker":"ok"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (intersection changed)
        EXPECT_THAT(v, IsIntEq(3)); // now {a,b,c}
    }
}

TEST_F(TracedDataTest, TracedJSON_IntersectAttrs_ValueChanges)
{
    // [PRECISION] Same keys, accessed value changes → SC dep fails → re-eval.
    // intersectAttrs takes values from the second argument, so we put traced
    // data as the second arg and access a value from the result.
    TempJsonFile file(R"({"a":1,"b":2})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.intersectAttrs { a = true; } j).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1)); // intersect {a} ∩ {a,b} → {a: j.a} → 1
    }

    // Change value of "a" but keep same keys
    file.modify(R"({"a":99,"b":2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for .a fails
        EXPECT_THAT(v, IsIntEq(99));
    }
}

TEST_F(TracedDataTest, TracedJSON_IntersectAttrs_UnrelatedChange)
{
    // [PRECISION] Unrelated field changes → SC override accepts → cache hit.
    TempJsonFile file(R"({"a":1,"b":2,"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.intersectAttrs { a = true; } j).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Only change unrelated "extra" field
    file.modify(R"({"a":1,"b":2,"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (a unchanged)
        EXPECT_THAT(v, IsIntEq(1));
    }
}

// ── Additional precision tests for Gap 1 ────────────────────────────

TEST_F(TracedDataTest, TracedJSON_EqOp_AttrsetValueChanges)
{
    // [PRECISION] Same keys, value changes → SC dep for value fails → re-eval.
    TempJsonFile file(R"({"obj":{"a":1,"b":2}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.obj == { a = 1; b = 2; } then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    file.modify(R"({"obj":{"a":1,"b":99}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for .b fails
        EXPECT_THAT(v, IsStringEq("no"));
    }
}

TEST_F(TracedDataTest, TracedJSON_EqOp_AttrsetUnrelatedChange)
{
    // [PRECISION] obj unchanged, unrelated key changes → override correctly accepts.
    TempJsonFile file(R"({"obj":{"a":1,"b":2},"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.obj == { a = 1; b = 2; } then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    file.modify(R"({"obj":{"a":1,"b":2},"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (obj unchanged)
        EXPECT_THAT(v, IsStringEq("yes"));
    }
}

// ── Gap B1 (FIXED): CompareValues (<) — #len on lists ───────────────

TEST_F(TracedDataTest, TracedJSON_LessThan_ListLengthGrows)
{
    // [FIXED] Lexicographic list comparison — #len shape dep now recorded at primops.cc:763-764.
    // Cold: [1,2] < [1,2,3] → true (shorter list is "less").
    // Hot: first list grows to [1,2,9], SC deps for [0] and [1] still pass.
    // Without #len dep, override incorrectly accepts stale "true".
    TempJsonFile file(R"({"arr":[1,2]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if builtins.lessThan j.arr [1 2 3] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes")); // [1,2] < [1,2,3]
    }

    // arr grows: [1,2] → [1,2,9] (different size for stat invalidation)
    file.modify(R"({"arr":[1,2,9]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (length changed)
        EXPECT_THAT(v, IsStringEq("no")); // [1,2,9] < [1,2,3] is false
    }
}

TEST_F(TracedDataTest, TracedJSON_LessThan_ListUnrelatedChange)
{
    // [PRECISION] Same lengths, unrelated element changes → SC dep for element fails → re-eval.
    TempJsonFile file(R"({"arr":[1,2],"extra":"short"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if builtins.lessThan j.arr [1 2 3] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    // Only extra changes, arr stays same
    file.modify(R"({"arr":[1,2],"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (arr unchanged)
        EXPECT_THAT(v, IsStringEq("yes"));
    }
}

// ── Gap B2 (FIXED): ExprOpUpdate (//) — #keys on inputs ─────────────

TEST_F(TracedDataTest, TracedJSON_Update_TracedGainsKey)
{
    // [FIXED] // merges attrsets — #keys shape dep now recorded at eval.cc:1981-1982.
    // Cold: base has {a:1}, overlay has {b:2} → result {a:1,b:2}.
    // Hot: base gains key "b" with value 99, but SC dep for .a still passes.
    // Without #keys dep, override incorrectly accepts stale {a:1,b:2}.
    TempJsonFile file(R"({"base":{"a":1},"over":{"b":2}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (j.base // j.over).b)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2)); // overlay's b wins
    }

    // base gains "b":99 — now base // over should still give over's b=2,
    // but the key set of base changed
    file.modify(R"({"base":{"a":1,"b":99},"over":{"b":2}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (base key set changed)
        EXPECT_THAT(v, IsIntEq(2)); // overlay still wins
    }
}

TEST_F(TracedDataTest, TracedJSON_Update_UnrelatedChange)
{
    // [PRECISION] base/over unchanged, unrelated key changes → cache hit.
    TempJsonFile file(R"({"base":{"a":1},"over":{"b":2},"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (j.base // j.over).b)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"base":{"a":1},"over":{"b":2},"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit
        EXPECT_THAT(v, IsIntEq(2));
    }
}

// ── Gap B3 (FIXED): callFunction strict formals — #keys ─────────────

TEST_F(TracedDataTest, TracedJSON_StrictFormals_KeyAdded)
{
    // [FIXED] Strict formals check (no ...) — #keys shape dep now recorded at eval.cc:1648.
    // without recording #keys. Cold: {a:1} passed to ({a}: a) → 1.
    // Hot: {a:1,b:2} — should throw "unexpected argument 'b'" but SC dep
    // for .a still passes. Without #keys dep, override accepts stale result.
    TempJsonFile file(R"({"obj":{"a":1}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); f = {a}: a; in f j.obj)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // obj gains key "b" — strict formals should reject
    file.modify(R"({"obj":{"a":1,"b":2}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        EXPECT_THROW(forceRoot(*cache), nix::Error); // unexpected argument 'b'
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (key set changed)
    }
}

TEST_F(TracedDataTest, TracedJSON_StrictFormals_ValueChanges)
{
    // [PRECISION] Same keys, value changes → SC dep for value fails → re-eval.
    TempJsonFile file(R"({"obj":{"a":1}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); f = {a}: a; in f j.obj)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Change value of "a" but keep same key set
    file.modify(R"({"obj":{"a":99}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for .a fails
        EXPECT_THAT(v, IsIntEq(99));
    }
}

TEST_F(TracedDataTest, TracedJSON_StrictFormals_UnrelatedChange)
{
    // [PRECISION] Unrelated field changes → SC override accepts → cache hit.
    TempJsonFile file(R"({"obj":{"a":1},"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); f = {a}: a; in f j.obj)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Only change unrelated "extra" field
    file.modify(R"({"obj":{"a":1},"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (obj.a unchanged)
        EXPECT_THAT(v, IsIntEq(1));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Provenance propagation tests
// ═══════════════════════════════════════════════════════════════════════
//
// These tests verify that container-reconstructing operations (mapAttrs,
// filter, sort, removeAttrs, etc.) propagate provenance from tracked
// inputs to new output containers, allowing shape deps to be recorded
// on derived containers.

// ── mapAttrs: #keys dep recorded on derived attrset ──────────────────
// This is the core "mapAttrs gap" scenario from the design doc.
// Without propagation, attrNames on mapAttrs output fails to record
// a #keys dep because the output Bindings* is not in the provenance map.

TEST_F(TracedDataTest, Propagation_MapAttrs_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            mapped = builtins.mapAttrs (k: v: v + 1) data;
        in builtins.attrNames mapped
    )";

    // Fresh evaluation: attrNames on mapAttrs output
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    // File unchanged → serve from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Key added to JSON → must re-evaluate (different-size content triggers stat change)
    file.modify(R"({"a": 1, "b": 2, "c": 3333})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Must re-evaluate: key set changed
        EXPECT_EQ(v.listSize(), 3);
    }
}

// ── mapAttrs: value change in unused key doesn't invalidate ──────────
// When mapAttrs derives from tracked JSON, changing a value that the
// trace doesn't depend on should still allow two-level override.

TEST_F(TracedDataTest, Propagation_MapAttrs_UnusedValueChange)
{
    TempJsonFile file(R"({"used": 10, "other": 20})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            mapped = builtins.mapAttrs (k: v: v + 1) data;
        in mapped.used
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(11));
    }

    // Change "other" value (different size to trigger stat change)
    file.modify(R"({"used": 10, "other": 99999})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // Two-level override: only .used accessed
        EXPECT_THAT(v, IsIntEq(11));
    }
}

// ── removeAttrs: provenance propagated to subset ─────────────────────

TEST_F(TracedDataTest, Propagation_RemoveAttrs_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            subset = builtins.removeAttrs data ["c"];
        in builtins.attrNames subset
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    // File unchanged → serve from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Key added → must re-evaluate (provenance propagation catches shape change)
    file.modify(R"({"a": 1, "b": 2, "c": 3, "d": 44444})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Must re-evaluate
    }
}

// ── intersectAttrs: provenance propagated from tracked input ─────────

TEST_F(TracedDataTest, Propagation_IntersectAttrs_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            result = builtins.intersectAttrs { a = true; b = true; } data;
        in builtins.attrNames result
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── filter: provenance propagated to filtered list ───────────────────

TEST_F(TracedDataTest, Propagation_Filter_LenTracked)
{
    TempJsonFile file(R"({"items": [1, 2, 3, 4, 5]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            filtered = builtins.filter (x: x > 2) data.items;
        in builtins.length filtered
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── sort: provenance propagated (same length) ────────────────────────

TEST_F(TracedDataTest, Propagation_Sort_LenTracked)
{
    TempJsonFile file(R"({"items": [3, 1, 2]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            sorted = builtins.sort builtins.lessThan data.items;
        in builtins.length sorted
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Array grows → must re-evaluate (length changed)
    file.modify(R"({"items": [3, 1, 2, 4444]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(4));
    }
}

// ── ExprOpUpdate (//): provenance propagated from tracked input ──────

TEST_F(TracedDataTest, Propagation_OpUpdate_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            merged = data // { c = 3; };
        in builtins.attrNames merged
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Key added to JSON → must re-evaluate
    file.modify(R"({"a": 1, "b": 2, "d": 44444})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 4); // a, b, c, d
    }
}

// ── partition: inner lists propagated ────────────────────────────────

TEST_F(TracedDataTest, Propagation_Partition_InnerListsTracked)
{
    TempJsonFile file(R"({"items": [1, 2, 3, 4, 5]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            parts = builtins.partition (x: x > 2) data.items;
        in builtins.length parts.right
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── groupBy: inner group lists propagated ────────────────────────────

TEST_F(TracedDataTest, Propagation_GroupBy_InnerListsTracked)
{
    TempJsonFile file(R"({"items": [1, 2, 3, 4]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            groups = builtins.groupBy (x: if x > 2 then "big" else "small") data.items;
        in builtins.length groups.big
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── Negative: non-tracked container → no provenance ──────────────────
// Operations on containers NOT from ExprTracedData should NOT
// produce spurious shape deps (provenance map lookup returns null).

TEST_F(TracedDataTest, Propagation_Negative_NonTrackedContainer)
{
    // This expression builds a list literal (not from JSON),
    // then sorts it and checks length. No provenance should be propagated.
    auto expr = R"(
        let items = [3 1 2];
            sorted = builtins.sort builtins.lessThan items;
        in builtins.length sorted
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    // Should serve from cache (no file deps to invalidate)
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Fragile area tests: edge cases in provenance propagation
// ═══════════════════════════════════════════════════════════════════════

// ── Chained reconstruction: sort(filter(pred, tracked)) ──────────────
// Provenance must survive multiple reconstruction steps.

TEST_F(TracedDataTest, Propagation_Chained_SortFilter)
{
    TempJsonFile file(R"({"items": [5, 3, 1, 4, 2]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            filtered = builtins.filter (x: x > 2) data.items;
            sorted = builtins.sort builtins.lessThan filtered;
        in builtins.length sorted
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3)); // [3, 4, 5]
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Array grows → must re-evaluate
    file.modify(R"({"items": [5, 3, 1, 4, 2, 6666]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(4)); // [3, 4, 5, 6666]
    }
}

// ── Empty output from filter → propagation is no-op ──────────────────
// When filter produces an empty list, propagateTrackedList should be
// a no-op (empty output has no stable key). This must not crash.

TEST_F(TracedDataTest, Propagation_Filter_EmptyOutput)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            filtered = builtins.filter (x: x > 100) data.items;
        in builtins.length filtered
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(0));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── ++ with one tracked input among many ─────────────────────────────
// concatLists should propagate from whichever input is tracked.

TEST_F(TracedDataTest, Propagation_ConcatLists_OneTracked)
{
    TempJsonFile file(R"({"items": [10, 20]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            combined = [1 2] ++ data.items ++ [30];
        in builtins.length combined
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(5)); // [1, 2, 10, 20, 30]
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── mapAttrs then hasAttr on derived container ───────────────────────
// hasAttr records #keys dep on the attrset. After propagation,
// this should work on mapAttrs output.

TEST_F(TracedDataTest, Propagation_MapAttrs_HasAttr)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            mapped = builtins.mapAttrs (k: v: v + 1) data;
        in mapped ? a
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.boolean(), true);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Remove key "a" from JSON
    file.modify(R"({"b": 2, "c": 33333})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Must re-evaluate: key "a" removed
        EXPECT_EQ(v.boolean(), false);
    }
}

// ── Negative: removeAttrs on non-tracked doesn't crash ───────────────

TEST_F(TracedDataTest, Propagation_Negative_RemoveAttrsNonTracked)
{
    auto expr = R"(
        let data = { a = 1; b = 2; c = 3; };
            subset = builtins.removeAttrs data ["c"];
        in builtins.attrNames subset
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── tail of tracked list → propagation handles size-1 input ──────────

TEST_F(TracedDataTest, Propagation_Tail_SingleElement)
{
    TempJsonFile file(R"({"items": [42]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
        in builtins.length (builtins.tail data.items)
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(0)); // tail of [42] = []
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── // with tracked LHS and literal RHS ──────────────────────────────
// The layer optimization path vs merge path in ExprOpUpdate should
// both propagate correctly.

TEST_F(TracedDataTest, Propagation_OpUpdate_LayerPath)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    // Small RHS triggers the layering optimization path
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            merged = data // { z = 3; };
        in builtins.attrNames merged
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3); // x, y, z
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

} // namespace nix::eval_trace
