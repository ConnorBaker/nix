#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── ImportTreeGen ────────────────────────────────────────────────────
//
// Models nixpkgs' tree-shaped imports.  Tests transitive FileBytes dep
// propagation through a 3-file import chain:
//
//   entry.nix:
//     let data = builtins.readFile <data.txt>;
//         lib  = import <lib.nix>;
//     in lib data
//
//   lib.nix:
//     s: "processed: ${s}"
//
//   data.txt:  <random printable ASCII content>
//
// DepSlot ordering:
//   [0] Kind::File — data.txt (randomized content; the mutated dep)
//   [1] Kind::File — entry.nix (fixed structure)
//   [2] Kind::File — lib.nix   (fixed structure)
//
// ResultKind: String ("processed: " + data content).

rc::Gen<TestExpr> makeImportTreeGen()
{
    return rc::gen::map(
        rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
        [](std::string dataContent) {
            auto dataHandle  = std::make_shared<TempExtFile>("txt",  dataContent);
            auto libHandle   = std::make_shared<TempExtFile>("nix",
                R"(s: "processed: ${s}")");
            std::string entryContent =
                "let data = builtins.readFile " + dataHandle->path.string() + ";"
                " lib = import " + libHandle->path.string() + ";"
                " in lib data";
            auto entryHandle = std::make_shared<TempExtFile>("nix", entryContent);

            DepSlot slotData;
            slotData.kind = DepSlot::Kind::File;
            slotData.path = dataHandle->path;
            slotData.fileHandle = dataHandle;
            slotData.currentValue = dataContent;
            slotData.setOriginal(dataContent);

            // entry.nix and lib.nix are imported/parsed as Nix code;
            // mutating them with arbitrary bytes would break parsing.
            // NixSource constraint makes generateMutation append a
            // syntax-preserving comment line instead.
            DepSlot slotEntry;
            slotEntry.kind = DepSlot::Kind::File;
            slotEntry.contentConstraint = DepSlot::ContentConstraint::NixSource;
            slotEntry.path = entryHandle->path;
            slotEntry.fileHandle = entryHandle;
            slotEntry.currentValue = entryContent;
            slotEntry.setOriginal(entryContent);

            std::string libContent = R"(s: "processed: ${s}")";
            DepSlot slotLib;
            slotLib.kind = DepSlot::Kind::File;
            slotLib.contentConstraint = DepSlot::ContentConstraint::NixSource;
            slotLib.path = libHandle->path;
            slotLib.fileHandle = libHandle;
            slotLib.currentValue = libContent;
            slotLib.setOriginal(libContent);

            std::string nixCode = "import " + entryHandle->path.string();

            std::vector<DepSlot> deps;
            deps.push_back(std::move(slotData));
            deps.push_back(std::move(slotEntry));
            deps.push_back(std::move(slotLib));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
