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
#include <mutex>
#include <thread>

#include "nix/util/object-hash-sink.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/archive.hh"
#include "nix/util/memory-source-accessor.hh"
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
            std::vector<std::pair<std::string, fun<std::string()>>> pending;
            children([&](std::string_view name, bool isDirectory, fun<std::string()> visit) {
                EXPECT_EQ(isDirectory, directories);
                pending.emplace_back(name, std::move(visit));
            });
            std::string result;
            /* Every enumeration/emitter stack frame has returned. */
            for (auto & [name, visit] : pending)
                result += name + ":" + visit() + "|";
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
                            [&](size_t i, bool detached) {
                                if (count < 2) {
                                    baseCaseTrace.push_back(detached ? "run-detached" : "run-inline");
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
                            expectedTrace.push_back("run-inline");
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
                    [&](size_t i, bool) { children[i] = recurse(remaining - 1); },
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
                        EXPECT_TRUE(actual.narSizeExact);
                        EXPECT_EQ(actual.narSize, nar.size());
                        ++cases;
                    }
                }
    EXPECT_EQ(cases, 256u);
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
    ObjectHashSink sink;
    EXPECT_THROW(sink.createDirectory(CanonPath::root), Error);
}

/* An empty sink has no root to report. */
TEST(ObjectHashSink, finish_without_root_throws)
{
    ObjectHashSink sink;
    EXPECT_THROW(sink.finish(), Error);
}

/* A driver that never announces a size (neither `parseDump` nor
   `copyRecursive`): the bytes are buffered and hashed whole, to the same
   blob id and NAR size as the announced route. */
TEST(ObjectHashSink, regular_file_without_announced_size)
{
    StringSink nar;
    dumpString("abc", nar);

    ObjectHashSink sink;
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
    ObjectHashSink sink;
    EXPECT_THROW(
        sink.createRegularFile(
            CanonPath::root,
            [](CreateRegularFileSink & crf) {
                crf.preallocateContents(3);
                crf("ab");
            }),
        Error);
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
        writer->isExecutable();
        writer->preallocateContents(5);
        (*writer)("hel");
        (*writer)("lo");
        EXPECT_TRUE(hooks.files.empty());
        writer->finish();
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
                (*writer)("plain");
                writer->finish();
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

        HashedNode node(const CanonPath & p, const SourceAccessor::Stat & st, fun<HashedNode()> descend) override
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
    EXPECT_FALSE(r.narSizeExact);
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
    EXPECT_FALSE(rRoot.narSizeExact);
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

        HashedNode node(const CanonPath & p, const SourceAccessor::Stat & st, fun<HashedNode()> descend) override
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
    EXPECT_TRUE(rFiles.narSizeExact);
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

} // namespace nix
