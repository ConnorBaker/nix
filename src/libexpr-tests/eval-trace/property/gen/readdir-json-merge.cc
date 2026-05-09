#include "../expr-gen.hh"

#include <nlohmann/json.hpp>

namespace nix::eval_trace::test::proptest {

// ── ReadDirJsonMergeGen ─────────────────────────────────────────────
//
// Combines readDir (DirectoryEntries) with structured JSON data:
//
//   let dir = builtins.readDir <dir>;
//       entry = dir."<filename>";               // directory entry type string
//       cfg = builtins.fromJSON (builtins.readFile <config.json>);
//       tag = cfg."tag";
//   in "${tag}:${entry}"
//
// Exercises DirectoryEntries + StructuredProjection deps flowing into
// one result.  No existing multi-source generator uses DirectoryEntries.
//
// Dep slots:
//   [0] Kind::DirectoryEntries (dir)          — Result
//   [1] Kind::JsonFile         (config.json)  — Result
//
// ResultKind: String

rc::Gen<TestExpr> makeReadDirJsonMergeGen()
{
    return rc::gen::map(
        rc::gen::container<std::string>(rc::gen::inRange('a', (char)('z' + 1))),
        [](std::string tag) {
            if (tag.empty()) tag = "t";

            // Directory with one file.
            auto dir = std::make_shared<TempDir>();
            std::string filename = "entry";
            dir->addFile(filename, "content");

            // JSON config.
            nlohmann::json cfg;
            cfg["tag"] = tag;
            cfg["extra"] = 0;
            std::string cfgContent = cfg.dump();
            auto cfgHandle = std::make_shared<TempExtFile>("json", cfgContent);

            // DirectoryEntries slot — mutation toggles the ACCESSED entry's
            // presence.  Removing "entry" causes the SC dep for that key
            // to fail → cache miss.  This is the soundness-relevant
            // mutation for DirectoryEntries.
            DepSlot dirSlot;
            dirSlot.kind = DepSlot::Kind::DirectoryEntries;
            dirSlot.path = dir->path();
            dirSlot.dirHandle = dir;
            dirSlot.dirEntryName = filename;
            dirSlot.currentValue = "exists";
            dirSlot.setOriginal("exists");

            DepSlot cfgSlot;
            cfgSlot.kind = DepSlot::Kind::JsonFile;
            cfgSlot.path = cfgHandle->path;
            cfgSlot.fileHandle = cfgHandle;
            cfgSlot.currentValue = cfgContent;
            cfgSlot.setOriginal(cfgContent);

            std::string nixCode =
                "let dir = builtins.readDir " + dir->path().string() + ";"
                " entry = dir.\"" + filename + "\";"
                " cfg = builtins.fromJSON (builtins.readFile "
                + cfgHandle->path.string() + ");"
                " tag = cfg.tag;"
                " in \"${tag}:${entry}\"";

            std::vector<DepSlot> deps;
            deps.push_back(std::move(dirSlot));
            deps.push_back(std::move(cfgSlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
