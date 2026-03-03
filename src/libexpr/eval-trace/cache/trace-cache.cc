#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/util/finally.hh"
#include "nix/expr/eval-trace/cache/trace-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/fetchers/fetchers.hh"

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
    AttrPathId pathId;           // Trie path in AttrVocabStore (computed at construction)
    TracedExpr * parentExpr;     // GC-traced, nullptr for root
    bool isListElement;          // true = list index, false = attr access

    /**
     * Lazy-initialized state for fields only needed when eval() or
     * navigateToReal() runs. Reduces per-child allocation from ~100 to ~40 bytes.
     * For unaccessed children (the vast majority), LazyState is never allocated.
     */
    struct LazyState : gc {
        Expr * origExpr = nullptr;
        Env * origEnv = nullptr;
        std::optional<TraceId> traceId;
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
    std::string attrPathStr() const { return vocab().displayPath(pathId); }

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

    /// Convenience accessor for the vocab store (lazy-inits if needed).
    AttrVocabStore & vocab() const { return cache->state.traceCtx->getVocabStore(cache->state.symbols); }
};

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
    std::vector<std::pair<AttrPathId, Hash>> accesses;
    boost::unordered_flat_set<AttrPathId, AttrPathId::Hash> seen;

    static thread_local SiblingAccessTracker * current;
    SiblingAccessTracker * previous;

    explicit SiblingAccessTracker(TracedExpr * parent)
        : parentExpr(parent), previous(current) { current = this; }
    ~SiblingAccessTracker() { current = previous; }

    SiblingAccessTracker(const SiblingAccessTracker &) = delete;
    SiblingAccessTracker & operator=(const SiblingAccessTracker &) = delete;

    void recordAccess(AttrPathId pathId, const Hash & traceHash)
    {
        if (seen.insert(pathId).second)
            accesses.emplace_back(pathId, traceHash);
    }

    static void maybeRecord(TracedExpr * expr, TraceStore & db)
    {
        if (!current) return;
        if (expr->parentExpr != current->parentExpr) return;
        if (!expr->lazy || !expr->lazy->traceId) return;
        try {
            auto hash = db.getCurrentTraceHash(expr->pathId);
            if (hash) current->recordAccess(expr->pathId, *hash);
        } catch (...) {}
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
    case nList:
        return list_t{target.listSize()};
    case nThunk:
    case nFunction:
    case nExternal:
    case nFailed:
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
    auto & pools = *st.traceCtx->pools;
    if (!attrs.origins.empty()) {
        pools.sessionSymbols = &st.symbols;
        originHandles.reserve(attrs.origins.size());
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
            originHandles.push_back({handle, 0});
        }
    }

    for (size_t i = 0; i < attrs.names.size(); i++) {
        auto childName = attrs.names[i];
        auto * childVal = st.allocValue();
        auto * wrapper = new TracedExpr(cache, childName, this);
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
            auto srcId = pools.intern<DepSourceId>(orig.depSource);
            auto fpId = pools.filePathPool.intern(orig.depKey);
            auto dpId = jsonStringToDataPathId(pools, orig.dataPath);
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
        auto & pools = *cache->state.traceCtx->pools;
        auto deps = cache->dbBackend->loadFullTrace(traceId);
        for (auto & idep : deps) {
            auto resolved = cache->dbBackend->resolveDep(idep);
            DependencyTracker::recordReplay(pools, resolved.type,
                resolved.source, resolved.key, resolved.expectedHash);
        }
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
                    auto * wrapper = new TracedExpr(cache, attr.name, parentEC);
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
        auto & pools = *st.traceCtx->pools;
        if (!attrs->origins.empty()) {
            pools.sessionSymbols = &st.symbols;
            originHandles.reserve(attrs->origins.size());
            std::vector<uint32_t> counts(attrs->origins.size(), 0);
            for (auto idx : attrs->originIndices)
                if (idx >= 0) counts[idx]++;
            for (auto & orig : attrs->origins) {
                auto srcId = pools.intern<DepSourceId>(orig.depSource);
                auto fpId = pools.filePathPool.intern(orig.depKey);
                auto dpId = jsonStringToDataPathId(pools, orig.dataPath);
                auto handle = st.positions.addOriginHandle(
                    allocateProvenanceRef(pools, srcId, fpId, dpId, orig.format),
                    counts[&orig - attrs->origins.data()]);
                originHandles.push_back({handle, 0});
            }
        }

        for (size_t i = 0; i < attrs->names.size(); i++) {
            auto childName = attrs->names[i];
            auto * childVal = st.allocValue();
            auto * child = new TracedExpr(cache, childName, this);
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
                auto srcId = pools.intern<DepSourceId>(orig.depSource);
                auto fpId = pools.filePathPool.intern(orig.depKey);
                auto dpId = jsonStringToDataPathId(pools, orig.dataPath);
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
            auto * child = new TracedExpr(cache, sym, this, /*isListElement=*/true);
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
    auto & pools = *cache->state.traceCtx->pools;
    DependencyTracker tracker(pools);
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
    auto appendParentContextDeps = [&](std::vector<Dep> & deps) {
        if (!parentExpr || !cache->dbBackend) return;
        if (siblingTracker && !siblingTracker->accesses.empty()) {
            // Per-sibling ParentContext deps: one dep per accessed sibling
            for (auto & [sibPathId, hash] : siblingTracker->accesses) {
                deps.push_back(Dep::makeParentContext(
                    sibPathId, DepHashValue(Blake3Hash::fromHash(hash))));
            }
        } else {
            // Zero sibling accesses: fallback to whole-parent dep
            auto parentTraceHash = cache->dbBackend->getCurrentTraceHash(parentExpr->pathId);
            if (parentTraceHash) {
                deps.push_back(Dep::makeParentContext(
                    parentExpr->pathId, DepHashValue(Blake3Hash::fromHash(*parentTraceHash))));
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
            auto directDeps = tracker.collectTraces();
            appendParentContextDeps(directDeps);

            try {
                auto result = cache->dbBackend->record(
                    pathId, failed_t{}, directDeps, !parentExpr);
                ensureLazy().traceId = result.traceId;
            } catch (std::exception & e) {
                debug("trace recording failed for '%s': %s", attrPathStr(), e.what());
            }
        }
        throw;
    }

    if (cache->dbBackend) {
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
                pathId, attrValue, directDeps, !parentExpr);
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

    auto warmResult = db.verify(pathId, cache->inputAccessors, cache->state);

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
