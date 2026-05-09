#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── SortLengthGen ────────────────────────────────────────────────────
//
// Generates: builtins.length (builtins.sort builtins.lessThan (builtins.fromJSON (builtins.readFile <path>)))
// Kind::JsonArray (integer array), ResultKind::Int.

rc::Gen<TestExpr> makeSortLengthGen()
{
    return rc::gen::map(
        rc::gen::mapcat(
            rc::gen::inRange<size_t>(1, 9),
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

            std::string nixCode =
                "builtins.length (builtins.sort builtins.lessThan "
                "(builtins.fromJSON (builtins.readFile "
                + handle->path.string() + ")))";

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::Int,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
