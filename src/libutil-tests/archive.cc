#include "nix/util/archive.hh"
#include "nix/util/tests/characterization.hh"
#include "nix/util/tests/gmock-matchers.hh"
#include "nix/util/tests/setting-scopes.hh"

#include <cctype>

#include <gtest/gtest.h>

namespace nix {

namespace {

class NarTest : public CharacterizationTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "nars";

public:
    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / (std::string(testStem) + ".nar");
    }
};

class InvalidNarTest : public NarTest, public ::testing::WithParamInterface<std::tuple<std::string, std::string>>
{};

enum class NarContentMode {
    Skip,
    Drain,
};

struct DrainingFileSystemObjectSink : NullFileSystemObjectSink
{
    void createRegularFile(const CanonPath &, fun<void(CreateRegularFileSink &)> func) override
    {
        struct : CreateRegularFileSink
        {
            void operator()(std::string_view) override {}

            void isExecutable() override {}
        } sink;

        func(sink);
    }
};

size_t parseNar(std::string_view narContents, NarContentMode mode)
{
    StringSource source{narContents};

    if (mode == NarContentMode::Skip) {
        NullFileSystemObjectSink sink;
        parseDump(sink, source);
    } else {
        DrainingFileSystemObjectSink sink;
        parseDump(sink, source);
    }

    return source.pos;
}

} // namespace

TEST_P(InvalidNarTest, throwsErrorMessage)
{
    const auto & [name, message] = GetParam();
    readTest(name, [&](const std::string & narContents) {
        ASSERT_THAT(
            [&]() {
                StringSource source{narContents};
                NullFileSystemObjectSink sink;
                parseDump(sink, source);
            },
            ::testing::ThrowsMessage<SerialisationError>(testing::HasSubstrIgnoreANSIMatcher(message)));
    });
}

INSTANTIATE_TEST_SUITE_P(
    NarTest,
    InvalidNarTest,
    ::testing::Values(
        std::pair{"invalid-tag-instead-of-contents", "bad archive: expected tag 'contents', got 'AAAAAAAA'"},
        // Unpacking a NAR with a NUL character in a file name should fail.
        std::pair{"nul-character", "bad archive: NAR contains invalid file name 'f"},
        // Likewise for a '.' filename.
        std::pair{"dot", "bad archive: NAR contains invalid file name '.'"},
        // Likewise for a '..' filename.
        std::pair{"dotdot", "bad archive: NAR contains invalid file name '..'"},
        // Likewise for a filename containing a slash.
        std::pair{"slash", "bad archive: NAR contains invalid file name 'x/y'"},
        // Likewise for an empty filename.
        std::pair{"empty", "bad archive: NAR contains invalid file name ''"},
        // Test that the 'executable' field cannot come before the 'contents' field.
        std::pair{"executable-after-contents", "bad archive: expected tag ')', got 'executable'"},
        // Test that the 'name' field cannot come before the 'node' field in a directory entry.
        std::pair{"name-after-node", "bad archive: expected tag 'name'"}));

TEST_F(NarTest, oneByteRegularFileParsesWithBothContentModes)
{
    readTest("regular-one-byte-zero-padding", [&](const std::string & narContents) {
        auto skippingOffset = parseNar(narContents, NarContentMode::Skip);
        auto drainingOffset = parseNar(narContents, NarContentMode::Drain);

        EXPECT_EQ(skippingOffset, narContents.size());
        EXPECT_EQ(drainingOffset, skippingOffset);
    });
}

TEST_F(NarTest, nonZeroContentPaddingFailsWithBothContentModes)
{
    readTest("regular-one-byte-non-zero-padding", [&](const std::string & narContents) {
        for (auto mode : {NarContentMode::Skip, NarContentMode::Drain}) {
            ASSERT_THAT(
                [&]() { parseNar(narContents, mode); },
                ::testing::ThrowsMessage<SerialisationError>(testing::HasSubstrIgnoreANSIMatcher("non-zero padding")));
        }
    });
}

TEST_F(NarTest, truncatedLargeContentsFailsWithBothContentModes)
{
    readTest("regular-truncated-large-contents", [&](const std::string & narContents) {
        for (auto mode : {NarContentMode::Skip, NarContentMode::Drain})
            EXPECT_THROW(parseNar(narContents, mode), EndOfFile);
    });
}

TEST_F(NarTest, truncatedContentPaddingFailsWithBothContentModes)
{
    readTest("regular-truncated-padding", [&](const std::string & narContents) {
        for (auto mode : {NarContentMode::Skip, NarContentMode::Drain})
            EXPECT_THROW(parseNar(narContents, mode), EndOfFile);
    });
}

/* The case-hack suffix is the parser's own marker on a case-colliding
   entry, removed by `unhackName` where `use-case-hack` is on (Apple's
   default) and kept elsewhere, so a NAR naming an entry with it would name
   two trees on two platforms.  The parser refuses it under either setting;
   the same NAR under a plain name parses. */
TEST_F(NarTest, entryNameWithCaseHackSuffixIsRefusedUnderEitherSetting)
{
    auto narWithEntry = [](std::string_view name) {
        StringSink nar;
        nar << narVersionMagic1 << "(" << "type" << "directory" << "entry" << "(" << "name" << name << "node" << "("
            << "type" << "regular" << "contents" << "" << ")" << ")" << ")";
        return std::move(nar.s);
    };
    auto hacked = "foo" + std::string(caseHackSuffix) + "1";

    for (bool hack : {false, true}) {
        WithCaseHack scope{hack};
        for (auto mode : {NarContentMode::Skip, NarContentMode::Drain}) {
            ASSERT_THAT(
                [&]() { parseNar(narWithEntry(hacked), mode); },
                ::testing::ThrowsMessage<SerialisationError>(testing::HasSubstrIgnoreANSIMatcher(
                    "bad archive: NAR contains file name '" + hacked + "' with the case-hack suffix")));
            EXPECT_EQ(parseNar(narWithEntry("foo1"), mode), narWithEntry("foo1").size());
        }
    }
}

/* The parser's collision check is reachable: the suffix check is
   case-sensitive, as `unhackName` is, so an entry spelling the suffix in
   another case is admitted, and on a case-insensitive file system it is
   the file the hacked name would overwrite.  `Foo`, `Foo~NIX~CASE~HACK~1`,
   `foo`: under the hack `foo` becomes `foo~nix~case~hack~1`, the second
   entry's file; without the hack the three are three names. */
TEST_F(NarTest, hackedNameCollidingWithAnUpperCaseSuffixEntryIsRefused)
{
    std::string upper(caseHackSuffix);
    for (auto & c : upper)
        c = std::toupper(static_cast<unsigned char>(c));
    ASSERT_NE(upper, caseHackSuffix);
    auto upperEntry = "Foo" + upper + "1";

    StringSink nar;
    nar << narVersionMagic1 << "(" << "type" << "directory";
    for (std::string_view name : {std::string_view("Foo"), std::string_view(upperEntry), std::string_view("foo")})
        nar << "entry" << "(" << "name" << name << "node" << "(" << "type" << "regular" << "contents" << "" << ")"
            << ")";
    nar << ")";

    {
        WithCaseHack on{true};
        ASSERT_THAT(
            [&]() { parseNar(nar.s, NarContentMode::Skip); },
            ::testing::ThrowsMessage<SerialisationError>(testing::HasSubstrIgnoreANSIMatcher(
                "NAR contains file name 'foo' that collides with case-hacked file name '" + upperEntry + "'")));
    }
    {
        WithCaseHack off{false};
        EXPECT_EQ(parseNar(nar.s, NarContentMode::Skip), nar.s.size());
    }
}

TEST_F(NarTest, trailingBytesRemainUnreadWithBothContentModes)
{
    readTest("regular-one-byte-zero-padding", [&](const std::string & narContents) {
        auto narWithSentinel = narContents + "sentinel";
        auto skippingOffset = parseNar(narWithSentinel, NarContentMode::Skip);
        auto drainingOffset = parseNar(narWithSentinel, NarContentMode::Drain);

        EXPECT_EQ(skippingOffset, narContents.size());
        EXPECT_EQ(drainingOffset, skippingOffset);
    });
}

} // namespace nix
