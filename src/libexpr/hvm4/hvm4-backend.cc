/**
 * HVM4 Backend Integration Implementation
 *
 * This file provides the main entry point for using HVM4 as an alternative
 * evaluator backend for Nix expressions. It coordinates the compiler, runtime,
 * and result extractor to evaluate expressions using HVM4's optimal reduction.
 *
 * The backend is designed to be non-invasive: it attempts evaluation and falls
 * back to the standard evaluator if HVM4 cannot handle the expression.
 *
 * @see hvm4-compiler.cc for expression compilation
 * @see hvm4-runtime.cc for evaluation
 * @see hvm4-result.cc for result extraction
 */

#include "nix/expr/hvm4/hvm4-backend.hh"
#include "nix/util/logging.hh"

namespace nix::hvm4 {

HVM4Backend::HVM4Backend(EvalState& state, size_t heapSize)
    : state_(state)
    , runtime_(heapSize)
{
    ensureInitialized();
}

HVM4Backend::~HVM4Backend() = default;

void HVM4Backend::ensureInitialized() {
    if (initialized_) return;

    debug("HVM4: initializing backend with %zu byte heap", runtime_.getHeapSize());
    compiler_ = std::make_unique<HVM4Compiler>(runtime_, state_.symbols, stringTable_, accessorRegistry_);
    extractor_ = std::make_unique<ResultExtractor>(state_, runtime_, stringTable_, accessorRegistry_);
    initialized_ = true;
}

bool HVM4Backend::canEvaluate(const Expr& expr) const {
    if (!initialized_ || !compiler_) return false;
    return compiler_->canCompile(expr);
}

bool HVM4Backend::tryEvaluate(Expr* expr, Env& env, Value& result) {
    if (!expr) return false;

    ensureInitialized();

    // Check if we can compile this expression
    if (!compiler_->canCompile(*expr)) {
        debug("HVM4: cannot compile expression, falling back to standard evaluator");
        stats_.fallbacks++;
        return false;
    }

    try {
        // Reset the runtime for a fresh evaluation
        runtime_.reset();

        // Compile the expression to HVM4
        debug("HVM4: compiling expression");
        Term term = compiler_->compile(*expr);
        stats_.compilations++;
        debug("HVM4: compiled to term 0x%016lx (tag=%d, ext=0x%x, val=%u)",
              term, HVM4Runtime::termTag(term), HVM4Runtime::termExt(term),
              HVM4Runtime::termVal(term));

        // Evaluate to strong normal form
        debug("HVM4: evaluating to normal form");
        Term normalForm = runtime_.evaluateSNF(term);
        debug("HVM4: evaluation complete after %lu interactions, result=0x%016lx (tag=%d, ext=0x%x, val=%u)",
              runtime_.getInteractionCount(), normalForm,
              HVM4Runtime::termTag(normalForm), HVM4Runtime::termExt(normalForm),
              HVM4Runtime::termVal(normalForm));

        // Check if we can extract the result
        if (!extractor_->canExtract(normalForm)) {
            debug("HVM4: cannot extract result (term=0x%016lx tag=%u, ext=0x%x), falling back",
                  normalForm, (unsigned)HVM4Runtime::termTag(normalForm), HVM4Runtime::termExt(normalForm));
            stats_.fallbacks++;
            return false;
        }

        // Extract the result
        extractor_->extract(normalForm, result);
        debug("HVM4: extracted result successfully");

        // Update statistics
        stats_.evaluations++;
        stats_.totalInteractions += runtime_.getInteractionCount();
        stats_.totalBytes += runtime_.getAllocatedBytes();

        debug("HVM4: evaluation #%lu complete (total interactions: %lu, bytes: %lu)",
              stats_.evaluations, stats_.totalInteractions, stats_.totalBytes);

        return true;

    } catch (const HVM4Error& e) {
        // Compilation or evaluation failed, fall back
        debug("HVM4: error during evaluation: %s", e.what());
        stats_.fallbacks++;
        return false;

    } catch (const std::exception& e) {
        // Unexpected error, fall back
        debug("HVM4: unexpected error: %s", e.what());
        stats_.fallbacks++;
        return false;
    }
}

void HVM4Backend::reset() {
    runtime_.reset();
}

}  // namespace nix::hvm4
