#include "nix/util/serialise.hh"
#include "nix/util/config.hh"

#include <boost/context/detail/exception.hpp>
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <algorithm>
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

TEST(Source, drainDistinguishesProducerCompletionFromConsumerFailure)
{
    struct Failure
    {};

    /* Completion belongs to the producer; any consumer exception is a
       failed delivery, even when its type is EndOfFile. */
    for (bool producer : {false, true})
        for (bool eof : {false, true})
            for (size_t delivered : {0u, 1u}) {
                SCOPED_TRACE((::testing::Message() << producer << ':' << eof << ':' << delivered));
                size_t reads = 0, writes = 0;
                std::string received;
                auto fail = [&] {
                    if (eof)
                        throw EndOfFile("selected stream boundary");
                    throw Failure{};
                };
                LambdaSource source{[&](char * data, size_t) {
                    auto call = reads++;
                    if (producer && call == delivered)
                        fail();
                    data[0] = 'x';
                    return size_t{1};
                }};
                LambdaSink sink{[&](std::string_view data) {
                    auto call = writes++;
                    if (!producer && call == delivered)
                        fail();
                    received += data;
                }};

                if (producer && eof) {
                    EXPECT_NO_THROW(source.drainInto(sink));
                } else if (eof) {
                    EXPECT_THROW(source.drainInto(sink), EndOfFile);
                } else {
                    EXPECT_THROW(source.drainInto(sink), Failure);
                }
                EXPECT_EQ(received, std::string(delivered, 'x'));
                EXPECT_EQ(reads, delivered + 1);
                EXPECT_EQ(writes, delivered + (producer ? 0 : 1));
            }
}

TEST(Source, drainPreservesAnEmptyDeliveryBeforeProducerCompletion)
{
    size_t reads = 0;
    LambdaSource source{[&](char * data, size_t) -> size_t {
        if (reads++ == 0)
            return 0;
        if (reads == 2) {
            data[0] = 'x';
            return 1;
        }
        throw EndOfFile("producer complete");
    }};
    std::vector<std::string> received;
    LambdaSink sink{[&](std::string_view data) { received.emplace_back(data); }};
    EXPECT_NO_THROW(source.drainInto(sink));
    EXPECT_EQ(received, (std::vector<std::string>{"", "x"}));
    EXPECT_EQ(reads, 3u);
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

TEST(readPrefix, finiteProducerProtocol)
{
    enum class End { Normal, Eof, Failure };
    struct Failure { int tag; };
    for (size_t limit : {0u, 1u, 7u})
        for (size_t available : {limit ? limit - 1 : 0, limit, limit + 1})
            for (size_t chunk : {1u, 7u})
                for (auto end : {End::Normal, End::Eof, End::Failure}) {
                    SCOPED_TRACE((::testing::Message() << limit << ':' << available << ':' << chunk << ':' << int(end)));
                    size_t consumed = 0, reads = 0;
                    auto read = [&](char * data, size_t requested) -> std::optional<size_t> {
                        if (reads++ == 0)
                            return 0; // An engaged zero delivery is not completion.
                        if (consumed == available) {
                            if (end == End::Normal)
                                return std::nullopt;
                            if (end == End::Eof)
                                throw EndOfFile("prefix boundary");
                            throw Failure{42};
                        }
                        auto n = std::min({requested, chunk, available - consumed});
                        std::fill_n(data, n, 'x');
                        consumed += n;
                        return n;
                    };
                    std::string result = "unchanged";
                    bool complete = available >= limit || end == End::Normal;
                    if (complete)
                        EXPECT_NO_THROW(result = readPrefix(limit, read));
                    else
                        try {
                            result = readPrefix(limit, read);
                            FAIL() << "missing producer failure";
                        } catch (EndOfFile & error) {
                            EXPECT_EQ(end, End::Eof);
                            EXPECT_EQ(error.message(), "prefix boundary");
                        } catch (Failure & error) {
                            EXPECT_EQ(end, End::Failure);
                            EXPECT_EQ(error.tag, 42);
                        }
                    EXPECT_EQ(consumed, std::min(available, limit));
                    EXPECT_EQ(result, complete ? std::string(consumed, 'x') : "unchanged");
                    if (!limit)
                        EXPECT_EQ(reads, 0u);
                }
}

TEST(readPrefix, binaryGrowthBoundaries)
{
    for (size_t limit : {65535u, 65536u, 65537u, 131071u, 131072u, 131073u}) {
        std::string bytes(limit + 1, '\0');
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<char>(i % 256);
        for (size_t available : {limit - 1, limit, limit + 1})
            for (size_t chunk : {size_t{32768}, std::numeric_limits<size_t>::max()})
                for (bool firstByte : {false, true}) {
                    SCOPED_TRACE((::testing::Message() << limit << ':' << available << ':' << chunk << ':' << firstByte));
                    size_t consumed = 0;
                    auto read = [&](char * data, size_t requested) -> std::optional<size_t> {
                        if (consumed == available)
                            return std::nullopt;
                        auto delivery = firstByte && !consumed ? size_t{1} : chunk;
                        auto n = bytes.copy(data, std::min({requested, delivery, available - consumed}), consumed);
                        consumed += n;
                        return n;
                    };
                    EXPECT_EQ(readPrefix(limit, read), bytes.substr(0, std::min(available, limit)));
                    EXPECT_EQ(consumed, std::min(available, limit));
                }
        StringSink encoded;
        encoded << std::string_view(bytes);
        StringSource framed{encoded.s};
        EXPECT_EQ(readString(framed), bytes);
    }
}

TEST(readString, rejectsLimitsTruncationAndBadPadding)
{
    StringSink encoded;
    encoded << "abc";
    for (size_t max : {2u, 3u}) {
        StringSource source{encoded.s};
        if (max < 3)
            EXPECT_THROW(readString(source, max), SerialisationError);
        else
            EXPECT_EQ(readString(source, max), "abc");
    }
    for (size_t end : {8u, 10u, 11u, 15u}) {
        StringSource truncated{encoded.s.substr(0, end)};
        EXPECT_THROW(readString(truncated), EndOfFile);
    }
    encoded.s.back() = 1;
    StringSource badPadding{encoded.s};
    EXPECT_THROW(readString(badPadding), SerialisationError);
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
