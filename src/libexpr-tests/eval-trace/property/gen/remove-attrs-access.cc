#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── RemoveAttrsAccessGen ─────────────────────────────────────────────
//
// Generates: (builtins.removeAttrs (fromJSON readFile) ["other"])."key"
// Kind::JsonFile, ResultKind: scalar.
// The "other" key to remove is a different key from the chosen access key.
// If the object has only one key, no key is removed (removeAttrs with empty list).

rc::Gen<TestExpr> makeRemoveAttrsAccessGen()
{
    return rc::gen::mapcat(
        makeAccessibleJsonObjectGen(),
        [](std::map<std::string, JsonValue> obj) {
            std::vector<std::string> keys;
            keys.reserve(obj.size());
            for (auto & [k, _] : obj)
                keys.push_back(k);

            return rc::gen::map(
                rc::gen::elementOf(keys),
                [obj, keys](std::string chosenKey) {
                    // Pick a key to remove that is NOT the chosen access key.
                    std::string removedKey;
                    for (auto & k : keys) {
                        if (k != chosenKey) {
                            removedKey = k;
                            break;
                        }
                    }

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

                    // Build removeAttrs list: either ["removedKey"] or [] if single key.
                    std::string removeList = removedKey.empty()
                        ? "[]"
                        : "[\"" + removedKey + "\"]";

                    std::string nixCode =
                        "(builtins.removeAttrs (builtins.fromJSON (builtins.readFile "
                        + handle->path.string() + ")) " + removeList + ").\"" + chosenKey + "\"";

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
