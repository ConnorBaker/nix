#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/// Extract dep keys from a dep vector for exact-match assertions.
static std::vector<std::string> keys(InterningPools & pools, const std::vector<Dep> & deps)
{
    std::vector<std::string> out;
    out.reserve(deps.size());
    for (auto & d : deps)
        out.push_back(depKeyDisplayForTest(pools, d));
    return out;
}

class DepStabilityTest : public ::testing::Test
{
protected:
    InterningPools pools;
    std::vector<Dep> epochLog;
    void SetUp() override { epochLog.clear(); }
    void TearDown() override { epochLog.clear(); }

    /// Simulate a parent evaluation that records its own deps, then a child
    /// evaluates fresh (recording into child's own ownDeps via a nested
    /// scope in DepRecordingContext). Returns the parent's collected deps —
    /// child deps are structurally isolated in child scope's ownDeps.
    std::vector<std::string> runWithFreshChild(
        const std::vector<Dep> & parentDeps,
        const std::vector<Dep> & childDeps)
    {
        epochLog.clear();
        DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);

        // Parent's own deps (before child)
        for (auto & d : parentDeps)
            ctx.record(d);

        // Child evaluates fresh — records into child scope's ownDeps
        {
            TestScopeAccess::pushScope(ctx);
            for (auto & d : childDeps)
                ctx.record(d);
            // child scope popped here; its ownDeps are discarded
            TestScopeAccess::popScope(ctx);
        }

        return keys(pools, TestScopeAccess::takeDeps(ctx));
    }

    /// Same as runWithFreshChild but the child's deps are replayed via
    /// recordToEpochLog (simulating a cached child replaying its trace).
    /// recordToEpochLog appends to epochLog but NOT to any
    /// scope's ownDeps, so parent's ownDeps are unaffected.
    std::vector<std::string> runWithCachedChild(
        const std::vector<Dep> & parentDeps,
        const std::vector<Dep> & childReplayedDeps)
    {
        epochLog.clear();
        DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);

        // Parent's own deps
        for (auto & d : parentDeps)
            ctx.record(d);

        // Child replays cached deps — goes to session/epoch only,
        // not to parent's ownDeps
        for (auto & d : childReplayedDeps)
            epochLog.push_back(d);

        return keys(pools, TestScopeAccess::takeDeps(ctx));
    }
};

// ═════════════════════════════════════════════════════════════════════
// Test 1: Parent deps stable when child evaluates fresh
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, ParentDeps_ChildFreshEval_Stable)
{
    std::vector<Dep> parentDeps = {
        makeContentDep(pools, "/parent/a.nix", "pa"),
        makeContentDep(pools, "/parent/b.nix", "pb"),
    };

    // Run 1: child has 4 deps
    auto run1 = runWithFreshChild(parentDeps, {
        makeContentDep(pools, "/child/f1.nix", "c1"),
        makeContentDep(pools, "/child/f2.nix", "c2"),
        makeContentDep(pools, "/child/f3.nix", "c3"),
        makeContentDep(pools, "/child/f4.nix", "c4"),
    });

    // Run 2: child has different deps (different caching state)
    auto run2 = runWithFreshChild(parentDeps, {
        makeContentDep(pools, "/child/f1.nix", "c1"),
        makeContentDep(pools, "/child/f5.nix", "c5"),
    });

    // Parent deps must be identical regardless of child's dep set
    EXPECT_EQ(run1, run2);
    EXPECT_EQ(run1, (std::vector<std::string>{"/parent/a.nix", "/parent/b.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 2: Parent deps stable when child replays cached deps
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, ParentDeps_ChildCachedReplay_Stable)
{
    std::vector<Dep> parentDeps = {
        makeContentDep(pools, "/parent/x.nix", "px"),
    };

    // Run 1: child evaluates fresh with N deps
    auto run1 = runWithFreshChild(parentDeps, {
        makeContentDep(pools, "/child/a.nix", "ca"),
        makeContentDep(pools, "/child/b.nix", "cb"),
        makeContentDep(pools, "/child/c.nix", "cc"),
    });

    // Run 2: child is cached, replays M != N deps
    auto run2 = runWithCachedChild(parentDeps, {
        makeContentDep(pools, "/child/a.nix", "ca"),
        makeContentDep(pools, "/child/d.nix", "cd"),
    });

    // Parent sees only its own deps in both cases
    EXPECT_EQ(run1, run2);
    EXPECT_EQ(run1, (std::vector<std::string>{"/parent/x.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 3: Oscillation pattern — three consecutive runs
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, Oscillation_ThreeRuns_ProduceSameDeps)
{
    std::vector<Dep> parentDeps = {
        makeContentDep(pools, "/parent/root.nix", "pr"),
    };

    // Run 1 (cold): child evaluates fresh, records {f1, f2, f3, f4}
    auto run1 = runWithFreshChild(parentDeps, {
        makeContentDep(pools, "/child/f1.nix", "c1"),
        makeContentDep(pools, "/child/f2.nix", "c2"),
        makeContentDep(pools, "/child/f3.nix", "c3"),
        makeContentDep(pools, "/child/f4.nix", "c4"),
    });

    // Run 2 (warm): child verify-replays subset {f1, f2}
    auto run2 = runWithCachedChild(parentDeps, {
        makeContentDep(pools, "/child/f1.nix", "c1"),
        makeContentDep(pools, "/child/f2.nix", "c2"),
    });

    // Run 3 (re-eval): child evaluates fresh again with {f1, f2, f5}
    auto run3 = runWithFreshChild(parentDeps, {
        makeContentDep(pools, "/child/f1.nix", "c1"),
        makeContentDep(pools, "/child/f2.nix", "c2"),
        makeContentDep(pools, "/child/f5.nix", "c5"),
    });

    // All three runs produce identical parent deps
    EXPECT_EQ(run1, run2);
    EXPECT_EQ(run2, run3);
    EXPECT_EQ(run1, (std::vector<std::string>{"/parent/root.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 4: Per-tracker ownDeps structurally prevents parent contamination
//
// With per-tracker ownDeps, child deps go to child.ownDeps, never to
// parent.ownDeps. Even without any exclusion mechanism, parent deps
// are always stable. This was previously a negative test showing the
// bug; it is now a positive test showing structural isolation.
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, PerScope_OwnDeps_ParentAlwaysStable)
{
    // Run 1: child has 3 deps, recorded via nested scope
    auto run1 = [&]() {
        epochLog.clear();
        DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/parent/p.nix", "pp"));
        {
            TestScopeAccess::pushScope(ctx);
            ctx.record(makeContentDep(pools, "/child/a.nix", "ca"));
            ctx.record(makeContentDep(pools, "/child/b.nix", "cb"));
            ctx.record(makeContentDep(pools, "/child/c.nix", "cc"));
            TestScopeAccess::popScope(ctx);
        }
        return keys(pools, TestScopeAccess::takeDeps(ctx));
    }();

    // Run 2: child has 1 dep, recorded via nested scope
    auto run2 = [&]() {
        epochLog.clear();
        DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/parent/p.nix", "pp"));
        {
            TestScopeAccess::pushScope(ctx);
            ctx.record(makeContentDep(pools, "/child/x.nix", "cx"));
            TestScopeAccess::popScope(ctx);
        }
        return keys(pools, TestScopeAccess::takeDeps(ctx));
    }();

    // Parent deps are identical — per-scope ownDeps provides structural isolation
    EXPECT_EQ(run1, run2);
    EXPECT_EQ(run1.size(), 1u);  // only parent dep
    EXPECT_EQ(run1, (std::vector<std::string>{"/parent/p.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 5: Multiple children with varying cache states
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, MultipleChildren_VaryingCacheState_NoEffect)
{
    std::vector<Dep> parentDeps = {
        makeContentDep(pools, "/parent/main.nix", "pm"),
    };

    // Run 1: C1 fresh (100 deps), C2 cached (50 deps)
    auto run1 = [&]() {
        epochLog.clear();
        DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);

        for (auto & d : parentDeps)
            ctx.record(d);

        // C1 fresh: 100 deps via nested scope
        {
            TestScopeAccess::pushScope(ctx);
            for (int i = 0; i < 100; i++)
                ctx.record(
                    makeContentDep(pools, "/c1/f" + std::to_string(i) + ".nix", "c1-" + std::to_string(i)));
            TestScopeAccess::popScope(ctx);
        }

        // C2 cached: 50 replayed deps (recordToEpochLog skips ownDeps)
        for (int i = 0; i < 50; i++)
            epochLog.push_back(
                makeContentDep(pools, "/c2/g" + std::to_string(i) + ".nix", "c2-" + std::to_string(i)));

        return keys(pools, TestScopeAccess::takeDeps(ctx));
    }();

    // Run 2: C1 cached (30 deps), C2 fresh (80 deps)
    auto run2 = [&]() {
        epochLog.clear();
        DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);

        for (auto & d : parentDeps)
            ctx.record(d);

        // C1 cached: 30 replayed deps (recordToEpochLog skips ownDeps)
        for (int i = 0; i < 30; i++)
            epochLog.push_back(
                makeContentDep(pools, "/c1/f" + std::to_string(i) + ".nix", "c1-" + std::to_string(i)));

        // C2 fresh: 80 deps via nested scope
        {
            TestScopeAccess::pushScope(ctx);
            for (int i = 0; i < 80; i++)
                ctx.record(
                    makeContentDep(pools, "/c2/g" + std::to_string(i) + ".nix", "c2-" + std::to_string(i)));
            TestScopeAccess::popScope(ctx);
        }

        return keys(pools, TestScopeAccess::takeDeps(ctx));
    }();

    EXPECT_EQ(run1, run2);
    EXPECT_EQ(run1, (std::vector<std::string>{"/parent/main.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 6: Nested child trackers compose naturally
//
// Child's own fresh deps AND replayed epoch deps within child scope
// stay in child's ownDeps. Parent only sees its own deps.
// ═════════════════════════════════════════════════════════════════════

TEST_F(DepStabilityTest, NestedChildren_Trackers_ComposeNaturally)
{
    epochLog.clear();
    DepRecordingContext ctx(pools, epochLog);
        TestScopeAccess::pushScope(ctx);

    ctx.record(makeContentDep(pools, "/parent/p.nix", "pp"));

    // Child evaluates fresh and also has replayed deps within its scope.
    // Both fresh and replayed deps are isolated from parent.
    {
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(pools, "/child/fresh1.nix", "cf1"));
        ctx.record(makeContentDep(pools, "/child/fresh2.nix", "cf2"));

        // Simulate epoch replay within child's scope — recordToEpochLog
        // appends to epochLog but NOT to child's ownDeps.
        // This is fine: the child's ownDeps has its fresh deps.
        epochLog.push_back(makeContentDep(pools, "/child/replayed.nix", "cr"));

        auto childDeps = keys(pools, TestScopeAccess::takeDeps(ctx));
        // Child sees its 2 fresh deps (replayed deps go to session/epoch only)
        EXPECT_EQ(childDeps.size(), 2u);
        TestScopeAccess::popScope(ctx);
    }

    // More parent deps after child
    ctx.record(makeContentDep(pools, "/parent/q.nix", "pq"));

    auto deps = keys(pools, TestScopeAccess::takeDeps(ctx));
    // Only parent deps survive — child's fresh and replayed deps isolated
    EXPECT_EQ(deps, (std::vector<std::string>{"/parent/p.nix", "/parent/q.nix"}));
}

// ═════════════════════════════════════════════════════════════════════
// Test 7: Per-sibling dep chain detects file change (SqliteTraceStorage level)
//
// Reproduces the relative-paths-lockfile regression mechanism:
// Child y has per-sibling trace-context dep on sibling x.
// Sibling x has a Content dep on a file.
// When the file changes, the chain y->x->file must detect it.
// ═════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════
// Test 9: Full integration -- rec attrset with file-reading sibling
//
// Uses TraceCacheFixture to exercise the complete flow:
// nested DepRecordingContext scopes + stamped trace-context sibling deps.
// ═════════════════════════════════════════════════════════════════════

class DepStabilityIntegrationTest : public TraceCacheFixture
{
public:
    DepStabilityIntegrationTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "dep-stability-integration");
    }
};

TEST_F(DepStabilityIntegrationTest, Stability_SiblingFileChange_PropagatedThroughDepChain)
{
    // rec { x = fromJSON(readFile f); y = x - 1; }
    // When f changes from 11 to 13, y must change from 10 to 12.
    // The file dep is on x (origExpr sibling). y detects it via
    // the sibling trace-context dep on x's trace hash.

    TempExtFile dataFile("json", "11");

    std::string expr = fmt(R"(
        rec {
            x = builtins.fromJSON (builtins.readFile "%s");
            y = x - 1;
        }
    )", dataFile.path.string());

    // Eval 1: cold cache -- ROOT + attributes evaluate fresh
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * y = root.attrs()->get(state.symbols.create("y"));
        state.forceValue(*y->value, noPos);
        EXPECT_EQ(y->value->integer().value, 10);
    }

    // Eval 2: warm cache -- ROOT materializes TracedExpr children,
    // x and y evaluate as TracedExprs (their traces are recorded)
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * y = root.attrs()->get(state.symbols.create("y"));
        state.forceValue(*y->value, noPos);
        EXPECT_EQ(y->value->integer().value, 10);
    }

    // Change the file (simulate lockfile update)
    dataFile.modify("13");
    invalidateFileCache(dataFile.path);

    // Eval 3: warm cache -- must detect file change and produce new result
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * y = root.attrs()->get(state.symbols.create("y"));
        state.forceValue(*y->value, noPos);
        EXPECT_EQ(y->value->integer().value, (int64_t)12)
            << "y must detect file change via per-sibling dep chain on x";
    }
}

// ═════════════════════════════════════════════════════════════════════
// Selective sibling invalidation tests
//
// The ValueContext dep type is intended to enable selective
// invalidation: if a child accesses sibling A but not sibling B,
// changing B should not invalidate the child. This is the key
// advantage over the old ParentContext dep (which invalidated on any
// sibling change).
//
// appendDeps() emits BOTH ParentSlot (coarse, covers entire parent)
// AND ValueContext (fine, per-sibling). The parent's trace only records
// its own direct deps (e.g., the attrset shape), not the Content deps
// of its children. So when an unaccessed sibling's file changes, the
// parent's trace still verifies — only the sibling's own trace changes.
// The ParentSlot dep checks the parent's trace hash, which is
// unaffected. The ValueContext deps check only the accessed siblings'
// trace hashes, which are also unaffected. Selective invalidation
// works.
// ═════════════════════════════════════════════════════════════════════

// ── Negative: changing an ACCESSED sibling must invalidate ───────────

TEST_F(DepStabilityIntegrationTest, Stability_AccessedSiblingChange_MustInvalidate)
{
    // rec { x = readFile f; y = x + " world"; }
    // y accesses x. When f changes, x changes, so y must re-evaluate.

    TempExtFile dataFile("txt", "hello");

    std::string expr = fmt(R"(
        rec {
            x = builtins.readFile "%s";
            y = x + " world";
        }
    )", dataFile.path.string());

    // Eval 1: cold
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * y = root.attrs()->get(state.symbols.create("y"));
        state.forceValue(*y->value, noPos);
        EXPECT_THAT(*y->value, IsStringEq("hello world"));
    }
    // Eval 2: warm (records traces)
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * y = root.attrs()->get(state.symbols.create("y"));
        state.forceValue(*y->value, noPos);
        EXPECT_THAT(*y->value, IsStringEq("hello world"));
    }

    // Change x's file
    dataFile.modify("goodbye");
    invalidateFileCache(dataFile.path);

    // Eval 3: y must see the change
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * y = root.attrs()->get(state.symbols.create("y"));
        state.forceValue(*y->value, noPos);
        EXPECT_THAT(*y->value, IsStringEq("goodbye world"))
            << "y accesses x; changing x's file must invalidate y";
    }
}

// ── Negative: changing BOTH siblings must invalidate the one that reads it ──

TEST_F(DepStabilityIntegrationTest, Stability_BothSiblingsChange_EachDetectsOwnDep)
{
    // rec { x = readFile fX; y = readFile fY; z = x + y; }
    // z accesses both x and y. Changing either file must invalidate z.

    TempExtFile fileX("txt", "aaa");
    TempExtFile fileY("txt", "bbb");

    std::string expr = fmt(R"(
        rec {
            x = builtins.readFile "%s";
            y = builtins.readFile "%s";
            z = x + y;
        }
    )", fileX.path.string(), fileY.path.string());

    // Eval 1+2: populate cache
    for (int i = 0; i < 2; i++) {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * z = root.attrs()->get(state.symbols.create("z"));
        state.forceValue(*z->value, noPos);
        EXPECT_THAT(*z->value, IsStringEq("aaabbb"));
    }

    // Change only fileY
    fileY.modify("ccc");
    invalidateFileCache(fileY.path);

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * z = root.attrs()->get(state.symbols.create("z"));
        state.forceValue(*z->value, noPos);
        EXPECT_THAT(*z->value, IsStringEq("aaaccc"))
            << "z accesses both x and y; changing y's file must invalidate z";
    }
}

// ── Positive: changing an UNACCESSED sibling should NOT invalidate ───
//
// This is the intended benefit of per-sibling ValueContext deps.
// Currently FAILS because ParentSlot is emitted alongside
// ValueContext deps, causing coarse invalidation.

TEST_F(DepStabilityIntegrationTest, Stability_UnaccessedSiblingChange_ShouldNotInvalidate)
{
    // rec { x = readFile fX; y = readFile fY; z = x + " only"; }
    // z accesses x but NOT y. Changing fY should not invalidate z,
    // because z's result depends only on x.
    //
    // z has ValueContext(x) — x didn't change, still valid.
    // z also has ParentSlot(parent), but the parent's trace only
    // tracks the attrset shape, not children's Content deps. So the
    // parent's trace hash is unaffected by y's file change.

    TempExtFile fileX("txt", "hello");
    TempExtFile fileY("txt", "unrelated");

    std::string expr = fmt(R"(
        rec {
            x = builtins.readFile "%s";
            y = builtins.readFile "%s";
            z = x + " only";
        }
    )", fileX.path.string(), fileY.path.string());

    int loaderCalls = 0;

    // Eval 1: cold
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * z = root.attrs()->get(state.symbols.create("z"));
        state.forceValue(*z->value, noPos);
        EXPECT_THAT(*z->value, IsStringEq("hello only"));
    }
    // Eval 2: warm (records traces)
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * z = root.attrs()->get(state.symbols.create("z"));
        state.forceValue(*z->value, noPos);
        EXPECT_THAT(*z->value, IsStringEq("hello only"));
    }

    // Change ONLY y's file (z never accesses y)
    fileY.modify("changed-but-irrelevant");
    invalidateFileCache(fileY.path);

    loaderCalls = 0;
    // Eval 3: z should cache-hit (only depends on x, which didn't change)
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * z = root.attrs()->get(state.symbols.create("z"));
        state.forceValue(*z->value, noPos);
        EXPECT_THAT(*z->value, IsStringEq("hello only"))
            << "z only accesses x; changing y should not change z's result";
    }
    // If selective invalidation works, z should cache-hit without
    // triggering rootLoader (full re-evaluation).
    EXPECT_EQ(loaderCalls, 0)
        << "z should cache-hit when only unaccessed sibling y changed";
}

// ── Positive: three siblings, only one accessed ─────────────────────

TEST_F(DepStabilityIntegrationTest, Stability_ThreeSiblings_OnlyOneAccessed_OthersChangeNoEffect)
{
    // rec { a = readFile fA; b = readFile fB; c = readFile fC; used = a; }
    // `used` accesses only `a`. Changing fB and fC should not invalidate `used`.

    TempExtFile fileA("txt", "alpha");
    TempExtFile fileB("txt", "beta");
    TempExtFile fileC("txt", "gamma");

    std::string expr = fmt(R"(
        rec {
            a = builtins.readFile "%s";
            b = builtins.readFile "%s";
            c = builtins.readFile "%s";
            used = a;
        }
    )", fileA.path.string(), fileB.path.string(), fileC.path.string());

    int loaderCalls = 0;

    // Eval 1+2: populate
    for (int i = 0; i < 2; i++) {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * used = root.attrs()->get(state.symbols.create("used"));
        state.forceValue(*used->value, noPos);
        EXPECT_THAT(*used->value, IsStringEq("alpha"));
    }

    // Change b and c (unaccessed by `used`)
    fileB.modify("beta-v2");
    fileC.modify("gamma-v2");
    invalidateFileCache(fileB.path);
    invalidateFileCache(fileC.path);

    loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * used = root.attrs()->get(state.symbols.create("used"));
        state.forceValue(*used->value, noPos);
        EXPECT_THAT(*used->value, IsStringEq("alpha"))
            << "used only accesses a; changing b and c should not change result";
    }
    EXPECT_EQ(loaderCalls, 0)
        << "used should cache-hit when only unaccessed siblings b,c changed";
}

// ── Positive: accessed sibling unchanged, unaccessed changed ────────

TEST_F(DepStabilityIntegrationTest, Stability_AccessedUnchanged_UnaccessedChanged_CacheHit)
{
    // rec { dep = readFile fDep; noise = readFile fNoise; result = dep + "!"; }
    // result accesses dep. fNoise changes. dep's file is unchanged.
    // result should cache-hit.

    TempExtFile fileDep("txt", "important");
    TempExtFile fileNoise("txt", "noise-v1");

    std::string expr = fmt(R"(
        rec {
            dep = builtins.readFile "%s";
            noise = builtins.readFile "%s";
            result = dep + "!";
        }
    )", fileDep.path.string(), fileNoise.path.string());

    int loaderCalls = 0;
    for (int i = 0; i < 2; i++) {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * r = root.attrs()->get(state.symbols.create("result"));
        state.forceValue(*r->value, noPos);
        EXPECT_THAT(*r->value, IsStringEq("important!"));
    }

    // Change noise only
    fileNoise.modify("noise-v2");
    invalidateFileCache(fileNoise.path);

    loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * r = root.attrs()->get(state.symbols.create("result"));
        state.forceValue(*r->value, noPos);
        EXPECT_THAT(*r->value, IsStringEq("important!"))
            << "result depends only on dep, which didn't change";
    }
    EXPECT_EQ(loaderCalls, 0)
        << "result should cache-hit when only unaccessed sibling noise changed";
}

} // namespace nix::eval_trace
