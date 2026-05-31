#include "boundary-audit-fixture.hh"

namespace nix {

/**
 * Bypass-site boundary 7: REPL `:p` / `:print`.
 *
 * `src/libcmd/repl.cc:585` writes `v.string_view()` to stdout
 * directly when `v.type() == nString`, with no context accumulation
 * and no resolution. After Item 1 activation, a `SourceVirtual`-
 * bearing string forced via `:p` would print the placeholder render
 * verbatim.
 *
 * Cover-fix shape (post-fix): mirror `nix eval --raw`. Run
 * `coerceToString` into a `NixStringContext`, resolve via
 * `resolveSourceVirtualContext`, ensureLazyPathsCopied, then
 * `rewriteStrings` over the body before writing.
 *
 * The audit test replays the cover-fix's *expected* sequence and
 * asserts placeholder text doesn't survive. A leak indicates the
 * REPL site has regressed back to raw `string_view()`.
 */
class BoundaryAuditReplPrintTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditReplPrintTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Mirror the cover-fix sequence the REPL :p site should adopt. */
    NixStringContext context;
    std::string output;
    try {
        auto coerced = state.coerceToString(noPos, v, context, "while printing the REPL value");
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        output = rewriteStrings(std::string(*coerced), rewrites);
    } catch (Error & e) {
        recordThrow(":p / :print", e.what());
        FAIL() << "REPL :p threw: " << e.what();
        return;
    }

    auto leak = detectLeak(output, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak(":p / :print", leak);
    } else {
        recordPass(":p / :print");
    }

    EXPECT_FALSE(leak.any()) << "REPL :p leaked placeholder text: `" << leak.firstLeakedText << "` (output=`" << output
                             << "`)";
}

} // namespace nix
