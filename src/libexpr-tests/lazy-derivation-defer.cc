#include <gtest/gtest.h>

#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/value/context.hh"
#include "nix/store/derivations.hh"
#include "nix/store/async-path-writer.hh"
#include "nix/store/store-open.hh"
#include "nix/util/file-system.hh"
#include "nix/util/configuration.hh"
#include "nix/util/experimental-features.hh"

#include <set>
#include <variant>

/**
 * LD-V1 (phase law) — `derivationStrict` with `lazy-derivations` on returns a
 * fully-rendered attrset (drvPath + per-output strings) while the `.drv` file
 * itself is *deferred* (not on disk), and the write-queue drain materialises
 * it. Drives the real `prim_derivationStrict` (Increment 2), so it is
 * non-vacuous: reverting the defer branch in primops.cc makes the
 * `EXPECT_FALSE(isValidPath)` fail.
 *
 * A real `local?root=` chroot store is used because the dummy store rejects
 * `.drv` writes through `addToStoreFromDump` (the write-queue's path).
 */
namespace nix {

class LazyDerivationDeferTest : public LibExprTest
{
public:
    LazyDerivationDeferTest()
        : LibExprTest(openStore("local?root=" + (createTempDir() / "store").string()), [](bool & readOnlyMode) {
            EvalSettings settings{readOnlyMode};
            settings.nixPath = {};
            settings.lazyDerivations = true;
            return settings;
        })
    {
    }

protected:
    std::string coerce(Value * val)
    {
        NixStringContext ctx;
        return state.coerceToString(noPos, *val, ctx, "while reading a derivationStrict attr", false, false, true)
            .toOwned();
    }
};

/* Scoped enable of an experimental feature (CAFloating needs `ca-derivations`).
   Restores the previous set on destruction. */
struct ScopedXp
{
    std::set<ExperimentalFeature> prev;
    explicit ScopedXp(std::string_view feature)
        : prev(experimentalFeatureSettings.experimentalFeatures.get())
    {
        experimentalFeatureSettings.set("extra-experimental-features", std::string{feature});
    }
    ~ScopedXp()
    {
        experimentalFeatureSettings.experimentalFeatures.assign(prev);
    }
    ScopedXp(const ScopedXp &) = delete;
    ScopedXp & operator=(const ScopedXp &) = delete;
};

TEST_F(LazyDerivationDeferTest, DefersDrvUntilFlushAndRendersAttrset)
{
    auto v = eval(R"(builtins.derivationStrict {
        name = "lazy-defer-test"; system = "x86_64-linux"; builder = "/bin/sh";
    })");
    state.forceAttrs(v, noPos, "while evaluating derivationStrict result");

    auto * drvPathAttr = v.attrs()->get(state.s.drvPath);
    ASSERT_TRUE(drvPathAttr);
    auto drvPathS = coerce(drvPathAttr->value);

    /* The output is keyed by its name ("out"); its string is the outPath. */
    auto * outAttr = v.attrs()->get(state.symbols.create("out"));
    ASSERT_TRUE(outAttr);
    auto outPathS = coerce(outAttr->value);

    /* LD-V1 rendering half: the full attrset renders with no `.drv` on disk. */
    EXPECT_FALSE(drvPathS.empty());
    EXPECT_FALSE(outPathS.empty());
    auto drvPath = state.store->parseStorePath(drvPathS);
    EXPECT_FALSE(state.store->isValidPath(drvPath)); // deferred — not written yet

    /* LD-V1 deferral half: draining the queue materialises the bytes. */
    state.asyncPathWriter->waitForAllPaths();
    EXPECT_TRUE(state.store->isValidPath(drvPath));
    EXPECT_NO_THROW(state.store->readDerivation(drvPath));
}

/* LD-V1 (CA arms) — the InputAddressed arm is `DefersDrvUntilFlush…` above; these
   cover the other two output kinds from §2.1. The returned attrset still renders
   (drvPath + outPath string) with NO `.drv` on disk, and the deferred `.drv`
   materialises at the drain. */

/* CAFixed (fixed-output): `outPath` is a real content-addressed path computed
   purely in-memory (`makeFixedOutputPathFromCA`), so it renders without the
   `.drv`. No experimental feature needed. */
TEST_F(LazyDerivationDeferTest, RendersCaFixedOutputWithoutDrvFile)
{
    auto v = eval(R"(builtins.derivationStrict {
        name = "lazy-cafixed"; system = "x86_64-linux"; builder = "/bin/sh";
        outputHashMode = "recursive"; outputHashAlgo = "sha256";
        outputHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    })");
    state.forceAttrs(v, noPos, "while evaluating derivationStrict result");

    auto * drvPathAttr = v.attrs()->get(state.s.drvPath);
    ASSERT_TRUE(drvPathAttr);
    auto drvPath = state.store->parseStorePath(coerce(drvPathAttr->value));
    auto * outAttr = v.attrs()->get(state.symbols.create("out"));
    ASSERT_TRUE(outAttr);

    EXPECT_FALSE(coerce(outAttr->value).empty());    // fixed-output outPath renders (in-memory CA path)
    EXPECT_FALSE(state.store->isValidPath(drvPath)); // `.drv` deferred
    state.asyncPathWriter->waitForAllPaths();
    EXPECT_TRUE(state.store->isValidPath(drvPath));
}

/* CAFloating (`__contentAddressed = true`): `outPath` is a `DownstreamPlaceholder`
   render — a non-empty string resolved post-build, needing no `.drv` file at all.
   Requires the `ca-derivations` experimental feature. */
TEST_F(LazyDerivationDeferTest, RendersCaFloatingOutputWithoutDrvFile)
{
    ScopedXp _ca{"ca-derivations"};
    auto v = eval(R"(builtins.derivationStrict {
        name = "lazy-cafloating"; system = "x86_64-linux"; builder = "/bin/sh";
        __contentAddressed = true; outputHashMode = "recursive"; outputHashAlgo = "sha256";
    })");
    state.forceAttrs(v, noPos, "while evaluating derivationStrict result");

    auto * drvPathAttr = v.attrs()->get(state.s.drvPath);
    ASSERT_TRUE(drvPathAttr);
    auto drvPath = state.store->parseStorePath(coerce(drvPathAttr->value));
    auto * outAttr = v.attrs()->get(state.symbols.create("out"));
    ASSERT_TRUE(outAttr);

    EXPECT_FALSE(coerce(outAttr->value).empty());    // CAFloating outPath renders (placeholder)
    EXPECT_FALSE(state.store->isValidPath(drvPath)); // `.drv` deferred
    state.asyncPathWriter->waitForAllPaths();
    EXPECT_TRUE(state.store->isValidPath(drvPath));
    EXPECT_NO_THROW(state.store->readDerivation(drvPath));
}

/* LD-P8 — reference-set fidelity through deferral, over a REAL evaluated
   derivation: a deferred `.drv` that has a source input registers that source
   as a reference once materialised (so GC/closure see the edge). Drives real
   `derivationStrict`. */
TEST_F(LazyDerivationDeferTest, DeferredDrvRegistersInputSrcReference)
{
    auto v = eval(R"(builtins.derivationStrict {
        name = "p8"; system = "x86_64-linux"; builder = "/bin/sh";
        src = builtins.toFile "p8-src" "p8 source contents";
    })");
    state.forceAttrs(v, noPos, "while evaluating derivationStrict result");
    auto * drvPathAttr = v.attrs()->get(state.s.drvPath);
    ASSERT_TRUE(drvPathAttr);
    auto drvPath = state.store->parseStorePath(coerce(drvPathAttr->value));

    state.asyncPathWriter->waitForAllPaths();
    ASSERT_TRUE(state.store->isValidPath(drvPath));

    /* The `.drv` must reference the (non-`.drv`) source path it captured. */
    bool hasSourceRef = false;
    for (auto & r : state.store->queryPathInfo(drvPath)->references)
        if (!r.isDerivation())
            hasSourceRef = true;
    EXPECT_TRUE(hasSourceRef) << "deferred .drv lost its inputSrc reference edge (LD-P8)";
}

/* LD-V2 / LD-X2 — no new `NixStringContextElem` variant was introduced for the
   lazy-derivations work (it reuses `DrvDeep`/`Built`), so the R3
   consumer-enumeration hazard is avoided. This static count fails if a future
   change adds (or removes) a context variant, forcing a re-audit. */
TEST(LazyDerivationContext, ContextVariantSetUnchanged)
{
    static_assert(
        std::variant_size_v<NixStringContextElem::Raw> == 4,
        "NixStringContextElem variant set changed — re-audit the lazy-derivations "
        "and source-materialisation context consumers (R3).");
    SUCCEED();
}

} // namespace nix
