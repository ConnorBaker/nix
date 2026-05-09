#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── ConcatStringsGen ─────────────────────────────────────────────────
//
// Generates: builtins.concatStringsSep ":" [(builtins.readFile <fileA>) (builtins.readFile <fileB>)]
// Two Kind::File slots, ResultKind::String.

rc::Gen<TestExpr> makeConcatStringsGen()
{
    return rc::gen::map(
        rc::gen::tuple(
            rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
            rc::gen::container<std::string>(rc::gen::inRange('!', '~'))),
        [](std::tuple<std::string, std::string> contents) {
            auto & [contentA, contentB] = contents;

            auto handleA = std::make_shared<TempTextFile>(contentA);
            auto handleB = std::make_shared<TempTextFile>(contentB);

            DepSlot slotA;
            slotA.kind = DepSlot::Kind::File;
            slotA.path = handleA->path;
            slotA.fileHandle = handleA;
            slotA.currentValue = contentA;
            slotA.setOriginal(contentA);

            DepSlot slotB;
            slotB.kind = DepSlot::Kind::File;
            slotB.path = handleB->path;
            slotB.fileHandle = handleB;
            slotB.currentValue = contentB;
            slotB.setOriginal(contentB);

            std::string nixCode =
                "builtins.concatStringsSep \":\" "
                "[(builtins.readFile " + handleA->path.string() + ") "
                "(builtins.readFile " + handleB->path.string() + ")]";

            std::vector<DepSlot> deps;
            deps.push_back(std::move(slotA));
            deps.push_back(std::move(slotB));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = std::move(deps),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
