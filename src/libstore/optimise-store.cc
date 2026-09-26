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
#include "nix/util/thread-pool.hh"

#include <algorithm>
#include <boost/container/small_vector.hpp>
#include <map>
#include <mutex>
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
   -- records a holder or shares an existing one -- or leaves it as written, and why:
   the exceptions of 01 §9.10 law 3 that can be read off the file.  Decided
   here alone: the on-disk walk skips such a file, and the
   verifier expects no holder for it. (The restore route asks
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
        objects.putObject(node.entry, target, hold);
        return node;
    }

    HashedNode directory(const CanonPath & path, Children children) override
    {
        auto [node, body] = hashDirectory(path, std::move(children));
        objects.putObject(node.entry, body, hold);
        return node;
    }
};

/* The visitor for a store path already on disk at `root`: a regular file
   whose inode is a current holder's (`inodes` supplies hints) is not read; every other
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

    HashedNode node(const CanonPath & path, const SourceAccessor::Stat & st, fun<HashedNode()> descend) override
    {
        if (auto result = tryKnown(path, st))
            return std::move(*result);
        return descend();
    }

    std::optional<HashedNode> tryKnown(const CanonPath & path, const SourceAccessor::Stat & st)
    {
        if (!inodes || repair || st.type != SourceAccessor::tRegular)
            return std::nullopt;
        auto where = onDisk(root, path);
        auto file = lstat(where);
        auto i = inodes->find(file.st_ino);
        if (i == inodes->end() || !linkableOnThisSystem(where))
            return std::nullopt;
        /* A preloaded inode can have lost its holder, been replaced, or
           belong to another device. The map only suggests a hash to query;
           a current canonical holder of this exact inode is the witness. */
        auto holder = objects.holderOrForget(i->second, st.isExecutable, NoRepair);
        if (!holder || !GitObjectStore::validHolderMetadata(*holder, st.isExecutable, file))
            return std::nullopt;
        auto node = HashedNode{
            .entry = {.mode = st.isExecutable ? merkle::Mode::Executable : merkle::Mode::Regular, .hash = i->second},
            .narSize = merkle::nar::regular(static_cast<uint64_t>(file.st_size), st.isExecutable),
        };
        /* Keep this path's own witness if the queried donor is collected.
           If an earlier file won this ingestion's claim, use the existing
           placement operation to resolve a different inode before return. */
        if (auto first = hold.addRow({
                .id = node.entry.hash,
                .executable = st.isExecutable,
                .ino = file.st_ino,
                .dev = file.st_dev,
                .size = static_cast<uint64_t>(file.st_size),
                .name = std::string(path.rel()),
            })) {
            auto earlier = maybeLstat(std::filesystem::path(*first));
            if (!earlier || earlier->st_ino != file.st_ino || earlier->st_dev != file.st_dev) {
                auto parent = where.parent_path();
                WritableDirectory dir(parent, parent != store.config->realStoreDir.get());
                auto name = CanonPath::fromFilename(where.filename().string());
                placeRegularFile(objects, where, node.entry, dir.fd.get(), name, repair, hold, inodes, stats, act);
            }
        }
        return node;
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

struct BufferedRegularFile
{
    HashedNode node;
    std::string buffer;
    std::unique_ptr<RegularFileWriter> writer;
};

/* Hash one observation and retain its bytes until the existing bound. The
   destination need not exist until the first streamed write. */
static BufferedRegularFile readBufferedRegular(
    const CanonPath & path,
    fun<void(CreateRegularFileSink &)> read,
    fun<std::unique_ptr<RegularFileWriter>()> open,
    Sink * contents = nullptr)
{
    struct BufferThenStreamFile : CreateRegularFileSink
    {
        const fun<std::unique_ptr<RegularFileWriter>()> & open;
        CreateRegularFileSink & hashing;
        Sink * contents;
        bool executable = false;
        std::string buffer;
        std::unique_ptr<RegularFileWriter> writer;

        BufferThenStreamFile(
            const fun<std::unique_ptr<RegularFileWriter>()> & open, CreateRegularFileSink & hashing, Sink * contents)
            : open(open)
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
            writer = open();
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

    std::string buffer;
    std::unique_ptr<RegularFileWriter> writer;
    auto readAndBuffer = [&](CreateRegularFileSink & hashing) {
        BufferThenStreamFile file{open, hashing, contents};
        read(file);
        buffer = std::move(file.buffer);
        writer = std::move(file.writer);
    };
    auto node = HashingVisitor{}.regular(path, std::ref(readAndBuffer));
    return {std::move(node), std::move(buffer), std::move(writer)};
}

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

    /* Only the current sink and optional physical child name are needed.
       Their owners enclose every accepted visit, including pool joins.
       C++ scopes restore the previous pair for recursion, helped jobs and
       reentrant visitors; each thread has its own current pair. */
    struct Frame
    {
        RestoreSink * sink = nullptr;
        const std::string * diskName = nullptr;
    };

    static Frame & currentFrame() noexcept
    {
        static thread_local Frame f;
        return f;
    }

    struct Scope
    {
        Frame saved;

        Scope(RestoreSink & sink, const std::string * diskName) noexcept
            : saved(currentFrame())
        {
            currentFrame() = {&sink, diskName};
        }

        ~Scope()
        {
            currentFrame() = saved;
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
        , scope(restore, nullptr)
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

    Target at(const CanonPath & path)
    {
        auto & frame = currentFrame();
        auto & sink = *frame.sink;
        /* Pull children supply their case-hacked name; push paths already
           carry the NAR restorer's physical spelling. */
        auto name = path.isRoot()
                        ? path
                        : CanonPath::root / (frame.diskName ? std::string_view(*frame.diskName) : *path.baseName());
        return {sink, name, name.isRoot() ? sink.dstPath : sink.dstPath / name.rel()};
    }

    HashedNode directory(const CanonPath & path, Children children) override
    {
        auto target = at(path);
        return ObjectStoreVisitor::directory(path, [&](fun<void(std::string_view, bool, fun<HashedNode()>)> each) {
            target.sink.createSubdirectory(target.name, [&](RestoreSink & sub, const CanonPath & rel) {
                assert(rel.isRoot());
                Scope scope{sub, currentFrame().diskName};
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
        auto target = at(path);
        return placeRegular(
            target,
            readBufferedRegular(
                path,
                std::move(read),
                [&] { return target.sink.beginRegularFile(target.name); },
                path.isRoot() ? rootContents : nullptr));
    }

    HashedNode placeRegular(const Target & target, BufferedRegularFile file)
    {
        auto & node = file.node;
        auto & buffer = file.buffer;
        auto & writer = file.writer;
        bool executable = node.entry.mode == merkle::Mode::Executable;
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
            return std::move(file.node);
        }

        /* The whole file is in memory and its blob known.  A hit: the
           store's valid file linked under the name, nothing written. */
        if (linkable && objects.link(entry.hash, executable, dirFd, name, target.where, buffer.size(), repair, hold))
            return std::move(file.node);

        /* A miss: written from memory, canonicalised by the restore sink's
           hooks, and entered as the store's file -- or, should another
           writer have entered the blob meanwhile, replaced by its file. */
        {
#ifdef _WIN32
            auto written = target.sink.beginRegularFile(target.name);
#else
            auto written = target.sink.beginRegularFileAt(target.name, dirFd, name);
#endif
            if (executable)
                written->isExecutable();
            written->preallocateContents(buffer.size());
            /* The complete file is already buffered, and this writer is fresh. */
            auto & sink = dynamic_cast<FdSink &>(*written);
            assert(!sink.hasData());
            sink.writeUnbuffered(buffer);
            written->finish();
        }
        if (linkable)
            objects.enterWritten(entry.hash, executable, dirFd, name, target.where, repair, hold);
        return std::move(file.node);
    }
};

#ifndef _WIN32
/* The accessor copy (01 §9.10; 08 §1.6). A node either makes a
   verified immutable file or named subtree from existing objects, or
   descends through LinkingVisitor. The namer is consulted for the root
   and directories only; raw unnamed files are read and hashed. Directory
   entries carry tree names, while the child scope borrows its case-hacked
   name for destination placement. */
struct MaterialisingVisitor : LinkingVisitor
{
    const SourcePath & source;
    LocalStore::TreeNamer namer;
    RestoreSinkHooks * hooks;
    std::function<void(const std::filesystem::path &, const GitObjectStore::Hold &)> commitAttempt;

    /* A caller-supplied naming callback need not permit simultaneous
       calls. The directory walk and holder writes run outside this lock. */
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
        const SourcePath & source,
        decltype(commitAttempt) commitAttempt)
        : LinkingVisitor(objects, hold, restore, repair)
        , source(source)
        , namer(std::move(namer))
        , hooks(hooks)
        , commitAttempt(std::move(commitAttempt))
        , workers(workers)
    {
    }

    HashedNode directory(const CanonPath & path, Children children) override
    {
        auto copy = [&](fun<void(std::string_view, bool, fun<HashedNode()>)> each) {
            copyChildren(path, std::move(children), each);
        };
        return LinkingVisitor::directory(path, std::ref(copy));
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
        if (workers == 1) {
            /* A filter may evaluate Nix or throw. Visit immediately, on
               the caller's thread, before asking it about the next sibling. */
            CaseHackNames hack;
            children([&](std::string_view name, bool isDirectory, fun<HashedNode()> visit) {
                auto disk = hack.diskName(name);
                Scope scope{*currentFrame().sink, &disk};
                each(name, isDirectory, std::move(visit));
            });
            return;
        }

        struct Child
        {
            std::string disk;
            size_t nameSize;
            fun<HashedNode()> visit;
            bool subdirectory;
            std::optional<HashedNode> node;
        };

        boost::container::small_vector<
            Child,
            1,
            void,
            boost::container::small_vector_options_t<
                boost::container::growth_factor<boost::container::growth_factor_100>>>
            kids;
        CaseHackNames hack;
        children([&](std::string_view name, bool isDirectory, fun<HashedNode()> visit) {
            /* The disk name is the tree name plus an optional suffix. */
            kids.push_back({hack.diskName(name), name.size(), std::move(visit), isDirectory, std::nullopt});
        });
        /* A singleton can still find parallel work below it. Construct
           the shared scheduler only at the first branching directory. */
        if (!pool && kids.size() > 1 && std::any_of(kids.begin(), kids.end(), [](auto & c) { return c.subdirectory; }))
            pool = std::make_unique<CopyPool>(workers);
        auto * sink = currentFrame().sink;
        auto run = [&](size_t i, bool) {
            auto & c = kids[i];
            Scope scope{*sink, &c.disk};
            c.node = c.visit();
        };
        auto consume = [&](size_t i) {
            auto & c = kids[i];
            assert(c.node);
            each(std::string_view(c.disk).substr(0, c.nameSize), c.subdirectory, [&] { return std::move(*c.node); });
        };
        if (pool)
            pool->ordered(kids.size(), [&](size_t i) { return kids[i].subdirectory; }, run, consume);
        else
            for (size_t i = 0; i < kids.size(); ++i) {
                run(i, false);
                consume(i);
            }
    }

    HashedNode node(const CanonPath & path, const SourceAccessor::Stat & st, fun<HashedNode()> descend) override
    {
        if (auto result = tryKnown(path, st))
            return std::move(*result);
        return descend();
    }

    std::optional<HashedNode> tryKnown(const CanonPath & path, const SourceAccessor::Stat & st)
    {
        /* A verified immutable blob already has an identity before any
           bytes are read. Probe the holder first and stream a miss directly
           into its destination, without buffering and hashing it again.
           Mutable worktrees supply no such identity; repair ignores it. */
        if (!repair && st.type == SourceAccessor::tRegular)
            if (auto entry = source.path.isRoot() ? source.accessor->getKnownEntry(path)
                                                  : source.accessor->getKnownEntry(source.path / path);
                entry && entry->hash.algo == merkle::hashAlgo
                && entry->mode == (st.isExecutable ? merkle::Mode::Executable : merkle::Mode::Regular)) {
                auto target = at(path);
                auto [owned, dirFd, name] = target.sink.parentOf(target.name);
                bool linkable = linkableOnThisSystem(target.where);
                PosixStat linked;
                if (linkable
                    && objects.link(
                        entry->hash, st.isExecutable, dirFd, name, target.where, st.fileSize, repair, hold, &linked))
                    return HashedNode{
                        .entry = *entry,
                        .narSize = merkle::nar::regular(static_cast<uint64_t>(linked.st_size), st.isExecutable)};

                auto writer = target.sink.beginRegularFileAt(target.name, dirFd, name);
                if (st.isExecutable)
                    writer->isExecutable();
                /* Stat need not know a packed blob's size. readFile must
                   announce it before writing, so the copy obtains it only
                   when it actually needs the bytes. */
                std::optional<uint64_t> size;
                source.accessor->readFile(source.path / path, *writer, [&](uint64_t announced) {
                    if (st.fileSize && announced != *st.fileSize)
                        throw Error("immutable file '%s' changed size while copying", source.path / path);
                    size = announced;
                    writer->preallocateContents(announced);
                });
                if (!size)
                    throw Error("immutable file '%s' did not announce its size", source.path / path);
                writer->finish();
                if (auto * fdSink = dynamic_cast<FdSink *>(writer.get()); fdSink && fdSink->written != *size)
                    throw Error("immutable file '%s' delivered the wrong number of bytes", source.path / path);
                writer.reset();
                if (linkable)
                    objects.enterWritten(entry->hash, st.isExecutable, dirFd, name, target.where, repair, hold);
                return HashedNode{.entry = *entry, .narSize = merkle::nar::regular(*size, st.isExecutable)};
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
        /* A directory may fail after creating some files. Its names must
           remain private until the whole attempt succeeds: a sibling cannot
           borrow a pending holder that fallback would subsequently delete. */
        std::optional<GitObjectStore::Hold> attempt;
        if (entry->mode == merkle::Mode::Directory) {
            attempt.emplace(hold.ownRows());
            attempt->setRoot(hold.root());
        }
        std::optional<uint64_t> narSize;
        try {
            narSize = materialise(*entry, dirFd, name, target.where, path.isRoot() ? 0 : 1, attempt ? *attempt : hold);
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
        /* Publishing a completed name ends rollback's authority over it.
           A merge error aborts the ingestion; it never resumes fallback. */
        if (attempt)
            commitAttempt(target.where, *attempt);
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
        size_t depth,
        const GitObjectStore::Hold & materialisationHold)
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
            if (!objects.link(
                    entry.hash, executable, dirFd, name, where, std::nullopt, repair, materialisationHold, &linked))
                return std::nullopt;
            return merkle::nar::regular(static_cast<uint64_t>(linked.st_size), executable);
        }

        case merkle::Mode::Symlink: {
            auto file = objects.blobFile(entry.hash, false);
            if (!objects.holdObject(entry, materialisationHold))
                return std::nullopt;
            auto target = objects.readBlob(entry.hash, false);
            if (!target)
                return std::nullopt;
            if (merkle::blobId(*target) != entry.hash) {
                warn("blob object %s does not hash to its name; not used", PathFmt(file));
                return std::nullopt;
            }
            if (::symlinkat(requireCString(*target), dirFd, name.rel_c_str()) == -1)
                throw SysError("creating symlink from %1% -> '%2%'", PathFmt(where), *target);
            if (hooks)
                hooks->symlinkCreated(dirFd, name);
            return merkle::nar::symlink(target->size());
        }

        case merkle::Mode::Directory: {
            if (depth >= narMaxDepth)
                throw Error("path %s exceeds maximum NAR directory depth of %d", PathFmt(where), narMaxDepth);
            if (!objects.holdObject(entry, materialisationHold))
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
                auto sub = materialise(
                    *child, fd.get(), CanonPath::fromFilename(disk), where / disk, depth + 1, materialisationHold);
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
    const GitObjectStore::Hold & hold,
    bool callbacksPermitParallelism)
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
        workers = CopyPool::defaultWorkers();
    /* Callbacks, filter and source must all permit reordered observations.
       The AllPaths type survives copies of defaultPathFilter. */
    if (!callbacksPermitParallelism || !filter.get_fn().target<AllPaths>()
        || !path.accessor->allowsConcurrentTraversal())
        workers = 1;
    auto commitAttempt = [&](const std::filesystem::path & subtree, const GitObjectStore::Hold & attempt) {
        std::vector<BlobCoalescing> collisions;
        for (auto & row : attempt.takeRows()) {
            auto other = hold.addRow(row);
            if (!other)
                continue;
            auto current = lstat(std::filesystem::path(*other));
            if (current.st_ino == row.ino && current.st_dev == row.dev)
                continue;
            auto own = row.name.empty() ? hold.root() : hold.root() / row.name;
            row.name = own == subtree ? "" : own.lexically_relative(subtree).string();
            collisions.push_back({
                std::move(row),
                {{current.st_ino, current.st_dev, static_cast<uint64_t>(current.st_size), *other}},
            });
        }
        /* A completed attempt may share several aliases of each losing
           inode. Reconcile all of them before returning, using the same
           physical walk as publication; replacing one row is insufficient. */
        if (!collisions.empty()) {
            auto result = coalesceBlobs(subtree, hold, collisions);
            if (!result.rejected.empty())
                throw Error("a completed ingestion holder changed while materialising %s", PathFmt(subtree));
        }
    };
    MaterialisingVisitor visitor{
        objects, hold, restore, repair, std::move(namer), hooks, workers, path, std::move(commitAttempt)};
    beginIngestion(hold, dst);
    auto result = objectHashOf(*path.accessor, path.path, filter, visitor);
    /* Every reused node carries the size of what it made, so the NAR
       size is exact, as the one route's is. */
    assert(result.narSizeExact);
    objects.putRootTree(result.root, hold);
    return result;
#endif
}

StorePath LocalStore::materialise(
    std::string_view name,
    const SourcePath & path,
    PathFilter & filter,
    RepairFlag repair,
    TreeNamer namer,
    bool callbacksPermitParallelism)
{
    if (!materialisesFromObjects())
        throw Error("this store cannot materialise a path from its object store");

    /* Name the bytes observed by this copy. An unnamed regular root can
       remain buffered until its destination is known to need writing;
       every other copy uses the existing locked temporary. Both register
       the same Git address, root entry and NAR size through one path. */
    const LocalSettings & localSettings = config->getLocalSettings();
    auto hooks = makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(localSettings.ignoredAcls));

    std::optional<GitObjectStore::Hold> hold;
    AutoCloseFD tempDirFd;
    std::optional<AutoDelete> delTempDir;
    std::filesystem::path tempPath;
    RestoreSink restore{localSettings.fsyncStorePaths, hooks.get()};
    auto prepare = [&] {
        if (hold)
            return;
        /* Collection precedes every write; a buffered, already valid
           root needs neither collection nor an ingestion's hold. */
        autoGC();
        hold.emplace(objects.hold());
        auto [tempDir, fd] = createTempDirInStore();
        tempDirFd = std::move(fd);
        delTempDir.emplace(tempDir);
        tempPath = tempDir / "x";
        restore.dstPath = tempPath;
    };

    std::optional<BufferedRegularFile> buffered;
    checkInterrupt();
    auto st = path.lstat();
    auto result = [&] {
        if (st.type == SourceAccessor::tRegular) {
            auto entry = repair ? std::nullopt : path.accessor->getKnownEntry(path.path);
            if (entry
                && (entry->hash.algo != merkle::hashAlgo
                    || entry->mode != (st.isExecutable ? merkle::Mode::Executable : merkle::Mode::Regular)))
                entry.reset();
            if (!entry) {
                entry = namer(CanonPath::root, st);
                /* Keep the answer associated with this observation, rather
                   than asking a mutable source's namer a second time. */
                if (entry)
                    namer = [entry, rest = std::move(namer)](const CanonPath & p, const SourceAccessor::Stat & s) {
                        return p.isRoot() ? entry : rest(p, s);
                    };
            }
            if (!entry) {
                buffered.emplace(readBufferedRegular(
                    CanonPath::root,
                    [&](CreateRegularFileSink & sink) {
                        if (st.isExecutable)
                            sink.isExecutable();
                        path.readFile(sink, [&](uint64_t size) { sink.preallocateContents(size); });
                    },
                    [&] {
                        prepare();
                        beginIngestion(*hold, tempPath);
                        return restore.beginRegularFile(CanonPath::root);
                    }));
                return ObjectHashSink::Result::of(buffered->node);
            }
        }
        prepare();
        return materialiseThroughObjects(
            tempPath,
            path,
            filter,
            localSettings.fsyncStorePaths,
            hooks.get(),
            repair,
            std::move(namer),
            *hold,
            callbacksPermitParallelism);
    }();

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

            if (buffered) {
                prepare();
                if (!buffered->writer)
                    beginIngestion(*hold, tempPath);
                LinkingVisitor linking{objects, *hold, restore, repair};
                linking.placeRegular(linking.at(CanonPath::root), std::move(*buffered));
                buffered.reset();
                objects.putRootTree(result.root, *hold);
            }

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
                delTempDir->deletePath();
                result = enterPath(realPath, repair, *hold);
            }

            if (localSettings.fsyncStorePaths) {
                recursiveSync(realPath);
                syncParent(realPath);
            }

            auto info = ValidPathInfo::makeFromCA(*this, name, std::move(desc), ObjectHash::of(result.root));
            info.narSize = result.narSize;
            registerValidPath(info, *hold);
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

LocalStore::BlobCoalescingResult LocalStore::coalesceBlobs(
    const std::filesystem::path & root,
    const GitObjectStore::Hold & hold,
    const std::vector<BlobCoalescing> & collisions)
{
#ifdef _WIN32
    return {};
#else
    /* Only overlapping misses need this walk. Publication reserves the
       database writer before selecting registered donors; speculative-copy
       completion supplies stable names already owned by its ingestion.
       Pin first, then validate the pinned inode and its bytes. Nothing here
       calls the index, takes a temp root, or acquires the auxiliary-object lock. */
    struct Winner
    {
        const GitObjectStore::Hold::Row & row;
        const GitObjectStore::Holder & holder;
        std::filesystem::path pin;
        AutoDelete cleanup;
        std::optional<std::filesystem::path> representative;
    };

    std::vector<Winner> winners;
    winners.reserve(collisions.size());
    BlobCoalescingResult result;
    std::map<std::pair<dev_t, ino_t>, size_t> byInode;
    std::map<std::pair<dev_t, ino_t>, size_t> byWinnerInode;
    auto temporary = [&] { return hold.directory() / objects.tempPath("coalesce").filename(); };

    for (auto & collision : collisions) {
        auto & row = collision.row;
        bool selected = false;
        bool refused = false;
        for (auto & candidate : collision.candidates) {
            if (candidate.size != row.size) {
                result.rejected.emplace_back(row, candidate);
                continue;
            }
            auto before = maybeLstat(std::filesystem::path(candidate.file));
            if (!before || !GitObjectStore::validHolderMetadata(candidate, row.executable, *before)) {
                result.rejected.emplace_back(row, candidate);
                continue;
            }
            if (candidate.ino == row.ino && candidate.dev == row.dev) {
                /* The owned target already pins this inode. No additional
                   link is needed, including at the filesystem's link limit. */
                auto own = row.name.empty() ? root : root / row.name;
                auto current = lstat(own);
                if (!GitObjectStore::validHolderMetadata(candidate, row.executable, current))
                    throw Error("ingestion holder %s changed before publication", PathFmt(own));
                auto observed = GitObjectStore::blobIdOfFile(own);
                if (observed != row.id)
                    throw Error(
                        "cannot publish ingestion holder %s: expected blob %s, observed %s",
                        PathFmt(own),
                        GitObjectStore::idString(row.id),
                        GitObjectStore::idString(observed));
                selected = true;
                result.representatives.push_back(row);
                break;
            }
            auto pin = temporary();
            std::error_code ec;
            if (objects.failPlaceWithNoSpace && objects.failPlaceWithNoSpace(pin))
                ec = std::make_error_code(std::errc::no_space_on_device);
            else
                std::filesystem::create_hard_link(candidate.file, pin, ec);
            if (ec) {
                if (ec == std::errc::no_such_file_or_directory) {
                    result.rejected.emplace_back(row, candidate);
                    continue;
                }
                if (ec == std::errc::too_many_links || ec == std::errc::no_space_on_device) {
                    refused = true;
                    continue;
                }
                /* A replacement by a directory can make link fail with
                   EPERM. Judge that stale candidate, not the link error. */
                auto now = maybeLstat(std::filesystem::path(candidate.file));
                if (!now || !GitObjectStore::validHolderMetadata(candidate, row.executable, *now)) {
                    result.rejected.emplace_back(row, candidate);
                    continue;
                }
                throw Error(
                    "pinning blob holder %1% as %2%: %3%",
                    PathFmt(std::filesystem::path(candidate.file)),
                    PathFmt(pin),
                    ec.message());
            }
            AutoDelete cleanup(pin, false);
            auto pinned = lstat(pin);
            if (!GitObjectStore::validHolderMetadata(candidate, row.executable, pinned)
                || GitObjectStore::blobIdOfFile(pin) != row.id) {
                result.rejected.emplace_back(row, candidate);
                continue;
            }
            selected = true;
            auto [_, inserted] = byInode.emplace(std::pair{row.dev, row.ino}, winners.size());
            if (!inserted)
                throw Error("one ingestion records different blobs for the same inode in %s", PathFmt(root));
            byWinnerInode.emplace(std::pair{candidate.dev, candidate.ino}, winners.size());
            winners.push_back(Winner{row, candidate, std::move(pin), std::move(cleanup), std::nullopt});
            break;
        }
        /* No usable donor means this file can become the holder. A file
           refused sharing is an exception, not a new canonical donor. */
        if (!selected && !refused)
            result.representatives.push_back(row);
    }
    if (winners.empty())
        return result;

    /* Enumerate physical aliases, not names reconstructed from the Merkle
       tree. Entered paths can contain noncanonical case-hack suffixes, and
       (device, inode) already proves which regular files are aliases. The
       final path is authoritative: hold.root() can name a moved temporary. */
    auto walk = [&](auto && self, const std::filesystem::path & where, size_t depth) -> void {
        checkInterrupt();
        auto st = lstat(where);
        if (S_ISDIR(st.st_mode)) {
            if (depth >= narMaxDepth)
                throw Error("path %s exceeds maximum NAR directory depth of %d", PathFmt(where), narMaxDepth);
            /* Replacing a directory entry while its iterator is live has
               unspecified visibility. Finish enumerating before relinking. */
            std::vector<std::filesystem::path> children;
            for (auto & entry : DirectoryIterator{where})
                children.push_back(entry.path());
            for (auto & child : children)
                self(self, child, depth + 1);
            return;
        }
        if (!S_ISREG(st.st_mode))
            return;
        auto i = byInode.find({st.st_dev, st.st_ino});
        if (!linkableOnThisSystem(where))
            return;
        if (i == byInode.end()) {
            /* A prior partially completed attempt may already have linked
               an alias even when the old representative still lost. */
            auto existing = byWinnerInode.find({st.st_dev, st.st_ino});
            if (existing != byWinnerInode.end()) {
                auto & winner = winners[existing->second];
                if (!winner.representative
                    && GitObjectStore::validHolderMetadata(winner.holder, winner.row.executable, st))
                    winner.representative = where;
            }
            return;
        }
        auto & winner = winners[i->second];
        auto & row = winner.row;
        auto expected = GitObjectStore::Holder{row.ino, row.dev, row.size, where.string()};
        if (!GitObjectStore::validHolderMetadata(expected, row.executable, st))
            return;
        if (objects.failPlaceWithNoSpace && objects.failPlaceWithNoSpace(where))
            return;

        auto parent = where.parent_path();
        WritableDirectory dir(parent, parent != config->realStoreDir.get());
        auto name = CanonPath::fromFilename(where.filename().string());
        /* Re-check after opening the containing directory, so a pathname
           replaced since enumeration is never intentionally overwritten. */
        auto before = statIn(dir.fd.get(), name, where);
        if (!GitObjectStore::validHolderMetadata(expected, row.executable, before))
            return;
        auto link = temporary();
        std::error_code ec;
        std::filesystem::create_hard_link(winner.pin, link, ec);
        if (ec) {
            if (ec == std::errc::too_many_links || ec == std::errc::no_space_on_device) {
                printInfo("cannot share %s: %s; left as written", PathFmt(where), ec.message());
                return;
            }
            throw Error("creating hard link from %1% to %2%: %3%", PathFmt(winner.pin), PathFmt(link), ec.message());
        }
        AutoDelete cleanup(link, false);
        if (::renameat(AT_FDCWD, link.c_str(), dir.fd.get(), name.rel_c_str()) == -1) {
            auto ec = std::error_code(errno, std::generic_category());
            if (ec == std::errc::too_many_links || ec == std::errc::no_space_on_device) {
                printInfo("cannot share %s: %s; left as written", PathFmt(where), ec.message());
                return;
            }
            throw Error("replacing %1% by a link to %2%: %3%", PathFmt(where), PathFmt(winner.pin), ec.message());
        }
        cleanup.cancel();
        result.modified = true;
        if (!winner.representative)
            winner.representative = where;
    };
    walk(walk, root, 0);

    /* A link limit can leave some aliases unchanged. Publish a surviving
       winning alias, or no representative if every replacement was refused;
       an exceptional unshared inode must not become a donor for later paths. */
    for (auto & winner : winners) {
        auto & row = winner.row;
        if (!winner.representative)
            continue;
        auto st = maybeLstat(*winner.representative);
        if (!st || !GitObjectStore::validHolderMetadata(winner.holder, row.executable, *st))
            continue;
        GitObjectStore::Hold::Row replacement{
            .id = row.id,
            .executable = row.executable,
            .ino = st->st_ino,
            .dev = st->st_dev,
            .size = static_cast<uint64_t>(st->st_size),
            .name = *winner.representative == root ? "" : winner.representative->lexically_relative(root).string(),
        };
        result.representatives.push_back(std::move(replacement));
    }
    return result;
#endif
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
        auto pathRoot = toRealPath(i);
        PathLocks outputLock;
        if (!locksHeld.count(printStorePath(i)))
            outputLock.lockPaths({pathRoot});
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
        /* Every path is walked; an inode hint that still has a matching
           canonical holder avoids reading that regular file's contents.
           The path is valid, so its root is live already; the trees written beneath it
           are held until the root is (`Hold`; `08` K16), since no GC lock
           is taken here and a collection may sweep meanwhile. */
        auto hold = objects.hold();
        enterIntoObjects(pathRoot, NoRepair, hold, &act, &stats, &blobInodes);
        /* `nix-store --optimise` is what re-establishes an index a
           collection or an older Nix left incomplete. */
        endIngestion(hold, i);
        outputLock.setDeletion(true);
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
