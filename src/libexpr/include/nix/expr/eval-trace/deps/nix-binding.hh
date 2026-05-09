#pragma once
///@file
/// NixBinding: per-binding .nix attrset tracking for eval-trace dep recording.

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
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
class EvalState;
namespace eval_trace { struct TraceAccess; }

using NixScopeHash = Tagged<struct NixScopeHashTag_, EvalTraceHash>;
using NixBindingHash = Tagged<struct NixBindingHashTag_, EvalTraceHash>;

/**
 * Registry entry for a single binding in a non-recursive ExprAttrs.
 * Keyed by PosIdx in the thread-local registry; looked up at attribute
 * access time by maybeRecordNixBindingDep.
 *
 * NOTE: sourceId/filePathId are NOT pre-computed here. They are resolved
 * lazily at access time in maybeRecordNixBindingDep using the
 * SemanticRegistry from the active DepCaptureScope. This is critical
 * because registerNixBindings runs during lockFlake when no DepCaptureScope
 * is active and resolution would be structurally impossible. At access
 * time, an active DepCaptureScope guarantees the SemanticRegistry is
 * available with correct input sources.
 */
struct NixBindingEntry {
    DataPathId dataPathId{};   ///< internChild(root, bindingName)
    NixBindingHash bindingHash{}; ///< hash(scopeHash + name + kind + showForHash(expr))
    SourcePath resolvedFile;    ///< for resolveDepPathKey + Content dep at access time
    std::optional<PathObject> origin; ///< exact logical source for aliased runtime roots
};

/**
 * Walk AST from root through lambda/with/let/update chains.
 * Returns (exprAttrs, scopeExprs) if an eligible non-recursive ExprAttrs
 * is found; (nullptr, {}) otherwise.
 */
std::pair<ExprAttrs *, std::vector<Expr *>> findNonRecExprAttrs(Expr * root);

/**
 * Compute an eval-trace hash capturing the enclosing scope structure.
 * Any change to the scope (lambda formals, let bindings, with expressions)
 * changes all binding hashes from that file.
 */
NixScopeHash computeNixScopeHash(const std::vector<Expr *> & scopeExprs, const SymbolTable & symbols,
                               const CanonPath * basePath = nullptr);

/**
 * Compute an eval-trace hash for a single binding definition.
 *
 * @param exprToShow  The expression to serialize. For plain/inherited bindings
 *   this is def.e directly. For InheritedFrom bindings, callers MUST pass the
 *   resolved source expression from inheritFromExprs[displ] -- NOT def.e,
 *   which is an ExprInheritFrom whose show() crashes (Symbol(0) underflow).
 *   Pass nullptr to omit the expression from the hash (conservative fallback).
 */
NixBindingHash computeNixBindingHash(
    const NixScopeHash & scopeHash,
    std::string_view name,
    int kindTag,
    const Expr * exprToShow,
    const SymbolTable & symbols,
    const CanonPath * basePath = nullptr);

/**
 * Register all bindings from an eligible ExprAttrs into the thread-local
 * registry. Called from ExprParseFile::eval at parse time.
 *
 * TODO: getFlake calls readFlake twice (once with accessor, once with
 * store path). This means registerNixBindings runs multiple times per file
 * with different resolved file identities. The last registration wins
 * (insert_or_assign). With the SemanticRegistry-based architecture,
 * provenance is resolved at dep-recording time via the immutable
 * SemanticRegistry (pre-populated at session open) rather than via
 * the old mutable mount table, so the double-readFlake pattern's
 * impact is limited to redundant registration work. The double-readFlake pattern in getFlake should still be
 * eliminated to avoid the redundant work.
 */
void registerNixBindings(EvalState & state,
                         ExprAttrs * exprAttrs,
                         const NixScopeHash & scopeHash,
                         const SymbolTable & symbols,
                         InterningPools & pools,
                         const SourcePath & resolvedFile,
                         const std::optional<PathObject> & origin = std::nullopt);

/**
 * Record a NixBinding StructuredContent dep if the given PosIdx is
 * registered in the thread-local registry. Called from ExprSelect::eval,
 * ExprOpHasAttr::eval, and prim_getAttr.
 */
[[gnu::cold]] void maybeRecordNixBindingDep(
    const eval_trace::TraceAccess & access,
    EvalState & state,
    PosIdx pos);

} // namespace nix
