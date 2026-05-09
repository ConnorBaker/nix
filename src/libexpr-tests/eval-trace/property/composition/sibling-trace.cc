// Pattern 5: Sibling TracedExpr with Shared Dep Source
//
// Models a mkDerivation-style attrset where multiple attributes share a common
// JSON source:
//
//   let cfg = builtins.fromJSON (builtins.readFile <config.json>);
//   in { name = cfg.name; version = cfg.version;
//        combined = "${cfg.name}-${cfg.version}"; }.combined
//
// Accessing .combined forces both cfg.name and cfg.version, creating two
// StructuredProjection deps on the same source file.  The tests below verify:
//
//   1. AccessedKey_Mutation_Invalidates    (RapidCheck, maxSuccess=50)
//      Mutate the JSON file (changing values) → the cache must miss.
//
//   2. UnrelatedKey_Mutation_StillHits     (RapidCheck, maxSuccess=50)
//      Parse the JSON, find a key that is neither "name" nor "version", mutate
//      only that key → the cache must still hit (SC precision).
//      RC_PRE: at least 3 keys exist so an unrelated key is guaranteed.
//
//   3. CrossSession_WarmHit                (deterministic)
//      Cold eval, simulateWarmRestart(), warm eval → cache must hit.
//
//   4. SiblingIndependence_Documented      (deterministic)
//      config = {"name":"hello","version":"1.0","extra":"x"}.
//      Cold → warm hit → change "extra" → invalidate → warm.
//      Document the observed behavior: calls==0 means SC precision works;
//      calls==1 means over-approximation.

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

class EvalTraceProperty_SiblingTrace : public TraceCacheFixture {
public:
    EvalTraceProperty_SiblingTrace() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-sibling-trace");
    }
};

// maxSuccess = 50: each iteration runs two evals (cold + warm).
static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 50;
    return params;
}

// ── Test 1: Mutating the JSON source invalidates the cache ────────────────
//
// The expression accesses cfg.name and cfg.version from the JSON file.
// generateMutation() for Kind::JsonFile produces a new valid JSON object with
// the same keys but different values.  After the mutation, the StructuredProjection
// dep hashes for "name" and "version" will no longer match the stored values,
// so the verifier must fail and the loader must be called (loaderCalls == 1).
TEST_F(EvalTraceProperty_SiblingTrace, AccessedKey_Mutation_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeSiblingTraceGen();
            RC_PRE(expr.expectsSuccess());

            auto & slot = expr.depSlots[0];

            // Generate a mutation that produces a different JSON content.
            auto newValue = *slot.generateMutation();
            RC_PRE(newValue != slot.currentValue);

            // Cold eval — records trace.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Warm eval — confirm baseline cache hit.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 0);
            }

            // Mutate the JSON file and invalidate the file cache.
            slot.mutate(newValue);
            invalidateFileCache(slot.path);

            // Warm eval — must miss because the accessed keys' values changed.
            int calls = 0;
            {
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
            }
            RC_ASSERT(calls == 1);

            // Restore for the next iteration.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// ── Test 2: Mutating an unrelated key still hits the cache ────────────────
//
// The expression only accesses cfg.name and cfg.version.  Any other key in the
// JSON is unrelated.  Changing an unrelated key's value must not affect the SC
// dep hashes for "name" and "version", so the cache must still hit.
//
// RC_PRE: the JSON must contain at least 3 keys (so at least one key besides
// "name" and "version" exists).
TEST_F(EvalTraceProperty_SiblingTrace, UnrelatedKey_Mutation_StillHits)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeSiblingTraceGen();
            RC_PRE(expr.expectsSuccess());

            auto & slot = expr.depSlots[0];

            // Parse the current JSON content.
            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(slot.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(parsed.is_object());

            // Need at least 3 keys: "name", "version", and at least one other.
            RC_PRE(parsed.size() >= 3);

            // Find a key that is neither "name" nor "version".
            std::string unrelatedKey;
            for (auto & [k, _] : parsed.items()) {
                if (k != "name" && k != "version") {
                    unrelatedKey = k;
                    break;
                }
            }
            RC_PRE(!unrelatedKey.empty());

            // Cold eval — records trace.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Warm eval — confirm baseline cache hit.
            {
                int calls = 0;
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
                RC_ASSERT(calls == 0);
            }

            // Mutate only the unrelated key, leaving "name" and "version" unchanged.
            // Mutation is type-preserving: int → int+1, string → string+"_x".
            auto & uval = parsed[unrelatedKey];
            if (uval.is_number_integer()) {
                uval = uval.get<int64_t>() + 1;
            } else if (uval.is_string()) {
                uval = uval.get<std::string>() + "_x";
            } else {
                // null / bool / other: discard — mutation not meaningful here.
                RC_DISCARD();
            }

            std::string mutated = parsed.dump();
            RC_PRE(mutated != slot.currentValue);

            slot.mutate(mutated);
            invalidateFileCache(slot.path);

            // Warm eval — must still hit: "name" and "version" unchanged, SC deps valid.
            int calls = 0;
            {
                auto cache = makeCache(expr.nixCode, &calls);
                (void) forceRoot(*cache);
            }
            RC_ASSERT(calls == 0);

            // Restore for the next iteration.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

// ── Test 3: Cross-session warm hit ───────────────────────────────────────
//
// Cold eval records the trace.  simulateWarmRestart() simulates a new evaluation
// session (releases the active backend, clears the FS
// accessor cache).  A subsequent warm eval must hit the cache — the trace
// recorded in the previous session must survive the session boundary.
TEST_F(EvalTraceProperty_SiblingTrace, CrossSession_WarmHit)
{
    // A fixed, deterministic expression so the test is reproducible.
    TempJsonFile cfg(R"({"name":"hello","version":"1.0","extra":"x"})");

    std::string nixCode =
        "let cfg = builtins.fromJSON (builtins.readFile " + cfg.path.string() + ");"
        " in { name = cfg.name; version = cfg.version;"
        " combined = \"${cfg.name}-${cfg.version}\"; }.combined";

    // Cold eval — records trace.
    {
        auto cache = makeCache(nixCode);
        (void) forceRoot(*cache);
    }

    // Simulate a new session (flushes SQLite, clears in-memory caches).
    simulateWarmRestart();

    // Warm eval in the new session — must hit the cache.
    int calls = 0;
    {
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
    }
    EXPECT_EQ(calls, 0);
}

// ── Test 4: SiblingIndependence — documented behavior ────────────────────
//
// The "extra" key is not accessed by the expression.  Whether changing it
// causes a cache miss depends on the SC dep precision of the implementation.
//
// Two outcomes are valid:
//   calls == 0: SC precision works — the StructuredProjection deps cover only
//               "name" and "version", so "extra" changing does not invalidate.
//   calls == 1: Over-approximation — the implementation records a broader dep
//               (e.g., whole-file content hash) that covers all keys.
//
// The test documents whichever outcome occurs, without asserting a specific
// value.  It does assert that the expression evaluates without error (the
// combined string still evaluates correctly after "extra" changes).
TEST_F(EvalTraceProperty_SiblingTrace, SiblingIndependence_Documented)
{
    TempJsonFile cfg(R"({"name":"hello","version":"1.0","extra":"x"})");

    std::string nixCode =
        "let cfg = builtins.fromJSON (builtins.readFile " + cfg.path.string() + ");"
        " in { name = cfg.name; version = cfg.version;"
        " combined = \"${cfg.name}-${cfg.version}\"; }.combined";

    // Cold eval — records trace.
    {
        auto cache = makeCache(nixCode);
        auto val = forceRoot(*cache);
        ASSERT_EQ(val.type(), nString);
        EXPECT_EQ(std::string_view(val.string_view()), "hello-1.0");
    }

    // Warm eval — confirm baseline cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "baseline warm hit failed before mutation";
    }

    // Change only "extra" — "name" and "version" remain "hello" and "1.0".
    cfg.modify(R"({"name":"hello","version":"1.0","extra":"y"})");
    invalidateFileCache(cfg.path);

    // Warm eval after "extra" mutation.  Document (don't assert) the outcome.
    int calls = 0;
    Value warmVal;
    {
        auto cache = makeCache(nixCode, &calls);
        warmVal = forceRoot(*cache);
    }

    // Regardless of cache hit or miss, the result must be "hello-1.0".
    EXPECT_EQ(warmVal.type(), nString);
    EXPECT_EQ(std::string_view(warmVal.string_view()), "hello-1.0");

    // Document the SC precision behavior.
    //   calls == 0: SC precision works; StructuredProjection deps cover only
    //               "name" and "version" → "extra" change does not invalidate.
    //   calls == 1: Over-approximation; impl records a broader dep that covers
    //               all JSON keys or the entire file content.
    RecordProperty("extra_mutation_loaderCalls", calls);
}

} // namespace nix::eval_trace::proptest
