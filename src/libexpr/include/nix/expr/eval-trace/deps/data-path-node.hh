#pragma once
/// @file
/// DataPathNode / DataPathNodeKey — the per-component nodes of an interned
/// structured-dep path. Extracted from interning-pools.hh into this minimal
/// header so input-resolution.hh can hold a `std::vector<DataPathNode>` by value
/// (in ResolvedDepKeyMaterial, RFC-perf H-cold-1) WITHOUT pulling the heavy
/// interning-pools.hh, which would create an include cycle
/// (interning-pools.hh → input-resolution-internal.hh → input-resolution.hh).

#include "nix/util/std-hash.hh"  // hashValues

#include <cstddef>
#include <cstdint>
#include <string>

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

} // namespace nix
