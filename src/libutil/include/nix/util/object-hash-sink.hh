#pragma once
///@file

#include "nix/util/fs-sink.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/serialise.hh"
#include "nix/util/tree-traversal.hh"

#include <optional>
#include <string>
#include <vector>

namespace nix {

/**
 * What the object hasher records for one node of a tree (01 §9.11): its
 * entry (mode and identifier), its length in the NAR when every size
 * beneath it was seen -- a node a visitor's `known` answered without its
 * subtree's sizes has none -- and, for a directory, the tree body its
 * identifier is the hash of, which an object store writes as it is.
 */
struct HashedNode
{
    merkle::TreeEntry entry;
    std::optional<uint64_t> narSize;
    std::string treeBody;
};

/**
 * The object hasher as a visitor (`04` §1.7), under
 * `merkle::hashAlgo`, for both drivers, `traverse` and `ObjectHashSink`.
 * Consumers derive from it, override what they need and call this class's
 * action first; no method here calls another virtually.
 */
struct HashingVisitor : NodeVisitor<HashedNode>
{
    HashedNode regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override;

    HashedNode symlink(const CanonPath & path, const std::string & target) override;

    /**
     * @throws Error if a child's identifier is under another algorithm than
     * `merkle::hashAlgo` (a `known` answer from a wrong table), or two
     * children share a name.
     */
    HashedNode directory(const CanonPath & path, Children children) override;
};

/**
 * The push driver of a `NodeVisitor<HashedNode>`: the sink protocol
 * (`parseDump`) adapted to the visitor (`08` §1.6).  Children are
 * recorded under the tree's own name (`unhackName`); `known` is never
 * consulted, since a NAR's bytes cannot be skipped.  Without a visitor
 * of its own it hashes with a `HashingVisitor`.
 */
struct ObjectHashSink : FileSystemObjectSink
{
    struct Result
    {
        /**
         * The root's entry, mode included.  The object hash is
         * `merkle::objectHash(root)`.
         */
        merkle::TreeEntry root;

        /**
         * The length of the NAR `dumpPath` serialises the tree to.  Exact
         * only while `narSizeExact` holds.
         */
        uint64_t narSize;

        /**
         * False when a node was answered by a visitor's `known` without
         * its size (a directory from the fetchers' memo).  No store route
         * does that; the memo reads the root alone.  Read by the tests
         * alone (`libutil-tests/object-hash-sink.cc`), as the invariant's
         * witness.
         */
        bool narSizeExact = true;

        static Result of(const HashedNode & root);
    };

    HashingVisitor hasher;

    NodeVisitor<HashedNode> & visitor;

    ObjectHashSink()
        : visitor(hasher)
    {
    }

    explicit ObjectHashSink(NodeVisitor<HashedNode> & visitor)
        : visitor(visitor)
    {
    }

    /**
     * @throws Error always: a directory's children arrive inside the
     * callback form's scope; no driver of this sink uses the flat form.
     */
    void createDirectory(const CanonPath & path) override;

    void createDirectory(const CanonPath & path, DirectoryCreatedCallback callback) override;

    void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)>) override;

    void createSymlink(const CanonPath & path, const std::string & target) override;

    /**
     * The root's entry and the NAR size.
     *
     * @throws Error if no root object was delivered.
     */
    Result finish();

private:

    /**
     * For each directory whose callback is running, innermost last, the
     * visitor's per-entry function: each child delivered is handed to it
     * under the tree's own name.
     */
    std::vector<fun<void(std::string_view, fun<HashedNode()>)>> open;

    std::optional<HashedNode> root;

    /**
     * A node delivered at `path`: the root's, visited here, or an entry
     * of the innermost open directory, visited by its function.
     */
    void deliver(const CanonPath & path, fun<HashedNode()> visit);
};

/**
 * Forwards every call to two sinks, so one delivery feeds two consumers.
 * A regular file is opened on both, nested, and the function called once
 * with a sink forwarding `isExecutable`, `preallocateContents` and the
 * bytes (not to an inner sink that set `skipContents`; the composite skips
 * only when both do); `b`'s file is complete when `a`'s function returns.
 * Both must follow the base class's directory callback convention (same
 * sink, full path), and the composite hands back itself and the path.
 */
struct TeeFileSystemObjectSink : FileSystemObjectSink
{
    FileSystemObjectSink & a;
    FileSystemObjectSink & b;

    TeeFileSystemObjectSink(FileSystemObjectSink & a, FileSystemObjectSink & b)
        : a(a)
        , b(b)
    {
    }

    void createDirectory(const CanonPath & path) override;

    void createDirectory(const CanonPath & path, DirectoryCreatedCallback callback) override;

    void createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)>) override;

    void createSymlink(const CanonPath & path, const std::string & target) override;
};

/**
 * Convenience: the object hash and NAR size of a NAR stream.
 */
ObjectHashSink::Result objectHashOfNar(Source & source);

/**
 * Of a tree behind an accessor: `traverse` with `visitor`, a
 * `HashingVisitor` or a consumer derived from one, whose `known` may
 * answer a node without reading it.  `merkle::objectHash(result.root)` is
 * then the git content address of the tree.
 */
ObjectHashSink::Result
objectHashOf(SourceAccessor & accessor, const CanonPath & path, PathFilter & filter, NodeVisitor<HashedNode> & visitor);

/**
 * The same with a plain `HashingVisitor`.
 */
ObjectHashSink::Result
objectHashOf(SourceAccessor & accessor, const CanonPath & path, PathFilter & filter = defaultPathFilter);

} // namespace nix
