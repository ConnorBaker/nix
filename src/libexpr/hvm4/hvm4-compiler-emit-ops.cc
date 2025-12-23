/**
 * HVM4 Compiler Operator Emitters
 *
 * Contains emitters for operator expressions:
 * - Boolean operators: emitOpNot, emitOpAnd, emitOpOr
 * - Comparison operators: emitOpEq, emitOpNEq
 * - List operators: emitOpConcatLists
 * - Attribute operators: emitOpUpdate
 */

#include "nix/expr/hvm4/hvm4-compiler.hh"
#include "nix/expr/hvm4/hvm4-attrs.hh"
#include "nix/expr/hvm4/hvm4-bigint.hh"
#include "nix/expr/hvm4/hvm4-list.hh"

namespace nix::hvm4 {

// ============================================================================
// Boolean Operators
// ============================================================================

Term HVM4Compiler::emitOpNot(const ExprOpNot& e, CompileContext& ctx) {
    // !x encoded as: if x then 0 else 1
    // Or using OP2: (x == 0) ? 1 : 0
    // Simplest: (x == 0)
    Term x = emit(*e.e, ctx);
    Term zero = HVM4Runtime::termNewNum(0);
    return ctx.runtime().termNewOp2(HVM4Runtime::OP_EQ, x, zero);
}

Term HVM4Compiler::emitOpAnd(const ExprOpAnd& e, CompileContext& ctx) {
    // Short-circuit AND using HVM4's AND operator
    Term a = emit(*e.e1, ctx);
    Term b = emit(*e.e2, ctx);
    return ctx.runtime().termNewAnd(a, b);
}

Term HVM4Compiler::emitOpOr(const ExprOpOr& e, CompileContext& ctx) {
    // Short-circuit OR using HVM4's OR operator
    Term a = emit(*e.e1, ctx);
    Term b = emit(*e.e2, ctx);
    return ctx.runtime().termNewOr(a, b);
}

Term HVM4Compiler::emitOpImpl(const ExprOpImpl& e, CompileContext& ctx) {
    // Implication: a -> b = !a || b = if a then b else true
    // Semantics: if a is false, return true (short-circuit); if a is true, return b
    Term cond = emit(*e.e1, ctx);
    Term thenBranch = emit(*e.e2, ctx);  // Return b if a is true
    Term elseBranch = HVM4Runtime::termNewNum(1);  // Return true if a is false

    // HVM4 conditional: (SWI 0 elseBranch (λ_. thenBranch)) cond
    // if cond == 0 (false): return elseBranch (true)
    // if cond != 0 (true): return thenBranch (b)
    Term matcher = ctx.runtime().termNewMat(0, elseBranch, ctx.runtime().termNewLam(thenBranch));
    return ctx.runtime().termNewApp(matcher, cond);
}

// ============================================================================
// Comparison Operators
// ============================================================================

Term HVM4Compiler::emitOpEq(const ExprOpEq& e, CompileContext& ctx) {
    Term a = emit(*e.e1, ctx);
    Term b = emit(*e.e2, ctx);

    // Emit null-aware equality using MAT pattern matching:
    // MAT(NIX_NULL, if_null_case, if_not_null_case) @ term
    //   - If term is #Nul{}: returns if_null_case
    //   - Otherwise: returns (if_not_null_case term)
    //
    // For a == b, we emit:
    // MAT(NIX_NULL,
    //     MAT(NIX_NULL, 1, λ_. 0) @ b,  // a is null: return (b is null)
    //     λa'. MAT(NIX_NULL, 0, λb'. OP_EQ(a', b')) @ b  // a not null
    // ) @ a

    Term one = HVM4Runtime::termNewNum(1);
    Term zero = HVM4Runtime::termNewNum(0);

    // Case: a is null - check if b is also null
    // MAT(NIX_NULL, 1, λ_. 0) @ b
    Term returnZeroLam = ctx.runtime().termNewLam(zero);
    Term bNullCheck = ctx.runtime().termNewMat(NIX_NULL, one, returnZeroLam);
    Term aIsNullCase = ctx.runtime().termNewApp(bNullCheck, b);

    // Case: a is not null - check if b is null (return 0), else compare
    // λa'. MAT(NIX_NULL, 0, λb'. EQL(a', b')) @ b
    //
    // We need to use lambda-bound variables so substitution works correctly.
    // We use EQL (structural equality) instead of OP_EQ to handle BigInt:
    // - NUM === NUM: compares numerically
    // - CTR === CTR: compares constructor tags and recursively compares fields
    //   (handles #Pos{lo, hi} and #Neg{lo, hi} BigInt representations)
    uint32_t aLamLoc = ctx.runtime().allocateLamSlot();
    uint32_t bLamLoc = ctx.runtime().allocateLamSlot();
    Term aPrime = HVM4Runtime::termNewVar(aLamLoc);
    Term bPrime = HVM4Runtime::termNewVar(bLamLoc);

    // Use EQL for structural equality (handles both NUM and BigInt constructors)
    Term structuralEq = ctx.runtime().termNewEql(aPrime, bPrime);
    Term bLambda = ctx.runtime().finalizeLam(bLamLoc, structuralEq);

    Term bNotNullMat = ctx.runtime().termNewMat(NIX_NULL, zero, bLambda);
    Term bMatApp = ctx.runtime().termNewApp(bNotNullMat, b);
    Term aLambda = ctx.runtime().finalizeLam(aLamLoc, bMatApp);

    // Now build the outer MAT:
    // MAT(NIX_NULL, aIsNullCase, aLambda) @ a
    Term outerMat = ctx.runtime().termNewMat(NIX_NULL, aIsNullCase, aLambda);
    return ctx.runtime().termNewApp(outerMat, a);
}

Term HVM4Compiler::emitOpNEq(const ExprOpNEq& e, CompileContext& ctx) {
    Term a = emit(*e.e1, ctx);
    Term b = emit(*e.e2, ctx);

    // Emit null-aware inequality using MAT pattern matching:
    // For a != b:
    // - null != null -> 0 (false)
    // - null != non-null -> 1 (true)
    // - non-null != null -> 1 (true)
    // - non-null != non-null -> BigInt-aware inequality

    Term one = HVM4Runtime::termNewNum(1);
    Term zero = HVM4Runtime::termNewNum(0);

    // Case: a is null - check if b is also null (return 0) or not (return 1)
    // MAT(NIX_NULL, 0, λ_. 1) @ b
    Term returnOneLam = ctx.runtime().termNewLam(one);
    Term bNullCheck = ctx.runtime().termNewMat(NIX_NULL, zero, returnOneLam);
    Term aIsNullCase = ctx.runtime().termNewApp(bNullCheck, b);

    // Case: a is not null - if b is null return 1, else compare
    // λa'. MAT(NIX_NULL, 1, λb'. 1 - EQL(a', b')) @ b
    //
    // We use EQL (structural equality) instead of OP_NE to handle BigInt:
    // - EQL returns 1 for equal, 0 for not equal
    // - We invert with 1 - EQL to get: 0 for equal, 1 for not equal
    uint32_t aLamLoc = ctx.runtime().allocateLamSlot();
    uint32_t bLamLoc = ctx.runtime().allocateLamSlot();
    Term aPrime = HVM4Runtime::termNewVar(aLamLoc);
    Term bPrime = HVM4Runtime::termNewVar(bLamLoc);

    // Use EQL for structural equality, then invert: 1 - EQL(a, b)
    Term structuralEq = ctx.runtime().termNewEql(aPrime, bPrime);
    Term structuralNe = ctx.runtime().termNewOp2(HVM4Runtime::OP_SUB, one, structuralEq);
    Term bLambda = ctx.runtime().finalizeLam(bLamLoc, structuralNe);

    Term bNotNullMat = ctx.runtime().termNewMat(NIX_NULL, one, bLambda);
    Term bMatApp = ctx.runtime().termNewApp(bNotNullMat, b);
    Term aLambda = ctx.runtime().finalizeLam(aLamLoc, bMatApp);

    Term outerMat = ctx.runtime().termNewMat(NIX_NULL, aIsNullCase, aLambda);
    return ctx.runtime().termNewApp(outerMat, a);
}

// ============================================================================
// List Operators
// ============================================================================

Term HVM4Compiler::emitOpConcatLists(const ExprOpConcatLists& e, CompileContext& ctx) {
    // Compile list concatenation: list1 ++ list2
    // Currently only supported for direct list literals (checked in canCompileWithScope)

    Term list1 = emit(*e.e1, ctx);
    Term list2 = emit(*e.e2, ctx);

    // Concatenate the two lists at compile time
    // This works because canCompileWithScope ensures both operands are ExprList
    return concatLists(list1, list2, ctx.runtime());
}

// ============================================================================
// Attribute Update Operator
// ============================================================================

Term HVM4Compiler::emitOpUpdate(const ExprOpUpdate& e, CompileContext& ctx) {
    // Compile attribute update: attrs1 // attrs2
    // Creates a layered attribute set

    Term base = emit(*e.e1, ctx);
    Term overlay = emit(*e.e2, ctx);

    // Merge the two attribute sets
    // mergeAttrs unwraps, merges spines, and rewraps with #Ats{}
    return mergeAttrs(base, overlay, ctx.runtime());
}

}  // namespace nix::hvm4
