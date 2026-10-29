#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/expr/print.hh"
#include "nix/expr/print-ambiguous.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/value-to-xml.hh"
#include "nix/util/signals.hh"
#include "nix/util/terminal.hh"
#include "nix/util/tests/test-data.hh"

#include <cstdint>

/* Jobs revert only where they have a region (unforce.cc): in builds without
   a garbage collector, on 64-bit Linux. Elsewhere these tests are left out. */
#if !NIX_USE_BOEHMGC && defined(__linux__) && SIZE_MAX > UINT32_MAX

namespace nix {

/* Defined in unforce.cc for these tests: forget that C API client code,
   which other tests in this process create, may run. */
void unforceTestResetNativeCode();

/* Defined in unforce.cc for these tests: while set, every mapping a job asks
   for fails. */
extern bool unforceTestFailMapping;

/* While alive, a job's region cannot be mapped (`unforceTestFailMapping`). */
struct FailMapping
{
    FailMapping()
    {
        unforceTestFailMapping = true;
    }

    ~FailMapping()
    {
        unforceTestFailMapping = false;
    }

    FailMapping(const FailMapping &) = delete;
    FailMapping & operator=(const FailMapping &) = delete;
};

class UnforceTest : public LibExprTest
{
protected:
    void SetUp() override
    {
        unforceTestResetNativeCode();
    }

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

    /* `v` printed as `nix eval` prints it, its elements forced in jobs if `jobs`. */
    std::string printed(Value & v, bool jobs)
    {
        state.forceValue(v, noPos);
        std::ostringstream out;
        std::optional<UnforceJobs> traversal;
        if (jobs)
            traversal.emplace(state, &v);
        printValue(state, out, v, PrintOptions{.force = true, .derivationPaths = true});
        return out.str();
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
    /* A function's position is its lambda's. */
    auto f = oldThunk("let x = 1; in y: x");
    state.beginUnforceScope();
    state.forceValue(*f, noPos);
    auto lambda = f->determinePos(noPos);
    state.endUnforceScope();
    ASSERT_TRUE(f->isThunk());
    EXPECT_EQ(f->determinePos(noPos), lambda);
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

/* An old cell whose force failed keeps its failure, as the job's other
   cells are reverted: read again, it throws the same error. */
TEST_F(UnforceTest, failureKept)
{
    auto v = oldThunk("{ a = 1; }");
    auto f = oldThunk("throw \"no\"");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_THROW(state.forceValue(*f, noPos), ThrownError);
    state.endUnforceScope();
    EXPECT_TRUE(v->isThunk());
    EXPECT_TRUE(f->isFailed());
    try {
        state.forceValue(*f, noPos);
        FAIL();
    } catch (ThrownError & e) {
        EXPECT_THAT(e.msg(), ::testing::HasSubstr("no"));
    }
}

/* A failure on max-call-depth in a job, here met from deeper than a first
   force that fitted, is retried by later reads at their own depth: an old
   cell, a young cell of a promoted owner and an application give their
   values when read again from less deep. */
TEST_F(UnforceTest, depthFailureRetried)
{
    evalSettings.maxCallDepth = 100;
    std::string f = "let f = n: v: if n == 0 then v else f (n - 1) v; in ";
    auto app = [&](std::string fun, Value * arg) {
        auto v = state.allocValue();
        v->mkApp(oldThunk(fun), arg);
        return v;
    };
    std::string d = "let d = n: x: if n == 0 then builtins.seq x x else d (n - 1) x; in ";
    auto c = oldThunk(f + "f 50 (builtins.genList (x: x) 3)");
    auto s = oldThunk(f + "{ c = f 50 (builtins.genList (x: x) 3); }");
    auto a = app(f + "f 50", c);
    state.beginUnforceScope();
    state.forceValue(*c, noPos);
    state.forceValue(*app("s: s.c", s), noPos);
    state.endUnforceScope();
    ASSERT_TRUE(c->isThunk() && s->isThunk());
    state.beginUnforceScope();
    for (auto x : {app(d + "d 90", c), app("s: " + d + "d 90 s.c", s), app(d + "d 90", a)})
        EXPECT_THROW(state.forceValue(*x, noPos), StackOverflowError);
    state.endUnforceScope();
    state.beginUnforceScope();
    for (auto x : {c, app("s: s.c", s), a}) {
        std::string out;
        EXPECT_NO_THROW(out = deep(*x));
        EXPECT_EQ(out, "[ 0 1 2 ]");
    }
    state.endUnforceScope();
}

/* A cell that an earlier job reverted keeps the value a later job gives
   it. */
TEST_F(UnforceTest, promotesRevertedCell)
{
    auto v = oldThunk("{ a = 1; }");
    for (int i = 0; i < 2; ++i) {
        state.beginUnforceScope();
        state.forceValue(*v, noPos);
        state.endUnforceScope();
    }
    ASSERT_EQ(v->type(), nAttrs);
    EXPECT_EQ(deep(*v), "{ a = 1; }");
}

/* A cell that a promoted value reaches is the cell its owner holds: the
   owner is promoted too. A root keeps a cell of `b`'s own, which promotes
   `b`, whose value reaches `a`'s cell `f`. */
TEST_F(UnforceTest, promotesOwner)
{
    auto a = oldThunk("{ f = x: x; }");
    auto b = state.allocValue();
    b->mkApp(oldThunk("a: builtins.attrValues a ++ [ (1 + 1) ]"), a);
    RootValue root;
    state.beginUnforceScope();
    state.forceValue(*b, noPos);
    root = allocRootValue(b->listView()[1]);
    state.endUnforceScope();
    ASSERT_EQ(a->type(), nAttrs);
    ASSERT_EQ(b->type(), nList);
    EXPECT_EQ(b->listView()[0], a->attrs()->get(createSymbol("f"))->value);
    EXPECT_EQ(*root, b->listView()[1]);
}

/* A cell whose value's young cell is forced after its force ended is
   reverted like any other: its re-force builds that cell again, unforced. */
TEST_F(UnforceTest, revertsConsumedOwner)
{
    auto v = oldThunk("{ a = 1 + 1; }");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    state.forceValue(*v->attrs()->get(createSymbol("a"))->value, noPos);
    state.endUnforceScope();
    EXPECT_TRUE(v->isThunk());
    EXPECT_EQ(deep(*v), "{ a = 2; }");
}

/* A seen set keeps the containers it holds, where they are (nothing moves),
   so that one seen again in a later job is found. */
TEST_F(UnforceTest, seenHoldsContainers)
{
    auto v = oldThunk("let s = { x = 1; }; in { a = s; b = s; }");
    auto junk = oldThunk("builtins.genList (x: { inherit x; }) 100");
    state.forceValue(*v, noPos);
    auto a = v->attrs()->get(createSymbol("a"))->value;
    auto b = v->attrs()->get(createSymbol("b"))->value;
    UnforceSeen seen;
    state.beginUnforceScope();
    state.forceValue(*junk, noPos);
    state.forceValue(*a, noPos);
    auto first = a->attrs();
    EXPECT_TRUE(seen.see(first, UnforceHolder::Kind::bindings));
    state.endUnforceScope();
    state.beginUnforceScope();
    state.forceValue(*b, noPos);
    EXPECT_EQ(b->attrs(), first);
    EXPECT_FALSE(seen.see(b->attrs(), UnforceHolder::Kind::bindings));
    state.endUnforceScope();
    EXPECT_EQ(deep(*b), "{ x = 1; }");
}

/* A traversal runs each element of its root in a job of its own: a
   printer prints what it prints without, «repeated» included, and the
   elements are thunks again afterwards. */
TEST_F(UnforceTest, printerJobs)
{
    auto expr = "let s = { x = 1; }; in { a = s; b = s; c = [ 1 2 3 ]; }";
    auto v = oldThunk(expr);
    EXPECT_EQ(printed(*v, true), printed(*oldThunk(expr), false));
    EXPECT_TRUE(v->attrs()->get(createSymbol("c"))->value->isThunk());
}

/* `getDerivations` finds each derivation in a job, drops one found in an
   earlier job, and the attributes of those it finds outlive the jobs. */
TEST_F(UnforceTest, getDerivationsJobs)
{
    auto v = oldThunk(
        "rec { x = derivation { name = \"x\"; system = \"s\"; builder = \"b\"; }; y = x; z = derivation { name = \"z\"; system = \"s\"; builder = \"b\"; outputs = [ \"dev\" ]; }; w = { a = 1; }; }");
    state.forceValue(*v, noPos);
    PackageInfos drvs;
    getDerivations(state, *v, "", Bindings::emptyBindings, drvs, false);
    EXPECT_TRUE(v->attrs()->get(createSymbol("w"))->value->isThunk());
    ASSERT_EQ(drvs.size(), 2);
    EXPECT_EQ(drvs.back().queryOutputName(), "dev");
}

/* The derivation that `getDerivation` hands to a callback keeps nothing of
   the job past the call: the job's other cells revert. */
TEST_F(UnforceTest, getDerivationCallback)
{
    auto v = oldThunk("derivation { name = \"x\"; system = \"s\"; builder = \"b\"; }");
    auto w = oldThunk("{ a = 1; }");
    std::string name;
    state.beginUnforceScope();
    state.forceValue(*w, noPos);
    EXPECT_TRUE(getDerivation(state, *v, false, [&](PackageInfo & drv) { name = drv.queryName(); }));
    state.endUnforceScope();
    EXPECT_EQ(name, "x");
    EXPECT_TRUE(w->isThunk());
}

/* Native code turns unforcing off: here, `importNative`. */
TEST_F(UnforceTest, nativeCodeTurnsOff)
{
    EXPECT_TRUE(unforceEnabled(state));
    evalSettings.enableNativeCode = true;
    EvalState other({}, state.store, fetchSettings, evalSettings, nullptr);
    EXPECT_FALSE(unforceEnabled(other));
}

/* So does a plugin's `extraPrimOps` entry. */
TEST_F(UnforceTest, extraPrimOpTurnsOff)
{
    evalSettings.extraPrimOps.push_back({
        .name = "__unforceTest",
        .arity = 1,
        .impl = [](EvalState &, CallSite, Value * const * args, Value & v) { v = *args[0]; },
    });
    EvalState other({}, state.store, fetchSettings, evalSettings, nullptr);
    EXPECT_FALSE(unforceEnabled(other));
}

/* And C API code: in a state a C API host built, or once a client's
   primop or external value exists, in any state. */
TEST_F(UnforceTest, cApiTurnsOff)
{
    EvalState other({}, state.store, fetchSettings, evalSettings, nullptr);
    unforceNoteCApiState(other);
    EXPECT_FALSE(unforceEnabled(other));
    EXPECT_TRUE(unforceEnabled(state));
    unforceNoteCApiCode();
    EXPECT_FALSE(unforceEnabled(state));
}

/* And a plugin's lookup path hook that changed the builtins, from then on. */
TEST_F(UnforceTest, builtinsChangeTurnsOff)
{
    auto & slot = state.baseEnv.values[state.staticBaseEnv->vars.begin()->second];
    {
        UnforceBuiltinsCheck check(state);
    }
    EXPECT_TRUE(unforceEnabled(state));
    {
        UnforceBuiltinsCheck check(state);
        slot = state.allocValue();
        slot->mkNull();
    }
    EXPECT_FALSE(unforceEnabled(state));
}

/* A job in which an effect ran outside any cell's force is kept whole. */
TEST_F(UnforceTest, effectKeepsJob)
{
    auto v = oldThunk("{ a = 1; }");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    unforceNoteEffect(state);
    state.endUnforceScope();
    EXPECT_FALSE(v->isThunk());
    EXPECT_EQ(deep(*v), "{ a = 1; }");
}

/* A trace keeps nothing: the cell whose evaluation showed it, here in the
   force of a young cell that `t` owns, reverts with the job's other cells,
   and so does a job in which one was shown outside any cell's force. */
TEST_F(UnforceTest, traceKeepsNothing)
{
    auto v = oldThunk("{ a = 1; }");
    auto t = oldThunk("let x = builtins.trace \"t\" { b = 2; }; in builtins.seq x { inherit x; }");
    auto f = oldThunk("x: builtins.trace x x");
    state.forceValue(*f, noPos);
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    state.forceValue(*t, noPos);
    Value r;
    state.callFunction(*f, *oldThunk("\"s\""), r, noPos);
    state.endUnforceScope();
    EXPECT_TRUE(v->isThunk());
    EXPECT_TRUE(t->isThunk());
}

/* A job in which a plugin's lookup path hook ran is kept whole. */
TEST_F(UnforceTest, lookupPathHookKeepsJob)
{
    evalSettings.lookupPathHooks.insert_or_assign(
        "unforce-test", [](EvalState &, std::string_view) -> std::optional<SourcePath> { return std::nullopt; });
    auto v = oldThunk("{ a = 1; }");
    auto r = oldThunk("builtins.foldl' (acc: i: acc + builtins.length (builtins.genList (j: j) 20)) 0 [ 1 2 3 ]");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_TRUE(unforceNoteLookupPathHook(evalSettings, "unforce-test"));
    /* Nor does a loop's step free anything from then on. */
    auto ends = unforceStats.stepEnds;
    state.forceValue(*r, noPos);
    EXPECT_EQ(unforceStats.stepEnds, ends);
    state.endUnforceScope();
    EXPECT_FALSE(v->isThunk());
}

/* A root made in a job (as the evaluation cache's cursors make them) keeps
   what it points to past the job's end, and with it its owner. */
TEST_F(UnforceTest, rootHoldsValue)
{
    auto v = oldThunk("{ a = { b = 1; }; }");
    RootValue root;
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    root = allocRootValue(v->attrs()->get(createSymbol("a"))->value);
    state.endUnforceScope();
    ASSERT_FALSE(v->isThunk());
    EXPECT_EQ(deep(**root), "{ b = 1; }");
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

/* A job keeps the memory of what it promotes up to the end of its last
   object, here the string a promoted cell holds: a later job allocates
   above it. */
TEST_F(UnforceTest, keptEndIsLive)
{
    auto v = oldThunk("let a = \"0123456789abcdef\"; b = \"0123456789abcde\"; in a + b");
    for (int i = 0; i < 2; ++i) {
        state.beginUnforceScope();
        state.forceValue(*v, noPos);
        state.endUnforceScope();
    }
    auto want = "0123456789abcdef0123456789abcde";
    EXPECT_THAT(*v, IsStringEq(want));
    state.beginUnforceScope();
    state.mem.allocBytes(64);
    state.endUnforceScope();
    EXPECT_THAT(*v, IsStringEq(want));
}

#  if NIX_ASAN_ENABLED
/* What a job's end frees is poisoned until a later job allocates it. */
TEST_F(UnforceTest, poisonsFreed)
{
    auto v = oldThunk("{ a = 1; }");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    auto freed = (const char *) state.mem.allocBytes(1024);
    state.endUnforceScope();
    ASSERT_TRUE(v->isThunk());
    EXPECT_TRUE(__asan_address_is_poisoned(freed));
    state.beginUnforceScope();
    auto again = (const char *) state.mem.allocBytes(1024);
    EXPECT_FALSE(__asan_address_is_poisoned(again));
    EXPECT_FALSE(__asan_address_is_poisoned(again + 1023));
    state.endUnforceScope();
}
#  endif

/* A job does not run without its region: where none can be mapped, opening
   the job runs out of memory, and leaves no job open. */
TEST_F(UnforceTest, unmappableRegionThrows)
{
    auto v = oldThunk("{ a = 1; }");
    {
        FailMapping fail;
        EXPECT_THROW(state.beginUnforceScope(), std::bad_alloc);
    }
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    state.endUnforceScope();
    EXPECT_TRUE(v->isThunk());
}

/* Not even where a traversal prints an element's error and goes on: that is
   no element's failure. */
TEST_F(UnforceTest, unmappableRegionNotSwallowed)
{
    auto v = oldThunk("{ a = 1; }");
    state.forceValue(*v, noPos);
    FailMapping fail;
    std::ostringstream out;
    UnforceJobs jobs(state, v);
    EXPECT_THROW(printValue(state, out, *v, PrintOptions{.force = true}), std::bad_alloc);
}

/* The cut is where the last force a promoted target owns ended. `before`
   ended earlier, so the time closure promotes it; `after` ended later, so
   it is reverted and its memory freed, which a later job allocates again
   zeroed, as all memory is. */
TEST_F(UnforceTest, timeClosure)
{
    auto before = oldThunk("{ x = 1; }");
    auto held = oldThunk("{ y = 2; }");
    auto after = oldThunk("builtins.genList (i: { inherit i; }) 100");
    UnforceSeen seen;
    auto freed = unforceStats.bytesFreed;
    state.beginUnforceScope();
    state.forceValue(*before, noPos);
    state.forceValue(*held, noPos);
    EXPECT_TRUE(seen.see(held->attrs(), UnforceHolder::Kind::bindings));
    state.forceValue(*after, noPos);
    state.endUnforceScope();
    EXPECT_FALSE(before->isThunk());
    EXPECT_FALSE(held->isThunk());
    EXPECT_TRUE(after->isThunk());
    freed = unforceStats.bytesFreed - freed;
    ASSERT_GT(freed, 0u);
    state.beginUnforceScope();
    auto p = (const char *) state.mem.allocBytes(freed);
    EXPECT_TRUE(std::all_of(p, p + freed, [](char c) { return c == 0; }));
    state.endUnforceScope();
    EXPECT_EQ(deep(*before), "{ x = 1; }");
    state.forceValue(*after, noPos);
    EXPECT_EQ(after->listSize(), 100u);
}

/* A value the driver allocated in the job, which a holder holds, is no
   target's: the job is kept whole. */
TEST_F(UnforceTest, driverHoldKeepsJob)
{
    auto v = oldThunk("{ a = 1; }");
    UnforceSeen seen;
    auto whole = unforceStats.jobsKeptWhole;
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    auto w = state.allocValue();
    w->mkInt(1);
    EXPECT_TRUE(seen.see(w, UnforceHolder::Kind::value));
    state.endUnforceScope();
    EXPECT_EQ(unforceStats.jobsKeptWhole, whole + 1);
    EXPECT_FALSE(v->isThunk());
    EXPECT_THAT(*w, IsIntEq(1));
}

/* An interrupted force leaves its cell failed with a recovery value the job
   allocated after the force ended. Nothing tells whether the failure
   outlives the job, so the job is kept whole, and a later read recovers:
   for an old cell, and for a young cell whose owner a holder keeps, after
   another job reused the memory a job freed. */
TEST_F(UnforceTest, interruptedKeepsJob)
{
    auto v = oldThunk("builtins.toJSON [ 1 2 3 ]");
    state.beginUnforceScope();
    setInterrupted(true);
    EXPECT_THROW(state.forceValue(*v, noPos), Interrupted);
    setInterrupted(false);
    state.endUnforceScope();
    ASSERT_TRUE(v->isFailed());
    state.forceValue(*v, noPos);
    EXPECT_THAT(*v, IsStringEq("[1,2,3]"));

    auto u = oldThunk("{ a = { b = builtins.toJSON [ 1 2 3 ]; }; }");
    UnforceSeen seen;
    state.beginUnforceScope();
    state.forceValue(*u, noPos);
    auto & a = *u->attrs()->get(createSymbol("a"))->value;
    state.forceValue(a, noPos);
    EXPECT_TRUE(seen.see(a.attrs(), UnforceHolder::Kind::bindings));
    auto & b = *a.attrs()->get(createSymbol("b"))->value;
    setInterrupted(true);
    EXPECT_THROW(state.forceValue(b, noPos), Interrupted);
    setInterrupted(false);
    state.endUnforceScope();
    state.beginUnforceScope();
    state.forceValue(*oldThunk("builtins.length (builtins.genList (i: { inherit i; }) 10000)"), noPos);
    state.endUnforceScope();
    ASSERT_TRUE(b.isFailed());
    state.forceValue(b, noPos);
    EXPECT_THAT(b, IsStringEq("[1,2,3]"));
}

/* Every failure a job made forgets the job's cells: here an infinite
   recursion found in a young cell, by a young cell that survives the job
   with its owner, and by an old cell, which keeps its failure. */
TEST_F(UnforceTest, keptFailureForgetsYoungCell)
{
    auto forgets = [](const Value & c) {
        ASSERT_TRUE(c.isFailed());
        try {
            std::rethrow_exception(c.failed().exRef->ex);
        } catch (InfiniteRecursionError & e) {
            EXPECT_EQ(e.v, nullptr);
        }
    };
    auto u = oldThunk("{ s = { r = let x = { a = x.a; }; in x.a; }; }");
    UnforceSeen seen;
    state.beginUnforceScope();
    state.forceValue(*u, noPos);
    auto & s = *u->attrs()->get(createSymbol("s"))->value;
    state.forceValue(s, noPos);
    EXPECT_TRUE(seen.see(s.attrs(), UnforceHolder::Kind::bindings));
    auto & r = *s.attrs()->get(createSymbol("r"))->value;
    EXPECT_THROW(state.forceValue(r, noPos), InfiniteRecursionError);
    state.endUnforceScope();
    forgets(r);

    auto o = oldThunk("let x = { a = x.a; }; in x.a");
    state.beginUnforceScope();
    EXPECT_THROW(state.forceValue(*o, noPos), InfiniteRecursionError);
    state.endUnforceScope();
    forgets(*o);
}

/* A failure of an old cell, forced in one job, stays a failure at the
   job's end: read in the next job, it throws the first force's error
   again, with that force's frames ("while evaluating the attribute 'c'"
   where `a` selected it). */
TEST_F(UnforceTest, failureKeepsFirstTrace)
{
    std::string expr = "let s = { c = builtins.throw \"boom\"; }; in { inherit s; a = builtins.tryEval s.c; b = s.c; }";
    uint32_t column = expr.find("s.c; b") + 1;
    auto v = oldThunk(expr);
    state.forceValue(*v, noPos);
    /* `s` and its cell `c` are old. */
    auto & sv = *v->attrs()->get(createSymbol("s"))->value;
    state.forceValue(sv, noPos);
    auto & c = *sv.attrs()->get(createSymbol("c"))->value;
    auto & a = *v->attrs()->get(createSymbol("a"))->value;
    auto & b = *v->attrs()->get(createSymbol("b"))->value;
    state.beginUnforceScope();
    state.forceValue(a, noPos);
    state.endUnforceScope();
    ASSERT_TRUE(c.isFailed());
    state.beginUnforceScope();
    bool first = false;
    try {
        state.forceValue(b, noPos);
        ADD_FAILURE() << "b did not throw";
    } catch (EvalError & e) {
        for (auto & t : e.info().traces)
            first = first || (t.pos && t.pos->column == column);
        EXPECT_NE(e.info().msg.str().find("boom"), std::string::npos);
    }
    state.endUnforceScope();
    EXPECT_TRUE(first);
}

/* `foldl'`'s steps, in one force in a job. Each case's `r` is forced after
   its list `xs`, made in the job, or before it (`old`). Then, once the job
   has allocated over what the steps freed, as zeroed memory, `c` prints as
   without jobs. Where the steps free all they allocate (`frees`), the region
   grows by no more for a list twice as long but by the two cells the steps
   take turns writing. */
TEST_F(UnforceTest, steps)
{
    struct Case
    {
        const char *xs, *r, *c;
        bool old, frees;
    };

    auto ints = "builtins.genList (i: i) n", sets = "builtins.genList (i: { a = i; }) n";
    auto forcedInts = "let l = builtins.genList (i: i) n; in builtins.deepSeq l l",
         forcedLists = "let l = builtins.genList (i: [ i ]) n; in builtins.deepSeq l l";
    auto run = [&](const Case & k, int n) {
        auto program =
            fmt("let n = %d; s = { z = 1; }; xs = %s; r = %s; in { inherit xs r; c = %s; }", n, k.xs, k.r, k.c);
        auto want = deep(*oldThunk(fmt("(%s).c", program)));
        auto v = oldThunk(program);
        state.forceValue(*v, noPos);
        auto attr = [&](const char * name) -> Value & { return *v->attrs()->get(createSymbol(name))->value; };
        if (k.old)
            state.forceValue(attr("xs"), noPos);
        state.beginUnforceScope();
        state.forceValue(attr("xs"), noPos);
        auto cur = evalRegion.cur;
        state.forceValue(attr("r"), noPos);
        size_t grew = evalRegion.cur - cur;
        auto p = (const char *) state.mem.allocBytes(4096);
        EXPECT_TRUE(std::all_of(p, p + 4096, [](char c) { return c == 0; })) << k.r;
        EXPECT_EQ(deep(attr("c")), want) << k.r;
        state.endUnforceScope();
        return grew;
    };
    const Case cases[] = {
        /* Each step's list is freed. */
        {ints, "builtins.foldl' (acc: x: acc + builtins.length (builtins.genList (j: j) 20)) 0 xs", "r", false, true},
        /* Each step's value points into its memory. */
        {ints,
         "builtins.foldl' (acc: x: { n = acc.n + x; prev = acc; }) { n = 0; prev = null; } xs",
         "let count = p: if p == null then 0 else 1 + count p.prev; in [ r.n (count r) ]",
         false,
         false},
        /* Each step's value holds the cell it read: no later step writes it. */
        {ints, "builtins.foldl' (acc: x: [ acc ]) null xs", "r", false, false},
        /* Each step's value holds no cell of the loop's. */
        {ints, "builtins.foldl' (acc: x: [ x ]) null xs", "r", false, true},
        /* A step forces a young cell made before the loop (a write), or an old one (logged). */
        {sets, "builtins.foldl' (acc: x: acc + x.a) 0 xs", "[ r xs ]", false, false},
        {sets, "builtins.foldl' (acc: x: acc + x.a) 0 xs", "[ r xs ]", true, false},
        /* Loops in a loop's steps: the inner steps free, and an inner step's write is the outer step's. */
        {ints,
         "builtins.foldl' (acc: x: acc + builtins.foldl' (a: j: a + builtins.length (builtins.genList (k: k) j)) 0 [ 1 2 3 ]) 0 xs",
         "r",
         false,
         true},
        {sets,
         "builtins.foldl' (acc: x: acc + builtins.foldl' (a: j: a + x.a) 0 [ 1 2 ]) 0 xs",
         "[ r xs ]",
         false,
         false},
        /* A failure caught in a step keeps that step; the others free. */
        {ints,
         "builtins.foldl' (acc: x: acc + (if (builtins.tryEval (if x == 3 then throw \"x\" else x)).success then 1 else 0)) 0 xs",
         "r",
         false,
         true},
        {ints,
         "builtins.foldl' (acc: x: acc + (if (builtins.tryEval (builtins.foldl' (a: j: if j == 2 then throw \"x\" else a + j) 0 [ 1 2 3 ])).success then 1 else 0)) 0 xs",
         "r",
         false,
         false},
        /* A step's value is a cell shared outside the loop. */
        {ints, "builtins.foldl' (acc: x: if x == 5 then s else acc) null xs", "[ r s ]", false, true},
        /* Steps that allocate nothing take turns writing two cells. */
        {forcedInts, "builtins.foldl' builtins.add 0 xs", "r", false, true},
        /* Steps whose inner steps free all they allocate: the value holds
           the cell the step read, which is not written again, or not. */
        {forcedLists, "builtins.foldl' (builtins.foldl' (a: x: [ a ])) null xs", "r", false, false},
        {forcedLists, "builtins.foldl' (builtins.foldl' (a: x: a + x)) 0 xs", "r", false, true},
    };
    for (auto & k : cases) {
        auto grew = run(k, 100);
        if (k.frees)
            EXPECT_LE(run(k, 200), grew + 2 * sizeof(Value)) << k.r;
    }
}

/* A value promoted at a job's end, built after steps freed memory, stays as
   it was while a later job reuses the region. */
TEST_F(UnforceTest, stepsThenPromote)
{
    auto v = oldThunk(
        "{ r = let n = builtins.foldl' (acc: i: acc + builtins.length (builtins.genList (j: j) 20)) 0 "
        "(builtins.genList (i: i) 50); in { inherit n; l = builtins.genList (k: { x = k + n; }) 50; }; }");
    state.forceValue(*v, noPos);
    auto & r = *v->attrs()->get(createSymbol("r"))->value;
    UnforceSeen seen;
    state.beginUnforceScope();
    auto ends = unforceStats.stepEnds;
    state.forceValueDeep(r);
    EXPECT_GT(unforceStats.stepEnds, ends);
    EXPECT_TRUE(seen.see(r.attrs(), UnforceHolder::Kind::bindings));
    state.endUnforceScope();
    EXPECT_FALSE(r.isThunk());
    auto big = oldThunk("builtins.length (builtins.genList (i: { inherit i; }) 10000)");
    state.beginUnforceScope();
    state.forceValue(*big, noPos);
    state.endUnforceScope();
    std::string expected = "{ l = [ ";
    for (int k = 0; k < 50; ++k)
        expected += "{ x = " + std::to_string(k + 1000) + "; } ";
    expected += "]; n = 1000; }";
    EXPECT_EQ(printValue(state, r), expected);
}

/* A step in which a failure was caught is kept: the job's end reads the
   failure's cell. */
TEST_F(UnforceTest, failedStepKept)
{
    auto v =
        oldThunk("builtins.foldl' (acc: x: acc + (if (builtins.tryEval (throw \"x\")).success then 1 else 0)) 0 [ 1 ]");
    state.beginUnforceScope();
    auto ends = unforceStats.stepEnds;
    state.forceValue(*v, noPos);
    EXPECT_EQ(unforceStats.stepEnds, ends);
    state.endUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_THAT(*v, IsIntEq(0));
}

/* A step frees what an old cell's force allocated in it (x's value is k's
   list, made before the step, so that x stays a target), and the owner
   timeline forgets that force: what y builds there after the loop is y's,
   so that a holder of it promotes y. */
TEST_F(UnforceTest, stepsCutOwners)
{
    auto v = oldThunk(
        "rec { k = builtins.genList (i: i) 3; x = builtins.seq (builtins.genList (j: j) 20) k; "
        "y = let n = builtins.seq k (builtins.foldl' builtins.seq null [ x ]); r = { s = { z = n; }; }; "
        "in builtins.seq n (builtins.seq r.s r); }");
    state.forceValue(*v, noPos);
    auto & y = *v->attrs()->get(createSymbol("y"))->value;
    UnforceSeen seen;
    state.beginUnforceScope();
    auto ends = unforceStats.stepEnds;
    state.forceValue(y, noPos);
    EXPECT_GT(unforceStats.stepEnds, ends);
    auto & s = *y.attrs()->get(createSymbol("s"))->value;
    EXPECT_TRUE(seen.see(s.attrs(), UnforceHolder::Kind::bindings));
    state.endUnforceScope();
    EXPECT_FALSE(y.isThunk());
}

/* A holder that may hold a step's memory (`youngTop`) keeps every step's. */
TEST_F(UnforceTest, holderBlocksSteps)
{
    auto r = oldThunk("builtins.foldl' (acc: i: acc + builtins.length (builtins.genList (j: j) 20)) 0 [ 1 2 3 ]");
    UnforceSeen seen;
    state.beginUnforceScope();
    seen.youngTop = (const void *) UINTPTR_MAX;
    auto ends = unforceStats.stepEnds;
    state.forceValue(*r, noPos);
    EXPECT_EQ(unforceStats.stepEnds, ends);
    EXPECT_THAT(*r, IsIntEq(60));
    state.endUnforceScope();
}

/* A frame entered in the job holds a thunk `slot`, which the first step of
   `f`'s loop forces: the slot's value lies in that step's memory, which the
   step keeps, however much is allocated after it. */
[[gnu::noinline]] static void forceThroughSlot(EvalState & state, Value & f)
{
    Value slot;
    slot.mkThunk(&state.baseEnv, state.parseExprFromString("{ y = 1; }", state.rootPath(CanonPath::root)));
    auto w = state.allocValue();
    w->mkApp(&f, &slot);
    auto cur = evalRegion.cur;
    state.forceValue(*w, noPos);
    ASSERT_EQ(slot.type(), nAttrs);
    EXPECT_GT(evalRegion.cur, (const char *) slot.attrs());
    state.mem.allocBytes(evalRegion.cur - cur);
    EXPECT_EQ(printValue(state, slot), "{ y = 1; }");
}

TEST_F(UnforceTest, stepsKeepSlot)
{
    auto f = oldThunk("x: builtins.foldl' (a: i: builtins.seq x (a + i)) 0 [ 1 2 3 ]");
    state.forceValue(*f, noPos);
    state.beginUnforceScope();
    forceThroughSlot(state, *f);
    state.endUnforceScope();
}

/* A step's value that reaches the step's memory through one pointer of one
   kind: a partly applied primop's operand, a curried function's
   environment, a two-element list's second element. The steps keep it. */
TEST_F(UnforceTest, stepsEscapeEdgeKinds)
{
    for (auto [step, use, want] : std::initializer_list<std::tuple<const char *, const char *, int>>{
             {"acc: i: builtins.add (builtins.length (builtins.genList (j: j) (i * 10)))", "r 1", 31},
             {"acc: i: let big = builtins.genList (j: j) (i * 10); in y: builtins.length big + y", "r 1", 31},
             {"acc: i: [ 0 (builtins.genList (j: j) (i * 10)) ]", "builtins.length (builtins.elemAt r 1)", 30},
         }) {
        auto v = oldThunk(fmt("rec { r = builtins.foldl' (%s) null [ 1 2 3 ]; u = %s; }", step, use));
        state.beginUnforceScope();
        state.forceValue(*v, noPos);
        state.forceValue(*v->attrs()->get(createSymbol("r"))->value, noPos);
        state.mem.allocBytes(4096);
        auto & u = *v->attrs()->get(createSymbol("u"))->value;
        state.forceValue(u, noPos);
        EXPECT_THAT(u, IsIntEq(want)) << step;
        state.endUnforceScope();
    }
}

/* The front ends' traversals, as the commands run them. */
static const std::vector<std::pair<std::string, std::function<std::string(EvalState &, Value &)>>> frontEnds = {
    {"nix-instantiate --eval --strict",
     [](EvalState & state, Value & v) {
         state.forceValueDeep(v, true);
         std::set<const void *> seen;
         std::ostringstream out;
         printAmbiguous(state, v, out, &seen);
         return out.str();
     }},
    {"nix-instantiate --eval --strict --xml",
     [](EvalState & state, Value & v) {
         NixStringContext context;
         std::ostringstream out;
         UnforceJobs jobs(state, &v);
         printValueAsXML(state, true, false, v, out, context, noPos);
         return out.str();
     }},
    {"nix eval --json",
     [](EvalState & state, Value & v) {
         NixStringContext context;
         std::ostringstream out;
         UnforceJobs jobs(state, &v);
         printValueAsJSON(state, true, v, noPos, out, context, false);
         return out.str();
     }},
    {"nix eval",
     [](EvalState & state, Value & v) {
         std::ostringstream out;
         UnforceJobs jobs(state, &v);
         printValue(state, out, v, PrintOptions{.force = true, .derivationPaths = true});
         return out.str();
     }},
    {"nix-instantiate",
     [](EvalState & state, Value & v) {
         PackageInfos drvs;
         getDerivations(state, v, "", Bindings::emptyBindings, drvs, false);
         std::ostringstream out;
         UnforceJobs jobs(state);
         for (auto & i : drvs) {
             jobs.element();
             out << state.store->printStorePath(i.requireDrvPath()) << "!" << i.queryOutputName() << "\n";
         }
         return out.str();
     }},
};

/* The end of the value printed at `j` in `s`: a bracketed one, a string, a
   «…» marker, or a word; npos if it does not end. */
static size_t endOfPrinted(std::string_view s, size_t j)
{
    if (j >= s.size())
        return std::string_view::npos;
    if (s.substr(j).starts_with("«")) {
        auto e = s.find("»", j);
        return e == std::string_view::npos ? e : e + std::string_view("»").size();
    }
    if (s[j] == '"') {
        for (++j; j < s.size(); ++j)
            if (s[j] == '\\')
                ++j;
            else if (s[j] == '"')
                return j + 1;
        return std::string_view::npos;
    }
    if (s[j] == '{' || s[j] == '[' || s[j] == '(') {
        int depth = 0;
        while (j < s.size()) {
            if (s[j] == '"') {
                j = endOfPrinted(s, j);
                if (j == std::string_view::npos)
                    return j;
                continue;
            }
            if (s[j] == '{' || s[j] == '[' || s[j] == '(')
                ++depth;
            else if ((s[j] == '}' || s[j] == ']' || s[j] == ')') && --depth == 0)
                return j + 1;
            ++j;
        }
        return std::string_view::npos;
    }
    auto e = s.find_first_of(" ;]})'", j);
    return e == std::string_view::npos ? s.size() : e;
}

/* Whether `got` prints what `want` prints, but that inside an error message
   it may print a value where `want` prints «thunk»: a failed young cell
   inside a reverted owner is built again, and its re-force fails later, its
   message showing what is evaluated by then (the exactness contract allows
   that much). An old failed cell keeps its failure. */
static bool printsAsOrMoreEvaluated(std::string want, std::string got)
{
    want = filterANSIEscapes(want, true);
    got = filterANSIEscapes(got, true);
    const std::string_view thunk = "«thunk»", open = "«", close = "»", error = "«error: ";
    /* The nesting of «…» in `want`, and the depth of the error message it
       is in (0: all of it; -1: none). */
    int depth = 0, inError = want.starts_with("error: ") ? 0 : -1;
    size_t i = 0, j = 0;
    while (i < want.size()) {
        auto rest = std::string_view(want).substr(i);
        if (inError >= 0 && rest.starts_with(thunk)) {
            i += thunk.size();
            j = endOfPrinted(got, j);
            if (j == std::string_view::npos)
                return false;
            continue;
        }
        if (rest.starts_with(open)) {
            ++depth;
            if (inError < 0 && rest.starts_with(error))
                inError = depth;
        } else if (rest.starts_with(close)) {
            if (depth == inError)
                inError = -1;
            --depth;
        }
        if (j >= got.size() || got[j] != want[i])
            return false;
        ++i;
        ++j;
    }
    return j == got.size();
}

/* The corpus, the .nix files in data/unforce (a function is called with
   its defaults): each program, as each front end evaluates it, prints
   what it prints in an evaluator without unforcing, or fails with the same
   error at the same position (its trace may differ: a failed young cell
   inside a reverted owner is built again, and its re-force throws again
   under other frames), but that a failure's message may show more of what
   is evaluated
   (`printsAsOrMoreEvaluated`). In an evaluator of its own each time, so
   that nothing is forced before. */
TEST_F(UnforceTest, corpus)
{
    std::vector<std::filesystem::path> programs;
    for (auto & entry : std::filesystem::directory_iterator(getUnitTestData() / "unforce"))
        if (entry.path().extension() == ".nix")
            programs.push_back(entry.path());
    std::ranges::sort(programs);
    ASSERT_FALSE(programs.empty());
    for (auto & program : programs) {
        uint64_t jobs = 0;
        for (auto & [frontEnd, run] : frontEnds) {
            SCOPED_TRACE(program.filename().string() + ", " + frontEnd);
            auto output = [&](bool unforce) {
                EvalState fresh({}, store, fetchSettings, evalSettings, nullptr);
                if (!unforce)
                    fresh.unforceNativeCodeReason = "a test's evaluator without unforcing";
                auto before = unforceStats.jobs;
                std::string out;
                try {
                    Value f, v;
                    fresh.evalFile(fresh.rootPath(CanonPath(program.string())), f);
                    fresh.autoCallFunction(Bindings::emptyBindings, f, v);
                    fresh.forceValue(v, noPos);
                    out = run(fresh, v);
                } catch (Error & e) {
                    std::ostringstream pos;
                    if (auto p = e.info().pos)
                        pos << *p;
                    out = "error: " + e.message() + " at " + pos.str();
                }
                if (unforce)
                    jobs += unforceStats.jobs - before;
                return out;
            };
            auto got = output(true), want = output(false);
            EXPECT_TRUE(printsAsOrMoreEvaluated(want, got)) << "with unforcing:    " << got << "\nwithout unforcing: " << want;
        }
        EXPECT_GT(jobs, 0) << program;
    }
}

} // namespace nix

#endif
