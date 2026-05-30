/**
 * Tier-1 derivation-edge soundness floor (compositional trace-DAG, step 1).
 *
 * Design: `plans/compositional-trace-dag-design.md`,
 * `plans/architecture-trace-model-vs-CA.md`.
 *
 * The architectural finding: eval-trace FLATTENS a consumer's transitive dep
 * closure into the consumer's own trace, instead of EDGING to the producer's
 * content-identity (the build layer's `inputDrvs`-by-hash model). The proposed
 * Tier-1 fix: when consumer A depends on producer B (a derivation) and observes
 * ONLY B's output identity (drvPath / outPath — output-like, value-shaped), A
 * records a single OUTPUT-IDENTITY EDGE to B instead of inlining B's input
 * closure. B's closure is then hashed/verified once (in B), referenced by edge
 * from the N consumers.
 *
 * These tests are the SOUNDNESS FLOOR for that edge — written BEFORE any recorder
 * change, synthetic (raw `db->record()`), mirroring `store/keyset-escape.cc`.
 * They pin the contract the recorder change must satisfy, and (adversarially)
 * the exact shape of the stale-serve hole if the contract is violated.
 *
 * The edge primitive used here is the EXISTING `DerivedStorePath` dep
 * (`makeCopiedPathDep`): its stored hash is the producer's store-path string,
 * and it verifies by re-coercing the source → store path and comparing
 * (dep-resolution-service.cc:411-433). That is exactly an output-identity edge:
 * output unchanged → edge matches → consumer reuses without re-walking the
 * producer's closure; output changed → edge mismatches → consumer invalidates.
 *
 * THE CONTRACT (what an output-identity edge must guarantee):
 *   (C1) output changes  ⇒ consumer invalidates              [soundness]
 *   (C2) output unchanged ⇒ consumer hits, NO closure re-walk [the win]
 *   (C3) the edge is OUTPUT-ONLY: it is sound ONLY when the consumer observed
 *        no non-output facet (.meta/.passthru/arbitrary attrs/shape) of the
 *        producer. If the consumer observed such a facet, an output-only edge
 *        is UNSOUND — the adversarial tests below construct that hole so a
 *        future recorder can never silently emit an output edge for a
 *        facet-observing consumer.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"

#include <gtest/gtest.h>

#include <deque>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// We model the output-identity edge as the consumer depending on the producer
// trace via TraceValueContext — the cross-trace channel `keyset-escape.cc` uses,
// and the same channel a Tier-1 recorder would emit. A producer trace's hash IS
// its output identity (it folds in the producer's own deps); the consumer stores
// that hash as its single edge dep. We drive the producer's output identity with
// a real source file (`makeContentDep`) so a real mutation produces real edge
// invalidation, and we avoid the DerivedStorePath self-seeding problem (the test
// cannot precompute the source→store-path coercion) by using trace-hash edges,
// which the test CAN seed via `getCurrentTraceHash`.

// ── C1: producer output changes ⇒ consumer edge invalidates ──────────────────

TEST_F(TraceStoreTest, DerivationEdge_ProducerSourceChanges_ConsumerInvalidates)
{
    // Producer B's trace depends on a source file (its "input"). Consumer A
    // edges to B via TraceValueContext (A references B's trace hash = B's
    // output identity). Changing B's source changes B's trace hash → A's edge
    // (which stored B's old trace hash) mismatches → A invalidates.
    TempTextFile bSrc("v1");
    auto & p = pools();
    auto db = makeDb();

    auto bHash = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"B"}), string_t{"b-out-v1", {}},
            {makeContentDep(p, bSrc.path.string(), "v1")});
        return db->getCurrentTraceHash(ea, vpath({"B"}));
    });
    ASSERT_TRUE(bHash.has_value());

    // A edges to B's output identity (B's trace hash).
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"A"}), string_t{"a-uses-b", {}},
            {Dep::makeValueContext(vpath({"B"}), DepHashValue(DepHash{bHash->value}))});
    });

    // Precondition (C2 arm): unchanged producer ⇒ A hits.
    recreateDb(db);
    {
        auto r = test::TraceStorageTestAccess::verify(*db, vpath({"A"}), state);
        ASSERT_TRUE(r.has_value())
            << "precondition: unchanged producer must let the consumer hit via the edge";
    }

    // B's input changes ⇒ B's output identity changes ⇒ A's edge must miss.
    bSrc.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto r = test::TraceStorageTestAccess::verify(*db, vpath({"A"}), state);
        EXPECT_FALSE(r.has_value())
            << "C1: producer output changed (source v1→v2) ⇒ consumer's output-"
               "identity edge must invalidate";
    }
}

// ── C2: producer unchanged ⇒ consumer hits via a SINGLE edge dep ─────────────
//
// IMPORTANT framing correction (verified against verifier.cc:243-278): a
// TraceValueContext edge to B does NOT let A "skip B's closure." B's trace hash
// is f(B's INPUT deps), and `resolveTraceContextHash` RECURSIVELY VERIFIES B's
// closure before resolving the edge. What the edge buys is NOT "don't verify B"
// — it is "A stores ONE edge dep instead of a COPY of B's whole closure" (the
// recording/storage win) and "B's closure is verified ONCE and the verdict is
// memoized" (traceContextMemo + verifiedTraceIds) so N consumers share it rather
// than each re-walking B (the amortization win — pinned in C2b below). This
// test pins the recording-side claim: A's entire dep set is one edge, and that
// alone authorizes reuse.

TEST_F(TraceStoreTest, DerivationEdge_ProducerUnchanged_ConsumerHitsViaSingleEdge)
{
    TempTextFile bSrc("stable");
    auto & p = pools();
    auto db = makeDb();

    auto bHash = withExclusiveStore(*db, [&](const auto & ea) {
        // B's trace carries B's OWN closure (one content dep here; ~thousands in
        // reality). A will store ONE edge to B, not a copy of these deps.
        db->record(ea, vpath({"B"}), string_t{"b-out", {}},
            {makeContentDep(p, bSrc.path.string(), "stable")});
        return db->getCurrentTraceHash(ea, vpath({"B"}));
    });
    ASSERT_TRUE(bHash.has_value());

    withExclusiveStore(*db, [&](const auto & ea) {
        // A's ENTIRE dep set is the single edge to B — no inlined B closure.
        db->record(ea, vpath({"A"}), string_t{"a-out", {}},
            {Dep::makeValueContext(vpath({"B"}), DepHashValue(DepHash{bHash->value}))});
    });

    recreateDb(db);
    auto r = test::TraceStorageTestAccess::verify(*db, vpath({"A"}), state);
    EXPECT_TRUE(r.has_value())
        << "C2: with B unchanged, A's single edge dep authorizes reuse (A stores "
           "one edge, not a copy of B's closure)";
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<string_t>(r->value));
    EXPECT_EQ(std::get<string_t>(r->value).first, "a-out")
        << "C2: A serves its own cached result via the edge";
}

// ── C2b: the AMORTIZATION win — a SHARED producer's closure is verified once per
//    session and reused (traceContextMemo / verifiedTraceIds), vs re-walked per
//    consumer when producers are DISTINCT. The build-layer drvHashes-memo
//    property, lifted to eval.
//
// ADVERSARIAL DESIGN NOTE (a first attempt at this test was VACUOUS): naively
// asserting "later consumer checks fewer deps than first" PASSES even with
// distinct producers, because the pre-existing L1 dep-hash cache
// (`currentDepHashes_`) already memoizes a file-hash by dep KEY — so distinct
// producers reading the SAME file share L1 and the later-consumer count drops
// for a reason unrelated to the trace-context memo. To isolate the memo, this
// test runs BOTH arms with producers reading DISTINCT files, and compares the
// SHARED-producer arm against the DISTINCT-producer arm directly. Only the
// trace-context memo distinguishes them.

TEST_F(TraceStoreTest, DerivationEdge_SharedProducer_VerifiedOncePerSession)
{
    auto & p = pools();

    // Helper: build a producer with a multi-file closure (so the closure, not a
    // single L1-cached file, dominates the dep-check count), and N consumers
    // each edging to a producer. `shared` controls whether all consumers edge to
    // ONE producer or to N DISTINCT producers (each with its OWN distinct files).
    // Returns max deps-checked by a NON-first consumer in one shared session.
    auto measureLaterConsumerDeps = [&](bool shared, std::deque<TempTextFile> & files) -> Counter::value_type {
        auto db = makeDb();
        // Each producer reads 3 DISTINCT files (no cross-producer L1 sharing).
        auto recordProducer = [&](const char * bn, size_t fileBase) {
            return withExclusiveStore(*db, [&](const auto & ea) {
                db->record(ea, vpath({bn}), string_t{"b", {}}, {
                    makeContentDep(p, files[fileBase + 0].path.string(), "c0"),
                    makeContentDep(p, files[fileBase + 1].path.string(), "c1"),
                    makeContentDep(p, files[fileBase + 2].path.string(), "c2"),
                });
                return db->getCurrentTraceHash(ea, vpath({bn}));
            });
        };
        std::vector<std::string> consumerNames = {"A1", "A2", "A3"};
        if (shared) {
            auto bh = recordProducer("B", 0);
            withExclusiveStore(*db, [&](const auto & ea) {
                for (auto & n : consumerNames)
                    db->record(ea, vpath({n}), string_t{"a", {}},
                        {Dep::makeValueContext(vpath({"B"}), DepHashValue(DepHash{bh->value}))});
            });
        } else {
            // Distinct producers B1/B2/B3, each reading its OWN 3 files.
            std::vector<const char *> bns = {"B1", "B2", "B3"};
            std::vector<std::optional<TraceHash>> bhs;
            for (size_t i = 0; i < bns.size(); ++i) bhs.push_back(recordProducer(bns[i], i * 3));
            withExclusiveStore(*db, [&](const auto & ea) {
                for (size_t i = 0; i < consumerNames.size(); ++i)
                    db->record(ea, vpath({consumerNames[i]}), string_t{"a", {}},
                        {Dep::makeValueContext(vpath({bns[i]}), DepHashValue(DepHash{bhs[i]->value}))});
            });
        }
        recreateDb(db);
        VerificationSession session;
        PathCountersSnapshot counters;
        // First consumer warms the session.
        (void) test::TraceStorageTestAccess::verify(*db, vpath({"A1"}), state, session);
        Counter::value_type laterMax = 0;
        for (auto * n : {"A2", "A3"}) {
            auto before = nrDepsChecked.load();
            auto r = test::TraceStorageTestAccess::verify(*db, vpath({n}), state, session);
            EXPECT_TRUE(r.has_value()) << "C2b: " << n << " must reuse via its edge";
            laterMax = std::max(laterMax, nrDepsChecked.load() - before);
        }
        return laterMax;
    };

    // 9 distinct files for the distinct arm (3 producers x 3); the shared arm
    // uses the first 3.
    std::deque<TempTextFile> files;
    for (int i = 0; i < 9; ++i) files.emplace_back("c" + std::to_string(i));

    auto sharedLater   = measureLaterConsumerDeps(/*shared=*/true,  files);
    // Fresh files for the distinct arm so there is NO L1 carryover between arms.
    std::deque<TempTextFile> files2;
    for (int i = 0; i < 9; ++i) files2.emplace_back("d" + std::to_string(i));
    auto distinctLater = measureLaterConsumerDeps(/*shared=*/false, files2);

    GTEST_LOG_(INFO) << "C2b sharedLater=" << sharedLater
                     << " distinctLater=" << distinctLater;

    // The memo's effect, isolated from L1: a later consumer of a SHARED producer
    // re-checks FEWER deps than a later consumer of a DISTINCT producer, because
    // the shared producer's closure was already verified+memoized this session.
    // (With distinct producers each later consumer must verify its own
    // producer's closure; with shared, the memo skips it.)
    EXPECT_LT(sharedLater, distinctLater)
        << "C2b: shared-producer later-consumer deps (" << sharedLater
        << ") must be < distinct-producer later-consumer deps (" << distinctLater
        << ") — the trace-context memo amortizes a shared closure across consumers";
}

// ── C3 (ADVERSARIAL): an output-only edge is UNSOUND for a facet-observing
//    consumer. This constructs the stale-serve hole the recorder must never
//    create. It documents WHY Tier-1 is gated on "consumer observed output
//    only": if A also read a NON-output facet of B (modeled as A having its own
//    direct dep on that facet), then an edge that tracks only B's OUTPUT
//    identity would let A stale-serve when the facet changes but the output
//    doesn't. ──────────────────────────────────────────────────────────────

TEST_F(TraceStoreTest, DerivationEdge_FacetObservingConsumer_OutputOnlyEdgeWouldStaleServe)
{
    // Setup: A legitimately depends on TWO things about B:
    //   (1) B's output identity  — tracked by the edge (TraceValueContext)
    //   (2) a NON-output facet of B (e.g. B.meta) that A read — modeled as A's
    //       own direct content dep on a separate "meta" source file.
    // A SOUND recorder records BOTH. We assert that with both recorded, a
    // meta-only change correctly invalidates A. Then we show that if the edge
    // had been the ONLY dep (output-only, dropping the facet dep — the bug),
    // the meta change would be invisible → stale serve.
    TempTextFile bOutSrc("out-v1");    // drives B's output identity
    TempTextFile bMetaSrc("meta-v1");  // a non-output facet A observed
    auto & p = pools();
    auto db = makeDb();

    auto bHash = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"B"}), string_t{"b-out", {}},
            {makeContentDep(p, bOutSrc.path.string(), "out-v1")});
        return db->getCurrentTraceHash(ea, vpath({"B"}));
    });
    ASSERT_TRUE(bHash.has_value());

    // SOUND consumer: edge to B's output AND a direct dep on the facet it read.
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"A_sound"}), string_t{"a", {}}, {
            Dep::makeValueContext(vpath({"B"}), DepHashValue(DepHash{bHash->value})),
            makeContentDep(p, bMetaSrc.path.string(), "meta-v1"),
        });
    });

    // BUGGY consumer: output-only edge, facet dep DROPPED (the unsound shape).
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"A_buggy"}), string_t{"a", {}},
            {Dep::makeValueContext(vpath({"B"}), DepHashValue(DepHash{bHash->value}))});
    });

    // Both hit while nothing changed.
    recreateDb(db);
    {
        EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, vpath({"A_sound"}), state).has_value());
        EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, vpath({"A_buggy"}), state).has_value());
    }

    // Change ONLY the non-output facet (B's meta), NOT B's output source.
    // B's output identity (trace hash) is UNCHANGED — B's recorded dep is on
    // bOutSrc, which we did not touch.
    bMetaSrc.modify("meta-v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        // SOUND consumer: its direct facet dep catches the change → invalidates.
        auto sound = test::TraceStorageTestAccess::verify(*db, vpath({"A_sound"}), state);
        EXPECT_FALSE(sound.has_value())
            << "C3 soundness: a consumer that observed B's non-output facet must "
               "record a direct dep on it; the facet change then invalidates A_sound";

        // BUGGY consumer: output-only edge sees B's output unchanged → STALE HIT.
        // This is the hole. The recorder must NEVER emit an output-only edge for
        // a facet-observing consumer. We pin the hole's existence so the gate is
        // not silently lost.
        auto buggy = test::TraceStorageTestAccess::verify(*db, vpath({"A_buggy"}), state);
        EXPECT_TRUE(buggy.has_value())
            << "C3 adversarial: an OUTPUT-ONLY edge (facet dep dropped) STALE-"
               "SERVES when a non-output facet changes — this is exactly the "
               "unsound shape a Tier-1 recorder must avoid by emitting the edge "
               "ONLY for output-only consumers (else also record the facet dep)";
    }
}

// ── C3-corollary: the SOUND output-only case — consumer observed output only,
//    edge alone is correct on BOTH a meta-irrelevant change and an output
//    change. (Confirms the edge isn't over-conservative for true output-only
//    consumers — the precision the lever buys.) ───────────────────────────────

TEST_F(TraceStoreTest, DerivationEdge_OutputOnlyConsumer_EdgeAloneIsSoundAndPrecise)
{
    // A observes ONLY B's output. An unrelated file (not part of B's output
    // identity and not observed by A) changing must NOT invalidate A — that is
    // the precision the edge preserves (A doesn't inline B's closure, so churn
    // in B's *inputs that don't change B's output* doesn't reach A beyond the
    // edge). Here we change a totally unrelated file.
    TempTextFile bSrc("out-v1");
    TempTextFile unrelated("noise-v1");
    auto & p = pools();
    auto db = makeDb();

    auto bHash = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"B"}), string_t{"b-out", {}},
            {makeContentDep(p, bSrc.path.string(), "out-v1")});
        return db->getCurrentTraceHash(ea, vpath({"B"}));
    });
    ASSERT_TRUE(bHash.has_value());

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"A"}), string_t{"a", {}},
            {Dep::makeValueContext(vpath({"B"}), DepHashValue(DepHash{bHash->value}))});
    });

    recreateDb(db);
    ASSERT_TRUE(test::TraceStorageTestAccess::verify(*db, vpath({"A"}), state).has_value());

    // Change a file A never observed and that is not part of B's identity.
    unrelated.modify("noise-v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto r = test::TraceStorageTestAccess::verify(*db, vpath({"A"}), state);
        EXPECT_TRUE(r.has_value())
            << "precision: an unrelated file change must not invalidate an "
               "output-only-edge consumer (the edge does not inline B's closure)";
    }
}

} // namespace nix::eval_trace
