#pragma once
///@file

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/store/attr-vocab-store.hh"
#include "nix/util/pointer-bloom-filter.hh"
#include "nix/util/ref.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-path.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <memory>

namespace nix {

struct Value;

namespace eval_trace {
class TraceCache;
}

/**
 * Trace-aware evaluation context (BSàlC trace store + Adapton DDG support).
 * [Lifetime 3: per-EvalState — owned as std::unique_ptr member of EvalState]
 *
 * Holds all state needed for incremental evaluation with dependency tracking.
 * Created eagerly in EvalState constructor when eval-trace is enabled;
 * nullptr otherwise (zero overhead). Destroyed with the EvalState.
 *
 * See recording.cc for the full lifetime documentation covering
 * all three scopes: (1) thread-local recording state, (2) per-root-tracker
 * caches, (3) per-EvalState context (this struct).
 */
struct EvalTraceContext {
    /// Interning pools for dep recording. Owned here so each EvalState
    /// gets its own pools, providing automatic test isolation.
    std::unique_ptr<InterningPools> pools{std::make_unique<InterningPools>()};

    /// Structured attr name/path vocabulary (shared across evaluations).
    /// Constructed lazily when first needed (requires SymbolTable).
    std::unique_ptr<eval_trace::AttrVocabStore> vocabStore;

    /// Get or create the vocab store. Must pass the EvalState's SymbolTable.
    eval_trace::AttrVocabStore & getVocabStore(SymbolTable & syms) {
        if (!vocabStore)
            vocabStore = std::make_unique<eval_trace::AttrVocabStore>(syms);
        return *vocabStore;
    }
    /**
     * Registry of trace cache instances (BSàlC: verifying traces), keyed by
     * flake identity hash. Allows reuse of the same traced root value across
     * installables that share the same flake lock.
     */
    boost::unordered_flat_map<Hash, ref<eval_trace::TraceCache>> evalCaches;

    /**
     * Cache of file content BLAKE3 hashes, keyed by SourcePath.
     * Populated during parseExprFromFile() and used by evalFile()
     * to record Content oracle deps (Adapton DDG edges) without
     * redundant re-hashing.
     */
    boost::unordered_flat_map<SourcePath, Blake3Hash> fileContentHashes;

    /**
     * Maps store mount points to (inputName, subdir) pairs.
     * Used to resolve absolute paths back to input-relative paths
     * for oracle dep recording (Adapton DDG). Populated by openTraceCache().
     */
    boost::unordered_flat_map<CanonPath, std::pair<std::string, std::string>> mountToInput;

    /**
     * Epoch-based memoized oracle deps from thunk/app evaluation, keyed by
     * Value address. Each entry records the [start, end) range in the
     * epochLog that was produced during the thunk's evaluation.
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
     * Note: Boost.Bloom (boost::bloom::filter with fast_multiblock32) was
     * benchmarked as a replacement but was 1.9x slower for replayMemoizedDeps
     * (3.48% vs 2.06% self time, 13151 vs 11563 total samples). The SIMD
     * block-oriented layout is optimized for batch throughput, but our access
     * pattern is single scattered probes interleaved with other work, where
     * the custom shift+multiply hash with direct byte-level bit test wins.
     */
    PointerBloomFilter<1 << 23, 16> replayBloom;

    // ── Sibling identity tracking (for already-materialized sibling detection) ──

    /// Identity of a TracedExpr child thunk, stored by Value* address.
    /// Uses void* to avoid depending on TracedExpr (defined in trace-cache.cc).
    struct SiblingIdentity {
        void * tracedExpr;  // TracedExpr*
        void * traceStore;  // TraceStore*
    };

    /// Maps Value* → SiblingIdentity for all TracedExpr child thunks created
    /// during materialization. Value* is stable across thunk→materialized
    /// transitions (forceValue mutates in-place). Lifetime 3 (per-EvalState)
    /// so entries persist across nested root tracker scopes.
    boost::unordered_flat_map<const void *, SiblingIdentity> siblingIdentityMap;

    /// Callback invoked by replayMemoizedDeps when a registered sibling is
    /// detected. Returns true if the sibling access was recorded. Set by
    /// SiblingAccessTracker ctor, cleared by dtor.
    using SiblingCallback = bool (*)(void * tracedExpr, void * traceStore);
    SiblingCallback siblingCallback = nullptr;

    /**
     * Record that thunk/app evaluation of `v` produced oracle deps in
     * [epochStart, epochLog.size()). Called from forceValue after
     * thunk or app evaluation completes to populate the epoch map
     * (enables dep replay for subsequent forcing).
     */
    void recordThunkDeps(const Value & v, uint32_t epochStart);

    /**
     * Replay memoized oracle deps for an already-forced Value into the
     * active tracker's ownDeps (Adapton: propagating transitive DDG edges).
     * Called when forceValue encounters a non-thunk, non-app Value with
     * an active DependencyTracker. Copies deps from the epochLog range
     * into the tracker's ownDeps (skipping if the range was recorded
     * during this tracker's lifetime).
     */
    void replayMemoizedDeps(const Value & v);

    /**
     * Clear all trace-specific state. Called from resetFileCache().
     */
    void reset();

    /**
     * Flush and commit trace caches atomically. Called before exec().
     * TraceStore destructors flush all pending data (including vocab
     * entries via ATTACH'd connection) and commit the single cross-DB
     * transaction. No separate vocab coordination needed.
     */
    void flush();
};

} // namespace nix
