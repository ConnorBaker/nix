/**
 * RECORDING-SIDE feasibility floor for the content-addressed-trace-identity RFC §3b
 * (the producer-trace boundary at `derivationStrict`).
 *
 * The consume side is already proven (`store/ca-trace-key-routing.cc`): the store
 * routes a CA-keyed producer trace and verifies a consumer edge to it. The
 * scope-isolation side is already proven (`dep/child-range-exclusion.cc`): a nested
 * DepRecordingContext scope collects ONLY its own deps and does not leak them to the
 * parent. What NEITHER tests is the COMPOSITION the recorder change actually needs:
 *
 *   Can the recording machinery, using ONLY the existing scope + store primitives,
 *   (1) capture a sub-computation's input-reads in an ISOLATED nested scope,
 *   (2) FINALIZE those isolated deps into a CA-keyed producer trace, and
 *   (3) leave ONLY a single edge (TraceValueContext → the CA key) in the parent's
 *       dep set — NOT the flattened inner deps —
 *   such that the parent verifies through the edge, AND a change to the producer's
 *   input invalidates the parent THROUGH the edge?
 *
 * This is the EXACT shape §3b proposes: open a DepCaptureScope around the
 * derivation's input force, record a producer trace keyed by `CATraceKey(drvPath)`,
 * and have the consumer record one edge instead of the flattened closure. These
 * tests drive that shape with the REAL `DepRecordingContext` + REAL `SqliteTraceStorage`,
 * via the test-only scope access — WITHOUT touching the evaluator hot path. If the
 * composition fails here, §3b is blocked at the recording layer; if it passes, the
 * only remaining unknown is the hot-path COST of opening the scope (RFC §7), not the
 * SHAPE.
 *
 * Synthetic in the sense that the deps are hand-recorded into the context (mirroring
 * keyset-escape.cc / ca-trace-key-routing.cc), but the SCOPE LIFECYCLE is real:
 * pushScope/record/takeDeps/popScope are the same calls a recorder would make.
 *
 * P1 — the boundary composes: producer trace persists from the isolated inner scope;
 *      parent holds exactly one edge dep (+ its own observation), NOT the flattened
 *      inner deps; parent verifies through it.
 * P2 — the flattening is COLLAPSED and INDEPENDENT of closure size: running the same
 *      composition with a 2-dep and a 50-dep producer closure yields the SAME
 *      consumer dep count (own + one edge) — the 607×→1 collapse the RFC targets.
 * P3 — SOUNDNESS through the edge: mutating the producer's captured input changes
 *      the producer trace hash ⇒ the parent's edge (which stored the OLD hash)
 *      mismatches ⇒ parent invalidates. This is the property the flattening
 *      currently provides by inlining; the edge must preserve it.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// The CA routing key, identical in construction to ca-trace-key-routing.cc: a
// single-component path off root named "__ca:<drvHash>". Position-independent.
static AttrPathId caKey(AttrVocabStore & vocab, std::string_view drvHash)
{
    return vocab.internPath(
        AttrVocabStore::rootPath(),
        vocab.internName(std::string("__ca:") + std::string(drvHash)));
}

// ── P1: the producer boundary composes from real scope + store primitives ────

TEST_F(TraceStoreTest, CAProducerBoundary_ScopeIsolatedProducer_ParentEdgesNotFlattens)
{
    // Real files for every dep that goes through verify(): the consumer's verify
    // resolves its own deps AND recursively resolves the producer's deps via
    // resolveTraceContextHash. A synthetic non-existent path on EITHER side causes
    // a hard miss regardless of edge logic.
    TempTextFile consumerOwn("consumer-own-v1");
    TempTextFile inputA("drv-input-a-v1");
    TempTextFile inputB("drv-input-b-v1");
    auto & p = pools();
    auto db = makeDb();

    // The producer's CA identity (in reality: CATraceKey(drvPath)). The consumer
    // computes the SAME key from the observed drvPath — here we just share it.
    auto producer = caKey(testVocab(), "drv-abc123");

    // Drive a REAL DepRecordingContext exactly as a §3b recorder would:
    //   - outer scope = the consumer (the attr-path node that forced the drv)
    //   - inner scope = the producer boundary (derivationStrict's input force)
    std::vector<Dep> innerDeps, outerDeps;
    {
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);             // consumer scope

        // The consumer does some of its own observation BEFORE forcing the drv —
        // a real file dep so the verify of the consumer's trace can resolve it.
        ctx.record(makeContentDep(p, consumerOwn.path.string(), "consumer-own-v1"));

        {
            TestScopeAccess::pushScope(ctx);         // producer boundary scope
            // The derivation's input-reads — captured in ISOLATION. In reality
            // these are the ~thousands of FileBytes/StorePathAvailability deps that
            // today flatten into the consumer. Two real files so the producer trace
            // resolves cleanly through resolveTraceContextHash on consumer verify.
            ctx.record(makeContentDep(p, inputA.path.string(), "drv-input-a-v1"));
            ctx.record(makeContentDep(p, inputB.path.string(), "drv-input-b-v1"));
            innerDeps = TestScopeAccess::takeDeps(ctx);
            TestScopeAccess::popScope(ctx);
        }

        // The recorder now PERSISTS the isolated inner deps as the CA-keyed producer
        // trace, and records ONE edge into the consumer's (outer) scope.
        auto pHash = withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, producer, string_t{"drv-out", {}}, innerDeps);
            return db->getCurrentTraceHash(ea, producer);
        });
        ASSERT_TRUE(pHash.has_value());

        // THE BOUNDARY MOVE: the consumer records an edge, NOT the flattened inner
        // deps. (A real recorder emits this in place of the replayMemoizedRange copy.)
        ctx.record(Dep::makeValueContext(producer, DepHashValue(DepHash{pHash->value})));

        outerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
    }

    // (a) The producer trace captured the inner closure (2 input deps).
    EXPECT_EQ(innerDeps.size(), 2u)
        << "the producer boundary must capture the derivation's input-reads in isolation";

    // (b) The consumer's dep set is its OWN observation + exactly ONE edge — the
    //     inner deps are NOT present (no flattening).
    ASSERT_EQ(outerDeps.size(), 2u)
        << "consumer must hold its own dep + one edge, NOT the flattened producer closure";
    size_t edgeCount = 0, innerLeak = 0;
    for (const auto & d : outerDeps) {
        if (d.key.kind == CanonicalQueryKind::TraceValueContext) {
            ++edgeCount;
            EXPECT_EQ(d.traceContextPath().value, producer.value)
                << "the edge must target the CA producer key";
        }
        // An inner dep key leaking into the parent would be the flattening bug.
        for (const auto & inner : innerDeps)
            if (inner.key == d.key) ++innerLeak;
    }
    EXPECT_EQ(edgeCount, 1u) << "exactly one producer edge in the consumer";
    EXPECT_EQ(innerLeak, 0u) << "no producer-input dep may flatten into the consumer";

    // (c) Persist the consumer trace and verify it routes through the edge.
    auto consumer = vpath({"pkgs", "app"});
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, consumer, string_t{"app-out", {}}, outerDeps);
    });
    recreateDb(db);
    auto r = test::TraceStorageTestAccess::verify(*db, consumer, state);
    ASSERT_TRUE(r.has_value())
        << "P1: the consumer must verify through the producer edge + its own dep";
    EXPECT_EQ(std::get<string_t>(r->value).first, "app-out");
}

// ── P2: the consumer's dep count is INDEPENDENT of the producer closure size ──
//
// The RFC's headline claim is that a derivation with a closure of M input deps
// records the M deps ONCE (in the producer) and is referenced by a single edge per
// consumer (607× → 1). This test pins the collapse directly: run the SAME boundary
// composition with M=2 and M=50, and assert the consumer's recorded dep set is the
// same size both times (its own dep + one edge). Today's flattening would make the
// consumer's size scale with M (it would inline all M deps).

TEST_F(TraceStoreTest, CAProducerBoundary_ConsumerDepCount_IndependentOfClosureSize)
{
    auto & p = pools();
    auto db = makeDb();

    // Run the producer-boundary composition for a closure of `M` input deps and a
    // distinct producer key; return the consumer's recorded dep-set size.
    auto consumerDepCount = [&](int M, std::string_view drvHash) -> size_t {
        auto producer = caKey(testVocab(), drvHash);
        std::vector<Dep> innerDeps, outerDeps;
        {
            std::vector<Dep> epochLog;
            DepRecordingContext ctx(p, epochLog);
            TestScopeAccess::pushScope(ctx);                 // consumer scope
            ctx.record(makeContentDep(p, "/consumer-own.nix", "co"));   // own observation
            {
                TestScopeAccess::pushScope(ctx);             // producer boundary
                for (int i = 0; i < M; ++i)
                    ctx.record(makeContentDep(p,
                        "/closure-" + std::string(drvHash) + "-" + std::to_string(i) + ".nix",
                        "c" + std::to_string(i)));
                innerDeps = TestScopeAccess::takeDeps(ctx);
                TestScopeAccess::popScope(ctx);
            }
            auto pHash = withExclusiveStore(*db, [&](const auto & ea) {
                db->record(ea, producer, string_t{"out", {}}, innerDeps);
                return db->getCurrentTraceHash(ea, producer);
            });
            EXPECT_TRUE(pHash.has_value());
            ctx.record(Dep::makeValueContext(producer, DepHashValue(DepHash{pHash->value})));
            outerDeps = TestScopeAccess::takeDeps(ctx);
            TestScopeAccess::popScope(ctx);
        }
        // Sanity: the producer captured the full closure.
        EXPECT_EQ(innerDeps.size(), static_cast<size_t>(M));
        return outerDeps.size();
    };

    auto small = consumerDepCount(2, "small-drv");
    auto large = consumerDepCount(50, "large-drv");

    EXPECT_EQ(small, large)
        << "P2: consumer dep count must NOT scale with producer closure size — the "
           "edge collapses the closure to one reference (607×→1)";
    EXPECT_EQ(large, 2u)
        << "P2: regardless of closure size, the consumer holds exactly its own dep "
           "+ one edge";
}

// ── P3: SOUNDNESS through the boundary — producer input change invalidates the
//    consumer via the edge (the property the flattening provides by inlining; the
//    edge must preserve it end-to-end through the real scope lifecycle). ─────────

TEST_F(TraceStoreTest, CAProducerBoundary_ProducerInputChange_InvalidatesConsumerViaEdge)
{
    TempTextFile inputSrc("input-v1");
    auto & p = pools();
    auto db = makeDb();
    auto producer = caKey(testVocab(), "sound-drv");

    // Record the producer from a REAL isolated scope (matching the boundary shape),
    // then capture its trace hash for the consumer's edge.
    std::vector<Dep> innerDeps;
    {
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);
        // Content string matches the file's current bytes (cold record), exactly as
        // ca-trace-key-routing.cc R3 does — the live-file re-resolution at verify is
        // what drives soundness.
        ctx.record(makeContentDep(p, inputSrc.path.string(), "input-v1"));
        innerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
    }
    auto pHash = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, producer, string_t{"out", {}}, innerDeps);
        return db->getCurrentTraceHash(ea, producer);
    });
    ASSERT_TRUE(pHash.has_value());

    auto consumer = vpath({"pkgs", "consumer"});
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, consumer, string_t{"consumer-out", {}},
            {Dep::makeValueContext(producer, DepHashValue(DepHash{pHash->value}))});
    });

    // Precondition: unchanged producer ⇒ consumer hits via the edge.
    recreateDb(db);
    ASSERT_TRUE(test::TraceStorageTestAccess::verify(*db, consumer, state).has_value())
        << "precondition: unchanged producer input ⇒ consumer hits via the edge";

    // Mutate the producer's captured input. resolveTraceContextHash recomputes the
    // producer's CURRENT hash from the live (mutated) file; it no longer matches the
    // OLD hash stored in the consumer's edge ⇒ verification must miss.
    inputSrc.modify("input-v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto r = test::TraceStorageTestAccess::verify(*db, consumer, state);
        EXPECT_FALSE(r.has_value())
            << "P3: producer input changed ⇒ producer trace hash changed ⇒ the "
               "consumer's edge (storing the OLD hash) must invalidate — soundness "
               "is preserved through the boundary, not lost by dropping the "
               "flattened inner deps";
    }
}

// ── P4: NESTED producer boundaries — the recursion that keeps the model
//    tractable. RFC §4(c): a child of a producer becomes an EDGE only if it is
//    identity-bearing; otherwise its deps flatten. With derivations as the
//    identity-bearing kind, derivation A whose input includes derivation B's
//    outPath stores an edge to B's CA producer trace, NOT B's whole closure.
//    Soundness: a change deep inside B's input must propagate up the chain
//    (B's hash changes → A's edge to B mismatches → A's hash changes → A's
//    consumer's edge to A mismatches → consumer invalidates). Test the chain
//    at depth 2 with REAL scope nesting.

TEST_F(TraceStoreTest, CAProducerBoundary_NestedProducers_ChainInvalidatesFromDeepest)
{
    TempTextFile bInput("b-input-v1");                    // B's deepest input
    auto & p = pools();
    auto db = makeDb();
    auto producerB = caKey(testVocab(), "drv-B-deepest");
    auto producerA = caKey(testVocab(), "drv-A-mid");

    // ── Record producer B from its own isolated boundary scope ───────────────
    std::vector<Dep> bInnerDeps;
    {
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(p, bInput.path.string(), "b-input-v1"));
        bInnerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
    }
    auto bHash = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, producerB, string_t{"b-out", {}}, bInnerDeps);
        return db->getCurrentTraceHash(ea, producerB);
    });
    ASSERT_TRUE(bHash.has_value());

    // ── Record producer A — its inner scope captures ONLY an edge to B (not
    //    B's flattened closure). This is the recursion: A's "input-reads"
    //    include "I read B's outPath," which the recorder represents as an edge
    //    to B's CA producer, NOT as a copy of B's deps. ──────────────────────
    std::vector<Dep> aInnerDeps;
    {
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);
        ctx.record(Dep::makeValueContext(producerB, DepHashValue(DepHash{bHash->value})));
        aInnerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
    }
    // A's producer trace's stored deps are exactly ONE edge — B's closure does
    // not flatten into A. This is the bounded-recursion claim made concrete.
    ASSERT_EQ(aInnerDeps.size(), 1u)
        << "P4 nesting: producer A's stored deps are exactly one edge to B, "
           "not B's flattened closure";
    EXPECT_EQ(aInnerDeps[0].key.kind, CanonicalQueryKind::TraceValueContext);

    auto aHash = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, producerA, string_t{"a-out", {}}, aInnerDeps);
        return db->getCurrentTraceHash(ea, producerA);
    });
    ASSERT_TRUE(aHash.has_value());

    // ── Record the consumer C, whose only dep is an edge to A. ───────────────
    auto consumer = vpath({"pkgs", "c"});
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, consumer, string_t{"c-out", {}},
            {Dep::makeValueContext(producerA, DepHashValue(DepHash{aHash->value}))});
    });

    // Precondition: full chain unchanged ⇒ consumer hits.
    recreateDb(db);
    ASSERT_TRUE(test::TraceStorageTestAccess::verify(*db, consumer, state).has_value())
        << "precondition: full chain unchanged ⇒ consumer hits via the C→A→B edge chain";

    // Mutate B's deepest input. The chain must propagate: B's trace hash
    // changes (its content dep mismatches) ⇒ A's edge to B (storing OLD bHash)
    // mismatches ⇒ A's trace hash changes ⇒ C's edge to A (storing OLD aHash)
    // mismatches ⇒ C invalidates. resolveTraceContextHash is recursive
    // (verifier.cc:243-278) — this is the build-layer's hashDerivationModulo
    // recursion lifted to eval, exercised through the real verifier.
    bInput.modify("b-input-v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto r = test::TraceStorageTestAccess::verify(*db, consumer, state);
        EXPECT_FALSE(r.has_value())
            << "P4 nesting soundness: a change at depth 2 (B's deepest input) "
               "must propagate up the entire C→A→B edge chain — recursion that "
               "bottoms out at content-addressed nodes (RFC §4(c))";
    }
}

// ── P5: edge IDEMPOTENCY — recording the same producer-edge twice within a
//    consumer scope must deduplicate to ONE stored dep. Today's
//    DepRecordingContext::record dedupes on Dep::Key (an edge is keyed by
//    (TraceValueContext, AttrPathId(producer))), so a derivation forced twice
//    inside one consumer (the typical case for shared sub-results) must NOT
//    produce two edge deps. If this failed, it would either bloat the
//    consumer's stored dep set or — worse — cause hash conflicts via
//    `seenDeps` if the second record hashed differently. ──────────────────────

TEST_F(TraceStoreTest, CAProducerBoundary_RepeatedEdgeRecord_DedupesToOne)
{
    TempTextFile inputSrc("input");
    auto & p = pools();
    auto db = makeDb();
    auto producer = caKey(testVocab(), "drv-shared");

    // Record a producer; capture its hash.
    auto pHash = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, producer, string_t{"out", {}}, {
            makeContentDep(p, inputSrc.path.string(), "input"),
        });
        return db->getCurrentTraceHash(ea, producer);
    });
    ASSERT_TRUE(pHash.has_value());

    // Drive a real consumer scope and record the SAME edge dep twice.
    std::vector<Dep> outerDeps;
    {
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);
        auto edge = Dep::makeValueContext(producer, DepHashValue(DepHash{pHash->value}));
        ctx.record(edge);
        ctx.record(edge);                                        // duplicate
        ctx.record(edge);                                        // and again
        outerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
    }

    // Expect exactly ONE edge — DepRecordingContext::record dedupes on Dep::Key.
    EXPECT_EQ(outerDeps.size(), 1u)
        << "P5 idempotency: duplicate ctx.record(edge) calls within one scope "
           "must dedupe to one entry — recorder is keyed by (kind, attrPathId), "
           "matching the content-dep dedup contract";
    if (!outerDeps.empty())
        EXPECT_EQ(outerDeps[0].key.kind, CanonicalQueryKind::TraceValueContext);
}

// ── P6: PRODUCER-SIDE-TABLE design — the missing piece between scope-isolated
//    recording (P1-P5, proven) and the live evaluator hot path. The §3b
//    recorder needs to route a Value's force between:
//      (a) flatten-replay (today's replayMemoizedRange path): for non-producer
//          Values, copy the value's epoch-log range into the current scope;
//      (b) edge-emission (the new path): for producer Values (derivations
//          whose force opened a CA-keyed producer scope), record ONLY a
//          TraceValueContext edge to the producer's CA key.
//    `MemoReplayStore::epochMap` (Value* → DepRange) lacks producer-identity
//    info — it knows the deps that grew, not which CA key (if any) bounds them.
//    This test sketches the SIDE-TABLE approach: an auxiliary map (Value* →
//    {AttrPathId, TraceHash}) populated when a producer scope finalizes. A
//    recorder consulting this side table BEFORE replayMemoizedRange routes
//    correctly. The test drives that routing with REAL DepRecordingContext
//    forces and asserts the consumer's stored deps reflect the routing
//    decision per Value. No production change yet — this PROVES the side
//    table's interface is sufficient. ──────────────────────────────────────

TEST_F(TraceStoreTest, CAProducerBoundary_ProducerSideTable_RoutesEdgeVsFlatten)
{
    // Keep input files alive for the whole test — verify-time resolution
    // requires the producer's input file to still exist.
    TempTextFile producerInput("v1");
    TempTextFile npFile("np");
    auto & p = pools();
    auto db = makeDb();
    auto producer = caKey(testVocab(), "drv-routed");

    // Simulate a "producer side table": Value-pointer-keyed map that the
    // recorder would consult at force time to decide edge vs flatten. In
    // production this would live in MemoReplayStore (or alongside it). Here
    // we model it with a plain map keyed by an opaque "value handle" (a
    // string ID) since we don't have real Value*s to manipulate from synthetic
    // dep-recording — what matters is that the LOOKUP GATE works.
    struct ProducerEntry { AttrPathId caKey; DepHash traceHash; };
    std::unordered_map<std::string, ProducerEntry> producerTable;

    // Record the producer trace from an isolated scope and populate the
    // side table. (In production: this happens inside the producer-scope
    // finalization, keyed by the derivation's Value*.)
    {
        std::vector<Dep> innerDeps;
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);
        ctx.record(makeContentDep(p, producerInput.path.string(), "v1"));
        innerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
        auto pHash = withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, producer, string_t{"out", {}}, innerDeps);
            return db->getCurrentTraceHash(ea, producer);
        });
        ASSERT_TRUE(pHash.has_value());
        producerTable["drv-result-handle"] = {producer, DepHash{pHash->value}};
    }

    // The recorder gate: given a "force handle," route between edge-emission
    // (handle is in producerTable ⇒ record edge) and flatten-replay (otherwise
    // ⇒ would call replayMemoizedRange in production; here we simulate by
    // inlining a sentinel content dep so the test can observe the routing).
    auto forceAndRoute = [&](DepRecordingContext & ctx, std::string_view handle,
                             const std::vector<Dep> & flattenedDeps) {
        auto it = producerTable.find(std::string(handle));
        if (it != producerTable.end()) {
            ctx.record(Dep::makeValueContext(it->second.caKey,
                                              DepHashValue(it->second.traceHash)));
        } else {
            for (const auto & d : flattenedDeps) ctx.record(d);
        }
    };

    // Consumer scope: forces TWO values — one a producer (→ edge), one a
    // non-producer (→ flatten). The recorder gate makes the routing decision.
    std::vector<Dep> outerDeps;
    {
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);
        // Force #1: a derivation result. handle is in the side table.
        forceAndRoute(ctx, "drv-result-handle", /*unused*/ {});
        // Force #2: an ordinary thunk's flattened range. Not in the side table
        // ⇒ flatten as today. (Real-file content dep stands in for an arbitrary
        // flattened range; production would call replayMemoizedRange.)
        std::vector<Dep> nonProducerRange = {makeContentDep(p, npFile.path.string(), "np")};
        forceAndRoute(ctx, "ordinary-thunk-handle", nonProducerRange);
        outerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);
    }

    // Verify the routing produced the expected dep shape:
    //   - one edge dep (producer routing fired);
    //   - one content dep (non-producer was flattened as today);
    //   - the producer's "v1" content dep is NOT present (the edge replaced
    //     the flatten for the producer).
    ASSERT_EQ(outerDeps.size(), 2u)
        << "P6 routing: consumer holds ONE edge + ONE flattened content dep "
           "— the producer-side-table gate replaces the producer's flatten "
           "with an edge while the non-producer continues to flatten";
    size_t edgeCount = 0, contentCount = 0;
    for (const auto & d : outerDeps) {
        if (d.key.kind == CanonicalQueryKind::TraceValueContext) ++edgeCount;
        if (d.key.kind == CanonicalQueryKind::FileBytes) ++contentCount;
    }
    EXPECT_EQ(edgeCount, 1u);
    EXPECT_EQ(contentCount, 1u);

    // Persist the consumer + verify it routes through the edge for the
    // producer half AND through the flattened content dep for the non-producer
    // half. Together this confirms the side-table interface — Value* →
    // (caKey, traceHash) — is SUFFICIENT for the recorder gate. The remaining
    // production work is plumbing it into MemoReplayStore + replayMemoizedRange.
    auto consumer = vpath({"pkgs", "routed"});
    {
        // Re-record the non-producer dep against the real `npFile` so the
        // consumer verify resolves it.
        outerDeps.clear();
        std::vector<Dep> epochLog;
        DepRecordingContext ctx(p, epochLog);
        TestScopeAccess::pushScope(ctx);
        forceAndRoute(ctx, "drv-result-handle", {});
        std::vector<Dep> realRange = {makeContentDep(p, npFile.path.string(), "np")};
        forceAndRoute(ctx, "ordinary-thunk-handle", realRange);
        outerDeps = TestScopeAccess::takeDeps(ctx);
        TestScopeAccess::popScope(ctx);

        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, consumer, string_t{"out", {}}, outerDeps);
        });
        recreateDb(db);
        auto r = test::TraceStorageTestAccess::verify(*db, consumer, state);
        ASSERT_TRUE(r.has_value())
            << "P6 routing: consumer must verify through the edge + the "
               "flattened content dep — confirms the side-table interface "
               "produces a sound mixed-routing trace";
    }
}

} // namespace nix::eval_trace
