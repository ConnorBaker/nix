#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <algorithm>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Nixpkgs cache failure pattern tests ──────────────────────────────
//
// These tests are synthesized from specific nixpkgs commit patterns observed
// in a 100-commit benchmark of `nix eval -f nixos/release.nix closures`.
// Each test simulates a cache failure mode at the TraceStore level.

// -- Category A: Irrelevant content change invalidates trace (aliases.nix pattern) --

TEST_F(TraceStoreTest, NixpkgsMiss_IrrelevantContentChange)
{
    // Synthesized from eval[1] (commit 3f7b5d89ca): aliases.nix renamed
    // ciscoPacketTracer{8,9} -> cisco-packet-tracer_{8,9}. All 32 traces
    // invalidated despite producing the same output.
    //
    // Models: a trace depends on a large shared file (e.g., aliases.nix)
    // where only an irrelevant section changed. Verify fails because the
    // Content dep hash changed. Recovery fails because the exact combination
    // of current dep hashes was never previously recorded.
    ScopedEnvVar relevant("NIX_CATAMISS_REL", "stable_value");
    ScopedEnvVar irrelevant("NIX_CATAMISS_IRREL", "original");

    auto db = makeDb();

    std::vector<Dep> deps = {
        makeEnvVarDep(pools(), "NIX_CATAMISS_REL", "stable_value"),
        makeEnvVarDep(pools(), "NIX_CATAMISS_IRREL", "original"),
    };
    db.record(vpath({"closures"}), string_t{"/nix/store/aaa-closures", {}}, deps);

    // Simulate commit transition: irrelevant dep changes (aliases.nix edit)
    setenv("NIX_CATAMISS_IRREL", "modified", 1);
    db.clearSessionCaches();

    auto result = db.verify(vpath({"closures"}), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: irrelevant dep changed but no prior trace "
           "with matching hash exists (aliases.nix pattern)";
}

TEST_F(TraceStoreTest, NixpkgsHit_RevertAfterIrrelevantChange)
{
    // Synthesized from cold run eval[0]->eval[1]->eval[0]-revert pattern.
    // After reverting aliases.nix to its original content, direct hash
    // recovery finds the original trace in history.
    ScopedEnvVar relevant("NIX_CATAHIT_REL", "stable");
    ScopedEnvVar irrelevant("NIX_CATAHIT_IRREL", "v1");

    auto db = makeDb();

    // Version 1: record trace
    std::vector<Dep> depsV1 = {
        makeEnvVarDep(pools(), "NIX_CATAHIT_REL", "stable"),
        makeEnvVarDep(pools(), "NIX_CATAHIT_IRREL", "v1"),
    };
    db.record(vpath({"closures"}), string_t{"/nix/store/aaa-closures", {}}, depsV1);

    // Version 2: irrelevant change (new aliases.nix content)
    setenv("NIX_CATAHIT_IRREL", "v2", 1);
    std::vector<Dep> depsV2 = {
        makeEnvVarDep(pools(), "NIX_CATAHIT_REL", "stable"),
        makeEnvVarDep(pools(), "NIX_CATAHIT_IRREL", "v2"),
    };
    db.record(vpath({"closures"}), string_t{"/nix/store/aaa-closures", {}}, depsV2);

    // Revert to v1: direct hash recovery should find original trace
    setenv("NIX_CATAHIT_IRREL", "v1", 1);
    db.clearSessionCaches();

    auto result = db.verify(vpath({"closures"}), {}, state);
    ASSERT_TRUE(result.has_value())
        << "Expected cache hit: reverting irrelevant dep should recover "
           "original trace via direct hash";
    assertCachedResultEquals(string_t{"/nix/store/aaa-closures", {}},
                             result->value, state.symbols);
}

// -- Category B: New directory entry invalidates trace (pkgs/by-name pattern) --

TEST_F(TraceStoreTest, NixpkgsMiss_NewDirectoryEntryChangesListing)
{
    // Synthesized from eval[53] (commit abed87246b): pkgs/by-name/fa/fabs/
    // added. The evaluation reads all entries in pkgs/by-name/fa/ (enumerates
    // the directory), so adding a new entry changes the listing hash.
    //
    // Even with per-entry StructuredContent deps, adding a new entry means
    // the coarse Directory dep hash changes. If the evaluation enumerates
    // all directory entries (e.g., via attrNames/mapAttrs on readDir), the
    // new entry constitutes a real change to the dep set.
    //
    // Models: two versions of a directory listing where an entry is added.
    // The trace depends on the old listing hash; the new hash is novel.
    ScopedEnvVar stable("NIX_CATB_STABLE", "unchanged_dep");

    auto db = makeDb();

    // Record trace depending on a "directory listing" (simulated as env var)
    // and a stable dep. The directory listing hash represents the coarse
    // Directory dep from readDir.
    std::vector<Dep> deps = {
        makeEnvVarDep(pools(), "NIX_CATB_STABLE", "unchanged_dep"),
        makeEnvVarDep(pools(), "NIX_CATB_DIR_LISTING", "hash_of_existing1_existing2"),
    };
    ScopedEnvVar dir("NIX_CATB_DIR_LISTING", "hash_of_existing1_existing2");
    db.record(vpath({"closures"}), string_t{"/nix/store/bbb-closures", {}}, deps);

    // Add new entry to directory -> listing hash changes
    setenv("NIX_CATB_DIR_LISTING", "hash_of_existing1_existing2_fabs", 1);
    db.clearSessionCaches();

    auto result = db.verify(vpath({"closures"}), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: directory listing changed (pkgs/by-name "
           "pattern), no prior trace with these dep hashes";
}

// -- Category C: Irrelevant line addition to shared file (python-packages.nix) --

TEST_F(TraceStoreTest, NixpkgsMiss_IrrelevantLineAdded)
{
    // Synthesized from eval[3] (commit 1575c9f64e): av_13 added to
    // python-packages.nix. The evaluation imports this file (Content dep)
    // but doesn't use av_13. Adding a line changes the content hash.
    //
    // Structurally identical to Category A (aliases.nix) -- both are
    // Content dep invalidation from an irrelevant change to a shared file.
    ScopedEnvVar shared("NIX_CATC_SHARED", "original_content_hash");
    ScopedEnvVar own("NIX_CATC_OWN", "my_stable_dep");

    auto db = makeDb();

    std::vector<Dep> deps = {
        makeEnvVarDep(pools(), "NIX_CATC_SHARED", "original_content_hash"),
        makeEnvVarDep(pools(), "NIX_CATC_OWN", "my_stable_dep"),
    };
    db.record(vpath({"closures"}), string_t{"/nix/store/ccc-closures", {}}, deps);

    // Add line to shared file -> content hash changes
    setenv("NIX_CATC_SHARED", "modified_content_hash", 1);
    db.clearSessionCaches();

    auto result = db.verify(vpath({"closures"}), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: shared file content changed (python-packages.nix "
           "pattern), no prior trace with these dep hashes";
}

// -- Category D: Interleaving between output groups --

TEST_F(TraceStoreTest, NixpkgsMiss_InterleavingGroups)
{
    // Synthesized from eval[71] (commit 0d3ea6c6b0): previous eval was
    // eval[70] (Group 8, different output). 12,781 files changed between
    // commits. The traces have different dep structures (different files read).
    //
    // Models: alternating between two commits with very different dep sets.
    // Recovery scans structural variant groups but finds no match because
    // the current dep hashes don't match any recorded trace.
    ScopedEnvVar a("NIX_CATD_A", "a1");
    ScopedEnvVar b("NIX_CATD_B", "b1");
    ScopedEnvVar c("NIX_CATD_C", "c1");

    auto db = makeDb();

    // Group 6 commit: depends on A + B
    std::vector<Dep> group6Deps = {
        makeEnvVarDep(pools(), "NIX_CATD_A", "a1"),
        makeEnvVarDep(pools(), "NIX_CATD_B", "b1"),
    };
    db.record(vpath({"closures"}), string_t{"/nix/store/grp6", {}}, group6Deps);

    // Group 8 commit: depends on A + C (different structural hash)
    std::vector<Dep> group8Deps = {
        makeEnvVarDep(pools(), "NIX_CATD_A", "a1"),
        makeEnvVarDep(pools(), "NIX_CATD_C", "c1"),
    };
    db.record(vpath({"closures"}), string_t{"/nix/store/grp8", {}}, group8Deps);

    // Switch to a third version where A changed and B is different
    setenv("NIX_CATD_A", "a2", 1);
    setenv("NIX_CATD_B", "b2", 1);
    db.clearSessionCaches();

    // Current traces point to group8 (A+C). Verify fails (A changed).
    // Recovery: direct hash with A+C -> no match (a2+c1 not recorded).
    // Structural variant scan: group6 structure (A+B) -> recompute: a2+b2 -> no match.
    // All recovery fails -> must re-evaluate.
    auto result = db.verify(vpath({"closures"}), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: interleaving groups with changed deps, "
           "no matching trace in history";
}

TEST_F(TraceStoreTest, NixpkgsHit_InterleavingRecovery)
{
    // Synthesized from hot run recovery successes: alternating between two
    // commits where the earlier commit's trace is recoverable via structural
    // variant recovery (same dep keys, matching dep values in history).
    ScopedEnvVar a("NIX_CATD2_A", "a1");
    ScopedEnvVar b("NIX_CATD2_B", "b1");

    auto db = makeDb();

    // Version 1: depends on A only
    std::vector<Dep> deps1 = {
        makeEnvVarDep(pools(), "NIX_CATD2_A", "a1"),
    };
    db.record(vpath({"closures"}), string_t{"/nix/store/v1", {}}, deps1);

    // Version 2: depends on A + B (different struct_hash, becomes current)
    std::vector<Dep> deps2 = {
        makeEnvVarDep(pools(), "NIX_CATD2_A", "a1"),
        makeEnvVarDep(pools(), "NIX_CATD2_B", "b1"),
    };
    db.record(vpath({"closures"}), string_t{"/nix/store/v2", {}}, deps2);

    // Change B -> version 2's trace invalid. But version 1 has only A dep.
    setenv("NIX_CATD2_B", "b2", 1);
    db.clearSessionCaches();

    // Recovery: direct hash for A+B (b2) fails.
    // Structural variant: finds group with only A, recomputes -> a1 matches -> recovered!
    auto result = db.verify(vpath({"closures"}), {}, state);
    ASSERT_TRUE(result.has_value())
        << "Expected cache hit: structural variant recovery should find "
           "version 1 trace (only depends on A, which is unchanged)";
    assertCachedResultEquals(string_t{"/nix/store/v1", {}},
                             result->value, state.symbols);
}

// -- Cross-category: multiple irrelevant changes compound --

TEST_F(TraceStoreTest, NixpkgsMiss_MultipleIrrelevantChanges)
{
    // Synthesized from commits where both aliases.nix AND pkgs/by-name/
    // changed simultaneously. Models a trace with multiple deps where
    // two independent irrelevant changes both invalidate the trace.
    ScopedEnvVar d1("NIX_MULTI_DEP1", "stable1");
    ScopedEnvVar d2("NIX_MULTI_DEP2", "stable2");
    ScopedEnvVar irr1("NIX_MULTI_ALIASES", "original");
    ScopedEnvVar irr2("NIX_MULTI_DIRNAME", "original_listing");

    auto db = makeDb();

    std::vector<Dep> deps = {
        makeEnvVarDep(pools(), "NIX_MULTI_DEP1", "stable1"),
        makeEnvVarDep(pools(), "NIX_MULTI_DEP2", "stable2"),
        makeEnvVarDep(pools(), "NIX_MULTI_ALIASES", "original"),
        makeEnvVarDep(pools(), "NIX_MULTI_DIRNAME", "original_listing"),
    };
    db.record(vpath({"closures"}), string_t{"/nix/store/multi", {}}, deps);

    // Both irrelevant deps change simultaneously
    setenv("NIX_MULTI_ALIASES", "renamed_alias", 1);
    setenv("NIX_MULTI_DIRNAME", "listing_with_new_pkg", 1);
    db.clearSessionCaches();

    auto result = db.verify(vpath({"closures"}), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: multiple irrelevant deps changed "
           "simultaneously (aliases.nix + pkgs/by-name pattern)";
}

TEST_F(TraceStoreTest, NixpkgsHit_SameOutputDifferentHistory)
{
    // Synthesized from the observation that 29 unnecessary re-evaluations
    // produced one of only 10 unique outputs. Models the scenario where
    // multiple traces in history all produce the same result value but have
    // different dep hashes. Direct hash recovery succeeds when reverting to
    // a previously-seen dep state.
    ScopedEnvVar common("NIX_HIST_COMMON", "c1");
    ScopedEnvVar varying("NIX_HIST_VARYING", "v1");

    auto db = makeDb();

    // Record 5 versions with same result but different dep hashes
    for (int i = 1; i <= 5; i++) {
        auto v = "v" + std::to_string(i);
        setenv("NIX_HIST_VARYING", v.c_str(), 1);
        std::vector<Dep> deps = {
            makeEnvVarDep(pools(), "NIX_HIST_COMMON", "c1"),
            makeEnvVarDep(pools(), "NIX_HIST_VARYING", v),
        };
        db.record(vpath({"closures"}), string_t{"/nix/store/same-output", {}}, deps);
    }

    // Revert to v2 (previously recorded state)
    setenv("NIX_HIST_VARYING", "v2", 1);
    db.clearSessionCaches();

    // Direct hash recovery finds v2's trace in history
    auto result = db.verify(vpath({"closures"}), {}, state);
    ASSERT_TRUE(result.has_value())
        << "Expected cache hit: reverting to previously-seen dep state "
           "should recover via direct hash lookup";
    assertCachedResultEquals(string_t{"/nix/store/same-output", {}},
                             result->value, state.symbols);
}

TEST_F(TraceStoreTest, NixpkgsMiss_NovelDepState)
{
    // Synthesized from the 29 unnecessary re-evaluations where recovery
    // fails because the exact combination of current dep hashes was never
    // recorded. Even though the result is the same, the dep state is novel.
    ScopedEnvVar common("NIX_NOVEL_COMMON", "c1");
    ScopedEnvVar varying("NIX_NOVEL_VARYING", "v1");

    auto db = makeDb();

    // Record v1 and v2
    std::vector<Dep> depsV1 = {
        makeEnvVarDep(pools(), "NIX_NOVEL_COMMON", "c1"),
        makeEnvVarDep(pools(), "NIX_NOVEL_VARYING", "v1"),
    };
    db.record(vpath({"closures"}), string_t{"/nix/store/same-output", {}}, depsV1);

    setenv("NIX_NOVEL_VARYING", "v2", 1);
    std::vector<Dep> depsV2 = {
        makeEnvVarDep(pools(), "NIX_NOVEL_COMMON", "c1"),
        makeEnvVarDep(pools(), "NIX_NOVEL_VARYING", "v2"),
    };
    db.record(vpath({"closures"}), string_t{"/nix/store/same-output", {}}, depsV2);

    // Set to v3 (never recorded) -- same result would be produced but
    // the dep state is novel, so recovery fails.
    setenv("NIX_NOVEL_VARYING", "v3", 1);
    db.clearSessionCaches();

    auto result = db.verify(vpath({"closures"}), {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: novel dep state (v3) was never recorded, "
           "recovery cannot find matching trace despite same output";
}

} // namespace nix::eval_trace
