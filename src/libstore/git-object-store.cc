#include "nix/store/git-object-store.hh"
#include "nix/store/pathlocks.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/base-n.hh"
#include "nix/util/git.hh"
#include "nix/util/file-system.hh"
#include "nix/util/file-system-at.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/finally.hh"
#include "nix/util/logging.hh"
#include "nix/util/os-string.hh"
#include "nix/util/signals.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/strings.hh"
#include "nix/util/util.hh"

#include <boost/unordered/unordered_flat_set.hpp>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <array>

#include <atomic>
#include <cstring>
#include <functional>
#include <random>
#include <type_traits>
#include <span>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nix {

/* The link count and the valid-root snapshot are meaningful together only
   while auxiliary links cannot be acquired or released. Use one ordinary
   file lock for that namespace, independent of the whole-store GC lock:
   evaluation must still progress while path collection is running.

   Concurrent readers of this instance share one open file description and
   one flock. Its last reader unlocks; another instance has its own open
   description and therefore participates in the same kernel lock normally.
   The descriptor stays open between operations, avoiding an open/close per
   auxiliary object. Holds retain this state for their eventual cleanup. */
struct GitObjectStore::AuxiliaryLock
{
    const std::filesystem::path file;
    std::mutex mutex;
    AutoCloseFD fd;
    size_t readers = 0;

    explicit AuxiliaryLock(std::filesystem::path file)
        : file(std::move(file))
    {
    }

    void lock_shared()
    {
        auto lock = std::lock_guard{mutex};
        if (!readers) {
            if (!fd)
                fd = openLockFile(file, true);
            while (!lockFile(fd.get(), ltRead, true))
                checkInterrupt();
        }
        ++readers;
    }

    void unlock_shared()
    {
        auto lock = std::lock_guard{mutex};
        assert(readers);
        if (!--readers)
            lockFile(fd.get(), ltNone, false);
    }

    AutoCloseFD lockExclusive() const
    {
        auto exclusive = openLockFile(file, true);
        while (!lockFile(exclusive.get(), ltWrite, true))
            checkInterrupt();
        return exclusive;
    }
};

GitObjectStore::GitObjectStore(std::filesystem::path dir, bool fsync, std::filesystem::path auxiliaryLockFile)
    : auxiliaryLock(
          std::make_shared<AuxiliaryLock>(auxiliaryLockFile.empty() ? dir / "gc.lock" : std::move(auxiliaryLockFile)))
    , dir(std::move(dir))
    , fsync(fsync)
    , blobsDir((this->dir / "blobs").string())
    , blobsXDir((this->dir / "blobs-x").string())
    , treesDir((this->dir / "trees").string())
{
}

void GitObjectStore::createDirectories() const
{
    createDirs(dir / "blobs");
    createDirs(dir / "blobs-x");
    createDirs(dir / "trees");
    createDirs(dir / "tmp");
}

Hash GitObjectStore::blobIdOfFile(const std::filesystem::path & path)
{
    merkle::ObjectHasher hasher{"blob", static_cast<uint64_t>(lstat(path).st_size)};
    readFile(path, hasher);
    return hasher.finish();
}

std::string GitObjectStore::idString(const Hash & id)
{
    return id.to_string(HashFormat::Base16, false);
}

/* The owned spelling `<subdirectory>/<xx>/<identifier>`. Both the shard
   `<xx>` and identifier use lowercase hexadecimal; the shard encodes the
   identifier's first byte (01 §9.10). */
static std::string nameUnder(const std::string & subdir, const Hash & id)
{
    auto bytes = std::as_bytes(std::span<const uint8_t>{&id.hash[0], id.hashSize});
    std::string s;
    s.reserve(subdir.size() + 1 + 2 + 1 + base16::encodedLength(id.hashSize));
    s += subdir;
    s += '/';
    /* Encode the shard and identifier directly into the result, without
       a separate identifier string. */
    base16::encodeTo(s, bytes.first(1));
    s += '/';
    base16::encodeTo(s, bytes);
    return s;
}

/* Whether `name` is a shard directory's: two lowercase hexadecimal digits,
   as `nameUnder` writes them. */
static bool isShardName(std::string_view name)
{
    return name.size() == 2 && name.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

static bool isObjectName(std::string_view name)
{
    return name.size() == 2 * regularHashSize(GitObjectStore::hashAlgo)
           && name.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

/* An auxiliary address denotes a regular file's bytes. In particular, a
   corrupt FIFO must neither block the sweep nor turn a source fallback into
   a read from an unrelated stream; a symlink is not its target's bytes.
   Size and bytes belong to this same opened inode, including during repair. */
static std::optional<std::pair<AutoCloseFD, PosixStat>>
openAuxiliary(const std::filesystem::path & file, std::optional<PosixStat> observed = std::nullopt)
{
    auto nonregular = [&] { return NotARegularFile("object %s is not a regular file!", PathFmt(file)); };
    if (!observed)
        observed = maybeLstat(file);
    if (!observed)
        return std::nullopt;
    /* Avoid opening existing devices at all. Supported publishers only
       replace auxiliaries with regular files; the opened-inode check also
       rejects a nonregular replacement before any read. */
    if (!S_ISREG(observed->st_mode))
        throw nonregular();
#ifdef _WIN32
    auto fd = openFileReadonly(file, FinalSymlink::DontFollow);
#else
    AutoCloseFD fd{open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
#endif
    if (!fd) {
        NativeSysError error("opening object %s", PathFmt(file));
        if (error.is(std::errc::no_such_file_or_directory) || error.is(std::errc::not_a_directory))
            return std::nullopt;
        /* Some nonregular kinds cannot be opened at all (a symlink with
           O_NOFOLLOW, for example). Preserve other I/O errors. */
        auto current = maybeLstat(file);
        if (!current)
            return std::nullopt;
        if (!S_ISREG(current->st_mode))
            throw nonregular();
        throw error;
    }
    auto st = nix::fstat(fd.get());
    if (!S_ISREG(st.st_mode))
        throw nonregular();
    return std::pair{std::move(fd), st};
}

void GitObjectStore::eachObjectFile(const std::filesystem::path & kindDir, fun<void(const std::filesystem::path &)> f)
{
    auto kind = maybeLstat(kindDir);
    if (!kind)
        return;
    if (!S_ISDIR(kind->st_mode))
        throw Error("object namespace %s is not a directory", PathFmt(kindDir));
    for (auto & entry : DirectoryIterator{kindDir}) {
        std::error_code isDir;
        if (std::filesystem::is_directory(entry.symlink_status(isDir))) {
            auto name = entry.path().filename().string();
            if (isShardName(name)) {
                for (auto & object : DirectoryIterator{entry.path()})
                    f(object.path());
            } else if (isObjectName(name))
                f(entry.path());
        } else
            f(entry.path());
    }
}

bool GitObjectStore::verify(RepairFlag repair)
{
    printInfo("checking object hashes...");
    bool errors = false;
    for (auto kind : {"blobs", "blobs-x", "trees"}) {
        bool tree = std::string_view{kind} == "trees";
        eachObjectFile(dir / kind, [&](const std::filesystem::path & object) {
            checkInterrupt();
            auto name = object.filename().string();
            auto what = tree ? "tree object" : "object";
            printMsg(lvlTalkative, "checking contents of %s", PathFmt(object));

            /* Size and bytes come from one open inode. Close it before
               diagnostics or removal: Windows retains a deleted name until
               the last handle closes. The repair reservation, rather than
               an inode comparison, excludes replacement before removal. */
            auto inspect = [&]() -> std::optional<std::string> {
                auto nonregular = [&] { return fmt("%s %s is not a regular file!", what, PathFmt(object)); };
                auto st = maybeLstat(object);
                if (!st)
                    return std::nullopt;
                /* A corrupt shard symlink may have been replaced with the
                   real shard directory by a concurrent repair and writer. */
                if (S_ISDIR(st->st_mode) && object.parent_path() == dir / kind && isShardName(name))
                    return std::nullopt;
                if (!S_ISREG(st->st_mode))
                    return nonregular();
                std::optional<std::pair<AutoCloseFD, PosixStat>> opened;
                try {
                    opened = openAuxiliary(object, st);
                } catch (NotARegularFile &) {
                    return nonregular();
                }
                if (!opened)
                    return std::nullopt;
                auto & [fd, metadata] = *opened;
                auto actual = [&] {
                    if (tree)
                        return merkle::treeId(
                            drainFD(fd.get(), {.size = static_cast<size_t>(metadata.st_size), .expected = false}));
                    merkle::ObjectHasher hasher{"blob", static_cast<uint64_t>(metadata.st_size)};
                    drainFD(fd.get(), hasher);
                    return hasher.finish();
                }();
                auto id = idString(actual);
                if (id != name)
                    return fmt("%s %s was modified! expected identifier %s, got '%s'", what, PathFmt(object), name, id);
                return std::nullopt;
            };

            auto observed = inspect();
            if (!observed)
                return;
            printError("%s", *observed);
            if (!repair) {
                errors = true;
                return;
            }

            bool removed = false;
            {
                /* Another verifier may have removed the corrupt inode and
                   an ingestion published its replacement since inspection.
                   Recheck only mismatches, with publication excluded; never
                   remove a sound replacement on the old observation. */
                auto lock = auxiliaryLock->lockExclusive();
                auto current = inspect();
                if (current) {
                    std::error_code ec;
                    removed = std::filesystem::remove(object, ec);
                    if (ec && ec != std::errc::no_such_file_or_directory && ec != std::errc::not_a_directory)
                        throw SystemError(ec, "removing corrupt %s %s", what, PathFmt(object));
                }
            }
            /* Logging may invoke other store operations; it is never part
               of the exclusive namespace reservation. */
            if (removed)
                printInfo("removed %s %s", what, PathFmt(object));
        });
    }
    return errors;
}

std::string GitObjectStore::blobFileName(const Hash & id, bool executable) const
{
    return nameUnder(executable ? blobsXDir : blobsDir, id);
}

std::string GitObjectStore::treeFileName(const Hash & id) const
{
    return nameUnder(treesDir, id);
}

std::filesystem::path GitObjectStore::blobFile(const Hash & id, bool executable) const
{
    return blobFileName(id, executable);
}

std::filesystem::path GitObjectStore::treeFile(const Hash & id) const
{
    return treeFileName(id);
}

std::filesystem::path GitObjectStore::tempPath(std::string_view kind) const
{
    /* The second is in the name because a temporary that is a hard link to
       a blob (`place`) shares the blob's inode, whose change time every
       later link of the blob advances: by the inode such a leftover would
       never be an hour old, and would keep the blob's link count above one
       for ever.  The counter starts at a random value, as `makeTempPath`'s
       does, against names an earlier process left. */
    static std::atomic<uint32_t> counter(std::random_device{}());
    return dir / "tmp"
           / fmt("%s-%d-%d-%d", kind, static_cast<long long>(time(nullptr)), getpid(), counter.fetch_add(1));
}

/* The second a temporary file's name carries (`tempPath`), if it does. */
static std::optional<time_t> tempTime(std::string_view name)
{
    auto parts = tokenizeString<std::vector<std::string>>(std::string{name}, "-");
    if (parts.size() != 4)
        return std::nullopt;
    if (auto t = string2Int<long long>(parts[1]))
        return static_cast<time_t>(*t);
    return std::nullopt;
}

/* A hard link `to` for the file `from`, by full paths; the error, empty
   on success, compares to `std::errc` on every platform. */
static std::error_code hardLink(const std::filesystem::path & from, const std::filesystem::path & to)
{
    std::error_code ec;
    std::filesystem::create_hard_link(from, to, ec);
    return ec;
}

/* The ingestion's pending index rows, **sharded 256 ways by the identifier's
   first byte** -- the object store's own layout, for the same reason.  The
   copy writes a directory's files on the walking thread and its
   subdirectories on a pool (`08` K17), so every worker records a row for
   every regular file it writes: one mutex over the whole set is a lock taken
   forty thousand times by eight threads, and it was the first reason the
   parallel copy was slower than the route it replaces.

   The finite map associates each (id, mode) with its first completed row.
   Rows stay private until the completed path is handed to the store;
   handing them over retires the pending map. Unused shards are never allocated. */
struct GitObjectStore::Hold::Pending
{
    /* Borrow the row's existing identity. Like the old binary key, Hash
       equality uses length and bytes, without an algorithm tag. */
    struct Key
    {
        const Hash & id;
        bool executable;

        Key(const Hash & id, bool executable) noexcept
            : id(id)
            , executable(executable)
        {
        }

        Key(const Row & row) noexcept
            : Key(row.id, row.executable)
        {
        }
    };

    struct KeyHash
    {
        using is_transparent = void;

        size_t operator()(Key key) const noexcept
        {
            return std::hash<std::string_view>{}({reinterpret_cast<const char *>(key.id.hash), key.id.hashSize})
                   ^ key.executable;
        }
    };

    struct KeyEqual
    {
        using is_transparent = void;

        bool operator()(Key a, Key b) const noexcept
        {
            return a.executable == b.executable && a.id == b.id;
        }
    };

    struct Shard
    {
        std::mutex mutex;
        /** (id, mode) -> the first completed file's row. */
        std::unordered_set<Row, KeyHash, KeyEqual> byKey;
    };

    /* A one-file ingestion occupies one shard. Allocate the others only
       when their identifier domain is actually used. Readers publish and
       acquire pointers because directory workers share this Pending. */
    std::array<std::atomic<Shard *>, 256> shards{};
    std::filesystem::path root;

    Shard & shard(size_t n)
    {
        auto * p = shards[n].load(std::memory_order_acquire);
        if (!p) {
            auto made = std::make_unique<Shard>();
            if (shards[n].compare_exchange_strong(p, made.get(), std::memory_order_acq_rel))
                p = made.release();
        }
        return *p;
    }

    ~Pending()
    {
        for (auto & shard : shards)
            delete shard.load();
    }
};

/* The shard an identifier falls in: its first byte, as `nameUnder` shards the
   object store's directories. */
static size_t shardOf(const Hash & id)
{
    return id.hashSize ? id.hash[0] : 0;
}

const std::filesystem::path & GitObjectStore::Hold::directory() const
{
    return lease->dir;
}

void GitObjectStore::Hold::setRoot(std::filesystem::path root) const
{
    /* A reused hold starts a new path projection. Its previous completed
       rows have already been handed to the store; abandoned rows need no
       publication. No walking worker may outlive this boundary. */
    for (auto & slot : pending->shards)
        delete slot.exchange(nullptr);
    pending->root = std::move(root);
}

const std::filesystem::path & GitObjectStore::Hold::root() const
{
    return pending->root;
}

std::optional<std::string> GitObjectStore::Hold::addRow(Row row) const
{
    auto & shard = pending->shard(shardOf(row.id));
    auto lock = std::lock_guard{shard.mutex};
    auto [i, inserted] = shard.byKey.insert(std::move(row));
    if (inserted)
        return std::nullopt;
    auto & name = i->name;
    return name.empty() ? pending->root.string() : (pending->root / name).string();
}

std::vector<GitObjectStore::Hold::Row> GitObjectStore::Hold::takeRows() const
{
    /* The caller owns a completed walk. Reserve before consuming any rows,
       so allocation failure leaves the complete pending relation intact.
       Publication owns the returned rows; no pending witness survives. */
    static_assert(std::is_nothrow_move_constructible_v<Row>);
    size_t count = 0;
    for (auto & slot : pending->shards)
        if (auto * shard = slot.load(std::memory_order_acquire))
            count += shard->byKey.size();
    std::vector<Row> rows;
    rows.reserve(count);
    for (auto & slot : pending->shards)
        if (std::unique_ptr<Pending::Shard> shard{slot.exchange(nullptr)}; shard)
            while (!shard->byKey.empty()) {
                /* Extract before moving: entries in a set are const. */
                auto node = shard->byKey.extract(shard->byKey.begin());
                rows.push_back(std::move(node.value()));
            }
    return rows;
}

std::optional<std::string> GitObjectStore::Hold::pendingHolder(const Hash & id, bool executable) const
{
    auto * shard = pending->shards[shardOf(id)].load(std::memory_order_acquire);
    if (!shard)
        return std::nullopt;
    auto lock = std::lock_guard{shard->mutex};
    auto i = shard->byKey.find(Pending::Key{id, executable});
    if (i == shard->byKey.end())
        return std::nullopt;
    auto & name = i->name;
    return name.empty() ? pending->root.string() : (pending->root / name).string();
}

/* The holder's name under the ingestion's root, made on the worker that wrote
   the file.  With no root set -- a caller that drives the object store
   directly, as the unit tests do -- the absolute path is kept instead and the
   registration relativises it; a name under a root never begins with a
   separator, so the two are told apart. */
static std::string nameUnderRoot(const std::filesystem::path & where, const std::filesystem::path & root)
{
    auto & r = root.native();
    auto & w = where.native();
    if (r.empty() || w.size() < r.size() || w.compare(0, r.size(), r) != 0)
        return w;
    return w.size() == r.size() ? std::string() : w.substr(r.size() + 1);
}

GitObjectStore::Hold::Hold(
    const std::filesystem::path & tmp,
    const std::function<void()> & atHoldMade,
    std::shared_ptr<AuxiliaryLock> auxiliaryLock)
    : pending(std::make_unique<Pending>())
    , lease(std::make_shared<Lease>(tmp, atHoldMade, std::move(auxiliaryLock)))
{
}

GitObjectStore::Hold::Lease::Lease(
    const std::filesystem::path & tmp,
    const std::function<void()> & atHoldMade,
    std::shared_ptr<AuxiliaryLock> auxiliaryLock)
    : auxiliaryLock(std::move(auxiliaryLock))
{
    /* As `createTempDirInStore` (local-store.cc): the sweep may remove the
       directory between its making and the lock -- it removes a hold it
       can lock -- so both are repeated until the directory exists locked.
       The name is unique to this process and this hold, so the lock is
       never contended by another maker; the sweep only tries it.

       The directory is made empty and its kind/shard directories made
       on demand under the lock, so that a hold under `tmp/` whose lock is free is
       either empty -- its maker between this `mkdir` and this `flock` --
       or a dead ingestion's, and the sweep's rule, "remove a hold you can
       lock, with its links", is right for both: it takes an empty
       directory from a maker that then finds it gone and makes another,
       and it takes a dead one's links.  Made before the lock, the kinds
       would be the maker's own second window: the sweep removing the
       directory partway through them left the maker's remaining
       `createDirs` to remake it *without* the kinds already made -- a
       hold that links nothing, every `holdObject` into a missing kind
       reading `ENOENT` as "no such object" -- and the sweep's own
       `deletePath` racing those `mkdir`s failed with `ENOTEMPTY` and
       abandoned the collection (`08` 1.5; `07`). */
    static std::atomic<uint32_t> counter(0);
    createDirs(tmp);
    while (true) {
        dir = tmp / fmt("hold-%d-%d", getpid(), counter.fetch_add(1));
        /* A PID may have been reused since a crashed ingestion. Its hold
           and unfinished temporary names are still its own: claim a new
           directory rather than adopting whatever is already here. */
        try {
            createDir(dir);
        } catch (const SystemError & e) {
            if (e.is(std::errc::file_exists))
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

GitObjectStore::Hold::Hold(std::shared_ptr<Lease> lease)
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
/* A live hold's writers have joined before release. Its directories are
   writable and its files are auxiliary hard links: no permission changes
   or estimate of freed bytes is needed. Generic deletePath stats every
   link for those purposes. Unlink directly, opening only directories and
   never following symlinks. Dead or partially removed holds still use the
   collector's general deletion path. */
static void releaseHoldContents(AutoCloseFD scan)
{
    auto fd = scan.get();
    AutoCloseDir entries{fdopendir(fd)};
    if (!entries)
        throw SysError("reading hold directory");
    scan.release();
    struct dirent * entry;
    while (errno = 0, entry = readdir(entries.get())) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        /* Joined writers leave this entry stable. A known directory cannot
           be unlinked as a file; unknown types retain the syscall fallback. */
        if (entry->d_type != DT_DIR) {
            if (unlinkat(fd, entry->d_name, 0) == 0 || errno == ENOENT)
                continue;
            if (errno != EISDIR && errno != EPERM)
                throw SysError("unlinking hold entry '%s'", entry->d_name);
        }
        AutoCloseFD child{openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (!child) {
            if (entry->d_type == DT_DIR && errno == ENOENT)
                continue;
            throw SysError("opening hold directory '%s'", entry->d_name);
        }
        releaseHoldContents(std::move(child));
        if (unlinkat(fd, entry->d_name, AT_REMOVEDIR) == -1 && errno != ENOENT)
            throw SysError("removing hold directory '%s'", entry->d_name);
    }
    if (errno)
        throw SysError("reading hold directory");
}
#endif

GitObjectStore::Hold::Lease::~Lease()
{
    if (!fd)
        return;
    try {
        auto lock = std::shared_lock{*auxiliaryLock};
#ifndef _WIN32
        /* Namespace exclusion lasts through rmdir, so cleanup can consume
           the root descriptor along with the directory stream. */
        releaseHoldContents(std::move(fd));
        if (::rmdir(dir.c_str()) == -1 && errno != ENOENT)
            throw SysError("removing hold %s", PathFmt(dir));
#else
        deletePath(dir);
#endif
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

GitObjectStore::Hold::Names
GitObjectStore::Hold::namesFor(const GitObjectStore & objects, const merkle::TreeEntry & object) const
{
    /* The caller owns the public namespace; the lease owns only the hold.
       Keep the global spelling and Windows native conversion before any
       held shard is created. The generated suffix is ASCII at every width. */
    size_t kind = object.mode == merkle::Mode::Directory ? 2 : object.mode == merkle::Mode::Executable ? 1 : 0;
    const std::array<const std::string *, 3> prefixes{&objects.blobsDir, &objects.blobsXDir, &objects.treesDir};
    constexpr std::array<OsStringView, 3> kinds{OS_STR("blobs"), OS_STR("blobs-x"), OS_STR("trees")};
    Names result{
#ifdef _WIN32
        std::filesystem::path(nameUnder(*prefixes[kind], object.hash)).native(),
#else
        nameUnder(*prefixes[kind], object.hash),
#endif
        std::filesystem::path::string_type{}};
    auto & [dst, held] = result;
    auto idSize = base16::encodedLength(object.hash.hashSize);
    auto name = dst.end() - idSize;
    auto shard = name - 3; // Two shard digits and the following separator.
    held.reserve(kinds[kind].size() + idSize + 4);
    held += kinds[kind];
    held += std::filesystem::path::preferred_separator;
    held.append(shard, shard + 2);
    std::call_once(lease->shards[kind * 256 + object.hash.hash[0]], [&] { createDirs(lease->dir / held); });
    held += std::filesystem::path::preferred_separator;
    held.append(name, dst.end());
    return result;
}

std::filesystem::path
GitObjectStore::Hold::linkFor(const GitObjectStore & objects, const merkle::TreeEntry & object) const
{
    return lease->dir / namesFor(objects, object).held;
}

GitObjectStore::Hold GitObjectStore::hold() const
{
    return Hold(dir / "tmp", atHoldMade, auxiliaryLock);
}

/* Hold the object file `object`, if it exists: a link to it from the hold.
   True when it is held (or was already); false when there is no such
   object.  A hold the filesystem refuses (its link limit, a full
   directory index) leaves the object unheld and says so: what a
   concurrent sweep may then take, the verifier reports and `--optimise`
   re-enters, as before the hold existed. */
bool GitObjectStore::Hold::linkObject(
    const std::filesystem::path::string_type & object, const std::filesystem::path::string_type & held) const
{
#ifdef _WIN32
    auto ec = hardLink(object, lease->dir / held);
#else
    if (::linkat(AT_FDCWD, object.c_str(), lease->fd.get(), held.c_str(), 0) != -1)
        return true;
    std::error_code ec{errno, std::generic_category()};
#endif
    if (!ec || ec == std::errc::file_exists)
        return true;
    if (ec == std::errc::no_such_file_or_directory)
        return false;
    if (ec == std::errc::too_many_links || ec == std::errc::no_space_on_device) {
        printInfo("cannot hold object %s: %s", PathFmt(object), ec.message());
        return true;
    }
    throw Error("creating hard link from %1% to %2%: %3%", PathFmt(object), PathFmt(lease->dir / held), ec.message());
}

bool GitObjectStore::Hold::holdObject(const GitObjectStore & objects, const merkle::TreeEntry & object) const
{
    auto [file, held] = namesFor(objects, object);
    return linkObject(file, held);
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
GitObjectStore::holderOrForget(const Hash & id, bool executable, RepairFlag repair, PosixStat * stOut)
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
        /* Each row is one ingestion's candidate, not the whole set of
           holders. Validate that candidate, forgetting only the row read.
           The candidate cursor also advances in a read-only store, whose
           forget operation deliberately does nothing. */
        if (st && validHolderMetadata(*row, executable, *st) && (!repair || blobIdOfFile(row->file) == id)) {
            if (stOut)
                *stOut = *st;
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
    GitObjectStore & objects,
    const std::string & file,
    Descriptor dirFd,
    const CanonPath & name,
    const std::filesystem::path & where,
    const PosixStat & before,
    Accept && accept)
{
    auto filePath = [&] { return PathFmt(std::filesystem::path(file)); };
    auto tempLink = objects.tempPath("link");
    if (auto ec = hardLink(file, tempLink)) {
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
#endif

bool GitObjectStore::link(
    const Hash & id,
    bool executable,
    Descriptor dirFd,
    const CanonPath & name,
    const std::filesystem::path & where,
    std::optional<uint64_t> size,
    RepairFlag repair,
    const Hold & hold,
    PosixStat * linked)
{
#ifdef _WIN32
    return false;
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
            return false;
        if (auto ec = linkPathIntoDir(*mine, dirFd, name)) {
            if (ec == std::errc::no_such_file_or_directory || ec == std::errc::too_many_links
                || ec == std::errc::no_space_on_device)
                return false;
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
                    return false;
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
            if (!validHolderMetadata(*holder, executable, acquired) || (repair && blobIdOfFile(where) != id)) {
                forget();
                continue;
            }
            cleanup.cancel();
            st = acquired;
            break;
        }
    }
    if (!st)
        return false;
    /* This path now holds the blob even if the donor is collected before
       registration. Keep its own witness, using the status already read.
       A concurrent worker may have claimed a different holder meanwhile. */
    if (!mine)
        if (auto other = hold.addRow(
                Hold::Row{
                    .id = id,
                    .executable = executable,
                    .ino = st->st_ino,
                    .dev = st->st_dev,
                    .size = static_cast<uint64_t>(st->st_size),
                    .name = nameUnderRoot(where, hold.root()),
                }))
            if (auto placed =
                    linkOver(*this, *other, dirFd, name, where, *st, [](auto &, const auto &) { return true; }))
                st->st_ino = placed->ino;
    if (linked)
        *linked = *st;
    return true;
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
    /* The file is written and canonicalised.  Either the store already has
       a holder for its blob -- then this file is replaced by a link to it,
       exactly as before -- or it becomes the holder itself, which costs a
       row and no filesystem call at all. */
    auto after = before;
    auto replaceBy = [&](const std::string & file, auto && accept) {
        auto placed = linkOver(*this, file, dirFd, name, where, after, std::forward<decltype(accept)>(accept));
        if (placed)
            after.st_ino = placed->ino;
        return placed;
    };

    /* This ingestion's own earlier file for the blob comes first (law 3
       within one path), then the index's holder. */
    bool entered = true;
    if (auto mine = hold.pendingHolder(id, executable)) {
        if (auto st = maybeLstatOf(*mine)) {
            if (st->st_ino == before.st_ino && st->st_dev == before.st_dev)
                return Placed{before.st_ino, false};
            return replaceBy(*mine, [](auto &, const auto &) { return true; });
        }
        return std::nullopt;
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
                        && (!repair || blobIdOfFile(pin) == id))
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

    /* Record this ingestion's own holder on a miss and on registered
       reuse: the donor may be collected before this path registers.
       The row is pending until registration. A full directory index
       cannot be met here, there being no directory to fill (K11). */
    if (entered && failPlaceWithNoSpace && failPlaceWithNoSpace(where)) {
        printInfo("cannot enter %s: %s", PathFmt(where), std::make_error_code(std::errc::no_space_on_device).message());
        return std::nullopt;
    }
    auto other = hold.addRow(
        Hold::Row{
            .id = id,
            .executable = executable,
            .ino = after.st_ino,
            .dev = after.st_dev,
            .size = static_cast<uint64_t>(after.st_size),
            .name = nameUnderRoot(where, hold.root()),
        });
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
    /* The file was written because the store had no holder for its blob.
       It becomes the holder: one `fstatat` for its inode and a pending
       row, and no `linkat` on a miss. Publication into the pending set is
       atomic: an earlier lookup miss cannot rule out another worker
       completing the same blob before this file was written. */
    PosixStat st;
    if (fstatat(dirFd, name.rel_c_str(), &st, AT_SYMLINK_NOFOLLOW) == -1)
        throw SysError("getting status of %1%", PathFmt(where));
    /* A file streamed past the buffer was written without a look, so the
       decision is taken here and it is `place`'s: a holder the store
       already has is linked over this file.  A file written after `link`
       missed needs no second look (`looked`), which is what keeps the
       first entry of a fresh tree at one index probe a file. */
    if (!looked && place(id, executable, dirFd, name, where, st, repair, hold))
        return;
    auto other = hold.addRow(
        Hold::Row{
            .id = id,
            .executable = executable,
            .ino = st.st_ino,
            .dev = st.st_dev,
            .size = static_cast<uint64_t>(st.st_size),
            .name = nameUnderRoot(where, hold.root()),
        });
    if (other)
        place(id, executable, dirFd, name, where, st, repair, hold);
#endif
}

/* Write auxiliary bytes to the fresh temporary file `tmp`, made read-only,
   link it into place and hold it; an entry that appeared meanwhile stands,
   and is held instead. A successfully published temporary stays in the
   hold as its protecting link; every unsuccessful one is removed.
   Under `fsync` the bytes and final mode are flushed before publication,
   and the directory afterward. Auxiliary modification times are not tree
   metadata: no reader uses them, and these files never become regular-file
   holders. Those holders retain the store's canonical timestamp.

   The invariant (`08` K16): the object file is never at one link without
   a hold while its ingestion is in flight.  It is linked into place while
   the temporary still links the same inode, so it has two links from its
   first instant, and the temporary itself is the hold's link;
   an existing file or a competing publisher's object is held instead. */
void GitObjectStore::Hold::writeObjectFile(
    const GitObjectStore & objects, const merkle::TreeEntry & object, std::string_view bytes) const
{
    bool executable = object.mode == merkle::Mode::Executable;
    bool fsync = objects.fsync;
    const auto & atFsync = objects.atFsync;
    auto [dst, tmp] = namesFor(objects, object);
    if (linkObject(dst, tmp))
        return;
    /* This name belongs to an already unique, locked hold. Its lifetime
       is the hold's, so it needs neither the global temporaries' timestamp
       nor their process ID. Own-row views of a hold share this process-wide
       sequence. The lease owns the directory resolved by every relative
       operation; full paths are needed only for Windows and diagnostics. */
    static std::atomic<uint64_t> counter{0};
    auto heldSize = tmp.size();
    auto suffix = ".tmp-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    tmp.append(suffix.begin(), suffix.end()); // The suffix is ASCII on every platform.
    auto tmpPath = [&] { return lease->dir / tmp; };
    auto unlinkTemporary = [&] {
#ifdef _WIN32
        unlinkIfExists(tmpPath());
#else
        if (::unlinkat(lease->fd.get(), tmp.c_str(), 0) == -1 && errno != ENOENT)
            throw SysError("removing %s", PathFmt(tmpPath()));
#endif
    };
    bool removeTemporary = true;
    Finally removeTmp{[&] {
        if (!removeTemporary)
            return;
        try {
            unlinkTemporary();
        } catch (...) {
            ignoreExceptionExceptInterrupt();
        }
    }};
    /* The caller holds the auxiliary namespace shared throughout. Other
       publishers may win this address, but sweep and repair cannot remove it. */
    {
        mode_t mode = executable ? 0755 : 0644;
#ifdef _WIN32
        AutoCloseFD fd = openNewFileForWrite(tmpPath(), mode, {.writeOnly = true});
#else
        AutoCloseFD fd{openat(lease->fd.get(), tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, mode)};
#endif
        if (!fd)
            throw SysError("creating file %1%", PathFmt(tmpPath()));
        writeFull(fd.get(), bytes);
#ifdef _WIN32
        /* Keep the existing Windows metadata implementation until an
           equivalent permission-only operation is provided there. */
        canonicaliseTimestampAndPermissions(tmpPath());
#else
        /* Creation's mode is filtered by the caller's umask. Set the
           exact readable mode before flushing, independently of it. */
        if (::fchmod(fd.get(), executable ? 0555 : 0444) == -1)
            throw SysError("changing permissions of auxiliary object");
#endif
        if (fsync) {
            if (atFsync)
                atFsync(tmpPath());
            fd.fsync();
        }
    }
    auto publish = [&] {
#ifdef _WIN32
        return hardLink(tmpPath(), dst);
#else
        return ::linkat(lease->fd.get(), tmp.c_str(), AT_FDCWD, dst.c_str(), 0) == -1
                   ? std::error_code{errno, std::generic_category()}
                   : std::error_code{};
#endif
    };
    auto ec = publish();
    if (ec == std::errc::no_such_file_or_directory) {
        /* The first object of this shard (`createDirectories`). */
        createDirs(std::filesystem::path(dst).parent_path());
        ec = publish();
    }
    if (ec) {
        unlinkTemporary();
        removeTemporary = false;
        if (ec == std::errc::file_exists) {
            /* The losing temporary is gone; reuse its name to hold the winner. */
            tmp.resize(heldSize);
            if (!linkObject(dst, tmp))
                throw Error("object %s disappeared during publication", PathFmt(dst));
            return;
        }
        if (ec == std::errc::no_space_on_device) {
            /* The object is not kept; every reader tolerates its absence. */
            printInfo("cannot write object %s: %s", PathFmt(dst), ec.message());
            return;
        }
        throw Error("creating hard link from %1% to %2%: %3%", PathFmt(tmpPath()), PathFmt(dst), ec.message());
    }
    /* The temporary is inside the locked hold. Keep it as the
       protecting link: publication already leaves two links, with
       no interval in which GC can take the object. A unique name
       also keeps concurrent writers from reading a partial file. */
    removeTemporary = false;
    if (fsync)
        syncParent(dst);
}

void GitObjectStore::putObject(const merkle::TreeEntry & object, std::string_view bytes, const Hold & hold)
{
    auto lock = std::shared_lock{*auxiliaryLock};
    hold.writeObjectFile(*this, object, bytes);
}

void GitObjectStore::putRootTree(const merkle::TreeEntry & root, const Hold & hold)
{
    if (auto body = merkle::syntheticRootTree(root))
        putObject({merkle::Mode::Directory, merkle::treeId(*body)}, *body, hold);
}

bool GitObjectStore::holdObject(const merkle::TreeEntry & object, const Hold & hold) const
{
    auto lock = std::shared_lock{*auxiliaryLock};
    return hold.holdObject(*this, object);
}

std::optional<std::string> GitObjectStore::readTree(const Hash & id) const
{
    if (counters)
        counters->treesRead++;
    auto opened = openAuxiliary(treeFile(id));
    if (!opened)
        return std::nullopt;
    return drainFD(opened->first.get(), {.size = static_cast<size_t>(opened->second.st_size), .expected = false});
}

std::optional<std::string> GitObjectStore::readBlob(const Hash & id, bool executable) const
{
    auto opened = openAuxiliary(blobFile(id, executable));
    if (!opened)
        return std::nullopt;
    return drainFD(opened->first.get(), {.size = static_cast<size_t>(opened->second.st_size), .expected = false});
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

    /* The file holds the entries alone; the parser reads the size git's
       header puts before them. */
    try {
        {
            auto framed = std::to_string(body.size()) + '\0';
            framed.append(body);
            StringSource source{framed};
            git::parseTree(into, source, hashAlgo);
        }
        /* The identifier names the canonical encoding of this map, not
           another spelling or ordering that the raw Git parser accepts. */
        if (merkle::serialiseTree(into.tree) != body)
            throw Error("tree body is not its canonical encoding");
    } catch (Error & e) {
        throw Error("malformed tree object %s: %s", idString(id), e.msg());
    }
    return std::move(into.tree);
}

std::optional<merkle::Tree> GitObjectStore::readVerifiedTree(const Hash & id) const
{
    std::optional<std::string> body;
    try {
        body = readTree(id);
    } catch (NotARegularFile & e) {
        warn("%s; not used\nYou should run `nix-store --verify --check-contents --repair` to fix it", e.msg());
        return std::nullopt;
    }
    if (!body)
        return std::nullopt;
    if (merkle::treeId(*body) != id) {
        warn(
            "tree object %s does not hash to its name; not used"
            "\nYou should run `nix-store --verify --check-contents --repair` to fix it",
            PathFmt(treeFile(id)));
        return std::nullopt;
    }
    try {
        return parseTreeBody(id, *body);
    } catch (Error & e) {
        warn("%s; not used\nYou should run `nix-store --verify --check-contents --repair` to fix it", e.msg());
        return std::nullopt;
    }
}

GitObjectStore::BlobInodes GitObjectStore::blobInodes() const
{
    BlobInodes inodes;
#ifndef _WIN32
    createDirectories();
    /* The index, not a walk of `.objects/blobs`: one query over the rows
       the store wrote, which is both cheaper and more complete than the
       walk it replaces (design.md section 5).  The symlink-target blobs
       under `blobs/` are not a path's file and are not in the table, which
       is right: `known` answers regular files alone. */
    if (index && index->inodes)
        index->inodes(inodes);
#endif
    return inodes;
}

GitObjectStore::SweepStats
GitObjectStore::sweep(fun<uint64_t(fun<void(const ObjectHash &)>)> forEachLiveObjectHash, const Owned * owned)
{
    SweepStats stats;
    createDirectories();
    auto lock = auxiliaryLock->lockExclusive();

    /* The files this sweep enumerates and removes: the whole object store,
       or the part of it the caller owns (`Owned`).  Reachability below is
       computed over the object store itself either way -- `readTree` and
       `blobFile` are its own -- so an object a part this sweep may not
       touch reaches is kept. */
    const auto & sweptDir = owned ? owned->dir : dir;
    auto remove = [&](const std::filesystem::path & file) {
        if (owned)
            owned->remove(file);
        else
            unlinkIfExists(file);
    };
    /* The store's own four directories are made before they are iterated
       (`08` K12); a part of it the caller owns is read as it is -- an
       absent directory there is empty, and the sweep writes nothing into
       it, only unlinks.  (On the overlay store the upper layer holds no
       `.objects/blobs` until something has been written into it.) */
    auto sweptSub = [&](std::string_view sub) -> std::optional<std::filesystem::path> {
        auto sub_ = sweptDir / sub;
        if (owned && !pathExists(sub_))
            return std::nullopt;
        return sub_;
    };

    /* A hold promises an object address, not a particular inode: repair
       can replace a corrupt published inode while the old link survives.
       The namespace lock keeps live holds stable throughout this scan.
       Rows and coalescing pins at a hold's root are not auxiliary claims. */
    constexpr std::array<std::string_view, 3> kinds{"blobs", "blobs-x", "trees"};
    std::array<boost::unordered_flat_set<std::string>, 3> heldObjects;
    auto markHeld = [&](const std::filesystem::path & held) {
        for (size_t kind = 0; kind < kinds.size(); ++kind) {
            auto directory = held / kinds[kind];
            auto st = maybeLstat(directory);
            if (!st || !S_ISDIR(st->st_mode))
                continue;
            auto mark = [&](const std::filesystem::directory_entry & entry, std::string_view shard) {
                auto name = entry.path().filename().string();
                constexpr auto length = 2 * regularHashSize(hashAlgo);
                std::string_view id{name.data(), std::min(name.size(), length)};
                if (!isObjectName(id) || (!shard.empty() && !id.starts_with(shard)))
                    return;
                auto suffix = std::string_view(name).substr(length);
                if (!suffix.empty()
                    && (!suffix.starts_with(".tmp-") || suffix.size() == 5
                        || suffix.substr(5).find_first_not_of("0123456789") != suffix.npos))
                    return;
                heldObjects[kind].emplace(id);
            };
            for (const auto & entry : DirectoryIterator{directory}) {
                auto shard = entry.path().filename().string();
                if (isShardName(shard) && std::filesystem::is_directory(entry.symlink_status())) {
                    for (const auto & file : DirectoryIterator{entry.path()})
                        mark(file, shard);
                } else
                    /* Earlier holders used <kind>/<id> without shards.
                       A claim is its name, regardless of its inode kind;
                       even a directory with that name is not descended. */
                    mark(entry, {});
            }
        }
    };

    /* Temporary files a crash left behind: none lives an hour.  First,
       before the blobs are looked at, so that a leftover link to a blob --
       which holds the blob's link count above one -- goes before the blob
       is judged.  The age is the name's when the name carries one
       (`tempPath`: a link's inode is the blob's, and its change time the
       blob's), else the inode's change time (a name an earlier build
       made). */
    auto now = time(nullptr);
    if (auto tmpDir = sweptSub("tmp"))
        for (auto & entry : DirectoryIterator{*tmpDir}) {
            checkInterrupt();
            if (std::filesystem::is_directory(entry.symlink_status())) {
                /* An ingestion's hold (`Hold`).  Locked: the ingestion is in
                   flight, and its link names keep its object addresses. Free: its
                   process is gone; the directory goes now, with its links, so
                   that the objects it held are judged in this sweep -- as the
                   collector removes a `tmp-*` directory it can lock (gc.cc).
                   The lock is taken, and the directory removed, through the
                   object store's own path: on a part the caller owns (the
                   overlay store's upper layer) the file the ingestion locked
                   is the merged view's, not the layer's, and a hold is never
                   the lower store's, so the removal whites nothing out. */
                auto held = dir / "tmp" / entry.path().filename();
                auto fd = openDirectory(held, FinalSymlink::DontFollow);
                if (!fd) {
                    if (errno == ENOENT)
                        continue;
                    throw SysError("opening hold %s", PathFmt(held));
                }
                if (!lockFile(fd.get(), ltWrite, false)) {
                    stats.holds++;
                    markHeld(held);
                    continue;
                }
                /* A removal that fails is a hold this sweep does not take,
                   not the end of the collection (01 section 9.10 law 5, which
                   sweeps on): a hold an older Nix is filling before its
                   lock answers `ENOTEMPTY`, one another sweep took first
                   `ENOENT`. The surviving link names still keep their object
                   addresses, even when repair replaced the linked inode. */
                try {
                    if (failHoldRemoval && failHoldRemoval(held))
                        throw SysError(ENOTEMPTY, "cannot unlink %s", PathFmt(held));
                    deletePath(held);
                } catch (SysError & e) {
                    printInfo("cannot remove the hold %s: %s", PathFmt(held), e.msg());
                    stats.holds++;
                    markHeld(held);
                }
                continue;
            }
            auto made = tempTime(entry.path().filename().string());
            if (!made)
                if (auto st = maybeLstat(entry.path()))
                    made = st->st_ctime;
            if (made && now - *made > 3600)
                remove(entry.path());
        }

    /* The live roots: the object hash of every valid path -- a tree id,
       or a plain file's blob id, which names no tree.  A root is told from
       a tree a reachable tree names, so that the two are counted apart
       when missing. */
    struct Live
    {
        Hash id;
        bool root;
    };

    std::vector<Live> live;
    stats.unmigrated = forEachLiveObjectHash([&](const ObjectHash & hash) { live.push_back({hash.hash, true}); });

    /* A valid path whose object hash is not yet known (a row older than
       schema 11) is a root this sweep cannot see: while any remains
       nothing but the temporary files is removed. */
    if (stats.unmigrated == 0) {
        /* Trees reachable from a live root stay, and every blob such a tree
           names is kept whatever its link count (law 4).  A tree whose body
           is not its name's abandons the removal phase (`corruptTrees`); a
           tree a root names but the store lacks is counted and keeps
           nothing (`unentered`). */
        boost::unordered_flat_set<std::string> reachable;
        boost::unordered_flat_set<std::string> named;
        while (!live.empty()) {
            auto [id, root] = live.back();
            live.pop_back();
            auto key = idString(id);
            if (!reachable.insert(key).second)
                continue;
            std::optional<std::string> body;
            try {
                body = readTree(id);
            } catch (NotARegularFile & e) {
                warn("%s", e.msg());
                stats.corruptTrees++;
                continue;
            }
            if (!body) {
                /* No tree of that name.  A tree a present tree names is a
                   missing subtree (`missingSubtrees`).  A plain file's
                   object hash is its blob's id, which names no tree; the
                   blob is what the root reaches, kept (law 4) whatever its
                   link count.  Anything else is a live root whose object
                   the store lacks (`SweepStats::unentered`): nothing
                   beneath it is kept, and the sweep goes on. */
                if (!root)
                    stats.missingSubtrees++;
                else if (
                    (index && index->lookup
                     && (index->lookup(id, false, Holder::initialCursor)
                         || index->lookup(id, true, Holder::initialCursor)))
                    || pathExists(blobFile(id, false)))
                    /* A plain-file root: its object hash is its blob's id,
                       and the path *is* the holder.  (The `blobFile` arm is
                       for a symlink-target blob that is also a root's id.) */
                    named.insert(key);
                else
                    stats.unentered++;
                continue;
            }
            std::optional<merkle::Tree> entries;
            if (merkle::treeId(*body) == id)
                try {
                    entries = parseTreeBody(id, *body);
                } catch (Error & e) {
                    warn("tree object %s does not parse: %s", PathFmt(treeFile(id)), e.msg());
                }
            else
                warn("tree object %s does not hash to its name", PathFmt(treeFile(id)));
            if (!entries) {
                stats.corruptTrees++;
                continue;
            }
            for (auto & [name, entry] : *entries) {
                if (entry.mode == merkle::Mode::Directory)
                    live.push_back({entry.hash, false});
                else
                    named.insert(idString(entry.hash));
            }
        }
        if (stats.corruptTrees)
            warn(
                "%d corrupt tree objects; the object store was not swept."
                "\nYou should run `nix-store --verify --check-contents --repair` to fix them",
                stats.corruptTrees);
        if (stats.unentered || stats.missingSubtrees) {
            /* Roots and subtrees apart: a valid path without its object,
               and a tree a present tree names. */
            std::string what;
            if (stats.unentered)
                what = fmt("%d valid paths have no object in the object store", stats.unentered);
            if (stats.missingSubtrees)
                what +=
                    fmt("%s%d trees named by a present tree are missing",
                        what.empty() ? "" : ", and ",
                        stats.missingSubtrees);
            warn(
                "%s (written by an older Nix, or being entered by `nix-store --optimise` now); nothing is kept for them."
                "\nRun `nix-store --optimise` to enter them (or `nix-store --verify --check-contents --repair`)",
                what);
        }
        if (!stats.corruptTrees) {
            /* Trees reachable from a live root, or whose address a live
               ingestion holds, stay. Additional links keep older layouts. */
            if (auto treesDir = sweptSub("trees"))
                eachObjectFile(*treesDir, [&](const std::filesystem::path & file) {
                    checkInterrupt();
                    if (reachable.count(file.filename().string()))
                        return;
                    if (heldObjects[2].contains(file.filename().string())) {
                        stats.treesHeld++;
                        return;
                    }
                    auto st = maybeLstat(file);
                    if (!st)
                        return;
                    if (S_ISDIR(st->st_mode) || st->st_nlink != 1) {
                        stats.treesHeld++;
                        return;
                    }
                    remove(file);
                    stats.treesRemoved++;
                });

            /* Blobs: an address neither reachable nor held, with no link
               but the store's own, is unreferenced. */
            for (size_t kind = 0; kind < 2; ++kind) {
                auto blobsDir = sweptSub(kinds[kind]);
                if (!blobsDir)
                    continue;
                eachObjectFile(*blobsDir, [&](const std::filesystem::path & file) {
                    checkInterrupt();
                    if (counters)
                        counters->blobsStatted++;
                    auto st = maybeLstat(file);
                    if (!st)
                        return;
                    if (S_ISDIR(st->st_mode) || st->st_nlink != 1
                        || heldObjects[kind].contains(file.filename().string())) {
                        stats.blobsKept++;
                        if (st->st_nlink > 1)
                            stats.sharedBytes +=
                                static_cast<uint64_t>(st->st_nlink - 2) * static_cast<uint64_t>(st->st_size);
                        return;
                    }
                    if (named.count(file.filename().string())) {
                        stats.blobsKept++;
                        return;
                    }
                    printMsg(lvlTalkative, "deleting unreferenced object %1%", PathFmt(file));
                    remove(file);
                    stats.blobsRemoved++;
                });
            }
        }
    }

    return stats;
}

} // namespace nix
