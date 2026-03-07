#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <algorithm>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── getCurrentTraceHash tests (ParentContext dep infrastructure) ─────

TEST_F(TraceStoreTest, GetCurrentTraceHash_ReturnsHash)
{
    auto db = makeDb();
    db.record(vpath({"root"}), string_t{"val", {}}, {makeEnvVarDep(pools(), "NIX_GTH_1", "a")});

    auto hash = db.getCurrentTraceHash(vpath({"root"}));
    ASSERT_TRUE(hash.has_value());

    // Deterministic: same call returns same hash
    auto hash2 = db.getCurrentTraceHash(vpath({"root"}));
    ASSERT_TRUE(hash2.has_value());
    EXPECT_EQ(hash->to_string(HashFormat::Base16, false),
              hash2->to_string(HashFormat::Base16, false));
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_MissingAttr)
{
    auto db = makeDb();
    auto hash = db.getCurrentTraceHash(vpath({"nonexistent"}));
    EXPECT_FALSE(hash.has_value());
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_ChangesWithDeps)
{
    auto db = makeDb();

    // Record with deps A
    db.record(vpath({"root"}), string_t{"v1", {}}, {makeEnvVarDep(pools(), "NIX_GTH_2A", "a")});
    auto hash1 = db.getCurrentTraceHash(vpath({"root"}));
    ASSERT_TRUE(hash1.has_value());

    // Re-record with deps B (different deps → different trace hash)
    db.record(vpath({"root"}), string_t{"v2", {}}, {makeEnvVarDep(pools(), "NIX_GTH_2B", "b")});
    auto hash2 = db.getCurrentTraceHash(vpath({"root"}));
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

    db.record(vpath({"a"}), string_t{"same-result", {}}, {makeEnvVarDep(pools(), "NIX_GTH_3A", "a")});
    db.record(vpath({"b"}), string_t{"same-result", {}}, {makeEnvVarDep(pools(), "NIX_GTH_3B", "b")});

    auto hashA = db.getCurrentTraceHash(vpath({"a"}));
    auto hashB = db.getCurrentTraceHash(vpath({"b"}));
    ASSERT_TRUE(hashA.has_value());
    ASSERT_TRUE(hashB.has_value());

    // Same result but different deps → different trace hashes
    EXPECT_NE(hashA->to_string(HashFormat::Base16, false),
              hashB->to_string(HashFormat::Base16, false));
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_MultiComponentPath)
{
    auto db = makeDb();

    auto pathId = vpath({"packages", "x86_64-linux"});

    db.record(pathId, string_t{"val", {}},
              {makeEnvVarDep(pools(), "NIX_GTH_4", "v")});

    // getCurrentTraceHash with AttrPathId — direct lookup
    auto hash = db.getCurrentTraceHash(pathId);
    ASSERT_TRUE(hash.has_value());

    // Deterministic: same pathId returns same hash
    auto hash2 = db.getCurrentTraceHash(pathId);
    ASSERT_TRUE(hash2.has_value());
    EXPECT_EQ(hash->to_string(HashFormat::Base16, false),
              hash2->to_string(HashFormat::Base16, false));
}

// ── ParentContext dep verification tests ─────────────────────────────

TEST_F(TraceStoreTest, ParentContext_VerifiesWhenParentUnchanged)
{
    ScopedEnvVar env("NIX_PCV_1", "val");
    auto db = makeDb();

    // Record parent
    db.record(vpath({"parent"}), string_t{"parent-val", {}},
              {makeEnvVarDep(pools(), "NIX_PCV_1", "val")});
    auto parentHash = db.getCurrentTraceHash(vpath({"parent"}));
    ASSERT_TRUE(parentHash.has_value());

    // Record child with ParentContext dep
    db.record(vpath({"child"}), string_t{"child-val", {}},
              {makeParentContextDep(vpath({"parent"}), *parentHash)});

    db.clearSessionCaches();

    // Parent unchanged → child verification passes
    auto result = db.verify(vpath({"child"}), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, ParentContext_FailsWhenParentChanges)
{
    ScopedEnvVar env("NIX_PCV_2", "val1");
    auto db = makeDb();

    // Record parent v1
    db.record(vpath({"parent"}), string_t{"parent-v1", {}},
              {makeEnvVarDep(pools(), "NIX_PCV_2", "val1")});
    auto parentHash1 = db.getCurrentTraceHash(vpath({"parent"}));
    ASSERT_TRUE(parentHash1.has_value());

    // Record child with ParentContext dep on parent v1
    db.record(vpath({"child"}), string_t{"child-v1", {}},
              {makeParentContextDep(vpath({"parent"}), *parentHash1)});

    // Change parent deps → different trace hash
    setenv("NIX_PCV_2", "val2", 1);
    db.record(vpath({"parent"}), string_t{"parent-v2", {}},
              {makeEnvVarDep(pools(), "NIX_PCV_2", "val2")});

    // Verify parent trace hash changed
    auto parentHash2 = db.getCurrentTraceHash(vpath({"parent"}));
    ASSERT_TRUE(parentHash2.has_value());
    EXPECT_NE(parentHash1->to_string(HashFormat::Base16, false),
              parentHash2->to_string(HashFormat::Base16, false));

    db.clearSessionCaches();

    // Child verification fails: ParentContext dep mismatch
    auto result = db.verify(vpath({"child"}), {}, state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, ParentContext_RecoveryOnRevert)
{
    ScopedEnvVar env("NIX_PCV_3", "val1");
    auto db = makeDb();

    // Version 1: parent with val1
    db.record(vpath({"parent"}), string_t{"parent-v1", {}},
              {makeEnvVarDep(pools(), "NIX_PCV_3", "val1")});
    auto parentHash1 = db.getCurrentTraceHash(vpath({"parent"}));
    ASSERT_TRUE(parentHash1.has_value());

    // Child v1 with ParentContext dep
    db.record(vpath({"child"}), string_t{"child-v1", {}},
              {makeParentContextDep(vpath({"parent"}), *parentHash1)});

    // Version 2: parent with val2
    setenv("NIX_PCV_3", "val2", 1);
    db.record(vpath({"parent"}), string_t{"parent-v2", {}},
              {makeEnvVarDep(pools(), "NIX_PCV_3", "val2")});
    auto parentHash2 = db.getCurrentTraceHash(vpath({"parent"}));
    ASSERT_TRUE(parentHash2.has_value());

    // Child v2 with ParentContext dep
    db.record(vpath({"child"}), string_t{"child-v2", {}},
              {makeParentContextDep(vpath({"parent"}), *parentHash2)});

    // Revert parent to v1 (re-record so CurrentTraces points to v1 trace)
    setenv("NIX_PCV_3", "val1", 1);
    db.record(vpath({"parent"}), string_t{"parent-v1", {}},
              {makeEnvVarDep(pools(), "NIX_PCV_3", "val1")});

    db.clearSessionCaches();

    // Child recovery finds child v1 trace (ParentContext matches v1 hash)
    auto result = db.verify(vpath({"child"}), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-v1", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, ParentContext_WithMultiComponentPath)
{
    ScopedEnvVar env("NIX_PCV_4", "val");
    auto db = makeDb();

    // Record parent with multi-component attr path
    auto parentPathId = vpath({"packages", "x86_64-linux"});

    db.record(parentPathId, string_t{"parent-val", {}},
              {makeEnvVarDep(pools(), "NIX_PCV_4", "val")});
    auto parentHash = db.getCurrentTraceHash(parentPathId);
    ASSERT_TRUE(parentHash.has_value());

    // Record child with ParentContext dep using AttrPathId
    auto childPathId = vpath({"packages", "x86_64-linux", "hello"});

    db.record(childPathId, string_t{"child-val", {}},
              {makeParentContextDep(parentPathId, *parentHash)});

    db.clearSessionCaches();

    // Child verification passes — ParentContext dep with AttrPathId works correctly
    auto result = db.verify(childPathId, {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-val", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, ParentContext_MixedWithOwnDeps)
{
    ScopedEnvVar env1("NIX_PCV_5P", "parent-val");
    ScopedEnvVar env2("NIX_PCV_5C", "child-val");
    auto db = makeDb();

    // Record parent
    db.record(vpath({"parent"}), string_t{"parent-result", {}},
              {makeEnvVarDep(pools(), "NIX_PCV_5P", "parent-val")});
    auto parentHash = db.getCurrentTraceHash(vpath({"parent"}));
    ASSERT_TRUE(parentHash.has_value());

    // Record child with both own dep AND ParentContext dep
    std::vector<Dep> childDeps = {
        makeEnvVarDep(pools(), "NIX_PCV_5C", "child-val"),
        makeParentContextDep(vpath({"parent"}), *parentHash),
    };
    db.record(vpath({"child"}), string_t{"child-result", {}}, childDeps);

    db.clearSessionCaches();

    // Both deps pass → verification succeeds
    auto result = db.verify(vpath({"child"}), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"child-result", {}}, result->value, state.symbols);

    // Now change only the child's own dep
    setenv("NIX_PCV_5C", "child-val-new", 1);
    db.clearSessionCaches();

    // Verification fails (own dep stale, not just ParentContext)
    auto result2 = db.verify(vpath({"child"}), {}, state);
    EXPECT_FALSE(result2.has_value());
}

TEST_F(TraceStoreTest, ParentContext_SameResultDifferentDeps_Detects)
{
    // Key test: parent returns same result (same attrset shape) but different deps.
    // With trace hash (not result hash), the change is detected.
    auto db = makeDb();

    ScopedEnvVar env("NIX_PCV_6", "val-A");

    // Parent v1: result "same" with dep on val-A
    db.record(vpath({"parent"}), string_t{"same", {}},
              {makeEnvVarDep(pools(), "NIX_PCV_6", "val-A")});
    auto parentHash1 = db.getCurrentTraceHash(vpath({"parent"}));
    ASSERT_TRUE(parentHash1.has_value());

    // Child with ParentContext dep on parent v1
    db.record(vpath({"child"}), string_t{"child-from-A", {}},
              {makeParentContextDep(vpath({"parent"}), *parentHash1)});

    // Parent v2: SAME result "same" but different dep value
    setenv("NIX_PCV_6", "val-B", 1);
    db.record(vpath({"parent"}), string_t{"same", {}},
              {makeEnvVarDep(pools(), "NIX_PCV_6", "val-B")});

    // Parent trace hash changed even though result is identical
    auto parentHash2 = db.getCurrentTraceHash(vpath({"parent"}));
    ASSERT_TRUE(parentHash2.has_value());
    EXPECT_NE(parentHash1->to_string(HashFormat::Base16, false),
              parentHash2->to_string(HashFormat::Base16, false));

    db.clearSessionCaches();

    // Child verification fails — trace hash detects dep change despite same result
    auto result = db.verify(vpath({"child"}), {}, state);
    EXPECT_FALSE(result.has_value());
}

} // namespace nix::eval_trace
