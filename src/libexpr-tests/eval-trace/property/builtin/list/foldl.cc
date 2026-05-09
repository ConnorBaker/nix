#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// EvalTraceProperty_Foldl — foldl' Soundness
//
// Expression: builtins.foldl' (a: b: a + b) 0 (fromJSON slot)
// where the JSON array contains integers.
//
// Soundness: changing any element's value invalidates (all elements are accessed
// by foldl' — it forces each element in turn).
//
// Precision: changing an unrelated file does not invalidate.
//
// foldl' calls forceListObserved and then forces each element —
// all element SC deps are recorded.
class EvalTraceProperty_Foldl : public TraceCacheFixture {
public:
    EvalTraceProperty_Foldl() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-eval-trace-foldl");
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

// Soundness — changing the first element invalidates.
TEST_F(EvalTraceProperty_Foldl, FirstElement_Invalidates)
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
            // Ensure all elements are integers (the expression uses a + b).
            for (auto & e : arr) {
                RC_PRE(e.is_number_integer());
            }

            std::string nixCode =
                "builtins.foldl' (a: b: a + b) 0"
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
            // Confirm cache hit.
            {
                int n = 0;
                auto cache = makeCache(nixCode, &n);
                (void) forceRoot(*cache);
                RC_ASSERT(n == 0);
            }

            // Mutate element 0.
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

// Soundness — changing the last element invalidates.
TEST_F(EvalTraceProperty_Foldl, LastElement_Invalidates)
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
            // Need at least one element to mutate the last one.
            RC_PRE(!arr.empty());
            // Ensure all elements are integers (the expression uses a + b).
            for (auto & e : arr) {
                RC_PRE(e.is_number_integer());
            }

            std::string nixCode =
                "builtins.foldl' (a: b: a + b) 0"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + "))";

            // Mutate the last element's value.
            size_t lastIdx = arr.size() - 1;
            auto v = arr[lastIdx].get<int>();
            arr[lastIdx] = (v == 999) ? 0 : 999;
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

            // Mutate last element.
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

// Precision — changing an unrelated file does not invalidate.
TEST_F(EvalTraceProperty_Foldl, UnrelatedFile_CacheHit)
{
    rc::detail::checkGTestWith(
        [this](std::string unrelatedContent) {
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
            for (auto & e : arr) {
                RC_PRE(e.is_number_integer());
            }

            auto handle = slot.fileHandle;

            std::string nixCode =
                "builtins.foldl' (a: b: a + b) 0"
                " (builtins.fromJSON (builtins.readFile " + handle->path.string() + "))";

            // Create unrelated file.
            TempTextFile unrelated(unrelatedContent);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Modify unrelated file.
            unrelated.modify(unrelatedContent + "_changed");
            invalidateFileCache(unrelated.path);

            // Warm eval must be a cache hit.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
