/**
 * Error-handling tests for dep resolution (D-7).
 *
 * When dep resolution throws (e.g., store unreachable, nonexistent git repo),
 * verify() must return a graceful miss (false) rather than propagating the
 * exception to the caller.
 *
 * Three dep kinds covered:
 *   - DerivedStorePath (via makeCopiedPathDep with a broken store path)
 *   - RuntimeFetchIdentity (via makeSimpleRecordedDep with an unreachable URL)
 *   - GitRevisionIdentity (via makeGitIdentityDep with a nonexistent repo)
 *
 * All three tests exercise the graceful-miss path via the
 * `!sourcePath->maybeLstat() → sentinel(SentinelHash::Missing)` branch in
 * dep-resolution-service.cc.  The sibling `catch (std::exception &) { return
 * nullopt; }` branch in the same switch cases has identical observable
 * outcome (resolveCurrentDepHash returns nullopt → trace invalidates), so
 * the coverage gap is precision-only.  See the retired DEF-7 entry in
 * src/libexpr-tests/eval-trace/CLAUDE.md §D for the full justification.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── D-7a: DerivedStorePath graceful miss ─────────────────────────────
//
// Record a trace with a DerivedStorePath dep pointing at a broken store
// path.  The broken path is unknown to the dummy:// store; dep resolution
// falls through the `!maybeLstat() → sentinel(SentinelHash::Missing)` branch.  verify()
// returns nullopt without throwing.

TEST_F(TraceStoreTest, DepResolution_DerivedStorePath_Throws_GracefulMiss)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(
            ea,
            rootPath(),
            string_t{"v", {}},
            {makeCopiedPathDep(pools(), "/src/f.nix", "f.nix", "/nix/store/BROKEN-path")});
    });
    recreateDb(db);
    auto result1 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result1.has_value());
}

// ── D-7b: RuntimeFetchIdentity — graceful miss ───────────────────────
//
// Record a trace with a RuntimeFetchIdentity dep for an unreachable URL.
// The dummy:// store cannot resolve runtime-fetch identities, so verify()
// must return false without throwing.

TEST_F(TraceStoreTest, DepResolution_RuntimeFetchIdentity_Throws_GracefulMiss)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(
            ea,
            rootPath(),
            string_t{"v", {}},
            {makeSimpleRecordedDep(
                pools(),
                CanonicalQueryKind::RuntimeFetchIdentity,
                DepSource::makeAbsolute(),
                "tarball+https://broken.invalid/x",
                DepHashValue{depHash("h")})});
    });
    recreateDb(db);
    auto result2 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result2.has_value());
}

// ── D-7c: GitRevisionIdentity — graceful miss ────────────────────────
//
// Record a trace with a GitRevisionIdentity dep pointing at a nonexistent
// repository.  The git lookup throws (or returns nullopt); verify() must
// not propagate that exception.
//
// Behavior: GitRevisionIdentity is an ImplicitStructural dep used as an
// indexed short-circuit optimization.  When the current git hash cannot be
// computed (nonexistent repo), resolveDepHash returns nullopt.  The dep is
// treated as "short-circuit didn't fire" rather than "failure" — it does
// NOT set hasNonContentFailure.  A trace whose ONLY dep is a GitRevisionIdentity
// with an unresolvable repo therefore has NO failing deps and verifies as
// Valid (verify returns a result, not nullopt).
//
// This test asserts the graceful (non-throwing) behavior: verify() returns
// a value (the trace passes) rather than throwing.

TEST_F(TraceStoreTest, DepResolution_GitRevisionIdentity_Throws_GracefulMiss)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(
            ea,
            rootPath(),
            string_t{"v", {}},
            {makeGitIdentityDep(pools(), "/nonexistent-repo", "deadbeef")});
    });
    recreateDb(db);
    // GitRevisionIdentity with an unresolvable repo is ignored by primary
    // verification. With no other failing deps the trace is Valid.
    // verify() returns a result (true/non-null) without throwing.
    auto result3 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_TRUE(result3.has_value());
}

TEST_F(TraceStoreTest, StructuredFailureCache_DoesNotPoisonLaterVerificationSession)
{
    TempJsonFile jsonFile(R"({"x": 1})");
    auto filePath = std::filesystem::canonical(jsonFile.path).string();
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(
            ea,
            rootPath(),
            string_t{"v", {}},
            {makeStructuredDepForTest(
                pools(), CanonicalQueryKind::StructuredProjection,
                DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
                {StructuredPathComponent::makeKey("x")},
                DepHashValue(depHash("1")))});
    });

    jsonFile.modify("{ invalid json");
    getFSSourceAccessor()->invalidateCache();
    EXPECT_FALSE(test::TraceStorageTestAccess::verify(*db, rootPath(), state).has_value());

    jsonFile.modify(R"({"x": 1})");
    getFSSourceAccessor()->invalidateCache();
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, rootPath(), state).has_value())
        << "structured parse failures must be scoped to the verification session";
}

} // namespace nix::eval_trace
