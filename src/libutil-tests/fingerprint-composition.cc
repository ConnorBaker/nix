/* Tests for the wrapper-fingerprint composition algebra (track B).
 *
 * The proposal makes three claims about `composeFingerprint`:
 *
 *   1. When the inner accessor returns a content-keyed fingerprint,
 *      the wrapper appends `computeOwnSuffix(path)` to it.
 *   2. When the inner accessor returns no fingerprint, the wrapper
 *      falls back to its own `fingerprint` field (master's
 *      pre-existing "outermost wrapper carries the input-level
 *      identity" pattern, preserved for workdir inputs).
 *   3. When `computeOwnSuffix` returns nullopt, we bypass the cache.
 *
 * These tests exercise each branch with mock accessors so we don't
 * need a real Git repo. Two-process determinism (R2): each `compose`
 * call receives byte-identical inputs and produces byte-identical
 * output.
 */

#include "nix/util/source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"

#include <gtest/gtest.h>

namespace nix {

/* A leaf accessor that hands out a configurable subpath-aware
   fingerprint. Mirrors what `GitSourceAccessor::getFingerprint` does
   for tree/blob branches. */
struct FpLeaf : MemorySourceAccessor
{
    std::optional<std::string> subpathFp;

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        if (path.isRoot())
            return {path, fingerprint};
        if (subpathFp)
            return {CanonPath::root, subpathFp};
        return {path, fingerprint};
    }
};

/* A wrapper that always allows access and exposes a customisable own-suffix. */
struct FpWrapper : SourceAccessor
{
    ref<SourceAccessor> next;
    std::optional<std::string> ownSuffix;

    explicit FpWrapper(ref<SourceAccessor> next_)
        : next(std::move(next_))
    {
    }

    std::optional<Stat> maybeLstat(const CanonPath & p) override
    {
        return next->maybeLstat(p);
    }

    DirEntries readDirectory(const CanonPath & p) override
    {
        return next->readDirectory(p);
    }

    std::string readLink(const CanonPath & p) override
    {
        return next->readLink(p);
    }

    std::optional<std::string> computeOwnSuffix(const CanonPath &) override
    {
        return ownSuffix;
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        return composeFingerprint(*this, *next, path, path);
    }
};

TEST(FingerprintComposition, ContentKeyedInnerGetsSuffixAppended)
{
    auto leaf = make_ref<FpLeaf>();
    leaf->subpathFp = "tree:T";
    auto wrapper = make_ref<FpWrapper>(leaf);
    wrapper->ownSuffix = ";a=H;e";

    auto [returnedPath, fp] = wrapper->getFingerprint(CanonPath("/sub"));
    EXPECT_EQ(returnedPath, CanonPath::root);
    ASSERT_TRUE(fp.has_value());
    EXPECT_EQ(*fp, "tree:T;a=H;e");
}

TEST(FingerprintComposition, NoInnerFingerprintFallsBackToWrapperField)
{
    /* Workdir-style: leaf has no content-keyed identity; wrapper
       carries the input-level fingerprint in its field. */
    auto leaf = make_ref<FpLeaf>();
    auto wrapper = make_ref<FpWrapper>(leaf);
    wrapper->fingerprint = "git:R;d=H";

    auto [returnedPath, fp] = wrapper->getFingerprint(CanonPath("/sub"));
    EXPECT_EQ(returnedPath, CanonPath("/sub"));
    ASSERT_TRUE(fp.has_value());
    EXPECT_EQ(*fp, "git:R;d=H");
}

TEST(FingerprintComposition, OwnSuffixNulloptBypassesCache)
{
    auto leaf = make_ref<FpLeaf>();
    leaf->subpathFp = "tree:T";
    auto wrapper = make_ref<FpWrapper>(leaf);
    wrapper->ownSuffix = std::nullopt;

    auto [returnedPath, fp] = wrapper->getFingerprint(CanonPath("/sub"));
    EXPECT_FALSE(fp.has_value());
}

TEST(FingerprintComposition, EmptyOwnSuffixForwardsInnerUnchanged)
{
    auto leaf = make_ref<FpLeaf>();
    leaf->subpathFp = "blob:B;m=100644";
    auto wrapper = make_ref<FpWrapper>(leaf);
    wrapper->ownSuffix = std::string("");

    auto [returnedPath, fp] = wrapper->getFingerprint(CanonPath("/foo/bar"));
    EXPECT_EQ(returnedPath, CanonPath::root);
    ASSERT_TRUE(fp.has_value());
    EXPECT_EQ(*fp, "blob:B;m=100644");
}

TEST(FingerprintComposition, TwoWrappersCommute)
{
    /* Stack-order independence: two wrappers with distinct tags, in
       either nesting, produce byte-identical fingerprints. */
    auto leaf = make_ref<FpLeaf>();
    leaf->subpathFp = "tree:T";

    auto inner = make_ref<FpWrapper>(leaf);
    inner->ownSuffix = ";e";

    auto outer = make_ref<FpWrapper>(inner);
    outer->ownSuffix = ";l";

    auto [_, fp1] = outer->getFingerprint(CanonPath("/x"));

    auto inner2 = make_ref<FpWrapper>(leaf);
    inner2->ownSuffix = ";l";

    auto outer2 = make_ref<FpWrapper>(inner2);
    outer2->ownSuffix = ";e";

    auto [__, fp2] = outer2->getFingerprint(CanonPath("/x"));

    ASSERT_TRUE(fp1.has_value());
    ASSERT_TRUE(fp2.has_value());
    EXPECT_EQ(*fp1, *fp2);
    EXPECT_EQ(*fp1, "tree:T;e;l");
}

} // namespace nix
