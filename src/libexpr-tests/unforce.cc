#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/get-drvs.hh"
#include "nix/expr/print.hh"
#include "nix/expr/print-ambiguous.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/value-to-xml.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/serialise.hh"
#include "nix/util/signals.hh"
#include "nix/util/terminal.hh"
#include "nix/util/tests/test-data.hh"

#include <cinttypes>
#include <cstdint>
#include <fstream>

/* Jobs revert only where they have a region (unforce.cc): in builds without
   a garbage collector, on 64-bit Linux. Elsewhere these tests are left out. */
#if !NIX_USE_BOEHMGC && defined(__linux__) && SIZE_MAX > UINT32_MAX

#  include <sys/mman.h>
#  include <unistd.h>

namespace nix {

/* Defined in unforce.cc for these tests: forget that C API client code,
   which other tests in this process create, may run, and the memory in use
   they left, so that each test's traversals start as a process's would. */
void unforceTestReset();

/* Defined in unforce.cc for these tests: while set, every mapping a job asks
   for fails. */
extern bool unforceTestFailMapping;

/* Defined in unforce.cc for these tests: the length at which a job's undo
   log is full. */
extern uint32_t unforceTestLogLimit;

class UnforceTest : public LibExprTest
{
protected:
    /* A store that paths can be added to; `restricted`: restrict-eval. */
    UnforceTest(bool restricted = false)
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}), [&](bool & readOnlyMode) {
            EvalSettings settings{readOnlyMode};
            settings.nixPath = {};
            settings.restrictEval = restricted;
            return settings;
        })
    {
    }

    /* A valid store path, a file holding `contents`. */
    std::string storeFile(std::string contents)
    {
        StringSource s{contents};
        return state.store->printStorePath(state.store->addToStoreFromDump(
            s, "w12", FileSerialisationMethod::Flat, ContentAddressMethod::Raw::Text, HashAlgorithm::SHA256));
    }

    /* `v` printed deep in a job. */
    std::string afterJob(Value & v)
    {
        state.beginUnforceScope();
        auto first = deep(v);
        state.endUnforceScope();
        return first;
    }

    void SetUp() override
    {
        unforceTestReset();
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
            traversal.emplace(state);
        printValue(state, out, v, PrintOptions{.force = true, .derivationPaths = true, .unforceJobs = jobs});
        return out.str();
    }
};

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
   it; with unforce-always-revert, that job reverts it again. */
TEST_F(UnforceTest, promotesRevertedCell)
{
    for (bool always : {false, true}) {
        evalSettings.unforceAlwaysRevert = always;
        auto v = oldThunk("{ a = 1; }");
        for (int i = 0; i < 2; ++i) {
            state.beginUnforceScope();
            state.forceValue(*v, noPos);
            state.endUnforceScope();
        }
        EXPECT_EQ(v->isThunk(), always);
        EXPECT_EQ(deep(*v), "{ a = 1; }");
    }
}

/* A cell that a promoted value reaches is the cell its owner holds: the
   owner is promoted too. A root keeps a cell of `b`'s own, which promotes
   `b`, whose value reaches `a`'s cell `f`; also with unforce-always-revert,
   which changes only what becomes of a cell an earlier job reverted. */
TEST_F(UnforceTest, promotesOwner)
{
    for (bool always : {false, true}) {
        evalSettings.unforceAlwaysRevert = always;
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
}

/* A traversal runs each element of its root in a job of its own: a printer
   prints what it prints without, «repeated» included, and the elements are
   thunks again afterwards. A reverted set (`s`, in `a`'s job) is found again
   once a later element forces its cell again (`b`), not a set the next job
   builds where it lay; a kept one (`u`, which promotes `a` and `s`) stays
   found, and so does a re-forced application's (`map`'s). */
TEST_F(UnforceTest, printerJobs)
{
    auto attr = [&](const char * expr, const char * name) {
        auto v = oldThunk(expr);
        EXPECT_EQ(printed(*v, true), printed(*oldThunk(expr), false)) << expr;
        return v->attrs()->get(createSymbol(name))->value;
    };
    EXPECT_TRUE(attr("let s = { x = 1; }; in { a = s; b = s; c = [ 1 2 3 ]; }", "c")->isThunk());
    EXPECT_TRUE(attr("let s = { x = 1; }; in { a = s; b = { y = 2; }; }", "a")->isThunk());
    attr("let s = { x = 1; }; in { a = { t = s; u = { z = 1; }; }; b = s; }", "b");
    attr("let l = map (n: { inherit n; }) [ 1 2 ]; in builtins.seq (builtins.length l) { a = builtins.head l; b = builtins.head l; c = builtins.elemAt l 1; }", "a");
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

/* Native code turns unforcing off: `importNative`; a plugin's
   `extraPrimOps` entry; a plugin's lookup path hook that changed the
   builtins, from then on; C API code, in a state a C API host built, or
   once a client's primop or external value exists, in any state (last: it
   holds for the process). */
TEST_F(UnforceTest, nativeCodeTurnsOff)
{
    auto fresh = [&] {
        return std::make_unique<EvalState>(LookupPath{}, state.store, fetchSettings, evalSettings, nullptr);
    };
    EXPECT_TRUE(unforceEnabled(*fresh()));
    evalSettings.enableNativeCode = true;
    EXPECT_FALSE(unforceEnabled(*fresh()));
    evalSettings.enableNativeCode = false;
    evalSettings.extraPrimOps.push_back({
        .name = "__unforceTest",
        .arity = 1,
        .impl = [](EvalState &, CallSite, Value * const * args, Value & v) { v = *args[0]; },
    });
    EXPECT_FALSE(unforceEnabled(*fresh()));
    evalSettings.extraPrimOps.pop_back();
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
    auto host = fresh(), other = fresh();
    unforceNoteCApiState(*host);
    EXPECT_FALSE(unforceEnabled(*host));
    EXPECT_TRUE(unforceEnabled(*other));
    unforceNoteCApiCode();
    EXPECT_FALSE(unforceEnabled(*other));
}

/* An effect in the driver's evaluation (no cell's force), which nothing runs
   again, keeps nothing. */
TEST_F(UnforceTest, driverEffectKeepsNothing)
{
    auto v = oldThunk("{ a = 1; }");
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    unforceNoteEffect(state);
    state.endUnforceScope();
    EXPECT_TRUE(v->isThunk());
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

/* A file is read once, as without jobs: a reverted import evaluates the
   file's first parse again, and a failed parse stays failed. */
TEST_F(UnforceTest, importParsedOnce)
{
    auto dir = createTempDir();
    AutoDelete del(dir, true);
    auto importOf = [&](auto name) { return oldThunk("import " + (dir / name).string()); };
    auto v = importOf("f.nix"), w = importOf("g.nix");
    writeFile(dir / "f.nix", "{ a = [ 1 ]; }");
    writeFile(dir / "g.nix", "{");
    auto reverted = unforceStats.cellsReverted;
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_THROW(state.forceValue(*w, noPos), ParseError);
    state.endUnforceScope();
    ASSERT_GE(unforceStats.cellsReverted, reverted + 2); // v and the file's cache cell
    writeFile(dir / "f.nix", "{ a = [ 2 ]; }");
    writeFile(dir / "g.nix", "{ }");
    state.beginUnforceScope();
    EXPECT_EQ(deep(*v), "{ a = [ 1 ]; }");
    EXPECT_THROW(state.forceValue(*importOf("g.nix"), noPos), ParseError);
    state.endUnforceScope();
}

/* An impure operation keeps its cell where running it again could differ: a
   copy that nothing remembers (with a filter), unless its source cannot change
   (a valid store path, or one mounted from a snapshot), and one that fails,
   also where the evaluator caches it once it succeeds, until it does, but not
   for a failure of its argument or of the file it imports. Where the file
   caches are reset while values stay (the REPL), every one keeps, an import
   too. */
TEST_F(UnforceTest, impureKeeps)
{
    auto dir = createTempDir();
    AutoDelete del(dir, true);
    auto d = dir.string();
    writeFile(dir / "f", "1");
    writeFile(dir / "t", "throw \"t\"");
    auto snapshot = make_ref<MemorySourceAccessor>();
    snapshot->addFile(CanonPath("/f"), "2");
    auto snap = state.store->printStorePath(StorePath::random("s")),
         work = state.store->printStorePath(StorePath::random("w"));
    state.storeFS->mount(CanonPath(snap), snapshot);
    state.storeFS->mount(CanonPath(work), makeFSSourceAccessor(dir));
    auto copy = [](std::string p) { return "\"${builtins.path { path = " + p + "; filter = _: _: true; }}\""; };
    for (auto & [op, keeps] : std::vector<std::pair<std::string, bool>>{
             {copy(d + "/f"), true},
             {copy(work + "/f"), true},
             {copy(storeFile("1")), false},
             {copy(snap + "/f"), false},
             {"import " + d + "/f", false},
             {"builtins.tryEval (import " + d + "/t) == builtins.tryEval (builtins.readFile (throw \"x\"))", false},
             {"builtins.readDir " + state.store->storeDir, true}, // the evaluation adds to it
         }) {
        for (bool reset : {false, true}) {
            state.fileCachesReset = reset;
            auto v = oldThunk("{ a = " + op + "; }");
            afterJob(*v);
            EXPECT_EQ(v->isThunk(), !keeps && !reset) << op;
        }
    }
    state.fileCachesReset = false;
    for (std::string op : {"import " + d + "/m", "builtins.readFileType " + d + "/m", "\"${" + d + "/m}\""}) {
        std::filesystem::remove(dir / "m");
        auto v = oldThunk("{ a = { b = " + op + "; }; }");
        state.beginUnforceScope();
        EXPECT_THROW(deep(*v), Error) << op;
        state.endUnforceScope();
        writeFile(dir / "m", "1");
        EXPECT_THROW(deep(*v), Error) << op;
    }
}

/* A failed target is not nominated, also where its force ran an impure
   operation that keeps its cell (a missing file's import): it keeps its
   failure, and `w`, forced before it in the job, is reverted as it would be
   without it, instead of being kept with what the job allocated. */
TEST_F(UnforceTest, failedImpureKeepsNothing)
{
    auto w = oldThunk("{ a = 1; }");
    auto f = oldThunk("import /nonexistent/unforce-test.nix");
    state.beginUnforceScope();
    state.forceValue(*w, noPos);
    EXPECT_THROW(state.forceValue(*f, noPos), Error);
    state.endUnforceScope();
    EXPECT_TRUE(f->isFailed());
    EXPECT_TRUE(w->isThunk());
}

/* A read that only an edit can change keeps no cell, whether it ran in the
   cell's own force or after it: read again, in a job or not, it must lead
   where it led and give what it first gave, or the evaluation stops, also
   where it now fails or a link made since leads elsewhere (k and j, links
   to m/../t and m/../u, once m is a link; u leads into the store). So must
   a copy. A new read is checked the same. The REPL keeps the cell instead,
   and a new read there sees the edit. */
TEST_F(UnforceTest, readChecked)
{
    auto dir = createTempDir();
    AutoDelete del(dir, true);
    using Edit = std::function<void(const std::filesystem::path &)>;
    Edit write = [](auto & d) { writeFile(d / "f", "2"); };
    Edit relink = [](auto & d) { std::filesystem::create_symlink(d / "t" / "x", d / "m"); }; // to t/t, t/u
    int n = 0;
    for (auto & [op, edit] : std::vector<std::pair<std::string, Edit>>{
             {"builtins.readFile @/f", write},
             {"builtins.readFile @/f", [](auto & d) { std::filesystem::remove(d / "f"); }},
             {"builtins.readFile @/f", [](auto & d) { writeFile(d / "f", std::string("1\0", 2)); }},
             {"builtins.hashFile \"sha256\" @/f", write},
             {"builtins.scopedImport { } @/f", [](auto & d) { writeFile(d / "f", "{"); }},
             {"builtins.readDir @", [](auto & d) { writeFile(d / "g", ""); }},
             {"builtins.pathExists @/l/x",
              [](auto & d) { std::filesystem::create_directory_symlink(d / "t", d / "l"); }},
             {"builtins.pathExists @/k/x", relink},
             {"\"${@/k/x}\"", relink},
             {"builtins.path { path = @/k/x; }", relink},
             {"builtins.readFile @/k/x", [](auto & d) { std::filesystem::create_symlink(d / "m", d / "m"); }},
             {"builtins.readFile @/j", relink},
             {"builtins.storePath @/j", relink},
         })
        for (bool own : {true, false}) {
            auto d = dir / std::to_string(n++);
            std::filesystem::create_directories(d / "t");
            writeFile(d / "t" / "x", "");
            writeFile(d / "f", "1");
            std::filesystem::create_symlink("m/../t", d / "k");
            std::filesystem::create_symlink("m/../u", d / "j");
            std::filesystem::create_symlink(storeFile("1"), d / "u");
            auto read = replaceStrings(op, "@", d.string());
            auto v = oldThunk(own ? "let a = " + read + "; in builtins.seq a { inherit a; }" : "{ a = " + read + "; }");
            afterJob(*v);
            EXPECT_TRUE(v->isThunk()) << read;
            edit(d);
            state.beginUnforceScope();
            EXPECT_THROW(deep(*v), UnforceFileChanged) << read;
            state.endUnforceScope();
            EXPECT_THROW(deep(*oldThunk(read)), UnforceFileChanged) << read;
        }
    auto f = (dir / "r").string();
    writeFile(f, "1");
    state.fileCachesReset = true;
    auto v = oldThunk("{ a = builtins.readFile " + f + "; }");
    afterJob(*v);
    writeFile(f, "2");
    EXPECT_EQ(deep(*v), "{ a = \"1\"; }");
    EXPECT_EQ(afterJob(*oldThunk("builtins.readFile " + f)), "\"2\"");
    state.fileCachesReset = false;
}

/* A store path that the store does not have but the host does, where the
   store keeps its objects elsewhere (the dummy store, as a chroot or a
   remote store), changes only by an edit: its read is recorded and keeps no
   cell. Here, the tests' data in the host's store. */
TEST_F(UnforceTest, hostStorePathRecorded)
{
    auto data = getUnitTestData().string();
    if (!state.store->isInStore(data))
        GTEST_SKIP() << "the test data is not in the store";
    auto records = state.readDigests.size();
    auto v = oldThunk("{ a = builtins.readDir " + data + "; }");
    afterJob(*v);
    EXPECT_TRUE(v->isThunk());
    EXPECT_EQ(state.readDigests.size(), records + 1);
}

class UnforceRestrictedTest : public UnforceTest
{
protected:
    UnforceRestrictedTest()
        : UnforceTest(true)
    {
    }
};

/* A path that restrict-eval does not allow yet may be allowed by the
   evaluation itself: the cell that found it missing keeps its value, and a
   new read, once it is allowed, finds it. */
TEST_F(UnforceRestrictedTest, notAllowedKeeps)
{
    auto p = storeFile("1");
    auto v = oldThunk("{ a = builtins.pathExists " + p + "; }");
    EXPECT_EQ(afterJob(*v), "{ a = false; }");
    state.allowPath(state.store->parseStorePath(p));
    EXPECT_EQ(deep(*v), "{ a = false; }");
    EXPECT_EQ(afterJob(*oldThunk("builtins.pathExists " + p)), "true");
}

/* A job continues past a boundary only while no holder but a seen set holds
   its memory, which it could let go of after a later element read through
   it. b and c allocate little next to a: b is trapped at c's boundary, and
   the job ends at the next one, where a root holds c's memory. */
TEST_F(UnforceTest, holderEndsMergedJob)
{
    auto v = oldThunk("{ a = builtins.length (builtins.genList (x: x) 300000); b = { x = 1; }; c = { y = 2; }; }");
    state.forceValue(*v, noPos);
    auto cell = [&](const char * name) { return v->attrs()->get(createSymbol(name))->value; };
    auto trapped = [&](const char * name) {
        return cell(name)->isThunk() && unforceTrapped(cell(name)->thunk().expr, cell(name)->thunk().env);
    };
    RootValue root;
    UnforceJobs jobs(state);
    for (auto name : {"a", "b", "c"}) {
        jobs.element();
        state.forceValue(*cell(name), noPos);
    }
    root = allocRootValue(state.allocValue());
    (*root)->mkInt(1);
    EXPECT_TRUE(trapped("b"));
    jobs.element();
    EXPECT_FALSE(trapped("b") || trapped("c"));
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

/* The `VmFlags` of the mapping that holds `a` (/proc/self/smaps), and its end. */
static std::pair<std::string, uintptr_t> vmFlagsAt(uintptr_t a)
{
    std::ifstream in("/proc/self/smaps");
    uintptr_t lo = 0, hi = 0;
    bool here = false;
    for (std::string line; std::getline(in, line);)
        if (sscanf(line.c_str(), "%" SCNxPTR "-%" SCNxPTR, &lo, &hi) == 2)
            here = lo <= a && a < hi;
        else if (here && line.starts_with("VmFlags:"))
            return {line + " ", hi};
    return {};
}

/* A core holds every page a job wrote, and not the mapping's rest, which no
   job touched (` dd `: left out of core dumps), also once a job reaches past
   the part dumped so far. */
TEST_F(UnforceTest, coreHoldsWrittenPages)
{
    state.beginUnforceScope();
    auto page = uintptr_t(sysconf(_SC_PAGESIZE));
    state.mem.allocBytes(1);
    auto top = vmFlagsAt(uintptr_t(evalRegion.cur) - 1).second;
    state.mem.allocBytes(top - uintptr_t(evalRegion.cur) + page);
    for (auto a = uintptr_t(evalRegion.base); a < uintptr_t(evalRegion.cur);) {
        auto [flags, end] = vmFlagsAt(a);
        EXPECT_EQ(flags.find(" dd "), std::string::npos) << flags;
        ASSERT_GT(end, a);
        a = end;
    }
    auto end = vmFlagsAt(uintptr_t(evalRegion.cur) - 1).second;
    EXPECT_GT(end, top);
    EXPECT_NE(vmFlagsAt(end).first.find(" dd "), std::string::npos);
    state.endUnforceScope();
}

/* A job does not run without its region: where none can be mapped, opening
   the job runs out of memory, also where a traversal prints an element's
   error and goes on (that is no element's failure). */
TEST_F(UnforceTest, unmappableRegionNotSwallowed)
{
    auto v = oldThunk("{ a = 1; }");
    state.forceValue(*v, noPos);
    unforceTestFailMapping = true;
    Finally restore([] { unforceTestFailMapping = false; });
    std::ostringstream out;
    UnforceJobs jobs(state);
    EXPECT_THROW(printValue(state, out, *v, PrintOptions{.force = true, .unforceJobs = true}), std::bad_alloc);
}

/* A job runs out of memory for a size so large that rounding it up would
   wrap around (`builtins.genList` of 2^61 - 1 elements asks for one), as
   evaluation outside a job does, instead of handing out nothing. */
TEST_F(UnforceTest, hugeAllocationFails)
{
    state.beginUnforceScope();
    EXPECT_THROW(state.mem.allocBytes(SIZE_MAX - 7), std::bad_alloc);
    state.endUnforceScope();
}

/* A job's memory 64 GiB and more above its start: v's set, which a seen set
   holds, lies there, below the start of the segment that u's force opened
   once a 32-bit offset wraps. The set names v, its value, which the seen set
   forgets: u and v are reverted, where the wrong owner would promote u (or
   keep the job whole). Only where the system maps a region that large. */
TEST_F(UnforceTest, regionBeyond64GiB)
{
    auto u = oldThunk("builtins.genList (i: i) 1000");
    auto v = oldThunk("{ b = 2; }");
    UnforceSeen seen;
    auto whole = unforceStats.jobsKeptWhole;
    state.beginUnforceScope();
    state.forceValue(*u, noPos);
    try {
        state.mem.allocBytes((size_t(1) << 36) - 4096);
    } catch (std::bad_alloc &) {
        state.endUnforceScope();
        GTEST_SKIP() << "no region of 64 GiB";
    }
    state.forceValue(*v, noPos);
    EXPECT_TRUE(seen.see(v->attrs(), UnforceHolder::Kind::bindings));
    state.endUnforceScope();
    EXPECT_EQ(unforceStats.jobsKeptWhole, whole);
    EXPECT_TRUE(u->isThunk());
    EXPECT_TRUE(v->isThunk());
}

/* Whether the page holding `p` went back to the system: under
   AddressSanitizer, whether `p` is poisoned instead. */
static bool givenBack(const void * p)
{
#  if NIX_ASAN_ENABLED
    return __asan_address_is_poisoned(p);
#  else
    auto page = sysconf(_SC_PAGESIZE);
    unsigned char resident = 1;
    mincore((void *) (uintptr_t(p) & ~uintptr_t(page - 1)), page, &resident);
    return !(resident & 1);
#  endif
}

/* A holder that keeps `cells` at a job's end. */
struct KeepCells : UnforceHolder
{
    std::vector<const Value *> cells;

    void held(Hold &) override {}

    void ended() override {}

    bool keeps(const Value * c) const override
    {
        return std::ranges::contains(cells, c);
    }
};

/* A job's end gives back the whole pages, below its cut, of the targets that
   keep nothing: `u`'s, whose force encloses that of `p`, which a holder
   keeps. `t`, a string target the holder keeps too, took `u`'s value, whose
   bytes lie in `u`'s run: it is kept by a copy of all of them. `p`'s list
   stays as it was. */
TEST_F(UnforceTest, deadRunsGivenBack)
{
    auto v = oldThunk(
        "rec { p = builtins.genList (i: i) 4096; t = u; u = let s = builtins.concatStringsSep \"\" "
        "(builtins.genList (i: \"abcdefgh\") 8192); in builtins.seq s (builtins.seq p s); }");
    state.forceValue(*v, noPos);
    auto cell = [&](const char * name) { return v->attrs()->get(createSymbol(name))->value; };
    KeepCells holder;
    holder.cells = {cell("p"), cell("t")};
    state.beginUnforceScope();
    state.forceValue(*cell("t"), noPos);
    auto bytes = cell("t")->c_str() + sysconf(_SC_PAGESIZE);
    state.endUnforceScope();
    EXPECT_TRUE(cell("u")->isThunk());
    EXPECT_TRUE(givenBack(bytes));
    EXPECT_EQ(cell("t")->string_view().size(), 65536u);
    EXPECT_EQ(cell("t")->string_view().find_first_not_of("abcdefgh"), std::string_view::npos);
    state.forceValue(*cell("p")->listView()[4095], noPos);
    EXPECT_THAT(*cell("p")->listView()[4095], IsIntEq(4095));
}

/* A value the driver allocated in the job, which a holder holds, is no
   target's: the job is kept whole, and a seen set forgets nothing. */
TEST_F(UnforceTest, driverHoldKeepsJob)
{
    auto v = oldThunk("{ a = 1; }");
    UnforceSeen seen;
    auto whole = unforceStats.jobsKeptWhole;
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_TRUE(seen.see(v->attrs(), UnforceHolder::Kind::bindings));
    auto w = state.allocValue();
    w->mkInt(1);
    EXPECT_TRUE(seen.see(w, UnforceHolder::Kind::value));
    state.endUnforceScope();
    EXPECT_EQ(unforceStats.jobsKeptWhole, whole + 1);
    EXPECT_FALSE(v->isThunk());
    EXPECT_FALSE(seen.see(v->attrs(), UnforceHolder::Kind::bindings));
    EXPECT_THAT(*w, IsIntEq(1));
}

/* An eval-cache cursor holds the cell it reads through: one its job built,
   in a set the job forced, is kept, as `nix search` reads a set's cursor
   after each of its elements' jobs. A later job reuses what the first freed. */
TEST_F(UnforceTest, cursorKeepsJobsCell)
{
    auto v = oldThunk("{ x = { s = 1 + 1; }; }");
    auto cache = make_ref<eval_cache::EvalCache>(std::nullopt, state, [&] { return v; });
    state.beginUnforceScope();
    auto x = cache->getRoot()->getAttr("x");
    state.endUnforceScope();
    state.beginUnforceScope();
    state.forceValueDeep(*oldThunk("builtins.genList (i: { a = i; b = [ i ]; }) 3000"));
    state.endUnforceScope();
    EXPECT_EQ(x->getAttr("s")->getInt().value, 2);
    EXPECT_EQ(&x->forceValue(), v->attrs()->get(createSymbol("x"))->value);
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

/* A cell whose force recovers from an interrupt in a later job is reverted
   to its failure, whose recovery evaluates the cell's expression, not a
   re-force: a seen set keeps the set it holds, and with it the cell. */
TEST_F(UnforceTest, seenKeepsRecovered)
{
    auto v = oldThunk("builtins.seq (builtins.toJSON [ 1 2 3 ]) { x = 1; }");
    state.beginUnforceScope();
    setInterrupted(true);
    EXPECT_THROW(state.forceValue(*v, noPos), Interrupted);
    setInterrupted(false);
    state.endUnforceScope();
    UnforceSeen seen;
    state.beginUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_TRUE(seen.see(v->attrs(), UnforceHolder::Kind::bindings));
    state.endUnforceScope();
    state.forceValue(*v, noPos);
    EXPECT_FALSE(seen.see(v->attrs(), UnforceHolder::Kind::bindings));
}

/* An infinite recursion's error forgets the cell it names once it has
   marked where the recursion is entered, so that no later job's cell at
   that address matches it: here one found in a young cell, by a young cell
   that survives the job with its owner, and by an old cell, which keeps its
   failure. */
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

#  if !NIX_ASAN_ENABLED
/* A failure whose cell a job's end frees keeps nothing past the end, its
   error included: each element here fails with a message of 1 MiB, and the
   process grows by less than half of what the 64 messages hold. (ASan's
   quarantine keeps what is freed.) */
TEST_F(UnforceTest, freedFailureKeepsNothing)
{
    auto resident = [] {
        long size = 0, pages = 0;
        std::ifstream("/proc/self/statm") >> size >> pages;
        return pages * sysconf(_SC_PAGESIZE);
    };
    auto v = oldThunk(
        "let m = builtins.concatStringsSep \"\" (builtins.genList (_: \"0123456789abcdef\") 65536); in "
        "builtins.seq m (builtins.genList (i: (builtins.tryEval (throw (m + toString i))).success) 64)");
    state.forceValue(*v, noPos);
    auto before = resident();
    auto out = printed(*v, true);
    EXPECT_LT(resident() - before, 32 << 20);
    std::string want = "[";
    for (int i = 0; i < 64; ++i)
        want += " false";
    EXPECT_EQ(out, want + " ]");
}
#  endif

/* `foldl'`'s steps, in one force in a job. Each case's `r` is forced after
   its list `xs`, made in the job, or before it (`old`). Then, once the job
   has allocated over what the steps freed, as zeroed memory, `c` prints as
   without jobs. Where the steps free all they allocate (`frees`), the region
   grows by no more for a list twice as long but by the two cells the loop
   takes turns handing on. */
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
        /* The same, each step a batch (it allocates a page), whose value moves to a new cell. */
        {ints,
         "builtins.foldl' (acc: x: builtins.seq (builtins.genList (j: j) 300) [ acc ]) null xs",
         "r",
         false,
         false},
        /* The batch's last value holds its first cell, which holds what the first step built. */
        {ints,
         "builtins.foldl' (acc: x: if x == 0 then { a = 1; } else if x == 1 then [ acc ] else acc) null xs",
         "r",
         false,
         false},
        /* Each step's value holds no cell of the loop's. */
        {ints, "builtins.foldl' (acc: x: [ x ]) null xs", "r", false, true},
        /* A step forces a young cell made before the loop (a write), or an old one (logged). */
        {sets, "builtins.foldl' (acc: x: acc + x.a) 0 xs", "[ r xs ]", false, false},
        {sets, "builtins.foldl' (acc: x: acc + x.a) 0 xs", "[ r xs ]", true, false},
        /* The same, the cell made in the loop's force (its owner's open segment). */
        {ints,
         "let ys = builtins.genList (i: { a = i; }) n; in [ (builtins.foldl' (a: y: a + y.a) 0 ys) ys ]",
         "r",
         false,
         false},
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
    /* From 200 steps: a loop keeps its last batch, which steps that
       allocate less (an Env in the caller's frame) fill later. */
    for (auto & k : cases) {
        auto grew = run(k, 200);
        if (k.frees)
            EXPECT_LE(run(k, 400), grew + 2 * sizeof(Value)) << k.r;
    }
}

/* Steps that hold less than a page in all end together, with the last. */
TEST_F(UnforceTest, stepsBatch)
{
    auto v = oldThunk("builtins.foldl' builtins.add 0 (builtins.genList (i: i) 100)");
    state.beginUnforceScope();
    auto ends = unforceStats.stepEnds;
    state.forceValue(*v, noPos);
    EXPECT_EQ(unforceStats.stepEnds, ends + 1);
    state.endUnforceScope();
}

/* A step in which a failure was caught is freed, and the failed cell's
   record with it: the job's end has nothing of it left to read (under
   AddressSanitizer, a record read or deleted again would fail). So are an
   inner loop's steps, in an outer step that catches failures too. */
TEST_F(UnforceTest, failedStepFreed)
{
    auto f = "(if (builtins.tryEval (throw \"x\")).success then 1 else 2)";
    const std::pair<std::string, int> cases[] = {
        {fmt("builtins.foldl' (acc: x: acc + %s) 0 [ 1 ]", f), 2},
        {fmt("builtins.foldl' (acc: x: acc + %1% + builtins.foldl' (a: y: a + %1%) 0 [ 1 2 ]) 0 [ 1 ]", f), 6},
    };
    for (auto & [loop, want] : cases) {
        auto v = oldThunk(loop);
        state.beginUnforceScope();
        auto ends = unforceStats.stepEnds;
        state.forceValue(*v, noPos);
        EXPECT_GT(unforceStats.stepEnds, ends) << loop;
        state.endUnforceScope();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsIntEq(want)) << loop;
    }
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

/* `s`'s force forces a cell of `t`'s, after `t`'s force, and a holder keeps
   `t` (or `q`, whose force ends after `t`'s and before `s`'s). What that
   force built stays `t`'s, and so is kept with it, where `t`'s cell reaches
   it (`v`), or a cell of `t`'s that it forced does (`w`, by `u`); and a
   force that reopened `t`'s segment (`s` allocated nothing first) leaves the
   segment `t`'s (`l`, which `t` built, spans pages). In the last case the
   force leaves no trace. A later job allocates over what the end freed. */
TEST_F(UnforceTest, youngForceKeepsOwner)
{
    auto t0 =
        "let l = builtins.genList (i: i) 1000; in builtins.seq l (rec { inherit l; v = builtins.genList (i: i * 2) "
        "1000; w = { z = 2; }; u = builtins.seq w 1; x = 0; y = x + 0; })";
    auto want = deep(*oldThunk(t0));
    for (auto [e, holder, held] : std::initializer_list<std::tuple<const char *, const char *, const char *>>{
             {"builtins.seq t.v { a = 1; }", "t", "v"},
             {"builtins.seq t.u { a = 1; }", "t", "w"},
             {"t.y + q.n", "q", "z"},
             {"builtins.seq t.y { a = 1; }", "t", "y"}}) {
        auto r = oldThunk(fmt("rec { t = %s; q = { n = 1; z = { }; }; s = %s; }", t0, e));
        state.forceValue(*r, noPos);
        auto get = [&](Value & a, const char * n) -> Value & { return *a.attrs()->get(createSymbol(n))->value; };
        auto & t = get(*r, "t");
        UnforceSeen seen;
        state.beginUnforceScope();
        state.forceValue(t, noPos);
        state.forceValue(get(*r, "s"), noPos);
        EXPECT_TRUE(seen.see(&get(get(*r, holder), held), UnforceHolder::Kind::value));
        state.endUnforceScope();
        EXPECT_FALSE(t.isThunk()) << e;
        state.beginUnforceScope();
        state.mem.allocBytes(1 << 20);
        EXPECT_EQ(deep(t), want) << e;
        state.endUnforceScope();
    }
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

/* A condition gives back the whole pages of the list it built as it ends:
   nothing outside it points there. But once the job's undo log is full
   (`unforceTestLogLimit`, here at one entry), `t` is forced unlogged, under
   the current owner: its value lies in the condition's memory, or in
   `foldl'`'s step's, and neither frees it. */
TEST_F(UnforceTest, boolFrameGivesBack)
{
    auto limit = unforceTestLogLimit;
    Finally restore([&] { unforceTestLogLimit = limit; });
    auto cond = "if builtins.length t.l + builtins.length (builtins.genList (i: i) 10000) > 0 then 1 else 0";
    for (auto [full, a] : std::initializer_list<std::pair<bool, const char *>>{
             {false, cond}, {true, cond}, {true, "builtins.foldl' (n: x: builtins.length t.l) 0 [ 1 ]"}}) {
        unforceTestLogLimit = full ? 1 : limit;
        auto v = oldThunk(fmt("rec { t = { l = builtins.genList (i: i * 2) 10000; }; a = %s; }", a));
        state.forceValue(*v, noPos);
        auto cell = [&](Value & s, const char * name) -> Value & { return *s.attrs()->get(createSymbol(name))->value; };
        state.beginUnforceScope();
        state.forceValue(cell(*v, "a"), noPos);
        if (a == cond)
            EXPECT_EQ(givenBack(evalRegion.cur - 2 * sysconf(_SC_PAGESIZE)), !full);
        state.endUnforceScope();
        auto & t = cell(*v, "t");
        state.forceValue(t, noPos);
        auto & l = cell(t, "l");
        state.forceValue(l, noPos);
        state.forceValue(*l.listView()[9999], noPos);
        EXPECT_THAT(*l.listView()[9999], IsIntEq(19998)) << a;
    }
}

/* A frame entered in the job holds a thunk `slot`, which `f`'s condition
   forces: the slot's value lies in the condition's memory, which it keeps. */
[[gnu::noinline]] static void conditionForcesSlot(EvalState & state, Value & f)
{
    Value slot;
    slot.mkThunk(
        &state.baseEnv,
        state.parseExprFromString("builtins.genList (i: i * 2) 10000", state.rootPath(CanonPath::root)));
    auto w = state.allocValue();
    w->mkApp(&f, &slot);
    state.forceValue(*w, noPos);
    state.forceValue(*slot.listView()[9999], noPos);
    EXPECT_THAT(*slot.listView()[9999], IsIntEq(19998));
}

TEST_F(UnforceTest, boolFrameKeepsSlot)
{
    auto f = oldThunk("x: if builtins.length x > 0 then 1 else 0");
    state.forceValue(*f, noPos);
    state.beginUnforceScope();
    conditionForcesSlot(state, *f);
    state.endUnforceScope();
}

/* A thunk in a frame on a coroutine's stack (a binary cache store runs a
   path filter on one), forced in the job to a value of the job's memory:
   the job's end reads or writes no such slot once the coroutine is gone. */
TEST_F(UnforceTest, coroutineSlot)
{
    auto t = oldThunk("{ a = 1; }");
    state.beginUnforceScope();
    auto source = sinkToSource([&](Sink & sink) {
        Value slot = *t;
        state.forceValue(slot, noPos);
        sink(printValue(state, slot));
    });
    EXPECT_EQ(source->drain(), "{ a = 1; }");
    source.reset();
    state.endUnforceScope();
}

/* Strict JSON printing: an element of a nested level runs in a job of its own,
   while the levels open keep their sets and lists (a, y, q a list, l a small
   list in an old cell), and prints as without jobs. `c` leaves its job empty,
   which x then continues. The printer that b's `__toString` calls
   (`builtins.toJSON`, on `g`, forced before) is no level of the traversal. */
TEST_F(UnforceTest, nestedLevelJobs)
{
    auto expr =
        "let big = n: builtins.length (builtins.genList (i: { inherit i; }) n); "
        "g = { u = big 6; v = { w = big 7; }; }; in builtins.seq g "
        "{ a = { c = 1; x = big 1000; y = { p = big 2000; q = [ (big 3) (big 4) (big 5) ]; }; z = big 5; }; "
        "b = { __toString = self: builtins.toJSON g; }; l = [ { x = big 1; } { y = big 2; } ]; }";
    auto json = [&](Value & v, bool jobs) {
        state.forceValue(v, noPos);
        NixStringContext context;
        std::optional<UnforceJobs> traversal;
        if (jobs)
            traversal.emplace(state);
        return printValueAsJSON(state, true, v, noPos, context, false, jobs).dump();
    };
    auto jobs = unforceStats.jobs;
    auto whole = unforceStats.jobsKeptWhole;
    auto freed = unforceStats.bytesFreed;
    EXPECT_EQ(json(*oldThunk(expr), true), json(*oldThunk(expr), false));
    /* Ended at c, y, p, q, q's three elements, z, b, l, l's two elements and
       their attributes, and the traversal's end. */
    EXPECT_EQ(unforceStats.jobs - jobs, 15u);
    EXPECT_EQ(unforceStats.jobsKeptWhole - whole, 0u);
    EXPECT_GT(unforceStats.bytesFreed - freed, 0u);
}

/* Non-strict printing has no nested levels: a value that an outPath's peel
   forced in one element is read as it is in another. */
TEST_F(UnforceTest, nestedJsonNonStrict)
{
    auto v = oldThunk(
        "let t = \"x\" + \"y\"; a = { outPath = t; }; top = { inherit a; b = t; }; in "
        "builtins.seq a (builtins.seq top { inherit top; })");
    state.forceValue(*v, noPos);
    NixStringContext context;
    UnforceJobs traversal(state);
    EXPECT_EQ(
        printValueAsJSON(state, false, *v, noPos, context, false, true).dump(),
        "{\"top\":{\"a\":\"xy\",\"b\":\"xy\"}}");
}

/* `getDerivations` finds the derivations of the sets it recurses into in
   jobs of their own; an element that leaves its job empty (a
   `recurseForDerivations`) does not end it, nor does one of what a functor
   (f) or a function (the list's first element) returned, which nothing
   holds. What it found outlives them. */
TEST_F(UnforceTest, getDerivationsNestedJobs)
{
    auto found = [&](std::string expr, uint64_t wantJobs) {
        auto v = oldThunk("let d = n: derivation { name = n; system = \"s\"; builder = \"b\"; }; in " + expr);
        state.forceValue(*v, noPos);
        auto jobs = unforceStats.jobs;
        PackageInfos drvs;
        getDerivations(state, *v, "", Bindings::emptyBindings, drvs, false);
        EXPECT_EQ(unforceStats.jobs - jobs, wantJobs) << expr;
        std::set<std::string> names;
        for (auto & drv : drvs) {
            names.insert(drv.queryName());
            EXPECT_EQ(drv.queryOutputName(), "out");
        }
        return names;
    };
    /* Ended at a's recurseForDerivations, x, y, y's recurseForDerivations, b, f, and the traversal's end. */
    EXPECT_EQ(
        found(
            "{ a = { recurseForDerivations = true; x = d \"x\"; y = { recurseForDerivations = true; z = d \"z\"; }; "
            "w = d \"w\"; }; b = d \"b\"; "
            "f = { recurseForDerivations = true; __functor = self: { u = d \"u\"; v = d \"v\"; }; }; }",
            7),
        (std::set<std::string>{"b", "u", "v", "w", "x", "z"}));
    /* Ended at r and the traversal's end. */
    EXPECT_EQ(
        found("[ ({ ... }: { p = d \"p\"; q = d \"q\"; }) (d \"r\") ]", 2), (std::set<std::string>{"p", "q", "r"}));
}

/* An `UnforceJobs` loop in another's job, at one frame address in each call:
   its element forces a local cell, before or after the element starts. */
[[gnu::noinline]] static bool sibling(EvalState & state, Expr * e, bool nests, bool forceFirst)
{
    Value local;
    local.mkThunk(&state.baseEnv, e);
    UnforceJobs inner(state);
    if (!forceFirst)
        state.forceValue(local, noPos);
    inner.element(nests);
    if (forceFirst)
        state.forceValue(local, noPos);
    return !local.isThunk();
}

/* An inner loop's element is a job of its own only if both loops declared
   they nest. A slot that a sibling's frame logged after a switch, at an
   address the next sibling's frame reuses, keeps the job whole: reverting it
   would write into the running frame. */
TEST_F(UnforceTest, nestedLoops)
{
    auto e = state.parseExprFromString("{ a = [ 1 2 ]; }", state.rootPath(CanonPath::root));
    for (auto outerNests : {false, true}) {
        auto jobs = unforceStats.jobs;
        {
            UnforceJobs outer(state);
            outer.element(outerNests);
            state.forceValue(*oldThunk("[ 1 ]"), noPos);
            sibling(state, e, true, true);
            if (outerNests) {
                EXPECT_EQ(unforceStats.jobs - jobs, 1u);
                EXPECT_TRUE(sibling(state, e, true, false));
            }
        }
        if (!outerNests)
            EXPECT_EQ(unforceStats.jobs - jobs, 1u);
    }
}

/* A switch whose next job cannot open (no region) leaves no job open: the
   next traversal's elements run in jobs. */
TEST_F(UnforceTest, nestedOpenFails)
{
    {
        UnforceJobs outer(state);
        outer.element(true);
        state.forceValue(*oldThunk("[ 1 ]"), noPos);
        unforceTestFailMapping = true;
        Finally restore([] { unforceTestFailMapping = false; });
        EXPECT_THROW(
            sibling(state, state.parseExprFromString("{ }", state.rootPath(CanonPath::root)), true, true),
            std::bad_alloc);
    }
    auto jobs = unforceStats.jobs;
    auto freed = unforceStats.bytesFreed;
    auto expr = "{ a = builtins.genList (i: i) 10; }";
    EXPECT_EQ(printed(*oldThunk(expr), true), printed(*oldThunk(expr), false));
    EXPECT_EQ(unforceStats.jobs - jobs, 1u);
    EXPECT_GT(unforceStats.bytesFreed - freed, 0u);
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
         UnforceJobs jobs(state);
         printValueAsXML(state, true, false, v, out, context, noPos, true);
         return out.str();
     }},
    {"nix eval --json",
     [](EvalState & state, Value & v) {
         NixStringContext context;
         std::ostringstream out;
         UnforceJobs jobs(state);
         printValueAsJSON(state, true, v, noPos, out, context, false, true);
         return out.str();
     }},
    {"nix eval",
     [](EvalState & state, Value & v) {
         std::ostringstream out;
         UnforceJobs jobs(state);
         printValue(state, out, v, PrintOptions{.force = true, .derivationPaths = true, .unforceJobs = true});
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
   error at the same position (its trace may differ), though a failure's
   message may show more of what is evaluated (`printsAsOrMoreEvaluated`).
   In an evaluator of its own each time. The store is a dummy one:
   derivations' paths are computed, not written. */
TEST_F(UnforceTest, corpus)
{
    auto readOnly = std::exchange(nix::settings.readOnlyMode, true);
    Finally restore([&] { nix::settings.readOnlyMode = readOnly; });
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

#if !NIX_USE_BOEHMGC

namespace nix {

void unforceTestReset();

/* Front ends open jobs only where a job has a region to free: elsewhere
   (a 32-bit system, another kernel) they take no path of unforcing's. */
TEST_F(LibExprTest, unforceOnlyWithRegion)
{
    unforceTestReset();
    if (!unforceEnabled(state))
        return;
    state.beginUnforceScope();
    EXPECT_NE(evalRegion.base, nullptr);
    state.endUnforceScope();
}

} // namespace nix

#endif
