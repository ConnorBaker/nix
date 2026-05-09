#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// Multi-Source Merge (Triple-source `//` operator) Soundness and Precision
//
// Expression shape (from makeTripleSourceMergeGen):
//   let a = builtins.fromJSON (builtins.readFile <json_a>);
//       b = builtins.fromJSON (builtins.readFile <json_b>);
//       c = builtins.fromJSON (builtins.readFile <json_c>);
//   in (a // b // c)."<key>"
//
// The accessed key exists in EXACTLY ONE source (the "winner").
// DepSlot ordering convention (from makeTripleSourceMergeGen):
//   depSlots[0] — the winning source (provides the accessed key)
//   depSlots[1] — a non-winning source (does NOT have the key)
//   depSlots[2] — a non-winning source (does NOT have the key)
//
// Soundness: mutating the winner's copy of the accessed key must invalidate.
// Precision: mutating a non-winning source must NOT invalidate.

class EvalTraceProperty_MultiSourceMerge : public TraceCacheFixture {
public:
    EvalTraceProperty_MultiSourceMerge() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-multi-source-merge");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 50;
    params.maxDiscardRatio = 100;
    return params;
}

// Soundness: cold eval, confirm warm hit, mutate the SOURCE that provides the
// accessed key (change the key's value), invalidateFileCache, warm eval → miss.
TEST_F(EvalTraceProperty_MultiSourceMerge, AccessedKeyChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeTripleSourceMergeGen();
            RC_PRE(expr.depSlots.size() == 3);

            auto & slotWinner = expr.depSlots[0];
            RC_PRE(slotWinner.kind == DepSlot::Kind::JsonFile);

            // Parse the winner's content to find the accessed key and build a mutation.
            nlohmann::json parsedWinner;
            try {
                parsedWinner = nlohmann::json::parse(slotWinner.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(parsedWinner.is_object());

            // Find the accessed key: the one that appears in nixCode quoted as ."key".
            std::string accessedKey;
            for (auto & [k, _] : parsedWinner.items()) {
                // nixCode contains the key in the form ."<key>" — search for the quoted form.
                if (expr.nixCode.find(".\"" + k + "\"") != std::string::npos) {
                    accessedKey = k;
                    break;
                }
            }
            RC_PRE(!accessedKey.empty());

            // Build a mutation: change the accessed key's value to something different.
            auto & val = parsedWinner[accessedKey];
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
            std::string mutatedContent = parsedWinner.dump();
            RC_PRE(mutatedContent != slotWinner.currentValue);

            // Cold eval — records the trace.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Confirm warm cache hit (precision pre-condition).
            {
                int n = 0;
                auto cache = makeCache(expr.nixCode, &n);
                (void) forceRoot(*cache);
                RC_ASSERT(n == 0);
            }

            // Mutate the winner's accessed key.
            slotWinner.mutate(mutatedContent);
            invalidateFileCache(slotWinner.path);

            // Warm eval must re-evaluate (soundness).
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.deltaTraceCacheMisses() >= 1);
            }

            // Restore the winner to its original state.
            slotWinner.restore();
            invalidateFileCache(slotWinner.path);
        },
        makeParams);
}

// Precision: cold eval, confirm warm hit, mutate a SOURCE that does NOT provide
// the accessed key (change one of its keys), invalidateFileCache, warm eval → hit.
//
// Since the `//` operator with SC deps tracks at key granularity, changing an
// unrelated source should not invalidate.
TEST_F(EvalTraceProperty_MultiSourceMerge, NonWinnerSourceChange_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto expr = *makeTripleSourceMergeGen();
            RC_PRE(expr.depSlots.size() == 3);

            // Use the first non-winning slot (index 1) for the mutation.
            auto & slotNonWinner = expr.depSlots[1];
            RC_PRE(slotNonWinner.kind == DepSlot::Kind::JsonFile);

            // Parse the non-winner's content.
            nlohmann::json parsedNonWinner;
            try {
                parsedNonWinner = nlohmann::json::parse(slotNonWinner.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(parsedNonWinner.is_object());
            // The non-winner must have at least one key to mutate.
            RC_PRE(!parsedNonWinner.empty());

            // Mutate the first key in the non-winner (any key — none are the accessed key).
            auto it = parsedNonWinner.begin();
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
            std::string mutatedContent = parsedNonWinner.dump();
            RC_PRE(mutatedContent != slotNonWinner.currentValue);

            // Cold eval — records the trace.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate the non-winner source.
            slotNonWinner.mutate(mutatedContent);
            invalidateFileCache(slotNonWinner.path);

            // Warm eval must still be a cache hit (precision).
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Restore.
            slotNonWinner.restore();
            invalidateFileCache(slotNonWinner.path);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
