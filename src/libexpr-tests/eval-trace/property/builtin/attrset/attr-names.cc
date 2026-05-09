#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P16 — attrNames / attrValues Soundness and Precision
//
// Expression: builtins.length (builtins.attrNames (fromJSON slot))
//
// Soundness: adding a key to the JSON object (changes #keys) → invalidates.
// Precision: changing a value without adding/removing keys → cache hit.
//   The #keys dep records only the count of keys, not their values.
//
// Note: We test the LENGTH of attrNames to get a scalar result for comparison.
// attrNames itself returns a list; forcing the list length exercises the #keys dep.
class EvalTraceProperty_AttrNames : public TraceCacheFixture {
public:
    EvalTraceProperty_AttrNames() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-attr-names");
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

// P16a: Soundness — adding a key to the JSON object invalidates (changes #keys count).
TEST_F(EvalTraceProperty_AttrNames, AddKey_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto obj = *makeJsonObjectGen();
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
                "builtins.length (builtins.attrNames"
                " (builtins.fromJSON (builtins.readFile " + handle->path.string() + ")))";

            // Add a new key that doesn't already exist.
            std::string newKey = "_newKey_p16";
            RC_PRE(!json.contains(newKey));
            json[newKey] = 42;
            std::string mutated = json.dump();
            RC_PRE(mutated != content);

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

            // Mutate: add a key.
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

// P16b: Precision — changing a value without adding/removing keys → cache hit.
TEST_F(EvalTraceProperty_AttrNames, ValueChangeOnly_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto obj = *makeJsonObjectGen();
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
                "builtins.length (builtins.attrNames"
                " (builtins.fromJSON (builtins.readFile " + handle->path.string() + ")))";

            // Change first key's value (not add/remove keys).
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

            // Mutate value only.
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

// P16c: attrValues length — same structural dep as attrNames.
TEST_F(EvalTraceProperty_AttrNames, AttrValues_AddKey_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto obj = *makeJsonObjectGen();
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

            // Add a key.
            std::string newKey = "_newKey_p16c";
            RC_PRE(!json.contains(newKey));
            json[newKey] = 99;
            std::string mutated = json.dump();
            RC_PRE(mutated != content);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }
            // Confirm hit.
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
