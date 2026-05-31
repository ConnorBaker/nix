#include "boundary-audit-fixture.hh"

#include <sstream>

#include "nix/util/xml-writer.hh"

namespace nix {

/**
 * Bypass-site boundary 9: `nix-env --query --xml --meta`.
 *
 * `src/nix/nix-env/nix-env.cc:1241-1278` writes meta string bodies
 * into XML attribute values via `attrs2["value"] = v->string_view()`,
 * with no context accumulation and no resolution. After Item 1
 * activation, a `SourceVirtual`-bearing meta string would emit the
 * placeholder render verbatim into the XML.
 *
 * Cover-fix shape (post-fix): construct the `XMLWriter` over a
 * stringstream rather than `std::cout`; accumulate `NixStringContext`
 * across all writes via `copyContext` at every leaf string;
 * `resolveSourceVirtualContext` + `ensureLazyPathsCopied` after the
 * writer is destroyed; `rewriteStrings` over the buffered XML before
 * writing to stdout.
 *
 * The audit test replays a smaller version: build an XML document
 * with placeholder-bearing values in attribute positions, accumulate
 * shared context, resolve, rewrite, assert no leak.
 */
class BoundaryAuditNixEnvQueryXmlTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditNixEnvQueryXmlTest, NoLeak)
{
    auto placeholder = mintPlaceholder();

    /* Two meta-shaped string Values, both bearing the placeholder
       context. Mirrors a derivation with multiple meta attributes
       referencing the same source. */
    Value vA, vB;
    mkPlaceholderString(vA, placeholder, "audit-source");
    mkPlaceholderString(vB, placeholder, "audit-source");

    NixStringContext context;
    std::string output;
    try {
        std::ostringstream buf;
        {
            XMLWriter xml(/*indent=*/true, buf);
            XMLOpenElement xmlRoot(xml, "items");
            XMLOpenElement item(xml, "item", XMLAttrs{});
            for (auto * v : {&vA, &vB}) {
                XMLAttrs attrs2;
                attrs2["type"] = "string";
                /* Emit the body as the attribute value AND copy
                   context into the shared accumulator — mirroring
                   what the cover-fix at nix-env.cc must do. */
                attrs2["value"] = std::string(v->string_view());
                copyContext(*v, context);
                xml.writeEmptyElement("meta", attrs2);
            }
            /* xmlRoot, item destructors fire here — XMLWriter dtor
               flushes any pending close-tags into `buf`. */
        }

        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        output = rewriteStrings(buf.str(), rewrites);
    } catch (Error & e) {
        recordThrow("nix-env --query --xml --meta", e.what());
        FAIL() << "nix-env XML walk threw: " << e.what();
        return;
    }

    auto leak = detectLeak(output, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("nix-env --query --xml --meta", leak);
    } else {
        recordPass("nix-env --query --xml --meta");
    }

    EXPECT_FALSE(leak.any()) << "nix-env --xml --meta leaked placeholder text: `" << leak.firstLeakedText
                             << "` (output=`" << output << "`)";
}

} // namespace nix
