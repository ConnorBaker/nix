#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/expr/print.hh"
#include "nix/expr/print-ambiguous.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/value-to-xml.hh"
#include "nix/util/finally.hh"
#include "nix/util/tests/test-data.hh"

#include <cstdint>

/* Jobs revert only where they have a region (unforce.cc): in builds without
   a garbage collector, on 64-bit Linux. Elsewhere these tests are left out. */
#if !NIX_USE_BOEHMGC && defined(__linux__) && SIZE_MAX > UINT32_MAX

#  include <pthread.h>

namespace nix {

/* Defined in unforce.cc for these tests: the most native stack that
   re-forces on this thread held as one entered a segment, and the thread's
   stack size. */
std::pair<size_t, size_t> unforceTestReforceStack();

/* Defined in unforce.cc for these tests: forget that C API client code,
   which other tests in this process create, may run. */
void unforceTestResetNativeCode();

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

/* Re-forces nested in re-forces hold no more native stack together than
   the thread's: here, a chain of 100 cells, each first forced alone and
   re-forced 100 calls deep in the next, on a thread with a stack of 1 MiB,
   which the chain would hold several times over. */
TEST_F(UnforceTest, reforceChainBound)
{
    auto v = oldThunk(
        "let step = n: x: if n == 0 then builtins.seq x { inherit x; } else step (n - 1) x; "
        "c = builtins.genList (i: step 100 (if i == 0 then null else builtins.elemAt c (i - 1))) 100; in c");
    state.forceValue(*v, noPos);
    auto & last = *v->listView()[99];
    bool reverted = false;
    std::pair<size_t, size_t> held;
    auto body = [&] {
        try {
            state.beginUnforceScope();
            for (auto e : v->listView())
                state.forceValue(*e, noPos);
            state.endUnforceScope();
            reverted = last.isThunk();
            state.beginUnforceScope();
            state.forceValue(last, noPos);
            state.endUnforceScope();
            held = unforceTestReforceStack();
        } catch (std::exception & e) {
            ADD_FAILURE() << e.what();
        }
    };
    using Body = decltype(body);
    pthread_attr_t attr;
    pthread_t thread;
    ASSERT_EQ(pthread_attr_init(&attr), 0);
    ASSERT_EQ(pthread_attr_setstacksize(&attr, 1 << 20), 0);
    auto run = [](void * body) -> void * {
        (*static_cast<Body *>(body))();
        return nullptr;
    };
    ASSERT_EQ(pthread_create(&thread, &attr, run, &body), 0);
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);
    ASSERT_TRUE(reverted);
    /* Some re-force ran on a segment. */
    EXPECT_GT(held.first, 0);
    EXPECT_LE(held.first, held.second);
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

/* A failed cell stays failed, with its error; the job's other cells are
   reverted. */
TEST_F(UnforceTest, failureStaysFailed)
{
    auto v = oldThunk("{ a = 1; }");
    auto f = oldThunk("throw \"no\"");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_THROW(state.forceValue(*f, noPos), ThrownError);
    state.endUnforceScope();
    EXPECT_TRUE(v->isThunk());
    ASSERT_TRUE(f->isFailed());
    try {
        state.forceValue(*f, noPos);
        FAIL();
    } catch (ThrownError & e) {
        EXPECT_THAT(e.msg(), ::testing::HasSubstr("no"));
    }
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
   owner is promoted too. `b`'s value has a failed cell of its own, which
   promotes it, and `a`'s cell `f`. */
TEST_F(UnforceTest, promotesOwner)
{
    auto a = oldThunk("{ f = x: x; }");
    auto b = state.allocValue();
    b->mkApp(oldThunk("a: builtins.attrValues a ++ [ (throw \"no\") ]"), a);
    state.beginUnforceScope();
    state.forceValue(*b, noPos);
    EXPECT_THROW(state.forceValue(*b->listView()[1], noPos), ThrownError);
    state.endUnforceScope();
    ASSERT_EQ(a->type(), nAttrs);
    ASSERT_EQ(b->type(), nList);
    EXPECT_EQ(b->listView()[0], a->attrs()->get(createSymbol("f"))->value);
    EXPECT_TRUE(b->listView()[1]->isFailed());
}

/* A promoted value keeps what it reaches, moved down over what the job
   freed (`junk`), and later jobs reuse the memory freed. */
TEST_F(UnforceTest, compactsPromoted)
{
    auto v = oldThunk(
        "let junk = builtins.genList (x: { inherit x; }) 100; in { a = builtins.length (builtins.deepSeq junk junk); b = [ 1 2 3 ]; }");
    for (int i = 0; i < 2; ++i) {
        state.beginUnforceScope();
        state.forceValue(*v, noPos);
        state.forceValue(*v->attrs()->get(createSymbol("a"))->value, noPos);
        state.forceValueDeep(*v->attrs()->get(createSymbol("b"))->value);
        state.endUnforceScope();
    }
    ASSERT_EQ(v->type(), nAttrs);
    for (int i = 0; i < 3; ++i) {
        auto w = oldThunk("builtins.genList (x: { y = x; }) 200");
        state.beginUnforceScope();
        state.forceValueDeep(*w);
        state.endUnforceScope();
    }
    EXPECT_EQ(deep(*v), "{ a = 100; b = [ 1 2 3 ]; }");
}

/* A cell whose value's young cell is forced after its force ended keeps
   its value: reverted, it would rebuild that cell unforced. */
TEST_F(UnforceTest, keepsConsumedOwner)
{
    auto v = oldThunk("{ a = 1 + 1; }");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    state.forceValue(*v->attrs()->get(createSymbol("a"))->value, noPos);
    state.endUnforceScope();
    EXPECT_FALSE(v->isThunk());
    EXPECT_EQ(deep(*v), "{ a = 2; }");
}

/* A seen set keeps the containers it holds, moved by the job's end, so
   that one seen again in a later job is found. */
TEST_F(UnforceTest, seenHoldsContainers)
{
    auto v = oldThunk(
        "let s = builtins.seq (builtins.length (builtins.genList (x: { inherit x; }) 100)) { x = 1; }; in { a = s; b = s; }");
    state.forceValue(*v, noPos);
    auto a = v->attrs()->get(createSymbol("a"))->value;
    auto b = v->attrs()->get(createSymbol("b"))->value;
    UnforceSeen seen;
    state.beginUnforceScope();
    state.forceValue(*a, noPos);
    auto first = a->attrs();
    EXPECT_TRUE(seen.see(first, UnforceHolder::Kind::bindings));
    state.endUnforceScope();
    state.beginUnforceScope();
    state.forceValue(*b, noPos);
    EXPECT_NE(b->attrs(), first);
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

/* A job in which an effect ran is kept whole. */
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

/* A trace keeps the value of the cell whose evaluation showed it, here in
   the force of a young cell that `t` owns; the job's other cells revert. */
TEST_F(UnforceTest, traceKeepsItsCell)
{
    auto v = oldThunk("{ a = 1; }");
    auto t = oldThunk("let x = builtins.trace \"t\" { b = 2; }; in builtins.seq x { inherit x; }");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    state.forceValue(*t, noPos);
    state.endUnforceScope();
    EXPECT_TRUE(v->isThunk());
    EXPECT_FALSE(t->isThunk());
}

/* Shown outside any cell's force, it keeps the job whole. */
TEST_F(UnforceTest, driverTraceKeepsJob)
{
    auto v = oldThunk("{ a = 1; }");
    auto f = oldThunk("x: builtins.trace x x");
    state.forceValue(*f, noPos);
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    Value r;
    state.callFunction(*f, *oldThunk("\"s\""), r, noPos);
    state.endUnforceScope();
    EXPECT_FALSE(v->isThunk());
}

/* So is one in which a plugin's lookup path hook ran. */
TEST_F(UnforceTest, lookupPathHookKeepsJob)
{
    evalSettings.lookupPathHooks.insert_or_assign(
        "unforce-test", [](EvalState &, std::string_view) -> std::optional<SourcePath> { return std::nullopt; });
    auto v = oldThunk("{ a = 1; }");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_TRUE(unforceNoteLookupPathHook(evalSettings, "unforce-test"));
    state.endUnforceScope();
    EXPECT_FALSE(v->isThunk());
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

/* The corpus, the .nix files in data/unforce (a function is called with
   its defaults): each program, as each front end evaluates it, prints
   what it prints in an evaluator without unforcing, or fails as it fails
   there, traces included. In an evaluator of its own each time, so that
   nothing is forced before. */
TEST_F(UnforceTest, corpus)
{
    auto showTrace = loggerSettings.showTrace.get();
    loggerSettings.showTrace = true;
    Finally restore([&] { loggerSettings.showTrace = showTrace; });
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
                    out = std::string("error: ") + e.what();
                }
                if (unforce)
                    jobs += unforceStats.jobs - before;
                return out;
            };
            EXPECT_EQ(output(true), output(false));
        }
        EXPECT_GT(jobs, 0) << program;
    }
}

} // namespace nix

#endif
