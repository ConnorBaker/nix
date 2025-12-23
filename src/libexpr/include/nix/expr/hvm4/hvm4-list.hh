#pragma once

/**
 * List Encoding for HVM4
 *
 * Nix lists are represented in HVM4 using a spine-based encoding with
 * cached length for O(1) length operations:
 *
 *   #Lst{length, spine}  where spine = #Nil{} | #Con{head, tail}
 *
 * Benefits:
 * - O(1) length operation (cached in #Lst{} constructor)
 * - Lazy element evaluation (elements remain as thunks until accessed)
 * - Standard cons-list structure for head/tail operations
 *
 * Constructor Tags:
 * - CTR_LST: List wrapper with cached length
 * - CTR_NIL: Empty spine marker
 * - CTR_CON: Cons cell with head and tail
 *
 * Example encodings:
 *   []        -> #Lst{0, #Nil{}}
 *   [1]       -> #Lst{1, #Con{1, #Nil{}}}
 *   [1, 2, 3] -> #Lst{3, #Con{1, #Con{2, #Con{3, #Nil{}}}}}
 */

#include "nix/expr/hvm4/hvm4-runtime.hh"
#include <cstdint>
#include <vector>

namespace nix::hvm4 {

// Constructor tags for list encoding
// These must be large values to avoid collision with NUM values
// (see hvm4-bigint.hh for explanation)
constexpr uint32_t CTR_LST = 0x100010;  // List wrapper: #Lst{length, spine}
constexpr uint32_t CTR_NIL = 0x100011;  // Empty spine: #Nil{}
constexpr uint32_t CTR_CON = 0x100012;  // Cons cell: #Con{head, tail}

/**
 * Create an empty spine marker #Nil{}.
 */
Term makeNil(HVM4Runtime& runtime);

/**
 * Create a cons cell #Con{head, tail}.
 *
 * @param head The head element (can be a thunk)
 * @param tail The tail spine (either #Nil{} or another #Con{})
 * @param runtime The HVM4 runtime
 * @return The constructed cons cell
 */
Term makeCons(Term head, Term tail, HVM4Runtime& runtime);

/**
 * Create a list wrapper #Lst{length, spine}.
 *
 * @param length The cached length (number of elements)
 * @param spine The list spine (#Nil{} or #Con{...})
 * @param runtime The HVM4 runtime
 * @return The constructed list
 */
Term makeList(uint32_t length, Term spine, HVM4Runtime& runtime);

/**
 * Build a complete list from a vector of element terms.
 *
 * Elements are stored in order (first element is head of list).
 * This function builds the spine from back to front.
 *
 * @param elements Vector of element terms (can be thunks)
 * @param runtime The HVM4 runtime
 * @return The constructed list (#Lst{length, spine})
 */
Term buildListFromElements(const std::vector<Term>& elements, HVM4Runtime& runtime);

/**
 * Check if a term represents an empty list.
 */
bool isEmptyList(Term term, const HVM4Runtime& runtime);

/**
 * Check if a term is a list (either empty or non-empty).
 */
bool isList(Term term);

/**
 * Check if a term is a cons cell (#Con{}).
 */
bool isCons(Term term);

/**
 * Check if a term is an empty spine marker (#Nil{}).
 */
bool isNil(Term term);

/**
 * Get the cached length from a list term.
 *
 * @param term A #Lst{length, spine} term (must be in normal form)
 * @param runtime The HVM4 runtime
 * @return The length, or 0 if not a valid list
 */
uint32_t getListLength(Term term, const HVM4Runtime& runtime);

/**
 * Get the spine from a list term.
 *
 * @param term A #Lst{length, spine} term (must be in normal form)
 * @param runtime The HVM4 runtime
 * @return The spine term (#Nil{} or #Con{...})
 */
Term getListSpine(Term term, const HVM4Runtime& runtime);

/**
 * Get the head element from a cons cell.
 *
 * @param term A #Con{head, tail} term (must be in normal form)
 * @param runtime The HVM4 runtime
 * @return The head element (may be a thunk)
 */
Term getConsHead(Term term, const HVM4Runtime& runtime);

/**
 * Get the tail spine from a cons cell.
 *
 * @param term A #Con{head, tail} term (must be in normal form)
 * @param runtime The HVM4 runtime
 * @return The tail spine (#Nil{} or another #Con{})
 */
Term getConsTail(Term term, const HVM4Runtime& runtime);

/**
 * Concatenate two lists (for ++ operator).
 *
 * Creates a new list by appending list2's elements after list1's elements.
 * The length of the result is the sum of both input lengths.
 * Elements are shared (not copied) - only the spine structure is rebuilt.
 *
 * @param list1 First list (#Lst{length1, spine1})
 * @param list2 Second list (#Lst{length2, spine2})
 * @param runtime The HVM4 runtime
 * @return #Lst{length1+length2, concatenated_spine}
 */
Term concatLists(Term list1, Term list2, HVM4Runtime& runtime);

}  // namespace nix::hvm4
