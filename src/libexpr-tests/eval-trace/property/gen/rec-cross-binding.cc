#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── RecCrossBindingGen ──────────────────────────────────────────────
//
// Recursive attrset where one binding references another, and both
// originate from different traced sources:
//
//   let cfg = builtins.fromJSON (builtins.readFile <config.json>);
//       ver = builtins.readFile <version.txt>;
//   in rec {
//     name = cfg."name";
//     version = ver;
//     qualified = "${name}-${version}";
//     upper = builtins.replaceStrings ["a" "e" "i" "o" "u"] ["A" "E" "I" "O" "U"] qualified;
//   }.upper
//
// `rec` creates bindings where `qualified` references `name` and `version`,
// and `upper` references `qualified`. The result `.upper` depends on both
// the structured data source (cfg.name) and the plain file (version.txt)
// through cross-binding references within a recursive attrset.
//
// Dep slots:
//   [0] Kind::JsonFile (config.json) — Result
//   [1] Kind::File     (version.txt) — Result
//
// ResultKind: String

rc::Gen<TestExpr> makeRecCrossBindingGen()
{
    return rc::gen::map(
        rc::gen::tuple(
            rc::gen::container<std::string>(rc::gen::inRange('a', (char)('z' + 1))),
            rc::gen::container<std::string>(rc::gen::inRange('0', (char)('9' + 1)))),
        [](std::tuple<std::string, std::string> tup) {
            auto & [name, version] = tup;
            if (name.empty()) name = "pkg";
            if (version.empty()) version = "0";

            nlohmann::json cfg;
            cfg["name"] = name;
            cfg["extra"] = 0;
            std::string cfgContent = cfg.dump();
            auto cfgHandle = std::make_shared<TempExtFile>("json", cfgContent);

            auto verHandle = std::make_shared<TempTextFile>(version);

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

            std::string nixCode =
                "let cfg = builtins.fromJSON (builtins.readFile "
                + cfgHandle->path.string() + ");"
                " ver = builtins.readFile " + verHandle->path.string() + ";"
                " in rec {"
                " name = cfg.name;"
                " version = ver;"
                " qualified = \"${name}-${version}\";"
                " upper = builtins.replaceStrings"
                " [\"a\" \"e\" \"i\" \"o\" \"u\"]"
                " [\"A\" \"E\" \"I\" \"O\" \"U\"]"
                " qualified;"
                " }.upper";

            std::vector<DepSlot> deps;
            deps.push_back(std::move(cfgSlot));
            deps.push_back(std::move(verSlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
