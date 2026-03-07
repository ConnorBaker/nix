#pragma once
///@file

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/counter.hh"
#include "nix/util/position.hh"

namespace nix::eval_trace {
extern Counter nrDepTrackerScopes;
extern Counter nrOwnDepsTotal;
extern Counter nrOwnDepsMax;
} // namespace nix::eval_trace

#include <filesystem>
#include <vector>

#include <boost/unordered/unordered_flat_set.hpp>

namespace nix {

/**
 * RAII tracker that records dynamic dependencies during evaluation
 * (Adapton DDG builder, Shake-style dynamic dep discovery).
 *
 * Each tracker has its own dep vector (ownDeps). record() appends to the
 * active tracker's ownDeps AND to the shared epochLog. collectTraces()
 * returns std::move(ownDeps). Parent-child isolation is structural:
 * nested trackers have separate ownDeps vectors, eliminating the need
 * for range exclusion or filtration.
 *
 * The epochLog is a shared append-only vector used by the epoch map
 * (thunk memoization). Deps that outlive individual trackers (e.g.,
 * thunk deps replayed into later trackers) are referenced via epochLog
 * ranges. replayMemoizedDeps() copies from epochLog into the active
 * tracker's ownDeps.
 *
 * This is safe because evaluation is single-threaded.
 */
struct DependencyTracker {
    static thread_local DependencyTracker * activeTracker;
    /// Shared epoch log: deps that outlive individual trackers.
    /// Used by epochMap (thunk memoization) to reference deps across
    /// tracker lifetimes. record() appends here unconditionally;
    /// recordToEpochLog() appends here only (not to any tracker's ownDeps).
    static thread_local std::vector<Dep> epochLog;
    /// Nesting depth of live DependencyTracker instances on this thread.
    /// Only the first (depth 0 → 1) is a true root that should call
    /// onRootConstruction(). Trackers created inside a SuspendDepTracking
    /// scope see previous == nullptr but depth > 0, so they correctly
    /// skip the session cache reset.
    static thread_local uint32_t depth;

    InterningPools & pools;
    DependencyTracker * previous;
    /// Per-tracker dep vector: structurally isolated from other trackers.
    /// record() appends here when this tracker is active.
    /// collectTraces() returns std::move(ownDeps).
    std::vector<Dep> ownDeps;
    /// Index into epochLog at construction time. Used by
    /// replayMemoizedDeps() to detect deps already captured.
    uint32_t epochLogStartIndex;

    /// Deduplication set: prevents adding the same memoized dep range twice
    /// when a Value is re-forced. Pointer keys are safe because Values are
    /// long-lived within a single evaluation context. No ordering needed.
    /// boost::unordered_flat_set for better cache locality (open addressing).
    boost::unordered_flat_set<const Value *> replayedValues;

    /**
     * Hash-based dedup filter: stores uint64_t identity hashes. Both
     * record() and recordStructuredDep() check membership before appending.
     * >90% of recording attempts are duplicates, so hash-only dedup avoids
     * constructing DepKey strings entirely.
     *
     * tryInsert returns true if the hash was NOT previously seen.
     */
    struct DedupFilter {
        boost::unordered_flat_set<uint64_t> seen;
        bool tryInsert(uint64_t h) { return seen.insert(h).second; }
    };
    DedupFilter depDedup;

    explicit DependencyTracker(InterningPools & pools)
        : pools(pools)
        , previous(activeTracker)
        , epochLogStartIndex(epochLog.size())
    {
        activeTracker = this;
        if (depth++ == 0) onRootConstruction();
        eval_trace::nrDepTrackerScopes++;
    }

    /**
     * Called when a root DependencyTracker is constructed (depth 0→1).
     * Creates a RootTrackerScope containing all Lifetime 2 caches.
     */
    static void onRootConstruction();

    /**
     * Called when a root DependencyTracker is destroyed (depth 1→0).
     * Destroys the RootTrackerScope, clearing all Lifetime 2 caches.
     */
    static void onRootDestruction();

    ~DependencyTracker() {
        activeTracker = previous;
        if (--depth == 0) onRootDestruction();
    }

    DependencyTracker(const DependencyTracker &) = delete;
    DependencyTracker & operator=(const DependencyTracker &) = delete;

    /**
     * Record a dependency into the active tracker's ownDeps and the epochLog.
     * String-accepting overload: interns source and key via the provided pools.
     * Deduplicates by hashing (type, source, key) within the active tracker scope.
     * When no tracker is active (e.g. during SuspendDepTracking), skips dedup
     * and ownDeps append but still writes to epochLog (needed for epoch tracking).
     */
    static void record(InterningPools & pools, DepType type,
                       std::string_view source, std::string_view key,
                       DepHashValue hash);

    /**
     * Record a pre-interned dependency into the active tracker's ownDeps
     * and the epochLog. Deduplicates within the active tracker scope.
     */
    static void record(const Dep & dep);

    /**
     * Append a dependency to the epochLog only (not to any tracker's ownDeps).
     * String-accepting overload: interns source and key.
     * Used by TracedExpr::replayTrace() to propagate a child's cached deps
     * into the epoch log (needed for thunk epoch ranges) without adding
     * them to any tracker's dep set.
     */
    static void recordToEpochLog(InterningPools & pools, DepType type,
                                 std::string_view source, std::string_view key,
                                 const DepHashValue & hash);

    /**
     * Append a pre-interned dependency to the epochLog only.
     */
    static void recordToEpochLog(const Dep & dep);

    /**
     * Collect this tracker's deps. Returns std::move(ownDeps).
     * Replayed epoch deps are already copied into ownDeps by
     * replayMemoizedDeps().
     */
    std::vector<Dep> collectTraces() const;

    /**
     * Returns true if there is at least one active tracker.
     */
    static bool isActive() { return activeTracker != nullptr; }

    /**
     * Clear the epoch log. Called from resetFileCache().
     */
    static void clearEpochLog();
};

/**
 * Allocate a provenance record and return a Pos::ProvenanceRef for use
 * in PosTable origins. The record is stored in the given pools' ProvenanceTable.
 */
Pos::ProvenanceRef allocateProvenanceRef(
    InterningPools & pools, DepSourceId srcId, FilePathId fpId, DataPathId dpId, StructuredFormat format);

/**
 * Resolve a Pos::ProvenanceRef to its full ProvenanceRecord.
 * Requires an InterningPools reference.
 */
const ProvenanceRecord & resolveProvenanceRef(InterningPools & pools, const Pos::ProvenanceRef & ref);

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
 * Compute a BLAKE3 hash of a git repo's identity (HEAD rev + dirty state).
 * Returns std::nullopt if the repo has no commits yet.
 * May throw on git or filesystem errors — callers should catch as appropriate.
 */
std::optional<Blake3Hash> computeGitIdentityHash(const std::filesystem::path & repoRoot);

/**
 * Compute a BLAKE3 hash of a directory listing using streaming API.
 * Each entry is hashed as "name:typeInt;" where typeInt is the
 * numeric value of the optional file type (-1 if unknown).
 * The entries map is iterated in its natural (lexicographic) order.
 */
Blake3Hash depHashDirListing(const SourceAccessor::DirEntries & entries);

/**
 * Resolve an absolute path to an (inputName, relativePath) pair using
 * a mount-point-to-input mapping. Walks up the path trying each prefix.
 */
std::optional<std::pair<std::string, CanonPath>> resolveToInput(
    const CanonPath & absPath,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput);

/**
 * Record a file dependency, resolving to an input-relative path if possible.
 * In non-flake mode (mountToInput empty), records absolute paths with
 * inputName="". In flake mode, paths that can't be resolved to any input
 * are recorded with inputName="<absolute>" so they are validated directly
 * against the real filesystem.
 */
void recordDep(
    InterningPools & pools,
    const CanonPath & absPath,
    const DepHashValue & hash,
    DepType depType,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput,
    std::string_view storeName = {});

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
[[gnu::cold]] void maybeRecordRawContentDep(EvalState & state, const Value & v);

/**
 * Resolve an absolute path to a (source, key) pair for dep recording,
 * using the same resolution logic as recordDep. Helper for provenance
 * consumers that need to construct dep keys.
 */
std::pair<std::string, std::string> resolveProvenance(
    const CanonPath & absPath,
    const boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> & mountToInput);

/**
 * Provenance information for a container Value (attrset or list) produced
 * by ExprTracedData::eval(). Uses interned IDs matching the session pools.
 * Used by shape-observing builtins (length, attrNames, hasAttr) to record
 * shape deps (#len, #keys, #has:key).
 */
struct TracedContainerProvenance {
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;
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
ProvenanceRef allocateProvenance(DepSourceId sourceId, FilePathId filePathId,
                                 DataPathId dataPathId, StructuredFormat format);

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

// ═══════════════════════════════════════════════════════════════════════
// Component interning — zero-allocation dep key construction
// ═══════════════════════════════════════════════════════════════════════

/**
 * Compact interned representation of a StructuredContent dep key.
 * All fields are process-lifetime pool indices. Zero string allocation.
 * JSON dep key is constructed only for non-duplicate deps at serialization time.
 */
struct CompactDepComponents {
    DepSourceId sourceId;
    FilePathId filePathId;
    StructuredFormat format;
    DataPathId dataPathId;
    ShapeSuffix suffix;     ///< None/Len/Keys/Type
    Symbol hasKey;           ///< non-zero for #has: deps
};

/**
 * Convert a DataPath trie node to a JSON array string of path components.
 * Object keys become JSON strings; array indices become JSON numbers.
 * Only called for non-duplicate deps at serialization time.
 * Returns the serialized JSON array, e.g. '["nodes","nixpkgs",0]'.
 */
std::string dataPathToJsonString(InterningPools & pools, DataPathId nodeId);

/**
 * Convert a JSON array string of path components back to a DataPath trie node ID.
 * Used when replaying cached origins (trace-cache.cc).
 */
DataPathId jsonStringToDataPathId(InterningPools & pools, std::string_view jsonStr);

/**
 * Record a StructuredContent dep with zero-allocation dedup.
 * Hashes the compact integer components for dedup; only builds JSON
 * dep key string for non-duplicates (confirmed novel deps).
 * Returns true if the dep was recorded (not a duplicate).
 */
[[gnu::cold]] bool recordStructuredDep(
    InterningPools & pools,
    const CompactDepComponents & c,
    const DepHashValue & hash,
    DepType depType = DepType::StructuredContent);

/**
 * Precomputed keys hash from ExprTracedData::eval() Object case.
 * Stored in a thread-local side map keyed by PosTable origin offset (stable
 * from PosTable origins vector). At access time, maybeRecordAttrKeysDep compares
 * visible key count to stored count; if equal, uses the precomputed hash directly,
 * avoiding the sort + concat + BLAKE3 hash that dominates its runtime.
 * Uses interned IDs to avoid string allocation.
 */
struct PrecomputedKeysInfo {
    Blake3Hash hash;
    uint32_t keyCount;
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;
    StructuredFormat format;
};

/**
 * Register a precomputed keys hash for a TracedData origin.
 * Called from ExprTracedData::eval() after recording ImplicitShape #keys.
 * The key is the PosTable origin offset (stable across vector reallocation,
 * unlike raw Origin* pointers which are invalidated by vector growth).
 */
void registerPrecomputedKeys(uint32_t originOffset, PrecomputedKeysInfo info);

/**
 * Clear the precomputed keys map. Called on root DependencyTracker construction.
 */
void clearPrecomputedKeysMap();

/**
 * Record a #len StructuredContent dep if the list value came from
 * ExprTracedData (checked via traced container provenance map).
 * No-op if dep tracking is inactive or list is empty (no stable key).
 */
[[gnu::cold]] void maybeRecordListLenDep(const Value & v);

/**
 * Record a #keys StructuredContent dep if the attrset contains attrs
 * with TracedData provenance (checked via PosTable::originOf on Attr::pos).
 * Groups by origin — mixed-provenance attrsets get partial recording.
 * No-op if dep tracking is inactive or value is not an attrset.
 */
[[gnu::cold]] void maybeRecordAttrKeysDep(const PosTable & positions, const SymbolTable & symbols, const Value & v);

/**
 * Record per-key #has deps for intersectAttrs in bulk. Pre-computes TracedData
 * origins for each operand (one scan each), then iterates keys recording:
 * - exists=true for intersection keys (tag bit check per attr, no origin scan)
 * - exists=false for disjoint keys (against pre-computed origins only)
 * Skips operands with no TracedData entirely, reducing ~100K deps to ~55
 * in the typical callPackage pattern (50-key functionArgs vs 50K allPackages).
 */
[[gnu::cold]] void recordIntersectAttrsDeps(const PosTable & positions, const SymbolTable & symbols,
                                            const Value & left, const Value & right);

/**
 * Record a #has:key StructuredContent dep using PosIdx-based provenance.
 * For exists=true: checks the found attr's PosIdx origin — if TracedData,
 * records depHash("1"); if Nix-added (no TracedData origin), skips.
 * For exists=false: scans all attrs, records depHash("0") against each
 * unique TracedData origin.
 */
[[gnu::cold]] void maybeRecordHasKeyDep(const PosTable & positions, const SymbolTable & symbols,
                          const Value & v, Symbol keyName, bool exists);

/**
 * Record a #type StructuredContent dep if the value is a container.
 * For attrsets: uses PosIdx-based TracedData origin scanning.
 * For lists: uses the existing tracedContainerMap lookup.
 * No-op if dep tracking is inactive or value is not a container.
 */
[[gnu::cold]] void maybeRecordTypeDep(const PosTable & positions, const Value & v);

// ═══════════════════════════════════════════════════════════════════════
// NixBinding — per-binding .nix attrset tracking
// ═══════════════════════════════════════════════════════════════════════

struct Expr;
struct ExprAttrs;

/**
 * Registry entry for a single binding in a non-recursive ExprAttrs.
 * Keyed by PosIdx in the thread-local registry; looked up at attribute
 * access time by maybeRecordNixBindingDep.
 */
struct NixBindingEntry {
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;     ///< internChild(root, bindingName)
    Blake3Hash bindingHash;    ///< BLAKE3(scopeHash + name + kind + show(expr))
};

/**
 * Walk AST from root through lambda/with/let/update chains.
 * Returns (exprAttrs, scopeExprs) if an eligible non-recursive ExprAttrs
 * is found; (nullptr, {}) otherwise.
 */
std::pair<ExprAttrs *, std::vector<Expr *>> findNonRecExprAttrs(Expr * root);

/**
 * Compute a BLAKE3 hash capturing the enclosing scope structure.
 * Any change to the scope (lambda formals, let bindings, with expressions)
 * changes all binding hashes from that file.
 */
Blake3Hash computeNixScopeHash(const std::vector<Expr *> & scopeExprs, const SymbolTable & symbols);

/**
 * Compute a BLAKE3 hash for a single binding definition.
 * Hash = BLAKE3(scopeHash || '\0' || name || '\0' || kindTag || '\0' || show(expr)).
 *
 * @param exprToShow  The expression to serialize. For plain/inherited bindings
 *   this is def.e directly. For InheritedFrom bindings, callers MUST pass the
 *   resolved source expression from inheritFromExprs[displ] — NOT def.e,
 *   which is an ExprInheritFrom whose show() crashes (Symbol(0) underflow).
 *   Pass nullptr to omit the expression from the hash (conservative fallback).
 */
Blake3Hash computeNixBindingHash(
    const Blake3Hash & scopeHash,
    std::string_view name,
    int kindTag,
    const Expr * exprToShow,
    const SymbolTable & symbols);

/**
 * Register all bindings from an eligible ExprAttrs into the thread-local
 * registry. Called from ExprParseFile::eval at parse time.
 */
void registerNixBindings(ExprAttrs * exprAttrs,
                         const std::string & depSource, const std::string & depKey,
                         const Blake3Hash & scopeHash, const SymbolTable & symbols,
                         InterningPools & pools);

/**
 * Record a NixBinding StructuredContent dep if the given PosIdx is
 * registered in the thread-local registry. Called from ExprSelect::eval,
 * ExprOpHasAttr::eval, and prim_getAttr.
 */
[[gnu::cold]] void maybeRecordNixBindingDep(PosIdx pos);

/**
 * Clear the thread-local NixBinding registry.
 * Called on root DependencyTracker construction.
 */
void clearNixBindingRegistry();

} // namespace nix
