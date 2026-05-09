/**
 * Tests for the `InMemoryTraceRows` backend — a proof-of-abstraction for the
 * `TraceStorage` interface that does not use SQLite.
 *
 * Exercises each of the four currently-abstract `TraceStorage` virtuals
 * (`setSessionConfig`, `recordRuntimeRoot`, `loadRuntimeRoots`, `flush`)
 * through the in-memory backend, plus the base-owned `allocateNodeStamp`
 * counter and `hasSessionConfig` slot. Pins that the abstract interface
 * works through a non-SQLite backend per rearchitecture-proposal.md
 * §14 step 17.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/in-memory-trace-rows.hh"
#include "nix/expr/eval-trace/store/session-identity.hh"
#include "nix/fetchers/fetchers.hh"

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

} // namespace

class InMemoryTraceRowsTest : public EvalTraceTest {};

TEST_F(InMemoryTraceRowsTest, SessionIdentity_SetOnce)
{
    auto bootstrap = SemanticSessionKey::fromSerialized("bootstrap:test");
    InMemoryTraceRows storage(bootstrap);
    EXPECT_FALSE(storage.hasSessionConfig());

    auto cfg = SessionConfig::forTest(depHash("fp").value);
    storage.setSessionConfig(cfg);
    EXPECT_TRUE(storage.hasSessionConfig());

    // Second call throws per SetOnce.
    EXPECT_THROW(storage.setSessionConfig(cfg), Error);
}

TEST_F(InMemoryTraceRowsTest, RuntimeRoot_RecordAndLoadRoundTrip)
{
    TempDir runtimeDir;
    runtimeDir.addFile("value.txt", "shared");
    ScopedExtraExperimentalFeatures flakes("flakes");

    auto input = fetchers::Input::fromURL(
        state.fetchSettings,
        "path:" + runtimeDir.path().string());
    auto [storePath, lockedInput] = input.fetchToStore(state.fetchSettings, *state.store);

    auto bootstrap = SemanticSessionKey::fromSerialized("bootstrap:runtime-root");
    InMemoryTraceRows storage(bootstrap);

    auto fetchIdentity = RuntimeFetchIdentityDepKey{.inputAttrs = lockedInput.toAttrs()};
    auto narHash = RuntimeRootNarHash{state.store->queryPathInfo(storePath)->narHash};
    auto source = DepSource::fromRuntimeRoot(makeRuntimeRootSourceKey(fetchIdentity));

    withBs([&](const auto & bs) {
        storage.withExclusiveAccess(bs, [&](const auto & ea) {
            storage.recordRuntimeRoot(
                ea,
                RuntimeRootRecord{
                    .source = source,
                    .fetchIdentity = fetchIdentity,
                    .narHash = narHash,
                    .storePath = RuntimeRootStorePath{storePath},
                },
                *state.store);

            auto loaded = storage.loadRuntimeRoots(ea, *state.store);
            EXPECT_EQ(loaded.storedCount, 1u);
            EXPECT_EQ(loaded.rejectedCount, 0u);
            ASSERT_EQ(loaded.entries.size(), 1u);

            auto & entry = loaded.entries.front();
            EXPECT_EQ(entry.source, source);
            EXPECT_EQ(entry.fetchIdentity.inputAttrs, fetchIdentity.inputAttrs);
            EXPECT_EQ(entry.narHash, narHash);
            EXPECT_EQ(entry.storePath.value, storePath);
        });
    });
}

TEST_F(InMemoryTraceRowsTest, Flush_IsNoOp)
{
    auto bootstrap = SemanticSessionKey::fromSerialized("bootstrap:flush");
    InMemoryTraceRows storage(bootstrap);
    auto cfg = SessionConfig::forTest(depHash("fp").value);
    storage.setSessionConfig(cfg);

    withBs([&](const auto & bs) {
        storage.withExclusiveAccess(bs, [&](const auto & ea) {
            EXPECT_NO_THROW(storage.flush(ea));
        });
    });
}

TEST_F(InMemoryTraceRowsTest, AllocateNodeStamp_Monotonic)
{
    auto bootstrap = SemanticSessionKey::fromSerialized("bootstrap:stamps");
    InMemoryTraceRows storage(bootstrap);
    auto first = storage.allocateNodeStamp();
    auto second = storage.allocateNodeStamp();
    EXPECT_LT(first.value, second.value);
}

} // namespace nix::eval_trace
