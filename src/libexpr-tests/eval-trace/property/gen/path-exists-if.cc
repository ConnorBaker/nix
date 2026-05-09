#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── PathExistsIfGen ──────────────────────────────────────────────────
//
// Generates: if builtins.pathExists <file> then "yes" else "no"
// Kind::FileExistence, ResultKind::String.
// The canonical pathExists usage pattern with an if-then-else wrapper.

rc::Gen<TestExpr> makePathExistsIfGen()
{
    return rc::gen::map(
        rc::gen::arbitrary<bool>(),
        [](bool) {
            auto handle = std::make_shared<TempExtFile>("txt", "");

            DepSlot slot;
            slot.kind = DepSlot::Kind::FileExistence;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = "exists";
            slot.setOriginal("exists");

            std::string nixCode =
                "if builtins.pathExists " + handle->path.string()
                + " then \"yes\" else \"no\"";

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
