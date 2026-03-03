#pragma once
///@file Internal header for RootTrackerScope — shared between recording.cc, input-resolution.cc, and shape-deps.cc

#include "nix/expr/eval-trace/deps/recording.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <deque>

namespace nix {

struct Bindings;

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
 * A single thread_local non-owning pointer provides access from shape
 * dep free functions. Between root tracker scopes, current is nullptr.
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

    RootTrackerScope() : previous(current) { current = this; }
    ~RootTrackerScope() { current = previous; }

    RootTrackerScope(const RootTrackerScope &) = delete;
    RootTrackerScope & operator=(const RootTrackerScope &) = delete;
};

} // namespace nix
