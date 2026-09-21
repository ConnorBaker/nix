#include "nix/util/serialise.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/archive.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/store/git-object-store.hh"
#include "nix/store/local-store.hh"
#include "nix/store/local-settings.hh"
#include "nix/store/globals.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/signals.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/file-system.hh"
#include "nix/util/file-system-at.hh"
#include "nix/util/finally.hh"
#include "nix/util/logging.hh"
#include "nix/util/strings.hh"
#include "nix/util/util.hh"

#include <algorithm>
#include <mutex>
#include <future>
#include <deque>
#include <condition_variable>
#include <thread>
#include <set>
#include <fcntl.h>
#include <regex>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nix {

/* The object store's visitors and their two drivers, a NAR being restored
   and a path already on disk (`08` §1.6). */

#ifdef __APPLE__
/* HFS/macOS has some undocumented security feature disabling hardlinking for
   special files within .app dirs. Known affected paths include
   *.app/Contents/{PkgInfo,Resources/\*.lproj,_CodeSignature} and .DS_Store.
   See https://github.com/NixOS/nix/issues/1443 and
   https://github.com/NixOS/nix/pull/2230 for more discussion. */
static bool linkableOnThisSystem(const std::filesystem::path & path)
{
    static const std::regex appContents("\\.app/Contents/.+$");
    return !std::regex_search(path.string(), appContents);
}
#else
static bool linkableOnThisSystem(const std::filesystem::path &)
{
    return true;
}
#endif

/* Whether the store places the regular file already on disk at `where`
   -- gives it a blob and links it -- or leaves it as written, and why:
   the exceptions of 01 §9.10 law 3 that can be read off the file.  Decided
   here alone: the on-disk walk skips such a file, and the
   verifier expects no blob for it.  (The restore route asks
   `linkableOnThisSystem` before the file exists; its files are never
   writable, having been canonicalised.)  Law 3's fourth exception -- a
   file `place` could not enter for `ENOSPC` on a full directory index, or
   `EMLINK` -- leaves no mark on the file, so it is not decided here: the
   verifier reports such a file as missing and `--repair` tries again. */
enum struct Placement { Placed, LeftWritable, NotLinkable };

static Placement placementOf(const std::filesystem::path & where)
{
#ifdef _WIN32
    /* The object store shares nothing there (git-object-store.hh). */
    return Placement::NotLinkable;
#else
    /* A file the store marks writable was modified in place (a SNAFU,
       e.g. a fontconfig cache under a root that ran programs); it is
       named by what it holds now and not shared, as optimisation left it. */
    if (lstat(where).st_mode & S_IWUSR)
        return Placement::LeftWritable;
    return linkableOnThisSystem(where) ? Placement::Placed : Placement::NotLinkable;
#endif
}

/* A directory made writable for the duration, and canonical again after.
   Opened before it is touched, so that a directory that cannot be opened
   is not left writable. */
struct WritableDirectory
{
    std::filesystem::path path;
    AutoCloseFD fd;

    WritableDirectory(const std::filesystem::path & path, bool toggle)
        : fd(openDirectory(path))
    {
        if (!fd)
            throw SysError("opening directory %1%", PathFmt(path));
        if (toggle) {
#ifndef _WIN32
            if (fchmod(fd.get(), nix::fstat(fd.get()).st_mode | S_IWUSR) == -1)
                throw SysError("making directory %1% writable", PathFmt(path));
#else
            chmod(path, lstat(path).st_mode | S_IWUSR);
#endif
            this->path = path;
        }
    }

    ~WritableDirectory()
    {
        try {
            /* Read-only again, and its timestamp back to 0. */
            if (!path.empty())
                canonicaliseTimestampAndPermissions(path.string());
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
};

/* The on-disk path of the node at `path` under `root`: the on-disk walks'
   paths carry the on-disk names, so this opens the right file on a
   case-insensitive filesystem. */
static std::filesystem::path onDisk(const std::filesystem::path & root, const CanonPath & path)
{
    return path.isRoot() ? root : root / path.rel();
}

/* The status of `name` in `dirFd`, at `where`, without following a symlink. */
static PosixStat statIn(Descriptor dirFd, const CanonPath & name, const std::filesystem::path & where)
{
#ifdef _WIN32
    return lstat(where);
#else
    PosixStat st;
    if (fstatat(dirFd, name.rel_c_str(), &st, AT_SYMLINK_NOFOLLOW) == -1)
        throw SysError("getting status of %1%", PathFmt(where));
    return st;
#endif
}

/* `GitObjectStore::place` for the written, canonicalised regular file `name` in the writable directory `dirFd`
   (at `where`); the walk's `inodes`, `stats` and `act`, when given, record the inode, a file entered as the store's
   and a file replaced by it. */
static void placeRegularFile(
    GitObjectStore & objects,
    const std::filesystem::path & where,
    const merkle::TreeEntry & entry,
    Descriptor dirFd,
    const CanonPath & name,
    RepairFlag repair,
    const GitObjectStore::Hold & hold,
    GitObjectStore::BlobInodes * inodes = nullptr,
    OptimiseStats * stats = nullptr,
    Activity * act = nullptr)
{
    bool executable = entry.mode == merkle::Mode::Executable;

    auto before = statIn(dirFd, name, where);

    auto after = objects.place(entry.hash, executable, dirFd, name, where, before, repair, hold);
    if (!after)
        return;
    if (inodes)
        inodes->insert_or_assign(after->ino, entry.hash);
    if (after->entered && stats)
        stats->filesEntered++;
    if (after->ino != before.st_ino && stats) {
        /* Replaced by the store's file: the bytes of this one are freed. */
        printMsg(lvlTalkative, "linked %1% to the blob's holder", PathFmt(where));
        stats->filesLinked++;
        stats->bytesFreed += static_cast<uint64_t>(before.st_size);
        if (act)
            act->result(
                resFileLinked,
                before.st_size
#ifndef _WIN32
                ,
                before.st_blocks
#endif
            );
    }
}

namespace {

/* The object store's side of the hasher for what is not a regular file:
   each directory's tree and each symlink's target written as hashed.
   Regular files are the drivers' business (below): placed after they are
   on disk, or linked instead of written. */
struct ObjectStoreVisitor : HashingVisitor
{
    GitObjectStore & objects;
    /* The ingestion's hold on what it writes (`08` K16), the caller's, so
       that it outlives the walk up to the registration. */
    const GitObjectStore::Hold & hold;

    ObjectStoreVisitor(GitObjectStore & objects, const GitObjectStore::Hold & hold)
        : objects(objects)
        , hold(hold)
    {
    }

    HashedNode symlink(const CanonPath & path, const std::string & target) override
    {
        auto node = HashingVisitor::symlink(path, target);
        objects.putBlob(node.entry.hash, false, target, hold);
        return node;
    }

    HashedNode directory(const CanonPath & path, Children children) override
    {
        auto node = HashingVisitor::directory(path, std::move(children));
        objects.putTree(node.entry.hash, node.treeBody, hold);
        return node;
    }
};

/* The visitor for a store path already on disk at `root`: a regular file
   whose inode is a blob's (`inodes`, when given) is not read; every other
   is hashed and placed, its read-only directory made writable around
   `place` and a writable file left alone as suspicious; `stats` and `act`,
   when given, count the files replaced by the store's. */
struct EnterVisitor : ObjectStoreVisitor
{
    LocalStore & store;
    const std::filesystem::path root;
    RepairFlag repair;
    GitObjectStore::BlobInodes * inodes;
    OptimiseStats * stats;
    Activity * act;

    EnterVisitor(
        LocalStore & store,
        const GitObjectStore::Hold & hold,
        std::filesystem::path root,
        RepairFlag repair,
        GitObjectStore::BlobInodes * inodes,
        OptimiseStats * stats,
        Activity * act)
        : ObjectStoreVisitor(store.objects, hold)
        , store(store)
        , root(std::move(root))
        , repair(repair)
        , inodes(inodes)
        , stats(stats)
        , act(act)
    {
    }

    std::optional<HashedNode> known(const CanonPath & path, const SourceAccessor::Stat & st) override
    {
        if (!inodes || st.type != SourceAccessor::tRegular)
            return std::nullopt;
        /* The index is the witness, not the link count: a holder that no
           second path shares is at one link (design.md section 3, K8).  The
           `lstat` stays, because `readdir`'s `d_ino` can differ from it. */
        auto file = lstat(onDisk(root, path));
        auto i = inodes->find(file.st_ino);
        if (i == inodes->end())
            return std::nullopt;
        /* The size is the stat's; the NAR size stays exact. */
        return HashedNode{
            .entry = {.mode = st.isExecutable ? merkle::Mode::Executable : merkle::Mode::Regular, .hash = i->second},
            .narSize = st.fileSize ? std::optional(merkle::nar::regular(*st.fileSize, st.isExecutable)) : std::nullopt,
        };
    }

    HashedNode regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override
    {
        auto node = HashingVisitor::regular(path, std::move(read));
        auto where = onDisk(root, path);

        switch (placementOf(where)) {
        case Placement::LeftWritable:
            warn("skipping suspicious writable file '%s'", PathFmt(where));
            return node;
        case Placement::NotLinkable:
            debug("%s is left as written: this system does not link it", PathFmt(where));
            return node;
        case Placement::Placed:
            break;
        }

        /* The containing directory writable, unless it is the store itself. */
        auto parent = where.parent_path();
        WritableDirectory dir(parent, parent != store.config->realStoreDir.get());
        auto name = CanonPath::fromFilename(where.filename().string());

        placeRegularFile(objects, where, node.entry, dir.fd.get(), name, repair, hold, inodes, stats, act);
        return node;
    }
};

/* The verifier's visitor (`LocalStore::verifyObjects`): law 4 of 01 §9.10
   for one path as it stands on disk, with law 3's exceptions -- every
   tree on the way and every symlink's target blob must be present, and
   the blob of every regular file the store places (`placementOf`).  The
   paths are the on-disk ones, so a case-hacked name is looked at where it
   is; the entry names are the tree's own, as the hasher's are. */
struct VerifyVisitor : HashingVisitor
{
    GitObjectStore & objects;
    const std::filesystem::path root;
    std::vector<std::filesystem::path> missing;

    VerifyVisitor(GitObjectStore & objects, std::filesystem::path root)
        : objects(objects)
        , root(std::move(root))
    {
    }

    void expect(const std::filesystem::path & object)
    {
        if (!pathExists(object))
            missing.push_back(object);
    }

    HashedNode regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override
    {
        auto node = HashingVisitor::regular(path, std::move(read));
        if (placementOf(onDisk(root, path)) == Placement::Placed) {
            /* Law 4 for a regular-file blob: the index knows the identifier
               with a holder that is still what the row says.  The bytes are
               under the verifier's own hands -- this very file -- so there
               is nothing else to check; what the row adds is that the store
               will not write the blob again (design.md section 2.3). */
            bool executable = node.entry.mode == merkle::Mode::Executable;
            if (!objects.holderOrForget(node.entry.hash, executable, NoRepair))
                missing.push_back(onDisk(root, path));
        }
        return node;
    }

    HashedNode symlink(const CanonPath & path, const std::string & target) override
    {
        auto node = HashingVisitor::symlink(path, target);
        expect(objects.blobFile(node.entry.hash, false));
        return node;
    }

    HashedNode directory(const CanonPath & path, Children children) override
    {
        auto node = HashingVisitor::directory(path, std::move(children));
        expect(objects.treeFile(node.entry.hash));
        return node;
    }
};

/* A regular file is held in memory up to this many bytes, so that whether
   to write it at all is decided before the disk is touched (01 §9.10). */
static constexpr uint64_t restoreBufferLimit = 1 << 20;

/* The restore route's visitor (01 §9.10, "Ingestion writes only what the
   store lacks"; `08` §1.6): a regular file is held up to
   `restoreBufferLimit` and, with its entry in hand, linked from the store's
   file -- never written, so `regularFileCreated` does not fire for it --
   or written and placed; above the limit it is streamed and placed.  The
   children are created inside `children`, through the sink the
   `RestoreSink`'s directory callback handed back, so that its
   `directoryDone` canonicalises the directory when the scope closes. */
struct LinkingVisitor : ObjectStoreVisitor
{
    RepairFlag repair;
    Sink * rootContents;

    /* The stacks a copy carries down its recursion: the sink a node's
       children are created on -- the enclosing directory's own, innermost
       last, the root's being the caller's `RestoreSink` -- and, under the
       second route, the case-hacked on-disk names around each child's
       visit.  They are *per thread*: the parallel copy below walks
       sibling directories on a pool, and the traversal hands every worker
       the same visitor object, so a worker seeds its own stacks with the
       directory it starts at and unwinds them alone.  `Scope` saves and
       restores, so the seeding nests. */
    struct Frames
    {
        std::vector<RestoreSink *> open;
        std::vector<std::string> diskNames;
    };

    static Frames & frames()
    {
        static thread_local Frames f;
        return f;
    }

    struct Scope
    {
        Frames saved;

        Scope(RestoreSink & sink, std::optional<std::string> diskName)
        {
            auto & f = frames();
            saved = std::move(f);
            f.open.clear();
            f.open.push_back(&sink);
            f.diskNames.clear();
            if (diskName)
                f.diskNames.push_back(std::move(*diskName));
        }

        ~Scope()
        {
            frames() = std::move(saved);
        }
    };

    /* The walking thread's seed, for this visitor's whole life. */
    Scope scope;

    LinkingVisitor(
        GitObjectStore & objects,
        const GitObjectStore::Hold & hold,
        RestoreSink & restore,
        RepairFlag repair,
        Sink * rootContents = nullptr)
        : ObjectStoreVisitor(objects, hold)
        , repair(repair)
        , rootContents(rootContents)
        , scope(restore, std::nullopt)
    {
    }

    /* Where the node at `path` goes: the innermost open sink, under `/name`
       (the root node under `/`), and the path on disk. */
    struct Target
    {
        RestoreSink & sink;
        CanonPath name;
        std::filesystem::path where;
    };

    /* The on-disk name of the node at `path`: under the push driver the
       NAR restorer's, case hack applied, which `path` carries already. */
    virtual std::string diskName(const CanonPath & path)
    {
        return std::string(*path.baseName());
    }

    Target at(const CanonPath & path)
    {
        auto & sink = *frames().open.back();
        auto name = path.isRoot() ? path : CanonPath::root / diskName(path);
        return {sink, name, name.isRoot() ? sink.dstPath : sink.dstPath / name.rel()};
    }

    HashedNode directory(const CanonPath & path, Children children) override
    {
        auto target = at(path);
        return ObjectStoreVisitor::directory(path, [&](fun<void(std::string_view, bool, fun<HashedNode()>)> each) {
            target.sink.createSubdirectory(target.name, [&](RestoreSink & sub, const CanonPath & rel) {
                assert(rel.isRoot());
                auto & open = frames().open;
                open.push_back(&sub);
                Finally close{[&] { open.pop_back(); }};
                children(std::move(each));
            });
        });
    }

    HashedNode symlink(const CanonPath & path, const std::string & target) override
    {
        auto t = at(path);
        t.sink.createSymlink(t.name, target);
        return ObjectStoreVisitor::symlink(path, target);
    }

    HashedNode regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override
    {
        /* Receives the file in the order both drivers deliver it -- the
           executable flag, the size, the bytes -- forwarding all of it to
           the hasher's file sink and holding the bytes until the limit,
           from which point the restore sink's file is open and written as
           they arrive. */
        struct BufferThenStreamFile : CreateRegularFileSink
        {
            RestoreSink & restore;
            const CanonPath & name;
            CreateRegularFileSink & hashing;
            Sink * contents;
            bool executable = false;
            std::string buffer;
            std::unique_ptr<RegularFileWriter> writer;

            BufferThenStreamFile(
                RestoreSink & restore, const CanonPath & name, CreateRegularFileSink & hashing, Sink * contents)
                : restore(restore)
                , name(name)
                , hashing(hashing)
                , contents(contents)
            {
            }

            void isExecutable() override
            {
                executable = true;
                hashing.isExecutable();
                if (writer)
                    writer->isExecutable();
            }

            void preallocateContents(uint64_t size) override
            {
                hashing.preallocateContents(size);
                if (size > restoreBufferLimit)
                    stream();
                if (writer)
                    writer->preallocateContents(size);
            }

            /* Open the file and hand it what has arrived so far. */
            void stream()
            {
                if (writer)
                    return;
                writer = restore.beginRegularFile(name);
                if (executable)
                    writer->isExecutable();
                if (!buffer.empty()) {
                    (*writer)(buffer);
                    buffer.clear();
                }
            }

            void operator()(std::string_view data) override
            {
                hashing(data);
                if (contents)
                    (*contents)(data);
                if (!writer && buffer.size() + data.size() > restoreBufferLimit)
                    stream();
                if (writer)
                    (*writer)(data);
                else
                    buffer.append(data);
            }
        };

        auto target = at(path);
        bool executable = false;
        std::string buffer;
        std::unique_ptr<RegularFileWriter> writer;

        auto node = HashingVisitor::regular(path, [&](CreateRegularFileSink & hashing) {
            BufferThenStreamFile file{target.sink, target.name, hashing, path.isRoot() ? rootContents : nullptr};
            read(file);
            executable = file.executable;
            buffer = std::move(file.buffer);
            writer = std::move(file.writer);
        });
        auto & entry = node.entry;

        bool linkable = linkableOnThisSystem(target.where);
        if (!linkable)
            debug("%s is not allowed to be linked in macOS", PathFmt(target.where));

#ifdef _WIN32
        /* Nothing is linked or placed there (git-object-store.hh). */
        AutoCloseFD owned;
        Descriptor dirFd = INVALID_DESCRIPTOR;
        CanonPath name = CanonPath::fromFilename(target.where.filename().string());
#else
        auto [owned, dirFd, name] = target.sink.parentOf(target.name);
#endif

        if (writer) {
            /* Streamed: written as received, then entered as the store's
               file -- or, should the store hold one, replaced by it. */
            writer->finish();
            writer.reset();
            if (linkable)
                objects.enterWritten(entry.hash, executable, dirFd, name, target.where, repair, hold, /*looked=*/false);
            return node;
        }

        /* The whole file is in memory and its blob known.  A hit: the
           store's valid file linked under the name, nothing written. */
        if (linkable && objects.link(entry.hash, executable, dirFd, name, target.where, buffer.size(), repair, hold))
            return node;

        /* A miss: written from memory, canonicalised by the restore sink's
           hooks, and entered as the store's file -- or, should another
           writer have entered the blob meanwhile, replaced by its file. */
        target.sink.createRegularFile(target.name, [&](CreateRegularFileSink & crf) {
            if (executable)
                crf.isExecutable();
            crf.preallocateContents(buffer.size());
            crf(buffer);
        });
        if (linkable)
            objects.enterWritten(entry.hash, executable, dirFd, name, target.where, repair, hold);
        return node;
    }
};

#ifndef _WIN32
/* A bounded pool for nested directory scopes. A full queue makes the
   caller visit inline. While joining, a caller helps only its own child
   jobs: nested helping follows the tree's depth, never an unbounded chain
   of siblings, and waiting parents cannot exhaust all execution slots.
   The caller counts as one additional worker. */
class CopyPool
{
    std::mutex mutex;
    std::condition_variable wake;

    struct Job
    {
        const void * group;
        std::packaged_task<void()> run;
    };

    std::deque<Job> pending;
    std::vector<std::thread> threads;
    size_t capacity;
    bool stopping = false;

    void work()
    {
        ReceiveInterrupts receiveInterrupts;
        while (true) {
            std::packaged_task<void()> job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !pending.empty(); });
                if (pending.empty())
                    return;
                job = std::move(pending.front().run);
                pending.pop_front();
            }
            job(); // The future carries the exception back to its directory.
        }
    }

public:
    explicit CopyPool(size_t workers)
        : capacity(workers - 1)
    {
        threads.reserve(capacity);
    }

    ~CopyPool()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        for (auto & thread : threads)
            thread.join();
    }

    std::future<void> tryAsync(const void * group, std::function<void()> run)
    {
        std::lock_guard lock(mutex);
        if (pending.size() == capacity)
            return {};
        std::packaged_task<void()> job(std::move(run));
        auto result = job.get_future();
        if (threads.size() < capacity)
            threads.emplace_back([this] { work(); });
        pending.push_back({group, std::move(job)});
        wake.notify_one();
        return result;
    }

    void wait(std::future<void> & result, const void * group)
    {
        while (result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            std::packaged_task<void()> job;
            {
                std::lock_guard lock(mutex);
                auto i = std::find_if(pending.begin(), pending.end(), [&](auto & job) { return job.group == group; });
                if (i != pending.end()) {
                    job = std::move(i->run);
                    pending.erase(i);
                }
            }
            if (!job.valid()) {
                result.wait();
                return;
            }
            job();
        }
    }
};

/* The second route's visitor (01 §9.10, "One route"; `08` §1.6): the
   pull driver over the source accessor with the one route's
   `LinkingVisitor` beneath, and `known` answering a directory, or the
   root, from the store's own objects -- when the naming names it
   (`namer`) and the store holds it, the subtree is made from `trees/` and
   `blobs/` by `mkdirat`, `linkat` and `symlinkat`, and nothing beneath it
   is read from the source; else the driver descends as it would, and
   what it reaches is linked or written as the one route links or writes
   it.  Per unchanged regular file: one `fstatat` of the blob's file and
   one `linkat`, no read, no hash.  `known` is asked for directories and
   the root alone: a file inside a directory the store lacks is read, so
   that a fresh tree costs one memo lookup per directory and not one per
   file.  Names: the driver hands the accessor's, the tree's own is what
   `directory` receives per child, and its on-disk form is the case hack's
   (`CaseHackNames`, the NAR restorer's rule), kept on `diskNames` around
   each child's visit for `at` to read. */
struct MaterialisingVisitor : LinkingVisitor
{
    const SourcePath & source;
    LocalStore::TreeNamer namer;
    RestoreSinkHooks * hooks;

    /* The naming's memo, which the namer closes over, is not made for two
       threads: `MemoVisitor::keyOf` initialises its `Cache` handle lazily
       and `getFingerprint` goes to the accessor. Calls are serialized;
       the directory walk and holder writes run outside this lock.
     */
    std::mutex namerLock;

    size_t workers;
    std::unique_ptr<CopyPool> pool;

    MaterialisingVisitor(
        GitObjectStore & objects,
        const GitObjectStore::Hold & hold,
        RestoreSink & restore,
        RepairFlag repair,
        LocalStore::TreeNamer namer,
        RestoreSinkHooks * hooks,
        size_t workers,
        const SourcePath & source)
        : LinkingVisitor(objects, hold, restore, repair)
        , source(source)
        , namer(std::move(namer))
        , hooks(hooks)
        , workers(workers)
    {
    }

    std::string diskName(const CanonPath & path) override
    {
        auto & diskNames = frames().diskNames;
        return diskNames.empty() ? std::string(*path.baseName()) : diskNames.back();
    }

    HashedNode directory(const CanonPath & path, Children children) override
    {
        return LinkingVisitor::directory(path, [&](fun<void(std::string_view, bool, fun<HashedNode()>)> each) {
            copyChildren(path, std::move(children), each);
        });
    }

    /* A directory's children visited, its subdirectories on a bounded pool
       and everything else on this thread, and the results handed to `each`
       in the order the traversal offered them -- so the tree body, the NAR
       size and the case hack are exactly what the sequential walk gives
       (`01` §9.10; the property test `prop_materialised_path_is_the_restored_path`).
       The on-disk names are the case hack's and are computed here, in that
       same order, on this thread.

       Nested directories may use an idle pool slot. Every child is joined
       even if another child fails, before its directory's accessor and sink
       leave scope. Listing types affect scheduling alone. */
    void copyChildren(const CanonPath &, Children children, fun<void(std::string_view, bool, fun<HashedNode()>)> & each)
    {
        struct Child
        {
            std::string name;
            std::string disk;
            fun<HashedNode()> visit;
            bool subdirectory;
            std::optional<HashedNode> node;
            std::future<void> running;
        };

        std::vector<Child> kids;
        CaseHackNames hack;
        children([&](std::string_view name, bool isDirectory, fun<HashedNode()> visit) {
            kids.push_back({std::string(name), hack.diskName(name), std::move(visit), isDirectory, std::nullopt, {}});
        });
        /* Futures from packaged tasks do not join in their destructor.
           Keep every captured directory alive on both success and failure. */
        Finally join{[&] {
            for (auto & child : kids)
                if (child.running.valid())
                    pool->wait(child.running, &kids);
        }};

        auto here = [&](Child & c) {
            auto & diskNames = frames().diskNames;
            diskNames.push_back(c.disk);
            Finally pop{[&] { diskNames.pop_back(); }};
            c.node = c.visit();
        };

        /* A singleton has no sibling work to overlap. Its visit can still
           use the pool below, when it reaches a branching directory. This
           also avoids constructing a scheduler for regular-file roots. */
        bool fork = workers > 1 && kids.size() > 1;
        if (fork && !pool && std::any_of(kids.begin(), kids.end(), [](auto & c) { return c.subdirectory; }))
            pool = std::make_unique<CopyPool>(workers);
        auto * sink = frames().open.back();
        for (auto & c : kids) {
            if (fork && c.subdirectory)
                c.running = pool->tryAsync(&kids, [&c, sink] {
                    Scope scope{*sink, c.disk};
                    c.node = c.visit();
                });
            if (!c.running.valid())
                here(c);
        }
        for (auto & c : kids)
            if (c.running.valid()) {
                pool->wait(c.running, &kids);
                c.running.get();
            }

        for (auto & c : kids) {
            assert(c.node);
            each(c.name, c.subdirectory, [&] { return std::move(*c.node); });
        }
    }

    std::optional<HashedNode> known(const CanonPath & path, const SourceAccessor::Stat & st) override
    {
        /* A verified immutable blob already has an identity before any
           bytes are read. Probe the holder first and stream a miss directly
           into its destination, without buffering and hashing it again.
           Mutable worktrees supply no such identity; repair ignores it. */
        if (!repair && st.type == SourceAccessor::tRegular && st.fileSize)
            if (auto entry = source.accessor->getKnownEntry(source.path / path);
                entry && entry->hash.algo == merkle::hashAlgo
                && entry->mode == (st.isExecutable ? merkle::Mode::Executable : merkle::Mode::Regular)) {
                auto target = at(path);
                auto [owned, dirFd, name] = target.sink.parentOf(target.name);
                bool linkable = linkableOnThisSystem(target.where);
                if (!linkable
                    || !objects.link(
                        entry->hash, st.isExecutable, dirFd, name, target.where, st.fileSize, repair, hold)) {
                    auto writer = target.sink.beginRegularFile(target.name);
                    if (st.isExecutable)
                        writer->isExecutable();
                    source.accessor->readFile(source.path / path, *writer, [&](uint64_t size) {
                        if (size != *st.fileSize)
                            throw Error("immutable file '%s' changed size while copying", source.path / path);
                        writer->preallocateContents(size);
                    });
                    writer->finish();
                    if (auto * fdSink = dynamic_cast<FdSink *>(writer.get()); fdSink && fdSink->written != *st.fileSize)
                        throw Error("immutable file '%s' delivered the wrong number of bytes", source.path / path);
                    writer.reset();
                    if (linkable)
                        objects.enterWritten(entry->hash, st.isExecutable, dirFd, name, target.where, repair, hold);
                }
                return HashedNode{.entry = *entry, .narSize = merkle::nar::regular(*st.fileSize, st.isExecutable)};
            }
        if (!path.isRoot() && st.type != SourceAccessor::tDirectory)
            return std::nullopt;
        auto entry = [&] {
            std::lock_guard<std::mutex> lock(namerLock);
            return namer(path, st);
        }();
        if (!entry)
            return std::nullopt;
        auto target = at(path);
        auto [owned, dirFd, name] = target.sink.parentOf(target.name);
        std::optional<uint64_t> narSize;
        try {
            narSize = materialise(*entry, dirFd, name, target.where, path.isRoot() ? 0 : 1);
        } catch (Error & e) {
            warn(
                "could not materialise %s from the object store: %s; copying it from the source instead",
                PathFmt(target.where),
                e.msg());
        }
        if (!narSize) {
            /* Whatever was made goes, and the driver reads the subtree. */
            if (maybeLstat(target.where))
                deletePath(target.where);
            debug("%s is not held by the object store; copying it from the source", PathFmt(target.where));
            return std::nullopt;
        }
        return HashedNode{.entry = *entry, .narSize = narSize};
    }

    /* The node `entry` made as `name` in the directory `dirFd` (at `where`)
       from the store's objects alone, and its NAR size; nullopt when an
       object it needs is missing or corrupt, or a file cannot be linked --
       what was made so far is the caller's to remove.  A tree body is
       verified against its identifier as it is read
       (`readVerifiedTree`); a blob's file is trusted as the on-disk walk
       trusts an inode that is a blob's (`08` K8), its bytes re-hashed
       under repair alone (K2); a symlink's target blob, a few bytes, is
       checked always.  A tree body and a symlink's blob the copy is made
       from are held first (`Hold`; `08` K16): the store's object is
       reachable from the path that entered it, which a collection running
       beside this copy may delete, and the path made here names it. */
    std::optional<uint64_t> materialise(
        const merkle::TreeEntry & entry,
        Descriptor dirFd,
        const CanonPath & name,
        const std::filesystem::path & where,
        size_t depth)
    {
        switch (entry.mode) {

        case merkle::Mode::Regular:
        case merkle::Mode::Executable: {
            bool executable = entry.mode == merkle::Mode::Executable;
            if (!linkableOnThisSystem(where)) {
                /* Left as written and not placed, as the one route leaves
                   it (`placementOf`): the blob's bytes, checked, written
                   plainly. */
                auto holder = objects.holderOrForget(entry.hash, executable, repair);
                if (!holder)
                    holder = objects.holderOrForget(entry.hash, !executable, repair);
                if (!holder)
                    return std::nullopt;
                auto file = std::filesystem::path(holder->file);
                auto bytes = readFile(file);
                if (merkle::blobId(bytes) != entry.hash) {
                    warn("blob object %s does not hash to its name; not used", PathFmt(file));
                    return std::nullopt;
                }
                auto fd = openFileEnsureBeneathNoSymlinks(dirFd, name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0666);
                if (!fd)
                    throw SysError("creating file %1%", PathFmt(where));
                writeFull(fd.get(), bytes);
                if (hooks)
                    hooks->regularFileCreated(fd.get(), executable);
                return merkle::nar::regular(bytes.size(), executable);
            }
            PosixStat linked;
            if (!objects.link(entry.hash, executable, dirFd, name, where, std::nullopt, repair, hold, &linked))
                return std::nullopt;
            return merkle::nar::regular(static_cast<uint64_t>(linked.st_size), executable);
        }

        case merkle::Mode::Symlink: {
            auto file = objects.blobFile(entry.hash, false);
            if (!objects.holdBlob(entry.hash, false, hold))
                return std::nullopt;
            auto target = readFile(file);
            if (merkle::blobId(target) != entry.hash) {
                warn("blob object %s does not hash to its name; not used", PathFmt(file));
                return std::nullopt;
            }
            if (::symlinkat(requireCString(target), dirFd, name.rel_c_str()) == -1)
                throw SysError("creating symlink from %1% -> '%2%'", PathFmt(where), target);
            if (hooks)
                hooks->symlinkCreated(dirFd, name);
            return merkle::nar::symlink(target.size());
        }

        case merkle::Mode::Directory: {
            if (depth >= narMaxDepth)
                throw Error("path %s exceeds maximum NAR directory depth of %d", PathFmt(where), narMaxDepth);
            if (!objects.holdTree(entry.hash, hold))
                return std::nullopt;
            auto tree = objects.readVerifiedTree(entry.hash);
            if (!tree)
                return std::nullopt;
            if (::mkdirat(dirFd, name.rel_c_str(), 0777) == -1)
                throw SysError("creating directory %s", PathFmt(where));
            auto fd = openFileEnsureBeneathNoSymlinks(dirFd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
            if (!fd)
                throw SysError("opening directory %s", PathFmt(where));
            /* In the NAR's order, by the tree's names (a directory's key
               carries a trailing slash), so that the case hack numbers a
               colliding name as the NAR restorer numbers it. */
            std::vector<std::pair<std::string_view, const merkle::TreeEntry *>> entries;
            for (auto & [key, child] : *tree) {
                std::string_view childName{key};
                if (child.mode == merkle::Mode::Directory)
                    childName.remove_suffix(1);
                if (childName.empty() || childName == "." || childName == ".."
                    || childName.find_first_of(std::string_view("/\0", 2)) != std::string_view::npos) {
                    warn(
                        "tree object %s names an entry '%s'; not used",
                        PathFmt(objects.treeFile(entry.hash)),
                        childName);
                    return std::nullopt;
                }
                entries.emplace_back(childName, &child);
            }
            std::sort(entries.begin(), entries.end(), [](auto & a, auto & b) { return a.first < b.first; });
            merkle::nar::Directory size;
            CaseHackNames hack;
            for (auto & [childName, child] : entries) {
                auto disk = hack.diskName(childName);
                auto sub = materialise(*child, fd.get(), CanonPath::fromFilename(disk), where / disk, depth + 1);
                if (!sub)
                    return std::nullopt;
                size.add(childName, *sub);
            }
            if (hooks)
                hooks->directoryDone(fd.get());
            return size.finish();
        }
        }
        throw Error("tree object names an entry of unknown mode %o", static_cast<RawMode>(entry.mode));
    }
};
#endif

} // namespace

ObjectHashSink::Result LocalStore::restoreThroughObjects(
    const std::filesystem::path & path,
    Source & source,
    bool startFsync,
    RestoreSinkHooks * hooks,
    const GitObjectStore::Hold & hold,
    RepairFlag repair,
    Sink * rootContents)
{
    RestoreSink restore{startFsync, hooks};
    restore.dstPath = path;
    LinkingVisitor linking{objects, hold, restore, repair, rootContents};
    ObjectHashSink adapter{linking};
    beginIngestion(hold, path);
    parseDump(adapter, source);
    auto result = adapter.finish();
    objects.putRootTree(result.root, hold);
    return result;
}

/* Test-only; zero means the fixed pool size computed in
   `materialiseThroughObjects` (local-store.hh). */
std::atomic<size_t> LocalStore::copyWorkersForTest{0};

bool LocalStore::materialisesFromObjects() const
{
#ifdef _WIN32
    /* The object store shares nothing there (git-object-store.hh), and
       the `*at` calls have no counterpart. */
    return false;
#else
    return ownsObjectStore();
#endif
}

ObjectHashSink::Result LocalStore::materialiseThroughObjects(
    const std::filesystem::path & dst,
    const SourcePath & path,
    PathFilter & filter,
    bool startFsync,
    RestoreSinkHooks * hooks,
    RepairFlag repair,
    TreeNamer namer,
    const GitObjectStore::Hold & hold)
{
#ifdef _WIN32
    throw Error("materialising a path from the object store is not supported on this system");
#else
    RestoreSink restore{startFsync, hooks};
    restore.dstPath = dst;
    /* The copy's subdirectories go on a small pool (`copyChildren`): the
       machine's width, capped to bound filesystem and index contention.
       Git repository reads remain serialized by their accessor; filesystem
       scopes permit independent reads.  Not a setting: what the copy makes does not
       depend on the count (01 §10), so there is nothing for a user to
       decide.  `copyWorkersForTest` is the tests' seam and is zero
       everywhere else; one thread is the sequential walk exactly. */
    size_t workers = copyWorkersForTest.load(std::memory_order_relaxed);
    if (!workers)
        workers = std::min<size_t>(std::max<unsigned int>(std::thread::hardware_concurrency(), 1), 8);
    MaterialisingVisitor visitor{objects, hold, restore, repair, std::move(namer), hooks, workers, path};
    beginIngestion(hold, dst);
    auto result = objectHashOf(*path.accessor, path.path, filter, visitor);
    /* Every `known` answer carries the size of what it made, so the NAR
       size is exact, as the one route's is. */
    assert(result.narSizeExact);
    objects.putRootTree(result.root, hold);
    return result;
#endif
}

StorePath LocalStore::materialise(
    std::string_view name, const SourcePath & path, PathFilter & filter, RepairFlag repair, TreeNamer namer)
{
    if (!materialisesFromObjects())
        throw Error("this store cannot materialise a path from its object store");

    /* The registration of `addToStoreFromDump`'s spilled route, step for
       step (local-store.cc): a NAR under the git method is restored into a
       locked temporary directory in the store, the content address is the
       root the restore computed, and the path is locked, moved into place
       and registered; the deferred signature check of `addToStore(info,
       source)` belongs to a received description and has no analogue
       here, as it has none there. */
    const LocalSettings & localSettings = config->getLocalSettings();
    auto hooks = makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(localSettings.ignoredAcls));

    /* Before the restore, for the reason `addToStore` gives: the objects
       are written before the path is registered (01 §10, *the
       ingestion's `autoGC()` runs before the restore*). */
    autoGC();

    /* The objects written and reused are held until the registration
       below (`GitObjectStore::Hold`; `08` K16). */
    auto hold = objects.hold();

    auto [tempDir, tempDirFd] = createTempDirInStore();
    AutoDelete delTempDir(tempDir);
    auto tempPath = tempDir / "x";

    auto result = materialiseThroughObjects(
        tempPath, path, filter, localSettings.fsyncStorePaths, hooks.get(), repair, std::move(namer), hold);

    if (settings.warnLargePathThreshold && result.narSize >= settings.warnLargePathThreshold)
        warn("copied large path '%s' to the store (%s)", path, renderSize(result.narSize));

    auto desc = ContentAddressWithReferences::fromParts(
        ContentAddressMethod::Raw::Git, merkle::objectHash(result.root), {.others = {}, .self = false});
    auto dstPath = makeFixedOutputPathFromCA(name, desc);

    addTempRoot(dstPath);

    if (repair || !isValidPath(dstPath)) {
        auto realPath = toRealPath(dstPath);
        PathLocks outputLock({realPath});

        /* The path may have been created by another process in the meantime, so check again. */
        if (repair || !isValidPathUncached(dstPath)) {
            deletePath(realPath);

            try {
                /* movePath and not renameFile because at this point the top-level directory is
                   read-only. */
                movePath(tempPath, realPath);
            } catch (const SystemError & e) {
                if (!e.is(std::errc::cross_device_link))
                    throw;
                /* As `addToStoreFromDump`: copied plainly, then entered by
                   the one walk. */
                warn("can't rename %s as %s, copying instead", PathFmt(tempPath), PathFmt(realPath));
                RestoreSink copySink{/*startFsync=*/false, /*hooks=*/hooks.get()};
                copySink.dstPath = realPath;
                copyRecursive(*makeFSSourceAccessor(tempPath), CanonPath::root, copySink, CanonPath::root);
                delTempDir.deletePath();
                result = enterPath(realPath, repair, hold);
            }

            if (localSettings.fsyncStorePaths) {
                recursiveSync(realPath);
                syncParent(realPath);
            }

            auto info = ValidPathInfo::makeFromCA(*this, name, std::move(desc), ObjectHash::of(result.root));
            info.narSize = result.narSize;
            registerValidPath(info, hold);
        } else
            // We may have a negative cache entry for this path, so get rid of it.
            invalidatePathInfoCacheFor(dstPath);

        outputLock.setDeletion(true);
    }

    return dstPath;
}

std::pair<ObjectHashSink::Result, std::vector<std::filesystem::path>>
LocalStore::verifyObjects(const std::filesystem::path & realPath)
{
    VerifyVisitor verify{objects, realPath};
    auto result = objectHashOf(*makeFSSourceAccessor(realPath), CanonPath::root, defaultPathFilter, verify);
    /* The synthetic root tree of a bare executable or symlink, through
       which its blob is reachable (`putRootTree`). */
    if (auto body = merkle::syntheticRootTree(result.root))
        verify.expect(objects.treeFile(merkle::treeId(*body)));
    return {result, std::move(verify.missing)};
}

ObjectHashSink::Result LocalStore::enterIntoObjects(
    const std::filesystem::path & path,
    RepairFlag repair,
    const GitObjectStore::Hold & hold,
    Activity * act,
    OptimiseStats * stats,
    GitObjectStore::BlobInodes * blobInodes)
{
    /* The on-disk walk is an ingestion too -- the flat add, the `EXDEV`
       copy, the after-build walk, `nix-store --optimise`'s re-entry -- and it
       records the same rows, so it opens the same ingestion: the root is the
       path it walks, and the caller names it when the path is registered
       (`endIngestion`).  Each call opens its own, so a hold shared by several
       outputs does not attribute one output's holders to another's path. */
    beginIngestion(hold, path);
    EnterVisitor enter{*this, hold, path, repair, blobInodes, stats, act};
    auto result = objectHashOf(*makeFSSourceAccessor(path), CanonPath::root, defaultPathFilter, enter);
    objects.putRootTree(result.root, hold);
    return result;
}

void LocalStore::optimiseStore(OptimiseStats & stats)
{
    Activity act(*logger, actOptimiseStore);

    auto paths = queryAllValidPaths();
    auto blobInodes = objects.blobInodes();

    act.progress(0, paths.size());

    uint64_t done = 0;

    for (auto & i : paths) {
        addTempRoot(i);
        auto row = retrySQLite<std::shared_ptr<const ValidPathInfo>>(
            [&]() { return queryPathInfoInternal(*_state->lock(), i); });
        if (!row)
            continue; /* path was GC'ed, probably */
        Activity act(*logger, lvlTalkative, actUnknown, fmt("optimising path '%s'", printStorePath(i)));
        /* A row an older Nix wrote (01 §9.11, "The database") is migrated
           first, by the walk `nix store migrate` makes: the NAR hash it
           asserts is checked on the tee before its column is written
           (`walkOldRow`; 01 §10, *a schema-10 row's migration checks the NAR
           hash the row asserts*), so a path modified since is
           neither blessed nor entered -- it is reported as the verifier
           reports it and left to `--verify --check-contents --repair`.  Such
           a path is read twice here, once to migrate and once to enter; a
           store this Nix wrote has no such row. */
        if (!row->objectHash) {
            ValidPathInfo old(*row);
            std::optional<std::pair<Hash, Hash>> narHashes;
            try {
                switch (walkOldRow(old, &narHashes)) {
                case OldRowWalk::Migrated:
                    break;
                case OldRowWalk::Modified:
                    printError(
                        "path '%s' was modified! expected hash '%s', got '%s'; not entered -- run `nix-store --verify --check-contents --repair`",
                        printStorePath(i),
                        narHashes->first.to_string(HashFormat::Nix32, true),
                        narHashes->second.to_string(HashFormat::Nix32, true));
                    continue;
                case OldRowWalk::FilesMissing:
                    printError(
                        "path '%s' is registered but its files are missing; run `nix-store --verify` to remove the row",
                        printStorePath(i));
                    continue;
                }
            } catch (InvalidPath &) {
                continue; /* collected meanwhile */
            }
            stats.rowsMigrated++;
        }
        /* Every path is walked; a path entered before costs its directory
           walk and no more, since its files' inodes are in `blobInodes`.
           On a store this Nix wrote that is every path.  The path is
           valid, so its root is live already; the trees written beneath it
           are held until the root is (`Hold`; `08` K16), since no GC lock
           is taken here and a collection may sweep meanwhile. */
        auto hold = objects.hold();
        auto pathRoot = config->realStoreDir.get() / i.to_string();
        enterIntoObjects(pathRoot, NoRepair, hold, &act, &stats, &blobInodes);
        /* `nix-store --optimise` is what re-establishes an index a
           collection or an older Nix left incomplete. */
        endIngestion(hold, i);
        done++;
        act.progress(done, paths.size());
    }

    removeLegacyLinks();
}

void LocalStore::removeLegacyLinks()
{
    auto links = config->realStoreDir.get() / ".links";
    if (!pathExists(links))
        return;
    printInfo("removing the legacy '.links' directory");
    deletePath(links);
}

void LocalStore::optimiseStore()
{
    OptimiseStats stats;

    optimiseStore(stats);

    /* A no-op reads all zeros; a migration says what it entered and wrote. */
    printInfo(
        "%s freed by hard-linking %d files; %d files entered as new objects; %d paths migrated to the object hash",
        renderSize(stats.bytesFreed),
        stats.filesLinked,
        stats.filesEntered,
        stats.rowsMigrated);
}

ObjectHashSink::Result
LocalStore::enterPath(const std::filesystem::path & path, RepairFlag repair, const GitObjectStore::Hold & hold)
{
    return enterIntoObjects(path, repair, hold);
}

} // namespace nix
