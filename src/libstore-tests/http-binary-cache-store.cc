#include <gtest/gtest.h>
#include <regex>
#include <thread>

#include "nix/store/http-binary-cache-store.hh"
#include "nix/util/callback.hh"
#include "nix/store/tests/https-store.hh"

namespace nix {

using Authority = ParsedURL::Authority;

TEST(HttpBinaryCacheStore, storeDir_absolutePath)
{
    HttpBinaryCacheStoreConfig config{parseURL("https://example.com"), {{"store", "/my/store"}}};
    EXPECT_EQ(config.storeDir, "/my/store");
}

TEST(HttpBinaryCacheStore, storeDir_relativePath_rejected)
{
    EXPECT_THROW(HttpBinaryCacheStoreConfig(parseURL("https://example.com"), {{"store", "my/store"}}), UsageError);
}

TEST(HttpBinaryCacheStore, constructConfig)
{
    HttpBinaryCacheStoreConfig config{
        {
            .scheme = "http",
            .authority = Authority{.host = "foo.bar.baz"},
        },
        {},
    };

    EXPECT_EQ(config.cacheUri.to_string(), "http://foo.bar.baz");
}

TEST(HttpBinaryCacheStore, constructConfigNoTrailingSlash)
{
    HttpBinaryCacheStoreConfig config{parseURL("https://foo.bar.baz/a/b/"), {}};
    EXPECT_EQ(config.cacheUri.to_string(), "https://foo.bar.baz/a/b");
}

TEST(HttpBinaryCacheStore, constructConfigWithParams)
{
    StoreConfig::Params params{{"compression", "xz"}};
    HttpBinaryCacheStoreConfig config{parseURL("https://foo.bar.baz/a/b/"), params};
    EXPECT_EQ(config.cacheUri.to_string(), "https://foo.bar.baz/a/b");
    EXPECT_EQ(config.getReference().params, params);
}

TEST(HttpBinaryCacheStore, constructConfigWithParamsAndUrlWithParams)
{
    StoreConfig::Params params{{"compression", "xz"}};
    HttpBinaryCacheStoreConfig config{parseURL("https://foo.bar.baz/a/b?some-param=some-value"), params};
    EXPECT_EQ(config.cacheUri.to_string(), "https://foo.bar.baz/a/b?some-param=some-value");
    EXPECT_EQ(config.getReference().params, params);
}

using testing::HttpsBinaryCacheStoreMtlsTest;
using testing::HttpsBinaryCacheStoreTest;

TEST_F(HttpsBinaryCacheStoreTest, queryPathInfo)
{
    auto store = openStore(makeConfig());
    StringSource dump{std::string_view("test")};
    auto path = localCacheStore->addToStoreFromDump(dump, "test-name", FileSerialisationMethod::Flat);
    EXPECT_NO_THROW(store->queryPathInfo(path));
}

TEST_F(HttpsBinaryCacheStoreMtlsTest, queryPathInfo)
{
    auto config = makeConfig();
    config->tlsCert = clientCert;
    config->tlsKey = clientKey;
    auto store = openStore(config);
    StringSource dump{std::string_view("test")};
    auto path = localCacheStore->addToStoreFromDump(dump, "test-name", FileSerialisationMethod::Flat);
    EXPECT_NO_THROW(store->queryPathInfo(path));
}

TEST_F(HttpsBinaryCacheStoreMtlsTest, rejectsWithoutClientCert)
{
    testFileTransferSettings->tries = 1;
    EXPECT_THROW(openStore(makeConfig()), Error);
}

TEST_F(HttpsBinaryCacheStoreMtlsTest, rejectsWrongClientCert)
{
    auto wrongKey = tmpDir / "wrong.key";
    auto wrongCert = tmpDir / "wrong.crt";

    // clang-format off
    openssl({"ecparam", "-genkey", "-name", "prime256v1", "-out", wrongKey.string()});
    openssl({"req", "-new", "-x509", "-days", "1", "-key", wrongKey.string(), "-out", wrongCert.string(), "-subj", "/CN=WrongClient"});
    // clang-format on

    auto config = makeConfig();
    config->tlsCert = wrongCert;
    config->tlsKey = wrongKey;
    testFileTransferSettings->tries = 1;
    EXPECT_THROW(openStore(config), Error);
}

TEST_F(HttpsBinaryCacheStoreMtlsTest, doesNotSendCertOnRedirectToDifferentAuthority)
{
    StringSource dump{std::string_view("test")};
    auto path = localCacheStore->addToStoreFromDump(dump, "test-name", FileSerialisationMethod::Flat);

    for (auto & entry : DirectoryIterator{cacheDir})
        if (entry.path().extension() == ".narinfo") {
            auto content = readFile(entry.path());
            content = std::regex_replace(content, std::regex("URL: nar/"), fmt("URL: https://127.0.0.1:%d/nar/", port));
            writeFile(entry.path(), content);
        }

    auto config = makeConfig();
    config->tlsCert = clientCert;
    config->tlsKey = clientKey;
    testFileTransferSettings->tries = 1;
    auto store = openStore(config);
    auto info = store->queryPathInfo(path);
    NullSink null;
    EXPECT_THROW(store->narFromPath(path, null), Error);
}

/* Call the base sink adapter explicitly: Http's own sink override must not
   hide the callback-to-synchronous handoff being checked. No request/init. */
TEST(HttpBinaryCacheStore, baseSinkAdapterPreservesCompletedBytesAndErrors)
{
    initLibStore(false);
    enum class Result { Empty, Bytes, Error };
    struct Probe : HttpBinaryCacheStore
    {
        Result result;
        bool asynchronous;
        unsigned calls = 0;
        std::jthread delivery;

        Probe(ref<Config> config, Result result, bool asynchronous)
            : Store{*config}, BinaryCacheStore{*config}, HttpBinaryCacheStore{config}
            , result(result), asynchronous(asynchronous)
        {
            diskCache = nullptr;
        }

        void getFile(const std::string & path, Callback<std::optional<std::string>> callback) noexcept override
        {
            EXPECT_EQ(path, "payload");
            ++calls;
            auto complete = [result = result, callback = std::move(callback)]() mutable {
                try {
                    if (result == Result::Error)
                        throw Error("completed cache read failed");
                    callback(std::optional<std::string>{result == Result::Empty ? "" : std::string("a\0b", 3)});
                } catch (...) {
                    callback.rethrow();
                }
            };
            if (asynchronous)
                delivery = std::jthread(std::move(complete));
            else
                complete();
        }
    };
    unsigned cells = 0;
    for (bool asynchronous : {false, true})
        for (auto result : {Result::Empty, Result::Bytes, Result::Error}) {
            SCOPED_TRACE(fmt("sink-adapter-cell=%u async=%d result=%d", cells++, asynchronous, int(result)));
            auto config = make_ref<HttpBinaryCacheStoreConfig>(parseURL("https://example.invalid"), StoreConfig::Params{});
            Probe probe{config, result, asynchronous};
            StringSink sink;
            sink.s = "prefix";
            if (result == Result::Error) {
                try {
                    probe.BinaryCacheStore::getFile("payload", sink);
                    FAIL() << "the completed read error must propagate";
                } catch (Error & e) {
                    EXPECT_THAT(e.what(), ::testing::HasSubstr("completed cache read failed"));
                }
                EXPECT_EQ(sink.s, "prefix");
            } else {
                EXPECT_NO_THROW(probe.BinaryCacheStore::getFile("payload", sink));
                EXPECT_EQ(sink.s, result == Result::Empty ? "prefix" : std::string("prefixa\0b", 9));
            }
            if (probe.delivery.joinable())
                probe.delivery.join();
            EXPECT_EQ(probe.calls, 1u);
        }
    EXPECT_EQ(cells, 6u);
    RecordProperty("finite_cases", int(cells));
}

} // namespace nix
