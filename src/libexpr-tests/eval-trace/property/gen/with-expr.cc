#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── WithExprGen ──────────────────────────────────────────────────────
//
// Generates: with builtins; readFile <file>
// depSlots: one Kind::File (same as ReadFileGen but uses `with` scope)
// ResultKind: String

rc::Gen<TestExpr> makeWithExprGen()
{
    return rc::gen::map(
        rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
        [](std::string content) {
            auto handle = std::make_shared<TempTextFile>(content);

            DepSlot slot;
            slot.kind = DepSlot::Kind::File;
            slot.path = handle->path;
            slot.fileHandle = handle;
            slot.currentValue = content;
            slot.setOriginal(content);

            std::string nixCode =
                "with builtins; readFile " + handle->path.string();

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
