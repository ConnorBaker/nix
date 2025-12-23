/**
 * HVM4 Compiler Lambda Emitters
 *
 * Contains emitters for lambda expressions:
 * - emitLambda: Simple lambdas (x: body)
 * - emitPatternLambda: Pattern-matching lambdas ({ a, b ? 1, ... } @ args: body)
 */

#include "nix/expr/hvm4/hvm4-compiler.hh"

namespace nix::hvm4 {

// ============================================================================
// Lambda Emitters
// ============================================================================

Term HVM4Compiler::emitLambda(const ExprLambda& e, CompileContext& ctx) {
    // Dispatch to pattern lambda handler if formals are present
    if (e.getFormals()) {
        return emitPatternLambda(e, ctx);
    }

    // Pre-allocate the lambda slot - we need the heap location for VAR references
    uint32_t lamLoc = ctx.runtime().allocateLamSlot();

    // Record starting binding index for DUP insertion
    size_t startBinding = ctx.getBindings().size();

    // First pass: count usages in the body (with heapLoc=0 since we're just counting)
    ctx.pushBinding(e.arg, 0);
    countUsages(*e.body, ctx);
    uint32_t useCount = ctx.getBindings().back().useCount;
    ctx.popBinding();

    // Second pass: emit body with the actual heap location
    ctx.pushBinding(e.arg, lamLoc);

    // Pre-allocate DUP labels and locations for multi-use variables
    auto& binding = ctx.getBindings().back();
    if (useCount > 1) {
        uint32_t numDups = useCount - 1;
        binding.useCount = useCount;
        binding.dupLabel = ctx.freshLabels(numDups);
        // Allocate 2 slots per DUP for CO0/CO1 storage
        binding.dupLoc = static_cast<uint32_t>(ctx.allocate(2 * numDups));
        binding.dupIndex = 0;  // Reset for emission
    }

    Term body = emit(*e.body, ctx);

    // If argument is used multiple times, wrap with DUPs
    if (useCount > 1) {
        body = wrapWithDups(body, ctx, startBinding);
    }

    ctx.popBinding();

    // Finalize the lambda with its body
    return ctx.runtime().finalizeLam(lamLoc, body);
}

Term HVM4Compiler::emitPatternLambda(const ExprLambda& e, CompileContext& ctx) {
    // Pattern-matching lambda: { a, b ? 1, ... } @ args: body
    // Desugars to: __arg: let a = __arg.a; b = if __arg ? b then __arg.b else 1; args = __arg; in body
    //
    // We implement this by:
    // 1. Creating an outer lambda that takes the attr set argument
    // 2. For each formal, creating a binding (via nested lambdas like emitLet)
    // 3. Emitting the body with all formals in scope

    auto formals = e.getFormals();
    if (!formals) {
        throw HVM4Error("emitPatternLambda called without formals");
    }

    // Pre-allocate the outer lambda slot for __arg
    uint32_t argLamLoc = ctx.runtime().allocateLamSlot();

    // Record starting binding index for DUP insertion
    size_t startBinding = ctx.getBindings().size();

    // First pass: count usages
    // Push __arg binding (with heapLoc=0 since we're just counting)
    ctx.pushBinding(symbols_.create("__arg"), 0);

    // Push formal bindings FIRST - defaults can reference other formals
    // In Nix, { a, b ? a * 2 }: ... is valid and a is in scope for b's default
    std::vector<uint32_t> formalUseCounts;
    for (const auto& formal : formals->formals) {
        ctx.pushBinding(formal.name, 0);
    }

    // Also push @-pattern binding if present
    if (e.arg) {
        ctx.pushBinding(e.arg, 0);
    }

    // Count usages in default expressions (with all formals in scope)
    for (const auto& formal : formals->formals) {
        if (formal.def) {
            countUsages(*formal.def, ctx);
        }
    }

    countUsages(*e.body, ctx);

    // Record use counts
    if (e.arg) {
        formalUseCounts.push_back(ctx.getBindings().back().useCount);
        ctx.popBinding();
    }
    for (int i = formals->formals.size() - 1; i >= 0; i--) {
        formalUseCounts.insert(formalUseCounts.begin(),
            ctx.getBindings()[startBinding + 1 + i].useCount);
        ctx.popBinding();
    }

    // Get __arg use count before popping (unused - we recalculate based on accesses)
    [[maybe_unused]] uint32_t argUseCount = ctx.getBindings()[startBinding].useCount;
    ctx.popBinding();

    // Second pass: emit code
    // Push __arg with actual heap location
    ctx.pushBinding(symbols_.create("__arg"), argLamLoc);

    // Set up DUP for __arg if used multiple times (very common in pattern lambdas)
    auto& argBinding = ctx.getBindings().back();
    // __arg will be referenced once per formal without default (for selection),
    // twice per formal with default (has-attr + selection), plus @-pattern
    // We need to count how many times __arg is actually accessed
    uint32_t argAccessCount = 0;
    for (const auto& formal : formals->formals) {
        if (formal.def) {
            argAccessCount += 2;  // has-attr + selection
        } else {
            argAccessCount += 1;  // just selection
        }
    }
    if (e.arg) {
        argAccessCount += 1;  // @-pattern binding
    }

    if (argAccessCount > 1) {
        argBinding.useCount = argAccessCount;
        uint32_t numDups = argAccessCount - 1;
        argBinding.dupLabel = ctx.freshLabels(numDups);
        argBinding.dupLoc = static_cast<uint32_t>(ctx.allocate(2 * numDups));
        argBinding.dupIndex = 0;
    }

    // Now emit the formal bindings using nested lambdas (like emitLet)
    // Pre-allocate lambda slots for all formals
    std::vector<uint32_t> formalLamLocs;
    for (size_t i = 0; i < formals->formals.size(); i++) {
        formalLamLocs.push_back(ctx.runtime().allocateLamSlot());
    }

    // Also allocate for @-pattern if present
    uint32_t argPatternLamLoc = 0;
    if (e.arg) {
        argPatternLamLoc = ctx.runtime().allocateLamSlot();
    }

    // Push all formal bindings with heap locations
    for (size_t i = 0; i < formals->formals.size(); i++) {
        ctx.pushBinding(formals->formals[i].name, formalLamLocs[i]);
    }
    if (e.arg) {
        ctx.pushBinding(e.arg, argPatternLamLoc);
    }

    // Set use counts and allocate DUP structures for multi-use formals
    size_t formalStartBinding = startBinding + 1;  // Skip __arg
    bool needsDup = false;
    size_t useCountIdx = 0;
    for (size_t i = 0; i < formals->formals.size(); i++) {
        auto& binding = ctx.getBindings()[formalStartBinding + i];
        binding.useCount = formalUseCounts[useCountIdx++];
        if (binding.useCount > 1) {
            needsDup = true;
            uint32_t numDups = binding.useCount - 1;
            binding.dupLabel = ctx.freshLabels(numDups);
            binding.dupLoc = static_cast<uint32_t>(ctx.allocate(2 * numDups));
            binding.dupIndex = 0;
        }
    }
    if (e.arg) {
        auto& binding = ctx.getBindings().back();
        binding.useCount = formalUseCounts[useCountIdx];
        if (binding.useCount > 1) {
            needsDup = true;
            uint32_t numDups = binding.useCount - 1;
            binding.dupLabel = ctx.freshLabels(numDups);
            binding.dupLoc = static_cast<uint32_t>(ctx.allocate(2 * numDups));
            binding.dupIndex = 0;
        }
    }

    // Emit the body
    Term body = emit(*e.body, ctx);

    // Wrap with DUPs if needed
    if (needsDup) {
        body = wrapWithDups(body, ctx, formalStartBinding);
    }

    // Build from inside out: wrap body with lambda for each formal
    // For @-pattern first (innermost) - we pop and finalize, then apply later
    if (e.arg) {
        ctx.popBinding();
        body = ctx.runtime().finalizeLam(argPatternLamLoc, body);
    }

    // Now wrap with formal bindings
    for (int i = formals->formals.size() - 1; i >= 0; i--) {
        ctx.popBinding();

        // Finalize lambda for this formal
        body = ctx.runtime().finalizeLam(formalLamLocs[i], body);

        // Emit the value for this formal
        const auto& formal = formals->formals[i];
        Term formalValue;

        // Get __arg reference for attribute access
        Term argRef = HVM4Runtime::termNewVar(argLamLoc);
        // If __arg is multi-use, we need to use CO0/CO1 projections
        if (argBinding.useCount > 1) {
            uint32_t idx = argBinding.dupIndex++;
            uint32_t numDups = argBinding.useCount - 1;
            if (idx < numDups) {
                uint32_t dupLoc = argBinding.dupLoc + 2 * idx;
                argRef = HVM4Runtime::termNewCo0(argBinding.dupLabel + idx, dupLoc);
            } else {
                uint32_t dupLoc = argBinding.dupLoc + 2 * (numDups - 1);
                argRef = HVM4Runtime::termNewCo1(argBinding.dupLabel + numDups - 1, dupLoc);
            }
        }

        if (formal.def) {
            // Has default: if __arg ? name then __arg.name else default
            // We need another __arg reference for the has-attr check
            Term argRefForHasAttr = HVM4Runtime::termNewVar(argLamLoc);
            if (argBinding.useCount > 1) {
                uint32_t idx = argBinding.dupIndex++;
                uint32_t numDups = argBinding.useCount - 1;
                if (idx < numDups) {
                    uint32_t dupLoc = argBinding.dupLoc + 2 * idx;
                    argRefForHasAttr = HVM4Runtime::termNewCo0(argBinding.dupLabel + idx, dupLoc);
                } else {
                    uint32_t dupLoc = argBinding.dupLoc + 2 * (numDups - 1);
                    argRefForHasAttr = HVM4Runtime::termNewCo1(argBinding.dupLabel + numDups - 1, dupLoc);
                }
            }

            // Emit has-attr check using the internal helper
            // This handles the #Ats{} unwrapping via MAT
            Term hasAttrResult = emitOpHasAttrInternal(argRefForHasAttr, formal.name.getId(), ctx);

            // Emit selection (__arg.name)
            uint32_t symbolId = formal.name.getId();
            Term selectResult = emitAttrLookup(argRef, symbolId, ctx);

            // Emit default value
            Term defaultValue = emit(*formal.def, ctx);

            // Build conditional: if hasAttr then selectResult else defaultValue
            // MAT(0, defaultValue, λ_. selectResult) hasAttrResult
            Term returnSelectLam = ctx.runtime().termNewLam(selectResult);
            Term condMat = ctx.runtime().termNewMat(0, defaultValue, returnSelectLam);
            formalValue = ctx.runtime().termNewApp(condMat, hasAttrResult);
        } else {
            // No default: just emit selection (__arg.name)
            uint32_t symbolId = formal.name.getId();
            formalValue = emitAttrLookup(argRef, symbolId, ctx);
        }

        // Apply lambda to value
        body = ctx.runtime().termNewApp(body, formalValue);
    }

    // Handle @-pattern binding application
    if (e.arg) {
        // @-pattern value is just __arg
        Term argRef = HVM4Runtime::termNewVar(argLamLoc);
        if (argBinding.useCount > 1) {
            uint32_t idx = argBinding.dupIndex++;
            uint32_t numDups = argBinding.useCount - 1;
            if (idx < numDups) {
                uint32_t dupLoc = argBinding.dupLoc + 2 * idx;
                argRef = HVM4Runtime::termNewCo0(argBinding.dupLabel + idx, dupLoc);
            } else {
                uint32_t dupLoc = argBinding.dupLoc + 2 * (numDups - 1);
                argRef = HVM4Runtime::termNewCo1(argBinding.dupLabel + numDups - 1, dupLoc);
            }
        }
        body = ctx.runtime().termNewApp(body, argRef);
    }

    // Wrap with DUPs for __arg if needed
    if (argBinding.useCount > 1) {
        Term result = body;
        uint32_t numDups = argBinding.useCount - 1;

        for (int j = numDups - 1; j >= 0; j--) {
            Term val;
            if (j == 0) {
                val = HVM4Runtime::termNewVar(argLamLoc);
            } else {
                uint32_t prevDupLoc = argBinding.dupLoc + 2 * (j - 1);
                val = HVM4Runtime::termNewCo1(argBinding.dupLabel + j - 1, prevDupLoc);
            }
            uint32_t dupLoc = argBinding.dupLoc + 2 * j;
            result = ctx.runtime().termNewDupAt(argBinding.dupLabel + j, dupLoc, val, result);
        }
        body = result;
    }

    // Pop __arg binding
    ctx.popBinding();

    // Finalize the outer lambda
    return ctx.runtime().finalizeLam(argLamLoc, body);
}

}  // namespace nix::hvm4
