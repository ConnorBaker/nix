#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P35 — == on Traced Containers Soundness
//
// Expression: (fromJSON slotA) == (fromJSON slotB)
// slotA and slotB initially contain identical JSON content → result is true.
//
// Soundness: changing slotA to different JSON → result becomes false → invalidates.
//
// The equality comparison forces both attrsets via eqValues, recording #type deps
// on both operands' provenance via maybeRecordTypeDep.
class EvalTraceProperty_EqualityComparison : public TraceCacheFixture {
public:
    EvalTraceProperty_EqualityComparison() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-equality-comparison");
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

// P35a: Soundness — making the two attrsets unequal invalidates.
TEST_F(EvalTraceProperty_EqualityComparison, MakeUnequal_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            // Generate one JSON object and use it for both slots (initially equal).
            auto obj = *makeJsonObjectGen();
            nlohmann::json json = nlohmann::json::object();
            for (auto & [k, v] : obj) json[k] = v.toJson();
            std::string content = json.dump();

            // Create two files with the SAME content.
            auto handleA = std::make_shared<TempExtFile>("json", content);
            auto handleB = std::make_shared<TempExtFile>("json", content);

            DepSlot slotA;
            slotA.kind = DepSlot::Kind::JsonFile;
            slotA.path = handleA->path;
            slotA.fileHandle = handleA;
            slotA.currentValue = content;
            slotA.setOriginal(content);

            std::string nixCode =
                "(builtins.fromJSON (builtins.readFile " + handleA->path.string() + "))"
                " == (builtins.fromJSON (builtins.readFile " + handleB->path.string() + "))";

            // Mutate slotA to have a different value for the first key.
            auto it = json.begin();
            auto & val = it.value();
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
            std::string mutatedA = json.dump();
            RC_PRE(mutatedA != content);

            // Cold eval (result: true — both files have same content).
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

            // Mutate slotA.
            slotA.mutate(mutatedA);
            invalidateFileCache(slotA.path);

            // Warm eval must re-evaluate (result would change to false).
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

// P35b: Soundness for scalars — changing one operand invalidates scalar equality.
TEST_F(EvalTraceProperty_EqualityComparison, ScalarChange_Invalidates)
{
    // SC dep hashes the actual scalar value via canonicalValue().
    // Changing 1→2 changes the hash.
    rc::detail::checkGTestWith(
        [this]() {
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

            auto & val = parsed[accessedKey];
            RC_PRE(val.is_number_integer() || val.is_boolean() || val.is_string());

            // Expression: (fromJSON slot)."key" == (fromJSON slot)."key"
            // Initially true (same value on both sides — same file!).
            // After mutation, both sides change but we check invalidation via loaderCalls.
            // Better: compare with a literal that equals the initial value.
            std::string literalStr;
            if (val.is_number_integer()) {
                literalStr = std::to_string(val.get<int64_t>());
            } else if (val.is_boolean()) {
                literalStr = val.get<bool>() ? "true" : "false";
            } else {
                literalStr = "\"" + nixEscapeString(val.get<std::string>()) + "\"";
            }

            std::string nixCode =
                "((builtins.fromJSON (builtins.readFile " + slot.path.string() + "))"
                ".\"" + accessedKey + "\") == " + literalStr;

            // Mutate the value.
            if (val.is_number_integer()) {
                auto v = val.get<int64_t>();
                val = (v == std::numeric_limits<int64_t>::max()) ? v - 1 : v + 1;
            } else if (val.is_boolean()) {
                val = !val.get<bool>();
            } else {
                val = val.get<std::string>() + "_changed";
            }
            std::string mutated = parsed.dump();
            RC_PRE(mutated != slot.currentValue);

            // Cold eval (result: true).
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
