#include "nix/expr/eval-cache-store.hh"
#include "nix/util/hash.hh"
#include "nix/util/serialise.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/store/content-address.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/util/environment-variables.hh"

#include <regex>

namespace nix::eval_cache {

// Forward declarations for dep validation helpers
static std::optional<SourcePath> resolveDepPathForStore(
    const Dep & dep, const std::map<std::string, SourcePath> & inputAccessors)
{
    if (dep.source == absolutePathDep)
        return SourcePath(getFSSourceAccessor(), CanonPath(dep.key));
    auto it = inputAccessors.find(dep.source);
    if (it != inputAccessors.end())
        return it->second / CanonPath(dep.key);
    if (dep.source.empty())
        return SourcePath(getFSSourceAccessor(), CanonPath(dep.key));
    return std::nullopt;
}

static std::optional<DepHashValue> computeCurrentHashForStore(
    EvalState & state, const Dep & dep,
    const std::map<std::string, SourcePath> & inputAccessors)
{
    switch (dep.type) {
    case DepType::Content: {
        auto path = resolveDepPathForStore(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(depHashFile(*path));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::NARContent: {
        auto path = resolveDepPathForStore(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(depHashPathCached(*path));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::Directory: {
        auto path = resolveDepPathForStore(dep, inputAccessors);
        if (!path) return std::nullopt;
        try {
            return DepHashValue(depHashDirListingCached(*path, path->readDirectory()));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::Existence: {
        auto path = resolveDepPathForStore(dep, inputAccessors);
        if (!path) return std::nullopt;
        auto st = path->maybeLstat();
        return DepHashValue(st
            ? fmt("type:%d", static_cast<int>(st->type))
            : std::string("missing"));
    }
    case DepType::EnvVar:
        return DepHashValue(depHash(getEnv(dep.key).value_or("")));
    case DepType::System:
        return DepHashValue(depHash(state.settings.getCurrentSystem()));
    case DepType::CopiedPath: {
        auto sourcePath = resolveDepPathForStore(dep, inputAccessors);
        if (!sourcePath) return std::nullopt;
        try {
            auto * storePathStr = std::get_if<std::string>(&dep.expectedHash);
            if (!storePathStr) return std::nullopt;
            auto expectedStorePath = state.store->parseStorePath(*storePathStr);
            auto name2 = std::string(expectedStorePath.name());
            auto [storePath, hash] = state.store->computeStorePath(
                name2,
                sourcePath->resolveSymlinks(SymlinkResolution::Ancestors),
                ContentAddressMethod::Raw::NixArchive,
                HashAlgorithm::SHA256, {});
            return DepHashValue(state.store->printStorePath(storePath));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::UnhashedFetch: {
        try {
            auto input = fetchers::Input::fromURL(state.fetchSettings, dep.key);
            auto [storePath, lockedInput] = input.fetchToStore(
                state.fetchSettings, *state.store);
            return DepHashValue(state.store->printStorePath(storePath));
        } catch (std::exception &) {
            return std::nullopt;
        }
    }
    case DepType::CurrentTime:
    case DepType::Exec:
    case DepType::ParentContext:
        return std::nullopt;
    }
    unreachable();
}

// ── Dep type naming ──────────────────────────────────────────────────

std::string depTypeString(DepType type)
{
    switch (type) {
    case DepType::Content:       return "content";
    case DepType::Directory:     return "directory";
    case DepType::Existence:     return "existence";
    case DepType::EnvVar:        return "envvar";
    case DepType::CurrentTime:   return "current-time";
    case DepType::System:        return "system";
    case DepType::UnhashedFetch: return "unhashed-fetch";
    case DepType::ParentContext: return "parent-context";
    case DepType::CopiedPath:    return "copied-path";
    case DepType::Exec:          return "exec";
    case DepType::NARContent:    return "nar-content";
    }
    return "unknown";
}

// ── EvalCacheStore ───────────────────────────────────────────────────

EvalCacheStore::EvalCacheStore(Store & store, SymbolTable & symbols, int64_t contextHash)
    : store(store)
    , symbols(symbols)
    , contextHash(contextHash)
{
}

bool EvalCacheStore::isStaged(const std::string & attrPath) const
{
    return stagedAttrPaths.count(attrPath) > 0;
}

void EvalCacheStore::defer(const std::string & attrPath, DeferredColdStore * write)
{
    stagedAttrPaths.insert(attrPath);
    deferredWrites.push_back(write);
}

void EvalCacheStore::flush()
{
    for (auto * w : deferredWrites) {
        try {
            w->execute();
        } catch (std::exception & e) {
            debug("deferred eval cache write failed: %s", e.what());
        } catch (...) {
            debug("deferred eval cache write failed (unknown error)");
        }
    }
    deferredWrites.clear();
    stagedAttrPaths.clear();
}

EvalCacheStore::~EvalCacheStore()
{
    try {
        flush();
    } catch (...) {
        ignoreExceptionExceptInterrupt();
    }
}

void EvalCacheStore::clearSessionCaches()
{
    validatedTraces.clear();
    validatedDepSets.clear();
    depSetCache.clear();
}

std::string EvalCacheStore::sanitizeName(std::string_view name)
{
    if (name.empty())
        return "root";
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        // Store path names allow [a-zA-Z0-9+\-._?=]
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '-' ||
            c == '.' || c == '_' || c == '?' || c == '=')
            result.push_back(c);
        else
            result.push_back('-');
    }
    // Store path names must not start with '.'
    if (!result.empty() && result[0] == '.')
        result[0] = '-';
    return result;
}

std::string EvalCacheStore::buildAttrPath(const std::vector<std::string> & components)
{
    std::string path;
    for (size_t i = 0; i < components.size(); i++) {
        if (i > 0) path.push_back('\0');
        path.append(components[i]);
    }
    return path;
}

// ── Trace I/O ────────────────────────────────────────────────────────

StorePath EvalCacheStore::storeTrace(
    std::string_view name,
    const std::vector<uint8_t> & cbor,
    const StorePathSet & references)
{
    // Compute expected path for dedup check
    auto contentHash = hashString(HashAlgorithm::SHA256,
        {reinterpret_cast<const char *>(cbor.data()), cbor.size()});
    auto ca = ContentAddressWithReferences::fromParts(
        ContentAddressMethod::Raw::Text, contentHash,
        StoreReferences{.others = references, .self = false});
    auto tracePath = store.makeFixedOutputPathFromCA(name, ca);

    if (store.isValidPath(tracePath))
        return tracePath; // Already stored (content-addressed dedup)

    auto sv = std::string_view(reinterpret_cast<const char *>(cbor.data()), cbor.size());
    StringSource source(sv);
    store.addToStoreFromDump(source, name,
        FileSerialisationMethod::Flat, ContentAddressMethod::Raw::Text,
        HashAlgorithm::SHA256, references);
    return tracePath;
}

EvalTrace EvalCacheStore::loadTrace(const StorePath & tracePath)
{
    auto accessor = store.requireStoreObjectAccessor(tracePath);
    auto content = accessor->readFile(CanonPath::root);
    std::vector<uint8_t> cbor(content.begin(), content.end());
    return deserializeEvalTrace(cbor, symbols, store);
}

// ── Dep Set I/O ──────────────────────────────────────────────────────

StorePath EvalCacheStore::storeDepSet(const std::vector<uint8_t> & compressed)
{
    return storeTrace("eval-deps", compressed, {});
}

std::vector<Dep> EvalCacheStore::loadDepSet(const StorePath & depSetPath)
{
    auto it = depSetCache.find(depSetPath);
    if (it != depSetCache.end())
        return it->second;

    auto accessor = store.requireStoreObjectAccessor(depSetPath);
    auto content = accessor->readFile(CanonPath::root);
    std::vector<uint8_t> compressed(content.begin(), content.end());
    auto deps = deserializeDepSet(compressed);

    depSetCache.emplace(depSetPath, deps);
    return deps;
}

std::vector<Dep> EvalCacheStore::loadDepsForTrace(const StorePath & tracePath)
{
    auto trace = loadTrace(tracePath);
    return loadDepSet(trace.depSetPath);
}

// ── Trace Validation ─────────────────────────────────────────────────

bool EvalCacheStore::validateTrace(
    const StorePath & tracePath,
    const std::map<std::string, SourcePath> & inputAccessors,
    EvalState & state)
{
    if (validatedTraces.count(tracePath))
        return true;

    if (!store.isValidPath(tracePath))
        return false;

    auto trace = loadTrace(tracePath);

    // Check dep set blob validity
    if (!store.isValidPath(trace.depSetPath))
        return false;

    // Check if dep set already validated (sibling sharing)
    if (!validatedDepSets.count(trace.depSetPath)) {
        auto deps = loadDepSet(trace.depSetPath);

        // Validate each dep
        for (auto & dep : deps) {
            if (dep.type == DepType::CurrentTime || dep.type == DepType::Exec) {
                debug("trace validation failed for %s dep: source='%s' key='%s' (always invalidates)",
                      depTypeString(dep.type), dep.source, dep.key);
                return false;
            }
            auto current = computeCurrentHashForStore(state, dep, inputAccessors);
            if (!current || *current != dep.expectedHash) {
                debug("trace validation failed for %s dep: source='%s' key='%s'",
                      depTypeString(dep.type), dep.source, dep.key);
                return false;
            }
        }

        validatedDepSets.insert(trace.depSetPath);
    }

    // Recursively validate parent
    if (trace.parent) {
        if (!validateTrace(*trace.parent, inputAccessors, state)) {
            debug("trace validation failed: parent trace invalid for '%s'",
                  store.printStorePath(tracePath));
            return false;
        }
    }

    validatedTraces.insert(tracePath);
    return true;
}

// ── Cold Path ────────────────────────────────────────────────────────

StorePath EvalCacheStore::coldStore(
    std::string_view attrPath,
    std::string_view name,
    const AttrValue & value,
    const std::vector<Dep> & directDeps,
    const std::optional<StorePath> & parentTracePath,
    bool isRoot)
{
    // 1. Filter deps: remove ParentContext (encoded via parent trace reference)
    std::vector<Dep> storedDeps;
    for (auto & dep : directDeps) {
        if (dep.type == DepType::ParentContext) continue;
        storedDeps.push_back(dep);
    }

    // 2. Sort+dedup ONCE (avoids redundant sort+dedup passes)
    auto sortedDeps = sortAndDedupDeps(storedDeps);

    // 3. Serialize dep set to zstd-compressed CBOR and store as standalone blob
    auto compressedDeps = serializeDepSet(sortedDeps);
    auto depSetPath = storeDepSet(compressedDeps);

    // 4. Serialize result trace (tiny, references dep set + parent)
    auto ctx = isRoot ? std::optional(contextHash) : std::nullopt;
    auto cbor = serializeEvalTrace(value, parentTracePath, ctx, depSetPath, symbols, store);

    // 5. Store result trace with references to parent + dep set
    StorePathSet refs{depSetPath};
    if (parentTracePath)
        refs.insert(*parentTracePath);
    auto tracePath = storeTrace("eval-" + sanitizeName(name), cbor, refs);

    // 6. Compute all recovery index hashes using pre-sorted deps (no redundant sort)
    auto depHash = computeDepContentHashFromSorted(sortedDeps);
    auto structHash = computeDepStructHashFromSorted(sortedDeps);

    std::optional<std::pair<Hash, StorePath>> parentDepHash;
    if (parentTracePath) {
        parentDepHash = {
            computeDepContentHashWithParentFromSorted(sortedDeps, *parentTracePath, store),
            *parentTracePath
        };
    }

    // 7. Single batched index write (1 lock acquisition, 3-4 statements).
    index.coldStoreIndex(contextHash, attrPath, tracePath, depHash,
                          parentDepHash, structHash, store);

    // 8. Session cache (skip volatile deps)
    bool hasVolatile = std::any_of(directDeps.begin(), directDeps.end(),
        [](auto & d) { return d.type == DepType::CurrentTime || d.type == DepType::Exec; });
    if (!hasVolatile) {
        validatedTraces.insert(tracePath);
        validatedDepSets.insert(depSetPath);
    }

    return tracePath;
}

// ── Warm Path ────────────────────────────────────────────────────────

std::optional<EvalCacheStore::WarmResult> EvalCacheStore::warmPath(
    std::string_view attrPath,
    const std::map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    const std::optional<StorePath> & parentTraceHint)
{
    // 1. Index lookup
    auto entry = index.lookup(contextHash, attrPath, store);
    if (!entry)
        return std::nullopt;

    // 2. Check store validity (GC may have removed them)
    if (!store.isValidPath(entry->tracePath))
        return std::nullopt;

    // 3. Validate trace (deps + parent chain)
    if (!validateTrace(entry->tracePath, inputAccessors, state)) {
        debug("warm path: trace validation failed for '%s', attempting recovery", attrPath);
        return recovery(entry->tracePath, attrPath, inputAccessors, state, parentTraceHint);
    }

    // 4. Read result from trace (tiny, ~200-500 bytes)
    try {
        auto trace = loadTrace(entry->tracePath);
        return WarmResult{std::move(trace.result), entry->tracePath};
    } catch (std::exception & e) {
        debug("failed to read cached result: %s", e.what());
        return std::nullopt;
    }
}

// ── Recovery helpers ──────────────────────────────────────────────────

/**
 * Try a single recovery candidate from DepHashRecovery.
 *
 * On success, updates the EvalIndex to point to the recovered trace so
 * subsequent warmPath calls skip recovery entirely (direct index hit).
 *
 * validateTrace recursively validates the candidate's parent chain.
 * For Phase 2 candidates, the embedded parent matches the recovered
 * parent (already in validatedTraces from the parent's own recovery),
 * so recursive validation is instant.
 */
std::optional<EvalCacheStore::WarmResult> EvalCacheStore::tryCandidate(
    const Hash & depHash,
    std::string_view attrPath,
    const std::map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    std::set<StorePath> & tried)
{
    auto candidatePath = index.lookupByDepHash(contextHash, attrPath, depHash, store);
    if (!candidatePath || !store.isValidPath(*candidatePath))
        return std::nullopt;
    // Avoid re-validating the same trace across phases
    if (tried.count(*candidatePath))
        return std::nullopt;
    tried.insert(*candidatePath);

    if (validateTrace(*candidatePath, inputAccessors, state)) {
        validatedTraces.insert(*candidatePath);
        // Update main index so next warmPath finds this trace directly
        index.upsert(contextHash, attrPath, *candidatePath, store);
        auto trace = loadTrace(*candidatePath);
        return WarmResult{std::move(trace.result), *candidatePath};
    }
    return std::nullopt;
}

// ── Recovery (three-phase) ───────────────────────────────────────────
//
// Three phases, tried in order:
//   Phase 1: depHash point lookup -- O(D + log N)
//     Load old trace's dep KEYS (from dep set blob), compute CURRENT hash
//     values, look up the resulting depHash in DepHashRecovery. Handles
//     same-structure reverts (e.g., env var changed and reverted back).
//
//   Phase 2: parent-aware depHash lookup -- O(D + log N)
//     Same as Phase 1 but with parent trace identity folded into the hash.
//     Fixes the "dep-less child clobbering" problem.
//
//   Phase 3: struct-group scan -- O(G*D + G*log N), G ~ 2-5
//     Scan DepStructGroups for all unique dep structures ever seen for
//     this (context_hash, attr_path). For each group, load the
//     representative trace's dep set blob, extract keys, compute current
//     hashes, then do a depHash point lookup.

std::optional<EvalCacheStore::WarmResult> EvalCacheStore::recovery(
    const StorePath & oldTracePath,
    std::string_view attrPath,
    const std::map<std::string, SourcePath> & inputAccessors,
    EvalState & state,
    const std::optional<StorePath> & parentTraceHint)
{
    std::set<StorePath> triedCandidates;

    // ── Phase 1: depHash point lookup ────────────────────────────
    bool hasVolatile = false;
    std::vector<Dep> newDeps;

    if (store.isValidPath(oldTracePath)) {
        // Load deps from old trace's dep set blob
        std::vector<Dep> oldDeps;
        try {
            oldDeps = loadDepsForTrace(oldTracePath);
        } catch (std::exception &) {
            // Dep set blob may have been GC'd
        }

        debug("recovery: Phase 1 for '%s' (recomputing %d dep hashes)",
              attrPath, oldDeps.size());

        bool allComputable = true;
        for (auto & dep : oldDeps) {
            if (dep.type == DepType::CurrentTime || dep.type == DepType::Exec) {
                hasVolatile = true;
                break;
            }
            auto current = computeCurrentHashForStore(state, dep, inputAccessors);
            if (!current) {
                allComputable = false;
                break;
            }
            newDeps.push_back({dep.source, dep.key, *current, dep.type});
        }

        if (hasVolatile) {
            debug("recovery: aborting for '%s' -- contains volatile dep", attrPath);
            return std::nullopt;
        }

        if (allComputable) {
            auto depHash = computeDepContentHash(newDeps);
            if (auto r = tryCandidate(depHash, attrPath, inputAccessors, state, triedCandidates)) {
                debug("recovery: Phase 1 succeeded for '%s'", attrPath);
                return r;
            }
        } else {
            newDeps.clear();
        }
    }

    // ── Phase 2: parent-aware depHash lookup ─────────────────────
    if (parentTraceHint) {
        if (!newDeps.empty()) {
            auto depHashP = computeDepContentHashWithParent(newDeps, *parentTraceHint, store);
            if (auto r = tryCandidate(depHashP, attrPath, inputAccessors, state, triedCandidates)) {
                debug("recovery: Phase 2 succeeded for '%s' (with deps)", attrPath);
                return r;
            }
        } else {
            auto depHashP = computeDepContentHashWithParent({}, *parentTraceHint, store);
            if (auto r = tryCandidate(depHashP, attrPath, inputAccessors, state, triedCandidates)) {
                debug("recovery: Phase 2 succeeded for '%s' (dep-less)", attrPath);
                return r;
            }
        }
    }

    // ── Phase 3: struct-group scan ───────────────────────────────
    auto groups = index.scanStructGroups(contextHash, attrPath, store);
    debug("recovery: Phase 3 for '%s' -- scanning %d struct groups", attrPath, groups.size());

    for (auto & group : groups) {
        if (!store.isValidPath(group.tracePath))
            continue;

        // Load deps from representative trace's dep set blob
        std::vector<Dep> repDeps;
        try {
            repDeps = loadDepsForTrace(group.tracePath);
        } catch (std::exception &) {
            continue; // dep set blob GC'd
        }

        std::vector<Dep> groupDeps;
        bool allGroupComputable = true;
        for (auto & dep : repDeps) {
            if (dep.type == DepType::CurrentTime || dep.type == DepType::Exec) {
                allGroupComputable = false;
                break;
            }
            auto current = computeCurrentHashForStore(state, dep, inputAccessors);
            if (!current) {
                allGroupComputable = false;
                break;
            }
            groupDeps.push_back({dep.source, dep.key, *current, dep.type});
        }
        if (!allGroupComputable)
            continue;

        // Try without parent (Phase 1-style key in DepHashRecovery)
        auto groupDepHash = computeDepContentHash(groupDeps);
        if (auto r = tryCandidate(groupDepHash, attrPath, inputAccessors, state, triedCandidates)) {
            debug("recovery: Phase 3 succeeded for '%s'", attrPath);
            return r;
        }

        // Also try with parent
        if (parentTraceHint) {
            auto groupDepHashP = computeDepContentHashWithParent(
                groupDeps, *parentTraceHint, store);
            if (auto r = tryCandidate(groupDepHashP, attrPath, inputAccessors, state, triedCandidates)) {
                debug("recovery: Phase 3 succeeded for '%s' (with parent)", attrPath);
                return r;
            }
        }
    }

    debug("recovery: all phases failed for '%s'", attrPath);
    return std::nullopt;
}

} // namespace nix::eval_cache
