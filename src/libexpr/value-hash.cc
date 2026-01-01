#include "nix/expr/value-hash.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/util/hash.hh"

#include <atomic>

namespace nix {

// Debug counters for understanding skip reasons
std::atomic<size_t> nrHashSkipDepth{0};
std::atomic<size_t> nrHashSkipThunk{0};
std::atomic<size_t> nrHashSkipLargeAttrs{0};
std::atomic<size_t> nrHashSkipLargeList{0};
std::atomic<size_t> nrHashSkipExternal{0};
std::atomic<size_t> nrHashSkipNonCheapThunk{0};
std::atomic<size_t> nrHashOK{0};
std::atomic<size_t> nrHashSkipNestedThunk{0};
std::atomic<size_t> nrHashSkipNestedNonCheap{0};

/**
 * Check if an expression is "cheap" to evaluate - i.e., it won't trigger
 * expensive computation. Cheap expressions are:
 * - Literals (int, float, string, path)
 * - Variable references (just an env lookup)
 * - Empty or small lists/attrsets with cheap elements
 */
static bool isCheapExpr(Expr * expr)
{
    if (dynamic_cast<ExprInt *>(expr)
        || dynamic_cast<ExprFloat *>(expr)
        || dynamic_cast<ExprString *>(expr)
        || dynamic_cast<ExprPath *>(expr)
        || dynamic_cast<ExprVar *>(expr))
        return true;

    // Empty list is cheap
    if (auto list = dynamic_cast<ExprList *>(expr)) {
        if (list->elems.empty())
            return true;
        // Small list with all cheap elements
        if (list->elems.size() <= 4) {
            for (auto & elem : list->elems)
                if (!isCheapExpr(elem))
                    return false;
            return true;
        }
    }

    // Empty or small non-recursive attrset is cheap
    if (auto attrs = dynamic_cast<ExprAttrs *>(expr)) {
        if (attrs->recursive)
            return false;  // rec { } could have cycles
        if (attrs->dynamicAttrs && !attrs->dynamicAttrs->empty())
            return false;  // dynamic attrs need evaluation
        if (!attrs->attrs || attrs->attrs->empty())
            return true;  // empty attrset
        // Small attrset with all cheap values
        if (attrs->attrs->size() <= 4) {
            for (auto & [name, def] : *attrs->attrs)
                if (!isCheapExpr(def.e))
                    return false;
            return true;
        }
    }

    return false;
}

/**
 * Check if a thunk is "cheap" to force - meaning it won't trigger
 * expensive computation chains.
 */
static bool isCheapThunk(Value & v)
{
    if (!v.isThunk())
        return false;

    // Only closure thunks have an expression we can inspect
    // App thunks and PrimOpApp thunks could trigger arbitrary computation
    auto thunk = v.thunk();
    return thunk.expr && isCheapExpr(thunk.expr);
}

bool isHashableValue(EvalState & state, Value & v, size_t depth)
{
    if (depth > maxHashDepth)
        return false;

    if (v.isThunk())
        return false;

    switch (v.type()) {
    case nInt:
    case nFloat:
    case nBool:
    case nNull:
    case nString:
        return true;

    case nAttrs: {
        auto attrs = v.attrs();
        if (attrs->size() > maxHashableAttrs)
            return false;
        for (auto & attr : *attrs) {
            if (!isHashableValue(state, *attr.value, depth + 1))
                return false;
        }
        return true;
    }

    case nList: {
        auto view = v.listView();
        for (size_t i = 0; i < view.size(); ++i) {
            if (!isHashableValue(state, *view[i], depth + 1))
                return false;
        }
        return true;
    }

    case nFunction:
    case nThunk:
    case nPath:
    case nExternal:
        return false;
    }

    return false;
}

std::optional<Hash> tryHashValue(EvalState & state, Value & v, size_t depth)
{
    if (depth > maxHashDepth)
        return std::nullopt;

    if (v.isThunk())
        return std::nullopt;

    switch (v.type()) {
    case nInt: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "int:" << std::to_string(v.integer().value);
        return sink.finish().hash;
    }

    case nFloat: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "float:" << std::to_string(v.fpoint());
        return sink.finish().hash;
    }

    case nBool: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "bool:" << (v.boolean() ? "true" : "false");
        return sink.finish().hash;
    }

    case nNull: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "null";
        return sink.finish().hash;
    }

    case nString: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "string:" << v.string_view();
        return sink.finish().hash;
    }

    case nAttrs: {
        auto attrs = v.attrs();
        if (attrs->size() > maxHashableAttrs)
            return std::nullopt;

        HashSink sink(HashAlgorithm::SHA256);
        sink << "attrs:" << std::to_string(attrs->size()) << ":";

        for (auto & attr : *attrs) {
            sink << state.symbols[attr.name] << ":";
            auto attrHash = tryHashValue(state, *attr.value, depth + 1);
            if (!attrHash)
                return std::nullopt;
            sink << attrHash->to_string(HashFormat::Base16, false) << ";";
        }
        return sink.finish().hash;
    }

    case nList: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "list:" << std::to_string(v.listSize()) << ":";

        auto view = v.listView();
        for (size_t i = 0; i < view.size(); ++i) {
            auto elemHash = tryHashValue(state, *view[i], depth + 1);
            if (!elemHash)
                return std::nullopt;
            sink << elemHash->to_string(HashFormat::Base16, false) << ";";
        }
        return sink.finish().hash;
    }

    case nFunction:
    case nThunk:
    case nPath:
    case nExternal:
        return std::nullopt;
    }

    return std::nullopt;
}

std::optional<Hash> tryForceAndHashValue(EvalState & state, Value & v, size_t depth)
{
    if (depth > maxHashDepth) {
        nrHashSkipDepth++;
        return std::nullopt;
    }

    // Only force "cheap" thunks - variable lookups and literals
    if (v.isThunk()) {
        if (!isCheapThunk(v)) {
            if (depth == 0)
                nrHashSkipNonCheapThunk++;
            else
                nrHashSkipNestedNonCheap++;
            return std::nullopt;
        }
        try {
            state.forceValue(v, noPos);
        } catch (...) {
            if (depth == 0)
                nrHashSkipThunk++;
            else
                nrHashSkipNestedThunk++;
            return std::nullopt;
        }
    }

    switch (v.type()) {
    case nInt: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "int:" << std::to_string(v.integer().value);
        return sink.finish().hash;
    }

    case nFloat: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "float:" << std::to_string(v.fpoint());
        return sink.finish().hash;
    }

    case nBool: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "bool:" << (v.boolean() ? "true" : "false");
        return sink.finish().hash;
    }

    case nNull: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "null";
        return sink.finish().hash;
    }

    case nString: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "string:" << v.string_view();
        return sink.finish().hash;
    }

    case nAttrs: {
        auto attrs = v.attrs();
        if (attrs->size() > maxHashableAttrs) {
            if (depth == 0) nrHashSkipLargeAttrs++;
            return std::nullopt;
        }

        HashSink sink(HashAlgorithm::SHA256);
        sink << "attrs:" << std::to_string(attrs->size()) << ":";

        for (auto & attr : *attrs) {
            sink << state.symbols[attr.name] << ":";
            auto attrHash = tryForceAndHashValue(state, *attr.value, depth + 1);
            if (!attrHash)
                return std::nullopt;
            sink << attrHash->to_string(HashFormat::Base16, false) << ";";
        }
        if (depth == 0) nrHashOK++;
        return sink.finish().hash;
    }

    case nList: {
        if (v.listSize() > maxHashableAttrs) {
            if (depth == 0) nrHashSkipLargeList++;
            return std::nullopt;
        }

        HashSink sink(HashAlgorithm::SHA256);
        sink << "list:" << std::to_string(v.listSize()) << ":";

        auto view = v.listView();
        for (size_t i = 0; i < view.size(); ++i) {
            auto elemHash = tryForceAndHashValue(state, *view[i], depth + 1);
            if (!elemHash)
                return std::nullopt;
            sink << elemHash->to_string(HashFormat::Base16, false) << ";";
        }
        return sink.finish().hash;
    }

    case nFunction: {
        // Hash functions by identity (stable for file-cached functions)
        HashSink sink(HashAlgorithm::SHA256);
        sink << "function:";
        if (v.isLambda()) {
            auto lambda = v.lambda();
            sink << std::to_string(reinterpret_cast<uintptr_t>(lambda.fun));
            sink << ":";
            sink << std::to_string(reinterpret_cast<uintptr_t>(lambda.env));
        } else if (v.isPrimOp()) {
            sink << "primop:" << v.primOp()->name;
        } else if (v.isPrimOpApp()) {
            sink << "primopapp:";
            sink << std::to_string(reinterpret_cast<uintptr_t>(&v));
        }
        return sink.finish().hash;
    }

    case nPath: {
        HashSink sink(HashAlgorithm::SHA256);
        sink << "path:" << v.path().to_string();
        return sink.finish().hash;
    }

    case nThunk:
        // Non-cheap thunk - give up (shouldn't reach here after the check above)
        if (depth == 0)
            nrHashSkipThunk++;
        else
            nrHashSkipNestedThunk++;
        return std::nullopt;

    case nExternal:
        nrHashSkipExternal++;
        return std::nullopt;
    }

    return std::nullopt;
}

} // namespace nix
