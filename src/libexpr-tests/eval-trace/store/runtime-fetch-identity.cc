/**
 * A-5 · RuntimeFetchIdentity warm hit + hash change
 *
 * Tests for the RuntimeFetchIdentity (CQK::RuntimeFetchIdentity) dep kind.
 *
 * This dep kind stores the store-path string that a fetch would produce
 * against a LOCKED fetch input. Verification re-invokes
 * `observeRuntimeFetchIdentity` against the current state and compares the
 * re-derived store-path string against the stored hash.
 *
 * Fixture uses a `path:` input locked via `fetchToStore` — locking populates
 * `narHash`, which makes the verifier's `computeFromLockedInput` path
 * deterministic and independent of any out-of-process fetcher.  Prior to
 * 2026-04-22 these tests recorded an unlocked input built directly from a
 * URL string via `makeSimpleRecordedDep(..., RuntimeFetchIdentity, ..., url, ...)`;
 * that shape caused `Input::computeStorePath` to throw "cannot compute
 * store path for unlocked input" inside the verifier and weakened the
 * assertions to `EXPECT_NO_THROW`.  See DEF-2 in the tests CLAUDE.md.
 */

#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/expr/eval-settings.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

namespace {

struct ScopedExtraExperimentalFeatures
{
    explicit ScopedExtraExperimentalFeatures(std::string_view features)
    {
        experimentalFeatureSettings.set("extra-experimental-features", std::string(features));
    }

    ~ScopedExtraExperimentalFeatures()
    {
        experimentalFeatureSettings.set("extra-experimental-features", "");
    }
};

/// Build a RuntimeFetchIdentity dep keyed on a LOCKED `path:` input, with
/// the stored hash equal to the store-path string the verifier will
/// re-derive on observe.  Returns the dep + the expected hash (so the
/// test can optionally tweak it for hash-change scenarios).
struct LockedRuntimeFetch {
    Dep dep;
    std::string expectedHash;
};

LockedRuntimeFetch makeLockedRuntimeFetchDep(
    EvalState & state, InterningPools & pools, const TempDir & dir)
{
    // fetchers reject paths that traverse a symlink (e.g., /tmp → /private/tmp on macOS).
    auto input = fetchers::Input::fromURL(
        state.fetchSettings,
        "path:" + std::filesystem::canonical(dir.path()).string());
    auto [storePath, lockedInput] = input.fetchToStore(state.fetchSettings, *state.store);

    auto source = DepSource::fromRuntimeRoot(
        makeRuntimeRootSourceKey(RuntimeFetchIdentityDepKey{.inputAttrs = lockedInput.toAttrs()}));
    auto sourceId = pools.intern<DepSourceId>(source);
    auto keyId = pools.intern(RuntimeFetchIdentityDepKey{.inputAttrs = lockedInput.toAttrs()});
    auto expected = state.store->printStorePath(storePath);
    Dep dep{
        Dep::Key::makeRuntimeFetchIdentity(sourceId, keyId),
        DepHashValue(expected),
    };
    return {std::move(dep), std::move(expected)};
}

} // namespace

// ── A-5: RuntimeFetchIdentity_RecordVerify_WarmHit ───────────────────

TEST_F(TraceStoreTest, RuntimeFetchIdentity_RecordVerify_WarmHit)
{
    TempDir dir;
    dir.addFile("value.txt", "v1");
    ScopedExtraExperimentalFeatures flakes("flakes");

    auto db = makeDb();
    auto locked = makeLockedRuntimeFetchDep(state, pools(), dir);

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}}, {locked.dep});
    });

    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_TRUE(result.has_value())
        << "locked path: input must verify against its own re-derived store path";
}

// ── A-5: RuntimeFetchIdentity_HashChange_WarmMiss ────────────────────
//
// Re-record the trace with an intentionally wrong stored hash.  The
// verifier re-derives the correct store-path string; comparison against
// the stored (wrong) hash fails, and the verify returns nullopt.

TEST_F(TraceStoreTest, RuntimeFetchIdentity_HashChange_WarmMiss)
{
    TempDir dir;
    dir.addFile("value.txt", "v1");
    ScopedExtraExperimentalFeatures flakes("flakes");

    auto db = makeDb();
    auto locked = makeLockedRuntimeFetchDep(state, pools(), dir);

    // Record with a DELIBERATELY-WRONG hash to simulate a prior session that
    // observed a different store path for the same locked input.  The
    // re-derive path will produce the correct store-path string and fail to
    // match the stored hash.
    Dep wrongHashDep{locked.dep.key, DepHashValue(std::string("sha256:wrong-stored-hash"))};
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v1", {}}, {wrongHashDep});
    });

    recreateDb(db);
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value())
        << "stored hash does not match re-derived store path → miss";
}

} // namespace nix::eval_trace
