#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── HasAttrTestGen ───────────────────────────────────────────────────
//
// Generates: (builtins.fromJSON (builtins.readFile <json>)) ? "key"
// The key is always present in the generated JSON, so the result is always true.
// depSlots: one Kind::JsonFile
// ResultKind: Bool

rc::Gen<TestExpr> makeHasAttrTestGen()
{
    return rc::gen::mapcat(
        makeJsonObjectGen(),
        [](std::map<std::string, JsonValue> obj) {
            // Collect keys for selection
            std::vector<std::string> keys;
            keys.reserve(obj.size());
            for (auto & [k, _] : obj)
                keys.push_back(k);

            return rc::gen::map(
                rc::gen::elementOf(std::move(keys)),
                [obj](std::string chosenKey) {
                    nlohmann::json json = nlohmann::json::object();
                    for (auto & [k, v] : obj)
                        json[k] = v.toJson();
                    std::string jsonContent = json.dump();

                    auto handle = std::make_shared<TempExtFile>("json", jsonContent);

                    DepSlot slot;
                    slot.kind = DepSlot::Kind::JsonFile;
                    slot.path = handle->path;
                    slot.fileHandle = handle;
                    slot.currentValue = jsonContent;
                    slot.setOriginal(jsonContent);

                    // Key is always present → result is always true
                    std::string nixCode =
                        "(builtins.fromJSON (builtins.readFile "
                        + handle->path.string() + ")) ? \"" + chosenKey + "\"";

                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = TestExpr::ResultKind::Bool,
                        .depSlots = {std::move(slot)},
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
