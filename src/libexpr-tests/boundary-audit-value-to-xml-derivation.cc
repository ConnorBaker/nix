#include "boundary-audit-fixture.hh"

#include "nix/expr/value-to-xml.hh"

namespace nix {

/**
 * Bypass-site boundary: `printValueAsXML` isDerivation arm.
 *
 * `src/libexpr/value-to-xml.cc` has a leaf-string nString case at
 * line 84 that calls `copyContext(v, context)` correctly. But the
 * `nAttrs → isDerivation` arm at lines 99-115 reads
 * `a->value->string_view()` for `drvPath` and `outPath` directly into
 * `XMLAttrs` without going through the leaf-nString case, so any
 * SourceVirtual context on those values would be dropped from the
 * accumulator threaded through to the caller.
 *
 * In normal Nix flows drvPath/outPath bodies are derivation-side
 * context (`Built`/`DrvDeep`/`Opaque`) not source-side, so a
 * SourceVirtual reaching them would require an unusual construction.
 * The fix (defensive `copyContext` in both branches) is symmetry with
 * the leaf-nString case rather than a known leak path; the audit
 * verifies the symmetry holds — if a derivation attrset's drvPath or
 * outPath body did carry SourceVirtual, the post-fix accumulator
 * captures it for the caller's resolve step.
 *
 * The audit constructs a fake derivation-shaped attrset with
 * SourceVirtual-bearing drvPath/outPath, drives the post-fix
 * `printValueAsXML` flow, and asserts the accumulated context
 * carries the SourceVirtual element so it survives to the caller's
 * resolve step.
 */
class BoundaryAuditValueToXmlDerivationTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditValueToXmlDerivationTest, DerivationDrvPathOutPathContextThreaded)
{
    auto placeholder = mintPlaceholder();

    /* Build a fake derivation: { type = "derivation"; drvPath = <SV>; outPath = <SV>; }. */
    Value vType;
    vType.mkString("derivation", state.mem);
    Value vDrvPath;
    mkPlaceholderString(vDrvPath, placeholder, "audit-source");
    Value vOutPath;
    mkPlaceholderString(vOutPath, placeholder, "audit-source");

    auto bindings = state.buildBindings(3);
    bindings.insert(state.s.type, &vType);
    bindings.insert(state.s.drvPath, &vDrvPath);
    bindings.insert(state.s.outPath, &vOutPath);

    Value v;
    v.mkAttrs(bindings);

    /* Drive printValueAsXML and check that the accumulated context
       contains a SourceVirtual element. The XML output itself will
       contain placeholder bodies (XML attribute values are not
       resolved by this serialiser; resolution is the caller's
       responsibility post-§6.2 — see boundary-audit-nix-env-query-xml.cc).
       What this audit verifies is that the cover-fix shape (defensive
       copyContext on the isDerivation arm) successfully threads the
       SourceVirtual element through to the caller's accumulator so
       the caller's resolve step can act on it. */
    NixStringContext context;
    std::ostringstream out;
    printValueAsXML(state, /*strict=*/true, /*location=*/false, v, out, context, noPos);

    bool foundSourceVirtual = false;
    for (auto & c : context) {
        if (auto * sv = std::get_if<NixStringContextElem::SourceVirtual>(&c.raw)) {
            if (sv->placeholder == placeholder) {
                foundSourceVirtual = true;
                break;
            }
        }
    }

    if (foundSourceVirtual)
        recordPass("printValueAsXML isDerivation arm (context threaded)");
    else
        recordLeak(
            "printValueAsXML isDerivation arm (context dropped)",
            BoundaryAuditTest::LeakDetail{
                .bodyLeak = false, .contextLeak = true, .firstLeakedText = "(SourceVirtual lost)"});

    EXPECT_TRUE(foundSourceVirtual)
        << "printValueAsXML's isDerivation arm did NOT thread drvPath/outPath SourceVirtual context to the "
           "caller; downstream resolve step has nothing to act on.";
}

} // namespace nix
