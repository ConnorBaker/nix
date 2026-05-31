#include "boundary-audit-fixture.hh"

#include <sstream>

#include "nix/expr/print-ambiguous.hh"

namespace nix {

/**
 * Bypass-site boundary 10: `nix-env` manifest writer (`createUserEnv`).
 *
 * `src/nix/nix-env/user-env.cc:113` previously called
 * `printAmbiguous(state, manifest, str, nullptr)` with nullptr
 * context-out — any context on leaf strings in the manifest (e.g.
 * a `meta.description` containing a `SourceVirtual` placeholder)
 * was dropped. The serialised manifest was then written to the store
 * as `env-manifest.nix`, persisting the placeholder render verbatim
 * on disk. This is a **persistent on-disk leak** — the manifest
 * outlives the in-process `MaterialisationScheduler` registration,
 * so subsequent `nix-env` invocations would re-read the manifest
 * and observe a stale placeholder.
 *
 * Cover-fix shape: thread a `NixStringContext` into `printAmbiguous`,
 * resolve via `resolveSourceVirtualContext`, rewrite the serialised
 * text before writing to the store.
 *
 * The audit test mirrors a shrunken manifest shape: a list with one
 * attrset containing a placeholder-bearing meta string. Drives
 * `printAmbiguous`, applies the cover-fix sequence, asserts no leak.
 */
class BoundaryAuditUserEnvManifestTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditUserEnvManifestTest, NoLeak)
{
    auto placeholder = mintPlaceholder();

    /* Construct a minimal manifest-shaped Value: a list of one
       attrset with a `meta` attribute whose value carries the
       placeholder context. */
    Value vMetaStr;
    mkPlaceholderString(vMetaStr, placeholder, "audit-source");

    Value vMeta;
    {
        auto attrs = state.buildBindings(1);
        attrs.alloc(state.symbols.create("description")) = vMetaStr;
        vMeta.mkAttrs(attrs);
    }

    Value vElem;
    {
        auto attrs = state.buildBindings(1);
        attrs.alloc(state.s.meta) = vMeta;
        vElem.mkAttrs(attrs);
    }

    Value manifest;
    auto list = state.buildList(1);
    list[0] = state.allocValue();
    *list[0] = vElem;
    manifest.mkList(list);

    NixStringContext context;
    std::string output;
    try {
        std::ostringstream buf;
        printAmbiguous(state, manifest, buf, /*seen=*/nullptr, &context);
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        output = rewriteStrings(buf.str(), rewrites);
    } catch (Error & e) {
        recordThrow("nix-env manifest writer (printAmbiguous)", e.what());
        FAIL() << "manifest writer threw: " << e.what();
        return;
    }

    auto leak = detectLeak(output, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("nix-env manifest writer (printAmbiguous)", leak);
    } else {
        recordPass("nix-env manifest writer (printAmbiguous)");
    }

    EXPECT_FALSE(leak.any()) << "user-env manifest leaked placeholder text: `" << leak.firstLeakedText << "` (output=`"
                             << output << "`)";
}

} // namespace nix
