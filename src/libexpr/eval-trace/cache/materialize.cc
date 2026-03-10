#include "traced-expr.hh"

#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/cache/trace-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/root-tracker-scope.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <algorithm>
#include <cassert>
#include <unordered_map>

namespace nix::eval_trace {

// ── Helpers ──────────────────────────────────────────────────────────

/**
 * Detect Value* aliases in a container: multiple elements pointing to
 * the same Value. `getValuePtr(i)` returns the Value* for element i.
 * Populates aliasOf: -1 = canonical (first occurrence), >= 0 = alias of that index.
 * Clears aliasOf if no aliases are detected.
 */
static void detectAliases(std::vector<int16_t> & aliasOf, size_t n, auto getValuePtr)
{
    std::unordered_map<const Value *, size_t> valueToIdx;
    aliasOf.resize(n, -1);
    bool hasAliases = false;
    for (size_t i = 0; i < n; i++) {
        auto [it, inserted] = valueToIdx.emplace(getValuePtr(i), i);
        if (!inserted) {
            aliasOf[i] = static_cast<int16_t>(it->second);
            hasAliases = true;
        }
    }
    if (!hasAliases) aliasOf.clear();
}

/// Set canonicalSiblingIdx from an aliasOf vector during materialization.
static void annotateAlias(TracedExpr * te, const std::vector<int16_t> & aliasOf, size_t i)
{
    if (!aliasOf.empty())
        te->canonicalSiblingIdx = (aliasOf[i] >= 0)
            ? aliasOf[i] : static_cast<int16_t>(i);
}

bool isLeafCached(const CachedResult & v)
{
    return std::get_if<string_t>(&v)
        || std::get_if<bool>(&v)
        || std::get_if<int_t>(&v)
        || std::get_if<path_t>(&v)
        || std::get_if<null_t>(&v)
        || std::get_if<float_t>(&v)
        || std::get_if<std::vector<std::string>>(&v);
}

/**
 * Convert a forced Value into a CachedResult for storage/comparison.
 * The value must already be forced. For lists, suspends dep tracking
 * during the allStrings check to avoid mixing StructuredContent deps.
 */
CachedResult buildCachedResult(EvalState & st, Value & target)
{
    switch (target.type()) {
    case nString: {
        NixStringContext ctx;
        if (target.context()) {
            for (auto * elem : *target.context())
                ctx.insert(NixStringContextElem::parse(elem->view()));
        }
        return string_t{std::string(target.string_view()), std::move(ctx)};
    }
    case nBool:
        return target.boolean();
    case nInt:
        return int_t{NixInt{target.integer().value}};
    case nNull:
        return null_t{};
    case nFloat:
        return float_t{target.fpoint()};
    case nPath:
        return path_t{target.path().path.abs()};
    case nAttrs: {
        attrs_t result;
        for (auto & attr : *target.attrs())
            result.names.push_back(attr.name);
        std::sort(result.names.begin(), result.names.end(),
            [&](Symbol a, Symbol b) {
                return std::string_view(st.symbols[a])
                     < std::string_view(st.symbols[b]);
            });
        detectAliases(result.aliasOf, result.names.size(), [&](size_t i) {
            return target.attrs()->get(result.names[i])->value;
        });
        // Capture per-attr TracedData origins for cross-scope materialization.
        // Resolve interned IDs back to strings for serialization.
        if (target.attrs()->hasAnyTracedDataLayer()) {
            result.originIndices.resize(result.names.size(), -1);
            for (size_t i = 0; i < result.names.size(); i++) {
                auto * attr = target.attrs()->get(result.names[i]);
                if (!attr || !attr->pos.isTracedData()) continue;
                auto * origin = st.positions.originOfPtr(attr->pos);
                if (!origin) continue;
                auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
                if (!pr) continue;
                auto & pools = *st.traceCtx->pools;
                auto & df = resolveProvenanceRef(pools, *pr);
                // Resolve interned IDs to strings for storage
                auto depSource = std::string(pools.resolve(df.sourceId));
                auto depKey = std::string(pools.filePathPool.resolve(df.filePathId));
                auto dataPath = dataPathToJsonString(pools, df.dataPathId);
                // Deduplicate origins by resolved values
                int8_t idx = -1;
                for (size_t j = 0; j < result.origins.size(); j++) {
                    if (result.origins[j].depSource == depSource
                        && result.origins[j].depKey == depKey
                        && result.origins[j].dataPath == dataPath
                        && result.origins[j].format == df.format) {
                        idx = static_cast<int8_t>(j);
                        break;
                    }
                }
                if (idx < 0) {
                    idx = static_cast<int8_t>(result.origins.size());
                    result.origins.push_back({std::move(depSource), std::move(depKey),
                                              std::move(dataPath), df.format});
                }
                result.originIndices[i] = idx;
            }
            if (result.origins.empty()) {
                result.originIndices.clear();
            }
        }
        return result;
    }
    case nList: {
        list_t result{target.listSize()};
        auto view = target.listView();
        detectAliases(result.aliasOf, result.size, [&](size_t i) {
            return view[i];
        });
        return result;
    }
    case nThunk:
    case nFunction:
    case nExternal:
    case nFailed:
        return misc_t{};
    }
    unreachable();
}

// ── Origin re-internment and precomputed-keys helpers ────────────────

namespace {

struct PerOrigin {
    PosTable::OriginHandle handle;
    uint32_t attrCount = 0;
};

/**
 * Re-intern cached origins into PosTable so downstream shape dep recording
 * (SC #keys, #has:key, #type) works. Returns handles indexed by origin index.
 */
std::vector<PerOrigin> reinternOrigins(
    const attrs_t & attrs, EvalState & st, InterningPools & pools)
{
    pools.sessionSymbols = &st.symbols;
    std::vector<PerOrigin> handles;
    handles.reserve(attrs.origins.size());
    std::vector<uint32_t> counts(attrs.origins.size(), 0);
    for (auto idx : attrs.originIndices)
        if (idx >= 0) counts[idx]++;
    for (auto & orig : attrs.origins) {
        auto srcId = pools.intern<DepSourceId>(orig.depSource);
        auto fpId = pools.filePathPool.intern(orig.depKey);
        auto dpId = jsonStringToDataPathId(pools, orig.dataPath);
        auto handle = st.positions.addOriginHandle(
            allocateProvenanceRef(pools, srcId, fpId, dpId, orig.format),
            counts[&orig - attrs.origins.data()]);
        handles.push_back({handle, 0});
    }
    return handles;
}

/**
 * Register precomputed keys for each origin (mirrors ExprTracedData::eval()
 * in json-to-value.cc). Computes canonical #keys hash per origin group.
 */
void registerAllPrecomputedKeys(
    const attrs_t & attrs, const std::vector<PerOrigin> & originHandles,
    EvalState & st, InterningPools & pools)
{
    for (size_t oidx = 0; oidx < attrs.origins.size(); oidx++) {
        auto & orig = attrs.origins[oidx];
        std::vector<std::string> keys;
        for (size_t i = 0; i < attrs.names.size(); i++) {
            if (!attrs.originIndices.empty() && attrs.originIndices[i] == static_cast<int8_t>(oidx))
                keys.push_back(std::string(st.symbols[attrs.names[i]]));
        }
        auto nKeys = static_cast<uint32_t>(keys.size());
        auto keysHash = canonicalKeysHash(std::move(keys));
        auto srcId = pools.intern<DepSourceId>(orig.depSource);
        auto fpId = pools.filePathPool.intern(orig.depKey);
        auto dpId = jsonStringToDataPathId(pools, orig.dataPath);
        auto originOffset = originHandles[oidx].handle.offset;
        if (auto * scope = RootTrackerScope::current)
            scope->registerPrecomputedKeys(originOffset, PrecomputedKeysInfo{
            keysHash, nKeys, srcId, fpId, dpId, orig.format,
        });
    }
}

} // anonymous namespace

// ── ExprOrigChild::eval ──────────────────────────────────────────────

void ExprOrigChild::eval(EvalState & state, Env &, Value & v)
{
    // SharedParentResult must always be pre-populated by materializeOrigExprAttrs.
    // If this assertion fires, a code path created ExprOrigChild without
    // ensuring the parent was evaluated first.
    assert(shared->value && "ExprOrigChild: SharedParentResult not pre-populated");
    auto * attr = shared->value->attrs()->get(childName);
    if (!attr)
        throw Error("attribute '%s' not found in cached parent",
                    state.symbols[childName]);
    state.forceValue(*attr->value, noPos);
    v = *attr->value;
}

// ── TracedExpr materialization methods ───────────────────────────────

void TracedExpr::materializeOrigExprAttrs(
    Value & v, const attrs_t & attrs, Value * prePopulatedParent)
{
    auto * shared = new SharedParentResult();
    if (prePopulatedParent) {
        shared->value = prePopulatedParent;
    } else {
        // Always pre-populate: ExprOrigChild must never re-evaluate parentOrigExpr
        // from scratch. Without this, a trace-cache hit on a sibling-wrapped
        // TracedExpr materializes children whose first access triggers a full
        // re-evaluation of the parent expression (e.g., an entire NixOS closure),
        // producing thousands of duplicate derivation instantiations.
        shared->value = cache->state.allocValue();
        SuspendDepTracking suspend;
        lazy->origExpr->eval(cache->state, *lazy->origEnv, *shared->value);
        cache->state.forceAttrs(*shared->value, noPos,
            "while resolving cached attribute for hit materialization");
    }
    auto & st = cache->state;
    auto bindings = st.buildBindings(attrs.names.size());
    auto & pools = *st.traceCtx->pools;

    std::vector<PerOrigin> originHandles;
    if (!attrs.origins.empty())
        originHandles = reinternOrigins(attrs, st, pools);

    for (size_t i = 0; i < attrs.names.size(); i++) {
        auto childName = attrs.names[i];
        auto * childVal = st.allocValue();
        auto * wrapper = new TracedExpr(cache, childName, this);
        nrTracedExprCreated++;
        nrTracedExprFromOrigAttrs++;
        annotateAlias(wrapper, attrs.aliasOf, i);
        wrapper->ensureLazy().origExpr = new ExprOrigChild(lazy->origExpr, lazy->origEnv, childName, shared);
        wrapper->ensureLazy().origEnv = lazy->origEnv;
        installChildThunk(childVal, &st.baseEnv, wrapper);

        PosIdx pos = noPos;
        if (!attrs.originIndices.empty() && attrs.originIndices[i] >= 0) {
            auto oidx = attrs.originIndices[i];
            pos = st.positions.add(originHandles[oidx].handle, originHandles[oidx].attrCount++);
        }
        bindings.insert(childName, childVal, pos);
    }
    v.mkAttrs(bindings.finish());

    if (!attrs.origins.empty())
        registerAllPrecomputedKeys(attrs, originHandles, st, pools);
}

// Replay trace (Adapton change propagation)
void TracedExpr::replayTrace(TraceId traceId)
{
    if (!DependencyTracker::isActive())
        return;

    try {
        auto & pools = *cache->state.traceCtx->pools;
        auto deps = cache->dbBackend->loadFullTrace(traceId);
        for (auto & idep : deps) {
            auto resolved = cache->dbBackend->resolveDep(idep);
            DependencyTracker::recordToEpochLog(pools, resolved.type,
                resolved.source, resolved.key, resolved.expectedHash);
        }
    } catch (std::exception &) {
        // DB may be corrupt or trace may have been evicted -- skip
    }
}

// navigateToReal -- real tree navigation for fresh evaluation
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
                    auto * wrapper = new TracedExpr(cache, attr.name, parentEC);
                    nrTracedExprCreated++;
                    nrTracedExprFromOrigAttrs++;
                    wrapper->ensureLazy().origExpr = attr.value->thunk().expr;
                    auto * origEnv = attr.value->thunk().env;
                    wrapper->ensureLazy().origEnv = origEnv;
                    installChildThunk(attr.value, origEnv, wrapper);
                }
                // Non-thunk siblings are left alone. Each sibling's TracedExpr
                // handles its own tracing independently when accessed.
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

// materializeResult -- construct deep traced values (Adapton articulation points)
void TracedExpr::materializeResult(Value & v, const CachedResult & cached)
{
    auto & st = cache->state;

    if (auto * attrs = std::get_if<attrs_t>(&cached)) {
        auto bindings = st.buildBindings(attrs->names.size());
        auto & pools = *st.traceCtx->pools;

        std::vector<PerOrigin> originHandles;
        if (!attrs->origins.empty())
            originHandles = reinternOrigins(*attrs, st, pools);

        for (size_t i = 0; i < attrs->names.size(); i++) {
            auto childName = attrs->names[i];
            auto * childVal = st.allocValue();
            auto * child = new TracedExpr(cache, childName, this);
            nrTracedExprCreated++;
            nrTracedExprFromMaterialize++;
            annotateAlias(child, attrs->aliasOf, i);
            installChildThunk(childVal, &st.baseEnv, child);

            PosIdx pos = noPos;
            if (!attrs->originIndices.empty() && attrs->originIndices[i] >= 0) {
                auto oidx = attrs->originIndices[i];
                pos = st.positions.add(originHandles[oidx].handle, originHandles[oidx].attrCount++);
            }
            bindings.insert(childName, childVal, pos);
        }
        v.mkAttrs(bindings.finish());

        if (!attrs->origins.empty())
            registerAllPrecomputedKeys(*attrs, originHandles, st, pools);
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
            auto * child = new TracedExpr(cache, sym, this, /*isListElement=*/true);
            nrTracedExprCreated++;
            nrTracedExprFromMaterialize++;
            annotateAlias(child, lt->aliasOf, i);
            installChildThunk(elemVal, &st.baseEnv, child);
            list[i] = elemVal;
        }
        v.mkList(list);
    } else {
        // misc_t, failed_t, missing_t, placeholder_t -- fresh evaluation needed
        evaluateFresh(v);
    }
}

} // namespace nix::eval_trace
