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

// ── C2: producer output unchanged ⇒ consumer hits (the reuse win) ────────────

TEST_F(TraceStoreTest, DerivationEdge_ProducerUnchanged_ConsumerHitsWithoutClosureWalk)
{
    // The win: A reuses via the edge when B is unchanged, WITHOUT A re-recording
    // or re-walking B's input closure. A holds exactly ONE edge dep (to B), not
    // a copy of B's deps. We assert A hits across a session boundary with only
    // that single edge present — proving the edge alone authorizes reuse.
    TempTextFile bSrc("stable");
    auto & p = pools();
    auto db = makeDb();

    auto bHash = withExclusiveStore(*db, [&](const auto & ea) {
        // B's trace carries B's OWN closure (here one content dep; in reality
        // thousands). The point of the edge is that A does NOT copy these.
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
        << "C2: with B unchanged, A's single output-identity edge authorizes "
           "reuse without inlining/re-walking B's closure";
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(std::holds_alternative<string_t>(r->value));
    EXPECT_EQ(std::get<string_t>(r->value).first, "a-out")
        << "C2: A serves its own cached result via the edge";
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
