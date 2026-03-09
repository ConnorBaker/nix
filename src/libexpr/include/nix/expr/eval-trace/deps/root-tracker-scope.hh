#pragma once
///@file
/// RootTrackerScope: RAII container for Lifetime 2 (per root-tracker) caches.
/// Accessed by dep recording helpers, primops, json-to-value, fromTOML, and
/// materialize.cc via the thread_local RootTrackerScope::current pointer.

#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <deque>

namespace nix {

class Bindings;

struct DirSetKey {
    std::vector<std::pair<DepSourceId, FilePathId>> sorted;
    bool operator==(const DirSetKey &) const = default;
    struct Hash {
        size_t operator()(const DirSetKey & k) const noexcept {
            size_t seed = k.sorted.size();
            for (auto & [s, f] : k.sorted)
                hash_combine(seed, s.value, f.value);
            return seed;
        }
    };
};

struct IntersectOriginInfo {
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;
    StructuredFormat format;
};

/**
 * RAII container for all Lifetime 2 caches. Created when a root
 * DependencyTracker is constructed (depth 0->1), destroyed when the
 * root is destroyed (depth 1->0). All caches are automatically cleared
 * by the destructor -- adding a new L2 cache is just adding a field.
 *
 * Accessed via thread_local `current` pointer from dep recording helpers,
 * primops, and materialization code. Between root tracker scopes,
 * current is nullptr; callers must null-check before use.
 */
struct RootTrackerScope {
    static thread_local RootTrackerScope * current;
    RootTrackerScope * previous;

    // Value* -> list provenance. Lists use this (not PosIdx).
    boost::unordered_flat_map<const void*, const TracedContainerProvenance *> tracedContainerMap;

    // Skip re-scanning the same Bindings* in maybeRecordAttrKeysDep.
    boost::unordered_flat_set<const void *> scannedBindings;

    // Stable pool for TracedContainerProvenance data (deque = no pointer invalidation).
    std::deque<TracedContainerProvenance> provenancePool;

    // Origin offset -> precomputed keys hash.
    boost::unordered_flat_map<uint32_t, PrecomputedKeysInfo> precomputedKeysMap;

    // Sorted dir-set -> BLAKE3 hex hash cache.
    boost::unordered_flat_map<DirSetKey, std::string, DirSetKey::Hash> dirSetHashCache;

    // Bindings* -> origin info cache for intersectAttrs bulk recording.
    boost::unordered_flat_map<const Bindings *, std::vector<IntersectOriginInfo>> intersectOriginsCache;

    // Content hash -> ReadFileProvenance (from prim_readFile -> prim_fromJSON/fromTOML).
    boost::unordered_flat_map<Blake3Hash, ReadFileProvenance, Blake3Hash::Hasher> readFileProvenanceMap;

    // String data pointer -> content hash (for RawContent dep tracking).
    boost::unordered_flat_map<const char *, Blake3Hash> readFileStringPtrs;

    // ── Container provenance (lists) ──────────────────────────────────

    /** Allocate a stable TracedContainerProvenance and return a non-owning pointer.
     *  The record lives in provenancePool (deque, no pointer invalidation). */
    ProvenanceRef allocateProvenance(DepSourceId sourceId, FilePathId filePathId,
                                     DataPathId dataPathId, StructuredFormat format) {
        provenancePool.emplace_back(TracedContainerProvenance{sourceId, filePathId, dataPathId, format});
        return &provenancePool.back();
    }

    /** Associate a container Value (by pointer) with its provenance.
     *  For lists, key is the address of the first element. */
    void registerTracedContainer(const void * key, const TracedContainerProvenance * prov) {
        tracedContainerMap.emplace(key, prov);
    }

    /** Look up provenance for a container Value. Returns nullptr if not tracked. */
    const TracedContainerProvenance * lookupTracedContainer(const void * key) const {
        auto it = tracedContainerMap.find(key);
        return it != tracedContainerMap.end() ? it->second : nullptr;
    }

    // ── Precomputed keys ────────────────────────────────────────────────

    /** Store a precomputed keys hash from ExprTracedData::eval() for fast
     *  lookup by maybeRecordAttrKeysDep when all original keys are visible. */
    void registerPrecomputedKeys(uint32_t originOffset, PrecomputedKeysInfo info) {
        precomputedKeysMap.emplace(originOffset, std::move(info));
    }

    // ── ReadFile provenance ─────────────────────────────────────────────

    /** Record that prim_readFile produced a file with the given content hash.
     *  Consumed by prim_fromJSON/prim_fromTOML to connect parsed data to the source file. */
    void addReadFileProvenance(ReadFileProvenance prov) {
        readFileProvenanceMap.insert_or_assign(prov.contentHash, std::move(prov));
    }

    /** Look up readFile provenance by content hash. Returns nullptr if not found. */
    const ReadFileProvenance * lookupReadFileProvenance(const Blake3Hash & contentHash) const {
        auto it = readFileProvenanceMap.find(contentHash);
        return it != readFileProvenanceMap.end() ? &it->second : nullptr;
    }

    // ── ReadFile string pointer tracking ────────────────────────────────

    /** Track the string data pointer from a readFile result for RawContent dep recording.
     *  maybeRecordRawContentDep uses pointer identity to detect readFile-sourced strings. */
    void addReadFileStringPtr(const char * ptr, const Blake3Hash & contentHash) {
        readFileStringPtrs.emplace(ptr, contentHash);
    }

    // ── Lifecycle ───────────────────────────────────────────────────────

    RootTrackerScope() : previous(current) { current = this; }
    ~RootTrackerScope() { current = previous; }

    RootTrackerScope(const RootTrackerScope &) = delete;
    RootTrackerScope & operator=(const RootTrackerScope &) = delete;
};

} // namespace nix
