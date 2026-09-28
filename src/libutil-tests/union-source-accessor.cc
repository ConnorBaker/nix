#include "nix/util/source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/serialise.hh"
#include "nix/util/file-system.hh"
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

TEST(UnionSourceAccessor, physicalPathsRepresentTheSelectedSubtree)
{
    auto root = createTempDir();
    AutoDelete cleanup(root, true);
    createDirs(root / "merged");
    createDirs(root / "blocked");
    createDirs(root / "link");
    createDirs(root / "alone");
    writeFile(root / "hidden", "physical bytes hidden by memory");
    writeFile(root / "merged" / "lower", "lower");
    writeFile(root / "blocked" / "child", "hidden by a leaf");
    writeFile(root / "link" / "child", "hidden by a symlink");
    writeFile(root / "alone" / "child", "visible");
    auto memory = memoryTree({{"hidden", "visible memory bytes"}, {"merged/upper", "upper"}, {"blocked", "leaf"}});
    MemorySink{*memory}.createSymlink(CanonPath("link"), "target");
    auto physical = makeFSSourceAccessor(root);
    auto view = makeUnionSourceAccessor({memory, physical});

    EXPECT_EQ(view->readFile(CanonPath("hidden")), "visible memory bytes");
    EXPECT_FALSE(view->getPhysicalPath(CanonPath("hidden")));
    EXPECT_FALSE(view->getPhysicalPath(CanonPath("blocked/child")));
    EXPECT_FALSE(view->getPhysicalPath(CanonPath("link/child")));
    EXPECT_FALSE(view->getPhysicalPath(CanonPath("link")));
    EXPECT_FALSE(view->getPhysicalPath(CanonPath("missing")));
    EXPECT_FALSE(view->getPhysicalPath(CanonPath("merged")));
    EXPECT_FALSE(view->getPhysicalPath(CanonPath::root));
    EXPECT_EQ(view->getPhysicalPath(CanonPath("merged/lower")), root / "merged" / "lower");
    EXPECT_EQ(view->getPhysicalPath(CanonPath("alone")), root / "alone");
    EXPECT_EQ(view->readFile(CanonPath("alone/child")), readFile(*view->getPhysicalPath(CanonPath("alone/child"))));
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
    EXPECT_EQ(general->getFingerprint(CanonPath("d/x")), std::pair(CanonPath("d/x"), std::string("A")));
    EXPECT_EQ(general->getFingerprint(CanonPath("d/y")), std::pair(CanonPath("d/y"), std::string("B")));
    EXPECT_EQ(general->getFingerprint(CanonPath("g")), std::pair(CanonPath("g"), std::string("B")));
    EXPECT_EQ(general->getFingerprint(CanonPath("f")), std::pair(CanonPath("f"), std::string("A")));
    EXPECT_FALSE(general->getFingerprint(CanonPath("f/z")));
    auto atD = general->getFingerprint(CanonPath("d"));
    ASSERT_TRUE(atD);
    EXPECT_EQ(atD->first, CanonPath::root);
    EXPECT_NE(atD->second, "A");
    EXPECT_NE(atD->second, "B");
    EXPECT_NE(general->getFingerprint(CanonPath::root).value().second, atD->second);

    b->fingerprint = std::nullopt;
    EXPECT_FALSE(general->getFingerprint(CanonPath("d")));
    EXPECT_EQ(general->getFingerprint(CanonPath("d/x")).value().second, std::string("A"));
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
                    auto identity = actual->getFingerprint(path);
                    if (!expected) {
                        EXPECT_FALSE(identity);
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
                    EXPECT_EQ(identity.has_value(), shouldName);
                    auto expectedNar = overlayNar(*expected);
                    EXPECT_EQ(narOf(*actual, path), expectedNar);
                    if (identity) {
                        auto entry =
                            namedTrees.emplace(std::pair{identity->second, identity->first.abs()}, expectedNar).first;
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

namespace {

struct CallbackLifetimeAccessor : MemorySourceAccessor
{
    int & destroyed;
    std::function<void()> & release;

    CallbackLifetimeAccessor(int & destroyed, std::function<void()> & release)
        : destroyed(destroyed)
        , release(release)
    {
    }

    ~CallbackLifetimeAccessor() override
    {
        ++destroyed;
    }

    void readFile(const CanonPath &, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        /* A failing ownership implementation may destroy this accessor in
           either callback. Keep the test itself defined: after calling out,
           use only locals and caller-owned state, never members of this. */
        const std::string bytes = "contents";
        sizeCallback(bytes.size());
        sink(bytes);
    }

    std::string readLink(const CanonPath &) override
    {
        auto callback = release;
        callback();
        return "target";
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        auto result = std::pair{path, fmt("lifetime-child-%d", number)};
        auto callback = release;
        callback();
        return result;
    }
};

} // namespace

TEST(UnionSourceAccessor, resolvedChildSurvivesReentrantCallerRelease)
{
    enum class Call { SizeCallback, SinkCallback, ReadLink, Fingerprint };
    size_t cases = 0;
    for (bool mounted : {false, true})
        for (bool nested : {false, true})
            for (auto call : {Call::SizeCallback, Call::SinkCallback, Call::ReadLink, Call::Fingerprint}) {
                SCOPED_TRACE(fmt("mounted=%d nested=%d call=%d", mounted, nested, static_cast<unsigned>(call)));
                int destroyed = 0;
                std::function<void()> release;
                std::weak_ptr<SourceAccessor> child;
                SourcePath source{
                    [&]() -> ref<SourceAccessor> {
                        auto leaf = make_ref<CallbackLifetimeAccessor>(destroyed, release);
                        leaf->addFile(CanonPath("f"), "contents");
                        if (call == Call::ReadLink)
                            leaf->root->raw = MemorySourceAccessor::File::Directory{
                                {{"f", MemorySourceAccessor::File::Symlink{.target = "target"}}}};
                        child = leaf.get_ptr();
                        auto empty = make_ref<MemorySourceAccessor>();
                        if (mounted) {
                            if (nested)
                                return makeMountedSourceAccessor({{CanonPath::root, empty}, {CanonPath("m/n"), leaf}});
                            return makeMountedSourceAccessor({{CanonPath::root, leaf}});
                        }
                        return nested ? makeUnionSourceAccessor({empty, leaf}) : makeUnionSourceAccessor({leaf, empty});
                    }(),
                    CanonPath(mounted && nested ? "m/n/f" : "f")};
                std::weak_ptr<SourceAccessor> parent = source.accessor.get_ptr();
                auto replacement = make_ref<MemorySourceAccessor>();
                unsigned releases = 0;
                release = [&] {
                    ++releases;
                    source.accessor = replacement;
                    EXPECT_FALSE(child.expired());
                    EXPECT_EQ(destroyed, 0);
                };
                if (call == Call::ReadLink)
                    EXPECT_EQ(source.readLink(), "target");
                else if (call == Call::Fingerprint) {
                    auto name = source.accessor->getFingerprint(source.path);
                    ASSERT_TRUE(name);
                    EXPECT_TRUE(name->second.starts_with("lifetime-child-"));
                } else {
                    std::string contents;
                    LambdaSink sink{[&](std::string_view bytes) {
                        if (call == Call::SinkCallback)
                            release();
                        contents += bytes;
                    }};
                    source.readFile(sink, [&](uint64_t size) {
                        EXPECT_EQ(size, 8u);
                        if (call == Call::SizeCallback)
                            release();
                    });
                    EXPECT_EQ(contents, "contents");
                }
                EXPECT_EQ(releases, 1u);
                EXPECT_EQ(destroyed, 1);
                EXPECT_TRUE(child.expired());
                EXPECT_TRUE(parent.expired());
                ++cases;
            }
    EXPECT_EQ(cases, 16u);
}

TEST(UnionSourceAccessor, mergedNamingRetainsEveryChildAcrossCallerRelease)
{
    for (unsigned releaseAt : {1u, 2u}) {
        SCOPED_TRACE(releaseAt);
        int destroyed = 0;
        std::function<void()> release;
        std::vector<std::weak_ptr<SourceAccessor>> children;
        SourcePath source{[&]() -> ref<SourceAccessor> {
            auto first = make_ref<CallbackLifetimeAccessor>(destroyed, release);
            auto last = make_ref<CallbackLifetimeAccessor>(destroyed, release);
            first->addFile(CanonPath("a"), "contents");
            last->addFile(CanonPath("b"), "contents");
            children = {first.get_ptr(), last.get_ptr()};
            return makeUnionSourceAccessor({first, last});
        }()};
        std::weak_ptr<SourceAccessor> parent = source.accessor.get_ptr();
        auto replacement = make_ref<MemorySourceAccessor>();
        unsigned calls = 0;
        release = [&] {
            if (++calls == releaseAt) {
                source.accessor = replacement;
            }
            EXPECT_EQ(destroyed, 0);
            for (auto & child : children)
                EXPECT_FALSE(child.expired());
        };
        EXPECT_TRUE(source.accessor->getFingerprint(source.path));
        EXPECT_EQ(calls, 2u);
        EXPECT_EQ(destroyed, 2);
        EXPECT_TRUE(parent.expired());
        for (auto & child : children)
            EXPECT_TRUE(child.expired());
    }
}

namespace {

enum class UnionCall { Stat, File, Link, Directory, Name, Physical, Invalidate, Display };

struct UnionOwnershipProbe : MemorySourceAccessor
{
    unsigned layer;
    std::function<void(unsigned, UnionCall)> & observe;

    UnionOwnershipProbe(unsigned layer, decltype(observe) observe)
        : layer(layer)
        , observe(observe)
    {
    }

    void notify(UnionCall call)
    {
        auto callback = observe;
        auto index = layer;
        callback(index, call);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto result = MemorySourceAccessor::maybeLstat(path);
        notify(UnionCall::Stat);
        return result;
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        StringSink bytes;
        MemorySourceAccessor::readFile(path, bytes, [](uint64_t) {});
        notify(UnionCall::File);
        sizeCallback(bytes.s.size());
        sink(bytes.s);
    }

    std::string readLink(const CanonPath & path) override
    {
        auto result = MemorySourceAccessor::readLink(path);
        notify(UnionCall::Link);
        return result;
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto result = MemorySourceAccessor::readDirectory(path);
        notify(UnionCall::Directory);
        return result;
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        auto result = std::pair{path, fmt("layer-%d", layer)};
        notify(UnionCall::Name);
        return result;
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        bool available = path == CanonPath("first") ? !layer : layer && path != CanonPath("missing");
        auto result = available ? std::optional{std::filesystem::path("physical")} : std::nullopt;
        notify(UnionCall::Physical);
        return result;
    }

    void invalidateCache() override
    {
        notify(UnionCall::Invalidate);
    }

    std::string showPath(const CanonPath & path) override
    {
        auto result = "shown:" + path.abs();
        notify(UnionCall::Display);
        return result;
    }
};

struct UnionOwnershipLost
{};

struct UnionCallbackStopped
{};

} // namespace

TEST(UnionSourceAccessor, everyDelegatedBoundaryPreservesItsRequiredOwners)
{
    enum class Operation { Exists, MaybeStat, Stat, File, Link, Directory, Name, Physical, Invalidate, Display };

    struct Case
    {
        Operation operation;
        const char * path;
        const char * expected;
    };

    /* These paths cover first/later selection, ancestor rejection, absence,
       merged directories and a single owner. Existing finite-tree tests
       independently check the overlay's tree and naming meanings. */
    const Case cases[]{
        {Operation::Exists, "lower", "yes"},
        {Operation::Exists, "missing", "no"},
        {Operation::MaybeStat, "lower", "regular"},
        {Operation::MaybeStat, "missing", "absent"},
        {Operation::MaybeStat, "blocked/hidden", "absent"},
        {Operation::Stat, "lower", "regular"},
        {Operation::Stat, "missing", "error"},
        {Operation::File, "lower", "contents"},
        {Operation::File, "missing", "error"},
        {Operation::File, "blocked/hidden", "error"},
        {Operation::Link, "link", "target"},
        {Operation::Link, "missing", "error"},
        {Operation::Directory, "shared", "a,b,"},
        {Operation::Directory, "lower", "error"},
        {Operation::Directory, "missing", "error"},
        {Operation::Name, "shared", "named"},
        {Operation::Name, "lower", "named"},
        {Operation::Name, "missing", "unnamed"},
        {Operation::Name, "blocked/hidden", "unnamed"},
        {Operation::Physical, "first", "absent"},
        {Operation::Physical, "upper", "absent"},
        {Operation::Physical, "lower", "physical"},
        {Operation::Physical, "shared", "absent"},
        {Operation::Physical, "blocked/hidden", "absent"},
        {Operation::Physical, "missing", "absent"},
        {Operation::Invalidate, "", "invalidated"},
        {Operation::Display, "upper", "shown:/upper"},
    };
    for (const auto & test : cases) {
        CanonPath path(test.path);
        using Event = std::pair<unsigned, UnionCall>;
        auto run = [&](std::optional<size_t> at, bool release, bool stop) {
            std::vector<Event> observed;
            std::vector<std::weak_ptr<SourceAccessor>> children;
            std::shared_ptr<SourceAccessor> owner;
            std::weak_ptr<SourceAccessor> parent;
            std::function<void(unsigned, UnionCall)> observe = [&](unsigned layer, UnionCall call) {
                observed.emplace_back(layer, call);
                if (at == observed.size() - 1) {
                    if (release)
                        owner.reset();
                    /* Selection/continuation still borrows wrapper state;
                       completed selection delegates using owned children. */
                    bool needsParent = call == UnionCall::Stat || call == UnionCall::Invalidate
                                       || (call == UnionCall::Directory && path == CanonPath("shared"))
                                       || (call == UnionCall::Physical && layer == 0 && path != CanonPath("first"));
                    bool childAlive = !children.at(layer).expired();
                    EXPECT_TRUE(childAlive);
                    if (needsParent)
                        EXPECT_FALSE(parent.expired());
                    /* No probe accesses itself after notify. Throw before
                       returning to a dead wrapper in a negative control. */
                    if (!childAlive || (needsParent && parent.expired()))
                        throw UnionOwnershipLost{};
                    if (stop)
                        throw UnionCallbackStopped{};
                }
            };
            std::vector<ref<SourceAccessor>> layers;
            for (unsigned layer = 0; layer < 2; ++layer) {
                auto child = make_ref<UnionOwnershipProbe>(layer, observe);
                child->addFile(CanonPath(layer ? "lower" : "upper"), "contents");
                child->addFile(CanonPath(layer ? "shared/b" : "shared/a"), "contents");
                child->addFile(CanonPath(layer ? "blocked/hidden" : "blocked"), "contents");
                if (!layer)
                    MemorySink{*child}.createSymlink(CanonPath("link"), "target");
                children.push_back(child.get_ptr());
                layers.push_back(child);
            }
            owner = makeUnionSourceAccessor(std::move(layers)).get_ptr();
            parent = owner;
            auto invoke = [&]() -> std::string {
                try {
                    switch (test.operation) {
                    case Operation::Exists:
                        return owner->pathExists(path) ? "yes" : "no";
                    case Operation::MaybeStat: {
                        auto stat = owner->maybeLstat(path);
                        return stat ? stat->typeString() : "absent";
                    }
                    case Operation::Stat:
                        return owner->lstat(path).typeString();
                    case Operation::File:
                        return owner->readFile(path);
                    case Operation::Link:
                        return owner->readLink(path);
                    case Operation::Directory: {
                        std::string names;
                        for (auto & [name, type] : owner->readDirectory(path))
                            names += name + ",";
                        return names;
                    }
                    case Operation::Name:
                        return owner->getFingerprint(path) ? "named" : "unnamed";
                    case Operation::Physical: {
                        auto physical = owner->getPhysicalPath(path);
                        return physical ? physical->string() : "absent";
                    }
                    case Operation::Invalidate:
                        owner->invalidateCache();
                        return "invalidated";
                    case Operation::Display:
                        return owner->showPath(path);
                    }
                } catch (SourceAccessorError &) {
                    return "error";
                }
                throw Error("unhandled operation");
            };
            if (stop)
                EXPECT_THROW(invoke(), UnionCallbackStopped);
            else
                EXPECT_NO_THROW(EXPECT_EQ(invoke(), test.expected));
            EXPECT_EQ(parent.expired(), release);
            if (release)
                for (auto & child : children)
                    EXPECT_TRUE(child.expired());
            owner.reset();
            EXPECT_TRUE(parent.expired());
            for (auto & child : children)
                EXPECT_TRUE(child.expired());
            return observed;
        };
        /* Enumerate every virtual-call position, not a sample of them.
           The baseline establishes the finite boundary count only; the
           result oracle above is independent of that trace. */
        auto baseline = run(std::nullopt, false, false);
        ASSERT_FALSE(baseline.empty());
        for (size_t at = 0; at < baseline.size(); ++at)
            for (bool release : {false, true})
                for (bool stop : {false, true}) {
                    SCOPED_TRACE(
                        fmt("operation=%d path=%s at=%d release=%d stop=%d",
                            unsigned(test.operation),
                            path,
                            at,
                            release,
                            stop));
                    auto observed = run(at, release, stop);
                    auto expected = baseline;
                    if (stop)
                        expected.resize(at + 1);
                    EXPECT_EQ(observed, expected);
                }
    }
}

} // namespace nix
