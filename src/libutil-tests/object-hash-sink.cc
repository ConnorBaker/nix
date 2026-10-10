/**
 * `ObjectHashSink` (doc/lazy-store/04-derivation.md, the hasher): the
 * object hash and the NAR size of a tree delivered through the
 * `FileSystemObjectSink` interface, checked against two independent
 * oracles -- `referenceObjectEntry`, a recursion over the hasher core alone, for
 * the root entry, and `dumpPath`'s length for the size -- through both
 * drivers, `parseDump` and the accessor walk; and against the ids `git`
 * itself gives under SHA-256 (`tests/functional/git-hashing/simple-sha256.sh`).
 */
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <memory>
#include <set>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "nix/util/object-hash-sink.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/archive.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"
#include "nix/util/tree-traversal.hh"

#include "nix/util/tests/setting-scopes.hh"
#include "nix/util/tests/source-accessor-gen.hh"

namespace nix {

namespace {

using File = MemorySourceAccessor::File;

merkle::TreeEntry expectedRoot(ref<MemorySourceAccessor> accessor)
{
    return referenceObjectEntry(*accessor, CanonPath::root);
}

/**
 * Both routes to the sink agree with the oracles: the NAR stream through
 * `parseDump`, and the accessor through the walk.
 */
void checkBothRoutes(ref<MemorySourceAccessor> accessor)
{
    auto nar = narOf(*accessor);
    auto expected = expectedRoot(accessor);

    StringSource in{nar};
    auto viaNar = objectHashOfNar(in);
    EXPECT_EQ(viaNar.root, expected);
    EXPECT_EQ(viaNar.narSize, nar.size());

    auto viaWalk = objectHashOf(*accessor, CanonPath::root);
    EXPECT_EQ(viaWalk.root, expected);
    EXPECT_EQ(viaWalk.narSize, nar.size());
}

/**
 * Nested directories, an empty directory, an executable, a symlink, an
 * empty file, and a file over 1 MiB (so that a file delivered in several
 * chunks is exercised).
 */
ref<MemorySourceAccessor> complexTree()
{
    auto acc = make_ref<MemorySourceAccessor>();
    acc->addFile(CanonPath{"/a/b/c.txt"}, "hello\n");
    acc->addFile(CanonPath{"/a/empty-file"}, "");
    acc->addFile(CanonPath{"/big"}, std::string((1 << 20) + 17, 'x'));
    acc->addFile(CanonPath{"/bin/run"}, "#!/bin/sh\nexit 0\n");
    std::get<File::Regular>(acc->open(CanonPath{"/bin/run"}, std::nullopt)->raw).executable = true;
    acc->open(CanonPath{"/link"}, File{File::Symlink{.target = "a/b/c.txt"}});
    acc->open(CanonPath{"/empty-dir"}, File{File::Directory{}});
    acc->open(CanonPath{"/a/empty-dir-below"}, File{File::Directory{}});
    return acc;
}

} // namespace

/* (1) A fixed tree through both routes. */
TEST(ObjectHashSink, complex_tree_matches_reference_and_nar_length)
{
    checkBothRoutes(complexTree());
}

/* A visitor's worker count cannot extend a push callback's lifetime.
   Every create call consumes its borrowed callback before returning,
   including when a directory is nested inside another push callback. */
TEST(ObjectHashSink, everyWorkerWidthConsumesPushCallbacksImmediately)
{
    auto source = make_ref<MemorySourceAccessor>();
    source->addFile(CanonPath("nested/leaf"), "abc");
    source->open(CanonPath("exec"), File{File::Regular{.executable = true, .contents = ""}});
    source->open(CanonPath("link"), File{File::Symlink{.target = "nested/leaf"}});
    source->open(CanonPath("empty"), File{File::Directory{}});
    const auto expected = expectedRoot(source);
    const auto nar = narOf(*source);
    size_t cases = 0;
    for (size_t workers = 1; workers <= 8; ++workers)
        for (bool warmed : {false, true}) {
            SCOPED_TRACE(fmt("workers=%d warmed=%d", workers, warmed));
            HashingVisitor visitor{workers};
            if (warmed) {
                /* The same visitor retains a pool after this branching pull
                   whenever its width permits concurrent children. */
                auto warm = objectHashOf(*source, CanonPath::root, defaultPathFilter, visitor);
                EXPECT_EQ(warm.root, expected);
                EXPECT_EQ(warm.narSize, nar.size());
            }
            ObjectHashSink sink{visitor};
            bool rootDone = false;
            EXPECT_NO_THROW(
                sink.createDirectory(CanonPath::root, [&](FileSystemObjectSink & dir, const CanonPath & path) {
                    bool nestedDone = false;
                    dir.createDirectory(path / "nested", [&](FileSystemObjectSink & nested, const CanonPath & under) {
                        bool read = false;
                        nested.createRegularFile(under / "leaf", [&](CreateRegularFileSink & file) {
                            read = true;
                            file.preallocateContents(3);
                            file("abc");
                        });
                        if (!read)
                            throw std::logic_error("push returned before consuming a regular file");
                        nestedDone = true;
                    });
                    /* Abort collection before a deferred visitor could invoke
                       an expired callback, so a regression fails without UB. */
                    if (!nestedDone)
                        throw std::logic_error("push returned before consuming a nested directory");
                    bool executableRead = false;
                    dir.createRegularFile(path / "exec", [&](CreateRegularFileSink & file) {
                        executableRead = true;
                        file.isExecutable();
                        file.preallocateContents(0);
                    });
                    if (!executableRead)
                        throw std::logic_error("push returned before consuming an executable");
                    dir.createSymlink(path / "link", "nested/leaf");
                    bool emptyDone = false;
                    dir.createDirectory(
                        path / "empty", [&](FileSystemObjectSink &, const CanonPath &) { emptyDone = true; });
                    if (!emptyDone)
                        throw std::logic_error("push returned before consuming an empty directory");
                    rootDone = true;
                }));
            if (!rootDone)
                continue;
            auto pushed = sink.finish();
            EXPECT_EQ(pushed.root, expected);
            EXPECT_EQ(pushed.narSize, nar.size());
            ++cases;

            /* The parser supplies the same immediate protocol for every root
               mode; ordinary pull and the independent tree hash are oracles. */
            for (auto root :
                 {CanonPath::root,
                  CanonPath("nested/leaf"),
                  CanonPath("exec"),
                  CanonPath("link"),
                  CanonPath("empty")}) {
                SCOPED_TRACE(root.abs());
                auto bytes = narOf(*source, root);
                auto pulled = objectHashOf(*source, root);
                HashingVisitor parsedVisitor{workers};
                if (warmed) {
                    auto warm = objectHashOf(*source, CanonPath::root, defaultPathFilter, parsedVisitor);
                    EXPECT_EQ(warm.root, expected);
                    EXPECT_EQ(warm.narSize, nar.size());
                }
                ObjectHashSink parsedSink{parsedVisitor};
                StringSource input{bytes};
                std::optional<ObjectHashSink::Result> parsed;
                EXPECT_NO_THROW({
                    parseDump(parsedSink, input);
                    parsed = parsedSink.finish();
                });
                if (!parsed)
                    continue;
                EXPECT_EQ(parsed->root, referenceObjectEntry(*source, root));
                EXPECT_EQ(parsed->root, pulled.root);
                EXPECT_EQ(parsed->narSize, pulled.narSize);
                EXPECT_EQ(parsed->narSize, bytes.size());
                ++cases;
            }
        }
    EXPECT_EQ(cases, 96u);
}

/* A visitor may use its callback's owned state after a recursive visit.
   Every directory keeps that same callback for its later siblings. */
TEST(ObjectHashSink, recursivePushPreservesOwnedCallbackState)
{
    struct Visitor : HashingVisitor
    {
        size_t completed = 0;

        HashedNode directory(const CanonPath & path, Children children) override
        {
            EXPECT_FALSE(children.canDefer);
            return HashingVisitor::directory(path, {[&](auto each) {
                struct Context
                {
                    decltype(each) & consume;
                    size_t & completed;
                    NodeVisitor<HashedNode> & visitor;
                    size_t ordinal = 0;
                } context{each, completed, *this};
                children.each([context = &context, ordinal = size_t{0}](
                             std::string_view name, bool isDirectory, Visit visit) mutable {
                    auto child = visit(context->visitor);
                    /* Both captures must remain valid after nested directory
                       callbacks, and the owned ordinal must persist. */
                    EXPECT_EQ(++ordinal, ++context->ordinal);
                    ++context->completed;
                    context->consume(std::string_view(name), bool(isDirectory), [&](auto &) { return child; });
                });
            }});
        }
    } visitor;

    auto source = complexTree();
    auto expected = referenceObjectEntry(*source, CanonPath::root);
    auto nar = narOf(*source);
    StringSource input{nar};
    ObjectHashSink sink{visitor};
    parseDump(sink, input);
    auto actual = sink.finish();
    EXPECT_EQ(actual.root, expected);
    EXPECT_TRUE(actual.narSize.has_value());
    EXPECT_EQ(actual.narSize, nar.size());
    EXPECT_EQ(visitor.completed, 10u);
}

TEST(ObjectHashSink, identifierDiagnosticsPreserveEveryNameClassAndDirectoryPath)
{
    ExperimentalFeatureSettings xpSettings;
    xpSettings.set("experimental-features", "blake3-hashes");
    const std::array algorithms{
        HashAlgorithm::MD5, HashAlgorithm::SHA1, HashAlgorithm::SHA512, HashAlgorithm::BLAKE3};
    const std::array modes{
        merkle::Mode::Directory, merkle::Mode::Regular, merkle::Mode::Executable, merkle::Mode::Symlink};
    const std::string marker(caseHackSuffix);
    const std::array<std::string, 13> names{
        "x", "x!", "x.", "y", "", ".", "..", "bad/name", std::string("a\0b", 3),
        marker, marker + "x", "x" + marker + "y", "x" + marker};
    const std::array paths{CanonPath::root, CanonPath("/parent")};
    size_t cases = 0;
    for (const auto & path : paths)
        for (auto algorithm : algorithms)
            for (auto mode : modes)
                for (size_t nameIndex = 0; nameIndex != names.size(); ++nameIndex) {
                    const auto & name = names[nameIndex];
                    SCOPED_TRACE(fmt("path=%s algorithm=%s mode=%o name=%s", path, printHashAlgo(algorithm), static_cast<RawMode>(mode), name));
                    Hash hash{algorithm, xpSettings};
                    HashingVisitor visitor;
                    try {
                        visitor.directory(path, {[&](auto each) {
                            each(name, false, [&](auto &) -> HashedNode { return {{mode, hash}, std::nullopt}; });
                        }});
                        ADD_FAILURE() << "wrong identifier algorithm was accepted";
                    } catch (const BadCanonPath &) {
                        ADD_FAILURE() << "name interpretation preempted the identifier diagnostic";
                    } catch (const Error & error) {
                        EXPECT_EQ(error.msg(), Error(
                            "object hash: '%s' was answered with a %s identifier, but the tree is hashed with %s",
                            std::string(path.abs()) + (path.isRoot() ? "" : "/") + name,
                            printHashAlgo(algorithm), printHashAlgo(merkle::hashAlgo)).msg());
                        if (nameIndex < 4) {
                            EXPECT_EQ(error.msg(), Error(
                                "object hash: '%s' was answered with a %s identifier, but the tree is hashed with %s",
                                path / name, printHashAlgo(algorithm), printHashAlgo(merkle::hashAlgo)).msg());
                        }
                    }
                    ++cases;
                }
    EXPECT_EQ(cases, 416u);
}

TEST(ObjectHashSink, recoveredChildInsertionDoesNotContributeToTheDirectory)
{
    const merkle::TreeEntry keep{merkle::Mode::Regular, merkle::blobId("first")};
    const merkle::TreeEntry tail{merkle::Mode::Regular, merkle::blobId("last")};
    const merkle::Tree expected{{"keep", keep}, {"tail", tail}};
    const auto expectedId = merkle::treeId(merkle::serialiseTree(expected));
    auto source = make_ref<MemorySourceAccessor>();
    source->addFile(CanonPath("keep"), "first");
    source->addFile(CanonPath("tail"), "last");
    const auto expectedNarSize = narOf(*source).size();
    size_t cases = 0;
    for (bool duplicate : {false, true})
        for (bool rejectedSize : {false, true})
            for (bool acceptedSize : {false, true}) {
                SCOPED_TRACE(
                    fmt("duplicate=%d rejectedSize=%d acceptedSize=%d", duplicate, rejectedSize, acceptedSize));
                HashingVisitor visitor;
                const HashedNode first{keep, merkle::nar::regular(5, false)};
                const HashedNode rejected{
                    {merkle::Mode::Regular, merkle::blobId("rejected")},
                    rejectedSize ? std::optional(merkle::nar::regular(8, false)) : std::nullopt};
                const HashedNode last{
                    tail, acceptedSize ? std::optional(merkle::nar::regular(4, false)) : std::nullopt};
                std::vector<std::string> visits;
                auto actual = visitor.directory(CanonPath::root, {[&](auto each) {
                    each("keep", false, [&](auto &) {
                        visits.push_back("first");
                        return first;
                    });
                    /* Recovery belongs to the child producer: a refused
                       insertion must leave both the map and its size alone. */
                    EXPECT_THROW(
                        each(
                            duplicate ? "keep" : "bad/name",
                            false,
                            [&](auto &) {
                                visits.push_back("rejected");
                                return rejected;
                            }),
                        Error);
                    each("tail", false, [&](auto &) {
                        visits.push_back("last");
                        return last;
                    });
                }});
                EXPECT_EQ(visits, (std::vector<std::string>{"first", "rejected", "last"}));
                EXPECT_EQ(actual.entry, (merkle::TreeEntry{merkle::Mode::Directory, expectedId}));
                EXPECT_EQ(actual.narSize.has_value(), acceptedSize);
                if (actual.narSize)
                    EXPECT_EQ(merkle::nar::root(*actual.narSize), expectedNarSize);
                ++cases;
            }
    EXPECT_EQ(cases, 8u);
}

TEST(TreeTraversal, directoryNamesMatchIndependentlyFramedNarBytes)
{
    struct Source : MemorySourceAccessor
    {
        bool typed;

        DirEntries readDirectory(const CanonPath & path) override
        {
            auto entries = MemorySourceAccessor::readDirectory(path);
            if (!typed)
                for (auto & [name, type] : entries)
                    type.reset();
            return entries;
        }
    };

    /* All name outcomes: identity, a remapping that reverses physical
       order, and a collision. Cross the case-hack setting, all four NAR
       node forms, known/unknown listing types, and all/accept/reject/throw
       filters. The oracle writes NAR framing without walking an accessor. */
    size_t cases = 0;
    for (bool hack : {false, true})
        for (unsigned spelling = 0; spelling != 3; ++spelling)
            for (unsigned kind = 0; kind != 4; ++kind)
                for (bool typed : {false, true})
                    for (unsigned filtering = 0; filtering != 4; ++filtering) {
                        SCOPED_TRACE(fmt(
                            "hack=%d spelling=%d kind=%d typed=%d filter=%d", hack, spelling, kind, typed, filtering));
                        WithCaseHack scope{hack};
                        auto source = make_ref<Source>();
                        source->root = File{File::Directory{}};
                        source->typed = typed;
                        auto actual = spelling ? "a" + std::string(caseHackSuffix) + "1" : "a";
                        File node = kind == 3   ? File{File::Directory{}}
                                    : kind == 2 ? File{File::Symlink{.target = "target"}}
                                                : File{File::Regular{.executable = kind == 1, .contents = "one"}};
                        source->open(CanonPath::root / actual, std::move(node));
                        source->addFile(CanonPath("a.b"), "two");
                        if (spelling == 2)
                            source->addFile(CanonPath("a"), "collision");

                        auto expected = [&](bool reverse) {
                            StringSink nar;
                            nar << narVersionMagic1 << "(" << "type" << "directory";
                            auto entry = [&](bool first) {
                                if (first && filtering == 2)
                                    return;
                                nar << "entry" << "(" << "name" << (first ? "a" : "a.b") << "node" << "(" << "type";
                                if (first && kind == 3)
                                    nar << "directory";
                                else if (first && kind == 2)
                                    nar << "symlink" << "target" << "target";
                                else {
                                    nar << "regular";
                                    if (first && kind == 1)
                                        nar << "executable" << "";
                                    nar << "contents" << (first ? "one" : "two");
                                }
                                nar << ")" << ")";
                            };
                            entry(!reverse);
                            entry(reverse);
                            nar << ")";
                            return std::move(nar.s);
                        };
                        std::vector<std::string> filtered;
                        PathFilter filter = defaultPathFilter;
                        if (filtering)
                            filter = [&](const std::string & path) {
                                filtered.push_back(path);
                                if (filtering == 3)
                                    throw std::runtime_error("filter failure");
                                return filtering != 2 || path != "/a";
                            };
                        StringSink nar;
                        if (spelling && (!hack || spelling == 2)) {
                            EXPECT_THROW(source->dumpPath(CanonPath::root, nar, filter), Error);
                            EXPECT_TRUE(filtered.empty());
                            StringSink header;
                            header << narVersionMagic1;
                            EXPECT_EQ(nar.s, header.s) << "invalid names fail before the directory callback";
                        } else if (filtering == 3) {
                            EXPECT_THROW(source->dumpPath(CanonPath::root, nar, filter), std::runtime_error);
                            EXPECT_EQ(filtered, (std::vector<std::string>{"/a"}));
                        } else {
                            source->dumpPath(CanonPath::root, nar, filter);
                            EXPECT_EQ(nar.s, expected(false));
                            EXPECT_EQ(
                                filtered,
                                filtering ? (std::vector<std::string>{"/a", "/a.b"}) : std::vector<std::string>{});
                            if (filtering != 2)
                                EXPECT_NE(nar.s, expected(true)) << "the byte oracle distinguishes reversed entries";
                        }
                        ++cases;
                    }
    EXPECT_EQ(cases, 192u);
}

TEST(TreeTraversal, borrowedChildrenKeepsMoveOnlyEmitterStateThroughTheDirectoryScope)
{
    using Children = NodeVisitor<HashedNode>::Children;
    static_assert(std::is_aggregate_v<Children>);
    static_assert(std::is_trivially_copyable_v<Children>);
    static_assert(!std::is_default_constructible_v<Children>);
    static_assert(!std::is_constructible_v<SourceAccessor::ReadDirectoryCallback, std::nullptr_t>);

    StringSink magic;
    magic << narVersionMagic1;
    for (size_t workers : {1u, 2u})
        for (size_t count : {0u, 1u, 4u}) {
            SCOPED_TRACE(fmt("workers=%d count=%d", workers, count));
            auto source = make_ref<MemorySourceAccessor>();
            source->open(CanonPath::root, File{File::Directory{}});
            std::vector<std::pair<std::string, HashedNode>> entries;
            for (size_t i = 0; i < count; ++i) {
                auto name = std::string("child") + std::to_string(i);
                auto path = CanonPath::root / name;
                source->addFile(path / "file", std::string(i + 1, char('a' + i)));
                entries.emplace_back(name, HashedNode{
                                               .entry = referenceObjectEntry(*source, path),
                                               .narSize = narOf(*source, path).size() - magic.s.size(),
                                           });
            }
            auto expected = expectedRoot(source);
            auto expectedSize = narOf(*source).size();
            std::atomic<bool> emitted{false};
            std::atomic<size_t> visited{0};
            size_t calls = 0;
            auto emitter = [state = std::make_unique<size_t>(0), wide = std::array<char, 256>{}, &entries,
                            &emitted, &visited, &calls, workers](Children::Each each) mutable {
                emitted = false;
                wide.front() = 'z';
                calls = ++*state;
                for (auto & [name, node] : entries)
                    each(std::string_view(name), true, [&, node](auto &) {
                        EXPECT_EQ(emitted.load(), workers > 1);
                        EXPECT_EQ(wide.front(), 'z');
                        ++visited;
                        return node;
                    });
                emitted = true;
            };
            Children children{emitter, true};
            auto copied = children;
            HashingVisitor visitor{workers};
            for (auto selected : {copied, children}) {
                auto result = visitor.directory(CanonPath::root, selected);
                EXPECT_EQ(result.entry, expected);
                ASSERT_TRUE(result.narSize);
                EXPECT_EQ(magic.s.size() + *result.narSize, expectedSize);
            }
            EXPECT_EQ(calls, 2u);
            EXPECT_EQ(visited.load(), 2 * count);
        }
}

TEST(TreeTraversal, borrowedDirectoryCallbackSharesMoveOnlyStateAndPropagatesFailure)
{
    auto source = make_ref<MemorySourceAccessor>();
    source->open(CanonPath::root, File{File::Directory{}});
    size_t calls = 0;
    bool fail = true;
    auto callback = [state = std::make_unique<size_t>(0), &calls, &fail](
                        SourceAccessor &, const CanonPath &, SourceAccessor::DirectoryListing && listing) {
        EXPECT_TRUE(listing.entries().empty());
        calls = ++*state;
        if (fail)
            throw std::runtime_error("borrowed directory callback");
    };
    SourceAccessor::ReadDirectoryCallback borrowed = callback;
    EXPECT_THROW(source->readDirectoryWithEntries(CanonPath::root, borrowed), std::runtime_error);
    fail = false;
    EXPECT_NO_THROW(source->readDirectoryWithEntries(CanonPath::root, borrowed));
    EXPECT_EQ(calls, 2u);
}

TEST(TreeTraversal, deferredVisitsKeepBothNameRepresentationsAlive)
{
    struct Source : MemorySourceAccessor
    {
        DirEntry hint;

        DirEntries readDirectory(const CanonPath & path) override
        {
            auto entries = MemorySourceAccessor::readDirectory(path);
            for (auto & [name, type] : entries)
                type = hint;
            return entries;
        }
    };

    struct Visitor : NodeVisitor<std::string>
    {
        bool directories;

        std::string regular(const CanonPath & path, fun<void(CreateRegularFileSink &)>) override
        {
            return path.abs();
        }

        std::string symlink(const CanonPath &, const std::string &) override
        {
            ADD_FAILURE() << "fixture has no symlinks";
            return {};
        }

        std::string directory(const CanonPath &, Children children) override
        {
            EXPECT_TRUE(children.canDefer);
            std::vector<std::pair<std::string, Visit>> pending;
            children.each([&](std::string_view name, bool isDirectory, Visit visit) {
                EXPECT_EQ(isDirectory, directories);
                pending.emplace_back(name, std::move(visit));
            });
            std::string result;
            /* Every enumeration/emitter stack frame has returned. */
            for (auto & [name, visit] : pending)
                result += name + ":" + visit(*this) + "|";
            return result;
        }
    };

    WithCaseHack scope{true};
    size_t cases = 0;
    for (bool suffix : {false, true})
        for (auto hint : std::array<SourceAccessor::DirEntry, 9>{
                 std::nullopt,
                 SourceAccessor::tRegular,
                 SourceAccessor::tSymlink,
                 SourceAccessor::tDirectory,
                 SourceAccessor::tChar,
                 SourceAccessor::tBlock,
                 SourceAccessor::tSocket,
                 SourceAccessor::tFifo,
                 SourceAccessor::tUnknown}) {
            auto source = make_ref<Source>();
            source->hint = hint;
            auto actual = suffix ? "a" + std::string(caseHackSuffix) + "1" : "a";
            source->addFile(CanonPath::root / actual, "one");
            source->addFile(CanonPath("a.b"), "two");
            Visitor visitor;
            visitor.directories = hint == SourceAccessor::tDirectory;
            EXPECT_EQ(traverse(*source, CanonPath::root, defaultPathFilter, visitor), "a:/" + actual + "|a.b:/a.b|");
            ++cases;
        }
    EXPECT_EQ(cases, 18u);
}

TEST(TreeTraversal, mutableFiltersKeepSourceCoordinatesAndObserveEveryChild)
{
    enum class Mode { All, Deny, Paths, AllToPaths, PathsToAll, PathsToDeny };
    struct Visitor : NodeVisitor<int>
    {
        std::function<void()> replace;
        bool beforeChildren;
        bool changed = false;
        CanonPath logicalRoot;
        std::vector<std::string> names;
        std::vector<std::string> paths;

        Visitor(std::function<void()> replace, bool beforeChildren, CanonPath logicalRoot)
            : replace(std::move(replace)), beforeChildren(beforeChildren), logicalRoot(std::move(logicalRoot)) {}

        int regular(const CanonPath & path, fun<void(CreateRegularFileSink &)>) override
        {
            EXPECT_TRUE(path.isWithin(logicalRoot));
            paths.push_back(path.abs());
            return 0;
        }

        int symlink(const CanonPath &, const std::string &) override
        {
            ADD_FAILURE() << "fixture has no symlinks";
            return 0;
        }

        int directory(const CanonPath &, Children children) override
        {
            if (beforeChildren)
                replace();
            children.each([&](std::string_view name, bool, Visit visit) {
                names.emplace_back(name);
                visit(*this);
                if (!beforeChildren && !changed) {
                    changed = true;
                    replace();
                }
            });
            return 0;
        }
    };
    size_t cases = 0;
    for (bool sourceRoot : {false, true})
        for (bool logicalRoot : {false, true})
            for (bool caseHack : {false, true})
                for (bool suffix : {false, true})
                    for (auto mode : {Mode::All, Mode::Deny, Mode::Paths, Mode::AllToPaths, Mode::PathsToAll, Mode::PathsToDeny})
                        for (bool beforeChildren : {false, true}) {
                            SCOPED_TRACE(fmt("sourceRoot=%d logicalRoot=%d caseHack=%d suffix=%d mode=%d before=%d",
                                sourceRoot, logicalRoot, caseHack, suffix, int(mode), beforeChildren));
                            WithCaseHack scope{caseHack};
                            auto source = make_ref<MemorySourceAccessor>();
                            auto from = sourceRoot ? CanonPath::root : CanonPath("selected");
                            auto logical = logicalRoot ? CanonPath::root : CanonPath("continued");
                            auto actualA = suffix ? "a" + std::string(caseHackSuffix) + "1" : "a";
                            source->addFile(from / actualA, "a");
                            source->addFile(from / "b", "b");
                            auto all = [&](bool changed) {
                                return mode == Mode::All || (mode == Mode::AllToPaths && !changed)
                                    || (mode == Mode::PathsToAll && changed);
                            };
                            auto admits = [&](bool changed, std::string_view name) {
                                if (all(changed)) return true;
                                if (mode == Mode::Deny || (mode == Mode::PathsToDeny && changed)) return false;
                                return name == "b" || (mode != Mode::Paths && mode != Mode::AllToPaths);
                            };
                            PathFilter filter = AllPaths{};
                            std::vector<std::string> calls;
                            auto assign = [&](bool changed) {
                                if (all(changed)) filter = AllPaths{};
                                else filter = [&, changed](const std::string & path) {
                                    calls.push_back(path);
                                    return (path == (from / "a").abs() && admits(changed, "a"))
                                        || (path == (from / "b").abs() && admits(changed, "b"));
                                };
                            };
                            assign(false);
                            Visitor visitor{[&] { assign(true); }, beforeChildren, logical};
                            if (suffix && !caseHack) {
                                EXPECT_THROW(traverse(*source, from, filter, visitor, logical), Error);
                                EXPECT_TRUE(visitor.names.empty());
                                EXPECT_TRUE(calls.empty());
                            } else {
                                std::vector<std::string> expectedNames, expectedPaths, expectedCalls;
                                bool changed = beforeChildren;
                                for (auto name : {"a", "b"}) {
                                    if (!all(changed)) expectedCalls.push_back((from / name).abs());
                                    if (!admits(changed, name)) continue;
                                    expectedNames.emplace_back(name);
                                    expectedPaths.push_back((logical / (std::string_view(name) == "a" ? actualA : "b")).abs());
                                    changed = true;
                                }
                                EXPECT_EQ(traverse(*source, from, filter, visitor, logical), 0);
                                EXPECT_EQ(visitor.names, expectedNames);
                                EXPECT_EQ(visitor.paths, expectedPaths);
                                EXPECT_EQ(calls, expectedCalls);
                            }
                            ++cases;
                        }
    EXPECT_EQ(cases, 192u);
    RecordProperty("finite_cells", cases);
}

TEST(TreeTraversal, narSinkMayReplaceIdentityFilterAtDirectoryHeader)
{
    struct MutatingSink : Sink
    {
        StringSink bytes;
        PathFilter & filter;
        std::string keep;
        bool changed = false;
        size_t headers = 0, targetHeader;

        MutatingSink(PathFilter & filter, std::string keep, size_t targetHeader)
            : filter(filter), keep(std::move(keep)), targetHeader(targetHeader) {}

        void operator()(std::string_view data) override
        {
            bytes(data);
            if (data == "directory") ++headers;
            if (!changed && headers == targetHeader) {
                changed = true;
                filter = [this](const std::string & path) { return path == keep; };
            }
        }
    };
    size_t cases = 0;
    for (bool sourceRoot : {false, true})
        for (bool suffix : {false, true})
            for (bool nested : {false, true}) {
                WithCaseHack scope{true};
                auto source = make_ref<MemorySourceAccessor>();
                auto from = sourceRoot ? CanonPath::root : CanonPath("selected");
                auto directory = nested ? from / "dir" : from;
                source->addFile(directory / (suffix ? "a" + std::string(caseHackSuffix) + "1" : "a"), "discarded");
                source->addFile(directory / "b", "kept");
                auto keep = (directory / "b").abs();
                PathFilter expectedFilter = [&](const std::string & path) {
                    return path == keep || (nested && path == directory.abs());
                };
                StringSink expected;
                source->dumpPath(from, expected, expectedFilter);
                PathFilter mutableFilter = AllPaths{};
                MutatingSink actual{mutableFilter, keep, nested ? 2u : 1u};
                source->dumpPath(from, actual, mutableFilter);
                EXPECT_TRUE(actual.changed);
                EXPECT_EQ(actual.bytes.s, expected.s);
                ++cases;
            }
    EXPECT_EQ(cases, 8u);
    RecordProperty("finite_cells", cases);
}

TEST(TreeTraversal, rejectedDirectoryDoesNotObserveItsDescendantsOrFilterTheRoot)
{
    size_t cases = 0;
    for (bool sourceRoot : {false, true})
        for (bool logicalRoot : {false, true}) {
            struct Source : MemorySourceAccessor
            {
                std::vector<CanonPath> observed;
                std::optional<Stat> maybeLstat(const CanonPath & path) override
                {
                    observed.push_back(path);
                    return MemorySourceAccessor::maybeLstat(path);
                }
            };
            auto source = make_ref<Source>();
            auto from = sourceRoot ? CanonPath::root : CanonPath("selected");
            auto logical = logicalRoot ? CanonPath::root : CanonPath("continued");
            source->addFile(from / CanonPath("drop/hidden"), "unobserved");
            source->addFile(from / "keep", "kept");
            std::vector<std::string> calls;
            PathFilter filter = [&](const std::string & path) {
                calls.push_back(path);
                return path == (from / "keep").abs();
            };
            HashingVisitor visitor;
            EXPECT_NO_THROW(traverse(*source, from, filter, visitor, logical));
            EXPECT_EQ(calls, (std::vector<std::string>{(from / "drop").abs(), (from / "keep").abs()}));
            EXPECT_EQ(source->observed, (std::vector<CanonPath>{from, from / "keep"}));
            ++cases;
        }
    EXPECT_EQ(cases, 4u);
    RecordProperty("finite_cells", cases);
}

TEST(CopyPool, orderedFoldChoosesTheFirstFailureAndJoinsEveryAcceptedChild)
{
    /* Zero through three children: each succeeds, fails while reading, or
       fails while folding. Enumerate every assignment and every advisory
       scheduling mask, including unknown types (the all-inline mask). */
    size_t cases = 0;
    for (size_t workers = 1; workers <= 8; ++workers) {
        CopyPool pool(workers);
        for (size_t count = 0; count <= 3; ++count)
            for (unsigned mask = 0; mask < (1u << count); ++mask)
                for (unsigned outcomes = 0; outcomes < std::array<unsigned, 4>{1, 3, 9, 27}[count]; ++outcomes) {
                    SCOPED_TRACE(fmt("workers=%d count=%d mask=%d outcomes=%d", workers, count, mask, outcomes));
                    std::array<unsigned, 3> outcome;
                    auto rest = outcomes;
                    for (auto & value : outcome) {
                        value = rest % 3;
                        rest /= 3;
                    }
                    std::atomic<unsigned> active{0}, started{0}, completed{0};
                    std::vector<size_t> consumed;
                    std::vector<std::string> baseCaseTrace;
                    auto caller = std::this_thread::get_id();
                    bool baseCaseOnCaller = true;
                    std::string failure;
                    try {
                        pool.ordered(
                            count,
                            [&](size_t i) {
                                if (count < 2)
                                    baseCaseTrace.push_back("canFork");
                                return mask & (1u << i);
                            },
                            [&](size_t i) {
                                if (count < 2) {
                                    baseCaseTrace.push_back("run");
                                    baseCaseOnCaller = std::this_thread::get_id() == caller;
                                }
                                ++active;
                                ++started;
                                Finally done{[&] {
                                    --active;
                                    ++completed;
                                }};
                                if (outcome[i] == 1)
                                    throw std::runtime_error(fmt("read-%d", i));
                            },
                            [&](size_t i) {
                                if (count < 2) {
                                    baseCaseTrace.push_back("consume");
                                    baseCaseOnCaller &= std::this_thread::get_id() == caller;
                                }
                                if (outcome[i] == 2)
                                    throw std::runtime_error(fmt("fold-%d", i));
                                consumed.push_back(i);
                            });
                    } catch (const std::runtime_error & e) {
                        failure = e.what();
                    }
                    std::vector<size_t> expected;
                    std::string expectedFailure;
                    for (size_t i = 0; i < count; ++i) {
                        if (outcome[i]) {
                            expectedFailure = fmt("%s-%d", outcome[i] == 1 ? "read" : "fold", i);
                            break;
                        }
                        expected.push_back(i);
                    }
                    EXPECT_EQ(failure, expectedFailure);
                    EXPECT_EQ(consumed, expected);
                    EXPECT_EQ(active.load(), 0u);
                    EXPECT_EQ(started.load(), completed.load());
                    if (count < 2) {
                        std::vector<std::string> expectedTrace;
                        if (count) {
                            expectedTrace.push_back("run");
                            if (outcome[0] != 1)
                                expectedTrace.push_back("consume");
                        }
                        EXPECT_EQ(baseCaseTrace, expectedTrace);
                        EXPECT_TRUE(baseCaseOnCaller);
                    }
                    ++cases;
                }
    }
    EXPECT_EQ(cases, 2072u);
}

TEST(CopyPool, predicateFailureJoinsTheLaunchedPrefixWithoutConsumingIt)
{
    /* The first workers - 1 visits are admitted from an empty queue, so
       their failures cannot stop launch or replace a reached predicate failure.
       With one worker, the first visit fails inline and stops launch instead.
       Every launched visit must finish before its captures can expire. */
    size_t cases = 0;
    for (size_t workers = 1; workers <= 8; ++workers) {
        CopyPool pool(workers);
        for (bool failPrefix : {false, true})
            for (size_t count = 2; count <= 4; ++count)
                for (size_t throwIndex = 0; throwIndex < count; ++throwIndex) {
                    SCOPED_TRACE(
                        fmt("workers=%d count=%d throwIndex=%d failPrefix=%d", workers, count, throwIndex, failPrefix));
                    std::atomic<size_t> active{0}, started{0}, completed{0}, childFailures{0};
                    std::vector<size_t> predicates, consumed;
                    std::string failure;
                    try {
                        pool.ordered(
                            count,
                            [&](size_t i) {
                                predicates.push_back(i);
                                if (i == throwIndex)
                                    throw std::runtime_error("predicate");
                                return true;
                            },
                            [&](size_t i) {
                                ++active;
                                ++started;
                                Finally done{[&] {
                                    --active;
                                    ++completed;
                                }};
                                if (failPrefix && i < std::max<size_t>(workers - 1, 1)) {
                                    ++childFailures;
                                    throw std::runtime_error("child");
                                }
                            },
                            [&](size_t i) { consumed.push_back(i); });
                    } catch (const std::runtime_error & e) {
                        failure = e.what();
                    }
                    bool inlineFailure = failPrefix && workers == 1 && throwIndex != 0;
                    std::vector<size_t> expectedPredicates;
                    for (size_t i = 0; i <= (inlineFailure ? 0 : throwIndex); ++i)
                        expectedPredicates.push_back(i);
                    EXPECT_EQ(failure, inlineFailure ? "child" : "predicate");
                    EXPECT_EQ(predicates, expectedPredicates);
                    EXPECT_TRUE(consumed.empty());
                    EXPECT_EQ(active.load(), 0u);
                    EXPECT_EQ(started.load(), inlineFailure ? 1 : throwIndex);
                    EXPECT_EQ(completed.load(), inlineFailure ? 1 : throwIndex);
                    EXPECT_EQ(
                        childFailures.load(), failPrefix ? std::min(throwIndex, std::max<size_t>(workers - 1, 1)) : 0u);
                    ++cases;
                }
    }
    EXPECT_EQ(cases, 144u);
}

TEST(CopyPool, failureObjectsMayReenterThePoolDuringCleanup)
{
    /* An exception can own cleanup which itself visits children. Discarding
       a non-winning error must neither hold the pool lock nor outlive the
       joined child scope. Sharing the cleanup tolerates exception copies. */
    struct Cleanup
    {
        CopyPool & pool;
        std::atomic<size_t> & released;
        std::atomic<size_t> & visits;
        std::atomic<size_t> & folds;

        ~Cleanup()
        {
            pool.ordered(2, [](size_t) { return false; }, [&](size_t) { ++visits; }, [&](size_t) { ++folds; });
            ++released;
        }
    };

    struct Failure
    {
        size_t index;
        std::shared_ptr<Cleanup> cleanup;
    };

    size_t cases = 0;
    for (size_t workers = 1; workers <= 8; ++workers) {
        CopyPool pool(workers);
        for (size_t count : {2u, 3u}) {
            SCOPED_TRACE(fmt("workers=%d count=%d", workers, count));
            std::atomic<size_t> started{0}, released{0}, visits{0}, folds{0};
            size_t failureIndex = count, consumed = 0;
            try {
                pool.ordered(
                    count,
                    [](size_t) { return true; },
                    [&](size_t i) {
                        ++started;
                        throw Failure{i, std::make_shared<Cleanup>(pool, released, visits, folds)};
                    },
                    [&](size_t) { ++consumed; });
            } catch (const Failure & failure) {
                failureIndex = failure.index;
            }
            EXPECT_EQ(failureIndex, 0u);
            EXPECT_EQ(consumed, 0u);
            EXPECT_GE(started.load(), workers > 1 ? 2u : 1u);
            EXPECT_EQ(released.load(), started.load());
            EXPECT_EQ(visits.load(), 2 * started.load());
            EXPECT_EQ(folds.load(), 2 * started.load());
            ++cases;
        }
    }
    EXPECT_EQ(cases, 16u);
}

TEST(CopyPool, nestedScopesJoinBeforeReturning)
{
    for (size_t workers = 1; workers <= 8; ++workers) {
        CopyPool pool(workers);
        for (unsigned depth = 0; depth <= 3; ++depth) {
            SCOPED_TRACE(fmt("workers=%d depth=%d", workers, depth));
            auto count = [&](this const auto & recurse, unsigned remaining) -> size_t {
                if (!remaining)
                    return 1;
                std::array<size_t, 3> children{};
                size_t result = 1;
                pool.ordered(
                    3,
                    [](size_t) { return true; },
                    [&](size_t i) { children[i] = recurse(remaining - 1); },
                    [&](size_t i) { result += children[i]; });
                return result;
            };
            EXPECT_EQ(count(depth), (std::array<size_t, 4>{1, 4, 13, 40}[depth]));
        }
    }
}

TEST(ObjectHashSink, permittedPullFoldMatchesSerialAndNarAcrossTheFiniteMetadataDomain)
{
    struct Source final : MemorySourceAccessor
    {
        bool typed = true, sized = true;

        bool allowsConcurrentTraversal() override
        {
            return true;
        }

        DirEntries readDirectory(const CanonPath & path) override
        {
            auto entries = MemorySourceAccessor::readDirectory(path);
            if (!typed)
                for (auto & [name, type] : entries)
                    type.reset();
            return entries;
        }

        std::optional<Stat> maybeLstat(const CanonPath & path) override
        {
            auto st = MemorySourceAccessor::maybeLstat(path);
            if (st && !sized)
                st->fileSize.reset();
            return st;
        }
    };

    Finally restoreWorkers{
        [old = HashingVisitor::workersForTest.load()] { HashingVisitor::workersForTest.store(old); }};
    size_t cases = 0;
    for (size_t workers = 1; workers <= 8; ++workers)
        for (bool typed : {false, true})
            for (bool sized : {false, true})
                for (bool hacked : {false, true}) {
                    WithCaseHack hack{hacked};
                    auto source = make_ref<Source>();
                    source->typed = typed;
                    source->sized = sized;
                    source->addFile(CanonPath("a/x"), "plain");
                    source->addFile(CanonPath("a/z"), "second leaf");
                    source->addFile(CanonPath("b/y"), "second");
                    source->addFile(CanonPath("Foo"), "upper");
                    source->addFile(
                        CanonPath::root / (hacked ? "foo" + std::string(caseHackSuffix) + "1" : "foo"), "lower");
                    MemorySink sink{*source};
                    sink.createDirectory(CanonPath("empty"));
                    sink.createRegularFile(CanonPath("exec"), [](CreateRegularFileSink & file) {
                        file.isExecutable();
                        file("executable");
                    });
                    sink.createSymlink(CanonPath("link"), "a/x");
                    for (auto root : {CanonPath::root, CanonPath("a/x"), CanonPath("exec"), CanonPath("link")}) {
                        SCOPED_TRACE(
                            fmt("workers=%d typed=%d sized=%d hack=%d root=%s", workers, typed, sized, hacked, root));
                        auto nar = narOf(*source, root);
                        StringSource input{nar};
                        auto pushed = objectHashOfNar(input);
                        HashingVisitor serial;
                        auto reference = objectHashOf(*source, root, defaultPathFilter, serial);
                        HashingVisitor::workersForTest.store(workers);
                        PathFilter copied = defaultPathFilter;
                        auto actual = objectHashOf(*source, root, copied);
                        EXPECT_EQ(actual.root, reference.root);
                        EXPECT_EQ(actual.root, pushed.root);
                        EXPECT_TRUE(actual.narSize.has_value());
                        EXPECT_EQ(actual.narSize, nar.size());
                        ++cases;
                    }
                }
    EXPECT_EQ(cases, 256u);
}

/* A pool used by an earlier directory must not change the first error of
   a later directory whose children are all scheduled inline. The oracle is
   the first child index, with its read preceding its fold, not a read trace. */
TEST(ObjectHashSink, warmHasherChoosesTheFirstChildFailure)
{
    enum class Outcome { Success, ReadFailure, FoldFailure };
    const auto outcomes = {Outcome::Success, Outcome::ReadFailure, Outcome::FoldFailure};
    const HashedNode file{
        .entry = {merkle::Mode::Regular, merkle::blobId("")}, .narSize = merkle::nar::regular(0, false)};
    const HashedNode empty{
        .entry = {merkle::Mode::Directory, merkle::treeId("")}, .narSize = merkle::nar::Directory{}.finish()};
    size_t cases = 0;
    for (size_t workers = 1; workers <= 8; ++workers)
        for (auto first : outcomes)
            for (auto second : outcomes) {
                HashingVisitor visitor{workers};
                /* These synthetic visits borrow state outside each entire
                   directory call, so they have the pull driver's lifetime. */
                visitor.directory(
                    CanonPath("warm"),
                    HashingVisitor::Children{
                        [&](auto each) {
                            each("directory", true, [&](auto &) { return empty; });
                            each("file", false, [&](auto &) { return file; });
                        },
                        /*canDefer=*/true});
                const std::array selected{first, second};
                const std::array names{"first", "second"};
                std::atomic<size_t> active{0}, started{0}, completed{0};
                std::string failure, message;
                try {
                    visitor.directory(
                        CanonPath("checked"),
                        HashingVisitor::Children{
                            [&](auto each) {
                                for (size_t i = 0; i < selected.size(); ++i)
                                    each(names[i], false, [&, i](auto &) {
                                        ++active;
                                        ++started;
                                        Finally done{[&] {
                                            --active;
                                            ++completed;
                                        }};
                                        if (selected[i] == Outcome::ReadFailure)
                                            throw std::runtime_error(names[i]);
                                        auto node = file;
                                        if (selected[i] == Outcome::FoldFailure)
                                            node.entry.hash = Hash{HashAlgorithm::SHA1};
                                        return node;
                                    });
                            },
                            /*canDefer=*/true});
                } catch (const Error & e) {
                    failure = "fold";
                    message = e.what();
                } catch (const std::runtime_error & e) {
                    failure = "read";
                    message = e.what();
                }
                auto expected = first != Outcome::Success    ? size_t{0}
                                : second != Outcome::Success ? size_t{1}
                                                             : size_t{2};
                if (expected == 2) {
                    EXPECT_TRUE(failure.empty());
                    EXPECT_EQ(started.load(), 2u);
                } else {
                    EXPECT_EQ(failure, selected[expected] == Outcome::ReadFailure ? "read" : "fold");
                    EXPECT_NE(message.find(names[expected]), std::string::npos);
                }
                EXPECT_EQ(active.load(), 0u);
                EXPECT_EQ(started.load(), completed.load());
                ++cases;
            }
    EXPECT_EQ(cases, 72u);

    /* A false hint can still descend into work which forks; true hints can
       describe regular nodes. Their values, not those hints, name the tree. */
    auto source = make_ref<MemorySourceAccessor>();
    source->addFile(CanonPath("directory/a"), "");
    source->addFile(CanonPath("directory/b"), "");
    source->addFile(CanonPath("file"), "");
    const auto expected = expectedRoot(source);
    for (size_t workers = 1; workers <= 8; ++workers) {
        HashingVisitor visitor{workers};
        auto result = visitor.directory(
            CanonPath::root,
            HashingVisitor::Children{
                [&](auto each) {
                    each("directory", false, [&](auto &) {
                        return visitor.directory(
                            CanonPath("directory"),
                            HashingVisitor::Children{
                                [&](auto nested) {
                                    nested("a", true, [&](auto &) { return file; });
                                    nested("b", true, [&](auto &) { return file; });
                                },
                                /*canDefer=*/true});
                    });
                    each("file", false, [&](auto &) { return file; });
                },
                /*canDefer=*/true});
        EXPECT_EQ(result.entry, expected);
        ASSERT_TRUE(result.narSize);
        EXPECT_EQ(merkle::nar::root(*result.narSize), narOf(*source).size());
    }
}

TEST(ObjectHashSink, permittedPullUsesWorkersButAnExplicitVisitorKeepsItsCallerThread)
{
    struct Source final : MemorySourceAccessor
    {
        bool gate = false;
        std::mutex mutex;
        std::condition_variable wake;
        bool first = false, second = false;
        std::thread::id caller;
        bool workerObserved = false;

        bool allowsConcurrentTraversal() override
        {
            return true;
        }

        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            if (gate) {
                std::unique_lock lock(mutex);
                if (path == CanonPath("a/x")) {
                    workerObserved = std::this_thread::get_id() != caller;
                    first = true;
                    wake.notify_all();
                    if (!wake.wait_for(lock, std::chrono::seconds(5), [&] { return second; }))
                        throw Error("parallel first child was not joined by its sibling");
                } else {
                    second = true;
                    wake.notify_all();
                    if (!wake.wait_for(lock, std::chrono::seconds(5), [&] { return first; }))
                        throw Error("parallel first child did not start");
                }
            }
            MemorySourceAccessor::readFile(path, sink, std::move(sizeCallback));
        }
    };

    struct EffectfulVisitor : HashingVisitor
    {
        std::mutex mutex;
        std::thread::id caller = std::this_thread::get_id();
        bool wrongThread = false;
        std::vector<CanonPath> calls;

        HashedNode regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override
        {
            {
                std::lock_guard lock(mutex);
                wrongThread |= std::this_thread::get_id() != caller;
                calls.push_back(path);
            }
            return HashingVisitor::regular(path, std::move(read));
        }
    };

    Finally restoreWorkers{
        [old = HashingVisitor::workersForTest.load()] { HashingVisitor::workersForTest.store(old); }};
    for (size_t workers = 1; workers <= 8; ++workers) {
        SCOPED_TRACE(workers);
        HashingVisitor::workersForTest.store(workers);
        auto source = make_ref<Source>();
        source->addFile(CanonPath("a/x"), "first");
        source->addFile(CanonPath("b"), "second");
        source->caller = std::this_thread::get_id();
        EffectfulVisitor visitor;
        auto reference = objectHashOf(*source, CanonPath::root, defaultPathFilter, visitor);
        EXPECT_FALSE(visitor.wrongThread);
        EXPECT_EQ(visitor.calls, (std::vector<CanonPath>{CanonPath("a/x"), CanonPath("b")}));
        source->gate = workers > 1;
        auto actual = objectHashOf(*source, CanonPath::root);
        EXPECT_EQ(actual.root, reference.root);
        EXPECT_EQ(source->workerObserved, workers > 1);
    }
}

/* (3) The case hack (01 section 9.11, "The names the hash is over").  The
   NAR of a tree with `Foo` and `foo` restored under the hack has `foo`
   renamed on disk; the sink hashes the tree's names, so the stream, the
   restored tree and the original agree.  With the hack off the restored
   tree's names are what they are, and the hash differs: the check
   discriminates. */
TEST(ObjectHashSink, case_hack_suffix_is_not_hashed)
{
    auto acc = make_ref<MemorySourceAccessor>();
    acc->addFile(CanonPath{"/Foo"}, "1");
    acc->addFile(CanonPath{"/foo"}, "2");
    auto nar = narOf(*acc);
    auto expected = expectedRoot(acc);

    auto restored = make_ref<MemorySourceAccessor>();
    {
        WithCaseHack hack{true};

        StringSource in{nar};
        auto r = objectHashOfNar(in);
        EXPECT_EQ(r.root, expected);
        EXPECT_EQ(r.narSize, nar.size());

        MemorySink sink{*restored};
        StringSource in2{nar};
        parseDump(sink, in2);

        /* The hack fired: the second name was rewritten on the way in. */
        auto hacked = std::string("foo") + std::string(caseHackSuffix) + "1";
        ASSERT_TRUE(restored->pathExists(CanonPath::root / "Foo"));
        ASSERT_TRUE(restored->pathExists(CanonPath::root / hacked));
        ASSERT_FALSE(restored->pathExists(CanonPath::root / "foo"));

        auto r2 = objectHashOf(*restored, CanonPath::root);
        EXPECT_EQ(r2.root, expected);
        EXPECT_EQ(r2.narSize, nar.size());
    }

    /* Negative control: with the hack off nothing strips the suffix, so the
       restored tree's names are outside the NAR's domain, and the walk and
       the serialiser refuse them rather than hash or write a tree no parser
       accepts. */
    {
        WithCaseHack hack{false};
        EXPECT_THROW(objectHashOf(*restored, CanonPath::root), Error);
        EXPECT_THROW(narOf(*restored), Error);
    }
}

/* The filter is applied as `dumpPath` applies it: the same admitted
   subset, the same NAR length. */
TEST(ObjectHashSink, filter_matches_dumpPath)
{
    auto acc = complexTree();

    PathFilter filter = [](const std::string & path) { return path != "/big" && path != "/a/b"; };

    StringSink nar;
    acc->dumpPath(CanonPath::root, nar, filter);
    auto expected = referenceObjectEntry(*acc, CanonPath::root, filter);

    auto r = objectHashOf(*acc, CanonPath::root, filter);
    EXPECT_EQ(r.root, expected);
    EXPECT_EQ(r.narSize, nar.s.size());

    /* And it did filter: the unfiltered hash differs. */
    EXPECT_NE(r.root, expectedRoot(acc));
}

/* The flat `createDirectory` form: a directory's children arrive inside
   the callback form's scope, and no driver of this sink uses the flat form
   (`parseDump` and the tee call back), so it is refused rather than left
   half-supported. */
TEST(ObjectHashSink, flat_createDirectory_form_is_refused)
{
    HashingVisitor objectVisitor;
    ObjectHashSink sink{objectVisitor};
    EXPECT_THROW(sink.createDirectory(CanonPath::root), Error);
}

/* An empty sink has no root to report. */
TEST(ObjectHashSink, finish_without_root_throws)
{
    HashingVisitor objectVisitor;
    ObjectHashSink sink{objectVisitor};
    EXPECT_THROW(sink.finish(), Error);
}

/* A driver that never announces a size (neither `parseDump` nor
   `copyRecursive`): the bytes are buffered and hashed whole, to the same
   blob id and NAR size as the announced route. */
TEST(ObjectHashSink, regular_file_without_announced_size)
{
    StringSink nar;
    dumpString("abc", nar);

    HashingVisitor objectVisitor;
    ObjectHashSink sink{objectVisitor};
    sink.createRegularFile(CanonPath::root, [](CreateRegularFileSink & crf) {
        crf("a");
        crf("bc");
    });
    auto r = sink.finish();
    EXPECT_EQ(r.root, (merkle::TreeEntry{.mode = merkle::Mode::Regular, .hash = merkle::blobId("abc")}));
    EXPECT_EQ(r.narSize, nar.s.size());
}

/* A size announced and then not met is refused rather than hashed wrong. */
TEST(ObjectHashSink, short_delivery_against_announced_size_throws)
{
    HashingVisitor objectVisitor;
    ObjectHashSink sink{objectVisitor};
    EXPECT_THROW(
        sink.createRegularFile(
            CanonPath::root,
            [](CreateRegularFileSink & crf) {
                crf.preallocateContents(3);
                crf("ab");
            }),
        Error);
}

/* One delivered regular node has the same meaning independently of the size
   announcement, chunk boundaries, or executable-marker position. */
namespace {

enum class SizePhase { Absent, Early, AfterEmpty, Mid, End };
struct RegularEvent
{
    bool announce;
    std::string_view bytes = {};
    uint64_t size = 0;
};

std::vector<RegularEvent> regularEvents(std::string_view bytes, SizePhase phase, bool zeros, uint64_t size)
{
    std::vector<RegularEvent> events;
    auto announce = [&] { events.push_back({true, {}, size}); };
    auto chunk = [&](std::string_view data) { events.push_back({false, data}); };
    if (phase == SizePhase::Early)
        announce();
    if (zeros || phase == SizePhase::AfterEmpty)
        chunk({});
    if (phase == SizePhase::AfterEmpty)
        announce();
    chunk(bytes.substr(0, bytes.size() / 2));
    if (zeros)
        chunk({});
    if (phase == SizePhase::Mid)
        announce();
    chunk(bytes.substr(bytes.size() / 2));
    if (zeros)
        chunk({});
    if (phase == SizePhase::End)
        announce();
    return events;
}

HashedNode deliverRegular(
    bool direct, const std::vector<RegularEvent> & events, size_t executableAt, size_t duplicateAt)
{
    const CanonPath path{"/file"};
    auto read = [&](CreateRegularFileSink & sink) {
        for (size_t position = 0; position <= events.size(); ++position) {
            if (position == executableAt)
                sink.isExecutable();
            if (position == duplicateAt) {
                auto first = std::find_if(events.begin(), events.end(), [](auto & event) { return event.announce; });
                EXPECT_THROW(sink.preallocateContents(first->size + 1), Error);
            }
            if (position < events.size()) {
                auto & event = events[position];
                if (event.announce)
                    sink.preallocateContents(event.size);
                else
                    sink(event.bytes);
            }
        }
    };
    if (direct) {
        HashingRegularFileSink sink{path};
        read(sink);
        return sink.finish();
    }
    return HashingVisitor{}.regular(path, std::ref(read));
}

static_assert(std::is_constructible_v<HashingRegularFileSink, const CanonPath &>);
static_assert(!std::is_constructible_v<HashingRegularFileSink, CanonPath &&>);

} // namespace

TEST(HashingRegularFileSink, delivery_protocol_matches_reference_and_nar)
{
    for (size_t length : {0, 1, 15, 16, 64}) {
        std::string bytes(length, '\0');
        for (size_t i = 0; i < length; ++i)
            bytes[i] = char(i);
        for (auto phase : {SizePhase::Absent, SizePhase::Early, SizePhase::AfterEmpty, SizePhase::Mid, SizePhase::End})
            for (bool zeros : {false, true}) {
                auto events = regularEvents(bytes, phase, zeros, length);
                /* Every event boundary plus the absent executable marker. */
                for (size_t executableAt = 0; executableAt <= events.size() + 1; ++executableAt)
                    for (size_t duplicateAt = 0; duplicateAt <= events.size() + 1; ++duplicateAt) {
                        /* A duplicate can only follow the first announcement. */
                        if (duplicateAt <= events.size()
                            && std::none_of(events.begin(), events.begin() + duplicateAt,
                                [](auto & event) { return event.announce; }))
                            continue;
                        auto source = make_ref<MemorySourceAccessor>();
                        source->root = File{File::Regular{
                            .executable = executableAt <= events.size(), .contents = bytes}};
                        auto expected = referenceObjectEntry(*source, CanonPath::root);
                        auto narSize = narOf(*source).size();
                        for (bool direct : {false, true}) {
                            SCOPED_TRACE(::testing::Message() << length << ':' << int(phase) << ':' << zeros
                                << ':' << executableAt << ':' << duplicateAt << ':' << direct);
                            auto node = deliverRegular(direct, events, executableAt, duplicateAt);
                            EXPECT_EQ(node.entry, expected);
                            ASSERT_TRUE(node.narSize);
                            EXPECT_EQ(merkle::nar::root(*node.narSize), narSize);
                        }
                    }
            }
    }
}

TEST(HashingRegularFileSink, short_and_long_delivery_are_rejected_at_every_announcement_phase)
{
    for (size_t length : {0, 1, 15, 16, 64})
        for (auto phase : {SizePhase::Early, SizePhase::AfterEmpty, SizePhase::Mid, SizePhase::End})
            for (bool zeros : {false, true})
                for (int delta : {-1, 1}) {
                    if (!length && delta < 0)
                        continue;
                    std::string bytes(length, '\0');
                    for (size_t i = 0; i < length; ++i)
                        bytes[i] = char(i);
                    auto events = regularEvents(bytes, phase, zeros, length + delta);
                    for (size_t executableAt = 0; executableAt <= events.size() + 1; ++executableAt)
                        for (bool direct : {false, true}) {
                            SCOPED_TRACE(::testing::Message() << length << ':' << int(phase) << ':' << zeros
                                << ':' << delta << ':' << executableAt << ':' << direct);
                            EXPECT_THROW(deliverRegular(direct, events, executableAt, events.size() + 1), Error);
                        }
                }
}

/* `RestoreSink`'s two-phase regular file (`beginRegularFile`, then
   `finish`) makes the file `createRegularFile` makes -- the bytes, the
   mode, the hooks fired once with the flag -- at the root (no `dirFd`) and
   in a directory (the typed callback form's sub-sink). */
TEST(RestoreSink, beginRegularFile_then_finish_equals_createRegularFile)
{
    struct Hooks : RestoreSinkHooks
    {
        std::vector<std::pair<Descriptor, bool>> files;

        void directoryDone(Descriptor) override {}

        void regularFileCreated(Descriptor fd, bool executable) override
        {
            files.emplace_back(fd, executable);
        }

        void symlinkCreated(Descriptor, const CanonPath &) override {}
    };

    auto tmpDir = createTempDir();
    AutoDelete delTmpDir(tmpDir, /*recursive=*/true);

    auto modeOf = [](const std::filesystem::path & p) { return lstat(p).st_mode & ~S_IFMT; };

    /* At the root: the destination is the file itself. */
    {
        Hooks hooks;
        RestoreSink sink(/*startFsync=*/false, &hooks);
        sink.dstPath = tmpDir / "one-phase";
        sink.createRegularFile(CanonPath::root, [](CreateRegularFileSink & crf) {
            crf.isExecutable();
            crf.preallocateContents(5);
            crf("hel");
            crf("lo");
        });
        ASSERT_EQ(hooks.files.size(), 1u);
        EXPECT_TRUE(hooks.files[0].second);
    }
    {
        Hooks hooks;
        RestoreSink sink(/*startFsync=*/false, &hooks);
        sink.dstPath = tmpDir / "two-phase";
        auto writer = sink.beginRegularFile(CanonPath::root);
        EXPECT_TRUE(hooks.files.empty());
        writer.isExecutable();
        writer.preallocateContents(5);
        writer("hel");
        writer("lo");
        EXPECT_TRUE(hooks.files.empty());
        auto fd = writer.fd;
        writer.finish();
        EXPECT_EQ(writer.fd, INVALID_DESCRIPTOR);
        EXPECT_THROW(getFileSize(fd), SystemError);
        EXPECT_EQ(writer.written, 5u);
        ASSERT_EQ(hooks.files.size(), 1u);
        EXPECT_TRUE(hooks.files[0].second);
        /* Flushed by `finish`: readable before the writer is destroyed. */
        EXPECT_EQ(readFile(tmpDir / "two-phase"), "hello");
    }
    EXPECT_EQ(readFile(tmpDir / "one-phase"), "hello");
    EXPECT_EQ(modeOf(tmpDir / "one-phase"), modeOf(tmpDir / "two-phase"));
    EXPECT_TRUE(modeOf(tmpDir / "two-phase") & S_IXUSR);

    /* In a directory, through the typed callback form's sub-sink, whose
       children are `/name`. */
    {
        Hooks hooks;
        RestoreSink sink(/*startFsync=*/false, &hooks);
        sink.dstPath = tmpDir / "dir";
        sink.createDirectory(CanonPath::root, [&](FileSystemObjectSink & root, const CanonPath & rel) {
            EXPECT_EQ(rel, CanonPath::root);
            sink.createSubdirectory(CanonPath("/sub"), [&](RestoreSink & sub, const CanonPath & subRel) {
                EXPECT_EQ(subRel, CanonPath::root);
                EXPECT_TRUE(sub.dirFd);
                EXPECT_EQ(sub.dstPath, tmpDir / "dir" / "sub");
                auto writer = sub.beginRegularFile(CanonPath("/f"));
                writer("plain");
                auto fd = writer.fd;
                writer.finish();
                EXPECT_EQ(writer.fd, INVALID_DESCRIPTOR);
                EXPECT_THROW(getFileSize(fd), SystemError);
                EXPECT_EQ(writer.written, 5u);
                sub.createRegularFile(CanonPath("/g"), [](CreateRegularFileSink & crf) { crf("plain"); });
            });
        });
        ASSERT_EQ(hooks.files.size(), 2u);
        EXPECT_FALSE(hooks.files[0].second);
        EXPECT_FALSE(hooks.files[1].second);
    }
    EXPECT_EQ(readFile(tmpDir / "dir/sub/f"), "plain");
    EXPECT_EQ(readFile(tmpDir / "dir/sub/g"), "plain");
    EXPECT_EQ(modeOf(tmpDir / "dir/sub/f"), modeOf(tmpDir / "dir/sub/g"));
    EXPECT_FALSE(modeOf(tmpDir / "dir/sub/f") & S_IXUSR);
}

/* Failure preserves the original exception and closes the writer. Bytes
   buffered before a read failure still reach the file; completion hooks run
   only after a successful read, with flushed bytes and a live descriptor. */
TEST(RestoreSink, regularFileFailuresFlushAndCloseWithoutRepeatingCompletion)
{
    struct Failure
    {};

    struct Hooks : RestoreSinkHooks
    {
        std::function<void(Descriptor, bool)> completed;

        void directoryDone(Descriptor) override {}

        void symlinkCreated(Descriptor, const CanonPath &) override {}

        void regularFileCreated(Descriptor fd, bool executable) override
        {
            completed(fd, executable);
        }
    };

    auto tmpDir = createTempDir();
    AutoDelete delTmpDir(tmpDir, /*recursive=*/true);
    const std::string bytes = "buffered bytes";
    size_t cases = 0;
    for (bool lazy : {false, true})
        for (bool hookFailure : {false, true})
            for (bool nested : {false, true})
                for (bool executable : {false, true}) {
                    SCOPED_TRACE(
                        ::testing::Message() << "lazy=" << lazy << " hookFailure=" << hookFailure
                                             << " nested=" << nested << " executable=" << executable);
                    auto failure = std::make_exception_ptr(Failure{});
                    Descriptor observed = INVALID_DESCRIPTOR;
                    size_t hookCalls = 0;
                    Hooks hooks;
                    RestoreSink sink(/*startFsync=*/false, &hooks);
                    sink.dstPath = tmpDir / std::to_string(cases++);
                    if (nested)
                        sink.createDirectory(CanonPath::root);
                    auto path = nested ? CanonPath("/file") : CanonPath::root;
                    auto file = nested ? sink.dstPath / "file" : sink.dstPath;
                    hooks.completed = [&](Descriptor fd, bool seenExecutable) {
                        ++hookCalls;
                        EXPECT_EQ(fd, observed);
                        EXPECT_EQ(getFileSize(fd), bytes.size());
                        EXPECT_EQ(readFile(file), bytes);
#ifndef _WIN32
                        EXPECT_EQ(seenExecutable, executable);
#else
                        EXPECT_FALSE(seenExecutable);
#endif
                        std::rethrow_exception(failure);
                    };
                    auto write = [&](CreateRegularFileSink & crf) {
                        observed = dynamic_cast<FdSink &>(crf).fd;
                        if (executable)
                            crf.isExecutable();
                        crf(bytes);
                        EXPECT_EQ(getFileSize(observed), 0u) << "the bytes still need flushing";
                        if (!hookFailure)
                            std::rethrow_exception(failure);
                    };
                    try {
                        if (lazy) {
                            auto writer = sink.beginRegularFile(path);
                            write(writer);
                            writer.finish();
                        } else
                            sink.createRegularFile(path, write);
                        ADD_FAILURE() << "the injected exception was lost";
                    } catch (...) {
                        EXPECT_EQ(std::current_exception(), failure);
                    }
                    EXPECT_NE(observed, INVALID_DESCRIPTOR);
                    EXPECT_EQ(hookCalls, hookFailure ? 1u : 0u);
                    /* Check closure before opening the file could reuse its descriptor. */
                    EXPECT_THROW(getFileSize(observed), SystemError);
                    EXPECT_EQ(readFile(file), bytes);
                }
    EXPECT_EQ(cases, 16u);
}

/* A `FileSystemObjectSink` of one's own, composed over `RestoreSink`, is
   handed every level: the sub-sink `createDirectory`'s callback gives back
   is the sink the directory's children are created on, so a composite that
   wraps it sees `createRegularFile` for a file two directories down, and
   the hooks fire for every directory, not for the root alone.  The
   composite here keeps no convention of its own -- it names a node by the
   directories it has entered, as `copyRecursive`'s visitor does -- so the
   same composite is driven over `MemorySink` too (the base convention: the
   same sink, the full path) and must see the same tree.  The copy
   visitor's own form is `CopyRecursive.copies_a_tree_under_both_sink_
   conventions`; this is the composition a caller writes. */
TEST(RestoreSink, derivedSinkSeesEveryLevel)
{
    struct Seen
    {
        /* The directories entered, innermost last: the composite's only
           state, so that it depends on no sink's convention. */
        std::vector<std::string> entered;
        std::vector<std::string> files, dirs, symlinks;
    };

    struct Watching : FileSystemObjectSink
    {
        FileSystemObjectSink & inner;
        Seen & seen;

        Watching(FileSystemObjectSink & inner, Seen & seen)
            : inner(inner)
            , seen(seen)
        {
        }

        std::string whole(const CanonPath & path) const
        {
            auto p = CanonPath::root;
            for (auto & name : seen.entered)
                p = p / name;
            if (!path.isRoot())
                p = p / *path.baseName();
            return p.abs();
        }

        void createDirectory(const CanonPath & path) override
        {
            inner.createDirectory(path);
        }

        void createDirectory(const CanonPath & path, DirectoryCreatedCallback callback) override
        {
            seen.dirs.push_back(whole(path));
            std::optional<std::string> name;
            if (!path.isRoot())
                name = std::string(*path.baseName());
            inner.createDirectory(path, [&](FileSystemObjectSink & sub, const CanonPath & rel) {
                if (name)
                    seen.entered.push_back(*name);
                Finally pop{[&] {
                    if (name)
                        seen.entered.pop_back();
                }};
                Watching wrapped{sub, seen};
                callback(wrapped, rel);
            });
        }

        void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)> func) override
        {
            seen.files.push_back(whole(path));
            inner.createRegularFile(path, func);
        }

        void createSymlink(const CanonPath & path, const std::string & target) override
        {
            seen.symlinks.push_back(whole(path));
            inner.createSymlink(path, target);
        }
    };

    struct Hooks : RestoreSinkHooks
    {
        unsigned directories = 0, files = 0, symlinks = 0;

        void directoryDone(Descriptor) override
        {
            directories++;
        }

        void regularFileCreated(Descriptor, bool) override
        {
            files++;
        }

        void symlinkCreated(Descriptor, const CanonPath &) override
        {
            symlinks++;
        }
    };

    auto src = complexTree();
    auto nar = narOf(*src);

    const std::vector<std::string> expectedFiles{"/a/b/c.txt", "/a/empty-file", "/big", "/bin/run"};
    const std::vector<std::string> expectedDirs{"/", "/a", "/a/b", "/a/empty-dir-below", "/bin", "/empty-dir"};
    const std::vector<std::string> expectedSymlinks{"/link"};

    auto tmpDir = createTempDir();
    AutoDelete delTmpDir(tmpDir, /*recursive=*/true);

    Seen overRestore;
    Hooks hooks;
    {
        RestoreSink restore(/*startFsync=*/false, &hooks);
        restore.dstPath = tmpDir / "tree";
        Watching watching{restore, overRestore};
        copyRecursive(*src, CanonPath::root, watching, CanonPath::root);
    }
    EXPECT_EQ(narOf(*makeFSSourceAccessor(tmpDir / "tree")), nar) << "the tree the source names";

    std::sort(overRestore.files.begin(), overRestore.files.end());
    std::sort(overRestore.dirs.begin(), overRestore.dirs.end());
    EXPECT_EQ(overRestore.files, expectedFiles) << "every regular file, at every level";
    EXPECT_EQ(overRestore.dirs, expectedDirs);
    EXPECT_EQ(overRestore.symlinks, expectedSymlinks);

    /* The hooks see every level too: one `directoryDone` per directory,
       which is what canonicalises a subdirectory's mode and timestamp. */
    EXPECT_EQ(hooks.directories, expectedDirs.size());
    EXPECT_EQ(hooks.files, expectedFiles.size());
    EXPECT_EQ(hooks.symlinks, expectedSymlinks.size());

    /* The same composite over the base convention. */
    Seen overMemory;
    auto dst = make_ref<MemorySourceAccessor>();
    MemorySink memory{*dst};
    Watching watching{memory, overMemory};
    copyRecursive(*src, CanonPath::root, watching, CanonPath::root);
    EXPECT_EQ(narOf(*dst), nar);

    std::sort(overMemory.files.begin(), overMemory.files.end());
    std::sort(overMemory.dirs.begin(), overMemory.dirs.end());
    EXPECT_EQ(overMemory.files, overRestore.files) << "the same tree under either convention";
    EXPECT_EQ(overMemory.dirs, overRestore.dirs);
    EXPECT_EQ(overMemory.symlinks, overRestore.symlinks);
}

/* `known` (the fetchers' subtree memo): a subdirectory the visitor
   answers is recorded under the answer and never descended -- its files
   are not read -- the root is the unmemoised root when the answer is right
   and another when it is wrong, the NAR size is marked inexact, and an
   answer under another algorithm is refused.  A regular file answered with
   its size from the stat keeps the size exact. */
TEST(ObjectHashSink, known_answers_without_reading)
{
    /* An accessor whose file reads beneath `a` throw and are counted. */
    struct Guarded : MemorySourceAccessor
    {
        CanonPath forbidden{"/a"};
        unsigned reads = 0;

        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            if (path.isWithin(forbidden))
                throw Error("read of '%s' under a directory the memo answered", path);
            reads++;
            MemorySourceAccessor::readFile(path, sink, sizeCallback);
        }

        using SourceAccessor::readFile;
    };

    auto fill = [](MemorySourceAccessor & acc) {
        acc.addFile(CanonPath{"/a/b/c.txt"}, "hello\n");
        acc.addFile(CanonPath{"/a/empty-file"}, "");
        acc.addFile(CanonPath{"/bin/run"}, "#!/bin/sh\nexit 0\n");
        std::get<File::Regular>(acc.open(CanonPath{"/bin/run"}, std::nullopt)->raw).executable = true;
        acc.open(CanonPath{"/link"}, File{File::Symlink{.target = "a/b/c.txt"}});
        acc.open(CanonPath{"/empty-dir"}, File{File::Directory{}});
    };
    auto plain = make_ref<MemorySourceAccessor>();
    fill(*plain);
    auto guarded = make_ref<Guarded>();
    fill(*guarded);

    auto expected = expectedRoot(plain);
    auto aEntry = referenceObjectEntry(*plain, CanonPath("/a"));
    auto nar = narOf(*plain);

    /* Without a `known` the walk reads under `a` and the accessor refuses. */
    EXPECT_THROW(objectHashOf(*guarded, CanonPath::root), Error);

    /* A memo of directories: `answer` says what a directory is known as. */
    struct Memo : HashingVisitor
    {
        std::function<std::optional<Hash>(const CanonPath &)> answer;
        std::vector<CanonPath> asked;
        std::vector<CanonPath> finished;

        HashedNode node(
            SourceAccessor &,
            const CanonPath &,
            const CanonPath & p,
            const SourceAccessor::Stat & st,
            function_ref<HashedNode()> descend) override
        {
            if (st.type != SourceAccessor::tDirectory)
                return descend();
            asked.push_back(p);
            if (auto id = answer(p))
                return HashedNode{.entry = {.mode = merkle::Mode::Directory, .hash = *id}};
            return descend();
        }

        HashedNode directory(const CanonPath & p, Children children) override
        {
            auto node = HashingVisitor::directory(p, std::move(children));
            finished.push_back(p);
            return node;
        }
    };

    Memo memo;
    memo.answer = [&](const CanonPath & p) -> std::optional<Hash> {
        if (p == CanonPath("/a"))
            return aEntry.hash;
        return std::nullopt;
    };

    auto r = objectHashOf(*guarded, CanonPath::root, defaultPathFilter, memo);
    EXPECT_EQ(r.root, expected);
    EXPECT_FALSE(r.narSize.has_value());
    /* The root and every directory not beneath the answered one were asked;
       nothing beneath `a` was. */
    EXPECT_EQ(
        memo.asked,
        (std::vector<CanonPath>{CanonPath::root, CanonPath("/a"), CanonPath("/bin"), CanonPath("/empty-dir")}));
    /* The answered directory is not hashed by the visitor; the others are. */
    EXPECT_EQ(memo.finished, (std::vector<CanonPath>{CanonPath("/bin"), CanonPath("/empty-dir"), CanonPath::root}));
    /* `bin/run` was read; nothing else is a regular file outside `a`. */
    EXPECT_EQ(guarded->reads, 1u);

    /* A wrong answer gives another root: the check discriminates. */
    memo.answer = [&](const CanonPath & p) -> std::optional<Hash> {
        if (p == CanonPath("/a"))
            return merkle::treeId("");
        return std::nullopt;
    };
    EXPECT_NE(objectHashOf(*guarded, CanonPath::root, defaultPathFilter, memo).root, expected);

    /* An answer under another algorithm is a bug in the caller, refused
       where it would enter a tree. */
    memo.answer = [&](const CanonPath & p) -> std::optional<Hash> {
        if (p == CanonPath("/a"))
            return hashString(HashAlgorithm::SHA1, "");
        return std::nullopt;
    };
    EXPECT_THROW(objectHashOf(*guarded, CanonPath::root, defaultPathFilter, memo), Error);

    /* The root answered: nothing is visited at all. */
    guarded->reads = 0;
    memo.asked.clear();
    memo.answer = [&](const CanonPath &) -> std::optional<Hash> { return expected.hash; };
    auto rRoot = objectHashOf(*guarded, CanonPath::root, defaultPathFilter, memo);
    EXPECT_EQ(rRoot.root, expected);
    EXPECT_FALSE(rRoot.narSize.has_value());
    EXPECT_EQ(memo.asked, (std::vector<CanonPath>{CanonPath::root}));
    EXPECT_EQ(guarded->reads, 0u);

    /* Regular files answered with the stat's size: exact, and the same NAR
       size as the full walk. */
    struct FileMemo : HashingVisitor
    {
        MemorySourceAccessor & plain;

        explicit FileMemo(MemorySourceAccessor & plain)
            : plain(plain)
        {
        }

        HashedNode node(
            SourceAccessor &,
            const CanonPath &,
            const CanonPath & p,
            const SourceAccessor::Stat & st,
            function_ref<HashedNode()> descend) override
        {
            if (st.type != SourceAccessor::tRegular)
                return descend();
            EXPECT_TRUE(st.fileSize.has_value());
            return HashedNode{
                .entry = referenceObjectEntry(plain, p),
                .narSize = merkle::nar::regular(*st.fileSize, st.isExecutable),
            };
        }
    } fileMemo{*plain};

    auto rFiles = objectHashOf(*plain, CanonPath::root, defaultPathFilter, fileMemo);
    EXPECT_EQ(rFiles.root, expected);
    EXPECT_TRUE(rFiles.narSize.has_value());
    EXPECT_EQ(rFiles.narSize, nar.size());
}

/* The identifiers are git's: the values `git rev-parse` gives for the same
   trees in `tests/functional/git-hashing/simple-sha256.sh` (test0, test1)
   in a repository made with `--object-format=sha256` (git 2.54), so this is
   the same external oracle without the repository.  The reference agrees.
   (This pin was under SHA-1 before the SHA-1 form of the git method went,
   01 section 10, *The git method is SHA-256 only*; `GitUtilsTest.sha256IdentifiersAgreeWithLibgit2`
   is the same equality against libgit2's SHA-256 object database.) */
TEST(ObjectHashSink, sha256_identifiers_are_gits)
{
    auto blob = make_ref<MemorySourceAccessor>();
    blob->root = File{File::Regular{.executable = false, .contents = "Hello World\n"}};
    auto r = objectHashOf(*blob, CanonPath::root);
    EXPECT_EQ(r.root.hash.algo, HashAlgorithm::SHA256);
    EXPECT_EQ(r.root.mode, merkle::Mode::Regular);
    EXPECT_EQ(
        merkle::objectHash(r.root).to_string(HashFormat::Base16, false),
        "7c5c8610459154bdde4984be72c48fb5d9c1c4ac793a6b5976fe38fd1b0b1284");
    EXPECT_EQ(r.narSize, narOf(*blob).size());

    auto tree = make_ref<MemorySourceAccessor>();
    tree->addFile(CanonPath{"/hello"}, "Hello World\n");
    tree->addFile(CanonPath{"/executable"}, "Run Hello World\n");
    std::get<File::Regular>(tree->open(CanonPath{"/executable"}, std::nullopt)->raw).executable = true;
    auto t = objectHashOf(*tree, CanonPath::root);
    EXPECT_EQ(t.root.hash.algo, HashAlgorithm::SHA256);
    EXPECT_EQ(
        merkle::objectHash(t.root).to_string(HashFormat::Base16, false),
        "cd79952f42462467d0ea574b0283bb6eb77e15b2b86891e29f2b981650365474");
    EXPECT_EQ(t.root, referenceObjectEntry(*tree, CanonPath::root));
    EXPECT_EQ(t.narSize, narOf(*tree).size());
}

/* (4) Over generated trees: `objectHashOfNar(dumpPath(t)) == reference(t)`
   and `narSize == |dumpPath(t)|`, through both routes. */
namespace {

struct GenTree
{
    File file;
};

std::ostream & operator<<(std::ostream & os, const GenTree & t)
{
    auto acc = make_ref<MemorySourceAccessor>();
    acc->root = t.file;
    return os << static_cast<nlohmann::json>(*acc).dump();
}

/* Names chosen so that byte order and git's order (a directory's key
   carries a trailing `/`) and a suffix that pads to different NAR lengths
   are all exercised; contents of several lengths, so are the paddings. */
FileGenSpec orderAndPaddingSpec()
{
    return {
        .names = {"a", "a.b", "B", "abcdefgh"},
        .contents = rc::gen::element(
            std::string(""), std::string("x"), std::string("yy"), std::string(7, 'p'), std::string(9, 'q')),
        .targets = rc::gen::element(std::string("t"), std::string("t/uv"), std::string("/abs/olute")),
    };
}

} // namespace

} // namespace nix

namespace rc {

template<>
struct Arbitrary<nix::GenTree>
{
    static Gen<nix::GenTree> arbitrary()
    {
        return gen::map(nix::genFile(3, nix::orderAndPaddingSpec()), [](nix::File f) {
            return nix::GenTree{.file = std::move(f)};
        });
    }
};

} // namespace rc

namespace nix {

/* A tee composes each sink's directory scope, including when only one
   side returns a new sink rooted at /. A failed child unwinds both scopes
   without reporting either enclosing directory as complete. */
TEST(TeeFileSystemObjectSink, composesEveryDirectoryConventionAndUnwindsBothScopes)
{
#ifdef _WIN32
    GTEST_SKIP() << "scoped RestoreSink directories and their hooks require Unix descriptors";
#else
    struct Failure
    {};

    struct Hooks : RestoreSinkHooks
    {
        std::vector<std::string> & events;
        std::string side;

        Hooks(std::vector<std::string> & events, std::string side)
            : events(events)
            , side(std::move(side))
        {
        }

        void directoryDone(Descriptor) override
        {
            events.push_back(side + "directory");
        }

        void regularFileCreated(Descriptor, bool) override
        {
            events.push_back(side + "file");
        }

        void symlinkCreated(Descriptor, const CanonPath &) override
        {
            events.push_back(side + "symlink");
        }
    };

    auto tmpDir = createTempDir();
    AutoDelete delTmpDir(tmpDir, /*recursive=*/true);
    size_t cases = 0;
    for (bool physicalA : {false, true})
        for (bool physicalB : {false, true})
            for (bool fail : {false, true}) {
                SCOPED_TRACE(
                    ::testing::Message() << "physicalA=" << physicalA << " physicalB=" << physicalB
                                         << " fail=" << fail);
                auto directory = tmpDir / std::to_string(cases++);
                createDirs(directory);
                auto a = make_ref<MemorySourceAccessor>(), b = make_ref<MemorySourceAccessor>();
                MemorySink memoryA{*a}, memoryB{*b};
                std::vector<std::string> events;
                Hooks hooksA{events, "a:"}, hooksB{events, "b:"};
                RestoreSink restoreA{/*startFsync=*/false, &hooksA}, restoreB{/*startFsync=*/false, &hooksB};
                restoreA.dstPath = directory / "a";
                restoreB.dstPath = directory / "b";
                FileSystemObjectSink & sinkA = physicalA ? static_cast<FileSystemObjectSink &>(restoreA) : memoryA;
                FileSystemObjectSink & sinkB = physicalB ? static_cast<FileSystemObjectSink &>(restoreB) : memoryB;
                TeeFileSystemObjectSink tee{sinkA, sinkB};
                auto failure = std::make_exception_ptr(Failure{});
                std::exception_ptr observed;
                size_t directories = 0, files = 0;
                try {
                    tee.createDirectory(CanonPath::root, [&](FileSystemObjectSink & root, const CanonPath & base) {
                        ++directories;
                        root.createDirectory(base / "nested", [&](FileSystemObjectSink & nested, const CanonPath & n) {
                            ++directories;
                            nested.createDirectory(n / "deep", [&](FileSystemObjectSink & deep, const CanonPath & d) {
                                ++directories;
                                deep.createRegularFile(d / "file", [&](CreateRegularFileSink & file) {
                                    ++files;
                                    file("payload");
                                });
                                deep.createSymlink(d / "link", "file");
                                if (fail)
                                    std::rethrow_exception(failure);
                            });
                        });
                        root.createRegularFile(base / "tail", [&](CreateRegularFileSink & file) {
                            ++files;
                            file("after");
                        });
                    });
                } catch (...) {
                    observed = std::current_exception();
                }
                EXPECT_EQ(observed, fail ? failure : std::exception_ptr{});
                EXPECT_EQ(directories, 3u);
                EXPECT_EQ(files, fail ? 1u : 2u) << "the producer runs once for both outputs";

                auto expected = make_ref<MemorySourceAccessor>();
                expected->addFile(CanonPath("/nested/deep/file"), "payload");
                expected->open(CanonPath("/nested/deep/link"), File{File::Symlink{.target = "file"}});
                if (!fail)
                    expected->addFile(CanonPath("/tail"), "after");
                auto nar = narOf(*expected);
                EXPECT_EQ(physicalA ? narOf(*makeFSSourceAccessor(restoreA.dstPath)) : narOf(*a), nar);
                EXPECT_EQ(physicalB ? narOf(*makeFSSourceAccessor(restoreB.dstPath)) : narOf(*b), nar);

                std::vector<std::string> expectedEvents;
                const std::array physical{physicalA, physicalB};
                auto completed = [&](std::string_view event, bool reverse) {
                    for (auto side : reverse ? std::array{1u, 0u} : std::array{0u, 1u})
                        if (physical[side])
                            expectedEvents.push_back(std::string(side ? "b:" : "a:") + std::string(event));
                };
                completed("file", true);
                completed("symlink", false);
                if (!fail) {
                    completed("directory", true);
                    completed("directory", true);
                    completed("file", true);
                    completed("directory", true);
                }
                EXPECT_EQ(events, expectedEvents);
            }
    EXPECT_EQ(cases, 8u);
#endif
}

/* Each endpoint may set or clear skipContents initially and after each
   metadata callback. Their six observed bits vary independently. Metadata
   reaches both; bytes are omitted only for the endpoints currently skipping. */
TEST(TeeFileSystemObjectSink, observesSkipContentsAfterEveryMetadataStage)
{
    struct Endpoint : NullFileSystemObjectSink
    {
        unsigned stages;
        size_t files = 0, executable = 0, sizes = 0;
        std::string bytes;

        explicit Endpoint(unsigned stages)
            : stages(stages)
        {
        }

        void createRegularFile(const CanonPath &, fun<void(CreateRegularFileSink &)> produce) override
        {
            ++files;

            struct File : CreateRegularFileSink
            {
                Endpoint & endpoint;

                explicit File(Endpoint & endpoint)
                    : endpoint(endpoint)
                {
                    skipContents = endpoint.stages & 1;
                }

                void isExecutable() override
                {
                    ++endpoint.executable;
                    skipContents = endpoint.stages & 2;
                }

                void preallocateContents(uint64_t size) override
                {
                    ++endpoint.sizes;
                    EXPECT_EQ(size, 3u);
                    skipContents = endpoint.stages & 4;
                }

                void operator()(std::string_view data) override
                {
                    endpoint.bytes += data;
                }
            } file{*this};

            produce(file);
        }
    };

    size_t checked = 0;
    for (unsigned aStages = 0; aStages < 8; ++aStages)
        for (unsigned bStages = 0; bStages < 8; ++bStages) {
            SCOPED_TRACE(::testing::Message() << "aStages=" << aStages << " bStages=" << bStages);
            Endpoint a{aStages}, b{bStages};
            TeeFileSystemObjectSink tee{a, b};
            size_t produced = 0;
            tee.createRegularFile(CanonPath::root, [&](CreateRegularFileSink & file) {
                ++produced;
                EXPECT_EQ(file.skipContents, bool(aStages & bStages & 1));
                file.isExecutable();
                EXPECT_EQ(file.skipContents, bool(aStages & bStages & 2));
                file.preallocateContents(3);
                EXPECT_EQ(file.skipContents, bool(aStages & bStages & 4));
                if (!file.skipContents)
                    file("abc");
            });
            EXPECT_EQ(produced, 1u);
            for (auto * endpoint : {&a, &b}) {
                EXPECT_EQ(endpoint->files, 1u);
                EXPECT_EQ(endpoint->executable, 1u);
                EXPECT_EQ(endpoint->sizes, 1u);
                EXPECT_EQ(endpoint->bytes, endpoint->stages & 4 ? "" : "abc");
            }
            ++checked;
        }
    EXPECT_EQ(checked, 64u);
}

/* `copyRecursive` under both directory-callback conventions: the base
   class's (`MemorySink`: the same sink, the full path) and `RestoreSink`'s
   (a sub-sink per directory under `/`).  The copy dumps to the source's
   NAR, at a nested destination and for a single-file root. */
TEST(CopyRecursive, copies_a_tree_under_both_sink_conventions)
{
    auto src = complexTree();
    auto nar = narOf(*src);

    auto dst = make_ref<MemorySourceAccessor>();
    dst->open(CanonPath::root, File{File::Directory{}});
    dst->open(CanonPath("/under"), File{File::Directory{}});
    MemorySink memory{*dst};
    copyRecursive(*src, CanonPath::root, memory, CanonPath("/under/here"));
    EXPECT_EQ(narOf(*dst, CanonPath("/under/here")), nar);

    auto tmpDir = createTempDir();
    AutoDelete delTmpDir(tmpDir, /*recursive=*/true);
    {
        RestoreSink restore(/*startFsync=*/false);
        restore.dstPath = tmpDir / "tree";
        copyRecursive(*src, CanonPath::root, restore, CanonPath::root);
    }
    EXPECT_EQ(narOf(*makeFSSourceAccessor(tmpDir / "tree")), nar);
    {
        RestoreSink restore(/*startFsync=*/false);
        restore.dstPath = tmpDir / "file";
        copyRecursive(*src, CanonPath("/bin/run"), restore, CanonPath::root);
    }
    EXPECT_EQ(narOf(*makeFSSourceAccessor(tmpDir / "file")), narOf(*src, CanonPath("/bin/run")));
}

RC_GTEST_PROP(ObjectHashSink, prop_both_routes_match_reference_and_nar_length, (const GenTree & t))
{
    auto acc = make_ref<MemorySourceAccessor>();
    acc->root = t.file;

    auto nar = narOf(*acc);
    auto expected = expectedRoot(acc);

    StringSource in{nar};
    auto viaNar = objectHashOfNar(in);
    RC_ASSERT(viaNar.root == expected);
    RC_ASSERT(viaNar.narSize == nar.size());

    auto viaWalk = objectHashOf(*acc, CanonPath::root);
    RC_ASSERT(viaWalk.root == expected);
    RC_ASSERT(viaWalk.narSize == nar.size());
}

/* Child continuation substitution is an operation on the consumer, not a
   replacement of the source path or a deferred push callback. Scoped consumers
   must remain live through every accepted child, including failed nested work. */
TEST(TreeTraversal, childContinuationsUseTheirSelectedScopedConsumer)
{
    struct Event
    {
        std::string path, environment;
        size_t consumer;
        bool node;
    };
    struct State
    {
        std::mutex mutex;
        size_t next = 0;
        std::vector<size_t> live;
        std::vector<Event> events;
        std::string failAt;
        unsigned failure = 0;

        size_t enter()
        {
            std::lock_guard lock(mutex);
            live.push_back(++next);
            return next;
        }
        size_t liveCount()
        {
            std::lock_guard lock(mutex);
            return live.size();
        }
    };
    struct Consumer : NodeVisitor<HashedNode>
    {
        HashingVisitor & hasher;
        State & state;
        const std::string environment;
        const size_t id;

        Consumer(State & state, HashingVisitor & hasher)
            : hasher(hasher), state(state), environment("/original"), id(state.enter()) {}
        Consumer(Consumer & parent, std::string environment)
            : hasher(parent.hasher), state(parent.state), environment(std::move(environment)), id(state.enter()) {}
        ~Consumer() override
        {
            std::lock_guard lock(state.mutex);
            std::erase(state.live, id);
        }
        static std::string logical(const CanonPath & path)
        {
            return path.isRoot() ? path.abs()
                                 : (*path.parent() / unhackName(*path.baseName())).abs();
        }
        void record(const CanonPath & path, bool node = false)
        {
            std::lock_guard lock(state.mutex);
            EXPECT_TRUE(std::find(state.live.begin(), state.live.end(), id) != state.live.end());
            state.events.push_back({logical(path), environment, id, node});
        }
        void fail(const CanonPath & path, unsigned phase)
        {
            if (state.failure == phase && logical(path) == state.failAt)
                throw std::runtime_error("scoped consumer failure");
        }
        HashedNode node(
            SourceAccessor &, const CanonPath &, const CanonPath & path,
            const SourceAccessor::Stat &, function_ref<HashedNode()> descend) override
        {
            record(path, true);
            return descend();
        }
        HashedNode regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override
        {
            record(path);
            fail(path, 1);
            auto result = hasher.HashingVisitor::regular(path, std::move(read));
            fail(path, 2);
            return result;
        }
        HashedNode symlink(const CanonPath & path, const std::string & target) override
        {
            record(path);
            fail(path, 1);
            auto result = hasher.HashingVisitor::symlink(path, target);
            fail(path, 2);
            return result;
        }
        HashedNode directory(const CanonPath & path, Children children) override
        {
            record(path);
            std::string body;
            if (path.isRoot()) {
                Consumer scoped{*this, "/substituted"};
                auto select = [&](Children::Each each) {
                    children.each([&](std::string_view name, bool directory, Visit visit) {
                        if (name == "selected")
                            each(name, directory, [visit = std::move(visit), &scoped](NodeVisitor<HashedNode> &) {
                                return visit(scoped);
                            });
                        else
                            each(name, directory, std::move(visit));
                    });
                };
                return hasher.hashDirectory(path, {select, children.canDefer}, body, *this);
            }
            Consumer scoped{*this, environment + "/" + std::string(unhackName(*path.baseName()))};
            return hasher.hashDirectory(path, children, body, scoped);
        }
    };
    struct Source final : MemorySourceAccessor
    {
        bool allowsConcurrentTraversal() override { return true; }
    };

    size_t cases = 0;
    for (size_t workers = 1; workers <= 8; ++workers)
        for (bool push : {false, true})
            for (bool hacked : {false, true})
                for (auto rootMode : {merkle::Mode::Regular, merkle::Mode::Executable,
                                      merkle::Mode::Symlink, merkle::Mode::Directory})
                    for (unsigned failure = 0; failure < 3; ++failure) {
                        SCOPED_TRACE(fmt("workers=%d push=%d hack=%d mode=%o failure=%d",
                            workers, push, hacked, static_cast<RawMode>(rootMode), failure));
                        WithCaseHack setting{hacked};
                        auto source = make_ref<Source>();
                        if (rootMode == merkle::Mode::Directory) {
                            source->addFile(CanonPath("selected/deep/first"), "nested");
                            source->addFile(CanonPath("selected/Foo"), "upper");
                            source->addFile(CanonPath("selected/foo"), "lower");
                            source->addFile(CanonPath("sibling"), "original consumer");
                            source->addFile(CanonPath("tail/data"), "another scope");
                            source->open(CanonPath("empty"), File{File::Directory{}});
                            source->open(CanonPath("link"), File{File::Symlink{"sibling"}});
                        } else if (rootMode == merkle::Mode::Symlink)
                            source->root = File{File::Symlink{"target"}};
                        else
                            source->root = File{File::Regular{rootMode == merkle::Mode::Executable, "root bytes"}};
                        auto expected = referenceObjectEntry(*source, CanonPath::root);
                        auto nar = narOf(*source);
                        State state;
                        state.failure = failure;
                        state.failAt = rootMode == merkle::Mode::Directory ? "/selected/deep/first" : "/";
                        {
                            HashingVisitor hasher{workers};
                            Consumer original{state, hasher};
                            bool failed = false;
                            try {
                                auto result = [&] {
                                    if (!push)
                                        return objectHashOf(*source, CanonPath::root, defaultPathFilter, original);
                                    StringSource input{nar};
                                    ObjectHashSink sink{original};
                                    parseDump(sink, input);
                                    return sink.finish();
                                }();
                                EXPECT_EQ(result.root, expected);
                                EXPECT_EQ(result.narSize, nar.size());
                            } catch (const std::runtime_error & error) {
                                failed = true;
                                EXPECT_EQ(std::string(error.what()), "scoped consumer failure");
                            }
                            EXPECT_EQ(failed, failure != 0);
                            EXPECT_EQ(state.liveCount(), 1u) << "scoped consumers must have joined and unwound";
                            std::map<std::string, size_t> actions, nodes;
                            for (auto & event : state.events) {
                                auto path = CanonPath(event.path);
                                auto parent = path.parent().value_or(CanonPath::root);
                                bool selected = path.isWithin(CanonPath("selected"));
                                auto prefix = selected ? "/substituted" : "/original";
                                auto expectedEnvironment = std::string(prefix) + (parent.isRoot() ? "" : parent.abs());
                                EXPECT_EQ(event.environment, expectedEnvironment) << event.path;
                                if (event.path == "/sibling")
                                    EXPECT_EQ(event.consumer, original.id) << "ordinary siblings retain the original consumer";
                                if (event.path == "/selected")
                                    EXPECT_NE(event.consumer, original.id) << "the selected child uses the supplied consumer";
                                ++(event.node ? nodes : actions)[event.path];
                            }
                            if (!failure) {
                                EXPECT_EQ(actions.size(), rootMode == merkle::Mode::Directory ? 11u : 1u);
                                for (auto & [path, count] : actions)
                                    EXPECT_EQ(count, 1u) << path;
                                EXPECT_EQ(nodes.size(), push ? 0u : actions.size());
                                for (auto & [path, count] : nodes)
                                    EXPECT_EQ(count, 1u) << path;
                            }
                        }
                        EXPECT_EQ(state.liveCount(), 0u);
                        ++cases;
                    }
    EXPECT_EQ(cases, 384u);
}

/* Moving a root between complete operations must preserve its tree meaning,
   scheduling width and ordered failure selection. Zero at the configuration
   seam means the machine default; it is not an explicit zero-width pool. */
TEST(HashingVisitor, movingARootPreservesItsResultsAndFirstFailure)
{
    struct Source final : MemorySourceAccessor
    {
        bool failReads = false;
        size_t gateWidth = 0, arrived = 0;
        std::mutex mutex;
        std::condition_variable wake;
        std::set<std::thread::id> threads;
        bool allowsConcurrentTraversal() override { return true; }
        void observe()
        {
            std::unique_lock lock(mutex);
            threads.insert(std::this_thread::get_id());
            if (gateWidth && arrived < gateWidth) {
                ++arrived;
                wake.notify_all();
                if (!wake.wait_for(lock, std::chrono::seconds(5), [&] { return arrived == gateWidth; }))
                    throw Error("moved visitor did not exercise its worker width");
            }
        }
        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            observe();
            if (failReads)
                throw Error("failed observation of %s", path.abs());
            MemorySourceAccessor::readFile(path, sink, std::move(sizeCallback));
        }
        std::string readLink(const CanonPath & path) override
        {
            observe();
            if (failReads)
                throw Error("failed observation of %s", path.abs());
            return MemorySourceAccessor::readLink(path);
        }
    };
    auto source = make_ref<Source>();
    for (size_t i = 0; i < 16; ++i)
        source->addFile(CanonPath(fmt("d%02d/file", i)), fmt("directory %d", i));
    source->addFile(CanonPath("plain"), "regular");
    source->open(CanonPath("exec"), File{File::Regular{true, "executable"}});
    source->open(CanonPath("link"), File{File::Symlink{"plain"}});
    PathFilter all = AllPaths{};
    Finally restoreWorkers{
        [old = HashingVisitor::workersForTest.load()] { HashingVisitor::workersForTest.store(old); }};
    size_t cases = 0;
    for (size_t configuredWidth = 0; configuredWidth <= 8; ++configuredWidth)
        for (bool warmed : {false, true})
            for (bool assignment : {false, true})
                for (auto root : {CanonPath::root, CanonPath("plain"), CanonPath("exec"), CanonPath("link")})
                    for (bool fail : {false, true}) {
                        SCOPED_TRACE(fmt("width=%d warm=%d assignment=%d root=%s fail=%d",
                            configuredWidth, warmed, assignment, root, fail));
                        HashingVisitor::workersForTest.store(configuredWidth);
                        auto workers = HashingVisitor::workerCountFor(*source, all);
                        EXPECT_GE(workers, 1u);
                        EXPECT_LE(workers, 8u);
                        source->failReads = false;
                        source->gateWidth = 0;
                        HashingVisitor original{workers};
                        if (warmed)
                            EXPECT_EQ(objectHashOf(*source, CanonPath::root, all, original).root,
                                referenceObjectEntry(*source, CanonPath::root));
                        source->failReads = fail;
                        std::optional<ObjectHashSink::Result> expected;
                        std::string expectedError;
                        HashingVisitor reference{workers};
                        try {
                            expected = objectHashOf(*source, root, all, reference);
                        } catch (const Error & error) {
                            expectedError = error.msg();
                        }
                        auto observe = [&](HashingVisitor & moved) {
                            source->arrived = 0;
                            source->threads.clear();
                            source->gateWidth = !fail && root.isRoot() ? workers : 0;
                            std::string actualError;
                            std::optional<ObjectHashSink::Result> actual;
                            try {
                                actual = objectHashOf(*source, root, all, moved);
                            } catch (const Error & error) {
                                actualError = error.msg();
                            }
                            EXPECT_EQ(actualError, expectedError);
                            if (!fail)
                                EXPECT_EQ(source->threads.size(), root.isRoot() ? workers : 1u);
                            ASSERT_EQ(actual.has_value(), expected.has_value());
                            if (actual) {
                                EXPECT_EQ(actual->root, expected->root);
                                EXPECT_EQ(actual->narSize, expected->narSize);
                            }
                        };
                        if (assignment) {
                            HashingVisitor moved{workers};
                            if (warmed) {
                                source->failReads = false;
                                EXPECT_EQ(objectHashOf(*source, CanonPath::root, all, moved).root,
                                    referenceObjectEntry(*source, CanonPath::root));
                                source->failReads = fail;
                            }
                            moved = std::move(original);
                            observe(moved);
                        } else {
                            HashingVisitor moved{std::move(original)};
                            observe(moved);
                        }
                        ++cases;
                    }
    EXPECT_EQ(cases, 288u);
}

/* Permission observations may invoke callbacks before traversal starts.
   Configuration and the selected filter remain stable during every walk. */
TEST(HashingVisitor, permissionCallbackCannotLendAReplacedFilterParallelism)
{
    enum class Answer { No, Yes, Throw };
    enum class Replacement { Keep, All, Custom };
    struct Source final : MemorySourceAccessor
    {
        std::function<bool()> permission;
        bool allowsConcurrentTraversal() override { return permission(); }
    };
    Finally restoreWorkers{
        [old = HashingVisitor::workersForTest.load()] { HashingVisitor::workersForTest.store(old); }};
    const auto defaultWorkers = CopyPool::defaultWorkers();
    size_t cases = 0, customWalks = 0;
    for (size_t configured = 0; configured <= 8; ++configured)
        for (bool initiallyAll : {false, true})
            for (auto answer : {Answer::No, Answer::Yes, Answer::Throw})
                for (auto replacement : {Replacement::Keep, Replacement::All, Replacement::Custom}) {
                    SCOPED_TRACE(fmt("width=%d all=%d answer=%d replacement=%d",
                        configured, initiallyAll, int(answer), int(replacement)));
                    auto source = make_ref<Source>();
                    source->addFile(CanonPath("a/keep"), "kept");
                    source->addFile(CanonPath("b/drop"), "rejected");
                    const auto caller = std::this_thread::get_id();
                    std::mutex mutex;
                    std::vector<std::string> filterCalls;
                    bool wrongThread = false;
                    auto custom = [&](const std::string & path) {
                        std::lock_guard lock(mutex);
                        filterCalls.push_back(path);
                        wrongThread |= std::this_thread::get_id() != caller;
                        return path != "/b/drop";
                    };
                    PathFilter filter = AllPaths{};
                    if (!initiallyAll) filter = custom;
                    size_t permissionCalls = 0;
                    source->permission = [&] {
                        ++permissionCalls;
                        EXPECT_EQ(std::this_thread::get_id(), caller);
                        if (replacement == Replacement::All) filter = AllPaths{};
                        if (replacement == Replacement::Custom) filter = custom;
                        if (answer == Answer::Throw) throw Error("permission callback");
                        return answer == Answer::Yes;
                    };
                    HashingVisitor::workersForTest.store(configured);
                    const bool finallyAll = initiallyAll && replacement != Replacement::Custom;
                    if (initiallyAll && answer == Answer::Throw) {
                        EXPECT_THROW(HashingVisitor::workerCountFor(*source, filter), Error);
                    } else {
                        const auto expected = initiallyAll && answer == Answer::Yes && finallyAll
                            ? (configured ? configured : defaultWorkers) : 1;
                        const auto selected = HashingVisitor::workerCountFor(*source, filter);
                        EXPECT_EQ(selected, expected);
                        if (!finallyAll) {
                            HashingVisitor visitor{selected};
                            EXPECT_NO_THROW(objectHashOf(*source, CanonPath::root, filter, visitor));
                            EXPECT_FALSE(wrongThread);
                            EXPECT_EQ(filterCalls, (std::vector<std::string>{"/a", "/a/keep", "/b", "/b/drop"}));
                            ++customWalks;
                        }
                    }
                    EXPECT_EQ(bool(filter.get_fn().target<AllPaths>()), finallyAll);
                    EXPECT_EQ(permissionCalls, initiallyAll ? 1u : 0u);
                    ++cases;
                }
    EXPECT_EQ(cases, 162u);
    EXPECT_EQ(customWalks, 99u);
    RecordProperty("finite_cells", cases);
}

} // namespace nix
