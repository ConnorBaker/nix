/**
 * Characterization: the dependency-flattening baseline (the ~607× the Tier-1
 * derivation-edge / compositional-trace-DAG work aims to remove).
 *
 * Architecture finding (plans/architecture-trace-model-vs-CA.md): eval-trace
 * FLATTENS a consumer's transitive dep closure into the consumer's OWN trace —
 * `replayMemoizedRange` (dep-recording-context.hh:307) copies a forced child's
 * dep range up into the enclosing scope. So when N sibling attr-path nodes each
 * read the SAME shared file, that file's dep is COPIED into each of the N
 * traces, instead of referenced once. On nixpkgs that is the measured ~607×
 * (one shared-closure dep recorded once per consumer whose closure includes it).
 *
 * These tests pin that flattening at UNIT scale, so:
 *   (1) the baseline the edge model must improve is regression-guarded, and
 *   (2) if a future change accidentally makes flattening WORSE (or, better,
 *       an edge change makes it BETTER), these tests move and say so.
 *
 * No production code; pure characterization via MaterializationDepTest +
 * getStoredDeps (inspects a specific attr-path's stored trace deps).
 */
#include "eval-trace/helpers.hh"

#include <format>

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// Two sibling attr-path nodes, each projecting a DIFFERENT field of the SAME
// shared JSON file. Each sibling's stored trace must INDEPENDENTLY carry a
// FileBytes dep on the shared file — i.e. the shared file's dep is flattened
// (copied) into BOTH traces, not stored once and referenced. This is the
// duplication the compositional-DAG/edge work targets.
TEST_F(MaterializationDepTest, SharedFileDep_FlattenedIntoEachSiblingTrace)
{
    TempJsonFile shared(R"({"x": 1, "y": 2})");
    // d is the shared source; cx and cy are two sibling consumers, each reading
    // a different field of d. Both transitively read the same file `shared`.
    auto expr = std::format(
        "let d = {}; in {{ cx = d.x; cy = d.y; }}", fj(shared.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        for (auto * name : {"cx", "cy"}) {
            auto * a = root.attrs()->get(state.symbols.create(name));
            ASSERT_NE(a, nullptr);
            state.forceValue(*a->value, noPos);
        }
    }

    auto cxDeps = getStoredDeps("cx");
    auto cyDeps = getStoredDeps("cy");

    // The flattening fact: BOTH sibling traces independently carry a FileBytes
    // dep on the shared file. (substring match on the temp file path.)
    auto fileName = std::string(shared.path.filename());
    EXPECT_TRUE(hasDep(cxDeps, CanonicalQueryKind::FileBytes, fileName))
        << "cx must carry the shared file's FileBytes dep (flattened in)\n"
        << dumpDeps(cxDeps);
    EXPECT_TRUE(hasDep(cyDeps, CanonicalQueryKind::FileBytes, fileName))
        << "cy must INDEPENDENTLY carry the same shared file's FileBytes dep — "
           "the dep is COPIED into each sibling, not referenced once (the ~607× "
           "flattening the edge model aims to remove)\n"
        << dumpDeps(cyDeps);

    // Non-vacuity: cx and cy are DISTINCT traces (cx carries the x-field
    // projection, cy the y-field), so "each independently carries the shared
    // FileBytes dep" is a real per-trace fact, not the same (e.g. root) trace
    // returned twice.
    EXPECT_TRUE(hasJsonDep(cxDeps, CanonicalQueryKind::StructuredProjection, pathContainsPred(nlohmann::json::array({"x"}))))
        << "cx's trace must carry the x-field projection (distinct from cy)\n" << dumpDeps(cxDeps);
    EXPECT_TRUE(hasJsonDep(cyDeps, CanonicalQueryKind::StructuredProjection, pathContainsPred(nlohmann::json::array({"y"}))))
        << "cy's trace must carry the y-field projection (distinct from cx)\n" << dumpDeps(cyDeps);
}

// Quantitative companion: with K sibling consumers all reading the shared file,
// the shared file's dep appears K times across the K traces (once per trace),
// confirming the cost scales with consumers, not with distinct files. We assert
// the count of traces carrying the shared dep == K.
TEST_F(MaterializationDepTest, SharedFileDep_CountScalesWithConsumers)
{
    TempJsonFile shared(R"({"a": 1, "b": 2, "c": 3})");
    auto expr = std::format(
        "let d = {}; in {{ ca = d.a; cb = d.b; cc = d.c; }}", fj(shared.path));

    {
        auto cache = makeCache(expr);
        auto root = forceRoot(*cache);
        state.forceAttrs(root, noPos, "test");
        for (auto * name : {"ca", "cb", "cc"}) {
            auto * a = root.attrs()->get(state.symbols.create(name));
            ASSERT_NE(a, nullptr);
            state.forceValue(*a->value, noPos);
        }
    }

    auto fileName = std::string(shared.path.filename());
    int tracesCarryingShared = 0;
    for (auto * name : {"ca", "cb", "cc"})
        if (hasDep(getStoredDeps(name), CanonicalQueryKind::FileBytes, fileName))
            ++tracesCarryingShared;

    EXPECT_EQ(tracesCarryingShared, 3)
        << "the shared file's dep is flattened into ALL 3 consumer traces "
           "(cost scales with #consumers — the duplication the edge model "
           "would collapse to one reference)";
}

} // namespace nix::eval_trace
