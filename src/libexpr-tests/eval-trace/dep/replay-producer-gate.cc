/**
 * Replay-time producer gate (RFC §3b: the actual hot-path glue).
 *
 * `dep/producer-side-table.cc` proved the side-table API in isolation.
 * `store/ca-producer-boundary-recording.cc` proved the recording-side
 * composition (synthetic). This file pins the BRIDGE: when
 * `TraceRuntime::replayMemoizedDeps(v)` is called from the recording path
 * and `v` is registered as a CA producer in the side table, the consumer's
 * scope receives a SINGLE `TraceValueContext` edge dep, NOT the flattened
 * range copy that today's `replayMemoizedRange` would do.
 *
 * Without this bridge, P1-P6 of `ca-producer-boundary-recording.cc` are
 * still hand-driven (the test calls `record(edge)` itself); production §3b
 * needs the gate to fire automatically when any consumer scope re-forces
 * a producer Value.
 *
 * Tests drive `TraceRuntime` directly, mirroring `dep/epoch-bugs.cc`. They
 * avoid the live evaluator (which would require a TracedExpr fixture); the
 * goal is to pin the gate's behavior at the recording/replay boundary.
 *
 * G1 producer Value forced inside a recording scope ⇒ scope's ownDeps
 *    contain ONE TraceValueContext edge to the registered CA key, NO
 *    flattened deps from the producer's epoch range.
 * G2 NON-producer Value forced inside a recording scope ⇒ today's flatten
 *    path runs (control: confirms the gate is only a routing decision, not
 *    a global suppression).
 * G3 producer Value forced with NO recording scope active ⇒ no-op (the
 *    gate is recording-context-aware; replays without an active scope
 *    don't emit the edge or a flatten).
 * G4 a producer with NO epoch range registered (only side-table entry)
 *    still emits the edge — the gate is independent of the epoch map.
 *    (This is the case where a derivation's deps were consumed via outer
 *    takeDeps before recordThunkDeps fired.)
 */
#include "eval-trace/helpers.hh"
#include "eval-trace/trace-runtime-test-access.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/eval-trace/deps/memo-replay-store.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

static AttrPathId caKey(AttrVocabStore & vocab, std::string_view drvHash)
{
    return vocab.internPath(
        AttrVocabStore::rootPath(),
        vocab.internName(std::string("__ca:") + std::string(drvHash)));
}

class ReplayProducerGateTest : public EvalTraceTest
{
protected:
    InterningPools pools;
    TraceRuntime ctx;

    void SetUp() override
    {
        TraceRuntimeTestAccess::epochLog(ctx).clear();
        TraceRuntimeTestAccess::clearReplayEntries(ctx);
        TraceRuntimeTestAccess::clearProducerMap(ctx);
    }

    void TearDown() override { SetUp(); }
};

// ── G1: producer + recording scope ⇒ edge emitted, no flatten ───────────────

TEST_F(ReplayProducerGateTest, ReplayProducerGate_RegisteredValue_EmitsEdgeNotFlatten)
{
    // Set up a registered producer Value with an epoch-range that WOULD
    // flatten today, plus a side-table entry that should redirect the
    // replay to edge-emission.
    Value producerV;
    producerV.mkInt(0);
    auto pKey = caKey(state.vocabStore(), "drv-G1");
    DepHash pHash{depHash("producer-G1-trace-hash")};
    TraceRuntimeTestAccess::registerProducer(ctx, producerV, pKey, pHash);

    // Cold-record a fake epoch range for producerV (3 file-bytes deps) so
    // that without the gate, the replay would copy 3 deps into the consumer.
    {
        DepRecordingContext dctx(pools, TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        StandaloneDepCtxGuard guard(dctx);
        uint32_t epochStart = ctx.currentReplayEpochSize();
        dctx.record(makeContentDep(pools, "/a.nix", "a"));
        dctx.record(makeContentDep(pools, "/b.nix", "b"));
        dctx.record(makeContentDep(pools, "/c.nix", "c"));
        TraceRuntimeTestAccess::recordThunkDeps(ctx, producerV, epochStart);
        TestScopeAccess::takeDeps(dctx);  // discard producer's own scope
    }

    // Now, inside a CONSUMER recording scope, replay producerV. With the
    // gate installed, the consumer should receive ONE edge dep, not 3
    // flattened content deps.
    std::vector<Dep> consumerDeps;
    {
        DepRecordingContext dctx(pools, TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        StandaloneDepCtxGuard guard(dctx);
        TraceRuntimeTestAccess::replayMemoizedDeps(ctx, producerV);
        consumerDeps = TestScopeAccess::takeDeps(dctx);
    }

    ASSERT_EQ(consumerDeps.size(), 1u)
        << "G1: producer replay must emit exactly one edge, NOT 3 flattened deps";
    EXPECT_EQ(consumerDeps[0].key.kind, CanonicalQueryKind::TraceValueContext)
        << "G1: the emitted dep must be a TraceValueContext edge";
    EXPECT_EQ(consumerDeps[0].traceContextPath().value, pKey.value)
        << "G1: the edge must target the registered CA key";
}

// ── G2: NON-producer ⇒ flatten today (control / non-vacuity) ────────────────

TEST_F(ReplayProducerGateTest, ReplayProducerGate_UnregisteredValue_StillFlattens)
{
    Value ordinaryV;
    ordinaryV.mkInt(1);
    // Do NOT register ordinaryV in the producer map.

    {
        DepRecordingContext dctx(pools, TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        StandaloneDepCtxGuard guard(dctx);
        uint32_t epochStart = ctx.currentReplayEpochSize();
        dctx.record(makeContentDep(pools, "/x.nix", "x"));
        dctx.record(makeContentDep(pools, "/y.nix", "y"));
        TraceRuntimeTestAccess::recordThunkDeps(ctx, ordinaryV, epochStart);
        TestScopeAccess::takeDeps(dctx);
    }

    std::vector<Dep> consumerDeps;
    {
        DepRecordingContext dctx(pools, TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        StandaloneDepCtxGuard guard(dctx);
        TraceRuntimeTestAccess::replayMemoizedDeps(ctx, ordinaryV);
        consumerDeps = TestScopeAccess::takeDeps(dctx);
    }

    EXPECT_EQ(consumerDeps.size(), 2u)
        << "G2 control: an unregistered Value still flattens its 2 epoch deps "
           "into the consumer scope (the gate routes only registered Values)";
    for (const auto & d : consumerDeps)
        EXPECT_NE(d.key.kind, CanonicalQueryKind::TraceValueContext)
            << "G2: no edge dep should be emitted for an unregistered Value";
}

// ── G3: producer registered, NO recording scope active ⇒ no-op ──────────────

TEST_F(ReplayProducerGateTest, ReplayProducerGate_NoRecordingScope_NoOp)
{
    Value producerV;
    producerV.mkInt(2);
    auto pKey = caKey(state.vocabStore(), "drv-G3");
    DepHash pHash{depHash("producer-G3-trace-hash")};
    TraceRuntimeTestAccess::registerProducer(ctx, producerV, pKey, pHash);

    // Replay with no DepRecordingContext active. The gate must not crash;
    // it must also not have any observable side effect.
    EXPECT_NO_THROW(TraceRuntimeTestAccess::replayMemoizedDeps(ctx, producerV));
    // No way to assert "nothing happened" beyond: producerMap unchanged,
    // no exception, and no recording context to mutate. Pin it explicitly.
    EXPECT_EQ(TraceRuntimeTestAccess::producerMapSize(ctx), 1u)
        << "G3: replay outside a recording scope must not corrupt the map";
}

// ── G4: producer registered, NO epoch range ⇒ edge still emitted ────────────

TEST_F(ReplayProducerGateTest, ReplayProducerGate_NoEpochRange_EdgeStillEmitted)
{
    // The case the side-table is for: a producer whose deps were already
    // consumed via outer takeDeps before recordThunkDeps could install an
    // epochMap entry. lookupReplayRange returns nullopt; lookupProducer
    // returns the entry. The gate must still emit an edge.
    Value producerV;
    producerV.mkInt(3);
    auto pKey = caKey(state.vocabStore(), "drv-G4");
    DepHash pHash{depHash("producer-G4-trace-hash")};
    TraceRuntimeTestAccess::registerProducer(ctx, producerV, pKey, pHash);

    // No call to recordThunkDeps — producerV has no epoch range.
    EXPECT_FALSE(TraceRuntimeTestAccess::lookupReplayRange(ctx, producerV).has_value())
        << "G4 precondition: the producer Value has no epoch range";

    std::vector<Dep> consumerDeps;
    {
        DepRecordingContext dctx(pools, TraceRuntimeTestAccess::epochLog(ctx));
        TestScopeAccess::pushScope(dctx);
        StandaloneDepCtxGuard guard(dctx);
        TraceRuntimeTestAccess::replayMemoizedDeps(ctx, producerV);
        consumerDeps = TestScopeAccess::takeDeps(dctx);
    }

    ASSERT_EQ(consumerDeps.size(), 1u)
        << "G4: producer with NO epoch range still emits an edge — the gate "
           "is independent of the epoch map";
    EXPECT_EQ(consumerDeps[0].key.kind, CanonicalQueryKind::TraceValueContext);
    EXPECT_EQ(consumerDeps[0].traceContextPath().value, pKey.value);
}

} // namespace nix::eval_trace
