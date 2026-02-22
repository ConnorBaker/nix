#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"
#include "nix/util/source-path.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DependencyTrackerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        DependencyTracker::clearSessionTraces();
    }

    void TearDown() override
    {
        DependencyTracker::clearSessionTraces();
    }
};

// ── StructuredFormat enum tests ───────────────────────────────────────

TEST_F(DependencyTrackerTest, StructuredFormatChar_Roundtrip)
{
    EXPECT_EQ(structuredFormatChar(StructuredFormat::Json), 'j');
    EXPECT_EQ(structuredFormatChar(StructuredFormat::Toml), 't');
    EXPECT_EQ(structuredFormatChar(StructuredFormat::Directory), 'd');
}

TEST_F(DependencyTrackerTest, ParseStructuredFormat_ValidChars)
{
    EXPECT_EQ(parseStructuredFormat('j'), StructuredFormat::Json);
    EXPECT_EQ(parseStructuredFormat('t'), StructuredFormat::Toml);
    EXPECT_EQ(parseStructuredFormat('d'), StructuredFormat::Directory);
}

TEST_F(DependencyTrackerTest, ParseStructuredFormat_InvalidChars)
{
    EXPECT_EQ(parseStructuredFormat('x'), std::nullopt);
    EXPECT_EQ(parseStructuredFormat('J'), std::nullopt);
    EXPECT_EQ(parseStructuredFormat('\0'), std::nullopt);
}

TEST_F(DependencyTrackerTest, StructuredFormatName_AllFormats)
{
    EXPECT_EQ(structuredFormatName(StructuredFormat::Json), "json");
    EXPECT_EQ(structuredFormatName(StructuredFormat::Toml), "toml");
    EXPECT_EQ(structuredFormatName(StructuredFormat::Directory), "directory");
}

// ── ShapeSuffix enum tests ────────────────────────────────────────────

TEST_F(DependencyTrackerTest, ShapeSuffixString_AllValues)
{
    EXPECT_EQ(shapeSuffixString(ShapeSuffix::None), "");
    EXPECT_EQ(shapeSuffixString(ShapeSuffix::Len), "#len");
    EXPECT_EQ(shapeSuffixString(ShapeSuffix::Keys), "#keys");
}

TEST_F(DependencyTrackerTest, ShapeSuffixName_AllValues)
{
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::None), "");
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::Len), "len");
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::Keys), "keys");
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_NoSuffix)
{
    auto [path, shape] = parseShapeSuffix(".foo.bar");
    EXPECT_EQ(path, ".foo.bar");
    EXPECT_EQ(shape, ShapeSuffix::None);
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_LenSuffix)
{
    auto [path, shape] = parseShapeSuffix(".foo.bar#len");
    EXPECT_EQ(path, ".foo.bar");
    EXPECT_EQ(shape, ShapeSuffix::Len);
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_KeysSuffix)
{
    auto [path, shape] = parseShapeSuffix(".foo.bar#keys");
    EXPECT_EQ(path, ".foo.bar");
    EXPECT_EQ(shape, ShapeSuffix::Keys);
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_EmptyPath)
{
    auto [path, shape] = parseShapeSuffix("#len");
    EXPECT_EQ(path, "");
    EXPECT_EQ(shape, ShapeSuffix::Len);
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_JustHashNotSuffix)
{
    auto [path, shape] = parseShapeSuffix(".foo#bar");
    EXPECT_EQ(path, ".foo#bar");
    EXPECT_EQ(shape, ShapeSuffix::None);
}

// ── buildStructuredDepKey tests ───────────────────────────────────────

TEST_F(DependencyTrackerTest, BuildStructuredDepKey_Scalar)
{
    auto key = buildStructuredDepKey("/file.json", StructuredFormat::Json, ".foo.bar");
    EXPECT_EQ(key, "/file.json\tj:.foo.bar");
}

TEST_F(DependencyTrackerTest, BuildStructuredDepKey_WithLen)
{
    auto key = buildStructuredDepKey("/file.toml", StructuredFormat::Toml, ".list", ShapeSuffix::Len);
    EXPECT_EQ(key, "/file.toml\tt:.list#len");
}

TEST_F(DependencyTrackerTest, BuildStructuredDepKey_WithKeys)
{
    auto key = buildStructuredDepKey("/dir", StructuredFormat::Directory, "", ShapeSuffix::Keys);
    EXPECT_EQ(key, "/dir\td:#keys");
}

// ── formatStructuredDepKey tests ──────────────────────────────────────

TEST_F(DependencyTrackerTest, FormatStructuredDepKey_JsonScalar)
{
    auto result = formatStructuredDepKey("/file.json\tj:.foo.bar");
    EXPECT_EQ(result, "/file.json [json] .foo.bar");
}

TEST_F(DependencyTrackerTest, FormatStructuredDepKey_TomlWithLen)
{
    auto result = formatStructuredDepKey("/f.toml\tt:.list#len");
    EXPECT_EQ(result, "/f.toml [toml] .list #len");
}

TEST_F(DependencyTrackerTest, FormatStructuredDepKey_DirWithKeys)
{
    auto result = formatStructuredDepKey("/dir\td:#keys");
    EXPECT_EQ(result, "/dir [directory]  #keys");
}

TEST_F(DependencyTrackerTest, FormatStructuredDepKey_InvalidFallback)
{
    EXPECT_EQ(formatStructuredDepKey("no-tab-here"), "no-tab-here");
    EXPECT_EQ(formatStructuredDepKey("a\tx:rest"), "a\tx:rest"); // invalid format char
    EXPECT_EQ(formatStructuredDepKey("a\tj"), "a\tj"); // too short (no colon)
}

// ── parseShapeSuffix edge cases ────────────────────────────────────────

TEST_F(DependencyTrackerTest, ParseShapeSuffix_TypeSuffix)
{
    auto [path, shape] = parseShapeSuffix(".foo#type");
    EXPECT_EQ(path, ".foo");
    EXPECT_EQ(shape, ShapeSuffix::Type);
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_HasKeyNotParsedAsShape)
{
    // #has:key is NOT a shape suffix — parseShapeSuffix returns None
    auto [path, shape] = parseShapeSuffix(".foo#has:bar");
    EXPECT_EQ(path, ".foo#has:bar");
    EXPECT_EQ(shape, ShapeSuffix::None);
}

TEST_F(DependencyTrackerTest, ParseShapeSuffix_HasKeyWithEscapedName)
{
    // Escaped key after #has: — parseShapeSuffix should not consume it
    auto [path, shape] = parseShapeSuffix(R"(#has:"key.name")");
    EXPECT_EQ(path, R"(#has:"key.name")");
    EXPECT_EQ(shape, ShapeSuffix::None);
}

// ── escapeDataPathKey / unescapeDataPathKey tests ─────────────────────

TEST_F(DependencyTrackerTest, EscapeDataPathKey_PlainKey)
{
    EXPECT_EQ(escapeDataPathKey("foo"), "foo");
    EXPECT_EQ(escapeDataPathKey("simple_key123"), "simple_key123");
}

TEST_F(DependencyTrackerTest, EscapeDataPathKey_KeyWithDot)
{
    EXPECT_EQ(escapeDataPathKey("key.name"), R"("key.name")");
}

TEST_F(DependencyTrackerTest, EscapeDataPathKey_KeyWithHash)
{
    // '#' triggers quoting — prevents collision with #len/#keys/#has:
    EXPECT_EQ(escapeDataPathKey("#has:foo"), R"("#has:foo")");
    EXPECT_EQ(escapeDataPathKey("x#len"), R"("x#len")");
}

TEST_F(DependencyTrackerTest, EscapeDataPathKey_KeyWithQuote)
{
    EXPECT_EQ(escapeDataPathKey(R"(key"name)"), R"("key\"name")");
}

TEST_F(DependencyTrackerTest, EscapeDataPathKey_KeyWithBackslash)
{
    EXPECT_EQ(escapeDataPathKey(R"(key\name)"), R"("key\\name")");
}

TEST_F(DependencyTrackerTest, EscapeDataPathKey_KeyWithBrackets)
{
    EXPECT_EQ(escapeDataPathKey("key[0]"), R"("key[0]")");
}

TEST_F(DependencyTrackerTest, EscapeDataPathKey_EmptyKey)
{
    EXPECT_EQ(escapeDataPathKey(""), "");
}

TEST_F(DependencyTrackerTest, UnescapeDataPathKey_PlainKey)
{
    EXPECT_EQ(unescapeDataPathKey("foo"), "foo");
}

TEST_F(DependencyTrackerTest, UnescapeDataPathKey_QuotedKey)
{
    EXPECT_EQ(unescapeDataPathKey(R"("key.name")"), "key.name");
}

TEST_F(DependencyTrackerTest, UnescapeDataPathKey_QuotedWithEscapes)
{
    EXPECT_EQ(unescapeDataPathKey(R"("key\"name")"), R"(key"name)");
    EXPECT_EQ(unescapeDataPathKey(R"("key\\name")"), R"(key\name)");
}

TEST_F(DependencyTrackerTest, EscapeUnescapeRoundtrip)
{
    // Roundtrip: unescape(escape(key)) == key for all keys
    std::vector<std::string> keys = {
        "simple", "key.name", "key[0]", R"(key"name)", R"(key\name)",
        "#has:foo", "x#len", "#keys", "", "a.b.c[1].d"
    };
    for (auto & key : keys) {
        EXPECT_EQ(unescapeDataPathKey(escapeDataPathKey(key)), key)
            << "Roundtrip failed for key: " << key;
    }
}

// ── buildStructuredDepKey raw suffix overload ─────────────────────────

TEST_F(DependencyTrackerTest, BuildStructuredDepKey_RawSuffix)
{
    auto key = buildStructuredDepKey("/file.json", StructuredFormat::Json, ".obj", "#has:x");
    EXPECT_EQ(key, "/file.json\tj:.obj#has:x");
}

TEST_F(DependencyTrackerTest, BuildStructuredDepKey_RawSuffixEmptyPath)
{
    auto key = buildStructuredDepKey("/file.json", StructuredFormat::Json, "", "#has:x");
    EXPECT_EQ(key, "/file.json\tj:#has:x");
}

} // namespace nix::eval_trace
