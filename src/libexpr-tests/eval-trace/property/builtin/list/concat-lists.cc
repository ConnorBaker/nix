#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// EvalTraceProperty_ConcatLists — concatLists Soundness and Precision
//
// Expression: builtins.length (builtins.concatLists [(fromJSON slotA) (fromJSON slotB)])
//
// Soundness: adding an element to one array invalidates (total length increases).
// Precision: changing an element value in one array (same length) → cache hit.
//
// concatLists does NOT use DerivedContainerBuilder.
// Length dep comes from the downstream length call via forceListObserved.
class EvalTraceProperty_ConcatLists : public TraceCacheFixture {
public:
    EvalTraceProperty_ConcatLists() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-eval-trace-concat-lists");
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

// Soundness — adding an element to one array invalidates.
TEST_F(EvalTraceProperty_ConcatLists, AddElement_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto exprA = *makeListFromJSONGen();
            RC_PRE(!exprA.depSlots.empty());
            auto & slotA = exprA.depSlots[0];
            RC_PRE(slotA.kind == DepSlot::Kind::JsonArray);

            auto exprB = *makeListFromJSONGen();
            RC_PRE(!exprB.depSlots.empty());
            auto & slotB = exprB.depSlots[0];
            RC_PRE(slotB.kind == DepSlot::Kind::JsonArray);

            nlohmann::json arrA;
            try {
                arrA = nlohmann::json::parse(slotA.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(arrA.is_array());

            std::string nixCode =
                "builtins.length (builtins.concatLists"
                " [(builtins.fromJSON (builtins.readFile " + slotA.path.string() + "))"
                "  (builtins.fromJSON (builtins.readFile " + slotB.path.string() + "))])";

            // Add an element to slotA to increase total length.
            arrA.push_back(99);
            std::string mutatedA = arrA.dump();
            RC_PRE(mutatedA != slotA.currentValue);

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

            // Mutate slotA (add element).
            slotA.mutate(mutatedA);
            invalidateFileCache(slotA.path);

            // Warm eval must re-evaluate.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            // Restore.
            slotA.restore();
            invalidateFileCache(slotA.path);
        },
        makeParams);
}

// Precision — changing an element value in one array (same lengths) → cache hit.
TEST_F(EvalTraceProperty_ConcatLists, ElementValueChange_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto exprA = *makeListFromJSONGen();
            RC_PRE(!exprA.depSlots.empty());
            auto & slotA = exprA.depSlots[0];
            RC_PRE(slotA.kind == DepSlot::Kind::JsonArray);

            auto exprB = *makeListFromJSONGen();
            RC_PRE(!exprB.depSlots.empty());
            auto & slotB = exprB.depSlots[0];
            RC_PRE(slotB.kind == DepSlot::Kind::JsonArray);

            nlohmann::json arrA;
            try {
                arrA = nlohmann::json::parse(slotA.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(arrA.is_array());
            // Need at least one element to mutate its value.
            RC_PRE(!arrA.empty());

            std::string nixCode =
                "builtins.length (builtins.concatLists"
                " [(builtins.fromJSON (builtins.readFile " + slotA.path.string() + "))"
                "  (builtins.fromJSON (builtins.readFile " + slotB.path.string() + "))])";

            // Change element 0's value (same array length).
            auto origVal = arrA[0];
            arrA[0] = (origVal.is_number_integer() && origVal.get<int>() != 999) ? 999 : 0;
            std::string mutatedA = arrA.dump();
            RC_PRE(mutatedA != slotA.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate element value (same length).
            slotA.mutate(mutatedA);
            invalidateFileCache(slotA.path);

            // Warm eval must be a cache hit.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Restore.
            slotA.restore();
            invalidateFileCache(slotA.path);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
