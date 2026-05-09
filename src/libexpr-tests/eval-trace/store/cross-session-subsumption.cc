/**
 * Cross-session subsumption unsoundness tests.
 *
 * Exercises the bug where `VerificationSession::verifiedContentFiles_`
 * grants subsumption to DIFFERENT traces that recorded their dep hash in
 * a different historical session than the one that just
 * `markFileVerified`'d the file.
 *
 * ── Bug mechanism ──────────────────────────────────────────────────────
 *
 * `markFileVerified(F)` records only a bit: "some stored hash for F
 * matched the current file state."  `isFileVerified(F)` returns that bit
 * for ALL trace verifications in the same VerificationSession.  The
 * subsumption fast-path in dep-resolution-service.cc:352-357 trusts the
 * bit unconditionally and returns the CURRENT dep's own stored hash as
 * "current":
 *
 *     if (session.isFileVerified(dep.key)) {
 *         session.cacheVerifiedHash(dep.key, VerifiedHash{dep.hash}, witness);
 *         return dep.hash;              // <-- stored hash from historical session
 *     }
 *
 * The verifier then compares `current == idep.hash`, which is
 * tautologically true.  If two traces recorded different stored hashes
 * for the same file in different recording sessions, the second trace's
 * verification passes regardless of whether its stored hash actually
 * matches current file content.
 *
 * ── Reproduction ───────────────────────────────────────────────────────
 *
 * 1. Record trace A at attr path "a" with a FileBytes dep on file F,
 *    stored hash h(F@v1).
 * 2. Record trace B at attr path "b" with a NarIdentity dep on file F,
 *    stored hash narHash(F@v1).  (NarIdentity goes through the same
 *    subsumption path; FileBytes is what pass 1 uses to set the bit.)
 * 3. Modify F: v1 -> v2.  Recompute hashes.
 * 4. Re-record trace A with a FileBytes dep carrying hash h(F@v2).  This
 *    simulates a cold re-evaluation after A's stale v1 trace failed.
 * 5. With a SHARED VerificationSession, verify "a" -> passes (FileBytes
 *    matches current).  markFileVerified(F) fires.
 * 6. With the SAME session, verify "b".  Its stored NarIdentity is
 *    narHash(F@v1).  With the bug: subsumption fires, returns stored v1,
 *    comparison is tautological, verification passes.  Without the bug:
 *    current NarIdentity is narHash(F@v2) != stored v1, verification
 *    fails and "b" re-evaluates.
 *
 * The positive test asserts the correct behavior (FAIL on current code).
 * Negative tests exercise related scenarios that should always behave
 * correctly, regardless of the bug.
 */

#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/source-accessor.hh"

#include <filesystem>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

namespace {

/// Mirrors the verifier's runtime call to depHashPath(sourcePath) for
/// the NarIdentity CQK.
DepHash computeNarHash(const std::filesystem::path & p)
{
    auto sp = SourcePath(getFSSourceAccessor(), CanonPath(p.string()));
    return depHashPath(sp);
}

} // anonymous namespace

// ── Negative tests: these MUST pass on current and fixed code ──────────

// A trace with a FileBytes dep whose file content changes must fail
// verification.  Baseline sanity check that the test infrastructure
// (file mutation + accessor invalidation + recreateDb) works.
TEST_F(TraceStoreTest, CrossSession_SingleTrace_FileChanged_VerifyFails)
{
    TempTextFile f("v1");
    auto filePath = std::filesystem::canonical(f.path).string();

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result_v1", {}}, {
            makeContentDep(pools(), filePath, "v1"),
        });
    });
    recreateDb(db);

    // Pre-condition: unchanged file verifies.
    EXPECT_TRUE(
        test::TraceStorageTestAccess::verify(*db, rootPath(), state).has_value());

    // Mutate the file and invalidate caches.
    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // The stored FileBytes hash is for v1 but current file is v2.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value())
        << "Single-trace verify should fail when the only dep's file changes";
}

// Two traces reading two separate files: the unchanged-file trace must
// still verify when the other file changes.  This rules out spurious
// invalidation leaking across files.
TEST_F(TraceStoreTest, CrossSession_TwoFiles_OneChanges_OnlyAffectedFails)
{
    TempTextFile f1("f1-v1");
    TempTextFile f2("f2-v1");
    auto p1 = std::filesystem::canonical(f1.path).string();
    auto p2 = std::filesystem::canonical(f2.path).string();

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"a"}), string_t{"a-result", {}}, {
            makeContentDep(pools(), p1, "f1-v1"),
        });
        db->record(ea, vpath({"b"}), string_t{"b-result", {}}, {
            makeContentDep(pools(), p2, "f2-v1"),
        });
    });
    recreateDb(db);

    // Both verify cleanly initially.
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, vpath({"a"}), state).has_value());
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, vpath({"b"}), state).has_value());

    // Mutate only f1.
    f1.modify("f1-v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    EXPECT_FALSE(test::TraceStorageTestAccess::verify(*db, vpath({"a"}), state).has_value())
        << "Trace on the changed file must fail";
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, vpath({"b"}), state).has_value())
        << "Trace on the unchanged file must still verify";
}

// Two traces sharing the same file, both recorded against the same
// content.  When the file is unchanged and the two traces are verified
// in the SAME session, subsumption is legitimate (trace A's markFileVerified
// correctly implies trace B's dep hash is still valid, because A and B
// recorded the same hash).  Both must verify.
TEST_F(TraceStoreTest, CrossSession_SharedFile_Unchanged_SameSessionBothPass)
{
    TempTextFile f("v1");
    auto filePath = std::filesystem::canonical(f.path).string();
    auto narV1 = computeNarHash(filePath);

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        // "a" uses FileBytes (the coarse dep that sets markFileVerified in pass 1).
        db->record(ea, vpath({"a"}), string_t{"a", {}}, {
            makeContentDep(pools(), filePath, "v1"),
        });
        // "b" uses NarIdentity, recorded against the SAME file at v1.
        db->record(ea, vpath({"b"}), string_t{"b", {}}, {
            makeNARContentDep(pools(), filePath, narV1),
        });
    });
    recreateDb(db);

    // Verify "a" and "b" in the same VerificationSession.  When "a"
    // passes, markFileVerified(F) fires; then "b"'s NarIdentity dep
    // is subsumed.  Because the file is unchanged and B's stored
    // hash matches current v1, the subsumption is sound.
    VerificationSession session;
    auto ra = test::TraceStorageTestAccess::verify(*db, vpath({"a"}), state, session);
    EXPECT_TRUE(ra.has_value())
        << "Trace A (FileBytes) must pass when file unchanged";

    auto rb = test::TraceStorageTestAccess::verify(*db, vpath({"b"}), state, session);
    EXPECT_TRUE(rb.has_value())
        << "Trace B (NarIdentity) must pass when file unchanged";
}

// Two traces sharing the same file, verified in INDEPENDENT sessions.
// When the file is unchanged and both traces recorded against that same
// content, each session independently verifies its trace.  This is the
// baseline that the buggy path above actually pollutes.
TEST_F(TraceStoreTest, CrossSession_SharedFile_Unchanged_FreshSessionsBothPass)
{
    TempTextFile f("v1");
    auto filePath = std::filesystem::canonical(f.path).string();
    auto narV1 = computeNarHash(filePath);

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"a"}), string_t{"a", {}}, {
            makeContentDep(pools(), filePath, "v1"),
        });
        db->record(ea, vpath({"b"}), string_t{"b", {}}, {
            makeNARContentDep(pools(), filePath, narV1),
        });
    });
    recreateDb(db);

    // Fresh session per verify — no subsumption state carried over.
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, vpath({"a"}), state).has_value());
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, vpath({"b"}), state).has_value());
}

// ── Positive tests: these MUST fail on current code (bug reproducers) ─
//
// These tests assert the SOUND behavior.  The current implementation
// violates soundness, so these tests will FAIL (EXPECT_FALSE triggers
// because verify returns a stale value).  After the bug is fixed, they
// should pass.

// Core reproducer.  Two attrs "a" and "b":
//   - "a" has a FileBytes dep on F, stored hash h(F@v1).
//   - "b" has a NarIdentity dep on F, stored hash narHash(F@v1).
//   - File F mutates v1 -> v2.
//   - We re-record "a" with h(F@v2), simulating a successful cold
//     re-evaluation after "a"'s first verify attempt failed.
//   - With a SHARED VerificationSession:
//       * verify("a") passes (hash matches current); markFileVerified(F)
//         fires.
//       * verify("b") has a stored v1 NarIdentity hash which does NOT
//         match the current file state.  The correct behavior is FAIL.
//   - Current bug: verify("b") subsumes the NarIdentity dep via the
//     isFileVerified(F) shortcut and returns stored v1 as "current",
//     tautologically passing.
TEST_F(TraceStoreTest, CrossSession_SubsumptionLeaks_NarIdentityStaleServed)
{
    TempTextFile f("v1");
    auto filePath = std::filesystem::canonical(f.path).string();
    auto narV1 = computeNarHash(filePath);

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record trace "a" with FileBytes dep at v1.
        db->record(ea, vpath({"a"}), string_t{"a_v1_result", {}}, {
            makeContentDep(pools(), filePath, "v1"),
        });

        // Record trace "b" with NarIdentity dep at v1.  Distinct trace
        // (different dep kind => different traceId) but same
        // contentFileKey(F) in the subsumption set.
        db->record(ea, vpath({"b"}), string_t{"b_v1_result", {}}, {
            makeNARContentDep(pools(), filePath, narV1),
        });
    });

    // File mutates.  v1 -> v2.  Accessor cache must be invalidated
    // so the verifier sees the new content.
    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // Re-record "a" with a FileBytes dep carrying the v2 hash.  The
    // trace_hash is content-addressed by the dep set, so this
    // produces a NEW trace row and republishes "a" to point at it.
    // "b" is NOT re-recorded — its v1 trace row remains current.
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"a"}), string_t{"a_v2_result", {}}, {
            makeContentDep(pools(), filePath, "v2"),
        });
    });
    recreateDb(db);

    // Share one VerificationSession across both verify calls.
    VerificationSession session;

    // Verify "a" — its new trace's FileBytes dep matches current v2
    // content, so pass 1 calls markFileVerified(F).  The returned
    // value must be the v2 result.
    auto ra = test::TraceStorageTestAccess::verify(*db, vpath({"a"}), state, session);
    ASSERT_TRUE(ra.has_value())
        << "Re-recorded 'a' trace must verify (FileBytes matches current v2)";
    assertCachedResultEquals(
        string_t{"a_v2_result", {}}, ra->value, state.symbols);

    // Now verify "b".  Its stored NarIdentity is narHash(F@v1) but
    // the file is now v2.  Correct behavior: FAIL (returns nullopt)
    // or returns a result that reflects v2.  Buggy behavior:
    // subsumption returns the v1-stored hash, comparison passes,
    // "b_v1_result" is served even though the file has changed.
    auto rb = test::TraceStorageTestAccess::verify(*db, vpath({"b"}), state, session);
    EXPECT_FALSE(rb.has_value())
        << "Trace 'b' should have failed verification because F changed "
           "between recording and verification.  If this EXPECT_FALSE "
           "triggers, cross-session subsumption is leaking verifier bits "
           "from trace 'a' to trace 'b'.";
}

// Additional reproducer using ExistenceCheck instead of NarIdentity for
// trace "b".  ExistenceCheck is in the isFileVerified subsumption path
// (see verification-session.hh:133) but its dep key CQK differs from
// FileBytes (which "a" uses).  Because the L1 cache keys on the full
// Dep::Key (including CQK), the v2 FileBytes hash cached when "a" was
// verified does NOT shadow "b"'s ExistenceCheck lookup.  The query
// falls into resolveDepHash, hits the isFileVerified shortcut, returns
// the STORED v1-era existence hash, and tautologically matches.
//
// Note: ExistenceCheck for a regular file returns "type:0" as the hash.
// In v1 the file exists with "type:0"; after mutation to v2 it still
// exists with "type:0" — the hashes happen to coincide.  To force a
// genuine mismatch we DELETE the file between recording and
// verification, causing the correct hash to be the missing-file
// sentinel "missing".  The stored "type:0" from recording time must
// NOT match "missing"; if subsumption leaks, it will anyway.
TEST_F(TraceStoreTest, CrossSession_SubsumptionLeaks_ExistenceStaleServed)
{
    TempTextFile f("v1");
    auto filePath = std::filesystem::canonical(f.path).string();

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // "a" has a FileBytes dep (the dep kind that sets
        // markFileVerified in pass 1).
        db->record(ea, vpath({"a"}), string_t{"a_v1_result", {}}, {
            makeContentDep(pools(), filePath, "v1"),
        });

        // "b" has an ExistenceCheck dep.  Its stored hash is the
        // "type:0" sentinel for a regular file.
        db->record(ea, vpath({"b"}), string_t{"b_v1_result", {}}, {
            makeSimpleRecordedDep(
                pools(),
                CanonicalQueryKind::ExistenceCheck,
                DepSource::makeAbsolute(),
                filePath,
                DepHashValue{std::string("type:0")}),
        });
    });

    // Change F so "a"'s v1 trace fails.  We modify AND keep the
    // file present so that "a" can be cold-reevaluated at v2.
    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // Re-record "a" with a v2 FileBytes hash so verify passes and
    // markFileVerified(F) fires.
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"a"}), string_t{"a_v2_result", {}}, {
            makeContentDep(pools(), filePath, "v2"),
        });
    });
    recreateDb(db);

    VerificationSession session;

    // "a"'s v2 FileBytes dep matches current content → pass,
    // markFileVerified(F).
    auto ra = test::TraceStorageTestAccess::verify(*db, vpath({"a"}), state, session);
    ASSERT_TRUE(ra.has_value());
    assertCachedResultEquals(
        string_t{"a_v2_result", {}}, ra->value, state.symbols);

    // Now delete F so that "b"'s correct current ExistenceCheck hash
    // would be the "missing" sentinel, clearly distinct from the
    // stored "type:0".
    std::filesystem::remove(filePath);
    getFSSourceAccessor()->invalidateCache();
    // The file just changed state (deleted) — but for this test we
    // rely on the fact that "a" was already verified before the
    // delete, so session.verifiedContentFiles_ still contains F.
    // isFileVerified(F) stays true even though F no longer exists.

    auto rb = test::TraceStorageTestAccess::verify(*db, vpath({"b"}), state, session);
    EXPECT_FALSE(rb.has_value())
        << "Trace 'b' (ExistenceCheck) must fail: F was deleted, so "
           "current hash is 'missing', but stored hash was 'type:0'.  "
           "If this EXPECT_FALSE triggers, cross-session subsumption "
           "is serving stale results across CQK kinds.";
}

// Negative test: demonstrates that L1 caching correctly catches
// same-CQK sibling staleness.  Both "a" and "b" use FileBytes on F.
// When "a"'s re-recorded v2 trace is verified, L1 caches
// Key(FileBytes, F) → ComputedHash(v2).  A subsequent verify of "b"
// on the same L1 key returns the v2 hash, which does not match b's
// stored v1 hash, so "b" correctly fails.  This is the reason the
// bug does NOT manifest when all deps share one CQK — the L1 hit
// short-circuits BEFORE the subsumption path in resolveDepHash.
TEST_F(TraceStoreTest, CrossSession_SameCQKOnSharedFile_L1CatchesStale)
{
    TempTextFile f("v1");
    auto filePath = std::filesystem::canonical(f.path).string();

    auto db = makeDb();

    // "a" and "b" both have FileBytes(v1) plus a distinguishing env
    // var so their trace IDs differ.
    ScopedEnvVar envA("NIX_L1CATCH_A", "a");
    ScopedEnvVar envB("NIX_L1CATCH_B", "b");
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"a"}), string_t{"a_v1_result", {}}, {
            makeContentDep(pools(), filePath, "v1"),
            makeEnvVarDep(pools(), "NIX_L1CATCH_A", "a"),
        });
        db->record(ea, vpath({"b"}), string_t{"b_v1_result", {}}, {
            makeContentDep(pools(), filePath, "v1"),
            makeEnvVarDep(pools(), "NIX_L1CATCH_B", "b"),
        });
    });

    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"a"}), string_t{"a_v2_result", {}}, {
            makeContentDep(pools(), filePath, "v2"),
            makeEnvVarDep(pools(), "NIX_L1CATCH_A", "a"),
        });
    });
    recreateDb(db);

    VerificationSession session;

    auto ra = test::TraceStorageTestAccess::verify(*db, vpath({"a"}), state, session);
    ASSERT_TRUE(ra.has_value());

    // "b" shares the SAME Dep::Key as "a" for the FileBytes dep, so
    // L1 lookup on that key returns the v2 ComputedHash set by "a".
    // Comparison against stored v1 fails.
    auto rb = test::TraceStorageTestAccess::verify(*db, vpath({"b"}), state, session);
    EXPECT_FALSE(rb.has_value())
        << "When b's dep uses the same CQK as a's (FileBytes), the L1 "
           "cache from a's verification correctly invalidates b's "
           "stored hash.  This test documents that same-CQK siblings "
           "are safe; the bug specifically affects cross-CQK siblings.";
}

// Third reproducer: independent sessions should reject "b" cleanly.
// This is the "control" for the bug: verify "b" alone in its own
// VerificationSession after the file change.  Without the subsumption
// state from trace "a", no bit is set, so the verifier must actually
// compute the current hash and compare it against stored v1 — which
// fails.  This test should pass on BOTH current and fixed code: it
// confirms that the failure in the positive tests above is specifically
// caused by session-scoped subsumption, not by a generally broken
// FileBytes/NarIdentity verifier.
TEST_F(TraceStoreTest, CrossSession_SubsumptionIsolated_FreshSessionFailsCorrectly)
{
    TempTextFile f("v1");
    auto filePath = std::filesystem::canonical(f.path).string();
    auto narV1 = computeNarHash(filePath);

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"b"}), string_t{"b_v1_result", {}}, {
            makeNARContentDep(pools(), filePath, narV1),
        });
    });

    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // Fresh session (no prior markFileVerified state).  The verifier
    // computes narHash(F@v2), compares to stored narHash(F@v1),
    // they differ, verification fails.
    auto rb = test::TraceStorageTestAccess::verify(*db, vpath({"b"}), state);
    EXPECT_FALSE(rb.has_value())
        << "Fresh-session NarIdentity verify must fail when F changed";
}

} // namespace nix::eval_trace
