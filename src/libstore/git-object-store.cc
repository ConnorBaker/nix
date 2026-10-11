#include "nix/store/git-object-store.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/git.hh"
#include "nix/util/archive.hh"
#include "nix/util/file-system.hh"
#include "nix/util/file-system-at.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/logging.hh"
#include "nix/util/memo.hh"
#include "nix/util/signals.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/strings.hh"
#include "nix/util/sync.hh"
#include "nix/util/util.hh"

#include <utility>
#include <ranges>

#include <atomic>
#include <cstring>
#include <functional>
#include <type_traits>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nix {

Hash GitObjectStore::blobIdOfFile(const std::filesystem::path & path, uint64_t observedSize)
{
    merkle::ObjectHasher hasher{"blob", observedSize};
    readFile(path, hasher);
    return hasher.finish();
}

std::filesystem::path GitObjectStore::Hold::tempPath(std::string_view kind) const
{
    /* Preserve the historical unique name format; the lease owns its lifetime. */
    static std::atomic<uint32_t> counter(0);
    return directory()
           / fmt("%s-%d-%d-%d", kind, static_cast<long long>(time(nullptr)), getpid(), counter.fetch_add(1));
}

static void checkAuxiliaryEntry(const merkle::TreeEntry & entry)
{
    if ((entry.mode != merkle::Mode::Directory && entry.mode != merkle::Mode::Symlink)
        || entry.hash.algo != GitObjectStore::hashAlgo
        || entry.hash.hashSize != regularHashSize(GitObjectStore::hashAlgo))
        throw Error("an auxiliary address must be a SHA-256 directory or symlink identifier");
}

/* Pending and completed publication use the same owning typed relation. */
struct GitObjectStore::Hold::Pending
{
    Sync<Projection> values;
    std::filesystem::path root;
};

const std::filesystem::path & GitObjectStore::Hold::directory() const
{
    return lease()->dir;
}

void GitObjectStore::Hold::setRoot(std::filesystem::path root) const
{
    /* A reused hold starts a new path projection. Its previous completed
       rows have already been handed to the store; abandoned rows need no
       publication. No walking worker may outlive this boundary. */
    auto values = pending->values.lock();
    values->regular.clear();
    values->auxiliary.clear();
    pending->root = std::move(root);
}

const std::filesystem::path & GitObjectStore::Hold::root() const
{
    return pending->root;
}

std::optional<std::string> GitObjectStore::Hold::addRow(Row && row) const
{
    auto values = pending->values.lock();
    auto & regular = values->regular;
    auto [i, inserted] = regular.insert(std::move(row));
    if (inserted)
        return std::nullopt;
    auto & name = i->name;
    return name.empty() ? pending->root.string() : (pending->root / name).string();
}

void GitObjectStore::Hold::appendNames(const Row & row) const
{
    auto values = pending->values.lock();
    auto & regular = values->regular;
    auto i = regular.find(Key{row});
    assert(i != regular.end());
    i->aliases.append_range(row.names() | std::views::as_rvalue);
}

void GitObjectStore::Hold::addAuxiliary(Auxiliary object) const
{
    checkAuxiliaryEntry(object.entry);
    auto values = pending->values.lock();
    values->auxiliary.insert(std::move(object));
}

GitObjectStore::Hold::Projection & GitObjectStore::Hold::projection() const
{
    auto values = pending->values.lock();
    return *values;
}

GitObjectStore::Hold::Projection GitObjectStore::Hold::takeProjection() const
{
    static_assert(std::is_nothrow_move_constructible_v<Projection>);
    static_assert(std::is_nothrow_move_assignable_v<Projection>);
    auto values = pending->values.lock();
    return std::move(*values);
}

std::optional<std::string> GitObjectStore::Hold::pendingHolder(const Hash & id, bool executable) const
{
    auto values = pending->values.lock();
    Key key{executable ? merkle::Mode::Executable : merkle::Mode::Regular, id.bytes()};
    auto & regular = values->regular;
    auto i = regular.find(key);
    if (i == regular.end())
        return std::nullopt;
    auto & name = i->name;
    return name.empty() ? pending->root.string() : (pending->root / name).string();
}

/* Preserve existing directory permissions, including directory symlinks.
   Only directories created by this call need owner access restored. */
static void createAccessibleDirs(const std::filesystem::path & path)
{
    try {
        std::error_code error;
        if (path.empty() || std::filesystem::is_directory(path, error))
            return;
        if (error && error != std::errc::no_such_file_or_directory)
            throw SystemError(error, "creating directory %s", PathFmt(path));
        createAccessibleDirs(path.parent_path());
        if (std::filesystem::create_directory(path))
            std::filesystem::permissions(
                path, std::filesystem::perms::owner_all,
                std::filesystem::perm_options::add | std::filesystem::perm_options::nofollow);
    } catch (const std::filesystem::filesystem_error & e) {
        throw SystemError(e.code(), "creating directory %s", PathFmt(path));
    }
}

std::atomic<uint32_t> GitObjectStore::Hold::Lease::counter{0};

GitObjectStore::Hold::Lease::Lease(const std::filesystem::path & tmp, const std::function<void()> & atHoldMade)
{
    /* GC may remove the empty directory before its lock is acquired.
       Retry until its name identifies the locked directory. A reused
       PID must not adopt a previous ingestion's directory. */
    createAccessibleDirs(tmp);
    while (true) {
        dir = tmp / fmt("hold-%d-%d", getpid(), counter.fetch_add(1));
        /* A PID may have been reused since a crashed ingestion. Its hold
           and unfinished temporary names are still its own: claim a new
           directory rather than adopting whatever is already here. */
        try {
            createDir(dir, 0700);
        } catch (const SystemError & e) {
            if (e.is(std::errc::file_exists))
                continue;
            throw;
        }
        /* This private lease must remain accessible under every umask. */
        try {
            chmod(dir, 0700);
        } catch (const SystemError & e) {
            if (e.is(std::errc::no_such_file_or_directory))
                continue;
            throw;
        }
        fd = openDirectory(dir, FinalSymlink::DontFollow);
        if (!fd) {
            if (errno == ENOENT)
                continue;
            throw SysError("opening %s", PathFmt(dir));
        }
        if (atHoldMade)
            atHoldMade();
        if (!lockFile(fd.get(), ltWrite, true))
            continue;
        if (auto current = maybeLstat(dir)) {
#ifndef _WIN32
            auto heldStat = nix::fstat(fd.get());
            if (current->st_ino != heldStat.st_ino || current->st_dev != heldStat.st_dev)
                continue;
#endif
            return;
        }
    }
}

GitObjectStore::Hold::Hold(fun<std::shared_ptr<Lease>()> lease)
    : pending(std::make_unique<Pending>())
    , lease(std::move(lease))
{
}

GitObjectStore::Hold GitObjectStore::Hold::ownRows() const
{
    return Hold(lease);
}

GitObjectStore::Hold::Hold(Hold &&) = default;
GitObjectStore::Hold::~Hold() = default;

#ifndef _WIN32
/* Own the directory stream through the caller's final namespace operation. */
template<typename Visit>
static AutoCloseDir scanObjectDirectory(AutoCloseFD scan, const char * what, Visit && visit)
{
    auto fd = scan.get();
    AutoCloseDir entries{fdopendir(fd)};
    if (!entries)
        throw SysError("reading %s", what);
    scan.release();
    struct dirent * entry;
    while (errno = 0, entry = readdir(entries.get())) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        visit(fd, entry->d_name);
    }
    if (errno)
        throw SysError("reading %s", what);
    return entries;
}
#endif

GitObjectStore::Hold::Lease::~Lease()
{
    if (!fd)
        return;
    try {
#ifndef _WIN32
        /* The directory stream retains the lease lock through removal. */
        auto entries = scanObjectDirectory(std::move(fd), "ingestion lease", [&](Descriptor parent, const char * name) {
            /* Joined workers cannot add pins; never descend a surprising directory. */
            if (unlinkat(parent, name, 0) == -1 && errno != ENOENT)
                throw SysError("unlinking ingestion pin '%s'", name);
        });
        if (::rmdir(dir.c_str()) == -1 && errno != ENOENT)
            throw SysError("removing hold %s", PathFmt(dir));
#else
        for (auto & entry : DirectoryIterator{dir}) {
            auto st = maybeLstat(entry.path());
            if (st && S_ISDIR(st->st_mode))
                throw Error("unexpected directory in ingestion lease %s", PathFmt(entry.path()));
            unlinkIfExists(entry.path());
        }
        fd.close();
        std::filesystem::remove(dir);
#endif
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

GitObjectStore::Hold GitObjectStore::hold() const
{
    return Hold(memo<std::shared_ptr<Hold::Lease>>([tmp = dir / "tmp", atHoldMade = atHoldMade] {
        return std::make_shared<Hold::Lease>(tmp, atHoldMade);
    }));
}

#ifndef _WIN32
std::optional<std::tuple<AutoCloseFD, Descriptor, std::string>> GitObjectStore::Holder::parentOf() const
{
    auto registered = std::get_if<Registered>(&address);
    if (!registered)
        return std::tuple{AutoCloseFD{}, AT_FDCWD, std::get<std::string>(address)};
    auto & [root, coordinate] = *registered;
    auto missing = [] { return errno == ENOENT || errno == ENOTDIR; };
    int flags = O_DIRECTORY | O_CLOEXEC;
#  ifdef O_PATH
    flags |= O_PATH;
#  else
    flags |= O_RDONLY;
#  endif
    AutoCloseFD anchor{::open(root.c_str(), flags)};
    if (!anchor) {
        if (missing())
            return std::nullopt;
        throw SysError("opening store directory for %s", PathFmt(root));
    }
    auto parent = *coordinate.parent();
    try {
        AutoCloseFD owned{parent.isRoot() ? std::move(anchor) : openFileEnsureBeneathNoSymlinks(anchor.get(), parent, flags)};
        if (!owned) {
            if (missing())
                return std::nullopt;
            throw SysError([&] { return HintFmt("opening parent of blob holder %s", PathFmt(file())); });
        }
        auto fd = owned.get();
        return std::tuple{std::move(owned), fd, std::string(*coordinate.baseName())};
    } catch (SymlinkNotAllowed &) {
        return std::nullopt;
    }
}

Hash GitObjectStore::blobIdOfFile(Descriptor parent, const std::string & name, uint64_t observedSize)
{
    if (parent == AT_FDCWD)
        return blobIdOfFile(std::filesystem::path(name), observedSize);
    AutoCloseFD fd{openat(parent, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (!fd)
        throw SysError([&] { return HintFmt("opening blob holder %s", PathFmt(std::filesystem::path(name))); });
    merkle::ObjectHasher hasher{"blob", observedSize};
    drainFD(fd.get(), hasher);
    return hasher.finish();
}

/* A hard link `name` in the directory `dirFd` for the file `from`: the
   directory resolved once is the directory written. */
static std::error_code linkPathIntoDir(const std::string & from, Descriptor dirFd, const CanonPath & name, Descriptor parent = AT_FDCWD)
{
    if (linkat(parent, from.c_str(), dirFd, name.rel_c_str(), 0) == -1)
        return {errno, std::generic_category()};
    return {};
}

#endif

bool GitObjectStore::validHolderMetadata(const Holder & holder, bool executable, const PosixStat & st)
{
#ifdef _WIN32
    return false;
#else
    return S_ISREG(st.st_mode) && (st.st_mode & ~S_IFMT) == (executable ? 0555 : 0444) && st.st_mtime == 1
#  ifdef __APPLE__
           && st.st_mtimespec.tv_nsec == 0
#  else
           && st.st_mtim.tv_nsec == 0
#  endif
           && holder.ino == st.st_ino && holder.dev == st.st_dev && static_cast<uint64_t>(st.st_size) == holder.size;
#endif
}

std::optional<GitObjectStore::Holder>
GitObjectStore::holderOrForget(const Hash & id, bool executable, RepairFlag repair)
{
#ifdef _WIN32
    return std::nullopt;
#else
    if (!index || !index->lookup)
        return std::nullopt;
    auto cursor = Holder::initialCursor;
    while (auto row = index->lookup(id, executable, cursor)) {
        assert(row->cursor() > cursor);
        cursor = row->cursor();
        if (counters)
            counters->blobsStatted++;
        auto source = row->parentOf();
        auto st = source ? maybeLstat(std::get<2>(*source), std::get<1>(*source)) : std::nullopt;
        /* Forget only this candidate. Advance even when read-only cleanup is a no-op. */
        if (st && validHolderMetadata(*row, executable, *st) && (!repair || blobIdOfFile(std::get<1>(*source), std::get<2>(*source), row->size) == id)) {
            return row;
        }
        if (index->forget)
            index->forget(id, executable, *row);
    }
    return std::nullopt;
#endif
}

#ifndef _WIN32
/* Names are relative to the ingestion root, empty for the root itself.
   Empty or nonmatching roots retain the caller's absolute name. */
static std::string nameUnderRoot(const std::filesystem::path & where, const std::filesystem::path & root)
{
    std::string_view r = root.native();
    auto & w = where.native();
    while (r.ends_with('/'))
        r.remove_suffix(1);
    if (root.empty() || !w.starts_with(r) || (w.size() != r.size() && w[r.size()] != '/'))
        return w;
    auto name = w.find_first_not_of('/', r.size());
    return name == w.npos ? std::string() : w.substr(name);
}

static GitObjectStore::Hold::Row rowOf(
    const Hash & id,
    bool executable,
    const PosixStat & st,
    const std::filesystem::path & where,
    const GitObjectStore::Hold & hold)
{
    return {id, executable, st.st_ino, st.st_dev, static_cast<uint64_t>(st.st_size),
        nameUnderRoot(where, hold.root())};
}

static std::optional<GitObjectStore::Placed> placePending(
    const GitObjectStore::Hold & hold,
    const std::string & file,
    Descriptor dirFd,
    const CanonPath & name,
    const std::filesystem::path & where,
    const PosixStat & before)
{
    auto st = maybeLstat(file);
    if (!st)
        return std::nullopt;
    if (st->st_ino == before.st_ino && st->st_dev == before.st_dev)
        return GitObjectStore::Placed{before.st_ino, false};
    return GitObjectStore::linkOver(
        hold, {AT_FDCWD, file}, [&] { return PathFmt(std::filesystem::path(file)); },
        dirFd, name, where, before, [](auto &, const auto &) { return true; });
}

#endif

std::optional<uint64_t> GitObjectStore::link(
    const Hash & id,
    bool executable,
    Descriptor dirFd,
    const CanonPath & name,
    const std::filesystem::path & where,
    std::optional<uint64_t> size,
    RepairFlag repair,
    const Hold & hold,
    const Index * emptyRegistered)
{
#ifdef _WIN32
    return std::nullopt;
#else
    /* The holder: a file this ingestion already wrote for the blob (law 3
       within one path, before any row is committed), else the index's. */
    std::optional<PosixStat> st;
    auto mine = hold.pendingHolder(id, executable);
    if (mine) {
        /* The first completed file belongs to this ingestion's locked
           destination. Speculative directory attempts keep separate rows
           until success, so their rollback cannot remove this name. */
        st = maybeLstat(*mine);
        if (st && size && static_cast<uint64_t>(st->st_size) != *size)
            st = std::nullopt;
        if (!st)
            return std::nullopt;
        if (auto ec = linkPathIntoDir(*mine, dirFd, name)) {
            if (ec == std::errc::no_such_file_or_directory || ec == std::errc::too_many_links
                || ec == std::errc::no_space_on_device)
                return std::nullopt;
            throw Error(
                "creating hard link from %1% to %2%: %3%",
                PathFmt(std::filesystem::path(*mine)),
                PathFmt(where),
                ec.message());
        }
    } else if (index && index != emptyRegistered && index->lookup) {
        auto cursor = Holder::initialCursor;
        while (auto holder = index->lookup(id, executable, cursor)) {
            assert(holder->cursor() > cursor);
            cursor = holder->cursor();
            auto forget = [&] {
                if (index->forget)
                    index->forget(id, executable, *holder);
            };
            if (size && holder->size != *size)
                continue;
            auto source = holder->parentOf();
            if (!source) {
                forget();
                continue;
            }
            auto & [owned, fromFd, fromName] = *source;
            if (auto ec = linkPathIntoDir(fromName, dirFd, name, fromFd)) {
                if (ec == std::errc::too_many_links || ec == std::errc::no_space_on_device)
                    return std::nullopt;
                if (ec == std::errc::no_such_file_or_directory) {
                    forget();
                    continue;
                }
                auto current = maybeLstat(fromName, fromFd);
                if (!current || !validHolderMetadata(*holder, executable, *current)) {
                    forget();
                    continue;
                }
                throw Error(
                    "creating hard link from %1% to %2%: %3%",
                    PathFmt(holder->file()),
                    PathFmt(where),
                    ec.message());
            }
            AutoDelete cleanup(where, false);
            PosixStat acquired;
            if (counters)
                counters->blobsStatted++;
            if (fstatat(dirFd, name.rel_c_str(), &acquired, AT_SYMLINK_NOFOLLOW) == -1)
                throw SysError("getting status of %s", PathFmt(where));
            /* The linked inode is the witness. A repaired replacement at
               the donor's pathname must not inherit the old row's hash. */
            if (!validHolderMetadata(*holder, executable, acquired)
                || (repair && blobIdOfFile(where, holder->size) != id)) {
                forget();
                continue;
            }
            cleanup.cancel();
            st = acquired;
            break;
        }
    }
    if (!st)
        return std::nullopt;
    /* This path now holds the blob even if the donor is collected before
       registration. Keep its own witness, using the status already read.
       A concurrent worker may have claimed a different holder meanwhile. */
    auto row = rowOf(id, executable, *st, where, hold);
    if (mine)
        hold.appendNames(std::move(row));
    else if (auto other = hold.addRow(std::move(row))) {
        linkOver(
            hold, {AT_FDCWD, *other}, [&] { return PathFmt(std::filesystem::path(*other)); },
            dirFd, name, where, *st, [](auto &, const auto &) { return true; });
        hold.appendNames(std::move(row));
    }
    return static_cast<uint64_t>(st->st_size);
#endif
}

std::optional<GitObjectStore::Placed> GitObjectStore::place(
    const Hash & id,
    bool executable,
    Descriptor dirFd,
    const CanonPath & name,
    const std::filesystem::path & where,
    const PosixStat & before,
    RepairFlag repair,
    const Hold & hold)
{
#ifdef _WIN32
    return std::nullopt;
#else
    /* The destination is complete and canonical. Reuse a holder or claim it. */
    auto after = before;


    /* This ingestion's own earlier file for the blob comes first (law 3
       within one path), then the index's holder. */
    bool entered = true;
    if (auto mine = hold.pendingHolder(id, executable)) {
        hold.appendNames(rowOf(id, executable, before, where, hold));
        return placePending(hold, *mine, dirFd, name, where, before);
    } else if (index && index != hold.emptyRegistered && index->lookup) {
        auto cursor = Holder::initialCursor;
        while (auto holder = index->lookup(id, executable, cursor)) {
            assert(holder->cursor() > cursor);
            cursor = holder->cursor();
            if (validHolderMetadata(*holder, executable, before)) {
                /* The caller has already hashed this owned file. */
                entered = false;
                break;
            }
            if (holder->size != static_cast<uint64_t>(before.st_size))
                continue;
            auto source = holder->parentOf();
            if (!source) {
                if (index->forget)
                    index->forget(id, executable, *holder);
                continue;
            }
            if (auto placed = linkOver(
                    hold, {std::get<1>(*source), std::get<2>(*source)}, [&] { return PathFmt(holder->file()); },
                    dirFd, name, where, after, [&](const auto & pin, const std::optional<PosixStat> & acquired) {
                    if (acquired && validHolderMetadata(*holder, executable, *acquired)
                        && (!repair || blobIdOfFile(pin.first, pin.second, holder->size) == id))
                        return true;
                    if (index->forget)
                        index->forget(id, executable, *holder);
                    return false;
                })) {
                after.st_ino = placed->ino;
                entered = false;
                break;
            }
        }
    }

    /* Keep our own witness on both miss and reuse: GC may remove the donor
       before registration. The row remains private until publication. */
    auto row = rowOf(id, executable, after, where, hold);
    auto other = hold.addRow(std::move(row));
    if (other) {
        auto placed = linkOver(
            hold, {AT_FDCWD, *other}, [&] { return PathFmt(std::filesystem::path(*other)); },
            dirFd, name, where, after, [](auto &, const auto &) { return true; });
        hold.appendNames(std::move(row));
        return placed;
    }
    return Placed{after.st_ino, entered};
#endif
}

void GitObjectStore::enterWritten(
    const Hash & id,
    bool executable,
    Descriptor dirFd,
    const CanonPath & name,
    const std::filesystem::path & where,
    RepairFlag repair,
    const Hold & hold,
    bool looked)
{
#ifndef _WIN32
    /* Claim atomically: another worker may finish this blob after our lookup miss. */
    PosixStat st;
    if (fstatat(dirFd, name.rel_c_str(), &st, AT_SYMLINK_NOFOLLOW) == -1)
        throw SysError("getting status of %1%", PathFmt(where));
    /* Streamed files have not probed the index. Buffered misses need only
       the pending-row claim; repeating their registered lookup is unnecessary. */
    if (!looked) {
        place(id, executable, dirFd, name, where, st, repair, hold);
        return;
    }
    auto row = rowOf(id, executable, st, where, hold);
    auto other = hold.addRow(std::move(row));
    if (other) {
        placePending(hold, *other, dirFd, name, where, st);
        hold.appendNames(std::move(row));
    }
#endif
}

void GitObjectStore::putRootTree(const merkle::TreeEntry & root, const Hold & hold)
{
    if (auto body = merkle::syntheticRootTree(root)) {
        auto id = merkle::treeId(*body);
        hold.addAuxiliary({{merkle::Mode::Directory, std::move(id)}, std::move(*body)});
    }
}

std::optional<std::string> GitObjectStore::readAuxiliary(const merkle::TreeEntry & entry) const
{
    checkAuxiliaryEntry(entry);
    if (counters && entry.mode == merkle::Mode::Directory)
        ++counters->treesRead;
    if (index && index->readAuxiliary)
        return index->readAuxiliary(entry);
    return std::nullopt;
}

std::map<std::string, merkle::TreeEntry> GitObjectStore::parseTreeBody(const Hash & id, std::string_view body)
{
    struct Into : merkle::DirectorySink
    {
        std::map<std::string, merkle::TreeEntry> tree;

        void insertChild(std::string_view name, merkle::TreeEntry entry) override
        {
            if (!isValidNarName(name)
                && (name != "." || (entry.mode != merkle::Mode::Executable && entry.mode != merkle::Mode::Symlink)))
                throw Error("invalid tree entry name '%s'", name);
            if (!tree.try_emplace(std::string(name), std::move(entry)).second)
                throw Error("duplicate tree entry name '%s'", name);
        }
    } into;

    try {
        git::parseTreeBody(into, body, hashAlgo);
        if (into.tree.size() > 1 && into.tree.contains("."))
            throw Error("synthetic root tree has more than one entry");
        return std::move(into.tree);
    } catch (Error & e) {
        throw Error("malformed tree object %s: %s", id.gitRev(), e.msg());
    }
}

static bool isHoldName(std::string_view name)
{
    if (!name.starts_with("hold-"))
        return false;
    name.remove_prefix(5);
    auto split = name.find('-');
    return split != name.npos && split && split + 1 < name.size()
           && name.substr(0, split).find_first_not_of("0123456789") == name.npos
           && name.substr(split + 1).find_first_not_of("0123456789") == name.npos;
}

uint64_t GitObjectStore::sweep(const Owned * owned)
{
    uint64_t holds = 0;
    const auto & sweptDir = owned ? owned->dir : dir;
    auto removed = [&] {
        if (owned)
            owned->removed();
    };
#ifndef _WIN32
    auto root = openDirectory(sweptDir, FinalSymlink::DontFollow);
    if (!root) {
        if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP)
            return holds;
        throw SysError("opening ingestion namespace %s", PathFmt(sweptDir));
    }
    auto openChild = [&](Descriptor parent, const char * name) {
        AutoCloseFD fd{openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (!fd && errno != ENOENT && errno != ENOTDIR && errno != ELOOP)
            throw SysError("opening ingestion directory '%s'", name);
        return fd;
    };
    auto remove = [&](Descriptor parent, const char * name, bool directory) {
        if (unlinkat(parent, name, directory ? AT_REMOVEDIR : 0) == 0) {
            removed();
            return true;
        }
        if (errno == ENOENT || errno == ENOTDIR)
            return false;
        if (directory && (errno == ENOTEMPTY || errno == EEXIST))
            return false;
        throw SysError("removing ingestion entry '%s'", name);
    };
    auto scan = [&](AutoCloseFD fd, auto && visit) {
        scanObjectDirectory(std::move(fd), "ingestion directory", [&](Descriptor parent, const char * name) {
            checkInterrupt();
            visit(parent, name);
        });
    };
    auto tmp = openChild(root.get(), "tmp");
    if (tmp) {
        AutoCloseFD mergedRoot, mergedTmp;
        if (owned) {
            mergedRoot = openDirectory(dir, FinalSymlink::DontFollow);
            if (mergedRoot)
                mergedTmp = openChild(mergedRoot.get(), "tmp");
        }
        scan(std::move(tmp), [&](Descriptor tmpFd, const char * name) {
            if (!isHoldName(name))
                return;
            auto held = openChild(tmpFd, name);
            if (!held)
                return;
            /* The overlay ingestion locks the merged name. Removal stays
               under the independently opened upper directory throughout. */
            AutoCloseFD merged;
            if (owned && mergedTmp)
                merged = openChild(mergedTmp.get(), name);
            auto lockFd = owned ? (merged ? merged.get() : -1) : held.get();
            if (lockFd == -1 || !lockFile(lockFd, ltWrite, false)) {
                ++holds;
                return;
            }
            auto heldStat = nix::fstat(held.get());
            auto current = maybeLstat(name, tmpFd);
            if (!current || current->st_ino != heldStat.st_ino || current->st_dev != heldStat.st_dev)
                return;
            try {
                /* Pins are leaves. Unknown directories are retained, not recursed. */
                scan(dupDescriptor(held.get()), [&](Descriptor holdFd, const char * leaf) {
                    auto leafSt = maybeLstat(leaf, holdFd);
                    if (leafSt && !S_ISDIR(leafSt->st_mode))
                        remove(holdFd, leaf, false);
                });
                if (!remove(tmpFd, name, true) && maybeLstat(name, tmpFd))
                    ++holds;
            } catch (SystemError & e) {
                printInfo("cannot remove the ingestion lease '%s': %s", name, e.msg());
                ++holds;
            }
        });
    }
#else
    /* Windows retains its existing portable namespace operations. */
    auto root = maybeLstat(sweptDir);
    if (!root || !S_ISDIR(root->st_mode))
        return holds;
    auto remove = [&](const std::filesystem::path & path) {
        std::error_code ec;
        bool didRemove = std::filesystem::remove(path, ec);
        if (ec && ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory
            && ec != std::errc::directory_not_empty)
            throw SystemError(ec, "removing ingestion entry %s", PathFmt(path));
        if (didRemove)
            removed();
        return didRemove;
    };
    auto tmp = sweptDir / "tmp";
    if (auto st = maybeLstat(tmp); st && S_ISDIR(st->st_mode))
        for (auto & entry : DirectoryIterator{tmp}) {
            checkInterrupt();
            auto st = maybeLstat(entry.path());
            if (!st)
                continue;
            auto name = entry.path().filename().string();
            if (!S_ISDIR(st->st_mode) || !isHoldName(name))
                continue;
            auto held = openDirectory(dir / "tmp" / name, FinalSymlink::DontFollow);
            if (!held || !lockFile(held.get(), ltWrite, false)) {
                ++holds;
                continue;
            }
            try {
                for (auto & leaf : DirectoryIterator{entry.path()}) {
                    checkInterrupt();
                    auto leafSt = maybeLstat(leaf.path());
                    if (leafSt && !S_ISDIR(leafSt->st_mode))
                        remove(leaf.path());
                }
                if (!remove(entry.path()) && maybeLstat(entry.path()))
                    ++holds;
            } catch (SystemError & e) {
                printInfo("cannot remove the ingestion lease '%s': %s", name, e.msg());
                ++holds;
            }
        }
#endif
    return holds;
}

} // namespace nix
