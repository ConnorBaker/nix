#include "../expr-gen.hh"

#include <nlohmann/json.hpp>
#include <limits>

namespace nix::eval_trace::test::proptest {

// ── StringInterpolationFromJSONGen ───────────────────────────────────
//
// Generates: "result: ${toString (builtins.fromJSON (builtins.readFile <path>))."key"}"
// Kind::JsonFile (string-valued key), ResultKind::String.
// Tests provenance through toString coercion boundary + string interpolation.
// Restricted to string-valued keys to make toString straightforward (no float).

rc::Gen<TestExpr> makeStringInterpolationFromJSONGen()
{
    // Use a strict scalar object gen that only produces string and integer values
    // (no null, bool, float) so toString produces a predictable result in Nix.
    auto strictStrIntGen = rc::gen::oneOf(
        rc::gen::map(
            rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
            [](std::string s) -> JsonValue {
                return JsonValue{.kind = JsonValue::Kind::String, .strVal = std::move(s)};
            }),
        rc::gen::map(
            rc::gen::suchThat(rc::gen::arbitrary<int64_t>(),
                [](int64_t n) { return n != std::numeric_limits<int64_t>::min(); }),
            [](int64_t n) -> JsonValue {
                return JsonValue{.kind = JsonValue::Kind::Int, .intVal = n};
            })
    );

    auto objGen = rc::gen::mapcat(
        rc::gen::inRange<size_t>(1, 6),
        [strictStrIntGen](size_t n) {
            return rc::gen::map(
                rc::gen::container<std::vector<std::pair<std::string, JsonValue>>>(
                    n,
                    rc::gen::apply(
                        [](std::string k, JsonValue v) {
                            return std::make_pair(std::move(k), std::move(v));
                        },
                        makeNixIdentifierGen(),
                        strictStrIntGen)),
                [](std::vector<std::pair<std::string, JsonValue>> pairs)
                    -> std::map<std::string, JsonValue>
                {
                    std::map<std::string, JsonValue> result;
                    for (auto & [k, v] : pairs)
                        result.insert_or_assign(std::move(k), std::move(v));
                    return result;
                });
        });

    return rc::gen::mapcat(
        std::move(objGen),
        [](std::map<std::string, JsonValue> obj) {
            std::vector<std::string> keys;
            keys.reserve(obj.size());
            for (auto & [k, _] : obj)
                keys.push_back(k);

            return rc::gen::map(
                rc::gen::elementOf(std::move(keys)),
                [obj](std::string chosenKey) {
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

                    // Use builtins.getAttr inside the ${} interpolation to
                    // avoid the unescapable ."key" quoting problem in double-quoted
                    // Nix strings.  Generated keys may be reserved words (if, let,
                    // etc.) which also rules out unquoted .key access.
                    std::string nixCode =
                        "\"result: ${toString "
                        "(builtins.getAttr \"" + chosenKey + "\" "
                        "(builtins.fromJSON (builtins.readFile "
                        + handle->path.string() + ")))}\"";

                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = TestExpr::ResultKind::String,
                        .depSlots = {std::move(slot)},
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
