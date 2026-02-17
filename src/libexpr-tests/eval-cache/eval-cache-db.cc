#include "helpers.hh"
#include "nix/expr/eval-cache-db.hh"
#include "nix/expr/eval-result-serialise.hh"

#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"

namespace nix::eval_cache {

using namespace nix::eval_cache::test;

class EvalCacheDbTest : public LibExprTest
{
public:
    EvalCacheDbTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {}

protected:
    // Writable cache dir for SQLite (sandbox has no writable $HOME)
    ScopedCacheDir cacheDir;

    static constexpr int64_t testContextHash = 0x1234567890ABCDEF;

    EvalCacheDb makeDb()
    {
        return EvalCacheDb(state.symbols, testContextHash);
    }
};

// ── buildAttrPath tests ──────────────────────────────────────────────

TEST_F(EvalCacheDbTest, BuildAttrPath_Empty)
{
    EXPECT_EQ(EvalCacheDb::buildAttrPath({}), "");
}

TEST_F(EvalCacheDbTest, BuildAttrPath_Single)
{
    EXPECT_EQ(EvalCacheDb::buildAttrPath({"packages"}), "packages");
}

TEST_F(EvalCacheDbTest, BuildAttrPath_Multiple)
{
    auto path = EvalCacheDb::buildAttrPath({"packages", "x86_64-linux", "hello"});
    // Should be null-byte separated
    std::string expected = "packages";
    expected.push_back('\0');
    expected.append("x86_64-linux");
    expected.push_back('\0');
    expected.append("hello");
    EXPECT_EQ(path, expected);
}

// ── depTypeString tests ──────────────────────────────────────────────

TEST_F(EvalCacheDbTest, DepTypeString_AllTypes)
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

// ── coldStore tests ─────────────────────────────────────────────────

TEST_F(EvalCacheDbTest, ColdStore_ReturnsDepSetId)
{
    auto db = makeDb();
    auto result = db.coldStore("", string_t{"hello", {}}, {}, std::nullopt, true);
    // depSetId should be a positive integer
    EXPECT_GT(result.depSetId, 0);
}

TEST_F(EvalCacheDbTest, ColdStore_AttrExists)
{
    auto db = makeDb();
    db.coldStore("", string_t{"hello", {}}, {}, std::nullopt, true);

    EXPECT_TRUE(db.attrExists(""));
    EXPECT_FALSE(db.attrExists("nonexistent"));
}

TEST_F(EvalCacheDbTest, ColdStore_WithDeps)
{
    auto db = makeDb();
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "a"),
        makeEnvVarDep("HOME", "/home"),
    };

    auto result = db.coldStore("", int_t{NixInt{42}}, deps, std::nullopt, true);

    auto loadedDeps = db.loadFullDepSet(result.depSetId);
    EXPECT_EQ(loadedDeps.size(), 2u);
}

TEST_F(EvalCacheDbTest, ColdStore_VolatileDep_NotSessionCached)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep()};

    auto result = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // CurrentTime dep -> should NOT be in validatedDepSetIds
    EXPECT_FALSE(db.validatedDepSetIds.count(result.depSetId));
}

TEST_F(EvalCacheDbTest, ColdStore_NonVolatile_SessionCached)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};

    auto result = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // Non-volatile dep -> SHOULD be in validatedDepSetIds
    EXPECT_TRUE(db.validatedDepSetIds.count(result.depSetId));
}

TEST_F(EvalCacheDbTest, ColdStore_ParentContextFiltered)
{
    auto db = makeDb();
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "a"),
        Dep{"", "", DepHashValue(std::string("parent-hash")), DepType::ParentContext},
    };

    auto result = db.coldStore("", null_t{}, deps, std::nullopt, true);

    auto loadedDeps = db.loadFullDepSet(result.depSetId);
    // ParentContext should be filtered out -- only 1 dep stored
    EXPECT_EQ(loadedDeps.size(), 1u);
    EXPECT_EQ(loadedDeps[0].type, DepType::Content);
}

TEST_F(EvalCacheDbTest, ColdStore_WithParent)
{
    auto db = makeDb();

    // Store parent
    auto parentResult = db.coldStore("", string_t{"parent-val", {}},
                                  {makeContentDep("/a.nix", "a")}, std::nullopt, true);

    // Store child referencing parent
    auto childResult = db.coldStore("child", string_t{"child-val", {}},
                                 {makeEnvVarDep("FOO", "bar")}, parentResult.depSetId, false);

    // With delta encoding, loadFullDepSet returns full set (parent + child deps)
    auto childDeps = db.loadFullDepSet(childResult.depSetId);
    EXPECT_EQ(childDeps.size(), 2u);

    // Verify both dep types are present
    bool hasContent = false, hasEnvVar = false;
    for (auto & dep : childDeps) {
        if (dep.type == DepType::Content) hasContent = true;
        if (dep.type == DepType::EnvVar) hasEnvVar = true;
    }
    EXPECT_TRUE(hasContent);
    EXPECT_TRUE(hasEnvVar);
}

TEST_F(EvalCacheDbTest, ColdStore_Deterministic)
{
    // Same inputs should produce consistent depSetId (upsert)
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};
    AttrValue value = string_t{"result", {}};

    auto r1 = db.coldStore("", value, deps, std::nullopt, true);
    auto r2 = db.coldStore("", value, deps, std::nullopt, true);

    // Same deps + same parent -> same dep set
    EXPECT_EQ(r1.depSetId, r2.depSetId);
}

TEST_F(EvalCacheDbTest, ColdStore_AllValueTypes)
{
    auto db = makeDb();

    auto testRoundtrip = [&](const AttrValue & value, std::string_view name) {
        auto path = std::string(name);
        db.coldStore(path, value, {}, std::nullopt, false);

        auto result = db.warmPath(path, {}, state);
        ASSERT_TRUE(result.has_value()) << "warmPath failed for " << name;
        assertAttrValueEquals(value, result->value, state.symbols);
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
    testRoundtrip(list_t{5}, "list");
    testRoundtrip(std::vector<std::string>{"a", "b", "c"}, "list-of-strings");
}

// ── Dep set dedup tests ──────────────────────────────────────────────

TEST_F(EvalCacheDbTest, DepSetDedup_IdenticalDeps)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/shared.nix", "shared")};

    // Two root attributes with identical deps should share the same dep set
    auto r1 = db.coldStore("a", string_t{"val1", {}}, deps, std::nullopt, false);
    auto r2 = db.coldStore("b", string_t{"val2", {}}, deps, std::nullopt, false);

    // Both should share the same dep set ID (content-addressed)
    EXPECT_EQ(r1.depSetId, r2.depSetId);

    auto deps1 = db.loadFullDepSet(r1.depSetId);
    EXPECT_EQ(deps1.size(), 1u);
}

TEST_F(EvalCacheDbTest, EmptyDepSet_HasDbRow)
{
    // Attributes with zero deps should still get a dep set row
    auto db = makeDb();

    auto result = db.coldStore("", string_t{"val", {}}, {}, std::nullopt, true);

    // loadFullDepSet should return empty vector (not fail)
    auto deps = db.loadFullDepSet(result.depSetId);
    EXPECT_TRUE(deps.empty());

    // Dep set should be valid (empty dep set passes validation)
    EXPECT_TRUE(db.validateDepSet(result.depSetId, {}, state));
}

// ── validateDepSet tests ──────────────────────────────────────────────

TEST_F(EvalCacheDbTest, ValidateDepSet_EnvVar_Valid)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR", "expected_value");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR", "expected_value")};
    auto result = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // Clear session cache so validateDepSet actually checks
    db.clearSessionCaches();

    bool valid = db.validateDepSet(result.depSetId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(EvalCacheDbTest, ValidateDepSet_EnvVar_Invalid)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR", "new_value");

    auto db = makeDb();
    // Create attr with OLD expected hash
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR", "old_value")};
    auto result = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // Clear session cache so validateDepSet actually checks
    db.clearSessionCaches();

    bool valid = db.validateDepSet(result.depSetId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheDbTest, ValidateDepSet_CurrentTime_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep()};
    auto result = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // CurrentTime is volatile, so not session-cached
    bool valid = db.validateDepSet(result.depSetId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheDbTest, ValidateDepSet_Exec_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeExecDep()};
    auto result = db.coldStore("", null_t{}, deps, std::nullopt, true);

    bool valid = db.validateDepSet(result.depSetId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheDbTest, ValidateDepSet_SessionCacheHit)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR2", "val");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR2", "val")};
    auto result = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // coldStore with non-volatile deps should have session-cached it
    EXPECT_TRUE(db.validatedDepSetIds.count(result.depSetId));
    // Second call should use session cache (instant return)
    EXPECT_TRUE(db.validateDepSet(result.depSetId, {}, state));
}

TEST_F(EvalCacheDbTest, ValidateDepSet_NoDeps_Valid)
{
    auto db = makeDb();
    auto result = db.coldStore("", string_t{"val", {}}, {}, std::nullopt, true);

    db.clearSessionCaches();

    bool valid = db.validateDepSet(result.depSetId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(EvalCacheDbTest, ValidateDepSet_ParentInvalid)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "current_value");

    auto db = makeDb();

    // Create parent with stale dep
    std::vector<Dep> staleDeps = {makeEnvVarDep("NIX_TEST_PARENT", "stale_value")};
    auto parentResult = db.coldStore("", string_t{"parent", {}},
                                  staleDeps, std::nullopt, true);

    // Create child with no direct deps, but parent is invalid
    auto childResult = db.coldStore("child", string_t{"child", {}},
                                 {}, parentResult.depSetId, false);

    // Clear session cache
    db.clearSessionCaches();

    bool valid = db.validateDepSet(childResult.depSetId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheDbTest, ValidateDepSet_ParentValid)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "correct_value");

    auto db = makeDb();

    // Create parent with correct dep
    std::vector<Dep> parentDeps = {makeEnvVarDep("NIX_TEST_PARENT", "correct_value")};
    auto parentResult = db.coldStore("", string_t{"parent", {}},
                                  parentDeps, std::nullopt, true);

    // Create child referencing valid parent
    auto childResult = db.coldStore("child", string_t{"child", {}},
                                 {}, parentResult.depSetId, false);

    // Clear session cache
    db.clearSessionCaches();

    bool valid = db.validateDepSet(childResult.depSetId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(EvalCacheDbTest, ValidateDepSet_MultipleDeps_OneInvalid)
{
    ScopedEnvVar env1("NIX_TEST_VALID", "current_value");
    ScopedEnvVar env2("NIX_TEST_STALE", "new_value");

    auto db = makeDb();
    std::vector<Dep> deps = {
        makeEnvVarDep("NIX_TEST_VALID", "current_value"),  // valid
        makeEnvVarDep("NIX_TEST_STALE", "old_value"),      // invalid (different hash)
    };
    auto result = db.coldStore("", null_t{}, deps, std::nullopt, true);

    db.clearSessionCaches();

    bool valid = db.validateDepSet(result.depSetId, {}, state);
    EXPECT_FALSE(valid);
}

// ── Cold -> Warm roundtrip ───────────────────────────────────────────

TEST_F(EvalCacheDbTest, ColdWarm_Roundtrip)
{
    ScopedEnvVar env("NIX_WARM_TEST", "stable");

    auto db = makeDb();
    AttrValue input = string_t{"cached value", {}};
    std::vector<Dep> deps = {makeEnvVarDep("NIX_WARM_TEST", "stable")};

    db.coldStore("", input, deps, std::nullopt, true);

    // Warm path should find it
    auto result = db.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(input, result->value, state.symbols);
}

TEST_F(EvalCacheDbTest, ColdWarm_Roundtrip_DepSetId)
{
    ScopedEnvVar env("NIX_WARM_TEST2", "stable");

    auto db = makeDb();
    AttrValue input = int_t{NixInt{99}};
    std::vector<Dep> deps = {makeEnvVarDep("NIX_WARM_TEST2", "stable")};

    auto coldResult = db.coldStore("", input, deps, std::nullopt, true);

    auto result = db.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    // Warm result's depSetId should match what coldStore returned
    EXPECT_EQ(result->depSetId, coldResult.depSetId);
}

TEST_F(EvalCacheDbTest, WarmPath_NoEntry)
{
    auto db = makeDb();
    auto result = db.warmPath("nonexistent", {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(EvalCacheDbTest, WarmPath_InvalidatedDeps)
{
    // Cold store with one env var value, then change it.
    // Warm path should fail (no recovery possible with no prior matching entry).
    ScopedEnvVar env("NIX_WARM_INVALID", "value1");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_WARM_INVALID", "value1")};
    db.coldStore("", string_t{"old", {}}, deps, std::nullopt, true);

    // Change env var
    setenv("NIX_WARM_INVALID", "value2", 1);

    // Clear session cache to force re-validation
    db.clearSessionCaches();

    auto result = db.warmPath("", {}, state);
    // Should fail because dep hash no longer matches and no recovery target exists
    EXPECT_FALSE(result.has_value());
}

// ── Recovery tests ───────────────────────────────────────────────────

TEST_F(EvalCacheDbTest, Phase1_StillWorks)
{
    // Phase 1: same structure, different values, revert -> succeeds
    ScopedEnvVar env("NIX_P1_TEST", "value_A");

    auto db = makeDb();
    std::vector<Dep> depsA = {makeEnvVarDep("NIX_P1_TEST", "value_A")};
    db.coldStore("", string_t{"result_A", {}}, depsA, std::nullopt, true);

    // Change env var to value_B and cold store
    setenv("NIX_P1_TEST", "value_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep("NIX_P1_TEST", "value_B")};
    db.coldStore("", string_t{"result_B", {}}, depsB, std::nullopt, true);

    // Revert to value_A -- Phase 1 should find the entry from first cold store
    setenv("NIX_P1_TEST", "value_A", 1);
    db.clearSessionCaches();

    auto result = db.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"result_A", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheDbTest, Phase1_ParentContextDisambiguation)
{
    // Dep-less child, parent changed. Phase 1 finds by parent full_hash mixing.
    ScopedEnvVar env("NIX_P1_ROOT", "val1");

    auto db = makeDb();

    // Cold store root with val1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P1_ROOT", "val1")};
    auto rootResult1 = db.coldStore("", string_t{"root1", {}}, rootDeps1, std::nullopt, true);

    // Cold store child (no deps) referencing root1
    db.coldStore("child", string_t{"child1", {}}, {}, rootResult1.depSetId, false);

    // Cold store root with val2
    setenv("NIX_P1_ROOT", "val2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P1_ROOT", "val2")};
    auto rootResult2 = db.coldStore("", string_t{"root2", {}}, rootDeps2, std::nullopt, true);

    // Cold store child referencing root2
    db.coldStore("child", string_t{"child2", {}}, {}, rootResult2.depSetId, false);

    // Revert to val1: root should be recovered, then child via parent hint
    setenv("NIX_P1_ROOT", "val1", 1);
    db.clearSessionCaches();

    // Root recovery (Phase 1)
    auto rootResult = db.warmPath("", {}, state);
    ASSERT_TRUE(rootResult.has_value());
    assertAttrValueEquals(string_t{"root1", {}}, rootResult->value, state.symbols);

    // Child recovery (Phase 1 -- uses parent depSetId hint for full_hash mixing)
    auto childResult = db.warmPath("child", {}, state, rootResult->depSetId);
    ASSERT_TRUE(childResult.has_value());
    assertAttrValueEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(EvalCacheDbTest, Phase1_ParentContextWithChildDeps)
{
    // Child with deps, parent changed. Phase 1 finds via full_hash with parent.
    ScopedEnvVar env1("NIX_P1W_ROOT", "rval1");
    ScopedEnvVar env2("NIX_P1W_CHILD", "cval");

    auto db = makeDb();

    // Cold store root with rval1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P1W_ROOT", "rval1")};
    auto rootResult1 = db.coldStore("", string_t{"root1", {}}, rootDeps1, std::nullopt, true);

    // Cold store child with stable dep + parent
    std::vector<Dep> childDeps = {makeEnvVarDep("NIX_P1W_CHILD", "cval")};
    db.coldStore("child", string_t{"child1", {}}, childDeps, rootResult1.depSetId, false);

    // Cold store root with rval2
    setenv("NIX_P1W_ROOT", "rval2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P1W_ROOT", "rval2")};
    auto rootResult2 = db.coldStore("", string_t{"root2", {}}, rootDeps2, std::nullopt, true);

    // Cold store child referencing root2
    db.coldStore("child", string_t{"child2", {}}, childDeps, rootResult2.depSetId, false);

    // Revert to rval1
    setenv("NIX_P1W_ROOT", "rval1", 1);
    db.clearSessionCaches();

    auto rootResult = db.warmPath("", {}, state);
    ASSERT_TRUE(rootResult.has_value());

    auto childResult = db.warmPath("child", {}, state, rootResult->depSetId);
    ASSERT_TRUE(childResult.has_value());
    assertAttrValueEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(EvalCacheDbTest, Phase1_CascadeThroughTree)
{
    // Root -> child1 -> child2 (grandchild), with deps at each level.
    // Phase 1 uses full_hash = hash(deps + "P" + parent_full_hash),
    // so each level's full_hash changes when parent deps change.
    ScopedEnvVar env1("NIX_P1C_ROOT", "v1");
    ScopedEnvVar env2("NIX_P1C_CHILD", "cv1");

    auto db = makeDb();

    // Build null-byte-separated attr path for grandchild
    std::string c2AttrPath = "c1";
    c2AttrPath.push_back('\0');
    c2AttrPath.append("c2");

    // Version 1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P1C_ROOT", "v1")};
    std::vector<Dep> childDeps1 = {makeEnvVarDep("NIX_P1C_CHILD", "cv1")};
    auto root1 = db.coldStore("", string_t{"r1", {}}, rootDeps1, std::nullopt, true);
    auto child1_1 = db.coldStore("c1", string_t{"c1v1", {}}, childDeps1, root1.depSetId, false);
    db.coldStore(c2AttrPath, string_t{"c2v1", {}}, {}, child1_1.depSetId, false);

    // Version 2 — both root and child deps change
    setenv("NIX_P1C_ROOT", "v2", 1);
    setenv("NIX_P1C_CHILD", "cv2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P1C_ROOT", "v2")};
    std::vector<Dep> childDeps2 = {makeEnvVarDep("NIX_P1C_CHILD", "cv2")};
    auto root2 = db.coldStore("", string_t{"r2", {}}, rootDeps2, std::nullopt, true);
    auto child1_2 = db.coldStore("c1", string_t{"c1v2", {}}, childDeps2, root2.depSetId, false);
    db.coldStore(c2AttrPath, string_t{"c2v2", {}}, {}, child1_2.depSetId, false);

    // Revert to v1
    setenv("NIX_P1C_ROOT", "v1", 1);
    setenv("NIX_P1C_CHILD", "cv1", 1);
    db.clearSessionCaches();

    auto rootR = db.warmPath("", {}, state);
    ASSERT_TRUE(rootR.has_value());
    assertAttrValueEquals(string_t{"r1", {}}, rootR->value, state.symbols);

    auto c1R = db.warmPath("c1", {}, state, rootR->depSetId);
    ASSERT_TRUE(c1R.has_value());
    assertAttrValueEquals(string_t{"c1v1", {}}, c1R->value, state.symbols);

    auto c2R = db.warmPath(c2AttrPath, {}, state, c1R->depSetId);
    ASSERT_TRUE(c2R.has_value());
    assertAttrValueEquals(string_t{"c2v1", {}}, c2R->value, state.symbols);
}

TEST_F(EvalCacheDbTest, Phase3_DepStructMismatch)
{
    // Phase 3: different dep structures between two cold evals.
    // Phase 1 fails because keys differ. Phase 3 finds matching group.
    ScopedEnvVar env1("NIX_P3_A", "aval");
    ScopedEnvVar env2("NIX_P3_B", "bval");

    auto db = makeDb();

    // First cold store: only dep A
    std::vector<Dep> deps1 = {makeEnvVarDep("NIX_P3_A", "aval")};
    db.coldStore("", string_t{"result1", {}}, deps1, std::nullopt, true);

    // Second cold store: deps A + B (different structure)
    std::vector<Dep> deps2 = {
        makeEnvVarDep("NIX_P3_A", "aval"),
        makeEnvVarDep("NIX_P3_B", "bval"),
    };
    db.coldStore("", string_t{"result2", {}}, deps2, std::nullopt, true);

    // Now attribute points to entry with deps A+B.
    // Change B to invalidate that entry's deps.
    setenv("NIX_P3_B", "bval_new", 1);
    db.clearSessionCaches();

    // Phase 1 will compute new hash for A+B (bval_new) -- no match.
    // Phase 3 will scan struct groups, find the group with only dep A,
    // compute current hash for A -> match the first entry.
    auto result = db.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"result1", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheDbTest, Phase3_MultipleStructGroups)
{
    // 3 cold stores with different dep structures. Phase 3 iterates groups.
    ScopedEnvVar env1("NIX_P3M_A", "a");
    ScopedEnvVar env2("NIX_P3M_B", "b");
    ScopedEnvVar env3("NIX_P3M_C", "c");

    auto db = makeDb();

    // Structure 1: only A
    db.coldStore("", string_t{"r1", {}},
                  {makeEnvVarDep("NIX_P3M_A", "a")}, std::nullopt, true);

    // Structure 2: A + B
    db.coldStore("", string_t{"r2", {}},
                  {makeEnvVarDep("NIX_P3M_A", "a"), makeEnvVarDep("NIX_P3M_B", "b")},
                  std::nullopt, true);

    // Structure 3: A + B + C (latest, in attribute entry)
    db.coldStore("", string_t{"r3", {}},
                  {makeEnvVarDep("NIX_P3M_A", "a"), makeEnvVarDep("NIX_P3M_B", "b"),
                   makeEnvVarDep("NIX_P3M_C", "c")},
                  std::nullopt, true);

    // Change C -> invalidates struct 3
    setenv("NIX_P3M_C", "c_new", 1);
    db.clearSessionCaches();

    auto result = db.warmPath("", {}, state);
    // Should recover r1 or r2 (whichever group is scanned first with valid deps)
    ASSERT_TRUE(result.has_value());
    // Both r1 and r2 have valid deps (A and B unchanged), either is acceptable
    auto & val = std::get<string_t>(result->value);
    EXPECT_TRUE(val.first == "r1" || val.first == "r2");
}

TEST_F(EvalCacheDbTest, Phase3_EmptyDeps)
{
    // Struct group with zero deps
    auto db = makeDb();

    db.coldStore("", string_t{"empty1", {}}, {}, std::nullopt, true);
    db.coldStore("", string_t{"empty2", {}},
                  {makeEnvVarDep("NIX_P3E_X", "x")}, std::nullopt, true);

    // Invalidate X
    ScopedEnvVar env("NIX_P3E_X", "x_new");
    db.clearSessionCaches();

    auto result = db.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"empty1", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheDbTest, Phase1_FallbackToPhase3)
{
    // No parent hint -- Phase 1 alone, then Phase 3 succeeds
    ScopedEnvVar env1("NIX_P1F_A", "a");
    ScopedEnvVar env2("NIX_P1F_B", "b");

    auto db = makeDb();

    // Two structures
    db.coldStore("", string_t{"r1", {}},
                  {makeEnvVarDep("NIX_P1F_A", "a")}, std::nullopt, true);
    db.coldStore("", string_t{"r2", {}},
                  {makeEnvVarDep("NIX_P1F_A", "a"), makeEnvVarDep("NIX_P1F_B", "b")},
                  std::nullopt, true);

    // Invalidate B
    setenv("NIX_P1F_B", "b_new", 1);
    db.clearSessionCaches();

    // Phase 1 fails (A+B with new B doesn't match). Phase 3 finds struct with only A.
    auto result = db.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"r1", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheDbTest, AllPhaseFail_Volatile)
{
    // CurrentTime dep -> immediate abort, nullopt
    auto db = makeDb();

    std::vector<Dep> deps = {makeCurrentTimeDep()};
    db.coldStore("", null_t{}, deps, std::nullopt, true);

    auto result = db.warmPath("", {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(EvalCacheDbTest, RecoveryUpdatesAttribute)
{
    // After Phase 1 recovery, next warmPath succeeds directly (attribute updated)
    ScopedEnvVar env("NIX_RUI_TEST", "val_A");

    auto db = makeDb();

    // Two cold stores
    std::vector<Dep> depsA = {makeEnvVarDep("NIX_RUI_TEST", "val_A")};
    db.coldStore("", string_t{"rA", {}}, depsA, std::nullopt, true);

    setenv("NIX_RUI_TEST", "val_B", 1);
    std::vector<Dep> depsB = {makeEnvVarDep("NIX_RUI_TEST", "val_B")};
    db.coldStore("", string_t{"rB", {}}, depsB, std::nullopt, true);

    // Revert to A, recovery should update attribute
    setenv("NIX_RUI_TEST", "val_A", 1);
    db.clearSessionCaches();

    auto r1 = db.warmPath("", {}, state);
    ASSERT_TRUE(r1.has_value());

    // Second warm path should succeed via direct lookup (no recovery needed)
    db.clearSessionCaches();
    auto r2 = db.warmPath("", {}, state);
    ASSERT_TRUE(r2.has_value());
    assertAttrValueEquals(string_t{"rA", {}}, r2->value, state.symbols);
}

TEST_F(EvalCacheDbTest, Phase1_Then_Phase3_Cascade)
{
    // Phase 1 fails, Phase 3 succeeds
    ScopedEnvVar env1("NIX_CASCADE_A", "a");
    ScopedEnvVar env2("NIX_CASCADE_B", "b");

    auto db = makeDb();

    // Structure 1: only dep A (this is what we want to recover)
    db.coldStore("child", string_t{"target", {}},
                  {makeEnvVarDep("NIX_CASCADE_A", "a")}, std::nullopt, false);

    // Structure 2: deps A + B (latest, in attribute)
    db.coldStore("child", string_t{"latest", {}},
                  {makeEnvVarDep("NIX_CASCADE_A", "a"), makeEnvVarDep("NIX_CASCADE_B", "b")},
                  std::nullopt, false);

    // Invalidate B -> Phase 1 fails (structure A+B, B can't match new value)
    // Phase 3 finds structure 1 (only A) -> succeeds
    setenv("NIX_CASCADE_B", "b_new", 1);
    db.clearSessionCaches();

    auto result = db.warmPath("child", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"target", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheDbTest, Phase1_DeepChainRecovery)
{
    // Deep chain (depth 6): root -> c1 -> c2 -> c3 -> c4 -> c5
    // Store two versions, revert, verify Phase 1 recovery at deepest level.
    ScopedEnvVar env1("NIX_DEEP_ROOT", "v1");
    ScopedEnvVar env2("NIX_DEEP_C1", "v1");

    auto db = makeDb();

    // Build attr paths
    auto makePath = [](std::vector<std::string> parts) {
        std::string path;
        for (size_t i = 0; i < parts.size(); i++) {
            if (i > 0) path.push_back('\0');
            path.append(parts[i]);
        }
        return path;
    };
    auto c1Path = makePath({"c1"});
    auto c2Path = makePath({"c1", "c2"});
    auto c3Path = makePath({"c1", "c2", "c3"});
    auto c4Path = makePath({"c1", "c2", "c3", "c4"});
    auto c5Path = makePath({"c1", "c2", "c3", "c4", "c5"});

    // Version 1: chain with deps at root and c1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_DEEP_ROOT", "v1")};
    std::vector<Dep> c1Deps1 = {makeEnvVarDep("NIX_DEEP_C1", "v1")};
    auto root1 = db.coldStore("", string_t{"root_v1", {}}, rootDeps1, std::nullopt, true);
    auto c1_1 = db.coldStore(c1Path, string_t{"c1_v1", {}}, c1Deps1, root1.depSetId, false);
    auto c2_1 = db.coldStore(c2Path, string_t{"c2_v1", {}}, {}, c1_1.depSetId, false);
    auto c3_1 = db.coldStore(c3Path, string_t{"c3_v1", {}}, {}, c2_1.depSetId, false);
    auto c4_1 = db.coldStore(c4Path, string_t{"c4_v1", {}}, {}, c3_1.depSetId, false);
    db.coldStore(c5Path, string_t{"c5_v1", {}}, {}, c4_1.depSetId, false);

    // Version 2: different values
    setenv("NIX_DEEP_ROOT", "v2", 1);
    setenv("NIX_DEEP_C1", "v2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_DEEP_ROOT", "v2")};
    std::vector<Dep> c1Deps2 = {makeEnvVarDep("NIX_DEEP_C1", "v2")};
    auto root2 = db.coldStore("", string_t{"root_v2", {}}, rootDeps2, std::nullopt, true);
    auto c1_2 = db.coldStore(c1Path, string_t{"c1_v2", {}}, c1Deps2, root2.depSetId, false);
    auto c2_2 = db.coldStore(c2Path, string_t{"c2_v2", {}}, {}, c1_2.depSetId, false);
    auto c3_2 = db.coldStore(c3Path, string_t{"c3_v2", {}}, {}, c2_2.depSetId, false);
    auto c4_2 = db.coldStore(c4Path, string_t{"c4_v2", {}}, {}, c3_2.depSetId, false);
    db.coldStore(c5Path, string_t{"c5_v2", {}}, {}, c4_2.depSetId, false);

    // Revert to v1
    setenv("NIX_DEEP_ROOT", "v1", 1);
    setenv("NIX_DEEP_C1", "v1", 1);
    db.clearSessionCaches();

    // Recover root (Phase 1)
    auto rootR = db.warmPath("", {}, state);
    ASSERT_TRUE(rootR.has_value());
    assertAttrValueEquals(string_t{"root_v1", {}}, rootR->value, state.symbols);

    // Recover chain down to c5 (Phase 1 at each level, using parent depSetId hint)
    auto c1R = db.warmPath(c1Path, {}, state, rootR->depSetId);
    ASSERT_TRUE(c1R.has_value());
    assertAttrValueEquals(string_t{"c1_v1", {}}, c1R->value, state.symbols);

    auto c2R = db.warmPath(c2Path, {}, state, c1R->depSetId);
    ASSERT_TRUE(c2R.has_value());
    assertAttrValueEquals(string_t{"c2_v1", {}}, c2R->value, state.symbols);

    auto c3R = db.warmPath(c3Path, {}, state, c2R->depSetId);
    ASSERT_TRUE(c3R.has_value());
    assertAttrValueEquals(string_t{"c3_v1", {}}, c3R->value, state.symbols);

    auto c4R = db.warmPath(c4Path, {}, state, c3R->depSetId);
    ASSERT_TRUE(c4R.has_value());
    assertAttrValueEquals(string_t{"c4_v1", {}}, c4R->value, state.symbols);

    auto c5R = db.warmPath(c5Path, {}, state, c4R->depSetId);
    ASSERT_TRUE(c5R.has_value());
    assertAttrValueEquals(string_t{"c5_v1", {}}, c5R->value, state.symbols);
}

TEST_F(EvalCacheDbTest, RecoveryStress_10Versions)
{
    // Cold-store 10 versions of same attribute (each with different env var value).
    // Revert to version 1. Verify recovery succeeds.
    ScopedEnvVar env("NIX_STRESS_VAR", "version_0");

    auto db = makeDb();

    // Store 10 versions
    for (int i = 0; i < 10; i++) {
        auto val = "version_" + std::to_string(i);
        setenv("NIX_STRESS_VAR", val.c_str(), 1);
        std::vector<Dep> deps = {makeEnvVarDep("NIX_STRESS_VAR", val)};
        auto result = "result_" + std::to_string(i);
        db.coldStore("", string_t{result, {}}, deps, std::nullopt, true);
    }

    // Revert to each version and verify recovery
    for (int target = 0; target < 10; target++) {
        auto val = "version_" + std::to_string(target);
        setenv("NIX_STRESS_VAR", val.c_str(), 1);
        db.clearSessionCaches();

        auto result = db.warmPath("", {}, state);
        ASSERT_TRUE(result.has_value()) << "Recovery failed for version " << target;
        auto expected = "result_" + std::to_string(target);
        assertAttrValueEquals(string_t{expected, {}}, result->value, state.symbols);
    }
}

TEST_F(EvalCacheDbTest, RecoveryFailure_AllPhasesFail)
{
    // All phases fail: attribute has dep on env var, env var changed to a value
    // that has never been stored. No recovery candidate matches.
    ScopedEnvVar env("NIX_FAIL_VAR", "original");

    auto db = makeDb();

    std::vector<Dep> deps = {makeEnvVarDep("NIX_FAIL_VAR", "original")};
    db.coldStore("", string_t{"old_result", {}}, deps, std::nullopt, true);

    // Change to a NEVER-STORED value
    setenv("NIX_FAIL_VAR", "completely_new_value", 1);
    db.clearSessionCaches();

    auto result = db.warmPath("", {}, state);
    EXPECT_FALSE(result.has_value());
}

// ── Context hash isolation tests ─────────────────────────────────────

TEST_F(EvalCacheDbTest, DifferentContextHash_Isolated)
{
    // Two different context hashes should have isolated namespaces

    {
        EvalCacheDb db1(state.symbols, 111);
        db1.coldStore("pkg", string_t{"v1", {}}, {}, std::nullopt, false);
    }
    {
        EvalCacheDb db2(state.symbols, 222);
        db2.coldStore("pkg", string_t{"v2", {}}, {}, std::nullopt, false);
    }

    {
        EvalCacheDb db1(state.symbols, 111);
        auto r1 = db1.warmPath("pkg", {}, state);
        ASSERT_TRUE(r1.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r1->value));
        EXPECT_EQ(std::get<string_t>(r1->value).first, "v1");
    }
    {
        EvalCacheDb db2(state.symbols, 222);
        auto r2 = db2.warmPath("pkg", {}, state);
        ASSERT_TRUE(r2.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r2->value));
        EXPECT_EQ(std::get<string_t>(r2->value).first, "v2");
    }
}

TEST_F(EvalCacheDbTest, NullByteAttrPath)
{
    auto db = makeDb();

    // Null-byte separated path like "packages\0x86_64-linux\0hello"
    std::string attrPath = "packages";
    attrPath.push_back('\0');
    attrPath.append("x86_64-linux");
    attrPath.push_back('\0');
    attrPath.append("hello");

    db.coldStore(attrPath, string_t{"val", {}}, {}, std::nullopt, false);
    EXPECT_TRUE(db.attrExists(attrPath));
    EXPECT_FALSE(db.attrExists("packages"));
}

TEST_F(EvalCacheDbTest, EmptyAttrPath)
{
    auto db = makeDb();
    db.coldStore("", string_t{"root-val", {}}, {}, std::nullopt, true);
    EXPECT_TRUE(db.attrExists(""));
}

TEST_F(EvalCacheDbTest, MultipleEntries_Stress)
{
    auto db = makeDb();

    // Insert 100 entries
    for (int i = 0; i < 100; i++) {
        auto name = "stress-" + std::to_string(i);
        db.coldStore(name, int_t{NixInt{i}}, {}, std::nullopt, false);
    }

    EXPECT_TRUE(db.attrExists("stress-0"));
    EXPECT_TRUE(db.attrExists("stress-99"));
    EXPECT_FALSE(db.attrExists("stress-100"));
}

// ── BLOB serialization roundtrip tests ────────────────────────────────

TEST_F(EvalCacheDbTest, BlobRoundTrip_Empty)
{
    std::vector<EvalCacheDb::InternedDep> deps;
    auto blob = EvalCacheDb::serializeDeps(deps);
    EXPECT_TRUE(blob.empty());
    auto result = EvalCacheDb::deserializeInternedDeps(blob.data(), blob.size());
    EXPECT_TRUE(result.empty());
}

TEST_F(EvalCacheDbTest, BlobRoundTrip_Blake3Deps)
{
    std::vector<EvalCacheDb::InternedDep> deps;
    for (int i = 0; i < 5; i++) {
        auto hash = depHash("content-" + std::to_string(i));
        deps.push_back({DepType::Content, static_cast<uint32_t>(i + 1),
                        static_cast<uint32_t>(i + 100), DepHashValue(hash)});
    }

    auto blob = EvalCacheDb::serializeDeps(deps);
    EXPECT_FALSE(blob.empty());

    auto result = EvalCacheDb::deserializeInternedDeps(blob.data(), blob.size());
    ASSERT_EQ(result.size(), 5u);
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(result[i].type, DepType::Content);
        EXPECT_EQ(result[i].sourceId, static_cast<uint32_t>(i + 1));
        EXPECT_EQ(result[i].keyId, static_cast<uint32_t>(i + 100));
        EXPECT_EQ(result[i].hash, deps[i].hash);
    }
}

TEST_F(EvalCacheDbTest, BlobRoundTrip_MixedDeps)
{
    std::vector<EvalCacheDb::InternedDep> deps;

    // BLAKE3 hash dep (Content)
    deps.push_back({DepType::Content, 1, 2, DepHashValue(depHash("file-data"))});

    // String hash dep (CopiedPath — store path)
    deps.push_back({DepType::CopiedPath, 3, 4,
                    DepHashValue(std::string("/nix/store/aaaa-test"))});

    // String hash dep (EnvVar)
    deps.push_back({DepType::EnvVar, 5, 6, DepHashValue(depHash("env-val"))});

    // Empty string hash dep
    deps.push_back({DepType::Existence, 7, 8, DepHashValue(std::string("missing"))});

    auto blob = EvalCacheDb::serializeDeps(deps);
    auto result = EvalCacheDb::deserializeInternedDeps(blob.data(), blob.size());
    ASSERT_EQ(result.size(), 4u);

    // Content: BLAKE3
    EXPECT_EQ(result[0].type, DepType::Content);
    EXPECT_EQ(result[0].sourceId, 1u);
    EXPECT_EQ(result[0].keyId, 2u);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(result[0].hash));

    // CopiedPath: string (not BLAKE3, so deserialized as string)
    EXPECT_EQ(result[1].type, DepType::CopiedPath);
    EXPECT_EQ(result[1].sourceId, 3u);
    EXPECT_EQ(result[1].keyId, 4u);
    EXPECT_TRUE(std::holds_alternative<std::string>(result[1].hash));
    EXPECT_EQ(std::get<std::string>(result[1].hash), "/nix/store/aaaa-test");

    // EnvVar: BLAKE3
    EXPECT_EQ(result[2].type, DepType::EnvVar);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(result[2].hash));

    // Existence: string
    EXPECT_EQ(result[3].type, DepType::Existence);
    EXPECT_TRUE(std::holds_alternative<std::string>(result[3].hash));
    EXPECT_EQ(std::get<std::string>(result[3].hash), "missing");
}

TEST_F(EvalCacheDbTest, BlobRoundTrip_LargeSet)
{
    std::vector<EvalCacheDb::InternedDep> deps;
    for (uint32_t i = 0; i < 10000; i++) {
        auto hash = depHash("content-" + std::to_string(i));
        deps.push_back({DepType::Content, i, i + 50000, DepHashValue(hash)});
    }

    auto blob = EvalCacheDb::serializeDeps(deps);
    // Each dep: 1 + 4 + 4 + 1 + 32 = 42 bytes
    EXPECT_EQ(blob.size(), 10000u * 42u);

    auto result = EvalCacheDb::deserializeInternedDeps(blob.data(), blob.size());
    ASSERT_EQ(result.size(), 10000u);

    // Spot-check first, middle, and last
    EXPECT_EQ(result[0].sourceId, 0u);
    EXPECT_EQ(result[0].keyId, 50000u);
    EXPECT_EQ(result[5000].sourceId, 5000u);
    EXPECT_EQ(result[5000].keyId, 55000u);
    EXPECT_EQ(result[9999].sourceId, 9999u);
    EXPECT_EQ(result[9999].keyId, 59999u);

    // Verify hashes match
    for (uint32_t i = 0; i < 10000; i++) {
        EXPECT_EQ(result[i].hash, deps[i].hash) << "Hash mismatch at index " << i;
    }
}

// ── Delta encoding tests ─────────────────────────────────────────────

TEST_F(EvalCacheDbTest, DeltaEncoding_SiblingOverlap)
{
    auto db = makeDb();

    // Parent with 0 deps (FullAttrs pattern)
    auto parentResult = db.coldStore("", null_t{}, {}, std::nullopt, true);

    // Child A with 100 deps
    std::vector<Dep> depsA;
    for (int i = 0; i < 100; i++) {
        depsA.push_back(makeContentDep("/file-" + std::to_string(i) + ".nix",
                                       "content-" + std::to_string(i)));
    }
    auto childA = db.coldStore("a", string_t{"val-a", {}}, depsA, parentResult.depSetId, false);

    // Child B with 95 overlapping deps + 5 different hashes
    std::vector<Dep> depsB;
    for (int i = 0; i < 95; i++) {
        depsB.push_back(makeContentDep("/file-" + std::to_string(i) + ".nix",
                                       "content-" + std::to_string(i)));
    }
    for (int i = 95; i < 100; i++) {
        depsB.push_back(makeContentDep("/file-" + std::to_string(i) + ".nix",
                                       "content-modified-" + std::to_string(i)));
    }
    auto childB = db.coldStore("b", string_t{"val-b", {}}, depsB, parentResult.depSetId, false);

    // Both should load correctly with full deps
    auto loadedA = db.loadFullDepSet(childA.depSetId);
    auto loadedB = db.loadFullDepSet(childB.depSetId);
    EXPECT_EQ(loadedA.size(), 100u);
    EXPECT_EQ(loadedB.size(), 100u);

    // After optimization, child B should use child A as base (same struct hash)
    // since they have the same 100 dep keys
    db.optimizeDepSets();

    // Verify both still load correctly after optimization
    auto reloadedA = db.loadFullDepSet(childA.depSetId);
    auto reloadedB = db.loadFullDepSet(childB.depSetId);
    EXPECT_EQ(reloadedA.size(), 100u);
    EXPECT_EQ(reloadedB.size(), 100u);
}

TEST_F(EvalCacheDbTest, DeltaEncoding_StructHashMatch)
{
    auto db = makeDb();

    // Attr v1 with deps [A=h1, B=h2, C=h3]
    std::vector<Dep> depsV1 = {
        makeContentDep("/a.nix", "h1"),
        makeContentDep("/b.nix", "h2"),
        makeContentDep("/c.nix", "h3"),
    };
    auto v1 = db.coldStore("attr", string_t{"v1", {}}, depsV1, std::nullopt, false);

    // Attr v2 with deps [A=h1', B=h2, C=h3] — same structure, different hash for A
    std::vector<Dep> depsV2 = {
        makeContentDep("/a.nix", "h1-modified"),
        makeContentDep("/b.nix", "h2"),
        makeContentDep("/c.nix", "h3"),
    };
    auto v2 = db.coldStore("attr", string_t{"v2", {}}, depsV2, std::nullopt, false);

    // v2 should use v1 as struct-hash base since they have same dep keys
    // After optimization, v2's delta should be much smaller than v1's full set
    db.optimizeDepSets();

    // Both should still load correctly
    auto loadedV1 = db.loadFullDepSet(v1.depSetId);
    auto loadedV2 = db.loadFullDepSet(v2.depSetId);

    ASSERT_EQ(loadedV1.size(), 3u);
    ASSERT_EQ(loadedV2.size(), 3u);

    // Verify v2 has the modified hash for A
    bool foundModifiedA = false;
    for (auto & dep : loadedV2) {
        if (dep.key == "/a.nix") {
            auto h = depHash("h1-modified");
            EXPECT_EQ(dep.expectedHash, DepHashValue(h));
            foundModifiedA = true;
        }
    }
    EXPECT_TRUE(foundModifiedA);
}

TEST_F(EvalCacheDbTest, DeltaEncoding_NoParentBase)
{
    auto db = makeDb();

    // Parent with 0 deps
    auto parent = db.coldStore("", null_t{}, {}, std::nullopt, true);

    // Child A with 100 deps
    std::vector<Dep> depsA;
    for (int i = 0; i < 100; i++) {
        depsA.push_back(makeContentDep("/f" + std::to_string(i) + ".nix",
                                       "c" + std::to_string(i)));
    }
    auto childA = db.coldStore("a", int_t{NixInt{1}}, depsA, parent.depSetId, false);

    // Child B with 99 overlapping + 1 different
    std::vector<Dep> depsB;
    for (int i = 0; i < 99; i++) {
        depsB.push_back(makeContentDep("/f" + std::to_string(i) + ".nix",
                                       "c" + std::to_string(i)));
    }
    depsB.push_back(makeContentDep("/f99.nix", "c99-modified"));
    auto childB = db.coldStore("b", int_t{NixInt{2}}, depsB, parent.depSetId, false);

    // After optimization, child B should NOT use parent (0 deps) as base
    // It should use child A (which has the same struct hash — same 100 dep keys)
    db.optimizeDepSets();

    // Verify both still load correctly
    auto loadedA = db.loadFullDepSet(childA.depSetId);
    auto loadedB = db.loadFullDepSet(childB.depSetId);
    EXPECT_EQ(loadedA.size(), 100u);
    EXPECT_EQ(loadedB.size(), 100u);

    // Verify B has the modified dep
    bool foundModified = false;
    for (auto & dep : loadedB) {
        if (dep.key == "/f99.nix") {
            auto h = depHash("c99-modified");
            EXPECT_EQ(dep.expectedHash, DepHashValue(h));
            foundModified = true;
        }
    }
    EXPECT_TRUE(foundModified);
}

TEST_F(EvalCacheDbTest, DeltaEncoding_PostWriteOptimization)
{
    auto db = makeDb();

    // Store 5 dep sets with same struct_hash in suboptimal order (smallest first)
    std::vector<int64_t> depSetIds;
    for (int setNum = 0; setNum < 5; setNum++) {
        std::vector<Dep> deps;
        for (int i = 0; i < 50; i++) {
            auto key = "/shared-" + std::to_string(i) + ".nix";
            auto content = "content-" + std::to_string(i) + "-v" + std::to_string(setNum);
            deps.push_back(makeContentDep(key, content));
        }
        auto result = db.coldStore(
            "attr-" + std::to_string(setNum), int_t{NixInt{setNum}},
            deps, std::nullopt, false);
        depSetIds.push_back(result.depSetId);
    }

    // Run optimization
    db.optimizeDepSets();

    // All 5 should still load correctly with 50 deps each
    for (int i = 0; i < 5; i++) {
        auto loaded = db.loadFullDepSet(depSetIds[i]);
        EXPECT_EQ(loaded.size(), 50u) << "Dep set " << i << " has wrong number of deps";
    }
}

TEST_F(EvalCacheDbTest, DeltaEncoding_CrossCommitDelta)
{
    // Simulate 3 "commits" for the same attribute, each with slightly different deps
    auto db = makeDb();

    // Base deps: 50 shared files
    auto makeDeps = [](int version) {
        std::vector<Dep> deps;
        for (int i = 0; i < 50; i++) {
            auto key = "/src/" + std::to_string(i) + ".nix";
            // Most deps are the same across versions, a few change
            auto content = (i < 45)
                ? "stable-content-" + std::to_string(i)
                : "content-v" + std::to_string(version) + "-" + std::to_string(i);
            deps.push_back(makeContentDep(key, content));
        }
        return deps;
    };

    auto r1 = db.coldStore("", string_t{"commit-1", {}}, makeDeps(1), std::nullopt, true);
    auto r2 = db.coldStore("", string_t{"commit-2", {}}, makeDeps(2), std::nullopt, true);
    auto r3 = db.coldStore("", string_t{"commit-3", {}}, makeDeps(3), std::nullopt, true);

    // After optimization, all 3 should share a base
    db.optimizeDepSets();

    // All should load correctly with 50 deps
    EXPECT_EQ(db.loadFullDepSet(r1.depSetId).size(), 50u);
    EXPECT_EQ(db.loadFullDepSet(r2.depSetId).size(), 50u);
    EXPECT_EQ(db.loadFullDepSet(r3.depSetId).size(), 50u);
}

TEST_F(EvalCacheDbTest, DeltaEncoding_RowCount)
{
    // Store 10 attrs each with 100 shared deps + 10 unique deps.
    // Total should be much less than 10 × 110.
    auto db = makeDb();

    std::vector<int64_t> depSetIds;
    for (int attrNum = 0; attrNum < 10; attrNum++) {
        std::vector<Dep> deps;
        // 100 shared deps (same keys, same hashes)
        for (int i = 0; i < 100; i++) {
            deps.push_back(makeContentDep(
                "/shared-" + std::to_string(i) + ".nix", "shared-content"));
        }
        // 10 unique deps per attr
        for (int i = 0; i < 10; i++) {
            deps.push_back(makeContentDep(
                "/unique-" + std::to_string(attrNum) + "-" + std::to_string(i) + ".nix",
                "unique-content"));
        }
        auto result = db.coldStore(
            "attr-" + std::to_string(attrNum), int_t{NixInt{attrNum}},
            deps, std::nullopt, false);
        depSetIds.push_back(result.depSetId);
    }

    // All 10 should load correctly with 110 deps each
    for (int i = 0; i < 10; i++) {
        auto loaded = db.loadFullDepSet(depSetIds[i]);
        EXPECT_EQ(loaded.size(), 110u) << "Dep set " << i << " has wrong number of deps";
    }
}

// ── Batch validation + hash caching tests ────────────────────────────

TEST_F(EvalCacheDbTest, WarmPath_BatchValidation)
{
    // Cold store attr with 50 deps where dep #25 is invalid.
    // Batch validation should compute ALL 50 hashes (not stop at #25).
    ScopedEnvVar env0("NIX_BATCH_0", "v0");

    auto db = makeDb();
    std::vector<Dep> deps;
    for (int i = 0; i < 50; i++) {
        auto key = "NIX_BATCH_" + std::to_string(i);
        auto value = "v" + std::to_string(i);
        setenv(key.c_str(), value.c_str(), 1);
        deps.push_back(makeEnvVarDep(key, value));
    }
    auto result = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // Change dep #25 to invalidate
    setenv("NIX_BATCH_25", "CHANGED", 1);
    db.clearSessionCaches();

    // Validate — should fail but cache ALL 50 hashes
    bool valid = db.validateDepSet(result.depSetId, {}, state);
    EXPECT_FALSE(valid);

    // All 50 dep keys should be in currentHashCache
    EXPECT_GE(db.currentHashCache.size(), 50u);

    // Clean up env vars
    for (int i = 0; i < 50; i++) {
        auto key = "NIX_BATCH_" + std::to_string(i);
        unsetenv(key.c_str());
    }
}

TEST_F(EvalCacheDbTest, WarmPath_HashCaching)
{
    // Cold store with deps, change 1, trigger validation failure → recovery.
    // Assert that recovery reuses cached hashes from validation.
    ScopedEnvVar env1("NIX_HASHCACHE_A", "valA");
    ScopedEnvVar env2("NIX_HASHCACHE_B", "valB");

    auto db = makeDb();

    // Version 1: deps A and B
    std::vector<Dep> deps1 = {
        makeEnvVarDep("NIX_HASHCACHE_A", "valA"),
        makeEnvVarDep("NIX_HASHCACHE_B", "valB"),
    };
    db.coldStore("", string_t{"result-1", {}}, deps1, std::nullopt, true);

    // Version 2: A changed, B same
    setenv("NIX_HASHCACHE_A", "valA2", 1);
    std::vector<Dep> deps2 = {
        makeEnvVarDep("NIX_HASHCACHE_A", "valA2"),
        makeEnvVarDep("NIX_HASHCACHE_B", "valB"),
    };
    db.coldStore("", string_t{"result-2", {}}, deps2, std::nullopt, true);

    // Revert A
    setenv("NIX_HASHCACHE_A", "valA", 1);
    db.clearSessionCaches();

    // warmPath should fail validation (deps2 doesn't match) then recover to result-1
    auto result = db.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"result-1", {}}, result->value, state.symbols);

    // currentHashCache should have entries from batch validation
    EXPECT_GE(db.currentHashCache.size(), 2u);
}

TEST_F(EvalCacheDbTest, WarmPath_BaseValidatedOnce)
{
    // Store 5 siblings sharing the same dep set.
    // Validate all 5 via warmPath. Base dep set should be validated only once.
    ScopedEnvVar env("NIX_BASE_VALID", "ok");

    auto db = makeDb();
    std::vector<Dep> sharedDeps = {makeEnvVarDep("NIX_BASE_VALID", "ok")};

    // Store 5 attrs with identical deps (all share the same dep set)
    for (int i = 0; i < 5; i++) {
        auto name = "sibling-" + std::to_string(i);
        db.coldStore(name, int_t{NixInt{i}}, sharedDeps, std::nullopt, false);
    }

    db.clearSessionCaches();

    // Warm all 5 — dep set validated on first, session-cached for rest
    for (int i = 0; i < 5; i++) {
        auto name = "sibling-" + std::to_string(i);
        auto result = db.warmPath(name, {}, state);
        ASSERT_TRUE(result.has_value()) << "Sibling " << i << " failed";
        ASSERT_TRUE(std::holds_alternative<int_t>(result->value));
        EXPECT_EQ(std::get<int_t>(result->value).x.value, i);
    }

    // validatedDepSetIds should have the shared dep set (only validated once)
    EXPECT_FALSE(db.validatedDepSetIds.empty());
}

// ── Full end-to-end delta + warm roundtrip ───────────────────────────

TEST_F(EvalCacheDbTest, DeltaEncoding_WarmRoundtrip)
{
    // End-to-end: cold store with delta encoding, then warm path retrieves correctly.
    // Use EnvVar deps so validation can actually check them (no files needed).
    ScopedEnvVar env1("NIX_DW_SHARED", "stable");
    ScopedEnvVar env2("NIX_DW_A", "a-val");
    ScopedEnvVar env3("NIX_DW_B", "b-val");

    auto db = makeDb();

    // 3 attrs with overlapping deps (all env vars — validatable)
    auto sharedDep = makeEnvVarDep("NIX_DW_SHARED", "stable");

    // Attr 1: shared + 1 unique
    std::vector<Dep> deps1 = {sharedDep, makeEnvVarDep("NIX_DW_A", "a-val")};
    db.coldStore("a", string_t{"val-a", {}}, deps1, std::nullopt, false);

    // Attr 2: shared + 1 different unique
    std::vector<Dep> deps2 = {sharedDep, makeEnvVarDep("NIX_DW_B", "b-val")};
    db.coldStore("b", string_t{"val-b", {}}, deps2, std::nullopt, false);

    // Attr 3: shared only
    std::vector<Dep> deps3 = {sharedDep};
    db.coldStore("c", string_t{"val-c", {}}, deps3, std::nullopt, false);

    db.clearSessionCaches();

    // Warm all 3
    auto ra = db.warmPath("a", {}, state);
    ASSERT_TRUE(ra.has_value());
    assertAttrValueEquals(string_t{"val-a", {}}, ra->value, state.symbols);

    auto rb = db.warmPath("b", {}, state);
    ASSERT_TRUE(rb.has_value());
    assertAttrValueEquals(string_t{"val-b", {}}, rb->value, state.symbols);

    auto rc = db.warmPath("c", {}, state);
    ASSERT_TRUE(rc.has_value());
    assertAttrValueEquals(string_t{"val-c", {}}, rc->value, state.symbols);
}

TEST_F(EvalCacheDbTest, OptimizeDepSets_NoDirty)
{
    // optimizeDepSets with no dirty sets should be a no-op
    auto db = makeDb();
    db.optimizeDepSets(); // Should not crash or throw
}

TEST_F(EvalCacheDbTest, OptimizeDepSets_SingletonGroup)
{
    // Single dep set in a struct group — no optimization needed
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};
    auto result = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // Should not crash; singleton groups are skipped
    db.optimizeDepSets();

    auto loaded = db.loadFullDepSet(result.depSetId);
    EXPECT_EQ(loaded.size(), 1u);
}

} // namespace nix::eval_cache
