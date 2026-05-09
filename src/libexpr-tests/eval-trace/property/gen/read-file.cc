#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── ReadFileGen ──────────────────────────────────────────────────────

rc::Gen<TestExpr> makeReadFileGen()
{
    return rc::gen::map(
        rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
        [](std::string content) {
            // Create file and retain RAII handle in the DepSlot.
            // Use printable ASCII content to avoid invalid UTF-8 in file paths
            // and to keep the generated file content round-trippable through
            // builtins.readFile without encoding surprises.
            auto handle = std::make_shared<TempTextFile>(content);
            DepSlot slot;
            slot.kind = DepSlot::Kind::File;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = content;
            slot.setOriginal(content);

            return TestExpr{
                .nixCode = "builtins.readFile " + handle->path.string(),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
