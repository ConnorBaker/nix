#include "nix/expr/trace-store.hh"
#include "nix/util/environment-variables.hh"
#include "nix/expr/trace-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/expr/value-to-json.hh"

#include <algorithm>

namespace nix::eval_trace {

Counter nrTraceCacheHits;
Counter nrTraceCacheMisses;
Counter nrTraceVerifications;
Counter nrVerificationsPassed;
Counter nrVerificationsFailed;
Counter nrDepsChecked;
Counter nrRecoveryFailures;

// Timing accumulators (microseconds)
Counter nrVerifyTimeUs;
Counter nrVerifyTraceTimeUs;
Counter nrRecoveryTimeUs;
Counter nrRecoveryDirectHashTimeUs;
Counter nrRecoveryStructVariantTimeUs;
Counter nrRecordTimeUs;
Counter nrLoadTraceTimeUs;
Counter nrDbInitTimeUs;
Counter nrDbCloseTimeUs;

// Event counters
Counter nrRecoveryAttempts;
Counter nrRecoveryDirectHashHits;
Counter nrRecoveryStructVariantHits;
Counter nrRecords;
Counter nrLoadTraces;

// ── TracedExpr struct definition ─────────────────────────────────────

/**
 * GC-allocated Expr thunk that implements deep constructive tracing (BSàlC DCT).
 *
 * Each TracedExpr is an Adapton articulation point: a memoized computation node
 * in the demand-driven dependency graph. When forced via eval(), it dispatches:
 *   1. Verify path (BSàlC VT check): if a trace exists with valid dep hashes,
 *      serve the stored constructive result without re-evaluation.
 *   2. Fresh evaluation (Adapton demand-driven recomputation): navigate to the
 *      real expression, force it, record the result as a constructive trace.
 *   3. Recovery (BSàlC CT recovery): on verification failure, search historical
 *      traces for one matching the current dep state.
 *
 * "Deep" means TracedExpr thunks are created at every nesting level — root
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
    TracedExpr * parentExpr; // GC-traced, nullptr for root
    bool isListElement;      // true = list index, false = attr access

    Expr * origExpr = nullptr;
    Env * origEnv = nullptr;

    /**
     * The trace ID for this attribute's trace (dependency record).
     * Set after verify path succeeds or fresh evaluation records the result.
     */
    std::optional<TraceId> traceId;

    /**
     * Cached storeAttrPath result. Lazily populated on first call.
     */
    mutable std::optional<std::string> cachedStoreAttrPath;

    TracedExpr(TraceCache * cache, AttrId /*unused*/, Symbol name, TracedExpr * parentExpr,
               bool isListElement = false)
        : cache(cache)
        , name(name)
        , parentExpr(parentExpr)
        , isListElement(isListElement)
    {
    }

    void eval(EvalState & state, Env & env, Value & v) override;
    void show(const SymbolTable &, std::ostream &) const override {}
    void bindVars(EvalState &, const std::shared_ptr<const StaticEnv> &) override {}

    std::string attrPathStr() const
    {
        if (!parentExpr)
            return "«root»";
        std::vector<Symbol> syms;
        for (auto * e = this; e->parentExpr; e = e->parentExpr)
            syms.push_back(e->name);
        std::reverse(syms.begin(), syms.end());
        AttrPath path(syms.begin(), syms.end());
        return path.to_string(cache->state);
    }

    // ── DB backend methods ──────────────────────────────────────

    std::string storeAttrPath() const
    {
        if (cachedStoreAttrPath)
            return *cachedStoreAttrPath;
        std::string result;
        if (!parentExpr) {
            result = ""; // root
        } else {
            std::vector<std::string> components;
            for (auto * e = this; e->parentExpr; e = e->parentExpr)
                components.push_back(std::string(cache->state.symbols[e->name]));
            std::reverse(components.begin(), components.end());
            result = TraceStore::buildAttrPath(components);
        }
        cachedStoreAttrPath = result;
        return result;
    }

    std::optional<TraceId> parentTraceId() const
    {
        if (!parentExpr) return std::nullopt;
        return parentExpr->traceId;
    }

    void evaluateFresh(Value & v);
    Value * navigateToReal();
    void materializeResult(Value & v, const CachedResult & cached);
    void materializeOrigExprAttrs(Value & v, const std::vector<Symbol> & childNames,
                                   Value * prePopulatedParent = nullptr);
    void replayTrace(TraceId traceId);
    void recordSiblingTrace(TracedExpr * parentEC, Symbol siblingName, Value & v);
    void sortChildNames(std::vector<Symbol> & names) const;
};

// ── Support types (SharedParentResult, ExprOrigChild) ────────────────

struct SharedParentResult : gc
{
    Value * value = nullptr;
};

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

    void eval(EvalState & state, Env &, Value & v) override
    {
        if (!shared->value) {
            shared->value = state.allocValue();
            {
                SuspendDepTracking suspend;
                parentOrigExpr->eval(state, *parentOrigEnv, *shared->value);
                state.forceAttrs(*shared->value, noPos,
                    "while resolving cached child attribute");
            }
        }
        auto * attr = shared->value->attrs()->get(childName);
        if (!attr)
            throw Error("attribute '%s' not found in cached parent",
                        state.symbols[childName]);
        state.forceValue(*attr->value, noPos);
        v = *attr->value;
    }

    void show(const SymbolTable &, std::ostream &) const override {}
    void bindVars(EvalState &, const std::shared_ptr<const StaticEnv> &) override {}
};

// ── Helpers ──────────────────────────────────────────────────────────

static bool isLeafCached(const CachedResult & v)
{
    return std::get_if<string_t>(&v)
        || std::get_if<bool>(&v)
        || std::get_if<int_t>(&v)
        || std::get_if<path_t>(&v)
        || std::get_if<null_t>(&v)
        || std::get_if<float_t>(&v)
        || std::get_if<std::vector<std::string>>(&v);
}

void TracedExpr::sortChildNames(std::vector<Symbol> & names) const
{
    std::sort(names.begin(), names.end(),
        [&](Symbol a, Symbol b) {
            return std::string_view(cache->state.symbols[a])
                 < std::string_view(cache->state.symbols[b]);
        });
}

// ── TracedExpr implementation (Adapton articulation points) ──────────

void TracedExpr::materializeOrigExprAttrs(
    Value & v, const std::vector<Symbol> & childNames, Value * prePopulatedParent)
{
    auto * shared = new SharedParentResult();
    if (prePopulatedParent)
        shared->value = prePopulatedParent;
    auto bindings = cache->state.buildBindings(childNames.size());
    for (auto & childName : childNames) {
        auto * childVal = cache->state.allocValue();
        auto * wrapper = new TracedExpr(cache, 0, childName, this);
        wrapper->origExpr = new ExprOrigChild(origExpr, origEnv, childName, shared);
        wrapper->origEnv = origEnv;
        childVal->mkThunk(&cache->state.baseEnv, wrapper);
        bindings.insert(childName, childVal, noPos);
    }
    v.mkAttrs(bindings.finish());
}

// Replay trace (Adapton change propagation)
void TracedExpr::replayTrace(TraceId traceId)
{
    if (!DependencyTracker::isActive())
        return;

    try {
        auto deps = cache->dbBackend->loadFullTrace(traceId);
        for (auto & dep : deps)
            DependencyTracker::record(dep);
    } catch (std::exception &) {
        // DB may be corrupt or trace may have been evicted — skip
    }
}

void TracedExpr::recordSiblingTrace(TracedExpr * /* parentEC */, Symbol /* siblingName */, Value & /* v */)
{
    // Disabled: with dep separation (no parent dep merging), sibling traces
    // recorded with empty deps always verify as valid, returning stale results.
    // Siblings will be evaluated fresh when needed and get proper deps.
}

// navigateToReal — real tree navigation for fresh evaluation
Value * TracedExpr::navigateToReal()
{
    struct PathStep {
        Symbol sym;
        bool isListElement;
    };
    std::vector<PathStep> path;
    for (auto * e = this; e->parentExpr; e = e->parentExpr)
        path.push_back({e->name, e->isListElement});
    std::reverse(path.begin(), path.end());

    auto * v = cache->getOrEvaluateRoot();

    std::vector<TracedExpr*> storeExprChain;
    for (auto * e = this; e; e = e->parentExpr)
        storeExprChain.push_back(e);
    std::reverse(storeExprChain.begin(), storeExprChain.end());
    size_t pathStep = 0;

    for (auto & step : path) {
        if (step.isListElement) {
            cache->state.forceValue(*v, noPos);
            if (v->type() != nList)
                throw Error("expected a list but found %s while navigating to cached list element",
                            showType(*v));

            auto indexStr = std::string(cache->state.symbols[step.sym]);
            size_t index = std::stoul(indexStr);
            if (index >= v->listSize())
                throw Error("list index %d out of bounds (size %d) during re-evaluation",
                            index, v->listSize());

            v = v->listView()[index];
        } else {
            cache->state.forceAttrs(*v, noPos, "while navigating to cached attribute");

            auto * parentEC = storeExprChain[pathStep];
            for (auto & attr : *v->attrs()) {
                if (attr.name == step.sym)
                    continue;
                if (attr.value->isThunk()
                    && !dynamic_cast<TracedExpr*>(attr.value->thunk().expr))
                {
                    auto * wrapper = new TracedExpr(cache, 0, attr.name, parentEC);
                    wrapper->origExpr = attr.value->thunk().expr;
                    wrapper->origEnv = attr.value->thunk().env;
                    attr.value->mkThunk(attr.value->thunk().env, wrapper);
                }
                else if (!attr.value->isThunk())
                {
                    recordSiblingTrace(parentEC, attr.name, *attr.value);
                }
            }

            auto * attr = v->attrs()->get(step.sym);
            if (!attr)
                throw Error("attribute '%s' vanished during re-evaluation",
                            cache->state.symbols[step.sym]);
            v = attr->value;
        }
        ++pathStep;
    }

    return v;
}

// materializeResult — construct deep traced values (Adapton articulation points)
void TracedExpr::materializeResult(Value & v, const CachedResult & cached)
{
    auto & st = cache->state;

    if (auto * attrs = std::get_if<std::vector<Symbol>>(&cached)) {
        auto bindings = st.buildBindings(attrs->size());
        for (auto & childName : *attrs) {
            auto * childVal = st.allocValue();
            auto * child = new TracedExpr(cache, 0, childName, this);
            childVal->mkThunk(&st.baseEnv, child);
            bindings.insert(childName, childVal, noPos);
        }
        v.mkAttrs(bindings.finish());
    } else if (auto * s = std::get_if<string_t>(&cached)) {
        if (s->second.empty())
            v.mkString(s->first, st.mem);
        else
            v.mkString(s->first, s->second, st.mem);
    } else if (auto * b = std::get_if<bool>(&cached)) {
        v.mkBool(*b);
    } else if (auto * i = std::get_if<int_t>(&cached)) {
        v.mkInt(i->x);
    } else if (auto * p = std::get_if<path_t>(&cached)) {
        v.mkPath(st.rootPath(CanonPath(p->path)), st.mem);
    } else if (auto * l = std::get_if<std::vector<std::string>>(&cached)) {
        auto list = st.buildList(l->size());
        for (size_t i = 0; i < l->size(); i++) {
            auto * elemVal = st.allocValue();
            elemVal->mkString((*l)[i], st.mem);
            list[i] = elemVal;
        }
        v.mkList(list);
    } else if (std::get_if<null_t>(&cached)) {
        v.mkNull();
    } else if (auto * f = std::get_if<float_t>(&cached)) {
        v.mkFloat(f->x);
    } else if (auto * lt = std::get_if<list_t>(&cached)) {
        auto list = st.buildList(lt->size);
        for (size_t i = 0; i < lt->size; i++) {
            auto * elemVal = st.allocValue();
            auto sym = st.symbols.create(std::to_string(i));
            auto * child = new TracedExpr(cache, 0, sym, this, /*isListElement=*/true);
            elemVal->mkThunk(&st.baseEnv, child);
            list[i] = elemVal;
        }
        v.mkList(list);
    } else {
        // misc_t, failed_t, missing_t, placeholder_t — fresh evaluation needed
        evaluateFresh(v);
    }
}

// Fresh evaluation (Adapton demand-driven recomputation)
void TracedExpr::evaluateFresh(Value & v)
{
    DependencyTracker tracker;

    Value * target;
    if (origExpr) {
        target = cache->state.allocValue();
        origExpr->eval(cache->state, *origEnv, *target);
    } else {
        if (parentExpr) {
            SuspendDepTracking suspend;
            target = navigateToReal();
        } else {
            target = navigateToReal();
        }

        if (target->isThunk()) {
            if (auto * ec = dynamic_cast<TracedExpr*>(target->thunk().expr)) {
                if (ec->origExpr) {
                    Expr * expr = ec->origExpr;
                    Env * oenv = ec->origEnv;
                    target = cache->state.allocValue();
                    expr->eval(cache->state, *oenv, *target);
                }
            }
        }
    }

    auto & st = cache->state;

    try {
        st.forceValue(*target, noPos);

        if (origExpr)
            v = *target;

        if (!origExpr && target->type() == nAttrs && st.isDerivation(*target)) {
            if (auto * dp = target->attrs()->get(st.s.drvPath))
                st.forceValue(*dp->value, noPos);
        }
    } catch (EvalError &) {
        debug("setting '%s' to failed (fresh evaluation)", attrPathStr());
        if (cache->dbBackend) {
            auto attrPath = storeAttrPath();
            auto directDeps = tracker.collectTraces();

            // Add ParentContext dep for navigated children
            if (!origExpr && parentExpr) {
                auto parentPath = parentExpr->storeAttrPath();
                auto parentTraceHash = cache->dbBackend->getCurrentTraceHash(parentPath);
                if (parentTraceHash) {
                    Blake3Hash b3;
                    std::memcpy(b3.bytes.data(), parentTraceHash->hash, 32);
                    // Use \t separator for dep key (Strings table is TEXT, truncates at \0)
                    auto depKey = parentPath;
                    std::replace(depKey.begin(), depKey.end(), '\0', '\t');
                    directDeps.push_back(Dep{
                        "", depKey, DepHashValue(b3), DepType::ParentContext});
                }
            }

            try {
                auto result = cache->dbBackend->record(
                    attrPath, failed_t{}, directDeps, !parentExpr);
                this->traceId = result.traceId;
            } catch (std::exception & e) {
                debug("trace recording failed for '%s': %s", attrPathStr(), e.what());
            }
        }
        throw;
    }

    if (cache->dbBackend) {
        auto attrPath = storeAttrPath();
        auto directDeps = tracker.collectTraces();

        // Build the CachedResult for storage
        CachedResult attrValue;
        if (target->type() == nString) {
            NixStringContext ctx;
            if (target->context()) {
                for (auto * elem : *target->context())
                    ctx.insert(NixStringContextElem::parse(elem->view()));
            }
            attrValue = string_t{std::string(target->string_view()), std::move(ctx)};
        } else if (target->type() == nBool) {
            attrValue = target->boolean();
        } else if (target->type() == nInt) {
            attrValue = int_t{NixInt{target->integer().value}};
        } else if (target->type() == nNull) {
            attrValue = null_t{};
        } else if (target->type() == nFloat) {
            attrValue = float_t{target->fpoint()};
        } else if (target->type() == nPath) {
            attrValue = path_t{target->path().path.abs()};
        } else if (target->type() == nAttrs) {
            std::vector<Symbol> childNames;
            for (auto & attr : *target->attrs())
                childNames.push_back(attr.name);
            sortChildNames(childNames);
            attrValue = childNames;
        } else if (target->type() == nList) {
            // Suspend dep tracking during the allStrings check. Without this,
            // forcing ExprTracedData element thunks would record StructuredContent
            // deps in this trace's DependencyTracker, mixing them with the Content
            // dep from readFile. That would let the two-level override incorrectly
            // validate the trace when the list size changes but element values don't
            // (e.g., element added to a JSON array). By suspending, the list trace
            // has only Content deps → any file change invalidates → correct.
            // Per-element override still works via TracedExpr children in list_t path.
            bool allStrings = true;
            {
                SuspendDepTracking suspend;
                for (size_t i = 0; i < target->listSize(); i++) {
                    st.forceValue(*target->listView()[i], noPos);
                    if (target->listView()[i]->type() != nString) { allStrings = false; break; }
                }
            }
            if (allStrings) {
                std::vector<std::string> strs;
                for (size_t i = 0; i < target->listSize(); i++)
                    strs.push_back(std::string(target->listView()[i]->c_str()));
                attrValue = strs;
            } else {
                attrValue = list_t{target->listSize()};
            }
        } else {
            attrValue = misc_t{};
        }

        // For navigated children (no origExpr), add a ParentContext dep linking
        // this trace to the parent's current trace hash. Without this, zero-dep
        // navigated children always verify as valid, returning stale results
        // when the parent's value changes (e.g., after fetchGit repo update).
        // We use trace_hash (not result hash) because result hash for attrsets
        // only captures attribute names, not values.
        if (!origExpr && parentExpr && cache->dbBackend) {
            auto parentPath = parentExpr->storeAttrPath();
            auto parentTraceHash = cache->dbBackend->getCurrentTraceHash(parentPath);
            if (parentTraceHash) {
                Blake3Hash b3;
                static_assert(sizeof(b3.bytes) == 32);
                std::memcpy(b3.bytes.data(), parentTraceHash->hash, 32);
                // Use \t separator for dep key (Strings table is TEXT, truncates at \0)
                auto depKey = parentPath;
                std::replace(depKey.begin(), depKey.end(), '\0', '\t');
                directDeps.push_back(Dep{
                    "", depKey, DepHashValue(b3), DepType::ParentContext});
            }
        }

        // Direct record — no deferred writes
        try {
            auto coldResult = cache->dbBackend->record(
                attrPath, attrValue, directDeps, !parentExpr);
            this->traceId = coldResult.traceId;
        } catch (std::exception & e) {
            debug("trace recording failed for '%s': %s", attrPathStr(), e.what());
        }

        // Store forced scalar children of derivation targets
        if (!origExpr && target->type() == nAttrs && cache->state.isDerivation(*target)) {
            for (auto & attr : *target->attrs()) {
                if (!attr.value->isThunk()) {
                    auto t = attr.value->type();
                    if (t == nString || t == nBool || t == nInt || t == nNull || t == nFloat) {
                        recordSiblingTrace(this, attr.name, *attr.value);
                    }
                }
            }
        }

        if (origExpr && std::holds_alternative<std::vector<Symbol>>(attrValue)) {
            materializeOrigExprAttrs(v, std::get<std::vector<Symbol>>(attrValue), target);
            return;
        }

        if (origExpr) {
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

// TracedExpr::eval — demand-driven dispatch (Adapton)
void TracedExpr::eval(EvalState & state, Env & env, Value & v)
{
    if (!cache->dbBackend) {
        if (origExpr) {
            origExpr->eval(state, *origEnv, v);
            state.forceValue(v, noPos);
        } else {
            auto * target = navigateToReal();
            state.forceValue(*target, noPos);
            v = *target;
        }
        return;
    }

    auto & db = *cache->dbBackend;

    auto attrPath = storeAttrPath();
    auto warmResult = db.verify(attrPath, cache->inputAccessors, cache->state);

    if (warmResult) {
        auto & [cachedValue, cachedTraceId] = *warmResult;
        this->traceId = cachedTraceId;

        // Handle failed values — reproduce error
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

        if (origExpr) {
            if (isLeafCached(cachedValue)) {
                materializeResult(v, cachedValue);
                replayTrace(cachedTraceId);
                return;
            }
            if (auto * attrs = std::get_if<std::vector<Symbol>>(&cachedValue)) {
                materializeOrigExprAttrs(v, *attrs);
                replayTrace(cachedTraceId);
                return;
            }
            // list_t or other → fresh evaluation needed
        } else {
            materializeResult(v, cachedValue);
            replayTrace(cachedTraceId);
            return;
        }
    }

    // Verify miss — fresh evaluation path
    nrTraceCacheMisses++;
    evaluateFresh(v);
}

// ── TraceCache public API ─────────────────────────────────────────────

static std::shared_ptr<TraceStore> makeDbBackend(
    const Hash & fingerprint, SymbolTable & symbols)
{
    int64_t contextHash;
    std::memcpy(&contextHash, fingerprint.hash, sizeof(contextHash));
    return std::make_shared<TraceStore>(symbols, contextHash);
}

TraceCache::TraceCache(
    std::optional<std::reference_wrapper<const Hash>> useCache,
    EvalState & state,
    RootLoader rootLoader,
    std::unordered_map<std::string, SourcePath> inputAccessors)
    : state(state)
    , rootLoader(rootLoader)
    , inputAccessors(std::move(inputAccessors))
{
    if (useCache) {
        try {
            dbBackend = makeDbBackend(*useCache, state.symbols);
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
        v->mkThunk(&state.baseEnv, new TracedExpr(this, 0, state.s.epsilon, nullptr));
        value = allocRootValue(v);
    }
    return *value;
}

} // namespace nix::eval_trace
