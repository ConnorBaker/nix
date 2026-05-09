/**
 * Large-trace performance test (G-2).
 *
 * Record and verify a trace with 10 000 deps. Uses makeEnvVarDep rather than
 * filesystem paths so that dep resolution during verify() does not require
 * creating 10 000 files on disk. Each dep's "current" value is set via
 * ScopedEnvVar before the verify call.
 *
 * The test asserts that both record + verify complete in under 5 seconds.
 * On slow CI machines the threshold can be relaxed, but it serves as a
 * canary for O(N^2) regressions in the serialization or lookup paths.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"

#include <chrono>
#include <gtest/gtest.h>
#include <vector>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── G-2: 10k-dep trace — record + verify in under 5 s ────────────────
//
// Build 10 000 EnvironmentLookup deps (VAR_0 … VAR_9999). Set each env var
// to its expected value via a single ScopedEnvVar per key before verify so
// dep resolution can compare current values against stored hashes.
//
// Using makeEnvVarDep avoids filesystem overhead: env var lookups resolve
// entirely in-process without stat() or file I/O.

TEST_F(TraceStoreTest, LargeTrace_10k_Deps_RecordVerifyUnder5s)
{
    constexpr int N = 10000;

    // Pre-set all env vars before constructing deps so that the hash values
    // stored in the trace match the env var values that verify() will see.
    std::vector<std::unique_ptr<ScopedEnvVar>> envGuards;
    envGuards.reserve(N);
    for (int i = 0; i < N; ++i)
        envGuards.push_back(
            std::make_unique<ScopedEnvVar>("VAR_" + std::to_string(i), "v" + std::to_string(i)));

    std::vector<Dep> deps;
    deps.reserve(N);
    for (int i = 0; i < N; ++i)
        deps.push_back(makeEnvVarDep(pools(), "VAR_" + std::to_string(i), "v" + std::to_string(i)));

    auto db = makeDb();
    auto t0 = std::chrono::steady_clock::now();

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}}, deps);
    });
    recreateDb(db);
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());

    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, std::chrono::seconds(5))
        << "10k-dep record+verify took too long";

    // Correctness: round-trip the stored value, not just the row
    // presence. If serialization got mangled under load, the timing
    // bound alone wouldn't catch a silent corruption.
    assertCachedResultEquals(string_t{"v", {}}, result->value, state.symbols);
}

} // namespace nix::eval_trace
