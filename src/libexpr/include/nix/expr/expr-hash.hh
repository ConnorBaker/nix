#pragma once
///@file

#include "nix/expr/eval-hash.hh"
#include "nix/expr/symbol-table.hh"

#include <boost/unordered/unordered_flat_map.hpp>

namespace nix {

struct Expr;

/**
 * Cache for expression content hashes.
 *
 * Since expressions are immutable after parsing, their content hashes
 * can be safely cached by pointer for the lifetime of the evaluation.
 * This dramatically improves performance when the same expression is
 * hashed multiple times (e.g., during thunk memoization).
 */
using ExprHashCache = boost::unordered_flat_map<const Expr *, ContentHash>;

/**
 * Compute a content hash for a Nix expression.
 *
 * This hash is computed from the expression's AST structure and is stable
 * across evaluations, machines, and time - making it suitable for cross-evaluation
 * caching.
 *
 * Key properties:
 * - Variable references use De Bruijn indices (level, displ) rather than symbol names
 * - Symbol names (in attrs, formals, etc.) are hashed by their string bytes, not Symbol IDs
 * - Handles expression cycles via ancestor stack cycle detection
 *
 * @param e The expression to hash. Must not be null.
 * @param symbols The symbol table for resolving symbol string bytes.
 * @param cache Optional cache for memoizing expression hashes. If provided,
 *              hashes are looked up/stored in this cache for performance.
 * @return A ContentHash that uniquely identifies the expression's semantic content.
 */
ContentHash hashExpr(const Expr * e, const SymbolTable & symbols, ExprHashCache * cache = nullptr);

/**
 * Compute expression hash with portability tracking.
 *
 * This variant returns both the hash and its portability classification,
 * allowing callers to determine if the hash is safe for persistent caching.
 *
 * Non-portable sources in expressions:
 * - **ExprPos (__curPos)**: Uses PosIdx::hash() which is session-local
 * - **ExprPath with raw fallback**: Paths without fingerprint/content hash
 *
 * @param e The expression to hash
 * @param symbols The symbol table for resolving symbol string bytes
 * @return ContentHashResult containing hash and portability info
 */
ContentHashResult hashExprWithPortability(const Expr * e, const SymbolTable & symbols);

} // namespace nix
