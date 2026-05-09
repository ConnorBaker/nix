#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── TryEvalGen ───────────────────────────────────────────────────────
//
// Generates: (builtins.tryEval (builtins.fromJSON (builtins.readFile <json>))."<key>").value
//
// This models the nixpkgs pattern for platform-conditional packages:
//   (builtins.tryEval someExpr).value
//
// Only the success path is generated here.  tryEval catches AssertionError
// (from assert / throw) but NOT EvalError (from missing attribute).  Since
// the key is always present in the generated JSON, the inner access succeeds
// and tryEval's .value always equals the accessed key's value.
//
// One JsonFile dep slot (slot[0]).  ResultKind: scalar (from chosen key's value
// type via makeAccessibleJsonObjectGen).
//
// This generator is used by the TryEval property test suite and is included
// in makeNixExprGen for general soundness/precision coverage.

rc::Gen<TestExpr> makeTryEvalGen()
{
    // Use makeAccessibleJsonObjectGen (strictly {String,Int,Bool,Null} values)
    // so the accessed key's ResultKind is always a scalar kind — same pattern
    // as makeAttrAccessGen.
    return rc::gen::mapcat(
        makeAccessibleJsonObjectGen(),
        [](std::map<std::string, JsonValue> obj) {
            std::vector<std::string> keys;
            keys.reserve(obj.size());
            for (auto & [k, _] : obj)
                keys.push_back(k);

            return rc::gen::map(
                rc::gen::elementOf(std::move(keys)),
                [obj](std::string chosenKey) {
                    // Serialize the full object to JSON.
                    nlohmann::json json = nlohmann::json::object();
                    for (auto & [k, v] : obj)
                        json[k] = v.toJson();
                    std::string jsonContent = json.dump();

                    // Create temp file.  Use Kind::JsonFile so that
                    // generateMutation() produces valid JSON (same as makeAttrAccessGen).
                    auto handle = std::make_shared<TempExtFile>("json", jsonContent);

                    DepSlot slot;
                    slot.kind = DepSlot::Kind::JsonFile;
                    slot.path = handle->path;
                    slot.fileHandle = handle;
                    slot.currentValue = jsonContent;
                    slot.setOriginal(jsonContent);

                    // Expression: (builtins.tryEval
                    //               (builtins.fromJSON (builtins.readFile <path>))."<key>"
                    //             ).value
                    //
                    // The key is always present, so tryEval always succeeds and .value
                    // is the actual key value.  Keys are quoted (."key") to handle
                    // Nix reserved words (if, let, etc.).
                    std::string nixCode =
                        "(builtins.tryEval "
                        "((builtins.fromJSON (builtins.readFile "
                        + handle->path.string() + ")).\"" + chosenKey + "\")"
                        ").value";

                    TestExpr::ResultKind kind = obj.at(chosenKey).resultKind();

                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = kind,
                        .depSlots = {std::move(slot)},
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
