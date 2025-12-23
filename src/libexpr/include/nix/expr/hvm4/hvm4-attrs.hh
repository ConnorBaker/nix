#pragma once

/**
 * Attribute Set Encoding for HVM4
 *
 * Nix attribute sets are represented as wrapped sorted lists:
 *
 *   attrs = #Ats{spine}
 *   spine = #Nil{} | #Con{#Atr{key_id, value}, tail}
 *   #Atr{key_id, value}     - Attribute node
 *
 * The #Ats{} wrapper enables type identification during result extraction,
 * while keeping internal operations simple (all attrs use the same wrapper).
 *
 * Benefits:
 * - Simple: All attrs use #Ats{} wrapper (no ABs/ALy dispatch needed)
 * - O(n) lookup via linear search on sorted keys
 * - Lazy value evaluation (values remain as thunks until accessed)
 * - Symbol IDs as keys for efficient comparison
 * - The `//` operator produces the same #Ats{} type
 *
 * Trade-offs:
 * - `//` operator is O(n+m) merge instead of O(1) wrap
 * - Values are shared during merge (only spine is rebuilt)
 *
 * Constructor Tags:
 * - CTR_ATS: Attribute set wrapper
 * - CTR_ATR: Attribute node with key and value
 * - List constructors (CTR_CON, CTR_NIL) from hvm4-list.hh
 *
 * Example encodings:
 *   { }           -> #Ats{#Nil{}}
 *   { a = 1; }    -> #Ats{#Con{#Atr{sym_a, 1}, #Nil{}}}
 *   { a = 1; b = 2; }
 *                 -> #Ats{#Con{#Atr{sym_a, 1}, #Con{#Atr{sym_b, 2}, #Nil{}}}}
 *   { a = 1; } // { b = 2; }
 *                 -> #Ats{#Con{#Atr{sym_a, 1}, #Con{#Atr{sym_b, 2}, #Nil{}}}}
 *                    (merged, sorted by key)
 */

#include "nix/expr/hvm4/hvm4-runtime.hh"
#include "nix/expr/hvm4/hvm4-list.hh"
#include "nix/expr/symbol-table.hh"
#include <cstdint>
#include <vector>
#include <utility>

namespace nix::hvm4 {

// Constructor tags for attribute sets
// Must be large values to avoid collision with NUM values
constexpr uint32_t CTR_ATS = 0x100030;  // Attrs wrapper: #Ats{spine}
constexpr uint32_t CTR_ATR = 0x100032;  // Attr node: #Atr{key_id, value}

// Maybe-like constructors for optional attribute lookup (used by select-or-default)
constexpr uint32_t CTR_SOM = 0x100040;  // Some: #Som{value}
constexpr uint32_t CTR_NON = 0x100041;  // None: #Non{}

/**
 * Create an empty attribute set.
 *
 * @param runtime The HVM4 runtime
 * @return #Ats{#Nil{}}
 */
Term makeEmptyAttrs(HVM4Runtime& runtime);

/**
 * Create an attribute node.
 *
 * @param symbolId The symbol ID for the key
 * @param value The attribute value (can be a thunk)
 * @param runtime The HVM4 runtime
 * @return #Atr{symbolId, value}
 */
Term makeAttrNode(uint32_t symbolId, Term value, HVM4Runtime& runtime);

/**
 * Build an attribute set from a vector of (symbol_id, value) pairs.
 *
 * The pairs do NOT need to be sorted - this function will sort them.
 * Returns a wrapped sorted spine #Ats{spine}.
 *
 * @param attrs Vector of (symbol_id, value) pairs
 * @param runtime The HVM4 runtime
 * @return #Ats{#Con{#Atr{...}, ...}} or #Ats{#Nil{}}
 */
Term buildAttrsFromPairs(std::vector<std::pair<uint32_t, Term>>& attrs, HVM4Runtime& runtime);

/**
 * Check if a term is an attribute set (#Ats{}).
 */
bool isAttrsSet(Term term);

/**
 * Check if a term is an attribute node (#Atr{}).
 */
bool isAttrNode(Term term);

/**
 * Check if a term could be an attribute spine (nil or cons list).
 *
 * Note: This doesn't verify the contents are #Atr nodes,
 * just that the structure is a valid list.
 */
bool isAttrSpine(Term term);

/**
 * Wrap a spine in an #Ats{} constructor.
 *
 * @param spine The sorted spine
 * @param runtime The HVM4 runtime
 * @return #Ats{spine}
 */
Term wrapAttrsSpine(Term spine, HVM4Runtime& runtime);

/**
 * Extract the spine from an #Ats{} term.
 *
 * @param attrs An #Ats{spine} term
 * @param runtime The HVM4 runtime
 * @return The spine term
 */
Term getAttrsSpine(Term attrs, const HVM4Runtime& runtime);

/**
 * Get the key symbol ID from an attribute node.
 *
 * @param term A #Atr{key_id, value} term
 * @param runtime The HVM4 runtime
 * @return The symbol ID
 */
uint32_t getAttrKey(Term term, const HVM4Runtime& runtime);

/**
 * Get the value from an attribute node.
 *
 * @param term A #Atr{key_id, value} term
 * @param runtime The HVM4 runtime
 * @return The value term (may be a thunk)
 */
Term getAttrValue(Term term, const HVM4Runtime& runtime);

/**
 * Merge two attribute sets (for // operator).
 *
 * Creates a new merged attrs where overlay keys take precedence over base keys.
 * Both inputs must be #Ats{spine} terms. Returns #Ats{merged_spine}.
 * This is O(n+m) where n and m are the sizes of the two sets.
 * Values are shared (not copied) - only the spine structure is rebuilt.
 *
 * @param base The base attribute set #Ats{spine}
 * @param overlay The overlay attribute set #Ats{spine} (takes precedence)
 * @param runtime The HVM4 runtime
 * @return #Ats{merged_spine}
 */
Term mergeAttrs(Term base, Term overlay, HVM4Runtime& runtime);

/**
 * Count the number of attributes in an attribute set.
 *
 * @param attrs An #Ats{spine} term
 * @param runtime The HVM4 runtime
 * @return Number of attributes
 */
size_t countAttrs(Term attrs, const HVM4Runtime& runtime);

}  // namespace nix::hvm4
