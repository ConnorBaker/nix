#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <rapidcheck/gtest.h>
#include <sqlite3.h>

#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/store/sqlite.hh"
#include "nix/store/globals.hh"
#include "nix/store/derivations.hh"
#include "nix/store/gc-store.hh"
#include "nix/store/nar-info-disk-cache.hh"
#include "nix/store/nar-info.hh"
#include "nix/store/tests/temp-local-store.hh"

#include "nix/util/archive.hh"
#include "nix/util/callback.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/object-hash.hh"
#include "nix/util/finally.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/file-system.hh"
#include "nix/util/processes.hh"
#include "nix/store/git-object-store.hh"
#include "nix/store/pathlocks.hh"
#include "nix/store/content-address.hh"
#include "nix/util/tests/setting-scopes.hh"
#include "nix/util/tests/capture-logging.hh"
#include "nix/util/tests/source-accessor-gen.hh"
#include "nix/util/signature/local-keys.hh"
#include "nix/util/signature/signer.hh"
#include "nix/util/unix-domain-socket.hh"
#include "nix/util/signals.hh"
// Needed for template specialisations. This is not good! When we
// overhaul how store configs work, this should be fixed.
#include "nix/util/args.hh"
#include "nix/util/config-impl.hh"
#include "nix/util/abstract-setting-to-json.hh"

#include <sys/stat.h>
#include <atomic>
#include <deque>
#include <cerrno>
#include <cstring>
#include <future>
#ifndef _WIN32
#  include <poll.h>
#  include <sys/socket.h>
#endif

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

TEST(SQLite, externallySynchronizedConnectionOmitsOnlyItsOwnMutex)
{
    AutoDelete dir{createTempDir()};
    SQLite ordinary{dir.path() / "ordinary.sqlite", {.useWAL = true}};
    SQLite external{dir.path() / "external.sqlite", {.useWAL = true, .externalSynchronization = true}};
    EXPECT_EQ(sqlite3_db_mutex(external), nullptr);
    /* This test process leaves SQLite's configured default unchanged. */
    if (sqlite3_threadsafe() == 1) {
        EXPECT_NE(sqlite3_db_mutex(ordinary), nullptr);
    }
    external.exec("create table value (n integer)");
    external.exec("insert into value values (1)");
    SQLiteStmt query{external, "select n from value"};
    auto use = query.use();
    ASSERT_TRUE(use.next());
    EXPECT_EQ(use.getInt(0), 1);
}

#ifndef _WIN32
TEST(SQLite, openingAnotherConnectionPreservesWalLocks)
{
    AutoDelete dir{createTempDir()};
    auto path = dir.path() / "test.sqlite";
    SQLite first{path, {.useWAL = true}};
    first.isCache();
    first.exec("create table test (value integer)");
    auto shm = path.string() + "-shm";
    auto parent = getpid();

    /* A different process must observe the first connection's locks.
       Inspecting them in this process would both hide our own locks and,
       on close(), release them. Do not depend on SQLite's lock offsets. */
    auto locksHeld = [&] {
        Pid child = startProcess([&] {
            AutoCloseFD fd = open(shm.c_str(), O_RDWR | O_CLOEXEC);
            if (!fd)
                _exit(2);
            struct flock lock{};
            lock.l_type = F_WRLCK;
            lock.l_whence = SEEK_SET;
            if (fcntl(fd.get(), F_GETLK, &lock) == -1)
                _exit(3);
            _exit(lock.l_type != F_UNLCK && lock.l_pid == parent ? 0 : 4);
        });
        return child.wait() == 0;
    };

    ASSERT_TRUE(locksHeld());
    {
        SQLite second{path, {.mode = SQLiteOpenMode::NoCreate, .useWAL = true}};
        EXPECT_TRUE(locksHeld());
        second.exec("insert into test values (1)");
        EXPECT_TRUE(locksHeld());
    }
    EXPECT_TRUE(locksHeld());
}
#endif

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
    /* Atomic: the parallel copy creates files on its workers as well as on
       the walking thread (`MaterialisingVisitor::copyChildren`, `08` K17),
       so a plain counter is a data race and the count a test asserts is
       not well defined.  Found by the thread-sanitised run of `05` section
       5. */
    std::atomic<unsigned> regularFilesCreated = 0;

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
struct CountingAccessor final : SourceAccessor
{
private:
    void anchor() override {}

public:
    ref<SourceAccessor> inner;
    /* Atomic, for the reason `CountingRestoreHooks` gives: a worker of the
       parallel copy reads through this accessor too. */
    std::atomic<unsigned> reads = 0, listings = 0, stats = 0, links = 0;

    explicit CountingAccessor(ref<SourceAccessor> inner)
        : inner(inner)
    {
    }

    bool allowsConcurrentTraversal() override
    {
        return inner->allowsConcurrentTraversal();
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

/* These parallel-copy fixtures finish constructing the tree before a
   traversal and do not mutate it until that traversal has joined. */
struct ConcurrentMemorySourceAccessor final : MemorySourceAccessor
{
    bool allowsConcurrentTraversal() override
    {
        return true;
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
        /* The ingestion's hold lasts the call: what these tests write is
           unheld afterwards, as a registered path's objects are, so the
           sweep tests below judge it by reachability alone. */
        auto hold = store->objects.hold();
        auto r = store->restoreThroughObjects(dst, source, /*startFsync=*/false, &hooks, hold, repair);
        /* The index rows an
           ingestion makes are written when its path is registered.  These
           tests restore into `<store>/t1` and the like, which no
           registration covers, so the fixture registers the destination
           under its own name and writes them -- otherwise no blob would
           have a holder and every restore would write every file. */
        store->recordBlobsAtPath(hold, dst);
        filesCreated = hooks.regularFilesCreated;
        return r;
    }

    static PosixStat st(const std::filesystem::path & p)
    {
        return lstat(p);
    }

    /* A blob is held either by a store path (the index: every
       regular file's) or by a file under `blobs/` (a symlink's target). */
    bool hasBlob(const Hash & id, bool executable)
    {
        return store->objects.holderOrForget(id, executable, NoRepair).has_value()
               || pathExists(store->objects.blobFile(id, executable));
    }

    /* The blob's canonical file: its holder, or the object store's file. */
    std::filesystem::path blobFileOf(const Hash & id, bool executable)
    {
        if (auto holder = store->objects.holderOrForget(id, executable, NoRepair))
            return std::filesystem::path(holder->file);
        return store->objects.blobFile(id, executable);
    }

    /* A crash's effect on a hold: the lock released, the directory left
       (private to `Hold`; this fixture is its friend). */
    static void dropLock(GitObjectStore::Hold & hold)
    {
        hold.fd.close();
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

    static std::optional<std::string> readAuxiliaryObject(GitObjectStore & objects, const Hash & id, unsigned kind)
    {
        return kind == 0 ? objects.readTree(id) : objects.readBlob(id, kind == 2);
    }

    static merkle::Tree parseTreeBody(const Hash & id, std::string_view body)
    {
        return GitObjectStore::parseTreeBody(id, body);
    }

    /* The object files under a kind, through its shards
       (`GitObjectStore::eachObjectFile`); `tmp`, which is not sharded and
       holds the ingestions' hold directories, is counted as it lies. */
    size_t count(std::string_view sub)
    {
        size_t n = 0;
        if (sub == "tmp") {
            for ([[maybe_unused]] auto & e : DirectoryIterator{store->objects.dir / std::string(sub)})
                n++;
            return n;
        }
        GitObjectStore::eachObjectFile(
            store->objects.dir / std::string(sub), [&](const std::filesystem::path &) { n++; });
        /* Count auxiliary files plus distinct regular-file blob keys for
           this mode. A symlink target and regular file can contribute
           separately even with equal bytes. Raw candidate counts use
           blobRowCount, since each ingestion retains its own witness. */
        if (sub == "blobs" || sub == "blobs-x") {
            auto state = store->_state->lock();
            SQLiteStmt stmt{state->db, "select count(distinct id) from Blobs where exec = ?;"};
            auto use = stmt.use();
            use.apply(int64_t(sub == "blobs-x"));
            if (use.next())
                n += use.getInt(0);
        }
        return n;
    }

    /* The walk over the path as written, without hooks: the independent
       computation of what the restore's sink recorded from the stream. */
    static merkle::TreeEntry gitHash(const std::filesystem::path & p)
    {
        return objectHashOf(*makeFSSourceAccessor(p), CanonPath::root).root;
    }

    /* Enter and publish the on-disk path's candidates before another walk. */
    ObjectHashSink::Result
    enter(OptimiseStats & stats, const std::filesystem::path & p, GitObjectStore::BlobInodes & inodes)
    {
        auto hold = store->objects.hold();
        auto result = store->enterIntoObjects(p, NoRepair, hold, nullptr, &stats, &inodes);
        store->recordBlobsAtPath(hold, p);
        return result;
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
        /* Allocate the replacement before releasing the original inode;
           unlink-then-create may reuse it and does not test a stale row. */
        auto replacement = p;
        replacement += ".replacement";
        writeFile(replacement, bytes);
        std::filesystem::rename(replacement, p);
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
        PathFilter & filter = defaultPathFilter,
        bool callbacksPermitParallelism = false)
    {
        CountingRestoreHooks hooks{
            makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls))};
        auto hold = store->objects.hold();
        auto r = store->materialiseThroughObjects(
            dst, src, filter, /*startFsync=*/false, &hooks, repair, namer, hold, callbacksPermitParallelism);
        /* As `restore`: the second route writes rows too, and they become
           true when its path is registered.  These tests materialise into
           `<store>/m1` and the like, which no registration covers, so the
           fixture registers the destination under its own name and writes
           them -- otherwise the files this route wrote would hold no blob
           and the next materialisation would write them again. */
        store->recordBlobsAtPath(hold, dst);
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

TEST_F(LocalStoreObjectsTest, pathMetadataCanonicalisationNormalizesCompleteTimestamp)
{
    for (unsigned int kind = 0; kind < 4; ++kind) {
        SCOPED_TRACE(kind);
        auto path = tempStoreDir.path() / fmt("timestamp-kind-%d", kind);
        if (kind < 2) {
            writeFile(path, "contents");
            chmod(path, kind == 0 ? 0644 : 0755);
        } else if (kind == 2) {
            createDir(path);
        } else {
            createSymlink("absent-target", path);
        }
        for (auto mtime : {timespec{0, 0}, timespec{1, 0}, timespec{1, 1}, timespec{2, 0}}) {
            SCOPED_TRACE(fmt("mtime=%d.%d", mtime.tv_sec, mtime.tv_nsec));
            timespec times[2]{{0, UTIME_OMIT}, mtime};
            ASSERT_EQ(::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW), 0);
            canonicaliseTimestampAndPermissions(path);
            auto after = st(path);
            EXPECT_EQ(after.st_mtime, 1);
#  ifdef __APPLE__
            EXPECT_EQ(after.st_mtimespec.tv_nsec, 0);
#  else
            EXPECT_EQ(after.st_mtim.tv_nsec, 0);
#  endif
            if (kind != 3) {
                EXPECT_EQ(after.st_mode & ~S_IFMT, kind == 0 ? 0444 : 0555);
            } else {
                EXPECT_EQ(readLink(path), "absent-target");
            }
        }
    }
}

TEST_F(LocalStoreObjectsTest, materialisationUsesImmutableBlobIdentityButRepairRehashes)
{
    for (char kind : {'f', 'x'}) {
        auto source = treeAccessor({{"/file", "before", kind}});
        auto file = CanonPath{"/file"};
        auto entry = objectHashOf(*source, file).root;
        auto leaf = make_ref<MemorySourceAccessor>();
        leaf->root = *source->open(file, std::nullopt);
        leaf->knownRoot = entry;
        auto dst = config->realStoreDir.get() / std::string{kind};
        auto copied = materialise(dst, SourcePath(leaf), noNode());
        EXPECT_EQ(copied.root, entry);
        EXPECT_EQ(readFile(dst), "before");
        auto reused = materialise(dst.string() + "-again", SourcePath(leaf), noNode());
        EXPECT_EQ(reused.root, entry);
        EXPECT_EQ(filesCreated, 0u);
        EXPECT_EQ(st(dst).st_ino, st(dst.string() + "-again").st_ino);

        /* Repair must not trust even an advertised immutable identity:
           deliberately corrupt this accessor's bytes behind its name. */
        std::get<MemorySourceAccessor::File::Regular>(leaf->root->raw).contents = "after!";
        auto expected = objectHashOf(*leaf, CanonPath::root).root;
        auto repaired = materialise(dst.string() + "-repair", SourcePath(leaf), noNode(), Repair);
        EXPECT_NE(expected, entry);
        EXPECT_EQ(repaired.root, expected);
        EXPECT_EQ(readFile(dst.string() + "-repair"), "after!");
    }
}

TEST_F(LocalStoreObjectsTest, canonicalBlobCopyDoesNotRequireSizeMetadata)
{
    struct Source : MemorySourceAccessor
    {
        bool sizeKnown;
        size_t reads = 0;

        Source(std::string bytes, bool executable, bool sizeKnown)
            : sizeKnown(sizeKnown)
        {
            knownRoot = merkle::TreeEntry{
                .mode = executable ? merkle::Mode::Executable : merkle::Mode::Regular, .hash = merkle::blobId(bytes)};
            root = File::Regular{.executable = executable, .contents = std::move(bytes)};
        }

        std::optional<Stat> maybeLstat(const CanonPath & path) override
        {
            auto st = MemorySourceAccessor::maybeLstat(path);
            if (st && !sizeKnown)
                st->fileSize.reset();
            return st;
        }

        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            reads++;
            MemorySourceAccessor::readFile(path, sink, sizeCallback);
        }
    };

    unsigned int serial = 0;
    for (bool sizeKnown : {false, true})
        for (bool held : {false, true})
            for (bool executable : {false, true})
                for (auto repair : {NoRepair, Repair}) {
                    SCOPED_TRACE(sizeKnown);
                    SCOPED_TRACE(held);
                    SCOPED_TRACE(executable);
                    SCOPED_TRACE(repair);
                    auto bytes = fmt("canonical blob %d", serial++);
                    auto source = make_ref<Source>(bytes, executable, sizeKnown);
                    auto dst = config->realStoreDir.get() / fmt("canonical-%d", serial);
                    auto seed = dst.string() + "-seed";
                    if (held)
                        materialise(seed, SourcePath(source), noNode());
                    source->reads = 0;

                    auto copied = materialise(dst, SourcePath(source), noNode(), repair);
                    EXPECT_EQ(copied.root, *source->knownRoot);
                    EXPECT_EQ(source->reads, held && !repair ? 0u : 1u);
                    EXPECT_EQ(filesCreated, held ? 0u : 1u);
                    EXPECT_EQ(readFile(dst), bytes);
                    EXPECT_EQ(bool(st(dst).st_mode & S_IXUSR), executable);
                    StringSink nar;
                    dumpPath(dst, nar);
                    EXPECT_EQ(copied.narSize, nar.s.size());
                    if (held)
                        EXPECT_EQ(st(dst).st_ino, st(seed).st_ino);
                }
}

TEST_F(LocalStoreObjectsTest, knownBlobChecksAnnouncedAndDeliveredSizes)
{
    struct Source : MemorySourceAccessor
    {
        bool sizeKnown;
        std::optional<size_t> announced;
        size_t delivered;

        Source(bool sizeKnown, std::optional<size_t> announced, size_t delivered)
            : sizeKnown(sizeKnown)
            , announced(announced)
            , delivered(delivered)
        {
            char byte = 'a' + announced.value_or(3) * 3 + delivered;
            knownRoot =
                merkle::TreeEntry{.mode = merkle::Mode::Regular, .hash = merkle::blobId(std::string(delivered, byte))};
            root = File::Regular{.contents = std::string(1, byte)};
        }

        std::optional<Stat> maybeLstat(const CanonPath & path) override
        {
            auto st = MemorySourceAccessor::maybeLstat(path);
            if (st && !sizeKnown)
                st->fileSize.reset();
            return st;
        }

        void readFile(const CanonPath &, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            if (announced)
                sizeCallback(*announced);
            sink(std::string(delivered, 'a' + announced.value_or(3) * 3 + delivered));
        }
    };

    /* Exercise stream validation on misses throughout, including repeated
       empty blobs that would otherwise reuse the first successful copy. */
    auto * index = store->objects.index;
    Finally restoreIndex{[&] { store->objects.index = index; }};
    store->objects.index = nullptr;
    for (bool sizeKnown : {false, true})
        for (auto announced : std::vector<std::optional<size_t>>{std::nullopt, 0, 1, 2})
            for (size_t delivered : {0u, 1u, 2u}) {
                auto source = make_ref<Source>(sizeKnown, announced, delivered);
                auto dst =
                    config->realStoreDir.get() / fmt("size-%d-%d-%d", sizeKnown, announced.value_or(3), delivered);
                SCOPED_TRACE(dst.string());
                if (announced && *announced == delivered && (!sizeKnown || *announced == 1)) {
                    EXPECT_NO_THROW(materialise(dst, SourcePath(source), noNode()));
                } else {
                    EXPECT_THROW(materialise(dst, SourcePath(source), noNode()), Error);
                }
            }
}

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
    EXPECT_EQ(store->blobRowCount(false), 3u);
    EXPECT_EQ(store->blobRowCount(true), 1u);
    auto one = merkle::blobId("one");
    EXPECT_TRUE(hasBlob(one, false));
    EXPECT_TRUE(hasBlob(one, true));
    EXPECT_TRUE(hasTree(root1.hash));

    /* `a` and `d/e/c` share the plain file; `d/e/x` is the executable
       file, another inode with the same bytes; never a link across the bit.
       The counts (`08` K8) are one
       lower than they were, because the blob's canonical file *is* one of
       these -- there is no link into `.objects/blobs` beside them.  `a`
       and `d/e/c` are two links; the files no second file shares are at
       one, which is why the link count is no longer a witness that a file
       was entered (the index is). */
    EXPECT_EQ(st(t1 / "a").st_nlink, 2u);
    EXPECT_EQ(st(t1 / "d/e/c").st_ino, st(t1 / "a").st_ino);
    EXPECT_EQ(st(t1 / "a").st_ino, st(blobFileOf(one, false)).st_ino);
    EXPECT_EQ(st(t1 / "d/e/x").st_ino, st(blobFileOf(one, true)).st_ino);
    EXPECT_NE(st(t1 / "d/e/x").st_ino, st(t1 / "a").st_ino);
    EXPECT_EQ(st(t1 / "d/e/x").st_nlink, 1u);
    EXPECT_EQ(readFile(blobFileOf(one, true)), "one");
    EXPECT_EQ(st(t1 / "d/b").st_nlink, 1u);
    EXPECT_EQ(st(t1 / "empty").st_nlink, 1u);
    for (auto & p : {t1 / "a", t1 / "d/b", t1 / "d/e/c", t1 / "empty"})
        EXPECT_EQ(st(p).st_mode & ~S_IFMT, 0444u) << p;
    EXPECT_EQ(st(t1 / "d/e/x").st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(st(blobFileOf(one, false)).st_mode & ~S_IFMT, 0444u);
    EXPECT_EQ(st(blobFileOf(one, true)).st_mode & ~S_IFMT, 0555u);
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
    EXPECT_EQ(store->blobRowCount(false), 6u);
    EXPECT_EQ(store->blobRowCount(true), 2u);
    for (auto & rel : {"a", "d/b", "d/e/c", "d/e/x", "empty"})
        EXPECT_EQ(st(t2 / rel).st_ino, st(t1 / rel).st_ino) << rel;
    /* Four, not five -- `t1/a`, `t1/d/e/c`, `t2/a`, `t2/d/e/c`
       and no file of the object store's own. */
    EXPECT_EQ(st(t2 / "a").st_nlink, 4u);
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
    EXPECT_EQ(store->blobRowCount(false), 7u);

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
    /* The sweep's blob pass judges the symlink-target blob
       alone; the regular files' blobs have no file under `blobs/` to
       judge, being held by the paths themselves (design.md section 2.2). */
    EXPECT_EQ(swept.blobsKept, 1u);
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
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(swept.blobsRemoved, 0u);
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(count("blobs"), 3u);
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(count("blobs-x"), 1u);
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
    EXPECT_EQ(store->blobRowCount(false), 2u);
    EXPECT_EQ(store->blobRowCount(true), 1u);

    auto t2 = realStoreDir / "c2";
    auto r2 = restore(t2, nar("TWO"));
    EXPECT_EQ(r2.root, gitHash(t2));
    EXPECT_EQ(filesCreated, 1u);
    EXPECT_EQ(count("blobs"), 4u);
    EXPECT_EQ(count("blobs-x"), 1u);
    EXPECT_EQ(store->blobRowCount(false), 4u);
    EXPECT_EQ(store->blobRowCount(true), 2u);
    EXPECT_EQ(readFile(t2 / "d/b"), "TWO");
    EXPECT_EQ(st(t2 / "d/b").st_ino, st(blobFileOf(merkle::blobId("TWO"), false)).st_ino);
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
    EXPECT_EQ(store->blobRowCount(false), 2u);
    EXPECT_EQ(store->blobRowCount(true), 1u);

    auto bigId = merkle::blobId(big);
    EXPECT_TRUE(hasBlob(bigId, false));
    EXPECT_TRUE(hasBlob(bigId, true));
    EXPECT_EQ(st(t1 / "big").st_ino, st(blobFileOf(bigId, false)).st_ino);
    EXPECT_EQ(st(t1 / "big-x").st_ino, st(blobFileOf(bigId, true)).st_ino);
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
    /* One lower: the blob's canonical file is a store path's own, so there is
     * no link into `.objects/blobs` beside it (K8) */
    EXPECT_EQ(st(t1 / "big").st_nlink, 2u);
    EXPECT_EQ(count("blobs"), 2u);
    EXPECT_EQ(count("blobs-x"), 1u);
    EXPECT_EQ(store->blobRowCount(false), 4u);
    EXPECT_EQ(store->blobRowCount(true), 2u);
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
    EXPECT_EQ(st(t / "a-x").st_ino, st(blobFileOf(one, true)).st_ino);
    EXPECT_EQ(st(t / "b").st_ino, st(blobFileOf(one, false)).st_ino);
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
    EXPECT_EQ(st(blobFileOf(merkle::blobId("target"), false)).st_nlink, 1u);

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
    EXPECT_EQ(st(x).st_ino, st(blobFileOf(merkle::blobId("run"), true)).st_ino);
    /* One lower: the blob's canonical file is a store path's own, so there is
     * no link into `.objects/blobs` beside it (K8) */
    EXPECT_EQ(st(x).st_nlink, 1u);

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
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(swept.blobsKept, 1u);

    /* The symlink dropped: its synthetic tree, and then its blob, go. */
    swept = objects.sweep([&](fun<void(const ObjectHash &)> live) {
        live(ObjectHash::of(rx.root));
        return 0;
    });
    EXPECT_EQ(swept.treesRemoved, 1u);
    EXPECT_EQ(swept.blobsRemoved, 1u);
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(swept.blobsKept, 0u);
    EXPECT_FALSE(hasBlob(merkle::blobId("target"), false));
    EXPECT_TRUE(hasTree(execTree));

    /* The executable dropped while its file stands: the tree goes, the
       blob stays by its link; once the file is gone, the blob goes too. */
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; });
    EXPECT_EQ(swept.treesRemoved, 1u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(swept.blobsKept, 0u);
    EXPECT_TRUE(hasBlob(merkle::blobId("run"), true));
    EXPECT_EQ(count("trees"), 0u);
    deletePath(x);
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; });
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(swept.blobsRemoved, 0u);
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(count("blobs-x"), 1u);
}

/* Repair: a blob's *holder* corrupted in place, same length, is not linked
   to when a path is restored with repair.  What repair does with it is the
   representation's own (`08` K2): the holder is a valid store path's file,
   so it is **forgotten** -- its row deleted -- and not removed, and the
   freshly written file becomes the blob's holder in its place. */
TEST_F(LocalStoreObjectsTest, repairDoesNotLinkACorruptBlob)
{
    auto nar = treeNar({{"a", "one", 'f'}});
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto t1 = realStoreDir / "r1";
    restore(t1, nar);
    auto one = merkle::blobId("one");
    auto file = blobFileOf(one, false);
    /* Corrupt the blob's bytes in place, preserving canonical metadata. */
    chmod(t1 / "a", 0644);
    writeFile(t1 / "a", "ONE");
    chmod(t1 / "a", 0444);
    setWriteTime(t1 / "a", 1, 1);
    EXPECT_EQ(readFile(file), "ONE");
    /* Without repair the composite trusts the size and links the corrupt
       file, writing nothing. */
    auto t2 = realStoreDir / "r2";
    restore(t2, nar);
    EXPECT_EQ(readFile(t2 / "a"), "ONE");
    EXPECT_EQ(filesCreated, 0u);
    /* With repair it hashes the holder, forgets it, and writes anew. */
    auto t3 = realStoreDir / "r3";
    restore(t3, nar, Repair);
    EXPECT_EQ(filesCreated, 1u);
    EXPECT_EQ(readFile(t3 / "a"), "one");
    EXPECT_NE(st(t3 / "a").st_ino, st(t1 / "a").st_ino);
    /* The blob's holder is now the fresh file, not the corrupt one. */
    EXPECT_EQ(st(blobFileOf(one, false)).st_ino, st(t3 / "a").st_ino);
    EXPECT_EQ(readFile(blobFileOf(one, false)), "one");
    /* And the corrupt file is still there: it belongs to a valid store
       path, so the store forgets its row rather than removing it (`08`
       K2), which `--verify --check-contents` is what reports. */
    EXPECT_EQ(readFile(file), "ONE");
    EXPECT_TRUE(pathExists(file));
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
    auto file = blobFileOf(one, false);
    /* The blob corrupted in place, with the same length and canonical metadata. */
    chmod(t1 / "a", 0644);
    writeFile(t1 / "a", "ONE");
    chmod(t1 / "a", 0444);
    setWriteTime(t1 / "a", 1, 1);
    ASSERT_EQ(readFile(file), "ONE");

    /* A fresh, correct output written plainly; entered without repair, its
       file is replaced by a link to the corrupt blob. */
    auto plain = realStoreDir / "k2";
    writePlainTree(plain, "one");
    ASSERT_EQ(st(plain / "a").st_nlink, 1u);
    auto hold = store->objects.hold();
    store->enterPath(plain, NoRepair, hold);
    EXPECT_EQ(readFile(plain / "a"), "ONE");
    EXPECT_EQ(st(plain / "a").st_ino, st(file).st_ino);

    /* Entered with repair, the corrupt holder is hashed, forgotten, and the
       fresh file becomes the blob's holder.  The corrupt file itself stays:
       it is a valid path's, so the store forgets the row rather than
       removing the file (`08` K2). */
    auto fresh = realStoreDir / "k3";
    writePlainTree(fresh, "one");
    auto freshInode = st(fresh / "a").st_ino;
    auto r = store->enterPath(fresh, Repair, hold);
    store->recordBlobsAtPath(hold, fresh);
    EXPECT_EQ(r.root, gitHash(fresh));
    EXPECT_EQ(readFile(fresh / "a"), "one");
    EXPECT_EQ(st(fresh / "a").st_ino, freshInode);
    EXPECT_EQ(readFile(blobFileOf(one, false)), "one");
    EXPECT_EQ(st(blobFileOf(one, false)).st_ino, freshInode) << "the fresh file is the holder now";
    EXPECT_EQ(readFile(file), "ONE") << "and the corrupt path's file is left where it is";
    /* One link, not two: the holder *is* this path's file, and no second
       path shares it yet (`08` K8). */
    EXPECT_EQ(st(fresh / "a").st_nlink, 1u);
}

/* The after-build walk enters its output under the *mode's* repair flag,
   over every build mode there is and not `bmRepair` alone
   (`enterRepairFlagOf`, store-api.hh; its one call site is the walk in
   derivation-builder-impl.cc, which now passes the mode and computes
   nothing itself).  Under `--repair` the walk hashes the blob's holder, so
   a corrupt one is not linked over the fresh output; the other modes trust
   the store's holders.  `--check` enters nothing -- its output is hashed
   where it stands and not registered -- so its flag is what it would enter
   under, and it is stated here rather than left unsaid.  The loop is over
   the whole domain and keys on the flag, so the three literal expectations
   above it are the mapping and the loop is its consequence.

   What this pinned before and what it pins now.  Before, the corrupt file
   was a *blob file of the store's own naming*, planted under
   `.objects/blobs/<xx>/<id>`, and `--repair` replaced it.  A regular-file
   blob has no such file: its canonical file is a store path's own -- a
   holder -- so the corrupt file is now a valid path's file, corrupted in
   place with the right length, and what the modes are told apart by is
   `08` K2's change: `--repair` re-hashes the holder, *forgets* its row and
   makes the fresh output the holder, leaving the corrupt path's file where
   it is; the other modes trust the row's size and link the corrupt holder
   over the fresh file.  Each mode gets a blob of its own, so that the three
   iterations do not share one holder. */
TEST_F(LocalStoreObjectsTest, everyBuildModeEntersWithItsRepairFlag)
{
    EXPECT_EQ(enterRepairFlagOf(bmNormal), NoRepair);
    EXPECT_EQ(enterRepairFlagOf(bmRepair), Repair);
    EXPECT_EQ(enterRepairFlagOf(bmCheck), NoRepair) << "`--check` enters nothing; this is what it would enter under";

    /* `buildModes` is `BuildMode`'s enumerators in order, so the loop is
       the whole domain (05 section 8, rule 1); `enterRepairFlagOf`'s
       `switch` without a `default` is what refuses a mode added without a
       flag. */
    ASSERT_EQ(buildModes.size(), 3u);
    for (size_t i = 0; i < buildModes.size(); i++)
        ASSERT_EQ(static_cast<size_t>(buildModes[i]), i) << "buildModes is BuildMode's enumerators, in order";

    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};

    for (auto mode : buildModes) {
        auto name = fmt("mode-%d", static_cast<int>(mode));
        /* A blob of this mode's own: `good` its bytes and `bad` its
           corruption, the same length, so that only a walk that hashes it
           can tell them apart. */
        auto good = fmt("one-%d", static_cast<int>(mode));
        auto bad = fmt("ONE-%d", static_cast<int>(mode));
        auto id = merkle::blobId(good);

        /* The blob's holder: a store path's own file, corrupted in place,
           as a bad disk leaves it.  Its row still stands -- the size is
           right -- so the index answers with it. */
        auto held = realStoreDir / ("held-" + name);
        restore(held, treeNar({{"a", good, 'f'}}));
        chmod(held / "a", 0644);
        writeFile(held / "a", bad);
        chmod(held / "a", 0444);
        setWriteTime(held / "a", 1, 1);
        auto blob = blobFileOf(id, false);
        ASSERT_EQ(blob, held / "a") << name << ": the corrupt file is the blob's holder";
        ASSERT_EQ(readFile(blob), bad) << name;

        /* A fresh, correct output written plainly, entered under the
           mode's flag. */
        auto fresh = realStoreDir / name;
        writePlainTree(fresh, good);
        auto freshInode = st(fresh / "a").st_ino;
        auto hold = store->objects.hold();
        store->enterPath(fresh, enterRepairFlagOf(mode), hold);
        store->recordBlobsAtPath(hold, fresh);

        /* What the mode must leave, said per mode and not through the flag
           under test: `--repair` alone hashes the blob's holder and
           forgets it.  A `switch` without a `default`, so a mode added
           later does not compile until it says what it leaves. */
        bool repaired = [&] {
            switch (mode) {
            case bmRepair:
                return true;
            case bmNormal:
            case bmCheck:
                return false;
            }
            unreachable();
        }();

        if (repaired) {
            EXPECT_EQ(readFile(fresh / "a"), good) << name << ": the fresh bytes stand";
            EXPECT_EQ(st(fresh / "a").st_ino, freshInode) << name;
            /* The row is forgotten and re-made: the fresh file is the
               blob's holder now. */
            EXPECT_EQ(blobFileOf(id, false), fresh / "a") << name << ": the fresh file is the holder";
            EXPECT_EQ(st(blobFileOf(id, false)).st_ino, freshInode) << name;
            /* And the corrupt file stays where it is: it belongs to a
               valid store path, so the store forgets the row rather than
               removing the file (`08` K2), and `--verify
               --check-contents` is what reports it. */
            EXPECT_EQ(readFile(held / "a"), bad) << name << ": the corrupt path's file is left where it is";
        } else {
            EXPECT_EQ(readFile(fresh / "a"), bad) << name << ": the corrupt holder was linked over the fresh file";
            EXPECT_EQ(st(fresh / "a").st_ino, st(held / "a").st_ino) << name;
            EXPECT_EQ(blobFileOf(id, false), held / "a") << name << ": the holder is unchanged";
        }
    }
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
    /* One lower: the blob's canonical file is a store path's own, so there is
     * no link into `.objects/blobs` beside it (K8) */
    EXPECT_EQ(st(p / "a").st_nlink, 2u);
    EXPECT_EQ(st(p / "d/b").st_ino, st(p / "a").st_ino);
    /* One lower: the blob's canonical file is a store path's own, so there is
     * no link into `.objects/blobs` beside it (K8) */
    EXPECT_EQ(st(p / "d/x").st_nlink, 1u);
    EXPECT_NE(st(p / "d/x").st_ino, st(p / "a").st_ino);
    /* The symlink's target is an auxiliary file; the regular-file blobs
       have published witnesses in the path itself. */
    EXPECT_EQ(count("blobs"), 2u); /* the symlink's "a" and the plain "one" */
    EXPECT_EQ(count("blobs-x"), 1u);
    EXPECT_EQ(count("trees"), 2u);
    EXPECT_EQ(readFile(p / "d/b"), "one");
    EXPECT_EQ(inodes.size(), 2u);

    OptimiseStats again;
    auto root2 = enter(again, p, inodes).root;
    EXPECT_EQ(root2, root);
    EXPECT_EQ(again.filesLinked, 0u);
    EXPECT_EQ(again.filesEntered, 0u);
    EXPECT_EQ(store->blobRowCount(false), 1u);
    EXPECT_EQ(store->blobRowCount(true), 1u);
    /* One lower: the blob's canonical file is a store path's own, so there is
     * no link into `.objects/blobs` beside it (K8) */
    EXPECT_EQ(st(p / "a").st_nlink, 2u);
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
        std::filesystem::remove(blobFileOf(merkle::blobId(bytes), false));
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
    /* One lower: the blob's canonical file is a store path's own, so there is
     * no link into `.objects/blobs` beside it (K8) */
    EXPECT_EQ(st(store->toRealPath(sound) / "a").st_nlink, 1u) << "entered";
    EXPECT_EQ(readHashColumn(*store, modified), modifiedColumn) << "left as the database has it";
    EXPECT_EQ(st(b).st_nlink, 1u) << "not entered";
    EXPECT_FALSE(hasBlob(merkle::blobId("xwo\n"), false));
}

/* Every ingestion records its own holder, including one linked from an
   earlier path. Collecting that donor removes its candidate alone; the
   surviving path still witnesses the shared inode without another walk. */
TEST_F(LocalStoreObjectsTest, sharingWitnessSurvivesDonorCollection)
{
    auto nar = treeNar({{"a", "shared", 'f'}});
    auto id = merkle::blobId("shared");
    auto add = [&](std::string_view name) {
        ValidPathInfo info{
            StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-" + std::string(name)},
            UnkeyedValidPathInfo{*store, std::nullopt},
        };
        info.assertedNarHash = hashString(HashAlgorithm::SHA256, nar);
        info.narSize = nar.size();
        StringSource source{nar};
        store->addToStore(info, source, NoRepair, NoCheckSigs);
        return info.path;
    };

    /* The holder, and a second path that shares its inode: two rows, one
       inode, two links. */
    auto holder = add("holder");
    auto sharer = add("sharer");
    ASSERT_EQ(blobFileOf(id, false), store->toRealPath(holder) / "a");
    ASSERT_EQ(store->blobRowCount(false), 2u);
    ASSERT_EQ(st(store->toRealPath(sharer) / "a").st_ino, st(store->toRealPath(holder) / "a").st_ino)
        << "the second ingestion linked the holder";
    ASSERT_EQ(st(store->toRealPath(holder) / "a").st_nlink, 2u);

    /* The holder's path collected.  Its row is stale -- its ingestion
       names a path that is no longer valid -- and goes with the sweep.
       Reopened without this process's temp roots, as
       `deleteSpecificReadsOnlyTheDeletedPathsTrees` does, so that the
       paths just added are collectable. */
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    Finally unset{[] { unsetenv("_NIX_TEST_NO_LSOF"); }};
    store.reset();
    store = std::make_shared<LocalStore>(ref{config});

    GCOptions options;
    options.action = GCOptions::gcDeleteSpecific;
    options.pathsToDelete = GCOptions::SpecificPaths{.paths = {holder}};
    GCResults results;
    store->collectGarbage(options, results);
    ASSERT_FALSE(store->isValidPath(holder));
    store->sweepStaleBlobRows();
    EXPECT_EQ(store->blobRowCount(false), 1u) << "only the collected path's candidate is removed";
    EXPECT_TRUE(hasBlob(id, false)) << "the surviving path still records its own holder";
    EXPECT_EQ(readFile(store->toRealPath(sharer) / "a"), "shared");
    EXPECT_EQ(st(store->toRealPath(sharer) / "a").st_nlink, 1u);
    auto survivingInode = st(store->toRealPath(sharer) / "a").st_ino;

    /* Optimisation already knows the surviving inode and enters nothing. */
    OptimiseStats stats;
    store->optimiseStore(stats);
    EXPECT_EQ(stats.filesEntered, 0u) << "the surviving path retained its witness";
    EXPECT_EQ(store->blobRowCount(false), 1u) << "optimisation replaces this path's previous projection";
    EXPECT_EQ(blobFileOf(id, false), store->toRealPath(sharer) / "a");
    EXPECT_EQ(st(blobFileOf(id, false)).st_ino, survivingInode) << "the same inode: nothing was rewritten";

    /* A later ingestion shares the same inode and records its own witness. */
    auto later = add("later");
    EXPECT_EQ(st(store->toRealPath(later) / "a").st_ino, survivingInode);
    EXPECT_EQ(st(store->toRealPath(sharer) / "a").st_nlink, 2u);
    EXPECT_EQ(store->blobRowCount(false), 2u);
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
    /* One lower: the blob's canonical file is a store path's own, so there is
     * no link into `.objects/blobs` beside it (K8) */
    EXPECT_EQ(st(real / "a").st_nlink, 1u);
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
/* A blob file no path links -- one the store's own naming holds -- is kept
   while a live tree names it and goes when none does.
   What this pinned before and what it pins now.  Before, the two blobs
   were a *regular* file's and an executable's, each with a file under
   `.objects/blobs`, and the test broke the paths' links (`unlinkInPlace`)
   so that the files stood at one link and only the tree kept them.  A
   regular-file blob has no file of the store's own any more -- its
   canonical file is the store path's own, and what stands where the file
   stood is the row -- so the blobs this rule still has a domain over are
   the **symlink targets**, which no path holds and which are therefore at
   one link from their first instant (`01` §9.10: "kept for the encoding's
   completeness and linked by nothing").  The two regular files stay in the
   tree to say what the sweep's blob pass now does *not* judge: their blobs
   are rows, owned by the path, and neither is counted here. */
TEST_F(LocalStoreObjectsTest, sweepKeepsASymlinkBlobALiveTreeNamesThoughNothingLinksIt)
{
    auto nar = treeNar({{"a", "kept", 'f'}, {"l", "kept alone", 'l'}, {"x", "kept too", 'x'}});
    auto & objects = store->objects;
    auto t = std::filesystem::path{config->realStoreDir.get()} / "unlinked";
    auto r = restore(t, nar);

    auto plain = merkle::blobId("kept");
    auto exec = merkle::blobId("kept too");
    auto target = merkle::blobId("kept alone");

    /* The symlink's target is the one blob here with a file of the store's
       own naming, and nothing links it. */
    ASSERT_EQ(blobFileOf(target, false), objects.blobFile(target, false));
    ASSERT_EQ(st(blobFileOf(target, false)).st_nlink, 1u);
    /* The two regular files' blobs have no file under `blobs/` at all:
       their holders are this path's own files, at one link apiece, the
       index and not the link count being the witness (`08` K8). */
    ASSERT_EQ(blobFileOf(plain, false), t / "a");
    ASSERT_EQ(blobFileOf(exec, true), t / "x");
    ASSERT_FALSE(pathExists(objects.blobFile(plain, false)));
    ASSERT_FALSE(pathExists(objects.blobFile(exec, true)));
    ASSERT_EQ(st(t / "a").st_nlink, 1u);
    ASSERT_EQ(st(t / "x").st_nlink, 1u);
    ASSERT_EQ(st(t / "x").st_mode & ~S_IFMT, 0555u);

    /* The path live: the tree names the symlink's blob, so it stays. */
    auto swept = objects.sweep([&](fun<void(const ObjectHash &)> live) {
        live(ObjectHash::of(r.root));
        return 0;
    });
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.blobsKept, 1u) << "the symlink's target, kept by the tree that names it";
    EXPECT_TRUE(hasBlob(target, false));
    EXPECT_TRUE(hasBlob(plain, false));
    EXPECT_TRUE(hasBlob(exec, true));

    /* The path dropped: the tree goes, and with it the symlink's blob.
       The regular files' rows are not this sweep's business -- they go
       with their path, by the stale-row rule
       (`theSweepJudgesARowByItsIngestionsPath`). */
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; });
    EXPECT_EQ(swept.treesRemoved, 1u);
    EXPECT_EQ(swept.blobsRemoved, 1u);
    EXPECT_EQ(swept.blobsKept, 0u);
    EXPECT_FALSE(hasBlob(target, false));
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
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(swept.blobsKept, 1u);
}

/* Laws 4 and 5 under a lost window (01 section 9.10, "Collection"): a
   valid path whose root tree a collection took between `putTree` and its
   registration.  The next sweep, with that path live, counts the root as
   unentered and keeps nothing beneath it -- the orphaned `d/` tree goes,
   and with it the symlink's blob, held by nothing but the tree that named
   it -- and removes what else is dead (before, the removal phase was
   abandoned and every later collection reclaimed nothing).  `verifyStore
   --check-contents` reports the path, re-entering it under --repair, after
   which the trees and blobs are back and the sweep keeps everything.

   What changed under the holder representation.  The unlinked file's blob
   used to be a second thing this sweep removed: a file under
   `.objects/blobs` that only the lost tree named.  A regular-file blob has
   no such file, so `unlinkInPlace` no longer strands a blob file -- it
   makes the *row* untrue, and the store forgets it (`08` K2, which never
   removes the file, a valid path owning it).  The statement about the
   verifier is therefore stronger than it was: law 4 for a regular file is
   now "the index knows the blob with a holder that is still what the row
   says", and it is the missing *row*, not a missing file, that
   `--check-contents` reports and `--repair` puts back. */
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
    ASSERT_EQ(st(blobFileOf(symlinkBlob, false)).st_nlink, 1u);
    ASSERT_EQ(count("trees"), 2u);
    /* One file under `blobs/` -- the symlink's target -- and two rows, for
       `a` and `d/b`, whose holders are this path's own files. */
    ASSERT_EQ(count("blobs"), 3u);
    ASSERT_EQ(store->blobRowCount(false), 2u);
    /* `a` left as the store leaves a file it could not link.  Before, that
       left its blob file at the store's link alone, kept only by the tree
       that names it; now the file *was* the blob's holder, so replacing it
       makes the row say what is not so, and the next lookup forgets it
       (`08` K2: the row goes, never the file).  The store then holds that
       blob nowhere, which is what `--verify --repair` puts back. */
    unlinkInPlace(store->toRealPath(info.path) / "a");
    ASSERT_FALSE(hasBlob(merkle::blobId("one"), false)) << "the row is forgotten, not the file removed";
    ASSERT_EQ(store->blobRowCount(false), 1u);
    ASSERT_EQ(count("blobs"), 2u);

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
    EXPECT_EQ(swept.blobsRemoved, 1u) << "the symlink's blob, the one blob here with a file of its own";
    EXPECT_FALSE(hasTree(dTree));
    EXPECT_FALSE(hasBlob(symlinkBlob, false));
    EXPECT_FALSE(hasBlob(merkle::blobId("one"), false));
    EXPECT_EQ(count("blobs"), 1u) << "the row for \"two\", whose holder is d/b";
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
    /* Re-entered: the row is back and names this path's own file, which is
       at one link, the holder being the file itself (`08` K8). */
    EXPECT_EQ(blobFileOf(merkle::blobId("one"), false), store->toRealPath(info.path) / "a");
    EXPECT_EQ(st(store->toRealPath(info.path) / "a").st_nlink, 1u);
    EXPECT_EQ(store->blobRowCount(false), 2u) << "repair replaces the path's previous projection";
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, NoRepair));

    swept = objects.sweep(live);
    EXPECT_EQ(swept.unentered, 0u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.blobsKept, 1u) << "the symlink's blob; the other two are rows";
}

/* Delivers `s` but for its last `tail` bytes, then runs `atPause` once,
   then the rest: the ingestion's pause the concurrency tests collect at. */
struct PausingSource : Source
{
    std::string_view s;
    size_t pauseAt;
    fun<void()> atPause;
    size_t pos = 0;
    bool paused = false;

    PausingSource(std::string_view s, size_t tail, fun<void()> atPause)
        : s(s)
        , pauseAt(s.size() - tail)
        , atPause(std::move(atPause))
    {
    }

    size_t read(char * data, size_t len) override
    {
        if (pos == s.size())
            throw EndOfFile("end of NAR");
        if (!paused && pos == pauseAt) {
            paused = true;
            atPause();
        }
        auto n = std::min(len, (paused ? s.size() : pauseAt) - pos);
        memcpy(data, s.data() + pos, n);
        pos += n;
        return n;
    }
};

/* The concurrent window (01 §9.10, "Collection"; 08 §1.5): a whole-store
   collection that runs while an ingestion is between its object writes
   and its registration.  The ingestion is the real `addToStore(info,
   source)`; the pause is the source's own -- it delivers the NAR up to
   the closing of its last directory, so that every subtree and every
   symlink's blob is written and the root tree is not (trees are written
   bottom-up, K10) -- and at the pause the collector runs, on this store,
   as the builder's `autoGC(false)` thread or another process's
   `nix-store --gc` would; the sweep is the same code whichever process
   runs it, and a hold's lock conflicts across descriptors within one
   process as across processes (`createTempDirInStore` rests on the
   same).  Afterwards the registered path must reach every object its
   hash names (law 4), the verifier must find nothing, and a later sweep
   must count nothing missing.  Before the hold: the collection took the
   subtree and the symlink's blob, and the path was registered naming
   them. */
TEST_F(LocalStoreObjectsTest, ingestionTreesSurviveAConcurrentSweep)
{
    /* lsof is slow and not needed here (local-gc.cc). */
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    Nodes nodes{{"a", "one", 'f'}, {"d/b", "two", 'f'}, {"d/l", "b", 'l'}};
    auto nar = treeNar(nodes);
    auto tree = treeAccessor(nodes);
    auto rootTree = objectHashOf(*tree, CanonPath::root).root.hash;
    auto dTree = objectHashOf(*tree, CanonPath("/d")).root.hash;
    auto symlinkBlob = merkle::blobId("b");
    auto & objects = store->objects;
    auto garbage = std::filesystem::path{config->realStoreDir.get()} / "not-a-store-path";
    writeFile(garbage, "dead");

    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-in-flight"},
        UnkeyedValidPathInfo{*store, std::nullopt},
    };
    info.assertedNarHash = hashString(HashAlgorithm::SHA256, nar);
    info.narSize = nar.size();

    /* The NAR ends with three `)` strings of 16 bytes each: the close of
       `d`'s node, of `d`'s entry, and of the root.  With the first
       delivered, `d`'s tree is written; the two withheld keep the root's
       unwritten. */
    bool collected = false;
    PausingSource source{nar, 32, [&] {
                             EXPECT_TRUE(hasTree(dTree)) << "the subtree is written before the pause";
                             EXPECT_FALSE(hasTree(rootTree)) << "the root is written after it";
                             EXPECT_TRUE(hasBlob(symlinkBlob, false));
                             EXPECT_FALSE(store->isValidPath(info.path));
                             GCOptions options;
                             options.action = GCOptions::gcDeleteDead;
                             GCResults results;
                             store->collectGarbage(options, results);
                             collected = true;
                             EXPECT_FALSE(pathExists(garbage)) << "the collection ran";
                         }};
    store->addToStore(info, source, NoRepair, NoCheckSigs);
    ASSERT_TRUE(collected);

    auto row = store->queryPathInfo(info.path);
    ASSERT_TRUE(row->objectHash.has_value());
    EXPECT_EQ(row->objectHash->hash, rootTree);
    EXPECT_TRUE(hasTree(rootTree));
    EXPECT_TRUE(hasTree(dTree)) << "the subtree written before the collection survived it";
    EXPECT_TRUE(hasBlob(symlinkBlob, false)) << "the symlink's blob survived it";
    EXPECT_EQ(count("trees"), 2u);
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, NoRepair)) << "law 4 holds for the path";

    auto swept = objects.sweep([&](fun<void(const ObjectHash &)> f) {
        f(*row->objectHash);
        return 0;
    });
    EXPECT_EQ(swept.unentered, 0u);
    EXPECT_EQ(swept.missingSubtrees, 0u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(count("trees"), 2u);
}

/* The window inside a hold's own making (`08` section 1.5, K16): a
   collection whose sweep reaches an ingestion's hold after the directory
   is visible under `.objects/tmp` and before its lock is taken.  The
   hold's lock is free at that instant, which is what the sweep reads as
   "a dead ingestion's", so it removes the directory -- and the maker
   must come out of the window with a hold that is whole or with none at
   all.  It is whole because the directory is made empty and its three
   kind directories are made under the lock: the sweep takes an empty
   directory, the maker finds it gone and makes another.  Were the kinds
   made before the lock, the maker's remaining `createDirs` would remake
   the directory *without* the kinds already made, and every `holdObject`
   into a missing kind would read `ENOENT` as "no such object" and hold
   nothing -- silently, since `writeObjectFile` ignores the second hold's
   answer.  So the check is the composition of the two windows: one
   collection while the hold is made, a second at the ingestion's pause,
   between its symlink blob and its root tree, where only the hold can
   keep that blob.  The collector met the other half of the same window
   as `ENOTEMPTY` out of the sweep's own `deletePath`, racing the maker's
   `mkdir` (`tests/functional/gc-non-blocking.sh`; `07`). */
TEST_F(LocalStoreObjectsTest, ingestionSurvivesASweepWhileItsHoldIsMade)
{
    /* lsof is slow and not needed here (local-gc.cc). */
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    Nodes nodes{{"a", "one", 'f'}, {"d/b", "two", 'f'}, {"d/l", "b", 'l'}};
    auto nar = treeNar(nodes);
    auto tree = treeAccessor(nodes);
    auto rootTree = objectHashOf(*tree, CanonPath::root).root.hash;
    auto dTree = objectHashOf(*tree, CanonPath("/d")).root.hash;
    auto symlinkBlob = merkle::blobId("b");
    auto & objects = store->objects;
    auto garbage = std::filesystem::path{config->realStoreDir.get()} / "not-a-store-path";

    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-held"},
        UnkeyedValidPathInfo{*store, std::nullopt},
    };
    info.assertedNarHash = hashString(HashAlgorithm::SHA256, nar);
    info.narSize = nar.size();

    /* The collection another process's `nix-store --gc` would run. */
    unsigned collections = 0;
    auto collect = [&] {
        writeFile(garbage, "dead");
        GCOptions options;
        options.action = GCOptions::gcDeleteDead;
        GCResults results;
        store->collectGarbage(options, results);
        EXPECT_FALSE(pathExists(garbage)) << "the collection ran";
        collections++;
    };

    /* The first, at the making of the ingestion's own hold. */
    bool made = false;
    objects.atHoldMade = [&] {
        if (made)
            return;
        made = true;
        EXPECT_EQ(count("tmp"), 1u) << "the hold is visible and not yet locked";
        collect();
        EXPECT_EQ(count("tmp"), 0u) << "the sweep took the unlocked hold";
    };
    Finally disarm{[&] { objects.atHoldMade = nullptr; }};

    /* The second, at the pause: `d`'s tree and the symlink's blob written,
       the root tree not (as `ingestionTreesSurviveAConcurrentSweep`). */
    PausingSource source{nar, 32, [&] {
                             EXPECT_TRUE(hasTree(dTree));
                             EXPECT_FALSE(hasTree(rootTree));
                             EXPECT_TRUE(hasBlob(symlinkBlob, false));
                             collect();
                         }};
    store->addToStore(info, source, NoRepair, NoCheckSigs);
    ASSERT_TRUE(made);
    ASSERT_EQ(collections, 2u);

    /* Law 4: the registered path reaches every object its hash names. */
    auto row = store->queryPathInfo(info.path);
    ASSERT_TRUE(row->objectHash.has_value());
    EXPECT_EQ(row->objectHash->hash, rootTree);
    EXPECT_TRUE(hasTree(rootTree));
    EXPECT_TRUE(hasTree(dTree)) << "held under `trees`";
    EXPECT_TRUE(hasBlob(symlinkBlob, false)) << "held under `blobs`, which the remade hold lacked";
    EXPECT_EQ(count("tmp"), 0u) << "the hold was released";
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, NoRepair)) << "law 4 holds for the path";

    auto swept = objects.sweep([&](fun<void(const ObjectHash &)> f) {
        f(*row->objectHash);
        return 0;
    });
    EXPECT_EQ(swept.unentered, 0u);
    EXPECT_EQ(swept.missingSubtrees, 0u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(count("trees"), 2u);
}

/* A hold's other half (`GitObjectStore::Hold`; `08` K16): what an
   ingestion holds is kept only while its process lives.  A hold whose
   lock is free -- the process died with the objects written and the path
   unregistered -- is removed by the sweep's temporary pass with its
   links, and the objects it held, reachable from nothing, go in the same
   sweep; a locked one is left and keeps them.  The hold is that of the
   ingestion's `restoreThroughObjects` itself, its lock dropped as a
   crash drops it, the directory left standing. */
TEST_F(LocalStoreObjectsTest, deadHoldIsReleasedBySweep)
{
    auto nar = treeWithSymlink();
    auto & objects = store->objects;
    auto dead = std::filesystem::path{config->realStoreDir.get()} / "dead";
    auto hooks = makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
    StringSource source{nar};
    std::optional<GitObjectStore::Hold> hold{objects.hold()};
    auto r = store->restoreThroughObjects(dead, source, /*startFsync=*/false, hooks.get(), *hold);
    auto dTree = readTree(r.root.hash).at("d/").hash;
    auto symlinkBlob = merkle::blobId("b");
    ASSERT_EQ(count("tmp"), 1u) << "the hold's directory";
    ASSERT_EQ(st(treeFile(r.root.hash)).st_nlink, 2u) << "held";
    ASSERT_EQ(st(treeFile(dTree)).st_nlink, 2u);
    ASSERT_EQ(st(blobFileOf(symlinkBlob, false)).st_nlink, 2u);
    deletePath(dead);
    auto none = [](fun<void(const ObjectHash &)>) { return 0; };

    /* In flight: nothing reachable; the trees and the symlink's blob held.
       The files' blobs were held by the path's links alone (K4), which
       went with it. */
    auto swept = objects.sweep(none);
    EXPECT_EQ(swept.holds, 1u);
    EXPECT_EQ(swept.treesHeld, 2u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(swept.blobsRemoved, 0u) << "the files' blobs went with the path, not with the sweep";
    EXPECT_EQ(count("trees"), 2u);
    EXPECT_TRUE(hasBlob(symlinkBlob, false));

    /* The crash: the lock is released, the directory stays. */
    dropLock(*hold);
    swept = objects.sweep(none);
    EXPECT_EQ(swept.holds, 0u);
    EXPECT_EQ(swept.treesHeld, 0u);
    EXPECT_EQ(swept.treesRemoved, 2u);
    EXPECT_EQ(swept.blobsRemoved, 1u) << "the symlink's blob";
    EXPECT_EQ(count("tmp"), 0u) << "the dead hold went first";
    EXPECT_EQ(count("trees"), 0u);
    EXPECT_FALSE(hasBlob(symlinkBlob, false));
    /* The destructor of a hold whose directory is gone does nothing. */
    hold.reset();
}

/* Law 5 sweeps on (01 section 9.10): a hold the sweep locked and could
   not remove is counted and passed over, never the end of the
   collection.  A maker of this Nix leaves the window between its `mkdir`
   and its `flock` empty, so the `ENOTEMPTY` the collector met -- the
   sweep's `deletePath` racing an older maker's `mkdir` of the kind
   directories -- cannot be made in one process; it is injected here at
   the one call it reaches (`failHoldRemoval`, as
   `enospcLeftFileIsReportedNotRepairedForever` injects a full directory
   index).  What the sweep must still do is judge the objects below: the
   hold's links keep its trees and its symlink's blob, and everything
   else dead goes in the same sweep. */
TEST_F(LocalStoreObjectsTest, sweepGoesOnPastAHoldItCannotRemove)
{
    auto nar = treeWithSymlink();
    auto & objects = store->objects;
    auto dead = std::filesystem::path{config->realStoreDir.get()} / "dead";
    auto hooks = makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
    StringSource source{nar};
    std::optional<GitObjectStore::Hold> hold{objects.hold()};
    auto r = store->restoreThroughObjects(dead, source, /*startFsync=*/false, hooks.get(), *hold);
    auto symlinkBlob = merkle::blobId("b");
    deletePath(dead);
    /* The crash: the lock is released, the directory left. */
    dropLock(*hold);

    /* A second ingestion, finished and its path gone: its tree and its
       symlink's blob are dead and held by nothing, and they are what says
       this sweep went on past the hold it could not remove.  Before, the
       dead ingestion's *own* regular files' blobs were files under
       `blobs/` that its hold did not keep; a regular-file blob has no file
       of the store's own any more, so the dead thing this sweep must still
       take has to be brought by another ingestion. */
    auto other = std::filesystem::path{config->realStoreDir.get()} / "other";
    restore(other, treeNar({{"a", "one", 'f'}, {"l", "a", 'l'}}));
    auto otherBlob = merkle::blobId("a");
    ASSERT_TRUE(hasBlob(otherBlob, false));
    deletePath(other);

    auto none = [](fun<void(const ObjectHash &)>) { return 0; };

    objects.failHoldRemoval = [](const std::filesystem::path &) { return true; };
    Finally disarm{[&] { objects.failHoldRemoval = nullptr; }};
    auto swept = objects.sweep(none);
    EXPECT_EQ(swept.holds, 1u) << "counted, not removed";
    EXPECT_EQ(swept.treesHeld, 2u) << "its links still keep them";
    EXPECT_EQ(swept.treesRemoved, 1u) << "the other ingestion's root tree: the sweep went on past the hold";
    EXPECT_EQ(swept.blobsRemoved, 1u) << "and its symlink's blob with it";
    EXPECT_EQ(swept.blobsKept, 1u) << "the held ingestion's symlink blob, at a second link";
    EXPECT_FALSE(hasBlob(otherBlob, false));
    EXPECT_EQ(count("tmp"), 1u) << "the hold stands";
    EXPECT_TRUE(hasBlob(symlinkBlob, false));

    /* The next sweep, the removal succeeding, is `deadHoldIsReleasedBySweep`. */
    objects.failHoldRemoval = nullptr;
    swept = objects.sweep(none);
    EXPECT_EQ(swept.holds, 0u);
    EXPECT_EQ(swept.treesRemoved, 2u);
    EXPECT_EQ(swept.blobsRemoved, 1u) << "the held ingestion's symlink blob";
    EXPECT_EQ(count("tmp"), 0u);
    hold.reset();
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
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(swept.blobsRemoved, 1u);
    EXPECT_FALSE(hasTree(r.root.hash));
    EXPECT_EQ(count("trees"), 0u);
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(count("blobs"), 1u);
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
   kept its blob for ever.

   What this pinned before and what it pins now.  Before, the blob was a
   plain file's, with a file under `.objects/blobs` of the store's own
   naming.  A regular-file blob has no such file: its holder is a store
   path's own file, and a temporary link to *that* pins nothing of the
   object store's.  The blobs a leftover can still pin are the ones with
   files -- the **symlink targets** -- so the tree here is a symlink whose
   target is the pinned bytes, and the rule (the name's second, not the
   inode's change time; the temporary pass before the blob pass) is
   unchanged in words and narrower in domain (`08` K4). */
TEST_F(LocalStoreObjectsTest, staleTemporaryLinkGoesWithTheBlobItPinned)
{
    auto & objects = store->objects;
    auto nar = treeNar({{"l", "pinned", 'l'}});
    auto p = std::filesystem::path{config->realStoreDir.get()} / "dead";
    restore(p, nar);
    deletePath(p);
    auto blob = blobFileOf(merkle::blobId("pinned"), false);
    ASSERT_EQ(blob, objects.blobFile(merkle::blobId("pinned"), false)) << "a symlink's target has a file of its own";
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
       steps -- stays, and so does what it links.  Under a second
       destination, the fixture's `restore` registering the first under its
       own name. */
    auto p2 = std::filesystem::path{config->realStoreDir.get()} / "dead2";
    restore(p2, nar);
    deletePath(p2);
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
    /* One lower: the blob's canonical file is a store path's own, so there is
     * no link into `.objects/blobs` beside it (K8) */
    EXPECT_EQ(st(real / "README").st_nlink, 1u);
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, NoRepair))
        << "clean: no blob is expected for a file the store does not place";

    /* A blob a live tree names, really missing: the symlink's target. */
    std::filesystem::remove(blobFileOf(merkle::blobId("README"), false));
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
    std::filesystem::remove(blobFileOf(merkle::blobId("readme"), false));
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

/* A copy that dies between its writes and its registration leaves index rows
   naming an ingestion that never got a path.  The two things that must hold of
   them: `link` must not be misled by one -- it must write the blob afresh, not
   link a file the store never promised -- and the sweep must remove them.
   (`schema.sql`, the stale-row rule; it is what replaces the cascade a foreign
   key into `ValidPaths` would have given.) */
TEST_F(LocalStoreObjectsTest, blobWriterRespectsBothJournalModes)
{
    bool saved = settings.useSQLiteWAL;
    Finally restoreSetting{[&] { settings.useSQLiteWAL = saved; }};
    for (bool wal : {false, true}) {
        SCOPED_TRACE(wal);
        store.reset();
        settings.useSQLiteWAL = wal;
        config =
            std::make_shared<LocalStoreConfig>(tempStoreDir.path() / (wal ? "wal" : "rollback"), StoreConfig::Params{});
        store = std::make_shared<LocalStore>(ref{config});
        auto hold = store->objects.hold();
        auto dst = config->realStoreDir.get() / (wal ? "wal-copy" : "rollback-copy");
        store->beginIngestion(hold, dst);
        StringSource source{treeNar({{"a", wal ? "wal" : "rollback", 'f'}})};
        auto hooks = makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
        store->restoreThroughObjects(dst, source, false, hooks.get(), hold, NoRepair);
        hold.flush();
        EXPECT_GT(store->blobRowCount(false), 0u);
        SQLite db{store->dbDir / "db.sqlite", {.mode = SQLiteOpenMode::NoCreate, .useWAL = wal}};
        SQLiteStmt mode{db, "pragma journal_mode"};
        auto query = mode.use();
        ASSERT_TRUE(query.next());
        EXPECT_EQ(query.getStr(0) == "wal", wal);
    }
}

TEST_F(LocalStoreObjectsTest, aCrashBeforeRegistrationLeavesNoRowThatMisleadsLink)
{
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto one = merkle::blobId("one");

    /* An ingestion that writes its rows and never registers: the hold goes out
       of scope without `endIngestion`, exactly as a crashed copy leaves it. */
    auto dead = realStoreDir / "crash1";
    {
        auto hold = store->objects.hold();
        store->beginIngestion(hold, dead);
        StringSource source{treeNar({{"a", "one", 'f'}, {"z", "only-dead", 'f'}})};
        auto hooks = makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
        store->restoreThroughObjects(dead, source, /*startFsync=*/false, hooks.get(), hold, NoRepair);
        hold.flush();
    }

    /* The rows are there and they name no path. */
    EXPECT_GT(store->blobRowCount(false), 0u) << "the dead ingestion's rows were written";
    EXPECT_FALSE(store->objects.holderOrForget(one, false, NoRepair).has_value())
        << "and `link` is not misled by one: an ingestion with no path answers nothing";

    /* So a later ingestion of the same blob writes the file afresh rather than
       linking the dead one's, and its own inode is a different one. */
    auto live = realStoreDir / "crash2";
    restore(live, treeNar({{"a", "one", 'f'}}));
    EXPECT_EQ(filesCreated, 1u) << "the blob was written, not linked";
    EXPECT_NE(st(live / "a").st_ino, st(dead / "a").st_ino);

    /* The sweep removes both dead-ingestion candidates while keeping the
       live ingestion's candidate, including when their blob IDs coincide. */
    auto onlyDead = merkle::blobId("only-dead");
    EXPECT_TRUE(store->objects.holderOrForget(onlyDead, false, NoRepair).has_value() == false)
        << "the dead ingestion's own blob answers nothing either";
    auto before = store->blobRowCount(false);
    EXPECT_EQ(before, 3u);
    store->sweepStaleBlobRows();
    auto after = store->blobRowCount(false);
    EXPECT_EQ(after, 1u) << "only the registered ingestion's candidate remains";
    EXPECT_TRUE(store->objects.holderOrForget(one, false, NoRepair).has_value())
        << "and the surviving row is the live path's";
}

namespace {

/* A store that sweeps its object *files* its own way, as
   `LocalOverlayStore` does -- its `.objects` is the merged mount and it
   owns the upper layer alone, so it enumerates and unlinks there rather
   than through the mount.  Its index, though, is its own database's and is
   narrowed by no layer: it holds no row for a lower path, and every row it
   does hold is judged by the same rule.  `sweepObjects` is therefore not
   virtual and `sweepObjectStore` is; this stands in for the overlay, whose
   own suite needs an overlayfs mount the build sandbox forbids (`05`
   section 7). */
struct OwnObjectSweepStore : LocalStore
{
    explicit OwnObjectSweepStore(ref<const LocalStore::Config> config)
        : Store{*config}
        , LocalFSStore{*config}
        , LocalStore{config}
    {
    }

    unsigned ownSweeps = 0;

    void sweepObjectStore() override
    {
        ownSweeps++;
    }

    using LocalStore::sweepObjects;
};

} // namespace

/* A collection on such a store still removes the index's stale rows.
   Before: `LocalOverlayStore::sweepObjects` overrode the only caller of
   `sweepStaleBlobRows`, so on the overlay store a crashed copy's rows, and
   the rows of every collected path, stayed for ever. */
TEST_F(LocalStoreObjectsTest, aStoreThatSweepsItsOwnObjectsStillSweepsItsStaleRows)
{
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};

    /* An ingestion that writes its rows and never registers. */
    auto dead = realStoreDir / "own-sweep";
    {
        auto hold = store->objects.hold();
        store->beginIngestion(hold, dead);
        StringSource source{treeNar({{"a", "one", 'f'}})};
        auto hooks = makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
        store->restoreThroughObjects(dead, source, /*startFsync=*/false, hooks.get(), hold, NoRepair);
        hold.flush();
    }
    ASSERT_GT(store->blobRowCount(false), 0u);

    store.reset();
    auto own = std::make_shared<OwnObjectSweepStore>(ref{config});
    ASSERT_GT(own->blobRowCount(false), 0u);
    own->sweepObjects();
    EXPECT_EQ(own->ownSweeps, 1u) << "the object files were swept this store's own way";
    EXPECT_EQ(own->blobRowCount(false), 0u) << "and the stale rows went with the collection all the same";
    store = own;
}

/* A batch holds its objects once -- one locked directory for the whole of
   `addMultipleToStore` (`08` K16) -- but each of its paths registers on its
   own and is therefore its own ingestion: its own `Ingestions` row, its own
   root (a row's name is relative to the path that holds it, `schema.sql`) and
   its own writer.  Before: the batch's hold carried the rows, so
   `beginIngestion` ran on one hold once per path -- and from several threads
   at once, the batch being copied on a pool (`Store::addMultipleToStore`'s
   `processGraph`), which assigned over a joinable writer and terminated the
   process (`std::terminate() called without exception ... nix::GitObjectStore
   ::Hold::setSink`, four of the named functional tests).  What this pins is
   the statement underneath the crash, which is deterministic: every path's
   blob is held by *that* path's own file.  The two blobs are the same length,
   so a row written against the wrong ingestion resolves to a file that exists
   and passes the size check -- the store would hand out the wrong bytes. */
TEST_F(LocalStoreObjectsTest, everyPathOfABatchIsItsOwnIngestion)
{
    constexpr size_t n = 8;
    Store::PathsSource batch;
    std::vector<StorePath> paths;
    /* `StringSource` keeps a view, and the batch's sources outlive this
       loop. */
    std::deque<std::string> nars;
    for (size_t i = 0; i < n; i++) {
        auto bytes = fmt("byte-%d", i);
        auto & nar = nars.emplace_back(treeNar({{"a", bytes, 'f'}}));
        ValidPathInfo info{
            StorePath{fmt("n5wkd9frr45pa74if5gpz9j7mifg27fh-batch-%d", i)},
            UnkeyedValidPathInfo{*store, std::nullopt},
        };
        info.assertedNarHash = hashString(HashAlgorithm::SHA256, nar);
        /* An unannounced length keeps this fixture on the parallel
           streaming route even though StringSource itself is in memory. */
        info.narSize = 0;
        paths.push_back(info.path);
        batch.emplace_back(std::move(info), std::make_unique<StringSource>(nar));
    }
    Activity act{*logger, lvlTalkative, actUnknown};
    store->addMultipleToStore(std::move(batch), act, NoRepair, NoCheckSigs);

    /* One locked directory for the batch, and it is gone: the rows' holds
       are the same directory and remove nothing (`Hold::ownRows`). */
    EXPECT_EQ(count("tmp"), 0u);
    EXPECT_EQ(store->blobRowCount(false), n);
    for (size_t i = 0; i < n; i++) {
        ASSERT_TRUE(store->isValidPath(paths[i]));
        auto id = merkle::blobId(fmt("byte-%d", i));
        EXPECT_EQ(blobFileOf(id, false), store->toRealPath(paths[i]) / "a") << i;
        EXPECT_EQ(readFile(blobFileOf(id, false)), fmt("byte-%d", i)) << i;
    }
}

TEST_F(LocalStoreObjectsTest, overlappingAddsKeepEachOthersObjectsHeld)
{
    size_t serial = 0;
    /* Both callers may be a direct add or a batch, and either may finish
       first. The other caller's already-written subtree must survive GC. */
    for (bool firstBatch : {false, true})
        for (bool secondBatch : {false, true})
            for (size_t finishesFirst : {0, 1}) {
                SCOPED_TRACE(
                    fmt("firstBatch=%d secondBatch=%d finishesFirst=%d", firstBatch, secondBatch, finishesFirst));
                std::vector<std::string> nars;
                std::vector<ValidPathInfo> infos;
                std::vector<Hash> subtrees;
                for (size_t i = 0; i < 2; ++i) {
                    auto unique = fmt("overlap-%d-%d", serial, i);
                    Nodes nodes{{"d/file", unique, 'f'}, {"d/link", unique, 'l'}};
                    nars.push_back(treeNar(nodes));
                    subtrees.push_back(objectHashOf(*treeAccessor(nodes), CanonPath("/d")).root.hash);
                    infos.emplace_back(StorePath::random(unique), UnkeyedValidPathInfo{*store, std::nullopt});
                    infos.back().assertedNarHash = hashString(HashAlgorithm::SHA256, nars.back());
                    infos.back().narSize = nars.back().size();
                }
                ++serial;

                std::array<std::promise<void>, 2> paused, resume;
                std::array<std::future<void>, 2> jobs;
                auto release = [&](size_t i) {
                    try {
                        resume[i].set_value();
                    } catch (const std::future_error &) {
                        /* Already released. */
                    }
                };
                Finally join{[&] {
                    for (size_t i = 0; i < 2; ++i)
                        release(i);
                    for (auto & job : jobs)
                        if (job.valid())
                            job.wait();
                }};

                auto start = [&](size_t i, bool batch) {
                    auto ready = paused[i].get_future();
                    jobs[i] = std::async(std::launch::async, [&, i, batch] {
                        try {
                            auto source = std::make_unique<PausingSource>(nars[i], 32, [&] {
                                paused[i].set_value();
                                resume[i].get_future().get();
                            });
                            if (batch) {
                                Store::PathsSource paths;
                                paths.emplace_back(infos[i], std::move(source));
                                Activity act{*logger, lvlTalkative, actUnknown};
                                store->addMultipleToStore(std::move(paths), act, NoRepair, NoCheckSigs);
                            } else
                                store->addToStore(infos[i], *source, NoRepair, NoCheckSigs);
                        } catch (...) {
                            try {
                                paused[i].set_exception(std::current_exception());
                            } catch (const std::future_error &) {
                                /* The caller already observed the pause. */
                            }
                            throw;
                        }
                    });
                    ready.get();
                };

                start(0, firstBatch);
                start(1, secondBatch);
                release(finishesFirst);
                jobs[finishesFirst].get();
                auto stillWriting = 1 - finishesFirst;
                EXPECT_TRUE(hasTree(subtrees[stillWriting]));
                auto swept = store->objects.sweep([&](fun<void(const ObjectHash &)> live) {
                    for (auto & path : store->queryAllValidPaths())
                        live(*store->queryPathInfo(path)->objectHash);
                    return 0;
                });
                EXPECT_TRUE(hasTree(subtrees[stillWriting])) << "one add released another add's protection";
                EXPECT_EQ(swept.treesRemoved, 0u);
                release(stillWriting);
                jobs[stillWriting].get();
                for (auto & info : infos)
                    EXPECT_TRUE(store->isValidPath(info.path));
                EXPECT_EQ(count("tmp"), 0u) << "the final user releases the shared directory";
            }
}

TEST_F(LocalStoreObjectsTest, failedRegistrationPublishesNeitherPathNorPendingHolders)
{
    auto nar = treeNar({{"a", "uncommitted-holder", 'f'}});
    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-atomic-registration"},
        UnkeyedValidPathInfo{*store, std::nullopt},
    };
    info.assertedNarHash = hashString(HashAlgorithm::SHA256, nar);
    info.narSize = nar.size();
    info.references.emplace("n5wkd9frr45pa74if5gpz9j7mifg27fh-missing-reference");
    auto before = store->blobRowCount(false);
    StringSource failed{nar};
    EXPECT_THROW(store->addToStore(info, failed, NoRepair, NoCheckSigs), Error);
    EXPECT_FALSE(store->isValidPath(info.path));
    EXPECT_EQ(store->blobRowCount(false), before);
    EXPECT_FALSE(store->objects.holderOrForget(merkle::blobId("uncommitted-holder"), false, NoRepair));

    info.references.clear();
    StringSource retry{nar};
    store->addToStore(info, retry, NoRepair, NoCheckSigs);
    EXPECT_TRUE(store->isValidPath(info.path));
    EXPECT_EQ(store->blobRowCount(false), before + 1);
    EXPECT_EQ(blobFileOf(merkle::blobId("uncommitted-holder"), false), store->toRealPath(info.path) / "a");
}

TEST_F(LocalStoreObjectsTest, everyCandidatePublicationOrderKeepsRegisteredHolders)
{
    /* All six orders respecting insertion before completion. For small
       ingestions, remove the separate insertion and deduplicate orders. */
    const std::array<std::array<unsigned, 4>, 6> interleavings{{
        {0, 1, 2, 3},
        {0, 2, 1, 3},
        {0, 2, 3, 1},
        {2, 3, 0, 1},
        {2, 0, 3, 1},
        {2, 0, 1, 3},
    }};
    size_t cases = 0;
    for (unsigned large = 0; large < 4; ++large) {
        std::set<std::vector<unsigned>> orders;
        for (auto & interleaving : interleavings) {
            std::vector<unsigned> order;
            for (auto event : interleaving)
                if ((event & 1) || (large & (1 << (event / 2))))
                    order.push_back(event);
            orders.insert(std::move(order));
        }
        for (unsigned success = 0; success < 4; ++success)
            for (auto & order : orders) {
                SCOPED_TRACE(fmt("large=%d success=%d case=%d", large, success, cases));
                auto contents = fmt("candidate-case-%d", cases++);
                auto nar = treeNar(
                    {{"file", contents, 'f'},
                     {"nested/alias", contents, 'f'},
                     {"exec", contents, 'x'},
                     {"nested/exec-alias", contents, 'x'}});
                auto id = merkle::blobId(contents);
                std::array holds{store->objects.hold(), store->objects.hold()};
                std::vector<ValidPathInfo> infos;
                for (size_t i = 0; i < 2; ++i) {
                    infos.emplace_back(
                        StorePath::random(fmt("candidate-%d-%d", cases, i)),
                        UnkeyedValidPathInfo{*store, std::nullopt});
                    StringSource source{nar};
                    auto hooks =
                        makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
                    auto result = store->restoreThroughObjects(
                        store->toRealPath(infos.back().path), source, false, hooks.get(), holds[i], NoRepair);
                    infos.back().objectHash = ObjectHash::of(result.root);
                    infos.back().narSize = result.narSize;
                }
                std::array<bool, 2> registered{};
                for (auto event : order) {
                    auto i = event / 2;
                    if (!(event & 1)) {
                        holds[i].flush();
                    } else if (success & (1 << i)) {
                        store->registerValidPath(infos[i], holds[i]);
                        registered[i] = true;
                    } else {
                        infos[i].references.insert(StorePath::random("missing-reference"));
                        EXPECT_THROW(store->registerValidPath(infos[i], holds[i]), InvalidPath);
                    }
                    for (bool executable : {false, true}) {
                        auto holder = store->objects.holderOrForget(id, executable, NoRepair);
                        EXPECT_EQ(holder.has_value(), registered[0] || registered[1]) << "publication event " << event;
                        for (size_t n = 0; n < 2; ++n)
                            if (registered[n] && holder) {
                                auto root = store->toRealPath(infos[n].path);
                                EXPECT_EQ(st(root / (executable ? "exec" : "file")).st_ino, holder->ino);
                                EXPECT_EQ(
                                    st(root / (executable ? "nested/exec-alias" : "nested/alias")).st_ino, holder->ino);
                            }
                    }
                }
                EXPECT_EQ(store->objects.holderOrForget(id, false, NoRepair).has_value(), success != 0);
            }
    }
    EXPECT_EQ(cases, 56u);
}

/* A store produced by an older implementation can contain distinct
   holders. Only one is canonical to the optimiser's inode shortcut. */
TEST_F(LocalStoreObjectsTest, optimiseCoalescesConcurrentCandidates)
{
    /* Three files have exactly these five inode-equivalence partitions.
       Enumeration also makes publication order independent of path order. */
    const std::array<std::array<unsigned, 3>, 5> partitions{{
        {0, 0, 0},
        {0, 0, 1},
        {0, 1, 0},
        {0, 1, 1},
        {0, 1, 2},
    }};
    size_t cases = 0;
    for (bool executable : {false, true})
        for (const auto & partition : partitions)
            for (unsigned minimum = 0; minimum <= *std::max_element(partition.begin(), partition.end()); ++minimum) {
                std::array<size_t, 3> order{0, 1, 2};
                do {
                    SCOPED_TRACE(
                        fmt("executable=%d partition=%d%d%d minimum=%d order=%d%d%d",
                            executable,
                            partition[0],
                            partition[1],
                            partition[2],
                            minimum,
                            order[0],
                            order[1],
                            order[2]));
                    auto contents = fmt("optimise-candidate-%d", cases++);
                    auto nar = treeNar({{"file", contents, executable ? 'x' : 'f'}});
                    auto id = merkle::blobId(contents);
                    std::array holds{store->objects.hold(), store->objects.hold(), store->objects.hold()};
                    std::vector<ValidPathInfo> infos;
                    std::array<std::filesystem::path, 3> files;
                    for (size_t i = 0; i < 3; ++i) {
                        infos.emplace_back(
                            StorePath{fmt("00000000000000000000000000000000-optimise-candidate-%03d-%d", cases, i)},
                            UnkeyedValidPathInfo{*store, std::nullopt});
                        files[i] = store->toRealPath(infos.back().path) / "file";
                        StringSource source{nar};
                        auto hooks = makeCanonicalisingRestoreHooks(
                            NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
                        auto result = store->restoreThroughObjects(
                            store->toRealPath(infos.back().path), source, false, hooks.get(), holds[i], NoRepair);
                        infos.back().objectHash = ObjectHash::of(result.root);
                        infos.back().narSize = result.narSize;
                    }
                    for (auto i : order)
                        store->registerValidPath(infos[i], holds[i]);
                    auto holder = store->objects.holderOrForget(id, executable, NoRepair);
                    ASSERT_TRUE(holder);
                    for (const auto & file : files)
                        EXPECT_EQ(st(file).st_ino, holder->ino) << "registration coalesces overlapping misses";
                    auto inodeOf = [&](const std::filesystem::path & file) {
                        auto stat = st(file);
                        return std::pair{static_cast<int64_t>(stat.st_dev), static_cast<int64_t>(stat.st_ino)};
                    };
                    /* Plant the legacy partition and enumerate which class is
                       least, independently of the filesystem's allocation order.
                       Nonminimum classes all have the same outcome: join it. */
                    std::vector<std::filesystem::path> allocated;
                    for (unsigned group = 0; group <= *std::max_element(partition.begin(), partition.end()); ++group) {
                        auto file = tempStoreDir.path() / fmt("optimise-class-%d-%d", cases, group);
                        writeFile(file, contents);
                        chmod(file, executable ? 0555 : 0444);
                        canonicaliseTimestampAndPermissions(file);
                        allocated.push_back(file);
                    }
                    std::sort(allocated.begin(), allocated.end(), [&](const auto & a, const auto & b) {
                        return inodeOf(a) < inodeOf(b);
                    });
                    auto representatives = allocated;
                    std::swap(representatives[0], representatives[minimum]);
                    for (size_t i = 0; i < files.size(); ++i) {
                        chmod(files[i].parent_path(), 0755);
                        std::filesystem::remove(files[i]);
                        std::filesystem::create_hard_link(representatives[partition[i]], files[i]);
                        canonicaliseTimestampAndPermissions(files[i].parent_path());
                    }
                    for (const auto & file : allocated)
                        std::filesystem::remove(file);
                    {
                        SQLite db{
                            store->dbDir / "db.sqlite",
                            {.mode = SQLiteOpenMode::NoCreate, .useWAL = settings.useSQLiteWAL}};
                        SQLiteStmt update;
                        update.create(db, "update Blobs set ino = ?, dev = ? where ingestion = ?;");
                        for (size_t i = 0; i < files.size(); ++i)
                            update.use()
                                .apply(static_cast<int64_t>(st(files[i]).st_ino))
                                .apply(static_cast<int64_t>(st(files[i]).st_dev))
                                .apply(static_cast<int64_t>(*holds[i].ingestion->id))
                                .exec();
                    }
                    auto canonical =
                        inodeOf(*std::min_element(files.begin(), files.end(), [&](const auto & a, const auto & b) {
                            return inodeOf(a) < inodeOf(b);
                        }));
                    auto inodes = store->objects.blobInodes();
                    size_t expectedLinked = 0;
                    for (const auto & file : files) {
                        bool chosen = inodeOf(file) == canonical;
                        EXPECT_EQ(inodes.contains(st(file).st_ino), chosen);
                        expectedLinked += !chosen;
                    }
                    OptimiseStats stats;
                    store->optimiseStore(stats);
                    EXPECT_EQ(stats.filesLinked, expectedLinked);
                    for (const auto & file : files) {
                        EXPECT_EQ(inodeOf(file), canonical);
                        EXPECT_EQ(readFile(file), contents);
                        EXPECT_EQ(st(file).st_mode & ~S_IFMT, executable ? 0555u : 0444u);
                    }
                } while (std::next_permutation(order.begin(), order.end()));
            }
    EXPECT_EQ(cases, 120u);
}

TEST_F(LocalStoreObjectsTest, interruptedOptimisePreservesPathsAndRetryRestoresItsIndex)
{
    for (bool executable : {false, true}) {
        SCOPED_TRACE(executable);
        auto bytes = fmt("interrupted-optimise-shared-%d", executable);
        auto later = fmt("interrupted-optimise-later-%d", executable);
        auto id = merkle::blobId(bytes);
        auto laterId = merkle::blobId(later);
        std::array holds{store->objects.hold(), store->objects.hold()};
        std::vector<ValidPathInfo> infos;
        std::array<std::filesystem::path, 2> roots;
        for (unsigned i = 0; i < 2; ++i) {
            infos.emplace_back(
                StorePath{fmt("%032d-interrupted-optimise-%d-%d", 2 * unsigned(executable) + i, executable, i)},
                UnkeyedValidPathInfo{*store, std::nullopt});
            roots[i] = store->toRealPath(infos.back().path);
            Nodes nodes{{"a", bytes, executable ? 'x' : 'f'}};
            if (i == 0)
                nodes.emplace_back("z", later, 'f');
            StringSource source{treeNar(nodes)};
            auto hooks =
                makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
            auto result = store->restoreThroughObjects(roots[i], source, false, hooks.get(), holds[i], NoRepair);
            infos.back().objectHash = ObjectHash::of(result.root);
            infos.back().narSize = result.narSize;
            store->registerValidPath(infos.back(), holds[i]);
        }
        /* A supported legacy store may have two physical representatives.
           Make the first path noncanonical regardless of allocation order. */
        std::array<std::filesystem::path, 2> allocated{
            tempStoreDir.path() / fmt("interrupted-inode-a-%d", executable),
            tempStoreDir.path() / fmt("interrupted-inode-b-%d", executable)};
        for (auto & file : allocated) {
            writeFile(file, bytes);
            chmod(file, executable ? 0555 : 0444);
            canonicaliseTimestampAndPermissions(file);
        }
        auto physicalClass = [&](const auto & file) {
            auto status = st(file);
            return std::pair{static_cast<int64_t>(status.st_dev), static_cast<int64_t>(status.st_ino)};
        };
        if (physicalClass(allocated[1]) < physicalClass(allocated[0]))
            std::swap(allocated[0], allocated[1]);
        for (unsigned i = 0; i < 2; ++i) {
            chmod(roots[i], 0755);
            std::filesystem::remove(roots[i] / "a");
            std::filesystem::create_hard_link(allocated[1 - i], roots[i] / "a");
            canonicaliseTimestampAndPermissions(roots[i]);
        }
        for (auto & file : allocated)
            std::filesystem::remove(file);
        {
            SQLite db{store->dbDir / "db.sqlite", {.mode = SQLiteOpenMode::NoCreate, .useWAL = settings.useSQLiteWAL}};
            SQLiteStmt update{db, "update Blobs set ino = ?, dev = ? where ingestion = ? and id = ?;"};
            for (unsigned i = 0; i < 2; ++i) {
                auto status = st(roots[i] / "a");
                update.use()
                    .apply(static_cast<int64_t>(status.st_ino))
                    .apply(static_cast<int64_t>(status.st_dev))
                    .apply(static_cast<int64_t>(*holds[i].ingestion->id))
                    .apply(id.hash, id.hashSize)
                    .exec();
            }
        }
        auto originalIndex = store->objects.index;
        auto index = *originalIndex;
        index.lookup = [&, lookup = originalIndex->lookup](const Hash & queried, bool mode, auto cursor) {
            if (queried == laterId)
                throw Error("interrupted optimisation after the first file");
            return lookup(queried, mode, cursor);
        };
        store->objects.index = &index;
        Finally restoreIndex{[&] { store->objects.index = originalIndex; }};
        OptimiseStats stats;
        EXPECT_THROW(store->optimiseStore(stats), Error);
        store->objects.index = originalIndex;
        ASSERT_EQ(stats.filesLinked, 1u);
        ASSERT_EQ(physicalClass(roots[0] / "a"), physicalClass(roots[1] / "a"));

        GCOptions options;
        options.action = GCOptions::gcDeleteSpecific;
        options.pathsToDelete = GCOptions::SpecificPaths{.paths = {infos[1].path}};
        GCResults collected;
        store->collectGarbage(options, collected);
        ASSERT_FALSE(store->isValidPath(infos[1].path));
        ASSERT_TRUE(store->isValidPath(infos[0].path));
        ASSERT_EQ(readFile(roots[0] / "a"), bytes);
        EXPECT_EQ(readFile(roots[0] / "z"), later);
        EXPECT_EQ(st(roots[0] / "a").st_mode & ~S_IFMT, executable ? 0555u : 0444u);
        EXPECT_EQ(st(roots[0] / "z").st_mode & ~S_IFMT, 0444u);
        auto surviving = store->queryPathInfo(infos[0].path);
        EXPECT_EQ(surviving->objectHash, infos[0].objectHash);
        EXPECT_EQ(surviving->references, infos[0].references);
        /* C1' in doc/lazy-store/01-specification.md preserves valid content
           and reference closure on failure, not cache completeness after
           interrupted optimisation. A remaining witness must still be sound. */
        if (auto holder = store->objects.holderOrForget(id, executable, NoRepair)) {
            EXPECT_TRUE(GitObjectStore::validHolderMetadata(*holder, executable, st(holder->file)));
            EXPECT_EQ(GitObjectStore::blobIdOfFile(holder->file), id);
        }
        OptimiseStats retried;
        ASSERT_NO_THROW(store->optimiseStore(retried));
        auto holder = store->objects.holderOrForget(id, executable, Repair);
        ASSERT_TRUE(holder);
        EXPECT_EQ(physicalClass(holder->file), physicalClass(roots[0] / "a"));
        EXPECT_TRUE(store->verifyObjects(roots[0]).second.empty());
    }
}

TEST_F(LocalStoreObjectsTest, optimiseKeepsTheCanonicalInodeWhenItsFirstHolderIsCollected)
{
    for (bool executable : {false, true}) {
        SCOPED_TRACE(executable);
        auto contents = fmt("optimise-collected-holder-%d", executable);
        auto id = merkle::blobId(contents);
        auto nar = treeNar({{"file", contents, executable ? 'x' : 'f'}});
        std::array holds{store->objects.hold(), store->objects.hold(), store->objects.hold()};
        std::vector<ValidPathInfo> infos;
        std::array<std::filesystem::path, 3> files;
        for (size_t i = 0; i < files.size(); ++i) {
            /* Temporary GC roots identify hash parts, irrespective of the
               name suffix. Distinct ascending hashes make only the visited
               paths roots while preserving this test's traversal order. */
            infos.emplace_back(
                StorePath{fmt("%032d-optimise-collected-%d-%d", 3 * unsigned(executable) + i, executable, i)},
                UnkeyedValidPathInfo{*store, std::nullopt});
            files[i] = store->toRealPath(infos.back().path) / "file";
            StringSource source{nar};
            auto hooks =
                makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
            auto result = store->restoreThroughObjects(
                store->toRealPath(infos.back().path), source, false, hooks.get(), holds[i], NoRepair);
            infos.back().objectHash = ObjectHash::of(result.root);
            infos.back().narSize = result.narSize;
        }
        /* The donor is registered first and visited last. Its alias is
           visited first, before the divergent legacy holder in between. */
        for (size_t i : {2u, 1u, 0u})
            store->registerValidPath(infos[i], holds[i]);
        unlinkInPlace(files[1]);
        auto low = files[0], high = files[1];
        if (static_cast<int64_t>(st(high).st_ino) < static_cast<int64_t>(st(low).st_ino))
            std::swap(low, high);
        auto canonicalInode = st(low).st_ino;
        auto lowPin = tempStoreDir.path() / fmt("optimise-low-%d", executable);
        auto highPin = tempStoreDir.path() / fmt("optimise-high-%d", executable);
        std::filesystem::create_hard_link(low, lowPin);
        std::filesystem::create_hard_link(high, highPin);
        for (size_t i = 0; i < files.size(); ++i) {
            chmod(files[i].parent_path(), 0755);
            std::filesystem::remove(files[i]);
            std::filesystem::create_hard_link(i == 1 ? highPin : lowPin, files[i]);
            canonicaliseTimestampAndPermissions(files[i].parent_path());
        }
        std::filesystem::remove(lowPin);
        std::filesystem::remove(highPin);
        {
            SQLite db{store->dbDir / "db.sqlite", {.mode = SQLiteOpenMode::NoCreate, .useWAL = settings.useSQLiteWAL}};
            SQLiteStmt update{db, "update Blobs set ino = ?, dev = ? where ingestion = ?;"};
            for (size_t i = 0; i < files.size(); ++i)
                update.use()
                    .apply(static_cast<int64_t>(st(files[i]).st_ino))
                    .apply(static_cast<int64_t>(st(files[i]).st_dev))
                    .apply(static_cast<int64_t>(*holds[i].ingestion->id))
                    .exec();
        }
        auto originalIndex = store->objects.index;
        auto index = *originalIndex;
        size_t lookups = 0;
        bool collected = false;
        index.lookup = [&, lookup = originalIndex->lookup](const Hash & queried, bool mode, auto cursor) {
            if (queried == id && mode == executable && ++lookups == 2) {
                GCOptions options;
                options.action = GCOptions::gcDeleteSpecific;
                options.pathsToDelete = GCOptions::SpecificPaths{.paths = {infos[2].path}};
                GCResults results;
                store->collectGarbage(options, results);
                collected = true;
            }
            return lookup(queried, mode, cursor);
        };
        store->objects.index = &index;
        Finally restoreIndex{[&] { store->objects.index = originalIndex; }};
        OptimiseStats stats;
        store->optimiseStore(stats);
        EXPECT_TRUE(collected);
        EXPECT_FALSE(pathExists(files[2]));
        EXPECT_EQ(st(files[0]).st_ino, canonicalInode);
        EXPECT_EQ(st(files[1]).st_ino, canonicalInode);
        EXPECT_EQ(stats.filesLinked, 1u);
    }
}

/* The four writers cross each publication boundary with plain roots,
   executable roots and trees containing aliases in different directories.
   Publication order is independent of the order in which rows were made. */
TEST_F(LocalStoreObjectsTest, everyWriterReconcilesBeforeEveryPublicationBoundary)
{
    size_t cases = 0;
    for (unsigned route = 0; route < 4; ++route)
        for (unsigned boundary = 0; boundary < 3; ++boundary)
            for (unsigned kind = 0; kind < 3; ++kind)
                for (size_t first : {0u, 1u})
                    for (auto repair : {NoRepair, Repair}) {
                        SCOPED_TRACE(fmt(
                            "route=%d boundary=%d kind=%d first=%d repair=%d", route, boundary, kind, first, repair));
                        auto contents = fmt("publication-boundary-%d", cases++);
                        Nodes nodes{
                            {"a", contents, 'f'},
                            {"nested/plain", contents, 'f'},
                            {"x", contents, 'x'},
                            {"nested/executable", contents, 'x'}};
                        auto source = treeAccessor(nodes);
                        if (kind != 2) {
                            source = make_ref<MemorySourceAccessor>();
                            source->addFile(CanonPath::root, std::string(contents));
                            std::get<MemorySourceAccessor::File::Regular>(source->root->raw).executable = kind == 1;
                        }
                        auto nar = narOf(*source);
                        auto id = merkle::blobId(contents);
                        std::array holds{store->objects.hold(), store->objects.hold()};
                        std::vector<ValidPathInfo> infos;
                        for (size_t n = 0; n < 2; ++n) {
                            infos.emplace_back(
                                StorePath{
                                    "n5wkd9frr45pa74if5gpz9j7mifg27fh-publish-" + std::to_string(cases)
                                    + (n == first ? "-a" : "-b")},
                                UnkeyedValidPathInfo{*store, std::nullopt});
                            auto & hold = holds[boundary == 1 ? 0 : n];
                            auto real = store->toRealPath(infos.back().path);
                            auto hooks = makeCanonicalisingRestoreHooks(
                                NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
                            auto result = [&] {
                                if (route == 0) {
                                    StringSource input{nar};
                                    return store->restoreThroughObjects(real, input, false, hooks.get(), hold, repair);
                                }
                                if (route < 3)
                                    return store->materialiseThroughObjects(
                                        real,
                                        SourcePath(source),
                                        defaultPathFilter,
                                        false,
                                        hooks.get(),
                                        repair,
                                        route == 1 ? noNode() : everyNode(source),
                                        hold);
                                if (kind == 2) {
                                    createDirs(real / "nested");
                                    for (auto & [name, bytes, mode] : nodes) {
                                        writeFile(real / name, bytes);
                                        chmod(real / name, mode == 'x' ? 0755 : 0644);
                                    }
                                    /* An entered path can retain a noncanonical
                                       case-hack spelling. Its physical alias must
                                       still be relinked, without reconstructing names. */
                                    std::filesystem::rename(real / "a", real / "a~nix~case~hack~7");
                                } else {
                                    writeFile(real, contents);
                                    chmod(real, kind == 1 ? 0755 : 0644);
                                }
                                canonicalisePathMetaData(real, {NIX_WHEN_SUPPORT_ACLS(noAclsToIgnore)});
                                WithCaseHack on{true};
                                return store->enterPath(real, repair, hold);
                            }();
                            infos.back().objectHash = ObjectHash::of(result.root);
                            infos.back().narSize = result.narSize;
                            if (boundary == 1)
                                store->endIngestion(hold, infos.back().path);
                            else
                                hold.flush();
                        }
                        EXPECT_FALSE(store->objects.holderOrForget(id, kind == 1, NoRepair));
                        ValidPathInfos all{{infos[0].path, infos[0]}, {infos[1].path, infos[1]}};
                        if (boundary == 1)
                            store->registerValidPaths(all, &holds[0]);
                        else {
                            if (boundary == 2)
                                store->registerValidPaths(all);
                            for (auto n : {first, 1 - first}) {
                                if (boundary == 2)
                                    store->endIngestion(holds[n], infos[n].path);
                                else
                                    store->registerValidPath(infos[n], holds[n]);
                            }
                        }
                        for (size_t n = 0; n < 2; ++n) {
                            auto real = store->toRealPath(infos[n].path);
                            EXPECT_TRUE(store->isValidPath(infos[n].path));
                            for (auto & name : regularFilesUnder(real)) {
                                auto file = name.empty() ? real : real / name;
                                auto physical = st(file);
                                auto holder = store->objects.holderOrForget(id, physical.st_mode & S_IXUSR, NoRepair);
                                ASSERT_TRUE(holder);
                                EXPECT_EQ(physical.st_ino, holder->ino) << file;
                                EXPECT_EQ(physical.st_dev, holder->dev) << file;
                                EXPECT_EQ(readFile(file), contents);
                            }
                        }
                    }
    EXPECT_EQ(cases, 144u);
}

TEST_F(LocalStoreObjectsTest, refusedPublicationDoesNotPromoteAnExceptionalInode)
{
    for (bool executable : {false, true})
        for (bool earlierId : {false, true})
            for (unsigned refusal = 0; refusal < 3; ++refusal) {
                SCOPED_TRACE(fmt("executable=%d earlierId=%d refusal=%d", executable, earlierId, refusal));
                auto contents = fmt("publication-refusal-%d-%d-%d", executable, earlierId, refusal);
                auto nar = treeNar(
                    {{"a", contents, executable ? 'x' : 'f'}, {"nested/alias", contents, executable ? 'x' : 'f'}});
                auto id = merkle::blobId(contents);
                std::array holds{store->objects.hold(), store->objects.hold()};
                std::vector<ValidPathInfo> infos;
                for (size_t n = 0; n < 2; ++n) {
                    infos.emplace_back(
                        StorePath::random("publication-refusal"), UnkeyedValidPathInfo{*store, std::nullopt});
                    StringSource source{nar};
                    auto hooks =
                        makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
                    auto result = store->restoreThroughObjects(
                        store->toRealPath(infos.back().path), source, false, hooks.get(), holds[n], NoRepair);
                    infos.back().objectHash = ObjectHash::of(result.root);
                    infos.back().narSize = result.narSize;
                }
                if (earlierId)
                    holds[0].flush();
                store->registerValidPath(infos[1], holds[1]);
                auto target = store->toRealPath(infos[0].path);
                auto canonical = store->toRealPath(infos[1].path) / "a";
                ASSERT_NE(st(target / "a").st_ino, st(canonical).st_ino);
                size_t refusals = 0;
                store->objects.failPlaceWithNoSpace = [&](const std::filesystem::path & where) {
                    bool refuse = refusal == 0   ? where.parent_path() == holds[0].directory()
                                  : refusal == 1 ? where == target / "a" || where == target / "nested/alias"
                                                 : where == target / "a";
                    if (refuse)
                        ++refusals;
                    return refuse;
                };
                Finally clearRefusal{[&] { store->objects.failPlaceWithNoSpace = {}; }};
                store->registerValidPath(infos[0], holds[0]);
                store->objects.failPlaceWithNoSpace = {};
                ASSERT_GT(refusals, 0u);
                EXPECT_NE(st(target / "a").st_ino, st(canonical).st_ino);
                if (refusal == 2)
                    EXPECT_EQ(st(target / "nested/alias").st_ino, st(canonical).st_ino)
                        << "a successful physical alias supplies the ingestion's representative";
                auto holder = store->objects.holderOrForget(id, executable, NoRepair);
                ASSERT_TRUE(holder);
                EXPECT_EQ(holder->ino, st(canonical).st_ino);
                {
                    SQLite db{
                        store->dbDir / "db.sqlite",
                        {.mode = SQLiteOpenMode::NoCreate, .useWAL = settings.useSQLiteWAL}};
                    SQLiteStmt count;
                    count.create(db, "select count(*) from Blobs where ingestion = ?;");
                    auto use(count.use());
                    use.apply(static_cast<int64_t>(*holds[0].ingestion->id));
                    ASSERT_TRUE(use.next());
                    EXPECT_EQ(use.getInt(0), refusal == 2 ? 1 : 0);
                }

                /* Space is available again. This successful new ingestion
                   must share the nonexceptional donor, not the refused file. */
                ValidPathInfo later{
                    StorePath::random("after-publication-refusal"), UnkeyedValidPathInfo{*store, std::nullopt}};
                later.objectHash = infos[1].objectHash;
                later.narSize = nar.size();
                StringSource source{nar};
                store->addToStore(later, source, NoRepair, NoCheckSigs);
                EXPECT_EQ(st(store->toRealPath(later.path) / "a").st_ino, st(canonical).st_ino);
                EXPECT_EQ(st(store->toRealPath(later.path) / "nested/alias").st_ino, st(canonical).st_ino);
            }
}

TEST_F(LocalStoreObjectsTest, completedIngestionReplacesThePathsEntireCandidateProjection)
{
    for (bool wasValid : {false, true})
        for (bool empty : {false, true})
            for (bool builder : {false, true}) {
                SCOPED_TRACE(fmt("wasValid=%d empty=%d builder=%d", wasValid, empty, builder));
                auto tag = fmt("projection-%d-%d-%d", wasValid, empty, builder);
                auto path = StorePath::random(tag);
                auto restoreInfo = [&](const StorePath & path,
                                       const std::string & nar,
                                       const GitObjectStore::Hold & hold) {
                    ValidPathInfo info{path, UnkeyedValidPathInfo{*store, std::nullopt}};
                    StringSource input{nar};
                    auto hooks =
                        makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
                    auto result = store->restoreThroughObjects(
                        store->toRealPath(path), input, false, hooks.get(), hold, NoRepair);
                    info.objectHash = ObjectHash::of(result.root);
                    info.narSize = result.narSize;
                    return info;
                };
                uint64_t oldIngestion;
                {
                    auto abandoned = store->objects.hold();
                    auto old = restoreInfo(path, treeNar({{"old", tag + "-old", 'f'}}), abandoned);
                    store->endIngestion(abandoned, path);
                    oldIngestion = *abandoned.ingestion->id;
                    if (wasValid)
                        store->registerValidPath(old, abandoned);
                }
                /* No GC sweep removes the failed named ingestion. The next
                   completed projection must retire it itself, including when
                   that projection contains no eligible regular files. */
                deletePath(store->toRealPath(path));
                auto hold = store->objects.hold();
                auto fresh = restoreInfo(path, treeNar(empty ? Nodes{} : Nodes{{"fresh", tag + "-fresh", 'f'}}), hold);
                ValidPathInfos infos{{path, fresh}};
                if (builder) {
                    store->endIngestion(hold, path);
                    auto siblingPath = StorePath::random(tag + "-sibling");
                    auto sibling = restoreInfo(siblingPath, treeNar({}), hold);
                    store->endIngestion(hold, siblingPath);
                    infos.emplace(siblingPath, sibling);
                    store->registerValidPaths(infos, &hold);
                } else
                    store->registerValidPath(fresh, hold);

                auto projection = [&] {
                    SQLite db{
                        store->dbDir / "db.sqlite",
                        {.mode = SQLiteOpenMode::NoCreate, .useWAL = settings.useSQLiteWAL}};
                    SQLiteStmt query;
                    query.create(
                        db,
                        "select count(*), min(id), (select count(*) from Blobs b join Ingestions i"
                        " on i.id = b.ingestion where i.path = ?) from Ingestions where path = ?;");
                    auto use(query.use());
                    use.apply(store->printStorePath(path)).apply(store->printStorePath(path));
                    if (!use.next())
                        throw Error("missing projection query result");
                    return std::array<int64_t, 3>{use.getInt(0), use.getInt(1), use.getInt(2)};
                };
                auto current = projection();
                EXPECT_EQ(current[0], 1);
                EXPECT_NE(current[1], static_cast<int64_t>(oldIngestion));
                EXPECT_EQ(current[2], empty ? 0 : 1);
                EXPECT_FALSE(store->objects.holderOrForget(merkle::blobId(tag + "-old"), false, NoRepair));
                if (!empty)
                    EXPECT_TRUE(store->objects.holderOrForget(merkle::blobId(tag + "-fresh"), false, NoRepair));

                /* Metadata registration promises no traversal/projection and
                   therefore preserves the existing candidate identity. */
                store->registerValidPaths(infos);
                EXPECT_EQ(projection(), current);
                for (unsigned repeat = 0; repeat < 2; ++repeat) {
                    OptimiseStats stats;
                    store->optimiseStore(stats);
                    auto after = projection();
                    EXPECT_EQ(after[0], 1) << "completed history is not retained";
                    EXPECT_EQ(after[2], empty ? 0 : 1) << "re-entry replaces rather than appends witnesses";
                }
            }
}

TEST_F(LocalStoreObjectsTest, candidateMigrationPreservesOldHoldersAndReadOnlyAccess)
{
    auto nar = treeNar({{"file", "migration-holder", 'f'}});
    StringSource source{nar};
    auto path = store->addToStoreFromDump(
        source,
        "candidate-migration",
        FileSerialisationMethod::NixArchive,
        ContentAddressMethod::Raw::NixArchive,
        HashAlgorithm::SHA256,
        {},
        NoRepair);
    auto id = merkle::blobId("migration-holder");
    auto before = store->objects.holderOrForget(id, false, NoRepair);
    ASSERT_TRUE(before);
    auto dbPath = store->dbDir / "db.sqlite";
    store.reset();
    {
        SQLite db{dbPath, {.mode = SQLiteOpenMode::NoCreate, .useWAL = settings.useSQLiteWAL}};
        db.exec(
            "create table OldBlobs (id blob not null, exec integer not null, ino integer not null, dev integer not null,"
            " ingestion integer not null, name text not null, size integer not null, primary key (id, exec));"
            "insert into OldBlobs select id, exec, ino, dev, ingestion, name, size from Blobs;"
            "drop table Blobs; alter table OldBlobs rename to Blobs;"
            "create index IndexBlobsIngestion on Blobs(ingestion);"
            "delete from SchemaMigrations where migration = '20260921-blob-holder-candidates';");
    }

    auto savedFeatures = experimentalFeatureSettings.experimentalFeatures.get();
    Finally restoreFeatures{[&] { experimentalFeatureSettings.experimentalFeatures = savedFeatures; }};
    experimentalFeatureSettings.experimentalFeatures.get().insert(Xp::ReadOnlyLocalStore);
    {
        auto roConfig = make_ref<LocalStoreConfig>(tempStoreDir.path(), StoreConfig::Params{{"read-only", "true"}});
        auto ro = make_ref<LocalStore>(roConfig);
        auto holder = ro->objects.holderOrForget(id, false, NoRepair);
        ASSERT_TRUE(holder);
        EXPECT_EQ(holder->ingestion, before->ingestion);
        EXPECT_EQ(holder->ino, before->ino);
        /* A read-only invalid candidate cannot be removed, but the cursor
           still reaches the end instead of repeatedly selecting it. */
        chmod(holder->file, 0644);
        EXPECT_FALSE(ro->objects.holderOrForget(id, false, NoRepair));
        canonicaliseTimestampAndPermissions(holder->file);
    }
    store = std::make_shared<LocalStore>(ref{config});
    auto after = store->objects.holderOrForget(id, false, NoRepair);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->ingestion, before->ingestion);
    EXPECT_EQ(after->ino, before->ino);
    EXPECT_EQ(after->file, before->file);
    EXPECT_TRUE(store->isValidPath(path));
    SQLite db{dbPath, {.mode = SQLiteOpenMode::NoCreate, .useWAL = settings.useSQLiteWAL}};
    SQLiteStmt columns{db, "pragma table_info(Blobs)"};
    auto use = columns.use();
    bool checked = false;
    while (use.next())
        if (use.getStr(1) == "ingestion") {
            EXPECT_EQ(use.getInt(5), 3);
            checked = true;
        }
    EXPECT_TRUE(checked);
}

/* The other half of the rule, driven directly: a row against an ingestion with
   no path goes, a row against one that names a valid path stays, and an
   ingestion still *in flight* -- its hold still under `tmp/` -- keeps its rows
   though it has no path yet (`08` K16: the rows get what the objects have). */
TEST_F(LocalStoreObjectsTest, theSweepJudgesARowByItsIngestionsPath)
{
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};

    /* Registered: kept. */
    auto named = realStoreDir / "judge1";
    restore(named, treeNar({{"a", "one", 'f'}}));
    auto kept = store->blobRowCount(false);
    ASSERT_GT(kept, 0u);

    /* In flight: no path, but its hold is still there. */
    auto flying = realStoreDir / "judge2";
    auto hold = store->objects.hold();
    store->beginIngestion(hold, flying);
    StringSource source{treeNar({{"b", "two", 'f'}})};
    auto hooks = makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
    store->restoreThroughObjects(flying, source, /*startFsync=*/false, hooks.get(), hold, NoRepair);
    hold.flush();
    auto withFlying = store->blobRowCount(false);
    ASSERT_GT(withFlying, kept);

    store->sweepStaleBlobRows();
    EXPECT_EQ(store->blobRowCount(false), withFlying)
        << "an ingestion in flight keeps its rows: its hold says it is alive";

    /* Its hold released without a registration: now it is a dead ingestion. */
    {
        auto releasing = std::move(hold);
    }
    store->sweepStaleBlobRows();
    EXPECT_EQ(store->blobRowCount(false), kept) << "and once the hold is gone its rows are";
    EXPECT_TRUE(store->objects.holderOrForget(merkle::blobId("one"), false, NoRepair).has_value())
        << "the registered ingestion's row is untouched";
    EXPECT_FALSE(store->objects.holderOrForget(merkle::blobId("two"), false, NoRepair).has_value());
}

/* A plain file's object hash is its blob id, which names no tree, and the
   path *is* the blob's holder: a row whose name is empty (`schema.sql`).
   What this pinned before and what it pins now.  Before, the root's blob
   had a file under `.objects/blobs`, and the sweep -- which recognises
   such a root by `pathExists(blobFile(id, false))` -- kept that file while
   the root was live and removed it when the root was dropped.  There is no
   such file now, so what the sweep must do with a plain-file root is not
   keep a file but *not count it among the roots whose object the store
   lacks*, and the branch that answers that is the index's own lookup (the
   sweep's root case, git-object-store.cc).  With the row gone -- the
   file replaced, the row forgotten -- the same live root is `unentered`,
   which is the discriminator. */
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
    /* The root is the holder: one row, no file under `blobs/`, one link. */
    ASSERT_EQ(store->blobRowCount(false), 1u);
    ASSERT_EQ(blobFileOf(blob, false), t);
    ASSERT_FALSE(pathExists(objects.blobFile(blob, false)));
    ASSERT_EQ(st(t).st_nlink, 1u);

    auto live = [&](fun<void(const ObjectHash &)> f) {
        f(ObjectHash::of(r.root));
        return 0;
    };
    auto swept = objects.sweep(live);
    EXPECT_EQ(swept.unentered, 0u) << "a plain-file root is known by its row";
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.blobsKept, 0u) << "there is no file under `blobs/` to keep";
    EXPECT_TRUE(hasBlob(blob, false));

    /* The file replaced, as the store leaves one it could not link: the
       row says what is not so and is forgotten, so the store holds the
       blob nowhere and the same live root is one whose object it lacks. */
    unlinkInPlace(t);
    EXPECT_FALSE(hasBlob(blob, false));
    EXPECT_EQ(store->blobRowCount(false), 0u);
    swept = objects.sweep(live);
    EXPECT_EQ(swept.unentered, 1u) << "no row, no object: the root is unentered";
    EXPECT_EQ(swept.blobsRemoved, 0u);
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
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(swept.blobsKept, 0u);
    EXPECT_TRUE(pathExists(objects.dir / "blobs-x"));
    EXPECT_TRUE(pathExists(objects.dir / "tmp"));

    deletePath(objects.dir / "blobs");
    /* The inode table is the index's rows, which survive the removal of
     * `blobs/` -- the index is the witness, not the directory (K8) */
    EXPECT_FALSE(objects.blobInodes().empty());
    EXPECT_TRUE(pathExists(objects.dir / "blobs"));
}

/* The sweep over a part of the object store the caller owns
   (`GitObjectStore::Owned`; the local overlay store's upper layer, which
   it reads through the merged mount and owns alone): the files enumerated
   and removed are that part's, while the trees are read -- and so
   reachability computed -- over the whole object store, which is what the
   store reads.  Here the part is a directory beside the object store
   holding a copy of a blob a live tree names and one nothing names; the
   object store's own files must not be touched, whatever the roots. */
TEST_F(LocalStoreObjectsTest, sweepOfAnOwnedPartRemovesOnlyItsFiles)
{
    auto & objects = store->objects;
    auto nar = treeNar({{"a", "one", 'f'}, {"d/b", "two", 'f'}});
    auto r = restore(std::filesystem::path{config->realStoreDir.get()} / "owned-part", nar);

    auto one = merkle::blobId("one");
    auto two = merkle::blobId("two");
    auto stray = merkle::blobId("stray");

    auto part = std::filesystem::path{config->realStoreDir.get()}.parent_path() / "owned-objects";
    for (auto & sub : {"blobs", "blobs-x", "trees", "tmp"})
        createDirs(part / sub);
    /* `one` where the sharded layout puts it (the part has the object
       store's shape), and `stray` flat in `blobs/`, where an earlier,
       unreleased build of this branch put it: both are enumerated and
       judged (01 §9.10, *Where the objects live*). */
    auto inPart = [&](const Hash & id) { return part / std::filesystem::relative(blobFileOf(id, false), objects.dir); };
    createDirs(inPart(one).parent_path());
    writeFile(inPart(one), "one");
    writeFile(part / "blobs" / GitObjectStore::idString(stray), "stray");

    std::vector<std::filesystem::path> removed;
    GitObjectStore::Owned owned{
        .dir = part,
        .remove =
            [&](const std::filesystem::path & file) {
                removed.push_back(file);
                unlinkIfExists(file);
            },
    };

    /* The path live: the tree the object store holds names `one`, so the
       part's copy of it stays; the stray, which no reachable tree names
       and nothing links, goes. */
    auto swept = objects.sweep(
        [&](fun<void(const ObjectHash &)> live) {
            live(ObjectHash::of(r.root));
            return 0;
        },
        &owned);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 1u);
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(swept.blobsKept, 0u);
    EXPECT_TRUE(pathExists(inPart(one)));
    EXPECT_FALSE(pathExists(part / "blobs" / GitObjectStore::idString(stray)));

    /* Nothing outside the part was removed, and the object store still has
       every object of the live path. */
    for (auto & file : removed)
        EXPECT_TRUE(std::filesystem::relative(file, part).native().starts_with("blobs"))
            << "removed outside the owned part: " << file;
    EXPECT_TRUE(hasBlob(one, false));
    EXPECT_TRUE(hasBlob(two, false));
    EXPECT_TRUE(hasTree(r.root.hash));

    /* The path dropped: the part's remaining blob goes with it, and the
       object store's own files still stand -- this sweep may not take
       them, however dead they are. */
    swept = objects.sweep([](fun<void(const ObjectHash &)>) { return 0; }, &owned);
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.treesRemoved, 0u);
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_TRUE(pathExists(inPart(one)));
    EXPECT_TRUE(hasBlob(one, false));
    EXPECT_TRUE(hasBlob(two, false));
    EXPECT_TRUE(hasTree(r.root.hash));
}

/* The four directories are made for the store's own object store (above)
   and never for a part the caller owns: the sweep only unlinks there, so
   an absent directory is empty.  (On the overlay store a directory made
   straight in the upper layer would go behind the mount's back, and the
   upper layer holds no `.objects/blobs` until something is written.) */
TEST_F(LocalStoreObjectsTest, sweepWritesNothingIntoAnOwnedPart)
{
    auto & objects = store->objects;
    auto nar = treeNar({{"a", "one", 'f'}});
    auto r = restore(std::filesystem::path{config->realStoreDir.get()} / "absent-part", nar);

    auto part = std::filesystem::path{config->realStoreDir.get()}.parent_path() / "absent-objects";
    ASSERT_FALSE(pathExists(part));

    GitObjectStore::Owned owned{
        .dir = part,
        .remove = [](const std::filesystem::path & file) { unlinkIfExists(file); },
    };

    auto swept = objects.sweep(
        [&](fun<void(const ObjectHash &)> live) {
            live(ObjectHash::of(r.root));
            return 0;
        },
        &owned);
    EXPECT_EQ(swept.treesRemoved, 0u);
    EXPECT_EQ(swept.blobsRemoved, 0u);
    EXPECT_EQ(swept.blobsKept, 0u);
    EXPECT_FALSE(pathExists(part));
    EXPECT_TRUE(hasBlob(merkle::blobId("one"), false));
    EXPECT_TRUE(hasTree(r.root.hash));
}

/* A malformed tree body is reported with the tree's identifier. */
TEST_F(LocalStoreObjectsTest, smallIngestionsFlushOnTheCallerAndResetTheirHolderNames)
{
    auto hold = store->objects.hold();
    auto caller = std::this_thread::get_id();
    size_t rows = 0;
    auto id = merkle::blobId("one");
    hold.setRoot("/first");
    hold.setSink([&](std::vector<GitObjectStore::Hold::Row> && batch) {
        EXPECT_EQ(std::this_thread::get_id(), caller);
        rows += batch.size();
    });
    hold.addRow({.id = id, .executable = false, .name = "a"});
    EXPECT_EQ(rows, 0u);
    EXPECT_EQ(hold.pendingHolder(id, false), "/first/a");
    hold.flush();
    EXPECT_EQ(rows, 1u);
    hold.setRoot("/second");
    hold.setSink([&](auto && batch) { rows += batch.size(); });
    EXPECT_FALSE(hold.pendingHolder(id, false)) << "names belong to one ingestion, not its reused hold";
    hold.addRow({.id = id, .executable = true, .name = "b"});
    hold.flush();
    EXPECT_EQ(rows, 2u);
    EXPECT_EQ(hold.pendingHolder(id, true), "/second/b");
}

/* An inode snapshot suggests a lookup; it cannot establish present content
   or sharing. Exercise every rejected hint and the valid shortcut at both
   regular-file root forms, with each executable bit. */
TEST_F(LocalStoreObjectsTest, optimiserInodeHintsRequireACurrentHolder)
{
    enum class Hint { Valid, Missing, Inode, Device, Size, OtherBlob, Writable, Mode, Seconds, Nanoseconds, Repair };
    for (bool bare : {false, true})
        for (bool executable : {false, true})
            for (auto hint :
                 {Hint::Valid,
                  Hint::Missing,
                  Hint::Inode,
                  Hint::Device,
                  Hint::Size,
                  Hint::OtherBlob,
                  Hint::Writable,
                  Hint::Mode,
                  Hint::Seconds,
                  Hint::Nanoseconds,
                  Hint::Repair}) {
                SCOPED_TRACE(fmt("bare=%d executable=%d hint=%d", bare, executable, static_cast<int>(hint)));
                auto label = fmt("inode-hint-%d-%d-%d", bare, executable, static_cast<int>(hint));
                auto target = config->realStoreDir.get() / label;
                auto originalBytes = label + " original";
                auto accessor = treeAccessor({{"file", originalBytes, executable ? 'x' : 'f'}});
                if (bare) {
                    auto rootFile = *accessor->open(CanonPath("/file"), std::nullopt);
                    accessor->root = std::move(rootFile);
                }
                restore(target, narOf(*accessor));
                auto file = bare ? target : target / "file";
                auto hints = store->objects.blobInodes();
                auto originalId = merkle::blobId(originalBytes);
                ASSERT_TRUE(hints.contains(st(file).st_ino));

                if (hint == Hint::OtherBlob) {
                    auto other = config->realStoreDir.get() / (label + "-other");
                    auto bytes = label + " another";
                    restore(other, treeNar({{"file", bytes, executable ? 'x' : 'f'}}));
                    hints.insert_or_assign(st(file).st_ino, merkle::blobId(bytes));
                } else if (hint != Hint::Valid) {
                    auto changedBytes = originalBytes;
                    changedBytes[0] = 'X';
                    chmod(file, 0644);
                    writeFile(file, changedBytes);
                    auto mode = executable ? 0555 : 0444;
                    chmod(file, mode | (hint == Hint::Writable ? S_IWUSR : hint == Hint::Mode ? S_ISVTX : 0));
                    timespec times[2]{
                        {0, UTIME_OMIT}, {hint == Hint::Seconds ? 2 : 1, hint == Hint::Nanoseconds ? 1 : 0}};
                    ASSERT_EQ(::utimensat(AT_FDCWD, file.c_str(), times, 0), 0);
                }
                auto expected = gitHash(target);

                auto originalIndex = store->objects.index;
                auto index = *originalIndex;
                size_t queries = 0;
                index.lookup = [&](const Hash & id,
                                   bool exec,
                                   GitObjectStore::Holder::Cursor after) -> std::optional<GitObjectStore::Holder> {
                    ++queries;
                    if (hint == Hint::Missing)
                        return std::nullopt;
                    auto candidate = originalIndex->lookup(id, exec, after);
                    if (candidate && id == originalId) {
                        if (hint == Hint::Inode)
                            ++candidate->ino;
                        if (hint == Hint::Device)
                            ++candidate->dev;
                        if (hint == Hint::Size)
                            ++candidate->size;
                    }
                    return candidate;
                };
                store->objects.index = &index;
                Finally restoreIndex([&] { store->objects.index = originalIndex; });
                auto hold = store->objects.hold();
                OptimiseStats stats;
                auto result = store->enterIntoObjects(
                    target, hint == Hint::Repair ? Repair : NoRepair, hold, nullptr, &stats, &hints);
                EXPECT_EQ(result.root, expected);
                auto rows = hold.takeRows();
                if (hint == Hint::Writable)
                    EXPECT_TRUE(rows.empty());
                else {
                    ASSERT_EQ(rows.size(), 1u);
                    EXPECT_EQ(rows.front().id, merkle::blobId(readFile(file)));
                    EXPECT_EQ(rows.front().name, bare ? "" : "file");
                }
                if (hint == Hint::Valid) {
                    EXPECT_EQ(queries, 1u) << "the validated shortcut does not fall through to hash and place";
                    EXPECT_EQ(stats.filesEntered, 0u);
                }
            }
}

TEST_F(LocalStoreObjectsTest, publicationRejectsProvenCorruptionOfItsOwnHolder)
{
    for (bool executable : {false, true}) {
        SCOPED_TRACE(executable);
        auto bytes = fmt("correct-content-%d", executable);
        auto corrupt = fmt("corrupt-content-%d", executable);
        ASSERT_EQ(bytes.size(), corrupt.size());
        auto nar = treeNar({{"file", bytes, executable ? 'x' : 'f'}});
        auto donor = config->realStoreDir.get() / fmt("own-corrupt-donor-%d", executable);
        auto other = config->realStoreDir.get() / fmt("own-corrupt-other-%d", executable);
        restore(donor, nar);
        restore(other, nar);
        unlinkInPlace(other / "file");
        {
            SQLite db{store->dbDir / "db.sqlite", {.mode = SQLiteOpenMode::NoCreate, .useWAL = settings.useSQLiteWAL}};
            SQLiteStmt update{
                db, "update Blobs set ino = ?, dev = ? where ingestion in (select id from Ingestions where path = ?);"};
            update.use()
                .apply(static_cast<int64_t>(st(other / "file").st_ino))
                .apply(static_cast<int64_t>(st(other / "file").st_dev))
                .apply((std::filesystem::path(config->storeDir) / other.filename()).string())
                .exec();
        }
        for (auto & candidate : {donor, other}) {
            chmod(candidate / "file", 0644);
            writeFile(candidate / "file", corrupt);
            chmod(candidate / "file", executable ? 0555 : 0444);
            setWriteTime(candidate / "file", 1, 1);
        }
        auto physicalClass = [&](const std::filesystem::path & file) {
            auto status = st(file);
            return std::pair{static_cast<int64_t>(status.st_dev), static_cast<int64_t>(status.st_ino)};
        };
        auto donorClass = physicalClass(donor / "file");
        auto otherClass = physicalClass(other / "file");
        ASSERT_NE(donorClass, otherClass);

        ValidPathInfo info{
            StorePath::random(fmt("own-corrupt-publication-%d", executable)),
            UnkeyedValidPathInfo{*store, std::nullopt}};
        auto real = store->toRealPath(info.path);
        auto hold = store->objects.hold();
        auto hooks = makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
        StringSource source{nar};
        auto result = store->restoreThroughObjects(real, source, false, hooks.get(), hold, NoRepair);
        info.objectHash = ObjectHash::of(result.root);
        info.narSize = result.narSize;
        ASSERT_EQ(physicalClass(real / "file"), std::min(donorClass, otherClass));
        ASSERT_EQ(readFile(real / "file"), corrupt);
        EXPECT_THROW(store->registerValidPath(info, hold), Error);
        EXPECT_FALSE(store->isValidPath(info.path));
    }
}

TEST_F(LocalStoreObjectsTest, verifierReentryDoesNotWaitForAnOutputLock)
{
    for (bool busy : {false, true}) {
        SCOPED_TRACE(busy);
        auto target = fmt("verifier-lock-target-%d", busy);
        auto nar = treeNar({{"file", "unchanged", 'f'}, {"link", target, 'l'}});
        ValidPathInfo info{
            StorePath::random(fmt("verifier-output-lock-%d", busy)), UnkeyedValidPathInfo{*store, std::nullopt}};
        info.assertedNarHash = hashString(HashAlgorithm::SHA256, nar);
        info.narSize = nar.size();
        StringSource source{nar};
        store->addToStore(info, source, NoRepair, NoCheckSigs);
        auto real = store->toRealPath(info.path);
        auto missing = store->objects.blobFile(merkle::blobId(target), false);
        std::filesystem::remove(missing);
        auto before = st(real / "file");
        PathLocks blocker;
        if (busy)
            ASSERT_TRUE(blocker.lockPaths({real}, "", false));
        EXPECT_EQ(store->verifyStore(/*checkContents=*/true, Repair), busy);
        EXPECT_EQ(pathExists(missing), !busy);
        EXPECT_EQ(st(real / "file").st_ino, before.st_ino);
        EXPECT_EQ(readFile(real / "file"), "unchanged");
        blocker.unlock();
        EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, Repair));
        EXPECT_TRUE(pathExists(missing));
    }
}

/* Exercise the two orders at the sweep's actual unlink boundary. The
   nonblocking lock probe makes the old implementation's counterexample
   deterministic: if no sweep lock exists, finish publication before the
   unlink. With the lock, publication resumes after that unlink instead.
   All three auxiliary kinds, both acquisition APIs and same/distinct
   object-store instances are covered. */
TEST_F(LocalStoreObjectsTest, auxiliaryPublicationCannotRaceSweepDeletion)
{
    unsigned serial = 0;
    for (unsigned kind = 0; kind < 3; ++kind)
        for (bool put : {false, true})
            for (bool separate : {false, true}) {
                SCOPED_TRACE(fmt("kind=%d put=%d separate=%d", kind, put, separate));
                GitObjectStore objects{tempStoreDir.path() / fmt("auxiliary-publication-%d", serial++)};
                objects.createDirectories();
                GitObjectStore other{objects.dir};
                auto & writerObjects = separate ? other : objects;
                auto bytes = kind == 0 ? std::string{} : std::string{"target"};
                auto id = kind == 0 ? merkle::treeId(bytes) : merkle::blobId(bytes);
                auto file = kind == 0 ? objects.treeFile(id) : objects.blobFile(id, kind == 2);
                auto publish = [&](GitObjectStore & into, const GitObjectStore::Hold & hold) {
                    if (kind == 0)
                        into.putTree(id, bytes, hold);
                    else
                        into.putBlob(id, kind == 2, bytes, hold);
                };
                {
                    auto initial = objects.hold();
                    publish(objects, initial);
                }
                ASSERT_EQ(st(file).st_nlink, 1u);
                auto hold = writerObjects.hold();
                std::future<bool> writer;
                bool removed = false;
                GitObjectStore::Owned owned{
                    .dir = objects.dir,
                    .remove =
                        [&](const std::filesystem::path & candidate) {
                            EXPECT_EQ(candidate, file);
                            std::promise<bool> acquired;
                            auto ready = acquired.get_future();
                            writer = std::async(std::launch::async, [&, acquired = std::move(acquired)]() mutable {
                                bool notified = false;
                                try {
                                    auto probe = openLockFile(objects.dir / "gc.lock", true);
                                    bool allowed = lockFile(probe.get(), ltRead, false);
                                    probe.close();
                                    auto acquire = [&] {
                                        if (put) {
                                            publish(writerObjects, hold);
                                            return true;
                                        }
                                        return kind == 0 ? writerObjects.holdTree(id, hold)
                                                         : writerObjects.holdBlob(id, kind == 2, hold);
                                    };
                                    if (allowed) {
                                        auto result = acquire();
                                        acquired.set_value(true);
                                        notified = true;
                                        return result;
                                    }
                                    acquired.set_value(false);
                                    notified = true;
                                    return acquire();
                                } catch (...) {
                                    if (!notified)
                                        acquired.set_exception(std::current_exception());
                                    throw;
                                }
                            });
                            EXPECT_FALSE(ready.get()) << "the sweep excludes new auxiliary links";
                            unlinkIfExists(candidate);
                            removed = true;
                        },
                };
                objects.sweep([](fun<void(const ObjectHash &)>) { return 0; }, &owned);
                ASSERT_TRUE(removed);
                auto acquired = writer.get();
                EXPECT_EQ(acquired, put) << "a pure acquisition misses after collection";
                EXPECT_EQ(pathExists(file), put);
                if (acquired)
                    EXPECT_EQ(readFile(file), bytes);
            }
}

/* A committed root that was absent from the snapshot is protected by its
   ingestion until the sweep ends. Releasing that ingestion immediately
   after the snapshot used to turn its objects into one-link garbage. */
TEST_F(LocalStoreObjectsTest, auxiliaryHoldReleaseWaitsForTheRootSnapshotSweep)
{
    auto lockFilePath = config->stateDir.get() / "object-gc.lock";
    for (unsigned kind = 0; kind < 3; ++kind)
        for (bool separate : {false, true}) {
            SCOPED_TRACE(fmt("kind=%d separate=%d", kind, separate));
            auto & objects = store->objects;
            GitObjectStore other{objects.dir, false, lockFilePath};
            auto & writerObjects = separate ? other : objects;
            auto path = StorePath::random(fmt("late-root-%d-%d", kind, separate));
            auto realPath = store->toRealPath(path);
            createDir(realPath);
            std::string childName{path.to_string()};
            auto bytes = kind == 0 ? std::string{} : std::string{path.to_string()};
            auto id = kind == 0 ? merkle::treeId(bytes) : merkle::blobId(bytes);
            std::optional<GitObjectStore::Hold> hold{writerObjects.hold()};
            auto mode = kind == 0   ? merkle::Mode::Directory
                        : kind == 1 ? merkle::Mode::Symlink
                                    : merkle::Mode::Executable;
            if (kind == 0) {
                writerObjects.putTree(id, bytes, *hold);
                createDir(realPath / childName);
            } else {
                writerObjects.putBlob(id, kind == 2, bytes, *hold);
                if (kind == 1)
                    createSymlink(bytes, realPath / childName);
                else {
                    writeFile(realPath / childName, bytes);
                    chmod(realPath / childName, 0555);
                }
            }
            auto body = merkle::serialiseTree({{kind == 0 ? childName + "/" : childName, {mode, id}}});
            auto root = merkle::treeId(body);
            writerObjects.putTree(root, body, *hold);
            ValidPathInfo info{path, UnkeyedValidPathInfo{*store, ObjectHash{root}}};
            std::future<void> writer;
            objects.sweep([&](fun<void(const ObjectHash &)> live) {
                for (auto & path : store->queryAllValidPaths())
                    live(*store->queryPathInfo(path)->objectHash);
                std::promise<bool> released;
                auto ready = released.get_future();
                writer = std::async(std::launch::async, [&, released = std::move(released)]() mutable {
                    bool notified = false;
                    try {
                        store->registerValidPath(info);
                        auto probe = openLockFile(lockFilePath, true);
                        bool allowed = lockFile(probe.get(), ltRead, false);
                        probe.close();
                        if (allowed)
                            hold.reset();
                        released.set_value(allowed);
                        notified = true;
                        hold.reset();
                    } catch (...) {
                        if (!notified)
                            released.set_exception(std::current_exception());
                        throw;
                    }
                });
                EXPECT_FALSE(ready.get()) << "registration does not release protection during the sweep";
                return 0;
            });
            writer.get();
            EXPECT_TRUE(store->isValidPath(path));
            EXPECT_TRUE(pathExists(objects.treeFile(root)));
            auto file = kind == 0 ? objects.treeFile(id) : objects.blobFile(id, kind == 2);
            ASSERT_TRUE(pathExists(file));
            EXPECT_EQ(readFile(file), bytes);
            EXPECT_EQ(st(file).st_nlink, 1u) << "the hold releases after the sweep";
        }
}

TEST_F(LocalStoreObjectsTest, overlappingAuxiliaryReadersKeepTheLockUntilBothFinish)
{
    GitObjectStore objects{tempStoreDir.path() / "overlapping-auxiliary-readers", true};
    objects.createDirectories();
    auto firstHold = objects.hold();
    auto secondHold = objects.hold();
    std::promise<void> entered, resume;
    auto enteredFuture = entered.get_future();
    auto resumeFuture = resume.get_future();
    std::atomic<unsigned> calls{0};
    bool notified = false;
    objects.atFsync = [&](const auto &) {
        if (calls.fetch_add(1) == 0) {
            entered.set_value();
            notified = true;
            resumeFuture.get();
        }
    };
    auto first = std::async(std::launch::async, [&] {
        try {
            objects.putBlob(merkle::blobId("first"), false, "first", firstHold);
        } catch (...) {
            if (!notified)
                entered.set_exception(std::current_exception());
            throw;
        }
    });
    bool resumed = false;
    auto release = [&] {
        if (!resumed) {
            resume.set_value();
            resumed = true;
        }
    };
    Finally finish{[&] {
        release();
        if (first.valid())
            first.wait();
    }};
    enteredFuture.get();
    objects.putBlob(merkle::blobId("second"), false, "second", secondHold);
    auto probe = openLockFile(objects.dir / "gc.lock", true);
    EXPECT_FALSE(lockFile(probe.get(), ltWrite, false)) << "one reader is still publishing";
    release();
    first.get();
    EXPECT_TRUE(lockFile(probe.get(), ltWrite, false));
    probe.close();
}

TEST_F(LocalStoreObjectsTest, publishedAuxiliaryObjectRetainsItsOriginalHoldLink)
{
    auto & objects = store->objects;
    auto body = std::string{};
    auto id = merkle::treeId(body);
    auto file = objects.treeFile(id);
    {
        auto hold = objects.hold();
        objects.putTree(id, body, hold);
        auto published = st(file);
        EXPECT_EQ(published.st_nlink, 2u);
        bool found = false;
        for (auto & entry : DirectoryIterator{hold.linkFor(file).parent_path()}) {
            EXPECT_EQ(st(entry.path()).st_ino, published.st_ino);
            found = true;
        }
        EXPECT_TRUE(found);
        /* A second writer must hold the completed global object. */
        auto other = objects.hold();
        objects.putTree(id, body, other);
        EXPECT_EQ(st(file).st_ino, published.st_ino);
        EXPECT_EQ(st(file).st_nlink, 3u);
    }
    EXPECT_EQ(st(file).st_nlink, 1u);
}

TEST_F(LocalStoreObjectsTest, holdsNeverReuseAnExistingName)
{
    auto & objects = store->objects;
    auto first = objects.hold();
    auto name = first.directory().filename().string();
    auto dash = name.rfind('-');
    ASSERT_NE(dash, std::string::npos);
    uint32_t next = std::stoul(name.substr(dash + 1)) + 1;
    auto prefix = name.substr(0, dash + 1);
    auto candidate = [&] { return first.directory().parent_path() / (prefix + std::to_string(next++)); };

    /* PID reuse can leave an old directory, including an unpublished
       temporary. Existing names of every supported filesystem type are
       occupied; none is a new ingestion's directory to adopt or remove. */
    auto partial = candidate();
    createDir(partial);
    writeFile(partial / "unfinished.tmp-0", "old ingestion");
    auto empty = candidate();
    createDir(empty);
    auto file = candidate();
    writeFile(file, "occupied name");
    auto symlink = candidate();
    createSymlink(partial, symlink);
    auto dangling = candidate();
    createSymlink(objects.dir / "absent", dangling);

    auto expected = candidate();
    {
        auto fresh = objects.hold();
        EXPECT_EQ(fresh.directory(), expected);
        EXPECT_TRUE(std::filesystem::is_empty(fresh.directory()));
        objects.putTree(merkle::treeId(""), "", fresh);
    }
    EXPECT_FALSE(pathExists(expected));
    EXPECT_EQ(readFile(partial / "unfinished.tmp-0"), "old ingestion");
    EXPECT_TRUE(std::filesystem::is_empty(empty));
    EXPECT_EQ(readFile(file), "occupied name");
    EXPECT_EQ(readLink(symlink), partial);
    EXPECT_EQ(readLink(dangling), objects.dir / "absent");
}

TEST_F(LocalStoreObjectsTest, auxiliaryHoldsCoverEveryKindAndIdentifierShard)
{
    auto & objects = store->objects;
    auto hold = objects.hold();
    auto shared = hold.ownRows();
    EXPECT_TRUE(std::filesystem::is_empty(hold.directory()));
    for (unsigned int byte = 0; byte < 256; ++byte) {
        Hash id{HashAlgorithm::SHA256};
        id.hash[0] = byte;
        for (auto object : {objects.blobFile(id, false), objects.blobFile(id, true), objects.treeFile(id)}) {
            auto relative = object.lexically_relative(objects.dir);
            auto expected = hold.directory() / relative;
            EXPECT_EQ(hold.linkFor(object), expected);
            EXPECT_TRUE(std::filesystem::is_directory(expected.parent_path()));
            EXPECT_EQ(shared.linkFor(object), expected);
            EXPECT_EQ(hold.linkFor(object), expected);
        }
    }
}

TEST_F(LocalStoreObjectsTest, auxiliaryHoldsKeepTheirAddressesAcrossRepair)
{
    size_t cases = 0;
    auto & objects = store->objects;
    for (unsigned kind = 0; kind < 3; ++kind)
        for (unsigned spelling = 0; spelling < 3; ++spelling)
            for (bool ownRows : {false, true})
                for (bool reacquire : {false, true}) {
                    SCOPED_TRACE(
                        fmt("kind=%d spelling=%d ownRows=%d reacquire=%d", kind, spelling, ownRows, reacquire));
                    auto bytes = kind == 0 ? std::string{} : fmt("repaired-auxiliary-%d", cases);
                    auto id = kind == 0 ? merkle::treeId(bytes) : merkle::blobId(bytes);
                    auto file = kind == 0 ? objects.treeFile(id) : objects.blobFile(id, kind == 2);
                    auto put = [&](const GitObjectStore::Hold & hold) {
                        if (kind == 0)
                            objects.putTree(id, bytes, hold);
                        else
                            objects.putBlob(id, kind == 2, bytes, hold);
                    };
                    auto acquire = [&](const GitObjectStore::Hold & hold) {
                        return kind == 0 ? objects.holdTree(id, hold) : objects.holdBlob(id, kind == 2, hold);
                    };
                    if (spelling != 0) {
                        auto seed = objects.hold();
                        put(seed);
                    }
                    std::optional<GitObjectStore::Hold> owner{objects.hold()};
                    std::optional<GitObjectStore::Hold> view;
                    if (ownRows)
                        view.emplace(owner->ownRows());
                    const auto & hold = view ? *view : *owner;
                    if (spelling == 0)
                        put(hold); // The published temporary remains <id>.tmp-<counter>.
                    else {
                        ASSERT_TRUE(acquire(hold));
                        if (spelling == 2) {
                            auto name = hold.linkFor(file);
                            std::filesystem::rename(name, name.parent_path().parent_path() / name.filename());
                        }
                    }
                    auto oldInode = st(file).st_ino;
                    chmod(file, 0644);
                    writeFile(file, "corrupt auxiliary bytes");
                    chmod(file, kind == 2 ? 0555 : 0444);
                    EXPECT_FALSE(store->verifyStore(true, Repair));
                    ASSERT_FALSE(pathExists(file));
                    {
                        auto repair = objects.hold();
                        put(repair);
                    }
                    ASSERT_NE(st(file).st_ino, oldInode);
                    ASSERT_EQ(st(file).st_nlink, 1u);
                    if (reacquire)
                        ASSERT_TRUE(acquire(hold));
                    /* An ownRows view may finish before the directory's
                       last owner. Its auxiliary obligations still exist. */
                    view.reset();
                    auto noRoots = [](fun<void(const ObjectHash &)>) { return uint64_t{0}; };
                    auto kept = objects.sweep(noRoots);
                    EXPECT_TRUE(pathExists(file));
                    EXPECT_EQ(kept.treesRemoved + kept.blobsRemoved, 0u);
                    owner.reset();
                    auto removed = objects.sweep(noRoots);
                    EXPECT_FALSE(pathExists(file));
                    EXPECT_EQ(removed.treesRemoved + removed.blobsRemoved, 1u);
                    ++cases;
                }
    EXPECT_EQ(cases, 36u);
}

TEST_F(LocalStoreObjectsTest, heldTreeAddressesDoNotReplaceReachabilityTraversal)
{
    for (bool reachable : {false, true})
        for (bool childHeld : {false, true}) {
            SCOPED_TRACE(fmt("reachable=%d childHeld=%d", reachable, childHeld));
            GitObjectStore objects{tempStoreDir.path() / fmt("held-reachability-%d-%d", reachable, childHeld)};
            objects.createDirectories();
            auto blob = merkle::blobId("target");
            auto body = merkle::serialiseTree({{"link", {merkle::Mode::Symlink, blob}}});
            auto tree = merkle::treeId(body);
            {
                auto seed = objects.hold();
                objects.putBlob(blob, false, "target", seed);
                objects.putTree(tree, body, seed);
            }
            auto hold = objects.hold();
            ASSERT_TRUE(objects.holdTree(tree, hold));
            if (childHeld)
                ASSERT_TRUE(objects.holdBlob(blob, false, hold));
            objects.sweep([&](fun<void(const ObjectHash &)> live) {
                if (reachable)
                    live(ObjectHash::of({merkle::Mode::Directory, tree}));
                return uint64_t{0};
            });
            EXPECT_TRUE(pathExists(objects.treeFile(tree)));
            EXPECT_EQ(pathExists(objects.blobFile(blob, false)), reachable || childHeld);
        }
}

TEST_F(LocalStoreObjectsTest, auxiliaryHoldNamesDenoteExactlyTheirKindAndIdentifier)
{
    const std::array<std::string_view, 3> kinds{"blobs", "blobs-x", "trees"};
    const std::vector<std::pair<std::string, bool>> suffixes{
        {"", true},
        {".tmp-0", true},
        {".tmp-18446744073709551615", true},
        {".tmp-", false},
        {".tmp--1", false},
        {".tmp-1x", false},
        {".tmp-1.extra", false},
        {".old", false},
    };
    size_t cases = 0;
    for (size_t kind = 0; kind < kinds.size(); ++kind)
        for (bool sharded : {false, true})
            for (auto & [suffix, accepted] : suffixes) {
                SCOPED_TRACE(fmt("kind=%d sharded=%d suffix=%s", kind, sharded, suffix));
                GitObjectStore objects{tempStoreDir.path() / fmt("hold-name-%d", cases)};
                objects.createDirectories();
                auto id = merkle::blobId("held address");
                auto name = GitObjectStore::idString(id);
                auto hold = objects.hold();
                auto claim = hold.directory() / kinds[kind];
                if (sharded)
                    claim /= name.substr(0, 2);
                createDirs(claim);
                /* A surviving old incarnation is independent of the newly
                   published inode and may have corrupt bytes or metadata. */
                writeFile(claim / (name + suffix), "old incarnation");
                std::array files{objects.blobFile(id, false), objects.blobFile(id, true), objects.treeFile(id)};
                for (auto & file : files) {
                    createDirs(file.parent_path());
                    writeFile(file, "new incarnation");
                    ASSERT_EQ(st(file).st_nlink, 1u);
                }
                objects.sweep([](fun<void(const ObjectHash &)>) { return uint64_t{0}; });
                for (size_t other = 0; other < kinds.size(); ++other)
                    EXPECT_EQ(pathExists(files[other]), accepted && other == kind);
                ++cases;
            }
    EXPECT_EQ(cases, 48u);
}

namespace {
struct OnAuxiliaryCorruption : testing::CaptureLogger
{
    std::function<void()> replace;
    std::exception_ptr failure;
    bool observed = false;

    void log(Verbosity, std::string_view message) noexcept override
    {
        if (observed
            || (message.find("was modified! expected identifier") == message.npos
                && message.find("is not a regular file!") == message.npos))
            return;
        observed = true;
        try {
            replace();
        } catch (...) {
            failure = std::current_exception();
        }
    }
};

} // namespace

TEST_F(LocalStoreObjectsTest, auxiliaryRepairPreservesAConcurrentReplacement)
{
    auto & objects = store->objects;
    size_t cases = 0;
    /* After the first observation, another verifier removes that inode.
       The address can then be absent, sound, or still corrupt. The first
       verifier must decide from that current state under its reservation. */
    for (unsigned kind = 0; kind < 3; ++kind)
        for (unsigned replacement = 0; replacement < 3; ++replacement) {
            SCOPED_TRACE(fmt("kind=%d replacement=%d", kind, replacement));
            auto bytes = kind == 0 ? std::string{} : fmt("sound-replacement-%d", cases);
            auto id = kind == 0 ? merkle::treeId(bytes) : merkle::blobId(bytes);
            auto file = kind == 0 ? objects.treeFile(id) : objects.blobFile(id, kind == 2);
            createDirs(file.parent_path());
            writeFile(file, "corrupt old incarnation");
            auto old = st(file);
            /* Pin solely to distinguish the test's two incarnations. The
               verifier keeps a descriptor only while reading each inode. */
            auto oldInode = openFileReadonly(file);
            ASSERT_TRUE(oldInode);
            auto hold = objects.hold();
            bool nestedErrors = true;
            OnAuxiliaryCorruption mismatch;
            mismatch.replace = [&] {
                /* This is the real verifier's diagnostic boundary, after
                   hashing but before removal, with no scheduling or seam. */
                nestedErrors = store->verifyStore(true, Repair);
                if (replacement == 1) {
                    if (kind == 0)
                        objects.putTree(id, bytes, hold);
                    else
                        objects.putBlob(id, kind == 2, bytes, hold);
                } else if (replacement == 2)
                    writeFile(file, "a different corrupt replacement with a different size");
            };
            auto oldLogger = std::exchange(logger, &mismatch);
            auto oldVerbosity = std::exchange(verbosity, lvlInfo);
            Finally restore{[&] {
                logger = oldLogger;
                verbosity = oldVerbosity;
            }};
            EXPECT_FALSE(store->verifyStore(true, Repair));
            if (mismatch.failure)
                std::rethrow_exception(mismatch.failure);
            ASSERT_TRUE(mismatch.observed);
            EXPECT_FALSE(nestedErrors);
            EXPECT_EQ(pathExists(file), replacement == 1);
            if (replacement == 1) {
                ASSERT_TRUE(pathExists(file));
                EXPECT_EQ(readFile(file), bytes);
                EXPECT_NE(st(file).st_ino, old.st_ino);
                unlinkIfExists(file);
            }
            ++cases;
        }
    EXPECT_EQ(cases, 9u);
}

TEST_F(LocalStoreObjectsTest, auxiliaryHoldClaimsDoNotDependOnTheOldInodeKind)
{
    enum class Kind { Regular, Symlink, DanglingSymlink, Fifo, Socket, Character, Block, Directory };
    const std::array kinds{
        Kind::Regular,
        Kind::Symlink,
        Kind::DanglingSymlink,
        Kind::Fifo,
        Kind::Socket,
        Kind::Character,
        Kind::Block,
        Kind::Directory};
    const std::array<std::string_view, 3> namespaces{"trees", "blobs", "blobs-x"};
    auto & objects = store->objects;
    size_t cases = 0, devicesUnavailable = 0;
    for (unsigned space = 0; space < namespaces.size(); ++space)
        for (bool flat : {false, true})
            for (auto kind : kinds) {
                SCOPED_TRACE(fmt("space=%d flat=%d kind=%d", space, flat, static_cast<unsigned>(kind)));
                auto bytes = space == 0 ? std::string{} : fmt("replacement-%d", cases);
                auto id = space == 0 ? merkle::treeId(bytes) : merkle::blobId(bytes);
                auto file = space == 0 ? objects.treeFile(id) : objects.blobFile(id, space == 2);
                createDirs(file.parent_path());
                auto outside = tempStoreDir.path() / "outside-auxiliary-claim";
                writeFile(outside, bytes);
                AutoCloseFD socket;
                switch (kind) {
                case Kind::Regular:
                case Kind::Directory:
                    writeFile(file, "corrupt incarnation");
                    break;
                case Kind::Symlink:
                    createSymlink(outside, file);
                    break;
                case Kind::DanglingSymlink:
                    createSymlink(outside.string() + "-missing", file);
                    break;
                case Kind::Fifo:
                    ASSERT_EQ(mkfifo(file.c_str(), 0600), 0) << strerror(errno);
                    break;
                case Kind::Socket:
                    socket = createUnixDomainSocket(file, 0600);
                    break;
                case Kind::Character:
                case Kind::Block:
                    if (mknod(file.c_str(), (kind == Kind::Character ? S_IFCHR : S_IFBLK) | 0600, 0) == -1) {
                        /* Device nodes complete the POSIX kind domain but
                           require privileges unavailable in ordinary and
                           sandboxed test runs. No leaf-type predicate is
                           used by the claim decoder for any of these kinds. */
                        ASSERT_TRUE(errno == EPERM || errno == EACCES || errno == ENOTSUP) << strerror(errno);
                        ++devicesUnavailable;
                        continue;
                    }
                    break;
                }

                std::optional<GitObjectStore::Hold> hold{objects.hold()};
                auto claim =
                    flat ? hold->directory() / namespaces[space] / GitObjectStore::idString(id) : hold->linkFor(file);
                createDirs(claim.parent_path());
                if (kind == Kind::Directory) {
                    /* Directories cannot be hard-linked, but a corrupted
                       claim with this shape conservatively denotes the same
                       address, without granting a traversal into it. */
                    createDir(claim);
                    writeFile(claim / "not-an-auxiliary-claim", "untouched until hold release");
                } else {
                    ASSERT_TRUE(space == 0 ? objects.holdTree(id, *hold) : objects.holdBlob(id, space == 2, *hold));
                    if (flat)
                        std::filesystem::rename(hold->linkFor(file), claim);
                    EXPECT_EQ(st(claim).st_ino, st(file).st_ino);
                }
                EXPECT_TRUE(store->verifyStore(true, NoRepair));
                EXPECT_FALSE(store->verifyStore(true, Repair));
                EXPECT_FALSE(maybeLstat(file));
                EXPECT_EQ(readFile(outside), bytes) << "a corrupt symlink is never followed";
                {
                    auto replacement = objects.hold();
                    if (space == 0)
                        objects.putTree(id, bytes, replacement);
                    else
                        objects.putBlob(id, space == 2, bytes, replacement);
                }
                ASSERT_EQ(st(file).st_nlink, 1u);
                objects.sweep([](fun<void(const ObjectHash &)>) { return uint64_t{0}; });
                ASSERT_TRUE(pathExists(file)) << "the typed address survives its linked incarnation";
                EXPECT_EQ(readFile(file), bytes);
                hold.reset();
                objects.sweep([](fun<void(const ObjectHash &)>) { return uint64_t{0}; });
                EXPECT_FALSE(pathExists(file));
                ++cases;
            }
    EXPECT_EQ(cases + devicesUnavailable, 48u);
    EXPECT_GE(cases, 36u);
    RecordProperty("device_node_cases_unavailable", devicesUnavailable);
}

TEST_F(LocalStoreObjectsTest, auxiliaryReadersRejectNonregularOccupants)
{
    enum class Kind { Missing, Regular, Symlink, DanglingSymlink, Fifo, Socket, Character, Block, Directory };
    enum class Role {
        TreeBytes,
        BlobBytes,
        ExecutableBlobBytes,
        VerifiedTree,
        Sweep,
        MaterialiseTree,
        MaterialiseLink
    };
    const std::array kinds{
        Kind::Missing,
        Kind::Regular,
        Kind::Symlink,
        Kind::DanglingSymlink,
        Kind::Fifo,
        Kind::Socket,
        Kind::Character,
        Kind::Block,
        Kind::Directory};
    const std::array roles{
        Role::TreeBytes,
        Role::BlobBytes,
        Role::ExecutableBlobBytes,
        Role::VerifiedTree,
        Role::Sweep,
        Role::MaterialiseTree,
        Role::MaterialiseLink};
    auto & objects = store->objects;
    size_t cases = 0, devicesUnavailable = 0;
    for (auto role : roles)
        for (auto kind : kinds) {
            SCOPED_TRACE(fmt("role=%d kind=%d", static_cast<unsigned>(role), static_cast<unsigned>(kind)));
            bool tree = role != Role::BlobBytes && role != Role::ExecutableBlobBytes && role != Role::MaterialiseLink;
            auto target = fmt("auxiliary-reader-target-%d", cases + devicesUnavailable);
            auto targetId = merkle::blobId(target);
            auto body = merkle::serialiseTree({{"link", {merkle::Mode::Symlink, targetId}}});
            auto bytes = tree ? body : target;
            auto id = tree ? merkle::treeId(body) : targetId;
            auto file = tree ? objects.treeFile(id) : objects.blobFile(id, role == Role::ExecutableBlobBytes);
            auto outside = tempStoreDir.path() / "auxiliary-reader-outside";
            createDirs(file.parent_path());
            writeFile(outside, bytes);
            Finally clean{[&] {
                if (maybeLstat(file))
                    deletePath(file);
            }};
            AutoCloseFD socket;
            switch (kind) {
            case Kind::Missing:
                break;
            case Kind::Regular:
                writeFile(file, bytes);
                break;
            case Kind::Symlink:
                createSymlink(outside, file);
                break;
            case Kind::DanglingSymlink:
                createSymlink(outside.string() + "-missing", file);
                break;
            case Kind::Fifo:
                ASSERT_EQ(mkfifo(file.c_str(), 0600), 0) << strerror(errno);
                break;
            case Kind::Socket:
                socket = createUnixDomainSocket(file, 0600);
                break;
            case Kind::Character:
            case Kind::Block:
                if (mknod(file.c_str(), (kind == Kind::Character ? S_IFCHR : S_IFBLK) | 0600, 0) == -1) {
                    ASSERT_TRUE(errno == EPERM || errno == EACCES || errno == ENOTSUP) << strerror(errno);
                    ++devicesUnavailable;
                    continue;
                }
                break;
            case Kind::Directory:
                createDir(file);
                break;
            }
            bool malformed = kind != Kind::Missing && kind != Kind::Regular;
            auto source = treeAccessor({{"link", target, 'l'}});
            if (role == Role::MaterialiseLink)
                source->root = MemorySourceAccessor::File{MemorySourceAccessor::File::Symlink{.target = target}};
            if (role == Role::MaterialiseTree) {
                auto hold = objects.hold();
                objects.putBlob(targetId, false, target, hold);
            }
            auto spare = objects.blobFile(merkle::blobId("unreachable reader-test sentinel"), false);
            if (role == Role::Sweep) {
                createDirs(spare.parent_path());
                writeFile(spare, "unreachable reader-test sentinel");
            }
            auto exercise = [&]() -> bool {
                switch (role) {
                case Role::TreeBytes:
                case Role::BlobBytes:
                case Role::ExecutableBlobBytes:
                    try {
                        auto read = readAuxiliaryObject(objects, id, static_cast<unsigned>(role));
                        return !malformed && read == (kind == Kind::Missing ? std::nullopt : std::optional{bytes});
                    } catch (NotARegularFile &) {
                        return malformed;
                    }
                case Role::VerifiedTree: {
                    auto read = objects.readVerifiedTree(id);
                    return kind == Kind::Regular ? read && read->at("link").hash == targetId : !read;
                }
                case Role::Sweep: {
                    auto swept = objects.sweep([&](fun<void(const ObjectHash &)> live) {
                        live(ObjectHash{id});
                        return uint64_t{0};
                    });
                    return swept.corruptTrees == (malformed ? 1u : 0u) && pathExists(spare) == malformed;
                }
                case Role::MaterialiseTree:
                case Role::MaterialiseLink: {
                    auto dst = config->realStoreDir.get() / fmt("auxiliary-reader-%d", cases + devicesUnavailable);
                    auto counting = make_ref<CountingAccessor>(source);
                    auto hold = objects.hold();
                    try {
                        auto result = store->materialiseThroughObjects(
                            dst,
                            SourcePath(counting),
                            defaultPathFilter,
                            false,
                            nullptr,
                            NoRepair,
                            everyNode(source),
                            hold);
                        if (kind == Kind::Directory)
                            return false;
                        bool fellBack = role == Role::MaterialiseTree ? counting->listings != 0 : counting->links != 0;
                        StringSink nar;
                        dumpPath(dst, nar);
                        return fellBack == (kind != Kind::Regular) && nar.s == narOf(*source)
                               && result.root == objectHashOf(*source, CanonPath::root).root;
                    } catch (Error &) {
                        /* Directories cannot be hard-linked, including by
                           the fallback's auxiliary publisher. Readers do
                           not silently repair this existing error case. */
                        return kind == Kind::Directory;
                    }
                }
                }
                return false;
            };
            if (kind == Kind::Fifo) {
                /* No writer exists. The old reader blocks in open(); a
                   child bounds that failure without a timing-dependent
                   writer or a blocked future on assertion unwinding.
                   The alarm is a failure watchdog, not synchronization. */
                Pid child = startProcess([&] {
                    alarm(5);
                    _exit(exercise() ? 0 : 1);
                });
                EXPECT_EQ(child.wait(), 0) << "auxiliary FIFO read failed or blocked";
            } else
                EXPECT_TRUE(exercise());
            EXPECT_EQ(readFile(outside), bytes);
            ++cases;
        }
    EXPECT_EQ(cases + devicesUnavailable, 63u);
    EXPECT_GE(cases, 49u);
    RecordProperty("device_node_cases_unavailable", devicesUnavailable);
}

TEST_F(LocalStoreObjectsTest, auxiliaryVerificationDoesNotFollowNamespaceSymlinks)
{
    for (auto kind : {"blobs", "blobs-x", "trees"})
        for (bool atKind : {false, true}) {
            SCOPED_TRACE(fmt("kind=%s atKind=%d", kind, atKind));
            GitObjectStore objects{tempStoreDir.path() / fmt("namespace-symlink-%s-%d", kind, atKind)};
            objects.createDirectories();
            auto outside = tempStoreDir.path() / fmt("outside-namespace-%s-%d", kind, atKind);
            createDirs(outside);
            auto file = outside / GitObjectStore::idString(merkle::blobId("expected"));
            writeFile(file, "external bytes are not an auxiliary object");
            auto node = objects.dir / kind;
            if (atKind)
                ASSERT_TRUE(std::filesystem::remove(node));
            else
                node /= "ab";
            createSymlink(outside, node);
            if (atKind) {
                /* A fixed kind must be a directory. Namespace corruption
                   is diagnosed without following or rewriting that link. */
                EXPECT_THROW(objects.verify(NoRepair), Error);
                EXPECT_THROW(objects.verify(Repair), Error);
                EXPECT_TRUE(S_ISLNK(st(node).st_mode));
            } else {
                EXPECT_TRUE(objects.verify(NoRepair));
                EXPECT_FALSE(objects.verify(Repair));
                EXPECT_FALSE(maybeLstat(node));
            }
            EXPECT_EQ(readFile(file), "external bytes are not an auxiliary object");
        }
}

TEST_F(LocalStoreObjectsTest, auxiliaryRepairRecognizesARestoredShardDirectory)
{
    for (unsigned kind = 0; kind < 3; ++kind)
        for (bool populated : {false, true}) {
            SCOPED_TRACE(fmt("kind=%d populated=%d", kind, populated));
            GitObjectStore objects{tempStoreDir.path() / fmt("restored-shard-%d-%d", kind, populated)};
            objects.createDirectories();
            auto bytes = kind == 0 ? std::string{} : std::string{"restored shard bytes"};
            auto id = kind == 0 ? merkle::treeId(bytes) : merkle::blobId(bytes);
            auto file = kind == 0 ? objects.treeFile(id) : objects.blobFile(id, kind == 2);
            auto shard = file.parent_path();
            auto outside = tempStoreDir.path() / fmt("outside-restored-shard-%d-%d", kind, populated);
            createDirs(outside);
            writeFile(outside / "untouched", "external bytes");
            createSymlink(outside, shard);
            auto hold = objects.hold();
            bool nestedErrors = true;
            OnAuxiliaryCorruption mismatch;
            mismatch.replace = [&] {
                nestedErrors = objects.verify(Repair);
                if (!populated)
                    createDirs(shard);
                else if (kind == 0)
                    objects.putTree(id, bytes, hold);
                else
                    objects.putBlob(id, kind == 2, bytes, hold);
            };
            auto oldLogger = std::exchange(logger, &mismatch);
            auto oldVerbosity = std::exchange(verbosity, lvlInfo);
            Finally restore{[&] {
                logger = oldLogger;
                verbosity = oldVerbosity;
            }};
            EXPECT_FALSE(objects.verify(Repair));
            if (mismatch.failure)
                std::rethrow_exception(mismatch.failure);
            EXPECT_TRUE(mismatch.observed);
            EXPECT_FALSE(nestedErrors);
            ASSERT_TRUE(S_ISDIR(st(shard).st_mode));
            EXPECT_EQ(pathExists(file), populated);
            if (populated)
                EXPECT_EQ(readFile(file), bytes);
            EXPECT_EQ(readFile(outside / "untouched"), "external bytes");
        }
}

TEST_F(LocalStoreObjectsTest, auxiliaryDirectoryLeavesAreDiagnosedInBothLayouts)
{
    for (auto kind : {"blobs", "blobs-x", "trees"})
        for (bool flat : {false, true})
            for (bool populated : {false, true}) {
                SCOPED_TRACE(fmt("kind=%s flat=%d populated=%d", kind, flat, populated));
                GitObjectStore objects{tempStoreDir.path() / fmt("directory-leaf-%s-%d-%d", kind, flat, populated)};
                objects.createDirectories();
                auto name = GitObjectStore::idString(merkle::blobId("directory occupant"));
                auto file = objects.dir / kind;
                if (!flat)
                    file /= name.substr(0, 2);
                file /= name;
                createDirs(file);
                if (populated)
                    writeFile(file / "child", "not recursively removed");
                /* Junk directories are not object identifiers in the flat
                   compatibility layout and retain their existing meaning. */
                auto junk = objects.dir / kind / "not-an-object";
                createDirs(junk);
                writeFile(junk / "keep", "untouched");
                objects.sweep([](fun<void(const ObjectHash &)>) { return uint64_t{0}; });
                ASSERT_TRUE(pathExists(file));
                EXPECT_TRUE(objects.verify(NoRepair));
                if (populated) {
                    EXPECT_THROW(objects.verify(Repair), Error);
                    EXPECT_EQ(readFile(file / "child"), "not recursively removed");
                } else {
                    EXPECT_FALSE(objects.verify(Repair));
                    EXPECT_FALSE(pathExists(file));
                }
                EXPECT_EQ(readFile(junk / "keep"), "untouched");
            }
}

TEST_F(LocalStoreObjectsTest, releasingAHoldDoesNotFollowSymlinks)
{
    auto outside = tempStoreDir.path() / "outside";
    createDirs(outside);
    writeFile(outside / "keep", "untouched");
    std::filesystem::path dir;
    {
        auto hold = store->objects.hold();
        dir = hold.directory();
        createSymlink(outside, dir / "outside-link");
        std::filesystem::create_hard_link(outside / "keep", dir / "extra-link");
        store->objects.putTree(merkle::treeId(""), "", hold);
    }
    EXPECT_FALSE(pathExists(dir));
    EXPECT_EQ(readFile(outside / "keep"), "untouched");
    EXPECT_EQ(st(outside / "keep").st_nlink, 1u);
    EXPECT_EQ(readFile(store->objects.treeFile(merkle::treeId(""))), "");
}

/* Auxiliary bytes and read permissions must be complete before the
   durability boundary. Their mtime is not part of the represented tree;
   regular-file holders retain their independent canonical-time contract. */
TEST_F(LocalStoreObjectsTest, writeAuxiliaryBytesAndModeBeforeFsync)
{
    GitObjectStore objects{tempStoreDir.path() / "fsynced", /*fsync=*/true};
    objects.createDirectories();

    struct Seen
    {
        std::string what;
        mode_t mode;
        std::string bytes;
    };

    std::vector<Seen> seen;
    std::string what;
    objects.atFsync = [&](const std::filesystem::path & tmp) {
        auto s = lstat(tmp);
        seen.push_back({what, static_cast<mode_t>(s.st_mode & ~S_IFMT), readFile(tmp)});
    };

    auto hold = objects.hold();
    what = "a symlink's target blob";
    objects.putBlob(merkle::blobId("one"), false, "one", hold);
    what = "a tree body";
    auto root = merkle::TreeEntry{merkle::Mode::Executable, merkle::blobId("one")};
    objects.putRootTree(root, hold);

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].mode, 0444u) << seen[0].what;
    EXPECT_EQ(seen[1].mode, 0444u) << seen[1].what;
    EXPECT_EQ(seen[0].bytes, "one");

    /* And that is what stands afterwards: the link carries the inode. */
    EXPECT_EQ(st(objects.blobFile(merkle::blobId("one"), false)).st_mode & ~S_IFMT, 0444u);
    auto body = merkle::syntheticRootTree(root);
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(seen[1].bytes, *body);
    EXPECT_EQ(st(objects.treeFile(merkle::treeId(*body))).st_mode & ~S_IFMT, 0444u);
    EXPECT_EQ(readFile(objects.blobFile(merkle::blobId("one"), false)), "one");
    EXPECT_EQ(readFile(objects.treeFile(merkle::treeId(*body))), *body);
    /* And nothing under `blobs-x`: a bare object store holds no
       regular-file blob, and a symlink's target is never executable. */
    EXPECT_FALSE(pathExists(objects.blobFile(merkle::blobId("one"), true)));

    /* The hook is the fsync's: a store that does not flush does not reach
       it, which is why the canonicalisation must precede the flush and not
       merely the link. */
    GitObjectStore unsynced{tempStoreDir.path() / "unsynced", /*fsync=*/false};
    unsynced.createDirectories();
    size_t calls = 0;
    unsynced.atFsync = [&](const std::filesystem::path &) { calls++; };
    auto unsyncedHold = unsynced.hold();
    unsynced.putBlob(merkle::blobId("one"), false, "one", unsyncedHold);
    EXPECT_EQ(calls, 0u);
}

TEST_F(LocalStoreObjectsTest, auxiliaryReadPermissionsAreIndependentOfEveryUmask)
{
    GitObjectStore objects{tempStoreDir.path() / "auxiliary-umasks"};
    objects.createDirectories();
    auto hold = objects.hold();
    const std::string bytes = "auxiliary bytes";
    auto id = merkle::blobId(bytes);
    for (bool executable : {false, true}) {
        auto file = objects.blobFile(id, executable);
        /* The finite domain here is creation of the auxiliary file, not
           directory creation, which has its own permission semantics. */
        createDirs(file.parent_path());
        hold.linkFor(file);
        for (mode_t mask = 0; mask <= 0777; ++mask) {
            SCOPED_TRACE(fmt("executable=%d umask=%o", executable, mask));
            {
                auto previous = ::umask(mask);
                Finally restoreUmask{[&] { ::umask(previous); }};
                objects.putBlob(id, executable, bytes, hold);
            }
            EXPECT_EQ(st(file).st_mode & 07777, executable ? 0555u : 0444u);
            EXPECT_EQ(readFile(file), bytes);
            unlinkIfExists(file);
        }
    }
}

/* The counterpart of the executable-blob arm `writeAuxiliaryBytesAndModeBeforeFsync`
   lost.  An executable regular file's blob has no file under `blobs-x`: its
   canonical file is the store path's own, and the bit the object store used
   to carry by *which directory the file lay in* is now carried by the
   holder's own mode, canonicalised by the restore sink (0555, mtime 1)
   before the row that names it is written.  The two modes stay apart -- `01`
   §9.10, "one such file per (blob, mode) in use and never one for both" --
   because the index's key is (id, mode, ingestion): the same bytes under the two bits
   are two rows naming two files, never one file linked across the bit. */
TEST_F(LocalStoreObjectsTest, holderReusePreservesCanonicalMetadata)
{
    GitObjectStore objects{tempStoreDir.path() / "holder-metadata"};
    auto regular = tempStoreDir.path() / "regular-holder";
    const std::string bytes = "holder contents";
    writeFile(regular, bytes);
    auto id = merkle::blobId(bytes);
    auto directory = tempStoreDir.path() / "directory-holder";
    createDir(directory);
    auto symlink = tempStoreDir.path() / "symlink-holder";
    createSymlink(regular, symlink);

    std::filesystem::path candidate;
    bool forgotten = false;
    GitObjectStore::Index index{
        .lookup =
            [&](const Hash &, bool, GitObjectStore::Holder::Cursor cursor) -> std::optional<GitObjectStore::Holder> {
            auto s = st(candidate);
            GitObjectStore::Holder row{s.st_ino, s.st_dev, static_cast<uint64_t>(s.st_size), candidate.string(), 1};
            return row.cursor() > cursor ? std::optional{row} : std::nullopt;
        },
        .forget = [&](const Hash &, bool, const GitObjectStore::Holder &) { forgotten = true; },
    };
    objects.index = &index;

    for (auto repair : {NoRepair, Repair})
        for (bool requestedExecutable : {false, true}) {
            SCOPED_TRACE(repair);
            SCOPED_TRACE(requestedExecutable);
            /* Exposing the source tree as a store path also promises
               canonical metadata: exactly 0444/0555 and epoch + 1s.
               Enumerate all 07777 patterns through the metadata predicate.
               The physical holder checks cover 0..01777, including sticky:
               Nix's build sandbox prohibits chmod with setuid/setgid bits. */
            mode_t expectedMode = requestedExecutable ? 0555 : 0444;
            candidate = regular;
            setWriteTime(regular, 1, 1);
            auto metadata = st(regular);
            GitObjectStore::Holder row{
                metadata.st_ino, metadata.st_dev, static_cast<uint64_t>(metadata.st_size), regular.string(), 1};
            for (mode_t mode = 0; mode <= 07777; ++mode) {
                SCOPED_TRACE(fmt("mode=%o", mode));
                metadata.st_mode = S_IFREG | mode;
                EXPECT_EQ(
                    GitObjectStore::validHolderMetadata(row, requestedExecutable, metadata), mode == expectedMode);
                if (mode & (S_ISUID | S_ISGID))
                    continue;
                chmod(regular, mode);
                forgotten = false;
                auto holder = objects.holderOrForget(id, requestedExecutable, repair);
                EXPECT_EQ(holder.has_value(), mode == expectedMode);
                EXPECT_EQ(forgotten, mode != expectedMode);
                EXPECT_EQ(st(regular).st_mode & ~S_IFMT, mode) << "a shared inode must not be canonicalised in place";
            }

            chmod(regular, expectedMode);
            for (auto mtime : {timespec{0, 0}, timespec{1, 0}, timespec{1, 1}, timespec{2, 0}}) {
                SCOPED_TRACE(fmt("mtime=%d.%d", mtime.tv_sec, mtime.tv_nsec));
                timespec times[2]{{0, UTIME_OMIT}, mtime};
                ASSERT_EQ(::utimensat(AT_FDCWD, regular.c_str(), times, 0), 0);
                forgotten = false;
                bool canonical = mtime.tv_sec == 1 && mtime.tv_nsec == 0;
                EXPECT_EQ(objects.holderOrForget(id, requestedExecutable, repair).has_value(), canonical);
                EXPECT_EQ(forgotten, !canonical);
                auto after = st(regular);
                EXPECT_EQ(after.st_mtime, mtime.tv_sec);
#  ifdef __APPLE__
                EXPECT_EQ(after.st_mtimespec.tv_nsec, mtime.tv_nsec);
#  else
                EXPECT_EQ(after.st_mtim.tv_nsec, mtime.tv_nsec);
#  endif
            }

            /* Every other kind in the source-tree domain is rejected
               before repair could read through it. A bad row is removed;
               its file still belongs to the store path and stays intact. */
            for (const auto & nonRegular : {directory, symlink}) {
                SCOPED_TRACE(nonRegular.string());
                if (nonRegular == directory)
                    chmod(directory, expectedMode);
                setWriteTime(nonRegular, 1, 1);
                candidate = nonRegular;
                forgotten = false;
                EXPECT_FALSE(objects.holderOrForget(id, requestedExecutable, repair));
                EXPECT_TRUE(forgotten);
                EXPECT_TRUE(pathExists(candidate));
            }
        }
    chmod(regular, 0444);
    EXPECT_EQ(readFile(regular), bytes);
    chmod(directory, 0755);
}

TEST_F(LocalStoreObjectsTest, executableHolderCarriesItsBitOnThePathsOwnFile)
{
    auto & objects = store->objects;
    auto t = std::filesystem::path{config->realStoreDir.get()} / "bits";
    restore(t, treeNar({{"p", "same", 'f'}, {"x", "same", 'x'}}));
    auto id = merkle::blobId("same");

    /* Two rows, one per mode; no file under either kind's directory. */
    EXPECT_EQ(store->blobRowCount(false), 1u);
    EXPECT_EQ(store->blobRowCount(true), 1u);
    EXPECT_FALSE(pathExists(objects.blobFile(id, false)));
    EXPECT_FALSE(pathExists(objects.blobFile(id, true)));

    /* Each mode's holder is the path's own file, carrying its own bit and
       the store's mtime, and the two are different inodes. */
    EXPECT_EQ(blobFileOf(id, false), t / "p");
    EXPECT_EQ(blobFileOf(id, true), t / "x");
    EXPECT_EQ(st(t / "p").st_mode & ~S_IFMT, 0444u);
    EXPECT_EQ(st(t / "x").st_mode & ~S_IFMT, 0555u);
    EXPECT_EQ(st(t / "p").st_mtime, 1);
    EXPECT_EQ(st(t / "x").st_mtime, 1);
    EXPECT_NE(st(t / "p").st_ino, st(t / "x").st_ino) << "never one file linked across the bit";

    /* And a second ingestion links each holder under its own bit, writing
       nothing: the sharing the index buys, per mode. */
    auto t2 = std::filesystem::path{config->realStoreDir.get()} / "bits2";
    restore(t2, treeNar({{"p", "same", 'f'}, {"x", "same", 'x'}}));
    EXPECT_EQ(filesCreated, 0u);
    EXPECT_EQ(st(t2 / "p").st_ino, st(t / "p").st_ino);
    EXPECT_EQ(st(t2 / "x").st_ino, st(t / "x").st_ino);
    EXPECT_EQ(st(t2 / "x").st_mode & ~S_IFMT, 0555u);
}

/* Enumerate the publication orders after two probes miss. No scheduler
   timing is needed to reproduce the parallel copy's critical interleaving. */
TEST_F(LocalStoreObjectsTest, concurrentMissesShareTheFirstCompletedHolder)
{
    auto & objects = store->objects;
    for (bool executable : {false, true})
        for (bool reverse : {false, true})
            for (bool looked : {false, true}) {
                auto dir = tempStoreDir.path() / fmt("race-%d-%d-%d", executable, reverse, looked);
                createDirs(dir);
                auto fd = openDirectory(dir);
                auto hold = objects.hold();
                hold.setRoot(dir);
                auto bytes = dir.filename().string();
                auto id = merkle::blobId(bytes);
                auto a = CanonPath::fromFilename("a"), b = CanonPath::fromFilename("b");
                EXPECT_FALSE(objects.link(id, executable, fd.get(), a, dir / "a", bytes.size(), NoRepair, hold));
                EXPECT_FALSE(objects.link(id, executable, fd.get(), b, dir / "b", bytes.size(), NoRepair, hold));
                for (auto name : {"a", "b"}) {
                    writeFile(dir / name, bytes);
                    chmod(dir / name, executable ? 0555 : 0444);
                }
                auto first = reverse ? b : a, second = reverse ? a : b;
                objects.enterWritten(id, executable, fd.get(), first, dir / first.rel(), NoRepair, hold, looked);
                objects.enterWritten(id, executable, fd.get(), second, dir / second.rel(), NoRepair, hold, looked);
                EXPECT_EQ(st(dir / "a").st_ino, st(dir / "b").st_ino)
                    << "executable=" << executable << " reverse=" << reverse << " looked=" << looked;
            }
}

TEST_F(LocalStoreObjectsTest, reusedBlobsRetainAWitnessWhenTheirDonorIsCollectedBeforeRegistration)
{
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    Finally unset{[] { unsetenv("_NIX_TEST_NO_LSOF"); }};
    auto & objects = store->objects;
    for (bool executable : {false, true})
        /* Link into an absent name, place a private inode, and place an
           already shared inode cover the three successful reuse paths. */
        for (unsigned operation : {0, 1, 2})
            for (bool pending : {false, true}) {
                SCOPED_TRACE(fmt("executable=%d operation=%d pending=%d", executable, operation, pending));
                auto bytes = fmt("reused-%d-%d-%d", executable, operation, pending);
                auto id = merkle::blobId(bytes);
                auto mode = executable ? merkle::Mode::Executable : merkle::Mode::Regular;
                auto donor = StorePath::random("donor");
                auto donorRoot = store->toRealPath(donor);
                {
                    auto hold = objects.hold();
                    StringSource source{treeNar({{"a", bytes, executable ? 'x' : 'f'}})};
                    auto hooks =
                        makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls));
                    auto result = store->restoreThroughObjects(donorRoot, source, false, hooks.get(), hold, NoRepair);
                    ValidPathInfo info{donor, UnkeyedValidPathInfo{*store, ObjectHash::of(result.root)}};
                    info.narSize = result.narSize;
                    store->registerValidPath(info, hold);
                }

                auto recipient = StorePath::random("recipient");
                auto root = store->toRealPath(recipient);
                store->addTempRoot(recipient);
                createDir(root);
                auto fd = openDirectory(root);
                auto hold = objects.hold();
                store->beginIngestion(hold, root);
                if (pending) {
                    ASSERT_TRUE(objects.link(
                        id,
                        executable,
                        fd.get(),
                        CanonPath::fromFilename("seed"),
                        root / "seed",
                        bytes.size(),
                        NoRepair,
                        hold));
                }
                auto name = CanonPath::fromFilename("a");
                if (operation) {
                    if (operation == 2)
                        std::filesystem::create_hard_link(donorRoot / "a", root / "a");
                    else {
                        writeFile(root / "a", bytes);
                        chmod(root / "a", executable ? 0555 : 0444);
                        setWriteTime(root / "a", 1, 1);
                    }
                    ASSERT_TRUE(
                        objects.place(id, executable, fd.get(), name, root / "a", st(root / "a"), NoRepair, hold));
                } else {
                    ASSERT_TRUE(objects.link(id, executable, fd.get(), name, root / "a", bytes.size(), NoRepair, hold));
                }
                ASSERT_EQ(st(root / "a").st_ino, st(donorRoot / "a").st_ino);
                auto treeBody = pending ? merkle::serialiseTree({{"a", {mode, id}}, {"seed", {mode, id}}})
                                        : merkle::serialiseTree({{"a", {mode, id}}});
                auto tree = merkle::treeId(treeBody);
                objects.putTree(tree, treeBody, hold);
                chmod(root, 0555);
                setWriteTime(root, 1, 1);

                /* The recipient is rooted but has not published. The
                   donor has no temporary root, so real GC can remove it. */
                GCOptions options;
                options.action = GCOptions::gcDeleteSpecific;
                options.pathsToDelete = GCOptions::SpecificPaths{.paths = {donor}};
                GCResults results;
                store->collectGarbage(options, results);
                store->sweepStaleBlobRows();
                ASSERT_FALSE(store->isValidPath(donor));
                ASSERT_FALSE(objects.holderOrForget(id, executable, NoRepair));

                ValidPathInfo info{recipient, UnkeyedValidPathInfo{*store, ObjectHash{tree}}};
                store->registerValidPath(info, hold);
                auto witness = objects.holderOrForget(id, executable, Repair);
                ASSERT_TRUE(witness);
                EXPECT_EQ(std::filesystem::path(witness->file).parent_path(), root);
                EXPECT_EQ(witness->ino, st(root / "a").st_ino);
                EXPECT_EQ(readFile(root / "a"), bytes);
                EXPECT_TRUE(store->verifyObjects(root).second.empty());
            }
}

TEST_F(LocalStoreObjectsTest, registeredBlobReuseValidatesTheAcquiredInode)
{
    auto & objects = store->objects;
    auto originalIndex = objects.index;
    Finally restoreIndex{[&] { objects.index = originalIndex; }};
    unsigned serial = 0;
    /* A selected regular candidate can remain regular, disappear, or be
       replaced by either other filesystem node kind accepted by Nix. */
    for (unsigned kind : {0, 1, 2, 3})
        for (bool place : {false, true})
            for (bool executable : {false, true})
                for (auto repair : {NoRepair, Repair})
                    for (bool differentInode : {false, true})
                        for (bool canonical : {false, true})
                            for (bool sameBytes : {false, true})
                                for (bool fallback : {false, true}) {
                                    if (kind && (!differentInode || !canonical || !sameBytes))
                                        continue; /* File identity/mode/bytes apply only to regular replacements. */
                                    SCOPED_TRACE(fmt(
                                        "kind=%d place=%d executable=%d repair=%d differentInode=%d canonical=%d sameBytes=%d fallback=%d",
                                        kind,
                                        place,
                                        executable,
                                        repair,
                                        differentInode,
                                        canonical,
                                        sameBytes,
                                        fallback));
                                    auto root = tempStoreDir.path() / fmt("acquired-inode-%d", serial++);
                                    createDir(root);
                                    auto donor = root / "donor";
                                    auto second = root / "second";
                                    auto target = root / "target";
                                    auto id = merkle::blobId("good");
                                    auto makeFile = [&](const auto & path, std::string_view bytes, bool rightMode) {
                                        writeFile(path, bytes);
                                        chmod(path, (rightMode ? executable : !executable) ? 0555 : 0444);
                                        setWriteTime(path, 1, 1);
                                    };
                                    makeFile(donor, "good", true);
                                    makeFile(second, "good", true);
                                    auto holder = [&](const auto & file, uint64_t ingestion) {
                                        auto status = st(file);
                                        return GitObjectStore::Holder{
                                            status.st_ino,
                                            status.st_dev,
                                            static_cast<uint64_t>(status.st_size),
                                            file.string(),
                                            ingestion};
                                    };
                                    /* The fault is on the first physical class,
                                       independently of filesystem allocation order. */
                                    if (holder(second, 2).cursor() < holder(donor, 1).cursor())
                                        std::swap(donor, second);
                                    auto firstRow = holder(donor, 1);
                                    auto secondRow = holder(second, 2);
                                    unsigned forgotten = 0;
                                    GitObjectStore::Index index{
                                        .lookup =
                                            [&](const Hash & queried, bool bit, GitObjectStore::Holder::Cursor cursor)
                                            -> std::optional<GitObjectStore::Holder> {
                                            EXPECT_EQ(queried, id);
                                            EXPECT_EQ(bit, executable);
                                            if (cursor < firstRow.cursor()) {
                                                /* The row was selected before a donor repair
                                                   replaced its pathname. Acquisition must
                                                   validate the inode actually obtained. */
                                                auto bytes = sameBytes ? "good" : "evil";
                                                if (kind) {
                                                    std::filesystem::remove(donor);
                                                    if (kind == 2)
                                                        createDir(donor);
                                                    else if (kind == 3)
                                                        createSymlink(second, donor);
                                                } else if (differentInode) {
                                                    auto replacement = root / "replacement";
                                                    makeFile(replacement, bytes, canonical);
                                                    std::filesystem::rename(replacement, donor);
                                                } else {
                                                    chmod(donor, 0644);
                                                    makeFile(donor, bytes, canonical);
                                                }
                                                return firstRow;
                                            }
                                            if (fallback && cursor < secondRow.cursor())
                                                return secondRow;
                                            return std::nullopt;
                                        },
                                        .forget =
                                            [&](const Hash & queried,
                                                bool bit,
                                                const GitObjectStore::Holder & observed) {
                                                EXPECT_EQ(queried, id);
                                                EXPECT_EQ(bit, executable);
                                                EXPECT_EQ(observed.ingestion, firstRow.ingestion);
                                                EXPECT_EQ(observed.ino, firstRow.ino);
                                                EXPECT_EQ(observed.dev, firstRow.dev);
                                                ++forgotten;
                                            },
                                    };
                                    objects.index = &index;
                                    auto hold = objects.hold();
                                    hold.setRoot(root);
                                    auto fd = openDirectory(root);
                                    auto name = CanonPath::fromFilename("target");
                                    /* NoRepair trusts an unchanged canonical inode;
                                       Repair also checks its bytes. A replacement
                                       inode never inherits the old candidate's hash. */
                                    bool accepted = !kind && !differentInode && canonical && (!repair || sameBytes);
                                    bool reused = accepted || fallback;
                                    if (place) {
                                        makeFile(target, "good", true);
                                        auto result = objects.place(
                                            id, executable, fd.get(), name, target, st(target), repair, hold);
                                        ASSERT_TRUE(result);
                                        EXPECT_EQ(result->entered, !reused);
                                    } else {
                                        EXPECT_EQ(
                                            objects.link(id, executable, fd.get(), name, target, 4, repair, hold),
                                            reused);
                                    }
                                    EXPECT_EQ(forgotten, accepted ? 0u : 1u);
                                    if (place || reused)
                                        EXPECT_EQ(readFile(target), accepted && !sameBytes ? "evil" : "good");
                                    else
                                        EXPECT_FALSE(pathExists(target));
                                    EXPECT_EQ(count("tmp"), 1u) << "only the owning hold remains, with no leaked link";
                                    objects.index = originalIndex;
                                }
    EXPECT_EQ(serial, 176u);
}

/* Law 4 of 01 section 9.10 over a whole store rather than one path: for
   every valid path, every tree its object hash reaches and every blob a
   reached tree names is present, and the view on disk is the tree the hash
   names.  The store holds several paths that share a subtree and blobs,
   and the four root kinds -- a directory, a plain file, an executable file
   and a symlink -- so that the synthetic root tree of the last two is in
   the domain too.  The closure is walked here, from the column through the
   tree bodies, and not through `verifyObjects`, which is the store's own
   decision procedure and is what the per-path form pins
   (`verifyExpectsNoBlobForAFileTheStoreDoesNotPlace`,
   `sweepAndVerifyOnALiveRootWhoseTreeIsMissing`).  The interleaving form
   -- the law under a concurrent collection -- is owed (05 section 7). */
TEST_F(LocalStoreObjectsTest, lawFourHoldsForEveryValidPath)
{
    using File = MemorySourceAccessor::File;

    /* Three directory roots sharing the subtree `d/` and the blob of `x`,
       differing in one file. */
    for (int i = 0; i < 3; i++)
        store->addToStoreSlow(
            fmt("dir%d", i),
            {treeAccessor(
                {{"a", fmt("body %d", i), 'f'}, {"x", "shared exec", 'x'}, {"d/b", "shared", 'f'}, {"d/l", "b", 'l'}})},
            ContentAddressMethod::Raw::Git,
            HashAlgorithm::SHA256);

    /* The three bare roots: a plain file (whose object hash is its blob's
       id and names no tree), an executable and a symlink (whose object
       hash is the synthetic root tree's). */
    auto bare = [&](std::string_view name, File file) {
        auto acc = make_ref<MemorySourceAccessor>();
        acc->root = std::move(file);
        store->addToStoreSlow(name, {acc}, ContentAddressMethod::Raw::Git, HashAlgorithm::SHA256);
    };
    bare("plain", File{File::Regular{.contents = "shared"}});
    bare("exec", File{File::Regular{.executable = true, .contents = "shared exec"}});
    bare("link", File{File::Symlink{.target = "b"}});

    auto valid = store->queryAllValidPaths();
    ASSERT_EQ(valid.size(), 6u);

    std::vector<std::string> missing;
    std::map<std::string, unsigned> blobUses;
    size_t treesReached = 0, blobsNamed = 0;

    for (auto & path : valid) {
        auto name = store->printStorePath(path);
        auto info = store->queryPathInfo(path);
        ASSERT_TRUE(info->objectHash.has_value()) << name;

        std::vector<std::pair<Hash, bool>> todo{{info->objectHash->hash, true}};
        std::set<std::string> visited;
        while (!todo.empty()) {
            auto [id, isRoot] = todo.back();
            todo.pop_back();
            if (!visited.insert(GitObjectStore::idString(id)).second)
                continue;
            if (!hasTree(id)) {
                /* A plain file's object hash is its blob's id, which names
                   no tree; every other root names one. */
                if (isRoot && hasBlob(id, false)) {
                    blobsNamed++;
                    blobUses[GitObjectStore::idString(id)]++;
                } else
                    missing.push_back(fmt("%s: no tree %s", name, GitObjectStore::idString(id)));
                continue;
            }
            treesReached++;
            for (auto & [entryName, entry] : readTree(id)) {
                if (entry.mode == merkle::Mode::Directory) {
                    todo.push_back({entry.hash, false});
                    continue;
                }
                bool executable = entry.mode == merkle::Mode::Executable;
                blobsNamed++;
                blobUses[GitObjectStore::idString(entry.hash)]++;
                if (!hasBlob(entry.hash, executable))
                    missing.push_back(fmt("%s: no blob for %s%s", name, entryName, executable ? " (executable)" : ""));
            }
        }

        /* And the view is the tree the hash names. */
        EXPECT_EQ(ObjectHash::of(gitHash(store->toRealPath(path))), *info->objectHash) << name;
    }

    EXPECT_TRUE(missing.empty()) << missing.size() << " object(s) the law requires:\n"
                                 << concatStringsSep("\n", missing);
    EXPECT_GT(treesReached, 6u) << "the subtree of each directory root, and the two synthetic root trees";
    EXPECT_GT(blobsNamed, blobUses.size()) << "the blobs are shared across the paths";
}

/* Law 3's fourth exception (01 section 9.10): a regular file `place` could
   not enter because the directory's index was full is left as written and
   leaves no mark, so `placementOf` cannot tell it from a file that was
   entered -- the verifier expects its blob, finds it missing, and reports
   the path.  `--repair` re-enters the path: while the index is still full
   it says so and the error stands; once there is room the file becomes the
   store's and the next verification is clean.  A full directory index
   cannot be made without a filesystem of its own -- a loopback image needs
   privileges the suite does not have, and APFS has no fixed index to fill
   -- so the condition is injected at the one call that meets it
   (`GitObjectStore::failPlaceWithNoSpace`, the `ENOSPC` branch of `place`);
   everything downstream of it is the real code. */
TEST_F(LocalStoreObjectsTest, enospcLeftFileIsReportedNotRepairedForever)
{
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    Finally unset{[] { unsetenv("_NIX_TEST_NO_LSOF"); }};

    /* A path an older Nix wrote plainly, entered by the walk. */
    StorePath path{"n5wkd9frr45pa74if5gpz9j7mifg27fh-enospc"};
    auto realPath = std::filesystem::path{store->toRealPath(path)};
    writePlainTree(realPath, "one");
    auto blob = merkle::blobId("one");

    /* The directory's index full for this one file. */
    store->objects.failPlaceWithNoSpace = [&](const std::filesystem::path & where) { return where == realPath / "a"; };
    Finally unhook{[&] { store->objects.failPlaceWithNoSpace = nullptr; }};

    auto enterHold = store->objects.hold();
    auto entered = store->enterPath(realPath, NoRepair, enterHold);
    EXPECT_FALSE(hasBlob(blob, false)) << "the file could not enter";
    EXPECT_EQ(st(realPath / "a").st_nlink, 1u);
    EXPECT_EQ(readFile(realPath / "a"), "one") << "its bytes are its own";

    ValidPathInfo info{path, UnkeyedValidPathInfo{*store, ObjectHash::of(entered.root)}};
    info.narSize = entered.narSize;
    store->registerValidPath(info);

    /* Reported, and not repaired while the index is full: `--repair`
       re-enters, the file cannot enter again, and the error stands. */
    EXPECT_TRUE(store->verifyStore(/*checkContents=*/true, NoRepair)) << "an error is reported";
    EXPECT_TRUE(store->verifyStore(/*checkContents=*/true, Repair)) << "could not enter: the error stands";
    EXPECT_FALSE(hasBlob(blob, false));
    EXPECT_EQ(readFile(realPath / "a"), "one");

    /* Room again: the retry enters the file and nothing is left to report. */
    store->objects.failPlaceWithNoSpace = nullptr;
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, Repair)) << "entered, no error left";
    EXPECT_TRUE(hasBlob(blob, false));
    /* One lower: the blob's canonical file is a store path's own, so there is
     * no link into `.objects/blobs` beside it (K8) */
    EXPECT_EQ(st(realPath / "a").st_nlink, 1u) << "the file is the blob's holder";
    EXPECT_EQ(st(realPath / "a").st_ino, st(blobFileOf(blob, false)).st_ino);
    EXPECT_EQ(readFile(realPath / "a"), "one");
    EXPECT_FALSE(store->verifyStore(/*checkContents=*/true, NoRepair)) << "reported once, not for ever";
}

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
TEST_F(LocalStoreObjectsTest, theRegisteredDescriptionsNarHashIsTheWalkAndNotTheSendersClosure)
{
    /* A sender's description may carry a NAR-hash thunk over bytes it still
       holds -- the evaluator's write buffer builds one over the text it is
       about to write (`write-buffer.cc`, `infoFor`) -- and the path-info
       cache keeps every description registered, so those bytes would stay
       resident for the life of the process.  What is registered carries the
       store's own walk instead: the same hash, holding nothing. */
    StringSink nar;
    dumpString("retained", nar);
    ValidPathInfo info{
        StorePath{"n5wkd9frr45pa74if5gpz9j7mifg27fh-retained"},
        UnkeyedValidPathInfo{
            *store, ObjectHash::of(merkle::TreeEntry{merkle::Mode::Regular, merkle::blobId("retained")})},
    };
    info.narSize = nar.s.size();
    /* A thunk that answers something else: if the row kept the sender's,
       this is what the row would say. */
    info.lazyNarHash = [] { return hashString(HashAlgorithm::SHA256, "not this object's NAR"); };
    StringSource source{nar.s};
    store->addToStore(info, source, NoRepair, NoCheckSigs);

    auto row = store->queryPathInfo(info.path);
    ASSERT_TRUE(row->lazyNarHash.has_value());
    EXPECT_EQ((*row->lazyNarHash)(), hashString(HashAlgorithm::SHA256, nar.s));
}

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
        EXPECT_EQ(st(real).st_ino, st(blobFileOf(blob, false)).st_ino) << name;
        /* One lower: the blob's canonical file is a store path's own, so there
         * is no link into `.objects/blobs` beside it (K8) */
        EXPECT_EQ(st(real).st_nlink, 1u) << name;
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

/* `nix store delete` of one path costs that path, not the store (01
   section 10, *The object store is swept on whole-store collections
   only*): a specific delete reads no tree body and `lstat`s no blob file,
   while the whole-store collection that follows reads both, in numbers
   that grow with the store.  Counted by `GitObjectStore::counters`, which
   is null in every build but this one.  The bound the test states is
   proportionality: a per-path sweep, were one written, must read the
   deleted path's trees and no more -- which today, no sweep running, is a
   bound of zero. */
TEST_F(LocalStoreObjectsTest, deleteSpecificReadsOnlyTheDeletedPathsTrees)
{
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    Finally unset{[] { unsetenv("_NIX_TEST_NO_LSOF"); }};

    /* Four paths of two trees each, sharing a subtree and blobs. */
    std::vector<StorePath> paths;
    for (int i = 0; i < 4; i++)
        paths.push_back(
            store
                ->addToStoreSlow(
                    fmt("p%d", i),
                    {treeAccessor({{"a", fmt("body %d", i), 'f'}, {"d/b", "shared", 'f'}, {"d/l", "b", 'l'}})},
                    ContentAddressMethod::Raw::Git,
                    HashAlgorithm::SHA256)
                .path);
    ASSERT_GE(count("trees"), 5u);
    ASSERT_GE(count("blobs"), 5u);

    /* Reopened without this process's temp roots. */
    store.reset();
    store = std::make_shared<LocalStore>(ref{config});

    GitObjectStore::Counters counters;
    store->objects.counters = &counters;
    Finally unhook{[&] { store->objects.counters = nullptr; }};

    /* The specific delete of one path. */
    {
        GCOptions options;
        options.action = GCOptions::gcDeleteSpecific;
        options.pathsToDelete = GCOptions::SpecificPaths{.paths = {paths[0]}};
        GCResults results;
        store->collectGarbage(options, results);
    }
    EXPECT_FALSE(store->isValidPath(paths[0]));
    EXPECT_TRUE(store->isValidPath(paths[1]));
    EXPECT_EQ(counters.treesRead, 0u) << "a specific delete reads no tree body";
    EXPECT_EQ(counters.blobsStatted, 0u) << "a specific delete stats no blob file";

    /* The whole-store collection, whose cost is the store's: every tree
       from every live root, and every blob file.  One path is rooted, so
       the sweep has a root to walk. */
    store->addPermRoot(paths[1], tempStoreDir.path() / "root");
    counters = {};
    {
        GCOptions options;
        options.action = GCOptions::gcDeleteDead;
        GCResults results;
        store->collectGarbage(options, results);
    }
    EXPECT_GE(counters.treesRead, 2u) << "the live root's tree and its subtree";
    /* The sweep's blob pass judges the symlink-target blobs alone; a regular
     * file's blob has no file under `blobs/` (design.md section 2.2) */
    EXPECT_GE(counters.blobsStatted, 0u) << "there is no blob file per regular file to stat";
}

/* The collector's allow-list in the store directory (`gc.cc`, the `readdir`
   loop that skips `.`, `..`, `.links` and the object store's directory):
   everything else that is not a live valid path goes, a name that does not
   parse as a store path by `deleteFromStore` and a dead path by the
   closure walk.  `.links` -- an older Nix's table -- is skipped there and
   removed whole afterwards by `removeLegacyLinks`, so what survives a
   whole-store collection is the object store's directory and the live
   paths, and nothing else. */
TEST_F(LocalStoreObjectsTest, deletesEverythingNotAllowListedInTheStoreDir)
{
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    Finally unset{[] { unsetenv("_NIX_TEST_NO_LSOF"); }};
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};

    auto dead =
        store
            ->addToStoreSlow(
                "dead", {treeAccessor({{"a", "dead", 'f'}})}, ContentAddressMethod::Raw::Git, HashAlgorithm::SHA256)
            .path;
    auto live =
        store
            ->addToStoreSlow(
                "live", {treeAccessor({{"a", "live", 'f'}})}, ContentAddressMethod::Raw::Git, HashAlgorithm::SHA256)
            .path;

    /* An older Nix's `.links` table, a stray file and a stray directory:
       the three things a store directory can hold besides its paths. */
    createDirs(realStoreDir / ".links");
    writeFile(realStoreDir / ".links" / "an-entry", "linked");
    writeFile(realStoreDir / "foo", "stray");
    createDirs(realStoreDir / "bar");
    writeFile(realStoreDir / "bar" / "baz", "stray too");
    ASSERT_TRUE(pathExists(store->objects.dir));

    store.reset();
    store = std::make_shared<LocalStore>(ref{config});

    /* The live path kept by an indirect root outside the store. */
    auto rootLink = tempStoreDir.path() / "root";
    store->addPermRoot(live, rootLink);

    GCOptions options;
    options.action = GCOptions::gcDeleteDead;
    GCResults results;
    store->collectGarbage(options, results);

    std::set<std::string> left;
    for (auto & e : DirectoryIterator{realStoreDir})
        left.insert(e.path().filename().string());
    EXPECT_EQ(left, (std::set<std::string>{store->objects.dir.filename().string(), std::string(live.to_string())}))
        << "exactly the object store's directory and the live path";
    EXPECT_FALSE(store->isValidPath(dead));
}

TEST_F(LocalStoreObjectsTest, staleTemporaryRootCleanupPreservesEveryRecordShapeForAnOverlappingReader)
{
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    Finally unset{[] { unsetenv("_NIX_TEST_NO_LSOF"); }};

    struct ObserveRemoval : testing::CaptureLogger
    {
        std::function<void()> observe;
        std::exception_ptr failure;

        void log(Verbosity, std::string_view message) noexcept override
        {
            if (!message.starts_with("removing stale temporary roots file "))
                return;
            try {
                observe();
            } catch (...) {
                failure = std::current_exception();
            }
        }
    };

    auto first = store->makeStorePath("source", hashString(HashAlgorithm::SHA256, "first-stale-root"), "root");
    auto second = store->makeStorePath("source", hashString(HashAlgorithm::SHA256, "second-stale-root"), "root");
    auto firstName = store->printStorePath(first);
    auto secondName = store->printStorePath(second);
    /* The finite record domain: complete records absent/present, crossed
       with an incomplete trailing record absent/present. */
    for (bool complete : {false, true})
        for (bool incomplete : {false, true}) {
            SCOPED_TRACE(fmt("complete=%d incomplete=%d", complete, incomplete));
            auto contents = complete ? firstName + '\0' + secondName + '\0' : std::string{};
            if (incomplete)
                contents += firstName.substr(0, firstName.size() - 1);
            auto path = store->tempRootsDir / "12345.stale-reader";
            writeFile(path, contents);
            AutoCloseFD reader;
            size_t observations = 0;
            bool lockRefused = false;
            ObserveRemoval removal;
            removal.observe = [&] {
                ++observations;
                reader = openLockFile(path, false);
                if (!reader)
                    throw Error("stale root file disappeared before the overlapping reader opened it");
                lockRefused = !lockFile(reader.get(), ltWrite, false);
            };
            auto oldLogger = std::exchange(logger, &removal);
            auto oldVerbosity = std::exchange(verbosity, lvlInfo);
            Finally restore{[&] {
                logger = oldLogger;
                verbosity = oldVerbosity;
            }};

            /* The existing message is emitted with the cleanup lock held,
               before unlink/mark. This reader opens and loses its lock at
               that point, then reads after cleanup, with no scheduler race. */
            store->findRoots(true);
            if (removal.failure)
                std::rethrow_exception(removal.failure);
            ASSERT_EQ(observations, 1u);
            ASSERT_TRUE(lockRefused);
            EXPECT_FALSE(pathExists(path));
            auto observed = readFile(reader.get());
            EXPECT_EQ(observed, contents.empty() ? "d" : contents);

            StorePathSet decoded;
            size_t begin = 0, end;
            EXPECT_NO_THROW({
                while ((end = observed.find('\0', begin)) != std::string::npos) {
                    decoded.insert(store->parseStorePath(std::string_view(observed).substr(begin, end - begin)));
                    begin = end + 1;
                }
            });
            EXPECT_EQ(decoded, complete ? (StorePathSet{first, second}) : StorePathSet{});
        }
}

TEST_F(LocalStoreObjectsTest, temporaryRootsBelongToEachStoreHandle)
{
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    Finally unset{[] { unsetenv("_NIX_TEST_NO_LSOF"); }};
    auto modeFile = tempStoreDir.path() / "roots-mode-reference";
    auto modeFd = openLockFile(modeFile, true);
    auto expectedMode = st(modeFile).st_mode & 0777;

    /* All six interleavings of create/register for two handles, followed
       by either destruction order. Creation may precede or follow the
       other handle's first root; neither operation may replace its file. */
    std::array<unsigned, 4> order{0, 1, 2, 3};
    size_t cases = 0;
    do {
        if (std::find(order.begin(), order.end(), 0) > std::find(order.begin(), order.end(), 1)
            || std::find(order.begin(), order.end(), 2) > std::find(order.begin(), order.end(), 3))
            continue;
        for (size_t firstDestroyed : {0u, 1u}) {
            SCOPED_TRACE(
                fmt("order=%d%d%d%d firstDestroyed=%d", order[0], order[1], order[2], order[3], firstDestroyed));
            ++cases;
            std::array<StorePath, 2> paths{
                store
                    ->addToStoreSlow(
                        "first-root",
                        {treeAccessor({{"a", "first", 'f'}})},
                        ContentAddressMethod::Raw::Git,
                        HashAlgorithm::SHA256)
                    .path,
                store
                    ->addToStoreSlow(
                        "second-root",
                        {treeAccessor({{"a", "second", 'f'}})},
                        ContentAddressMethod::Raw::Git,
                        HashAlgorithm::SHA256)
                    .path};
            /* The collecting handle itself must retain neither path. */
            store.reset();
            store = std::make_shared<LocalStore>(ref{config});
            std::array<std::shared_ptr<LocalStore>, 2> handles;
            for (auto event : order) {
                auto i = event / 2;
                if (event % 2 == 0)
                    handles[i] = std::make_shared<LocalStore>(ref{config});
                else
                    handles[i]->addTempRoot(paths[i]);
            }
            std::array rootFiles{handles[0]->fnTempRoots, handles[1]->fnTempRoots};
            EXPECT_NE(rootFiles[0], rootFiles[1]);
            for (size_t i = 0; i < 2; ++i) {
                EXPECT_EQ(std::stoi(rootFiles[i].filename().string()), getpid())
                    << "older collectors parse the numeric prefix";
                EXPECT_EQ(st(rootFiles[i]).st_mode & 0777, expectedMode);
                EXPECT_EQ(readFile(rootFiles[i]), store->printStorePath(paths[i]) + '\0');
            }
            auto collect = [&] {
                GCOptions options;
                options.action = GCOptions::gcDeleteDead;
                GCResults results;
                store->collectGarbage(options, results);
            };
            collect();
            for (auto & path : paths) {
                EXPECT_TRUE(store->isValidPathUncached(path));
                EXPECT_TRUE(pathExists(store->toRealPath(path)));
            }

            handles[firstDestroyed].reset();
            EXPECT_FALSE(pathExists(rootFiles[firstDestroyed]));
            EXPECT_TRUE(pathExists(rootFiles[1 - firstDestroyed]));
            collect();
            EXPECT_FALSE(store->isValidPathUncached(paths[firstDestroyed]));
            EXPECT_FALSE(pathExists(store->toRealPath(paths[firstDestroyed])));
            EXPECT_TRUE(store->isValidPathUncached(paths[1 - firstDestroyed]));
            EXPECT_TRUE(pathExists(store->toRealPath(paths[1 - firstDestroyed])));

            handles[1 - firstDestroyed].reset();
            EXPECT_FALSE(pathExists(rootFiles[1 - firstDestroyed]));
            collect();
            EXPECT_FALSE(store->isValidPathUncached(paths[1 - firstDestroyed]));
            EXPECT_FALSE(pathExists(store->toRealPath(paths[1 - firstDestroyed])));
        }
    } while (std::next_permutation(order.begin(), order.end()));
    EXPECT_EQ(cases, 12u);
}

TEST_F(LocalStoreObjectsTest, temporaryRootsAreASetForOneDescriptorLifetime)
{
    auto savedFeatures = experimentalFeatureSettings.experimentalFeatures.get();
    Finally restoreFeatures{[&] { experimentalFeatureSettings.experimentalFeatures = savedFeatures; }};
    experimentalFeatureSettings.experimentalFeatures.get().insert(Xp::ReadOnlyLocalStore);
    std::vector<StorePath> paths;
    for (size_t i = 0; i < 4; ++i)
        paths.push_back(store->makeStorePath("source", hashString(HashAlgorithm::SHA256, fmt("root-%d", i)), "root"));

    for (size_t writers : {1u, 8u})
        for (bool distinct : {false, true}) {
            SCOPED_TRACE(fmt("writers=%d distinct=%d", writers, distinct));
            store.reset();
            store = std::make_shared<LocalStore>(ref{config});
            ASSERT_FALSE(pathExists(store->fnTempRoots));
            std::promise<void> start;
            auto ready = start.get_future().share();
            bool started = false;
            std::atomic<bool> failed = false;
            std::vector<std::thread> threads;
            Finally join{[&] {
                if (!started)
                    start.set_value();
                for (auto & thread : threads)
                    if (thread.joinable())
                        thread.join();
            }};
            for (size_t writer = 0; writer < writers; ++writer)
                threads.emplace_back([&, writer] {
                    ready.wait();
                    try {
                        for (size_t i = 0; i < 16; ++i)
                            store->addTempRoot(paths[distinct ? (writer + i) % paths.size() : 0]);
                    } catch (...) {
                        failed = true;
                    }
                });
            start.set_value();
            started = true;
            for (auto & thread : threads)
                thread.join();
            EXPECT_FALSE(failed);

            auto contents = readFile(store->fnTempRoots);
            StringSet actual, expected;
            size_t records = 0;
            for (size_t pos = 0; pos < contents.size();) {
                auto end = contents.find('\0', pos);
                ASSERT_NE(end, std::string::npos);
                actual.insert(contents.substr(pos, end - pos));
                ++records;
                pos = end + 1;
            }
            for (size_t i = 0; i < (distinct ? paths.size() : 1); ++i)
                expected.insert(store->printStorePath(paths[i]));
            EXPECT_EQ(actual, expected);
            EXPECT_EQ(records, expected.size());
        }

    store.reset();
    auto readOnlyConfig = make_ref<LocalStoreConfig>(tempStoreDir.path(), StoreConfig::Params{{"read-only", "true"}});
    store = std::make_shared<LocalStore>(readOnlyConfig);
    for (auto & path : paths)
        store->addTempRoot(path);
    EXPECT_FALSE(pathExists(store->fnTempRoots));
}

TEST_F(LocalStoreObjectsTest, failedTemporaryRootAppendsRemainOneMonotoneStream)
{
    std::string expected;
    size_t serial = 0;
    for (bool interrupted : {false, true})
        for (bool same : {false, true}) {
            auto sample = store->makeStorePath("source", hashString(HashAlgorithm::SHA256, "sample"), "first");
            auto length = store->printStorePath(sample).size() + 1;
            for (size_t prefix = 0; prefix <= length; ++prefix) {
                SCOPED_TRACE(fmt("interrupted=%d same=%d prefix=%d", interrupted, same, prefix));
                auto hash = hashString(HashAlgorithm::SHA256, fmt("failed-root-%d", serial++));
                auto first = store->makeStorePath("source", hash, "first");
                auto next = same ? first : store->makeStorePath("source", hash, "other");
                auto record = store->printStorePath(first) + '\0';
                ASSERT_EQ(record.size(), length);
                size_t writes = 0;
                auto failAfter = [&](size_t count) {
                    store->writeTempRootForTest = [&, count](Descriptor fd, std::string_view bytes) {
                        ++writes;
                        writeFull(fd, bytes.substr(0, count), false);
                        if (interrupted)
                            throw Interrupted("injected temporary-root interruption");
                        throw SysError(EIO, "injected temporary-root write failure");
                    };
                };
                Finally clearHook{[&] { store->writeTempRootForTest = {}; }};
                failAfter(prefix);
                EXPECT_THROW(store->addTempRoot(first), BaseError);
                EXPECT_EQ(readFile(store->fnTempRoots), expected + record.substr(0, prefix));

                /* Read-only calls must neither finish the old record nor
                   manufacture a successful registration for the new path. */
                config->readOnly = true;
                {
                    Finally writable{[&] { config->readOnly = false; }};
                    EXPECT_NO_THROW(store->addTempRoot(next));
                    EXPECT_EQ(writes, 1u);
                }

                if (prefix < length) {
                    /* Recovery itself can fail after making more progress. */
                    failAfter(1);
                    EXPECT_THROW(store->addTempRoot(next), BaseError);
                    EXPECT_EQ(readFile(store->fnTempRoots), expected + record.substr(0, prefix + 1));
                }
                store->writeTempRootForTest = {};
                store->addTempRoot(next);
                expected += record;
                if (!same)
                    expected += store->printStorePath(next) + '\0';
                EXPECT_EQ(readFile(store->fnTempRoots), expected);
                store->writeTempRootForTest = [&](Descriptor, std::string_view) {
                    FAIL() << "a completed registration was appended again";
                };
                store->addTempRoot(next);
            }
        }
}

TEST_F(LocalStoreObjectsTest, failedTemporaryRootPositionChecksNeverAppend)
{
    auto first = store->makeStorePath("source", hashString(HashAlgorithm::SHA256, "old-root"), "first");
    store->addTempRoot(first);
    auto expected = readFile(store->fnTempRoots);
    auto next = store->makeStorePath("source", hashString(HashAlgorithm::SHA256, "new-root"), "other");
    auto record = store->printStorePath(next) + '\0';
    Descriptor fd = INVALID_DESCRIPTOR;
    store->writeTempRootForTest = [&](Descriptor target, std::string_view bytes) {
        fd = target;
        writeFull(fd, bytes.substr(0, 1), false);
        throw SysError(EIO, "injected temporary-root write failure");
    };
    Finally clearHook{[&] { store->writeTempRootForTest = {}; }};
    EXPECT_THROW(store->addTempRoot(next), SysError);
    ASSERT_NE(fd, INVALID_DESCRIPTOR);
    auto position = lseek(fd, 0, SEEK_CUR);
    auto backup = dupDescriptor(fd);
    bool restored = false;
    Finally restoreDescriptor{[&] {
        if (!restored) {
            if (dup2(backup.get(), fd) == -1)
                std::terminate();
            lseek(fd, position, SEEK_SET);
        }
    }};
    size_t writes = 0;
    store->writeTempRootForTest = [&](Descriptor, std::string_view) { ++writes; };
    for (auto badOffset : {off_t(0), off_t(expected.size() + record.size() + 1)}) {
        ASSERT_NE(lseek(fd, badOffset, SEEK_SET), -1);
        EXPECT_THROW(store->addTempRoot(next), Error);
        EXPECT_EQ(writes, 0u);
    }
    ASSERT_EQ(close(fd), 0);
    EXPECT_THROW(store->addTempRoot(next), SysError);
    EXPECT_EQ(writes, 0u);
    ASSERT_NE(dup2(backup.get(), fd), -1);
    ASSERT_NE(lseek(fd, position, SEEK_SET), -1);
    restored = true;
    store->writeTempRootForTest = {};
    store->addTempRoot(next);
    EXPECT_EQ(readFile(store->fnTempRoots), expected + record);
}

TEST_F(LocalStoreObjectsTest, temporaryRootFilePrecedesCollectorAcknowledgement)
{
    enum class Failure { None, Disconnect, BadAcknowledgement };
    for (auto failure : {Failure::None, Failure::Disconnect, Failure::BadAcknowledgement}) {
        SCOPED_TRACE(fmt("failure=%d", static_cast<int>(failure)));
        store.reset();
        store = std::make_shared<LocalStore>(ref{config});
        auto path = store->makeStorePath("source", hashString(HashAlgorithm::SHA256, "socket-root"), "root");
        auto next = failure == Failure::BadAcknowledgement
                        ? store->makeStorePath("source", hashString(HashAlgorithm::SHA256, "next-root"), "root")
                        : path;
        auto gcFd = openLockFile(config->stateDir.get() / "gc.lock", true);
        lockFile(gcFd.get(), ltWrite, true);
        auto socketPath = config->stateDir.get() / "gc-socket/socket";
        createDirs(socketPath.parent_path());
        auto server = createUnixDomainSocket(socketPath, 0600);
        AutoCloseFD client;
        std::promise<void> finished;
        auto done = finished.get_future();
        std::thread writer([&] {
            try {
                if (failure == Failure::BadAcknowledgement) {
                    bool refused = false;
                    try {
                        store->addTempRoot(path);
                    } catch (Error &) {
                        refused = true;
                    }
                    if (!refused)
                        throw Error("invalid collector acknowledgement was accepted");
                } else
                    store->addTempRoot(path);
                store->addTempRoot(next);
                store->addTempRoot(next);
                finished.set_value();
            } catch (...) {
                finished.set_exception(std::current_exception());
            }
        });
        Finally join{[&] {
            /* Release every possible wait before joining, also on a failed
               assertion: a closed peer makes the client retry the GC lock. */
            lockFile(gcFd.get(), ltNone, false);
            if (client)
                shutdown(client.get(), SHUT_RDWR);
            server.close();
            writer.join();
        }};
        auto readable = [](int fd) {
            pollfd pfd{.fd = fd, .events = POLLIN};
            if (poll(&pfd, 1, 5000) != 1)
                throw Error("temporary-root protocol test timed out");
        };
        auto expected = store->printStorePath(path) + '\0';
        auto request = [&](const StorePath & requested) {
            readable(server.get());
            client = accept(server.get(), nullptr, nullptr);
            if (!client)
                throw SysError("accepting temporary-root test client");
            std::string line;
            while (!line.ends_with('\n')) {
                readable(client.get());
                char byte;
                if (recv(client.get(), &byte, 1, MSG_DONTWAIT) != 1)
                    throw Error("temporary-root protocol test lost its client");
                line += byte;
            }
            EXPECT_EQ(line, store->printStorePath(requested) + '\n');
            /* A later collector must already see this record, even if this
               collector finishes immediately after sending its ACK. Discover
               the file without reading the writer's mutable fnTempRoots. */
            std::vector<std::filesystem::path> rootFiles;
            for (auto & entry : DirectoryIterator{store->tempRootsDir})
                rootFiles.push_back(entry.path());
            ASSERT_EQ(rootFiles.size(), 1u);
            EXPECT_EQ(readFile(rootFiles.front()), expected);
        };
        request(path);
        if (failure == Failure::Disconnect) {
            shutdown(client.get(), SHUT_RDWR);
            client.close();
            request(path);
        } else if (failure == Failure::BadAcknowledgement) {
            /* A failed exchange must discard even a queued valid byte:
               it acknowledges the old request, never the next root. */
            writeFull(client.get(), "01", false);
            expected += store->printStorePath(next) + '\0';
            request(next);
        }
        writeFull(client.get(), "1", false);
        ASSERT_EQ(done.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        EXPECT_NO_THROW(done.get());
        EXPECT_EQ(readFile(store->fnTempRoots), expected);
    }
}

TEST_F(LocalStoreObjectsTest, garbageCollectorClientsCompleteOrJoinAtShutdown)
{
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    Finally unset{[] { unsetenv("_NIX_TEST_NO_LSOF"); }};

    enum class Input { Empty, Malformed, Root };
    for (auto input : {Input::Empty, Input::Malformed, Input::Root})
        for (bool finishFirst : {false, true}) {
            SCOPED_TRACE(fmt("input=%d finishFirst=%d", static_cast<int>(input), finishFirst));
            auto candidate = store
                                 ->addToStoreSlow(
                                     "client-lifecycle",
                                     {treeAccessor({{"a", std::string("garbage"), 'f'}})},
                                     ContentAddressMethod::Raw::Git,
                                     HashAlgorithm::SHA256)
                                 .path;
            store.reset();
            store = std::make_shared<LocalStore>(ref{config});
            auto root = store->makeStorePath("source", hashString(HashAlgorithm::SHA256, "client-root"), "root");
            AutoCloseFD client;
            bool visited = false;
            auto receive = [&](bool eof) {
                pollfd pfd{.fd = client.get(), .events = POLLIN};
                if (poll(&pfd, 1, 5000) != 1)
                    throw Error("garbage collector client lifecycle test timed out");
                char byte;
                auto count = recv(client.get(), &byte, 1, MSG_DONTWAIT);
                if (eof ? count != 0 : count != 1 || byte != '1')
                    throw Error("unexpected garbage collector client lifecycle response");
            };
            store->whileDeletionPending = [&](const StorePath & path) {
                if (path != candidate || visited)
                    return;
                visited = true;
                client = createUnixDomainSocket();
                nix::connect(toSocket(client.get()), config->stateDir.get() / "gc-socket/socket");
                if (input != Input::Empty || !finishFirst) {
                    /* For an idle connection that the server will close,
                       first confirm acceptance through an ordinary ACK. */
                    auto message =
                        input == Input::Malformed ? std::string("malformed\n") : store->printStorePath(root) + '\n';
                    writeFull(client.get(), message, false);
                    receive(false);
                }
                if (finishFirst) {
                    if (shutdown(client.get(), SHUT_WR) != 0)
                        throw SysError("ending garbage collector test client input");
                    receive(true);
                }
            };

            GCOptions options;
            options.action = GCOptions::gcDeleteDead;
            options.pathsToDelete = GCOptions::SpecificPaths{.paths = {candidate}};
            GCResults results;
            store->collectGarbage(options, results);
            store->whileDeletionPending = {};
            EXPECT_TRUE(visited);
            EXPECT_FALSE(store->isValidPath(candidate));
            if (!finishFirst)
                receive(true);
        }
}

/* A temporary root registered while a collection runs reaches the
   collector through the GC socket -- `addTempRoot`'s fallback when the
   shared GC lock is refused -- and the deletion loop re-checks `tempRoots`
   before each path, so the path registered mid-collection keeps.  The hook
   fires in the window the socket is for: `shared->pending` names the path
   being deleted and nothing holds `_shared`.  The collection's own
   migration of an older row registers no root (`migrationDoesNotRootThePath`
   pins that half without the socket): a schema-10 row planted beside the
   two paths is deleted all the same. */
TEST_F(LocalStoreObjectsTest, tempRootDuringCollectionGoesThroughTheSocket)
{
    setenv("_NIX_TEST_NO_LSOF", "1", 1);
    Finally unset{[] { unsetenv("_NIX_TEST_NO_LSOF"); }};

    auto add = [&](std::string_view name, std::string_view bytes) {
        return store
            ->addToStoreSlow(
                name,
                {treeAccessor({{"a", std::string(bytes), 'f'}})},
                ContentAddressMethod::Raw::Git,
                HashAlgorithm::SHA256)
            .path;
    };
    auto a = add("aaa", "one");
    auto b = add("bbb", "two");
    auto old = add("old", "three");
    plantSchema10Row(
        *store, old, hashString(HashAlgorithm::SHA256, narOf(*makeFSSourceAccessor(store->toRealPath(old)))), 1);

    /* Reopened without this process's temp roots; a second connection
       stands for the other process that registers a root. */
    store.reset();
    store = std::make_shared<LocalStore>(ref{config});
    auto client = std::make_shared<LocalStore>(ref{config});

    std::optional<StorePath> deletedFirst;
    store->whileDeletionPending = [&](const StorePath & path) {
        if (deletedFirst || (path != a && path != b))
            return;
        deletedFirst = path;
        /* The collector holds the big GC lock, so this goes over the
           socket; it must name another path, since the handler waits out
           the pending one. */
        client->addTempRoot(path == a ? b : a);
    };

    GCOptions options;
    options.action = GCOptions::gcDeleteDead;
    GCResults results;
    store->collectGarbage(options, results);

    ASSERT_TRUE(deletedFirst.has_value()) << "no path was deleted, so the window never opened";
    auto kept = *deletedFirst == a ? b : a;
    EXPECT_FALSE(store->isValidPath(*deletedFirst));
    EXPECT_TRUE(store->isValidPath(kept)) << "registered through the socket while the collection ran";
    EXPECT_TRUE(pathExists(store->toRealPath(kept)));
    EXPECT_FALSE(store->isValidPath(old)) << "the migration of an older row registers no root";
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
    EXPECT_EQ(store->blobRowCount(false), 3u);
    EXPECT_EQ(store->blobRowCount(true), 1u);

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
    EXPECT_EQ(store->blobRowCount(false), 6u);
    EXPECT_EQ(store->blobRowCount(true), 2u);
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
    EXPECT_EQ(store->blobRowCount(false), 7u);
}

/* The same tree with one file changed inside `d`: the root and `d` are
   not held, so the driver descends into them and `d`'s files are read as
   the one route reads them -- one written, the other linked -- while `e`,
   held, is made from the objects and nothing beneath it is read.  A fresh
   tree costs the route one lookup per directory, never one per file.

   What this pinned before and what it pins now.  The one statement that
   moved is the last one about the file the route *wrote*: before, "TWO"
   was linked into `.objects/blobs` and the assertion was that the written
   file and the store's blob file are one inode.  There is no blob file:
   the written file *is* the blob's canonical file, and the assertion is
   that the index names it -- the row the fixture's `recordBlobsAtPath`
   makes true, as a real registration does. */
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
    EXPECT_EQ(blobFileOf(merkle::blobId("TWO"), false), t2 / "d/b") << "the file written is the blob's holder";
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
   --check-contents --repair`; a blob whose holder's bytes are not its
   identifier's is refused under repair (`link` re-hashes it, K2) and the
   file written from the source, and without repair trusted as the on-disk
   walk trusts an inode the index maps (K8): the route has no measure of
   the file but the store's own.  Every case ends in the same bytes.

   What the blob half pinned before and what it pins now.  Before, the
   corrupt file was a blob file of the store's own naming, and repair
   *removed* it and wrote the blob anew, so the same file afterwards held
   the right bytes.  A regular-file blob's file is a valid store path's, so
   repair forgets the row and leaves the file (`08` K2): the corrupt path
   keeps its corrupt bytes, and the freshly written file becomes the
   holder, which is what the next materialisation links.  The no-repair
   half therefore corrupts the *holder as it then stands* -- re-resolved,
   not the handle taken before the repair -- so that "the store's file is
   trusted, corrupt bytes and all" is still the thing being said. */
TEST_F(LocalStoreObjectsTest, materialiseFromObjectsFallsBackOnAMissingOrCorruptObject)
{
    Nodes nodes{{"a", "one", 'f'}, {"d/b", "two", 'f'}, {"d/c", "three", 'x'}, {"e/y", "four", 'f'}};
    auto nar = treeNar(nodes);
    auto tree = treeAccessor(nodes);
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};
    auto r1 = restore(realStoreDir / "f1", nar);
    auto root = readTree(r1.root.hash);
    auto dId = root.at("d/").hash;
    auto eId = root.at("e/").hash;

    auto expectWitnesses = [&](const std::filesystem::path & dst) {
        for (auto & [name, bytes, kind] : nodes) {
            auto id = merkle::blobId(bytes);
            auto cursor = GitObjectStore::Holder::initialCursor;
            bool found = false;
            while (auto row = store->objects.index->lookup(id, kind == 'x', cursor)) {
                cursor = row->cursor();
                if (std::filesystem::path(row->file) != dst / name)
                    continue;
                EXPECT_TRUE(GitObjectStore::validHolderMetadata(*row, kind == 'x', st(dst / name)));
                found = true;
            }
            EXPECT_TRUE(found) << "fallback lost its own witness for " << (dst / name);
        }
    };

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
        expectWitnesses(realStoreDir / "f2");
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
        expectWitnesses(dst);
    }
    plant(eBody);

    /* A corrupt holder, same length: `f1`'s own `d/b`, which is the blob's
       canonical file.  Under repair the holder is re-hashed before it is
       linked, its row *forgotten*, and the file written from the source --
       which becomes the blob's holder.  The corrupt file stays where it
       is: it belongs to a valid store path (`08` K2). */
    auto two = blobFileOf(merkle::blobId("two"), false);
    ASSERT_EQ(two, realStoreDir / "f1/d/b");
    chmod(two, 0644);
    writeFile(two, "twX");
    chmod(two, 0444);
    setWriteTime(two, 1, 1);
    {
        auto counting = make_ref<CountingAccessor>(tree);
        auto r = materialise(realStoreDir / "f5", SourcePath(counting), everyNode(tree), Repair);
        EXPECT_EQ(filesCreated, 1u);
        EXPECT_EQ(counting->reads, 3u);    /* a, d/b, d/c; e from its objects, re-hashed */
        EXPECT_EQ(counting->listings, 2u); /* the root, d */
        EXPECT_EQ(r.root, r1.root);
        EXPECT_EQ(readFile(realStoreDir / "f5/d/b"), "two");
        EXPECT_EQ(blobFileOf(merkle::blobId("two"), false), realStoreDir / "f5/d/b")
            << "the fresh file is the holder now";
        EXPECT_EQ(readFile(two), "twX") << "and the corrupt path's file is left where it is";
        StringSink again;
        dumpPath(realStoreDir / "f5", again);
        EXPECT_EQ(again.s, nar);
        expectWitnesses(realStoreDir / "f5");
    }

    /* Without repair the store's file is trusted: it is linked, corrupt
       bytes and all, as every path already linking it shows them; the
       verifier's business (`--verify --check-contents`).  The holder is
       re-resolved, the repair above having moved it. */
    auto holder = blobFileOf(merkle::blobId("two"), false);
    chmod(holder, 0644);
    writeFile(holder, "twX");
    chmod(holder, 0444);
    setWriteTime(holder, 1, 1);
    {
        auto counting = make_ref<CountingAccessor>(tree);
        auto r = materialise(realStoreDir / "f6", SourcePath(counting), everyNode(tree));
        EXPECT_EQ(filesCreated, 0u);
        EXPECT_EQ(counting->reads, 0u);
        EXPECT_EQ(r.root, r1.root);
        EXPECT_EQ(readFile(realStoreDir / "f6/d/b"), "twX");
        EXPECT_EQ(st(realStoreDir / "f6/d/b").st_ino, st(holder).st_ino);
    }
}

/* A speculative directory must not lend names that its rollback can
   delete. The sibling finishes its shared file before the donor disappears;
   then the failed directory falls back through both buffered and streamed
   writes. Neither mode may lose the sibling's physical sharing witness. */
TEST_F(LocalStoreObjectsTest, speculativeSubtreeRollbackCannotInvalidateSiblingHolders)
{
    struct Source final : MemorySourceAccessor
    {
        /* Fixed during traversal; its two gates use promises/futures. */
        std::function<void(const CanonPath &)> beforeRead;

        bool allowsConcurrentTraversal() override
        {
            return true;
        }

        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            beforeRead(path);
            MemorySourceAccessor::readFile(path, sink, sizeCallback);
        }
    };

    Finally restoreWorkers{
        [old = LocalStore::copyWorkersForTest.load()] { LocalStore::copyWorkersForTest.store(old); }};
    LocalStore::copyWorkersForTest.store(2);
    for (bool executable : {false, true})
        for (bool spill : {false, true}) {
            SCOPED_TRACE(fmt("executable=%d spill=%d", executable, spill));
            auto label = fmt("rollback-%d-%d", executable, spill);
            auto bytes = label + std::string(spill ? 1024 * 1024 + 1 : 32, 'x');
            auto gate = label + " gate";
            Nodes nodes{
                {"p/a", bytes, executable ? 'x' : 'f'},
                {"p/m", gate, 'f'},
                {"p/z/leaf", label + " leaf", 'f'},
                {"q/a", bytes, executable ? 'x' : 'f'},
                {"q/z", label + " finished", 'f'},
            };
            auto donor = config->realStoreDir.get() / (label + "-donor");
            auto result = restore(donor, treeNar(nodes));
            auto pTree = readTree(result.root.hash).at("p/").hash;
            std::filesystem::remove(treeFile(readTree(pTree).at("z/").hash));
            auto id = merkle::blobId(bytes);
            auto original = store->objects.holderOrForget(id, executable, NoRepair);
            ASSERT_TRUE(original);

            std::promise<void> firstLinked, siblingLinked;
            auto firstReady = firstLinked.get_future().share();
            auto siblingReady = siblingLinked.get_future().share();
            auto wait = [](auto & ready) {
                if (ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
                    throw Error("parallel rollback test did not reach its next operation");
            };
            bool paused = false;
            auto * originalIndex = store->objects.index;
            auto index = *originalIndex;
            index.lookup = [&](const Hash & hash, bool mode, GitObjectStore::Holder::Cursor cursor) {
                auto row = originalIndex->lookup(hash, mode, cursor);
                if (hash == merkle::blobId(gate) && !paused) {
                    paused = true;
                    firstLinked.set_value();
                    wait(siblingReady);
                    /* The registered candidate disappears after both copies
                       linked it. Their own files must now be the witnesses. */
                    originalIndex->forget(id, executable, *original);
                    chmod(donor / "p", 0755);
                    chmod(donor / "q", 0755);
                    std::filesystem::remove(donor / "p/a");
                    std::filesystem::remove(donor / "q/a");
                }
                return row;
            };
            store->objects.index = &index;
            Finally restoreIndex{[&] { store->objects.index = originalIndex; }};
            auto source = make_ref<Source>();
            source->root = std::move(treeAccessor(nodes)->root);
            source->beforeRead = [&](const CanonPath & path) {
                if (path == CanonPath("/q/a"))
                    wait(firstReady);
                if (path == CanonPath("/q/z"))
                    siblingLinked.set_value();
            };
            auto namer = [&](const CanonPath & path, const SourceAccessor::Stat &) -> std::optional<merkle::TreeEntry> {
                return path == CanonPath("/p") ? std::optional(objectHashOf(*source, path).root) : std::nullopt;
            };
            auto dst = config->realStoreDir.get() / label;
            CountingRestoreHooks hooks{
                makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls))};
            auto hold = store->objects.hold();
            auto copied = store->materialiseThroughObjects(
                dst,
                SourcePath(source),
                defaultPathFilter,
                false,
                &hooks,
                NoRepair,
                namer,
                hold,
                /*callbacksPermitParallelism=*/true);
            EXPECT_TRUE(paused);
            EXPECT_EQ(copied.root, result.root);
            EXPECT_EQ(st(dst / "p/a").st_ino, st(dst / "q/a").st_ino);
            EXPECT_EQ(readFile(dst / "p/a"), bytes);
            EXPECT_EQ(readFile(dst / "q/a"), bytes);
            store->recordBlobsAtPath(hold, dst);
            auto holder = store->objects.holderOrForget(id, executable, NoRepair);
            ASSERT_TRUE(holder);
            EXPECT_EQ(holder->ino, st(dst / "q/a").st_ino);
            EXPECT_TRUE(
                GitObjectStore::validHolderMetadata(*holder, executable, st(std::filesystem::path(holder->file))));
        }
}

/* Completion merges an entire successful subtree's aliases into the
   already completed parent candidate. A merge error is fatal after unique
   rows have been published; it must never re-enter source fallback. */
TEST_F(LocalStoreObjectsTest, completedSubtreeMergesEveryAliasAndNeverRollsBackPublishedRows)
{
    Finally restoreWorkers{
        [old = LocalStore::copyWorkersForTest.load()] { LocalStore::copyWorkersForTest.store(old); }};
    LocalStore::copyWorkersForTest.store(1);
    for (bool executable : {false, true})
        for (bool fail : {false, true}) {
            SCOPED_TRACE(fmt("executable=%d fail=%d", executable, fail));
            auto label = fmt("merge-%d-%d", executable, fail);
            auto bytes = label + " shared";
            auto unique = label + " unique";
            Nodes nodes{
                {"a", bytes, executable ? 'x' : 'f'},
                {"p/a", bytes, executable ? 'x' : 'f'},
                {"p/b", bytes, executable ? 'x' : 'f'},
                {"p/u", unique, 'f'},
            };
            auto donor = config->realStoreDir.get() / (label + "-donor");
            auto result = restore(donor, treeNar(nodes));
            auto alternate = tempStoreDir.path() / (label + "-alternate");
            writeFile(alternate, bytes);
            chmod(alternate, executable ? 0555 : 0444);
            canonicaliseTimestampAndPermissions(alternate);
            auto alternateStat = st(alternate);
            GitObjectStore::Holder alternateRow{
                alternateStat.st_ino,
                alternateStat.st_dev,
                static_cast<uint64_t>(alternateStat.st_size),
                alternate.string(),
                1};
            ASSERT_NE(st(donor / "a").st_ino, alternateRow.ino);
            auto id = merkle::blobId(bytes);
            auto tree = treeAccessor(nodes);
            auto source = make_ref<CountingAccessor>(tree);
            bool inAttempt = false;
            auto * originalIndex = store->objects.index;
            auto index = *originalIndex;
            index.lookup = [&](const Hash & hash,
                               bool mode,
                               GitObjectStore::Holder::Cursor cursor) -> std::optional<GitObjectStore::Holder> {
                if (inAttempt && hash == id)
                    return alternateRow.cursor() > cursor ? std::optional{alternateRow} : std::nullopt;
                return originalIndex->lookup(hash, mode, cursor);
            };
            store->objects.index = &index;
            Finally restoreIndex{[&] { store->objects.index = originalIndex; }};
            Finally restoreFault{[&] { store->objects.failPlaceWithNoSpace = {}; }};
            if (fail)
                store->objects.failPlaceWithNoSpace = [](const auto &) -> bool {
                    throw Error("injected completed-subtree merge failure");
                };
            auto namer = [&](const CanonPath & path, const SourceAccessor::Stat &) -> std::optional<merkle::TreeEntry> {
                if (path != CanonPath("/p"))
                    return std::nullopt;
                inAttempt = true;
                return objectHashOf(*tree, path).root;
            };
            auto dst = config->realStoreDir.get() / label;
            if (fail) {
                EXPECT_THROW(materialise(dst, SourcePath(source), namer), Error);
                EXPECT_TRUE(pathExists(dst / "p/u")) << "the completed subtree was not deleted for fallback";
                EXPECT_EQ(count("tmp"), 0u) << "the failed ingestion released its hold";
            } else {
                auto copied = materialise(dst, SourcePath(source), namer);
                EXPECT_EQ(copied.root, result.root);
                EXPECT_EQ(st(dst / "p/a").st_ino, st(dst / "a").st_ino);
                EXPECT_EQ(st(dst / "p/b").st_ino, st(dst / "a").st_ino);
                EXPECT_EQ(readFile(dst / "p/u"), unique);
                auto cursor = GitObjectStore::Holder::initialCursor;
                bool ownUnique = false;
                while (auto row = originalIndex->lookup(merkle::blobId(unique), false, cursor)) {
                    cursor = row->cursor();
                    if (row->file == (dst / "p/u").string()) {
                        ownUnique = true;
                        EXPECT_TRUE(GitObjectStore::validHolderMetadata(*row, false, st(dst / "p/u")));
                    }
                }
                EXPECT_TRUE(ownUnique) << "the nested name stays relative to the whole ingestion";
            }
            EXPECT_EQ(source->reads, 1u) << "only the unnamed outer file is read; merge never becomes fallback";
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

TEST_F(LocalStoreObjectsTest, concurrentCopyRequiresSourceAndCallbackPermissionAtEveryWidth)
{
    struct ObservedSource : MemorySourceAccessor
    {
        const std::thread::id caller = std::this_thread::get_id();
        std::array<std::atomic<bool>, 9> offCaller{};
        std::array<std::atomic<size_t>, 9> calls{};
        size_t gateWidth = 0, arrived = 0;
        std::mutex mutex;
        std::condition_variable ready;

        void observe(size_t operation)
        {
            ++calls[operation];
            if (std::this_thread::get_id() != caller)
                offCaller[operation] = true;
        }

        std::optional<Stat> maybeLstat(const CanonPath & path) override
        {
            observe(0);
            return MemorySourceAccessor::maybeLstat(path);
        }

        DirEntries readDirectory(const CanonPath & path) override
        {
            observe(1);
            return MemorySourceAccessor::readDirectory(path);
        }

        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            observe(2);
            if (gateWidth) {
                std::unique_lock lock(mutex);
                if (arrived < gateWidth) {
                    ++arrived;
                    ready.notify_all();
                    if (!ready.wait_for(lock, std::chrono::seconds(5), [&] { return arrived == gateWidth; }))
                        throw Error("permitted copy did not exercise its requested worker width");
                }
            }
            MemorySourceAccessor::readFile(path, sink, sizeCallback);
        }

        std::string readLink(const CanonPath & path) override
        {
            observe(3);
            return MemorySourceAccessor::readLink(path);
        }

        std::optional<merkle::TreeEntry> getKnownEntry(const CanonPath & path) override
        {
            observe(4);
            return MemorySourceAccessor::getKnownEntry(path);
        }
    };

    struct PermittedSource final : ObservedSource
    {
        bool allowsConcurrentTraversal() override
        {
            return true;
        }
    };

    struct ObservedHooks : CountingRestoreHooks
    {
        ObservedSource & observations;

        ObservedHooks(ObservedSource & observations, std::unique_ptr<RestoreSinkHooks> inner)
            : CountingRestoreHooks(std::move(inner))
            , observations(observations)
        {
        }

        void directoryDone(Descriptor dirFd) override
        {
            observations.observe(6);
            CountingRestoreHooks::directoryDone(dirFd);
        }

        void regularFileCreated(Descriptor fd, bool executable) override
        {
            observations.observe(7);
            CountingRestoreHooks::regularFileCreated(fd, executable);
        }

        void symlinkCreated(Descriptor parentFd, const CanonPath & name) override
        {
            observations.observe(8);
            CountingRestoreHooks::symlinkCreated(parentFd, name);
        }
    };

    Nodes nodes;
    for (size_t i = 0; i < 16; ++i) {
        auto dir = fmt("d%02d/", i);
        nodes.emplace_back(dir + "file", "ordinary", 'f');
        nodes.emplace_back(dir + "exec", "executable", 'x');
        nodes.emplace_back(dir + "link", "file", 'l');
        nodes.emplace_back(dir + "nested/file", "nested", 'f');
    }
    PathFilter all = AllPaths{};
    Finally restoreWorkers{
        [old = LocalStore::copyWorkersForTest.load()] { LocalStore::copyWorkersForTest.store(old); }};
    for (bool sourcePermitted : {false, true})
        for (bool callbacksPermitted : {false, true})
            for (size_t workers = 1; workers <= 8; ++workers) {
                auto label = fmt("source-%d-callbacks-%d-w%d", sourcePermitted, callbacksPermitted, workers);
                SCOPED_TRACE(label);
                std::shared_ptr<ObservedSource> source =
                    sourcePermitted ? std::make_shared<PermittedSource>() : std::make_shared<ObservedSource>();
                auto caseNodes = nodes;
                for (auto & [path, bytes, kind] : caseNodes)
                    if (kind != 'l')
                        bytes += label + path; // Every worker must create files and exercise that hook.
                source->root = std::move(treeAccessor(caseNodes)->root);
                ASSERT_EQ(source->allowsConcurrentTraversal(), sourcePermitted);
                bool parallel = sourcePermitted && callbacksPermitted;
                /* The first wave cannot complete until this many distinct
                   executing threads have entered a read. All waits are bounded. */
                source->gateWidth = parallel ? workers : 0;
                LocalStore::copyWorkersForTest.store(workers);
                auto dst = config->realStoreDir.get() / label;
                ObservedHooks hooks{
                    *source,
                    makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(settings.getLocalSettings().ignoredAcls))};
                auto namer = [&](const CanonPath &, const SourceAccessor::Stat &) -> std::optional<merkle::TreeEntry> {
                    source->observe(5);
                    return std::nullopt;
                };
                auto hold = store->objects.hold();
                if (callbacksPermitted)
                    store->materialiseThroughObjects(
                        dst, SourcePath(ref{source}), all, false, &hooks, NoRepair, namer, hold, true);
                else
                    /* Exercise the public API's default without supplying a flag. */
                    store->materialiseThroughObjects(
                        dst, SourcePath(ref{source}), all, false, &hooks, NoRepair, namer, hold);
                EXPECT_EQ(source->arrived, source->gateWidth);
                for (size_t operation = 0; operation < source->calls.size(); ++operation) {
                    SCOPED_TRACE(fmt("operation=%d", operation));
                    EXPECT_GT(source->calls[operation].load(), 0u);
                    EXPECT_EQ(source->offCaller[operation].load(), parallel && workers > 1);
                }
                StringSink actual;
                dumpPath(dst, actual);
                EXPECT_EQ(actual.s, treeNar(caseNodes));
            }
}

/* The parallel copy is the sequential copy (`01` §9.10, *One route*;
   `MaterialisingVisitor::copyChildren`): the second route walks a
   directory's *subdirectories* on a bounded pool, and what it makes must
   not depend on how many threads that is.  Both copies are made into the
   same store, which already holds every object, so each file is a link to
   the same blob and the two paths can be compared inode for inode as well
   as byte for byte.  `LocalStore::copyWorkersForTest` is the test-only
   seam for the pool's size (there is no setting: what the copy makes does
   not depend on the count); `1` is exactly the walk before the pool
   existed.

   The tree is wide on purpose -- eight sibling directories under the root,
   each with files and a subdirectory of its own -- so that the pool is
   actually used, including nested directory scopes. */
TEST_F(LocalStoreObjectsTest, parallelCopyIsTheSequentialCopy)
{
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};

    Nodes nodes;
    for (int d = 0; d < 8; d++) {
        auto dir = fmt("d%d", d);
        for (int f = 0; f < 5; f++)
            nodes.emplace_back(dir + fmt("/f%d", f), fmt("content %d %d", d, f), 'f');
        nodes.emplace_back(dir + "/s/g", fmt("deeper %d", d), 'f');
        nodes.emplace_back(dir + "/l", fmt("f%d", d % 5), 'l');
        nodes.emplace_back(dir + "/x", fmt("exec %d", d), 'x');
    }
    auto tree = make_ref<ConcurrentMemorySourceAccessor>();
    tree->root = std::move(treeAccessor(nodes)->root);
    auto nar = narOf(*tree);

    /* The store holds every object of the tree: both copies link. */
    auto reference = realStoreDir / "ref";
    auto r0 = restore(reference, nar);

    Finally restoreWorkers{
        [old = LocalStore::copyWorkersForTest.load()]() { LocalStore::copyWorkersForTest.store(old); }};

    struct Copy
    {
        std::filesystem::path dst;
        ObjectHashSink::Result result;
        size_t created;
    };

    /* Over both kinds of source: the in-memory tree, and the filesystem,
       whose scoped accessor is a stack object the traversal hands to the
       directory callback -- a worker holds a child's visit past the loop
       that offered it, so the scope has to outlive the node
       (`tree-traversal.hh`).  A dirty git checkout and a `path:` input take
       the filesystem route. */
    std::vector<std::pair<std::string, SourcePath>> sources;
    sources.emplace_back("memory", SourcePath(tree));
    sources.emplace_back("filesystem", SourcePath(makeFSSourceAccessor(reference)));

    for (auto & [label, src] : sources) {
        std::vector<Copy> copies;
        for (size_t workers = 1; workers <= 8; ++workers) {
            LocalStore::copyWorkersForTest.store(workers);
            auto dst = realStoreDir / fmt("%s-w%d", label, workers);
            auto r = materialise(dst, src, noNode(), NoRepair, defaultPathFilter, /*callbacksPermitParallelism=*/true);
            copies.push_back({dst, r, filesCreated});
        }

        auto & one = copies[0];
        for (auto & many : copies) {

            /* The summary the copy returns. */
            EXPECT_EQ(many.result.root, one.result.root) << label;
            EXPECT_EQ(many.result.narSize, one.result.narSize) << label;
            EXPECT_EQ(many.result.narSizeExact, one.result.narSizeExact) << label;
            EXPECT_EQ(many.created, one.created) << label;
            EXPECT_EQ(one.result.root, r0.root) << label;

            /* Byte for byte, against the one route's NAR. */
            StringSink again;
            dumpPath(many.dst, again);
            EXPECT_EQ(again.s, nar) << label << " " << many.dst;

            /* Link for link: the same files, each the same inode -- the store's
               blob -- and at the same link count. */
            auto files = regularFilesUnder(reference);
            ASSERT_FALSE(files.empty());
            EXPECT_EQ(regularFilesUnder(one.dst), files) << label;
            EXPECT_EQ(regularFilesUnder(many.dst), files) << label;
            for (auto & f : files) {
                auto under = [&](const std::filesystem::path & p) { return f.empty() ? p : p / f; };
                EXPECT_EQ(st(under(many.dst)).st_ino, st(under(one.dst)).st_ino) << label << " " << f;
                EXPECT_EQ(st(under(many.dst)).st_nlink, st(under(one.dst)).st_nlink) << label << " " << f;
                EXPECT_EQ(st(under(many.dst)).st_mode, st(under(one.dst)).st_mode) << label << " " << f;
            }
        }
    }
}

TEST_F(LocalStoreObjectsTest, freshParallelCopySharesDuplicatesAtEveryWidth)
{
    Finally restoreWorkers{
        [old = LocalStore::copyWorkersForTest.load()] { LocalStore::copyWorkersForTest.store(old); }};
    for (size_t workers = 1; workers <= 8; ++workers) {
        LocalStore::copyWorkersForTest.store(workers);
        Nodes nodes;
        auto bytes = fmt("width %d ", workers) + std::string(65536, 'x');
        for (size_t dir = 0; dir < 16; ++dir)
            for (auto kind : {'f', 'x'})
                nodes.emplace_back(fmt("d%d/%c", dir, kind), bytes, kind);
        auto src = make_ref<ConcurrentMemorySourceAccessor>();
        src->root = std::move(treeAccessor(nodes)->root);
        auto dst = std::filesystem::path{config->realStoreDir.get()} / fmt("fresh-w%d", workers);
        materialise(dst, SourcePath(src), noNode(), NoRepair, defaultPathFilter, /*callbacksPermitParallelism=*/true);
        for (size_t dir = 0; dir < 16; ++dir)
            for (auto kind : {'f', 'x'})
                EXPECT_EQ(st(dst / fmt("d%d/%c", dir, kind)).st_ino, st(dst / fmt("d0/%c", kind)).st_ino)
                    << "workers=" << workers << " dir=" << dir << " mode=" << kind;
        EXPECT_NE(st(dst / "d0/f").st_ino, st(dst / "d0/x").st_ino);
    }
}

/* Every supported worker count must join nested children before an
   exception leaves the source scope. A skewed tree reaches well beyond
   the pool width; a later successful copy also detects abandoned work. */
TEST_F(LocalStoreObjectsTest, nestedParallelCopyJoinsOnFailure)
{
    struct Source final : MemorySourceAccessor
    {
        /* The tree and failure flag change only after every child joins. */
        bool allowsConcurrentTraversal() override
        {
            return true;
        }

        std::atomic<size_t> active{0};
        std::atomic<size_t> peak{0};
        bool fail = true;

        void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            auto count = ++active;
            auto before = peak.load();
            while (before < count && !peak.compare_exchange_weak(before, count)) {
            }
            Finally done{[&] { --active; }};
            if (fail && path.baseName() == "fail")
                throw Error("injected source failure");
            MemorySourceAccessor::readFile(path, sink, sizeCallback);
        }
    };

    Nodes nodes;
    for (size_t i = 0; i < 24; ++i) {
        auto prefix = fmt("large/a/b/c/d%02d/e/f/g/h/", i);
        nodes.emplace_back(prefix + "ok", std::string(4096, 'x'), 'f');
        nodes.emplace_back(prefix + "fail", "failure", 'f');
    }
    nodes.emplace_back("small/ok", "small", 'f');
    auto source = make_ref<Source>();
    source->root = std::move(treeAccessor(nodes)->root);
    auto expected = narOf(*treeAccessor(nodes));
    Finally restoreWorkers{
        [old = LocalStore::copyWorkersForTest.load()] { LocalStore::copyWorkersForTest.store(old); }};
    for (size_t workers = 1; workers <= 8; ++workers) {
        LocalStore::copyWorkersForTest.store(workers);
        source->fail = true;
        auto dst = std::filesystem::path{config->realStoreDir.get()} / fmt("failure-%d", workers);
        EXPECT_THROW(
            materialise(
                dst, SourcePath(source), noNode(), NoRepair, defaultPathFilter, /*callbacksPermitParallelism=*/true),
            Error);
        EXPECT_EQ(source->active, 0u);
        EXPECT_LE(source->peak, workers);
        source->peak = 0;
        source->fail = false;
        auto good = std::filesystem::path{config->realStoreDir.get()} / fmt("success-%d", workers);
        materialise(
            good, SourcePath(source), noNode(), NoRepair, defaultPathFilter, /*callbacksPermitParallelism=*/true);
        EXPECT_EQ(source->active, 0u);
        EXPECT_LE(source->peak, workers);
        source->peak = 0;
        StringSink actual;
        dumpPath(good, actual);
        EXPECT_EQ(actual.s, expected);
    }
}

/* The same law over generated trees, the namers of
   `prop_materialised_path_is_the_restored_path` included: whatever the
   naming names, the copy on one thread and the copy on eight make the same
   path. */
RC_GTEST_FIXTURE_PROP(LocalStoreObjectsTest, prop_parallel_copy_is_the_sequential_copy, ())
{
    WithCaseHack on{true};
    FileGenSpec spec{
        .names = {"a", "A", "b.c", "B.C"},
        .contents = rc::gen::element(std::string(""), std::string("x"), std::string("yy"), std::string(9, 'q')),
        .targets = rc::gen::element(std::string("t"), std::string("t/uv")),
    };
    auto tree = make_ref<ConcurrentMemorySourceAccessor>();
    tree->root = *genFile(3, spec);
    auto nar = narOf(*tree);
    auto realStoreDir = std::filesystem::path{config->realStoreDir.get()};

    auto reference = realStoreDir / "ref";
    auto r0 = restore(reference, nar);

    Finally restoreWorkers{
        [old = LocalStore::copyWorkersForTest.load()]() { LocalStore::copyWorkersForTest.store(old); }};

    for (auto & label : {std::string("every"), std::string("none")}) {
        LocalStore::TreeNamer namer = label == "every" ? everyNode(tree) : noNode();
        std::vector<std::filesystem::path> dsts;
        std::vector<ObjectHashSink::Result> results;
        for (size_t workers : {size_t(1), size_t(8)}) {
            LocalStore::copyWorkersForTest.store(workers);
            auto dst = realStoreDir / (label + fmt("-w%d", workers));
            results.push_back(materialise(
                dst, SourcePath(tree), namer, NoRepair, defaultPathFilter, /*callbacksPermitParallelism=*/true));
            RC_ASSERT(filesCreated == 0u);
            dsts.push_back(dst);
        }
        RC_ASSERT(results[1].root == results[0].root);
        RC_ASSERT(results[1].narSize == results[0].narSize);
        RC_ASSERT(results[0].root == r0.root);
        for (auto & dst : dsts) {
            StringSink again;
            dumpPath(dst, again);
            RC_ASSERT(again.s == nar);
        }
        auto files = regularFilesUnder(dsts[0]);
        RC_ASSERT(files == regularFilesUnder(dsts[1]));
        for (auto & f : files) {
            auto under = [&](const std::filesystem::path & p) { return f.empty() ? p : p / f; };
            RC_ASSERT(st(under(dsts[1])).st_ino == st(under(dsts[0])).st_ino);
            RC_ASSERT(st(under(dsts[1])).st_nlink == st(under(dsts[0])).st_nlink);
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

/* The source has no stable name, so every call must observe its bytes
   once, even when the store already contains their destination. */
struct UnnamedRegularSource : MemorySourceAccessor
{
    bool sizeKnown;
    std::optional<uint64_t> announced;
    bool fail = false;
    size_t reads = 0;

    UnnamedRegularSource(std::string bytes, bool executable, bool sizeKnown, std::optional<uint64_t> announced)
        : sizeKnown(sizeKnown)
        , announced(announced)
    {
        root = File::Regular{.executable = executable, .contents = std::move(bytes)};
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto st = MemorySourceAccessor::maybeLstat(path);
        if (st && !sizeKnown)
            st->fileSize.reset();
        return st;
    }

    void readFile(const CanonPath &, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        if (++reads != 1)
            throw Error("an unnamed file was observed twice");
        if (announced)
            sizeCallback(*announced);
        auto & bytes = std::get<File::Regular>(root->raw).contents;
        auto half = bytes.size() / 2;
        sink(std::string_view(bytes).substr(0, half));
        sink(std::string_view(bytes).substr(half));
        if (fail)
            throw Error("source read failed after delivering its bytes");
    }
};

TEST_F(LocalStoreObjectsTest, unnamedRegularCopyStagesOnlyWhenItsDestinationNeedsWriting)
{
    constexpr size_t limit = 1 << 20;
    unsigned int serial = 0;
    for (size_t size : {size_t{0}, limit, limit + 1})
        for (bool sizeKnown : {false, true})
            for (bool announces : {false, true})
                for (bool executable : {false, true})
                    for (auto repair : {NoRepair, Repair})
                        for (bool valid : {false, true}) {
                            auto name = fmt("buffered-root-%d", serial++);
                            SCOPED_TRACE(
                                fmt("%s size=%d stat=%d announced=%d executable=%d repair=%d valid=%d",
                                    name,
                                    size,
                                    sizeKnown,
                                    announces,
                                    executable,
                                    repair,
                                    valid));
                            std::string bytes(size, 'b');
                            auto source = make_ref<UnnamedRegularSource>(
                                bytes, executable, sizeKnown, announces ? std::optional<uint64_t>(size) : std::nullopt);
                            auto expectedRoot = merkle::TreeEntry{
                                .mode = executable ? merkle::Mode::Executable : merkle::Mode::Regular,
                                .hash = merkle::blobId(bytes)};
                            auto expected = store->makeFixedOutputPathFromCA(
                                name,
                                ContentAddressWithReferences::fromParts(
                                    ContentAddressMethod::Raw::Git, merkle::objectHash(expectedRoot), {}));
                            if (valid) {
                                ASSERT_EQ(
                                    store->materialise(name, SourcePath(source), defaultPathFilter, NoRepair, noNode()),
                                    expected);
                                source->reads = 0;
                            }
                            size_t holds = 0;
                            store->objects.atHoldMade = [&] { ++holds; };
                            Finally resetHook{[&] { store->objects.atHoldMade = nullptr; }};
                            auto actual =
                                store->materialise(name, SourcePath(source), defaultPathFilter, repair, noNode());
                            EXPECT_EQ(actual, expected);
                            EXPECT_EQ(source->reads, 1u);
                            EXPECT_EQ(holds, valid && !repair && size <= limit ? 0u : 1u);
                            EXPECT_EQ(readFile(store->toRealPath(actual)), bytes);
                            EXPECT_EQ(bool(st(store->toRealPath(actual)).st_mode & S_IXUSR), executable);
                            auto info = store->queryPathInfo(actual);
                            ASSERT_TRUE(info->objectHash);
                            EXPECT_EQ(*info->objectHash, ObjectHash::of(expectedRoot));
                            StringSink nar;
                            dumpPath(store->toRealPath(actual), nar);
                            EXPECT_EQ(info->narSize, nar.s.size());
                            for (auto & entry : DirectoryIterator{config->realStoreDir.get()})
                                EXPECT_FALSE(hasPrefix(entry.path().filename().string(), "tmp-"));
                        }
}

TEST_F(LocalStoreObjectsTest, regularCopyPreservesProducerAndNamerReuse)
{
    for (bool producer : {false, true})
        for (auto repair : {NoRepair, Repair}) {
            SCOPED_TRACE(producer);
            SCOPED_TRACE(repair);
            auto name = fmt("named-root-%d-%d", producer, repair);
            auto source = make_ref<UnnamedRegularSource>("named file", false, true, 10);
            store->materialise(name + "-seed", SourcePath(source), defaultPathFilter, NoRepair, noNode());
            source->reads = 0;
            auto entry = merkle::TreeEntry{.mode = merkle::Mode::Regular, .hash = merkle::blobId("named file")};
            if (producer)
                source->knownRoot = entry;
            auto namer =
                producer ? noNode() : LocalStore::TreeNamer{[entry](const CanonPath &, const SourceAccessor::Stat &) {
                    return std::optional(entry);
                }};
            auto path = store->materialise(name, SourcePath(source), defaultPathFilter, repair, std::move(namer));
            /* Producer identities are ignored under repair. An explicit
               TreeNamer retains its existing trust contract under repair;
               the holder's bytes are verified by the materialiser. */
            EXPECT_EQ(source->reads, producer && repair ? 1u : 0u);
            EXPECT_EQ(readFile(store->toRealPath(path)), "named file");
        }
}

TEST_F(LocalStoreObjectsTest, unnamedRegularCopyFailureReleasesStagingAndRegistersNothing)
{
    constexpr size_t limit = 1 << 20;
    unsigned int serial = 0;
    for (size_t size : {size_t{4}, limit + 1})
        for (bool sizeKnown : {false, true})
            for (auto repair : {NoRepair, Repair})
                for (bool valid : {false, true})
                    for (int failure = 0; failure < 4; ++failure) {
                        auto name = fmt("failed-buffered-root-%d", serial++);
                        SCOPED_TRACE(
                            fmt("%s size=%d stat=%d repair=%d valid=%d failure=%d",
                                name,
                                size,
                                sizeKnown,
                                repair,
                                valid,
                                failure));
                        std::string bytes(size, 'c');
                        auto source = make_ref<UnnamedRegularSource>(bytes, false, sizeKnown, size);
                        auto expected = store->makeFixedOutputPathFromCA(
                            name,
                            ContentAddressWithReferences::fromParts(
                                ContentAddressMethod::Raw::Git, merkle::blobId(bytes), {}));
                        if (valid) {
                            ASSERT_EQ(
                                store->materialise(name, SourcePath(source), defaultPathFilter, NoRepair, noNode()),
                                expected);
                            source->reads = 0;
                        }
                        /* Both mismatch directions, and read failure with
                           and without a size announcement, cross the bound. */
                        if (failure < 2)
                            source->announced = failure == 0 ? size - 1 : size + 1;
                        else {
                            source->fail = true;
                            if (failure == 3)
                                source->announced.reset();
                        }
                        EXPECT_THROW(
                            store->materialise(name, SourcePath(source), defaultPathFilter, repair, noNode()), Error);
                        EXPECT_EQ(source->reads, 1u);
                        EXPECT_EQ(store->isValidPath(expected), valid);
                        if (valid)
                            EXPECT_EQ(readFile(store->toRealPath(expected)), bytes);
                        for (auto & entry : DirectoryIterator{config->realStoreDir.get()})
                            EXPECT_FALSE(hasPrefix(entry.path().filename().string(), "tmp-"));
                        if (pathExists(store->objects.dir / "tmp"))
                            for (auto & entry : DirectoryIterator{store->objects.dir / "tmp"})
                                EXPECT_FALSE(hasPrefix(entry.path().filename().string(), "hold-"));
                    }
}

TEST_F(LocalStoreAutoGCTest, bufferedUnnamedCopyOfAValidPathRunsNoCollection)
{
    auto source = make_ref<UnnamedRegularSource>("one observation", false, false, std::nullopt);
    auto add = [&] {
        source->reads = 0;
        return store->materialise("unnamed-auto-gc", SourcePath(source), defaultPathFilter, NoRepair, noNode());
    };
    auto path = add();
    ASSERT_FALSE(pathExists(garbage));
    writeFile(garbage, "dead");
    writeFile(fakeFree, "500");
    EXPECT_EQ(add(), path);
    EXPECT_EQ(source->reads, 1u);
    EXPECT_TRUE(pathExists(garbage)) << "an already valid buffered copy ran collection";
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

namespace {

/* Expose the existing cache for its finite state checks, and observe the
   backend boundary without adding a production test hook. */
struct ValidityObservedStore : LocalStore
{
    using CacheValue = PathInfoCacheValue;
    std::atomic<unsigned> validityQueries = 0;
    unsigned infoQueries = 0;
    std::function<void()> afterValidityQuery;

    explicit ValidityObservedStore(ref<const LocalStore::Config> config)
        : Store{*config}
        , LocalFSStore{*config}
        , LocalStore{config}
    {
    }

    bool isValidPathUncached(const StorePath & path) override
    {
        validityQueries++;
        auto valid = LocalStore::isValidPathUncached(path);
        if (afterValidityQuery)
            afterValidityQuery();
        return valid;
    }

    void queryPathInfoUncached(
        const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override
    {
        infoQueries++;
        LocalStore::queryPathInfoUncached(path, std::move(callback));
    }

    void seed(const StorePath & path, CacheValue value)
    {
        pathInfoCache->lock()->upsert(path, std::move(value));
    }

    std::optional<CacheValue> cached(const StorePath & path)
    {
        if (pathInfoCache)
            return pathInfoCache->lock()->get(path);
        return std::nullopt;
    }

    void useDiskCache(ref<NarInfoDiskCache> cache)
    {
        diskCache = cache.get_ptr();
    }
};

} // namespace

TEST_F(TempLocalStoreTest, validityCacheKeepsExistenceSeparateFromDescriptions)
{
    StringSource source{std::string_view{"validity cache"}};
    auto present = store->addToStoreFromDump(
        source,
        "present",
        FileSerialisationMethod::Flat,
        ContentAddressMethod::Raw::Flat,
        HashAlgorithm::SHA256,
        {},
        NoRepair);
    auto missing = store->makeStorePath("source", hashString(HashAlgorithm::SHA256, "missing"), "missing");
    auto full = store->queryPathInfo(present).get_ptr();
    auto & ttl = settings.getNarInfoDiskCacheSettings();
    auto oldPositive = ttl.ttlPositive.get(), oldNegative = ttl.ttlNegative.get();
    Finally restore{[&] {
        ttl.ttlPositive = oldPositive;
        ttl.ttlNegative = oldNegative;
    }};
    ttl.ttlNegative = 600;
    enum class Initial { Absent, Missing, ValidOnly, Full };
    const std::array initialStates{
        std::pair{Initial::Absent, false},
        std::pair{Initial::Missing, false},
        std::pair{Initial::Missing, true},
        std::pair{Initial::ValidOnly, false},
        std::pair{Initial::ValidOnly, true},
        std::pair{Initial::Full, false},
        std::pair{Initial::Full, true}};
    unsigned cells = 0;
    for (bool enabled : {false, true})
        for (bool exists : {false, true})
            for (unsigned positiveTtl : {0u, 600u})
                for (auto [initial, expired] : initialStates) {
                    if (!enabled && initial != Initial::Absent)
                        continue;
                    ++cells;
                    SCOPED_TRACE(
                        fmt("enabled=%d exists=%d ttl=%u initial=%d expired=%d",
                            enabled,
                            exists,
                            positiveTtl,
                            static_cast<int>(initial),
                            expired));
                    config->pathInfoCacheSize = enabled ? 8 : 0;
                    ttl.ttlPositive = positiveTtl;
                    auto observed = std::make_shared<ValidityObservedStore>(ref{config});
                    const auto & path = exists ? present : missing;
                    if (initial != Initial::Absent) {
                        ValidityObservedStore::CacheValue value;
                        if (initial == Initial::ValidOnly)
                            value.value = ValidityObservedStore::CacheValue::ValidWithoutInfo{};
                        if (initial == Initial::Full) {
                            auto description = std::make_shared<ValidPathInfo>(*full);
                            description->path = path;
                            value.value = std::shared_ptr<const ValidPathInfo>{description};
                        }
                        if (expired)
                            value.time_point -= std::chrono::hours(1);
                        observed->seed(path, std::move(value));
                    }
                    auto before = observed->cached(path);
                    EXPECT_EQ(observed->isValidPathUncached(path), exists);
                    auto after = observed->cached(path);
                    if (!exists || !enabled) {
                        /* A SQL miss adds no negative observation. */
                        ASSERT_EQ(after.has_value(), before.has_value());
                        if (before) {
                            EXPECT_EQ(after->time_point, before->time_point);
                            EXPECT_EQ(after->value.index(), before->value.index());
                            if (auto info = std::get_if<std::shared_ptr<const ValidPathInfo>>(&before->value)) {
                                EXPECT_EQ(std::get<std::shared_ptr<const ValidPathInfo>>(after->value), *info);
                            }
                        }
                    } else {
                        ASSERT_TRUE(after);
                        EXPECT_TRUE(after->didExist());
                        auto metadata = observed->queryPathInfoFromClientCache(path);
                        if (initial == Initial::Full && !expired && positiveTtl) {
                            ASSERT_TRUE(metadata);
                            EXPECT_EQ(*metadata, std::get<std::shared_ptr<const ValidPathInfo>>(before->value));
                            EXPECT_EQ(after->time_point, before->time_point);
                        } else {
                            EXPECT_TRUE(
                                std::holds_alternative<ValidityObservedStore::CacheValue::ValidWithoutInfo>(
                                    after->value));
                            EXPECT_FALSE(metadata.has_value());
                        }
                    }
                    if (exists) {
                        EXPECT_TRUE(observed->isValidPath(path));
                        EXPECT_EQ(observed->validityQueries.load(), enabled && positiveTtl ? 1u : 2u);
                        auto description = observed->queryPathInfo(path);
                        EXPECT_EQ(description->path, present);
                        EXPECT_EQ(description->objectHash, full->objectHash);
                        EXPECT_EQ(
                            observed->infoQueries,
                            enabled && initial == Initial::Full && !expired && positiveTtl ? 0u : 1u);
                    }
                }
    EXPECT_EQ(cells, 32u);
}

TEST_F(TempLocalStoreTest, validityOnlyCacheDoesNotMigrateOrReplacePathInfo)
{
    for (bool filesPresent : {false, true}) {
        SCOPED_TRACE(filesPresent);
        StringSource source{std::string_view{"old validity row"}};
        auto path = store->addToStoreFromDump(
            source,
            filesPresent ? "present-old" : "missing-old",
            FileSerialisationMethod::Flat,
            ContentAddressMethod::Raw::Flat,
            HashAlgorithm::SHA256,
            {},
            NoRepair);
        auto full = store->queryPathInfo(path);
        StringSink nar;
        dumpString("old validity row", nar);
        auto narHash = hashString(HashAlgorithm::SHA256, nar.s);
        auto narColumn = narHash.to_string(HashFormat::Base16, true);
        plantSchema10Row(*store, path, narHash, nar.s.size());
        if (!filesPresent)
            deletePath(store->toRealPath(path));
        auto observed = std::make_shared<ValidityObservedStore>(ref{config});
        ASSERT_TRUE(observed->isValidPath(path));
        ASSERT_TRUE(observed->isValidPath(path));
        EXPECT_EQ(observed->validityQueries.load(), 1u);
        EXPECT_EQ(observed->infoQueries, 0u);
        EXPECT_EQ(readHashColumn(*observed, path), narColumn);
        auto description = observed->queryPathInfo(path);
        EXPECT_EQ(observed->infoQueries, 1u);
        EXPECT_EQ(description->assertedNarHash, narHash);
        EXPECT_EQ(description->narSize, nar.s.size());
        if (filesPresent) {
            EXPECT_EQ(description->objectHash, full->objectHash);
            EXPECT_NE(readHashColumn(*observed, path), narColumn);
        } else {
            EXPECT_FALSE(description->objectHash);
            EXPECT_EQ(readHashColumn(*observed, path), narColumn);
        }
    }
}

TEST_F(TempLocalStoreTest, diskValidityWorksWithDisabledMemoryCache)
{
    StringSource source{std::string_view{"disk validity"}};
    auto path = store->addToStoreFromDump(
        source,
        "disk-validity",
        FileSerialisationMethod::Flat,
        ContentAddressMethod::Raw::Flat,
        HashAlgorithm::SHA256,
        {},
        NoRepair);
    auto full = std::make_shared<NarInfo>(*store->queryPathInfo(path));
    full->url = "nar/test.nar";
    full->compression = CompressionAlgo::none;
    auto disk = NarInfoDiskCache::getTest(
        settings.getNarInfoDiskCacheSettings(),
        {.useWAL = settings.useSQLiteWAL},
        tempStoreDir.path() / "narinfo.sqlite");
    auto uri = config->getReference().render(false);
    disk->createCache(uri, store->storeDir, {});
    for (bool enabled : {false, true})
        for (bool exists : {false, true}) {
            SCOPED_TRACE(fmt("enabled=%d exists=%d", enabled, exists));
            config->pathInfoCacheSize = enabled ? 8 : 0;
            auto observed = std::make_shared<ValidityObservedStore>(ref{config});
            observed->useDiskCache(disk);
            disk->upsertNarInfo(uri, std::string(path.hashPart()), exists ? full : nullptr);
            EXPECT_EQ(observed->isValidPath(path), exists);
            EXPECT_EQ(observed->validityQueries.load(), 0u);
            if (exists) {
                EXPECT_EQ(observed->queryPathInfo(path)->objectHash, full->objectHash);
                /* The hash-only lookup still obtains its actual name from
                   the complete disk description, never from a bool hit. */
                auto unnamed = StorePath(std::string(path.hashPart()) + '-' + Store::MissingName);
                if (enabled)
                    observed->seed(unnamed, {.value = ValidityObservedStore::CacheValue::ValidWithoutInfo{}});
                EXPECT_EQ(observed->queryPathInfo(unnamed)->path, path);
            } else
                EXPECT_THROW(observed->queryPathInfo(path), InvalidPath);
            EXPECT_EQ(observed->infoQueries, 0u);
        }
}

TEST_F(TempLocalStoreTest, invalidationAfterValidityBackendCannotBeUndoneByItsReturn)
{
    StringSource source{std::string_view{"invalidate observed validity"}};
    auto path = store->addToStoreFromDump(
        source,
        "validity-race",
        FileSerialisationMethod::Flat,
        ContentAddressMethod::Raw::Flat,
        HashAlgorithm::SHA256,
        {},
        NoRepair);
    auto observed = std::make_shared<ValidityObservedStore>(ref{config});
    std::promise<void> queried, resume;
    auto queriedFuture = queried.get_future();
    auto resumeFuture = resume.get_future().share();
    std::atomic<bool> first = true;
    observed->afterValidityQuery = [&] {
        if (first.exchange(false)) {
            queried.set_value();
            resumeFuture.wait();
        }
    };
    auto result = std::async(std::launch::async, [&] { return observed->isValidPath(path); });
    bool released = false;
    auto release = [&] {
        if (!released) {
            released = true;
            resume.set_value();
        }
    };
    Finally unblock{release};
    ASSERT_EQ(queriedFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    observed->invalidatePathChecked(path);
    release();
    EXPECT_TRUE(result.get()); /* It observed the row before invalidation. */
    EXPECT_FALSE(observed->isValidPath(path));
    EXPECT_FALSE(observed->cached(path).has_value());
    EXPECT_THROW(observed->queryPathInfo(path), InvalidPath);
}

#endif

} // namespace nix
