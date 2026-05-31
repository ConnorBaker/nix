/* Tests for `SourceContentId` and `SourcePlaceholder` (Track O').
 *
 * The cargo-workspace property: same inputs anywhere produce the
 * same contentId and the same placeholder. **No call-site identity.**
 */

#include "nix/store/content-address.hh"
#include "nix/store/source-content-id.hh"
#include "nix/store/source-placeholder.hh"
#include "nix/util/hash.hh"

#include <gtest/gtest.h>

namespace nix {

namespace {
StoreReferences emptyRefs()
{
    return StoreReferences{};
}
} // namespace

TEST(SourceContentId, SameInputsSameId)
{
    auto fp = "git:abc";
    auto shape = hashString(HashAlgorithm::SHA256, "shape-bytes");
    auto method = ContentAddressMethod::Raw::NixArchive;
    auto refs = emptyRefs();

    auto a = SourceContentId::compute(fp, shape, method, refs);
    auto b = SourceContentId::compute(fp, shape, method, refs);
    EXPECT_EQ(a, b);
}

TEST(SourceContentId, DifferentFingerprintsDifferentIds)
{
    auto shape = hashString(HashAlgorithm::SHA256, "shape");
    auto method = ContentAddressMethod::Raw::NixArchive;
    auto refs = emptyRefs();

    auto a = SourceContentId::compute("git:r1", shape, method, refs);
    auto b = SourceContentId::compute("git:r2", shape, method, refs);
    EXPECT_NE(a, b);
}

TEST(SourceContentId, DifferentShapesDifferentIds)
{
    auto fp = "git:R";
    auto method = ContentAddressMethod::Raw::NixArchive;
    auto refs = emptyRefs();

    auto a = SourceContentId::compute(fp, hashString(HashAlgorithm::SHA256, "A"), method, refs);
    auto b = SourceContentId::compute(fp, hashString(HashAlgorithm::SHA256, "B"), method, refs);
    EXPECT_NE(a, b);
}

TEST(SourceContentId, NameIsNotInTheKey)
{
    /* The cargo-workspace property: contentId does NOT depend on
       output name. Two `builtins.path { name = ... }` calls with
       different names but identical content share the contentId. */
    auto fp = "git:R";
    auto shape = hashString(HashAlgorithm::SHA256, "S");
    auto method = ContentAddressMethod::Raw::NixArchive;
    auto refs = emptyRefs();

    auto a = SourceContentId::compute(fp, shape, method, refs);
    /* No name parameter; this is a static check that the API
       doesn't take one. If a future regression added one, this
       test would fail to compile. */
    auto b = SourceContentId::compute(fp, shape, method, refs);
    EXPECT_EQ(a, b);
}

/* ---------- SourcePlaceholder ---------- */

TEST(SourcePlaceholder, SameContentIdSameNameSamePlaceholder)
{
    /* Two calls: identical contentId AND identical name. Same
       placeholder string, render-by-render. */
    auto fp = "git:R";
    auto shape = hashString(HashAlgorithm::SHA256, "S");
    auto cid = SourceContentId::compute(fp, shape, ContentAddressMethod::Raw::NixArchive, emptyRefs());

    auto p1 = SourcePlaceholder::make(cid, "foo");
    auto p2 = SourcePlaceholder::make(cid, "foo");
    EXPECT_EQ(p1, p2);
    EXPECT_EQ(p1.render(), p2.render());
}

TEST(SourcePlaceholder, SameContentIdDifferentNameDifferentPlaceholder)
{
    /* The cargo workspace's 200 packages share contentId but
       have distinct names → distinct placeholders, each unique
       per package. */
    auto fp = "git:R";
    auto shape = hashString(HashAlgorithm::SHA256, "S");
    auto cid = SourceContentId::compute(fp, shape, ContentAddressMethod::Raw::NixArchive, emptyRefs());

    auto pkg1 = SourcePlaceholder::make(cid, "cargo-pkg-foo");
    auto pkg2 = SourcePlaceholder::make(cid, "cargo-pkg-bar");
    EXPECT_NE(pkg1, pkg2);
    EXPECT_NE(pkg1.render(), pkg2.render());
}

TEST(SourcePlaceholder, RenderShapeMatchesDownstreamPlaceholder)
{
    /* Mirror DownstreamPlaceholder::render: leading slash plus a
       base32-encoded SHA-256 (52 chars), no name suffix. Opaque to
       consumers. Distinguishes placeholders from real
       /nix/store/<hash>-<name> paths so error messages and
       rewriting can pattern-match. */
    auto cid = SourceContentId::compute(
        "g", hashString(HashAlgorithm::SHA256, "x"), ContentAddressMethod::Raw::NixArchive, emptyRefs());
    auto p = SourcePlaceholder::make(cid, "name");
    auto rendered = p.render();
    EXPECT_EQ(rendered.size(), 53u); // '/' + 52 base32 chars (SHA-256 in Nix32)
    EXPECT_EQ(rendered.front(), '/');
    /* No '-' suffix for name. Plain hash format. */
    EXPECT_EQ(rendered.find('-'), std::string::npos);
}

TEST(SourcePlaceholder, ParseRoundTrip)
{
    auto cid = SourceContentId::compute(
        "g", hashString(HashAlgorithm::SHA256, "x"), ContentAddressMethod::Raw::NixArchive, emptyRefs());
    auto original = SourcePlaceholder::make(cid, "n");
    auto rendered = original.render();
    auto parsed = SourcePlaceholder::tryParse(rendered);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, original);
}

TEST(SourcePlaceholder, ParseRejectsRealStorePaths)
{
    /* A real store path has a '-name' suffix; tryParse must reject
       it so we don't confuse them. */
    EXPECT_FALSE(SourcePlaceholder::tryParse("/nix/store/abcdefghijklmnopqrstuvwxyz012345-source").has_value());
    EXPECT_FALSE(SourcePlaceholder::tryParse("/abcdefghijklmnopqrstuvwxyz012345-source").has_value());
    EXPECT_FALSE(SourcePlaceholder::tryParse("not-a-path").has_value());
    EXPECT_FALSE(SourcePlaceholder::tryParse("").has_value());
}

/* ---------- The cargo-workspace property end-to-end ---------- */

TEST(SourcePlaceholder, CargoWorkspaceSharingPattern)
{
    /* Simulate 200 cargo packages in a workspace, each calling
       builtins.path with the same source + same filter shape
       (same contentId), differing only by name. */
    auto fp = "git:workspace-source";
    auto shape = hashString(HashAlgorithm::SHA256, "exclude-target-dir");
    auto cid = SourceContentId::compute(fp, shape, ContentAddressMethod::Raw::NixArchive, emptyRefs());

    std::vector<SourcePlaceholder> placeholders;
    for (int i = 0; i < 200; ++i)
        placeholders.push_back(SourcePlaceholder::make(cid, fmt("pkg-%d", i)));

    /* All 200 share the contentId we'd use to dedup walks. */
    for (auto & p : placeholders) {
        /* tryParse round-trips through the rendered form.
           Implicitly: every placeholder is a valid placeholder. */
        auto parsed = SourcePlaceholder::tryParse(p.render());
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, p);
    }

    /* But: 200 distinct rendered strings — names differ. */
    std::set<std::string> rendered;
    for (auto & p : placeholders)
        rendered.insert(p.render());
    EXPECT_EQ(rendered.size(), 200u);
}

} // namespace nix
