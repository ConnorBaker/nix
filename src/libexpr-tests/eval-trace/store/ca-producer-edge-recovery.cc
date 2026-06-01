/**
 * Recovery-aware producer-edge resolution (RFC §11 fix, verifier.cc
 * resolveTraceContextHash history-bootstrap branch).
 *
 * THE BUG (RFC §10): the aggressive edge-recorder makes a consumer store a single
 * `TraceValueContext` edge to a `__ca:<drvHash>` producer trace instead of the
 * flattened closure. When a flake-source edit rotates the SESSION KEY, the
 * consumer recovers via History bootstrap (its stableRecoveryKey is
 * source-identity-stable), but the producer's CurrentNode lives under the OLD
 * session key — so `resolveTraceContextHash`'s `lookupCurrentNode(producer)`
 * missed and returned nullopt, leaving the edge unresolvable and the consumer
 * over-invalidating (fails CLOSED — never stale, just a needless miss).
 *
 * THE FIX: when `lookupCurrentNode(parent)` misses, fall back to the parent's
 * History row under the current stableRecoveryKey (`lookupLatestHistoryForAttr`),
 * verify it, and re-publish its CurrentNode — mirroring `verify()`'s own
 * history-bootstrap.
 *
 * These tests reproduce session-key rotation directly via
 * `setSessionConfig(SessionConfig::forTest(<distinct policy>, <shared stable key>))`
 * — the exact pattern recovery.cc uses for cross-session bootstrap. Synthetic
 * `db->record` (mirrors ca-trace-key-routing.cc), so they pin the verifier
 * behaviour independent of the primop recorder.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"

#include <filesystem>

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

static AttrPathId caKey(AttrVocabStore & vocab, std::string_view drvHash)
{
    return vocab.internPath(
        AttrVocabStore::rootPath(),
        vocab.internName(std::string("__ca:") + std::string(drvHash)));
}

// THE FIX, positive case: a consumer's CA-producer edge resolves across a
// session-key rotation via History bootstrap of the producer. Without the fix
// (resolveTraceContextHash bails on lookupCurrentNode(producer)==null), the
// consumer would MISS even though nothing relevant changed.
TEST_F(TraceStoreTest, CAProducerEdge_SessionKeyRotation_ResolvesViaHistoryBootstrap)
{
    TempTextFile prodSrc("producer-input-v1");
    auto srcPath = std::filesystem::canonical(prodSrc.path).string();
    auto producer = caKey(testVocab(), "rot-deadbeef");
    EvalTraceHash producerHash{};

    // Session 1 (session key from policy "rev-1"; shared stable key "rot-stable"):
    // record the producer trace + a consumer that edges to it.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("rev-1").value, "rot-stable"));
        auto fileHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(srcPath)).readFile());
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, producer, string_t{"producer-out", {}},
                {makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
                    DepSource::makeAbsolute(), srcPath, fileHash)});
            auto ph = db->getCurrentTraceHash(ea, producer);
            ASSERT_TRUE(ph.has_value());
            producerHash = ph->value;
            db->record(ea, vpath({"consumer"}), string_t{"consumer-out", {}},
                {Dep::makeValueContext(producer, DepHashValue(DepHash{producerHash}))});
        });
    }

    // Session 2 (DIFFERENT session key from policy "rev-2"; SAME stable key):
    // the producer's CurrentNode is absent under this session key. The consumer
    // bootstraps from History; its edge must resolve the producer via History too.
    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("rev-2").value, "rot-stable"));
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"consumer"}), state);
        ASSERT_TRUE(result.has_value())
            << "consumer's CA-producer edge must resolve via History bootstrap of "
               "the producer after a session-key rotation (RFC §11 fix)";
        auto * s = std::get_if<string_t>(&result->value);
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(s->first, "consumer-out");
    }
}

// SOUNDNESS: the History-bootstrapped producer is still VERIFIED against the live
// FS. If the producer's input changed across the rotation, the edge must STILL
// invalidate — the bootstrap must not blindly serve the old producer.
TEST_F(TraceStoreTest, CAProducerEdge_SessionKeyRotation_ProducerInputChanged_Invalidates)
{
    TempTextFile prodSrc("producer-input-v1");
    auto srcPath = std::filesystem::canonical(prodSrc.path).string();
    auto producer = caKey(testVocab(), "rot-f00dface");
    EvalTraceHash producerHash{};

    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("rev-1").value, "chg-stable"));
        auto fileHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(srcPath)).readFile());
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, producer, string_t{"producer-out", {}},
                {makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
                    DepSource::makeAbsolute(), srcPath, fileHash)});
            auto ph = db->getCurrentTraceHash(ea, producer);
            ASSERT_TRUE(ph.has_value());
            producerHash = ph->value;
            db->record(ea, vpath({"consumer"}), string_t{"consumer-out", {}},
                {Dep::makeValueContext(producer, DepHashValue(DepHash{producerHash}))});
        });
    }

    // Mutate the producer's input BEFORE the rotated session verifies.
    prodSrc.modify("producer-input-v2");
    getFSSourceAccessor()->invalidateCache();

    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("rev-2").value, "chg-stable"));
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({"consumer"}), state);
        EXPECT_FALSE(result.has_value())
            << "producer input changed ⇒ the History-bootstrapped producer must FAIL "
               "verify ⇒ the consumer's edge must invalidate (bootstrap is verified, "
               "not blindly served)";
    }
}

// ADVERSARIAL (RFC §12 adversarial pass): TWO consumers of the SAME producer
// across a session-key rotation. The first resolution bootstraps + re-publishes
// the producer's CurrentNode; the second must also resolve (via the now-published
// CurrentNode + the traceContextMemo fast path). Guards the memo/re-publish
// interaction — a bug there (e.g. memoizing the stamp-0 bootstrap row such that
// the second consumer mis-resolves) would surface here.
TEST_F(TraceStoreTest, CAProducerEdge_SessionKeyRotation_TwoConsumers_BothResolve)
{
    TempTextFile prodSrc("producer-input-v1");
    auto srcPath = std::filesystem::canonical(prodSrc.path).string();
    auto producer = caKey(testVocab(), "rot-twocons");
    EvalTraceHash producerHash{};

    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("rev-1").value, "two-stable"));
        auto fileHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(srcPath)).readFile());
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, producer, string_t{"producer-out", {}},
                {makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
                    DepSource::makeAbsolute(), srcPath, fileHash)});
            auto ph = db->getCurrentTraceHash(ea, producer);
            ASSERT_TRUE(ph.has_value());
            producerHash = ph->value;
            db->record(ea, vpath({"consumerX"}), string_t{"x-out", {}},
                {Dep::makeValueContext(producer, DepHashValue(DepHash{producerHash}))});
            db->record(ea, vpath({"consumerY"}), string_t{"y-out", {}},
                {Dep::makeValueContext(producer, DepHashValue(DepHash{producerHash}))});
        });
    }

    {
        auto db = makeDb();
        db->setSessionConfig(SessionConfig::forTest(depHash("rev-2").value, "two-stable"));
        // Both verified in ONE session (shared VerificationSession) so the second
        // exercises the post-bootstrap memo/CurrentNode fast path.
        VerificationSession session;
        auto rx = test::TraceStorageTestAccess::verify(*db, vpath({"consumerX"}), state, session);
        auto ry = test::TraceStorageTestAccess::verify(*db, vpath({"consumerY"}), state, session);
        ASSERT_TRUE(rx.has_value()) << "first consumer bootstraps the producer and resolves";
        ASSERT_TRUE(ry.has_value())
            << "second consumer must ALSO resolve (via re-published CurrentNode / memo) — "
               "not mis-resolve off the stamp-0 bootstrap row";
        EXPECT_EQ(std::get<string_t>(rx->value).first, "x-out");
        EXPECT_EQ(std::get<string_t>(ry->value).first, "y-out");
    }
}

} // namespace nix::eval_trace
