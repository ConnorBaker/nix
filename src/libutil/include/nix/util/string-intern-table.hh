#pragma once
/// @file
/// Arena-backed string intern table. Strings are copied once into a monotonic
/// buffer; IDs are indices into a vector of string_view. Dedup via
/// boost::unordered_flat_set with transparent hashing. Returns string_view on
/// resolve (zero copy). Thread-unsafe (single-writer, like SymbolTable).
///
/// Index 0 is a reserved sentinel (empty string_view). Real IDs start at 1.
/// This matches the StrongId convention where operator bool() returns false
/// for value == 0.

#include <boost/unordered/unordered_flat_set.hpp>

#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <string_view>
#include <vector>

namespace nix {

class StringInternTable {
    std::pmr::monotonic_buffer_resource arena{8192};
    std::vector<std::string_view> strings; // index → string_view into arena

    struct ViewHash {
        using is_transparent = void;
        const std::vector<std::string_view> * table;
        size_t operator()(uint32_t idx) const {
            return boost::hash<std::string_view>{}((*table)[idx]);
        }
        size_t operator()(std::string_view sv) const {
            return boost::hash<std::string_view>{}(sv);
        }
    };

    struct ViewEqual {
        using is_transparent = void;
        const std::vector<std::string_view> * table;
        bool operator()(uint32_t a, uint32_t b) const {
            return (*table)[a] == (*table)[b];
        }
        bool operator()(uint32_t a, std::string_view b) const {
            return (*table)[a] == b;
        }
        bool operator()(std::string_view a, uint32_t b) const {
            return a == (*table)[b];
        }
    };

    boost::unordered_flat_set<uint32_t, ViewHash, ViewEqual> dedup;

    /// Copy string into arena, return string_view into arena memory.
    std::string_view copyToArena(std::string_view sv);

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

    /// Resolve a raw index to string_view. O(1) vector lookup.
    std::string_view resolveRaw(uint32_t idx) const;

    /// Typed intern: wraps internRaw with StrongId construction.
    template<typename Id>
    Id intern(std::string_view sv) {
        return Id(internRaw(sv));
    }

    /// Typed resolve: unwraps StrongId, calls resolveRaw.
    template<typename Id>
    std::string_view resolve(Id id) const {
        return resolveRaw(id.value);
    }

    /// Bulk-load a string at a specific index (for DB population).
    /// Caller guarantees uniqueness. Inserts into dedup set for future lookups.
    /// IDs should be contiguous (1, 2, 3, ...); gaps are filled with empty
    /// string_view and will resolve to "".
    void bulkLoad(uint32_t id, std::string_view sv);

    /// Number of entries (including sentinel at index 0).
    size_t size() const { return strings.size(); }

    /// Next available ID (= size(), since IDs are 1-based contiguous).
    uint32_t nextId() const { return static_cast<uint32_t>(strings.size()); }

    /// Reset to initial state: release arena memory, clear all entries.
    void clear();
};

} // namespace nix
