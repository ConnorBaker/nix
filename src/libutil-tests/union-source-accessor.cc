#include "nix/util/source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/tests/gmock-matchers.hh"
#include "nix/util/tests/source-accessor-gen.hh"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace nix {

/* Directories merge with earlier children winning; a non-directory in an
   earlier child hides the later children's subtrees at and below it. */
TEST(UnionSourceAccessor, overlaySemantics)
{
    auto a = memoryTree({{"d/x", "ax"}, {"f", "af"}, {"both", "a"}});
    auto b = memoryTree({{"d/y", "by"}, {"f/z", "bz"}, {"both", "b"}, {"g", "bg"}});
    auto accessor = makeUnionSourceAccessor({a, b});

    EXPECT_THAT(accessor, testing::HasDirectory(CanonPath::root, std::set<std::string>{"d", "f", "both", "g"}));
    EXPECT_THAT(accessor, testing::HasDirectory(CanonPath("d"), std::set<std::string>{"x", "y"}));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("d/x"), "ax"));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("d/y"), "by"));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("both"), "a"));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("g"), "bg"));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("f"), "af"));
    EXPECT_THROW(accessor->readDirectory(CanonPath("f")), NotADirectory);
    EXPECT_FALSE(accessor->pathExists(CanonPath("f/z")));
    EXPECT_THROW(accessor->readFile(CanonPath("f/z")), FileNotFound);
    EXPECT_FALSE(accessor->pathExists(CanonPath("nope")));
}

/* The general naming rule: a subtree that is one child's alone gets that
   child's name; a merged directory is named from all its parts or not at
   all.  A union whose children are asserted to agree names from the first
   child that can, as the evaluator's impure root does. */
TEST(UnionSourceAccessor, fingerprints)
{
    auto a = memoryTree({{"d/x", "ax"}, {"f", "af"}});
    auto b = memoryTree({{"d/y", "by"}, {"f/z", "bz"}, {"g", "bg"}});
    a->fingerprint = "A";
    b->fingerprint = "B";

    auto general = makeUnionSourceAccessor({a, b});
    EXPECT_EQ(general->getFingerprint(CanonPath("d/x")), std::pair(CanonPath("d/x"), std::optional<std::string>("A")));
    EXPECT_EQ(general->getFingerprint(CanonPath("d/y")), std::pair(CanonPath("d/y"), std::optional<std::string>("B")));
    EXPECT_EQ(general->getFingerprint(CanonPath("g")), std::pair(CanonPath("g"), std::optional<std::string>("B")));
    EXPECT_EQ(general->getFingerprint(CanonPath("f")), std::pair(CanonPath("f"), std::optional<std::string>("A")));
    EXPECT_FALSE(general->getFingerprint(CanonPath("f/z")).second);
    auto atD = general->getFingerprint(CanonPath("d"));
    ASSERT_TRUE(atD.second);
    EXPECT_EQ(atD.first, CanonPath::root);
    EXPECT_NE(*atD.second, "A");
    EXPECT_NE(*atD.second, "B");
    EXPECT_NE(general->getFingerprint(CanonPath::root).second, atD.second);

    b->fingerprint = std::nullopt;
    EXPECT_FALSE(general->getFingerprint(CanonPath("d")).second);
    EXPECT_EQ(general->getFingerprint(CanonPath("d/x")).second, std::optional<std::string>("A"));
    b->fingerprint = "B";

    auto coherent = makeUnionSourceAccessor({a, b}, UnionCoherence::ChildrenAgree);
    EXPECT_EQ(coherent->getFingerprint(CanonPath("d")), std::pair(CanonPath("d"), std::optional<std::string>("A")));
    EXPECT_EQ(coherent->getFingerprint(CanonPath("g")), std::pair(CanonPath("g"), std::optional<std::string>("A")));
    a->fingerprint = std::nullopt;
    EXPECT_EQ(coherent->getFingerprint(CanonPath("d")), std::pair(CanonPath("d"), std::optional<std::string>("B")));
}

} // namespace nix
