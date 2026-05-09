#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P19 — map Soundness and Precision
//
// Expression: builtins.length (builtins.map (x: x) (fromJSON slot))
// where fromJSON returns a JSON array.
//
// Soundness: adding an element to the array → length increases → invalidates.
// Precision: changing an element's value without changing the list length → cache hit.
//
// map does NOT use DerivedContainerBuilder — length dep comes from the downstream
// `length` call which calls forceListObserved.
class EvalTraceProperty_MapList : public TraceCacheFixture {
public:
    EvalTraceProperty_MapList() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-map-list");
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

// P19a: Soundness — adding an element invalidates.
TEST_F(EvalTraceProperty_MapList, AddElement_Invalidates)
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
                "builtins.length (builtins.map (x: x)"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + ")))";

            // Add an element.
            arr.push_back(42);
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

// P19b: Precision — changing an element's value (same length) → cache hit.
TEST_F(EvalTraceProperty_MapList, ElementValueChange_CacheHit)
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
                "builtins.length (builtins.map (x: x)"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + ")))";

            // Change element 0's value (same array length).
            auto v = arr[0].get<int>();
            arr[0] = (v == 100) ? 99 : v + 1;
            std::string mutated = arr.dump();
            RC_PRE(mutated != slot.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate element value.
            slot.mutate(mutated);
            invalidateFileCache(slot.path);

            // Warm eval must be a cache hit (length unchanged).
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

} // namespace nix::eval_trace::proptest
