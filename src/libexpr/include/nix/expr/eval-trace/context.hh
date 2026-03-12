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
class Bindings;

namespace eval_trace {
class TraceCache;
class TraceStore;
struct TracedExpr;
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

    /// Identity of a sibling Value, stored by Value* address.
    ///
    /// Populated by two sources:
    ///   - installChildThunk: TracedExpr children from materializeResult
    ///     (tracedExpr is set, used for Tier 3 equality recovery)
    ///   - navigateToReal: real siblings at each navigation level
    ///     (tracedExpr is null, used for identity recovery only)
    ///
    /// Critical fields (parentExpr, canonicalSiblingIdx) are cached here at
    /// registration time so haveSameResolvedTarget Tier 1 and Tier 2 never
    /// dereference the TracedExpr pointer. Tier 3 dereferences tracedExpr
    /// but is best-effort (GC safety deferred to Stage 2).
    ///
    /// The maps use default allocator (mimalloc, GC-invisible). TracedExpr*
    /// may be GC-collected after thunk forcing. Tier 1/2 use cached fields
    /// only; Tier 3 is not GC-safe for navigateToReal entries (tracedExpr=null)
    /// or after GC collects installChildThunk entries.
    struct SiblingIdentity {
        eval_trace::TracedExpr * tracedExpr;
        eval_trace::TraceStore * traceStore;
        /// Cached from TracedExpr at registration time (GC-safe).
        eval_trace::TracedExpr * parentExpr;
        int16_t canonicalSiblingIdx;
        AttrPathId pathId;
        /// Bindings* from the original (pre-materialization) Value. Cached at
        /// TracedExpr::eval() completion from resolvedTarget->attrs(). Enables
        /// Tier 2 alias detection: two TracedExprs that evaluate to the same
        /// underlying attrset share the same originalBindings, even when
        /// detectAliases missed the alias (because thunks have different Value*
        /// before evaluation). The Bindings object is GC-allocated and kept
        /// alive by the original expression tree for the evaluation's lifetime.
        const Bindings * originalBindings = nullptr;
    };

    /// Maps Value* → SiblingIdentity for TracedExpr children (from
    /// installChildThunk) and real siblings (from navigateToReal).
    /// Value* is stable across thunk→materialized transitions (forceValue
    /// mutates in-place). Lifetime 3 (per-EvalState) so entries persist
    /// across nested root tracker scopes.
    ///
    /// Keyed by Value*; emplace means first-registration-wins for shared
    /// values. navigateToReal skips aliases of the navigation target to
    /// avoid registering the target under a sibling's pathId.
    boost::unordered_flat_map<const void *, SiblingIdentity> siblingIdentityMap;

    /// Secondary identity map: Bindings* → SiblingIdentity.
    /// Needed because ExprOpEq::eval creates stack-local Value copies via
    /// ExprSelect, and these copies have different Value* addresses than the
    /// originals in siblingIdentityMap. However, copies preserve the Bindings*
    /// pointer (it's a field in the Value struct), so this map enables lookup
    /// for Value copies. Populated in TracedExpr::eval() after materialization.
    boost::unordered_flat_map<const Bindings *, SiblingIdentity> bindingsIdentityMap;

    /// Callback invoked by replayMemoizedDeps when a registered sibling is
    /// detected. Returns true if the sibling access was recorded. Set by
    /// SiblingAccessTracker ctor, cleared by dtor.
    ///
    /// Takes const SiblingIdentity& (not TracedExpr*) so the callback uses
    /// cached fields and never dereferences the potentially-GC'd TracedExpr.
    using SiblingCallback = bool (*)(const SiblingIdentity &);
    SiblingCallback siblingCallback = nullptr;

    /**
     * Check whether two Values were produced by TracedExpr thunks that
     * resolve to the same underlying (non-materialized) Value.
     *
     * When eval-trace materialization creates fresh wrappers (Value and
     * Bindings pointers), pointer identity is lost for aliased values
     * (e.g., hostPlatform and targetPlatform pointing to the same platform
     * object). This method restores identity using a three-tier approach:
     *
     *   Tier 1: Same parent + same canonicalSiblingIdx → O(1), no rootLoader.
     *           Handles direct aliases like { a = platform; b = platform; }.
     *   Tier 2: Same originalBindings (pre-materialization Bindings*) → O(1).
     *           Handles cross-parent aliases where different thunks evaluate
     *           to the same attrset. Without real-tree contamination, this is
     *           reliable since Bindings* matches the real evaluation result.
     *   Tier 3: Navigate via getResolvedTarget → may trigger rootLoader.
     *           Best-effort fallback. GC safety of TracedExpr* deref is
     *           not guaranteed (deferred to Stage 2).
     *
     * Lookup uses siblingIdentityMap (by Value*) with fallback to
     * bindingsIdentityMap (by Bindings*) for Value copies created by
     * ExprSelect::eval (`v = *vAttrs`).
     */
    bool haveSameResolvedTarget(Value & v1, Value & v2);

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
