#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// Container transformation builtins on traced data.
//
// Tests cover: listToAttrs, groupBy, zipAttrsWith, partition.
// Each test follows the standard cold→warm hit→mutate→warm miss/hit pattern.
class EvalTraceProperty_ContainerTransforms : public TraceCacheFixture {
public:
    EvalTraceProperty_ContainerTransforms() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-container-transforms");
    }
};

// ListToAttrs_Soundness:
// (builtins.listToAttrs (builtins.fromJSON (builtins.readFile f))).a
// JSON: [{"name":"a","value":1},{"name":"b","value":2}]
// Change value of a → miss.
TEST_F(EvalTraceProperty_ContainerTransforms, ListToAttrs_Soundness)
{
    std::string content = R"([{"name":"a","value":1},{"name":"b","value":2}])";
    TempJsonFile file(content);
    std::string nixCode =
        "(builtins.listToAttrs"
        " (builtins.fromJSON (builtins.readFile " + file.path.string() + "))).a";

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
        EXPECT_EQ(n, 0);
    }

    // Change value of a from 1 to 99 → accessed key changes → miss.
    file.modify(R"([{"name":"a","value":99},{"name":"b","value":2}])");
    invalidateFileCache(file.path);

    // Warm eval must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u);
    }
}

// ListToAttrs_Precision:
// (builtins.listToAttrs (builtins.fromJSON (builtins.readFile f))).a
// Change value of b (unaccessed) → hit (SC override: only .a was accessed).
TEST_F(EvalTraceProperty_ContainerTransforms, ListToAttrs_Precision)
{
    std::string content = R"([{"name":"a","value":1},{"name":"b","value":2}])";
    TempJsonFile file(content);
    std::string nixCode =
        "(builtins.listToAttrs"
        " (builtins.fromJSON (builtins.readFile " + file.path.string() + "))).a";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
    }

    // Change value of b (unaccessed by the expression) → should hit.
    file.modify(R"([{"name":"a","value":1},{"name":"b","value":999}])");
    invalidateFileCache(file.path);

    // Warm eval must be a cache hit.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly());
    }
}

// GroupBy_Soundness:
// (builtins.groupBy (x: x.type) (builtins.fromJSON (builtins.readFile f))).x
// JSON: [{"type":"x","val":1},{"type":"y","val":2}]
// Change type of first element from "x" to "z" → .x key disappears → miss.
TEST_F(EvalTraceProperty_ContainerTransforms, GroupBy_Soundness)
{
    std::string content = R"([{"type":"x","val":1},{"type":"y","val":2}])";
    TempJsonFile file(content);
    std::string nixCode =
        "(builtins.groupBy (x: x.type)"
        " (builtins.fromJSON (builtins.readFile " + file.path.string() + "))).x";

    // Cold eval.
    {
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
    }
    // Confirm cache hit (precision pre-condition).
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
        EXPECT_TRUE(snap.primaryCacheServedOnly());
    }

    // Change type of first element from "x" to "z": .x key disappears → miss.
    file.modify(R"([{"type":"z","val":1},{"type":"y","val":2}])");
    invalidateFileCache(file.path);

    // Warm eval must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        // Evaluation may throw because .x no longer exists; catch and count.
        try {
            (void) forceRoot(*cache);
        } catch (...) {
            // The loader was still called (miss), so deltaTraceCacheMisses >= 1.
        }
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u);
    }
}

// ZipAttrsWith_Soundness:
// (builtins.zipAttrsWith (name: vals: builtins.head vals) (builtins.fromJSON (builtins.readFile f))).a
// JSON: [{"a":1,"b":2},{"a":3,"b":4}]
// Change first a's value → miss.
TEST_F(EvalTraceProperty_ContainerTransforms, ZipAttrsWith_Soundness)
{
    std::string content = R"([{"a":1,"b":2},{"a":3,"b":4}])";
    TempJsonFile file(content);
    std::string nixCode =
        "(builtins.zipAttrsWith (name: vals: builtins.head vals)"
        " (builtins.fromJSON (builtins.readFile " + file.path.string() + "))).a";

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
        EXPECT_EQ(n, 0);
    }

    // Change first a's value from 1 to 99 → accessed key value changes → miss.
    file.modify(R"([{"a":99,"b":2},{"a":3,"b":4}])");
    invalidateFileCache(file.path);

    // Warm eval must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u);
    }
}

// Partition_Soundness:
// (builtins.partition (x: x > 0) (builtins.fromJSON (builtins.readFile f))).right
// JSON: [1,-2,3]
// Change -2 to 2 → partition result changes (right grows, wrong shrinks) → miss.
TEST_F(EvalTraceProperty_ContainerTransforms, Partition_Soundness)
{
    std::string content = "[1,-2,3]";
    TempJsonFile file(content);
    std::string nixCode =
        "(builtins.partition (x: x > 0)"
        " (builtins.fromJSON (builtins.readFile " + file.path.string() + "))).right";

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
        EXPECT_EQ(n, 0);
    }

    // Change -2 to 2: partition .right grows from [1,3] to [1,2,3] → miss.
    file.modify("[1,2,3]");
    invalidateFileCache(file.path);

    // Warm eval must re-evaluate.
    {
        PathCountersSnapshot snap;
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
        EXPECT_GE(snap.deltaTraceCacheMisses(), 1u);
    }
}

} // namespace nix::eval_trace::proptest
