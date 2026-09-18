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
#include <optional>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace nix {

/**
 * The local store's object store (doc/lazy-store/01-specification.md, section 9.10) under `dir`:
 * `blobs/<id>` (0444) and `blobs-x/<id>` (0555), never linked across the bit; `trees/<id>`; `tmp/`.
 * Under `_WIN32` the object store shares nothing: `link` is false, `place` nullopt, `blobInodes()` empty.
 */
struct GitObjectStore
{
    static constexpr HashAlgorithm hashAlgo = merkle::hashAlgo;

    const std::filesystem::path dir;

    /** Whether object files (and their directory) are fsynced when written. */
    const bool fsync;

    explicit GitObjectStore(std::filesystem::path dir, bool fsync = false);

    /**
     * Make the directories: at a writable store's open, and at the start
     * of `sweep` and `blobInodes`, which iterate them (a directory removed
     * after the open must not abort a collection).  The other readers
     * (`link`, `place`, `readTree`, `blobFileOrRemove`) treat their
     * absence as emptiness.
     */
    void createDirectories() const;

    /** The identifier of the blob a regular file's bytes are. */
    static Hash blobIdOfFile(const std::filesystem::path & path);

    static std::string idString(const Hash & id);

    std::filesystem::path blobFile(const Hash & id, bool executable) const;

    std::filesystem::path treeFile(const Hash & id) const;

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
     * lacks the blob (or held an invalid file, now removed), when it was
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
        RepairFlag repair);

    /** Write the blob's file for a mode from bytes, when absent. */
    void putBlob(const Hash & id, bool executable, std::string_view bytes);

    /** Write the tree's body, when absent. */
    void putTree(const Hash & id, std::string_view body);

    /**
     * Write the synthetic root tree of an executable or symlink root
     * (`merkle::syntheticRootTree`), through which its blob is reachable;
     * nothing for a directory or a plain file.
     */
    void putRootTree(const merkle::TreeEntry & root);

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
         * migrated by the walk or by `--load-db`, or a path whose objects a
         * collection took between its restore and its registration (01
         * §9.10, "Collection").  Such a root keeps nothing beneath
         * it -- trees are written bottom-up and swept as a set, and a
         * path's regular-file blobs are held by its own links -- so the
         * removal phase runs; the count is reported with `nix-store
         * --optimise` named.  Trees a present tree names are counted apart
         * (`missingSubtrees`).
         */
        uint64_t unentered = 0;
        /**
         * Trees a present tree names that the store lacks (an interrupted
         * repair, `ENOSPC` on one tree): counted apart from the roots, and
         * reported with them.
         */
        uint64_t missingSubtrees = 0;
    };

    /**
     * Law 5 of 01 §9.10.  `forEachLiveObjectHash` yields the live roots
     * (the valid paths' object hashes) and returns the number it could
     * not give; when that is non-zero, or a tree is corrupt, only the
     * temporary files go.  A live root whose object is missing is counted
     * (`SweepStats::unentered`) and does not stop the removal phase.
     */
    SweepStats sweep(fun<uint64_t(fun<void(const ObjectHash &)>)> forEachLiveObjectHash);

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
    blobFileOrRemove(const Hash & id, bool executable, std::optional<uint64_t> size, RepairFlag repair);

    /* Reads trees and drives `enterIntoObjects` alone (src/libstore-tests/local-store.cc). */
    friend class LocalStoreObjectsTest;
};

} // namespace nix
