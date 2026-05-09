#include "eval-trace/helpers.hh"
#include "../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

class EvalTraceProperty_StructuralOverridePrecision : public TraceCacheFixture {
public:
    EvalTraceProperty_StructuralOverridePrecision() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-structural-override-precision");
    }
};

static rc::detail::TestParams makeParams()
{
    auto const & conf = rc::detail::configuration();
    auto params = conf.testParams;
    params.maxSuccess = 100;
    params.maxDiscardRatio = 100;  // many iterations will be discarded (single-key objects, non-file slots)
    return params;
}

// Changing an unaccessed key in a JSON file produces a cache hit.
//
// The SC (StructuredContent) dep override path records only the specific
// key(s) accessed from a fromJSON attrset.  Changing a key that was never
// accessed must not cause re-evaluation — the verifier should pass the
// existing SC deps and serve from cache.
//
// Note: This test is most valuable after W2 lands (when makeAttrAccessGen
// uses Kind::JsonFile), but is written now against Kind::File slots so it
// exercises the same end-to-end cache path today.  The test logic is
// identical regardless of Kind — what matters is that the JSON file contains
// at least two keys and only one appears in nixCode.
TEST_F(EvalTraceProperty_StructuralOverridePrecision, UnaccessedKeyChange)
{
    rc::detail::checkGTestWith(
        [this]() {
            // *makeAttrAccessGen() inside the property lambda is correct RapidCheck idiom:
            // RC records all *gen() calls and replays them with shrunk random values
            // during counterexample minimization.
            auto expr = *makeAttrAccessGen();

            // Guard: need at least one dep slot.
            RC_PRE(!expr.depSlots.empty());

            auto & slot = expr.depSlots[0];

            // Guard: slot must be file-backed (File or JsonFile kinds).
            // EnvVar and FileExistence slots do not hold JSON content.
            RC_PRE(slot.kind == DepSlot::Kind::File
                || slot.kind == DepSlot::Kind::JsonFile);

            // Guard: slot content must parse as a JSON object.
            nlohmann::json parsed;
            try {
                parsed = nlohmann::json::parse(slot.currentValue);
            } catch (...) {
                RC_DISCARD();
            }
            RC_PRE(parsed.is_object());

            // Guard: JSON object must have at least 2 keys so there is at
            // least one key that was NOT accessed by nixCode.
            RC_PRE(parsed.size() >= 2);

            // Find an unaccessed key: one whose name does not appear anywhere
            // in the generated nixCode.  makeAttrAccessGen embeds exactly one
            // key access as ."<chosenKey>" in nixCode.  Any key whose name
            // does not appear literally in nixCode is unaccessed.
            //
            // This is conservative but correct: key names are Nix identifiers
            // (alphanumeric + '_'), so a substring match on nixCode is safe —
            // no false positives from partial matches with path separators etc.
            std::string unaccessedKey;
            for (auto & [k, _] : parsed.items()) {
                if (expr.nixCode.find(k) == std::string::npos) {
                    unaccessedKey = k;
                    break;
                }
            }

            // Guard: must have found an unaccessed key.
            // If all keys appear in nixCode, discard this iteration.
            RC_PRE(!unaccessedKey.empty());

            // Cold eval: evaluates the expression and records a trace.
            {
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
            }

            // Build a mutated JSON object: same as original except the
            // unaccessed key gets a changed value.
            //
            // The mutation is type-preserving to avoid changing the Nix
            // value type (which could cause an unrelated type error):
            //   int    → int + 1  (with INT64_MIN guard)
            //   bool   → !bool
            //   string → string + "_x"
            //
            // For any other type (null, object, array), discard — we cannot
            // produce a safe mutation without risk of evaluation errors.
            auto & unaccessedVal = parsed[unaccessedKey];
            if (unaccessedVal.is_number_integer()) {
                auto v = unaccessedVal.get<int64_t>();
                // Avoid INT64_MAX overflow
                unaccessedVal = (v == std::numeric_limits<int64_t>::max()) ? v - 1 : v + 1;
            } else if (unaccessedVal.is_boolean()) {
                unaccessedVal = !unaccessedVal.get<bool>();
            } else if (unaccessedVal.is_string()) {
                unaccessedVal = unaccessedVal.get<std::string>() + "_x";
            } else {
                // null / nested object / array: not supported — discard
                RC_DISCARD();
            }

            std::string mutatedContent = parsed.dump();

            // Write the mutated content to the slot's file and invalidate
            // the file cache so the verifier sees the new content.
            slot.mutate(mutatedContent);
            invalidateFileCache(slot.path);

            // Warm eval: assert primary-cache hit (no recovery).
            //
            // The trace recorded during cold eval contains only SC deps for
            // the accessed key.  The unaccessed key's value changed, but no
            // dep covers it, so verification must pass and the result must
            // be served from cache without re-evaluating. Asserting
            // deltaRecoveryAttempts() == 0 forbids History-recovery-as-
            // primary-cache-replacement (§N.4).
            {
                PathCountersSnapshot snap;
                auto cache = makeCache(expr.nixCode);
                (void) forceRoot(*cache);
                RC_ASSERT(snap.primaryCacheServedOnly());
            }

            // Restore slot and invalidate cache for clean state on shrink/retry.
            slot.restore();
            invalidateFileCache(slot.path);
        },
        makeParams);
}

} // namespace nix::eval_trace::proptest
