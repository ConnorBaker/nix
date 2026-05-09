#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── OverlayGen ──────────────────────────────────────────────────────
//
// Models a simplified nixpkgs overlay (no fixpoint — self is null):
//
//   let base   = builtins.fromJSON (builtins.readFile <base.json>);
//       overlay = self: super: { foo = super.bar + 1; baz = super.baz; };
//       result  = overlay null base;
//   in result.foo
//
// The JSON always has "bar" (integer) and "baz" (integer) keys, plus 0–3
// additional random integer keys (from makeAccessibleJsonObjectGen with
// bar/baz injected).  The result is base.bar + 1 (an integer).
//
// DepSlot ordering:
//   slot[0] — Kind::JsonFile: the base JSON file.
//
// ResultKind: Int (super.bar + 1 is always an integer).
//
// This generator exercises key-level dep propagation through a function
// application: the SC dep on base.bar must propagate through overlay→result.foo.
// Mutating bar invalidates; mutating baz or extra keys does not.

rc::Gen<TestExpr> makeOverlayGen()
{
    // Use makeAccessibleJsonObjectGen to build a base object that has strictly
    // scalar values, then inject/override "bar" and "baz" as integer JsonValues.
    return rc::gen::mapcat(
        makeAccessibleJsonObjectGen(),
        [](std::map<std::string, JsonValue> base) {
            // Generate integer values for bar and baz.
            return rc::gen::map(
                rc::gen::tuple(
                    rc::gen::inRange<int64_t>(-1000, 1001),
                    rc::gen::inRange<int64_t>(-1000, 1001)),
                [base](std::tuple<int64_t, int64_t> barBaz) {
                    auto [barVal, bazVal] = barBaz;

                    // Build the object: start with random base, then inject
                    // "bar" and "baz" as integers, overriding any existing values.
                    auto obj = base;
                    obj.insert_or_assign("bar", JsonValue{.kind = JsonValue::Kind::Int, .intVal = barVal});
                    obj.insert_or_assign("baz", JsonValue{.kind = JsonValue::Kind::Int, .intVal = bazVal});

                    // Serialize to JSON.
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

                    // Expression: simplified overlay (self unused, no fixpoint).
                    // Build as concatenated string to keep Nix lambda syntax correct.
                    std::string nixCode =
                        "let base = builtins.fromJSON (builtins.readFile " + handle->path.string() + ");"
                        " overlay = self: super: { foo = super.bar + 1; baz = super.baz; };"
                        " result = overlay null base;"
                        " in result.foo";

                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = TestExpr::ResultKind::Int,
                        .depSlots = {std::move(slot)},
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
