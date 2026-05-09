#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/nix-binding.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"

#include <algorithm>
#include <sstream>

namespace nix {

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

// ═══════════════════════════════════════════════════════════════════════
// computeNixScopeHash — scope fingerprint
// ═══════════════════════════════════════════════════════════════════════

NixScopeHash computeNixScopeHash(const std::vector<Expr *> & scopeExprs, const SymbolTable & symbols,
                               const CanonPath * basePath)
{
    auto builder = eval_trace::makeDomainBuilder<eval_trace::hash_domain::NixBindingScopeHash>();

    auto base = basePath ? *basePath : CanonPath::root;
    auto showExpr = [&](const Expr * e) {
        std::ostringstream ss;
        e->showForHash(symbols, ss, base);
        return ss.str();
    };

    builder.field("scope-count", static_cast<uint64_t>(scopeExprs.size()));
    for (auto * expr : scopeExprs) {
        if (auto * lambda = dynamic_cast<ExprLambda *>(expr)) {
            builder.field("scope-kind", std::string_view("lambda"));
            if (lambda->arg)
                builder.field("lambda-arg", std::string_view(symbols[lambda->arg]));
            if (auto formals = lambda->getFormals()) {
                // Sort by string name for cross-session stability.
                // validateFormals sorts by Symbol ID (creation-time order),
                // which varies across EvalState instances.
                builder.field("lambda-formal-count", static_cast<uint64_t>(formals->formals.size()));
                for (auto & f : formals->lexicographicOrder(symbols)) {
                    builder.field("lambda-formal-name", std::string_view(symbols[f.name]));
                    builder.optionalField("lambda-formal-default", f.def ? std::optional(showExpr(f.def)) : std::nullopt);
                }
                builder.field("lambda-formals-ellipsis", formals->ellipsis);
            } else {
                builder.field("lambda-formal-count", uint64_t{0});
                builder.field("lambda-formals-ellipsis", false);
            }
        } else if (auto * with = dynamic_cast<ExprWith *>(expr)) {
            builder.field("scope-kind", std::string_view("with"));
            builder.field("with-attrs", showExpr(with->attrs));
        } else if (auto * let = dynamic_cast<ExprLet *>(expr)) {
            builder.field("scope-kind", std::string_view("let"));
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
                builder.field("let-binding-count", static_cast<uint64_t>(sorted.size()));
                for (auto & [name, def] : sorted) {
                    builder.field("let-binding-name", name);
                    builder.optionalField(
                        "let-binding-expr",
                        [&]() -> std::optional<std::string> {
                            if (auto * e = resolveBindingExpr(*def, let->attrs))
                                return showExpr(e);
                            return std::nullopt;
                        }());
                }
            } else {
                builder.field("let-binding-count", uint64_t{0});
            }
        } else if (auto * call = dynamic_cast<ExprCall *>(expr)) {
            // Hash the function expression so that changing the called
            // function (e.g., mapAliases → mapAliases2) invalidates all
            // binding hashes in the argument attrset.
            builder.field("scope-kind", std::string_view("call"));
            builder.field("call-fun", showExpr(call->fun));
        }
    }

    return NixScopeHash{builder.finish()};
}

// ═══════════════════════════════════════════════════════════════════════
// computeNixBindingHash — per-binding fingerprint
// ═══════════════════════════════════════════════════════════════════════

NixBindingHash computeNixBindingHash(
    const NixScopeHash & scopeHash,
    std::string_view name,
    int kindTag,
    const Expr * exprToShow,
    const SymbolTable & symbols,
    const CanonPath * basePath)
{
    auto builder = eval_trace::makeDomainBuilder<eval_trace::hash_domain::NixBindingBindingHash>();
    builder.field("scope-hash", scopeHash);
    builder.field("name", name);
    builder.field("kind-tag", static_cast<int64_t>(kindTag));
    if (exprToShow) {
        std::ostringstream ss;
        exprToShow->showForHash(symbols, ss, basePath ? *basePath : CanonPath::root);
        builder.optionalField("expr", std::optional<std::string>{ss.str()});
    } else {
        builder.optionalField("expr", std::optional<std::string>{});
    }
    return NixBindingHash{builder.finish()};
}

// ═══════════════════════════════════════════════════════════════════════
// registerNixBindings — populate the thread-local registry
// ═══════════════════════════════════════════════════════════════════════

void registerNixBindings(EvalState & state,
                         ExprAttrs * exprAttrs,
                         const NixScopeHash & scopeHash,
                         const SymbolTable & symbols,
                         InterningPools & pools,
                         const SourcePath & resolvedFile,
                         const std::optional<PathObject> & origin)
{
    auto parentDir = resolvedFile.path.parent().value_or(CanonPath::root);

    for (auto & [sym, def] : *exprAttrs->attrs) {
        if (!def.pos) continue;

        auto name = std::string(symbols[sym]);
        auto hash = computeNixBindingHash(
            scopeHash, name, static_cast<int>(def.kind),
            resolveBindingExpr(def, exprAttrs), symbols, &parentDir);
        auto dataPathId = pools.dataPathPool.internChild(pools.dataPathPool.root(), name);

        state.rememberNixBinding(def.pos, NixBindingEntry{
            dataPathId, hash, resolvedFile, origin});
    }
}

// ═══════════════════════════════════════════════════════════════════════
// maybeRecordNixBindingDep — attribute access recording
// ═══════════════════════════════════════════════════════════════════════

[[gnu::cold]] void maybeRecordNixBindingDep(
    const eval_trace::TraceAccess & access,
    EvalState & state,
    PosIdx pos)
{
    // Uses PosIdx as a join key: registerNixBindings populates the registry
    // with PosIdx→NixBindingEntry at parse time, and attribute access sites
    // (ExprSelect, ExprOpHasAttr, prim_getAttr) call this to record deps.
    // PosIdx survives the // overlay chain because ExprOpUpdate copies full
    // Attr structs including pos (eval.cc:2079-2105).
    if (!pos) return;

    auto * entry = state.lookupNixBinding(pos);
    if (!entry) return;

    // entry->resolvedFile was resolved at parse time with
    // SymlinkResolution::Ancestors (see registerNixBindings). Resolve Full
    // here so that dep-key identity matches the resolution semantics used by
    // other file-bytes recording paths (which call realiseCoercedPath with
    // SymlinkResolution::Full). For a regular .nix file this is a no-op; the
    // Full resolution matters when the leaf file is itself a symlink.
    SourcePath sp = entry->resolvedFile.resolveSymlinks(SymlinkResolution::Full);
    auto resolved = resolveProvenanceViaRegistry(access, sp, entry->origin);
    if (!resolved)
        return;

    auto & pools = access.tracingPools();
    auto sourceId = pools.intern<DepSourceId>(resolved->source);
    auto filePathId = pools.intern<FilePathId>(resolved->key);

    CompactDepComponents c{
        sourceId,
        filePathId,
        StructuredFormat::Nix,
        entry->dataPathId,
        ShapeSuffix::None,
        StringId(),
        StringId(),
    };
    access.recordStructured(c, DepHashValue(DepHash{entry->bindingHash.value}));

    recordFileBytesDepViaCache(access, state, sp, entry->origin);
}

} // namespace nix
