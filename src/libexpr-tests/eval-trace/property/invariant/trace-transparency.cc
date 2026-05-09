// Trace transparency invariant: traced evaluation must produce the same
// result as plain (untraced) evaluation.
//
// This is a stronger invariant than P1 soundness (cold == warm).  P1 checks
// that the cache serves what it recorded.  Transparency checks that the
// trace cache doesn't alter evaluation semantics — the result of evaluating
// through TraceSession must equal the result of a plain state.eval().
//
// The untraced eval runs FIRST (before makeCache) to avoid reading cached
// state from the traced eval's fileContentHashes.

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

class EvalTraceProperty_TraceTransparency : public TraceCacheFixture {
public:
    EvalTraceProperty_TraceTransparency() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-trace-transparency");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    return params;
}

// Invariant: forall E: eval(E) == traced_eval(E)
TEST_F(EvalTraceProperty_TraceTransparency, TracedEvalMatchesPlainEval)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration](TestExpr expr) {
            RC_PRE(expr.expectsSuccess());
            RC_PRE(!state.settings.pureEval);

            // Each RC iteration evaluates a different root expression. Reusing
            // one fingerprint across the whole property lets a previous
            // iteration's root trace occupy AttrPathId(0), so the "cold"
            // traced eval can verify and serve the wrong cached root value.
            // Give every iteration its own namespace and clear in-process
            // caches so plain vs traced eval compares the current expression.
            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-trace-transparency-" + std::to_string(iteration++));

            // Step 1: plain (untraced) eval — no trace context active.
            // Must run BEFORE makeCache so no fileContentHash state from
            // traced eval contaminates the plain eval's filesystem reads.
            Value untracedVal = eval(expr.nixCode);
            state.forceValue(untracedVal, noPos);

            // Step 2: traced eval (cold — records trace).
            auto cache = makeCache(expr.nixCode);
            Value tracedVal = forceRoot(*cache);

            // Step 3: compare. Both values are from the same EvalState.
            assertValuesEqual(untracedVal, tracedVal);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
