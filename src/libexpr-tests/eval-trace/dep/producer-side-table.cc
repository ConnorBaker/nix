/**
 * Producer side-table for the content-addressed-trace-identity RFC §3b.
 *
 * `store/ca-producer-boundary-recording.cc` proved the recording-side
 * COMPOSITION (P1-P6) using a hand-rolled `unordered_map` to model the
 * routing gate. Production §3b needs that map to live alongside
 * `MemoReplayStore::epochMap` so `replayMemoizedDeps` can consult it at
 * force time.
 *
 * This test file pins the SMALLEST production surface that makes the gate
 * possible:
 *
 *   MemoReplayStore::registerProducer(value, caKey, traceHash)
 *   MemoReplayStore::lookupProducer(value)
 *   MemoReplayStore::producerMapSize() / clearProducerMap()
 *
 * The map is keyed by `const Value *` (same key as `epochMap`) and stored
 * via `traceable_allocator` (same lifetime concerns as `epochMap` —
 * keyed Value*s must stay live across GC). It is INDEPENDENT of `epochMap`:
 * a Value can be in the producer map without being in the epoch map (e.g.,
 * a producer whose trace finalization installed an edge but whose deps
 * were already consumed via an outer scope's takeDeps).
 *
 * The five tests below are the unit-level floor. None touch the live
 * evaluator path; they exercise only the `MemoReplayStore` API in isolation,
 * mirroring `dep/epoch-bugs.cc::Epoch_InsertOrAssign_OverwritesStaleEntry`.
 *
 * T1 register/lookup round-trip (the basic gate)
 * T2 distinct producer Values map to distinct CA keys (no collision)
 * T3 re-registration of the SAME Value* overwrites the prior entry
 *    (BUG-7 hazard for the producer map: GC address reuse must not retain
 *    a stale producer→caKey binding)
 * T4 lookupProducer returns nullopt for an unregistered Value
 * T5 clearProducerMap empties the map (lifecycle parity with epochMap)
 */
#include "eval-trace/helpers.hh"
#include "eval-trace/trace-runtime-test-access.hh"

#include <gtest/gtest.h>

#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/memo-replay-store.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/value.hh"

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// Build a synthetic CA routing key — same construction as
// `store/ca-trace-key-routing.cc` and `store/ca-producer-boundary-
// recording.cc`. Reused here so the unit-level floor stays consistent with
// the store-level floor.
static AttrPathId caKey(AttrVocabStore & vocab, std::string_view drvHash)
{
    return vocab.internPath(
        AttrVocabStore::rootPath(),
        vocab.internName(std::string("__ca:") + std::string(drvHash)));
}

class ProducerSideTableTest : public LibExprTest
{
protected:
    InterningPools pools;
    TraceRuntime ctx;

    void SetUp() override
    {
        TraceRuntimeTestAccess::clearProducerMap(ctx);
    }

    void TearDown() override
    {
        TraceRuntimeTestAccess::clearProducerMap(ctx);
    }
};

// ── T1: register / lookup round-trip ────────────────────────────────────────

TEST_F(ProducerSideTableTest, ProducerMap_RegisterAndLookup_RoundTrips)
{
    Value v;
    v.mkInt(42);

    auto key = caKey(state.vocabStore(), "drv-T1");
    DepHash hash{depHash("trace-hash-bytes")};

    EXPECT_FALSE(TraceRuntimeTestAccess::lookupProducer(ctx, v).has_value())
        << "T1 precondition: a fresh Value has no producer entry";

    TraceRuntimeTestAccess::registerProducer(ctx, v, key, hash);

    auto entry = TraceRuntimeTestAccess::lookupProducer(ctx, v);
    ASSERT_TRUE(entry.has_value())
        << "T1: registerProducer + lookupProducer must round-trip";
    EXPECT_EQ(entry->caKey.value, key.value);
    EXPECT_EQ(entry->traceHash.value, hash.value);
}

// ── T2: distinct Values → distinct entries (no key collision) ───────────────

TEST_F(ProducerSideTableTest, ProducerMap_DistinctValues_DistinctEntries)
{
    Value vA, vB;
    vA.mkInt(1); vB.mkInt(2);

    auto keyA = caKey(state.vocabStore(), "drv-A");
    auto keyB = caKey(state.vocabStore(), "drv-B");
    DepHash hashA{depHash("hash-A")};
    DepHash hashB{depHash("hash-B")};

    TraceRuntimeTestAccess::registerProducer(ctx, vA, keyA, hashA);
    TraceRuntimeTestAccess::registerProducer(ctx, vB, keyB, hashB);

    auto entryA = TraceRuntimeTestAccess::lookupProducer(ctx, vA);
    auto entryB = TraceRuntimeTestAccess::lookupProducer(ctx, vB);
    ASSERT_TRUE(entryA.has_value());
    ASSERT_TRUE(entryB.has_value());
    EXPECT_EQ(entryA->caKey.value, keyA.value);
    EXPECT_EQ(entryB->caKey.value, keyB.value);
    EXPECT_NE(entryA->caKey.value, entryB->caKey.value)
        << "T2: distinct Values must keep their distinct CA keys";
    EXPECT_NE(entryA->traceHash.value, entryB->traceHash.value);
}

// ── T3: re-registration overwrites (GC address reuse, BUG-7-shaped hazard) ──

TEST_F(ProducerSideTableTest, ProducerMap_ReRegister_OverwritesStaleEntry)
{
    // A Value* that GC reclaims and re-uses for a different trace must NOT
    // retain its prior CA-key binding. Mirrors the BUG-7 hazard the epoch
    // map handles via `insert_or_assign`.
    Value v;
    v.mkInt(0);

    auto key1 = caKey(state.vocabStore(), "drv-old");
    auto key2 = caKey(state.vocabStore(), "drv-new");
    DepHash hash1{depHash("hash-old")};
    DepHash hash2{depHash("hash-new")};

    TraceRuntimeTestAccess::registerProducer(ctx, v, key1, hash1);
    TraceRuntimeTestAccess::registerProducer(ctx, v, key2, hash2);

    auto entry = TraceRuntimeTestAccess::lookupProducer(ctx, v);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->caKey.value, key2.value)
        << "T3: re-registration must overwrite the stale binding (GC address "
           "reuse hazard, same shape as BUG-7 on epochMap)";
    EXPECT_EQ(entry->traceHash.value, hash2.value);
}

// ── T4: lookup of unregistered value returns nullopt ────────────────────────

TEST_F(ProducerSideTableTest, ProducerMap_LookupUnregistered_Nullopt)
{
    Value vRegistered, vUnregistered;
    vRegistered.mkInt(1); vUnregistered.mkInt(2);

    auto key = caKey(state.vocabStore(), "drv-only");
    TraceRuntimeTestAccess::registerProducer(
        ctx, vRegistered, key, DepHash{depHash("h")});

    EXPECT_FALSE(TraceRuntimeTestAccess::lookupProducer(ctx, vUnregistered).has_value())
        << "T4: an unregistered Value must lookup as nullopt — the gate "
           "in replayMemoizedDeps will fall through to flatten-replay";
}

// ── T5: clearProducerMap empties the table (lifecycle parity with epochMap) ─

TEST_F(ProducerSideTableTest, ProducerMap_Clear_Empties)
{
    Value v1, v2;
    v1.mkInt(1); v2.mkInt(2);
    TraceRuntimeTestAccess::registerProducer(
        ctx, v1, caKey(state.vocabStore(), "a"), DepHash{depHash("ha")});
    TraceRuntimeTestAccess::registerProducer(
        ctx, v2, caKey(state.vocabStore(), "b"), DepHash{depHash("hb")});

    EXPECT_EQ(TraceRuntimeTestAccess::producerMapSize(ctx), 2u);
    TraceRuntimeTestAccess::clearProducerMap(ctx);
    EXPECT_EQ(TraceRuntimeTestAccess::producerMapSize(ctx), 0u);
    EXPECT_FALSE(TraceRuntimeTestAccess::lookupProducer(ctx, v1).has_value());
    EXPECT_FALSE(TraceRuntimeTestAccess::lookupProducer(ctx, v2).has_value());
}

} // namespace nix::eval_trace
