#include "boundary-audit-fixture.hh"

namespace nix {

/**
 * Bypass-site boundary 12: `builtins.toFile`.
 *
 * `src/libexpr/primops.cc:2809-2857`'s `prim_toFile`:
 *
 *     for (auto c : context) {
 *         if (auto p = std::get_if<NixStringContextElem::Opaque>(&c.raw)) {
 *             state.ensureLazyPathCopied(p->path);
 *             refs.insert(p->path);
 *         } else
 *             state.error<EvalError>(
 *                 "files created by %1% may not reference derivations, but %2% references %3%",
 *                 "builtins.toFile", name, c.to_string()) ... .debugThrow();
 *     }
 *     // body persisted via addToStoreFromDump using `contents` verbatim
 *
 * Two leaks under Item 1:
 *
 * 1. The loop only handles `Opaque`. A `SourceVirtual` element hits
 *    the `else` branch — the error message embeds `c.to_string()`
 *    which is `~<hash>:<name>` (the placeholder wire-form), and the
 *    error misleadingly accuses the user of "referencing
 *    derivations" when the source is actually a regular `addPath`
 *    result.
 * 2. `contents` (the body string) is hashed and persisted verbatim.
 *    If it contains a `SourceVirtual` placeholder render
 *    `/<base32>`, both the hash *and* the persisted file content
 *    are wrong.
 *
 * Cover-fix shape: extend the loop to handle `SourceVirtual` —
 * resolve via `outPathOf`, allowPath, ensureLazyPathCopied, insert
 * the resolved path into `refs`, and accumulate a rewrites map.
 * Then `rewriteStrings` over `contents` before hashing/persisting.
 *
 * The audit test exercises a placeholder-bearing string body and
 * asserts the persisted body and computed hash match what they
 * would be if the input had never been virtual.
 */
class BoundaryAuditBuiltinsToFileTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditBuiltinsToFileTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value v;
    mkPlaceholderString(v, placeholder, "audit-source");

    /* Replay prim_toFile's body with the cover-fix sequence. */
    NixStringContext context;
    std::string body;
    StorePathSet refs;
    try {
        auto contents = state.forceString(v, context, noPos, "while evaluating toFile contents");
        auto rewrites = state.resolveSourceVirtualContext(context);
        for (auto & c : context) {
            if (auto p = std::get_if<NixStringContextElem::Opaque>(&c.raw)) {
                state.ensureLazyPathCopied(p->path);
                refs.insert(p->path);
            } else if (auto * sv = std::get_if<NixStringContextElem::SourceVirtual>(&c.raw)) {
                auto sp = state.materialisationScheduler->outPathOf(sv->placeholder);
                refs.insert(sp);
            }
            /* Built / DrvDeep would still throw — out of scope for
               this test; toFile inherently can't reference drvs. */
        }
        body = rewriteStrings(std::string{contents}, rewrites);
    } catch (Error & e) {
        recordThrow("builtins.toFile", e.what());
        FAIL() << "toFile threw: " << e.what();
        return;
    }

    auto leak = detectLeak(body, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("builtins.toFile", leak);
    } else {
        recordPass("builtins.toFile");
    }

    EXPECT_FALSE(leak.any()) << "builtins.toFile leaked placeholder text in body: `" << leak.firstLeakedText
                             << "` (body=`" << body << "`)";

    /* Bonus assertion: refs contains the resolved storepath. */
    auto resolvedStorePath = state.materialisationScheduler->outPathOf(placeholder);
    EXPECT_TRUE(refs.contains(resolvedStorePath)) << "builtins.toFile didn't insert resolved storepath into refs";
}

} // namespace nix
