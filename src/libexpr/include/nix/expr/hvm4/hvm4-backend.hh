#pragma once

/**
 * HVM4 Backend Integration
 *
 * This module provides the main entry point for using HVM4 as an
 * alternative evaluator backend for Nix expressions.
 *
 * The HVM4 backend can accelerate certain pure computational patterns
 * by using HVM4's optimal lambda calculus evaluation.
 *
 * Usage:
 *   HVM4Backend backend(state);
 *   if (backend.canEvaluate(expr)) {
 *       if (backend.tryEvaluate(expr, env, result)) {
 *           // Successfully evaluated with HVM4
 *       }
 *   }
 */

#include "nix/expr/hvm4/hvm4-runtime.hh"
#include "nix/expr/hvm4/hvm4-compiler.hh"
#include "nix/expr/hvm4/hvm4-result.hh"
#include "nix/expr/hvm4/hvm4-string.hh"
#include "nix/expr/hvm4/hvm4-path.hh"
#include "nix/expr/eval.hh"

#include <memory>

namespace nix::hvm4 {

/**
 * HVM4 Backend for Nix evaluation.
 *
 * Manages the HVM4 runtime, compiler, and result extractor,
 * providing a unified interface for evaluating expressions.
 */
class HVM4Backend {
public:
    /**
     * Create an HVM4 backend.
     *
     * @param state The Nix evaluation state
     * @param heapSize Size of the HVM4 heap (default 64M terms)
     */
    explicit HVM4Backend(
        EvalState& state,
        size_t heapSize = 1ULL << 26
    );

    ~HVM4Backend();

    // Non-copyable
    HVM4Backend(const HVM4Backend&) = delete;
    HVM4Backend& operator=(const HVM4Backend&) = delete;

    /**
     * Check if an expression can be evaluated by HVM4.
     *
     * Returns true if the expression uses only supported constructs:
     * - Integer literals
     * - Variables
     * - Simple lambdas (not pattern-matching)
     * - Function application
     * - Non-recursive let bindings
     * - If-then-else
     * - Boolean and comparison operations
     * - Integer addition
     *
     * @param expr The expression to check
     * @return true if the expression can be evaluated by HVM4
     */
    bool canEvaluate(const Expr& expr) const;

    /**
     * Try to evaluate an expression using HVM4.
     *
     * If successful, the result is stored in the provided Value.
     * If evaluation fails for any reason, returns false and the
     * caller should fall back to the standard evaluator.
     *
     * @param expr The expression to evaluate
     * @param env The environment (currently not used - expressions must be closed)
     * @param result The Value to store the result in
     * @return true if evaluation succeeded, false to fall back
     */
    bool tryEvaluate(Expr* expr, Env& env, Value& result);

    /**
     * Get statistics about HVM4 evaluation.
     */
    struct Stats {
        uint64_t compilations = 0;     // Number of expressions compiled
        uint64_t evaluations = 0;      // Number of successful evaluations
        uint64_t fallbacks = 0;        // Number of fallbacks to standard eval
        uint64_t totalInteractions = 0; // Total HVM4 reduction interactions
        uint64_t totalBytes = 0;       // Total heap bytes used
    };

    const Stats& getStats() const { return stats_; }

    /**
     * Reset the runtime for a fresh evaluation.
     * Called automatically between evaluations.
     */
    void reset();

    /**
     * Get the string table for string interning.
     */
    StringTable& getStringTable() { return stringTable_; }
    const StringTable& getStringTable() const { return stringTable_; }

    /**
     * Get the accessor registry for path handling.
     */
    AccessorRegistry& getAccessorRegistry() { return accessorRegistry_; }
    const AccessorRegistry& getAccessorRegistry() const { return accessorRegistry_; }

private:
    EvalState& state_;
    HVM4Runtime runtime_;
    StringTable stringTable_;  // Persists across evaluations for string interning
    AccessorRegistry accessorRegistry_;  // Persists across evaluations for path accessor IDs
    std::unique_ptr<HVM4Compiler> compiler_;
    std::unique_ptr<ResultExtractor> extractor_;
    Stats stats_;

    bool initialized_ = false;
    void ensureInitialized();
};

}  // namespace nix::hvm4
