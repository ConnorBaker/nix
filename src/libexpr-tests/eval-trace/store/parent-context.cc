#include "eval-trace/helpers.hh"
#include "nix/expr/trace-store.hh"
#include "nix/expr/trace-hash.hh"

#include <algorithm>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── getCurrentTraceHash tests (ParentContext dep infrastructure) ─────

TEST_F(TraceStoreTest, GetCurrentTraceHash_ReturnsHash)
{
    auto db = makeDb();
    db.recordDeps("root", string_t{"val", {}}, {makeEnvVarDep("NIX_GTH_1", "a")}, true);

    auto hash = db.getCurrentTraceHash("root");
    ASSERT_TRUE(hash.has_value());

    // Deterministic: same call returns same hash
    auto hash2 = db.getCurrentTraceHash("root");
    ASSERT_TRUE(hash2.has_value());
    EXPECT_EQ(hash->to_string(HashFormat::Base16, false),
              hash2->to_string(HashFormat::Base16, false));
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_MissingAttr)
{
    auto db = makeDb();
    auto hash = db.getCurrentTraceHash("nonexistent");
    EXPECT_FALSE(hash.has_value());
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_ChangesWithDeps)
{
    auto db = makeDb();

    // Record with deps A
    db.recordDeps("root", string_t{"v1", {}}, {makeEnvVarDep("NIX_GTH_2A", "a")}, true);
    auto hash1 = db.getCurrentTraceHash("root");
    ASSERT_TRUE(hash1.has_value());

    // Re-record with deps B (different deps → different trace hash)
    db.recordDeps("root", string_t{"v2", {}}, {makeEnvVarDep("NIX_GTH_2B", "b")}, true);
    auto hash2 = db.getCurrentTraceHash("root");
    ASSERT_TRUE(hash2.has_value());

    EXPECT_NE(hash1->to_string(HashFormat::Base16, false),
              hash2->to_string(HashFormat::Base16, false));
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_DiffersFromResultHash)
{
    // Two attrs with identical results but different deps should have
    // different trace hashes. This is the key property: trace hash captures
    // dep structure, not just result content.
    auto db = makeDb();

    db.recordDeps("a", string_t{"same-result", {}}, {makeEnvVarDep("NIX_GTH_3A", "a")}, false);
    db.recordDeps("b", string_t{"same-result", {}}, {makeEnvVarDep("NIX_GTH_3B", "b")}, false);

    auto hashA = db.getCurrentTraceHash("a");
    auto hashB = db.getCurrentTraceHash("b");
    ASSERT_TRUE(hashA.has_value());
    ASSERT_TRUE(hashB.has_value());

    // Same result but different deps → different trace hashes
    EXPECT_NE(hashA->to_string(HashFormat::Base16, false),
              hashB->to_string(HashFormat::Base16, false));
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_TabSeparatorConversion)
{
    auto db = makeDb();

    // Build a null-byte-separated attr path (like buildAttrPath produces)
    std::string attrPath = "packages";
    attrPath.push_back('\0');
    attrPath.append("x86_64-linux");

    db.recordDeps(attrPath, string_t{"val", {}},
              {makeEnvVarDep("NIX_GTH_4", "v")}, false);

    // Convert \0 to \t (as trace-cache.cc does for ParentContext dep keys)
    std::string depKey = attrPath;
    std::replace(depKey.begin(), depKey.end(), '\0', '\t');

    // getCurrentTraceHash converts \t back to \0 internally
    auto hashViaTab = db.getCurrentTraceHash(depKey);
    ASSERT_TRUE(hashViaTab.has_value());

    // Should match direct lookup with original \0-separated path
    auto hashDirect = db.getCurrentTraceHash(attrPath);
    ASSERT_TRUE(hashDirect.has_value());

    EXPECT_EQ(hashViaTab->to_string(HashFormat::Base16, false),
              hashDirect->to_string(HashFormat::Base16, false));
}

// ── ParentContext dep verification tests ─────────────────────────────

TEST_F(TraceStoreTest, ParentContext_VerifiesWhenParentUnchanged)
{
    ScopedEnvVar env("NIX_PCV_1", "val");
    auto db = makeDb();

    // Record parent
    db.recordDeps("parent", string_t{"parent-val", {}},
              {makeEnvVarDep("NIX_PCV_1", "val")}, true);
    auto parentHash = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash.has_value());

    // Record child with ParentContext dep
    db.recordDeps("child", string_t{"child-val", {}},
              {makeParentContextDep("parent", *parentHash)}, false);

    db.clearSessionCaches();

    // Parent unchanged → child verification passes
    auto result = db.verify("child", {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, ParentContext_FailsWhenParentChanges)
{
    ScopedEnvVar env("NIX_PCV_2", "val1");
    auto db = makeDb();

    // Record parent v1
    db.recordDeps("parent", string_t{"parent-v1", {}},
              {makeEnvVarDep("NIX_PCV_2", "val1")}, true);
    auto parentHash1 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash1.has_value());

    // Record child with ParentContext dep on parent v1
    db.recordDeps("child", string_t{"child-v1", {}},
              {makeParentContextDep("parent", *parentHash1)}, false);

    // Change parent deps → different trace hash
    setenv("NIX_PCV_2", "val2", 1);
    db.recordDeps("parent", string_t{"parent-v2", {}},
              {makeEnvVarDep("NIX_PCV_2", "val2")}, true);

    // Verify parent trace hash changed
    auto parentHash2 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash2.has_value());
    EXPECT_NE(parentHash1->to_string(HashFormat::Base16, false),
              parentHash2->to_string(HashFormat::Base16, false));

    db.clearSessionCaches();

    // Child verification fails: ParentContext dep mismatch
    auto result = db.verify("child", {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, ParentContext_RecoveryOnRevert)
{
    ScopedEnvVar env("NIX_PCV_3", "val1");
    auto db = makeDb();

    // Version 1: parent with val1
    db.recordDeps("parent", string_t{"parent-v1", {}},
              {makeEnvVarDep("NIX_PCV_3", "val1")}, true);
    auto parentHash1 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash1.has_value());

    // Child v1 with ParentContext dep
    db.recordDeps("child", string_t{"child-v1", {}},
              {makeParentContextDep("parent", *parentHash1)}, false);

    // Version 2: parent with val2
    setenv("NIX_PCV_3", "val2", 1);
    db.recordDeps("parent", string_t{"parent-v2", {}},
              {makeEnvVarDep("NIX_PCV_3", "val2")}, true);
    auto parentHash2 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash2.has_value());

    // Child v2 with ParentContext dep
    db.recordDeps("child", string_t{"child-v2", {}},
              {makeParentContextDep("parent", *parentHash2)}, false);

    // Revert parent to v1 (re-record so CurrentTraces points to v1 trace)
    setenv("NIX_PCV_3", "val1", 1);
    db.recordDeps("parent", string_t{"parent-v1", {}},
              {makeEnvVarDep("NIX_PCV_3", "val1")}, true);

    db.clearSessionCaches();

    // Child recovery finds child v1 trace (ParentContext matches v1 hash)
    auto result = db.verify("child", {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-v1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, ParentContext_WithTabSeparatedKey)
{
    ScopedEnvVar env("NIX_PCV_4", "val");
    auto db = makeDb();

    // Record parent with null-byte-separated attr path
    std::string parentPath = "packages";
    parentPath.push_back('\0');
    parentPath.append("x86_64-linux");

    db.recordDeps(parentPath, string_t{"parent-val", {}},
              {makeEnvVarDep("NIX_PCV_4", "val")}, false);
    auto parentHash = db.getCurrentTraceHash(parentPath);
    ASSERT_TRUE(parentHash.has_value());

    // Build dep key with \t separator (as trace-cache.cc does)
    std::string depKey = parentPath;
    std::replace(depKey.begin(), depKey.end(), '\0', '\t');

    // Record child with ParentContext dep using \t-separated key
    std::string childPath = parentPath;
    childPath.push_back('\0');
    childPath.append("hello");

    db.recordDeps(childPath, string_t{"child-val", {}},
              {makeParentContextDep(depKey, *parentHash)}, false);

    db.clearSessionCaches();

    // Child verification passes — ParentContext dep with \t key works correctly
    auto result = db.verify(childPath, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, ParentContext_MixedWithOwnDeps)
{
    ScopedEnvVar env1("NIX_PCV_5P", "parent-val");
    ScopedEnvVar env2("NIX_PCV_5C", "child-val");
    auto db = makeDb();

    // Record parent
    db.recordDeps("parent", string_t{"parent-result", {}},
              {makeEnvVarDep("NIX_PCV_5P", "parent-val")}, true);
    auto parentHash = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash.has_value());

    // Record child with both own dep AND ParentContext dep
    std::vector<Dep> childDeps = {
        makeEnvVarDep("NIX_PCV_5C", "child-val"),
        makeParentContextDep("parent", *parentHash),
    };
    db.recordDeps("child", string_t{"child-result", {}}, childDeps, false);

    db.clearSessionCaches();

    // Both deps pass → verification succeeds
    auto result = db.verify("child", {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-result", {}}, result->value, state.symbols);

    // Now change only the child's own dep
    setenv("NIX_PCV_5C", "child-val-new", 1);
    db.clearSessionCaches();

    // Verification fails (own dep stale, not just ParentContext)
    auto result2 = db.verify("child", {}, state);
    EXPECT_FALSE(result2.has_value());
}

TEST_F(TraceStoreTest, ParentContext_SameResultDifferentDeps_Detects)
{
    // Key test: parent returns same result (same attrset shape) but different deps.
    // With trace hash (not result hash), the change is detected.
    auto db = makeDb();

    ScopedEnvVar env("NIX_PCV_6", "val-A");

    // Parent v1: result "same" with dep on val-A
    db.recordDeps("parent", string_t{"same", {}},
              {makeEnvVarDep("NIX_PCV_6", "val-A")}, true);
    auto parentHash1 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash1.has_value());

    // Child with ParentContext dep on parent v1
    db.recordDeps("child", string_t{"child-from-A", {}},
              {makeParentContextDep("parent", *parentHash1)}, false);

    // Parent v2: SAME result "same" but different dep value
    setenv("NIX_PCV_6", "val-B", 1);
    db.recordDeps("parent", string_t{"same", {}},
              {makeEnvVarDep("NIX_PCV_6", "val-B")}, true);

    // Parent trace hash changed even though result is identical
    auto parentHash2 = db.getCurrentTraceHash("parent");
    ASSERT_TRUE(parentHash2.has_value());
    EXPECT_NE(parentHash1->to_string(HashFormat::Base16, false),
              parentHash2->to_string(HashFormat::Base16, false));

    db.clearSessionCaches();

    // Child verification fails — trace hash detects dep change despite same result
    auto result = db.verify("child", {}, state);
    EXPECT_FALSE(result.has_value());
}

} // namespace nix::eval_trace
