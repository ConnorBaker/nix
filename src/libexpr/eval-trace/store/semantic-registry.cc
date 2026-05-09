/// semantic-registry.cc — SemanticRegistry implementation.

#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/util/file-system.hh"

namespace nix::eval_trace {

std::optional<SourcePath> SemanticRegistry::resolve(
    const DepSource & source, const std::string & key) const
{
    switch (source.kind()) {
    case DepSourceKind::Absolute:
        if (key.empty() || key[0] != '/')
            return std::nullopt;
        return SourcePath(getFSSourceAccessor(), CanonPath(key));

    case DepSourceKind::Registered: {
        auto it = entries_.find(source);
        if (it == entries_.end()) {
            auto sourceDisplay = serializeDepSource(source);
            warn("eval-trace/registry: resolve: registered source '%s' not found "
                 "in %zu entries — dep was recorded with an identity that "
                 "doesn't match any graph node key or runtime root",
                 sourceDisplay, entries_.size());
            return std::nullopt;
        }
        if (key.empty())
            return it->second;
        return it->second / CanonPath(key);
    }
    }
    unreachable();
}

std::optional<std::pair<DepSource, CanonPath>> SemanticRegistry::reverseResolve(
    const CanonPath & absPath) const
{
    auto path = absPath;
    std::vector<std::string> subpathParts;
    while (true) {
        CanonPath relPath = CanonPath::root;
        for (auto it = subpathParts.rbegin(); it != subpathParts.rend(); ++it)
            relPath = relPath / *it;

        auto mountIt = mountPoints_.find(path);
        if (mountIt != mountPoints_.end()) {
            for (auto & [source, subdir] : mountIt->second) {
                if (subdir.value != CanonPath::root) {
                    if (!relPath.isWithin(subdir.value))
                        continue;
                    relPath = relPath.removePrefix(subdir.value);
                }
                return {{source, relPath}};
            }
        }

        if (path.isRoot())
            break;
        auto bn = path.baseName();
        if (bn)
            subpathParts.push_back(std::string(*bn));
        path.pop();
    }
    return std::nullopt;
}

std::optional<PathObject> SemanticRegistry::resolvePathObject(const SourcePath & path) const
{
    auto mountPoint = path.path;
    while (true) {
        auto mountIt = mountPoints_.find(mountPoint);
        if (mountIt != mountPoints_.end()) {
            for (auto & [source, subdir] : mountIt->second) {
                auto rootPath = subdir.value == CanonPath::root
                    ? mountPoint
                    : mountPoint / subdir.value;
                if (!(path.path == rootPath || path.path.isWithin(rootPath)))
                    continue;
                return PathObject{.source = source, .rootPath = std::move(rootPath)};
            }
        }
        if (mountPoint.isRoot())
            break;
        mountPoint.pop();
    }
    return std::nullopt;
}

} // namespace nix::eval_trace
