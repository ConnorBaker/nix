#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── RecAttrsetGen ────────────────────────────────────────────────────
//
// Generates: (rec { val = builtins.readFile <file>;
//                   name = "pkg-${val}";
//                   qualified = "${name}-release"; }).qualified
//
// Models a package derivation rec { ... } with self-referential bindings:
//   qualified → name → val → file (two levels of self-reference).
// The file contains random printable ASCII and is the single dep.
//
// Dep slots: slot[0] = Kind::File.
// ResultKind: String.
//
// The ${val} and ${name} below are Nix string interpolations — literal
// characters in the C++ string that the Nix parser interprets as
// interpolation inside the rec attrset's string literals.

rc::Gen<TestExpr> makeRecAttrsetGen()
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
                "(rec { val = builtins.readFile " + handle->path.string() + ";"
                " name = \"pkg-${val}\";"
                " qualified = \"${name}-release\"; }).qualified";

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::String,
                .depSlots = {std::move(slot)},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
