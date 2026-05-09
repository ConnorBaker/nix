#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── FilterLengthGen ──────────────────────────────────────────────────
//
// Generates: builtins.length (builtins.filter (x: x > 0) (builtins.fromJSON (builtins.readFile <path>)))
// Kind::JsonArray (integer array), ResultKind::Int.
// Tests #len dep through filter's DerivedContainerBuilder.
//
// The array always contains at least one positive integer (appended by
// the generator) so that the filter predicate (x: x > 0) always has
// at least one match.  Without this guarantee, ~50% of short arrays
// have no positive element, causing RC to exhaust its discard budget.

rc::Gen<TestExpr> makeFilterLengthGen()
{
    return rc::gen::map(
        rc::gen::mapcat(
            rc::gen::inRange<size_t>(1, 9),
            [](size_t n) {
                // Generate n arbitrary integers + 1 forced-positive integer.
                return rc::gen::map(
                    rc::gen::pair(
                        rc::gen::container<std::vector<int>>(
                            n, rc::gen::inRange(-100, 101)),
                        rc::gen::inRange(1, 101)),
                    [](std::pair<std::vector<int>, int> p) {
                        p.first.push_back(p.second);
                        return p.first;
                    });
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
                "builtins.length (builtins.filter (x: x > 0) "
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
