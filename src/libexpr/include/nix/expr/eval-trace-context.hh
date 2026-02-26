#pragma once
///@file

#include "nix/expr/eval-trace-deps.hh"
#include "nix/util/ref.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-path.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <bitset>
#include <cstring>
#include <map>
#include <memory>
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
     * replayMemoizedDeps tests the bit before doing the epochMap lookup.
     *
     * Sizing (k=2 hash functions, m=8M bits = 1MB):
     *
     *   Profiled on `nix eval -f nixos/release.nix closures --json`:
     *     n=654K insertions, 494M tests → 1.3% FP, 97.4% rejection rate
     *   Profiled on `nix eval -f nixos/release.nix closures.gnome --json`:
     *     n=174K insertions, 106M tests → 0.07% FP, 98.5% rejection rate
     *
     *   Saves ~1s on the heavy workload (replayMemoizedDeps drops from
     *   2270ms to 1258ms by avoiding 480M+ epochMap lookups).
     *
     *   Smaller sizes were tested and rejected:
     *     256KB (1<<21): 8.5% FP at 654K insertions (46% load), 42M wasted lookups
     *      32KB (1<<18): 16.6% FP at 174K insertions (66% load), catastrophically overloaded
     *
     * Hash functions: h1 = ptr >> 4 (strips 16-byte GC alignment),
     * h2 = (ptr >> 4) * phi64 >> 43 (golden-ratio multiplicative hash
     * for an independent bit position).
     *
     * Note: Boost.Bloom (boost::bloom::filter with fast_multiblock32) was
     * benchmarked as a replacement but was 1.9x slower for replayMemoizedDeps
     * (3.48% vs 2.06% self time, 13151 vs 11563 total samples). The SIMD
     * block-oriented layout is optimized for batch throughput, but our access
     * pattern is single scattered probes interleaved with other work, where
     * the custom shift+multiply hash with direct byte-level bit test wins.
     */
    static constexpr size_t BLOOM_BITS = 1 << 23;
    static constexpr size_t BLOOM_BYTES = BLOOM_BITS / 8;

    /**
     * GC-invisible bloom storage. Allocated with malloc (not GC_MALLOC)
     * so Boehm never scans the 1MB bitset for pointers. Contains only
     * hashed bit positions — no pointer-like data to cause false retention.
     */
    struct BloomStorage {
        uint8_t * data;
        BloomStorage() : data(static_cast<uint8_t *>(::malloc(BLOOM_BYTES))) {
            std::memset(data, 0, BLOOM_BYTES);
        }
        ~BloomStorage() { ::free(data); }
        BloomStorage(const BloomStorage &) = delete;
        BloomStorage & operator=(const BloomStorage &) = delete;

        void set(size_t bit) { data[bit / 8] |= (1u << (bit % 8)); }
        bool test(size_t bit) const { return data[bit / 8] & (1u << (bit % 8)); }
        void reset() { std::memset(data, 0, BLOOM_BYTES); }
    } replayBloom;

    void bloomSet(const Value * v) {
        auto h = reinterpret_cast<uintptr_t>(v);
        replayBloom.set((h >> 4) % BLOOM_BITS);
        replayBloom.set(((h >> 4) * 0x9E3779B97F4A7C15ULL >> 43) % BLOOM_BITS);
    }
    bool bloomTest(const Value * v) const {
        auto h = reinterpret_cast<uintptr_t>(v);
        return replayBloom.test((h >> 4) % BLOOM_BITS)
            && replayBloom.test(((h >> 4) * 0x9E3779B97F4A7C15ULL >> 43) % BLOOM_BITS);
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
