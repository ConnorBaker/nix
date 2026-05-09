#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── FoldMergePipelineGen ────────────────────────────────────────────
//
// Reads a JSON array, folds over it, then merges the result with a
// structured data access and a plain file read:
//
//   let nums = builtins.fromJSON (builtins.readFile <nums.json>);     // JSON array
//       sum  = builtins.foldl' (a: b: a + b) 0 nums;                 // foldl' over list
//       cfg  = builtins.fromJSON (builtins.readFile <cfg.json>);      // JSON object
//       tag  = cfg."tag";                                              // structured access
//       sep  = builtins.readFile <sep.txt>;                           // plain file
//   in "${tag}${sep}${toString sum}"                                  // combine all
//
// Exercises: foldl' consuming a traced list + structured key access +
// plain file read, all combined via string interpolation.
//
// Dep slots:
//   [0] Kind::JsonArray (nums.json)  — Result (array content flows through foldl' into sum)
//   [1] Kind::JsonFile  (cfg.json)   — Result (tag key flows into result)
//   [2] Kind::File      (sep.txt)    — Result (separator string in result)
//
// ResultKind: String

rc::Gen<TestExpr> makeFoldMergePipelineGen()
{
    return rc::gen::map(
        rc::gen::tuple(
            rc::gen::mapcat(
                rc::gen::inRange<size_t>(1, 6),
                [](size_t n) {
                    return rc::gen::container<std::vector<int>>(
                        n, rc::gen::inRange(0, 100));
                }),
            rc::gen::container<std::string>(
                rc::gen::inRange('a', (char)('z' + 1))),
            rc::gen::container<std::string>(
                rc::gen::inRange('!', '~'))),
        [](std::tuple<std::vector<int>, std::string, std::string> tup) {
            auto & [nums, tag, sep] = tup;
            if (tag.empty()) tag = "v";
            if (sep.empty()) sep = "-";

            // nums.json
            nlohmann::json arr = nlohmann::json::array();
            for (auto n : nums) arr.push_back(n);
            std::string numsContent = arr.dump();
            auto numsHandle = std::make_shared<TempExtFile>("json", numsContent);

            // cfg.json
            nlohmann::json cfg;
            cfg["tag"] = tag;
            cfg["extra"] = 0;
            std::string cfgContent = cfg.dump();
            auto cfgHandle = std::make_shared<TempExtFile>("json", cfgContent);

            // sep.txt
            auto sepHandle = std::make_shared<TempTextFile>(sep);

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

            DepSlot sepSlot;
            sepSlot.kind = DepSlot::Kind::File;
            sepSlot.path = sepHandle->path;
            sepSlot.fileHandle = sepHandle;
            sepSlot.currentValue = sep;
            sepSlot.setOriginal(sep);

            std::string nixCode =
                "let nums = builtins.fromJSON (builtins.readFile "
                + numsHandle->path.string() + ");"
                " sum = builtins.foldl' (a: b: a + b) 0 nums;"
                " cfg = builtins.fromJSON (builtins.readFile "
                + cfgHandle->path.string() + ");"
                " tag = cfg.tag;"
                " sep = builtins.readFile " + sepHandle->path.string() + ";"
                " in \"${tag}${sep}${toString sum}\"";

            std::vector<DepSlot> deps;
            deps.push_back(std::move(numsSlot));
            deps.push_back(std::move(cfgSlot));
            deps.push_back(std::move(sepSlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
