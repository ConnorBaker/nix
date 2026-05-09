#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── FunctionChainGen ────────────────────────────────────────────────
//
// Routes traced values through user-defined functions:
//
//   let transform = x: "transformed-${toString x}";
//       add1 = x: x + 1;
//       cfg = builtins.fromJSON (builtins.readFile <config.json>);
//       raw = builtins.readFile <data.txt>;
//       processed = transform cfg."value";
//       bumped = add1 cfg."count";
//   in "${processed}-${toString bumped}-${raw}"
//
// Tests that traced data deps propagate through lambda application.
// A bug that drops provenance during function calls would cause
// the JsonFile dep to disappear from the stored trace.
//
// Dep slots:
//   [0] Kind::JsonFile (config.json)  — Result
//   [1] Kind::File     (data.txt)     — Result
//
// ResultKind: String

rc::Gen<TestExpr> makeFunctionChainGen()
{
    return rc::gen::map(
        rc::gen::tuple(
            rc::gen::container<std::string>(rc::gen::inRange('a', (char)('z' + 1))),
            rc::gen::inRange(0, 100),
            rc::gen::container<std::string>(rc::gen::inRange('!', '~'))),
        [](std::tuple<std::string, int, std::string> tup) {
            auto & [strVal, intVal, rawContent] = tup;
            if (strVal.empty()) strVal = "v";
            if (rawContent.empty()) rawContent = "r";

            nlohmann::json cfg;
            cfg["value"] = strVal;
            cfg["count"] = intVal;
            std::string cfgContent = cfg.dump();
            auto cfgHandle = std::make_shared<TempExtFile>("json", cfgContent);

            auto rawHandle = std::make_shared<TempTextFile>(rawContent);

            DepSlot cfgSlot;
            cfgSlot.kind = DepSlot::Kind::JsonFile;
            cfgSlot.path = cfgHandle->path;
            cfgSlot.fileHandle = cfgHandle;
            cfgSlot.currentValue = cfgContent;
            cfgSlot.setOriginal(cfgContent);

            DepSlot rawSlot;
            rawSlot.kind = DepSlot::Kind::File;
            rawSlot.path = rawHandle->path;
            rawSlot.fileHandle = rawHandle;
            rawSlot.currentValue = rawContent;
            rawSlot.setOriginal(rawContent);

            std::string nixCode =
                "let transform = x: \"transformed-${toString x}\";"
                " add1 = x: x + 1;"
                " cfg = builtins.fromJSON (builtins.readFile "
                + cfgHandle->path.string() + ");"
                " raw = builtins.readFile " + rawHandle->path.string() + ";"
                " processed = transform cfg.value;"
                " bumped = add1 cfg.count;"
                " in \"${processed}-${toString bumped}-${raw}\"";

            std::vector<DepSlot> deps;
            deps.push_back(std::move(cfgSlot));
            deps.push_back(std::move(rawSlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
