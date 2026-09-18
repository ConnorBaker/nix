#include "nix/util/object-hash-sink.hh"
#include "nix/util/archive.hh"
#include "nix/util/error.hh"
#include "nix/util/finally.hh"

namespace nix {

HashedNode HashingVisitor::regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read)
{
    /* Receives one file in the order both drivers deliver it: the
       executable flag, then the size, then the bytes.  The size opens the
       blob's header, so the identifier is complete when the bytes are and
       nothing is buffered. */
    struct FileHasher : CreateRegularFileSink
    {
        const CanonPath & path;
        bool executable = false;
        std::optional<merkle::BlobHasher> hasher;
        uint64_t announced = 0;
        uint64_t received = 0;

        /* A fallback for a driver that never announces the size: the
           bytes are kept and hashed whole at the end.  Neither driver
           takes this path. */
        std::string buffer;

        explicit FileHasher(const CanonPath & path)
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
            hasher.emplace(size);
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

        /* The blob's identifier and the file's size. */
        std::pair<Hash, uint64_t> finish()
        {
            if (!hasher)
                return {merkle::blobId(buffer), buffer.size()};
            if (received != announced)
                throw Error("object hash: file '%s' announced %d bytes but delivered %d", path, announced, received);
            return {hasher->finish(), announced};
        }
    };

    FileHasher f{path};
    read(f);
    auto [id, size] = f.finish();
    return {
        .entry = {.mode = f.executable ? merkle::Mode::Executable : merkle::Mode::Regular, .hash = id},
        .narSize = merkle::nar::regular(size, f.executable),
    };
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
    merkle::Tree tree;
    merkle::nar::Directory size;
    bool sizeExact = true;
    children([&](std::string_view name, fun<HashedNode()> visit) {
        auto child = visit();
        if (child.entry.hash.algo != merkle::hashAlgo)
            throw Error(
                "object hash: '%s' was answered with a %s identifier, but the tree is hashed with %s",
                path / name,
                printHashAlgo(child.entry.hash.algo),
                printHashAlgo(merkle::hashAlgo));
        merkle::insertEntry(tree, name, child.entry);
        if (child.narSize)
            size.add(name, *child.narSize);
        else
            sizeExact = false;
    });

    auto body = merkle::serialiseTree(tree);
    return {
        .entry = {.mode = merkle::Mode::Directory, .hash = merkle::treeId(body)},
        .narSize = sizeExact ? std::optional(size.finish()) : std::nullopt,
        .treeBody = std::move(body),
    };
}

ObjectHashSink::Result ObjectHashSink::Result::of(const HashedNode & root)
{
    return {
        .root = root.entry,
        .narSize = merkle::nar::root(root.narSize.value_or(0)),
        .narSizeExact = root.narSize.has_value(),
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
    if (open.empty())
        throw Error("object hash sink: '%s' arrived with no open directory to hold it", path);
    /* The tree's own name: the NAR restorer may have appended the
       case-hack suffix before this child was delivered (archive.cc,
       `parse`), and the serialiser removes it again (`unhackedEntries`);
       so the hash and the NAR size are over the name without it. */
    open.back()(unhackName(*path.baseName()), std::move(visit));
}

void ObjectHashSink::createDirectory(const CanonPath & path)
{
    throw Error("object hash sink: directory '%s' was created without its callback, which no driver does", path);
}

void ObjectHashSink::createDirectory(const CanonPath & path, DirectoryCreatedCallback callback)
{
    deliver(path, [&] {
        return visitor.directory(path, [&](fun<void(std::string_view, fun<HashedNode()>)> each) {
            open.push_back(std::move(each));
            Finally close{[&] { open.pop_back(); }};
            /* As the base class's default: the same sink, the same path, so
               the children arrive at `path / name`. */
            callback(*this, path);
        });
    });
}

void ObjectHashSink::createRegularFile(const CanonPath & path, fun<void(CreateRegularFileSink &)> func)
{
    deliver(path, [&] { return visitor.regular(path, func); });
}

void ObjectHashSink::createSymlink(const CanonPath & path, const std::string & target)
{
    deliver(path, [&] { return visitor.symlink(path, target); });
}

ObjectHashSink::Result ObjectHashSink::finish()
{
    if (!root)
        throw Error("object hash sink: no root object was delivered");
    return Result::of(*root);
}

void TeeFileSystemObjectSink::createDirectory(const CanonPath & path)
{
    a.createDirectory(path);
    b.createDirectory(path);
}

void TeeFileSystemObjectSink::createDirectory(const CanonPath & path, DirectoryCreatedCallback callback)
{
    /* Both inner sinks follow the base class's convention -- the callback
       is handed the same sink and the directory's full path -- so the
       composite does too. */
    a.createDirectory(path, [&](FileSystemObjectSink &, const CanonPath & aRel) {
        b.createDirectory(path, [&](FileSystemObjectSink &, const CanonPath & bRel) {
            if (aRel != path || bRel != path)
                throw Error("tee sink: a sink renamed directory '%s' ('%s', '%s')", path, aRel, bRel);
            callback(*this, path);
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
        }

        void preallocateContents(uint64_t size) override
        {
            a.preallocateContents(size);
            b.preallocateContents(size);
        }

        void operator()(std::string_view data) override
        {
            if (!a.skipContents)
                a(data);
            if (!b.skipContents)
                b(data);
        }
    };

    a.createRegularFile(path, [&](CreateRegularFileSink & aFile) {
        b.createRegularFile(path, [&](CreateRegularFileSink & bFile) {
            TeeRegularFile both{aFile, bFile};
            func(both);
        });
    });
}

void TeeFileSystemObjectSink::createSymlink(const CanonPath & path, const std::string & target)
{
    a.createSymlink(path, target);
    b.createSymlink(path, target);
}

ObjectHashSink::Result objectHashOfNar(Source & source)
{
    ObjectHashSink sink;
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
    HashingVisitor hasher;
    return objectHashOf(accessor, path, filter, hasher);
}

} // namespace nix
