/**
 * A-3 · DerivedStorePath end-to-end
 *
 * Tests for the DerivedStorePath (CQK::DerivedStorePath) dep kind.
 *
 * Key format: canonical DerivedStorePath dep-key blob.
 * Hash value: the expected store path string (DepHashValue string variant).
 *
 * The three scenarios covered:
 *  - Warm hit: recorded store path matches the current derivation.
 *  - Source change invalidation: modifying the source file changes the
 *    derived store path → verification fails.
 *  - Missing file: source path does not exist → sentinel hash is used →
 *    stored store path never matches → verification fails.
 *
 * File invalidation uses the direct pattern for TraceStoreTest:
 *   getFSSourceAccessor()->invalidateCache(...)
 * (TraceStoreFixture does not expose invalidateFileCache().)
 */

#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/source-accessor.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/// Compute the DerivedStorePath store path for a test file using its baseName.
/// Uses store->computeStorePath() (pure hash — no store write required).
static std::string computeDerivedStorePath(EvalState & state, const std::filesystem::path & filePath)
{
    auto baseName = std::string(*CanonPath(filePath.string()).baseName());
    return state.store->printStorePath(
        std::get<0>(state.store->computeStorePath(
            baseName,
            SourcePath(getFSSourceAccessor(), CanonPath(filePath.string()))
                .resolveSymlinks(SymlinkResolution::Ancestors),
            ContentAddressMethod::Raw::NixArchive,
            HashAlgorithm::SHA256, {})));
}

// ── A-3: DerivedStorePath_CanonicalKey_WarmHit ───────────────────────

TEST_F(TraceStoreTest, DerivedStorePath_CanonicalKey_WarmHit)
{
    // Use a real temporary file so the verifier can compute a hash.
    TempTestFile src("let x=1;in x");
    auto baseName = std::string(*CanonPath(src.path.string()).baseName());
    auto storePath = computeDerivedStorePath(state, src.path);

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}},
                   {makeCopiedPathDep(pools(), src.path.string(), baseName,
                                     storePath)});
    });

    recreateDb(db);

    // The verifier computes computeStorePath for the real file and
    // compares against the recorded store path string — must match.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_TRUE(result.has_value());
}

// ── A-3: DerivedStorePath_SourceChange_Invalidation ──────────────────

TEST_F(TraceStoreTest, DerivedStorePath_SourceChange_Invalidation)
{
    // Use a real temporary file and record with the real computed
    // store path so the cold/warm warm-hit baseline holds. Without
    // this, any miss on the mutated path could be "hit a stale
    // synthetic path" rather than "source change invalidates."
    TempTestFile src("let x=1;in x");
    auto baseName = std::string(*CanonPath(src.path.string()).baseName());
    auto storePath = computeDerivedStorePath(state, src.path);

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v1", {}},
                   {makeCopiedPathDep(pools(), src.path.string(), baseName,
                                     storePath)});
    });

    recreateDb(db);

    // Precondition: warm verify must hit on the unchanged file. If
    // this fails, any subsequent miss assertion is vacuous (the test
    // would have missed for unrelated reasons).
    {
        auto warm = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        ASSERT_TRUE(warm.has_value())
            << "precondition: warm hit on unchanged source is required "
               "before the invalidation check is meaningful";
    }

    // Modify the source and re-verify: the recorded storePath no
    // longer matches the recomputed hash, so verification must miss.
    src.modify("let x=2;in x");
    getFSSourceAccessor()->invalidateCache();

    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        EXPECT_FALSE(result.has_value())
            << "source change must invalidate the DerivedStorePath dep";
    }
}

// ── A-3: DerivedStorePath_MissingFile_Miss ───────────────────────────

TEST_F(TraceStoreTest, DerivedStorePath_MissingFile_Miss)
{
    auto db = makeDb();

    // Source path does not exist → resolver returns a sentinel hash.
    // The sentinel will never match the recorded store path → miss.
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}},
                   {makeCopiedPathDep(pools(), "/nonexistent", "f.nix", "")});
    });

    recreateDb(db);

    // kHashMissing / sentinel path: verification must return false.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value());
}

} // namespace nix::eval_trace
