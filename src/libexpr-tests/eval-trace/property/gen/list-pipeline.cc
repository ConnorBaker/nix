#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── ListPipelineGen ──────────────────────────────────────────────────
//
// Generates a 2-layer pipeline:
//   let list   = builtins.fromJSON (builtins.readFile <path>);
//       mapped = builtins.map (x: x * 2) list;
//   in builtins.length mapped
//
// Uses Kind::JsonArray (integer array, 3–8 elements), ResultKind::Int.
// The dep from readFile must propagate through map and be observable via length.
// Only 2 layers (not filter/sort) because filter creates per-element SC deps
// that make precision assertions false.  map→length is sufficient to test dep
// propagation without that problem.

rc::Gen<TestExpr> makeListPipelineGen()
{
    return rc::gen::map(
        rc::gen::mapcat(
            rc::gen::inRange<size_t>(3, 9),  // 3 to 8 elements
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
                "let list = builtins.fromJSON (builtins.readFile "
                + handle->path.string() + ");"
                " mapped = builtins.map (x: x * 2) list;"
                " in builtins.length mapped";

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::Int,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
