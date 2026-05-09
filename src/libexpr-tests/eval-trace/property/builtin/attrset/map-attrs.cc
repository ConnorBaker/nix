#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P15 — mapAttrs Soundness and Precision
//
// Expression: (builtins.mapAttrs (k: v: v) (fromJSON slot))."<key>"
// (identity map for simplicity — output value matches input value exactly)
//
// Soundness: changing accessed key's value invalidates.
// Precision: changing an unaccessed key's value does not invalidate.
class EvalTraceProperty_MapAttrs : public TraceCacheFixture {
public:
    EvalTraceProperty_MapAttrs() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-map-attrs");
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

// P15a: Soundness — changing accessed key's value invalidates.
TEST_F(EvalTraceProperty_MapAttrs, AccessedKeyChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            // *makeAttrAccessGen() inside the property lambda is correct RapidCheck idiom:
            // RC records all *gen() calls and replays them with shrunk random values
            // during counterexample minimization.
            auto expr = *makeAttrAccessGen();
            RC_PRE(!expr.depSlots.empty());
            auto & slot = expr.depSlots[0];
            RC_PRE(slot.kind == DepSlot::Kind::JsonFile);

            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(slot.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(parsed.is_object());

            // Find the accessed key.
            std::string accessedKey;
            for (auto & [k, _] : parsed.items()) {
                if (expr.nixCode.find(k) != std::string::npos) {
                    accessedKey = k;
                    break;
                }
            }
            RC_PRE(!accessedKey.empty());

            // Build mapAttrs expression using the same slot.
            std::string nixCode =
                "(builtins.mapAttrs (k: v: v)"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + ")))"
                ".\"" + accessedKey + "\"";

            // Mutate the accessed key's value.
            auto & val = parsed[accessedKey];
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
            std::string mutated = parsed.dump();
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

// P15b: Precision — changing an unaccessed key's value does not invalidate.
TEST_F(EvalTraceProperty_MapAttrs, UnaccessedKeyChange_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            // *makeAttrAccessGen() inside the property lambda is correct RapidCheck idiom:
            // RC records all *gen() calls and replays them with shrunk random values
            // during counterexample minimization.
            auto expr = *makeAttrAccessGen();
            RC_PRE(!expr.depSlots.empty());
            auto & slot = expr.depSlots[0];
            RC_PRE(slot.kind == DepSlot::Kind::JsonFile);

            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(slot.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(parsed.is_object());
            RC_PRE(parsed.size() >= 2);

            // Find accessed key and an unaccessed key.
            std::string accessedKey;
            std::string unaccessedKey;
            for (auto & [k, _] : parsed.items()) {
                if (expr.nixCode.find(k) != std::string::npos)
                    accessedKey = k;
                else if (unaccessedKey.empty())
                    unaccessedKey = k;
            }
            RC_PRE(!accessedKey.empty());
            RC_PRE(!unaccessedKey.empty());

            std::string nixCode =
                "(builtins.mapAttrs (k: v: v)"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + ")))"
                ".\"" + accessedKey + "\"";

            // Mutate unaccessed key.
            auto & val = parsed[unaccessedKey];
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
            std::string mutated = parsed.dump();
            RC_PRE(mutated != slot.currentValue);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate unaccessed key.
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
