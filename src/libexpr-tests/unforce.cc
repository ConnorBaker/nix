#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/expr/print.hh"
#include "nix/expr/print-ambiguous.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/value-to-xml.hh"
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

/* A failed cell is reverted, as the job's other cells are: forced again,
   it throws the same error. */
TEST_F(UnforceTest, failureReverts)
{
    auto v = oldThunk("{ a = 1; }");
    auto f = oldThunk("throw \"no\"");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_THROW(state.forceValue(*f, noPos), ThrownError);
    state.endUnforceScope();
    EXPECT_TRUE(v->isThunk());
    EXPECT_TRUE(f->isThunk());
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
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_TRUE(unforceNoteLookupPathHook(evalSettings, "unforce-test"));
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
   the job runs out of memory, and leaves no job open. A job first maps all
   it needs but the region, which the next job maps again. */
TEST_F(UnforceTest, unmappableRegionThrows)
{
    auto v = oldThunk("{ a = 1; }");
    state.beginUnforceScope();
    state.endUnforceScope();
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
    state.beginUnforceScope();
    state.endUnforceScope();
    state.forceValue(*v, noPos);
    FailMapping fail;
    std::ostringstream out;
    UnforceJobs jobs(state, v);
    EXPECT_THROW(printValue(state, out, *v, PrintOptions{.force = true}), std::bad_alloc);
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
   it may print a value where `want` prints «thunk»: a failed cell is
   reverted, and its re-force fails later, its message showing what is
   evaluated by then (the exactness contract allows that much). */
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
   error at the same position (its trace may differ: a failed cell is
   reverted, and its re-force throws again under other frames), but that a
   failure's message may show more of what is evaluated
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
