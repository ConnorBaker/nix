#include "nix/store/git-object-store.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/git.hh"
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
#include <boost/unordered/unordered_flat_set.hpp>

#include <atomic>
#include <cstring>
#include <functional>
#include <type_traits>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nix {

GitObjectStore::GitObjectStore(std::filesystem::path dir)
    : dir(std::move(dir))
{
}

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

/* The creation second carried by a retired flat temporary name, if present. */
static std::optional<time_t> tempTime(std::string_view name)
{
    auto parts = tokenizeString<std::vector<std::string>>(name, "-");
    if (parts.size() != 4)
        return std::nullopt;
    if (auto t = string2Int<long long>(parts[1]))
        return static_cast<time_t>(*t);
    return std::nullopt;
}

static void checkAuxiliaryEntry(const merkle::TreeEntry & entry)
{
    if ((entry.mode != merkle::Mode::Directory && entry.mode != merkle::Mode::Symlink)
        || entry.hash.algo != GitObjectStore::hashAlgo
        || entry.hash.hashSize != regularHashSize(GitObjectStore::hashAlgo))
        throw Error("an auxiliary address must be a SHA-256 directory or symlink identifier");
}

/* The finite pending map associates each typed (mode, id) with its first
   completed regular witness or owned auxiliary body. Its disjoint key domains
   are independent maps; values stay private until the completed-path handoff. */
struct GitObjectStore::Hold::Pending
{
    /* Borrow the value's existing identity. Like the old binary key, Hash
       equality uses length and bytes, without an algorithm tag. */
    struct Key : std::pair<merkle::Mode, const Hash &>
    {
        using std::pair<merkle::Mode, const Hash &>::pair;

        Key(const Row & row) noexcept
            : Key(row.executable ? merkle::Mode::Executable : merkle::Mode::Regular, row.id)
        {
        }

        Key(const Auxiliary & object) noexcept
            : Key(object.entry.mode, object.entry.hash)
        {
        }
    };

    struct KeyHash
    {
        using is_transparent = void;

        size_t operator()(Key key) const noexcept
        {
            return std::hash<Hash>{}(key.second) ^ static_cast<size_t>(key.first);
        }
    };

    struct KeyEqual : std::equal_to<Key>
    {
        using is_transparent = void;
    };

    Sync<boost::unordered_flat_set<Row, KeyHash, KeyEqual>> regular;
    Sync<boost::unordered_flat_set<Auxiliary, KeyHash, KeyEqual>> auxiliary;
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
    auto regular = pending->regular.lock();
    auto auxiliary = pending->auxiliary.lock();
    regular->clear();
    auxiliary->clear();
    pending->root = std::move(root);
}

const std::filesystem::path & GitObjectStore::Hold::root() const
{
    return pending->root;
}

std::optional<std::string> GitObjectStore::Hold::addRow(Row && row) const
{
    auto regular = pending->regular.lock();
    auto [i, inserted] = regular->insert(std::move(row));
    if (inserted)
        return std::nullopt;
    auto & name = i->name;
    return name.empty() ? pending->root.string() : (pending->root / name).string();
}

void GitObjectStore::Hold::addAuxiliary(Auxiliary object) const
{
    checkAuxiliaryEntry(object.entry);
    pending->auxiliary.lock()->insert(std::move(object));
}

GitObjectStore::Hold::Projection GitObjectStore::Hold::takeProjection() const
{
    static_assert(std::is_trivially_move_constructible_v<Hash>);
    static_assert(std::is_nothrow_move_constructible_v<Row>);
    static_assert(std::is_nothrow_move_constructible_v<Auxiliary>);
    auto regular = pending->regular.lock();
    auto auxiliary = pending->auxiliary.lock();
    Projection projection;
    /* Reserve both before extracting: failure leaves the complete relation intact. */
    projection.regular.reserve(regular->size());
    projection.auxiliary.reserve(auxiliary->size());
    /* Moving preserves each fixed hash/mode key; only owned strings change. */
    for (const auto & row : *regular)
        projection.regular.push_back(std::move(const_cast<Row &>(row)));
    regular->clear();
    for (const auto & object : *auxiliary)
        projection.auxiliary.push_back(std::move(const_cast<Auxiliary &>(object)));
    auxiliary->clear();
    return projection;
}

std::optional<std::string> GitObjectStore::Hold::pendingHolder(const Hash & id, bool executable) const
{
    auto regular = pending->regular.lock();
    auto i = regular->find(Pending::Key{executable ? merkle::Mode::Executable : merkle::Mode::Regular, id});
    if (i == regular->end())
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

GitObjectStore::Hold::Lease::Lease(const std::filesystem::path & tmp, const std::function<void()> & atHoldMade)
{
    /* GC may remove the empty directory before its lock is acquired.
       Retry under a fresh name until the lease exists while locked. A
       reused PID must not adopt a previous ingestion's directory. */
    static std::atomic<uint32_t> counter(0);
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
        if (!lockFile(fd.get(), ltWrite, true) || !pathExists(dir))
            continue;
        return;
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
/* New leases contain only this ingestion's flat regular-file pins. Joined
   workers cannot add entries. Never descend a surprising directory. */
static AutoCloseDir releaseHoldContents(AutoCloseFD scan)
{
    auto fd = scan.get();
    AutoCloseDir entries{fdopendir(fd)};
    if (!entries)
        throw SysError("reading ingestion lease");
    scan.release();
    struct dirent * entry;
    while (errno = 0, entry = readdir(entries.get())) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (unlinkat(fd, entry->d_name, 0) == -1 && errno != ENOENT)
            throw SysError("unlinking ingestion pin '%s'", entry->d_name);
    }
    if (errno)
        throw SysError("reading ingestion lease");
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
        auto entries = releaseHoldContents(std::move(fd));
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
/* The status of a file by name, without following a symlink; nothing when
   there is no such file.  As `nix::maybeLstat`, but over a string, so that
   the object store's names need not be `std::filesystem::path`s. */
static std::optional<PosixStat> maybeLstatOf(const std::string & path)
{
    std::optional<PosixStat> st{std::in_place};
    if (::lstat(path.c_str(), &*st)) {
        if (errno == ENOENT || errno == ENOTDIR)
            return std::nullopt;
        throw SysError("getting status of %s", PathFmt(std::filesystem::path(path)));
    }
    return st;
}

/* A hard link `name` in the directory `dirFd` for the file `from`: the
   directory resolved once is the directory written. */
static std::error_code linkPathIntoDir(const std::string & from, Descriptor dirFd, const CanonPath & name)
{
    if (linkat(AT_FDCWD, from.c_str(), dirFd, name.rel_c_str(), 0) == -1)
        return {errno, std::generic_category()};
    return {};
}

/* Rename `from`, by full path, over `name` in `dirFd`. */
static std::error_code renameInto(const std::filesystem::path & from, Descriptor dirFd, const CanonPath & name)
{
    if (renameat(AT_FDCWD, from.c_str(), dirFd, name.rel_c_str()) == -1)
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
        auto st = maybeLstatOf(row->file);
        /* Forget only this candidate. Advance even when read-only cleanup is a no-op. */
        if (st && validHolderMetadata(*row, executable, *st) && (!repair || blobIdOfFile(row->file, row->size) == id)) {
            return row;
        }
        if (index->forget)
            index->forget(id, executable, *row);
    }
    return std::nullopt;
#endif
}

#ifndef _WIN32
/* Replace a completed file by the chosen holder. Shared by placement and
   a link whose registered-holder probe raced this ingestion's first claim. */
template<typename Accept>
static std::optional<GitObjectStore::Placed> linkOver(
    const GitObjectStore::Hold & hold,
    const std::string & file,
    Descriptor dirFd,
    const CanonPath & name,
    const std::filesystem::path & where,
    const PosixStat & before,
    Accept && accept)
{
    auto filePath = [&] { return PathFmt(std::filesystem::path(file)); };
    auto tempLink = hold.tempPath("link");
    if (::link(file.c_str(), tempLink.c_str()) == -1) {
        auto ec = std::error_code(errno, std::generic_category());
        if (ec == std::errc::too_many_links || ec == std::errc::no_space_on_device) {
            printInfo("%1% has maximum number of links; %2% is left as it is", filePath(), PathFmt(where));
            return std::nullopt;
        }
        auto from = std::filesystem::path(file);
        if (ec == std::errc::no_such_file_or_directory) {
            accept(from, std::optional<PosixStat>{});
            return std::nullopt;
        }
        auto current = maybeLstatOf(file);
        if (!accept(from, current))
            return std::nullopt;
        throw Error("creating hard link from %1% to %2%: %3%", filePath(), PathFmt(tempLink), ec.message());
    }
    AutoDelete cleanup(tempLink, false);
    auto placed = lstat(tempLink);
    if (!accept(tempLink, placed))
        return std::nullopt;
    if (auto ec = renameInto(tempLink, dirFd, name)) {
        if (ec == std::errc::too_many_links || ec == std::errc::no_space_on_device) {
            printInfo("%1% has maximum number of links; %2% is left as it is", filePath(), PathFmt(where));
            return std::nullopt;
        }
        throw Error("replacing %1% by a link to %2%: %3%", PathFmt(where), filePath(), ec.message());
    }
    /* Renaming two names of the same inode succeeds without removing
       either name. Only that case leaves the temporary link behind. */
    if (placed.st_ino != before.st_ino || placed.st_dev != before.st_dev)
        cleanup.cancel();
    return GitObjectStore::Placed{placed.st_ino, false};
}

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
    return {id, executable, st.st_ino, st.st_dev, static_cast<uint64_t>(st.st_size), nameUnderRoot(where, hold.root())};
}

static std::optional<GitObjectStore::Placed> placePending(
    const GitObjectStore::Hold & hold,
    const std::string & file,
    Descriptor dirFd,
    const CanonPath & name,
    const std::filesystem::path & where,
    const PosixStat & before)
{
    auto st = maybeLstatOf(file);
    if (!st)
        return std::nullopt;
    if (st->st_ino == before.st_ino && st->st_dev == before.st_dev)
        return GitObjectStore::Placed{before.st_ino, false};
    return linkOver(hold, file, dirFd, name, where, before, [](auto &, const auto &) { return true; });
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
    const Hold & hold)
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
        st = maybeLstatOf(*mine);
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
    } else if (index && index->lookup) {
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
            if (auto ec = linkPathIntoDir(holder->file, dirFd, name)) {
                if (ec == std::errc::too_many_links || ec == std::errc::no_space_on_device)
                    return std::nullopt;
                if (ec == std::errc::no_such_file_or_directory) {
                    forget();
                    continue;
                }
                auto current = maybeLstatOf(holder->file);
                if (!current || !validHolderMetadata(*holder, executable, *current)) {
                    forget();
                    continue;
                }
                throw Error(
                    "creating hard link from %1% to %2%: %3%",
                    PathFmt(std::filesystem::path(holder->file)),
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
    if (!mine)
        if (auto other = hold.addRow(rowOf(id, executable, *st, where, hold)))
            linkOver(hold, *other, dirFd, name, where, *st, [](auto &, const auto &) { return true; });
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
    auto replaceBy = [&](const std::string & file, auto && accept) {
        auto placed = linkOver(hold, file, dirFd, name, where, after, std::forward<decltype(accept)>(accept));
        if (placed)
            after.st_ino = placed->ino;
        return placed;
    };

    /* This ingestion's own earlier file for the blob comes first (law 3
       within one path), then the index's holder. */
    bool entered = true;
    if (auto mine = hold.pendingHolder(id, executable)) {
        return placePending(hold, *mine, dirFd, name, where, before);
    } else if (index && index->lookup) {
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
            if (replaceBy(holder->file, [&](const auto & pin, const std::optional<PosixStat> & acquired) {
                    if (acquired && validHolderMetadata(*holder, executable, *acquired)
                        && (!repair || blobIdOfFile(pin, holder->size) == id))
                        return true;
                    if (index->forget)
                        index->forget(id, executable, *holder);
                    return false;
                })) {
                entered = false;
                break;
            }
        }
    }

    /* Keep our own witness on both miss and reuse: GC may remove the donor
       before registration. The row remains private until publication. */
    if (entered && failPlaceWithNoSpace && failPlaceWithNoSpace(where)) {
        printInfo("cannot enter %s: %s", PathFmt(where), std::make_error_code(std::errc::no_space_on_device).message());
        return std::nullopt;
    }
    auto other = hold.addRow(rowOf(id, executable, after, where, hold));
    if (other)
        return replaceBy(*other, [](auto &, const auto &) { return true; });
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
    auto other = hold.addRow(rowOf(id, executable, st, where, hold));
    if (other)
        placePending(hold, *other, dirFd, name, where, st);
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

merkle::Tree GitObjectStore::parseTreeBody(const Hash & id, std::string_view body)
{
    struct Into : merkle::DirectorySink
    {
        merkle::Tree tree;

        void insertChild(std::string_view name, merkle::TreeEntry entry) override
        {
            /* A bare executable or symlink has a singleton synthetic
               root. Every other tree is an ordinary directory map. */
            if (name == "." && tree.empty()
                && (entry.mode == merkle::Mode::Executable || entry.mode == merkle::Mode::Symlink)) {
                tree.emplace(".", std::move(entry));
                return;
            }
            if (!tree.empty() && tree.begin()->first == ".")
                throw Error("synthetic root tree has more than one entry");
            merkle::insertEntry(tree, std::string(name), entry);
        }
    } into;

    try {
        git::parseTreeBody(into, body, hashAlgo);
    } catch (Error & e) {
        throw Error("malformed tree object %s: %s", id.gitRev(), e.msg());
    }
    return std::move(into.tree);
}

/* Legacy caches are no longer a readable representation. Descend only the
   fixed kind/shard shape; an unexpected directory is an opaque occupant. */
static bool isShardName(std::string_view name)
{
    return name.size() == 2 && name.find_first_not_of("0123456789abcdef") == name.npos;
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

GitObjectStore::SweepStats GitObjectStore::sweep(const Owned * owned)
{
    SweepStats stats;
    const auto & sweptDir = owned ? owned->dir : dir;
    constexpr std::array<std::string_view, 3> kinds{"blobs", "blobs-x", "trees"};
    auto removed = [&] {
        if (owned)
            owned->removed();
    };
#ifndef _WIN32
    auto root = openDirectory(sweptDir, FinalSymlink::DontFollow);
    if (!root) {
        if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP)
            return stats;
        throw SysError("opening legacy object namespace %s", PathFmt(sweptDir));
    }
    auto openChild = [&](Descriptor parent, const char * name) {
        AutoCloseFD fd{openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (!fd && errno != ENOENT && errno != ENOTDIR && errno != ELOOP)
            throw SysError("opening legacy object directory '%s'", name);
        return fd;
    };
    auto status = [&](Descriptor parent, const char * name) -> std::optional<PosixStat> {
        PosixStat st;
        if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
            if (errno == ENOENT || errno == ENOTDIR)
                return std::nullopt;
            throw SysError("getting status of legacy object '%s'", name);
        }
        return st;
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
        throw SysError("removing legacy object '%s'", name);
    };
    auto scan = [&](AutoCloseFD fd, auto && visit) {
        auto parent = fd.get();
        AutoCloseDir stream{fdopendir(parent)};
        if (!stream)
            throw SysError("reading legacy object directory");
        fd.release();
        struct dirent * entry;
        while (errno = 0, entry = readdir(stream.get())) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            checkInterrupt();
            visit(parent, entry->d_name);
        }
        if (errno)
            throw SysError("reading legacy object directory");
    };
    auto cleanKind = [&](Descriptor parent, const char * kind, uint64_t * count) {
        auto fd = openChild(parent, kind);
        if (!fd)
            return;
        scan(std::move(fd), [&](Descriptor kindFd, const char * name) {
            auto st = status(kindFd, name);
            if (!st)
                return;
            if (S_ISDIR(st->st_mode)) {
                if (!isShardName(name))
                    return;
                auto shard = openChild(kindFd, name);
                if (!shard)
                    return;
                scan(std::move(shard), [&](Descriptor shardFd, const char * leaf) {
                    auto leafSt = status(shardFd, leaf);
                    if (leafSt && !S_ISDIR(leafSt->st_mode) && remove(shardFd, leaf, false) && count)
                        ++*count;
                });
                remove(kindFd, name, true);
            } else if (remove(kindFd, name, false) && count)
                ++*count;
        });
        remove(parent, kind, true);
    };
    auto tmp = openChild(root.get(), "tmp");
    if (tmp) {
        AutoCloseFD mergedRoot, mergedTmp;
        if (owned) {
            mergedRoot = openDirectory(dir, FinalSymlink::DontFollow);
            if (mergedRoot)
                mergedTmp = openChild(mergedRoot.get(), "tmp");
        }
        auto now = time(nullptr);
        scan(std::move(tmp), [&](Descriptor tmpFd, const char * name) {
            auto st = status(tmpFd, name);
            if (!st)
                return;
            if (!S_ISDIR(st->st_mode)) {
                auto made = tempTime(name).value_or(st->st_ctime);
                if (now - made > 3600)
                    remove(tmpFd, name, false);
                return;
            }
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
                ++stats.holds;
                return;
            }
            auto heldStat = nix::fstat(held.get());
            auto current = status(tmpFd, name);
            if (!current || current->st_ino != heldStat.st_ino || current->st_dev != heldStat.st_dev)
                return;
            try {
                if (failHoldRemoval && failHoldRemoval(dir / "tmp" / name))
                    throw SysError(ENOTEMPTY, "cannot remove ingestion lease '%s'", name);
                for (auto kind : kinds)
                    cleanKind(held.get(), kind.data(), nullptr);
                /* Regular coalescing pins and old flat temporary links are
                   leaves. Unknown directories are retained, not recursed. */
                scan(dupDescriptor(held.get()), [&](Descriptor holdFd, const char * leaf) {
                    auto leafSt = status(holdFd, leaf);
                    if (leafSt && !S_ISDIR(leafSt->st_mode))
                        remove(holdFd, leaf, false);
                });
                if (!remove(tmpFd, name, true) && status(tmpFd, name))
                    ++stats.holds;
            } catch (SystemError & e) {
                printInfo("cannot remove the ingestion lease '%s': %s", name, e.msg());
                ++stats.holds;
            }
        });
    }
    for (auto kind : kinds)
        cleanKind(root.get(), kind.data(), kind == "trees" ? &stats.treesRemoved : &stats.blobsRemoved);
#else
    /* Keep the existing portable no-follow metadata/directory operations on
       Windows. No publisher uses these legacy names after the transition. */
    auto root = maybeLstat(sweptDir);
    if (!root || !S_ISDIR(root->st_mode))
        return stats;
    auto remove = [&](const std::filesystem::path & path) {
        std::error_code ec;
        bool didRemove = std::filesystem::remove(path, ec);
        if (ec && ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory
            && ec != std::errc::directory_not_empty)
            throw SystemError(ec, "removing legacy object %s", PathFmt(path));
        if (didRemove)
            removed();
        return didRemove;
    };
    auto cleanKind = [&](const std::filesystem::path & path, uint64_t * count) {
        auto st = maybeLstat(path);
        if (!st || !S_ISDIR(st->st_mode))
            return;
        for (auto & entry : DirectoryIterator{path}) {
            checkInterrupt();
            auto observed = maybeLstat(entry.path());
            if (!observed)
                continue;
            if (S_ISDIR(observed->st_mode)) {
                if (!isShardName(entry.path().filename().string()))
                    continue;
                for (auto & leaf : DirectoryIterator{entry.path()}) {
                    checkInterrupt();
                    auto leafSt = maybeLstat(leaf.path());
                    if (leafSt && !S_ISDIR(leafSt->st_mode) && remove(leaf.path()) && count)
                        ++*count;
                }
                remove(entry.path());
            } else if (remove(entry.path()) && count)
                ++*count;
        }
        remove(path);
    };
    auto tmp = sweptDir / "tmp";
    if (auto st = maybeLstat(tmp); st && S_ISDIR(st->st_mode))
        for (auto & entry : DirectoryIterator{tmp}) {
            checkInterrupt();
            auto st = maybeLstat(entry.path());
            if (!st)
                continue;
            auto name = entry.path().filename().string();
            if (!S_ISDIR(st->st_mode)) {
                if (time(nullptr) - tempTime(name).value_or(st->st_ctime) > 3600)
                    remove(entry.path());
                continue;
            }
            if (!isHoldName(name))
                continue;
            auto held = openDirectory(dir / "tmp" / name, FinalSymlink::DontFollow);
            if (!held || !lockFile(held.get(), ltWrite, false)) {
                ++stats.holds;
                continue;
            }
            try {
                if (failHoldRemoval && failHoldRemoval(dir / "tmp" / name))
                    throw SysError(ENOTEMPTY, "cannot remove ingestion lease '%s'", name);
                for (auto kind : kinds)
                    cleanKind(entry.path() / kind, nullptr);
                for (auto & leaf : DirectoryIterator{entry.path()}) {
                    checkInterrupt();
                    auto leafSt = maybeLstat(leaf.path());
                    if (leafSt && !S_ISDIR(leafSt->st_mode))
                        remove(leaf.path());
                }
                if (!remove(entry.path()) && maybeLstat(entry.path()))
                    ++stats.holds;
            } catch (SystemError & e) {
                printInfo("cannot remove the ingestion lease '%s': %s", name, e.msg());
                ++stats.holds;
            }
        }
    for (auto kind : kinds)
        cleanKind(sweptDir / kind, kind == "trees" ? &stats.treesRemoved : &stats.blobsRemoved);
#endif
    return stats;
}

} // namespace nix
