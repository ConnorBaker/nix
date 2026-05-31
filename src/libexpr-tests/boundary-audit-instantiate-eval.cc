#include "boundary-audit-fixture.hh"

#include <set>
#include <sstream>

#include "nix/expr/print-ambiguous.hh"
#include "nix/expr/value-to-xml.hh"

namespace nix {

/**
 * Boundary 3 / 5: `nix-instantiate --eval`.
 *
 * `nix-instantiate --eval` (`src/nix/nix-instantiate/nix-instantiate.cc`)
 * has four output modes: `okRaw` (coerceToString), `okXML`
 * (printValueAsXML), `okJSON` (printValueAsJSON), and a fallback
 * (`printAmbiguous`). The Raw and JSON variants share their
 * codepaths with `nix eval --raw` and `nix eval --json` — those
 * tests cover them.
 *
 * Cover-fix shape (post-fix): capture each serialiser's output to
 * a stringstream, run `realiseContext` + `rewriteStrings` once over
 * the captured text, write the rewritten output to stdout. The XML
 * and printAmbiguous tests below replay that pattern.
 */
class BoundaryAuditInstantiateEvalTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditInstantiateEvalTest, NoLeakXml)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    NixStringContext context;
    std::string output;
    try {
        std::ostringstream oss;
        printValueAsXML(state, /*strict=*/true, /*location=*/false, v, oss, context, noPos);
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        output = rewriteStrings(oss.str(), rewrites);
    } catch (Error & e) {
        recordThrow("nix-instantiate --eval --xml", e.what());
        FAIL() << "nix-instantiate --eval --xml threw: " << e.what();
        return;
    }

    auto leak = detectLeak(output, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("nix-instantiate --eval --xml", leak);
    } else {
        recordPass("nix-instantiate --eval --xml");
    }

    EXPECT_FALSE(leak.any()) << "nix-instantiate --eval --xml leaked placeholder text: `" << leak.firstLeakedText
                             << "` (output=`" << output << "`)";
}

/**
 * The default (plain) `nix-instantiate --eval` output mode goes
 * through `printAmbiguous`
 * (`src/libexpr/include/nix/expr/print-ambiguous.hh:20`). Although
 * this format is officially deprecated (see issue #9730 in the
 * header), it remains the default for `nix-instantiate --eval`
 * without `--raw`/`--json`/`--xml` and is still part of the API.
 *
 * `printAmbiguous` walks the value, threads context, and writes the
 * raw `string_view` body — same leak shape as `printValueAsXML` /
 * `printValueAsJSON`.
 */
TEST_F(BoundaryAuditInstantiateEvalTest, NoLeakAmbiguous)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    NixStringContext context;
    std::string output;
    try {
        std::ostringstream oss;
        std::set<const void *> seen;
        printAmbiguous(state, v, oss, &seen, &context);
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        output = rewriteStrings(oss.str(), rewrites);
    } catch (Error & e) {
        recordThrow("nix-instantiate --eval (printAmbiguous)", e.what());
        FAIL() << "nix-instantiate --eval (default) threw: " << e.what();
        return;
    }

    auto leak = detectLeak(output, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("nix-instantiate --eval (printAmbiguous)", leak);
    } else {
        recordPass("nix-instantiate --eval (printAmbiguous)");
    }

    EXPECT_FALSE(leak.any()) << "nix-instantiate --eval (default) leaked placeholder text: `" << leak.firstLeakedText
                             << "` (output=`" << output << "`)";
}

} // namespace nix
