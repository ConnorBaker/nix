#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── GitIdentity query behavior tests ────────────────────────────────

TEST_F(TraceStoreTest, GitIdentity_QueryBehavior_IsImplicitStructural)
{
    EXPECT_EQ(queryBehavior(CanonicalQueryKind::GitRevisionIdentity), QueryBehavior::ImplicitStructural);
}

TEST_F(TraceStoreTest, GitIdentity_Descriptor_CorrectFields)
{
    auto desc = describe(CanonicalQueryKind::GitRevisionIdentity);
    EXPECT_EQ(desc.behavior, QueryBehavior::ImplicitStructural);
    EXPECT_EQ(desc.kind, CanonicalQueryKind::GitRevisionIdentity);
    EXPECT_EQ(queryKindName(CanonicalQueryKind::GitRevisionIdentity), "gitRevisionIdentity");
    EXPECT_TRUE(queryDomainContains(desc.observedDomains, QueryDomain::Identity));
    EXPECT_EQ(repoRootAddressingKind(CanonicalQueryKind::GitRevisionIdentity), RepoRootAddressingKind::None);
    EXPECT_TRUE(desc.isDigest);
    EXPECT_FALSE(desc.isOverrideable);
    EXPECT_FALSE(desc.isVolatile);
}

// ── Round-trip: record → load → verify dep structure ────────────────

TEST_F(TraceStoreTest, GitIdentity_RecordAndLoad_DepStructure)
{
    auto db = makeDb();
    TempTestFile file("42");
    auto filePath = file.path.string();

    auto contentHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());

    std::vector<Dep> deps = {
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), filePath, contentHash),
        makeGitIdentityDep(pools(), "/tmp/repo", "rev-abc123"),
    };

    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), int_t{NixInt{42}}, deps);
        auto loaded = db->loadFullTrace(ea, result.traceId);
        ASSERT_EQ(loaded->size(), 2u);

        // Verify each dep's type and resolved key
        std::map<CanonicalQueryKind, std::string> loadedByType;
        for (const auto & d : *loaded)
            loadedByType[d.key.kind] = depKeyDisplayForTest(pools(), d);

        EXPECT_EQ(loadedByType.count(CanonicalQueryKind::FileBytes), 1u);
        EXPECT_EQ(loadedByType[CanonicalQueryKind::FileBytes], filePath);
        EXPECT_EQ(loadedByType.count(CanonicalQueryKind::GitRevisionIdentity), 1u);
        EXPECT_EQ(loadedByType[CanonicalQueryKind::GitRevisionIdentity], "/tmp/repo");

        // Verify GitIdentity hash is the active eval-trace digest of the
        // fingerprint string.
        for (const auto & d : *loaded) {
            if (d.key.kind == CanonicalQueryKind::GitRevisionIdentity) {
                auto expected = depHash("rev-abc123");
                auto * b3 = std::get_if<DepHash>(&d.hash);
                ASSERT_NE(b3, nullptr);
                EXPECT_EQ(*b3, expected);
            }
        }
    });
}

// ── Cross-commit reuse: Content passes, GitIdentity unresolvable ────
//
// The primary cross-commit reuse scenario. Content dep matches (file
// unchanged). GitIdentity is recorded for session identity/history but
// primary verification ignores it, so Pass 2 sees no failures → outcome =
// Valid.

TEST_F(TraceStoreTest, GitIdentity_ContentPasses_GitUnresolvable_Valid)
{
    auto db = makeDb();
    TempTestFile file("hello");
    auto filePath = file.path.string();

    auto contentHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());

    std::vector<Dep> deps = {
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), filePath, contentHash),
        makeGitIdentityDep(pools(), "/nonexistent/repo", "rev-OLD"),
    };

    TraceId traceId;
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), string_t{"hello", {}}, deps);
        traceId = result.traceId;
    });
    VerificationSession verifyTraceSession;
    bool ok = test::TraceStorageTestAccess::verifyTrace(*db, traceId, state, verifyTraceSession);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(verifyTraceSession.verifiedTraceIds.count(traceId))
        << "Verified trace should be session-cached";

    // Also test via verify() — should return the same trace
    VerificationSession verifyPathSession;
    auto verified = test::TraceStorageTestAccess::verify(*db, rootPath(), state, verifyPathSession);
    ASSERT_TRUE(verified.has_value());
    EXPECT_EQ(verified->traceId, traceId);
}

// ── Content changed → Invalid (no structural override) ──────────────

TEST_F(TraceStoreTest, GitIdentity_ContentChanged_Invalid)
{
    auto db = makeDb();
    TempTestFile file("original content");
    auto filePath = file.path.string();

    auto contentHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());

    std::vector<Dep> deps = {
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), filePath, contentHash),
        makeGitIdentityDep(pools(), "/nonexistent/repo", "rev-abc"),
    };

    TraceId traceId;
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), string_t{"original content", {}}, deps);
        traceId = result.traceId;
    });

    // Modify file so Content dep fails
    file.modify("MODIFIED content");
    getFSSourceAccessor()->invalidateCache();

    // Content fails, GitIdentity unresolvable, no SC deps → Invalid
    VerificationSession session;
    bool ok = test::TraceStorageTestAccess::verifyTrace(*db, traceId, state, session);
    EXPECT_FALSE(ok) << "Trace should be invalid when Content dep fails";
    EXPECT_FALSE(session.verifiedTraceIds.count(traceId));
}

// ── Volatile dep blocks verification regardless of GitIdentity ──────

TEST_F(TraceStoreTest, GitIdentity_WithVolatile_AlwaysInvalid)
{
    auto db = makeDb();

    std::vector<Dep> deps = {
        makeCurrentTimeDep(pools()),
        makeGitIdentityDep(pools(), "/tmp/repo", "rev-abc123"),
    };

    TraceId traceId;
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), makeNull(), deps);
        traceId = result.traceId;
    });

    // Explicit verification also fails
    VerificationSession session;
    bool ok = test::TraceStorageTestAccess::verifyTrace(*db, traceId, state, session);
    EXPECT_FALSE(ok) << "Volatile dep should block verification regardless of GitIdentity";
    EXPECT_FALSE(session.verifiedTraceIds.count(traceId));
}

TEST_F(TraceStoreTest, GitIdentity_WithExec_AlwaysInvalid)
{
    auto db = makeDb();

    std::vector<Dep> deps = {
        makeExecDep(pools()),
        makeGitIdentityDep(pools(), "/tmp/repo", "rev-abc123"),
    };

    TraceId traceId;
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), makeNull(), deps);
        traceId = result.traceId;
    });

    VerificationSession session;
    bool ok = test::TraceStorageTestAccess::verifyTrace(*db, traceId, state, session);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(session.verifiedTraceIds.count(traceId));
}

// ── GitIdentity as sole dep → Valid ─────────────────────────────────
//
// Edge case: trace with only a GitIdentity dep. Primary verification ignores
// it. No other deps means no failures of any kind; Pass 2 returns Valid.

TEST_F(TraceStoreTest, GitIdentity_OnlyDep_Valid)
{
    auto db = makeDb();

    std::vector<Dep> deps = {
        makeGitIdentityDep(pools(), "/nonexistent/repo", "rev-abc"),
    };

    TraceId traceId;
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), int_t{NixInt{1}}, deps);
        traceId = result.traceId;
    });
    VerificationSession session;
    bool ok = test::TraceStorageTestAccess::verifyTrace(*db, traceId, state, session);
    EXPECT_TRUE(ok) << "GitIdentity-only trace should pass (mismatch is benign)";
    EXPECT_TRUE(session.verifiedTraceIds.count(traceId));
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

    TraceId traceId;
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), makeNull(), deps);
        traceId = result.traceId;
    });

    // EnvVar "NIX_TEST_GIT_IDENTITY_XYZ" is unset → hash("") != hash("expected-value")
    VerificationSession session;
    bool ok = test::TraceStorageTestAccess::verifyTrace(*db, traceId, state, session);
    EXPECT_FALSE(ok) << "Normal dep failure should invalidate regardless of GitIdentity";
    EXPECT_FALSE(session.verifiedTraceIds.count(traceId));
}

// ── System dep passes + GitIdentity mismatch → Valid ────────────────

TEST_F(TraceStoreTest, GitIdentity_SystemDepPasses_GitMismatch_Valid)
{
    auto db = makeDb();

    auto systemHash = depHash(state.settings.getCurrentSystem());

    std::vector<Dep> deps = {
        makeSimpleRecordedDep(
            pools(), CanonicalQueryKind::SessionSystemValue, DepSource::makeAbsolute(), "", systemHash),
        makeGitIdentityDep(pools(), "/nonexistent/repo", "rev-old"),
    };

    TraceId traceId;
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), makeNull(), deps);
        traceId = result.traceId;
    });

    VerificationSession session;
    bool ok = test::TraceStorageTestAccess::verifyTrace(*db, traceId, state, session);
    EXPECT_TRUE(ok) << "System dep passes, GitIdentity mismatch is benign";
    EXPECT_TRUE(session.verifiedTraceIds.count(traceId));
}

// ── Multiple deps: Content + System + GitIdentity, all pass ─────────

TEST_F(TraceStoreTest, GitIdentity_MultiplePassingDeps_Valid)
{
    auto db = makeDb();
    TempTestFile file("multi-dep test");
    auto filePath = file.path.string();

    auto contentHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
    auto systemHash = depHash(state.settings.getCurrentSystem());

    std::vector<Dep> deps = {
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), filePath, contentHash),
        makeSimpleRecordedDep(
            pools(), CanonicalQueryKind::SessionSystemValue, DepSource::makeAbsolute(), "", systemHash),
        makeGitIdentityDep(pools(), "/nonexistent/repo", "rev-xyz"),
    };

    TraceId traceId;
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), string_t{"multi-dep test", {}}, deps);
        traceId = result.traceId;

        // Verify loaded trace has all 3 deps with correct types
        auto loaded = db->loadFullTrace(ea, result.traceId);
        ASSERT_EQ(loaded->size(), 3u);

        std::set<CanonicalQueryKind> types;
        for (const auto & d : *loaded)
            types.insert(d.key.kind);
        EXPECT_TRUE(types.count(CanonicalQueryKind::FileBytes));
        EXPECT_TRUE(types.count(CanonicalQueryKind::SessionSystemValue));
        EXPECT_TRUE(types.count(CanonicalQueryKind::GitRevisionIdentity));
    });

    // Verification should pass
    VerificationSession session;
    bool ok = test::TraceStorageTestAccess::verifyTrace(*db, traceId, state, session);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(session.verifiedTraceIds.count(traceId));
}

// ── trace_hash stable across GitIdentity changes (trace-context chain) ──
//
// Two recordings with identical Content+System deps but different GitIdentity
// fingerprints must produce the same canonical trace_hash. Stored TraceIds
// remain exact: the full trace hash includes implicit structure deps, so the
// two full dep vectors are not deduplicated. The canonical trace_hash is what
// keeps trace-context deps stable across GitIdentity changes.

TEST_F(TraceStoreTest, GitIdentity_CrossCommit_TraceHashStable)
{
    auto db = makeDb();
    TempTestFile file("stable content");
    auto filePath = file.path.string();

    auto contentHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
    auto systemHash = depHash(state.settings.getCurrentSystem());

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record with GitIdentity from "commit 1"
        std::vector<Dep> deps1 = {
            makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), filePath, contentHash),
            makeSimpleRecordedDep(
                pools(), CanonicalQueryKind::SessionSystemValue, DepSource::makeAbsolute(), "", systemHash),
            makeGitIdentityDep(pools(), "/tmp/repo", "commit-1-rev"),
        };
        auto result1 = db->record(ea, rootPath(), string_t{"stable content", {}}, deps1);
        auto traceHash1 = db->getCurrentTraceHash(ea, rootPath());
        ASSERT_TRUE(traceHash1.has_value());

        // Record with GitIdentity from "commit 2" (different fingerprint, same Content+System)
        std::vector<Dep> deps2 = {
            makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), filePath, contentHash),
            makeSimpleRecordedDep(
                pools(), CanonicalQueryKind::SessionSystemValue, DepSource::makeAbsolute(), "", systemHash),
            makeGitIdentityDep(pools(), "/tmp/repo", "commit-2-rev"),
        };
        auto result2 = db->record(ea, rootPath(), string_t{"stable content", {}}, deps2);
        auto traceHash2 = db->getCurrentTraceHash(ea, rootPath());
        ASSERT_TRUE(traceHash2.has_value());

        ASSERT_NE(result1.traceId, result2.traceId)
            << "Traces differing only in GitIdentity must remain exact stored traces";

        EXPECT_EQ(*traceHash1, *traceHash2)
            << "Traces differing only in GitIdentity should share the canonical recovery trace_hash";
    });
}

} // namespace nix::eval_trace
