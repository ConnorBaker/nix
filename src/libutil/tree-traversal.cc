#include "nix/util/tree-traversal.hh"

#include <variant>

namespace nix {

void copyRecursive(
    SourceAccessor & accessor, const CanonPath & sourcePath, FileSystemObjectSink & sink, const CanonPath & destPath)
{
    struct Copy : NodeVisitor<std::monostate>
    {
        FileSystemObjectSink & sink;
        const CanonPath & under;

        Copy(FileSystemObjectSink & sink, const CanonPath & under) : sink(sink), under(under) {}

        std::pair<FileSystemObjectSink &, CanonPath> at(const CanonPath & path)
        {
            return {sink, path.isRoot() ? CanonPath(under) : under / *path.baseName()};
        }

        std::monostate regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override
        {
            auto [sink, p] = at(path);
            sink.createRegularFile(p, std::move(read));
            return {};
        }

        std::monostate symlink(const CanonPath & path, const std::string & target) override
        {
            auto [sink, p] = at(path);
            sink.createSymlink(p, target);
            return {};
        }

        std::monostate directory(const CanonPath & path, Children children) override
        {
            auto [sink, p] = at(path);
            sink.createDirectory(p, [&](FileSystemObjectSink & dirSink, const CanonPath & rel) {
                Copy child{dirSink, rel};
                children.each([&](std::string_view, bool, Visit visit) { visit(child); });
            });
            return {};
        }
    } copy{sink, destPath};
    traverse(accessor, sourcePath, defaultPathFilter, copy);
}

} // namespace nix
