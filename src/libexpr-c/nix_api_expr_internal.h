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
    /**
     * The evaluator the value belongs to.  Ideally we would take in EvalState as an argument when we need it, but we
     * don't want to make changes to the stable C api, so we stuff it into the nix_value that will get passed in to the
     * relevant functions: allocation needs its memory, and `nix_get_string` needs a door (`EvalState::realise`), which
     * writes the pending store objects a string may name before its text reaches C.
     */
    nix::EvalState * state;
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

/**
 * The C API's boundary with the evaluator: every entry point that receives one
 * runs its body here, and whatever evaluation left pending is written before
 * control returns to C, after a return and after a throw (04-derivation.md,
 * section 1.2).  An error is stored in `context` as `NIXC_CATCH_ERRS` does; a
 * failing write after an error leaves the first error standing and the objects
 * pending.  A `void` body yields `NIX_OK` or the error code; any other yields
 * its result, or a value-initialised one after a failure (a heap result whose
 * write then failed is not released).
 */
template<typename Body>
auto nix_c_boundary(nix_c_context * context, EvalState * state, Body && body)
{
    using Result = std::invoke_result_t<Body>;
    if (context)
        context->last_err_code = NIX_OK;
    try {
        if constexpr (std::is_void_v<Result>) {
            body();
            state->state.flushPendingWrites();
            return NIX_OK;
        } else {
            static_assert(
                !std::is_same_v<Result, nix_err>,
                "a body returning nix_err would yield NIX_OK on failure; return void and let the boundary report");
            Result result = body();
            state->state.flushPendingWrites();
            return result;
        }
    } catch (...) {
        try {
            state->state.flushPendingWrites();
        } catch (...) {
            /* The first error stands. */
        }
        if constexpr (std::is_void_v<Result>)
            return nix_context_error(context);
        else {
            nix_context_error(context);
            return Result{};
        }
    }
}

/**
 * The boundary in two phases: `produce` allocates the object handed to C from
 * what `evaluate` returned only after the write, so a failing write leaks
 * nothing; a null or empty `evaluate` result (a soft failure already in
 * `context`) is mapped to null.
 */
template<typename Evaluate, typename Produce>
auto nix_c_boundary(nix_c_context * context, EvalState * state, Evaluate && evaluate, Produce && produce)
{
    using Result = std::invoke_result_t<Produce, std::invoke_result_t<Evaluate>>;
    static_assert(!std::is_same_v<Result, nix_err>, "an object-producing boundary does not return nix_err");
    if (context)
        context->last_err_code = NIX_OK;
    try {
        auto evaluated = evaluate();
        state->state.flushPendingWrites();
        return produce(std::move(evaluated));
    } catch (...) {
        try {
            state->state.flushPendingWrites();
        } catch (...) {
            /* The first error stands. */
        }
        nix_context_error(context);
        return Result{};
    }
}

#endif // NIX_API_EXPR_INTERNAL_H
