#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── FromJSONGen ──────────────────────────────────────────────────────

rc::Gen<TestExpr> makeFromJSONGen()
{
    return rc::gen::map(
        makeJsonObjectGen(),
        [](std::map<std::string, JsonValue> obj) {
            // Serialize to JSON string
            nlohmann::json json = nlohmann::json::object();
            for (auto & [k, v] : obj)
                json[k] = v.toJson();
            std::string jsonContent = json.dump();

            // Create a .json temp file with RAII lifetime in the DepSlot.
            // Use Kind::JsonFile so that generateMutation() produces valid JSON
            // instead of arbitrary ASCII.  invalidation.cc's guard already covers
            // Kind::JsonFile for invalidateFileCache calls (file-backed slots
            // require file cache invalidation after mutation).
            auto handle = std::make_shared<TempExtFile>("json", jsonContent);

            DepSlot slot;
            slot.kind = DepSlot::Kind::JsonFile;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = jsonContent;
            slot.setOriginal(jsonContent);

            // Expression: builtins.fromJSON (builtins.readFile <path>)
            std::string nixCode =
                "builtins.fromJSON (builtins.readFile " + handle->path.string() + ")";

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::Attrset,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
