#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── DeepAttrsetAccessGen ────────────────────────────────────────────
//
// 3-level nested attrset with selective forcing at the deepest level:
//
//   { level1 = {
//       level2 = {
//         json_val = (builtins.fromJSON (builtins.readFile <data.json>))."key";
//         plain_val = builtins.readFile <plain.txt>;
//       };
//       side2 = builtins.readFile <side2.txt>;
//     };
//     side1 = builtins.readFile <side1.txt>;
//   }
//
// attrsToForce = {"level1.level2.json_val", "level1.level2.plain_val"}
//
// Forces exactly the two leaf values at depth 3.  "side1" and "side2"
// are never forced and marked Absent.  This stress-tests the dot-path
// force walker at 3 components and verifies that intermediate attrset
// construction does not leak deps from unforced siblings.
//
// Dep slots:
//   [0] Kind::JsonFile (data.json)   — Result
//   [1] Kind::File     (plain.txt)   — Result
//   [2] Kind::File     (side2.txt)   — Absent (level1.side2, not forced)
//   [3] Kind::File     (side1.txt)   — Absent (side1, not forced)
//
// ResultKind: Attrset
// attrsToForce: {"level1.level2.json_val", "level1.level2.plain_val"}

rc::Gen<TestExpr> makeDeepAttrsetAccessGen()
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
                    rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
                    rc::gen::container<std::string>(rc::gen::inRange('!', '~'))),
                [obj](std::tuple<std::string, std::string, std::string, std::string> tup) {
                    auto & [chosenKey, plainContent, side2Content, side1Content] = tup;

                    nlohmann::json json = nlohmann::json::object();
                    for (auto & [k, v] : obj) json[k] = v.toJson();
                    std::string jsonContent = json.dump();
                    auto jsonHandle = std::make_shared<TempExtFile>("json", jsonContent);

                    if (plainContent.empty()) plainContent = "p";
                    auto plainHandle = std::make_shared<TempTextFile>(plainContent);

                    if (side2Content.empty()) side2Content = "s2";
                    auto side2Handle = std::make_shared<TempTextFile>(side2Content);

                    if (side1Content.empty()) side1Content = "s1";
                    auto side1Handle = std::make_shared<TempTextFile>(side1Content);

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

                    DepSlot side2Slot;
                    side2Slot.kind = DepSlot::Kind::File;
                    side2Slot.contribution = DepSlot::Contribution::Absent;
                    side2Slot.path = side2Handle->path;
                    side2Slot.fileHandle = side2Handle;
                    side2Slot.currentValue = side2Content;
                    side2Slot.setOriginal(side2Content);

                    DepSlot side1Slot;
                    side1Slot.kind = DepSlot::Kind::File;
                    side1Slot.contribution = DepSlot::Contribution::Absent;
                    side1Slot.path = side1Handle->path;
                    side1Slot.fileHandle = side1Handle;
                    side1Slot.currentValue = side1Content;
                    side1Slot.setOriginal(side1Content);

                    std::string nixCode =
                        "{ level1 = {"
                        " level2 = {"
                        " json_val = (builtins.fromJSON (builtins.readFile "
                        + jsonHandle->path.string() + ")).\"" + chosenKey + "\";"
                        " plain_val = builtins.readFile " + plainHandle->path.string() + ";"
                        " };"
                        " side2 = builtins.readFile " + side2Handle->path.string() + ";"
                        " };"
                        " side1 = builtins.readFile " + side1Handle->path.string() + ";"
                        " }";

                    std::vector<DepSlot> deps;
                    deps.push_back(std::move(jsonSlot));
                    deps.push_back(std::move(plainSlot));
                    deps.push_back(std::move(side2Slot));
                    deps.push_back(std::move(side1Slot));

                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = TestExpr::ResultKind::Attrset,
                        .depSlots = std::move(deps),
                        .attrsToForce = {
                            "level1.level2.json_val",
                            "level1.level2.plain_val",
                        },
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
