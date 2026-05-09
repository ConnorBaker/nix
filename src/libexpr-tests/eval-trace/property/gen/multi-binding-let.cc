#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── MultiBindingLetGen ───────────────────────────────────────────────
//
// Models mkDerivation: many bindings from different sources, all forced.
//
// Expression:
//   let
//     a = builtins.readFile <file_a>;
//     b = (builtins.fromJSON (builtins.readFile <file_b>))."key";
//     c = builtins.length (builtins.fromJSON (builtins.readFile <file_c>));
//     d = builtins.readFile <file_d>;
//     e = (builtins.fromJSON (builtins.readFile <file_e>))."ekey";
//   in { inherit a d; value = b; count = c; extra = e; }.a
//
// Dep slots (5):
//   slot[0] = file_a  (Kind::File,      TempTextFile)
//   slot[1] = file_b  (Kind::JsonFile,  JSON object with ≥1 key)
//   slot[2] = file_c  (Kind::JsonArray, JSON integer array)
//   slot[3] = file_d  (Kind::File,      TempTextFile)
//   slot[4] = file_e  (Kind::JsonFile,  JSON object with ≥1 key)
//
// The attrset construction forces all five bindings even though only .a is
// returned at the outer level, because Nix evaluates all attrset values during
// construction.  ResultKind: String (accessing .a which is a readFile result).

rc::Gen<TestExpr> makeMultiBindingLetGen()
{
    return rc::gen::mapcat(
        rc::gen::tuple(makeAccessibleJsonObjectGen(), makeAccessibleJsonObjectGen()),
        [](std::tuple<std::map<std::string, JsonValue>,
                      std::map<std::string, JsonValue>> objPair) {
            auto & [objB, objE] = objPair;

            std::vector<std::string> keysB, keysE;
            keysB.reserve(objB.size());
            for (auto & [k, _] : objB)
                keysB.push_back(k);
            keysE.reserve(objE.size());
            for (auto & [k, _] : objE)
                keysE.push_back(k);

            return rc::gen::mapcat(
                rc::gen::tuple(
                    rc::gen::elementOf(keysB),
                    rc::gen::elementOf(keysE)),
                [objB, objE](std::tuple<std::string, std::string> keys) {
                    auto & [keyB, keyE] = keys;

                    return rc::gen::mapcat(
                        rc::gen::tuple(
                            rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
                            rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
                            rc::gen::mapcat(
                                rc::gen::inRange<size_t>(1, 9),
                                [](size_t n) {
                                    return rc::gen::container<std::vector<int>>(
                                        n, rc::gen::inRange(-100, 101));
                                })),
                        [objB, objE, keyB, keyE](
                            std::tuple<std::string, std::string, std::vector<int>> plainAndArr)
                        {
                            auto & [contentA, contentD, arrElems] = plainAndArr;

                            nlohmann::json jsonB = nlohmann::json::object();
                            for (auto & [k, v] : objB) jsonB[k] = v.toJson();
                            std::string contentB = jsonB.dump();

                            nlohmann::json jsonE = nlohmann::json::object();
                            for (auto & [k, v] : objE) jsonE[k] = v.toJson();
                            std::string contentE = jsonE.dump();

                            nlohmann::json arrJson = nlohmann::json::array();
                            for (auto elem : arrElems) arrJson.push_back(elem);
                            std::string contentC = arrJson.dump();

                            auto handleA = std::make_shared<TempTextFile>(contentA);
                            auto handleB = std::make_shared<TempExtFile>("json", contentB);
                            auto handleC = std::make_shared<TempExtFile>("json", contentC);
                            auto handleD = std::make_shared<TempTextFile>(contentD);
                            auto handleE = std::make_shared<TempExtFile>("json", contentE);

                            // slot[0] = file_a → .a (Result: flows into returned value)
                            DepSlot slotA; slotA.kind = DepSlot::Kind::File;
                            slotA.path = handleA->path; slotA.fileHandle = handleA;
                            slotA.currentValue = contentA; slotA.setOriginal(contentA);

                            // slot[1] = file_b → .value (SideEffect: forced during attrset
                            // construction but .a is returned, not .value)
                            DepSlot slotB; slotB.kind = DepSlot::Kind::JsonFile;
                            slotB.contribution = DepSlot::Contribution::Absent;
                            slotB.path = handleB->path; slotB.fileHandle = handleB;
                            slotB.currentValue = contentB; slotB.setOriginal(contentB);

                            // slot[2] = file_c → .count (SideEffect)
                            DepSlot slotC; slotC.kind = DepSlot::Kind::JsonArray;
                            slotC.contribution = DepSlot::Contribution::Absent;
                            slotC.path = handleC->path; slotC.fileHandle = handleC;
                            slotC.currentValue = contentC; slotC.setOriginal(contentC);

                            // slot[3] = file_d → inherit d (SideEffect)
                            DepSlot slotD; slotD.kind = DepSlot::Kind::File;
                            slotD.contribution = DepSlot::Contribution::Absent;
                            slotD.path = handleD->path; slotD.fileHandle = handleD;
                            slotD.currentValue = contentD; slotD.setOriginal(contentD);

                            // slot[4] = file_e → .extra (SideEffect)
                            DepSlot slotE; slotE.kind = DepSlot::Kind::JsonFile;
                            slotE.contribution = DepSlot::Contribution::Absent;
                            slotE.path = handleE->path; slotE.fileHandle = handleE;
                            slotE.currentValue = contentE; slotE.setOriginal(contentE);

                            std::string nixCode =
                                "let\n"
                                "  a = builtins.readFile " + handleA->path.string() + ";\n"
                                "  b = (builtins.fromJSON (builtins.readFile "
                                    + handleB->path.string() + ")).\"" + keyB + "\";\n"
                                "  c = builtins.length (builtins.fromJSON (builtins.readFile "
                                    + handleC->path.string() + "));\n"
                                "  d = builtins.readFile " + handleD->path.string() + ";\n"
                                "  e = (builtins.fromJSON (builtins.readFile "
                                    + handleE->path.string() + ")).\"" + keyE + "\";\n"
                                "in { inherit a d; value = b; count = c; extra = e; }.a";

                            std::vector<DepSlot> deps;
                            deps.push_back(std::move(slotA));
                            deps.push_back(std::move(slotB));
                            deps.push_back(std::move(slotC));
                            deps.push_back(std::move(slotD));
                            deps.push_back(std::move(slotE));

                            return rc::gen::just(TestExpr{
                                .nixCode = std::move(nixCode),
                                .expectedKind = TestExpr::ResultKind::String,
                                .depSlots = std::move(deps),
                            });
                        });
                });
        });
}

} // namespace nix::eval_trace::test::proptest
