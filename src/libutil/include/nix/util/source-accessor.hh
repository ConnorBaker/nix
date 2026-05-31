#pragma once

#include <filesystem>

#include "nix/util/canon-path.hh"
#include "nix/util/fun.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

namespace nix {

struct Sink;

/**
 * Note there is a decent chance this type soon goes away because the problem is solved another way.
 * See the discussion in https://github.com/NixOS/nix/pull/9985.
 */
enum class SymlinkResolution {
    /**
     * Resolve symlinks in the ancestors only.
     *
     * Only the last component of the result is possibly a symlink.
     */
    Ancestors,

    /**
     * Resolve symlinks fully, realpath(3)-style.
     *
     * No component of the result will be a symlink.
     */
    Full,
};

MakeError(SourceAccessorError, Error);
MakeError(FileNotFound, SourceAccessorError);
MakeError(NotASymlink, SourceAccessorError);
MakeError(NotADirectory, SourceAccessorError);
MakeError(NotARegularFile, SourceAccessorError);

/**
 * A read-only filesystem abstraction. This is used by the Nix
 * evaluator and elsewhere for accessing sources in various
 * filesystem-like entities (such as the real filesystem, tarballs or
 * Git repositories).
 */
struct SourceAccessor : std::enable_shared_from_this<SourceAccessor>
{
    const size_t number;

    std::string displayPrefix, displaySuffix;

    SourceAccessor();

    virtual ~SourceAccessor() {}

    /**
     * Return the contents of a file as a string.
     *
     * @note Unlike Unix, this method should *not* follow symlinks. Nix
     * by default wants to manipulate symlinks explicitly, and not
     * implicitly follow them, as they are frequently untrusted user data
     * and thus may point to arbitrary locations. Acting on the targets
     * targets of symlinks should only occasionally be done, and only
     * with care.
     */
    std::string readFile(const CanonPath & path);

    /**
     * Write the contents of a file as a sink. `sizeCallback` must be
     * called with the size of the file before any data is written to
     * the sink.
     *
     * @note Like the other `readFile`, this method should *not* follow
     * symlinks.
     *
     * @note subclasses of `SourceAccessor` need to implement at least
     * one of the `readFile()` variants.
     */
    virtual void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback = [](uint64_t size) {});

    /**
     * Test whether `path` resolves to anything in this accessor.
     *
     * **Contract.** Equivalent to `maybeLstat(path).has_value()`.
     * The default implementation in `SourceAccessor` is precisely
     * that. Overrides are permitted on **leaf** accessors as a
     * cheaper specialisation of the same predicate (e.g. POSIX
     * `stat()` vs. building a full `Stat` struct).
     *
     * **Wrapper accessors MUST NOT override.** A wrapper that
     * synthesises stats in `maybeLstat` (e.g.
     * `DirectorySynthesizerSourceAccessor`) would silently miss the
     * synthesis if `pathExists` short-circuited to `next->pathExists`.
     * The default routes through this wrapper's `maybeLstat`, which
     * picks up synthesis automatically.
     */
    virtual bool pathExists(const CanonPath & path);

    enum Type {
        tRegular,
        tSymlink,
        tDirectory,
        /**
          Any other node types that may be encountered on the file system, such as device nodes, sockets, named pipe,
          and possibly even more exotic things.

          Responsible for `"unknown"` from `builtins.readFileType "/dev/null"`.

          Unlike `DT_UNKNOWN`, this must not be used for deferring the lookup of types.
        */
        tChar,
        tBlock,
        tSocket,
        tFifo,
        tUnknown
    };

    struct Stat
    {
        Type type = tUnknown;

        /**
         * For regular files only: the size of the file. Not all
         * accessors return this since it may be too expensive to
         * compute.
         */
        std::optional<uint64_t> fileSize;

        /**
         * For regular files only: whether this is an executable.
         */
        bool isExecutable = false;

        /**
         * For regular files only: the position of the contents of this
         * file in the NAR. Only returned by NAR accessors.
         */
        std::optional<uint64_t> narOffset;

        bool isNotNARSerialisable();
        std::string typeString();
    };

    virtual Stat lstat(const CanonPath & path);

    virtual std::optional<Stat> maybeLstat(const CanonPath & path) = 0;

    typedef std::optional<Type> DirEntry;

    typedef std::map<std::string, DirEntry> DirEntries;

    /**
     * @note Like `readFile`, this method should *not* follow symlinks.
     */
    virtual DirEntries readDirectory(const CanonPath & path) = 0;

    /**
     * Variation of readDirectory that receives a SourceAccessor possibly scoped to \ref dirPath.
     * Primary meant for recursive traversal functions that would benefit from *at-style syscalls
     * relative to a particular directory.
     *
     * @note Like `readFile`, this method should *not* follow symlinks.
     * @param callback Caller-provided function invoked with a maximally deeply scoped SourceAccessor and the path that
     * would have to be prepended to each path relative to dirPath to access a particular file with it.
     */
    virtual void readDirectory(
        const CanonPath & dirPath,
        std::function<void(SourceAccessor & subdirAccessor, const CanonPath & subdirRelPath)> callback)
    {
        callback(*this, dirPath);
    }

    virtual std::string readLink(const CanonPath & path) = 0;

    virtual void dumpPath(const CanonPath & path, Sink & sink, PathFilter & filter = defaultPathFilter);

    Hash
    hashPath(const CanonPath & path, PathFilter & filter = defaultPathFilter, HashAlgorithm ha = HashAlgorithm::SHA256);

    /**
     * Return a corresponding path in the root filesystem, if
     * possible. This is only possible for filesystems that are
     * materialised in the root filesystem.
     */
    virtual std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path)
    {
        return std::nullopt;
    }

    bool operator==(const SourceAccessor & x) const
    {
        return number == x.number;
    }

    auto operator<=>(const SourceAccessor & x) const
    {
        return number <=> x.number;
    }

    void setPathDisplay(std::string displayPrefix, std::string displaySuffix = "");

    virtual std::string showPath(const CanonPath & path);

    /**
     * Resolve any symlinks in `path` according to the given
     * resolution mode.
     *
     * @param mode might only be a temporary solution for this.
     * See the discussion in https://github.com/NixOS/nix/pull/9985.
     */
    CanonPath resolveSymlinks(const CanonPath & path, SymlinkResolution mode = SymlinkResolution::Full);

    /**
     * A string that uniquely represents the contents of this
     * accessor. This is used for caching lookups (see `fetchToStore()`).
     *
     * For source accessors with intrinsic content identity (Git, NAR),
     * this is set to the input-level fingerprint at construction time.
     * For wrappers, this acts as a fallback when the inner accessor
     * lacks a content-keyed identity (e.g. workdir / `PosixSourceAccessor`).
     */
    std::optional<std::string> fingerprint;

    /**
     * Return the fingerprint for `path`. The returned `(CanonPath,
     * fingerprint)` pair names a content-anchored cache cell: the cell
     * called `returned-path` under the namespace identified by
     * `fingerprint`.
     *
     * For leaf accessors with content-keyed identity (e.g. Git tree- or
     * blob-rooted), the returned path is `CanonPath::root` and the
     * fingerprint is content-derived (`tree:<sha>`,
     * `blob:<sha>;m=<mode>`). For wrappers, the path is translated
     * through the wrapper's prefix/mount and the inner fingerprint has
     * the wrapper's own suffix (from `computeOwnSuffix`) merged in.
     *
     * Master's previous "short-circuit on `this->fingerprint`" pattern
     * is now the wrapper-fallback case: if the inner returns no
     * content-keyed fingerprint and the wrapper has its own
     * `fingerprint` field set, the wrapper returns it. This preserves
     * cache rows for workdir inputs (where `Input::getAccessor` writes
     * the input-level fingerprint onto the wrapper) while letting
     * content-keyed inner identity flow through wrappers.
     */
    virtual std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path)
    {
        return {path, fingerprint};
    }

    /**
     * If this accessor is backed by a Git ODB and rooted at a Git
     * *tree* object, return that tree's OID; otherwise `std::nullopt`.
     *
     * This is the hook for the universal git-ODB narHash bridge
     * (PROPOSAL.md Track Z): `treeOID → narHash` is a pure, total
     * projection, so the NAR walk for a given tree need happen at most
     * once across all revs/pipelines/processes. Whole git inputs key
     * their fetcher-cache row on the *commit rev* (not the tree OID), so
     * two commits with an identical root tree — and a tarball unpacking
     * to the same tree — re-walk today; surfacing the root tree OID here
     * lets `fetchToStore2` consult/populate the cross-pipeline
     * `treeHashToNarHash` projection on it.
     *
     * Returns a plain `Hash` (the OID), NOT a `GitRepo`, so it adds no
     * libutil→libfetchers layering dependency — the value is nameable in
     * libutil. Subpath-rooted git accessors already bridge via the
     * `tree:<sha>` subpath fingerprint, so an accessor whose fingerprint
     * is already `tree:`-shaped should return `nullopt` here (the
     * existing bare-`tree:` path covers it). Default: not git-backed.
     */
    virtual std::optional<Hash> getRootTreeHash()
    {
        return std::nullopt;
    }

    /**
     * Suffix that this wrapper contributes to the inner accessor's
     * fingerprint at `path`.
     *
     * Return value:
     *   - empty: nothing to add (default — the wrapper does not affect
     *     content identity).
     *   - non-empty: a suffix beginning with `;` to splice in (e.g.
     *     `";a=<hash>;e"` for export-ignore).
     *   - nullopt: this concern is not content-determined for `path`;
     *     bypass the cache.
     *
     * Wrapper accessors override this to record export-ignore, LFS,
     * etc. The base implementation returns the empty string.
     */
    virtual std::optional<std::string> computeOwnSuffix(const CanonPath & path)
    {
        return std::string{};
    }

    /**
     * Return the maximum last-modified time of the files in this
     * tree, if available.
     */
    virtual std::optional<time_t> getLastModified()
    {
        return std::nullopt;
    }

    /**
     * Drop any cached state that could go stale across external filesystem
     * mutation (e.g. cached directory fds).
     */
    virtual void invalidateCache() {}

    /**
     * Prefetch reachable content under `subpath` so subsequent reads
     * are local. Default is a no-op — most accessors have nothing to
     * prefetch.
     *
     * Concrete uses:
     *   - `GitSourceAccessor` over a partial-clone repo: enumerate
     *     blob OIDs reachable from the given subpath up to `depth`,
     *     hand them to a `GitPromisorProvider` for one coalesced
     *     fetch. Without this, naive partial-clone is one HTTPS
     *     round-trip per blob — orders of magnitude slower than full
     *     clone on cold nixpkgs eval.
     *   - Wrapper accessors forward, translating paths and composing
     *     filters with their own `isAllowed` semantics.
     *
     * `depth = 0`: just `subpath` itself; `depth = 1`: `subpath` and
     * its immediate children; etc.
     */
    virtual void prefetchSubtree(const CanonPath & subpath, unsigned depth = 1) {}
};

/**
 * Helper for wrapper accessors: forward `getFingerprint` to `inner`,
 * splicing in `wrapper.computeOwnSuffix(path)` and falling back to
 * `wrapper.fingerprint` when `inner` has no content-keyed identity.
 *
 * `outerPath` is the path the wrapper sees; `innerPath` is the path
 * `inner->getFingerprint` is called with after the wrapper's own
 * translation (mount-prefix strip / `prefix /` prepend).
 */
std::pair<CanonPath, std::optional<std::string>> composeFingerprint(
    SourceAccessor & wrapper, SourceAccessor & inner, const CanonPath & outerPath, const CanonPath & innerPath);

/**
 * Return a source accessor that contains only an empty root directory.
 */
ref<SourceAccessor> makeEmptySourceAccessor();

/**
 * Exception thrown when accessing a filtered path (see
 * `FilteringSourceAccessor`).
 */
MakeError(RestrictedPathError, Error);

struct SymlinkNotAllowed final : public CloneableError<SymlinkNotAllowed, Error>
{
    CanonPath path;

    SymlinkNotAllowed(CanonPath path)
        : CloneableError("relative path '%s' points to a symlink, which is not allowed", path.rel())
        , path(std::move(path))
    {
    }

    template<typename... Args>
    SymlinkNotAllowed(CanonPath path, const std::string & fs, Args &&... args)
        : CloneableError(fs, std::forward<Args>(args)...)
        , path(std::move(path))
    {
    }
};

/**
 * Return an accessor for the root filesystem.
 */
ref<SourceAccessor> getFSSourceAccessor();

/**
 * Construct an accessor for the filesystem rooted at `root`. Note
 * that it is not possible to escape `root` by appending `..` path
 * elements, and that absolute symlinks are resolved relative to
 * `root`.
 *
 * Symlinks in parents of `root` are resolved. Final symlink is not.
 */
ref<SourceAccessor> makeFSSourceAccessor(
    std::filesystem::path root, bool trackLastModified = false, FinalSymlink finalSymlink = FinalSymlink::DontFollow);

/**
 * Construct an accessor that presents a "union" view of a vector of
 * underlying accessors. Earlier accessors take precedence over later.
 */
ref<SourceAccessor> makeUnionSourceAccessor(std::vector<ref<SourceAccessor>> && accessors);

/**
 * Construct `Layer(accessors)` with monoid normalization per
 * `doc/tecnix-survey/PROPOSAL.md` §3 (Layer laws L6a–L6e).
 *
 * - L6a: associativity / flattening — nested `UnionSourceAccessor`s in
 *   `accessors` are spliced into the outer list.
 * - L6d: singleton short-circuit — `Layer([a]) ≡ a`.
 * - L6e: empty Layer — `Layer([]) ≡ makeEmptySourceAccessor()`.
 *
 * (Left/right identities L6b/L6c on `empty` are handled implicitly:
 * after flattening, an empty accessor in the list still appears as one
 * entry; full L6b/L6c absorption is delegated to higher-level
 * factories that know what counts as `empty` in their context.)
 *
 * Returns `ref<SourceAccessor>` because singleton/empty branches may
 * return a non-`UnionSourceAccessor` result.
 */
ref<SourceAccessor> makeLayer(std::vector<ref<SourceAccessor>> accessors);

/**
 * Make a wrapper source accessor that caches positive lookup results.
 * Useful for the evaluator which already assumes a mostly immutable view of the filesystem.
 */
ref<SourceAccessor> makeCachingSourceAccessor(ref<SourceAccessor> next);

} // namespace nix
