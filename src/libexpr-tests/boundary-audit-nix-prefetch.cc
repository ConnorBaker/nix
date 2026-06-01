#include "boundary-audit-fixture.hh"

namespace nix {

/**
 * Bypass-site boundary 15: `nix-prefetch-url` / `resolveMirrorUrl`.
 *
 * `src/nix/prefetch.cc` reads three context-bearing strings via
 * 3-arg `forceString` and feeds them downstream:
 *
 *   - `resolveMirrorUrl` (line 53, pre-fix): the first element of
 *     a mirror list is concatenated into a URL and fetched. A
 *     `SourceVirtual` placeholder in the body would leak into the
 *     network request as `/<base32>`.
 *   - main `nix-prefetch-url` op (line 230, pre-fix): the first
 *     element of the `urls` list is read into `url` and fed to
 *     `prefetchFile`. Same placeholder-leak shape.
 *   - main `nix-prefetch-url` op (line 238, pre-fix): the
 *     `outputHashMode` body is compared to `"recursive"`. A
 *     `SourceVirtual` placeholder render wouldn't equal the literal
 *     `"recursive"` so the comparison is structurally safe — but
 *     the cover-fix discipline (PROPOSAL.md §6.2) is to thread
 *     the accumulator through every observation site uniformly,
 *     and the line shares the function-scope context with the URL
 *     site at zero extra cost.
 *
 * Cover-fix shape (matches §6.2 canonical: `getFlake`,
 * `hashString`): switch each 3-arg `forceString` to the 4-arg form
 * with a `NixStringContext &` accumulator, then call
 * `resolveSourceVirtualContext` + `ensureLazyPathsCopied` +
 * `rewriteStrings` over the body before the URL is fetched. For
 * `resolveMirrorUrl` this happens once per call (function-local
 * context); for the main op the URL and hash-mode reads share one
 * context and resolve once.
 *
 * The audit replays the main-op shape on a placeholder-bearing URL
 * value, drives the same `forceString` + resolve + rewrite +
 * `rewriteStrings` sequence, and asserts the resulting URL string
 * contains no placeholder text. This is regression-gate framing
 * (PROPOSAL.md §6.2): if the cover-fix is removed or skewed
 * (3-arg `forceString` reintroduced, resolve step elided),
 * `detectLeak` flags the placeholder render in the URL bytes.
 */
class BoundaryAuditNixPrefetchTest : public BoundaryAuditTest
{};

TEST_F(BoundaryAuditNixPrefetchTest, NoLeak)
{
    auto placeholder = mintPlaceholder();
    Value urlValue;
    mkPlaceholderString(urlValue, placeholder, "audit-source");

    /* Replay nix-prefetch-url's main-op cover-fix sequence: thread
       a single `NixStringContext` through `forceString`, resolve
       and rewrite before the URL is used. */
    NixStringContext context;
    std::string url;
    try {
        auto urlView = state.forceString(urlValue, context, noPos, "while evaluating the first url from the urls list");
        std::string urlRaw{urlView};
        auto rewrites = state.resolveSourceVirtualContext(context);
        state.ensureLazyPathsCopied(context);
        url = rewriteStrings(std::move(urlRaw), rewrites);
    } catch (Error & e) {
        recordThrow("nix-prefetch-url url", e.what());
        FAIL() << "nix-prefetch-url cover-fix replay threw: " << e.what();
        return;
    }

    auto leak = detectLeak(url, placeholder, "audit-source");
    if (leak.any()) {
        recordLeak("nix-prefetch-url url", leak);
    } else {
        recordPass("nix-prefetch-url url");
    }

    EXPECT_FALSE(leak.any()) << "nix-prefetch-url url leaked placeholder text after cover-fix replay: `"
                             << leak.firstLeakedText << "` (url=`" << url << "`)";
}

} // namespace nix
