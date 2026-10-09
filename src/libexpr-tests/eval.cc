#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "nix/expr/eval.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/store/local-store.hh"
#include "nix/util/archive.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"
#include "nix/util/environment-variables.hh"
#include "nix/expr/value-to-json.hh"

namespace nix {

TEST(Exprs, pathSourcesLiveExactlyAsLongAsTheExpressions)
{
    std::array<std::weak_ptr<SourceAccessor>, 2> sources;
    {
        Exprs exprs;
        std::array<ExprPath *, 2> paths;
        for (size_t i = 0; i < sources.size(); ++i) {
            auto source = make_ref<MemorySourceAccessor>();
            source->addFile(CanonPath("/file"), fmt("source-%d", i));
            sources[i] = source.get_ptr();
            paths[i] = exprs.add<ExprPath>(exprs.alloc, source, "/file");
            exprs.add<ExprPath>(exprs.alloc, source, "/another");
        }
        for (size_t i = 0; i < sources.size(); ++i) {
            ASSERT_FALSE(sources[i].expired());
            EXPECT_EQ(paths[i]->v.path().readFile(), fmt("source-%d", i));
        }
    }
    for (auto & source : sources)
        EXPECT_TRUE(source.expired());
}

TEST(nix_isAllowedURI, http_example_com)
{
    Strings allowed;
    allowed.push_back("http://example.com");

    ASSERT_TRUE(isAllowedURI("http://example.com", allowed));
    ASSERT_TRUE(isAllowedURI("http://example.com/foo", allowed));
    ASSERT_TRUE(isAllowedURI("http://example.com/foo/", allowed));
    ASSERT_FALSE(isAllowedURI("/", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.co", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.como", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.org", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.org/foo", allowed));
}

TEST(nix_isAllowedURI, http_example_com_foo)
{
    Strings allowed;
    allowed.push_back("http://example.com/foo");

    ASSERT_TRUE(isAllowedURI("http://example.com/foo", allowed));
    ASSERT_TRUE(isAllowedURI("http://example.com/foo/", allowed));
    ASSERT_FALSE(isAllowedURI("/foo", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.com", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.como", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.org/foo", allowed));
    // Broken?
    // ASSERT_TRUE(isAllowedURI("http://example.com/foo?ok=1", allowed));
}

TEST(nix_isAllowedURI, http)
{
    Strings allowed;
    allowed.push_back("http://");

    ASSERT_TRUE(isAllowedURI("http://", allowed));
    ASSERT_TRUE(isAllowedURI("http://example.com", allowed));
    ASSERT_TRUE(isAllowedURI("http://example.com/foo", allowed));
    ASSERT_TRUE(isAllowedURI("http://example.com/foo/", allowed));
    ASSERT_TRUE(isAllowedURI("http://example.com", allowed));
    ASSERT_FALSE(isAllowedURI("/", allowed));
    ASSERT_FALSE(isAllowedURI("https://", allowed));
    ASSERT_FALSE(isAllowedURI("http:foo", allowed));
}

TEST(nix_isAllowedURI, https)
{
    Strings allowed;
    allowed.push_back("https://");

    ASSERT_TRUE(isAllowedURI("https://example.com", allowed));
    ASSERT_TRUE(isAllowedURI("https://example.com/foo", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.com", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.com/https:", allowed));
}

TEST(nix_isAllowedURI, absolute_path)
{
    Strings allowed;
    allowed.push_back("/var/evil"); // bad idea

    ASSERT_TRUE(isAllowedURI("/var/evil", allowed));
    ASSERT_TRUE(isAllowedURI("/var/evil/", allowed));
    ASSERT_TRUE(isAllowedURI("/var/evil/foo", allowed));
    ASSERT_TRUE(isAllowedURI("/var/evil/foo/", allowed));
    ASSERT_FALSE(isAllowedURI("/", allowed));
    ASSERT_FALSE(isAllowedURI("/var/evi", allowed));
    ASSERT_FALSE(isAllowedURI("/var/evilo", allowed));
    ASSERT_FALSE(isAllowedURI("/var/evilo/", allowed));
    ASSERT_FALSE(isAllowedURI("/var/evilo/foo", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.com/var/evil", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.com//var/evil", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.com//var/evil/foo", allowed));
}

TEST(nix_isAllowedURI, file_url)
{
    Strings allowed;
    allowed.push_back("file:///var/evil"); // bad idea

    ASSERT_TRUE(isAllowedURI("file:///var/evil", allowed));
    ASSERT_TRUE(isAllowedURI("file:///var/evil/", allowed));
    ASSERT_TRUE(isAllowedURI("file:///var/evil/foo", allowed));
    ASSERT_TRUE(isAllowedURI("file:///var/evil/foo/", allowed));
    ASSERT_FALSE(isAllowedURI("/", allowed));
    ASSERT_FALSE(isAllowedURI("/var/evi", allowed));
    ASSERT_FALSE(isAllowedURI("/var/evilo", allowed));
    ASSERT_FALSE(isAllowedURI("/var/evilo/", allowed));
    ASSERT_FALSE(isAllowedURI("/var/evilo/foo", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.com/var/evil", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.com//var/evil", allowed));
    ASSERT_FALSE(isAllowedURI("http://example.com//var/evil/foo", allowed));
    ASSERT_FALSE(isAllowedURI("http://var/evil", allowed));
    ASSERT_FALSE(isAllowedURI("http:///var/evil", allowed));
    ASSERT_FALSE(isAllowedURI("http://var/evil/", allowed));
    ASSERT_FALSE(isAllowedURI("file:///var/evi", allowed));
    ASSERT_FALSE(isAllowedURI("file:///var/evilo", allowed));
    ASSERT_FALSE(isAllowedURI("file:///var/evilo/", allowed));
    ASSERT_FALSE(isAllowedURI("file:///var/evilo/foo", allowed));
    ASSERT_FALSE(isAllowedURI("file:///", allowed));
    ASSERT_FALSE(isAllowedURI("file://", allowed));
}

TEST(nix_isAllowedURI, github_all)
{
    Strings allowed;
    allowed.push_back("github:");
    ASSERT_TRUE(isAllowedURI("github:", allowed));
    ASSERT_TRUE(isAllowedURI("github:foo/bar", allowed));
    ASSERT_TRUE(isAllowedURI("github:foo/bar/feat-multi-bar", allowed));
    ASSERT_TRUE(isAllowedURI("github:foo/bar?ref=refs/heads/feat-multi-bar", allowed));
    ASSERT_TRUE(isAllowedURI("github://foo/bar", allowed));
    ASSERT_FALSE(isAllowedURI("https://github:443/foo/bar/archive/master.tar.gz", allowed));
    ASSERT_FALSE(isAllowedURI("file://github:foo/bar/archive/master.tar.gz", allowed));
    ASSERT_FALSE(isAllowedURI("file:///github:foo/bar/archive/master.tar.gz", allowed));
    ASSERT_FALSE(isAllowedURI("github", allowed));
}

TEST(nix_isAllowedURI, github_org)
{
    Strings allowed;
    allowed.push_back("github:foo");
    ASSERT_FALSE(isAllowedURI("github:", allowed));
    ASSERT_TRUE(isAllowedURI("github:foo/bar", allowed));
    ASSERT_TRUE(isAllowedURI("github:foo/bar/feat-multi-bar", allowed));
    ASSERT_TRUE(isAllowedURI("github:foo/bar?ref=refs/heads/feat-multi-bar", allowed));
    ASSERT_FALSE(isAllowedURI("github://foo/bar", allowed));
    ASSERT_FALSE(isAllowedURI("https://github:443/foo/bar/archive/master.tar.gz", allowed));
    ASSERT_FALSE(isAllowedURI("file://github:foo/bar/archive/master.tar.gz", allowed));
    ASSERT_FALSE(isAllowedURI("file:///github:foo/bar/archive/master.tar.gz", allowed));
}

TEST(nix_isAllowedURI, non_scheme_colon)
{
    Strings allowed;
    allowed.push_back("https://foo/bar:");
    ASSERT_TRUE(isAllowedURI("https://foo/bar:", allowed));
    ASSERT_TRUE(isAllowedURI("https://foo/bar:/baz", allowed));
    ASSERT_FALSE(isAllowedURI("https://foo/bar:baz", allowed));
}

class EvalStateTest : public LibExprTest
{};

TEST_F(EvalStateTest, parserContainersReleaseAllStorageWithTheEvaluator)
{
    struct TrackingResource : std::pmr::memory_resource
    {
        std::atomic<size_t> outstanding{0}, allocations{0};

        void * do_allocate(size_t bytes, size_t alignment) override
        {
            auto p = std::pmr::new_delete_resource()->allocate(bytes, alignment);
            ++outstanding;
            ++allocations;
            return p;
        }

        void do_deallocate(void * p, size_t bytes, size_t alignment) override
        {
            --outstanding;
            std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
        }

        bool do_is_equal(const std::pmr::memory_resource & other) const noexcept override
        {
            return this == &other;
        }
    };

    enum class Failure { None, Parse, Bind };

    struct Case
    {
        const char * name;
        const char * expression;
        Failure failure;
    };

    /* These are the two AST node types with growing parser containers.
       Attrsets also have a merge path that discards the source node. */
    const Case cases[] = {
        {"attrs-success",
         R"({ static = 1; ${"dynamic"} = 2; inherit ({ inherited = 3; }) inherited; })",
         Failure::None},
        {"attrs-parse-failure",
         R"({ static = 1; ${"dynamic"} = 2; inherit ({ inherited = 3; }) inherited;)",
         Failure::Parse},
        {"attrs-bind-failure",
         R"(missing { static = 1; ${"dynamic"} = 2; inherit ({ inherited = 3; }) inherited; })",
         Failure::Bind},
        {"attrs-bind-failure-after-migration",
         R"({ static = 1; ${"dynamic"} = 2; inherit (missing) inherited; })",
         Failure::Bind},
        {"attrs-merged-discard",
         R"({ a = {}; a = { static = 1; ${"dynamic"} = 2; inherit ({ inherited = 3; }) inherited; }; })",
         Failure::None},
        {"attrs-merge-conflict",
         R"({ a = { inherit ({ same = 1; }) same; }; a = { inherit ({ same = 2; }) same; }; })",
         Failure::Parse},
        {"call-success", "(x: y: x) 1 2", Failure::None},
        {"call-parse-failure", "{ a = (x: x) 1;", Failure::Parse},
        {"call-bind-failure", "missing ((x: x) 1)", Failure::Bind},
    };
    for (const auto & test : cases) {
        SCOPED_TRACE(test.name);
        TrackingResource resource;
        auto previous = std::pmr::set_default_resource(&resource);
        Finally restore([&] { std::pmr::set_default_resource(previous); });
        {
            EvalState nested({}, state.store, fetchSettings, evalSettings);
            auto parse = [&] { return nested.parseExprFromString(test.expression, nested.rootPath(CanonPath::root)); };
            switch (test.failure) {
            case Failure::None:
                EXPECT_NO_THROW(parse());
                break;
            case Failure::Parse:
                EXPECT_THROW(parse(), ParseError);
                break;
            case Failure::Bind:
                EXPECT_THROW(parse(), UndefinedVarError);
                break;
            }
        }
        EXPECT_GT(resource.allocations.load(), 0u);
        EXPECT_EQ(resource.outstanding.load(), 0u);
    }
}

TEST_F(EvalStateTest, mergingAttributeSetsPreservesEveryInheritedSourceCombination)
{
    for (bool leftInherited : {false, true})
        for (bool rightInherited : {false, true}) {
            SCOPED_TRACE(::testing::Message() << leftInherited << " " << rightInherited);
            auto left = leftInherited ? "inherit ({ one = 1; }) one;" : "one = 1;";
            auto right = rightInherited ? "inherit ({ two = 2; }) two;" : "two = 2;";
            EXPECT_THAT(
                eval(fmt("let merged = { a = { %s }; a = { %s }; }; in merged.a.one * 10 + merged.a.two", left, right)),
                IsIntEq(12));
        }
}

/* A mount replaces the selected store or ambient host node; it never unions
   their private children into the fetched value. The ambient store case
   intentionally has the same logical and physical directory. */
TEST_F(EvalStateTest, mountInputInvalidatesPreviouslyObservedStoreNodes)
{
    using File = MemorySourceAccessor::File;
    enum class Occupant { SelectedStore, AmbientStore, RelocatedHost };
    enum class Mode { Impure, Restricted, Pure };
    auto nodes = [](std::string contents, std::string privateName) {
        return std::vector<File>{
            File{File::Regular{.executable = false, .contents = contents}},
            File{File::Regular{.executable = true, .contents = contents}},
            File{File::Symlink{.target = contents}},
            File{File::Directory{
                .entries =
                    {
                        {"child", File{File::Regular{.contents = contents}}},
                        {privateName, File{File::Regular{.contents = contents}}},
                    }}},
        };
    };
    auto before = nodes("before", "old-only");
    auto after = nodes("after-longer", "mounted-only");
    size_t serial = 0;
    /* 3 locations × 3 access modes × 5 old kinds × 4 fetched kinds. */
    for (auto occupant : {Occupant::SelectedStore, Occupant::AmbientStore, Occupant::RelocatedHost}) {
        AutoDelete root{canonPath(createTempDir(), true)};
        auto physicalRoot = root.path() / "physical-root";
        auto logicalDir =
            occupant == Occupant::AmbientStore ? physicalRoot / "nix/store" : root.path() / "logical-store";
        auto local = make_ref<LocalStore>(
            make_ref<LocalStoreConfig>(physicalRoot, StoreConfig::Params{{"store", logicalDir.string()}}));
        createDirs(logicalDir);
        for (auto mode : {Mode::Impure, Mode::Restricted, Mode::Pure})
            for (size_t oldNode = 0; oldNode <= before.size(); ++oldNode)
                for (size_t newNode = 0; newNode < after.size(); ++newNode) {
                    SCOPED_TRACE(fmt("location=%d mode=%d old=%d new=%d", int(occupant), int(mode), oldNode, newNode));
                    EvalSettings nestedSettings{readOnlyMode};
                    nestedSettings.nixPath = {};
                    nestedSettings.pureEval = mode == Mode::Pure;
                    nestedSettings.restrictEval = mode == Mode::Restricted;
                    EvalState nested({}, local, fetchSettings, nestedSettings);
                    auto mounted = make_ref<MemorySourceAccessor>();
                    mounted->root = after[newNode];
                    fetchers::Input input;
                    input.attrs.insert_or_assign("name", fmt("mount-cache-%d", serial++));
                    auto [storePath, hash] =
                        fetchToStore2(fetchSettings, *local, SourcePath{mounted}, FetchMode::DryRun, input.getName());
                    mounted->fingerprint = "mount-cache:" + hash.to_string(HashFormat::SRI, true);
                    auto logical = CanonPath(local->printStorePath(storePath));
                    ASSERT_FALSE(local->isValidPath(storePath));
                    if (oldNode < before.size()) {
                        auto old = make_ref<MemorySourceAccessor>();
                        old->root = before[oldNode];
                        StringSink dump;
                        old->dumpPath(CanonPath::root, dump);
                        StringSource source{dump.s};
                        restorePath(
                            occupant == Occupant::SelectedStore ? local->toRealPath(storePath)
                                                                : std::filesystem::path(logical.abs()),
                            source);
                    }
                    nested.allowPath(storePath);
                    if (mode == Mode::Pure)
                        EXPECT_THROW(nested.rootFS->maybeLstat(logical), InvalidPath);
                    else {
                        auto observed = nested.rootFS->maybeLstat(logical);
                        EXPECT_EQ(bool(observed), oldNode < before.size());
                        if (observed && observed->type == SourceAccessor::tSymlink) {
                            EXPECT_EQ(nested.rootFS->readLink(logical), "before");
                        }
                        /* A declined name is cached too, and must be discarded. */
                        EXPECT_FALSE(nested.rootFS->getFingerprint(logical));
                        EXPECT_FALSE(nested.rootFS->getFingerprint(logical));
                    }
                    EXPECT_EQ(nested.mountInput(input, mounted), storePath);
                    EXPECT_EQ(nested.rootFS->getFingerprint(logical), mounted->getFingerprint(CanonPath::root));
                    auto expectedStat = mounted->lstat(CanonPath::root);
                    auto actualStat = nested.rootFS->lstat(logical);
                    EXPECT_EQ(actualStat.type, expectedStat.type);
                    EXPECT_EQ(actualStat.isExecutable, expectedStat.isExecutable);
                    StringSink actual, expected;
                    nested.rootFS->dumpPath(logical, actual);
                    mounted->dumpPath(CanonPath::root, expected);
                    EXPECT_EQ(actual.s, expected.s);
                    if (newNode == 3) {
                        EXPECT_FALSE(nested.rootFS->pathExists(logical / "old-only"));
                        EXPECT_EQ(nested.rootFS->readDirectory(logical), mounted->readDirectory(CanonPath::root));
                    }
                    /* The public fetch route also preserves the first mount. */
                    auto repeated = make_ref<MemorySourceAccessor>();
                    repeated->root = mounted->root;
                    EXPECT_EQ(nested.mountInput(input, repeated), storePath);
                    EXPECT_EQ(nested.storeFS->getMount(logical).get(), mounted.get_ptr().get());
                }
    }
}

/* Registration and physical presence are separate from input selection.
   A valid row does not imply that its physical bytes still exist or agree. */
TEST_F(EvalStateTest, mountedReadsAndCopiesUseTheSameInputDespitePhysicalOccupants)
{
    enum class Physical { Missing, Same, Different };
    size_t serial = 0;
    for (bool relocated : {false, true}) {
        AutoDelete root{canonPath(createTempDir(), true)};
        auto physicalRoot = root.path() / "physical-root";
        auto logicalDir = relocated ? root.path() / "logical-store" : physicalRoot / "nix/store";
        auto local = make_ref<LocalStore>(
            make_ref<LocalStoreConfig>(physicalRoot, StoreConfig::Params{{"store", logicalDir.string()}}));
        createDirs(logicalDir);
        auto writeTree = [](ref<MemorySourceAccessor> tree, const std::filesystem::path & path) {
            StringSink dump;
            tree->dumpPath(CanonPath::root, dump);
            StringSource source{dump.s};
            restorePath(path, source);
        };
        for (bool registered : {false, true})
            for (auto physical : {Physical::Missing, Physical::Same, Physical::Different}) {
                SCOPED_TRACE(fmt("relocated=%d registered=%d physical=%d", relocated, registered, int(physical)));
                bool readOnly = false;
                EvalSettings nestedSettings{readOnly};
                nestedSettings.nixPath = {};
                nestedSettings.pureEval = false;
                EvalState nested({}, local, fetchSettings, nestedSettings);
                auto tree = make_ref<MemorySourceAccessor>();
                tree->addFile(CanonPath("default.nix"), "42");
                auto other = make_ref<MemorySourceAccessor>();
                other->addFile(CanonPath("default.nix"), "7");
                other->addFile(CanonPath("host-only"), "not part of the input");
                fetchers::Input input;
                input.attrs.insert_or_assign("name", fmt("mount-physical-%d", serial++));
                auto path =
                    fetchToStore2(fetchSettings, *local, SourcePath{tree}, FetchMode::DryRun, input.getName()).first;
                auto real = local->toRealPath(path);
                auto logical = CanonPath(local->printStorePath(path));
                if (registered) {
                    EXPECT_EQ(
                        nested.store->addToStore(input.getName(), SourcePath{tree}, ContentAddressMethod::Raw::Git),
                        path);
                    if (physical != Physical::Same)
                        deletePath(real);
                }
                if (physical == Physical::Different)
                    writeTree(other, real);
                else if (!registered && physical == Physical::Same)
                    writeTree(tree, real);
                if (relocated)
                    writeTree(other, std::filesystem::path(logical.abs()));

                EXPECT_EQ(nested.mountInput(input, tree), path);
                Value value;
                nested.evalFile(nested.storePath(path) / "default.nix", value);
                nested.forceValue(value, noPos);
                EXPECT_THAT(value, IsIntEq(42));
                EXPECT_FALSE(nested.rootFS->pathExists(logical / "host-only"));
                NixStringContext context;
                auto copied = nested.copyPathToStore(context, nested.storePath(path) / "default.nix");
                EXPECT_EQ(readFile(local->toRealPath(copied)), "42");
                EXPECT_NO_THROW(nested.ensureLazyPathCopied(path));
                EXPECT_TRUE(local->isValidPath(path));
                /* Ordinary copy does not repair an already-valid corrupt or
                   missing destination. Selecting the input is no repair claim. */
                if (registered && physical == Physical::Missing)
                    EXPECT_FALSE(pathExists(real));
                else
                    EXPECT_EQ(
                        readFile(real / "default.nix"), registered && physical == Physical::Different ? "7" : "42");

                /* A valid destination needs no restore or free-space probe.
                   Keep that probe unavailable through the changed-source check. */
                auto previousFreeSpaceFile = getEnv("_NIX_TEST_FREE_SPACE_FILE");
                auto & gc = nix::settings.getLocalSettings().getGCSettings();
                auto previousInterval = gc.minFreeCheckInterval.get();
                Finally restoreCollectionProbe{[&] {
                    gc.minFreeCheckInterval = previousInterval;
                    if (previousFreeSpaceFile)
                        setEnv("_NIX_TEST_FREE_SPACE_FILE", previousFreeSpaceFile->c_str());
                    else
                        unsetenv("_NIX_TEST_FREE_SPACE_FILE");
                }};
                gc.minFreeCheckInterval = 0;
                setEnv("_NIX_TEST_FREE_SPACE_FILE", (root.path() / "missing-free-space").string().c_str());
                EXPECT_NO_THROW(nested.ensureLazyPathCopied(path));

                /* Mounting fixes an expected address, not mutable source bytes.
                   A valid old destination must not suppress the later check. */
                tree->addFile(CanonPath("default.nix"), "99");
                EXPECT_EQ(nested.rootFS->readFile(logical / "default.nix"), "99");
                try {
                    nested.ensureLazyPathCopied(path);
                    ADD_FAILURE() << "a mounted input changed without the address check failing";
                } catch (Error & error) {
                    EXPECT_EQ(error.info().status, 102u);
                    EXPECT_NE(error.message().find("contents have changed"), std::string::npos);
                }
            }
    }
}

/* Away from grafts and their constructed ancestors, the previous host/store
   union still applies. A fetched file without a deeper explicit graft is
   a blocker, not an invitation to expose the fallback's descendants. */
TEST_F(EvalStateTest, mountedInputsPreserveFallbackBoundariesAndSymlinkEscapes)
{
    for (bool restricted : {false, true}) {
        SCOPED_TRACE(restricted);
        AutoDelete root{canonPath(createTempDir(), true)};
        auto logicalDir = root.path() / "logical-store";
        auto local = make_ref<LocalStore>(make_ref<LocalStoreConfig>(
            root.path() / "physical-root", StoreConfig::Params{{"store", logicalDir.string()}}));
        EvalSettings nestedSettings{readOnlyMode};
        nestedSettings.nixPath = {};
        nestedSettings.pureEval = false;
        nestedSettings.restrictEval = restricted;
        EvalState nested({}, local, fetchSettings, nestedSettings);
        StorePath ambient{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-ambient"};
        StorePath blocked{"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-blocked"};
        auto ambientPath = CanonPath(local->printStorePath(ambient));
        auto blockedPath = CanonPath(local->printStorePath(blocked));
        createDirs(std::filesystem::path(ambientPath.abs()));
        writeFile(std::filesystem::path(ambientPath.abs()) / "default.nix", "7");
        createDirs(local->toRealPath(ambient));
        writeFile(local->toRealPath(ambient) / "default.nix", "9");
        writeFile(std::filesystem::path(blockedPath.abs()), "host file blocks the lower directory");
        createDirs(local->toRealPath(blocked));
        writeFile(local->toRealPath(blocked) / "hidden", "lower-only");
        nested.allowPath(blocked);
        auto fallback = [&] {
            EXPECT_EQ(nested.rootFS->readFile(blockedPath), "host file blocks the lower directory");
            EXPECT_FALSE(nested.rootFS->pathExists(blockedPath / "hidden"));
        };
        fallback();

        auto tree = make_ref<MemorySourceAccessor>();
        tree->addFile(CanonPath("default.nix"), "42");
        tree->addFile(CanonPath("blocked"), "input file");
        tree->addFile(CanonPath("nested/old"), "original nested tree");
        MemorySink sink{*tree};
        sink.createSymlink(CanonPath("escape"), "../" + ambient.to_string() + "/default.nix");
        fetchers::Input input;
        input.attrs.insert_or_assign("name", "mount-boundaries");
        auto path = fetchToStore2(fetchSettings, *local, SourcePath{tree}, FetchMode::DryRun, input.getName()).first;
        auto logical = CanonPath(local->printStorePath(path));
        createDirs(std::filesystem::path(logical.abs()) / "blocked");
        writeFile(std::filesystem::path(logical.abs()) / "blocked/hidden", "host-only");
        EXPECT_EQ(nested.mountInput(input, tree), path);
        EXPECT_FALSE(nested.rootFS->pathExists(logical / CanonPath("blocked/hidden")));
        fallback();

        auto escape = nested.storePath(path) / "escape";
        if (restricted) {
            EXPECT_THROW(escape.resolveSymlinks().readFile(), RestrictedPathError);
        }
        nested.allowPath(ambient);
        EXPECT_EQ(escape.resolveSymlinks().readFile(), "7");
        EXPECT_EQ(nested.rootFS->readFile(ambientPath / "default.nix"), "7");

        /* This graft preserves its fetched directory ancestors. The file
           above has no deeper graft, so it must keep hiding host children. */
        auto replacement = make_ref<MemorySourceAccessor>();
        replacement->addFile(CanonPath("new"), "new nested tree");
        nested.storeFS->mount(logical / "nested", replacement);
        nested.rootFS->invalidateCache();
        EXPECT_FALSE(nested.rootFS->pathExists(logical / CanonPath("nested/old")));
        EXPECT_EQ(nested.rootFS->readFile(logical / CanonPath("nested/new")), "new nested tree");
        fallback();
    }
}

/* A relocated store's logical ancestors are independent of its real store
   directory. Explicit input descendants promote host blockers to directories;
   the replaced symlink's physical target must not leak sibling entries. */
TEST_F(EvalStateTest, mountedInputsPromoteEveryLogicalAncestorKind)
{
    using File = MemorySourceAccessor::File;
    enum class Mode { Impure, Restricted, Pure };
    size_t serial = 0;
    for (bool atStoreDir : {false, true})
        for (unsigned kind = 0; kind != 5; ++kind) {
            SCOPED_TRACE(fmt("storeDir=%d kind=%d", atStoreDir, kind));
            AutoDelete root{canonPath(createTempDir(), true)};
            auto logicalDir = root.path() / "logical-parent/store";
            auto blocker = atStoreDir ? logicalDir : logicalDir.parent_path();
            auto local = make_ref<LocalStore>(make_ref<LocalStoreConfig>(
                root.path() / "physical-root", StoreConfig::Params{{"store", logicalDir.string()}}));
            createDirs(blocker.parent_path());
            auto target = root.path() / "symlink-target";
            createDirs(target);
            writeFile(target / "outside", "must not leak through a promoted symlink");
            if (kind != 0) {
                auto host = make_ref<MemorySourceAccessor>();
                if (kind == 1 || kind == 2)
                    host->root = File{File::Regular{.executable = kind == 2, .contents = "old blocker"}};
                else if (kind == 3)
                    host->root = File{File::Symlink{.target = target.string()}};
                else
                    host->addFile(CanonPath("outside"), "surviving directory sibling");
                StringSink dump;
                host->dumpPath(CanonPath::root, dump);
                StringSource source{dump.s};
                restorePath(blocker, source);
            }
            for (auto mode : {Mode::Impure, Mode::Restricted, Mode::Pure}) {
                SCOPED_TRACE(int(mode));
                EvalSettings nestedSettings{readOnlyMode};
                nestedSettings.nixPath = {};
                nestedSettings.pureEval = mode == Mode::Pure;
                nestedSettings.restrictEval = mode == Mode::Restricted;
                EvalState nested({}, local, fetchSettings, nestedSettings);
                /* Admission is separate from whether the composed tree has
                   the node; permit siblings so absence cannot be a policy hit. */
                nested.allowPathLegacy(blocker.string());
                auto tree = make_ref<MemorySourceAccessor>();
                tree->addFile(CanonPath("default.nix"), "42");
                fetchers::Input input;
                input.attrs.insert_or_assign("name", fmt("mount-ancestor-%d", serial++));
                auto path = nested.mountInput(input, tree);
                auto logical = CanonPath(local->printStorePath(path));
                auto ancestor = CanonPath(blocker.string());
                EXPECT_EQ(nested.rootFS->lstat(ancestor).type, SourceAccessor::tDirectory);
                ASSERT_TRUE(nested.rootFS->maybeLstat(ancestor));
                EXPECT_EQ(nested.rootFS->maybeLstat(ancestor)->type, SourceAccessor::tDirectory);
                auto entries = nested.rootFS->readDirectory(ancestor);
                EXPECT_TRUE(entries.contains(atStoreDir ? path.to_string() : "store"));
                bool keepsSibling = kind == 4 && mode != Mode::Pure;
                EXPECT_EQ(entries.contains("outside"), keepsSibling);
                EXPECT_EQ(nested.rootFS->pathExists(ancestor / "outside"), keepsSibling);
                if (keepsSibling) {
                    EXPECT_EQ(nested.rootFS->readFile(ancestor / "outside"), "surviving directory sibling");
                }
                EXPECT_THROW(nested.rootFS->readFile(ancestor), Error);
                EXPECT_THROW(nested.rootFS->readLink(ancestor), Error);
                EXPECT_FALSE(nested.rootFS->getPhysicalPath(ancestor));
                EXPECT_FALSE(nested.rootFS->getKnownEntry(ancestor));
                EXPECT_FALSE(nested.rootFS->getFingerprint(ancestor));
                auto expression = nested.storePath(path) / "default.nix";
                EXPECT_EQ(expression.resolveSymlinks().path, logical / "default.nix");
                EXPECT_EQ(expression.resolveSymlinks().readFile(), "42");
                Value value;
                nested.evalFile(expression, value);
                nested.forceValue(value, noPos);
                EXPECT_THAT(value, IsIntEq(42));
            }
        }
}

TEST_F(EvalStateTest, parsedFilesReleaseTheirSourceWithTheEvaluator)
{
    for (bool mustBeTrivial : {false, true}) {
        SCOPED_TRACE(mustBeTrivial);
        std::weak_ptr<SourceAccessor> source;
        {
            EvalState nested({}, state.store, fetchSettings, evalSettings);
            {
                auto accessor = make_ref<MemorySourceAccessor>();
                accessor->addFile(CanonPath("/file.nix"), mustBeTrivial ? "{ value = 42; }" : "42");
                source = accessor.get_ptr();
                Value first, cached;
                nested.evalFile(SourcePath{accessor, CanonPath("/file.nix")}, first, mustBeTrivial);
                nested.evalFile(SourcePath{accessor, CanonPath("/file.nix")}, cached, mustBeTrivial);
                if (mustBeTrivial) {
                    EXPECT_EQ(first.attrs(), cached.attrs());
                } else {
                    EXPECT_THAT(first, IsIntEq(42));
                    EXPECT_THAT(cached, IsIntEq(42));
                }
            }
            EXPECT_FALSE(source.expired());
        }
        EXPECT_TRUE(source.expired());
    }
}

TEST_F(EvalStateTest, cachedErrorsReleaseTheirSourceUnlessTheErrorEscapes)
{
    constexpr std::string_view contents = "assert false; 42";
    for (bool keepError : {false, true}) {
        SCOPED_TRACE(keepError);
        std::weak_ptr<SourceAccessor> source;
        std::exception_ptr escaped;
        {
            EvalState nested({}, state.store, fetchSettings, evalSettings);
            auto accessor = make_ref<MemorySourceAccessor>();
            accessor->addFile(CanonPath("/failure.nix"), std::string(contents));
            source = accessor.get_ptr();
            for (int attempt = 0; attempt < 2; ++attempt) {
                Value value;
                try {
                    nested.evalFile(SourcePath{accessor, CanonPath("/failure.nix")}, value);
                    FAIL() << "expected the assertion to fail";
                } catch (const AssertionError &) {
                    if (keepError)
                        escaped = std::current_exception();
                }
            }
        }
        if (keepError) {
            EXPECT_FALSE(source.expired());
            try {
                std::rethrow_exception(escaped);
            } catch (const AssertionError & error) {
                ASSERT_TRUE(error.info().pos);
                auto path = error.info().pos->getSourcePath();
                ASSERT_TRUE(path);
                EXPECT_EQ(path->readFile(), contents);
            }
            escaped = nullptr;
        }
        EXPECT_TRUE(source.expired());
    }
}

TEST_F(EvalStateTest, getBuiltins_ok)
{
    auto evaled = maybeThunk("builtins");
    auto & builtins = state.getBuiltins();
    ASSERT_TRUE(builtins.type() == nAttrs);
    ASSERT_EQ(evaled, &builtins);
}

TEST_F(EvalStateTest, getBuiltin_ok)
{
    auto & builtin = state.getBuiltin("toString");
    ASSERT_TRUE(builtin.type() == nFunction);
    // FIXME
    // auto evaled = maybeThunk("builtins.toString");
    // ASSERT_EQ(evaled, &builtin);
    auto & builtin2 = state.getBuiltin("true");
    ASSERT_EQ(state.forceBool(builtin2, noPos, "in unit test"), true);
}

TEST_F(EvalStateTest, getBuiltin_fail)
{
    ASSERT_THROW(state.getBuiltin("nonexistent"), EvalError);
}

class PureEvalTest : public LibExprTest
{
public:
    PureEvalTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}), [](bool & readOnlyMode) {
            EvalSettings settings{readOnlyMode};
            settings.pureEval = true;
            settings.restrictEval = true;
            return settings;
        })
    {
    }
};

TEST_F(PureEvalTest, pathExists)
{
    ASSERT_THAT(eval("builtins.pathExists /."), IsFalse());
    ASSERT_THAT(eval("builtins.pathExists /nix"), IsFalse());
    ASSERT_THAT(eval("builtins.pathExists /nix/store"), IsFalse());

    {
        std::string contents = "Lorem ipsum";

        StringSource s{contents};
        auto path = state.store->addToStoreFromDump(
            s, "source", FileSerialisationMethod::Flat, ContentAddressMethod::Raw::Text, HashAlgorithm::SHA256);
        auto printed = store->printStorePath(path);

        ASSERT_THROW(eval(fmt("builtins.readFile %s", printed)), RestrictedPathError);
        ASSERT_THAT(eval(fmt("builtins.pathExists %s", printed)), IsFalse());

        ASSERT_THROW(eval("builtins.readDir /."), RestrictedPathError);
        state.allowPath(path); // FIXME: This shouldn't behave this way.
        /* Allowing a store path allows the directories leading to it, and the
           evaluator's root is a tree: the store directory's parents exist and
           list the way down to it. */
        ASSERT_THAT(eval("builtins.readDir /."), IsAttrsOfSize(1));
        ASSERT_THAT(eval("builtins.pathExists /nix"), IsTrue());
        ASSERT_THAT(eval("builtins.pathExists /nix/store"), IsTrue());
    }
}

/* Input selection changes the accessor view, not values and search results
   already memoized in this evaluation. Explicit reset starts a fresh epoch. */
TEST_F(EvalStateTest, mountingPreservesEvaluationMemoizationUntilExplicitReset)
{
    enum class Observation { FileImport, SymlinkImport, MissingLookupRoot, MissingLookupChild };
    for (auto observation :
         {Observation::FileImport,
          Observation::SymlinkImport,
          Observation::MissingLookupRoot,
          Observation::MissingLookupChild}) {
        SCOPED_TRACE(int(observation));
        AutoDelete root{canonPath(createTempDir(), true)};
        auto logicalDir = root.path() / "logical-store";
        auto local = make_ref<LocalStore>(make_ref<LocalStoreConfig>(
            root.path() / "physical-root", StoreConfig::Params{{"store", logicalDir.string()}}));
        createDirs(logicalDir);
        auto tree = make_ref<MemorySourceAccessor>();
        tree->addFile(CanonPath("default.nix"), "42");
        fetchers::Input input;
        input.attrs.insert_or_assign("name", "mount-evaluation-epoch");
        auto path = fetchToStore2(fetchSettings, *local, SourcePath{tree}, FetchMode::DryRun, input.getName()).first;
        auto logical = CanonPath(local->printStorePath(path));
        auto host = std::filesystem::path(logical.abs());
        const bool lookup =
            observation == Observation::MissingLookupRoot || observation == Observation::MissingLookupChild;
        if (observation != Observation::MissingLookupRoot)
            createDirs(host);
        if (observation == Observation::FileImport)
            writeFile(host / "default.nix", "7");
        else if (observation == Observation::SymlinkImport) {
            auto target = root.path() / "previous-target.nix";
            writeFile(target, "7");
            createSymlink(target, host / "default.nix");
        }

        EvalSettings nestedSettings{readOnlyMode};
        nestedSettings.nixPath = {};
        nestedSettings.pureEval = false;
        nestedSettings.restrictEval = true;
        auto search = lookup ? LookupPath::parse({"mounted=" + logical.abs()}) : LookupPath{};
        EvalState nested(search, local, fetchSettings, nestedSettings);
        nested.allowPathLegacy(root.path().string());
        auto imported = [&] {
            Value value;
            nested.evalFile(nested.storePath(path) / "default.nix", value);
            nested.forceValue(value, noPos);
            return value;
        };
        Value earlier;
        if (lookup)
            EXPECT_THROW(nested.findFile("mounted/default.nix"), ThrownError);
        else {
            earlier = imported();
            EXPECT_THAT(earlier, IsIntEq(7));
        }

        EXPECT_EQ(nested.mountInput(input, tree), path);
        EXPECT_EQ(nested.rootFS->readFile(logical / "default.nix"), "42");
        if (lookup)
            EXPECT_THROW(nested.findFile("mounted/default.nix"), ThrownError);
        else
            EXPECT_THAT(imported(), IsIntEq(7));

        nested.resetFileCache();
        if (lookup)
            EXPECT_EQ(nested.findFile("mounted/default.nix").readFile(), "42");
        else {
            EXPECT_THAT(imported(), IsIntEq(42));
            EXPECT_THAT(earlier, IsIntEq(7));
        }
    }
}

namespace {

struct CopyRequestSource : MemorySourceAccessor
{
    size_t reads = 0;
    std::function<void()> afterRead, afterReadLink, onInvalidate;

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        auto callback = afterRead;
        ++reads;
        MemorySourceAccessor::readFile(path, sink, sizeCallback);
        if (callback)
            callback();
    }

    void invalidateCache() override
    {
        auto callback = onInvalidate;
        if (callback)
            callback();
    }

    std::string readLink(const CanonPath & path) override
    {
        auto target = MemorySourceAccessor::readLink(path);
        auto callback = afterReadLink;
        if (callback)
            callback();
        return target;
    }
};

static StorePath evalCopiedPath(EvalState & state, std::string expression)
{
    Value value;
    state.eval(state.parseExprFromString(std::move(expression), state.rootPath(CanonPath::root)), value);
    NixStringContext context;
    return state.store->parseStorePath(state.forceString(value, context, noPos, "copy request test"));
}

} // namespace

TEST_F(EvalStateTest, sourceCopyRequestsRespectModesRepairAndDestinationPresence)
{
    AutoDelete root{createTempDir()};
    auto local = make_ref<LocalStore>(make_ref<LocalStoreConfig>(root.path(), StoreConfig::Params{}));
    for (bool executable : {false, true})
        for (auto firstMode : {FetchMode::DryRun, FetchMode::Copy})
            for (auto nextMode : {FetchMode::DryRun, FetchMode::Copy})
                for (bool present : {false, true})
                    for (bool repair : {false, true}) {
                        SCOPED_TRACE(
                            ::testing::Message() << executable << " " << int(firstMode) << " " << int(nextMode) << " "
                                                 << present << " " << repair);
                        EvalSettings settings{readOnlyMode};
                        settings.pureEval = false;
                        settings.nixPath = {};
                        EvalState nested({}, local, fetchSettings, settings);
                        auto source = make_ref<CopyRequestSource>();
                        source->root = MemorySourceAccessor::File::Regular{executable, "AAA"};
                        auto path = SourcePath{source};
                        auto name =
                            fmt("request-%d-%d-%d-%d-%d", executable, int(firstMode), int(nextMode), present, repair);
                        auto first = nested.copySourceToStore(path, firstMode, name, ContentAddressMethod::Raw::Git);
                        if (present && firstMode == FetchMode::DryRun)
                            EXPECT_EQ(fetchToStore(fetchSettings, *local, path, FetchMode::Copy, name), first);
                        if (!present && firstMode == FetchMode::Copy) {
                            local->invalidatePathChecked(first);
                            deletePath(local->toRealPath(first));
                        }
                        source->root = MemorySourceAccessor::File::Regular{executable, "BBB"};
                        auto oracle = make_ref<MemorySourceAccessor>();
                        oracle->root = source->root;
                        auto fresh = fetchToStore(fetchSettings, *local, SourcePath{oracle}, FetchMode::DryRun, name);
                        auto before = source->reads;
                        nested.repair = repair ? Repair : NoRepair;
                        auto next = nested.copySourceToStore(path, nextMode, name, ContentAddressMethod::Raw::Git);
                        bool reuse =
                            !repair && (nextMode == FetchMode::DryRun || (firstMode == FetchMode::Copy && present));
                        EXPECT_EQ(next, reuse ? first : fresh);
                        EXPECT_EQ(source->reads, before + (reuse ? 0 : 1));
                        if (nextMode == FetchMode::Copy)
                            EXPECT_EQ(
                                local->requireStoreObjectAccessor(next)->readFile(CanonPath::root),
                                reuse ? "AAA" : "BBB");
                    }
}

TEST_F(EvalStateTest, sourceCopyRequestKeysSeparateEveryArgument)
{
    for (auto firstMethod :
         {ContentAddressMethod::Raw::Flat,
          ContentAddressMethod::Raw::Text,
          ContentAddressMethod::Raw::NixArchive,
          ContentAddressMethod::Raw::Git})
        for (auto nextMethod :
             {ContentAddressMethod::Raw::Flat,
              ContentAddressMethod::Raw::Text,
              ContentAddressMethod::Raw::NixArchive,
              ContentAddressMethod::Raw::Git})
            for (unsigned changed = 0; changed < 8; ++changed) {
                SCOPED_TRACE(::testing::Message() << int(firstMethod) << " " << int(nextMethod) << " " << changed);
                auto source = make_ref<CopyRequestSource>();
                source->addFile(CanonPath("a"), "AAA");
                source->addFile(CanonPath("b"), "BBB");
                auto first = state.copySourceToStore(
                    SourcePath{source, CanonPath("a")}, FetchMode::DryRun, "request", firstMethod);
                source->addFile(CanonPath("a"), "BBB");
                auto other = make_ref<CopyRequestSource>();
                other->root = source->root;
                SourcePath nextPath{changed & 1 ? other : source, CanonPath(changed & 2 ? "b" : "a")};
                auto name = changed & 4 ? "another-request" : "request";
                auto next = state.copySourceToStore(nextPath, FetchMode::DryRun, name, nextMethod);
                auto oracle = make_ref<MemorySourceAccessor>();
                oracle->root = MemorySourceAccessor::File::Regular{false, "BBB"};
                auto fresh =
                    fetchToStore(fetchSettings, *store, SourcePath{oracle}, FetchMode::DryRun, name, nextMethod);
                bool reuse = !changed && firstMethod == nextMethod;
                EXPECT_EQ(next, reuse ? first : fresh);
                EXPECT_EQ(source->reads, 1u + (!reuse && !(changed & 1)));
                EXPECT_EQ(other->reads, bool(changed & 1));
            }
}

TEST_F(EvalStateTest, coercionJsonAndUnassertedPathsShareOnlyTheSameRequest)
{
    auto previousMode = std::exchange(nix::settings.readOnlyMode, true);
    Finally restoreMode{[&] { nix::settings.readOnlyMode = previousMode; }};
    for (int firstCall : {0, 1, 2})
        for (int nextCall : {0, 1, 2}) {
            SCOPED_TRACE(::testing::Message() << firstCall << " " << nextCall);
            EvalSettings settings{readOnlyMode};
            settings.pureEval = false;
            settings.nixPath = {};
            EvalState nested({}, store, fetchSettings, settings);
            auto source = make_ref<CopyRequestSource>();
            source->addFile(CanonPath("file"), "AAA");
            nested.storeFS->mount(CanonPath("/copy-request"), source);
            auto call = [&](int kind) {
                if (kind == 2)
                    return evalCopiedPath(nested, "builtins.path { path = /copy-request/file; name = \"file\"; }");
                Value value;
                value.mkPath(nested.rootPath(CanonPath("/copy-request/file")), nested.mem);
                NixStringContext context;
                auto result = kind == 0 ? nested.coerceToString(noPos, value, context, "copy request test").toOwned()
                                        : renderValueAsJSON(nested, true, value, noPos, context);
                if (kind == 1)
                    result = result.substr(1, result.size() - 2);
                auto path = store->parseStorePath(result);
                EXPECT_TRUE(context.contains(NixStringContextElem::Opaque{.path = path}));
                return path;
            };
            auto first = call(firstCall);
            source->addFile(CanonPath("file"), "BBB");
            EXPECT_EQ(call(nextCall), first);
            EXPECT_EQ(source->reads, 1u);
            EXPECT_EQ(nested.rootFS->readFile(CanonPath("/copy-request/file")), "BBB");
        }
}

TEST_F(EvalStateTest, filteredNamedAndAssertedCopiesObserveTheNewSource)
{
    /* addPath uses the process mode; keep it aligned with this read-only
       evaluator so asserted destinations remain absent in the dummy store. */
    auto previousMode = std::exchange(nix::settings.readOnlyMode, true);
    Finally restoreMode{[&] { nix::settings.readOnlyMode = previousMode; }};
    for (int request : {0, 1, 2, 3, 4, 5}) {
        SCOPED_TRACE(request);
        EvalSettings settings{readOnlyMode};
        settings.pureEval = false;
        settings.nixPath = {};
        EvalState nested({}, store, fetchSettings, settings);
        auto source = make_ref<CopyRequestSource>();
        source->addFile(CanonPath("file"), "AAA");
        nested.storeFS->mount(CanonPath("/copy-request"), source);
        NixStringContext context;
        auto first = nested.copyPathToStore(context, nested.rootPath(CanonPath("/copy-request/file")));
        source->addFile(CanonPath("file"), "BBB");
        auto oracle = make_ref<MemorySourceAccessor>();
        oracle->root = MemorySourceAccessor::File::Regular{false, "BBB"};
        auto tree = fetchToStore2(fetchSettings, *store, SourcePath{oracle}, FetchMode::DryRun, "file");
        auto nar = fetchToStore2(
            fetchSettings,
            *store,
            SourcePath{oracle},
            FetchMode::DryRun,
            "file",
            ContentAddressMethod::Raw::NixArchive);
        std::string attrs;
        if (request == 0)
            attrs = "name = \"renamed\";";
        if (request == 1 || request == 5)
            attrs += "filter = path: type: true;";
        if (request == 2 || request == 5)
            attrs += fmt("sha256 = \"%s\";", nar.second.to_string(HashFormat::SRI, true));
        if (request == 3)
            attrs = fmt("treeHash = \"%s\";", tree.second.to_string(HashFormat::SRI, true));
        if (request == 4)
            attrs = "recursive = false;";
        auto result = evalCopiedPath(nested, fmt("builtins.path { path = /copy-request/file; %s }", attrs));
        auto expected = fetchToStore2(
            fetchSettings,
            *store,
            SourcePath{oracle},
            FetchMode::DryRun,
            request == 0 ? "renamed" : "file",
            request == 4 ? ContentAddressMethod::Raw::Flat : ContentAddressMethod::Raw::Git);
        EXPECT_EQ(result, expected.first);
        EXPECT_NE(result, first);
        EXPECT_GT(source->reads, 1u);
    }
}

TEST_F(EvalStateTest, sourceCopyRequestsRetireOnMountAndResetCompletion)
{
    for (bool mount : {false, true})
        for (bool nestedInvalidation : {false, true})
            for (bool fail : {false, true}) {
                SCOPED_TRACE(::testing::Message() << mount << " " << nestedInvalidation << " " << fail);
                EvalSettings settings{readOnlyMode};
                settings.pureEval = false;
                settings.nixPath = {};
                EvalState nested({}, store, fetchSettings, settings);
                auto source = make_ref<CopyRequestSource>();
                source->root = MemorySourceAccessor::File::Regular{false, "AAA"};
                nested.storeFS->mount(CanonPath("/invalidation-callback"), source);
                auto path = SourcePath{source};
                auto copy = [&] {
                    return nested.copySourceToStore(path, FetchMode::DryRun, "request", ContentAddressMethod::Raw::Git);
                };
                auto invalidate = [&] {
                    if (!mount)
                        return nested.resetFileCache();
                    auto tree = make_ref<MemorySourceAccessor>();
                    tree->addFile(CanonPath("value"), "input");
                    fetchers::Input input;
                    input.attrs.insert_or_assign("name", "copy-request-mount");
                    nested.mountInput(input, tree);
                };
                source->afterRead = [&] {
                    source->afterRead = {};
                    source->root = MemorySourceAccessor::File::Regular{false, "BBB"};
                    invalidate();
                };
                source->onInvalidate = [&] {
                    source->onInvalidate = {};
                    if (nestedInvalidation) {
                        nested.resetFileCache();
                        copy();
                    }
                    if (fail)
                        throw Error("copy request invalidation failed");
                };
                if (fail)
                    EXPECT_THROW(copy(), Error);
                else
                    copy();
                auto before = source->reads;
                auto fresh = copy();
                EXPECT_EQ(source->reads, before + 1);
                auto oracle = make_ref<MemorySourceAccessor>();
                oracle->root = source->root;
                EXPECT_EQ(fresh, fetchToStore(fetchSettings, *store, SourcePath{oracle}, FetchMode::DryRun, "request"));
                EXPECT_EQ(copy(), fresh);
                EXPECT_EQ(source->reads, before + 1);
            }
}

TEST_F(EvalStateTest, onlyCompletedSourceRequestsRetainTheirSources)
{
    for (int completion : {0, 1, 2}) {
        SCOPED_TRACE(completion);
        std::weak_ptr<SourceAccessor> retained;
        {
            auto source = make_ref<CopyRequestSource>();
            retained = source.get_ptr();
            source->root = MemorySourceAccessor::File::Regular{false, "AAA"};
            if (completion)
                source->afterRead = [] { throw Error("source request failed"); };
            auto copy = [&] {
                return state.copySourceToStore(
                    SourcePath{source}, FetchMode::DryRun, "request", ContentAddressMethod::Raw::Git);
            };
            if (completion) {
                EXPECT_THROW(copy(), Error);
                source->afterRead = {};
                EXPECT_EQ(source->reads, 1u);
                if (completion == 2) {
                    auto retried = copy();
                    EXPECT_EQ(copy(), retried);
                    EXPECT_EQ(source->reads, 2u);
                }
            } else {
                copy();
                copy();
                EXPECT_EQ(source->reads, 1u);
            }
        }
        EXPECT_EQ(retained.expired(), completion == 1);
        state.resetFileCache();
        EXPECT_TRUE(retained.expired());
    }
}

TEST_F(EvalStateTest, repairCopiesNeitherUseNorReplaceCompletedRequests)
{
    auto source = make_ref<CopyRequestSource>();
    source->root = MemorySourceAccessor::File::Regular{false, "AAA"};
    auto copy = [&] {
        return state.copySourceToStore(
            SourcePath{source}, FetchMode::DryRun, "request", ContentAddressMethod::Raw::Git);
    };
    auto first = copy();
    source->root = MemorySourceAccessor::File::Regular{false, "BBB"};
    state.repair = Repair;
    EXPECT_NE(copy(), first);
    state.repair = NoRepair;
    EXPECT_EQ(copy(), first);
    EXPECT_EQ(source->reads, 2u);
}

TEST_F(EvalStateTest, admissionGrowthRetainsARequestButDoesNotNameTheChangedTree)
{
    auto previousMode = std::exchange(nix::settings.readOnlyMode, true);
    Finally restoreMode{[&] { nix::settings.readOnlyMode = previousMode; }};
    EvalSettings settings{readOnlyMode};
    settings.pureEval = false;
    settings.restrictEval = true;
    settings.nixPath = {};
    EvalState nested({}, store, fetchSettings, settings);
    auto source = make_ref<CopyRequestSource>();
    source->addFile(CanonPath("a"), "a");
    source->addFile(CanonPath("b"), "b");
    nested.storeFS->mount(CanonPath("/copy-request"), source);
    nested.allowPathLegacy("/copy-request/a");
    NixStringContext context;
    auto path = nested.rootPath(CanonPath("/copy-request"));
    auto first = nested.copyPathToStore(context, path);
    auto before = source->reads;
    nested.allowPathLegacy("/copy-request/b");
    EXPECT_EQ(nested.copyPathToStore(context, path), first);
    EXPECT_EQ(source->reads, before);
    auto full = fetchToStore(fetchSettings, *store, SourcePath{source}, FetchMode::DryRun, "copy-request");
    EXPECT_NE(full, first);
    EXPECT_EQ(evalCopiedPath(nested, "builtins.path { path = /copy-request; filter = path: type: true; }"), full);
    auto renamed = evalCopiedPath(nested, "builtins.path { path = /copy-request; name = \"expanded\"; }");
    EXPECT_EQ(renamed, fetchToStore(fetchSettings, *store, SourcePath{source}, FetchMode::DryRun, "expanded"));
    nested.resetFileCache();
    EXPECT_EQ(nested.copyPathToStore(context, path), full);
}

TEST_F(EvalStateTest, resolutionCallbacksPrecedeTheResolvedCopyRequest)
{
    for (bool mount : {false, true}) {
        SCOPED_TRACE(mount);
        EvalSettings settings{readOnlyMode};
        settings.pureEval = false;
        settings.nixPath = {};
        EvalState nested({}, store, fetchSettings, settings);
        auto source = make_ref<CopyRequestSource>();
        source->addFile(CanonPath("old/file"), "AAA");
        source->addFile(CanonPath("new/file"), "BBB");
        MemorySink sink{*source};
        sink.createSymlink(CanonPath("link"), "old");
        nested.storeFS->mount(CanonPath("/copy-request"), source);
        source->afterReadLink = [&] {
            source->afterReadLink = {};
            source->open(CanonPath("link"), std::nullopt)->raw = MemorySourceAccessor::File::Symlink{"new"};
            if (!mount)
                nested.resetFileCache();
            else {
                auto inputTree = make_ref<MemorySourceAccessor>();
                inputTree->addFile(CanonPath("value"), "input");
                fetchers::Input input;
                input.attrs.insert_or_assign("name", "resolution-mount");
                nested.mountInput(input, inputTree);
            }
        };
        NixStringContext context;
        auto alias = nested.rootPath(CanonPath("/copy-request/link/file"));
        auto old = nested.copyPathToStore(context, alias);
        auto current = nested.copyPathToStore(context, alias);
        EXPECT_NE(old, current);
        EXPECT_EQ(
            old,
            fetchToStore(fetchSettings, *store, SourcePath{source, CanonPath("old/file")}, FetchMode::DryRun, "file"));
        EXPECT_EQ(
            current,
            fetchToStore(fetchSettings, *store, SourcePath{source, CanonPath("new/file")}, FetchMode::DryRun, "file"));
        auto before = source->reads;
        EXPECT_EQ(nested.copyPathToStore(context, alias), current);
        EXPECT_EQ(source->reads, before);
    }
}

} // namespace nix
