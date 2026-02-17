#include "nix/expr/eval-cache-store.hh"
#include "nix/util/environment-variables.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/expr/value-to-json.hh"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace nix::eval_cache {

Counter nrEvalCacheHits;
Counter nrEvalCacheMisses;
Counter nrDepValidations;
Counter nrDepValidationsPassed;
Counter nrDepValidationsFailed;
Counter nrDepsChecked;
Counter nrCacheVerificationFailures;

// ── ExprCached struct definition ─────────────────────────────────────

/**
 * An Expr node whose eval() reads from the eval cache (warm path) or
 * evaluates the real thunk and stores the result (cold path). GC-allocated
 * so it can be embedded in Values as a thunk. CLI commands use standard
 * forceValue() + Value operations instead of cache-specific APIs.
 *
 * During cold-path evaluation, navigateToReal() wraps sibling thunks
 * with ExprCached, enabling partial tree invalidation: unchanged siblings
 * are served from cache while the target attribute is re-evaluated.
 */
struct ExprCached : Expr, gc
{
    EvalCache * cache;
    Symbol name;
    ExprCached * parentExpr; // GC-traced, nullptr for root
    bool isListElement;      // true = list index, false = attr access
    /**
     * For side-effect sibling wrappers created during navigateToReal.
     *
     * When navigateToReal traverses the real evaluation tree to reach a
     * target attribute (e.g., hello.drvPath), it wraps sibling thunks
     * (e.g., stdenv) with ExprCached wrappers that have origExpr/origEnv
     * set. When these siblings are later forced as side effects (e.g.,
     * derivationStrictInternal forcing stdenv), the wrapper's eval()
     * tries the warm cache first, then falls back to evaluating the
     * original thunk expression directly — without navigateToReal.
     *
     * Warm cache for FullAttrs uses ExprOrigChild as the origExpr of
     * materialized children. ExprOrigChild resolves children by
     * evaluating the parent's original expression directly (shared
     * across siblings) rather than using navigateToReal, which would
     * cycle through the materialized parent and hit a blackhole.
     *
     * Cold path still produces real values (v = *target) with children
     * wrapped as origExpr ExprCached thunks for future warm cache hits.
     */
    Expr * origExpr = nullptr;
    Env * origEnv = nullptr;

    /**
     * The trace store path for this attribute.
     * Set after warm path succeeds or cold path stores the result.
     */
    std::optional<StorePath> tracePath;

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

    /**
     * Build a human-readable attr path string from the ExprCached parent chain.
     */
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

    // ── Store backend methods ────────────────────────────────────

    /**
     * Build the null-byte-separated attr path for the store backend.
     */
    std::string storeAttrPath() const
    {
        if (!parentExpr)
            return ""; // root
        std::vector<std::string> components;
        for (auto * e = this; e->parentExpr; e = e->parentExpr)
            components.push_back(std::string(cache->state.symbols[e->name]));
        std::reverse(components.begin(), components.end());
        return EvalCacheStore::buildAttrPath(components);
    }

    /**
     * Get the parent's trace store path.
     */
    std::optional<StorePath> parentTracePath() const
    {
        if (!parentExpr) return std::nullopt;
        return parentExpr->tracePath;
    }

    void evaluateCold(Value & v);
    Value * navigateToReal();
    void materializeValue(Value & v, const AttrValue & cached);
    void replayDepsToTracker(const StorePath & tracePath);
    void storeForcedSibling(ExprCached * parentEC, Symbol siblingName, Value & v);
    void sortChildNames(std::vector<Symbol> & names) const;
};

// ── DeferredColdStore::execute() ──────────────────────────────────────

void DeferredColdStore::execute()
{
    std::optional<StorePath> parentTrace;
    if (parentSrc) {
        parentTrace = parentSrc->tracePath;
        if (!parentTrace) return;
    }
    auto traceP = sb->coldStore(attrPath, name, value, deps, parentTrace, isRoot);
    target->tracePath = traceP;
}

// ── Support types (SharedParentResult, ExprOrigChild) ────────────────

/**
 * GC-allocated container for a shared lazy parent evaluation result.
 * Multiple ExprOrigChild instances share this so the parent is
 * evaluated at most once.
 */
struct SharedParentResult : gc
{
    Value * value = nullptr;
};

/**
 * Expr subclass for children of a materialized origExpr FullAttrs.
 *
 * Instead of using navigateToReal (which would cycle through the
 * materialized parent and hit a blackhole), this resolves the child
 * by evaluating the parent's original expression and looking up
 * the child name in the resulting real attrset.
 */
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
            // Suspend dep recording while evaluating the parent expression.
            // The parent (e.g., buildPackages = all of nixpkgs) can record
            // 10K+ file deps that would bloat this child's dep set.
            // The child's own deps are recorded after the guard is destroyed.
            // Parent dep chains in the store handle invalidation chain-of-custody.
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

/**
 * Returns true if the AttrValue is a leaf that can be served from cache
 * without navigateToReal (scalars, paths, string lists).
 */
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

void ExprCached::replayDepsToTracker(const StorePath & traceP)
{
    if (!FileLoadTracker::isActive())
        return;

    auto & sb = *cache->storeBackend;
    try {
        // Load deps from the trace's dep set blob (two-object model).
        // Uses depSetCache to avoid redundant decompression if validation
        // already loaded the same dep set.
        auto deps = sb.loadDepsForTrace(traceP);
        // Replay only direct deps from this trace — do NOT recurse into
        // parent traces (parent deps are already captured in the parent's
        // FileLoadTracker).
        for (auto & dep : deps) {
            FileLoadTracker::record(dep);
        }
    } catch (std::exception &) {
        // trace or dep set blob may have been GC'd — skip
    }
}

void ExprCached::storeForcedSibling(ExprCached * parentEC, Symbol siblingName, Value & v)
{
    // Skip path values: they reference source trees needing SourceAccessor
    // context from cold evaluation for fetchToStore hash caching.
    if (v.type() == nPath)
        return;

    auto & sb = *cache->storeBackend;
    auto nameStr = std::string(cache->state.symbols[siblingName]);

    // Create a GC-allocated ExprCached node for the sibling so children
    // have correct parentExpr chain for storeAttrPath() / parentEvalDrvPath()
    auto * siblingEC = new ExprCached(cache, 0, siblingName, parentEC);
    auto siblingAttrPath = siblingEC->storeAttrPath();

    // If already staged or stored (by a prior evaluateCold with full deps),
    // don't overwrite with this dep-less speculative entry.
    if (sb.isStaged(siblingAttrPath) || sb.index.lookup(sb.contextHash, siblingAttrPath, sb.store))
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

    // Defer sibling coldStore — parent's tracePath resolved at flush time.
    // DeferredColdStore is GC-allocated, keeping siblingEC and parentEC
    // reachable by the Boehm GC (prevents use-after-free in flush()).
    auto * sbPtr = &sb;
    sbPtr->defer(siblingAttrPath, new DeferredColdStore{
        {}, sbPtr, siblingEC, parentEC,
        std::string(siblingAttrPath), nameStr,
        attrValue, {}, false});

    try {
        if (v.type() == nAttrs) {
            // For derivations, eagerly force drvPath/outPath so they get
            // stored as leaf children. Use try-catch because nixpkgs wraps
            // these with license assertions (extendDerivation). Guard against
            // double-wrapping via dynamic_cast (derivation.nix Value* aliasing:
            // default.out and default share the same Value*).
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
            // Skip path, attrset, list — only simple scalars/strings.
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

                auto childNameStr = std::string(cache->state.symbols[attr.name]);
                auto * childEC = new ExprCached(cache, 0, attr.name, siblingEC);
                auto childAttrPath = childEC->storeAttrPath();
                sbPtr->defer(childAttrPath, new DeferredColdStore{
                    {}, sbPtr, childEC, siblingEC,
                    std::string(childAttrPath), childNameStr,
                    std::move(childValue), {}, false});
            }

            // Wrap remaining thunk children with ExprCached origExpr wrappers
            // so they get cached when later forced as side effects.
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
    // Build path with list-index metadata from parent chain
    struct PathStep {
        Symbol sym;
        bool isListElement;
    };
    std::vector<PathStep> path;
    for (auto * e = this; e->parentExpr; e = e->parentExpr)
        path.push_back({e->name, e->isListElement});
    std::reverse(path.begin(), path.end());

    auto * v = cache->getOrEvaluateRoot();

    // Build ExprCached chain from root to this.
    // exprChain[i] is the parent ExprCached for siblings at path step i.
    std::vector<ExprCached*> storeExprChain;
    for (auto * e = this; e; e = e->parentExpr)
        storeExprChain.push_back(e);
    std::reverse(storeExprChain.begin(), storeExprChain.end());
    size_t pathStep = 0;

    for (auto & step : path) {
        if (step.isListElement) {
            // List indexing — force the value, parse index, direct access
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
            // Attrset access
            cache->state.forceAttrs(*v, noPos, "while navigating to cached attribute");

            // Wrap sibling thunks and store already-forced siblings.
            // When siblings are later forced as side effects, the wrapper's
            // eval() stores results in the cache. Already-forced siblings
            // are stored speculatively with parent deps inherited via inputDrvs.
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
            child->tracePath = std::nullopt; // Will be resolved on access
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

        // navigateToReal() may have wrapped this value with ExprCached
        // during a prior sibling wrapping pass. Unwrap it so forceValue
        // evaluates the original expression directly, capturing deps in
        // THIS tracker rather than in the wrapper's nested tracker.
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

        // For origExpr wrappers: exit the blackhole on v early.
        //
        // v (the Value* in the real evaluation tree, e.g. pkgs.perl) is
        // currently blackholed from the caller's forceValue(v). The
        // derivationStrict call below can trigger deep dependency chains
        // through the nixpkgs fixed-point (self), e.g.:
        //   perl.derivationStrict → libxcrypt → buildPackages.perl → self.perl
        // If self.perl is still blackholed, this causes infinite recursion.
        //
        // In normal (non-cached) evaluation, the blackhole exits as soon as
        // the thunk expression produces a result — before derivationStrict
        // runs. We replicate that by setting v = *target now. v will be
        // overwritten with the final materialized value at the end.
        if (origExpr)
            v = *target;

        // derivation.nix uses `let strict = derivationStrict drvAttrs` lazily,
        // so merely forcing the derivation attrset to WHNF does NOT call
        // derivationStrict. Force drvPath to trigger it, ensuring deps from
        // env var processing (e.g., readFile via buildCommand string
        // interpolation) are captured in this attribute's dep set.
        //
        // Only do this for the main navigation target (!origExpr). For
        // origExpr wrappers (side-effect siblings wrapped by navigateToReal),
        // skip eager drvPath forcing: it triggers derivationStrict chains
        // that don't exist in normal (non-cached) evaluation, causing
        // infinite recursion in nixpkgs' fixed-point package sets where
        // buildPackages = self on native builds. Side-effect siblings will
        // have derivationStrict triggered naturally when string coercion
        // needs outPath, matching normal evaluation order.
        if (!origExpr && target->type() == nAttrs && st.isDerivation(*target)) {
            if (auto * dp = target->attrs()->get(st.s.drvPath))
                st.forceValue(*dp->value, noPos);
        }
    } catch (EvalError &) {
        debug("setting '%s' to failed (cold path)", attrPathStr());
        if (cache->storeBackend) {
            auto attrPath = storeAttrPath();
            auto nameStr = std::string(st.symbols[name]);
            auto collectedDeps = tracker.collectDeps();

            // Filter ParentContext
            std::vector<Dep> directDeps;
            for (auto & dep : collectedDeps)
                if (dep.type != DepType::ParentContext)
                    directDeps.push_back(dep);

            auto * sb = cache->storeBackend.get();
            auto safeName = nameStr.empty() ? std::string("root") : nameStr;
            bool isRoot = !parentExpr;
            ExprCached * self = this;
            ExprCached * parent = parentExpr;

            sb->defer(attrPath, new DeferredColdStore{
                {}, sb, self, parent,
                std::string(attrPath), safeName,
                failed_t{}, std::move(directDeps), isRoot});
        }
        throw;
    }

    if (cache->storeBackend) {
        auto attrPath = storeAttrPath();
        auto nameStr = std::string(st.symbols[name]);
        auto collectedDeps = tracker.collectDeps();

        // Filter ParentContext — parent dep inheritance is via trace references
        std::vector<Dep> directDeps;
        for (auto & dep : collectedDeps)
            if (dep.type != DepType::ParentContext)
                directDeps.push_back(dep);

        // Build the AttrValue for serialization
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

        {
            auto * sb = cache->storeBackend.get();
            auto safeName = nameStr.empty() ? std::string("root") : nameStr;
            bool isRoot = !parentExpr;
            ExprCached * self = this;
            ExprCached * parent = parentExpr;

            // Copy for lambda; original stays on stack for materializeValue
            auto deferredValue = attrValue;
            auto deferredDeps = std::move(directDeps);

            sb->defer(attrPath, new DeferredColdStore{
                {}, sb, self, parent,
                std::string(attrPath), safeName,
                std::move(deferredValue), std::move(deferredDeps), isRoot});
        }

        // Store forced scalar children of derivation targets before
        // materializeValue/ExprOrigChild replaces them with thunks.
        // evaluateCold eagerly forces drvPath for derivation targets (the
        // !origExpr && isDerivation guard above), but materializeValue and
        // ExprOrigChild replace ALL children with thunks, losing the forced
        // values. Only apply to derivations — non-derivation attrsets (e.g.,
        // flake inputs) must NOT be cached as scalars because their warm path
        // would bypass SourceAccessor setup (AllowListSourceAccessor).
        // Only store scalars — NOT attrsets, which would trigger thunk
        // wrapping in storeForcedSibling that modifies the real tree.
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
            // origExpr wrapper with attrset result: create children
            // with ExprOrigChild to maintain cache chain WITHOUT
            // navigateToReal. materializeValue would create
            // ExprCached children without origExpr in the real tree,
            // causing subsequent navigateToReal calls to hit blackholed
            // Values (infinite recursion). ExprOrigChild resolves
            // children via the parent's origExpr instead.
            //
            // Pre-fill SharedParentResult with already-evaluated target
            // so ExprOrigChild doesn't re-evaluate the parent.
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
            // origExpr with non-attrset result: return real value
            v = *target;
        } else if (std::holds_alternative<misc_t>(attrValue)
                || std::holds_alternative<missing_t>(attrValue)
                || std::holds_alternative<placeholder_t>(attrValue)) {
            // Non-materializable types: use real value directly.
            // materializeValue for these calls evaluateCold(v) which
            // would create infinite recursion.
            v = *target;
        } else {
            // Main navigation path: materialize ExprCached children to
            // maintain cache chain for deeper attribute access.
            materializeValue(v, attrValue);
        }
        return;
    } else {
        v = *target;
    }
}

void ExprCached::eval(EvalState & state, Env & env, Value & v)
{
    if (!cache->storeBackend) {
        // No cache backend — evaluate directly
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

    auto & sb = *cache->storeBackend;

    // Try warm path. parentTracePath() (defined at line ~117) returns
    // the parent ExprCached's tracePath — set synchronously when the parent
    // is warm-served or recovered, before eval() returns. For root attrs
    // (no parent), returns nullopt → Phase 2 recovery skipped.
    // This enables Phase 2 parent-aware recovery in EvalCacheStore::recovery().
    auto attrPath = storeAttrPath();
    auto warmResult = sb.warmPath(attrPath, cache->inputAccessors, cache->state, parentTracePath());

    if (warmResult) {
        auto & [cachedValue, traceP] = *warmResult;
        tracePath = traceP;

        // Handle failed values — reproduce error
        if (std::get_if<failed_t>(&cachedValue)) {
            evaluateCold(v);
            return;
        }

        // Non-materializable types (functions, externals): re-evaluate.
        // materializeValue for these calls evaluateCold → infinite recursion.
        if (std::get_if<misc_t>(&cachedValue)
            || std::get_if<missing_t>(&cachedValue)
            || std::get_if<placeholder_t>(&cachedValue)) {
            evaluateCold(v);
            return;
        }

        nrEvalCacheHits++;
        debug("eval cache hit for '%s'", attrPathStr());

        // For origExpr wrappers: serve leaf or FullAttrs
        if (origExpr) {
            if (isLeafCached(cachedValue)) {
                materializeValue(v, cachedValue);
                replayDepsToTracker(traceP);
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
                replayDepsToTracker(traceP);
                return;
            }
            // list_t or other → cold
        } else {
            // Non-origExpr: materialize value
            materializeValue(v, cachedValue);
            replayDepsToTracker(traceP);
            return;
        }
    }

    // Cold path
    nrEvalCacheMisses++;
    debug("eval cache miss for '%s'", attrPathStr());
    evaluateCold(v);
}

// ── EvalCache public API ─────────────────────────────────────────────

static std::shared_ptr<EvalCacheStore> makeStoreBackend(
    Store & store, const Hash & fingerprint, SymbolTable & symbols)
{
    // Compute context hash from fingerprint (first 8 bytes as int64)
    int64_t contextHash;
    std::memcpy(&contextHash, fingerprint.hash, sizeof(contextHash));

    return std::make_shared<EvalCacheStore>(store, symbols, contextHash);
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
            storeBackend = makeStoreBackend(*state.store, *useCache, state.symbols);
        } catch (...) {
            // If we can't create the cache (e.g., unwritable cache dir in
            // sandbox), degrade gracefully to uncached mode.
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
