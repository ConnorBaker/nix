/**
 * HVM4 Compiler Analysis Functions
 *
 * Contains compile-time analysis functions:
 * - canCompileWithScope: Check if an expression can be compiled
 * - countUsages: Count variable usages for DUP allocation
 * - countWithUsages: Count uses of with attrsets
 * - collectDependencies: Collect variable dependencies for recursive lets
 * - topologicalSort: Sort bindings by dependencies
 */

#include "nix/expr/hvm4/hvm4-compiler.hh"

namespace nix::hvm4 {

// ============================================================================
// canCompileWithScope Implementation
// ============================================================================

bool HVM4Compiler::canCompileWithScope(const Expr& expr, std::vector<Symbol>& scope) const {
    // Check expression type using dynamic_cast
    if (dynamic_cast<const ExprInt*>(&expr)) return true;
    if (dynamic_cast<const ExprFloat*>(&expr)) return true;
    if (dynamic_cast<const ExprString*>(&expr)) return true;
    if (dynamic_cast<const ExprPath*>(&expr)) return true;

    if (auto* e = dynamic_cast<const ExprVar*>(&expr)) {
        // Variable must be in scope (bound by lambda or let)
        for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
            if (*it == e->name) return true;
        }
        // Check if it's a builtin constant (true, false, null)
        if (getBuiltinConstant(e->name).has_value()) {
            return true;
        }
        // Check if it's from a with expression (will be resolved at runtime)
        if (e->fromWith != nullptr) {
            return true;  // Variable from with - resolved via attr lookup
        }
        return false;  // Free variable (likely a builtin) - not supported
    }

    if (auto* e = dynamic_cast<const ExprLambda*>(&expr)) {
        // Support both simple lambdas (x: body) and pattern-matching ({ a, b ? 1, ... }: body)
        if (auto formals = e->getFormals()) {
            // Pattern-matching lambda
            // Add all formals to scope FIRST - defaults can reference other formals
            // In Nix, { a, b ? a * 2 }: ... is valid and a is in scope for b's default
            for (const auto& formal : formals->formals) {
                scope.push_back(formal.name);
            }
            // Also add @-pattern binding if present
            if (e->arg) {
                scope.push_back(e->arg);
            }
            // Now check all default value expressions (with formals in scope)
            for (const auto& formal : formals->formals) {
                if (formal.def) {
                    if (!canCompileWithScope(*formal.def, scope)) return false;
                }
            }
            bool result = canCompileWithScope(*e->body, scope);
            // Pop all added bindings
            if (e->arg) scope.pop_back();
            for (size_t i = 0; i < formals->formals.size(); i++) {
                scope.pop_back();
            }
            return result;
        } else {
            // Simple lambda
            scope.push_back(e->arg);
            bool result = canCompileWithScope(*e->body, scope);
            scope.pop_back();
            return result;
        }
    }

    if (auto* e = dynamic_cast<const ExprCall*>(&expr)) {
        // Check for arithmetic primop calls (__sub, __mul, __div, __lessThan)
        // These are generated when Nix desugars operators like -, *, /, <
        if (auto* funVar = dynamic_cast<const ExprVar*>(e->fun)) {
            if (getArithmeticPrimopOpcode(funVar->name).has_value()) {
                // Arithmetic primop: must have exactly 2 arguments
                if (e->args && e->args->size() == 2) {
                    // Reject string operands for arithmetic/comparison ops
                    if (dynamic_cast<const ExprString*>((*e->args)[0]) ||
                        dynamic_cast<const ExprString*>((*e->args)[1])) {
                        return false;
                    }
                    // Reject float operands - float arithmetic not implemented
                    if (dynamic_cast<const ExprFloat*>((*e->args)[0]) ||
                        dynamic_cast<const ExprFloat*>((*e->args)[1])) {
                        return false;
                    }
                    return canCompileWithScope(*(*e->args)[0], scope) &&
                           canCompileWithScope(*(*e->args)[1], scope);
                }
                return false;  // Wrong number of arguments
            }
        }

        // Regular function call
        if (!canCompileWithScope(*e->fun, scope)) return false;
        if (e->args) {
            for (auto* arg : *e->args) {
                if (!canCompileWithScope(*arg, scope)) return false;
            }
        }
        return true;
    }

    if (auto* e = dynamic_cast<const ExprIf*>(&expr)) {
        return canCompileWithScope(*e->cond, scope) &&
               canCompileWithScope(*e->then, scope) &&
               canCompileWithScope(*e->else_, scope);
    }

    if (auto* e = dynamic_cast<const ExprLet*>(&expr)) {
        // Only support non-recursive let
        if (e->attrs->recursive) return false;

        // Check bindings incrementally - each binding can reference prior bindings
        // For non-recursive let: x = e1; f = e2; means e2 can reference x
        size_t bindingsAdded = 0;
        if (e->attrs->attrs) {
            for (auto& [name, def] : *e->attrs->attrs) {
                // Check this binding's expression with current scope
                if (!canCompileWithScope(*def.e, scope)) {
                    // Pop any bindings we added
                    for (size_t i = 0; i < bindingsAdded; i++) scope.pop_back();
                    return false;
                }
                // Add this binding to scope for subsequent bindings
                scope.push_back(name);
                bindingsAdded++;
            }
        }
        bool result = canCompileWithScope(*e->body, scope);
        // Remove all bindings from scope
        for (size_t i = 0; i < bindingsAdded; i++) {
            scope.pop_back();
        }
        return result;
    }

    if (auto* e = dynamic_cast<const ExprOpNot*>(&expr)) {
        return canCompileWithScope(*e->e, scope);
    }

    if (auto* e = dynamic_cast<const ExprOpAnd*>(&expr)) {
        return canCompileWithScope(*e->e1, scope) && canCompileWithScope(*e->e2, scope);
    }

    if (auto* e = dynamic_cast<const ExprOpOr*>(&expr)) {
        return canCompileWithScope(*e->e1, scope) && canCompileWithScope(*e->e2, scope);
    }

    if (auto* e = dynamic_cast<const ExprOpImpl*>(&expr)) {
        return canCompileWithScope(*e->e1, scope) && canCompileWithScope(*e->e2, scope);
    }

    if (auto* e = dynamic_cast<const ExprAssert*>(&expr)) {
        return canCompileWithScope(*e->cond, scope) && canCompileWithScope(*e->body, scope);
    }

    if (auto* e = dynamic_cast<const ExprOpEq*>(&expr)) {
        // String comparison is not yet implemented
        if (dynamic_cast<const ExprString*>(e->e1) || dynamic_cast<const ExprString*>(e->e2)) {
            return false;
        }
        // Float comparison is not yet implemented
        if (dynamic_cast<const ExprFloat*>(e->e1) || dynamic_cast<const ExprFloat*>(e->e2)) {
            return false;
        }
        return canCompileWithScope(*e->e1, scope) && canCompileWithScope(*e->e2, scope);
    }

    if (auto* e = dynamic_cast<const ExprOpNEq*>(&expr)) {
        // String comparison is not yet implemented
        if (dynamic_cast<const ExprString*>(e->e1) || dynamic_cast<const ExprString*>(e->e2)) {
            return false;
        }
        // Float comparison is not yet implemented
        if (dynamic_cast<const ExprFloat*>(e->e1) || dynamic_cast<const ExprFloat*>(e->e2)) {
            return false;
        }
        return canCompileWithScope(*e->e1, scope) && canCompileWithScope(*e->e2, scope);
    }

    if (auto* e = dynamic_cast<const ExprConcatStrings*>(&expr)) {
        // Support both numeric addition and string concatenation
        // The type is determined by the first element at runtime in Nix
        // We must reject expressions where we can't determine type at compile time

        if (e->es.empty()) return true;  // Empty concat is fine

        const Expr* first = e->es[0].second;

        // forceString=true means string interpolation context
        // We support both constant strings and variables/expressions that will be
        // coerced to strings at runtime
        if (e->forceString) {
            for (const auto& elem : e->es) {
                // Each element must either be a constant string, or a compilable expression
                // that will be coerced to a string at runtime
                if (!isConstantString(elem.second)) {
                    // Reject path expressions - path-to-string coercion requires store copy
                    // which is not yet implemented
                    if (dynamic_cast<const ExprPath*>(elem.second)) {
                        return false;  // Path coercion not implemented
                    }
                    // Reject lambda expressions - cannot coerce functions to strings
                    if (dynamic_cast<const ExprLambda*>(elem.second)) {
                        return false;  // Cannot coerce lambda to string
                    }
                    // Non-constant element - must be compilable
                    // We'll convert it to a string at runtime
                    if (!canCompileWithScope(*elem.second, scope)) {
                        return false;  // Non-compilable element in string interpolation
                    }
                }
            }
            return true;
        }

        // Determine operation type from first operand
        if (dynamic_cast<const ExprInt*>(first)) {
            // Numeric addition - need exactly 2 elements, both must be numeric
            if (e->es.size() != 2) return false;
            // Check that second operand is also numeric (int or nested numeric add)
            const Expr* second = e->es[1].second;
            // Reject if second operand is a float (float arithmetic not implemented)
            if (dynamic_cast<const ExprFloat*>(second)) return false;
            if (!dynamic_cast<const ExprInt*>(second)) {
                // Second could be a variable or nested expression - allow if compilable
                if (!canCompileWithScope(*second, scope)) return false;
            }
            return canCompileWithScope(*first, scope);
        }

        // Float arithmetic is not yet supported - OP_ADD operates on NUM, not float constructors
        // Float literals are supported, but float + float and int + float are not
        if (dynamic_cast<const ExprFloat*>(first)) {
            return false;
        }

        if (dynamic_cast<const ExprString*>(first)) {
            // String concatenation - only support constant strings
            for (const auto& elem : e->es) {
                if (!isConstantString(elem.second)) {
                    return false;  // Non-constant in string concat
                }
            }
            return true;
        }

        // Path concatenation (path + string) is not yet implemented
        // Reject any ConcatStrings starting with a path
        if (dynamic_cast<const ExprPath*>(first)) {
            return false;
        }

        // First operand is nested concat - check recursively
        if (auto* concat = dynamic_cast<const ExprConcatStrings*>(first)) {
            // Determine if this is numeric or string from the nested concat
            if (isNumericAddition(*concat)) {
                // Numeric addition chain
                if (e->es.size() != 2) return false;
                // Reject if second operand is a float (float arithmetic not implemented)
                if (dynamic_cast<const ExprFloat*>(e->es[1].second)) return false;
                return canCompileWithScope(*first, scope) && canCompileWithScope(*e->es[1].second, scope);
            } else {
                // String concat chain - all must be constant strings
                for (const auto& elem : e->es) {
                    if (!isConstantString(elem.second)) {
                        return false;
                    }
                }
                return true;
            }
        }

        // First operand is variable, call, let, etc. - we can't determine type at compile time
        // We use heuristics to decide:
        // - If ANY operand is a string literal, this is string concat (reject with variables)
        // - If no string literals visible, assume numeric addition (may be wrong at runtime
        //   but keeps existing behavior for common cases like `let x = 1; y = 2; in x + y`)
        if (e->es.size() == 2) {
            const Expr* second = e->es[1].second;

            // Check if second operand is a string (literal or constant concat)
            bool secondIsString = dynamic_cast<const ExprString*>(second) != nullptr ||
                                  isConstantString(second);

            if (secondIsString) {
                // Mixing variable with string literal - reject (string concat with var)
                return false;
            }

            // Check if second operand is a float - float arithmetic not implemented
            if (dynamic_cast<const ExprFloat*>(second)) {
                return false;
            }

            // No obvious string involvement - assume numeric, let runtime handle it
            return canCompileWithScope(*first, scope) && canCompileWithScope(*second, scope);
        }

        // Multi-operand concat (3+ elements) with unknown first operand - reject
        return false;
    }

    if (auto* e = dynamic_cast<const ExprList*>(&expr)) {
        // All list elements must be compilable
        for (auto* elem : e->elems) {
            if (!canCompileWithScope(*elem, scope)) return false;
        }
        return true;
    }

    if (auto* e = dynamic_cast<const ExprAttrs*>(&expr)) {
        // Support both non-recursive and acyclic recursive attribute sets
        if (e->dynamicAttrs && !e->dynamicAttrs->empty()) return false;

        if (e->recursive) {
            // Recursive attr set - check if acyclic and all expressions compile
            // First check all inheritFromExprs can be compiled
            if (e->inheritFromExprs) {
                for (auto* fromExpr : *e->inheritFromExprs) {
                    if (!canCompileWithScope(*fromExpr, scope)) return false;
                }
            }

            // Collect binding names
            std::set<Symbol> bindingNames;
            if (e->attrs) {
                for (const auto& [name, def] : *e->attrs) {
                    // Support Plain, Inherited, and InheritedFrom
                    if (def.kind != ExprAttrs::AttrDef::Kind::Plain &&
                        def.kind != ExprAttrs::AttrDef::Kind::Inherited &&
                        def.kind != ExprAttrs::AttrDef::Kind::InheritedFrom) return false;
                    bindingNames.insert(name);
                }
            }

            // Build dependency graph
            std::map<Symbol, std::set<Symbol>> deps;
            if (e->attrs) {
                for (const auto& [name, def] : *e->attrs) {
                    deps[name] = std::set<Symbol>();
                    collectDependencies(*def.e, bindingNames, deps[name]);
                }
            }

            // Check for cycles
            auto sortedOpt = topologicalSort(deps);
            if (!sortedOpt) {
                return false;  // Cyclic dependencies not supported yet
            }

            // Add all bindings to scope for checking
            for (const auto& name : bindingNames) {
                scope.push_back(name);
            }

            // Check all expressions can compile
            bool result = true;
            if (e->attrs) {
                for (const auto& [name, def] : *e->attrs) {
                    if (!canCompileWithScope(*def.e, scope)) {
                        result = false;
                        break;
                    }
                }
            }

            // Pop bindings
            for (size_t i = 0; i < bindingNames.size(); i++) {
                scope.pop_back();
            }

            return result;
        } else {
            // Non-recursive attr set
            // First check all inheritFromExprs can be compiled
            if (e->inheritFromExprs) {
                for (auto* fromExpr : *e->inheritFromExprs) {
                    if (!canCompileWithScope(*fromExpr, scope)) return false;
                }
            }

            if (e->attrs) {
                for (const auto& [name, def] : *e->attrs) {
                    // Support Plain, Inherited, and InheritedFrom
                    if (def.kind != ExprAttrs::AttrDef::Kind::Plain &&
                        def.kind != ExprAttrs::AttrDef::Kind::Inherited &&
                        def.kind != ExprAttrs::AttrDef::Kind::InheritedFrom) return false;

                    // For InheritedFrom, we don't need to check def.e here since
                    // it's an ExprSelect from ExprInheritFrom which references inheritFromExprs
                    // which we already checked above
                    if (def.kind != ExprAttrs::AttrDef::Kind::InheritedFrom) {
                        if (!canCompileWithScope(*def.e, scope)) return false;
                    }
                }
            }
            return true;
        }
    }

    if (auto* e = dynamic_cast<const ExprSelect*>(&expr)) {
        // Attribute selection: expr.a.b.c or expr.a.b.c or default
        // Support multi-level nested attribute paths with static attribute names

        // Check that all attribute names in the path are static (Symbols, not dynamic expressions)
        for (size_t i = 0; i < e->nAttrPath; i++) {
            const AttrName& attrName = e->attrPathStart[i];
            if (attrName.expr) return false;  // Dynamic attribute name not supported
        }

        // Check the default expression if present
        if (e->def && !canCompileWithScope(*e->def, scope)) {
            return false;
        }

        return canCompileWithScope(*e->e, scope);
    }

    if (auto* e = dynamic_cast<const ExprOpHasAttr*>(&expr)) {
        // Has attribute: expr ? attr
        // Only support simple single-level check
        if (e->attrPath.size() != 1) return false;

        // Check that the attribute name is static
        const AttrName& attrName = e->attrPath[0];
        if (attrName.expr) return false;  // Dynamic attribute name not supported

        return canCompileWithScope(*e->e, scope);
    }

    if (auto* e = dynamic_cast<const ExprOpUpdate*>(&expr)) {
        // Attribute update: attrs1 // attrs2
        return canCompileWithScope(*e->e1, scope) && canCompileWithScope(*e->e2, scope);
    }

    if (auto* e = dynamic_cast<const ExprOpConcatLists*>(&expr)) {
        // List concatenation: list1 ++ list2
        // Currently only supported when both operands are direct list literals
        // (the compile-time optimization path)
        if (!canCompileWithScope(*e->e1, scope) || !canCompileWithScope(*e->e2, scope)) {
            return false;
        }
        // Check if both operands are list literals - only those are supported for now
        auto* list1 = dynamic_cast<const ExprList*>(e->e1);
        auto* list2 = dynamic_cast<const ExprList*>(e->e2);
        return list1 != nullptr && list2 != nullptr;
    }

    if (auto* e = dynamic_cast<const ExprWith*>(&expr)) {
        // With expression: with attrs; body
        // Check that attrs can be compiled
        if (!canCompileWithScope(*e->attrs, scope)) return false;
        // Check that body can be compiled (variables from with are handled in ExprVar)
        if (!canCompileWithScope(*e->body, scope)) return false;
        return true;
    }

    // Not supported: ExprPos, etc.
    return false;
}

// ============================================================================
// countUsages Implementation (First Pass)
// ============================================================================

void HVM4Compiler::countUsages(const Expr& expr, CompileContext& ctx) {
    if (dynamic_cast<const ExprInt*>(&expr)) {
        // No variables to count
        return;
    }

    if (dynamic_cast<const ExprFloat*>(&expr)) {
        // No variables to count
        return;
    }

    if (dynamic_cast<const ExprString*>(&expr)) {
        // No variables to count
        return;
    }

    if (dynamic_cast<const ExprPath*>(&expr)) {
        // No variables to count - paths are constants
        return;
    }

    if (auto* e = dynamic_cast<const ExprVar*>(&expr)) {
        if (auto* binding = ctx.lookup(e->name)) {
            // Only count if this binding was pushed for counting (heapLoc=0).
            // Bindings pushed during second pass (heapLoc>0) have already been
            // counted and configured - don't increment their useCount again.
            if (binding->heapLoc == 0) {
                binding->useCount++;
            }
        }
        // If not found, it's a free variable (error will be caught in emit)
        return;
    }

    if (auto* e = dynamic_cast<const ExprLambda*>(&expr)) {
        if (auto formals = e->getFormals()) {
            // Pattern-matching lambda: count usages in defaults and body
            // First count usages in default expressions
            for (const auto& formal : formals->formals) {
                if (formal.def) {
                    countUsages(*formal.def, ctx);
                }
            }
            // Push all formals as bindings for body
            for (const auto& formal : formals->formals) {
                ctx.pushBinding(formal.name);
            }
            // Also push @-pattern binding if present
            if (e->arg) {
                ctx.pushBinding(e->arg);
            }
            countUsages(*e->body, ctx);
            // Pop all bindings
            if (e->arg) ctx.popBinding();
            for (size_t i = 0; i < formals->formals.size(); i++) {
                ctx.popBinding();
            }
        } else {
            // Simple lambda
            ctx.pushBinding(e->arg);
            countUsages(*e->body, ctx);
            ctx.popBinding();
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprCall*>(&expr)) {
        countUsages(*e->fun, ctx);
        if (e->args) {
            for (auto* arg : *e->args) {
                countUsages(*arg, ctx);
            }
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprIf*>(&expr)) {
        countUsages(*e->cond, ctx);
        countUsages(*e->then, ctx);
        countUsages(*e->else_, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprLet*>(&expr)) {
        // First count usages in binding expressions
        if (e->attrs->attrs) {
            for (auto& [name, def] : *e->attrs->attrs) {
                countUsages(*def.e, ctx);
            }
        }
        // Then push bindings for body
        if (e->attrs->attrs) {
            for (auto& [name, def] : *e->attrs->attrs) {
                ctx.pushBinding(name);
            }
        }
        countUsages(*e->body, ctx);
        // Pop bindings
        if (e->attrs->attrs) {
            for (size_t i = 0; i < e->attrs->attrs->size(); i++) {
                ctx.popBinding();
            }
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpNot*>(&expr)) {
        countUsages(*e->e, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpAnd*>(&expr)) {
        countUsages(*e->e1, ctx);
        countUsages(*e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpOr*>(&expr)) {
        countUsages(*e->e1, ctx);
        countUsages(*e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpImpl*>(&expr)) {
        countUsages(*e->e1, ctx);
        countUsages(*e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprAssert*>(&expr)) {
        countUsages(*e->cond, ctx);
        countUsages(*e->body, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpEq*>(&expr)) {
        countUsages(*e->e1, ctx);
        countUsages(*e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpNEq*>(&expr)) {
        countUsages(*e->e1, ctx);
        countUsages(*e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprConcatStrings*>(&expr)) {
        for (auto& elem : e->es) {
            countUsages(*elem.second, ctx);
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprList*>(&expr)) {
        for (auto* elem : e->elems) {
            countUsages(*elem, ctx);
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprAttrs*>(&expr)) {
        if (e->attrs) {
            for (const auto& [name, def] : *e->attrs) {
                countUsages(*def.e, ctx);
            }
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprSelect*>(&expr)) {
        countUsages(*e->e, ctx);
        if (e->def) {
            countUsages(*e->def, ctx);
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpHasAttr*>(&expr)) {
        countUsages(*e->e, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpUpdate*>(&expr)) {
        countUsages(*e->e1, ctx);
        countUsages(*e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpConcatLists*>(&expr)) {
        countUsages(*e->e1, ctx);
        countUsages(*e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprWith*>(&expr)) {
        // Count usages in attrs and body
        // The with attrset is used once for each variable lookup from it
        countUsages(*e->attrs, ctx);
        countUsages(*e->body, ctx);
        return;
    }
}

// ============================================================================
// countWithUsages Implementation
// ============================================================================

void HVM4Compiler::countWithUsages(const ExprWith& withExpr, const Expr& expr, CompileContext& ctx) {
    // Count how many variables in expr are from the given with expression
    // This increments the with binding's useCount for each variable from it

    if (auto* e = dynamic_cast<const ExprVar*>(&expr)) {
        if (e->fromWith == &withExpr) {
            // This variable is from our with expression
            // Increment the with attrset binding's useCount
            // Just increment the last binding (which is the with attrset)
            auto& bindings = ctx.getBindings();
            if (!bindings.empty()) {
                bindings.back().useCount++;
            }
        }
        return;
    }

    if (dynamic_cast<const ExprInt*>(&expr)) return;
    if (dynamic_cast<const ExprFloat*>(&expr)) return;
    if (dynamic_cast<const ExprString*>(&expr)) return;
    if (dynamic_cast<const ExprPath*>(&expr)) return;

    if (auto* e = dynamic_cast<const ExprLambda*>(&expr)) {
        if (e->getFormals()) {
            for (const auto& formal : e->getFormals()->formals) {
                if (formal.def) {
                    countWithUsages(withExpr, *formal.def, ctx);
                }
            }
        }
        countWithUsages(withExpr, *e->body, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprCall*>(&expr)) {
        countWithUsages(withExpr, *e->fun, ctx);
        if (e->args) {
            for (auto* arg : *e->args) {
                countWithUsages(withExpr, *arg, ctx);
            }
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprIf*>(&expr)) {
        countWithUsages(withExpr, *e->cond, ctx);
        countWithUsages(withExpr, *e->then, ctx);
        countWithUsages(withExpr, *e->else_, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprLet*>(&expr)) {
        if (e->attrs && e->attrs->attrs) {
            for (auto& [name, def] : *e->attrs->attrs) {
                countWithUsages(withExpr, *def.e, ctx);
            }
        }
        countWithUsages(withExpr, *e->body, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpNot*>(&expr)) {
        countWithUsages(withExpr, *e->e, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpAnd*>(&expr)) {
        countWithUsages(withExpr, *e->e1, ctx);
        countWithUsages(withExpr, *e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpOr*>(&expr)) {
        countWithUsages(withExpr, *e->e1, ctx);
        countWithUsages(withExpr, *e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpImpl*>(&expr)) {
        countWithUsages(withExpr, *e->e1, ctx);
        countWithUsages(withExpr, *e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprAssert*>(&expr)) {
        countWithUsages(withExpr, *e->cond, ctx);
        countWithUsages(withExpr, *e->body, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpEq*>(&expr)) {
        countWithUsages(withExpr, *e->e1, ctx);
        countWithUsages(withExpr, *e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpNEq*>(&expr)) {
        countWithUsages(withExpr, *e->e1, ctx);
        countWithUsages(withExpr, *e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprConcatStrings*>(&expr)) {
        for (const auto& elem : e->es) {
            countWithUsages(withExpr, *elem.second, ctx);
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprList*>(&expr)) {
        for (auto* elem : e->elems) {
            countWithUsages(withExpr, *elem, ctx);
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprAttrs*>(&expr)) {
        if (e->attrs) {
            for (auto& [name, def] : *e->attrs) {
                countWithUsages(withExpr, *def.e, ctx);
            }
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprSelect*>(&expr)) {
        countWithUsages(withExpr, *e->e, ctx);
        if (e->def) {
            countWithUsages(withExpr, *e->def, ctx);
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpHasAttr*>(&expr)) {
        countWithUsages(withExpr, *e->e, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpUpdate*>(&expr)) {
        countWithUsages(withExpr, *e->e1, ctx);
        countWithUsages(withExpr, *e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpConcatLists*>(&expr)) {
        countWithUsages(withExpr, *e->e1, ctx);
        countWithUsages(withExpr, *e->e2, ctx);
        return;
    }

    if (auto* e = dynamic_cast<const ExprWith*>(&expr)) {
        // Recurse into nested with
        countWithUsages(withExpr, *e->attrs, ctx);
        countWithUsages(withExpr, *e->body, ctx);
        return;
    }
}

// =============================================================================
// Recursive Let Helpers
// =============================================================================

void HVM4Compiler::collectDependencies(const Expr& expr, const std::set<Symbol>& candidates,
                                       std::set<Symbol>& deps) const {
    // Recursively collect variable references that are in the candidates set

    if (auto* e = dynamic_cast<const ExprVar*>(&expr)) {
        if (candidates.count(e->name)) {
            deps.insert(e->name);
        }
        return;
    }

    if (dynamic_cast<const ExprInt*>(&expr)) {
        return;  // No dependencies
    }

    if (dynamic_cast<const ExprFloat*>(&expr)) {
        return;  // No dependencies
    }

    if (dynamic_cast<const ExprString*>(&expr)) {
        return;  // No dependencies
    }

    if (dynamic_cast<const ExprPath*>(&expr)) {
        return;  // No dependencies
    }

    if (auto* e = dynamic_cast<const ExprLambda*>(&expr)) {
        // Lambda body might reference candidates, but the parameter shadows
        std::set<Symbol> innerCandidates = candidates;
        if (auto formals = e->getFormals()) {
            for (const auto& formal : formals->formals) {
                innerCandidates.erase(formal.name);
                if (formal.def) {
                    collectDependencies(*formal.def, candidates, deps);
                }
            }
            if (e->arg) {
                innerCandidates.erase(e->arg);
            }
        } else {
            innerCandidates.erase(e->arg);
        }
        collectDependencies(*e->body, innerCandidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprCall*>(&expr)) {
        collectDependencies(*e->fun, candidates, deps);
        if (e->args) {
            for (auto* arg : *e->args) {
                collectDependencies(*arg, candidates, deps);
            }
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprIf*>(&expr)) {
        collectDependencies(*e->cond, candidates, deps);
        collectDependencies(*e->then, candidates, deps);
        collectDependencies(*e->else_, candidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprLet*>(&expr)) {
        // Let bindings shadow candidates
        std::set<Symbol> innerCandidates = candidates;
        if (e->attrs && e->attrs->attrs) {
            for (const auto& [name, def] : *e->attrs->attrs) {
                collectDependencies(*def.e, candidates, deps);
                innerCandidates.erase(name);
            }
        }
        collectDependencies(*e->body, innerCandidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpNot*>(&expr)) {
        collectDependencies(*e->e, candidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpAnd*>(&expr)) {
        collectDependencies(*e->e1, candidates, deps);
        collectDependencies(*e->e2, candidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpOr*>(&expr)) {
        collectDependencies(*e->e1, candidates, deps);
        collectDependencies(*e->e2, candidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpImpl*>(&expr)) {
        collectDependencies(*e->e1, candidates, deps);
        collectDependencies(*e->e2, candidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprAssert*>(&expr)) {
        collectDependencies(*e->cond, candidates, deps);
        collectDependencies(*e->body, candidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpEq*>(&expr)) {
        collectDependencies(*e->e1, candidates, deps);
        collectDependencies(*e->e2, candidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpNEq*>(&expr)) {
        collectDependencies(*e->e1, candidates, deps);
        collectDependencies(*e->e2, candidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprConcatStrings*>(&expr)) {
        for (const auto& elem : e->es) {
            collectDependencies(*elem.second, candidates, deps);
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprList*>(&expr)) {
        for (auto* elem : e->elems) {
            collectDependencies(*elem, candidates, deps);
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprAttrs*>(&expr)) {
        if (e->attrs) {
            for (const auto& [name, def] : *e->attrs) {
                collectDependencies(*def.e, candidates, deps);
            }
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprSelect*>(&expr)) {
        collectDependencies(*e->e, candidates, deps);
        if (e->def) {
            collectDependencies(*e->def, candidates, deps);
        }
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpHasAttr*>(&expr)) {
        collectDependencies(*e->e, candidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpUpdate*>(&expr)) {
        collectDependencies(*e->e1, candidates, deps);
        collectDependencies(*e->e2, candidates, deps);
        return;
    }

    if (auto* e = dynamic_cast<const ExprOpConcatLists*>(&expr)) {
        collectDependencies(*e->e1, candidates, deps);
        collectDependencies(*e->e2, candidates, deps);
        return;
    }

    // For other expression types, no dependencies collected
}

std::optional<std::vector<Symbol>> HVM4Compiler::topologicalSort(
    const std::map<Symbol, std::set<Symbol>>& deps) const {
    // Kahn's algorithm for topological sorting
    // Returns std::nullopt if there's a cycle

    // Build in-degree map and adjacency list
    std::map<Symbol, int> inDegree;
    std::map<Symbol, std::set<Symbol>> dependents;

    for (const auto& [sym, _] : deps) {
        inDegree[sym] = 0;
    }

    for (const auto& [sym, dependencies] : deps) {
        for (const auto& dep : dependencies) {
            if (deps.count(dep)) {  // Only count dependencies that are in our set
                dependents[dep].insert(sym);
                inDegree[sym]++;
            }
        }
    }

    // Start with nodes that have no dependencies
    std::vector<Symbol> queue;
    for (const auto& [sym, degree] : inDegree) {
        if (degree == 0) {
            queue.push_back(sym);
        }
    }

    std::vector<Symbol> result;
    result.reserve(deps.size());

    while (!queue.empty()) {
        Symbol current = queue.back();
        queue.pop_back();
        result.push_back(current);

        for (const auto& dependent : dependents[current]) {
            inDegree[dependent]--;
            if (inDegree[dependent] == 0) {
                queue.push_back(dependent);
            }
        }
    }

    // If we didn't process all nodes, there's a cycle
    if (result.size() != deps.size()) {
        return std::nullopt;
    }

    return result;
}

}  // namespace nix::hvm4
