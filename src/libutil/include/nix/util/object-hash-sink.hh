#pragma once
///@file

#include "nix/util/fs-sink.hh"
#include "nix/util/merkle-hash.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/serialise.hh"
#include "nix/util/tree-traversal.hh"
#include "nix/util/thread-pool.hh"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nix {

/**
 * What the object hasher records for one node of a tree (01 §9.11): its
 * entry (mode and identifier), its length in the NAR when every size
 * beneath it was seen -- a node a visitor's `node` answered without its
 * subtree's sizes has none. A directory's encoded body is needed only
 * while hashing and publishing that directory, not by its parent.
 */
struct HashedNode
{
    merkle::TreeEntry entry;
    std::optional<uint64_t> narSize;
};

/** Hash a regular node as size-prefixed bytes arrive, retaining bytes until
 * their size is announced. Borrows path; finish once after delivery. */
struct HashingRegularFileSink final : CreateRegularFileSink
{
private:
    const CanonPath & path;
    bool executable = false;
    std::optional<merkle::ObjectHasher> hasher;
    uint64_t announced = 0;
    uint64_t received = 0;
    std::string buffer;

public:
    ~HashingRegularFileSink() override;

    explicit HashingRegularFileSink(std::reference_wrapper<const CanonPath> path)
        : path(path)
    {
    }

    void isExecutable() override
    {
        executable = true;
    }

    void preallocateContents(uint64_t size) override
    {
        if (hasher)
            throw Error("object hash: the size of file '%s' was announced twice", path);
        announced = size;
        hasher.emplace("blob", size);
        if (!buffer.empty()) {
            (*hasher)(buffer);
            buffer.clear();
        }
    }

    void operator()(std::string_view data) override
    {
        received += data.size();
        if (hasher)
            (*hasher)(data);
        else
            buffer.append(data);
    }

    HashedNode finish()
    {
        auto size = hasher ? announced : buffer.size();
        if (hasher && received != announced)
            throw Error("object hash: file '%s' announced %d bytes but delivered %d", path, announced, received);
        return {
            .entry = {
                .mode = executable ? merkle::Mode::Executable : merkle::Mode::Regular,
                .hash = hasher ? hasher->finish() : merkle::blobId(buffer),
            },
            .narSize = merkle::nar::regular(size, executable),
        };
    }
};

/**
 * The object hasher as a visitor (`04` §1.7), under
 * `merkle::hashAlgo`, for both drivers, `traverse` and `ObjectHashSink`.
 * Consumers derive from it, override what they need and call this class's
 * action first; no method here calls another virtually.
 */
struct HashingVisitor : NodeVisitor<HashedNode>
{
private:
    size_t workers;
    std::unique_ptr<CopyPool> pool;

public:
    /* The default, including the push driver and arbitrary subclasses,
       consumes every child immediately. Only audited pull consumers
       permit more than one worker. */
    explicit HashingVisitor(size_t workers = 1)
        : workers(workers)
    {
    }

    /** Test-only width for audited hash/copy walks; zero selects the bounded
     * machine width, and tests enumerate 1..8. Permissions still gate concurrency. */
    static std::atomic<size_t> workersForTest;

    static size_t workerCountFor(SourceAccessor & accessor, PathFilter & filter)
    {
        if (!filter.get_fn().target<AllPaths>() || !accessor.allowsConcurrentTraversal())
            return 1;
        auto workers = workersForTest.load(std::memory_order_relaxed);
        if (!workers)
            workers = CopyPool::defaultWorkers();
        return workers;
    }

    HashedNode regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override;

    HashedNode symlink(const CanonPath & path, const std::string & target) override;

    /**
     * @throws Error if a child's identifier is under another algorithm than
     * `merkle::hashAlgo` (a node answer from a wrong table), or two
     * children share a name.
     */
    HashedNode directory(const CanonPath & path, Children children) override;

protected:
    /** The directory summary and its encoding, for a consumer that publishes the body. */
    std::pair<HashedNode, std::string> hashDirectory(const CanonPath & path, Children children);
};

/**
 * The push driver of a `NodeVisitor<HashedNode>`: the sink protocol
 * (`parseDump`) adapted to the visitor (`08` §1.6).  Children are
 * recorded under the tree's own name (`unhackName`); `node` is never
 * consulted, since a NAR's bytes cannot be skipped. The caller owns the
 * visitor; this stationary adapter borrows it through callbacks and finish.
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
         * The exact length of the NAR `dumpPath` serialises the tree to,
         * absent when a skipped node supplied no size.
         */
        std::optional<uint64_t> narSize;

        static Result of(const HashedNode & root);
    };

    NodeVisitor<HashedNode> & visitor;

    ObjectHashSink(const ObjectHashSink &) = delete;
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
     * The active directory lends its per-entry function for immediate child
     * delivery. A recursive directory saves and restores this pointer without
     * moving the enclosing callback or its owned state.
     */
    NodeVisitor<HashedNode>::Children::Each * open = nullptr;

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
 * A directory callback receives a scoped composite under `/`; each inner
 * sink and its returned path remain live for that scope.
 */
struct TeeFileSystemObjectSink : FileSystemObjectSink
{
    FileSystemObjectSink & a;
    FileSystemObjectSink & b;

private:
    const CanonPath & aRoot;
    const CanonPath & bRoot;

    TeeFileSystemObjectSink(
        FileSystemObjectSink & a, FileSystemObjectSink & b, const CanonPath & aRoot, const CanonPath & bRoot)
        : a(a)
        , b(b)
        , aRoot(aRoot)
        , bRoot(bRoot)
    {
    }

public:
    TeeFileSystemObjectSink(FileSystemObjectSink & a, FileSystemObjectSink & b)
        : TeeFileSystemObjectSink(a, b, CanonPath::root, CanonPath::root)
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
 * `HashingVisitor` or a consumer derived from one, whose `node` may
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
