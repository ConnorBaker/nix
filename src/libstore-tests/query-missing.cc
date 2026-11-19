#include <gtest/gtest.h>

#include "nix/store/derivations.hh"
#include "nix/store/dummy-store-impl.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/store/uds-remote-store.hh"
#include "nix/util/callback.hh"

namespace nix {

/**
 * A store that forwards what `Store::queryMissing` asks to a dummy
 * store, and counts its validity questions.
 */
struct CountingStore : virtual Store
{
    ref<DummyStore> inner;

    /**
     * Calls to `isValidPathUncached` and `queryValidPaths`: against a
     * remote store, each is one round trip.
     */
    size_t validityQueries = 0;

    CountingStore(ref<DummyStoreConfig> config, ref<DummyStore> inner)
        : Store{*config}
        , inner(inner)
    {
    }

    void anchor() override {}

    bool isValidPathUncached(const StorePath & path) override
    {
        ++validityQueries;
        return inner->isValidPath(path);
    }

    StorePathSet queryValidPaths(const StorePathSet & paths, SubstituteFlag maybeSubstitute) override
    {
        ++validityQueries;
        return inner->queryValidPaths(paths, maybeSubstitute);
    }

    bool queryValidPathsIsOneRequest() override
    {
        return true;
    }

    void queryPathInfoUncached(
        const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override
    {
        try {
            callback(inner->queryPathInfo(path).get_ptr());
        } catch (InvalidPath &) {
            callback(nullptr);
        } catch (...) {
            callback.rethrow();
        }
    }

    StorePath writeDerivation(const Derivation & drv, RepairFlag repair) override
    {
        return inner->writeDerivation(drv, repair);
    }

    Derivation readDerivation(const StorePath & drvPath) override
    {
        return inner->readDerivation(drvPath);
    }

    Derivation readInvalidDerivation(const StorePath & drvPath) override
    {
        return inner->readInvalidDerivation(drvPath);
    }

    /* Not asked by `queryMissing` without CA derivations or substituters. */

    void queryRealisationUncached(
        const DrvOutput &, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override
    {
        callback(nullptr);
    }

    std::optional<StorePath> queryPathFromHashPart(const std::string &) override
    {
        unsupported("queryPathFromHashPart");
    }

    void addToStore(const ValidPathInfo &, Source &, RepairFlag, CheckSigsFlag) override
    {
        unsupported("addToStore");
    }

    StorePath addToStoreFromDump(
        Source &,
        std::string_view,
        FileSerialisationMethod,
        ContentAddressMethod,
        HashAlgorithm,
        const StorePathSet &,
        RepairFlag) override
    {
        unsupported("addToStoreFromDump");
    }

    void registerDrvOutputUnchecked(const Realisation &) override
    {
        unsupported("registerDrvOutputUnchecked");
    }

    ref<SourceAccessor> getFSAccessor(bool) override
    {
        unsupported("getFSAccessor");
    }

    std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath &, bool) override
    {
        unsupported("getFSAccessor");
    }

    std::optional<TrustedFlag> isTrustedClient() override
    {
        return Trusted;
    }
};

class QueryMissingTest : public LibStoreTest
{
protected:
    ref<CountingStore> counting;

    QueryMissingTest()
        : LibStoreTest([]() {
            auto config = make_ref<DummyStoreConfig>(DummyStoreConfig::Params{});
            config->readOnly = false;
            return make_ref<CountingStore>(config, config->openDummyStore());
        }())
        , counting(store.dynamic_pointer_cast<CountingStore>())
    {
    }

    /**
     * Write an input-addressed derivation that depends on the `out`
     * output of each of `inputs`.
     */
    StorePath addDrv(std::string name, const std::vector<StorePath> & inputs)
    {
        Derivation drv{
            .outputs{{"out", DerivationOutput::Deferred{}}},
            .platform = "x86_64-linux",
            .builder = "/bin/sh",
            .env = {{"out", ""}},
            .name = std::move(name),
        };
        for (auto & input : inputs)
            drv.inputs.insert(
                SingleDerivedPath::Built{
                    .drvPath = makeConstantStorePathRef(input),
                    .output = "out",
                });
        fillInOutputPaths(drv, *store);
        return store->writeDerivation(drv, NoRepair);
    }

    static DerivedPath built(const StorePath & drvPath)
    {
        return DerivedPath::Built{
            .drvPath = makeConstantStorePathRef(drvPath),
            .outputs = OutputsSpec::All{},
        };
    }
};

/**
 * N derivations in `depth` levels, every one of which must be built,
 * and one that the store does not have, which is unknown. The number of
 * validity questions must not grow with N.
 */
TEST_F(QueryMissingTest, validityQueriesPerLevel)
{
    constexpr size_t depth = 4, width = 16;

    StorePathSet all;
    std::vector<StorePath> previous;
    for (size_t level = 0; level < depth; ++level) {
        std::vector<StorePath> current;
        for (size_t i = 0; i < width; ++i)
            current.push_back(addDrv(fmt("level-%d-%d", level, i), previous));
        all.insert(current.begin(), current.end());
        previous = std::move(current);
    }

    std::vector<DerivedPath> targets;
    for (auto & drvPath : previous)
        targets.push_back(built(drvPath));
    auto absent = StorePath{"00000000000000000000000000000000-absent.drv"};
    targets.push_back(built(absent));

    counting->validityQueries = 0;
    auto missing = store->queryMissing(targets);

    EXPECT_EQ(missing.willBuild, all);
    EXPECT_EQ(missing.unknown, StorePathSet{absent});

    /* Two per level: the derivations, then their wanted outputs. */
    EXPECT_LE(counting->validityQueries, 2 * depth);
}

/**
 * Whether a daemon is asked in batches is known without asking it: a
 * store that has not connected yet must not connect to answer.
 */
TEST(QueryMissing, batchingOpensNoConnection)
{
    struct NoDaemon : UDSRemoteStore
    {
        size_t opened = 0;

        NoDaemon(ref<const Config> config)
            : Store{*config}
            , LocalFSStore{*config}
            , RemoteStore{*config}
            , UDSRemoteStore{config}
        {
        }

        ref<RemoteStore::Connection> openConnection() override
        {
            ++opened;
            throw Error("no daemon here");
        }
    };

    auto store = make_ref<NoDaemon>(make_ref<UDSRemoteStoreConfig>("/no-daemon", StoreConfig::Params{}));
    bool batched = false;
    EXPECT_NO_THROW(batched = store->queryValidPathsIsOneRequest());
    EXPECT_TRUE(batched);
    EXPECT_EQ(store->opened, 0);
}

} // namespace nix
