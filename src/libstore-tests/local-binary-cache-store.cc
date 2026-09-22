#include <gtest/gtest.h>

#include "nix/store/local-binary-cache-store.hh"
#include "nix/store/globals.hh"
#include "nix/store/nar-info.hh"
#include "nix/util/file-system.hh"

#include <array>

namespace nix {

TEST(LocalBinaryCacheStore, storeDir_absolutePath)
{
    LocalBinaryCacheStoreConfig config{std::filesystem::path("/foo/bar/baz"), {{"store", "/my/store"}}};
    EXPECT_EQ(config.storeDir, "/my/store");
}

TEST(LocalBinaryCacheStore, storeDir_relativePath_rejected)
{
    EXPECT_THROW(
        LocalBinaryCacheStoreConfig(std::filesystem::path("/foo/bar/baz"), {{"store", "my/store"}}), UsageError);
}

TEST(LocalBinaryCacheStore, constructConfig)
{
    LocalBinaryCacheStoreConfig config{std::filesystem::path("/foo/bar/baz"), {}};
    EXPECT_EQ(config.binaryCacheDir, "/foo/bar/baz");
}

static NarInfo makeTestNarInfo(const Store & store, const StorePath & path)
{
    NarInfo info{store, path, std::nullopt};
    info.assertedNarHash = hashString(HashAlgorithm::SHA256, "contents");
    info.narSize = 1;
    info.url = "nar/unused.nar";
    info.compression = CompressionAlgo::none;
    return info;
}

TEST(LocalBinaryCacheStore, validityChecksTheMetadataIdentityOnFirstAndRepeatedQueries)
{
    initLibStore(false);
    auto tmpDir = createTempDir();
    AutoDelete cleanup{tmpDir};
    const StorePath canonical{"00000000000000000000000000000000-canonical"};
    const std::array<StorePath, 5> queries{
        canonical,
        StorePath{"00000000000000000000000000000000-" + std::string(Store::MissingName)},
        StorePath{"00000000000000000000000000000000-wrong-name"},
        StorePath{"11111111111111111111111111111111-canonical"},
        StorePath{"22222222222222222222222222222222-absent"}};

    for (bool memoryCache : {false, true})
        for (size_t first = 0; first < queries.size(); ++first) {
            SCOPED_TRACE(fmt("memory=%d first=%d", memoryCache, first));
            auto root = tmpDir / fmt("cache-%d-%d", memoryCache, first);
            auto store = make_ref<LocalBinaryCacheStoreConfig>(
                             root, StoreConfig::Params{{"path-info-cache-size", memoryCache ? "16" : "0"}})
                             ->openStore();
            auto body = makeTestNarInfo(*store, canonical).to_string(*store);
            writeFile(root / (std::string(canonical.hashPart()) + ".narinfo"), body);
            /* Existence under another hash also fails to establish the
               requested identity: the description still names canonical. */
            writeFile(root / (std::string(queries[3].hashPart()) + ".narinfo"), body);
            for (unsigned repeat = 0; repeat < 2; ++repeat)
                for (size_t offset = 0; offset < queries.size(); ++offset) {
                    auto i = (first + offset) % queries.size();
                    SCOPED_TRACE(fmt("repeat=%d query=%d", repeat, i));
                    EXPECT_EQ(store->isValidPath(queries[i]), i < 2);
                    if (i < 2)
                        EXPECT_EQ(store->queryPathInfo(queries[i])->path, canonical);
                    else
                        EXPECT_THROW(store->queryPathInfo(queries[i]), InvalidPath);
                }
        }
}

TEST(LocalBinaryCacheStore, validityCachesTheReadMetadataAndPropagatesCorruption)
{
    initLibStore(false);
    auto tmpDir = createTempDir();
    AutoDelete cleanup{tmpDir};
    const StorePath path{"00000000000000000000000000000000-canonical"};
    for (bool memoryCache : {false, true}) {
        SCOPED_TRACE(memoryCache);
        auto root = tmpDir / (memoryCache ? "cached" : "uncached");
        auto store = make_ref<LocalBinaryCacheStoreConfig>(
                         root, StoreConfig::Params{{"path-info-cache-size", memoryCache ? "16" : "0"}})
                         ->openStore();
        auto narInfo = root / (std::string(path.hashPart()) + ".narinfo");
        writeFile(narInfo, makeTestNarInfo(*store, path).to_string(*store));
        ASSERT_TRUE(store->isValidPath(path));
        writeFile(narInfo, "not a narinfo\n");
        if (memoryCache) {
            EXPECT_TRUE(store->isValidPath(path));
            EXPECT_EQ(store->queryPathInfo(path)->path, path);
        } else {
            EXPECT_THROW(store->isValidPath(path), Error);
            EXPECT_THROW(store->queryPathInfo(path), Error);
        }
        store->clearPathInfoCache();
        EXPECT_THROW(store->isValidPath(path), Error);
        EXPECT_THROW(store->queryPathInfo(path), Error);
    }
}

TEST(LocalBinaryCacheStore, publicationInvalidatesTheHashOnlyName)
{
    initLibStore(false);
    auto tmpDir = createTempDir();
    AutoDelete cleanup{tmpDir};
    const std::string contents = "published after an absent-path observation";
    enum class Query { Validity, Description, Hash };
    unsigned cells = 0;
    for (bool memoryCache : {false, true})
        for (auto name : {"canonical", Store::MissingName})
            for (auto query : {Query::Validity, Query::Description, Query::Hash})
                for (unsigned requested = 0; requested < (query == Query::Hash ? 1u : 3u); ++requested) {
                    SCOPED_TRACE(
                        fmt("memory=%d name=%s query=%d requested=%d", memoryCache, name, int(query), requested));
                    auto store = make_ref<LocalBinaryCacheStoreConfig>(
                                     tmpDir / std::to_string(cells++),
                                     StoreConfig::Params{
                                         {"path-info-cache-size", memoryCache ? "16" : "0"}, {"compression", "none"}})
                                     ->openStore();
                    auto expected = store->makeFixedOutputPath(
                        name,
                        FixedOutputInfo{
                            .method = FileIngestionMethod::Flat,
                            .hash = hashString(HashAlgorithm::SHA256, contents),
                            .references = {},
                        });
                    auto hash = std::string(expected.hashPart());
                    auto wildcard = StorePath(hash + '-' + Store::MissingName);
                    auto wrong = StorePath(hash + "-wrong-name");
                    auto before = requested == 0 ? expected : requested == 1 ? wrong : wildcard;
                    switch (query) {
                    case Query::Validity:
                        EXPECT_FALSE(store->isValidPath(before));
                        break;
                    case Query::Description:
                        EXPECT_THROW(store->queryPathInfo(before), InvalidPath);
                        break;
                    case Query::Hash:
                        EXPECT_FALSE(store->queryPathFromHashPart(hash));
                        break;
                    }

                    StringSource source{contents};
                    auto published = store->addToStoreFromDump(
                        source, name, FileSerialisationMethod::Flat, ContentAddressMethod::Raw::Flat);
                    EXPECT_EQ(published, expected);
                    EXPECT_TRUE(store->isValidPath(published));
                    EXPECT_TRUE(store->isValidPath(wildcard));
                    EXPECT_FALSE(store->isValidPath(wrong));
                    EXPECT_NO_THROW(EXPECT_EQ(store->queryPathInfo(wildcard)->path, published));
                    EXPECT_EQ(store->queryPathFromHashPart(hash), published);

                    /* A hash-only positive is also a description of this
                       publication, not of every later metadata update. */
                    Signature signature{.keyName = "new-signature", .sig = std::string(64, '\0')};
                    store->addSignatures(published, {signature});
                    EXPECT_NO_THROW(EXPECT_TRUE(store->queryPathInfo(wildcard)->sigs.contains(signature)));
                }
    EXPECT_EQ(cells, 28u);
}

} // namespace nix
