/**
 * Cross-trace keyset-escape soundness (OR-4 residual).
 *
 * These tests pin the HARDEST residual for the provenance-proof downgrade
 * (plans/keyset-provenance-differential-harness.md; the v6→v11 obligation):
 * a complete-keyset observation (`StructuredProjection #keys`) that is NOT
 * result-visible WITHIN the trace that observed it, but ESCAPES into a
 * sibling/parent trace via `TraceValueContext` / `TraceParentSlot`.
 *
 * The downgrade prunes a keyset dep when "the keyset never reaches a
 * result-visible sink." A single-trace finalization-reachability check
 * sees the parent's own result and could conclude the keyset is dead —
 * but a CONSUMING trace's correctness depends on the parent's key set
 * through the cross-trace context channel. This is the GHC-orphan-instance
 * analogue: the obligation discharges in one module but is observed in
 * another. Fail-closed = a cross-trace keyset escape MUST behave as a
 * result-visible sink (keep the keyset dep live).
 *
 * Why these are synthetic (raw `db->record()`), not real-eval:
 *   Real-eval cross-trace keyset escape is unreachable — the
 *   `ExprParseFile` FileBytes backstop (eval.cc:1422-1435) records a
 *   content dep on the source file first, so a key-changing source edit
 *   invalidates via FileBytes before the keyset channel is even exercised.
 *   To isolate the keyset channel we construct the parent trace with ONLY
 *   a `StructuredProjection #keys` dep (no FileBytes) and wire the consumer
 *   by hand. See project_keyset_provenance_harness memory: "cross-trace
 *   keyset-provenance cases go in synthetic TraceStoreFixture tests."
 *
 * Mechanism (verified against current code):
 *   - Parent's `#keys` dep value = `canonicalKeysHash(sortedKeys)`
 *     (resolveShapeSuffix → ShapeSuffix::Keys, dep-resolution-service.cc:247).
 *   - That hash folds into the parent's canonical trace hash
 *     (StructuredProjection contributesToTraceHash).
 *   - Consumer's `TraceValueContext`/`TraceParentSlot` dep stores the
 *     parent's trace hash; verify recomputes it via
 *     `resolveTraceContextHash` → recursive `verifyTrace(parent)` +
 *     `getCurrentTraceHash` (verifier.cc:243-278).
 *   - A key add/remove → parent `#keys` recompute mismatches → parent
 *     Invalid → context hash nullopt → consumer misses.
 *   - A value-only change keeping the key set → `#keys` unchanged →
 *     parent valid → consumer still hits (the precision the keyset
 *     abstraction buys, and the downgrade's cross-trace win).
 *
 * Companion: verify/integration.cc::Integration_ParentSlot_DoesNotCaptureKeySetRemoval
 * documents the COMPLEMENTARY gap — a parent recorded with NO keyset dep
 * (zero deps) has a keyset-insensitive trace hash, so the consumer
 * stale-serves on key removal. That is precisely the shape an
 * over-aggressive downgrade would produce if it pruned the parent's
 * `#keys` dep without accounting for cross-trace escape; the tests here
 * are the positive control proving the keyset dep, when present, closes it.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

namespace {

/// Record a parent trace whose ONLY dep is a complete-keyset
/// (`StructuredProjection #keys`) observation on `jsonPath`, with the
/// stored hash matching `keys` (so an unchanged file warm-verifies).
/// Returns the parent's trace hash for wiring a cross-trace consumer.
std::optional<TraceHash> recordKeysetParent(
    SqliteTraceStorage & db, InterningPools & p,
    const AttrPathId & parentPath,
    const std::string & jsonPath,
    std::vector<std::string> keys)
{
    return EvalTraceTest::withExclusiveStore(db, [&](const auto & ea) {
        db.record(ea, parentPath, string_t{"parent-shape", {}},
            {makeStructuredDepForTest(
                p, CanonicalQueryKind::StructuredProjection,
                DepSource::makeAbsolute(), jsonPath,
                StructuredFormat::Json, /*dataPath=*/{},
                DepHashValue(canonicalKeysHash(std::move(keys))),
                ShapeSuffix::Keys)});
        return db.getCurrentTraceHash(ea, parentPath);
    });
}

} // namespace

// ── Soundness: a key change to an ESCAPED keyset invalidates the consumer ──

TEST_F(TraceStoreTest, KeySetEscape_ViaValueContext_KeyAdded_InvalidatesConsumer)
{
    TempJsonFile f(R"({"a":1,"b":2})");
    auto & p = pools();
    auto db = makeDb();

    auto parentHash = recordKeysetParent(*db, p, vpath({"parent"}), f.path.string(), {"a", "b"});
    ASSERT_TRUE(parentHash.has_value());

    // Consumer escapes the parent's key set via TraceValueContext.
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"consumer"}), string_t{"cv", {}},
            {Dep::makeValueContext(vpath({"parent"}), DepHashValue(DepHash{parentHash->value}))});
    });

    // Precondition: unchanged key set → consumer warm-hits.
    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"consumer"}), state);
        ASSERT_TRUE(result.has_value())
            << "precondition: unchanged key set must warm-hit the consumer";
    }

    // Add a key WITHOUT touching existing values. The parent's #keys dep
    // recomputes canonicalKeysHash({a,b,c}) ≠ stored → parent invalid →
    // escaped context hash unresolvable → consumer must miss.
    f.modify(R"({"a":1,"b":2,"c":3})");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"consumer"}), state);
        EXPECT_FALSE(result.has_value())
            << "cross-trace keyset escape: key ADD to the escaped set must "
               "invalidate the consuming trace (fail-closed sink)";
    }
}

TEST_F(TraceStoreTest, KeySetEscape_ViaValueContext_KeyRemoved_InvalidatesConsumer)
{
    // Key REMOVAL is the harder direction — it is exactly what bare
    // TraceParentSlot misses (Integration_ParentSlot_DoesNotCaptureKeySetRemoval).
    // With the parent's #keys dep present, removal is caught.
    TempJsonFile f(R"({"a":1,"b":2,"c":3})");
    auto & p = pools();
    auto db = makeDb();

    auto parentHash = recordKeysetParent(*db, p, vpath({"parent"}), f.path.string(), {"a", "b", "c"});
    ASSERT_TRUE(parentHash.has_value());

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"consumer"}), string_t{"cv", {}},
            {Dep::makeValueContext(vpath({"parent"}), DepHashValue(DepHash{parentHash->value}))});
    });

    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"consumer"}), state);
        ASSERT_TRUE(result.has_value())
            << "precondition: unchanged key set must warm-hit the consumer";
    }

    f.modify(R"({"a":1,"b":2})");  // remove "c"
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"consumer"}), state);
        EXPECT_FALSE(result.has_value())
            << "cross-trace keyset escape: key REMOVAL from the escaped set "
               "must invalidate the consuming trace";
    }
}

TEST_F(TraceStoreTest, KeySetEscape_ViaParentSlot_KeyAdded_InvalidatesConsumer)
{
    // Same soundness property through the TraceParentSlot channel: both
    // CQK types route through resolveTraceContextHash.
    TempJsonFile f(R"({"a":1,"b":2})");
    auto & p = pools();
    auto db = makeDb();

    auto parentHash = recordKeysetParent(*db, p, vpath({"parent"}), f.path.string(), {"a", "b"});
    ASSERT_TRUE(parentHash.has_value());

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"consumer"}), string_t{"cv", {}},
            {Dep::makeParentSlot(ParentSlot{vpath({"parent"})},
                                 DepHashValue(DepHash{parentHash->value}))});
    });

    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"consumer"}), state);
        ASSERT_TRUE(result.has_value())
            << "precondition: unchanged key set must warm-hit the consumer";
    }

    f.modify(R"({"a":1,"b":2,"c":3})");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"consumer"}), state);
        EXPECT_FALSE(result.has_value())
            << "cross-trace keyset escape via ParentSlot: key add must "
               "invalidate the consuming trace";
    }
}

// ── Precision: a value-only change to an escaped keyset still hits ─────────

TEST_F(TraceStoreTest, KeySetEscape_ViaValueContext_ValueChangeKeepingKeys_StillHits)
{
    // The downgrade's cross-trace PRECISION target: a consumer that escapes
    // ONLY the parent's key set (not its values) must stay valid when the
    // file's values change but its key set does not. canonicalKeysHash is a
    // function of keys alone, so the parent's #keys dep — and thus its trace
    // hash — is unchanged, and the consumer warm-hits.
    TempJsonFile f(R"({"a":1,"b":2})");
    auto & p = pools();
    auto db = makeDb();

    auto parentHash = recordKeysetParent(*db, p, vpath({"parent"}), f.path.string(), {"a", "b"});
    ASSERT_TRUE(parentHash.has_value());

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"consumer"}), string_t{"cv", {}},
            {Dep::makeValueContext(vpath({"parent"}), DepHashValue(DepHash{parentHash->value}))});
    });

    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"consumer"}), state);
        ASSERT_TRUE(result.has_value())
            << "precondition: unchanged file must warm-hit the consumer";
    }

    // Change values, keep the key set {a,b}.
    f.modify(R"({"a":99,"b":88})");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"consumer"}), state);
        EXPECT_TRUE(result.has_value())
            << "precision: a value-only change preserving the key set must "
               "NOT invalidate a consumer that escaped only the key set";
    }
}

// ── Multi-hop: keyset escape propagates through a context chain ────────────

TEST_F(TraceStoreTest, KeySetEscape_DepthTwo_KeyChangePropagatesThroughChain)
{
    // A(#keys on file) ← B(ctx A) ← C(ctx B). A key change at the leaf
    // keyset must propagate all the way to C — the keyset escape is
    // transitive across the context chain, not just one hop.
    TempJsonFile f(R"({"a":1,"b":2})");
    auto & p = pools();
    auto db = makeDb();

    auto hashA = recordKeysetParent(*db, p, vpath({"A"}), f.path.string(), {"a", "b"});
    ASSERT_TRUE(hashA.has_value());

    auto hashB = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"B"}), string_t{"bv", {}},
            {Dep::makeValueContext(vpath({"A"}), DepHashValue(DepHash{hashA->value}))});
        return db->getCurrentTraceHash(ea, vpath({"B"}));
    });
    ASSERT_TRUE(hashB.has_value());

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"C"}), string_t{"cv", {}},
            {Dep::makeValueContext(vpath({"B"}), DepHashValue(DepHash{hashB->value}))});
    });

    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"C"}), state);
        ASSERT_TRUE(result.has_value())
            << "precondition: unchanged key set must warm-hit C through the chain";
    }

    f.modify(R"({"a":1,"b":2,"c":3})");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);
    {
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"C"}), state);
        EXPECT_FALSE(result.has_value())
            << "transitive keyset escape: a leaf key change must propagate "
               "A → B → C and invalidate the deepest consumer";
    }
}

} // namespace nix::eval_trace
