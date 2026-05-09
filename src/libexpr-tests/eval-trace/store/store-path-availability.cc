/**
 * A-4 · StorePathAvailability record/verify
 *
 * Tests for the StorePathAvailability (CQK::StorePathAvailability) dep kind.
 *
 * The verifier checks isValidPath() against the store for the dep key.
 * With a write-enabled dummy:// store we can pre-populate the store
 * (via addToStore) so the dep records "valid" and verification passes.
 *
 * The round-trip test exercises the canonical typed dep-key encoding.
 */

#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/source-accessor.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

/// Add a real file to the dummy:// store and return its store path string.
/// The store is opened read-write by EvalTraceTest.
static std::string addDummyStorePath(EvalState & state, const std::filesystem::path & filePath)
{
    auto storePath = state.store->addToStore(
        std::string(*CanonPath(filePath.string()).baseName()),
        SourcePath(getFSSourceAccessor(), CanonPath(filePath.string())));
    return state.store->printStorePath(storePath);
}

// ── A-4: StorePathAvailability_RecordVerify_WarmHit ──────────────────

TEST_F(TraceStoreTest, StorePathAvailability_RecordVerify_WarmHit)
{
    // Pre-populate the dummy store so isValidPath() returns true.
    TempTextFile f("spa-content-v1");
    auto storePathStr = addDummyStorePath(state, f.path);

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}},
                   {makeSimpleRecordedDep(pools(),
                        CanonicalQueryKind::StorePathAvailability,
                        DepSource::makeAbsolute(),
                        storePathStr,
                        DepHashValue{std::string("valid")})});
    });

    recreateDb(db);

    // The verifier calls isValidPath for storePathStr — store has it →
    // returns "valid" which matches the recorded hash.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_TRUE(result.has_value());
}

// ── A-4: StorePathAvailability_CanonicalKey_RoundTrip ────────────────

TEST_F(TraceStoreTest, StorePathAvailability_CanonicalKey_RoundTrip)
{
    // The canonical StorePathAvailability dep-key blob must survive
    // serialization and decode back to the original store path.
    TempTextFile f("spa-tab-content");
    auto storePathStr = addDummyStorePath(state, f.path);
    auto storePath = StorePath(std::filesystem::path(storePathStr).filename().string());

    auto db = makeDb();
    auto keyId = pools().intern(StorePathAvailabilityDepKey{storePath});
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(
            ea,
            rootPath(),
            string_t{"v", {}},
            {Dep{
                Dep::Key::makeStorePathAvailability(
                    pools().intern<DepSourceId>(DepSource::makeAbsolute()),
                    keyId),
                DepHashValue{std::string("valid")},
            }});
    });

    recreateDb(db);

    // The verifier decodes the canonical dep-key blob and calls
    // isValidPath — store has it → warm hit.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_TRUE(result.has_value());
}

} // namespace nix::eval_trace
