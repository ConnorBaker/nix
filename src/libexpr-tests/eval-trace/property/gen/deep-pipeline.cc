#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── DeepPipelineGen ─────────────────────────────────────────────────
//
// 7-operation chain combining plain file and structured data deps:
//
//   let data = builtins.fromJSON (builtins.readFile <data.json>);     // 1: readFile, 2: fromJSON
//       mapped = builtins.mapAttrs (k: v: v) data;                    // 3: mapAttrs
//       cleaned = builtins.removeAttrs mapped ["_extra"];              // 4: removeAttrs
//       value = toString cleaned."<key>";                              // 5: attr access, 6: toString
//       prefix = builtins.readFile <prefix.txt>;                      // 7: readFile (plain)
//   in "${prefix}:${value}"                                           // 8: string interpolation
//
// This exercises dep propagation through mapAttrs → removeAttrs → access,
// combined with a plain file dep in the same result string.
//
// Dep slots:
//   [0] Kind::JsonFile (data.json)   — Result
//   [1] Kind::File     (prefix.txt)  — Result
//
// ResultKind: String

rc::Gen<TestExpr> makeDeepPipelineGen()
{
    return rc::gen::mapcat(
        makeAccessibleJsonObjectGen(),
        [](std::map<std::string, JsonValue> obj) {
            // Ensure at least one key that isn't "_extra".
            std::vector<std::string> keys;
            for (auto & [k, _] : obj)
                if (k != "_extra")
                    keys.push_back(k);
            if (keys.empty()) {
                // Force a usable key.
                obj["val"] = JsonValue{.kind = JsonValue::Kind::Int, .intVal = 42};
                keys.push_back("val");
            }

            return rc::gen::map(
                rc::gen::tuple(
                    rc::gen::elementOf(std::move(keys)),
                    rc::gen::container<std::string>(rc::gen::inRange('a', (char)('z' + 1)))),
                [obj](std::tuple<std::string, std::string> tup) {
                    auto & [chosenKey, prefixContent] = tup;

                    // Add an "_extra" key that removeAttrs will strip.
                    auto fullObj = obj;
                    fullObj["_extra"] = JsonValue{.kind = JsonValue::Kind::Int, .intVal = 999};

                    nlohmann::json json = nlohmann::json::object();
                    for (auto & [k, v] : fullObj)
                        json[k] = v.toJson();
                    std::string jsonContent = json.dump();

                    auto jsonHandle = std::make_shared<TempExtFile>("json", jsonContent);
                    DepSlot jsonSlot;
                    jsonSlot.kind = DepSlot::Kind::JsonFile;
                    jsonSlot.path = jsonHandle->path;
                    jsonSlot.fileHandle = jsonHandle;
                    jsonSlot.currentValue = jsonContent;
                    jsonSlot.setOriginal(jsonContent);

                    if (prefixContent.empty()) prefixContent = "pfx";
                    auto prefixHandle = std::make_shared<TempTextFile>(prefixContent);
                    DepSlot prefixSlot;
                    prefixSlot.kind = DepSlot::Kind::File;
                    prefixSlot.path = prefixHandle->path;
                    prefixSlot.fileHandle = prefixHandle;
                    prefixSlot.currentValue = prefixContent;
                    prefixSlot.setOriginal(prefixContent);

                    std::string nixCode =
                        "let data = builtins.fromJSON (builtins.readFile "
                        + jsonHandle->path.string() + ");"
                        " mapped = builtins.mapAttrs (k: v: v) data;"
                        " cleaned = builtins.removeAttrs mapped [\"_extra\"];"
                        " value = toString cleaned.\"" + chosenKey + "\";"
                        " prefix = builtins.readFile " + prefixHandle->path.string() + ";"
                        " in \"${prefix}:${value}\"";

                    std::vector<DepSlot> deps;
                    deps.push_back(std::move(jsonSlot));
                    deps.push_back(std::move(prefixSlot));

                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = TestExpr::ResultKind::String,
                        .depSlots = std::move(deps),
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
