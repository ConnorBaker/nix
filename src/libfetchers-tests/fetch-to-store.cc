#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/attrs.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/git.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/archive.hh"
#include "nix/util/util.hh"
#include "nix/util/hash.hh"
#include "nix/store/store-open.hh"
#include "nix/store/globals.hh"
#include "nix/store/local-store.hh"
#include "nix/store/tests/temp-local-store.hh"
#include "nix/util/tests/source-accessor-gen.hh"
#include "nix/util/tests/setting-scopes.hh"

#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

namespace nix {

namespace {

/* A small tree, optionally named as a whole. */
ref<MemorySourceAccessor> namedTree(std::optional<std::string> name)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->addFile(CanonPath("keep/x"), "x");
    accessor->addFile(CanonPath("keep/drop/y"), "y");
    accessor->addFile(CanonPath("drop/z"), "z");
    accessor->addFile(CanonPath("top"), "t");
    accessor->fingerprint = std::move(name);
    return accessor;
}

PathFilter rejectDrop = [](const std::string & path) { return !hasSuffix(path, "/drop"); };
PathFilter rejectTop = [](const std::string & path) { return !hasSuffix(path, "/top"); };

} // namespace

TEST(FixedSetFilter, namesByTheInnerNameAndTheAdmittedSet)
{
    auto inner = namedTree("T");
    auto set = filteredPaths(*inner, CanonPath::root, rejectDrop);

    auto w1 = makeFixedSetFilteringSourceAccessor(SourcePath(inner), set);
    auto w2 = makeFixedSetFilteringSourceAccessor(SourcePath(namedTree("T")), set);
    auto name1 = w1->getFingerprint(CanonPath::root);
    ASSERT_TRUE(name1.second);
    EXPECT_EQ(name1.first, CanonPath::root);
    /* Same inner name, same set: same name. */
    EXPECT_EQ(name1, w2->getFingerprint(CanonPath::root));

    /* Another set, another name. */
    auto w3 = makeFixedSetFilteringSourceAccessor(SourcePath(inner), filteredPaths(*inner, CanonPath::root, rejectTop));
    EXPECT_NE(name1.second, w3->getFingerprint(CanonPath::root).second);

    /* Another inner name, another name. */
    auto w4 = makeFixedSetFilteringSourceAccessor(SourcePath(namedTree("U")), set);
    EXPECT_NE(name1.second, w4->getFingerprint(CanonPath::root).second);

    /* A subtree is named by the set below it; at a subtree the filter
       does not touch, the name still differs from the inner name, since
       it says the set is the whole. */
    auto atKeep = w1->getFingerprint(CanonPath("keep"));
    ASSERT_TRUE(atKeep.second);
    EXPECT_EQ(atKeep.first, CanonPath::root);
    EXPECT_NE(atKeep.second, name1.second);

    /* An unnamed inner tree gives an unnamed filtered tree. */
    auto w5 = makeFixedSetFilteringSourceAccessor(SourcePath(namedTree(std::nullopt)), set);
    EXPECT_FALSE(w5->getFingerprint(CanonPath::root).second);

    /* A whole-tree name, when given, wins. */
    w5->fingerprint = "whole";
    EXPECT_EQ(w5->getFingerprint(CanonPath("keep")).second, std::optional<std::string>("whole"));
}

/* The fetcher cache under a directory of the fixture's own (`FreshCacheHome`):
   these tests write memo rows. */
class FetchToStoreTest : FreshCacheHome, public ::testing::Test
{
protected:
    void SetUp() override
    {
        nix::initLibStore(/*loadConfig=*/false);
    }
};

TEST_F(FetchToStoreTest, filteredNamedTreesAreMemoised)
{
    auto store = openStore("dummy://?read-only=false");
    fetchers::Settings settings;

    auto inner = namedTree("fetch-to-store-test-T");
    auto method = ContentAddressMethod::Raw::NixArchive;

    auto [storePath, hash] =
        fetchToStore2(settings, *store, SourcePath(inner), FetchMode::DryRun, "src", method, &rejectDrop);

    /* The result is what the filtered dump gives. */
    auto [expectedPath, expectedHash] =
        store->computeStorePath("src", SourcePath(inner), method, HashAlgorithm::SHA256, {}, rejectDrop);
    EXPECT_EQ(storePath, expectedPath);
    EXPECT_EQ(hash, expectedHash);

    /* And the memo now has it under the filtered tree's name. */
    auto wrapped =
        makeFixedSetFilteringSourceAccessor(SourcePath(inner), filteredPaths(*inner, CanonPath::root, rejectDrop));
    auto [subpath, name] = wrapped->getFingerprint(CanonPath::root);
    ASSERT_TRUE(name);
    auto row = settings.getCache()->lookup(makeSourcePathToHashCacheKey(*name, method, subpath));
    ASSERT_TRUE(row);
    EXPECT_EQ(fetchers::getStrAttr(*row, "hash"), hash.to_string(HashFormat::SRI, true));

    /* The same filtered tree under another whole-tree name is another
       memo row; a second request for the first is answered from the memo
       (the store path and hash agree, and nothing new is walked here that
       this test can see). */
    auto [storePath2, hash2] =
        fetchToStore2(settings, *store, SourcePath(inner), FetchMode::DryRun, "src", method, &rejectDrop);
    EXPECT_EQ(storePath2, storePath);
    EXPECT_EQ(hash2, hash);
}

namespace {

std::string base(const std::string & path)
{
    auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

/* Construction soundness at any prefix: dumping the fixed-set accessor with
   no filter is dumping the inner tree with that filter, at the root and at
   every directory below.  This is the example that caught the coordinate
   bug (05 section 11) generalised over random trees, filters and prefixes. */
RC_GTEST_PROP(FetchToStore, prop_wrapped_dump_equals_filtered_dump, ())
{
    auto inner = make_ref<MemorySourceAccessor>();
    auto names = std::vector<std::string>{"a", "b", "c", "d"};
    auto n = *rc::gen::inRange<int>(1, 6);
    for (int i = 0; i < n; ++i)
        inner->addFile(
            CanonPath(*rc::gen::elementOf(names)) / *rc::gen::elementOf(names), *rc::gen::arbitrary<std::string>());
    inner->addFile(CanonPath("top"), *rc::gen::arbitrary<std::string>());

    /* A random subset of `names`.  `gen::container<std::set>` over this
       four-element domain gives up once rapidcheck's size parameter asks
       for more distinct values than exist (it cannot draw 41 from 4),
       which failed the property on Linux under a seed that grew the size;
       drawing a subset directly cannot exhaust. */
    std::set<std::string> rejected;
    for (auto & nm : names)
        if (*rc::gen::arbitrary<bool>())
            rejected.insert(nm);
    PathFilter filter = [&](const std::string & path) { return !rejected.contains(base(path)); };

    std::vector<CanonPath> dirs{CanonPath::root};
    std::function<void(const CanonPath &)> walk = [&](const CanonPath & p) {
        for (auto & [name, type] : inner->readDirectory(p)) {
            auto c = p / name;
            if (inner->lstat(c).type == SourceAccessor::tDirectory) {
                dirs.push_back(c);
                walk(c);
            }
        }
    };
    walk(CanonPath::root);

    for (auto & q : dirs) {
        auto wrapped = makeFixedSetFilteringSourceAccessor(SourcePath(inner, q), filteredPaths(*inner, q, filter));
        RC_ASSERT(narOf(*wrapped, CanonPath::root) == narOf(*inner, q, filter));
    }
}

/* Naming homomorphism: two inner trees that share a subtree, filtered the
   same way, name the shared filtered subtree equally and it has the same
   NAR; a different accepted set gives a different name. */
RC_GTEST_PROP(FetchToStore, prop_filtered_names_are_sound, ())
{
    auto keepContent = *rc::gen::arbitrary<std::string>();
    auto dropContent = *rc::gen::arbitrary<std::string>();
    auto build = [&](std::string priv) {
        auto mem = make_ref<MemorySourceAccessor>();
        mem->addFile(CanonPath("shared/keep"), std::string(keepContent));
        mem->addFile(CanonPath("shared/drop"), std::string(dropContent));
        mem->addFile(CanonPath(priv), std::string(priv));
        return make_ref<ContentNamedAccessor>(mem).cast<SourceAccessor>();
    };
    auto t1 = build("x"), t2 = build("y");

    PathFilter keepOnly = [](const std::string & p) { return base(p) != "drop"; };
    auto w1 = makeFixedSetFilteringSourceAccessor(
        SourcePath(t1, CanonPath("shared")), filteredPaths(*t1, CanonPath("shared"), keepOnly));
    auto w2 = makeFixedSetFilteringSourceAccessor(
        SourcePath(t2, CanonPath("shared")), filteredPaths(*t2, CanonPath("shared"), keepOnly));

    auto name1 = w1->getFingerprint(CanonPath::root);
    RC_ASSERT(name1.second);
    RC_ASSERT(name1 == w2->getFingerprint(CanonPath::root));
    RC_ASSERT(narOf(*w1, CanonPath::root) == narOf(*w2, CanonPath::root));

    auto full = makeFixedSetFilteringSourceAccessor(
        SourcePath(t1, CanonPath("shared")), filteredPaths(*t1, CanonPath("shared"), defaultPathFilter));
    RC_ASSERT(full->getFingerprint(CanonPath::root).second != name1.second);
}

namespace {

/* A memory tree under a fixed name, with or without one extra entry.  The
   name is the memo's key, so a tree that changes under it shows exactly
   when the memo is consulted rather than the tree. */
ref<MemorySourceAccessor> treeNamed(const std::string & name, bool extra)
{
    auto a = make_ref<MemorySourceAccessor>();
    a->addFile(CanonPath("dir/keep"), "k");
    a->addFile(CanonPath("dir/drop"), "d");
    a->addFile(CanonPath("file"), "f");
    if (extra)
        a->addFile(CanonPath("dir/extra"), "e");
    a->fingerprint = name;
    return a;
}

/* An unnamed root with the named tree mounted below it: `rootFS` above a
   lazily mounted input. */
SourcePath mountedUnder(ref<SourceAccessor> tree)
{
    auto root = make_ref<MemorySourceAccessor>();
    root->addFile(CanonPath("plain"), "p");
    return SourcePath(makeMountedSourceAccessor({{CanonPath::root, root}, {CanonPath("m"), tree}}));
}

} // namespace

/* The subtree memo of the compositional name (01 section 9.9): an
   unfiltered walk answers a named subtree from the memo; a walk under a
   filter never answers a directory from it, since the filter decides the
   directory's entries, while a file's blob is the same under any filter. */
TEST_F(FetchToStoreTest, gitSubtreeMemoRespectsFilters)
{
    auto store = openStore("dummy://?read-only=false");
    fetchers::Settings settings;
    auto method = ContentAddressMethod::Raw::Git;
    /* This test poisons the memo on purpose: a name over a changed tree. */
    auto name = "fetch-to-store-test-memo-" + Hash::random(HashAlgorithm::SHA256).to_string(HashFormat::Nix32, false);
    PathFilter dropDrop = [](const std::string & p) { return !hasSuffix(p, "/drop"); };

    /* An unfiltered walk names the tree and every subtree under `name`. */
    auto t1 = mountedUnder(treeNamed(name, false));
    auto [p1, h1] = fetchToStore2(settings, *store, t1, FetchMode::DryRun, "src", method);
    EXPECT_EQ(h1, objectHashOf(*t1.accessor, t1.path).root.hash);

    /* The same name over a tree with one more entry: unfiltered, the
       subtree is answered from the memo, so the old hash comes back. */
    auto t2 = mountedUnder(treeNamed(name, true));
    auto [p2, h2] = fetchToStore2(settings, *store, t2, FetchMode::DryRun, "src", method);
    EXPECT_EQ(h2, h1);
    EXPECT_NE(h1, objectHashOf(*t2.accessor, t2.path).root.hash);

    /* Under a filter the directories are walked: the hash is the filtered
       tree's own, with `extra` in and `drop` out. */
    auto [p3, h3] = fetchToStore2(settings, *store, t2, FetchMode::DryRun, "src", method, &dropDrop);
    EXPECT_EQ(h3, objectHashOf(*t2.accessor, t2.path, dropDrop).root.hash);
    EXPECT_NE(h3, objectHashOf(*t1.accessor, t1.path, dropDrop).root.hash);
}

/* A bare root is addressed by the object hash (doc/lazy-store/
   01-specification.md, section 9.11): the blob id for a plain file, and for
   an executable file or a symlink the id of the one-entry tree `{"." ->
   entry}`, which keeps the mode.  Total, so the git method names every
   root kind, and injective, so a plain file, an executable and a symlink
   with the same bytes are three paths. */
/* A git row of the earlier form -- the hash alone, under the key without
   `version` -- is a miss (`01` §9.9, the memo's rule): a wrong hash planted there
   is not read, and the true address is computed.  A row of the current
   form is read; a bare root's row holds its entry, so no stat of the root
   is needed to address it. */
TEST_F(FetchToStoreTest, oldFormGitRowIsAMissAndTheRowIsTheEntry)
{
    auto store = openStore("dummy://?read-only=false");
    auto git = ContentAddressMethod::Raw::Git;
    auto tree = namedTree("fetch-to-store-test-V");
    auto expected = objectHashOf(*tree, CanonPath::root).root;
    auto [subpath, name] = tree->getFingerprint(CanonPath::root);
    ASSERT_TRUE(name);

    fetchers::Settings settings;
    auto wrong = merkle::treeId("");
    settings.getCache()->upsert(
        fetchers::Cache::Key{
            "sourcePathToHash",
            {{"fingerprint", *name},
             {"method", std::string{ContentAddressMethod(git).render()}},
             {"path", subpath.abs()}}},
        {{"hash", wrong.to_string(HashFormat::SRI, true)}});
    auto [p, h] = fetchToStore2(settings, *store, SourcePath(tree), FetchMode::DryRun, "src");
    EXPECT_EQ(h, expected.hash);
    EXPECT_NE(h, wrong);

    /* The row written is the entry, and it answers the next naming. */
    auto row = settings.getCache()->lookup(makeSourcePathToHashCacheKey(*name, git, subpath));
    ASSERT_TRUE(row);
    EXPECT_EQ(Hash::parseSRI(fetchers::getStrAttr(*row, "hash")), expected.hash);
    EXPECT_EQ(fetchers::getIntAttr(*row, "mode"), uint64_t(merkle::Mode::Directory));
    fetchers::Settings settings2;
    settings2.getCache()->upsert(makeSourcePathToHashCacheKey(*name, git, subpath), *row);
    auto [p2, h2] = fetchToStore2(settings2, *store, SourcePath(tree), FetchMode::DryRun, "src");
    EXPECT_EQ(p2, p);
    EXPECT_EQ(h2, h);
}

namespace {

/* A memory tree whose nodes carry the names given (`names`, a fingerprint
   per path, as a git accessor names its subtrees by object id), counting
   the files read of it: the copy route's promise is that nothing beneath
   a named subtree the store holds is read (01 §9.10, the second route). */
struct NamedCountingAccessor : MemorySourceAccessor
{
    std::map<CanonPath, std::string> names;
    unsigned reads = 0;

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        reads++;
        MemorySourceAccessor::readFile(path, sink, sizeCallback);
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        if (auto i = names.find(path); i != names.end())
            return {CanonPath::root, i->second};
        return {path, std::nullopt};
    }
};

ref<NamedCountingAccessor> dirtyTree(std::string_view b, std::string_view dName)
{
    auto t = make_ref<NamedCountingAccessor>();
    t->addFile(CanonPath("a"), "one");
    t->addFile(CanonPath("d/b"), std::string(b));
    t->addFile(CanonPath("d/c"), "three");
    t->addFile(CanonPath("e/x"), "four");
    t->addFile(CanonPath("e/y"), "five");
    /* The shape of a dirty checkout: the root and the changed directory
       carry no name the memo could answer, the clean directory does. */
    t->names.emplace(CanonPath("d"), std::string(dName));
    t->names.emplace(CanonPath("e"), "fetch-to-store-test-e-clean");
    return t;
}

} // namespace

/* The fetcher cache and a local store of the fixture's own. */
class FetchToStoreLocalTest : FreshCacheHome, public TempLocalStoreTest
{
protected:
    void SetUp() override
    {
        nix::initLibStore(/*loadConfig=*/false);
        TempLocalStoreTest::SetUp();
    }
};

/* The second route of 01 §9.10 through `fetchToStore2`: a copy into a
   local store under the git method is made from the store's own objects
   wherever the naming's memo names a directory the store holds, and reads
   the source only elsewhere; the path is the one the dry run named, its
   bytes the one route's, its files the blobs' links.  The memo's own
   window is the route's: what the memo names and the store holds is
   copied as named. */
TEST_F(FetchToStoreLocalTest, copyIsMadeFromTheObjectStoreWhereTheMemoNamesWhatItHolds)
{
    fetchers::Settings settings;
    ASSERT_TRUE(store->materialisesFromObjects());
    auto realPath = [&](const StorePath & p) { return store->toRealPath(p); };
    auto ino = [](const std::filesystem::path & p) { return lstat(p).st_ino; };

    /* The first tree: named, then copied.  Nothing is held yet, so every
       directory is read; the copy consults the memo per directory and
       never per file. */
    auto t1 = dirtyTree("two", "fetch-to-store-test-d-v1");
    auto [dry1, hash1] = fetchToStore2(settings, *store, SourcePath(t1), FetchMode::DryRun, "src");
    t1->reads = 0;
    auto [p1, h1] = fetchToStore2(settings, *store, SourcePath(t1), FetchMode::Copy, "src");
    EXPECT_EQ(p1, dry1);
    EXPECT_EQ(h1, hash1);
    EXPECT_EQ(t1->reads, 5u);
    ASSERT_TRUE(store->isValidPath(p1));
    EXPECT_EQ(narOf(*makeFSSourceAccessor(realPath(p1))), narOf(*t1));

    /* The second tree, one file changed in `d`: the dry run names it
       (reading `d`, answering `e` from the memo); the copy reads the root's
       file and `d`'s two and makes `e` from the objects, its files the
       first path's inodes. */
    auto t2 = dirtyTree("TWO", "fetch-to-store-test-d-v2");
    auto [dry2, hash2] = fetchToStore2(settings, *store, SourcePath(t2), FetchMode::DryRun, "src");
    EXPECT_NE(dry2, dry1);
    EXPECT_EQ(t2->reads, 3u); /* a, d/b, d/c: e from the memo */
    t2->reads = 0;
    auto [p2, h2] = fetchToStore2(settings, *store, SourcePath(t2), FetchMode::Copy, "src");
    EXPECT_EQ(p2, dry2);
    EXPECT_EQ(h2, hash2);
    EXPECT_EQ(t2->reads, 3u); /* a, d/b, d/c: e from the object store */
    ASSERT_TRUE(store->isValidPath(p2));
    auto nar2 = narOf(*t2);
    EXPECT_EQ(narOf(*makeFSSourceAccessor(realPath(p2))), nar2);
    auto info2 = store->queryPathInfo(p2);
    EXPECT_EQ(info2->narSize, nar2.size());
    ASSERT_TRUE(info2->objectHash);
    EXPECT_EQ(info2->objectHash->hash, hash2);
    for (auto & rel : {"a", "d/c", "e/x", "e/y"})
        EXPECT_EQ(ino(realPath(p2) / rel), ino(realPath(p1) / rel)) << rel;
    EXPECT_EQ(ino(realPath(p2) / "e/x"), ino(store->objects.blobFile(merkle::blobId("four"), false)));
    EXPECT_EQ(readFile(realPath(p2) / "d/b"), "TWO");

    /* An explicit filter on the tree: the memo answers no directory under
       a filter, so the route is the one route's walk with the filter
       applied; the path is the filtered tree's. */
    PathFilter dropY = [](const std::string & path) { return !hasSuffix(path, "/e/y"); };
    auto t3 = dirtyTree("TWO", "fetch-to-store-test-d-v2");
    auto [p3, h3] =
        fetchToStore2(settings, *store, SourcePath(t3), FetchMode::Copy, "src", ContentAddressMethod::Raw::Git, &dropY);
    auto [expected3, hash3] = store->computeStorePath(
        "src", SourcePath(t3), ContentAddressMethod::Raw::Git, HashAlgorithm::SHA256, {}, dropY);
    EXPECT_EQ(p3, expected3);
    EXPECT_EQ(h3, hash3);
    EXPECT_EQ(narOf(*makeFSSourceAccessor(realPath(p3))), narOf(*t3, CanonPath::root, dropY));
    EXPECT_FALSE(pathExists(realPath(p3) / "e/y"));

    /* The window, stated (01 §2.2): a change inside a subtree the memo
       still names -- here `e/y`, its name kept -- is not seen by the copy,
       which makes `e` as the memo names it; the naming would have said the
       same.  The path is the one named, and its bytes are that tree's. */
    auto t4 = dirtyTree("TWO", "fetch-to-store-test-d-v3");
    auto [dry4, hash4] = fetchToStore2(settings, *store, SourcePath(t4), FetchMode::DryRun, "src");
    t4->addFile(CanonPath("e/y"), "FIVE");
    auto [p4, h4] = fetchToStore2(settings, *store, SourcePath(t4), FetchMode::Copy, "src");
    EXPECT_EQ(p4, dry4);
    EXPECT_EQ(readFile(realPath(p4) / "e/y"), "five");
    EXPECT_EQ(narOf(*makeFSSourceAccessor(realPath(p4))), narOf(*dirtyTree("TWO", "")));
}

TEST_F(FetchToStoreTest, aBareRootIsAddressedByTheObjectHash)
{
    auto store = openStore("dummy://?read-only=false");
    /* No setting: the git name is the only name (04 section 1.9, "Step
       22's representation"). */
    fetchers::Settings settings;
    auto git = ContentAddressMethod::Raw::Git;

    auto mk = [](auto fill) {
        auto acc = make_ref<MemorySourceAccessor>();
        MemorySink sink{*acc};
        fill(sink);
        return acc;
    };
    auto plain = mk(
        [](MemorySink & s) { s.createRegularFile(CanonPath::root, [](CreateRegularFileSink & f) { f("hello\n"); }); });
    auto exec = mk([](MemorySink & s) {
        s.createRegularFile(CanonPath::root, [](CreateRegularFileSink & f) {
            f.isExecutable();
            f("hello\n");
        });
    });
    auto link = mk([](MemorySink & s) { s.createSymlink(CanonPath::root, "hello"); });
    auto dir = mk([](MemorySink & s) {
        s.createDirectory(CanonPath::root);
        s.createRegularFile(CanonPath("run"), [](CreateRegularFileSink & f) {
            f.isExecutable();
            f("hello\n");
        });
    });

    /* The executable root under the default: its hash is the object hash
       of its entry, and its path is not the plain file's and not the
       symlink's. */
    auto [pExec, hExec] = fetchToStore2(settings, *store, SourcePath(exec), FetchMode::DryRun, "x");
    EXPECT_EQ(hExec, merkle::objectHash(objectHashOf(*exec, CanonPath::root).root));
    auto [pPlain, hPlain] = fetchToStore2(settings, *store, SourcePath(plain), FetchMode::DryRun, "x");
    auto [pLink, hLink] = fetchToStore2(settings, *store, SourcePath(link), FetchMode::DryRun, "x");
    EXPECT_NE(pExec, pPlain);
    EXPECT_NE(pExec, pLink);
    EXPECT_NE(pPlain, pLink);
    /* The default is the git method: the path's content address says so. */
    EXPECT_EQ(pExec, store->makeFixedOutputPathFromCA("x", ContentAddressWithReferences::fromParts(git, hExec, {})));

    /* The plain file's object hash is its blob id: git's own identifier
       wherever git has one. */
    auto ePlain = objectHashOf(*plain, CanonPath::root).root;
    EXPECT_EQ(ePlain.mode, git::Mode::Regular);
    EXPECT_EQ(hPlain, ePlain.hash);
    EXPECT_EQ(hPlain, merkle::objectHash(ePlain));

    /* The git method asked for by name is no longer refused for an
       executable root, and gives the path the default does. */
    auto [pByName, hByName] = fetchToStore2(settings, *store, SourcePath(exec), FetchMode::DryRun, "x", git);
    EXPECT_EQ(pByName, pExec);
    EXPECT_EQ(hByName, hExec);

    /* A directory takes its tree id, as before. */
    auto [pDir, hDir] = fetchToStore2(settings, *store, SourcePath(dir), FetchMode::DryRun, "x");
    EXPECT_EQ(hDir, objectHashOf(*dir, CanonPath::root).root.hash);
}

} // namespace nix
