#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// EvalTraceProperty_CatAttrs — catAttrs Soundness and Precision
//
// Expression:
//   builtins.elemAt
//     (builtins.catAttrs catKey
//       [(fromJSON slotA) (fromJSON slotB)])
//     0
//
// Both slotA and slotB are JSON objects with a randomly chosen catKey field.
//
// Soundness: changing catKey's value in slotA invalidates (it's the element at index 0).
// Precision: changing an unrelated key in slotB does not invalidate.
//
// catAttrs does NOT use DerivedContainerBuilder — element SC deps propagate via
// value pointer inheritance from each source.
class EvalTraceProperty_CatAttrs : public TraceCacheFixture {
public:
    EvalTraceProperty_CatAttrs() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-eval-trace-cat-attrs");
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

// Helper: build JSON object ensuring it has the given catKey with the given integer value.
// Other keys from obj are included as-is.
static std::pair<nlohmann::json, std::string> makeJsonWithCatKey(
    const std::map<std::string, JsonValue> & obj,
    const std::string & catKey,
    int catValue)
{
    nlohmann::json json = nlohmann::json::object();
    for (auto & [k, v] : obj)
        json[k] = v.toJson();
    json[catKey] = catValue;
    return {json, json.dump()};
}

// Soundness — changing the catKey's value in slotA invalidates.
TEST_F(EvalTraceProperty_CatAttrs, AccessedKeyChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            // Generate two random JSON objects and a random key name to use as catKey.
            auto objA = *makeJsonObjectGen();
            auto objB = *makeJsonObjectGen();

            // Generate a fresh key name for catKey; use rc::gen::string to get a
            // random non-empty identifier and prefix it to avoid collision with
            // any randomly generated keys in objA/objB.
            auto catKeySuffix = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
            const std::string catKey = "catKey_" + catKeySuffix;

            // Pick random integer values for catKey in each slot.
            int catValueA = *rc::gen::arbitrary<int>();
            int catValueB = *rc::gen::arbitrary<int>();

            auto [jsonA, contentA] = makeJsonWithCatKey(objA, catKey, catValueA);
            auto [jsonB, contentB] = makeJsonWithCatKey(objB, catKey, catValueB);

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

            std::string nixCode =
                "builtins.elemAt"
                " (builtins.catAttrs \"" + catKey + "\""
                " [(builtins.fromJSON (builtins.readFile " + handleA->path.string() + "))"
                "  (builtins.fromJSON (builtins.readFile " + handleB->path.string() + "))])"
                " 0";

            // Change catKey's value in slotA to a different integer.
            int mutatedCatValueA = (catValueA == 999) ? 0 : 999;
            jsonA[catKey] = mutatedCatValueA;
            std::string mutatedA = jsonA.dump();
            RC_PRE(mutatedA != contentA);

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

            // Mutate slotA's catKey value.
            slotA.mutate(mutatedA);
            invalidateFileCache(slotA.path);

            // Warm eval must re-evaluate.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            // Restore.
            slotA.restore();
            invalidateFileCache(slotA.path);
        },
        makeParams);
}

// Precision — changing an unrelated key in slotB does not invalidate.
TEST_F(EvalTraceProperty_CatAttrs, UnrelatedKey_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            // Generate two random JSON objects and random key names.
            auto objA = *makeJsonObjectGen();
            auto objB = *makeJsonObjectGen();

            // Generate catKey and a separate unrelated key (guaranteed distinct by prefix).
            auto catKeySuffix = *rc::gen::nonEmpty(rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
            const std::string catKey = "catKey_" + catKeySuffix;
            const std::string otherKey = "other_" + catKeySuffix;

            // Pick random integer values.
            int catValueA = *rc::gen::arbitrary<int>();
            int catValueB = *rc::gen::arbitrary<int>();
            int otherValue = *rc::gen::arbitrary<int>();

            auto [jsonA, contentA] = makeJsonWithCatKey(objA, catKey, catValueA);
            auto [jsonB, contentB] = makeJsonWithCatKey(objB, catKey, catValueB);

            // Add the unrelated key to slotB.
            jsonB[otherKey] = otherValue;
            contentB = jsonB.dump();

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

            std::string nixCode =
                "builtins.elemAt"
                " (builtins.catAttrs \"" + catKey + "\""
                " [(builtins.fromJSON (builtins.readFile " + handleA->path.string() + "))"
                "  (builtins.fromJSON (builtins.readFile " + handleB->path.string() + "))])"
                " 0";

            // Change the unrelated key's value in slotB.
            int mutatedOtherValue = (otherValue == 999) ? 0 : 999;
            jsonB[otherKey] = mutatedOtherValue;
            std::string mutatedB = jsonB.dump();
            RC_PRE(mutatedB != contentB);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate the unrelated key in slotB.
            slotB.mutate(mutatedB);
            invalidateFileCache(slotB.path);

            // Warm eval must be a cache hit.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Restore.
            slotB.restore();
            invalidateFileCache(slotB.path);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
