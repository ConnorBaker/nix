/**
 * Tests for DepSource serialization/parsing (DepSourceEncodingTest) and
 * InterningPools intern/resolve round-trips (DepSourceInterningTest).
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── DepSource encoding / parsing ────────────────────────────────────

TEST(DepSourceEncodingTest, DepSource_Parse_UsesExplicitKinds)
{
    auto synthetic = parseDepSource(serializeDepSource(DepSource::makeAbsolute()));
    ASSERT_TRUE(synthetic);
    EXPECT_EQ(synthetic->kind(), DepSourceKind::Absolute);
    EXPECT_EQ(synthetic->value.index(), 0u);

    auto absolute = parseDepSource(serializeDepSource(DepSource::makeAbsolute()));
    ASSERT_TRUE(absolute);
    EXPECT_EQ(absolute->kind(), DepSourceKind::Absolute);

    auto registered1 = parseDepSource(serializeDepSource(DepSource::fromNodeKey("nixpkgs")));
    ASSERT_TRUE(registered1);
    EXPECT_EQ(registered1->kind(), DepSourceKind::Registered);
    ASSERT_NE(registered1->graphNodeKey(), nullptr);
    EXPECT_EQ(registered1->graphNodeKey()->value, "nixpkgs");

    auto registered2 = parseDepSource(serializeDepSource(
        DepSource::fromRuntimeRoot(runtimeRootSourceKeyFromDebugString("path:/tmp/src?narHash=sha256-abc"))));
    ASSERT_TRUE(registered2);
    EXPECT_EQ(registered2->kind(), DepSourceKind::Registered);
    ASSERT_NE(registered2->runtimeRootKey(), nullptr);
    EXPECT_EQ(
        *registered2->runtimeRootKey(),
        runtimeRootSourceKeyFromDebugString("path:/tmp/src?narHash=sha256-abc"));

    EXPECT_FALSE(parseDepSource(""));
}

TEST(DepSourceEncodingTest, DepSource_Resolve_RejectsEmptyPayload)
{
    InterningPools pools;
    pools.bulkLoadString(1, "");
    auto badSourceId = DepSourceId(1);
    EXPECT_THROW((void) pools.resolveDepSource(badSourceId), Error);
}

// ── DepSource interning consistency ────────────────────────────────

TEST(DepSourceInterningTest, Registered_InternResolveCycle_IsIdentity)
{
    InterningPools pools;

    auto source1 = DepSource::fromNodeKey("nixpkgs");
    auto source2 = DepSource::fromRuntimeRoot(
        runtimeRootSourceKeyFromDebugString("path:/tmp/runtime?narHash=sha256-abc"));
    auto absolute = DepSource::makeAbsolute();

    // Intern each source.
    auto id1a = pools.intern<DepSourceId>(source1);
    auto id2a = pools.intern<DepSourceId>(source2);
    auto idAbs = pools.intern<DepSourceId>(absolute);

    // Intern the same sources again — must get the same IDs.
    auto id1b = pools.intern<DepSourceId>(source1);
    auto id2b = pools.intern<DepSourceId>(source2);
    EXPECT_EQ(id1a, id1b);
    EXPECT_EQ(id2a, id2b);

    // Resolve back — must recover the original DepSource.
    auto resolved1 = pools.resolveDepSource(id1a);
    auto resolved2 = pools.resolveDepSource(id2a);
    auto resolvedAbs = pools.resolveDepSource(idAbs);

    EXPECT_EQ(resolved1, source1);
    EXPECT_EQ(resolved2, source2);
    EXPECT_EQ(resolvedAbs, absolute);

    // Different sources must get different IDs.
    EXPECT_NE(id1a, id2a);
    EXPECT_NE(id1a, idAbs);
}

} // namespace nix::eval_trace
