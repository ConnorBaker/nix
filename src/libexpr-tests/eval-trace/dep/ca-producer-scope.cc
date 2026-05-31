/**
 * `CAProducerScope` helper test (RFC §3b boundary recorder).
 *
 * Builds on the prior slices:
 * - `dep/producer-side-table.cc`: producerMap API (in-memory binding).
 * - `dep/replay-producer-gate.cc`: replayMemoizedDeps gate fires when a
 *   bound Value is re-forced.
 * - `store/ca-producer-boundary-recording.cc`: hand-rolled scope composition.
 *
 * This file pins the SMALLEST production helper that combines them: a
 * scoped class that opens a sub-scope, captures inputs in isolation,
 * persists them as a CA-keyed producer trace via SqliteTraceStorage, and
 * registers the producer Value→{caKey, hash} binding so subsequent
 * re-forces of the Value emit an edge.
 *
 * The helper is the SHAPE the §3b primops.cc hook needs — wrapping a
 * derivation's input force + derivationStrictInternal in this RAII scope
 * IS the producer boundary.
 *
 * S1 RAII round-trip: open scope, record inner deps, close scope ⇒ producer
 *    trace persisted at CA key + producerMap bound + scope's deps not in
 *    the parent.
 * S2 Bound Value's subsequent re-force in a recording scope ⇒ replay gate
 *    fires (via replayMemoizedDeps) ⇒ edge emitted, no flatten.
 * S3 Soundness: the producer trace verifies; mutation of producer's input
 *    invalidates a consumer that holds the edge.
 */
#include "eval-trace/helpers.hh"
#include "eval-trace/trace-runtime-test-access.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/eval-trace/deps/memo-replay-store.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/value.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

static AttrPathId caKey(AttrVocabStore & vocab, std::string_view drvHash)
{
    return vocab.internPath(
        AttrVocabStore::rootPath(),
        vocab.internName(std::string("__ca:") + std::string(drvHash)));
}

// ── S1: open scope, finalize as CA producer, register in producerMap ────────
//
// This test pins the AGGRESSIVE-shape composition: a nested DepCaptureScope
// captures the producer's input-reads in isolation, the producer trace
// persists those isolated deps, and the consumer scope holds own + edge
// (NOT the flattened producer deps). This shape was tried in production
// (`prim_derivationStrict` with sub-scope isolation) and REVERTED —
// the `forceAttrs(*args[0])` inside derivationStrict pulls in ambient-
// eval deps that legitimately belong to the consumer, and isolating them
// under-records. Production today uses the conservative shape (epoch-
// range snapshot WITHOUT isolation; consumer keeps its flattened deps).
//
// S1 is retained as a regression guard for the recording-side primitives:
// `pushScope`/`takeDeps`/`registerProducer`/`recordSync` compose cleanly
// when the caller chooses to isolate. If a future caller needs aggressive
// shape (e.g., an alternative producer hook with stronger isolation
// guarantees), this test pins that the primitives support it.

TEST_F(TraceStoreTest, CAProducerScope_OpenFinalizeRegister_RoundTrips)
{
    TempTextFile inputSrc("scope-v1");
    auto & p = pools();
    auto db = makeDb();

    // The Value the producer scope will be associated with — a derivation
    // result in production. Here it's a plain int Value.
    Value producerV;
    producerV.mkInt(42);

    // Drive the boundary in three phases:
    //   (1) outer (consumer) scope active before the producer fires;
    //   (2) producer sub-scope captures the derivation's input-reads;
    //   (3) finalize: persist as CA producer trace, register Value*.
    std::vector<Dep> outerDeps;
    {
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);             // outer / consumer scope

        // Consumer's own pre-producer observations.
        ctx.record(makeContentDep(p, "/consumer-pre.nix", "co"));

        // Inline §3b boundary recorder shape — this is what
        // CAProducerScope's constructor + destructor will encapsulate.
        std::vector<Dep> innerDeps;
        AttrPathId producerKey = caKey(testVocab(), "drv-S1");
        std::optional<TraceHash> pHash;
        {
            TestScopeAccess::pushScope(ctx);          // PRODUCER sub-scope
            // Producer captures its own input-reads (forceAttrs + derivationStrictInternal).
            ctx.record(makeContentDep(p, inputSrc.path.string(), "scope-v1"));
            innerDeps = TestScopeAccess::takeDeps(ctx);
            TestScopeAccess::popScope(ctx);
        }
        // Finalize: persist as CA producer trace, capture trace hash.
        pHash = withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, producerKey, string_t{"drv-out", {}}, innerDeps);
            return db->getCurrentTraceHash(ea, producerKey);
        });
        ASSERT_TRUE(pHash.has_value());

        // Register Value* → {caKey, traceHash} in the producer side-table.
        TraceRuntimeTestAccess::registerProducer(
            *state.traceCtx, producerV, producerKey,
            DepHash{pHash->value});

        // The consumer ALSO records the edge into its own scope (manual here
        // because no replay re-forces producerV in this test — see S2 below
        // for the replay-driven fire). In production §3b the consumer's edge
        // is emitted by the gate when replayMemoizedDeps re-forces v.
        ctx.record(Dep::makeValueContext(producerKey, DepHashValue(DepHash{pHash->value})));

        outerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
    }

    // (a) Outer scope holds: own dep + edge — NOT producer's flattened deps.
    EXPECT_EQ(outerDeps.size(), 2u)
        << "S1: consumer holds own observation + producer edge";
    bool foundEdge = false;
    for (const auto & d : outerDeps)
        if (d.key.kind == CanonicalQueryKind::TraceValueContext) {
            foundEdge = true;
            EXPECT_EQ(d.traceContextPath().value, caKey(testVocab(), "drv-S1").value);
        }
    EXPECT_TRUE(foundEdge);

    // (b) The producer side-table is populated.
    auto bound = TraceRuntimeTestAccess::lookupProducer(*state.traceCtx, producerV);
    ASSERT_TRUE(bound.has_value());

    // (c) The producer trace verifies through the existing pipeline.
    recreateDb(db);
    auto producerVerify = test::TraceStorageTestAccess::verify(
        *db, caKey(testVocab(), "drv-S1"), state);
    EXPECT_TRUE(producerVerify.has_value())
        << "S1: the persisted CA producer trace must verify after a fresh "
           "session";
}

// ── S2: replay gate fires for a bound producer, AFTER a future re-force ─────
//
// The gate's interaction with the side-table: we don't drive forceThunkValue
// (no real derivation here), but we exercise replayMemoizedDeps directly with
// a registered Value, mirroring the call site in eval.cc:1693.

TEST_F(TraceStoreTest, CAProducerScope_BoundValue_ReplayGateEmitsEdge)
{
    auto & p = pools();
    auto producerKey = caKey(testVocab(), "drv-S2");
    DepHash pHash{depHash("S2-trace-hash")};

    Value producerV;
    producerV.mkInt(1);
    TraceRuntimeTestAccess::registerProducer(
        *state.traceCtx, producerV, producerKey, pHash);

    // Consumer scope: re-forces producerV ⇒ replay gate fires.
    std::vector<Dep> consumerDeps;
    {
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);
        StandaloneDepCtxGuard guard(ctx);
        TraceRuntimeTestAccess::replayMemoizedDeps(*state.traceCtx, producerV);
        consumerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
    }

    ASSERT_EQ(consumerDeps.size(), 1u)
        << "S2: replay gate emitted exactly one dep — the producer edge";
    EXPECT_EQ(consumerDeps[0].key.kind, CanonicalQueryKind::TraceValueContext);
    EXPECT_EQ(consumerDeps[0].traceContextPath().value, producerKey.value);
}

// ── S3: end-to-end soundness through the boundary + gate ────────────────────
//
// Drive the full chain: persist the producer; register Value*; record a
// consumer trace that holds the edge (whether emitted manually or via the
// gate). Then mutate the producer's input. The consumer's stored edge holds
// the OLD producer hash; verification recomputes via resolveTraceContextHash
// against the live (mutated) input ⇒ mismatch ⇒ consumer invalidates.

TEST_F(TraceStoreTest, CAProducerScope_ProducerInputMutation_InvalidatesConsumerViaGate)
{
    TempTextFile inputSrc("v1");
    auto & p = pools();
    auto db = makeDb();
    auto producerKey = caKey(testVocab(), "drv-S3");

    // Step 1: persist producer trace with v1 input.
    std::optional<TraceHash> pHash;
    {
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(p, inputSrc.path.string(), "v1"));
        auto innerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
        pHash = withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, producerKey, string_t{"out", {}}, innerDeps);
            return db->getCurrentTraceHash(ea, producerKey);
        });
    }
    ASSERT_TRUE(pHash.has_value());

    // Step 2: register producer Value*.
    Value producerV;
    producerV.mkInt(0);
    TraceRuntimeTestAccess::registerProducer(
        *state.traceCtx, producerV, producerKey, DepHash{pHash->value});

    // Step 3: consumer scope re-forces producerV ⇒ edge emitted by gate.
    auto consumerPath = vpath({"pkgs", "consumer"});
    {
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);
        StandaloneDepCtxGuard guard(ctx);
        TraceRuntimeTestAccess::replayMemoizedDeps(*state.traceCtx, producerV);
        auto consumerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);

        // Persist consumer trace.
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, consumerPath, string_t{"consumer-out", {}}, consumerDeps);
        });
    }

    // Precondition: unchanged producer input ⇒ consumer hits.
    recreateDb(db);
    ASSERT_TRUE(test::TraceStorageTestAccess::verify(*db, consumerPath, state).has_value())
        << "precondition: with the producer input unchanged, consumer must "
           "verify through the edge emitted by the gate";

    // Mutate producer's input ⇒ producer trace hash changes ⇒ consumer's
    // edge mismatches via resolveTraceContextHash.
    inputSrc.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto r = test::TraceStorageTestAccess::verify(*db, consumerPath, state);
        EXPECT_FALSE(r.has_value())
            << "S3 soundness: producer input changed ⇒ consumer's edge "
               "(emitted by the replay gate) must invalidate via "
               "resolveTraceContextHash";
    }
}

} // namespace nix::eval_trace
