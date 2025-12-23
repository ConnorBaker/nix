/**
 * String Encoding Implementation for HVM4
 *
 * Uses an indexed string table approach for efficient string handling.
 */

#include "nix/expr/hvm4/hvm4-string.hh"
#include "nix/expr/hvm4/hvm4-runtime.hh"

namespace nix::hvm4 {

// =============================================================================
// StringTable Implementation
// =============================================================================

uint32_t StringTable::intern(std::string_view str) {
    // Check if string already exists
    auto it = index_.find(str);
    if (it != index_.end()) {
        return it->second;
    }

    // Add new string
    uint32_t id = static_cast<uint32_t>(strings_.size());
    strings_.emplace_back(str);
    // Update index to point to the stored string (not the input view)
    index_[strings_.back()] = id;
    return id;
}

std::string_view StringTable::get(uint32_t id) const {
    if (id >= strings_.size()) {
        return {};
    }
    return strings_[id];
}

bool StringTable::valid(uint32_t id) const {
    return id < strings_.size();
}

void StringTable::clear() {
    strings_.clear();
    index_.clear();
}

// =============================================================================
// String Term Construction
// =============================================================================

Term makeString(uint32_t stringId, HVM4Runtime& runtime) {
    // Create #Str{string_id} constructor
    Term idTerm = HVM4Runtime::termNewNum(stringId);
    Term args[1] = {idTerm};
    return runtime.termNewCtr(CTR_STR, 1, args);
}

Term makeStringFromContent(std::string_view content, StringTable& table, HVM4Runtime& runtime) {
    uint32_t id = table.intern(content);
    return makeString(id, runtime);
}

// =============================================================================
// String Term Inspection
// =============================================================================

bool isString(Term term) {
    return HVM4Runtime::termTag(term) == HVM4Runtime::TAG_C01 &&
           HVM4Runtime::termExt(term) == CTR_STR;
}

uint32_t getStringId(Term term, const HVM4Runtime& runtime) {
    if (!isString(term)) {
        return 0;
    }
    uint32_t loc = HVM4Runtime::termVal(term);
    Term idTerm = runtime.load(loc);
    return static_cast<uint32_t>(HVM4Runtime::termVal(idTerm));
}

// =============================================================================
// String Operations
// =============================================================================

Term concatStrings(Term a, Term b, StringTable& table, HVM4Runtime& runtime) {
    // Get string IDs and content
    uint32_t idA = getStringId(a, runtime);
    uint32_t idB = getStringId(b, runtime);

    std::string_view contentA = table.get(idA);
    std::string_view contentB = table.get(idB);

    // Concatenate and intern result
    std::string result;
    result.reserve(contentA.size() + contentB.size());
    result.append(contentA);
    result.append(contentB);

    return makeStringFromContent(result, table, runtime);
}

// =============================================================================
// Runtime String Concatenation Support
// =============================================================================

Term makeStringConcat(Term left, Term right, HVM4Runtime& runtime) {
    // Create #SCat{left, right} constructor
    Term args[2] = {left, right};
    return runtime.termNewCtr(CTR_SCAT, 2, args);
}

Term makeStringFromInt(Term intTerm, HVM4Runtime& runtime) {
    // Create #SNum{value} constructor
    Term args[1] = {intTerm};
    return runtime.termNewCtr(CTR_SNUM, 1, args);
}

bool isStringConcat(Term term) {
    return HVM4Runtime::termTag(term) == HVM4Runtime::TAG_C02 &&
           HVM4Runtime::termExt(term) == CTR_SCAT;
}

bool isStringFromInt(Term term) {
    return HVM4Runtime::termTag(term) == HVM4Runtime::TAG_C01 &&
           HVM4Runtime::termExt(term) == CTR_SNUM;
}

Term getStringConcatLeft(Term term, const HVM4Runtime& runtime) {
    uint32_t loc = HVM4Runtime::termVal(term);
    return runtime.load(loc);
}

Term getStringConcatRight(Term term, const HVM4Runtime& runtime) {
    uint32_t loc = HVM4Runtime::termVal(term);
    return runtime.load(loc + 1);
}

Term getStringFromIntValue(Term term, const HVM4Runtime& runtime) {
    uint32_t loc = HVM4Runtime::termVal(term);
    return runtime.load(loc);
}

std::string extractStringContent(Term term, const StringTable& table, HVM4Runtime& runtime) {
    // First, evaluate to normal form
    term = runtime.evaluateSNF(term);

    // Handle different string term types
    if (isString(term)) {
        // Simple string literal - just look up the content
        uint32_t id = getStringId(term, runtime);
        return std::string(table.get(id));
    }

    if (isStringConcat(term)) {
        // Recursively extract and concatenate
        Term left = getStringConcatLeft(term, runtime);
        Term right = getStringConcatRight(term, runtime);

        std::string leftContent = extractStringContent(left, table, runtime);
        std::string rightContent = extractStringContent(right, table, runtime);

        return leftContent + rightContent;
    }

    if (isStringFromInt(term)) {
        // Extract the integer and convert to string
        Term intTerm = getStringFromIntValue(term, runtime);
        intTerm = runtime.evaluateSNF(intTerm);

        // Handle both small NUM and BigInt
        uint8_t tag = HVM4Runtime::termTag(intTerm);
        if (tag == HVM4Runtime::TAG_NUM) {
            // Small integer
            uint32_t bits = HVM4Runtime::termVal(intTerm);
            int32_t signedVal = static_cast<int32_t>(bits);
            return std::to_string(signedVal);
        }

        // Could be BigInt - for now just handle small integers
        // TODO: Handle BigInt conversion
        throw std::runtime_error("BigInt to string conversion not yet implemented");
    }

    throw std::runtime_error("Unknown string term type in extractStringContent");
}

}  // namespace nix::hvm4
