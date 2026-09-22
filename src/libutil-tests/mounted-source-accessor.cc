#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/tests/gmock-matchers.hh"
#include "nix/util/tests/source-accessor-gen.hh"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#ifndef _WIN32
#  include <unistd.h>
#endif

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

    EXPECT_EQ(accessor->getFingerprint(CanonPath("a/x")), std::pair(CanonPath("a/x"), std::string("R")));
    EXPECT_EQ(accessor->getFingerprint(CanonPath("m/n/y")), std::pair(CanonPath("y"), std::string("U")));
    EXPECT_EQ(accessor->getFingerprint(CanonPath("m/n")), std::pair(CanonPath::root, std::string("U")));

    auto atM = accessor->getFingerprint(CanonPath("m"));
    auto atRoot = accessor->getFingerprint(CanonPath::root);
    ASSERT_TRUE(atM);
    ASSERT_TRUE(atRoot);
    EXPECT_EQ(atM->first, CanonPath::root);
    EXPECT_NE(atM->second, "R");
    EXPECT_NE(atM->second, atRoot->second);

    /* The same parts give the same name; a different mounted tree a different one. */
    auto again = makeMountedSourceAccessor({{CanonPath::root, root}, {CanonPath("m/n"), mounted}});
    EXPECT_EQ(again->getFingerprint(CanonPath("m")), atM);
    auto other = memoryTree({{"z", "z"}});
    other->fingerprint = "V";
    auto different = makeMountedSourceAccessor({{CanonPath::root, root}, {CanonPath("m/n"), other}});
    EXPECT_NE(different->getFingerprint(CanonPath("m")).value().second, atM->second);

    /* An unnamed part leaves the composite unnamed. */
    mounted->fingerprint = std::nullopt;
    EXPECT_FALSE(accessor->getFingerprint(CanonPath("m")));
    mounted->fingerprint = "U";
    root->fingerprint = std::nullopt;
    EXPECT_FALSE(accessor->getFingerprint(CanonPath("m")));
    EXPECT_EQ(accessor->getFingerprint(CanonPath("m/n/y")).value().second, std::string("U"));
}

namespace {

struct MountedCallbackException
{};

struct MountedOwnershipLost
{};

struct MountedOwnershipProbe : MemorySourceAccessor
{
    std::function<void()> callback;
    bool present = false;

    void anchor() override {}

    std::optional<Stat> maybeLstat(const CanonPath &) override
    {
        auto result = present ? std::optional<Stat>{Stat{.type = tDirectory}} : std::nullopt;
        auto invoke = callback;
        invoke();
        return result;
    }

    std::string showPath(const CanonPath &) override
    {
        auto invoke = callback;
        invoke();
        return "child";
    }

    void invalidateCache() override
    {
        auto invoke = callback;
        invoke();
    }
};

/* No probe member is accessed after its copied callback. Invalidation
   ownership cells exercise the fixed snapshot; the old locked-map body
   is counterchecked only by the separate, retained-owner reentry test. */
void checkMountedInvalidation(unsigned releaseAt, bool release, bool throwing, bool reenter)
{
    std::shared_ptr<MountedSourceAccessor> owner;
    std::vector<std::weak_ptr<SourceAccessor>> children;
    unsigned calls = 0, addedCalls = 0;
    {
        std::map<CanonPath, ref<SourceAccessor>> mounts;
        for (auto path : {CanonPath::root, CanonPath("first"), CanonPath("second")}) {
            auto child = make_ref<MountedOwnershipProbe>();
            children.push_back(child.get_ptr());
            child->callback = [&, path] {
                ++calls;
                if (reenter) {
                    /* Reentry is exercised with a retained wrapper. The
                       selected key is precisely the key a map visitor
                       would still hold while invoking this callback. */
                    auto selected = owner->getMount(path);
                    ASSERT_TRUE(selected);
                    owner->mount(path, ref<SourceAccessor>(selected));
                    if (calls == 1) {
                        auto added = make_ref<MountedOwnershipProbe>();
                        added->callback = [&] { ++addedCalls; };
                        owner->mount(CanonPath("added"), added);
                    }
                }
                if (calls == releaseAt && release)
                    owner.reset();
                for (auto & child : children) {
                    auto alive = !child.expired();
                    EXPECT_TRUE(alive);
                    if (!alive)
                        throw MountedOwnershipLost{};
                }
                if (calls == releaseAt && throwing)
                    throw MountedCallbackException{};
            };
            mounts.emplace(path, child);
        }
        owner = makeMountedSourceAccessor(std::move(mounts)).get_ptr();
    }
    std::weak_ptr<SourceAccessor> weakOwner = owner;
    auto * accessor = owner.get();
    if (throwing)
        EXPECT_THROW(accessor->invalidateCache(), MountedCallbackException);
    else
        EXPECT_NO_THROW(accessor->invalidateCache());
    EXPECT_EQ(calls, throwing ? releaseAt : 3u);
    EXPECT_EQ(weakOwner.expired(), release);
    for (auto & child : children)
        EXPECT_EQ(child.expired(), release);
    if (reenter) {
        /* A new mount is outside the already collected snapshot. It is
           included by the next invalidation, even after an exception. */
        EXPECT_EQ(addedCalls, 0u);
        calls = 0;
        releaseAt = 0;
        EXPECT_NO_THROW(owner->invalidateCache());
        EXPECT_EQ(calls, 3u);
        EXPECT_EQ(addedCalls, 1u);
    }
    owner.reset();
    EXPECT_TRUE(weakOwner.expired());
    for (auto & child : children)
        EXPECT_TRUE(child.expired());
}

} // namespace

TEST(MountedSourceAccessor, statCallbacksRetainTheFallbackOwner)
{
    for (bool present : {false, true})
        for (bool descendant : {false, true})
            for (bool release : {false, true})
                for (bool throwing : {false, true}) {
                    SCOPED_TRACE(
                        ::testing::Message() << "present=" << present << " descendant=" << descendant
                                             << " release=" << release << " throwing=" << throwing);
                    auto child = std::make_shared<MountedOwnershipProbe>();
                    child->present = present;
                    std::weak_ptr<SourceAccessor> weakChild = child;
                    std::shared_ptr<MountedSourceAccessor> owner =
                        makeMountedSourceAccessor({{CanonPath::root, ref<SourceAccessor>(child)}}).get_ptr();
                    if (descendant)
                        owner->mount(CanonPath("missing/mounted"), makeEmptySourceAccessor());
                    std::weak_ptr<SourceAccessor> weakOwner = owner;
                    unsigned calls = 0;
                    child->callback = [&] {
                        ++calls;
                        if (release)
                            owner.reset();
                        auto childAlive = !weakChild.expired();
                        auto ownerAlive = !weakOwner.expired();
                        EXPECT_TRUE(childAlive);
                        if (!present)
                            EXPECT_TRUE(ownerAlive);
                        if (!childAlive || (!present && !ownerAlive))
                            throw MountedOwnershipLost{};
                        if (throwing)
                            throw MountedCallbackException{};
                    };
                    child.reset();
                    auto * accessor = owner.get();
                    auto run = [&] {
                        auto result = accessor->maybeLstat(CanonPath("missing"));
                        EXPECT_EQ(result.has_value(), present || descendant);
                        if (result)
                            EXPECT_EQ(result->type, SourceAccessor::tDirectory);
                    };
                    if (throwing)
                        EXPECT_THROW(run(), MountedCallbackException);
                    else
                        EXPECT_NO_THROW(run());
                    EXPECT_EQ(calls, 1u);
                    EXPECT_EQ(weakOwner.expired(), release);
                    EXPECT_EQ(weakChild.expired(), release);
                    owner.reset();
                    EXPECT_TRUE(weakOwner.expired());
                    EXPECT_TRUE(weakChild.expired());
                }
}

TEST(MountedSourceAccessor, displayCallbacksRetainTheDisplayOwner)
{
    for (bool release : {false, true})
        for (bool throwing : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "release=" << release << " throwing=" << throwing);
            auto child = std::make_shared<MountedOwnershipProbe>();
            std::weak_ptr<SourceAccessor> weakChild = child;
            std::shared_ptr<MountedSourceAccessor> owner =
                makeMountedSourceAccessor({{CanonPath::root, ref<SourceAccessor>(child)}}).get_ptr();
            owner->setPathDisplay("prefix<", ">suffix");
            std::weak_ptr<SourceAccessor> weakOwner = owner;
            unsigned calls = 0;
            child->callback = [&] {
                ++calls;
                if (release)
                    owner.reset();
                auto childAlive = !weakChild.expired();
                auto ownerAlive = !weakOwner.expired();
                EXPECT_TRUE(childAlive);
                EXPECT_TRUE(ownerAlive);
                if (!childAlive || !ownerAlive)
                    throw MountedOwnershipLost{};
                if (throwing)
                    throw MountedCallbackException{};
            };
            child.reset();
            auto * accessor = owner.get();
            if (throwing)
                EXPECT_THROW(accessor->showPath(CanonPath::root), MountedCallbackException);
            else
                EXPECT_EQ(accessor->showPath(CanonPath::root), "prefix<child>suffix");
            EXPECT_EQ(calls, 1u);
            EXPECT_EQ(weakOwner.expired(), release);
            EXPECT_EQ(weakChild.expired(), release);
            owner.reset();
            EXPECT_TRUE(weakOwner.expired());
            EXPECT_TRUE(weakChild.expired());
        }
}

TEST(MountedSourceAccessor, invalidationRetainsTheCollectedChildren)
{
    /* Child order is unspecified: first and last refer to callback order,
       not hash-table iteration order. Each initial child remains owned. */
    for (unsigned releaseAt : {1u, 3u})
        for (bool release : {false, true})
            for (bool throwing : {false, true}) {
                SCOPED_TRACE(
                    ::testing::Message() << "at=" << releaseAt << " release=" << release << " throwing=" << throwing);
                checkMountedInvalidation(releaseAt, release, throwing, false);
            }
}

TEST(MountedSourceAccessor, invalidationAllowsReentryAfterTheSnapshot)
{
#ifdef _WIN32
    GTEST_SKIP() << "The negative map-lock control requires a bounded Unix subprocess";
#else
    /* Keep the wrapper alive in this separate countercheck. Restoring
       locked visitation may block reentry, but cannot destroy a locked
       map. The alarm bounds that regression without sleeping. */
    for (unsigned callbackAt : {1u, 3u})
        for (bool throwing : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "at=" << callbackAt << " throwing=" << throwing);
            EXPECT_EXIT(
                {
                    alarm(5);
                    checkMountedInvalidation(callbackAt, false, throwing, true);
                    _exit(::testing::Test::HasFailure() ? 1 : 0);
                },
                ::testing::ExitedWithCode(0),
                "");
        }
#endif
}

} // namespace nix
