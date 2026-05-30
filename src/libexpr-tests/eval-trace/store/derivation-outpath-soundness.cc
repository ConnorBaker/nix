/**
 * Soundness of derivation outPath caching under input change — the assumption
 * the Tier-1 derivation-edge direction rests on.
 *
 * Design: plans/compositional-trace-dag-design.md. Tier-1 proposes that a
 * consumer observing a derivation's OUTPUT can edge to its identity. A natural
 * worry: a derivation's recorded `StorePathAvailability(.drv)` dep verifies
 * EXISTENCE only (dep-resolution-service.cc:655, isValidPath → valid/missing),
 * not identity. So if a derivation's INPUT changes — producing a NEW .drv —
 * could the OLD trace still verify (old .drv still valid in store) and
 * STALE-SERVE the old outPath?
 *
 * These end-to-end cache tests (makeCache/forceRoot, real evaluator + store)
 * answer it as a measured fact: NO. The drv path is content-addressed (changing
 * an input yields a different .drv — pinned below), AND the outPath-producing
 * eval records the input observations themselves, so an input change invalidates
 * the trace via those deps — the StorePathAvailability existence check is a GC
 * guard, not the sole identity guard. This underwrites Tier-1: an output edge is
 * sound because input changes are caught, not bypassed.
 */
#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// Content-addressing: two derivations differing only in an arg get DIFFERENT
// .drv paths (the build-layer hashDerivationModulo property, surfaced as the
// StorePathAvailability dep key). This is what makes the existence dep
// input-sensitive at all.
TEST_F(DepPrecisionTest, DrvPath_IsContentAddressedByInputs)
{
    auto deps1 = evalAndCollectDeps(R"(
        (derivation { name = "p"; builder = "/bin/sh"; system = builtins.currentSystem; arg = "v1"; }).outPath)");
    auto deps2 = evalAndCollectDeps(R"(
        (derivation { name = "p"; builder = "/bin/sh"; system = builtins.currentSystem; arg = "v2"; }).outPath)");

    // Each records exactly one StorePathAvailability(.drv) dep; the two .drv
    // keys must DIFFER (different inputs → different content-addressed drv).
    auto drvKey = [](const std::vector<DepPrecisionTest::ResolvedDep> & ds) -> std::string {
        for (auto & d : ds)
            if (d.type == CanonicalQueryKind::StorePathAvailability) return d.key;
        return {};
    };
    auto k1 = drvKey(deps1), k2 = drvKey(deps2);
    EXPECT_FALSE(k1.empty()) << "v1 outPath must record a .drv availability dep\n" << dumpDeps(deps1);
    EXPECT_FALSE(k2.empty()) << "v2 outPath must record a .drv availability dep\n" << dumpDeps(deps2);
    EXPECT_NE(k1, k2)
        << "differing inputs must yield differing .drv identities (content-"
           "addressed) — k1=" << k1 << " k2=" << k2;
}

// End-to-end: a derivation whose input is file-backed. After the input changes,
// the warm cache must serve the NEW outPath (ground truth), NOT stale-serve the
// old one — even though the old .drv may remain valid in the store. This is the
// soundness floor the Tier-1 output edge inherits.
TEST_F(TraceCacheFixture, DrvOutPath_InputChange_NoStaleServe)
{
    TempTextFile argf("v1");
    auto expr = "(derivation { name = \"p\"; builder = \"/bin/sh\"; "
                "system = builtins.currentSystem; "
                "arg = builtins.readFile " + argf.path.string() + "; }).outPath";

    std::string coldOut;
    { auto c = makeCache(expr); auto v = forceRoot(*c); coldOut = std::string(v.string_view()); }

    argf.modify("v2");
    invalidateFileCache(argf.path);

    // Ground truth for the changed world.
    std::string truth;
    { auto v = eval(expr); state.forceValue(v, noPos); truth = std::string(v.string_view()); }
    EXPECT_NE(coldOut, truth)
        << "precondition: changing the input must change outPath (else the test "
           "proves nothing) cold=" << coldOut << " truth=" << truth;

    int calls = 0;
    std::string warm;
    { auto c = makeCache(expr, &calls); auto v = forceRoot(*c); warm = std::string(v.string_view()); }

    EXPECT_EQ(warm, truth)
        << "SOUNDNESS: after input v1→v2, warm cache must serve the NEW outPath, "
           "not stale-serve the old one. warm=" << warm << " truth=" << truth;
    EXPECT_EQ(calls, 1)
        << "the input change must force re-evaluation (the StorePathAvailability "
           "existence dep is a GC guard; input deps catch the change)";
}

// Precision companion: when the input is UNCHANGED, the warm cache must hit
// (no spurious re-eval) — the existence/identity machinery isn't over-conservative.
TEST_F(TraceCacheFixture, DrvOutPath_InputUnchanged_WarmHit)
{
    TempTextFile argf("stable");
    auto expr = "(derivation { name = \"p\"; builder = \"/bin/sh\"; "
                "system = builtins.currentSystem; "
                "arg = builtins.readFile " + argf.path.string() + "; }).outPath";

    { auto c = makeCache(expr); (void) forceRoot(*c); }
    invalidateFileCache(argf.path);  // clear caches, but file content is unchanged

    int calls = 0;
    { auto c = makeCache(expr, &calls); (void) forceRoot(*c); }
    EXPECT_EQ(calls, 0)
        << "precision: unchanged input must warm-hit the cached outPath";
}

} // namespace nix::eval_trace
