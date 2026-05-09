#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── AttrNamesLengthGen ───────────────────────────────────────────────
//
// Generates: builtins.length (builtins.attrNames (builtins.fromJSON (builtins.readFile <path>)))
// Kind::JsonFile, ResultKind::Int.
// Tests #keys dep propagation.

rc::Gen<TestExpr> makeAttrNamesLengthGen()
{
    return rc::gen::map(
        makeJsonObjectGen(),
        [](std::map<std::string, JsonValue> obj) {
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
                "builtins.length (builtins.attrNames (builtins.fromJSON (builtins.readFile "
                + handle->path.string() + ")))";

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::Int,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
