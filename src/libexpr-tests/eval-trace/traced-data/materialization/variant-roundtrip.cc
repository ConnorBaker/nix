/**
 * attrs_t serialization round-trip tests.
 * Tests that origin fields survive the record/verify cycle and that
 * materialized values carry correct origins.
 *
 * Uses the record() + verify() API (public) to test serialization,
 * rather than calling encodeCachedResult/decodeCachedResult (private).
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"

#include <format>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace nix::eval_trace::test {

// ── SqliteTraceStorage-level round-trip tests ──────────────────────────────

TEST_F(TraceStoreTest, Attrs_OriginFields_RecordVerifyRoundtrip)
{
    auto db = makeDb();

    attrs_t original;
    original.entries = {
        CachedAttrEntry{state.symbols.create("alpha"), StructuredObject{DepSource::fromNodeKey("input1"), "file.json", {}, StructuredFormat::Json}, invalidSiblingIndex},
        CachedAttrEntry{state.symbols.create("beta"), StructuredObject{DepSource::fromNodeKey("input1"), "file.json", {}, StructuredFormat::Json}, invalidSiblingIndex},
    };

    withExclusiveStore(*db, [&](const auto & ea) { db->record(ea, vpath({"test_attr"}), CachedResult(original), {}); });

    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"test_attr"}), state);
    ASSERT_TRUE(result.has_value());

    auto * decoded = std::get_if<attrs_t>(&result->value);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(decoded->entries.size(), 2u);
    EXPECT_EQ(std::string_view(state.symbols[decoded->entries[0].name]), "alpha");
    EXPECT_EQ(std::string_view(state.symbols[decoded->entries[1].name]), "beta");

    ASSERT_TRUE(decoded->entries[0].producerOrigin.has_value());
    ASSERT_TRUE(decoded->entries[1].producerOrigin.has_value());
    auto & origin = *decoded->entries[0].producerOrigin;
    EXPECT_EQ(origin.source, DepSource::fromNodeKey("input1"));
    EXPECT_EQ(origin.key, "file.json");
    EXPECT_TRUE(origin.dataPath.empty());
    EXPECT_EQ(origin.format, StructuredFormat::Json);
    EXPECT_EQ(decoded->entries[1].producerOrigin->key, "file.json");
}

TEST_F(TraceStoreTest, Attrs_NonTracedData_NoOrigins)
{
    auto db = makeDb();

    attrs_t original;
    original.entries = {
        CachedAttrEntry{state.symbols.create("x")},
        CachedAttrEntry{state.symbols.create("y")},
    };
    // No origins set — plain Nix attrset

    withExclusiveStore(*db, [&](const auto & ea) { db->record(ea, vpath({"plain_attr"}), CachedResult(original), {}); });

    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"plain_attr"}), state);
    ASSERT_TRUE(result.has_value());

    auto * decoded = std::get_if<attrs_t>(&result->value);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(decoded->entries.size(), 2u);
    EXPECT_FALSE(decoded->entries[0].producerOrigin.has_value());
    EXPECT_FALSE(decoded->entries[1].producerOrigin.has_value());
}

TEST_F(TraceStoreTest, Attrs_MultiOrigin_PreservesPerAttrMapping)
{
    auto db = makeDb();

    attrs_t original;
    original.entries = {
        CachedAttrEntry{state.symbols.create("a"), StructuredObject{DepSource::fromNodeKey("input1"), "f1.json", {}, StructuredFormat::Json}, invalidSiblingIndex},
        CachedAttrEntry{state.symbols.create("b"), StructuredObject{DepSource::fromNodeKey("input2"), "f2.json", {}, StructuredFormat::Json}, invalidSiblingIndex},
        CachedAttrEntry{state.symbols.create("c"), StructuredObject{DepSource::fromNodeKey("input1"), "f1.json", {}, StructuredFormat::Json}, invalidSiblingIndex},
    };

    withExclusiveStore(*db, [&](const auto & ea) { db->record(ea, vpath({"multi_attr"}), CachedResult(original), {}); });

    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"multi_attr"}), state);
    ASSERT_TRUE(result.has_value());

    auto * decoded = std::get_if<attrs_t>(&result->value);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(decoded->entries.size(), 3u);
    EXPECT_EQ(decoded->entries[0].producerOrigin->key, "f1.json");
    EXPECT_EQ(decoded->entries[1].producerOrigin->key, "f2.json");
    EXPECT_EQ(decoded->entries[2].producerOrigin->key, "f1.json");
}

TEST_F(TraceStoreTest, Attrs_MixedOrigin_NixAddedAttrsMinusOne)
{
    auto db = makeDb();

    attrs_t original;
    original.entries = {
        CachedAttrEntry{state.symbols.create("a"), StructuredObject{DepSource::fromNodeKey("input1"), "f1.json", {}, StructuredFormat::Json}, invalidSiblingIndex},
        CachedAttrEntry{state.symbols.create("extra"), std::nullopt, invalidSiblingIndex},
    };

    withExclusiveStore(*db, [&](const auto & ea) { db->record(ea, vpath({"mixed_attr"}), CachedResult(original), {}); });

    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"mixed_attr"}), state);
    ASSERT_TRUE(result.has_value());

    auto * decoded = std::get_if<attrs_t>(&result->value);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(decoded->entries.size(), 2u);
    ASSERT_TRUE(decoded->entries[0].producerOrigin.has_value());
    EXPECT_FALSE(decoded->entries[1].producerOrigin.has_value());
}

TEST_F(TraceStoreTest, Attrs_EmptyObject_NoOrigins)
{
    auto db = makeDb();

    attrs_t original;
    // Empty names, no origins

    withExclusiveStore(*db, [&](const auto & ea) { db->record(ea, vpath({"empty_attr"}), CachedResult(original), {}); });

    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"empty_attr"}), state);
    ASSERT_TRUE(result.has_value());

    auto * decoded = std::get_if<attrs_t>(&result->value);
    ASSERT_NE(decoded, nullptr);
    EXPECT_TRUE(decoded->entries.empty());
}

TEST_F(TraceStoreTest, Attrs_EmptyDepSource_PayloadRejected)
{
    auto db = makeDb();

    attrs_t original;
    original.entries = {
        CachedAttrEntry{
            state.symbols.create("alpha"),
            StructuredObject{
                DepSource::fromNodeKey("input1"),
                "file.json",
                {},
                StructuredFormat::Json,
            },
            invalidSiblingIndex,
        },
    };

    auto encoded =
        test::TraceStorageTestAccess::encodeCachedResult(*db, CachedResult(original));
    auto payloadJson = nlohmann::json::parse(encoded.payload);
    payloadJson["entries"][0]["o"]["s"] = "";

    EXPECT_THROW(
        (void) test::TraceStorageTestAccess::decodeCachedResult(
            *db,
            SqliteTraceStorage::ResultPayload{
                .type = encoded.type,
                .encodingVersion = encoded.encodingVersion,
                .payload = payloadJson.dump(),
                .auxContext = encoded.auxContext,
            }),
        Error);
}

TEST_F(TraceStoreTest, String_TextObject_Roundtrip)
{
    auto db = makeDb();

    string_t original{
        .first = R"({"name":"demo"})",
        .second = {},
        .publication = SemanticHandle{
            .path = PathObject{
                .source = DepSource::fromRuntimeRoot(runtimeRootSourceKeyFromDebugString("path:/tmp/runtime?narHash=sha256-abc")),
                .rootPath = CanonPath("/nix/store/runtime-root"),
            },
            .text = TextObject{
                .source = DepSource::fromNodeKey("input1"),
                .key = "/flake.lock",
                .contentHash = depHash(R"({"name":"demo"})"),
            },
        },
    };

    auto encoded =
        test::TraceStorageTestAccess::encodeCachedResult(*db, CachedResult(original));

    auto decoded = test::TraceStorageTestAccess::decodeCachedResult(
        *db,
        SqliteTraceStorage::ResultPayload{
            .type = encoded.type,
            .encodingVersion = encoded.encodingVersion,
            .payload = encoded.payload,
            .auxContext = encoded.auxContext,
        });

    assertCachedResultEquals(CachedResult(original), decoded, state.symbols);
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
    EXPECT_EQ(attrs->entries.size(), 2u);
    ASSERT_TRUE(attrs->entries[0].producerOrigin.has_value());
    ASSERT_TRUE(attrs->entries[1].producerOrigin.has_value());
    EXPECT_EQ(attrs->entries[0].producerOrigin->format, StructuredFormat::Json);
    EXPECT_EQ(attrs->entries[1].producerOrigin->format, StructuredFormat::Json);
}

TEST_F(MaterializationDepTest, UpdateAlias_LeftEmpty_DoesNotAttachObservedLhsProvenanceToPlainRhs)
{
    TempJsonFile f(R"({"empty":{}})");
    auto expr = std::format(
        "let j = {}; d = j.empty // {{ a = 1; }}; in {{ inherit d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * dAttr = root.attrs()->get(state.symbols.create("d"));
        ASSERT_NE(dAttr, nullptr);
        state.forceValue(*dAttr->value, noPos);
    }

    auto result = getStoredResult("d");
    ASSERT_TRUE(result.has_value());
    auto * attrs = std::get_if<attrs_t>(&*result);
    ASSERT_NE(attrs, nullptr);
    EXPECT_EQ(attrs->entries.size(), 1u);
    EXPECT_FALSE(attrs->entries[0].producerOrigin.has_value());
    EXPECT_FALSE(attrs->meta && attrs->meta->producerOrigin.has_value())
        << "the returned plain RHS must not inherit provenance from the observed empty LHS";
}

TEST_F(MaterializationDepTest, UpdateAlias_RightEmpty_DoesNotAttachObservedRhsProvenanceToPlainLhs)
{
    TempJsonFile f(R"({"empty":{}})");
    auto expr = std::format(
        "let j = {}; d = {{ a = 1; }} // j.empty; in {{ inherit d; }}",
        fj(f.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        auto * dAttr = root.attrs()->get(state.symbols.create("d"));
        ASSERT_NE(dAttr, nullptr);
        state.forceValue(*dAttr->value, noPos);
    }

    auto result = getStoredResult("d");
    ASSERT_TRUE(result.has_value());
    auto * attrs = std::get_if<attrs_t>(&*result);
    ASSERT_NE(attrs, nullptr);
    EXPECT_EQ(attrs->entries.size(), 1u);
    EXPECT_FALSE(attrs->entries[0].producerOrigin.has_value());
    EXPECT_FALSE(attrs->meta && attrs->meta->producerOrigin.has_value())
        << "the returned plain LHS must not inherit provenance from the observed empty RHS";
}

} // namespace nix::eval_trace::test
