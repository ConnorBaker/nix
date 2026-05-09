#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P14 — removeAttrs Soundness and Precision
//
// Expression: (builtins.removeAttrs (fromJSON slot) ["<otherKey>"])."<accessedKey>"
// where accessedKey ≠ otherKey.
//
// Soundness: changing accessedKey's value invalidates.
// Precision A: changing otherKey's value (the removed key) does not invalidate.
// Precision B: removing a key that is never accessed does not invalidate.
class EvalTraceProperty_RemoveAttrs : public TraceCacheFixture {
public:
    EvalTraceProperty_RemoveAttrs() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-remove-attrs");
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

// P14a: Soundness — changing accessed key's value invalidates.
TEST_F(EvalTraceProperty_RemoveAttrs, AccessedKeyChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto obj = *makeJsonObjectGen();
            // Need at least 2 keys so we can remove one and access another.
            RC_PRE(obj.size() >= 2);

            auto it = obj.begin();
            std::string removedKey = it->first; ++it;
            std::string accessedKey = it->first;

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
                "(builtins.removeAttrs"
                " (builtins.fromJSON (builtins.readFile " + handle->path.string() + "))"
                " [\"" + removedKey + "\"])"
                ".\"" + accessedKey + "\"";

            // Mutate accessed key's value.
            auto & val = json[accessedKey];
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

// P14b: Precision — changing the removed key's value does not invalidate.
TEST_F(EvalTraceProperty_RemoveAttrs, RemovedKeyChange_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto obj = *makeJsonObjectGen();
            RC_PRE(obj.size() >= 2);

            auto it = obj.begin();
            std::string removedKey = it->first; ++it;
            std::string accessedKey = it->first;

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
                "(builtins.removeAttrs"
                " (builtins.fromJSON (builtins.readFile " + handle->path.string() + "))"
                " [\"" + removedKey + "\"])"
                ".\"" + accessedKey + "\"";

            // Mutate removed key's value.
            auto & val = json[removedKey];
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

            // Mutate removed key.
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

} // namespace nix::eval_trace::proptest
