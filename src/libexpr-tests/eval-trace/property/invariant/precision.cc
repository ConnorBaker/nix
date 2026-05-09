#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

class EvalTraceProperty_UnrelatedFilePrecision : public TraceCacheFixture {
public:
    EvalTraceProperty_UnrelatedFilePrecision() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-unrelated-file-precision");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    return params;
}

TEST_F(EvalTraceProperty_UnrelatedFilePrecision, UnrelatedFileMutation)
{
    std::size_t iteration = 0;
    rc::detail::checkGTestWith(
        [this, &iteration](TestExpr expr, std::string unrelatedContent) {
            simulateWarmRestart();
            testFingerprint = hashString(HashAlgorithm::SHA256,
                "prop-unrelated-precision-" + std::to_string(iteration++));

            RC_PRE(expr.expectsSuccess());

            // ScalarGen produces depSlots.empty() — these iterations are
            // trivially true because there are no deps that could be affected
            // by the unrelated file mutation.  Filter them out to avoid
            // polluting coverage statistics with zero-signal inputs.
            RC_PRE(!expr.depSlots.empty());

            // Guard: GetEnvGen requires impure eval.
            // Use RC_PRE (not ASSERT_FALSE) because ASSERT_* inside a lambda
            // only exits the lambda, not the test.
            RC_PRE(!state.settings.pureEval);

            // Create a temp file not referenced by expr.
            // Its path never appears in expr.nixCode — guaranteed because the
            // generator constructs nixCode before this lambda runs, and
            // TempTextFile uses pid+counter naming that is distinct from any
            // dep slot path already embedded in nixCode.
            TempTextFile unrelated(unrelatedContent);

            // Cold eval — evaluates expression and records trace.
            // The unrelated file is not referenced by the expression and so
            // cannot appear in any recorded dep.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Modify the unrelated file and invalidate the file cache.
            // This simulates what would happen if a real filesystem change were
            // observed for a file the expression does not depend on.
            unrelated.modify(unrelatedContent + "_changed");
            invalidateFileCache(unrelated.path);

            // Warm eval — must still be a cache hit.
            // The unrelated file is not in the trace deps, so the verifier
            // must not re-evaluate the expression.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
