#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── MultiSourceAttrGen ───────────────────────────────────────────────
//
// Generates: ((builtins.fromJSON (builtins.readFile slotA)) //
//             (builtins.fromJSON (builtins.readFile slotB)))."<key>"
//
// The key is chosen from one of the two source objects.  Two JsonFile dep slots
// are created (slotA at index 0, slotB at index 1).
//
// This generator is used for P12 (update operator soundness/precision).

rc::Gen<TestExpr> makeMultiSourceAttrGen()
{
    // Generate two independent JSON objects.  Each object has 1–5 keys.
    // Choose the accessed key from objA (index 0) so that:
    //   - changing the accessed key in objA invalidates (soundness)
    //   - changing an unaccessed key in objA or any key in objB does not (precision)
    // Use makeAccessibleJsonObjectGen (strictly {String,Int,Bool,Null} values)
    // so that the accessed key's ResultKind is always a scalar kind.
    return rc::gen::mapcat(
        rc::gen::tuple(makeAccessibleJsonObjectGen(), makeAccessibleJsonObjectGen()),
        [](std::tuple<std::map<std::string, JsonValue>,
                      std::map<std::string, JsonValue>> objs) {
            auto & [objA, objB] = objs;

            // Collect objA's keys for key selection.
            std::vector<std::string> keysA;
            keysA.reserve(objA.size());
            for (auto & [k, _] : objA)
                keysA.push_back(k);

            return rc::gen::map(
                rc::gen::elementOf(std::move(keysA)),
                [objA, objB](std::string chosenKey) {
                    // Ensure objB doesn't have chosenKey (// would shadow objA's value).
                    // Remove it from objB if present so objA's value is authoritative.
                    auto localB = objB;
                    localB.erase(chosenKey);

                    // Serialize both objects.
                    nlohmann::json jsonA = nlohmann::json::object();
                    for (auto & [k, v] : objA)
                        jsonA[k] = v.toJson();
                    std::string contentA = jsonA.dump();

                    nlohmann::json jsonB = nlohmann::json::object();
                    for (auto & [k, v] : localB)
                        jsonB[k] = v.toJson();
                    std::string contentB = jsonB.dump();

                    // Create two JsonFile temp files.
                    auto handleA = std::make_shared<TempExtFile>("json", contentA);
                    auto handleB = std::make_shared<TempExtFile>("json", contentB);

                    // slotA: has chosenKey → Result (value flows into result)
                    DepSlot slotA;
                    slotA.kind = DepSlot::Kind::JsonFile;
                    slotA.path = handleA->path;
                    slotA.fileHandle = handleA;
                    slotA.currentValue = contentA;
                    slotA.setOriginal(contentA);

                    // slotB: chosenKey erased → SideEffect (read by evaluator
                    // for the // merge but never contributes to chosenKey's value)
                    DepSlot slotB;
                    slotB.kind = DepSlot::Kind::JsonFile;
                    slotB.contribution = DepSlot::Contribution::Recorded;
                    slotB.path = handleB->path;
                    slotB.fileHandle = handleB;
                    slotB.currentValue = contentB;
                    slotB.setOriginal(contentB);

                    // Expression: ((fromJSON slotA) // (fromJSON slotB))."key"
                    std::string pathA = handleA->path.string();
                    std::string pathB = handleB->path.string();
                    std::string nixCode =
                        "((builtins.fromJSON (builtins.readFile " + pathA + "))"
                        " // (builtins.fromJSON (builtins.readFile " + pathB + ")))"
                        ".\"" + chosenKey + "\"";

                    TestExpr::ResultKind kind = objA.at(chosenKey).resultKind();

                    std::vector<DepSlot> deps;
                    deps.push_back(std::move(slotA));
                    deps.push_back(std::move(slotB));

                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = kind,
                        .depSlots = std::move(deps),
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
