#include "eval-trace/helpers.hh"

#include <format>
#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DepPrecisionNixpkgsPatternsTest : public DepPrecisionTest {};

// ═══════════════════════════════════════════════════════════════════════
// Realistic nixpkgs patterns with traced data
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DepPrecisionNixpkgsPatternsTest, FlakeLockParsing_NestedScalar_CacheHit)
{
    // Mimic: (fromJSON (readFile ./flake.lock)).nodes.nixpkgs.locked.rev
    TempJsonFile file(R"({"nodes":{"nixpkgs":{"locked":{"rev":"abc123","type":"github"}}}})");
    auto expr = std::format("({}).nodes.nixpkgs.locked.rev", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "rev"))
            << "Nested scalar access records SC dep\n" << dumpDeps(deps);
        EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << "Pure scalar access must NOT record SC #keys\n" << dumpDeps(deps);
    }

    // ── Cache behavior: unrelated change ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("abc123"));
    }

    // Change type field (unrelated to rev) -- cache hit
    file.modify(R"({"nodes":{"nixpkgs":{"locked":{"rev":"abc123","type":"git"}}}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "Unrelated field change must not invalidate";
        EXPECT_THAT(v, IsStringEq("abc123"));
    }
}

TEST_F(DepPrecisionNixpkgsPatternsTest, FlakeLockParsing_RevChanged_CacheMiss)
{
    TempJsonFile file(R"({"nodes":{"nixpkgs":{"locked":{"rev":"abc123"}}}})");
    auto expr = std::format("({}).nodes.nixpkgs.locked.rev", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("abc123"));
    }

    file.modify(R"({"nodes":{"nixpkgs":{"locked":{"rev":"def456"}}}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("def456"));
    }
}

TEST_F(DepPrecisionNixpkgsPatternsTest, CallPackage_OverridePattern_CacheHit)
{
    // (data // { meta.broken = (fromJSON ...).broken; }).name
    // The // adds a nix-created attr; accessing .name should only need the scalar dep
    TempJsonFile file(R"({"name": "hello", "broken": false})");
    auto expr = std::format("let j = {}; in (j // {{ meta = {{ broken = j.broken; }}; }}).name", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "name"))
            << dumpDeps(deps);
        EXPECT_FALSE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << "// must NOT record SC #keys\n" << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Change broken (unrelated to .name access) -- cache hit
    file.modify(R"({"name": "hello", "broken": true})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "Changing unrelated field must not invalidate";
        EXPECT_THAT(v, IsStringEq("hello"));
    }
}

TEST_F(DepPrecisionNixpkgsPatternsTest, ConditionalPackageSet_CacheMiss)
{
    // if (fromJSON ...).enabled then { pkg = ...; } else {}
    TempJsonFile file(R"({"enabled": true, "version": "1.0"})");
    auto expr = std::format(R"(let j = {}; in if j.enabled then j.version else "disabled")", fj(file.path));

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("1.0"));
    }

    // Change enabled -- cache miss
    file.modify(R"({"enabled": false, "version": "1.0"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_THAT(v, IsStringEq("disabled"));
    }
}

TEST_F(DepPrecisionNixpkgsPatternsTest, LetBindings_TracedData_CacheHit)
{
    // let data = fromJSON ...; in { inherit (data) x y; }
    TempJsonFile file(R"({"x": "a", "y": "b", "z": "c"})");
    auto expr = std::format(R"(let j = {}; in j.x + "-" + j.y)", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "x"))
            << dumpDeps(deps);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "y"))
            << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("a-b"));
    }

    // Change z (unrelated) -- cache hit
    file.modify(R"({"x": "a", "y": "b", "z": "CHANGED"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("a-b"));
    }
}

TEST_F(DepPrecisionNixpkgsPatternsTest, ModuleSystem_MkOption_CacheHit)
{
    // Simplified: accessing a specific field from JSON (like mkOption default)
    TempJsonFile file(R"({"x": "default-val", "y": "other"})");
    auto expr = std::format("({}).x", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "x"))
            << "mkOption scalar access must record SC dep\n" << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("default-val"));
    }

    file.modify(R"({"x": "default-val", "y": "changed"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("default-val"));
    }
}

TEST_F(DepPrecisionNixpkgsPatternsTest, NixpkgsAllPackages_MapAttrs_CacheMiss)
{
    // mapAttrs (n: v: v.meta.license) (fromJSON ...) -> attrNames on result
    TempJsonFile file(R"({"pkg1": {"meta": {"license": "MIT"}}, "pkg2": {"meta": {"license": "GPL"}}})");
    auto expr = std::format("builtins.attrNames (builtins.mapAttrs (n: v: v.meta.license) ({}))", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << "mapAttrs -> attrNames records SC #keys\n" << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    file.modify(R"({"pkg1": {"meta": {"license": "MIT"}}, "pkg2": {"meta": {"license": "GPL"}}, "pkg3": {"meta": {"license": "BSD"}}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3);
    }
}

TEST_F(DepPrecisionNixpkgsPatternsTest, OverlayComposition_CacheHit)
{
    // Simplified overlay: data // { extra = "added"; }
    // Access a field from original data -- unrelated extra doesn't invalidate
    TempJsonFile file(R"({"pkg": "hello-1.0", "lib": "lib-2.0"})");
    auto expr = std::format(R"(let j = {}; in (j // {{ extra = "overlay"; }}).pkg)", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "pkg"))
            << "overlay pattern must record SC dep for accessed field\n" << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello-1.0"));
    }

    // Change lib (unrelated) -- cache hit
    file.modify(R"({"pkg": "hello-1.0", "lib": "lib-3.0"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsStringEq("hello-1.0"));
    }
}

TEST_F(DepPrecisionNixpkgsPatternsTest, PackageSet_FilterBroken_CacheMiss)
{
    // filterAttrs (n: v: !v.broken) data -> attrNames
    TempJsonFile file(R"({"a": {"broken": false}, "b": {"broken": true}})");
    auto expr = std::format("builtins.attrNames (builtins.removeAttrs ({}) [])", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << "attrNames on removeAttrs records SC #keys\n" << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    file.modify(R"({"a": {"broken": false}, "b": {"broken": true}, "c": {"broken": false}})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3);
    }
}

TEST_F(DepPrecisionNixpkgsPatternsTest, MkDerivation_MetaFromJson_CacheMiss)
{
    // Accessing meta.license from JSON -- adding an unrelated meta field invalidates
    // if we use attrNames, but NOT if we just access .license
    TempJsonFile file(R"({"license": "MIT", "description": "A package"})");
    auto expr = std::format("({}).license", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasDep(deps, DepType::StructuredContent, "license"))
            << "Scalar access must record SC dep for license\n" << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("MIT"));
    }

    // Add maintainers (unrelated to .license) -- cache hit
    file.modify(R"({"license": "MIT", "description": "A package", "maintainers": ["alice"]})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0) << "Adding unrelated key must not invalidate .license";
        EXPECT_THAT(v, IsStringEq("MIT"));
    }
}

TEST_F(DepPrecisionNixpkgsPatternsTest, WithPackages_Pattern_CacheMiss)
{
    // attrNames on fromJSON -- adding a key invalidates
    TempJsonFile file(R"({"numpy": "1.0", "pandas": "2.0"})");
    auto expr = std::format("builtins.attrNames ({})", fj(file.path));

    // ── Dep verification ──
    {
        auto deps = evalAndCollectDeps(expr);
        EXPECT_TRUE(hasJsonDep(deps, DepType::StructuredContent, shapePred("keys")))
            << "attrNames records SC #keys\n" << dumpDeps(deps);
    }

    // ── Cache behavior ──
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.listSize(), 2);
    }

    file.modify(R"({"numpy": "1.0", "pandas": "2.0", "scipy": "3.0"})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1);
        EXPECT_EQ(v.listSize(), 3);
    }
}

} // namespace nix::eval_trace
