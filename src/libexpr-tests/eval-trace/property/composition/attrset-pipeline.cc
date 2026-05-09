// Attrset Pipeline composition property test.
//
// Expression shape:
//   let data     = builtins.fromJSON (builtins.readFile <json>);
//       mapped   = builtins.mapAttrs (k: v: v + 1) data;
//       filtered = builtins.removeAttrs mapped ["unwanted"];
//   in filtered."<key>"
//
// This is a 3-layer composition: readFile → fromJSON → mapAttrs → removeAttrs
// → access.  The dep from readFile must survive through all layers for the
// cache to work correctly.
//
// Two properties are tested:
//
// Soundness: mutating the accessed key's value in the file forces a re-eval.
// Precision: mutating a key that is NOT accessed (but present in the object)
//            does not force a re-eval — the cache still hits.

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ───────────────────────────────────────────────────────────────

class EvalTraceProperty_AttrsetPipeline : public TraceCacheFixture {
public:
    EvalTraceProperty_AttrsetPipeline() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-attrset-pipeline");
    }
};

// maxSuccess = 50: two cold evals per iteration is expensive.
static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 50;
    return params;
}

// ── Soundness ─────────────────────────────────────────────────────────────
//
// Cold eval, warm hit, mutate the accessed key's value in the JSON file,
// invalidateFileCache, warm eval → must miss.
TEST_F(EvalTraceProperty_AttrsetPipeline, AccessedKeyChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeAttrsetPipelineGen();
            // makeAttrsetPipelineGen always produces exactly one JsonFile dep slot.
            RC_PRE(expr.expectsSuccess());

            auto & slot = expr.depSlots[0];

            // Cold eval — records trace.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Warm eval — confirm cache hit.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 0);
            }

            // Mutate the slot (valid JSON, same keys, changed int values).
            auto mutGen = slot.generateMutation();
            auto newValue = *mutGen;
            RC_PRE(newValue != slot.currentValue);
            slot.mutate(newValue);
            invalidateFileCache(slot.path);

            // Warm eval — must miss because the accessed key's value changed.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 1);
            }

            // Restore for the next iteration.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// ── Precision ─────────────────────────────────────────────────────────────
//
// Cold eval, warm hit, mutate a key that is NOT the accessed one, but IS
// present in the JSON object.  invalidateFileCache, warm eval → must still hit.
//
// The accessed key is embedded in expr.nixCode as filtered."<key>".  We parse
// the chosen key from the expression by looking at what key is accessed, then
// mutate a *different* key in the JSON while keeping the accessed key's value
// identical.
TEST_F(EvalTraceProperty_AttrsetPipeline, UnaccedKeyChange_StillHits)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeAttrsetPipelineGen();
            RC_PRE(expr.expectsSuccess());

            auto & slot = expr.depSlots[0];

            // Parse the current JSON to find all keys.
            auto currentJson = nlohmann::json::parse(slot.currentValue, nullptr, false);
            RC_PRE(currentJson.is_object());

            // We need at least 2 non-"unwanted" keys so there is an unaccessed
            // key to mutate.  The generator guarantees this, but guard anyway.
            std::vector<std::string> otherKeys;
            for (auto & [k, _] : currentJson.items()) {
                if (k != "unwanted")
                    otherKeys.push_back(k);
            }
            RC_PRE(otherKeys.size() >= 2);

            // Extract the accessed key from expr.nixCode.  The expression ends
            // with: in filtered."<key>"
            // Find the last '."' and extract the key between the quotes.
            auto nixCode = expr.nixCode;
            auto lastDotQuote = nixCode.rfind(".\"");
            RC_PRE(lastDotQuote != std::string::npos);
            auto keyStart = lastDotQuote + 2;
            auto keyEnd = nixCode.rfind('"');
            RC_PRE(keyEnd > keyStart);
            std::string accessedKey = nixCode.substr(keyStart, keyEnd - keyStart);
            RC_PRE(!accessedKey.empty());

            // Pick a key to mutate that is neither the accessed key nor "unwanted".
            std::string unaccedKey;
            for (auto & k : otherKeys) {
                if (k != accessedKey) {
                    unaccedKey = k;
                    break;
                }
            }
            RC_PRE(!unaccedKey.empty());

            // Cold eval — records trace.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Warm eval — confirm baseline hit.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 0);
            }

            // Build a mutated JSON that changes ONLY unaccedKey's value.
            auto mutated = currentJson;
            auto oldVal = mutated[unaccedKey].get<int64_t>();
            mutated[unaccedKey] = oldVal + 1;
            // Make sure the mutation actually changed the JSON.
            auto mutatedStr = mutated.dump();
            RC_PRE(mutatedStr != slot.currentValue);

            slot.mutate(mutatedStr);
            invalidateFileCache(slot.path);

            // Warm eval — must still hit: accessed key unchanged, SC dep valid.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 0);
            }

            // Restore for the next iteration.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
