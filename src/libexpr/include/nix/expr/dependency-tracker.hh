#pragma once
///@file

#include "nix/expr/eval-trace-deps.hh"

#include <sys/types.h>

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nix {

/**
 * RAII tracker that records dynamic dependencies during evaluation
 * (Adapton DDG builder, Shake-style dynamic dep discovery).
 *
 * Analogous to Adapton's demanded computation graph (DCG) construction:
 * as evaluation forces thunks, the active tracker records each dependency
 * (file content, directory listing, env var, etc.) into a trace. Unlike
 * static build systems where deps are declared upfront, deps are discovered
 * dynamically during evaluation — a property shared with Shake (Mitchell,
 * ICFP 2012). The set of deps can vary between evaluations of the same
 * attribute depending on evaluation order and caching state.
 *
 * All deps are recorded into a single thread-local sessionTraces vector.
 * Each tracker records its startIndex at construction time. The range
 * [startIndex, sessionTraces.size()) represents deps recorded during
 * this tracker's lifetime. Deps from previously-evaluated thunks are
 * replayed via replayedRanges (epoch ranges from before this tracker
 * started). This replay is analogous to Adapton's change propagation:
 * when a cached thunk is served from its trace, its recorded deps are
 * propagated to the parent tracker.
 *
 * This is safe because evaluation is single-threaded.
 */
struct DependencyTracker {
    static thread_local DependencyTracker * activeTracker;
    static thread_local std::vector<Dep> sessionTraces;

    DependencyTracker * previous;
    std::vector<Dep> * mySessionTraces;
    uint32_t startIndex;
    /// Append-only list of dep ranges replayed from memoized thunks.
    /// Order preserved: ranges are appended in evaluation encounter order,
    /// then concatenated by collectTraces() to form the final dep vector.
    std::vector<DepRange> replayedRanges;

    /// Deduplication set: prevents adding the same memoized dep range twice
    /// when a Value is re-forced. Pointer keys are safe because Values are
    /// long-lived within a single evaluation context. No ordering needed.
    std::unordered_set<const Value *> replayedValues;

    /// Deduplication set for (type, source, key) triples within this tracker
    /// scope. record() checks membership before appending to sessionTraces.
    /// Hash-based for O(1) expected dedup checks; no ordering needed.
    std::unordered_set<DepKey, DepKey::Hash> recordedKeys;

    DependencyTracker()
        : previous(activeTracker)
        , mySessionTraces(&sessionTraces)
        , startIndex(mySessionTraces->size())
    {
        activeTracker = this;
        if (!previous) onRootConstruction();
    }

    /**
     * Called when a root DependencyTracker is constructed (no parent).
     * Clears per-evaluation-session state such as the traced container map.
     */
    static void onRootConstruction();
    ~DependencyTracker() { activeTracker = previous; }

    DependencyTracker(const DependencyTracker &) = delete;
    DependencyTracker & operator=(const DependencyTracker &) = delete;

    /**
     * Record a dependency into the session-wide dep vector.
     * Deduplicates by (type, source, key) within the active tracker scope.
     */
    static void record(const Dep & dep);

    /**
     * Collect all deps: session range [startIndex, current) plus
     * all replayed epoch ranges.
     */
    std::vector<Dep> collectTraces() const;

    /**
     * Returns true if there is at least one active tracker.
     */
    static bool isActive() { return activeTracker != nullptr; }

    /**
     * Clear the session-wide dep vector. Called from resetFileCache().
     */
    static void clearSessionTraces();
};

/**
 * RAII guard that temporarily suspends dep recording by setting
 * activeTracker to nullptr. On destruction, restores the previous tracker.
 *
 * Used in ExprOrigChild::eval() to prevent recording the parent's massive
 * file deps (e.g., 10K+ deps from evaluating all of nixpkgs). Nested
 * DependencyTrackers created within the suspended scope work correctly:
 * their constructors set activeTracker = this, and their destructors
 * restore nullptr (the suspended value).
 */
struct SuspendDepTracking {
    DependencyTracker * saved;

    SuspendDepTracking()
        : saved(DependencyTracker::activeTracker)
    {
        DependencyTracker::activeTracker = nullptr;
    }
    ~SuspendDepTracking() { DependencyTracker::activeTracker = saved; }

    SuspendDepTracking(const SuspendDepTracking &) = delete;
    SuspendDepTracking & operator=(const SuspendDepTracking &) = delete;
};

/**
 * Compute a BLAKE3 hash of data. Zero-allocation, returns stack-allocated Blake3Hash.
 */
Blake3Hash depHash(std::string_view data);

/**
 * Compute a BLAKE3 hash of a path's NAR serialization using streaming API.
 * Unlike depHash() which hashes raw file bytes, this captures the executable
 * bit via the NAR format. Used for builtins.path filtered file deps where
 * the resulting store path depends on permissions.
 */
Blake3Hash depHashPath(const SourcePath & path);

/**
 * Compute a BLAKE3 hash of a directory listing using streaming API.
 * Each entry is hashed as "name:typeInt;" where typeInt is the
 * numeric value of the optional file type (-1 if unknown).
 * The entries map is iterated in its natural (lexicographic) order.
 */
Blake3Hash depHashDirListing(const SourceAccessor::DirEntries & entries);

/**
 * Stat-cached Content hash: looks up the physical file's stat metadata in
 * the persistent stat-hash cache before falling back to depHash(readFile()).
 */
Blake3Hash depHashFile(const SourcePath & path);

/**
 * Stat-cached NARContent hash: like depHashPath() but checks stat cache first.
 */
Blake3Hash depHashPathCached(const SourcePath & path);

/**
 * Stat-cached Directory hash: like depHashDirListing() but checks stat cache
 * for the directory's own stat metadata first.
 */
Blake3Hash depHashDirListingCached(const SourcePath & path, const SourceAccessor::DirEntries & entries);

/**
 * Resolve an absolute path to an (inputName, relativePath) pair using
 * a mount-point-to-input mapping. Walks up the path trying each prefix.
 */
std::optional<std::pair<std::string, CanonPath>> resolveToInput(
    const CanonPath & absPath,
    const std::unordered_map<CanonPath, std::pair<std::string, std::string>> & mountToInput);

/**
 * Record a file dependency, resolving to an input-relative path if possible.
 * In non-flake mode (mountToInput empty), records absolute paths with
 * inputName="". In flake mode, paths that can't be resolved to any input
 * are recorded with inputName="<absolute>" so they are validated directly
 * against the real filesystem.
 */
void recordDep(
    const CanonPath & absPath,
    const DepHashValue & hash,
    DepType depType,
    const std::unordered_map<CanonPath, std::pair<std::string, std::string>> & mountToInput);

/**
 * Provenance information from a readFile call, used to connect
 * fromJSON/fromTOML to the file that was read. Thread-local, set by
 * prim_readFile and consumed by prim_fromJSON/prim_fromTOML.
 */
struct ReadFileProvenance {
    CanonPath path;
    Blake3Hash contentHash;
};

/**
 * Add read-file provenance keyed by content hash. Multiple readFile
 * results can coexist; fromJSON/fromTOML look up by content hash.
 */
void addReadFileProvenance(ReadFileProvenance prov);

/**
 * Look up read-file provenance by content hash. Returns a non-owning
 * pointer (nullptr if not found). Non-consuming: the same provenance
 * can serve multiple fromJSON/fromTOML calls on the same string.
 */
const ReadFileProvenance * lookupReadFileProvenance(const Blake3Hash & contentHash);

/**
 * Clear the thread-local read-file provenance map.
 * Called on root DependencyTracker construction.
 */
void clearReadFileProvenanceMap();

/**
 * Resolve an absolute path to a (source, key) pair for dep recording,
 * using the same resolution logic as recordDep. Helper for provenance
 * consumers that need to construct dep keys.
 */
std::pair<std::string, std::string> resolveProvenance(
    const CanonPath & absPath,
    const std::unordered_map<CanonPath, std::pair<std::string, std::string>> & mountToInput);

/**
 * Clear the in-memory (L1) stat-hash cache. Used by tests to force
 * re-hashing after modifying files.
 */
void clearStatHashMemoryCache();

/**
 * Provenance information for a container Value (attrset or list) produced
 * by ExprTracedData::eval(). Used by shape-observing builtins (length,
 * attrNames, hasAttr) to record shape deps (#len, #keys).
 */
struct TracedContainerProvenance {
    std::string depSource;
    std::string depKey;
    std::string dataPath;
    StructuredFormat format;
};

/**
 * Register a container in the thread-local provenance map.
 * Called by ExprTracedData::eval() for Object and Array nodes.
 *
 * The key is a stable internal pointer that survives Value copies:
 * - For attrsets: Bindings* (heap-allocated, shared across copies)
 * - For lists: first element Value* (heap-allocated, shared across copies)
 * Empty lists cannot be tracked (no stable internal pointer).
 */
void registerTracedContainer(const void * key, TracedContainerProvenance prov);

/**
 * Look up a container in the thread-local provenance map.
 * Returns nullptr if not found.
 */
const TracedContainerProvenance * lookupTracedContainer(const void * key);

/**
 * Clear the thread-local traced container provenance map.
 * Called on root DependencyTracker construction.
 */
void clearTracedContainerMap();

/**
 * Convert a directory entry type to its canonical string form.
 * Must be consistent between DirScalarNode::canonicalValue() (primops.cc)
 * and computeCurrentHash 'd' format handler (trace-store.cc).
 */
std::string dirEntryTypeString(std::optional<SourceAccessor::Type> type);

class SymbolTable;

/**
 * Record a #len StructuredContent dep if the list value came from
 * ExprTracedData (checked via traced container provenance map).
 * No-op if dep tracking is inactive or list is empty (no stable key).
 */
void maybeRecordListLenDep(const Value & v);

/**
 * Record a #keys StructuredContent dep if the attrset value came from
 * ExprTracedData (checked via traced container provenance map).
 * No-op if dep tracking is inactive or value is not an attrset.
 */
void maybeRecordAttrKeysDep(const SymbolTable & symbols, const Value & v);

/**
 * Record a #type StructuredContent dep if the value is a container
 * (attrset/list) that came from ExprTracedData. Records hash("object")
 * for attrsets, hash("array") for lists. Only records when type is
 * explicitly observed (typeOf, isAttrs, isList), NOT at materialization.
 * No-op if dep tracking is inactive or value is not a container.
 */
void maybeRecordTypeDep(const Value & v);

// ═══════════════════════════════════════════════════════════════════════
// Provenance propagation for container-reconstructing operations
// ═══════════════════════════════════════════════════════════════════════
//
// When builtins like mapAttrs, filter, sort, removeAttrs create new
// containers from tracked inputs, provenance must be propagated to
// the output so that subsequent shape observations (attrNames, length)
// can record shape deps. Without propagation, the output container's
// Bindings*/Value* is not in the provenance map, and shape deps are
// silently lost.
//
// Example: mapAttrs(f, trackedFromJSON) produces a new attrset with
// different Bindings*. Without propagation, attrNames on the result
// would fail to record a #keys dep, creating a soundness gap.

/**
 * Propagate attrset provenance from a single tracked input to the output.
 * Call after building the output attrset (mkAttrs).
 * No-op if dep tracking is inactive, input is not tracked, or output is empty.
 */
void propagateTrackedAttrs(const Value & output, const Value & input);

/**
 * Propagate list provenance from a single tracked input to the output.
 * Call after building the output list (mkList).
 * No-op if dep tracking is inactive, input is not tracked, or either is empty.
 */
void propagateTrackedList(const Value & output, const Value & input);

/**
 * Propagate attrset provenance from the first tracked input found.
 * Used for operations like // and intersectAttrs where either input
 * could be tracked. Copies provenance from whichever input is found first.
 */
void propagateTrackedAttrsAny(const Value & output, std::initializer_list<const Value *> inputs);

/**
 * Propagate list provenance from the first tracked input found.
 * Used for concatLists where any input list could be tracked.
 */
void propagateTrackedListFromAny(const Value & output, size_t nInputs, Value * const * inputs);

/**
 * Stat metadata used as a cache key: if these fields match, the file
 * hasn't changed and the cached hash is still valid. Field types match
 * the POSIX stat struct. Used as the L1 cache key in StatHashCache and
 * embedded in StatHashEntry for the L2/SQLite round-trip.
 */
struct StatHashKey {
    dev_t dev;
    ino_t ino;
    time_t mtime_sec;
    int64_t mtime_nsec;
    off_t size;
    DepType depType;

    bool operator==(const StatHashKey &) const = default;

    struct Hash {
        std::size_t operator()(const StatHashKey & k) const noexcept
        {
            return hashValues(k.dev, k.ino, k.mtime_sec, k.mtime_nsec, k.size, std::to_underlying(k.depType));
        }
    };
};

/**
 * Entry for bulk-loading/flushing the stat-hash cache between
 * TraceStore (SQLite owner) and the in-memory StatHashCache singleton.
 * The int64_t cast for SQLite binding happens at the TraceStore boundary.
 */
struct StatHashEntry {
    std::string path;
    StatHashKey stat;
    Blake3Hash hash;
};

/**
 * Bulk-load entries from TraceStore's SQLite into the in-memory
 * StatHashCache L2 map. Called once during TraceStore construction.
 */
void loadStatHashEntries(std::vector<StatHashEntry> entries);

/**
 * Iterate dirty (newly-stored) stat-hash entries for TraceStore to
 * flush back to its SQLite StatHashCache table during destruction.
 */
void forEachDirtyStatHash(std::function<void(const StatHashEntry &)> callback);

} // namespace nix
