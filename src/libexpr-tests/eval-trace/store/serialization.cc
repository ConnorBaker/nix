#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/util/compression.hh"

#include <algorithm>
#include <cstring>
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

// ── Bootstrap session-key isolation tests (BSàlC: separate trace stores per task) ──

TEST_F(TraceStoreTest, Session_DifferentBootstrapKeys_Isolated)
{
    auto key111 = SemanticSessionKey::fromSerialized("test-serialization:111");
    auto key222 = SemanticSessionKey::fromSerialized("test-serialization:222");

    // Two different bootstrap session keys should have isolated trace namespaces.

    {
        SqliteTraceStorage db1(state.symbols, state.tracingPools(), state.vocabStore(), key111);
        withExclusiveStore(db1, [&](const auto & ea) {
            db1.record(ea, vpath({"pkg"}), string_t{"v1", {}}, {});
        });
    }
    {
        SqliteTraceStorage db2(state.symbols, state.tracingPools(), state.vocabStore(), key222);
        withExclusiveStore(db2, [&](const auto & ea) {
            db2.record(ea, vpath({"pkg"}), string_t{"v2", {}}, {});
        });
    }

    {
        SqliteTraceStorage db1(state.symbols, state.tracingPools(), state.vocabStore(), key111);
        auto r1 = test::TraceStorageTestAccess::verify(db1, vpath({"pkg"}), state);
        ASSERT_TRUE(r1.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r1->value));
        EXPECT_EQ(std::get<string_t>(r1->value).first, "v1");
    }
    {
        SqliteTraceStorage db2(state.symbols, state.tracingPools(), state.vocabStore(), key222);
        auto r2 = test::TraceStorageTestAccess::verify(db2, vpath({"pkg"}), state);
        ASSERT_TRUE(r2.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r2->value));
        EXPECT_EQ(std::get<string_t>(r2->value).first, "v2");
    }
}

TEST_F(TraceStoreTest, Serialization_NullByteAttrPath_RoundTrips)
{
    auto db = makeDb();

    // Multi-component attr path: packages.x86_64-linux.hello
    auto fullPath = vpath({"packages", "x86_64-linux", "hello"});

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, fullPath, string_t{"val", {}}, {});
        EXPECT_TRUE(db->attrExists(ea, fullPath));
        EXPECT_FALSE(db->attrExists(ea, vpath({"packages"})));
    });
}

TEST_F(TraceStoreTest, Serialization_EmptyAttrPath_RoundTrips)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"root-val", {}}, {});
        EXPECT_TRUE(db->attrExists(ea, rootPath()));
    });
}

TEST_F(TraceStoreTest, Serialization_MultipleEntries_Stress)
{
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record 100 trace entries
        for (int i = 0; i < 100; i++) {
            auto name = "stress-" + std::to_string(i);
            db->record(ea, vpath({name}), int_t{NixInt{i}}, {});
        }

        EXPECT_TRUE(db->attrExists(ea, vpath({"stress-0"})));
        EXPECT_TRUE(db->attrExists(ea, vpath({"stress-99"})));
        EXPECT_FALSE(db->attrExists(ea, vpath({"stress-100"})));
    });
}

// ── BLOB serialization roundtrip tests (keys_blob + values_blob encoding) ──

TEST_F(TraceStoreTest, Blob_Empty_RoundTrips)
{
    std::vector<Dep::Key> keys;
    auto keysBlob = SqliteTraceStorage::serializeKeys(keys);
    EXPECT_TRUE(keysBlob.empty());
    auto db = makeDb();
    auto keysResult = db->deserializeKeys(keysBlob.data(), keysBlob.size());
    EXPECT_TRUE(keysResult.empty());

    std::vector<Dep> deps;
    auto valsBlob = SqliteTraceStorage::serializeValues(deps);
    EXPECT_TRUE(valsBlob.empty());
    auto valsResult = SqliteTraceStorage::deserializeValues(valsBlob.data(), valsBlob.size());
    EXPECT_TRUE(valsResult.empty());
}

TEST_F(TraceStoreTest, Blob_DigestDeps_RoundTrips)
{
    std::vector<Dep::Key> keys;
    std::vector<Dep> deps;
    for (int i = 0; i < 5; i++) {
        auto hash = depHash("content-" + std::to_string(i));
        auto key = makeBlobTestKey(CanonicalQueryKind::FileBytes,
            static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i + 100));
        keys.push_back(key);
        deps.push_back({key, DepHashValue(hash)});
    }

    auto keysBlob = SqliteTraceStorage::serializeKeys(keys);
    EXPECT_FALSE(keysBlob.empty());
    auto db = makeDb();
    auto keysResult = db->deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 5u);

    auto valsBlob = SqliteTraceStorage::serializeValues(deps);
    EXPECT_FALSE(valsBlob.empty());
    auto valsResult = SqliteTraceStorage::deserializeValues(valsBlob.data(), valsBlob.size());
    ASSERT_EQ(valsResult.size(), 5u);

    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(keysResult[i].kind, CanonicalQueryKind::FileBytes);
        EXPECT_EQ(keysResult[i].sourceId, DepSourceId(static_cast<uint32_t>(i + 1)));
        EXPECT_EQ(keysResult[i].simpleKeyId(), SimpleDepKeyId(static_cast<uint32_t>(i + 100)));
        EXPECT_EQ(valsResult[i], deps[i].hash);
    }
}

TEST_F(TraceStoreTest, Blob_MixedDeps_RoundTrips)
{
    std::vector<Dep::Key> keys;
    std::vector<Dep> deps;

    // Digest dep (Content — file content oracle)
    keys.push_back(makeBlobTestKey(CanonicalQueryKind::FileBytes, 1, 2));
    deps.push_back({keys.back(), DepHashValue(depHash("file-data"))});

    // String hash dep (CopiedPath — store path oracle)
    keys.push_back(makeBlobTestKey(CanonicalQueryKind::DerivedStorePath, 3, 4));
    deps.push_back({keys.back(), DepHashValue(std::string("/nix/store/aaaa-test"))});

    // Digest dep (EnvVar — environment oracle)
    keys.push_back(makeBlobTestKey(CanonicalQueryKind::EnvironmentLookup, 5, 6));
    deps.push_back({keys.back(), DepHashValue(depHash("env-val"))});

    // String hash dep (Existence — filesystem oracle)
    keys.push_back(makeBlobTestKey(CanonicalQueryKind::ExistenceCheck, 7, 8));
    deps.push_back({keys.back(), DepHashValue(std::string("missing"))});

    auto keysBlob = SqliteTraceStorage::serializeKeys(keys);
    auto db = makeDb();
    auto keysResult = db->deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 4u);

    auto valsBlob = SqliteTraceStorage::serializeValues(deps);
    auto valsResult = SqliteTraceStorage::deserializeValues(valsBlob.data(), valsBlob.size());
    ASSERT_EQ(valsResult.size(), 4u);

    // Content oracle: digest key + value
    EXPECT_EQ(keysResult[0].kind, CanonicalQueryKind::FileBytes);
    EXPECT_EQ(keysResult[0].sourceId, DepSourceId(1));
    EXPECT_EQ(keysResult[0].simpleKeyId(), SimpleDepKeyId(2));
    EXPECT_TRUE(std::holds_alternative<DepHash>(valsResult[0]));

    // CopiedPath oracle: string (not a digest, so deserialized as string)
    EXPECT_EQ(keysResult[1].kind, CanonicalQueryKind::DerivedStorePath);
    EXPECT_EQ(keysResult[1].sourceId, DepSourceId(3));
    EXPECT_EQ(keysResult[1].derivedStorePathKeyId(), DerivedStorePathDepKeyId{DepKeyId(4)});
    EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[1]));
    EXPECT_EQ(std::get<std::string>(valsResult[1]), "/nix/store/aaaa-test");

    // EnvVar oracle: digest
    EXPECT_EQ(keysResult[2].kind, CanonicalQueryKind::EnvironmentLookup);
    EXPECT_TRUE(std::holds_alternative<DepHash>(valsResult[2]));

    // Existence oracle: string
    EXPECT_EQ(keysResult[3].kind, CanonicalQueryKind::ExistenceCheck);
    EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[3]));
    EXPECT_EQ(std::get<std::string>(valsResult[3]), "missing");
}

TEST_F(TraceStoreTest, Blob_LargeSet_RoundTrips)
{
    std::vector<Dep::Key> keys;
    std::vector<Dep> deps;
    for (uint32_t i = 0; i < 10000; i++) {
        auto hash = depHash("content-" + std::to_string(i));
        auto key = makeBlobTestKey(CanonicalQueryKind::FileBytes, i, i + 50000);
        keys.push_back(key);
        deps.push_back({key, DepHashValue(hash)});
    }

    auto keysBlob = SqliteTraceStorage::serializeKeys(keys);
    // keys_blob is zstd-compressed; verify smaller than raw (10000 * 9 bytes)
    EXPECT_GT(keysBlob.size(), 0u);
    EXPECT_LT(keysBlob.size(), 10000u * 9u);

    auto db = makeDb();
    auto keysResult = db->deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 10000u);

    auto valsBlob = SqliteTraceStorage::serializeValues(deps);
    // values_blob is zstd-compressed; random-looking digests don't compress well,
    // so just verify non-empty (zstd overhead may exceed raw size for random data)
    EXPECT_GT(valsBlob.size(), 0u);

    auto valsResult = SqliteTraceStorage::deserializeValues(valsBlob.data(), valsBlob.size());
    ASSERT_EQ(valsResult.size(), 10000u);

    // Spot-check first, middle, and last
    EXPECT_EQ(keysResult[0].sourceId, DepSourceId(0));
    EXPECT_EQ(keysResult[0].simpleKeyId(), SimpleDepKeyId(50000));
    EXPECT_EQ(keysResult[5000].sourceId, DepSourceId(5000));
    EXPECT_EQ(keysResult[5000].simpleKeyId(), SimpleDepKeyId(55000));
    EXPECT_EQ(keysResult[9999].sourceId, DepSourceId(9999));
    EXPECT_EQ(keysResult[9999].simpleKeyId(), SimpleDepKeyId(59999));

    // Verify hashes match
    for (uint32_t i = 0; i < 10000; i++) {
        EXPECT_EQ(valsResult[i], deps[i].hash) << "Hash mismatch at index " << i;
    }
}

// ── Grouped format validation tests ──────────────────────────────────

TEST_F(TraceStoreTest, Serialization_DigestOnly_GroupedFormat)
{
    std::vector<Dep> deps;
    for (int i = 0; i < 3; i++) {
        auto key = makeBlobTestKey(CanonicalQueryKind::FileBytes, static_cast<uint32_t>(i), static_cast<uint32_t>(i));
        deps.push_back({key, DepHashValue(depHash("b3-" + std::to_string(i)))});
    }
    auto blob = SqliteTraceStorage::serializeValues(deps);
    auto result = SqliteTraceStorage::deserializeValues(blob.data(), blob.size());
    ASSERT_EQ(result.size(), 3u);
    for (int i = 0; i < 3; i++) {
        EXPECT_TRUE(std::holds_alternative<DepHash>(result[i]));
        EXPECT_EQ(result[i], deps[i].hash);
    }
}

TEST_F(TraceStoreTest, Serialization_StringOnly_GroupedFormat)
{
    std::vector<Dep> deps;
    for (int i = 0; i < 3; i++) {
        auto key = makeBlobTestKey(CanonicalQueryKind::ExistenceCheck, static_cast<uint32_t>(i), static_cast<uint32_t>(i));
        deps.push_back({key, DepHashValue(std::string("str-" + std::to_string(i)))});
    }
    auto blob = SqliteTraceStorage::serializeValues(deps);
    auto result = SqliteTraceStorage::deserializeValues(blob.data(), blob.size());
    ASSERT_EQ(result.size(), 3u);
    for (int i = 0; i < 3; i++) {
        EXPECT_TRUE(std::holds_alternative<std::string>(result[i]));
        EXPECT_EQ(std::get<std::string>(result[i]), "str-" + std::to_string(i));
    }
}

TEST_F(TraceStoreTest, Serialization_TruncatedBlob_Throws)
{
    std::vector<Dep> deps;
    auto key = makeBlobTestKey(CanonicalQueryKind::FileBytes, 1, 1);
    deps.push_back({key, DepHashValue(depHash("data"))});
    auto blob = SqliteTraceStorage::serializeValues(deps);

    // Truncate the compressed blob
    std::vector<uint8_t> truncated(blob.begin(), blob.begin() + blob.size() / 2);
    EXPECT_THROW(SqliteTraceStorage::deserializeValues(truncated.data(), truncated.size()), Error);
}

TEST_F(TraceStoreTest, Serialization_BadEntryType_Throws)
{
    // Manually craft a blob with an invalid entry type byte (2)
    std::vector<uint8_t> raw;
    raw.insert(raw.end(), {'v', 'a', 'l', 's', '2'});
    raw.push_back(evalTraceHashAlgorithmTag(getEvalTraceHashAlgorithm()));
    uint32_t numEntries = 1;
    uint8_t buf[4];
    std::memcpy(buf, &numEntries, 4);
    raw.insert(raw.end(), buf, buf + 4);
    raw.push_back(2);  // invalid type byte

    auto compressed = nix::compress(CompressionAlgo::zstd,
        {reinterpret_cast<const char *>(raw.data()), raw.size()}, false, 1);
    std::vector<uint8_t> blob(compressed.begin(), compressed.end());
    EXPECT_THROW(SqliteTraceStorage::deserializeValues(blob.data(), blob.size()), Error);
}

TEST_F(TraceStoreTest, Serialization_CountMismatch_Throws)
{
    // Craft a blob where entry types say 1 digest, but digest count says 2
    std::vector<uint8_t> raw;
    raw.insert(raw.end(), {'v', 'a', 'l', 's', '2'});
    raw.push_back(evalTraceHashAlgorithmTag(getEvalTraceHashAlgorithm()));
    uint32_t numEntries = 1;
    uint8_t buf[4];
    std::memcpy(buf, &numEntries, 4);
    raw.insert(raw.end(), buf, buf + 4);
    raw.push_back(1);  // 1 digest entry

    uint32_t numDigests = 2;  // but claim 2 digest values
    std::memcpy(buf, &numDigests, 4);
    raw.insert(raw.end(), buf, buf + 4);

    auto compressed = nix::compress(CompressionAlgo::zstd,
        {reinterpret_cast<const char *>(raw.data()), raw.size()}, false, 1);
    std::vector<uint8_t> blob(compressed.begin(), compressed.end());
    EXPECT_THROW(SqliteTraceStorage::deserializeValues(blob.data(), blob.size()), Error);
}

TEST_F(TraceStoreTest, Serialization_TrailingBytes_Throws)
{
    // Serialize normally, then decompress, append extra bytes, recompress
    std::vector<Dep> deps;
    auto key = makeBlobTestKey(CanonicalQueryKind::ExistenceCheck, 1, 1);
    deps.push_back({key, DepHashValue(std::string("ok"))});
    auto blob = SqliteTraceStorage::serializeValues(deps);

    // Decompress, add trailing bytes, recompress
    auto decompressed = nix::decompress("zstd",
        {reinterpret_cast<const char *>(blob.data()), blob.size()});
    std::string modified = decompressed + "\xff\xff";
    auto recompressed = nix::compress(CompressionAlgo::zstd,
        {modified.data(), modified.size()}, false, 1);
    std::vector<uint8_t> badBlob(recompressed.begin(), recompressed.end());
    EXPECT_THROW(SqliteTraceStorage::deserializeValues(badBlob.data(), badBlob.size()), Error);
}

// ── Dep storage tests (content-addressed DepKeySets dedup) ────────────

TEST_F(TraceStoreTest, Serialization_DepKeySets_SiblingOverlap)
{
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Parent with 0 deps (FullAttrs pattern — trace records only child names)
        db->record(ea, rootPath(), makeNull(), {});

        // Child A trace with 100 deps (own deps only, no parent inheritance)
        std::vector<Dep> depsA;
        for (int i = 0; i < 100; i++) {
            depsA.push_back(makeContentDep(pools(), "/file-" + std::to_string(i) + ".nix",
                                           "content-" + std::to_string(i)));
        }
        auto childA = db->record(ea, vpath({"a"}), string_t{"val-a", {}}, depsA);

        // Child B trace with 95 overlapping deps + 5 different hashes
        std::vector<Dep> depsB;
        for (int i = 0; i < 95; i++) {
            depsB.push_back(makeContentDep(pools(), "/file-" + std::to_string(i) + ".nix",
                                           "content-" + std::to_string(i)));
        }
        for (int i = 95; i < 100; i++) {
            depsB.push_back(makeContentDep(pools(), "/file-" + std::to_string(i) + ".nix",
                                           "content-modified-" + std::to_string(i)));
        }
        auto childB = db->record(ea, vpath({"b"}), string_t{"val-b", {}}, depsB);

        // Both traces should load correctly with full deps
        auto loadedA = db->loadFullTrace(ea, childA.traceId);
        auto loadedB = db->loadFullTrace(ea, childB.traceId);
        EXPECT_EQ(loadedA->size(), 100u);
        EXPECT_EQ(loadedB->size(), 100u);
    });
}

TEST_F(TraceStoreTest, Record_SeparatedDeps_NoParentInheritance)
{
    // Parent with deps, child with own deps — each stores only its own.
    // Tests that parent deps are NOT merged into child traces.
    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Parent with 0 deps (FullAttrs pattern — trace records only child names)
        db->record(ea, rootPath(), makeNull(), {});

        // Child A trace with 100 deps
        std::vector<Dep> depsA;
        for (int i = 0; i < 100; i++) {
            depsA.push_back(makeContentDep(pools(), "/f" + std::to_string(i) + ".nix",
                                           "c" + std::to_string(i)));
        }
        auto childA = db->record(ea, vpath({"a"}), int_t{NixInt{1}}, depsA);

        // Child B trace with 99 overlapping + 1 different dep hash
        std::vector<Dep> depsB;
        for (int i = 0; i < 99; i++) {
            depsB.push_back(makeContentDep(pools(), "/f" + std::to_string(i) + ".nix",
                                           "c" + std::to_string(i)));
        }
        depsB.push_back(makeContentDep(pools(), "/f99.nix", "c99-modified"));
        auto childB = db->record(ea, vpath({"b"}), int_t{NixInt{2}}, depsB);

        // Verify both traces load correctly
        auto loadedA = db->loadFullTrace(ea, childA.traceId);
        auto loadedB = db->loadFullTrace(ea, childB.traceId);
        EXPECT_EQ(loadedA->size(), 100u);
        EXPECT_EQ(loadedB->size(), 100u);

        // Verify B trace has the modified dep hash
        bool foundModified = false;
        for (const auto & idep : *loadedB) {
            auto dep = db->resolveDep(idep);
            if (dep.key == "/f99.nix") {
                auto h = depHash("c99-modified");
                EXPECT_EQ(dep.expectedHash, DepHashValue(h));
                foundModified = true;
            }
        }
        EXPECT_TRUE(foundModified);
    });
}

// ── Batch verification + dep hash caching tests (Shake: unchanged check) ──

TEST_F(TraceStoreTest, Verify_BatchValidation_ComputesAllDepHashes)
{
    // Record trace with 50 deps where dep #25 is stale.
    // Batch verification should compute ALL 50 current hashes (not stop at #25).
    ScopedEnvVar env0("NIX_BATCH_0", "v0");

    auto db = makeDb();
    std::vector<Dep> deps;
    for (int i = 0; i < 50; i++) {
        auto key = "NIX_BATCH_" + std::to_string(i);
        auto value = "v" + std::to_string(i);
        setenv(key.c_str(), value.c_str(), 1);
        deps.push_back(makeEnvVarDep(pools(), key, value));
    }
    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, rootPath(), makeNull(), deps);
    });

    // Change dep #25 to invalidate its hash
    setenv("NIX_BATCH_25", "CHANGED", 1);
    recreateDb(db);

    // Verify — should fail but cache ALL 50 current dep hashes for reuse in recovery
    VerificationSession session;
    bool valid = test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state, session);
    EXPECT_FALSE(valid);

    // All 50 dep keys should be in the session dep-hash memo for constructive recovery
    EXPECT_GE(session.depHashCount(), 50u);

    // Clean up env vars
    for (int i = 0; i < 50; i++) {
        auto key = "NIX_BATCH_" + std::to_string(i);
        unsetenv(key.c_str());
    }
}

TEST_F(TraceStoreTest, Verify_HashCaching_ReusesMemoizedHashes)
{
    // Record trace with deps, change 1, trigger verification failure -> constructive recovery.
    // Assert that recovery reuses cached dep hashes from verification (Shake: unchanged check reuse).
    ScopedEnvVar env1("NIX_HASHCACHE_A", "valA");
    ScopedEnvVar env2("NIX_HASHCACHE_B", "valB");

    auto db = makeDb();

    withExclusiveStore(*db, [&](const auto & ea) {
        // Version 1: deps A and B
        std::vector<Dep> deps1 = {
            makeEnvVarDep(pools(), "NIX_HASHCACHE_A", "valA"),
            makeEnvVarDep(pools(), "NIX_HASHCACHE_B", "valB"),
        };
        db->record(ea, rootPath(), string_t{"result-1", {}}, deps1);

        // Version 2: A changed, B same
        setenv("NIX_HASHCACHE_A", "valA2", 1);
        std::vector<Dep> deps2 = {
            makeEnvVarDep(pools(), "NIX_HASHCACHE_A", "valA2"),
            makeEnvVarDep(pools(), "NIX_HASHCACHE_B", "valB"),
        };
        db->record(ea, rootPath(), string_t{"result-2", {}}, deps2);
    });

    // Revert A
    setenv("NIX_HASHCACHE_A", "valA", 1);
    recreateDb(db);

    // verify should fail verification (trace 2 deps don't match) then constructively recover to result-1
    VerificationSession session;
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state, session);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result-1", {}}, result->value, state.symbols);

    // The shared session should retain dep hashes computed during batch verification
    EXPECT_GE(session.depHashCount(), 2u);
}

TEST_F(TraceStoreTest, Verify_SharedTrace_ValidatedOnce)
{
    // Record 5 siblings sharing the same trace.
    // Verify all 5. Shared trace should be verified only once (Salsa: memoized verification).
    ScopedEnvVar env("NIX_BASE_VALID", "ok");

    auto db = makeDb();
    std::vector<Dep> sharedDeps = {makeEnvVarDep(pools(), "NIX_BASE_VALID", "ok")};

    withExclusiveStore(*db, [&](const auto & ea) {
        // Record 5 attrs with identical deps (all share the same trace)
        for (int i = 0; i < 5; i++) {
            auto name = "sibling-" + std::to_string(i);
            db->record(ea, vpath({name}), int_t{NixInt{i}}, sharedDeps);
        }
    });

    recreateDb(db);

    VerificationSession session;
    // Verify all 5 — trace verified on first access, session-memoized for rest
    for (int i = 0; i < 5; i++) {
        auto name = "sibling-" + std::to_string(i);
        auto result = test::TraceStorageTestAccess::verify(*db, vpath({name}), state, session);
        ASSERT_TRUE(result.has_value()) << "Sibling " << i << " failed";
        ASSERT_TRUE(std::holds_alternative<int_t>(result->value));
        EXPECT_EQ(std::get<int_t>(result->value).x.value, i);
    }

    // The shared session should memoize the shared trace after the first verification
    EXPECT_FALSE(session.verifiedTraceIds.empty());
}

// ── Full end-to-end record + verification roundtrip ──────────────────

TEST_F(TraceStoreTest, Verify_WarmRoundtrip_AllAttrsServed)
{
    // End-to-end: record traces, then verification retrieves correctly.
    // Use EnvVar deps so verification can actually check them (no files needed).
    ScopedEnvVar env1("NIX_DW_SHARED", "stable");
    ScopedEnvVar env2("NIX_DW_A", "a-val");
    ScopedEnvVar env3("NIX_DW_B", "b-val");

    auto db = makeDb();

    // 3 attrs with overlapping deps (all env vars — verifiable)
    auto sharedDep = makeEnvVarDep(pools(), "NIX_DW_SHARED", "stable");

    withExclusiveStore(*db, [&](const auto & ea) {
        // Attr 1: shared + 1 unique
        std::vector<Dep> deps1 = {sharedDep, makeEnvVarDep(pools(), "NIX_DW_A", "a-val")};
        db->record(ea, vpath({"a"}), string_t{"val-a", {}}, deps1);

        // Attr 2: shared + 1 different unique
        std::vector<Dep> deps2 = {sharedDep, makeEnvVarDep(pools(), "NIX_DW_B", "b-val")};
        db->record(ea, vpath({"b"}), string_t{"val-b", {}}, deps2);

        // Attr 3: shared only
        std::vector<Dep> deps3 = {sharedDep};
        db->record(ea, vpath({"c"}), string_t{"val-c", {}}, deps3);
    });

    recreateDb(db);

    // Verify all 3 traces (BSàlC: verify trace -> serve cached result)
    auto ra = test::TraceStorageTestAccess::verify(*db, vpath({"a"}), state);
    ASSERT_TRUE(ra.has_value());
    assertCachedResultEquals(string_t{"val-a", {}}, ra->value, state.symbols);

    auto rb = test::TraceStorageTestAccess::verify(*db, vpath({"b"}), state);
    ASSERT_TRUE(rb.has_value());
    assertCachedResultEquals(string_t{"val-b", {}}, rb->value, state.symbols);

    auto rc = test::TraceStorageTestAccess::verify(*db, vpath({"c"}), state);
    ASSERT_TRUE(rc.has_value());
    assertCachedResultEquals(string_t{"val-c", {}}, rc->value, state.symbols);
}

// ── ParentSlot recovery regression tests ─────────────────────────────
//
// When a parent trace is recovered during a cold run, children with
// ParentSlot deps must still be able to recover via direct hash matching.
// These tests verify that recovery works correctly when the parent
// trace is recovered to a previous version.

TEST_F(TraceStoreTest, Recovery_ParentSlot_ParentRecovered)
{
    // ParentSlot dep survives parent recovery via direct hash matching.
    auto db = makeDb();
    auto parentPath = vpath({"parent"});
    auto childPath = vpath({"parent", "child"});

    withExclusiveStore(*db, [&](const auto & ea) {
        // Version 1
        setenv("NIX_PSLOT_VAR", "v1", 1);
        db->record(ea, parentPath, string_t{"parent-v1", {}},
            {makeEnvVarDep(pools(), "NIX_PSLOT_VAR", "v1")});

        auto parentTraceHash = db->getCurrentTraceHash(ea, parentPath);
        ASSERT_TRUE(parentTraceHash.has_value());

        // Child depends on parent via ParentSlot
        Dep parentSlotDep = Dep::makeParentSlot(
            ParentSlot{parentPath},
            DepHashValue(DepHash{parentTraceHash->value}));
        db->record(ea, childPath, string_t{"child-v1", {}}, {parentSlotDep});

        // Version 2
        setenv("NIX_PSLOT_VAR", "v2", 1);
        db->record(ea, parentPath, string_t{"parent-v2", {}},
            {makeEnvVarDep(pools(), "NIX_PSLOT_VAR", "v2")});

        auto parentTraceHashV2 = db->getCurrentTraceHash(ea, parentPath);
        ASSERT_TRUE(parentTraceHashV2.has_value());

        Dep parentSlotDepV2 = Dep::makeParentSlot(
            ParentSlot{parentPath},
            DepHashValue(DepHash{parentTraceHashV2->value}));
        db->record(ea, childPath, string_t{"child-v2", {}}, {parentSlotDepV2});
    });

    // Revert to v1 state
    setenv("NIX_PSLOT_VAR", "v1", 1);
    recreateDb(db);

    // Verify parent first — recovery to v1
    auto parentResult = test::TraceStorageTestAccess::verify(*db, parentPath, state);
    ASSERT_TRUE(parentResult.has_value());
    assertCachedResultEquals(string_t{"parent-v1", {}}, parentResult->value, state.symbols);

    // Verify child — should recover to v1 via ParentSlot dep
    auto childResult = test::TraceStorageTestAccess::verify(*db, childPath, state);
    ASSERT_TRUE(childResult.has_value())
        << "Child with ParentSlot should recover when parent is recovered";
    assertCachedResultEquals(string_t{"child-v1", {}}, childResult->value, state.symbols);

    unsetenv("NIX_PSLOT_VAR");
}

TEST_F(TraceStoreTest, Recovery_StampedParentSlot_ParentRecovered)
{
    // Regression test: ParentSlot dep recovers when parent is recovered.
    //
    // Scenario: parent and child are recorded together. State reverts to a
    // previous version. Parent recovery succeeds (direct hash match in history).
    // Child's ParentSlot dep should also recover via direct hash matching
    // since the parent's trace hash matches.
    auto db = makeDb();
    auto parentPath = vpath({"sparent"});
    auto childPath = vpath({"sparent", "child"});

    withExclusiveStore(*db, [&](const auto & ea) {
        // Version 1
        setenv("NIX_STAMPED_VAR", "v1", 1);
        db->record(ea, parentPath, string_t{"parent-v1", {}},
            {makeEnvVarDep(pools(), "NIX_STAMPED_VAR", "v1")});

        auto parentTraceHash = db->getCurrentTraceHash(ea, parentPath);
        ASSERT_TRUE(parentTraceHash.has_value());

        // Child depends on parent via ParentSlot
        Dep parentDep = Dep::makeParentSlot(
            ParentSlot{parentPath},
            DepHashValue(DepHash{parentTraceHash->value}));
        db->record(ea, childPath, string_t{"child-v1", {}}, {parentDep});

        // Version 2
        setenv("NIX_STAMPED_VAR", "v2", 1);
        db->record(ea, parentPath, string_t{"parent-v2", {}},
            {makeEnvVarDep(pools(), "NIX_STAMPED_VAR", "v2")});

        auto parentTraceHashV2 = db->getCurrentTraceHash(ea, parentPath);
        ASSERT_TRUE(parentTraceHashV2.has_value());

        Dep parentDepV2 = Dep::makeParentSlot(
            ParentSlot{parentPath},
            DepHashValue(DepHash{parentTraceHashV2->value}));
        db->record(ea, childPath, string_t{"child-v2", {}}, {parentDepV2});
    });

    // Revert to v1 state
    setenv("NIX_STAMPED_VAR", "v1", 1);
    recreateDb(db);

    // Verify parent first — recovery to v1
    auto parentResult = test::TraceStorageTestAccess::verify(*db, parentPath, state);
    ASSERT_TRUE(parentResult.has_value());
    assertCachedResultEquals(string_t{"parent-v1", {}}, parentResult->value, state.symbols);

    // Verify child — should recover to v1 via ParentSlot dep
    auto childResult = test::TraceStorageTestAccess::verify(*db, childPath, state);
    ASSERT_TRUE(childResult.has_value())
        << "Child with ParentSlot should recover when parent is recovered to same trace";
    assertCachedResultEquals(string_t{"child-v1", {}}, childResult->value, state.symbols);

    unsetenv("NIX_STAMPED_VAR");
}

TEST_F(TraceStoreTest, Recovery_StampedValueContext_ParentRecovered)
{
    // Same pattern as ParentSlot but with ValueContext dep type.
    auto db = makeDb();
    auto parentPath = vpath({"vcparent"});
    auto childPath = vpath({"vcparent", "child"});

    withExclusiveStore(*db, [&](const auto & ea) {
        // Version 1
        setenv("NIX_SVC_VAR", "v1", 1);
        db->record(ea, parentPath, string_t{"parent-v1", {}},
            {makeEnvVarDep(pools(), "NIX_SVC_VAR", "v1")});

        auto parentTraceHash = db->getCurrentTraceHash(ea, parentPath);
        ASSERT_TRUE(parentTraceHash.has_value());

        // Child depends on parent via ValueContext
        Dep valueDep = Dep::makeValueContext(
            parentPath,
            DepHashValue(DepHash{parentTraceHash->value}));
        db->record(ea, childPath, string_t{"child-v1", {}}, {valueDep});

        // Version 2
        setenv("NIX_SVC_VAR", "v2", 1);
        db->record(ea, parentPath, string_t{"parent-v2", {}},
            {makeEnvVarDep(pools(), "NIX_SVC_VAR", "v2")});

        auto parentTraceHashV2 = db->getCurrentTraceHash(ea, parentPath);
        ASSERT_TRUE(parentTraceHashV2.has_value());

        Dep valueDepV2 = Dep::makeValueContext(
            parentPath,
            DepHashValue(DepHash{parentTraceHashV2->value}));
        db->record(ea, childPath, string_t{"child-v2", {}}, {valueDepV2});
    });

    // Revert to v1 state
    setenv("NIX_SVC_VAR", "v1", 1);
    recreateDb(db);

    auto parentResult = test::TraceStorageTestAccess::verify(*db, parentPath, state);
    ASSERT_TRUE(parentResult.has_value());
    assertCachedResultEquals(string_t{"parent-v1", {}}, parentResult->value, state.symbols);

    auto childResult = test::TraceStorageTestAccess::verify(*db, childPath, state);
    ASSERT_TRUE(childResult.has_value())
        << "Child with ValueContext should recover when parent is recovered to same trace";
    assertCachedResultEquals(string_t{"child-v1", {}}, childResult->value, state.symbols);

    unsetenv("NIX_SVC_VAR");
}

} // namespace nix::eval_trace
