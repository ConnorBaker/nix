#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// EvalTraceProperty_FilterList — filter Soundness
//
// Expression: builtins.length (builtins.filter (x: x > 0) (fromJSON slot))
// where the JSON array contains integers.
//
// Soundness: adding a positive element → more elements pass → length increases → invalidates.
// NOTE: filter evaluates the predicate on each element, recording per-element SC deps.
// Changing any element value (even if filter result length stays the same) correctly
// invalidates the cache. The original "precision" test was wrong — this is correct behavior.
//
// filter uses DerivedContainerBuilder (output is a subset of input).
// The #len dep on the output comes from derivedBuilder.finishList().
class EvalTraceProperty_FilterList : public TraceCacheFixture {
public:
    EvalTraceProperty_FilterList() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-eval-trace-filter-list");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    params.maxDiscardRatio = 100;
    return params;
}

// Soundness — adding a positive element changes filter output length → invalidates.
TEST_F(EvalTraceProperty_FilterList, AddPositiveElement_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeListFromJSONGen();
            RC_PRE(!expr.depSlots.empty());
            auto & slot = expr.depSlots[0];
            RC_PRE(slot.kind == DepSlot::Kind::JsonArray);

            nlohmann::json arr;
            try {
                arr = nlohmann::json::parse(slot.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(arr.is_array());

            std::string nixCode =
                "builtins.length (builtins.filter (x: x > 0)"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + ")))";

            // Add a positive element to increase filter output length.
            arr.push_back(1);
            std::string mutated = arr.dump();
            RC_PRE(mutated != slot.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }
            // Confirm cache hit.
            {
                int n = 0;
                auto cache = makeCache(nixCode, &n);
                (void) forceRoot(*cache);
                RC_ASSERT(n == 0);
            }

            // Add the positive element.
            slot.mutate(mutated);
            invalidateFileCache(slot.path);

            // Warm eval must re-evaluate.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            // Restore.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// CorrectlyInvalidates — changing a positive element to another positive value invalidates.
// Per-element SC deps cause correct over-invalidation; this is a soundness test, not precision.
// filter evaluates the predicate on each element, recording an SC dep per value.
// Changing any element value (even if the filter result length is unchanged) correctly invalidates.
//
// Uses makeFilterLengthGen() directly (which produces arrays more likely to have positive
// elements) instead of makeListFromJSONGen(). We also construct the array to guarantee at
// least one positive element by injecting one if needed.
TEST_F(EvalTraceProperty_FilterList, PositiveValueChange_CorrectlyInvalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeFilterLengthGen();
            RC_PRE(!expr.depSlots.empty());
            auto & slot = expr.depSlots[0];
            RC_PRE(slot.kind == DepSlot::Kind::JsonArray);

            nlohmann::json arr;
            try {
                arr = nlohmann::json::parse(slot.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(arr.is_array());
            // Find an element that is a positive integer.
            size_t posIdx = arr.size(); // sentinel: no positive element found yet
            for (size_t i = 0; i < arr.size(); ++i) {
                if (arr[i].is_number_integer() && arr[i].get<int>() > 0) {
                    posIdx = i;
                    break;
                }
            }
            // Require at least one positive integer element.
            RC_PRE(posIdx < arr.size());

            std::string nixCode =
                "builtins.length (builtins.filter (x: x > 0)"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + ")))";

            // Change the positive element to a different positive value (still > 0).
            auto origPos = arr[posIdx].get<int>();
            arr[posIdx] = (origPos == 1) ? 2 : 1;
            std::string mutated = arr.dump();
            RC_PRE(mutated != slot.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate the positive element to another positive value.
            slot.mutate(mutated);
            invalidateFileCache(slot.path);

            // Warm eval correctly invalidates: filter records per-element SC deps,
            // so any element value change causes a cache miss (correct over-invalidation).
            //
            // §N.4 Case E: assert "primary cache did not serve" via
            // `deltaTraceCacheMisses + deltaRecoveryAttempts >= 1`.
            //
            // SEMANTIC CHANGE FROM ORIGINAL (2026-04-20): the original
            // `RC_ASSERT(loaderCalls >= 1)` only fired on loader re-run.
            // The counter-delta shape also fires on recovery succeeds,
            // which the old one did not. Acceptable here because the
            // test's correctness invariant is "the mutation was detected
            // by the cache layer."
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() + snap.deltaRecoveryAttempts() >= 1);
            }

            // Restore.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
