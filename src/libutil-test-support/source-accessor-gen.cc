#include <exception> // IWYU pragma: keep (Needed by rapidcheck on Darwin and FreeBSD)
#include <rapidcheck.h>

#include "nix/util/serialise.hh"
#include "nix/util/archive.hh"
#include "nix/util/hash.hh"

#include "nix/util/tests/source-accessor-gen.hh"

namespace nix {

rc::Gen<MemorySourceAccessor::File> genFile(int depth, const FileGenSpec & spec)
{
    using File = MemorySourceAccessor::File;

    auto regular =
        rc::gen::map(rc::gen::pair(rc::gen::arbitrary<bool>(), spec.contents), [](std::pair<bool, std::string> p) {
            return File{File::Regular{.executable = p.first, .contents = std::move(p.second)}};
        });
    auto symlink =
        rc::gen::map(spec.targets, [](std::string t) { return File{File::Symlink{.target = std::move(t)}}; });
    if (depth == 0)
        return rc::gen::oneOf(regular, symlink);

    auto children =
        rc::gen::container<std::vector<rc::Maybe<File>>>(spec.names.size(), rc::gen::maybe(genFile(depth - 1, spec)));
    auto directory = rc::gen::map(children, [names = spec.names](std::vector<rc::Maybe<File>> children) {
        File::Directory d;
        for (size_t i = 0; i < names.size(); ++i)
            if (children[i])
                d.entries.emplace(names[i], *children[i]);
        return File{std::move(d)};
    });
    return rc::gen::oneOf(regular, symlink, directory, directory);
}

std::string narOf(SourceAccessor & accessor, const CanonPath & path, PathFilter & filter)
{
    StringSink sink;
    accessor.dumpPath(path, sink, filter);
    return std::move(sink.s);
}

ref<MemorySourceAccessor> memoryTree(std::initializer_list<std::pair<const char *, const char *>> files)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    MemorySink sink{*accessor};
    sink.createDirectory(CanonPath::root);
    for (auto & [path, contents] : files)
        accessor->addFile(CanonPath(path), contents);
    return accessor;
}

merkle::TreeEntry referenceObjectEntry(SourceAccessor & acc, const CanonPath & p, PathFilter & filter)
{
    auto st = acc.lstat(p);
    switch (st.type) {
    case SourceAccessor::tRegular:
        return {
            .mode = st.isExecutable ? merkle::Mode::Executable : merkle::Mode::Regular,
            .hash = merkle::blobId(acc.readFile(p))};
    case SourceAccessor::tSymlink:
        return {.mode = merkle::Mode::Symlink, .hash = merkle::blobId(acc.readLink(p))};
    case SourceAccessor::tDirectory: {
        merkle::Tree t;
        for (auto & [name, actual] : unhackedEntries(acc, p))
            if (filter((p / name).abs()))
                merkle::insertEntry(t, name, referenceObjectEntry(acc, p / actual, filter));
        return {.mode = merkle::Mode::Directory, .hash = merkle::treeId(merkle::serialiseTree(t))};
    }
    case SourceAccessor::tChar:
    case SourceAccessor::tBlock:
    case SourceAccessor::tSocket:
    case SourceAccessor::tFifo:
    case SourceAccessor::tUnknown:
    default:
        throw Error("reference hasher: unsupported type at '%s'", p);
    }
}

ContentNamedAccessor::ContentNamedAccessor(ref<SourceAccessor> inner)
    : inner(inner)
{
    displayPrefix.clear();
}

void ContentNamedAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    inner->readFile(path, sink, sizeCallback);
}

std::optional<SourceAccessor::Stat> ContentNamedAccessor::maybeLstat(const CanonPath & path)
{
    return inner->maybeLstat(path);
}

SourceAccessor::DirEntries ContentNamedAccessor::readDirectory(const CanonPath & path)
{
    return inner->readDirectory(path);
}

std::string ContentNamedAccessor::readLink(const CanonPath & path)
{
    return inner->readLink(path);
}

std::pair<CanonPath, std::optional<std::string>> ContentNamedAccessor::getFingerprint(const CanonPath & path)
{
    if (!inner->maybeLstat(path))
        return {path, std::nullopt};
    return {CanonPath::root, "nar:" + inner->hashPath(path).to_string(HashFormat::Nix32, false)};
}

} // namespace nix
