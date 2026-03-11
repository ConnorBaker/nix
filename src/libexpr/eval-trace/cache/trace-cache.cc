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
            EvalTraceContext::SiblingIdentity{child, cache->dbBackend.get()});
}

/// Resolve a potentially contaminated real-tree cell to its clean underlying
/// value WITHOUT modifying the original cell.  Returns `v` itself if clean,
/// or a newly allocated Value containing the origExpr result if contaminated.
///
/// Contamination happens when navigateToReal's sibling wrapping replaces a cell
/// with a TracedExpr thunk, and that thunk is later force-evaluated through
/// TracedExpr::eval(), producing a MATERIALIZED value with distinct Bindings*.
Value * TracedExpr::resolveClean(Value * v)
{
    // Case (a): cell is still a TracedExpr thunk (wrapped, not yet forced).
    if (v->isThunk()) {
        if (auto * ec = dynamic_cast<TracedExpr*>(v->thunk().expr)) {
            if (ec->lazy && ec->lazy->origExpr) {
                auto * clean = cache->state.allocValue();
                ec->lazy->origExpr->eval(cache->state, *ec->lazy->origEnv, *clean);
                return clean;
            }
        }
        return v; // vanilla thunk — clean
    }
    // Case (b): cell was wrapped + forced → materialized.
    // Detect via siblingIdentityMap (installChildThunk registers all wrapped cells).
    if (cache->state.traceCtx) {
        auto it = cache->state.traceCtx->siblingIdentityMap.find(v);
        if (it != cache->state.traceCtx->siblingIdentityMap.end()) {
            auto * ec = it->second.tracedExpr;
            if (ec->lazy && ec->lazy->origExpr) {
                auto * clean = cache->state.allocValue();
                ec->lazy->origExpr->eval(cache->state, *ec->lazy->origEnv, *clean);
                return clean;
            }
        }
    }
    return v; // not contaminated
}

Value * TracedExpr::getResolvedTarget()
{
    if (lazy && lazy->resolvedTarget) {
        return lazy->resolvedTarget;
    }
    // Only child nodes can navigate (root has no parent to navigate from).
    if (!parentExpr)
        return nullptr;
    try {
        // Navigate the real tree, resolving contamination at every level.
        // navigateToReal's sibling wrapping contaminates real-tree cells:
        // wrapped cells that are subsequently force-evaluated through
        // TracedExpr::eval() produce MATERIALIZED values with distinct
        // Bindings*, breaking pointer identity for aliased values.
        // resolveClean() allocates a temp Value when needed, avoiding
        // in-place modification that would corrupt other evaluation paths.
        struct PathStep { Symbol sym; bool isListElement; };
        std::vector<PathStep> path;
        for (auto * e = this; e->parentExpr; e = e->parentExpr)
            path.push_back({e->name, e->isListElement});
        std::reverse(path.begin(), path.end());

        Value * v = cache->getOrEvaluateRoot();

        for (auto & step : path) {
            v = resolveClean(v);

            if (step.isListElement) {
                cache->state.forceValue(*v, noPos);
                if (v->type() != nList)
                    throw Error("expected a list but found %s while resolving target",
                                showType(*v));
                auto indexStr = std::string(cache->state.symbols[step.sym]);
                size_t index = std::stoul(indexStr);
                if (index >= v->listSize())
                    throw Error("list index %d out of bounds (size %d) while resolving target",
                                index, v->listSize());
                v = v->listView()[index];
            } else {
                cache->state.forceAttrs(*v, noPos,
                    "while resolving target for pointer identity");
                auto * attr = v->attrs()->get(step.sym);
                if (!attr)
                    throw Error("attribute '%s' vanished while resolving target",
                                cache->state.symbols[step.sym]);
                v = attr->value;
            }
        }

        // Resolve the final target cell.
        v = resolveClean(v);
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
    if (lazy && lazy->origExpr)
        return SiblingWrapped{lazy->origExpr, lazy->origEnv};
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
    const bool hasOrig = std::holds_alternative<SiblingWrapped>(nav);

    auto & pools = *cache->state.traceCtx->pools;
    DependencyTracker tracker(pools);
    cache->state.traceActiveDepth++;
    Finally decrementDepth{[&]{ cache->state.traceActiveDepth--; }};

    std::optional<SiblingAccessTracker> siblingTracker;
    if (parentExpr && cache->dbBackend)
        siblingTracker.emplace(parentExpr, cache->state.traceCtx.get());

    auto appendParentContextDeps = [&](std::vector<Dep> & deps) {
        if (!parentExpr || !cache->dbBackend) return;
        if (siblingTracker && !siblingTracker->accesses.empty()) {
            for (auto & [sibPathId, hash] : siblingTracker->accesses) {
                deps.push_back(Dep::makeParentContext(
                    sibPathId, DepHashValue(hash.raw())));
            }
        } else {
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
            // Child !hasOrig: target already navigated by Phase 1.
            return nc.target;
        },
        [&](UnevaluatedRoot &&) -> Value * {
            // Root !hasOrig: rootLoader deps MUST be captured.
            return navigateToReal();
        },
        [&](SiblingWrapped && sw) -> Value * {
            // hasOrig: origExpr deps MUST be captured.
            auto * t = cache->state.allocValue();
            sw.origExpr->eval(cache->state, *sw.origEnv, *t);
            return t;
        },
    }, std::move(nav));

    // Unwrap TracedExpr thunks created by sibling wrapping (Lesson 8).
    // navigateToReal may return a value that a prior sibling's wrapping turned
    // into a TracedExpr thunk. The unwrap evaluates origExpr -- real Nix code
    // (readFile, derivationStrict, etc.) -- so it MUST run inside the tracker.
    if (!hasOrig && target->isThunk()) {
        if (auto * ec = dynamic_cast<TracedExpr*>(target->thunk().expr)) {
            if (ec->lazy && ec->lazy->origExpr) {
                ec->lazy->origExpr->eval(cache->state, *ec->lazy->origEnv, *target);
            }
        }
    }

    auto & st = cache->state;

    // Cache the resolved target for pointer identity restoration in eqValues.
    ensureLazy().resolvedTarget = target;

    try {
        st.forceValue(*target, noPos);

        if (hasOrig)
            v = *target;

        // drvPath-forcing guard -- CRITICAL CORRECTNESS INVARIANT.
        // For !hasOrig derivation attrs, force drvPath BEFORE sibling wrapping
        // to prevent infinite recursion from the `//` operator sharing Value ptrs.
        if (!hasOrig && target->type() == nAttrs && st.isDerivation(*target)) {
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

        if (hasOrig && std::holds_alternative<attrs_t>(attrValue)) {
            materializeOrigExprAttrs(v, std::get<attrs_t>(attrValue), target);
            return;
        }

        if (hasOrig) {
            v = *target;
        } else if (std::holds_alternative<misc_t>(attrValue)
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
        if (lazy && lazy->origExpr) {
            lazy->origExpr->eval(state, *lazy->origEnv, v);
            state.forceValue(v, noPos);
        } else {
            auto * target = navigateToReal();
            state.forceValue(*target, noPos);
            v = *target;
        }
        return;
    }

    auto & db = *cache->dbBackend;

    // Record this TracedExpr as a sibling access in any active tracker
    // when eval() exits (both cache-hit and cache-miss paths). Uses RAII
    // to ensure recording happens even on exception paths.
    struct SiblingRecordGuard {
        TracedExpr * self;
        TraceStore & db;
        ~SiblingRecordGuard() {
            try { SiblingAccessTracker::maybeRecord(self, db); }
            catch (...) {}
        }
    } siblingGuard{this, db};

    // Register Bindings* → SiblingIdentity on all exit paths.
    // ExprOpEq::eval creates stack-local Value copies (via ExprSelect's
    // `v = *vAttrs`), which have different addresses than the originals
    // in siblingIdentityMap. But copies preserve the Bindings* pointer,
    // so this secondary map enables haveSameResolvedTarget to find the
    // TracedExpr that produced a copied attrset Value.
    Finally registerBindings{[&]{
        try {
            if (v.type() == nAttrs && cache->state.traceCtx)
                cache->state.traceCtx->bindingsIdentityMap.emplace(
                    v.attrs(), EvalTraceContext::SiblingIdentity{this, cache->dbBackend.get()});
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

        bool handled = false;
        if (lazy && lazy->origExpr) {
            if (isLeafCached(cachedValue)) {
                materializeResult(v, cachedValue);
                replayTrace(cachedTraceId);
                handled = true;
            } else if (auto * attrs = std::get_if<attrs_t>(&cachedValue)) {
                materializeOrigExprAttrs(v, *attrs);
                replayTrace(cachedTraceId);
                handled = true;
            }
            // list_t or other -> falls through to evaluateFresh (not a cache hit)
        } else {
            materializeResult(v, cachedValue);
            replayTrace(cachedTraceId);
            handled = true;
        }

        if (handled)
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
