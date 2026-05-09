#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P21 — length Soundness and Precision
//
// Expression: builtins.length (fromJSON slot)
// where fromJSON returns a JSON array.
//
// Soundness: adding/removing an element → length changes → invalidates.
// Precision: changing an element's value without changing list length → cache hit.
//
// length calls forceListObserved which records the #len dep on the list's
// ImplicitStructure provenance.
class EvalTraceProperty_ListLength : public TraceCacheFixture {
public:
    EvalTraceProperty_ListLength() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-list-length");
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

// P21a: Soundness — adding an element invalidates (length changes).
TEST_F(EvalTraceProperty_ListLength, AddElement_Invalidates)
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
                "builtins.length"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + "))";

            // Add an element.
            arr.push_back(0);
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

            // Mutate.
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

// P21b: Precision — changing element value (same length) → cache hit.
TEST_F(EvalTraceProperty_ListLength, ElementValueChange_CacheHit)
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
            RC_PRE(!arr.empty());

            std::string nixCode =
                "builtins.length"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + "))";

            // Change element 0's value.
            auto v = arr[0].get<int>();
            arr[0] = (v == 100) ? 99 : v + 1;
            std::string mutated = arr.dump();
            RC_PRE(mutated != slot.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate.
            slot.mutate(mutated);
            invalidateFileCache(slot.path);

            // Warm eval must be a cache hit.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Restore.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// P21c: Soundness — removing an element invalidates.
TEST_F(EvalTraceProperty_ListLength, RemoveElement_Invalidates)
{
    std::size_t iteration = 0;
    rc::detail::checkGTestWith(
        [this, &iteration]() {
            // Clear shared DB state between RC iterations to prevent cross-
            // iteration contamination from stale trace hash cache entries.
            simulateWarmRestart();
            testFingerprint = hashString(
                HashAlgorithm::SHA256,
                "prop-list-length-remove-" + std::to_string(iteration++));

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
            // Need at least one element to remove.
            RC_PRE(!arr.empty());

            std::string nixCode =
                "builtins.length"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + "))";

            // Remove the last element.
            arr.erase(arr.end() - 1);
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

            // Mutate.
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

} // namespace nix::eval_trace::proptest
