/* The single-thread unforcing evaluator treats an allocation failure at the
   end of an unforce batch -- while deciding which values to keep and copying
   them -- as fatal: there is no way to release a job region partially, so
   `decideJob` (unforce.cc) throwing `std::bad_alloc` is caught at the batch
   boundary and turned into `unforceFatal`, which prints a diagnostic and
   calls `std::abort`.

   That OOM-abort path cannot be reached from ordinary evaluation on demand:
   it needs the allocator to fail at one precise instant. The test-only
   `unforceTestFailDecide` hook (unforce.hh) injects the throw at that exact
   instant -- after the keep-set has been decided, before the evacuator
   copies -- so the fatal path runs deterministically. This death test drives
   an evaluation that reaches an unforce batch decide with a promoted target
   (the same two-job force-between-jobs workload the identity tests use, which
   nominates a cell forced in two jobs) and asserts the child dies with the
   `unforceFatal` message.

   Mutation check: if the batch-boundary catch did NOT call `unforceFatal`
   (e.g. it swallowed the throw, or `keepWhole` bypassed the copy), the child
   would exit normally without the message and this test would fail. */

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/unforce.hh"
#include "nix/util/canon-path.hh"

#include <gtest/gtest.h>

#include <csignal>

namespace nix {

namespace {

/* Run `f` as one job (an outermost unforce scope) with batch size 0, so the
   job ends -- and decides -- as soon as `f` returns. */
template<typename F>
void oomJob(EvalState & state, F && f)
{
    UnforceBatch batch(state);
    {
        UnforceBatch::Element element(batch);
        f();
    }
    batch.end();
}

Value * oomAttr(EvalState & state, Value & v, std::string_view name)
{
    state.forceValue(v, noPos);
    auto a = v.attrs()->get(state.symbols.create(name));
    assert(a);
    return a->value;
}

} // namespace

class UnforceOOMTest : public LibExprTest
{
protected:
    UnforceOOMTest()
    {
        evalSettings.unforce = true;
        evalSettings.unforceBatchSize = 0;
    }

    /* A cell allocated outside any job, so older than every job. */
    Value * oldCell(std::string_view expr)
    {
        auto v = state.allocValue();
        state.eval(state.parseExprFromString(std::string(expr), state.rootPath(CanonPath::root)), *v);
        return v;
    }
};

/* Clean build: the injected allocation failure reaches `unforceFatal`, which
   prints its message and aborts. The child must die with that message. */
TEST_F(UnforceOOMTest, decideOutOfMemoryIsFatal)
{
    /* Unforcing never runs with Boehm GC, so the decide path is never taken
       and the hook would never fire. */
    if (NIX_USE_BOEHMGC)
        GTEST_SKIP() << "unforcing is disabled under Boehm GC";

    ASSERT_EXIT(
        {
            /* Set inside the forked death-test child; the parent's global
               stays false (and is reset below regardless). */
            unforceTestFailDecide = true;

            /* Force R, then force its producer P between jobs, then force R
               again: R, forced in two jobs, is nominated for promotion, so
               the second job's decide has a promoted target and reaches the
               injection point. (Mirrors UnforceIdentityTest's
               forcingBetweenJobsKeepsIdentity.) */
            auto top = oldCell("let P = { a = { f = x: x; }; }; R = P.a; in { inherit P R; }");
            auto P = oomAttr(state, *top, "P");
            auto R = oomAttr(state, *top, "R");

            oomJob(state, [&] { state.forceValue(*R, noPos); });
            state.forceValue(*P, noPos);
            oomJob(state, [&] { state.forceValue(*R, noPos); });
        },
        ::testing::KilledBySignal(SIGABRT),
        "out of memory while deciding");

    /* Defensive: the parent never ran the injected statement, but keep the
       global clean for any later test in this binary. */
    unforceTestFailDecide = false;
}

} // namespace nix
