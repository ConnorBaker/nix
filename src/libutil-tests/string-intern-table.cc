#include "nix/util/string-intern-table.hh"
#include "nix/util/traced-data-ids.hh"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace nix {

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

// ── Typed intern/resolve (StrongId) ──────────────────────────────────

TEST(StringInternTable, TypedInternDepSourceId)
{
    StringInternTable table;
    auto srcId = table.intern<DepSourceId>("nixpkgs");
    EXPECT_EQ(table.resolve(srcId), "nixpkgs");
    EXPECT_TRUE(static_cast<bool>(srcId)); // non-zero
}

TEST(StringInternTable, TypedInternDepKeyId)
{
    StringInternTable table;
    auto keyId = table.intern<DepKeyId>("/path/to/file");
    EXPECT_EQ(table.resolve(keyId), "/path/to/file");
    EXPECT_TRUE(static_cast<bool>(keyId));
}

TEST(StringInternTable, TypedInternStringId)
{
    StringInternTable table;
    auto strId = table.intern<StringId>("shared");
    EXPECT_EQ(table.resolve(strId), "shared");
    EXPECT_TRUE(static_cast<bool>(strId));
}

TEST(StringInternTable, SharedIndexSpaceAcrossTypes)
{
    StringInternTable table;
    auto srcId = table.intern<DepSourceId>("shared-string");
    auto keyId = table.intern<DepKeyId>("shared-string");
    auto strId = table.intern<StringId>("shared-string");
    // All types share the same dedup — same string returns same raw value
    EXPECT_EQ(srcId.value, keyId.value);
    EXPECT_EQ(keyId.value, strId.value);
}

TEST(StringInternTable, DifferentTypesCompileTimeSafety)
{
    StringInternTable table;
    auto srcId = table.intern<DepSourceId>("a");
    auto keyId = table.intern<DepKeyId>("b");
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

TEST(StringInternTable, ClearResetsToEmpty)
{
    StringInternTable table;
    table.internRaw("a");
    table.internRaw("b");
    table.internRaw("c");
    EXPECT_EQ(table.size(), 4u); // sentinel + 3

    table.clear();
    EXPECT_EQ(table.size(), 1u); // sentinel only
    EXPECT_EQ(table.nextId(), 1u);
}

TEST(StringInternTable, ClearThenReintern)
{
    StringInternTable table;
    auto id1 = table.internRaw("before-clear");
    EXPECT_EQ(id1, 1u);

    table.clear();
    auto id2 = table.internRaw("after-clear");
    EXPECT_EQ(id2, 1u); // IDs restart from 1
    EXPECT_EQ(table.resolveRaw(id2), "after-clear");
}

TEST(StringInternTable, ClearReleasesArena)
{
    StringInternTable table;
    // Intern many strings to grow arena
    for (int i = 0; i < 1000; i++)
        table.internRaw("string-" + std::to_string(i));
    EXPECT_EQ(table.size(), 1001u);

    table.clear();
    EXPECT_EQ(table.size(), 1u);

    // Re-interning after clear works correctly
    auto id = table.internRaw("after-bulk-clear");
    EXPECT_EQ(id, 1u);
    EXPECT_EQ(table.resolveRaw(id), "after-bulk-clear");
}

TEST(StringInternTable, ClearThenBulkLoad)
{
    StringInternTable table;
    table.internRaw("session1");
    table.clear();
    table.bulkLoad(1, "reloaded");
    EXPECT_EQ(table.resolveRaw(1), "reloaded");
    // Dedup works after reload
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

} // namespace nix
