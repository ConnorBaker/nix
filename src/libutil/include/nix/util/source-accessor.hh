#pragma once

#include <filesystem>
#include <functional>

#include "nix/util/canon-path.hh"
#include "nix/util/fun.hh"
#include "nix/util/hash.hh"
#include "nix/util/merkle-files.hh"
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
 *
 * Operations that continue using or borrowing the accessor across callbacks
 * retain its shared owner. Accessors without shared ownership remain
 * borrowed and must outlive the operation.
 */
struct SourceAccessor : std::enable_shared_from_this<SourceAccessor>
{
private:
    /* VTable anchor to avoid weak linkage of the vtable - it breaks
     * dynamic_cast across shared libraries on Darwin. */
    virtual void anchor() = 0;
public:
    const size_t number;

    std::string displayPrefix, displaySuffix;

    SourceAccessor();

    virtual ~SourceAccessor() {}

    /** Whether sibling traversal reads and naming observations may run
     * concurrently and out of order. This permits neither retaining a
     * scoped accessor nor assuming an immutable filesystem snapshot.
     * Unknown implementations, including wrappers with arbitrary policy
     * callbacks, keep the sequential traversal. Permission covers the
     * accessor's directory scopes as well. Configuration must remain
     * stable during a traversal. */
    virtual bool allowsConcurrentTraversal()
    {
        return false;
    }

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
    virtual void
    readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback = [](uint64_t size) {}) = 0;

    /**
     * @brief Check whether a file exists at @p path.
     *
     * @todo Consider making this non-virtual, since the evaluator uses
     * maybeLstat as an indication that a file exists always (for positive
     * caching purposes).
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

    typedef std::map<std::string, DirEntry, std::less<>> DirEntries;

    /** A listing owned by this value or borrowed from a live directory scope.
     * Consumers may read either form or filter it into an owning map.
     * Filtering transfers owned nodes and copies only accepted borrowed nodes,
     * preserving the lender's admission snapshot. Borrowing is callback-scoped. */
    class DirectoryListing
    {
        using Borrowed = std::reference_wrapper<const DirEntries>;
        DirEntries owned;
        const DirEntries * borrowed = nullptr;

    public:
        explicit DirectoryListing(DirEntries && entries)
            : owned(std::move(entries))
        {
        }

        explicit DirectoryListing(Borrowed entries)
            : borrowed(&entries.get())
        {
        }

        DirectoryListing(DirectoryListing &&) = default;

        const DirEntries & entries() const
        {
            return borrowed ? *borrowed : owned;
        }

        template<typename Predicate>
        DirEntries filter(Predicate && keep) &&
        {
            if (!borrowed) {
                std::erase_if(owned, [&](const auto & entry) { return !keep(entry); });
                return std::move(owned);
            }
            DirEntries admitted;
            for (const auto & entry : *borrowed)
                if (keep(entry))
                    admitted.insert(admitted.end(), entry);
            return admitted;
        }
    };

    /**
     * @note Like `readFile`, this method should *not* follow symlinks.
     */
    virtual DirEntries readDirectory(const CanonPath & path) = 0;

    using ReadDirectoryCallback = function_ref<void(SourceAccessor &, const CanonPath &, DirectoryListing &&)>;

    /**
     * List and scope the same directory for a recursive walk, without
     * following symlinks. The callback receives the listing and the accessor
     * and relative path for reading its children. The callback must be valid and
     * outlive this call; it is borrowed and never retained. All uses, including
     * joined parallel visits, must finish before the callback returns.
     */
    virtual void readDirectoryWithEntries(const CanonPath & path, ReadDirectoryCallback callback)
    {
        auto keepAlive = weak_from_this().lock();
        callback(*this, path, DirectoryListing(readDirectory(path)));
    }

    virtual std::string readLink(const CanonPath & path) = 0;

    virtual void dumpPath(const CanonPath & path, Sink & sink, PathFilter & filter = defaultPathFilter);

    Hash
    hashPath(const CanonPath & path, PathFilter & filter = defaultPathFilter, HashAlgorithm ha = HashAlgorithm::SHA256);

    /**
     * Return a corresponding path in the root filesystem, if
     * possible. This is only possible for filesystems that are
     * materialized in the root filesystem.
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
     * An optional name for this accessor's complete rendered tree. Equal
     * names must identify equal trees; a producer assigning one must keep
     * it valid for the lifetime of that view, including after mutations.
     */
    std::optional<std::string> fingerprint;

    /**
     * A canonical SHA-256 entry established by the producer of this view.
     * This is a fact about the bytes, not a hash asserted by an input or a
     * Git object ID whose modes/rendering have not been canonicalised.
     * Wrappers may forward it only when they preserve the entire subtree.
     * Verification and repair must still read the bytes.
     */
    std::optional<merkle::TreeEntry> knownRoot;

    virtual std::optional<merkle::TreeEntry> getKnownEntry(const CanonPath & path)
    {
        return path.isRoot() ? knownRoot : std::nullopt;
    }

    /**
     * Optionally identify the rendered subtree at `path` by a relative
     * path and name. Equal (name, relative path) pairs must identify equal
     * subtrees, even across different accessors. Returning no identity is
     * always valid and carries no relative path. An empty name is present.
     *
     * A wrapper may forward an inner name only when that inner subtree is
     * the whole rendered subtree, including its domain. Otherwise it must
     * derive a name for its transformed view or decline to name it.
     *
     * For example, below an authoritative mount at `/nix/store/foo`,
     * `/nix/store/foo/bar` can use the mounted accessor's name with `/bar`.
     * A union directory with another contributor cannot use that name alone.
     */
    virtual std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path)
    {
        if (fingerprint)
            return std::pair{path, *fingerprint};
        return std::nullopt;
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
};

/**
 * Return a source accessor that contains only an empty root directory.
 */
ref<SourceAccessor> makeEmptySourceAccessor();

/**
 * Exception thrown when accessing a filtered path (see
 * `FilteringSourceAccessor`).
 */
MakeError(RestrictedPathError, Error);

class SymlinkNotAllowed final : public CloneableError<SymlinkNotAllowed, Error>
{
    void anchor() override;

public:
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
 * Return the union of the given accessors.  At each path the first child
 * that has a node decides: a directory is the merge of the directories
 * every child has there, earlier children winning name collisions; a
 * non-directory hides the later children's subtrees at and below it.
 */
ref<SourceAccessor> makeUnionSourceAccessor(std::vector<ref<SourceAccessor>> && accessors);

/**
 * Retain positive metadata/link facts and optional naming observations until
 * invalidateCache(). File reads and directory listings still reach the child;
 * retaining these observations does not freeze its bytes. An absent name does
 * not assert that a filesystem node is absent; failed existence lookups are
 * never cached. The evaluator invalidates after mounting inputs or explicitly
 * refreshing external sources.
 */
ref<SourceAccessor> makeCachingSourceAccessor(ref<SourceAccessor> next);

} // namespace nix
