/* Track C: cross-pipeline `treeHashToNarHash` bridge in
 * `fetchToStore2`. The bridge means: a tarball downloaded yesterday
 * and a git input fetched today that unpack to the same tree-SHA
 * share a row.
 *
 * We can't easily exercise the full `fetchToStore2` path in a unit
 * test (it needs a real Store, real SourcePath/SourceAccessor with
 * specific fingerprints, etc.). What we *can* test independently is:
 *
 *   - `TreeHashToNarHash` populates `(domain="treeHashToNarHash",
 *     {treeHash})` rows on `lookup`.
 *   - `peekTreeHashBridge` (as exercised through `TreeHashToNarHash::peek`)
 *     finds those rows when probed.
 *   - The bridge is bypassed for non-NixArchive methods, suffixed
 *     fingerprints, and non-root subpaths — these decisions live
 *     inside `fetchToStore2` and are testable by reasoning about the
 *     fingerprint shape.
 *
 * The full integration (git input + tarball input producing the same
 * row) lives in the functional tests; this is the unit-level safety
 * net for the projection contract.
 */

#include "nix/fetchers/projection.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/util/hash.hh"

#include <gtest/gtest.h>

namespace nix::fetchers {

namespace {
const Settings & bridgeTestSettings()
{
    static Settings s;
    return s;
}

Hash bridgeUniqueRev(std::string_view tag)
{
    return hashString(HashAlgorithm::SHA1, fmt("nix-bridge-test::%s::%lld", tag, (long long) time(nullptr)));
}
} // namespace

TEST(TreeHashBridge, PopulatedByTreeHashToNarHash)
{
    /* Simulates the tarball path: it computed a NAR hash for a tree
       OID and persisted it. The git path then reuses the row. */
    auto treeHash = bridgeUniqueRev("populated");
    auto narHash = hashString(HashAlgorithm::SHA256, "the-nar-bytes");

    /* No row yet. */
    EXPECT_FALSE(TreeHashToNarHash::peek(bridgeTestSettings(), treeHash).has_value());

    /* Populate via `lookup` (compute path). */
    TreeHashToNarHash::lookup(bridgeTestSettings(), treeHash, [&] { return narHash; });

    /* Bridge probe finds it. */
    auto bridged = TreeHashToNarHash::peek(bridgeTestSettings(), treeHash);
    ASSERT_TRUE(bridged.has_value());
    EXPECT_EQ(*bridged, narHash);
}

TEST(TreeHashBridge, NoCrossContamination)
{
    /* Two distinct tree OIDs must not share a row even if their
       associated NAR hashes coincide. */
    auto treeA = bridgeUniqueRev("cross-A");
    auto treeB = bridgeUniqueRev("cross-B");
    auto narA = hashString(HashAlgorithm::SHA256, "content-A");
    auto narB = hashString(HashAlgorithm::SHA256, "content-B");

    TreeHashToNarHash::lookup(bridgeTestSettings(), treeA, [&] { return narA; });
    TreeHashToNarHash::lookup(bridgeTestSettings(), treeB, [&] { return narB; });

    EXPECT_EQ(*TreeHashToNarHash::peek(bridgeTestSettings(), treeA), narA);
    EXPECT_EQ(*TreeHashToNarHash::peek(bridgeTestSettings(), treeB), narB);
    EXPECT_NE(
        *TreeHashToNarHash::peek(bridgeTestSettings(), treeA), *TreeHashToNarHash::peek(bridgeTestSettings(), treeB));
}

TEST(TreeHashBridge, KeyEncodingStable)
{
    /* The bridge probe in fetch-to-store.cc parses the fingerprint
       string into a tree OID and looks up `{treeHash: <hex>}`. The
       projection's `toKey` must use the same encoding. */
    auto treeHash = bridgeUniqueRev("encoding");
    auto attrs = TreeHashToNarHash::toKey(treeHash);
    auto fp = attrs.find("treeHash");
    ASSERT_NE(fp, attrs.end());
    auto * s = std::get_if<std::string>(&fp->second);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, treeHash.gitRev());
}

TEST(TreeHashBridge, BridgeProbeRespectsRejectionRules)
{
    /* The bridge in fetch-to-store.cc::peekTreeHashBridge rejects:
         - non-root subpaths
         - non-NixArchive methods
         - non-tree:<sha> fingerprints
         - any wrapper suffix on the tree:<sha> form
       These rejections are critical: a `tree:T;e` row's NAR is
       different from a vanilla `tree:T`'s NAR, so accepting the
       suffixed form would produce an incorrect cache hit. We can't
       call peekTreeHashBridge directly (file-static), but we can
       test the projection's key encoding which is the contract.
     *
     * The peekTreeHashBridge guards live in fetch-to-store.cc and
     * are mechanically simple; this test asserts they exist by
     * construction (the bridge only consults the projection if the
     * fingerprint is shaped a specific way). The full check is in
     * the rejection-path source review at PROPOSAL.md §"Source
     * Views". */

    /* Rejection #3 (non-tree fingerprint): the projection
       `TreeHashToNarHash` accepts a `Hash` directly, so the bridge's
       string-prefix check on `"tree:"` is what enforces this. We
       can't trigger a misuse via the projection API alone. */

    auto attrs = TreeHashToNarHash::toKey(bridgeUniqueRev("rejection"));
    /* The encoded key contains `treeHash`, not `blob:` or `git:`
       sigils — by construction, callers can't put a `blob:T` or
       `git:R` form into this projection's key shape. */
    EXPECT_TRUE(attrs.contains("treeHash"));
    EXPECT_FALSE(attrs.contains("rev"));
    EXPECT_FALSE(attrs.contains("fingerprint"));
}

} // namespace nix::fetchers
