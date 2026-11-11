#if !defined(_WIN32) && !defined(__CYGWIN__)

#  include <thread>

#  include <gmock/gmock.h>
#  include <gtest/gtest.h>

#  include "nix/store/daemon.hh"
#  include "nix/store/derivations.hh"
#  include "nix/store/dummy-store.hh"
#  include "nix/store/worker-protocol-connection.hh"
#  include "nix/util/file-descriptor.hh"
#  include "nix/util/finally.hh"
#  include "nix/util/signals.hh"

namespace nix {

TEST(DaemonConnection, interruptedHandshakeClearsInterrupt)
{
    auto config = make_ref<DummyStoreConfig>(StoreReference::Params{});
    auto store = config->openStore();

    Pipe fromClient, toClient;
    fromClient.create();
    toClient.create();
    FdSink clientSink(fromClient.writeSide.get());
    clientSink << 0; // Keep the read ready. The interrupt is detected before this value is checked.
    clientSink.flush();

    // Interrupt the first handshake read, before request processing begins.
    setInterrupted(true);
    Finally clearInterruptFlag([] { setInterrupted(false); });

    EXPECT_THROW(
        daemon::processConnection(
            store,
            FdSource(fromClient.readSide.get()),
            FdSink(toClient.writeSide.get()),
            Trusted,
            daemon::RecursiveFlag::NotRecursive),
        Interrupted);
    EXPECT_FALSE(getInterrupted());
}

TEST(DaemonConnection, untrustedBuildDerivationRepairIsRefused)
{
    auto store = make_ref<DummyStoreConfig>(StoreReference::Params{})->openStore();
    Pipe in, out;
    in.create();
    out.create();
    // `Recursive` leaves the global logger alone.
    std::jthread daemon([&] {
        daemon::processConnection(
            store,
            FdSource(in.readSide.get()),
            FdSink(out.writeSide.get()),
            NotTrusted,
            daemon::RecursiveFlag::Recursive);
    });
    Finally closeIn([&] { in.writeSide.close(); });

    struct : WorkerProto::BasicClientConnection
    {
        void closeWrite() override {}
    } conn;

    conn.to = FdSink(in.writeSide.get());
    conn.from = FdSource(out.readSide.get());
    conn.protoVersion = conn.handshake(conn.to, conn.from, WorkerProto::latest);
    conn.postHandshake(*store);
    conn.processStderrReturn();
    conn.putBuildDerivationRequest(*store, nullptr, StorePath::random("x.drv"), BasicDerivation{.name = "x"}, bmRepair);
    // Read once: gmock calls the callable again to explain a mismatch.
    auto ex = conn.processStderrReturn();
    ASSERT_TRUE(ex);
    EXPECT_THAT(
        [&] { std::rethrow_exception(ex); },
        testing::ThrowsMessage<Error>(testing::HasSubstr("repairing is not allowed")));
}

} // namespace nix

#endif
