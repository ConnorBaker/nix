#include "nix/expr/file-load-tracker.hh"
#include "nix/expr/stat-hash-cache.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/source-path.hh"

#include <filesystem>

namespace nix {

thread_local FileLoadTracker * FileLoadTracker::activeTracker = nullptr;
thread_local std::vector<Dep> FileLoadTracker::sessionDeps;

Blake3Hash depHash(std::string_view data)
{
    return Blake3Hash::fromHash(hashString(HashAlgorithm::BLAKE3, data));
}

Blake3Hash depHashPath(const SourcePath & path)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    path.dumpPath(sink);
    return Blake3Hash::fromHash(sink.finish().hash);
}

Blake3Hash depHashDirListing(const SourceAccessor::DirEntries & entries)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & [name, type] : entries) {
        sink(name);
        sink(":");
        int typeInt = type ? static_cast<int>(*type) : -1;
        auto typeStr = std::to_string(typeInt);
        sink(typeStr);
        sink(";");
    }
    return Blake3Hash::fromHash(sink.finish().hash);
}

const char * depTypeName(DepType type)
{
    switch (type) {
    case DepType::Content: return "content";
    case DepType::Directory: return "directory";
    case DepType::Existence: return "existence";
    case DepType::EnvVar: return "envvar";
    case DepType::CurrentTime: return "currentTime";
    case DepType::System: return "system";
    case DepType::UnhashedFetch: return "unhashedFetch";
    case DepType::ParentContext: return "parentContext";
    case DepType::CopiedPath: return "copiedPath";
    case DepType::Exec: return "exec";
    case DepType::NARContent: return "narContent";
    }
    unreachable();
}

void FileLoadTracker::record(const Dep & dep)
{
    if (activeTracker) {
        DepKey key{dep.type, dep.source, dep.key};
        if (!activeTracker->recordedKeys.insert(key).second)
            return;  // Already recorded this (type, source, key) in this tracker scope
    }
    debug("recording %s dep: input='%s' key='%s'", depTypeName(dep.type), dep.source, dep.key);
    sessionDeps.push_back(dep);
}

std::vector<Dep> FileLoadTracker::collectDeps() const
{
    uint32_t endIndex = mySessionDeps->size();

    if (replayedRanges.empty())
        return {mySessionDeps->begin() + startIndex, mySessionDeps->begin() + endIndex};

    std::vector<Dep> result(mySessionDeps->begin() + startIndex, mySessionDeps->begin() + endIndex);
    for (auto & r : replayedRanges)
        result.insert(result.end(), r.deps->begin() + r.start, r.deps->begin() + r.end);
    return result;
}

void FileLoadTracker::clearSessionDeps()
{
    sessionDeps.clear();
}

Blake3Hash depHashFile(const SourcePath & path)
{
    if (auto physPath = path.getPhysicalPath()) {
        auto result = StatHashCache::instance().lookupHash(*physPath, DepType::Content);
        if (result.hash)
            return *result.hash;
        auto content = path.readFile();
        auto hash = depHash(content);
        if (result.stat)
            StatHashCache::instance().storeHash(*physPath, DepType::Content, hash, *result.stat);
        return hash;
    }
    return depHash(path.readFile());
}

Blake3Hash depHashPathCached(const SourcePath & path)
{
    if (auto physPath = path.getPhysicalPath()) {
        auto result = StatHashCache::instance().lookupHash(*physPath, DepType::NARContent);
        if (result.hash)
            return *result.hash;
        auto hash = depHashPath(path);
        if (result.stat)
            StatHashCache::instance().storeHash(*physPath, DepType::NARContent, hash, *result.stat);
        return hash;
    }
    return depHashPath(path);
}

Blake3Hash depHashDirListingCached(const SourcePath & path, const SourceAccessor::DirEntries & entries)
{
    if (auto physPath = path.getPhysicalPath()) {
        auto result = StatHashCache::instance().lookupHash(*physPath, DepType::Directory);
        if (result.hash)
            return *result.hash;
        auto hash = depHashDirListing(entries);
        if (result.stat)
            StatHashCache::instance().storeHash(*physPath, DepType::Directory, hash, *result.stat);
        return hash;
    }
    return depHashDirListing(entries);
}

std::optional<std::pair<std::string, CanonPath>> resolveToInput(
    const CanonPath & absPath,
    const std::map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
{
    auto path = absPath;
    std::vector<std::string> subpathParts;
    while (true) {
        if (auto it = mountToInput.find(path); it != mountToInput.end()) {
            auto & [nodeKey, subdir] = it->second;
            std::reverse(subpathParts.begin(), subpathParts.end());
            CanonPath relPath = CanonPath::root;
            for (auto & part : subpathParts)
                relPath = relPath / part;
            if (!subdir.empty()) {
                auto subdirPath = CanonPath("/" + subdir);
                if (!relPath.isWithin(subdirPath))
                    return std::nullopt; // Path outside the flake's subdir
                relPath = relPath.removePrefix(subdirPath);
            }
            return {{nodeKey, relPath}};
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

void recordDep(
    const CanonPath & absPath,
    const DepHashValue & hash,
    DepType depType,
    const std::map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
{
    bool recorded = false;
    // Stat once, reuse for both existence check and stat-hash-cache population
    std::optional<PosixStat> fileStat;

    if (!mountToInput.empty()) {
        if (auto resolved = resolveToInput(absPath, mountToInput)) {
            FileLoadTracker::record({resolved->first, resolved->second.abs(), hash, depType});
            recorded = true;
            // fileStat stays nullopt — flake input path, no direct lstat needed
        } else {
            fileStat = maybeLstat(std::filesystem::path(absPath.abs()));
            if (fileStat) {
                // Path outside flake input tree but exists on the real filesystem
                // (e.g., store paths from IFD). Record with "<absolute>" sentinel
                // so validation reads directly from the filesystem.
                FileLoadTracker::record({std::string(absolutePathDep), absPath.abs(), hash, depType});
                recorded = true;
            }
            // else: virtual file (e.g., call-flake.nix from MemorySourceAccessor)
            // — not a real filesystem path, nothing to validate against.
        }
    } else {
        fileStat = maybeLstat(std::filesystem::path(absPath.abs()));
        if (fileStat) {
            FileLoadTracker::record({"", absPath.abs(), hash, depType});
            recorded = true;
        }
        // else: virtual file (e.g., fetchurl.nix from MemorySourceAccessor)
        // — not a real filesystem path, nothing to validate against.
    }

    // Populate stat-hash cache for file-based deps (best-effort).
    if (recorded
        && (depType == DepType::Content || depType == DepType::Directory || depType == DepType::NARContent))
    {
        try {
            if (auto * b3 = std::get_if<Blake3Hash>(&hash)) {
                if (fileStat) {
                    // Use 4-arg overload — no redundant lstat
                    StatHashCache::instance().storeHash(
                        std::filesystem::path(absPath.abs()), depType, *b3, *fileStat);
                } else {
                    // Flake input path (resolved via mountToInput) — use 2-arg overload
                    StatHashCache::instance().storeHash(
                        std::filesystem::path(absPath.abs()), depType, *b3);
                }
            }
        } catch (...) {}
    }
}

} // namespace nix
