#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/util/serialise.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/canon-path.hh"

#include <map>
#include <set>

namespace nix {

/**
 * dumpPath creates a Nix archive of the specified path.
 *
 * @param path the file system data to dump. Dumping is recursive so if
 * this is a directory we dump it and all its children.
 *
 * @param [out] sink The serialised archive is fed into this sink.
 *
 * @param filter Can be used to skip certain files.
 *
 * The format is as follows:
 *
 * ```
 * IF path points to a REGULAR FILE:
 *   dump(path) = attrs(
 *     [ ("type", "regular")
 *     , ("contents", contents(path))
 *     ])
 *
 * IF path points to a DIRECTORY:
 *   dump(path) = attrs(
 *     [ ("type", "directory")
 *     , ("entries", concat(map(f, sort(entries(path)))))
 *     ])
 *     where f(fn) = attrs(
 *       [ ("name", fn)
 *       , ("file", dump(path + "/" + fn))
 *       ])
 *
 * where:
 *
 *   attrs(as) = concat(map(attr, as)) + encN(0)
 *   attrs((a, b)) = encS(a) + encS(b)
 *
 *   encS(s) = encN(len(s)) + s + (padding until next 64-bit boundary)
 *
 *   encN(n) = 64-bit little-endian encoding of n.
 *
 *   contents(path) = the contents of a regular file.
 *
 *   sort(strings) = lexicographic sort by 8-bit value (strcmp).
 *
 *   entries(path) = the entries of a directory, without `.` and
 *   `..`.
 *
 *   `+` denotes string concatenation.
 * ```
 */
void dumpPath(const std::filesystem::path & path, Sink & sink, PathFilter & filter = defaultPathFilter);

/**
 * Same as dumpPath(), but returns the last modified date of the path.
 */
time_t dumpPathAndGetMtime(const std::filesystem::path & path, Sink & sink, PathFilter & filter = defaultPathFilter);

struct SourceAccessor;

/**
 * The set of paths that `SourceAccessor::dumpPath(path, sink, filter)`
 * would serialise: `path` itself and, below each directory in the set,
 * the entries the filter admits.  Prefix-closed by construction: it is
 * `traverse` -- the dump's own walk -- with a visitor that reads directory
 * listings and file types, not contents.
 */
std::set<CanonPath> filteredPaths(SourceAccessor & accessor, const CanonPath & path, PathFilter & filter);

/**
 * The name `dumpPath` serialises for an on-disk name: with
 * `use-case-hack` on, everything from `caseHackSuffix` on removed, which
 * undoes the renaming `restorePath()` applies to a case-colliding entry;
 * otherwise the name unchanged.  A view into `name`.
 *
 * @throws Error with the hack off when the name carries `caseHackSuffix`,
 * which the NAR format reserves (`parseDump` refuses it on every
 * platform), so that no on-disk name outside the format is serialised
 * or hashed.
 */
std::string_view unhackName(std::string_view name);

/**
 * The entries of a directory as `dumpPath` serialises them, keyed by the
 * name that is written (`unhackName` of the on-disk name) and mapping to
 * the name that is read -- the names the hash is over (01 §9.11).
 *
 * @throws Error when two on-disk names unhack to one, or, with the hack
 * off, when a name carries `caseHackSuffix`.
 */
StringMap unhackedEntries(SourceAccessor & accessor, const CanonPath & path);

/**
 * Dump an archive with a single file with these contents.
 *
 * @param s Contents of the file.
 */
void dumpString(std::string_view s, Sink & sink);

void parseDump(FileSystemObjectSink & sink, Source & source);

void restorePath(
    const std::filesystem::path & path, /**< Root where to unpack the NAR. */
    Source & source,                    /**< NAR source */
    bool startFsync = false,
    RestoreSinkHooks * hooks = nullptr /**< Hooks for canonicalising metadata. */
);

/**
 * Read a NAR from 'source' and write it to 'sink'.
 */
void copyNAR(Source & source, Sink & sink);

inline constexpr std::string_view narVersionMagic1 = "nix-archive-1";

inline constexpr std::string_view caseHackSuffix = "~nix~case~hack~";

/**
 * A tree's name whose case-hacked form is itself a name of the directory
 * (`CaseHackNames::diskName`): `name` as given, `existing` the name it
 * would overwrite.
 */
struct CaseHackCollision : Error
{
    std::string name, existing;

    CaseHackCollision(std::string name, std::string existing);
    ~CaseHackCollision() override;
};

/**
 * The renaming the NAR restorer applies within one directory under
 * `use-case-hack` (on by default on macOS, whose file systems fold
 * case): the second of two entry names that differ only in case gets
 * `caseHackSuffix` and a counter, which `unhackName` removes again when
 * the directory is serialised.  One instance per directory, fed the
 * tree's names in the order they are written (sorted); the same rule for
 * every writer of a tree to disk (`parseDump`, and the store's
 * materialisation from its objects), so that the two give one set of
 * on-disk names (01 §9.10 law 2).
 */
struct CaseHackNames
{
    /**
     * The on-disk name for the tree's `name`: `name` itself, or with the
     * suffix and a counter when a name given before differs from it only
     * in case; `name` unchanged with the hack off.
     *
     * @throws CaseHackCollision when the hacked name is itself a name
     * given before (an entry spelling the suffix in another case, which
     * the case-sensitive suffix check admits).
     */
    std::string diskName(std::string_view name);

private:
    struct CaseInsensitiveCompare
    {
        bool operator()(const std::string & a, const std::string & b) const;
    };

    std::map<std::string, int, CaseInsensitiveCompare> names;
};

/**
 * Maximum directory nesting depth for `dumpPath()` (through `traverse`)
 * and `parseDump()`.  Bounds stack usage so deep trees cannot overflow the
 * (possibly coroutine) stack these run on.
 */
inline constexpr size_t narMaxDepth = 64;

} // namespace nix
