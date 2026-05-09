#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── StringInterpolationGen ───────────────────────────────────────────
//
// Generates: "prefix-${builtins.readFile <file>}-suffix"
// depSlots: one Kind::File
// ResultKind: String

rc::Gen<TestExpr> makeStringInterpolationGen()
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
                "\"prefix-${builtins.readFile "
                + handle->path.string() + "}-suffix\"";

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
