#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── ConditionalDepGen ────────────────────────────────────────────────
//
// Generates: if builtins.pathExists <guard.txt> then builtins.readFile <data.txt> else "default"
//
// This models the NixOS pattern:
//   if pathExists ./config then import ./config else {}
//
// Two dep slots are created:
//   slot[0] — Kind::FileExistence (guard.txt): the path-existence guard.
//             The file starts existing (currentValue = "exists").
//   slot[1] — Kind::File (data.txt): the file read in the then-branch.
//             Content is random printable ASCII.
//
// ResultKind: String (both branches produce strings — the file content or "default").
//
// This generator exercises:
//   1. Dep soundness: mutating data.txt invalidates when the guard exists.
//   2. Guard deletion: deleting guard.txt (exists→missing) changes the branch.
//   3. Precision under lazy evaluation: when the guard is absent, data.txt is
//      NOT read, so the cache must not depend on it.  This is the "lazy dep"
//      property — the then-branch deps are only recorded when the branch is taken.

rc::Gen<TestExpr> makeConditionalDepGen()
{
    return rc::gen::map(
        rc::gen::container<std::string>(rc::gen::inRange('!', '~')),  // data file content
        [](std::string dataContent) {
            // Guard file: starts existing (empty content — pathExists only checks
            // the path, not the content).
            auto guardHandle = std::make_shared<TempExtFile>("txt", "");

            // Data file: random printable ASCII content.
            auto dataHandle = std::make_shared<TempTextFile>(dataContent);

            // slot[0]: FileExistence for guard.txt (Result: always a dep —
            // determines which branch is taken)
            DepSlot guardSlot;
            guardSlot.kind = DepSlot::Kind::FileExistence;
            guardSlot.path = guardHandle->path;
            guardSlot.fileHandle = guardHandle;
            guardSlot.currentValue = "exists";
            guardSlot.setOriginal("exists");

            // slot[1]: File for data.txt (Conditional: only flows into result
            // when the guard file exists and the then-branch is taken)
            DepSlot dataSlot;
            dataSlot.kind = DepSlot::Kind::File;
            dataSlot.contribution = DepSlot::Contribution::Conditional;
            dataSlot.path = dataHandle->path;
            dataSlot.fileHandle = dataHandle;
            dataSlot.currentValue = dataContent;
            dataSlot.setOriginal(dataContent);

            std::string nixCode =
                "if builtins.pathExists " + guardHandle->path.string()
                + " then builtins.readFile " + dataHandle->path.string()
                + " else \"default\"";

            std::vector<DepSlot> deps;
            deps.push_back(std::move(guardSlot));
            deps.push_back(std::move(dataSlot));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
