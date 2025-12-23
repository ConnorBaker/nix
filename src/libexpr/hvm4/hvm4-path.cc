/**
 * HVM4 Path Implementation
 *
 * Implements path encoding and operations for the HVM4 backend.
 * Paths are represented as #Pth{accessor_id, path_string_id} where
 * both IDs reference side tables (accessor registry and string table).
 */

#include "nix/expr/hvm4/hvm4-path.hh"
#include "nix/util/source-path.hh"

namespace nix::hvm4 {

// =============================================================================
// AccessorRegistry Implementation
// =============================================================================

uint32_t AccessorRegistry::registerAccessor(SourceAccessor* accessor) {
    // Check if already registered
    auto it = index_.find(accessor);
    if (it != index_.end()) {
        return it->second;
    }

    // Register new accessor
    uint32_t id = static_cast<uint32_t>(accessors_.size());
    accessors_.push_back(accessor);
    index_[accessor] = id;
    return id;
}

SourceAccessor* AccessorRegistry::getAccessor(uint32_t id) const {
    if (id >= accessors_.size()) {
        return nullptr;
    }
    return accessors_[id];
}

bool AccessorRegistry::valid(uint32_t id) const {
    return id < accessors_.size();
}

void AccessorRegistry::clear() {
    accessors_.clear();
    index_.clear();
}

// =============================================================================
// Path Term Operations
// =============================================================================

Term makePath(uint32_t accessorId, uint32_t pathStringId, HVM4Runtime& runtime) {
    // Create arity-2 constructor: #Pth{accessor_id, path_string_id}
    Term args[2] = {
        HVM4Runtime::termNewNum(accessorId),
        HVM4Runtime::termNewNum(pathStringId)
    };
    return runtime.termNewCtr(CTR_PTH, 2, args);
}

Term makePathFromSource(
    const SourcePath& path,
    AccessorRegistry& accessorRegistry,
    StringTable& stringTable,
    HVM4Runtime& runtime)
{
    // Register the accessor
    uint32_t accessorId = accessorRegistry.registerAccessor(&*path.accessor);

    // Intern the path string
    uint32_t pathStringId = stringTable.intern(path.path.abs());

    return makePath(accessorId, pathStringId, runtime);
}

bool isPath(Term term) {
    uint8_t tag = HVM4Runtime::termTag(term);
    if (tag != HVM4Runtime::TAG_C02) {
        return false;
    }
    uint32_t name = HVM4Runtime::termExt(term);
    return name == CTR_PTH;
}

uint32_t getPathAccessorId(Term term, const HVM4Runtime& runtime) {
    if (!isPath(term)) {
        return 0;
    }

    uint32_t loc = HVM4Runtime::termVal(term);
    Term accessorTerm = runtime.load(loc);

    // Should be a NUM
    if (HVM4Runtime::termTag(accessorTerm) != HVM4Runtime::TAG_NUM) {
        return 0;
    }

    return HVM4Runtime::termVal(accessorTerm);
}

uint32_t getPathStringId(Term term, const HVM4Runtime& runtime) {
    if (!isPath(term)) {
        return 0;
    }

    uint32_t loc = HVM4Runtime::termVal(term);
    Term stringTerm = runtime.load(loc + 1);

    // Should be a NUM
    if (HVM4Runtime::termTag(stringTerm) != HVM4Runtime::TAG_NUM) {
        return 0;
    }

    return HVM4Runtime::termVal(stringTerm);
}

Term concatPathString(
    Term pathTerm,
    uint32_t suffixStringId,
    StringTable& stringTable,
    HVM4Runtime& runtime)
{
    if (!isPath(pathTerm)) {
        // Return the original term if not a path
        return pathTerm;
    }

    uint32_t loc = HVM4Runtime::termVal(pathTerm);

    // Get accessor ID (stays the same)
    Term accessorTerm = runtime.load(loc);
    uint32_t accessorId = HVM4Runtime::termVal(accessorTerm);

    // Get existing path string
    Term pathStringTerm = runtime.load(loc + 1);
    uint32_t pathStringId = HVM4Runtime::termVal(pathStringTerm);

    // Concatenate the path strings
    std::string_view basePath = stringTable.get(pathStringId);
    std::string_view suffix = stringTable.get(suffixStringId);

    std::string newPath = std::string(basePath) + std::string(suffix);
    uint32_t newPathStringId = stringTable.intern(newPath);

    return makePath(accessorId, newPathStringId, runtime);
}

}  // namespace nix::hvm4
