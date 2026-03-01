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

struct StringPool16 {
    std::vector<std::string> strings;
    boost::unordered_flat_map<std::string, uint16_t> lookup;
#ifndef NDEBUG
    uint32_t generation = 0;
#endif

    uint16_t intern(std::string_view sv) {
        std::string key(sv);
        auto it = lookup.find(key);
        if (it != lookup.end()) return it->second;
        uint16_t id = static_cast<uint16_t>(strings.size());
        strings.push_back(key);
        lookup.emplace(std::move(key), id);
        return id;
    }

    std::string_view resolve(uint16_t id) const {
        assert(id < strings.size());
        return strings[id];
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

    DataPathPool() {
        nodes.push_back({0, "", -1}); // root sentinel
    }

    uint32_t internChild(uint32_t parentId, std::string_view key) {
        uint64_t h = hashValues(uint8_t(0), parentId, std::hash<std::string_view>{}(key));
        auto [it, inserted] = lookup.try_emplace(h, 0);
        if (!inserted) return it->second;
        uint32_t id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({parentId, std::string(key), -1});
        it->second = id;
        return id;
    }

    uint32_t internArrayChild(uint32_t parentId, int32_t index) {
        uint64_t h = hashValues(uint8_t(1), parentId, index);
        auto [it, inserted] = lookup.try_emplace(h, 0);
        if (!inserted) return it->second;
        uint32_t id = static_cast<uint32_t>(nodes.size());
        nodes.push_back({parentId, "", index});
        it->second = id;
        return id;
    }

    /// Build path components from a trie node. Returns the sequence of
    /// (component, arrayIndex) pairs from root to the given node.
    /// The JSON serialization is done by dataPathToJsonString() in recording.cc.
    std::vector<DataPathNode> collectPath(uint32_t nodeId) const {
        std::vector<DataPathNode> path;
        uint32_t cur = nodeId;
        while (cur != 0) {
            path.push_back(nodes[cur]);
            cur = nodes[cur].parentId;
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

#ifndef NDEBUG
    uint32_t generation = 0;
#endif

    void clear() {
        nodes.clear();
        lookup.clear();
        nodes.push_back({0, "", -1});
#ifndef NDEBUG
        generation++;
#endif
    }
};

struct StringPool32 {
    std::vector<std::string> strings;
    boost::unordered_flat_map<std::string, uint32_t> lookup;
#ifndef NDEBUG
    uint32_t generation = 0;
#endif

    uint32_t intern(std::string_view sv) {
        std::string key(sv);
        auto it = lookup.find(key);
        if (it != lookup.end()) return it->second;
        uint32_t id = static_cast<uint32_t>(strings.size());
        strings.push_back(key);
        lookup.emplace(std::move(key), id);
        return id;
    }

    std::string_view resolve(uint32_t id) const {
        assert(id < strings.size());
        return strings[id];
    }

    void clear() {
        strings.clear();
        lookup.clear();
#ifndef NDEBUG
        generation++;
#endif
    }
};

/**
 * Owns all Lifetime 1 interning pools for eval-trace dep recording.
 * Owned by EvalTraceContext (one per EvalState), providing automatic
 * test isolation. Accessed from free functions via the thread_local
 * `current` pointer.
 */
struct InterningPools {
    static thread_local InterningPools * current;

    StringPool16 depSourcePool;
    StringPool16 filePathPool;
    DataPathPool dataPathPool;
    StringPool32 depKeyPool;
    const SymbolTable * sessionSymbols = nullptr;
    ProvenanceTable provenanceTable;
};

} // namespace nix
