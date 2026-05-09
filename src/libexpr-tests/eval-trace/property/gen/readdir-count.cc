#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── ReadDirCountGen ─────────────────────────────────────────────────
//
// Generates: toString (length (attrNames (builtins.readDir <dir>)))
//
// The expression counts directory entries. Mutation toggles an entry's
// presence, changing the count. The expression always succeeds because
// attrNames/length work on any attrset — even empty.
//
// This is the DirectoryEntries generator compatible with Test 12
// (CrossSession_MultiSlotInvalidation): mutation changes the result
// without causing eval errors, and restore recovers the original.
//
// Dep slots:
//   [0] Kind::DirectoryEntries (dir) — Result, mutation toggles entry.
//   [1] Kind::File (data.txt)        — Result, plain file read appended
//                                      to the count for multi-slot coverage.
//
// ResultKind: String (toString produces a string)

rc::Gen<TestExpr> makeReadDirCountGen()
{
    return rc::gen::map(
        rc::gen::container<std::string>(rc::gen::inRange('a', (char)('z' + 1))),
        [](std::string dataContent) {
            if (dataContent.empty()) dataContent = "x";

            // Directory with two files initially.
            auto dir = std::make_shared<TempDir>();
            dir->addFile("existing1", "c1");
            dir->addFile("existing2", "c2");

            // The mutation entry — starts absent, mutation adds it.
            std::string toggleEntry = "toggleme";

            // Plain data file for multi-slot coverage.
            auto dataHandle = std::make_shared<TempTextFile>(dataContent);

            // Slot 0: DirectoryEntries — mutation adds/removes toggleEntry.
            DepSlot dirSlot;
            dirSlot.kind = DepSlot::Kind::DirectoryEntries;
            dirSlot.path = dir->path();
            dirSlot.dirHandle = dir;
            dirSlot.dirEntryName = toggleEntry;
            dirSlot.currentValue = "missing";
            dirSlot.setOriginal("missing");

            // Slot 1: plain File — for multi-slot coverage in Test 12.
            DepSlot fileSlot;
            fileSlot.kind = DepSlot::Kind::File;
            fileSlot.path = dataHandle->path;
            fileSlot.fileHandle = dataHandle;
            fileSlot.currentValue = dataContent;
            fileSlot.setOriginal(dataContent);

            // Expression: count dir entries + append file content.
            // toString (length (attrNames (readDir dir))) + "-" + readFile data
            std::string nixCode =
                "toString (builtins.length (builtins.attrNames (builtins.readDir "
                + dir->path().string() + "))) + \"-\" + builtins.readFile "
                + dataHandle->path.string();

            std::vector<DepSlot> deps;
            deps.push_back(std::move(dirSlot));
            deps.push_back(std::move(fileSlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
