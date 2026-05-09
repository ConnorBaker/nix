#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/value.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/hash.hh"
#include "nix/util/pos-table.hh"

#include <boost/unordered/unordered_flat_set.hpp>

#include <nlohmann/json.hpp>

namespace nix {

// Cached constant dep hashes used in shape dep recording and verification.
// Function-local statics avoid static initialization order issues across TUs.
// Declared in dep-hash-fns.hh so the verification side shares them.
const DepHash & sentinel(SentinelHash kind) noexcept
{
    switch (kind) {
    case SentinelHash::Zero:    { static const auto h = depHash("0"); return h; }
    case SentinelHash::One:     { static const auto h = depHash("1"); return h; }
    case SentinelHash::Object:  { static const auto h = depHash("object"); return h; }
    case SentinelHash::Array:   { static const auto h = depHash("array"); return h; }
    case SentinelHash::Empty:   { static const auto h = depHash(""); return h; }
    case SentinelHash::Missing: { static const auto h = depHash("<missing>"); return h; }
    }
    // Unreachable — SentinelHash is an exhaustive enum and -Wswitch-enum
    // catches additions at compile time.
    std::abort();
}

// ═══════════════════════════════════════════════════════════════════════
// Shape dep recording for traced data containers
// ═══════════════════════════════════════════════════════════════════════

/**
 * Iterate unique TracedData origins in an attrset and invoke a callback for each.
 * The callback receives the TracedData struct (interned IDs + format char).
 * Deduplicates by TracedData pointer identity. Used by maybeRecordTypeDep and
 * maybeRecordHasKeyDep (exists=false) to avoid duplicated scanning loops.
 */
template<typename Fn>
static void forEachTracedDataOrigin(const PosTable & positions, const Value & v, const Fn & fn)
{
    boost::unordered_flat_set<uint32_t> seen;
    for (auto & attr : *v.attrs()) {
        if (!attr.pos.isTracedData()) continue;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) continue;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) continue;
        if (!seen.insert(pr->id).second) continue;
        auto access = eval_trace::TraceAccess::current();
        if (!access) continue;
        auto & rec = resolveProvenanceRef(access->tracingPools(), *pr);
        fn(rec);
    }
}

[[gnu::cold]] void maybeRecordListLenDep(const Value & v)
{
    auto access = eval_trace::TraceAccess::current();
    if (!access) return;
    // No early return for empty lists — length 0 is a valid observable
    // property.  The provenance guard below handles non-traced lists.
    auto & pools = access->tracingPools();
    auto * prov = access->lookupTracedContainer(&v);
    if (!prov) return;
    auto hash = depHash(std::to_string(v.listSize()));
    CompactDepComponents c{prov->sourceId, prov->filePathId, prov->format, prov->dataPathId,
                           ShapeSuffix::Len, StringId(), StringId()};
    recordStructuredDep(pools, c, DepHashValue(hash));
}

[[gnu::cold]] void maybeRecordAttrKeysDep(const PosTable & positions, const SymbolTable & symbols, const Value & v)
{
    auto access = eval_trace::TraceAccess::current();
    if (!access) return;
    if (v.type() != nAttrs) return;
    if (!v.attrs()->hasAnyTracedDataLayer()) return;

    // Memoize: skip if we've already scanned this exact Bindings*.
    if (!access->markBindingsScanned(v.attrs())) return;

    // Group attrs by their TracedData origin.
    struct OriginKeys {
        const ProvenanceRecord * df;
        uint32_t originOffset;
        std::vector<std::string_view> keys;
    };
    std::vector<OriginKeys> groups;

    for (auto & attr : *v.attrs()) {
        if (!attr.pos.isTracedData()) continue;
        auto resolved = positions.resolveOriginFull(attr.pos);
        if (!resolved) continue;
        auto * pr = std::get_if<Pos::ProvenanceRef>(resolved->origin);
        if (!pr) continue;
        auto & rec = resolveProvenanceRef(access->tracingPools(), *pr);
        const ProvenanceRecord * recPtr = &rec;
        OriginKeys * group = nullptr;
        for (auto & g : groups) {
            if (g.df == recPtr) { group = &g; break; }
        }
        if (!group) {
            groups.push_back({recPtr, resolved->offset, {}});
            group = &groups.back();
        }
        group->keys.push_back(symbols[attr.name]);
    }

    auto & pools = access->tracingPools();
    for (auto & g : groups) {
        // Fast path: if all original keys are visible (no shadowing by //),
        // use the precomputed hash from ExprTracedData::eval() creation time.
        if (auto * info = access->lookupPrecomputedKeys(g.originOffset);
            info && info->keyCount == g.keys.size()) {
            CompactDepComponents c{info->sourceId, info->filePathId, g.df->format,
                                   info->dataPathId, ShapeSuffix::Keys, StringId(), StringId()};
            recordStructuredDep(pools, c, DepHashValue(info->hash));
            continue;
        }
        // Slow path REMOVED: when keys are shadowed by //, ImplicitShape
        // (recorded at creation time with ALL source keys) handles verification.
    }
}

[[gnu::cold]] void maybeRecordTypeDep(const PosTable & positions, const Value & v)
{
    auto access = eval_trace::TraceAccess::current();
    if (!access) return;
    auto & pools = access->tracingPools();

    switch (v.type()) {
    case nAttrs: {
        if (!v.attrs()->hasAnyTracedDataLayer()) return;
        forEachTracedDataOrigin(positions, v, [&](const ProvenanceRecord & df) {
            CompactDepComponents c{df.sourceId, df.filePathId, df.format, df.dataPathId,
                                   ShapeSuffix::Type, StringId(), StringId()};
            recordStructuredDep(pools, c, DepHashValue(sentinel(SentinelHash::Object)));
        });
        break;
    }
    case nList: {
        auto * prov = access->lookupTracedContainer(&v);
        if (!prov) return;
        CompactDepComponents c{prov->sourceId, prov->filePathId, prov->format,
                               prov->dataPathId, ShapeSuffix::Type, StringId(), StringId()};
        recordStructuredDep(pools, c, DepHashValue(sentinel(SentinelHash::Array)));
        break;
    }
    case nInt:
    case nBool:
    case nString:
    case nPath:
    case nNull:
    case nFloat:
    case nThunk:
    case nFunction:
    case nExternal:
    case nFailed:
        break;
    }
}

// Forward declarations for DirSet aggregation helpers
static std::pair<std::string, DirSetDefinition> computeDirSetHashAndDefinition(
    const std::vector<std::pair<DepSourceId, FilePathId>> & dirs);
static const std::vector<IntersectOriginInfo> emptyOrigins;

static const std::vector<IntersectOriginInfo> & collectOriginsCached(
    const PosTable & positions, const Value & v)
{
    auto access = eval_trace::TraceAccess::current();
    if (!access) return emptyOrigins;
    auto * bindings = v.attrs();
    if (auto * cached = access->lookupIntersectOrigins(bindings))
        return *cached;

    std::vector<IntersectOriginInfo> result;
    boost::unordered_flat_set<uint32_t> seen;
    for (auto & attr : *bindings) {
        if (!attr.pos.isTracedData()) continue;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) continue;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) continue;
        if (!seen.insert(pr->id).second) continue;
        auto & rec = resolveProvenanceRef(access->tracingPools(), *pr);
        result.push_back({rec.sourceId, rec.filePathId, rec.dataPathId, rec.format});
    }
    if (auto * cached = access->cacheIntersectOriginsIfScoped(bindings, std::move(result)))
        return *cached;
    return emptyOrigins;
}

[[gnu::cold]] void recordIntersectAttrsDeps(const PosTable & positions, const SymbolTable & symbols,
                                            const Value & left, const Value & right)
{
    auto access = eval_trace::TraceAccess::current();
    if (!access) return;

    bool leftHasData = left.type() == nAttrs && left.attrs()->hasAnyTracedDataLayer();
    bool rightHasData = right.type() == nAttrs && right.attrs()->hasAnyTracedDataLayer();
    if (!leftHasData && !rightHasData) return;

    const auto & leftOrigins = leftHasData ? collectOriginsCached(positions, left) : emptyOrigins;
    const auto & rightOrigins = rightHasData ? collectOriginsCached(positions, right) : emptyOrigins;
    if (leftOrigins.empty() && rightOrigins.empty()) return;

    auto & leftAttrs = *left.attrs();
    auto & rightAttrs = *right.attrs();

    auto & pools = access->tracingPools();

    // Per-operand precomputation of the dep-recording shape used by
    // recordAbsent. Origins split into non-directory and directory groups;
    // when there are >1 directory origins, the aggregate DirSet hash and
    // its interned StringId are computed once here so the per-missing-key
    // recordAbsent body only reuses them. Without this, sorting and hashing
    // the directory origins dominated the cold-path profile (once per
    // missing key, for large attrsets produced by `//`).
    struct AbsentOperand {
        std::vector<const IntersectOriginInfo *> nonDirOrigins;
        std::vector<const IntersectOriginInfo *> dirOriginInfos;
        /// True iff dirOriginInfos.size() > 1; then dirSetHashId is set.
        bool aggregate = false;
        StringId dirSetHashId{};
    };

    auto buildAbsentOperand = [&](const std::vector<IntersectOriginInfo> & origins) {
        AbsentOperand a;
        a.nonDirOrigins.reserve(origins.size());
        a.dirOriginInfos.reserve(origins.size());
        std::vector<std::pair<DepSourceId, FilePathId>> dirOrigins;
        dirOrigins.reserve(origins.size());
        for (auto & oi : origins) {
            if (oi.format == StructuredFormat::Directory) {
                a.dirOriginInfos.push_back(&oi);
                dirOrigins.push_back({oi.sourceId, oi.filePathId});
            } else {
                a.nonDirOrigins.push_back(&oi);
            }
        }
        if (dirOrigins.size() > 1) {
            a.aggregate = true;
            auto [dsHash, dsDef] = computeDirSetHashAndDefinition(dirOrigins);
            if (!pools.dirSets.contains(dsHash))
                pools.dirSets[dsHash] = std::move(dsDef);
            a.dirSetHashId = pools.intern<StringId>(dsHash);
        }
        return a;
    };

    // An empty origins list is guarded by the !origins.empty() checks at
    // the call sites, so we only build the helper for side(s) actually
    // used.
    AbsentOperand leftAbsent;
    AbsentOperand rightAbsent;
    if (!rightOrigins.empty()) rightAbsent = buildAbsentOperand(rightOrigins);
    if (!leftOrigins.empty()) leftAbsent = buildAbsentOperand(leftOrigins);

    // Record exists=true for a single attr against its operand's origins
    auto recordExists = [&](const Attr & attr, const std::vector<IntersectOriginInfo> &) {
        if (!attr.pos.isTracedData()) return;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) return;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) return;
        auto & rec = resolveProvenanceRef(pools, *pr);
        CompactDepComponents c{rec.sourceId, rec.filePathId, rec.format, rec.dataPathId,
                               ShapeSuffix::None, pools.intern<StringId>(symbols[attr.name]), StringId()};
        recordStructuredDep(pools, c, DepHashValue(sentinel(SentinelHash::One)));
    };

    // Record exists=false using a prepared AbsentOperand: non-directory
    // origins are recorded individually; directory origins are recorded as
    // a single aggregate dep (via the pre-interned dirSetHashId) when
    // there are >1, otherwise individually.
    auto recordAbsent = [&](Symbol keyName, const AbsentOperand & a) {
        auto keyId = pools.intern<StringId>(symbols[keyName]);
        for (auto * oi : a.nonDirOrigins) {
            CompactDepComponents c{oi->sourceId, oi->filePathId, oi->format, oi->dataPathId,
                                   ShapeSuffix::None, keyId, StringId()};
            recordStructuredDep(pools, c, DepHashValue(sentinel(SentinelHash::Zero)));
        }
        if (a.aggregate) {
            CompactDepComponents c{
                DepSourceId(),
                FilePathId(),
                StructuredFormat::Directory,
                pools.dataPathPool.root(),
                ShapeSuffix::None,
                keyId,
                a.dirSetHashId,
            };
            recordStructuredDep(pools, c, DepHashValue(sentinel(SentinelHash::Zero)));
        } else {
            for (auto * oi : a.dirOriginInfos) {
                CompactDepComponents c{oi->sourceId, oi->filePathId, oi->format, oi->dataPathId,
                                       ShapeSuffix::None, keyId, StringId()};
                recordStructuredDep(pools, c, DepHashValue(sentinel(SentinelHash::Zero)));
            }
        }
    };

    for (auto & l : leftAttrs) {
        auto * r = rightAttrs.get(l.name);
        if (r) {
            if (!rightOrigins.empty()) recordExists(*r, rightOrigins);
            if (!leftOrigins.empty()) recordExists(l, leftOrigins);
        } else {
            if (!rightOrigins.empty()) recordAbsent(l.name, rightAbsent);
        }
    }

    if (!leftOrigins.empty()) {
        for (auto & r : rightAttrs) {
            if (!leftAttrs.get(r.name))
                recordAbsent(r.name, leftAbsent);
        }
    }
}

// ── DirSet aggregation for has-key-miss deps ─────────────────────────

/**
 * Compute a deterministic eval-trace hash for a set of directory origins.
 * Sorts by (source, filePath) for determinism regardless of // operand order.
 */
[[gnu::cold]]
/// Sort DirSet origins and compute hash in a single pass.
/// Returns (hash_hex, sorted_definition). Eliminates the double-sort
/// that occurred when computeDirSetHash and normalizeDirSetDefinition
/// were called separately.
///
/// Pre-resolves interned strings before sorting to avoid O(n log n)
/// redundant resolve calls in the sort comparator.
static std::pair<std::string, DirSetDefinition> computeDirSetHashAndDefinition(
    const std::vector<std::pair<DepSourceId, FilePathId>> & dirs)
{
    auto access = eval_trace::TraceAccess::current();
    if (!access) return {"", {}};
    auto & pools = access->tracingPools();

    // Pre-resolve strings once (O(n)), then sort on resolved views (O(n log n)
    // comparisons without repeated resolveRaw calls).
    struct ResolvedOrigin {
        DirSetOrigin origin;
        std::string_view source;
        std::string_view filePath;
    };
    std::vector<ResolvedOrigin> resolved;
    resolved.reserve(dirs.size());
    for (auto & [sourceId, filePathId] : dirs)
        resolved.push_back({
            DirSetOrigin{sourceId, filePathId},
            pools.resolve(sourceId),
            pools.resolve(filePathId)});

    std::sort(resolved.begin(), resolved.end(),
        [](const auto & a, const auto & b) {
            if (a.source != b.source) return a.source < b.source;
            return a.filePath < b.filePath;
        });

    // Build cache key + check cache.
    DirSetKey cacheKey;
    cacheKey.sorted.reserve(resolved.size());
    for (auto & r : resolved)
        cacheKey.sorted.emplace_back(r.origin.sourceId, r.origin.filePathId);
    if (auto cached = access->lookupDirSetHash(cacheKey)) {
        DirSetDefinition def;
        def.reserve(resolved.size());
        for (auto & r : resolved)
            def.push_back(r.origin);
        return {std::string(*cached), std::move(def)};
    }

    // Compute hash from pre-resolved strings (no additional resolveRaw calls).
    HashSink sink(eval_trace::toHashAlgorithm(eval_trace::getEvalTraceHashAlgorithm()));
    DirSetDefinition def;
    def.reserve(resolved.size());
    for (auto & r : resolved) {
        sink(r.source);
        sink(std::string_view("\0", 1));
        sink(r.filePath);
        sink(std::string_view("\0", 1));
        def.push_back(r.origin);
    }
    auto hex = EvalTraceHash::fromHash(sink.finish().hash).toHex();

    access->cacheDirSetHash(std::move(cacheKey), hex);
    return {hex, std::move(def)};
}

/**
 * Record has-key-miss deps, aggregating directory origins when >1 exist.
 */
[[gnu::cold]]
static void recordHasKeyMissDeps(
    const PosTable & positions, const SymbolTable & symbols,
    const Value & v, Symbol keyName)
{
    auto access = eval_trace::TraceAccess::current();
    if (!access) return;
    std::vector<std::pair<DepSourceId, FilePathId>> dirOrigins;
    std::vector<const ProvenanceRecord *> nonDirOrigins;

    forEachTracedDataOrigin(positions, v, [&](const ProvenanceRecord & df) {
        if (df.format == StructuredFormat::Directory)
            dirOrigins.push_back({df.sourceId, df.filePathId});
        else
            nonDirOrigins.push_back(&df);
    });

    auto & pools = access->tracingPools();

    // Non-directory origins: always individual deps
    for (auto * df : nonDirOrigins) {
        CompactDepComponents c{df->sourceId, df->filePathId, df->format, df->dataPathId,
                               ShapeSuffix::None, pools.intern<StringId>(symbols[keyName]), StringId()};
        recordStructuredDep(pools, c, DepHashValue(sentinel(SentinelHash::Zero)));
    }

    // Directory origins: aggregate when >1
    if (dirOrigins.size() > 1) {
        auto [dsHash, dsDef] = computeDirSetHashAndDefinition(dirOrigins);
        if (!pools.dirSets.contains(dsHash))
            pools.dirSets[dsHash] = std::move(dsDef);
        CompactDepComponents c{
            DepSourceId(),
            FilePathId(),
            StructuredFormat::Directory,
            pools.dataPathPool.root(),
            ShapeSuffix::None,
            pools.intern<StringId>(symbols[keyName]),
            pools.intern<StringId>(dsHash),
        };
        recordStructuredDep(pools, c, DepHashValue(sentinel(SentinelHash::Zero)));
    } else {
        forEachTracedDataOrigin(positions, v, [&](const ProvenanceRecord & df) {
            if (df.format != StructuredFormat::Directory) return;
            CompactDepComponents c{df.sourceId, df.filePathId, df.format, df.dataPathId,
                                   ShapeSuffix::None, pools.intern<StringId>(symbols[keyName]), StringId()};
            recordStructuredDep(pools, c, DepHashValue(sentinel(SentinelHash::Zero)));
        });
    }
}

[[gnu::cold]] void maybeRecordHasKeyDep(const PosTable & positions, const SymbolTable & symbols,
                          const Value & v, Symbol keyName, bool exists)
{
    auto access = eval_trace::TraceAccess::current();
    if (!access) return;
    if (v.type() != nAttrs) return;
    if (!v.attrs()->hasAnyTracedDataLayer()) return;

    if (exists) {
        auto * attr = v.attrs()->get(keyName);
        if (!attr || !attr->pos.isTracedData()) return;
        auto * origin = positions.originOfPtr(attr->pos);
        if (!origin) return;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) return;
        auto & pools = access->tracingPools();
        auto & rec = resolveProvenanceRef(pools, *pr);
        CompactDepComponents c{rec.sourceId, rec.filePathId, rec.format, rec.dataPathId,
                               ShapeSuffix::None, pools.intern<StringId>(symbols[keyName]), StringId()};
        recordStructuredDep(pools, c, DepHashValue(sentinel(SentinelHash::One)));
    } else {
        recordHasKeyMissDeps(positions, symbols, v, keyName);
    }
}

} // namespace nix
