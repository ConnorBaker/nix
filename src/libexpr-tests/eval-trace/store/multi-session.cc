/**
 * Multi-session DB bulk-load test (D-4).
 *
 * Build three separate SqliteTraceStorage instances in sequence, each recording a
 * distinct path. Then verify all entries survive into a final construction.
 *
 * This exercises the SQLite persistence layer: each store instance flushes
 * its in-memory writes to the DB file on destruction (via recreateDb), and
 * the final instance loads all previously flushed rows from the same file.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── D-4: Bulk load across separate constructions ─────────────────────
//
// Create three SqliteTraceStorage instances sequentially, each recording one path.
// After all three are destroyed (flushed), open a final instance and verify
// that all three paths are present and verifiable.
//
// Each iteration creates a fresh SqliteTraceStorage with its own scoped
// ExclusiveTraceStoreAccess (`ea`). Each makeDb() / recreateDb() call still
// destroys the previous store and opens a fresh one against the same SQLite file.

TEST_F(TraceStoreTest, MultiSession_BulkLoad_AcrossSeparateConstructions)
{
    // Use non-empty deps keyed per-iteration so the final-construction
    // verify exercises the dep-resolution path (not the trivial
    // empty-deps verify that succeeds for any row). Env vars pre-set
    // so verify can resolve the stored hashes against the same values.
    std::vector<std::unique_ptr<ScopedEnvVar>> envGuards;
    envGuards.reserve(3);
    for (int i = 0; i < 3; ++i)
        envGuards.push_back(std::make_unique<ScopedEnvVar>(
            "MULTISESSION_VAR_" + std::to_string(i), "v" + std::to_string(i)));

    for (int i = 0; i < 3; ++i) {
        auto db = makeDb();
        auto dep = makeEnvVarDep(
            pools(),
            "MULTISESSION_VAR_" + std::to_string(i),
            "v" + std::to_string(i));
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, vpath({std::to_string(i)}),
                       string_t{std::to_string(i), {}}, {dep});
        });
        // Flush to SQLite by destroying and reopening the store.
        recreateDb(db);
    }
    // Final construction: each row must be present AND carry its own
    // recorded value. assertCachedResultEquals pins row-to-value
    // identity — a regression that loaded the wrong row for a given
    // attr path (e.g., all three paths resolving to i=0) would fail
    // two of the three assertions.
    auto db = makeDb();
    for (int i = 0; i < 3; ++i) {
        auto result = test::TraceStorageTestAccess::verify(
            *db, vpath({std::to_string(i)}), state);
        ASSERT_TRUE(result.has_value()) << "row " << i << " must persist";
        assertCachedResultEquals(
            string_t{std::to_string(i), {}}, result->value, state.symbols);
    }
}

} // namespace nix::eval_trace
