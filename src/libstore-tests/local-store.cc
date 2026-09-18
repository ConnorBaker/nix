#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <rapidcheck/gtest.h>

#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/store/sqlite.hh"
#include "nix/store/globals.hh"
#include "nix/store/derivations.hh"
#include "nix/store/gc-store.hh"
#include "nix/store/tests/temp-local-store.hh"

#include "nix/util/archive.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/object-hash.hh"
#include "nix/util/finally.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/file-system.hh"
#include "nix/store/git-object-store.hh"
#include "nix/store/content-address.hh"
#include "nix/util/tests/setting-scopes.hh"
#include "nix/util/tests/source-accessor-gen.hh"
#include "nix/util/signature/local-keys.hh"
#include "nix/util/signature/signer.hh"
// Needed for template specialisations. This is not good! When we
// overhaul how store configs work, this should be fixed.
#include "nix/util/args.hh"
#include "nix/util/config-impl.hh"
#include "nix/util/abstract-setting-to-json.hh"

#include <sys/stat.h>
#include <cerrno>
#include <cstring>

namespace nix {

namespace {
/* `CanonicalizePathMetadataOptions::ignoredAcls` is a reference where ACLs
   are supported (Linux), so a brace-empty options value is ill-formed there;
   the tests canonicalise with nothing ignored. */
[[maybe_unused]] const StringSet noAclsToIgnore;
} // namespace

TEST(LocalStore, storeDir_absolutePath)
{
    std::filesystem::path storeDir =
#ifdef _WIN32
        "C:\\";
#else
        "/";
#endif
    storeDir /= "nix";
    storeDir /= "store";
    LocalStoreConfig config{"", {{"store", storeDir.string()}}};
    EXPECT_EQ(config.storeDir, storeDir.string());
}

TEST(LocalStore, storeDir_relativePath_rejected)
{
    EXPECT_THROW(LocalStoreConfig("", {{"store", (std::filesystem::path{"nix"} / "store").string()}}), UsageError);
}

TEST(LocalStore, storeDir_empty_rejected)
{
    EXPECT_THROW(LocalStoreConfig("", {{"store", ""}}), UsageError);
}

TEST(LocalStore, constructConfig_rootQueryParam)
{
#ifdef _WIN32
    constexpr std::string_view root = "C:\\foo\\bar";
#else
    constexpr std::string_view root = "/foo/bar";
#endif
    LocalStoreConfig config{
        "",
        {
            {
                "root",
                std::string{root},
            },
        },
    };

    EXPECT_EQ(config.rootDir.get(), std::optional<AbsolutePath>{std::string{root}});
}

TEST(LocalStore, constructConfig_rootPath)
{
#ifdef _WIN32
    constexpr std::string_view root = "C:\\foo\\bar";
#else
    constexpr std::string_view root = "/foo/bar";
#endif
    LocalStoreConfig config{std::string{root}, {}};

    EXPECT_EQ(config.rootDir.get(), std::optional<AbsolutePath>{std::string{root}});
}

TEST(LocalStore, constructConfig_to_string)
{
    LocalStoreConfig config{"", {}};
    EXPECT_EQ(config.getReference().to_string(), "local");
}

#ifndef _WIN32 /* Windows doesn't exactly have a notion of canonicalation for now. */

class LocalStoreCanonicalisationTest : public TempLocalStoreTest,
                                       public ::testing::WithParamInterface<std::tuple<HashAlgorithm>>
{
protected:
    void assertCanonicalPermissions(const std::filesystem::path & path)
    {
        auto st = lstat(path);

        /* TODO: Figure out how to test xattrs properly. Maybe with posix ACLs?
           But xattrs are unavailable in the nix sandbox, so that would have to
           also run in NixOS tests? */

        auto modeTypeMaskedOut = st.st_mode & ~S_IFMT;

        /* Store mtime is always 1 sec into the epoch. */
        ASSERT_EQ(st.st_mtime, 1);

        if (S_ISLNK(st.st_mode)) {
            /* Not much to check for symlinks. */
        } else if (S_ISREG(st.st_mode)) {
            ASSERT_TRUE(modeTypeMaskedOut == 0555 || modeTypeMaskedOut == 0444);
        } else if (S_ISDIR(st.st_mode)) {
            ASSERT_EQ(modeTypeMaskedOut, 0555);
            for (const auto & p : DirectoryIterator{path})
                assertCanonicalPermissions(p.path());
        } else {
            FAIL() << fmt("unknown file type at %1%", PathFmt(path));
        }
    }

    HashAlgorithm getHashAlgo() const
    {
        return std::get<HashAlgorithm>(GetParam());
    }
};

namespace {

/* The canonicalising hooks, counting the regular files the restore sink
   created: the discriminator for the hit path (01 section 9.10, "The
   sink"), which links a blob the store holds and creates no file. */
struct CountingRestoreHooks : RestoreSinkHooks
{
    std::unique_ptr<RestoreSinkHooks> inner;
    unsigned regularFilesCreated = 0;

    explicit CountingRestoreHooks(std::unique_ptr<RestoreSinkHooks> inner)
        : inner(std::move(inner))
    {
    }

    void directoryDone(Descriptor dirFd) override
    {
        inner->directoryDone(dirFd);
    }

    void regularFileCreated(Descriptor fd, bool executable) override
    {
        regularFilesCreated++;
        inner->regularFileCreated(fd, executable);
    }

    void symlinkCreated(Descriptor parentFd, const CanonPath & name) override
    {
        inner->symlinkCreated(parentFd, name);
    }
};

/* An accessor that counts what is asked of it: the second route's promise
   is that nothing beneath a subtree the store holds is read (01 §9.10). */
struct CountingAccessor : SourceAccessor
{
private:
    void anchor() override {}

public:
    ref<SourceAccessor> inner;
    unsigned reads = 0, listings = 0, stats = 0, links = 0;

    explicit CountingAccessor(ref<SourceAccessor> inner)
        : inner(inner)
    {
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        reads++;
        inner->readFile(path, sink, sizeCallback);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        stats++;
        return inner->maybeLstat(path);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        listings++;
        return inner->readDirectory(path);
    }

    std::string readLink(const CanonPath & path) override
    {
        links++;
        return inner->readLink(path);
    }
};

} // namespace

/* The store's object store (doc/lazy-store/01-specification.md, sections
   9.10 and 9.11): `restoreThroughObjects` alone, with no pass after it, so
   that what the composite and the hasher's hooks did is what is checked.
   The object store is the store's representation, unconditionally: every
   restore enters it. */
class LocalStoreObjectsTest : public TempLocalStoreTest
{
protected:
    /* The regular files the last `restore` created (wrote), as its hooks
       counted them; a file linked from the object store is not one. */
    unsigned filesCreated = 0;

    ObjectHashSink::Result
    restore(const std::filesystem::path & dst, std::string_view nar, RepairFlag repair = NoRepair)
    {
        StringSource source{nar};
        CountingRestoreHooks hooks{
            makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls))};
        auto r = store->restoreThroughObjects(dst, source, /*startFsync=*/false, &hooks, repair);
        filesCreated = hooks.regularFilesCreated;
        return r;
    }

    static PosixStat st(const std::filesystem::path & p)
    {
        return lstat(p);
    }

    bool hasBlob(const Hash & id, bool executable)
    {
        return pathExists(store->objects.blobFile(id, executable));
    }

    /* The object store's tree readers (private to it; this fixture is its friend). */
    std::filesystem::path treeFile(const Hash & id)
    {
        return store->objects.treeFile(id);
    }

    bool hasTree(const Hash & id)
    {
        return pathExists(treeFile(id));
    }

    merkle::Tree readTree(const Hash & id)
    {
        auto body = store->objects.readTree(id);
        if (!body)
            throw Error("no tree %s", GitObjectStore::idString(id));
        return GitObjectStore::parseTreeBody(id, *body);
    }

    static merkle::Tree parseTreeBody(const Hash & id, std::string_view body)
    {
        return GitObjectStore::parseTreeBody(id, body);
    }

    size_t count(std::string_view sub)
    {
        size_t n = 0;
        for ([[maybe_unused]] auto & e : DirectoryIterator{store->objects.dir / std::string(sub)})
            n++;
        return n;
    }

    /* The walk over the path as written, without hooks: the independent
       computation of what the restore's sink recorded from the stream. */
    static merkle::TreeEntry gitHash(const std::filesystem::path & p)
    {
        return objectHashOf(*makeFSSourceAccessor(p), CanonPath::root).root;
    }

    /* The on-disk walk alone (private to the store; this fixture is its friend). */
    ObjectHashSink::Result
    enter(OptimiseStats & stats, const std::filesystem::path & p, GitObjectStore::BlobInodes & inodes)
    {
        return store->enterIntoObjects(p, NoRepair, nullptr, &stats, &inodes);
    }

    /* A tree written plainly at `p` (an older store's path, a build's
       output): `a` holds `bytes`, canonicalised. */
    static void writePlainTree(const std::filesystem::path & p, std::string_view bytes)
    {
        createDirs(p);
        writeFile(p / "a", bytes);
        canonicalisePathMetaData(p, {NIX_WHEN_SUPPORT_ACLS(noAclsToIgnore)});
    }

    /* Replace the link at `p` by a plain file holding the same bytes, as
       the store leaves a file it could not link (the link limit, macOS's
       `.app/Contents`, a writable file). */
    static void unlinkInPlace(const std::filesystem::path & p)
    {
        auto bytes = readFile(p);
        auto mode = lstat(p).st_mode & ~S_IFMT;
        auto dir = p.parent_path();
        chmod(dir, 0755);
        std::filesystem::remove(p);
        writeFile(p, bytes);
        chmod(p, mode);
        canonicaliseTimestampAndPermissions(p);
        canonicaliseTimestampAndPermissions(dir);
    }

    using Nodes = std::vector<std::tuple<std::string, std::string, char>>;

    /* An in-memory tree given as (path, bytes, kind) with kind 'f' a
       regular file, 'x' an executable, 'l' a symlink whose target is the
       bytes; the directories on the way are made. */
    static ref<MemorySourceAccessor> treeAccessor(const Nodes & nodes)
    {
        using File = MemorySourceAccessor::File;
        auto accessor = make_ref<MemorySourceAccessor>();
        accessor->root = File{File::Directory{}};
        for (auto & [path, bytes, kind] : nodes) {
            CanonPath p{path};
            if (kind == 'l')
                accessor->open(p, File{File::Symlink{.target = bytes}});
            else {
                accessor->addFile(p, std::string(bytes));
                if (kind == 'x')
                    std::get<File::Regular>(accessor->open(p, std::nullopt)->raw).executable = true;
            }
        }
        return accessor;
    }

    /* The NAR of such a tree. */
    static std::string treeNar(const Nodes & nodes)
    {
        return narOf(*treeAccessor(nodes));
    }

    /* The second route of 01 §9.10 alone, no pass after it, with the same
       counting hooks as `restore`: the tree at `src` into `dst`, `namer`
       standing for the naming's memo. */
    ObjectHashSink::Result materialise(
        const std::filesystem::path & dst,
        const SourcePath & src,
        LocalStore::TreeNamer namer,
        RepairFlag repair = NoRepair,
        PathFilter & filter = defaultPathFilter)
    {
        CountingRestoreHooks hooks{
            makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls))};
        auto r = store->materialiseThroughObjects(dst, src, filter, /*startFsync=*/false, &hooks, repair, namer);
        filesCreated = hooks.regularFilesCreated;
        return r;
    }

    /* A namer that has every node's entry, as a fully memoised naming
       has: the reference hash of the subtree. */
    static LocalStore::TreeNamer everyNode(ref<SourceAccessor> tree)
    {
        return [tree](const CanonPath & to, const SourceAccessor::Stat &) -> std::optional<merkle::TreeEntry> {
            return objectHashOf(*tree, to).root;
        };
    }

    static LocalStore::TreeNamer noNode()
    {
        return [](const CanonPath &, const SourceAccessor::Stat &) -> std::optional<merkle::TreeEntry> {
            return std::nullopt;
        };
    }

    /* Every regular file below `p` (or `p` itself), by its path relative
       to `p`. */
    static std::vector<std::filesystem::path> regularFilesUnder(const std::filesystem::path & p)
    {
        std::vector<std::filesystem::path> files;
        auto st = lstat(p);
        if (S_ISREG(st.st_mode))
            files.push_back("");
        else if (S_ISDIR(st.st_mode))
            for (auto & e : std::filesystem::recursive_directory_iterator{p, std::filesystem::directory_options::none})
                if (e.is_regular_file() && !e.is_symlink())
                    files.push_back(std::filesystem::relative(e.path(), p));
        return files;
    }

    /* A tree with a file, a subdirectory and a symlink: the shape the sweep
       and collection tests share. */
    static std::string treeWithSymlink()
    {
        return treeNar({{"a", "one", 'f'}, {"d/b", "two", 'f'}, {"d/l", "b", 'l'}});
    }
};

TEST_F(LocalStoreObjectsTest, restoreThroughObjects)
{
    /* Files at the root and two directories down; `a` and `d/e/c` are the
       same bytes, `d/e/x` the same bytes executable: one blob, two files. */
    auto nar = treeNar(
        {{"a", "one", 'f'},
         {"d/b", "two", 'f'},
         {"d/e/c", "one", 'f'},
         {"d/e/x", "one", 'x'},
         {"empty", "", 'f'},
         {"d/link", "b", 'l'}});

    auto & objects = store->objects;
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto t1 = realStoreDir / "t1";
    auto r1 = restore(t1, nar);
    auto & root1 = r1.root;

    /* The identifiers are git's, computed independently over the tree; the
       NAR's length is summed from names and sizes. */
    EXPECT_EQ(root1, gitHash(t1));
    EXPECT_EQ(root1.mode, merkle::Mode::Directory);
    EXPECT_EQ(r1.narSize, nar.size());

    /* Five regular files, four written: `d/e/c` arrived after `a` had
       entered its blob, so it was linked and not written. */
    EXPECT_EQ(filesCreated, 4u);

    /* Blobs: "one", "two", "" and the symlink's "b" under the plain mode,
       "one" again under the executable mode; three trees. */
    EXPECT_EQ(count("blobs"), 4u);
    EXPECT_EQ(count("blobs-x"), 1u);
    EXPECT_EQ(count("trees"), 3u);
    auto one = merkle::blobId("one");
    EXPECT_TRUE(hasBlob(one, false));
    EXPECT_TRUE(hasBlob(one, true));
    EXPECT_TRUE(hasTree(root1.hash));

    /* `a` and `d/e/c` share the plain file; `d/e/x` is the executable
       file, another inode with the same bytes; never a link across the bit. */
    EXPECT_EQ(st(t1 / "a").st_nlink, 3u);
    EXPECT_EQ(st(t1 / "d/e/c").st_ino, st(t1 / "a").st_ino);
    EXPECT_EQ(st(t1 / "a").st_ino, st(objects.blobFile(one, false)).st_ino);
    EXPECT_EQ(st(t1 / "d/e/x").st_ino, st(objects.blobFile(one, true)).st_ino);
    EXPECT_NE(st(t1 / "d/e/x").st_ino, st(t1 / "a").st_ino);
    EXPECT_EQ(st(t1 / "d/e/x").st_nlink, 2u);
    EXPECT_EQ(readFile(objects.blobFile(one, true)), "one");
    EXPECT_EQ(st(t1 / "d/b").st_nlink, 2u);
    EXPECT_EQ(st(t1 / "empty").st_nlink, 2u);
    for (auto & p : {t1 / "a", t1 / "d/b", t1 / "d/e/c", t1 / "empty"})
        EXPECT_EQ(st(p).st_mode & ~S_IFMT, 0444u) << p;
    EXPECT_EQ(st(t1 / "d/e/x").st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(st(objects.blobFile(one, false)).st_mode & ~S_IFMT, 0444u);
    EXPECT_EQ(st(objects.blobFile(one, true)).st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(st(t1 / "d").st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(st(t1 / "a").st_mtime, 1);
    EXPECT_EQ(readLink(t1 / "d/link"), "b");

    /* The trees read back as what was written. */
    auto rootTree = readTree(root1.hash);
    ASSERT_EQ(rootTree.size(), 3u);
    EXPECT_EQ(rootTree.at("a"), (merkle::TreeEntry{merkle::Mode::Regular, one}));
    EXPECT_EQ(rootTree.at("d/").mode, merkle::Mode::Directory);
    EXPECT_EQ(rootTree.at("empty"), (merkle::TreeEntry{merkle::Mode::Regular, merkle::blobId("")}));
    auto dTree = readTree(rootTree.at("d/").hash);
    EXPECT_EQ(dTree.at("link"), (merkle::TreeEntry{merkle::Mode::Symlink, merkle::blobId("b")}));
    auto eTree = readTree(dTree.at("e/").hash);
    EXPECT_EQ(eTree.at("x"), (merkle::TreeEntry{merkle::Mode::Executable, one}));

    /* The same tree again: the store holds every blob, so every file is
       linked from the object store's file and none is written; it shares
       every inode with the first and no object is added; the tree dumps
       back as the bytes restored. */
    auto t2 = realStoreDir / "t2";
    auto r2 = restore(t2, nar);
    EXPECT_EQ(r2.root, root1);
    EXPECT_EQ(r2.narSize, r1.narSize);
    EXPECT_EQ(filesCreated, 0u);
    EXPECT_EQ(count("blobs"), 4u);
    EXPECT_EQ(count("blobs-x"), 1u);
    EXPECT_EQ(count("trees"), 3u);
    for (auto & rel : {"a", "d/b", "d/e/c", "d/e/x", "empty"})
        EXPECT_EQ(st(t2 / rel).st_ino, st(t1 / rel).st_ino) << rel;
    EXPECT_EQ(st(t2 / "a").st_nlink, 5u);
    StringSink again;
    dumpPath(t2, again);
    EXPECT_EQ(again.s, nar);

    /* A one-file NAR: the root is the file, the destination itself. */
    StringSink oneFile;
    dumpString("two", oneFile);
    auto t3 = realStoreDir / "t3";
    auto r3 = restore(t3, oneFile.s);
    EXPECT_EQ(r3.root, (merkle::TreeEntry{merkle::Mode::Regular, merkle::blobId("two")}));
    EXPECT_EQ(r3.root, gitHash(t3));
    EXPECT_EQ(r3.narSize, oneFile.s.size());
    EXPECT_EQ(st(t3).st_ino, st(t1 / "d/b").st_ino);
    EXPECT_EQ(filesCreated, 0u); /* linked by its full path: the store held "two" */
    EXPECT_EQ(count("blobs"), 4u);

    /* The sweep, over the live roots the caller yields (the object hashes
       of the valid paths, in the store the database's column): the trees
       nothing reaches go, then the blob files nothing links.  A plain
       file's object hash (`r3`'s) names no tree and is simply no root of
       any tree; its blob is held by its links. */
    auto swept = objects.sweep([&](fun<void(const ObjectHash &)> live) {
        live(ObjectHash::of(root1));
        live(ObjectHash::of(r3.root));
        return 0;
    });
    EXPECT_EQ(swept.unmigrated, 0u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.blobsKept, 5u);
    /* A root the enumeration cannot give (an unmigrated row) stops the
       sweep from removing anything but temporary files. */
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 1; });
    EXPECT_EQ(swept.unmigrated, 1u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(count("trees"), 3u);
    EXPECT_EQ(count("blobs"), 4u);
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; });
    EXPECT_EQ(swept.treesRemoved, 3u);
    /* The symlink's blob, linked by nothing, goes with the trees that named it. */
    EXPECT_EQ(swept.blobsRemoved, 1u);
    EXPECT_FALSE(hasBlob(merkle::blobId("b"), false));
    EXPECT_EQ(count("trees"), 0u);
    deletePath(t1);
    deletePath(t2);
    deletePath(t3);
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; });
    EXPECT_EQ(swept.blobsRemoved, 4u);
    EXPECT_EQ(count("blobs"), 0u);
    EXPECT_EQ(count("blobs-x"), 0u);
}

/* A tree differing from one the store holds in one file: that file is
   written and its blob entered; every other is linked and not written. */
TEST_F(LocalStoreObjectsTest, oneChangedFileWritesOneFile)
{
    auto nar = [](std::string_view b) {
        return treeNar({{"a", "one", 'f'}, {"d/b", std::string(b), 'f'}, {"d/x", "one", 'x'}, {"d/l", "b", 'l'}});
    };
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto t1 = realStoreDir / "c1";
    restore(t1, nar("two"));
    EXPECT_EQ(filesCreated, 3u);
    EXPECT_EQ(count("blobs"), 3u); /* "one", "two", the symlink's "b" */
    EXPECT_EQ(count("blobs-x"), 1u);

    auto t2 = realStoreDir / "c2";
    auto r2 = restore(t2, nar("TWO"));
    EXPECT_EQ(r2.root, gitHash(t2));
    EXPECT_EQ(filesCreated, 1u);
    EXPECT_EQ(count("blobs"), 4u);
    EXPECT_EQ(count("blobs-x"), 1u);
    EXPECT_EQ(readFile(t2 / "d/b"), "TWO");
    EXPECT_EQ(st(t2 / "d/b").st_ino, st(store->objects.blobFile(merkle::blobId("TWO"), false)).st_ino);
    EXPECT_NE(st(t2 / "d/b").st_ino, st(t1 / "d/b").st_ino);
    EXPECT_EQ(st(t2 / "a").st_ino, st(t1 / "a").st_ino);
    EXPECT_EQ(st(t2 / "d/x").st_ino, st(t1 / "d/x").st_ino);
    EXPECT_EQ(st(t2 / "a").st_mode & ~S_IFMT, 0444u);
    EXPECT_EQ(st(t2 / "d/b").st_mode & ~S_IFMT, 0444u);
    EXPECT_EQ(st(t2 / "d/x").st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(st(t2 / "d").st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(st(t2 / "a").st_mtime, 1);
    EXPECT_EQ(st(t2 / "d/b").st_mtime, 1);
    StringSink again;
    dumpPath(t2, again);
    EXPECT_EQ(again.s, nar("TWO"));
}

/* A file above the composite's buffer limit is written while its bytes
   arrive and hashed from the header `preallocateContents` announced; the
   identifier must be the same as for a buffered file, and the file is
   entered or replaced by the object store's file like any other, under
   either mode.  Restored twice, the large files are written once per
   restore (the write-then-link cost, accepted above the limit) and the
   small one is linked: the boundary. */
TEST_F(LocalStoreObjectsTest, restoreThroughObjectsStreamsLargeFiles)
{
    std::string big(3 * 1024 * 1024 + 17, 'x');
    for (size_t i = 0; i < big.size(); i += 4099)
        big[i] = 'a' + (i % 26);

    auto nar = treeNar({{"big", big, 'f'}, {"big-x", big, 'x'}, {"small", "s", 'f'}});

    auto & objects = store->objects;
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto t1 = realStoreDir / "big1";
    auto r1 = restore(t1, nar);
    EXPECT_EQ(r1.root, gitHash(t1));
    EXPECT_EQ(r1.narSize, nar.size());
    EXPECT_EQ(filesCreated, 3u);

    auto bigId = merkle::blobId(big);
    EXPECT_TRUE(hasBlob(bigId, false));
    EXPECT_TRUE(hasBlob(bigId, true));
    EXPECT_EQ(st(t1 / "big").st_ino, st(objects.blobFile(bigId, false)).st_ino);
    EXPECT_EQ(st(t1 / "big-x").st_ino, st(objects.blobFile(bigId, true)).st_ino);
    EXPECT_NE(st(t1 / "big").st_ino, st(t1 / "big-x").st_ino);
    EXPECT_EQ(st(t1 / "big-x").st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(readFile(t1 / "big"), big);
    EXPECT_EQ(readFile(t1 / "big-x"), big);

    /* Again: each large file is written, then replaced by the object
       store's file once its identifier is known; the small one is linked
       and not written; the two paths share every inode. */
    auto t2 = realStoreDir / "big2";
    auto r2 = restore(t2, nar);
    EXPECT_EQ(r2.root, r1.root);
    EXPECT_EQ(r2.narSize, r1.narSize);
    EXPECT_EQ(filesCreated, 2u);
    EXPECT_EQ(st(t2 / "small").st_ino, st(t1 / "small").st_ino);
    EXPECT_EQ(st(t2 / "big").st_ino, st(t1 / "big").st_ino);
    EXPECT_EQ(st(t2 / "big-x").st_ino, st(t1 / "big-x").st_ino);
    EXPECT_EQ(st(t1 / "big").st_nlink, 3u);
    EXPECT_EQ(count("blobs"), 2u);
    EXPECT_EQ(count("blobs-x"), 1u);
    StringSink again;
    dumpPath(t2, again);
    EXPECT_EQ(again.s, nar);
}

/* The executable file first, then the plain one with the same bytes: the
   twin is made whichever mode comes first. */
TEST_F(LocalStoreObjectsTest, executableFirst)
{
    auto nar = treeNar({{"a-x", "one", 'x'}, {"b", "one", 'f'}});
    auto t = std::filesystem::path{config->realStoreDir.get()} / "xf";
    restore(t, nar);
    auto one = merkle::blobId("one");
    EXPECT_EQ(st(t / "a-x").st_ino, st(store->objects.blobFile(one, true)).st_ino);
    EXPECT_EQ(st(t / "b").st_ino, st(store->objects.blobFile(one, false)).st_ino);
    EXPECT_NE(st(t / "a-x").st_ino, st(t / "b").st_ino);
}

/* An executable or symlink root (01 section 9.11, the review): its object
   hash is the id of the synthetic tree `{"." -> root}`, and when entering
   the sink writes that tree, so that the root's blob is reachable from
   the tree its object hash names as a directory's blobs are from its
   trees.  A sweep that keeps the path removes nothing; one that drops it
   removes the synthetic tree, and with it the symlink's blob (linked by
   nothing) but not the executable's while the store path's link holds
   it -- the link-count rule for blobs. */
TEST_F(LocalStoreObjectsTest, syntheticRootTreeIsWrittenAndKeepsItsBlob)
{
    auto & objects = store->objects;
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};

    /* The two one-object NARs, dumped from disk. */
    auto srcLink = tempStoreDir.path() / "src-link";
    createSymlink("target", srcLink);
    StringSink linkNar;
    dumpPath(srcLink, linkNar);
    auto srcExec = tempStoreDir.path() / "src-exec";
    writeFile(srcExec, "run");
    chmod(srcExec, 0755);
    StringSink execNar;
    dumpPath(srcExec, execNar);

    auto l = realStoreDir / "l";
    auto rl = restore(l, linkNar.s);
    EXPECT_EQ(rl.root, (merkle::TreeEntry{merkle::Mode::Symlink, merkle::blobId("target")}));
    EXPECT_EQ(rl.root, gitHash(l));
    EXPECT_EQ(readLink(l), "target");
    auto linkBody = merkle::syntheticRootTree(rl.root);
    ASSERT_TRUE(linkBody);
    auto linkTree = merkle::treeId(*linkBody);
    EXPECT_EQ(ObjectHash::of(rl.root).hash, linkTree);
    EXPECT_TRUE(hasTree(linkTree));
    EXPECT_EQ(readTree(linkTree).at("."), (merkle::TreeEntry{merkle::Mode::Symlink, merkle::blobId("target")}));
    EXPECT_TRUE(hasBlob(merkle::blobId("target"), false));
    EXPECT_EQ(st(objects.blobFile(merkle::blobId("target"), false)).st_nlink, 1u);

    auto x = realStoreDir / "x";
    auto rx = restore(x, execNar.s);
    EXPECT_EQ(rx.root, (merkle::TreeEntry{merkle::Mode::Executable, merkle::blobId("run")}));
    EXPECT_EQ(rx.root, gitHash(x));
    auto execBody = merkle::syntheticRootTree(rx.root);
    ASSERT_TRUE(execBody);
    auto execTree = merkle::treeId(*execBody);
    EXPECT_EQ(ObjectHash::of(rx.root).hash, execTree);
    EXPECT_TRUE(hasTree(execTree));
    EXPECT_NE(execTree, linkTree);
    EXPECT_EQ(st(x).st_ino, st(objects.blobFile(merkle::blobId("run"), true)).st_ino);
    EXPECT_EQ(st(x).st_nlink, 2u);

    EXPECT_EQ(count("trees"), 2u);
    EXPECT_EQ(count("blobs"), 1u);
    EXPECT_EQ(count("blobs-x"), 1u);

    /* Both live: nothing goes; the symlink's blob is kept through its tree. */
    auto swept = objects.sweep([&](fun<void(const ObjectHash &)> live) {
        live(ObjectHash::of(rl.root));
        live(ObjectHash::of(rx.root));
        return 0;
    });
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.blobsKept, 2u);

    /* The symlink dropped: its synthetic tree, and then its blob, go. */
    swept = objects.sweep([&](fun<void(const ObjectHash &)> live) {
        live(ObjectHash::of(rx.root));
        return 0;
    });
    EXPECT_EQ(swept.treesRemoved, 1u);
    EXPECT_EQ(swept.blobsRemoved, 1u);
    EXPECT_EQ(swept.blobsKept, 1u);
    EXPECT_FALSE(hasBlob(merkle::blobId("target"), false));
    EXPECT_TRUE(hasTree(execTree));

    /* The executable dropped while its file stands: the tree goes, the
       blob stays by its link; once the file is gone, the blob goes too. */
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; });
    EXPECT_EQ(swept.treesRemoved, 1u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.blobsKept, 1u);
    EXPECT_TRUE(hasBlob(merkle::blobId("run"), true));
    EXPECT_EQ(count("trees"), 0u);
    deletePath(x);
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; });
    EXPECT_EQ(swept.blobsRemoved, 1u);
    EXPECT_EQ(count("blobs-x"), 0u);
}

/* Repair: a blob file corrupted in place, same length, is not linked to
   when a path is restored with repair; it is replaced. */
TEST_F(LocalStoreObjectsTest, repairDoesNotLinkACorruptBlob)
{
    auto nar = treeNar({{"a", "one", 'f'}});
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto t1 = realStoreDir / "r1";
    restore(t1, nar);
    auto one = merkle::blobId("one");
    auto file = store->objects.blobFile(one, false);
    /* Corrupt the blob's file in place (through its store-path link). */
    chmod(t1 / "a", 0644);
    writeFile(t1 / "a", "ONE");
    chmod(t1 / "a", 0444);
    EXPECT_EQ(readFile(file), "ONE");
    /* Without repair the composite trusts the size and links the corrupt
       file, writing nothing. */
    auto t2 = realStoreDir / "r2";
    restore(t2, nar);
    EXPECT_EQ(readFile(t2 / "a"), "ONE");
    EXPECT_EQ(filesCreated, 0u);
    /* With repair it hashes the file, removes it, and writes anew. */
    auto t3 = realStoreDir / "r3";
    restore(t3, nar, Repair);
    EXPECT_EQ(filesCreated, 1u);
    EXPECT_EQ(readFile(t3 / "a"), "one");
    EXPECT_EQ(readFile(file), "one");
    EXPECT_NE(st(t3 / "a").st_ino, st(t1 / "a").st_ino);
}

/* The on-disk walk under repair -- a `--repair` build's, after the output
   is written -- must not link a corrupt blob the store holds over the
   fresh output: the fresh bytes stand and become the store's file.  The
   discriminator first: without repair the walk trusts the blob's size and
   links the corrupt file over the fresh one (what the builder did when it
   passed `NoRepair` under `bmRepair`). */
TEST_F(LocalStoreObjectsTest, repairRelinksAFreshFileOverACorruptBlob)
{
    auto nar = treeNar({{"a", "one", 'f'}});
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto t1 = realStoreDir / "k1";
    restore(t1, nar);
    auto one = merkle::blobId("one");
    auto file = store->objects.blobFile(one, false);
    /* The blob corrupted in place, the same length. */
    chmod(t1 / "a", 0644);
    writeFile(t1 / "a", "ONE");
    chmod(t1 / "a", 0444);
    ASSERT_EQ(readFile(file), "ONE");

    /* A fresh, correct output written plainly; entered without repair, its
       file is replaced by a link to the corrupt blob. */
    auto plain = realStoreDir / "k2";
    writePlainTree(plain, "one");
    ASSERT_EQ(st(plain / "a").st_nlink, 1u);
    store->enterPath(plain, NoRepair);
    EXPECT_EQ(readFile(plain / "a"), "ONE");
    EXPECT_EQ(st(plain / "a").st_ino, st(file).st_ino);

    /* Entered with repair, the corrupt blob is hashed, removed, and the
       fresh file becomes the store's. */
    auto fresh = realStoreDir / "k3";
    writePlainTree(fresh, "one");
    auto freshInode = st(fresh / "a").st_ino;
    auto r = store->enterPath(fresh, Repair);
    EXPECT_EQ(r.root, gitHash(fresh));
    EXPECT_EQ(readFile(fresh / "a"), "one");
    EXPECT_EQ(readFile(file), "one");
    EXPECT_EQ(st(fresh / "a").st_ino, freshInode);
    EXPECT_EQ(st(file).st_ino, freshInode);
    EXPECT_EQ(st(fresh / "a").st_nlink, 2u);
}

TEST_F(LocalStoreObjectsTest, enterIntoObjectsAndMigrate)
{
    /* A path written plainly (an older store), then entered: its files become links, its trees written, its root the
       tree's identifier; entering it again reads no file (their inodes
       are the object store's) and changes nothing. */
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto p = realStoreDir / "p";
    createDirs(p / "d");
    writeFile(p / "a", "one");
    writeFile(p / "d/b", "one");
    writeFile(p / "d/x", "one");
    chmod(p / "d/x", 0755);
    createSymlink("a", p / "d/l");
    canonicalisePathMetaData(p, {NIX_WHEN_SUPPORT_ACLS(noAclsToIgnore)});
    EXPECT_EQ(st(p / "a").st_nlink, 1u);

    OptimiseStats stats;
    GitObjectStore::BlobInodes inodes;
    auto result = enter(stats, p, inodes);
    auto root = result.root;
    EXPECT_EQ(root, gitHash(p));
    StringSink asNar;
    dumpPath(p, asNar);
    EXPECT_EQ(result.narSize, asNar.s.size());
    EXPECT_EQ(stats.filesLinked, 1u); /* `d/b`, linked to `a`'s inode; `a` and `d/x` entered */
    EXPECT_EQ(stats.filesEntered, 2u);
    EXPECT_EQ(st(p / "a").st_nlink, 3u);
    EXPECT_EQ(st(p / "d/b").st_ino, st(p / "a").st_ino);
    EXPECT_EQ(st(p / "d/x").st_nlink, 2u);
    EXPECT_NE(st(p / "d/x").st_ino, st(p / "a").st_ino);
    EXPECT_EQ(count("blobs"), 2u); /* "one", and the symlink's "a" */
    EXPECT_EQ(count("blobs-x"), 1u);
    EXPECT_EQ(count("trees"), 2u);
    EXPECT_EQ(readFile(p / "d/b"), "one");
    EXPECT_EQ(inodes.size(), 2u);

    OptimiseStats again;
    auto root2 = enter(again, p, inodes).root;
    EXPECT_EQ(root2, root);
    EXPECT_EQ(again.filesLinked, 0u);
    EXPECT_EQ(again.filesEntered, 0u);
    EXPECT_EQ(st(p / "a").st_nlink, 3u);
}

/* `--optimise` on a store an older Nix wrote (01 section 10, *`nix-store
   --optimise` writes an older row's object hash*): a schema-10 row is
   migrated by the verified walk before its path is entered -- counted in
   `rowsMigrated`, the column then the object hash's rendering -- and a row
   whose path was modified since is neither migrated nor entered.  Before,
   the column write was pinned by the functional block alone. */
TEST_F(LocalStoreObjectsTest, optimiseStoreMigratesAnOldRowAndEntersNoModifiedPath)
{
    auto add = [&](std::string_view name, std::string_view bytes) {
        auto nar = treeNar({{"a", std::string(bytes), 'f'}});
        StringSource source{nar};
        auto p = store->addToStoreFromDump(
            source,
            std::string{name},
            FileSerialisationMethod::NixArchive,
            ContentAddressMethod::Raw::NixArchive,
            HashAlgorithm::SHA256,
            {},
            NoRepair);
        auto narHash = hashString(HashAlgorithm::SHA256, nar);
        plantSchema10Row(*store, p, narHash, nar.size());
        /* As an older store holds the path: its file plain, no blob. */
        unlinkInPlace(store->toRealPath(p) / "a");
        std::filesystem::remove(store->objects.blobFile(merkle::blobId(bytes), false));
        return std::pair{p, narHash.to_string(HashFormat::Base16, true)};
    };
    auto [sound, soundColumn] = add("sound", "one\n");
    auto [modified, modifiedColumn] = add("modified", "two\n");
    auto b = store->toRealPath(modified) / "a";
    chmod(b, 0644);
    writeFile(b, "xwo\n");
    chmod(b, 0444);

    OptimiseStats stats;
    store->optimiseStore(stats);
    EXPECT_EQ(stats.rowsMigrated, 1u);
    EXPECT_EQ(stats.filesEntered, 1u) << "the sound path's file";
    EXPECT_EQ(readHashColumn(*store, sound), ObjectHash::of(gitHash(store->toRealPath(sound))).render());
    EXPECT_EQ(st(store->toRealPath(sound) / "a").st_nlink, 2u) << "entered";
    EXPECT_EQ(readHashColumn(*store, modified), modifiedColumn) << "left as the database has it";
    EXPECT_EQ(st(b).st_nlink, 1u) << "not entered";
    EXPECT_FALSE(hasBlob(merkle::blobId("xwo\n"), false));
}

/* A tree added under the git method: its content address is the root the
   sink computed, no file read back and the dump not hashed, and it is
   the on-disk walk's. */
TEST_F(LocalStoreObjectsTest, addToStoreFromDumpUnderTheGitMethod)
{
    auto nar = treeNar({{"a", "one", 'f'}, {"d/x", "one", 'x'}, {"d/l", "x", 'l'}});
    StringSource source{nar};
    auto p = store->addToStoreFromDump(
        source,
        "git-named",
        FileSerialisationMethod::NixArchive,
        ContentAddressMethod::Raw::Git,
        HashAlgorithm::SHA256,
        {},
        NoRepair);
    auto real = store->toRealPath(p);
    auto info = store->queryPathInfo(p);
    ASSERT_TRUE(info->ca);
    EXPECT_EQ(info->ca->hash, gitHash(real).hash);
    EXPECT_TRUE(hasTree(gitHash(real).hash));
    /* The row's identity is the object hash; the NAR hash is the shim's. */
    EXPECT_EQ(info->objectHash, ObjectHash::of(gitHash(real)));
    EXPECT_EQ(hashString(HashAlgorithm::SHA256, nar), narHashOf(*store, p));
    EXPECT_EQ(st(real / "a").st_nlink, 2u);
    EXPECT_NE(st(real / "d/x").st_ino, st(real / "a").st_ino);
    /* The same tree again is the same path, valid already. */
    StringSource again{nar};
    EXPECT_EQ(
        store->addToStoreFromDump(
            again,
            "git-named",
            FileSerialisationMethod::NixArchive,
            ContentAddressMethod::Raw::Git,
            HashAlgorithm::SHA256,
            {},
            NoRepair),
        p);
}

/* The names the hash is over (01 section 9.11): a tree with `Foo` and
   `foo` restored under the case hack has `foo` renamed on disk; the sink
   hashes the tree's own names, so its root is the original tree's and is
   what a walk of the restored directory computes when that walk unhacks as
   the serialiser does. */
TEST_F(LocalStoreObjectsTest, caseHackSuffixIsNotHashed)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    accessor->addFile(CanonPath{"/Foo"}, "1");
    accessor->addFile(CanonPath{"/foo"}, "2");
    StringSink nar;
    accessor->dumpPath(CanonPath::root, nar);
    auto expected = objectHashOf(*accessor, CanonPath::root).root;

    WithCaseHack caseHack{true};

    auto t = std::filesystem::path{config->realStoreDir.get()} / "case";
    auto r = restore(t, nar.s);
    EXPECT_EQ(r.root, expected);
    EXPECT_EQ(r.narSize, nar.s.size());

    /* The hack fired: the second name was rewritten on the way in.  The
       listing, not `pathExists`, since on a case-insensitive filesystem
       `foo` would resolve to `Foo`. */
    auto hacked = std::string("foo") + std::string(caseHackSuffix) + "1";
    std::set<std::string> onDisk;
    for (auto & e : DirectoryIterator{t})
        onDisk.insert(e.path().filename().string());
    ASSERT_EQ(onDisk, (std::set<std::string>{"Foo", hacked}));
    EXPECT_EQ(readFile(t / "Foo"), "1");
    EXPECT_EQ(readFile(t / hacked), "2");

    /* Re-hashed from disk, unhacking: the same root, the same NAR size. */
    auto fromDisk = objectHashOf(*makeFSSourceAccessor(t), CanonPath::root);
    EXPECT_EQ(fromDisk.root, expected);
    EXPECT_EQ(fromDisk.narSize, nar.s.size());

    /* And the tree the sink wrote holds the tree's names, not the disk's. */
    auto tree = readTree(r.root.hash);
    ASSERT_EQ(tree.size(), 2u);
    EXPECT_EQ(tree.at("Foo"), (merkle::TreeEntry{merkle::Mode::Regular, merkle::blobId("1")}));
    EXPECT_EQ(tree.at("foo"), (merkle::TreeEntry{merkle::Mode::Regular, merkle::blobId("2")}));
}

/* Law 4 of 01 section 9.10 against the sweep: a blob a live tree names is
   kept whatever its link count.  A file the store could not link -- here
   made so by hand, under both modes -- leaves its blob with the store's
   link alone, and the sweep must not take it while the path is valid. */
TEST_F(LocalStoreObjectsTest, sweepKeepsABlobALiveTreeNamesThoughNothingLinksIt)
{
    auto nar = treeNar({{"a", "kept", 'f'}, {"x", "kept too", 'x'}});
    auto & objects = store->objects;
    auto t = std::filesystem::path{config->realStoreDir.get()} / "unlinked";
    auto r = restore(t, nar);

    auto plain = merkle::blobId("kept");
    auto exec = merkle::blobId("kept too");
    ASSERT_EQ(st(objects.blobFile(plain, false)).st_nlink, 2u);
    ASSERT_EQ(st(objects.blobFile(exec, true)).st_nlink, 2u);
    unlinkInPlace(t / "a");
    unlinkInPlace(t / "x");
    ASSERT_EQ(st(objects.blobFile(plain, false)).st_nlink, 1u);
    ASSERT_EQ(st(objects.blobFile(exec, true)).st_nlink, 1u);
    ASSERT_EQ(st(t / "x").st_mode & ~S_IFMT, 0555u);

    /* The path live: the tree names both blobs, so both stay. */
    auto swept = objects.sweep([&](fun<void(const ObjectHash &)> live) {
        live(ObjectHash::of(r.root));
        return 0;
    });
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.blobsKept, 2u);
    EXPECT_TRUE(hasBlob(plain, false));
    EXPECT_TRUE(hasBlob(exec, true));

    /* The path dropped: the tree goes, and with it both blobs. */
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; });
    EXPECT_EQ(swept.treesRemoved, 1u);
    EXPECT_EQ(swept.blobsRemoved, 2u);
    EXPECT_FALSE(hasBlob(plain, false));
    EXPECT_FALSE(hasBlob(exec, true));
}

/* A tree file whose body is not its name's -- empty after a crash, or
   short -- is not trusted by the sweep: an empty body parses as an empty
   tree, under which nothing beneath the root would be reachable, and a
   partial body does not parse.  On either the sweep warns and removes
   nothing; once the tree is whole again, the same sweep removes nothing
   because everything is live.  The discriminator: before, the empty body
   was an empty tree and the subtree and its blob went. */
TEST_F(LocalStoreObjectsTest, sweepAbortsOnACorruptTree)
{
    auto nar = treeWithSymlink();
    auto & objects = store->objects;
    auto t = std::filesystem::path{config->realStoreDir.get()} / "corrupt-tree";
    auto r = restore(t, nar);
    auto rootFile = treeFile(r.root.hash);
    auto body = readFile(rootFile);
    ASSERT_EQ(count("trees"), 2u);
    ASSERT_EQ(count("blobs"), 3u); /* "one", "two", the symlink's "b" */
    auto live = [&](fun<void(const ObjectHash &)> f) {
        f(ObjectHash::of(r.root));
        return 0;
    };

    /* Empty: hashes to the empty tree, not to its name. */
    chmod(rootFile, 0644);
    writeFile(rootFile, "");
    auto swept = objects.sweep(live);
    EXPECT_EQ(swept.corruptTrees, 1u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(count("trees"), 2u);
    EXPECT_EQ(count("blobs"), 3u);

    /* Short: the right prefix, which does not parse. */
    writeFile(rootFile, body.substr(0, body.size() / 2));
    swept = objects.sweep(live);
    EXPECT_EQ(swept.corruptTrees, 1u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(count("trees"), 2u);
    EXPECT_EQ(count("blobs"), 3u);

    /* Whole again: live, so nothing goes, and nothing is reported. */
    writeFile(rootFile, body);
    swept = objects.sweep(live);
    EXPECT_EQ(swept.corruptTrees, 0u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.blobsKept, 3u);
}

/* Laws 4 and 5 under a lost window (01 section 9.10, "Collection"): a
   valid path whose root tree a collection took between `putTree` and its
   registration.  The next sweep, with that path live, counts the root as
   unentered and keeps nothing beneath it -- the orphaned `d/` tree goes,
   and with it the symlink's blob and the unlinked file's blob, held by
   nothing but the trees that named them -- and removes what else is dead
   (before, the removal phase was abandoned and every later collection
   reclaimed nothing).  `verifyStore
   --check-contents` reports the path, re-entering it under --repair, after
   which the trees and blobs are back and the sweep keeps everything. */
TEST_F(LocalStoreObjectsTest, sweepAndVerifyOnALiveRootWhoseTreeIsMissing)
{
    auto nar = treeWithSymlink();
    auto & objects = store->objects;

    /* Registered, so that `verifyStore` visits it. */
    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-lost-tree"},
        UnkeyedValidPathInfo{*store, std::nullopt},
    };
    info.assertedNarHash = hashString(HashAlgorithm::SHA256, nar);
    info.narSize = nar.size();
    StringSource source{nar};
    store->addToStore(info, source, NoRepair, NoCheckSigs);
    auto row = store->queryPathInfo(info.path);
    ASSERT_TRUE(row->objectHash.has_value());
    auto rootTree = row->objectHash->hash;
    ASSERT_TRUE(hasTree(rootTree));
    auto dTree = readTree(rootTree).at("d/").hash;
    ASSERT_TRUE(hasTree(dTree));
    auto symlinkBlob = merkle::blobId("b");
    ASSERT_TRUE(hasBlob(symlinkBlob, false));
    ASSERT_EQ(st(objects.blobFile(symlinkBlob, false)).st_nlink, 1u);
    /* `a` left as the store leaves a file it could not link: its blob too
       has the store's link alone, kept only by the tree that names it. */
    unlinkInPlace(store->toRealPath(info.path) / "a");
    ASSERT_EQ(st(objects.blobFile(merkle::blobId("one"), false)).st_nlink, 1u);
    ASSERT_EQ(count("trees"), 2u);
    ASSERT_EQ(count("blobs"), 3u);

    /* The race lost: the root tree is gone. */
    std::filesystem::remove(treeFile(rootTree));
    ASSERT_FALSE(hasTree(rootTree));
    auto live = [&](fun<void(const ObjectHash &)> f) {
        f(*row->objectHash);
        return 0;
    };

    auto swept = objects.sweep(live);
    EXPECT_EQ(swept.unentered, 1u);
    EXPECT_EQ(swept.treesRemoved, 1u) << "the orphaned d/ tree";
    EXPECT_EQ(swept.blobsRemoved, 2u) << "the symlink's blob and the unlinked file's blob";
    EXPECT_FALSE(hasTree(dTree));
    EXPECT_FALSE(hasBlob(symlinkBlob, false));
    EXPECT_FALSE(hasBlob(merkle::blobId("one"), false));
    EXPECT_EQ(count("blobs"), 1u) << "\"two\", held by d/b's link";
    EXPECT_EQ(readFile(store->toRealPath(info.path) / "a"), "one") << "the path's bytes are its own";

    /* The verifier: the path reaches objects the store lacks. */
    EXPECT_TRUE(store->verifyStore(/*checkContents=*/true, NoRepair)) << "an error is reported";
    EXPECT_FALSE(hasTree(rootTree));
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, Repair)) << "repaired, no error left";
    EXPECT_TRUE(hasTree(rootTree));
    EXPECT_TRUE(hasTree(dTree));
    EXPECT_EQ(readTree(rootTree).at("d/").hash, dTree);
    EXPECT_TRUE(hasBlob(symlinkBlob, false));
    EXPECT_TRUE(hasBlob(merkle::blobId("one"), false));
    EXPECT_EQ(st(store->toRealPath(info.path) / "a").st_nlink, 2u) << "re-entered: the file is the blob's again";
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, NoRepair));

    swept = objects.sweep(live);
    EXPECT_EQ(swept.unentered, 0u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.blobsKept, 3u);
}

/* A live root the store has no object for -- a row migrated by the walk
   (an upgraded or `--load-db` store), a plain-file root whose blob was
   never entered (a full directory index, Windows) -- keeps
   nothing and does not stop the sweep: a dead tree beside it goes, with
   the symlink blob it named.  Before: the root abandoned the removal phase
   and the dead objects stayed until `nix-store --optimise`. */
TEST_F(LocalStoreObjectsTest, sweepTolerantOfAnUnenteredRoot)
{
    auto nar = treeNar({{"a", "one", 'f'}, {"l", "a", 'l'}});
    auto & objects = store->objects;
    auto dead = std::filesystem::path{config->realStoreDir.get()} / "dead";
    auto r = restore(dead, nar);
    deletePath(dead);
    ASSERT_EQ(count("trees"), 1u);
    ASSERT_EQ(count("blobs"), 2u); /* "one" (now the store's link alone), the symlink's "a" */

    /* Two live roots without an object: a directory's tree never written,
       a plain file's blob never entered. */
    auto dirRoot = ObjectHash::of(merkle::TreeEntry{merkle::Mode::Directory, merkle::treeId("")});
    auto fileRoot = ObjectHash::of(merkle::TreeEntry{merkle::Mode::Regular, merkle::blobId("never entered")});
    ASSERT_FALSE(hasTree(dirRoot.hash));
    ASSERT_FALSE(hasBlob(fileRoot.hash, false));

    auto swept = objects.sweep([&](fun<void(const ObjectHash &)> live) {
        live(dirRoot);
        live(fileRoot);
        return 0;
    });
    EXPECT_EQ(swept.unentered, 2u);
    EXPECT_EQ(swept.corruptTrees, 0u);
    EXPECT_EQ(swept.treesRemoved, 1u);
    EXPECT_EQ(swept.blobsRemoved, 2u);
    EXPECT_FALSE(hasTree(r.root.hash));
    EXPECT_EQ(count("trees"), 0u);
    EXPECT_EQ(count("blobs"), 0u);
}

/* The sweep's count of live roots without an object (`SweepStats::
   unentered`) is of roots -- valid paths -- alone; a tree a present tree
   names that the store lacks (an interrupted repair, `ENOSPC` on one tree)
   is counted apart (`missingSubtrees`), and the warning says which.
   Before, both landed in one count rendered as "N valid paths have no
   tree". */
TEST_F(LocalStoreObjectsTest, sweepCountsRootsApartFromMissingSubtrees)
{
    auto nar = treeNar({{"a", "one", 'f'}, {"d/b", "two", 'f'}});
    auto & objects = store->objects;
    auto p = std::filesystem::path{config->realStoreDir.get()} / "two-level";
    auto r = restore(p, nar);
    auto rootTree = r.root.hash;
    auto dTree = readTree(rootTree).at("d/").hash;
    ASSERT_TRUE(hasTree(dTree));
    auto live = [&](fun<void(const ObjectHash &)> f) {
        f(ObjectHash::of(r.root));
        return 0;
    };

    /* The subtree gone, the root present: no path is without its object. */
    std::filesystem::remove(treeFile(dTree));
    auto swept = objects.sweep(live);
    EXPECT_EQ(swept.unentered, 0u);
    EXPECT_EQ(swept.missingSubtrees, 1u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_TRUE(hasTree(rootTree));
    EXPECT_EQ(count("blobs"), 2u) << "held by the path's links";

    /* The root gone too: one path without its object, and nothing beneath
       it is looked at. */
    std::filesystem::remove(treeFile(rootTree));
    swept = objects.sweep(live);
    EXPECT_EQ(swept.unentered, 1u);
    EXPECT_EQ(swept.missingSubtrees, 0u);
}

/* A temporary link `place` leaves behind on a crash between its `link`
   and its `rename` is a hard link to a blob file: it ages by the second
   in its name, not by the inode's change time, which every later link of
   that blob advances; and the temporary pass runs before the blob pass,
   so the blob the leftover pinned (link count two) goes in the same
   sweep.  A leftover with no time in its name (an earlier build's) still
   ages by the inode.  Before, such a leftover was never an hour old and
   kept its blob for ever. */
TEST_F(LocalStoreObjectsTest, staleTemporaryLinkGoesWithTheBlobItPinned)
{
    auto & objects = store->objects;
    StringSink nar;
    dumpString("pinned", nar);
    auto p = std::filesystem::path{config->realStoreDir.get()} / "dead";
    restore(p, nar.s);
    deletePath(p);
    auto blob = objects.blobFile(merkle::blobId("pinned"), false);
    ASSERT_TRUE(pathExists(blob));
    ASSERT_EQ(st(blob).st_nlink, 1u);

    auto now = time(nullptr);
    auto stale = objects.dir / "tmp" / fmt("link-%d-%d-0", now - 7200, getpid());
    std::filesystem::create_hard_link(blob, stale);
    ASSERT_EQ(st(blob).st_nlink, 2u);

    auto swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; });
    EXPECT_FALSE(pathExists(stale)) << "two hours old by its name";
    EXPECT_FALSE(pathExists(blob)) << "unpinned, unnamed: gone in the same sweep";
    EXPECT_EQ(swept.blobsRemoved, 1u);

    /* One made a moment ago -- another process's `place` between its two
       steps -- stays, and so does what it links. */
    restore(p, nar.s);
    deletePath(p);
    auto fresh = objects.dir / "tmp" / fmt("link-%d-%d-1", now, getpid());
    std::filesystem::create_hard_link(blob, fresh);
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; });
    EXPECT_TRUE(pathExists(fresh));
    EXPECT_TRUE(pathExists(blob));
    EXPECT_EQ(swept.blobsRemoved, 0u);
    std::filesystem::remove(fresh);
}

/* The verifier expects a blob for every regular file the store places and
   none for a file it does not (01 section 9.10, law 4 with law 3's
   exceptions): on macOS a file under `.app/Contents` is written and never
   placed, and before this the verifier reported every application bundle
   as reaching objects the store lacks, and `--repair` printed "entered"
   without effect.  The
   exception is decided in one place (`placementOf`), so the walk that
   enters and the walk that verifies agree.  A blob a live tree names that
   is really missing is still reported, and `--repair` brings it back. */
TEST_F(LocalStoreObjectsTest, verifyExpectsNoBlobForAFileTheStoreDoesNotPlace)
{
    std::vector<std::tuple<std::string, std::string, char>> nodes{{"README", "readme", 'f'}, {"l", "README", 'l'}};
#  ifdef __APPLE__
    nodes.emplace_back("Foo.app/Contents/PkgInfo", "APPL????", 'f');
#  endif
    auto nar = treeNar(nodes);
    auto & objects = store->objects;

    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-bundle"},
        UnkeyedValidPathInfo{*store, std::nullopt},
    };
    info.assertedNarHash = hashString(HashAlgorithm::SHA256, nar);
    info.narSize = nar.size();
    StringSource source{nar};
    store->addToStore(info, source, NoRepair, NoCheckSigs);
    auto real = store->toRealPath(info.path);

#  ifdef __APPLE__
    /* Written and left as written: no blob, one link. */
    EXPECT_FALSE(hasBlob(merkle::blobId("APPL????"), false));
    EXPECT_EQ(st(real / "Foo.app/Contents/PkgInfo").st_nlink, 1u);
#  endif
    EXPECT_EQ(st(real / "README").st_nlink, 2u);
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, NoRepair))
        << "clean: no blob is expected for a file the store does not place";

    /* A blob a live tree names, really missing: the symlink's target. */
    std::filesystem::remove(objects.blobFile(merkle::blobId("README"), false));
    EXPECT_TRUE(store->verifyStore(/*checkContents=*/true, NoRepair)) << "reported";
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, Repair)) << "re-entered";
    EXPECT_TRUE(hasBlob(merkle::blobId("README"), false));
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, NoRepair));

    /* The writable-file exception, on every platform: a
       file made writable in place is left as written -- `placementOf` says
       `LeftWritable` -- so no blob is expected for it even when the store
       holds none.  Its own bytes, and its blob's, are one inode here, so
       the file is first made its own again. */
    unlinkInPlace(real / "README");
    std::filesystem::remove(objects.blobFile(merkle::blobId("readme"), false));
    ASSERT_FALSE(hasBlob(merkle::blobId("readme"), false));
    chmod(real / "README", 0644);
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, NoRepair))
        << "clean: a writable file is left as written, and no blob is expected";
    chmod(real / "README", 0444);
    EXPECT_TRUE(store->verifyStore(/*checkContents=*/true, NoRepair))
        << "read-only again, the file is one the store places: its blob is missing";
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, Repair)) << "re-entered";
    EXPECT_TRUE(hasBlob(merkle::blobId("readme"), false));
}

/* A plain file's object hash is its blob id, which names no tree: the
   blob is what the root reaches and is kept whether or not the path still
   links it (law 4).  Before: with the path's file no longer a link, the
   blob went as unnamed. */
TEST_F(LocalStoreObjectsTest, sweepKeepsAPlainFileRootsBlob)
{
    auto & objects = store->objects;
    auto src = tempStoreDir.path() / "src-plain";
    writeFile(src, "plain root");
    StringSink nar;
    dumpPath(src, nar);
    auto t = std::filesystem::path{config->realStoreDir.get()} / "plain";
    auto r = restore(t, nar.s);
    auto blob = merkle::blobId("plain root");
    ASSERT_EQ(r.root, (merkle::TreeEntry{merkle::Mode::Regular, blob}));
    ASSERT_EQ(ObjectHash::of(r.root).hash, blob);
    ASSERT_EQ(st(objects.blobFile(blob, false)).st_nlink, 2u);
    unlinkInPlace(t);
    ASSERT_EQ(st(objects.blobFile(blob, false)).st_nlink, 1u);

    auto swept = objects.sweep([&](fun<void(const ObjectHash &)> f) {
        f(ObjectHash::of(r.root));
        return 0;
    });
    EXPECT_EQ(swept.unentered, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.blobsKept, 1u);
    EXPECT_TRUE(hasBlob(blob, false));

    /* Dropped, the blob goes. */
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; });
    EXPECT_EQ(swept.blobsRemoved, 1u);
    EXPECT_FALSE(hasBlob(blob, false));
}

/* A collection must not abort on a missing object-store subdirectory (a
   user removed one after the store was opened; a crash between the four
   `createDirs` at open): the sweep and the inode table make what they
   iterate.  A fresh process would re-create them at open, so
   this drives the sweep in an open store.  Before: `DirectoryIterator`
   threw on the missing directory and the collection failed. */
TEST_F(LocalStoreObjectsTest, sweepMakesAMissingObjectDirectory)
{
    auto & objects = store->objects;
    auto nar = treeNar({{"a", "one", 'f'}});
    auto r = restore(std::filesystem::path{config->realStoreDir.get()} / "dirs", nar);

    deletePath(objects.dir / "blobs-x");
    deletePath(objects.dir / "tmp");
    ASSERT_FALSE(pathExists(objects.dir / "blobs-x"));
    auto swept = objects.sweep([&](fun<void(const ObjectHash &)> live) {
        live(ObjectHash::of(r.root));
        return 0;
    });
    EXPECT_EQ(swept.blobsKept, 1u);
    EXPECT_TRUE(pathExists(objects.dir / "blobs-x"));
    EXPECT_TRUE(pathExists(objects.dir / "tmp"));

    deletePath(objects.dir / "blobs");
    EXPECT_TRUE(objects.blobInodes().empty());
    EXPECT_TRUE(pathExists(objects.dir / "blobs"));
}

/* A malformed tree body is reported with the tree's identifier. */
TEST_F(LocalStoreObjectsTest, parseTreeBodyErrorsNameTheTree)
{
    auto id = merkle::treeId("");
    auto named = ::testing::ThrowsMessage<Error>(::testing::HasSubstr(GitObjectStore::idString(id)));
    EXPECT_THAT([&] { parseTreeBody(id, "garbage"); }, named);
    EXPECT_THAT([&] { parseTreeBody(id, std::string("777777 name\0", 12) + std::string(32, 'h')); }, named);
}

/* An info naming no content -- no content address, no object hash, no
   asserted NAR hash -- is refused before anything is written: nothing
   below could verify what arrives.  With the NAR hash asserted the same
   add is verified and admitted. */
TEST_F(LocalStoreObjectsTest, addToStoreRefusesAnInfoWithNoContentHash)
{
    StringSink nar;
    dumpString("unverified", nar);
    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-unverified"},
        UnkeyedValidPathInfo{*store, std::nullopt},
    };
    info.narSize = nar.s.size();
    StringSource source{nar.s};
    EXPECT_THAT(
        [&] { store->addToStore(info, source, NoRepair, NoCheckSigs); },
        ::testing::ThrowsMessage<Error>(::testing::HasSubstr("carries no content hash")));
    EXPECT_FALSE(store->isValidPath(info.path));
    EXPECT_FALSE(pathExists(store->toRealPath(info.path)));

    info.assertedNarHash = hashString(HashAlgorithm::SHA256, nar.s);
    StringSource again{nar.s};
    store->addToStore(info, again, NoRepair, NoCheckSigs);
    EXPECT_TRUE(store->isValidPath(info.path));
    EXPECT_EQ(
        store->queryPathInfo(info.path)->objectHash,
        ObjectHash::of(merkle::TreeEntry{merkle::Mode::Regular, merkle::blobId("unverified")}));
}

/* A version-1 signature on a description that asserts no NAR hash (01
   section 9.11, "Signatures"): a path substituted from a NAR-hash-only
   cache, whose row then holds the object hash alone, copied on to a store
   that requires signatures.  The signature can be verified only against
   the stream, so `addToStore` verifies it there and admits the path; a
   forged one is refused after the stream and nothing is left.  Before:
   refused before the stream ("lacks a signature by a trusted key"). */
TEST_F(LocalStoreObjectsTest, versionOneSignatureWithoutAssertedNarHashIsCheckedOnTheStream)
{
    auto secretKey = SecretKey::generate("test-key");
    auto publicKey = secretKey.toPublicKey();
    PublicKeys trusted;
    trusted.insert_or_assign(publicKey.name, publicKey);
    /* The store's keys are the global setting's, read once per store. */
    auto savedKeys = settings.trustedPublicKeys.get();
    Finally restoreKeys{[&] { settings.trustedPublicKeys = savedKeys; }};
    settings.trustedPublicKeys = Strings{publicKey.to_string()};
    config->requireSigs = true;
    LocalSigner signer(std::move(secretKey));

    StringSink nar;
    dumpString("signed", nar);
    auto narHash = hashString(HashAlgorithm::SHA256, nar.s);

    /* The sender's description: the object hash (its row's), the NAR size,
       the version-1 signature, and no NAR hash. */
    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-signed"},
        UnkeyedValidPathInfo{
            *store, ObjectHash::of(merkle::TreeEntry{merkle::Mode::Regular, merkle::blobId("signed")})},
    };
    info.narSize = nar.s.size();
    info.signV1(*store, narHash, signer);
    ASSERT_EQ(info.sigs.size(), 1u);
    ASSERT_FALSE(info.assertedNarHash.has_value());
    ASSERT_TRUE(store->pathInfoIsUntrusted(info)) << "as it stands the description verifies under no fingerprint";

    StringSource source{nar.s};
    store->addToStore(info, source, NoRepair, CheckSigs);
    EXPECT_TRUE(store->isValidPath(info.path));
    EXPECT_EQ(readFile(store->toRealPath(info.path)), "signed");

    /* The row keeps the signature and holds no NAR hash. */
    auto row = store->queryPathInfo(info.path);
    EXPECT_EQ(row->sigs, info.sigs);
    EXPECT_FALSE(row->assertedNarHash.has_value());
    EXPECT_EQ(row->objectHash, info.objectHash);

    /* The forgery: the same key over another NAR hash. */
    ValidPathInfo forged{
        StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-forged"},
        UnkeyedValidPathInfo{
            *store, ObjectHash::of(merkle::TreeEntry{merkle::Mode::Regular, merkle::blobId("signed")})},
    };
    forged.narSize = nar.s.size();
    forged.signV1(*store, hashString(HashAlgorithm::SHA256, "not the NAR"), signer);
    StringSource again{nar.s};
    EXPECT_THAT(
        [&] { store->addToStore(forged, again, NoRepair, CheckSigs); },
        ::testing::ThrowsMessage<Error>(::testing::HasSubstr("lacks a signature by a trusted key")));
    EXPECT_FALSE(store->isValidPath(forged.path));
    EXPECT_FALSE(pathExists(store->toRealPath(forged.path)));
}

/* The registered row's version-1 signature is dead to `checkSignatures`
   as the row stands -- no NAR hash to verify it against -- and alive
   given the walk (`narHashOf`), which `nix store verify` binds.  Before:
   0 either way (and no way to hand the walk in). */
TEST_F(LocalStoreObjectsTest, versionOneSignatureOnARowVerifiesWithTheWalk)
{
    auto secretKey = SecretKey::generate("test-key");
    auto publicKey = secretKey.toPublicKey();
    PublicKeys trusted;
    trusted.insert_or_assign(publicKey.name, publicKey);
    LocalSigner signer(std::move(secretKey));

    StringSink nar;
    dumpString("walked", nar);
    auto narHash = hashString(HashAlgorithm::SHA256, nar.s);
    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-walked"},
        UnkeyedValidPathInfo{
            *store, ObjectHash::of(merkle::TreeEntry{merkle::Mode::Regular, merkle::blobId("walked")})},
    };
    info.narSize = nar.s.size();
    info.signV1(*store, narHash, signer);
    StringSource source{nar.s};
    store->addToStore(info, source, NoRepair, NoCheckSigs);

    auto row = store->queryPathInfo(info.path);
    ASSERT_FALSE(row->assertedNarHash.has_value());
    EXPECT_EQ(row->checkSignatures(*store, trusted), 0u);
    unsigned walks = 0;
    UnkeyedValidPathInfo::NarHashThunk walk{[&]() {
        walks++;
        return narHashOf(*store, info.path);
    }};
    EXPECT_EQ(row->checkSignatures(*store, trusted, walk), 1u);
    EXPECT_EQ(walks, 1u);

    /* A signature by a key not trusted costs no walk. */
    LocalSigner other(SecretKey::generate("other-key"));
    ValidPathInfo untrusted{*row};
    untrusted.sigs.clear();
    untrusted.signV1(*store, narHash, other);
    walks = 0;
    EXPECT_EQ(untrusted.checkSignatures(*store, trusted, walk), 0u);
    EXPECT_EQ(walks, 0u);
}

/* A flat add -- in memory, and spilled to disk -- is entered by the walk
   that also computes what is registered: the file is the blob's inode and
   the row carries the blob's object hash and the NAR size. */
TEST_F(LocalStoreObjectsTest, flatAddIsEnteredAndRegistersItsObjectHash)
{
    auto check = [&](std::string_view bytes, std::string_view name) {
        StringSource source{bytes};
        auto p = store->addToStoreFromDump(
            source,
            name,
            FileSerialisationMethod::Flat,
            ContentAddressMethod::Raw::Flat,
            HashAlgorithm::SHA256,
            {},
            NoRepair);
        auto real = store->toRealPath(p);
        auto info = store->queryPathInfo(p);
        auto blob = merkle::blobId(bytes);
        EXPECT_EQ(info->objectHash, ObjectHash::of(merkle::TreeEntry{merkle::Mode::Regular, blob})) << name;
        StringSink nar;
        dumpString(bytes, nar);
        EXPECT_EQ(info->narSize, nar.s.size()) << name;
        ASSERT_TRUE(hasBlob(blob, false)) << name;
        EXPECT_EQ(st(real).st_ino, st(store->objects.blobFile(blob, false)).st_ino) << name;
        EXPECT_EQ(st(real).st_nlink, 2u) << name;
        EXPECT_EQ(st(real).st_mode & ~S_IFMT, 0444u) << name;
    };
    check("flat in memory", "flat-mem");

    Finally restoreNarBufferSize{[oldSize = settings.getLocalSettings().narBufferSize]() {
        settings.getLocalSettings().narBufferSize.assign(oldSize);
    }};
    settings.getLocalSettings().narBufferSize = 0;
    check("flat spilled to disk", "flat-spilled");
}

/* The lazy migration of a schema-10 row (01 section 9.11, "The database")
   walks the path without rooting it: the collector queries infos under its
   exclusive lock (`keep-derivations`), and a root taken then, through the
   collector's own socket, kept the dead path alive.  Whoever else queries
   holds a root or the shared lock.  The row is current afterwards; a row
   whose path is gone yields `InvalidPath`, the caller's contract. */
TEST_F(LocalStoreObjectsTest, migrationDoesNotRootThePath)
{
    StringSink nar;
    dumpString("an old row", nar);
    StringSource source{nar.s};
    auto p = store->addToStoreFromDump(
        source,
        "old",
        FileSerialisationMethod::NixArchive,
        ContentAddressMethod::Raw::NixArchive,
        HashAlgorithm::SHA256,
        {},
        NoRepair);
    auto objectHash = store->queryPathInfo(p)->objectHash;
    ASSERT_TRUE(objectHash);
    auto narHash = hashString(HashAlgorithm::SHA256, nar.s);

    plantSchema10Row(*store, p, narHash, nar.s.size());
    /* Reopened: the add above rooted its path, and the store's destructor
       removes this process's temp-roots file. */
    store.reset();
    store = std::make_shared<LocalStore>(ref{config});
    ASSERT_FALSE(pathExists(store->fnTempRoots));
    auto migrated = store->queryPathInfo(p);
    EXPECT_EQ(migrated->objectHash, objectHash);
    EXPECT_EQ(migrated->assertedNarHash, narHash);
    /* No temp-roots file was written for the walk. */
    EXPECT_FALSE(pathExists(store->fnTempRoots));

    /* The row is current: a fresh store reads the object hash from the
       column and asserts no NAR hash. */
    store.reset();
    store = std::make_shared<LocalStore>(ref{config});
    auto again = store->queryPathInfo(p);
    EXPECT_EQ(again->objectHash, objectHash);
    EXPECT_FALSE(again->assertedNarHash.has_value());
}

/* A schema-10 row whose files are gone -- removed by hand, or a crash
   before master's `--verify` ran -- is valid in the database, and master
   answered every query from the row; `nix-store --verify` is what finds
   and removes such a row.  The migration leaves it as read (no throw, no
   write), `nix store migrate` counts it failed naming `nix-store
   --verify`, and a row that is gone is still `InvalidPath` (01 §10, *a
   schema-10 row whose files are gone is answered from the database*).
   Before, `queryPathInfo` threw `InvalidPath` for the valid
   row -- `nix path-info`, `-q --references` and `nix copy` failed where
   master answered, and `nix store migrate` counted it "collected
   meanwhile". */
TEST_F(LocalStoreObjectsTest, schema10RowWithMissingFilesIsAnsweredFromTheDatabase)
{
    StringSink nar;
    dumpString("gone", nar);
    StringSource source{nar.s};
    auto p = store->addToStoreFromDump(
        source,
        "gone",
        FileSerialisationMethod::NixArchive,
        ContentAddressMethod::Raw::NixArchive,
        HashAlgorithm::SHA256,
        {},
        NoRepair);
    auto narHash = hashString(HashAlgorithm::SHA256, nar.s);
    auto narColumn = narHash.to_string(HashFormat::Base16, true);
    plantSchema10Row(*store, p, narHash, nar.s.size());
    deletePath(store->toRealPath(p));

    auto info = store->queryPathInfo(p);
    EXPECT_FALSE(info->objectHash.has_value());
    EXPECT_EQ(info->assertedNarHash, narHash);
    EXPECT_EQ(info->narSize, nar.s.size());
    EXPECT_EQ(readHashColumn(*store, p), narColumn) << "nothing written";
    try {
        store->migratePathInfo(p);
        FAIL() << "the migration must report the missing files";
    } catch (InvalidPath &) {
        FAIL() << "the row is valid";
    } catch (Error & e) {
        EXPECT_THAT(e.what(), ::testing::HasSubstr("nix-store --verify"));
    }
    EXPECT_EQ(readHashColumn(*store, p), narColumn);

    /* `nix-store --verify` removes the row; the path is then invalid. */
    store->verifyStore(/*checkContents=*/false, NoRepair);
    EXPECT_FALSE(store->isValidPath(p));
    EXPECT_THROW(store->queryPathInfo(p), InvalidPath);
    EXPECT_THROW(store->migratePathInfo(p), InvalidPath);
}

/* A schema-10 row whose files an older Nix wrote and something modified
   since, at equal length (bit rot, a write through a file made writable):
   the NAR hash the row asserts is checked on the same walk that computes
   the object hash, as `--load-db` checks it, and on a mismatch nothing is
   written -- the row is answered as the database has it, and the verifiers
   find the modification through the NAR hash, as master's did (01 §10, *a
   schema-10 row's migration checks the NAR hash the row asserts*).
   Before, the walk wrote the object hash of the modified
   bytes into the column and every verifier then passed the path
   (master: "was modified!"; the branch: exit 0). */
TEST_F(LocalStoreObjectsTest, modifiedSchema10RowIsNotMigrated)
{
    auto nar = treeNar({{"a", "one\n", 'f'}, {"d/b", "two\n", 'f'}});
    StringSource source{nar};
    auto p = store->addToStoreFromDump(
        source,
        "old",
        FileSerialisationMethod::NixArchive,
        ContentAddressMethod::Raw::NixArchive,
        HashAlgorithm::SHA256,
        {},
        NoRepair);
    auto narHash = hashString(HashAlgorithm::SHA256, nar);
    auto narColumn = narHash.to_string(HashFormat::Base16, true);
    plantSchema10Row(*store, p, narHash, nar.size());
    ASSERT_EQ(readHashColumn(*store, p), narColumn);

    /* The path's files as an older store holds them (no links into the
       object store), one modified at equal length. */
    auto a = store->toRealPath(p) / "a";
    unlinkInPlace(a);
    chmod(a, 0644);
    writeFile(a, "xne\n");
    chmod(a, 0444);
    ASSERT_EQ(st(a).st_nlink, 1u);

    auto info = store->queryPathInfo(p);
    EXPECT_FALSE(info->objectHash.has_value()) << "the row as the database has it";
    EXPECT_EQ(info->assertedNarHash, narHash);
    EXPECT_EQ(readHashColumn(*store, p), narColumn) << "nothing written";

    /* The verifiers: `contentMismatch` compares the NAR hash, `verifyStore
       --check-contents` reports and writes nothing, `nix store migrate`
       counts it failed with the message. */
    auto mismatch = store->contentMismatch(*info);
    ASSERT_TRUE(mismatch.has_value());
    EXPECT_EQ(mismatch->first, narHash.to_string(HashFormat::Nix32, true));
    EXPECT_TRUE(store->verifyStore(/*checkContents=*/true, NoRepair)) << "an error is reported";
    EXPECT_EQ(readHashColumn(*store, p), narColumn) << "the verifier writes nothing either";
    try {
        store->migratePathInfo(p);
        FAIL() << "the migration must refuse the modified path";
    } catch (InvalidPath &) {
        FAIL() << "the row is valid";
    } catch (Error & e) {
        EXPECT_THAT(e.what(), ::testing::HasSubstr("was modified"));
    }
    EXPECT_EQ(readHashColumn(*store, p), narColumn);

    /* The bytes restored: the same walk migrates the row. */
    chmod(a, 0644);
    writeFile(a, "one\n");
    chmod(a, 0444);
    store->clearPathInfoCache();
    auto migrated = store->queryPathInfo(p);
    ASSERT_TRUE(migrated->objectHash.has_value());
    EXPECT_EQ(*migrated->objectHash, ObjectHash::of(gitHash(store->toRealPath(p))));
    EXPECT_EQ(readHashColumn(*store, p), migrated->objectHash->render());
}

/* The collector under `keep-derivations` (the default): a dead derivation
   is visited with its outputs, each output's info queried under the
   exclusive GC lock; an output on a schema-10 row is migrated by that
   query.  Before, the migration rooted the output through the collector's
   own socket and both survived; now both go. */
TEST_F(LocalStoreObjectsTest, gcUnderKeepDerivationsDeletesASchema10Output)
{
    ASSERT_TRUE(config->getLocalSettings().getGCSettings().keepDerivations);
    /* lsof is slow and not needed here (local-gc.cc). */
    setenv("_NIX_TEST_NO_LSOF", "1", 1);

    /* A fixed-output derivation and its output, both valid, neither a root. */
    std::string bytes = "an output";
    StringSource source{bytes};
    auto outPath = store->addToStoreFromDump(
        source,
        "fixed-out",
        FileSerialisationMethod::Flat,
        ContentAddressMethod::Raw::Flat,
        HashAlgorithm::SHA256,
        {},
        NoRepair);
    Derivation drv;
    drv.name = "fixed-out";
    drv.platform = "nowhere";
    drv.builder = "/bin/sh";
    drv.env["out"] = store->printStorePath(outPath);
    drv.outputs.insert_or_assign(
        "out",
        derivation::Output{derivation::Output::CAFixed{
            .ca =
                ContentAddress{
                    .method = ContentAddressMethod::Raw::Flat,
                    .hash = hashString(HashAlgorithm::SHA256, bytes),
                },
        }});
    auto drvPath = store->writeDerivation(drv);
    ASSERT_TRUE(store->queryPartialDerivationOutputMap(drvPath).at("out") == outPath);

    /* The output's row as an older Nix left it: the derivation as its
       deriver, the NAR hash in the hash column. */
    {
        SQLite db(store->dbDir / "db.sqlite", {.mode = SQLiteOpenMode::NoCreate, .useWAL = settings.useSQLiteWAL});
        SQLiteStmt stmt{db, "update ValidPaths set deriver = ? where path = ?"};
        stmt.use().apply(store->printStorePath(drvPath)).apply(store->printStorePath(outPath)).exec();
    }
    StringSink nar;
    dumpString(bytes, nar);
    plantSchema10Row(*store, outPath, hashString(HashAlgorithm::SHA256, nar.s), nar.s.size());
    ASSERT_TRUE(store->queryPathInfo(outPath)->deriver == drvPath);
    plantSchema10Row(*store, outPath, hashString(HashAlgorithm::SHA256, nar.s), nar.s.size());

    /* Reopened: the adds above rooted their paths in this process's
       temp-roots file, which the collector reads too; the store's
       destructor removes it. */
    store.reset();
    store = std::make_shared<LocalStore>(ref{config});
    ASSERT_FALSE(pathExists(store->fnTempRoots));

    GCOptions options;
    options.action = GCOptions::gcDeleteDead;
    GCResults results;
    store->collectGarbage(options, results);

    EXPECT_FALSE(store->isValidPath(outPath));
    EXPECT_FALSE(store->isValidPath(drvPath));
    EXPECT_FALSE(pathExists(store->toRealPath(outPath)));
    EXPECT_FALSE(pathExists(store->toRealPath(drvPath)));
    EXPECT_FALSE(pathExists(store->fnTempRoots));
}

/* The git method admits SHA-256 only (01 section 9.9, "The algorithm";
   section 10, *The git method is SHA-256 only*).  Refused, naming the
   remedy, at every entry
   that creates a git-addressed object or name: the dump route (the
   daemon's entry), the slow route (`nix store add`), the dry run, a binary
   cache's upload, and the import of a description whose content address
   is the SHA-1 form, which no walk here can check.  SHA-256 is admitted
   on each. */
TEST_F(LocalStoreObjectsTest, gitMethodRefusesSha1AtEveryCreationEntry)
{
    auto refused = [](auto && f) {
        EXPECT_THAT(
            f,
            ::testing::ThrowsMessage<Error>(
                ::testing::HasSubstr("the git content-address method admits SHA-256 only")));
    };
    auto acc = make_ref<MemorySourceAccessor>();
    acc->addFile(CanonPath{"/hello"}, "Hello World\n");
    StringSink nar;
    acc->dumpPath(CanonPath::root, nar);

    refused([&] {
        StringSource dump{nar.s};
        store->addToStoreFromDump(
            dump,
            "x",
            FileSerialisationMethod::NixArchive,
            ContentAddressMethod::Raw::Git,
            HashAlgorithm::SHA1,
            {},
            NoRepair);
    });
    refused([&] { store->addToStoreSlow("x", {acc}, ContentAddressMethod::Raw::Git, HashAlgorithm::SHA1); });
    refused([&] { store->computeStorePath("x", {acc}, ContentAddressMethod::Raw::Git, HashAlgorithm::SHA1); });

    auto cache = openStore("file://" + (tempStoreDir.path() / "cache").string());
    refused([&] {
        StringSource dump{nar.s};
        cache->addToStoreFromDump(
            dump,
            "x",
            FileSerialisationMethod::NixArchive,
            ContentAddressMethod::Raw::Git,
            HashAlgorithm::SHA1,
            {},
            NoRepair);
    });

    /* The import: git's own SHA-1 tree id of the tree, so that the
       description is one an older Nix made and verified. */
    auto blob = hashString(HashAlgorithm::SHA1, std::string("blob 12\0", 8) + "Hello World\n");
    std::string body = std::string("100644 hello\0", 13);
    body.append(reinterpret_cast<const char *>(blob.hash), blob.hashSize);
    auto tree = hashString(HashAlgorithm::SHA1, fmt("tree %d", body.size()) + std::string("\0", 1) + body);
    /* `git rev-parse HEAD:hash-path` in a SHA-1 repository holding that one
       file (the record of this step, 05 section 42, has the command). */
    EXPECT_EQ(tree.to_string(HashFormat::Base16, false), "117c62a8c5e01758bd284126a6af69deab9dbbe2");
    ValidPathInfo info{
        store->makeFixedOutputPath("x", FixedOutputInfo{.method = FileIngestionMethod::Git, .hash = tree}),
        UnkeyedValidPathInfo{*store, ObjectHash::of(objectHashOf(*acc, CanonPath::root).root)},
    };
    info.ca = ContentAddress{.method = ContentAddressMethod::Raw::Git, .hash = tree};
    info.narSize = nar.s.size();
    refused([&] {
        StringSource source{nar.s};
        store->addToStore(info, source, NoRepair, NoCheckSigs);
    });
    EXPECT_FALSE(store->isValidPath(info.path));
    EXPECT_FALSE(pathExists(store->toRealPath(info.path)));

    auto p = store->addToStoreSlow("x", {acc}, ContentAddressMethod::Raw::Git, HashAlgorithm::SHA256).path;
    EXPECT_TRUE(store->isValidPath(p));
    EXPECT_EQ(store->queryPathInfo(p)->ca->hash.algo, HashAlgorithm::SHA256);
}

/* A row an older Nix made under the SHA-1 form is read, verified and
   collected: the collector reads every row it visits (gc.cc,
   `topoSortPaths`) and the verifier every row, so the content-address
   reader tolerates the form and only creation refuses it (01 section 10,
   *The git method is SHA-256 only*).  The row is planted: no Nix this test can drive writes
   one. */
TEST_F(LocalStoreObjectsTest, sha1GitRowIsReadVerifiedAndCollected)
{
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    auto acc = make_ref<MemorySourceAccessor>();
    acc->addFile(CanonPath{"/hello"}, "Hello World\n");
    auto p = store->addToStoreSlow("x", {acc}, ContentAddressMethod::Raw::Git, HashAlgorithm::SHA256).path;
    auto ca = "fixed:git:sha1:" + hashString(HashAlgorithm::SHA1, "an old tree").to_string(HashFormat::Nix32, false);
    plantCaColumn(*store, p, ca);

    auto info = store->queryPathInfo(p);
    ASSERT_TRUE(info->ca.has_value());
    EXPECT_EQ(info->ca->method, ContentAddressMethod::Raw::Git);
    EXPECT_EQ(info->ca->hash.algo, HashAlgorithm::SHA1);
    EXPECT_EQ(info->ca->render(), ca);

    /* The object hash is what the row is checked against. */
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, NoRepair));

    /* Reopened without this process's temp roots, then collected. */
    store.reset();
    store = std::make_shared<LocalStore>(ref{config});
    GCOptions options;
    options.action = GCOptions::gcDeleteDead;
    GCResults results;
    store->collectGarbage(options, results);
    EXPECT_FALSE(store->isValidPath(p));
}

/* The collector reads a dead row as the database has it (01 section 10,
   *the collector reads a dead row without migrating it*): `topoSortPaths`
   needs a row's references and the `keep-derivations` check its deriver,
   both row fields, so a schema-10 row on its way to deletion is not
   migrated by the walk that reads and hashes every byte of the path
   (`walkOldRow`) -- a cost proportional to the garbage, paid once per
   dead old row (`05` section 6, the E5 measurement).  The probe is a FIFO
   planted in the dead path: the serialising walk refuses it
   (`tree-traversal.hh`, "has an unsupported type"), the deletion unlinks
   it.  Before, `collectGarbage` failed with that error at the first dead
   old row it sorted; the walk is otherwise unobservable once the path is
   gone. */
TEST_F(LocalStoreObjectsTest, deadSchema10RowIsDeletedWithoutAWalk)
{
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    auto nar = treeNar({{"a", "aaa", 'f'}});
    StringSource source{nar};
    auto p = store->addToStoreFromDump(
        source,
        "dead",
        FileSerialisationMethod::NixArchive,
        ContentAddressMethod::Raw::NixArchive,
        HashAlgorithm::SHA256,
        {},
        NoRepair);
    plantSchema10Row(*store, p, hashString(HashAlgorithm::SHA256, nar), nar.size());

    auto realPath = store->toRealPath(p);
    chmod(realPath, 0755);
    ASSERT_EQ(mkfifo((realPath / "fifo").c_str(), 0644), 0) << strerror(errno);
    chmod(realPath, 0555);

    /* Reopened without this process's temp roots, then collected. */
    store.reset();
    store = std::make_shared<LocalStore>(ref{config});
    GCOptions options;
    options.action = GCOptions::gcDeleteDead;
    GCResults results;
    store->collectGarbage(options, results);
    EXPECT_FALSE(store->isValidPath(p));
    EXPECT_FALSE(pathExists(realPath));
}

/* The second route of 01 §9.10 ("One route", its second sentence; `04`
   §1.7): a tree the store holds is made from `trees/` and `blobs/` by
   `mkdirat`, `linkat` and `symlinkat` -- every regular file the blob's
   link (law 3), nothing read from the source, nothing written (the
   counting hooks), the NAR size exact and the path the one route's byte
   for byte (law 2). */
TEST_F(LocalStoreObjectsTest, materialiseFromObjectsLinksEveryFileAndReadsNothing)
{
    Nodes nodes{
        {"a", "one", 'f'},
        {"d/b", "two", 'f'},
        {"d/e/c", "one", 'f'},
        {"d/e/x", "one", 'x'},
        {"empty", "", 'f'},
        {"d/link", "b", 'l'}};
    auto nar = treeNar(nodes);
    auto tree = treeAccessor(nodes);
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};

    /* The one route enters the tree's objects. */
    auto t1 = realStoreDir / "m1";
    auto r1 = restore(t1, nar);
    EXPECT_EQ(filesCreated, 4u);

    /* The second: the root is named and held, so the whole tree is made
       from the objects; the source is asked for the root's status alone. */
    auto counting = make_ref<CountingAccessor>(tree);
    auto t2 = realStoreDir / "m2";
    auto r2 = materialise(t2, SourcePath(counting), everyNode(tree));
    EXPECT_EQ(filesCreated, 0u);
    EXPECT_EQ(counting->reads, 0u);
    EXPECT_EQ(counting->listings, 0u);
    EXPECT_EQ(counting->links, 0u);
    EXPECT_EQ(counting->stats, 1u);
    EXPECT_EQ(r2.root, r1.root);
    EXPECT_EQ(r2.narSize, nar.size());
    EXPECT_TRUE(r2.narSizeExact);
    StringSink again;
    dumpPath(t2, again);
    EXPECT_EQ(again.s, nar);
    EXPECT_EQ(gitHash(t2), r1.root);

    /* Law 3: each regular file is the blob's inode, shared with the first
       path; the modes and times canonical as the restore sink makes them. */
    for (auto & rel : {"a", "d/b", "d/e/c", "d/e/x", "empty"}) {
        EXPECT_EQ(st(t2 / rel).st_ino, st(t1 / rel).st_ino) << rel;
        EXPECT_EQ(st(t2 / rel).st_mtime, 1) << rel;
    }
    EXPECT_EQ(st(t2 / "a").st_mode & ~S_IFMT, 0444u);
    EXPECT_EQ(st(t2 / "d/e/x").st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(st(t2).st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(st(t2 / "d/e").st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(st(t2 / "d/e").st_mtime, 1);
    EXPECT_EQ(st(t2 / "d/link").st_mtime, 1);
    EXPECT_EQ(readLink(t2 / "d/link"), "b");
    EXPECT_EQ(count("blobs"), 4u);
    EXPECT_EQ(count("blobs-x"), 1u);
    EXPECT_EQ(count("trees"), 3u);

    /* A one-file root: the file is the destination itself, linked. */
    auto file = make_ref<MemorySourceAccessor>();
    file->addFile(CanonPath::root, "two");
    auto t3 = realStoreDir / "m3";
    auto r3 = materialise(t3, SourcePath(file), everyNode(file));
    EXPECT_EQ(filesCreated, 0u);
    EXPECT_EQ(r3.root, (merkle::TreeEntry{merkle::Mode::Regular, merkle::blobId("two")}));
    EXPECT_EQ(r3.narSize, narOf(*file).size());
    EXPECT_EQ(st(t3).st_ino, st(t1 / "d/b").st_ino);
}

/* The same tree with one file changed inside `d`: the root and `d` are
   not held, so the driver descends into them and `d`'s files are read as
   the one route reads them -- one written, the other linked -- while `e`,
   held, is made from the objects and nothing beneath it is read.  A fresh
   tree costs the route one lookup per directory, never one per file. */
TEST_F(LocalStoreObjectsTest, materialiseFromObjectsReadsOnlyWhatTheStoreLacks)
{
    auto nodes = [](std::string_view b) {
        return Nodes{
            {"a", "one", 'f'},
            {"d/b", std::string(b), 'f'},
            {"d/c", "three", 'f'},
            {"e/x", "one", 'x'},
            {"e/y", "four", 'f'},
            {"e/l", "y", 'l'}};
    };
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto t1 = realStoreDir / "w1";
    restore(t1, treeNar(nodes("two")));
    EXPECT_EQ(filesCreated, 5u);

    auto tree = treeAccessor(nodes("TWO"));
    auto counting = make_ref<CountingAccessor>(tree);
    auto t2 = realStoreDir / "w2";
    auto r2 = materialise(t2, SourcePath(counting), everyNode(tree));
    EXPECT_EQ(filesCreated, 1u);
    EXPECT_EQ(counting->reads, 3u);    /* a, d/b and d/c: the files of the two directories not held */
    EXPECT_EQ(counting->listings, 2u); /* the root and d */
    EXPECT_EQ(counting->links, 0u);    /* e/l came from its blob */
    EXPECT_EQ(r2.root, gitHash(t2));
    EXPECT_EQ(r2.narSize, treeNar(nodes("TWO")).size());
    StringSink again;
    dumpPath(t2, again);
    EXPECT_EQ(again.s, treeNar(nodes("TWO")));
    EXPECT_EQ(readFile(t2 / "d/b"), "TWO");
    EXPECT_EQ(st(t2 / "d/b").st_ino, st(store->objects.blobFile(merkle::blobId("TWO"), false)).st_ino);
    EXPECT_EQ(st(t2 / "d/c").st_ino, st(t1 / "d/c").st_ino);
    EXPECT_EQ(st(t2 / "e/x").st_ino, st(t1 / "e/x").st_ino);
    EXPECT_EQ(st(t2 / "e/y").st_ino, st(t1 / "e/y").st_ino);
    EXPECT_EQ(st(t2 / "d").st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(st(t2 / "e").st_mode & ~S_IFMT, 0555u);
    /* The new root and `d` trees were entered by the fallback, `e`'s was
       there. */
    EXPECT_TRUE(hasTree(r2.root.hash));
    EXPECT_EQ(count("trees"), 5u);

    /* With nothing named, the route is the one route under the pull
       driver: everything read, everything held linked, nothing written. */
    auto counting2 = make_ref<CountingAccessor>(tree);
    auto t3 = realStoreDir / "w3";
    auto r3 = materialise(t3, SourcePath(counting2), noNode());
    EXPECT_EQ(filesCreated, 0u);
    EXPECT_EQ(counting2->reads, 5u);
    EXPECT_EQ(r3.root, r2.root);
    EXPECT_EQ(r3.narSize, r2.narSize);
    StringSink again3;
    dumpPath(t3, again3);
    EXPECT_EQ(again3.s, treeNar(nodes("TWO")));
}

/* The fallbacks, each planted.  The unit that falls back is the subtree
   the naming named and the store could not complete: here the root is
   named and held, so a missing object anywhere beneath it sends the driver
   into the source from the root, where each directory is tried again on
   its own -- bounded by one read of the tree, the one route's cost.  A
   tree body the store lacks is entered again by the fallback; a body that
   does not hash to its name is refused and left for `--verify
   --check-contents --repair`; a blob file whose bytes are not its
   identifier's is refused under repair (`link` re-hashes it, K2) and the
   file written from the source, and without repair trusted as the on-disk
   walk trusts a blob's inode (K8): the route has no measure of the file
   but the store's own.  Every case ends in the same bytes. */
TEST_F(LocalStoreObjectsTest, materialiseFromObjectsFallsBackOnAMissingOrCorruptObject)
{
    Nodes nodes{{"a", "one", 'f'}, {"d/b", "two", 'f'}, {"d/c", "three", 'f'}, {"e/y", "four", 'f'}};
    auto nar = treeNar(nodes);
    auto tree = treeAccessor(nodes);
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto r1 = restore(realStoreDir / "f1", nar);
    auto root = readTree(r1.root.hash);
    auto dId = root.at("d/").hash;
    auto eId = root.at("e/").hash;

    /* A missing subtree body: the root's materialisation stops at `d`, the
       driver lists the root and `d` and reads their files -- linked, every
       blob being held -- while `e` is made from its objects; `d`'s body
       comes back. */
    std::filesystem::remove(treeFile(dId));
    {
        auto counting = make_ref<CountingAccessor>(tree);
        auto r = materialise(realStoreDir / "f2", SourcePath(counting), everyNode(tree));
        EXPECT_EQ(filesCreated, 0u);
        EXPECT_EQ(counting->reads, 3u);    /* a, d/b, d/c */
        EXPECT_EQ(counting->listings, 2u); /* the root, d */
        EXPECT_EQ(r.root, r1.root);
        EXPECT_EQ(r.narSize, nar.size());
        StringSink again;
        dumpPath(realStoreDir / "f2", again);
        EXPECT_EQ(again.s, nar);
        EXPECT_TRUE(hasTree(dId));
    }

    /* A corrupt body that parses -- `e`'s body with `y` pointing at the
       blob of "one", which the store holds -- is refused by the identifier
       check alone: trusted, it would link `e/y` to the wrong bytes under
       the right name.  Not removed; the subtree is read from the source.
       Then a body that does not parse, refused as well. */
    auto eBody = readFile(treeFile(eId));
    auto plant = [&](const std::string & body) {
        chmod(treeFile(eId), 0644);
        writeFile(treeFile(eId), body);
        chmod(treeFile(eId), 0444);
    };
    auto misnaming = readTree(eId);
    misnaming.at("y") = {merkle::Mode::Regular, merkle::blobId("one")};
    auto misnamingBody = merkle::serialiseTree(misnaming);
    ASSERT_NE(merkle::treeId(misnamingBody), eId);
    ASSERT_EQ(parseTreeBody(eId, misnamingBody).at("y").hash, merkle::blobId("one"));
    auto unparseable = eBody;
    unparseable[0] ^= 1;
    unsigned n = 3;
    for (auto & corrupt : {misnamingBody, unparseable}) {
        plant(corrupt);
        auto counting = make_ref<CountingAccessor>(tree);
        auto dst = realStoreDir / fmt("f%d", n++);
        auto r = materialise(dst, SourcePath(counting), everyNode(tree));
        EXPECT_EQ(filesCreated, 0u);
        EXPECT_EQ(counting->reads, 2u);    /* a, e/y; d from its objects */
        EXPECT_EQ(counting->listings, 2u); /* the root, e */
        EXPECT_EQ(r.root, r1.root);
        EXPECT_EQ(readFile(dst / "e/y"), "four");
        StringSink again;
        dumpPath(dst, again);
        EXPECT_EQ(again.s, nar);
        EXPECT_EQ(readFile(treeFile(eId)), corrupt);
    }
    plant(eBody);

    /* A corrupt blob, same length.  Under repair the store's file is
       re-hashed before it is linked, removed, and the file written from
       the source; the new blob file is sound. */
    auto two = store->objects.blobFile(merkle::blobId("two"), false);
    chmod(two, 0644);
    writeFile(two, "twX");
    chmod(two, 0444);
    {
        auto counting = make_ref<CountingAccessor>(tree);
        auto r = materialise(realStoreDir / "f5", SourcePath(counting), everyNode(tree), Repair);
        EXPECT_EQ(filesCreated, 1u);
        EXPECT_EQ(counting->reads, 3u);    /* a, d/b, d/c; e from its objects, re-hashed */
        EXPECT_EQ(counting->listings, 2u); /* the root, d */
        EXPECT_EQ(r.root, r1.root);
        EXPECT_EQ(readFile(realStoreDir / "f5/d/b"), "two");
        EXPECT_EQ(readFile(two), "two");
        StringSink again;
        dumpPath(realStoreDir / "f5", again);
        EXPECT_EQ(again.s, nar);
    }

    /* Without repair the store's file is trusted: it is linked, corrupt
       bytes and all, as every path already linking it shows them; the
       verifier's business (`--verify --check-contents`). */
    chmod(two, 0644);
    writeFile(two, "twX");
    chmod(two, 0444);
    {
        auto counting = make_ref<CountingAccessor>(tree);
        auto r = materialise(realStoreDir / "f6", SourcePath(counting), everyNode(tree));
        EXPECT_EQ(filesCreated, 0u);
        EXPECT_EQ(counting->reads, 0u);
        EXPECT_EQ(r.root, r1.root);
        EXPECT_EQ(readFile(realStoreDir / "f6/d/b"), "twX");
        EXPECT_EQ(st(realStoreDir / "f6/d/b").st_ino, st(two).st_ino);
    }
}

/* `LocalStore::materialise` registers what `addToStore` would: the same
   path, content address, object hash and NAR size; a second call finds
   the path valid and makes nothing. */
TEST_F(LocalStoreObjectsTest, materialiseRegistersWhatAddToStoreWould)
{
    Nodes nodes{{"a", "one", 'f'}, {"d/b", "two", 'f'}, {"d/l", "b", 'l'}};
    auto tree = treeAccessor(nodes);
    auto nar = treeNar(nodes);
    auto git = ContentAddressMethod::Raw::Git;

    ASSERT_TRUE(store->materialisesFromObjects());
    auto [expected, hash] = store->computeStorePath("src", SourcePath(tree), git, HashAlgorithm::SHA256, {});
    auto p = store->materialise("src", SourcePath(tree), defaultPathFilter, NoRepair, everyNode(tree));
    EXPECT_EQ(p, expected);
    ASSERT_TRUE(store->isValidPath(p));
    auto info = store->queryPathInfo(p);
    EXPECT_EQ(info->narSize, nar.size());
    ASSERT_TRUE(info->objectHash);
    EXPECT_EQ(*info->objectHash, ObjectHash::of(objectHashOf(*tree, CanonPath::root).root));
    ASSERT_TRUE(info->ca);
    EXPECT_EQ(info->ca->method, git);
    EXPECT_EQ(info->ca->hash, hash);
    StringSink again;
    dumpPath(store->toRealPath(p), again);
    EXPECT_EQ(again.s, nar);

    /* The same tree by the one route names the same path. */
    auto p2 = store->Store::addToStore("src", SourcePath(tree), git, HashAlgorithm::SHA256, {}, defaultPathFilter);
    EXPECT_EQ(p2, p);
    auto p3 = store->materialise("src", SourcePath(tree), defaultPathFilter, NoRepair, everyNode(tree));
    EXPECT_EQ(p3, p);
    /* No temporary left in the store. */
    for (auto & e : DirectoryIterator{config->realStoreDir.get()})
        EXPECT_FALSE(hasPrefix(e.path().filename().string(), "tmp-")) << e.path();
}

/* The case hack (`use-case-hack`, on by default on macOS) renames the
   second of two names differing only in case on disk; the NAR restorer
   does it on the one route, and the second route does the same both from
   a tree body (`Foo` and `foo` held) and on the pull driver's writing path
   (nothing named): the on-disk names, the dump and the inodes are the one
   route's.  The hack forced on, so the example runs on Linux too. */
TEST_F(LocalStoreObjectsTest, materialiseAppliesTheCaseHackAsTheRestorerDoes)
{
    WithCaseHack on{true};
    Nodes nodes{{"d/Foo", "one", 'f'}, {"d/foo", "two", 'f'}, {"d/B", "three", 'f'}, {"d/b", "four", 'x'}};
    auto nar = treeNar(nodes);
    auto tree = treeAccessor(nodes);
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto hacked = [](std::string_view name) { return std::string(name) + std::string(caseHackSuffix) + "1"; };

    auto t1 = realStoreDir / "h1";
    auto r1 = restore(t1, nar);
    ASSERT_TRUE(pathExists(t1 / "d/Foo"));
    ASSERT_TRUE(pathExists(t1 / "d" / hacked("foo")));
    ASSERT_TRUE(pathExists(t1 / "d" / hacked("b")));

    for (auto & [label, namer] : {std::pair{"held", everyNode(tree)}, std::pair{"read", noNode()}}) {
        auto t2 = realStoreDir / label;
        auto r2 = materialise(t2, SourcePath(tree), namer);
        EXPECT_EQ(filesCreated, 0u) << label;
        EXPECT_EQ(r2.root, r1.root) << label;
        EXPECT_EQ(r2.narSize, nar.size()) << label;
        StringSink again;
        dumpPath(t2, again);
        EXPECT_EQ(again.s, nar) << label;
        for (auto & rel : {std::string("d/Foo"), "d/" + hacked("foo"), std::string("d/B"), "d/" + hacked("b")}) {
            ASSERT_TRUE(pathExists(t2 / rel)) << label << " " << rel;
            EXPECT_EQ(st(t2 / rel).st_ino, st(t1 / rel).st_ino) << label << " " << rel;
        }
        EXPECT_EQ(readFile(t2 / "d" / hacked("foo")), "two") << label;
    }
}

/* Law 2 of 01 §9.10 over the second route, on generated trees: whatever
   the naming names -- everything, nothing, or some directories -- the
   materialised path dumps to the one route's NAR byte for byte, its root
   and NAR size are the one route's, every regular file is the blob's
   inode (law 3) and, the store holding every blob, nothing is written.
   Names collide in case, so the case hack renames on both routes alike
   (the hack forced on, as on macOS). */
RC_GTEST_FIXTURE_PROP(LocalStoreObjectsTest, prop_materialised_path_is_the_restored_path, ())
{
    WithCaseHack on{true};
    FileGenSpec spec{
        .names = {"a", "A", "b.c", "B.C"},
        .contents = rc::gen::element(std::string(""), std::string("x"), std::string("yy"), std::string(9, 'q')),
        .targets = rc::gen::element(std::string("t"), std::string("t/uv")),
    };
    auto tree = make_ref<MemorySourceAccessor>();
    tree->root = *genFile(3, spec);
    auto nar = narOf(*tree);
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};

    auto reference = realStoreDir / "ref";
    auto r1 = restore(reference, nar);
    RC_ASSERT(r1.narSize == nar.size());

    std::vector<std::pair<std::string, LocalStore::TreeNamer>> namers;
    namers.emplace_back("every", everyNode(tree));
    namers.emplace_back("none", noNode());
    namers.emplace_back(
        "some", [tree](const CanonPath & to, const SourceAccessor::Stat &) -> std::optional<merkle::TreeEntry> {
            if (*rc::gen::arbitrary<bool>())
                return objectHashOf(*tree, to).root;
            return std::nullopt;
        });
    for (auto & [label, namer] : namers) {
        auto dst = realStoreDir / label;
        auto r = materialise(dst, SourcePath(tree), namer);
        RC_ASSERT(filesCreated == 0u);
        RC_ASSERT(r.root == r1.root);
        RC_ASSERT(r.narSize == r1.narSize);
        RC_ASSERT(r.narSizeExact);
        StringSink again;
        dumpPath(dst, again);
        RC_ASSERT(again.s == nar);
        auto files = regularFilesUnder(reference);
        RC_ASSERT(files == regularFilesUnder(dst));
        for (auto & f : files) {
            auto under = [&](const std::filesystem::path & p) { return f.empty() ? p : p / f; };
            RC_ASSERT(st(under(reference)).st_ino == st(under(dst)).st_ino);
        }
    }
}

/* The ingestion's own collection (`min-free`, `autoGC()` inside the add)
   must not sweep the objects the add is writing: the collector runs before
   the restore, as on master it ran before the link pass.  Before, it ran
   between the restore and the registration, and the first add that
   triggered it lost its trees and its symlink's blob (01 section 10, *the
   ingestion's `autoGC()` runs before the restore*).  The planted garbage
   proves a collection ran. */
class LocalStoreAutoGCTest : public LocalStoreObjectsTest
{
protected:
    std::filesystem::path garbage;
    std::filesystem::path fakeFree;

    void SetUp() override
    {
        LocalStoreObjectsTest::SetUp();
        /* The collector's trigger: the free space read from the test hook's
           file, below `min-free`; `max-free` bounds what it frees; the
           check interval zero, so that a second add in the same test is
           checked too. */
        fakeFree = tempStoreDir.path() / "fake-free";
        writeFile(fakeFree, "1000");
        setenv("_NIX_TEST_FREE_SPACE_FILE", fakeFree.c_str(), 1);
        setenv("_NIX_TEST_NO_LSOF", "1", 1);
        auto & gc = settings.getLocalSettings().getGCSettings();
        savedMinFree = gc.minFree.get();
        savedMaxFree = gc.maxFree.get();
        savedInterval = gc.minFreeCheckInterval.get();
        gc.minFree = 1000000;
        gc.maxFree = 2000000;
        gc.minFreeCheckInterval = 0;
        garbage = std::filesystem::path{config->realStoreDir.get()} / "not-a-store-path";
        writeFile(garbage, "dead");
    }

    void TearDown() override
    {
        unsetenv("_NIX_TEST_FREE_SPACE_FILE");
        auto & gc = settings.getLocalSettings().getGCSettings();
        gc.minFree.assign(savedMinFree);
        gc.maxFree.assign(savedMaxFree);
        gc.minFreeCheckInterval.assign(savedInterval);
        LocalStoreObjectsTest::TearDown();
    }

private:
    uint64_t savedMinFree = 0, savedMaxFree = 0, savedInterval = 0;
};

TEST_F(LocalStoreAutoGCTest, addToStoresOwnCollectionKeepsItsObjects)
{
    auto nar = treeWithSymlink();
    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-auto-gc"},
        UnkeyedValidPathInfo{*store, std::nullopt},
    };
    info.assertedNarHash = hashString(HashAlgorithm::SHA256, nar);
    info.narSize = nar.size();
    StringSource source{nar};
    store->addToStore(info, source, NoRepair, NoCheckSigs);

    ASSERT_FALSE(pathExists(garbage)) << "a collection ran inside the add";
    auto row = store->queryPathInfo(info.path);
    ASSERT_TRUE(row->objectHash.has_value());
    EXPECT_TRUE(hasTree(row->objectHash->hash)) << "the root tree survived the add's own collection";
    EXPECT_TRUE(hasBlob(merkle::blobId("b"), false)) << "the symlink's blob survived it";
    EXPECT_EQ(count("trees"), 2u);
}

TEST_F(LocalStoreAutoGCTest, spilledAddToStoreFromDumpsOwnCollectionKeepsItsObjects)
{
    Finally restoreNarBufferSize{[oldSize = settings.getLocalSettings().narBufferSize]() {
        settings.getLocalSettings().narBufferSize.assign(oldSize);
    }};
    settings.getLocalSettings().narBufferSize = 0;

    auto nar = treeWithSymlink();
    StringSource source{nar};
    auto p = store->addToStoreFromDump(
        source,
        "auto-gc-spilled",
        FileSerialisationMethod::NixArchive,
        ContentAddressMethod::Raw::NixArchive,
        HashAlgorithm::SHA256,
        {},
        NoRepair);

    ASSERT_FALSE(pathExists(garbage)) << "a collection ran inside the add";
    auto row = store->queryPathInfo(p);
    ASSERT_TRUE(row->objectHash.has_value());
    EXPECT_TRUE(hasTree(row->objectHash->hash)) << "the root tree survived the add's own collection";
    EXPECT_TRUE(hasBlob(merkle::blobId("b"), false)) << "the symlink's blob survived it";
    EXPECT_EQ(count("trees"), 2u);
}

/* An add whose path is valid already writes nothing, so it runs no
   collection: the in-memory route's `autoGC()` sits after the validity
   check and before its restore (master's order; 01 section 10, *the
   ingestion's `autoGC()` runs before the restore*).  Before, one call
   served both routes and ran before the check, so a no-op add under
   `min-free` paid the free-space check and, when the trigger held, a
   synchronous collection.  The spilled route cannot tell in time -- its
   validity is known from the restored tree -- and keeps its call before
   the restore (the test above). */
TEST_F(LocalStoreAutoGCTest, inMemoryAddOfAValidPathRunsNoCollection)
{
    auto nar = treeWithSymlink();
    auto add = [&] {
        StringSource source{nar};
        return store->addToStoreFromDump(
            source,
            "auto-gc-valid",
            FileSerialisationMethod::NixArchive,
            ContentAddressMethod::Raw::NixArchive,
            HashAlgorithm::SHA256,
            {},
            NoRepair);
    };
    auto p = add();
    ASSERT_FALSE(pathExists(garbage)) << "the first add's collection ran";
    ASSERT_TRUE(store->isValidPath(p));

    /* The trigger armed again: garbage planted, the free space below 97 %
       of what the last collection left. */
    writeFile(garbage, "dead");
    writeFile(fakeFree, "500");
    EXPECT_EQ(add(), p);
    EXPECT_TRUE(pathExists(garbage)) << "a collection ran for an add that wrote nothing";
}

TEST_P(LocalStoreCanonicalisationTest, flatFitsInMemory)
{
    using namespace std::string_view_literals;
    StringSource source{"very simple file"sv};
    auto sp = store->addToStoreFromDump(
        source,
        "flat",
        FileSerialisationMethod::Flat,
        /* For the purposes of this test, it doesn't really matter to vary the hashing method. */
        ContentAddressMethod::Raw::NixArchive,
        getHashAlgo(),
        {},
        NoRepair);
    assertCanonicalPermissions(store->toRealPath(sp));
    StringSink narDump;
    dumpString(source.s, narDump);
    StringSource narSource{narDump.s};
    auto sp2 = store->addToStoreFromDump(
        narSource,
        "nar",
        FileSerialisationMethod::NixArchive,
        /* For the purposes of this test, it doesn't really matter to vary the hashing method. */
        ContentAddressMethod::Raw::NixArchive,
        getHashAlgo(),
        {},
        NoRepair);
    assertCanonicalPermissions(store->toRealPath(sp2));
}

TEST_P(LocalStoreCanonicalisationTest, simpleNar)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    MemorySink memorySink{*accessor};
    memorySink.createDirectory(CanonPath::root);
    memorySink.createRegularFile(CanonPath("/regular"), [](auto & crf) { crf("test"); });
    memorySink.createRegularFile(CanonPath("/executable"), [](auto & crf) {
        crf("test");
        crf.isExecutable();
    });
    memorySink.createDirectory(CanonPath("/dir"));
    memorySink.createRegularFile(CanonPath("/dir/regular"), [](auto & crf) { crf("test 2"); });
    memorySink.createRegularFile(CanonPath("/dir/executable"), [](auto & crf) {
        crf("test 2");
        crf.isExecutable();
    });
    memorySink.createSymlink(CanonPath("/symlink"), "some target");
    memorySink.createSymlink(CanonPath("/dir/symlink"), "..");
    auto source = sinkToSource([&accessor](Sink & sink) { accessor->dumpPath(CanonPath::root, sink); });
    auto sp = store->addToStoreFromDump(
        *source,
        "nar-from-dump",
        FileSerialisationMethod::NixArchive,
        /* For the purposes of this test, it doesn't really matter to vary the hashing method. */
        ContentAddressMethod::Raw::NixArchive,
        getHashAlgo(),
        {},
        NoRepair);
    assertCanonicalPermissions(store->toRealPath(sp));

    memorySink.createRegularFile(CanonPath("/other"), [](auto & crf) { crf(""); });
    auto sp2 = store->addToStoreSlow("nar-slow", {accessor}, ContentAddressMethod::Raw::NixArchive, getHashAlgo()).path;
    assertCanonicalPermissions(store->toRealPath(sp2));

    Finally restoreNarBufferSize{[oldSize = settings.getLocalSettings().narBufferSize]() {
        settings.getLocalSettings().narBufferSize.assign(oldSize);
    }};
    // So that we always spill to the file system.
    settings.getLocalSettings().narBufferSize = 0;

    memorySink.createDirectory(CanonPath("/dir/entirely-something-else"));
    StringSink narDump;
    accessor->dumpPath(CanonPath::root, narDump);
    StringSource narSource{narDump.s};
    auto sp3 = store->addToStoreFromDump(
        narSource,
        "nar-from-dump-spilled",
        FileSerialisationMethod::NixArchive,
        /* For the purposes of this test, it doesn't really matter to vary the hashing method. */
        ContentAddressMethod::Raw::NixArchive,
        getHashAlgo(),
        {},
        NoRepair);
    assertCanonicalPermissions(store->toRealPath(sp3));
}

INSTANTIATE_TEST_SUITE_P(
    LocalStoreCanonicalisation,
    LocalStoreCanonicalisationTest,
    ::testing::Combine(
        ::testing::Values(
            HashAlgorithm::SHA256,
            /* Other algorithms are included because SHA256 is special-cased in some places,
               to avoid computing narHash twice. */
            HashAlgorithm::SHA1,
            HashAlgorithm::SHA512,
            HashAlgorithm::MD5)),
    [](const ::testing::TestParamInfo<LocalStoreCanonicalisationTest::ParamType> & info) {
        return std::string(printHashAlgo(std::get<HashAlgorithm>(info.param)));
    });

#endif

} // namespace nix
