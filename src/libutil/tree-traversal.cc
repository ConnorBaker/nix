#include "nix/util/tree-traversal.hh"
#include "nix/util/finally.hh"

#include <variant>

namespace nix {

void copyRecursive(
    SourceAccessor & accessor, const CanonPath & sourcePath, FileSystemObjectSink & sink, const CanonPath & destPath)
{
    struct Copy : NodeVisitor<std::monostate>
    {
        /* The sink and the path a node's children go under: what the
           enclosing directory's callback handed back, innermost last; the
           root's frame is the caller's sink and `destPath`. */
        std::vector<std::pair<FileSystemObjectSink *, CanonPath>> open;

        std::pair<FileSystemObjectSink &, CanonPath> at(const CanonPath & path)
        {
            auto & [sink, under] = open.back();
            return {*sink, path.isRoot() ? under : under / *path.baseName()};
        }

        std::monostate regular(const CanonPath & path, fun<void(CreateRegularFileSink &)> read) override
        {
            auto [sink, p] = at(path);
            sink.createRegularFile(p, read);
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
                open.emplace_back(&dirSink, rel);
                Finally pop{[&] { open.pop_back(); }};
                children([](std::string_view, fun<std::monostate()> visit) { visit(); });
            });
            return {};
        }
    } copy;

    copy.open.emplace_back(&sink, destPath);
    traverse(accessor, sourcePath, defaultPathFilter, copy);
}

} // namespace nix
