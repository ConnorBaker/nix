#include "../expr-gen.hh"

#include <nlohmann/json.hpp>
#include <limits>

namespace nix::eval_trace::test::proptest {

// ── AttrsetPipelineGen ───────────────────────────────────────────────
//
// Generates a 3-layer composition:
//   let data    = builtins.fromJSON (builtins.readFile <json>);
//       mapped  = builtins.mapAttrs (k: v: v + 1) data;
//       filtered = builtins.removeAttrs mapped ["unwanted"];
//   in filtered."<key>"
//
// The JSON object has 3+ integer-valued keys.  One key is named "unwanted"
// (guaranteed absent from the access) so that removeAttrs has something to
// remove.  The access key is chosen from the remaining keys (never "unwanted").
// Kind::JsonFile, ResultKind::Int (v + 1 on an integer is always an integer).
//
// This generator tests that a single file dep propagates correctly through
// a 3-step pipeline: readFile → fromJSON → mapAttrs → removeAttrs → access.

rc::Gen<TestExpr> makeAttrsetPipelineGen()
{
    return rc::gen::mapcat(
        rc::gen::inRange<size_t>(3, 8),
        [](size_t n) {
            auto pairGen = rc::gen::apply(
                [](std::string k, int64_t v) -> std::pair<std::string, int64_t> {
                    return {std::move(k), v};
                },
                makeNixIdentifierGen(),
                rc::gen::suchThat(
                    rc::gen::inRange<int64_t>(-1000, 1001),
                    [](int64_t v) { return v != std::numeric_limits<int64_t>::min(); }));

            return rc::gen::mapcat(
                rc::gen::container<std::vector<std::pair<std::string, int64_t>>>(n - 1, pairGen),
                [](std::vector<std::pair<std::string, int64_t>> regularPairs) {
                    std::map<std::string, int64_t> obj;
                    for (auto & [k, v] : regularPairs) {
                        if (k != "unwanted")
                            obj.insert_or_assign(std::move(k), v);
                    }
                    if (obj.empty())
                        obj["a"] = 0;
                    if (obj.size() < 2)
                        obj["b"] = 1;
                    obj["unwanted"] = 0;

                    std::vector<std::string> accessibleKeys;
                    accessibleKeys.reserve(obj.size() - 1);
                    for (auto & [k, _] : obj)
                        if (k != "unwanted")
                            accessibleKeys.push_back(k);

                    return rc::gen::map(
                        rc::gen::elementOf(std::move(accessibleKeys)),
                        [obj](std::string chosenKey) {
                            nlohmann::json json = nlohmann::json::object();
                            for (auto & [k, v] : obj)
                                json[k] = v;
                            std::string jsonContent = json.dump();

                            auto handle = std::make_shared<TempExtFile>("json", jsonContent);

                            DepSlot slot;
                            slot.kind = DepSlot::Kind::JsonFile;
                            slot.path = handle->path;
                            slot.fileHandle = handle;
                            slot.currentValue = jsonContent;
                            slot.setOriginal(jsonContent);

                            std::string nixCode =
                                "let data = builtins.fromJSON (builtins.readFile "
                                + handle->path.string() + ");"
                                " mapped = builtins.mapAttrs (k: v: v + 1) data;"
                                " filtered = builtins.removeAttrs mapped [\"unwanted\"];"
                                " in filtered.\"" + chosenKey + "\"";

                            return TestExpr{
                                .nixCode = std::move(nixCode),
                                .expectedKind = TestExpr::ResultKind::Int,
                                .depSlots = {std::move(slot)},
                            };
                        });
                });
        });
}

} // namespace nix::eval_trace::test::proptest
