#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── AttrAccessGen ────────────────────────────────────────────────────

rc::Gen<TestExpr> makeAttrAccessGen()
{
    // Use mapcat: first generate the JSON object, then pick one key from it.
    // Use makeAccessibleJsonObjectGen (strictly {String,Int,Bool,Null} values)
    // so that the accessed key's ResultKind is always a scalar kind.
    return rc::gen::mapcat(
        makeAccessibleJsonObjectGen(),
        [](std::map<std::string, JsonValue> obj) {
            // Collect keys into a vector for random selection
            std::vector<std::string> keys;
            keys.reserve(obj.size());
            for (auto & [k, _] : obj)
                keys.push_back(k);

            return rc::gen::map(
                rc::gen::elementOf(std::move(keys)),
                [obj](std::string chosenKey) {
                    // Serialize the full object to JSON
                    nlohmann::json json = nlohmann::json::object();
                    for (auto & [k, v] : obj)
                        json[k] = v.toJson();
                    std::string jsonContent = json.dump();

                    // Create temp file.  Use Kind::JsonFile so that
                    // generateMutation() produces valid JSON for P2 invalidation
                    // (see makeFromJSONGen for the full rationale).
                    auto handle = std::make_shared<TempExtFile>("json", jsonContent);

                    DepSlot slot;
                    slot.kind = DepSlot::Kind::JsonFile;
                    slot.path = handle->path;
                    slot.fileHandle = handle;
                    slot.currentValue = jsonContent;
                    slot.setOriginal(jsonContent);

                    // Expression: (builtins.fromJSON (builtins.readFile <path>))."<key>"
                    // Keys must be quoted with ."key" syntax because generated
                    // identifiers could be Nix reserved words (if, then, else,
                    // let, in, assert, with, inherit, rec) which are parse
                    // errors in unquoted .key access.
                    std::string nixCode =
                        "(builtins.fromJSON (builtins.readFile "
                        + handle->path.string() + ")).\"" + chosenKey + "\"";

                    // ResultKind is derived from the chosen key's value type
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
