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

TEST_F(DependencyTrackerTest, ShapeSuffixName_AllValues)
{
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::None), "");
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::Len), "len");
    EXPECT_EQ(shapeSuffixName(ShapeSuffix::Keys), "keys");
}

// ── StrongId type safety tests ────────────────────────────────────────

TEST_F(DependencyTrackerTest, StrongId_DefaultZero)
{
    DepSourceId src{};
    FilePathId fp{};
    DataPathId dp{};
    EXPECT_EQ(src.value, 0);
    EXPECT_EQ(fp.value, 0);
    EXPECT_EQ(dp.value, 0);
    EXPECT_FALSE(static_cast<bool>(src));
    EXPECT_FALSE(static_cast<bool>(fp));
    EXPECT_FALSE(static_cast<bool>(dp));
}

TEST_F(DependencyTrackerTest, StrongId_NonZeroIsTrue)
{
    DepSourceId src(1);
    DataPathId dp(42);
    EXPECT_TRUE(static_cast<bool>(src));
    EXPECT_TRUE(static_cast<bool>(dp));
}

TEST_F(DependencyTrackerTest, StrongId_EqualityAndOrdering)
{
    DepSourceId a(1), b(1), c(2);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_LT(a, c);
}

} // namespace nix::eval_trace
