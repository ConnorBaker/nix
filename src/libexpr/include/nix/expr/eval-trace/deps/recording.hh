#pragma once
///@file
/// Core dependency tracking: DependencyTracker, SuspendDepTracking, and
/// provenance helpers. For specific dep recording categories, include:
///   - input-resolution.hh: file deps, readFile provenance, RawContent
///   - shape-recording.hh: StructuredContent deps (#len, #keys, #has:key, #type)
///   - nix-binding.hh: NixBinding per-binding .nix attrset tracking

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/position.hh"

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
    /// Only the first (depth 0 -> 1) is a true root that should call
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
     * Called when a root DependencyTracker is constructed (depth 0->1).
     * Creates a RootTrackerScope containing all Lifetime 2 caches.
     */
    static void onRootConstruction();

    /**
     * Called when a root DependencyTracker is destroyed (depth 1->0).
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

} // namespace nix
