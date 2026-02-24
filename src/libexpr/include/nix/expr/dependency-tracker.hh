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
    /// then appended by collectTraces() to form the final dep vector
    /// (filtering out ranges fully within excludedChildRanges).
    std::vector<DepRange> replayedRanges;

    /// Deduplication set: prevents adding the same memoized dep range twice
    /// when a Value is re-forced. Pointer keys are safe because Values are
    /// long-lived within a single evaluation context. No ordering needed.
    std::unordered_set<const Value *> replayedValues;

    /// Ranges in sessionTraces belonging to child TracedExpr evaluations.
    /// Excluded from this tracker's collectTraces() output to prevent
    /// parent traces from inheriting children's dependencies. Each
    /// TracedExpr manages its own trace; the parent references children
    /// via ParentContext deps.
    std::vector<std::pair<uint32_t, uint32_t>> excludedChildRanges;

    void excludeChildRange(uint32_t start, uint32_t end) {
        if (start < end)
            excludedChildRanges.emplace_back(start, end);
    }

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
     * replayed epoch ranges, skipping any regions in excludedChildRanges.
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
 * Register a readFile result's string data pointer for RawContent tracking.
 * Called by prim_readFile after mkString. Maps the Value's c_str() pointer
 * to the content hash already computed by readFile.
 */
void addReadFileStringPtr(const char * ptr, const Blake3Hash & contentHash);

/**
 * Clear the thread-local readFile string pointer map.
 * Called on root DependencyTracker construction.
 */
void clearReadFileStringPtrs();

class EvalState;

/**
 * Record a RawContent dep if the string value came from readFile.
 * Checks the readFileStringPtrs map by pointer identity (O(1)).
 * Called by string builtins that observe raw bytes (stringLength,
 * hashString, substring, match, split, replaceStrings) and eqValues.
 * No-op if dep tracking is inactive or the string didn't come from readFile.
 */
void maybeRecordRawContentDep(EvalState & state, const Value & v);

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
 * attrNames, hasAttr) to record shape deps (#len, #keys, #has:key).
 */
struct TracedContainerProvenance {
    std::string depSource;
    std::string depKey;
    std::string dataPath;
    StructuredFormat format;
};

/**
 * Non-owning pointer to a GC-stable TracedContainerProvenance.
 * Used for list provenance tracking (lists still use the container map).
 */
using ProvenanceRef = const TracedContainerProvenance *;

/**
 * Allocate a TracedContainerProvenance in a stable, non-GC thread-local pool.
 * Used for list provenance. Returns a pointer valid until the next root
 * DependencyTracker construction (which clears the pool).
 */
ProvenanceRef allocateProvenance(std::string depSource, std::string depKey,
                                 std::string dataPath, StructuredFormat format);

/**
 * Register a list container in the thread-local provenance map.
 * The key is the first element Value* (heap-allocated, stable across copies).
 * Empty lists cannot be tracked (no stable internal pointer).
 */
void registerTracedContainer(const void * key, const TracedContainerProvenance * prov);

/**
 * Look up a list container's provenance in the thread-local provenance map.
 * Returns nullptr if not found. For lists only (attrsets use PosIdx).
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

class PosTable;
class SymbolTable;
class Symbol;

/**
 * Record a #len StructuredContent dep if the list value came from
 * ExprTracedData (checked via traced container provenance map).
 * No-op if dep tracking is inactive or list is empty (no stable key).
 */
void maybeRecordListLenDep(const Value & v);

/**
 * Record a #keys StructuredContent dep if the attrset contains attrs
 * with TracedData provenance (checked via PosTable::originOf on Attr::pos).
 * Groups by origin — mixed-provenance attrsets get partial recording.
 * No-op if dep tracking is inactive or value is not an attrset.
 */
void maybeRecordAttrKeysDep(const PosTable & positions, const SymbolTable & symbols, const Value & v);

/**
 * Record a #has:key StructuredContent dep using PosIdx-based provenance.
 * For exists=true: checks the found attr's PosIdx origin — if TracedData,
 * records depHash("1"); if Nix-added (no TracedData origin), skips.
 * For exists=false: scans all attrs, records depHash("0") against each
 * unique TracedData origin.
 */
void maybeRecordHasKeyDep(const PosTable & positions, const SymbolTable & symbols,
                          const Value & v, Symbol keyName, bool exists);

/**
 * Record a #type StructuredContent dep if the value is a container.
 * For attrsets: uses PosIdx-based TracedData origin scanning.
 * For lists: uses the existing tracedContainerMap lookup.
 * No-op if dep tracking is inactive or value is not a container.
 */
void maybeRecordTypeDep(const PosTable & positions, const Value & v);

// ═══════════════════════════════════════════════════════════════════════
// Provenance propagation for list-reconstructing operations
// ═══════════════════════════════════════════════════════════════════════
//
// Attrset provenance uses PosIdx (TracedData origin per Attr) and needs
// no propagation — PosIdx survives attrset operations (//, mapAttrs,
// removeAttrs) because Attr objects are copied with their pos field.
//
// List provenance still uses the container map (first-element Value*
// → provenance) and requires explicit propagation.

/**
 * Propagate list provenance from a single tracked input to the output.
 * Call after building the output list (mkList).
 * No-op if dep tracking is inactive, input is not tracked, or either is empty.
 * When shapeModifying is true, sets shapeModified flag on the output provenance
 * if the output size differs from the input size.
 */
void propagateTrackedList(const Value & output, const Value & input, bool shapeModifying = false);

/**
 * Propagate list provenance from the first tracked input found.
 * Used for concatLists where any input list could be tracked.
 * Sets shapeModified when output size differs from any input's size.
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
