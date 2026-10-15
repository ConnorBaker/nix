#include "nix/util/environment-variables.hh"
#include "nix/util/finally.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/store/globals.hh"
#include "nix/util/hash.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/util/merkle-files.hh"
#include "nix/util/nar-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/git.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/terminal.hh"
#include "nix/util/users.hh"
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
#include <git2/status.h>
#include <git2/types.h>
#include <git2/object.h>
#include <git2/odb.h>
#include <git2/tag.h>
#include <gtest/gtest.h>
#include "nix/util/serialise.hh"

#include <git2/attr.h>
#include <git2/blob.h>
#include <git2/commit.h>
#include <git2/config.h>
#include <git2/index.h>
#include <git2/refs.h>
#include <git2/tree.h>

#include <rapidcheck/gtest.h>
#include <algorithm>
#include <array>
#include <exception> // IWYU pragma: keep (rapidcheck on Darwin/FreeBSD)
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <ranges>
#include <stdexcept>
#include <tuple>
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

/* A bare blob has one root file. Reads and metadata have the same domain. */
TEST_F(GitUtilsTest, blobRootHasNoChildren)
{
    for (auto algorithm : {HashAlgorithm::SHA1, HashAlgorithm::SHA256}) {
        SCOPED_TRACE(printHashAlgo(algorithm));
        auto path = tmpDir / printHashAlgo(algorithm);
        GitRepo::Options options{.create = true, .bare = true, .oidType = algorithm};
        auto repo = GitRepo::openRepo(path, options);
        auto pool = GitRepoPool::create(path, options);
        auto blob = writeString(*pool, "root bytes");
        pool->flush();
        auto accessor = repo->getAccessor(blob.hash, {}, "blob");

        EXPECT_TRUE(accessor->pathExists(CanonPath::root));
        EXPECT_EQ(accessor->lstat(CanonPath::root).type, SourceAccessor::tRegular);
        EXPECT_EQ(accessor->readFile(CanonPath::root), "root bytes");
        EXPECT_THROW(accessor->readLink(CanonPath::root), Error);
        EXPECT_THROW(accessor->readDirectory(CanonPath::root), Error);
        /* Direct children and paths with a nonroot parent exercise both
           lookup routes; neither can exist below a regular-file root. */
        for (const auto & path : {CanonPath("child"), CanonPath("child/grandchild")}) {
            SCOPED_TRACE(path.abs());
            EXPECT_FALSE(accessor->pathExists(path));
            EXPECT_FALSE(accessor->maybeLstat(path));
            EXPECT_THROW(accessor->readFile(path), Error);
            EXPECT_THROW(accessor->readLink(path), Error);
            EXPECT_THROW(accessor->readDirectory(path), Error);
        }
        EXPECT_EQ(accessor->readFile(CanonPath::root), "root bytes");
    }
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
    ASSERT_TRUE(shared1);
    EXPECT_EQ(shared1, shared2);
    EXPECT_EQ(shared1->first, CanonPath::root);
    EXPECT_EQ(acc1->getFingerprint(CanonPath("shared/a")), acc2->getFingerprint(CanonPath("shared/a")));

    /* The law: equal names, equal NARs. */
    StringSink nar1, nar2;
    acc1->dumpPath(CanonPath("shared"), nar1);
    acc2->dumpPath(CanonPath("shared"), nar2);
    EXPECT_EQ(nar1.s, nar2.s);

    /* Different roots, different names; both named. */
    auto rootName1 = acc1->getFingerprint(CanonPath::root);
    auto rootName2 = acc2->getFingerprint(CanonPath::root);
    ASSERT_TRUE(rootName1);
    ASSERT_TRUE(rootName2);
    EXPECT_NE(rootName1->second, rootName2->second);

    /* The mode is part of the NAR, so it is part of the name: same
       contents, different executable bit, different names and NARs. */
    auto exe = acc2->getFingerprint(CanonPath("exe"));
    auto plain = acc2->getFingerprint(CanonPath("plain"));
    ASSERT_TRUE(exe);
    ASSERT_TRUE(plain);
    EXPECT_NE(exe->second, plain->second);
    StringSink narExe, narPlain;
    acc2->dumpPath(CanonPath("exe"), narExe);
    acc2->dumpPath(CanonPath("plain"), narPlain);
    EXPECT_NE(narExe.s, narPlain.s);
    auto link = acc2->getFingerprint(CanonPath("link"));
    ASSERT_TRUE(link);
    EXPECT_NE(link->second, plain->second);

    /* A path that is not there has no name. */
    EXPECT_FALSE(acc1->getFingerprint(CanonPath("nope")));

    /* A name given to the whole does not displace the finer subtree names. */
    acc1->fingerprint = "input-level";
    EXPECT_EQ(acc1->getFingerprint(CanonPath("shared")), shared1);
}

TEST_F(GitUtilsTest, rawModesMetadataReadersAndNamesAgree)
{
    for (auto algorithm : {HashAlgorithm::SHA1, HashAlgorithm::SHA256}) {
        SCOPED_TRACE(printHashAlgo(algorithm));
        auto path = tmpDir / printHashAlgo(algorithm);
        auto repo = GitRepo::openRepo(path, {.create = true, .bare = true, .oidType = algorithm});
        git_repository * rawRepo = nullptr;
        ASSERT_EQ(git_repository_open(&rawRepo, path.string().c_str()), 0);
        Finally closeRepo{[&] { git_repository_free(rawRepo); }};
        git_odb * odb = nullptr;
        ASSERT_EQ(git_repository_odb(&odb, rawRepo), 0);
        Finally closeOdb{[&] { git_odb_free(odb); }};
        git_oid blob, emptyTree;
        ASSERT_EQ(git_odb_write(&blob, odb, "x", 1, GIT_OBJECT_BLOB), 0);
        ASSERT_EQ(git_odb_write(&emptyTree, odb, "", 0, GIT_OBJECT_TREE), 0);
        const size_t oidSize = algorithm == HashAlgorithm::SHA1 ? GIT_OID_SHA1_SIZE : GIT_OID_SHA256_SIZE;
        auto absent = blob;
        std::fill_n(absent.id, oidSize, 0xff);
        ASSERT_EQ(git_odb_exists(odb, &absent), 0);

        /* Every uint16 raw mode in one immutable tree, with just two
           payload objects. Fixed-width names stay sorted with tree suffixes. */
        constexpr unsigned modes = 1u << 16;
        std::string body;
        body.reserve((12 + oidSize) * modes);
        for (unsigned mode = 0; mode < modes; ++mode) {
            body += fmt("%o %04x", mode, mode);
            body += '\0';
            auto kind = mode & 0170000;
            const auto & oid = kind == 0040000 ? emptyTree : kind == 0160000 ? absent : blob;
            body.append(reinterpret_cast<const char *>(oid.id), oidSize);
        }
        git_oid root;
        ASSERT_EQ(git_odb_write(&root, odb, body.data(), body.size(), GIT_OBJECT_TREE), 0);
        git_tree * tree = nullptr;
        ASSERT_EQ(git_tree_lookup(&tree, rawRepo, &root), 0);
        Finally closeTree{[&] { git_tree_free(tree); }};
        ASSERT_EQ(git_tree_entrycount(tree), modes);
        auto acc = repo->getAccessor(Hash::parseAny(git_oid_tostr_s(&root), algorithm), {}, "raw-modes");
        const auto entries = acc->readDirectory(CanonPath::root);
        ASSERT_EQ(entries.size(), modes);
        unsigned named = 0, corrected = 0;
        unsigned typeMismatches = 0, executableMismatches = 0, listingMismatches = 0;
        for (unsigned mode = 0; mode < modes; ++mode) {
            SCOPED_TRACE(fmt("%o", mode));
            const auto name = fmt("%04x", mode);
            const CanonPath node(name);
            const auto kind = mode & 0170000;
            const bool executable = mode & 0100;
            const auto objectType = kind == 0040000 ? GIT_OBJECT_TREE
                                    : kind == 0160000 ? GIT_OBJECT_COMMIT : GIT_OBJECT_BLOB;
            const auto normalized = kind == 0040000 ? GIT_FILEMODE_TREE
                                    : executable ? GIT_FILEMODE_BLOB_EXECUTABLE
                                    : kind == 0160000 ? GIT_FILEMODE_COMMIT
                                    : kind == 0120000 ? GIT_FILEMODE_LINK : GIT_FILEMODE_BLOB;
            const auto type = objectType != GIT_OBJECT_BLOB ? SourceAccessor::tDirectory
                              : normalized == GIT_FILEMODE_LINK ? SourceAccessor::tSymlink : SourceAccessor::tRegular;
            auto entry = git_tree_entry_byindex(tree, mode);
            ASSERT_NE(entry, nullptr);
            EXPECT_EQ(git_tree_entry_name(entry), name);
            EXPECT_EQ(unsigned(git_tree_entry_filemode_raw(entry)), mode);
            EXPECT_EQ(git_tree_entry_filemode(entry), normalized);
            EXPECT_EQ(git_tree_entry_type(entry), objectType);
            const auto stat = acc->lstat(node);
            typeMismatches += stat.type != type;
            executableMismatches += stat.isExecutable != (type == SourceAccessor::tRegular && executable);
            EXPECT_FALSE(stat.fileSize);
            listingMismatches += entries.at(name) != std::optional(type);
            corrected += kind == 0160000 && executable;
            if (type == SourceAccessor::tDirectory)
                EXPECT_TRUE(acc->readDirectory(node).empty());
            else
                EXPECT_THROW(acc->readDirectory(node), Error);
            if (type == SourceAccessor::tRegular) {
                unsigned announcements = 0;
                std::string contents;
                LambdaSink sink{[&](std::string_view bytes) {
                    EXPECT_EQ(announcements, 1u);
                    contents += bytes;
                }};
                acc->readFile(node, sink, [&](uint64_t size) {
                    EXPECT_EQ(size, 1u);
                    ++announcements;
                });
                EXPECT_EQ(announcements, 1u);
                EXPECT_EQ(contents, "x");
            } else
                EXPECT_THROW(acc->readFile(node), Error);
            if (type == SourceAccessor::tSymlink)
                EXPECT_EQ(acc->readLink(node), "x");
            else
                EXPECT_THROW(acc->readLink(node), Error);
            auto identity = acc->getFingerprint(node);
            const bool canonical =
                mode == 0040000 || mode == 0100644 || mode == 0100755 || mode == 0120000 || mode == 0160000;
            EXPECT_EQ(identity.has_value(), canonical);
            EXPECT_FALSE(acc->getKnownEntry(node)) << "raw ODB writes have no canonical-writer provenance";
            if (identity) {
                ++named;
                EXPECT_EQ(identity->first, CanonPath::root);
                EXPECT_THAT(identity->second, ::testing::StartsWith("git-object-v2:"));
            }
        }
        EXPECT_EQ(named, 5u);
        EXPECT_EQ(corrected, 2048u);
        /* Aggregate metadata contradictions so a negative control reads
           every cell without printing thousands of identical failures. */
        EXPECT_EQ(typeMismatches, 0u);
        EXPECT_EQ(executableMismatches, 0u);
        EXPECT_EQ(listingMismatches, 0u);
        /* The parser rejects values outside uint16; it does not truncate.
           Both 0 and 0177777 already round-tripped in the complete tree. */
        for (auto mode : {"-1", "200000"}) {
            auto invalid = std::string(mode) + " node";
            invalid += '\0';
            invalid.append(reinterpret_cast<const char *>(blob.id), oidSize);
            git_oid oid;
            ASSERT_EQ(git_odb_write(&oid, odb, invalid.data(), invalid.size(), GIT_OBJECT_TREE), 0);
            git_tree * rejected = nullptr;
            EXPECT_LT(git_tree_lookup(&rejected, rawRepo, &oid), 0);
            git_tree_free(rejected);
        }
    }
}

TEST_F(GitUtilsTest, malformedModesCannotReuseExecutableOrLegacyParentMemos)
{
    nix::initLibStore(/*loadConfig=*/false);
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    Finally closeRepo{[&] { git_repository_free(rawRepo); }};
    git_odb * odb = nullptr;
    ASSERT_EQ(git_repository_odb(&odb, rawRepo), 0);
    Finally closeOdb{[&] { git_odb_free(odb); }};
    git_oid blob;
    ASSERT_EQ(git_odb_write(&blob, odb, "x", 1, GIT_OBJECT_BLOB), 0);
    std::array<git_oid, 2> roots{};
    const std::array modes{"100755", "160100"};
    for (size_t i = 0; i < roots.size(); ++i) {
        auto body = std::string(modes[i]) + " node";
        body += '\0';
        body.append(reinterpret_cast<const char *>(blob.id), GIT_OID_SHA1_SIZE);
        ASSERT_EQ(git_odb_write(&roots[i], odb, body.data(), body.size(), GIT_OBJECT_TREE), 0);
    }
    auto repo = openRepo();
    auto good = repo->getAccessor(Hash::parseAny(git_oid_tostr_s(&roots[0]), HashAlgorithm::SHA1), {}, "executable");
    auto badHash = Hash::parseAny(git_oid_tostr_s(&roots[1]), HashAlgorithm::SHA1);
    auto bad = repo->getAccessor(badHash, {}, "gitlink-with-executable-bit");
    const CanonPath node("node");
    ASSERT_TRUE(good->lstat(node).isExecutable);
    EXPECT_EQ(bad->lstat(node).type, SourceAccessor::tDirectory);
    EXPECT_FALSE(bad->lstat(node).isExecutable);
    EXPECT_EQ(bad->readDirectory(CanonPath::root).at("node"), std::optional(SourceAccessor::tDirectory));
    EXPECT_FALSE(bad->getFingerprint(node));
    EXPECT_EQ(good->readFile(node), "x");
    EXPECT_TRUE(bad->readDirectory(node).empty());
    EXPECT_THROW(bad->readFile(node), Error);

    auto store = openStore("dummy://?read-only=false");
    fetchers::Settings settings;
    const auto method = ContentAddressMethod::Raw::Git;
    const merkle::TreeEntry executable{merkle::Mode::Executable, merkle::blobId("x")};
    const auto result = fetchToStore2(settings, *store, SourcePath(good, node), FetchMode::DryRun, "entry", method);
    EXPECT_EQ(result.second, merkle::objectHash(executable));
    auto identity = good->getFingerprint(node);
    ASSERT_TRUE(identity);
    auto row = settings.getCache()->lookup(makeSourcePathToHashCacheKey(identity->second, method, identity->first));
    ASSERT_TRUE(row);
    EXPECT_EQ(fetchers::getIntAttr(*row, "mode"), uint64_t(merkle::Mode::Executable));

    const merkle::TreeEntry empty{merkle::Mode::Directory, merkle::treeId("")};
    auto correct = [&](const fetchers::Settings & settings, const SourcePath & source, const merkle::TreeEntry & entry) {
        EXPECT_NO_THROW({
            auto result = fetchToStore2(settings, *store, source, FetchMode::DryRun, "entry", method);
            EXPECT_EQ(result.second, merkle::objectHash(entry));
        });
    };
    /* The same Settings may hit its result memo, fresh Settings persistent
       rows. An unnamed malformed gitlink must never reuse the executable. */
    correct(settings, SourcePath(bad, node), empty);
    fetchers::Settings fresh;
    correct(fresh, SourcePath(bad, node), empty);
    EXPECT_EQ(settings.getCache()->lookup(makeSourcePathToHashCacheKey(identity->second, method, identity->first)), row);
    auto memory = make_ref<MemorySourceAccessor>();
    MemorySink sink{*memory};
    sink.createDirectory(CanonPath::root);
    StringSink expectedNar, actualNar;
    memory->dumpPath(CanonPath::root, expectedNar);
    EXPECT_NO_THROW(bad->dumpPath(node, actualNar));
    EXPECT_EQ(actualNar.s, expectedNar.s);

    /* v1 could fold a bad child hit into a parent row. Correct leaf
       classification must not let that historical executable answer back in. */
    const merkle::Tree substituted{{"node", executable}};
    auto wrongParent = merkle::treeId(merkle::serialiseTree(substituted));
    auto legacyKey = makeSourcePathToHashCacheKey("git-object-v1:tree:" + badHash.gitRev(), method, CanonPath::root);
    settings.getCache()->upsert(
        legacyKey,
        {{"mode", uint64_t(merkle::Mode::Directory)}, {"hash", wrongParent.to_string(HashFormat::SRI, true)}});
    const auto poisoned = settings.getCache()->lookup(legacyKey);
    ASSERT_TRUE(poisoned);
    merkle::Tree rendered;
    merkle::insertEntry(rendered, "node", empty);
    const merkle::TreeEntry parent{merkle::Mode::Directory, merkle::treeId(merkle::serialiseTree(rendered))};
    EXPECT_NE(parent.hash, wrongParent);
    correct(fresh, SourcePath(bad), parent);
    fetchers::Settings again;
    correct(again, SourcePath(bad), parent);
    auto parentName = bad->getFingerprint(CanonPath::root);
    ASSERT_TRUE(parentName);
    auto parentRow = again.getCache()->lookup(makeSourcePathToHashCacheKey(parentName->second, method, parentName->first));
    EXPECT_EQ(settings.getCache()->lookup(legacyKey), poisoned);
    ASSERT_TRUE(parentRow);
    EXPECT_EQ(fetchers::getIntAttr(*parentRow, "mode"), uint64_t(merkle::Mode::Directory));
    EXPECT_EQ(fetchers::getStrAttr(*parentRow, "hash"), parent.hash.to_string(HashFormat::SRI, true));
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
        RC_ASSERT(f1);
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
   .drv/toFile is one regular file, Store::addTextToStore) is the
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

TEST_F(GitUtilsTest, canonicalEntriesRequireProvenanceAndPreserveEveryMode)
{
    auto dir = createTempDir();
    AutoDelete cleanup(dir, true);
    GitRepo::Options options{.create = true, .bare = true, .oidType = HashAlgorithm::SHA256, .canonicalObjects = true};
    auto repo = GitRepo::openRepo(dir / "objects", options);
    auto pool = GitRepoPool::create(dir / "objects", options);
    auto memory = make_ref<MemorySourceAccessor>();
    MemorySink sink{*memory};
    sink.createDirectory(CanonPath::root);
    sink.createDirectory(CanonPath("directory"));
    sink.createRegularFile(CanonPath("regular"), [](auto & file) { file("same"); });
    sink.createRegularFile(CanonPath("executable"), [](auto & file) {
        file.isExecutable();
        file("same");
    });
    sink.createSymlink(CanonPath("symlink"), "same");
    auto tree = ingestAccessorToTree(*memory, CanonPath::root, *pool);
    pool->flush();
    auto canonical = repo->getAccessor(tree.hash, {}, "canonical");
    options.canonicalObjects = false;
    auto ordinary = GitRepo::openRepo(dir / "objects", options)->getAccessor(tree.hash, {}, "ordinary");
    for (auto path : {"/", "/regular", "/executable", "/symlink", "/directory"}) {
        SCOPED_TRACE(path);
        auto expected = objectHashOf(*memory, CanonPath(path)).root;
        EXPECT_EQ(canonical->getKnownEntry(CanonPath(path)), expected);
        EXPECT_FALSE(ordinary->getKnownEntry(CanonPath(path))) << "SHA-256 alone is not canonical-tree provenance";
        EXPECT_EQ(objectHashOf(*ordinary, CanonPath(path)).root, expected);
    }
    EXPECT_FALSE(canonical->maybeLstat(CanonPath("regular"))->fileSize);
    EXPECT_EQ(canonical->readFile(CanonPath("regular")), "same");
}

TEST_F(GitUtilsTest, canonicalWriterRequiresLogicalNamesInEveryMode)
{
    using merkle::Mode;

    struct Name
    {
        std::string value;
        bool raw;
        bool canonical;
    };

    const std::string marker(caseHackSuffix);
    const std::array names{
        Name{"", false, false},
        Name{".", true, false},
        Name{"..", true, false},
        Name{"a/b", false, false},
        Name{std::string("a\0b", 3), false, false},
        Name{marker, true, false},
        Name{marker + "x", true, false},
        Name{"x" + marker + "y", true, false},
        Name{"x" + marker, true, false},
        Name{"entry", true, true},
        Name{".git", true, true},
        Name{"...", true, true},
        Name{"x~NIX~CASE~HACK~1", true, true},
    };
    const std::array modes{Mode::Regular, Mode::Executable, Mode::Symlink, Mode::Directory};
    size_t cases = 0;
    for (bool canonical : {false, true}) {
        GitRepo::Options options{
            .create = true, .bare = true, .oidType = HashAlgorithm::SHA256, .canonicalObjects = canonical};
        auto path = tmpDir / (canonical ? "canonical-names" : "raw-names");
        auto repo = GitRepo::openRepo(path, options);
        auto pool = GitRepoPool::create(path, options);
        auto blob = pool->makeRegularFileSink();
        (*blob)("contents");
        const auto blobHash = std::move(*blob).finalize();
        const auto emptyHash = std::move(*pool->makeDirectorySink()).finalize();
        for (bool caseHack : {false, true}) {
            WithCaseHack policy(caseHack);
            for (size_t n = 0; n != names.size(); ++n)
                for (auto mode : modes) {
                    SCOPED_TRACE(
                        fmt("canonical=%d caseHack=%d name=%d mode=%o", canonical, caseHack, n, (RawMode) mode));
                    auto sink = pool->makeDirectorySink();
                    const auto hash = mode == Mode::Directory ? emptyHash : blobHash;
                    const merkle::TreeEntry entry{mode, hash};
                    if (canonical ? names[n].canonical : names[n].raw) {
                        sink->insertChild(names[n].value, entry);
                        auto tree = std::move(*sink).finalize();
                        const auto body = fmt("%o ", (RawMode) mode) + names[n].value + '\0'
                                          + std::string(reinterpret_cast<const char *>(hash.hash), hash.hashSize);
                        EXPECT_EQ(tree, merkle::treeId(body));
                        pool->flush();
                        auto accessor = repo->getAccessor(tree, {}, "name-domain");
                        if (canonical)
                            EXPECT_EQ(
                                accessor->getKnownEntry(CanonPath::root), (merkle::TreeEntry{Mode::Directory, tree}));
                        else
                            EXPECT_FALSE(accessor->getKnownEntry(CanonPath::root));
                    } else
                        EXPECT_THROW(sink->insertChild(names[n].value, entry), Error);
                    ++cases;
                }
        }
    }
    EXPECT_EQ(cases, 208u);
}

TEST_F(GitUtilsTest, canonicalWriterRequiresOneChildPerLogicalName)
{
    using merkle::Mode;
    const std::array modes{Mode::Regular, Mode::Executable, Mode::Symlink, Mode::Directory};
    size_t cases = 0;
    for (bool canonical : {false, true}) {
        GitRepo::Options options{
            .create = true, .bare = true, .oidType = HashAlgorithm::SHA256, .canonicalObjects = canonical};
        auto pool = GitRepoPool::create(tmpDir / (canonical ? "canonical-duplicates" : "raw-duplicates"), options);
        auto file = pool->makeRegularFileSink();
        (*file)("value");
        auto blob = std::move(*file).finalize();
        auto directory = std::move(*pool->makeDirectorySink()).finalize();
        for (auto first : modes)
            for (auto second : modes) {
                SCOPED_TRACE(fmt("canonical=%d first=%o second=%o", canonical, (RawMode) first, (RawMode) second));
                auto sink = pool->makeDirectorySink();
                const merkle::TreeEntry a{first, first == Mode::Directory ? directory : blob};
                const merkle::TreeEntry b{second, second == Mode::Directory ? directory : blob};
                sink->insertChild("child", a);
                merkle::Tree expected{{first == Mode::Directory ? "child/" : "child", a}};
                if (canonical)
                    EXPECT_THROW(sink->insertChild("child", b), Error);
                else {
                    sink->insertChild("child", b);
                    expected.insert_or_assign(second == Mode::Directory ? "child/" : "child", b);
                }
                EXPECT_EQ(std::move(*sink).finalize(), merkle::treeId(merkle::serialiseTree(expected)));
                ++cases;
            }
    }
    EXPECT_EQ(cases, 32u);
}

TEST_F(GitUtilsTest, canonicalTarballCacheDoesNotTrustTheOldNameDomain)
{
    GitRepo::Options oldOptions{.create = true, .bare = true, .oidType = HashAlgorithm::SHA256};
    auto oldPath = getCacheDir() / "tarball-cache-v3";
    auto oldRepo = GitRepo::openRepo(oldPath, oldOptions);
    auto oldPool = GitRepoPool::create(oldPath, oldOptions);
    auto file = oldPool->makeRegularFileSink();
    (*file)(tmpDir.string());
    const auto blob = std::move(*file).finalize();
    auto tree = oldPool->makeDirectorySink();
    tree->insertChild("entry~nix~case~hack~1", {merkle::Mode::Regular, blob});
    const auto oldHash = std::move(*tree).finalize();
    oldPool->flush();
    ASSERT_TRUE(oldRepo->hasObject(oldHash));

    Settings settings;
    auto current = settings.getTarballCache();
    EXPECT_FALSE(current->hasObject(oldHash)) << "v3 writer provenance cannot justify canonical entries";
    auto currentPool = settings.getTarballWriterPool();
    auto writer = currentPool->makeDirectorySink();
    EXPECT_THROW(writer->insertChild("entry~nix~case~hack~1", {merkle::Mode::Regular, blob}), Error);
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

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        return inner->getFingerprint(path);
    }
};

/* A source name identifies the physical tree; the directory-name decoding
   policy is a separate argument to its Git/NAR fold. Reusing a successful
   fold must not turn a later reserved-name error into a cached success. */
TEST_F(GitUtilsTest, namedMemoIncludesDirectoryNameDecoding)
{
    nix::initLibStore(/*loadConfig=*/false);
    auto store = make_ref<DummyStoreConfig>(StoreReference::Params{})->openStore();
    auto repo = openRepo();
    auto pool = openWriterPool();
    size_t serial = 0;
    for (bool firstHack : {false, true})
        for (bool secondHack : {false, true})
            for (auto method : {ContentAddressMethod::Raw::Git, ContentAddressMethod::Raw::NixArchive})
                for (bool persistent : {false, true})
                    for (bool suffixed : {false, true}) {
                        SCOPED_TRACE(
                            fmt("first=%d second=%d method=%d persistent=%d suffix=%d",
                                firstHack,
                                secondHack,
                                static_cast<int>(method),
                                persistent,
                                suffixed));
                        const auto contents = fmt("decoding-cell-%d", serial++);
                        const auto physical = suffixed ? "file" + std::string(caseHackSuffix) + "7" : "file";
                        auto directory = pool->makeDirectorySink();
                        directory->insertChild(physical, writeString(*pool, contents));
                        auto tree = std::move(*directory).finalize();
                        pool->flush();
                        auto source = make_ref<CountingAccessor>(repo->getAccessor(tree, {}, "decoding"));
                        ASSERT_TRUE(source->getFingerprint(CanonPath::root));

                        /* The independent expected tree has the logical name,
                           with neither source naming nor memoisation. */
                        auto logical = make_ref<MemorySourceAccessor>();
                        MemorySink sink{*logical};
                        sink.createDirectory(CanonPath::root);
                        sink.createRegularFile(CanonPath("file"), [&](auto & file) { file(contents); });
                        auto expected =
                            store->computeStorePath("source", SourcePath(logical), method, HashAlgorithm::SHA256);

                        Settings firstSettings;
                        {
                            WithCaseHack setting(firstHack);
                            auto fetch = [&] {
                                return fetchToStore2(
                                    firstSettings, *store, SourcePath(source), FetchMode::DryRun, "source", method);
                            };
                            if (suffixed && !firstHack)
                                EXPECT_THROW(fetch(), Error);
                            else
                                EXPECT_EQ(fetch(), expected);
                        }

                        /* A fresh Settings has no in-process memo, but shares
                           this fixture's persistent fetcher cache. */
                        Settings nextSettings;
                        source->reads = 0;
                        {
                            WithCaseHack setting(secondHack);
                            auto fetch = [&] {
                                return fetchToStore2(
                                    persistent ? nextSettings : firstSettings,
                                    *store,
                                    SourcePath(source),
                                    FetchMode::DryRun,
                                    "source",
                                    method);
                            };
                            if (suffixed && !secondHack)
                                EXPECT_THROW(fetch(), Error);
                            else {
                                EXPECT_EQ(fetch(), expected);
                                if (firstHack == secondHack)
                                    EXPECT_EQ(source->reads, 0u);
                            }
                        }
                    }
    EXPECT_EQ(serial, 32u);
}

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
    ASSERT_TRUE(first->getFingerprint(CanonPath::root));
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

    /* Listing retains tree ownership; cached entry pointers and negative
       lookups survive further listings and lookups in other directories. */
    for (size_t pass = 0; pass < 2; ++pass) {
        EXPECT_EQ(acc->readDirectory(CanonPath("dir")).size(), 1u);
        EXPECT_EQ(acc->readDirectory(CanonPath("submodule")).size(), 0u);
        EXPECT_FALSE(acc->maybeLstat(CanonPath("missing")));
        EXPECT_FALSE(acc->maybeLstat(CanonPath("dir/missing")));
        EXPECT_EQ(acc->readFile(CanonPath("dir/inner")), "x");
        EXPECT_EQ(acc->readFile(CanonPath("file")), "x");
        EXPECT_EQ(acc->readFile(CanonPath("exe")), "x");
        EXPECT_EQ(acc->readLink(CanonPath("link")), "x");
        EXPECT_EQ(acc->readDirectory(CanonPath::root), entries);
    }
}

/* The entry mode selects an operation; the ODB object must also have the
   required type. Raw tree bodies deliberately bypass treebuilder's checks. */
TEST_F(GitUtilsTest, entryReadsRequireMatchingObjectTypes)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    Finally closeRepo{[&] { git_repository_free(rawRepo); }};
    git_odb * odb = nullptr;
    ASSERT_EQ(git_repository_odb(&odb, rawRepo), 0);
    Finally closeOdb{[&] { git_odb_free(odb); }};

    auto treeBody = [](std::string_view mode, const git_oid & oid) {
        auto body = std::string(mode) + " node";
        body += '\0';
        body.append(reinterpret_cast<const char *>(oid.id), GIT_OID_SHA1_SIZE);
        return body;
    };

    std::array<git_oid, 5> objects{};
    ASSERT_EQ(git_odb_write(&objects[0], odb, "x", 1, GIT_OBJECT_BLOB), 0);
    auto body = treeBody("100644", objects[0]);
    ASSERT_EQ(git_odb_write(&objects[1], odb, body.data(), body.size(), GIT_OBJECT_TREE), 0);
    body = std::string("tree ") + git_oid_tostr_s(&objects[1])
           + "\nauthor Nix <nix@example.com> 0 +0000\ncommitter Nix <nix@example.com> 0 +0000\n\ncommit\n";
    ASSERT_EQ(git_odb_write(&objects[2], odb, body.data(), body.size(), GIT_OBJECT_COMMIT), 0);
    body = std::string("object ") + git_oid_tostr_s(&objects[2])
           + "\ntype commit\ntag test\ntagger Nix <nix@example.com> 0 +0000\n\ntag\n";
    ASSERT_EQ(git_odb_write(&objects[3], odb, body.data(), body.size(), GIT_OBJECT_TAG), 0);
    ASSERT_EQ(git_oid_fromstr(&objects[4], "ffffffffffffffffffffffffffffffffffffffff"), 0);
    ASSERT_EQ(git_odb_exists(odb, &objects[4]), 0);

    const std::array types{GIT_OBJECT_BLOB, GIT_OBJECT_TREE, GIT_OBJECT_COMMIT, GIT_OBJECT_TAG};
    for (size_t i = 0; i < types.size(); ++i) {
        git_object * object = nullptr;
        ASSERT_EQ(git_object_lookup(&object, rawRepo, &objects[i], GIT_OBJECT_ANY), 0);
        Finally closeObject{[&] { git_object_free(object); }};
        ASSERT_EQ(git_object_type(object), types[i]);
    }

    std::vector<std::pair<std::string, git_filemode_t>> modes;
    for (unsigned kind = 0; kind < 16; ++kind)
        for (bool executable : {false, true}) {
            const auto raw = (kind << 12) | (executable ? 0100 : 0);
            const auto reader = kind == 4 ? GIT_FILEMODE_TREE : kind == 14 ? GIT_FILEMODE_COMMIT
                                : executable ? GIT_FILEMODE_BLOB_EXECUTABLE
                                : kind == 10 ? GIT_FILEMODE_LINK : GIT_FILEMODE_BLOB;
            modes.emplace_back(fmt("%o", raw), reader);
        }
    modes.emplace_back("100644", GIT_FILEMODE_BLOB);
    modes.emplace_back("100755", GIT_FILEMODE_BLOB_EXECUTABLE);
    const std::array names{"blob", "tree", "commit", "tag", "missing"};
    const CanonPath node("node");
    for (auto [modeString, readerKind] : modes)
        for (size_t actual = 0; actual < objects.size(); ++actual) {
            auto body = treeBody(modeString, objects[actual]);
            git_oid root;
            ASSERT_EQ(git_odb_write(&root, odb, body.data(), body.size(), GIT_OBJECT_TREE), 0);
            for (bool primeObject : {false, true}) {
                /* Priming parses existing objects; whether they stay in
                   libgit2's cache depends on its object-cache policy. */
                if (primeObject && actual == 4)
                    continue;
                for (bool entryCached : {false, true}) {
                    SCOPED_TRACE(
                        ::testing::Message()
                        << modeString << "/" << names[actual] << "/" << primeObject << "/" << entryCached);
                    auto repo = openRepo();
                    if (primeObject)
                        (void) repo->getAccessor(
                            Hash::parseAny(git_oid_tostr_s(&objects[actual]), HashAlgorithm::SHA1), {}, "prime");
                    auto acc =
                        repo->getAccessor(Hash::parseAny(git_oid_tostr_s(&root), HashAlgorithm::SHA1), {}, "types");
                    if (entryCached)
                        ASSERT_TRUE(acc->maybeLstat(node));
                    for (unsigned pass = 0; pass < 2; ++pass) {
                        SCOPED_TRACE(pass);
                        if (readerKind == GIT_FILEMODE_TREE && actual == 1) {
                            auto entries = acc->readDirectory(node);
                            ASSERT_EQ(entries.size(), 1u);
                            EXPECT_EQ(entries.at("node"), std::optional(SourceAccessor::tRegular));
                        } else if (readerKind == GIT_FILEMODE_COMMIT)
                            EXPECT_TRUE(acc->readDirectory(node).empty());
                        else
                            EXPECT_THROW(acc->readDirectory(node), Error);

                        if ((readerKind == GIT_FILEMODE_BLOB || readerKind == GIT_FILEMODE_BLOB_EXECUTABLE)
                            && actual == 0)
                            EXPECT_EQ(acc->readFile(node), "x");
                        else
                            EXPECT_THROW(acc->readFile(node), Error);

                        if (readerKind == GIT_FILEMODE_LINK && actual == 0)
                            EXPECT_EQ(acc->readLink(node), "x");
                        else
                            EXPECT_THROW(acc->readLink(node), Error);
                    }
                }
            }
        }
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

/* Git status describes tracked membership and changes under Git's comparison
   rules. It does not identify the filesystem objects a worktree accessor
   reads, so even paths reported clean retain the whole-view fingerprint. */
TEST_F(GitUtilsTest, workdirReadsAndNamesItsFilesystemView)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    Finally closeRepo{[&] { git_repository_free(rawRepo); }};
    write("clean/a", "a");
    write("dirty/c", "c");
    commitPaths(rawRepo, {"clean/a", "dirty/c"}, "initial");
    write("dirty/c", "changed");

    auto repo = openRepo();
    auto wd = repo->getWorkdirInfo();
    ASSERT_TRUE(wd.isDirty);
    ASSERT_THAT(wd.files, ::testing::ElementsAre(CanonPath("clean/a"), CanonPath("dirty/c")));
    unsigned int cleanStatus = 0, dirtyStatus = 0;
    ASSERT_EQ(git_status_file(&cleanStatus, rawRepo, "clean/a"), 0);
    ASSERT_EQ(git_status_file(&dirtyStatus, rawRepo, "dirty/c"), 0);
    EXPECT_EQ(cleanStatus, GIT_STATUS_CURRENT);
    EXPECT_EQ(dirtyStatus, GIT_STATUS_WT_MODIFIED);
    auto workdir = repo->getAccessor(
        make_ref<const GitRepo::WorkdirInfo>(wd), {}, [](const CanonPath &) { return RestrictedPathError("no"); });
    workdir->fingerprint = "whole";
    for (auto p : {"", "clean", "clean/a", "dirty", "dirty/c"}) {
        EXPECT_EQ(workdir->getFingerprint(CanonPath(p)), (std::pair{CanonPath(p), std::string("whole")}));
        EXPECT_FALSE(workdir->getKnownEntry(CanonPath(p)));
    }

    /* The membership snapshot does not turn the filesystem into HEAD. */
    write("clean/a", "A");
    write("dirty/c", "C");
    EXPECT_EQ(workdir->readFile(CanonPath("clean/a")), "A");
    EXPECT_EQ(workdir->readFile(CanonPath("dirty/c")), "C");
}

TEST(GitWorkdirInfo, bulkMembershipMatchesCanonicalSetsAndPrefixQueries)
{
    const std::array<const char *, 6> names{"a/child", "a!", "A", "a//child", "a\xc3\xa9", "a"};
    const std::vector<GitRepo::Submodule> modules{
        {CanonPath("module/deep"), "deep-url", "deep-branch"},
        {CanonPath("a"), "a-url", "a-branch"},
        {CanonPath("module"), "module-url", "module-branch"},
        {CanonPath("module"), "duplicate-url", "duplicate-branch"},
    };
    const std::array<const char *, 12> queries{
        "",
        "a",
        "a/child",
        "a/child/deeper",
        "a!",
        "A",
        "a\xc3\xa9",
        "a/absent",
        "module",
        "module/deep",
        "module/other",
        "missing"};
    auto prefixOf = [](const CanonPath & a, const CanonPath & b) {
        return a.isRoot() || a.abs() == b.abs() || b.abs().starts_with(a.abs() + "/");
    };
    std::array<size_t, 6> order{0, 1, 2, 3, 4, 5};
    size_t permutations = 0;
    do {
        /* Every arrival order and prefix length: empty sets, canonical
           aliases, duplicates, case, high bytes and slash/punctuation order. */
        for (size_t count = 0; count <= names.size(); ++count)
            for (bool root : {false, true}) {
                SCOPED_TRACE(fmt("permutation=%d count=%d root=%d", permutations, count, root));
                GitRepo::WorkdirInfo wd;
                wd.submodules = modules;
                decltype(wd.files)::sequence_type paths;
                paths.reserve(count + root);
                std::set<CanonPath> expected;
                if (root) {
                    paths.push_back(CanonPath::root);
                    expected.insert(CanonPath::root);
                }
                for (size_t i = 0; i < count; ++i) {
                    paths.emplace_back(names[order[i]]);
                    expected.emplace(names[order[i]]);
                }
                wd.files.adopt_sequence(std::move(paths));
                EXPECT_THAT(wd.files, ::testing::ElementsAreArray(expected));
                for (bool mount : {false, true}) {
                    auto view = wd;
                    auto members = expected;
                    if (mount) {
                        auto mountPaths = view.submodules | std::views::transform(&GitRepo::Submodule::path);
                        view.files.insert(mountPaths.begin(), mountPaths.end());
                        for (auto & module : modules)
                            members.insert(module.path);
                    }
                    EXPECT_THAT(view.files, ::testing::ElementsAreArray(members));
                    for (auto raw : queries) {
                        CanonPath q(raw);
                        bool admitted = std::any_of(members.begin(), members.end(), [&](const auto & p) {
                            return prefixOf(p, q) || prefixOf(q, p);
                        });
                        EXPECT_EQ(q.isAllowed(view.files), admitted) << raw;
                    }
                    EXPECT_THAT(wd.files, ::testing::ElementsAreArray(expected));
                    for (size_t i = 0; i < modules.size(); ++i) {
                        EXPECT_EQ(view.submodules[i].path, modules[i].path);
                        EXPECT_EQ(view.submodules[i].url, modules[i].url);
                        EXPECT_EQ(view.submodules[i].branch, modules[i].branch);
                    }
                }
            }
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));
    EXPECT_EQ(permutations, 720u);
}

TEST_F(GitUtilsTest, workdirMembershipKeepsConflictsAndExcludesBothDeletionKinds)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    Finally closeRepo{[&] { git_repository_free(rawRepo); }};
    for (auto path : {"A-file", "a!", "a/child", "gone", "staged-gone"})
        write(path, path);
    auto rev = commitPaths(rawRepo, {"A-file", "a!", "a/child", "gone", "staged-gone"}, "initial");
    git_index * index = nullptr;
    ASSERT_EQ(git_repository_index(&index, rawRepo), 0);
    Finally closeIndex{[&] { git_index_free(index); }};
    auto original = git_index_get_bypath(index, "a/child", 0);
    ASSERT_NE(original, nullptr);
    auto conflict = *original;
    conflict.path = "a/child"; // independent of index removal inside conflict_add
    ASSERT_EQ(git_index_conflict_add(index, &conflict, &conflict, &conflict), 0);
    deletePath(tmpDir / "gone");
    ASSERT_EQ(git_index_remove_bypath(index, "staged-gone"), 0);
    write("added", "added");
    ASSERT_EQ(git_index_add_bypath(index, "added"), 0);
    ASSERT_EQ(git_index_write(index), 0);
    git_config * config = nullptr;
    ASSERT_EQ(git_repository_config(&config, rawRepo), 0);
    Finally closeConfig{[&] { git_config_free(config); }};
    for (bool ignoreCase : {false, true}) {
        SCOPED_TRACE(ignoreCase);
        ASSERT_EQ(git_config_set_bool(config, "core.ignorecase", ignoreCase), 0);
        auto repo = openRepo();
        auto wd = repo->getWorkdirInfo();
        EXPECT_EQ(wd.headRev, rev);
        EXPECT_TRUE(wd.isDirty);
        EXPECT_THAT(
            wd.files,
            ::testing::ElementsAre(CanonPath("A-file"), CanonPath("a/child"), CanonPath("a!"), CanonPath("added")));
        auto accessor = repo->getAccessor(
            make_ref<const GitRepo::WorkdirInfo>(wd), {}, [](const CanonPath &) { return RestrictedPathError("no"); });
        write("A-file", ignoreCase ? "second live value" : "first live value");
        EXPECT_EQ(accessor->readFile(CanonPath("A-file")), ignoreCase ? "second live value" : "first live value");
        EXPECT_FALSE(accessor->pathExists(CanonPath("staged-gone")));
    }
}

TEST_F(GitUtilsTest, workdirDomainIsTheFixedTrackedPrefixesAndTheirAncestors)
{
    write("a/x/z", "plain");
    write("a/extra", "other");
    write("a!", "executable");
    std::filesystem::permissions(tmpDir / "a!", std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    createDirs(tmpDir / "ab");
    std::filesystem::create_symlink("../a/x/z", tmpDir / "ab/link");
    write("hidden", "untracked");

    const std::vector<CanonPath> domain{
        CanonPath::root,
        CanonPath("a"),
        CanonPath("a/x"),
        CanonPath("a/x/z"),
        CanonPath("a!"),
        CanonPath("ab/link"),
        CanonPath("missing")};
    const std::vector<CanonPath> queries{
        CanonPath::root,
        CanonPath("a"),
        CanonPath("a/x"),
        CanonPath("a/x/z"),
        CanonPath("a/extra"),
        CanonPath("a!"),
        CanonPath("ab"),
        CanonPath("ab/link"),
        CanonPath("missing"),
        CanonPath("missing/child"),
        CanonPath("hidden")};
    auto prefixOf = [](const CanonPath & a, const CanonPath & b) {
        return a.isRoot() || a.abs() == b.abs() || b.abs().starts_with(a.abs() + "/");
    };
    auto physical = makeFSSourceAccessor(tmpDir);
    auto repo = openRepo();
    for (unsigned subset = 0; subset < (1u << domain.size()); ++subset) {
        SCOPED_TRACE(subset);
        GitRepo::WorkdirInfo wd;
        decltype(wd.files)::sequence_type paths;
        for (size_t i = 0; i < domain.size(); ++i)
            if (subset & (1u << i))
                paths.push_back(domain[i]);
        wd.files.adopt_sequence(std::move(paths));
        auto admitted = [&](const CanonPath & q) {
            return q.isRoot() || std::any_of(wd.files.begin(), wd.files.end(), [&](const auto & p) {
                       return prefixOf(p, q) || prefixOf(q, p);
                   });
        };
        auto workdir = repo->getAccessor(make_ref<const GitRepo::WorkdirInfo>(wd), {}, [](const CanonPath &) {
            return RestrictedPathError("untracked");
        });
        EXPECT_EQ(workdir->allowsConcurrentTraversal(), physical->allowsConcurrentTraversal());
        for (const auto & q : queries) {
            SCOPED_TRACE(q.abs());
            auto expected = admitted(q) ? physical->maybeLstat(q) : std::nullopt;
            auto actual = workdir->maybeLstat(q);
            EXPECT_EQ(bool(actual), bool(expected));
            EXPECT_EQ(workdir->pathExists(q), bool(expected));
            EXPECT_FALSE(workdir->getFingerprint(q));
            EXPECT_FALSE(workdir->getKnownEntry(q));
            if (!admitted(q)) {
                EXPECT_THROW(workdir->lstat(q), RestrictedPathError);
            } else if (!expected) {
                EXPECT_THROW(workdir->lstat(q), Error);
            } else if (actual && expected) {
                EXPECT_EQ(actual->type, expected->type);
                EXPECT_EQ(actual->isExecutable, expected->isExecutable);
                if (expected->type == SourceAccessor::tRegular)
                    EXPECT_EQ(workdir->readFile(q), physical->readFile(q));
                else if (expected->type == SourceAccessor::tSymlink)
                    EXPECT_EQ(workdir->readLink(q), physical->readLink(q));
                else if (expected->type == SourceAccessor::tDirectory) {
                    SourceAccessor::DirEntries entries;
                    for (const auto & entry : physical->readDirectory(q))
                        if (admitted(q / entry.first))
                            entries.insert(entry);
                    EXPECT_EQ(workdir->readDirectory(q), entries);
                    workdir->readDirectoryWithEntries(
                        q,
                        [&](SourceAccessor & scoped,
                            const CanonPath & relative,
                            SourceAccessor::DirectoryListing && listing) {
                            const auto & observed = listing.entries();
                            EXPECT_EQ(observed, entries);
                            for (const auto & entry : observed)
                                EXPECT_EQ(
                                    scoped.lstat(relative / entry.first).type, physical->lstat(q / entry.first).type);
                        });
                }
            }
        }
        for (auto name : {"", "whole"}) {
            workdir->fingerprint = name;
            for (const auto & q : queries)
                EXPECT_EQ(workdir->getFingerprint(q), (std::pair{q, std::string(name)}));
        }
    }
}

TEST_F(GitUtilsTest, workdirTrackedPrefixStillAdmitsAReplacementDirectory)
{
    write("tracked", "before");
    GitRepo::WorkdirInfo wd;
    wd.files = {CanonPath("tracked")};
    auto workdir = openRepo()->getAccessor(make_ref<const GitRepo::WorkdirInfo>(wd), {}, [](const CanonPath &) {
        return RestrictedPathError("untracked");
    });
    EXPECT_EQ(workdir->readFile(CanonPath("tracked")), "before");
    EXPECT_FALSE(workdir->getFingerprint(CanonPath("tracked")));

    std::filesystem::remove(tmpDir / "tracked");
    write("tracked/sub/child", "after");
    write("untracked", "hidden");
    wd.files = {CanonPath("tracked"), CanonPath("untracked")};
    EXPECT_EQ(workdir->lstat(CanonPath("tracked")).type, SourceAccessor::tDirectory);
    EXPECT_EQ(workdir->readFile(CanonPath("tracked/sub/child")), "after");
    EXPECT_FALSE(workdir->getFingerprint(CanonPath("tracked/sub/child")));
    EXPECT_FALSE(workdir->pathExists(CanonPath("untracked")));
    EXPECT_THROW(workdir->readFile(CanonPath("untracked")), RestrictedPathError);
}

TEST_F(GitUtilsTest, cachedWorkdirSnapshotsStayImmutableAcrossRenderingAndInvalidation)
{
    nix::initLibStore(/*loadConfig=*/false);
    auto store = openStore("dummy://?read-only=false");
    Settings settings;
    settings.warnDirty = false;
    Finally clearCache{[] { GitRepo::invalidateWorkdirInfoCache(); }};
    for (bool submodule : {false, true})
        for (bool initiallyDirty : {false, true}) {
            SCOPED_TRACE(fmt("submodule=%d initiallyDirty=%d", submodule, initiallyDirty));
            auto repoPath = tmpDir / fmt("snapshot-%d-%d", submodule, initiallyDirty);
            GitRepo::openRepo(repoPath, {.create = true});
            git_repository * rawRepo = nullptr;
            ASSERT_EQ(git_repository_open(&rawRepo, repoPath.string().c_str()), 0);
            Finally closeRepo{[&] { git_repository_free(rawRepo); }};
            writeFile(repoPath / "file", "original");
            auto rev = commitPaths(rawRepo, {"file"}, "initial");
            if (submodule) {
                git_index * index = nullptr;
                ASSERT_EQ(git_repository_index(&index, rawRepo), 0);
                Finally closeIndex{[&] { git_index_free(index); }};
                git_index_entry entry{};
                entry.mode = GIT_FILEMODE_COMMIT;
                entry.path = "module";
                ASSERT_EQ(git_oid_fromstr(&entry.id, rev.gitRev().c_str()), 0);
                ASSERT_EQ(git_index_add(index, &entry), 0);
                ASSERT_EQ(git_index_write(index), 0);
                rev = commitPaths(rawRepo, {}, "gitlink");
            }
            if (initiallyDirty)
                writeFile(repoPath / "file", "changed bytes");

            GitRepo::invalidateWorkdirInfoCache();
            auto first = GitRepo::getCachedWorkdirInfo(repoPath);
            EXPECT_EQ(first, GitRepo::getCachedWorkdirInfo(repoPath));
            EXPECT_EQ(first->isDirty, initiallyDirty);
            EXPECT_EQ(first->headRev, rev);
            EXPECT_EQ(first->submodules.size(), submodule ? 1u : 0u);
            EXPECT_THAT(first->files, ::testing::ElementsAre(CanonPath("file")));

            auto input = Input::fromAttrs({
                {"type", "git"},
                {"url", repoPath.string()},
                {"shallow", Explicit{true}},
            });
            auto [accessor, resolved] = input.getAccessor(settings, *store);
            EXPECT_EQ(resolved.getRev().has_value(), !initiallyDirty);
            EXPECT_FALSE(resolved.getFingerprint(*store));
            if (submodule) {
                EXPECT_TRUE(accessor->pathExists(CanonPath("module")));
                EXPECT_TRUE(accessor->readDirectory(CanonPath("module")).empty());
            }
            /* Adding mount prefixes belongs to the rendered view, never
               to the cached status snapshot shared by another caller. */
            EXPECT_THAT(first->files, ::testing::ElementsAre(CanonPath("file")));
            EXPECT_EQ(first, GitRepo::getCachedWorkdirInfo(repoPath));

            auto nextContents = initiallyDirty ? "original" : "changed bytes";
            writeFile(repoPath / "file", nextContents);
            EXPECT_EQ(accessor->readFile(CanonPath("file")), nextContents);
            EXPECT_EQ(GitRepo::getCachedWorkdirInfo(repoPath)->isDirty, initiallyDirty);
            GitRepo::invalidateWorkdirInfoCache();
            auto second = GitRepo::getCachedWorkdirInfo(repoPath);
            EXPECT_NE(second, first);
            EXPECT_EQ(second, GitRepo::getCachedWorkdirInfo(repoPath));
            EXPECT_EQ(second->isDirty, !initiallyDirty);
            EXPECT_EQ(first->isDirty, initiallyDirty);
            EXPECT_EQ(first->files, second->files);
            EXPECT_EQ(first->headRev, second->headRev);
            auto [after, afterInput] = input.getAccessor(settings, *store);
            EXPECT_EQ(afterInput.getRev().has_value(), initiallyDirty);
            EXPECT_EQ(after->readFile(CanonPath("file")), nextContents);
        }
}

TEST_F(GitUtilsTest, submoduleDirtinessBelongsToTheRenderedWorkdir)
{
    nix::initLibStore(/*loadConfig=*/false);
    auto store = openStore("dummy://?read-only=false");
    Settings settings;
    settings.warnDirty = false;
    Finally clearCache{[] { GitRepo::invalidateWorkdirInfoCache(); }};
    auto subdir = tmpDir / "module";
    GitRepo::openRepo(subdir, {.create = true});
    git_repository * rawChild = nullptr;
    ASSERT_EQ(git_repository_open(&rawChild, subdir.string().c_str()), 0);
    Finally closeChild{[&] { git_repository_free(rawChild); }};
    writeFile(subdir / "file", "original");
    auto childRev = commitPaths(rawChild, {"file"}, "child");

    git_repository * rawParent = nullptr;
    ASSERT_EQ(git_repository_open(&rawParent, tmpDir.string().c_str()), 0);
    Finally closeParent{[&] { git_repository_free(rawParent); }};
    write("file", "original");
    write(".gitmodules", "[submodule \"module\"]\npath = module\nurl = ./module\n");
    git_index * index = nullptr;
    ASSERT_EQ(git_repository_index(&index, rawParent), 0);
    Finally closeIndex{[&] { git_index_free(index); }};
    git_index_entry entry{};
    entry.mode = GIT_FILEMODE_COMMIT;
    entry.path = "module";
    ASSERT_EQ(git_oid_fromstr(&entry.id, childRev.gitRev().c_str()), 0);
    ASSERT_EQ(git_index_add(index, &entry), 0);
    ASSERT_EQ(git_index_write(index), 0);
    commitPaths(rawParent, {"file", ".gitmodules"}, "parent");

    for (bool parentDirty : {false, true})
        for (bool childDirty : {false, true}) {
            write("file", parentDirty ? "changed parent" : "original");
            writeFile(subdir / "file", childDirty ? "changed child" : "original");
            GitRepo::invalidateWorkdirInfoCache();
            auto snapshot = GitRepo::getCachedWorkdirInfo(tmpDir);
            ASSERT_EQ(snapshot->isDirty, parentDirty);
            ASSERT_EQ(snapshot->submodules.size(), 1u);
            ASSERT_FALSE(snapshot->files.contains(CanonPath("module")));
            /* Render the enabled view first: its child's dirtiness must
               not change the snapshot used by the disabled view. */
            for (bool enabled : {true, false}) {
                SCOPED_TRACE(fmt("parent=%d child=%d enabled=%d", parentDirty, childDirty, enabled));
                auto input = Input::fromAttrs({
                    {"type", "git"},
                    {"url", tmpDir.string()},
                    {"shallow", Explicit{true}},
                    {"submodules", Explicit{enabled}},
                });
                auto [accessor, resolved] = input.getAccessor(settings, *store);
                EXPECT_EQ(resolved.getRev().has_value(), !(parentDirty || (enabled && childDirty)));
                EXPECT_FALSE(resolved.getFingerprint(*store));
                if (enabled) {
                    EXPECT_EQ(accessor->readFile(CanonPath("module/file")), childDirty ? "changed child" : "original");
                } else {
                    EXPECT_TRUE(accessor->readDirectory(CanonPath("module")).empty());
                }
                EXPECT_EQ(snapshot, GitRepo::getCachedWorkdirInfo(tmpDir));
                EXPECT_EQ(snapshot->isDirty, parentDirty);
                EXPECT_FALSE(snapshot->files.contains(CanonPath("module")));
            }
        }
}

TEST_F(GitUtilsTest, workdirIndexFlagsDoNotChangeItsContents)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    Finally closeRepo{[&] { git_repository_free(rawRepo); }};
    std::vector<std::string> names;
    for (unsigned flags = 0; flags < 4; ++flags)
        for (bool changed : {false, true})
            for (bool executable : {false, true}) {
                auto name = fmt("file-%d-%d-%d", flags, changed, executable);
                write(name, "before");
                names.push_back(std::move(name));
            }
    std::vector<const char *> paths;
    for (auto & name : names)
        paths.push_back(name.c_str());
    commitPaths(rawRepo, paths, "initial");

    git_index * index = nullptr;
    ASSERT_EQ(git_repository_index(&index, rawRepo), 0);
    Finally closeIndex{[&] { git_index_free(index); }};
    for (unsigned flags = 0; flags < 4; ++flags)
        for (bool changed : {false, true})
            for (bool executable : {false, true}) {
                auto name = fmt("file-%d-%d-%d", flags, changed, executable);
                auto original = git_index_get_bypath(index, name.c_str(), 0);
                ASSERT_NE(original, nullptr);
                auto entry = *original;
                if (flags & 1)
                    entry.flags |= GIT_INDEX_ENTRY_VALID;
                if (flags & 2) {
                    entry.flags |= GIT_INDEX_ENTRY_EXTENDED;
                    entry.flags_extended |= GIT_INDEX_ENTRY_SKIP_WORKTREE;
                }
                ASSERT_EQ(git_index_add(index, &entry), 0);
            }
    ASSERT_EQ(git_index_write(index), 0);

    auto repo = openRepo();
    /* One changed file per observation: the boolean must witness whether
       these index flags suppress comparison, independently of other cells. */
    for (unsigned flags = 0; flags < 4; ++flags)
        for (bool changed : {false, true})
            for (bool executable : {false, true}) {
                auto name = fmt("file-%d-%d-%d", flags, changed, executable);
                SCOPED_TRACE(name);
                auto path = CanonPath(name);
                if (changed)
                    write(name, "after!");
                if (executable)
                    std::filesystem::permissions(
                        tmpDir / name, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
                auto wd = repo->getWorkdirInfo();
                ASSERT_TRUE(wd.files.contains(path));
                EXPECT_EQ(wd.isDirty, !flags && (changed || executable));
                auto workdir = repo->getAccessor(make_ref<const GitRepo::WorkdirInfo>(wd), {}, [](const CanonPath &) {
                    return RestrictedPathError("no");
                });
                workdir->fingerprint = "whole";
                EXPECT_EQ(workdir->readFile(path), changed ? "after!" : "before");
                EXPECT_EQ(workdir->lstat(path).isExecutable, executable);
                EXPECT_EQ(workdir->getFingerprint(path), (std::pair{path, std::string("whole")}));
                write(name, "before");
                std::filesystem::permissions(
                    tmpDir / name, std::filesystem::perms::owner_exec, std::filesystem::perm_options::remove);
            }
}

TEST_F(GitUtilsTest, workdirAttributesFromTheIndexDoNotReplaceFilesystemBytes)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    Finally closeRepo{[&] { git_repository_free(rawRepo); }};
    /* Both per-directory locations, with and without a working-tree copy:
       libgit2's FILE_THEN_INDEX attribute lookup falls back to the index. */
    for (bool nested : {false, true}) {
        auto prefix = nested ? std::string("sub/") : std::string();
        auto attributes = prefix + ".gitattributes";
        auto file = prefix + "text";
        write(attributes, "* text eol=crlf\n");
        write(file, "line\r\n");
        auto rev = commitPaths(rawRepo, {attributes.c_str(), file.c_str()}, "attributes");
        auto head = openRepo()->getAccessor(rev, {}, "");
        ASSERT_EQ(head->readFile(CanonPath(file)), "line\n");
        for (bool present : {true, false}) {
            SCOPED_TRACE(fmt("nested=%d present=%d", nested, present));
            if (!present)
                std::filesystem::remove(tmpDir / attributes);
            auto repo = openRepo();
            auto wd = repo->getWorkdirInfo();
            ASSERT_TRUE(wd.files.contains(CanonPath(file)));
            EXPECT_EQ(wd.isDirty, !present);
            EXPECT_EQ(wd.files.contains(CanonPath(attributes)), present);
            unsigned int status = 0;
            ASSERT_EQ(git_status_file(&status, rawRepo, file.c_str()), 0);
            EXPECT_EQ(status, GIT_STATUS_CURRENT);
            auto workdir = repo->getAccessor(make_ref<const GitRepo::WorkdirInfo>(wd), {}, [](const CanonPath &) {
                return RestrictedPathError("no");
            });
            workdir->fingerprint = "whole";
            EXPECT_EQ(workdir->readFile(CanonPath(file)), "line\r\n");
            EXPECT_EQ(workdir->getFingerprint(CanonPath(file)), (std::pair{CanonPath(file), std::string("whole")}));
        }
        /* Restore the first directory's attributes before the next case. */
        write(attributes, "* text eol=crlf\n");
    }
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
    ASSERT_TRUE(acc->getFingerprint(CanonPath("x")));

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
        auto identity = acc->getFingerprint(leafPath);
        ASSERT_TRUE(identity) << leaf;
        const auto & [subpath, name] = *identity;
        auto row = settings.getCache()->lookup(makeSourcePathToHashCacheKey(name, git, subpath));
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
    auto wrapped = repo->getAccessor(rev1, {.exportIgnore = true}, "");
    /* The filter is left out, so the accessor is the inner one and the
       root's name is the tree's, as with `exportIgnore = false`. */
    wrapped->fingerprint = "whole";
    for (auto p : {"", "clean", "clean/a", "top"})
        EXPECT_EQ(wrapped->getFingerprint(CanonPath(p)), raw->getFingerprint(CanonPath(p))) << p;

    /* Construction proves elision for every caller, not only fetchers. */
    auto another = repo->getAccessor(rev1, {.exportIgnore = true}, "");
    EXPECT_EQ(another->getFingerprint(CanonPath("clean")), raw->getFingerprint(CanonPath("clean")));

    /* The working directory, dirty in one file, under the same wrapper. */
    write("dirty/c", "changed");
    auto wd = repo->getWorkdirInfo();
    ASSERT_TRUE(wd.isDirty);
    auto workdir =
        repo->getAccessor(make_ref<const GitRepo::WorkdirInfo>(wd), {.exportIgnore = true}, [](const CanonPath & path) {
            return RestrictedPathError("no");
        });
    workdir->fingerprint = "whole";
    EXPECT_EQ(workdir->getFingerprint(CanonPath("clean/a")), (std::pair{CanonPath("clean/a"), std::string("whole")}));
    EXPECT_EQ(workdir->getFingerprint(CanonPath("dirty")), (std::pair{CanonPath("dirty"), std::string("whole")}));

    /* A committed `.gitattributes` naming export-ignore, anywhere in the tree, takes the names away. */
    write("clean/.gitattributes", "top export-ignore\n");
    write("dirty/c", "c");
    auto rev2 = commit({"clean/.gitattributes", "dirty/c"}, "two");
    auto repo2 = openRepo();
    auto wrapped2 = repo2->getAccessor(rev2, {.exportIgnore = true}, "");
    wrapped2->fingerprint = "whole";
    EXPECT_EQ(wrapped2->getFingerprint(CanonPath("clean/a")), (std::pair{CanonPath("clean/a"), std::string("whole")}));
    auto wd2 = repo2->getWorkdirInfo();
    auto workdir2 = repo2->getAccessor(
        make_ref<const GitRepo::WorkdirInfo>(wd2), {.exportIgnore = true}, [](const CanonPath & path) {
            return RestrictedPathError("no");
        });
    workdir2->fingerprint = "whole";
    EXPECT_EQ(workdir2->getFingerprint(CanonPath("clean/a")), (std::pair{CanonPath("clean/a"), std::string("whole")}));

    git_repository_free(rawRepo);
}

/* A commit with no export-ignore bytes needs no filter. A working tree
   retains the native lookup and renders the same files in this case. */
TEST_F(GitUtilsTest, exportIgnoreFilterIsLeftOutWhenNoSourceNamesIt)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    write("sub/a", "a");
    write("top", "t");
    /* Unrelated attributes permit the conservative absence proof. */
    write(".gitattributes", "# unrelated attributes\n*.lock linguist-generated\n* text=auto eol=lf\n");
    auto rev = commitPaths(rawRepo, {"sub/a", "top", ".gitattributes"}, "one");
    git_repository_free(rawRepo);

    auto repo = openRepo();
    auto raw = repo->getAccessor(rev, {}, "");
    auto filtered = repo->getAccessor(rev, {.exportIgnore = true}, "");
    EXPECT_TRUE(typeid(*filtered) == typeid(*raw)) << typeid(*filtered).name();
    EXPECT_EQ(filtered->readDirectory(CanonPath::root), raw->readDirectory(CanonPath::root));

    auto wd = repo->getWorkdirInfo();
    auto notAllowed = [](const CanonPath & path) { return RestrictedPathError("no"); };
    auto workdir = repo->getAccessor(make_ref<const GitRepo::WorkdirInfo>(wd), {.exportIgnore = true}, notAllowed);
    auto rawWorkdir = repo->getAccessor(make_ref<const GitRepo::WorkdirInfo>(wd), {}, notAllowed);
    EXPECT_FALSE(workdir->isFixed());
    EXPECT_FALSE(rawWorkdir->isFixed());
    EXPECT_FALSE(typeid(*workdir) == typeid(*rawWorkdir));
    EXPECT_EQ(workdir->readDirectory(CanonPath::root), rawWorkdir->readDirectory(CanonPath::root));
    EXPECT_EQ(workdir->readFile(CanonPath("sub/a")), rawWorkdir->readFile(CanonPath("sub/a")));

    /* The same tree, the same name and path. */
    nix::initLibStore(/*loadConfig=*/false);
    auto store = openStore("dummy://?read-only=false");
    fetchers::Settings settings;
    auto [pRaw, hRaw] = fetchToStore2(settings, *store, SourcePath(raw), FetchMode::DryRun, "src");
    auto [pFiltered, hFiltered] = fetchToStore2(settings, *store, SourcePath(filtered), FetchMode::DryRun, "src");
    EXPECT_EQ(hFiltered, hRaw);
    EXPECT_EQ(pFiltered, pRaw);
}

TEST_F(GitUtilsTest, exportIgnoreLiteralInACommentConservativelyKeepsTheFilter)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    Finally closeRepo = [&]() { git_repository_free(rawRepo); };
    write("keep", "k");
    write(".gitattributes", "# export-ignore\n* text=auto\n");
    auto rev = commitPaths(rawRepo, {"keep", ".gitattributes"}, "comment only");
    auto repo = openRepo();
    auto raw = repo->getAccessor(rev, {}, "");
    auto filtered = repo->getAccessor(rev, {.exportIgnore = true}, "");
    EXPECT_FALSE(typeid(*filtered) == typeid(*raw));
    EXPECT_EQ(filtered->readDirectory(CanonPath::root), raw->readDirectory(CanonPath::root));
    EXPECT_EQ(filtered->readFile(CanonPath("keep")), raw->readFile(CanonPath("keep")));
}

TEST_F(GitUtilsTest, exportIgnoreDoesNotReuseTheOldAttributeParserMemo)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    Finally closeRepo = [&]() { git_repository_free(rawRepo); };
    write("hide", "h");
    write("keep", "k");
    std::string attributes = "hide export-ignore";
    attributes += '\0';
    attributes += "junk\n";
    write(".gitattributes", attributes);
    auto rev = commitPaths(rawRepo, {"hide", "keep", ".gitattributes"}, "NUL assignment terminator");
    /* Avoid an external root macro source so only the immutable proof is tested. */
    std::filesystem::remove(tmpDir / ".gitattributes");
    Settings settings;
    auto cache = settings.getCache();
    Cache::Key oldKey{"treeMentionsAttribute", {{"rev", rev.gitRev()}, {"attribute", "export-ignore"}}};
    cache->upsert(oldKey, {{"mentions", Explicit<bool>{false}}});
    auto repo = openRepo();
    auto filtered = repo->getAccessor(rev, {.exportIgnore = true}, "", cache);
    EXPECT_FALSE(filtered->pathExists(CanonPath("hide")));
    EXPECT_EQ(filtered->readFile(CanonPath("keep")), "k");
    Cache::Key newKey{"treeContainsAttribute", {{"rev", rev.gitRev()}, {"attribute", "export-ignore"}}};
    auto memo = cache->lookup(newKey);
    ASSERT_TRUE(memo);
    EXPECT_TRUE(getBoolAttr(*memo, "mentions"));
}

TEST_F(GitUtilsTest, exportFilterConstructionRechecksExternalSourcesAfterAnImmutableMemoHit)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    write("keep", "visible");
    write("hide", "conditionally visible");
    auto rev = commitPaths(rawRepo, {"keep", "hide"}, "without attributes");
    auto attributes = std::filesystem::path(git_repository_path(rawRepo)) / "info" / "attributes";
    git_repository_free(rawRepo);
    Settings settings;
    auto cache = settings.getCache();
    auto repo = openRepo();
    auto raw = repo->getAccessor(rev, {}, "");
    auto first = repo->getAccessor(rev, {.exportIgnore = true}, "", cache);
    EXPECT_TRUE(raw->isFixed());
    EXPECT_TRUE(first->isFixed());
    EXPECT_EQ(first->getFingerprint(CanonPath::root), raw->getFingerprint(CanonPath::root));
    Cache::Key key{"treeContainsAttribute", {{"rev", rev.gitRev()}, {"attribute", "export-ignore"}}};
    auto memo = cache->lookup(key);
    ASSERT_TRUE(memo);
    EXPECT_FALSE(getBoolAttr(*memo, "mentions"));

    createDirs(attributes.parent_path());
    writeFile(attributes, "hide export-ignore\n");
    auto filtered = repo->getAccessor(rev, {.exportIgnore = true}, "", cache);
    EXPECT_FALSE(filtered->isFixed());
    EXPECT_FALSE(filtered->pathExists(CanonPath("hide")));
    EXPECT_EQ(filtered->readFile(CanonPath("keep")), "visible");
    EXPECT_FALSE(filtered->getFingerprint(CanonPath::root));
    std::filesystem::remove(attributes);
    auto again = repo->getAccessor(rev, {.exportIgnore = true}, "", cache);
    EXPECT_TRUE(again->isFixed());
    EXPECT_EQ(again->readFile(CanonPath("hide")), "conditionally visible");
    EXPECT_EQ(again->getFingerprint(CanonPath::root), raw->getFingerprint(CanonPath::root));
}

/* A repository naming `export-ignore` in a source the *commit's* lookup
   consults keeps the filter, which hides what libgit2 hides.  The sources
   of a commit, each alone: `info/attributes`; the user's file on the XDG
   search path; a macro in the system's file, whose rules
   `GIT_ATTR_CHECK_NO_SYSTEM` excludes but whose macros load;
   `core.attributesFile` with `~/`; a nested `.gitattributes` in the commit;
   CRLF line endings; and `-export-ignore`, beside a rule and alone.  The
   sources the commit's lookup does *not* consult -- the working directory's
   files below the root, and the index -- are the separate
   `committedRevIgnoresTheWorkdirsUncommittedGitattributes`; the working
   directory accessor's own set is `workdirExportIgnoreSources`. */
TEST_F(GitUtilsTest, exportIgnoreDeclaredAnywhereKeepsTheFilter)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    write("hide", "h");
    write("keep", "k");
    write("sub/hide2", "h");
    write("sub/keep2", "k");
    auto rev0 = commitPaths(rawRepo, {"hide", "keep", "sub/hide2", "sub/keep2"}, "one");

    /* Observe constructor elision separately from the rendered tree. */
    auto fetch = [&](const Hash & rev) {
        auto repo = openRepo();
        auto raw = repo->getAccessor(rev, {}, "");
        auto filtered = repo->getAccessor(rev, {.exportIgnore = true}, "");
        return std::pair{typeid(*filtered) == typeid(*raw), filtered};
    };
    auto expectFiltered = [&](const Hash & rev, const char * hidden, const char * kept, const char * source) {
        auto [filterOmitted, acc] = fetch(rev);
        EXPECT_FALSE(filterOmitted) << source;
        EXPECT_FALSE(acc->pathExists(CanonPath(hidden))) << source << ": " << hidden;
        EXPECT_TRUE(acc->pathExists(CanonPath(kept))) << source << ": " << kept;
    };
    auto gitDir = std::filesystem::path(git_repository_path(rawRepo));
    auto scratch = gitDir / "test-attribute-sources";
    createDirs(gitDir / "info");

    /* info/attributes */
    writeFile(gitDir / "info" / "attributes", "hide export-ignore\n");
    expectFiltered(rev0, "hide", "keep", "info/attributes");
    std::filesystem::remove(gitDir / "info" / "attributes");

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

    /* Configuration selection is not general get_path parsing: null/empty
       inhibit XDG; relative paths use the workdir; only "~/" expands. */
    auto configFile = gitDir / "config";
    auto originalConfig = readFile(configFile);
    Finally restoreConfig = [&]() { writeFile(configFile, originalConfig); };
    auto configure = [&](std::optional<std::string> value, bool nullValue = false) {
        writeFile(configFile, originalConfig + (nullValue ? "\n[core]\nattributesFile\n" : ""));
        if (value) {
            git_config * config = nullptr;
            ASSERT_EQ(git_config_open_ondisk(&config, configFile.string().c_str()), 0);
            ASSERT_EQ(git_config_set_string(config, "core.attributesfile", value->c_str()), 0);
            git_config_free(config);
        }
    };
    auto expectSelection = [&](bool mentions, bool hidden) {
        auto [filterOmitted, acc] = fetch(rev0);
        EXPECT_EQ(!filterOmitted, mentions);
        EXPECT_EQ(!acc->pathExists(CanonPath("hide")), hidden);
        EXPECT_EQ(acc->readFile(CanonPath("keep")), "k");
        if (!mentions)
            EXPECT_TRUE(acc->getFingerprint(CanonPath::root));
    };
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_XDG, xdg.string().c_str()), 0);
    configure(std::nullopt);
    expectSelection(true, true);
    configure(std::nullopt, true);
    expectSelection(false, false);
    configure("");
    expectSelection(false, false);
    write("attributes-relative", "hide export-ignore\n");
    write("~", "hide export-ignore\n");
    write("~other/attrs", "hide export-ignore\n");
    for (auto value : {"attributes-relative", "~", "~other/attrs"}) {
        SCOPED_TRACE(value);
        configure(value);
        expectSelection(true, true);
    }
    write("attributes-clean", "# no rules\n");
    for (auto value : {"attributes-clean", "missing-attributes"}) {
        SCOPED_TRACE(value);
        configure(value);
        expectSelection(false, false);
    }
#ifndef _WIN32
    /* This proof deliberately declines DOS/UNC/backslash spellings;
       native Windows paths remain accepted by the retained libgit2 view. */
    writeFile(home / "attrs", "# no rules\n");
    for (auto & value : std::vector<std::string>{(home / "attrs").string(), "~/attrs"}) {
        configure(value);
        expectSelection(false, false);
    }
#endif
    for (auto value : {"x:/attrs", "\\attrs", "//server/attrs"}) {
        configure(value);
        auto [filterOmitted, acc] = fetch(rev0);
        EXPECT_FALSE(filterOmitted);
        EXPECT_FALSE(acc->getFingerprint(CanonPath::root));
    }
    configure("~/attrs");
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_HOMEDIR, (home / "missing").string().c_str()), 0);
    auto [knownAbsent, unknownView] = fetch(rev0);
    EXPECT_FALSE(knownAbsent);
    EXPECT_FALSE(unknownView->getFingerprint(CanonPath::root));
    writeFile(configFile, originalConfig);
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_XDG, nullptr), 0);
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_HOMEDIR, oldHome.ptr), 0);
    git_buf_dispose(&oldHome);

    /* A nested .gitattributes in the commit. */
    write("sub/.gitattributes", "hide2 export-ignore\n");
    auto rev1 = commitPaths(rawRepo, {"sub/.gitattributes"}, "two");
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
    auto [filterOmitted, acc] = fetch(rev4);
    EXPECT_FALSE(filterOmitted);
    EXPECT_TRUE(acc->pathExists(CanonPath("hide")));

    git_repository_free(rawRepo);
}

TEST_F(GitUtilsTest, exportIgnoreSearchPathProofDeclinesUnsharedSyntax)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    Finally closeRepo = [&]() { git_repository_free(rawRepo); };
    write("keep", "k");
    auto rev = commitPaths(rawRepo, {"keep"}, "search-path domain");
    auto scratch = std::filesystem::path(git_repository_path(rawRepo)) / "search-proof";
    createDirs(scratch / "first");
    createDirs(scratch / "second");
    git_buf oldXdg = GIT_BUF_INIT, oldSystem = GIT_BUF_INIT;
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_XDG, &oldXdg), 0);
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, GIT_CONFIG_LEVEL_SYSTEM, &oldSystem), 0);
    Finally restore = [&]() {
        git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_XDG, oldXdg.ptr);
        git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_SYSTEM, oldSystem.ptr);
        git_buf_dispose(&oldXdg);
        git_buf_dispose(&oldSystem);
    };
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_XDG, ""), 0);
    ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, GIT_CONFIG_LEVEL_SYSTEM, ""), 0);
    for (auto level : {GIT_CONFIG_LEVEL_XDG, GIT_CONFIG_LEVEL_SYSTEM}) {
        auto first = (scratch / "first").generic_string();
        auto second = (scratch / "second").generic_string();
        for (auto [search, unknown] : std::vector<std::pair<std::string, bool>>{
                 {"", false},
                 {first, false},
                 {first + GIT_PATH_LIST_SEPARATOR + second, false},
                 {"relative-attributes", true},
                 {first + "\\" + GIT_PATH_LIST_SEPARATOR + second, true}}) {
            SCOPED_TRACE(search);
            ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, level, search.c_str()), 0);
            auto repo = openRepo();
            auto raw = repo->getAccessor(rev, {}, "");
            auto filtered = repo->getAccessor(rev, {.exportIgnore = true}, "");
            EXPECT_EQ(typeid(*filtered) != typeid(*raw), unknown);
            EXPECT_EQ(filtered->readFile(CanonPath("keep")), "k");
        }
        ASSERT_EQ(git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, level, ""), 0);
    }
}

TEST_F(GitUtilsTest, exportIgnoreAbsenceProofAgreesWithEveryAssignmentTerminatorByte)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    write("hide", "h");
    write("keep", "k");
    auto rev = commitPaths(rawRepo, {"hide", "keep"}, "attribute byte domain");
    auto attributes = std::filesystem::path(git_repository_path(rawRepo)) / "info" / "attributes";
    git_repository_free(rawRepo);
    createDirs(attributes.parent_path());

    unsigned cases = 0;
    for (const char * prefix : {"", "-", "!"}) {
        SCOPED_TRACE(prefix);
        for (unsigned byte = 0; byte != 256; ++byte) {
            SCOPED_TRACE(byte);
            std::string contents = "hide ";
            contents += prefix;
            contents += "export-ignore";
            contents += static_cast<char>(byte);
            contents += "suffix\n";
            writeFile(attributes, contents);

            /* Both the raw index and this commit have no .gitattributes.
               Their native source sets therefore agree for this fixture.
               Fresh handles avoid reusing attribute rules between cells. */
            git_repository * native = nullptr;
            ASSERT_EQ(git_repository_open(&native, tmpDir.string().c_str()), 0);
            Finally closeNative = [&]() { git_repository_free(native); };
            const char * value = nullptr;
            ASSERT_EQ(
                git_attr_get(
                    &value, native, GIT_ATTR_CHECK_INDEX_ONLY | GIT_ATTR_CHECK_NO_SYSTEM, "hide", "export-ignore"),
                0);
            bool hidden = GIT_ATTR_IS_TRUE(value);
            if (*prefix == '\0' && byte == 0)
                EXPECT_TRUE(hidden); // NUL ends the native assignment before the suffix.

            auto repo = openRepo();
            auto view = repo->getAccessor(rev, {.exportIgnore = true}, "");
            bool visible = view->pathExists(CanonPath("hide"));
            EXPECT_EQ(visible, !hidden);
            if (visible)
                EXPECT_EQ(view->readFile(CanonPath("hide")), "h");
            EXPECT_EQ(view->readFile(CanonPath("keep")), "k");
            ++cases;
        }
    }
    EXPECT_EQ(cases, 768u);
}

/* The tree a commit accessor names is the commit's, so the attributes that
   decide the export-ignore filter are the commit's own `.gitattributes`,
   `info/attributes` and the files outside any tree -- never the working
   directory's `.gitattributes` and never the index's.  This is what `git
   archive <rev>` does: it unpacks the tree being archived into its in-core
   index and reads attributes from the index alone (git `archive.c`,
   `unpack_trees` with `index_only` then
   `git_attr_set_direction(GIT_ATTR_INDEX)`), and `--worktree-attributes` is
   what asks for the checkout's files instead ("attributes are by default
   taken from the `.gitattributes` files in the tree that is being
   archived", `git-archive(1)`).

   Two halves.  An untracked `.gitattributes` in every directory of the tree
   below the root: the answer must be no for all of them, which is the
   observable form of "the decision reads no working-directory file" -- the
   decision before this change stopped at the first one it stat'd, one
   `stat` per tree directory.  Then a staged one, removed from the working
   directory: the index is not the tree named either.

   Before this change, and on master (`git show
   8aad447f0:src/libfetchers/git-utils.cc`, the same two calls), the
   commit's lookup passed `GIT_ATTR_CHECK_INCLUDE_COMMIT |
   GIT_ATTR_CHECK_NO_SYSTEM`, whose low bits are `GIT_ATTR_CHECK_FILE_THEN_INDEX`
   (0), so libgit2 read the working directory's file and then the index's
   entry *before* the commit's blob, and both halves hid the file. */
TEST_F(GitUtilsTest, committedRevIgnoresTheWorkdirsUncommittedGitattributes)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    write("sub/hide2", "h");
    write("sub/keep2", "k");
    write("sub/deep/hide3", "h");
    write("sub/deep/keep3", "k");
    auto rev = commitPaths(rawRepo, {"sub/hide2", "sub/keep2", "sub/deep/hide3", "sub/deep/keep3"}, "one");

    /* Observe constructor elision separately from the forced native view. */
    auto fetch = [&](const Hash & r) {
        auto repo = openRepo();
        auto raw = repo->getAccessor(r, {}, "");
        auto filtered = repo->getAccessor(r, {.exportIgnore = true}, "");
        return std::pair{typeid(*filtered) == typeid(*raw), filtered};
    };
    /* A harmless attribute source requires a real filter at construction.
       Remove it before querying: the lookup must still ignore uncommitted
       per-directory attributes, independently of the elision fast path. */
    auto forced = [&](const Hash & r) {
        auto attributes = std::filesystem::path(git_repository_path(rawRepo)) / "info" / "attributes";
        createDirs(attributes.parent_path());
        writeFile(attributes, "__absent_filter_probe__ export-ignore\n");
        auto accessor = openRepo()->getAccessor(r, {.exportIgnore = true}, "");
        std::filesystem::remove(attributes);
        return accessor;
    };
    auto expectKept = [&](const char * source) {
        auto [filterOmitted, acc] = fetch(rev);
        EXPECT_TRUE(filterOmitted) << source;
        for (auto p : {"sub/hide2", "sub/keep2", "sub/deep/hide3", "sub/deep/keep3"})
            EXPECT_TRUE(acc->pathExists(CanonPath(p))) << source << ": " << p;
        auto filtered = forced(rev);
        for (auto p : {"sub/hide2", "sub/deep/hide3"})
            EXPECT_TRUE(filtered->pathExists(CanonPath(p))) << source << ", filter forced on: " << p;
    };

    /* Untracked, in every directory of the tree below the root. */
    write("sub/.gitattributes", "hide2 export-ignore\n");
    write("sub/deep/.gitattributes", "hide3 export-ignore\n");
    expectKept("workdir-only .gitattributes");
    std::filesystem::remove(tmpDir / "sub" / ".gitattributes");
    std::filesystem::remove(tmpDir / "sub" / "deep" / ".gitattributes");

    /* Staged and then taken out of the working directory: the index alone. */
    auto stage = [&](const char * rel, bool add) {
        git_index * index = nullptr;
        ASSERT_EQ(git_repository_index(&index, rawRepo), 0);
        ASSERT_EQ(add ? git_index_add_bypath(index, rel) : git_index_remove_bypath(index, rel), 0);
        ASSERT_EQ(git_index_write(index), 0);
        git_index_free(index);
    };
    write("sub/.gitattributes", "hide2 export-ignore\n");
    write("sub/deep/.gitattributes", "hide3 export-ignore\n");
    stage("sub/.gitattributes", true);
    stage("sub/deep/.gitattributes", true);
    std::filesystem::remove(tmpDir / "sub" / ".gitattributes");
    std::filesystem::remove(tmpDir / "sub" / "deep" / ".gitattributes");
    expectKept("index-only .gitattributes");
    stage("sub/.gitattributes", false);
    stage("sub/deep/.gitattributes", false);

    git_repository_free(rawRepo);
}

/* `$GIT_DIR/info/attributes` is a source of the commit's tree, not of the
   working directory: "you can keep necessary attributes that should apply
   while archiving any tree in your `$GIT_DIR/info/attributes` file"
   (`git-archive(1)`), and libgit2 pushes it for every lookup whatever the
   flags (`attr.c`, `collect_attr_files`).  Unchanged behaviour, pinned
   beside the working directory's withdrawal above. */
TEST_F(GitUtilsTest, committedRevHonoursInfoAttributes)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    write("hide", "h");
    write("keep", "k");
    auto rev = commitPaths(rawRepo, {"hide", "keep"}, "one");
    auto gitDir = std::filesystem::path(git_repository_path(rawRepo));
    createDirs(gitDir / "info");
    writeFile(gitDir / "info" / "attributes", "hide export-ignore\n");
    git_repository_free(rawRepo);

    auto repo = openRepo();
    auto acc = repo->getAccessor(rev, {.exportIgnore = true}, "");
    EXPECT_FALSE(acc->pathExists(CanonPath("hide")));
    EXPECT_TRUE(acc->pathExists(CanonPath("keep")));
}

/* The working-directory accessor names the working tree, and asks libgit2
   with `GIT_ATTR_CHECK_INDEX_ONLY | GIT_ATTR_CHECK_NO_SYSTEM`: its
   per-directory source is the index's `.gitattributes`, and the files
   outside any tree are pushed as before.  Unchanged by the commit route's
   change, and pinned here: `info/attributes` and an index entry each keep
   the filter and hide what libgit2 hides; a `.gitattributes` present only
   in the working directory below the root does not reach the lookup, which
   is libgit2's answer under those flags (`01` section 13 records the
   question). */
TEST_F(GitUtilsTest, workdirExportIgnoreSources)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    write("sub/hide2", "h");
    write("sub/keep2", "k");
    commitPaths(rawRepo, {"sub/hide2", "sub/keep2"}, "one");
    auto gitDir = std::filesystem::path(git_repository_path(rawRepo));
    createDirs(gitDir / "info");
    auto stage = [&](const char * rel, bool add) {
        git_index * index = nullptr;
        ASSERT_EQ(git_repository_index(&index, rawRepo), 0);
        ASSERT_EQ(add ? git_index_add_bypath(index, rel) : git_index_remove_bypath(index, rel), 0);
        ASSERT_EQ(git_index_write(index), 0);
        git_index_free(index);
    };
    auto notAllowed = [](const CanonPath & path) { return RestrictedPathError("no"); };
    auto workdirAccessor = [&]() {
        auto repo = openRepo();
        auto wd = repo->getWorkdirInfo();
        return repo->getAccessor(make_ref<const GitRepo::WorkdirInfo>(wd), {.exportIgnore = true}, notAllowed);
    };

    /* info/attributes. */
    writeFile(gitDir / "info" / "attributes", "hide2 export-ignore\n");
    {
        auto acc = workdirAccessor();
        EXPECT_FALSE(acc->pathExists(CanonPath("sub/hide2"))) << "info/attributes";
        EXPECT_TRUE(acc->pathExists(CanonPath("sub/keep2"))) << "info/attributes";
    }
    std::filesystem::remove(gitDir / "info" / "attributes");

    /* Staged, and taken out of the working directory: the index's entry. */
    write("sub/.gitattributes", "hide2 export-ignore\n");
    stage("sub/.gitattributes", true);
    std::filesystem::remove(tmpDir / "sub" / ".gitattributes");
    {
        auto acc = workdirAccessor();
        EXPECT_FALSE(acc->pathExists(CanonPath("sub/hide2"))) << "index .gitattributes";
        EXPECT_TRUE(acc->pathExists(CanonPath("sub/keep2"))) << "index .gitattributes";
    }
    stage("sub/.gitattributes", false);

    /* Only in the working directory, below the root: not a source of this
       lookup, so the filter is the identity and both names stand. */
    write("sub/.gitattributes", "hide2 export-ignore\n");
    {
        auto acc = workdirAccessor();
        EXPECT_TRUE(acc->pathExists(CanonPath("sub/hide2"))) << "workdir-only .gitattributes";
    }
    std::filesystem::remove(tmpDir / "sub" / ".gitattributes");

    git_repository_free(rawRepo);
}

TEST_F(GitUtilsTest, sharedRepositoryAttributeReadersAgreeAfterRefresh)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    Finally closeRepo{[&] { git_repository_free(rawRepo); }};
    std::vector<std::string> paths;
    /* Four applicable views, two independent wrappers per view, and two
       concurrent readers per wrapper. Distinct paths also miss the outer
       filtering cache after the external attribute file changes. */
    constexpr size_t readers = 16;
    for (size_t phase = 0; phase < 2; ++phase)
        for (size_t reader = 0; reader < readers; ++reader) {
            auto dir = "phase" + std::to_string(phase) + "/reader" + std::to_string(reader) + "/";
            for (auto [name, contents] :
                 {std::pair{".gitattributes", "hide hidden\nkeep filter=lfs\n"},
                  std::pair{"hide", "hidden contents"},
                  std::pair{"keep", "ordinary file contents"}}) {
                paths.push_back(dir + name);
                write(paths.back(), contents);
            }
        }
    std::vector<const char *> staged;
    for (auto & path : paths)
        staged.push_back(path.c_str());
    auto rev = commitPaths(rawRepo, staged, "attribute readers");
    auto attributes = tmpDir / ".git/info/attributes";
    createDirs(attributes.parent_path());
    writeFile(attributes, "[attr]hidden export-ignore\n");
    /* Obtain membership on a separate handle so the shared handle's
       attribute cache is still cold when the concurrent readers start. */
    auto wd = openRepo()->getWorkdirInfo();
    auto repo = openRepo();
    repo->setRemote("origin", "https://example.invalid/attribute-readers");
    std::vector<std::pair<ref<SourceAccessor>, bool>> views;
    for (size_t copy = 0; copy < 2; ++copy) {
        views.emplace_back(
            repo->getAccessor(
                make_ref<const GitRepo::WorkdirInfo>(wd),
                {.exportIgnore = true},
                [](const CanonPath &) { return RestrictedPathError("no"); }),
            true);
        views.emplace_back(repo->getAccessor(rev, {.exportIgnore = true}, ""), true);
        views.emplace_back(repo->getAccessor(rev, {.smudgeLfs = true}, ""), false);
        views.emplace_back(repo->getAccessor(rev, {.exportIgnore = true, .smudgeLfs = true}, ""), true);
    }
    for (size_t phase = 0; phase < 2; ++phase) {
        for (const auto & view : views) {
            EXPECT_FALSE(view.first->isFixed());
        }
        SCOPED_TRACE(phase);
        if (phase)
            writeFile(attributes, "[attr]hidden export-ignore\n# changed stamp, same rules\n");
        std::latch start{1};
        using Observation = std::tuple<std::set<std::string>, std::string, bool>;
        std::vector<std::future<Observation>> pending;
        for (size_t reader = 0; reader < readers; ++reader) {
            auto accessor = views.at(reader / 2).first;
            auto path = CanonPath("phase" + std::to_string(phase) + "/reader" + std::to_string(reader));
            pending.push_back(std::async(std::launch::async, [&, accessor, path] {
                start.wait();
                Observation result;
                accessor->readDirectoryWithEntries(
                    path,
                    [&](SourceAccessor & scoped,
                        const CanonPath & relative,
                        SourceAccessor::DirectoryListing && listing) {
                        const auto & entries = listing.entries();
                        for (auto & [name, type] : entries)
                            std::get<0>(result).insert(name);
                        /* The LFS attribute is true but these are ordinary
                           bytes, so pointer parsing performs no download. */
                        std::get<1>(result) = scoped.readFile(relative / "keep");
                        std::get<2>(result) = scoped.pathExists(relative / "hide");
                    });
                return result;
            }));
        }
        auto status = std::async(std::launch::async, [&] {
            start.wait();
            return repo->getWorkdirInfo();
        });
        start.count_down();
        for (size_t reader = 0; reader < readers; ++reader) {
            SCOPED_TRACE(reader);
            auto [entries, contents, hasHidden] = pending.at(reader).get();
            auto filtered = views.at(reader / 2).second;
            auto expected = std::set<std::string>{".gitattributes", "keep"};
            if (!filtered)
                expected.insert("hide");
            EXPECT_EQ(entries, expected);
            EXPECT_EQ(contents, "ordinary file contents");
            EXPECT_EQ(hasHidden, !filtered);
        }
        EXPECT_FALSE(status.get().isDirty);
    }
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

/* These accessors already own the complete immutable bytes. Their stream
   contract concerns bytes, callback order and failure, not chunk boundaries. */
TEST_F(GitUtilsTest, immutableReadsPreserveBytesFailuresAndCallbackLifetime)
{
    git_repository * rawRepo = nullptr;
    ASSERT_EQ(git_repository_open(&rawRepo, tmpDir.string().c_str()), 0);
    Finally closeRepo{[&] { git_repository_free(rawRepo); }};
    auto repo = openRepo();
    repo->setRemote("origin", "https://example.invalid/immutable-byte-delivery");
    const CanonPath file("file");
    enum class Producer { Nar, Git, GitLfsUnselected, Captured };
    enum class Release { Never, Size, Sink };
    enum class Failure { None, SizeError, SizeEof, SinkError, SinkEof };

    struct ConsumerError : Error
    {
        using Error::Error;
    };

    size_t cases = 0;

    for (size_t length : {0u, 1u, 8191u, 8192u, 8193u, 24577u}) {
        std::string contents(length, '\0');
        for (size_t i = 0; i < length; ++i)
            contents[i] = static_cast<char>(i % 256);
        write("file", contents);
        auto rev = commitPaths(rawRepo, {"file"}, "immutable byte delivery");
        auto memory = make_ref<MemorySourceAccessor>();
        memory->addFile(file, std::string(contents));
        StringSink nar;
        memory->dumpPath(CanonPath::root, nar);

        for (auto producer : {Producer::Nar, Producer::Git, Producer::GitLfsUnselected, Producer::Captured})
            for (auto release : {Release::Never, Release::Size, Release::Sink})
                for (auto failure :
                     {Failure::None, Failure::SizeError, Failure::SizeEof, Failure::SinkError, Failure::SinkEof}) {
                    SCOPED_TRACE(
                        ::testing::Message() << "length=" << length << " producer=" << int(producer)
                                             << " release=" << int(release) << " failure=" << int(failure));
                    std::shared_ptr<SourceAccessor> accessor =
                        producer == Producer::Nar
                            ? makeNarAccessor(std::string(nar.s)).get_ptr()
                            : producer == Producer::Captured
                                ? GitRepo::acquire(SourcePath{memory}).get_ptr()
                                : repo->getAccessor(rev, {.smudgeLfs = producer == Producer::GitLfsUnselected}, "")
                                      .get_ptr();
                    std::weak_ptr<SourceAccessor> weak = accessor;
                    bool released = false, sawSize = false;
                    size_t sinkCalls = 0;
                    std::string received;
                    auto releaseAt = [&](Release at) {
                        if (release != at || released)
                            return;
                        /* A negative control without the guard must fail before
                           destroying a Git accessor whose mutex is still held. */
                        if (weak.use_count() < 2)
                            throw std::logic_error("reader has no owner across its callback");
                        accessor.reset();
                        released = true;
                    };
                    LambdaSink sink([&](std::string_view bytes) {
                        ++sinkCalls;
                        EXPECT_TRUE(sawSize);
                        EXPECT_FALSE(bytes.empty());
                        releaseAt(Release::Sink);
                        if (failure == Failure::SinkError)
                            throw ConsumerError("sink callback failure");
                        if (failure == Failure::SinkEof)
                            throw EndOfFile("sink callback failure");
                        received.append(bytes);
                    });
                    auto read = [&] {
                        accessor->readFile(file, sink, [&](uint64_t size) {
                            EXPECT_FALSE(sawSize);
                            sawSize = true;
                            EXPECT_EQ(size, contents.size());
                            releaseAt(Release::Size);
                            if (failure == Failure::SizeError)
                                throw ConsumerError("size callback failure");
                            if (failure == Failure::SizeEof)
                                throw EndOfFile("size callback failure");
                        });
                    };
                    bool sizeFailure = failure == Failure::SizeError || failure == Failure::SizeEof;
                    bool sinkFailure = length && (failure == Failure::SinkError || failure == Failure::SinkEof);
                    if (failure == Failure::SizeError || (length && failure == Failure::SinkError))
                        EXPECT_THROW(read(), ConsumerError);
                    else if (failure == Failure::SizeEof || (length && failure == Failure::SinkEof))
                        EXPECT_THROW(read(), EndOfFile);
                    else
                        EXPECT_NO_THROW(read());
                    EXPECT_TRUE(sawSize);
                    EXPECT_EQ(sinkCalls == 0, sizeFailure || !length);
                    EXPECT_EQ(received, sizeFailure || sinkFailure ? std::string{} : contents);
                    EXPECT_EQ(weak.expired(), released);
                    ++cases;
                }
    }
    EXPECT_EQ(cases, 360u);
    RecordProperty("finite_cases", cases);

    auto pool = openWriterPool();
    auto links = pool->makeDirectorySink();
    links->insertChild("empty-link", pool->makeSymlink(""));
    links->insertChild("link", pool->makeSymlink("target"));
    auto tree = std::move(*links).finalize();
    pool->flush();
    auto accessor = repo->getAccessor(tree, {}, "");
    EXPECT_EQ(accessor->readLink(CanonPath("empty-link")), "");
    EXPECT_EQ(accessor->readLink(CanonPath("link")), "target");
}

TEST_F(GitUtilsTest, immutableBlobCallbacksMayReadTheSameAccessor)
{
    size_t cases = 0;
    for (auto algorithm : {HashAlgorithm::SHA1, HashAlgorithm::SHA256}) {
        GitRepo::Options options{.create = true, .bare = true, .oidType = algorithm};
        auto path = tmpDir / printHashAlgo(algorithm);
        auto repo = GitRepo::openRepo(path, options);
        auto pool = GitRepoPool::create(path, options);
        auto blob = writeString(*pool, "owned bytes");
        auto tree = pool->makeDirectorySink();
        tree->insertChild("regular", blob);
        tree->insertChild("executable", {merkle::Mode::Executable, blob.hash});
        tree->insertChild("link", pool->makeSymlink("target"));
        auto root = std::move(*tree).finalize();
        pool->flush();
        for (auto node : {"/", "/regular", "/executable"})
            for (bool fromSize : {false, true}) {
                SCOPED_TRACE(::testing::Message() << printHashAlgo(algorithm) << node << " size=" << fromSize);
                CanonPath file(node);
                auto accessor = repo->getAccessor(file.isRoot() ? blob.hash : root, {}, "");
                bool reentered = false, sawSize = false;
                auto reenter = [&] {
                    EXPECT_FALSE(reentered);
                    reentered = true;
                    EXPECT_EQ(accessor->lstat(file).type, SourceAccessor::tRegular);
                    EXPECT_EQ(accessor->readFile(file), "owned bytes");
                    if (!file.isRoot())
                        EXPECT_EQ(accessor->readLink(CanonPath("link")), "target");
                };
                std::string received;
                LambdaSink sink([&](std::string_view bytes) {
                    EXPECT_TRUE(sawSize);
                    if (!fromSize)
                        reenter();
                    received.append(bytes);
                });
                accessor->readFile(file, sink, [&](uint64_t size) {
                    sawSize = true;
                    EXPECT_EQ(size, 11u);
                    if (fromSize)
                        reenter();
                });
                EXPECT_TRUE(reentered);
                EXPECT_EQ(received, "owned bytes");
                ++cases;
            }
    }
    EXPECT_EQ(cases, 12u);
    RecordProperty("finite_cases", cases);
}

TEST_F(GitUtilsTest, acquiredTreeRejectsSuppressedDirectoryFailures)
{
    enum class History { Escape, Return, Retry, Zero, Once, Twice, SuccessThenReturn };
    enum class Shape { Root, NestedLast, NestedWithSibling };
    struct Source : MemorySourceAccessor
    {
        History history;
        CanonPath target;
        unsigned callbacks = 0, reads = 0, siblingReads = 0;
        Source(History history, CanonPath target) : history(history), target(std::move(target)) {}

        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            if (path == CanonPath("/z")) {
                ++siblingReads;
                MemorySourceAccessor::readFile(path, sink, std::move(sizeCallback));
                return;
            }
            ++reads;
            if (reads == 2 && history == History::SuccessThenReturn)
                throw Error("directory child producer failure");
            if (reads == 1 && (history == History::Escape || history == History::Return || history == History::Retry))
                throw Error("directory child producer failure");
            MemorySourceAccessor::readFile(path, sink, std::move(sizeCallback));
        }

        void readDirectoryWithEntries(const CanonPath & path, ReadDirectoryCallback callback) override
        {
            if (path != target) {
                MemorySourceAccessor::readDirectoryWithEntries(path, callback);
                return;
            }
            if (history == History::Zero)
                return;
            auto invoke = [&] {
                ++callbacks;
                callback(*this, path, DirectoryListing(readDirectory(path)));
            };
            try { invoke(); }
            catch (Error &) {
                if (history == History::Escape)
                    throw;
                if (history == History::Return)
                    return;
                if (history != History::Retry)
                    throw;
            }
            if (history == History::Twice)
                addFile(target / "payload", "BBB");
            if (history == History::Retry || history == History::Twice || history == History::SuccessThenReturn) {
                try { invoke(); }
                catch (Error &) {
                    if (history != History::SuccessThenReturn)
                        throw;
                }
            }
        }
    };

    unsigned rejected = 0, completed = 0;
    for (auto shape : {Shape::Root, Shape::NestedLast, Shape::NestedWithSibling})
        for (auto history : {History::Escape, History::Return, History::Retry, History::Zero, History::Once, History::Twice, History::SuccessThenReturn}) {
            SCOPED_TRACE(fmt("shape=%d history=%d", int(shape), int(history)));
            auto target = shape == Shape::Root ? CanonPath::root : CanonPath("/nested");
            auto file = target / "payload";
            auto source = make_ref<Source>(history, target);
            source->addFile(file, "AAA");
            if (shape == Shape::NestedWithSibling)
                source->addFile(CanonPath("/z"), "late sibling");
            if (history == History::Once || history == History::Twice) {
                auto acquired = GitRepo::acquire(SourcePath{source});
                auto expected = history == History::Twice ? "BBB" : "AAA";
                EXPECT_EQ(acquired->readFile(file), expected);
                auto reference = make_ref<MemorySourceAccessor>();
                reference->addFile(file, expected);
                if (shape == Shape::NestedWithSibling)
                    reference->addFile(CanonPath("/z"), "late sibling");
                EXPECT_EQ(acquired->getKnownEntry(CanonPath::root),
                          std::optional{objectHashOf(*reference, CanonPath::root).root});
                ++completed;
            } else {
                try {
                    GitRepo::acquire(SourcePath{source});
                    ADD_FAILURE() << "failed or absent directory callback produced a root";
                } catch (Error & error) {
                    auto message = filterANSIEscapes(error.what(), true);
                    EXPECT_THAT(message, ::testing::HasSubstr(
                        history == History::Return || history == History::Zero
                            ? "directory accessor did not produce a traversal result"
                            : "directory child producer failure"));
                    ++rejected;
                }
            }
            auto count = history == History::Zero ? 0u : history == History::Retry || history == History::Twice || history == History::SuccessThenReturn ? 2u : 1u;
            EXPECT_EQ(source->callbacks, count);
            EXPECT_EQ(source->reads, history == History::Retry ? 1u : count);
            EXPECT_EQ(source->siblingReads, shape == Shape::NestedWithSibling
                && (history == History::Once || history == History::Twice) ? 1u : 0u);
        }
    EXPECT_EQ(rejected, 15u);
    EXPECT_EQ(completed, 6u);
    RecordProperty("finite_cells", rejected + completed);
}

/* Owned acquisition keeps bytes in its private mempack and never needs
   a temporary blob file, even if the source obstructs objects while writing. */
#ifndef _WIN32
TEST_F(GitUtilsTest, acquiredTreeDoesNotSpillToTheObjectsDirectory)
{
    struct Source : MemorySourceAccessor
    {
        std::filesystem::path objects;
        bool obstruct = true;
        explicit Source(std::filesystem::path objects) : objects(std::move(objects)) {}
        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            if (!obstruct) {
                MemorySourceAccessor::readFile(path, sink, std::move(sizeCallback));
                return;
            }
            auto saved = objects;
            saved += ".saved";
            std::filesystem::rename(objects, saved);
            Finally restore{[&] {
                std::filesystem::remove(objects);
                std::filesystem::rename(saved, objects);
            }};
            writeFile(objects, "a file obstructing the stream directory");
            MemorySourceAccessor::readFile(path, sink, std::move(sizeCallback));
        }
    };
    for (bool directory : {false, true}) {
        SCOPED_TRACE(fmt("directory=%d", directory));
        auto path = tmpDir / fmt("acquire-unspilled-%d", directory);
        auto source = make_ref<Source>(path / "objects");
        createDirs(path / "objects");
        auto file = directory ? CanonPath("/payload") : CanonPath::root;
        auto bytes = std::string(1024 * 1024 + 1, 'x');
        bytes[bytes.size() / 2] = '\0';
        source->addFile(file, std::string(bytes));
        auto unspilled = GitRepo::acquire(SourcePath{source});
        EXPECT_TRUE(std::filesystem::is_directory(path / "objects"));
        EXPECT_FALSE(std::filesystem::exists(path / "objects.saved"));
        source->obstruct = false;
        auto expected = objectHashOf(*source, CanonPath::root).root;
        auto restored = GitRepo::acquire(SourcePath{source});
        source->root.reset();
        for (auto acquired : {unspilled, restored}) {
            EXPECT_EQ(acquired->getKnownEntry(CanonPath::root), std::optional{expected});
            EXPECT_EQ(acquired->readFile(file), bytes);
            EXPECT_EQ(acquired->lstat(CanonPath::root).type,
                      directory ? SourceAccessor::tDirectory : SourceAccessor::tRegular);
            EXPECT_FALSE(acquired->lstat(file).isExecutable);
        }
    }
    RecordProperty("unspilled_shapes", 2u);
    RecordProperty("restored_shapes", 2u);
}

TEST_F(GitUtilsTest, publicRegularSinkRetainsRealStreamFailures)
{
    auto path = tmpDir / "public-stream-failure";
    auto repo = GitRepo::openRepo(path, {.create = true, .bare = true, .packfilesOnly = true,
        .oidType = HashAlgorithm::SHA256, .canonicalObjects = true});
    auto objects = path / "objects", saved = path / "objects.saved";
    std::filesystem::rename(objects, saved);
    Finally restore{[&] { std::filesystem::remove(objects); std::filesystem::rename(saved, objects); }};
    writeFile(objects, "a file obstructing the stream directory");
    auto sink = repo->makeRegularFileSink();
    EXPECT_THAT(
        [&] { (*sink)(std::string(1024 * 1024 + 1, 'x')); },
        ::testing::ThrowsMessage<Error>(::testing::AllOf(
            testing::HasSubstrIgnoreANSIMatcher("creating blob stream"),
            testing::HasSubstrIgnoreANSIMatcher("Not a directory"),
            testing::HasSubstrIgnoreANSIMatcher("objects"))));
}
#endif


/* The writer observes completed raw bytes. ODB setup and owner phase may
   change placement, never the framed identity or the readable contents. */
TEST_F(GitUtilsTest, rawBlobBoundariesPreserveIdentityAndPublication)
{
    constexpr size_t boundary = 1 << 20;
    size_t cases = 0, writes = 0, aborts = 0;
    for (auto options : std::array{
             GitRepo::Options{.oidType = HashAlgorithm::SHA1},
             GitRepo::Options{.oidType = HashAlgorithm::SHA256},
             GitRepo::Options{.oidType = HashAlgorithm::SHA256, .canonicalObjects = true}})
        for (bool packOnly : {false, true})
            for (unsigned phase : {0u, 1u, 2u})
                for (size_t size : {size_t{0}, boundary, boundary + 1})
                    for (unsigned chunks : {0u, 1u, 2u}) {
                        SCOPED_TRACE(::testing::Message()
                                     << printHashAlgo(options.oidType) << " canonical=" << options.canonicalObjects
                                     << " packOnly=" << packOnly << " phase=" << phase
                                     << " size=" << size << " chunks=" << chunks);
                        options.create = options.bare = options.dontFindDeltas = true;
                        options.packfilesOnly = packOnly;
                        auto path = tmpDir / fmt("raw-blob-%d", cases++);
                        AutoDelete cleanup(path, true);
                        auto repo = GitRepo::openRepo(path, options);
                        auto existing = options;
                        existing.create = false;
                        existing.oidType = options.oidType == HashAlgorithm::SHA1
                                               ? HashAlgorithm::SHA256 : HashAlgorithm::SHA1;
                        repo = GitRepo::openRepo(path, existing);
                        std::optional<ref<GitRepoPool>> pool;
                        if (phase)
                            pool.emplace(GitRepoPool::create(path, existing));
                        auto make = [&] {
                            if (phase == 2)
                                std::move(*(*pool)->makeDirectorySink()).finalize();
                            return pool ? (*pool)->makeRegularFileSink() : repo->makeRegularFileSink();
                        };
                        auto flush = [&] {
                            if (pool)
                                (*pool)->flush();
                            else
                                repo->flush();
                        };
                        auto expected = [&](std::string_view bytes) {
                            HashSink hash(options.oidType);
                            merkle::feedHeader(hash, "blob", bytes.size());
                            hash(bytes);
                            return hash.finish().hash;
                        };
                        auto write = [&](std::string_view bytes) {
                            auto file = make();
                            if (chunks == 0) {
                                if (!bytes.empty())
                                    (*file)(bytes);
                            } else if (chunks == 1) {
                                (*file)({});
                                for (size_t offset = 0; offset < bytes.size(); offset += 65535)
                                    (*file)(bytes.substr(offset, std::min(size_t{65535}, bytes.size() - offset)));
                                (*file)({});
                            } else {
                                auto split = std::min(boundary, bytes.size());
                                (*file)(bytes.substr(0, split));
                                (*file)({});
                                (*file)(bytes.substr(split));
                            }
                            ++writes;
                            return std::move(*file).finalize();
                        };
                        auto read = [&](GitRepo & from, const Hash & id, std::string_view bytes) {
                            auto accessor = from.getAccessor(id, {}, "raw-blob");
                            auto st = accessor->lstat(CanonPath::root);
                            EXPECT_EQ(st.type, SourceAccessor::tRegular);
                            EXPECT_FALSE(st.isExecutable);
                            EXPECT_EQ(accessor->readFile(CanonPath::root), bytes);
                            if (options.canonicalObjects)
                                EXPECT_EQ(accessor->getKnownEntry(CanonPath::root),
                                          (merkle::TreeEntry{merkle::Mode::Regular, id}));
                            else
                                EXPECT_FALSE(accessor->getKnownEntry(CanonPath::root));
                        };
                        std::string bytes(size, 'a');
                        if (size)
                            bytes[size / 2] = '\0';
                        auto id = expected(bytes);
                        EXPECT_EQ(write(bytes), id);
                        EXPECT_EQ(id.algo, options.oidType);
                        if (pool)
                            EXPECT_FALSE(repo->hasObject(id)) << "a fresh pool blob is not published before flush";
                        else
                            read(*repo, id, bytes);
                        EXPECT_EQ(write(bytes), id); // Existing object in the writer's mempack.
                        flush();
                        auto reopened = GitRepo::openRepo(path, existing);
                        read(*reopened, id, bytes);
                        EXPECT_EQ(write(bytes), id); // Existing object in the lower pack backend.
                        flush();
                        read(*GitRepo::openRepo(path, existing), id, bytes);

                        if (chunks == 0 && size != boundary) {
                            std::string abandoned(size ? size : 1, '!');
                            auto abandonedId = expected(abandoned);
                            EXPECT_THROW(([&] {
                                auto file = make();
                                (*file)(abandoned);
                                throw std::runtime_error("producer failed after delivered bytes");
                            })(), std::runtime_error);
                            EXPECT_FALSE(repo->hasObject(abandonedId));
                            auto healthy = expected("healthy control");
                            auto control = pool ? (*pool)->makeRegularFileSink() : repo->makeRegularFileSink();
                            (*control)("healthy control");
                            ++writes;
                            EXPECT_EQ(std::move(*control).finalize(), healthy);
                            control.reset();
                            flush();
                            auto afterAbort = GitRepo::openRepo(path, existing);
                            EXPECT_FALSE(afterAbort->hasObject(abandonedId));
                            read(*afterAbort, healthy, "healthy control");
                            ++aborts;
                        }
                    }
    EXPECT_EQ(cases, 162u);
    EXPECT_EQ(writes, 522u);
    EXPECT_EQ(aborts, 36u);
    RecordProperty("finite_inputs", cases);
    RecordProperty("finalizations", writes);
    RecordProperty("producer_aborts", aborts);
}

TEST_F(GitUtilsTest, symlinkBlobBytesPreserveIdentityAndRejectNul)
{
    size_t cases = 0, rejected = 0;
    for (auto options : std::array{
             GitRepo::Options{.oidType = HashAlgorithm::SHA1},
             GitRepo::Options{.oidType = HashAlgorithm::SHA256},
             GitRepo::Options{.oidType = HashAlgorithm::SHA256, .canonicalObjects = true}})
        for (bool packOnly : {false, true})
            for (bool dependent : {false, true}) {
                options.create = options.bare = options.dontFindDeltas = true;
                options.packfilesOnly = packOnly;
                auto path = tmpDir / fmt("symlink-blob-%d", rejected);
                AutoDelete cleanup(path, true);
                auto repo = GitRepo::openRepo(path, options);
                auto pool = GitRepoPool::create(path, options);
                if (dependent)
                    std::move(*pool->makeDirectorySink()).finalize();
                EXPECT_THROW(pool->makeSymlink(std::string("a\0b", 3)), Error);
                ++rejected;
                std::vector<merkle::TreeEntry> links;
                for (auto target : {std::string{}, std::string{"target"}, std::string((1 << 20) + 1, 'l')}) {
                    SCOPED_TRACE(::testing::Message() << printHashAlgo(options.oidType)
                                 << " packOnly=" << packOnly << " dependent=" << dependent
                                 << " targetSize=" << target.size());
                    auto link = pool->makeSymlink(target);
                    EXPECT_EQ(link.mode, merkle::Mode::Symlink);
                    EXPECT_EQ(link.hash.algo, options.oidType);
                    EXPECT_EQ(link.hash, writeString(*pool, target).hash);
                    links.push_back(link);
                    ++cases;
                }
                auto tree = pool->makeDirectorySink();
                for (size_t i = 0; i < links.size(); ++i)
                    tree->insertChild(fmt("link-%d", cases - links.size() + i), links[i]);
                auto root = std::move(*tree).finalize();
                pool->flush();
                auto accessor = GitRepo::openRepo(path, options)->getAccessor(root, {}, "symlink-blob");
                auto first = cases - 3;
                for (auto target : {std::string{}, std::string{"target"}, std::string((1 << 20) + 1, 'l')}) {
                    auto name = CanonPath(fmt("link-%d", first++));
                    EXPECT_EQ(accessor->lstat(name).type, SourceAccessor::tSymlink);
                    EXPECT_EQ(accessor->readLink(name), target);
                }
            }
    EXPECT_EQ(cases, 36u);
    EXPECT_EQ(rejected, 12u);
    RecordProperty("symlink_cells", cases);
    RecordProperty("nul_rejections", rejected);
}

TEST_F(GitUtilsTest, acquiredRootModesRetainTheRenderedValue)
{
    using File = MemorySourceAccessor::File;
    enum class Repository { Missing, SHA1, SHA256 };
    unsigned cases = 0;
    for (auto repository : {Repository::Missing, Repository::SHA1, Repository::SHA256})
        for (auto mode : {merkle::Mode::Directory, merkle::Mode::Regular, merkle::Mode::Executable, merkle::Mode::Symlink}) {
            SCOPED_TRACE(fmt("repository=%d mode=%d", int(repository), int(mode)));
            auto source = make_ref<MemorySourceAccessor>();
            EXPECT_FALSE(source->isFixed());
            if (mode == merkle::Mode::Directory)
                source->addFile(CanonPath("/payload"), "target");
            else if (mode == merkle::Mode::Symlink)
                source->root = File{File::Symlink{"target"}};
            else
                source->root = File{File::Regular{mode == merkle::Mode::Executable, "target"}};
            auto expected = objectHashOf(*source, CanonPath::root).root;
            auto stat = source->lstat(CanonPath::root);
            StringSink before;
            source->dumpPath(CanonPath::root, before);
            auto repoPath = getCacheDir() / "tarball-cache-v4";
            if (pathExists(repoPath))
                deletePath(repoPath);
            if (repository != Repository::Missing)
                GitRepo::openRepo(repoPath, {.create = true, .bare = true, .packfilesOnly = true,
                    .oidType = repository == Repository::SHA1 ? HashAlgorithm::SHA1 : HashAlgorithm::SHA256});
            auto acquired = GitRepo::acquire(SourcePath{source});
            EXPECT_EQ(pathExists(repoPath), repository != Repository::Missing);
            source->root.reset();
            EXPECT_TRUE(acquired->isFixed());
            EXPECT_EQ(acquired->getKnownEntry(CanonPath::root), std::optional{expected});
            auto observed = acquired->lstat(CanonPath::root);
            EXPECT_EQ(observed.type, stat.type);
            EXPECT_EQ(observed.isExecutable, stat.isExecutable);
            if (mode == merkle::Mode::Symlink)
                EXPECT_EQ(acquired->readLink(CanonPath::root), "target");
            else
                EXPECT_EQ(acquired->readFile(mode == merkle::Mode::Directory ? CanonPath("/payload") : CanonPath::root), "target");
            StringSink after;
            acquired->dumpPath(CanonPath::root, after);
            EXPECT_EQ(after.s, before.s);
            ++cases;
        }
    EXPECT_EQ(cases, 12u);
    RecordProperty("root_modes", cases);
    RecordProperty("ignored_cache_states", 3u);
}

/* Acquisition keeps its producer's diagnostics. Paths display as the
   producer's did, so a filesystem root has no trailing slash. With
   `explainMisses`, a lookup of a path the value lacks raises the producer's
   own error, such as a worktree's "not tracked by Git" hint; the producer
   still never answers a read. */
TEST_F(GitUtilsTest, acquiredTreeKeepsProducerDiagnostics)
{
    auto dir = tmpDir / "acquire-diagnostics";
    createDirs(dir);
    writeFile(dir / "tracked", "a");
    writeFile(dir / "untracked", "b");
    auto fs = makeFSSourceAccessor(dir);
    auto memory = make_ref<MemorySourceAccessor>();
    memory->addFile(CanonPath("/tracked"), "a");
    memory->setPathDisplay("«memory»");
    for (auto display : {fs, ref<SourceAccessor>(memory)}) {
        auto producer = AllowListSourceAccessor::create(
            display, {}, {CanonPath::root, CanonPath("/tracked")}, [](const CanonPath & path) {
                return RestrictedPathError("Path '%s' is not tracked by Git.", path.rel());
            });
        for (bool explain : {false, true}) {
            SCOPED_TRACE(fmt("producer=%s explain=%d", producer->showPath(CanonPath::root), explain));
            auto acquired = GitRepo::acquire(SourcePath{producer}, explain);
            for (auto path : {CanonPath::root, CanonPath("/tracked"), CanonPath("/missing/below")})
                EXPECT_EQ(acquired->showPath(path), producer->showPath(path));
            EXPECT_EQ(acquired->readFile(CanonPath("/tracked")), "a");
            for (auto path : {CanonPath("/untracked"), CanonPath("/missing"), CanonPath("/tracked/below")}) {
                EXPECT_FALSE(acquired->pathExists(path));
                EXPECT_EQ(acquired->maybeLstat(path), std::nullopt);
                if (explain) {
                    EXPECT_THAT(
                        [&] { acquired->lstat(path); },
                        ::testing::ThrowsMessage<RestrictedPathError>(
                            testing::HasSubstrIgnoreANSIMatcher(fmt("Path '%s' is not tracked by Git.", path.rel()))));
                    EXPECT_THROW(acquired->readFile(path), RestrictedPathError);
                    EXPECT_THROW(acquired->readDirectory(path), RestrictedPathError);
                } else {
                    EXPECT_THAT(
                        [&] { acquired->lstat(path); },
                        ::testing::ThrowsMessage<FileNotFound>(testing::HasSubstrIgnoreANSIMatcher(
                            fmt("path '%s' does not exist", producer->showPath(path)))));
                    EXPECT_THROW(acquired->readFile(path), FileNotFound);
                    EXPECT_THROW(acquired->readDirectory(path), FileNotFound);
                }
            }
        }
    }
    /* The posix root displays as the directory itself. */
    EXPECT_EQ(GitRepo::acquire(SourcePath{fs})->showPath(CanonPath::root), fs->showPath(CanonPath::root));
    EXPECT_FALSE(fs->showPath(CanonPath::root).ends_with('/'));
}

} // namespace nix::fetchers
