#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── NestedAttrsetAccessGen ──────────────────────────────────────────
//
// Multi-level attrset with attrsToForce at two levels of nesting.
// Forces specific paths to test per-attribute dep recording for
// nested container results.
//
//   { top = {
//       json_val = (builtins.fromJSON (builtins.readFile <data.json>))."key";
//       plain_val = builtins.readFile <plain.txt>;
//     };
//     side = builtins.readFile <side.txt>;
//   }
//
// attrsToForce = {"top"} — forces the inner attrset.
// The inner attrset's children (json_val, plain_val) are also forced by
// forceRootAndChildren because "top" evaluates to an attrset whose values
// are thunks that get forced when the trace session records the attrset.
//
// "side" is NOT forced and its dep MUST NOT appear.
//
// Dep slots:
//   [0] Kind::JsonFile (data.json)  — Result (accessed by json_val)
//   [1] Kind::File     (plain.txt)  — Result (accessed by plain_val)
//   [2] Kind::File     (side.txt)   — Absent (not forced via attrsToForce)
//
// ResultKind: Attrset
// attrsToForce: {"top"}

rc::Gen<TestExpr> makeNestedAttrsetAccessGen()
{
    return rc::gen::mapcat(
        makeAccessibleJsonObjectGen(),
        [](std::map<std::string, JsonValue> obj) {
            std::vector<std::string> keys;
            for (auto & [k, _] : obj) keys.push_back(k);

            return rc::gen::map(
                rc::gen::tuple(
                    rc::gen::elementOf(std::move(keys)),
                    rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
                    rc::gen::container<std::string>(rc::gen::inRange('!', '~'))),
                [obj](std::tuple<std::string, std::string, std::string> tup) {
                    auto & [chosenKey, plainContent, sideContent] = tup;

                    nlohmann::json json = nlohmann::json::object();
                    for (auto & [k, v] : obj) json[k] = v.toJson();
                    std::string jsonContent = json.dump();
                    auto jsonHandle = std::make_shared<TempExtFile>("json", jsonContent);

                    if (plainContent.empty()) plainContent = "p";
                    auto plainHandle = std::make_shared<TempTextFile>(plainContent);

                    if (sideContent.empty()) sideContent = "s";
                    auto sideHandle = std::make_shared<TempTextFile>(sideContent);

                    DepSlot jsonSlot;
                    jsonSlot.kind = DepSlot::Kind::JsonFile;
                    jsonSlot.path = jsonHandle->path;
                    jsonSlot.fileHandle = jsonHandle;
                    jsonSlot.currentValue = jsonContent;
                    jsonSlot.setOriginal(jsonContent);

                    DepSlot plainSlot;
                    plainSlot.kind = DepSlot::Kind::File;
                    plainSlot.path = plainHandle->path;
                    plainSlot.fileHandle = plainHandle;
                    plainSlot.currentValue = plainContent;
                    plainSlot.setOriginal(plainContent);

                    DepSlot sideSlot;
                    sideSlot.kind = DepSlot::Kind::File;
                    sideSlot.contribution = DepSlot::Contribution::Absent;
                    sideSlot.path = sideHandle->path;
                    sideSlot.fileHandle = sideHandle;
                    sideSlot.currentValue = sideContent;
                    sideSlot.setOriginal(sideContent);

                    std::string nixCode =
                        "{ top = {"
                        " json_val = (builtins.fromJSON (builtins.readFile "
                        + jsonHandle->path.string() + ")).\"" + chosenKey + "\";"
                        " plain_val = builtins.readFile " + plainHandle->path.string() + ";"
                        " };"
                        " side = builtins.readFile " + sideHandle->path.string() + ";"
                        " }";

                    std::vector<DepSlot> deps;
                    deps.push_back(std::move(jsonSlot));
                    deps.push_back(std::move(plainSlot));
                    deps.push_back(std::move(sideSlot));

                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = TestExpr::ResultKind::Attrset,
                        .depSlots = std::move(deps),
                        // Force only the inner values — NOT "side".
                        // "top" alone would just construct the inner attrset
                        // without forcing json_val or plain_val.
                        .attrsToForce = {"top.json_val", "top.plain_val"},
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
