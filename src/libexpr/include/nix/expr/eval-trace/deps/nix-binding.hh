#pragma once
///@file
/// NixBinding: per-binding .nix attrset tracking for eval-trace dep recording.

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/ids.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/position.hh"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nix {

struct InterningPools;
struct Expr;
struct ExprAttrs;

/**
 * Registry entry for a single binding in a non-recursive ExprAttrs.
 * Keyed by PosIdx in the thread-local registry; looked up at attribute
 * access time by maybeRecordNixBindingDep.
 */
struct NixBindingEntry {
    DepSourceId sourceId;
    FilePathId filePathId;
    DataPathId dataPathId;     ///< internChild(root, bindingName)
    Blake3Hash bindingHash;    ///< BLAKE3(scopeHash + name + kind + show(expr))
};

/**
 * Walk AST from root through lambda/with/let/update chains.
 * Returns (exprAttrs, scopeExprs) if an eligible non-recursive ExprAttrs
 * is found; (nullptr, {}) otherwise.
 */
std::pair<ExprAttrs *, std::vector<Expr *>> findNonRecExprAttrs(Expr * root);

/**
 * Compute a BLAKE3 hash capturing the enclosing scope structure.
 * Any change to the scope (lambda formals, let bindings, with expressions)
 * changes all binding hashes from that file.
 */
Blake3Hash computeNixScopeHash(const std::vector<Expr *> & scopeExprs, const SymbolTable & symbols);

/**
 * Compute a BLAKE3 hash for a single binding definition.
 * Hash = BLAKE3(scopeHash || '\0' || name || '\0' || kindTag || '\0' || show(expr)).
 *
 * @param exprToShow  The expression to serialize. For plain/inherited bindings
 *   this is def.e directly. For InheritedFrom bindings, callers MUST pass the
 *   resolved source expression from inheritFromExprs[displ] -- NOT def.e,
 *   which is an ExprInheritFrom whose show() crashes (Symbol(0) underflow).
 *   Pass nullptr to omit the expression from the hash (conservative fallback).
 */
Blake3Hash computeNixBindingHash(
    const Blake3Hash & scopeHash,
    std::string_view name,
    int kindTag,
    const Expr * exprToShow,
    const SymbolTable & symbols);

/**
 * Register all bindings from an eligible ExprAttrs into the thread-local
 * registry. Called from ExprParseFile::eval at parse time.
 */
void registerNixBindings(ExprAttrs * exprAttrs,
                         const std::string & depSource, const std::string & depKey,
                         const Blake3Hash & scopeHash, const SymbolTable & symbols,
                         InterningPools & pools);

/**
 * Record a NixBinding StructuredContent dep if the given PosIdx is
 * registered in the thread-local registry. Called from ExprSelect::eval,
 * ExprOpHasAttr::eval, and prim_getAttr.
 */
[[gnu::cold]] void maybeRecordNixBindingDep(PosIdx pos);

/**
 * Clear the thread-local NixBinding registry.
 * Called on root DependencyTracker construction.
 */
void clearNixBindingRegistry();

} // namespace nix
