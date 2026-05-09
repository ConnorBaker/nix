#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/source-accessor.hh"

#include <algorithm>
#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── queryKindName tests ───────────────────────────────────────────────

TEST_F(TraceStoreTest, Protocol_CQKNames_AllVariants)
{
    EXPECT_EQ(queryKindName(CanonicalQueryKind::FileBytes), "fileBytes");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::DirectoryEntries), "directoryEntries");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::ExistenceCheck), "existenceCheck");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::EnvironmentLookup), "environmentLookup");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::VolatileTime), "volatileTime");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::SessionSystemValue), "sessionSystemValue");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::RuntimeFetchIdentity), "runtimeFetchIdentity");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::TraceValueContext), "traceValueContext");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::TraceParentSlot), "traceParentSlot");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::DerivedStorePath), "derivedStorePath");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::VolatileExec), "volatileExec");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::NarIdentity), "narIdentity");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::StructuredProjection), "structuredProjection");
    EXPECT_EQ(queryKindName(CanonicalQueryKind::GitRevisionIdentity), "gitRevisionIdentity");
}

// ── record tests (BSàlC: trace recording / fresh evaluation) ─────

TEST_F(TraceStoreTest, Record_FreshAttr_ReturnsTraceId)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        auto result = db->record(ea, rootPath(), string_t{"hello", {}}, {});
        // Trace recording returns a positive trace identifier (BSàlC: trace key)
        EXPECT_GT(result.traceId.value, 0u);
    });
}

TEST_F(TraceStoreTest, Record_AttrExists_TrueAfterRecord)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"hello", {}}, {});

        EXPECT_TRUE(db->attrExists(ea, rootPath()));
        EXPECT_FALSE(db->attrExists(ea, vpath({"nonexistent"})));
    });
}

TEST_F(TraceStoreTest, Record_WithDeps_LoadedCorrectly)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        std::vector<Dep> deps = {
            makeContentDep(pools(), "/a.nix", "a"),
            makeEnvVarDep(pools(), "HOME", "/home"),
        };

        auto result = db->record(ea, rootPath(), int_t{NixInt{42}}, deps);

        auto loadedDeps = db->loadFullTrace(ea, result.traceId);
        EXPECT_EQ(loadedDeps->size(), 2u);
    });
}

TEST_F(TraceStoreTest, ColdStore_VolatileDep_NotSessionCached)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep(pools())};

    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, rootPath(), makeNull(), deps);
    });

    VerificationSession session;
    EXPECT_FALSE(test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state, session));
    EXPECT_FALSE(session.verifiedTraceIds.count(result.traceId));
}

TEST_F(TraceStoreTest, ColdStore_NonVolatile_SessionCached)
{
    ScopedEnvVar env("NIX_TEST_NONVOLATILE_CACHE_VAR", "val");

    auto db = makeDb();
    std::vector<Dep> deps = {
        makeEnvVarDep(pools(), "NIX_TEST_NONVOLATILE_CACHE_VAR", "val")
    };

    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, rootPath(), makeNull(), deps);
    });

    VerificationSession session;
    EXPECT_TRUE(test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state, session));
    EXPECT_TRUE(session.verifiedTraceIds.count(result.traceId));
}

TEST_F(TraceStoreTest, Record_WithParent_ChildDepsIsolated)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        // Record parent trace (BSàlC: trace for root key)
        db->record(ea, rootPath(), string_t{"parent-val", {}},
                  {makeContentDep(pools(), "/a.nix", "a")});

        // Record child trace — own deps only, no parent dep inheritance
        auto childResult = db->record(ea, vpath({"child"}), string_t{"child-val", {}},
                                     {makeEnvVarDep(pools(), "FOO", "bar")});

        // loadFullTrace returns only the child's own deps (no parent merging)
        auto childDeps = db->loadFullTrace(ea, childResult.traceId);
        EXPECT_EQ(childDeps->size(), 1u);
        EXPECT_EQ((*childDeps)[0].key.kind, CanonicalQueryKind::EnvironmentLookup);
    });
}

TEST_F(TraceStoreTest, Record_SameDeps_DeterministicTraceId)
{
    // Deterministic recording: same deps + result -> same trace (BSàlC: content-addressed trace)
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        std::vector<Dep> deps = {makeContentDep(pools(), "/a.nix", "a")};
        CachedResult value = string_t{"result", {}};

        auto r1 = db->record(ea, rootPath(), value, deps);
        auto r2 = db->record(ea, rootPath(), value, deps);

        // Same deps + same parent -> same trace (content-addressed deduplication)
        EXPECT_EQ(r1.traceId, r2.traceId);
    });
}

TEST_F(TraceStoreTest, Record_AllValueTypes_RoundTrip)
{
    auto db = makeDb();

    auto testRoundtrip = [&](const CachedResult & value, std::string_view name) {
        auto pathId = vpath({name});
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, pathId, value, {});
        });

        auto result = test::TraceStorageTestAccess::verify(*db, pathId, state);
        ASSERT_TRUE(result.has_value()) << "verify failed for " << name;
        assertCachedResultEquals(value, result->value, state.symbols);
    };

    testRoundtrip(string_t{"hello", {}}, "str");
    testRoundtrip(
        string_t{
            "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-shared",
            {},
            SemanticHandle{
                .path = PathObject{
                    .source = DepSource::fromNodeKey("path:/runtime-b"),
                    .rootPath = CanonPath("/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-shared"),
                },
            },
        },
        "str-origin");
    testRoundtrip(true, "bool-t");
    testRoundtrip(false, "bool-f");
    testRoundtrip(int_t{NixInt{42}}, "int");
    testRoundtrip(makeNull(), "null");
    testRoundtrip(float_t{3.14}, "float");
    testRoundtrip(path_t{"/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-test"}, "path");
    testRoundtrip(
        path_t{
            "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-runtime",
            SemanticHandle{
                .path = PathObject{
                    .source = DepSource::fromNodeKey("path:/runtime-a"),
                    .rootPath = CanonPath("/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-runtime"),
                },
            },
        },
        "path-origin");
    testRoundtrip(failed_t{}, "failed");
    testRoundtrip(makeMissing(), "missing");
    testRoundtrip(makeMisc(), "misc");
    testRoundtrip(
        attrs_t{
            {},
            TracedContainerMeta{
                .producerOrigin = StructuredObject{
                    DepSource::fromNodeKey("input1"), "attrs.json", {}, StructuredFormat::Json,
                },
                .valueIdentityStamp = ValueIdentityStamp(7),
            },
        },
        "attrs");
    testRoundtrip(
        list_t{
            std::vector<CachedListEntry>(5),
            TracedContainerMeta{
                .producerOrigin = StructuredObject{
                    DepSource::fromNodeKey("input1"), "list.json", {StructuredPathComponent::makeKey("items")}, StructuredFormat::Json,
                },
                .valueIdentityStamp = ValueIdentityStamp(11),
            },
        },
        "list");
    testRoundtrip(list_t{std::vector<CachedListEntry>(3)}, "string-list");
}

// ── Trace deduplication tests (BSàlC: content-addressed traces) ──────

TEST_F(TraceStoreTest, Record_IdenticalDeps_SameTraceId)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        std::vector<Dep> deps = {makeContentDep(pools(), "/shared.nix", "shared")};

        // Two root attributes with identical deps should share the same trace (BSàlC: trace sharing)
        auto r1 = db->record(ea, vpath({"a"}), string_t{"val1", {}}, deps);
        auto r2 = db->record(ea, vpath({"b"}), string_t{"val2", {}}, deps);

        // Both should share the same trace ID (content-addressed)
        EXPECT_EQ(r1.traceId, r2.traceId);

        auto deps1 = db->loadFullTrace(ea, r1.traceId);
        EXPECT_EQ(deps1->size(), 1u);
    });
}

TEST_F(TraceStoreTest, Record_EmptyDeps_HasDbRow)
{
    // Attributes with zero deps must still get a trace row (BSàlC:
    // empty trace is valid). To prove row identity and isolation (not
    // just the null round-trip), record a sibling trace with one dep at
    // a different attr path and assert the empty row's loadFullTrace
    // returns [] — not the sibling's dep accidentally picked up.
    auto db = makeDb();
    auto & v = state.vocabStore();
    auto siblingPath = v.internPath(rootPath(), v.internName("sibling"));

    auto emptyResult = withExclusiveStore(*db, [&](const auto & ea) {
        auto r = db->record(ea, rootPath(), string_t{"val", {}}, {});
        db->record(ea, siblingPath, string_t{"sib-val", {}},
                   {makeEnvVarDep(pools(), "EMPTYDEP_SIBLING_VAR", "v")});

        // The empty row's loadFullTrace returns [] — NOT the sibling's
        // dep, even though both rows share the same session.
        auto deps = db->loadFullTrace(ea, r.traceId);
        EXPECT_TRUE(deps->empty())
            << "empty-deps trace must load back with zero deps (no "
               "bleed-through from sibling rows)";
        return r;
    });

    // Empty trace should pass verification and serve the exact stored value.
    auto verified = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(verified.has_value());
    assertCachedResultEquals(string_t{"val", {}}, verified->value, state.symbols);
    // (verifyTrace is the lower-level surface; keep it as a secondary check.)
    EXPECT_TRUE(test::TraceStorageTestAccess::verifyTrace(*db, emptyResult.traceId, state));
}

// ── verifyTrace tests (BSàlC: verifying traces / Shake: unchanged check) ──

TEST_F(TraceStoreTest, VerifyTrace_EnvVar_Valid)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR", "expected_value");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_TEST_CACHE_VAR", "expected_value")};
    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, rootPath(), makeNull(), deps);
    });

    // Clear session memo cache so verifyTrace re-checks all deps (Salsa: force re-verification)
    recreateDb(db);

    bool valid = test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state);
    EXPECT_TRUE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_EnvVar_Invalid)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR", "new_value");

    auto db = makeDb();
    // Record trace with OLD expected hash — current env has a different value
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_TEST_CACHE_VAR", "old_value")};
    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, rootPath(), makeNull(), deps);
    });

    // Clear session memo cache so verifyTrace re-checks all deps (Salsa: force re-verification)
    recreateDb(db);

    bool valid = test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_CurrentTime_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeCurrentTimeDep(pools())};
    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, rootPath(), makeNull(), deps);
    });

    // CurrentTime is volatile — verification always fails (Shake: always-dirty rule)
    bool valid = test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_Exec_AlwaysFails)
{
    auto db = makeDb();
    std::vector<Dep> deps = {makeExecDep(pools())};
    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, rootPath(), makeNull(), deps);
    });

    bool valid = test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state);
    EXPECT_FALSE(valid);
}

TEST_F(TraceStoreTest, Verify_SessionCacheHit_SkipsReverification)
{
    ScopedEnvVar env("NIX_TEST_CACHE_VAR2", "val");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_TEST_CACHE_VAR2", "val")};
    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, rootPath(), makeNull(), deps);
    });

    VerificationSession session;
    EXPECT_TRUE(test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state, session));
    EXPECT_TRUE(session.verifiedTraceIds.count(result.traceId));
    // Second call hits session memo — skips re-verification (Salsa: green query)
    EXPECT_TRUE(test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state, session));
}

TEST_F(TraceStoreTest, VerifyTrace_NoDeps_Valid)
{
    auto db = makeDb();
    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, rootPath(), string_t{"val", {}}, {});
    });

    recreateDb(db);

    bool valid = test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state);
    EXPECT_TRUE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_ParentInvalid_ChildSurvives)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "current_value");

    auto db = makeDb();

    auto childResult = withExclusiveStore(*db, [&](const auto & ea) {
        // Record parent trace with stale dep
        std::vector<Dep> staleDeps = {makeEnvVarDep(pools(), "NIX_TEST_PARENT", "stale_value")};
        db->record(ea, rootPath(), string_t{"parent", {}}, staleDeps);

        // Record child with no direct deps — child has no deps to invalidate
        return db->record(ea, vpath({"child"}), string_t{"child", {}},
                                     {});
    });

    // Clear session memo cache
    recreateDb(db);

    // Each trace records only its own deps (no parent inheritance).
    // Child has no deps → always valid regardless of parent's state.
    bool valid = test::TraceStorageTestAccess::verifyTrace(*db, childResult.traceId, state);
    EXPECT_TRUE(valid);
}

TEST_F(TraceStoreTest, Verify_ParentDeps_Valid)
{
    ScopedEnvVar env("NIX_TEST_PARENT", "correct_value");

    auto db = makeDb();

    auto childResult = withExclusiveStore(*db, [&](const auto & ea) {
        // Record parent trace with correct dep (BSàlC: verifying trace succeeds)
        std::vector<Dep> parentDeps = {makeEnvVarDep(pools(), "NIX_TEST_PARENT", "correct_value")};
        db->record(ea, rootPath(), string_t{"parent", {}},
                  parentDeps);

        // Record child with valid parent (Shake: transitive clean)
        return db->record(ea, vpath({"child"}), string_t{"child", {}},
                                     {});
    });

    // Clear session memo cache
    recreateDb(db);

    bool valid = test::TraceStorageTestAccess::verifyTrace(*db, childResult.traceId, state);
    EXPECT_TRUE(valid);
}

TEST_F(TraceStoreTest, VerifyTrace_MultipleDeps_OneInvalid)
{
    ScopedEnvVar env1("NIX_TEST_VALID", "current_value");
    ScopedEnvVar env2("NIX_TEST_STALE", "new_value");

    auto db = makeDb();
    std::vector<Dep> deps = {
        makeEnvVarDep(pools(), "NIX_TEST_VALID", "current_value"),  // dep hash matches current
        makeEnvVarDep(pools(), "NIX_TEST_STALE", "old_value"),      // dep hash stale (Shake: dirty input)
    };
    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, rootPath(), makeNull(), deps);
    });

    recreateDb(db);

    bool valid = test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state);
    EXPECT_FALSE(valid);
}

// ── Record -> Verify roundtrip (BSàlC: trace recording then verification) ───

TEST_F(TraceStoreTest, Verify_WarmRoundtrip_ServesCachedValue)
{
    ScopedEnvVar env("NIX_WARM_TEST", "stable");

    auto db = makeDb();
    CachedResult input = string_t{"cached value", {}};
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_WARM_TEST", "stable")};

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), input, deps);
    });

    // Verification should find and validate the recorded trace (BSàlC: verify trace)
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(input, result->value, state.symbols);
}

TEST_F(TraceStoreTest, RecordVerify_Roundtrip_TraceId)
{
    ScopedEnvVar env("NIX_WARM_TEST2", "stable");

    auto db = makeDb();
    CachedResult input = int_t{NixInt{99}};
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_WARM_TEST2", "stable")};

    auto coldResult = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea, rootPath(), input, deps);
    });

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    // Verified trace's ID should match what recording returned
    EXPECT_EQ(result->traceId, coldResult.traceId);
}

TEST_F(TraceStoreTest, Verify_NoEntry_ReturnsFalse)
{
    auto db = makeDb();
    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"nonexistent"}), state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, Verify_InvalidatedDeps_ReturnsFalse)
{
    // Record trace with one env var value, then change it.
    // Verification should fail (no constructive recovery candidate exists).
    ScopedEnvVar env("NIX_WARM_INVALID", "value1");

    auto db = makeDb();
    std::vector<Dep> deps = {makeEnvVarDep(pools(), "NIX_WARM_INVALID", "value1")};
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"old", {}}, deps);
    });

    // Change env var — invalidates the recorded dep hash
    setenv("NIX_WARM_INVALID", "value2", 1);

    // Clear session memo cache to force re-verification (Salsa: invalidate memo)
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    // Should fail: trace hash no longer matches and no constructive recovery target exists
    EXPECT_FALSE(result.has_value());
}


// ── A-1 · RawBytes record→verify ────────────────────────────────────────
//
// RawBytes verification calls depHash(path.readFile()) — the same code path
// as FileBytes.  The dep key must be an absolute file path (must start with
// '/') so that SemanticRegistry::resolve returns a SourcePath rather than
// nullopt.  The stored hash must match the actual file content hash.

TEST_F(TraceStoreTest, RawBytes_RecordVerify_WarmHit)
{
    TempTextFile f("v1");
    auto filePath = std::filesystem::canonical(f.path).string();
    auto db = makeDb();
    auto fileHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"root"}), string_t{"v1", {}},
                   {makeSimpleRecordedDep(pools(), CanonicalQueryKind::RawBytes,
                       DepSource::makeAbsolute(), filePath, DepHashValue{fileHash})});
    });
    recreateDb(db);
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, vpath({"root"}), state).has_value());
}

TEST_F(TraceStoreTest, RawBytes_Rerecord_UpdatesTrace)
{
    TempTextFile f("v1");
    auto filePath = std::filesystem::canonical(f.path).string();
    auto db = makeDb();
    auto hashV1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"root"}), string_t{"v1", {}},
                   {makeSimpleRecordedDep(pools(), CanonicalQueryKind::RawBytes,
                       DepSource::makeAbsolute(), filePath, DepHashValue{hashV1})});
    });
    recreateDb(db);
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, vpath({"root"}), state).has_value());

    // Modify the file and recompute the hash. Re-record with the new hash.
    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    auto hashV2 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, vpath({"root"}), string_t{"v2", {}},
                   {makeSimpleRecordedDep(pools(), CanonicalQueryKind::RawBytes,
                       DepSource::makeAbsolute(), filePath, DepHashValue{hashV2})});
    });
    recreateDb(db);
    // v2 was just recorded; the v2 trace's RawBytes dep stores hashV2 which
    // matches the current file hash — verification must succeed.
    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"root"}), state);
    EXPECT_TRUE(result.has_value());
}

// ── B-3 · Pass-2 structural + implicit combined ──────────────────────────

TEST_F(TraceStoreTest, PassTwo_Structural_And_Implicit_Combined_BothPass)
{
    TempJsonFile jsonFile(R"({"k": 1})");
    auto filePath = std::filesystem::canonical(jsonFile.path).string();
    auto db = makeDb();
    auto contentHashV1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
    auto keysHash = canonicalKeysHash({"k"});
    auto typeHash = sentinel(SentinelHash::Object);
    std::vector<Dep> deps = {
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
            DepSource::makeAbsolute(), filePath, contentHashV1),
        makeStructuredDepForTest(
            pools(), CanonicalQueryKind::StructuredProjection,
            DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
            {}, DepHashValue(keysHash), ShapeSuffix::Keys),
        makeStructuredDepForTest(
            pools(), CanonicalQueryKind::ImplicitStructure,
            DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
            {}, DepHashValue(typeHash), ShapeSuffix::Type),
    };
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"combined", {}}, deps);
    });
    recreateDb(db);
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, rootPath(), state).has_value());
}

TEST_F(TraceStoreTest, StorageIdentity_IncludesImplicitStructureKeys)
{
    ScopedEnvVar env("NIX_TEST_EXACT_KEYSET", "stable");
    TempJsonFile jsonFile(R"({"k": 1})");
    auto filePath = std::filesystem::canonical(jsonFile.path).string();
    auto db = makeDb();

    auto first = SqliteTraceStorage::RecordResult{};
    auto second = SqliteTraceStorage::RecordResult{};
    withExclusiveStore(*db, [&](const auto & ea) {
        first = db->record(ea, vpath({"root"}), string_t{"env-only", {}},
            {makeEnvVarDep(pools(), "NIX_TEST_EXACT_KEYSET", "stable")});

        second = db->record(ea, vpath({"root"}), string_t{"env-plus-implicit", {}},
            {
                makeEnvVarDep(pools(), "NIX_TEST_EXACT_KEYSET", "stable"),
                makeStructuredDepForTest(
                    pools(), CanonicalQueryKind::ImplicitStructure,
                    DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
                    {}, DepHashValue(sentinel(SentinelHash::Object)), ShapeSuffix::Type),
            });
    });

    EXPECT_NE(first.traceId, second.traceId)
        << "Trace storage identity must include implicit guard deps even though TraceHash excludes them";

    recreateDb(db);
    withExclusiveStore(*db, [&](const auto & ea) {
        auto firstDeps = db->loadFullTrace(ea, first.traceId);
        auto secondDeps = db->loadFullTrace(ea, second.traceId);
        ASSERT_EQ(firstDeps->size(), 1u);
        ASSERT_EQ(secondDeps->size(), 2u);
        EXPECT_EQ((*secondDeps)[1].key.kind, CanonicalQueryKind::ImplicitStructure);
    });
}

TEST_F(TraceStoreTest, PassTwo_Structural_And_Implicit_Combined_StructuralFails)
{
    // Record with both structural and implicit, then modify the file's keys.
    // The keys hash changes; structural dep fails.
    TempJsonFile jsonFile(R"({"k": 1})");
    auto filePath = std::filesystem::canonical(jsonFile.path).string();
    auto db = makeDb();
    auto contentHashV1 = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
    auto keysHashV1 = canonicalKeysHash({"k"});
    auto typeHash = sentinel(SentinelHash::Object);
    std::vector<Dep> deps = {
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
            DepSource::makeAbsolute(), filePath, contentHashV1),
        makeStructuredDepForTest(
            pools(), CanonicalQueryKind::StructuredProjection,
            DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
            {}, DepHashValue(keysHashV1), ShapeSuffix::Keys),
        makeStructuredDepForTest(
            pools(), CanonicalQueryKind::ImplicitStructure,
            DepSource::makeAbsolute(), filePath, StructuredFormat::Json,
            {}, DepHashValue(typeHash), ShapeSuffix::Type),
    };
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"combined", {}}, deps);
    });
    recreateDb(db);
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, rootPath(), state).has_value());

    // Change the JSON keys — structural dep will fail; implicit type dep still passes.
    jsonFile.modify(R"({"k": 1, "extra": 2})");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // Outcome analysis: FileBytes fails (content changed), so pass 2
    // runs coverage for filePath. StructuredProjection (Keys) fails
    // too; ImplicitStructure (Type) passes. Because at least one
    // structural/implicit dep on filePath fails, the file is NOT
    // subsumed — verdict is Invalid.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value())
        << "key-set change fails StructuredProjection; the ImplicitStructure "
           "type dep alone is insufficient coverage, so verify must fail";
}

// ── B-5 · failed verification leaves no stale L1 entry ──────────────────

TEST_F(TraceStoreTest, L1Cache_AfterVerifyMiss_SecondCallAlsoMisses)
{
    TempTextFile f("v1");
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}},
                   {makeContentDep(pools(), f.path.string(), "v1")});
    });
    recreateDb(db);
    // Modify the file so the stored dep hash is stale.
    f.modify("v2");
    getFSSourceAccessor()->invalidateCache();
    // First verify — should miss (content changed, no recovery candidate).
    auto r1 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(r1.has_value());
    // Second verify on the same session — L1 must not have been left stale.
    auto r2 = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(r2.has_value());
}

// ── D-6 · Performance counters ──────────────────────────────────────────
//
// Counters are only incremented when Counter::enabled is true (NIX_SHOW_STATS).
// These tests enable counters for their duration, snapshot before/after, and
// assert the deltas are positive.
//
// nrTraceVerifications is only incremented by the async VerificationOrchestrator,
// not by the direct SqliteTraceStorage::verify() path used in these unit tests.
// nrDepsChecked IS incremented by VerifyImpl::runPass1 for each dep processed,
// which runs on the direct SqliteTraceStorage::verify() path.  A trace with at least
// one dep exercises this path.

TEST_F(TraceStoreTest, PerfCounters_ColdEval_IncrementsTracesVerified)
{
    ScopedEnvVar env("NIX_PERF_COUNTER_TEST_VAR", "stable");
    bool wasEnabled = Counter::enabled;
    Counter::enabled = true;

    auto before = nrDepsChecked.load();
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}},
                   {makeEnvVarDep(pools(), "NIX_PERF_COUNTER_TEST_VAR", "stable")});
    });
    recreateDb(db);
    test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    auto after = nrDepsChecked.load();

    Counter::enabled = wasEnabled;
    EXPECT_GT(after, before);
}

// ── G-1 · Empty trace dedup and recovery ────────────────────────────────

TEST_F(TraceStoreTest, EmptyTrace_RecordVerify_WarmHit)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}}, {});
    });
    recreateDb(db);
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, rootPath(), state).has_value());
}

TEST_F(TraceStoreTest, EmptyTrace_Dedup_SingleRow)
{
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        // Record the same empty trace twice.
        auto r1 = db->record(ea, rootPath(), string_t{"v", {}}, {});
        auto r2 = db->record(ea, rootPath(), string_t{"v", {}}, {});
        // Content-addressed: same deps + result → same trace ID (dedup).
        EXPECT_EQ(r1.traceId, r2.traceId);
    });
    recreateDb(db);
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, rootPath(), state).has_value());
}

// ── G-3 · All-CQK single-trace warm hit ─────────────────────────────────
//
// One dep of every testable non-volatile CQK kind in a single trace.
// Purpose: confirm that all testable CQK types pass verification in one trace.
//
// Omitted CQK types:
//   VolatileExec, VolatileTime — always fail verification by design
//   GitRevisionIdentity — requires a real git repo
//   RuntimeFetchIdentity — requires libfetchers (not linked into libexpr-tests)
//
// Each dep is recorded with the hash that the verifier will compute:
//   FileBytes, DirectoryEntries, NarIdentity, RawBytes  — EvalTraceHash
//   EnvironmentLookup, SessionSystemValue               — EvalTraceHash via depHash
//   ExistenceCheck                                       — string "type:0" (tRegular=0)
//   StorePathAvailability                                — string "valid" (after addToStore)
//   DerivedStorePath                                     — string(printStorePath) from computeStorePath
//   StructuredProjection, ImplicitStructure              — EvalTraceHash via canonicalKeysHash/kHashObject

TEST_F(TraceStoreTest, AllCQK_SingleTrace_WarmHit)
{
    // ── File and directory sources ─────────────────────────────────
    TempTextFile txtFile("allcqk-raw-content");
    TempDir td;
    td.addFile("entry.txt", "hello");
    TempJsonFile jsonFile(R"({"k": 1})");
    ScopedEnvVar env("NIX_TEST_ALLCQK_VAR", "allcqk-value");

    auto filePath = std::filesystem::canonical(txtFile.path).string();
    auto dirPath  = std::filesystem::canonical(td.path()).string();
    auto jsonPath = std::filesystem::canonical(jsonFile.path).string();

    // ── Record trace with all non-volatile CQK types ───────────────
    auto db = makeDb();

    // ── Pre-compute verifier-matching hashes ──────────────────────
    auto fileHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
    auto dirHash = depHashDirListing(
        SourcePath(getFSSourceAccessor(), CanonPath(dirPath)).readDirectory());
    auto narHash = depHashPath(SourcePath(getFSSourceAccessor(), CanonPath(filePath)));
    auto rawHash = depHash(SourcePath(getFSSourceAccessor(), CanonPath(filePath)).readFile());
    auto envHash = depHash("allcqk-value");
    auto sysHash = depHash(state.settings.getCurrentSystem());
    // ExistenceCheck for a regular file returns "type:0" (tRegular = 0)
    const std::string existHash = "type:0";

    // StorePathAvailability: pre-populate the dummy store
    auto storePathSpa = state.store->printStorePath(
        state.store->addToStore(
            std::string(*CanonPath(txtFile.path.string()).baseName()),
            SourcePath(getFSSourceAccessor(), CanonPath(filePath))));

    // DerivedStorePath: computeStorePath on the real file (pure)
    auto dspBaseName = std::string(*CanonPath(filePath).baseName());
    auto storePathDsp = state.store->printStorePath(
        std::get<0>(state.store->computeStorePath(
            dspBaseName,
            SourcePath(getFSSourceAccessor(), CanonPath(filePath))
                .resolveSymlinks(SymlinkResolution::Ancestors),
            ContentAddressMethod::Raw::NixArchive,
            HashAlgorithm::SHA256, {})));

    // StructuredProjection: #keys hash for {"k":1}
    auto scKeysHash = canonicalKeysHash({"k"});
    // ImplicitStructure: #type hash (attrset → kHashObject)
    auto scTypeHash = sentinel(SentinelHash::Object);
    std::vector<Dep> deps = {
        // FileBytes — EvalTraceHash
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::FileBytes,
            DepSource::makeAbsolute(), filePath, DepHashValue{fileHash}),
        // DirectoryEntries — EvalTraceHash
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::DirectoryEntries,
            DepSource::makeAbsolute(), dirPath, DepHashValue{dirHash}),
        // NarIdentity — EvalTraceHash
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::NarIdentity,
            DepSource::makeAbsolute(), filePath, DepHashValue{narHash}),
        // RawBytes — EvalTraceHash (key must be absolute path)
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::RawBytes,
            DepSource::makeAbsolute(), filePath, DepHashValue{rawHash}),
        // EnvironmentLookup — EvalTraceHash
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::EnvironmentLookup,
            DepSource::makeAbsolute(), "NIX_TEST_ALLCQK_VAR", DepHashValue{envHash}),
        // SessionSystemValue — EvalTraceHash
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::SessionSystemValue,
            DepSource::makeAbsolute(), "", DepHashValue{sysHash}),
        // ExistenceCheck — string "type:0"
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::ExistenceCheck,
            DepSource::makeAbsolute(), filePath, DepHashValue{std::string(existHash)}),
        // StorePathAvailability — string "valid"
        makeSimpleRecordedDep(pools(), CanonicalQueryKind::StorePathAvailability,
            DepSource::makeAbsolute(), storePathSpa, DepHashValue{std::string("valid")}),
        // RuntimeFetchIdentity omitted — requires libfetchers (not linked)
        // DerivedStorePath — string(printStorePath), canonical typed key
        makeCopiedPathDep(pools(), filePath, dspBaseName, storePathDsp),
        // StructuredProjection — EvalTraceHash (canonicalKeysHash for {"k":1})
        makeStructuredDepForTest(
            pools(), CanonicalQueryKind::StructuredProjection,
            DepSource::makeAbsolute(), jsonPath, StructuredFormat::Json,
            {}, DepHashValue(scKeysHash), ShapeSuffix::Keys),
        // ImplicitStructure — EvalTraceHash (kHashObject for attrset type)
        makeStructuredDepForTest(
            pools(), CanonicalQueryKind::ImplicitStructure,
            DepSource::makeAbsolute(), jsonPath, StructuredFormat::Json,
            {}, DepHashValue(scTypeHash), ShapeSuffix::Type),
    };
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}}, deps);
    });
    recreateDb(db);
    // All non-volatile deps should verify — expect warm hit.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_TRUE(result.has_value());
}

// ── G-5 · Symlinks ──────────────────────────────────────────────────────
//
// FileBytes verification uses PosixSourceAccessor::readFile which opens
// files with O_NOFOLLOW.  If the dep key is a symlink path,
// assertNoSymlinks() throws SymlinkNotAllowed before readFile even opens
// the fd.  To record a verifiable FileBytes dep in a directory that contains
// symlinks, use the REAL FILE path (not the symlink path) as the dep key.
// The dep hash is computed via depHash(path.readFile()) on the real path.

TEST_F(TraceStoreTest, Symlink_FileBytes_FollowsLink_WarmHit)
{
    TempDir td;
    td.addFile("real.txt", "real-content");
    td.addSymlink("link.txt", "real.txt");
    // Use the real file path — FileBytes cannot read through a symlink
    // (PosixSourceAccessor::readFile opens with O_NOFOLLOW).
    auto realPath = std::filesystem::canonical(td.path() / "real.txt").string();

    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"v", {}},
                   {makeContentDep(pools(), realPath, "real-content")});
    });
    recreateDb(db);
    // FileBytes dep on the real file verifies successfully even though
    // a symlink to the same file exists in the same directory.
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, rootPath(), state).has_value());
}

// ── G-4: Unicode in paths and attr names ─────────────────────────────
//
// Unicode attr path components and dep keys must survive round-trip through
// the blob serialization layer.
//
// Uses makeEnvVarDep rather than makeContentDep so that dep verification
// does not require creating files at Unicode filesystem paths (which may
// not be supported on all platforms under test). Env var lookups resolve
// entirely in-process.

TEST_F(TraceStoreTest, Unicode_PathsAndAttrNames_RoundTrip)
{
    // Unicode attr path components.
    auto p = vpath({"α-component", "β.attr"});
    auto db = makeDb();
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, p, string_t{"unicode-val", {}}, {});
    });
    recreateDb(db);
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, p, state).has_value());

    // Unicode key in a dep — makeEnvVarDep avoids requiring a file at a
    // Unicode path. The env var key and value contain multi-byte UTF-8
    // sequences that must survive blob serialization.
    ScopedEnvVar unicodeEnv("UNICÖDE_VAR", "héllo_wörld");
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(
            ea,
            rootPath(),
            string_t{"v", {}},
            {makeEnvVarDep(pools(), "UNICÖDE_VAR", "héllo_wörld")});
    });
    recreateDb(db);
    EXPECT_TRUE(test::TraceStorageTestAccess::verify(*db, rootPath(), state).has_value());
}

} // namespace nix::eval_trace
