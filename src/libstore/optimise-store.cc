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
#include "nix/util/sync.hh"

#include <algorithm>
#include <ranges>
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

/* Physical eligibility shared by entry and verification. Writable and
   platform-forbidden files stay unshared. ENOSPC/EMLINK leave no metadata
   marker, so verification reports a missing holder and repair retries. */
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
    const std::filesystem::path * path = nullptr;
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
            this->path = path.empty() ? nullptr : &path;
        }
    }

    ~WritableDirectory()
    {
        try {
            /* Restore the directory we opened, including its canonical timestamp. */
            if (path)
#ifndef _WIN32
                canonicaliseTimestampAndPermissions(fd.get(), true);
#else
                canonicaliseTimestampAndPermissions(*path);
#endif
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

/* Place the written, canonicalised regular file in its temporarily writable parent;
   the walk's optional observers record its resulting inode and sharing work. */
static void placeRegularFile(
    LocalStore & store,
    const std::filesystem::path & where,
    const merkle::TreeEntry & entry,
    RepairFlag repair,
    const GitObjectStore::Hold & hold,
    GitObjectStore::BlobInodes * inodes = nullptr,
    OptimiseStats * stats = nullptr,
    Activity * act = nullptr)
{
    auto parent = where.parent_path();
    WritableDirectory dir(parent, parent != store.config->realStoreDir.get());
    auto name = CanonPath::fromFilename(where.filename().string());
    bool executable = entry.mode == merkle::Mode::Executable;

    auto dirFd = dir.fd.get();
    auto before = statIn(dirFd, name, where);

    auto after = store.objects.place(entry.hash, executable, dirFd, name, where, before, repair, hold);
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

/* Enter a materialised tree. Inode hints skip reads only with a current
   physical witness; all other eligible files are hashed and placed. */
struct EnterVisitor : HashingVisitor
{
    LocalStore & store;
    /* The caller retains this hold through traversal and publication. */
    const GitObjectStore::Hold & hold;
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
        : HashingVisitor(1)
        , store(store)
        , hold(hold)
        , root(std::move(root))
        , repair(repair)
        , inodes(inodes)
        , stats(stats)
        , act(act)
    {
    }

    HashedNode symlink(const CanonPath & path, const std::string & target) override
    {
        auto node = HashingVisitor::symlink(path, target);
        hold.addAuxiliary({node.entry, target});
        return node;
    }

    HashedNode directory(const CanonPath & path, Children children) override
    {
        std::string body;
        auto node = hashDirectory(path, std::move(children), body, *this);
        hold.addAuxiliary({node.entry, std::move(body)});
        return node;
    }

    HashedNode node(
        SourceAccessor &,
        const CanonPath &,
        const CanonPath & path,
        const SourceAccessor::Stat & st,
        function_ref<HashedNode()> descend) override
    {
        if (st.type != SourceAccessor::tRegular)
            return descend();
        if (inodes && !repair) {
            auto where = onDisk(root, path);
            auto file = lstat(where);
            auto i = inodes->find(file.st_ino);
            if (i != inodes->end() && linkableOnThisSystem(where)) {
                /* A preloaded inode can have lost its holder, been replaced, or
                   belong to another device. The map only suggests a hash to query;
                   a current canonical holder of this exact inode is the witness. */
                auto holder = store.objects.holderOrForget(i->second, st.isExecutable, NoRepair);
                if (holder && GitObjectStore::validHolderMetadata(*holder, st.isExecutable, file)) {
                    auto node = HashedNode{
                        .entry = {.mode = st.isExecutable ? merkle::Mode::Executable : merkle::Mode::Regular, .hash = i->second},
                        .narSize = merkle::nar::regular(static_cast<uint64_t>(file.st_size), st.isExecutable),
                    };
                    /* Keep this path's own witness if the queried donor is collected.
                       If an earlier file won this ingestion's claim, use the existing
                       placement operation to resolve a different inode before return. */
                    GitObjectStore::Hold::Row row{
                            .id = node.entry.hash,
                            .executable = st.isExecutable,
                            .ino = file.st_ino,
                            .dev = file.st_dev,
                            .size = static_cast<uint64_t>(file.st_size),
                            .name = std::string(path.rel()),
                        };
                    if (auto first = hold.addRow(std::move(row))) {
                        auto earlier = maybeLstat(*first);
                        if (!earlier || earlier->st_ino != file.st_ino || earlier->st_dev != file.st_dev) {
                            placeRegularFile(store, where, node.entry, repair, hold, inodes, stats, act);
                        } else
                            hold.appendNames(std::move(row));
                    }
                    return node;
                }
            }
        }
        auto node = descend();
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

        placeRegularFile(store, where, node.entry, repair, hold, inodes, stats, act);
        return node;
    }
};

/* Check physical hashes and availability of every eligible reached object.
   Diagnostics use physical spellings, including case-hacked names. */
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

    void expect(const merkle::TreeEntry & entry, const std::filesystem::path & path)
    {
        /* Old read-only stores may lack the auxiliary provider. Still hash their
           physical trees; report missing metadata by its store-node path. */
        if (objects.index && objects.index->readAuxiliary && !objects.readAuxiliary(entry))
            missing.push_back(path);
    }

    HashedNode node(
        SourceAccessor &,
        const CanonPath &,
        const CanonPath & path,
        const SourceAccessor::Stat & st,
        function_ref<HashedNode()> descend) override
    {
        auto node = descend();
        auto where = onDisk(root, path);
        if (st.type == SourceAccessor::tRegular) {
            /* The bytes were checked by this walk; regular presence adds
               a valid holder with the same executable bit. */
            if (placementOf(where) == Placement::Placed
                && !objects.holderOrForget(node.entry.hash, node.entry.mode == merkle::Mode::Executable, NoRepair))
                missing.push_back(std::move(where));
        } else
            expect(node.entry, where);
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
    function_ref<void(CreateRegularFileSink &)> read,
    function_ref<std::unique_ptr<RegularFileWriter>()> open,
    Sink * contents = nullptr)
{
    struct BufferThenStreamFile : HashingRegularFileSink
    {
        const function_ref<std::unique_ptr<RegularFileWriter>()> & open;
        Sink * contents;
        std::string buffer;
        std::unique_ptr<RegularFileWriter> writer;

        BufferThenStreamFile(
            const CanonPath & path, const function_ref<std::unique_ptr<RegularFileWriter>()> & open, Sink * contents)
            : HashingRegularFileSink(path)
            , open(open)
            , contents(contents)
        {
        }

        void isExecutable() override
        {
            HashingRegularFileSink::isExecutable();
            if (writer)
                writer->isExecutable();
        }

        void preallocateContents(uint64_t size) override
        {
            HashingRegularFileSink::preallocateContents(size);
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
            HashingRegularFileSink::operator()(data);
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

    BufferThenStreamFile file{path, open, contents};
    read(file);
    return {file.finish(), std::move(file.buffer), std::move(file.writer)};
}

/* Buffer regular files up to restoreBufferLimit, then link or write;
   larger files stream before placement. Linked files fire no write hook.
   Directory scopes retain child sinks and canonicalise before staging the body. */
struct LinkingVisitor : HashingVisitor
{
    GitObjectStore & objects;
    const GitObjectStore::Hold & hold;
    RepairFlag repair;
    Sink * rootContents;

    RestoreSink & sink;
    const std::string * diskName = nullptr;

#ifndef _WIN32
    struct Naming
    {
        using Lock = std::lock_guard<std::mutex>;
        SyncBase<LocalStore::TreeNamer, std::mutex, Lock, Lock, std::condition_variable> namer;
        function_ref<void(const std::filesystem::path &, const GitObjectStore::Hold &)> commitAttempt;

        Naming(LocalStore::TreeNamer namer, decltype(commitAttempt) commitAttempt)
            : namer(std::move(namer)), commitAttempt(commitAttempt) {}
    };
    Naming * naming = nullptr;
#endif

    LinkingVisitor(
        GitObjectStore & objects,
        const GitObjectStore::Hold & hold,
        RestoreSink & restore,
        RepairFlag repair,
        Sink * rootContents = nullptr,
        size_t workers = 1)
        : HashingVisitor(workers), objects(objects), hold(hold)
        , repair(repair)
        , rootContents(rootContents)
        , sink(restore)
    {
    }

    LinkingVisitor(LinkingVisitor & parent, RestoreSink & sink, const std::string * diskName = nullptr)
        : HashingVisitor(parent), objects(parent.objects), hold(parent.hold)
        , repair(parent.repair), rootContents(parent.rootContents), sink(sink), diskName(diskName)
#ifndef _WIN32
        , naming(parent.naming)
#endif
    {}

    /* Where the node at `path` goes: the innermost open sink, under `/name`
       (the root node under `/`). */
    struct Target
    {
        RestoreSink & sink;
        CanonPath name;

        std::filesystem::path path() const
        {
            return name.isRoot() ? sink.dstPath : sink.dstPath / name.rel();
        }
    };

    Target at(const CanonPath & path)
    {
        if (path.isRoot())
            return {sink, path};
        return {sink, CanonPath::root / (diskName ? std::string_view(*diskName) : *path.baseName())};
    }

    HashedNode directory(const CanonPath & path, Children children) override
    {
        auto target = at(path);
        std::optional<HashedNode> result;
        std::string body;
        auto populate = [&](RestoreSink & sub, const CanonPath & rel) {
            assert(rel.isRoot());
            LinkingVisitor child{*this, sub};
#ifndef _WIN32
            if (naming && isCaseHackEnabled()) {
                auto named = [&](Children::Each each) {
                    CaseHackNames hack;
                    children.each([&](std::string_view name, bool directory, Visit visit) {
                        each(name, directory, [disk = hack.diskName(name), visit = std::move(visit)](auto & consumer, bool mayFork) {
                            auto & parent = static_cast<LinkingVisitor &>(consumer);
                            LinkingVisitor target{parent, parent.sink, &disk};
                            return visit(target, mayFork);
                        });
                    });
                };
                result = hashDirectory(path, {named, children.canDefer, children.mayFork}, body, child);
            } else
#endif
                result = hashDirectory(path, children, body, child);
        };
        target.sink.createSubdirectory(target.name, std::ref(populate));
        assert(result);
        hold.addAuxiliary({result->entry, std::move(body)});
        return std::move(*result);
    }

    HashedNode symlink(const CanonPath & path, const std::string & target) override
    {
        auto t = at(path);
        t.sink.createSymlink(t.name, target);
        auto node = HashingVisitor::symlink(path, target);
        hold.addAuxiliary({node.entry, target});
        return node;
    }

    HashedNode regular(const CanonPath & path, function_ref<void(CreateRegularFileSink &)> read) override
    {
        auto target = at(path);
        return placeRegular(
            target,
            readBufferedRegular(
                path,
                read,
                [&] {
                    return std::unique_ptr<RegularFileWriter>{
                        new RegularFileWriter(target.sink.beginRegularFile(target.name))};
                },
                path.isRoot() ? rootContents : nullptr));
    }

    HashedNode placeRegular(const Target & target, BufferedRegularFile file)
    {
        auto where = target.path();
        auto & node = file.node;
        auto & buffer = file.buffer;
        auto & writer = file.writer;
        bool executable = node.entry.mode == merkle::Mode::Executable;
        auto & entry = node.entry;

        bool linkable = linkableOnThisSystem(where);
        if (!linkable)
            debug("%s is not allowed to be linked in macOS", PathFmt(where));

#ifdef _WIN32
        /* Nothing is linked or placed there (git-object-store.hh). */
        AutoCloseFD owned;
        Descriptor dirFd = INVALID_DESCRIPTOR;
        CanonPath name = CanonPath::fromFilename(where.filename().string());
#else
        auto [owned, dirFd, name] = target.sink.parentOf(target.name);
#endif

        if (writer) {
            /* Streamed: written as received, then entered as the store's
               file -- or, should the store hold one, replaced by it. */
            writer->finish();
            writer.reset();
            if (linkable)
                objects.enterWritten(entry.hash, executable, dirFd, name, where, repair, hold, /*looked=*/false);
            return std::move(file.node);
        }

        /* The whole file is in memory and its blob known.  A hit: the
           store's valid file linked under the name, nothing written. */
        if (linkable && objects.link(entry.hash, executable, dirFd, name, where, buffer.size(), repair, hold, hold.emptyRegistered))
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
                written.isExecutable();
            written.preallocateContents(buffer.size());
            /* The complete file is already buffered, and this writer is fresh. */
            assert(!written.hasData());
            written.writeUnbuffered(buffer);
            written.finish();
        }
        if (linkable)
            objects.enterWritten(entry.hash, executable, dirFd, name, where, repair, hold);
        return std::move(file.node);
    }
#ifndef _WIN32
    struct ObjectUnavailable {};

    HashedNode node(
        SourceAccessor & accessor,
        const CanonPath & from,
        const CanonPath & path,
        const SourceAccessor::Stat & st,
        function_ref<HashedNode()> descend) override
    {
        if (naming)
            if (auto result = tryKnown(accessor, from, path, st))
                return std::move(*result);
        return descend();
    }

    std::string auxiliary(const merkle::TreeEntry & entry)
    {
        auto body = objects.readAuxiliary(entry);
        if (!body)
            throw ObjectUnavailable{};
        return std::move(*body);
    }

    /* Materialise validated Merkle values directly. Directory scopes own their
       parsed children and original bodies until all child visits complete. */
    uint64_t materialise(
        const merkle::TreeEntry & entry, const CanonPath & path, size_t depth,
        const GitObjectStore::Hold & attemptHold, const Target & target)
    {
        checkInterrupt();
        if (depth >= narMaxDepth)
            throw Error("path '%s' exceeds maximum NAR directory depth of %d", path, narMaxDepth);
        switch (entry.mode) {
        case merkle::Mode::Regular:
        case merkle::Mode::Executable: {
            bool executable = entry.mode == merkle::Mode::Executable;
            auto where = target.path();
            if (linkableOnThisSystem(where)) {
                auto [owned, dirFd, name] = target.sink.parentOf(target.name);
                auto linked = objects.link(
                    entry.hash, executable, dirFd, name, where, std::nullopt, repair, attemptHold);
                if (!linked)
                    throw ObjectUnavailable{};
                return merkle::nar::regular(*linked, executable);
            }
            auto holder = objects.holderOrForget(entry.hash, executable, repair);
            if (!holder)
                holder = objects.holderOrForget(entry.hash, !executable, repair);
            if (!holder)
                throw ObjectUnavailable{};
            auto source = holder->parentOf();
            if (!source)
                throw ObjectUnavailable{};
            auto & [owned, parent, name] = *source;
            AutoCloseFD fd{openat(parent, name.c_str(), O_RDONLY | O_CLOEXEC | (parent == AT_FDCWD ? 0 : O_NOFOLLOW))};
            if (!fd)
                throw SysError([&] { return HintFmt("opening blob holder %s", PathFmt(holder->file())); });
            auto bytes = nix::readFile(fd.get());
            if (merkle::blobId(bytes) != entry.hash)
                throw Error("blob object %s does not hash to its name", entry.hash.gitRev());
            target.sink.createRegularFile(target.name, [&](CreateRegularFileSink & writer) {
                if (executable)
                    writer.isExecutable();
                writer.preallocateContents(bytes.size());
                writer(bytes);
            });
            return merkle::nar::regular(bytes.size(), executable);
        }
        case merkle::Mode::Symlink: {
            auto body = auxiliary(entry);
            target.sink.createSymlink(target.name, body);
            auto size = merkle::nar::symlink(body.size());
            attemptHold.addAuxiliary({entry, std::move(body)});
            return size;
        }
        case merkle::Mode::Directory: {
            std::string body;
            std::map<std::string, merkle::TreeEntry> tree;
            try {
                body = auxiliary(entry);
                tree = GitObjectStore::parseTreeBody(entry.hash, body);
            } catch (Error & e) {
                warn("%s; not used\nYou should run `nix-store --verify --check-contents --repair` to fix it", e.msg());
                throw ObjectUnavailable{};
            }
            if (tree.contains("."))
                throw Error("synthetic root object cannot be used as a directory");

            merkle::nar::Directory size;
            auto populate = [&](RestoreSink & sub, const CanonPath & rel) {
                assert(rel.isRoot());
                CaseHackNames hack;
                for (auto & [name, child] : tree) {
                    auto diskName = hack.diskName(name);
                    size.add(name.size(), materialise(
                        child, path / name, depth + 1, attemptHold, {sub, CanonPath::root / diskName}));
                }
            };
            target.sink.createSubdirectory(target.name, std::ref(populate));
            /* Acquisition checked the hash; parsing checked canonical encoding.
               Every successful child preserves its typed entry. */
            attemptHold.addAuxiliary({entry, std::move(body)});
            return size.finish();
        }
        }
        throw Error("object has unsupported mode %o", static_cast<RawMode>(entry.mode));
    }

    std::optional<HashedNode>
    tryKnown(SourceAccessor & accessor, const CanonPath & from, const CanonPath & path, const SourceAccessor::Stat & st)
    {
        /* Use immutable identities to link hits and stream misses without hashing.
           Mutable worktrees provide none; repair bypasses this shortcut. */
        if (!repair && st.type == SourceAccessor::tRegular)
            if (auto entry = accessor.getKnownEntry(from);
                entry && entry->hash.algo == merkle::hashAlgo
                && entry->mode == (st.isExecutable ? merkle::Mode::Executable : merkle::Mode::Regular)) {
                auto target = at(path);
                auto where = target.path();
                auto [owned, dirFd, name] = target.sink.parentOf(target.name);
                bool linkable = linkableOnThisSystem(where);
                if (linkable)
                    if (auto linked = objects.link(
                            entry->hash, st.isExecutable, dirFd, name, where, st.fileSize, repair, hold))
                        return HashedNode{
                            .entry = *entry,
                            .narSize = merkle::nar::regular(*linked, st.isExecutable)};

                std::optional<uint64_t> size;
                {
                    auto writer = target.sink.beginRegularFileAt(target.name, dirFd, name);
                    if (st.isExecutable)
                        writer.isExecutable();
                    /* Stat need not know a packed blob's size. readFile must
                       announce it before writing, so the copy obtains it only
                       when it actually needs the bytes. */
                    auto sizeCallback = [&](uint64_t announced) {
                        if (st.fileSize && announced != *st.fileSize)
                            throw Error("immutable file '%s' changed size while copying", accessor.showPath(from));
                        size = announced;
                        writer.preallocateContents(announced);
                    };
                    accessor.readFile(from, writer, std::ref(sizeCallback));
                    if (!size)
                        throw Error("immutable file '%s' did not announce its size", accessor.showPath(from));
                    writer.finish();
                    if (writer.written != *size)
                        throw Error("immutable file '%s' delivered the wrong number of bytes", accessor.showPath(from));
                }
                if (linkable)
                    objects.enterWritten(entry->hash, st.isExecutable, dirFd, name, where, repair, hold);
                return HashedNode{.entry = *entry, .narSize = merkle::nar::regular(*size, st.isExecutable)};
            }
        if (!path.isRoot() && st.type != SourceAccessor::tDirectory)
            return std::nullopt;
        auto entry = (*naming->namer.lock())(path, st);
        if (!entry)
            return std::nullopt;
        auto target = at(path);
        auto where = target.path();
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
            /* createSubdirectory may open this fresh root in the outer
               sink. Its attempt must not leave that FD pointing at a
               deleted tree when the source traversal resumes. */
            bool freshRoot = path.isRoot() && !target.sink.dirFd;
            Finally closeAttemptRoot{[&] {
                if (freshRoot) {
                    AutoCloseFD owned{std::move(target.sink.dirFd)};
                }
            }};
            narSize = materialise(*entry, path, std::ranges::distance(path), attempt ? *attempt : hold, target);
        } catch (ObjectUnavailable &) {
        } catch (Error & e) {
            warn(
                "could not materialise %s from the object store: %s; copying it from the source instead",
                PathFmt(where),
                e.msg());
        }
        if (!narSize) {
            /* Whatever was made goes, and the driver reads the subtree. */
            if (maybeLstat(where))
                deletePath(where);
            debug("%s is not held by the object store; copying it from the source", PathFmt(where));
            return std::nullopt;
        }
        /* Publishing a completed name ends rollback's authority over it.
           A merge error aborts the ingestion; it never resumes fallback. */
        if (attempt)
            naming->commitAttempt(where, *attempt);
        return HashedNode{.entry = *entry, .narSize = narSize};
    }
#endif
};

} // namespace

ObjectHashSink::Result LocalStore::restoreThroughObjects(
    const std::filesystem::path & path,
    fun<void(FileSystemObjectSink &)> produce,
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
    produce(adapter);
    auto result = adapter.finish();
    objects.putRootTree(result.root, hold);
    return result;
}

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
    /* Callback permission additionally gates the audited source/filter policy. */
    size_t workers = callbacksPermitParallelism ? HashingVisitor::workerCountFor(*path.accessor, filter) : 1;
    auto commitAttempt = [&](const std::filesystem::path & subtree, const GitObjectStore::Hold & attempt) {
        auto projection = attempt.takeProjection();
        std::vector<BlobCoalescing> collisions;
        for (auto & row : projection.regular) {
            auto other = hold.addRow(std::move(row));
            if (!other)
                continue;
            auto current = lstat(std::filesystem::path(*other));
            if (current.st_ino == row.ino && current.st_dev == row.dev) {
                hold.appendNames(std::move(row));
                continue;
            }
            collisions.push_back({
                row,
                {{current.st_ino, current.st_dev, static_cast<uint64_t>(current.st_size), *other}},
            });
        }
        /* A completed attempt may share several aliases of each losing
           inode. Reconcile all of them before returning, using the same
           physical walk as publication; replacing one row is insufficient. */
        if (!collisions.empty()) {
            auto result = coalesceBlobs(hold.root(), hold, collisions);
            if (!result.rejected.empty())
                throw Error("a completed ingestion holder changed while materialising %s", PathFmt(subtree));
            for (auto & collision : collisions)
                hold.appendNames(std::move(collision.row));
        }
        for (auto & auxiliary : projection.auxiliary)
            hold.addAuxiliary(std::move(auxiliary));
    };
    LinkingVisitor::Naming naming{std::move(namer), commitAttempt};
    beginIngestion(hold, dst);
    /* Only completed unknown bodies omit discoveries after an EMPTY acquisition. */
    if (!config->readOnly && objects.index == &blobIndex && blobIndex.lookup && !mayHaveValidPaths())
        hold.emptyRegistered = &blobIndex;
    LinkingVisitor visitor{objects, hold, restore, repair, nullptr, workers};
    visitor.naming = &naming;
    auto result = objectHashOf(*path.accessor, path.path, filter, visitor);
    /* Every reused node carries the size of what it made, so the NAR
       size is exact, as the one route's is. */
    assert(result.narSize);
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

    /* Defer buffered regular-root writes until validity is known. Other
       roots use a locked temporary; both publish the same Git description. */
    const LocalSettings & localSettings = config->getLocalSettings();
    auto hooks = makeCanonicalisingRestoreHooks(NIX_WHEN_SUPPORT_ACLS2(localSettings.ignoredAcls));

    std::optional<GitObjectStore::Hold> hold;
    std::optional<std::pair<AutoCloseFD, AutoDelete>> tempDir;
    RestoreSink restore{localSettings.fsyncStorePaths, hooks.get()};
    auto prepare = [&] {
        if (hold)
            return;
        /* Collection precedes every write; a buffered, already valid
           root needs neither collection nor an ingestion's hold. */
        autoGC();
        hold.emplace(objects.hold());
        tempDir.emplace(createTempDirInStore());
        restore.dstPath = tempDir->second.path() / "x";
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
                        beginIngestion(*hold, restore.dstPath);
                        return std::unique_ptr<RegularFileWriter>{
                            new RegularFileWriter(restore.beginRegularFile(CanonPath::root))};
                    }));
                return ObjectHashSink::Result::of(buffered->node);
            }
        }
        prepare();
        return materialiseThroughObjects(
            restore.dstPath,
            path,
            filter,
            localSettings.fsyncStorePaths,
            hooks.get(),
            repair,
            std::move(namer),
            *hold,
            callbacksPermitParallelism);
    }();

    if (settings.warnLargePathThreshold && result.narSize.value() >= settings.warnLargePathThreshold)
        warn("copied large path '%s' to the store (%s)", path, renderSize(result.narSize.value()));

    auto desc = ContentAddressWithReferences::fromParts(
        ContentAddressMethod::Raw::Git, merkle::objectHash(result.root), {.others = {}, .self = false});
    auto populate =
        [&](const std::filesystem::path & realPath) -> std::pair<ObjectHashSink::Result, const GitObjectStore::Hold &> {
        if (buffered) {
            prepare();
            if (!buffered->writer)
                beginIngestion(*hold, restore.dstPath);
            LinkingVisitor linking{objects, *hold, restore, repair};
            auto target = linking.at(CanonPath::root);
            linking.placeRegular(target, std::move(*buffered));
            buffered.reset();
            objects.putRootTree(result.root, *hold);
        }
        if (!moveIntoStore(restore.dstPath, realPath, tempDir->second, hooks.get()))
            result = enterPath(realPath, repair, *hold);
        return {result, *hold};
    };
    return addToStorePrepared(name, std::move(desc), repair, std::ref(populate));
}

std::pair<ObjectHashSink::Result, std::vector<std::filesystem::path>>
LocalStore::verifyObjects(const std::filesystem::path & realPath)
{
    VerifyVisitor verify{objects, realPath};
    auto result = objectHashOf(*makeFSSourceAccessor(realPath), CanonPath::root, defaultPathFilter, verify);
    /* The synthetic root tree of a bare executable or symlink, through
       which its blob is reachable (`putRootTree`). */
    if (auto body = merkle::syntheticRootTree(result.root))
        verify.expect({merkle::Mode::Directory, merkle::treeId(*body)}, realPath);
    return {result, std::move(verify.missing)};
}

LocalStore::BlobCoalescingResult LocalStore::coalesceBlobs(
    const std::filesystem::path & root,
    const GitObjectStore::Hold & hold,
    std::vector<BlobCoalescing> & collisions)
{
#ifdef _WIN32
    return {};
#else
    /* Publication holds the database writer; speculative completion owns its names.
       Pin and verify without index calls, temp roots or auxiliary-object locks. */
    BlobCoalescingResult result;
    auto temporary = [&] { return hold.tempPath("coalesce"); };

    for (auto & collision : collisions) {
        auto & row = collision.row;
        /* No usable donor means this file can become the holder. A file
           refused sharing is an exception, not a new canonical donor. */
        collision.eligible = true;
        for (auto & candidate : collision.candidates) {
            if (candidate.size != row.size) {
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
                auto observed = GitObjectStore::blobIdOfFile(own, candidate.size);
                if (observed != row.id)
                    throw Error(
                        "cannot publish ingestion holder %s: expected blob %s, observed %s",
                        PathFmt(own),
                        row.id.gitRev(),
                        observed.gitRev());
                collision.eligible = true;
                break;
            }
            auto source = candidate.parentOf();
            if (!source) {
                result.rejected.emplace_back(row, candidate);
                continue;
            }
            auto & [owned, parent, name] = *source;
            auto pin = temporary();
            std::error_code ec;
            if (objects.failPlaceWithNoSpace && objects.failPlaceWithNoSpace(pin))
                ec = std::make_error_code(std::errc::no_space_on_device);
            else if (::linkat(parent, name.c_str(), AT_FDCWD, pin.c_str(), 0) == -1)
                ec = std::error_code(errno, std::generic_category());
            if (ec) {
                /* A stale candidate is rejected even when pinning met a link
                   limit; a refusal applies only to a current physical holder. */
                auto now = ec == std::errc::no_such_file_or_directory
                               ? std::nullopt
                               : maybeLstat(name, parent);
                if (!now || !GitObjectStore::validHolderMetadata(candidate, row.executable, *now)) {
                    result.rejected.emplace_back(row, candidate);
                    continue;
                }
                if (ec == std::errc::too_many_links || ec == std::errc::no_space_on_device) {
                    collision.eligible = false;
                    continue;
                }
                throw Error(
                    "pinning blob holder %1% as %2%: %3%",
                    PathFmt(candidate.file()),
                    PathFmt(pin),
                    ec.message());
            }
            AutoDelete cleanup(pin, false);
            auto pinned = lstat(pin);
            if (!GitObjectStore::validHolderMetadata(candidate, row.executable, pinned)
                || GitObjectStore::blobIdOfFile(pin, candidate.size) != row.id) {
                result.rejected.emplace_back(row, candidate);
                continue;
            }
            collision.eligible = false;
            std::string * representative = nullptr;
            std::erase(row.aliases, row.name);
            std::ranges::sort(row.aliases);
            row.aliases.erase(std::unique(row.aliases.begin(), row.aliases.end()), row.aliases.end());
            for (auto & recorded : row.names()) {
                auto where = recorded.empty() ? root : root / recorded;
                checkInterrupt();
                auto observed = maybeLstat(where);
                if (!observed || !linkableOnThisSystem(where))
                    continue;
                auto & st = *observed;
                /* A name may already share the winning class. */
                if (row.dev != st.st_dev || row.ino != st.st_ino) {
                    if (!representative && GitObjectStore::validHolderMetadata(candidate, row.executable, st))
                        representative = &recorded;
                    continue;
                }
                auto expected = GitObjectStore::Holder{row.ino, row.dev, row.size, {}};
                if (!GitObjectStore::validHolderMetadata(expected, row.executable, st))
                    continue;
                if (objects.failPlaceWithNoSpace && objects.failPlaceWithNoSpace(where))
                    continue;

                auto parent = where.parent_path();
                WritableDirectory dir(parent, parent != config->realStoreDir.get());
                auto name = CanonPath::fromFilename(where.filename().string());
                /* Re-check after opening the containing directory, so a pathname
                   replaced since enumeration is never intentionally overwritten. */
                auto before = statIn(dir.fd.get(), name, where);
                if (!GitObjectStore::validHolderMetadata(expected, row.executable, before))
                    continue;
                if (!GitObjectStore::linkOver(
                        hold, {AT_FDCWD, cleanup.path().native()}, [&] { return PathFmt(cleanup.path()); },
                        dir.fd.get(), name, where, before,
                        [](auto &, const auto &) { return true; }, &candidate))
                    continue;
                result.modified = true;
                if (!representative)
                    representative = &recorded;
            }
            /* Retain this class's physical representative before any later SQL can retry. */
            if (!representative)
                break;
            auto st = maybeLstat(representative->empty() ? root : root / *representative);
            if (!st || !GitObjectStore::validHolderMetadata(candidate, row.executable, *st))
                break;
            row.ino = st->st_ino;
            row.dev = st->st_dev;
            row.size = static_cast<uint64_t>(st->st_size);
            row.name.swap(*representative);
            collision.eligible = true;
            break;
        }
    }
    return result;
#endif
}

ObjectHashSink::Result LocalStore::enterPath(
    const std::filesystem::path & path,
    RepairFlag repair,
    const GitObjectStore::Hold & hold,
    Activity * act,
    OptimiseStats * stats,
    GitObjectStore::BlobInodes * blobInodes)
{
    /* Start a fresh projection per path, including reused multioutput holds.
       The caller supplies its logical name at publication. */
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
    auto blobInodes = this->blobInodes();

    act.progress(0, paths.size());

    uint64_t done = 0;

    for (auto & i : paths) {
        addTempRoot(i);
        auto pathRoot = toRealPath(i);
        PathLocks outputLock;
        if (!locksHeld.count(printStorePath(i)))
            outputLock.lockPaths({pathRoot});
        auto row = retrySQLite<std::shared_ptr<ValidPathInfo>>(
            [&]() { return queryPathInfoInternal(*_state->lock(), i); });
        if (!row)
            continue; /* path was GC'ed, probably */
        Activity act(*logger, lvlTalkative, actUnknown, fmt("optimising path '%s'", printStorePath(i)));
        /* Migrate legacy rows before entry, checking their asserted NAR hash.
           Modified paths remain unentered. Legacy paths need this second walk;
           current rows already carry the object hash. */
        if (!row->objectHash) {
            try {
                if (auto failure = walkOldRow(*row)) {
                    printError("%s", failure->str());
                    continue;
                }
            } catch (InvalidPath &) {
                continue; /* collected meanwhile */
            }
            stats.rowsMigrated++;
        }
        /* Current inode witnesses skip regular reads. Publish the complete
           regular and auxiliary projection after traversal joins. */
        auto hold = objects.hold();
        enterPath(pathRoot, NoRepair, hold, &act, &stats, &blobInodes);
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

} // namespace nix
