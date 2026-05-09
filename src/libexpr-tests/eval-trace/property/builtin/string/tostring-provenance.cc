#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P30 — toString Provenance Preservation
//
// Expression: builtins.toString (fromJSON slot)."<key>"
// where <key> has a string or integer value.
//
// Soundness: changing the key's value in the JSON file invalidates.
//
// The coercion boundary (coerceToContextObject → publishContextObject) must
// preserve the TextObject provenance so the resulting string carries the SC dep.
class EvalTraceProperty_ToStringProvenance : public TraceCacheFixture {
public:
    EvalTraceProperty_ToStringProvenance() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-tostring-provenance");
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

// P30a: Soundness — changing the accessed key's value invalidates after toString.
TEST_F(EvalTraceProperty_ToStringProvenance, AccessedKeyChange_Invalidates)
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

            // Find the accessed key (embedded in nixCode as ."<key>").
            std::string accessedKey;
            for (auto & [k, _] : parsed.items()) {
                if (expr.nixCode.find(k) != std::string::npos) {
                    accessedKey = k;
                    break;
                }
            }
            RC_PRE(!accessedKey.empty());

            // Only string and integer values are toString-safe without errors.
            auto & val = parsed[accessedKey];
            RC_PRE(val.is_string() || val.is_number_integer() || val.is_boolean());

            // Build toString expression.
            std::string nixCode =
                "builtins.toString"
                " (builtins.fromJSON (builtins.readFile " + slot.path.string() + "))"
                ".\"" + accessedKey + "\"";

            // Mutate the accessed key's value.
            if (val.is_number_integer()) {
                auto v = val.get<int64_t>();
                val = (v == std::numeric_limits<int64_t>::max()) ? v - 1 : v + 1;
            } else if (val.is_boolean()) {
                val = !val.get<bool>();
            } else if (val.is_string()) {
                val = val.get<std::string>() + "_changed";
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

} // namespace nix::eval_trace::proptest
