/* PROTOTYPE (unforce): promotion must not break pointer identity, which
   `eqValues` short-circuits on. These tests drive jobs (outermost unforce
   scopes) by hand, around events that change the heap between jobs. With
   the default NIX_UNFORCE_PROMOTE (2), a cell forced in two jobs is
   nominated for promotion, and the producer closure (`decideJob` in
   unforce.cc) must keep what it holds consistent. Ported from the epoch
   branch as identity regressions: they assert identity, not a policy. */

#include "nix_api_store.h"
#include "nix_api_util.h"
#include "nix_api_expr.h"
#include "nix_api_value.h"
#include "nix_api_expr_internal.h"

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/tests/nix_api_expr.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/expr/print.hh"

#include <gtest/gtest.h>

namespace nix {

namespace {

/* Run `f` as one job with batch size 0. */
template<typename F>
void job(EvalState & state, F && f)
{
    UnforceBatch batch(state);
    {
        UnforceBatch::Element element(batch);
        f();
    }
    batch.end();
}

Value * attr(EvalState & state, Value & v, std::string_view name)
{
    state.forceValue(v, noPos);
    auto a = v.attrs()->get(state.symbols.create(name));
    assert(a);
    return a->value;
}

} // namespace

class UnforceIdentityTest : public LibExprTest
{
protected:
    UnforceIdentityTest()
    {
        evalSettings.unforce = true;
        evalSettings.unforceBatchSize = 0;
    }

    /* Cells allocated outside any job, so older than every job. */
    Value * oldCell(std::string_view expr)
    {
        auto v = state.allocValue();
        state.eval(state.parseExprFromString(std::string(expr), state.rootPath(CanonPath::root)), *v);
        return v;
    }
};

/* `resetFileCache` drops the cell an import was evaluated into; the next
   import creates a new one, F, whose consumers were counted in earlier jobs.
   A count-only policy promotes R (second job) holding a copy of F's value
   while F is reverted, and F is promoted with its value rebuilt in the third
   job, along with R2. Without unforcing, R and R2 both hold parts of the
   value imported before the reset; with unforcing they are reverted and
   re-import after it. Either way they must hold parts of one import. */
TEST_F(UnforceIdentityTest, resetFileCacheBetweenJobsKeepsIdentity)
{
    auto accessor = make_ref<MemorySourceAccessor>();
    auto file = accessor->addFile(CanonPath("/set.nix"), "{ a = { b = { f = x: x; }; }; }");
    auto pathV = state.allocValue();
    pathV->mkPath(file, state.mem);
    auto selectA = oldCell("p: (import p).a");
    auto selectB = oldCell("p: (import p).a.b");
    state.forceValue(*selectA, noPos);
    state.forceValue(*selectB, noPos);

    auto R = state.allocValue();
    R->mkApp(selectA, pathV);
    auto R2 = state.allocValue();
    R2->mkApp(selectB, pathV);

    job(state, [&] {
        state.forceValue(*R, noPos);
        state.forceValue(*R2, noPos);
    });
    state.resetFileCache();
    job(state, [&] { state.forceValue(*R, noPos); });
    job(state, [&] { state.forceValue(*R2, noPos); });

    bool equal = false;
    job(state, [&] { equal = state.eqValues(*R2, *attr(state, *R, "b"), noPos, ""); });
    EXPECT_TRUE(equal);
}

/* Forcing outside a job is permanent: P becomes a value whose attribute `a`
   is a new old thunk, while R was already counted. A count-only policy
   promotes R (second job) holding a copy of `a`'s value while `a` is
   reverted. */
TEST_F(UnforceIdentityTest, forcingBetweenJobsKeepsIdentity)
{
    auto top = oldCell("let P = { a = { f = x: x; }; }; R = P.a; in { inherit P R; }");
    auto P = attr(state, *top, "P");
    auto R = attr(state, *top, "R");

    job(state, [&] { state.forceValue(*R, noPos); });
    state.forceValue(*P, noPos);
    job(state, [&] { state.forceValue(*R, noPos); });

    bool equal = false;
    job(state, [&] { equal = state.eqValues(*R, *attr(state, *P, "a"), noPos, ""); });
    EXPECT_TRUE(equal);
}

} // namespace nix

namespace nixC {

/* `mk _` returns `{ a = g 0; }`, where `g 0` is an application cell the C
   code allocates and `g` (the user data) is a function allocated before any
   job. A C primop allocates outside the job, so every call inside a job
   creates a new old cell; it keeps no argument (which may be young). */
static void primop_mk(void * g, nix_c_context * context, EvalState * state, nix_value **, nix_value * ret)
{
    nix_value * zero = nix_alloc_value(context, state);
    nix_init_int(context, zero, 0);
    nix_value * c = nix_alloc_value(context, state);
    nix_init_apply(context, c, (nix_value *) g, zero);
    BindingsBuilder * b = nix_make_bindings_builder(context, state, 1);
    nix_bindings_builder_insert(context, b, "a", c);
    nix_make_attrs(context, ret, b);
    nix_bindings_builder_free(b);
    nix_gc_decref(context, c);
    nix_gc_decref(context, zero);
}

/* A cell created by C code in one job is reverted unless promoted. S (a copy
   of `a`'s value) and R (which refers to `a`) both reach the promotion count
   in the second job, while the `a` created in that job has been forced once:
   promoting S and R would split S from `R.a`. The producer closure cannot
   see `a`'s cell, which the C code allocated outside the job; the rule that
   a job running C API client code promotes nothing keeps identity. */
TEST_F(nix_api_expr_test, unforce_c_primop_cells_not_promoted_with_consumers)
{
    auto & st = state->state;
    state->ownedSettings->unforce = true;
    state->ownedSettings->unforceBatchSize = 0;

    nix_value * g = nix_alloc_value(ctx, state);
    nix_expr_eval_from_string(ctx, state, "_: { f = x: x; }", ".", g);
    nix_value_force(ctx, state, g);
    PrimOp * primop = nix_alloc_primop(ctx, primop_mk, 1, "mk", nullptr, nullptr, g);
    ASSERT_NE(nullptr, primop);
    nix_value * mk = nix_alloc_value(ctx, state);
    nix_init_primop(ctx, mk, primop);

    auto fn = st.allocValue();
    st.eval(
        st.parseExprFromString("mk: let R = mk null; S = R.a; in { inherit R S; }", st.rootPath(nix::CanonPath::root)),
        *fn);
    auto top = st.allocValue();
    top->mkApp(fn, mk->value);
    st.forceValue(*top, nix::noPos);
    auto R = top->attrs()->get(st.symbols.create("R"))->value;
    auto S = top->attrs()->get(st.symbols.create("S"))->value;

    nix::job(st, [&] { st.forceValue(*S, nix::noPos); });
    nix::job(st, [&] { st.forceValue(*S, nix::noPos); });

    bool equal = false;
    nix::job(st, [&] { equal = st.eqValues(*S, *nix::attr(st, *R, "a"), nix::noPos, ""); });
    EXPECT_TRUE(equal);

    nix_gc_decref(ctx, mk);
    nix_gc_decref(ctx, primop);
    nix_gc_decref(ctx, g);
}

static size_t unforceStat(std::string_view name)
{
    auto json = nix::unforceStatsJSON();
    auto key = "\"" + std::string(name) + "\":";
    auto i = json.find(key);
    return i == std::string::npos ? 0 : std::stoul(json.substr(i + key.size()));
}

/* A set printed in a job where client code ran, and reached again in a later
   job, prints as «repeated» (review of markers v3, S4). That job promotes
   nothing, so it would revert the set, which the next job would rebuild at
   another address; it keeps all its memory instead. Printed as `nix eval`
   prints (a job per list element). */
TEST_F(nix_api_expr_test, unforce_client_code_job_keeps_repeated)
{
    auto & st = state->state;
    state->ownedSettings->unforceBatchSize = 0;

    nix_value * g = nix_alloc_value(ctx, state);
    nix_expr_eval_from_string(ctx, state, "_: { f = x: x; }", ".", g);
    nix_value_force(ctx, state, g);
    PrimOp * primop = nix_alloc_primop(ctx, primop_mk, 1, "mk", nullptr, nullptr, g);
    ASSERT_NE(nullptr, primop);
    nix_value * mk = nix_alloc_value(ctx, state);
    nix_init_primop(ctx, mk, primop);

    auto fn = st.allocValue();
    st.eval(
        st.parseExprFromString(
            "mk: let s = { a = 1; }; in [ (builtins.seq (mk null) s) s ]", st.rootPath(nix::CanonPath::root)),
        *fn);
    auto print = [&](bool unforce) {
        state->ownedSettings->unforce = unforce;
        auto v = st.allocValue();
        v->mkApp(fn, mk->value);
        std::ostringstream out;
        nix::printValue(st, out, *v, nix::PrintOptions{.force = true, .derivationPaths = true, .unforce = unforce});
        return out.str();
    };

    auto expected = print(false);
    EXPECT_EQ(expected, "[ { a = 1; } «repeated» ]");
    auto clientJobs = unforceStat("clientJobs"), kept = unforceStat("keptJobs");
    EXPECT_EQ(print(true), expected);
    /* Unforcing never runs with Boehm GC (`unforceEnabled`). */
    if (!NIX_USE_BOEHMGC) {
        EXPECT_GE(unforceStat("clientJobs"), clientJobs + 1);
        EXPECT_GE(unforceStat("keptJobs"), kept + 1);
    }

    nix_gc_decref(ctx, mk);
    nix_gc_decref(ctx, primop);
    nix_gc_decref(ctx, g);
}

} // namespace nixC
