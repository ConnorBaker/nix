#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace nix {

// ═══════════════════════════════════════════════════════════════════════
// Thread-local NixBinding registry
// ═══════════════════════════════════════════════════════════════════════
//
// Populated by registerNixBindings (at parse time in ExprParseFile::eval).
// Queried by maybeRecordNixBindingDep (at attribute access time).
// Cleared at root DependencyTracker construction.

static thread_local std::unordered_map<PosIdx, NixBindingEntry> nixBindingRegistry;

void clearNixBindingRegistry()
{
    nixBindingRegistry.clear();
}

// ═══════════════════════════════════════════════════════════════════════
// findNonRecExprAttrs — AST walking for eligible attrsets
// ═══════════════════════════════════════════════════════════════════════

std::pair<ExprAttrs *, std::vector<Expr *>> findNonRecExprAttrs(Expr * root)
{
    // Walk through scope-introducing expressions (lambda, with, let),
    // update (//) chains, and function call arguments to find the
    // innermost non-recursive ExprAttrs.
    //
    // Eligible:   { x = 1; }                    (bare attrset)
    //             a: b: { x = a; }              (lambda chain)
    //             let h = 1; in { x = h; }      (let wrapping)
    //             with lib; { x = foo; }         (with wrapping)
    //             { x = 1; } // { y = 2; }      (update — walks LHS)
    //             f { x = 1; }                   (call — walks last arg)
    //             f (a: { x = a; })              (call with lambda arg)
    //
    // Ineligible: rec { x = 1; y = x; }         (recursive)
    //             { ${name} = 1; }               (dynamic attrs)
    //             if cond then { ... } else ...  (conditional body)

    std::vector<Expr *> scopeExprs;
    Expr * current = root;

    while (current) {
        if (auto * lambda = dynamic_cast<ExprLambda *>(current)) {
            scopeExprs.push_back(current);
            current = lambda->body;
        } else if (auto * with = dynamic_cast<ExprWith *>(current)) {
            scopeExprs.push_back(current);
            current = with->body;
        } else if (auto * let = dynamic_cast<ExprLet *>(current)) {
            scopeExprs.push_back(current);
            current = let->body;
        } else if (auto * attrs = dynamic_cast<ExprAttrs *>(current)) {
            if (attrs->recursive)
                return {nullptr, {}};
            if (attrs->dynamicAttrs && !attrs->dynamicAttrs->empty())
                return {nullptr, {}};
            return {attrs, std::move(scopeExprs)};
        } else if (auto * update = dynamic_cast<ExprOpUpdate *>(current)) {
            // { bindings... } // rhs — walk LHS to find the attrset
            current = update->e1;
        } else if (auto * call = dynamic_cast<ExprCall *>(current)) {
            // f { bindings... } — walk into the last argument.
            // The call's function expression is added to scopeExprs so
            // that computeNixScopeHash includes it: changing the called
            // function (e.g., mapAliases → mapAliases2) invalidates all
            // binding hashes. Covers aliases.nix (mapAliases { ... }),
            // package.nix (stdenv.mkDerivation { ... }), and
            // mkDerivation (finalAttrs: { ... }).
            if (!call->args || call->args->empty())
                return {nullptr, {}};
            scopeExprs.push_back(current);
            current = call->args->back();
        } else {
            return {nullptr, {}};
        }
    }
    return {nullptr, {}};
}

// ═══════════════════════════════════════════════════════════════════════
// InheritedFrom resolution + binding expression serialization
// ═══════════════════════════════════════════════════════════════════════

/**
 * Resolve the expression to show() for a binding. For InheritedFrom bindings,
 * returns the source expression from inheritFromExprs (e.g., the `builtins` in
 * `inherit (builtins) x`). For all other kinds, returns def.e directly.
 *
 * InheritedFrom is unsafe to show() directly because ExprInheritFrom extends
 * ExprVar with Symbol{} (id=0), causing unsigned underflow in
 * SymbolTable::operator[].
 */
static const Expr * resolveBindingExpr(
    const ExprAttrs::AttrDef & def,
    const ExprAttrs * ownerAttrs)
{
    if (def.kind == ExprAttrs::AttrDef::Kind::InheritedFrom) {
        auto * inheritVar = dynamic_cast<ExprInheritFrom *>(def.e);
        if (inheritVar && ownerAttrs->inheritFromExprs
            && static_cast<size_t>(inheritVar->displ) < ownerAttrs->inheritFromExprs->size())
            return (*ownerAttrs->inheritFromExprs)[inheritVar->displ];
        return nullptr; // Malformed AST — omit expression (conservative)
    }
    return def.e;
}

/**
 * Show the resolved expression for a binding into a Sink.
 * Used by computeNixScopeHash for let bindings.
 */
static void showBindingExpr(
    Sink & sink,
    const ExprAttrs::AttrDef & def,
    const ExprAttrs * ownerAttrs,
    const SymbolTable & symbols)
{
    auto * expr = resolveBindingExpr(def, ownerAttrs);
    if (expr) {
        std::ostringstream ss;
        expr->show(symbols, ss);
        sink(ss.str());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// computeNixScopeHash — scope fingerprint
// ═══════════════════════════════════════════════════════════════════════

Blake3Hash computeNixScopeHash(const std::vector<Expr *> & scopeExprs, const SymbolTable & symbols)
{
    HashSink sink(HashAlgorithm::BLAKE3);

    for (auto * expr : scopeExprs) {
        if (auto * lambda = dynamic_cast<ExprLambda *>(expr)) {
            sink("lambda:");
            if (lambda->arg)
                sink(std::string(symbols[lambda->arg]));
            sink(std::string_view("\0", 1));
            if (auto formals = lambda->getFormals()) {
                // Sort by string name for cross-session stability.
                // validateFormals sorts by Symbol ID (creation-time order),
                // which varies across EvalState instances.
                for (auto & f : formals->lexicographicOrder(symbols)) {
                    sink(std::string(symbols[f.name]));
                    if (f.def) {
                        sink("=");
                        std::ostringstream ss;
                        f.def->show(symbols, ss);
                        sink(ss.str());
                    }
                    sink(std::string_view("\0", 1));
                }
                if (formals->ellipsis)
                    sink("...");
            }
        } else if (auto * with = dynamic_cast<ExprWith *>(expr)) {
            sink("with:");
            std::ostringstream ss;
            with->attrs->show(symbols, ss);
            sink(ss.str());
        } else if (auto * let = dynamic_cast<ExprLet *>(expr)) {
            sink("let:");
            if (let->attrs && let->attrs->attrs) {
                // Sort by string name for cross-session stability.
                // pmr::map<Symbol, ...> sorts by Symbol ID (assignment order),
                // which varies across EvalState instances. String-based sorting
                // ensures identical hashes for identical file content.
                std::vector<std::pair<std::string, const ExprAttrs::AttrDef *>> sorted;
                sorted.reserve(let->attrs->attrs->size());
                for (auto & [sym, def] : *let->attrs->attrs)
                    sorted.emplace_back(std::string(symbols[sym]), &def);
                std::sort(sorted.begin(), sorted.end(),
                    [](auto & a, auto & b) { return a.first < b.first; });

                for (auto & [name, def] : sorted) {
                    sink(name);
                    sink("=");
                    showBindingExpr(sink, *def, let->attrs, symbols);
                    sink(std::string_view("\0", 1));
                }
            }
        } else if (auto * call = dynamic_cast<ExprCall *>(expr)) {
            // Hash the function expression so that changing the called
            // function (e.g., mapAliases → mapAliases2, or
            // stdenv.mkDerivation → stdenv.mkDerivation2) invalidates
            // all binding hashes in the argument attrset.
            sink("call:");
            std::ostringstream ss;
            call->fun->show(symbols, ss);
            sink(ss.str());
        }
        sink(";");
    }

    return Blake3Hash::fromHash(sink.finish().hash);
}

// ═══════════════════════════════════════════════════════════════════════
// computeNixBindingHash — per-binding fingerprint
// ═══════════════════════════════════════════════════════════════════════

Blake3Hash computeNixBindingHash(
    const Blake3Hash & scopeHash,
    std::string_view name,
    int kindTag,
    const Expr * exprToShow,
    const SymbolTable & symbols)
{
    HashSink sink(HashAlgorithm::BLAKE3);
    sink(scopeHash.view());
    sink(std::string_view("\0", 1));
    sink(name);
    sink(std::string_view("\0", 1));
    sink(std::to_string(kindTag));
    sink(std::string_view("\0", 1));
    if (exprToShow) {
        std::ostringstream ss;
        exprToShow->show(symbols, ss);
        sink(ss.str());
    }
    return Blake3Hash::fromHash(sink.finish().hash);
}

// ═══════════════════════════════════════════════════════════════════════
// registerNixBindings — populate the thread-local registry
// ═══════════════════════════════════════════════════════════════════════

void registerNixBindings(ExprAttrs * exprAttrs,
                         const std::string & depSource, const std::string & depKey,
                         const Blake3Hash & scopeHash, const SymbolTable & symbols,
                         InterningPools & pools)
{
    // Ensure sessionSymbols is set — recordStructuredDep needs it for JSON
    // key serialization. May not be set yet if no TracedData origins exist.
    pools.sessionSymbols = &symbols;

    auto sourceId = pools.intern<DepSourceId>(depSource);
    auto filePathId = pools.filePathPool.intern(depKey);

    for (auto & [sym, def] : *exprAttrs->attrs) {
        if (!def.pos) continue;  // No position — can't track

        auto name = std::string(symbols[sym]);
        auto hash = computeNixBindingHash(
            scopeHash, name, static_cast<int>(def.kind),
            resolveBindingExpr(def, exprAttrs), symbols);
        auto dataPathId = pools.dataPathPool.internChild(pools.dataPathPool.root(), name);

        nixBindingRegistry.emplace(def.pos, NixBindingEntry{
            sourceId, filePathId, dataPathId, hash});
    }
}

// ═══════════════════════════════════════════════════════════════════════
// maybeRecordNixBindingDep — attribute access recording
// ═══════════════════════════════════════════════════════════════════════

[[gnu::cold]] void maybeRecordNixBindingDep(PosIdx pos)
{
    // Uses PosIdx as a join key: registerNixBindings populates the registry
    // with PosIdx→NixBindingEntry at parse time, and attribute access sites
    // (ExprSelect, ExprOpHasAttr, prim_getAttr) call this to record deps.
    // PosIdx survives the // overlay chain because ExprOpUpdate copies full
    // Attr structs including pos (eval.cc:2079-2105).
    if (!DependencyTracker::isActive()) return;
    if (!pos) return;

    auto it = nixBindingRegistry.find(pos);
    if (it == nixBindingRegistry.end()) return;

    auto & entry = it->second;
    CompactDepComponents c{
        entry.sourceId,
        entry.filePathId,
        StructuredFormat::Nix,
        entry.dataPathId,
        ShapeSuffix::None,
        Symbol{},
    };
    recordStructuredDep(
        DependencyTracker::activeTracker->pools,
        c,
        DepHashValue(entry.bindingHash));
}

} // namespace nix
