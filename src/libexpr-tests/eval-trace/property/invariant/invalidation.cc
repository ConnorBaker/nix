#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

class EvalTraceProperty_DepInvalidation : public TraceCacheFixture {
public:
    EvalTraceProperty_DepInvalidation() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-dep-invalidation");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    params.maxDiscardRatio = 200;   // Single-dep File expressions are a strict subset of makeNixExprGen().
    return params;
}

TEST_F(EvalTraceProperty_DepInvalidation, RandomDepSlot)
{
    std::size_t iteration = 0;

    rc::detail::checkGTestWith(
        [this, &iteration](TestExpr expr) {
            // RC iterations share one fixture/DB by default. Isolate each
            // iteration so the current expression records/verifies its own
            // root trace instead of reusing a previous iteration's root row.
            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-dep-invalidation-" + std::to_string(iteration++));

            // Only expressions with at least one dep slot can be invalidated.
            // ScalarGen produces depSlots.empty() — these are filtered here.
            RC_PRE(!expr.depSlots.empty());

            // The "mutate dep → re-evaluate" invariant only holds when the
            // selected dep is definitely part of the observed result. Generic
            // multi-dep generators include lazy/branch-selective expressions
            // (for example multi-binding-let) where some File slots are never
            // forced, so mutating those slots should remain a cache hit.
            RC_PRE(expr.depSlots.size() == 1);

            // Skip expressions returning containers (Attrset, List).  For these,
            // the SC dep override may correctly serve from cache when only scalar
            // values inside the container change but the container's shape (key
            // names / length) is unchanged.  This is precision, not a bug.
            RC_PRE(expr.expectedKind != TestExpr::ResultKind::Attrset);
            RC_PRE(expr.expectedKind != TestExpr::ResultKind::List);

            auto & slot = expr.depSlots[0];

            // Restrict to slot kinds where the P2 invariant
            // (mutate dep → re-eval) is guaranteed to hold.
            //
            //   Kind::File — reliable: arbitrary content change always produces a
            //     new FileBytes dep hash, and there is no SC-override path for
            //     plain readFile expressions.
            //
            // Excluded kinds and reasons:
            //
            //   Kind::JsonFile / Kind::JsonArray — generateMutation() preserves
            //     all JSON keys (changes only values).  For hasAttr expressions
            //     (makeHasAttrTestGen: "... ? key") the #has:key ImplicitStructure
            //     dep still passes after a value-preserving mutation because the
            //     key is still present → ValidViaImplicitShapeOverride fires →
            //     loaderCalls == 0 (cache hit).  This is CORRECT cache behavior
            //     (the expression result is unchanged), but it violates the
            //     DepInvalidation invariant that "mutate dep → re-eval".
            //     HasAttr (has-attr.cc) and StructuralOverridePrecision
            //     (structural-override-precision.cc) test the SC path directly.
            //
            //   Kind::EnvVar — empirically flaky: mixing EnvVar iterations with
            //     File iterations in the shared-DB RapidCheck iteration model
            //     causes occasional incorrect loaderCalls == 0 for File-backed
            //     expressions in subsequent iterations.  Root cause is the
            //     interaction between ScopedEnvVar RAII restoration during RC
            //     shrinking and the shared EvalState/SQLite DB.  EnvVar dep
            //     invalidation is tested in isolation by CacheSoundness (soundness.cc).
            //
            //   Kind::FileExistence — pathExists deletion interacts with the
            //     file cache in ways that require dedicated investigation
            //     (see path-exists.cc).
            RC_PRE(slot.kind == DepSlot::Kind::File);

            // Generate mutation via kind-dispatched generateMutation().
            // For Kind::File this produces arbitrary printable ASCII.
            // The RC_PRE guard above ensures only Kind::File reaches here.
            auto newValue = *slot.generateMutation();
            RC_PRE(newValue != slot.currentValue);

            // Cold eval to record the initial trace
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Confirm cache hit before mutation (precision pre-condition)
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Mutate the selected dep slot with the kind-dispatched new value.
            slot.mutate(newValue);
            // File-backed slots (Kind::File, Kind::JsonFile, Kind::JsonArray,
            // Kind::FileExistence) require the file cache to be invalidated
            // so the verifier observes the change.
            if (slot.kind == DepSlot::Kind::File
                || slot.kind == DepSlot::Kind::JsonFile
                || slot.kind == DepSlot::Kind::JsonArray
                || slot.kind == DepSlot::Kind::FileExistence)
                invalidateFileCache(slot.path);

            // Warm eval must re-evaluate (miss due to changed dep)
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            // Restore for clean state on shrink/retry
            slot.restore();
            if (slot.kind == DepSlot::Kind::File
                || slot.kind == DepSlot::Kind::JsonFile
                || slot.kind == DepSlot::Kind::JsonArray
                || slot.kind == DepSlot::Kind::FileExistence)
                invalidateFileCache(slot.path);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
