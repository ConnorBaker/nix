#include "nix/expr/dep-tracker.hh"
#include "nix/expr/stat-hash-cache.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/source-path.hh"

#include <filesystem>

namespace nix {

// Per-thread active dependency tracker for dynamic dependency discovery
// (Adapton: the "currently adapting" node whose edges are being recorded).
thread_local DependencyTracker * DependencyTracker::activeTracker = nullptr;
// Per-thread append-only dependency vector (Shake: the "journal" of all
// dependencies observed during this evaluation session).
thread_local std::vector<Dep> DependencyTracker::sessionTraces;

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

// Record a dependency edge in the dynamic dependency graph (Adapton: "add-edge").
// BSàlC §3.2: during fresh evaluation, the scheduler records each dependency
// into the trace for later verification. Deduplication ensures each (type,
// source, key) triple appears at most once per trace, matching Salsa's
// "dependency de-duplication" invariant.
void DependencyTracker::record(const Dep & dep)
{
    if (activeTracker) {
        DepKey key{dep.type, dep.source, dep.key};
        if (!activeTracker->recordedKeys.insert(key).second)
            return;  // Dependency already recorded in this trace scope — skip duplicate
    }
    debug("recording %s dep: input='%s' key='%s'", depTypeName(dep.type), dep.source, dep.key);
    sessionTraces.push_back(dep);
}

// Collect the complete trace for this evaluation scope (BSàlC §3.1: a trace
// is the ordered sequence of (key, hash) pairs observed during evaluation).
// Combines two sources:
//   1. Directly recorded deps from this scope [startIndex, endIndex)
//   2. Replayed deps from previously-verified thunks (Adapton: "demanded
//      computations" whose cached traces are transitively included)
// The result is the flattened dependency vector for trace storage.
std::vector<Dep> DependencyTracker::collectTraces() const
{
    uint32_t endIndex = mySessionTraces->size();

    if (replayedRanges.empty())
        return {mySessionTraces->begin() + startIndex, mySessionTraces->begin() + endIndex};

    std::vector<Dep> result(mySessionTraces->begin() + startIndex, mySessionTraces->begin() + endIndex);
    for (auto & r : replayedRanges)
        result.insert(result.end(), r.deps->begin() + r.start, r.deps->begin() + r.end);
    return result;
}

// Clear the session dependency vector between evaluation sessions.
// Called when the file cache is reset.
void DependencyTracker::clearSessionTraces()
{
    sessionTraces.clear();
}

// Compute Content dep hash with stat-cache acceleration (Shake: "file
// modification time" early-cutoff check before reading file contents).
// The stat-hash cache acts as an oracle memoization layer — if the file's
// stat metadata (dev, ino, mtime_ns, size) is unchanged since the last
// hashing, the cached BLAKE3 hash is returned without re-reading the file.
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

// Stat-cached NAR content hash (Shake: early-cutoff via stat metadata).
// Like depHashFile but hashes the NAR serialization, which captures both
// file content and the executable permission bit — needed for builtins.path
// deps where store path identity depends on permissions.
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

// Stat-cached directory listing hash (Shake: early-cutoff via stat metadata).
// Directory stat changes (mtime update on entry add/remove) trigger rehashing;
// unchanged stat metadata serves the cached hash without re-reading the listing.
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

// Resolve an absolute filesystem path to an (input name, relative path) pair
// by walking up the directory hierarchy and matching against the flake input
// mount table. This produces stable, relocatable dependency keys (BSàlC §3.1:
// trace keys must be deterministic and reproducible across sessions).
// The subdir prefix is stripped so that deps are relative to the input root,
// not the mount point — ensuring the same trace key regardless of where the
// input is checked out on disk.
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
                    return std::nullopt; // Path outside the flake's subdir — not a valid dep key
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

// Record a dependency edge during fresh evaluation (BSàlC §3.2: "recording
// scheduler"). Resolves the accessed path to a stable trace key, then appends
// the dependency to the active tracker's session dependency vector.
//
// Three recording modes, depending on flake input context:
//   1. Flake input path → (inputName, relativePath) key via resolveToInput
//   2. Out-of-tree real path → ("<absolute>", absPath) key (direct FS oracle)
//   3. Non-flake mode → ("", absPath) key
//
// Virtual files (MemorySourceAccessor, e.g., call-flake.nix, fetchurl.nix)
// are silently dropped — they have no filesystem oracle to verify against
// during trace verification (BSàlC: no hash function for synthetic inputs).
//
// After recording, the stat-hash cache is populated as a side effect
// (Shake: "write-back" of computed file hashes for future early-cutoff
// verification). This MUST happen after record() succeeds — if stat caching
// throws, the dependency is still recorded, preserving trace completeness.
void recordDep(
    const CanonPath & absPath,
    const DepHashValue & hash,
    DepType depType,
    const std::map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
{
    bool recorded = false;
    // Single lstat — reused for both existence gating and stat-hash-cache population
    std::optional<PosixStat> fileStat;

    if (!mountToInput.empty()) {
        if (auto resolved = resolveToInput(absPath, mountToInput)) {
            DependencyTracker::record({resolved->first, resolved->second.abs(), hash, depType});
            recorded = true;
            // Flake input path — no lstat needed (accessor provides content)
        } else {
            fileStat = maybeLstat(std::filesystem::path(absPath.abs()));
            if (fileStat) {
                // Real path outside flake input tree (e.g., store path from IFD).
                // Record with "<absolute>" sentinel — verification oracle reads
                // directly from the filesystem, bypassing input accessor resolution.
                DependencyTracker::record({std::string(absolutePathDep), absPath.abs(), hash, depType});
                recorded = true;
            }
            // else: virtual file — no filesystem oracle, skip (see above)
        }
    } else {
        fileStat = maybeLstat(std::filesystem::path(absPath.abs()));
        if (fileStat) {
            DependencyTracker::record({"", absPath.abs(), hash, depType});
            recorded = true;
        }
        // else: virtual file — no filesystem oracle, skip (see above)
    }

    // Populate stat-hash cache for hashable deps (Shake: write-back of
    // computed hashes for future early-cutoff verification checks).
    // Best-effort — failures are silently ignored to avoid disrupting evaluation.
    if (recorded
        && (depType == DepType::Content || depType == DepType::Directory || depType == DepType::NARContent))
    {
        try {
            if (auto * b3 = std::get_if<Blake3Hash>(&hash)) {
                if (fileStat) {
                    // Reuse existing stat metadata — avoids redundant lstat syscall
                    StatHashCache::instance().storeHash(
                        std::filesystem::path(absPath.abs()), depType, *b3, *fileStat);
                } else {
                    // Flake input path (resolved via mountToInput) — stat internally
                    StatHashCache::instance().storeHash(
                        std::filesystem::path(absPath.abs()), depType, *b3);
                }
            }
        } catch (...) {}
    }
}

} // namespace nix
