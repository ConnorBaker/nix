#include "eval-trace/helpers.hh"
#include "../../expr-gen.hh"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include "nix/util/tests/gtest-with-params.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::proptest {

using namespace nix::eval_trace::test;
using namespace nix::eval_trace::test::proptest;

// P37 — Strict Formals with Ellipsis
//
// Expression: ({ <formal>, ... }: <formal>) (fromJSON slot)
// where slot has JSON {"formal": ..., "other1": ..., "other2": ...}.
//
// Soundness: changing formal's value invalidates.
// Precision: changing "other1" or "other2" (not listed in formals) → cache hit.
//   With ellipsis: only the listed formals are required; unlisted keys are not deps.
class EvalTraceProperty_FormalsEllipsis : public TraceCacheFixture {
public:
    EvalTraceProperty_FormalsEllipsis() {
        testFingerprint = hashString(HashAlgorithm::SHA256, "prop-formals-ellipsis");
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

// P37a: Soundness — changing the formal's value invalidates.
TEST_F(EvalTraceProperty_FormalsEllipsis, FormalValueChange_Invalidates)
{
    rc::detail::checkGTestWith(
        [this]() {
            // Generate an object with at least 2 keys.
            auto obj = *makeJsonObjectGen();
            RC_PRE(obj.size() >= 2);

            nlohmann::json json = nlohmann::json::object();
            for (auto & [k, v] : obj) json[k] = v.toJson();
            std::string content = json.dump();

            // Use first key as the formal, leave others as extras.
            auto it = obj.begin();
            std::string formalKey = it->first;

            // Ensure formalKey is a valid Nix identifier (not a reserved word).
            // makeNixIdentifierGen guarantees valid identifiers, but we check for
            // Nix reserved words to avoid parse errors.
            static const std::vector<std::string> reserved = {
                "if", "then", "else", "let", "in", "assert", "with",
                "inherit", "rec", "or", "null", "true", "false"
            };
            RC_PRE(std::find(reserved.begin(), reserved.end(), formalKey) == reserved.end());

            auto & formalVal = json[formalKey];
            RC_PRE(formalVal.is_number_integer() || formalVal.is_boolean() || formalVal.is_string());

            auto handle = std::make_shared<TempExtFile>("json", content);
            DepSlot slot;
            slot.kind = DepSlot::Kind::JsonFile;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = content;
            slot.setOriginal(content);

            // Expression: ({ formalKey, ... }: formalKey) (fromJSON slot)
            std::string nixCode =
                "({ " + formalKey + ", ... }: " + formalKey + ")"
                " (builtins.fromJSON (builtins.readFile " + handle->path.string() + "))";

            // Mutate formalKey's value.
            if (formalVal.is_number_integer()) {
                auto v = formalVal.get<int64_t>();
                formalVal = (v == std::numeric_limits<int64_t>::max()) ? v - 1 : v + 1;
            } else if (formalVal.is_boolean()) {
                formalVal = !formalVal.get<bool>();
            } else {
                formalVal = formalVal.get<std::string>() + "_changed";
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

            // Mutate formal.
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

// P37b: Precision — changing an unlisted key (ellipsis covers extras) → cache hit.
TEST_F(EvalTraceProperty_FormalsEllipsis, UnlistedKeyChange_CacheHit)
{
    rc::detail::checkGTestWith(
        [this]() {
            auto obj = *makeJsonObjectGen();
            RC_PRE(obj.size() >= 2);

            nlohmann::json json = nlohmann::json::object();
            for (auto & [k, v] : obj) json[k] = v.toJson();
            std::string content = json.dump();

            auto it = obj.begin();
            std::string formalKey = it->first; ++it;
            std::string extraKey = it->first;

            static const std::vector<std::string> reserved = {
                "if", "then", "else", "let", "in", "assert", "with",
                "inherit", "rec", "or", "null", "true", "false"
            };
            RC_PRE(std::find(reserved.begin(), reserved.end(), formalKey) == reserved.end());
            RC_PRE(std::find(reserved.begin(), reserved.end(), extraKey) == reserved.end());

            auto handle = std::make_shared<TempExtFile>("json", content);
            DepSlot slot;
            slot.kind = DepSlot::Kind::JsonFile;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = content;
            slot.setOriginal(content);

            // Expression: ({ formalKey, ... }: formalKey) (fromJSON slot)
            std::string nixCode =
                "({ " + formalKey + ", ... }: " + formalKey + ")"
                " (builtins.fromJSON (builtins.readFile " + handle->path.string() + "))";

            // Mutate extraKey's value (not in formals).
            auto & extraVal = json[extraKey];
            if (extraVal.is_number_integer()) {
                auto v = extraVal.get<int64_t>();
                extraVal = (v == std::numeric_limits<int64_t>::max()) ? v - 1 : v + 1;
            } else if (extraVal.is_boolean()) {
                extraVal = !extraVal.get<bool>();
            } else if (extraVal.is_string()) {
                extraVal = extraVal.get<std::string>() + "_x";
            } else {
                RC_DISCARD();
            }
            std::string mutated = json.dump();
            RC_PRE(mutated != content);

            // Cold eval.
            {
                auto cache = makeCache(nixCode);
                (void) forceRoot(*cache);
            }

            // Mutate extra key.
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
