#pragma once
///@file

#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-trace/eval-context.hh"
#include "nix/util/guarded.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/memo-replay-store.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/semantic-analyzer.hh"
#include "nix/expr/eval-trace/store/attr-vocab-store.hh"
#include "nix/util/ref.hh"
#include "nix/util/hash.hh"
#include "nix/util/source-path.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace nix {

struct Value;
class Bindings;
class EvalState;
struct TraceRuntime;

namespace eval_trace::test {
struct TraceRuntimeTestAccess;
}

namespace eval_trace {
class TraceSession;
struct SqliteTraceStorage;
class TraceBackend;
struct TracedExpr;
struct SiblingReplayCaptureScope;
}

namespace eval_trace {

/**
 * Sibling capture scope for navigated child evaluation.
 *
 * Scoped around a child's fresh evaluation to capture sibling accesses under a
 * single parent slot. Every captured child keeps an explicit
 * `ParentSlot` dep for parent-shape invalidation, while sibling forces
 * collapse to compact `ValueContext` deps instead of copying the
 * sibling's full dep set into the child.
 */
struct SiblingReplayCaptureScope
{
private:
    struct CapturedAccess
    {
        ValueContext pathContext{};
        TraceHash traceHash{};
    };

    ParentSlot parentSlot;
    /// Pre-computed at construction time (ctx available). Eliminates
    /// the need to call getCurrentTraceHash during appendDeps.
    std::optional<TraceHash> parentTraceHash_;
    /// Carried ctx for getCurrentTraceHash calls from replayMemoizedDeps.
    /// RAII-scoped: valid for the scope's lifetime, which is exactly the
    /// duration of evaluateResolvedTarget.
    EvalContext<Suspendable> & ctx_;
    ValueContext selfContext;
    std::vector<CapturedAccess> accesses;
    boost::unordered_flat_set<ValueContext, ValueContext::Hash> seen;

    static thread_local SiblingReplayCaptureScope * current_;
    SiblingReplayCaptureScope * previous;

    /// Snapshot/restore of the thread-local for FiberThreadLocals.
    static SiblingReplayCaptureScope * snapshotPointer() noexcept { return current_; }
    static void restorePointer(SiblingReplayCaptureScope * s) noexcept { current_ = s; }

public:
    explicit SiblingReplayCaptureScope(
        ParentSlot parentSlot,
        ValueContext selfContext,
        TraceBackend & backend,
        EvalContext<Suspendable> & ctx);
    ~SiblingReplayCaptureScope();

    SiblingReplayCaptureScope(const SiblingReplayCaptureScope &) = delete;
    SiblingReplayCaptureScope & operator=(const SiblingReplayCaptureScope &) = delete;

    void appendDeps(std::vector<Dep> & deps) const;
    static bool shouldCapture(ParentSlot parentSlot, ValueContext pathContext);
    /// Get the Suspendable context carried by the active scope.
    EvalContext<Suspendable> & ctx() const { return ctx_; }

    /// Innermost active scope on this thread, or nullptr. Read-only;
    /// mirrors SuspendableCtxScope::innermost().
    static SiblingReplayCaptureScope * innermost() noexcept { return current_; }

private:
    friend struct ::nix::TraceRuntime;
    friend void appendActiveReplayCaptureDeps(std::vector<Dep> & deps);
    /// FiberThreadLocals snapshot/restore — narrow grants, one read + one write.
    friend SiblingReplayCaptureScope * snapshotReplayCaptureScope();
    friend void restoreReplayCaptureScope(SiblingReplayCaptureScope *);

    static bool maybeCapture(
        ParentSlot parentSlot,
        ValueContext pathContext,
        std::optional<TraceHash> traceHash);
    void appendRecordedValueContextDeps(std::vector<Dep> & deps) const;
    void recordAccess(
        ValueContext pathContext,
        const TraceHash & traceHash);
};

void appendActiveReplayCaptureDeps(std::vector<Dep> & deps);

/// Snapshot/restore the SiblingReplayCaptureScope thread-local, used
/// only by FiberThreadLocals for structured fiber context save/restore.
SiblingReplayCaptureScope * snapshotReplayCaptureScope();
void restoreReplayCaptureScope(SiblingReplayCaptureScope * scope);

} // namespace eval_trace

struct PublishedMaterializedIdentity {
    struct ValueIdentitySnapshot {
        bool isTracedProducer = false;
        std::optional<SiblingIdentity> siblingIdentity;
        ValueContext valueContext;
        std::optional<ValueIdentityStamp> valueIdentityStamp;
        eval_trace::TraceBackend * traceBackend = nullptr;
    };

    const Value * key = nullptr;
    const Bindings * bindings = nullptr;
    Value * const * listBacking = nullptr;
    std::optional<ValueIdentityStamp> stamp;
    std::optional<ValueIdentitySnapshot> previousValueIdentity;
    std::optional<ValueIdentitySnapshot> previousStampedIdentity;
    std::optional<ValueIdentityStamp> previousBindingsStamp;
    std::optional<ValueIdentityStamp> previousListBackingStamp;
};

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
struct TraceRuntime {
private:
    /// Structured attr name/path vocabulary (shared across evaluations).
    /// Constructed lazily when first needed (requires SymbolTable).
    std::unique_ptr<eval_trace::AttrVocabStore> vocabStore;

public:
    // ── Sibling identity tracking (for already-materialized sibling detection) ──

private:
    /// Identity of a sibling Value, stored by Value* address.
    ///
    /// Populated by two sources:
    ///   - traced child installation during materialization
    ///   - real-sibling navigation during fresh evaluation
    ///
    /// The identity logic uses only owned metadata (siblingIdentity,
    /// valueContext, valueIdentityStamp). Whether the identity came from a
    /// traced child is tracked as a boolean so sibling-force isolation does
    /// not need to store a GC-sensitive TracedExpr pointer.
    struct ValueIdentity {
    private:
        friend struct TraceRuntime;

        ValueIdentity(
            bool isTracedProducer,
            eval_trace::TraceBackend * traceBackend,
            std::optional<SiblingIdentity> siblingIdentity,
            ValueContext valueContext,
            std::optional<ValueIdentityStamp> valueIdentityStamp);

        bool isTracedProducer = false;
        eval_trace::TraceBackend * traceBackend;
        /// Cached sibling identity from the producing child, when applicable (GC-safe).
        std::optional<SiblingIdentity> siblingIdentity;
        ValueContext valueContext;
        std::optional<ValueIdentityStamp> valueIdentityStamp;
    };

    struct ListIdentityStamp {
        ParentSlot parentSlot;
        uint32_t canonicalSiblingIdx = invalidSiblingIndex;

        bool operator==(const ListIdentityStamp &) const = default;

        struct Hash {
            size_t operator()(const ListIdentityStamp & stamp) const noexcept
            {
                size_t seed = std::hash<uint32_t>{}(stamp.parentSlot.value.value);
                auto idxHash = std::hash<uint32_t>{}(stamp.canonicalSiblingIdx);
                seed ^= idxHash + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                return seed;
            }
        };
    };

    std::unique_ptr<InterningPools> pools{std::make_unique<InterningPools>()};
    boost::unordered_flat_map<SourcePath, DepHash> fileContentHashes;

    eval_trace::NixSemanticAnalyzer semanticAnalyzer;
    eval_trace::MemoReplayStore replayStore;
    /// Identity maps — lock-free reads, locked writes.
    ///
    /// Reads are lock-free because: (1) evaluation is single-threaded
    /// (cooperative scheduling on one carrier thread), (2) read-only
    /// flat_map::find does not allocate, so no GC reentrancy.
    ///
    /// Writes use recursive_mutex because: insert may trigger rehash →
    /// allocation → Boehm GC runs gc_cleanup finalizers on the allocating
    /// thread → finalizer chains (~TracedExpr → ~TraceSession → members)
    /// can trigger allocations that re-enter the same lock.
    ///
    /// Maps with GC-allocated pointer keys (Value*, Value*const*) use
    /// traceable_allocator so the GC can scan for pointers (session 83 SEGV).
    ///
    /// Use readMap() for lock-free reads, withLock() for writes.
    template<typename Map>
    struct ReadOptimizedMap {
        Map map;
        mutable std::recursive_mutex mutex;

        /// Lock-free read access. Safe because evaluation is single-threaded
        /// and reads don't allocate (no GC reentrancy). Do NOT insert,
        /// erase, or rehash through this reference.
        const Map & readMap() const { return map; }

        /// Locked write access. Required for insert/emplace/erase which
        /// may allocate and trigger GC finalizer reentrancy.
        template<typename F>
        decltype(auto) withLock(F && f) {
            std::lock_guard guard(mutex);
            return std::forward<F>(f)(map);
        }

        void clear() { map.clear(); }
    };

    ReadOptimizedMap<boost::unordered_flat_map<
        const Value *, ValueIdentity,
        boost::hash<const Value *>, std::equal_to<const Value *>,
        traceable_allocator<std::pair<const Value * const, ValueIdentity>>>>
        valueIdentityMap;
    /// Dense vector indexed by ValueIdentityStamp::value (monotonic uint32).
    /// No mutex needed: evaluation is single-threaded, and the vector does
    /// not use traceable_allocator (ValueIdentity has no GC pointers), so
    /// resize cannot trigger GC finalizer reentrancy.
    std::vector<std::optional<ValueIdentity>> stampedValueIdentityVec;
    ReadOptimizedMap<boost::unordered_flat_map<
        ListIdentityStamp, ValueIdentityStamp, ListIdentityStamp::Hash>>
        listValueIdentityStampMap;
    ReadOptimizedMap<boost::unordered_flat_map<
        Value * const *, ValueIdentityStamp,
        boost::hash<Value * const *>, std::equal_to<Value * const *>,
        traceable_allocator<std::pair<Value * const * const, ValueIdentityStamp>>>>
        listBackingValueIdentityStampMap;
    uint32_t nextValueIdentityStamp = 1;

public:
    TraceRuntime();

    uint32_t currentReplayEpochSize() const { return replayStore.epochSize(); }
    void rollbackReplayEpoch(uint32_t epochStart);
    std::unique_ptr<eval_trace::TraceBackend> makeTraceBackend(
        const Hash & fingerprint, SymbolTable & symbols);
    /// Clear the file-content hash cache.  Called by test fixtures to
    /// simulate a new session (a real new process would start with an
    /// empty cache).  NOT thread-safe — only call from the evaluation
    /// thread.
    void clearFileContentHashes() { fileContentHashes.clear(); }

private:
    // ── Test-only accessors (reached via TraceRuntimeTestAccess) ─────
    // Exposed to test code through the friend struct in
    // src/libexpr-tests/eval-trace/trace-runtime-test-access.hh.  Production
    // code has no path to these names.
    std::vector<Dep> & epochLog_ForTest() { return replayStore.epochLog_; }
    std::optional<DepRange> lookupReplayRange_ForTest(const Value & v) const;
    bool hasReplayEntries_ForTest() const;
    void clearReplayEntries_ForTest();
    void registerValueIdentity_ForTest(const Value * key, AttrPathId pathId = AttrPathId());
    bool hasValueIdentity_ForTest(const Value * key) const;
    bool hasBindingsValueIdentity_ForTest(const Bindings * key) const;
    void reset_ForTest() { reset(); }
    void recordThunkDeps_ForTest(const Value & v, uint32_t epochStart) { recordThunkDeps(v, epochStart); }
    void replayMemoizedDeps_ForTest(const Value & v) { replayMemoizedDeps(v); }


    InterningPools & tracingPools() { return *pools; }
    const InterningPools & tracingPools() const { return *pools; }
    std::optional<DepHash> lookupFileContentHash(const SourcePath & path) const;
    void cacheFileContentHash(const SourcePath & path, DepHash hash);
    void registerTracedValueIdentity(const Value * key, const eval_trace::TracedExpr & tracedExpr);
    void registerTracedBindingsValueIdentity(
        const Bindings * key,
        const eval_trace::TracedExpr & tracedExpr,
        const Bindings * originalBindings = nullptr);
    void rememberNixBinding(PosIdx pos, const NixBindingEntry & entry);
    const NixBindingEntry * lookupNixBinding(PosIdx pos) const;
    void registerMaterializedValueIdentity(
        const Value * key,
        eval_trace::TraceBackend * traceBackend,
        std::optional<SiblingIdentity> siblingIdentity,
        AttrPathId pathId,
        std::optional<ValueIdentityStamp> valueIdentityStamp = std::nullopt);
    ::nix::PublishedMaterializedIdentity publishRootMaterializedValueIdentity(
        const Value * key,
        eval_trace::TraceBackend * traceBackend,
        AttrPathId pathId,
        ValueIdentityStamp valueIdentityStamp);
    void rollbackRootMaterializedValueIdentity(const ::nix::PublishedMaterializedIdentity & publication);
    bool shouldIsolateSiblingForce(const Value & v) const;
    void recordThunkDeps(const Value & v, uint32_t epochStart);
    void replayMemoizedDeps(const Value & v);
    bool sameValueIdentity(Value & v1, Value & v2);
    void reset();
    friend class EvalState;
    friend struct eval_trace::TracedExpr;
    friend struct eval_trace::SiblingReplayCaptureScope;
    friend struct eval_trace::test::TraceRuntimeTestAccess;

    std::optional<ValueIdentity> lookupCapturedValueIdentity(const Value & v) const;
    std::optional<ValueIdentity> lookupValueIdentity(Value & v) const;
    std::optional<ValueIdentityStamp> lookupValueIdentityStamp(const Value & v) const;
    static ValueIdentity makeValueIdentity(
        bool isTracedProducer,
        eval_trace::TraceBackend * traceBackend,
        std::optional<SiblingIdentity> siblingIdentity,
        ValueContext valueContext,
        std::optional<ValueIdentityStamp> valueIdentityStamp = std::nullopt);
    static std::optional<SiblingIdentity> makeSiblingIdentity(
        std::optional<ParentSlot> parentSlot,
        std::optional<DefinitionStamp> definitionStamp,
        std::optional<SlotStamp> slotStamp,
        uint32_t canonicalSiblingIdx);
    static std::optional<ListIdentityStamp> makeListIdentityStamp(
        std::optional<ParentSlot> parentSlot,
        uint32_t canonicalSiblingIdx);
    ValueIdentityStamp lookupOrCreateBindingsIdentityStamp(const Bindings * key);
    ValueIdentityStamp lookupOrCreateListValueIdentityStamp(const ListIdentityStamp & stamp);

    /// Get or create the vocab store. Must pass the EvalState's SymbolTable.
    eval_trace::AttrVocabStore & getVocabStore(SymbolTable & syms) {
        if (!vocabStore)
            vocabStore = std::make_unique<eval_trace::AttrVocabStore>(syms);
        return *vocabStore;
    }

    /**
     * Check whether two Values were produced by TracedExpr thunks that
     * resolve to the same logical traced value.
     *
     * When eval-trace materialization creates fresh wrappers, pointer identity
     * is lost for aliased values such as copied attrsets, independently
     * materialized traced lists, or incomparable function-bearing containers.
     * This method restores identity with owned metadata rather than by
     * re-navigating the real tree:
     *
     *   1. Same object / same list backing fast path.
     *   2. Matching ValueIdentityStamp fast path for traced containers.
     *   3. Matching stamped sibling identity for other traced incomparable
     *      values that still compare through their producer child metadata.
     *
     * Lookup uses valueIdentityMap only for exact traced values. Value copies
     * recover identity from owned container stamps instead of long-lived
     * pointer-keyed side maps or rootLoader-based recovery.
     */
};

} // namespace nix
