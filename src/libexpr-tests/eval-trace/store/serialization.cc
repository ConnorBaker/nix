#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/trace-store.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <algorithm>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── Context hash isolation tests (BSàlC: separate trace stores per task) ──

TEST_F(TraceStoreTest, DifferentContextHash_Isolated)
{
    // Two different context hashes should have isolated trace namespaces

    {
        TraceStore db1(state.symbols, *state.traceCtx->pools, state.traceCtx->getVocabStore(state.symbols), 111);
        db1.record(vpath({"pkg"}), string_t{"v1", {}}, {}, false);
    }
    {
        TraceStore db2(state.symbols, *state.traceCtx->pools, state.traceCtx->getVocabStore(state.symbols), 222);
        db2.record(vpath({"pkg"}), string_t{"v2", {}}, {}, false);
    }

    {
        TraceStore db1(state.symbols, *state.traceCtx->pools, state.traceCtx->getVocabStore(state.symbols), 111);
        auto r1 = db1.verify(vpath({"pkg"}), {}, state);
        ASSERT_TRUE(r1.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r1->value));
        EXPECT_EQ(std::get<string_t>(r1->value).first, "v1");
    }
    {
        TraceStore db2(state.symbols, *state.traceCtx->pools, state.traceCtx->getVocabStore(state.symbols), 222);
        auto r2 = db2.verify(vpath({"pkg"}), {}, state);
        ASSERT_TRUE(r2.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r2->value));
        EXPECT_EQ(std::get<string_t>(r2->value).first, "v2");
    }
}

TEST_F(TraceStoreTest, NullByteAttrPath)
{
    auto db = makeDb();

    // Multi-component attr path: packages.x86_64-linux.hello
    auto fullPath = vpath({"packages", "x86_64-linux", "hello"});

    db.record(fullPath, string_t{"val", {}}, {}, false);
    EXPECT_TRUE(db.attrExists(fullPath));
    EXPECT_FALSE(db.attrExists(vpath({"packages"})));
}

TEST_F(TraceStoreTest, EmptyAttrPath)
{
    auto db = makeDb();
    db.record(rootPath(), string_t{"root-val", {}}, {}, true);
    EXPECT_TRUE(db.attrExists(rootPath()));
}

TEST_F(TraceStoreTest, MultipleEntries_Stress)
{
    auto db = makeDb();

    // Record 100 trace entries
    for (int i = 0; i < 100; i++) {
        auto name = "stress-" + std::to_string(i);
        db.record(vpath({name}), int_t{NixInt{i}}, {}, false);
    }

    EXPECT_TRUE(db.attrExists(vpath({"stress-0"})));
    EXPECT_TRUE(db.attrExists(vpath({"stress-99"})));
    EXPECT_FALSE(db.attrExists(vpath({"stress-100"})));
}

// ── BLOB serialization roundtrip tests (keys_blob + values_blob encoding) ──

TEST_F(TraceStoreTest, BlobRoundTrip_Empty)
{
    std::vector<TraceStore::InternedDepKey> keys;
    auto keysBlob = TraceStore::serializeKeys(keys);
    EXPECT_TRUE(keysBlob.empty());
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    EXPECT_TRUE(keysResult.empty());

    std::vector<TraceStore::InternedDep> deps;
    auto valsBlob = TraceStore::serializeValues(deps);
    EXPECT_TRUE(valsBlob.empty());
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    EXPECT_TRUE(valsResult.empty());
}

TEST_F(TraceStoreTest, BlobRoundTrip_Blake3Deps)
{
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;
    for (int i = 0; i < 5; i++) {
        auto hash = depHash("content-" + std::to_string(i));
        keys.push_back({DepType::Content, StringId(static_cast<uint32_t>(i + 1)),
                        StringId(static_cast<uint32_t>(i + 100))});
        deps.push_back({{DepType::Content, StringId(static_cast<uint32_t>(i + 1)),
                         StringId(static_cast<uint32_t>(i + 100))}, DepHashValue(hash)});
    }

    auto keysBlob = TraceStore::serializeKeys(keys);
    EXPECT_FALSE(keysBlob.empty());
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 5u);

    auto valsBlob = TraceStore::serializeValues(deps);
    EXPECT_FALSE(valsBlob.empty());
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 5u);

    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(keysResult[i].type, DepType::Content);
        EXPECT_EQ(keysResult[i].sourceId, StringId(static_cast<uint32_t>(i + 1)));
        EXPECT_EQ(keysResult[i].keyId, StringId(static_cast<uint32_t>(i + 100)));
        EXPECT_EQ(valsResult[i], deps[i].hash);
    }
}

TEST_F(TraceStoreTest, BlobRoundTrip_MixedDeps)
{
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;

    // BLAKE3 hash dep (Content — file content oracle)
    keys.push_back({DepType::Content, StringId(1), StringId(2)});
    deps.push_back({{DepType::Content, StringId(1), StringId(2)}, DepHashValue(depHash("file-data"))});

    // String hash dep (CopiedPath — store path oracle)
    keys.push_back({DepType::CopiedPath, StringId(3), StringId(4)});
    deps.push_back({{DepType::CopiedPath, StringId(3), StringId(4)},
                    DepHashValue(std::string("/nix/store/aaaa-test"))});

    // BLAKE3 hash dep (EnvVar — environment oracle)
    keys.push_back({DepType::EnvVar, StringId(5), StringId(6)});
    deps.push_back({{DepType::EnvVar, StringId(5), StringId(6)}, DepHashValue(depHash("env-val"))});

    // String hash dep (Existence — filesystem oracle)
    keys.push_back({DepType::Existence, StringId(7), StringId(8)});
    deps.push_back({{DepType::Existence, StringId(7), StringId(8)}, DepHashValue(std::string("missing"))});

    auto keysBlob = TraceStore::serializeKeys(keys);
    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 4u);

    auto valsBlob = TraceStore::serializeValues(deps);
    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 4u);

    // Content oracle: BLAKE3 hash key + value
    EXPECT_EQ(keysResult[0].type, DepType::Content);
    EXPECT_EQ(keysResult[0].sourceId, StringId(1));
    EXPECT_EQ(keysResult[0].keyId, StringId(2));
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[0]));

    // CopiedPath oracle: string (not BLAKE3, so deserialized as string)
    EXPECT_EQ(keysResult[1].type, DepType::CopiedPath);
    EXPECT_EQ(keysResult[1].sourceId, StringId(3));
    EXPECT_EQ(keysResult[1].keyId, StringId(4));
    EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[1]));
    EXPECT_EQ(std::get<std::string>(valsResult[1]), "/nix/store/aaaa-test");

    // EnvVar oracle: BLAKE3 hash
    EXPECT_EQ(keysResult[2].type, DepType::EnvVar);
    EXPECT_TRUE(std::holds_alternative<Blake3Hash>(valsResult[2]));

    // Existence oracle: string
    EXPECT_EQ(keysResult[3].type, DepType::Existence);
    EXPECT_TRUE(std::holds_alternative<std::string>(valsResult[3]));
    EXPECT_EQ(std::get<std::string>(valsResult[3]), "missing");
}

TEST_F(TraceStoreTest, BlobRoundTrip_LargeSet)
{
    std::vector<TraceStore::InternedDepKey> keys;
    std::vector<TraceStore::InternedDep> deps;
    for (uint32_t i = 0; i < 10000; i++) {
        auto hash = depHash("content-" + std::to_string(i));
        keys.push_back({DepType::Content, StringId(i), StringId(i + 50000)});
        deps.push_back({{DepType::Content, StringId(i), StringId(i + 50000)}, DepHashValue(hash)});
    }

    auto keysBlob = TraceStore::serializeKeys(keys);
    // keys_blob is zstd-compressed; verify smaller than raw (10000 * 9 bytes)
    EXPECT_GT(keysBlob.size(), 0u);
    EXPECT_LT(keysBlob.size(), 10000u * 9u);

    auto keysResult = TraceStore::deserializeKeys(keysBlob.data(), keysBlob.size());
    ASSERT_EQ(keysResult.size(), 10000u);

    auto valsBlob = TraceStore::serializeValues(deps);
    // values_blob is zstd-compressed; random BLAKE3 hashes don't compress well,
    // so just verify non-empty (zstd overhead may exceed raw size for random data)
    EXPECT_GT(valsBlob.size(), 0u);

    auto valsResult = TraceStore::deserializeValues(valsBlob.data(), valsBlob.size(), keysResult);
    ASSERT_EQ(valsResult.size(), 10000u);

    // Spot-check first, middle, and last
    EXPECT_EQ(keysResult[0].sourceId, StringId(0));
    EXPECT_EQ(keysResult[0].keyId, StringId(50000));
    EXPECT_EQ(keysResult[5000].sourceId, StringId(5000));
    EXPECT_EQ(keysResult[5000].keyId, StringId(55000));
    EXPECT_EQ(keysResult[9999].sourceId, StringId(9999));
    EXPECT_EQ(keysResult[9999].keyId, StringId(59999));

    // Verify hashes match
    for (uint32_t i = 0; i < 10000; i++) {
        EXPECT_EQ(valsResult[i], deps[i].hash) << "Hash mismatch at index " << i;
    }
}

// ── Dep storage tests (content-addressed DepKeySets dedup) ────────────

TEST_F(TraceStoreTest, DepKeySets_SiblingOverlap)
{
    auto db = makeDb();

    // Parent with 0 deps (FullAttrs pattern — trace records only child names)
    db.record(rootPath(), null_t{}, {}, true);

    // Child A trace with 100 deps (own deps only, no parent inheritance)
    std::vector<Dep> depsA;
    for (int i = 0; i < 100; i++) {
        depsA.push_back(makeContentDep(pools(), "/file-" + std::to_string(i) + ".nix",
                                       "content-" + std::to_string(i)));
    }
    auto childA = db.record(vpath({"a"}), string_t{"val-a", {}}, depsA, false);

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
    auto childB = db.record(vpath({"b"}), string_t{"val-b", {}}, depsB, false);

    // Both traces should load correctly with full deps
    auto loadedA = db.loadFullTrace(childA.traceId);
    auto loadedB = db.loadFullTrace(childB.traceId);
    EXPECT_EQ(loadedA.size(), 100u);
    EXPECT_EQ(loadedB.size(), 100u);
}

TEST_F(TraceStoreTest, Record_SeparatedDeps)
{
    // Parent with deps, child with own deps — each stores only its own.
    // Tests that parent deps are NOT merged into child traces.
    auto db = makeDb();

    // Parent with 0 deps (FullAttrs pattern — trace records only child names)
    db.record(rootPath(), null_t{}, {}, true);

    // Child A trace with 100 deps
    std::vector<Dep> depsA;
    for (int i = 0; i < 100; i++) {
        depsA.push_back(makeContentDep(pools(), "/f" + std::to_string(i) + ".nix",
                                       "c" + std::to_string(i)));
    }
    auto childA = db.record(vpath({"a"}), int_t{NixInt{1}}, depsA, false);

    // Child B trace with 99 overlapping + 1 different dep hash
    std::vector<Dep> depsB;
    for (int i = 0; i < 99; i++) {
        depsB.push_back(makeContentDep(pools(), "/f" + std::to_string(i) + ".nix",
                                       "c" + std::to_string(i)));
    }
    depsB.push_back(makeContentDep(pools(), "/f99.nix", "c99-modified"));
    auto childB = db.record(vpath({"b"}), int_t{NixInt{2}}, depsB, false);

    // Verify both traces load correctly
    auto loadedA = db.loadFullTrace(childA.traceId);
    auto loadedB = db.loadFullTrace(childB.traceId);
    EXPECT_EQ(loadedA.size(), 100u);
    EXPECT_EQ(loadedB.size(), 100u);

    // Verify B trace has the modified dep hash
    bool foundModified = false;
    for (auto & idep : loadedB) {
        auto dep = db.resolveDep(idep);
        if (dep.key == "/f99.nix") {
            auto h = depHash("c99-modified");
            EXPECT_EQ(dep.expectedHash, DepHashValue(h));
            foundModified = true;
        }
    }
    EXPECT_TRUE(foundModified);
}

// ── Batch verification + dep hash caching tests (Shake: unchanged check) ──

TEST_F(TraceStoreTest, WarmPath_BatchValidation)
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
    auto result = db.record(rootPath(), null_t{}, deps, true);

    // Change dep #25 to invalidate its hash
    setenv("NIX_BATCH_25", "CHANGED", 1);
    db.clearSessionCaches();

    // Verify — should fail but cache ALL 50 current dep hashes for reuse in recovery
    bool valid = db.verifyTrace(result.traceId, {}, state);
    EXPECT_FALSE(valid);

    // All 50 dep keys should be in currentDepHashes (cached for constructive recovery)
    EXPECT_GE(db.currentDepHashes.size(), 50u);

    // Clean up env vars
    for (int i = 0; i < 50; i++) {
        auto key = "NIX_BATCH_" + std::to_string(i);
        unsetenv(key.c_str());
    }
}

TEST_F(TraceStoreTest, WarmPath_HashCaching)
{
    // Record trace with deps, change 1, trigger verification failure -> constructive recovery.
    // Assert that recovery reuses cached dep hashes from verification (Shake: unchanged check reuse).
    ScopedEnvVar env1("NIX_HASHCACHE_A", "valA");
    ScopedEnvVar env2("NIX_HASHCACHE_B", "valB");

    auto db = makeDb();

    // Version 1: deps A and B
    std::vector<Dep> deps1 = {
        makeEnvVarDep(pools(), "NIX_HASHCACHE_A", "valA"),
        makeEnvVarDep(pools(), "NIX_HASHCACHE_B", "valB"),
    };
    db.record(rootPath(), string_t{"result-1", {}}, deps1, true);

    // Version 2: A changed, B same
    setenv("NIX_HASHCACHE_A", "valA2", 1);
    std::vector<Dep> deps2 = {
        makeEnvVarDep(pools(), "NIX_HASHCACHE_A", "valA2"),
        makeEnvVarDep(pools(), "NIX_HASHCACHE_B", "valB"),
    };
    db.record(rootPath(), string_t{"result-2", {}}, deps2, true);

    // Revert A
    setenv("NIX_HASHCACHE_A", "valA", 1);
    db.clearSessionCaches();

    // verify should fail verification (trace 2 deps don't match) then constructively recover to result-1
    auto result = db.verify(rootPath(), {}, state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result-1", {}}, result->value, state.symbols);

    // currentDepHashes should have entries from batch verification
    EXPECT_GE(db.currentDepHashes.size(), 2u);
}

TEST_F(TraceStoreTest, WarmPath_BaseValidatedOnce)
{
    // Record 5 siblings sharing the same trace.
    // Verify all 5. Shared trace should be verified only once (Salsa: memoized verification).
    ScopedEnvVar env("NIX_BASE_VALID", "ok");

    auto db = makeDb();
    std::vector<Dep> sharedDeps = {makeEnvVarDep(pools(), "NIX_BASE_VALID", "ok")};

    // Record 5 attrs with identical deps (all share the same trace)
    for (int i = 0; i < 5; i++) {
        auto name = "sibling-" + std::to_string(i);
        db.record(vpath({name}), int_t{NixInt{i}}, sharedDeps, false);
    }

    db.clearSessionCaches();

    // Verify all 5 — trace verified on first access, session-memoized for rest
    for (int i = 0; i < 5; i++) {
        auto name = "sibling-" + std::to_string(i);
        auto result = db.verify(vpath({name}), {}, state);
        ASSERT_TRUE(result.has_value()) << "Sibling " << i << " failed";
        ASSERT_TRUE(std::holds_alternative<int_t>(result->value));
        EXPECT_EQ(std::get<int_t>(result->value).x.value, i);
    }

    // verifiedTraceIds should have the shared trace (verified only once, then memoized)
    EXPECT_FALSE(db.verifiedTraceIds.empty());
}

// ── Full end-to-end record + verification roundtrip ──────────────────

TEST_F(TraceStoreTest, RecordVerify_WarmRoundtrip)
{
    // End-to-end: record traces, then verification retrieves correctly.
    // Use EnvVar deps so verification can actually check them (no files needed).
    ScopedEnvVar env1("NIX_DW_SHARED", "stable");
    ScopedEnvVar env2("NIX_DW_A", "a-val");
    ScopedEnvVar env3("NIX_DW_B", "b-val");

    auto db = makeDb();

    // 3 attrs with overlapping deps (all env vars — verifiable)
    auto sharedDep = makeEnvVarDep(pools(), "NIX_DW_SHARED", "stable");

    // Attr 1: shared + 1 unique
    std::vector<Dep> deps1 = {sharedDep, makeEnvVarDep(pools(), "NIX_DW_A", "a-val")};
    db.record(vpath({"a"}), string_t{"val-a", {}}, deps1, false);

    // Attr 2: shared + 1 different unique
    std::vector<Dep> deps2 = {sharedDep, makeEnvVarDep(pools(), "NIX_DW_B", "b-val")};
    db.record(vpath({"b"}), string_t{"val-b", {}}, deps2, false);

    // Attr 3: shared only
    std::vector<Dep> deps3 = {sharedDep};
    db.record(vpath({"c"}), string_t{"val-c", {}}, deps3, false);

    db.clearSessionCaches();

    // Verify all 3 traces (BSàlC: verify trace -> serve cached result)
    auto ra = db.verify(vpath({"a"}), {}, state);
    ASSERT_TRUE(ra.has_value());
    assertCachedResultEquals(string_t{"val-a", {}}, ra->value, state.symbols);

    auto rb = db.verify(vpath({"b"}), {}, state);
    ASSERT_TRUE(rb.has_value());
    assertCachedResultEquals(string_t{"val-b", {}}, rb->value, state.symbols);

    auto rc = db.verify(vpath({"c"}), {}, state);
    ASSERT_TRUE(rc.has_value());
    assertCachedResultEquals(string_t{"val-c", {}}, rc->value, state.symbols);
}

} // namespace nix::eval_trace
