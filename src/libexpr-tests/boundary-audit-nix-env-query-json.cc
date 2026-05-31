#include "boundary-audit-fixture.hh"

#include <sstream>

#include "nix/expr/value-to-json.hh"

namespace nix {

/**
 * Bypass-site boundary 8: `nix-env --query --json --meta`.
 *
 * `src/nix/nix-env/nix-env.cc:935-949` constructs a fresh
 * `NixStringContext` per meta attribute, calls `printValueAsJSON`,
 * stores the result in `metaObj[j]`, then dumps `topObj` to stdout
 * at line 957 — without ever calling resolve. After Item 1
 * activation a `SourceVirtual`-bearing meta string would print the
 * placeholder render verbatim.
 *
 * Cover-fix shape (post-fix): accumulate context across ALL attrs
 * of the meta object (single `NixStringContext` shared across the
 * loop), then resolve once at the end and `rewriteStrings` over the
 * dumped JSON before writing to stdout.
 *
 * The audit test replays a smaller version of the loop: build an
 * attrset of two meta-shaped string Values both carrying placeholder
 * context, walk via `printValueAsJSON` accumulating shared context,
 * resolve, dump-and-rewrite. Asserts no placeholder text in output.
 */
class BoundaryAuditNixEnvQueryJsonTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditNixEnvQueryJsonTest, NoLeak)
{
    auto placeholder = mintPlaceholder();

    /* Two meta-shaped string Values, both bearing the same
       SourceVirtual placeholder context. Mirrors a derivation with
       multiple meta attributes referencing the same source. */
    Value vA, vB;
    mkPlaceholderString(vA, placeholder, "audit-source");
    mkPlaceholderString(vB, placeholder, "audit-source");

    NixStringContext context;
    std::string output;
    try {
        /* Mirror the per-attribute walk in nix-env.cc:939-948 but
           with a SINGLE accumulated context — the cover-fix that
           the production site needs. */
        auto jsonA = printValueAsJSON(state, /*strict=*/true, vA, noPos, context, /*copyToStore=*/false);
        auto jsonB = printValueAsJSON(state, /*strict=*/true, vB, noPos, context, /*copyToStore=*/false);

        /* Combine into a topObj-shaped wrapper so the rewrite must
           survive nested JSON serialisation. */
        nlohmann::json topObj = nlohmann::json::object();
        topObj["pkgA"] = nlohmann::json::object();
        topObj["pkgA"]["meta"] = nlohmann::json::object();
        topObj["pkgA"]["meta"]["a"] = jsonA;
        topObj["pkgB"] = nlohmann::json::object();
        topObj["pkgB"]["meta"] = nlohmann::json::object();
        topObj["pkgB"]["meta"]["b"] = jsonB;

        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        output = rewriteStrings(topObj.dump(2), rewrites);
    } catch (Error & e) {
        recordThrow("nix-env --query --json --meta", e.what());
        FAIL() << "nix-env JSON walk threw: " << e.what();
        return;
    }

    auto leak = detectLeak(output, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("nix-env --query --json --meta", leak);
    } else {
        recordPass("nix-env --query --json --meta");
    }

    EXPECT_FALSE(leak.any()) << "nix-env --json --meta leaked placeholder text: `" << leak.firstLeakedText
                             << "` (output=`" << output << "`)";
}

} // namespace nix
