#include "boundary-audit-fixture.hh"

#include "nix/expr/eval-inline.hh"
#include "nix/store/derivations.hh"

namespace nix {

/**
 * Boundary: `builtins.derivationStrict` — derivation CONSTRUCTION.
 *
 * This is the highest-stakes leak boundary and the one the original
 * cover-fix sweep MISSED: a `SourceVirtual` placeholder interpolated into
 * a derivation attribute must be rewritten to the resolved store path in
 * EVERY persisted field of the resulting `Derivation` — `builder`, `args`,
 * `env`, `platform`, AND `structuredAttrs` (the last is serialised into
 * the `.drv` independently of `env` via `StructuredAttrs::unparse`). The
 * pre-fix code rewrote only builder/args/env, so a placeholder leaked into
 * the on-disk `.drv` and the builder's `__json`/`.attrs.json` under
 * `__structuredAttrs = true`.
 *
 * Unlike the display-boundary audits (which inspect a serialiser's text),
 * this drives `prim_derivationStrict` and inspects the WRITTEN
 * `Derivation` read back from the store — the actual artifact a build
 * would consume.
 */
class BoundaryAuditDerivationStrictTest : public BoundaryAuditTest
{
protected:
    /* Concatenate every persisted string field of a written .drv so a
       single detectLeak call covers builder/args/env/platform/
       structuredAttrs. */
    std::string drvAllFields(const StorePath & drvPath)
    {
        auto drv = state.store->readDerivation(drvPath);
        std::string all = drv.platform + "\n" + drv.builder;
        for (auto & a : drv.args)
            all += "\n" + a;
        for (auto & [k, val] : drv.env)
            all += "\n" + k + "=" + val;
        if (drv.structuredAttrs) {
            auto [_, jsonS] = drv.structuredAttrs->unparse();
            all += "\n" + jsonS;
        }
        return all;
    }

    /* Build `derivationStrict` arg attrs with `src` = a placeholder
       string, optionally enabling `__structuredAttrs`. Returns the
       written drvPath after calling the production primop. */
    StorePath buildAuditDrv(const SourcePlaceholder & placeholder, bool structuredAttrs)
    {
        Value vSrc;
        mkPlaceholderString(vSrc, placeholder, "audit-source");
        Value vName;
        vName.mkString("audit-drv", state.mem);
        Value vSystem;
        vSystem.mkString("x86_64-linux", state.mem);
        Value vBuilder;
        vBuilder.mkString("/bin/sh", state.mem);
        Value vTrue;
        vTrue.mkBool(true);

        auto attrs = state.buildBindings(8);
        attrs.insert(state.s.name, &vName);
        attrs.insert(state.s.system, &vSystem);
        attrs.insert(state.symbols.create("builder"), &vBuilder);
        attrs.insert(state.symbols.create("src"), &vSrc);
        if (structuredAttrs)
            attrs.insert(state.s.structuredAttrs, &vTrue);
        Value vAttrs;
        vAttrs.mkAttrs(attrs.finish());

        auto vDrvStrict = eval("builtins.derivationStrict");
        Value vCall;
        state.callFunction(vDrvStrict, vAttrs, vCall, noPos);
        state.forceAttrs(vCall, noPos, "while evaluating derivationStrict result");
        auto * drvPathAttr = vCall.attrs()->get(state.s.drvPath);
        assert(drvPathAttr);
        NixStringContext ctx;
        auto drvPathStr =
            state.coerceToString(noPos, *drvPathAttr->value, ctx, "while reading drvPath", false, false, true).toOwned();
        return state.store->parseStorePath(drvPathStr);
    }
};

/* `__structuredAttrs = true`: the placeholder must not survive into ANY
   persisted drv field — this is the field the original svRewrites block
   forgot. Reverting the structuredAttrs rewrite in primops.cc makes this
   FAIL (non-vacuity). */
TEST_F(BoundaryAuditDerivationStrictTest, StructuredAttrsNoLeak)
{
    auto placeholder = mintPlaceholder();
    std::string allFields;
    try {
        allFields = drvAllFields(buildAuditDrv(placeholder, /*structuredAttrs=*/true));
    } catch (Error & e) {
        recordThrow("builtins.derivationStrict (__structuredAttrs)", e.what());
        FAIL() << "derivationStrict threw: " << e.what();
        return;
    }

    auto leak = detectLeak(allFields, placeholder, "audit-source");
    if (leak.any())
        recordLeak("builtins.derivationStrict (__structuredAttrs)", leak);
    else
        recordPass("builtins.derivationStrict (__structuredAttrs)");
    EXPECT_FALSE(leak.any()) << "derivationStrict leaked placeholder text into a persisted .drv field: `"
                             << leak.firstLeakedText << "`";
}

/* The plain (non-structured) path: placeholder interpolated into `src`
   reaches `env` and must be rewritten there too. */
TEST_F(BoundaryAuditDerivationStrictTest, PlainEnvNoLeak)
{
    auto placeholder = mintPlaceholder();
    std::string allFields;
    try {
        allFields = drvAllFields(buildAuditDrv(placeholder, /*structuredAttrs=*/false));
    } catch (Error & e) {
        recordThrow("builtins.derivationStrict (plain)", e.what());
        FAIL() << "derivationStrict threw: " << e.what();
        return;
    }

    auto leak = detectLeak(allFields, placeholder, "audit-source");
    if (leak.any())
        recordLeak("builtins.derivationStrict (plain)", leak);
    else
        recordPass("builtins.derivationStrict (plain)");
    EXPECT_FALSE(leak.any()) << "derivationStrict leaked placeholder text into a persisted .drv field: `"
                             << leak.firstLeakedText << "`";
}

} // namespace nix
