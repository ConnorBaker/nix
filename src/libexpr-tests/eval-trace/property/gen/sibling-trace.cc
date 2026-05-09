#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── SiblingTraceGen ──────────────────────────────────────────────────
//
// Models a mkDerivation-style attrset where multiple attributes share a
// common JSON source.
//
// Expression:
//   let cfg = builtins.fromJSON (builtins.readFile <config.json>);
//   in { name = cfg.name; version = cfg.version;
//        combined = "${cfg.name}-${cfg.version}"; }.combined
//
// The result is the string "${name}-${version}".  Accessing .combined forces
// both cfg.name and cfg.version, creating two StructuredProjection deps on
// the same source file.  This exercises the case where sibling TracedExpr
// values share a single dep source — the dep recording must handle both
// projections correctly without duplication or omission.
//
// Generator construction:
//   1. Start with makeAccessibleJsonObjectGen() (strictly {String,Int,Bool,Null}
//      values) as the base for additional random keys.
//   2. Ensure the object has "name" and "version" as String-typed JsonValues.
//      If the base generator produced these keys with a non-string type, or
//      omitted them entirely, insert/overwrite with generated string values.
//   3. Insert 1–3 additional random keys (from makeNixIdentifierGen) with
//      integer values for precision-testing: these keys are present in the
//      JSON but never accessed by the expression.
//   4. Serialize to JSON, create a TempExtFile(".json", content).
//
// Dep slots: slot[0] = Kind::JsonFile.
// ResultKind: String (the combined "${name}-${version}" string).
//
// Wired into makeNixExprGen().

rc::Gen<TestExpr> makeSiblingTraceGen()
{
    return rc::gen::map(
        rc::gen::tuple(
            makeAccessibleJsonObjectGen(),
            // 1–3 extra key names for precision testing.
            rc::gen::mapcat(
                rc::gen::inRange<size_t>(1, 4),
                [](size_t n) {
                    return rc::gen::container<std::vector<std::string>>(
                        n, makeNixIdentifierGen());
                }),
            // String content for the "name" field (1–8 lowercase chars).
            rc::gen::mapcat(
                rc::gen::inRange<size_t>(1, 9),
                [](size_t n) {
                    return rc::gen::container<std::string>(
                        n, rc::gen::inRange('a', (char)('z' + 1)));
                }),
            // String content for the "version" field (1–4 digit chars).
            rc::gen::mapcat(
                rc::gen::inRange<size_t>(1, 5),
                [](size_t n) {
                    return rc::gen::container<std::string>(
                        n, rc::gen::inRange('0', (char)('9' + 1)));
                })),
        [](std::tuple<
               std::map<std::string, JsonValue>,
               std::vector<std::string>,
               std::string,
               std::string> tup) {
            auto & [obj, extraKeys, nameVal, versionVal] = tup;

            // Force "name" and "version" to String-typed JsonValues.
            obj["name"]    = JsonValue{.kind = JsonValue::Kind::String, .strVal = nameVal};
            obj["version"] = JsonValue{.kind = JsonValue::Kind::String, .strVal = versionVal};

            // Insert extra keys with integer values, skipping reserved names.
            int extraVal = 0;
            for (auto & k : extraKeys) {
                if (k != "name" && k != "version" && k != "combined")
                    obj.insert_or_assign(k, JsonValue{.kind = JsonValue::Kind::Int, .intVal = extraVal++});
            }

            // Serialize the full object to JSON.
            nlohmann::json json = nlohmann::json::object();
            for (auto & [k, v] : obj)
                json[k] = v.toJson();
            std::string jsonContent = json.dump();

            // Create a .json temp file with RAII lifetime in the DepSlot.
            auto handle = std::make_shared<TempExtFile>("json", jsonContent);

            DepSlot slot;
            slot.kind = DepSlot::Kind::JsonFile;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = jsonContent;
            slot.setOriginal(jsonContent);

            // Expression: let cfg = builtins.fromJSON (builtins.readFile <path>);
            // in { name = cfg.name; version = cfg.version;
            //      combined = "${cfg.name}-${cfg.version}"; }.combined
            //
            // Accessing .combined forces both cfg.name and cfg.version, creating
            // two StructuredProjection deps on the same JSON source file.
            std::string nixCode =
                "let cfg = builtins.fromJSON (builtins.readFile "
                + handle->path.string() + ");"
                " in { name = cfg.name; version = cfg.version;"
                " combined = \"${cfg.name}-${cfg.version}\"; }.combined";

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
