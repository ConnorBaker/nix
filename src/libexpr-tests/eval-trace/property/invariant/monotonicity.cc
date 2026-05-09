#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// InvalidationMonotonicity — Monotonicity: Adding deps to A never reduces cache hits for unrelated B.
//
// Formal:
//   ∀ A (with at least one dep), B (independent, disjoint paths):
//     eval(A); eval(B)               — both recorded
//     mutate(A's dep) → re-eval(A)   — A's trace is updated with new deps
//     eval(B) must still hit cache   — B is unaffected
//
// This catches dep-set growth causing spurious misses in unrelated attrs:
// epoch-log pollution, broadcast invalidation, or shared-state bugs where
// A's invalidation affects B's trace verification.
class EvalTraceProperty_InvalidationMonotonicity : public TraceCacheFixture {
public:
    EvalTraceProperty_InvalidationMonotonicity() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-invalidation-monotonicity");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 50;
    params.maxDiscardRatio = 50;
    return params;
}

TEST_F(EvalTraceProperty_InvalidationMonotonicity, AInvalidationDoesNotAffectB)
{
    rc::detail::checkGTestWith(
        [this](TestExpr exprA, TestExpr exprB) {
            RC_PRE(exprA.expectsSuccess());
            RC_PRE(exprB.expectsSuccess());

            // Guard: GetEnvGen requires impure eval.
            RC_PRE(!state.settings.pureEval);

            // A must have at least one dep slot so we can mutate it.
            RC_PRE(!exprA.depSlots.empty());

            // Skip container expressions — SC override may correctly serve
            // from cache when only scalar values change but shape is unchanged.
            RC_PRE(exprA.expectedKind != TestExpr::ResultKind::Attrset);
            RC_PRE(exprA.expectedKind != TestExpr::ResultKind::List);

            // Restrict to Kind::File for the same reasons as DepInvalidation (invalidation.cc).
            // See invalidation.cc for the full rationale:
            //   Kind::JsonFile / Kind::JsonArray — SC override may correctly
            //     serve from cache (hasAttr with value-preserving mutation).
            //   Kind::EnvVar — empirically flaky in the shared-DB iteration model.
            //   Kind::FileExistence — file deletion issue.
            RC_PRE(exprA.depSlots[0].kind == DepSlot::Kind::File);

            // A and B must have distinct nixCode (otherwise they share the same
            // trace entry and invalidating A's dep would invalidate B too).
            RC_PRE(exprA.nixCode != exprB.nixCode);

            // B must not share any dep file paths with A — otherwise
            // invalidating A's dep also invalidates B (via shared file),
            // or B has a TraceParentSlot dep on A via LetGen wrapping.
            for (auto & slotB : exprB.depSlots)
                for (auto & slotA_ : exprA.depSlots)
                    RC_PRE(slotB.path != slotA_.path);

            auto & slotA = exprA.depSlots[0];

            // Pick a mutation value that actually changes the dep.
            auto newValueA = *slotA.generateMutation();
            RC_PRE(newValueA != slotA.currentValue);

            // Use separate fingerprints for A and B so they write to
            // different Sessions rows.  Without this, both A and B target
            // (session_key, AttrPathId(0)) → B never records its own trace.
            auto fpA = hashString(HashAlgorithm::SHA256, "prop-monotonicity-A");
            auto fpB = hashString(HashAlgorithm::SHA256, "prop-monotonicity-B");
            auto savedFp = testFingerprint;

            // --- Cold eval both A and B ---
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

            // Confirm B is a cache hit (precision pre-condition before we mutate A).
            {
                testFingerprint = fpB;
                int n = 0;
                auto cB = makeCache(exprB.nixCode, &n);
                (void) forceRoot(*cB);
                RC_ASSERT(n == 0);
            }

            // --- Mutate A's dep slot ---
            slotA.mutate(newValueA);
            // All file-backed slot kinds require file cache invalidation.
            if (slotA.kind != DepSlot::Kind::EnvVar)
                invalidateFileCache(slotA.path);

            // Re-eval A (should miss and record new trace).
            {
                testFingerprint = fpA;
                int n = 0;
                auto cA = makeCache(exprA.nixCode, &n);
                (void) forceRoot(*cA);
                // A must have re-evaluated after dep mutation.
                RC_ASSERT(n == 1);
            }

            // --- B must still hit cache despite A's dep change ---
            {
                testFingerprint = fpB;
                int callsB = 0;
                auto cB = makeCache(exprB.nixCode, &callsB);
                (void) forceRoot(*cB);
                RC_ASSERT(callsB == 0);
            }

            // Restore A's slot and fingerprint for clean state.
            testFingerprint = savedFp;
            slotA.restore();
            if (slotA.kind != DepSlot::Kind::EnvVar)
                invalidateFileCache(slotA.path);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
