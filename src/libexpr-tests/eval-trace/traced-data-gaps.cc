#include "helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/traced-data.hh"
#include "nix/expr/json-to-value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ═══════════════════════════════════════════════════════════════════════
// These tests verify that SC deps for files WITHOUT a corresponding
// Content/Directory dep in the same trace are correctly verified.
// This arises when a parent trace calls readFile+fromJSON (Content dep
// in parent), and a child trace forces thunks from the result (SC deps
// in child, but NO Content dep). The child's standalone SC deps must
// be checked even when no coarse dep fails in the child trace.

TEST_F(TracedDataTest, TracedJSON_StandaloneStructuralDep_CacheMiss)
{
    // Child trace has SC dep for .name but no Content dep for the file.
    // When .name changes, the standalone SC dep must fail → cache miss.
    TempJsonFile f(R"({"name":"foo","marker":"ok"})");

    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + f.path.string()
        + R"(); in builtins.seq j.marker { x = j.name; })";

    // Cold run: record root + child traces
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);
        EXPECT_THAT(*xAttr->value, IsStringEq("foo"));
    }

    // Modify: change .name, keep .marker unchanged
    f.modify(R"({"name":"changed","marker":"ok"})");
    invalidateFileCache(f.path);

    // Hot run: standalone SC dep for .name in child must fail
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);

        auto xStr = state.forceStringNoCtx(*xAttr->value, noPos, "");
        EXPECT_EQ(std::string(xStr), "changed");
    }
    EXPECT_EQ(loaderCalls, 1);
}

TEST_F(TracedDataTest, TracedJSON_StandaloneStructuralDep_CacheHit)
{
    // Child trace has SC dep for .name (standalone). When an unrelated
    // field changes, root's override accepts (marker SC passes) and
    // child's standalone SC dep for .name also passes → cache hit.
    TempJsonFile f(R"({"name":"foo","marker":"ok","extra":"bar"})");

    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + f.path.string()
        + R"(); in builtins.seq j.marker { x = j.name; })";

    // Cold run
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);
        EXPECT_THAT(*xAttr->value, IsStringEq("foo"));
    }

    // Modify: change unrelated field, keep .name and .marker
    f.modify(R"({"name":"foo","marker":"ok","extra":"changed"})");
    invalidateFileCache(f.path);

    // Hot run: everything should be served from cache
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);

        auto xStr = state.forceStringNoCtx(*xAttr->value, noPos, "");
        EXPECT_EQ(std::string(xStr), "foo");
    }
    EXPECT_EQ(loaderCalls, 0);
}

TEST_F(TracedDataTest, TracedJSON_StandaloneStructuralDep_MarkerChanges)
{
    // When the root's SC dep (.marker) fails, the root is re-evaluated
    // (override rejected). This changes the root's trace hash, so the
    // child's ParentContext dep fails → child also re-evaluated.
    // Confirms existing ParentContext cascade behavior.
    TempJsonFile f(R"({"name":"foo","marker":"ok"})");

    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + f.path.string()
        + R"(); in builtins.seq j.marker { x = j.name; })";

    // Cold run
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);
        EXPECT_THAT(*xAttr->value, IsStringEq("foo"));
    }

    // Modify: change .marker, keep .name
    f.modify(R"({"name":"foo","marker":"changed"})");
    invalidateFileCache(f.path);

    // Hot run: root's SC dep for .marker fails → root re-evaluated →
    // child's ParentContext dep fails → child re-evaluated
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);

        auto xStr = state.forceStringNoCtx(*xAttr->value, noPos, "");
        EXPECT_EQ(std::string(xStr), "foo");
    }
    EXPECT_EQ(loaderCalls, 1);
}

TEST_F(TracedDataTest, TracedTOML_StandaloneStructuralDep_CacheMiss)
{
    // Same as TracedJSON_StandaloneStructuralDep_CacheMiss but with TOML.
    // Confirms the fix is format-agnostic.
    TempTomlFile f("[data]\nname = \"foo\"\nmarker = \"ok\"\n");

    auto expr = R"(let t = builtins.fromTOML (builtins.readFile )" + f.path.string()
        + R"(); in builtins.seq t.data.marker { x = t.data.name; })";

    // Cold run
    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);
        EXPECT_THAT(*xAttr->value, IsStringEq("foo"));
    }

    // Modify: change name, keep marker
    f.modify("[data]\nname = \"changed\"\nmarker = \"ok\"\n");
    invalidateFileCache(f.path);

    // Hot run: standalone SC dep for data.name must fail
    int loaderCalls = 0;
    {
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * xAttr = root.attrs()->get(state.symbols.create("x"));
        ASSERT_NE(xAttr, nullptr);
        state.forceValue(*xAttr->value, noPos);

        auto xStr = state.forceStringNoCtx(*xAttr->value, noPos, "");
        EXPECT_EQ(std::string(xStr), "changed");
    }
    EXPECT_EQ(loaderCalls, 1);
}

// ═══════════════════════════════════════════════════════════════════════
// Adversarial soundness tests: gaps where shape dep recording is missing
// ═══════════════════════════════════════════════════════════════════════

// ── Gap 1 (FIXED): eqValues (== and !=) — shape deps added ──────────

TEST_F(TracedDataTest, TracedJSON_EqOp_ListLengthGrows)
{
    // [FIXED] == checks list length — #len shape dep now recorded at eval.cc:2964-2975.
    // Cold: arr has 2 elements, matches literal → true.
    // Hot: arr grows to 3 elements, but SC deps for [0] and [1] still pass.
    // Without #len dep, override incorrectly accepts stale "true".
    TempJsonFile file(R"({"arr":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.arr == ["a" "b"] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    // arr grows: ["a","b"] → ["a","b","c!!"] (different size for stat invalidation)
    file.modify(R"({"arr":["a","b","c!!"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (length changed)
        EXPECT_THAT(v, IsStringEq("no")); // 3-element list != 2-element literal
    }
}

TEST_F(TracedDataTest, TracedJSON_EqOp_AttrsetKeyAdded)
{
    // [FIXED] == checks attrset key count/names — #keys shape dep now recorded at eval.cc:2964-2975.
    // Cold: obj has {a,b}, matches literal → true.
    // Hot: obj gains key "c", but SC deps for .a and .b still pass.
    TempJsonFile file(R"({"obj":{"a":1,"b":2}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.obj == { a = 1; b = 2; } then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    file.modify(R"({"obj":{"a":1,"b":2,"c":3}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (key set changed)
        EXPECT_THAT(v, IsStringEq("no")); // extra key → not equal
    }
}

TEST_F(TracedDataTest, TracedJSON_NeqOp_ListLengthGrows)
{
    // [FIXED] != has the same fix as == (shares eqValues).
    // Cold: arr matches → != returns false → "no".
    // Hot: arr grows → should return true → "yes".
    TempJsonFile file(R"({"arr":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.arr != ["a" "b"] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("no")); // equal → != is false
    }

    file.modify(R"({"arr":["a","b","c!!"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate
        EXPECT_THAT(v, IsStringEq("yes")); // not equal → != is true
    }
}

TEST_F(TracedDataTest, TracedJSON_EqOp_ListElementChanges)
{
    // [PRECISION] Same length, element changes → SC dep for element fails → re-eval.
    TempJsonFile file(R"({"arr":["a","b"]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.arr == ["a" "b"] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    // Same length but element 1 changes (different size for stat invalidation)
    file.modify(R"({"arr":["a","B!!!"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for [1] fails → re-eval
        EXPECT_THAT(v, IsStringEq("no"));
    }
}

TEST_F(TracedDataTest, TracedJSON_EqOp_ListUnrelatedChange)
{
    // [PRECISION] arr unchanged, unrelated key changes → override correctly accepts.
    TempJsonFile file(R"({"arr":["a","b"],"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.arr == ["a" "b"] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    file.modify(R"({"arr":["a","b"],"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (arr unchanged)
        EXPECT_THAT(v, IsStringEq("yes"));
    }
}

// ── Gap 2 (FIXED): genericClosure — indirect #len via shared Value* ─

TEST_F(TracedDataTest, TracedJSON_GenericClosure_StartSetGrows)
{
    // [FIXED] genericClosure — indirect fix via shared Value* provenance (see design.md Gap 2).
    // Cold: 2 nodes → closure has 2 elements.
    // Hot: 3 nodes, existing SC deps for [0].key and [1].key pass, no #len dep.
    TempJsonFile file(R"({"nodes":[{"key":"a"},{"key":"b"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length (builtins.genericClosure { startSet = j.nodes; operator = n: []; }))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"nodes":[{"key":"a"},{"key":"b"},{"key":"c!!"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (startSet grew)
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(TracedDataTest, TracedJSON_GenericClosure_ElementChanges)
{
    // [PRECISION] Element value changes → SC dep for that element fails → re-eval.
    TempJsonFile file(R"({"nodes":[{"key":"a"},{"key":"b"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.head (builtins.genericClosure { startSet = j.nodes; operator = n: []; }))";

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * keyAttr = root.attrs()->get(state.symbols.create("key"));
        ASSERT_NE(keyAttr, nullptr);
        state.forceValue(*keyAttr->value, noPos);
        EXPECT_THAT(*keyAttr->value, IsStringEq("a"));
    }

    file.modify(R"({"nodes":[{"key":"CHANGED!"},{"key":"b"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * keyAttr = root.attrs()->get(state.symbols.create("key"));
        ASSERT_NE(keyAttr, nullptr);
        state.forceValue(*keyAttr->value, noPos);
        EXPECT_EQ(loaderCalls, 1); // SC dep for [0].key fails
        EXPECT_THAT(*keyAttr->value, IsStringEq("CHANGED!"));
    }
}

TEST_F(TracedDataTest, TracedJSON_GenericClosure_CacheHit)
{
    // [PRECISION] Unaccessed non-key attribute changes → cache hit.
    // genericClosure reads ALL elements' .key for dedup, so we must change
    // a non-.key attribute to avoid invalidation.
    TempJsonFile file(R"({"nodes":[{"key":"a","val":"x"},{"key":"b","val":"y"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.head (builtins.genericClosure { startSet = j.nodes; operator = n: []; }))";

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        auto * keyAttr = root.attrs()->get(state.symbols.create("key"));
        ASSERT_NE(keyAttr, nullptr);
        state.forceValue(*keyAttr->value, noPos);
        EXPECT_THAT(*keyAttr->value, IsStringEq("a"));
    }

    // Change element [1]'s non-key attribute only (unaccessed by result)
    file.modify(R"({"nodes":[{"key":"a","val":"x"},{"key":"b","val":"CHANGED!"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto root = forceRoot(*cache);
        auto * keyAttr = root.attrs()->get(state.symbols.create("key"));
        ASSERT_NE(keyAttr, nullptr);
        state.forceValue(*keyAttr->value, noPos);
        EXPECT_EQ(loaderCalls, 0); // cache hit ([0].key and [1].key unchanged)
        EXPECT_THAT(*keyAttr->value, IsStringEq("a"));
    }
}

// ── Gap 3 (FIXED): listToAttrs — #len on input list ─────────────────

TEST_F(TracedDataTest, TracedJSON_ListToAttrs_FullResultGrows)
{
    // [FIXED] listToAttrs — #len shape dep now recorded at primops.cc:3543.
    // Cold: 2 items → 2 attrs. Hot: 3 items, existing SC deps pass.
    TempJsonFile file(R"({"items":[{"name":"a","value":"1"},{"name":"b","value":"2"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.length (builtins.attrNames (builtins.listToAttrs j.items)))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"items":[{"name":"a","value":"1"},{"name":"b","value":"2"},{"name":"c","value":"3!!"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (list grew)
        EXPECT_THAT(v, IsIntEq(3));
    }
}

TEST_F(TracedDataTest, TracedJSON_ListToAttrs_ElementChanges)
{
    // [PRECISION] Same list length, element value changes → SC dep fails → re-eval.
    TempJsonFile file(R"({"items":[{"name":"a","value":"1"},{"name":"b","value":"2"}]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.listToAttrs j.items).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("1"));
    }

    // Change value of "a" item, keep list length the same
    file.modify(R"({"items":[{"name":"a","value":"99"},{"name":"b","value":"2"}]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for .value fails
        EXPECT_THAT(v, IsStringEq("99"));
    }
}

TEST_F(TracedDataTest, TracedJSON_ListToAttrs_UnrelatedChange)
{
    // [PRECISION] Unrelated field changes → SC override accepts → cache hit.
    TempJsonFile file(R"({"items":[{"name":"a","value":"1"},{"name":"b","value":"2"}],"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.listToAttrs j.items).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("1"));
    }

    // Only change unrelated field
    file.modify(R"({"items":[{"name":"a","value":"1"},{"name":"b","value":"2"}],"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (items unchanged)
        EXPECT_THAT(v, IsStringEq("1"));
    }
}

// ── Gap 4: Positive tests for raw + parsed readFile scenarios ────────

TEST_F(TracedDataTest, RawOnly_StringLength_ContentChanges)
{
    // Raw-only readFile correctly invalidates when content changes.
    // No SC deps at all — Content dep failure forces re-evaluation.
    TempJsonFile file(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let raw = builtins.readFile )" + file.path.string()
        + R"(; in builtins.stringLength raw)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(30));
    }

    file.modify(R"({"name":"foo","extra":"longer!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Content dep fails, no SC deps → must re-eval
        EXPECT_THAT(v, IsIntEq(33));
    }
}

TEST_F(TracedDataTest, RawOnly_Substring_ContentChanges)
{
    // Raw substring invalidates when content changes.
    // No SC deps — Content dep failure forces re-evaluation.
    TempJsonFile file(R"({"name":"foo"})");
    auto expr = R"(let raw = builtins.readFile )" + file.path.string()
        + R"(; in builtins.substring 0 10 raw)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq(R"({"name":"f)"));
    }

    file.modify(R"({"xame":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Content dep fails, no SC override
        EXPECT_THAT(v, IsStringEq(R"({"xame":"f)"));
    }
}

TEST_F(TracedDataTest, RawOnly_StringLength_SameSizeChange)
{
    // Raw invalidates even when file size is unchanged.
    // Tests that we hash content, not just stat metadata.
    TempJsonFile file(R"({"a":"xxx"})");
    auto expr = R"(let raw = builtins.readFile )" + file.path.string()
        + R"(; in builtins.stringLength raw)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(11));
    }

    file.modify(R"({"a":"yyy"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Content hash changes even though size is same
        EXPECT_THAT(v, IsIntEq(11));
    }
}

TEST_F(TracedDataTest, ParsedOnly_UnusedFieldChange_CacheHit)
{
    // Parsed-only path allows SC override when only unused fields change.
    // Adjacent to Gap 4 to make the contrast with raw+parsed explicit.
    TempJsonFile file(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("foo"));
    }

    file.modify(R"({"name":"foo","extra":"longer!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // Content fails, SC dep on .name passes → override
        EXPECT_THAT(v, IsStringEq("foo"));
    }
}

TEST_F(TracedDataTest, RawAndParsed_DifferentFiles_RawFileChanges)
{
    // Raw + parsed from different files: SC deps from parsed file cannot cover
    // raw file's Content dep failure.
    TempTextFile file1("hello world");
    TempJsonFile file2(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let raw = builtins.readFile )" + file1.path.string()
        + R"(; j = builtins.fromJSON (builtins.readFile )" + file2.path.string()
        + R"(); in toString (builtins.stringLength raw) + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_EQ(std::string(str), "11-foo");
    }

    // Only modify the raw file
    file1.modify("hello world!!");
    invalidateFileCache(file1.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // file1 Content fails, no SC deps for file1 → re-eval
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_EQ(std::string(str), "13-foo");
    }
}

TEST_F(TracedDataTest, RawAndParsed_DifferentFiles_ParsedUnusedChange)
{
    // Raw + parsed from different files: SC override applies only to parsed file.
    // Raw file unchanged → its Content dep passes. Parsed file's unused field
    // changes → Content fails but SC dep on .name passes → override.
    TempTextFile file1("hello world");
    TempJsonFile file2(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let raw = builtins.readFile )" + file1.path.string()
        + R"(; j = builtins.fromJSON (builtins.readFile )" + file2.path.string()
        + R"(); in toString (builtins.stringLength raw) + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_EQ(std::string(str), "11-foo");
    }

    // Only modify parsed file's unused field
    file2.modify(R"({"name":"foo","extra":"longer!!"})");
    invalidateFileCache(file2.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // file1 passes, file2 SC override → cache hit
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_EQ(std::string(str), "11-foo");
    }
}

TEST_F(TracedDataTest, RawAndParsed_SameFile_AccessedFieldChanges)
{
    // Same file, raw + parsed: correctly re-evals when SC dep also fails.
    // Content dep fails AND SC dep on .name fails → override rejected → re-eval.
    // Contrasts with the DISABLED Gap 4 test where only an unused field changes.
    TempJsonFile file(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let raw = builtins.readFile )" + file.path.string()
        + R"(; j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in toString (builtins.stringLength raw) + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_TRUE(std::string(str).find("-foo") != std::string::npos);
    }

    // Change the accessed field (.name)
    file.modify(R"({"name":"bar","extra":"short"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Content fails, SC dep on .name ALSO fails → re-eval
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_TRUE(std::string(str).find("-bar") != std::string::npos);
    }
}

// ── Gap 4: Raw + parsed readFile from same file (KNOWN BUG) ─────────

TEST_F(TracedDataTest, DISABLED_TracedJSON_RawAndParsedReadFile_ContentChanges)
{
    // [SOUNDNESS] DEFERRED: Raw + parsed readFile from same file requires deeper
    // design work. When both a raw readFile (used via stringLength) and a parsed
    // readFile (used via fromJSON) reference the same file, SC deps from the parsed
    // path can incorrectly "cover" the Content dep failure for the raw path.
    // This needs a mechanism to distinguish raw vs parsed readFile provenance.
    TempJsonFile file(R"({"name":"foo","extra":"short"})");
    auto expr = R"(let raw = builtins.readFile )" + file.path.string()
        + R"(; j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in toString (builtins.stringLength raw) + "-" + j.name)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        // File is 30 bytes: {"name":"foo","extra":"short"}
        auto str = state.forceStringNoCtx(v, noPos, "");
        // Just verify it has the right format (number-name)
        EXPECT_TRUE(std::string(str).find("-foo") != std::string::npos);
    }

    // Change extra (name stays same, file size changes)
    file.modify(R"({"name":"foo","extra":"longer!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // raw stringLength changed → must re-evaluate
        auto str = state.forceStringNoCtx(v, noPos, "");
        EXPECT_TRUE(std::string(str).find("-foo") != std::string::npos);
    }
}

// ── Gap 5 (FIXED): intersectAttrs — #keys on inputs ────────────────

TEST_F(TracedDataTest, TracedJSON_IntersectAttrs_TracedGainsMatchingKey)
{
    // [FIXED] intersectAttrs — #keys shape dep now recorded at primops.cc:3627-3628.
    // A passing SC dep (marker) triggers the override, but key set changes
    // in intersectAttrs inputs go undetected.
    TempJsonFile file(R"({"a":1,"b":2,"marker":"ok"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in builtins.seq j.marker (builtins.length (builtins.attrNames (builtins.intersectAttrs j { a = 1; b = 2; c = 3; }))))";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2)); // intersection of {a,b,marker} ∩ {a,b,c} = {a,b}
    }

    // j gains "c" which is in the literal set → intersection grows
    file.modify(R"({"a":1,"b":2,"c":99,"marker":"ok"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (intersection changed)
        EXPECT_THAT(v, IsIntEq(3)); // now {a,b,c}
    }
}

TEST_F(TracedDataTest, TracedJSON_IntersectAttrs_ValueChanges)
{
    // [PRECISION] Same keys, accessed value changes → SC dep fails → re-eval.
    // intersectAttrs takes values from the second argument, so we put traced
    // data as the second arg and access a value from the result.
    TempJsonFile file(R"({"a":1,"b":2})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.intersectAttrs { a = true; } j).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1)); // intersect {a} ∩ {a,b} → {a: j.a} → 1
    }

    // Change value of "a" but keep same keys
    file.modify(R"({"a":99,"b":2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for .a fails
        EXPECT_THAT(v, IsIntEq(99));
    }
}

TEST_F(TracedDataTest, TracedJSON_IntersectAttrs_UnrelatedChange)
{
    // [PRECISION] Unrelated field changes → SC override accepts → cache hit.
    TempJsonFile file(R"({"a":1,"b":2,"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (builtins.intersectAttrs { a = true; } j).a)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Only change unrelated "extra" field
    file.modify(R"({"a":1,"b":2,"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (a unchanged)
        EXPECT_THAT(v, IsIntEq(1));
    }
}

// ── Additional precision tests for Gap 1 ────────────────────────────

TEST_F(TracedDataTest, TracedJSON_EqOp_AttrsetValueChanges)
{
    // [PRECISION] Same keys, value changes → SC dep for value fails → re-eval.
    TempJsonFile file(R"({"obj":{"a":1,"b":2}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.obj == { a = 1; b = 2; } then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    file.modify(R"({"obj":{"a":1,"b":99}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for .b fails
        EXPECT_THAT(v, IsStringEq("no"));
    }
}

TEST_F(TracedDataTest, TracedJSON_EqOp_AttrsetUnrelatedChange)
{
    // [PRECISION] obj unchanged, unrelated key changes → override correctly accepts.
    TempJsonFile file(R"({"obj":{"a":1,"b":2},"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if j.obj == { a = 1; b = 2; } then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    file.modify(R"({"obj":{"a":1,"b":2},"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (obj unchanged)
        EXPECT_THAT(v, IsStringEq("yes"));
    }
}

// ── Gap B1 (FIXED): CompareValues (<) — #len on lists ───────────────

TEST_F(TracedDataTest, TracedJSON_LessThan_ListLengthGrows)
{
    // [FIXED] Lexicographic list comparison — #len shape dep now recorded at primops.cc:763-764.
    // Cold: [1,2] < [1,2,3] → true (shorter list is "less").
    // Hot: first list grows to [1,2,9], SC deps for [0] and [1] still pass.
    // Without #len dep, override incorrectly accepts stale "true".
    TempJsonFile file(R"({"arr":[1,2]})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if builtins.lessThan j.arr [1 2 3] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes")); // [1,2] < [1,2,3]
    }

    // arr grows: [1,2] → [1,2,9] (different size for stat invalidation)
    file.modify(R"({"arr":[1,2,9]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (length changed)
        EXPECT_THAT(v, IsStringEq("no")); // [1,2,9] < [1,2,3] is false
    }
}

TEST_F(TracedDataTest, TracedJSON_LessThan_ListUnrelatedChange)
{
    // [PRECISION] Same lengths, unrelated element changes → SC dep for element fails → re-eval.
    TempJsonFile file(R"({"arr":[1,2],"extra":"short"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in if builtins.lessThan j.arr [1 2 3] then "yes" else "no")";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("yes"));
    }

    // Only extra changes, arr stays same
    file.modify(R"({"arr":[1,2],"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (arr unchanged)
        EXPECT_THAT(v, IsStringEq("yes"));
    }
}

// ── Gap B2 (FIXED): ExprOpUpdate (//) — #keys on inputs ─────────────

TEST_F(TracedDataTest, TracedJSON_Update_TracedGainsKey)
{
    // [FIXED] // merges attrsets — #keys shape dep now recorded at eval.cc:1981-1982.
    // Cold: base has {a:1}, overlay has {b:2} → result {a:1,b:2}.
    // Hot: base gains key "b" with value 99, but SC dep for .a still passes.
    // Without #keys dep, override incorrectly accepts stale {a:1,b:2}.
    TempJsonFile file(R"({"base":{"a":1},"over":{"b":2}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (j.base // j.over).b)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2)); // overlay's b wins
    }

    // base gains "b":99 — now base // over should still give over's b=2,
    // but the key set of base changed
    file.modify(R"({"base":{"a":1,"b":99},"over":{"b":2}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (base key set changed)
        EXPECT_THAT(v, IsIntEq(2)); // overlay still wins
    }
}

TEST_F(TracedDataTest, TracedJSON_Update_UnrelatedChange)
{
    // [PRECISION] base/over unchanged, unrelated key changes → cache hit.
    TempJsonFile file(R"({"base":{"a":1},"over":{"b":2},"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); in (j.base // j.over).b)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    file.modify(R"({"base":{"a":1},"over":{"b":2},"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit
        EXPECT_THAT(v, IsIntEq(2));
    }
}

// ── Gap B3 (FIXED): callFunction strict formals — #keys ─────────────

TEST_F(TracedDataTest, TracedJSON_StrictFormals_KeyAdded)
{
    // [FIXED] Strict formals check (no ...) — #keys shape dep now recorded at eval.cc:1648.
    // without recording #keys. Cold: {a:1} passed to ({a}: a) → 1.
    // Hot: {a:1,b:2} — should throw "unexpected argument 'b'" but SC dep
    // for .a still passes. Without #keys dep, override accepts stale result.
    TempJsonFile file(R"({"obj":{"a":1}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); f = {a}: a; in f j.obj)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // obj gains key "b" — strict formals should reject
    file.modify(R"({"obj":{"a":1,"b":2}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        EXPECT_THROW(forceRoot(*cache), nix::Error); // unexpected argument 'b'
        EXPECT_EQ(loaderCalls, 1); // must re-evaluate (key set changed)
    }
}

TEST_F(TracedDataTest, TracedJSON_StrictFormals_ValueChanges)
{
    // [PRECISION] Same keys, value changes → SC dep for value fails → re-eval.
    TempJsonFile file(R"({"obj":{"a":1}})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); f = {a}: a; in f j.obj)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Change value of "a" but keep same key set
    file.modify(R"({"obj":{"a":99}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // SC dep for .a fails
        EXPECT_THAT(v, IsIntEq(99));
    }
}

TEST_F(TracedDataTest, TracedJSON_StrictFormals_UnrelatedChange)
{
    // [PRECISION] Unrelated field changes → SC override accepts → cache hit.
    TempJsonFile file(R"({"obj":{"a":1},"extra":"x"})");
    auto expr = R"(let j = builtins.fromJSON (builtins.readFile )" + file.path.string()
        + R"(); f = {a}: a; in f j.obj)";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    // Only change unrelated "extra" field
    file.modify(R"({"obj":{"a":1},"extra":"changed!!"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // cache hit (obj.a unchanged)
        EXPECT_THAT(v, IsIntEq(1));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Provenance propagation tests
// ═══════════════════════════════════════════════════════════════════════
//
// These tests verify that container-reconstructing operations (mapAttrs,
// filter, sort, removeAttrs, etc.) propagate provenance from tracked
// inputs to new output containers, allowing shape deps to be recorded
// on derived containers.

// ── mapAttrs: #keys dep recorded on derived attrset ──────────────────
// This is the core "mapAttrs gap" scenario from the design doc.
// Without propagation, attrNames on mapAttrs output fails to record
// a #keys dep because the output Bindings* is not in the provenance map.

TEST_F(TracedDataTest, Propagation_MapAttrs_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            mapped = builtins.mapAttrs (k: v: v + 1) data;
        in builtins.attrNames mapped
    )";

    // Fresh evaluation: attrNames on mapAttrs output
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    // File unchanged → serve from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Key added to JSON → must re-evaluate (different-size content triggers stat change)
    file.modify(R"({"a": 1, "b": 2, "c": 3333})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Must re-evaluate: key set changed
        EXPECT_EQ(v.listSize(), 3);
    }
}

// ── mapAttrs: value change in unused key doesn't invalidate ──────────
// When mapAttrs derives from tracked JSON, changing a value that the
// trace doesn't depend on should still allow two-level override.

TEST_F(TracedDataTest, Propagation_MapAttrs_UnusedValueChange)
{
    TempJsonFile file(R"({"used": 10, "other": 20})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            mapped = builtins.mapAttrs (k: v: v + 1) data;
        in mapped.used
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(11));
    }

    // Change "other" value (different size to trigger stat change)
    file.modify(R"({"used": 10, "other": 99999})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0); // Two-level override: only .used accessed
        EXPECT_THAT(v, IsIntEq(11));
    }
}

// ── removeAttrs: provenance propagated to subset ─────────────────────

TEST_F(TracedDataTest, Propagation_RemoveAttrs_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            subset = builtins.removeAttrs data ["c"];
        in builtins.attrNames subset
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    // File unchanged → serve from cache
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Key added → must re-evaluate (provenance propagation catches shape change)
    file.modify(R"({"a": 1, "b": 2, "c": 3, "d": 44444})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Must re-evaluate
    }
}

// ── intersectAttrs: provenance propagated from tracked input ─────────

TEST_F(TracedDataTest, Propagation_IntersectAttrs_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            result = builtins.intersectAttrs { a = true; b = true; } data;
        in builtins.attrNames result
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── filter: provenance propagated to filtered list ───────────────────

TEST_F(TracedDataTest, Propagation_Filter_LenTracked)
{
    TempJsonFile file(R"({"items": [1, 2, 3, 4, 5]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            filtered = builtins.filter (x: x > 2) data.items;
        in builtins.length filtered
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── sort: provenance propagated (same length) ────────────────────────

TEST_F(TracedDataTest, Propagation_Sort_LenTracked)
{
    TempJsonFile file(R"({"items": [3, 1, 2]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            sorted = builtins.sort builtins.lessThan data.items;
        in builtins.length sorted
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Array grows → must re-evaluate (length changed)
    file.modify(R"({"items": [3, 1, 2, 4444]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(4));
    }
}

// ── ExprOpUpdate (//): provenance propagated from tracked input ──────

TEST_F(TracedDataTest, Propagation_OpUpdate_KeysTracked)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            merged = data // { c = 3; };
        in builtins.attrNames merged
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Key added to JSON → must re-evaluate
    file.modify(R"({"a": 1, "b": 2, "d": 44444})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 4); // a, b, c, d
    }
}

// ── partition: inner lists propagated ────────────────────────────────

TEST_F(TracedDataTest, Propagation_Partition_InnerListsTracked)
{
    TempJsonFile file(R"({"items": [1, 2, 3, 4, 5]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            parts = builtins.partition (x: x > 2) data.items;
        in builtins.length parts.right
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── groupBy: inner group lists propagated ────────────────────────────

TEST_F(TracedDataTest, Propagation_GroupBy_InnerListsTracked)
{
    TempJsonFile file(R"({"items": [1, 2, 3, 4]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            groups = builtins.groupBy (x: if x > 2 then "big" else "small") data.items;
        in builtins.length groups.big
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(2));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── Negative: non-tracked container → no provenance ──────────────────
// Operations on containers NOT from ExprTracedData should NOT
// produce spurious shape deps (provenance map lookup returns null).

TEST_F(TracedDataTest, Propagation_Negative_NonTrackedContainer)
{
    // This expression builds a list literal (not from JSON),
    // then sorts it and checks length. No provenance should be propagated.
    auto expr = R"(
        let items = [3 1 2];
            sorted = builtins.sort builtins.lessThan items;
        in builtins.length sorted
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }

    // Should serve from cache (no file deps to invalidate)
    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Fragile area tests: edge cases in provenance propagation
// ═══════════════════════════════════════════════════════════════════════

// ── Chained reconstruction: sort(filter(pred, tracked)) ──────────────
// Provenance must survive multiple reconstruction steps.

TEST_F(TracedDataTest, Propagation_Chained_SortFilter)
{
    TempJsonFile file(R"({"items": [5, 3, 1, 4, 2]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            filtered = builtins.filter (x: x > 2) data.items;
            sorted = builtins.sort builtins.lessThan filtered;
        in builtins.length sorted
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3)); // [3, 4, 5]
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Array grows → must re-evaluate
    file.modify(R"({"items": [5, 3, 1, 4, 2, 6666]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsIntEq(4)); // [3, 4, 5, 6666]
    }
}

// ── Empty output from filter → propagation is no-op ──────────────────
// When filter produces an empty list, propagateTrackedList should be
// a no-op (empty output has no stable key). This must not crash.

TEST_F(TracedDataTest, Propagation_Filter_EmptyOutput)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            filtered = builtins.filter (x: x > 100) data.items;
        in builtins.length filtered
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(0));
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── ++ with one tracked input among many ─────────────────────────────
// concatLists should propagate from whichever input is tracked.

TEST_F(TracedDataTest, Propagation_ConcatLists_OneTracked)
{
    TempJsonFile file(R"({"items": [10, 20]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            combined = [1 2] ++ data.items ++ [30];
        in builtins.length combined
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(5)); // [1, 2, 10, 20, 30]
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── mapAttrs then hasAttr on derived container ───────────────────────
// hasAttr records #keys dep on the attrset. After propagation,
// this should work on mapAttrs output.

TEST_F(TracedDataTest, Propagation_MapAttrs_HasAttr)
{
    TempJsonFile file(R"({"a": 1, "b": 2})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            mapped = builtins.mapAttrs (k: v: v + 1) data;
        in mapped ? a
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.boolean(), true);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }

    // Remove key "a" from JSON
    file.modify(R"({"b": 2, "c": 33333})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // Must re-evaluate: key "a" removed
        EXPECT_EQ(v.boolean(), false);
    }
}

// ── Negative: removeAttrs on non-tracked doesn't crash ───────────────

TEST_F(TracedDataTest, Propagation_Negative_RemoveAttrsNonTracked)
{
    auto expr = R"(
        let data = { a = 1; b = 2; c = 3; };
            subset = builtins.removeAttrs data ["c"];
        in builtins.attrNames subset
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── tail of tracked list → propagation handles size-1 input ──────────

TEST_F(TracedDataTest, Propagation_Tail_SingleElement)
{
    TempJsonFile file(R"({"items": [42]})");
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
        in builtins.length (builtins.tail data.items)
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(0)); // tail of [42] = []
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── // with tracked LHS and literal RHS ──────────────────────────────
// The layer optimization path vs merge path in ExprOpUpdate should
// both propagate correctly.

TEST_F(TracedDataTest, Propagation_OpUpdate_LayerPath)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    // Small RHS triggers the layering optimization path
    auto expr = R"(
        let data = builtins.fromJSON (builtins.readFile )" + file.path.string() + R"();
            merged = data // { z = 3; };
        in builtins.attrNames merged
    )";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 3); // x, y, z
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
    }
}

} // namespace nix::eval_trace
