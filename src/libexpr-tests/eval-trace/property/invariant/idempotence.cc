#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

class EvalTraceProperty_CacheIdempotence : public TraceCacheFixture {
public:
    EvalTraceProperty_CacheIdempotence() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-cache-idempotence");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    return params;
}

TEST_F(EvalTraceProperty_CacheIdempotence, CachedEvalIsCachedEval)
{
    rc::detail::checkGTestWith(
        [this](TestExpr expr) {
            RC_PRE(expr.expectsSuccess());

            // Guard: GetEnvGen requires impure eval.
            // Use RC_PRE (not ASSERT_FALSE) because ASSERT_* inside a lambda
            // only exits the lambda, not the test.
            RC_PRE(!state.settings.pureEval);

            // True idempotence: multiple evaluations produce the same value,
            // regardless of whether the cache hits or misses. Unlike CacheSoundness
            // (soundness.cc), we do NOT assert loaderCalls == 0 — the property
            // is about value consistency, not cache behavior.

            // First eval
            Value val1;
            {
                auto cache = makeCache(expr.nixCode);
                val1 = forceRoot(*cache);
            }

            // Second eval (may or may not hit cache — doesn't matter)
            Value val2;
            {
                auto cache = makeCache(expr.nixCode);
                val2 = forceRoot(*cache);
            }

            // Both evaluations must produce identical values
            assertValuesEqual(val1, val2);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
