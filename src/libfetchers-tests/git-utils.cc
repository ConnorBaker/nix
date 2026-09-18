#include "nix/util/environment-variables.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/store/globals.hh"
#include "nix/util/hash.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/util/merkle-files.hh"
#include "nix/util/nar-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/git.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/terminal.hh"
#include "nix/util/object-hash.hh"
#include "nix/util/source-path.hh"
#include "nix/util/archive.hh"
#include "nix/store/store-api.hh"
#include "nix/store/store-open.hh"
#include "nix/store/dummy-store.hh"
#include "nix/util/file-system.hh"
#include "nix/util/tests/gmock-matchers.hh"
#include "nix/util/tests/setting-scopes.hh"

#include <gmock/gmock.h>
#include <git2/common.h>
#include <git2/global.h>
#include <git2/repository.h>
#include <git2/signature.h>
#include <git2/types.h>
#include <git2/object.h>
#include <git2/tag.h>
#include <gtest/gtest.h>
#include "nix/util/serialise.hh"

#include <git2/blob.h>
#include <git2/commit.h>
#include <git2/config.h>
#include <git2/index.h>
#include <git2/refs.h>
#include <git2/tree.h>

#include <rapidcheck/gtest.h>
#include <exception> // IWYU pragma: keep (rapidcheck on Darwin/FreeBSD)
#include <functional>
#include <iostream>
#include <typeinfo>

namespace nix::fetchers {

class GitUtilsTest : public ::testing::Test
{
    /* The fetcher cache under a directory of this fixture's own: several
       tests write memo rows. */
    FreshCacheHome cacheHome;

    // We use a single repository for all tests.
    AutoDelete delTmpDir;

protected:
    std::filesystem::path tmpDir;

public:
    void SetUp() override
    {
        tmpDir = createTempDir() / "test-git-repo";
        GitRepo::openRepo(tmpDir, {.create = true});
        delTmpDir = AutoDelete(tmpDir, true);
    }

    void TearDown() override
    {
        delTmpDir.deletePath();
    }

    ref<GitRepo> openRepo()
    {
        return GitRepo::openRepo(tmpDir, {.create = false});
    }

    ref<GitRepoPool> openWriterPool()
    {
        return GitRepoPool::create(tmpDir, {.create = true});
    }

    std::string getRepoName() const
    {
        return tmpDir.filename().string();
    }

    /* Write `contents` at `rel` under the working directory. */
    void write(const std::string & rel, const std::string & contents)
    {
        auto p = tmpDir / rel;
        createDirs(p.parent_path());
        writeFile(p, contents);
    }

    /* Stage `paths` and commit them on HEAD, on top of HEAD's commit when
       there is one; the commit's id. */
    Hash commitPaths(git_repository * rawRepo, const std::vector<const char *> & paths, const char * message)
    {
        git_index * index = nullptr;
        EXPECT_EQ(git_repository_index(&index, rawRepo), 0);
        for (auto rel : paths)
            EXPECT_EQ(git_index_add_bypath(index, rel), 0);
        git_oid treeOid;
        EXPECT_EQ(git_index_write_tree(&treeOid, index), 0);
        EXPECT_EQ(git_index_write(index), 0);
        git_tree * tree = nullptr;
        EXPECT_EQ(git_tree_lookup(&tree, rawRepo, &treeOid), 0);
        git_signature * sig = nullptr;
        EXPECT_EQ(git_signature_now(&sig, "nix", "nix@example.com"), 0);
        git_oid commitOid;
        git_commit * parent = nullptr;
        git_oid parentOid;
        if (git_reference_name_to_id(&parentOid, rawRepo, "HEAD") == 0)
            EXPECT_EQ(git_commit_lookup(&parent, rawRepo, &parentOid), 0);
        EXPECT_EQ(
            git_commit_create_v(&commitOid, rawRepo, "HEAD", sig, sig, nullptr, message, tree, parent ? 1 : 0, parent),
            0);
        if (parent)
            git_commit_free(parent);
        git_signature_free(sig);
        git_tree_free(tree);
        git_index_free(index);
        return Hash::parseAny(git_oid_tostr_s(&commitOid), HashAlgorithm::SHA1);
    }
};

merkle::TreeEntry writeString(merkle::FileSinkBuilder & store, std::string contents, bool executable = false)
{
    auto sink = store.makeRegularFileSink();
    (*sink)(contents);
    return merkle::TreeEntry{
        executable ? merkle::Mode::Executable : merkle::Mode::Regular,
        std::move(*sink).finalize(),
    };
}

TEST_F(GitUtilsTest, sink_basic)
{
    auto repo = openRepo();
    auto pool = openWriterPool();

    // Build tree bottom-up using insertChild
    // hello file
    auto hello = writeString(*pool, "hello world");

    // bye file
    auto bye = writeString(*pool, "thanks for all the fish");

    // bye-link symlink
    auto byeLink = pool->makeSymlink("bye");

    // empty directory
    auto empty = [&] {
        auto emptyDir = pool->makeDirectorySink();
        return merkle::TreeEntry{merkle::Mode::Directory, std::move(*emptyDir).finalize()};
    }();

    // links/foo file
    auto linksFoo = writeString(*pool, "hello world");

    // links directory
    auto links = [&] {
        auto linksDir = pool->makeDirectorySink();
        linksDir->insertChild("foo", linksFoo);
        return merkle::TreeEntry{merkle::Mode::Directory, std::move(*linksDir).finalize()};
    }();

    // foo-1.1 directory (contains hello, bye, bye-link, empty, links)
    auto foo = [&] {
        auto fooDir = pool->makeDirectorySink();
        fooDir->insertChild("hello", hello);
        fooDir->insertChild("bye", bye);
        fooDir->insertChild("bye-link", byeLink);
        fooDir->insertChild("empty", empty);
        fooDir->insertChild("links", links);
        return merkle::TreeEntry{merkle::Mode::Directory, std::move(*fooDir).finalize()};
    }();

    // root directory (contains foo-1.1)
    auto rootHash = [&] {
        auto rootDir = pool->makeDirectorySink();
        rootDir->insertChild("foo-1.1", foo);
        return std::move(*rootDir).finalize();
    }();

    pool->flush();

    auto result = repo->dereferenceSingletonDirectory(rootHash);
    auto accessor = repo->getAccessor(result, {}, getRepoName());

    ASSERT_THAT(
        accessor,
        testing::HasDirectory(
            CanonPath::root,
            std::set<std::string>{
                "hello",
                "bye",
                "bye-link",
                "empty",
                "links",
            }));

    ASSERT_THAT(accessor, testing::HasContents(CanonPath("hello"), "hello world"));
    ASSERT_THAT(accessor, testing::HasContents(CanonPath("bye"), "thanks for all the fish"));
    ASSERT_THAT(accessor, testing::HasSymlink(CanonPath("bye-link"), "bye"));
    ASSERT_THAT(accessor, testing::HasDirectory(CanonPath("empty"), std::set<std::string>{}));
    ASSERT_THAT(accessor, testing::HasContents(CanonPath("links/foo"), "hello world"));
}

TEST_F(GitUtilsTest, namesFollowTheTreeObjects)
{
    auto repo = openRepo();
    auto pool = openWriterPool();

    auto file = [&](std::string contents, bool executable = false) { return writeString(*pool, contents, executable); };
    auto dir = [&](std::vector<std::pair<std::string, merkle::TreeEntry>> entries) {
        auto d = pool->makeDirectorySink();
        for (auto & [name, entry] : entries)
            d->insertChild(name, entry);
        return merkle::TreeEntry{merkle::Mode::Directory, std::move(*d).finalize()};
    };

    /* Two trees sharing a subtree and differing elsewhere. */
    auto shared = dir({{"a", file("aaa")}, {"b", file("bbb")}});
    auto root1 = dir({{"shared", shared}, {"top", file("one")}});
    auto root2 = dir({
        {"shared", shared},
        {"top", file("two")},
        {"exe", file("same", true)},
        {"plain", file("same")},
        {"link", pool->makeSymlink("plain")},
    });
    pool->flush();

    auto acc1 = repo->getAccessor(root1.hash, {}, "r1");
    auto acc2 = repo->getAccessor(root2.hash, {}, "r2");

    /* The subtree rule: the shared subtree has the same name under both
       roots, and the name is for the subtree exactly. */
    auto shared1 = acc1->getFingerprint(CanonPath("shared"));
    auto shared2 = acc2->getFingerprint(CanonPath("shared"));
    ASSERT_TRUE(shared1.second);
    EXPECT_EQ(shared1, shared2);
    EXPECT_EQ(shared1.first, CanonPath::root);
    EXPECT_EQ(acc1->getFingerprint(CanonPath("shared/a")), acc2->getFingerprint(CanonPath("shared/a")));

    /* The law: equal names, equal NARs. */
    StringSink nar1, nar2;
    acc1->dumpPath(CanonPath("shared"), nar1);
    acc2->dumpPath(CanonPath("shared"), nar2);
    EXPECT_EQ(nar1.s, nar2.s);

    /* Different roots, different names; both named. */
    auto rootName1 = acc1->getFingerprint(CanonPath::root);
    auto rootName2 = acc2->getFingerprint(CanonPath::root);
    ASSERT_TRUE(rootName1.second);
    ASSERT_TRUE(rootName2.second);
    EXPECT_NE(rootName1.second, rootName2.second);

    /* The mode is part of the NAR, so it is part of the name: same
       contents, different executable bit, different names and NARs. */
    auto exe = acc2->getFingerprint(CanonPath("exe"));
    auto plain = acc2->getFingerprint(CanonPath("plain"));
    ASSERT_TRUE(exe.second);
    ASSERT_TRUE(plain.second);
    EXPECT_NE(exe.second, plain.second);
    StringSink narExe, narPlain;
    acc2->dumpPath(CanonPath("exe"), narExe);
    acc2->dumpPath(CanonPath("plain"), narPlain);
    EXPECT_NE(narExe.s, narPlain.s);
    auto link = acc2->getFingerprint(CanonPath("link"));
    ASSERT_TRUE(link.second);
    EXPECT_NE(link.second, plain.second);

    /* A path that is not there has no name. */
    EXPECT_FALSE(acc1->getFingerprint(CanonPath("nope")).second);

    /* A name given to the whole does not displace the finer subtree names. */
    acc1->fingerprint = "input-level";
    EXPECT_EQ(acc1->getFingerprint(CanonPath("shared")), shared1);
}

/* Soundness over real Git trees: a subtree shared between two roots is stored
   once by Git under one object id, so it carries the same name under both, and
   its NAR must be the same — the naming law equal-names-imply-equal-NARs
   (01-specification.md section 8), generalised from the hand-built tree
   above to random ones.  The shared subtree is generated once and embedded in
   both roots, so equal names actually occur (two independently random trees
   almost never share a subtree). */
RC_GTEST_PROP(GitUtils, prop_sharedSubtreesNameAndRenderAlike, ())
{
    auto tmpDir = createTempDir() / "prop-git-repo";
    AutoDelete del(tmpDir, true);
    GitRepo::openRepo(tmpDir, {.create = true});
    auto repo = GitRepo::openRepo(tmpDir, {.create = false});
    auto pool = GitRepoPool::create(tmpDir, {.create = true});

    std::function<merkle::TreeEntry(int)> genTree = [&](int depth) -> merkle::TreeEntry {
        if (depth <= 0 || *rc::gen::inRange(0, 3) != 0) {
            switch (*rc::gen::inRange(0, 3)) {
            case 0:
                return writeString(*pool, *rc::gen::arbitrary<std::string>(), false);
            case 1:
                return writeString(*pool, *rc::gen::arbitrary<std::string>(), true);
            default:
                return pool->makeSymlink("target-" + std::to_string(*rc::gen::inRange(0, 1000)));
            }
        }
        auto d = pool->makeDirectorySink();
        auto n = *rc::gen::inRange(1, 4);
        for (int i = 0; i < n; ++i)
            d->insertChild("e" + std::to_string(i), genTree(depth - 1));
        return merkle::TreeEntry{merkle::Mode::Directory, std::move(*d).finalize()};
    };

    auto shared = genTree(3);
    auto makeRoot = [&](merkle::TreeEntry filler) {
        auto d = pool->makeDirectorySink();
        d->insertChild("shared", shared);
        d->insertChild("filler", filler);
        return merkle::TreeEntry{merkle::Mode::Directory, std::move(*d).finalize()};
    };
    auto root1 = makeRoot(genTree(2));
    auto root2 = makeRoot(genTree(2));
    pool->flush();

    auto acc1 = repo->getAccessor(root1.hash, {}, "r1");
    auto acc2 = repo->getAccessor(root2.hash, {}, "r2");

    std::function<void(const CanonPath &)> check = [&](const CanonPath & q) {
        auto f1 = acc1->getFingerprint(q);
        auto f2 = acc2->getFingerprint(q);
        RC_ASSERT(f1.second);
        RC_ASSERT(f1 == f2);
        StringSink n1, n2;
        acc1->dumpPath(q, n1);
        acc2->dumpPath(q, n2);
        RC_ASSERT(n1.s == n2.s);
        if (acc1->lstat(q).type == SourceAccessor::tDirectory)
            for (auto & e : acc1->readDirectory(q))
                check(q / e.first);
    };
    check(CanonPath("shared"));
}

/* Rebuild a Git tree from an accessor over a NAR (or any tree) through the
   merkle sinks: a regular file becomes a blob under its mode edge, a symlink a
   symlink blob, a directory a tree.  An if-chain, not a switch:
   SourceAccessor::Type has char/block/socket/fifo members a NAR never carries,
   and -Werror=switch-enum would demand each be listed, while only the three NAR
   node types occur here. */
static merkle::TreeEntry ingestAccessorToTree(SourceAccessor & acc, const CanonPath & q, GitRepoPool & pool)
{
    auto st = acc.lstat(q);
    if (st.type == SourceAccessor::tRegular) {
        auto sink = pool.makeRegularFileSink();
        (*sink)(acc.readFile(q));
        return merkle::TreeEntry{
            st.isExecutable ? merkle::Mode::Executable : merkle::Mode::Regular, std::move(*sink).finalize()};
    }
    if (st.type == SourceAccessor::tSymlink)
        return pool.makeSymlink(acc.readLink(q));
    if (st.type == SourceAccessor::tDirectory) {
        auto dd = pool.makeDirectorySink();
        for (auto & e : acc.readDirectory(q))
            dd->insertChild(e.first, ingestAccessorToTree(acc, q / e.first, pool));
        return merkle::TreeEntry{merkle::Mode::Directory, std::move(*dd).finalize()};
    }
    throw Error("NAR contained a node type that is not regular, symlink, or directory");
}

/* The NAR<->Merkle-tree bijection over plain store content (01-specification.md §2.4).  narTreeRoundTrip renders a tree
   to a NAR, parses that NAR back with makeNarAccessor, rebuilds a fresh Git tree from it, and returns the rebuilt oid
   and both NARs; a faithful bijection lands on the same Git object and the same bytes. */
struct RoundTrip
{
    Hash oid1;
    std::string nar0, nar1;
};

static RoundTrip narTreeRoundTrip(GitRepo & repo, GitRepoPool & pool, const Hash & oid0)
{
    auto acc0 = repo.getAccessor(oid0, {}, "rt0");
    StringSink nar0;
    acc0->dumpPath(CanonPath::root, nar0);

    auto narAcc = makeNarAccessor(std::string(nar0.s));
    auto oid1 = ingestAccessorToTree(*narAcc, CanonPath::root, pool).hash;
    pool.flush();

    auto acc1 = repo.getAccessor(oid1, {}, "rt1");
    StringSink nar1;
    acc1->dumpPath(CanonPath::root, nar1);
    return RoundTrip{oid1, std::move(nar0.s), std::move(nar1.s)};
}

/* Over random trees: the four NAR modes, empty and nested directories, wide and deep. */
RC_GTEST_PROP(GitUtils, prop_narTreeRoundTripsByteExact, ())
{
    auto tmpDir = createTempDir() / "prop-git-roundtrip";
    AutoDelete del(tmpDir, true);
    GitRepo::openRepo(tmpDir, {.create = true});
    auto repo = GitRepo::openRepo(tmpDir, {.create = false});
    auto pool = GitRepoPool::create(tmpDir, {.create = true});

    std::function<merkle::TreeEntry(int)> genTree = [&](int depth) -> merkle::TreeEntry {
        if (depth <= 0 || *rc::gen::inRange(0, 3) != 0) {
            switch (*rc::gen::inRange(0, 3)) {
            case 0:
                return writeString(*pool, *rc::gen::arbitrary<std::string>(), false);
            case 1:
                return writeString(*pool, *rc::gen::arbitrary<std::string>(), true);
            default:
                return pool->makeSymlink("target-" + std::to_string(*rc::gen::inRange(0, 1000)));
            }
        }
        auto d = pool->makeDirectorySink();
        auto n = *rc::gen::inRange(0, 6); /* 0 exercises empty directories */
        for (int i = 0; i < n; ++i)
            d->insertChild("e" + std::to_string(i), genTree(depth - 1));
        return merkle::TreeEntry{merkle::Mode::Directory, std::move(*d).finalize()};
    };

    auto d = pool->makeDirectorySink();
    auto n = *rc::gen::inRange(0, 6);
    for (int i = 0; i < n; ++i)
        d->insertChild("e" + std::to_string(i), genTree(3));
    auto oid0 = std::move(*d).finalize();
    pool->flush();

    auto rt = narTreeRoundTrip(*repo, *pool, oid0);
    RC_ASSERT(rt.oid1 == oid0);    /* NAR -> tree lands on the same object */
    RC_ASSERT(rt.nar1 == rt.nar0); /* tree -> NAR is byte-exact */
}

/* The subtle shapes a random generator reaches only rarely: an empty root, a
   large file that spans the file sink's chunking, deeply nested empty
   directories, the same bytes under different modes, a symlink with awkward
   target bytes, a wide directory, and a mixed tree. */
TEST_F(GitUtilsTest, narTreeRoundTripEdgeCases)
{
    auto repo = openRepo();
    auto pool = openWriterPool();

    auto file = [&](std::string c, bool x = false) { return writeString(*pool, std::move(c), x); };
    auto dirOf = [&](std::vector<std::pair<std::string, merkle::TreeEntry>> es) {
        auto dd = pool->makeDirectorySink();
        for (auto & [nm, e] : es)
            dd->insertChild(nm, e);
        return merkle::TreeEntry{merkle::Mode::Directory, std::move(*dd).finalize()};
    };

    std::string big;
    big.reserve(200000);
    for (unsigned i = 0; i < 200000; ++i)
        big.push_back((char) ((i * 2654435761u) >> 13));

    std::vector<std::pair<std::string, merkle::TreeEntry>> many;
    for (int i = 0; i < 300; ++i)
        many.emplace_back("f" + std::to_string(i), file("content-" + std::to_string(i), i % 2 == 0));

    std::vector<std::pair<std::string, merkle::TreeEntry>> roots = {
        {"empty-root", dirOf({})},
        {"one-empty-dir", dirOf({{"e", dirOf({})}})},
        {"deep-empty", dirOf({{"a", dirOf({{"b", dirOf({{"c", dirOf({})}})}})}})},
        {"big-file", dirOf({{"blob", file(big)}})},
        {"same-bytes-diff-mode", dirOf({{"plain", file("same")}, {"exe", file("same", true)}})},
        {"symlink-awkward-target", dirOf({{"l", pool->makeSymlink(std::string("a/b\tc \xe2\x9c\x93 d"))}})},
        {"many-entries", dirOf(many)},
        {"mixed",
         dirOf(
             {{"r", file("r")},
              {"x", file("x", true)},
              {"l", pool->makeSymlink("r")},
              {"d", dirOf({{"n", file("n")}})},
              {"empty", dirOf({})}})},
    };
    pool->flush();

    for (auto & [name, entry] : roots) {
        auto rt = narTreeRoundTrip(*repo, *pool, entry.hash);
        EXPECT_EQ(rt.oid1, entry.hash) << "oid changed for " << name;
        EXPECT_EQ(rt.nar1, rt.nar0) << "NAR changed for " << name;
    }
}

/* 01-specification.md §9.8: the shim's assertion checked on a
   *real* store object.  A tree and a single file are added to a LocalStore
   through its ordinary ingestion; the store's own NAR (narFromPath) and recorded
   narHash (queryPathInfo) are then checked to round-trip through a Git tree
   exactly.  For a directory the two shim assertions run: the Git-tree-derived
   narHash equals the store's (the shim `narHashOf`, the address map) and dumping
   the tree reproduces the store's NAR byte-for-byte.  A single-file store object (a
   .drv/toFile is one regular file, src/libexpr/write-buffer.cc:21,24) is the
   degenerate root the plan calls out: it interns as a blob, has no tree to hash,
   and must not be forced into a directory. */
TEST_F(GitUtilsTest, storeObjectsRoundTripThroughAGitTree)
{
    nix::initLibStore(/*loadConfig=*/false);
    auto storeRoot = createTempDir() / "shim-store";
    AutoDelete delStore(storeRoot, true);
    auto store = openStore("local?root=" + storeRoot.string());

    auto repo = openRepo();
    auto pool = openWriterPool();
    fetchers::Settings fetchSettings;

    auto check = [&](const StorePath & path) {
        auto info = store->queryPathInfo(path);
        StringSink narSink;
        store->narFromPath(path, narSink);
        auto & nar0 = narSink.s;

        auto narAcc = makeNarAccessor(std::string(nar0));
        /* The store's own NAR is the object its recorded object hash names
           (content identity, computed again by the walk over the NAR). */
        ASSERT_EQ(info->objectHash, ObjectHash::of(objectHashOf(*narAcc, CanonPath::root).root))
            << store->printStorePath(path);

        if (narAcc->lstat(CanonPath::root).type == SourceAccessor::tDirectory) {
            auto tree = ingestAccessorToTree(*narAcc, CanonPath::root, *pool).hash;
            pool->flush();
            /* Address map: the fetchers' shim over the Git tree gives the
               store's shim's NAR hash (nothing records a NAR hash any more). */
            EXPECT_EQ(
                narHashOf(fetchSettings, *store, SourcePath(repo->getAccessor(tree, {}, "shim")), tree),
                narHashOf(*store, path))
                << store->printStorePath(path);
            /* Present: dumping the tree reproduces the store's NAR byte-for-byte. */
            StringSink narSink1;
            repo->getAccessor(tree, {}, "shim")->dumpPath(CanonPath::root, narSink1);
            EXPECT_EQ(narSink1.s, nar0) << store->printStorePath(path);
        } else {
            /* Degenerate single-file/symlink root: a blob, no tree. */
            auto blob = ingestAccessorToTree(*narAcc, CanonPath::root, *pool);
            pool->flush();
            EXPECT_TRUE(
                blob.mode == merkle::Mode::Regular || blob.mode == merkle::Mode::Executable
                || blob.mode == merkle::Mode::Symlink)
                << store->printStorePath(path);
            StringSink narSink1;
            narAcc->dumpPath(CanonPath::root, narSink1);
            EXPECT_EQ(narSink1.s, nar0) << store->printStorePath(path);
        }
    };

    /* A directory store object: files, a subdirectory, and a symlink. */
    auto mem = make_ref<MemorySourceAccessor>();
    mem->addFile(CanonPath("a/x"), "hello");
    mem->addFile(CanonPath("a/deep/y"), "world");
    mem->addFile(CanonPath("top"), "t");
    /* createSymlink lives on MemorySink; the addFile calls above make "a" a directory. */
    MemorySink(*mem).createSymlink(CanonPath("a/link"), "x");
    check(store->addToStore("shimtree", SourcePath(mem)));

    /* A single-file store object: the NAR of one regular file, as a .drv/toFile. */
    StringSink flat;
    dumpString("derivation-like contents", flat);
    StringSource flatSource(flat.s);
    check(store->addToStoreFromDump(
        flatSource,
        "shimfile",
        FileSerialisationMethod::NixArchive,
        ContentAddressMethod::Raw::NixArchive,
        HashAlgorithm::SHA256));
}

/* The scale round-trip of 01-specification.md §9.8, a read-only scale
   verifier rather than a Store subclass: the
   shim's two assertions run over real store objects at scale --- the ambient
   store's own build outputs and fetched sources.  For each sampled valid path
   the store's canonical NAR (narFromPath) is ingested into a Git tree and
   checked to round-trip byte-exactly with a matching narHash (the shim `narHashOf`);
   single-file objects take the degenerate blob branch.  An earlier form of this
   test counted store objects with an entry named ".git": libgit2's tree builder
   refuses that name, with its case, HFS and NTFS variants, on every insert, while
   Git's object format, its raw write and its parser admit any name without "/"
   or NUL, and a NAR admits ".git" too (libutil's git test).  The sink now writes
   trees raw, so the bijection is asserted on every sampled object.  Skips where
   no populated store is available (a build sandbox); meaningful only against a
   real /nix/store.  Byte-equality is platform-independent. */
TEST_F(GitUtilsTest, realStorePathsRoundTripThroughAGitTree)
{
    /* The one unit test that opens a store other than a fixture's; gated so
       that the default run is hermetic and its result and runtime do not
       depend on the machine. */
    if (getEnv("NIX_TEST_REAL_STORE") != std::optional<std::string>{"1"})
        GTEST_SKIP() << "set NIX_TEST_REAL_STORE=1 to check the bijection over this machine's own store";

    nix::initLibStore(/*loadConfig=*/false);

    std::shared_ptr<Store> storePtr;
    try {
        storePtr = openStore("auto").get_ptr();
    } catch (const std::exception & e) {
        GTEST_SKIP() << "no ambient store: " << e.what();
    }
    auto & store = *storePtr;

    StorePathSet all;
    try {
        all = store.queryAllValidPaths();
    } catch (const std::exception & e) {
        GTEST_SKIP() << "cannot enumerate the store: " << e.what();
    }
    if (all.size() < 50)
        GTEST_SKIP() << "store too small for a scale check (" << all.size() << " paths)";

    auto repo = openRepo();
    auto pool = openWriterPool();
    fetchers::Settings fetchSettings;

    const size_t maxToCheck = 300;
    const uint64_t maxNarSize = 4 * 1024 * 1024;
    size_t checked = 0, dirs = 0, files = 0, skipped = 0, withDotGit = 0;

    for (auto & path : all) {
        if (checked >= maxToCheck)
            break;
        ref<const ValidPathInfo> info = store.queryPathInfo(path);
        if (info->narSize > maxNarSize) {
            skipped++;
            continue;
        }
        StringSink narSink;
        try {
            store.narFromPath(path, narSink);
        } catch (const std::exception &) {
            skipped++;
            continue;
        }
        auto & nar0 = narSink.s;
        auto narAcc = makeNarAccessor(std::string(nar0));
        /* The ambient store may be served by an older daemon, whose reply
           carries only the NAR hash (`assertedNarHash`, no `objectHash`);
           a new store carries the object hash.  Either way the store's own
           NAR must be the object the store describes. */
        if (info->objectHash) {
            ASSERT_EQ(*info->objectHash, ObjectHash::of(objectHashOf(*narAcc, CanonPath::root).root))
                << store.printStorePath(path);
        } else {
            ASSERT_TRUE(info->assertedNarHash.has_value()) << store.printStorePath(path);
            ASSERT_EQ(*info->assertedNarHash, hashString(HashAlgorithm::SHA256, nar0)) << store.printStorePath(path);
        }

        if (narAcc->lstat(CanonPath::root).type == SourceAccessor::tDirectory) {
            auto tree = ingestAccessorToTree(*narAcc, CanonPath::root, *pool).hash;
            pool->flush();
            /* The address map's NAR hash against the store's NAR (tied to the
               store's own description just above). */
            EXPECT_EQ(
                narHashOf(fetchSettings, store, SourcePath(repo->getAccessor(tree, {}, "shim")), tree),
                hashString(HashAlgorithm::SHA256, nar0))
                << store.printStorePath(path);
            StringSink narSink1;
            repo->getAccessor(tree, {}, "shim")->dumpPath(CanonPath::root, narSink1);
            EXPECT_EQ(narSink1.s, nar0) << store.printStorePath(path);
            dirs++;
            if (narAcc->pathExists(CanonPath(".git")))
                withDotGit++;
        } else {
            StringSink narSink1;
            narAcc->dumpPath(CanonPath::root, narSink1);
            EXPECT_EQ(narSink1.s, nar0) << store.printStorePath(path);
            files++;
        }
        checked++;
    }

    std::cerr << "[ bijection ] verified " << checked << " real store objects (" << dirs << " directories, " << files
              << " single-file/symlink), " << withDotGit << " of the directories with a `.git` entry; skipped "
              << skipped << " (too large or unreadable)\n";
    EXPECT_GT(checked, 0u);
}

/* The tree sink writes Git's format raw, so a tree carries every name a NAR
   can: `.git`, its case and NTFS variants, a name of dots.  libgit2's tree
   builder refused them; the object database and the parser do not.  The
   hash is our serialiser's -- in a SHA-256 repository, the store's
   algorithm -- and the tree reads back byte-exact. */
TEST_F(GitUtilsTest, sinkWritesEveryNarName)
{
    auto acc = make_ref<MemorySourceAccessor>();
    acc->addFile(CanonPath{"/.git/HEAD"}, "ref: refs/heads/main\n");
    acc->addFile(CanonPath{"/.GIT"}, "upper");
    acc->addFile(CanonPath{"/.git."}, "trailing dot");
    acc->addFile(CanonPath{"/git~1"}, "short name");
    acc->addFile(CanonPath{"/..x"}, "dots");
    acc->addFile(CanonPath{"/README"}, "r");

    auto dir = createTempDir() / "sha256-names-repo";
    AutoDelete delDir(dir, true);
    GitRepo::Options options{.create = true, .bare = true, .oidType = HashAlgorithm::SHA256};
    auto repo = GitRepo::openRepo(dir, options);
    auto pool = GitRepoPool::create(dir, options);
    auto tree = ingestAccessorToTree(*acc, CanonPath::root, *pool);
    pool->flush();

    EXPECT_EQ(tree.hash, objectHashOf(*acc, CanonPath::root).root.hash);
    StringSink nar0, nar1;
    acc->dumpPath(CanonPath::root, nar0);
    auto back = repo->getAccessor(tree.hash, {}, "shim");
    back->dumpPath(CanonPath::root, nar1);
    EXPECT_EQ(nar0.s, nar1.s);
    EXPECT_EQ(back->readFile(CanonPath(".git/HEAD")), "ref: refs/heads/main\n");
}

/* The store's identifiers are libgit2's: a tree written through the pool
   into a SHA-256 repository has the identifier `objectHashOf` computes
   under SHA-256, blob by blob and tree by tree, which is what the local
   store's object store computes for its own files
   (doc/lazy-store/01-specification.md, section 9.10). */
TEST_F(GitUtilsTest, sha256IdentifiersAgreeWithLibgit2)
{
    auto dir = createTempDir() / "sha256-repo";
    AutoDelete delDir(dir, true);
    GitRepo::Options options{.create = true, .bare = true, .oidType = HashAlgorithm::SHA256};
    auto repo = GitRepo::openRepo(dir, options);
    auto pool = GitRepoPool::create(dir, options);

    auto acc = make_ref<MemorySourceAccessor>();
    MemorySink ms{*acc};
    ms.createDirectory(CanonPath::root);
    ms.createRegularFile(CanonPath("/a"), [](auto & crf) { crf("one"); });
    ms.createDirectory(CanonPath("/d"));
    ms.createRegularFile(CanonPath("/d/b"), [](auto & crf) { crf("two"); });
    ms.createDirectory(CanonPath("/d/e"));
    ms.createRegularFile(CanonPath("/d/e/c"), [](auto & crf) { crf("one"); });
    ms.createRegularFile(CanonPath("/d/e/x"), [](auto & crf) {
        crf("one");
        crf.isExecutable();
    });
    ms.createSymlink(CanonPath("/d/link"), "b");

    auto tree = ingestAccessorToTree(*acc, CanonPath::root, *pool);
    pool->flush();

    EXPECT_EQ(tree.hash.algo, HashAlgorithm::SHA256);
    EXPECT_EQ(tree.hash, objectHashOf(*acc, CanonPath::root).root.hash);

    /* A blob's identifier, both ways. */
    auto blobWritten = writeString(*pool, "one");
    pool->flush();
    EXPECT_EQ(blobWritten.hash.algo, HashAlgorithm::SHA256);
    EXPECT_EQ(blobWritten.hash, objectHashOf(*acc, CanonPath("a")).root.hash);
    {
        HashSink sink(HashAlgorithm::SHA256);
        sink(std::string_view("blob 3"));
        sink(std::string_view("\0", 1));
        sink(std::string_view("one"));
        EXPECT_EQ(sink.finish().hash, blobWritten.hash);
    }

    /* The repository reads the tree back as the bytes written, and its
       object database is a SHA-256 one. */
    EXPECT_TRUE(repo->hasObject(tree.hash));
    StringSink nar0, nar1;
    acc->dumpPath(CanonPath::root, nar0);
    auto back = repo->getAccessor(tree.hash, {}, "shim");
    back->dumpPath(CanonPath::root, nar1);
    EXPECT_EQ(nar0.s, nar1.s);
}

/* A forwarding accessor that counts the files read through it: the
   discriminator for "no walk" (01 section 9.11, "The cost property"). */
struct CountingAccessor : SourceAccessor
{
    ref<SourceAccessor> inner;
    unsigned reads = 0;

    CountingAccessor(ref<SourceAccessor> inner)
        : inner(inner)
    {
    }

    void anchor() override {}

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        reads++;
        inner->readFile(path, sink, std::move(sizeCallback));
    }

    bool pathExists(const CanonPath & path) override
    {
        return inner->pathExists(path);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        return inner->maybeLstat(path);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        return inner->readDirectory(path);
    }

    std::string readLink(const CanonPath & path) override
    {
        return inner->readLink(path);
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        return inner->getFingerprint(path);
    }
};

/* The shim (01 section 9.11; 04 section 1.9, the fetchers' and the language's representation):
   `narHashOf(settings, store, tree, treeHash)` is the NAR hash of a tree the
   store names by its tree hash -- the hash of `dumpPath` -- computed by one
   walk (`fetchToStore2`'s dry run under `NixArchive`) and memoised under the
   tree's name, so a second call over a fresh accessor of the same named tree
   reads nothing; the pair is recorded, so `lookupTreeAddress` answers
   the tree hash for the NAR hash afterwards; and `assertNarHash` accepts the
   right assertion without a walk and refuses a wrong one with the fetchers'
   error and exit status 102. */
TEST_F(GitUtilsTest, narHashOfIsTheMemoisedShim)
{
    nix::initLibStore(/*loadConfig=*/false);
    fetchers::Settings settings;
    auto store = make_ref<DummyStoreConfig>(StoreReference::Params{})->openStore();

    auto dir = createTempDir() / "sha256-repo-shim";
    AutoDelete delDir(dir, true);
    GitRepo::Options options{.create = true, .bare = true, .oidType = HashAlgorithm::SHA256};
    auto repo = GitRepo::openRepo(dir, options);
    auto pool = GitRepoPool::create(dir, options);

    auto salt = Hash::random(HashAlgorithm::SHA256).to_string(HashFormat::Nix32, false);
    auto acc = make_ref<MemorySourceAccessor>();
    MemorySink ms{*acc};
    ms.createDirectory(CanonPath::root);
    ms.createRegularFile(CanonPath("/a"), [&](auto & crf) { crf("one " + salt); });
    ms.createDirectory(CanonPath("/d"));
    ms.createRegularFile(CanonPath("/d/b"), [&](auto & crf) { crf("two " + salt); });
    auto tree = ingestAccessorToTree(*acc, CanonPath::root, *pool);
    pool->flush();
    ASSERT_EQ(tree.mode, merkle::Mode::Directory);

    /* One walk: both files read once; the hash is the serialisation's. */
    auto first = make_ref<CountingAccessor>(repo->getAccessor(tree.hash, {}, "shim"));
    ASSERT_TRUE(first->getFingerprint(CanonPath::root).second);
    auto narHash = narHashOf(settings, *store, SourcePath(first), tree.hash);
    EXPECT_EQ(first->reads, 2u);
    StringSink nar;
    acc->dumpPath(CanonPath::root, nar);
    EXPECT_EQ(narHash, hashString(HashAlgorithm::SHA256, nar.s));

    /* Memoised under the tree's name: a second call over a fresh accessor
       of the same tree reads nothing. */
    auto second = make_ref<CountingAccessor>(repo->getAccessor(tree.hash, {}, "shim"));
    EXPECT_EQ(narHashOf(settings, *store, SourcePath(second), tree.hash), narHash);
    EXPECT_EQ(second->reads, 0u);

    /* The pair recorded: the NAR hash names the tree. */
    EXPECT_EQ(lookupTreeAddress(settings, narHash), std::optional<Hash>(tree.hash));

    /* The assertion: right, silent and without a walk; wrong, refused. */
    auto third = make_ref<CountingAccessor>(repo->getAccessor(tree.hash, {}, "shim"));
    assertNarHash(settings, *store, SourcePath(third), tree.hash, narHash, "the tree");
    EXPECT_EQ(third->reads, 0u);
    auto wrong = hashString(HashAlgorithm::SHA256, "not the NAR");
    try {
        assertNarHash(settings, *store, SourcePath(third), tree.hash, wrong, "the tree");
        FAIL() << "a wrong NAR hash was accepted";
    } catch (Error & e) {
        EXPECT_EQ(e.info().status, 102u);
        EXPECT_THAT(
            filterANSIEscapes(e.what(), /*filterAll=*/true),
            ::testing::HasSubstr("NAR hash mismatch in input 'the tree'"));
    }
}

/* `readDirectory` returns each entry's type from the tree entry's mode, as
   `maybeLstat` reads it (a submodule an empty directory), so a consumer that
   wants the types -- `builtins.readDir` attaches a `readFileType` lookup to
   every untyped entry -- looks nothing up.  Before, every entry was untyped
   (`DirEntry{}`) and each type was one lookup. */
TEST_F(GitUtilsTest, readDirectoryReturnsTypedEntries)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);

    git_oid blob;
    ASSERT_EQ(git_blob_create_from_buffer(&blob, rawRepo, "x", 1), 0);

    auto tree = [&](const std::vector<std::tuple<const char *, const git_oid *, git_filemode_t>> & entries) {
        git_treebuilder * builder = nullptr;
        EXPECT_EQ(git_treebuilder_new(&builder, rawRepo, nullptr), 0);
        for (auto & [name, oid, mode] : entries)
            EXPECT_EQ(git_treebuilder_insert(nullptr, builder, name, oid, mode), 0);
        git_oid oid;
        EXPECT_EQ(git_treebuilder_write(&oid, builder), 0);
        git_treebuilder_free(builder);
        return oid;
    };
    auto sub = tree({{"inner", &blob, GIT_FILEMODE_BLOB}});
    /* A submodule's commit need not be in this repository's object database
       (libgit2 validates every other kind, `tree.c`). */
    auto root = tree({
        {"dir", &sub, GIT_FILEMODE_TREE},
        {"exe", &blob, GIT_FILEMODE_BLOB_EXECUTABLE},
        {"file", &blob, GIT_FILEMODE_BLOB},
        {"link", &blob, GIT_FILEMODE_LINK},
        {"submodule", &blob, GIT_FILEMODE_COMMIT},
    });
    git_repository_free(rawRepo);

    auto acc = openRepo()->getAccessor(Hash::parseAny(git_oid_tostr_s(&root), HashAlgorithm::SHA1), {}, "typed");
    auto entries = acc->readDirectory(CanonPath::root);
    ASSERT_EQ(entries.size(), 5u);

    std::map<std::string, SourceAccessor::Type> expected{
        {"dir", SourceAccessor::tDirectory},
        {"exe", SourceAccessor::tRegular},
        {"file", SourceAccessor::tRegular},
        {"link", SourceAccessor::tSymlink},
        {"submodule", SourceAccessor::tDirectory},
    };
    unsigned lookups = 0;
    for (auto & [name, type] : entries) {
        /* What a consumer does with an untyped entry. */
        auto resolved = type ? *type : (lookups++, acc->lstat(CanonPath(name)).type);
        EXPECT_EQ(resolved, expected.at(name)) << name;
        EXPECT_EQ(type, std::optional(acc->lstat(CanonPath(name)).type)) << name << " typed as the lookup types it";
    }
    EXPECT_EQ(lookups, 0u) << "a typed listing costs no lookup";
}

TEST_F(GitUtilsTest, peel_reference)
{
    // Create a commit in the repo
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);

    // Create a blob
    git_oid blob_oid;
    const char * blob_content = "hello world";
    ASSERT_EQ(git_blob_create_from_buffer(&blob_oid, rawRepo, blob_content, strlen(blob_content)), 0);

    // Create a tree with that blob
    git_treebuilder * builder = nullptr;
    ASSERT_EQ(git_treebuilder_new(&builder, rawRepo, nullptr), 0);
    ASSERT_EQ(git_treebuilder_insert(nullptr, builder, "file.txt", &blob_oid, GIT_FILEMODE_BLOB), 0);

    git_oid tree_oid;
    ASSERT_EQ(git_treebuilder_write(&tree_oid, builder), 0);
    git_treebuilder_free(builder);

    git_tree * tree = nullptr;
    ASSERT_EQ(git_tree_lookup(&tree, rawRepo, &tree_oid), 0);

    // Create a commit
    git_signature * sig = nullptr;
    ASSERT_EQ(git_signature_now(&sig, "nix", "nix@example.com"), 0);

    git_oid commit_oid;
    ASSERT_EQ(git_commit_create_v(&commit_oid, rawRepo, "HEAD", sig, sig, nullptr, "initial commit", tree, 0), 0);

    // Lookup our commit
    git_object * commit_object = nullptr;
    ASSERT_EQ(git_object_lookup(&commit_object, rawRepo, &commit_oid, GIT_OBJECT_COMMIT), 0);

    // Create annotated tag
    git_oid tag_oid;
    ASSERT_EQ(git_tag_create(&tag_oid, rawRepo, "v1", commit_object, sig, "annotated tag", 0), 0);

    auto repo = openRepo();

    // Use resolveRef to get peeled object
    auto resolved = repo->resolveRef("refs/tags/v1");

    // Now assert that we have unpeeled it!
    ASSERT_STREQ(resolved.gitRev().c_str(), git_oid_tostr_s(&commit_oid));

    git_signature_free(sig);
    git_repository_free(rawRepo);
}

/* A dirty working directory names its clean subtrees by the commit's
   objects, exactly as the commit's accessor does, and its root and changed
   paths by the whole tree's name; a configuration under which the working
   directory may not hold the blobs takes the object names away. */
TEST_F(GitUtilsTest, workdirNamesCleanSubtreesByTheCommit)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    write("clean/a", "a");
    write("clean/sub/b", "b");
    write("dirty/c", "c");
    write("top", "t");
    commitPaths(rawRepo, {"clean/a", "clean/sub/b", "dirty/c", "top"}, "initial");

    /* One modification: `dirty/c` and its ancestors change, nothing else. */
    write("dirty/c", "changed");

    auto repo = openRepo();
    auto wd = repo->getWorkdirInfo();
    ASSERT_TRUE(wd.isDirty);
    ASSERT_TRUE(wd.headRev);
    ASSERT_TRUE(wd.dirtyFiles.contains(CanonPath("dirty/c")));

    auto head = repo->getAccessor(*wd.headRev, {}, "");
    auto workdir = repo->getAccessor(wd, {}, [](const CanonPath & path) { return RestrictedPathError("no"); });
    workdir->fingerprint = "whole";

    auto whole = [](const char * p) { return std::pair{CanonPath(p), std::optional<std::string>("whole")}; };
    EXPECT_EQ(workdir->getFingerprint(CanonPath::root), whole("/"));
    EXPECT_EQ(workdir->getFingerprint(CanonPath("dirty")), whole("dirty"));
    EXPECT_EQ(workdir->getFingerprint(CanonPath("dirty/c")), whole("dirty/c"));
    for (auto p : {"clean", "clean/a", "clean/sub", "clean/sub/b", "top"}) {
        auto name = head->getFingerprint(CanonPath(p));
        ASSERT_TRUE(name.second) << p;
        EXPECT_EQ(workdir->getFingerprint(CanonPath(p)), name) << p;
    }
    /* The content is the working directory's, whatever the name. */
    EXPECT_EQ(workdir->readFile(CanonPath("dirty/c")), "changed");
    EXPECT_EQ(workdir->readFile(CanonPath("clean/a")), "a");

    /* An untracked `.gitattributes` below the root that could filter takes
       the object names away; removed, they return. */
    write("clean/.gitattributes", "* text=auto\n");
    auto filtered = repo->getAccessor(wd, {}, [](const CanonPath & path) { return RestrictedPathError("no"); });
    filtered->fingerprint = "whole";
    EXPECT_EQ(filtered->getFingerprint(CanonPath("clean")), whole("clean"));
    EXPECT_EQ(filtered->getFingerprint(CanonPath("top")), whole("top"));
    deletePath(tmpDir / "clean/.gitattributes");
    auto again = repo->getAccessor(wd, {}, [](const CanonPath & path) { return RestrictedPathError("no"); });
    EXPECT_EQ(again->getFingerprint(CanonPath("clean")), head->getFingerprint(CanonPath("clean")));

    /* `core.autocrlf` could make a clean file's bytes differ from its blob:
       no object names then. */
    git_config * config = nullptr;
    ASSERT_EQ(git_repository_config(&config, rawRepo), 0);
    ASSERT_EQ(git_config_set_bool(config, "core.autocrlf", 1), 0);
    git_config_free(config);
    auto repo2 = openRepo();
    auto workdir2 = repo2->getAccessor(wd, {}, [](const CanonPath & path) { return RestrictedPathError("no"); });
    workdir2->fingerprint = "whole";
    EXPECT_EQ(workdir2->getFingerprint(CanonPath("clean")), whole("clean"));
    EXPECT_EQ(workdir2->getFingerprint(CanonPath("top")), whole("top"));

    git_repository_free(rawRepo);
}

/* The memo's rows hold ENTRY hashes -- a blob id, a tree id -- never the
   object hash of a bare root (01 section 9.9).  The two readers of one key
   must agree: `fetchToStore2` names a tree by its root's row, and the
   subtree memo (`gitTreeHashMemoised`) reads the same key for that node as
   a child of its parent.  An executable file or a symlink is where they
   differed: its object hash is the synthetic tree's id, its entry hash the
   blob's.  Named alone and then as a child, the parent must still get its
   true tree hash, and a second naming of the file alone -- answered from
   its row -- must give the object hash and path the first did.  Before the
   fix the root's row held the object hash and the parent's tree hash was
   wrong: this test failed at `hr == expectedRoot.hash` and at the row. */
TEST_F(GitUtilsTest, memoRowsHoldEntryHashesForBareExecutableAndSymlinkRoots)
{
    nix::initLibStore(/*loadConfig=*/false);
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);

    auto salt = Hash::random(HashAlgorithm::SHA256).to_string(HashFormat::Nix32, false);
    std::string xContents = "#!/bin/sh\necho " + salt + "\n";
    writeFile(tmpDir / "x", xContents);
    std::filesystem::permissions(
        tmpDir / "x",
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
    writeFile(tmpDir / "a", "plain " + salt);
    std::filesystem::create_symlink("a", tmpDir / "l");

    auto rev = commitPaths(rawRepo, {"x", "a", "l"}, "bare roots");
    git_repository_free(rawRepo);

    auto repo = openRepo();
    auto acc = repo->getAccessor(rev, {}, "");
    /* The commit recorded the modes this test is about. */
    ASSERT_TRUE(acc->lstat(CanonPath("x")).isExecutable);
    ASSERT_EQ(acc->lstat(CanonPath("l")).type, SourceAccessor::tSymlink);
    ASSERT_TRUE(acc->getFingerprint(CanonPath("x")).second);

    auto store = openStore("dummy://?read-only=false");
    fetchers::Settings settings;
    auto git = ContentAddressMethod::Raw::Git;

    /* The unmemoised answers: the whole tree, and each bare root's entry. */
    auto expectedRoot = objectHashOf(*acc, CanonPath::root).root;
    ASSERT_EQ(expectedRoot.mode, merkle::Mode::Directory);

    for (auto leaf : {"x", "l"}) {
        CanonPath leafPath(leaf);
        auto entry = objectHashOf(*acc, leafPath).root;
        ASSERT_NE(entry.mode, merkle::Mode::Regular) << leaf;
        ASSERT_NE(merkle::objectHash(entry), entry.hash) << leaf;

        /* Named alone: addressed by its object hash, the synthetic tree's id. */
        auto [pLeaf, hLeaf] = fetchToStore2(settings, *store, SourcePath(acc, leafPath), FetchMode::DryRun, leaf);
        EXPECT_EQ(hLeaf, merkle::objectHash(entry)) << leaf;
        EXPECT_EQ(pLeaf, gitTreePath(*store, leaf, merkle::objectHash(entry))) << leaf;

        /* Its row holds the entry hash, the blob id. */
        auto [subpath, name] = acc->getFingerprint(leafPath);
        ASSERT_TRUE(name) << leaf;
        auto row = settings.getCache()->lookup(makeSourcePathToHashCacheKey(*name, git, subpath));
        ASSERT_TRUE(row) << leaf;
        EXPECT_EQ(Hash::parseSRI(fetchers::getStrAttr(*row, "hash")), entry.hash) << leaf;
        EXPECT_EQ(fetchers::getIntAttr(*row, "mode"), uint64_t(entry.mode)) << leaf;

        /* Named alone again, answered from that row (another `Settings`,
           since the in-memory memo is per `Settings`): the same address. */
        fetchers::Settings settings2;
        auto [pLeaf2, hLeaf2] = fetchToStore2(settings2, *store, SourcePath(acc, leafPath), FetchMode::DryRun, leaf);
        EXPECT_EQ(hLeaf2, hLeaf) << leaf;
        EXPECT_EQ(pLeaf2, pLeaf) << leaf;
    }

    /* The repository root, its bare children answered from their rows:
       the true tree hash and path. */
    auto [pr, hr] = fetchToStore2(settings, *store, SourcePath(acc), FetchMode::DryRun, "src");
    EXPECT_EQ(hr, expectedRoot.hash);
    EXPECT_EQ(pr, gitTreePath(*store, "src", expectedRoot.hash));
}

/* The export-ignore wrapper passes the inner names through when no
   attributes source names `export-ignore`, for a commit and for the working
   directory, and hides them once one does. */
TEST_F(GitUtilsTest, exportIgnoreWrapperPassesNamesThroughWhenItHidesNothing)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    auto commit = [&](std::vector<const char *> paths, const char * message) {
        return commitPaths(rawRepo, paths, message);
    };
    write("clean/a", "a");
    write("dirty/c", "c");
    write("top", "t");
    auto rev1 = commit({"clean/a", "dirty/c", "top"}, "one");

    auto repo = openRepo();
    auto raw = repo->getAccessor(rev1, {}, "");
    EXPECT_FALSE(repo->treeMentionsAttribute(rev1, "export-ignore"));
    auto hidesNothing = [&](ref<GitRepo> r, const Hash & rev) {
        return !r->treeMentionsAttribute(rev, "export-ignore")
               && !r->attributesOutsideTheTreeMention("export-ignore", rev);
    };
    auto wrapped =
        repo->getAccessor(rev1, {.exportIgnore = true, .exportIgnoreHidesNothing = hidesNothing(repo, rev1)}, "");
    /* The filter is left out, so the accessor is the inner one and the
       root's name is the tree's, as with `exportIgnore = false`. */
    wrapped->fingerprint = "whole";
    for (auto p : {"", "clean", "clean/a", "top"})
        EXPECT_EQ(wrapped->getFingerprint(CanonPath(p)), raw->getFingerprint(CanonPath(p))) << p;

    /* Not asked to name subtrees: the whole tree's name, as before. */
    auto unnamed = repo->getAccessor(rev1, {.exportIgnore = true}, "");
    unnamed->fingerprint = "whole";
    EXPECT_EQ(
        unnamed->getFingerprint(CanonPath("clean")),
        (std::pair{CanonPath("clean"), std::optional<std::string>("whole")}));

    /* The working directory, dirty in one file, under the same wrapper. */
    write("dirty/c", "changed");
    auto wd = repo->getWorkdirInfo();
    ASSERT_TRUE(wd.isDirty);
    auto workdir =
        repo->getAccessor(wd, {.exportIgnore = true}, [](const CanonPath & path) { return RestrictedPathError("no"); });
    workdir->fingerprint = "whole";
    EXPECT_EQ(workdir->getFingerprint(CanonPath("clean/a")), raw->getFingerprint(CanonPath("clean/a")));
    EXPECT_EQ(
        workdir->getFingerprint(CanonPath("dirty")),
        (std::pair{CanonPath("dirty"), std::optional<std::string>("whole")}));

    /* A committed `.gitattributes` naming export-ignore, anywhere in the tree, takes the names away. */
    write("clean/.gitattributes", "top export-ignore\n");
    write("dirty/c", "c");
    auto rev2 = commit({"clean/.gitattributes", "dirty/c"}, "two");
    auto repo2 = openRepo();
    EXPECT_TRUE(repo2->treeMentionsAttribute(rev2, "export-ignore"));
    auto wrapped2 =
        repo2->getAccessor(rev2, {.exportIgnore = true, .exportIgnoreHidesNothing = hidesNothing(repo2, rev2)}, "");
    wrapped2->fingerprint = "whole";
    EXPECT_EQ(
        wrapped2->getFingerprint(CanonPath("clean/a")),
        (std::pair{CanonPath("clean/a"), std::optional<std::string>("whole")}));
    auto wd2 = repo2->getWorkdirInfo();
    auto workdir2 = repo2->getAccessor(
        wd2, {.exportIgnore = true}, [](const CanonPath & path) { return RestrictedPathError("no"); });
    workdir2->fingerprint = "whole";
    EXPECT_EQ(
        workdir2->getFingerprint(CanonPath("clean/a")),
        (std::pair{CanonPath("clean/a"), std::optional<std::string>("whole")}));

    git_repository_free(rawRepo);
}

/* When no attributes source names `export-ignore`, the filter is the
   identity and is left out: the accessor is the inner one, for a commit and
   for the working directory, and the tree is named alike with and without
   `exportIgnore`. */
TEST_F(GitUtilsTest, exportIgnoreFilterIsLeftOutWhenNoSourceNamesIt)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    write("sub/a", "a");
    write("top", "t");
    /* Attributes, none of them export-ignore; a comment naming it is a comment. */
    write(".gitattributes", "# not export-ignore\n*.lock linguist-generated\n* text=auto eol=lf\n");
    auto rev = commitPaths(rawRepo, {"sub/a", "top", ".gitattributes"}, "one");
    git_repository_free(rawRepo);

    auto repo = openRepo();
    EXPECT_FALSE(repo->treeMentionsAttribute(rev, "export-ignore"));
    EXPECT_FALSE(repo->attributesOutsideTheTreeMention("export-ignore", rev));
    auto raw = repo->getAccessor(rev, {}, "");
    auto filtered = repo->getAccessor(rev, {.exportIgnore = true, .exportIgnoreHidesNothing = true}, "");
    EXPECT_TRUE(typeid(*filtered) == typeid(*raw)) << typeid(*filtered).name();
    EXPECT_EQ(filtered->readDirectory(CanonPath::root), raw->readDirectory(CanonPath::root));

    auto wd = repo->getWorkdirInfo();
    auto notAllowed = [](const CanonPath & path) { return RestrictedPathError("no"); };
    auto workdir = repo->getAccessor(wd, {.exportIgnore = true}, notAllowed);
    EXPECT_TRUE(typeid(*workdir) == typeid(*repo->getAccessor(wd, {}, notAllowed))) << typeid(*workdir).name();

    /* The same tree, the same name and path. */
    nix::initLibStore(/*loadConfig=*/false);
    auto store = openStore("dummy://?read-only=false");
    fetchers::Settings settings;
    auto [pRaw, hRaw] = fetchToStore2(settings, *store, SourcePath(raw), FetchMode::DryRun, "src");
    auto [pFiltered, hFiltered] = fetchToStore2(settings, *store, SourcePath(filtered), FetchMode::DryRun, "src");
    EXPECT_EQ(hFiltered, hRaw);
    EXPECT_EQ(pFiltered, pRaw);
}

/* A repository naming `export-ignore` anywhere libgit2's lookup consults
   keeps the filter, which hides what libgit2 hides.  The sources, each
   alone: `info/attributes`; a `.gitattributes` only in the working
   directory, which the commit accessor's lookup reads before the index and
   the commit (`GIT_ATTR_CHECK_FILE_THEN_INDEX`, the low bits of its flags),
   libgit2's behaviour pinned; one only in the index; the user's file on the
   XDG search path; a macro in the system's file, whose rules
   `GIT_ATTR_CHECK_NO_SYSTEM` excludes but whose macros load;
   `core.attributesFile` with `~/`; a nested `.gitattributes` in the commit;
   CRLF line endings; and `-export-ignore`, beside a rule and alone. */
TEST_F(GitUtilsTest, exportIgnoreDeclaredAnywhereKeepsTheFilter)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    write("hide", "h");
    write("keep", "k");
    write("sub/hide2", "h");
    write("sub/keep2", "k");
    auto rev0 = commitPaths(rawRepo, {"hide", "keep", "sub/hide2", "sub/keep2"}, "one");

    /* The fetcher's decision (`exportIgnoreHidesNothing`, git.cc) without its cache, and the accessor it gives. */
    auto fetch = [&](const Hash & rev) {
        auto repo = openRepo();
        bool hidesNothing = !repo->treeMentionsAttribute(rev, "export-ignore")
                            && !repo->attributesOutsideTheTreeMention("export-ignore", rev);
        return std::pair{
            hidesNothing, repo->getAccessor(rev, {.exportIgnore = true, .exportIgnoreHidesNothing = hidesNothing}, "")};
    };
    auto expectFiltered = [&](const Hash & rev, const char * hidden, const char * kept, const char * source) {
        auto [hidesNothing, acc] = fetch(rev);
        EXPECT_FALSE(hidesNothing) << source;
        EXPECT_FALSE(acc->pathExists(CanonPath(hidden))) << source << ": " << hidden;
        EXPECT_TRUE(acc->pathExists(CanonPath(kept))) << source << ": " << kept;
    };
    auto stage = [&](const char * rel, bool add) {
        git_index * index = nullptr;
        ASSERT_EQ(git_repository_index(&index, rawRepo), 0);
        ASSERT_EQ(add ? git_index_add_bypath(index, rel) : git_index_remove_bypath(index, rel), 0);
        ASSERT_EQ(git_index_write(index), 0);
        git_index_free(index);
    };
    auto gitDir = std::filesystem::path(git_repository_path(rawRepo));
    auto scratch = gitDir / "test-attribute-sources";
    createDirs(gitDir / "info");

    /* info/attributes */
    writeFile(gitDir / "info" / "attributes", "hide export-ignore\n");
    expectFiltered(rev0, "hide", "keep", "info/attributes");
    std::filesystem::remove(gitDir / "info" / "attributes");

    /* Only in the working directory, untracked, below the root. */
    write("sub/.gitattributes", "hide2 export-ignore\n");
    expectFiltered(rev0, "sub/hide2", "sub/keep2", "workdir-only .gitattributes");
    std::filesystem::remove(tmpDir / "sub" / ".gitattributes");

    /* Only in the index. */
    write("sub/.gitattributes", "hide2 export-ignore\n");
    stage("sub/.gitattributes", true);
    std::filesystem::remove(tmpDir / "sub" / ".gitattributes");
    expectFiltered(rev0, "sub/hide2", "sub/keep2", "index-only .gitattributes");
    stage("sub/.gitattributes", false);

    /* The user's file, on libgit2's XDG search path. */
    auto xdg = scratch / "xdg-git";
    createDirs(xdg);
    writeFile(xdg / "attributes", "hide export-ignore\n");
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_XDG, xdg.string().c_str()), 0);
    expectFiltered(rev0, "hide", "keep", "XDG attributes");
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_XDG, nullptr), 0);

    /* A macro in the system's file, used by a rule in info/attributes. */
    auto sys = scratch / "sys";
    createDirs(sys);
    writeFile(sys / "gitattributes", "[attr]hidden export-ignore\n");
    writeFile(gitDir / "info" / "attributes", "hide hidden\n");
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_SYSTEM, sys.string().c_str()), 0);
    expectFiltered(rev0, "hide", "keep", "system macro");
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_SYSTEM, nullptr), 0);
    std::filesystem::remove(gitDir / "info" / "attributes");

    /* core.attributesFile, `~/` expanded against libgit2's home directory. */
    auto home = scratch / "home";
    createDirs(home);
    writeFile(home / "attrs", "hide export-ignore\n");
    git_buf oldHome = GIT_BUF_INIT;
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_GET_HOMEDIR, &oldHome), 0);
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_HOMEDIR, home.string().c_str()), 0);
    auto setAttributesFile = [&](const char * value) {
        git_config * config = nullptr;
        ASSERT_EQ(git_repository_config(&config, rawRepo), 0);
        ASSERT_EQ(
            value ? git_config_set_string(config, "core.attributesfile", value)
                  : git_config_delete_entry(config, "core.attributesfile"),
            0);
        git_config_free(config);
    };
    setAttributesFile("~/attrs");
    expectFiltered(rev0, "hide", "keep", "core.attributesFile");
    setAttributesFile(nullptr);
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_HOMEDIR, oldHome.ptr), 0);
    git_buf_dispose(&oldHome);

    /* A nested .gitattributes in the commit. */
    write("sub/.gitattributes", "hide2 export-ignore\n");
    auto rev1 = commitPaths(rawRepo, {"sub/.gitattributes"}, "two");
    EXPECT_TRUE(openRepo()->treeMentionsAttribute(rev1, "export-ignore"));
    expectFiltered(rev1, "sub/hide2", "sub/keep2", "nested .gitattributes in the commit");

    /* CRLF line endings, blanks to libgit2. */
    write(".gitattributes", "hide export-ignore\r\n");
    auto rev2 = commitPaths(rawRepo, {".gitattributes"}, "three");
    expectFiltered(rev2, "hide", "keep", "CRLF .gitattributes");

    /* A negation beside a rule, and alone: the filter stays and hides what libgit2 hides, nothing. */
    write(".gitattributes", "* export-ignore\nkeep -export-ignore\n");
    auto rev3 = commitPaths(rawRepo, {".gitattributes"}, "four");
    expectFiltered(rev3, "hide", "keep", "-export-ignore beside a rule");
    write(".gitattributes", "keep -export-ignore\n");
    auto rev4 = commitPaths(rawRepo, {".gitattributes"}, "five");
    auto [hidesNothing, acc] = fetch(rev4);
    EXPECT_FALSE(hidesNothing);
    EXPECT_TRUE(acc->pathExists(CanonPath("hide")));

    git_repository_free(rawRepo);
}

/* The pack builder stores what would not shrink and deflates what would
   (packaging/patches/0004-pack-compression-heuristic.patch): a blob of
   pseudo-random bytes leaves the cache within one percent of its size and
   with zlib's level-0 header, a blob of repeating text under half its size
   with zlib's default-level header.  The header bytes are what distinguish
   this from libgit2 as shipped, which deflates everything at the default and
   lands the random blob at the same size through zlib's own stored-block
   fallback (0x78 0x9c there, 0x78 0x01 here). */
TEST_F(GitUtilsTest, packsStoreWhatWouldNotShrink)
{
    const size_t size = 4 << 20;

    /* One bare, packfiles-only, SHA-256 repository per blob, as the tarball cache is. */
    auto packOf = [&](std::string name, std::string contents) {
        auto dir = tmpDir / name;
        auto pool = GitRepoPool::create(
            dir,
            {.create = true,
             .bare = true,
             .packfilesOnly = true,
             .dontFindDeltas = true,
             .oidType = HashAlgorithm::SHA256});
        writeString(*pool, std::move(contents));
        pool->flush();

        std::string pack;
        for (auto & entry : std::filesystem::directory_iterator(dir / "objects" / "pack"))
            if (entry.path().extension() == ".pack") {
                EXPECT_TRUE(pack.empty()) << "one packfile expected";
                pack = readFile(entry.path());
            }
        EXPECT_FALSE(pack.empty()) << "no packfile written under " << dir;
        return pack;
    };

    /* The first object follows the 12-byte pack header and a 4-byte object
       header (type and a 22-bit size, 4 bits then three 7-bit groups). */
    auto zlibHeaderOfFirstObject = [](const std::string & pack) { return pack.size() >= 18 ? pack.substr(16, 2) : ""; };

    std::string random(size, '\0');
    uint64_t x = 0x9e3779b97f4a7c15ull; /* xorshift64, fixed seed */
    for (auto & c : random) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        c = (char) x;
    }
    auto randomPack = packOf("random", random);
    EXPECT_GE(randomPack.size(), size);
    EXPECT_LE(randomPack.size(), size + size / 100);
    EXPECT_EQ(zlibHeaderOfFirstObject(randomPack), std::string("\x78\x01", 2)) << "the random blob is stored";

    std::string text;
    while (text.size() < size)
        text += "lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor\n";
    text.resize(size);
    auto textPack = packOf("text", text);
    EXPECT_LT(textPack.size(), size / 2);
    EXPECT_EQ(zlibHeaderOfFirstObject(textPack), std::string("\x78\x9c", 2))
        << "the text blob is deflated at the default";
}

TEST(GitUtils, isLegalRefName)
{
    ASSERT_TRUE(isLegalRefName("A/b"));
    ASSERT_TRUE(isLegalRefName("AaA/b"));
    ASSERT_TRUE(isLegalRefName("FOO/BAR/BAZ"));
    ASSERT_TRUE(isLegalRefName("HEAD"));
    ASSERT_TRUE(isLegalRefName("refs/tags/1.2.3"));
    ASSERT_TRUE(isLegalRefName("refs/heads/master"));
    ASSERT_TRUE(isLegalRefName("foox"));
    ASSERT_TRUE(isLegalRefName("1337"));
    ASSERT_TRUE(isLegalRefName("foo.baz"));
    ASSERT_TRUE(isLegalRefName("foo/bar/baz"));
    ASSERT_TRUE(isLegalRefName("foo./bar"));
    ASSERT_TRUE(isLegalRefName("heads/foo@bar"));
    ASSERT_TRUE(isLegalRefName("heads/fu\303\237"));
    ASSERT_TRUE(isLegalRefName("foo-bar-baz"));
    ASSERT_TRUE(isLegalRefName("branch#"));
    ASSERT_TRUE(isLegalRefName("$1"));
    ASSERT_TRUE(isLegalRefName("foo.locke"));

    ASSERT_FALSE(isLegalRefName("refs///heads/foo"));
    ASSERT_FALSE(isLegalRefName("heads/foo/"));
    ASSERT_FALSE(isLegalRefName("///heads/foo"));
    ASSERT_FALSE(isLegalRefName(".foo"));
    ASSERT_FALSE(isLegalRefName("./foo"));
    ASSERT_FALSE(isLegalRefName("./foo/bar"));
    ASSERT_FALSE(isLegalRefName("foo/./bar"));
    ASSERT_FALSE(isLegalRefName("foo/bar/."));
    ASSERT_FALSE(isLegalRefName("foo bar"));
    ASSERT_FALSE(isLegalRefName("foo?bar"));
    ASSERT_FALSE(isLegalRefName("foo^bar"));
    ASSERT_FALSE(isLegalRefName("foo~bar"));
    ASSERT_FALSE(isLegalRefName("foo:bar"));
    ASSERT_FALSE(isLegalRefName("foo[bar"));
    ASSERT_FALSE(isLegalRefName("foo/bar/."));
    ASSERT_FALSE(isLegalRefName(".refs/foo"));
    ASSERT_FALSE(isLegalRefName("refs/heads/foo."));
    ASSERT_FALSE(isLegalRefName("heads/foo..bar"));
    ASSERT_FALSE(isLegalRefName("heads/foo?bar"));
    ASSERT_FALSE(isLegalRefName("heads/foo.lock"));
    ASSERT_FALSE(isLegalRefName("heads///foo.lock"));
    ASSERT_FALSE(isLegalRefName("foo.lock/bar"));
    ASSERT_FALSE(isLegalRefName("foo.lock///bar"));
    ASSERT_FALSE(isLegalRefName("heads/v@{ation"));
    ASSERT_FALSE(isLegalRefName("heads/foo\bar"));

    ASSERT_FALSE(isLegalRefName("@"));
    ASSERT_FALSE(isLegalRefName("\37"));
    ASSERT_FALSE(isLegalRefName("\177"));

    ASSERT_FALSE(isLegalRefName("foo/*"));
    ASSERT_FALSE(isLegalRefName("*/foo"));
    ASSERT_FALSE(isLegalRefName("foo/*/bar"));
    ASSERT_FALSE(isLegalRefName("*"));
    ASSERT_FALSE(isLegalRefName("foo/*/*"));
    ASSERT_FALSE(isLegalRefName("*/foo/*"));
    ASSERT_FALSE(isLegalRefName("/foo"));
    ASSERT_FALSE(isLegalRefName(""));
}

} // namespace nix::fetchers
