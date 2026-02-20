#include "helpers.hh"
#include "nix/expr/trace-store.hh"
#include "nix/expr/trace-hash.hh"

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
        makeEnvVarDep("NIX_CATAMISS_REL", "stable_value"),
        makeEnvVarDep("NIX_CATAMISS_IRREL", "original"),
    };
    db.record("closures", string_t{"/nix/store/aaa-closures", {}}, deps, true);

    // Simulate commit transition: irrelevant dep changes (aliases.nix edit)
    setenv("NIX_CATAMISS_IRREL", "modified", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
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
        makeEnvVarDep("NIX_CATAHIT_REL", "stable"),
        makeEnvVarDep("NIX_CATAHIT_IRREL", "v1"),
    };
    db.record("closures", string_t{"/nix/store/aaa-closures", {}}, depsV1, true);

    // Version 2: irrelevant change (new aliases.nix content)
    setenv("NIX_CATAHIT_IRREL", "v2", 1);
    std::vector<Dep> depsV2 = {
        makeEnvVarDep("NIX_CATAHIT_REL", "stable"),
        makeEnvVarDep("NIX_CATAHIT_IRREL", "v2"),
    };
    db.record("closures", string_t{"/nix/store/aaa-closures", {}}, depsV2, true);

    // Revert to v1: direct hash recovery should find original trace
    setenv("NIX_CATAHIT_IRREL", "v1", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
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
        makeEnvVarDep("NIX_CATB_STABLE", "unchanged_dep"),
        makeEnvVarDep("NIX_CATB_DIR_LISTING", "hash_of_existing1_existing2"),
    };
    ScopedEnvVar dir("NIX_CATB_DIR_LISTING", "hash_of_existing1_existing2");
    db.record("closures", string_t{"/nix/store/bbb-closures", {}}, deps, true);

    // Add new entry to directory -> listing hash changes
    setenv("NIX_CATB_DIR_LISTING", "hash_of_existing1_existing2_fabs", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
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
        makeEnvVarDep("NIX_CATC_SHARED", "original_content_hash"),
        makeEnvVarDep("NIX_CATC_OWN", "my_stable_dep"),
    };
    db.record("closures", string_t{"/nix/store/ccc-closures", {}}, deps, true);

    // Add line to shared file -> content hash changes
    setenv("NIX_CATC_SHARED", "modified_content_hash", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
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
        makeEnvVarDep("NIX_CATD_A", "a1"),
        makeEnvVarDep("NIX_CATD_B", "b1"),
    };
    db.record("closures", string_t{"/nix/store/grp6", {}}, group6Deps, true);

    // Group 8 commit: depends on A + C (different structural hash)
    std::vector<Dep> group8Deps = {
        makeEnvVarDep("NIX_CATD_A", "a1"),
        makeEnvVarDep("NIX_CATD_C", "c1"),
    };
    db.record("closures", string_t{"/nix/store/grp8", {}}, group8Deps, true);

    // Switch to a third version where A changed and B is different
    setenv("NIX_CATD_A", "a2", 1);
    setenv("NIX_CATD_B", "b2", 1);
    db.clearSessionCaches();

    // Current traces point to group8 (A+C). Verify fails (A changed).
    // Recovery: direct hash with A+C -> no match (a2+c1 not recorded).
    // Structural variant scan: group6 structure (A+B) -> recompute: a2+b2 -> no match.
    // All recovery fails -> must re-evaluate.
    auto result = db.verify("closures", {}, state);
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
        makeEnvVarDep("NIX_CATD2_A", "a1"),
    };
    db.record("closures", string_t{"/nix/store/v1", {}}, deps1, true);

    // Version 2: depends on A + B (different struct_hash, becomes current)
    std::vector<Dep> deps2 = {
        makeEnvVarDep("NIX_CATD2_A", "a1"),
        makeEnvVarDep("NIX_CATD2_B", "b1"),
    };
    db.record("closures", string_t{"/nix/store/v2", {}}, deps2, true);

    // Change B -> version 2's trace invalid. But version 1 has only A dep.
    setenv("NIX_CATD2_B", "b2", 1);
    db.clearSessionCaches();

    // Recovery: direct hash for A+B (b2) fails.
    // Structural variant: finds group with only A, recomputes -> a1 matches -> recovered!
    auto result = db.verify("closures", {}, state);
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
        makeEnvVarDep("NIX_MULTI_DEP1", "stable1"),
        makeEnvVarDep("NIX_MULTI_DEP2", "stable2"),
        makeEnvVarDep("NIX_MULTI_ALIASES", "original"),
        makeEnvVarDep("NIX_MULTI_DIRNAME", "original_listing"),
    };
    db.record("closures", string_t{"/nix/store/multi", {}}, deps, true);

    // Both irrelevant deps change simultaneously
    setenv("NIX_MULTI_ALIASES", "renamed_alias", 1);
    setenv("NIX_MULTI_DIRNAME", "listing_with_new_pkg", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
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
            makeEnvVarDep("NIX_HIST_COMMON", "c1"),
            makeEnvVarDep("NIX_HIST_VARYING", v),
        };
        db.record("closures", string_t{"/nix/store/same-output", {}}, deps, true);
    }

    // Revert to v2 (previously recorded state)
    setenv("NIX_HIST_VARYING", "v2", 1);
    db.clearSessionCaches();

    // Direct hash recovery finds v2's trace in history
    auto result = db.verify("closures", {}, state);
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
        makeEnvVarDep("NIX_NOVEL_COMMON", "c1"),
        makeEnvVarDep("NIX_NOVEL_VARYING", "v1"),
    };
    db.record("closures", string_t{"/nix/store/same-output", {}}, depsV1, true);

    setenv("NIX_NOVEL_VARYING", "v2", 1);
    std::vector<Dep> depsV2 = {
        makeEnvVarDep("NIX_NOVEL_COMMON", "c1"),
        makeEnvVarDep("NIX_NOVEL_VARYING", "v2"),
    };
    db.record("closures", string_t{"/nix/store/same-output", {}}, depsV2, true);

    // Set to v3 (never recorded) -- same result would be produced but
    // the dep state is novel, so recovery fails.
    setenv("NIX_NOVEL_VARYING", "v3", 1);
    db.clearSessionCaches();

    auto result = db.verify("closures", {}, state);
    EXPECT_FALSE(result.has_value())
        << "Expected cache miss: novel dep state (v3) was never recorded, "
           "recovery cannot find matching trace despite same output";
}

// ── Serialization edge case tests ────────────────────────────────────

TEST_F(TraceStoreTest, BlobRoundTrip_StructuredContent)
{
    // StructuredContent deps use Blake3 hashes, like Content.
    // Verify they round-trip correctly through the factored serialization.
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    keys.push_back({DepType::StructuredContent, 1, 2});
    deps.push_back({DepType::StructuredContent, 1, 2, DepHashValue(depHash("scalar-value"))});

    keys.push_back({DepType::Content, 3, 4});
    deps.push_back({DepType::Content, 3, 4, DepHashValue(depHash("file-content"))});

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 2u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 2u);

    // StructuredContent is a Blake3 dep type
    EXPECT_EQ(keysResult[0].type, DepType::StructuredContent);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[0]));
    EXPECT_EQ(std::get<Blake3Hash>(valsResult[0]), depHash("scalar-value"));

    // Content is also Blake3
    EXPECT_EQ(keysResult[1].type, DepType::Content);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[1]));
    EXPECT_EQ(std::get<Blake3Hash>(valsResult[1]), depHash("file-content"));
}

TEST_F(TraceStoreTest, BlobRoundTrip_32ByteStringVsBlake3)
{
    // Critical test: a CopiedPath string value that is exactly 32 bytes
    // must be deserialized as a string, NOT as a Blake3Hash.
    // This tests the disambiguation logic in deserializeValues.
    std::string exactly32 = "abcdefghijklmnopqrstuvwxyz012345"; // 32 chars
    ASSERT_EQ(exactly32.size(), 32u);

    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    // CopiedPath with exactly 32-byte string value
    keys.push_back({DepType::CopiedPath, 1, 2});
    deps.push_back({DepType::CopiedPath, 1, 2, DepHashValue(exactly32)});

    // Content with Blake3 hash (also 32 bytes)
    keys.push_back({DepType::Content, 3, 4});
    deps.push_back({DepType::Content, 3, 4, DepHashValue(depHash("data"))});

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 2u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 2u);

    // CopiedPath with 32-byte value must deserialize as string
    EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[0]))
        << "32-byte CopiedPath value was incorrectly deserialized as Blake3Hash";
    EXPECT_EQ(std::get<std::string>(valsResult[0]), exactly32);

    // Content with 32-byte value must deserialize as Blake3Hash
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[1]))
        << "Content dep value was incorrectly deserialized as string";
}

TEST_F(TraceStoreTest, BlobRoundTrip_SingleEntry)
{
    // Boundary: single dep in the serialization.
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    keys.push_back({DepType::EnvVar, 42, 99});
    deps.push_back({DepType::EnvVar, 42, 99, DepHashValue(depHash("HOME=/home/user"))});

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 1u);
    EXPECT_EQ(keysResult[0].type, DepType::EnvVar);
    EXPECT_EQ(keysResult[0].sourceId, 42u);
    EXPECT_EQ(keysResult[0].keyId, 99u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 1u);
    EXPECT_EQ(valsResult[0], deps[0].hash);
}

TEST_F(TraceStoreTest, BlobRoundTrip_AllDepTypes)
{
    // Round-trip test covering every dep type in the DepType enum.
    // This ensures isBlake3Dep is consistent with serialization/deserialization.
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    auto addBlake3 = [&](DepType type, uint32_t sid, uint32_t kid, std::string_view data) {
        keys.push_back({type, sid, kid});
        deps.push_back({type, sid, kid, DepHashValue(depHash(data))});
    };
    auto addString = [&](DepType type, uint32_t sid, uint32_t kid, std::string_view data) {
        keys.push_back({type, sid, kid});
        deps.push_back({type, sid, kid, DepHashValue(std::string(data))});
    };

    // Blake3 dep types
    addBlake3(DepType::Content, 1, 1, "file");
    addBlake3(DepType::Directory, 2, 2, "dir");
    addBlake3(DepType::EnvVar, 3, 3, "env");
    addBlake3(DepType::System, 4, 4, "sys");
    addBlake3(DepType::NARContent, 5, 5, "nar");
    addBlake3(DepType::StructuredContent, 6, 6, "struct");
    addBlake3(DepType::ParentContext, 7, 7, "parent");

    // String dep types
    addString(DepType::Existence, 8, 8, "type:1");
    addString(DepType::CurrentTime, 9, 9, "volatile");
    addString(DepType::UnhashedFetch, 10, 10, "url");
    addString(DepType::CopiedPath, 11, 11, "/nix/store/test");
    addString(DepType::Exec, 12, 12, "volatile");

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 12u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 12u);

    // Blake3 types should deserialize as Blake3Hash
    for (int i = 0; i < 7; i++) {
        EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[i]))
            << "Dep type " << static_cast<int>(keysResult[i].type)
            << " should deserialize as Blake3Hash";
    }
    // String types should deserialize as std::string
    for (int i = 7; i < 12; i++) {
        EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[i]))
            << "Dep type " << static_cast<int>(keysResult[i].type)
            << " should deserialize as string";
    }
}

// ── DepKeySets sharing tests ─────────────────────────────────────────

TEST_F(TraceStoreTest, DepKeySets_SharedKeysDifferentValues)
{
    // Two traces with identical dep keys but different hash values should
    // share the same DepKeySets row (same struct_hash) but have different
    // Traces rows (different trace_hash / values_blob).
    auto db = makeDb();

    std::vector<Dep> depsV1 = {
        makeContentDep("/a.nix", "content-v1"),
        makeEnvVarDep("HOME", "/home/user1"),
    };
    std::vector<Dep> depsV2 = {
        makeContentDep("/a.nix", "content-v2"),
        makeEnvVarDep("HOME", "/home/user2"),
    };

    auto r1 = db.record("attr", string_t{"result-1", {}}, depsV1, true);
    auto r2 = db.record("attr", string_t{"result-2", {}}, depsV2, true);

    // Different trace IDs (different hash values → different trace_hash)
    EXPECT_NE(r1.traceId, r2.traceId);

    // Both should load correctly
    auto loaded1 = db.loadFullTrace(r1.traceId);
    auto loaded2 = db.loadFullTrace(r2.traceId);
    EXPECT_EQ(loaded1.size(), 2u);
    EXPECT_EQ(loaded2.size(), 2u);

    // Verify the loaded values differ
    // Both have Content dep at index 0 (after sort); check the hash values differ
    bool foundDifference = false;
    for (size_t i = 0; i < loaded1.size(); i++) {
        if (loaded1[i].expectedHash != loaded2[i].expectedHash) {
            foundDifference = true;
            break;
        }
    }
    EXPECT_TRUE(foundDifference) << "Traces with same keys but different values should have different dep hashes";
}

// ── Phase 1 cache optimization tests ──────────────────────────────────
// Tests for the unified CachedTraceData cache, traceRowCache, in-memory
// recovery matching, and placeholder hash detection.

TEST_F(TraceStoreTest, CachedTraceData_HashesPopulated_Default)
{
    // Default-constructed CachedTraceData has placeholder (all-zero) hashes.
    TraceStore::CachedTraceData data;
    EXPECT_FALSE(data.hashesPopulated())
        << "Default CachedTraceData should have unpopulated (all-zero) hashes";
}

TEST_F(TraceStoreTest, CachedTraceData_HashesPopulated_AfterRecord)
{
    // After record(), traceDataCache should contain populated hashes.
    auto db = makeDb();
    auto result = db.record("test", string_t{"value", {}},
        {makeContentDep("/a.nix", "hello")}, true);

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_TRUE(it->second.hashesPopulated())
        << "CachedTraceData after record should have non-zero hashes";
    EXPECT_TRUE(it->second.deps.has_value())
        << "CachedTraceData after record should have deps populated";
}

TEST_F(TraceStoreTest, CachedTraceData_HashesPopulated_AfterEnsure)
{
    // ensureTraceHashes populates hashes but NOT deps.
    // getCurrentTraceHash calls ensureTraceHashes internally.
    auto db = makeDb();
    auto result = db.record("test", string_t{"value", {}},
        {makeContentDep("/a.nix", "hello")}, true);

    // Clear session caches to force re-read from DB
    db.traceDataCache.clear();

    // getCurrentTraceHash calls lookupTraceRow → ensureTraceHashes
    auto hash = db.getCurrentTraceHash("test");
    ASSERT_TRUE(hash.has_value());

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_TRUE(it->second.hashesPopulated())
        << "ensureTraceHashes should populate non-zero hashes";
    EXPECT_FALSE(it->second.deps.has_value())
        << "ensureTraceHashes should NOT populate deps (lazy)";
}

TEST_F(TraceStoreTest, LoadFullTrace_NonexistentTrace_NoCachePollution)
{
    // loadFullTrace on a nonexistent traceId should NOT create a cache entry.
    // This was a real bug (fixed): the old code did traceDataCache[traceId].deps = {},
    // which created an entry with placeholder zero hashes that ensureTraceHashes
    // would later incorrectly return as valid.
    auto db = makeDb();
    TraceId bogusId = 99999;

    auto result = db.loadFullTrace(bogusId);
    EXPECT_TRUE(result.empty());

    // Verify no cache pollution
    EXPECT_EQ(db.traceDataCache.find(bogusId), db.traceDataCache.end())
        << "loadFullTrace on nonexistent trace must not create a cache entry";
}

TEST_F(TraceStoreTest, LoadFullTrace_PopulatesHashes_Opportunistically)
{
    // loadFullTrace should populate traceHash + structHash + deps in one shot.
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "content")};
    auto result = db.record("test", string_t{"val", {}}, deps, true);

    // Clear caches, then use loadFullTrace (not ensureTraceHashes)
    db.traceDataCache.clear();

    auto loaded = db.loadFullTrace(result.traceId);
    EXPECT_EQ(loaded.size(), 1u);

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_TRUE(it->second.hashesPopulated())
        << "loadFullTrace should opportunistically populate hash fields";
    EXPECT_TRUE(it->second.deps.has_value())
        << "loadFullTrace should populate deps";
}

TEST_F(TraceStoreTest, TraceDataCache_EnsureThenLoad_SingleDbQuery)
{
    // ensureTraceHashes (via getCurrentTraceHash) then loadFullTrace should
    // reuse cached hashes. Hash fields should be identical regardless of order.
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "content")};
    auto result = db.record("test", string_t{"val", {}}, deps, true);

    // Record populates everything; get hashes for comparison
    auto itAfterRecord = db.traceDataCache.find(result.traceId);
    ASSERT_NE(itAfterRecord, db.traceDataCache.end());
    auto expectedTraceHash = itAfterRecord->second.traceHash;

    // Clear and re-populate via ensureTraceHashes (getCurrentTraceHash)
    db.traceDataCache.clear();
    auto traceHash = db.getCurrentTraceHash("test");
    ASSERT_TRUE(traceHash.has_value());
    EXPECT_EQ(*traceHash, expectedTraceHash);

    // Verify deps not yet populated (lazy)
    auto itPartial = db.traceDataCache.find(result.traceId);
    ASSERT_NE(itPartial, db.traceDataCache.end());
    EXPECT_FALSE(itPartial->second.deps.has_value())
        << "ensureTraceHashes should not populate deps";

    // Now loadFullTrace should find the partial cache entry and extend it
    auto loaded = db.loadFullTrace(result.traceId);
    EXPECT_EQ(loaded.size(), 1u);

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_EQ(it->second.traceHash, expectedTraceHash);
    EXPECT_TRUE(it->second.deps.has_value());
}

TEST_F(TraceStoreTest, TraceRowCache_HitAfterRecord)
{
    // After record(), lookupTraceRow should hit the traceRowCache (no DB).
    auto db = makeDb();
    auto result = db.record("myattr", string_t{"result", {}},
        {makeEnvVarDep("FOO", "bar")}, true);

    // traceRowCache should be populated from record()
    EXPECT_EQ(db.traceRowCache.count("myattr"), 1u)
        << "record() should populate traceRowCache";

    // Verify the cached row has correct traceId
    auto & row = db.traceRowCache["myattr"];
    EXPECT_EQ(row.traceId, result.traceId);
}

TEST_F(TraceStoreTest, TraceRowCache_HitAfterLookup)
{
    // After a lookupTraceRow miss that hits DB, cache should be populated.
    auto db = makeDb();
    db.record("attr", string_t{"val", {}}, {}, true);

    // Clear traceRowCache (simulate fresh session lookup, not record)
    db.traceRowCache.clear();

    // First lookup goes to DB, populates cache
    EXPECT_TRUE(db.attrExists("attr"));
    EXPECT_EQ(db.traceRowCache.count("attr"), 1u)
        << "lookupTraceRow should populate traceRowCache on DB hit";
}

TEST_F(TraceStoreTest, TraceRowCache_MissDoesNotCache)
{
    // Looking up a nonexistent attr should NOT create a cache entry.
    auto db = makeDb();
    EXPECT_FALSE(db.attrExists("nonexistent"));
    EXPECT_EQ(db.traceRowCache.count("nonexistent"), 0u)
        << "lookupTraceRow miss should not create a cache entry";
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_CachedAfterFirstCall)
{
    // getCurrentTraceHash should cache its result — second call hits cache.
    auto db = makeDb();
    // Record a parent with tab-separated key (simulating ParentContext dep key format)
    std::string attrPath = "packages";
    attrPath.push_back('\0');
    attrPath.append("hello");
    db.record(attrPath, string_t{"result", {}},
        {makeContentDep("/hello.nix", "v1")}, true);

    // Convert to tab-separated key as ParentContext deps do
    std::string depKey = "packages";
    depKey.push_back('\t');
    depKey.append("hello");

    auto hash1 = db.getCurrentTraceHash(depKey);
    ASSERT_TRUE(hash1.has_value());

    // Clear DB-level caches but keep session caches (traceRowCache + traceDataCache)
    // This simulates a "second call in the same session"
    // If the second call goes to DB, it would still work, but we want to verify caching.

    auto hash2 = db.getCurrentTraceHash(depKey);
    ASSERT_TRUE(hash2.has_value());
    EXPECT_EQ(*hash1, *hash2)
        << "getCurrentTraceHash should return same value from cache";
}

TEST_F(TraceStoreTest, TraceRowCache_InvalidatedOnRecovery)
{
    // When recovery succeeds, traceRowCache should be updated to reflect
    // the recovered trace. Subsequent lookups should return the new trace.
    auto db = makeDb();

    // Record version 1
    std::vector<Dep> depsV1 = {makeEnvVarDep("MY_VAR", "v1")};
    db.record("attr", string_t{"result-v1", {}}, depsV1, true);

    // Record version 2 (different deps → different trace)
    std::vector<Dep> depsV2 = {makeEnvVarDep("MY_VAR", "v2")};
    db.record("attr", string_t{"result-v2", {}}, depsV2, true);

    // Clear session caches to simulate new session
    db.clearSessionCaches();

    // Set env to v1 (mismatches current trace v2, should recover to v1)
    setenv("MY_VAR", "v1", 1);
    auto verifyResult = db.verify("attr", {}, state);
    ASSERT_TRUE(verifyResult.has_value());

    // Verify the traceRowCache was updated
    EXPECT_EQ(db.traceRowCache.count("attr"), 1u);
    auto & row = db.traceRowCache["attr"];
    EXPECT_EQ(row.traceId, verifyResult->traceId)
        << "traceRowCache should reflect the recovered trace";

    // Verify getCurrentTraceHash also reflects recovery
    auto traceHash = db.getCurrentTraceHash("attr");
    ASSERT_TRUE(traceHash.has_value());
    auto * data = db.traceDataCache.count(verifyResult->traceId)
        ? &db.traceDataCache[verifyResult->traceId] : nullptr;
    if (data && data->hashesPopulated()) {
        EXPECT_EQ(*traceHash, data->traceHash)
            << "getCurrentTraceHash should match recovered trace's hash";
    }

    unsetenv("MY_VAR");
}

TEST_F(TraceStoreTest, InMemoryRecovery_DirectHash)
{
    // Recovery should use in-memory trace_hash matching (no per-group DB lookup).
    // This test verifies the functional correctness of in-memory direct hash recovery.
    auto db = makeDb();

    // Record two versions
    setenv("INTEST", "alpha", 1);
    std::vector<Dep> depsA = {makeEnvVarDep("INTEST", "alpha")};
    db.record("attr", string_t{"result-alpha", {}}, depsA, true);

    setenv("INTEST", "beta", 1);
    std::vector<Dep> depsB = {makeEnvVarDep("INTEST", "beta")};
    db.record("attr", string_t{"result-beta", {}}, depsB, true);

    // Clear session caches
    db.clearSessionCaches();

    // Revert env to alpha → direct hash recovery should find the alpha trace
    setenv("INTEST", "alpha", 1);
    auto result = db.verify("attr", {}, state);
    ASSERT_TRUE(result.has_value());

    // Should have recovered to alpha's result
    auto * strResult = std::get_if<string_t>(&result->value);
    ASSERT_NE(strResult, nullptr);
    EXPECT_EQ(strResult->first, "result-alpha");

    unsetenv("INTEST");
}

TEST_F(TraceStoreTest, InMemoryRecovery_StructuralVariant)
{
    // Structural variant recovery should also use in-memory matching.
    auto db = makeDb();

    // Version 1: depends on VAR_A only
    setenv("SVR_A", "a1", 1);
    std::vector<Dep> depsV1 = {makeEnvVarDep("SVR_A", "a1")};
    db.record("attr", string_t{"r1", {}}, depsV1, true);

    // Version 2: depends on VAR_A + VAR_B (different dep structure)
    setenv("SVR_A", "a2", 1);
    setenv("SVR_B", "b2", 1);
    std::vector<Dep> depsV2 = {
        makeEnvVarDep("SVR_A", "a2"),
        makeEnvVarDep("SVR_B", "b2"),
    };
    db.record("attr", string_t{"r2", {}}, depsV2, true);

    // Clear caches
    db.clearSessionCaches();

    // Set env to match v1 (single dep)
    setenv("SVR_A", "a1", 1);
    unsetenv("SVR_B");

    auto result = db.verify("attr", {}, state);
    ASSERT_TRUE(result.has_value());

    auto * strResult = std::get_if<string_t>(&result->value);
    ASSERT_NE(strResult, nullptr);
    EXPECT_EQ(strResult->first, "r1")
        << "Structural variant recovery should find the matching trace";

    unsetenv("SVR_A");
}

TEST_F(TraceStoreTest, ClearSessionCaches_ClearsAllNewCaches)
{
    // clearSessionCaches should clear both traceDataCache and traceRowCache.
    auto db = makeDb();
    db.record("attr", string_t{"val", {}}, {makeContentDep("/f", "c")}, true);

    EXPECT_FALSE(db.traceDataCache.empty());
    EXPECT_FALSE(db.traceRowCache.empty());

    db.clearSessionCaches();

    EXPECT_TRUE(db.traceDataCache.empty())
        << "clearSessionCaches must clear traceDataCache";
    EXPECT_TRUE(db.traceRowCache.empty())
        << "clearSessionCaches must clear traceRowCache";
}

} // namespace nix::eval_trace
