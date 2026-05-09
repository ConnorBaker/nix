#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <algorithm>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

static Dep::Key makeBlobTestKey(CanonicalQueryKind type, uint32_t sid, uint32_t kid)
{
    if (isStructuredQueryKind(type))
        return makeStructuredKeyForTest(type, sid, kid);
    if (isTraceContextQueryKind(type))
        return makeTraceContextKeyForTest(type, sid);
    if (type == CanonicalQueryKind::DerivedStorePath)
        return Dep::Key::makeDerivedStorePath(
            DepSourceId(sid),
            DerivedStorePathDepKeyId{DepKeyId(kid)});
    if (type == CanonicalQueryKind::StorePathAvailability)
        return Dep::Key::makeStorePathAvailability(
            DepSourceId(sid),
            StorePathAvailabilityDepKeyId{DepKeyId(kid)});
    if (type == CanonicalQueryKind::RuntimeFetchIdentity)
        return Dep::Key::makeRuntimeFetchIdentity(
            DepSourceId(sid),
            RuntimeFetchIdentityDepKeyId{DepKeyId(kid)});
    return makeSimpleKeyForTest(type, sid, kid);
}

// ── Serialization edge case tests ────────────────────────────────────

TEST_F(TraceStoreTest, Blob_StructuredContent_RoundTrips)
{
    // StructuredContent deps use digest values, like Content.
    // Verify they round-trip correctly through the factored serialization.
    std::vector<Dep::Key> keys;
    std::vector<Dep> deps;

    keys.push_back(makeBlobTestKey(CanonicalQueryKind::StructuredProjection, 1, 2));
    deps.push_back({keys.back(), DepHashValue(depHash("scalar-value"))});

    keys.push_back(makeBlobTestKey(CanonicalQueryKind::FileBytes, 3, 4));
    deps.push_back({keys.back(), DepHashValue(depHash("file-content"))});

    auto keysBlob = SqliteTraceStorage::serializeKeys(keys);
    auto db = makeDb();
    auto keysResult = db->deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 2u);

    auto valsBlob = SqliteTraceStorage::serializeValues(deps);
    auto valsResult = SqliteTraceStorage::deserializeValues(valsBlob.data(), valsBlob.size());
    ASSERT_EQ(valsResult.size(), 2u);

    // StructuredContent is a digest dep type
    EXPECT_EQ(keysResult[0].kind, CanonicalQueryKind::StructuredProjection);
    EXPECT_TRUE(std::holds_alternative<DepHash>(valsResult[0]));
    EXPECT_EQ(std::get<DepHash>(valsResult[0]), depHash("scalar-value"));

    // Content is also a digest dep type
    EXPECT_EQ(keysResult[1].kind, CanonicalQueryKind::FileBytes);
    EXPECT_TRUE(std::holds_alternative<DepHash>(valsResult[1]));
    EXPECT_EQ(std::get<DepHash>(valsResult[1]), depHash("file-content"));
}

TEST_F(TraceStoreTest, Blob_32ByteStringVsDigest_Disambiguated)
{
    // Critical test: a CopiedPath string value that is exactly 32 bytes
    // must be deserialized as a string, NOT as a EvalTraceHash.
    // The self-describing value format embeds type info at serialization time.
    std::string exactly32 = "abcdefghijklmnopqrstuvwxyz012345"; // 32 chars
    ASSERT_EQ(exactly32.size(), 32u);

    std::vector<Dep::Key> keys;
    std::vector<Dep> deps;

    // CopiedPath with exactly 32-byte string value
    keys.push_back(makeBlobTestKey(CanonicalQueryKind::DerivedStorePath, 1, 2));
    deps.push_back({keys.back(), DepHashValue(exactly32)});

    // Content with digest hash (also 32 bytes)
    keys.push_back(makeBlobTestKey(CanonicalQueryKind::FileBytes, 3, 4));
    deps.push_back({keys.back(), DepHashValue(depHash("data"))});

    auto keysBlob = SqliteTraceStorage::serializeKeys(keys);
    auto db = makeDb();
    auto keysResult = db->deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 2u);

    auto valsBlob = SqliteTraceStorage::serializeValues(deps);
    auto valsResult = SqliteTraceStorage::deserializeValues(valsBlob.data(), valsBlob.size());
    ASSERT_EQ(valsResult.size(), 2u);

    // CopiedPath with 32-byte value must deserialize as string
    EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[0]))
        << "32-byte CopiedPath value was incorrectly deserialized as EvalTraceHash";
    EXPECT_EQ(std::get<std::string>(valsResult[0]), exactly32);

    // Content with 32-byte value must deserialize as EvalTraceHash
    EXPECT_TRUE(std::holds_alternative<DepHash>(valsResult[1]))
        << "Content dep value was incorrectly deserialized as string";
}

TEST_F(TraceStoreTest, Blob_SingleEntry_RoundTrips)
{
    // Boundary: single dep in the serialization.
    std::vector<Dep::Key> keys;
    std::vector<Dep> deps;

    keys.push_back(makeBlobTestKey(CanonicalQueryKind::EnvironmentLookup, 42, 99));
    deps.push_back({keys.back(), DepHashValue(depHash("HOME=/home/user"))});

    auto keysBlob = SqliteTraceStorage::serializeKeys(keys);
    auto db = makeDb();
    auto keysResult = db->deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 1u);
    EXPECT_EQ(keysResult[0].kind, CanonicalQueryKind::EnvironmentLookup);
    EXPECT_EQ(keysResult[0].sourceId, DepSourceId(42));
    EXPECT_EQ(keysResult[0].simpleKeyId(), SimpleDepKeyId(99));

    auto valsBlob = SqliteTraceStorage::serializeValues(deps);
    auto valsResult = SqliteTraceStorage::deserializeValues(valsBlob.data(), valsBlob.size());
    ASSERT_EQ(valsResult.size(), 1u);
    EXPECT_EQ(valsResult[0], deps[0].hash);
}

TEST_F(TraceStoreTest, Blob_AllCQKVariants_RoundTrip)
{
    // Round-trip test covering every CQK variant in the CanonicalQueryKind enum.
    // This ensures isDigestDep stays consistent with serialization/deserialization.
    std::vector<Dep::Key> keys;
    std::vector<Dep> deps;

    auto addDigest = [&](CanonicalQueryKind type, uint32_t sid, uint32_t kid, std::string_view data) {
        auto key = makeBlobTestKey(type, sid, kid);
        keys.push_back(key);
        deps.push_back({key, DepHashValue(depHash(data))});
    };
    auto addString = [&](CanonicalQueryKind type, uint32_t sid, uint32_t kid, std::string_view data) {
        auto key = makeBlobTestKey(type, sid, kid);
        keys.push_back(key);
        deps.push_back({key, DepHashValue(std::string(data))});
    };

    addDigest(CanonicalQueryKind::FileBytes, 1, 1, "file");
    addDigest(CanonicalQueryKind::DirectoryEntries, 2, 2, "dir");
    addString(CanonicalQueryKind::ExistenceCheck, 3, 3, "type:1");
    addDigest(CanonicalQueryKind::EnvironmentLookup, 4, 4, "env");
    addString(CanonicalQueryKind::VolatileTime, 5, 5, "volatile");
    addDigest(CanonicalQueryKind::SessionSystemValue, 6, 6, "sys");
    addString(CanonicalQueryKind::RuntimeFetchIdentity, 7, 7, "url");
    addString(CanonicalQueryKind::DerivedStorePath, 8, 8, "/nix/store/test");
    addString(CanonicalQueryKind::VolatileExec, 9, 9, "volatile");
    addDigest(CanonicalQueryKind::NarIdentity, 10, 10, "nar");
    addDigest(CanonicalQueryKind::StructuredProjection, 11, 11, "struct");
    addDigest(CanonicalQueryKind::ImplicitStructure, 12, 12, "shape");
    addDigest(CanonicalQueryKind::RawBytes, 13, 13, "raw");
    addString(CanonicalQueryKind::StorePathAvailability, 14, 14, "valid");
    addDigest(CanonicalQueryKind::GitRevisionIdentity, 15, 15, "git");
    addDigest(CanonicalQueryKind::TraceValueContext, 16, 16, "value");
    addDigest(CanonicalQueryKind::TraceParentSlot, 17, 17, "parent");

    auto keysBlob = SqliteTraceStorage::serializeKeys(keys);
    auto db = makeDb();
    auto keysResult = db->deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), keys.size());

    auto valsBlob = SqliteTraceStorage::serializeValues(deps);
    auto valsResult = SqliteTraceStorage::deserializeValues(valsBlob.data(), valsBlob.size());
    ASSERT_EQ(valsResult.size(), deps.size());

    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(keysResult[i], keys[i]);
        if (isDigestDep(keysResult[i].kind)) {
            EXPECT_TRUE(std::holds_alternative<DepHash>(valsResult[i]))
                << "Dep type " << static_cast<int>(keysResult[i].kind)
                << " should deserialize as EvalTraceHash";
        } else {
            EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[i]))
                << "Dep type " << static_cast<int>(keysResult[i].kind)
                << " should deserialize as string";
        }
    }
}

// ── DepKeySets sharing tests ─────────────────────────────────────────

TEST_F(TraceStoreTest, Serialization_SharedKeys_DifferentValues)
{
    // Two traces with identical dep keys but different hash values should
    // share the same DepKeySets row (same struct_hash) but have different
    // Traces rows (different trace_hash / values_blob).
    auto db = makeDb();

    std::vector<Dep> depsV1 = {
        makeContentDep(pools(), "/a.nix", "content-v1"),
        makeEnvVarDep(pools(), "HOME", "/home/user1"),
    };
    std::vector<Dep> depsV2 = {
        makeContentDep(pools(), "/a.nix", "content-v2"),
        makeEnvVarDep(pools(), "HOME", "/home/user2"),
    };

    withExclusiveStore(*db, [&](const auto & ea) {
        auto r1 = db->record(ea, vpath({"attr"}), string_t{"result-1", {}}, depsV1);
        auto r2 = db->record(ea, vpath({"attr"}), string_t{"result-2", {}}, depsV2);

        // Different trace IDs (different hash values → different trace_hash)
        EXPECT_NE(r1.traceId, r2.traceId);

        // Both should load correctly
        auto loaded1 = db->loadFullTrace(ea, r1.traceId);
        auto loaded2 = db->loadFullTrace(ea, r2.traceId);
        EXPECT_EQ(loaded1->size(), 2u);
        EXPECT_EQ(loaded2->size(), 2u);

        // Verify the loaded values differ
        // Both have Content dep at index 0 (after sort); check the hash values differ
        bool foundDifference = false;
        for (size_t i = 0; i < loaded1->size(); i++) {
            if ((*loaded1)[i].hash != (*loaded2)[i].hash) {
                foundDifference = true;
                break;
            }
        }
        EXPECT_TRUE(foundDifference) << "Traces with same keys but different values should have different dep hashes";
    });
}

// ── Phase 5 cache/load tests ─────────────────────────────────────────
// Tests for the split trace header/full caches, currentNodeIndex, and
// in-memory recovery matching.

TEST_F(TraceStoreTest, TraceHeader_HashesPopulated_AfterRecord)
{
    // After record(), the metadata cache should contain the trace header and
    // the full-trace cache should contain deps.
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, vpath({"test"}), string_t{"value", {}},
            {makeContentDep(pools(), "/a.nix", "hello")});

        auto it = db->traceCache.find(result.traceId);
        ASSERT_NE(it, db->traceCache.end());
        EXPECT_NE(it->second.header.traceHash, TraceHash{});
        EXPECT_NE(it->second.header.keySetHash, DepKeySetHash{});
        EXPECT_NE(it->second.full, nullptr)
            << "traceCache.full after record should have deps populated";
    });
}

TEST_F(TraceStoreTest, TraceHeader_HashesPopulated_AfterEnsure)
{
    // ensureTraceHeader populates metadata only.
    // getCurrentTraceHash calls ensureTraceHeader internally.
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, vpath({"test"}), string_t{"value", {}},
            {makeContentDep(pools(), "/a.nix", "hello")});

        // Clear session caches to force re-read from DB
        db->traceCache.clear();

        // getCurrentTraceHash calls lookupCurrentNode → ensureTraceHeader
        auto hash = db->getCurrentTraceHash(ea, vpath({"test"}));
        ASSERT_TRUE(hash.has_value());

        auto it = db->traceCache.find(result.traceId);
        ASSERT_NE(it, db->traceCache.end());
        EXPECT_NE(it->second.header.traceHash, TraceHash{});
        EXPECT_NE(it->second.header.keySetHash, DepKeySetHash{});
        EXPECT_EQ(it->second.full, nullptr)
            << "ensureTraceHeader should not populate full deps";
    });
}

TEST_F(TraceStoreTest, LoadFullTrace_NonexistentTrace_NoCachePollution)
{
    // loadFullTrace on a nonexistent traceId should NOT create a cache entry.
    auto db = makeDb();
    TraceId bogusId(99999);

    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->loadFullTrace(ea, bogusId);
        EXPECT_TRUE(result->empty());
    });

    // Verify no cache pollution
    EXPECT_EQ(db->traceCache.find(bogusId), db->traceCache.end())
        << "loadFullTrace on nonexistent trace must not create a cache entry";
}

TEST_F(TraceStoreTest, LoadFullTrace_PopulatesHashes_Opportunistically)
{
    // loadFullTrace should populate traceHash + keySetHash + full deps in one shot.
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep(pools(), "/a.nix", "content")};
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, vpath({"test"}), string_t{"val", {}}, deps);

        // Clear cache, then use loadFullTrace (not ensureTraceHeader)
        db->traceCache.clear();

        auto loaded = db->loadFullTrace(ea, result.traceId);
        EXPECT_EQ(loaded->size(), 1u);

        auto it = db->traceCache.find(result.traceId);
        ASSERT_NE(it, db->traceCache.end());
        EXPECT_NE(it->second.header.traceHash, TraceHash{});
        EXPECT_NE(it->second.header.keySetHash, DepKeySetHash{});
        ASSERT_NE(it->second.full, nullptr);
        EXPECT_EQ(it->second.full->size(), 1u)
            << "loadFullTrace should populate full deps";
    });
}

TEST_F(TraceStoreTest, TraceDataCache_EnsureThenLoad_SingleDbQuery)
{
    // ensureTraceHeader (via getCurrentTraceHash) then loadFullTrace should
    // reuse cached hashes. Hash fields should be identical regardless of order.
    auto db = makeDb();
    std::vector<Dep> deps = {makeContentDep(pools(), "/a.nix", "content")};
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, vpath({"test"}), string_t{"val", {}}, deps);

        // Record populates everything; get hashes for comparison
        auto itAfterRecord = db->traceCache.find(result.traceId);
        ASSERT_NE(itAfterRecord, db->traceCache.end());
        auto expectedTraceHash = itAfterRecord->second.header.traceHash;

        // Clear and re-populate via ensureTraceHeader (getCurrentTraceHash)
        db->traceCache.clear();
        auto traceHash = db->getCurrentTraceHash(ea, vpath({"test"}));
        ASSERT_TRUE(traceHash.has_value());
        EXPECT_EQ(*traceHash, expectedTraceHash);

        // Verify deps not yet populated (lazy)
        auto itPartial = db->traceCache.find(result.traceId);
        ASSERT_NE(itPartial, db->traceCache.end());
        EXPECT_EQ(itPartial->second.full, nullptr)
            << "ensureTraceHeader should not populate full deps";

        // Now loadFullTrace should find the cache entry and populate full deps
        auto loaded = db->loadFullTrace(ea, result.traceId);
        EXPECT_EQ(loaded->size(), 1u);

        auto it = db->traceCache.find(result.traceId);
        ASSERT_NE(it, db->traceCache.end());
        EXPECT_EQ(it->second.header.traceHash, expectedTraceHash);
        ASSERT_NE(it->second.full, nullptr);
        EXPECT_EQ(it->second.full->size(), 1u);
    });
}

TEST_F(TraceStoreTest, Session_CurrentNodeIndex_HitAfterRecord)
{
    // After record(), lookupCurrentNode should hit currentNodeIndex (no DB).
    auto db = makeDb();
    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, vpath({"myattr"}), string_t{"result", {}},
            {makeEnvVarDep(pools(), "FOO", "bar")});
    });

    // currentNodeIndex should be populated from record()
    EXPECT_EQ(db->currentNodeIndex.count(vpath({"myattr"})), 1u)
        << "record() should populate currentNodeIndex";

    // Verify the cached row has correct traceId
    auto & row = db->currentNodeIndex[vpath({"myattr"})];
    EXPECT_EQ(row.traceId, result.traceId);
}

TEST_F(TraceStoreTest, Session_CurrentNodeIndex_HitAfterLookup)
{
    // After a lookupCurrentNode miss that hits DB, cache should be populated.
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"attr"}), string_t{"val", {}}, {});

        // Clear currentNodeIndex (simulate fresh session lookup, not record)
        db->currentNodeIndex.clear();

        // First lookup goes to DB, populates cache
        EXPECT_TRUE(db->attrExists(ea, vpath({"attr"})));
        EXPECT_EQ(db->currentNodeIndex.count(vpath({"attr"})), 1u)
            << "lookupCurrentNode should populate currentNodeIndex on DB hit";
    });
}

TEST_F(TraceStoreTest, Session_CurrentNodeIndex_MissDoesNotCache)
{
    // Looking up a nonexistent attr should NOT create a cache entry.
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        EXPECT_FALSE(db->attrExists(ea, vpath({"nonexistent"})));
        EXPECT_EQ(db->currentNodeIndex.count(vpath({"nonexistent"})), 0u)
            << "lookupCurrentNode miss should not create a cache entry";
    });
}

TEST_F(TraceStoreTest, Session_GetCurrentTraceHash_CachedAfterFirstCall)
{
    // getCurrentTraceHash should cache its result — second call hits cache.
    auto db = makeDb();
    // Record an attr using AttrPathId
    auto pathId = vpath({"packages", "hello"});
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, pathId, string_t{"result", {}},
            {makeContentDep(pools(), "/hello.nix", "v1")});

        auto hash1 = db->getCurrentTraceHash(ea, pathId);
        ASSERT_TRUE(hash1.has_value());

        // Second call should hit session cache (currentNodeIndex + traceHeaderCache).
        auto hash2 = db->getCurrentTraceHash(ea, pathId);
        ASSERT_TRUE(hash2.has_value());
        EXPECT_EQ(*hash1, *hash2)
            << "getCurrentTraceHash should return same value from cache";
    });
}

TEST_F(TraceStoreTest, Session_CurrentNodeIndex_InvalidatedOnRecovery)
{
    // When recovery succeeds, currentNodeIndex should be updated to reflect
    // the recovered trace. Subsequent lookups should return the new trace.
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record version 1
        std::vector<Dep> depsV1 = {makeEnvVarDep(pools(), "MY_VAR", "v1")};
        db->record(ea, vpath({"attr"}), string_t{"result-v1", {}}, depsV1);

        // Record version 2 (different deps → different trace)
        std::vector<Dep> depsV2 = {makeEnvVarDep(pools(), "MY_VAR", "v2")};
        db->record(ea, vpath({"attr"}), string_t{"result-v2", {}}, depsV2);
    });

    // Clear session caches to simulate new session
    recreateDb(db);

    // Set env to v1 (mismatches current trace v2, should recover to v1)
    setenv("MY_VAR", "v1", 1);
    auto verifyResult = test::TraceStorageTestAccess::verify(*db, vpath({"attr"}), state);
    ASSERT_TRUE(verifyResult.has_value());

    // Verify the currentNodeIndex was updated
    auto attrId = vpath({"attr"});
    EXPECT_EQ(db->currentNodeIndex.count(attrId), 1u);
    auto rowIt = db->currentNodeIndex.find(attrId);
    ASSERT_NE(rowIt, db->currentNodeIndex.end());
    auto & row = rowIt->second;
    EXPECT_EQ(row.traceId, verifyResult->traceId)
        << "currentNodeIndex should reflect the recovered trace";

    // Verify getCurrentTraceHash also reflects recovery
    auto traceHash = withExclusiveStore(*db, [&](const auto & ea) {
        return db->getCurrentTraceHash(ea, vpath({"attr"}));
    });
    ASSERT_TRUE(traceHash.has_value());
    auto cacheIt = db->traceCache.find(verifyResult->traceId);
    if (cacheIt != db->traceCache.end()) {
        EXPECT_EQ(*traceHash, cacheIt->second.header.traceHash)
            << "getCurrentTraceHash should match recovered trace's hash";
    }

    unsetenv("MY_VAR");
}

TEST_F(TraceStoreTest, Recovery_InMemoryDirect_HashMatch)
{
    // Recovery should use in-memory trace_hash matching (no per-group DB lookup).
    // This test verifies the functional correctness of in-memory direct hash recovery.
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record two versions
        setenv("INTEST", "alpha", 1);
        std::vector<Dep> depsA = {makeEnvVarDep(pools(), "INTEST", "alpha")};
        db->record(ea, vpath({"attr"}), string_t{"result-alpha", {}}, depsA);

        setenv("INTEST", "beta", 1);
        std::vector<Dep> depsB = {makeEnvVarDep(pools(), "INTEST", "beta")};
        db->record(ea, vpath({"attr"}), string_t{"result-beta", {}}, depsB);
    });

    // Clear session caches
    recreateDb(db);

    // Revert env to alpha → direct hash recovery should find the alpha trace
    setenv("INTEST", "alpha", 1);
    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"attr"}), state);
    ASSERT_TRUE(result.has_value());

    // Should have recovered to alpha's result
    auto * strResult = std::get_if<string_t>(&result->value);
    ASSERT_NE(strResult, nullptr);
    EXPECT_EQ(strResult->first, "result-alpha");

    unsetenv("INTEST");
}

TEST_F(TraceStoreTest, Recovery_InMemoryStructural_VariantMatch)
{
    // Structural variant recovery should also use in-memory matching.
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Version 1: depends on VAR_A only
        setenv("SVR_A", "a1", 1);
        std::vector<Dep> depsV1 = {makeEnvVarDep(pools(), "SVR_A", "a1")};
        db->record(ea, vpath({"attr"}), string_t{"r1", {}}, depsV1);

        // Version 2: depends on VAR_A + VAR_B (different dep structure)
        setenv("SVR_A", "a2", 1);
        setenv("SVR_B", "b2", 1);
        std::vector<Dep> depsV2 = {
            makeEnvVarDep(pools(), "SVR_A", "a2"),
            makeEnvVarDep(pools(), "SVR_B", "b2"),
        };
        db->record(ea, vpath({"attr"}), string_t{"r2", {}}, depsV2);
    });

    // Clear caches
    recreateDb(db);

    // Set env to match v1 (single dep)
    setenv("SVR_A", "a1", 1);
    unsetenv("SVR_B");

    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"attr"}), state);
    ASSERT_TRUE(result.has_value());

    auto * strResult = std::get_if<string_t>(&result->value);
    ASSERT_NE(strResult, nullptr);
    EXPECT_EQ(strResult->first, "r1")
        << "Structural variant recovery should find the matching trace";

    unsetenv("SVR_A");
}

} // namespace nix::eval_trace
