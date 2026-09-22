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
    /* The missing fallback at m contributes no contents; the surviving
       root directory still needs its own name. */
    EXPECT_EQ(accessor->getFingerprint(CanonPath("m")), atM);
    EXPECT_FALSE(accessor->getFingerprint(CanonPath::root));
    EXPECT_EQ(accessor->getFingerprint(CanonPath("m/n/y")).value().second, std::string("U"));
}

namespace {

/* The hidden descendants deliberately remain observable to direct source
   calls, as they can beneath a physical symlink. A graft must decide whether
   its ancestor survives before consulting those descendants. */
struct GraftFallback : MemorySourceAccessor
{
    CanonPath blocker = CanonPath("unused");
    std::optional<Type> kind;

    void anchor() override {}

    void checkBelow(const CanonPath & path)
    {
        if (kind != tDirectory && path != blocker && path.isWithin(blocker))
            throw Error("consulted displaced fallback at '%s'", path.abs());
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        checkBelow(path);
        if (path == blocker)
            return kind ? std::optional{Stat{.type = *kind}} : std::nullopt;
        return MemorySourceAccessor::maybeLstat(path);
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        checkBelow(path);
        MemorySourceAccessor::readFile(path, sink, sizeCallback);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        checkBelow(path);
        return MemorySourceAccessor::readDirectory(path);
    }

    std::string readLink(const CanonPath & path) override
    {
        checkBelow(path);
        return MemorySourceAccessor::readLink(path);
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        checkBelow(path);
        return std::filesystem::path("/physical") / path.rel();
    }

    std::optional<merkle::TreeEntry> getKnownEntry(const CanonPath & path) override
    {
        checkBelow(path);
        return referenceObjectEntry(*this, path);
    }

    std::optional<std::pair<CanonPath, std::string>> getFingerprint(const CanonPath & path) override
    {
        checkBelow(path);
        return SourceAccessor::getFingerprint(path);
    }
};

struct GraftObservationError : Error
{
    using Error::Error;
};

struct GraftDemandProbe : MemorySourceAccessor
{
    enum class Observation { Missing, Regular, Directory, Error };
    CanonPath observedAt = CanonPath::root;
    Observation observation = Observation::Missing;
    unsigned calls = 0;

    void anchor() override {}

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        ++calls;
        if (path != observedAt)
            return MemorySourceAccessor::maybeLstat(path);
        switch (observation) {
        case Observation::Missing:
            return std::nullopt;
        case Observation::Regular:
            return Stat{.type = tRegular};
        case Observation::Directory:
            return Stat{.type = tDirectory};
        case Observation::Error:
            throw GraftObservationError("fallback observation is unavailable");
        }
        unreachable();
    }
};

/* All SourceAccessor node kinds, plus absence. Regular executable mode does
   not change the directory/non-directory partition (the evaluator fixture
   independently includes both regular modes on a real filesystem). */
const std::vector<std::optional<SourceAccessor::Type>> graftAncestorKinds{
    std::nullopt,
    SourceAccessor::tRegular,
    SourceAccessor::tSymlink,
    SourceAccessor::tDirectory,
    SourceAccessor::tChar,
    SourceAccessor::tBlock,
    SourceAccessor::tSocket,
    SourceAccessor::tFifo,
    SourceAccessor::tUnknown,
};

} // namespace

TEST(MountedSourceAccessor, totalGraftPromotesEveryAncestorKind)
{
    for (auto kind : graftAncestorKinds)
        for (bool rootBlocker : {false, true})
            for (bool rootMount : {false, true}) {
                SCOPED_TRACE(
                    ::testing::Message() << "kind=" << (kind ? int(*kind) : -1) << " rootBlocker=" << rootBlocker
                                         << " rootMount=" << rootMount);
                auto fallback = make_ref<GraftFallback>();
                fallback->root = memoryTree({})->root;
                fallback->kind = kind;
                fallback->blocker = rootBlocker ? CanonPath::root : CanonPath("ancestor");
                fallback->fingerprint =
                    "fallback-" + std::to_string(kind ? int(*kind) : -1) + (rootBlocker ? "-root" : "-child");
                fallback->addFile(fallback->blocker / "old", "old");
                fallback->addFile(fallback->blocker / "branch" / "hidden", "hidden");
                if (!rootBlocker)
                    fallback->addFile(CanonPath("outside"), "outside");
                auto mountPoint = rootMount ? CanonPath::root : CanonPath("selected");
                auto ancestor = mountPoint / fallback->blocker;
                auto fetched = memoryTree({{"leaf", "mounted"}});
                fetched->fingerprint = "fetched";
                std::map<CanonPath, ref<SourceAccessor>> mounts{{CanonPath::root, makeEmptySourceAccessor()}};
                mounts.insert_or_assign(mountPoint, fallback);
                mounts.emplace(ancestor / "branch" / "mounted", fetched);
                auto accessor = makeMountedSourceAccessor(std::move(mounts));

                /* Independent value oracle: retain fallback children exactly
                   when their ancestor is a directory, then graft the input. */
                auto expected = memoryTree({});
                if (!rootBlocker)
                    expected->addFile(CanonPath("outside"), "outside");
                if (kind == SourceAccessor::tDirectory) {
                    expected->addFile(fallback->blocker / "old", "old");
                    expected->addFile(fallback->blocker / "branch" / "hidden", "hidden");
                }
                expected->addFile(fallback->blocker / "branch" / "mounted" / "leaf", "mounted");
                EXPECT_EQ(narOf(*accessor, mountPoint), narOf(*expected));
                for (auto path : {ancestor, ancestor / "branch"}) {
                    EXPECT_EQ(accessor->lstat(path).type, SourceAccessor::tDirectory);
                    EXPECT_EQ(accessor->maybeLstat(path)->type, SourceAccessor::tDirectory);
                    EXPECT_THROW(accessor->readFile(path), NotARegularFile);
                    EXPECT_THROW(accessor->readLink(path), NotASymlink);
                    EXPECT_FALSE(accessor->getKnownEntry(path));
                    EXPECT_FALSE(accessor->getPhysicalPath(path));
                    auto name = accessor->getFingerprint(path);
                    ASSERT_TRUE(name);
                    EXPECT_NE(name->second.find("graft-v2"), std::string::npos);
                }
                auto old = ancestor / "old";
                if (kind == SourceAccessor::tDirectory) {
                    EXPECT_EQ(accessor->readFile(old), "old");
                    EXPECT_TRUE(accessor->getKnownEntry(old));
                    EXPECT_TRUE(accessor->getPhysicalPath(old));
                    EXPECT_TRUE(accessor->getFingerprint(old));
                } else {
                    EXPECT_FALSE(accessor->maybeLstat(old));
                    EXPECT_THROW(accessor->lstat(old), FileNotFound);
                    EXPECT_THROW(accessor->readFile(old), FileNotFound);
                    EXPECT_THROW(accessor->readDirectory(old), FileNotFound);
                    EXPECT_THROW(accessor->readLink(old), FileNotFound);
                    EXPECT_FALSE(accessor->getKnownEntry(old));
                    EXPECT_FALSE(accessor->getPhysicalPath(old));
                    EXPECT_FALSE(accessor->getFingerprint(old));
                }
                auto leaf = ancestor / "branch" / "mounted" / "leaf";
                EXPECT_EQ(accessor->readFile(leaf), "mounted");
                EXPECT_EQ(accessor->getFingerprint(leaf), std::pair(CanonPath("leaf"), std::string("fetched")));
            }
}

TEST(MountedSourceAccessor, ancestorShapeDoesNotForceFallbackContents)
{
    using Observation = GraftDemandProbe::Observation;
    for (unsigned method = 0; method < 8; ++method)
        for (bool root : {false, true})
            for (auto observation :
                 {Observation::Missing, Observation::Regular, Observation::Directory, Observation::Error}) {
                SCOPED_TRACE(
                    ::testing::Message() << "method=" << method << " root=" << root
                                         << " observation=" << int(observation));
                auto fallback = make_ref<GraftDemandProbe>();
                fallback->root = memoryTree({})->root;
                fallback->observedAt = root ? CanonPath::root : CanonPath("ancestor");
                fallback->observation = observation;
                fallback->fingerprint = "fallback-" + std::to_string(int(observation)) + (root ? "-root" : "-child");
                fallback->addFile(fallback->observedAt / "old", "old");
                auto mounted = memoryTree({{"leaf", "new"}});
                mounted->fingerprint = "mounted";
                auto accessor = makeMountedSourceAccessor(
                    {{CanonPath::root, fallback}, {fallback->observedAt / "mounted", mounted}});
                auto project = [&] {
                    const auto & path = fallback->observedAt;
                    switch (method) {
                    case 0:
                        EXPECT_THROW(accessor->readFile(path), NotARegularFile);
                        break;
                    case 1:
                        EXPECT_EQ(accessor->maybeLstat(path)->type, SourceAccessor::tDirectory);
                        break;
                    case 2:
                        EXPECT_EQ(accessor->lstat(path).type, SourceAccessor::tDirectory);
                        break;
                    case 3: {
                        SourceAccessor::DirEntries expected{{"mounted", SourceAccessor::tDirectory}};
                        if (observation == Observation::Directory)
                            expected.emplace("old", SourceAccessor::tRegular);
                        EXPECT_EQ(accessor->readDirectory(path), expected);
                        break;
                    }
                    case 4:
                        EXPECT_THROW(accessor->readLink(path), NotASymlink);
                        break;
                    case 5:
                        EXPECT_FALSE(accessor->getKnownEntry(path));
                        break;
                    case 6:
                        EXPECT_TRUE(accessor->getFingerprint(path));
                        break;
                    case 7:
                        EXPECT_FALSE(accessor->getPhysicalPath(path));
                        break;
                    }
                };
                bool needsContents = method == 3 || method == 6;
                if (needsContents && observation == Observation::Error)
                    EXPECT_THROW(project(), GraftObservationError);
                else
                    EXPECT_NO_THROW(project());
                EXPECT_EQ(fallback->calls, needsContents ? (root ? 1u : 2u) : 0u);
            }
}

TEST(MountedSourceAccessor, deeperBindingsDetermineListedChildShape)
{
    using Observation = GraftDemandProbe::Observation;
    for (auto observation : {Observation::Missing, Observation::Regular, Observation::Directory, Observation::Error})
        for (bool nested : {false, true}) {
            SCOPED_TRACE(::testing::Message() << "observation=" << int(observation) << " nested=" << nested);
            auto mounted = make_ref<GraftDemandProbe>();
            mounted->root = memoryTree({})->root;
            mounted->observation = observation;
            auto accessor = makeMountedSourceAccessor(
                {{CanonPath::root, memoryTree({{"m/stale", "stale"}})}, {CanonPath("m"), mounted}});
            if (nested)
                accessor->mount(CanonPath("m/child"), memoryTree({{"leaf", "new"}}));
            auto list = [&] {
                SourceAccessor::DirEntries expected;
                if (nested || observation == Observation::Directory)
                    expected.emplace("m", SourceAccessor::tDirectory);
                else if (observation == Observation::Regular)
                    expected.emplace("m", SourceAccessor::tRegular);
                EXPECT_EQ(accessor->readDirectory(CanonPath::root), expected);
            };
            if (!nested && observation == Observation::Error)
                EXPECT_THROW(list(), GraftObservationError);
            else
                EXPECT_NO_THROW(list());
            EXPECT_EQ(mounted->calls, nested ? 0u : 1u);
        }
}

TEST(MountedSourceAccessor, nestedMountsPromoteEveryMountedRootKind)
{
    for (auto kind : graftAncestorKinds)
        for (bool nested : {false, true})
            for (bool childFirst : {false, true}) {
                SCOPED_TRACE(
                    ::testing::Message() << "kind=" << (kind ? int(*kind) : -1) << " nested=" << nested
                                         << " childFirst=" << childFirst);
                auto mounted = make_ref<GraftFallback>();
                mounted->root = memoryTree({{"old", "old"}})->root;
                mounted->blocker = CanonPath::root;
                mounted->kind = kind;
                auto accessor = makeMountedSourceAccessor({{CanonPath::root, memoryTree({{"m/stale", "stale"}})}});
                if (nested && childFirst)
                    accessor->mount(CanonPath("m/child"), memoryTree({{"leaf", "new"}}));
                accessor->mount(CanonPath("m"), mounted);
                if (nested && !childFirst)
                    accessor->mount(CanonPath("m/child"), memoryTree({{"leaf", "new"}}));
                auto entries = accessor->readDirectory(CanonPath::root);
                auto st = accessor->maybeLstat(CanonPath("m"));
                EXPECT_EQ(entries.contains("m"), nested || kind.has_value());
                EXPECT_EQ(st.has_value(), nested || kind.has_value());
                if (nested || kind) {
                    auto expected = nested ? SourceAccessor::tDirectory : *kind;
                    EXPECT_EQ(entries.at("m"), expected);
                    EXPECT_EQ(st->type, expected);
                }
                if (nested) {
                    auto expected = kind == SourceAccessor::tDirectory
                                        ? memoryTree({{"old", "old"}, {"child/leaf", "new"}})
                                        : memoryTree({{"child/leaf", "new"}});
                    EXPECT_EQ(narOf(*accessor, CanonPath("m")), narOf(*expected));
                }
            }
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
                    if (throwing && !descendant)
                        EXPECT_THROW(run(), MountedCallbackException);
                    else
                        EXPECT_NO_THROW(run());
                    EXPECT_EQ(calls, descendant ? 0u : 1u);
                    EXPECT_EQ(weakOwner.expired(), release && !descendant);
                    EXPECT_EQ(weakChild.expired(), release && !descendant);
                    owner.reset();
                    EXPECT_TRUE(weakOwner.expired());
                    EXPECT_TRUE(weakChild.expired());
                }
}

TEST(MountedSourceAccessor, ancestorCallbacksRetainOwnersForEveryProjection)
{
    /* Every projection newly observes the required root before deciding
       that an off-branch descendant is absent. The child copies its callback
       before invoking it, so a missing guard has a defined negative control. */
    for (unsigned method = 0; method < 8; ++method)
        for (bool release : {false, true})
            for (bool throwing : {false, true}) {
                SCOPED_TRACE(
                    ::testing::Message() << "method=" << method << " release=" << release << " throwing=" << throwing);
                auto child = std::make_shared<MountedOwnershipProbe>();
                std::weak_ptr<SourceAccessor> weakChild = child;
                std::shared_ptr<MountedSourceAccessor> owner =
                    makeMountedSourceAccessor(
                        {{CanonPath::root, ref<SourceAccessor>(child)}, {CanonPath("m/n"), memoryTree({})}})
                        .get_ptr();
                std::weak_ptr<SourceAccessor> weakOwner = owner;
                unsigned calls = 0;
                child->callback = [&] {
                    ++calls;
                    if (release)
                        owner.reset();
                    auto alive = !weakOwner.expired() && !weakChild.expired();
                    EXPECT_TRUE(alive);
                    if (!alive)
                        throw MountedOwnershipLost{};
                    if (throwing)
                        throw MountedCallbackException{};
                };
                child.reset();
                auto * accessor = owner.get();
                auto project = [&] {
                    CanonPath path("off-branch");
                    switch (method) {
                    case 0:
                        accessor->readFile(path);
                        break;
                    case 1:
                        EXPECT_FALSE(accessor->maybeLstat(path));
                        break;
                    case 2:
                        accessor->lstat(path);
                        break;
                    case 3:
                        accessor->readDirectory(path);
                        break;
                    case 4:
                        accessor->readLink(path);
                        break;
                    case 5:
                        EXPECT_FALSE(accessor->getKnownEntry(path));
                        break;
                    case 6:
                        EXPECT_FALSE(accessor->getFingerprint(path));
                        break;
                    case 7:
                        EXPECT_FALSE(accessor->getPhysicalPath(path));
                        break;
                    }
                };
                if (throwing)
                    EXPECT_THROW(project(), MountedCallbackException);
                else if (method == 0 || method == 2 || method == 3 || method == 4)
                    EXPECT_THROW(project(), FileNotFound);
                else
                    EXPECT_NO_THROW(project());
                EXPECT_EQ(calls, 1u);
                EXPECT_EQ(weakOwner.expired(), release);
                EXPECT_EQ(weakChild.expired(), release);
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
