#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/unforce.hh"

#include <cstdint>

/* Jobs revert only where they have a region (unforce.cc): in builds without
   a garbage collector, on 64-bit Linux. Elsewhere these tests are left out. */
#if !NIX_USE_BOEHMGC && defined(__linux__) && SIZE_MAX > UINT32_MAX

namespace nix {

class UnforceTest : public LibExprTest
{
protected:
    /* A cell allocated outside any job: a thunk of `expr`. */
    Value * oldThunk(std::string_view expr)
    {
        auto v = state.allocValue();
        v->mkThunk(&state.baseEnv, state.parseExprFromString(std::string(expr), state.rootPath(CanonPath::root)));
        return v;
    }

    std::string deep(Value & v)
    {
        state.forceValueDeep(v);
        return printValue(state, v);
    }
};

/* A cell whose value is built in the job is reverted, and forced again
   gives the value it had. */
TEST_F(UnforceTest, revertsYoungValue)
{
    for (auto expr : {
             "{ a = 1; b = [ 2 3 ]; }",
             "[ 1 2 3 ]",
             "\"a\" + \"b\"",
             "let y = 1; in x: y",
             "builtins.map (x: x * 2) [ 1 2 ]",
             "builtins.tryEval (throw \"no\")",
         }) {
        auto v = oldThunk(expr);
        state.beginUnforceScope();
        state.forceValue(*v, noPos);
        state.endUnforceScope();
        EXPECT_TRUE(v->isThunk()) << expr;
        EXPECT_EQ(deep(*v), deep(*oldThunk(expr))) << expr;
    }
}

/* A cell whose value points into no memory of the job keeps it. */
TEST_F(UnforceTest, keepsYoungFreeValue)
{
    auto v = oldThunk("1 + 2");
    auto s = oldThunk("{ a = 1; }");
    state.forceValue(*s, noPos);
    auto w = state.allocValue();
    w->mkApp(oldThunk("s: s"), s);
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    state.forceValue(*w, noPos);
    state.endUnforceScope();
    EXPECT_THAT(*v, IsIntEq(3));
    EXPECT_EQ(w->type(), nAttrs);
    EXPECT_EQ(w->attrs(), s->attrs());
}

/* A cell an earlier job reverted is reverted again, and keeps its value
   outside jobs. */
TEST_F(UnforceTest, revertsAgain)
{
    auto v = oldThunk("{ a = 1; }");
    for (int i = 0; i < 3; ++i) {
        state.beginUnforceScope();
        state.forceValue(*v, noPos);
        state.endUnforceScope();
        EXPECT_TRUE(v->isThunk());
    }
    state.forceValue(*v, noPos);
    EXPECT_EQ(deep(*v), "{ a = 1; }");
}

/* A reverted application is called again. */
TEST_F(UnforceTest, revertsApplication)
{
    auto w = state.allocValue();
    w->mkApp(oldThunk("x: { inherit x; }"), oldThunk("1"));
    state.beginUnforceScope();
    state.forceValue(*w, noPos);
    state.endUnforceScope();
    ASSERT_TRUE(w->isThunk());
    EXPECT_EQ(deep(*w), "{ x = 1; }");
}

/* A cell is re-forced at the call depth of its first force, however deep
   the force that finds it reverted. */
TEST_F(UnforceTest, reforceAtFirstDepth)
{
    evalSettings.maxCallDepth = 100;
    auto v = oldThunk("let f = n: if n == 0 then { inherit n; } else f (n - 1); in f 60");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    state.endUnforceScope();
    ASSERT_TRUE(v->isThunk());
    /* 60 calls deep: 120 in all without the rebase. */
    Value r;
    state.callFunction(*oldThunk("v: let g = n: if n == 0 then v else g (n - 1); in g 60"), *v, r, noPos);
    state.forceValue(r, noPos);
    EXPECT_EQ(r.type(), nAttrs);
}

/* A reverted cell has the position of the value it held, and is trivial,
   as a cell that holds a value is. */
TEST_F(UnforceTest, revertedPosition)
{
    auto e = state.parseExprFromString("let x = 1; in { a = x; }", state.rootPath(CanonPath::root));
    auto v = state.allocValue(), w = state.allocValue();
    v->mkThunk(&state.baseEnv, e);
    w->mkThunk(&state.baseEnv, e);
    EXPECT_FALSE(v->isTrivial());
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    state.endUnforceScope();
    state.forceValue(*w, noPos);
    ASSERT_TRUE(v->isThunk());
    EXPECT_NE(w->determinePos(noPos), noPos);
    EXPECT_EQ(v->determinePos(noPos), w->determinePos(noPos));
    EXPECT_TRUE(v->isTrivial());
}

/* Only the outermost scope is a job. */
TEST_F(UnforceTest, nestedScope)
{
    auto v = oldThunk("{ a = 1; }");
    state.beginUnforceScope();
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    state.endUnforceScope();
    EXPECT_EQ(v->type(), nAttrs);
    state.endUnforceScope();
    EXPECT_TRUE(v->isThunk());
}

/* A failed cell stays failed, with its error, and the job is kept whole. */
TEST_F(UnforceTest, failureKeepsJob)
{
    auto v = oldThunk("{ a = 1; }");
    auto f = oldThunk("throw \"no\"");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_THROW(state.forceValue(*f, noPos), ThrownError);
    state.endUnforceScope();
    EXPECT_EQ(v->type(), nAttrs);
    EXPECT_EQ(deep(*v), "{ a = 1; }");
    ASSERT_TRUE(f->isFailed());
    try {
        state.forceValue(*f, noPos);
        FAIL();
    } catch (ThrownError & e) {
        EXPECT_THAT(e.msg(), ::testing::HasSubstr("no"));
    }
}

/* So is a job in which an effect ran. */
TEST_F(UnforceTest, effectKeepsJob)
{
    auto v = oldThunk("{ a = 1; }");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    unforceNoteEffect();
    state.endUnforceScope();
    EXPECT_FALSE(v->isThunk());
    EXPECT_EQ(deep(*v), "{ a = 1; }");
}

/* What a job allocates while the region is paused outlives it. */
TEST_F(UnforceTest, pause)
{
    Value * kept;
    state.beginUnforceScope();
    {
        PauseRegion pause;
        kept = state.allocValue();
        kept->mkString("kept", state.mem);
    }
    state.endUnforceScope();
    EXPECT_THAT(*kept, IsStringEq("kept"));
}

} // namespace nix

#endif
