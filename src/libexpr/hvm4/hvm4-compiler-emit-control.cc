/**
 * HVM4 Compiler Control Flow Emitters
 *
 * Contains emitters for control flow expressions:
 * - emitCall: Function application
 * - emitIf: If-then-else conditionals
 * - emitLet: Let expressions
 * - emitWith: With expressions
 */

#include "nix/expr/hvm4/hvm4-compiler.hh"
#include "nix/expr/hvm4/hvm4-bigint.hh"

namespace nix::hvm4 {

// ============================================================================
// Function Application
// ============================================================================

Term HVM4Compiler::emitCall(const ExprCall& e, CompileContext& ctx) {
    // Check for arithmetic primop calls (__sub, __mul, __div, __lessThan)
    if (auto* funVar = dynamic_cast<const ExprVar*>(e.fun)) {
        if (auto opcode = getArithmeticPrimopOpcode(funVar->name)) {
            // Emit as HVM4 binary operation
            if (!e.args || e.args->size() != 2) {
                throw HVM4Error("Arithmetic primop requires exactly 2 arguments");
            }
            Term left = emit(*(*e.args)[0], ctx);
            Term right = emit(*(*e.args)[1], ctx);

            // For less-than, use BigInt-aware comparison.
            // This handles both small integers (NUM) and large integers (BigInt constructors).
            if (*opcode == HVM4Runtime::OP_LT) {
                return emitBigIntLessThan(left, right, ctx.runtime());
            }

            return ctx.runtime().termNewOp2(*opcode, left, right);
        }
    }

    // Regular function call
    Term fun = emit(*e.fun, ctx);

    // Apply each argument
    if (e.args) {
        for (auto* arg : *e.args) {
            Term argTerm = emit(*arg, ctx);
            fun = ctx.runtime().termNewApp(fun, argTerm);
        }
    }

    return fun;
}

// ============================================================================
// If-Then-Else
// ============================================================================

Term HVM4Compiler::emitIf(const ExprIf& e, CompileContext& ctx) {
    // Compile condition, then, and else branches
    Term cond = emit(*e.cond, ctx);
    Term thenBranch = emit(*e.then, ctx);
    Term elseBranch = emit(*e.else_, ctx);

    // HVM4 conditional using MAT:
    // We create: (MAT 0 thenBranch (λ_. elseBranch)) cond
    // - If cond == 0 (false), return thenBranch... wait, that's backwards
    //
    // Actually, in Nix: true = 1, false = 0 (typically)
    // In HVM4 MAT: (MAT tag f g) applied to #tag returns f, otherwise (g val)
    //
    // For boolean, we want:
    //   if cond then thenBranch else elseBranch
    //
    // We can encode this as:
    //   (MAT 1 thenBranch (λ_. elseBranch)) cond
    // Which means: if cond == 1 (true), return thenBranch; else apply (λ_. elseBranch) to cond
    //
    // But actually HVM4's SWI/MAT with NUM is:
    //   (SWI n f g) applied to #m: if n==m then f else (g #m)
    //
    // So for if-then-else:
    //   (SWI 0 elseBranch (λ_. thenBranch)) cond
    // if cond == 0 (false): return elseBranch
    // if cond != 0 (true): return (λ_. thenBranch) cond = thenBranch

    // Create the switch term
    Term matcher = ctx.runtime().termNewMat(0, elseBranch, ctx.runtime().termNewLam(thenBranch));

    // Apply to condition
    return ctx.runtime().termNewApp(matcher, cond);
}

// ============================================================================
// Let Expressions
// ============================================================================

Term HVM4Compiler::emitLet(const ExprLet& e, CompileContext& ctx) {
    // Only support non-recursive let
    if (e.attrs->recursive) {
        throw HVM4Error("Recursive let not supported by HVM4 backend");
    }

    if (!e.attrs->attrs || e.attrs->attrs->empty()) {
        // No bindings, just emit body
        return emit(*e.body, ctx);
    }

    // We encode let as:
    //   let x = e1; f = e2; in body
    // becomes (for non-recursive let where e2 may reference x):
    //   (λx. (λf. body) e2) e1
    //
    // This ensures e2 is compiled with x in scope, allowing closures.
    // The key insight: binding expressions are compiled INSIDE the lambdas
    // for prior bindings, not outside.

    // Collect bindings in order
    std::vector<std::pair<Symbol, Expr*>> bindings;
    for (auto& [name, def] : *e.attrs->attrs) {
        bindings.push_back({name, def.e});
    }

    // Pre-allocate lambda slots for all bindings
    std::vector<uint32_t> lamLocs;
    for (size_t i = 0; i < bindings.size(); i++) {
        lamLocs.push_back(ctx.runtime().allocateLamSlot());
    }

    // Record starting index for DUP handling
    size_t startBinding = ctx.getBindings().size();

    // First pass: count usages
    // For each binding expression, count with prior bindings in scope
    for (size_t i = 0; i < bindings.size(); i++) {
        countUsages(*bindings[i].second, ctx);
        ctx.pushBinding(bindings[i].first, 0);
    }
    countUsages(*e.body, ctx);

    // Check use counts for all bindings
    std::vector<uint32_t> useCounts;
    for (size_t i = 0; i < bindings.size(); i++) {
        useCounts.push_back(ctx.getBindings()[startBinding + i].useCount);
    }

    // Pop bindings after counting
    for (size_t i = 0; i < bindings.size(); i++) {
        ctx.popBinding();
    }

    // Second pass: emit code
    // Push bindings with heap locations
    for (size_t i = 0; i < bindings.size(); i++) {
        ctx.pushBinding(bindings[i].first, lamLocs[i]);
    }

    // Set use counts for all bindings (needed for emitVar to work correctly)
    // and pre-allocate DUP labels and locations for multi-use bindings
    bool needsDup = false;
    for (size_t i = 0; i < bindings.size(); i++) {
        auto& binding = ctx.getBindings()[startBinding + i];
        // Always set useCount - needed for emitVar to properly handle single-use vs multi-use
        binding.useCount = useCounts[i];
        if (useCounts[i] > 1) {
            needsDup = true;
            uint32_t numDups = useCounts[i] - 1;
            binding.dupLabel = ctx.freshLabels(numDups);
            binding.dupLoc = static_cast<uint32_t>(ctx.allocate(2 * numDups));
            binding.dupIndex = 0;  // Reset for emission
        }
    }

    // Emit body (all bindings in scope)
    Term body = emit(*e.body, ctx);

    // Wrap with DUPs if needed for multi-use variables
    if (needsDup) {
        body = wrapWithDups(body, ctx, startBinding);
    }

    // Build from inside out: each binding i gets wrapped as (λi. inner) ei
    // We emit binding expressions with PRIOR bindings in scope
    for (int i = bindings.size() - 1; i >= 0; i--) {
        // Pop binding i - it's no longer in scope for its own expression
        ctx.popBinding();

        // Finalize the lambda for binding i
        body = ctx.runtime().finalizeLam(lamLocs[i], body);

        // Emit the binding expression with bindings 0..i-1 in scope
        Term val = emit(*bindings[i].second, ctx);

        // Apply the lambda to the binding value
        body = ctx.runtime().termNewApp(body, val);
    }

    return body;
}

// ============================================================================
// With Expressions
// ============================================================================

Term HVM4Compiler::emitWith(const ExprWith& e, CompileContext& ctx) {
    // With expression: with attrs; body
    //
    // We treat the with attrset as a hidden binding that variables from this
    // with will look up from. The approach is similar to emitLet:
    // 1. Count usages of the attrset (one per variable lookup from this with)
    // 2. Set up DUP if the attrset is used multiple times
    // 3. Emit the attrset and body
    // 4. Wrap with DUP if needed

    // Pre-allocate a lambda slot for the with attrset
    uint32_t withLoc = ctx.runtime().allocateLamSlot();

    // Record starting binding index
    size_t startBinding = ctx.getBindings().size();
    size_t withBindingIndex = startBinding;

    // First pass: count usages
    // Create a synthetic symbol for the with attrset binding
    // We use a unique name that won't conflict with user variables
    Symbol withSym = symbols_.create("__with_" + std::to_string(reinterpret_cast<uintptr_t>(&e)));
    ctx.pushBinding(withSym, 0);

    // Register this with expression so variables can find it
    ctx.pushWith(&e, withBindingIndex);

    // Count usages in the body
    // Variables from this with will increment the attrset binding's useCount
    // when they're encountered via lookupWith in countUsages path
    // But actually, countUsages doesn't handle with lookups yet...
    // We need to count how many variables in the body are from this with

    // Walk the body and count variables from this with
    countWithUsages(e, *e.body, ctx);

    uint32_t useCount = ctx.getBindings()[withBindingIndex].useCount;

    // Pop with and binding after counting
    ctx.popWith();
    ctx.popBinding();

    // Second pass: emit code
    ctx.pushBinding(withSym, withLoc);

    auto& withBinding = ctx.getBindings()[withBindingIndex];
    withBinding.useCount = useCount;

    // Set up DUP if the attrset is used multiple times
    if (useCount > 1) {
        uint32_t numDups = useCount - 1;
        withBinding.dupLabel = ctx.freshLabels(numDups);
        withBinding.dupLoc = static_cast<uint32_t>(ctx.allocate(2 * numDups));
        withBinding.dupIndex = 0;
    }

    // Register the with expression
    ctx.pushWith(&e, withBindingIndex);

    // Emit the attrs expression
    Term attrsTerm = emit(*e.attrs, ctx);

    // Emit the body
    Term body = emit(*e.body, ctx);

    // Pop the with and binding
    ctx.popWith();
    ctx.popBinding();

    // If the attrset is used multiple times, wrap with DUP chain
    if (useCount > 1) {
        uint32_t numDups = useCount - 1;
        Term current = body;

        // Build DUP chain from innermost to outermost
        for (int32_t i = static_cast<int32_t>(numDups) - 1; i >= 0; i--) {
            uint32_t dupLocForI = withBinding.dupLoc + 2 * i;
            Term val;
            if (i == 0) {
                // First (outermost) DUP duplicates the original variable
                val = HVM4Runtime::termNewVar(withLoc);
            } else {
                // Subsequent DUPs duplicate the CO1 of the previous DUP
                val = HVM4Runtime::termNewCo1(withBinding.dupLabel + i - 1, withBinding.dupLoc + 2 * (i - 1));
            }
            current = ctx.runtime().termNewDupAt(withBinding.dupLabel + i, dupLocForI, val, current);
        }

        // Wrap in a let for the attrset: (λ. current) attrsTerm
        Term lam = ctx.runtime().finalizeLam(withLoc, current);
        return ctx.runtime().termNewApp(lam, attrsTerm);
    } else if (useCount == 1) {
        // Single use, just a simple let: (λ. body) attrsTerm
        Term lam = ctx.runtime().finalizeLam(withLoc, body);
        return ctx.runtime().termNewApp(lam, attrsTerm);
    } else {
        // Attrset not used, just return the body (dead code elimination)
        return body;
    }
}

// ============================================================================
// Assert Expressions
// ============================================================================

Term HVM4Compiler::emitAssert(const ExprAssert& e, CompileContext& ctx) {
    // Compile condition and body
    Term cond = emit(*e.cond, ctx);
    Term body = emit(*e.body, ctx);

    // Assert: if condition is true, return body; otherwise error (ERA)
    //
    // Using the same pattern as emitIf:
    //   (SWI 0 ERA (λ_. body)) cond
    // if cond == 0 (false): return ERA (assertion failed)
    // if cond != 0 (true): return (λ_. body) cond = body
    //
    // Note: In proper Nix, assertion failure throws an error with position info.
    // For HVM4, we use ERA (erasure) which propagates as undefined behavior.

    Term era = HVM4Runtime::termNewEra();
    Term matcher = ctx.runtime().termNewMat(0, era, ctx.runtime().termNewLam(body));
    return ctx.runtime().termNewApp(matcher, cond);
}

}  // namespace nix::hvm4
