#include "../expr-gen.hh"

#include <cmath>
#include <format>
#include <limits>

namespace nix::eval_trace::test::proptest {

// ── ScalarGen ────────────────────────────────────────────────────────

rc::Gen<TestExpr> makeScalarGen()
{
    using ResultKind = TestExpr::ResultKind;

    return rc::gen::oneOf(
        // integer (exclude INT64_MIN: Nix parses as negate(overflow) → ParseError)
        rc::gen::map(
            rc::gen::suchThat(rc::gen::arbitrary<int64_t>(),
                [](int64_t n) { return n != std::numeric_limits<int64_t>::min(); }),
            [](int64_t n) {
                return TestExpr{
                    .nixCode = std::to_string(n),
                    .expectedKind = ResultKind::Int,
                    .depSlots = {},
                };
            }),
        // string (printable ASCII only — arbitrary byte strings can produce
        // invalid UTF-8 which causes Nix parse errors)
        rc::gen::map(
            rc::gen::container<std::string>(rc::gen::inRange('!', '~')),
            [](std::string s) {
                return TestExpr{
                    .nixCode = "\"" + nixEscapeString(s) + "\"",
                    .expectedKind = ResultKind::String,
                    .depSlots = {},
                };
            }),
        // bool
        rc::gen::map(rc::gen::arbitrary<bool>(), [](bool b) {
            return TestExpr{
                .nixCode = b ? "true" : "false",
                .expectedKind = ResultKind::Bool,
                .depSlots = {},
            };
        }),
        // null
        rc::gen::just(TestExpr{
            .nixCode = "null",
            .expectedKind = ResultKind::Null,
            .depSlots = {},
        }),
        // float (restricted range, guaranteed valid Nix float literal)
        rc::gen::map(
            rc::gen::arbitrary<double>(),
            [](double d) {
                // Clamp to [-1e15, 1e15] to keep numbers manageable.
                d = std::fmod(d, 1e15);
                // Fallback for non-finite or zero (0.0 formats as "0" = int literal).
                if (!std::isfinite(d) || d == 0.0) d = 1.5;
                // Format and ensure valid Nix float literal (must have decimal point).
                auto s = std::format("{}", d);
                // std::format may produce "2" for 2.0 or "1e+05" for 100000.0.
                // Nix requires a decimal point in the mantissa.
                if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
                    s += ".0";
                else if (s.find('e') != std::string::npos && s.find('.') == std::string::npos) {
                    auto epos = s.find('e');
                    s.insert(epos, ".0");
                }
                return TestExpr{
                    .nixCode = s,
                    .expectedKind = ResultKind::Float,
                    .depSlots = {},
                };
            })
    );
}

} // namespace nix::eval_trace::test::proptest
