#include "nix/store/local-store.hh"
#include "nix/store/local-settings.hh"
#include "nix/store/pathlocks.hh"
#include "nix/util/signals.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/posix-source-accessor.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/file-system.hh"
#include "nix/util/file-content-address.hh"
#include "nix/util/hash.hh"
#include "nix/util/serialise.hh"

#include <cstdlib>
#include <cstring>
#ifdef __APPLE__
#  include <regex>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "store-config-private.hh"

namespace nix {

static void makeWritable(const std::filesystem::path & path)
{
    auto st = lstat(path);
    chmod(path, st.st_mode | S_IWUSR);
}

struct MakeReadOnly
{
    std::filesystem::path path;

    MakeReadOnly(std::filesystem::path path)
        : path(std::move(path))
    {
    }

    ~MakeReadOnly()
    {
        try {
            /* This will make the path read-only. */
            if (!path.empty())
                canonicaliseTimestampAndPermissions(path.string());
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
};

LocalStore::InodeHash LocalStore::loadInodeHash()
{
    debug("loading hash inodes in memory");
    InodeHash inodeHash;

    AutoCloseDir dir(opendir(linksDir.string().c_str()));
    if (!dir)
        throw SysError("opening directory %1%", PathFmt(linksDir));

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir.get())) { /* sic */
        checkInterrupt();
        // We don't care if we hit non-hash files, anything goes
        inodeHash.insert(dirent->d_ino);
    }
    if (errno)
        throw SysError("reading directory %1%", PathFmt(linksDir));

    printMsg(lvlTalkative, "loaded %1% hash inodes", inodeHash.size());

    return inodeHash;
}

Strings LocalStore::readDirectoryIgnoringInodes(const std::filesystem::path & path, const InodeHash & inodeHash)
{
    Strings names;

    AutoCloseDir dir(opendir(path.string().c_str()));
    if (!dir)
        throw SysError("opening directory %s", PathFmt(path));

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir.get())) { /* sic */
        checkInterrupt();

        if (inodeHash.count(dirent->d_ino)) {
            debug("'%1%' is already linked", dirent->d_name);
            continue;
        }

        std::string name = dirent->d_name;
        if (name == "." || name == "..")
            continue;
        names.push_back(name);
    }
    if (errno)
        throw SysError("reading directory %s", PathFmt(path));

    return names;
}

void LocalStore::optimisePath_(
    Activity * act, OptimiseStats & stats, const std::filesystem::path & path, InodeHash & inodeHash, RepairFlag repair)
{
    checkInterrupt();

    auto st = lstat(path);

#ifdef __APPLE__
    /* HFS/macOS has some undocumented security feature disabling hardlinking for
       special files within .app dirs. Known affected paths include
       *.app/Contents/{PkgInfo,Resources/\*.lproj,_CodeSignature} and .DS_Store.
       See https://github.com/NixOS/nix/issues/1443 and
       https://github.com/NixOS/nix/pull/2230 for more discussion. */

    if (std::regex_search(path.string(), std::regex("\\.app/Contents/.+$"))) {
        debug("%s is not allowed to be linked in macOS", PathFmt(path));
        return;
    }
#endif

    if (S_ISDIR(st.st_mode)) {
        Strings names = readDirectoryIgnoringInodes(path, inodeHash);
        for (auto & i : names)
            optimisePath_(act, stats, path / i, inodeHash, repair);
        return;
    }

    /* We can hard link regular files and maybe symlinks. */
    if (!S_ISREG(st.st_mode)
#if CAN_LINK_SYMLINK
        && !S_ISLNK(st.st_mode)
#endif
    )
        return;

    /* Sometimes SNAFUs can cause files in the Nix store to be
       modified, in particular when running programs as root under
       NixOS (example: $fontconfig/var/cache being modified).  Skip
       those files.  FIXME: check the modification time. */
    if (S_ISREG(st.st_mode) && (st.st_mode & S_IWUSR)) {
        warn("skipping suspicious writable file '%s'", PathFmt(path));
        return;
    }

    /* This can still happen on top-level files. */
    if (st.st_nlink > 1 && inodeHash.count(st.st_ino)) {
        debug("%s is already linked, with %d other file(s)", PathFmt(path), st.st_nlink - 2);
        return;
    }

    /* Hash the file.  Note that hashPath() returns the hash over the
       NAR serialisation, which includes the execute bit on the file.
       Thus, executable and non-executable files with the same
       contents *won't* be linked (which is good because otherwise the
       permissions would be screwed up).

       Also note that if `path' is a symlink, then we're hashing the
       contents of the symlink (i.e. the result of readlink()), not
       the contents of the target (which may not even exist). */
    Hash hash = hashPath(makeFSSourceAccessor(path), FileSerialisationMethod::NixArchive, HashAlgorithm::SHA256).hash;
    debug("%s has hash '%s'", PathFmt(path), hash.to_string(HashFormat::Nix32, true));

    /* Check if this is a known hash. */
    std::filesystem::path linkPath = std::filesystem::path{linksDir} / hash.to_string(HashFormat::Nix32, false);

    /* Maybe delete the link, if it has been corrupted. */
    if (pathExists(linkPath)) {
        auto stLink = lstat(linkPath);
        if (st.st_size != stLink.st_size || (repair && hash != ({
                                                           hashPath(
                                                               makeFSSourceAccessor(linkPath),
                                                               FileSerialisationMethod::NixArchive,
                                                               HashAlgorithm::SHA256)
                                                               .hash;
                                                       }))) {
            // XXX: Consider overwriting linkPath with our valid version.
            warn("removing corrupted link %s", PathFmt(linkPath));
            warn(
                "There may be more corrupted paths."
                "\nYou should run `nix-store --verify --check-contents --repair` to fix them all");
            unlinkIfExists(linkPath);
        }
    }

    if (!pathExists(linkPath)) {
        /* Nope, create a hard link in the links directory. */
        try {
            std::filesystem::create_hard_link(path, linkPath);
            inodeHash.insert(st.st_ino);
        } catch (std::filesystem::filesystem_error & e) {
            if (e.code() == std::errc::file_exists) {
                /* Fall through if another process created ‘linkPath’ before
                   we did. */
            }

            else if (e.code() == std::errc::no_space_on_device) {
                /* On ext4, that probably means the directory index is
                   full.  When that happens, it's fine to ignore it: we
                   just effectively disable deduplication of this
                   file.
                   */
                printInfo("cannot link %s to '%s': %s", PathFmt(linkPath), PathFmt(path), e.code().message());
                return;
            }

            else
                throw SystemError(e.code(), "creating hard link from %1% to %2%", PathFmt(linkPath), PathFmt(path));
        }
    }

    /* Yes!  We've seen a file with the same contents.  Replace the
       current file with a hard link to that file. */
    auto stLink = lstat(linkPath);

    if (st.st_ino == stLink.st_ino) {
        debug("%1% is already linked to %2%", PathFmt(path), PathFmt(linkPath));
        return;
    }

    printMsg(lvlTalkative, "linking %1% to %2%", PathFmt(path), PathFmt(linkPath));

    /* Make the containing directory writable, but only if it's not
       the store itself (we don't want or need to mess with its
       permissions). */
    const auto dirOfPath = path.parent_path();
    bool mustToggle = dirOfPath != config->realStoreDir.get();
    if (mustToggle)
        makeWritable(dirOfPath);

    /* When we're done, make the directory read-only again and reset
       its timestamp back to 0. */
    MakeReadOnly makeReadOnly(mustToggle ? dirOfPath : std::filesystem::path{});

    std::filesystem::path tempLink = makeTempPath(config->realStoreDir.get(), ".tmp-link");

    try {
        std::filesystem::create_hard_link(linkPath, tempLink);
        inodeHash.insert(st.st_ino);
    } catch (std::filesystem::filesystem_error & e) {
        if (e.code() == std::errc::too_many_links) {
            /* Too many links to the same file (>= 32000 on most file
               systems).  This is likely to happen with empty files.
               Just shrug and ignore. */
            if (st.st_size)
                printInfo("%1% has maximum number of links", PathFmt(linkPath));
            return;
        }
        throw SystemError(e.code(), "creating hard link from %1% to %2%", PathFmt(linkPath), PathFmt(tempLink));
    }

    /* Atomically replace the old file with the new hard link. */
    try {
        std::filesystem::rename(tempLink, path);
    } catch (std::filesystem::filesystem_error & e) {
        {
            std::error_code ec;
            remove(tempLink, ec); /* Clean up after ourselves. */
            if (ec)
                printError("unable to unlink %1%: %2%", PathFmt(tempLink), ec.message());
        }
        if (e.code() == std::errc::too_many_links) {
            /* Some filesystems generate too many links on the rename,
               rather than on the original link.  (Probably it
               temporarily increases the st_nlink field before
               decreasing it again.) */
            debug("%s has reached maximum number of links", PathFmt(linkPath));
            return;
        }
        throw SystemError(e.code(), "renaming %1% to %2%", PathFmt(tempLink), PathFmt(path));
    }

    stats.filesLinked++;
    stats.bytesFreed += st.st_size;

    if (act)
        act->result(
            resFileLinked,
            st.st_size
#ifndef _WIN32
            ,
            st.st_blocks
#endif
        );
}

void LocalStore::optimiseStore(OptimiseStats & stats)
{
    Activity act(*logger, actOptimiseStore);

    auto paths = queryAllValidPaths();
    InodeHash inodeHash = loadInodeHash();

    act.progress(0, paths.size());

    uint64_t done = 0;

    for (auto & i : paths) {
        addTempRoot(i);
        if (!isValidPath(i))
            continue; /* path was GC'ed, probably */
        {
            Activity act(*logger, lvlTalkative, actUnknown, fmt("optimising path '%s'", printStorePath(i)));
            optimisePath_(&act, stats, config->realStoreDir.get() / i.to_string(), inodeHash, NoRepair);
        }
        done++;
        act.progress(done, paths.size());
    }
}

void LocalStore::optimiseStore()
{
    OptimiseStats stats;

    optimiseStore(stats);

    printInfo("%s freed by hard-linking %d files", renderSize(stats.bytesFreed), stats.filesLinked);
}

void LocalStore::optimisePath(const std::filesystem::path & path, RepairFlag repair)
{
    OptimiseStats stats;
    InodeHash inodeHash;

    if (config->getLocalSettings().autoOptimiseStore)
        optimisePath_(nullptr, stats, path, inodeHash, repair);
}

/* Recreate `src`'s file tree at `dst`, hardlinking leaves (regular files
   and symlinks) and recursing into directories. On any link failure
   (EXDEV cross-device, EMLINK too-many-links, …) copy that one leaf's
   bytes instead — unlike `optimisePath_`, which can `return` and leave
   the pre-existing file, `dst` has NO pre-existing file, so we MUST
   produce it or `dst` is an incomplete/corrupt store path.

   Caller toggles the parent dir writable around the whole operation (the
   directories we `createDirs` here are freshly created and writable). */
static void linkOrCopyTree(const std::filesystem::path & src, const std::filesystem::path & dst)
{
    auto st = lstat(src);
    if (S_ISDIR(st.st_mode)) {
        createDirs(dst);
        /* `directory_iterator` construction/iteration throws
           `std::filesystem::filesystem_error` (a `std::system_error`, NOT
           a nix `Error`) on a transient read failure. The scheduler's
           per-sibling fallback (`materialiseGroup`) catches `Error` only,
           so an unwrapped `filesystem_error` would escape and abort the
           whole batch instead of degrading to a copy. Wrap it into a nix
           `SysError` so the intended "any link-time failure → copy"
           contract holds. */
        try {
            for (auto & entry : std::filesystem::directory_iterator{src})
                linkOrCopyTree(entry.path(), dst / entry.path().filename());
        } catch (std::filesystem::filesystem_error & e) {
            /* `e.what()` already carries the system error string, so use a
               plain `Error` rather than `SysError` (which would append a
               possibly-stale `errno`). */
            throw Error("iterating directory '%s' while linking store path: %s", src.string(), e.what());
        }
    } else {
        /* Regular file or symlink. */
        try {
            std::filesystem::create_hard_link(src, dst);
        } catch (std::filesystem::filesystem_error &) {
            /* EXDEV / EMLINK / etc. — fall back to a byte copy that
               preserves mode (incl. +x) and symlink targets.
               `andDelete=false`: we're creating, not moving. */
            copyFile(src, dst, /*andDelete=*/false);
        }
    }
}

/* Recursively assemble `dst` (a freshly-created, writable directory tree)
   to mirror `accessor`'s structure rooted at `relPath`, sourcing the
   bytes of UNCHANGED regular files from the corresponding file under
   `baseReal` (reflink, else hardlink, else copy) and writing CHANGED
   files from `accessor`. `relPath` is the path relative to the tree root
   (used to look up `baseReal / relPath` and to test membership in
   `changedFiles`).

   Returns false if assembly cannot proceed for this subtree (e.g. an
   unchanged file is absent from `base` — a sign the base is not actually
   a prefix of the target), so the caller can fall back to a full copy. */
static bool assembleTree(
    SourceAccessor & accessor,
    const CanonPath & relPath,
    const std::filesystem::path & baseReal,
    const std::filesystem::path & dst,
    const std::set<CanonPath> & changedFiles)
{
    auto st = accessor.lstat(relPath);

    switch (st.type) {

    case SourceAccessor::tDirectory: {
        createDirs(dst);
        for (auto & [name, _] : accessor.readDirectory(relPath)) {
            if (!assembleTree(accessor, relPath / name, baseReal / name, dst / name, changedFiles))
                return false;
        }
        return true;
    }

    case SourceAccessor::tSymlink: {
        /* Symlinks are tiny; always write fresh from the accessor (the
           target string is what matters; there are no extents to share). */
        createSymlink(accessor.readLink(relPath), dst);
        return true;
    }

    case SourceAccessor::tRegular: {
        bool changed = changedFiles.count(relPath) > 0;
        if (!changed) {
            /* Unchanged: source the bytes from base. Reflink (CoW) if the
               filesystem supports it, else hardlink, else byte-copy. If
               the base file is missing entirely, the base is not a valid
               prefix of this target — bail so the caller copies. */
            if (!pathExists(baseReal))
                return false;
            if (!tryCloneFile(baseReal, dst)) {
                try {
                    std::filesystem::create_hard_link(baseReal, dst);
                } catch (std::filesystem::filesystem_error &) {
                    copyFile(baseReal, dst, /*andDelete=*/false);
                }
            }
            /* Match the source's executable bit (clone/hardlink already
               carry it; an EXDEV copyFile preserves mode too — this is a
               cheap belt-and-braces before canonicalisation overwrites
               perms anyway). */
            return true;
        }
        /* Changed (added/modified): write the new bytes from the accessor,
           preserving the executable bit. */
        {
            AutoCloseFD fd{open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.isExecutable ? 0777 : 0666)};
            if (!fd)
                throw SysError("creating assembled file '%s'", dst.string());
            FdSink sink{fd.get()};
            accessor.readFile(relPath, sink);
            sink.flush();
        }
        return true;
    }

    case SourceAccessor::tChar:
    case SourceAccessor::tBlock:
    case SourceAccessor::tSocket:
    case SourceAccessor::tFifo:
    case SourceAccessor::tUnknown:
        /* Char/block/socket/fifo/unknown have no place in a store path. */
        throw Error("assembling store path: unsupported file type at '%s'", relPath.abs());
    }
    unreachable();
}

std::optional<StorePath> LocalStore::assembleCAPathFromBase(
    const StorePath & base,
    ref<SourceAccessor> accessor,
    const std::set<CanonPath> & changedFiles,
    const std::set<CanonPath> & deletedFiles,
    std::string_view name,
    std::optional<StorePath> expectedPath)
{
    if (config->readOnly)
        throw Error("cannot assemble a store path in a read-only store");
    assert(isValidPath(base));

    (void) deletedFiles; // deletions are implicit: absent from the accessor walk

    auto baseReal = toRealPath(base);

    /* Assemble into a temp dir in the store, hash it ONCE, derive the CA
       path from that hash, then atomically move it into place. We cannot
       name the destination before hashing (NAR-CA identity is content),
       so unlike the `toInfo` overload we build in a scratch location and
       relocate. The single hash here IS the re-hash guard — there is no
       separate DryRun, so no double walk. */
    auto [scratchParent, scratchFd] = createTempDirInStore();
    AutoDelete delScratch(scratchParent, /*recursive=*/true);
    auto scratch = std::filesystem::path{scratchParent} / "x";

    if (!assembleTree(*accessor, CanonPath::root, baseReal, scratch, changedFiles)) {
        debug("assembleCAPathFromBase: base '%s' not a usable prefix — caller should copy", baseReal.string());
        return std::nullopt;
    }

    auto h = hashPath(makeFSSourceAccessor(scratch), FileSerialisationMethod::NixArchive, HashAlgorithm::SHA256);

    auto ca = ContentAddressWithReferences::fromParts(
        ContentAddressMethod::Raw::NixArchive, h.hash, StoreReferences{.others = {}, .self = false});
    auto info = ValidPathInfo::makeFromCA(*this, name, std::move(ca), h.hash);
    info.narSize = h.numBytesDigested;

    /* If the evaluator already minted a name from the lock's narHash, the
       assembled content MUST hash to it; otherwise the source changed vs
       its lock (a stale lock). Report as the canonical NAR-hash mismatch. */
    if (expectedPath && info.path != *expectedPath)
        throw Error(
            (unsigned int) 102,
            "NAR hash mismatch for '%s': the source content does not match the hash it was locked with "
            "(expected store path '%s' but the content hashes to '%s'). Re-lock the input or remove the stale lock.",
            name,
            printStorePath(*expectedPath),
            printStorePath(info.path));

    auto dstReal = toRealPath(info.path);
    addTempRoot(info.path);
    PathLocks outputLock({dstReal});

    if (isValidPath(info.path))
        return info.path; /* a concurrent writer committed identical content */

    if (pathExists(dstReal))
        deletePath(dstReal);

    /* Move the assembled (still-writable) tree into place, THEN canonicalise
       at the destination — the same order `addToStoreFromDump` uses. Doing
       it the other way (canonicalise → rename) makes the scratch tree
       read-only first, and renaming a 0555 directory into the store fails
       with EACCES on the directory's own write bit. The rename is within
       the store (one filesystem) so it is atomic; we hold the parent dir
       writable across it. */
    const auto dirOfDst = std::filesystem::path{dstReal}.parent_path();
    bool mustToggle = dirOfDst != config->realStoreDir.get();
    if (mustToggle)
        makeWritable(dirOfDst);
    {
        MakeReadOnly makeReadOnly(mustToggle ? dirOfDst : std::filesystem::path{});
        std::filesystem::rename(scratch, dstReal);
    }
    /* `delScratch` still fires (we do NOT cancel it): the rename moved
       `scratchParent/x` into place, leaving `scratchParent` an EMPTY dir,
       and `AutoDelete(recursive)` removes that leftover. Cancelling here
       would leak an empty `tmp-*` dir in the store on every assembly
       (matching `addToStoreFromDump`, which likewise lets its temp parent
       be cleaned). */

    canonicalisePathMetaData(dstReal, {NIX_WHEN_SUPPORT_ACLS(config->getLocalSettings().ignoredAcls)});

    registerValidPath(info);
    return info.path;
}

void LocalStore::registerLinkedCAPath(const StorePath & from, const ValidPathInfo & toInfo)
{
    if (config->readOnly)
        throw Error("cannot register a linked store path in a read-only store");

    /* A self-reference would make toInfo's NAR differ from `from`'s (the
       embedded self-path hash-part differs), so byte-identity — the whole
       premise of linking — fails. Caller must filter these out. */
    assert(!toInfo.references.count(toInfo.path));

    assert(isValidPath(from));

    if (isValidPath(toInfo.path))
        return; /* already materialised (e.g. a concurrent sibling) */

    auto srcReal = toRealPath(from);
    auto dstReal = toRealPath(toInfo.path);

    /* Pin the destination before any bytes exist so a concurrent GC
       can't race the half-built path away. Held until registerValidPath
       commits below (all within this call). */
    addTempRoot(toInfo.path);

    /* Serialise concurrent writers of the SAME destination path. Two
       evaluations of one workspace (cross-process, or two scheduler
       threads) resolve identical sibling paths (same name + contentId),
       so without this lock both could enter the delete+link region on
       `dstReal` at once and corrupt the tree. Every other LocalStore
       byte-writer (`addToStoreFromDump`, `addToStore`) takes this lock;
       `addTempRoot` only guards against GC, not against other writers.
       Re-check validity UNDER the lock: a racer may have committed the
       path while we waited. */
    PathLocks outputLock({dstReal});

    if (isValidPath(toInfo.path))
        return; /* a concurrent sibling won the race and committed it */

    if (pathExists(dstReal)) {
        /* A stale/aborted prior attempt; clear it so the link tree is
           clean. (isValidPath was false above, so this isn't a live
           store object.) */
        deletePath(dstReal);
    }

    /* Toggle the parent dir writable while we create `dstReal`, then
       restore it (the store root itself is never chmod'd — guard like
       optimisePath_). */
    const auto dirOfDst = std::filesystem::path{dstReal}.parent_path();
    bool mustToggle = dirOfDst != config->realStoreDir.get();
    if (mustToggle)
        makeWritable(dirOfDst);
    MakeReadOnly makeReadOnly(mustToggle ? dirOfDst : std::filesystem::path{});

    linkOrCopyTree(srcReal, dstReal);

    /* Canonicalise (1970 mtime, 444/555 perms) so the tree matches what
       a fresh addToStoreFromDump would have produced, then register so
       GC + isValidPath see it as a first-class path. */
    canonicalisePathMetaData(dstReal, {NIX_WHEN_SUPPORT_ACLS(config->getLocalSettings().ignoredAcls)});

    /* `registerValidPath` re-derives the path from toInfo.ca + narHash
       and asserts it equals toInfo.path (via makeFromCA in the caller),
       so a mis-derived link is caught here, not silently committed. */
    registerValidPath(toInfo);
}

} // namespace nix
