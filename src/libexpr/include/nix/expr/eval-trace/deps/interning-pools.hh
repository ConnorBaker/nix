#pragma once
/// @file
/// InterningPools — owns all Lifetime 1 interning pools for eval-trace.
/// Owned by EvalTraceContext (one per EvalState).

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/symbol-table.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

template <typename Id>
struct StringPool {
    using Repr = decltype(Id::value);
    std::vector<std::string> strings;
    boost::unordered_flat_map<std::string, Repr> lookup;
#ifndef NDEBUG
    uint32_t generation = 0;
#endif

    Id intern(std::string_view sv) {
        std::string key(sv);
        auto it = lookup.find(key);
        if (it != lookup.end()) {
#ifndef NDEBUG
            return Id(it->second, generation);
#else
            return Id(it->second);
#endif
        }
        Repr id = static_cast<Repr>(strings.size());
        strings.push_back(key);
        lookup.emplace(std::move(key), id);
#ifndef NDEBUG
        return Id(id, generation);
#else
        return Id(id);
#endif
    }

    std::string_view resolve(Id id) const {
#ifndef NDEBUG
        assert(id.generation == generation && "StrongId used after pool clear");
#endif
        assert(id.value < strings.size());
        return strings[id.value];
    }

    void clear() {
        strings.clear();
        lookup.clear();
#ifndef NDEBUG
        generation++;
#endif
    }
};

struct DataPathNode {
    uint32_t parentId;      ///< 0 = root
    std::string component;  ///< object key (resolved string, not Symbol)
    int32_t arrayIndex;     ///< -1 if object key, >=0 if array index
};

struct DataPathPool {
    std::vector<DataPathNode> nodes;
    boost::unordered_flat_map<uint64_t, uint32_t> lookup;
#ifndef NDEBUG
    uint32_t generation = 0;
#endif

    DataPathPool() {
        nodes.push_back({0, "", -1}); // root sentinel
    }

    /// Return the root sentinel DataPathId (node 0) with correct generation.
    DataPathId root() const {
#ifndef NDEBUG
        return DataPathId(0, generation);
#else
        return DataPathId(0);
#endif
    }

    DataPathId internChild(DataPathId parentId, std::string_view key) {
#ifndef NDEBUG
        assert(parentId.generation == generation && "DataPathId used after pool clear");
#endif
        uint64_t h = hashValues(uint8_t(0), parentId.value, std::hash<std::string_view>{}(key));
        auto [it, inserted] = lookup.try_emplace(h, 0);
        if (!inserted) {
#ifndef NDEBUG
            return DataPathId(it->second, generation);
#else
            return DataPathId(it->second);
#endif
        }
        uint32_t id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({parentId.value, std::string(key), -1});
        it->second = id;
#ifndef NDEBUG
        return DataPathId(id, generation);
#else
        return DataPathId(id);
#endif
    }

    DataPathId internArrayChild(DataPathId parentId, int32_t index) {
#ifndef NDEBUG
        assert(parentId.generation == generation && "DataPathId used after pool clear");
#endif
        uint64_t h = hashValues(uint8_t(1), parentId.value, index);
        auto [it, inserted] = lookup.try_emplace(h, 0);
        if (!inserted) {
#ifndef NDEBUG
            return DataPathId(it->second, generation);
#else
            return DataPathId(it->second);
#endif
        }
        uint32_t id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({parentId.value, "", index});
        it->second = id;
#ifndef NDEBUG
        return DataPathId(id, generation);
#else
        return DataPathId(id);
#endif
    }

    /// Build path components from a trie node.
    std::vector<DataPathNode> collectPath(DataPathId nodeId) const {
#ifndef NDEBUG
        assert(nodeId.generation == generation && "DataPathId used after pool clear");
#endif
        std::vector<DataPathNode> path;
        uint32_t cur = nodeId.value;
        while (cur != 0) {
            path.push_back(nodes[cur]);
            cur = nodes[cur].parentId;
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    void clear() {
        nodes.clear();
        lookup.clear();
        nodes.push_back({0, "", -1});
#ifndef NDEBUG
        generation++;
#endif
    }
};

/**
 * Owns all Lifetime 1 interning pools for eval-trace dep recording.
 * Owned by EvalTraceContext (one per EvalState), providing automatic
 * test isolation.
 */
struct InterningPools {
    InterningPools() = default;
    ~InterningPools() = default;
    InterningPools(const InterningPools &) = delete;
    InterningPools & operator=(const InterningPools &) = delete;

    StringPool<DepSourceId> depSourcePool;
    StringPool<FilePathId> filePathPool;
    DataPathPool dataPathPool;
    StringPool<DepKeyId> depKeyPool;
    const SymbolTable * sessionSymbols = nullptr;
    ProvenanceTable provenanceTable;
};

} // namespace nix
