#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// EvalOrderCommutativity — Commutativity of cache evaluation order
//
// Formal:
//   ∀ A, B independent expressions:
//     cache(eval(A); eval(B))  ≡  cache(eval(B); eval(A))
//
// Both A and B are evaluated in two orderings.  After both orderings have been
// recorded, a final warm pass must be a cache hit for both expressions.
//
// This catches hidden ordering dependencies in the trace store: session key
// leakage, epoch log contamination, or attr-path collisions across unrelated
// expressions.
//
// Implementation note: we simulate the two orderings within one shared
// TraceCacheFixture session (same SQLite DB) by recording A then B, then B
// then A (both cold evals write traces), then confirming warm hits for both.
class EvalTraceProperty_EvalOrderCommutativity : public TraceCacheFixture {
public:
    EvalTraceProperty_EvalOrderCommutativity() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-eval-order-commutativity");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 50;  // two cold evals per expression per iteration
    return params;
}

TEST_F(EvalTraceProperty_EvalOrderCommutativity, OrderIndependentCacheHit)
{
    rc::detail::checkGTestWith(
        [this](TestExpr exprA, TestExpr exprB) {
            RC_PRE(exprA.expectsSuccess());
            RC_PRE(exprB.expectsSuccess());

            // Guard: GetEnvGen requires impure eval.
            RC_PRE(!state.settings.pureEval);

            // Guard: expressions must have distinct nixCode to avoid trivial
            // same-expression tests (though the property still holds, it's less useful).
            RC_PRE(exprA.nixCode != exprB.nixCode);

            // Use separate fingerprints for A and B so they write to
            // different Sessions rows (both would otherwise target
            // AttrPathId(0) in the same session namespace).
            auto fpA = hashString(HashAlgorithm::SHA256, "prop-commutativity-A");
            auto fpB = hashString(HashAlgorithm::SHA256, "prop-commutativity-B");
            auto savedFp = testFingerprint;

            // --- Ordering 1: eval A then B ---
            {
                testFingerprint = fpA;
                auto cA = makeCache(exprA.nixCode);
                (void) forceRoot(*cA);
            }
            {
                testFingerprint = fpB;
                auto cB = makeCache(exprB.nixCode);
                (void) forceRoot(*cB);
            }

            // --- Ordering 2: eval B then A ---
            // (makeCache releases the previous session automatically)
            {
                testFingerprint = fpB;
                auto cB = makeCache(exprB.nixCode);
                (void) forceRoot(*cB);
            }
            {
                testFingerprint = fpA;
                auto cA = makeCache(exprA.nixCode);
                (void) forceRoot(*cA);
            }

            // --- Final warm pass: both must hit cache ---
            int callsA = 0;
            {
                testFingerprint = fpA;
                auto cA = makeCache(exprA.nixCode, &callsA);
                (void) forceRoot(*cA);
            }
            RC_ASSERT(callsA == 0);

            int callsB = 0;
            {
                testFingerprint = fpB;
                auto cB = makeCache(exprB.nixCode, &callsB);
                (void) forceRoot(*cB);
            }
            RC_ASSERT(callsB == 0);

            testFingerprint = savedFp;
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
