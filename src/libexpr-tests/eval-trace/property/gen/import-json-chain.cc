#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── ImportJsonChainGen ──────────────────────────────────────────────
//
// Nests structured data inside an import chain — the pattern where a
// .nix file reads and parses JSON:
//
//   loader.nix:
//     let cfg = builtins.fromJSON (builtins.readFile <config.json>);
//     in "${cfg.name}-${builtins.readFile <version.txt>}"
//
//   Expression: import <loader.nix>
//
// This exercises BOTH import-chain FileBytes deps (for loader.nix itself)
// and structured data deps (for config.json) flowing through a transitive
// import.  No existing generator combines imports with structured data.
//
// Dep slots:
//   [0] Kind::JsonFile  (config.json)  — Result (cfg.name flows into result)
//   [1] Kind::File      (version.txt)  — Result (version string in result)
//   [2] Kind::File      (loader.nix)   — Result (the import target)
//
// ResultKind: String

rc::Gen<TestExpr> makeImportJsonChainGen()
{
    return rc::gen::map(
        rc::gen::container<std::string>(rc::gen::inRange('a', (char)('z' + 1))),
        [](std::string name) {
            if (name.empty()) name = "pkg";

            // config.json with a "name" key.
            nlohmann::json cfg = nlohmann::json::object();
            cfg["name"] = name;
            cfg["extra"] = 42;
            std::string cfgContent = cfg.dump();
            auto cfgHandle = std::make_shared<TempExtFile>("json", cfgContent);

            // version.txt — plain text.
            std::string versionContent = "1.0";
            auto versionHandle = std::make_shared<TempTextFile>(versionContent);

            // loader.nix — imports the JSON and version file.
            std::string loaderContent =
                "let cfg = builtins.fromJSON (builtins.readFile "
                + cfgHandle->path.string() + ");"
                " in \"${cfg.name}-${builtins.readFile "
                + versionHandle->path.string() + "}\"";
            auto loaderHandle = std::make_shared<TempExtFile>("nix", loaderContent);

            // Slots.
            DepSlot cfgSlot;
            cfgSlot.kind = DepSlot::Kind::JsonFile;
            cfgSlot.path = cfgHandle->path;
            cfgSlot.fileHandle = cfgHandle;
            cfgSlot.currentValue = cfgContent;
            cfgSlot.setOriginal(cfgContent);

            DepSlot versionSlot;
            versionSlot.kind = DepSlot::Kind::File;
            versionSlot.path = versionHandle->path;
            versionSlot.fileHandle = versionHandle;
            versionSlot.currentValue = versionContent;
            versionSlot.setOriginal(versionContent);

            DepSlot loaderSlot;
            loaderSlot.kind = DepSlot::Kind::File;
            loaderSlot.path = loaderHandle->path;
            loaderSlot.fileHandle = loaderHandle;
            loaderSlot.currentValue = loaderContent;
            loaderSlot.setOriginal(loaderContent);

            std::string nixCode = "import " + loaderHandle->path.string();

            std::vector<DepSlot> deps;
            deps.push_back(std::move(cfgSlot));
            deps.push_back(std::move(versionSlot));
            deps.push_back(std::move(loaderSlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
