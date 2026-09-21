#pragma once
///@file

#include "nix/util/hash.hh"
#include "nix/util/merkle-files.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/file-system.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/object-hash.hh"
#include "nix/util/repair-flag.hh"
#include "nix/util/fun.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include <memory>
#include <mutex>

#include <sys/stat.h>

namespace nix {

/**
 * The local store's object store (doc/lazy-store/01-specification.md, section 9.10) under `dir`:
 * `blobs/<xx>/<id>` (0444) and `blobs-x/<xx>/<id>` (0555), never linked across the bit;
 * `trees/<xx>/<id>`; `tmp/`.  `<xx>` is the identifier's first byte in hexadecimal -- git's own
 * layout, 256 shards under each kind -- so that no one directory holds every object of a large
 * store and writes into different shards do not contend for one directory's lock.
 * Under `_WIN32` the object store shares nothing: `link` is false, `place` nullopt, `blobInodes()` empty.
 */
struct GitObjectStore
{
private:
    struct AuxiliaryLock;
    std::shared_ptr<AuxiliaryLock> auxiliaryLock;

public:
    static constexpr HashAlgorithm hashAlgo = merkle::hashAlgo;

    class Hold;

    /**
     * A regular-file blob's canonical file is a *store path's own file* --
     * a **holder** -- and this is the store's record of where it is: one
     * candidate row per (blob, mode, ingestion) in the store's database
     * (`schema.sql`, `Blobs`). Written and reused files both retain a witness.  `.objects/blobs` and `.objects/blobs-x`
     * then hold no regular-file blob at all: they hold the symlink-target blobs, which no path holds, and `trees/` is
     * unchanged.  The first entry of a tree writes each file once and records rows -- no `linkat` a file, which is the
     * 0.19-0.21 s per 40 000 files of `05-validation.md` section 6.
     *
     * When a row is written, when it becomes true, and when it goes.  The
     * rows are written **while the copy runs**, by the index's writer on a
     * connection of its own (`LocalStore::writeBlobRows`), because the
     * copy is where the holders are made; they name an `Ingestions` row
     * rather than a `ValidPaths` one, since the path does not exist yet,
     * and become visible only after publication reconciles overlapping
     * candidates to one physical inode and names the valid path
     * (`LocalStore::registerValidPaths`, `LocalStore::endIngestion`).  There is therefore
     * **no foreign key and no cascade**: a row whose ingestion has no
     * path, or whose path is no longer valid, is *stale*, and two things
     * judge it -- `link`, which finds the holder gone or not what the row
     * says and forgets the row, and `LocalStore::sweepStaleBlobRows`,
     * which removes every stale row under the GC lock with the sweep,
     * keeping the rows of an ingestion still in flight (its hold still
     * under `tmp/`) exactly as the sweep keeps that ingestion's objects
     * (`08` K16).
     */
    struct Holder
    {
        /** SQLite stores device and inode numbers as signed integers. The
         * physical class must precede the replaceable ingestion identity. */
        using Cursor = std::tuple<int64_t, int64_t, uint64_t>;
        static constexpr Cursor initialCursor{
            std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::min(), 0};

        ino_t ino;
        dev_t dev;
        uint64_t size;
        /** The holder's absolute on-disk path. */
        std::string file;
        /** The ingestion that owns this candidate, strictly positive. */
        uint64_t ingestion = 0;

        Cursor cursor() const
        {
            return {static_cast<int64_t>(dev), static_cast<int64_t>(ino), ingestion};
        }
    };

    /**
     * The index, supplied by the `LocalStore` that owns this object store
     * (null in a bare object store, which then holds no regular-file blob
     * and misses every lookup).
     */
    struct Index
    {
        /** The next registered candidate of (id, mode), ordered by physical
         * class and then ingestion. Its cursor strictly exceeds the input. */
        std::function<std::optional<Holder>(const Hash &, bool, Holder::Cursor)> lookup;
        /** Forget this candidate only if it is still the row observed. */
        std::function<void(const Hash &, bool, const Holder &)> forget;
        /** The first registered candidate's inode for each (id, mode),
         * with its identifier, for the optimiser's shortcut (`blobInodes`). */
        std::function<void(boost::unordered_flat_map<ino_t, Hash> &)> inodes;
    };

    const Index * index = nullptr;

    const std::filesystem::path dir;

    /** Whether object files (and their directory) are fsynced when written. */
    const bool fsync;

    /** All users of this object namespace must use the same lock file.
     * LocalStore supplies one outside its possibly remounted store directory. */
    explicit GitObjectStore(
        std::filesystem::path dir, bool fsync = false, std::filesystem::path auxiliaryLockFile = {});

    /**
     * What a collection cost, counted only while a test asks for it
     * (`05-validation.md` section 7): the tree bodies read and the blob
     * files `lstat`ed.  A specific delete must pay neither (01 §10, *The
     * object store is swept on whole-store collections only*), and a
     * per-path sweep, were one written, must pay the deleted path's trees
     * and no more.
     */
    struct Counters
    {
        /** Tree bodies read (`readTree`: the sweep, `readVerifiedTree`). */
        uint64_t treesRead = 0;
        /** Blob files `lstat`ed (the sweep's blob pass, `blobInodes`, `blobFileOrRemove`). */
        uint64_t blobsStatted = 0;
    };

    /**
     * Where to count, or null -- which it is in every build but a test's,
     * so that the counting costs one null check per object touched.
     */
    Counters * counters = nullptr;

    /**
     * Called with an object file's temporary path at the moment of its
     * fsync, before it is linked into place, so that what a crash would
     * leave -- the canonical mode and timestamp, set before the flush
     * covers them -- can be observed there (`05-validation.md` section 7).
     * Empty in every build but a test's; checked for emptiness, so it
     * costs nothing unset.
     */
    std::function<void(const std::filesystem::path &)> atFsync;

    /**
     * When set and true for the file at `where`, `place` behaves as it does
     * on a filesystem whose directory index is full: the link into the
     * directory fails with `ENOSPC`, the file is left as written and
     * nothing on it says so (`05-validation.md` section 7; law 3's fourth
     * exception, 01 §9.10).  A full index cannot be made without a
     * filesystem of its own, and on APFS there is none to fill; this is the
     * condition, injected at the one call it reaches.  Empty in every build
     * but a test's.
     */
    std::function<bool(const std::filesystem::path &)> failPlaceWithNoSpace;

    /**
     * Called inside `Hold`'s constructor at the one instant a hold is
     * visible under `tmp/` and not yet locked -- after the directory is
     * made and opened, before the `flock` -- so that a sweep's removal of
     * a hold in that window can be driven from one thread
     * (`05-validation.md` section 7).  Empty in every build but a test's.
     */
    std::function<void()> atHoldMade;

    /**
     * When set and true for a hold the sweep has locked, its removal
     * fails with `ENOTEMPTY`, as it did against a maker that filled the
     * directory before locking it: the sweep counts the hold and goes on
     * (01 section 9.10 law 5).  A maker of this Nix leaves the window
     * empty, so the condition cannot be made in one process; this is it,
     * injected at the one call it reaches, as `failPlaceWithNoSpace` is.
     * Empty in every build but a test's.
     */
    std::function<bool(const std::filesystem::path &)> failHoldRemoval;

    /**
     * Make the four directories: at a writable store's open, and at the
     * start of `sweep` and `blobInodes`, which iterate them (a directory
     * removed after the open must not abort a collection).  The 256 shards
     * under each of the three kinds are *not* made here: a shard is made
     * by the first write into it (`place`, `enterWritten`, `putBlob`,
     * `putTree`, each on the `ENOENT` its link then gets), so that opening
     * a writable store costs four `mkdir`s and not 772, and a store holds
     * only the shards it has used.  The other readers (`link`, `place`,
     * `readTree`, `blobFileOrRemove`) treat their absence as emptiness.
     */
    void createDirectories() const;

    /**
     * Every object file under a kind's directory `kindDir` (`blobs`,
     * `blobs-x` or `trees` of some object store): the files in its shard
     * directories, and any file lying directly in it -- the flat layout an
     * earlier, unreleased build of this branch wrote, enumerated so that
     * the sweep and the verifier judge it as they judge a sharded one (01
     * §9.10, *Where the objects live*).  An absent `kindDir` is empty; an
     * entry that is neither a shard directory nor a file is passed over.
     */
    static void eachObjectFile(const std::filesystem::path & kindDir, fun<void(const std::filesystem::path &)> f);

    /** The identifier of the blob a regular file's bytes are. */
    static Hash blobIdOfFile(const std::filesystem::path & path);

    static std::string idString(const Hash & id);

    std::filesystem::path blobFile(const Hash & id, bool executable) const;

    std::filesystem::path treeFile(const Hash & id) const;

    /**
     * The same names as plain strings.  Every operation on a node needs
     * one, and `std::filesystem::path`'s `operator/` splits and reallocates
     * its components: a callgrind profile of a first entry put the path
     * class at about a twentieth of all instructions, three names a file.
     */
    std::string blobFileName(const Hash & id, bool executable) const;

    std::string treeFileName(const Hash & id) const;

    /**
     * The index's holder for the blob under a mode when it is still what
     * the row says (present, the row's inode and device, the row's size,
     * and under repair the identifier's bytes); a row whose holder is
     * gone or is not what it says is *forgotten* -- the file is a valid
     * store path's and is never removed here (design.md section 3, K2).
     */
    std::optional<Holder> holderOrForget(const Hash & id, bool executable, RepairFlag repair, PosixStat * st = nullptr);

    /** Physical facts required before sharing a recorded regular-file holder. */
    static bool validHolderMetadata(const Holder & holder, bool executable, const PosixStat & st);

    /**
     * A fresh name under `tmp/` for a temporary of the given kind
     * (`link`, `object`), carrying the second it was made, which is the
     * age the sweep goes by: a temporary link's inode is a blob's.
     */
    std::filesystem::path tempPath(std::string_view kind) const;

    /**
     * Link the store's valid file for the blob under its mode as `name`
     * in the writable directory `dirFd`, where nothing stands yet: true
     * when done, and the file need not be written; false when the store
     * lacks the blob (or its invalid candidate was forgotten), when it was
     * swept meanwhile, or at the filesystem's link limit -- the caller
     * writes the file instead.  `size` is the blob's, for the validity
     * check; without one (a materialisation from the store's objects,
     * which has no other measure of the file) the file is trusted as the
     * on-disk walk trusts an inode that is a blob's, its bytes re-hashed
     * under repair alone.  `linked`, when given, receives the status of
     * the store's file, whose size the caller may need.
     */
    bool link(
        const Hash & id,
        bool executable,
        Descriptor dirFd,
        const CanonPath & name,
        const std::filesystem::path & where,
        std::optional<uint64_t> size,
        RepairFlag repair,
        const Hold & hold,
        PosixStat * linked = nullptr);

    struct Placed
    {
        /** The file's inode afterwards. */
        ino_t ino;
        /** The file became the store's blob file (a miss); false when the store's file stood already or replaced it. */
        bool entered;
    };

    /**
     * Put the canonicalised regular file `name` in the writable directory `dirFd` (at `where`, status `before`)
     * under the store's care for its blob and mode: replaced by the store's valid file, or entered as it.
     * @return The file's inode afterwards and whether it was entered; nullopt when it was left as it is (a link or
     * directory limit, said here).
     */
    std::optional<Placed> place(
        const Hash & id,
        bool executable,
        Descriptor dirFd,
        const CanonPath & name,
        const std::filesystem::path & where,
        const PosixStat & before,
        RepairFlag repair,
        const Hold & hold);

    /**
     * The regular file `name` in the writable directory `dirFd` (at
     * `where`), just written because the store had no file for its blob,
     * entered as that file: read its status and claim a pending holder.
     * Another worker's completed file wins an overlapping miss; a streamed
     * file also probes registered holders through `place`. Should the
     * link be refused for `EMLINK` or a full directory index the file is
     * left as written, as `place` leaves it (01 §9.10 law 3's fourth
     * exception).
     */
    void enterWritten(
        const Hash & id,
        bool executable,
        Descriptor dirFd,
        const CanonPath & name,
        const std::filesystem::path & where,
        RepairFlag repair,
        const Hold & hold,
        bool looked = true);

    /**
     * An ingestion's hold on the objects it writes or reuses (01 §9.10,
     * "Collection"; `08` K16): a locked directory under `tmp/` with one
     * hard link per object, from the object's write until the hold is
     * released -- after the path's registration, or on the ingestion's
     * failure, when it goes out of scope.  The sweep removes a tree or a
     * blob at one link only, so what a hold links is kept though no live
     * root reaches it yet; a hold whose lock is free is either empty --
     * its maker between the `mkdir` and the `flock`, which is why the
     * directory is made empty and filled under its lock -- or a dead
     * process's, and the sweep removes it with its links before it judges
     * the objects, counting one it cannot remove and going on.  The same meaning a locked `tmp-*` directory in the
     * store has for files not yet at their name (`createTempDirInStore`,
     * `gc.cc`), and a temp root for a path -- a temp root protects no
     * object; this does.  Nothing waits on a hold: the sweep only tries
     * its lock.  Move-only; the directory is removed by the destructor.
     */
    class Hold
    {
        friend struct GitObjectStore;
        /* Drops the lock as a crash would (src/libstore-tests/local-store.cc). */
        friend class LocalStoreObjectsTest;
        std::filesystem::path dir;
        AutoCloseFD fd;
        std::shared_ptr<AuxiliaryLock> auxiliaryLock;
    public:
        /* Behind a pointer so that `Hold` stays movable with a mutex in it. */
        struct Pending;

    private:
        std::unique_ptr<Pending> pending;
        Hold(
            const std::filesystem::path & tmp,
            const std::function<void()> & atHoldMade,
            std::shared_ptr<AuxiliaryLock> auxiliaryLock);
        /* The `ownRows` form: the same directory, no lock of its own. */
        explicit Hold(const std::filesystem::path & shared, std::shared_ptr<AuxiliaryLock> auxiliaryLock);

    public:
        /**
         * A hold over the same directory with **rows of its own**.
         *
         * A batch holds its objects once -- one locked directory for the
         * whole of `addMultipleToStore` (K16) -- but each of its paths
         * registers separately and is therefore its own ingestion: its own
         * `Ingestions` row, its own root (a row's name is relative to the
         * path that holds it, `schema.sql`), and its own writer.  Sharing
         * one hold's row machinery across a batch is doubly wrong: the
         * batch's paths are copied on a thread pool
         * (`Store::addMultipleToStore`'s `processGraph`), so
         * `LocalStore::beginIngestion` ran from several threads at once and
         * assigned over a joinable writer, which terminates the process;
         * and a row written against another path's ingestion names a file
         * that does not exist, or -- the sizes agreeing -- one with the
         * wrong bytes.
         *
         * The directory belongs to the hold that locked it and is not
         * removed by this one, so the objects stay held until the batch is
         * done, which is what K16 asks.
         */
        Hold ownRows() const;

        /**
         * A pending index row: a regular file this ingestion wrote or linked, which
         * becomes the blob's holder when the path is registered
         * (`LocalStore::recordBlobs`).  Kept here because the hold is
         * already the object whose life is "from the first write to the
         * registration", and every route already has one.
         */
        struct Row
        {
            Hash id;
            bool executable;
            ino_t ino;
            dev_t dev;
            uint64_t size;
            /**
             * The holder's name under the ingestion's root -- the store
             * path's own name for it, empty for a plain-file root.  It is
             * made *here*, on the worker that wrote the file, and not at
             * the registration: forty thousand relative names built on one
             * thread after the copy is forty thousand strings on the
             * critical path (`05` section 6).
             */
            std::string name;
        };

        /**
         * The directory this ingestion writes under, set before the walk:
         * a locked temporary in the store for a route that moves its path
         * into place, the real path for one that writes there.  `addRow`
         * makes each row's name relative to it.
         */
        void setRoot(std::filesystem::path root) const;

        /** The root, for turning a row's name back into a path to link from. */
        const std::filesystem::path & root() const;

        /**
         * Where a filled batch of rows goes: the store's index writer,
         * set before the walk.  The copy's workers call it as their
         * batches fill, so the index write overlaps the copy.  Unset, the
         * rows stay in the hold and nothing writes an index.
         */
        void setSink(std::function<void(std::vector<Row> &&)> sink) const;

        /** Hand every row not yet given to the sink to it. */
        void flush() const;

        /** Stop the writer and take the rows it has not published. The
         * caller may include them in the path's registration transaction. */
        std::vector<Row> takeRows() const;

        /**
         * The ingestion this hold's rows are written against
         * (`Ingestions.id`), set by `LocalStore::beginIngestion`; unset
         * when nothing writes an index.
         */
        struct Ingestion
        {
            std::optional<uint64_t> id;
            std::optional<std::string> path;
        };

        /* Shared with the row sink so moving a hold cannot invalidate it.
           LocalStore serialises access with its blob-writer mutex. */
        mutable std::shared_ptr<Ingestion> ingestion;

        /** The hold's own directory under `tmp/`: its name is what says this ingestion is in flight. */
        const std::filesystem::path & directory() const;

        /** Claim a completed file as this ingestion's holder. Return the
         * existing holder if another worker published first. The claim
         * remains stable until the ingestion finishes. */
        std::optional<std::string> addRow(Row row) const;

        /**
         * The completed file this ingestion owns for (id, mode), if any:
         * the in-flight half of the index, without which two identical
         * files of one tree would both be written (law 3 within a path).
         */
        std::optional<std::string> pendingHolder(const Hash & id, bool executable) const;

        Hold(Hold &&);
        Hold & operator=(Hold &&) = delete;
        ~Hold();
        /**
         * Where the link to the object file `object` (under
         * `<kind>/<shard>/`, `<kind>` one of `blobs`, `blobs-x`, `trees`)
         * goes: `<hold>/<kind>/<shard>/<id>`. The hold takes one link per
         * tree and symlink blob, and none for a regular file, whose own
         * store path's link holds its blob. Shards are made on demand.
         */
        std::filesystem::path linkFor(const std::filesystem::path & object) const;
    };

    /**
     * A fresh hold: the directory made empty and locked, retried under a
     * new name if the sweep removed it before the lock was taken. Auxiliary
     * kind/shard directories are made only when an object needs them.
     */
    Hold hold() const;

    /** Write the blob's file for a mode from bytes, when absent, and hold it. */
    void putBlob(const Hash & id, bool executable, std::string_view bytes, const Hold & hold);

    /** Write the tree's body, when absent, and hold it. */
    void putTree(const Hash & id, std::string_view body, const Hold & hold);

    /**
     * Write the synthetic root tree of an executable or symlink root
     * (`merkle::syntheticRootTree`), through which its blob is reachable,
     * and hold it; nothing for a directory or a plain file.
     */
    void putRootTree(const merkle::TreeEntry & root, const Hold & hold);

    /**
     * Hold an object the store already has, for a path made from it (the
     * second route): false when the store lacks it -- swept meanwhile, or
     * never there -- and the caller falls back.
     */
    bool holdTree(const Hash & id, const Hold & hold) const;
    bool holdBlob(const Hash & id, bool executable, const Hold & hold) const;

    /**
     * The entries of the tree `id` for materialising a path from the
     * store's objects (01 §9.10, the second route): the body read and
     * checked against its name, as the sweep checks it (law 4: "a tree
     * body is verified against its identifier when it is read to
     * materialise a path").  Nullopt when the store lacks the tree, and
     * -- refused, with a warning naming the repair -- when the body does
     * not hash to its name or does not parse; the body is left for
     * `--verify --check-contents --repair`, since removing it would let
     * the sweep take what it names (K6).
     */
    std::optional<merkle::Tree> readVerifiedTree(const Hash & id) const;

    /**
     * The inode of every blob file (by `lstat`, as `knownRegularFile`
     * looks it up), with its identifier: a file already a blob's need not
     * be hashed again.
     */
    using BlobInodes = boost::unordered_flat_map<ino_t, Hash>;

    BlobInodes blobInodes() const;

    struct SweepStats
    {
        uint64_t treesRemoved = 0, blobsRemoved = 0, blobsKept = 0;
        /** The bytes the kept blobs' links beyond the first would otherwise occupy. */
        uint64_t sharedBytes = 0;
        /**
         * Valid paths whose object hash the enumeration could not give
         * (rows older than schema 11); when non-zero the sweep removed no
         * tree and no blob.
         */
        uint64_t unmigrated = 0;
        /**
         * Tree files whose body does not hash to their name or does not
         * parse (a crash left them short); when non-zero the sweep
         * removed no tree and no blob, and said to run
         * `nix-store --verify --check-contents --repair`.
         */
        uint64_t corruptTrees = 0;
        /**
         * Live roots -- valid paths -- whose object the store lacks: a row
         * migrated by the walk or by `--load-db`, or a path `nix-store
         * --optimise` is entering while this sweep runs (its root live
         * before its trees, which its hold keeps).  Such a root keeps
         * nothing beneath it -- trees are written bottom-up and swept as a
         * set, and a path's regular-file blobs are held by its own links --
         * so the removal phase runs; the count is reported with
         * `nix-store --optimise` named.  Trees a present tree names are
         * counted apart (`missingSubtrees`).
         */
        uint64_t unentered = 0;
        /**
         * Trees a present tree names that the store lacks (an interrupted
         * repair, `ENOSPC` on one tree): counted apart from the roots, and
         * reported with them.
         */
        uint64_t missingSubtrees = 0;
        /**
         * Trees no live root reaches that an ingestion in flight holds
         * (`Hold`: a second link), kept; and the holds whose lock was
         * taken, so left.  Both are the sweep's view of ingestions it ran
         * beside.
         */
        uint64_t treesHeld = 0, holds = 0;
    };

    /**
     * The part of an object store a sweep may remove, when that is not
     * the whole of it: the local overlay store reads through the merged
     * mount -- which is this object store -- and owns its upper layer
     * alone (01 §10, *the local overlay store's object store is not its
     * own*).  `dir` is a directory of the same shape whose files are the
     * ones enumerated and removed; the trees are still read, and a
     * root's blob still looked for, through the object store itself, so
     * that what a lower layer holds keeps what it reaches.  `remove`
     * takes one file under `dir` away -- the caller's, since on the
     * overlay an unlink through the mount of a file the lower layer also
     * has would white it out.
     */
    struct Owned
    {
        std::filesystem::path dir;
        fun<void(const std::filesystem::path & file)> remove;
    };

    /**
     * Law 5 of 01 §9.10.  `forEachLiveObjectHash` yields the live roots
     * (the valid paths' object hashes) and returns the number it could
     * not give; when that is non-zero, or a tree is corrupt, only the
     * temporary files go.  A live root whose object is missing is counted
     * (`SweepStats::unentered`) and does not stop the removal phase.  A
     * tree or blob with a second link is held -- by a path's file or by an
     * ingestion's `Hold` -- and kept; a hold whose lock is free is a dead
     * ingestion's and goes first, with its links.
     * Publication, acquisition and release of auxiliary links cannot
     * overlap this sweep. In particular, a hold whose path registers
     * after the root snapshot keeps its links until the sweep finishes.
     *
     * `owned`, when given, is the part of the object store this sweep may
     * remove from; reachability is computed over the whole of it either
     * way, and a hold is locked and removed through the object store's own
     * directory, which is what the ingestion holding it locked.
     */
    SweepStats sweep(fun<uint64_t(fun<void(const ObjectHash &)>)> forEachLiveObjectHash, const Owned * owned = nullptr);

private:
    std::optional<std::string> readTree(const Hash & id) const;

    /**
     * The entries of the tree `id` from its raw body (`git::parseTree`
     * over the body behind the size git's header gives); directory names
     * carry a trailing slash, as `merkle::Tree` keys them.
     *
     * @throws Error naming `id` if the body is not a tree object.
     */
    static merkle::Tree parseTreeBody(const Hash & id, std::string_view body);

    /**
     * The store's file for the blob under a mode when valid (of the given
     * size, and under repair of the identifier's bytes); an invalid one is
     * removed and nullopt returned.
     */
    std::optional<PosixStat>
    blobFileOrRemove(const std::string & file, const Hash & id, std::optional<uint64_t> size, RepairFlag repair);

    const std::string blobsDir, blobsXDir, treesDir;

    /* Reads trees and drives `enterIntoObjects` alone (src/libstore-tests/local-store.cc). */
    friend class LocalStoreObjectsTest;
};

} // namespace nix
