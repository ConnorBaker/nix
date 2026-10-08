#include "nix/util/object-hash-sink.hh"
#include "nix/util/archive.hh"
#include "nix/util/error.hh"
#include "nix/util/finally.hh"

#include <algorithm>
#include <boost/container/small_vector.hpp>

namespace nix {

std::atomic<size_t> HashingVisitor::workersForTest{0};

size_t HashingVisitor::workerCountFor(SourceAccessor & accessor, PathFilter & filter)
{
    if (!filter.get_fn().target<AllPaths>() || !accessor.allowsConcurrentTraversal())
        return 1;
    auto workers = workersForTest.load(std::memory_order_relaxed);
    if (!workers)
        workers = CopyPool::defaultWorkers();
    return workers;
}

HashingRegularFileSink::~HashingRegularFileSink() = default;

HashedNode HashingVisitor::regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read)
{
    HashingRegularFileSink file{path};
    read(file);
    return file.finish();
}

HashedNode HashingVisitor::symlink(const CanonPath &, const std::string & target)
{
    return {
        .entry = {.mode = merkle::Mode::Symlink, .hash = merkle::blobId(target)},
        .narSize = merkle::nar::symlink(target.size()),
    };
}

HashedNode HashingVisitor::directory(const CanonPath & path, Children children)
{
    return hashDirectory(path, std::move(children)).first;
}

std::pair<HashedNode, std::string> HashingVisitor::hashDirectory(const CanonPath & path, Children children)
{
    merkle::Tree tree;
    merkle::nar::Directory size;
    bool sizeExact = true;
    auto consume = [&](std::string && name, const HashedNode & child) {
        if (child.entry.hash.algo != merkle::hashAlgo)
            throw Error(
                "object hash: '%s' was answered with a %s identifier, but the tree is hashed with %s",
                path / name,
                printHashAlgo(child.entry.hash.algo),
                printHashAlgo(merkle::hashAlgo));
        auto nextSize = size;
        if (child.narSize)
            nextSize.add(name, *child.narSize);
        merkle::insertEntry(tree, std::move(name), child.entry);
        size = nextSize;
        sizeExact &= child.narSize.has_value();
    };
    if (workers == 1 || !children.canDefer) {
        children([&](std::string_view name, bool, fun<HashedNode()> visit) {
            auto child = visit();
            consume(std::string(name), child);
        });
    } else {
        struct Child
        {
            std::string name;
            bool subdirectory;
            fun<HashedNode()> visit;
        };
        boost::container::small_vector<
            Child,
            1,
            void,
            boost::container::small_vector_options_t<
                boost::container::growth_factor<boost::container::growth_factor_100>>>
            kids;
        children([&](std::string_view name, bool subdirectory, fun<HashedNode()> visit) {
            kids.push_back({std::string(name), subdirectory, std::move(visit)});
        });
        if (kids.size() > 1 && std::ranges::any_of(kids.data(), kids.data() + kids.size(), &Child::subdirectory)) {
            if (!pool)
                pool = std::make_unique<CopyPool>(workers);
            std::vector<std::optional<HashedNode>> nodes(kids.size());
            auto fold = [&](size_t i) { consume(std::move(kids[i].name), nodes[i].value()); };
            pool->ordered(
                kids.size(),
                [&](size_t i) { return kids[i].subdirectory; },
                [&](size_t i) { nodes[i] = kids[i].visit(); },
                std::ref(fold));
        } else
            for (auto & child : kids)
                consume(std::move(child.name), child.visit());
    }

    auto body = merkle::serialiseTree(tree);
    return {
        HashedNode{
            .entry = {.mode = merkle::Mode::Directory, .hash = merkle::treeId(body)},
            .narSize = sizeExact ? std::optional(size.finish()) : std::nullopt,
        },
        std::move(body),
    };
}

ObjectHashSink::Result ObjectHashSink::Result::of(const HashedNode & root)
{
    return {
        .root = root.entry,
        .narSize = root.narSize.transform([](uint64_t size) { return merkle::nar::root(size); }),
    };
}

void ObjectHashSink::deliver(const CanonPath & path, fun<HashedNode()> visit)
{
    if (path.isRoot()) {
        if (root)
            throw Error("object hash sink: the root object was delivered twice");
        root = visit();
        return;
    }
    if (!open)
        throw Error("object hash sink: '%s' arrived with no open directory to hold it", path);
    /* The tree's own name: the NAR restorer may have appended the
       case-hack suffix before this child was delivered (archive.cc,
       `parse`), and the serialiser removes it again (`unhackedEntries`);
       so the hash and the NAR size are over the name without it. */
    /* The push driver has no listing to type a child by, and never
       needs one: only a visitor that may split its children reads it. */
    (*open)(unhackName(*path.baseName()), false, std::move(visit));
}

void ObjectHashSink::createDirectory(const CanonPath & path)
{
    throw Error("object hash sink: directory '%s' was created without its callback, which no driver does", path);
}

void ObjectHashSink::createDirectory(const CanonPath & path, DirectoryCreatedCallback callback)
{
    auto visit = [&] {
        auto children = [&](fun<void(std::string_view, bool, fun<HashedNode()>)> each) {
            auto previous = std::exchange(open, &each);
            Finally restore{[&] { open = previous; }};
            /* As the base class's default: the same sink, the same path, so
               the children arrive at `path / name`. */
            callback(*this, path);
        };
        return visitor.directory(path, {std::ref(children)});
    };
    deliver(path, std::ref(visit));
}

void ObjectHashSink::createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)> func)
{
    auto visit = [&] { return visitor.regular(path, std::move(func)); };
    deliver(path, std::ref(visit));
}

void ObjectHashSink::createSymlink(const CanonPath & path, const std::string & target)
{
    auto visit = [&] { return visitor.symlink(path, target); };
    deliver(path, std::ref(visit));
}

ObjectHashSink::Result ObjectHashSink::finish()
{
    if (!root)
        throw Error("object hash sink: no root object was delivered");
    return Result::of(*root);
}

void TeeFileSystemObjectSink::createDirectory(const CanonPath & path)
{
    a.createDirectory(aRoot / path);
    b.createDirectory(bRoot / path);
}

void TeeFileSystemObjectSink::createDirectory(const CanonPath & path, DirectoryCreatedCallback callback)
{
    a.createDirectory(aRoot / path, [&](FileSystemObjectSink & aDir, const CanonPath & aRel) {
        b.createDirectory(bRoot / path, [&](FileSystemObjectSink & bDir, const CanonPath & bRel) {
            TeeFileSystemObjectSink directory{aDir, bDir, aRel, bRel};
            callback(directory, CanonPath::root);
        });
    });
}

void TeeFileSystemObjectSink::createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)> func)
{
    struct TeeRegularFile : CreateRegularFileSink
    {
        CreateRegularFileSink & a;
        CreateRegularFileSink & b;

        TeeRegularFile(CreateRegularFileSink & a, CreateRegularFileSink & b)
            : a(a)
            , b(b)
        {
            skipContents = a.skipContents && b.skipContents;
        }

        void isExecutable() override
        {
            a.isExecutable();
            b.isExecutable();
            skipContents = a.skipContents && b.skipContents;
        }

        void preallocateContents(uint64_t size) override
        {
            a.preallocateContents(size);
            b.preallocateContents(size);
            skipContents = a.skipContents && b.skipContents;
        }

        void operator()(std::string_view data) override
        {
            if (!a.skipContents)
                a(data);
            if (!b.skipContents)
                b(data);
        }
    };

    a.createRegularFile(aRoot / path, [&](CreateRegularFileSink & aFile) {
        b.createRegularFile(bRoot / path, [&](CreateRegularFileSink & bFile) {
            TeeRegularFile both{aFile, bFile};
            func(both);
        });
    });
}

void TeeFileSystemObjectSink::createSymlink(const CanonPath & path, const std::string & target)
{
    a.createSymlink(aRoot / path, target);
    b.createSymlink(bRoot / path, target);
}

ObjectHashSink::Result objectHashOfNar(Source & source)
{
    HashingVisitor objectVisitor;
    ObjectHashSink sink{objectVisitor};
    parseDump(sink, source);
    return sink.finish();
}

ObjectHashSink::Result
objectHashOf(SourceAccessor & accessor, const CanonPath & path, PathFilter & filter, NodeVisitor<HashedNode> & visitor)
{
    return ObjectHashSink::Result::of(traverse(accessor, path, filter, visitor));
}

ObjectHashSink::Result objectHashOf(SourceAccessor & accessor, const CanonPath & path, PathFilter & filter)
{
    HashingVisitor hasher{HashingVisitor::workerCountFor(accessor, filter)};
    return objectHashOf(accessor, path, filter, hasher);
}

} // namespace nix
