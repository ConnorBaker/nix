#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── SelectiveAttrsetGen ─────────────────────────────────────────────
//
// Generates an attrset where different attributes have different dep kinds:
//
//   { plain = builtins.readFile <plain.txt>;
//     structured = (builtins.fromJSON (builtins.readFile <data.json>))."<key>";
//     env = builtins.getEnv "<var>"; }
//
// attrsToForce lists which attributes the test should force.
//
// When the test forces only "plain", only the FileBytes dep for plain.txt
// should appear.  When it forces "structured", the JSON file's FileBytes +
// StructuredProjection deps should appear.  This exercises the trace
// system's per-attribute laziness.
//
// For the property tests (which force ALL listed attrs), all three slots
// produce deps.
//
// Dep slots:
//   [0] Kind::File     (plain.txt)   — Result
//   [1] Kind::JsonFile (data.json)   — Result
//   [2] Kind::EnvVar   (env var)     — Result
//
// ResultKind: Attrset
// attrsToForce: {"plain", "structured", "env"}

static std::atomic<int> envCounter{0};

rc::Gen<TestExpr> makeSelectiveAttrsetGen()
{
    return rc::gen::mapcat(
        makeAccessibleJsonObjectGen(),
        [](std::map<std::string, JsonValue> obj) {
            std::vector<std::string> keys;
            keys.reserve(obj.size());
            for (auto & [k, _] : obj)
                keys.push_back(k);

            return rc::gen::map(
                rc::gen::tuple(
                    rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
                    rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
                    rc::gen::elementOf(std::move(keys))),
                [obj](std::tuple<std::string, std::string, std::string> tup) {
                    auto & [plainContent, envValue, chosenKey] = tup;

                    // Slot 0: plain file.
                    auto plainHandle = std::make_shared<TempTextFile>(plainContent);
                    DepSlot plainSlot;
                    plainSlot.kind = DepSlot::Kind::File;
                    plainSlot.path = plainHandle->path;
                    plainSlot.fileHandle = plainHandle;
                    plainSlot.currentValue = plainContent;
                    plainSlot.setOriginal(plainContent);

                    // Slot 1: JSON file.
                    nlohmann::json json = nlohmann::json::object();
                    for (auto & [k, v] : obj)
                        json[k] = v.toJson();
                    std::string jsonContent = json.dump();
                    auto jsonHandle = std::make_shared<TempExtFile>("json", jsonContent);
                    DepSlot jsonSlot;
                    jsonSlot.kind = DepSlot::Kind::JsonFile;
                    jsonSlot.path = jsonHandle->path;
                    jsonSlot.fileHandle = jsonHandle;
                    jsonSlot.currentValue = jsonContent;
                    jsonSlot.setOriginal(jsonContent);

                    // Slot 2: env var.
                    auto varName = "NIX_PROP_SEL_"
                        + std::to_string(getpid()) + "_"
                        + std::to_string(envCounter++);
                    auto guard = std::make_shared<ScopedEnvVar>(varName, envValue);
                    DepSlot envSlot;
                    envSlot.kind = DepSlot::Kind::EnvVar;
                    envSlot.envVarName = varName;
                    envSlot.envGuard = std::move(guard);
                    envSlot.currentValue = envValue;
                    envSlot.setOriginal(envValue);

                    std::string nixCode =
                        "{ plain = builtins.readFile " + plainHandle->path.string() + ";"
                        " structured = (builtins.fromJSON (builtins.readFile "
                        + jsonHandle->path.string() + ")).\"" + chosenKey + "\";"
                        " env = builtins.getEnv \"" + varName + "\"; }";

                    std::vector<DepSlot> deps;
                    deps.push_back(std::move(plainSlot));
                    deps.push_back(std::move(jsonSlot));
                    deps.push_back(std::move(envSlot));

                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = TestExpr::ResultKind::Attrset,
                        .depSlots = std::move(deps),
                        .attrsToForce = {"plain", "structured", "env"},
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
