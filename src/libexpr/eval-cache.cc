#include "nix/expr/eval-cache-db.hh"
#include "nix/util/environment-variables.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/expr/value-to-json.hh"

#include <algorithm>

namespace nix::eval_cache {

Counter nrEvalCacheHits;
Counter nrEvalCacheMisses;
Counter nrDepValidations;
Counter nrDepValidationsPassed;
Counter nrDepValidationsFailed;
Counter nrDepsChecked;
Counter nrCacheVerificationFailures;

// ── ExprCached struct definition ─────────────────────────────────────

struct ExprCached : Expr, gc
{
    EvalCache * cache;
    Symbol name;
    ExprCached * parentExpr; // GC-traced, nullptr for root
    bool isListElement;      // true = list index, false = attr access

    Expr * origExpr = nullptr;
    Env * origEnv = nullptr;

    /**
     * The SQLite attr_id for this attribute.
     * Set after warm path succeeds or cold path stores the result.
     */
    std::optional<AttrId> attrId;

    ExprCached(EvalCache * cache, AttrId /*unused*/, Symbol name, ExprCached * parentExpr,
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
        if (!parentExpr)
            return ""; // root
        std::vector<std::string> components;
        for (auto * e = this; e->parentExpr; e = e->parentExpr)
            components.push_back(std::string(cache->state.symbols[e->name]));
        std::reverse(components.begin(), components.end());
        return EvalCacheDb::buildAttrPath(components);
    }

    std::optional<AttrId> parentAttrId() const
    {
        if (!parentExpr) return std::nullopt;
        return parentExpr->attrId;
    }

    void evaluateCold(Value & v);
    Value * navigateToReal();
    void materializeValue(Value & v, const AttrValue & cached);
    void replayDepsToTracker(AttrId id);
    void storeForcedSibling(ExprCached * parentEC, Symbol siblingName, Value & v);
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
                SuspendFileLoadTracker suspend;
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

static bool isLeafCached(const AttrValue & v)
{
    return std::get_if<string_t>(&v)
        || std::get_if<bool>(&v)
        || std::get_if<int_t>(&v)
        || std::get_if<path_t>(&v)
        || std::get_if<null_t>(&v)
        || std::get_if<float_t>(&v)
        || std::get_if<std::vector<std::string>>(&v);
}

void ExprCached::sortChildNames(std::vector<Symbol> & names) const
{
    std::sort(names.begin(), names.end(),
        [&](Symbol a, Symbol b) {
            return std::string_view(cache->state.symbols[a])
                 < std::string_view(cache->state.symbols[b]);
        });
}

// ── ExprCached implementation ────────────────────────────────────────

void ExprCached::replayDepsToTracker(AttrId id)
{
    if (!FileLoadTracker::isActive())
        return;

    try {
        auto deps = cache->dbBackend->loadDepsForAttr(id);
        for (auto & dep : deps)
            FileLoadTracker::record(dep);
    } catch (std::exception &) {
        // DB may be corrupt or attr may have been evicted — skip
    }
}

void ExprCached::storeForcedSibling(ExprCached * parentEC, Symbol siblingName, Value & v)
{
    // Skip path values: they reference source trees needing SourceAccessor
    // context from cold evaluation for fetchToStore hash caching.
    if (v.type() == nPath)
        return;

    auto & db = *cache->dbBackend;
    auto nameStr = std::string(cache->state.symbols[siblingName]);

    auto * siblingEC = new ExprCached(cache, 0, siblingName, parentEC);
    auto siblingAttrPath = siblingEC->storeAttrPath();

    // If already stored (by a prior evaluateCold with full deps),
    // don't overwrite with this dep-less speculative entry.
    if (db.lookupAttr(siblingAttrPath))
        return;

    // Serialize value
    AttrValue attrValue;
    if (v.type() == nString) {
        NixStringContext ctx;
        if (v.context())
            for (auto * elem : *v.context())
                ctx.insert(NixStringContextElem::parse(elem->view()));
        attrValue = string_t{std::string(v.string_view()), std::move(ctx)};
    } else if (v.type() == nBool) {
        attrValue = v.boolean();
    } else if (v.type() == nInt) {
        attrValue = int_t{NixInt{v.integer().value}};
    } else if (v.type() == nNull) {
        attrValue = null_t{};
    } else if (v.type() == nFloat) {
        attrValue = float_t{v.fpoint()};
    } else if (v.type() == nAttrs) {
        std::vector<Symbol> childNames;
        for (auto & attr : *v.attrs())
            childNames.push_back(attr.name);
        sortChildNames(childNames);
        attrValue = childNames;
    } else {
        return; // list, function, external — skip
    }

    // Direct coldStore — no deferred writes needed with SQLite backend.
    // Parent's attrId may be nullopt (parent not yet stored); skip silently.
    if (!parentEC->attrId)
        return;

    try {
        siblingEC->attrId = db.coldStore(
            siblingAttrPath, attrValue, {}, parentEC->attrId, false);
    } catch (std::exception & e) {
        debug("storeForcedSibling failed for '%s': %s", siblingAttrPath, e.what());
        return;
    }

    try {
        if (v.type() == nAttrs) {
            if (cache->state.isDerivation(v)) {
                if (auto * a = v.attrs()->get(cache->state.s.drvPath))
                    if (a->value->isThunk()
                        && !dynamic_cast<ExprCached*>(a->value->thunk().expr))
                        try { cache->state.forceValue(*a->value, a->pos); }
                        catch (EvalError &) {}
                if (auto * a = v.attrs()->get(cache->state.s.outPath))
                    if (a->value->isThunk()
                        && !dynamic_cast<ExprCached*>(a->value->thunk().expr))
                        try { cache->state.forceValue(*a->value, a->pos); }
                        catch (EvalError &) {}
            }

            // Store forced leaf children (single level, no recursion).
            for (auto & attr : *v.attrs()) {
                if (attr.value->isThunk())
                    continue;
                auto childType = attr.value->type();
                if (childType == nPath || childType == nAttrs || childType == nList)
                    continue;

                AttrValue childValue;
                if (childType == nString) {
                    NixStringContext ctx;
                    if (attr.value->context())
                        for (auto * elem : *attr.value->context())
                            ctx.insert(NixStringContextElem::parse(elem->view()));
                    childValue = string_t{std::string(attr.value->string_view()), std::move(ctx)};
                } else if (childType == nBool) {
                    childValue = attr.value->boolean();
                } else if (childType == nInt) {
                    childValue = int_t{NixInt{attr.value->integer().value}};
                } else if (childType == nNull) {
                    childValue = null_t{};
                } else if (childType == nFloat) {
                    childValue = float_t{attr.value->fpoint()};
                } else {
                    continue;
                }

                auto * childEC = new ExprCached(cache, 0, attr.name, siblingEC);
                auto childAttrPath = childEC->storeAttrPath();
                try {
                    childEC->attrId = db.coldStore(
                        childAttrPath, std::move(childValue), {},
                        siblingEC->attrId, false);
                } catch (std::exception &) {}
            }

            // Wrap remaining thunk children with ExprCached origExpr wrappers
            for (auto & attr : *v.attrs()) {
                if (attr.value->isThunk()
                    && !dynamic_cast<ExprCached*>(attr.value->thunk().expr))
                {
                    auto * wrapper = new ExprCached(cache, 0, attr.name, siblingEC);
                    wrapper->origExpr = attr.value->thunk().expr;
                    wrapper->origEnv = attr.value->thunk().env;
                    attr.value->mkThunk(attr.value->thunk().env, wrapper);
                }
            }
        }
    } catch (std::exception &) {
        // Failed to process children — silently skip
    }
}

Value * ExprCached::navigateToReal()
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

    std::vector<ExprCached*> storeExprChain;
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
                    && !dynamic_cast<ExprCached*>(attr.value->thunk().expr))
                {
                    auto * wrapper = new ExprCached(cache, 0, attr.name, parentEC);
                    wrapper->origExpr = attr.value->thunk().expr;
                    wrapper->origEnv = attr.value->thunk().env;
                    attr.value->mkThunk(attr.value->thunk().env, wrapper);
                }
                else if (!attr.value->isThunk())
                {
                    storeForcedSibling(parentEC, attr.name, *attr.value);
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

void ExprCached::materializeValue(Value & v, const AttrValue & cached)
{
    auto & st = cache->state;

    if (auto * attrs = std::get_if<std::vector<Symbol>>(&cached)) {
        auto bindings = st.buildBindings(attrs->size());
        for (auto & childName : *attrs) {
            auto * childVal = st.allocValue();
            auto * child = new ExprCached(cache, 0, childName, this);
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
            auto * child = new ExprCached(cache, 0, sym, this, /*isListElement=*/true);
            elemVal->mkThunk(&st.baseEnv, child);
            list[i] = elemVal;
        }
        v.mkList(list);
    } else {
        // misc_t, failed_t, missing_t, placeholder_t — go cold
        evaluateCold(v);
    }
}

void ExprCached::evaluateCold(Value & v)
{
    FileLoadTracker tracker;

    Value * target;
    if (origExpr) {
        target = cache->state.allocValue();
        origExpr->eval(cache->state, *origEnv, *target);
    } else {
        if (parentExpr) {
            SuspendFileLoadTracker suspend;
            target = navigateToReal();
        } else {
            target = navigateToReal();
        }

        if (target->isThunk()) {
            if (auto * ec = dynamic_cast<ExprCached*>(target->thunk().expr)) {
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
        debug("setting '%s' to failed (cold path)", attrPathStr());
        if (cache->dbBackend) {
            auto attrPath = storeAttrPath();
            auto collectedDeps = tracker.collectDeps();

            std::vector<Dep> directDeps;
            for (auto & dep : collectedDeps)
                if (dep.type != DepType::ParentContext)
                    directDeps.push_back(dep);

            try {
                this->attrId = cache->dbBackend->coldStore(
                    attrPath, failed_t{}, directDeps,
                    parentAttrId(), !parentExpr);
            } catch (std::exception & e) {
                debug("cold store failed for '%s': %s", attrPathStr(), e.what());
            }
        }
        throw;
    }

    if (cache->dbBackend) {
        auto attrPath = storeAttrPath();
        auto collectedDeps = tracker.collectDeps();

        std::vector<Dep> directDeps;
        for (auto & dep : collectedDeps)
            if (dep.type != DepType::ParentContext)
                directDeps.push_back(dep);

        // Build the AttrValue for storage
        AttrValue attrValue;
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
            bool allStrings = true;
            for (size_t i = 0; i < target->listSize(); i++) {
                st.forceValue(*target->listView()[i], noPos);
                if (target->listView()[i]->type() != nString) { allStrings = false; break; }
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

        // Direct coldStore — no deferred writes
        try {
            this->attrId = cache->dbBackend->coldStore(
                attrPath, attrValue, directDeps,
                parentAttrId(), !parentExpr);
        } catch (std::exception & e) {
            debug("cold store failed for '%s': %s", attrPathStr(), e.what());
        }

        // Store forced scalar children of derivation targets
        if (!origExpr && target->type() == nAttrs && cache->state.isDerivation(*target)) {
            for (auto & attr : *target->attrs()) {
                if (!attr.value->isThunk()) {
                    auto t = attr.value->type();
                    if (t == nString || t == nBool || t == nInt || t == nNull || t == nFloat) {
                        storeForcedSibling(this, attr.name, *attr.value);
                    }
                }
            }
        }

        if (origExpr && std::holds_alternative<std::vector<Symbol>>(attrValue)) {
            auto & childNames = std::get<std::vector<Symbol>>(attrValue);
            auto * shared = new SharedParentResult();
            shared->value = target;
            auto bindings = cache->state.buildBindings(childNames.size());
            for (auto & childName : childNames) {
                auto * childVal = cache->state.allocValue();
                auto * wrapper = new ExprCached(cache, 0, childName, this);
                wrapper->origExpr = new ExprOrigChild(origExpr, origEnv, childName, shared);
                wrapper->origEnv = origEnv;
                childVal->mkThunk(&cache->state.baseEnv, wrapper);
                bindings.insert(childName, childVal, noPos);
            }
            v.mkAttrs(bindings.finish());
            return;
        }

        if (origExpr) {
            v = *target;
        } else if (std::holds_alternative<misc_t>(attrValue)
                || std::holds_alternative<missing_t>(attrValue)
                || std::holds_alternative<placeholder_t>(attrValue)) {
            v = *target;
        } else {
            materializeValue(v, attrValue);
        }
        return;
    } else {
        v = *target;
    }
}

void ExprCached::eval(EvalState & state, Env & env, Value & v)
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
    auto warmResult = db.warmPath(attrPath, cache->inputAccessors, cache->state, parentAttrId());

    if (warmResult) {
        auto & [cachedValue, cachedAttrId] = *warmResult;
        this->attrId = cachedAttrId;

        // Handle failed values — reproduce error
        if (std::get_if<failed_t>(&cachedValue)) {
            evaluateCold(v);
            return;
        }

        // Non-materializable types: re-evaluate
        if (std::get_if<misc_t>(&cachedValue)
            || std::get_if<missing_t>(&cachedValue)
            || std::get_if<placeholder_t>(&cachedValue)) {
            evaluateCold(v);
            return;
        }

        nrEvalCacheHits++;
        debug("eval cache hit for '%s'", attrPathStr());

        if (origExpr) {
            if (isLeafCached(cachedValue)) {
                materializeValue(v, cachedValue);
                replayDepsToTracker(cachedAttrId);
                return;
            }
            if (auto * attrs = std::get_if<std::vector<Symbol>>(&cachedValue)) {
                auto * shared = new SharedParentResult();
                auto bindings = cache->state.buildBindings(attrs->size());
                for (auto & childName : *attrs) {
                    auto * childVal = cache->state.allocValue();
                    auto * wrapper = new ExprCached(cache, 0, childName, this);
                    wrapper->origExpr = new ExprOrigChild(origExpr, origEnv, childName, shared);
                    wrapper->origEnv = origEnv;
                    childVal->mkThunk(&cache->state.baseEnv, wrapper);
                    bindings.insert(childName, childVal, noPos);
                }
                v.mkAttrs(bindings.finish());
                replayDepsToTracker(cachedAttrId);
                return;
            }
            // list_t or other → cold
        } else {
            materializeValue(v, cachedValue);
            replayDepsToTracker(cachedAttrId);
            return;
        }
    }

    // Cold path
    nrEvalCacheMisses++;
    debug("eval cache miss for '%s'", attrPathStr());
    evaluateCold(v);
}

// ── EvalCache public API ─────────────────────────────────────────────

static std::shared_ptr<EvalCacheDb> makeDbBackend(
    const Hash & fingerprint, SymbolTable & symbols)
{
    int64_t contextHash;
    std::memcpy(&contextHash, fingerprint.hash, sizeof(contextHash));
    return std::make_shared<EvalCacheDb>(symbols, contextHash);
}

EvalCache::EvalCache(
    std::optional<std::reference_wrapper<const Hash>> useCache,
    EvalState & state,
    RootLoader rootLoader,
    std::map<std::string, SourcePath> inputAccessors)
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

Value * EvalCache::getOrEvaluateRoot()
{
    if (!realRoot) {
        debug("getting root value via rootLoader");
        realRoot = allocRootValue(rootLoader());
    }
    return *realRoot;
}

Value * EvalCache::getRootValue()
{
    if (!value) {
        auto * v = state.allocValue();
        v->mkThunk(&state.baseEnv, new ExprCached(this, 0, state.s.epsilon, nullptr));
        value = allocRootValue(v);
    }
    return *value;
}

} // namespace nix::eval_cache
