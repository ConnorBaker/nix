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

} // namespace

class UnforceReclaimTest : public LibExprTest
{};

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
