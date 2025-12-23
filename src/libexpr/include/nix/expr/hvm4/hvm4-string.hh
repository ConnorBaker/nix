#pragma once

/**
 * String Encoding for HVM4
 *
 * Strings in HVM4 are represented using an indexed string table approach:
 *   #Str{string_id}
 *
 * Where string_id is an index into a side table that stores the actual
 * string content. This is more efficient than encoding strings as
 * character lists and preserves exact string content.
 *
 * For now, we don't track string context (store path dependencies).
 * Context handling will be added when derivation support is implemented.
 *
 * Constructor Tags:
 * - CTR_STR: String with indexed content
 */

#include "nix/expr/hvm4/hvm4-runtime.hh"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace nix::hvm4 {

// Constructor tags for string encoding
// Must be large to avoid collision with NUM values
constexpr uint32_t CTR_STR = 0x100020;   // String literal: #Str{string_id}
constexpr uint32_t CTR_SCAT = 0x100021;  // String concatenation: #SCat{left, right}
constexpr uint32_t CTR_SNUM = 0x100022;  // Integer to string: #SNum{value}

/**
 * String table for storing string content.
 *
 * Strings are interned - identical strings share the same ID.
 * This is a simple implementation; could be optimized with a hash map
 * for larger codebases.
 */
class StringTable {
public:
    /**
     * Intern a string and return its ID.
     * If the string already exists, returns the existing ID.
     */
    uint32_t intern(std::string_view str);

    /**
     * Get a string by its ID.
     * Returns empty string_view if ID is invalid.
     */
    std::string_view get(uint32_t id) const;

    /**
     * Check if an ID is valid.
     */
    bool valid(uint32_t id) const;

    /**
     * Get the number of interned strings.
     */
    size_t size() const { return strings_.size(); }

    /**
     * Clear all interned strings.
     */
    void clear();

private:
    std::vector<std::string> strings_;
    std::unordered_map<std::string_view, uint32_t> index_;
};

/**
 * Create a string term.
 *
 * @param stringId The string table ID
 * @param runtime The HVM4 runtime
 * @return The constructed string term #Str{string_id}
 */
Term makeString(uint32_t stringId, HVM4Runtime& runtime);

/**
 * Create a string term from content.
 *
 * @param content The string content
 * @param table The string table (for interning)
 * @param runtime The HVM4 runtime
 * @return The constructed string term
 */
Term makeStringFromContent(std::string_view content, StringTable& table, HVM4Runtime& runtime);

/**
 * Check if a term represents a string.
 */
bool isString(Term term);

/**
 * Get the string ID from a string term.
 *
 * @param term A #Str{string_id} term (must be in normal form)
 * @param runtime The HVM4 runtime
 * @return The string ID, or 0 if not a valid string
 */
uint32_t getStringId(Term term, const HVM4Runtime& runtime);

/**
 * Concatenate two strings (post-evaluation, for result extraction).
 *
 * @param a First string term
 * @param b Second string term
 * @param table The string table
 * @param runtime The HVM4 runtime
 * @return New string term with concatenated content
 */
Term concatStrings(Term a, Term b, StringTable& table, HVM4Runtime& runtime);

/**
 * Create a string concatenation term (for compile-time, lazy concatenation).
 * This creates #SCat{left, right} which is flattened during result extraction.
 *
 * @param left Left string term
 * @param right Right string term
 * @param runtime The HVM4 runtime
 * @return The #SCat{left, right} term
 */
Term makeStringConcat(Term left, Term right, HVM4Runtime& runtime);

/**
 * Create an integer-to-string term.
 * This creates #SNum{value} which is converted to string during result extraction.
 *
 * @param intTerm The integer term
 * @param runtime The HVM4 runtime
 * @return The #SNum{value} term
 */
Term makeStringFromInt(Term intTerm, HVM4Runtime& runtime);

/**
 * Check if a term represents a string concatenation.
 */
bool isStringConcat(Term term);

/**
 * Check if a term represents an integer-to-string conversion.
 */
bool isStringFromInt(Term term);

/**
 * Get the left operand of a string concatenation.
 */
Term getStringConcatLeft(Term term, const HVM4Runtime& runtime);

/**
 * Get the right operand of a string concatenation.
 */
Term getStringConcatRight(Term term, const HVM4Runtime& runtime);

/**
 * Get the integer value from an integer-to-string term.
 */
Term getStringFromIntValue(Term term, const HVM4Runtime& runtime);

/**
 * Extract the final string content from a string term that may contain
 * concatenations and int-to-string conversions. Returns the string content
 * after flattening all concatenations.
 *
 * @param term A string-like term (#Str, #SCat, or #SNum)
 * @param table The string table (for looking up literal string content)
 * @param runtime The HVM4 runtime
 * @return The final string content
 */
std::string extractStringContent(Term term, const StringTable& table, HVM4Runtime& runtime);

}  // namespace nix::hvm4
