#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

#include "nix/expr/dependency-tracker.hh"
#include "nix/expr/eval-trace-deps.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionTypeOfTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Dep verification: typeOf records #type
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionTypeOfTest, TypeOf_List_RecordsSCType)
{
    TempJsonFile file(R"({"data": [1, 2, 3]})");
    auto expr = std::format("builtins.typeOf ({}).data", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "typeOf must record SC #type\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "typeOf must NOT record SC #keys\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("len")))
        << "typeOf must NOT record SC #len\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTypeOfTest, TypeOf_Attrset_RecordsSCType)
{
    TempJsonFile file(R"({"data": {"a": 1}})");
    auto expr = std::format("builtins.typeOf ({}).data", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "typeOf on attrset must record SC #type\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTypeOfTest, TypeOf_Scalar_NoSCType)
{
    // typeOf on a scalar (int/string/bool) does NOT record #type because
    // scalars don't have TracedData provenance at the scalar level.
    TempJsonFile file(R"({"x": 42})");
    auto expr = std::format("builtins.typeOf ({}).x", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    // Scalar typeOf: the value is already forced, typeOf is trivial.
    // No #type dep recorded for scalars.
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "typeOf on scalar must NOT record SC #type\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTypeOfTest, IsAttrs_RecordsSCType)
{
    TempJsonFile file(R"({"data": {"a": 1}})");
    auto expr = std::format("builtins.isAttrs ({}).data", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "isAttrs must record SC #type\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTypeOfTest, IsList_RecordsSCType)
{
    TempJsonFile file(R"({"data": [1, 2]})");
    auto expr = std::format("builtins.isList ({}).data", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "isList must record SC #type\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTypeOfTest, IsString_NoTypeDep)
{
    // isString on a scalar doesn't go through TracedData container path.
    TempJsonFile file(R"({"x": "hello"})");
    auto expr = std::format("builtins.isString ({}).x", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "isString on scalar must NOT record SC #type\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTypeOfTest, IsInt_NoTypeDep)
{
    TempJsonFile file(R"({"x": 42})");
    auto expr = std::format("builtins.isInt ({}).x", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "isInt on scalar must NOT record SC #type\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionTypeOfTest, IsBool_NoTypeDep)
{
    TempJsonFile file(R"({"x": true})");
    auto expr = std::format("builtins.isBool ({}).x", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "isBool on scalar must NOT record SC #type\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Cache behavior: type changes (moved from builtins-access.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionTypeOfTest, TypeChange_ArrayToObject_CacheMiss)
{
    TempJsonFile file(R"({"data":[1,2,3],"name":"foo"})");
    auto expr = std::format("let j = {}; in builtins.typeOf j.data + \"-\" + j.name", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
            << "typeOf records SC #type\n" << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    file.modify(R"({"data":{"a":1},"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }
}

TEST_F(DepPrecisionTypeOfTest, TypeChange_ObjectToArray_CacheMiss)
{
    TempJsonFile file(R"({"data":{"a":1},"name":"foo"})");
    auto expr = std::format("let j = {}; in builtins.typeOf j.data + \"-\" + j.name", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }

    file.modify(R"({"data":[1,2,3],"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }
}

TEST_F(DepPrecisionTypeOfTest, TypeUnchanged_CacheHit)
{
    TempJsonFile file(R"({"data":[1,2],"name":"foo"})");
    auto expr = std::format("let j = {}; in builtins.typeOf j.data + \"-\" + j.name", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    // Array grows but is still an array — #type passes. name unchanged.
    file.modify(R"({"data":[1,2,3],"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Cache behavior: isAttrs/isList type changes (moved from builtins-misc.cc)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionTypeOfTest, IsAttrs_ObjectToArray_CacheMiss)
{
    TempJsonFile file(R"({"data":{"a":1},"name":"foo"})");
    auto expr = std::format(
        "let j = {}; in (if builtins.isAttrs j.data then \"set\" else \"other\") + \"-\" + j.name",
        fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }

    file.modify(R"({"data":[1,2,3],"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #type dep fails
        EXPECT_THAT(v, IsStringEq("other-foo"));
    }
}

TEST_F(DepPrecisionTypeOfTest, IsList_ArrayToObject_CacheMiss)
{
    TempJsonFile file(R"({"data":[1,2],"name":"foo"})");
    auto expr = std::format(
        "let j = {}; in (if builtins.isList j.data then \"list\" else \"other\") + \"-\" + j.name",
        fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    file.modify(R"({"data":{"a":1},"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1); // #type dep fails
        EXPECT_THAT(v, IsStringEq("other-foo"));
    }
}

TEST_F(DepPrecisionTypeOfTest, NestedTypeChange_CacheMiss)
{
    TempJsonFile file(R"({"data":{"inner":[1,2]},"name":"foo"})");
    auto expr = std::format("let j = {}; in builtins.typeOf j.data.inner + \"-\" + j.name", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("list-foo"));
    }

    file.modify(R"({"data":{"inner":{"a":1}},"name":"foo"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("set-foo"));
    }
}

} // namespace nix::eval_trace
