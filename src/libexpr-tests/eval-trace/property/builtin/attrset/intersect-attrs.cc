#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P13 — intersectAttrs Soundness and Precision
//
// Expression: (builtins.intersectAttrs (fromJSON slotA) (fromJSON slotB))."<key>"
// where <key> is in the intersection of both objects.
//
// Soundness: changing key's value in either slotA or slotB invalidates.
// Precision: changing a key present in slotA but NOT in slotB does not invalidate
//   the accessed key (it's not in the intersection).
//
// intersectAttrs(a, b) returns keys from b that are also in a.
// So: result = { k: b[k] | k ∈ dom(a) ∩ dom(b) }
// Dep: #has:k recorded for all keys in either source.
class EvalTraceProperty_IntersectAttrs : public TraceCacheFixture {
public:
    EvalTraceProperty_IntersectAttrs() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-intersect-attrs");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    params.maxDiscardRatio = 200;
    return params;
}

// Helper: build a nixCode for intersectAttrs accessing a shared key.
// Returns empty string if preconditions aren't met (caller uses RC_PRE).
static std::string makeIntersectExpr(
    const std::string & pathA,
    const std::string & pathB,
    const std::string & key)
{
    return "(builtins.intersectAttrs"
           " (builtins.fromJSON (builtins.readFile " + pathA + "))"
           " (builtins.fromJSON (builtins.readFile " + pathB + ")))"
           ".\"" + key + "\"";
}

// P13a: Soundness — changing value of intersection key in slotB invalidates.
// (slotB provides the VALUES; slotA provides the key FILTER)
TEST_F(EvalTraceProperty_IntersectAttrs, ValueChangeInB_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            // Generate two JSON objects with at least one shared key.
            auto objA = *makeJsonObjectGen();
            auto objB = *makeJsonObjectGen();

            // Find a shared key.
            std::string sharedKey;
            for (auto & [k, _] : objA) {
                if (objB.count(k)) {
                    sharedKey = k;
                    break;
                }
            }
            RC_PRE(!sharedKey.empty());

            // Serialize both objects.
            nlohmann::json jsonA = nlohmann::json::object();
            for (auto & [k, v] : objA) jsonA[k] = v.toJson();
            nlohmann::json jsonB = nlohmann::json::object();
            for (auto & [k, v] : objB) jsonB[k] = v.toJson();

            std::string contentA = jsonA.dump();
            std::string contentB = jsonB.dump();

            auto handleA = std::make_shared<TempExtFile>("json", contentA);
            auto handleB = std::make_shared<TempExtFile>("json", contentB);

            DepSlot slotA;
            slotA.kind = DepSlot::Kind::JsonFile;
            slotA.path = handleA->path;
            slotA.fileHandle = handleA;
            slotA.currentValue = contentA;
            slotA.setOriginal(contentA);

            DepSlot slotB;
            slotB.kind = DepSlot::Kind::JsonFile;
            slotB.path = handleB->path;
            slotB.fileHandle = handleB;
            slotB.currentValue = contentB;
            slotB.setOriginal(contentB);

            std::string nixCode = makeIntersectExpr(
                handleA->path.string(), handleB->path.string(), sharedKey);

            // Mutate sharedKey's value in slotB (value source).
            auto & valB = jsonB[sharedKey];
            if (valB.is_number_integer()) {
                auto v = valB.get<int64_t>();
                valB = (v == std::numeric_limits<int64_t>::max()) ? v - 1 : v + 1;
            } else if (valB.is_boolean()) {
                valB = !valB.get<bool>();
            } else if (valB.is_string()) {
                valB = valB.get<std::string>() + "_changed";
            } else {
                RC_DISCARD();
            }
            std::string mutatedB = jsonB.dump();
            RC_PRE(mutatedB != contentB);

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

            // Mutate slotB.
            slotB.mutate(mutatedB);
            invalidateFileCache(slotB.path);

            // Warm eval must re-evaluate.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            // Restore.
            slotB.restore();
            invalidateFileCache(slotB.path);
        },
        makeParams);
}

// P13b: Precision — changing a key only in slotA (not in intersection) does not invalidate.
TEST_F(EvalTraceProperty_IntersectAttrs, NonIntersectionKeyInA_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            // Generate two JSON objects where objA has an extra key not in objB.
            auto objA = *makeJsonObjectGen();
            auto objB = *makeJsonObjectGen();

            // Find a shared key (needed to access intersection result).
            std::string sharedKey;
            for (auto & [k, _] : objA) {
                if (objB.count(k)) {
                    sharedKey = k;
                    break;
                }
            }
            RC_PRE(!sharedKey.empty());

            // Find a key in objA that's NOT in objB.
            std::string onlyInA;
            for (auto & [k, _] : objA) {
                if (!objB.count(k)) {
                    onlyInA = k;
                    break;
                }
            }
            RC_PRE(!onlyInA.empty());

            nlohmann::json jsonA = nlohmann::json::object();
            for (auto & [k, v] : objA) jsonA[k] = v.toJson();
            nlohmann::json jsonB = nlohmann::json::object();
            for (auto & [k, v] : objB) jsonB[k] = v.toJson();

            std::string contentA = jsonA.dump();
            std::string contentB = jsonB.dump();

            auto handleA = std::make_shared<TempExtFile>("json", contentA);
            auto handleB = std::make_shared<TempExtFile>("json", contentB);

            std::string nixCode = makeIntersectExpr(
                handleA->path.string(), handleB->path.string(), sharedKey);

            // Mutate the A-only key's value.
            auto & valA = jsonA[onlyInA];
            if (valA.is_number_integer()) {
                auto v = valA.get<int64_t>();
                valA = (v == std::numeric_limits<int64_t>::max()) ? v - 1 : v + 1;
            } else if (valA.is_boolean()) {
                valA = !valA.get<bool>();
            } else if (valA.is_string()) {
                valA = valA.get<std::string>() + "_x";
            } else {
                RC_DISCARD();
            }
            std::string mutatedA = jsonA.dump();
            RC_PRE(mutatedA != contentA);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate A-only key directly (file is already at contentA from handle creation).
            std::ofstream(handleA->path, std::ios::trunc) << mutatedA;
            invalidateFileCache(handleA->path);

            // Warm eval must be a cache hit.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Restore.
            std::ofstream(handleA->path, std::ios::trunc) << contentA;
            invalidateFileCache(handleA->path);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
