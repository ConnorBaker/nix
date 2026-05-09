#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── FilterSortMultiGen ──────────────────────────────────────────────
//
// Chains filter and sort on a traced list from one source, then combines
// the length with a key from a second source:
//
//   let nums = builtins.fromJSON (builtins.readFile <nums.json>);
//       filtered = builtins.filter (x: x > 0) nums;
//       sorted = builtins.sort builtins.lessThan filtered;
//       len = builtins.length sorted;
//       cfg = builtins.fromJSON (builtins.readFile <cfg.json>);
//       tag = cfg."tag";
//   in "${tag}:${toString len}"
//
// Filter and sort both modify list shape, interacting with #len deps.
// This exercises shape-dep propagation through filter→sort→length combined
// with a structured access from a different source.
//
// Dep slots:
//   [0] Kind::JsonArray (nums.json) — Result
//   [1] Kind::JsonFile  (cfg.json)  — Result
//
// ResultKind: String

rc::Gen<TestExpr> makeFilterSortMultiGen()
{
    return rc::gen::map(
        rc::gen::tuple(
            rc::gen::mapcat(
                rc::gen::inRange<size_t>(2, 8),
                [](size_t n) {
                    return rc::gen::container<std::vector<int>>(
                        n, rc::gen::inRange(-50, 51));
                }),
            rc::gen::container<std::string>(rc::gen::inRange('a', (char)('z' + 1)))),
        [](std::tuple<std::vector<int>, std::string> tup) {
            auto & [nums, tag] = tup;
            if (tag.empty()) tag = "t";

            nlohmann::json arr = nlohmann::json::array();
            for (auto n : nums) arr.push_back(n);
            std::string numsContent = arr.dump();
            auto numsHandle = std::make_shared<TempExtFile>("json", numsContent);

            nlohmann::json cfg;
            cfg["tag"] = tag;
            std::string cfgContent = cfg.dump();
            auto cfgHandle = std::make_shared<TempExtFile>("json", cfgContent);

            DepSlot numsSlot;
            numsSlot.kind = DepSlot::Kind::JsonArray;
            numsSlot.path = numsHandle->path;
            numsSlot.fileHandle = numsHandle;
            numsSlot.currentValue = numsContent;
            numsSlot.setOriginal(numsContent);

            DepSlot cfgSlot;
            cfgSlot.kind = DepSlot::Kind::JsonFile;
            cfgSlot.path = cfgHandle->path;
            cfgSlot.fileHandle = cfgHandle;
            cfgSlot.currentValue = cfgContent;
            cfgSlot.setOriginal(cfgContent);

            std::string nixCode =
                "let nums = builtins.fromJSON (builtins.readFile "
                + numsHandle->path.string() + ");"
                " filtered = builtins.filter (x: x > 0) nums;"
                " sorted = builtins.sort builtins.lessThan filtered;"
                " len = builtins.length sorted;"
                " cfg = builtins.fromJSON (builtins.readFile "
                + cfgHandle->path.string() + ");"
                " tag = cfg.tag;"
                " in \"${tag}:${toString len}\"";

            std::vector<DepSlot> deps;
            deps.push_back(std::move(numsSlot));
            deps.push_back(std::move(cfgSlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
