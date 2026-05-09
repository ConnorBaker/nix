#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

class EvalTraceProperty_CacheSoundness : public TraceCacheFixture {
public:
    EvalTraceProperty_CacheSoundness() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-cache-soundness");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    return params;
}

TEST_F(EvalTraceProperty_CacheSoundness, ScalarReadFileGetEnv)
{
    rc::detail::checkGTestWith(
        [this](TestExpr expr) {
            RC_PRE(expr.expectsSuccess());

            // Guard: GetEnvGen requires impure eval.
            // Use RC_PRE (not ASSERT_FALSE) because ASSERT_* inside a lambda
            // only exits the lambda, not the test.
            RC_PRE(!state.settings.pureEval);

            // Cold eval: evaluates expression and records trace
            Value coldVal;
            {
                auto cache = makeCache(expr.nixCode);
                coldVal = forceRoot(*cache);
            }

            // Warm eval: verifies trace, serves from cache
            Value warmVal;
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                warmVal = forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            assertValuesEqual(coldVal, warmVal);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
