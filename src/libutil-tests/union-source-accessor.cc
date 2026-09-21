#include "nix/util/source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/tests/gmock-matchers.hh"
#include "nix/util/tests/source-accessor-gen.hh"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>

namespace nix {

/* Directories merge with earlier children winning; a non-directory in an
   earlier child hides the later children's subtrees at and below it. */
TEST(UnionSourceAccessor, overlaySemantics)
{
    auto a = memoryTree({{"d/x", "ax"}, {"f", "af"}, {"both", "a"}});
    auto b = memoryTree({{"d/y", "by"}, {"f/z", "bz"}, {"both", "b"}, {"g", "bg"}});
    auto accessor = makeUnionSourceAccessor({a, b});

    EXPECT_THAT(accessor, testing::HasDirectory(CanonPath::root, std::set<std::string>{"d", "f", "both", "g"}));
    EXPECT_THAT(accessor, testing::HasDirectory(CanonPath("d"), std::set<std::string>{"x", "y"}));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("d/x"), "ax"));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("d/y"), "by"));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("both"), "a"));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("g"), "bg"));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("f"), "af"));
    EXPECT_THROW(accessor->readDirectory(CanonPath("f")), NotADirectory);
    EXPECT_FALSE(accessor->pathExists(CanonPath("f/z")));
    EXPECT_THROW(accessor->readFile(CanonPath("f/z")), FileNotFound);
    EXPECT_FALSE(accessor->pathExists(CanonPath("nope")));
}

/* The general naming rule: a subtree that is one child's alone gets that
   child's name; a merged directory is named from all its parts or not at
   all.  Overlap agreement alone cannot justify forwarding a child's name
   for a directory to which another child contributes additional entries. */
TEST(UnionSourceAccessor, fingerprints)
{
    auto a = memoryTree({{"d/x", "ax"}, {"f", "af"}});
    auto b = memoryTree({{"d/y", "by"}, {"f/z", "bz"}, {"g", "bg"}});
    a->fingerprint = "A";
    b->fingerprint = "B";

    auto general = makeUnionSourceAccessor({a, b});
    EXPECT_EQ(general->getFingerprint(CanonPath("d/x")), std::pair(CanonPath("d/x"), std::optional<std::string>("A")));
    EXPECT_EQ(general->getFingerprint(CanonPath("d/y")), std::pair(CanonPath("d/y"), std::optional<std::string>("B")));
    EXPECT_EQ(general->getFingerprint(CanonPath("g")), std::pair(CanonPath("g"), std::optional<std::string>("B")));
    EXPECT_EQ(general->getFingerprint(CanonPath("f")), std::pair(CanonPath("f"), std::optional<std::string>("A")));
    EXPECT_FALSE(general->getFingerprint(CanonPath("f/z")).second);
    auto atD = general->getFingerprint(CanonPath("d"));
    ASSERT_TRUE(atD.second);
    EXPECT_EQ(atD.first, CanonPath::root);
    EXPECT_NE(*atD.second, "A");
    EXPECT_NE(*atD.second, "B");
    EXPECT_NE(general->getFingerprint(CanonPath::root).second, atD.second);

    b->fingerprint = std::nullopt;
    EXPECT_FALSE(general->getFingerprint(CanonPath("d")).second);
    EXPECT_EQ(general->getFingerprint(CanonPath("d/x")).second, std::optional<std::string>("A"));
    b->fingerprint = "B";
}

TEST(UnionSourceAccessor, anEarlierDirectoryHidesAMiddleLayersBlockingLeaf)
{
    for (bool symlink : {false, true}) {
        auto first = make_ref<MemorySourceAccessor>();
        MemorySink firstSink{*first};
        firstSink.createDirectory(CanonPath::root);
        firstSink.createDirectory(CanonPath("d"));
        auto middle = make_ref<MemorySourceAccessor>();
        MemorySink sink{*middle};
        sink.createDirectory(CanonPath::root);
        if (symlink)
            sink.createSymlink(CanonPath("d"), "target");
        else
            middle->addFile(CanonPath("d"), "hidden");
        auto last = memoryTree({{"d/x", "visible"}});
        auto combined = makeUnionSourceAccessor({first, middle, last});
        EXPECT_THAT(combined, testing::HasDirectory(CanonPath("d"), std::set<std::string>{"x"}));
        EXPECT_THAT(combined, testing::HasContents(CanonPath("d/x"), "visible"));
    }
}

namespace {

using OverlayFile = MemorySourceAccessor::File;
using OverlayNodes = std::vector<const OverlayFile *>;

/* All trees over one child name per depth, with absent, plain,
   executable, symlink and empty-directory leaves: 5, 9 and 13 trees
   at depths zero, one and two. Bytes distinguish the source layers. */
std::vector<std::optional<OverlayFile>> smallOverlayTrees(size_t depth, const std::string & bytes)
{
    std::vector<std::optional<OverlayFile>> trees{
        std::nullopt,
        OverlayFile{OverlayFile::Regular{.contents = bytes}},
        OverlayFile{OverlayFile::Regular{.executable = true, .contents = bytes}},
        OverlayFile{OverlayFile::Symlink{.target = bytes}}};
    if (!depth)
        trees.emplace_back(OverlayFile::Directory{});
    else
        for (auto & child : smallOverlayTrees(depth - 1, bytes)) {
            OverlayFile::Directory directory;
            if (child)
                directory.entries.emplace(depth == 2 ? "d" : "x", *child);
            trees.emplace_back(std::move(directory));
        }
    return trees;
}

/* The specification recurses through directory values, not through
   absolute path lookups or the implementation's ancestor checks. */
std::optional<OverlayFile> renderOverlay(const OverlayNodes & nodes)
{
    auto first = std::find_if(nodes.begin(), nodes.end(), [](auto node) { return node; });
    if (first == nodes.end())
        return std::nullopt;
    if (!std::holds_alternative<OverlayFile::Directory>((*first)->raw))
        return **first;
    std::map<std::string, OverlayNodes> children;
    for (auto node : nodes)
        if (node)
            if (auto directory = std::get_if<OverlayFile::Directory>(&node->raw))
                for (auto & [name, child] : directory->entries)
                    children[name].push_back(&child);
    OverlayFile::Directory result;
    for (auto & [name, childNodes] : children)
        result.entries.emplace(name, *renderOverlay(childNodes));
    return result;
}

/* Project the same top-down model to one path, retaining layer indices
   solely to determine which named leaves contribute to its name. */
OverlayNodes overlayNodesAt(OverlayNodes nodes, const CanonPath & path)
{
    for (auto component : path) {
        auto first = std::find_if(nodes.begin(), nodes.end(), [](auto node) { return node; });
        if (first == nodes.end() || !std::holds_alternative<OverlayFile::Directory>((*first)->raw))
            return OverlayNodes(nodes.size());
        for (auto & node : nodes) {
            auto directory = node ? std::get_if<OverlayFile::Directory>(&node->raw) : nullptr;
            if (directory) {
                auto child = directory->entries.find(component);
                node = child == directory->entries.end() ? nullptr : &child->second;
            } else
                node = nullptr;
        }
    }
    return nodes;
}

std::string overlayNar(const OverlayFile & file)
{
    MemorySourceAccessor tree;
    tree.root = file;
    return narOf(tree);
}

} // namespace

TEST(UnionSourceAccessor, everySmallTreeAndNamingCombinationThroughThreeLayers)
{
    std::vector<std::vector<std::optional<OverlayFile>>> domains;
    for (size_t layer = 0; layer < 3; ++layer)
        domains.push_back(smallOverlayTrees(2, fmt("layer-%d", layer)));
    const std::vector<CanonPath> queries{
        CanonPath::root,
        CanonPath("d"),
        CanonPath("d/x"),
        CanonPath("other"),
        CanonPath("d/other"),
        CanonPath("d/x/other")};
    auto leafName = [](size_t layer, size_t shape) { return fmt("overlay-leaf-%d-%d", layer, shape); };
    std::map<std::pair<std::string, std::string>, std::string> namedTrees;
    /* Include leaf names in the soundness comparison, so incorrectly
       forwarding a child's name for a larger union is observable. */
    for (size_t layer = 0; layer < domains.size(); ++layer)
        for (size_t shape = 0; shape < domains[layer].size(); ++shape)
            for (auto & path : queries) {
                auto & tree = domains[layer][shape];
                if (auto file = renderOverlay(overlayNodesAt({tree ? &*tree : nullptr}, path)))
                    namedTrees.emplace(std::pair{leafName(layer, shape), path.abs()}, overlayNar(*file));
            }

    size_t cases = 0, tuples = 1;
    for (size_t count = 0; count <= 3; ++count) {
        if (count)
            tuples *= domains.front().size();
        for (size_t tuple = 0; tuple < tuples; ++tuple) {
            std::vector<ref<MemorySourceAccessor>> leaves;
            std::vector<ref<SourceAccessor>> layers;
            std::vector<size_t> shapes;
            OverlayNodes roots;
            auto remaining = tuple;
            for (size_t layer = 0; layer < count; ++layer) {
                auto shape = remaining % domains[layer].size();
                remaining /= domains[layer].size();
                auto leaf = make_ref<MemorySourceAccessor>();
                leaf->root = domains[layer][shape];
                roots.push_back(leaf->root ? &*leaf->root : nullptr);
                shapes.push_back(shape);
                layers.push_back(leaf);
                leaves.push_back(leaf);
            }
            auto actual = makeUnionSourceAccessor(std::move(layers));
            for (size_t named = 0; named < (size_t{1} << count); ++named) {
                ++cases;
                SCOPED_TRACE(fmt("layers=%d tuple=%d named=%d", count, tuple, named));
                for (size_t layer = 0; layer < count; ++layer)
                    leaves[layer]->fingerprint = named & (size_t{1} << layer)
                                                     ? std::optional<std::string>(leafName(layer, shapes[layer]))
                                                     : std::nullopt;
                for (auto & path : queries) {
                    SCOPED_TRACE(path.abs());
                    auto candidates = overlayNodesAt(roots, path);
                    auto expected = renderOverlay(candidates);
                    auto observed = actual->maybeLstat(path);
                    ASSERT_EQ(observed.has_value(), expected.has_value());
                    auto [relative, name] = actual->getFingerprint(path);
                    if (!expected) {
                        EXPECT_FALSE(name);
                        continue;
                    }
                    auto expectedStat = expected->lstat();
                    EXPECT_EQ(observed->type, expectedStat.type);
                    EXPECT_EQ(observed->fileSize, expectedStat.fileSize);
                    EXPECT_EQ(observed->isExecutable, expectedStat.isExecutable);
                    if (auto file = std::get_if<OverlayFile::Regular>(&expected->raw))
                        EXPECT_EQ(actual->readFile(path), file->contents);
                    else if (auto link = std::get_if<OverlayFile::Symlink>(&expected->raw))
                        EXPECT_EQ(actual->readLink(path), link->target);
                    else {
                        SourceAccessor::DirEntries entries;
                        for (auto & [entry, child] : std::get<OverlayFile::Directory>(expected->raw).entries)
                            entries.emplace(entry, child.lstat().type);
                        EXPECT_EQ(actual->readDirectory(path), entries);
                    }
                    bool shouldName = true;
                    for (size_t layer = 0; layer < count; ++layer)
                        if (auto node = candidates[layer]) {
                            if (expectedStat.type != SourceAccessor::tDirectory) {
                                shouldName = named & (size_t{1} << layer);
                                break;
                            }
                            if (std::holds_alternative<OverlayFile::Directory>(node->raw))
                                shouldName &= bool(named & (size_t{1} << layer));
                        }
                    EXPECT_EQ(name.has_value(), shouldName);
                    auto expectedNar = overlayNar(*expected);
                    EXPECT_EQ(narOf(*actual, path), expectedNar);
                    if (name) {
                        auto entry = namedTrees.emplace(std::pair{*name, relative.abs()}, expectedNar).first;
                        EXPECT_EQ(entry->second, expectedNar);
                    }
                }
            }
        }
    }
    EXPECT_EQ(cases, 18279u);
}

TEST(UnionSourceAccessor, twoLayersKeepTheSamePathProbeOrder)
{
    struct Observed : MemorySourceAccessor
    {
        size_t layer;
        std::vector<std::pair<size_t, CanonPath>> & calls;

        Observed(size_t layer, decltype(calls) calls)
            : layer(layer)
            , calls(calls)
        {
        }

        std::optional<Stat> maybeLstat(const CanonPath & path) override
        {
            calls.emplace_back(layer, path);
            return MemorySourceAccessor::maybeLstat(path);
        }
    };

    for (size_t upper = 0; upper < 6; ++upper)
        for (bool lowerPresent : {false, true}) {
            SCOPED_TRACE(fmt("upper=%d lower-present=%d", upper, lowerPresent));
            std::vector<std::pair<size_t, CanonPath>> calls;
            auto a = make_ref<Observed>(0, calls), b = make_ref<Observed>(1, calls);
            if (upper == 1)
                MemorySink{*a}.createDirectory(CanonPath::root);
            if (upper == 2) {
                MemorySink sink{*a};
                sink.createDirectory(CanonPath::root);
                sink.createDirectory(CanonPath("d"));
            }
            if (upper == 3)
                a->addFile(CanonPath("d"), "blocker");
            if (upper == 4) {
                MemorySink sink{*a};
                sink.createDirectory(CanonPath::root);
                sink.createSymlink(CanonPath("d"), "blocker");
            }
            if (upper == 5)
                a->addFile(CanonPath("d/x"), "upper");
            if (lowerPresent)
                b->addFile(CanonPath("d/x"), "lower");
            auto combined = makeUnionSourceAccessor({a, b});
            auto result = combined->maybeLstat(CanonPath("d/x"));
            EXPECT_EQ(result.has_value(), upper == 5 || (lowerPresent && upper < 3));
            std::vector<std::pair<size_t, CanonPath>> expected{{0, CanonPath("d/x")}};
            if (upper != 5) {
                expected.emplace_back(1, CanonPath("d/x"));
                if (lowerPresent) {
                    expected.emplace_back(0, CanonPath("d"));
                    if (upper < 2)
                        expected.emplace_back(0, CanonPath::root);
                }
            }
            EXPECT_EQ(calls, expected);
        }
}

} // namespace nix
