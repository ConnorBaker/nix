/**
 * HVM4 Compiler Basic Expression Emitters
 *
 * Contains emitters for basic expressions:
 * - emitInt, emitString, emitPath, emitVar, emitList
 * - emitConcatStrings, emitStringConcat
 * - Helper functions: isConstantString, isNumericAddition, wrapWithDups
 */

#include "nix/expr/hvm4/hvm4-compiler.hh"
#include "nix/expr/hvm4/hvm4-bigint.hh"
#include "nix/expr/hvm4/hvm4-list.hh"
#include "nix/expr/hvm4/hvm4-string.hh"
#include "nix/expr/hvm4/hvm4-path.hh"

namespace nix::hvm4 {

// ============================================================================
// Primitive Expression Emitters
// ============================================================================

Term HVM4Compiler::emitInt(const ExprInt& e, CompileContext& ctx) {
    // Get the integer value from the Value
    int64_t value = e.v.integer().value;
    return encodeInt64(value, ctx.runtime());
}

Term HVM4Compiler::emitFloat(const ExprFloat& e, CompileContext& ctx) {
    // Get the float value from the Value
    double value = e.v.fpoint();
    return encodeFloat(value, ctx.runtime());
}

Term HVM4Compiler::emitString(const ExprString& e, CompileContext& ctx) {
    // Intern the string and create a string term
    // ExprString stores the string in a Value, access via c_str()
    return makeStringFromContent(e.v.c_str(), stringTable_, ctx.runtime());
}

Term HVM4Compiler::emitPath(const ExprPath& e, CompileContext& ctx) {
    // Register the accessor and intern the path string
    // ExprPath stores accessor directly and the path in a Value
    uint32_t accessorId = accessorRegistry_.registerAccessor(&*e.accessor);
    uint32_t pathStringId = stringTable_.intern(e.v.pathStrView());
    return makePath(accessorId, pathStringId, ctx.runtime());
}

Term HVM4Compiler::emitVar(const ExprVar& e, CompileContext& ctx) {
    // First, check if it's a builtin constant (true, false, null)
    if (auto constTerm = getBuiltinConstant(e.name)) {
        return *constTerm;
    }

    // Check if this is a variable from a with expression
    if (e.fromWith != nullptr) {
        // In Nix, e.fromWith points to the INNERMOST with in scope, not necessarily
        // the one containing this attribute. We need to search from innermost to outermost.
        //
        // For simplicity, we emit a chained lookup that searches each with scope:
        // result = lookup(outer) then lookup(inner)
        // Each lookup returns ERA if not found, so the chain continues.
        //
        // Proper implementation would check hasAttr first, but for now we just
        // emit lookups and let the first found win (searching inner to outer).

        uint32_t symbolId = e.name.getId();

        // Find the position of e.fromWith in the with stack
        const auto& withStack = ctx.getWithStack();
        int startIdx = -1;
        for (int i = static_cast<int>(withStack.size()) - 1; i >= 0; i--) {
            if (withStack[i].expr == e.fromWith) {
                startIdx = i;
                break;
            }
        }

        if (startIdx < 0) {
            throw HVM4Error("Variable from with but no with binding found");
        }

        // Simple approach: search from outermost to innermost
        // Build: lookup(outer) then check lookup(inner) with hasAttr
        // Actually simpler: just lookup from outer and keep going until found
        //
        // For now, iterate from innermost to outermost and return first found
        // Emit: if hasAttr(inner, key) then inner.key
        //       else if hasAttr(outer, key) then outer.key
        //       else ERA

        // Simplified: just lookup from innermost with
        // TODO: implement proper fallback for nested with where attr is in outer scope
        // This is a known limitation - nested with where the variable is only in outer scope
        // will return ERA (null) instead of properly falling back

        VarBinding& attrBinding = ctx.getBindings()[withStack[startIdx].bindingIndex];

        // Get a reference to this attrset
        Term attrsTerm;
        if (attrBinding.useCount <= 1) {
            attrsTerm = HVM4Runtime::termNewVar(attrBinding.heapLoc);
        } else {
            uint32_t idx = attrBinding.dupIndex++;
            uint32_t numDups = attrBinding.useCount - 1;
            if (idx < numDups) {
                uint32_t dupLoc = attrBinding.dupLoc + 2 * idx;
                attrsTerm = HVM4Runtime::termNewCo0(attrBinding.dupLabel + idx, dupLoc);
            } else {
                uint32_t dupLoc = attrBinding.dupLoc + 2 * (numDups - 1);
                attrsTerm = HVM4Runtime::termNewCo1(attrBinding.dupLabel + numDups - 1, dupLoc);
            }
        }

        // Emit lookup from innermost with
        return emitAttrLookup(attrsTerm, symbolId, ctx);
    }

    auto* binding = ctx.lookup(e.name);
    if (!binding) {
        throw HVM4Error("Undefined variable in HVM4 compilation");
    }

    // Single-use variable: just reference the heap location
    if (binding->useCount <= 1) {
        return HVM4Runtime::termNewVar(binding->heapLoc);
    }

    // Multi-use variable: use CO0/CO1 projections from DUP chain
    // For N uses with N-1 DUPs:
    // - Uses 0 to N-2: CO0 of DUPs 0 to N-2
    // - Use N-1: CO1 of DUP N-2
    uint32_t idx = binding->dupIndex++;
    uint32_t numDups = binding->useCount - 1;

    if (idx < numDups) {
        // Use CO0 of DUP idx
        // Each DUP allocates 2 slots, so DUP idx is at dupLoc + 2*idx
        uint32_t dupLoc = binding->dupLoc + 2 * idx;
        return HVM4Runtime::termNewCo0(binding->dupLabel + idx, dupLoc);
    } else {
        // Last use: CO1 of last DUP (idx == numDups == useCount - 1)
        uint32_t dupLoc = binding->dupLoc + 2 * (numDups - 1);
        return HVM4Runtime::termNewCo1(binding->dupLabel + numDups - 1, dupLoc);
    }
}

Term HVM4Compiler::emitList(const ExprList& e, CompileContext& ctx) {
    // Compile each element and collect the terms
    std::vector<Term> elements;
    elements.reserve(e.elems.size());

    for (auto* elem : e.elems) {
        elements.push_back(emit(*elem, ctx));
    }

    // Build the list using the helper function
    return buildListFromElements(elements, ctx.runtime());
}

// ============================================================================
// String/Numeric Operations
// ============================================================================

bool HVM4Compiler::isConstantString(const Expr* expr) const {
    // Check if expression is a constant string (ExprString or nested constant string concat)
    if (dynamic_cast<const ExprString*>(expr)) return true;

    if (auto* concat = dynamic_cast<const ExprConcatStrings*>(expr)) {
        // Check if it's a string concat where ALL elements are constant strings
        if (!concat->es.empty()) {
            const Expr* first = concat->es[0].second;
            // If first is a string or nested string concat, check all elements
            if (dynamic_cast<const ExprString*>(first)) {
                for (const auto& elem : concat->es) {
                    if (!isConstantString(elem.second)) return false;
                }
                return true;
            }
            // If first is another concat, check if that's a constant string
            if (auto* nested = dynamic_cast<const ExprConcatStrings*>(first)) {
                if (!isNumericAddition(*nested)) {
                    // String concat - check all elements
                    for (const auto& elem : concat->es) {
                        if (!isConstantString(elem.second)) return false;
                    }
                    return true;
                }
            }
        }
    }

    return false;
}

bool HVM4Compiler::isNumericAddition(const ExprConcatStrings& e) const {
    // Addition happens when forceString=false AND first operand is numeric
    // In Nix, the + operator determines its behavior from the first operand type
    if (e.forceString || e.es.size() != 2) return false;

    // Check first operand type recursively
    const Expr* first = e.es[0].second;

    // If first is an integer literal, it's numeric addition
    if (dynamic_cast<const ExprInt*>(first)) return true;

    // If first is a string literal, it's string concatenation
    if (dynamic_cast<const ExprString*>(first)) return false;

    // If first is another ExprConcatStrings, check its first element recursively
    if (auto* concat = dynamic_cast<const ExprConcatStrings*>(first)) {
        return isNumericAddition(*concat);
    }

    // For variables, let bindings, etc., we need to track types through the compilation
    // For now, assume it's numeric if not a string literal (conservative choice that may fail at runtime)
    // TODO: Implement proper type tracking or runtime dispatch
    return true;
}

Term HVM4Compiler::emitConcatStrings(const ExprConcatStrings& e, CompileContext& ctx) {
    if (isNumericAddition(e)) {
        // This is numeric addition: a + b
        Term a = emit(*e.es[0].second, ctx);
        Term b = emit(*e.es[1].second, ctx);
        return ctx.runtime().termNewOp2(HVM4Runtime::OP_ADD, a, b);
    }

    // String concatenation
    return emitStringConcat(e, ctx);
}

Term HVM4Compiler::emitStringConcat(const ExprConcatStrings& e, CompileContext& ctx) {
    // Build all parts and concatenate them into a single string
    // Elements can be strings or coerced values (integers, paths, variables)
    if (e.es.empty()) {
        // Empty concatenation is empty string
        return makeStringFromContent("", stringTable_, ctx.runtime());
    }

    // Check if all elements are constant strings - if so, we can pre-compute
    bool allConstant = true;
    for (const auto& elem : e.es) {
        if (!isConstantString(elem.second)) {
            allConstant = false;
            break;
        }
    }

    if (allConstant) {
        // All elements are constant strings - emit and concatenate immediately
        std::vector<Term> parts;
        parts.reserve(e.es.size());
        for (const auto& elem : e.es) {
            parts.push_back(emit(*elem.second, ctx));
        }

        Term result = parts[0];
        for (size_t i = 1; i < parts.size(); i++) {
            result = concatStrings(result, parts[i], stringTable_, ctx.runtime());
        }
        return result;
    }

    // Some elements are non-constant - emit lazy concatenation terms
    // For each element, we need to:
    // 1. If it's a string literal, emit the string term
    // 2. If it's an integer, wrap in #SNum{} for runtime conversion
    // 3. If it's something else (variable, etc.), emit it and wrap in #SNum{} if needed
    std::vector<Term> parts;
    parts.reserve(e.es.size());

    for (const auto& elem : e.es) {
        const Expr* expr = elem.second;
        Term part;

        // Check what type of expression this is
        if (dynamic_cast<const ExprString*>(expr)) {
            // String literal - emit directly
            part = emit(*expr, ctx);
        } else if (dynamic_cast<const ExprInt*>(expr)) {
            // Integer literal - wrap in #SNum{} for string conversion
            Term intTerm = emit(*expr, ctx);
            part = makeStringFromInt(intTerm, ctx.runtime());
        } else if (isConstantString(expr)) {
            // Constant string concat - emit directly (already produces string term)
            part = emit(*expr, ctx);
        } else {
            // Variable or other expression - we need to determine at runtime
            // For now, assume it evaluates to a string or can be coerced
            // In a more complete implementation, we'd add runtime type checking
            Term valueTerm = emit(*expr, ctx);

            // If the expression is likely to evaluate to an integer (e.g., arithmetic),
            // wrap it in #SNum{}. Otherwise assume it's a string.
            // For now, we'll use a simple heuristic: if it's an ExprConcatStrings
            // that's numeric addition, wrap in #SNum{}
            if (auto* concat = dynamic_cast<const ExprConcatStrings*>(expr)) {
                if (isNumericAddition(*concat)) {
                    part = makeStringFromInt(valueTerm, ctx.runtime());
                } else {
                    // Nested string concat - already produces string-like term
                    part = valueTerm;
                }
            } else {
                // For variables and other expressions, we don't know the type
                // at compile time. For now, assume they evaluate to strings.
                // This works for most Nix patterns where interpolated values
                // are already strings.
                part = valueTerm;
            }
        }

        parts.push_back(part);
    }

    // Build the concatenation chain using #SCat{left, right}
    Term result = parts[0];
    for (size_t i = 1; i < parts.size(); i++) {
        result = makeStringConcat(result, parts[i], ctx.runtime());
    }

    return result;
}

// ============================================================================
// DUP Chain Generation
// ============================================================================

Term HVM4Compiler::wrapWithDups(Term body, CompileContext& ctx, size_t startBinding) {
    // For each multi-use variable, insert DUP nodes.
    //
    // For N uses of a variable, we need N-1 DUP nodes.
    // The DUPs are chained: each DUP (except the first) duplicates
    // the CO1 of the previous DUP. Uses map to projections:
    // - Use 0: CO0 of DUP 0
    // - Use 1: CO0 of DUP 1
    // - ...
    // - Use N-1: CO1 of DUP N-2
    //
    // The DUP chain structure (from outside in):
    //   DUP 0 = VAR(heapLoc)
    //   DUP 1 = CO1(DUP 0)
    //   ...
    //   DUP N-2 = CO1(DUP N-3)
    //   body (uses CO0/CO1 projections)

    auto& bindings = ctx.getBindings();

    for (size_t i = startBinding; i < bindings.size(); i++) {
        auto& binding = bindings[i];
        if (binding.useCount <= 1) continue;

        uint32_t numDups = binding.useCount - 1;

        // Build DUP chain from inside out
        // Start with the body, wrap with DUPs from innermost to outermost
        Term result = body;

        for (int j = numDups - 1; j >= 0; j--) {
            // Value being duplicated
            Term val;
            if (j == 0) {
                // First (outermost) DUP duplicates the original variable
                val = HVM4Runtime::termNewVar(binding.heapLoc);
            } else {
                // Subsequent DUPs duplicate the CO1 of the previous DUP
                uint32_t prevDupLoc = binding.dupLoc + 2 * (j - 1);
                val = HVM4Runtime::termNewCo1(binding.dupLabel + j - 1, prevDupLoc);
            }

            // Create DUP at pre-allocated location
            // CO0/CO1 references in the body use dupLoc + 2*j
            uint32_t dupLoc = binding.dupLoc + 2 * j;
            result = ctx.runtime().termNewDupAt(binding.dupLabel + j, dupLoc, val, result);
        }

        body = result;
    }

    return body;
}

}  // namespace nix::hvm4
