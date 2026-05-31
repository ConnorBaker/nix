#include "boundary-audit-fixture.hh"

#include <sstream>

#include "nix/expr/value-to-json.hh"

namespace nix {

/**
 * Boundary 2 / 5: `nix eval --json`.
 *
 * Cover-fix shape (`src/nix/eval.cc`, post-fix): capture the JSON
 * dump, run `realiseContext` + `ensureLazyPathsCopied`, then
 * `rewriteStrings` over the dumped text before writing to stdout.
 * The rewrite is safe on the JSON-serialised form because
 * placeholder render strings (`/<base32>`) cannot accidentally
 * collide with JSON syntax tokens — the rewrite map's keys are
 * 53-byte sentinel strings, not arbitrary JSON content.
 *
 * `--json` differs from `--raw` only in the serialiser used; the
 * audit verifies the same `realiseContext`+`rewriteStrings`
 * pattern works against `printValueAsJSON`'s output.
 */
class BoundaryAuditEvalJsonTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditEvalJsonTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    NixStringContext context;
    std::string output;
    try {
        std::ostringstream oss;
        printValueAsJSON(state, /*strict=*/true, v, noPos, oss, context, /*copyToStore=*/false);
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        output = rewriteStrings(oss.str(), rewrites);
    } catch (Error & e) {
        recordThrow("nix eval --json", e.what());
        FAIL() << "nix eval --json threw: " << e.what();
        return;
    }

    auto leak = detectLeak(output, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("nix eval --json", leak);
    } else {
        recordPass("nix eval --json");
    }

    EXPECT_FALSE(leak.any()) << "nix eval --json leaked placeholder text: `" << leak.firstLeakedText << "` (output=`"
                             << output << "`)";
}

} // namespace nix
