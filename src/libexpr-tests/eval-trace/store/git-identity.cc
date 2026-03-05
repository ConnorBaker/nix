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

    auto result = db.record(rootPath(), int_t{NixInt{42}}, deps, true);
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

    auto result = db.record(rootPath(), string_t{"hello", {}}, deps, true);
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

    auto result = db.record(rootPath(), string_t{"original content", {}}, deps, true);
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

    auto result = db.record(rootPath(), null_t{}, deps, true);

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

    auto result = db.record(rootPath(), null_t{}, deps, true);
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

    auto result = db.record(rootPath(), int_t{NixInt{1}}, deps, true);
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

    auto result = db.record(rootPath(), null_t{}, deps, true);
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

    auto result = db.record(rootPath(), null_t{}, deps, true);
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

    auto result = db.record(rootPath(), string_t{"multi-dep test", {}}, deps, true);

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

} // namespace nix::eval_trace
