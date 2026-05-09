#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── CompoundGen ──────────────────────────────────────────────────────

rc::Gen<TestExpr> makeCompoundGen()
{
    // Combine a ReadFileGen sub-expression and a GetEnvGen sub-expression into a
    // single attrset expression with two dep slots.  The generated Nix code is:
    //   let _a = <readFile-expr>; _b = <getEnv-expr>; in { a = _a; b = _b; }
    //
    // Both sub-expressions use fixed, non-recursive generators — no rc::gen::lazy
    // needed here since makeCompoundGen does not recurse through makeNixExprGen.
    // The two dep slots come from different sources (a file and an env var), so
    // they can never collide in path or variable name.
    return rc::gen::map(
        rc::gen::tuple(makeReadFileGen(), makeGetEnvGen()),
        [](std::tuple<TestExpr, TestExpr> pair) {
            auto & [exprA, exprB] = pair;

            // Build the combined Nix expression.
            std::string nixCode =
                "let _a = " + exprA.nixCode
                + "; _b = " + exprB.nixCode
                + "; in { a = _a; b = _b; }";

            // Merge dep slots from both sub-expressions.  The vector starts
            // with exprA's slots (index 0) followed by exprB's slots (index 1).
            std::vector<DepSlot> deps;
            deps.reserve(exprA.depSlots.size() + exprB.depSlots.size());
            for (auto & s : exprA.depSlots)
                deps.push_back(std::move(s));
            for (auto & s : exprB.depSlots)
                deps.push_back(std::move(s));

            return TestExpr{
                .nixCode = std::move(nixCode),
                .expectedKind = TestExpr::ResultKind::Attrset,
                .depSlots = std::move(deps),
                .attrsToForce = {"a", "b"},
            };
        });
}

} // namespace nix::eval_trace::test::proptest
