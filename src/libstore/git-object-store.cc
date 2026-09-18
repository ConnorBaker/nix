#include "nix/store/git-object-store.hh"
#include "nix/store/posix-fs-canonicalise.hh"
#include "nix/util/git.hh"
#include "nix/util/file-system.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/finally.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/strings.hh"
#include "nix/util/util.hh"

#include <boost/unordered/unordered_flat_set.hpp>

#include <atomic>
#include <cstring>
#include <random>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nix {

GitObjectStore::GitObjectStore(std::filesystem::path dir, bool fsync)
    : dir(std::move(dir))
    , fsync(fsync)
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

std::filesystem::path GitObjectStore::blobFile(const Hash & id, bool executable) const
{
    return dir / (executable ? "blobs-x" : "blobs") / idString(id);
}

std::filesystem::path GitObjectStore::treeFile(const Hash & id) const
{
    return dir / "trees" / idString(id);
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

#ifndef _WIN32
/* A hard link `name` in the directory `dirFd` for the file `from`: the
   directory resolved once is the directory written. */
static std::error_code linkPathIntoDir(const std::filesystem::path & from, Descriptor dirFd, const CanonPath & name)
{
    if (linkat(AT_FDCWD, from.c_str(), dirFd, name.rel_c_str(), 0) == -1)
        return {errno, std::generic_category()};
    return {};
}

/* The converse: a hard link `to`, by full path, for `name` in `dirFd`. */
static std::error_code linkDirEntryToPath(Descriptor dirFd, const CanonPath & name, const std::filesystem::path & to)
{
    if (linkat(dirFd, name.rel_c_str(), AT_FDCWD, to.c_str(), 0) == -1)
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

std::optional<PosixStat>
GitObjectStore::blobFileOrRemove(const Hash & id, bool executable, std::optional<uint64_t> size, RepairFlag repair)
{
    auto file = blobFile(id, executable);
    auto st = maybeLstat(file);
    if (!st)
        return std::nullopt;
    if ((size && static_cast<uint64_t>(st->st_size) != *size) || (repair && blobIdOfFile(file) != id)) {
        warn("removing corrupt object %s", PathFmt(file));
        warn(
            "There may be more corrupted paths."
            "\nYou should run `nix-store --verify --check-contents --repair` to fix them all");
        unlinkIfExists(file);
        return std::nullopt;
    }
    return st;
}

bool GitObjectStore::link(
    const Hash & id,
    bool executable,
    Descriptor dirFd,
    const CanonPath & name,
    const std::filesystem::path & where,
    std::optional<uint64_t> size,
    RepairFlag repair,
    PosixStat * linked)
{
#ifdef _WIN32
    return false;
#else
    auto st = blobFileOrRemove(id, executable, size, repair);
    if (!st)
        return false;
    auto file = blobFile(id, executable);
    if (auto ec = linkPathIntoDir(file, dirFd, name)) {
        /* Swept meanwhile, or at the filesystem's link limit: the caller
           writes the file instead. */
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::too_many_links)
            return false;
        throw Error("creating hard link from %1% to %2%: %3%", PathFmt(file), PathFmt(where), ec.message());
    }
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
    RepairFlag repair)
{
#ifdef _WIN32
    return std::nullopt;
#else
    /* Twice at most: the second time after another writer entered the
       identifier between our look and our attempt. */
    auto file = blobFile(id, executable);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (auto stFile = blobFileOrRemove(id, executable, static_cast<uint64_t>(before.st_size), repair)) {
            if (stFile->st_ino == before.st_ino)
                return Placed{before.st_ino, false};
            auto tempLink = tempPath("link");
            if (auto ec = hardLink(file, tempLink)) {
                if (ec == std::errc::too_many_links) {
                    printInfo("%1% has maximum number of links; %2% is left as it is", PathFmt(file), PathFmt(where));
                    return std::nullopt;
                }
                if (ec == std::errc::no_such_file_or_directory)
                    continue;
                throw Error("creating hard link from %1% to %2%: %3%", PathFmt(file), PathFmt(tempLink), ec.message());
            }
            if (auto ec = renameInto(tempLink, dirFd, name)) {
                unlinkIfExists(tempLink);
                if (ec == std::errc::too_many_links) {
                    printInfo("%1% has maximum number of links; %2% is left as it is", PathFmt(file), PathFmt(where));
                    return std::nullopt;
                }
                throw Error("replacing %1% by a link to %2%: %3%", PathFmt(where), PathFmt(file), ec.message());
            }
            /* The inode of what now stands under the name, not of the file
               looked at above: between that look and the link the store's
               file may have been swept and entered again, and the link is
               then to the new file (`blobInodes` must not learn the old
               number). */
            PosixStat placed;
            if (fstatat(dirFd, name.rel_c_str(), &placed, AT_SYMLINK_NOFOLLOW) == -1)
                throw SysError("getting status of %1%", PathFmt(where));
            return Placed{placed.st_ino, false};
        }
        /* A miss: the file becomes the store's.  The store's file appearing
           meanwhile (another writer entered the identifier) sends us round
           once more, to link to it. */
        auto ec = linkDirEntryToPath(dirFd, name, file);
        if (!ec)
            return Placed{before.st_ino, true};
        if (ec == std::errc::file_exists)
            continue;
        if (ec == std::errc::too_many_links || ec == std::errc::no_space_on_device) {
            /* The link limit, or on ext4 a full directory index: the file
               is then simply not shared, as before.  Nothing on the file
               says so afterwards (`placementOf` cannot tell it from a file
               never entered), so `--verify --check-contents` reports it
               and `--repair` tries again (01 §9.10 law 3). */
            printInfo("cannot enter %s as %s: %s", PathFmt(where), PathFmt(file), ec.message());
            return std::nullopt;
        }
        throw Error("creating hard link from %1% to %2%: %3%", PathFmt(where), PathFmt(file), ec.message());
    }
    return std::nullopt;
#endif
}

/* Write a file's bytes to the fresh temporary file `tmp`, canonicalised,
   and link it into place; an entry that appeared meanwhile stands.  The
   temporary file is removed however this ends.  Under `fsync` the file --
   its bytes and its canonical mode and timestamp, which are set first so
   that the flush covers them (fsync(2) flushes the file's data and
   metadata) -- is flushed before it is linked, and its directory after. */
static void writeObjectFile(
    const std::filesystem::path & tmp,
    const std::filesystem::path & dst,
    std::string_view bytes,
    bool executable,
    bool fsync)
{
    if (pathExists(dst))
        return;
    Finally removeTmp{[&] {
        try {
            unlinkIfExists(tmp);
        } catch (...) {
            ignoreExceptionExceptInterrupt();
        }
    }};
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
        canonicaliseTimestampAndPermissions(tmp);
        if (fsync)
            fd.fsync();
    }
    if (auto ec = hardLink(tmp, dst)) {
        if (ec == std::errc::file_exists)
            return;
        if (ec == std::errc::no_space_on_device) {
            /* The object is not kept; every reader tolerates its absence. */
            printInfo("cannot write object %s: %s", PathFmt(dst), ec.message());
            return;
        }
        throw Error("creating hard link from %1% to %2%: %3%", PathFmt(tmp), PathFmt(dst), ec.message());
    }
    if (fsync)
        syncParent(dst);
}

void GitObjectStore::putBlob(const Hash & id, bool executable, std::string_view bytes)
{
    writeObjectFile(tempPath("object"), blobFile(id, executable), bytes, executable, fsync);
}

void GitObjectStore::putTree(const Hash & id, std::string_view body)
{
    writeObjectFile(tempPath("object"), treeFile(id), body, false, fsync);
}

void GitObjectStore::putRootTree(const merkle::TreeEntry & root)
{
    if (auto body = merkle::syntheticRootTree(root))
        putTree(merkle::treeId(*body), *body);
}

std::optional<std::string> GitObjectStore::readTree(const Hash & id) const
{
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
    for (auto & sub : {"blobs", "blobs-x"}) {
        for (auto & entry : DirectoryIterator{dir / sub}) {
            checkInterrupt();
            Hash id(hashAlgo);
            try {
                id = Hash::parseNonSRIUnprefixed(entry.path().filename().string(), hashAlgo);
            } catch (BadHash &) {
                /* Not an object of ours. */
                continue;
            }
            /* By `lstat`, the key `knownRegularFile` looks up: `readdir`'s
               `d_ino` can differ from it (overlayfs without xino).  A file
               swept meanwhile is simply absent. */
            if (auto st = maybeLstat(entry.path()))
                inodes.insert_or_assign(st->st_ino, id);
        }
    }
#endif
    return inodes;
}

GitObjectStore::SweepStats GitObjectStore::sweep(fun<uint64_t(fun<void(const ObjectHash &)>)> forEachLiveObjectHash)
{
    SweepStats stats;
    createDirectories();

    /* Temporary files a crash left behind: none lives an hour.  First,
       before the blobs are looked at, so that a leftover link to a blob --
       which holds the blob's link count above one -- goes before the blob
       is judged.  The age is the name's when the name carries one
       (`tempPath`: a link's inode is the blob's, and its change time the
       blob's), else the inode's change time (a name an earlier build
       made). */
    auto now = time(nullptr);
    for (auto & entry : DirectoryIterator{dir / "tmp"}) {
        checkInterrupt();
        auto made = tempTime(entry.path().filename().string());
        if (!made)
            if (auto st = maybeLstat(entry.path()))
                made = st->st_ctime;
        if (made && now - *made > 3600)
            unlinkIfExists(entry.path());
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
                else if (pathExists(blobFile(id, false)))
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
                "%s (written by an older Nix, or added while a collection ran); nothing is kept for them."
                "\nRun `nix-store --optimise` to enter them (or `nix-store --verify --check-contents --repair`)",
                what);
        }
        if (!stats.corruptTrees) {
            for (auto & entry : DirectoryIterator{dir / "trees"}) {
                checkInterrupt();
                if (reachable.count(entry.path().filename().string()))
                    continue;
                unlinkIfExists(entry.path());
                stats.treesRemoved++;
            }

            /* Blobs: a file with no link but the store's own, that no
               reachable tree names, is unreferenced. */
            for (auto & sub : {"blobs", "blobs-x"}) {
                for (auto & entry : DirectoryIterator{dir / sub}) {
                    checkInterrupt();
                    auto st = maybeLstat(entry.path());
                    if (!st)
                        continue;
                    if (st->st_nlink != 1) {
                        stats.blobsKept++;
                        stats.sharedBytes +=
                            static_cast<uint64_t>(st->st_nlink - 2) * static_cast<uint64_t>(st->st_size);
                        continue;
                    }
                    if (named.count(entry.path().filename().string())) {
                        stats.blobsKept++;
                        continue;
                    }
                    printMsg(lvlTalkative, "deleting unreferenced object %1%", PathFmt(entry.path()));
                    unlinkIfExists(entry.path());
                    stats.blobsRemoved++;
                }
            }
        }
    }

    return stats;
}

} // namespace nix
