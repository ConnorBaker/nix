#pragma once

/**
 * Path Encoding for HVM4
 *
 * Paths in HVM4 are represented using an indexed approach:
 *   #Pth{accessor_id, path_string_id}
 *
 * Where:
 * - accessor_id is an index into an AccessorRegistry that maps to SourceAccessors
 * - path_string_id is a string table ID containing the path string
 *
 * This design keeps path evaluation pure - store operations are deferred
 * to result extraction time.
 *
 * Constructor Tags:
 * - CTR_PTH: Path with accessor ID and path string
 */

#include "nix/expr/hvm4/hvm4-runtime.hh"
#include "nix/expr/hvm4/hvm4-string.hh"
#include "nix/util/ref.hh"
#include <cstdint>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace nix {
    struct SourceAccessor;
    struct SourcePath;
}

namespace nix::hvm4 {

// Constructor tag for path encoding
// Using next available tag after strings and attrs
constexpr uint32_t CTR_PTH = 0x100040;  // Path: #Pth{accessor_id, path_string_id}

/**
 * Registry for SourceAccessors.
 *
 * Maps integer IDs to SourceAccessor pointers so that paths can be
 * represented in HVM4 as pure data (ID + string) without needing
 * to embed C++ pointers in the HVM4 heap.
 */
class AccessorRegistry {
public:
    /**
     * Register an accessor and return its ID.
     * If the accessor is already registered, returns the existing ID.
     */
    uint32_t registerAccessor(SourceAccessor* accessor);

    /**
     * Get an accessor by its ID.
     * Returns nullptr if ID is invalid.
     */
    SourceAccessor* getAccessor(uint32_t id) const;

    /**
     * Check if an ID is valid.
     */
    bool valid(uint32_t id) const;

    /**
     * Get the number of registered accessors.
     */
    size_t size() const { return accessors_.size(); }

    /**
     * Clear all registered accessors.
     */
    void clear();

private:
    std::vector<SourceAccessor*> accessors_;
    std::unordered_map<SourceAccessor*, uint32_t> index_;
};

/**
 * Create a path term.
 *
 * @param accessorId The accessor registry ID
 * @param pathStringId The string table ID for the path
 * @param runtime The HVM4 runtime
 * @return The constructed path term #Pth{accessor_id, path_string_id}
 */
Term makePath(uint32_t accessorId, uint32_t pathStringId, HVM4Runtime& runtime);

/**
 * Create a path term from a SourcePath.
 *
 * @param path The source path
 * @param accessorRegistry The accessor registry
 * @param stringTable The string table
 * @param runtime The HVM4 runtime
 * @return The constructed path term
 */
Term makePathFromSource(
    const SourcePath& path,
    AccessorRegistry& accessorRegistry,
    StringTable& stringTable,
    HVM4Runtime& runtime);

/**
 * Check if a term represents a path.
 */
bool isPath(Term term);

/**
 * Get the accessor ID from a path term.
 *
 * @param term A #Pth{accessor_id, path_string_id} term
 * @param runtime The HVM4 runtime
 * @return The accessor ID
 */
uint32_t getPathAccessorId(Term term, const HVM4Runtime& runtime);

/**
 * Get the path string ID from a path term.
 *
 * @param term A #Pth{accessor_id, path_string_id} term
 * @param runtime The HVM4 runtime
 * @return The path string ID
 */
uint32_t getPathStringId(Term term, const HVM4Runtime& runtime);

/**
 * Concatenate a path with a string suffix.
 *
 * In Nix, path + "/suffix" creates a new path.
 *
 * @param pathTerm The path term
 * @param suffixStringId The string ID of the suffix
 * @param stringTable The string table
 * @param runtime The HVM4 runtime
 * @return New path term with concatenated path
 */
Term concatPathString(
    Term pathTerm,
    uint32_t suffixStringId,
    StringTable& stringTable,
    HVM4Runtime& runtime);

}  // namespace nix::hvm4
