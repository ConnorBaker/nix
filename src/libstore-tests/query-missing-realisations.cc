#include <gtest/gtest.h>

#ifndef _WIN32

#  include <condition_variable>
#  include <mutex>
#  include <thread>

#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>

#  include "nix/store/derivations.hh"
#  include "nix/store/dummy-store-impl.hh"
#  include "nix/store/filetransfer.hh"
#  include "nix/store/globals.hh"
#  include "nix/store/tests/libstore.hh"
#  include "nix/store/tests/libstore-network.hh"
#  include "nix/util/environment-variables.hh"
#  include "nix/util/file-system.hh"

namespace nix {

namespace {

/**
 * A binary cache over HTTP that serves `nix-cache-info` and holds every
 * other request until `expected` of them are waiting at once, then
 * answers them all with 404. With fewer it answers only when destroyed.
 * `maxWaiting` is the most requests that were ever waiting at once.
 */
struct HoldingCache
{
    const std::string storeDir;
    const size_t expected;
    AutoCloseFD listener;
    uint16_t port = 0;

    std::mutex mutex;
    std::condition_variable cv;
    size_t waiting = 0, maxWaiting = 0;

    std::atomic<bool> stop = false;
    std::vector<std::thread> connections;
    std::thread acceptor;

    HoldingCache(std::string storeDir, size_t expected)
        : storeDir(std::move(storeDir))
        , expected(expected)
        , listener(socket(AF_INET, SOCK_STREAM, 0))
    {
        sockaddr_in addr{.sin_family = AF_INET, .sin_port = 0, .sin_addr = {htonl(INADDR_LOOPBACK)}};
        socklen_t len = sizeof(addr);
        if (!listener || bind(listener.get(), (sockaddr *) &addr, len) || listen(listener.get(), 64)
            || getsockname(listener.get(), (sockaddr *) &addr, &len))
            throw SysError("setting up the test HTTP server");
        port = ntohs(addr.sin_port);
        acceptor = std::thread([this] {
            while (!stop) {
                pollfd p{.fd = listener.get(), .events = POLLIN};
                if (poll(&p, 1, 100) == 1)
                    if (int fd = accept(listener.get(), nullptr, nullptr); fd != -1)
                        connections.emplace_back([this, fd] { serve(AutoCloseFD{fd}); });
            }
        });
    }

    void serve(AutoCloseFD fd)
    {
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            auto n = ::read(fd.get(), buf, sizeof(buf));
            if (n <= 0)
                return;
            request.append(buf, n);
        }
        auto path = request.substr(4, request.find(' ', 4) - 4); /* after "GET " */

        std::string status = "404 Not Found", body;
        if (path == "/nix-cache-info") {
            status = "200 OK";
            body = "StoreDir: " + storeDir + "\n";
        } else {
            std::unique_lock lock(mutex);
            maxWaiting = std::max(maxWaiting, ++waiting);
            cv.notify_all();
            cv.wait(lock, [&] { return stop || maxWaiting >= expected; });
            --waiting;
        }
        writeFull(
            fd.get(),
            fmt("HTTP/1.1 %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", status, body.size(), body));
    }

    ~HoldingCache()
    {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        cv.notify_all();
        acceptor.join();
        for (auto & t : connections)
            t.join();
    }
};

/**
 * Ask `queryMissing` about `n` floating content-addressed derivations,
 * with a `HoldingCache` as the only substituter.
 */
std::string queryMissingConcurrently(size_t n)
{
    auto cacheHome = createTempDir();
    AutoDelete deleteCacheHome(cacheHome);
    setEnv("NIX_CACHE_HOME", cacheHome.string().c_str());
    EnableExperimentalFeature enableCA{"ca-derivations"};

    auto config = make_ref<DummyStoreConfig>(DummyStoreConfig::Params{});
    config->readOnly = false;
    auto store = config->openDummyStore();

    HoldingCache cache{store->storeDir, n};
    settings.getWorkerSettings().substituters = {StoreReference::parse(fmt("http://127.0.0.1:%d", cache.port))};
    /* The cache speaks HTTP/1.1 only. While HTTP/2 is allowed, curl makes
       a request that starts during the first connection's setup wait
       until that connection has answered, to learn whether it can carry
       both (`CURLOPT_PIPEWAIT`): some lookups would then not overlap,
       however `queryMissing` asks for them. */
    fileTransferSettings.enableHttp2 = false;

    std::vector<DerivedPath> targets;
    for (size_t i = 0; i < n; ++i) {
        Derivation drv{
            .outputs{
                {"out",
                 DerivationOutput{DerivationOutput::CAFloating{
                     .method = ContentAddressMethod::Raw::NixArchive,
                     .hashAlgo = HashAlgorithm::SHA256,
                 }}},
            },
            .name = fmt("concurrent-%d", i),
        };
        targets.push_back(
            DerivedPath::Built{
                .drvPath = makeConstantStorePathRef(store->writeDerivation(drv)),
                .outputs = OutputsSpec::All{},
            });
    }

    auto missing = store->queryMissing(targets);
    std::unique_lock lock(cache.mutex);
    return fmt("waiting at once: %d, will build: %d\n", cache.maxWaiting, missing.willBuild.size());
}

} // namespace

class QueryMissingRealisationsTest : public testing::LibStoreNetworkTest
{};

/**
 * `queryMissing` asks the substituters for the realisations of floating
 * content-addressed outputs. Those lookups must overlap, not wait for
 * each other.
 */
TEST_F(QueryMissingRealisationsTest, lookupsAreConcurrent)
{
    /* The default substituters are opened once per process, so run in a
       fresh one. */
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            std::cerr << queryMissingConcurrently(4);
            std::exit(0);
        },
        ::testing::ExitedWithCode(0),
        "waiting at once: 4, will build: 4");
}

} // namespace nix

#endif
