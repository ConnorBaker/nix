#include "nix/util/string-intern-table.hh"
#include "nix/util/tagged.hh"

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nix {

// Local Tagged instantiations for testing StringInternTable's typed API.
// The actual eval-trace ID types live in libexpr; these are equivalent
// test-only substitutes so libutil-tests doesn't depend on libexpr.
struct TestTagA {};
struct TestTagB {};
struct TestTagC {};
using TestIdA = Tagged<TestTagA, uint32_t>;
using TestIdB = Tagged<TestTagB, uint32_t>;
using TestIdC = Tagged<TestTagC, uint32_t>;

// ── Basic intern/resolve ──────────────────────────────────────────────

TEST(StringInternTable, SentinelAtIndexZero)
{
    StringInternTable table;
    // Index 0 is the sentinel (empty string_view)
    EXPECT_EQ(table.resolveRaw(0), "");
    EXPECT_EQ(table.size(), 1u); // sentinel only
    EXPECT_EQ(table.nextId(), 1u);
}

TEST(StringInternTable, InternReturnsOneBasedIds)
{
    StringInternTable table;
    auto id1 = table.internRaw("hello");
    auto id2 = table.internRaw("world");
    EXPECT_EQ(id1, 1u);
    EXPECT_EQ(id2, 2u);
}

TEST(StringInternTable, InternResolveRoundtrip)
{
    StringInternTable table;
    auto id = table.internRaw("test string");
    EXPECT_EQ(table.resolveRaw(id), "test string");
}

TEST(StringInternTable, DedupReturnsSameId)
{
    StringInternTable table;
    auto id1 = table.internRaw("duplicate");
    auto id2 = table.internRaw("duplicate");
    EXPECT_EQ(id1, id2);
    EXPECT_EQ(table.size(), 2u); // sentinel + one string
}

TEST(StringInternTable, DifferentStringsGetDifferentIds)
{
    StringInternTable table;
    auto id1 = table.internRaw("alpha");
    auto id2 = table.internRaw("beta");
    EXPECT_NE(id1, id2);
}

TEST(StringInternTable, ResolveOutOfRange)
{
    StringInternTable table;
    // Out-of-range index returns empty string_view
    EXPECT_EQ(table.resolveRaw(999), "");
}

// ── Empty string handling ─────────────────────────────────────────────

TEST(StringInternTable, InternEmptyString)
{
    StringInternTable table;
    auto id = table.internRaw("");
    // Empty string gets a real ID (not the sentinel)
    EXPECT_GE(id, 1u);
    EXPECT_EQ(table.resolveRaw(id), "");
}

TEST(StringInternTable, EmptyStringDedups)
{
    StringInternTable table;
    auto id1 = table.internRaw("");
    auto id2 = table.internRaw("");
    EXPECT_EQ(id1, id2);
}

TEST(StringInternTable, EmptyStringDistinctFromSentinel)
{
    StringInternTable table;
    auto id = table.internRaw("");
    // The interned empty string has a different index from the sentinel
    EXPECT_NE(id, 0u);
}

TEST(StringInternTable, AllStringViewsHaveNonNullData)
{
    StringInternTable table;
    // Sentinel has non-null data
    EXPECT_NE(table.resolveRaw(0).data(), nullptr);
    // Empty interned string has non-null data
    auto id = table.internRaw("");
    EXPECT_NE(table.resolveRaw(id).data(), nullptr);
    // Non-empty string has non-null data
    auto id2 = table.internRaw("hello");
    EXPECT_NE(table.resolveRaw(id2).data(), nullptr);
    // Out-of-range has non-null data
    EXPECT_NE(table.resolveRaw(999).data(), nullptr);
}

// ── Typed intern/resolve (Tagged) ──────────────────────────────────

TEST(StringInternTable, TypedInternTestIdA)
{
    StringInternTable table;
    auto srcId = table.intern<TestIdA>("nixpkgs");
    EXPECT_EQ(table.resolve(srcId), "nixpkgs");
    EXPECT_TRUE(static_cast<bool>(srcId)); // non-zero
}

TEST(StringInternTable, TypedInternTestIdB)
{
    StringInternTable table;
    auto keyId = table.intern<TestIdB>("/path/to/file");
    EXPECT_EQ(table.resolve(keyId), "/path/to/file");
    EXPECT_TRUE(static_cast<bool>(keyId));
}

TEST(StringInternTable, TypedInternTestIdC)
{
    StringInternTable table;
    auto strId = table.intern<TestIdC>("shared");
    EXPECT_EQ(table.resolve(strId), "shared");
    EXPECT_TRUE(static_cast<bool>(strId));
}

TEST(StringInternTable, SharedIndexSpaceAcrossTypes)
{
    StringInternTable table;
    auto srcId = table.intern<TestIdA>("shared-string");
    auto keyId = table.intern<TestIdB>("shared-string");
    auto strId = table.intern<TestIdC>("shared-string");
    // All types share the same dedup — same string returns same raw value
    EXPECT_EQ(srcId.value, keyId.value);
    EXPECT_EQ(keyId.value, strId.value);
}

TEST(StringInternTable, DifferentTypesCompileTimeSafety)
{
    StringInternTable table;
    auto srcId = table.intern<TestIdA>("a");
    auto keyId = table.intern<TestIdB>("b");
    // These are different types — cannot be compared directly
    // (This is a compile-time check, not a runtime test)
    EXPECT_NE(srcId.value, keyId.value); // different strings → different IDs
}

// ── bulkLoad ──────────────────────────────────────────────────────────

TEST(StringInternTable, BulkLoadPopulatesTable)
{
    StringInternTable table;
    table.bulkLoad(1, "first");
    table.bulkLoad(2, "second");
    table.bulkLoad(3, "third");
    EXPECT_EQ(table.resolveRaw(1), "first");
    EXPECT_EQ(table.resolveRaw(2), "second");
    EXPECT_EQ(table.resolveRaw(3), "third");
}

TEST(StringInternTable, BulkLoadEnablesDedup)
{
    StringInternTable table;
    table.bulkLoad(1, "existing");
    // intern() should find the bulkLoaded string
    auto id = table.internRaw("existing");
    EXPECT_EQ(id, 1u);
}

TEST(StringInternTable, BulkLoadThenInternNewString)
{
    StringInternTable table;
    table.bulkLoad(1, "from-db");
    table.bulkLoad(2, "also-from-db");
    // New intern gets next available ID
    auto id = table.internRaw("new-string");
    EXPECT_EQ(id, 3u);
    EXPECT_EQ(table.resolveRaw(id), "new-string");
}

TEST(StringInternTable, BulkLoadWithGaps)
{
    StringInternTable table;
    table.bulkLoad(1, "one");
    table.bulkLoad(5, "five"); // creates gap at indices 2-4
    EXPECT_EQ(table.resolveRaw(1), "one");
    EXPECT_EQ(table.resolveRaw(2), ""); // gap → empty
    EXPECT_EQ(table.resolveRaw(5), "five");
    // Next intern fills after the last bulkLoaded index
    auto id = table.internRaw("next");
    EXPECT_EQ(id, 6u);
}

// ── clear() ───────────────────────────────────────────────────────────

// StringInternTable is not movable or clearable. Each test that needs
// a fresh table simply constructs a new local. These tests verify that
// fresh construction starts with the expected initial state.

TEST(StringInternTable, FreshTableIsEmpty)
{
    StringInternTable table;
    EXPECT_EQ(table.size(), 1u); // sentinel only
    EXPECT_EQ(table.nextId(), 1u);
}

TEST(StringInternTable, FreshTableReinterns)
{
    // First table
    {
        StringInternTable table;
        auto id1 = table.internRaw("first-session");
        EXPECT_EQ(id1, 1u);
    }
    // Second table starts fresh — IDs restart from 1
    {
        StringInternTable table;
        auto id2 = table.internRaw("second-session");
        EXPECT_EQ(id2, 1u);
        EXPECT_EQ(table.resolveRaw(id2), "second-session");
    }
}

TEST(StringInternTable, FreshTableBulkLoad)
{
    StringInternTable table;
    table.bulkLoad(1, "reloaded");
    EXPECT_EQ(table.resolveRaw(1), "reloaded");
    // Dedup works after bulk load
    EXPECT_EQ(table.internRaw("reloaded"), 1u);
}

// ── Arena stability ───────────────────────────────────────────────────

TEST(StringInternTable, StringViewsStableAcrossGrowth)
{
    StringInternTable table;
    std::vector<std::pair<uint32_t, std::string>> expected;

    // Intern enough strings to force multiple arena allocations
    for (int i = 0; i < 500; i++) {
        auto s = "string-number-" + std::to_string(i);
        auto id = table.internRaw(s);
        expected.push_back({id, s});
    }

    // Verify all string_views are still valid
    for (auto & [id, str] : expected)
        EXPECT_EQ(table.resolveRaw(id), str);
}

// ── Negative tests ────────────────────────────────────────────────────

TEST(StringInternTable, ResolveInvalidIdReturnsEmpty)
{
    StringInternTable table;
    table.internRaw("only-one");
    // ID beyond table size
    EXPECT_EQ(table.resolveRaw(100), "");
    // Sentinel
    EXPECT_EQ(table.resolveRaw(0), "");
}

TEST(StringInternTable, BulkLoadDoesNotBreakDedup)
{
    StringInternTable table;
    // Intern a string first
    auto id1 = table.internRaw("shared");
    // BulkLoad the same string at a different ID
    table.bulkLoad(10, "shared");
    // The dedup set should still find the first entry
    auto id2 = table.internRaw("shared");
    EXPECT_EQ(id1, id2); // original ID, not 10
}

// ── Concurrency tests ────────────────────────────────────────────────

TEST(StringInternTable, ConcurrentIntern_UniqueStrings)
{
    StringInternTable table;
    constexpr int numThreads = 8;
    constexpr int stringsPerThread = 100;

    std::vector<std::vector<uint32_t>> results(numThreads);
    std::atomic<int> ready{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; t++) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1);
            while (ready.load() < numThreads) {}
            for (int i = 0; i < stringsPerThread; i++) {
                auto s = "thread" + std::to_string(t) + "_str" + std::to_string(i);
                results[t].push_back(table.internRaw(s));
            }
        });
    }
    for (auto & t : threads) t.join();

    // All IDs unique across threads
    std::set<uint32_t> allIds;
    for (auto & r : results)
        for (auto id : r)
            allIds.insert(id);
    EXPECT_EQ(allIds.size(), static_cast<size_t>(numThreads * stringsPerThread));

    // All strings resolvable
    for (int t = 0; t < numThreads; t++) {
        for (int i = 0; i < stringsPerThread; i++) {
            auto expected = "thread" + std::to_string(t) + "_str" + std::to_string(i);
            EXPECT_EQ(table.resolveRaw(results[t][i]), expected);
        }
    }
}

TEST(StringInternTable, ConcurrentIntern_SameString)
{
    StringInternTable table;
    constexpr int numThreads = 8;

    std::vector<uint32_t> results(numThreads);
    std::atomic<int> ready{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; t++) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1);
            while (ready.load() < numThreads) {}
            results[t] = table.internRaw("shared-string");
        });
    }
    for (auto & t : threads) t.join();

    // All threads got the same ID
    for (int t = 1; t < numThreads; t++) {
        EXPECT_EQ(results[0], results[t]);
    }
    EXPECT_EQ(table.resolveRaw(results[0]), "shared-string");
}

TEST(StringInternTable, ConcurrentInternAndResolve)
{
    StringInternTable table;
    constexpr int numStrings = 200;

    // Pre-intern some strings
    for (int i = 0; i < 50; i++) {
        table.internRaw("pre-" + std::to_string(i));
    }

    std::atomic<bool> done{false};

    // Producer: interns new strings
    std::thread producer([&] {
        for (int i = 50; i < numStrings; i++) {
            table.internRaw("str-" + std::to_string(i));
        }
        done = true;
    });

    // Consumer: resolves existing IDs concurrently
    std::thread consumer([&] {
        while (!done.load()) {
            auto sz = table.size();
            for (uint32_t i = 1; i < sz; i++) {
                auto sv = table.resolveRaw(i);
                EXPECT_NE(sv.data(), nullptr);
            }
        }
    });

    producer.join();
    consumer.join();
}

TEST(StringInternTable, ConcurrentFind)
{
    StringInternTable table;
    constexpr int numStrings = 100;

    // Pre-intern strings
    for (int i = 0; i < numStrings; i++) {
        table.internRaw("find-" + std::to_string(i));
    }

    constexpr int numThreads = 4;
    std::atomic<int> ready{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; t++) {
        threads.emplace_back([&] {
            ready.fetch_add(1);
            while (ready.load() < numThreads) {}
            for (int i = 0; i < numStrings; i++) {
                auto id = table.findRaw("find-" + std::to_string(i));
                EXPECT_NE(id, 0u);
            }
            // Non-existent string
            EXPECT_EQ(table.findRaw("nonexistent"), 0u);
        });
    }
    for (auto & t : threads) t.join();
}

TEST(StringInternTable, BulkLoadThenConcurrentIntern)
{
    StringInternTable table;

    // Bulk load phase (sequential)
    for (uint32_t i = 1; i <= 50; i++) {
        table.bulkLoad(i, "bulk-" + std::to_string(i));
    }

    constexpr int numThreads = 4;
    constexpr int newStrings = 50;
    std::atomic<int> ready{0};

    std::vector<std::vector<uint32_t>> results(numThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < numThreads; t++) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1);
            while (ready.load() < numThreads) {}
            // Re-intern bulk-loaded strings
            for (uint32_t i = 1; i <= 50; i++) {
                auto id = table.internRaw("bulk-" + std::to_string(i));
                EXPECT_EQ(id, i); // should return original bulk-loaded ID
            }
            // Intern new strings
            for (int i = 0; i < newStrings; i++) {
                auto s = "new-t" + std::to_string(t) + "-" + std::to_string(i);
                results[t].push_back(table.internRaw(s));
            }
        });
    }
    for (auto & t : threads) t.join();

    // New strings have unique IDs > 50
    std::set<uint32_t> newIds;
    for (auto & r : results)
        for (auto id : r) {
            EXPECT_GT(id, 50u);
            newIds.insert(id);
        }
    EXPECT_EQ(newIds.size(), static_cast<size_t>(numThreads * newStrings));
}

} // namespace nix
