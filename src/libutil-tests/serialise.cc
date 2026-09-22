#include "nix/util/serialise.hh"
#include "nix/util/config.hh"

#include <boost/context/detail/exception.hpp>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <array>
#include <limits>
#include <utility>

namespace nix {

template<typename T>
    requires std::is_integral_v<T>
auto makeNumSource(T num)
{
    return sinkToSource([num](Sink & writer) { writer << num; });
}

TEST(BufferedSink, finiteChunkSequencesPreserveWriteBoundaries)
{
    struct Failure
    {};

    struct Record
    {
        std::vector<std::string> calls;
        std::string bytes;
        size_t failAt = 0;

        void write(std::string_view data)
        {
            calls.emplace_back(data);
            if (calls.size() == failAt)
                throw Failure{};
            bytes += data;
        }
    };

    struct Observed : BufferedSink
    {
        Record record;
        using BufferedSink::BufferedSink;

        void writeUnbuffered(std::string_view data) override
        {
            record.write(data);
        }
    };

    for (size_t capacity = 0; capacity <= 4; ++capacity) {
        /* Every reachable pending length, plus an allocated but empty
           buffer where the capacity admits a buffered write. */
        auto states = std::max(capacity, size_t{1}) + (capacity > 1);
        for (size_t state = 0; state < states; ++state)
            for (size_t length = 0, combinations = 1; length <= 3; ++length, combinations *= 4)
                for (size_t sequence = 0; sequence < combinations; ++sequence) {
                    std::vector<std::string> chunks;
                    const std::array<size_t, 4> sizes{0, 1, capacity, capacity + 1};
                    for (size_t i = 0, code = sequence; i < length; ++i, code /= 4)
                        chunks.emplace_back(sizes[code % 4], static_cast<char>(i));

                    /* At most two callbacks per chunk and one final
                       flush. Inject a failure at every possible call. */
                    for (size_t failAt = 0; failAt <= 2 * length + 1; ++failAt) {
                        SCOPED_TRACE(
                            (::testing::Message()
                             << capacity << ':' << state << ':' << length << ':' << sequence << ':' << failAt));
                        Observed actual{capacity};
                        Record expected;
                        if (capacity > 1 && state == capacity) {
                            actual("p");
                            actual.flush();
                            actual.record = {};
                        }
                        std::string pending(state < capacity ? state : 0, 'p');
                        actual(pending);
                        actual.record.failAt = expected.failAt = failAt;
                        auto flushExpected = [&] {
                            if (!pending.empty()) {
                                auto bytes = std::exchange(pending, {});
                                expected.write(bytes);
                            }
                        };
                        auto feedExpected = [&](std::string_view chunk) {
                            if (chunk.empty())
                                return;
                            /* Buffering retains a chunk only while the
                               accumulated bytes remain below capacity. */
                            if (pending.size() + chunk.size() < capacity)
                                pending += chunk;
                            else {
                                flushExpected();
                                expected.write(chunk);
                            }
                        };
                        auto run = [&](auto feed, auto flush) {
                            size_t operation = 0;
                            try {
                                for (auto & chunk : chunks) {
                                    feed(chunk);
                                    ++operation;
                                }
                                flush();
                                ++operation;
                            } catch (Failure &) {
                            }
                            return operation;
                        };
                        EXPECT_EQ(
                            run([&](auto chunk) { actual(chunk); }, [&] { actual.flush(); }),
                            run(feedExpected, flushExpected));
                        EXPECT_EQ(actual.bufPos, pending.size());
                        EXPECT_EQ(actual.hasData(), !pending.empty());
                        EXPECT_EQ(actual.record.calls, expected.calls);
                        EXPECT_EQ(actual.record.bytes, expected.bytes);
                        /* A failed flush has consumed its pending bytes;
                           recovery must neither repeat nor lose others. */
                        actual.record.failAt = expected.failAt = 0;
                        actual.flush();
                        flushExpected();
                        EXPECT_EQ(actual.record.calls, expected.calls);
                        EXPECT_EQ(actual.record.bytes, expected.bytes);
                    }
                }
    }
}

TEST(readNum, negativeValuesSerialiseWellDefined)
{
    EXPECT_THROW(readNum<uint32_t>(*makeNumSource(int64_t(-1))), SerialisationError);
    EXPECT_THROW(readNum<int64_t>(*makeNumSource(int16_t(-1))), SerialisationError);
    EXPECT_EQ(readNum<uint64_t>(*makeNumSource(int64_t(-1))), std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(readNum<uint64_t>(*makeNumSource(int64_t(-2))), std::numeric_limits<uint64_t>::max() - 1);
    /* The result doesn't depend on the source type - only the destination matters. */
    EXPECT_EQ(readNum<uint64_t>(*makeNumSource(int32_t(-1))), std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(readNum<uint64_t>(*makeNumSource(int16_t(-1))), std::numeric_limits<uint64_t>::max());
}

TEST(readString, roundTrip)
{
    for (auto & s : std::vector<std::string>{"", "x", "1234567", "12345678", "123456789", std::string(300'000, 'a')}) {
        StringSink sink;
        sink << s;
        StringSource source(sink.s);
        EXPECT_EQ(readString(source), s);
    }
}

TEST(readString, bogusLengthDoesNotPreallocate)
{
    // 1 TiB length prefix followed by EOF: must fail on the read, not by
    // allocating (and zeroing) the announced size up front.
    StringSink sink;
    sink << (uint64_t(1) << 40);
    StringSource source(sink.s);
    EXPECT_THROW(readString(source), EndOfFile);
}

TEST(readError, bogusLevelIsClamped)
{
    for (uint64_t level : {
             uint64_t(8),
             uint64_t(1234),
             uint64_t(std::numeric_limits<unsigned>::max()),
         }) {
        StringSink sink;
        sink << "Error" << level << "Error" << "oops" << uint64_t(0) << uint64_t(0);
        StringSource source(sink.s);
        auto e = readError(source);
        EXPECT_THAT(std::string(e.what()), ::testing::HasSubstr("oops"));
        EXPECT_EQ(e.info().level, Verbosity::lvlVomit);
    }
}

TEST(readError, uint64LevelError)
{
    StringSink sink;
    sink << "Error" << std::numeric_limits<uint64_t>::max() << "Error" << "oops" << uint64_t(0) << uint64_t(0);
    StringSource source(sink.s);
    EXPECT_THROW(readError(source), SerialisationError);
}

TEST(readPadding, works)
{
    for (unsigned i = 0; i < 8; ++i)
        EXPECT_NO_THROW(readPadding(i, *makeNumSource(0)));

    for (unsigned len = 1; len < 8; ++len) {
        EXPECT_THROW(readPadding(len, *makeNumSource(~uint64_t(0))), SerialisationError);
        unsigned padLen = 8 - len;
        for (unsigned byte = 0; byte < padLen; ++byte) {
            uint64_t val = uint64_t{1} << (8 * byte);
            EXPECT_THROW(readPadding(len, *makeNumSource(val)), SerialisationError);
        }
    }
}

// The following test catches the bug only under fcontext backend,
// but Boost.Coroutine2 stack switching when abandoning confuses the
// hell out of ASan. The workaround to getting ASan working isn't
// immediately useful because it works only with ucontext implementation.
// https://www.boost.org/doc/libs/1_89_0/libs/coroutine2/doc/html/coroutine2/stack/sanitizers.html
#if !NIX_ASAN_ENABLED

TEST(sourceToSink, forcedUnwindUcaughtExceptions)
{
    int uncaughtExceptions = 42;
    bool caught = false;

    auto sink = sourceToSink([&](Source & source) {
        auto recordUncaughtExceptions = Finally([&]() { uncaughtExceptions = std::uncaught_exceptions(); });
        try {
            StringSink s;
            source.drainInto(s, 8);
            source.drainInto(s, 8);
        } catch (const boost::context::detail::forced_unwind &) {
            caught = true;
            throw;
        }
    });

    *sink << 42;

    // Abandon the coroutine. This will trigger it to unwind with boost::context::detail::forced_unwind.
    sink.reset();

    ASSERT_TRUE(caught);
    // This is a test for boost.context regression fixed by https://github.com/boostorg/context/pull/337.
    // Without the fix std::uncaught_exceptions() *misreports* 0 while there's stack unwinding in progress.
    // The issue only surfaces with libstdc++ and fcontext when boost.context
    // uses fiber-specific exception states and messes with __cxa_get_globals().
    ASSERT_EQ(uncaughtExceptions, 1);
}

#endif

} // namespace nix
