#pragma once

/**
 * HVM4 Compiler for Nix Expressions
 *
 * This module compiles Nix AST expressions to HVM4 terms.
 *
 * Key design decisions:
 *
 * 1. Variable Usage: Nix allows unlimited variable usage, but HVM4 uses
 *    affine (single-use) variables. We use a two-pass approach:
 *    - Pass 1: Count variable usages
 *    - Pass 2: Emit code with DUP nodes for multi-use variables
 *
 * 2. VAR References: HVM4 VAR terms use heap locations, NOT de Bruijn
 *    indices. When constructing a lambda, we pre-allocate a heap slot
 *    (via allocateLamSlot), then build the body with VAR(heapLoc)
 *    references, and finally finalize the lambda (via finalizeLam).
 *
 * 3. Addition Operator: In Nix, `+` is handled by ExprConcatStrings
 *    when forceString=false and operands are numeric.
 *
 * 4. Arithmetic Operators: In Nix, `-`, `*`, `/`, `<` are desugared to
 *    primop calls (e.g., `5 - 3` becomes `__sub 5 3`). The compiler
 *    detects these patterns in ExprCall and emits HVM4 OP2 terms.
 *
 * 5. Scope Checking: The canCompile function tracks variable scope to
 *    properly reject expressions with free variables (like builtins
 *    `true`, `false`, `sub`, etc.) unless they are handled specially.
 *
 * 6. Builtin Constants: `true`, `false`, and `null` are handled as
 *    special constants when their symbols are detected.
 *
 * Known limitations:
 * - Closures are not fully supported: lambdas that capture outer
 *   variables from let bindings may not evaluate correctly
 * - Functions stored in let bindings and called multiple times
 *   require proper DUP handling of lambda values
 */

#include "nix/expr/hvm4/hvm4-runtime.hh"
#include "nix/expr/hvm4/hvm4-string.hh"
#include "nix/expr/hvm4/hvm4-path.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/eval.hh"

#include <vector>
#include <optional>
#include <unordered_map>
#include <set>
#include <map>

namespace nix::hvm4 {

/**
 * Tracks information about a variable binding during compilation.
 *
 * For multi-use variables (useCount > 1), we need DUP nodes:
 * - dupLabel: Base label for the DUP chain
 * - dupLoc: Heap location of the first DUP (for CO0/CO1 references)
 * - dupIndex: Tracks which use we're currently emitting
 *
 * For N uses, we need N-1 DUPs. Uses map to projections:
 * - Use 0 -> CO0 of DUP 0
 * - Use 1 -> CO0 of DUP 1
 * - ...
 * - Use N-2 -> CO0 of DUP N-2
 * - Use N-1 -> CO1 of DUP N-2
 */
struct VarBinding {
    Symbol name;              // Variable name
    uint32_t depth;           // Binding depth (for tracking)
    uint32_t heapLoc = 0;     // Heap location where substitution happens
    uint32_t useCount = 0;    // Number of times referenced
    uint32_t dupLabel = 0;    // Base label for DUP chain (if useCount > 1)
    uint32_t dupLoc = 0;      // Heap location of first DUP
    mutable uint32_t dupIndex = 0;    // Current use index (0 to useCount-1)
};

/**
 * Tracks a with expression during compilation.
 * The attrset is stored in a binding for proper DUP handling.
 */
struct WithBinding {
    const ExprWith* expr;      // The ExprWith node
    size_t bindingIndex;       // Index into the bindings vector
};

/**
 * Tracks inherit-from expressions during attr set compilation.
 * For `inherit (s) a b;`, the expression `s` is stored in inheritFromExprs
 * and accessed via ExprInheritFrom which has a displacement index.
 * Each expression may be used multiple times, requiring DUP handling.
 */
struct InheritFromEntry {
    Term compiledExpr;     // Pre-compiled inherit-from expression
    uint32_t useCount;     // How many attributes reference this
    uint32_t dupLabel;     // Base label for DUP chain (if useCount > 1)
    uint32_t dupLoc;       // Heap location of first DUP
    mutable uint32_t useIndex;  // Current use index
};

struct InheritFromContext {
    std::vector<InheritFromEntry> entries;
};

/**
 * Compilation context maintaining state during compilation.
 */
class CompileContext {
public:
    explicit CompileContext(HVM4Runtime& runtime, const SymbolTable& symbols);

    // Binding management
    void pushBinding(Symbol name, uint32_t heapLoc = 0);
    void popBinding();
    VarBinding* lookup(Symbol name);
    const VarBinding* lookup(Symbol name) const;

    // Check if a binding exists
    bool hasBinding(Symbol name) const;

    // Get all bindings (for DUP insertion)
    std::vector<VarBinding>& getBindings() { return bindings_; }
    const std::vector<VarBinding>& getBindings() const { return bindings_; }

    // With expression management
    void pushWith(const ExprWith* expr, size_t bindingIndex);
    void popWith();
    const WithBinding* lookupWith(const ExprWith* expr) const;
    std::vector<WithBinding>& getWithStack() { return withStack_; }
    const std::vector<WithBinding>& getWithStack() const { return withStack_; }

    // Inherit-from expression management
    void pushInheritFrom(InheritFromContext ctx);
    void popInheritFrom();
    Term getInheritFromExpr(size_t displ) const;
    bool hasInheritFrom() const { return !inheritFromStack_.empty(); }

    // Depth tracking
    uint32_t currentDepth() const { return depth_; }
    void incrementDepth() { depth_++; }
    void decrementDepth() { if (depth_ > 0) depth_--; }

    // Fresh label generation for DUP nodes
    uint32_t freshLabel();
    uint32_t freshLabels(uint32_t count);

    // Memory allocation delegated to runtime
    uint64_t allocate(uint64_t size);
    Term* heap() { return runtime_.getHeap(); }

    // Symbol table access
    const SymbolTable& symbols() const { return symbols_; }

    // Runtime access
    HVM4Runtime& runtime() { return runtime_; }

private:
    HVM4Runtime& runtime_;
    const SymbolTable& symbols_;
    std::vector<VarBinding> bindings_;
    std::vector<WithBinding> withStack_;  // Stack of active with expressions
    std::vector<InheritFromContext> inheritFromStack_;  // Stack of inherit-from contexts
    uint32_t depth_ = 0;
    uint32_t labelCounter_ = 0x800000;  // Start high to avoid collisions
};

/**
 * The HVM4 Compiler.
 *
 * Compiles Nix expressions to HVM4 terms using a two-pass approach:
 * 1. First pass counts variable usages
 * 2. Second pass emits code with DUP nodes for multi-use variables
 */
class HVM4Compiler {
public:
    HVM4Compiler(HVM4Runtime& runtime, SymbolTable& symbols, StringTable& stringTable, AccessorRegistry& accessorRegistry);

    /**
     * Main entry point - compiles expression to HVM4 term.
     *
     * @param expr The expression to compile
     * @return The compiled HVM4 term
     * @throws HVM4Error if the expression cannot be compiled
     */
    Term compile(const Expr& expr);

    /**
     * Check if an expression can be compiled to HVM4.
     *
     * Currently supported:
     * - Integer literals
     * - Boolean literals (true, false)
     * - null literal
     * - String literals (constant)
     * - Path literals
     * - Variables
     * - Lambdas (simple and pattern-matching)
     * - Function application
     * - Let expressions (non-recursive and acyclic recursive)
     * - If-then-else
     * - Boolean operations (!, &&, ||)
     * - Comparison (==, !=, <, <=, >, >=)
     * - Arithmetic (via primops: +, -, *, /)
     * - Lists
     * - Attribute sets (non-recursive and acyclic recursive, static keys)
     * - Attribute selection (single-level)
     * - Has-attr operator (single-level)
     * - Attribute update (//)
     * - With expressions
     *
     * Not supported:
     * - Floats
     * - Cyclic recursive let/rec (requires Y-combinator)
     * - Dynamic attribute names
     * - Nested attribute paths in selection
     * - Inherit expressions
     */
    bool canCompile(const Expr& expr) const;

private:
    HVM4Runtime& runtime_;
    SymbolTable& symbols_;
    StringTable& stringTable_;
    AccessorRegistry& accessorRegistry_;

    // Pre-defined symbols for arithmetic primops
    // These are set at compile time via StaticEvalSymbols
    static constexpr Expr::AstSymbols astSymbols_ = StaticEvalSymbols::create().exprSymbols;

    // Symbols for builtin constants (true, false, null)
    // These need to be looked up at runtime since they're not in AstSymbols
    Symbol sTrue_;
    Symbol sFalse_;
    Symbol sNull_;

    /**
     * Check if a symbol is an arithmetic primop (__sub, __mul, __div, __lessThan).
     * @return The HVM4 opcode if it's an arithmetic primop, or std::nullopt
     */
    std::optional<uint32_t> getArithmeticPrimopOpcode(Symbol sym) const;

    /**
     * Check if a symbol is a builtin constant (true, false, null).
     * @return The HVM4 term for the constant, or std::nullopt
     */
    std::optional<Term> getBuiltinConstant(Symbol sym) const;

    // First pass: count variable usages
    void countUsages(const Expr& expr, CompileContext& ctx);

    // Second pass: emit code with auto-dup
    Term emit(const Expr& expr, CompileContext& ctx);

    // Expression-specific emitters
    Term emitInt(const ExprInt& e, CompileContext& ctx);
    Term emitFloat(const ExprFloat& e, CompileContext& ctx);
    Term emitString(const ExprString& e, CompileContext& ctx);
    Term emitVar(const ExprVar& e, CompileContext& ctx);
    Term emitLambda(const ExprLambda& e, CompileContext& ctx);
    Term emitPatternLambda(const ExprLambda& e, CompileContext& ctx);
    Term emitCall(const ExprCall& e, CompileContext& ctx);
    Term emitIf(const ExprIf& e, CompileContext& ctx);
    Term emitLet(const ExprLet& e, CompileContext& ctx);
    Term emitOpNot(const ExprOpNot& e, CompileContext& ctx);
    Term emitOpAnd(const ExprOpAnd& e, CompileContext& ctx);
    Term emitOpOr(const ExprOpOr& e, CompileContext& ctx);
    Term emitOpImpl(const ExprOpImpl& e, CompileContext& ctx);
    Term emitAssert(const ExprAssert& e, CompileContext& ctx);
    Term emitOpEq(const ExprOpEq& e, CompileContext& ctx);
    Term emitOpNEq(const ExprOpNEq& e, CompileContext& ctx);
    Term emitConcatStrings(const ExprConcatStrings& e, CompileContext& ctx);
    Term emitStringConcat(const ExprConcatStrings& e, CompileContext& ctx);
    Term emitList(const ExprList& e, CompileContext& ctx);
    Term emitPath(const ExprPath& e, CompileContext& ctx);

    // Attribute set emitters
    Term emitAttrs(const ExprAttrs& e, CompileContext& ctx);
    Term emitSelect(const ExprSelect& e, CompileContext& ctx);
    Term emitOpHasAttr(const ExprOpHasAttr& e, CompileContext& ctx);
    Term emitOpUpdate(const ExprOpUpdate& e, CompileContext& ctx);

    // List emitters
    Term emitOpConcatLists(const ExprOpConcatLists& e, CompileContext& ctx);

    // Attribute lookup helpers
    Term emitAttrLookup(Term attrs, uint32_t symbolId, CompileContext& ctx);
    Term emitMaybeAttrLookup(Term maybeTerm, uint32_t symbolId, CompileContext& ctx);
    Term emitSpineSearch(Term spine, Term targetKey, CompileContext& ctx);
    Term emitSpineSearchMaybe(Term spine, Term targetKey, CompileContext& ctx);
    Term emitSpineHasAttr(Term spine, Term targetKey, CompileContext& ctx);
    Term emitOpHasAttrInternal(Term attrs, uint32_t symbolId, CompileContext& ctx);

    // With expression emitter
    Term emitWith(const ExprWith& e, CompileContext& ctx);

    // Auto-dup insertion for multi-use variables
    Term wrapWithDups(Term body, CompileContext& ctx, size_t startBinding);

    // Helper to check if ConcatStrings is actually addition
    bool isNumericAddition(const ExprConcatStrings& e) const;

    // Helper to check if expression is a constant string
    bool isConstantString(const Expr* expr) const;

    // Scope-aware canCompile helper
    bool canCompileWithScope(const Expr& expr, std::vector<Symbol>& scope) const;

    // With expression helper - count usages of with attrset
    void countWithUsages(const ExprWith& withExpr, const Expr& expr, CompileContext& ctx);

    // Recursive let helpers
    // Collect variables referenced by an expression that are in the given set
    void collectDependencies(const Expr& expr, const std::set<Symbol>& candidates,
                            std::set<Symbol>& deps) const;

    // Topological sort of bindings. Returns sorted order or std::nullopt if cyclic.
    std::optional<std::vector<Symbol>> topologicalSort(
        const std::map<Symbol, std::set<Symbol>>& deps) const;

    // Emit recursive attr set (acyclic case)
    Term emitRecAttrs(const ExprAttrs& e, CompileContext& ctx);
};

}  // namespace nix::hvm4
