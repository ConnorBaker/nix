/* inc3-S1 rethrow parity (the folded 2b finished-election gap).

   Under concurrent forcing a cell seen as a thunk at the inlined forceValue
   check (`eval-inline.hh`, `v.isThunk()`) can be FINISHED by the elected
   producer between that read and the election's own acquire read inside
   `electThunk`/`electApp`. When the producer finished it as a FAILED value, the
   finished-election arm must rethrow the stored exception -- exactly as the two
   sibling waited paths do (`forceValuePending` and `forceValueOnPending` in
   eval.cc, `if (v.isFailed()) v.failed().rethrow();`). Before this fix that arm
   returned {null,null} without rethrowing, so a concurrently-failed elected
   cell was silently treated as a valid WHNF (a dropped error, and a crash if
   later serialized).

   That arm is unreachable from a single-threaded `forceValue` (the inline
   `isThunk()` read and the election acquire read agree with no concurrent
   finisher), so it is tested here white-box: build a FAILED cell with
   `mkFailed` and call the election entry points directly. This is deterministic
   and has teeth -- it throws on the fixed tree and (silently) returns without
   throwing on the unfixed one. */

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/eval-inline.hh"

#include <gtest/gtest.h>

namespace nix {

class ElectionRethrowTest : public LibExprTest
{
protected:
    /* These reach the private election entry points via the `friend` on
       EvalState (eval.hh). Calling them on a FINISHED cell takes the
       finished-election arm the fix touches. */
    Value::ClosureThunk electThunk(Value & v)
    {
        return state.electThunk(v, noPos);
    }
    Value::FunctionApplicationThunk electApp(Value & v)
    {
        return state.electApp(v, noPos);
    }

    /* A cell finished as FAILED carrying `msg` (no `%` -> no format surprise). */
    Value * failedCell(const char * msg)
    {
        std::exception_ptr ep;
        try {
            throw Error(msg);
        } catch (...) {
            ep = std::current_exception();
        }
        auto v = state.allocValue();
        v->mkFailed(ep, nullptr);
        return v;
    }
};

TEST_F(ElectionRethrowTest, failedElectedThunkRethrows)
{
    auto v = failedCell("boom-thunk");
    EXPECT_THROW(electThunk(*v), Error);
    try {
        electThunk(*v);
        FAIL() << "electThunk did not rethrow a FAILED elected-finished cell";
    } catch (Error & e) {
        EXPECT_NE(std::string(e.what()).find("boom-thunk"), std::string::npos);
    }
}

TEST_F(ElectionRethrowTest, failedElectedAppRethrows)
{
    auto v = failedCell("boom-app");
    EXPECT_THROW(electApp(*v), Error);
    try {
        electApp(*v);
        FAIL() << "electApp did not rethrow a FAILED elected-finished cell";
    } catch (Error & e) {
        EXPECT_NE(std::string(e.what()).find("boom-app"), std::string::npos);
    }
}

} // namespace nix
