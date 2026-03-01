#pragma once
/// @file
/// InterningPools — owns all Lifetime 1 interning pools for eval-trace.
/// Owned by EvalTraceContext (one per EvalState).

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/string-intern-table.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

namespace nix {

template <typename Id>
struct StringPool {
    using Repr = decltype(Id::value);

    struct Hash {
        using is_transparent = void;
        const std::vector<std::string> * strings;
        size_t operator()(Repr id) const {
            return boost::hash<std::string_view>{}((*strings)[id]);
        }
        size_t operator()(std::string_view sv) const {
            return boost::hash<std::string_view>{}(sv);
        }
    };

    struct Equal {
        using is_transparent = void;
        const std::vector<std::string> * strings;
        bool operator()(Repr a, Repr b) const {
            return (*strings)[a] == (*strings)[b];
        }
        bool operator()(Repr a, std::string_view b) const {
            return (*strings)[a] == b;
        }
        bool operator()(std::string_view a, Repr b) const {
            return a == (*strings)[b];
        }
    };

    std::vector<std::string> strings;
    boost::unordered_flat_set<Repr, Hash, Equal> lookup;
#ifndef NDEBUG
    uint32_t generation = 0;
#endif

    StringPool() : lookup(0, Hash{&strings}, Equal{&strings}) {}

    StringPool(const StringPool &) = delete;
    StringPool & operator=(const StringPool &) = delete;
    StringPool(StringPool &&) = delete;
    StringPool & operator=(StringPool &&) = delete;

    Id intern(std::string_view sv) {
        auto it = lookup.find(sv);
        if (it != lookup.end()) {
#ifndef NDEBUG
            return Id(*it, generation);
#else
            return Id(*it);
#endif
        }
        Repr id = static_cast<Repr>(strings.size());
        strings.emplace_back(sv);
        lookup.insert(id);
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
 *
 * DepSourceId, DepKeyId, and StringId all share the same StringInternTable
 * (arena-backed, zero per-string heap allocation). FilePathId uses a separate
 * StringPool (session-only, not persisted to DB).
 */
struct InterningPools {
    InterningPools() = default;
    ~InterningPools() = default;
    InterningPools(const InterningPools &) = delete;
    InterningPools & operator=(const InterningPools &) = delete;

    /// Shared arena-backed string intern table for DepSourceId, DepKeyId, StringId.
    StringInternTable strings;

    StringPool<FilePathId> filePathPool;
    DataPathPool dataPathPool;
    const SymbolTable * sessionSymbols = nullptr;
    ProvenanceTable provenanceTable;

    /// Typed intern: delegates to strings.intern<Id>.
    template<typename Id>
    Id intern(std::string_view sv) { return strings.intern<Id>(sv); }

    /// Typed resolve: delegates to strings.resolve<Id>.
    template<typename Id>
    std::string_view resolve(Id id) const { return strings.resolve(id); }
};

} // namespace nix
