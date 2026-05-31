/**
 * Feasibility proof for the content-addressed-trace-identity RFC §3a (routing key).
 *
 * The RFC proposes keying a derivation's producer trace by a CONTENT ADDRESS
 * (drvPath), not by an attr-path position — so a derivation reached from N
 * different positions records ONCE and is referenced by N edges. The concrete
 * routing mechanism (Design A): intern a synthetic name `"__ca:<drvHash>"` as a
 * single-component path off root (`internName` accepts any string_view, per
 * attr-vocab-store.hh:36), giving the producer a stable, position-independent
 * AttrPathId that any consumer scope can compute identically from the drvPath.
 *
 * These tests prove the EXISTING store/vocab/verify pipeline supports that
 * without modification:
 *   (R1) a producer trace recorded under a synthetic CA key round-trips and
 *        verifies (the CA key is a first-class trace identity);
 *   (R2) TWO consumers at DIFFERENT attr-path positions both edge to the SAME CA
 *        producer trace and both hit — the cross-scope sharing C2b could not show
 *        (C2b used a literal vpath({"B"}) reachable as an ordinary attr path);
 *   (R3) mutating the CA producer's input invalidates BOTH cross-scope consumers
 *        through the single shared producer trace (soundness of the shared edge).
 *
 * Synthetic (raw db->record), mirroring keyset-escape.cc / derivation-edge-
 * soundness.cc — this pins the ROUTING contract a recorder change would target.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// Build a synthetic content-addressed trace key: a single-component path off
// root whose name is "__ca:<drvHash>". Position-independent by construction.
static AttrPathId caKey(AttrVocabStore & vocab, std::string_view drvHash)
{
    return vocab.internPath(
        AttrVocabStore::rootPath(),
        vocab.internName(std::string("__ca:") + std::string(drvHash)));
}

// ── R1: a CA-keyed producer trace is a first-class trace identity ────────────

TEST_F(TraceStoreTest, CATraceKey_ProducerRoundTripsAndVerifies)
{
    TempTextFile bSrc("input-v1");
    auto & p = pools();
    auto db = makeDb();

    auto key = caKey(testVocab(), "deadbeef");

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, key, string_t{"producer-out", {}},
            {makeContentDep(p, bSrc.path.string(), "input-v1")});
    });

    recreateDb(db);
    auto r = test::TraceStorageTestAccess::verify(*db, key, state);
    ASSERT_TRUE(r.has_value())
        << "R1: a producer trace recorded under a synthetic CA key must verify "
           "like any attr-path trace";
    ASSERT_TRUE(std::holds_alternative<string_t>(r->value));
    EXPECT_EQ(std::get<string_t>(r->value).first, "producer-out");
}

// ── R2: two consumers at DIFFERENT positions share ONE CA producer ───────────

TEST_F(TraceStoreTest, CATraceKey_TwoCrossScopeConsumers_BothHitViaSharedProducer)
{
    TempTextFile bSrc("input-stable");
    auto & p = pools();
    auto db = makeDb();

    auto producer = caKey(testVocab(), "cafe1234");

    auto pHash = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, producer, string_t{"shared-drv-out", {}},
            {makeContentDep(p, bSrc.path.string(), "input-stable")});
        return db->getCurrentTraceHash(ea, producer);
    });
    ASSERT_TRUE(pHash.has_value());

    // Two consumers at genuinely DIFFERENT attr-path positions (distinct parents),
    // each edging to the SAME CA producer. This is the cross-scope sharing the RFC
    // needs and C2b could not demonstrate.
    auto consumerX = vpath({"pkgsX", "appA"});
    auto consumerY = vpath({"pkgsY", "appB"});

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, consumerX, string_t{"x-uses-shared", {}},
            {Dep::makeValueContext(producer, DepHashValue(DepHash{pHash->value}))});
        db->record(ea, consumerY, string_t{"y-uses-shared", {}},
            {Dep::makeValueContext(producer, DepHashValue(DepHash{pHash->value}))});
    });

    recreateDb(db);
    {
        auto rx = test::TraceStorageTestAccess::verify(*db, consumerX, state);
        auto ry = test::TraceStorageTestAccess::verify(*db, consumerY, state);
        ASSERT_TRUE(rx.has_value()) << "R2: cross-scope consumer X must hit via the shared CA edge";
        ASSERT_TRUE(ry.has_value()) << "R2: cross-scope consumer Y must hit via the shared CA edge";
        EXPECT_EQ(std::get<string_t>(rx->value).first, "x-uses-shared");
        EXPECT_EQ(std::get<string_t>(ry->value).first, "y-uses-shared");
    }
}

// ── R3: mutating the shared CA producer's input invalidates BOTH consumers ───

TEST_F(TraceStoreTest, CATraceKey_ProducerInputChange_InvalidatesBothConsumers)
{
    TempTextFile bSrc("input-v1");
    auto & p = pools();
    auto db = makeDb();

    auto producer = caKey(testVocab(), "f00dface");

    auto pHash = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, producer, string_t{"shared-out-v1", {}},
            {makeContentDep(p, bSrc.path.string(), "input-v1")});
        return db->getCurrentTraceHash(ea, producer);
    });
    ASSERT_TRUE(pHash.has_value());

    auto consumerX = vpath({"pkgsX", "appA"});
    auto consumerY = vpath({"pkgsY", "appB"});
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, consumerX, string_t{"x", {}},
            {Dep::makeValueContext(producer, DepHashValue(DepHash{pHash->value}))});
        db->record(ea, consumerY, string_t{"y", {}},
            {Dep::makeValueContext(producer, DepHashValue(DepHash{pHash->value}))});
    });

    // Precondition: both hit unchanged.
    recreateDb(db);
    {
        EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, consumerX, state).has_value());
        EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, consumerY, state).has_value());
    }

    // Mutate the shared producer's input ⇒ its trace hash changes ⇒ BOTH
    // consumers' edges (which stored the OLD producer hash) must mismatch.
    bSrc.modify("input-v2");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto rx = test::TraceStorageTestAccess::verify(*db, consumerX, state);
        auto ry = test::TraceStorageTestAccess::verify(*db, consumerY, state);
        EXPECT_FALSE(rx.has_value())
            << "R3: shared producer's input changed ⇒ consumer X's edge must invalidate";
        EXPECT_FALSE(ry.has_value())
            << "R3: shared producer's input changed ⇒ consumer Y's edge must invalidate";
    }
}

} // namespace nix::eval_trace
