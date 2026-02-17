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

TEST_F(EvalCacheDbTest, ColdStore_ReturnsAttrId)
{
    auto db = makeDb();
    auto attrId = db.coldStore("", string_t{"hello", {}}, {}, std::nullopt, true);
    // AttrId should be a positive integer
    EXPECT_GT(attrId, 0u);
}

TEST_F(EvalCacheDbTest, ColdStore_LookupAttrWorks)
{
    auto db = makeDb();
    auto attrId = db.coldStore("", string_t{"hello", {}}, {}, std::nullopt, true);

    auto found = db.lookupAttr("");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, attrId);
}

TEST_F(EvalCacheDbTest, ColdStore_WithDeps)
{
    auto db = makeDb();
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "a"),
        makeEnvVarDep("HOME", "/home"),
    };

    auto attrId = db.coldStore("", int_t{NixInt{42}}, deps, std::nullopt, true);

    auto loadedDeps = db.loadDepsForAttr(attrId);
    EXPECT_EQ(loadedDeps.size(), 2u);
}

TEST_F(EvalCacheDbTest, ColdStore_VolatileDep_NotSessionCached)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep()};

    auto attrId = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // CurrentTime dep -> should NOT be in validatedAttrIds
    EXPECT_FALSE(db.validatedAttrIds.count(attrId));
}

TEST_F(EvalCacheDbTest, ColdStore_NonVolatile_SessionCached)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};

    auto attrId = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // Non-volatile dep -> SHOULD be in validatedAttrIds
    EXPECT_TRUE(db.validatedAttrIds.count(attrId));
}

TEST_F(EvalCacheDbTest, ColdStore_ParentContextFiltered)
{
    auto db = makeDb();
    std::vector<Dep> deps = {
        makeContentDep("/a.nix", "a"),
        Dep{"", "", DepHashValue(std::string("parent-hash")), DepType::ParentContext},
    };

    auto attrId = db.coldStore("", null_t{}, deps, std::nullopt, true);

    auto loadedDeps = db.loadDepsForAttr(attrId);
    // ParentContext should be filtered out -- only 1 dep stored
    EXPECT_EQ(loadedDeps.size(), 1u);
    EXPECT_EQ(loadedDeps[0].type, DepType::Content);
}

TEST_F(EvalCacheDbTest, ColdStore_WithParent)
{
    auto db = makeDb();

    // Store parent
    auto parentId = db.coldStore("", string_t{"parent-val", {}},
                                  {makeContentDep("/a.nix", "a")}, std::nullopt, true);

    // Store child referencing parent
    auto childId = db.coldStore("child", string_t{"child-val", {}},
                                 {makeEnvVarDep("FOO", "bar")}, parentId, false);

    // Verify child stored correctly
    auto childDeps = db.loadDepsForAttr(childId);
    EXPECT_EQ(childDeps.size(), 1u);
    EXPECT_EQ(childDeps[0].type, DepType::EnvVar);
}

TEST_F(EvalCacheDbTest, ColdStore_Deterministic)
{
    // Same inputs should produce consistent attrId (upsert)
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "a")};
    AttrValue value = string_t{"result", {}};

    auto id1 = db.coldStore("", value, deps, std::nullopt, true);
    auto id2 = db.coldStore("", value, deps, std::nullopt, true);

    // ON CONFLICT DO UPDATE preserves attr_id
    EXPECT_EQ(id1, id2);
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

    // Two attributes with identical deps should share the same dep set
    auto id1 = db.coldStore("a", string_t{"val1", {}}, deps, std::nullopt, false);
    auto id2 = db.coldStore("b", string_t{"val2", {}}, deps, std::nullopt, false);

    // Both should have loadable deps
    auto deps1 = db.loadDepsForAttr(id1);
    auto deps2 = db.loadDepsForAttr(id2);
    EXPECT_EQ(deps1.size(), 1u);
    EXPECT_EQ(deps2.size(), 1u);
}

TEST_F(EvalCacheDbTest, EmptyDepSet_HasDbRow)
{
    // Attributes with zero deps should still get a dep set row
    auto db = makeDb();

    auto attrId = db.coldStore("", string_t{"val", {}}, {}, std::nullopt, true);

    // loadDepsForAttr should return empty vector (not fail)
    auto deps = db.loadDepsForAttr(attrId);
    EXPECT_TRUE(deps.empty());

    // Attribute should be valid (empty dep set passes validation)
    EXPECT_TRUE(db.validateAttr(attrId, {}, state));
}

// ── validateAttr tests ──────────────────────────────────────────────

TEST_F(EvalCacheDbTest, ValidateAttr_EnvVar_Valid)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR", "expected_value");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR", "expected_value")};
    auto attrId = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // Clear session cache so validateAttr actually checks
    db.clearSessionCaches();

    bool valid = db.validateAttr(attrId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(EvalCacheDbTest, ValidateAttr_EnvVar_Invalid)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR", "new_value");

    auto db = makeDb();
    // Create attr with OLD expected hash
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR", "old_value")};
    auto attrId = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // Clear session cache so validateAttr actually checks
    db.clearSessionCaches();

    bool valid = db.validateAttr(attrId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheDbTest, ValidateAttr_CurrentTime_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep()};
    auto attrId = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // CurrentTime is volatile, so not session-cached
    bool valid = db.validateAttr(attrId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheDbTest, ValidateAttr_Exec_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeExecDep()};
    auto attrId = db.coldStore("", null_t{}, deps, std::nullopt, true);

    bool valid = db.validateAttr(attrId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheDbTest, ValidateAttr_SessionCacheHit)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR2", "val");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep("NIX_TEST_CACHE_VAR2", "val")};
    auto attrId = db.coldStore("", null_t{}, deps, std::nullopt, true);

    // coldStore with non-volatile deps should have session-cached it
    EXPECT_TRUE(db.validatedAttrIds.count(attrId));
    // Second call should use session cache (instant return)
    EXPECT_TRUE(db.validateAttr(attrId, {}, state));
}

TEST_F(EvalCacheDbTest, ValidateAttr_NoDeps_Valid)
{
    auto db = makeDb();
    auto attrId = db.coldStore("", string_t{"val", {}}, {}, std::nullopt, true);

    db.clearSessionCaches();

    bool valid = db.validateAttr(attrId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(EvalCacheDbTest, ValidateAttr_ParentInvalid)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "current_value");

    auto db = makeDb();

    // Create parent with stale dep
    std::vector<Dep> staleDeps = {makeEnvVarDep("NIX_TEST_PARENT", "stale_value")};
    auto parentId = db.coldStore("", string_t{"parent", {}},
                                  staleDeps, std::nullopt, true);

    // Create child with no direct deps, but parent is invalid
    auto childId = db.coldStore("child", string_t{"child", {}},
                                 {}, parentId, false);

    // Clear session cache
    db.clearSessionCaches();

    bool valid = db.validateAttr(childId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheDbTest, ValidateAttr_ParentValid)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "correct_value");

    auto db = makeDb();

    // Create parent with correct dep
    std::vector<Dep> parentDeps = {makeEnvVarDep("NIX_TEST_PARENT", "correct_value")};
    auto parentId = db.coldStore("", string_t{"parent", {}},
                                  parentDeps, std::nullopt, true);

    // Create child referencing valid parent
    auto childId = db.coldStore("child", string_t{"child", {}},
                                 {}, parentId, false);

    // Clear session cache
    db.clearSessionCaches();

    bool valid = db.validateAttr(childId, {}, state);
    EXPECT_TRUE(valid);
}

TEST_F(EvalCacheDbTest, ValidateAttr_MultipleDeps_OneInvalid)
{
    ScopedEnvVar env1("NIX_TEST_VALID", "current_value");
    ScopedEnvVar env2("NIX_TEST_STALE", "new_value");

    auto db = makeDb();
    std::vector<Dep> deps = {
        makeEnvVarDep("NIX_TEST_VALID", "current_value"),  // valid
        makeEnvVarDep("NIX_TEST_STALE", "old_value"),      // invalid (different hash)
    };
    auto attrId = db.coldStore("", null_t{}, deps, std::nullopt, true);

    db.clearSessionCaches();

    bool valid = db.validateAttr(attrId, {}, state);
    EXPECT_FALSE(valid);
}

TEST_F(EvalCacheDbTest, ValidateAttr_ParentEpochMismatch)
{
    // Parent epoch mismatch: parent re-cold-stored (epoch increments),
    // child's stored parent_epoch no longer matches → validation fails.
    ScopedEnvVar env("NIX_EPOCH_TEST", "val");

    auto db = makeDb();

    // Cold store parent
    std::vector<Dep> parentDeps = {makeEnvVarDep("NIX_EPOCH_TEST", "val")};
    auto parentId = db.coldStore("", string_t{"parent_v1", {}}, parentDeps, std::nullopt, true);

    // Cold store child referencing parent
    auto childId = db.coldStore("child", string_t{"child_v1", {}}, {}, parentId, false);

    // Re-cold-store parent with DIFFERENT value (same deps, so deps still validate),
    // which increments its epoch
    db.coldStore("", string_t{"parent_v2", {}}, parentDeps, std::nullopt, true);

    // Clear session caches to force actual DB validation
    db.clearSessionCaches();

    // Child should fail: its stored parent_epoch doesn't match parent's new epoch
    bool valid = db.validateAttr(childId, {}, state);
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

TEST_F(EvalCacheDbTest, ColdWarm_Roundtrip_AttrId)
{
    ScopedEnvVar env("NIX_WARM_TEST2", "stable");

    auto db = makeDb();
    AttrValue input = int_t{NixInt{99}};
    std::vector<Dep> deps = {makeEnvVarDep("NIX_WARM_TEST2", "stable")};

    auto coldAttrId = db.coldStore("", input, deps, std::nullopt, true);

    auto result = db.warmPath("", {}, state);
    ASSERT_TRUE(result.has_value());
    // Warm result's attrId should match what coldStore returned
    EXPECT_EQ(result->attrId, coldAttrId);
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

// ── Three-phase recovery tests ───────────────────────────────────────

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

TEST_F(EvalCacheDbTest, Phase2_ParentLookup_NoDeps)
{
    // Phase 2: dep-less child, parent changed. Phase 2 finds by parent identity.
    ScopedEnvVar env("NIX_P2_ROOT", "val1");

    auto db = makeDb();

    // Cold store root with val1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P2_ROOT", "val1")};
    auto rootId1 = db.coldStore("", string_t{"root1", {}}, rootDeps1, std::nullopt, true);

    // Cold store child (no deps) referencing root1
    db.coldStore("child", string_t{"child1", {}}, {}, rootId1, false);

    // Cold store root with val2
    setenv("NIX_P2_ROOT", "val2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P2_ROOT", "val2")};
    auto rootId2 = db.coldStore("", string_t{"root2", {}}, rootDeps2, std::nullopt, true);

    // Cold store child referencing root2
    db.coldStore("child", string_t{"child2", {}}, {}, rootId2, false);

    // Revert to val1: root should be recovered, then child via Phase 2
    setenv("NIX_P2_ROOT", "val1", 1);
    db.clearSessionCaches();

    // Root recovery (Phase 1)
    auto rootResult = db.warmPath("", {}, state);
    ASSERT_TRUE(rootResult.has_value());
    assertAttrValueEquals(string_t{"root1", {}}, rootResult->value, state.symbols);

    // Child recovery (Phase 2 -- uses parent hint)
    auto childResult = db.warmPath("child", {}, state, rootResult->attrId);
    ASSERT_TRUE(childResult.has_value());
    assertAttrValueEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(EvalCacheDbTest, Phase2_ParentLookup_WithDeps)
{
    // Phase 2: child with deps, parent changed. Phase 2 finds.
    ScopedEnvVar env1("NIX_P2W_ROOT", "rval1");
    ScopedEnvVar env2("NIX_P2W_CHILD", "cval");

    auto db = makeDb();

    // Cold store root with rval1
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P2W_ROOT", "rval1")};
    auto rootId1 = db.coldStore("", string_t{"root1", {}}, rootDeps1, std::nullopt, true);

    // Cold store child with stable dep + parent
    std::vector<Dep> childDeps = {makeEnvVarDep("NIX_P2W_CHILD", "cval")};
    db.coldStore("child", string_t{"child1", {}}, childDeps, rootId1, false);

    // Cold store root with rval2
    setenv("NIX_P2W_ROOT", "rval2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P2W_ROOT", "rval2")};
    auto rootId2 = db.coldStore("", string_t{"root2", {}}, rootDeps2, std::nullopt, true);

    // Cold store child referencing root2
    db.coldStore("child", string_t{"child2", {}}, childDeps, rootId2, false);

    // Revert to rval1
    setenv("NIX_P2W_ROOT", "rval1", 1);
    db.clearSessionCaches();

    auto rootResult = db.warmPath("", {}, state);
    ASSERT_TRUE(rootResult.has_value());

    auto childResult = db.warmPath("child", {}, state, rootResult->attrId);
    ASSERT_TRUE(childResult.has_value());
    assertAttrValueEquals(string_t{"child1", {}}, childResult->value, state.symbols);
}

TEST_F(EvalCacheDbTest, Phase2_CascadeThroughTree)
{
    // Root -> child1 -> child2 (grandchild), with deps at each level.
    // Phase 2 uses parent's Merkle identity hash (value + deps + ancestors),
    // so each level's identity changes when deps change, propagating down
    // the tree to invalidate cached children.
    ScopedEnvVar env1("NIX_P2C_ROOT", "v1");
    ScopedEnvVar env2("NIX_P2C_CHILD", "cv1");

    auto db = makeDb();

    // Build null-byte-separated attr path for grandchild
    std::string c2AttrPath = "c1";
    c2AttrPath.push_back('\0');
    c2AttrPath.append("c2");

    // Cold store: root -> child1 (with dep) -> child2
    // Each level has its own dep that changes between versions,
    // ensuring different identity hashes at each level.
    std::vector<Dep> rootDeps1 = {makeEnvVarDep("NIX_P2C_ROOT", "v1")};
    std::vector<Dep> childDeps1 = {makeEnvVarDep("NIX_P2C_CHILD", "cv1")};
    auto root1 = db.coldStore("", string_t{"r1", {}}, rootDeps1, std::nullopt, true);
    auto child1_1 = db.coldStore("c1", string_t{"c1v1", {}}, childDeps1, root1, false);
    db.coldStore(c2AttrPath, string_t{"c2v1", {}}, {}, child1_1, false);

    // Second cold store — both root and child deps change
    setenv("NIX_P2C_ROOT", "v2", 1);
    setenv("NIX_P2C_CHILD", "cv2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_P2C_ROOT", "v2")};
    std::vector<Dep> childDeps2 = {makeEnvVarDep("NIX_P2C_CHILD", "cv2")};
    auto root2 = db.coldStore("", string_t{"r2", {}}, rootDeps2, std::nullopt, true);
    auto child1_2 = db.coldStore("c1", string_t{"c1v2", {}}, childDeps2, root2, false);
    db.coldStore(c2AttrPath, string_t{"c2v2", {}}, {}, child1_2, false);

    // Revert to v1
    setenv("NIX_P2C_ROOT", "v1", 1);
    setenv("NIX_P2C_CHILD", "cv1", 1);
    db.clearSessionCaches();

    auto rootR = db.warmPath("", {}, state);
    ASSERT_TRUE(rootR.has_value());
    assertAttrValueEquals(string_t{"r1", {}}, rootR->value, state.symbols);

    auto c1R = db.warmPath("c1", {}, state, rootR->attrId);
    ASSERT_TRUE(c1R.has_value());
    assertAttrValueEquals(string_t{"c1v1", {}}, c1R->value, state.symbols);

    auto c2R = db.warmPath(c2AttrPath, {}, state, c1R->attrId);
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

TEST_F(EvalCacheDbTest, Phase2_FallbackToPhase3)
{
    // No parent hint -- Phase 2 skipped, Phase 3 succeeds
    ScopedEnvVar env1("NIX_P2F_A", "a");
    ScopedEnvVar env2("NIX_P2F_B", "b");

    auto db = makeDb();

    // Two structures
    db.coldStore("", string_t{"r1", {}},
                  {makeEnvVarDep("NIX_P2F_A", "a")}, std::nullopt, true);
    db.coldStore("", string_t{"r2", {}},
                  {makeEnvVarDep("NIX_P2F_A", "a"), makeEnvVarDep("NIX_P2F_B", "b")},
                  std::nullopt, true);

    // Invalidate B
    setenv("NIX_P2F_B", "b_new", 1);
    db.clearSessionCaches();

    // No parent hint (root attr) -- Phase 2 skipped
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

TEST_F(EvalCacheDbTest, Phase1_2_3_Cascade)
{
    // All three phases attempted in order -- Phase 1 and 2 fail, Phase 3 succeeds
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
    // Phase 2 has no parent hint -> skipped
    // Phase 3 finds structure 1 (only A) -> succeeds
    setenv("NIX_CASCADE_B", "b_new", 1);
    db.clearSessionCaches();

    auto result = db.warmPath("child", {}, state);
    ASSERT_TRUE(result.has_value());
    assertAttrValueEquals(string_t{"target", {}}, result->value, state.symbols);
}

TEST_F(EvalCacheDbTest, Phase2_DeepChainRecovery)
{
    // Deep chain (depth 6): root → c1 → c2 → c3 → c4 → c5
    // Store two versions, revert, verify Phase 2 recovery at deepest level.
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
    auto c1_1 = db.coldStore(c1Path, string_t{"c1_v1", {}}, c1Deps1, root1, false);
    auto c2_1 = db.coldStore(c2Path, string_t{"c2_v1", {}}, {}, c1_1, false);
    auto c3_1 = db.coldStore(c3Path, string_t{"c3_v1", {}}, {}, c2_1, false);
    auto c4_1 = db.coldStore(c4Path, string_t{"c4_v1", {}}, {}, c3_1, false);
    db.coldStore(c5Path, string_t{"c5_v1", {}}, {}, c4_1, false);

    // Version 2: different values
    setenv("NIX_DEEP_ROOT", "v2", 1);
    setenv("NIX_DEEP_C1", "v2", 1);
    std::vector<Dep> rootDeps2 = {makeEnvVarDep("NIX_DEEP_ROOT", "v2")};
    std::vector<Dep> c1Deps2 = {makeEnvVarDep("NIX_DEEP_C1", "v2")};
    auto root2 = db.coldStore("", string_t{"root_v2", {}}, rootDeps2, std::nullopt, true);
    auto c1_2 = db.coldStore(c1Path, string_t{"c1_v2", {}}, c1Deps2, root2, false);
    auto c2_2 = db.coldStore(c2Path, string_t{"c2_v2", {}}, {}, c1_2, false);
    auto c3_2 = db.coldStore(c3Path, string_t{"c3_v2", {}}, {}, c2_2, false);
    auto c4_2 = db.coldStore(c4Path, string_t{"c4_v2", {}}, {}, c3_2, false);
    db.coldStore(c5Path, string_t{"c5_v2", {}}, {}, c4_2, false);

    // Revert to v1
    setenv("NIX_DEEP_ROOT", "v1", 1);
    setenv("NIX_DEEP_C1", "v1", 1);
    db.clearSessionCaches();

    // Recover root (Phase 1)
    auto rootR = db.warmPath("", {}, state);
    ASSERT_TRUE(rootR.has_value());
    assertAttrValueEquals(string_t{"root_v1", {}}, rootR->value, state.symbols);

    // Recover chain down to c5 (Phase 2 at each level)
    auto c1R = db.warmPath(c1Path, {}, state, rootR->attrId);
    ASSERT_TRUE(c1R.has_value());
    assertAttrValueEquals(string_t{"c1_v1", {}}, c1R->value, state.symbols);

    auto c2R = db.warmPath(c2Path, {}, state, c1R->attrId);
    ASSERT_TRUE(c2R.has_value());
    assertAttrValueEquals(string_t{"c2_v1", {}}, c2R->value, state.symbols);

    auto c3R = db.warmPath(c3Path, {}, state, c2R->attrId);
    ASSERT_TRUE(c3R.has_value());
    assertAttrValueEquals(string_t{"c3_v1", {}}, c3R->value, state.symbols);

    auto c4R = db.warmPath(c4Path, {}, state, c3R->attrId);
    ASSERT_TRUE(c4R.has_value());
    assertAttrValueEquals(string_t{"c4_v1", {}}, c4R->value, state.symbols);

    auto c5R = db.warmPath(c5Path, {}, state, c4R->attrId);
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
    // All 3 phases fail: attribute has dep on env var, env var changed to a value
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

    auto attrId = db.coldStore(attrPath, string_t{"val", {}}, {}, std::nullopt, false);

    auto result = db.lookupAttr(attrPath);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, attrId);
}

TEST_F(EvalCacheDbTest, EmptyAttrPath)
{
    auto db = makeDb();
    auto attrId = db.coldStore("", string_t{"root-val", {}}, {}, std::nullopt, true);

    auto result = db.lookupAttr("");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, attrId);
}

TEST_F(EvalCacheDbTest, MultipleEntries_Stress)
{
    auto db = makeDb();

    // Insert 100 entries
    for (int i = 0; i < 100; i++) {
        auto name = "stress-" + std::to_string(i);
        db.coldStore(name, int_t{NixInt{i}}, {}, std::nullopt, false);
    }

    auto r0 = db.lookupAttr("stress-0");
    ASSERT_TRUE(r0.has_value());

    auto r99 = db.lookupAttr("stress-99");
    ASSERT_TRUE(r99.has_value());

    auto rMissing = db.lookupAttr("stress-100");
    EXPECT_FALSE(rMissing.has_value());
}

} // namespace nix::eval_cache
