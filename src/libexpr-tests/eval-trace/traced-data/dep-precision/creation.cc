#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

#include <format>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionCreationTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// JSON object creation-time deps
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionCreationTest, Object_ImplicitShapeKeys_Present)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format("({}).x", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("keys")))
        << "Object creation must record ImplicitShape #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, Object_NoImplicitShapeLen)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = std::format("({}).x", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_FALSE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("len")))
        << "Object creation must NOT record ImplicitShape #len\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, Object_EmptyObject_ImplicitShapeKeys)
{
    TempJsonFile file(R"({"obj": {}})");
    auto expr = std::format("builtins.attrNames ({}).obj", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("keys")))
        << "Empty object must still get ImplicitShape #keys\n" << dumpDeps(deps);
    EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "attrNames on empty object records SC #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, Object_EmptyObject_BlockingSCKeys_CacheBehavior)
{
    TempJsonFile file(R"({"obj": {}})");
    auto expr = std::format("builtins.attrNames ({}).obj", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 0u);
    }

    file.modify(R"({"obj": {"a": 1}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "Key added to empty object must invalidate via #keys";
        EXPECT_EQ(v.listSize(), 1u);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// JSON array creation-time deps
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionCreationTest, Array_ImplicitShapeLen_Present)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format("builtins.elemAt ({}).items 0", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("len")))
        << "Array creation must record ImplicitShape #len\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, Array_NoImplicitShapeKeys)
{
    TempJsonFile file(R"({"items": [1, 2, 3]})");
    auto expr = std::format("builtins.elemAt ({}).items 0", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    // The parent object DOES get IS #keys; the array gets IS #len.
    EXPECT_TRUE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("len")))
        << "Array should have IS #len\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, RootArray_ImplicitShapeLen)
{
    TempJsonFile file(R"([1, 2, 3])");
    auto expr = std::format("builtins.elemAt ({}) 0", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("len")))
        << "Root array creation must record ImplicitShape #len\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Scalar access deps
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionCreationTest, Scalar_Int_RecordsSCDep)
{
    TempJsonFile file(R"({"x": 42})");
    auto expr = std::format("({}).x", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "x"))
        << "Scalar int access must record SC dep\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
        << "Scalar access alone must NOT record SC #keys\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("len")))
        << "Scalar access must NOT record SC #len\n" << dumpDeps(deps);
    EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("type")))
        << "Scalar access must NOT record SC #type\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, Scalar_String_RecordsSCDep)
{
    TempJsonFile file(R"({"name": "hello"})");
    auto expr = std::format("({}).name", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "name"))
        << "Scalar string access must record SC dep\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, Scalar_Bool_RecordsSCDep)
{
    TempJsonFile file(R"({"enabled": true})");
    auto expr = std::format("({}).enabled", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "enabled"))
        << "Scalar bool access must record SC dep\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, Scalar_Null_RecordsSCDep)
{
    TempJsonFile file(R"({"data": null})");
    auto expr = std::format("({}).data", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "data"))
        << "Scalar null access must record SC dep\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, Scalar_Float_RecordsSCDep)
{
    TempJsonFile file(R"({"pi": 3.14})");
    auto expr = std::format("({}).pi", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "pi"))
        << "Scalar float access must record SC dep\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, Scalar_NestedAccess_RecordsSCDep)
{
    TempJsonFile file(R"({"inner": {"value": 99}})");
    auto expr = std::format("({}).inner.value", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "value"))
        << "Nested scalar access must record SC dep for leaf\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// TOML creation-time deps
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionCreationTest, TOML_Object_ImplicitShapeKeys)
{
    TempTomlFile file("[section]\nx = 1\ny = 2\n");
    auto expr = std::format("({}).section.x", ft(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("keys")))
        << "TOML object creation must record ImplicitShape #keys\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, TOML_Scalar_RecordsSCDep)
{
    TempTomlFile file("name = \"hello\"\n");
    auto expr = std::format("({}).name", ft(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "name"))
        << "TOML scalar access must record SC dep\n" << dumpDeps(deps);
}

TEST_F(DepPrecisionCreationTest, TOML_Array_ImplicitShapeLen)
{
    TempTomlFile file("items = [\"a\", \"b\", \"c\"]\n");
    auto expr = std::format("builtins.elemAt ({}).items 0", ft(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("len")))
        << "TOML array creation must record ImplicitShape #len\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// readDir creation-time deps
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionCreationTest, ReadDir_ImplicitShapeKeys)
{
    TempDir dir;
    dir.addFile("foo", "content");
    dir.addFile("bar", "content");

    auto expr = std::format("({}).foo", rd(dir.path()));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasJsonDep(deps, DepType::ImplicitShape, shapePred("keys")))
        << "readDir creation must record ImplicitShape #keys\n" << dumpDeps(deps);
}

// ═══════════════════════════════════════════════════════════════════════
// Content dep always present
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionCreationTest, ContentDep_AlwaysPresent)
{
    TempJsonFile file(R"({"x": 1})");
    auto expr = std::format("({}).x", fj(file.path));

    auto deps = evalAndCollectDeps(expr);
    EXPECT_TRUE(hasDep(deps, DepType::Content, ""))
        << "File read must always record Content dep\n" << dumpDeps(deps);
}

} // namespace nix::eval_trace
