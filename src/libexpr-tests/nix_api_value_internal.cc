#include "nix_api_store.h"
#include "nix_api_util.h"
#include "nix_api_expr.h"
#include "nix_api_value.h"
#include "nix_api_external.h"
#include "nix_api_expr_internal.h"

#include "nix/expr/tests/nix_api_expr.hh"
#include "nix/util/tests/string_callback.hh"

#include "nix/expr/eval-settings.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/unforce.hh"

#include <gmock/gmock.h>
#include <cstddef>
#include <cstdlib>
#include <sstream>
#include <gtest/gtest.h>

namespace nixC {

static void
primop_leaf(void * user_data, nix_c_context * context, EvalState * state, nix_value ** args, nix_value * ret)
{
    nix_copy_value(context, ret, args[0]);
}

/* Returns a new `{ f = <function>; }` on every call. */
static void
primop_fresh_set(void * user_data, nix_c_context * context, EvalState * state, nix_value ** args, nix_value * ret)
{
    nix_value * f = nix_alloc_value(context, state);
    nix_init_primop(context, f, (PrimOp *) user_data);
    BindingsBuilder * b = nix_make_bindings_builder(context, state, 1);
    nix_bindings_builder_insert(context, b, "f", f);
    nix_make_attrs(context, ret, b);
    nix_bindings_builder_free(b);
    nix_gc_decref(context, f);
}

/* PROTOTYPE (unforce): a C primop's result is allocated outside the job's
   region, so the producer closure cannot see it, yet a re-forced call builds
   a new one. demo1's schedule (tests/functional/eval-unforce-identity) with
   the inner set built by a C primop: R must stay identical to P.a when R is
   promoted and the thunk calling the primop is reverted. */
TEST_F(nix_api_expr_test, unforce_identity_c_primop)
{
    PrimOp * leaf = nix_alloc_primop(ctx, primop_leaf, 1, "leaf", nullptr, "a function", nullptr);
    assert_ctx_ok();
    PrimOp * fresh = nix_alloc_primop(ctx, primop_fresh_set, 1, "freshSet", nullptr, "a new set", leaf);
    assert_ctx_ok();
    nix_value * freshValue = nix_alloc_value(ctx, state);
    nix_init_primop(ctx, freshValue, fresh);
    assert_ctx_ok();

    nix_expr_eval_from_string(
        ctx,
        state,
        "mk: let P = { a = mk null; }; R = P.a; in "
        "[ (builtins.seq R true) (P ? a) (builtins.seq R true) (R == P.a) ]",
        ".",
        value);
    assert_ctx_ok();
    nix_value * list = nix_alloc_value(ctx, state);
    nix_value_call(ctx, state, value, freshValue, list);
    assert_ctx_ok();
    nix_value_force(ctx, state, list);
    assert_ctx_ok();

    auto & settings = const_cast<nix::EvalSettings &>(state->state.settings);
    settings.unforce = true;
    settings.unforceBatchSize = 0;
    nix::NixStringContext context;
    std::ostringstream out;
    nix::printValueAsJSON(state->state, true, *list->value, nix::noPos, out, context, false, true);
    EXPECT_EQ(out.str(), "[true,true,true,true]");
    nix_gc_decref(ctx, list);
    nix_gc_decref(ctx, freshValue);
}

static size_t unforceClientJobs()
{
    auto json = nix::unforceStatsJSON();
    auto key = std::string("\"clientJobs\":");
    auto i = json.find(key);
    return i == std::string::npos ? 0 : std::stoul(json.substr(i + key.size()));
}

static void print_external_as_json(
    void * self, EvalState * state, bool strict, nix_string_context * c, bool copyToStore, nix_string_return * res)
{
    nix_value * v = nix_alloc_value(nullptr, state);
    nix_gc_decref(nullptr, v);
    nix_set_string_return(res, "42");
}

/* PROTOTYPE (unforce): an external value's JSON printer is client code like
   a C primop: what it allocates is outside the job's region, so a job in
   which it ran promotes nothing (`unforceNoteClientCode`). */
TEST_F(nix_api_expr_test, unforce_external_printer_is_client_code)
{
    NixCExternalValueDesc desc{};
    desc.print = [](void *, nix_printer *) {};
    desc.showType = [](void *, nix_string_return *) {};
    desc.typeOf = [](void *, nix_string_return *) {};
    desc.printValueAsJSON = print_external_as_json;
    ExternalValue * ext = nix_create_external_value(ctx, &desc, nullptr);
    assert_ctx_ok();
    nix_value * extValue = nix_alloc_value(ctx, state);
    nix_init_external(ctx, extValue, ext);
    assert_ctx_ok();

    nix_expr_eval_from_string(ctx, state, "e: [ e e ]", ".", value);
    assert_ctx_ok();
    nix_value * list = nix_alloc_value(ctx, state);
    nix_value_call(ctx, state, value, extValue, list);
    assert_ctx_ok();
    nix_value_force(ctx, state, list);
    assert_ctx_ok();

    auto & settings = const_cast<nix::EvalSettings &>(state->state.settings);
    settings.unforce = true;
    settings.unforceBatchSize = 0;
    auto before = unforceClientJobs();
    nix::NixStringContext context;
    std::ostringstream out;
    nix::printValueAsJSON(state->state, true, *list->value, nix::noPos, out, context, false, true);
    EXPECT_EQ(out.str(), "[42,42]");
    /* Unforcing never runs with Boehm GC (`unforceEnabled`). */
    if (!NIX_USE_BOEHMGC) {
        EXPECT_GE(unforceClientJobs(), before + 2);
    }
    nix_gc_decref(ctx, list);
    nix_gc_decref(ctx, extValue);
}

} // namespace nixC
