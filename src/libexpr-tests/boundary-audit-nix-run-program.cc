#include "boundary-audit-fixture.hh"

namespace nix {

/**
 * Bypass-site boundary 14: `nix run`'s `apps.<system>.<name>.program`.
 *
 * `src/nix/app.cc:69-112`'s `InstallableValue::toApp` reads the
 * `program` attribute via `getStringWithContext`, walks the context
 * to build a `vector<DerivedPath>` (with a correct `SourceVirtual`
 * arm at line 95-102 that resolves to real storepaths), and stores
 * the **raw body** as `program` (line 111). After Item 1 activation,
 * if the program string carries a `SourceVirtual` placeholder, the
 * body contains the placeholder render `/<base32>` — and `nix run`
 * tries to `exec` that path, which fails.
 *
 * DetSys hit this in production (commit 5d6ab843d, "Fix `nix run` on
 * an app with lazy trees enabled", Feb 2026 — the most recent
 * lazy-trees fix in their tree).
 *
 * Cover-fix shape: when iterating the context to build `context2`,
 * also collect the rewrite map for `SourceVirtual` elements and
 * apply `rewriteStrings(program, rewrites)` before stashing into
 * `UnresolvedApp::program`.
 *
 * The audit replays a shrunken version: build a placeholder-bearing
 * "program" string, walk the context with the cover-fix shape,
 * assert the resulting program string has no placeholder text.
 */
class BoundaryAuditNixRunProgramTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditNixRunProgramTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay app.cc::toApp's body construction with the cover-fix
       sequence: rewrite the program string before stashing it. */
    NixStringContext context;
    std::string program;
    try {
        copyContext(v, context);
        program = std::string{v.string_view()};
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        program = rewriteStrings(std::move(program), rewrites);
    } catch (Error & e) {
        recordThrow("nix run program", e.what());
        FAIL() << "nix run program walk threw: " << e.what();
        return;
    }

    auto leak = detectLeak(program, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("nix run program", leak);
    } else {
        recordPass("nix run program");
    }

    EXPECT_FALSE(leak.any()) << "nix run program leaked placeholder text in program string: `" << leak.firstLeakedText
                             << "` (program=`" << program << "`)";
}

} // namespace nix
