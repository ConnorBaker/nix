#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── TripleSourceMergeGen ─────────────────────────────────────────────
//
// Generates:
//   let a = builtins.fromJSON (builtins.readFile <json_a>);
//       b = builtins.fromJSON (builtins.readFile <json_b>);
//       c = builtins.fromJSON (builtins.readFile <json_c>);
//   in (a // b // c)."<key>"
//
// The accessed key exists in EXACTLY ONE of the three sources (the "winner").
// The other two sources never have that key, making soundness/precision
// unambiguous: changing the winner's copy of the key must invalidate, while
// changing either of the other two sources must not.
//
// DepSlot ordering convention (mirrors makeMultiSourceAttrGen):
//   index 0 — the winning source (the one that actually provides the key)
//   index 1 — a non-winning source (does NOT have the key)
//   index 2 — a non-winning source (does NOT have the key)
//
// In (a // b // c), later sources override earlier ones.  The winner is
// chosen at one of positions a/b/c randomly; the winning source is always
// placed at depSlots[0] so that property tests can use a stable convention.
//
// This generator is used for the MultiSourceMerge property tests.

rc::Gen<TestExpr> makeTripleSourceMergeGen()
{
    // Generate three independent JSON objects.
    // Use makeAccessibleJsonObjectGen (strictly {String,Int,Bool,Null} values)
    // so that the accessed key's ResultKind is always a scalar kind.
    return rc::gen::mapcat(
        rc::gen::tuple(
            makeAccessibleJsonObjectGen(),
            makeAccessibleJsonObjectGen(),
            makeAccessibleJsonObjectGen()),
        [](std::tuple<std::map<std::string, JsonValue>,
                      std::map<std::string, JsonValue>,
                      std::map<std::string, JsonValue>> objs) {
            auto & [obj0, obj1, obj2] = objs;

            // Collect all keys from obj0 for access key selection.
            std::vector<std::string> keys0;
            keys0.reserve(obj0.size());
            for (auto & [k, _] : obj0)
                keys0.push_back(k);

            return rc::gen::mapcat(
                rc::gen::elementOf(std::move(keys0)),
                [obj0, obj1, obj2](std::string chosenKey) {
                    // The chosen key will come from one of three positions (a/b/c).
                    // Randomly select which position is the winner.
                    return rc::gen::map(
                        rc::gen::inRange(0, 3),  // 0=a, 1=b, 2=c
                        [obj0, obj1, obj2, chosenKey](int winnerPos) {
                            // Build three objects:
                            //   winnerObj has chosenKey (from obj0's value)
                            //   the other two do NOT have chosenKey
                            const JsonValue & winnerVal = obj0.at(chosenKey);

                            auto makeWithKey = [&](const std::map<std::string, JsonValue> & src) {
                                auto m = src;
                                m.insert_or_assign(chosenKey, winnerVal);
                                return m;
                            };
                            auto makeWithoutKey = [&](const std::map<std::string, JsonValue> & src) {
                                auto m = src;
                                m.erase(chosenKey);
                                return m;
                            };

                            // Assign winner and non-winners based on winnerPos.
                            // In a // b // c, later sources override earlier ones, so:
                            //   winnerPos=0 → winner=a, non-winners=b,c (b and c don't have key)
                            //   winnerPos=1 → winner=b, non-winners=a,c (a and c don't have key)
                            //   winnerPos=2 → winner=c, non-winners=a,b (a and b don't have key)
                            std::map<std::string, JsonValue> objA, objB, objC;
                            switch (winnerPos) {
                            case 0:
                                objA = makeWithKey(obj0);
                                objB = makeWithoutKey(obj1);
                                objC = makeWithoutKey(obj2);
                                break;
                            case 1:
                                objA = makeWithoutKey(obj0);
                                objB = makeWithKey(obj1);
                                objC = makeWithoutKey(obj2);
                                break;
                            default:  // case 2
                                objA = makeWithoutKey(obj0);
                                objB = makeWithoutKey(obj1);
                                objC = makeWithKey(obj2);
                                break;
                            }

                            // Ensure the winner object is non-empty after key assignment.
                            // (obj0 always has chosenKey so winner is always non-empty.)
                            // The non-winner objects may be empty after erasing chosenKey;
                            // an empty JSON object "{}" is valid and evaluates to an empty
                            // attrset, so (a // {} // {}) is fine.

                            // Serialize all three objects.
                            auto serialize = [](const std::map<std::string, JsonValue> & m) {
                                nlohmann::json j = nlohmann::json::object();
                                for (auto & [k, v] : m)
                                    j[k] = v.toJson();
                                return j.dump();
                            };
                            std::string contentA = serialize(objA);
                            std::string contentB = serialize(objB);
                            std::string contentC = serialize(objC);

                            // Identify winner content and the two non-winner contents.
                            std::string winnerContent, nonWinner1Content, nonWinner2Content;
                            switch (winnerPos) {
                            case 0:
                                winnerContent = contentA;
                                nonWinner1Content = contentB;
                                nonWinner2Content = contentC;
                                break;
                            case 1:
                                winnerContent = contentB;
                                nonWinner1Content = contentA;
                                nonWinner2Content = contentC;
                                break;
                            default:  // case 2
                                winnerContent = contentC;
                                nonWinner1Content = contentA;
                                nonWinner2Content = contentB;
                                break;
                            }

                            // Create three JsonFile temp files.
                            auto handleA = std::make_shared<TempExtFile>("json", contentA);
                            auto handleB = std::make_shared<TempExtFile>("json", contentB);
                            auto handleC = std::make_shared<TempExtFile>("json", contentC);

                            // Build DepSlots: winner at index 0, non-winners at 1 and 2.
                            auto makeSlot = [](std::shared_ptr<TempExtFile> handle,
                                               const std::string & content) {
                                DepSlot slot;
                                slot.kind = DepSlot::Kind::JsonFile;
                                slot.path = handle->path;
                                slot.fileHandle = handle;
                                slot.currentValue = content;
                                slot.setOriginal(content);
                                return slot;
                            };

                            std::shared_ptr<TempExtFile> winnerHandle, non1Handle, non2Handle;
                            switch (winnerPos) {
                            case 0:
                                winnerHandle = handleA;
                                non1Handle   = handleB;
                                non2Handle   = handleC;
                                break;
                            case 1:
                                winnerHandle = handleB;
                                non1Handle   = handleA;
                                non2Handle   = handleC;
                                break;
                            default:  // case 2
                                winnerHandle = handleC;
                                non1Handle   = handleA;
                                non2Handle   = handleB;
                                break;
                            }

                            DepSlot slotWinner = makeSlot(winnerHandle, winnerContent);
                            // Non-winners: chosenKey is absent, so their content
                            // never flows into the result.
                            DepSlot slotNon1   = makeSlot(non1Handle,   nonWinner1Content);
                            slotNon1.contribution = DepSlot::Contribution::Recorded;
                            DepSlot slotNon2   = makeSlot(non2Handle,   nonWinner2Content);
                            slotNon2.contribution = DepSlot::Contribution::Recorded;

                            // Build the Nix expression.
                            std::string pathA = handleA->path.string();
                            std::string pathB = handleB->path.string();
                            std::string pathC = handleC->path.string();
                            std::string nixCode =
                                "let a = builtins.fromJSON (builtins.readFile " + pathA + ");"
                                " b = builtins.fromJSON (builtins.readFile " + pathB + ");"
                                " c = builtins.fromJSON (builtins.readFile " + pathC + ");"
                                " in (a // b // c).\"" + chosenKey + "\"";

                            TestExpr::ResultKind kind = winnerVal.resultKind();

                            // DepSlot ordering convention:
                            //   [0] = winner (provides the accessed key)
                            //   [1] = non-winner 1
                            //   [2] = non-winner 2
                            std::vector<DepSlot> deps;
                            deps.push_back(std::move(slotWinner));
                            deps.push_back(std::move(slotNon1));
                            deps.push_back(std::move(slotNon2));

                            return TestExpr{
                                .nixCode = std::move(nixCode),
                                .expectedKind = kind,
                                .depSlots = std::move(deps),
                            };
                        });
                });
        });
}

} // namespace nix::eval_trace::test::proptest
