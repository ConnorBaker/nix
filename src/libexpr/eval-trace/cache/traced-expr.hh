#pragma once
/// @file
/// TracedExpr and supporting types — private header for eval-trace cache internals.
/// Not installed; included only by .cc files in eval-trace/cache/.

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/eval-context.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "node-locator.hh"

#include <functional>
#include <optional>

namespace nix::eval_trace {

class TraceSession;
struct AttrVocabStore;

// ── TracedExpr ───────────────────────────────────────────────────────

/**
 * GC-allocated Expr thunk that implements deep constructive tracing (BSalC DCT).
 *
 * Each TracedExpr is an Adapton articulation point: a memoized computation node
 * in the demand-driven dependency graph. When forced via eval(), it dispatches:
 *   1. Verify path (BSalC VT check): if a trace exists with valid dep hashes,
 *      serve the stored constructive result without re-evaluation.
 *   2. Fresh evaluation (Adapton demand-driven recomputation): navigate to the
 *      real expression, force it, record the result as a constructive trace.
 *   3. Recovery (BSalC CT recovery): on verification failure, search historical
 *      traces for one matching the current dep state.
 *
 * "Deep" means TracedExpr thunks are created at every nesting level -- root
 * attrsets, intermediate attrsets, and leaf values each get their own trace.
 * This is done by materializeResult() (on the fresh path) and the verify path's
 * child thunk creation, enabling per-attribute granularity.
 *
 * navigateToReal() traverses the real expression tree without mutating it —
 * siblings are registered in valueIdentityMap for dep tracking and identity
 * recovery, but their Value cells are never wrapped with TracedExpr thunks.
 * Dep isolation for real sibling thunks is handled in forceThunkValue() via
 * a conditional child DepRecordingContext.
 *
 * Devirtualisation (2026-05): Root/Child were previously two distinct
 * concrete subclasses with 7 virtuals between them.  Collapsed into
 * this single struct discriminated by `kind_`; call sites now go
 * directly through `TracedExpr::makeRoot` / `TracedExpr::makeChild`.
 * `Expr::eval` remains the one legitimate virtual slot — TracedExpr
 * overrides the evaluator's base.
 */
struct TracedExpr : Expr, gc
{
    enum class Kind : uint8_t { Root, Child };

    /// Raw pointer to the owning session. NOT ref<TraceSession> —
    /// shared_ptr inside a GC object creates an unsound destructor
    /// chain: gc_cleanup finalizer → ~shared_ptr → ~TraceSession →
    /// free() (mimalloc) called from inside GC_malloc(). This corrupts
    /// the heap on some platforms (observed as segfaults in nix build
    /// sandbox and deadlocks in debug builds after ~200 tests).
    ///
    /// Safety: TracedExpr is only created by TraceSession methods that
    /// pass `shared_from_this()`. In production, the session is kept alive by
    /// the environment-adjacent session-factory reuse cache plus any
    /// command-owned `ref<TraceSession>` handles; tests keep an explicit
    /// `activeSession_` owner. After releaseBackend(), zombie TracedExprs
    /// fall through to evaluateDirect() (null backend: traceBackend() is
    /// null so verify/getScheduler are skipped).
    ///
    /// No gc_cleanup = no finalizer = no destructor chain = no
    /// free-inside-GC_malloc = no heap corruption.
    TraceSession * cache;

    /// Discriminator between Root and Child.  Stored inline; no vtable
    /// lookup needed to switch on role.
    Kind kind_;

    /// Trie path in AttrVocabStore.  Root uses `AttrPathId{0}` (the
    /// root sentinel), set explicitly via the `{}` initializer;
    /// Child's ctor fills it in with `vocab().extendPath(parent.pathId, name)`.
    AttrPathId pathId{};

    /// Index of the canonical sibling this child is aliased to.
    /// Set during materialization from the parent's aliasOf vector.
    /// -1 = not set (no alias info). Canonical entries store their own index.
    /// Two children of the same parent with the same canonicalSiblingIdx
    /// are aliases (same underlying Value*).
    uint32_t canonicalSiblingIdx = invalidSiblingIndex;

    // ── Child-only fields ───────────────────────────────────────
    //
    // For a Root, `parentExpr == nullptr`; `name` is an empty Symbol;
    // `listIndex` is nullopt; and the three stamps below are default-
    // constructed (never observed because parentSlot()/etc. return
    // nullopt on Root).

    TracedExpr * parentExpr = nullptr;
    Symbol name;
    std::optional<size_t> listIndex;
    ParentSlot parentSlot_{};
    DefinitionStamp definitionStamp_{};
    SlotStamp slotStamp_{};

    /**
     * Lazy-initialized state for fields only needed when eval() or
     * navigateToReal() runs. Reduces per-child allocation from ~100 to ~40 bytes.
     * For unaccessed children (the vast majority), LazyState is never allocated.
     */
    struct LazyState : gc {
        std::optional<TraceId> traceId;
        /// The real (non-materialized) Value* this TracedExpr navigates to.
        /// Set during fresh evaluation (cold path) or lazily via getResolvedTarget().
        /// Used by eqValues() to restore pointer identity: two TracedExpr children
        /// that navigate to the same underlying Value share the same resolvedTarget,
        /// allowing the equality check to short-circuit before recursing into
        /// function-containing containers.
        Value * resolvedTarget = nullptr;
    };
    LazyState * lazy = nullptr;

    LazyState & ensureLazy() {
        if (!lazy) { lazy = new LazyState{}; nrLazyStateAllocated++; }
        return *lazy;
    }

    // ── Factories ───────────────────────────────────────────────

    /// Create a root-kind TracedExpr.  `pathId` is left at the root
    /// sentinel (`AttrPathId{0}`).
    static TracedExpr * makeRoot(TraceSession & session);

    /// Create a child-kind TracedExpr with the given parent + name.
    /// Extends `parent.pathId` via `vocab().extendPath` and computes
    /// the definition/slot stamps.
    static TracedExpr * makeChild(
        TraceSession & cache,
        Symbol name,
        TracedExpr & parent,
        std::optional<size_t> listIndex = std::nullopt);

    // ── Expr interface ──────────────────────────────────────────

    void eval(EvalState & state, Env & env, Value & v) override;
    void show(const SymbolTable &, std::ostream &) const override {}
    void showForHash(const SymbolTable &, std::ostream &, const CanonPath &) const override {}
    void bindVars(EvalState &, const std::shared_ptr<const StaticEnv> &) override {}

    // ── Non-virtual role-discriminated accessors ────────────────

    std::optional<ParentSlot> parentSlot() const
    {
        return kind_ == Kind::Root ? std::nullopt : std::optional{parentSlot_};
    }

    std::optional<DefinitionStamp> definitionStamp() const
    {
        return kind_ == Kind::Root ? std::nullopt : std::optional{definitionStamp_};
    }

    std::optional<SlotStamp> slotStamp() const
    {
        return kind_ == Kind::Root ? std::nullopt : std::optional{slotStamp_};
    }

    ValueContext valueContext() const { return ValueContext(pathId); }

    /// Dot-separated display path for diagnostics.
    std::string attrPathStr() const;

    /// Colored path: eval + publish + materialize. Requires Suspendable.
    void evaluateFresh(EvalContext<Suspendable> & ctx, Value & v);

    /// Uncolored path: just force the real target and copy. No recording,
    /// no publishing, no materialization. Used when no executor exists
    /// (no backend — EvalContext<Suspendable> cannot be constructed).
    void evaluateDirect(Value & v);

    /// Switches on `kind_`: Root returns `cache->getRealRoot()`, Child
    /// walks up the parent chain via `traverseRealTree`.
    Value * navigateToReal();

    /**
     * Get the real (non-materialized) Value* this TracedExpr navigates to.
     * Set eagerly during evaluatePhase2 (cold path), or computed lazily
     * on first call (hot path — triggers rootLoader + direct navigation).
     * Returns nullptr if navigation is not possible (e.g., root node).
     */
    Value * getResolvedTarget();

    Value * peekResolvedTarget() const
    {
        return lazy ? lazy->resolvedTarget : nullptr;
    }
    void cacheResolvedTarget(Value * target)
    {
        ensureLazy().resolvedTarget = target;
    }
    const Bindings * resolvedAttrBindingsHint() const
    {
        auto * target = peekResolvedTarget();
        return target && target->type() == nAttrs ? target->attrs() : nullptr;
    }

    void materializeResult(EvalContext<Suspendable> & ctx, Value & v, const CachedResult & cached);
    void replayTrace(EvalContext<Suspendable> & ctx,
                     const EpochLogWriteProof<EpochLogWriteReason::ReplayActive> &,
                     TraceId traceId);
    void installChildThunk(Value * val, Env * env, TracedExpr * child);
    TraceBackend * traceBackend() const;

    /// Convenience accessor for the vocab store (lazy-inits if needed).
    AttrVocabStore & vocab() const;

    // ── Child-only traversal helpers ────────────────────────────
    //
    // Safe to call on a Root too: `tracePathFromRoot` returns an
    // empty vector (because the Root's own parentExpr is null);
    // `traceChainFromRoot` returns `{this}`; `traverseRealTree`
    // degenerates to returning `cache->getRealRoot()`.

    NodeLocator tracePathFromRoot() const;
    std::vector<TracedExpr *> traceChainFromRoot() const;
    Value * traverseRealTree(bool registerSiblingIdentities, bool forceFinalValue, bool cacheTarget);

protected:
    /// Full eval + publish + materialize path. Requires Suspendable
    /// context for backend.record() strand dispatch.
    void evaluateResolvedTarget(EvalContext<Suspendable> & ctx, Value & v, std::function<Value *()> targetProvider);

    TracedExpr(TraceSession & cache, Kind kind);
};

// ── Free functions used across trace-cache translation units ─────────

/// Convert a forced Value into a CachedResult for storage/comparison.
CachedResult buildCachedResult(EvalState & st, Value & target);

} // namespace nix::eval_trace
