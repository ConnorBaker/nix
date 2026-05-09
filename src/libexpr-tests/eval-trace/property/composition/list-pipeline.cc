#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// List Pipeline composition property tests.
//
// Expression shape (from makeListPipelineGen):
//   let list   = builtins.fromJSON (builtins.readFile <path>);
//       mapped = builtins.map (x: x * 2) list;
//   in builtins.length mapped
//
// readFile → fromJSON → map → length.  The dep from readFile must propagate
// through map and be observable via length.
//
// Soundness: changing the array length (adding an element) must invalidate the
//   cache, because length mapped changes.
//
// Precision: changing an element's VALUE while keeping the array length the same
//   must NOT invalidate the cache — length mapped is unchanged, so the #len dep
//   stored by length still matches.
//
// Only 2 layers (map→length, not filter→length or sort→length) because filter
// creates per-element SC deps that make precision assertions false.  map→length
// is sufficient to test dep propagation without that problem.

class EvalTraceProperty_ListPipeline : public TraceCacheFixture {
public:
    EvalTraceProperty_ListPipeline() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-list-pipeline");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 50;
    params.maxDiscardRatio = 100;
    return params;
}

// Soundness: add one element to the JSON array → length increases → must miss.
TEST_F(EvalTraceProperty_ListPipeline, AddElement_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeListPipelineGen();
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

            // Build a mutated array with one extra element.
            nlohmann::json mutatedArr = arr;
            mutatedArr.push_back(42);
            std::string mutated = mutatedArr.dump();
            RC_PRE(mutated != slot.currentValue);

            // Cold eval — records trace.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Warm eval — confirm cache hit (precision pre-condition).
            {
                int n = 0;
                auto cache = makeCache(expr.nixCode, &n);
                (void) forceRoot(*cache);
                RC_ASSERT(n == 0);
            }

            // Mutate: add an element to the array.
            slot.mutate(mutated);
            invalidateFileCache(slot.path);

            // Warm eval must re-evaluate (length changed).
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            // Restore for next iteration.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// Precision: change an element's VALUE (same array length) → length unchanged → cache hit.
TEST_F(EvalTraceProperty_ListPipeline, ElementValueChange_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeListPipelineGen();
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

            // Build a mutated array with same length but different values.
            // Change every element by adding 1 (wrapping at 100 to stay in range).
            nlohmann::json mutatedArr = arr;
            for (auto & elem : mutatedArr) {
                auto v = elem.get<int>();
                elem = (v >= 100) ? v - 1 : v + 1;
            }
            std::string mutated = mutatedArr.dump();
            RC_PRE(mutated != slot.currentValue);

            // Cold eval — records trace.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate: change element values (length unchanged).
            slot.mutate(mutated);
            invalidateFileCache(slot.path);

            // Warm eval must be a cache hit (length unchanged).
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Restore for next iteration.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
