#include "nix_api_store.h"
#include "nix_api_util.h"
#include "nix_api_expr.h"
#include "nix_api_value.h"
#include "nix_api_expr_internal.h"

#include "nix/expr/tests/nix_api_expr.hh"
#include "nix/util/tests/string_callback.hh"

#include "nix/expr/eval-settings.hh"
#include "nix/expr/value-to-json.hh"

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

} // namespace nixC
