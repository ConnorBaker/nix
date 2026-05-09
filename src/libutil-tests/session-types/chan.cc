#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "nix/util/session-types/in-process.hh"

namespace nix::session {

namespace asio = boost::asio;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

/// Run an async test. Creates io_context, co_spawns f(executor), runs to
/// completion, rethrows. Same sync-wrapper pattern as computeClosure in
/// closure.hh:169-182.
inline void runAsync(auto f) {
    asio::io_context ctx;
    std::exception_ptr ex;
    asio::co_spawn(ctx, f(ctx.get_executor()),
        [&](std::exception_ptr e) { ex = e; });
    ctx.run();
    if (ex) std::rethrow_exception(ex);
}

/// Run two awaitables concurrently on the same strand.
/// Follows the forEachAsync join pattern from async.hh:90-116 but for
/// exactly 2 tasks. Both awaitables are co_spawned on the same strand.
/// The completion handler collects the first exception and resumes the
/// caller when both tasks complete.
inline asio::awaitable<void> concurrently(auto a, auto b) {
    auto exec = co_await asio::this_coro::executor;
    std::exception_ptr err;
    size_t pending = 2;

    co_await asio::async_initiate<decltype(asio::use_awaitable),
                                   void(std::exception_ptr)>(
        [&exec, &err, &pending, a = std::move(a), b = std::move(b)]
        (auto handler) mutable {
            auto h = std::make_shared<decltype(handler)>(std::move(handler));
            auto done = [&exec, h, &err, &pending](std::exception_ptr e) {
                if (e && !err) err = e;
                if (--pending == 0)
                    asio::post(exec, [h = std::move(h), &err]() mutable {
                        std::move(*h)(err);
                    });
            };
            asio::co_spawn(exec, std::move(a), done);
            asio::co_spawn(exec, std::move(b), done);
        },
        asio::use_awaitable);
}

// ---------------------------------------------------------------------------
// Basic send/recv round-trip
// ---------------------------------------------------------------------------

TEST(SessionChan, SendRecv) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        using Proto = Send<int, Recv<std::string, Done>>;
        auto [client, server] = channel<Proto>(exec);

        // Client sends int.
        auto c1 = co_await std::move(client).send(42);

        // Server (dual = Recv<int, Send<string, Done>>) receives int, sends string.
        auto [val, s1] = co_await std::move(server).recv();
        EXPECT_EQ(val, 42);
        auto s2 = co_await std::move(s1).send(std::string("hello"));

        // Client receives string.
        auto [str, c2] = co_await std::move(c1).recv();
        EXPECT_EQ(str, "hello");

        std::move(c2).close();
        std::move(s2).close();
    });
}

// ---------------------------------------------------------------------------
// Choose branch 0
// ---------------------------------------------------------------------------

TEST(SessionChan, ChooseBranch0) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        using Proto = Choose<Send<int, Done>, Done>;
        auto [client, server] = channel<Proto>(exec);

        // Client chooses branch 0 and sends an int.
        auto c1 = co_await std::move(client).template choose<0>();
        auto c2 = co_await std::move(c1).send(42);
        std::move(c2).close();

        // Server offers, visits branch 0.
        auto branches = co_await std::move(server).offer();
        int result = co_await std::move(branches).visit(
            [](auto chan) -> asio::awaitable<int> {
                auto [val, next] = co_await std::move(chan).recv();
                std::move(next).close();
                co_return val;
            },
            [](auto chan) -> asio::awaitable<int> {
                std::move(chan).close();
                co_return -1;
            }
        );
        EXPECT_EQ(result, 42);
    });
}

// ---------------------------------------------------------------------------
// Choose branch 1
// ---------------------------------------------------------------------------

TEST(SessionChan, ChooseBranch1) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        using Proto = Choose<Done, Send<std::string, Done>>;
        auto [client, server] = channel<Proto>(exec);

        // Client chooses branch 1 and sends a string.
        auto c1 = co_await std::move(client).template choose<1>();
        auto c2 = co_await std::move(c1).send(std::string("branch1"));
        std::move(c2).close();

        // Server offers, visits branch 1.
        auto branches = co_await std::move(server).offer();
        std::string result = co_await std::move(branches).visit(
            [](auto chan) -> asio::awaitable<std::string> {
                std::move(chan).close();
                co_return "wrong";
            },
            [](auto chan) -> asio::awaitable<std::string> {
                auto [val, next] = co_await std::move(chan).recv();
                std::move(next).close();
                co_return val;
            }
        );
        EXPECT_EQ(result, "branch1");
    });
}

// ---------------------------------------------------------------------------
// Loop: 3 iterations then Done
// ---------------------------------------------------------------------------

TEST(SessionChan, LoopThreeTimes) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        using Proto = Loop<Choose<Send<int, Continue<0>>, Done>>;
        auto [client, server] = channel<Proto>(exec);

        // Client: send 3 ints then signal done.
        {
            auto c = std::move(client);
            for (int i = 0; i < 3; i++) {
                auto c1 = co_await std::move(c).template choose<0>();
                c = co_await std::move(c1).send(i * 10);
            }
            auto done = co_await std::move(c).template choose<1>();
            std::move(done).close();
        }

        // Server: receive all ints.
        {
            using ServerProto = Dual_t<Proto>;
            auto s = std::move(server);
            int count = 0;
            bool running = true;
            while (running) {
                auto branches = co_await std::move(s).offer();
                auto next = co_await std::move(branches).visit(
                    [&](auto chan) -> asio::awaitable<std::optional<Chan<ServerProto, InProcessTx, InProcessRx>>> {
                        auto [val, cont] = co_await std::move(chan).recv();
                        EXPECT_EQ(val, count * 10);
                        count++;
                        co_return std::move(cont);
                    },
                    [&](auto chan) -> asio::awaitable<std::optional<Chan<ServerProto, InProcessTx, InProcessRx>>> {
                        std::move(chan).close();
                        running = false;
                        co_return std::nullopt;
                    }
                );
                if (next) s = std::move(*next);
            }
            EXPECT_EQ(count, 3);
        }
    });
}

// ---------------------------------------------------------------------------
// Sequential sends: Send<int, Send<float, Send<string, Done>>>
// ---------------------------------------------------------------------------

TEST(SessionChan, SequentialSends) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        using Proto = Send<int, Send<float, Send<std::string, Done>>>;
        auto [client, server] = channel<Proto>(exec);

        // Client sends all three.
        auto c1 = co_await std::move(client).send(1);
        auto c2 = co_await std::move(c1).send(2.5f);
        auto c3 = co_await std::move(c2).send(std::string("three"));
        std::move(c3).close();

        // Server receives all three.
        auto [v1, s1] = co_await std::move(server).recv();
        EXPECT_EQ(v1, 1);
        auto [v2, s2] = co_await std::move(s1).recv();
        EXPECT_FLOAT_EQ(v2, 2.5f);
        auto [v3, s3] = co_await std::move(s2).recv();
        EXPECT_EQ(v3, "three");
        std::move(s3).close();
    });
}

// ---------------------------------------------------------------------------
// Done: create and close immediately
// ---------------------------------------------------------------------------

TEST(SessionChan, DoneImmediate) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        auto [client, server] = channel<Done>(exec);
        std::move(client).close();
        std::move(server).close();
        co_return;
    });
}

// ---------------------------------------------------------------------------
// Move-only: Chan is not copyable
// ---------------------------------------------------------------------------

TEST(SessionChan, MoveOnly) {
    using C = Chan<Done, InProcessTx, InProcessRx>;
    static_assert(!std::is_copy_constructible_v<C>);
    static_assert(std::is_move_constructible_v<C>);
    static_assert(!std::is_copy_assignable_v<C>);
    // Move assignment is allowed (for loop patterns) but guarded:
    // asserts the target was already consumed.
    static_assert(std::is_move_assignable_v<C>);
}

// ---------------------------------------------------------------------------
// Dual symmetry: channel<S>() types check
// ---------------------------------------------------------------------------

TEST(SessionChan, DualSymmetry) {
    using Proto = Send<int, Recv<std::string, Done>>;
    using ClientChan = Chan<Proto, InProcessTx, InProcessRx>;
    using ServerChan = Chan<Dual_t<Proto>, InProcessTx, InProcessRx>;

    static_assert(std::is_same_v<ClientChan::session_type, Proto>);
    static_assert(std::is_same_v<ServerChan::session_type, Dual_t<Proto>>);
    static_assert(std::is_same_v<
        ServerChan::session_type, Recv<int, Send<std::string, Done>>>);

    runAsync([](auto exec) -> asio::awaitable<void> {
        using Proto = Send<int, Recv<std::string, Done>>;
        auto [client, server] = channel<Proto>(exec);

        // Consume both to satisfy linear requirement.
        auto c1 = co_await std::move(client).send(0);
        auto [v, s1] = co_await std::move(server).recv();
        auto s2 = co_await std::move(s1).send(std::string(""));
        auto [v2, c2] = co_await std::move(c1).recv();
        std::move(c2).close();
        std::move(s2).close();
    });
}

// ---------------------------------------------------------------------------
// Call sub-protocol
// ---------------------------------------------------------------------------

TEST(SessionChan, CallSubprotocol) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        using Sub = Send<int, Done>;
        using Proto = Call<Sub, Recv<float, Done>>;
        auto [client, server] = channel<Proto>(exec);

        // Client calls sub-protocol (sends int), then continues (receives float).
        auto c = co_await std::move(client).call(
            [](auto sub) -> asio::awaitable<Chan<Done, InProcessTx, InProcessRx>> {
                co_return co_await std::move(sub).send(42);
            });
        auto s = co_await std::move(server).call(
            [](auto sub) -> asio::awaitable<Chan<Done, InProcessTx, InProcessRx>> {
                auto [val, done] = co_await std::move(sub).recv();
                EXPECT_EQ(val, 42);
                co_return std::move(done);
            });

        // Continuation: server sends float, client receives.
        auto s2 = co_await std::move(s).send(3.14f);
        auto [fval, c2] = co_await std::move(c).recv();
        EXPECT_FLOAT_EQ(fval, 3.14f);

        std::move(c2).close();
        std::move(s2).close();
    });
}

// ---------------------------------------------------------------------------
// Nested call: Call with a loop sub-protocol
// ---------------------------------------------------------------------------

TEST(SessionChan, NestedCall) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        using DepLoop = Loop<Choose<Send<int, Continue<0>>, Done>>;
        // Continuation is Choose<Done, Done> — tests that call() composes with branching.
        using Proto = Call<DepLoop, Choose<Done, Done>>;
        auto [client, server] = channel<Proto>(exec);

        // Client drives the sub-protocol loop, then chooses branch 0 of continuation.
        auto c = co_await std::move(client).call(
            [](auto dep_chan) -> asio::awaitable<Chan<Done, InProcessTx, InProcessRx>>
        {
            auto c = std::move(dep_chan);
            for (int i = 0; i < 3; i++) {
                auto c1 = co_await std::move(c).template choose<0>();
                c = co_await std::move(c1).send(i);
            }
            co_return co_await std::move(c).template choose<1>();
        });
        // c: Chan<Choose<Done, Done>, ...>
        auto c2 = co_await std::move(c).template choose<0>();
        std::move(c2).close();

        // Server drives the dual sub-protocol loop, then offers on continuation.
        auto s = co_await std::move(server).call(
            [](auto dep_chan) -> asio::awaitable<Chan<Done, InProcessTx, InProcessRx>>
        {
            using ServerLoop = Dual_t<DepLoop>;
            using LoopChan = Chan<ServerLoop, InProcessTx, InProcessRx>;
            using DoneChan = Chan<Done, InProcessTx, InProcessRx>;

            auto s = std::move(dep_chan);
            int count = 0;
            while (true) {
                auto branches = co_await std::move(s).offer();
                auto outcome = co_await std::move(branches).visit(
                    [&](auto chan) -> asio::awaitable<std::variant<LoopChan, DoneChan>> {
                        auto [val, cont] = co_await std::move(chan).recv();
                        EXPECT_EQ(val, count);
                        count++;
                        co_return std::move(cont);
                    },
                    [](auto chan) -> asio::awaitable<std::variant<LoopChan, DoneChan>> {
                        co_return std::move(chan);
                    }
                );
                if (auto * done = std::get_if<DoneChan>(&outcome)) {
                    EXPECT_EQ(count, 3);
                    co_return std::move(*done);
                }
                s = std::move(std::get<LoopChan>(outcome));
            }
        });
        // s: Chan<Offer<Done, Done>, ...>
        auto branches = co_await std::move(s).offer();
        int branch = co_await std::move(branches).visit(
            [](auto chan) -> asio::awaitable<int> { std::move(chan).close(); co_return 0; },
            [](auto chan) -> asio::awaitable<int> { std::move(chan).close(); co_return 1; }
        );
        EXPECT_EQ(branch, 0);
    });
}

// ---------------------------------------------------------------------------
// Error propagation via exceptions
// ---------------------------------------------------------------------------

namespace {

struct FailTx {
    using transmitter_tag = void;

    template<typename T>
    asio::awaitable<void> transmit(T) {
        throw std::runtime_error("transmit failed");
        co_return; // unreachable, makes this a coroutine
    }
};

struct FailRx {
    using receiver_tag = void;

    template<typename T>
    asio::awaitable<T> receive() {
        (void) co_await asio::this_coro::executor; // no-op, makes this a coroutine
        throw std::runtime_error("receive failed");
    }
};

} // anonymous namespace

TEST(SessionChan, ExceptionErrorPropagation) {
    runAsync([](auto) -> asio::awaitable<void> {
        // Send fails.
        {
            auto chan = makeChan<Send<int, Done>>(FailTx{}, FailRx{});
            try {
                (void) co_await std::move(chan).send(42);
                EXPECT_TRUE(false) << "Expected exception";
            } catch (const std::runtime_error & e) {
                EXPECT_STREQ(e.what(), "transmit failed");
            }
        }

        // Recv fails.
        {
            auto chan = makeChan<Recv<int, Done>>(FailTx{}, FailRx{});
            try {
                co_await std::move(chan).recv();
                EXPECT_TRUE(false) << "Expected exception";
            } catch (const std::runtime_error & e) {
                EXPECT_STREQ(e.what(), "receive failed");
            }
        }

        // Choose fails.
        {
            auto chan = makeChan<Choose<Done, Done>>(FailTx{}, FailRx{});
            try {
                (void) co_await std::move(chan).template choose<0>();
                EXPECT_TRUE(false) << "Expected exception";
            } catch (const std::runtime_error & e) {
                EXPECT_STREQ(e.what(), "transmit failed");
            }
        }

        // Offer fails.
        {
            auto chan = makeChan<Offer<Done, Done>>(FailTx{}, FailRx{});
            try {
                (void) co_await std::move(chan).offer();
                EXPECT_TRUE(false) << "Expected exception";
            } catch (const std::runtime_error & e) {
                EXPECT_STREQ(e.what(), "receive failed");
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Branches is move-only and nodiscard
// ---------------------------------------------------------------------------

TEST(SessionChan, BranchesMoveOnly) {
    using B = Branches<Offer<Done, Done>, InProcessTx, InProcessRx>;
    static_assert(!std::is_copy_constructible_v<B>);
    static_assert(std::is_move_constructible_v<B>);
    static_assert(!std::is_copy_assignable_v<B>);
    static_assert(!std::is_move_assignable_v<B>);
}

// ---------------------------------------------------------------------------
// Offer with invalid choice index throws exception
// ---------------------------------------------------------------------------

TEST(SessionChan, OfferInvalidChoice) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        // Manually construct a queue with an out-of-range Choice.
        auto rx_queue = std::make_shared<detail::AsyncQueueState>(exec);
        rx_queue->data.push(std::any(Choice{99})); // 99 >= BranchCount<Offer<Done, Done>> (2)

        auto tx_queue = std::make_shared<detail::AsyncQueueState>(exec);
        auto chan = makeChan<Offer<Done, Done>>(
            InProcessTx{tx_queue}, InProcessRx{rx_queue});

        try {
            (void) co_await std::move(chan).offer();
            EXPECT_TRUE(false) << "Expected exception";
        } catch (const std::runtime_error & e) {
            EXPECT_STREQ(e.what(), "invalid branch index in offer");
        }
    });
}

// ---------------------------------------------------------------------------
// Concept satisfaction: InProcess backend satisfies Transmitter/Receiver
// ---------------------------------------------------------------------------

TEST(SessionChan, ConceptSatisfaction) {
    static_assert(Transmitter<InProcessTx>);
    static_assert(Receiver<InProcessRx>);
    static_assert(CanTransmit<InProcessTx, int>);
    static_assert(CanTransmit<InProcessTx, std::string>);
    static_assert(CanTransmit<InProcessTx, Choice>);
    static_assert(CanReceive<InProcessRx, int>);
    static_assert(CanReceive<InProcessRx, std::string>);
    static_assert(CanReceive<InProcessRx, Choice>);

    // Fail backends also satisfy the base concepts.
    static_assert(Transmitter<FailTx>);
    static_assert(Receiver<FailRx>);
}

// ---------------------------------------------------------------------------
// Linearity enforcement: unconditional abort on protocol violation.
// These are death tests — they verify that Linear<>'s linearity checks
// fire in all build modes (not just debug), catching logic bugs that
// the type system cannot prevent (named variables dropped without use).
//
// Note: type aliases and pair member access (not structured bindings)
// are used to avoid commas inside the ASSERT_DEATH macro argument,
// which the preprocessor would misparse as argument separators.
// ---------------------------------------------------------------------------

TEST(SessionChanDeathTest, ChanDroppedWithoutConsuming) {
    ASSERT_DEATH(
        {
            asio::io_context ctx;
            auto pair = channel<Done>(ctx.get_executor());
            (void) pair;
        },
        "Chan");
}

TEST(SessionChanDeathTest, BranchesDroppedWithoutVisit) {
    using Proto = Choose<Done, Done>;
    ASSERT_DEATH(
        {
            asio::io_context ctx;
            auto exec = ctx.get_executor();
            auto pair = channel<Proto>(exec);
            auto client = std::move(pair.first);
            auto server = std::move(pair.second);
            // Run the async ops to completion to get branches.
            asio::co_spawn(ctx, [](auto client, auto server) -> asio::awaitable<void> {
                auto c = co_await std::move(client).template choose<0>();
                std::move(c).close();
                auto branches = co_await std::move(server).offer();
                (void) branches;
            }(std::move(client), std::move(server)), [](std::exception_ptr) {});
            ctx.run();
        },
        "Branches");
}

TEST(SessionChanDeathTest, MoveAssignOverUnconsumed) {
    ASSERT_DEATH(
        {
            asio::io_context ctx;
            auto exec = ctx.get_executor();
            auto p1 = channel<Done>(exec);
            auto p2 = channel<Done>(exec);
            std::move(p1.second).close();
            std::move(p2.second).close();
            p1.first = std::move(p2.first);
        },
        "reassigning over unconsumed value");
}

// ---------------------------------------------------------------------------
// New test: BidirectionalCall — previously impossible with sync InProcess.
// Client sends int, receives string back within one Call.
// Tests that recv() suspends and resumes when peer sends.
// ---------------------------------------------------------------------------

TEST(SessionChan, BidirectionalCall) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        // Sub-protocol is bidirectional: client sends int, then receives string.
        using Sub = Send<int, Recv<std::string, Done>>;
        using Proto = Call<Sub, Done>;
        auto [client, server] = channel<Proto>(exec);

        co_await concurrently(
            // Client side of the Call.
            [](auto client) -> asio::awaitable<void> {
                auto c = co_await std::move(client).call(
                    [](auto sub) -> asio::awaitable<Chan<Done, InProcessTx, InProcessRx>> {
                        auto c1 = co_await std::move(sub).send(42);
                        auto [reply, done] = co_await std::move(c1).recv();
                        EXPECT_EQ(reply, "forty-two");
                        co_return std::move(done);
                    });
                std::move(c).close();
            }(std::move(client)),
            // Server side of the Call (dual sub-protocol: recv int, send string).
            [](auto server) -> asio::awaitable<void> {
                auto s = co_await std::move(server).call(
                    [](auto sub) -> asio::awaitable<Chan<Done, InProcessTx, InProcessRx>> {
                        auto [val, s1] = co_await std::move(sub).recv();
                        EXPECT_EQ(val, 42);
                        auto done = co_await std::move(s1).send(std::string("forty-two"));
                        co_return std::move(done);
                    });
                std::move(s).close();
            }(std::move(server))
        );
    });
}

// ---------------------------------------------------------------------------
// New test: ConcurrentPingPong — both endpoints interleave send/recv.
// Tests natural coroutine interleaving on a single strand.
// ---------------------------------------------------------------------------

TEST(SessionChan, ConcurrentPingPong) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        using Proto = Send<int, Recv<int, Send<int, Recv<int, Done>>>>;
        auto [client, server] = channel<Proto>(exec);

        co_await concurrently(
            [](auto client) -> asio::awaitable<void> {
                auto c1 = co_await std::move(client).send(1);
                auto [v1, c2] = co_await std::move(c1).recv();
                EXPECT_EQ(v1, 2);
                auto c3 = co_await std::move(c2).send(3);
                auto [v2, c4] = co_await std::move(c3).recv();
                EXPECT_EQ(v2, 4);
                std::move(c4).close();
            }(std::move(client)),
            [](auto server) -> asio::awaitable<void> {
                auto [v1, s1] = co_await std::move(server).recv();
                EXPECT_EQ(v1, 1);
                auto s2 = co_await std::move(s1).send(2);
                auto [v2, s3] = co_await std::move(s2).recv();
                EXPECT_EQ(v2, 3);
                auto s4 = co_await std::move(s3).send(4);
                std::move(s4).close();
            }(std::move(server))
        );
    });
}

// ---------------------------------------------------------------------------
// New test: ConcurrentLoop — bidirectional loop with concurrent endpoints.
// Each iteration both sends and receives. Tests that the timer-condvar
// mechanism works across multiple suspend/resume cycles.
// ---------------------------------------------------------------------------

TEST(SessionChan, ConcurrentLoop) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        using Proto = Loop<Choose<Send<int, Recv<int, Continue<0>>>, Done>>;
        auto [client, server] = channel<Proto>(exec);

        co_await concurrently(
            [](auto client) -> asio::awaitable<void> {
                auto c = std::move(client);
                for (int i = 0; i < 3; i++) {
                    auto c1 = co_await std::move(c).template choose<0>();
                    auto c2 = co_await std::move(c1).send(i);
                    auto [reply, c3] = co_await std::move(c2).recv();
                    EXPECT_EQ(reply, i * 100);
                    c = std::move(c3);
                }
                auto done = co_await std::move(c).template choose<1>();
                std::move(done).close();
            }(std::move(client)),
            [](auto server) -> asio::awaitable<void> {
                using ServerProto = Dual_t<Loop<Choose<Send<int, Recv<int, Continue<0>>>, Done>>>;
                auto s = std::move(server);
                bool running = true;
                while (running) {
                    auto branches = co_await std::move(s).offer();
                    auto next = co_await std::move(branches).visit(
                        [](auto chan) -> asio::awaitable<std::optional<Chan<ServerProto, InProcessTx, InProcessRx>>> {
                            auto [val, s1] = co_await std::move(chan).recv();
                            auto s2 = co_await std::move(s1).send(val * 100);
                            co_return std::move(s2);
                        },
                        [&running](auto chan) -> asio::awaitable<std::optional<Chan<ServerProto, InProcessTx, InProcessRx>>> {
                            std::move(chan).close();
                            running = false;
                            co_return std::nullopt;
                        }
                    );
                    if (next) s = std::move(*next);
                }
            }(std::move(server))
        );
    });
}

// ---------------------------------------------------------------------------
// Split: concurrent tx-only and rx-only halves
// ---------------------------------------------------------------------------

TEST(SessionChan, SplitSendRecv) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        // P = Send<int, Done> (tx half sends int)
        // Q = Recv<std::string, Done> (rx half receives string)
        // R = Done (continuation)
        using Proto = Split<Send<int, Done>, Recv<std::string, Done>, Done>;
        auto [client, server] = channel<Proto>(exec);

        co_await concurrently(
            // Client: split, send int on tx half, recv string on rx half.
            [](auto client) -> asio::awaitable<void> {
                auto c = co_await std::move(client).split(
                    [](auto tx_chan, auto rx_chan)
                        -> asio::awaitable<std::pair<
                               Chan<Done, InProcessTx, Unavailable>,
                               Chan<Done, Unavailable, InProcessRx>>>
                    {
                        auto tx_done = co_await std::move(tx_chan).send(42);
                        auto [str, rx_done] = co_await std::move(rx_chan).recv();
                        EXPECT_EQ(str, "hello");
                        co_return std::pair(std::move(tx_done), std::move(rx_done));
                    });
                std::move(c).close();
            }(std::move(client)),
            // Server (dual): Split<Dual<Q>, Dual<P>, Dual<R>>
            //   = Split<Send<string, Done>, Recv<int, Done>, Done>
            [](auto server) -> asio::awaitable<void> {
                auto s = co_await std::move(server).split(
                    [](auto tx_chan, auto rx_chan)
                        -> asio::awaitable<std::pair<
                               Chan<Done, InProcessTx, Unavailable>,
                               Chan<Done, Unavailable, InProcessRx>>>
                    {
                        auto tx_done = co_await std::move(tx_chan).send(std::string("hello"));
                        auto [val, rx_done] = co_await std::move(rx_chan).recv();
                        EXPECT_EQ(val, 42);
                        co_return std::pair(std::move(tx_done), std::move(rx_done));
                    });
                std::move(s).close();
            }(std::move(server))
        );
    });
}

// ---------------------------------------------------------------------------
// Split with continuation: after split completes, protocol continues
// ---------------------------------------------------------------------------

TEST(SessionChan, SplitWithContinuation) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        // After split, client sends a final int.
        using Proto = Split<Send<int, Done>, Recv<std::string, Done>,
                            Send<int, Done>>;
        auto [client, server] = channel<Proto>(exec);

        co_await concurrently(
            [](auto client) -> asio::awaitable<void> {
                auto c = co_await std::move(client).split(
                    [](auto tx_chan, auto rx_chan)
                        -> asio::awaitable<std::pair<
                               Chan<Done, InProcessTx, Unavailable>,
                               Chan<Done, Unavailable, InProcessRx>>>
                    {
                        auto tx_done = co_await std::move(tx_chan).send(1);
                        auto [str, rx_done] = co_await std::move(rx_chan).recv();
                        EXPECT_EQ(str, "mid");
                        co_return std::pair(std::move(tx_done), std::move(rx_done));
                    });
                // Continuation: send final int.
                auto c2 = co_await std::move(c).send(99);
                std::move(c2).close();
            }(std::move(client)),
            [](auto server) -> asio::awaitable<void> {
                auto s = co_await std::move(server).split(
                    [](auto tx_chan, auto rx_chan)
                        -> asio::awaitable<std::pair<
                               Chan<Done, InProcessTx, Unavailable>,
                               Chan<Done, Unavailable, InProcessRx>>>
                    {
                        auto tx_done = co_await std::move(tx_chan).send(std::string("mid"));
                        auto [val, rx_done] = co_await std::move(rx_chan).recv();
                        EXPECT_EQ(val, 1);
                        co_return std::pair(std::move(tx_done), std::move(rx_done));
                    });
                // Continuation: recv final int.
                auto [val, s2] = co_await std::move(s).recv();
                EXPECT_EQ(val, 99);
                std::move(s2).close();
            }(std::move(server))
        );
    });
}

// ---------------------------------------------------------------------------
// Split with truly concurrent halves: tx and rx co_spawned independently.
// Both endpoints simultaneously send and receive multiple values.
// ---------------------------------------------------------------------------

TEST(SessionChan, SplitConcurrentHalves) {
    runAsync([](auto exec) -> asio::awaitable<void> {
        // Client sends 3 ints (tx) while simultaneously receiving 3 strings (rx).
        using Proto = Split<
            Send<int, Send<int, Send<int, Done>>>,
            Recv<std::string, Recv<std::string, Recv<std::string, Done>>>,
            Done>;
        auto [client, server] = channel<Proto>(exec);

        co_await concurrently(
            [](auto client) -> asio::awaitable<void> {
                auto c = co_await std::move(client).split(
                    [](auto tx_chan, auto rx_chan)
                        -> asio::awaitable<std::pair<
                               Chan<Done, InProcessTx, Unavailable>,
                               Chan<Done, Unavailable, InProcessRx>>>
                    {
                        // Run both halves concurrently via concurrently().
                        using TxDone = Chan<Done, InProcessTx, Unavailable>;
                        using RxDone = Chan<Done, Unavailable, InProcessRx>;
                        std::optional<TxDone> tx_result;
                        std::optional<RxDone> rx_result;

                        co_await concurrently(
                            [&tx_result](auto tx) -> asio::awaitable<void> {
                                auto t1 = co_await std::move(tx).send(10);
                                auto t2 = co_await std::move(t1).send(20);
                                auto t3 = co_await std::move(t2).send(30);
                                tx_result.emplace(std::move(t3));
                            }(std::move(tx_chan)),
                            [&rx_result](auto rx) -> asio::awaitable<void> {
                                auto [s1, r1] = co_await std::move(rx).recv();
                                EXPECT_EQ(s1, "a");
                                auto [s2, r2] = co_await std::move(r1).recv();
                                EXPECT_EQ(s2, "b");
                                auto [s3, r3] = co_await std::move(r2).recv();
                                EXPECT_EQ(s3, "c");
                                rx_result.emplace(std::move(r3));
                            }(std::move(rx_chan))
                        );

                        co_return std::pair(std::move(*tx_result), std::move(*rx_result));
                    });
                std::move(c).close();
            }(std::move(client)),
            // Server dual: Split<Send<string×3, Done>, Recv<int×3, Done>, Done>
            [](auto server) -> asio::awaitable<void> {
                auto s = co_await std::move(server).split(
                    [](auto tx_chan, auto rx_chan)
                        -> asio::awaitable<std::pair<
                               Chan<Done, InProcessTx, Unavailable>,
                               Chan<Done, Unavailable, InProcessRx>>>
                    {
                        using TxDone = Chan<Done, InProcessTx, Unavailable>;
                        using RxDone = Chan<Done, Unavailable, InProcessRx>;
                        std::optional<TxDone> tx_result;
                        std::optional<RxDone> rx_result;

                        co_await concurrently(
                            [&tx_result](auto tx) -> asio::awaitable<void> {
                                auto t1 = co_await std::move(tx).send(std::string("a"));
                                auto t2 = co_await std::move(t1).send(std::string("b"));
                                auto t3 = co_await std::move(t2).send(std::string("c"));
                                tx_result.emplace(std::move(t3));
                            }(std::move(tx_chan)),
                            [&rx_result](auto rx) -> asio::awaitable<void> {
                                auto [v1, r1] = co_await std::move(rx).recv();
                                EXPECT_EQ(v1, 10);
                                auto [v2, r2] = co_await std::move(r1).recv();
                                EXPECT_EQ(v2, 20);
                                auto [v3, r3] = co_await std::move(r2).recv();
                                EXPECT_EQ(v3, 30);
                                rx_result.emplace(std::move(r3));
                            }(std::move(rx_chan))
                        );

                        co_return std::pair(std::move(*tx_result), std::move(*rx_result));
                    });
                std::move(s).close();
            }(std::move(server))
        );
    });
}

} // namespace nix::session
