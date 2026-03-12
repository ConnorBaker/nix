#include "traced-expr.hh"
#include "sibling-tracker.hh"

#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/util/finally.hh"
#include "nix/expr/eval-trace/cache/trace-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/fetchers/fetchers.hh"

#include <cassert>
#include <cstring>

namespace nix::eval_trace {

// ── TracedExpr core methods ──────────────────────────────────────────

// Defined out-of-line because it needs TraceCache and AttrVocabStore definitions.
AttrVocabStore & TracedExpr::vocab() const
{
    return cache->state.traceCtx->getVocabStore(cache->state.symbols);
}

std::string TracedExpr::attrPathStr() const
{
    return vocab().displayPath(pathId);
}

TracedExpr::TracedExpr(TraceCache * cache, Symbol name, TracedExpr * parentExpr,
                       bool isListElement)
    : cache(cache)
    , name(name)
    , pathId(parentExpr
          ? vocab().extendPath(parentExpr->pathId, name)
          : AttrVocabStore::rootPath())
    , parentExpr(parentExpr)
    , isListElement(isListElement)
{
}

/// Install `child` as a TracedExpr thunk on `val` and register it in
/// the sibling identity map for already-materialized sibling detection.
void TracedExpr::installChildThunk(Value * val, Env * env, TracedExpr * child)
{
    val->mkThunk(env, child);
    if (cache->state.traceCtx)
        cache->state.traceCtx->siblingIdentityMap.emplace(val,
            EvalTraceContext::SiblingIdentity{
                child, cache->dbBackend.get(),
                child->parentExpr, child->canonicalSiblingIdx,
                child->pathId, nullptr});
}

Value * TracedExpr::getResolvedTarget()
{
    if (lazy && lazy->resolvedTarget)
        return lazy->resolvedTarget;
    if (!parentExpr)
        return nullptr;
    try {
        struct PathStep { Symbol sym; bool isListElement; };
        std::vector<PathStep> path;
        for (auto * e = this; e->parentExpr; e = e->parentExpr)
            path.push_back({e->name, e->isListElement});
        std::reverse(path.begin(), path.end());

        Value * v = cache->getOrEvaluateRoot();

        for (auto & step : path) {
            if (step.isListElement) {
                cache->state.forceValue(*v, noPos);
                if (v->type() != nList)
                    throw Error("expected a list while resolving target");
                auto indexStr = std::string(cache->state.symbols[step.sym]);
                size_t index = std::stoul(indexStr);
                if (index >= v->listSize())
                    throw Error("list index out of bounds while resolving target");
                v = v->listView()[index];
            } else {
                cache->state.forceAttrs(*v, noPos,
                    "while resolving target for pointer identity");
                auto * attr = v->attrs()->get(step.sym);
                if (!attr)
                    throw Error("attribute vanished while resolving target");
                v = attr->value;
            }
        }

        cache->state.forceValue(*v, noPos);
        ensureLazy().resolvedTarget = v;
        return v;
    } catch (std::exception & e) {
        debug("getResolvedTarget failed for '%s': %s", attrPathStr(), e.what());
        return nullptr;
    }
}

// ── Phase 1: Navigate (outside DependencyTracker) ────────────────────
//
// Determines node type and navigates child nodes BEFORE creating the
// tracker. NavigationResult is move-only and consumed exactly once by
// evaluatePhase2(), enforcing the invariant at compile time.

NavigationResult TracedExpr::navigatePhase1()
{
    if (parentExpr)
        return NavigatedChild{navigateToReal()};
    return UnevaluatedRoot{};
}

// ── Phase 2: Evaluate (inside DependencyTracker) ─────────────────────
//
// Consumes the NavigationResult from Phase 1. The exhaustive variant
// visit ensures every node type is handled. Adding a new variant without
// a handler is a compile error.

void TracedExpr::evaluatePhase2(NavigationResult && nav, Value & v)
{
    auto & pools = *cache->state.traceCtx->pools;
    DependencyTracker tracker(pools);
    cache->state.traceActiveDepth++;
    Finally decrementDepth{[&]{ cache->state.traceActiveDepth--; }};

    std::optional<SiblingAccessTracker> siblingTracker;
    if (parentExpr && cache->dbBackend)
        siblingTracker.emplace(parentExpr, pathId, cache->state.traceCtx.get());

    // ── appendParentContextDeps and first-touch sibling bypass ─────
    //
    // SiblingAccessTracker records sibling accesses via the siblingCallback
    // in replayMemoizedDeps.  However, that callback only fires for
    // already-forced siblings (those with epochMap entries).  When a child
    // forces a sibling thunk for the first time, the sibling's deps flow
    // directly into the child's ownDeps via record(), and siblingCallback
    // never fires.  This means siblingTracker->accesses will be empty for
    // first-touch siblings, causing the whole-parent ParentContext fallback
    // to be used instead of per-sibling ParentContext deps.
    //
    // Per-sibling ParentContext (the precise path) only works when ALL
    // accessed siblings were already forced before this child's evaluation
    // began.  The whole-parent fallback is sound but coarser — it depends
    // on the parent's entire trace hash rather than individual sibling
    // trace hashes.
    //
    // See forceThunkValue in eval.cc for the full explanation and why
    // generic quarantine was rejected.
    auto appendParentContextDeps = [&](std::vector<Dep> & deps) {
        if (!parentExpr || !cache->dbBackend) return;

        // Retry untraced siblings: their traces may have been recorded
        // during forceValue (siblingTracker retry resolves their trace hashes).
        if (siblingTracker && siblingTracker->hasUntracedAccess) {
            bool allResolved = true;
            for (auto & [sibPathId, store] : siblingTracker->untracedSiblings) {
                try {
                    auto hash = store->getCurrentTraceHash(sibPathId);
                    if (hash)
                        siblingTracker->recordAccess(sibPathId, *hash);
                    else
                        allResolved = false;
                } catch (...) { allResolved = false; }
            }
            if (allResolved)
                siblingTracker->hasUntracedAccess = false;
        }

        if (siblingTracker && !siblingTracker->accesses.empty()
            && !siblingTracker->hasUntracedAccess) {
            // Per-sibling deps (precise): all accessed siblings have trace hashes.
            for (auto & [sibPathId, hash] : siblingTracker->accesses) {
                deps.push_back(Dep::makeParentContext(
                    sibPathId, DepHashValue(hash.raw())));
            }
        } else {
            // Whole-parent fallback (sound): either no sibling accesses recorded,
            // or some siblings still have no trace hash after retry.
            auto parentTraceHash = cache->dbBackend->getCurrentTraceHash(parentExpr->pathId);
            if (parentTraceHash) {
                deps.push_back(Dep::makeParentContext(
                    parentExpr->pathId, DepHashValue(parentTraceHash->raw())));
            }
        }
    };

    // Exhaustive dispatch: each variant handler runs inside the tracker.
    Value * target = std::visit(overloaded{
        [&](NavigatedChild && nc) -> Value * {
            return nc.target;
        },
        [&](UnevaluatedRoot &&) -> Value * {
            // Root: rootLoader deps MUST be captured.
            return navigateToReal();
        },
    }, std::move(nav));

    auto & st = cache->state;

    // Cache the resolved target for pointer identity restoration in eqValues.
    ensureLazy().resolvedTarget = target;

    try {
        st.forceValue(*target, noPos);

        // drvPath-forcing guard -- CRITICAL CORRECTNESS INVARIANT.
        // Force drvPath BEFORE materialization to prevent infinite recursion
        // from the `//` operator sharing Value ptrs.
        if (target->type() == nAttrs && st.isDerivation(*target)) {
            if (auto * dp = target->attrs()->get(st.s.drvPath))
                st.forceValue(*dp->value, noPos);
        }
    } catch (EvalError &) {
        debug("setting '%s' to failed (fresh evaluation)", attrPathStr());
        if (cache->dbBackend) {
            auto directDeps = tracker.collectTraces();
            appendParentContextDeps(directDeps);

            try {
                auto result = cache->dbBackend->record(
                    pathId, failed_t{}, directDeps);
                ensureLazy().traceId = result.traceId;
            } catch (std::exception & e) {
                debug("trace recording failed for '%s': %s", attrPathStr(), e.what());
            }
        }
        throw;
    }

    if (cache->dbBackend) {
        auto directDeps = tracker.collectTraces();
        CachedResult attrValue = buildCachedResult(st, *target);
        appendParentContextDeps(directDeps);

        try {
            auto coldResult = cache->dbBackend->record(
                pathId, attrValue, directDeps);
            ensureLazy().traceId = coldResult.traceId;
        } catch (std::exception & e) {
            debug("trace recording failed for '%s': %s", attrPathStr(), e.what());
        }

        if (std::holds_alternative<misc_t>(attrValue)
            || std::holds_alternative<missing_t>(attrValue)
            || std::holds_alternative<placeholder_t>(attrValue)) {
            v = *target;
        } else {
            materializeResult(v, attrValue);
        }
        return;
    } else {
        v = *target;
    }
}

// Fresh evaluation -- type-safe two-phase dispatch (Adapton demand-driven recomputation)
void TracedExpr::evaluateFresh(Value & v)
{
    auto nav = navigatePhase1();
    evaluatePhase2(std::move(nav), v);
}

// TracedExpr::eval -- demand-driven dispatch (Adapton)
void TracedExpr::eval(EvalState & state, Env & env, Value & v)
{
    nrTracedExprForced++;
    if (!cache->dbBackend) {
        auto * target = navigateToReal();
        state.forceValue(*target, noPos);
        v = *target;
        return;
    }

    auto & db = *cache->dbBackend;

    // Record this TracedExpr as a sibling access in any active tracker
    // when eval() exits (both cache-hit and cache-miss paths). Uses RAII
    // to ensure recording happens even on exception paths.
    struct SiblingRecordGuard {
        EvalTraceContext::SiblingIdentity si;
        ~SiblingRecordGuard() {
            try { SiblingAccessTracker::maybeRecord(si); }
            catch (...) {}
        }
    } siblingGuard{EvalTraceContext::SiblingIdentity{
        this, cache->dbBackend.get(),
        parentExpr, canonicalSiblingIdx, pathId, nullptr}};

    // Register Bindings* → SiblingIdentity on all exit paths (attrset values).
    // ExprOpEq::eval creates stack-local Value copies (via ExprSelect's
    // `v = *vAttrs`), which have different addresses than the originals
    // in siblingIdentityMap. But copies preserve the Bindings* pointer,
    // so this secondary map enables haveSameResolvedTarget to find the
    // TracedExpr that produced a copied attrset Value.
    //
    // originalBindings is set from resolvedTarget when available (cold path
    // via evaluatePhase2). On hot path cache hits, resolvedTarget is null
    // and originalBindings stays null. Cross-parent alias detection for
    // these cases falls through to Tier 3 in haveSameResolvedTarget.
    Finally registerBindings{[&]{
        try {
            if (!cache->state.traceCtx) return;
            if (v.type() != nAttrs) return;
            auto & ctx = *cache->state.traceCtx;
            const Bindings * origBindings = nullptr;
            if (lazy && lazy->resolvedTarget && lazy->resolvedTarget->type() == nAttrs)
                origBindings = lazy->resolvedTarget->attrs();
            ctx.bindingsIdentityMap.emplace(v.attrs(),
                EvalTraceContext::SiblingIdentity{
                    this, cache->dbBackend.get(),
                    parentExpr, canonicalSiblingIdx, pathId,
                    origBindings});
        } catch (...) {}
    }};

    auto warmResult = db.verify(pathId, cache->inputAccessors, cache->state);

    if (warmResult) {
        auto & [cachedValue, cachedTraceId] = *warmResult;
        ensureLazy().traceId = cachedTraceId;

        // Handle failed values -- reproduce error
        if (std::get_if<failed_t>(&cachedValue)) {
            evaluateFresh(v);
            return;
        }

        // Non-materializable types: re-evaluate
        if (std::get_if<misc_t>(&cachedValue)
            || std::get_if<missing_t>(&cachedValue)
            || std::get_if<placeholder_t>(&cachedValue)) {
            evaluateFresh(v);
            return;
        }

        nrTraceCacheHits++;
        debug("trace verify hit for '%s'", attrPathStr());

        materializeResult(v, cachedValue);
        replayTrace(cachedTraceId);
        return;
    }

    // Verify miss -- fresh evaluation path
    nrTraceCacheMisses++;
    evaluateFresh(v);
}

// ── TraceCache public API ─────────────────────────────────────────────

static std::shared_ptr<TraceStore> makeDbBackend(
    const Hash & fingerprint, SymbolTable & symbols, InterningPools & pools, AttrVocabStore & vocab)
{
    int64_t contextHash;
    std::memcpy(&contextHash, fingerprint.hash, sizeof(contextHash));
    return std::make_shared<TraceStore>(symbols, pools, vocab, contextHash);
}

TraceCache::TraceCache(
    std::optional<std::reference_wrapper<const Hash>> useCache,
    EvalState & state,
    RootLoader rootLoader,
    boost::unordered_flat_map<std::string, SourcePath> inputAccessors)
    : state(state)
    , rootLoader(rootLoader)
    , inputAccessors(std::move(inputAccessors))
{
    if (useCache) {
        try {
            dbBackend = makeDbBackend(*useCache, state.symbols, *state.traceCtx->pools,
                state.traceCtx->getVocabStore(state.symbols));
        } catch (...) {
            ignoreExceptionExceptInterrupt();
        }
    }
}

Value * TraceCache::getOrEvaluateRoot()
{
    if (!realRoot) {
        debug("getting root value via rootLoader");
        realRoot = allocRootValue(rootLoader());
    }
    return *realRoot;
}

Value * TraceCache::getRootValue()
{
    if (!value) {
        auto * v = state.allocValue();
        v->mkThunk(&state.baseEnv, new TracedExpr(this, state.s.epsilon, nullptr));
        nrTracedExprCreated++;
        value = allocRootValue(v);
    }
    return *value;
}

} // namespace nix::eval_trace
