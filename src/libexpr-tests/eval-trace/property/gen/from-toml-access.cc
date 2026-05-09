#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── FromTOMLAccessGen ────────────────────────────────────────────────
//
// Generates: (builtins.fromTOML (builtins.readFile <tomlfile>))."key"
// Kind::TomlFile (plain content write, valid TOML mutation), ResultKind: scalar.
// Generates simple 2-key TOML: one string key and one integer key.
// This is the only generator that exercises fromTOML.

rc::Gen<TestExpr> makeFromTOMLAccessGen()
{
    // Generate a string value and an integer value for a simple 2-key TOML.
    // Keys are fixed: "name" (string) and "count" (int).
    return rc::gen::mapcat(
        rc::gen::arbitrary<bool>(),  // true = access "name", false = access "count"
        [](bool accessName) {
            return rc::gen::mapcat(
                rc::gen::container<std::string>(
                    rc::gen::inRange('a', (char)('z' + 1))),  // printable alpha for TOML string
                [accessName](std::string strVal) {
                    return rc::gen::map(
                        rc::gen::inRange<int64_t>(0, 10000),
                        [accessName, strVal](int64_t intVal) {
                            // Build TOML content: simple key=value pairs.
                            // TOML string values must be quoted; integers are bare.
                            std::string tomlContent =
                                "name = \"" + strVal + "\"\n"
                                "count = " + std::to_string(intVal) + "\n";

                            // Use Kind::TomlFile (plain content write, but generateMutation
                            // produces valid TOML instead of arbitrary ASCII).
                            auto handle = std::make_shared<TempExtFile>("toml", tomlContent);

                            DepSlot slot;
                            slot.kind = DepSlot::Kind::TomlFile;
                            slot.path = handle->path;
                            slot.fileHandle = handle;
                            slot.currentValue = tomlContent;
                            slot.setOriginal(tomlContent);

                            std::string chosenKey = accessName ? "name" : "count";
                            std::string nixCode =
                                "(builtins.fromTOML (builtins.readFile "
                                + handle->path.string() + ")).\"" + chosenKey + "\"";

                            TestExpr::ResultKind kind = accessName
                                ? TestExpr::ResultKind::String
                                : TestExpr::ResultKind::Int;

                            return TestExpr{
                                .nixCode = std::move(nixCode),
                                .expectedKind = kind,
                                .depSlots = {std::move(slot)},
                            };
                        });
                });
        });
}

} // namespace nix::eval_trace::test::proptest
