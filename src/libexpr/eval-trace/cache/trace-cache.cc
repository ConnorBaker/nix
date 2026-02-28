#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/finally.hh"
#include "nix/expr/eval-trace/cache/trace-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/store/store-api.hh"
#include "nix/store/derivations.hh"
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

// TracedExpr creation/forcing breakdown
Counter nrTracedExprCreated;
Counter nrTracedExprFromMaterialize;
Counter nrTracedExprFromOrigAttrs;
Counter nrTracedExprFromDataFile;
Counter nrTracedExprForced;
Counter nrLazyStateAllocated;

// Data-file node type breakdown
Counter nrDataFileScalarChildren;
Counter nrDataFileContainerChildren;

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

    /**
     * Lazy-initialized state for fields only needed when eval() or
     * navigateToReal() runs. Reduces per-child allocation from ~100 to ~40 bytes.
     * For unaccessed children (the vast majority), LazyState is never allocated.
     */
    struct LazyState : gc {
        Expr * origExpr = nullptr;
        Env * origEnv = nullptr;
        std::optional<TraceId> traceId;
        mutable std::optional<std::string> cachedStoreAttrPath;
    };
    LazyState * lazy = nullptr;

    LazyState & ensureLazy() {
        if (!lazy) { lazy = new LazyState{}; nrLazyStateAllocated++; }
        return *lazy;
    }

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
        if (lazy && lazy->cachedStoreAttrPath)
            return *lazy->cachedStoreAttrPath;
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
        const_cast<TracedExpr *>(this)->ensureLazy().cachedStoreAttrPath = result;
        return result;
    }

    std::optional<TraceId> parentTraceId() const
    {
        if (!parentExpr) return std::nullopt;
        return parentExpr->lazy ? parentExpr->lazy->traceId : std::nullopt;
    }

    void evaluateFresh(Value & v);
    Value * navigateToReal();
    void materializeResult(Value & v, const CachedResult & cached);
    void materializeOrigExprAttrs(Value & v, const attrs_t & attrs,
                                   Value * prePopulatedParent = nullptr);
    void replayTrace(TraceId traceId);
};

// ── SiblingAccessTracker (per-accessed-sibling ParentContext deps) ────

/**
 * RAII tracker for recording which siblings a navigated child accesses
 * during evaluation. Thread-local linked list enables nesting (inner
 * child evaluations isolate from outer).
 *
 * Used to create per-sibling ParentContext deps instead of a single
 * whole-parent dep, avoiding cascading invalidation when unrelated
 * siblings are added/removed. Each navigated child's evaluateFresh()
 * creates a tracker scoped to its parentExpr; sibling TracedExpr::eval()
 * calls maybeRecord() at completion to register themselves.
 */
struct SiblingAccessTracker
{
    TracedExpr * parentExpr;
    std::vector<std::pair<std::string, Hash>> accesses;
    std::unordered_set<std::string> seen; // dedup by attrPath

    static thread_local SiblingAccessTracker * current;
    SiblingAccessTracker * previous;

    explicit SiblingAccessTracker(TracedExpr * parent)
        : parentExpr(parent), previous(current) { current = this; }
    ~SiblingAccessTracker() { current = previous; }

    SiblingAccessTracker(const SiblingAccessTracker &) = delete;
    SiblingAccessTracker & operator=(const SiblingAccessTracker &) = delete;

    void recordAccess(const std::string & attrPath, const Hash & traceHash)
    {
        if (seen.insert(attrPath).second)
            accesses.emplace_back(attrPath, traceHash);
    }

    /**
     * Called at the end of TracedExpr::eval() to record a sibling access.
     * Only records if:
     * - A tracker is active (some ancestor is being evaluated fresh)
     * - The sibling shares the same parentExpr as the tracker
     * - The sibling has a valid traceId (eval completed successfully)
     * - The sibling's current trace hash is available in the DB
     */
    static void maybeRecord(TracedExpr * expr, TraceStore & db)
    {
        if (!current) return;
        if (expr->parentExpr != current->parentExpr) return;
        if (!expr->lazy || !expr->lazy->traceId) return;
        try {
            auto path = expr->storeAttrPath();
            auto hash = db.getCurrentTraceHash(path);
            if (hash) current->recordAccess(path, *hash);
        } catch (...) {
            // DB error during sibling recording — skip silently
        }
    }
};
thread_local SiblingAccessTracker * SiblingAccessTracker::current = nullptr;

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

/**
 * Convert a forced Value into a CachedResult for storage/comparison.
 * The value must already be forced. For lists, suspends dep tracking
 * during the allStrings check to avoid mixing StructuredContent deps.
 */
static CachedResult buildCachedResult(EvalState & st, Value & target)
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
                auto * df = resolveProvenanceRef(*pr);
                if (!df) continue;
                // Resolve interned IDs to strings for storage
                auto depSource = std::string(resolveDepSource(df->sourceId));
                auto depKey = std::string(resolveFilePath(df->filePathId));
                auto dataPath = dataPathToJsonString(df->dataPathId);
                // Deduplicate origins by resolved values
                int8_t idx = -1;
                for (size_t j = 0; j < result.origins.size(); j++) {
                    if (result.origins[j].depSource == depSource
                        && result.origins[j].depKey == depKey
                        && result.origins[j].dataPath == dataPath
                        && result.origins[j].format == df->format) {
                        idx = static_cast<int8_t>(j);
                        break;
                    }
                }
                if (idx < 0) {
                    idx = static_cast<int8_t>(result.origins.size());
                    result.origins.push_back({std::move(depSource), std::move(depKey),
                                              std::move(dataPath), df->format});
                }
                result.originIndices[i] = idx;
            }
            if (result.origins.empty()) {
                result.originIndices.clear();
            }
        }
        return result;
    }
    case nList:
        return list_t{target.listSize()};
    case nThunk:
    case nFunction:
    case nExternal:
        return misc_t{};
    }
    unreachable();
}

// ── TracedExpr implementation (Adapton articulation points) ──────────

void TracedExpr::materializeOrigExprAttrs(
    Value & v, const attrs_t & attrs, Value * prePopulatedParent)
{
    auto * shared = new SharedParentResult();
    if (prePopulatedParent)
        shared->value = prePopulatedParent;
    auto & st = cache->state;
    auto bindings = st.buildBindings(attrs.names.size());

    // When origins are present, re-intern and register with PosTable
    // so that downstream shape dep recording (SC #keys, #has:key, #type) works.
    struct PerOrigin {
        PosTable::OriginHandle handle;
        uint32_t attrCount = 0;
    };
    std::vector<PerOrigin> originHandles;
    if (!attrs.origins.empty()) {
        initSessionSymbols(st.symbols);
        originHandles.reserve(attrs.origins.size());
        std::vector<uint32_t> counts(attrs.origins.size(), 0);
        for (auto idx : attrs.originIndices)
            if (idx >= 0) counts[idx]++;
        for (auto & orig : attrs.origins) {
            auto srcId = internDepSource(orig.depSource);
            auto fpId = internFilePath(orig.depKey);
            auto dpId = jsonStringToDataPathId(orig.dataPath);
            auto handle = st.positions.addOriginHandle(
                allocateProvenanceRef(srcId, fpId, dpId, orig.format),
                counts[&orig - attrs.origins.data()]);
            originHandles.push_back({handle, 0});
        }
    }

    for (size_t i = 0; i < attrs.names.size(); i++) {
        auto childName = attrs.names[i];
        auto * childVal = st.allocValue();
        auto * wrapper = new TracedExpr(cache, 0, childName, this);
        nrTracedExprCreated++;
        nrTracedExprFromOrigAttrs++;
        wrapper->ensureLazy().origExpr = new ExprOrigChild(lazy->origExpr, lazy->origEnv, childName, shared);
        wrapper->ensureLazy().origEnv = lazy->origEnv;
        childVal->mkThunk(&st.baseEnv, wrapper);

        PosIdx pos = noPos;
        if (!attrs.originIndices.empty() && attrs.originIndices[i] >= 0) {
            auto oidx = attrs.originIndices[i];
            pos = st.positions.add(originHandles[oidx].handle, originHandles[oidx].attrCount++);
        }
        bindings.insert(childName, childVal, pos);
    }
    v.mkAttrs(bindings.finish());

    // Register precomputed keys per origin (mirrors ExprTracedData::eval() in json-to-value.cc)
    if (!attrs.origins.empty()) {
        for (size_t oidx = 0; oidx < attrs.origins.size(); oidx++) {
            auto & orig = attrs.origins[oidx];
            auto fmt = parseStructuredFormat(orig.format);
            if (!fmt) continue;
            // Collect sorted key names for this origin
            std::vector<std::string> keys;
            for (size_t i = 0; i < attrs.names.size(); i++) {
                if (!attrs.originIndices.empty() && attrs.originIndices[i] == static_cast<int8_t>(oidx))
                    keys.push_back(std::string(st.symbols[attrs.names[i]]));
            }
            std::sort(keys.begin(), keys.end());
            std::string canonical;
            for (size_t i = 0; i < keys.size(); i++) {
                if (i > 0) canonical += '\0';
                canonical += keys[i];
            }
            auto keysHash = depHash(canonical);
            auto srcId = internDepSource(orig.depSource);
            auto fpId = internFilePath(orig.depKey);
            auto dpId = jsonStringToDataPathId(orig.dataPath);
            auto originOffset = originHandles[oidx].handle.offset;
            registerPrecomputedKeys(originOffset, PrecomputedKeysInfo{
                keysHash,
                static_cast<uint32_t>(keys.size()),
                srcId, fpId, dpId, *fmt,
            });
        }
    }
}

// Replay trace (Adapton change propagation)
void TracedExpr::replayTrace(TraceId traceId)
{
    if (!DependencyTracker::isActive())
        return;

    try {
        auto deps = cache->dbBackend->loadFullTrace(traceId);
        for (auto & dep : deps)
            DependencyTracker::recordReplay(dep);
    } catch (std::exception &) {
        // DB may be corrupt or trace may have been evicted — skip
    }
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
                    nrTracedExprCreated++;
                    nrTracedExprFromOrigAttrs++;
                    wrapper->ensureLazy().origExpr = attr.value->thunk().expr;
                    wrapper->ensureLazy().origEnv = attr.value->thunk().env;
                    attr.value->mkThunk(attr.value->thunk().env, wrapper);
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

// materializeResult — construct deep traced values (Adapton articulation points)
void TracedExpr::materializeResult(Value & v, const CachedResult & cached)
{
    auto & st = cache->state;

    if (auto * attrs = std::get_if<attrs_t>(&cached)) {
        auto bindings = st.buildBindings(attrs->names.size());

        // When origins are present, re-intern and register with PosTable for TracedData provenance
        struct PerOrigin {
            PosTable::OriginHandle handle;
            uint32_t attrCount = 0;
        };
        std::vector<PerOrigin> originHandles;
        if (!attrs->origins.empty()) {
            initSessionSymbols(st.symbols);
            originHandles.reserve(attrs->origins.size());
            std::vector<uint32_t> counts(attrs->origins.size(), 0);
            for (auto idx : attrs->originIndices)
                if (idx >= 0) counts[idx]++;
            for (auto & orig : attrs->origins) {
                auto srcId = internDepSource(orig.depSource);
                auto fpId = internFilePath(orig.depKey);
                auto dpId = jsonStringToDataPathId(orig.dataPath);
                auto handle = st.positions.addOriginHandle(
                    allocateProvenanceRef(srcId, fpId, dpId, orig.format),
                    counts[&orig - attrs->origins.data()]);
                originHandles.push_back({handle, 0});
            }
        }

        for (size_t i = 0; i < attrs->names.size(); i++) {
            auto childName = attrs->names[i];
            auto * childVal = st.allocValue();
            auto * child = new TracedExpr(cache, 0, childName, this);
            nrTracedExprCreated++;
            nrTracedExprFromMaterialize++;
            childVal->mkThunk(&st.baseEnv, child);

            PosIdx pos = noPos;
            if (!attrs->originIndices.empty() && attrs->originIndices[i] >= 0) {
                auto oidx = attrs->originIndices[i];
                pos = st.positions.add(originHandles[oidx].handle, originHandles[oidx].attrCount++);
            }
            bindings.insert(childName, childVal, pos);
        }
        v.mkAttrs(bindings.finish());

        // Register precomputed keys per origin
        if (!attrs->origins.empty()) {
            for (size_t oidx = 0; oidx < attrs->origins.size(); oidx++) {
                auto & orig = attrs->origins[oidx];
                auto fmt = parseStructuredFormat(orig.format);
                if (!fmt) continue;
                std::vector<std::string> keys;
                for (size_t i = 0; i < attrs->names.size(); i++) {
                    if (!attrs->originIndices.empty() && attrs->originIndices[i] == static_cast<int8_t>(oidx))
                        keys.push_back(std::string(st.symbols[attrs->names[i]]));
                }
                std::sort(keys.begin(), keys.end());
                std::string canonical;
                for (size_t i = 0; i < keys.size(); i++) {
                    if (i > 0) canonical += '\0';
                    canonical += keys[i];
                }
                auto keysHash = depHash(canonical);
                auto srcId = internDepSource(orig.depSource);
                auto fpId = internFilePath(orig.depKey);
                auto dpId = jsonStringToDataPathId(orig.dataPath);
                auto originOffset = originHandles[oidx].handle.offset;
                registerPrecomputedKeys(originOffset, PrecomputedKeysInfo{
                    keysHash,
                    static_cast<uint32_t>(keys.size()),
                    srcId, fpId, dpId, *fmt,
                });
            }
        }
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
            nrTracedExprCreated++;
            nrTracedExprFromMaterialize++;
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
    cache->state.traceActiveDepth++;
    Finally decrementDepth{[&]{ cache->state.traceActiveDepth--; }};

    // Track which siblings this child accesses during evaluation.
    // Per-sibling ParentContext deps replace the single whole-parent dep to
    // avoid cascading invalidation when unrelated siblings are added/removed.
    std::optional<SiblingAccessTracker> siblingTracker;
    if (parentExpr && cache->dbBackend)
        siblingTracker.emplace(parentExpr);

    // Append per-sibling or whole-parent ParentContext deps for children.
    // Called from both the success and error paths of evaluateFresh().
    auto appendParentContextDeps = [&](std::vector<CompactDep> & deps) {
        if (!parentExpr || !cache->dbBackend) return;
        if (siblingTracker && !siblingTracker->accesses.empty()) {
            // Per-sibling ParentContext deps: one dep per accessed sibling
            for (auto & [path, hash] : siblingTracker->accesses) {
                Blake3Hash b3;
                static_assert(sizeof(b3.bytes) == 32);
                std::memcpy(b3.bytes.data(), hash.hash, 32);
                auto depKey = path;
                std::replace(depKey.begin(), depKey.end(), '\0', '\t');
                deps.push_back(CompactDep{
                    DepType::ParentContext, internDepSource(""), internDepKey(depKey), DepHashValue(b3)});
            }
        } else {
            // Zero sibling accesses: fallback to whole-parent dep
            auto parentPath = parentExpr->storeAttrPath();
            auto parentTraceHash = cache->dbBackend->getCurrentTraceHash(parentPath);
            if (parentTraceHash) {
                Blake3Hash b3;
                static_assert(sizeof(b3.bytes) == 32);
                std::memcpy(b3.bytes.data(), parentTraceHash->hash, 32);
                auto depKey = parentPath;
                std::replace(depKey.begin(), depKey.end(), '\0', '\t');
                deps.push_back(CompactDep{
                    DepType::ParentContext, internDepSource(""), internDepKey(depKey), DepHashValue(b3)});
            }
        }
    };

    Value * target;
    bool hasOrig = lazy && lazy->origExpr;
    if (hasOrig) {
        target = cache->state.allocValue();
        lazy->origExpr->eval(cache->state, *lazy->origEnv, *target);
    } else {
        if (parentExpr) {
            SuspendDepTracking suspend;
            target = navigateToReal();
        } else {
            target = navigateToReal();
        }

        if (target->isThunk()) {
            if (auto * ec = dynamic_cast<TracedExpr*>(target->thunk().expr)) {
                if (ec->lazy && ec->lazy->origExpr) {
                    Expr * expr = ec->lazy->origExpr;
                    Env * oenv = ec->lazy->origEnv;
                    target = cache->state.allocValue();
                    expr->eval(cache->state, *oenv, *target);
                }
            }
        }
    }

    auto & st = cache->state;

    try {
        st.forceValue(*target, noPos);

        if (hasOrig)
            v = *target;

        if (!hasOrig && target->type() == nAttrs && st.isDerivation(*target)) {
            if (auto * dp = target->attrs()->get(st.s.drvPath))
                st.forceValue(*dp->value, noPos);
        }
    } catch (EvalError &) {
        debug("setting '%s' to failed (fresh evaluation)", attrPathStr());
        if (cache->dbBackend) {
            auto attrPath = storeAttrPath();
            auto directDeps = tracker.collectTraces();
            appendParentContextDeps(directDeps);

            try {
                auto result = cache->dbBackend->record(
                    attrPath, failed_t{}, directDeps, !parentExpr);
                ensureLazy().traceId = result.traceId;
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
        CachedResult attrValue = buildCachedResult(st, *target);

        // For all children with a parent, add ParentContext deps linking
        // this trace to the siblings it accessed during evaluation. Per-sibling
        // deps avoid cascading invalidation: adding/removing an unaccessed
        // sibling doesn't invalidate this child's trace. When zero siblings
        // were accessed, falls back to whole-parent dep for soundness (e.g.,
        // constant-valued attributes like `version = "1.0"`).
        // We use trace_hash (not result hash) because result hash for attrsets
        // only captures attribute names, not values.
        //
        // KNOWN LIMITATION (soundness gap): If a parent overlay changes this
        // child's definition without changing any file this child reads OR any
        // sibling this child accesses, the trace incorrectly validates. See
        // design.md §9.8 and DISABLED test ParentMediatedValueChange_SoundnessGap.
        appendParentContextDeps(directDeps);

        // Direct record — no deferred writes
        try {
            auto coldResult = cache->dbBackend->record(
                attrPath, attrValue, directDeps, !parentExpr);
            ensureLazy().traceId = coldResult.traceId;
        } catch (std::exception & e) {
            debug("trace recording failed for '%s': %s", attrPathStr(), e.what());
        }

        // Sibling traces are no longer speculatively recorded. Each sibling's
        // TracedExpr handles its own tracing independently when accessed.

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

// ── Cold verification helpers ────────────────────────────────────────

/**
 * Compare two derivations by their .drv contents and report which
 * environment variables, input derivations, or input sources differ.
 */
[[gnu::cold]]
static std::string diffDerivationInputs(
    EvalState & state,
    const StorePath & drvA, const StorePath & drvB)
{
    std::string msg;
    try {
        auto derivA = state.store->readDerivation(drvA);
        auto derivB = state.store->readDerivation(drvB);

        // Compare env vars
        std::vector<std::string> envDiffs;
        for (auto & [k, vA] : derivA.env) {
            auto it = derivB.env.find(k);
            if (it == derivB.env.end()) {
                envDiffs.push_back(fmt("  env '%s': present in A, missing in B", k));
            } else if (it->second != vA) {
                auto showVal = [](const std::string & s) -> std::string {
                    if (s.size() <= 120) return s;
                    return s.substr(0, 120) + "...";
                };
                envDiffs.push_back(fmt("  env '%s':\n    A: %s\n    B: %s",
                    k, showVal(vA), showVal(it->second)));
            }
        }
        for (auto & [k, vB] : derivB.env) {
            if (derivA.env.find(k) == derivA.env.end())
                envDiffs.push_back(fmt("  env '%s': missing in A, present in B", k));
        }
        if (!envDiffs.empty()) {
            msg += "\nenv diffs (" + std::to_string(envDiffs.size()) + "):";
            for (auto & d : envDiffs)
                msg += "\n" + d;
        }

        // Compare inputDrvs
        auto printInputDrv = [&](const auto & map) {
            std::set<std::string> paths;
            for (auto & [p, _] : map.map)
                paths.insert(state.store->printStorePath(p));
            return paths;
        };
        auto inputsA = printInputDrv(derivA.inputDrvs);
        auto inputsB = printInputDrv(derivB.inputDrvs);
        std::vector<std::string> onlyA, onlyB;
        for (auto & p : inputsA)
            if (!inputsB.count(p)) onlyA.push_back(p);
        for (auto & p : inputsB)
            if (!inputsA.count(p)) onlyB.push_back(p);
        if (!onlyA.empty() || !onlyB.empty()) {
            msg += "\ninputDrvs diffs:";
            for (auto & p : onlyA) msg += "\n  only in A: " + p;
            for (auto & p : onlyB) msg += "\n  only in B: " + p;
        }

        // Compare inputSrcs
        std::set<std::string> srcsA, srcsB;
        for (auto & p : derivA.inputSrcs) srcsA.insert(state.store->printStorePath(p));
        for (auto & p : derivB.inputSrcs) srcsB.insert(state.store->printStorePath(p));
        std::vector<std::string> srcOnlyA, srcOnlyB;
        for (auto & p : srcsA) if (!srcsB.count(p)) srcOnlyA.push_back(p);
        for (auto & p : srcsB) if (!srcsA.count(p)) srcOnlyB.push_back(p);
        if (!srcOnlyA.empty() || !srcOnlyB.empty()) {
            msg += "\ninputSrcs diffs:";
            for (auto & p : srcOnlyA) msg += "\n  only in A: " + p;
            for (auto & p : srcOnlyB) msg += "\n  only in B: " + p;
        }
    } catch (std::exception & e) {
        msg += fmt("\n(could not diff .drv contents: %s)", e.what());
    }
    return msg;
}

/**
 * Recursively compare two Nix values, returning a diagnostic string
 * on the first mismatch or std::nullopt if they match.
 * Depth-limited to 20 to avoid infinite recursion on self-referential attrsets.
 */
[[gnu::cold]]
std::optional<std::string> deepCompare(
    EvalState & state, Value & a, Value & b,
    const std::string & path, int depth)
{
    if (depth > 20)
        return std::nullopt; // depth limit reached, assume match

    try {
        state.forceValue(a, noPos);
    } catch (std::exception & e) {
        return fmt("at '%s': could not force value a: %s", path, e.what());
    }
    try {
        state.forceValue(b, noPos);
    } catch (std::exception & e) {
        return fmt("at '%s': could not force value b: %s", path, e.what());
    }

    if (a.type() != b.type())
        return fmt("type mismatch at '%s': %s vs %s", path, showType(a), showType(b));

    switch (a.type()) {
    case nString: {
        auto sA = a.string_view();
        auto sB = b.string_view();
        if (sA != sB) {
            // Find first differing position
            size_t pos = 0;
            while (pos < sA.size() && pos < sB.size() && sA[pos] == sB[pos]) pos++;
            auto showAround = [](std::string_view s, size_t pos) -> std::string {
                size_t start = pos > 20 ? pos - 20 : 0;
                size_t end = std::min(pos + 20, s.size());
                return std::string(s.substr(start, end - start));
            };
            return fmt("string mismatch at '%s' (pos %d):\n  a: ...%s...\n  b: ...%s...",
                path, pos, showAround(sA, pos), showAround(sB, pos));
        }
        // Compare string contexts
        NixStringContext ctxA, ctxB;
        if (a.context())
            for (auto * elem : *a.context())
                ctxA.insert(NixStringContextElem::parse(elem->view()));
        if (b.context())
            for (auto * elem : *b.context())
                ctxB.insert(NixStringContextElem::parse(elem->view()));
        if (ctxA != ctxB) {
            std::string msg = fmt("string context mismatch at '%s':", path);
            for (auto & c : ctxA)
                if (!ctxB.count(c))
                    msg += fmt("\n  only in a: %s", c.to_string());
            for (auto & c : ctxB)
                if (!ctxA.count(c))
                    msg += fmt("\n  only in b: %s", c.to_string());
            return msg;
        }
        return std::nullopt;
    }
    case nInt:
        if (a.integer().value != b.integer().value)
            return fmt("int mismatch at '%s': %d vs %d", path, a.integer().value, b.integer().value);
        return std::nullopt;
    case nFloat:
        if (a.fpoint() != b.fpoint())
            return fmt("float mismatch at '%s': %f vs %f", path, a.fpoint(), b.fpoint());
        return std::nullopt;
    case nBool:
        if (a.boolean() != b.boolean())
            return fmt("bool mismatch at '%s': %s vs %s", path, a.boolean() ? "true" : "false", b.boolean() ? "true" : "false");
        return std::nullopt;
    case nNull:
        return std::nullopt;
    case nPath:
        if (a.path().path.abs() != b.path().path.abs())
            return fmt("path mismatch at '%s': %s vs %s", path, a.path().path.abs(), b.path().path.abs());
        return std::nullopt;
    case nAttrs: {
        // Collect attribute names
        std::set<std::string_view> namesA, namesB;
        for (auto & attr : *a.attrs()) namesA.insert(state.symbols[attr.name]);
        for (auto & attr : *b.attrs()) namesB.insert(state.symbols[attr.name]);

        if (namesA != namesB) {
            std::string msg = fmt("attrset key mismatch at '%s':", path);
            for (auto & n : namesA)
                if (!namesB.count(n))
                    msg += fmt("\n  only in a: %s", n);
            for (auto & n : namesB)
                if (!namesA.count(n))
                    msg += fmt("\n  only in b: %s", n);
            return msg;
        }

        // For derivations, compare drvPath first (most diagnostic)
        if (state.isDerivation(a)) {
            auto * dpA = a.attrs()->get(state.s.drvPath);
            auto * dpB = b.attrs()->get(state.s.drvPath);
            if (dpA && dpB) {
                state.forceValue(*dpA->value, noPos);
                state.forceValue(*dpB->value, noPos);
                auto svA = dpA->value->string_view();
                auto svB = dpB->value->string_view();
                if (svA != svB) {
                    std::string msg = fmt(
                        "drvPath mismatch at '%s':\n"
                        "  a: %s\n"
                        "  b: %s",
                        path, svA, svB);
                    try {
                        auto spA = state.store->parseStorePath(svA);
                        auto spB = state.store->parseStorePath(svB);
                        msg += diffDerivationInputs(state, spA, spB);
                    } catch (...) {}
                    return msg;
                }
            }
        }

        // Recursively compare children
        for (auto & attrA : *a.attrs()) {
            auto name = state.symbols[attrA.name];
            auto * attrB = b.attrs()->get(attrA.name);
            if (!attrB) continue; // already caught above
            auto childPath = path.empty()
                ? std::string(name)
                : path + "." + std::string(name);
            auto result = deepCompare(state, *attrA.value, *attrB->value, childPath, depth + 1);
            if (result) return result;
        }
        return std::nullopt;
    }
    case nList: {
        if (a.listSize() != b.listSize())
            return fmt("list size mismatch at '%s': %d vs %d", path, a.listSize(), b.listSize());
        for (size_t i = 0; i < a.listSize(); i++) {
            auto childPath = path + "[" + std::to_string(i) + "]";
            auto result = deepCompare(state, *a.listView()[i], *b.listView()[i], childPath, depth + 1);
            if (result) return result;
        }
        return std::nullopt;
    }
    case nThunk:
    case nFunction:
    case nExternal:
        return std::nullopt; // can't compare these
    }
    return std::nullopt;
}

// TracedExpr::eval — demand-driven dispatch (Adapton)
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

    // Exclude this child TracedExpr's dep range from the parent's trace.
    // Each TracedExpr manages its own deps via its DependencyTracker in
    // evaluateFresh(); the parent references children via ParentContext
    // deps (appendParentContextDeps). Without this exclusion, parent
    // traces inherit ~30K child deps, making them evaluation-order-dependent.
    auto * parentTracker = DependencyTracker::activeTracker;
    uint32_t childRangeStart = DependencyTracker::sessionTraces.size();
    if (parentTracker && state.traceCtx)
        state.traceCtx->skipEpochRecordFor = &v;

    struct ChildRangeExcluder {
        DependencyTracker * parentTracker;
        uint32_t rangeStart;
        ~ChildRangeExcluder() {
            if (parentTracker) {
                uint32_t rangeEnd = DependencyTracker::sessionTraces.size();
                parentTracker->excludeChildRange(rangeStart, rangeEnd);
            }
        }
    } childExcluder{parentTracker, childRangeStart};

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

    auto attrPath = storeAttrPath();
    auto warmResult = db.verify(attrPath, cache->inputAccessors, cache->state);

    if (warmResult) {
        auto & [cachedValue, cachedTraceId] = *warmResult;
        ensureLazy().traceId = cachedTraceId;

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
            // list_t or other → falls through to evaluateFresh (not a cache hit)
        } else {
            materializeResult(v, cachedValue);
            replayTrace(cachedTraceId);
            handled = true;
        }

        if (handled)
            return;
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
        nrTracedExprCreated++;
        value = allocRootValue(v);
    }
    return *value;
}

void TraceCache::verifyCold(const std::string & attrPath, Value & tracedResult)
{
    SuspendDepTracking suspend;

    // Call rootLoader() directly (NOT getOrEvaluateRoot()) to avoid reusing
    // a cached realRoot that may have been partially forced with tracing active.
    Value * freshRoot = rootLoader();
    state.forceValue(*freshRoot, noPos);

    auto emptyArgs = state.buildBindings(0).finish();
    auto [coldV, pos] = findAlongAttrPath(state, attrPath, *emptyArgs, *freshRoot);
    state.forceValue(*coldV, noPos);

    // Force drvPath on both sides if derivation
    if (tracedResult.type() == nAttrs && state.isDerivation(tracedResult)) {
        if (auto * dp = tracedResult.attrs()->get(state.s.drvPath))
            state.forceValue(*dp->value, noPos);
    }
    if (coldV->type() == nAttrs && state.isDerivation(*coldV)) {
        if (auto * dp = coldV->attrs()->get(state.s.drvPath))
            state.forceValue(*dp->value, noPos);
    }

    auto mismatch = deepCompare(state, tracedResult, *coldV, attrPath);
    if (mismatch)
        throw Error("verify-eval-trace (cold): %s", *mismatch);

    debug("verify-eval-trace (cold): '%s' matches", attrPath);
}

} // namespace nix::eval_trace
