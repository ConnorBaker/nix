#pragma once
///@file
/**
 * The one walk of a tree (01 §2.1; `08` §1.6):
 * `traverse` visits what `dumpPath` serialises, in its order, and a
 * `NodeVisitor` says what each node means.
 */

#include "nix/util/archive.hh"
#include "nix/util/fs-sink.hh"
#include "nix/util/fun.hh"
#include "nix/util/signals.hh"
#include "nix/util/source-accessor.hh"

#include <optional>
#include <set>
#include <string>
#include <utility>

namespace nix {

/**
 * The node actions of one traversal; each returns the node's summary.
 * Every `path` is relative to the traversal's root and spelt with the
 * accessor's own names, so a visitor can open the node (on macOS a restored
 * name may carry the case-hack suffix); the entry name handed with each
 * child is the tree's own (`unhackName`).  The same visitor serves the
 * pull driver (`traverse`) and the push driver (`ObjectHashSink` behind
 * `parseDump`): both deliver a regular file in the sink protocol's order
 * and a directory as a scope in which its children are visited.
 */
template<typename Result>
struct NodeVisitor
{
    /**
     * A directory's admitted entries in visit order: calls its argument
     * once per entry with the tree's own name, whether the accessor's
     * listing typed that entry as a directory, and `visit`, which visits
     * the child and returns its result.  A writer that frames entries (the
     * NAR) writes around `visit()`.
     *
     * The flag is **advisory** and nothing a visitor makes may depend on
     * it: a listing that gives no type says `false`, and the push driver,
     * which has no listing, says `false` for every child.  It exists so
     * that a visitor which may walk siblings on a pool can tell which of
     * them are worth one (`MaterialisingVisitor::copyChildren`,
     * `01` §9.10), without a `lstat` a child.
     *
     * A visitor may hold a child's `visit` and call it after the entries
     * have all been offered -- the accessor stays scoped around the whole
     * of `directory` for exactly that (below) -- but it must call every
     * one, and call `each` for every entry, before it returns.
     */
    using Children = fun<void(fun<void(std::string_view name, bool isDirectory, fun<Result()> visit)>)>;

    virtual ~NodeVisitor() = default;

    /**
     * Before any node is read, the root included: a summary the visitor
     * already knows (a memo row, an inode that is a blob's), or nullopt to
     * visit it.  What is returned is recorded and nothing at or beneath the
     * node is read.  Never consulted by the push driver, which cannot skip
     * a NAR's bytes.
     */
    virtual std::optional<Result> known(const CanonPath & path, const SourceAccessor::Stat & st)
    {
        return std::nullopt;
    }

    /**
     * A regular file: `read(sink)` delivers it once -- `isExecutable()` when
     * the bit is set, `preallocateContents` when the size is known first,
     * then the bytes.
     */
    virtual Result regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) = 0;

    virtual Result symlink(const CanonPath & path, const std::string & target) = 0;

    /**
     * A directory: call `children` exactly once, and `visit` once per
     * entry it hands over.  A scope the children need (a `RestoreSink`'s
     * directory callback) is opened around that call.
     */
    virtual Result directory(const CanonPath & path, Children children) = 0;
};

/**
 * The one walk: what `SourceAccessor::dumpPath(path, sink, filter)`
 * serialises, in the order it serialises it -- depth first, a directory's
 * entries by the name the NAR writes (`unhackedEntries`), the filter
 * applied to `path / <entry>` and never to `path` itself, a rejected
 * directory's descendants never asked about (01 §7.1), a tree deeper than
 * `narMaxDepth` refused, and with `use-case-hack` off an on-disk name
 * carrying the suffix refused.  Returns the root's result.
 */
template<typename Result>
Result traverse(SourceAccessor & accessor, const CanonPath & path, PathFilter & filter, NodeVisitor<Result> & v)
{
    /* `from` is the node in `acc`'s coordinates (a `readDirectory` callback
       may scope the accessor to a subdirectory); `to` is the node relative
       to `path`, spelt with the accessor's names. */
    return [&](this const auto & walk, SourceAccessor & acc, const CanonPath & from, const CanonPath & to, size_t depth)
               -> Result {
        checkInterrupt();

        if (depth >= narMaxDepth)
            throw Error("path '%s' exceeds maximum NAR directory depth of %d", acc.showPath(from), narMaxDepth);

        auto st = acc.lstat(from);

        if (auto r = v.known(to, st))
            return std::move(*r);

        switch (st.type) {
        case SourceAccessor::tRegular:
            return v.regular(to, [&](CreateRegularFileSink & crf) {
                if (st.isExecutable)
                    crf.isExecutable();
                acc.readFile(from, crf, [&](uint64_t size) { crf.preallocateContents(size); });
            });

        case SourceAccessor::tSymlink:
            return v.symlink(to, acc.readLink(from));

        case SourceAccessor::tDirectory: {
            /* The entries and, from the same listing, which of them the
               accessor typed as directories -- advisory, and read by a
               visitor that may walk siblings on a pool (`Children`). */
            std::set<std::string, std::less<>> subdirs;
            auto unhacked = unhackedEntries(acc, from, [&](const std::string & name, bool isDirectory) {
                if (isDirectory)
                    subdirs.insert(name);
            });
            auto filterBase = path / to;
            /* The accessor is scoped around the *whole* of `v.directory`,
               not around the loop alone: an accessor's scoped form may be
               a stack object of its own (`PosixDirectorySourceAccessor`
               makes one and hands it to the callback), and a visitor is
               allowed to hold a child's `visit` and call it after the loop
               has ended -- which the parallel copy does (`01` §9.10). So
               the scope must outlive every visit, and the only bound that
               does is the directory node itself. */
            std::optional<Result> result;
            acc.readDirectory(from, [&](SourceAccessor & subdirAccessor, const CanonPath & subdirFrom) {
                result = v.directory(to, [&](fun<void(std::string_view, bool, fun<Result()>)> each) {
                    for (auto & [name, actual] : unhacked)
                        if (filter((filterBase / name).abs()))
                            each(name, subdirs.contains(name), [&] {
                                return walk(subdirAccessor, subdirFrom / actual, to / actual, depth + 1);
                            });
                });
            });
            assert(result);
            return std::move(*result);
        }

        case SourceAccessor::tChar:
        case SourceAccessor::tBlock:
        case SourceAccessor::tSocket:
        case SourceAccessor::tFifo:
        case SourceAccessor::tUnknown:
        default:
            throw Error("file '%1%' has an unsupported type of %2%", from, st.typeString());
        }
    }(accessor, path, CanonPath::root, 0);
}

/**
 * Copy the tree at `sourcePath` of `accessor` into `sink` at `destPath`:
 * `traverse` with a visitor that creates each node on the sink, under the
 * sink its directory callback handed back and the name it gave (the base
 * convention: the same sink and the full path; `RestoreSink`: the
 * subdirectory's own sink under `/`).
 */
void copyRecursive(
    SourceAccessor & accessor, const CanonPath & sourcePath, FileSystemObjectSink & sink, const CanonPath & destPath);

} // namespace nix
