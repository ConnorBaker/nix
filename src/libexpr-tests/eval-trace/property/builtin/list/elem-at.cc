#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// EvalTraceProperty_ElemAt — head / elemAt Soundness and Precision
//
// Expression: builtins.elemAt (fromJSON slot) idx
// where fromJSON returns a JSON array with at least 2 elements.
//
// Soundness: changing the accessed element invalidates.
// Precision: changing a non-accessed element does not invalidate.
//
// elemAt does NOT call forceListObserved — it accesses element N via listView()[n].
// The per-element SC dep comes from the element's TracedExpr provenance in the JSON array.
class EvalTraceProperty_ElemAt : public TraceCacheFixture {
public:
    EvalTraceProperty_ElemAt() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-eval-trace-elem-at");
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

// Soundness — changing the accessed element invalidates.
TEST_F(EvalTraceProperty_ElemAt, AccessedElement_Invalidates)
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
            // Need at least 2 elements so the precision test can pick a non-accessed one.
            RC_PRE(arr.size() >= 2);

            // Pick a random index to access.
            auto idx = *rc::gen::inRange<size_t>(0, arr.size());

            std::string nixCode =
                "builtins.elemAt"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + "))"
                " " + std::to_string(idx);

            // Mutate the accessed element.
            auto origVal = arr[idx];
            arr[idx] = (origVal.is_number_integer() && origVal.get<int>() != 999) ? 999 : 0;
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

            // Mutate the accessed element.
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

// Precision — changing a non-accessed element does not invalidate.
TEST_F(EvalTraceProperty_ElemAt, UnaccesedElement_CacheHit)
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
            // Need at least 2 elements so there is always a non-accessed element.
            RC_PRE(arr.size() >= 2);

            // Access element 0; mutate a different element.
            size_t accessIdx = 0;
            // Pick the index to mutate: any index that is not the accessed one.
            auto mutateIdx = *rc::gen::inRange<size_t>(1, arr.size());

            std::string nixCode =
                "builtins.elemAt"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + "))"
                " " + std::to_string(accessIdx);

            // Mutate the non-accessed element.
            auto origVal = arr[mutateIdx];
            arr[mutateIdx] = (origVal.is_number_integer() && origVal.get<int>() != 999) ? 999 : 0;
            std::string mutated = arr.dump();
            RC_PRE(mutated != slot.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate the non-accessed element.
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

// Soundness — builtins.head (sugar for elemAt 0) — changing element 0 invalidates.
TEST_F(EvalTraceProperty_ElemAt, Head_AccessedElement_Invalidates)
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
            RC_PRE(arr.size() >= 2);

            // Use builtins.head which is sugar for elemAt 0.
            std::string nixCode =
                "builtins.head"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + "))";

            // Mutate element 0 (the accessed element).
            auto origVal = arr[0];
            arr[0] = (origVal.is_number_integer() && origVal.get<int>() != 777) ? 777 : 0;
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
