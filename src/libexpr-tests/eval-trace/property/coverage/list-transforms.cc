#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// List transformation builtins on traced lists.
//
// These tests exercise builtins that transform lists from fromJSON:
//   genList, concatMap, all, any
//
// Each test follows the standard cold→warm hit→mutate→warm miss/hit pattern.
class EvalTraceProperty_ListTransforms : public TraceCacheFixture {
public:
    EvalTraceProperty_ListTransforms() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-list-transforms");
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

// GenList_Length_Soundness:
// builtins.length (builtins.genList (i: i) (builtins.length (builtins.fromJSON (builtins.readFile f))))
// JSON array length drives genList. Add element → miss.
TEST_F(EvalTraceProperty_ListTransforms, GenList_Length_Soundness)
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
                "builtins.length (builtins.genList (i: i)"
                " (builtins.length (builtins.fromJSON (builtins.readFile "
                + slot.path.string() + "))))";

            // Add an element to increase the array length.
            arr.push_back(42);
            std::string mutated = arr.dump();
            RC_PRE(mutated != slot.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }
            // Confirm cache hit (precision pre-condition).
            {
                int n = 0;
                auto cache = makeCache(nixCode, &n);
                (void) forceRoot(*cache);
                RC_ASSERT(n == 0);
            }

            // Mutate: add element.
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

// ConcatMap_Soundness:
// builtins.length (builtins.concatMap (x: [x x]) (builtins.fromJSON (builtins.readFile f)))
// Add element to array → miss (doubled length changes).
TEST_F(EvalTraceProperty_ListTransforms, ConcatMap_Soundness)
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
                "builtins.length (builtins.concatMap (x: [x x])"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + ")))";

            // Add an element — doubled length changes.
            arr.push_back(99);
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

            // Mutate: add element.
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

// All_Soundness:
// builtins.all (x: x > 0) (builtins.fromJSON (builtins.readFile f))
// Array [1,2,3]; change to [1,-1,3] → miss (result flips from true to false).
TEST_F(EvalTraceProperty_ListTransforms, All_Soundness)
{
    // Use a deterministic array of positive integers.
    std::string content = "[1,2,3]";
    TempJsonFile file(content);
    std::string nixCode =
        "builtins.all (x: x > 0)"
        " (builtins.fromJSON (builtins.readFile " + file.path.string() + "))";

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
        EXPECT_EQ(n, 0);
    }

    // Change [1,2,3] → [1,-1,3]: result flips from true to false.
    file.modify("[1,-1,3]");
    invalidateFileCache(file.path);

    // Warm eval must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u);
    }
}

// All_CorrectlyInvalidates:
// builtins.all (x: x > 0) (builtins.fromJSON (builtins.readFile f))
// Change [1,2,3] → [4,5,6] → correctly invalidates.
// Per-element SC deps cause correct over-invalidation; this is a soundness test, not precision.
// all evaluates the predicate on every element, recording value deps per element.
// Changing element values (even without changing the boolean result) correctly invalidates.
TEST_F(EvalTraceProperty_ListTransforms, All_CorrectlyInvalidates)
{
    std::string content = "[1,2,3]";
    TempJsonFile file(content);
    std::string nixCode =
        "builtins.all (x: x > 0)"
        " (builtins.fromJSON (builtins.readFile " + file.path.string() + "))";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
    }

    // Change values but all are still > 0: result is still true, but
    // per-element SC deps cause the cache to correctly invalidate.
    file.modify("[4,5,6]");
    invalidateFileCache(file.path);

    // Warm eval correctly invalidates: all records per-element SC deps,
    // so any element value change causes a cache miss (correct over-invalidation).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u);
    }
}

// Any_Soundness:
// builtins.any (x: x < 0) (builtins.fromJSON (builtins.readFile f))
// Array [1,2,3]; change to [1,-1,3] → miss (result flips from false to true).
TEST_F(EvalTraceProperty_ListTransforms, Any_Soundness)
{
    std::string content = "[1,2,3]";
    TempJsonFile file(content);
    std::string nixCode =
        "builtins.any (x: x < 0)"
        " (builtins.fromJSON (builtins.readFile " + file.path.string() + "))";

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
        EXPECT_EQ(n, 0);
    }

    // Change [1,2,3] → [1,-1,3]: result flips from false to true.
    file.modify("[1,-1,3]");
    invalidateFileCache(file.path);

    // Warm eval must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u);
    }
}

// Any_CorrectlyInvalidates:
// builtins.any (x: x < 0) (builtins.fromJSON (builtins.readFile f))
// Change [1,2,3] → [4,5,6] → correctly invalidates.
// Per-element SC deps cause correct over-invalidation; this is a soundness test, not precision.
// any evaluates the predicate on elements until true, recording value deps per element.
// Changing element values (even without changing the boolean result) correctly invalidates.
TEST_F(EvalTraceProperty_ListTransforms, Any_CorrectlyInvalidates)
{
    std::string content = "[1,2,3]";
    TempJsonFile file(content);
    std::string nixCode =
        "builtins.any (x: x < 0)"
        " (builtins.fromJSON (builtins.readFile " + file.path.string() + "))";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
    }

    // Change values but none are < 0: result is still false, but
    // per-element SC deps cause the cache to correctly invalidate.
    file.modify("[4,5,6]");
    invalidateFileCache(file.path);

    // Warm eval correctly invalidates: any records per-element SC deps,
    // so any element value change causes a cache miss (correct over-invalidation).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u);
    }
}

} // namespace nix::eval_trace::proptest
