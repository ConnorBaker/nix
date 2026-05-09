/**
 * Tests for SemanticRegistry — forward resolve dispatch by DepSourceKind,
 * mount-point-based reverse resolution (resolveDepPathKey), and the
 * recording→verification round-trip invariant that mount-point sources
 * must match forward-entry sources.
 */
#include "eval-trace/helpers.hh"
#include "eval-trace/semantic-registry-test-access.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

TEST(SemanticRegistryTest, Registry_Resolve_DispatchesBySourceKind)
{
    boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> entries;
    entries.emplace(
        DepSource::fromNodeKey("nixpkgs"),
        SourcePath(getFSSourceAccessor(), CanonPath("/tmp/flake-root")));
    entries.emplace(
        DepSource::fromRuntimeRoot(runtimeRootSourceKeyFromDebugString("path:/tmp/runtime?narHash=sha256-abc")),
        SourcePath(getFSSourceAccessor(), CanonPath("/tmp/runtime-root")));

    SemanticRegistry registry(std::move(entries));

    auto absolute = registry.resolve(DepSource::makeAbsolute(), "/tmp/absolute-file");
    ASSERT_TRUE(absolute);
    EXPECT_EQ(absolute->path.abs(), "/tmp/absolute-file");

    auto flake = registry.resolve(DepSource::fromNodeKey("nixpkgs"), "default.nix");
    ASSERT_TRUE(flake);
    EXPECT_EQ(flake->path.abs(), "/tmp/flake-root/default.nix");

    auto runtime = registry.resolve(
        DepSource::fromRuntimeRoot(runtimeRootSourceKeyFromDebugString("path:/tmp/runtime?narHash=sha256-abc")),
        "value.txt");
    ASSERT_TRUE(runtime);
    EXPECT_EQ(runtime->path.abs(), "/tmp/runtime-root/value.txt");

    // Unregistered runtime source returns nullopt.
    EXPECT_FALSE(registry.resolve(
        DepSource::fromRuntimeRoot(runtimeRootSourceKeyFromDebugString("path:/tmp/runtime-2?narHash=sha256-def")),
        "value.txt"));

    // Absolute source with non-absolute key is not meaningful (test API contract).
    EXPECT_FALSE(registry.resolve(DepSource::makeAbsolute(), "ignored"));
}

TEST(SemanticRegistryTest, Registry_DepPathKey_PrefersFlakeMountOverRuntime)
{
    SemanticRegistry registry;
    SemanticRegistryTestAccess::addMountPoint(registry,
        CanonPath("/nix/store/hash-source"),
        DepSource::fromNodeKey("self"),
        "b-low");

    auto path = SourcePath(
        getFSSourceAccessor(),
        CanonPath("/nix/store/hash-source/b-low/flake.nix"));
    auto origin = PathObject{
        .source = DepSource::fromRuntimeRoot(runtimeRootSourceKeyFromDebugString("path:/tmp/runtime-self?narHash=sha256-abc")),
        .rootPath = CanonPath("/nix/store/hash-source"),
    };

    auto depPath = resolveDepPathKey(path, registry, origin);
    EXPECT_EQ(depPath.source, DepSource::fromNodeKey("self"));
    EXPECT_EQ(depPath.key, "/flake.nix");
}

TEST(SemanticRegistryTest, Registry_MountPoint_IdentityMatchesForwardEntry)
{
    // Build registry with both forward entries and mount points using node keys.
    boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> entries;
    auto rootSource = DepSource::fromNodeKey("root");
    auto rootPath = SourcePath(getFSSourceAccessor(), CanonPath("/nix/store/abc-source"));
    entries.emplace(rootSource, rootPath);

    SemanticRegistry registry(std::move(entries));
    SemanticRegistryTestAccess::addMountPoint(registry,
        CanonPath("/nix/store/abc-source"),
        DepSource::fromNodeKey("root"),
        "");

    // Simulate recording: resolve a file path via reverseResolve (mount points)
    auto filePath = SourcePath(
        getFSSourceAccessor(),
        CanonPath("/nix/store/abc-source/data.txt"));
    auto depPath = resolveDepPathKey(filePath, registry, std::nullopt);

    // The recorded dep must use the node-key-based source
    EXPECT_EQ(depPath.source.kind(), DepSourceKind::Registered);
    EXPECT_EQ(depPath.source, DepSource::fromNodeKey("root"));
    EXPECT_EQ(depPath.key, "/data.txt");

    // Simulate verification: resolve the same source+key via forward entries
    auto resolved = registry.resolve(depPath.source, depPath.key);
    ASSERT_TRUE(resolved) << "Forward resolve must succeed for deps recorded via mount points";
    EXPECT_EQ(resolved->path.abs(), "/nix/store/abc-source/data.txt");
}

TEST(SemanticRegistryTest, Registry_SubdirMountPoint_ResolvesCorrectly)
{
    boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> entries;
    auto subSource = DepSource::fromNodeKey("sub");
    auto subPath = SourcePath(getFSSourceAccessor(), CanonPath("/nix/store/xyz-source/sub"));
    entries.emplace(subSource, subPath);

    SemanticRegistry registry(std::move(entries));
    SemanticRegistryTestAccess::addMountPoint(registry,
        CanonPath("/nix/store/xyz-source"),
        DepSource::fromNodeKey("sub"),
        "sub");

    auto filePath = SourcePath(
        getFSSourceAccessor(),
        CanonPath("/nix/store/xyz-source/sub/flake.nix"));
    auto depPath = resolveDepPathKey(filePath, registry, std::nullopt);

    EXPECT_EQ(depPath.source, DepSource::fromNodeKey("sub"));
    EXPECT_EQ(depPath.key, "/flake.nix");

    auto resolved = registry.resolve(depPath.source, depPath.key);
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->path.abs(), "/nix/store/xyz-source/sub/flake.nix");
}

TEST(SemanticRegistryTest, Registry_DepPathKey_UsesOriginForRuntimeAlias)
{
    // When the registry has no mount point for the path, resolveDepPathKey
    // falls through to the PathObject provided by the caller.
    SemanticRegistry registry;

    auto path = SourcePath(
        getFSSourceAccessor(),
        CanonPath("/nix/store/runtime-root/value.txt"));
    auto origin = PathObject{
        .source = DepSource::fromRuntimeRoot(runtimeRootSourceKeyFromDebugString("path:/tmp/runtime-b?narHash=sha256-def")),
        .rootPath = CanonPath("/nix/store/runtime-root"),
    };

    auto depPath = resolveDepPathKey(path, registry, origin);
    EXPECT_EQ(depPath.source, origin.source);
    EXPECT_EQ(depPath.key, "/value.txt");
}

} // namespace nix::eval_trace
