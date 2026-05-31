#include "boundary-audit-fixture.hh"

namespace nix {

/**
 * Boundary 4 / 5: `prim_import` (and any other primop that
 * funnels through `EvalState::realisePath`).
 *
 * `realisePath` (`src/libexpr/primops.cc:198-217`) calls
 * `realiseContext(context)` *before* using the path:
 *
 *     auto rewrites = realiseContext(context);
 *     if (copyLazyPaths == CopyLazyPaths::Copy)
 *         ensureLazyPathsCopied(context);
 *     path = {path.accessor, CanonPath(rewriteStrings(path.path.abs(), rewrites))};
 *
 * `realiseContext` (`primops.cc:107-133`) already has a
 * `SourceVirtual` arm that calls `materialisationScheduler->outPathOf`,
 * inserts the rewrite, and `allowPath`s the resolved storePath. So
 * this boundary should be *covered* — the audit's job is to confirm
 * the resolved path is the storePath, not the placeholder path.
 *
 * Coverage criterion (per audit interpretation (a)): after
 * `realisePath`, the returned `SourcePath`'s `path.abs()` must NOT
 * contain the placeholder render text.
 */
class BoundaryAuditPrimImportTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditPrimImportTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    std::string pathStr;
    try {
        /* Mirror prim_import: src/libexpr/primops.cc:326 calls
           `state.realisePath(pos, vPath, std::nullopt)`. */
        auto resolved = state.realisePath(noPos, v, /*resolveSymlinks=*/std::nullopt);
        pathStr = resolved.path.abs();
    } catch (Error & e) {
        recordThrow("prim_import (realisePath)", e.what());
        FAIL() << "prim_import threw: " << e.what();
        return;
    }

    auto leak = detectLeak(pathStr, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("prim_import (realisePath)", leak);
    } else {
        recordPass("prim_import (realisePath)");
    }

    EXPECT_FALSE(leak.any()) << "prim_import leaked placeholder text in resolved path: `" << leak.firstLeakedText
                             << "` (resolved=`" << pathStr << "`)";

    /* Strengthen: the resolved path must look like a real store
       path. A "covered" boundary not only avoids the placeholder
       text but actually points at the materialised storePath. */
    EXPECT_TRUE(
        pathStr.find("/nix/store/") != std::string::npos || pathStr.find(state.store->storeDir) != std::string::npos)
        << "prim_import resolved to `" << pathStr << "` which doesn't look like a store path";
}

} // namespace nix
