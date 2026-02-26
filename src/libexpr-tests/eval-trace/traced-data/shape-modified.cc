#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ═══════════════════════════════════════════════════════════════════════
// shapeModified — single-source removeAttrs
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, RemoveAttrs_AttrNames_KeyAdded)
{
    // removeAttrs removes "y", attrNames on result, add "z" to source → re-eval
    // (#keys suppressed because output size ≠ input size)
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.attrNames (builtins.removeAttrs (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()) ["y"]))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 1); // just "x"
    }

    file.modify(R"({"x": 1, "y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys suppressed → Content fails → re-eval
        EXPECT_EQ(v.listSize(), 2); // x, z (y still removed)
    }
}

TEST_F(TracedDataTest, RemoveAttrs_HasKey_SiblingAdded)
{
    // hasAttr "x" on removeAttrs result, add "z" to source → cache hit
    // (#has:x NOT suppressed on single-source; x still exists)
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.hasAttr "x" (builtins.removeAttrs (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()) ["y"]))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"x": 1, "y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #has:x passes
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(TracedDataTest, RemoveAttrs_AttrNames_NoopRemove)
{
    // removeAttrs of absent key → no actual removal → sizes match → NOT shapeModified.
    // #keys IS recorded and correctly detects when source gains a key.
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.attrNames (builtins.removeAttrs (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()) ["absent"]))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2); // x, y
    }

    file.modify(R"({"x": 1, "y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys NOT suppressed (sizes matched) → detects change
        EXPECT_EQ(v.listSize(), 3); // x, y, z
    }
}

TEST_F(TracedDataTest, RemoveAttrs_HasKey_QueriedKeyRemoved)
{
    // hasAttr "y" on removeAttrs result where "y" was removed → always false.
    // #has:y is recorded against source provenance with hash("0"), but source
    // has y → verification computes hash("1") → SC dep fails → re-eval.
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.hasAttr "y" (builtins.removeAttrs (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()) ["y"]))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsFalse()); // y removed
    }

    // Change value of y in source — result is still false but must re-eval
    // because Content dep fails and #has:y SC dep also fails (source has y,
    // but we recorded hash("0"))
    file.modify(R"({"x": 1, "y": 999})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsFalse());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified — multi-source #has:key
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, MultiProv_Update_HasKey_BothHave)
{
    // hasAttr "x" (fj a // fj b) where both have "x" and identical key sets.
    // a={x:1,y:2}, b={x:10,y:20} → output={x:10,y:20} (b wins).
    // #has:x recorded on b's origin only. When a changes (value, not keys),
    // Content(a) fails. No SC dep covers a, but ImplicitShape #keys from
    // creation time provides fallback verification: a's keys unchanged →
    // override succeeds → cache hit.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "y": 20})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"() // builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    // Change value in a (not keys) → cache hit (ImplicitShape #keys passes)
    fileA.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // ImplicitShape fallback: a's keys unchanged
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(TracedDataTest, MultiProv_Update_HasKey_OnlyAHas)
{
    // hasAttr "x" (fj a // fj b) where only a has "x"; add key to b → re-eval.
    // a={x:1}, b={y:2} → output={x:1,y:2}. Output size (2) ≠ a size (1) ≠ b size (1)
    // → shapeModified=true + multi-source → #has:key suppressed → Content fails
    // without SC coverage → re-eval.
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    auto expr = R"(builtins.hasAttr "x" (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"() // builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    fileB.modify(R"({"y": 2, "z": 3})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // shapeModified + multi-source → #has suppressed → re-eval
        EXPECT_THAT(v, IsTrue()); // x still exists after re-eval
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified — single-source mapAttrs (keys always preserved)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, MapAttrs_AttrNames_KeyAdded)
{
    // mapAttrs NOT shapeModified → #keys recorded → detects key change
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.attrNames (builtins.mapAttrs (n: v: v) (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"())))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    file.modify(R"({"x": 1, "y": 2, "z": 3})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #keys NOT suppressed, detects key change
        EXPECT_EQ(v.listSize(), 3);
    }
}

TEST_F(TracedDataTest, MapAttrs_AttrNames_ValueChanged)
{
    // mapAttrs → #keys recorded, change value (not keys) → cache hit
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.attrNames (builtins.mapAttrs (n: v: v) (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"())))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    file.modify(R"({"x": 99, "y": 88})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #keys passes, values irrelevant
        EXPECT_EQ(v.listSize(), 2);
    }
}

TEST_F(TracedDataTest, MapAttrs_HasKey_QueriedKeyRemoved)
{
    // mapAttrs #has:x, remove "x" → re-eval
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"(builtins.hasAttr "x" (builtins.mapAttrs (n: v: v) (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"())))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"y": 2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsFalse());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified — single-source filter (list)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, Filter_Length_ElementRemoved)
{
    // filter removes elements → shapeModified, #len suppressed, add element → re-eval
    TempJsonFile file(R"({"items": [1, 10, 2, 20]})");
    auto expr = R"(builtins.length (builtins.filter (x: x > 5) (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()).items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2)); // [10, 20]
    }

    file.modify(R"({"items": [1, 10, 2, 20, 30]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len suppressed → re-eval
        EXPECT_THAT(v, IsIntEq(3)); // [10, 20, 30]
    }
}

TEST_F(TracedDataTest, Filter_Length_AllKept)
{
    // filter keeps all → NOT shapeModified, #len recorded, add element → re-eval
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = R"(builtins.length (builtins.filter (x: true) (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()).items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [1, 2, 3, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len NOT suppressed, detects change
        EXPECT_THAT(v, IsIntEq(4));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified — single-source tail (list)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, Tail_Length_ElementAdded)
{
    // tail always shrinks → shapeModified, #len suppressed
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = R"(builtins.length (builtins.tail (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()).items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"items": [1, 2, 3, 4]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len suppressed → re-eval
        EXPECT_THAT(v, IsIntEq(3)); // tail of [1,2,3,4] = [2,3,4], len=3
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified — single-source sort (elements always preserved)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, Sort_Length_ElementAdded)
{
    // sort NOT shapeModified → #len recorded, add element → re-eval
    TempJsonFile file(R"({"items": [3, 1, 2]})");
    auto expr = R"(builtins.length (builtins.sort (a: b: a < b) (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()).items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [3, 1, 2, 0]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len NOT suppressed, detects change
        EXPECT_THAT(v, IsIntEq(4));
    }
}

TEST_F(TracedDataTest, Sort_Length_ValueChanged)
{
    // sort's comparator accesses element values, creating SC deps. When values
    // change, those SC deps fail even though #len passes. This is expected —
    // the sort result depends on element values through the comparator.
    TempJsonFile file(R"({"items": [3, 1, 2]})");
    auto expr = R"(builtins.length (builtins.sort (a: b: a < b) (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()).items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [9, 8, 7]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC deps from comparator fail → re-eval
        EXPECT_THAT(v, IsIntEq(3)); // still 3 elements
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified — multi-source // scalar access
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, MultiProv_Update_ScalarAccess_ChangeA)
{
    // (fj a // fj b).x where x from a; change unrelated VALUE in a → cache hit
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"z": 3})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + fileA.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileB.path.string()
        + R"()).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    fileA.modify(R"({"x": 1, "y": 99})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // SC(.x) passes
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(TracedDataTest, MultiProv_Update_ScalarAccess_ChangeB)
{
    // (fj a // fj b).x where x from a; change unrelated value in b → cache hit.
    // .x records scalar dep on a's origin. When b changes (value, not keys),
    // Content(b) fails. No SC dep covers b, but ImplicitShape #keys from
    // creation time provides fallback: b's keys unchanged → override succeeds.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"z": 3})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + fileA.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileB.path.string()
        + R"()).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    fileB.modify(R"({"z": 999})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // ImplicitShape fallback: b's keys unchanged
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(TracedDataTest, MultiProv_Update_ScalarAccess_ChangeX)
{
    // (fj a // fj b).x; change x's value in a → re-eval
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"z": 3})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + fileA.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileB.path.string()
        + R"()).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    fileA.modify(R"({"x": 42, "y": 2})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(42));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified — multi-source // shape observation
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, MultiProv_Update_AttrNames_KeyAdded)
{
    // attrNames (fj a // fj b), add key to b → re-eval
    // (shapeModified, #keys suppressed, Content fails)
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"() // builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    fileB.modify(R"({"y": 2, "z": 3})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3); // x, y, z
    }
}

TEST_F(TracedDataTest, MultiProv_Update_AttrNames_SameKeys_ValueChanged)
{
    // attrNames (fj a // fj b) where a and b have identical keys;
    // change a value → cache hit. In the merged result, all attrs come from
    // b (b wins for both x and y). attrNames records #keys on b's origin.
    // When a changes (value, not keys), Content(a) fails. No SC dep covers a,
    // but ImplicitShape #keys from creation time passes (a's keys unchanged)
    // → override succeeds.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "y": 20})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"() // builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    fileA.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // ImplicitShape fallback: a's keys unchanged
        EXPECT_EQ(v.listSize(), 2);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified — multi-source intersectAttrs
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, IntersectAttrs_SameKeys_ValueChanged)
{
    // attrNames(intersectAttrs a b) where both have same keys; change value → cache hit
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "y": 20})");
    auto expr = R"(builtins.attrNames (builtins.intersectAttrs (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"()) (builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"())))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    fileA.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // NOT shapeModified — sizes match — #keys passes
        EXPECT_EQ(v.listSize(), 2);
    }
}

TEST_F(TracedDataTest, IntersectAttrs_DisjointKeys_KeyAdded)
{
    // intersectAttrs with partially overlapping keys, add matching key → re-eval
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"x": 10, "z": 30})");
    auto expr = R"(builtins.attrNames (builtins.intersectAttrs (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"()) (builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"())))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 1); // only "x" overlaps
    }

    // Add "y" to fileB so intersection gains a key
    fileB.modify(R"({"x": 10, "y": 20, "z": 30})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 2); // x and y now overlap
    }
}

// ═══════════════════════════════════════════════════════════════════════
// shapeModified — multi-source ++ (list concat)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, Concat_EmptyPlusTracked_Length)
{
    // [] ++ (fj f).items; change value (not count) → cache hit
    // (#len NOT suppressed — sizes match — correctly passes)
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = R"(builtins.length ([] ++ (builtins.fromJSON (builtins.readFile )"
        + file.path.string() + R"()).items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    file.modify(R"({"items": [9, 8, 7]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // #len passes — same count
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(TracedDataTest, Concat_BothTracked_Length_ElementAdded)
{
    // length ((fj a).items ++ (fj b).items); add element to b → re-eval
    TempJsonFile fileA(R"({"items": [1, 2]})");
    TempJsonFile fileB(R"({"items": [3, 4]})");
    auto expr = R"(builtins.length ((builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"()).items ++ (builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"()).items))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(4));
    }

    fileB.modify(R"({"items": [3, 4, 5]})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #len suppressed → re-eval
        EXPECT_THAT(v, IsIntEq(5));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Chained // (three-way)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, ChainedUpdate_ScalarAccess_ChangeB)
{
    // (fj a // fj b // fj c).x where x from a; change unrelated value in b → cache hit.
    // .x records scalar dep on a's origin. When b changes (value, not keys),
    // Content(b) fails. No SC dep covers b, but ImplicitShape #keys from
    // creation time passes (b's keys unchanged) → override succeeds.
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    TempJsonFile fileC(R"({"z": 3})");
    auto expr = R"((builtins.fromJSON (builtins.readFile )" + fileA.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileB.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileC.path.string()
        + R"()).x)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    fileB.modify(R"({"y": 99})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // ImplicitShape fallback: b's keys unchanged
        EXPECT_THAT(v, IsIntEq(1));
    }
}

TEST_F(TracedDataTest, ChainedUpdate_AttrNames_KeyAdded)
{
    // attrNames (fj a // fj b // fj c), add key to c → re-eval
    TempJsonFile fileA(R"({"x": 1})");
    TempJsonFile fileB(R"({"y": 2})");
    TempJsonFile fileC(R"({"z": 3})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )" + fileA.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileB.path.string()
        + R"() // builtins.fromJSON (builtins.readFile )" + fileC.path.string()
        + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3);
    }

    fileC.modify(R"({"z": 3, "w": 4})");
    invalidateFileCache(fileC.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // shapeModified → #keys suppressed → re-eval
        EXPECT_EQ(v.listSize(), 4); // x, y, z, w
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Partial overlap: a // b where b shadows some of a's keys
// Tests the precision fix: SC #keys with visible subset is no longer
// recorded (would always fail verification). ImplicitShape handles it.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TracedDataTest, PartialOverlap_AttrNames_ValueChanged_CacheHit)
{
    // a.json: {x, y}, b.json: {y, z} → merged = {x: a, y: b, z: b}
    // attrNames sees x from a, y+z from b. a contributes 1 visible key (x),
    // but a.json has 2 keys → fast path misses (key count mismatch).
    // With the fix, slow path is removed → no SC #keys for a.
    // Change a.json values only → ImplicitShape passes → cache hit.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"y": 3, "z": 4})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"() // builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3); // x, y, z
    }

    fileA.modify(R"({"x": 999, "y": 888})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // ImplicitShape fallback: a's keys unchanged
        EXPECT_EQ(v.listSize(), 3);
    }
}

TEST_F(TracedDataTest, PartialOverlap_AttrNames_KeyAdded_ReEval)
{
    // Same setup: a={x,y}, b={y,z}. Add key "w" to a.json → re-eval.
    // ImplicitShape detects key change in a.json.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"y": 3, "z": 4})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"() // builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3); // x, y, z
    }

    fileA.modify(R"({"x": 1, "y": 2, "w": 99})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // ImplicitShape detects key change
        EXPECT_EQ(v.listSize(), 4); // w, x, y, z
    }
}

TEST_F(TracedDataTest, PartialOverlap_AttrNames_KeyRemoved_ReEval)
{
    // Same setup: a={x,y}, b={y,z}. Remove "y" from a.json → re-eval.
    // Even though "y" was shadowed by b, ImplicitShape detects key change.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"y": 3, "z": 4})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"() // builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3); // x, y, z
    }

    fileA.modify(R"({"x": 1})");
    invalidateFileCache(fileA.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // ImplicitShape detects key removal
        EXPECT_EQ(v.listSize(), 3); // x, y, z (y still from b, x from a)
    }
}

TEST_F(TracedDataTest, PartialOverlap_AttrNames_BFileChanged_CacheHit)
{
    // Same setup: a={x,y}, b={y,z}. Change b.json values only → cache hit.
    // b has 2 visible keys (y, z) and 2 total keys → fast path matches →
    // SC #keys recorded with precomputed hash. SC passes → cache hit.
    TempJsonFile fileA(R"({"x": 1, "y": 2})");
    TempJsonFile fileB(R"({"y": 3, "z": 4})");
    auto expr = R"(builtins.attrNames (builtins.fromJSON (builtins.readFile )"
        + fileA.path.string() + R"() // builtins.fromJSON (builtins.readFile )"
        + fileB.path.string() + R"()))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3); // x, y, z
    }

    fileB.modify(R"({"y": 999, "z": 888})");
    invalidateFileCache(fileB.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // SC #keys for b passes
        EXPECT_EQ(v.listSize(), 3);
    }
}

} // namespace nix::eval_trace
