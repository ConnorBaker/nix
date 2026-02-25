#pragma once
///@file

#include "nix/expr/eval-trace-deps.hh"
#include "nix/util/ref.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-path.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <bitset>
#include <map>
#include <unordered_map>

namespace nix {

class Value;

namespace eval_trace {
class TraceCache;
}

/**
 * Trace-aware evaluation context (BSàlC trace store + Adapton DDG support).
 *
 * Holds all state needed for incremental evaluation with dependency tracking.
 * Created eagerly in EvalState constructor when eval-trace is enabled;
 * nullptr otherwise (zero overhead).
 *
 * This separates the trace-specific fields from EvalState's core memoization
 * caches (fileTraceCache, importResolutionCache, etc.), making the boundary
 * between "evaluator-fundamental" and "trace-specific" state explicit.
 */
struct EvalTraceContext {
    /**
     * Registry of trace cache instances (BSàlC: verifying traces), keyed by
     * flake identity hash. Allows reuse of the same traced root value across
     * installables that share the same flake lock.
     */
    std::map<const Hash, ref<eval_trace::TraceCache>> evalCaches;

    /**
     * Cache of file content BLAKE3 hashes, keyed by SourcePath.
     * Populated during parseExprFromFile() and used by evalFile()
     * to record Content oracle deps (Adapton DDG edges) without
     * redundant re-hashing.
     */
    std::map<SourcePath, Blake3Hash> fileContentHashes;

    /**
     * Maps store mount points to (inputName, subdir) pairs.
     * Used to resolve absolute paths back to input-relative paths
     * for oracle dep recording (Adapton DDG). Populated by openTraceCache().
     */
    std::unordered_map<CanonPath, std::pair<std::string, std::string>> mountToInput;

    /**
     * Epoch-based memoized oracle deps from thunk/app evaluation, keyed by
     * Value address. Each entry records the [start, end) range in the
     * session-wide dep vector that was produced during the thunk's evaluation.
     * Used by replayMemoizedDeps() to propagate deps into active dependency
     * trackers (Adapton DDG: transitive dependency edges).
     */
    boost::unordered_flat_map<const Value *, DepRange> epochMap;

    /**
     * Bloom filter for fast rejection in replayMemoizedDeps().
     * A bit is set when a Value* is inserted into epochMap via recordThunkDeps.
     * replayMemoizedDeps tests the bit before doing the epochMap lookup —
     * ~95% of calls miss epochMap, so this avoids ~9M x 50ns flat_map lookups.
     * 2M bits = 256KB, giving ~5% false positive rate at typical load.
     */
    static constexpr size_t BLOOM_BITS = 1 << 21;
    std::bitset<BLOOM_BITS> replayBloom;

    void bloomSet(const Value * v) {
        auto h = reinterpret_cast<uintptr_t>(v) >> 4;
        replayBloom.set(h % BLOOM_BITS);
    }
    bool bloomTest(const Value * v) const {
        auto h = reinterpret_cast<uintptr_t>(v) >> 4;
        return replayBloom.test(h % BLOOM_BITS);
    }

    /**
     * Value pointer for which the next recordThunkDeps() call should be
     * skipped. Set by TracedExpr::eval() to prevent forceValue's post-eval
     * recordThunkDeps from creating an epoch map entry for TracedExpr
     * thunks, which would cause parent dep contamination via
     * replayMemoizedDeps(). Uses Value pointer (not a boolean flag)
     * so that sub-thunk recordThunkDeps calls within evaluateFresh()
     * are not incorrectly suppressed.
     */
    const Value * skipEpochRecordFor = nullptr;

    /**
     * Record that thunk/app evaluation of `v` produced oracle deps in
     * [epochStart, sessionTraces.size()). Called from forceValue after
     * thunk or app evaluation completes to populate the epoch map
     * (enables dep replay for subsequent forcing).
     */
    void recordThunkDeps(Value & v, uint32_t epochStart);

    /**
     * Replay memoized oracle deps for an already-forced Value into active
     * dependency trackers (Adapton: propagating transitive DDG edges).
     * Called when forceValue encounters a non-thunk, non-app Value with
     * an active DependencyTracker. Adds the value's epoch range to each
     * active tracker's replayedRanges (skipping trackers that already
     * include those deps in their session range).
     */
    void replayMemoizedDeps(const Value & v);

    /**
     * Clear all trace-specific state. Called from resetFileCache().
     */
    void reset();

    /**
     * Flush trace caches to persist SQLite WAL. Called before exec().
     */
    void flush();
};

} // namespace nix
