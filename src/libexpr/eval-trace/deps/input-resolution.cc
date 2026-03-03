#include "nix/expr/eval-trace/deps/recording.hh"
#include "root-tracker-scope.hh"
#include "nix/expr/eval-trace/store/stat-hash-store.hh"
#include "nix/expr/eval.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-path.hh"

#include <filesystem>

namespace nix {

// ═══════════════════════════════════════════════════════════════════════
// Dep hash functions (BLAKE3 oracle hashing)
// ═══════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════
// Input resolution + dep recording
// ═══════════════════════════════════════════════════════════════════════

// Resolve an absolute filesystem path to an (input name, relative path) pair
// by walking up the directory hierarchy and matching against the flake input
// mount table. This produces stable, relocatable dependency keys (BSàlC §3.1:
// trace keys must be deterministic and reproducible across sessions).
// The subdir prefix is stripped so that deps are relative to the input root,
// not the mount point — ensuring the same trace key regardless of where the
// input is checked out on disk.
std::optional<std::pair<std::string, CanonPath>> resolveToInput(
    const CanonPath & absPath,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
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
    InterningPools & pools,
    const CanonPath & absPath,
    const DepHashValue & hash,
    DepType depType,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
{
    bool recorded = false;
    // Single lstat — reused for both existence gating and stat-hash-cache population
    std::optional<PosixStat> fileStat;

    if (!mountToInput.empty()) {
        if (auto resolved = resolveToInput(absPath, mountToInput)) {
            DependencyTracker::record(pools, depType, resolved->first, resolved->second.abs(), hash);
            recorded = true;
            // Flake input path — no lstat needed (accessor provides content)
        } else if ((fileStat = maybeLstat(std::filesystem::path(absPath.abs())))) {
            DependencyTracker::record(pools, depType, absolutePathDep, absPath.abs(), hash);
            recorded = true;
        }
        // else: virtual file — no filesystem oracle, skip (see above)
    } else if ((fileStat = maybeLstat(std::filesystem::path(absPath.abs())))) {
        DependencyTracker::record(pools, depType, "", absPath.abs(), hash);
        recorded = true;
    }
    // else: virtual file — no filesystem oracle, skip (see above)

    // Populate stat-hash cache for hashable deps (Shake: write-back of
    // computed hashes for future early-cutoff verification checks).
    // Best-effort — failures are silently ignored to avoid disrupting evaluation.
    if (recorded
        && (depType == DepType::Content || depType == DepType::Directory || depType == DepType::NARContent))
    {
        try {
            if (auto * b3 = std::get_if<Blake3Hash>(&hash))
                eval_trace::StatHashStore::instance().storeHash(
                    std::filesystem::path(absPath.abs()), depType, *b3, fileStat);
        } catch (...) {}
    }
}

// ═══════════════════════════════════════════════════════════════════════
// ReadFile provenance threading
// ═══════════════════════════════════════════════════════════════════════

// Content hash → ReadFileProvenance (field of RootTrackerScope).
// Populated by prim_readFile, queried by prim_fromJSON/prim_fromTOML to
// enable lazy structural dep tracking. Keyed by content hash so multiple
// readFile results coexist and the same provenance can serve multiple
// fromJSON/fromTOML calls (non-consuming lookup).
void addReadFileProvenance(ReadFileProvenance prov)
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->readFileProvenanceMap.insert_or_assign(prov.contentHash, std::move(prov));
}

const ReadFileProvenance * lookupReadFileProvenance(const Blake3Hash & contentHash)
{
    auto * scope = RootTrackerScope::current;
    if (!scope) return nullptr;
    auto it = scope->readFileProvenanceMap.find(contentHash);
    return it != scope->readFileProvenanceMap.end() ? &it->second : nullptr;
}

void clearReadFileProvenanceMap()
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->readFileProvenanceMap.clear();
}

// ═══════════════════════════════════════════════════════════════════════
// ReadFile string pointer tracking for RawContent deps
// ═══════════════════════════════════════════════════════════════════════

// String data pointer → content hash (field of RootTrackerScope).
// Populated by prim_readFile, queried by string builtins that observe raw bytes.
// Key is a raw const char* — valid because readFile strings are GC-allocated
// and stable within a root tracker scope. Typically empty or very small.
void addReadFileStringPtr(const char * ptr, const Blake3Hash & contentHash)
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->readFileStringPtrs.emplace(ptr, contentHash);
}

void clearReadFileStringPtrs()
{
    auto * scope = RootTrackerScope::current;
    if (scope) scope->readFileStringPtrs.clear();
}

[[gnu::cold]] void maybeRecordRawContentDep(EvalState & state, const Value & v)
{
    if (!DependencyTracker::isActive()) return;
    if (v.type() != nString) return;
    auto * scope = RootTrackerScope::current;
    if (!scope) return;
    auto it = scope->readFileStringPtrs.find(v.c_str());
    if (it == scope->readFileStringPtrs.end()) return;
    auto * prov = lookupReadFileProvenance(it->second);
    if (!prov) return;
    auto [source, key] = resolveProvenance(prov->path, state.getMountToInput());
    DependencyTracker::record(DependencyTracker::activeTracker->pools, DepType::RawContent, source, key, DepHashValue(it->second));
}

std::pair<std::string, std::string> resolveProvenance(
    const CanonPath & absPath,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput)
{
    if (!mountToInput.empty()) {
        if (auto resolved = resolveToInput(absPath, mountToInput))
            return {resolved->first, resolved->second.abs()};
        return {std::string(absolutePathDep), absPath.abs()};
    }
    return {"", absPath.abs()};
}

std::string dirEntryTypeString(std::optional<SourceAccessor::Type> type)
{
    if (!type) return "unknown";
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (*type) {
    case SourceAccessor::tRegular: return "regular";
    case SourceAccessor::tDirectory: return "directory";
    case SourceAccessor::tSymlink: return "symlink";
    default: return "unknown";
    }
#pragma GCC diagnostic pop
}

} // namespace nix
