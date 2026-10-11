#include <gtest/gtest.h>
#include <type_traits>
#include <optional>
#include <string>

#include "nix/util/fun.hh"
#include "nix/util/callback.hh"
#include "nix/util/signals.hh"

#include "nix/util/async.hh"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>

namespace nix {

static int add(int a, int b)
{
    return a + b;
}

TEST(fun, constructFromLambda)
{
    fun<int(int)> f = [](int x) { return x * 2; };
    EXPECT_EQ(f(3), 6);
}

TEST(fun, constructFromFunctionPointer)
{
    fun<int(int, int)> f = add;
    EXPECT_EQ(f(2, 3), 5);
}

TEST(fun, constructFromStdFunction)
{
    std::function<int(int)> sf = [](int x) { return x + 1; };
    fun<int(int)> f(sf);
    EXPECT_EQ(f(5), 6);
}

TEST(fun, moveConstructFromStdFunction)
{
    std::function<int(int)> sf = [](int x) { return x + 1; };
    fun<int(int)> f(std::move(sf));
    EXPECT_EQ(f(5), 6);
}

TEST(fun, rejectsEmptyStdFunction)
{
    std::function<int(int)> empty;
    EXPECT_THROW((fun<int(int)>{empty}), std::invalid_argument);
}

TEST(fun, rejectsEmptyStdFunctionMove)
{
    std::function<int(int)> empty;
    EXPECT_THROW((fun<int(int)>{std::move(empty)}), std::invalid_argument);
}

TEST(fun, rejectsNullFunctionPointer)
{
    int (*nullFp)(int) = nullptr;
    EXPECT_THROW((fun<int(int)>{nullFp}), std::invalid_argument);
}

TEST(fun, nullptrDeletedAtCompileTime)
{
    // fun(nullptr) is a deleted constructor
    static_assert(!std::is_constructible_v<fun<void()>, std::nullptr_t>);
}

TEST(fun, notDefaultConstructible)
{
    static_assert(!std::is_default_constructible_v<fun<void()>>);
}

TEST(fun, voidReturn)
{
    int called = 0;
    fun<void()> f = [&]() { called++; };
    f();
    EXPECT_EQ(called, 1);
}

TEST(fun, referenceArgs)
{
    fun<void(int &)> f = [](int & x) { x += 10; };
    int val = 5;
    f(val);
    EXPECT_EQ(val, 15);
}

TEST(fun, convertsToStdFunction)
{
    fun<int(int)> f = [](int x) { return x * 3; };
    std::function<int(int)> sf = f.get_fn();
    EXPECT_EQ(sf(4), 12);
}

TEST(fun, copyable)
{
    fun<int(int)> f = [](int x) { return x + 1; };
    auto g = f;
    EXPECT_EQ(f(1), 2);
    EXPECT_EQ(g(1), 2);
}

TEST(fun, movable)
{
    fun<int(int)> f = [](int x) { return x + 1; };
    auto g = std::move(f);
    EXPECT_EQ(g(1), 2);
}

TEST(fun, capturesState)
{
    int offset = 100;
    fun<int(int)> f = [offset](int x) { return x + offset; };
    EXPECT_EQ(f(5), 105);
}

TEST(fun, getFn)
{
    fun<int(int)> f = [](int x) { return x; };
    const auto & sf = f.get_fn();
    EXPECT_EQ(sf(42), 42);
}

TEST(fun, getFnMove)
{
    fun<int(int)> f = [](int x) { return x; };
    auto sf = std::move(f).get_fn();
    EXPECT_EQ(sf(42), 42);
}

TEST(fun, forwardsMoveOnlyTypes)
{
    fun<int(std::unique_ptr<int>)> f = [](std::unique_ptr<int> p) { return *p; };
    auto p = std::make_unique<int>(42);
    EXPECT_EQ(f(std::move(p)), 42);
}

TEST(fun, perfectForwardingZeroCost)
{
    int copies = 0, moves = 0;

    struct Tracker
    {
        int & copies;
        int & moves;

        Tracker(int & copies, int & moves)
            : copies(copies)
            , moves(moves)
        {
        }

        Tracker(const Tracker & o)
            : copies(o.copies)
            , moves(o.moves)
        {
            copies++;
        }

        Tracker(Tracker && o) noexcept
            : copies(o.copies)
            , moves(o.moves)
        {
            moves++;
        }
    };

    // Baseline: call std::function directly
    std::function<void(Tracker, Tracker)> sf = [](Tracker, Tracker) {};
    Tracker t1{copies, moves}, t2{copies, moves};
    sf(t1, t2);
    int baseline_copies = copies, baseline_moves = moves;

    copies = 0;
    moves = 0;

    // Call through fun<> — should match baseline exactly
    fun<void(Tracker, Tracker)> f = [](Tracker, Tracker) {};
    f(t1, t2);
    EXPECT_EQ(copies, baseline_copies);
    EXPECT_EQ(moves, baseline_moves);
}

TEST(Callback, deliversOwnedValuesAndOriginalExceptions)
{
    using Value = std::optional<std::string>;
    using Result = Callback<Value>::Result;
    unsigned cells = 0;
    for (bool moved : {false, true}) {
        for (const auto & expected : {Value{}, Value{std::string{}}, Value{std::string("a\0b", 3)}}) {
            SCOPED_TRACE(fmt("moved=%d cell=%d", moved, cells));
            std::optional<Result> received;
            unsigned calls = 0;
            Callback<Value> callback{[&](Result result) {
                ++calls;
                received.emplace(std::move(result));
            }};
            if (moved) {
                auto destination = std::move(callback);
                destination(Value{expected});
            } else
                callback(Value{expected});
            EXPECT_EQ(calls, 1u);
            ASSERT_TRUE(received);
            EXPECT_EQ(std::move(*received).value(), expected);
            ++cells;
        }
        for (auto exception : {
                 std::make_exception_ptr(std::runtime_error("callback error")),
                 std::make_exception_ptr(Interrupted("callback interruption"))})
            for (bool explicitException : {false, true}) {
                SCOPED_TRACE(fmt("moved=%d explicit=%d cell=%d", moved, explicitException, cells));
                std::optional<Result> received;
                unsigned calls = 0;
                Callback<Value> callback{[&](Result result) {
                    ++calls;
                    received.emplace(std::move(result));
                }};
                auto deliver = [&](Callback<Value> & target) {
                    if (explicitException)
                        target.rethrow(exception);
                    else {
                        try {
                            std::rethrow_exception(exception);
                        } catch (...) {
                            target.rethrow();
                        }
                    }
                };
                if (moved) {
                    auto destination = std::move(callback);
                    deliver(destination);
                } else
                    deliver(callback);
                EXPECT_EQ(calls, 1u);
                ASSERT_TRUE(received);
                try {
                    std::move(*received).value();
                    ADD_FAILURE() << "callback lost its exception";
                } catch (...) {
                    EXPECT_EQ(std::current_exception(), exception);
                }
                ++cells;
            }
    }
    EXPECT_EQ(cells, 14u);
    RecordProperty("finite_cases", cells);
}

TEST(Callback, borrowingAvoidsAnExtraPayloadMove)
{
    struct Value
    {
        unsigned & moves;
        int payload = 42;
        explicit Value(unsigned & moves)
            : moves(moves)
        {
        }
        Value(const Value &) = delete;
        Value(Value && other) noexcept
            : moves(other.moves)
            , payload(other.payload)
        {
            ++moves;
        }
    };
    using Result = Callback<Value>::Result;
    unsigned cells = 0;
    for (bool moved : {false, true})
        for (bool error : {false, true}) {
            unsigned entered[2]{};
            for (bool ownsParameter : {false, true}) {
                SCOPED_TRACE(fmt("moved=%d error=%d owned=%d", moved, error, ownsParameter));
                unsigned moves = 0, calls = 0;
                auto exception = std::make_exception_ptr(std::runtime_error("borrowed error"));
                auto observe = [&](Result && result) {
                    entered[ownsParameter] = moves;
                    ++calls;
                    if (error) {
                        try {
                            std::move(result).value();
                            ADD_FAILURE() << "callback lost its exception";
                        } catch (...) {
                            EXPECT_EQ(std::current_exception(), exception);
                        }
                    } else
                        EXPECT_EQ(std::move(result).value().payload, 42);
                };
                Callback<Value> callback{ownsParameter
                    ? fun<void(Result &&)>{[&](Result result) { observe(std::move(result)); }}
                    : fun<void(Result &&)>{observe}};
                auto deliver = [&](Callback<Value> & target) {
                    if (error)
                        target.rethrow(exception);
                    else
                        target(Value{moves});
                };
                if (moved) {
                    auto destination = std::move(callback);
                    deliver(destination);
                } else
                    deliver(callback);
                EXPECT_EQ(calls, 1u);
                ++cells;
            }
            EXPECT_EQ(entered[1], entered[0] + (error ? 0u : 1u));
        }
    EXPECT_EQ(cells, 8u);
    RecordProperty("finite_cases", cells);
}

TEST(CallbackToAwaitable, completionAndCancellationPreserveFirstOutcome)
{
    using Result = Callback<int>::Result;
    unsigned cells = 0;
    for (bool cancelFirst : {false, true})
        for (bool producerError : {false, true}) {
            SCOPED_TRACE(fmt("cancelFirst=%d producerError=%d", cancelFirst, producerError));
            asio::io_context context;
            asio::cancellation_signal cancellation;
            std::optional<Callback<int>> pending;
            std::optional<Result> received;
            unsigned deliveries = 0;
            auto exception = std::make_exception_ptr(std::runtime_error("producer failure"));
            auto token = asio::bind_executor(asio::any_io_executor(context.get_executor()),
                asio::bind_cancellation_slot(cancellation.slot(), [&](Result result) {
                    ++deliveries;
                    received.emplace(std::move(result));
                }));
            callbackToAwaitable<int>(
                [&](Callback<int> callback) { pending.emplace(std::move(callback)); }, token);
            ASSERT_TRUE(pending);
            if (cancelFirst) {
                cancellation.emit(asio::cancellation_type::all);
                cancellation.emit(asio::cancellation_type::all);
            }
            if (producerError)
                pending->rethrow(exception);
            else
                (*pending)(42);
            pending.reset();
            if (!cancelFirst) {
                cancellation.emit(asio::cancellation_type::all);
                cancellation.emit(asio::cancellation_type::all);
            }
            EXPECT_EQ(deliveries, 0u); // Completion is marshalled onto the executor.
            context.run();
            EXPECT_EQ(deliveries, 1u);
            ASSERT_TRUE(received);
            if (cancelFirst)
                EXPECT_THROW(std::move(*received).value(), Interrupted);
            else if (producerError) {
                try {
                    std::move(*received).value();
                    ADD_FAILURE() << "producer exception was lost";
                } catch (...) {
                    EXPECT_EQ(std::current_exception(), exception);
                }
            } else
                EXPECT_EQ(std::move(*received).value(), 42);
            ++cells;
        }
    EXPECT_EQ(cells, 4u);
    RecordProperty("finite_cases", cells);
}

} // namespace nix
