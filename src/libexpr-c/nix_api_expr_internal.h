#ifndef NIX_API_EXPR_INTERNAL_H
#define NIX_API_EXPR_INTERNAL_H

#include <memory>
#include <stdexcept>
#include <type_traits>

#include "nix/fetchers/fetch-settings.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/attr-set.hh"
#include "nix_api_value.h"
#include "nix_api_util_internal.h"
#include "nix/expr/search-path.hh"

extern "C" {

struct nix_eval_state_builder
{
    nix::ref<nix::Store> store;
    nix::EvalSettings settings;
    nix::fetchers::Settings fetchSettings;
    nix::LookupPath lookupPath;
    nix::ref<bool> readOnlyMode;
};

struct EvalState
{
    nix::EvalState & state;
    // Owned resources; null for temporary wrappers created in C API callbacks.
    std::shared_ptr<bool> ownedReadOnlyMode;
    std::unique_ptr<nix::fetchers::Settings> ownedFetchSettings;
    std::unique_ptr<nix::EvalSettings> ownedSettings;
    std::shared_ptr<nix::EvalState> ownedState;
};

struct BindingsBuilder
{
    nix::BindingsBuilder builder;
};

struct ListBuilder
{
    nix::ListBuilder builder;
};

struct nix_value
{
    nix::Value * value;
    /** Memory for allocations associated with this value. */
    nix::EvalMemory * mem;
};

struct nix_string_return
{
    std::string str;
};

struct nix_printer
{
    std::ostream & s;
};

struct nix_string_context
{
    nix::NixStringContext & ctx;
};

struct nix_realised_string
{
    std::string str;
    std::vector<StorePath> storePaths;
};

} // extern "C"

// Shared helpers for validating nix_value [in] parameters across libexpr-c translation units.
inline const nix::Value & check_value_not_null(const nix_value * value)
{
    if (!value || !value->value)
        throw std::runtime_error("nix_value is null");
    return *value->value;
}

inline nix::Value & check_value_not_null(nix_value * value)
{
    if (!value || !value->value)
        throw std::runtime_error("nix_value is null");
    return *value->value;
}

inline const nix::Value & check_value_in(const nix_value * value)
{
    auto & v = check_value_not_null(value);
    if (!v.isValid())
        throw std::runtime_error("Uninitialized nix_value");
    return v;
}

inline nix::Value & check_value_in(nix_value * value)
{
    auto & v = check_value_not_null(value);
    if (!v.isValid())
        throw std::runtime_error("Uninitialized nix_value");
    return v;
}

/** Convert C++ failures to C errors, publishing results only on success. */
template<typename Body>
auto nix_c_boundary(nix_c_context * context, Body && body)
{
    using Result = std::invoke_result_t<Body>;
    if (context)
        context->last_err_code = NIX_OK;
    try {
        if constexpr (std::is_void_v<Result>) {
            body();
            return NIX_OK;
        } else {
            static_assert(
                !std::is_same_v<Result, nix_err>,
                "a body returning nix_err would yield NIX_OK on failure; return void and let the boundary report");
            Result result = body();
            return result;
        }
    } catch (...) {
        if constexpr (std::is_void_v<Result>)
            return nix_context_error(context);
        else {
            nix_context_error(context);
            return Result{};
        }
    }
}

/** Keep evaluation and result allocation separate: no C handle is published
 * until evaluation succeeds. A soft failure is passed to `produce`, which
 * preserves the error and maps it to the API's empty result. */
template<typename Evaluate, typename Produce>
auto nix_c_boundary(nix_c_context * context, Evaluate && evaluate, Produce && produce)
{
    using Result = std::invoke_result_t<Produce, std::invoke_result_t<Evaluate>>;
    static_assert(!std::is_same_v<Result, nix_err>, "an object-producing boundary does not return nix_err");
    if (context)
        context->last_err_code = NIX_OK;
    try {
        auto evaluated = evaluate();
        return produce(std::move(evaluated));
    } catch (...) {
        nix_context_error(context);
        return Result{};
    }
}

#endif // NIX_API_EXPR_INTERNAL_H
