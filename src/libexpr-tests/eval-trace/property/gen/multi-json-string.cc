#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── MultiJsonStringGen ──────────────────────────────────────────────
//
// Accesses keys from two different JSON files and combines them into one
// result string:
//
//   let a = builtins.fromJSON (builtins.readFile <a.json>);
//       b = builtins.fromJSON (builtins.readFile <b.json>);
//   in "${toString a."<keyA>"}-${toString b."<keyB>"}"
//
// Both files produce FileBytes + StructuredProjection deps.  Both keys'
// values flow into the result.  This exercises multi-source structured
// data in a single scalar result.
//
// Dep slots:
//   [0] Kind::JsonFile  (a.json)  — Result
//   [1] Kind::JsonFile  (b.json)  — Result
//
// ResultKind: String

rc::Gen<TestExpr> makeMultiJsonStringGen()
{
    return rc::gen::mapcat(
        rc::gen::tuple(makeAccessibleJsonObjectGen(), makeAccessibleJsonObjectGen()),
        [](std::tuple<std::map<std::string, JsonValue>,
                      std::map<std::string, JsonValue>> objs) {
            auto & [objA, objB] = objs;

            std::vector<std::string> keysA, keysB;
            for (auto & [k, _] : objA) keysA.push_back(k);
            for (auto & [k, _] : objB) keysB.push_back(k);

            return rc::gen::map(
                rc::gen::tuple(
                    rc::gen::elementOf(std::move(keysA)),
                    rc::gen::elementOf(std::move(keysB))),
                [objA, objB](std::tuple<std::string, std::string> keys) {
                    auto & [keyA, keyB] = keys;

                    nlohmann::json jsonA = nlohmann::json::object();
                    for (auto & [k, v] : objA) jsonA[k] = v.toJson();
                    std::string contentA = jsonA.dump();

                    nlohmann::json jsonB = nlohmann::json::object();
                    for (auto & [k, v] : objB) jsonB[k] = v.toJson();
                    std::string contentB = jsonB.dump();

                    auto handleA = std::make_shared<TempExtFile>("json", contentA);
                    auto handleB = std::make_shared<TempExtFile>("json", contentB);

                    DepSlot slotA;
                    slotA.kind = DepSlot::Kind::JsonFile;
                    slotA.path = handleA->path;
                    slotA.fileHandle = handleA;
                    slotA.currentValue = contentA;
                    slotA.setOriginal(contentA);

                    DepSlot slotB;
                    slotB.kind = DepSlot::Kind::JsonFile;
                    slotB.path = handleB->path;
                    slotB.fileHandle = handleB;
                    slotB.currentValue = contentB;
                    slotB.setOriginal(contentB);

                    std::string nixCode =
                        "let a = builtins.fromJSON (builtins.readFile " + handleA->path.string() + ");"
                        " b = builtins.fromJSON (builtins.readFile " + handleB->path.string() + ");"
                        " in \"${toString a.\"" + keyA + "\"}-${toString b.\"" + keyB + "\"}\"";

                    std::vector<DepSlot> deps;
                    deps.push_back(std::move(slotA));
                    deps.push_back(std::move(slotB));

                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = TestExpr::ResultKind::String,
                        .depSlots = std::move(deps),
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
