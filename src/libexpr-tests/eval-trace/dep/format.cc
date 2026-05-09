#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/source-path.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;


// ── StructuredFormat enum tests ───────────────────────────────────────

TEST(DepFormatTest, Format_StructuredFormatChar_Roundtrip)
{
    EXPECT_EQ(structuredFormatChar(StructuredFormat::Json), 'j');
    EXPECT_EQ(structuredFormatChar(StructuredFormat::Toml), 't');
    EXPECT_EQ(structuredFormatChar(StructuredFormat::Directory), 'd');
    EXPECT_EQ(structuredFormatChar(StructuredFormat::Nix), 'n');
}

TEST(DepFormatTest, Format_ParseStructuredFormat_ValidCharsAccepted)
{
    EXPECT_EQ(parseStructuredFormat('j'), StructuredFormat::Json);
    EXPECT_EQ(parseStructuredFormat('t'), StructuredFormat::Toml);
    EXPECT_EQ(parseStructuredFormat('d'), StructuredFormat::Directory);
    EXPECT_EQ(parseStructuredFormat('n'), StructuredFormat::Nix);
}

TEST(DepFormatTest, Format_ParseStructuredFormat_InvalidCharsRejected)
{
    EXPECT_EQ(parseStructuredFormat('x'), std::nullopt);
    EXPECT_EQ(parseStructuredFormat('J'), std::nullopt);
    EXPECT_EQ(parseStructuredFormat('\0'), std::nullopt);
}

TEST(DepFormatTest, Format_StructuredFormatName_AllFormatsNamed)
{
    EXPECT_EQ(structuredFormatName(StructuredFormat::Json), "json");
    EXPECT_EQ(structuredFormatName(StructuredFormat::Toml), "toml");
    EXPECT_EQ(structuredFormatName(StructuredFormat::Directory), "directory");
    EXPECT_EQ(structuredFormatName(StructuredFormat::Nix), "nix");
}

// ── ShapeSuffix enum tests ────────────────────────────────────────────

TEST(DepFormatTest, Format_ShapeSuffixName_AllValues)
{
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::None), "");
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::Len), "len");
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::Keys), "keys");
}

// ── Tagged type safety tests ────────────────────────────────────────

TEST(DepFormatTest, Format_TaggedType_DefaultZero)
{
    DepSourceId src{};
    FilePathId fp{};
    DataPathId dp{};
    EXPECT_EQ(src.value, 0u);
    EXPECT_EQ(fp.value, 0u);
    EXPECT_EQ(dp.value, 0u);
    EXPECT_FALSE(static_cast<bool>(src));
    EXPECT_FALSE(static_cast<bool>(fp));
    EXPECT_FALSE(static_cast<bool>(dp));
}

TEST(DepFormatTest, Format_TaggedType_NonZeroIsTrue)
{
    DepSourceId src(1);
    DataPathId dp(42);
    EXPECT_TRUE(static_cast<bool>(src));
    EXPECT_TRUE(static_cast<bool>(dp));
}

TEST(DepFormatTest, Format_TaggedType_EqualityAndOrdering)
{
    DepSourceId a(1), b(1), c(2);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_LT(a, c);
}

} // namespace nix::eval_trace
