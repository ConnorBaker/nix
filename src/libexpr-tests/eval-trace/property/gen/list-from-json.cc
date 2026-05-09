#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── ListFromJSONGen ──────────────────────────────────────────────────
//
// Generates: builtins.fromJSON (builtins.readFile <path>)
// where the JSON content is an array of integers.
// Uses Kind::JsonArray so generateMutation() produces valid JSON arrays.

rc::Gen<TestExpr> makeListFromJSONGen()
{
    // Generate an array of 1–8 integer elements.
    return rc::gen::map(
        rc::gen::mapcat(
            rc::gen::inRange<size_t>(1, 9),  // 1 to 8 elements
            [](size_t n) {
                return rc::gen::container<std::vector<int>>(
                    n, rc::gen::inRange(-100, 101));
            }),
        [](std::vector<int> elems) {
            nlohmann::json arr = nlohmann::json::array();
            for (auto e : elems)
                arr.push_back(e);
            std::string jsonContent = arr.dump();

            auto handle = std::make_shared<TempExtFile>("json", jsonContent);

            DepSlot slot;
            slot.kind = DepSlot::Kind::JsonArray;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = jsonContent;
            slot.setOriginal(jsonContent);

            return TestExpr{
                .nixCode = "builtins.fromJSON (builtins.readFile " + handle->path.string() + ")",
                .expectedKind = TestExpr::ResultKind::List,
                .depSlots = {std::move(slot)},
                .indicesToForce = {0},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
