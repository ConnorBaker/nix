/**
 * MEASUREMENT (RFC §8 step 3 / redesign-plan follow-up #20): the verify-time
 * constant M = (cost of verifying ONE memo-hit TraceValueContext edge) / (cost of
 * verifying ONE flat content dep). #20 showed the verify-benefit gate flips
 * win↔loss across M ∈ {1,2,3}; this measures M directly on the real store/verify
 * pipeline, WITHOUT touching the hot eval path (the cheapest faithful way — vs the
 * full edge-emitting recorder).
 *
 * Design: a consumer with K flat content-deps vs a consumer with K edges to K
 * distinct CA producers (each producer holding one content dep). Warm the memo by
 * verifying the producers first in a shared session, then time verifying the
 * edge-consumer (memo-HIT path — the N>1-sharing common case) vs the flat-consumer,
 * over many iterations. M = edge-verify-time / flat-verify-time, per dep.
 *
 * This is the memo-HIT cost (the steady state once a producer is verified once per
 * session). The memo-MISS first-verify cost (recursive verifyTrace of the producer)
 * is the producer's OWN-dep walk + recursion setup — measured separately below.
 *
 * Synthetic (raw db->record), mirroring ca-trace-key-routing.cc. Gated behind an
 * env var so it is a benchmark, not a CI assertion (timing is machine-dependent);
 * a non-timed correctness check (both consumers verify) runs unconditionally so the
 * harness itself is validated.
 */
#include "eval-trace/helpers.hh"
#include "nix/util/environment-variables.hh"

#include <chrono>
#include <cstdio>

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

namespace {

AttrPathId caKeyN(AttrVocabStore & vocab, size_t i)
{
    return vocab.internPath(
        AttrVocabStore::rootPath(),
        vocab.internName("__ca:prod" + std::to_string(i)));
}

} // namespace

// Validated harness + (when NIX_BENCH_EDGE_VERIFY=1) the M measurement.
TEST_F(TraceStoreTest, CAEdgeVerifyCost_MeasuresM)
{
    auto & p = pools();
    // K is env-overridable so we can run two points (e.g. 50, 800) and decompose
    // per-verify time = F (fixed) + K·c (per-item), separately for flat vs edge —
    // confirming M compares the PER-ITEM slope c, not fixed overhead F.
    size_t K = 200;
    if (auto kv = getEnv("NIX_BENCH_EDGE_VERIFY_K"); kv && !kv->empty())
        K = std::stoul(*kv);

    // K distinct source files (shared content so flat & edge sides observe the same work).
    std::vector<std::unique_ptr<TempTextFile>> srcs;
    for (size_t i = 0; i < K; ++i)
        srcs.push_back(std::make_unique<TempTextFile>("input-" + std::to_string(i)));

    auto db = makeDb();

    // Producer traces: K distinct CA producers, each with ONE content dep.
    std::vector<AttrPathId> producers;
    std::vector<DepHashValue> producerHashes;
    for (size_t i = 0; i < K; ++i)
        producers.push_back(caKeyN(testVocab(), i));

    auto flatConsumer = vpath({"consumers", "flat"});
    auto edgeConsumer = vpath({"consumers", "edge"});

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record each producer + capture its trace hash for the edges.
        for (size_t i = 0; i < K; ++i) {
            db->record(ea, producers[i], string_t{"prod", {}},
                {makeContentDep(p, srcs[i]->path.string(), "input-" + std::to_string(i))});
            auto h = db->getCurrentTraceHash(ea, producers[i]);
            producerHashes.push_back(DepHashValue(DepHash{h->value}));
        }
        // Flat consumer: K content deps (the SAME K files — what a consumer flattens today).
        std::vector<Dep> flatDeps;
        for (size_t i = 0; i < K; ++i)
            flatDeps.push_back(makeContentDep(p, srcs[i]->path.string(), "input-" + std::to_string(i)));
        db->record(ea, flatConsumer, string_t{"flat", {}}, std::move(flatDeps));
        // Edge consumer: K edges to the K producers (the edge model).
        std::vector<Dep> edgeDeps;
        for (size_t i = 0; i < K; ++i)
            edgeDeps.push_back(Dep::makeValueContext(producers[i], producerHashes[i]));
        db->record(ea, edgeConsumer, string_t{"edge", {}}, std::move(edgeDeps));
    });

    recreateDb(db);

    // Correctness (always): both consumers verify (validates the harness — the M
    // number is meaningless if either side doesn't actually verify K things).
    {
        auto rf = test::TraceStorageTestAccess::verify(*db, flatConsumer, state);
        auto re = test::TraceStorageTestAccess::verify(*db, edgeConsumer, state);
        ASSERT_TRUE(rf.has_value()) << "flat consumer must verify";
        ASSERT_TRUE(re.has_value()) << "edge consumer must verify";
        EXPECT_EQ(std::get<string_t>(rf->value).first, "flat");
        EXPECT_EQ(std::get<string_t>(re->value).first, "edge");
    }

    if (getEnv("NIX_BENCH_EDGE_VERIFY").value_or("") != "1") {
        GTEST_SKIP() << "set NIX_BENCH_EDGE_VERIFY=1 to run the timing (machine-dependent)";
        return;
    }

    // CONFOUND FIX (adversarial pass): the FAIR comparison is STEADY STATE for BOTH
    // sides — flat with its L1 dep-hash cache warm (the real hot path is L1-warm:
    // 21.3M hits / 61K misses in the doc), edge with its verifiedTraceIds memo warm.
    // A fresh session per iter denied L1 warmth to the flat side (re-reading+re-hashing
    // each file every iter ~18µs/dep, ~140× the doc's ~131ns warm-walk), which
    // flattered the edge. So: ONE session per side, warm it once (untimed), then time
    // repeated re-verifies of the SAME consumer in that warm session. Each re-verify
    // walks the consumer's K items hitting its steady-state cache (L1 for flat, memo
    // for edge) — the apples-to-apples steady-state cost.
    const int warmups = 5;
    const int iters = 5000;
    auto timeSteadyState = [&](AttrPathId who) -> double {
        VerificationSession session; // ONE session, persists L1 + memo across iters
        for (int w = 0; w < warmups; ++w)
            (void) test::TraceStorageTestAccess::verify(*db, who, state, session);
        auto t0 = std::chrono::steady_clock::now();
        for (int it = 0; it < iters; ++it)
            (void) test::TraceStorageTestAccess::verify(*db, who, state, session);
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
    };

    double flatNs = timeSteadyState(flatConsumer); // L1-warm flat dep walk
    double edgeNs = timeSteadyState(edgeConsumer); // memo-warm edge walk
    double mPerItem = (edgeNs / K) / (flatNs / K);

    std::fprintf(stderr,
        "\n=== M MEASUREMENT (steady state, K=%zu, %d warmups + %d iters) ===\n"
        "flat consumer verify (L1-warm):   %8.0f ns  (%6.1f ns/dep)\n"
        "edge consumer verify (memo-warm): %8.0f ns  (%6.1f ns/edge)\n"
        "M (edge / flat, per item, BOTH steady-state): %.2f\n"
        "=> #20 gate: M<~2 => edge model is a verify WIN; M>~3 => verify LOSS.\n"
        "   (sanity: L1-warm flat ns/dep should be ~hundreds of ns, near the doc's ~131ns —\n"
        "    if it is ~18000 ns the session is NOT warming L1 and the number is invalid.)\n",
        K, warmups, iters,
        flatNs, flatNs / K,
        edgeNs, edgeNs / K,
        mPerItem);
    EXPECT_GT(flatNs, 0.0);
    EXPECT_GT(edgeNs, 0.0);
}

} // namespace nix::eval_trace
