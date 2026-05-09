#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── MapAttrsAccessGen ────────────────────────────────────────────────
//
// Generates: (builtins.mapAttrs (k: v: v) (builtins.fromJSON (builtins.readFile <path>)))."key"
// Kind::JsonFile, ResultKind: scalar (from the chosen key's value type).
// Tests dep propagation through mapAttrs + DerivedContainerBuilder.

rc::Gen<TestExpr> makeMapAttrsAccessGen()
{
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

                    std::string nixCode =
                        "(builtins.mapAttrs (k: v: v) (builtins.fromJSON (builtins.readFile "
                        + handle->path.string() + "))).\"" + chosenKey + "\"";

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
