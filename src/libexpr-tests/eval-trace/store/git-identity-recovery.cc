/**
 * Tests around GitIdentity-indexed recovery and adjacent session-key behavior.
 *
 * The real GitIdentity recovery path requires a trace whose own deps include
 * a GitRevisionIdentity dep. Several child/descendant fixtures below record
 * GitIdentity on the root only; those exercise primary CurrentNodes lookup
 * under matching semantic session keys rather than GitIdentity-indexed child
 * recovery. The later root-level tests force lookupHistoryByGitIdentity.
 *
 * sessionConfig is set on SqliteTraceStorage at session creation (eagerly,
 * before any verify/record calls). All traces in the session share it.
 * Each makeDb() scope simulates a separate evaluation session
 * (SqliteTraceStorage destructor commits the transaction).
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/counters.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Matching-session child lookup ───────────────────────────────────

TEST_F(TraceStoreTest, CurrentNode_ChildTrace_CrossCommitSessionKeyHit)
{
    auto & v = state.vocabStore();
    auto childPath = v.internPath(rootPath(), v.internName("child"));

    TempTestFile fileA("content-a");
    TempTestFile fileB("content-b");
    auto pathA = std::filesystem::canonical(fileA.path).string();
    auto pathB = std::filesystem::canonical(fileB.path).string();

    DepHash hashA, hashB;

    // Session 1: commit-1
    {
        auto db = makeDb();
        hashA = depHash(SourcePath(getFSSourceAccessor(), CanonPath(pathA)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-1").value));
        withExclusiveStore(*db, [&](const auto & ea) {
            std::vector<Dep> rootDeps = {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), pathA, hashA),
                makeGitIdentityDep(pools(), "/tmp/repo", "commit-1"),
            };
            db->record(ea, rootPath(), string_t{"root-A", {}}, rootDeps);

            std::vector<Dep> childDeps = {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), pathA, hashA),
            };
            db->record(ea, childPath, string_t{"child-A", {}}, childDeps);
        });
    }

    // Session 2: commit-2
    {
        auto db = makeDb();
        hashB = depHash(SourcePath(getFSSourceAccessor(), CanonPath(pathB)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-2").value));
        withExclusiveStore(*db, [&](const auto & ea) {
            std::vector<Dep> rootDeps = {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), pathB, hashB),
                makeGitIdentityDep(pools(), "/tmp/repo", "commit-2"),
            };
            db->record(ea, rootPath(), string_t{"root-B", {}}, rootDeps);

            std::vector<Dep> childDeps = {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), pathB, hashB),
            };
            db->record(ea, childPath, string_t{"child-B", {}}, childDeps);
        });
    }

    // Modify fileB so commit-2's traces fail
    fileB.modify("MODIFIED");
    getFSSourceAccessor()->invalidateCache();

    // Session 3: same semantic key as session 1, so this should hit the
    // persisted CurrentNodes row directly. The child trace has no
    // GitRevisionIdentity dep of its own, so it cannot exercise
    // tryGitIdentityRecovery.
    {
        auto snap = PathCountersSnapshot{};
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-1").value));
        auto recovered = test::TraceStorageTestAccess::verify(*db, childPath, state);

        ASSERT_TRUE(recovered.has_value())
            << "Should verify child trace from commit-1";
        EXPECT_EQ(std::get<string_t>(recovered->value).first, "child-A");

        EXPECT_EQ(snap.deltaHistoryBootstraps(), 0)
            << "matching semantic session key should not need history bootstrap";
        EXPECT_EQ(snap.deltaRecoveryAttempts(), 0)
            << "matching semantic session key should not enter recovery";
        EXPECT_EQ(snap.deltaRecoveryGitIdentityHits(), 0)
            << "child trace has no GitRevisionIdentity dep, so GitIdentity recovery is not exercised";
        EXPECT_EQ(snap.deltaRecoveryStructVariantHits(), 0)
            << "matching-session child lookup must not require structural-variant scanning";
        EXPECT_EQ(snap.deltaRecoveryDirectHashHits(), 0)
            << "matching-session child lookup must not fall through to direct-hash recovery";
    }
}

// ── Test 3: Three commits, recover to middle ────────────────────────

TEST_F(TraceStoreTest, CurrentNode_ThreeCommits_SelectsMatchingSession)
{
    auto & v = state.vocabStore();
    auto childPath = v.internPath(rootPath(), v.internName("child"));

    TempTestFile file1("content-1");
    TempTestFile file2("content-2");
    TempTestFile file3("content-3");
    auto path1 = std::filesystem::canonical(file1.path).string();
    auto path2 = std::filesystem::canonical(file2.path).string();
    auto path3 = std::filesystem::canonical(file3.path).string();

    DepHash hash1, hash2, hash3;

    // Session 1: commit-1
    {
        auto db = makeDb();
        hash1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(path1)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-1").value));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"root-1", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path1, hash1),
                makeGitIdentityDep(pools(), "/tmp/repo", "commit-1"),
            });
            db->record(ea, childPath, string_t{"child-1", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path1, hash1),
            });
        });
    }

    // Session 2: commit-2
    {
        auto db = makeDb();
        hash2 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(path2)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-2").value));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"root-2", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path2, hash2),
                makeGitIdentityDep(pools(), "/tmp/repo", "commit-2"),
            });
            db->record(ea, childPath, string_t{"child-2", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path2, hash2),
            });
        });
    }

    // Session 3: commit-3
    {
        auto db = makeDb();
        hash3 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(path3)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-3").value));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"root-3", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path3, hash3),
                makeGitIdentityDep(pools(), "/tmp/repo", "commit-3"),
            });
            db->record(ea, childPath, string_t{"child-3", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path3, hash3),
            });
        });
    }

    // Modify file3 so commit-3's traces fail
    file3.modify("MODIFIED");
    getFSSourceAccessor()->invalidateCache();

    // Same semantic key as session 2, so lookupCurrentNode should select
    // the middle session's persisted row.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-2").value));
        auto result = test::TraceStorageTestAccess::verify(*db, childPath, state);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(std::get<string_t>(result->value).first, "child-2");
    }
}

// ── Multiple children under a matching semantic session ─────────────

TEST_F(TraceStoreTest, CurrentNode_MultipleChildren_MatchingSessionAllVerify)
{
    auto & v = state.vocabStore();
    auto childA = v.internPath(rootPath(), v.internName("childA"));
    auto childB = v.internPath(rootPath(), v.internName("childB"));
    auto childC = v.internPath(rootPath(), v.internName("childC"));

    TempTestFile file1("content-1");
    TempTestFile file2("content-2");
    auto path1 = std::filesystem::canonical(file1.path).string();
    auto path2 = std::filesystem::canonical(file2.path).string();

    DepHash hash1, hash2;

    // Session 1: commit-1
    {
        auto db = makeDb();
        hash1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(path1)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-1").value));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"root-1", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path1, hash1),
                makeGitIdentityDep(pools(), "/tmp/repo", "commit-1"),
            });
            auto contentKey = Dep::Key::makeSimple(
                CanonicalQueryKind::FileBytes,
                pools().intern<DepSourceId>(DepSource::makeAbsolute()),
                pools().intern(SimpleDepKeyAtom{path1}));
            db->record(ea, childA, string_t{"childA-1", {}}, {{contentKey, hash1}});
            db->record(ea, childB, string_t{"childB-1", {}}, {{contentKey, hash1}});
            db->record(ea, childC, string_t{"childC-1", {}}, {{contentKey, hash1}});
        });
    }

    // Session 2: commit-2
    {
        auto db = makeDb();
        hash2 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(path2)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-2").value));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"root-2", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path2, hash2),
                makeGitIdentityDep(pools(), "/tmp/repo", "commit-2"),
            });
            auto contentKey = Dep::Key::makeSimple(
                CanonicalQueryKind::FileBytes,
                pools().intern<DepSourceId>(DepSource::makeAbsolute()),
                pools().intern(SimpleDepKeyAtom{path2}));
            db->record(ea, childA, string_t{"childA-2", {}}, {{contentKey, hash2}});
            db->record(ea, childB, string_t{"childB-2", {}}, {{contentKey, hash2}});
            db->record(ea, childC, string_t{"childC-2", {}}, {{contentKey, hash2}});
        });
    }

    // Break commit-2's file
    file2.modify("MODIFIED");
    getFSSourceAccessor()->invalidateCache();

    // All 3 children should verify under commit-1's semantic session key.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-1").value));

        auto resultA = test::TraceStorageTestAccess::verify(*db, childA, state);
        ASSERT_TRUE(resultA.has_value());
        EXPECT_EQ(std::get<string_t>(resultA->value).first, "childA-1");

        auto resultB = test::TraceStorageTestAccess::verify(*db, childB, state);
        ASSERT_TRUE(resultB.has_value());
        EXPECT_EQ(std::get<string_t>(resultB->value).first, "childB-1");

        auto resultC = test::TraceStorageTestAccess::verify(*db, childC, state);
        ASSERT_TRUE(resultC.has_value());
        EXPECT_EQ(std::get<string_t>(resultC->value).first, "childC-1");
    }
}

// ── Grandchild under a matching semantic session ────────────────────

TEST_F(TraceStoreTest, CurrentNode_Grandchild_MatchingSessionVerifies)
{
    auto & v = state.vocabStore();
    auto childPath = v.internPath(rootPath(), v.internName("child"));
    auto grandchildPath = v.internPath(childPath, v.internName("grandchild"));

    TempTestFile file1("content-1");
    TempTestFile file2("content-2");
    auto path1 = std::filesystem::canonical(file1.path).string();
    auto path2 = std::filesystem::canonical(file2.path).string();

    DepHash hash1, hash2;

    // Session 1: commit-1, root → child → grandchild
    {
        auto db = makeDb();
        hash1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(path1)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-1").value));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"root-1", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path1, hash1),
                makeGitIdentityDep(pools(), "/tmp/repo", "commit-1"),
            });
            db->record(ea, childPath, string_t{"child-1", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path1, hash1),
            });
            db->record(ea, grandchildPath, string_t{"grandchild-1", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path1, hash1),
            });
        });
    }

    // Session 2: commit-2
    {
        auto db = makeDb();
        hash2 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(path2)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-2").value));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"root-2", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path2, hash2),
                makeGitIdentityDep(pools(), "/tmp/repo", "commit-2"),
            });
            db->record(ea, childPath, string_t{"child-2", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path2, hash2),
            });
            db->record(ea, grandchildPath, string_t{"grandchild-2", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path2, hash2),
            });
        });
    }

    // Break commit-2's file
    file2.modify("MODIFIED");
    getFSSourceAccessor()->invalidateCache();

    // Grandchild should verify under commit-1's semantic session key.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("commit-1").value));
        auto result = test::TraceStorageTestAccess::verify(*db, grandchildPath, state);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(std::get<string_t>(result->value).first, "grandchild-1");
    }
}

TEST_F(TraceStoreTest, GitIdentityRecovery_RootAndChild_BothVerify)
{
    auto db = makeDb();
    db->setSessionConfig(SessionConfig::forTest(depHash("rev-123").value));
    auto & v = state.vocabStore();
    auto childPath = v.internPath(rootPath(), v.internName("child"));

    TempTestFile file("content");
    auto filePath = std::filesystem::canonical(file.path).string();

    auto contentHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());

    withExclusiveStore(*db, [&](const auto & ea) {
        // Root trace with GitIdentity
        std::vector<Dep> rootDeps = {
            makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), filePath, contentHash),
            makeGitIdentityDep(pools(), "/tmp/repo", "rev-123"),
        };
        db->record(ea, rootPath(), string_t{"root-result", {}}, rootDeps);

        // Child trace without GitIdentity (inherits root's hash via session)
        std::vector<Dep> childDeps = {
            makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), filePath, contentHash),
        };
        db->record(ea, childPath, string_t{"child-result", {}}, childDeps);
    });

    // Both should verify fine
    VerificationSession session;
    session.gitIdentityCache[pools().intern<RepoRootId>("/tmp/repo")] =
        std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{depHash("rev-123").value});
    auto rootVerified = test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);
    ASSERT_TRUE(rootVerified.has_value());

    auto childVerified = test::TraceStorageTestAccess::verify(*db, childPath, state, session);
    ASSERT_TRUE(childVerified.has_value());
}

TEST_F(TraceStoreTest, GitIdentityRecovery_Hash_UsesActiveDigestOfFingerprint)
{
    auto db = makeDb();
    TempTestFile file("content");
    auto filePath = std::filesystem::canonical(file.path).string();

    auto contentHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());

    std::string fingerprint = "test-fingerprint-123";
    db->setSessionConfig(SessionConfig::forTest(depHash(fingerprint).value));

    std::vector<Dep> deps = {
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), filePath, contentHash),
        makeGitIdentityDep(pools(), "/tmp/repo", fingerprint),
    };

    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), string_t{"result", {}}, deps);

        // Verify the GitIdentity dep hash is stored correctly
        auto loaded = db->loadFullTrace(ea, result.traceId);
        bool foundGitDep = false;
        for (const auto & d : *loaded) {
            if (d.key.kind == CanonicalQueryKind::GitRevisionIdentity) {
                auto * digest = std::get_if<DepHash>(&d.hash);
                ASSERT_NE(digest, nullptr);
                EXPECT_EQ(*digest, depHash(fingerprint));
                foundGitDep = true;
            }
        }
        EXPECT_TRUE(foundGitDep);
    });
}

// ── Cross-session history bootstrap via stable recovery key ─────────
//
// When the graph digest changes (e.g. `nix flake update dep`), the semantic
// session key changes. Traces recorded under the old session key are not found
// by lookupCurrentNode. History bootstrap uses the stable recovery key
// (version-independent flake identity) to find old traces, then verifyTrace
// checks their deps against current content. If deps are unchanged, the
// trace is recovered.

TEST_F(TraceStoreTest, Recovery_CrossSessionGraphChange_UnchangedDepsHit)
{
    auto & v = state.vocabStore();
    auto childPath = v.internPath(rootPath(), v.internName("child"));

    TempTestFile fileA("content-a");
    auto pathA = std::filesystem::canonical(fileA.path).string();

    DepHash hashA;

    // Session 1: record root+child at "graph version 1".
    // Use an explicit stableRecoveryKey shared across sessions.
    {
        auto db = makeDb();
        hashA = depHash(SourcePath(getFSSourceAccessor(), CanonPath(pathA)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("graph-v1").value, "flake-stable:test-flake"));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"root-1", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), pathA, hashA),
                makeGitIdentityDep(pools(), "/tmp/repo", "graph-v1"),
            });
            db->record(ea, childPath, string_t{"child-1", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), pathA, hashA),
            });
        });
    }

    // Session 2: different graph version (simulates `nix flake update dep`),
    // same stableRecoveryKey (same flake URL), file unchanged.
    // lookupCurrentNode misses (different semantic key).
    // History bootstrap finds the trace from session 1 via stableRecoveryKey.
    // verifyTrace passes (fileA unchanged).
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("graph-v2").value, "flake-stable:test-flake"));

        auto result = test::TraceStorageTestAccess::verify(*db, childPath, state);
        ASSERT_TRUE(result.has_value())
            << "Cross-session recovery should succeed when deps are unchanged";
        EXPECT_EQ(std::get<string_t>(result->value).first, "child-1");
    }
}

TEST_F(TraceStoreTest, Recovery_CrossSessionGraphChange_ChangedDepsMiss)
{
    auto & v = state.vocabStore();
    auto childPath = v.internPath(rootPath(), v.internName("child"));

    TempTestFile fileA("content-a");
    auto pathA = std::filesystem::canonical(fileA.path).string();

    // Session 1: record at graph-v1.
    {
        auto db = makeDb();
        auto hashA = depHash(SourcePath(getFSSourceAccessor(), CanonPath(pathA)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("graph-v1").value, "flake-stable:test-flake-2"));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, childPath, string_t{"child-1", {}}, {
                makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), pathA, hashA),
            });
        });
    }

    // Modify the file — deps are now stale.
    fileA.modify("MODIFIED");
    getFSSourceAccessor()->invalidateCache();

    // Session 2: different graph, same stable key, but file changed.
    // History bootstrap finds the trace, but verifyTrace fails (hash mismatch).
    // Recovery also fails (no matching history entry for new hash).
    // Result: nullopt — correct, stale result NOT served.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("graph-v2").value, "flake-stable:test-flake-2"));

        auto result = test::TraceStorageTestAccess::verify(*db, childPath, state);
        EXPECT_FALSE(result.has_value())
            << "Cross-session recovery must NOT serve stale results when deps changed";
    }
}

// ── BUG-1 regression: stale stored hash not served after revision change ──
//
// BUG-1: tryGitIdentityRecovery previously used the hash stored in the old
// trace's deps (stale) instead of the current hash from gitIdentityCache.
// Fixed by rewriting to use extractGitRepoRoot + allDepsGitRecoverable +
// cache lookup.
//
// This test simulates the bug: record a trace under hashA, then start a new
// session with gitIdentityCache populated with hashB ≠ hashA. The query uses
// hashB which does not match the stored hashA → recovery must return nullopt.

TEST_F(TraceStoreTest, GitIdentityRecovery_StaleHash_NotUsedAfterRevisionChange)
{
    // Use the temp directory as the git repo root so allDepsGitRecoverable
    // returns true, isolating the failure to the hash-mismatch check (BUG-1).
    auto tmpBase = std::filesystem::canonical(std::filesystem::temp_directory_path()) / "nix-test-eval-trace";
    createDirs(tmpBase);
    auto repoRoot = std::filesystem::canonical(tmpBase).string();

    auto db = makeDb();
    db->setSessionConfig(SessionConfig::forTest(depHash("rev-A").value));

    TempTestFile file("content");
    auto filePath = std::filesystem::canonical(file.path).string();
    auto contentHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());

    // Record a trace with gitIdentityHash = depHash("rev-A"), file under repoRoot.
    // Attach governingRepoId so allDepsGitRecoverable can match the file dep
    // against the GitIdentity dep's repo.
    std::vector<Dep> deps = {
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), filePath, contentHash, repoRoot),
        makeGitIdentityDep(pools(), repoRoot, "rev-A"),
    };
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result-A", {}}, deps);
    });

    // Start a new session (recreateDb flushes writes, clears in-memory caches)
    recreateDb(db);
    db->setSessionConfig(SessionConfig::forTest(depHash("rev-B").value));

    // Modify the file so direct verification fails — forcing the recovery path
    file.modify("modified content");
    getFSSourceAccessor()->invalidateCache();

    // Populate gitIdentityCache with hashB (different from stored hashA).
    // BUG-1 would use stored hashA for the SQL query; the fix uses current hashB.
    // Current hashB ≠ stored hashA → lookupHistoryByGitIdentity finds no match.
    VerificationSession session;
    session.gitIdentityCache[pools().intern<RepoRootId>(repoRoot)] =
        std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{depHash("rev-B").value});

    // Recovery must return nullopt: current hash B ≠ stored hash A
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);
    EXPECT_FALSE(result.has_value())
        << "Recovery must not serve stale result when current hash differs from stored hash";
}

// ── Current hash matches stored hash: recovery succeeds ─────────────

TEST_F(TraceStoreTest, GitIdentityRecovery_CurrentHash_UsedForRecovery)
{
    // Use the temp directory as the git repo root so allDepsGitRecoverable
    // returns true for the file dep, letting the hash-match check decide.
    auto tmpBase = std::filesystem::canonical(std::filesystem::temp_directory_path()) / "nix-test-eval-trace";
    createDirs(tmpBase);
    auto repoRoot = std::filesystem::canonical(tmpBase).string();

    auto db = makeDb();
    db->setSessionConfig(SessionConfig::forTest(depHash("rev-A").value));

    TempTestFile file("content");
    auto filePath = std::filesystem::canonical(file.path).string();
    auto contentHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());

    // Record a trace with gitIdentityHash = depHash("rev-A"), file under repoRoot.
    // Attach governingRepoId so allDepsGitRecoverable can match.
    std::vector<Dep> deps = {
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), filePath, contentHash, repoRoot),
        makeGitIdentityDep(pools(), repoRoot, "rev-A"),
    };
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result-A", {}}, deps);
    });

    // Start a new session
    recreateDb(db);
    db->setSessionConfig(SessionConfig::forTest(depHash("rev-A").value));

    // Modify the file so direct verification fails — forcing the recovery path
    file.modify("modified content");
    getFSSourceAccessor()->invalidateCache();

    // Populate gitIdentityCache with hashA (same as stored).
    // The fix uses this current hash for the SQL query, finding the stored entry.
    VerificationSession session;
    session.gitIdentityCache[pools().intern<RepoRootId>(repoRoot)] =
        std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{depHash("rev-A").value});

    // Recovery must succeed: current hash A == stored hash A
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);
    ASSERT_TRUE(result.has_value())
        << "Recovery must succeed when current hash matches stored hash";
    EXPECT_EQ(std::get<string_t>(result->value).first, "result-A");
}

TEST_F(TraceStoreTest, GitIdentityRecovery_RejectsFailingImplicitStructureGuard)
{
    TempDir repoDir;
    repoDir.addFile("tracked.json", R"({"value": 1})");
    auto repoRoot = std::filesystem::canonical(repoDir.path()).string();
    auto filePath = std::filesystem::canonical(repoDir.path() / "tracked.json").string();
    auto gitHash = depHash("rev-stable");

    auto implicitGuard = makeStructuredDepForTest(
        pools(), CanonicalQueryKind::ImplicitStructure,
        DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
        {}, DepHashValue(sentinel(SentinelHash::Object)), ShapeSuffix::Type);
    implicitGuard.key.governingRepoId = pools().intern<RepoRootId>(repoRoot);

    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(gitHash.value));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"guarded", {}}, {
                implicitGuard,
                makeGitIdentityDep(pools(), repoRoot, "rev-stable"),
            });
        });
    }

    repoDir.addFile("tracked.json", R"([1, 2, 3])");
    getFSSourceAccessor()->invalidateCache();

    auto db = makeDb();
    db->setSessionConfig(SessionConfig::forTest(gitHash.value));
    VerificationSession session;
    session.gitIdentityCache[pools().intern<RepoRootId>(repoRoot)] =
        std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{gitHash.value});

    PathCountersSnapshot snap;
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);
    EXPECT_FALSE(result.has_value())
        << "GitIdentity recovery must not serve a trace whose implicit structure guard now fails";
    EXPECT_EQ(snap.deltaRecoveryGitIdentityHits(), 0)
        << "a rejected implicit guard is not a GitIdentity recovery hit";
}

TEST_F(TraceStoreTest, GitIdentityRecovery_SkipsFailingImplicitGuardAndUsesOlderCandidate)
{
    TempDir repoDir;
    repoDir.addFile("tracked.json", R"({"value": 1})");
    auto repoRoot = std::filesystem::canonical(repoDir.path()).string();
    auto filePath = std::filesystem::canonical(repoDir.path() / "tracked.json").string();
    auto gitHash = depHash("rev-stable");
    auto repoId = pools().intern<RepoRootId>(repoRoot);

    auto makeGuard = [&](const DepHash & shapeHash) {
        auto dep = makeStructuredDepForTest(
            pools(), CanonicalQueryKind::ImplicitStructure,
            DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
            {}, DepHashValue(shapeHash), ShapeSuffix::Type);
        dep.key.governingRepoId = repoId;
        return dep;
    };

    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(gitHash.value));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"object", {}}, {
                makeGuard(sentinel(SentinelHash::Object)),
                makeGitIdentityDep(pools(), repoRoot, "rev-stable"),
            });
        });
    }

    repoDir.addFile("tracked.json", R"([1, 2, 3])");
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(gitHash.value));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"array", {}}, {
                makeGuard(sentinel(SentinelHash::Array)),
                makeGitIdentityDep(pools(), repoRoot, "rev-stable"),
            });
        });
    }

    repoDir.addFile("tracked.json", R"({"value": 2})");
    getFSSourceAccessor()->invalidateCache();

    auto db = makeDb();
    db->setSessionConfig(SessionConfig::forTest(gitHash.value));
    VerificationSession session;
    session.gitIdentityCache[repoId] =
        std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{gitHash.value});

    PathCountersSnapshot snap;
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);
    ASSERT_TRUE(result.has_value())
        << "GitIdentity recovery should skip the newest candidate if its implicit guard fails";
    EXPECT_EQ(std::get<string_t>(result->value).first, "object");
    EXPECT_EQ(snap.deltaRecoveryGitIdentityHits(), 1);
}

// ── ORDER BY DESC: multiple traces, recovery returns latest ──────────
//
// BUG-2: lookupHistoryByGitIdentity had no ORDER BY clause.
// Fixed by scanning ORDER BY h.trace_id DESC and accepting the first candidate
// whose guards still hold.
//
// Both recorded traces share the same git identity hash "rev-stable" and are
// git-recoverable (all FileBytes deps are under the repo root). After modifying
// both files so direct verification fails, tryGitIdentityRecovery fires.
// lookupHistoryByGitIdentity with hash "rev-stable" finds both history entries;
// the newest valid candidate must be v2 (most recently recorded).

TEST_F(TraceStoreTest, GitIdentityRecovery_MultipleTraces_OrderByReturnsLatest)
{
    // Use the temp directory as the git repo root so that all file deps
    // pass allDepsGitRecoverable (they are under this prefix).
    auto tmpBase = std::filesystem::canonical(std::filesystem::temp_directory_path()) / "nix-test-eval-trace";
    createDirs(tmpBase);
    auto repoRoot = std::filesystem::canonical(tmpBase).string();

    TempTestFile file1("content-v1");
    TempTestFile file2("content-v2");
    auto path1 = std::filesystem::canonical(file1.path).string();
    auto path2 = std::filesystem::canonical(file2.path).string();

    DepHash hash1, hash2;

    // Session 1: record trace with result "v1"
    {
        auto db = makeDb();
        hash1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(path1)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("rev-stable").value));
        std::vector<Dep> deps = {
            makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path1, hash1, repoRoot),
            makeGitIdentityDep(pools(), repoRoot, "rev-stable"),
        };
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v1", {}}, deps);
        });
    }

    // Session 2: record trace with result "v2" — same git hash, higher trace_id
    {
        auto db = makeDb();
        hash2 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(path2)).readFile());
        db->setSessionConfig(SessionConfig::forTest(depHash("rev-stable").value));
        std::vector<Dep> deps = {
            makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), path2, hash2, repoRoot),
            makeGitIdentityDep(pools(), repoRoot, "rev-stable"),
        };
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"v2", {}}, deps);
        });
    }

    // Modify both files to break direct verification for both traces
    file1.modify("modified-v1");
    file2.modify("modified-v2");
    getFSSourceAccessor()->invalidateCache();
    getFSSourceAccessor()->invalidateCache();

    // Session 3: gitIdentityCache has "rev-stable", triggering tryGitIdentityRecovery
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("rev-stable").value));

        VerificationSession session;
        session.gitIdentityCache[pools().intern<RepoRootId>(repoRoot)] =
            std::optional<CurrentGitIdentityHash>(CurrentGitIdentityHash{depHash("rev-stable").value});

        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);
        ASSERT_TRUE(result.has_value())
            << "GitIdentity recovery should succeed via lookupHistoryByGitIdentity";
        EXPECT_EQ(std::get<string_t>(result->value).first, "v2")
            << "newest-first GitIdentity recovery should return the most recently recorded valid trace";
    }
}

// ── Phantom type distinction: StoredGitIdentityHash ≠ CurrentGitIdentityHash ──
//
// Compile-time test verifying the phantom types are distinct and mutually
// non-convertible. This is the type-level guard that prevents BUG-1.

TEST(GitIdentityPhantomTypes, GitIdentity_PhantomTypes_DistinctFromEachOther)
{
    // Distinct types even though both wrap EvalTraceHash
    static_assert(!std::is_same_v<StoredGitIdentityHash, CurrentGitIdentityHash>);

    // Mutually non-convertible — passing one where the other is expected is a compile error
    static_assert(!std::is_convertible_v<StoredGitIdentityHash, CurrentGitIdentityHash>);
    static_assert(!std::is_convertible_v<CurrentGitIdentityHash, StoredGitIdentityHash>);

    // Both are distinct from their underlying type (no implicit unwrapping)
    static_assert(!std::is_same_v<StoredGitIdentityHash, EvalTraceHash>);
    static_assert(!std::is_same_v<CurrentGitIdentityHash, EvalTraceHash>);

    // Runtime smoke check: same EvalTraceHash value, different wrapper types
    EvalTraceHash underlying = depHash("test-rev").value;
    StoredGitIdentityHash stored{underlying};
    CurrentGitIdentityHash current{underlying};
    EXPECT_EQ(stored.value, current.value)
        << "Both wrappers hold the same EvalTraceHash value when constructed identically";
}

// ── extractGoverningRepoId: returns repo id from GitRevisionIdentity dep ──

TEST_F(TraceStoreTest, GitIdentityRecovery_ExtractGoverningRepoId_ReturnsFirstDep)
{
    auto db = makeDb();

    // Deps with a GitRevisionIdentity dep for "/repo/root"
    std::vector<Dep> depsWithGit = {
        makeSimpleRecordedDep(
            pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), "/repo/root/file.nix",
            depHash("file-content")),
        makeGitIdentityDep(pools(), "/repo/root", "rev-abc"),
    };

    auto repoId = db->extractGoverningRepoId(depsWithGit);
    ASSERT_TRUE(repoId.has_value());
    EXPECT_EQ(*repoId, pools().intern<RepoRootId>("/repo/root"));

    // Deps with no GitRevisionIdentity dep → returns nullopt
    std::vector<Dep> depsNoGit = {
        makeSimpleRecordedDep(
            pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), "/some/file.nix",
            depHash("file-content")),
    };

    auto noId = db->extractGoverningRepoId(depsNoGit);
    EXPECT_FALSE(noId.has_value())
        << "extractGoverningRepoId should return nullopt when no GitRevisionIdentity dep is present";
}

// ── allDepsGitRecoverable: volatile dep makes recovery impossible ─────

TEST_F(TraceStoreTest, AllDepsGitRecoverable_VolatileDep_ReturnsFalse)
{
    auto db = makeDb();
    auto repoId = pools().intern<RepoRootId>("/repo/root");

    // Deps with a VolatileExec dep: not recoverable
    std::vector<Dep> depsWithVolatile = {
        makeSimpleRecordedDep(
            pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), "/repo/root/file.nix",
            depHash("file-content"), "/repo/root"),
        makeGitIdentityDep(pools(), "/repo/root", "rev-abc"),
        makeExecDep(pools()),
    };

    EXPECT_FALSE(db->allDepsGitRecoverable(depsWithVolatile, repoId))
        << "allDepsGitRecoverable must return false when a VolatileExec dep is present";

    // Same structure with VolatileTime is also unrecoverable
    std::vector<Dep> depsWithTime = {
        makeGitIdentityDep(pools(), "/repo/root", "rev-abc"),
        makeCurrentTimeDep(pools()),
    };

    EXPECT_FALSE(db->allDepsGitRecoverable(depsWithTime, repoId))
        << "allDepsGitRecoverable must return false when a VolatileTime dep is present";
}

// ── allDepsGitRecoverable: file dep outside repo root returns false ───

TEST_F(TraceStoreTest, AllDepsGitRecoverable_FileOutsideRoot_ReturnsFalse)
{
    auto db = makeDb();
    auto repoId = pools().intern<RepoRootId>("/repo/root");

    // File dep with governingRepoId pointing at a DIFFERENT repo → not recoverable.
    std::vector<Dep> depsOutside = {
        makeSimpleRecordedDep(
            pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), "/other/path/file.nix",
            depHash("file-content"), "/other/path"),
        makeGitIdentityDep(pools(), "/repo/root", "rev-abc"),
    };

    EXPECT_FALSE(db->allDepsGitRecoverable(depsOutside, repoId))
        << "allDepsGitRecoverable must return false when a FileBytes dep's governingRepoId does not match the target repo";

    // File dep with governingRepoId == target → recoverable.
    std::vector<Dep> depsInside = {
        makeSimpleRecordedDep(
            pools(), CanonicalQueryKind::FileBytes, DepSource::makeAbsolute(), "/repo/root/file.nix",
            depHash("file-content"), "/repo/root"),
        makeGitIdentityDep(pools(), "/repo/root", "rev-abc"),
    };

    EXPECT_TRUE(db->allDepsGitRecoverable(depsInside, repoId))
        << "allDepsGitRecoverable should return true when all file deps share the target repo's governingRepoId";
}

} // namespace nix::eval_trace
