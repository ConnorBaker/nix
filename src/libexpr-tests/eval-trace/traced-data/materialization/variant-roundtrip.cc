/**
 * attrs_t serialization round-trip tests.
 * Tests that origin fields survive the record/verify cycle and that
 * materialized values carry correct origins.
 *
 * Uses the record() + verify() API (public) to test serialization,
 * rather than calling encodeCachedResult/decodeCachedResult (private).
 */
#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

namespace nix::eval_trace::test {

// ── TraceStore-level round-trip tests ──────────────────────────────

TEST_F(TraceStoreTest, Attrs_RecordVerifyRoundtrip)
{
    auto db = makeDb();

    attrs_t original;
    original.names = {
        state.symbols.create("alpha"),
        state.symbols.create("beta"),
    };
    original.origins.push_back({"input1", "file.json", "", 'j'});
    original.originIndices = {0, 0};

    db.record("test_attr", CachedResult(original), {}, true);

    auto result = db.verify("test_attr", {}, state);
    ASSERT_TRUE(result.has_value());

    auto * decoded = std::get_if<attrs_t>(&result->value);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(decoded->names.size(), 2u);
    EXPECT_EQ(std::string_view(state.symbols[decoded->names[0]]), "alpha");
    EXPECT_EQ(std::string_view(state.symbols[decoded->names[1]]), "beta");

    ASSERT_EQ(decoded->origins.size(), 1u);
    EXPECT_EQ(decoded->origins[0].depSource, "input1");
    EXPECT_EQ(decoded->origins[0].depKey, "file.json");
    EXPECT_EQ(decoded->origins[0].dataPath, "");
    EXPECT_EQ(decoded->origins[0].format, 'j');

    ASSERT_EQ(decoded->originIndices.size(), 2u);
    EXPECT_EQ(decoded->originIndices[0], 0);
    EXPECT_EQ(decoded->originIndices[1], 0);
}

TEST_F(TraceStoreTest, Attrs_NonTracedData_NoOrigins)
{
    auto db = makeDb();

    attrs_t original;
    original.names = {
        state.symbols.create("x"),
        state.symbols.create("y"),
    };
    // No origins set — plain Nix attrset

    db.record("plain_attr", CachedResult(original), {}, true);

    auto result = db.verify("plain_attr", {}, state);
    ASSERT_TRUE(result.has_value());

    auto * decoded = std::get_if<attrs_t>(&result->value);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(decoded->names.size(), 2u);
    EXPECT_TRUE(decoded->origins.empty());
    EXPECT_TRUE(decoded->originIndices.empty());
}

TEST_F(TraceStoreTest, Attrs_MultiOrigin_PreservesPerAttrMapping)
{
    auto db = makeDb();

    attrs_t original;
    original.names = {
        state.symbols.create("a"),
        state.symbols.create("b"),
        state.symbols.create("c"),
    };
    original.origins.push_back({"input1", "f1.json", "", 'j'});
    original.origins.push_back({"input2", "f2.json", "", 'j'});
    original.originIndices = {0, 1, 0}; // a->origin0, b->origin1, c->origin0

    db.record("multi_attr", CachedResult(original), {}, true);

    auto result = db.verify("multi_attr", {}, state);
    ASSERT_TRUE(result.has_value());

    auto * decoded = std::get_if<attrs_t>(&result->value);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(decoded->origins.size(), 2u);
    EXPECT_EQ(decoded->origins[0].depKey, "f1.json");
    EXPECT_EQ(decoded->origins[1].depKey, "f2.json");
    ASSERT_EQ(decoded->originIndices.size(), 3u);
    EXPECT_EQ(decoded->originIndices[0], 0);
    EXPECT_EQ(decoded->originIndices[1], 1);
    EXPECT_EQ(decoded->originIndices[2], 0);
}

TEST_F(TraceStoreTest, Attrs_MixedOrigin_NixAddedAttrsMinusOne)
{
    auto db = makeDb();

    attrs_t original;
    original.names = {
        state.symbols.create("a"),
        state.symbols.create("extra"),
    };
    original.origins.push_back({"input1", "f1.json", "", 'j'});
    original.originIndices = {0, -1}; // a->origin0, extra->no origin (Nix-added)

    db.record("mixed_attr", CachedResult(original), {}, true);

    auto result = db.verify("mixed_attr", {}, state);
    ASSERT_TRUE(result.has_value());

    auto * decoded = std::get_if<attrs_t>(&result->value);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(decoded->origins.size(), 1u);
    ASSERT_EQ(decoded->originIndices.size(), 2u);
    EXPECT_EQ(decoded->originIndices[0], 0);
    EXPECT_EQ(decoded->originIndices[1], -1);
}

TEST_F(TraceStoreTest, Attrs_EmptyObject_NoOrigins)
{
    auto db = makeDb();

    attrs_t original;
    // Empty names, no origins

    db.record("empty_attr", CachedResult(original), {}, true);

    auto result = db.verify("empty_attr", {}, state);
    ASSERT_TRUE(result.has_value());

    auto * decoded = std::get_if<attrs_t>(&result->value);
    ASSERT_NE(decoded, nullptr);
    EXPECT_TRUE(decoded->names.empty());
    EXPECT_TRUE(decoded->origins.empty());
    EXPECT_TRUE(decoded->originIndices.empty());
}

// ── End-to-end materialization round-trip ───────────────────────────

TEST_F(MaterializationDepTest, Attrs_StoredResult_HasOrigins)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto expr = std::format(
        "let d = {}; in {{ inherit d; names = builtins.attrNames d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        // Force the 'd' child
        auto * dAttr = root.attrs()->get(state.symbols.create("d"));
        ASSERT_NE(dAttr, nullptr);
        state.forceValue(*dAttr->value, noPos);
    }

    // Query the stored result for 'd'
    auto result = getStoredResult("d");
    ASSERT_TRUE(result.has_value());

    auto * attrs = std::get_if<attrs_t>(& *result);
    ASSERT_NE(attrs, nullptr) << "Stored result should be attrs_t";
    EXPECT_EQ(attrs->names.size(), 2u);
    EXPECT_FALSE(attrs->origins.empty()) << "TracedData attrset should have origins";
    EXPECT_EQ(attrs->origins.size(), 1u);
    EXPECT_EQ(attrs->origins[0].format, 'j');
    EXPECT_EQ(attrs->originIndices.size(), 2u);
    EXPECT_EQ(attrs->originIndices[0], 0);
    EXPECT_EQ(attrs->originIndices[1], 0);
}

} // namespace nix::eval_trace::test
