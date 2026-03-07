#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/store/stat-hash-store.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── GitIdentity dep type descriptor tests ───────────────────────────

TEST_F(TraceStoreTest, GitIdentity_DepKind_IsImplicitStructural)
{
    EXPECT_EQ(depKind(DepType::GitIdentity), DepKind::ImplicitStructural);
}

TEST_F(TraceStoreTest, GitIdentity_Descriptor)
{
    auto desc = describe(DepType::GitIdentity);
    EXPECT_EQ(desc.kind, DepKind::ImplicitStructural);
    EXPECT_TRUE(desc.isBlake3);
    EXPECT_FALSE(desc.isOverrideable);
    EXPECT_FALSE(desc.isVolatile);
}

// ── Round-trip: record → load → verify dep structure ────────────────

TEST_F(TraceStoreTest, GitIdentity_RecordAndLoad_DepStructure)
{
    auto db = makeDb();
    TempTestFile file("42");
    auto filePath = file.path.string();

    auto contentHash = StatHashStore::instance().depHashFile(
        SourcePath(getFSSourceAccessor(), CanonPath(filePath)));

    std::vector<Dep> deps = {
        {{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(filePath)}, contentHash},
        makeGitIdentityDep(pools(), "/tmp/repo", "rev-abc123"),
    };

    auto result = db.record(rootPath(), int_t{NixInt{42}}, deps);
    auto loaded = db.loadFullTrace(result.traceId);
    ASSERT_EQ(loaded.size(), 2u);

    // Verify each dep's type and resolved key
    std::map<DepType, std::string> loadedByType;
    for (auto & d : loaded)
        loadedByType[d.key.type] = std::string(pools().resolve(d.key.keyId));

    EXPECT_EQ(loadedByType.count(DepType::Content), 1u);
    EXPECT_EQ(loadedByType[DepType::Content], filePath);
    EXPECT_EQ(loadedByType.count(DepType::GitIdentity), 1u);
    EXPECT_EQ(loadedByType[DepType::GitIdentity], "/tmp/repo");

    // Verify GitIdentity hash is BLAKE3 of the fingerprint string
    for (auto & d : loaded) {
        if (d.key.type == DepType::GitIdentity) {
            auto expected = depHash("rev-abc123");
            auto * b3 = std::get_if<Blake3Hash>(&d.hash);
            ASSERT_NE(b3, nullptr);
            EXPECT_EQ(*b3, expected);
        }
    }
}

// ── Cross-commit reuse: Content passes, GitIdentity unresolvable ────
//
// The primary cross-commit reuse scenario. Content dep matches (file
// unchanged), GitIdentity dep points to a nonexistent repo so
// resolveCurrentDepHash returns nullopt → gitIdentityMatched stays false.
// Since GitIdentity is ImplicitStructural and not deferred to
// implicitShapeDepIndices, Pass 2 sees no failures → outcome = Valid.

TEST_F(TraceStoreTest, GitIdentity_ContentPasses_GitUnresolvable_Valid)
{
    auto db = makeDb();
    TempTestFile file("hello");
    auto filePath = file.path.string();

    auto contentHash = StatHashStore::instance().depHashFile(
        SourcePath(getFSSourceAccessor(), CanonPath(filePath)));

    std::vector<Dep> deps = {
        {{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(filePath)}, contentHash},
        makeGitIdentityDep(pools(), "/nonexistent/repo", "rev-OLD"),
    };

    auto result = db.record(rootPath(), string_t{"hello", {}}, deps);
    db.verifiedTraceIds.clear();

    bool ok = db.verifyTrace(result.traceId, {}, state);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId))
        << "Verified trace should be session-cached";

    // Also test via verify() — should return the same trace
    db.verifiedTraceIds.clear();
    auto verified = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(verified.has_value());
    EXPECT_EQ(verified->traceId, result.traceId);
}

// ── Content changed → Invalid (no structural override) ──────────────

TEST_F(TraceStoreTest, GitIdentity_ContentChanged_Invalid)
{
    auto db = makeDb();
    TempTestFile file("original content");
    auto filePath = file.path.string();

    auto contentHash = StatHashStore::instance().depHashFile(
        SourcePath(getFSSourceAccessor(), CanonPath(filePath)));

    std::vector<Dep> deps = {
        {{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(filePath)}, contentHash},
        makeGitIdentityDep(pools(), "/nonexistent/repo", "rev-abc"),
    };

    auto result = db.record(rootPath(), string_t{"original content", {}}, deps);
    db.verifiedTraceIds.clear();

    // Modify file so Content dep fails
    file.modify("MODIFIED content");
    getFSSourceAccessor()->invalidateCache(CanonPath(filePath));
    StatHashStore::instance().clear();

    // Content fails, GitIdentity unresolvable, no SC deps → Invalid
    bool ok = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(ok) << "Trace should be invalid when Content dep fails";
    EXPECT_FALSE(db.verifiedTraceIds.count(result.traceId));
}

// ── Volatile dep blocks verification regardless of GitIdentity ──────

TEST_F(TraceStoreTest, GitIdentity_WithVolatile_AlwaysInvalid)
{
    auto db = makeDb();

    std::vector<Dep> deps = {
        makeCurrentTimeDep(pools()),
        makeGitIdentityDep(pools(), "/tmp/repo", "rev-abc123"),
    };

    auto result = db.record(rootPath(), null_t{}, deps);

    // Volatile dep → NOT session-cached at record time
    EXPECT_FALSE(db.verifiedTraceIds.count(result.traceId));

    // Explicit verification also fails
    bool ok = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(ok) << "Volatile dep should block verification regardless of GitIdentity";
    EXPECT_FALSE(db.verifiedTraceIds.count(result.traceId));
}

TEST_F(TraceStoreTest, GitIdentity_WithExec_AlwaysInvalid)
{
    auto db = makeDb();

    std::vector<Dep> deps = {
        makeExecDep(pools()),
        makeGitIdentityDep(pools(), "/tmp/repo", "rev-abc123"),
    };

    auto result = db.record(rootPath(), null_t{}, deps);
    EXPECT_FALSE(db.verifiedTraceIds.count(result.traceId));

    bool ok = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(db.verifiedTraceIds.count(result.traceId));
}

// ── GitIdentity as sole dep → Valid ─────────────────────────────────
//
// Edge case: trace with only a GitIdentity dep. Pass 1 eagerly checks
// it — resolveCurrentDepHash returns nullopt → gitIdentityMatched=false.
// No other deps means no failures of any kind. Fast-path not taken
// (gitIdentityMatched=false). Pass 2: !hasContentFailure, no deferred
// deps → standalonePassed=true → outcome = Valid.

TEST_F(TraceStoreTest, GitIdentity_OnlyDep_Valid)
{
    auto db = makeDb();

    std::vector<Dep> deps = {
        makeGitIdentityDep(pools(), "/nonexistent/repo", "rev-abc"),
    };

    auto result = db.record(rootPath(), int_t{NixInt{1}}, deps);
    db.verifiedTraceIds.clear();

    bool ok = db.verifyTrace(result.traceId, {}, state);
    EXPECT_TRUE(ok) << "GitIdentity-only trace should pass (mismatch is benign)";
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId));
}

// ── Normal dep failure + GitIdentity → Invalid ──────────────────────

TEST_F(TraceStoreTest, GitIdentity_NormalDepFails_Invalid)
{
    auto db = makeDb();

    // EnvVar dep with value that won't match (env var is unset)
    std::vector<Dep> deps = {
        makeEnvVarDep(pools(), "NIX_TEST_GIT_IDENTITY_XYZ", "expected-value"),
        makeGitIdentityDep(pools(), "/nonexistent/repo", "rev-abc"),
    };

    auto result = db.record(rootPath(), null_t{}, deps);
    db.verifiedTraceIds.clear();

    // EnvVar "NIX_TEST_GIT_IDENTITY_XYZ" is unset → hash("") != hash("expected-value")
    bool ok = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(ok) << "Normal dep failure should invalidate regardless of GitIdentity";
    EXPECT_FALSE(db.verifiedTraceIds.count(result.traceId));
}

// ── System dep passes + GitIdentity mismatch → Valid ────────────────

TEST_F(TraceStoreTest, GitIdentity_SystemDepPasses_GitMismatch_Valid)
{
    auto db = makeDb();

    auto systemHash = depHash(state.settings.getCurrentSystem());

    std::vector<Dep> deps = {
        {{DepType::System, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>("")}, systemHash},
        makeGitIdentityDep(pools(), "/nonexistent/repo", "rev-old"),
    };

    auto result = db.record(rootPath(), null_t{}, deps);
    db.verifiedTraceIds.clear();

    bool ok = db.verifyTrace(result.traceId, {}, state);
    EXPECT_TRUE(ok) << "System dep passes, GitIdentity mismatch is benign";
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId));
}

// ── Multiple deps: Content + System + GitIdentity, all pass ─────────

TEST_F(TraceStoreTest, GitIdentity_MultiplePassingDeps_Valid)
{
    auto db = makeDb();
    TempTestFile file("multi-dep test");
    auto filePath = file.path.string();

    auto contentHash = StatHashStore::instance().depHashFile(
        SourcePath(getFSSourceAccessor(), CanonPath(filePath)));
    auto systemHash = depHash(state.settings.getCurrentSystem());

    std::vector<Dep> deps = {
        {{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(filePath)}, contentHash},
        {{DepType::System, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>("")}, systemHash},
        makeGitIdentityDep(pools(), "/nonexistent/repo", "rev-xyz"),
    };

    auto result = db.record(rootPath(), string_t{"multi-dep test", {}}, deps);

    // Verify loaded trace has all 3 deps with correct types
    auto loaded = db.loadFullTrace(result.traceId);
    ASSERT_EQ(loaded.size(), 3u);

    std::set<DepType> types;
    for (auto & d : loaded)
        types.insert(d.key.type);
    EXPECT_TRUE(types.count(DepType::Content));
    EXPECT_TRUE(types.count(DepType::System));
    EXPECT_TRUE(types.count(DepType::GitIdentity));

    // Verification should pass
    db.verifiedTraceIds.clear();
    bool ok = db.verifyTrace(result.traceId, {}, state);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(db.verifiedTraceIds.count(result.traceId));
}

// ── trace_hash stable across GitIdentity changes (ParentContext chain) ──
//
// Two recordings with identical Content+System deps but different GitIdentity
// fingerprints must produce the same trace_hash. This keeps ParentContext
// deps stable: children recorded against commit 1's root trace_hash can
// still be recovered when the root is verified with commit 2's GitIdentity.

TEST_F(TraceStoreTest, GitIdentity_TraceHashStableAcrossCommits)
{
    auto db = makeDb();
    TempTestFile file("stable content");
    auto filePath = file.path.string();

    auto contentHash = StatHashStore::instance().depHashFile(
        SourcePath(getFSSourceAccessor(), CanonPath(filePath)));
    auto systemHash = depHash(state.settings.getCurrentSystem());

    // Record with GitIdentity from "commit 1"
    std::vector<Dep> deps1 = {
        {{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(filePath)}, contentHash},
        {{DepType::System, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>("")}, systemHash},
        makeGitIdentityDep(pools(), "/tmp/repo", "commit-1-rev"),
    };
    auto result1 = db.record(rootPath(), string_t{"stable content", {}}, deps1);

    // Record with GitIdentity from "commit 2" (different fingerprint, same Content+System)
    std::vector<Dep> deps2 = {
        {{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(filePath)}, contentHash},
        {{DepType::System, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>("")}, systemHash},
        makeGitIdentityDep(pools(), "/tmp/repo", "commit-2-rev"),
    };
    auto result2 = db.record(rootPath(), string_t{"stable content", {}}, deps2);

    // Both recordings should produce the same trace_id (same trace_hash)
    EXPECT_EQ(result1.traceId, result2.traceId)
        << "Traces differing only in GitIdentity should dedup to same trace_id";
}

// ── ParentContext recovery across commits ────────────────────────────
//
// Verifies the full scenario: record root + child with commit 1,
// re-record root with commit 2 (deduped), verify child still resolves.

TEST_F(TraceStoreTest, GitIdentity_ParentContextChainSurvivesCommitChange)
{
    auto db = makeDb();
    TempTestFile file("root content");
    auto filePath = file.path.string();

    auto contentHash = StatHashStore::instance().depHashFile(
        SourcePath(getFSSourceAccessor(), CanonPath(filePath)));

    // Record root with commit 1
    std::vector<Dep> rootDeps1 = {
        {{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(filePath)}, contentHash},
        makeGitIdentityDep(pools(), "/tmp/repo", "commit-1"),
    };
    auto rootResult = db.record(rootPath(), string_t{"root content", {}}, rootDeps1);

    // Get root's trace_hash for the ParentContext dep
    auto rootTraceHash = db.getCurrentTraceHash(rootPath());
    ASSERT_TRUE(rootTraceHash.has_value());

    // Record child with ParentContext pointing to root's trace_hash
    auto childPath = vpath({"child"});
    std::vector<Dep> childDeps = {
        makeParentContextDep(rootPath(), *rootTraceHash),
    };
    auto childResult = db.record(childPath, int_t{NixInt{42}}, childDeps);

    // Re-record root with commit 2 (different GitIdentity)
    std::vector<Dep> rootDeps2 = {
        {{DepType::Content, pools().intern<DepSourceId>(""), pools().intern<DepKeyId>(filePath)}, contentHash},
        makeGitIdentityDep(pools(), "/tmp/repo", "commit-2"),
    };
    auto rootResult2 = db.record(rootPath(), string_t{"root content", {}}, rootDeps2);

    // Root trace_hash should be unchanged (GitIdentity excluded from hash)
    auto rootTraceHash2 = db.getCurrentTraceHash(rootPath());
    ASSERT_TRUE(rootTraceHash2.has_value());
    EXPECT_EQ(*rootTraceHash, *rootTraceHash2)
        << "Root trace_hash must be stable across GitIdentity changes";

    // Verify child's trace still passes (ParentContext matches)
    db.verifiedTraceIds.clear();
    bool ok = db.verifyTrace(childResult.traceId, {}, state);
    EXPECT_TRUE(ok) << "Child's ParentContext should still match after root re-recorded with different GitIdentity";
}

} // namespace nix::eval_trace
