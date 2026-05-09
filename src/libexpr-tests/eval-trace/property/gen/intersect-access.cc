#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── IntersectAccessGen ───────────────────────────────────────────────
//
// Generates: (builtins.intersectAttrs (fromJSON slotA) (fromJSON slotB))."key"
// Two Kind::JsonFile slots; key must exist in BOTH objects.
// ResultKind: scalar from slotB's value (intersectAttrs keeps values from second arg).

rc::Gen<TestExpr> makeIntersectAccessGen()
{
    return rc::gen::mapcat(
        rc::gen::tuple(makeAccessibleJsonObjectGen(), makeAccessibleJsonObjectGen()),
        [](std::tuple<std::map<std::string, JsonValue>,
                      std::map<std::string, JsonValue>> objs) {
            auto & [objA, objB] = objs;

            // Find keys present in both objects (intersect keys).
            std::vector<std::string> commonKeys;
            for (auto & [k, _] : objA) {
                if (objB.count(k))
                    commonKeys.push_back(k);
            }

            // If no common keys, add the first key from objA into objB.
            // We need at least one common key for the expression to succeed.
            auto localA = objA;
            auto localB = objB;
            if (commonKeys.empty()) {
                auto & [firstKey, firstVal] = *localA.begin();
                localB.insert_or_assign(firstKey, firstVal);
                commonKeys.push_back(firstKey);
            }

            return rc::gen::map(
                rc::gen::elementOf(std::move(commonKeys)),
                [localA, localB](std::string chosenKey) {
                    nlohmann::json jsonA = nlohmann::json::object();
                    for (auto & [k, v] : localA)
                        jsonA[k] = v.toJson();
                    std::string contentA = jsonA.dump();

                    nlohmann::json jsonB = nlohmann::json::object();
                    for (auto & [k, v] : localB)
                        jsonB[k] = v.toJson();
                    std::string contentB = jsonB.dump();

                    auto handleA = std::make_shared<TempExtFile>("json", contentA);
                    auto handleB = std::make_shared<TempExtFile>("json", contentB);

                    // slotA provides the key-set filter. Its values don't
                    // appear in the result — only its key names matter.
                    // Recorded: dep IS tracked (file is read) but value-only
                    // changes don't affect the result (SC override serves cache).
                    DepSlot slotA;
                    slotA.kind = DepSlot::Kind::JsonFile;
                    slotA.contribution = DepSlot::Contribution::Recorded;
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

                    std::string pathA = handleA->path.string();
                    std::string pathB = handleB->path.string();

                    // intersectAttrs keeps the keys of the first arg (objA) but values
                    // from the second arg (objB).
                    std::string nixCode =
                        "(builtins.intersectAttrs "
                        "(builtins.fromJSON (builtins.readFile " + pathA + ")) "
                        "(builtins.fromJSON (builtins.readFile " + pathB + "))"
                        ").\"" + chosenKey + "\"";

                    // Result value comes from objB (second argument to intersectAttrs).
                    TestExpr::ResultKind kind = localB.at(chosenKey).resultKind();

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
