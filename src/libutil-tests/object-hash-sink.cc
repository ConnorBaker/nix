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

#include "nix/util/object-hash-sink.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/archive.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/file-system.hh"

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

        std::optional<HashedNode> known(const CanonPath & p, const SourceAccessor::Stat & st) override
        {
            if (st.type != SourceAccessor::tDirectory)
                return std::nullopt;
            asked.push_back(p);
            if (auto id = answer(p))
                return HashedNode{.entry = {.mode = merkle::Mode::Directory, .hash = *id}};
            return std::nullopt;
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

        std::optional<HashedNode> known(const CanonPath & p, const SourceAccessor::Stat & st) override
        {
            if (st.type != SourceAccessor::tRegular)
                return std::nullopt;
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
