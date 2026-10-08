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
#include <utility>
#include <vector>
#include <memory>

#include <sys/stat.h>

namespace nix {

/**
 * Regular bytes are represented by indexed files in store-path holders.
 * Directory bodies and symlink targets are owned bytes in an ingestion's
 * projection, published with that projection in the local-store database.
 * `dir/tmp` contains leases and temporary regular-file links only.
 */
struct GitObjectStore
{
public:
    static constexpr HashAlgorithm hashAlgo = merkle::hashAlgo;

    class Hold;

    /**
     * A regular-file blob's canonical file is a *store path's own file* --
     * a **holder** -- and this is the store's record of where it is: one
     * candidate row per (blob, mode, ingestion) in the store's database
     * (`schema.sql`, `Blobs`). Written and reused files both retain a witness.
     * Regular-file ingestion publishes no auxiliary blob copy. Claiming a
     * first holder needs a row; repeated occurrences reuse the holder.
     *
     * When a row is written, when it becomes true, and when it goes.  The
     * rows are staged in the hold while copying and inserted when the path
     * completes. They name an `Ingestions` row rather than a `ValidPaths`
     * one, since a completed builder output is not necessarily valid yet,
     * and become visible only after publication reconciles overlapping
     * candidates to one physical inode and names the valid path
     * (`LocalStore::registerValidPaths`, `LocalStore::endIngestion`).  There is therefore
     * **no foreign key to validity**: a row whose ingestion has no
     * path, or whose path is no longer valid, is *stale*, and two things
     * judge it -- `link`, which finds the holder gone or not what the row
     * says and forgets the row, and `LocalStore::sweepStaleBlobRows`,
     * which removes stale ingestions and cascades to their rows under the GC lock,
     * keeping the projection of an ingestion still in flight (its lease
     * still under `tmp/`).
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

    /** One owned auxiliary value, addressed by a full SHA-256 Directory or Symlink hash. */
    struct Auxiliary
    {
        merkle::TreeEntry entry;
        std::string body;
    };

    /**
     * The index, supplied by the `LocalStore` that owns this object store
     * (null in a bare object store, which stages pending projections but
     * has no committed values and misses every lookup).
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
        /** A committed owned body whose framed hash matches the typed address.
         * The provider releases its statement/transaction before returning;
         * missing or corrupt candidates do not escape as usable bytes. */
        std::function<std::optional<std::string>(const merkle::TreeEntry &)> readAuxiliary;
    };

    const Index * index = nullptr;

    const std::filesystem::path dir;

    explicit GitObjectStore(std::filesystem::path dir);

    /**
     * Object-store work, counted only while a test asks for it
     * (`05-validation.md` section 7): tree-body reads and blob/holder
     * metadata probes. A specific delete must pay neither (01 §10, *The
     * object store is swept on whole-store collections only*), and a
     * per-path sweep, were one written, must pay the deleted path's trees
     * and no more.
     */
    struct Counters
    {
        /** Committed directory bodies requested. */
        uint64_t treesRead = 0;
        /** Metadata probes of holder candidates and acquired regular-file links. */
        uint64_t blobsStatted = 0;
    };

    /**
     * Where to count, or null -- which it is in every build but a test's,
     * so that the counting costs one null check per object touched.
     */
    Counters * counters = nullptr;

    /**
     * When set and true for the file at `where`, `place` declines a new
     * holder claim with `ENOSPC`, leaving the file as written and without a
     * row (`05-validation.md` section 7; law 3's fourth exception, 01 §9.10).
     * This preserves the old full-directory failure seam; an ordinary
     * holder miss now records a row and creates no auxiliary directory entry.
     * Empty in every build but a test's.
     */
    std::function<bool(const std::filesystem::path &)> failPlaceWithNoSpace;

    /**
     * Called while constructing the physical lease at the one instant it is
     * visible under `tmp/` and not yet locked -- after the directory is
     * made and opened, before the `flock` -- so that a sweep's removal of
     * a hold in that window can be driven from one thread
     * (`05-validation.md` section 7). Captured when the hold is made and
     * called when its directory is first needed. Clearing this member does
     * not disarm existing holds; captured references must outlive first use.
     * Empty outside tests.
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

    /** Ensure the temporary regular-link and ingestion-lease namespace exists. */
    void createDirectories() const;

    /** Hash file bytes using their previously observed size.
     * The caller must ensure that size is still correct or compare the result
     * with the expected canonical blob identifier. This does not establish
     * a snapshot of a concurrently mutable file. */
    static Hash blobIdOfFile(const std::filesystem::path & path, uint64_t observedSize);

    static std::string idString(const Hash & id);

    /**
     * The index's holder for the blob under a mode when it is still what
     * the row says (present, the row's inode and device, the row's size,
     * and under repair the identifier's bytes); a row whose holder is
     * gone or is not what it says is *forgotten* -- the file is a valid
     * store path's and is never removed here (design.md section 3, K2).
     */
    std::optional<Holder> holderOrForget(const Hash & id, bool executable, RepairFlag repair);

    /** Physical facts required before sharing a recorded regular-file holder. */
    static bool validHolderMetadata(const Holder & holder, bool executable, const PosixStat & st);

    /**
     * A fresh name under `tmp/` for a temporary of the given kind
     * (`link`, `coalesce`), carrying the second it was made, which is the
     * age the sweep goes by: a temporary link's inode is a blob's.
     */
    std::filesystem::path tempPath(std::string_view kind) const;

    /**
     * Link the store's valid file for the blob under its mode as `name`
     * in the writable directory `dirFd`, where nothing stands yet: its size
     * when done, and the file need not be written; nullopt when the store
     * lacks the blob (or its invalid candidate was forgotten), when it was
     * swept meanwhile, or at the filesystem's link limit -- the caller
     * writes the file instead.  `size` is the blob's, for the validity
     * check; without one (a materialisation from the store's objects,
     * which has no other measure of the file) the file is trusted as the
     * on-disk walk trusts an inode that is a blob's, its bytes re-hashed
     * under repair alone. An engaged zero is a successful empty-file link.
     */
    std::optional<uint64_t> link(
        const Hash & id,
        bool executable,
        Descriptor dirFd,
        const CanonPath & name,
        const std::filesystem::path & where,
        std::optional<uint64_t> size,
        RepairFlag repair,
        const Hold & hold);

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
     * A private pending projection and a shared, on-demand ingestion lease. The lease
     * directory's lock tells GC that completed-but-not-yet-valid SQL rows still
     * belong to an in-flight ingestion. Its lifetime also covers regular-file
     * coalescing pins. Auxiliary bytes are owned in memory, not by hard links.
     * GC only tries the lease lock; concurrent first acquisition synchronizes
     * through the shared memo. Own-row views share that acquisition, never
     * pending values or ingestion identity.
     */
    class Hold
    {
        friend struct GitObjectStore;
        /* Drops the lock as a crash would (src/libstore-tests/local-store.cc). */
        friend class LocalStoreObjectsTest;

        /* The physical lease is shared; pending values belong to each view. */
        struct Lease
        {
            std::filesystem::path dir;
            AutoCloseFD fd;

            Lease(const std::filesystem::path & tmp, const std::function<void()> & atHoldMade);
            ~Lease();
        };
    public:
        /* Behind a pointer so that `Hold` stays movable with a mutex in it. */
        struct Pending;

    private:
        std::unique_ptr<Pending> pending;
        fun<std::shared_ptr<Lease>()> lease;
        explicit Hold(fun<std::shared_ptr<Lease>()> lease);

    public:
        /**
         * A private projection sharing this hold's on-demand physical lease.
         * Speculative copy attempts can discard their pending values without
         * exposing witnesses whose files fallback will remove. Once created,
         * the directory and lock survive until the last view is released; witnesses,
         * auxiliary bytes and ingestion identity remain private to each view.
         */
        Hold ownRows() const;

        /**
         * A pending index row: a regular file this ingestion wrote or linked, which
         * becomes eligible through `LocalStore::registerValidPaths` or
         * `LocalStore::endIngestion`. Kept here because the hold is
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
             * path's own name for it, empty for a regular-file root. It is
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
         * into place, the real path for one that writes there. Names stay relative
         * to that root; native root/file boundary separator runs may differ.
         * Clears the preceding pending projection; no walking worker may still use it.
         */
        void setRoot(std::filesystem::path root) const;

        /** The root, for turning a row's name back into a path to link from. */
        const std::filesystem::path & root() const;

        /** The complete projection transferred to one publication transaction. */
        struct Projection
        {
            std::vector<Row> regular;
            std::vector<Auxiliary> auxiliary;
        };

        /** All walking workers have joined. Reserve both result vectors before
         * consuming the pending relation; allocation failure leaves it intact.
         * Each vector contains one owned value per typed key; order is unspecified. */
        Projection takeProjection() const;

        /** Stage an owned directory body or symlink target. Concurrent duplicate
         * typed addresses retain the first completed value. Noncanonical modes,
         * algorithms and hash widths are rejected before touching the map. */
        void addAuxiliary(Auxiliary object) const;

        /** Active per-path state. The ID can name a previously staged projection. */
        struct Ingestion
        {
            std::optional<uint64_t> id;
        };

        /** Completed outputs retain their database identity across the next beginIngestion. */
        struct CompletedIngestion
        {
            std::string path;
            uint64_t id;
        };

        /* Accessed only before or after the path's walking workers run. */
        mutable std::optional<Ingestion> ingestion;
        mutable std::vector<CompletedIngestion> completedIngestions;

        /** The hold's own directory under `tmp/`: its name is what says this ingestion is in flight. */
        const std::filesystem::path & directory() const;

        /** Consume a completed row only when its key is new; otherwise leave
         * it intact and return the first holder. Claims remain stable until
         * the ingestion finishes. */
        std::optional<std::string> addRow(Row && row) const;

        /**
         * The completed file this ingestion owns for (id, mode), if any:
         * the in-flight half of the index, without which two identical
         * files of one tree would both be written (law 3 within a path).
         */
        std::optional<std::string> pendingHolder(const Hash & id, bool executable) const;

        Hold(Hold &&);
        Hold & operator=(Hold &&) = delete;
        ~Hold();
    };

    /** A fresh projection; its shared lease is created only when its directory is needed. */
    Hold hold() const;

    /** Stage the synthetic directory body of an executable or symlink root. */
    void putRootTree(const merkle::TreeEntry & root, const Hold & hold);

    /** Read an owned, framed-hash-verified committed Directory/Symlink body.
     * The body remains owned by the caller across source callbacks and GC. */
    std::optional<std::string> readAuxiliary(const merkle::TreeEntry & entry) const;

    /** Parse and check canonical tree encoding. The caller has already checked
     * the framed hash. Throws Error naming the address for a malformed body. */
    static merkle::Tree parseTreeBody(const Hash & id, std::string_view body);

    /**
     * Inode-to-identifier hints from registered holder rows. Before skipping
     * a file's bytes, the on-disk visitor checks a current holder against the
     * file's full physical metadata; an inode hint alone is insufficient.
     */
    using BlobInodes = boost::unordered_flat_map<ino_t, Hash>;

    BlobInodes blobInodes() const;

    struct SweepStats
    {
        uint64_t treesRemoved = 0, blobsRemoved = 0, holds = 0;
    };

    /** The physical namespace this store owns, e.g. an overlay's upper layer.
     * `removed` is notified after an actual removal so an overlay can
     * request a remount. Deletion stays inside the opened owned namespace. */
    struct Owned
    {
        std::filesystem::path dir;
        fun<void()> removed;
    };

    /** Reap dead leases and obsolete auxiliary cache files. Only fixed kinds
     * and two-digit shard directories are descended, never symbolic links.
     * The live representation and its GC are the indexed projections, so
     * this cleanup reads no body and computes no graph reachability. */
    SweepStats sweep(const Owned * owned = nullptr);

private:
    friend class LocalStoreObjectsTest;
};

} // namespace nix
