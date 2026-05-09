#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── IfThenElseGen ────────────────────────────────────────────────────
//
// Generates: if <bool-literal> then <scalar> else <scalar>
// Both branches use makeScalarGen so they share the same ResultKind.
// The condition is a random bool literal (true/false).

rc::Gen<TestExpr> makeIfThenElseGen()
{
    return rc::gen::mapcat(
        rc::gen::arbitrary<bool>(),  // condition: true or false
        [](bool cond) {
            return rc::gen::map(
                makeScalarGen(),
                [cond](TestExpr branch) {
                    // Both branches are the same scalar expression — this
                    // ensures the ResultKind is well-defined regardless of which
                    // branch is taken at evaluation time.
                    std::string nixCode =
                        "if " + std::string(cond ? "true" : "false")
                        + " then " + branch.nixCode
                        + " else " + branch.nixCode;
                    return TestExpr{
                        .nixCode = std::move(nixCode),
                        .expectedKind = branch.expectedKind,
                        .depSlots = {},  // scalar branches have no dep slots
                    };
                });
        });
}

} // namespace nix::eval_trace::test::proptest
