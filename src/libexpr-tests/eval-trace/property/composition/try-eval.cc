// tryEval success-path property tests (RapidCheck).
//
// Models the nixpkgs pattern of using builtins.tryEval for platform-conditional
// packages.  tryEval catches AssertionError (from throw/assert) but NOT
// EvalError (from missing attribute access).
//
// Tests:
//   1. SuccessPath_AccessedKeyChange_Invalidates — soundness via generateMutation()
//   2. SuccessPath_UnaccedKeyChange_StillHits — precision via surgical key mutation
//
// Error-path deterministic tests are in try-eval-error.cc.

#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// ── Fixture ───────────────────────────────────────────────────────────────────

class EvalTraceProperty_TryEval : public TraceCacheFixture {
public:
    EvalTraceProperty_TryEval() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-try-eval");
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

// ── SuccessPath_AccessedKeyChange_Invalidates ─────────────────────────────────
//
// Generated expression shape:
//   (builtins.tryEval ((builtins.fromJSON (builtins.readFile <json>))."key")).value
//
// The key is always present in the generated JSON, so tryEval always succeeds.
// Mutating the JSON file (via generateMutation()) produces new JSON with the
// same keys but changed values, which should invalidate the cache.
TEST_F(EvalTraceProperty_TryEval, SuccessPath_AccessedKeyChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeTryEvalGen();
            // makeTryEvalGen always produces exactly one JsonFile dep slot.
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

            // Mutate the slot (valid JSON, same keys, changed values).
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

// ── SuccessPath_UnaccedKeyChange_StillHits ────────────────────────────────────
//
// Precision: mutating a key that is NOT accessed by the expression (but is
// present in the JSON object) must not invalidate the cached result.
//
// The accessed key is embedded in expr.nixCode as:
//   ...(builtins.fromJSON ...)."<key>")).value
// We extract it by finding the last `."` before `)).value`.
//
// We then find a different int-valued key in the JSON, increment its value,
// and verify the cache still hits.  Only int keys are mutated because the
// SC dep granularity is per key-value; we need to produce a JSON string that
// differs from the original but keeps the accessed key unchanged, which is
// straightforward with integers.
TEST_F(EvalTraceProperty_TryEval, SuccessPath_UnaccedKeyChange_StillHits)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeTryEvalGen();
            RC_PRE(expr.expectsSuccess());

            auto & slot = expr.depSlots[0];

            // Parse the current JSON to find all keys.
            auto currentJson = nlohmann::json::parse(slot.currentValue, nullptr, false);
            RC_PRE(currentJson.is_object());

            // Extract the accessed key from nixCode.
            // The expression ends with: ...)."<key>")).value
            // Find the `."` before `)).value` — the pattern is `."<key>")).value`.
            auto nixCode = expr.nixCode;
            // Locate the marker ")).value" which always appears at the end.
            auto markerPos = nixCode.rfind(")).value");
            RC_PRE(markerPos != std::string::npos);
            // Before the marker: ...)."<key>"
            // Find the last `."` before the marker.
            auto dotQuotePos = nixCode.rfind(".\"", markerPos);
            RC_PRE(dotQuotePos != std::string::npos);
            auto keyStart = dotQuotePos + 2;
            // The closing `"` is at markerPos - 1 (immediately before `)).value`).
            RC_PRE(markerPos >= 1);
            auto keyEnd = markerPos - 1;
            RC_PRE(keyEnd > keyStart);
            std::string accessedKey = nixCode.substr(keyStart, keyEnd - keyStart);
            RC_PRE(!accessedKey.empty());
            RC_PRE(currentJson.contains(accessedKey));

            // Collect unaccessed int keys (we can mutate these safely without
            // changing the accessed key's value).
            std::string unaccedKey;
            for (auto & [k, v] : currentJson.items()) {
                if (k != accessedKey && v.is_number_integer()) {
                    unaccedKey = k;
                    break;
                }
            }
            // Discard if no unaccessed int key is available (object is too small
            // or all other values are non-int).
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
