// Overlay-like function application property test.
//
// Expression shape (Pattern 9 — nixpkgs overlay):
//   let base   = builtins.fromJSON (builtins.readFile <base.json>);
//       overlay = self: super: { foo = super.bar + 1; baz = super.baz; };
//       result  = overlay null base;
//   in result.foo
//
// This is a simplified overlay (no fixpoint — self is null).  The result is
// base.bar + 1 (an integer).  The SC dep must track super.bar access through
// the overlay function application to result.foo.
//
// Properties tested:
//
//   BarChange_Invalidates (RC): mutate slot[0] (all values change) → calls == 1.
//   UnrelatedKeyChange_StillHits (RC): mutate only an unrelated key (not bar, not baz)
//                                      → calls == 0.
//   OverlayTransparency_ValueCorrect (deterministic): verify the computed value
//                                      is bar + 1 and that re-eval after bar mutation
//                                      produces the new bar + 1.
//   BazAccess_ThroughOverlay_StillHits (deterministic): change expression to access
//                                      result.baz; baz = super.baz is a passthrough.
//                                      Mutate bar only → the baz-accessing expression
//                                      must NOT invalidate (baz dep ≠ bar dep).

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

class EvalTraceProperty_Overlay : public TraceCacheFixture {
public:
    EvalTraceProperty_Overlay() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-overlay");
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

// ── Test 1: BarChange_Invalidates ─────────────────────────────────────────
//
// RapidCheck property: mutating slot[0] via generateMutation() (all values
// change — bar, baz, and any extra keys) must cause a cache miss (calls == 1).
// The generateMutation() for Kind::JsonFile preserves key names and value types
// but changes values, so bar's value always changes.

TEST_F(EvalTraceProperty_Overlay, BarChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeOverlayGen();
            RC_PRE(expr.expectsSuccess());

            auto & slot = expr.depSlots[0];

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

            // Mutate all values in the slot (bar changes along with everything else).
            auto mutGen = slot.generateMutation();
            auto newValue = *mutGen;
            RC_PRE(newValue != slot.currentValue);
            slot.mutate(newValue);
            invalidateFileCache(slot.path);

            // Warm eval — must miss because bar's value changed.
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

// ── Test 2: UnrelatedKeyChange_StillHits ──────────────────────────────────
//
// RapidCheck property: find a key in the JSON that is neither "bar" nor "baz"
// (RC_PRE: JSON has 3+ keys so such a key exists), mutate only that key, and
// assert the cache still hits (calls == 0).

TEST_F(EvalTraceProperty_Overlay, UnrelatedKeyChange_StillHits)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeOverlayGen();
            RC_PRE(expr.expectsSuccess());

            auto & slot = expr.depSlots[0];

            // Require at least 3 keys so there is a key that is neither bar nor baz.
            auto currentJson = nlohmann::json::parse(slot.currentValue, nullptr, false);
            RC_PRE(currentJson.is_object());
            RC_PRE(currentJson.size() >= 3);

            // Find a key that is neither "bar" nor "baz".
            std::string unrelatedKey;
            for (auto & [k, _] : currentJson.items()) {
                if (k != "bar" && k != "baz") {
                    unrelatedKey = k;
                    break;
                }
            }
            RC_PRE(!unrelatedKey.empty());

            // The unrelated key must be an integer so we can mutate it cleanly.
            RC_PRE(currentJson[unrelatedKey].is_number_integer());

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

            // Build a mutated JSON that changes ONLY the unrelated key.
            auto mutated = currentJson;
            auto oldVal = mutated[unrelatedKey].get<int64_t>();
            mutated[unrelatedKey] = oldVal + 1;
            auto mutatedStr = mutated.dump();
            RC_PRE(mutatedStr != slot.currentValue);

            slot.mutate(mutatedStr);
            invalidateFileCache(slot.path);

            // Warm eval — must still hit: bar is unchanged, SC dep valid.
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

// ── Test 3: OverlayTransparency_ValueCorrect ──────────────────────────────
//
// Deterministic test: verify the computed result is bar + 1.
// base = {"bar": 10, "baz": 20} → result.foo == 11.
// Mutate bar to 50 → invalidate → calls == 1, result.foo == 51.

TEST_F(EvalTraceProperty_Overlay, OverlayTransparency_ValueCorrect)
{
    // Build the initial JSON: {"bar": 10, "baz": 20}.
    nlohmann::json baseJson = {{"bar", 10}, {"baz", 20}};
    std::string initialContent = baseJson.dump();

    TempExtFile jsonFile("json", initialContent);
    std::string nixCode =
        "let base = builtins.fromJSON (builtins.readFile " + jsonFile.path.string() + ");"
        " overlay = self: super: { foo = super.bar + 1; baz = super.baz; };"
        " result = overlay null base;"
        " in result.foo";

    // Cold eval — records trace, result.foo == 11.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 11);
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(v.integer().value, 11);
    }

    // Mutate bar from 10 to 50.
    nlohmann::json mutatedJson = {{"bar", 50}, {"baz", 20}};
    {
        std::ofstream ofs(jsonFile.path, std::ios::trunc);
        ofs << mutatedJson.dump();
    }
    invalidateFileCache(jsonFile.path);

    // Warm eval — must miss (bar changed), result.foo == 51.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(v.integer().value, 51);
    }
}

// ── Test 4: BazAccess_ThroughOverlay_StillHits ────────────────────────────
//
// Deterministic test: access result.baz instead of result.foo.
// baz = super.baz is a passthrough — it depends on base.baz, not base.bar.
// Mutate bar only → the baz-accessing expression must NOT invalidate (calls == 0).

TEST_F(EvalTraceProperty_Overlay, BazAccess_ThroughOverlay_StillHits)
{
    // Build the initial JSON: {"bar": 10, "baz": 20}.
    nlohmann::json baseJson = {{"bar", 10}, {"baz", 20}};
    std::string initialContent = baseJson.dump();

    TempExtFile jsonFile("json", initialContent);

    // Expression accesses result.baz (not result.foo).
    std::string nixCode =
        "let base = builtins.fromJSON (builtins.readFile " + jsonFile.path.string() + ");"
        " overlay = self: super: { foo = super.bar + 1; baz = super.baz; };"
        " result = overlay null base;"
        " in result.baz";

    // Cold eval — records trace, result.baz == 20.
    {
        auto cache = makeCache(nixCode);
        auto v = forceRoot(*cache);
        EXPECT_EQ(v.type(), nInt);
        EXPECT_EQ(v.integer().value, 20);
    }

    // Warm eval — confirm cache hit.
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(v.integer().value, 20);
    }

    // Mutate bar only (not baz).
    nlohmann::json mutatedJson = {{"bar", 99}, {"baz", 20}};
    {
        std::ofstream ofs(jsonFile.path, std::ios::trunc);
        ofs << mutatedJson.dump();
    }
    invalidateFileCache(jsonFile.path);

    // Warm eval — must still hit: result.baz depends on base.baz, not base.bar.
    // baz = super.baz = base.baz = 20 (unchanged).
    {
        int calls = 0;
        auto cache = makeCache(nixCode, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0);
    }
}

} // namespace nix::eval_trace::proptest
