/**
 * HVM4 Compiler Implementation - Core Entry Points
 *
 * This file contains the core entry points for the HVM4 compiler:
 * - HVM4Compiler constructor
 * - compile(): Main compilation entry point
 * - canCompile(): Capability checking entry point
 * - emit(): Main dispatch for code generation
 *
 * The actual implementation is split across multiple files:
 * - hvm4-compile-context.cc: CompileContext implementation
 * - hvm4-compiler-analysis.cc: canCompileWithScope, countUsages, dependency analysis
 * - hvm4-compiler-emit-basic.cc: Basic expression emitters (Int, String, Path, Var, List)
 * - hvm4-compiler-emit-lambda.cc: Lambda emitters
 * - hvm4-compiler-emit-control.cc: Control flow emitters (Call, If, Let, With)
 * - hvm4-compiler-emit-ops.cc: Operator emitters
 * - hvm4-compiler-emit-attrs.cc: Attribute set emitters
 *
 * @see hvm4-compiler.hh for the class and structure definitions
 */

#include "nix/expr/hvm4/hvm4-compiler.hh"
#include "nix/expr/hvm4/hvm4-bigint.hh"

namespace nix::hvm4 {

// ============================================================================
// HVM4Compiler Implementation
// ============================================================================

HVM4Compiler::HVM4Compiler(HVM4Runtime& runtime, SymbolTable& symbols, StringTable& stringTable, AccessorRegistry& accessorRegistry)
    : runtime_(runtime)
    , symbols_(symbols)
    , stringTable_(stringTable)
    , accessorRegistry_(accessorRegistry)
    // Look up builtin constant symbols at runtime
    // These are in the SymbolTable as they're added by primops.cc
    , sTrue_(symbols.create("true"))
    , sFalse_(symbols.create("false"))
    , sNull_(symbols.create("null"))
{}

std::optional<uint32_t> HVM4Compiler::getArithmeticPrimopOpcode(Symbol sym) const {
    // Check against pre-defined arithmetic primop symbols
    // These are used when Nix desugars operators like -, *, /, < to primop calls
    if (sym == astSymbols_.sub) {
        return HVM4Runtime::OP_SUB;
    } else if (sym == astSymbols_.mul) {
        return HVM4Runtime::OP_MUL;
    } else if (sym == astSymbols_.div) {
        return HVM4Runtime::OP_DIV;
    } else if (sym == astSymbols_.lessThan) {
        return HVM4Runtime::OP_LT;
    }
    return std::nullopt;
}

std::optional<Term> HVM4Compiler::getBuiltinConstant(Symbol sym) const {
    // Check for builtin constants: true, false, null
    if (sym == sTrue_) {
        return HVM4Runtime::termNewNum(1);
    } else if (sym == sFalse_) {
        return HVM4Runtime::termNewNum(0);
    } else if (sym == sNull_) {
        // null is represented as a constructor #Nul{} instead of ERA
        // because ERA gets absorbed by operations (any op with ERA returns ERA)
        // Using a constructor allows null comparisons to work correctly
        return runtime_.termNewCtr(NIX_NULL, 0, nullptr);
    }
    return std::nullopt;
}

// ============================================================================
// Main Entry Points
// ============================================================================

Term HVM4Compiler::compile(const Expr& expr) {
    // Create context for usage counting pass
    CompileContext countCtx(runtime_, symbols_);
    countUsages(expr, countCtx);

    // Create fresh context for emission pass
    // We need to preserve the use counts from the first pass
    CompileContext emitCtx(runtime_, symbols_);

    // Copy the bindings with use counts from count pass
    // Actually, we can just do both passes with fresh contexts
    // since the emit pass will re-push bindings

    return emit(expr, emitCtx);
}

bool HVM4Compiler::canCompile(const Expr& expr) const {
    std::vector<Symbol> scope;
    return canCompileWithScope(expr, scope);
}

// ============================================================================
// Main Emission Dispatch
// ============================================================================

Term HVM4Compiler::emit(const Expr& expr, CompileContext& ctx) {
    if (auto* e = dynamic_cast<const ExprInt*>(&expr)) {
        return emitInt(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprFloat*>(&expr)) {
        return emitFloat(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprString*>(&expr)) {
        return emitString(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprPath*>(&expr)) {
        return emitPath(*e, ctx);
    }

    // Check for ExprInheritFrom before ExprVar since it's a subclass
    if (auto* e = dynamic_cast<const ExprInheritFrom*>(&expr)) {
        // ExprInheritFrom references an expression in the current inherit-from context
        // Get the pre-compiled term from the context using the displacement
        return ctx.getInheritFromExpr(e->displ);
    }

    if (auto* e = dynamic_cast<const ExprVar*>(&expr)) {
        return emitVar(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprLambda*>(&expr)) {
        return emitLambda(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprCall*>(&expr)) {
        return emitCall(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprIf*>(&expr)) {
        return emitIf(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprLet*>(&expr)) {
        return emitLet(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprOpNot*>(&expr)) {
        return emitOpNot(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprOpAnd*>(&expr)) {
        return emitOpAnd(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprOpOr*>(&expr)) {
        return emitOpOr(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprOpImpl*>(&expr)) {
        return emitOpImpl(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprAssert*>(&expr)) {
        return emitAssert(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprOpEq*>(&expr)) {
        return emitOpEq(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprOpNEq*>(&expr)) {
        return emitOpNEq(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprConcatStrings*>(&expr)) {
        return emitConcatStrings(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprList*>(&expr)) {
        return emitList(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprAttrs*>(&expr)) {
        return emitAttrs(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprSelect*>(&expr)) {
        return emitSelect(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprOpHasAttr*>(&expr)) {
        return emitOpHasAttr(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprOpUpdate*>(&expr)) {
        return emitOpUpdate(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprOpConcatLists*>(&expr)) {
        return emitOpConcatLists(*e, ctx);
    }

    if (auto* e = dynamic_cast<const ExprWith*>(&expr)) {
        return emitWith(*e, ctx);
    }

    throw HVM4Error("Unsupported expression type for HVM4 backend");
}

}  // namespace nix::hvm4
