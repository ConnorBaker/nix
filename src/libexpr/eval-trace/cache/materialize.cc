#include "traced-expr.hh"
#include "materialization-scope.hh"

#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "../fiber/fiber-scheduler.hh"

#include <algorithm>
#include <boost/unordered/unordered_flat_map.hpp>
#include <cassert>

namespace nix::eval_trace {

static std::optional<SemanticHandle> nonEmptyHandle(const SemanticHandle & handle)
{
    if (handle.empty())
        return std::nullopt;

    return handle;
}

/**
 * Detect Value* aliases in a container: multiple elements pointing to
 * the same Value. `getValuePtr(i)` returns the Value* for element i.
 * Populates aliasOf: invalidSiblingIndex = canonical, otherwise alias of that index.
 * Clears aliasOf if no aliases are detected.
 */
static void detectAliases(std::vector<uint32_t> & aliasOf, size_t n, auto getValuePtr)
{
    boost::unordered_flat_map<const Value *, size_t> valueToIdx;
    aliasOf.resize(n, invalidSiblingIndex);
    bool hasAliases = false;
    for (size_t i = 0; i < n; i++) {
        auto [it, inserted] = valueToIdx.emplace(getValuePtr(i), i);
        if (!inserted) {
            aliasOf[i] = static_cast<uint32_t>(it->second);
            hasAliases = true;
        }
    }
    if (!hasAliases) aliasOf.clear();
}

/// Set canonicalSiblingIdx from per-list child metadata during materialization.
static void annotateAlias(TracedExpr * te, const std::vector<CachedListEntry> & entries, size_t i)
{
    if (!entries.empty())
        te->canonicalSiblingIdx = (entries[i].aliasOf != invalidSiblingIndex)
            ? entries[i].aliasOf : static_cast<uint32_t>(i);
}

/// Set canonicalSiblingIdx from per-attr child metadata during materialization.
static void annotateAlias(TracedExpr * te, const std::vector<CachedAttrEntry> & entries, size_t i)
{
    if (!entries.empty())
        te->canonicalSiblingIdx = (entries[i].aliasOf != invalidSiblingIndex)
            ? entries[i].aliasOf : static_cast<uint32_t>(i);
}

static std::optional<StructuredObject> maybeEncodeContainerOrigin(
    const TracedContainerProvenance * prov,
    InterningPools & pools)
{
    if (!prov)
        return std::nullopt;

    return StructuredObject{
        .source = pools.resolveDepSource(prov->sourceId),
        .key = std::string(pools.resolve(prov->filePathId)),
        .dataPath = resolveStructuredPath(pools, prov->dataPathId),
        .format = prov->format,
    };
}

/**
 * Convert a forced Value into a CachedResult for storage/comparison.
 * The value must already be forced. For lists, suspends dep tracking
 * during the allStrings check to avoid mixing StructuredContent deps.
 */
CachedResult buildCachedResult(EvalState & st, Value & target)
{
    // NOTE: TraceAccess::current() may be nullopt here. When no recording
    // context is active (e.g., warm-hit materialization before replayTrace),
    // provenance capture is silently skipped. This is intentional — the
    // materialized Value carries provenance only when a recording scope
    // exists to receive replayed deps.
    switch (target.type()) {
    case nString: {
        NixStringContext ctx;
        if (target.context()) {
            for (auto * elem : *target.context())
                ctx.insert(NixStringContextElem::parse(elem->view()));
        }
        auto pub = st.lookupSemanticHandle(target).value_or(SemanticHandle{});
        if (auto stamp = st.lookupValueIdentityStamp(target))
            pub = pub.withIdentity(IdentityObject{stamp->value});
        return string_t{
            std::string(target.string_view()),
            std::move(ctx),
            std::move(pub),
        };
    }
    case nBool:
        return target.boolean();
    case nInt:
        return int_t{NixInt{target.integer().value}};
    case nNull:
        return makeNull();
    case nFloat:
        return float_t{target.fpoint()};
    case nPath: {
        auto pub = st.lookupSemanticHandle(target).value_or(SemanticHandle{});
        if (auto stamp = st.lookupValueIdentityStamp(target))
            pub = pub.withIdentity(IdentityObject{stamp->value});
        return path_t{
            target.path().path.abs(),
            std::move(pub),
        };
    }
    case nAttrs: {
        attrs_t result;
        for (auto & attr : *target.attrs())
            result.entries.push_back(CachedAttrEntry{.name = attr.name});
        std::sort(result.entries.begin(), result.entries.end(),
            [&](const CachedAttrEntry & a, const CachedAttrEntry & b) {
                return std::string_view(st.symbols[a.name])
                     < std::string_view(st.symbols[b.name]);
            });
        std::vector<uint32_t> aliasOf;
        detectAliases(aliasOf, result.entries.size(), [&](size_t i) {
            return target.attrs()->get(result.entries[i].name)->value;
        });
        if (!aliasOf.empty()) {
            for (size_t i = 0; i < result.entries.size(); i++)
                result.entries[i].aliasOf = aliasOf[i];
        }
        // Capture per-attr TracedData origins for cross-scope materialization.
        // Resolve interned IDs back to strings for serialization.
        if (target.attrs()->hasAnyTracedDataLayer()) {
            for (size_t i = 0; i < result.entries.size(); i++) {
                auto * attr = target.attrs()->get(result.entries[i].name);
                if (!attr || !attr->pos.isTracedData()) continue;
                auto * origin = st.positions.originOfPtr(attr->pos);
                if (!origin) continue;
                auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
                if (!pr) continue;
                auto access = TraceAccess::current();
                auto & pools = access ? access->tracingPools() : st.tracingPools();
                auto & df = resolveProvenanceRef(pools, *pr);
                auto depKey = std::string(pools.resolve(df.filePathId));
                result.entries[i].producerOrigin = StructuredObject{
                    .source = pools.resolveDepSource(df.sourceId),
                    .key = std::move(depKey),
                    .dataPath = resolveStructuredPath(pools, df.dataPathId),
                    .format = df.format,
                };
            }
        }
        if (auto access = TraceAccess::current()) {
            if (auto * prov = access->lookupTracedContainer(&target)) {
                if (auto origin = maybeEncodeContainerOrigin(prov, access->tracingPools()))
                    result.meta = TracedContainerMeta{.producerOrigin = std::move(origin)};
            }
        }
        if (auto stamp = st.lookupValueIdentityStamp(target)) {
            if (!result.meta)
                result.meta = TracedContainerMeta{};
            result.meta->valueIdentityStamp = *stamp;
        }
        return result;
    }
    case nList: {
        list_t result;
        result.entries.resize(target.listSize());
        auto view = target.listView();
        std::vector<uint32_t> aliasOf;
        detectAliases(aliasOf, result.entries.size(), [&](size_t i) {
            return view[i];
        });
        if (!aliasOf.empty()) {
            for (size_t i = 0; i < result.entries.size(); i++)
                result.entries[i].aliasOf = aliasOf[i];
        }
        if (auto access = TraceAccess::current()) {
            if (auto * prov = access->lookupTracedContainer(&target)) {
                if (auto origin = maybeEncodeContainerOrigin(prov, access->tracingPools()))
                    result.meta = TracedContainerMeta{.producerOrigin = std::move(origin)};
            }
        }
        if (auto stamp = st.lookupValueIdentityStamp(target)) {
            if (!result.meta)
                result.meta = TracedContainerMeta{};
            result.meta->valueIdentityStamp = *stamp;
        }
        return result;
    }
    case nThunk:
    case nFunction:
    case nExternal:
    case nFailed:
        return makeMisc();
    }
    unreachable();
}

// ── Origin re-internment and precomputed-keys helpers ────────────────

namespace {

struct PerOrigin {
    StructuredObject origin;
    PosTable::OriginHandle handle;
    uint32_t attrCount = 0;
};

struct ReinternedOrigins {
    std::vector<PerOrigin> handles;
    std::vector<int> childOriginIndices;
};

/**
 * Re-intern cached origins into PosTable so downstream shape dep recording
 * (SC #keys, #has:key, #type) works. Returns handles indexed by origin index.
 */
ReinternedOrigins reinternOrigins(
    const attrs_t & attrs, EvalState & st, InterningPools & pools)
{
    ReinternedOrigins result;
    result.childOriginIndices.assign(attrs.entries.size(), -1);
    std::vector<StructuredObject> origins;
    std::vector<uint32_t> counts;
    origins.reserve(attrs.entries.size());
    counts.reserve(attrs.entries.size());
    for (size_t i = 0; i < attrs.entries.size(); i++) {
        auto & entry = attrs.entries[i];
        if (!entry.producerOrigin) continue;
        int originIdx = -1;
        for (size_t j = 0; j < origins.size(); j++) {
            if (origins[j] == *entry.producerOrigin) {
                originIdx = static_cast<int>(j);
                break;
            }
        }
        if (originIdx < 0) {
            originIdx = static_cast<int>(origins.size());
            origins.push_back(*entry.producerOrigin);
            counts.push_back(0);
        }
        result.childOriginIndices[i] = originIdx;
        counts[static_cast<size_t>(originIdx)]++;
    }
    result.handles.reserve(origins.size());
    for (size_t i = 0; i < origins.size(); i++) {
        auto & structured = origins[i];
        auto srcId = pools.intern<DepSourceId>(structured.source);
        auto fpId = pools.intern<FilePathId>(structured.key);
        auto dpId = internStructuredPath(pools, structured.dataPath);
        auto handle = st.positions.addOriginHandle(
            allocateProvenanceRef(pools, srcId, fpId, dpId, structured.format),
            counts[i]);
        result.handles.push_back(PerOrigin{
            .origin = std::move(origins[i]),
            .handle = handle,
            .attrCount = 0,
        });
    }
    if (result.handles.empty())
        result.childOriginIndices.clear();
    return result;
}

/**
 * Register precomputed keys for each origin (mirrors ExprTracedData::eval()
 * in json-to-value.cc). Computes canonical #keys hash per origin group.
 */
std::vector<PendingPrecomputedKey> collectPrecomputedKeys(
    const attrs_t & attrs, const std::vector<PerOrigin> & originHandles,
    const std::vector<int> & childOriginIndices,
    EvalState & st, InterningPools & pools)
{
    std::vector<PendingPrecomputedKey> batch;
    batch.reserve(originHandles.size());
    for (size_t oidx = 0; oidx < originHandles.size(); oidx++) {
        auto & orig = originHandles[oidx].origin;
        std::vector<std::string> keys;
        for (size_t i = 0; i < attrs.entries.size(); i++) {
            if (!childOriginIndices.empty() && childOriginIndices[i] == static_cast<int>(oidx))
                keys.push_back(std::string(st.symbols[attrs.entries[i].name]));
        }
        auto nKeys = static_cast<uint32_t>(keys.size());
        auto keysHash = canonicalKeysHash(std::move(keys));
        auto & structured = orig;
        auto srcId = pools.intern<DepSourceId>(structured.source);
        auto fpId = pools.intern<FilePathId>(structured.key);
        auto dpId = internStructuredPath(pools, structured.dataPath);
        batch.push_back(PendingPrecomputedKey{
            .originOffset = originHandles[oidx].handle.offset,
            .info = PrecomputedKeysInfo{
                keysHash, nKeys, srcId, fpId, dpId, structured.format,
            },
        });
    }
    return batch;
}

} // anonymous namespace

void MaterializationScope::commit()
{
    std::vector<PublishedMaterializedIdentity> publishedIdentities;
    std::vector<Value *> publishedContainers;
    std::vector<uint32_t> publishedPrecomputedOffsets;

    publishedIdentities.reserve(valueIdentities.size());
    publishedContainers.reserve(containerProvenances.size());
    publishedPrecomputedOffsets.reserve(precomputedKeys.size());

    try {
        for (auto & staged : valueIdentities) {
            publishedIdentities.push_back(state.publishRootMaterializedValueIdentity(
                staged.value, backend, pathId, staged.stamp));
        }

        if (access) {
            for (auto & staged : containerProvenances) {
                auto * prov = access->allocateProvenance(
                    staged.sourceId, staged.filePathId, staged.dataPathId, staged.format);
                access->registerTracedContainer(staged.value, prov);
                publishedContainers.push_back(staged.value);
            }

            for (auto & staged : precomputedKeys) {
                access->registerPrecomputedKeys(staged.originOffset, staged.info);
                publishedPrecomputedOffsets.push_back(staged.originOffset);
            }
        }
    } catch (...) {
        if (access) {
            for (auto it = publishedPrecomputedOffsets.rbegin();
                 it != publishedPrecomputedOffsets.rend(); ++it)
                access->erasePrecomputedKeys(*it);
            for (auto it = publishedContainers.rbegin(); it != publishedContainers.rend(); ++it)
                access->unregisterTracedContainer(*it);
        }
        for (auto it = publishedIdentities.rbegin(); it != publishedIdentities.rend(); ++it)
            state.rollbackRootMaterializedValueIdentity(*it);
        throw;
    }

    valueIdentities.clear();
    containerProvenances.clear();
    precomputedKeys.clear();
}

// ── TracedExpr materialization methods ───────────────────────────────

// Replay trace (Adapton change propagation).
// Requires proof that a dep recording scope is active — without an
// active scope, replayed deps change memoization behavior for later
// recordings, altering trace hashes. The proof is created by
// RecordingScopeGuard::ifActive at the call site.
void TracedExpr::replayTrace(
    EvalContext<Suspendable> & ctx,
    const EpochLogWriteProof<EpochLogWriteReason::ReplayActive> & proof,
    TraceId traceId)
{
    try {
        auto * depCtx = eval_trace::currentFiberDepCtx();
        if (!depCtx) depCtx = eval_trace::currentStandaloneDepCtx();
        if (!depCtx) return;
        auto deps = cache->runtime_->loadFullTrace(ctx, traceId);
        for (const auto & dep : *deps)
            depCtx->epochLog.appendReplayed(proof, dep);
    } catch (std::exception &) {
    }
}

// `navigateToReal` for Child is in `trace-session.cc` alongside
// Root's version (single symbol, kind-discriminated).

// materializeResult -- construct deep traced values (Adapton articulation points)
void TracedExpr::materializeResult(EvalContext<Suspendable> & ctx, Value & v, const CachedResult & cached)
{
    auto & st = cache->state;

    if (auto * attrs = std::get_if<attrs_t>(&cached)) {
        auto access = TraceAccess::current();
        MaterializationScope materializationScope(st, cache->runtime_.get(), pathId, access);
        auto bindings = st.buildBindings(
            attrs->entries.size(),
            attrs->entries.empty() && attrs->meta && attrs->meta->producerOrigin
                ? EmptyBindingsAllocation::AllocateFresh
                : EmptyBindingsAllocation::ReuseSharedEmpty);
        auto & pools = access ? access->tracingPools() : st.tracingPools();

        std::vector<PerOrigin> originHandles;
        std::vector<int> childOriginIndices;
        if (!attrs->entries.empty()) {
            auto reinternedOrigins = reinternOrigins(*attrs, st, pools);
            originHandles = std::move(reinternedOrigins.handles);
            childOriginIndices = std::move(reinternedOrigins.childOriginIndices);
        }

        for (size_t i = 0; i < attrs->entries.size(); i++) {
            auto childName = attrs->entries[i].name;
            auto * childVal = st.allocValue();
            auto * child = TracedExpr::makeChild(*cache, childName, *this);
            nrTracedExprCreated++;
            nrTracedExprFromMaterialize++;
            annotateAlias(child, attrs->entries, i);
            installChildThunk(childVal, &st.baseEnv, child);

            PosIdx pos = noPos;
            if (!childOriginIndices.empty() && childOriginIndices[i] >= 0) {
                auto oidx = static_cast<size_t>(childOriginIndices[i]);
                pos = st.positions.add(originHandles[oidx].handle, originHandles[oidx].attrCount++);
            }
            bindings.insert(childName, childVal, pos);
        }
        v.mkAttrs(bindings.finish());
        if (attrs->meta && attrs->meta->valueIdentityStamp)
            materializationScope.stageValueIdentity(v, *attrs->meta->valueIdentityStamp);
        if (attrs->meta && attrs->meta->producerOrigin && access) {
            auto & structured = *attrs->meta->producerOrigin;
            auto srcId = pools.intern<DepSourceId>(structured.source);
            auto fpId = pools.intern<FilePathId>(structured.key);
            auto dpId = internStructuredPath(pools, structured.dataPath);
            materializationScope.stageContainerProvenance(v, srcId, fpId, dpId, structured.format);

            // Restore StructuredObject publication on Bindings via the sealed
            // EvalState::publishStructuredProvenance helper.
            st.publishStructuredProvenance(v, structured);
        }

        materializationScope.stagePrecomputedKeys(
            collectPrecomputedKeys(*attrs, originHandles, childOriginIndices, st, pools));
        materializationScope.commit();

        // Submit prefetch hints for all child attrs. The orchestrator may
        // start speculative verification before children are forced.
        // Collect pathIds from the already-created TracedExpr child thunks.
        if (attrs->entries.size() > 1) {
            std::vector<AttrPathId> childPathIds;
            childPathIds.reserve(attrs->entries.size());
            for (auto it = v.attrs()->begin(); it != v.attrs()->end(); ++it) {
                if (it->value->isThunk()) {
                    auto * expr = dynamic_cast<TracedExpr *>(it->value->thunk().expr);
                    if (expr)
                        childPathIds.push_back(expr->pathId);
                }
            }
            if (!childPathIds.empty())
                cache->runtime_->submitPrefetchHints(childPathIds);
        }
    } else if (auto * s = std::get_if<string_t>(&cached)) {
        if (s->second.empty())
            v.mkString(s->first, st.mem);
        else
            v.mkString(s->first, s->second, st.mem);
        st.setSemanticHandle(v, nonEmptyHandle(s->publication));
    } else if (auto * b = std::get_if<bool>(&cached)) {
        v.mkBool(*b);
    } else if (auto * i = std::get_if<int_t>(&cached)) {
        v.mkInt(i->x);
    } else if (auto * p = std::get_if<path_t>(&cached)) {
        v.mkPath(st.rootPath(CanonPath(p->path)), st.mem);
        st.setSemanticHandle(v, nonEmptyHandle(p->publication));
    } else if (auto * trivial = std::get_if<trivial_t>(&cached); trivial && trivial->kind == TrivialKind::Null) {
        v.mkNull();
    } else if (auto * f = std::get_if<float_t>(&cached)) {
        v.mkFloat(f->x);
    } else if (auto * lt = std::get_if<list_t>(&cached)) {
        auto access = TraceAccess::current();
        MaterializationScope materializationScope(st, cache->runtime_.get(), pathId, access);
        auto list = st.buildList(lt->entries.size(), true);
        for (size_t i = 0; i < lt->entries.size(); i++) {
            auto * elemVal = st.allocValue();
            auto sym = st.symbols.create(std::to_string(i));
            auto * child = TracedExpr::makeChild(*cache, sym, *this, i);
            nrTracedExprCreated++;
            nrTracedExprFromMaterialize++;
            annotateAlias(child, lt->entries, i);
            installChildThunk(elemVal, &st.baseEnv, child);
            list[i] = elemVal;
        }
        v.mkList(list);
        if (lt->meta && lt->meta->valueIdentityStamp)
            materializationScope.stageValueIdentity(v, *lt->meta->valueIdentityStamp);
        if (lt->meta && lt->meta->producerOrigin && access) {
            auto & pools = access->tracingPools();
            auto & structured = *lt->meta->producerOrigin;
            auto srcId = pools.intern<DepSourceId>(structured.source);
            auto fpId = pools.intern<FilePathId>(structured.key);
            auto dpId = internStructuredPath(pools, structured.dataPath);
            materializationScope.stageContainerProvenance(v, srcId, fpId, dpId, structured.format);
            // NOTE: publishStructuredProvenance is intentionally not called for list
            // values. stageContainerProvenance above is the operative call; list
            // provenance is carried via ContainerProvenanceRegistry, not via
            // Value::publication(). See value.hh List struct comment.
        }
        materializationScope.commit();
    } else {
        // trivial_t (Placeholder/Missing/Misc — Null handled above) or
        // failed_t: fresh evaluation needed
        evaluateFresh(ctx, v);
    }
}

} // namespace nix::eval_trace
