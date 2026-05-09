#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── PathExistsGen ────────────────────────────────────────────────────

rc::Gen<TestExpr> makePathExistsGen()
{
    // Map over a dummy bool so that each generated value creates a fresh
    // TempExtFile (unique path).  The file always starts as existing.
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

            return TestExpr{
                .nixCode = "builtins.pathExists " + handle->path.string(),
                .expectedKind = TestExpr::ResultKind::Bool,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
