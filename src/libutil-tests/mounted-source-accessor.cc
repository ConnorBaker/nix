#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/tests/gmock-matchers.hh"
#include "nix/util/tests/source-accessor-gen.hh"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace nix {

/* A mount point is listed in its parent, the directories leading to it
   exist, and below it only the mounted tree is consulted. */
TEST(MountedSourceAccessor, graftIsATree)
{
    auto root = memoryTree({{"a/x", "x"}});
    auto mounted = memoryTree({{"y", "y"}});
    auto accessor = makeMountedSourceAccessor({{CanonPath::root, root}, {CanonPath("m/n"), mounted}});

    EXPECT_THAT(accessor, testing::HasDirectory(CanonPath::root, std::set<std::string>{"a", "m"}));
    EXPECT_THAT(accessor, testing::HasDirectory(CanonPath("m"), std::set<std::string>{"n"}));
    EXPECT_THAT(accessor, testing::HasDirectory(CanonPath("m/n"), std::set<std::string>{"y"}));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("m/n/y"), "y"));
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("a/x"), "x"));
    EXPECT_EQ(accessor->lstat(CanonPath("m")).type, SourceAccessor::tDirectory);
    EXPECT_FALSE(accessor->pathExists(CanonPath("m/zz")));
    EXPECT_FALSE(accessor->pathExists(CanonPath("m/n/zz")));
    EXPECT_THROW(accessor->readDirectory(CanonPath("nope")), FileNotFound);
    EXPECT_THROW(accessor->readDirectory(CanonPath("a/x")), NotADirectory);
}

/* An entry the underlying tree already has at the mount point (a gitlink
   rendered as a directory, for instance) is listed once. */
TEST(MountedSourceAccessor, mountOverExistingEntryIsListedOnce)
{
    auto root = memoryTree({{"m/n/placeholder", ""}, {"m/other", ""}});
    auto mounted = memoryTree({{"y", "y"}});
    auto accessor = makeMountedSourceAccessor({{CanonPath::root, root}, {CanonPath("m/n"), mounted}});

    EXPECT_THAT(accessor, testing::HasDirectory(CanonPath("m"), std::set<std::string>{"n", "other"}));
    EXPECT_THAT(accessor, testing::HasDirectory(CanonPath("m/n"), std::set<std::string>{"y"}));
    EXPECT_FALSE(accessor->pathExists(CanonPath("m/n/placeholder")));
}

/* A missing path with nothing mounted below it is the resolved tree's business,
   including which error it raises: an access-control accessor explains itself
   through `lstat`, and that explanation must not be replaced by a generic one. */
namespace {

struct ExplainedError : Error
{
    using Error::Error;
};

struct ExplainingAccessor : MemorySourceAccessor
{
    void anchor() override {}

    Stat lstat(const CanonPath & path) override
    {
        if (auto st = maybeLstat(path))
            return *st;
        throw ExplainedError("path '%s' is missing for a reason", path.abs());
    }
};

} // namespace

TEST(MountedSourceAccessor, missingPathRaisesTheResolvedTreesError)
{
    auto root = make_ref<ExplainingAccessor>();
    {
        MemorySink sink{*root};
        sink.createDirectory(CanonPath::root);
    }
    auto mounted = memoryTree({{"y", "y"}});
    auto accessor = makeMountedSourceAccessor({{CanonPath::root, root}, {CanonPath("m/n"), mounted}});

    EXPECT_THROW(accessor->lstat(CanonPath("missing")), ExplainedError);
    EXPECT_FALSE(accessor->maybeLstat(CanonPath("missing")));
    /* Above a mount point the directory is synthesised, whatever the resolved tree says. */
    EXPECT_EQ(accessor->lstat(CanonPath("m")).type, SourceAccessor::tDirectory);
}

TEST(MountedSourceAccessor, mountingTwiceKeepsTheFirst)
{
    auto root = memoryTree({});
    auto first = memoryTree({{"f", "1"}});
    auto second = memoryTree({{"f", "2"}});
    auto accessor = makeMountedSourceAccessor({{CanonPath::root, root}, {CanonPath("m"), first}});
    accessor->mount(CanonPath("m"), second);
    EXPECT_THAT(accessor, testing::HasContents(CanonPath("m/f"), "1"));
}

/* The graft naming rule: below a mount point the mounted tree's name; away
   from all mount points the underlying tree's name; above a mount point a
   name built from all the parts, or none if any part is unnamed. */
TEST(MountedSourceAccessor, fingerprints)
{
    auto root = memoryTree({{"a/x", "x"}});
    auto mounted = memoryTree({{"y", "y"}});
    root->fingerprint = "R";
    mounted->fingerprint = "U";
    auto accessor = makeMountedSourceAccessor({{CanonPath::root, root}, {CanonPath("m/n"), mounted}});

    EXPECT_EQ(accessor->getFingerprint(CanonPath("a/x")), std::pair(CanonPath("a/x"), std::optional<std::string>("R")));
    EXPECT_EQ(accessor->getFingerprint(CanonPath("m/n/y")), std::pair(CanonPath("y"), std::optional<std::string>("U")));
    EXPECT_EQ(accessor->getFingerprint(CanonPath("m/n")), std::pair(CanonPath::root, std::optional<std::string>("U")));

    auto atM = accessor->getFingerprint(CanonPath("m"));
    auto atRoot = accessor->getFingerprint(CanonPath::root);
    ASSERT_TRUE(atM.second);
    ASSERT_TRUE(atRoot.second);
    EXPECT_EQ(atM.first, CanonPath::root);
    EXPECT_NE(*atM.second, "R");
    EXPECT_NE(*atM.second, *atRoot.second);

    /* The same parts give the same name; a different mounted tree a different one. */
    auto again = makeMountedSourceAccessor({{CanonPath::root, root}, {CanonPath("m/n"), mounted}});
    EXPECT_EQ(again->getFingerprint(CanonPath("m")), atM);
    auto other = memoryTree({{"z", "z"}});
    other->fingerprint = "V";
    auto different = makeMountedSourceAccessor({{CanonPath::root, root}, {CanonPath("m/n"), other}});
    EXPECT_NE(different->getFingerprint(CanonPath("m")).second, atM.second);

    /* An unnamed part leaves the composite unnamed. */
    mounted->fingerprint = std::nullopt;
    EXPECT_FALSE(accessor->getFingerprint(CanonPath("m")).second);
    mounted->fingerprint = "U";
    root->fingerprint = std::nullopt;
    EXPECT_FALSE(accessor->getFingerprint(CanonPath("m")).second);
    EXPECT_EQ(accessor->getFingerprint(CanonPath("m/n/y")).second, std::optional<std::string>("U"));
}

} // namespace nix
