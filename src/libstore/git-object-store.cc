#include "nix/store/git-object-store.hh"
#include "nix/store/pathlocks.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/base-n.hh"
#include "nix/util/git.hh"
#include "nix/util/file-system.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/finally.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/strings.hh"
#include "nix/util/util.hh"

#include <boost/unordered/unordered_flat_set.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <mutex>
#include <shared_mutex>
#include <array>

#include <atomic>
#include <thread>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <random>
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
    merkle::BlobHasher hasher{static_cast<uint64_t>(lstat(path).st_size)};
    readFile(path, hasher);
    return hasher.finish();
}

std::string GitObjectStore::idString(const Hash & id)
{
    return id.to_string(HashFormat::Base16, false);
}

/* `<subdirectory>/<xx>/<identifier>` appended, without
   `std::filesystem::path`: the class splits and reallocates its components
   at every `/`, and the object store names a file three times for every
   regular file it enters.  `<xx>` is the identifier's first byte in
   hexadecimal, git's own shard (01 §9.10). */
static std::string nameUnder(const std::string & subdir, const Hash & id)
{
    auto bytes = std::as_bytes(std::span<const uint8_t>{&id.hash[0], id.hashSize});
    std::string s;
    s.reserve(subdir.size() + 1 + 2 + 1 + base16::encodedLength(id.hashSize));
    s += subdir;
    s += '/';
    /* The identifier encoded into `s`, not into a string of its own: this
       names a blob's file once or twice for every regular file entered
       (`05` section 6).  The shard is the same encoding of the first byte. */
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

void GitObjectStore::eachObjectFile(const std::filesystem::path & kindDir, fun<void(const std::filesystem::path &)> f)
{
    if (!pathExists(kindDir))
        return;
    for (auto & entry : DirectoryIterator{kindDir}) {
        std::error_code isDir;
        if (entry.is_directory(isDir)) {
            if (!isShardName(entry.path().filename().string()))
                continue;
            for (auto & object : DirectoryIterator{entry.path()})
                f(object.path());
        } else
            f(entry.path());
    }
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

   A writer thread drains a large ingestion into the store's index while
   the copy runs. A small ingestion drains on the registering thread. Two
   things make that cheap.  A shard's queue is taken by *swap* under its lock,
   so a drain copies no rows; and what a later `link` needs -- the file this
   ingestion already wrote for a blob (law 3 within one path) -- is kept in
   `byKey`, which maps the blob to its name and does not index into the queue,
   so the queue may be emptied under it.

   Neither a writer nor unused shards are allocated for the common case
   of a store object containing one regular file. */
struct GitObjectStore::Hold::Pending
{
    struct Shard
    {
        std::mutex mutex;
        /** Rows not yet handed to the sink; taken by swap. */
        std::vector<Row> queue;
        /** (id, mode) -> the holder's name, kept after the row is written. */
        boost::unordered_flat_map<std::string, std::string> byKey;
    };

    /* A one-file ingestion occupies one shard. Allocate the others only
       when their identifier domain is actually used. Readers publish and
       acquire pointers because directory workers share this Pending. */
    std::array<std::atomic<Shard *>, 256> shards{};
    std::atomic<size_t> queued{0};

    /* Auxiliary objects are protected under the same first-byte shards
       as their published names. Independent directory workers must not
       create every temporary in one shared directory. */
    std::array<std::once_flag, 3 * 256> auxiliaryShards;

    std::mutex wake;
    std::condition_variable ready;
    bool stopping = false;
    std::thread writer;
    std::exception_ptr error;

    std::filesystem::path root;
    std::function<void(std::vector<Row> &&)> sink;

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

/* Rows queued before a background writer is needed. */
static size_t holdBatchSize()
{
    /* Settable for the measurement alone (`NIX_HOLDER_BATCH`); the default is
       what measured best. */
    static size_t n = [] {
        if (auto * v = getenv("NIX_HOLDER_BATCH")) {
            auto k = strtoull(v, nullptr, 10);
            if (k > 0)
                return size_t(k);
        }
        return size_t(16384);
    }();
    return n;
}

static std::string pendingKey(const Hash & id, bool executable)
{
    std::string key(reinterpret_cast<const char *>(id.hash), id.hashSize);
    key += executable ? 'x' : ' ';
    return key;
}

/* The shard an identifier falls in: its first byte, as `nameUnder` shards the
   object store's directories. */
static size_t shardOf(const Hash & id)
{
    return id.hashSize ? id.hash[0] : 0;
}

/* Every queued row, from every shard, as one batch: each shard's queue taken
   by swap under its lock and none held while the sink runs, since the sink
   takes the database's lock and nothing that waits on that may hold a
   shard's.  Over the `Pending` and not the `Hold`, because a `Hold` is moved
   -- into an `optional`, out of `hold()` -- and the writer thread must not
   hold a pointer to one. */
static std::vector<GitObjectStore::Hold::Row> takePending(GitObjectStore::Hold::Pending & pending)
{
    std::vector<GitObjectStore::Hold::Row> batch;
    for (auto & slot : pending.shards) {
        auto * shard = slot.load(std::memory_order_acquire);
        if (!shard)
            continue;
        std::vector<GitObjectStore::Hold::Row> taken;
        {
            auto lock = std::lock_guard{shard->mutex};
            if (shard->queue.empty())
                continue;
            taken.swap(shard->queue);
        }
        if (batch.empty())
            batch = std::move(taken);
        else
            batch.insert(batch.end(), std::make_move_iterator(taken.begin()), std::make_move_iterator(taken.end()));
    }
    pending.queued.fetch_sub(batch.size());
    return batch;
}

const std::filesystem::path & GitObjectStore::Hold::directory() const
{
    return dir;
}

void GitObjectStore::Hold::setRoot(std::filesystem::path root) const
{
    pending->root = std::move(root);
}

const std::filesystem::path & GitObjectStore::Hold::root() const
{
    return pending->root;
}

void GitObjectStore::Hold::setSink(std::function<void(std::vector<Row> &&)> sink) const
{
    /* One hold may carry several ingestions in turn -- `optimiseStore` walks
       every valid path under one, the verifier's repair re-enters a path
       under one, the builder enters each output under one -- so a sink may
       replace a sink.  The writer of the ingestion that is ending is stopped
       and its rows drained *before* the next one's starts: assigning over a
       joinable `std::thread` terminates the process, which is how the unit
       suite found this. */
    flush();
    pending->stopping = false;
    for (auto & slot : pending->shards)
        delete slot.exchange(nullptr);
    pending->queued = 0;
    pending->sink = std::move(sink);
}

std::optional<std::string> GitObjectStore::Hold::addRow(Row row) const
{
    auto key = pendingKey(row.id, row.executable);
    size_t queued;
    {
        auto & shard = pending->shard(shardOf(row.id));
        auto lock = std::lock_guard{shard.mutex};
        auto [i, inserted] = shard.byKey.try_emplace(std::move(key), row.name);
        if (!inserted)
            return i->second.empty() ? pending->root.string() : (pending->root / i->second).string();
        shard.queue.push_back(std::move(row));
        /* Publish the count before a drain can take this row. */
        queued = pending->queued.fetch_add(1) + 1;
    }
    if (queued >= holdBatchSize() && pending->sink) {
        /* Background work only exists when there is a batch to overlap
           with a continuing copy. A single-file store object drains on
           the registering thread and never starts another thread. */
        auto lock = std::lock_guard{pending->wake};
        if (pending->error)
            std::rethrow_exception(pending->error);
        if (!pending->writer.joinable()) {
            auto * p = pending.get();
            pending->writer = std::thread([p] {
                try {
                    while (true) {
                        {
                            auto lock = std::unique_lock{p->wake};
                            p->ready.wait(lock, [&] { return p->stopping || p->queued.load() >= holdBatchSize(); });
                            if (p->stopping)
                                return;
                        }
                        auto batch = takePending(*p);
                        if (!batch.empty())
                            p->sink(std::move(batch));
                    }
                } catch (...) {
                    auto lock = std::lock_guard{p->wake};
                    p->error = std::current_exception();
                }
            });
        }
        pending->ready.notify_one();
    }
    return std::nullopt;
}

void GitObjectStore::Hold::flush() const
{
    if (!pending->sink)
        return;
    auto batch = takeRows();
    if (!batch.empty())
        pending->sink(std::move(batch));
}

std::vector<GitObjectStore::Hold::Row> GitObjectStore::Hold::takeRows() const
{
    if (pending->writer.joinable()) {
        {
            auto lock = std::lock_guard{pending->wake};
            pending->stopping = true;
        }
        pending->ready.notify_all();
        pending->writer.join();
    }
    if (pending->error)
        std::rethrow_exception(pending->error);
    return takePending(*pending);
}

std::optional<std::string> GitObjectStore::Hold::pendingHolder(const Hash & id, bool executable) const
{
    auto * shard = pending->shards[shardOf(id)].load(std::memory_order_acquire);
    if (!shard)
        return std::nullopt;
    auto lock = std::lock_guard{shard->mutex};
    auto i = shard->byKey.find(pendingKey(id, executable));
    if (i == shard->byKey.end())
        return std::nullopt;
    return i->second.empty() ? pending->root.string() : (pending->root / i->second).string();
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
    pending = std::make_unique<Pending>();
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

GitObjectStore::Hold::Hold(const std::filesystem::path & shared, std::shared_ptr<AuxiliaryLock> auxiliaryLock)
    : dir(shared)
    , auxiliaryLock(std::move(auxiliaryLock))
{
    /* No `fd`: the lock, and the directory, belong to the hold that made
       them, and the destructor's `if (!fd) return` is what keeps this one
       from removing either.  A `Pending` of its own is the whole point. */
    pending = std::make_unique<Pending>();
}

GitObjectStore::Hold GitObjectStore::Hold::ownRows() const
{
    return Hold(dir, auxiliaryLock);
}

GitObjectStore::Hold::Hold(Hold &&) = default;

#ifndef _WIN32
/* A live hold's writers have joined before release. Its directories are
   writable and its files are auxiliary hard links: no permission changes
   or estimate of freed bytes is needed. Generic deletePath stats every
   link for those purposes. Unlink directly, opening only directories and
   never following symlinks. Dead or partially removed holds still use the
   collector's general deletion path. */
static void releaseHoldContents(Descriptor fd)
{
    AutoCloseFD scan{dup(fd)};
    if (!scan)
        throw SysError("duplicating hold directory descriptor");
    AutoCloseDir entries{fdopendir(scan.get())};
    if (!entries)
        throw SysError("reading hold directory");
    scan.release();
    struct dirent * entry;
    while (errno = 0, entry = readdir(entries.get())) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (unlinkat(fd, entry->d_name, 0) == 0 || errno == ENOENT)
            continue;
        if (errno != EISDIR && errno != EPERM)
            throw SysError("unlinking hold entry '%s'", entry->d_name);
        AutoCloseFD child{openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (!child)
            throw SysError("opening hold directory '%s'", entry->d_name);
        releaseHoldContents(child.get());
        if (unlinkat(fd, entry->d_name, AT_REMOVEDIR) == -1 && errno != ENOENT)
            throw SysError("removing hold directory '%s'", entry->d_name);
    }
    if (errno)
        throw SysError("reading hold directory");
}
#endif

GitObjectStore::Hold::~Hold()
{
    /* An ingestion that failed never flushed: the writer is stopped here, so
       nothing outlives the hold.  Its rows stay in the index against an
       ingestion that never got a path, which the sweep removes and `link`
       forgets (schema.sql). */
    if (pending && pending->writer.joinable()) {
        {
            auto lock = std::lock_guard{pending->wake};
            pending->stopping = true;
        }
        pending->ready.notify_all();
        try {
            pending->writer.join();
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
    if (!fd)
        return;
    try {
        auto lock = std::shared_lock{*auxiliaryLock};
#ifndef _WIN32
        releaseHoldContents(fd.get());
        if (::rmdir(dir.c_str()) == -1 && errno != ENOENT)
            throw SysError("removing hold %s", PathFmt(dir));
#else
        deletePath(dir);
#endif
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

std::filesystem::path GitObjectStore::Hold::linkFor(const std::filesystem::path & object) const
{
    /* Keep the hold's layout sharded too: a full source can hold tens of
       thousands of trees. Its one lock protects the entire hierarchy,
       and both normal release and the stale-hold sweep remove it recursively. */
    auto part = object.end();
    auto name = *--part;
    auto shard = *--part;
    auto kind = *--part;
    /* These are already path components. Assemble their native spelling
       once; operator/ would split and copy the whole path at each step. */
    std::filesystem::path::string_type parent;
    parent.reserve(dir.native().size() + kind.native().size() + shard.native().size() + name.native().size() + 3);
    parent += dir.native();
    parent += std::filesystem::path::preferred_separator;
    parent += kind.native();
    parent += std::filesystem::path::preferred_separator;
    parent += shard.native();
    auto shardName = shard.string();
    assert(isShardName(shardName));
    auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    size_t n = digit(shardName[0]) * 16 + digit(shardName[1]);
    if (kind == "blobs-x")
        n += 256;
    else if (kind == "trees")
        n += 512;
    else
        assert(kind == "blobs");
    std::call_once(pending->auxiliaryShards[n], [&] { createDirs(parent); });
    parent += std::filesystem::path::preferred_separator;
    parent += name.native();
    return std::filesystem::path(std::move(parent));
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
static bool holdObject(const std::filesystem::path & object, const std::filesystem::path & held)
{
    auto ec = hardLink(object, held);
    if (!ec || ec == std::errc::file_exists)
        return true;
    if (ec == std::errc::no_such_file_or_directory)
        return false;
    if (ec == std::errc::too_many_links || ec == std::errc::no_space_on_device) {
        printInfo("cannot hold object %s: %s", PathFmt(object), ec.message());
        return true;
    }
    throw Error("creating hard link from %1% to %2%: %3%", PathFmt(object), PathFmt(held), ec.message());
}

static bool holdObject(const std::filesystem::path & object, const GitObjectStore::Hold & hold)
{
    return holdObject(object, hold.linkFor(object));
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

std::optional<PosixStat> GitObjectStore::blobFileOrRemove(
    const std::string & file, const Hash & id, std::optional<uint64_t> size, RepairFlag repair)
{
#ifdef _WIN32
    return std::nullopt;
#else
    if (counters)
        counters->blobsStatted++;
    auto st = maybeLstatOf(file);
    if (!st)
        return std::nullopt;
    if ((size && static_cast<uint64_t>(st->st_size) != *size) || (repair && blobIdOfFile(file) != id)) {
        warn("removing corrupt object %s", PathFmt(std::filesystem::path(file)));
        warn(
            "There may be more corrupted paths."
            "\nYou should run `nix-store --verify --check-contents --repair` to fix them all");
        unlinkIfExists(file);
        return std::nullopt;
    }
    return st;
#endif
}

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

/* Write a file's bytes to the fresh temporary file `tmp`, canonicalised,
   link it into place and hold it; an entry that appeared meanwhile stands,
   and is held instead. A successfully published temporary stays in the
   hold as its protecting link; every unsuccessful one is removed.
   Under `fsync` the file -- its bytes and its canonical mode and
   timestamp, which are set first so that the flush covers them (fsync(2)
   flushes the file's data and metadata) -- is flushed before it is
   linked, and its directory after.

   The invariant (`08` K16): the object file is never at one link without
   a hold while its ingestion is in flight.  It is linked into place while
   the temporary still links the same inode, so it has two links from its
   first instant, and the temporary itself is the hold's link;
   an existing file is held before it is trusted, and one the sweep took
   between the look and the link is written afresh. */
static void writeObjectFile(
    const std::filesystem::path & dst,
    std::string_view bytes,
    bool executable,
    bool fsync,
    const GitObjectStore::Hold & hold,
    const std::function<void(const std::filesystem::path &)> & atFsync)
{
    auto held = hold.linkFor(dst);
    /* This name belongs to an already unique, locked hold. Its lifetime
       is the hold's, so it needs neither the global temporaries' timestamp
       nor their process ID. Own-row views of a hold share this process-wide
       sequence. Reuse the resolved hold path throughout publication. */
    static std::atomic<uint64_t> counter{0};
    auto tmp = held;
    tmp += fmt(".tmp-%d", counter.fetch_add(1, std::memory_order_relaxed));
    bool published = false;
    Finally removeTmp{[&] {
        if (published)
            return;
        try {
            unlinkIfExists(tmp);
        } catch (...) {
            ignoreExceptionExceptInterrupt();
        }
    }};
    /* Thrice at most: another writer's file appearing sends us round to
       hold it; the sweep taking it between our look and our link, to
       write it. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (holdObject(dst, held))
            return;
        {
            mode_t mode = executable ? 0755 : 0644;
#ifdef _WIN32
            AutoCloseFD fd = openNewFileForWrite(tmp, mode, {.writeOnly = true});
#else
            AutoCloseFD fd{open(tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, mode)};
#endif
            if (!fd)
                throw SysError("creating file %1%", PathFmt(tmp));
            writeFull(fd.get(), bytes);
#ifdef _WIN32
            canonicaliseTimestampAndPermissions(tmp);
#else
            canonicaliseTimestampAndPermissions(fd.get(), executable);
#endif
            if (fsync) {
                if (atFsync)
                    atFsync(tmp);
                fd.fsync();
            }
        }
        auto link = [&] {
            auto ec = hardLink(tmp, dst);
            if (ec == std::errc::no_such_file_or_directory) {
                /* The first object of this shard (`createDirectories`). */
                createDirs(dst.parent_path());
                ec = hardLink(tmp, dst);
            }
            return ec;
        };
        if (auto ec = link()) {
            unlinkIfExists(tmp);
            if (ec == std::errc::file_exists)
                continue;
            if (ec == std::errc::no_space_on_device) {
                /* The object is not kept; every reader tolerates its absence. */
                printInfo("cannot write object %s: %s", PathFmt(dst), ec.message());
                return;
            }
            throw Error("creating hard link from %1% to %2%: %3%", PathFmt(tmp), PathFmt(dst), ec.message());
        }
        /* The temporary is inside the locked hold. Keep it as the
           protecting link: publication already leaves two links, with
           no interval in which GC can take the object. A unique name
           also keeps concurrent writers from reading a partial file. */
        published = true;
        if (fsync)
            syncParent(dst);
        return;
    }
    throw Error("object %s appeared and disappeared repeatedly; not written", PathFmt(dst));
}

void GitObjectStore::putBlob(const Hash & id, bool executable, std::string_view bytes, const Hold & hold)
{
    auto lock = std::shared_lock{*auxiliaryLock};
    writeObjectFile(blobFile(id, executable), bytes, executable, fsync, hold, atFsync);
}

void GitObjectStore::putTree(const Hash & id, std::string_view body, const Hold & hold)
{
    auto lock = std::shared_lock{*auxiliaryLock};
    writeObjectFile(treeFile(id), body, false, fsync, hold, atFsync);
}

void GitObjectStore::putRootTree(const merkle::TreeEntry & root, const Hold & hold)
{
    if (auto body = merkle::syntheticRootTree(root))
        putTree(merkle::treeId(*body), *body, hold);
}

bool GitObjectStore::holdTree(const Hash & id, const Hold & hold) const
{
    auto lock = std::shared_lock{*auxiliaryLock};
    return holdObject(treeFile(id), hold);
}

bool GitObjectStore::holdBlob(const Hash & id, bool executable, const Hold & hold) const
{
    auto lock = std::shared_lock{*auxiliaryLock};
    return holdObject(blobFile(id, executable), hold);
}

std::optional<std::string> GitObjectStore::readTree(const Hash & id) const
{
    if (counters)
        counters->treesRead++;
    auto file = treeFile(id);
    if (!pathExists(file))
        return std::nullopt;
    return readFile(file);
}

merkle::Tree GitObjectStore::parseTreeBody(const Hash & id, std::string_view body)
{
    struct Into : merkle::DirectorySink
    {
        merkle::Tree tree;

        /* As `merkle::Tree` keys entries, and not through `insertEntry`,
           which refuses the name `.` that the synthetic root tree of a
           bare executable or symlink carries (`merkle::syntheticRootTree`). */
        void insertChild(std::string_view name, merkle::TreeEntry entry) override
        {
            std::string key(name);
            if (entry.mode == merkle::Mode::Directory)
                key += '/';
            tree.insert_or_assign(std::move(key), entry);
        }
    } into;

    /* The file holds the entries alone; the parser reads the size git's
       header puts before them. */
    StringSource source{std::to_string(body.size()) + '\0' + std::string(body)};
    try {
        git::parseTree(into, source, hashAlgo);
    } catch (Error & e) {
        throw Error("malformed tree object %s: %s", idString(id), e.msg());
    }
    return std::move(into.tree);
}

std::optional<merkle::Tree> GitObjectStore::readVerifiedTree(const Hash & id) const
{
    auto body = readTree(id);
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
            std::error_code isDir;
            if (entry.is_directory(isDir)) {
                /* An ingestion's hold (`Hold`).  Locked: the ingestion is in
                   flight, and its links keep its objects below.  Free: its
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
                if (!fd || !lockFile(fd.get(), ltWrite, false)) {
                    stats.holds++;
                    continue;
                }
                /* A removal that fails is a hold this sweep does not take,
                   not the end of the collection (01 section 9.10 law 5, which
                   sweeps on): a hold an older Nix is filling before its
                   lock answers `ENOTEMPTY`, one another sweep took first
                   `ENOENT`, and either way the objects below are judged at
                   their link counts, which the hold's links still carry. */
                try {
                    if (failHoldRemoval && failHoldRemoval(held))
                        throw SysError(ENOTEMPTY, "cannot unlink %s", PathFmt(held));
                    deletePath(held);
                } catch (SysError & e) {
                    printInfo("cannot remove the hold %s: %s", PathFmt(held), e.msg());
                    stats.holds++;
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
            auto body = readTree(id);
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
            /* Trees: reachable from a live root, or held by an ingestion
               in flight (a second link, `Hold`; K16), stay.  The blob
               rule below, made the tree rule. */
            if (auto treesDir = sweptSub("trees"))
                eachObjectFile(*treesDir, [&](const std::filesystem::path & file) {
                    checkInterrupt();
                    if (reachable.count(file.filename().string()))
                        return;
                    auto st = maybeLstat(file);
                    if (!st)
                        return;
                    if (st->st_nlink != 1) {
                        stats.treesHeld++;
                        return;
                    }
                    remove(file);
                    stats.treesRemoved++;
                });

            /* Blobs: a file with no link but the store's own -- no path's
               file, no ingestion's hold -- that no reachable tree names,
               is unreferenced. */
            for (auto & sub : {"blobs", "blobs-x"}) {
                auto blobsDir = sweptSub(sub);
                if (!blobsDir)
                    continue;
                eachObjectFile(*blobsDir, [&](const std::filesystem::path & file) {
                    checkInterrupt();
                    if (counters)
                        counters->blobsStatted++;
                    auto st = maybeLstat(file);
                    if (!st)
                        return;
                    if (st->st_nlink != 1) {
                        stats.blobsKept++;
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
