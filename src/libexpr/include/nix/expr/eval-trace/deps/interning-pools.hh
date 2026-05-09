#pragma once
/// @file
/// InterningPools — owns all Lifetime 1 interning pools for eval-trace.
/// Owned by TraceRuntime (one per EvalState).

#include "nix/expr/eval-trace/deps/input-resolution-internal.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/error.hh"
#include "nix/util/string-intern-table.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <cassert>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace nix {

struct DataPathNode {
    uint32_t parentId = 0;  ///< 0 = root
    std::string component;  ///< object key (resolved string, not Symbol)
    int32_t arrayIndex = -1; ///< -1 if object key, >=0 if array index
};

struct DataPathNodeKey {
    uint32_t parentId = 0;
    std::string component;
    int32_t arrayIndex = -1;

    bool operator==(const DataPathNodeKey &) const = default;

    struct Hash {
        // No `is_avalanching` marker.  `hashValues` is `hash_combine`
        // over `std::hash<size_t>`; on libstdc++ `std::hash<size_t>` is
        // identity, so the combine does not avalanche.  Entries
        // sharing a `parentId` would cluster without a post-mixer.
        size_t operator()(const DataPathNodeKey & key) const noexcept
        {
            return hashValues(key.parentId, key.arrayIndex, key.component);
        }
    };
};

struct DataPathPool {
    std::vector<DataPathNode> nodes;
    boost::unordered_flat_map<DataPathNodeKey, uint32_t, DataPathNodeKey::Hash> lookup;

    DataPathPool() {
        nodes.push_back({0, "", -1}); // root sentinel
    }

    /// Return the root sentinel DataPathId (node 0).
    DataPathId root() const { return DataPathId(0); }

    /// Wrap a raw persisted node index.
    DataPathId fromRaw(uint32_t id) const { return DataPathId(id); }

    DataPathId internChild(DataPathId parentId, std::string_view key) {
        DataPathNodeKey lookupKey{
            .parentId = parentId.value,
            .component = std::string(key),
            .arrayIndex = -1,
        };
        auto it = lookup.find(lookupKey);
        if (it != lookup.end())
            return DataPathId(it->second);
        uint32_t id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({parentId.value, lookupKey.component, -1});
        lookup.emplace(std::move(lookupKey), id);
        return DataPathId(id);
    }

    DataPathId internArrayChild(DataPathId parentId, int32_t index) {
        DataPathNodeKey lookupKey{
            .parentId = parentId.value,
            .component = "",
            .arrayIndex = index,
        };
        auto it = lookup.find(lookupKey);
        if (it != lookup.end())
            return DataPathId(it->second);
        uint32_t id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({parentId.value, "", index});
        lookup.emplace(std::move(lookupKey), id);
        return DataPathId(id);
    }

    /// Build path components from a trie node.
    std::vector<DataPathNode> collectPath(DataPathId nodeId) const {
        std::vector<DataPathNode> path;
        uint32_t cur = nodeId.value;
        while (cur != 0) {
            path.push_back(nodes[cur]);
            cur = nodes[cur].parentId;
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    void bulkLoad(
        uint32_t id,
        uint32_t parentId,
        std::string component,
        int32_t arrayIndex)
    {
        if (id >= nodes.size())
            nodes.resize(id + 1);
        nodes[id] = DataPathNode{parentId, std::move(component), arrayIndex};
        lookup.emplace(DataPathNodeKey{
            .parentId = parentId,
            .component = nodes[id].component,
            .arrayIndex = arrayIndex,
        }, id);
    }

    uint32_t nextId() const
    {
        return static_cast<uint32_t>(nodes.size());
    }
};

struct DirSetOrigin {
    DepSourceId sourceId;
    FilePathId filePathId;

    bool operator==(const DirSetOrigin &) const = default;
};

using DirSetDefinition = std::vector<DirSetOrigin>;

/**
 * Owns all Lifetime 1 interning pools for eval-trace dep recording.
 * Owned by TraceRuntime (one per EvalState), providing automatic
 * test isolation.
 *
 * DepSourceId, SimpleDepKeyId, the typed dep-key IDs, FilePathId, and StringId
 * all share the same StringInternTable (arena-backed, zero per-string heap
 * allocation). The shared substrate is an implementation detail; callers
 * should stay in the typed ID domains until true serialization.
 */
struct InterningPools {
    InterningPools() = default;
    ~InterningPools() = default;
    InterningPools(const InterningPools &) = delete;
    InterningPools & operator=(const InterningPools &) = delete;

    friend DerivedStorePathDepKey decodeDerivedStorePathDepKey(const InterningPools &, DerivedStorePathDepKeyId);
    friend StorePathAvailabilityDepKey decodeStorePathAvailabilityDepKey(const InterningPools &, StorePathAvailabilityDepKeyId);
    friend RuntimeFetchIdentityDepKey decodeRuntimeFetchIdentityDepKey(const InterningPools &, RuntimeFetchIdentityDepKeyId);

private:
    /// Shared arena-backed string intern table for dep-source/dep-key/string IDs.
    StringInternTable strings_;

    template<typename Id>
    Id internDepKeyBlob(std::string_view blob)
    {
        return Id{strings_.intern<DepKeyId>(blob)};
    }

    /// Internal escape hatch to the raw encoded dep-key blob substrate.
    /// Kept private so the typed dep-key surface remains the only public API.
    std::string_view resolveEncodedDepKeyBlobForPersistence(DepKeyId id) const
    {
        return strings_.resolve(id);
    }

public:

    DataPathPool dataPathPool;
    ProvenanceTable provenanceTable;

    /// Typed intern: delegates to strings.intern<Id>.
    template<typename Id>
    Id intern(std::string_view sv)
    {
        static_assert(!std::is_same_v<Id, DepSourceId>,
            "DepSourceId must be interned from DepSource, not raw strings");
        return strings_.intern<Id>(sv);
    }

    /// Resolve a RepoRootId back to its absolute path.  Id 0 returns "".
    std::string_view resolveRepoRoot(RepoRootId id) const
    {
        if (id.value == 0)
            return {};
        return strings_.resolve(id);
    }

    template<typename Id>
    Id intern(const DepSource & source)
    {
        static_assert(std::is_same_v<Id, DepSourceId>);
        auto encoded = encodeDepSourceBlob(source);
        return strings_.intern<Id>(encoded.value);
    }

    SimpleDepKeyId intern(const SimpleDepKeyAtom & key)
    {
        return strings_.intern<SimpleDepKeyId>(key.value);
    }

    DerivedStorePathDepKeyId intern(const DerivedStorePathDepKey & key)
    {
        auto encoded = encodeDerivedStorePathDepKey(key);
        static const uint8_t empty = 0;
        auto blob = std::string_view{
            reinterpret_cast<const char *>(encoded.value.empty() ? &empty : encoded.value.data()),
            encoded.value.size()};
        return internDepKeyBlob<DerivedStorePathDepKeyId>(blob);
    }

    StorePathAvailabilityDepKeyId intern(const StorePathAvailabilityDepKey & key)
    {
        auto encoded = encodeStorePathAvailabilityDepKey(key);
        static const uint8_t empty = 0;
        auto blob = std::string_view{
            reinterpret_cast<const char *>(encoded.value.empty() ? &empty : encoded.value.data()),
            encoded.value.size()};
        return internDepKeyBlob<StorePathAvailabilityDepKeyId>(blob);
    }

    RuntimeFetchIdentityDepKeyId intern(const RuntimeFetchIdentityDepKey & key)
    {
        auto encoded = encodeRuntimeFetchIdentityDepKey(key);
        static const uint8_t empty = 0;
        auto blob = std::string_view{
            reinterpret_cast<const char *>(encoded.value.empty() ? &empty : encoded.value.data()),
            encoded.value.size()};
        return internDepKeyBlob<RuntimeFetchIdentityDepKeyId>(blob);
    }

    /// Typed resolve: delegates to strings.resolve<Id>.
    template<typename Id>
    std::string_view resolve(Id id) const { return strings_.resolve(id); }

    std::string_view resolve(SimpleDepKeyId id) const
    {
        return strings_.resolve(eraseDepKeyType(id));
    }

    DerivedStorePathDepKey resolve(DerivedStorePathDepKeyId id) const
    {
        return decodeDerivedStorePathDepKey(*this, id);
    }

    StorePathAvailabilityDepKey resolve(StorePathAvailabilityDepKeyId id) const
    {
        return decodeStorePathAvailabilityDepKey(*this, id);
    }

    RuntimeFetchIdentityDepKey resolve(RuntimeFetchIdentityDepKeyId id) const
    {
        return decodeRuntimeFetchIdentityDepKey(*this, id);
    }

    DepSource resolveDepSource(DepSourceId id) const
    {
        auto parsed = decodeDepSourceBlob(strings_.resolve(id));
        if (!parsed)
            throw Error("internal error: invalid serialized dep source '%s'",
                std::string(strings_.resolve(id)));
        return std::move(*parsed);
    }

    void bulkLoadString(uint32_t id, std::string value)
    {
        strings_.bulkLoad(id, std::move(value));
    }

    uint32_t nextStringId() const
    {
        return strings_.nextId();
    }

    std::string_view resolveRawString(uint32_t id) const
    {
        return strings_.resolveRaw(id);
    }

    /// DirSet definitions: dsHash -> sorted directory origins.
    /// Populated during recording, persisted to the DirSets table by
    /// TraceStore::flush(), loaded from DB by TraceStore::bulkLoadAll().
    boost::unordered_flat_map<std::string, DirSetDefinition> dirSets;

    /// Resolve the git repo root that governs `absPath`, returning its
    /// interned id.  RepoRootId{} (value 0) means the path is outside any
    /// git repo (or the path is empty / relative).  Walks ancestors on
    /// cache miss; subsequent lookups are O(1).  Defined out-of-line in
    /// recording.cc (needs <filesystem>).
    ///
    /// Thread-safety: NOT thread-safe.  `governingRepoCache_` is a plain
    /// map with no lock.  The invariant is that `internGoverningRepo` is
    /// only called from the eval thread (the sole recording path — see
    /// callers in eval-environment.cc, input-resolution.cc, recording.cc).
    /// Verification reads `dep.key.governingRepoId` and resolves it via
    /// the thread-safe `StringInternTable` through `resolveRepoRoot`; it
    /// never calls `internGoverningRepo`.  If recording is ever
    /// parallelised, add a `Guarded<>` wrapper or migrate the cache to a
    /// concurrent map.
    RepoRootId internGoverningRepo(std::string_view absPath);

    /// Clear the governing-repo cache.  Used by test fixtures only.
    void clearGoverningRepoCache() { governingRepoCache_.clear(); }

private:
    /// Maps an absolute file path → its governing RepoRootId.  Populated
    /// by `internGoverningRepo`.  RepoRootId{} (value 0) is a valid cached
    /// answer meaning "not inside any repo".  See `internGoverningRepo`
    /// for the single-threaded-writer invariant.
    boost::unordered_flat_map<std::string, RepoRootId> governingRepoCache_;
};

} // namespace nix
