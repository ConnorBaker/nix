/**
 * CompileContext Implementation
 *
 * Manages compilation state including variable bindings, scope tracking,
 * and fresh label generation for DUP nodes.
 */

#include "nix/expr/hvm4/hvm4-compiler.hh"

namespace nix::hvm4 {

// ============================================================================
// CompileContext Implementation
// ============================================================================

CompileContext::CompileContext(HVM4Runtime& runtime, const SymbolTable& symbols)
    : runtime_(runtime)
    , symbols_(symbols)
{}

void CompileContext::pushBinding(Symbol name, uint32_t heapLoc) {
    bindings_.push_back(VarBinding{
        .name = name,
        .depth = depth_,
        .heapLoc = heapLoc,
        .useCount = 0,
        .dupLabel = 0,
        .dupIndex = 0
    });
    depth_++;
}

void CompileContext::popBinding() {
    if (!bindings_.empty()) {
        bindings_.pop_back();
    }
    if (depth_ > 0) {
        depth_--;
    }
}

VarBinding* CompileContext::lookup(Symbol name) {
    // Search from innermost to outermost
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
        if (it->name == name) {
            return &(*it);
        }
    }
    return nullptr;
}

const VarBinding* CompileContext::lookup(Symbol name) const {
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
        if (it->name == name) {
            return &(*it);
        }
    }
    return nullptr;
}

bool CompileContext::hasBinding(Symbol name) const {
    return lookup(name) != nullptr;
}

void CompileContext::pushWith(const ExprWith* expr, size_t bindingIndex) {
    withStack_.push_back(WithBinding{expr, bindingIndex});
}

void CompileContext::popWith() {
    if (!withStack_.empty()) {
        withStack_.pop_back();
    }
}

const WithBinding* CompileContext::lookupWith(const ExprWith* expr) const {
    // Search from innermost to outermost
    for (auto it = withStack_.rbegin(); it != withStack_.rend(); ++it) {
        if (it->expr == expr) {
            return &(*it);
        }
    }
    return nullptr;
}

void CompileContext::pushInheritFrom(InheritFromContext ctx) {
    inheritFromStack_.push_back(std::move(ctx));
}

void CompileContext::popInheritFrom() {
    if (!inheritFromStack_.empty()) {
        inheritFromStack_.pop_back();
    }
}

Term CompileContext::getInheritFromExpr(size_t displ) const {
    // Get the compiled term for inherit-from expression at index displ
    // The displ is relative to the current inherit-from context
    // Handles DUP for multi-use inherit-from expressions
    if (inheritFromStack_.empty()) {
        throw HVM4Error("No inherit-from context available");
    }
    const auto& ctx = inheritFromStack_.back();
    if (displ >= ctx.entries.size()) {
        throw HVM4Error("Invalid inherit-from displacement");
    }

    const auto& entry = ctx.entries[displ];

    // Single-use: just return the compiled term
    if (entry.useCount <= 1) {
        return entry.compiledExpr;
    }

    // Multi-use: return appropriate projection from DUP chain
    // For N uses with N-1 DUPs:
    // - Uses 0 to N-2: CO0 of DUPs 0 to N-2
    // - Use N-1: CO1 of DUP N-2
    uint32_t idx = entry.useIndex++;
    uint32_t numDups = entry.useCount - 1;

    if (idx < numDups) {
        // Use CO0 of DUP idx
        uint32_t dupLoc = entry.dupLoc + 2 * idx;
        return HVM4Runtime::termNewCo0(entry.dupLabel + idx, dupLoc);
    } else {
        // Last use: CO1 of last DUP
        uint32_t dupLoc = entry.dupLoc + 2 * (numDups - 1);
        return HVM4Runtime::termNewCo1(entry.dupLabel + numDups - 1, dupLoc);
    }
}

uint32_t CompileContext::freshLabel() {
    return labelCounter_++;
}

uint32_t CompileContext::freshLabels(uint32_t count) {
    uint32_t base = labelCounter_;
    labelCounter_ += count;
    return base;
}

uint64_t CompileContext::allocate(uint64_t size) {
    return runtime_.allocate(size);
}

}  // namespace nix::hvm4
