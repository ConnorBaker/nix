#include <gtest/gtest.h>

#include "nix/fetchers/attrs.hh"

#include <nlohmann/json.hpp>

namespace nix::fetchers {

TEST(LazyAttr, resolveToInt)
{
    Attrs attrs;
    attrs.insert_or_assign(
        "count", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return uint64_t(42);
        }})));
    EXPECT_EQ(maybeGetIntAttr(attrs, "count"), 42);
}

TEST(LazyAttr, resolveToString)
{
    Attrs attrs;
    attrs.insert_or_assign(
        "name", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return std::string("hello");
        }})));
    EXPECT_EQ(maybeGetStrAttr(attrs, "name"), "hello");
}

TEST(LazyAttr, resolveToBool)
{
    Attrs attrs;
    attrs.insert_or_assign(
        "flag", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return Explicit<bool>{true};
        }})));
    EXPECT_EQ(maybeGetBoolAttr(attrs, "flag"), true);
}

TEST(LazyAttr, attrsToJSONForcesLazy)
{
    Attrs attrs;
    attrs.insert_or_assign(
        "x", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return uint64_t(99);
        }})));
    auto json = attrsToJSON(attrs);
    EXPECT_EQ(json["x"], 99);
}

TEST(LazyAttr, attrsToJSONForKeyDoesNotForce)
{
    int calls = 0;
    Attrs attrs;
    attrs.insert_or_assign("plain", std::string("hello"));
    attrs.insert_or_assign(
        "lazy", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = [&calls]() -> ResolvedAttr {
            calls++;
            return uint64_t(42);
        }})));
    auto json = attrsToJSONForKey(attrs);
    EXPECT_EQ(calls, 0);                 // never forced
    EXPECT_EQ(json["plain"], "hello");   // concrete preserved
    EXPECT_TRUE(json["lazy"].is_null()); // sentinel for unforced
}

TEST(LazyAttr, attrsToJSONForKeyEmitsNullSentinel)
{
    /* The R3 sentinel choice: `null`, not the string "<lazy>". This
       test pins the choice so a future refactor can't silently shift
       to a colliding placeholder. */
    Attrs attrs;
    attrs.insert_or_assign(
        "x", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return std::string("forced-value");
        }})));
    auto unforced = attrsToJSONForKey(attrs);
    auto forced = attrsToJSON(attrs);
    EXPECT_TRUE(unforced["x"].is_null());
    EXPECT_EQ(forced["x"], "forced-value");
    /* Cache-key invariant: unforced JSON is byte-distinct from
       forced JSON. A consumer cache hit on the unforced key cannot
       collide with a hit on the forced key for the same input. */
    EXPECT_NE(unforced.dump(), forced.dump());
}

TEST(LazyAttr, attrsToQueryForcesLazy)
{
    Attrs attrs;
    attrs.insert_or_assign(
        "v", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = []() -> ResolvedAttr {
            return std::string("val");
        }})));
    auto query = attrsToQuery(attrs);
    EXPECT_EQ(query.at("v"), "val");
}

TEST(LazyAttr, notCalledUntilForced)
{
    int calls = 0;
    Attrs attrs;
    attrs.insert_or_assign(
        "lazy", LazyAttr(make_ref<LazyAttrComputation>(LazyAttrComputation{.compute = [&calls]() -> ResolvedAttr {
            calls++;
            return uint64_t(1);
        }})));
    EXPECT_EQ(calls, 0);
    maybeGetIntAttr(attrs, "lazy");
    EXPECT_EQ(calls, 1);
}

} // namespace nix::fetchers
