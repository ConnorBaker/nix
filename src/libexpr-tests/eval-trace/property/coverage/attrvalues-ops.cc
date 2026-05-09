#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// builtins.attrValues tests (separate from attrNames in P16).
//
// P16 in attr-names.cc already tests attrValues length (adding a key → miss).
// These tests focus on:
//   a) Accessing the head value — soundness when the first attr's value changes.
//   b) Length precision — changing a value (no key add/remove) is a cache hit.
//
// Uses deterministic TempJsonFile to ensure consistent key ordering.
class EvalTraceProperty_AttrValuesOps : public TraceCacheFixture {
public:
    EvalTraceProperty_AttrValuesOps() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-attrvalues-ops");
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

// AttrValues_Soundness:
// builtins.head (builtins.attrValues (builtins.fromJSON (builtins.readFile f)))
// Change first attr's value → miss (accessed value changes).
TEST_F(EvalTraceProperty_AttrValuesOps, AttrValues_Soundness)
{
    rc::detail::checkGTestWith(
        [this]() {
            // Build a JSON object with at least one key using the generator.
            auto obj = *makeJsonObjectGen();
            RC_PRE(!obj.empty());

            nlohmann::json json = nlohmann::json::object();
            for (auto & [k, v] : obj) json[k] = v.toJson();
            std::string content = json.dump();

            auto handle = std::make_shared<TempExtFile>("json", content);
            DepSlot slot;
            slot.kind = DepSlot::Kind::JsonFile;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = content;
            slot.setOriginal(content);

            std::string nixCode =
                "builtins.head (builtins.attrValues"
                " (builtins.fromJSON (builtins.readFile " + handle->path.string() + ")))";

            // Mutate first key's value.
            auto it = json.begin();
            auto & val = it.value();
            if (val.is_number_integer()) {
                auto v = val.get<int64_t>();
                val = (v == std::numeric_limits<int64_t>::max()) ? v - 1 : v + 1;
            } else if (val.is_boolean()) {
                val = !val.get<bool>();
            } else if (val.is_string()) {
                val = val.get<std::string>() + "_changed";
            } else {
                RC_DISCARD();
            }
            std::string mutated = json.dump();
            RC_PRE(mutated != content);

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

            // Mutate: change first value.
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

// AttrValues_Length_Precision:
// builtins.length (builtins.attrValues (builtins.fromJSON (builtins.readFile f)))
// Change value (no key add/remove) → hit (#keys dep unchanged).
TEST_F(EvalTraceProperty_AttrValuesOps, AttrValues_Length_Precision)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto obj = *makeJsonObjectGen();
            RC_PRE(!obj.empty());

            nlohmann::json json = nlohmann::json::object();
            for (auto & [k, v] : obj) json[k] = v.toJson();
            std::string content = json.dump();

            auto handle = std::make_shared<TempExtFile>("json", content);
            DepSlot slot;
            slot.kind = DepSlot::Kind::JsonFile;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = content;
            slot.setOriginal(content);

            std::string nixCode =
                "builtins.length (builtins.attrValues"
                " (builtins.fromJSON (builtins.readFile " + handle->path.string() + ")))";

            // Change first key's value without adding or removing keys.
            auto it = json.begin();
            auto & val = it.value();
            if (val.is_number_integer()) {
                auto v = val.get<int64_t>();
                val = (v == std::numeric_limits<int64_t>::max()) ? v - 1 : v + 1;
            } else if (val.is_boolean()) {
                val = !val.get<bool>();
            } else if (val.is_string()) {
                val = val.get<std::string>() + "_x";
            } else {
                RC_DISCARD();
            }
            std::string mutated = json.dump();
            RC_PRE(mutated != content);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate value only (no key change).
            slot.mutate(mutated);
            invalidateFileCache(slot.path);

            // Warm eval must be a cache hit (length = number of keys, unchanged).
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
