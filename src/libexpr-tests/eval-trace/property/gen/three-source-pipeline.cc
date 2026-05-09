#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── ThreeSourcePipelineGen ──────────────────────────────────────────
//
// 3-source expression that reads from a JSON config, a plain data file,
// and a TOML config, applies primops at each stage, and combines into
// a single result string:
//
//   let cfg   = builtins.fromJSON (builtins.readFile <config.json>);
//       toml  = builtins.fromTOML (builtins.readFile <settings.toml>);
//       data  = builtins.readFile <data.txt>;
//       name  = cfg."name";                              // structured access
//       count = toString toml."count";                   // structured access
//       upper = builtins.replaceStrings ["a"] ["A"] data;// primop on plain
//   in "${name}-${count}-${upper}"
//
// Exercises dep tracking through:
//   - JSON structured projection (cfg.name)
//   - TOML structured projection (toml.count)
//   - replaceStrings on plain file content
//   - string interpolation combining all three
//
// Dep slots:
//   [0] Kind::JsonFile (config.json)   — Result
//   [1] Kind::TomlFile (settings.toml) — Result
//   [2] Kind::File     (data.txt)      — Result
//
// ResultKind: String

rc::Gen<TestExpr> makeThreeSourcePipelineGen()
{
    return rc::gen::map(
        rc::gen::tuple(
            rc::gen::container<std::string>(
                rc::gen::inRange('a', (char)('z' + 1))),
            rc::gen::inRange(1, 1000),
            rc::gen::container<std::string>(
                rc::gen::inRange('a', (char)('z' + 1)))),
        [](std::tuple<std::string, int, std::string> tup) {
            auto & [name, count, data] = tup;
            if (name.empty()) name = "pkg";
            if (data.empty()) data = "hello";

            // config.json
            nlohmann::json cfg;
            cfg["name"] = name;
            cfg["unused"] = true;
            std::string cfgContent = cfg.dump();
            auto cfgHandle = std::make_shared<TempExtFile>("json", cfgContent);

            // settings.toml
            std::string tomlContent = "count = " + std::to_string(count) + "\nextra = \"x\"\n";
            auto tomlHandle = std::make_shared<TempExtFile>("toml", tomlContent);

            // data.txt
            auto dataHandle = std::make_shared<TempTextFile>(data);

            DepSlot cfgSlot;
            cfgSlot.kind = DepSlot::Kind::JsonFile;
            cfgSlot.path = cfgHandle->path;
            cfgSlot.fileHandle = cfgHandle;
            cfgSlot.currentValue = cfgContent;
            cfgSlot.setOriginal(cfgContent);

            DepSlot tomlSlot;
            tomlSlot.kind = DepSlot::Kind::TomlFile;
            tomlSlot.path = tomlHandle->path;
            tomlSlot.fileHandle = tomlHandle;
            tomlSlot.currentValue = tomlContent;
            tomlSlot.setOriginal(tomlContent);

            DepSlot dataSlot;
            dataSlot.kind = DepSlot::Kind::File;
            dataSlot.path = dataHandle->path;
            dataSlot.fileHandle = dataHandle;
            dataSlot.currentValue = data;
            dataSlot.setOriginal(data);

            std::string nixCode =
                "let cfg = builtins.fromJSON (builtins.readFile "
                + cfgHandle->path.string() + ");"
                " toml = builtins.fromTOML (builtins.readFile "
                + tomlHandle->path.string() + ");"
                " data = builtins.readFile " + dataHandle->path.string() + ";"
                " name = cfg.name;"
                " count = toString toml.count;"
                " upper = builtins.replaceStrings [\"a\"] [\"A\"] data;"
                " in \"${name}-${count}-${upper}\"";

            std::vector<DepSlot> deps;
            deps.push_back(std::move(cfgSlot));
            deps.push_back(std::move(tomlSlot));
            deps.push_back(std::move(dataSlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
