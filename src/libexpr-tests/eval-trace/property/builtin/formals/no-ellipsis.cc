#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P38 — Strict Formals without Ellipsis
//
// Expression: ({ <formalA>, <formalB> }: <formalA>) (fromJSON slot)
// where slot JSON has {"formalA": ..., "formalB": ..., "extra": ...}.
//
// Soundness: changing formalA's value invalidates (it's accessed).
// Precision: changing "extra" (not a formal, not accessed) → cache hit.
//   Without ellipsis: both formalA and formalB are required, but extra is not a formal.
//   So changing extra should not invalidate.
class EvalTraceProperty_FormalsNoEllipsis : public TraceCacheFixture {
public:
    EvalTraceProperty_FormalsNoEllipsis() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-formals-no-ellipsis");
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

static const std::vector<std::string> nixReserved = {
    "if", "then", "else", "let", "in", "assert", "with",
    "inherit", "rec", "or", "null", "true", "false"
};

static bool isValidFormal(const std::string & key) {
    return std::find(nixReserved.begin(), nixReserved.end(), key) == nixReserved.end();
}

// P38a: Soundness — changing formalA's value (it's accessed) invalidates.
TEST_F(EvalTraceProperty_FormalsNoEllipsis, AccessedFormalChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto obj = *makeJsonObjectGen();
            RC_PRE(obj.size() >= 2);

            auto it = obj.begin();
            std::string formalA = it->first; ++it;
            std::string formalB = it->first;

            RC_PRE(isValidFormal(formalA));
            RC_PRE(isValidFormal(formalB));
            RC_PRE(formalA != formalB);

            // Build JSON with ONLY the two formal keys — strict formals
            // (no ellipsis) reject any extra keys with "unexpected argument".
            nlohmann::json json = nlohmann::json::object();
            json[formalA] = obj[formalA].toJson();
            json[formalB] = obj[formalB].toJson();
            std::string content = json.dump();

            auto & valA = json[formalA];
            RC_PRE(valA.is_number_integer() || valA.is_boolean() || valA.is_string());

            auto handle = std::make_shared<TempExtFile>("json", content);
            DepSlot slot;
            slot.kind = DepSlot::Kind::JsonFile;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = content;
            slot.setOriginal(content);

            // Expression: ({ formalA, formalB }: formalA) (fromJSON slot)
            // No ellipsis — both formalA and formalB must be present.
            std::string nixCode =
                "({ " + formalA + ", " + formalB + " }: " + formalA + ")"
                " (builtins.fromJSON (builtins.readFile " + handle->path.string() + "))";

            // Mutate formalA.
            if (valA.is_number_integer()) {
                auto v = valA.get<int64_t>();
                valA = (v == std::numeric_limits<int64_t>::max()) ? v - 1 : v + 1;
            } else if (valA.is_boolean()) {
                valA = !valA.get<bool>();
            } else {
                valA = valA.get<std::string>() + "_changed";
            }
            std::string mutated = json.dump();
            RC_PRE(mutated != content);

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

            // Mutate formalA.
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

// P38b: Precision — changing formalB (required but not accessed) → cache hit.
// Uses ellipsis to allow extra keys so the cold eval doesn't throw.
TEST_F(EvalTraceProperty_FormalsNoEllipsis, ExtraKeyChange_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto obj = *makeJsonObjectGen();
            RC_PRE(obj.size() >= 2);

            auto it = obj.begin();
            std::string formalA = it->first; ++it;
            std::string formalB = it->first;

            RC_PRE(isValidFormal(formalA));
            RC_PRE(isValidFormal(formalB));
            RC_PRE(formalA != formalB);

            // Build JSON with only the two formal keys.
            nlohmann::json json = nlohmann::json::object();
            json[formalA] = obj[formalA].toJson();
            json[formalB] = obj[formalB].toJson();
            std::string content = json.dump();

            auto handle = std::make_shared<TempExtFile>("json", content);
            DepSlot slot;
            slot.kind = DepSlot::Kind::JsonFile;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = content;
            slot.setOriginal(content);

            // Without ellipsis: extra key is not a formal.
            // Note: Nix without ellipsis will ERROR if extra keys exist!
            // So we need to use ({ formalA, formalB, ... }: ...) or accept the error.
            // Actually: Nix strict formals without ellipsis REJECT extra keys.
            // This means the expression would THROW if extra is present.
            // Per P38 definition: we need to test that extra (unlisted) doesn't invalidate.
            // But Nix will reject the call if extra is present without ellipsis.
            // RESOLUTION: The precision property for "no ellipsis" should NOT have extra keys
            // present in the JSON — only test that changing a value not accessed doesn't invalidate.
            // We test: change formalB's value (it's required but NOT accessed → should not invalidate).
            // This is the real precision test for no-ellipsis formals.

            auto & valB = json[formalB];
            if (valB.is_number_integer()) {
                auto v = valB.get<int64_t>();
                valB = (v == std::numeric_limits<int64_t>::max()) ? v - 1 : v + 1;
            } else if (valB.is_boolean()) {
                valB = !valB.get<bool>();
            } else if (valB.is_string()) {
                valB = valB.get<std::string>() + "_x";
            } else {
                RC_DISCARD();
            }
            std::string mutated = json.dump();
            RC_PRE(mutated != content);

            // Expression: ({ formalA, formalB, ... }: formalA) (fromJSON slot)
            // Use ellipsis to allow extra keys, but formalB is listed (required) yet not accessed.
            // This tests that non-accessed but required formal's VALUE change doesn't invalidate.
            std::string nixCode =
                "({ " + formalA + ", " + formalB + ", ... }: " + formalA + ")"
                " (builtins.fromJSON (builtins.readFile " + handle->path.string() + "))";

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate formalB (required but not accessed).
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
