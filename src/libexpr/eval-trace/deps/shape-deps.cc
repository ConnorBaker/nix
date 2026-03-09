#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/root-tracker-scope.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/value.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/hash.hh"
#include "nix/util/pos-table.hh"

#include <boost/unordered/unordered_flat_set.hpp>

#include <nlohmann/json.hpp>

namespace nix {

// Cached constant Blake3Hash values used in shape dep recording.
// Function-local statics avoid static initialization order issues across TUs.
static const Blake3Hash & kHashZero()   { static const auto h = depHash("0"); return h; }
static const Blake3Hash & kHashOne()    { static const auto h = depHash("1"); return h; }
static const Blake3Hash & kHashObject() { static const auto h = depHash("object"); return h; }
static const Blake3Hash & kHashArray()  { static const auto h = depHash("array"); return h; }

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
static void forEachTracedDataOrigin(const PosTable & positions, const Value & v, Fn && fn)
{
    boost::unordered_flat_set<uint32_t> seen;
    for (auto & attr : *v.attrs()) {
        if (!attr.pos.isTracedData()) continue;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) continue;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) continue;
        if (!seen.insert(pr->id).second) continue;
        auto & rec = resolveProvenanceRef(DependencyTracker::activeTracker->pools, *pr);
        fn(rec);
    }
}

[[gnu::cold]] void maybeRecordListLenDep(const Value & v)
{
    if (!DependencyTracker::isActive()) return;
    if (v.listSize() == 0) return; // Empty lists can't be tracked (no stable key)
    // Use first element Value* as key (matches registration in ExprTracedData::eval)
    auto * scope = RootTrackerScope::current;
    auto * prov = scope ? scope->lookupTracedContainer((const void *)v.listView()[0]) : nullptr;
    if (!prov) return;
    auto hash = depHash(std::to_string(v.listSize()));
    auto & pools = DependencyTracker::activeTracker->pools;
    CompactDepComponents c{prov->sourceId, prov->filePathId, prov->format, prov->dataPathId,
                           ShapeSuffix::Len, Symbol{}};
    recordStructuredDep(pools, c, DepHashValue(hash));
}

[[gnu::cold]] void maybeRecordAttrKeysDep(const PosTable & positions, const SymbolTable & symbols, const Value & v)
{
    if (!DependencyTracker::isActive()) return;
    if (v.type() != nAttrs) return;
    if (!v.attrs()->hasAnyTracedDataLayer()) return;

    // Memoize: skip if we've already scanned this exact Bindings*.
    auto * scope = RootTrackerScope::current;
    if (!scope) return;
    if (!scope->scannedBindings.insert(v.attrs()).second) return;

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
        auto & rec = resolveProvenanceRef(DependencyTracker::activeTracker->pools, *pr);
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

    auto & pools = DependencyTracker::activeTracker->pools;
    for (auto & g : groups) {
        // Fast path: if all original keys are visible (no shadowing by //),
        // use the precomputed hash from ExprTracedData::eval() creation time.
        auto pcIt = scope->precomputedKeysMap.find(g.originOffset);
        if (pcIt != scope->precomputedKeysMap.end() && pcIt->second.keyCount == g.keys.size()) {
            auto & info = pcIt->second;
            CompactDepComponents c{info.sourceId, info.filePathId, g.df->format,
                                   info.dataPathId, ShapeSuffix::Keys, Symbol{}};
            recordStructuredDep(pools, c, DepHashValue(info.hash));
            continue;
        }
        // Slow path REMOVED: when keys are shadowed by //, ImplicitShape
        // (recorded at creation time with ALL source keys) handles verification.
    }
}

[[gnu::cold]] void maybeRecordTypeDep(const PosTable & positions, const Value & v)
{
    if (!DependencyTracker::isActive()) return;

    switch (v.type()) {
    case nAttrs: {
        if (!v.attrs()->hasAnyTracedDataLayer()) return;
        forEachTracedDataOrigin(positions, v, [&](const ProvenanceRecord & df) {
            CompactDepComponents c{df.sourceId, df.filePathId, df.format, df.dataPathId,
                                   ShapeSuffix::Type, Symbol{}};
            recordStructuredDep(DependencyTracker::activeTracker->pools, c, DepHashValue(kHashObject()));
        });
        break;
    }
    case nList: {
        if (v.listSize() == 0) return;
        auto * scope = RootTrackerScope::current;
        auto * prov = scope ? scope->lookupTracedContainer((const void *)v.listView()[0]) : nullptr;
        if (!prov) return;
        CompactDepComponents c{prov->sourceId, prov->filePathId, prov->format,
                               prov->dataPathId, ShapeSuffix::Type, Symbol{}};
        recordStructuredDep(DependencyTracker::activeTracker->pools, c, DepHashValue(kHashArray()));
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
static std::string computeDirSetHash(const std::vector<std::pair<DepSourceId, FilePathId>> & dirs);
static std::string buildAggregatedHasKeyJson(
    const std::string & dsHash, std::string_view keyName,
    const std::vector<std::pair<DepSourceId, FilePathId>> & dirs);

static const std::vector<IntersectOriginInfo> emptyOrigins;

static const std::vector<IntersectOriginInfo> & collectOriginsCached(
    const PosTable & positions, const Value & v)
{
    auto * scope = RootTrackerScope::current;
    if (!scope) return emptyOrigins;
    auto * bindings = v.attrs();
    auto [it, inserted] = scope->intersectOriginsCache.try_emplace(bindings);
    if (!inserted)
        return it->second;

    auto & result = it->second;
    boost::unordered_flat_set<uint32_t> seen;
    for (auto & attr : *bindings) {
        if (!attr.pos.isTracedData()) continue;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) continue;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) continue;
        if (!seen.insert(pr->id).second) continue;
        auto & rec = resolveProvenanceRef(DependencyTracker::activeTracker->pools, *pr);
        result.push_back({rec.sourceId, rec.filePathId, rec.dataPathId, rec.format});
    }
    return result;
}

[[gnu::cold]] void recordIntersectAttrsDeps(const PosTable & positions, const SymbolTable & symbols,
                                            const Value & left, const Value & right)
{
    if (!DependencyTracker::isActive()) return;

    bool leftHasData = left.type() == nAttrs && left.attrs()->hasAnyTracedDataLayer();
    bool rightHasData = right.type() == nAttrs && right.attrs()->hasAnyTracedDataLayer();
    if (!leftHasData && !rightHasData) return;

    const auto & leftOrigins = leftHasData ? collectOriginsCached(positions, left) : emptyOrigins;
    const auto & rightOrigins = rightHasData ? collectOriginsCached(positions, right) : emptyOrigins;
    if (leftOrigins.empty() && rightOrigins.empty()) return;

    auto & leftAttrs = *left.attrs();
    auto & rightAttrs = *right.attrs();

    auto & pools = DependencyTracker::activeTracker->pools;

    // Record exists=true for a single attr against its operand's origins
    auto recordExists = [&](const Attr & attr, const std::vector<IntersectOriginInfo> &) {
        if (!attr.pos.isTracedData()) return;
        auto * origin = positions.originOfPtr(attr.pos);
        if (!origin) return;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) return;
        auto & rec = resolveProvenanceRef(pools, *pr);
        CompactDepComponents c{rec.sourceId, rec.filePathId, rec.format, rec.dataPathId,
                               ShapeSuffix::None, attr.name};
        recordStructuredDep(pools, c, DepHashValue(kHashOne()));
    };

    // Record exists=false, aggregating directory origins when >1
    auto recordAbsent = [&](Symbol keyName, const std::vector<IntersectOriginInfo> & origins) {
        std::vector<std::pair<DepSourceId, FilePathId>> dirOrigins;
        for (auto & oi : origins) {
            if (oi.format == StructuredFormat::Directory)
                dirOrigins.push_back({oi.sourceId, oi.filePathId});
            else {
                CompactDepComponents c{oi.sourceId, oi.filePathId, oi.format, oi.dataPathId,
                                       ShapeSuffix::None, keyName};
                recordStructuredDep(pools, c, DepHashValue(kHashZero()));
            }
        }
        if (dirOrigins.size() > 1) {
            auto dsHash = computeDirSetHash(dirOrigins);
            auto key = buildAggregatedHasKeyJson(dsHash, symbols[keyName], dirOrigins);
            DependencyTracker::record(pools, DepType::StructuredContent, "", key, DepHashValue(kHashZero()));
        } else {
            for (auto & oi : origins) {
                if (oi.format != StructuredFormat::Directory) continue;
                CompactDepComponents c{oi.sourceId, oi.filePathId, oi.format, oi.dataPathId,
                                       ShapeSuffix::None, keyName};
                recordStructuredDep(pools, c, DepHashValue(kHashZero()));
            }
        }
    };

    for (auto & l : leftAttrs) {
        auto * r = rightAttrs.get(l.name);
        if (r) {
            if (!rightOrigins.empty()) recordExists(*r, rightOrigins);
            if (!leftOrigins.empty()) recordExists(l, leftOrigins);
        } else {
            if (!rightOrigins.empty()) recordAbsent(l.name, rightOrigins);
        }
    }

    if (!leftOrigins.empty()) {
        for (auto & r : rightAttrs) {
            if (!leftAttrs.get(r.name))
                recordAbsent(r.name, leftOrigins);
        }
    }
}

// ── DirSet aggregation for has-key-miss deps ─────────────────────────

/**
 * Compute a deterministic BLAKE3 hash for a set of directory origins.
 * Sorts by (source, filePath) for determinism regardless of // operand order.
 */
[[gnu::cold]]
static std::string computeDirSetHash(const std::vector<std::pair<DepSourceId, FilePathId>> & dirs)
{
    auto & pools = DependencyTracker::activeTracker->pools;
    auto sorted = dirs;
    std::sort(sorted.begin(), sorted.end(),
        [&pools](const auto & a, const auto & b) {
            auto sa = pools.resolve(a.first);
            auto fa = pools.filePathPool.resolve(a.second);
            auto sb = pools.resolve(b.first);
            auto fb = pools.filePathPool.resolve(b.second);
            if (sa != sb) return sa < sb;
            return fa < fb;
        });

    // Cache lookup after sorting (same sorted set → same hash).
    DirSetKey cacheKey{sorted};
    auto * scope = RootTrackerScope::current;
    if (scope) {
        auto cacheIt = scope->dirSetHashCache.find(cacheKey);
        if (cacheIt != scope->dirSetHashCache.end())
            return cacheIt->second;
    }

    HashSink sink(HashAlgorithm::BLAKE3);
    for (auto & [srcId, fpId] : sorted) {
        sink(pools.resolve(srcId));
        sink(std::string_view("\0", 1));
        sink(pools.filePathPool.resolve(fpId));
        sink(std::string_view("\0", 1));
    }
    auto hash = sink.finish().hash;

    std::string hex;
    hex.reserve(64);
    for (size_t i = 0; i < hash.hashSize; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", hash.hash[i]);
        hex += buf;
    }

    if (scope)
        scope->dirSetHashCache.emplace(std::move(cacheKey), hex);
    return hex;
}

/**
 * Build JSON dep key for an aggregated DirSet has-key-miss dep.
 * The dirs array is stored once in pools.dirSets (keyed by dsHash)
 * and persisted to the DirSets table by TraceStore::flush().
 * The dep key itself is compact (~100 bytes) with no embedded dirs.
 */
[[gnu::cold]]
static std::string buildAggregatedHasKeyJson(
    const std::string & dsHash, std::string_view keyName,
    const std::vector<std::pair<DepSourceId, FilePathId>> & dirs)
{
    auto & pools = DependencyTracker::activeTracker->pools;

    // Lazily build and cache the dirs JSON (once per unique dsHash)
    if (!pools.dirSets.contains(dsHash)) {
        nlohmann::json dirArr = nlohmann::json::array();
        for (auto & [srcId, fpId] : dirs) {
            dirArr.push_back({std::string(pools.resolve(srcId)),
                              std::string(pools.filePathPool.resolve(fpId))});
        }
        pools.dirSets[dsHash] = dirArr.dump();
    }

    // Compact key — no dirs embedded (~100 bytes)
    nlohmann::json j;
    j["ds"] = dsHash;
    j["h"] = std::string(keyName);
    j["t"] = "d";
    return j.dump();
}

/**
 * Record has-key-miss deps, aggregating directory origins when >1 exist.
 */
[[gnu::cold]]
static void recordHasKeyMissDeps(
    const PosTable & positions, const SymbolTable & symbols,
    const Value & v, Symbol keyName)
{
    std::vector<std::pair<DepSourceId, FilePathId>> dirOrigins;
    std::vector<const ProvenanceRecord *> nonDirOrigins;

    forEachTracedDataOrigin(positions, v, [&](const ProvenanceRecord & df) {
        if (df.format == StructuredFormat::Directory)
            dirOrigins.push_back({df.sourceId, df.filePathId});
        else
            nonDirOrigins.push_back(&df);
    });

    auto & pools = DependencyTracker::activeTracker->pools;

    // Non-directory origins: always individual deps
    for (auto * df : nonDirOrigins) {
        CompactDepComponents c{df->sourceId, df->filePathId, df->format, df->dataPathId,
                               ShapeSuffix::None, keyName};
        recordStructuredDep(pools, c, DepHashValue(kHashZero()));
    }

    // Directory origins: aggregate when >1
    if (dirOrigins.size() > 1) {
        auto dsHash = computeDirSetHash(dirOrigins);
        auto key = buildAggregatedHasKeyJson(dsHash, symbols[keyName], dirOrigins);
        DependencyTracker::record(pools, DepType::StructuredContent, "", key, DepHashValue(kHashZero()));
    } else {
        forEachTracedDataOrigin(positions, v, [&](const ProvenanceRecord & df) {
            if (df.format != StructuredFormat::Directory) return;
            CompactDepComponents c{df.sourceId, df.filePathId, df.format, df.dataPathId,
                                   ShapeSuffix::None, keyName};
            recordStructuredDep(pools, c, DepHashValue(kHashZero()));
        });
    }
}

[[gnu::cold]] void maybeRecordHasKeyDep(const PosTable & positions, const SymbolTable & symbols,
                          const Value & v, Symbol keyName, bool exists)
{
    if (!DependencyTracker::isActive()) return;
    if (v.type() != nAttrs) return;
    if (!v.attrs()->hasAnyTracedDataLayer()) return;

    if (exists) {
        auto * attr = v.attrs()->get(keyName);
        if (!attr || !attr->pos.isTracedData()) return;
        auto * origin = positions.originOfPtr(attr->pos);
        if (!origin) return;
        auto * pr = std::get_if<Pos::ProvenanceRef>(origin);
        if (!pr) return;
        auto & pools = DependencyTracker::activeTracker->pools;
        auto & rec = resolveProvenanceRef(pools, *pr);
        CompactDepComponents c{rec.sourceId, rec.filePathId, rec.format, rec.dataPathId,
                               ShapeSuffix::None, keyName};
        recordStructuredDep(pools, c, DepHashValue(kHashOne()));
    } else {
        recordHasKeyMissDeps(positions, symbols, v, keyName);
    }
}

} // namespace nix
