#include "helpers.hh"
#include "nix/expr/eval-index-db.hh"

#include <gtest/gtest.h>

#include "nix/store/store-api.hh"
#include "nix/store/tests/libstore.hh"

namespace nix::eval_cache {

class EvalIndexDbTest : public LibStoreTest
{
protected:
    // Writable cache dir for EvalIndexDb SQLite (sandbox has no writable $HOME)
    test::ScopedCacheDir cacheDir;

    // Use unlikely context hashes to avoid collisions with persistent DB data
    static constexpr int64_t baseCtx = 0x7E57CA5E00000000LL; // "TEST CASE"
    int64_t nextCtx = baseCtx;

    int64_t freshContextHash()
    {
        return nextCtx++;
    }

    const StoreDirConfig & storeConfig()
    {
        return *store;
    }

    StorePath makeStorePath(std::string_view name)
    {
        // Nix base-32 uses: 0123456789abcdfghijklmnpqrsvwxyz
        return StorePath{std::string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-") + std::string(name)};
    }
};

// ── Basic CRUD ───────────────────────────────────────────────────────

TEST_F(EvalIndexDbTest, Lookup_EmptyDb)
{
    EvalIndexDb db;
    // Use a unique context hash that no test will have inserted
    auto result = db.lookup(freshContextHash(), "nonexistent-attr-path-xyz", storeConfig());
    EXPECT_FALSE(result.has_value());
}

TEST_F(EvalIndexDbTest, Upsert_ThenLookup)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();
    auto tracePath = makeStorePath("trace-pkg");

    db.upsert(ctx, "pkg", tracePath, storeConfig());

    auto result = db.lookup(ctx, "pkg", storeConfig());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(store->printStorePath(result->tracePath),
              store->printStorePath(tracePath));
}

TEST_F(EvalIndexDbTest, Upsert_OverwritesPrevious)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();
    auto trace1 = makeStorePath("trace-v1");
    auto trace2 = makeStorePath("trace-v2");

    db.upsert(ctx, "pkg", trace1, storeConfig());
    db.upsert(ctx, "pkg", trace2, storeConfig());

    auto result = db.lookup(ctx, "pkg", storeConfig());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(store->printStorePath(result->tracePath),
              store->printStorePath(trace2));
}

TEST_F(EvalIndexDbTest, DifferentContextHash_Isolated)
{
    EvalIndexDb db;
    auto ctx1 = freshContextHash();
    auto ctx2 = freshContextHash();
    auto trace1 = makeStorePath("trace-ctx1");
    auto trace2 = makeStorePath("trace-ctx2");

    db.upsert(ctx1, "pkg", trace1, storeConfig());
    db.upsert(ctx2, "pkg", trace2, storeConfig());

    auto r1 = db.lookup(ctx1, "pkg", storeConfig());
    auto r2 = db.lookup(ctx2, "pkg", storeConfig());

    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(store->printStorePath(r1->tracePath),
              store->printStorePath(trace1));
    EXPECT_EQ(store->printStorePath(r2->tracePath),
              store->printStorePath(trace2));
}

TEST_F(EvalIndexDbTest, DifferentAttrPath_Isolated)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();
    auto traceA = makeStorePath("trace-a");
    auto traceB = makeStorePath("trace-b");

    db.upsert(ctx, "a", traceA, storeConfig());
    db.upsert(ctx, "b", traceB, storeConfig());

    auto rA = db.lookup(ctx, "a", storeConfig());
    auto rB = db.lookup(ctx, "b", storeConfig());

    ASSERT_TRUE(rA.has_value());
    ASSERT_TRUE(rB.has_value());
    EXPECT_EQ(store->printStorePath(rA->tracePath),
              store->printStorePath(traceA));
    EXPECT_EQ(store->printStorePath(rB->tracePath),
              store->printStorePath(traceB));
}

TEST_F(EvalIndexDbTest, NullByteAttrPath)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();
    auto tracePath = makeStorePath("trace-null");

    // Null-byte separated path like "packages\0x86_64-linux\0hello"
    std::string attrPath = "packages";
    attrPath.push_back('\0');
    attrPath.append("x86_64-linux");
    attrPath.push_back('\0');
    attrPath.append("hello");

    db.upsert(ctx, attrPath, tracePath, storeConfig());

    auto result = db.lookup(ctx, attrPath, storeConfig());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(store->printStorePath(result->tracePath),
              store->printStorePath(tracePath));
}

TEST_F(EvalIndexDbTest, EmptyAttrPath)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();
    auto tracePath = makeStorePath("trace-root");

    db.upsert(ctx, "", tracePath, storeConfig());

    auto result = db.lookup(ctx, "", storeConfig());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(store->printStorePath(result->tracePath),
              store->printStorePath(tracePath));
}

TEST_F(EvalIndexDbTest, StorePath_Roundtrip)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();
    auto tracePath = makeStorePath("trace-roundtrip");

    db.upsert(ctx, "test", tracePath, storeConfig());

    auto result = db.lookup(ctx, "test", storeConfig());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(store->printStorePath(result->tracePath),
              store->printStorePath(tracePath));
}

TEST_F(EvalIndexDbTest, LookupNonexistentContextHash)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();
    auto unusedCtx = freshContextHash();
    auto tracePath = makeStorePath("trace-exists");

    db.upsert(ctx, "test", tracePath, storeConfig());

    auto result = db.lookup(unusedCtx, "test", storeConfig());
    EXPECT_FALSE(result.has_value());
}

TEST_F(EvalIndexDbTest, MultipleEntries_Stress)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();

    // Insert 100 entries using valid nix base-32 hashes
    // Nix base-32 chars: 0123456789abcdfghijklmnpqrsvwxyz
    const char b32[] = "0123456789abcdfghijklmnpqrsvwxyz";
    for (int i = 0; i < 100; i++) {
        auto name = "stress-" + std::to_string(i);
        // Generate a unique 32-char base-32 hash
        std::string hashStr(32, '0');
        int val = i;
        for (int j = 31; j >= 0 && val > 0; j--) {
            hashStr[j] = b32[val % 32];
            val /= 32;
        }
        auto tracePath = StorePath{hashStr + "-trace-" + std::to_string(i)};
        db.upsert(ctx, name, tracePath, storeConfig());
    }

    auto r0 = db.lookup(ctx, "stress-0", storeConfig());
    ASSERT_TRUE(r0.has_value());

    auto r99 = db.lookup(ctx, "stress-99", storeConfig());
    ASSERT_TRUE(r99.has_value());

    auto rMissing = db.lookup(ctx, "stress-100", storeConfig());
    EXPECT_FALSE(rMissing.has_value());
}

// ── DepStructGroups tests ────────────────────────────────────────────

TEST_F(EvalIndexDbTest, StructGroup_BasicInsert)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();
    auto trace1 = makeStorePath("trace-sg1");
    auto trace2 = makeStorePath("trace-sg2");
    auto trace3 = makeStorePath("trace-sg3");

    auto h1 = hashString(HashAlgorithm::SHA256, "struct1");
    auto h2 = hashString(HashAlgorithm::SHA256, "struct2");
    auto h3 = hashString(HashAlgorithm::SHA256, "struct3");

    db.upsertStructGroup(ctx, "pkg", h1, trace1, storeConfig());
    db.upsertStructGroup(ctx, "pkg", h2, trace2, storeConfig());
    db.upsertStructGroup(ctx, "pkg", h3, trace3, storeConfig());

    auto groups = db.scanStructGroups(ctx, "pkg", storeConfig());
    EXPECT_EQ(groups.size(), 3u);
}

TEST_F(EvalIndexDbTest, StructGroup_UpsertSameHash)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();
    auto trace1 = makeStorePath("trace-old");
    auto trace2 = makeStorePath("trace-new");
    auto h = hashString(HashAlgorithm::SHA256, "struct-same");

    db.upsertStructGroup(ctx, "pkg", h, trace1, storeConfig());
    db.upsertStructGroup(ctx, "pkg", h, trace2, storeConfig());

    auto groups = db.scanStructGroups(ctx, "pkg", storeConfig());
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(store->printStorePath(groups[0].tracePath),
              store->printStorePath(trace2));
}

TEST_F(EvalIndexDbTest, StructGroup_Isolation)
{
    EvalIndexDb db;
    auto ctx1 = freshContextHash();
    auto ctx2 = freshContextHash();
    auto trace1 = makeStorePath("trace-iso1");
    auto trace2 = makeStorePath("trace-iso2");
    auto h = hashString(HashAlgorithm::SHA256, "struct-iso");

    db.upsertStructGroup(ctx1, "pkg", h, trace1, storeConfig());
    db.upsertStructGroup(ctx2, "other", h, trace2, storeConfig());

    auto groups1 = db.scanStructGroups(ctx1, "pkg", storeConfig());
    EXPECT_EQ(groups1.size(), 1u);

    auto groups2 = db.scanStructGroups(ctx2, "other", storeConfig());
    EXPECT_EQ(groups2.size(), 1u);

    // Cross-lookup should be empty
    auto empty = db.scanStructGroups(ctx1, "other", storeConfig());
    EXPECT_TRUE(empty.empty());
}

TEST_F(EvalIndexDbTest, StructGroup_Empty)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();

    auto groups = db.scanStructGroups(ctx, "nonexistent", storeConfig());
    EXPECT_TRUE(groups.empty());
}

TEST_F(EvalIndexDbTest, ColdStoreIndex_BatchWrites)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();
    auto tracePath = makeStorePath("trace-batch");
    auto depHash = hashString(HashAlgorithm::SHA256, "dep-hash");
    auto structHash = hashString(HashAlgorithm::SHA256, "struct-hash");
    auto parentPath = makeStorePath("trace-parent");
    auto parentDepHash = hashString(HashAlgorithm::SHA256, "parent-dep-hash");

    db.coldStoreIndex(ctx, "pkg", tracePath, depHash,
                       std::pair{parentDepHash, parentPath}, structHash, storeConfig());

    // Verify EvalIndex entry
    auto entry = db.lookup(ctx, "pkg", storeConfig());
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(store->printStorePath(entry->tracePath),
              store->printStorePath(tracePath));

    // Verify Phase 1 recovery entry (depHash without parent)
    auto r1 = db.lookupByDepHash(ctx, "pkg", depHash, storeConfig());
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(store->printStorePath(*r1), store->printStorePath(tracePath));

    // Verify Phase 2 recovery entry (parent-aware depHash)
    auto r2 = db.lookupByDepHash(ctx, "pkg", parentDepHash, storeConfig());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(store->printStorePath(*r2), store->printStorePath(tracePath));

    // Verify struct group
    auto groups = db.scanStructGroups(ctx, "pkg", storeConfig());
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(store->printStorePath(groups[0].tracePath),
              store->printStorePath(tracePath));
}

TEST_F(EvalIndexDbTest, ColdStoreIndex_NoParent)
{
    EvalIndexDb db;
    auto ctx = freshContextHash();
    auto tracePath = makeStorePath("trace-noparent");
    auto depHash = hashString(HashAlgorithm::SHA256, "dep-hash-np");
    auto structHash = hashString(HashAlgorithm::SHA256, "struct-hash-np");

    db.coldStoreIndex(ctx, "root", tracePath, depHash,
                       std::nullopt, structHash, storeConfig());

    // Verify EvalIndex entry
    auto entry = db.lookup(ctx, "root", storeConfig());
    ASSERT_TRUE(entry.has_value());

    // Verify Phase 1 recovery entry
    auto r1 = db.lookupByDepHash(ctx, "root", depHash, storeConfig());
    ASSERT_TRUE(r1.has_value());

    // Verify struct group
    auto groups = db.scanStructGroups(ctx, "root", storeConfig());
    ASSERT_EQ(groups.size(), 1u);
}

} // namespace nix::eval_cache
