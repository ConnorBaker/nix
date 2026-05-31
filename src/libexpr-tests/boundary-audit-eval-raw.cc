#include "boundary-audit-fixture.hh"

namespace nix {

/**
 * Boundary 1 / 5: `nix eval --raw`.
 *
 * Cover-fix shape (`src/nix/eval.cc`, post-fix):
 *
 *     auto string = state->coerceToString(noPos, *v, context, ...);
 *     auto rewrites = state->realiseContext(context);
 *     state->ensureLazyPathsCopied(context);
 *     writeFull(getStandardOutput(), rewriteStrings(*string, rewrites));
 *
 * The audit replays the same sequence and asserts the placeholder
 * render text does not leak into the rewritten output. A leak here
 * means the cover-fix has regressed — `realiseContext` did not
 * resolve the placeholder, or `rewriteStrings` did not pick up the
 * rewrite key.
 */
class BoundaryAuditEvalRawTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditEvalRawTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Mirror the cover-fix sequence in src/nix/eval.cc. */
    NixStringContext context;
    std::string output;
    try {
        auto coerced = state.coerceToString(noPos, v, context, "while generating the eval command output");
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        output = rewriteStrings(std::string(*coerced), rewrites);
    } catch (Error & e) {
        recordThrow("nix eval --raw", e.what());
        FAIL() << "nix eval --raw threw: " << e.what();
        return;
    }

    auto leak = detectLeak(output, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("nix eval --raw", leak);
    } else {
        recordPass("nix eval --raw");
    }

    EXPECT_FALSE(leak.any()) << "nix eval --raw leaked placeholder text: `" << leak.firstLeakedText << "` (output=`"
                             << output << "`)";
}

} // namespace nix
