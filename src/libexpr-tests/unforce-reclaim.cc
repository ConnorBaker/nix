/* Anti-rot cover for the memory-pressure-gated reclaim path
   (`eval-unforce-reclaim-fraction`, `decideJob` in unforce.cc).

   DEFAULT-ON behaviour must not rot behind a flag: this test forces the
   reclaim path on regardless of the machine's memory (the test-only
   `unforceTestForceReclaim` override bypasses both the pressure gate and the
   transient-cap region ceiling), runs a printing workload whose element
   batches are *heavy* (their young closure exceeds the per-batch copy limit)
   and which exercises «repeated» printing (so the printer nominates targets,
   `seenNominated > 0` — blocker (b)), and asserts:

     (1) the printed output is BYTE-IDENTICAL to the same workload run with
         reclaim disabled (heavy batches kept whole, the pre-reclaim baseline);
     (2) the reclaim path was actually taken (`unforceHeavyEvacuatedCount` > 0)
         when forced, and NOT taken (== 0) when disabled.

   Mutation sensitivity (why this cannot rot):
     - "keep-whole-always" (the gate never reclaims, e.g. `wantReclaim = false`)
       leaves the counter at 0, so assertion (2) fails.
     - "skip-revert" (end a reclaimed batch without restoring its reverted
       cells) leaves cells pointing into freed region memory; re-forcing and
       re-printing them in a later batch yields different (or garbage) output,
       so assertion (1) fails.
   Both are demonstrated in the session report. Because assertions (1) and (2)
   run together on every build, the reclaim path is exercised in CI and cannot
   silently stop running. */

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/unforce.hh"
#include "nix/expr/print.hh"
#include "nix/util/canon-path.hh"

#include <gtest/gtest.h>

#include <sstream>

namespace nix {

namespace {

/* A printing workload with heavy element batches and «repeated» printing.

   `rep` is one shared value referenced by two attributes of every element and
   across all six elements: printing it the first time descends into it, every
   later encounter prints «repeated» (seen-set nomination). Each element's
   batch also forces `mkHeavy`, which builds a 400 000-element list (~6 MiB of
   young values, well over the 4 MiB copy limit) and folds it to a single
   integer — so the batch is heavy, yet prints compactly. From the second
   element on, the shared `rep` has been forced in a previous batch, so it is a
   promoted target of a heavy batch: exactly the case the reclaim gate acts on. */
static const char * workload =
    "let\n"
    "  mkHeavy = i: builtins.foldl' (a: b: a + b) 0 (builtins.genList (k: k + i) 400000);\n"
    "  rep = { a = 1; b = 2; };\n"
    "in builtins.genList (i: { n = mkHeavy i; r = rep; s = rep; }) 6\n";

/* Run the workload on a fresh evaluator with the given reclaim settings and
   return its printed form. `forceReclaim` drives the test-only override;
   `fraction` is `eval-unforce-reclaim-fraction` (>= 1 disables reclaim). */
std::string runWorkload(ref<Store> store, fetchers::Settings & fetchSettings, bool forceReclaim, float fraction)
{
    bool readOnlyMode = true;
    EvalSettings settings{readOnlyMode};
    settings.nixPath = {};
    settings.unforce = true;
    /* Smallest batch size: every element is its own batch. */
    settings.unforceBatchSize = 0;
    settings.unforceReclaimFraction = fraction;
    EvalState state{{}, store, fetchSettings, settings, nullptr};

    Value v;
    Expr * e = state.parseExprFromString(workload, state.rootPath(CanonPath::root));
    state.eval(e, v);

    PrintOptions options;
    options.force = true;
    options.unforce = true;
    options.trackRepeated = true;

    std::ostringstream out;
    unforceTestForceReclaim = forceReclaim;
    try {
        printValue(state, out, v, options);
    } catch (...) {
        unforceTestForceReclaim = false;
        throw;
    }
    unforceTestForceReclaim = false;
    return out.str();
}

/* A workload where the transient-cap ceiling decides (`reclaimEvacCeiling`,
   Evac(S) ≤ headroom/16). Every element is its own batch, and every batch is
   heavy: it forces `heavy`, a 300 000-element list folded to one integer
   (young garbage well over the ceiling the test sets, nothing kept).
   Elements 0 and 1 also deep-force the shared `mid`, elements 2 and 3 the
   shared `big`: the first force of each is reverted, the second (the cell's
   second force, `promoteAfter`) promotes it, so

     batch 0: Evac(S) small,                region > ceiling
     batch 1: Evac(S) = mid (~6 MiB),       region > ceiling
     batch 2: Evac(S) small,                region > ceiling
     batch 3: Evac(S) = big (~19 MiB),      region > ceiling
     batch 4: Evac(S) small,                region > ceiling

   With a 192 MiB headroom the ceiling is 12 MiB, above the 4 MiB copy limit
   and `mid`, below `big`: under pressure only batch 3 may be kept whole.
   Without pressure (keep-whole) batches 1 and 3 are, as their Evac(S)
   exceeds the copy limit. */
static const char * ceilingWorkload =
    "let\n"
    "  heavy = i: builtins.foldl' (a: b: a + b) 0 (builtins.genList (k: k + i) 300000);\n"
    "  mid = builtins.genList (k: { x = k; }) 80000;\n"
    "  big = builtins.genList (k: { x = k; }) 250000;\n"
    "  touch = i: if i < 2 then builtins.deepSeq mid 0 else if i < 4 then builtins.deepSeq big 0 else 0;\n"
    "in builtins.genList (i: { n = heavy i + touch i; }) 5\n";

struct CeilingRun
{
    std::string out;
    size_t evacuated, overCeiling, keptJobs;
};

/* Run `ceilingWorkload` with headroom overridden to `headroomBytes` (the
   test-only `unforceTestHeadroom`) and the given reclaim fraction; return the
   printed form and the deltas of the reclaim counters. */
CeilingRun
runCeilingWorkload(ref<Store> store, fetchers::Settings & fetchSettings, size_t headroomBytes, float fraction)
{
    bool readOnlyMode = true;
    EvalSettings settings{readOnlyMode};
    settings.nixPath = {};
    settings.unforce = true;
    settings.unforceBatchSize = 0;
    settings.unforceReclaimFraction = fraction;
    EvalState state{{}, store, fetchSettings, settings, nullptr};

    Value v;
    Expr * e = state.parseExprFromString(ceilingWorkload, state.rootPath(CanonPath::root));
    state.eval(e, v);

    PrintOptions options;
    options.force = true;
    options.unforce = true;

    size_t evac0 = unforceHeavyEvacuatedCount(), over0 = unforceHeavyOverCeilingCount(), kept0 = unforceKeptJobsCount();
    std::ostringstream out;
    unforceTestHeadroom = headroomBytes;
    try {
        printValue(state, out, v, options);
    } catch (...) {
        unforceTestHeadroom = 0;
        throw;
    }
    unforceTestHeadroom = 0;
    return {
        out.str(),
        unforceHeavyEvacuatedCount() - evac0,
        unforceHeavyOverCeilingCount() - over0,
        unforceKeptJobsCount() - kept0};
}

} // namespace

class UnforceReclaimTest : public LibExprTest
{};

/* The transient cap bounds Evac(S) — what a reclaim copies — not the region:
   a heavy batch whose region is above the ceiling but whose Evac(S) is below
   it is reclaimed, and only a batch whose Evac(S) itself exceeds the ceiling
   is kept whole. Mutation sensitivity:
     - capping the region instead (`youngBytes <= ceiling`, the earlier rule)
       keeps batch 1 whole as well and reclaims none: `keptJobs` is 2 and
       `evacuated` 0, failing both;
     - dropping the cap (no ceiling) reclaims batch 3 too: `keptJobs` 0 and
       `overCeiling` 0, failing both;
     - the gate never reclaiming: `evacuated` 0. */
TEST_F(UnforceReclaimTest, ceilingBoundsEvacNotRegion)
{
    if (NIX_USE_BOEHMGC)
        GTEST_SKIP() << "unforcing is disabled under Boehm GC";

    constexpr size_t headroomBytes = size_t(192) << 20; /* ceiling 12 MiB */

    /* Baseline: reclaim disabled. Batches 1 and 3 (Evac(S) over the 4 MiB
       copy limit) are kept whole. */
    auto keepWhole = runCeilingWorkload(store, fetchSettings, headroomBytes, 1.0f);
    EXPECT_EQ(keepWhole.keptJobs, 2u);
    EXPECT_EQ(keepWhole.evacuated, 0u);
    EXPECT_EQ(keepWhole.overCeiling, 0u);

    /* Under pressure (fraction 0: the gate is open from the first batch). */
    auto pressured = runCeilingWorkload(store, fetchSettings, headroomBytes, 0.0f);
    EXPECT_EQ(pressured.out, keepWhole.out) << "reclaiming must be byte-identical to keeping whole";
    EXPECT_EQ(pressured.keptJobs, 1u) << "only the batch whose Evac(S) exceeds the ceiling (3) is kept whole";
    EXPECT_EQ(pressured.overCeiling, 1u) << "batch 3's Evac(S) exceeds the ceiling";
    EXPECT_EQ(pressured.evacuated, 4u) << "batches 0, 1, 2 and 4 (regions over the ceiling) are reclaimed";

    /* Dormant: with ample headroom the default gate never opens. */
    auto dormant = runCeilingWorkload(store, fetchSettings, size_t(1) << 40, 0.5f);
    EXPECT_EQ(dormant.out, keepWhole.out);
    EXPECT_EQ(dormant.keptJobs, 2u);
    EXPECT_EQ(dormant.evacuated, 0u);
    EXPECT_EQ(dormant.overCeiling, 0u);
}

TEST_F(UnforceReclaimTest, forcedReclaimIsByteIdenticalToKeepWhole)
{
    if (NIX_USE_BOEHMGC)
        GTEST_SKIP() << "unforcing is disabled under Boehm GC";

    /* Baseline: reclaim disabled (heavy batches kept whole). */
    size_t evacBefore0 = unforceHeavyEvacuatedCount();
    std::string keepWhole = runWorkload(store, fetchSettings, /*forceReclaim=*/false, /*fraction=*/1.0f);
    size_t keptWholeEvacuated = unforceHeavyEvacuatedCount() - evacBefore0;

    /* Forced reclaim: evacuate-then-free every heavy batch, on any machine. */
    size_t seenBefore = unforceSeenNominatedCount();
    size_t evacBefore1 = unforceHeavyEvacuatedCount();
    std::string reclaimed = runWorkload(store, fetchSettings, /*forceReclaim=*/true, /*fraction=*/0.0f);
    size_t reclaimedEvacuated = unforceHeavyEvacuatedCount() - evacBefore1;
    size_t seenNominated = unforceSeenNominatedCount() - seenBefore;

    /* (1) Byte-identical output. */
    EXPECT_EQ(keepWhole, reclaimed);

    /* (2) The reclaim path ran only when forced. A mutation that keeps heavy
       batches whole regardless (the path rotting) makes `reclaimedEvacuated`
       zero and fails here. */
    EXPECT_EQ(keptWholeEvacuated, 0u) << "reclaim must not run with the feature disabled (fraction >= 1)";
    EXPECT_GT(reclaimedEvacuated, 0u) << "forced reclaim must evacuate-then-free at least one heavy batch";

    /* Blocker (b): the workload exercises printing/«repeated», so the printer
       nominated targets (heavy batches coincide with seen-set nominations). */
    EXPECT_GT(seenNominated, 0u) << "workload must exercise printing/«repeated» (seenNominated > 0)";
}

} // namespace nix
