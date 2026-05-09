#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P31 — pathExists Soundness and Precision
//
// Formal:
//   Soundness: ∀ path: pathExists(path), toggle_existence(path) → re-eval
//   Precision: changing an unrelated file does not invalidate pathExists
//
// pathExists records an ExistenceCheck dep.  Mutation toggles "exists" ↔ "missing".
// The initial value is always "exists" (file created before TestExpr is returned).
class EvalTraceProperty_PathExists : public TraceCacheFixture {
public:
    EvalTraceProperty_PathExists() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-path-exists");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    return params;
}

// P31a: Soundness — toggling file existence invalidates the cache.
//
TEST_F(EvalTraceProperty_PathExists, ToggleExistence_Invalidates)
{
    std::size_t iteration = 0;
    rc::detail::checkGTestWith(
        [this, &iteration]() {
            simulateWarmRestart();
            testFingerprint = hashString(HashAlgorithm::SHA256,
                "prop-path-exists-" + std::to_string(iteration++));
            auto expr = *makePathExistsGen();

            RC_PRE(!expr.depSlots.empty());
            auto & slot = expr.depSlots[0];
            RC_PRE(slot.kind == DepSlot::Kind::FileExistence);

            // Cold eval: evaluates pathExists and records ExistenceCheck dep.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Confirm cache hit before mutation (precision pre-condition).
            {
                int n = 0;
                auto cache = makeCache(expr.nixCode, &n);
                (void) forceRoot(*cache);
                RC_ASSERT(n == 0);
            }

            // Toggle existence: "exists" → "missing" (delete the file).
            auto newValue = *slot.generateMutation();
            RC_PRE(newValue != slot.currentValue);
            slot.mutate(newValue);
            invalidateFileCache(slot.path);

            // Warm eval must re-evaluate (ExistenceCheck dep changed).
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            // Restore for clean state on shrink/retry.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// P31b: Precision — changing an unrelated file does not invalidate pathExists.
TEST_F(EvalTraceProperty_PathExists, UnrelatedFile_CacheHit)
{
    std::size_t iteration = 0;
    rc::detail::checkGTestWith(
        [this, &iteration](std::string unrelatedContent) {
            simulateWarmRestart();
            testFingerprint = hashString(HashAlgorithm::SHA256,
                "prop-path-exists-precision-" + std::to_string(iteration++));

            auto expr = *makePathExistsGen();

            RC_PRE(!expr.depSlots.empty());

            // Create a temp file whose path never appears in expr.nixCode.
            TempTextFile unrelated(unrelatedContent);

            // Cold eval: records ExistenceCheck dep for the pathExists path only.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Modify the unrelated file — should not affect pathExists dep.
            unrelated.modify(unrelatedContent + "_changed");
            invalidateFileCache(unrelated.path);

            // Warm eval must still be a cache hit.
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
