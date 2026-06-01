/**
 * `TraceSession::recordCAProducer` — RFC §3b production wiring.
 *
 * The infrastructure (`MemoReplayStore::producerMap` + replay-time gate +
 * counter) is dormant until a caller registers producers. The cleanest API
 * — informed by the four-way investigation reports — mirrors the existing
 * `TraceBackend::recordRuntimeRoot` (`Certifier<BlockingTag>::withProof` +
 * `withExclusiveAccess`, no `EvalContext<Suspendable>` threading): a sync
 * record path that can be called from primops.
 *
 * Tests drive the new API directly through a real `TraceSession` (cold
 * recording path), without touching `prim_derivationStrict`. Once these
 * pass, the primop integration is a one-liner that constructs a
 * `DepCaptureScope` around the input force and calls
 * `session->recordCAProducer(v, drvHashStr, innerDeps)`.
 *
 * R1 round-trip: recordCAProducer persists a CA producer trace under the
 *    synthetic `__ca:<drvHash>` key AND registers the Value*→{caKey, hash}
 *    binding so subsequent re-forces of the Value emit edges.
 * R2 distinct drvHashes ⇒ distinct CA keys (no collision).
 * R3 the recorded producer trace verifies in a fresh session (the CA-keyed
 *    routing carries through cross-session, mirroring ca-trace-key-routing
 *    R1).
 * R4 input mutation invalidates the recorded producer trace (soundness —
 *    `recordCAProducer` produces a real, verifiable trace, not a dummy).
 */
#include "eval-trace/helpers.hh"
#include "eval-trace/trace-runtime-test-access.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/value.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class TraceSessionRecordCAProducerTest : public TraceCacheFixture
{
protected:
    /// Allocate a producer-deps vector via a real DepRecordingContext +
    /// DepCaptureScope, mirroring how `prim_derivationStrict` will capture
    /// inputs around `forceAttrs(args[0])` + `derivationStrictInternal`.
    std::vector<Dep> captureProducerInputs(
        std::function<void(DepRecordingContext &)> body)
    {
        std::vector<Dep> innerDeps;
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(state.tracingPools(), epochLog);
        TestScopeAccess::pushScope(ctx);
        body(ctx);
        innerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
        return innerDeps;
    }

    /// Build the synthetic CA routing key for inspection (must match what
    /// `recordCAProducer` computes internally — same construction as
    /// `store/ca-trace-key-routing.cc::caKey`).
    AttrPathId expectedCAKey(std::string_view drvHash)
    {
        return state.vocabStore().internPath(
            AttrVocabStore::rootPath(),
            state.vocabStore().internName(std::string("__ca:") + std::string(drvHash)));
    }
};

// ── R1: round-trip ───────────────────────────────────────────────────────────

TEST_F(TraceSessionRecordCAProducerTest, RecordCAProducer_RoundTripsAndRegisters)
{
    TempTextFile inputSrc("R1-v1");
    auto session = makeCache("42");                           // any expr
    forceRoot(*session);                                      // open the backend

    auto innerDeps = captureProducerInputs([&](auto & ctx) {
        ctx.record(makeContentDep(state.tracingPools(), inputSrc.path.string(), "R1-v1"));
    });

    // Use an attrset Value: production `recordCAProducer` requires nAttrs
    // (derivation results are always attrsets) to extract a stable
    // `Bindings *` key. `mkInt` Values short-circuit early.
    Value producerV;
    producerV.mkAttrs(state.buildBindings(0, EmptyBindingsAllocation::AllocateFresh).finish());

    bool ok = session->recordCAProducer(producerV, "drv-R1", innerDeps);
    ASSERT_TRUE(ok)
        << "R1: recordCAProducer must succeed when a backend is active";

    // (a) producerMap is populated with a binding pointing at expectedCAKey.
    auto bound = TraceRuntimeTestAccess::lookupProducer(*state.traceCtx, producerV);
    ASSERT_TRUE(bound.has_value())
        << "R1: registerProducer must have fired alongside the trace persist";
    EXPECT_EQ(bound->caKey.value, expectedCAKey("drv-R1").value);

    // (b) the producer trace is persisted in the SQLite store under the CA key.
    //     Fresh-session verify proves it survives a backend boundary.
    releaseActiveSession();
    // Reopen the same session config + verify the CA-keyed trace.
    auto session2 = makeCache("42");
    EXPECT_TRUE(session2->verifyAttrPathForTest(expectedCAKey("drv-R1")))
        << "R1: persisted CA producer trace must verify in a fresh session";
}

// ── R2: distinct drvHashes ⇒ distinct CA keys ───────────────────────────────

TEST_F(TraceSessionRecordCAProducerTest, RecordCAProducer_DistinctDrvHashes_DistinctKeys)
{
    TempTextFile inputA("a"), inputB("b");
    auto session = makeCache("42");
    forceRoot(*session);

    auto depsA = captureProducerInputs([&](auto & ctx) {
        ctx.record(makeContentDep(state.tracingPools(), inputA.path.string(), "a"));
    });
    auto depsB = captureProducerInputs([&](auto & ctx) {
        ctx.record(makeContentDep(state.tracingPools(), inputB.path.string(), "b"));
    });

    Value vA, vB;
    // Attrset Values for the production gate. Each gets its own fresh
    // `Bindings*` so distinct registrations don't collide on key.
    vA.mkAttrs(state.buildBindings(0, EmptyBindingsAllocation::AllocateFresh).finish());
    vB.mkAttrs(state.buildBindings(0, EmptyBindingsAllocation::AllocateFresh).finish());
    ASSERT_TRUE(session->recordCAProducer(vA, "drv-A", depsA));
    ASSERT_TRUE(session->recordCAProducer(vB, "drv-B", depsB));

    auto bA = TraceRuntimeTestAccess::lookupProducer(*state.traceCtx, vA);
    auto bB = TraceRuntimeTestAccess::lookupProducer(*state.traceCtx, vB);
    ASSERT_TRUE(bA.has_value());
    ASSERT_TRUE(bB.has_value());
    EXPECT_NE(bA->caKey.value, bB->caKey.value)
        << "R2: distinct drvHashes must produce distinct CA AttrPathIds";
    EXPECT_NE(bA->traceHash.value, bB->traceHash.value)
        << "R2: distinct producer closures must produce distinct trace hashes";
}

// ── R3: cross-session persistence ───────────────────────────────────────────

TEST_F(TraceSessionRecordCAProducerTest, RecordCAProducer_PersistedTraceVerifiesAcrossSession)
{
    TempTextFile inputSrc("R3-stable");
    auto session = makeCache("42");
    forceRoot(*session);
    auto deps = captureProducerInputs([&](auto & ctx) {
        ctx.record(makeContentDep(state.tracingPools(), inputSrc.path.string(), "R3-stable"));
    });
    Value v;
    v.mkAttrs(state.buildBindings(0, EmptyBindingsAllocation::AllocateFresh).finish());
    ASSERT_TRUE(session->recordCAProducer(v, "drv-R3", deps));

    releaseActiveSession();
    auto session2 = makeCache("42");
    EXPECT_TRUE(session2->verifyAttrPathForTest(expectedCAKey("drv-R3")))
        << "R3: with the input file unchanged, the persisted CA producer "
           "trace must verify in a fresh session";
}

// ── R4: mutation invalidation (soundness) ───────────────────────────────────

TEST_F(TraceSessionRecordCAProducerTest, RecordCAProducer_InputMutation_InvalidatesProducer)
{
    TempTextFile inputSrc("R4-v1");
    auto session = makeCache("42");
    forceRoot(*session);
    auto deps = captureProducerInputs([&](auto & ctx) {
        ctx.record(makeContentDep(state.tracingPools(), inputSrc.path.string(), "R4-v1"));
    });
    Value v;
    v.mkAttrs(state.buildBindings(0, EmptyBindingsAllocation::AllocateFresh).finish());
    ASSERT_TRUE(session->recordCAProducer(v, "drv-R4", deps));

    // Precondition — unchanged ⇒ verifies.
    releaseActiveSession();
    {
        auto s2 = makeCache("42");
        ASSERT_TRUE(s2->verifyAttrPathForTest(expectedCAKey("drv-R4")));
    }

    // Mutate the producer's input. The persisted producer trace's FileBytes
    // dep no longer matches; verify must miss.
    inputSrc.modify("R4-v2");
    invalidateFileCache(inputSrc.path);
    {
        auto s2 = makeCache("42");
        EXPECT_FALSE(s2->verifyAttrPathForTest(expectedCAKey("drv-R4")))
            << "R4 soundness: input change ⇒ persisted producer trace's FileBytes "
               "dep mismatches ⇒ verify must miss. (This is the same chain "
               "that ca-trace-key-routing.cc R3 + ca-producer-scope.cc S3 pin "
               "from synthetic angles; this test pins it through the real "
               "TraceSession::recordCAProducer entry point.)";
    }
}

// ── R5: edgeOut out-param (RFC §9 B1 / §11 Finding 5) ────────────────────────
//
// The aggressive shape needs `{caKey, traceHash}` from recordCAProducer to emit
// the consumer's TraceValueContext edge. It must be populated on BOTH paths:
//  (a) the fresh-record path, and
//  (b) the per-session DEDUP-HIT path (a SECOND cold consumer of the same
//      producer in one session — the 607× sharing case). If the dedup-hit path
//      left edgeOut at its default {caKey=0(root), traceHash=0}, the second
//      consumer would emit an edge to the ROOT trace and mis-resolve.
TEST_F(TraceSessionRecordCAProducerTest, RecordCAProducer_EdgeOut_PopulatedOnFreshAndDedupHit)
{
    TempTextFile inputSrc("R5-v1");
    auto session = makeCache("42");
    forceRoot(*session);
    auto deps = captureProducerInputs([&](auto & ctx) {
        ctx.record(makeContentDep(state.tracingPools(), inputSrc.path.string(), "R5-v1"));
    });

    // First (fresh-record) call — edgeOut populated from the freshly persisted trace.
    Value v;
    v.mkAttrs(state.buildBindings(0, EmptyBindingsAllocation::AllocateFresh).finish());
    TraceSession::ProducerEdge edgeFresh;
    ASSERT_TRUE(session->recordCAProducer(v, "drv-R5", deps, &edgeFresh));
    EXPECT_EQ(edgeFresh.caKey.value, expectedCAKey("drv-R5").value)
        << "R5(a): fresh-record edgeOut.caKey must be the producer's CA key";
    EXPECT_NE(edgeFresh.caKey.value, AttrVocabStore::rootPath().value)
        << "R5(a): edgeOut.caKey must NOT be the root (caKey=0) sentinel";
    EXPECT_NE(edgeFresh.traceHash.value, EvalTraceHash{})
        << "R5(a): fresh-record edgeOut.traceHash must be the real producer hash";

    // Second call, SAME Bindings* ⇒ dedup-hit path (trace-session.cc:849). edgeOut
    // must be supplied from the side-table, matching the fresh-record values.
    TraceSession::ProducerEdge edgeDedup;
    ASSERT_TRUE(session->recordCAProducer(v, "drv-R5", deps, &edgeDedup));
    EXPECT_EQ(edgeDedup.caKey.value, edgeFresh.caKey.value)
        << "R5(b): dedup-hit edgeOut.caKey must match the fresh-record caKey "
           "(NOT the default root sentinel — Finding 5 regression guard)";
    EXPECT_EQ(edgeDedup.traceHash.value, edgeFresh.traceHash.value)
        << "R5(b): dedup-hit edgeOut.traceHash must match the fresh-record hash";
}

} // namespace nix::eval_trace
