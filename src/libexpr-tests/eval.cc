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
        {"attrs-merged-discard",
         R"({ a = {}; a = { static = 1; ${"dynamic"} = 2; inherit ({ inherited = 3; }) inherited; }; })",
         Failure::None},
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

} // namespace nix
