#include "../expr-gen.hh"

namespace nix::eval_trace::test::proptest {

// ── LetGen ───────────────────────────────────────────────────────────

rc::Gen<TestExpr> makeLetGen(int depth)
{
    // Depth guard: at depth >= 3 fall back to a non-recursive generator so
    // that makeLetGen → makeNixExprGen → makeLetGen recursion terminates even
    // when RapidCheck's size parameter stays large.  Without this guard,
    // RapidCheck can drive deeply nested let expressions that exhaust the stack
    // or run for a very long time before the size mechanism kicks in.
    if (depth >= 3)
        return rc::gen::map(makeScalarGen(), [](TestExpr inner) {
            return TestExpr{
                .nixCode = "let _letVar = " + inner.nixCode + "; in _letVar",
                .expectedKind = inner.expectedKind,
                .depSlots = std::move(inner.depSlots),
                .attrsToForce = std::move(inner.attrsToForce),
                .indicesToForce = std::move(inner.indicesToForce),
            };
        });

    // rc::gen::lazy breaks the mutual recursion: makeLetGen → makeNixExprGen →
    // makeLetGen would be infinite without it.  lazy defers the call to
    // makeNixExprGen until the generator is actually sampled, so construction
    // terminates immediately and recursion only happens at sample time where
    // both RapidCheck's size parameter and the explicit depth counter limit depth.
    return rc::gen::map(
        rc::gen::lazy([depth]{ return makeNixExprGen(depth + 1); }),
        [](TestExpr inner) {
            // Wrap the inner expression in a let-binding.  Use a fixed variable
            // name to avoid collisions with identifiers that may appear in the
            // inner expression's generated code.
            return TestExpr{
                .nixCode = "let _letVar = " + inner.nixCode + "; in _letVar",
                .expectedKind = inner.expectedKind,
                .depSlots = std::move(inner.depSlots),
                .attrsToForce = std::move(inner.attrsToForce),
                .indicesToForce = std::move(inner.indicesToForce),
            };
        });
}

} // namespace nix::eval_trace::test::proptest
