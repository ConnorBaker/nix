#include "nix/util/tree-traversal.hh"
#include "nix/util/finally.hh"

#include <variant>

namespace nix {

void copyRecursive(
    SourceAccessor & accessor, const CanonPath & sourcePath, FileSystemObjectSink & sink, const CanonPath & destPath)
{
    struct Copy : NodeVisitor<std::monostate>
    {
        /* Each synchronous directory scope owns its copied coordinate;
           recursive calls retain their enclosing frame on the C++ stack. */
        using Target = std::pair<FileSystemObjectSink *, CanonPath>;
        const Target * open = nullptr;

        std::pair<FileSystemObjectSink &, CanonPath> at(const CanonPath & path)
        {
            auto & [sink, under] = *open;
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
                const Target target{&dirSink, rel};
                auto previous = std::exchange(open, &target);
                Finally restore{[&] { open = previous; }};
                children([](std::string_view, bool, fun<std::monostate()> visit) { visit(); });
            });
            return {};
        }
    } copy;

    const Copy::Target root{&sink, destPath};
    copy.open = &root;
    traverse(accessor, sourcePath, defaultPathFilter, copy);
}

} // namespace nix
