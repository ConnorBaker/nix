#include "nix/util/fingerprint.hh"
#include "nix/util/error.hh"

#include <gtest/gtest.h>

namespace nix {

/* ---------- mergeFingerprintSuffix: alphabetical splice ---------- */

TEST(Fingerprint, MergeIntoBareRoot)
{
    /* No existing suffixes: append. */
    EXPECT_EQ(mergeFingerprintSuffix("git:abc", ";e"), "git:abc;e");
    EXPECT_EQ(mergeFingerprintSuffix("tree:def", ";l"), "tree:def;l");
}

TEST(Fingerprint, MergeKeepsAlphabeticalOrder)
{
    /* Existing `;l` and we splice `;e` — `;e` lands before `;l`. */
    EXPECT_EQ(mergeFingerprintSuffix("git:abc;l", ";e"), "git:abc;e;l");
    /* Existing `;e` and we splice `;l` — appended. */
    EXPECT_EQ(mergeFingerprintSuffix("git:abc;e", ";l"), "git:abc;e;l");
    /* Existing `;e;l`, splice `;d=H`. */
    EXPECT_EQ(mergeFingerprintSuffix("git:abc;e;l", ";d=H"), "git:abc;d=H;e;l");
}

TEST(Fingerprint, MergeWithValuedSuffix)
{
    EXPECT_EQ(mergeFingerprintSuffix("blob:abc;m=100644", ";a=H"), "blob:abc;a=H;m=100644");
}

TEST(Fingerprint, MergeRefusesDuplicateTag)
{
    /* The wrapper-stack-order independence law: two wrappers competing
       for the same identity dimension is a programming error. */
    EXPECT_THROW(mergeFingerprintSuffix("git:abc;e", ";e"), Error);
    EXPECT_THROW(mergeFingerprintSuffix("git:abc;a=X", ";a=Y"), Error);
}

TEST(Fingerprint, MergeRefusesNonSuffix)
{
    EXPECT_THROW(mergeFingerprintSuffix("git:abc", "e"), Error);
}

TEST(Fingerprint, MergeEmptyIsNoop)
{
    EXPECT_EQ(mergeFingerprintSuffix("git:abc;e", ""), "git:abc;e");
}

/* ---------- Wrapper-stack-order independence (the soundness law) ---------- */

TEST(Fingerprint, StackOrderInvariant)
{
    /* Two wrappers append `;a=H` and `;e`; either nesting yields the
       same canonical form. */
    auto inside_then_outside = mergeFingerprintSuffix(mergeFingerprintSuffix("tree:T", ";a=H"), ";e");
    auto outside_then_inside = mergeFingerprintSuffix(mergeFingerprintSuffix("tree:T", ";e"), ";a=H");
    EXPECT_EQ(inside_then_outside, outside_then_inside);
    EXPECT_EQ(inside_then_outside, "tree:T;a=H;e");
}

/* ---------- Helpers: tree:/blob: rendering ---------- */

TEST(Fingerprint, BlobAndTreeRendering)
{
    EXPECT_EQ(treeFingerprint("47665c66"), "tree:47665c66");
    EXPECT_EQ(blobFingerprint("17b4ccef", "100644"), "blob:17b4ccef;m=100644");
    EXPECT_EQ(blobFingerprint("17b4ccef", "100755"), "blob:17b4ccef;m=100755");
    EXPECT_EQ(blobFingerprint("17b4ccef", "120000"), "blob:17b4ccef;m=120000");
}

/* ---------- Idempotency / repeated-merge ---------- */

TEST(Fingerprint, SuffixesWithValuesPreserveValueAlongsideTag)
{
    /* The valued form `;a=H` must keep the value through the merge. */
    auto out = mergeFingerprintSuffix("git:abc", ";a=somehash");
    EXPECT_EQ(out, "git:abc;a=somehash");

    /* Still merges in alphabetical position. */
    auto out2 = mergeFingerprintSuffix(out, ";e");
    EXPECT_EQ(out2, "git:abc;a=somehash;e");
}

TEST(Fingerprint, MergeIntoFingerprintWithMultipleExistingSuffixes)
{
    /* `git:R;a=H;e;l;s` — splice in `;d=DIRTYHASH`. d < e
       alphabetically, so it lands between a= and e. */
    auto out = mergeFingerprintSuffix("git:R;a=H;e;l;s", ";d=DH");
    EXPECT_EQ(out, "git:R;a=H;d=DH;e;l;s");
}

TEST(Fingerprint, MergePreservesExistingValuedSuffixes)
{
    /* Existing valued suffix must round-trip its value verbatim. */
    auto out = mergeFingerprintSuffix("tree:T;a=H1", ";l");
    EXPECT_EQ(out, "tree:T;a=H1;l");
}

/* ---------- Negative cases beyond the basic API checks ---------- */

TEST(Fingerprint, MergeRefusesEmptyFingerprintRoot)
{
    /* Empty input is degenerate; we tolerate it as "no root tag,
       just a suffix list" since some early-construction paths
       could produce it. The output is just the bare suffix. */
    auto out = mergeFingerprintSuffix("", ";e");
    EXPECT_EQ(out, ";e");
}

TEST(Fingerprint, MergeWithSuffixContainingHexValue)
{
    /* `;d=<hex>` is the dirty-workdir hash. Verify the `=` in the
       value isn't mistakenly treated as a tag separator. */
    auto out = mergeFingerprintSuffix("git:R", ";d=abcdef0123456789");
    EXPECT_EQ(out, "git:R;d=abcdef0123456789");
}

} // namespace nix
