#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "nix/util/memory-source-accessor.hh"
#include "nix/util/serialise.hh"
#include "nix/store/dummy-store-impl.hh"
#include "nix/store/globals.hh"
#include "nix/store/realisation.hh"

#include "nix/store/tests/libstore.hh"
#include "nix/util/tests/json-characterization.hh"

namespace nix {

class DummyStoreTest : public virtual CharacterizationTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "dummy-store";

public:

    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / testStem;
    }

    static void SetUpTestSuite()
    {
        initLibStore(false);
    }
};

TEST(DummyStore, storeDir_default)
{
    DummyStoreConfig config{{}};
    EXPECT_EQ(config.storeDir, "/nix/store");
}

TEST(DummyStore, storeDir_absolutePath)
{
    DummyStoreConfig config{{{"store", "/my/store"}}};
    EXPECT_EQ(config.storeDir, "/my/store");
}

TEST(DummyStore, storeDir_canonicalized)
{
    DummyStoreConfig config{{{"store", "/my//store/../store"}}};
    EXPECT_EQ(config.storeDir, "/my/store");
}

TEST(DummyStore, storeDir_relativePath_rejected)
{
    EXPECT_THROW((DummyStoreConfig{{{"store", "my/store"}}}), UsageError);
}

TEST(DummyStore, storeDir_empty_rejected)
{
    EXPECT_THROW((DummyStoreConfig{{{"store", ""}}}), UsageError);
}

TEST(DummyStore, getStateDir_default)
{
    // DummyStore uses the base StoreConfig::getStateDir which returns
    // the global settings.nixStateDir
    DummyStoreConfig config{{}};
    EXPECT_EQ(config.getStateDir(), settings.nixStateDir);
}

TEST(DummyStore, realisation_read)
{
    initLibStore(/*loadConfig=*/false);

    EnableExperimentalFeature enableCA{"ca-derivations"};

    auto store = [] {
        auto cfg = make_ref<DummyStoreConfig>(StoreReference::Params{});
        cfg->readOnly = false;
        return cfg->openDummyStore();
    }();

    StorePath drvPath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv"};

    auto outputName = "foo";

    EXPECT_EQ(store->queryRealisation({drvPath, outputName}), nullptr);

    UnkeyedRealisation value{
        .outPath = StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo"},
    };

    store->buildTrace.insert({drvPath, {{outputName, value}}});

    auto value2 = store->queryRealisation({drvPath, outputName});

    ASSERT_TRUE(value2);
    EXPECT_EQ(*value2, value);
}

/* ----------------------------------------------------------------------------
 * JSON
 * --------------------------------------------------------------------------*/

using nlohmann::json;

struct DummyStoreJsonTest : DummyStoreTest,
                            JsonCharacterizationTest<ref<DummyStore>>,
                            ::testing::WithParamInterface<std::pair<std::string_view, ref<DummyStore>>>
{};

TEST_P(DummyStoreJsonTest, from_json)
{
    auto & [name, expected] = GetParam();
    using namespace nlohmann;
    /* Cannot use `readJsonTest` because need to dereference the stores
       for equality. */
    readTest(std::string{name} + ".json", [&](const auto & encodedRaw) {
        auto encoded = json::parse(encodedRaw);
        ref<DummyStore> decoded = adl_serializer<ref<DummyStore>>::from_json(encoded);
        ASSERT_EQ(*decoded, *expected);
    });
}

TEST_P(DummyStoreJsonTest, to_json)
{
    auto & [name, value] = GetParam();
    writeJsonTest(name, value);
}

INSTANTIATE_TEST_SUITE_P(DummyStoreJSON, DummyStoreJsonTest, [] {
    initLibStore(false);
    auto writeCfg = make_ref<DummyStore::Config>(DummyStore::Config::Params{});
    writeCfg->readOnly = false;
    return ::testing::Values(
        std::pair{
            "empty",
            make_ref<DummyStore::Config>(DummyStore::Config::Params{})->openDummyStore(),
        },
        std::pair{
            "one-flat-file",
            [&] {
                auto store = writeCfg->openDummyStore();
                store->addToStore(
                    "my-file",
                    SourcePath{
                        [] {
                            auto sc = make_ref<MemorySourceAccessor>();
                            sc->root = MemorySourceAccessor::File{MemorySourceAccessor::File::Regular{
                                .executable = false,
                                .contents = "asdf",
                            }};
                            return sc;
                        }(),
                    },
                    ContentAddressMethod::Raw::NixArchive,
                    HashAlgorithm::SHA256);
                return store;
            }(),
        },
        std::pair{
            "one-derivation",
            [&] {
                auto store = writeCfg->openDummyStore();
                store->writeDerivation(Derivation{.name = "foo"});
                return store;
            }(),
        },
        std::pair{
            "one-realisation",
            [&] {
                auto store = writeCfg->openDummyStore();
                store->buildTrace.insert_or_assign(
                    StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv"},
                    std::map<std::string, UnkeyedRealisation>{
                        {
                            "out",
                            UnkeyedRealisation{
                                .outPath = StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo"},
                            },
                        },
                    });
                return store;
            }(),
        });
}());

/* A whole-store read borrows a selected memory object through callbacks.
   The callback may release both owners; the selected object must remain
   alive until delegation ends. A weak observation catches the old raw
   borrow before MemorySourceAccessor can resume on freed contents. */
TEST(DummyStore, wholeStoreReadOwnsItsSelectedChildAcrossCallbacks)
{
    initLibStore(false);

    struct CallbackStopped
    {};

    struct LostChild
    {};

    for (bool atSize : {false, true})
        for (bool release : {false, true})
            for (bool stop : {false, true}) {
                SCOPED_TRACE(::testing::Message() << "size=" << atSize << " release=" << release << " stop=" << stop);
                auto config = make_ref<DummyStoreConfig>(StoreReference::Params{});
                config->readOnly = false;
                auto store = config->openDummyStore().get_ptr();
                EXPECT_TRUE(store->getFSAccessor()->readDirectory(CanonPath::root).empty());
                const std::string expected = "owned callback bytes";
                StringSource source{expected};
                auto path = store->addToStoreFromDump(
                    source,
                    "callback-child",
                    FileSerialisationMethod::Flat,
                    ContentAddressMethod::Raw::Flat,
                    HashAlgorithm::SHA256,
                    {},
                    NoRepair);
                std::weak_ptr<SourceAccessor> child = store->getFSAccessor(path);
                auto view = store->getFSAccessor().get_ptr();
                EXPECT_EQ(
                    view->readDirectory(CanonPath::root),
                    (SourceAccessor::DirEntries{{std::string(path.to_string()), SourceAccessor::tRegular}}));
                EXPECT_EQ(view->showPath(CanonPath::root), config->storeDir + "/");
                EXPECT_EQ(view->showPath(CanonPath(path.to_string())), store->printStorePath(path) + "/");
                std::weak_ptr<SourceAccessor> outer = view;
                unsigned entered = 0;
                auto callback = [&] {
                    ++entered;
                    if (release) {
                        store.reset();
                        view.reset();
                    }
                    bool alive = !child.expired();
                    EXPECT_TRUE(alive) << "the selected child must outlive the callback";
                    if (!alive)
                        throw LostChild{};
                    if (stop)
                        throw CallbackStopped{};
                };
                std::string bytes;
                LambdaSink sink{[&](std::string_view data) {
                    if (!atSize)
                        callback();
                    bytes += data;
                }};
                bool stopped = false;
                try {
                    view->readFile(CanonPath(path.to_string()), sink, [&](uint64_t size) {
                        EXPECT_EQ(size, expected.size());
                        if (atSize)
                            callback();
                    });
                } catch (CallbackStopped &) {
                    stopped = true;
                } catch (LostChild &) {
                    /* The assertion above is the negative-control failure;
                       do not resume the old implementation's dangling read. */
                }
                EXPECT_EQ(entered, 1u);
                EXPECT_EQ(stopped, stop);
                if (!stop)
                    EXPECT_EQ(bytes, expected);
                store.reset();
                view.reset();
                EXPECT_TRUE(outer.expired());
                EXPECT_TRUE(child.expired());
            }
}

} // namespace nix
