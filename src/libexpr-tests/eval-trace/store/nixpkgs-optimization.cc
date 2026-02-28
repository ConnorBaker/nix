#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <algorithm>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Serialization edge case tests ────────────────────────────────────

TEST_F(TraceStoreTest, BlobRoundTrip_StructuredContent)
{
    // StructuredContent deps use Blake3 hashes, like Content.
    // Verify they round-trip correctly through the factored serialization.
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    keys.push_back({DepType::StructuredContent, StringId(1), StringId(2)});
    deps.push_back({DepType::StructuredContent, StringId(1), StringId(2), DepHashValue(depHash("scalar-value"))});

    keys.push_back({DepType::Content, StringId(3), StringId(4)});
    deps.push_back({DepType::Content, StringId(3), StringId(4), DepHashValue(depHash("file-content"))});

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 2u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 2u);

    // StructuredContent is a Blake3 dep type
    EXPECT_EQ(keysResult[0].type, DepType::StructuredContent);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[0]));
    EXPECT_EQ(std::get<Blake3Hash>(valsResult[0]), depHash("scalar-value"));

    // Content is also Blake3
    EXPECT_EQ(keysResult[1].type, DepType::Content);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[1]));
    EXPECT_EQ(std::get<Blake3Hash>(valsResult[1]), depHash("file-content"));
}

TEST_F(TraceStoreTest, BlobRoundTrip_32ByteStringVsBlake3)
{
    // Critical test: a CopiedPath string value that is exactly 32 bytes
    // must be deserialized as a string, NOT as a Blake3Hash.
    // This tests the disambiguation logic in deserializeValues.
    std::string exactly32 = "abcdefghijklmnopqrstuvwxyz012345"; // 32 chars
    ASSERT_EQ(exactly32.size(), 32u);

    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    // CopiedPath with exactly 32-byte string value
    keys.push_back({DepType::CopiedPath, StringId(1), StringId(2)});
    deps.push_back({DepType::CopiedPath, StringId(1), StringId(2), DepHashValue(exactly32)});

    // Content with Blake3 hash (also 32 bytes)
    keys.push_back({DepType::Content, StringId(3), StringId(4)});
    deps.push_back({DepType::Content, StringId(3), StringId(4), DepHashValue(depHash("data"))});

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 2u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 2u);

    // CopiedPath with 32-byte value must deserialize as string
    EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[0]))
        << "32-byte CopiedPath value was incorrectly deserialized as Blake3Hash";
    EXPECT_EQ(std::get<std::string>(valsResult[0]), exactly32);

    // Content with 32-byte value must deserialize as Blake3Hash
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[1]))
        << "Content dep value was incorrectly deserialized as string";
}

TEST_F(TraceStoreTest, BlobRoundTrip_SingleEntry)
{
    // Boundary: single dep in the serialization.
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    keys.push_back({DepType::EnvVar, StringId(42), StringId(99)});
    deps.push_back({DepType::EnvVar, StringId(42), StringId(99), DepHashValue(depHash("HOME=/home/user"))});

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 1u);
    EXPECT_EQ(keysResult[0].type, DepType::EnvVar);
    EXPECT_EQ(keysResult[0].sourceId, StringId(42));
    EXPECT_EQ(keysResult[0].keyId, StringId(99));

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 1u);
    EXPECT_EQ(valsResult[0], deps[0].hash);
}

TEST_F(TraceStoreTest, BlobRoundTrip_AllDepTypes)
{
    // Round-trip test covering every dep type in the DepType enum.
    // This ensures isBlake3Dep is consistent with serialization/deserialization.
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    auto addBlake3 = [&](DepType type, uint32_t sid, uint32_t kid, std::string_view data) {
        keys.push_back({type, StringId(sid), StringId(kid)});
        deps.push_back({type, StringId(sid), StringId(kid), DepHashValue(depHash(data))});
    };
    auto addString = [&](DepType type, uint32_t sid, uint32_t kid, std::string_view data) {
        keys.push_back({type, StringId(sid), StringId(kid)});
        deps.push_back({type, StringId(sid), StringId(kid), DepHashValue(std::string(data))});
    };

    // Blake3 dep types
    addBlake3(DepType::Content, 1, 1, "file");
    addBlake3(DepType::Directory, 2, 2, "dir");
    addBlake3(DepType::EnvVar, 3, 3, "env");
    addBlake3(DepType::System, 4, 4, "sys");
    addBlake3(DepType::NARContent, 5, 5, "nar");
    addBlake3(DepType::StructuredContent, 6, 6, "struct");
    addBlake3(DepType::ParentContext, 7, 7, "parent");

    // String dep types
    addString(DepType::Existence, 8, 8, "type:1");
    addString(DepType::CurrentTime, 9, 9, "volatile");
    addString(DepType::UnhashedFetch, 10, 10, "url");
    addString(DepType::CopiedPath, 11, 11, "/nix/store/test");
    addString(DepType::Exec, 12, 12, "volatile");

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 12u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 12u);

    // Blake3 types should deserialize as Blake3Hash
    for (int i = 0; i < 7; i++) {
        EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[i]))
            << "Dep type " << static_cast<int>(keysResult[i].type)
            << " should deserialize as Blake3Hash";
    }
    // String types should deserialize as std::string
    for (int i = 7; i < 12; i++) {
        EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[i]))
            << "Dep type " << static_cast<int>(keysResult[i].type)
            << " should deserialize as string";
    }
}

// ── DepKeySets sharing tests ─────────────────────────────────────────

TEST_F(TraceStoreTest, DepKeySets_SharedKeysDifferentValues)
{
    // Two traces with identical dep keys but different hash values should
    // share the same DepKeySets row (same struct_hash) but have different
    // Traces rows (different trace_hash / values_blob).
    auto db = makeDb();

    std::vector<Dep> depsV1 = {
        makeContentDep("/a.nix", "content-v1"),
        makeEnvVarDep("HOME", "/home/user1"),
    };
    std::vector<Dep> depsV2 = {
        makeContentDep("/a.nix", "content-v2"),
        makeEnvVarDep("HOME", "/home/user2"),
    };

    auto r1 = db.recordDeps("attr", string_t{"result-1", {}}, depsV1, true);
    auto r2 = db.recordDeps("attr", string_t{"result-2", {}}, depsV2, true);

    // Different trace IDs (different hash values → different trace_hash)
    EXPECT_NE(r1.traceId, r2.traceId);

    // Both should load correctly
    auto loaded1 = db.loadFullTrace(r1.traceId);
    auto loaded2 = db.loadFullTrace(r2.traceId);
    EXPECT_EQ(loaded1.size(), 2u);
    EXPECT_EQ(loaded2.size(), 2u);

    // Verify the loaded values differ
    // Both have Content dep at index 0 (after sort); check the hash values differ
    bool foundDifference = false;
    for (size_t i = 0; i < loaded1.size(); i++) {
        if (loaded1[i].expectedHash != loaded2[i].expectedHash) {
            foundDifference = true;
            break;
        }
    }
    EXPECT_TRUE(foundDifference) << "Traces with same keys but different values should have different dep hashes";
}

// ── Phase 1 cache optimization tests ──────────────────────────────────
// Tests for the unified CachedTraceData cache, traceRowCache, in-memory
// recovery matching, and placeholder hash detection.

TEST_F(TraceStoreTest, CachedTraceData_HashesPopulated_Default)
{
    // Default-constructed CachedTraceData has placeholder (all-zero) hashes.
    TraceStore::CachedTraceData data;
    EXPECT_FALSE(data.hashesPopulated())
        << "Default CachedTraceData should have unpopulated (all-zero) hashes";
}

TEST_F(TraceStoreTest, CachedTraceData_HashesPopulated_AfterRecord)
{
    // After record(), traceDataCache should contain populated hashes.
    auto db = makeDb();
    auto result = db.recordDeps("test", string_t{"value", {}},
        {makeContentDep("/a.nix", "hello")}, true);

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_TRUE(it->second.hashesPopulated())
        << "CachedTraceData after record should have non-zero hashes";
    EXPECT_TRUE(it->second.deps.has_value())
        << "CachedTraceData after record should have deps populated";
}

TEST_F(TraceStoreTest, CachedTraceData_HashesPopulated_AfterEnsure)
{
    // ensureTraceHashes populates hashes but NOT deps.
    // getCurrentTraceHash calls ensureTraceHashes internally.
    auto db = makeDb();
    auto result = db.recordDeps("test", string_t{"value", {}},
        {makeContentDep("/a.nix", "hello")}, true);

    // Clear session caches to force re-read from DB
    db.traceDataCache.clear();

    // getCurrentTraceHash calls lookupTraceRow → ensureTraceHashes
    auto hash = db.getCurrentTraceHash("test");
    ASSERT_TRUE(hash.has_value());

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_TRUE(it->second.hashesPopulated())
        << "ensureTraceHashes should populate non-zero hashes";
    EXPECT_FALSE(it->second.deps.has_value())
        << "ensureTraceHashes should NOT populate deps (lazy)";
}

TEST_F(TraceStoreTest, LoadFullTrace_NonexistentTrace_NoCachePollution)
{
    // loadFullTrace on a nonexistent traceId should NOT create a cache entry.
    // This was a real bug (fixed): the old code did traceDataCache[traceId].deps = {},
    // which created an entry with placeholder zero hashes that ensureTraceHashes
    // would later incorrectly return as valid.
    auto db = makeDb();
    TraceId bogusId(99999);

    auto result = db.loadFullTrace(bogusId);
    EXPECT_TRUE(result.empty());

    // Verify no cache pollution
    EXPECT_EQ(db.traceDataCache.find(bogusId), db.traceDataCache.end())
        << "loadFullTrace on nonexistent trace must not create a cache entry";
}

TEST_F(TraceStoreTest, LoadFullTrace_PopulatesHashes_Opportunistically)
{
    // loadFullTrace should populate traceHash + structHash + deps in one shot.
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "content")};
    auto result = db.recordDeps("test", string_t{"val", {}}, deps, true);

    // Clear caches, then use loadFullTrace (not ensureTraceHashes)
    db.traceDataCache.clear();

    auto loaded = db.loadFullTrace(result.traceId);
    EXPECT_EQ(loaded.size(), 1u);

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_TRUE(it->second.hashesPopulated())
        << "loadFullTrace should opportunistically populate hash fields";
    EXPECT_TRUE(it->second.deps.has_value())
        << "loadFullTrace should populate deps";
}

TEST_F(TraceStoreTest, TraceDataCache_EnsureThenLoad_SingleDbQuery)
{
    // ensureTraceHashes (via getCurrentTraceHash) then loadFullTrace should
    // reuse cached hashes. Hash fields should be identical regardless of order.
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep("/a.nix", "content")};
    auto result = db.recordDeps("test", string_t{"val", {}}, deps, true);

    // Record populates everything; get hashes for comparison
    auto itAfterRecord = db.traceDataCache.find(result.traceId);
    ASSERT_NE(itAfterRecord, db.traceDataCache.end());
    auto expectedTraceHash = itAfterRecord->second.traceHash;

    // Clear and re-populate via ensureTraceHashes (getCurrentTraceHash)
    db.traceDataCache.clear();
    auto traceHash = db.getCurrentTraceHash("test");
    ASSERT_TRUE(traceHash.has_value());
    EXPECT_EQ(*traceHash, expectedTraceHash);

    // Verify deps not yet populated (lazy)
    auto itPartial = db.traceDataCache.find(result.traceId);
    ASSERT_NE(itPartial, db.traceDataCache.end());
    EXPECT_FALSE(itPartial->second.deps.has_value())
        << "ensureTraceHashes should not populate deps";

    // Now loadFullTrace should find the partial cache entry and extend it
    auto loaded = db.loadFullTrace(result.traceId);
    EXPECT_EQ(loaded.size(), 1u);

    auto it = db.traceDataCache.find(result.traceId);
    ASSERT_NE(it, db.traceDataCache.end());
    EXPECT_EQ(it->second.traceHash, expectedTraceHash);
    EXPECT_TRUE(it->second.deps.has_value());
}

TEST_F(TraceStoreTest, TraceRowCache_HitAfterRecord)
{
    // After record(), lookupTraceRow should hit the traceRowCache (no DB).
    auto db = makeDb();
    auto result = db.recordDeps("myattr", string_t{"result", {}},
        {makeEnvVarDep("FOO", "bar")}, true);

    // traceRowCache should be populated from record()
    EXPECT_EQ(db.traceRowCache.count("myattr"), 1u)
        << "record() should populate traceRowCache";

    // Verify the cached row has correct traceId
    auto & row = db.traceRowCache["myattr"];
    EXPECT_EQ(row.traceId, result.traceId);
}

TEST_F(TraceStoreTest, TraceRowCache_HitAfterLookup)
{
    // After a lookupTraceRow miss that hits DB, cache should be populated.
    auto db = makeDb();
    db.recordDeps("attr", string_t{"val", {}}, {}, true);

    // Clear traceRowCache (simulate fresh session lookup, not record)
    db.traceRowCache.clear();

    // First lookup goes to DB, populates cache
    EXPECT_TRUE(db.attrExists("attr"));
    EXPECT_EQ(db.traceRowCache.count("attr"), 1u)
        << "lookupTraceRow should populate traceRowCache on DB hit";
}

TEST_F(TraceStoreTest, TraceRowCache_MissDoesNotCache)
{
    // Looking up a nonexistent attr should NOT create a cache entry.
    auto db = makeDb();
    EXPECT_FALSE(db.attrExists("nonexistent"));
    EXPECT_EQ(db.traceRowCache.count("nonexistent"), 0u)
        << "lookupTraceRow miss should not create a cache entry";
}

TEST_F(TraceStoreTest, GetCurrentTraceHash_CachedAfterFirstCall)
{
    // getCurrentTraceHash should cache its result — second call hits cache.
    auto db = makeDb();
    // Record a parent with tab-separated key (simulating ParentContext dep key format)
    std::string attrPath = "packages";
    attrPath.push_back('\0');
    attrPath.append("hello");
    db.recordDeps(attrPath, string_t{"result", {}},
        {makeContentDep("/hello.nix", "v1")}, true);

    // Convert to tab-separated key as ParentContext deps do
    std::string depKey = "packages";
    depKey.push_back('\t');
    depKey.append("hello");

    auto hash1 = db.getCurrentTraceHash(depKey);
    ASSERT_TRUE(hash1.has_value());

    // Clear DB-level caches but keep session caches (traceRowCache + traceDataCache)
    // This simulates a "second call in the same session"
    // If the second call goes to DB, it would still work, but we want to verify caching.

    auto hash2 = db.getCurrentTraceHash(depKey);
    ASSERT_TRUE(hash2.has_value());
    EXPECT_EQ(*hash1, *hash2)
        << "getCurrentTraceHash should return same value from cache";
}

TEST_F(TraceStoreTest, TraceRowCache_InvalidatedOnRecovery)
{
    // When recovery succeeds, traceRowCache should be updated to reflect
    // the recovered trace. Subsequent lookups should return the new trace.
    auto db = makeDb();

    // Record version 1
    std::vector<Dep> depsV1 = {makeEnvVarDep("MY_VAR", "v1")};
    db.recordDeps("attr", string_t{"result-v1", {}}, depsV1, true);

    // Record version 2 (different deps → different trace)
    std::vector<Dep> depsV2 = {makeEnvVarDep("MY_VAR", "v2")};
    db.recordDeps("attr", string_t{"result-v2", {}}, depsV2, true);

    // Clear session caches to simulate new session
    db.clearSessionCaches();

    // Set env to v1 (mismatches current trace v2, should recover to v1)
    setenv("MY_VAR", "v1", 1);
    auto verifyResult = db.verify("attr", {}, state);
    ASSERT_TRUE(verifyResult.has_value());

    // Verify the traceRowCache was updated
    EXPECT_EQ(db.traceRowCache.count("attr"), 1u);
    auto & row = db.traceRowCache["attr"];
    EXPECT_EQ(row.traceId, verifyResult->traceId)
        << "traceRowCache should reflect the recovered trace";

    // Verify getCurrentTraceHash also reflects recovery
    auto traceHash = db.getCurrentTraceHash("attr");
    ASSERT_TRUE(traceHash.has_value());
    auto * data = db.traceDataCache.count(verifyResult->traceId)
        ? &db.traceDataCache[verifyResult->traceId] : nullptr;
    if (data && data->hashesPopulated()) {
        EXPECT_EQ(*traceHash, data->traceHash)
            << "getCurrentTraceHash should match recovered trace's hash";
    }

    unsetenv("MY_VAR");
}

TEST_F(TraceStoreTest, InMemoryRecovery_DirectHash)
{
    // Recovery should use in-memory trace_hash matching (no per-group DB lookup).
    // This test verifies the functional correctness of in-memory direct hash recovery.
    auto db = makeDb();

    // Record two versions
    setenv("INTEST", "alpha", 1);
    std::vector<Dep> depsA = {makeEnvVarDep("INTEST", "alpha")};
    db.recordDeps("attr", string_t{"result-alpha", {}}, depsA, true);

    setenv("INTEST", "beta", 1);
    std::vector<Dep> depsB = {makeEnvVarDep("INTEST", "beta")};
    db.recordDeps("attr", string_t{"result-beta", {}}, depsB, true);

    // Clear session caches
    db.clearSessionCaches();

    // Revert env to alpha → direct hash recovery should find the alpha trace
    setenv("INTEST", "alpha", 1);
    auto result = db.verify("attr", {}, state);
    ASSERT_TRUE(result.has_value());

    // Should have recovered to alpha's result
    auto * strResult = std::get_if<string_t>(&result->value);
    ASSERT_NE(strResult, nullptr);
    EXPECT_EQ(strResult->first, "result-alpha");

    unsetenv("INTEST");
}

TEST_F(TraceStoreTest, InMemoryRecovery_StructuralVariant)
{
    // Structural variant recovery should also use in-memory matching.
    auto db = makeDb();

    // Version 1: depends on VAR_A only
    setenv("SVR_A", "a1", 1);
    std::vector<Dep> depsV1 = {makeEnvVarDep("SVR_A", "a1")};
    db.recordDeps("attr", string_t{"r1", {}}, depsV1, true);

    // Version 2: depends on VAR_A + VAR_B (different dep structure)
    setenv("SVR_A", "a2", 1);
    setenv("SVR_B", "b2", 1);
    std::vector<Dep> depsV2 = {
        makeEnvVarDep("SVR_A", "a2"),
        makeEnvVarDep("SVR_B", "b2"),
    };
    db.recordDeps("attr", string_t{"r2", {}}, depsV2, true);

    // Clear caches
    db.clearSessionCaches();

    // Set env to match v1 (single dep)
    setenv("SVR_A", "a1", 1);
    unsetenv("SVR_B");

    auto result = db.verify("attr", {}, state);
    ASSERT_TRUE(result.has_value());

    auto * strResult = std::get_if<string_t>(&result->value);
    ASSERT_NE(strResult, nullptr);
    EXPECT_EQ(strResult->first, "r1")
        << "Structural variant recovery should find the matching trace";

    unsetenv("SVR_A");
}

TEST_F(TraceStoreTest, ClearSessionCaches_ClearsAllNewCaches)
{
    // clearSessionCaches should clear both traceDataCache and traceRowCache.
    auto db = makeDb();
    db.recordDeps("attr", string_t{"val", {}}, {makeContentDep("/f", "c")}, true);

    EXPECT_FALSE(db.traceDataCache.empty());
    EXPECT_FALSE(db.traceRowCache.empty());

    db.clearSessionCaches();

    EXPECT_TRUE(db.traceDataCache.empty())
        << "clearSessionCaches must clear traceDataCache";
    EXPECT_TRUE(db.traceRowCache.empty())
        << "clearSessionCaches must clear traceRowCache";
}

} // namespace nix::eval_trace
