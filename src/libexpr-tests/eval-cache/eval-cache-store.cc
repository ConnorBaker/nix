#include "helpers.hh"
#include "nix/expr/eval-cache-store.hh"
#include "nix/expr/eval-result-serialise.hh"

#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/store/store-api.hh"

namespace nix::eval_cache {

using namespace nix::eval_cache::test;

class EvalCacheStoreTest : public LibExprTest
{
public:
    EvalCacheStoreTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {}

protected:
    // Writable cache dir for EvalIndexDb SQLite (sandbox has no writable $HOME)
    ScopedCacheDir cacheDir;

    static constexpr int64_t testContextHash = 0x1234567890ABCDEF;

    EvalCacheStore makeStore()
    {
        return EvalCacheStore(*state.store, state.symbols, testContextHash);
    }
};

// ── sanitizeName tests ───────────────────────────────────────────────

TEST_F(EvalCacheStoreTest, SanitizeName_ValidChars)
{
    EXPECT_EQ(EvalCacheStore::sanitizeName("hello-world_1.0"), "hello-world_1.0");
}

TEST_F(EvalCacheStoreTest, SanitizeName_InvalidChars)
{
    EXPECT_EQ(EvalCacheStore::sanitizeName("foo@bar#baz"), "foo-bar-baz");
}

TEST_F(EvalCacheStoreTest, SanitizeName_Empty)
{
    EXPECT_EQ(EvalCacheStore::sanitizeName(""), "root");
}

TEST_F(EvalCacheStoreTest, SanitizeName_LeadingDot)
{
    EXPECT_EQ(EvalCacheStore::sanitizeName(".hidden"), "-hidden");
}

TEST_F(EvalCacheStoreTest, SanitizeName_AllInvalid)
{
    EXPECT_EQ(EvalCacheStore::sanitizeName("@#$"), "---");
}

TEST_F(EvalCacheStoreTest, SanitizeName_AllowedSpecialChars)
{
    EXPECT_EQ(EvalCacheStore::sanitizeName("a+b=c?d"), "a+b=c?d");
}

// ── buildAttrPath tests ──────────────────────────────────────────────

TEST_F(EvalCacheStoreTest, BuildAttrPath_Empty)
{
    EXPECT_EQ(EvalCacheStore::buildAttrPath({}), "");
}

TEST_F(EvalCacheStoreTest, BuildAttrPath_Single)
{
    EXPECT_EQ(EvalCacheStore::buildAttrPath({"packages"}), "packages");
}

TEST_F(EvalCacheStoreTest, BuildAttrPath_Multiple)
{
    auto path = EvalCacheStore::buildAttrPath({"packages", "x86_64-linux", "hello"});
    // Should be null-byte separated
    std::string expected = "packages";
    expected.push_back('\0');
    expected.append("x86_64-linux");
    expected.push_back('\0');
    expected.append("hello");
    EXPECT_EQ(path, expected);
}

// ── depTypeString tests ──────────────────────────────────────────────

TEST_F(EvalCacheStoreTest, DepTypeString_AllTypes)
{
    EXPECT_EQ(depTypeString(DepType::Content), "content");
    EXPECT_EQ(depTypeString(DepType::Directory), "directory");
    EXPECT_EQ(depTypeString(DepType::Existence), "existence");
    EXPECT_EQ(depTypeString(DepType::EnvVar), "envvar");
    EXPECT_EQ(depTypeString(DepType::CurrentTime), "current-time");
    EXPECT_EQ(depTypeString(DepType::System), "system");
    EXPECT_EQ(depTypeString(DepType::UnhashedFetch), "unhashed-fetch");
    EXPECT_EQ(depTypeString(DepType::ParentContext), "parent-context");
    EXPECT_EQ(depTypeString(DepType::CopiedPath), "copied-path");
    EXPECT_EQ(depTypeString(DepType::Exec), "exec");
    EXPECT_EQ(depTypeString(DepType::NARContent), "nar-content");
}

// ── storeTrace tests ─────────────────────────────────────────────────

TEST_F(EvalCacheStoreTest, StoreTrace_ContentAddressed)
{
    // Two identical traces should produce the same store path
    auto sb = makeStore();
    std::vector<Dep> deps = {makeContentDep("/test.nix", "hello")};
    AttrValue value = string_t{"hello", {}};

    auto compressed = serializeDepSet(sortAndDedupDeps(deps));
    auto depSetPath = sb.storeDepSet(compressed);

    auto cbor1 = serializeEvalTrace(value, std::nullopt,
                                     testContextHash, depSetPath, state.symbols, *state.store);
    auto cbor2 = serializeEvalTrace(value, std::nullopt,
                                     testContextHash, depSetPath, state.symbols, *state.store);

    auto path1 = sb.storeTrace("eval-test", cbor1, {depSetPath});
    auto path2 = sb.storeTrace("eval-test", cbor2, {depSetPath});

    EXPECT_EQ(state.store->printStorePath(path1),
              state.store->printStorePath(path2));
}

TEST_F(EvalCacheStoreTest, StoreTrace_DifferentDeps_DifferentPaths)
{
    auto sb = makeStore();
    AttrValue value = string_t{"hello", {}};

    auto compressed1 = serializeDepSet(sortAndDedupDeps({makeContentDep("/a.nix", "a")}));
    auto depSetPath1 = sb.storeDepSet(compressed1);
    auto compressed2 = serializeDepSet(sortAndDedupDeps({makeContentDep("/b.nix", "b")}));
    auto depSetPath2 = sb.storeDepSet(compressed2);

    auto cbor1 = serializeEvalTrace(
        value, std::nullopt,
        testContextHash, depSetPath1, state.symbols, *state.store);
    auto cbor2 = serializeEvalTrace(
        value, std::nullopt,
        testContextHash, depSetPath2, state.symbols, *state.store);

    auto path1 = sb.storeTrace("eval-test", cbor1, {depSetPath1});
    auto path2 = sb.storeTrace("eval-test", cbor2, {depSetPath2});

    EXPECT_NE(state.store->printStorePath(path1),
              state.store->printStorePath(path2));
}

TEST_F(EvalCacheStoreTest, StoreTrace_DifferentValues_DifferentPaths)
{
    auto sb = makeStore();
    std::vector<Dep> deps = {makeContentDep("/test.nix", "x")};

    auto compressed = serializeDepSet(sortAndDedupDeps(deps));
    auto depSetPath = sb.storeDepSet(compressed);

    auto cbor1 = serializeEvalTrace(string_t{"one", {}}, std::nullopt,
                                     testContextHash, depSetPath, state.symbols, *state.store);
    auto cbor2 = serializeEvalTrace(string_t{"two", {}}, std::nullopt,
                                     testContextHash, depSetPath, state.symbols, *state.store);

    auto path1 = sb.storeTrace("eval-test", cbor1, {depSetPath});
    auto path2 = sb.storeTrace("eval-test", cbor2, {depSetPath});

    EXPECT_NE(state.store->printStorePath(path1),
              state.store->printStorePath(path2));
}

TEST_F(EvalCacheStoreTest, StoreTrace_WithParentReference)
{
    auto sb = makeStore();

    auto emptyDeps = serializeDepSet({});
    auto depSetPath = sb.storeDepSet(emptyDeps);

    // Store parent trace
    auto parentCbor = serializeEvalTrace(string_t{"parent", {}}, std::nullopt,
                                          testContextHash, depSetPath, state.symbols, *state.store);
    auto parentPath = sb.storeTrace("eval-parent", parentCbor, {depSetPath});

    // Store child trace referencing parent
    auto childCbor = serializeEvalTrace(string_t{"child", {}}, parentPath,
                                         std::nullopt, depSetPath, state.symbols, *state.store);
    auto childPath = sb.storeTrace("eval-child", childCbor, {parentPath, depSetPath});

    EXPECT_NE(state.store->printStorePath(parentPath),
              state.store->printStorePath(childPath));
}

// ── loadTrace tests ──────────────────────────────────────────────────

TEST_F(EvalCacheStoreTest, LoadTrace_Roundtrip)
{
    auto sb = makeStore();
    std::vector<Dep> deps = {
        makeContentDep("/test.nix", "hello"),
        makeEnvVarDep("HOME", "/home/user"),
    };
    AttrValue value = string_t{"result", {}};

    auto compressed = serializeDepSet(sortAndDedupDeps(deps));
    auto depSetPath = sb.storeDepSet(compressed);

    auto cbor = serializeEvalTrace(value, std::nullopt,
                                    testContextHash, depSetPath, state.symbols, *state.store);
    auto tracePath = sb.storeTrace("eval-test", cbor, {depSetPath});

    auto trace = sb.loadTrace(tracePath);
    assertAttrValueEquals(value, trace.result, state.symbols);
    EXPECT_FALSE(trace.parent.has_value());
    ASSERT_TRUE(trace.contextHash.has_value());
    EXPECT_EQ(*trace.contextHash, testContextHash);
    EXPECT_EQ(state.store->printStorePath(trace.depSetPath),
              state.store->printStorePath(depSetPath));
    // Load deps from the dep set blob and verify count
    auto deps2 = sb.loadDepSet(trace.depSetPath);
    EXPECT_EQ(deps2.size(), 2u);
}

TEST_F(EvalCacheStoreTest, LoadTrace_AllValueTypes)
{
    auto sb = makeStore();

    auto emptyDeps = serializeDepSet({});
    auto depSetPath = sb.storeDepSet(emptyDeps);

    auto testRoundtrip = [&](const AttrValue & value, std::string_view name) {
        auto cbor = serializeEvalTrace(value, std::nullopt,
                                        testContextHash, depSetPath, state.symbols, *state.store);
        auto tracePath = sb.storeTrace(std::string("eval-") + std::string(name), cbor, {depSetPath});
        auto trace = sb.loadTrace(tracePath);
        assertAttrValueEquals(value, trace.result, state.symbols);
    };

    testRoundtrip(string_t{"hello", {}}, "str");
    testRoundtrip(true, "bool-t");
    testRoundtrip(false, "bool-f");
    testRoundtrip(int_t{NixInt{42}}, "int");
    testRoundtrip(null_t{}, "null");
    testRoundtrip(float_t{3.14}, "float");
    testRoundtrip(path_t{"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-test"}, "path");
    testRoundtrip(failed_t{}, "failed");
    testRoundtrip(missing_t{}, "missing");
    testRoundtrip(misc_t{}, "misc");
}

TEST_F(EvalCacheStoreTest, LoadTrace_WithParent)
{
    auto sb = makeStore();

    auto emptyDeps = serializeDepSet({});
    auto depSetPath = sb.storeDepSet(emptyDeps);

    // Store parent trace
    auto parentCbor = serializeEvalTrace(string_t{"parent", {}}, std::nullopt,
                                          testContextHash, depSetPath, state.symbols, *state.store);
    auto parentPath = sb.storeTrace("eval-parent", parentCbor, {depSetPath});

    // Store child trace referencing parent
    auto childCbor = serializeEvalTrace(string_t{"child", {}}, parentPath,
                                         std::nullopt, depSetPath, state.symbols, *state.store);
    auto childPath = sb.storeTrace("eval-child", childCbor, {parentPath, depSetPath});

    auto trace = sb.loadTrace(childPath);
    assertAttrValueEquals(string_t{"child", {}}, trace.result, state.symbols);
    ASSERT_TRUE(trace.parent.has_value());
    EXPECT_EQ(state.store->printStorePath(*trace.parent),
              state.store->printStorePath(parentPath));
    // Child should not have contextHash (not root)
    EXPECT_FALSE(trace.contextHash.has_value());
}

// ── Cold path tests ──────────────────────────────────────────────────

TEST_F(EvalCacheStoreTest, ColdStore_PopulatesIndex)
{
    auto sb = makeStore();
    auto tracePath = sb.coldStore(
        "", "root", string_t{"hello", {}}, {}, std::nullopt, true);

    auto entry = sb.index.lookup(testContextHash, "", *state.store);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(state.store->printStorePath(entry->tracePath),
              state.store->printStorePath(tracePath));
}

TEST_F(EvalCacheStoreTest, ColdStore_ResultReadable)
{
    auto sb = makeStore();
    AttrValue input = string_t{"hello world", {}};
    auto tracePath = sb.coldStore("", "root", input, {}, std::nullopt, true);

    auto trace = sb.loadTrace(tracePath);
    assertAttrValueEquals(input, trace.result, state.symbols);
}

TEST_F(EvalCacheStoreTest, ColdStore_WithDeps)
{
    auto sb = makeStore();
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "a"),
        makeEnvVarDep("HOME", "/home"),
    };

    auto tracePath = sb.coldStore("", "root", int_t{NixInt{42}}, deps, std::nullopt, true);

    auto loadedDeps = sb.loadDepsForTrace(tracePath);
    EXPECT_EQ(loadedDeps.size(), 2u);
}

TEST_F(EvalCacheStoreTest, ColdStore_VolatileDep_NotSessionCached)
{
    auto sb = makeStore();
    std::vector<Dep> deps = {makeCurrentTimeDep()};

    auto tracePath = sb.coldStore("", "root", null_t{}, deps, std::nullopt, true);

    // CurrentTime dep -> should NOT be in validatedTraces
    EXPECT_FALSE(sb.validatedTraces.count(tracePath));
}

TEST_F(EvalCacheStoreTest, ColdStore_NonVolatile_SessionCached)
{
    auto sb = makeStore();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};

    auto tracePath = sb.coldStore("", "root", null_t{}, deps, std::nullopt, true);

    // Non-volatile dep -> SHOULD be in validatedTraces
    EXPECT_TRUE(sb.validatedTraces.count(tracePath));
}

TEST_F(EvalCacheStoreTest, ColdStore_ParentContextFiltered)
{
    auto sb = makeStore();
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "a"),
        Dep{"", "", DepHashValue(std::string("parent-hash")), DepType::ParentContext},
    };

    auto tracePath = sb.coldStore("", "root", null_t{}, deps, std::nullopt, true);

    auto loadedDeps = sb.loadDepsForTrace(tracePath);
    // ParentContext should be filtered out -- only 1 dep stored
    EXPECT_EQ(loadedDeps.size(), 1u);
    EXPECT_EQ(loadedDeps[0].type, DepType::Content);
}

TEST_F(EvalCacheStoreTest, ColdStore_WithParentTrace)
{
    auto sb = makeStore();

    // Store parent
    auto parentPath = sb.coldStore("", "root", string_t{"parent-val", {}},
                                    {makeContentDep("/a.nix", "a")}, std::nullopt, true);

    // Store child referencing parent
    auto childPath = sb.coldStore("child", "child", string_t{"child-val", {}},
                                   {makeEnvVarDep("FOO", "bar")}, parentPath, false);

    auto childTrace = sb.loadTrace(childPath);
    assertAttrValueEquals(string_t{"child-val", {}}, childTrace.result, state.symbols);
    ASSERT_TRUE(childTrace.parent.has_value());
    EXPECT_EQ(state.store->printStorePath(*childTrace.parent),
              state.store->printStorePath(parentPath));
}

TEST_F(EvalCacheStoreTest, ColdStore_RootHasContextHash)
{
    auto sb = makeStore();
    auto tracePath = sb.coldStore("", "root", string_t{"val", {}}, {}, std::nullopt, true);

    auto trace = sb.loadTrace(tracePath);
    ASSERT_TRUE(trace.contextHash.has_value());
    EXPECT_EQ(*trace.contextHash, testContextHash);
}

TEST_F(EvalCacheStoreTest, ColdStore_NonRootNoContextHash)
{
    auto sb = makeStore();
    auto parentPath = sb.coldStore("", "root", string_t{"parent", {}}, {}, std::nullopt, true);
    auto childPath = sb.coldStore("child", "child", string_t{"child", {}}, {}, parentPath, false);

    auto trace = sb.loadTrace(childPath);
    EXPECT_FALSE(trace.contextHash.has_value());
}

TEST_F(EvalCacheStoreTest, ColdStore_Deterministic)
{
    // Same inputs should produce the same trace path (content-addressed)
    auto sb = makeStore();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};
    AttrValue value = string_t{"result", {}};

    auto path1 = sb.coldStore("", "root", value, deps, std::nullopt, true);
    auto path2 = sb.coldStore("", "root", value, deps, std::nullopt, true);

    EXPECT_EQ(state.store->printStorePath(path1),
              state.store->printStorePath(path2));
}

// ── validateTrace tests ──────────────────────────────────────────────

TEST_F(EvalCacheStoreTest, ValidateTrace_EnvVar_Valid)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR", "expected_value");

    auto sb = makeStore();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR", "expected_value")};
    auto tracePath = sb.coldStore("", "root", null_t{}, deps, std::nullopt, true);

    // Clear session cache so validateTrace actually checks
    sb.clearSessionCaches();

    bool valid = sb.validateTrace(tracePath, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(EvalCacheStoreTest, ValidateTrace_EnvVar_Invalid)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR", "new_value");

    auto sb = makeStore();
    // Create trace with OLD expected hash
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR", "old_value")};
    auto tracePath = sb.coldStore("", "root", null_t{}, deps, std::nullopt, true);

    // Clear session cache so validateTrace actually checks
    sb.clearSessionCaches();

    bool valid = sb.validateTrace(tracePath, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheStoreTest, ValidateTrace_CurrentTime_AlwaysFails)
{
    auto sb = makeStore();
    std::vector<Dep> deps = {makeCurrentTimeDep()};
    auto tracePath = sb.coldStore("", "root", null_t{}, deps, std::nullopt, true);

    // CurrentTime is volatile, so not session-cached (already tested above)
    bool valid = sb.validateTrace(tracePath, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheStoreTest, ValidateTrace_Exec_AlwaysFails)
{
    auto sb = makeStore();
    std::vector<Dep> deps = {makeExecDep()};
    auto tracePath = sb.coldStore("", "root", null_t{}, deps, std::nullopt, true);

    bool valid = sb.validateTrace(tracePath, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheStoreTest, ValidateTrace_SessionCacheHit)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR2", "val");

    auto sb = makeStore();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR2", "val")};
    auto tracePath = sb.coldStore("", "root", null_t{}, deps, std::nullopt, true);

    // coldStore with non-volatile deps should have session-cached it
    EXPECT_TRUE(sb.validatedTraces.count(tracePath));
    // Second call should use session cache (instant return)
    EXPECT_TRUE(sb.validateTrace(tracePath, {}, state));
}

TEST_F(EvalCacheStoreTest, ValidateTrace_NoDeps_Valid)
{
    auto sb = makeStore();
    auto tracePath = sb.coldStore("", "root", string_t{"val", {}}, {}, std::nullopt, true);

    sb.clearSessionCaches();

    bool valid = sb.validateTrace(tracePath, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(EvalCacheStoreTest, ValidateTrace_ParentInvalid)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "current_value");

    auto sb = makeStore();

    // Create parent with stale dep
    std::vector<Dep> staleDeps = {makeEnvVarDep("NIX_TEST_PARENT", "stale_value")};
    auto parentPath = sb.coldStore("", "root", string_t{"parent", {}},
                                    staleDeps, std::nullopt, true);

    // Create child with no direct deps, but parent is invalid
    auto childPath = sb.coldStore("child", "child", string_t{"child", {}},
                                   {}, parentPath, false);

    // Clear session cache
    sb.clearSessionCaches();

    bool valid = sb.validateTrace(childPath, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheStoreTest, ValidateTrace_ParentValid)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "correct_value");

    auto sb = makeStore();

    // Create parent with correct dep
    std::vector<Dep> parentDeps = {makeEnvVarDep("NIX_TEST_PARENT", "correct_value")};
    auto parentPath = sb.coldStore("", "root", string_t{"parent", {}},
                                    parentDeps, std::nullopt, true);

    // Create child referencing valid parent
    auto childPath = sb.coldStore("child", "child", string_t{"child", {}},
                                   {}, parentPath, false);

    // Clear session cache
    sb.clearSessionCaches();

    bool valid = sb.validateTrace(childPath, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(EvalCacheStoreTest, ValidateTrace_MultipleDeps_OneInvalid)
{
    ScopedEnvVar env1("NIX_TEST_VALID", "current_value");
    ScopedEnvVar env2("NIX_TEST_STALE", "new_value");

    auto sb = makeStore();
    std::vector<Dep> deps = {
        makeEnvVarDep("NIX_TEST_VALID", "current_value"),  // valid
        makeEnvVarDep("NIX_TEST_STALE", "old_value"),      // invalid (different hash)
    };
    auto tracePath = sb.coldStore("", "root", null_t{}, deps, std::nullopt, true);

    sb.clearSessionCaches();

    bool valid = sb.validateTrace(tracePath, {}, state);
    EXPECT_FALSE(valid);
}

// ── Cold -> Warm roundtrip ───────────────────────────────────────────

TEST_F(EvalCacheStoreTest, ColdWarm_Roundtrip)
{
    ScopedEnvVar env("NIX_WARM_TEST", "stable");

    auto sb = makeStore();
    AttrValue input = string_t{"cached value", {}};
    std::vector<Dep> deps = {makeEnvVarDep("NIX_WARM_TEST", "stable")};

    sb.coldStore("", "root", input, deps, std::nullopt, true);

    // Warm path should find it
    auto result = sb.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(input, result->value, state.symbols);
}

TEST_F(EvalCacheStoreTest, ColdWarm_Roundtrip_TracePath)
{
    ScopedEnvVar env("NIX_WARM_TEST2", "stable");

    auto sb = makeStore();
    AttrValue input = int_t{NixInt{99}};
    std::vector<Dep> deps = {makeEnvVarDep("NIX_WARM_TEST2", "stable")};

    auto coldTracePath = sb.coldStore("", "root", input, deps, std::nullopt, true);

    auto result = sb.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    // Warm result's tracePath should match what coldStore returned
    EXPECT_EQ(state.store->printStorePath(result->tracePath),
              state.store->printStorePath(coldTracePath));
}

TEST_F(EvalCacheStoreTest, WarmPath_NoEntry)
{
    auto sb = makeStore();
    auto result = sb.warmPath("nonexistent", {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(EvalCacheStoreTest, WarmPath_InvalidatedDeps)
{
    // Cold store with one env var value, then change it.
    // Warm path should fail (no recovery possible with no prior matching trace).
    ScopedEnvVar env("NIX_WARM_INVALID", "value1");

    auto sb = makeStore();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_WARM_INVALID", "value1")};
    sb.coldStore("", "root", string_t{"old", {}}, deps, std::nullopt, true);

    // Change env var
    setenv("NIX_WARM_INVALID", "value2", 1);

    // Clear session cache to force re-validation
    sb.clearSessionCaches();

    auto result = sb.warmPath("", {}, state);
    // Should fail because dep hash no longer matches and no recovery target exists
    EXPECT_FALSE(result.has_value());
}

// ── Three-phase recovery tests ───────────────────────────────────────

TEST_F(EvalCacheStoreTest, Phase1_StillWorks)
{
    // Phase 1: same structure, different values, revert → succeeds
    ScopedEnvVar env("NIX_P1_TEST", "value_A");

    auto sb = makeStore();
    std::vector<Dep> depsA = {makeEnvVarDep("NIX_P1_TEST", "value_A")};
    sb.coldStore("", "root", string_t{"result_A", {}}, depsA, std::nullopt, true);

    // Change env var to value_B and cold store
    setenv("NIX_P1_TEST", "value_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep("NIX_P1_TEST", "value_B")};
    sb.coldStore("", "root", string_t{"result_B", {}}, depsB, std::nullopt, true);

    // Revert to value_A — Phase 1 should find the trace from first cold store
    setenv("NIX_P1_TEST", "value_A", 1);
    sb.clearSessionCaches();

    auto result = sb.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"result_A", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheStoreTest, Phase2_ParentLookup_NoDeps)
{
    // Phase 2: dep-less child, parent changed. Phase 2 finds by parent identity.
    ScopedEnvVar env("NIX_P2_ROOT", "val1");

    auto sb = makeStore();

    // Cold store root with val1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P2_ROOT", "val1")};
    auto rootTrace1 = sb.coldStore("", "root", string_t{"root1", {}}, rootDeps1, std::nullopt, true);

    // Cold store child (no deps) referencing root1
    sb.coldStore("child", "child", string_t{"child1", {}}, {}, rootTrace1, false);

    // Cold store root with val2
    setenv("NIX_P2_ROOT", "val2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P2_ROOT", "val2")};
    auto rootTrace2 = sb.coldStore("", "root", string_t{"root2", {}}, rootDeps2, std::nullopt, true);

    // Cold store child referencing root2
    sb.coldStore("child", "child", string_t{"child2", {}}, {}, rootTrace2, false);

    // Revert to val1: root should be recovered, then child via Phase 2
    setenv("NIX_P2_ROOT", "val1", 1);
    sb.clearSessionCaches();

    // Root recovery (Phase 1)
    auto rootResult = sb.warmPath("", {}, state);
    ASSERT_TRUE(rootResult.has_value());
    assertAttrValueEquals(string_t{"root1", {}}, rootResult->value, state.symbols);

    // Child recovery (Phase 2 — uses parent hint)
    auto childResult = sb.warmPath("child", {}, state, rootResult->tracePath);
    ASSERT_TRUE(childResult.has_value());
    assertAttrValueEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(EvalCacheStoreTest, Phase2_ParentLookup_WithDeps)
{
    // Phase 2: child with deps, parent changed. Phase 2 finds.
    ScopedEnvVar env1("NIX_P2W_ROOT", "rval1");
    ScopedEnvVar env2("NIX_P2W_CHILD", "cval");

    auto sb = makeStore();

    // Cold store root with rval1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P2W_ROOT", "rval1")};
    auto rootTrace1 = sb.coldStore("", "root", string_t{"root1", {}}, rootDeps1, std::nullopt, true);

    // Cold store child with stable dep + parent
    std::vector<Dep> childDeps = {makeEnvVarDep("NIX_P2W_CHILD", "cval")};
    sb.coldStore("child", "child", string_t{"child1", {}}, childDeps, rootTrace1, false);

    // Cold store root with rval2
    setenv("NIX_P2W_ROOT", "rval2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P2W_ROOT", "rval2")};
    auto rootTrace2 = sb.coldStore("", "root", string_t{"root2", {}}, rootDeps2, std::nullopt, true);

    // Cold store child referencing root2
    sb.coldStore("child", "child", string_t{"child2", {}}, childDeps, rootTrace2, false);

    // Revert to rval1
    setenv("NIX_P2W_ROOT", "rval1", 1);
    sb.clearSessionCaches();

    auto rootResult = sb.warmPath("", {}, state);
    ASSERT_TRUE(rootResult.has_value());

    auto childResult = sb.warmPath("child", {}, state, rootResult->tracePath);
    ASSERT_TRUE(childResult.has_value());
    assertAttrValueEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(EvalCacheStoreTest, Phase2_CascadeThroughTree)
{
    // Root → child1 → child2 (grandchild), all via Phase 2
    ScopedEnvVar env("NIX_P2C_ROOT", "v1");

    auto sb = makeStore();

    // Build null-byte-separated attr path for grandchild
    std::string c2AttrPath = "c1";
    c2AttrPath.push_back('\0');
    c2AttrPath.append("c2");

    // Cold store: root → child1 → child2
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P2C_ROOT", "v1")};
    auto root1 = sb.coldStore("", "root", string_t{"r1", {}}, rootDeps1, std::nullopt, true);
    auto child1_1 = sb.coldStore("c1", "c1", string_t{"c1v1", {}}, {}, root1, false);
    sb.coldStore(c2AttrPath, "c2", string_t{"c2v1", {}}, {}, child1_1, false);

    // Second cold store
    setenv("NIX_P2C_ROOT", "v2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P2C_ROOT", "v2")};
    auto root2 = sb.coldStore("", "root", string_t{"r2", {}}, rootDeps2, std::nullopt, true);
    auto child1_2 = sb.coldStore("c1", "c1", string_t{"c1v2", {}}, {}, root2, false);
    sb.coldStore(c2AttrPath, "c2", string_t{"c2v2", {}}, {}, child1_2, false);

    // Revert to v1
    setenv("NIX_P2C_ROOT", "v1", 1);
    sb.clearSessionCaches();

    auto rootR = sb.warmPath("", {}, state);
    ASSERT_TRUE(rootR.has_value());
    assertAttrValueEquals(string_t{"r1", {}}, rootR->value, state.symbols);

    auto c1R = sb.warmPath("c1", {}, state, rootR->tracePath);
    ASSERT_TRUE(c1R.has_value());
    assertAttrValueEquals(string_t{"c1v1", {}}, c1R->value, state.symbols);

    auto c2R = sb.warmPath(c2AttrPath, {}, state, c1R->tracePath);
    ASSERT_TRUE(c2R.has_value());
    assertAttrValueEquals(string_t{"c2v1", {}}, c2R->value, state.symbols);
}

TEST_F(EvalCacheStoreTest, Phase3_DepStructMismatch)
{
    // Phase 3: different dep structures between two cold evals.
    // Phase 1 fails because keys differ. Phase 3 finds matching group.
    ScopedEnvVar env1("NIX_P3_A", "aval");
    ScopedEnvVar env2("NIX_P3_B", "bval");

    auto sb = makeStore();

    // First cold store: only dep A
    std::vector<Dep> deps1 = {makeEnvVarDep("NIX_P3_A", "aval")};
    sb.coldStore("", "root", string_t{"result1", {}}, deps1, std::nullopt, true);

    // Second cold store: deps A + B (different structure)
    std::vector<Dep> deps2 = {
        makeEnvVarDep("NIX_P3_A", "aval"),
        makeEnvVarDep("NIX_P3_B", "bval"),
    };
    sb.coldStore("", "root", string_t{"result2", {}}, deps2, std::nullopt, true);

    // Now index points to trace with deps A+B.
    // Change B to invalidate that trace's deps.
    setenv("NIX_P3_B", "bval_new", 1);
    sb.clearSessionCaches();

    // Phase 1 will compute new hash for A+B (bval_new) — no match.
    // Phase 3 will scan struct groups, find the group with only dep A,
    // compute current hash for A → match the first trace.
    auto result = sb.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"result1", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheStoreTest, Phase3_MultipleStructGroups)
{
    // 3 cold stores with different dep structures. Phase 3 iterates groups.
    ScopedEnvVar env1("NIX_P3M_A", "a");
    ScopedEnvVar env2("NIX_P3M_B", "b");
    ScopedEnvVar env3("NIX_P3M_C", "c");

    auto sb = makeStore();

    // Structure 1: only A
    sb.coldStore("", "root", string_t{"r1", {}},
                  {makeEnvVarDep("NIX_P3M_A", "a")}, std::nullopt, true);

    // Structure 2: A + B
    sb.coldStore("", "root", string_t{"r2", {}},
                  {makeEnvVarDep("NIX_P3M_A", "a"), makeEnvVarDep("NIX_P3M_B", "b")},
                  std::nullopt, true);

    // Structure 3: A + B + C (latest, in index)
    sb.coldStore("", "root", string_t{"r3", {}},
                  {makeEnvVarDep("NIX_P3M_A", "a"), makeEnvVarDep("NIX_P3M_B", "b"),
                   makeEnvVarDep("NIX_P3M_C", "c")},
                  std::nullopt, true);

    // Change C → invalidates struct 3 in index
    setenv("NIX_P3M_C", "c_new", 1);
    sb.clearSessionCaches();

    auto result = sb.warmPath("", {}, state);
    // Should recover r1 or r2 (whichever group is scanned first with valid deps)
    ASSERT_TRUE(result.has_value());
    // Both r1 and r2 have valid deps (A and B unchanged), either is acceptable
    auto & val = std::get<string_t>(result->value);
    EXPECT_TRUE(val.first == "r1" || val.first == "r2");
}

TEST_F(EvalCacheStoreTest, Phase3_EmptyDeps)
{
    // Struct group with zero deps
    auto sb = makeStore();

    sb.coldStore("", "root", string_t{"empty1", {}}, {}, std::nullopt, true);
    sb.coldStore("", "root", string_t{"empty2", {}},
                  {makeEnvVarDep("NIX_P3E_X", "x")}, std::nullopt, true);

    // Invalidate X
    ScopedEnvVar env("NIX_P3E_X", "x_new");
    sb.clearSessionCaches();

    auto result = sb.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"empty1", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheStoreTest, Phase2_FallbackToPhase3)
{
    // No parent hint — Phase 2 skipped, Phase 3 succeeds
    ScopedEnvVar env1("NIX_P2F_A", "a");
    ScopedEnvVar env2("NIX_P2F_B", "b");

    auto sb = makeStore();

    // Two structures
    sb.coldStore("", "root", string_t{"r1", {}},
                  {makeEnvVarDep("NIX_P2F_A", "a")}, std::nullopt, true);
    sb.coldStore("", "root", string_t{"r2", {}},
                  {makeEnvVarDep("NIX_P2F_A", "a"), makeEnvVarDep("NIX_P2F_B", "b")},
                  std::nullopt, true);

    // Invalidate B
    setenv("NIX_P2F_B", "b_new", 1);
    sb.clearSessionCaches();

    // No parent hint (root attr) — Phase 2 skipped
    auto result = sb.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"r1", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheStoreTest, AllPhaseFail_Volatile)
{
    // CurrentTime dep → immediate abort, nullopt
    auto sb = makeStore();

    std::vector<Dep> deps = {makeCurrentTimeDep()};
    sb.coldStore("", "root", null_t{}, deps, std::nullopt, true);

    auto result = sb.warmPath("", {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(EvalCacheStoreTest, RecoveryUpdatesIndex)
{
    // After Phase 1 recovery, next warmPath succeeds directly (index updated)
    ScopedEnvVar env("NIX_RUI_TEST", "val_A");

    auto sb = makeStore();

    // Two cold stores
    std::vector<Dep> depsA = {makeEnvVarDep("NIX_RUI_TEST", "val_A")};
    sb.coldStore("", "root", string_t{"rA", {}}, depsA, std::nullopt, true);

    setenv("NIX_RUI_TEST", "val_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep("NIX_RUI_TEST", "val_B")};
    sb.coldStore("", "root", string_t{"rB", {}}, depsB, std::nullopt, true);

    // Revert to A, recovery should update index
    setenv("NIX_RUI_TEST", "val_A", 1);
    sb.clearSessionCaches();

    auto r1 = sb.warmPath("", {}, state);
    ASSERT_TRUE(r1.has_value());

    // Second warm path should succeed via direct index lookup (no recovery needed)
    sb.clearSessionCaches();
    auto r2 = sb.warmPath("", {}, state);
    ASSERT_TRUE(r2.has_value());
    assertAttrValueEquals(string_t{"rA", {}}, r2->value, state.symbols);
}

TEST_F(EvalCacheStoreTest, Phase3_GCdRepresentative)
{
    // Representative trace GC'd → group skipped gracefully, next group tried
    ScopedEnvVar env("NIX_GC_A", "a");

    auto sb = makeStore();

    // First store with struct 1
    sb.coldStore("", "root", string_t{"r1", {}},
                  {makeEnvVarDep("NIX_GC_A", "a")}, std::nullopt, true);

    // Second store with struct 2 (latest, in index)
    sb.coldStore("", "root", string_t{"r2", {}},
                  {makeEnvVarDep("NIX_GC_A", "a"), makeEnvVarDep("NIX_GC_B", "b")},
                  std::nullopt, true);

    // Invalidate B — need to recover from Phase 3
    ScopedEnvVar env2("NIX_GC_B", "b_new");
    sb.clearSessionCaches();

    // Phase 3 should iterate groups: some may be GC'd (store is dummy://,
    // paths exist because we just stored them). Test that valid groups work.
    auto result = sb.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"r1", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheStoreTest, Phase1_2_3_Cascade)
{
    // All three phases attempted in order — Phase 1 and 2 fail, Phase 3 succeeds
    ScopedEnvVar env1("NIX_CASCADE_A", "a");
    ScopedEnvVar env2("NIX_CASCADE_B", "b");

    auto sb = makeStore();

    // Structure 1: only dep A (this is what we want to recover)
    sb.coldStore("child", "child", string_t{"target", {}},
                  {makeEnvVarDep("NIX_CASCADE_A", "a")}, std::nullopt, false);

    // Structure 2: deps A + B (latest, in index)
    sb.coldStore("child", "child", string_t{"latest", {}},
                  {makeEnvVarDep("NIX_CASCADE_A", "a"), makeEnvVarDep("NIX_CASCADE_B", "b")},
                  std::nullopt, false);

    // Invalidate B → Phase 1 fails (structure A+B, B can't match new value)
    // Phase 2 has no parent hint → skipped
    // Phase 3 finds structure 1 (only A) → succeeds
    setenv("NIX_CASCADE_B", "b_new", 1);
    sb.clearSessionCaches();

    auto result = sb.warmPath("child", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"target", {}}, result->value, state.symbols);
}

// ── Store GC interaction tests ───────────────────────────────────────
//
// Simulate garbage collection by creating a second EvalCacheStore with
// a fresh dummy:// store. Both stores share the same eval-index-v2.sqlite
// (via NIX_CACHE_HOME set by ScopedCacheDir), so the fresh store sees
// index entries pointing to trace paths that don't exist in its store.
//
// The first store is destroyed before creating the second, ensuring
// the EvalIndexDb transaction is committed and visible to the new
// SQLite connection.

TEST_F(EvalCacheStoreTest, GC_WarmPath_ReturnsNullopt)
{
    // Index entry exists but trace path is GC'd → warmPath returns nullopt
    ScopedEnvVar env("NIX_GC_WP", "val");

    {
        auto sb = makeStore();
        sb.coldStore("", "root", string_t{"cached", {}},
                      {makeEnvVarDep("NIX_GC_WP", "val")}, std::nullopt, true);
    } // sb destroyed → EvalIndexDb transaction committed

    // Fresh store: index entries exist but trace store paths are missing
    auto freshStore = openStore("dummy://", {{"read-only", "false"}});
    EvalCacheStore sb2(*freshStore, state.symbols, testContextHash);

    auto result = sb2.warmPath("", {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(EvalCacheStoreTest, GC_ValidateTrace_ReturnsFalse)
{
    // validateTrace returns false when trace path doesn't exist in store
    ScopedEnvVar env("NIX_GC_VT", "val");

    std::optional<StorePath> tracePath;
    {
        auto sb = makeStore();
        tracePath = sb.coldStore("", "root", null_t{},
                                  {makeEnvVarDep("NIX_GC_VT", "val")}, std::nullopt, true);
    }

    auto freshStore = openStore("dummy://", {{"read-only", "false"}});
    EvalCacheStore sb2(*freshStore, state.symbols, testContextHash);

    EXPECT_FALSE(sb2.validateTrace(*tracePath, {}, state));
}

TEST_F(EvalCacheStoreTest, GC_ParentAndChild_BothGCd)
{
    // Both parent and child traces GC'd → warmPath returns nullopt for both
    ScopedEnvVar env("NIX_GC_PC", "val");

    {
        auto sb = makeStore();
        auto parent = sb.coldStore("", "root", string_t{"parent", {}},
                                    {makeEnvVarDep("NIX_GC_PC", "val")}, std::nullopt, true);
        sb.coldStore("child", "child", string_t{"child", {}}, {}, parent, false);
    }

    auto freshStore = openStore("dummy://", {{"read-only", "false"}});
    EvalCacheStore sb2(*freshStore, state.symbols, testContextHash);

    EXPECT_FALSE(sb2.warmPath("", {}, state).has_value());
    EXPECT_FALSE(sb2.warmPath("child", {}, state).has_value());
}

TEST_F(EvalCacheStoreTest, GC_Recovery_AllTracesGCd)
{
    // Recovery candidates all GC'd → warmPath returns nullopt gracefully
    ScopedEnvVar env("NIX_GC_R", "val_A");

    {
        auto sb = makeStore();

        // Two cold stores with different dep values populate DepHashRecovery
        // and DepStructGroups with recovery entries
        sb.coldStore("", "root", string_t{"rA", {}},
                      {makeEnvVarDep("NIX_GC_R", "val_A")}, std::nullopt, true);

        setenv("NIX_GC_R", "val_B", 1);
        sb.coldStore("", "root", string_t{"rB", {}},
                      {makeEnvVarDep("NIX_GC_R", "val_B")}, std::nullopt, true);
    }

    // Revert to val_A (would normally trigger Phase 1 recovery)
    setenv("NIX_GC_R", "val_A", 1);

    // Fresh store: all traces GC'd. warmPath finds index entry but
    // isValidPath fails at line 348 → returns nullopt without crashing.
    auto freshStore = openStore("dummy://", {{"read-only", "false"}});
    EvalCacheStore sb2(*freshStore, state.symbols, testContextHash);

    auto result = sb2.warmPath("", {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(EvalCacheStoreTest, GC_RecacheAfterGC)
{
    // After GC wipes traces, cold-storing fresh values re-establishes the cache
    ScopedEnvVar env("NIX_GC_RC", "val");

    {
        auto sb = makeStore();
        sb.coldStore("", "root", string_t{"original", {}},
                      {makeEnvVarDep("NIX_GC_RC", "val")}, std::nullopt, true);
    }

    auto freshStore = openStore("dummy://", {{"read-only", "false"}});
    EvalCacheStore sb2(*freshStore, state.symbols, testContextHash);

    // Verify traces are GC'd
    EXPECT_FALSE(sb2.warmPath("", {}, state).has_value());

    // Re-cold-store in the new store (simulates re-evaluation after GC)
    sb2.coldStore("", "root", string_t{"re-cached", {}},
                   {makeEnvVarDep("NIX_GC_RC", "val")}, std::nullopt, true);

    // warmPath now succeeds
    auto result = sb2.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"re-cached", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheStoreTest, GC_RecacheWithParentChild)
{
    // After GC, re-cold-storing parent + child tree works end-to-end
    ScopedEnvVar env("NIX_GC_RPC", "val");

    {
        auto sb = makeStore();
        auto parent = sb.coldStore("", "root", string_t{"parent-v1", {}},
                                    {makeEnvVarDep("NIX_GC_RPC", "val")}, std::nullopt, true);
        sb.coldStore("child", "child", string_t{"child-v1", {}}, {}, parent, false);
    }

    auto freshStore = openStore("dummy://", {{"read-only", "false"}});
    EvalCacheStore sb2(*freshStore, state.symbols, testContextHash);

    // Both GC'd
    EXPECT_FALSE(sb2.warmPath("", {}, state).has_value());
    EXPECT_FALSE(sb2.warmPath("child", {}, state).has_value());

    // Re-cold-store parent + child
    auto newParent = sb2.coldStore("", "root", string_t{"parent-v2", {}},
                                    {makeEnvVarDep("NIX_GC_RPC", "val")}, std::nullopt, true);
    sb2.coldStore("child", "child", string_t{"child-v2", {}}, {}, newParent, false);

    // Both warm paths now succeed
    auto parentResult = sb2.warmPath("", {}, state);
    ASSERT_TRUE(parentResult.has_value());
    assertAttrValueEquals(string_t{"parent-v2", {}}, parentResult->value, state.symbols);

    auto childResult = sb2.warmPath("child", {}, state);
    ASSERT_TRUE(childResult.has_value());
    assertAttrValueEquals(string_t{"child-v2", {}}, childResult->value, state.symbols);
}

} // namespace nix::eval_cache
