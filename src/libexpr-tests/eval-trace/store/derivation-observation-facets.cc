/**
 * Characterization: what deps does observing a derivation's OUTPUT vs a
 * NON-OUTPUT FACET actually record? (Tier-1 derivation-edge prerequisite.)
 *
 * Design: plans/compositional-trace-dag-design.md (Tier-1 is gated on the
 * consumer observing a derivation OUTPUT-ONLY). These tests turn that
 * prerequisite from an assumption into a pinned, measured fact — so a future
 * Tier-1 recorder has a precise, regression-guarded contract for WHEN it may
 * emit an output-identity edge (output-only observation) vs when it must NOT
 * (a facet was observed → facet deps were recorded → an output-only edge would
 * drop them → stale serve, the C3 hole in derivation-edge-soundness.cc).
 *
 * Measured via the real evaluator (`evalAndCollectDeps`) on real derivations.
 */
#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// Observing ONLY a derivation's output (drv.outPath) records ONLY output-identity
// deps: the .drv store-path availability + the session system. No FileBytes, no
// StructuredProjection — nothing that would be dropped by an output-identity
// edge. This is the SOUND case for a Tier-1 edge.
TEST_F(DepPrecisionTest, DrvOutPath_RecordsOnlyOutputIdentityDeps)
{
    auto deps = evalAndCollectDeps(R"(
        (derivation { name = "p"; builder = "/bin/sh"; system = builtins.currentSystem; }).outPath
    )");

    EXPECT_GE(countDepsByType(deps, CanonicalQueryKind::StorePathAvailability), 1u)
        << "outPath observation must record the .drv store-path availability "
           "(the output identity an edge would carry)";

    // The decisive output-only property: NO content/structured deps surface.
    EXPECT_EQ(countDepsByType(deps, CanonicalQueryKind::FileBytes), 0u)
        << "output-only observation must record no FileBytes (else an "
           "output-identity edge would drop a real file dependency)";
    EXPECT_EQ(countDepsByType(deps, CanonicalQueryKind::StructuredProjection), 0u)
        << "output-only observation must record no StructuredProjection";
}

// Real mkDerivation shape: meta lives on the OUTER attrset (drv // { meta = ...; }),
// NOT as a derivationStrict argument. Observing .outPath must NOT force .meta —
// confirmed by meta = throw not firing. So outPath stays output-only even when a
// meta attr is present alongside it. (Contrast: putting meta INSIDE
// `derivation {...}` makes derivationStrict force it — not the nixpkgs shape.)
TEST_F(DepPrecisionTest, DrvOutPath_DoesNotForceSiblingMeta)
{
    // If outPath forced meta, the throw would propagate and this would throw.
    std::vector<ResolvedDep> deps;
    ASSERT_NO_THROW({
        deps = evalAndCollectDeps(R"(
            let drv = derivation { name = "p"; builder = "/bin/sh"; system = builtins.currentSystem; };
            in (drv // { meta = throw "META_FORCED"; }).outPath
        )");
    }) << "outPath observation must not force a sibling meta attr (real mkDerivation shape)";

    EXPECT_GE(countDepsByType(deps, CanonicalQueryKind::StorePathAvailability), 1u);
    EXPECT_EQ(countDepsByType(deps, CanonicalQueryKind::FileBytes), 0u);
}

// Observing a FILE-BACKED non-output facet (meta sourced from JSON) DOES record
// the facet's file deps (FileBytes + StructuredProjection). This is the hazard
// the Tier-1 gate must respect: a consumer that read such a facet has recorded
// real deps; an output-only edge that dropped them would stale-serve. The gate
// "emit an edge only when NO facet dep was recorded" is therefore checkable from
// the recorded dep set.
TEST_F(DepPrecisionTest, DrvFacetFromFile_RecordsFacetFileDeps)
{
    TempJsonFile mf(R"({"x": 42})");
    auto deps = evalAndCollectDeps(
        "let metaData = builtins.fromJSON (builtins.readFile " + mf.path.string() + "); "
        "    drv = derivation { name = \"p\"; builder = \"/bin/sh\"; system = builtins.currentSystem; }; "
        "in (drv // { meta = metaData; }).meta.x");

    EXPECT_GE(countDepsByType(deps, CanonicalQueryKind::FileBytes), 1u)
        << "observing a file-backed facet must record a FileBytes dep on the "
           "facet source — an output-only edge dropping this would be unsound";
    EXPECT_GE(countDepsByType(deps, CanonicalQueryKind::StructuredProjection), 1u)
        << "the projected facet field records a StructuredProjection dep";
    // And crucially it records NO .drv store-path dep — observing meta is not
    // observing the output, so an output edge wouldn't even be a candidate here.
    EXPECT_EQ(countDepsByType(deps, CanonicalQueryKind::StorePathAvailability), 0u)
        << "observing only a facet records no output-identity dep";
}

} // namespace nix::eval_trace
