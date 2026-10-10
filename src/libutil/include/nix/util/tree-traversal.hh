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

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
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
    /* A child is independent of its consumer. The default consumer remains
       this visitor; a directory may substitute a scoped consumer whose
       lifetime includes every joined child. */
    using Visit = fun<Result(NodeVisitor &)>;
    /**
     * A directory's admitted entries in visit order: calls its argument
     * once per entry with the tree's own name, whether the accessor's
     * listing typed that entry as a directory, and `visit`, which visits
     * the child and returns its result.  A writer that frames entries (the
     * NAR) writes around `visit(visitor)`.
     *
     * The flag is **advisory** and nothing a visitor makes may depend on
     * it: a listing that gives no type says `false`, and the push driver,
     * which has no listing, says `false` for every child.  It exists so
     * that a visitor which may walk siblings on a pool can tell which of
     * them are worth one (`HashingVisitor::hashDirectory`,
     * `01` §9.10), without a `lstat` a child.
     *
     * The pull driver permits holding a child's `visit` until later in
     * this directory's scope; all accepted visits must finish before the
     * directory returns. The push driver requires immediate consumption:
     * its child's bytes and callback captures exist only during `each`.
     */
    /* The emitter must be valid and outlive directory(); each consumer must
       be valid and outlive the emitter's call. Neither callable is retained. */
    struct Children
    {
        using Each = function_ref<void(std::string_view name, bool isDirectory, Visit visit)>;
        function_ref<void(Each)> each;
        /* Lifetime permission, distinct from permission for concurrent
           observations. Push producers borrow each visit only until Each
           returns; pull producers lend it through the directory scope. */
        bool canDefer = false;
    };

    virtual ~NodeVisitor() = default;

    /**
     * Around one node, after its stat and before its contents are read.
     * `accessor` and `from` are the same scoped source used for that stat;
     * `path` is the visitor's logical coordinate. They outlive `descend`.
     * Its callable target must be valid and outlive node().
     * Return an already known summary or call `descend` exactly once and
     * return its result. The continuation is synchronous and must not be
     * retained. Its scope can carry a memo key without shared visitor state.
     * Never called by the push driver, which cannot skip a NAR's bytes.
     */
    virtual Result node(
        SourceAccessor & accessor,
        const CanonPath & from,
        const CanonPath & path,
        const SourceAccessor::Stat & st,
        function_ref<Result()> descend)
    {
        return descend();
    }

    /**
     * A regular file: `read(sink)` delivers it once -- `isExecutable()` when
     * the bit is set, `preallocateContents` when the size is known first,
     * then the bytes.
     */
    virtual Result regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) = 0;

    virtual Result symlink(const CanonPath & path, const std::string & target) = 0;

    /**
     * A directory: call `children.each` exactly once, and `visit(visitor)` once per
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
 * carrying the suffix refused. Returns the root's result. `logicalRoot`
 * continues an enclosing traversal at that logical path and depth, while
 * source reads and filter arguments remain relative to `path`.
 */
template<typename Result>
Result traverse(
    SourceAccessor & accessor,
    const CanonPath & path,
    PathFilter & filter,
    NodeVisitor<Result> & v,
    const CanonPath & logicalRoot = CanonPath::root)
{
    /* `from` is the node in `acc`'s coordinates (a `readDirectory` callback
       may scope the accessor to a subdirectory); `to` is the node relative
       to `path`, spelt with the accessor's names. */
    return [&](this const auto & walk, SourceAccessor & acc, const CanonPath & from, const CanonPath & to, size_t depth, NodeVisitor<Result> & visitor)
               -> Result {
        checkInterrupt();

        if (depth >= narMaxDepth)
            throw Error("path '%s' exceeds maximum NAR directory depth of %d", acc.showPath(from), narMaxDepth);

        auto st = acc.lstat(from);

        return visitor.node(acc, from, to, st, [&]() -> Result {
            switch (st.type) {
            case SourceAccessor::tRegular: {
                auto read = [&](CreateRegularFileSink & crf) {
                    if (st.isExecutable)
                        crf.isExecutable();
                    acc.readFile(from, crf, [&](uint64_t size) { crf.preallocateContents(size); });
                };
                return visitor.regular(to, std::ref(read));
            }

            case SourceAccessor::tSymlink:
                return visitor.symlink(to, acc.readLink(from));

            case SourceAccessor::tDirectory: {
                /* The entries and, from the same listing, which of them the
                   accessor typed as directories -- advisory, and read by a
                   visitor that may walk siblings on a pool (`Children`). */
                /* The accessor is scoped around the *whole* of `v.directory`,
                   not around the loop alone: an accessor's scoped form may be
                   a stack object of its own (`PosixDirectorySourceAccessor`
                   makes one and hands it to the callback), and a visitor is
                   allowed to hold a child's `visit` and call it after the loop
                   has ended -- which the parallel copy does (`01` §9.10). So
                   the scope must outlive every visit, and the only bound that
                   does is the directory node itself. */
                std::optional<Result> result;
                acc.readDirectoryWithEntries(
                    from, [&](auto & subdirAccessor, const auto & subdirFrom, auto && listing) {
                        const auto & entries = listing.entries();
                        /* Without a suffix the listing already has the tree's
                           unique names in its order, under either case-hack
                           setting. Otherwise validate and reorder every name
                           before exposing any directory or child callbacks. */
                        std::optional<StringMap> unhacked;
                        if (std::any_of(entries.begin(), entries.end(), [](const auto & entry) {
                                return entry.first.find(caseHackSuffix) != std::string::npos;
                            }))
                            unhacked = unhackedEntries(entries, acc, from);
                        auto visitChild = [&](const std::string & actual, NodeVisitor<Result> & childVisitor) {
                            return walk(subdirAccessor, subdirFrom / actual, to / actual, depth + 1, childVisitor);
                        };
                        auto children = [&](typename NodeVisitor<Result>::Children::Each each) {
                            auto child = [&](std::string_view name, const std::string & actual, auto type) {
                                if (filter.get_fn().target<AllPaths>() || filter((path / to.removePrefix(logicalRoot) / name).abs()))
                                    /* The context and name both live for this
                                       directory scope; each deferred visit only
                                       borrows those two existing objects. */
                                    each(
                                        std::string_view(name),
                                        type == SourceAccessor::tDirectory,
                                        [&visitChild, &actual](NodeVisitor<Result> & childVisitor) { return visitChild(actual, childVisitor); });
                            };
                            if (unhacked)
                                for (auto & [name, actual] : *unhacked)
                                    child(name, actual, entries.at(actual));
                            else
                                for (auto & [name, type] : entries)
                                    child(name, name, type);
                        };
                        result = visitor.directory(to, {children, true});
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
        });
    }(accessor, path, logicalRoot, std::ranges::distance(logicalRoot), v);
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
