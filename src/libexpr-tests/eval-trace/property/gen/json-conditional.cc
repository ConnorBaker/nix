#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── JsonConditionalGen ──────────────────────────────────────────────
//
// Chains structured data through a conditional:
//
//   let cfg = builtins.fromJSON (builtins.readFile <config.json>);
//   in if cfg.enabled then builtins.readFile <data.txt> else "disabled"
//
// config.json always has {"enabled": true, ...} so the then-branch is
// always taken.  This exercises BOTH structured dep (cfg.enabled access)
// and plain file dep (data.txt read) flowing into the result, with the
// structured dep gating the control flow.
//
// Dep slots:
//   [0] Kind::JsonFile  (config.json)  — Result (cfg.enabled determines branch)
//   [1] Kind::File      (data.txt)     — Result (content is the result when enabled)
//
// ResultKind: String

rc::Gen<TestExpr> makeJsonConditionalGen()
{
    return rc::gen::map(
        rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
        [](std::string dataContent) {
            // config.json: enabled=true plus some extra keys.
            nlohmann::json cfg = nlohmann::json::object();
            cfg["enabled"] = true;
            cfg["version"] = 1;
            std::string cfgContent = cfg.dump();

            auto cfgHandle = std::make_shared<TempExtFile>("json", cfgContent);
            DepSlot cfgSlot;
            cfgSlot.kind = DepSlot::Kind::JsonFile;
            cfgSlot.path = cfgHandle->path;
            cfgSlot.fileHandle = cfgHandle;
            cfgSlot.currentValue = cfgContent;
            cfgSlot.setOriginal(cfgContent);

            auto dataHandle = std::make_shared<TempTextFile>(dataContent);
            DepSlot dataSlot;
            dataSlot.kind = DepSlot::Kind::File;
            dataSlot.path = dataHandle->path;
            dataSlot.fileHandle = dataHandle;
            dataSlot.currentValue = dataContent;
            dataSlot.setOriginal(dataContent);

            std::string nixCode =
                "let cfg = builtins.fromJSON (builtins.readFile "
                + cfgHandle->path.string() + ");"
                " in if cfg.enabled then builtins.readFile "
                + dataHandle->path.string() + " else \"disabled\"";

            std::vector<DepSlot> deps;
            deps.push_back(std::move(cfgSlot));
            deps.push_back(std::move(dataSlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
