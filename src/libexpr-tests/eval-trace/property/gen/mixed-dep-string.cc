#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── MixedDepStringGen ───────────────────────────────────────────────
//
// Generates an expression whose result is a string that depends on both
// a plain file read AND a structured data access from different files:
//
//   "${builtins.readFile <plain.txt>}-${(builtins.fromJSON (builtins.readFile <data.json>))."<key>"}"
//
// This is the simplest expression that produces BOTH FileBytes (for
// plain.txt) and FileBytes + StructuredProjection (for data.json) deps
// flowing into a single scalar result.  No existing generator combines
// a plain-file dep with a structured-data dep in the same result.
//
// Dep slots:
//   [0] Kind::File      (plain.txt)  — Result
//   [1] Kind::JsonFile  (data.json)  — Result
//
// ResultKind: String

rc::Gen<TestExpr> makeMixedDepStringGen()
{
    return rc::gen::mapcat(
        makeAccessibleJsonObjectGen(),
        [](std::map<std::string, JsonValue> obj) {
            std::vector<std::string> keys;
            keys.reserve(obj.size());
            for (auto & [k, _] : obj)
                keys.push_back(k);

            return rc::gen::map(
                rc::gen::tuple(
                    rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
                    rc::gen::elementOf(std::move(keys))),
                [obj](std::tuple<std::string, std::string> tup) {
                    auto & [plainContent, chosenKey] = tup;

                    // Plain file slot.
                    auto plainHandle = std::make_shared<TempTextFile>(plainContent);
                    DepSlot plainSlot;
                    plainSlot.kind = DepSlot::Kind::File;
                    plainSlot.path = plainHandle->path;
                    plainSlot.fileHandle = plainHandle;
                    plainSlot.currentValue = plainContent;
                    plainSlot.setOriginal(plainContent);

                    // JSON file slot.
                    nlohmann::json json = nlohmann::json::object();
                    for (auto & [k, v] : obj)
                        json[k] = v.toJson();
                    std::string jsonContent = json.dump();

                    auto jsonHandle = std::make_shared<TempExtFile>("json", jsonContent);
                    DepSlot jsonSlot;
                    jsonSlot.kind = DepSlot::Kind::JsonFile;
                    jsonSlot.path = jsonHandle->path;
                    jsonSlot.fileHandle = jsonHandle;
                    jsonSlot.currentValue = jsonContent;
                    jsonSlot.setOriginal(jsonContent);

                    std::string nixCode =
                        "\"${builtins.readFile " + plainHandle->path.string()
                        + "}-${toString (builtins.fromJSON (builtins.readFile "
                        + jsonHandle->path.string() + ")).\"" + chosenKey + "\"}\"";

                    std::vector<DepSlot> deps;
                    deps.push_back(std::move(plainSlot));
                    deps.push_back(std::move(jsonSlot));

                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = TestExpr::ResultKind::String,
                        .depSlots = std::move(deps),
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
