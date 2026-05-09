#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── ErrorGen ─────────────────────────────────────────────────────────
//
// Generates expressions that are expected to throw during evaluation.
// expectsSuccess() returns false for all generated TestExprs.
// Do NOT include in makeNixExprGen — use only in dedicated error-path tests
// that catch exceptions manually (error-path.cc).
//
// Two variants:
//   (1) Missing key: (builtins.fromJSON (builtins.readFile <json>))."nonexistent_key_XXXX"
//       The JSON object never contains any key starting with "nonexistent_key_",
//       so the attribute access always throws.
//   (2) Invalid JSON: builtins.fromJSON "not{valid json"
//       The literal string is not valid JSON — fromJSON always throws.

rc::Gen<TestExpr> makeErrorGen()
{
    auto missingKeyGen = rc::gen::map(
        rc::gen::tuple(makeJsonObjectGen(), rc::gen::inRange(0, 100000)),
        [](std::tuple<std::map<std::string, JsonValue>, int> t) {
            auto & [obj, suffix] = t;
            // Serialize object to JSON
            nlohmann::json json = nlohmann::json::object();
            for (auto & [k, v] : obj)
                json[k] = v.toJson();
            std::string jsonContent = json.dump();

            auto handle = std::make_shared<TempExtFile>("json", jsonContent);

            DepSlot slot;
            slot.kind = DepSlot::Kind::JsonFile;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = jsonContent;
            slot.setOriginal(jsonContent);

            // Key guaranteed absent: "nonexistent_key_XXXX"
            std::string absentKey = "nonexistent_key_" + std::to_string(suffix);
            std::string nixCode =
                "(builtins.fromJSON (builtins.readFile "
                + handle->path.string() + ")).\"" + absentKey + "\"";

            TestExpr expr;
            expr.nixCode = std::move(nixCode);
            expr.expectedKind = TestExpr::ResultKind::Null;  // never reached
            expr.depSlots = {std::move(slot)};
            expr.expectsSuccess_ = false;
            return expr;
        });

    auto invalidJsonGen = rc::gen::just([] {
        TestExpr expr;
        expr.nixCode = R"(builtins.fromJSON "not{valid json")";
        expr.expectedKind = TestExpr::ResultKind::Null;  // never reached
        expr.depSlots = {};
        expr.expectsSuccess_ = false;
        return expr;
    }());

    return rc::gen::oneOf(std::move(missingKeyGen), std::move(invalidJsonGen));
}

} // namespace nix::eval_trace::test::proptest
