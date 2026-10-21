/* Anti-rot cover for the memory-pressure-gated reclaim path
   (`eval-unforce-reclaim-margin`, `decideJob` in unforce.cc).

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

#include <optional>
#include <sstream>
#include <vector>

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
   `margin` is `eval-unforce-reclaim-margin` (0 disables reclaim). */
std::string runWorkload(ref<Store> store, fetchers::Settings & fetchSettings, bool forceReclaim, uint64_t margin)
{
    bool readOnlyMode = true;
    EvalSettings settings{readOnlyMode};
    settings.nixPath = {};
    settings.unforce = true;
    /* Smallest batch size: every element is its own batch. */
    settings.unforceBatchSize = 0;
    settings.unforceReclaimMargin = margin;
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
   test-only `unforceTestHeadroom`) and the given reclaim margin (the default
   if none); return the printed form and the deltas of the reclaim counters. */
CeilingRun runCeilingWorkload(
    ref<Store> store, fetchers::Settings & fetchSettings, size_t headroomBytes, std::optional<uint64_t> margin)
{
    bool readOnlyMode = true;
    EvalSettings settings{readOnlyMode};
    settings.nixPath = {};
    settings.unforce = true;
    settings.unforceBatchSize = 0;
    if (margin)
        settings.unforceReclaimMargin = *margin;
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

/* A workload where the pressure gate alone decides (`eval-unforce-reclaim-
   margin`, `decideJob`). Every element is its own batch and every batch is
   heavy (`heavy`, as above). Elements 0-1 deep-force the shared `a`, 2-3 `b`,
   4-5 `c` (each ~6 MiB): the first force of each is reverted, the second
   promotes it, so batches 1, 3 and 5 have an Evac(S) over the 4 MiB copy
   limit and under the 12 MiB ceiling (192 MiB headroom). Without pressure
   they are kept whole; under pressure they are reclaimed. Batches 0, 2 and 4
   copy little: without pressure they copy anyway (neither kept whole nor
   counted), under pressure they count as reclaimed. */
static const char * gateWorkload =
    "let\n"
    "  heavy = i: builtins.foldl' (a: b: a + b) 0 (builtins.genList (k: k + i) 300000);\n"
    "  a = builtins.genList (k: { x = k; }) 80000;\n"
    "  b = builtins.genList (k: { x = k + 1; }) 80000;\n"
    "  c = builtins.genList (k: { x = k + 2; }) 80000;\n"
    "  touch = i: builtins.deepSeq (if i < 2 then a else if i < 4 then b else c) 0;\n"
    "in builtins.genList (i: { n = heavy i + touch i; }) 6\n";

/* The memory readings the gate is fed, one per heavy decision, and the
   reclaim counters seen at each read: the counters at read i+1 (or at the
   end) minus those at read i are batch i's outcome. */
struct GateScript
{
    std::vector<size_t> readings;
    size_t next = 0;

    struct Snap
    {
        size_t evacuated, kept, overCeiling;
    };

    std::vector<Snap> snaps;
};

GateScript * gateScript = nullptr;

GateScript::Snap reclaimCounters()
{
    return {unforceHeavyEvacuatedCount(), unforceKeptJobsCount(), unforceHeavyOverCeilingCount()};
}

size_t scriptedReading()
{
    auto & g = *gateScript;
    g.snaps.push_back(reclaimCounters());
    size_t i = g.next++;
    return i < g.readings.size() ? g.readings[i] : g.readings.back();
}

struct GateRun
{
    std::string out;
    /* One letter per heavy decision: R reclaimed by the gate, K kept whole,
       C kept whole over the ceiling, - neither (copied without pressure). */
    std::string outcomes;
    size_t reads;
};

/* Run `gateWorkload` with headroom 192 MiB and the given margin. With
   `readings`, the gate is fed them (the test-only
   `unforceTestMemoryReading`); without, it reads the process's memory. */
GateRun runGateWorkload(
    ref<Store> store, fetchers::Settings & fetchSettings, uint64_t margin, std::optional<std::vector<size_t>> readings)
{
    bool readOnlyMode = true;
    EvalSettings settings{readOnlyMode};
    settings.nixPath = {};
    settings.unforce = true;
    settings.unforceBatchSize = 0;
    settings.unforceReclaimMargin = margin;
    EvalState state{{}, store, fetchSettings, settings, nullptr};

    Value v;
    Expr * e = state.parseExprFromString(gateWorkload, state.rootPath(CanonPath::root));
    state.eval(e, v);

    PrintOptions options;
    options.force = true;
    options.unforce = true;

    GateScript script;
    size_t reads0 = unforceMemoryReadsCount();
    auto start = reclaimCounters();
    std::ostringstream out;
    unforceTestHeadroom = size_t(192) << 20;
    if (readings) {
        script.readings = *readings;
        gateScript = &script;
        unforceTestMemoryReading = scriptedReading;
    }
    try {
        printValue(state, out, v, options);
    } catch (...) {
        unforceTestHeadroom = 0;
        unforceTestMemoryReading = nullptr;
        gateScript = nullptr;
        throw;
    }
    unforceTestHeadroom = 0;
    unforceTestMemoryReading = nullptr;
    gateScript = nullptr;

    GateRun r{out.str(), "", unforceMemoryReadsCount() - reads0};
    /* Per-batch outcomes are only observable through the script's snapshots;
       a live run reports the totals as a sorted string. */
    if (readings) {
        script.snaps.push_back(reclaimCounters());
        for (size_t i = 0; i + 1 < script.snaps.size(); ++i) {
            auto & a = script.snaps[i];
            auto & b = script.snaps[i + 1];
            r.outcomes += b.overCeiling > a.overCeiling ? 'C'
                          : b.kept > a.kept             ? 'K'
                          : b.evacuated > a.evacuated   ? 'R'
                                                        : '-';
        }
    } else {
        auto end = reclaimCounters();
        r.outcomes = std::string(end.evacuated - start.evacuated, 'R') + std::string(end.kept - start.kept, 'K');
    }
    return r;
}

} // namespace

class UnforceReclaimTest : public LibExprTest
{};

/* The gate reclaims a heavy batch when less than the margin of headroom is
   free (headroom − the process's current memory), at every heavy decision
   (`processMemory` in unforce.cc). Fed scripted readings (192 MiB headroom,
   64 MiB margin: reclaim when the reading exceeds exactly 128 MiB), it
   reclaims exactly the heavy batches whose reading leaves less than the
   margin free, keeps the rest whole, reads once per heavy decision, and
   prints byte-identically to keeping whole. Mutation sensitivity:
     - gating on `keptJobBytes` again (the old signal): the per-batch
       outcomes follow the retained bytes, not the readings;
     - inverting the comparison: "all at the margin" reclaims;
     - `>=` for `>` (free ≤ margin): batch 3 of "mixed" (exactly the margin
       free) is reclaimed;
     - comparing the reading itself with the margin (used for free): batches 0
       and 4 of "mixed" (100 MiB used, 92 MiB free) are reclaimed. */
TEST_F(UnforceReclaimTest, gateReadsProcessMemory)
{
    if (NIX_USE_BOEHMGC)
        GTEST_SKIP() << "unforcing is disabled under Boehm GC";

    constexpr size_t mib = size_t(1) << 20;
    constexpr uint64_t margin = 64 * mib;
    /* `edge` leaves exactly the margin free (kept); `over` one byte less. */
    constexpr size_t edge = 128 * mib, over = edge + 1, low = 100 * mib, above = size_t(1) << 40;

    /* Baseline: reclaim disabled (margin 0 is the sentinel; no reads). Live
       totals: batches 1, 3 and 5 are kept whole. */
    auto keepWhole = runGateWorkload(store, fetchSettings, 0, std::nullopt);
    EXPECT_EQ(keepWhole.reads, 0u);
    EXPECT_EQ(keepWhole.outcomes, "KKK");

    auto allAtMargin = runGateWorkload(store, fetchSettings, margin, std::vector<size_t>(6, edge));
    EXPECT_EQ(allAtMargin.out, keepWhole.out);
    EXPECT_EQ(allAtMargin.outcomes, "-K-K-K")
        << "with the margin still free every heavy batch is decided as without the gate";
    EXPECT_EQ(allAtMargin.reads, 6u) << "one read per heavy decision";

    auto allOver = runGateWorkload(store, fetchSettings, margin, std::vector<size_t>(6, over));
    EXPECT_EQ(allOver.out, keepWhole.out) << "reclaiming must be byte-identical to keeping whole";
    EXPECT_EQ(allOver.outcomes, "RRRRRR") << "with less than the margin free every heavy batch is reclaimed";
    EXPECT_EQ(allOver.reads, 6u);

    auto mixed = runGateWorkload(store, fetchSettings, margin, std::vector<size_t>{low, over, above, edge, low, above});
    EXPECT_EQ(mixed.out, keepWhole.out);
    EXPECT_EQ(mixed.outcomes, "-RRK-R") << "each heavy batch follows its own reading";
    EXPECT_EQ(mixed.reads, 6u);

    /* A margin of all of headroom always reclaims, without reading. */
    auto always = runGateWorkload(store, fetchSettings, 192 * mib, std::nullopt);
    EXPECT_EQ(always.out, keepWhole.out);
    EXPECT_EQ(always.outcomes, "RRRRRR");
    EXPECT_EQ(always.reads, 0u);

#if defined(__linux__) || defined(__APPLE__)
    /* The live reading (no script): this process's resident memory is far
       above 1 MiB, so less than a 191 MiB margin of the 192 MiB headroom is
       free. Live dormancy (an ample headroom) is covered by
       `ceilingBoundsEvacNotRegion`. */
    auto liveTight = runGateWorkload(store, fetchSettings, 191 * mib, std::nullopt);
    EXPECT_EQ(liveTight.out, keepWhole.out);
    EXPECT_EQ(liveTight.outcomes, "RRRRRR") << "live reading over the margin reclaims every heavy batch";
    EXPECT_EQ(liveTight.reads, 6u);
#endif
}

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
    auto keepWhole = runCeilingWorkload(store, fetchSettings, headroomBytes, 0);
    EXPECT_EQ(keepWhole.keptJobs, 2u);
    EXPECT_EQ(keepWhole.evacuated, 0u);
    EXPECT_EQ(keepWhole.overCeiling, 0u);

    /* Under pressure (a margin of all of headroom: the gate is open from the
       first batch). */
    auto pressured = runCeilingWorkload(store, fetchSettings, headroomBytes, headroomBytes);
    EXPECT_EQ(pressured.out, keepWhole.out) << "reclaiming must be byte-identical to keeping whole";
    EXPECT_EQ(pressured.keptJobs, 1u) << "only the batch whose Evac(S) exceeds the ceiling (3) is kept whole";
    EXPECT_EQ(pressured.overCeiling, 1u) << "batch 3's Evac(S) exceeds the ceiling";
    EXPECT_EQ(pressured.evacuated, 4u) << "batches 0, 1, 2 and 4 (regions over the ceiling) are reclaimed";

    /* Dormant: with ample headroom the default gate never opens. */
    auto dormant = runCeilingWorkload(store, fetchSettings, size_t(1) << 40, std::nullopt);
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
    std::string keepWhole = runWorkload(store, fetchSettings, /*forceReclaim=*/false, /*margin=*/0);
    size_t keptWholeEvacuated = unforceHeavyEvacuatedCount() - evacBefore0;

    /* Forced reclaim: evacuate-then-free every heavy batch, on any machine. */
    size_t seenBefore = unforceSeenNominatedCount();
    size_t evacBefore1 = unforceHeavyEvacuatedCount();
    std::string reclaimed = runWorkload(store, fetchSettings, /*forceReclaim=*/true, /*margin=*/0);
    size_t reclaimedEvacuated = unforceHeavyEvacuatedCount() - evacBefore1;
    size_t seenNominated = unforceSeenNominatedCount() - seenBefore;

    /* (1) Byte-identical output. */
    EXPECT_EQ(keepWhole, reclaimed);

    /* (2) The reclaim path ran only when forced. A mutation that keeps heavy
       batches whole regardless (the path rotting) makes `reclaimedEvacuated`
       zero and fails here. */
    EXPECT_EQ(keptWholeEvacuated, 0u) << "reclaim must not run with the feature disabled (margin 0)";
    EXPECT_GT(reclaimedEvacuated, 0u) << "forced reclaim must evacuate-then-free at least one heavy batch";

    /* Blocker (b): the workload exercises printing/«repeated», so the printer
       nominated targets (heavy batches coincide with seen-set nominations). */
    EXPECT_GT(seenNominated, 0u) << "workload must exercise printing/«repeated» (seenNominated > 0)";
}

} // namespace nix
