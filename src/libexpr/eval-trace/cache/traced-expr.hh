#pragma once
/// @file
/// TracedExpr and supporting types — private header for eval-trace cache internals.
/// Not installed; included only by .cc files in eval-trace/cache/.

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/eval-trace/counters.hh"

#include <optional>
#include <variant>

namespace nix::eval_trace {

struct TraceCache;
struct AttrVocabStore;

// ── Phase 1/Phase 2 type-state for evaluateFresh ─────────────────────
//
// Navigation must happen BEFORE DependencyTracker creation for child nodes
// (their navigation deps are parent-scope infrastructure). These types
// encode that invariant: navigatePhase1() produces a NavigationResult
// consumed by evaluatePhase2(), making it impossible to accidentally
// navigate inside the tracker for child nodes.

/// Child !hasOrig: navigated to real value BEFORE tracker.
struct NavigatedChild {
    Value * target;
};

/// Root !hasOrig: no Phase 1 action. Navigation happens in Phase 2
/// because rootLoader's deps MUST be captured in the root's tracker.
struct UnevaluatedRoot {};

/// Sibling-wrapped (hasOrig): origExpr evaluation happens in Phase 2
/// because its deps (readFile, derivationStrict, etc.) MUST be captured.
struct SiblingWrapped {
    Expr * origExpr;
    Env * origEnv;
};

using NavigationResult = std::variant<NavigatedChild, UnevaluatedRoot, SiblingWrapped>;

// ── SharedParentResult ───────────────────────────────────────────────

struct SharedParentResult : gc
{
    Value * value = nullptr;
};

// ── ExprOrigChild ────────────────────────────────────────────────────

struct ExprOrigChild : Expr, gc
{
    Expr * parentOrigExpr;
    Env * parentOrigEnv;
    Symbol childName;
    SharedParentResult * shared;

    ExprOrigChild(Expr * parentOrigExpr, Env * parentOrigEnv,
                  Symbol childName, SharedParentResult * shared)
        : parentOrigExpr(parentOrigExpr)
        , parentOrigEnv(parentOrigEnv)
        , childName(childName)
        , shared(shared)
    {}

    void eval(EvalState & state, Env &, Value & v) override;
    void show(const SymbolTable &, std::ostream &) const override {}
    void bindVars(EvalState &, const std::shared_ptr<const StaticEnv> &) override {}
};

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
 * The origExpr/origEnv fields support dual-mode evaluation: when set, the thunk
 * was created by navigateToReal() as a sibling wrapper, and evaluateFresh() uses
 * the original expression rather than navigating through the real tree (which
 * would cause infinite recursion via blackholed parent values).
 */
struct TracedExpr : Expr, gc
{
    TraceCache * cache;
    Symbol name;
    AttrPathId pathId;           // Trie path in AttrVocabStore (computed at construction)
    TracedExpr * parentExpr;     // GC-traced, nullptr for root
    bool isListElement;          // true = list index, false = attr access
    /// Index of the canonical sibling this child is aliased to.
    /// Set during materialization from the parent's aliasOf vector.
    /// -1 = not set (no alias info). Canonical entries store their own index.
    /// Two children of the same parent with the same canonicalSiblingIdx
    /// are aliases (same underlying Value*).
    int16_t canonicalSiblingIdx = -1;

    /**
     * Lazy-initialized state for fields only needed when eval() or
     * navigateToReal() runs. Reduces per-child allocation from ~100 to ~40 bytes.
     * For unaccessed children (the vast majority), LazyState is never allocated.
     */
    struct LazyState : gc {
        Expr * origExpr = nullptr;
        Env * origEnv = nullptr;
        std::optional<TraceId> traceId;
        /// The real (non-materialized) Value* this TracedExpr navigates to.
        /// Set during evaluatePhase2 (cold path) or lazily via getResolvedTarget().
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

    TracedExpr(TraceCache * cache, Symbol name, TracedExpr * parentExpr,
               bool isListElement = false);

    void eval(EvalState & state, Env & env, Value & v) override;
    void show(const SymbolTable &, std::ostream &) const override {}
    void bindVars(EvalState &, const std::shared_ptr<const StaticEnv> &) override {}

    /// Dot-separated display path for diagnostics.
    std::string attrPathStr() const;

    std::optional<TraceId> parentTraceId() const
    {
        if (!parentExpr) return std::nullopt;
        return parentExpr->lazy ? parentExpr->lazy->traceId : std::nullopt;
    }

    void evaluateFresh(Value & v);
    NavigationResult navigatePhase1();
    void evaluatePhase2(NavigationResult && nav, Value & v);
    Value * navigateToReal();

    /**
     * Get the real (non-materialized) Value* this TracedExpr navigates to.
     * Set eagerly during evaluatePhase2 (cold path), or computed lazily
     * on first call (hot path — triggers rootLoader + clean navigation).
     * Returns nullptr if navigation is not possible (e.g., root node).
     *
     * Uses decontaminating navigation: at each step, if a real-tree cell
     * was contaminated by navigateToReal's sibling wrapping (TracedExpr
     * thunk still pending, or wrapped-then-forced to materialized value),
     * re-evaluates the original expression to recover the real shared value.
     */
    Value * getResolvedTarget();

    /// Resolve a potentially contaminated real-tree cell to a clean Value*
    /// without modifying the original.  Returns `v` if clean, or a newly
    /// allocated Value with the origExpr result if contaminated.
    Value * resolveClean(Value * v);

    void materializeResult(Value & v, const CachedResult & cached);
    void materializeOrigExprAttrs(Value & v, const attrs_t & attrs,
                                   Value * prePopulatedParent = nullptr);
    void replayTrace(TraceId traceId);
    void installChildThunk(Value * val, Env * env, TracedExpr * child);

    /// Convenience accessor for the vocab store (lazy-inits if needed).
    AttrVocabStore & vocab() const;
};

// ── Free functions used across trace-cache translation units ─────────

/// True if the CachedResult is a leaf type (string, bool, int, path, null, float, string list).
bool isLeafCached(const CachedResult & v);

/// Convert a forced Value into a CachedResult for storage/comparison.
CachedResult buildCachedResult(EvalState & st, Value & target);

} // namespace nix::eval_trace
