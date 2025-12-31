#pragma once
///@file

#include "nix/expr/eval-hash.hh"
#include "nix/expr/expr-hash.hh"
#include "nix/expr/value-hash.hh"

namespace nix {

struct Env;
struct Expr;
class SymbolTable;

/**
 * Compute the structural hash of a thunk.
 *
 * A thunk's identity is determined by:
 * - The expression it will evaluate (hashed via expr-hash)
 * - The environment in which it will be evaluated (hashed via env-hash)
 * - The tryEval depth (affects exception handling behavior)
 *
 * Two thunks with the same structural hash will produce semantically
 * equivalent results when forced, making them candidates for:
 * - Within-evaluation deduplication (thunk interning)
 * - Cross-evaluation caching (persistent cache lookup) - requires env size tracking
 *
 * @param expr The expression the thunk will evaluate
 * @param env The environment for evaluation
 * @param envSize The size of the environment (number of values)
 * @param tryLevel The current tryEval nesting depth (affects exception semantics)
 * @param symbols The symbol table for resolving names
 * @param exprCache Optional cache for expression hashes (improves performance)
 * @param valueCache Optional cache for value hashes (improves performance)
 * @return Structural hash of the thunk
 *
 * @note The envSize parameter is required because Env doesn't store its own size.
 *       For full cross-evaluation stability, ALL env sizes in the parent chain
 *       must be known. See env-hash.hh for stability limitations.
 *
 * @note The tryLevel parameter is critical for correctness: the same expression
 *       may behave differently inside vs outside a tryEval (e.g., assertions).
 */
StructuralHash computeThunkHash(
    const Expr * expr,
    const Env * env,
    size_t envSize,
    int tryLevel,
    const SymbolTable & symbols,
    ExprHashCache * exprCache = nullptr,
    ValueHashCache * valueCache = nullptr);

/**
 * Compute the structural hash of a thunk using the env's stored size.
 *
 * This is the preferred API for thunk hashing. It extracts env->size
 * automatically and produces content-based hashes that are stable within
 * an evaluation session.
 *
 * @param expr The expression the thunk will evaluate
 * @param env The environment for evaluation (may be null)
 * @param tryLevel The current tryEval nesting depth (affects exception semantics)
 * @param symbols The symbol table for resolving names
 * @param exprCache Optional cache for expression hashes (improves performance)
 * @param valueCache Optional cache for value hashes (improves performance)
 * @return Structural hash of the thunk
 */
StructuralHash computeThunkStructuralHash(
    const Expr * expr,
    const Env * env,
    int tryLevel,
    const SymbolTable & symbols,
    ExprHashCache * exprCache = nullptr,
    ValueHashCache * valueCache = nullptr);

} // namespace nix
