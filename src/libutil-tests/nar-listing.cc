#include <string_view>

#include "nix/util/archive.hh"
#include "nix/util/nar-accessor.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/error.hh"

#include "nix/util/tests/json-characterization.hh"
#include "nix/util/tests/setting-scopes.hh"

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

TEST(NarListing, parserScopesPreserveNodesAndPayloadOffsetsAcrossSinks)
{
    using File = MemorySourceAccessor::File;
    size_t cases = 0;
    for (bool caseHack : {false, true}) {
        WithCaseHack setting{caseHack};
        for (auto mode :
             {merkle::Mode::Regular, merkle::Mode::Executable, merkle::Mode::Symlink, merkle::Mode::Directory}) {
            auto tree = make_ref<MemorySourceAccessor>();
            if (mode == merkle::Mode::Directory) {
                for (size_t size = 0; size <= 8; ++size)
                    tree->addFile(CanonPath("/nested/data-" + std::to_string(size)), std::string(size, 'a' + size));
                tree->open(CanonPath("/nested/empty"), File{File::Directory{}});
                tree->open(CanonPath("/nested/exec"), File{File::Regular{true, "executable"}});
                tree->open(CanonPath("/nested/link"), File{File::Symlink{"../Foo"}});
                tree->addFile(CanonPath("/Foo"), "first");
                tree->addFile(CanonPath("/foo"), "second");
                tree->addFile(CanonPath("/tail"), "after nested directory");
            } else if (mode == merkle::Mode::Symlink)
                tree->open(CanonPath::root, File{File::Symlink{"absent"}});
            else
                tree->open(CanonPath::root, File{File::Regular{mode == merkle::Mode::Executable, "root payload"}});
            StringSink encoded;
            tree->dumpPath(CanonPath::root, encoded);
            auto framed = encoded.s + "trailing bytes";
            for (size_t sinkKind = 0; sinkKind < 5; ++sinkKind) {
                SCOPED_TRACE(fmt("caseHack=%d mode=%o sink=%d", caseHack, static_cast<RawMode>(mode), sinkKind));
                auto copied = make_ref<MemorySourceAccessor>();
                MemorySink memory{*copied};
                NullFileSystemObjectSink skip;
                ObjectHashSink hash;
                TeeFileSystemObjectSink scoped{memory, skip};
                FileSystemObjectSink * sinks[]{nullptr, &skip, &memory, &hash, &scoped};
                StringSource input{framed};
                auto listing = sinks[sinkKind] ? parseNarListing(input, *sinks[sinkKind]) : parseNarListing(input);
                EXPECT_EQ(input.pos, encoded.s.size());
                auto check = [&](this const auto & self, const File & expected, const NarListing & actual) -> void {
                    ASSERT_EQ(actual.raw.index(), expected.raw.index());
                    if (auto regular = std::get_if<File::Regular>(&expected.raw)) {
                        auto & entry = std::get<NarListing::Regular>(actual.raw);
                        EXPECT_EQ(entry.executable, regular->executable);
                        ASSERT_TRUE(entry.contents.fileSize);
                        ASSERT_TRUE(entry.contents.narOffset);
                        EXPECT_EQ(*entry.contents.fileSize, regular->contents.size());
                        ASSERT_LE(*entry.contents.narOffset, encoded.s.size());
                        ASSERT_LE(*entry.contents.fileSize, encoded.s.size() - *entry.contents.narOffset);
                        EXPECT_EQ(
                            encoded.s.substr(*entry.contents.narOffset, *entry.contents.fileSize), regular->contents);
                    } else if (auto directory = std::get_if<File::Directory>(&expected.raw)) {
                        auto & entries = std::get<NarListing::Directory>(actual.raw).entries;
                        ASSERT_EQ(entries.size(), directory->entries.size());
                        for (auto & [name, child] : directory->entries) {
                            auto physical = caseHack && name == "foo" ? name + std::string(caseHackSuffix) + "1" : name;
                            ASSERT_TRUE(entries.contains(physical));
                            self(child, entries.at(physical));
                        }
                    } else
                        EXPECT_EQ(
                            std::get<NarListing::Symlink>(actual.raw).target,
                            std::get<File::Symlink>(expected.raw).target);
                };
                check(*tree->root, listing);
                if (sinkKind == 2 || sinkKind == 4) {
                    StringSink output;
                    copied->dumpPath(CanonPath::root, output);
                    EXPECT_EQ(output.s, encoded.s);
                } else if (sinkKind == 3) {
                    EXPECT_EQ(hash.finish().narSize, encoded.s.size());
                }
                ++cases;
            }
        }
    }
    EXPECT_EQ(cases, 40u);
}

TEST_F(NarListingTest, parserErrorsKeepTheirSourcePositionWithEitherListingOverload)
{
    for (auto name :
         {"invalid-tag-instead-of-contents",
          "nul-character",
          "dot",
          "dotdot",
          "slash",
          "empty",
          "executable-after-contents",
          "name-after-node",
          "regular-one-byte-non-zero-padding",
          "regular-truncated-padding",
          "regular-truncated-large-contents"}) {
        SCOPED_TRACE(name);
        auto bytes = readFile(getUnitTestData() / "nars" / (std::string(name) + ".nar"));
        std::pair<std::string, size_t> expected;
        for (size_t route = 0; route < 3; ++route) {
            StringSource source{bytes};
            NullFileSystemObjectSink skip;
            ObjectHashSink hash;
            try {
                if (route == 0)
                    parseDump(skip, source);
                else if (route == 1)
                    (void) parseNarListing(source);
                else
                    (void) parseNarListing(source, hash);
                FAIL() << "invalid NAR was accepted";
            } catch (Error & error) {
                auto observed = std::pair{error.msg(), source.pos};
                if (route == 0)
                    expected = observed;
                else
                    EXPECT_EQ(observed, expected);
            }
        }
    }
}

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
