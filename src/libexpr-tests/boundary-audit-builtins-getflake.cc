#include "boundary-audit-fixture.hh"

namespace nix {

/**
 * Bypass-site boundary 13: `builtins.getFlake`.
 *
 * `src/libflake/flake-primops.cc:48`'s `prim_getFlake` calls
 * `forceStringNoCtx(*args[0], pos, ...)` which throws on any
 * context. After Item 1 activation, a flakeref string carrying a
 * `SourceVirtual` placeholder context would trigger the
 * `forceStringNoCtx` error with the placeholder wire form
 * (`~<hash>:<name>`) embedded in the error message.
 *
 * DetSys hit this in production (commit bb3846e6d, Feb 2026,
 * "builtins.getFlake: Devirtualize strings", #302).
 *
 * Cover-fix shape: switch to `forceString(v, context, ...)` to
 * accept context, resolve via `resolveSourceVirtualContext`,
 * rewrite the body to the real storepath text, then `parseFlakeRef`
 * over the rewritten body. The result is a flakeref pointing at
 * the real storepath rather than the placeholder.
 *
 * The audit replays the cover-fix shape: force-with-context,
 * resolve, rewrite, parse. Asserts the resulting body contains
 * no placeholder text.
 */
class BoundaryAuditBuiltinsGetFlakeTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditBuiltinsGetFlakeTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay prim_getFlake's body string handling with the cover-fix
       sequence. We don't call `parseFlakeRef` because that's
       upstream of the leak — the leak is in the body that would be
       parsed. We only assert the body, post-resolution, contains no
       placeholder render. */
    NixStringContext context;
    std::string flakeRefS;
    try {
        auto s = state.forceString(v, context, noPos, "while evaluating getFlake input");
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        flakeRefS = rewriteStrings(std::string{s}, rewrites);
    } catch (Error & e) {
        recordThrow("builtins.getFlake", e.what());
        FAIL() << "getFlake threw: " << e.what();
        return;
    }

    auto leak = detectLeak(flakeRefS, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("builtins.getFlake", leak);
    } else {
        recordPass("builtins.getFlake");
    }

    EXPECT_FALSE(leak.any()) << "builtins.getFlake leaked placeholder text in flakeref string: `"
                             << leak.firstLeakedText << "` (flakeRef=`" << flakeRefS << "`)";
}

} // namespace nix
