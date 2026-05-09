#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── NestedImportPipelineGen ─────────────────────────────────────────
//
// 4-file import chain with structured data at every level:
//
//   entry.nix:
//     let base = import <lib.nix>;
//         cfg  = builtins.fromJSON (builtins.readFile <config.json>);
//         ver  = builtins.readFile <version.txt>;
//     in base cfg."name" ver
//
//   lib.nix:
//     name: ver: "${name}-${ver}"
//
//   config.json: {"name": "<random>", "extra": 0}
//   version.txt: <random>
//
// The import chain crosses 2 .nix files.  The result depends on
// a structured data access (cfg.name) and a plain file read (version.txt)
// flowing through a function imported from lib.nix.  All 4 files
// contribute to the result.
//
// Dep slots:
//   [0] Kind::JsonFile (config.json)  — Result (cfg.name in result)
//   [1] Kind::File     (version.txt)  — Result (version string in result)
//   [2] Kind::File     (lib.nix)      — Result (function body)
//   [3] Kind::File     (entry.nix)    — Result (import target)
//
// ResultKind: String

rc::Gen<TestExpr> makeNestedImportPipelineGen()
{
    return rc::gen::map(
        rc::gen::tuple(
            rc::gen::container<std::string>(
                rc::gen::inRange('a', (char)('z' + 1))),
            rc::gen::container<std::string>(
                rc::gen::inRange('0', (char)('9' + 1)))),
        [](std::tuple<std::string, std::string> tup) {
            auto & [name, version] = tup;
            if (name.empty()) name = "pkg";
            if (version.empty()) version = "0";

            // config.json
            nlohmann::json cfg;
            cfg["name"] = name;
            cfg["extra"] = 0;
            std::string cfgContent = cfg.dump();
            auto cfgHandle = std::make_shared<TempExtFile>("json", cfgContent);

            // version.txt
            auto verHandle = std::make_shared<TempTextFile>(version);

            // lib.nix — curried function
            auto libHandle = std::make_shared<TempExtFile>("nix",
                R"(name: ver: "${name}-${ver}")");

            // entry.nix — imports lib, reads config + version
            std::string entryContent =
                "let base = import " + libHandle->path.string() + ";"
                " cfg = builtins.fromJSON (builtins.readFile "
                + cfgHandle->path.string() + ");"
                " ver = builtins.readFile " + verHandle->path.string() + ";"
                " in base cfg.name ver";
            auto entryHandle = std::make_shared<TempExtFile>("nix", entryContent);

            DepSlot cfgSlot;
            cfgSlot.kind = DepSlot::Kind::JsonFile;
            cfgSlot.path = cfgHandle->path;
            cfgSlot.fileHandle = cfgHandle;
            cfgSlot.currentValue = cfgContent;
            cfgSlot.setOriginal(cfgContent);

            DepSlot verSlot;
            verSlot.kind = DepSlot::Kind::File;
            verSlot.path = verHandle->path;
            verSlot.fileHandle = verHandle;
            verSlot.currentValue = version;
            verSlot.setOriginal(version);

            // lib.nix and entry.nix are parsed as Nix code; mutation
            // must preserve syntax.  NixSource constraint makes
            // generateMutation append a trailing comment rather than
            // replace the file with random bytes.
            std::string libContent = R"(name: ver: "${name}-${ver}")";
            DepSlot libSlot;
            libSlot.kind = DepSlot::Kind::File;
            libSlot.contentConstraint = DepSlot::ContentConstraint::NixSource;
            libSlot.path = libHandle->path;
            libSlot.fileHandle = libHandle;
            libSlot.currentValue = libContent;
            libSlot.setOriginal(libContent);

            DepSlot entrySlot;
            entrySlot.kind = DepSlot::Kind::File;
            entrySlot.contentConstraint = DepSlot::ContentConstraint::NixSource;
            entrySlot.path = entryHandle->path;
            entrySlot.fileHandle = entryHandle;
            entrySlot.currentValue = entryContent;
            entrySlot.setOriginal(entryContent);

            std::string nixCode = "import " + entryHandle->path.string();

            std::vector<DepSlot> deps;
            deps.push_back(std::move(cfgSlot));
            deps.push_back(std::move(verSlot));
            deps.push_back(std::move(libSlot));
            deps.push_back(std::move(entrySlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
