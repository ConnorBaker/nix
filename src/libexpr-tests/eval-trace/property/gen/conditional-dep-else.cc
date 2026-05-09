#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── ConditionalDepElseBranchGen ─────────────────────────────────────
//
// Generates: if builtins.pathExists <guard.txt> then builtins.readFile <data.txt> else "default"
//
// Unlike makeConditionalDepGen() where the guard starts existing (then-branch),
// this generator starts with the guard file ABSENT so the else-branch is
// always taken on initial evaluation.
//
// Two dep slots:
//   slot[0] — Kind::FileExistence (guard.txt): starts MISSING.
//             Contribution: Result (ExistenceCheck always recorded).
//   slot[1] — Kind::File (data.txt): the file read in the then-branch.
//             Contribution: Absent (then-branch is never taken when guard
//             is missing, so data.txt is never read and no FileBytes dep
//             is recorded).
//
// This generator exercises:
//   1. Else-branch dep absence: data.txt should NOT appear in deps.
//   2. ExistenceCheck dep presence for absent files (fixed by the
//      recordDep ExistenceCheck bypass for absolute paths).
//   3. Guard-only invalidation: when the guard file is created
//      (missing→exists), the cache must miss.

rc::Gen<TestExpr> makeConditionalDepElseBranchGen()
{
    return rc::gen::map(
        rc::gen::container<std::string>(rc::gen::inRange('!', '~')),  // data file content
        [](std::string dataContent) {
            // Guard file: starts ABSENT. We create a TempExtFile then delete
            // it immediately so we have a valid path that does not exist.
            auto guardHandle = std::make_shared<TempExtFile>("txt", "");
            std::filesystem::remove(guardHandle->path);

            // Data file: exists with random content, but should never be
            // read because the guard is absent.
            auto dataHandle = std::make_shared<TempTextFile>(dataContent);

            // slot[0]: FileExistence for guard.txt — starts missing.
            // Contribution: Result (ExistenceCheck dep is always recorded
            // regardless of file existence).
            DepSlot guardSlot;
            guardSlot.kind = DepSlot::Kind::FileExistence;
            guardSlot.path = guardHandle->path;
            guardSlot.fileHandle = guardHandle;
            guardSlot.currentValue = "missing";
            guardSlot.setOriginal("missing");

            // slot[1]: File for data.txt — Absent because the then-branch
            // is not taken when the guard is missing.
            DepSlot dataSlot;
            dataSlot.kind = DepSlot::Kind::File;
            dataSlot.contribution = DepSlot::Contribution::Absent;
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
