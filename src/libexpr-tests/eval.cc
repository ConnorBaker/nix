#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "nix/expr/eval.hh"
#include "nix/util/memory-source-accessor.hh"
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

TEST_F(EvalStateTest, mountInputInvalidatesPreviouslyObservedStoreNodes)
{
    using File = MemorySourceAccessor::File;
    AutoDelete root{canonPath(createTempDir(), true)};
    auto config = make_ref<LocalStoreConfig>(
        root.path() / "physical-root", StoreConfig::Params{{"store", (root.path() / "logical-store").string()}});
    auto local = make_ref<LocalStore>(config);
    auto nodes = [](std::string contents) {
        return std::vector<File>{
            File{File::Regular{.executable = false, .contents = contents}},
            File{File::Regular{.executable = true, .contents = contents}},
            File{File::Symlink{.target = contents}},
            File{File::Directory{.entries = {{"child", File{File::Regular{.contents = contents}}}}}},
        };
    };
    auto before = nodes("before");
    auto after = nodes("after-longer");
    size_t serial = 0;
    /* Every old node kind, absence included, crossed with every mounted
       node kind; repeat through the restricted evaluator's allow list. */
    for (bool restricted : {false, true})
        for (size_t oldNode = 0; oldNode <= before.size(); ++oldNode)
            for (size_t newNode = 0; newNode < after.size(); ++newNode) {
                SCOPED_TRACE(fmt("restricted=%d old=%d new=%d", restricted, oldNode, newNode));
                EvalSettings nestedSettings{readOnlyMode};
                nestedSettings.nixPath = {};
                nestedSettings.pureEval = false;
                nestedSettings.restrictEval = restricted;
                EvalState nested({}, local, fetchSettings, nestedSettings);
                auto mounted = make_ref<MemorySourceAccessor>();
                mounted->root = after[newNode];
                fetchers::Input input;
                input.attrs.insert_or_assign("name", fmt("mount-cache-%d", serial++));
                auto [storePath, hash] =
                    fetchToStore2(fetchSettings, *local, SourcePath{mounted}, FetchMode::DryRun, input.getName());
                auto logical = CanonPath(local->printStorePath(storePath));
                ASSERT_FALSE(getFSSourceAccessor()->pathExists(logical));
                ASSERT_FALSE(local->isValidPath(storePath));
                if (oldNode < before.size()) {
                    auto old = make_ref<MemorySourceAccessor>();
                    old->root = before[oldNode];
                    StringSink dump;
                    old->dumpPath(CanonPath::root, dump);
                    StringSource source{dump.s};
                    restorePath(local->toRealPath(storePath), source);
                }
                nested.allowPath(storePath);
                auto observed = nested.rootFS->maybeLstat(logical);
                EXPECT_EQ(bool(observed), oldNode < before.size());
                if (observed && observed->type == SourceAccessor::tSymlink)
                    EXPECT_EQ(nested.rootFS->readLink(logical), "before");

                EXPECT_EQ(nested.mountInput(input, mounted), storePath);
                auto expectedStat = mounted->lstat(CanonPath::root);
                auto actualStat = nested.rootFS->lstat(logical);
                EXPECT_EQ(actualStat.type, expectedStat.type);
                EXPECT_EQ(actualStat.isExecutable, expectedStat.isExecutable);
                StringSink actual, expected;
                nested.rootFS->dumpPath(logical, actual);
                mounted->dumpPath(CanonPath::root, expected);
                EXPECT_EQ(actual.s, expected.s);
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

} // namespace nix
