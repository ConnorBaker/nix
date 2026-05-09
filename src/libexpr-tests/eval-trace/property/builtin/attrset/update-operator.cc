#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P12 — Update Operator (`//`) Soundness and Precision
//
// Generator: MultiSourceAttrGen producing ((fromJSON slotA) // (fromJSON slotB))."key"
// where "key" is in slotA only (slotB never shadows it).
//
// Soundness: changing the accessed key in slotA invalidates.
// Precision A: changing an unaccessed key in slotA does not invalidate.
// Precision B: changing any key in slotB does not invalidate (key is in A only).
class EvalTraceProperty_UpdateOperator : public TraceCacheFixture {
public:
    EvalTraceProperty_UpdateOperator() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-update-operator");
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

// P12a: Soundness — changing accessed key in slotA produces correct result.
//
// This test verifies SOUNDNESS, not forced re-evaluation.  The distinction
// matters because the recovery pipeline (tryDirectHashRecovery /
// tryStructuralVariantRecovery) can correctly serve a cached result from a
// prior History entry without calling the loader, as long as the served
// result equals what fresh evaluation would produce.
//
// Specifically: after many RC iterations, the History table accumulates traces
// recorded when slotA had various values.  If a previous iteration happened
// to start with slotA[accessedKey] == mutatedValue, recovery finds that
// historical trace (its full dep hash matches current state) and returns it
// without re-evaluation.  The returned value is correct — it was computed
// when the file had exactly the current content.
//
// Therefore the correct soundness assertion is VALUE EQUALITY, not
// loaderCalls == 1.  The cache serves the right result regardless of whether
// it goes through a loader call or recovery.
TEST_F(EvalTraceProperty_UpdateOperator, AccessedKeyChange_CorrectResult)
{
    rc::detail::checkGTestWith(
        [this]() {
            // *makeMultiSourceAttrGen() inside the property lambda is correct RapidCheck
            // idiom: RC records all *gen() calls and replays them with shrunk random
            // values during counterexample minimization.
            auto expr = *makeMultiSourceAttrGen();
            RC_PRE(expr.depSlots.size() == 2);

            auto & slotA = expr.depSlots[0];
            RC_PRE(slotA.kind == DepSlot::Kind::JsonFile);

            // Parse slotA's content to find the accessed key.
            nlohmann::json parsedA;
            try {
                parsedA = nlohmann::json::parse(slotA.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(parsedA.is_object());

            // Find the accessed key: the one that appears in nixCode.
            std::string accessedKey;
            for (auto & [k, _] : parsedA.items()) {
                if (expr.nixCode.find(k) != std::string::npos) {
                    accessedKey = k;
                    break;
                }
            }
            RC_PRE(!accessedKey.empty());

            // Build mutation: change the accessed key's value.
            auto & val = parsedA[accessedKey];
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
            std::string mutatedContent = parsedA.dump();
            RC_PRE(mutatedContent != slotA.currentValue);

            // Cold eval with original slotA.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Confirm cache hit (precision pre-condition).
            {
                int n = 0;
                auto cache = makeCache(expr.nixCode, &n);
                (void) forceRoot(*cache);
                RC_ASSERT(n == 0);
            }

            // Mutate slotA's accessed key.
            slotA.mutate(mutatedContent);
            invalidateFileCache(slotA.path);

            // Fresh (uncached) evaluation of the mutated expression — this is
            // the ground-truth result the cache must agree with.
            Value freshResult = eval(expr.nixCode);
            state.forceValue(freshResult, noPos);

            // Warm eval after mutation.  May call loader (cache miss) or not
            // (recovery found a matching historical trace).  Either way the
            // served result must equal freshResult.
            Value cachedResult;
            {
                auto cache = makeCache(expr.nixCode);
                cachedResult = forceRoot(*cache);
            }
            assertValuesEqual(freshResult, cachedResult);

            // Restore.
            slotA.restore();
            invalidateFileCache(slotA.path);
        },
        makeParams);
}

// P12b: Precision — changing an unaccessed key in slotA does not invalidate.
TEST_F(EvalTraceProperty_UpdateOperator, UnaccessedKeyInA_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            // *makeMultiSourceAttrGen() inside the property lambda is correct RapidCheck
            // idiom: RC records all *gen() calls and replays them with shrunk random
            // values during counterexample minimization.
            auto expr = *makeMultiSourceAttrGen();
            RC_PRE(expr.depSlots.size() == 2);

            auto & slotA = expr.depSlots[0];
            RC_PRE(slotA.kind == DepSlot::Kind::JsonFile);

            nlohmann::json parsedA;
            try {
                parsedA = nlohmann::json::parse(slotA.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(parsedA.is_object());
            // Need at least 2 keys so there's an unaccessed one.
            RC_PRE(parsedA.size() >= 2);

            // Find an unaccessed key.
            std::string unaccessedKey;
            for (auto & [k, _] : parsedA.items()) {
                if (expr.nixCode.find(k) == std::string::npos) {
                    unaccessedKey = k;
                    break;
                }
            }
            RC_PRE(!unaccessedKey.empty());

            // Mutate the unaccessed key.
            auto & val = parsedA[unaccessedKey];
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
            std::string mutatedContent = parsedA.dump();
            RC_PRE(mutatedContent != slotA.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate unaccessed key.
            slotA.mutate(mutatedContent);
            invalidateFileCache(slotA.path);

            // Warm eval must still be a cache hit.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Restore.
            slotA.restore();
            invalidateFileCache(slotA.path);
        },
        makeParams);
}

// P12c: Precision — changing any key in slotB does not invalidate
// (the accessed key is in slotA only, so slotB changes are irrelevant).
TEST_F(EvalTraceProperty_UpdateOperator, AnyKeyInB_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            // *makeMultiSourceAttrGen() inside the property lambda is correct RapidCheck
            // idiom: RC records all *gen() calls and replays them with shrunk random
            // values during counterexample minimization.
            auto expr = *makeMultiSourceAttrGen();
            RC_PRE(expr.depSlots.size() == 2);

            auto & slotB = expr.depSlots[1];
            RC_PRE(slotB.kind == DepSlot::Kind::JsonFile);

            nlohmann::json parsedB;
            try {
                parsedB = nlohmann::json::parse(slotB.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(parsedB.is_object());
            RC_PRE(!parsedB.empty());

            // Mutate first key in slotB.
            auto it = parsedB.begin();
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
            std::string mutatedContent = parsedB.dump();
            RC_PRE(mutatedContent != slotB.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate slotB.
            slotB.mutate(mutatedContent);
            invalidateFileCache(slotB.path);

            // Warm eval must still be a cache hit.
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
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
