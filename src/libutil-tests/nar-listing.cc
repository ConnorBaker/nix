#include <string_view>

#include "nix/util/nar-accessor.hh"
#include "nix/util/error.hh"

#include "nix/util/tests/json-characterization.hh"

namespace nix {

// Forward declaration from memory-source-accessor.cc
namespace memory_source_accessor {
ref<MemorySourceAccessor> exampleComplex();
}

/* ----------------------------------------------------------------------------
 * JSON
 * --------------------------------------------------------------------------*/

class NarListingTest : public virtual CharacterizationTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "nar-listing";

public:

    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / testStem;
    }
};

using nlohmann::json;

struct NarListingJsonTest : NarListingTest,
                            JsonCharacterizationTest<NarListing>,
                            ::testing::WithParamInterface<std::pair<std::string_view, NarListing>>
{};

TEST_P(NarListingJsonTest, from_json)
{
    auto & [name, expected] = GetParam();
    readJsonTest(name, expected);
}

TEST_P(NarListingJsonTest, to_json)
{
    auto & [name, value] = GetParam();
    writeJsonTest(name, value);
}

INSTANTIATE_TEST_SUITE_P(
    NarListingJSON,
    NarListingJsonTest,
    ::testing::Values(
        std::pair{
            "deep",
            listNarDeep(*memory_source_accessor::exampleComplex(), CanonPath::root),
        }));

struct ShallowNarListingJsonTest : NarListingTest,
                                   JsonCharacterizationTest<ShallowNarListing>,
                                   ::testing::WithParamInterface<std::pair<std::string_view, ShallowNarListing>>
{};

TEST_P(ShallowNarListingJsonTest, from_json)
{
    auto & [name, expected] = GetParam();
    readJsonTest(name, expected);
}

TEST_P(ShallowNarListingJsonTest, to_json)
{
    auto & [name, value] = GetParam();
    writeJsonTest(name, value);
}

INSTANTIATE_TEST_SUITE_P(
    ShallowNarListingJSON,
    ShallowNarListingJsonTest,
    ::testing::Values(
        std::pair{
            "shallow",
            listNarShallow(*memory_source_accessor::exampleComplex(), CanonPath::root),
        }));

TEST(NarAccessor, directoryEntryTypesFollowListingKinds)
{
    for (bool lazy : {false, true}) {
        SCOPED_TRACE(lazy);
        NarListing::Directory children;
        std::map<std::string, SourceAccessor::Stat> expected;
        auto add = [&](std::string name, NarListing node, SourceAccessor::Stat stat) {
            children.entries.emplace(name, std::move(node));
            expected.emplace(std::move(name), stat);
        };
        for (bool executable : {false, true})
            for (bool sizeKnown : {false, true})
                for (bool offsetKnown : {false, true}) {
                    auto name = "regular-" + std::to_string(executable) + std::to_string(sizeKnown)
                                + std::to_string(offsetKnown);
                    auto size = sizeKnown ? std::optional<uint64_t>{0} : std::nullopt;
                    auto offset = offsetKnown ? std::optional<uint64_t>{23} : std::nullopt;
                    add(name,
                        NarListing::Regular{.executable = executable, .contents = {size, offset}},
                        {.type = SourceAccessor::tRegular,
                         .fileSize = size,
                         .isExecutable = executable,
                         .narOffset = offset});
                }
        add("empty-directory", NarListing::Directory{}, {.type = SourceAccessor::tDirectory});
        add("directory",
            NarListing::Directory{.entries = {{"child", NarListing::Symlink{.target = "absent"}}}},
            {.type = SourceAccessor::tDirectory});
        add("empty-link", NarListing::Symlink{.target = ""}, {.type = SourceAccessor::tSymlink});
        add("dangling-link", NarListing::Symlink{.target = "absent"}, {.type = SourceAccessor::tSymlink});
        NarListing listing = NarListing::Directory{.entries = {{"nested", std::move(children)}}};
        size_t reads = 0;
        auto accessor = lazy ? makeLazyNarAccessor(std::move(listing), [&](uint64_t, uint64_t, Sink &) { ++reads; })
                             : makeNarAccessor(std::move(listing));
        auto rootEntries = accessor->readDirectory(CanonPath::root);
        ASSERT_EQ(rootEntries.size(), 1);
        EXPECT_EQ(rootEntries.at("nested"), SourceAccessor::tDirectory);
        auto path = CanonPath("/nested");
        auto entries = accessor->readDirectory(path);
        ASSERT_EQ(entries.size(), expected.size());
        for (auto & [name, stat] : expected) {
            SCOPED_TRACE(name);
            ASSERT_TRUE(entries.at(name));
            EXPECT_EQ(*entries.at(name), stat.type);
            auto actual = accessor->maybeLstat(path / name);
            ASSERT_TRUE(actual);
            EXPECT_EQ(actual->type, stat.type);
            EXPECT_EQ(actual->fileSize, stat.fileSize);
            EXPECT_EQ(actual->isExecutable, stat.isExecutable);
            EXPECT_EQ(actual->narOffset, stat.narOffset);
        }
        EXPECT_TRUE(accessor->readDirectory(path / "empty-directory").empty());
        EXPECT_EQ(accessor->readDirectory(path / "directory").at("child"), SourceAccessor::tSymlink);
        EXPECT_FALSE(accessor->maybeLstat(path / "absent"));
        EXPECT_THROW(accessor->readDirectory(path / "absent"), Error);
        EXPECT_THROW(accessor->readDirectory(path / "regular-000"), Error);
        EXPECT_THROW(accessor->readDirectory(path / "dangling-link"), Error);
        EXPECT_EQ(reads, 0);
    }
}

} // namespace nix
