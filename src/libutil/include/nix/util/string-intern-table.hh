#pragma once
///@file
///
/// Thread-safe string intern table using ChunkedVector + concurrent_flat_set.
/// O(1) amortized intern, O(1) resolve. Uses boost::concurrent_flat_set
/// with transparent hashing. Returns string_view on resolve (zero copy).
///
/// Index 0 is a reserved sentinel (empty string). Real IDs start at 1.
/// This matches the Tagged convention where operator bool() returns false
/// for value == 0.

#include "nix/util/chunked-vector.hh"

#include <boost/unordered/concurrent_flat_set.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace nix {

class StringInternTable {
    static constexpr size_t chunkSize = 8192;
    using Store = ChunkedVector<std::string, chunkSize, std::numeric_limits<uint32_t>::max() / chunkSize>;

    Store store;

    /// Lookup key for concurrent_flat_set. Holds a string_view and pre-computed
    /// hash. Converts to uint32_t (the set's element type) only on insertion,
    /// calling store.add() to allocate the string in ChunkedVector.
    struct Key {
        Store & store;
        std::string_view sv;
        size_t hash;

        operator uint32_t() const {
            auto [s, idx] = store.add(std::string(sv));
            return idx;
        }
    };

    struct IdxHash {
        using is_transparent = void;
        using is_avalanching = std::true_type;
        const Store & store;

        size_t operator()(uint32_t idx) const {
            return boost::hash<std::string_view>{}(store[idx]);
        }
        size_t operator()(std::string_view sv) const {
            return boost::hash<std::string_view>{}(sv);
        }
        size_t operator()(const Key & k) const {
            return k.hash;
        }
    };

    struct IdxEqual {
        using is_transparent = void;
        const Store & store;

        bool operator()(uint32_t a, uint32_t b) const {
            return std::string_view(store[a]) == std::string_view(store[b]);
        }
        bool operator()(uint32_t a, std::string_view b) const {
            return std::string_view(store[a]) == b;
        }
        bool operator()(std::string_view a, uint32_t b) const {
            return a == std::string_view(store[b]);
        }
        bool operator()(uint32_t a, const Key & b) const {
            return std::string_view(store[a]) == b.sv;
        }
        bool operator()(const Key & a, uint32_t b) const {
            return a.sv == std::string_view(store[b]);
        }
    };

    boost::concurrent_flat_set<uint32_t, IdxHash, IdxEqual> dedup;

public:
    StringInternTable();
    ~StringInternTable() = default;

    StringInternTable(const StringInternTable &) = delete;
    StringInternTable & operator=(const StringInternTable &) = delete;
    StringInternTable(StringInternTable &&) = delete;
    StringInternTable & operator=(StringInternTable &&) = delete;

    /// Intern a string, returning a raw uint32_t index (1-based).
    /// Dedup: same string always returns the same index.
    uint32_t internRaw(std::string_view sv);

    /// Resolve a raw index to string_view. O(1) lookup.
    std::string_view resolveRaw(uint32_t idx) const;

    /// Typed intern: wraps internRaw with Tagged construction.
    template<typename Id>
    Id intern(std::string_view sv) {
        return Id(internRaw(sv));
    }

    /// Typed resolve: unwraps Tagged, calls resolveRaw.
    template<typename Id>
    std::string_view resolve(Id id) const {
        return resolveRaw(id.value);
    }

    /// Lookup a string without interning. Returns 0 if not found.
    uint32_t findRaw(std::string_view sv) const;

    /// Typed find: returns nullopt if the string is not interned.
    template<typename Id>
    std::optional<Id> find(std::string_view sv) const {
        auto idx = findRaw(sv);
        if (idx == 0 && !sv.empty()) return std::nullopt;
        return Id(idx);
    }

    /// Bulk-load a string at a specific index (for DB population).
    /// Caller guarantees ascending IDs. Inserts into dedup set for future lookups.
    /// IDs should be contiguous; gaps are filled with empty strings.
    void bulkLoad(uint32_t id, std::string_view sv);

    /// Number of entries (including sentinel at index 0).
    size_t size() const { return store.size(); }

    /// Next available ID (= size(), since IDs are 1-based contiguous).
    uint32_t nextId() const { return static_cast<uint32_t>(store.size()); }
};

} // namespace nix
