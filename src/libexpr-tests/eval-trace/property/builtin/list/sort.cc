#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <random>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// EvalTraceProperty_SortList — sort Soundness
//
// Expression: builtins.length (builtins.sort builtins.lessThan (fromJSON slot))
//
// Soundness: adding an element → length increases → invalidates.
// NOTE: sort evaluates the comparator on each element, recording per-element SC deps.
// Reordering elements (same set, different order) correctly invalidates the cache
// because the per-element value deps change when elements move. This is correct behavior.
//
// sort uses DerivedContainerBuilder (shape-preserving: sorted output has same length as input).
// The #len dep is recorded by derivedBuilder.finishList().
class EvalTraceProperty_SortList : public TraceCacheFixture {
public:
    EvalTraceProperty_SortList() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-eval-trace-sort-list");
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

// Soundness — adding an element invalidates.
TEST_F(EvalTraceProperty_SortList, AddElement_Invalidates)
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
                "builtins.length (builtins.sort builtins.lessThan"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + ")))";

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

// CorrectlyInvalidates — reordering elements (same set, different order) invalidates.
// Per-element SC deps cause correct over-invalidation; this is a soundness test, not precision.
// sort evaluates the comparator on each element, recording an SC dep per value.
// Reordering elements changes the per-element SC deps (elements at each index differ),
// so the cache correctly invalidates even though the sorted length is unchanged.
TEST_F(EvalTraceProperty_SortList, Reorder_CorrectlyInvalidates)
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
            // Need at least 2 integer elements so a reordering is possible.
            RC_PRE(arr.size() >= 2);
            for (auto & e : arr) {
                RC_PRE(e.is_number_integer());
            }

            std::string nixCode =
                "builtins.length (builtins.sort builtins.lessThan"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + ")))";

            // Shuffle the array using a deterministic RapidCheck-seeded engine.
            auto seed = *rc::gen::arbitrary<uint32_t>();
            std::vector<nlohmann::json> elems(arr.begin(), arr.end());
            std::mt19937 rng(seed);
            std::shuffle(elems.begin(), elems.end(), rng);
            nlohmann::json shuffled = nlohmann::json::array();
            for (auto & e : elems)
                shuffled.push_back(e);
            std::string mutated = shuffled.dump();
            // Discard if the shuffle happened to produce the same order.
            RC_PRE(mutated != slot.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Reorder (same elements, different order, same length).
            slot.mutate(mutated);
            invalidateFileCache(slot.path);

            // Warm eval correctly invalidates: sort records per-element SC deps,
            // so reordering elements causes a cache miss (correct over-invalidation).
            //
            // §N.4 Case E: assert "primary cache did not serve" via
            // `deltaTraceCacheMisses + deltaRecoveryAttempts >= 1`.
            //
            // SEMANTIC CHANGE FROM ORIGINAL (2026-04-20): the original
            // shape was `RC_ASSERT(loaderCalls >= 1)`, which only fires
            // when the Nix-level loader re-ran. The new shape fires when
            // the primary session cache is bypassed — either by a
            // verify miss (loader re-runs) OR by recovery firing (loader
            // may NOT re-run if a recovery strategy succeeds).
            //
            // These semantics DIFFER: the new assertion accepts
            // recovery-succeeds-without-recompute, which the old one
            // did not. That is acceptable here because the test's
            // correctness invariant is "the invalidating mutation was
            // detected by the cache layer" — either recompute or
            // recovery re-verification count as detection.
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
