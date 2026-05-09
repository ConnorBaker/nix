#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/registry.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

namespace {

struct ScopedExtraExperimentalFeatures
{
    explicit ScopedExtraExperimentalFeatures(std::string_view features)
    {
        experimentalFeatureSettings.set("extra-experimental-features", std::string(features));
    }

    ~ScopedExtraExperimentalFeatures()
    {
        experimentalFeatureSettings.set("extra-experimental-features", "");
    }
};

} // namespace

class TraceCacheIntegrationTest : public TraceCacheFixture
{
public:
    TraceCacheIntegrationTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "integration-test");
    }

protected:
    static SemanticSessionKey bootstrapKey(std::string_view suffix)
    {
        return SemanticSessionKey::fromSerialized("verify-integration:" + std::string(suffix));
    }

    AttrVocabStore & testVocab() {
        return state.vocabStore();
    }

    /// Build an AttrPathId from string components.
    AttrPathId vpath(std::initializer_list<std::string_view> parts) {
        return vocabPath(testVocab(), parts);
    }

    /// Root path sentinel.
    AttrPathId rootPath() { return AttrVocabStore::rootPath(); }

    std::unique_ptr<SqliteTraceStorage> makeDbBackend()
    {
        return std::make_unique<SqliteTraceStorage>(state.symbols, state.tracingPools(),
            state.vocabStore(), bootstrapKey("default"));
    }
};

// ── SqliteTraceStorage record/verify integration (BSàlC: trace recording then verification) ──

TEST_F(TraceCacheIntegrationTest, Integration_ColdStore_ThenWarmPath)
{
    auto db = makeDbBackend();

    auto storeResult = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea,
            rootPath(), string_t{"hello", {}}, {});
    });

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<string_t>(result->value));
    EXPECT_EQ(std::get<string_t>(result->value).first, "hello");
    EXPECT_EQ(result->traceId, storeResult.traceId);
}

TEST_F(TraceCacheIntegrationTest, Integration_MultipleBootstrapKeys_Isolated)
{
    // Use separate SqliteTraceStorage instances with different bootstrap session
    // keys (BSalC: isolated trace stores).
    {
        SqliteTraceStorage db1(state.symbols, state.tracingPools(), state.vocabStore(), bootstrapKey("111"));
        withExclusiveStore(db1, [&](const auto & ea) {
            db1.record(ea, rootPath(), string_t{"value-1", {}}, {});
        });
    }

    {
        SqliteTraceStorage db2(state.symbols, state.tracingPools(), state.vocabStore(), bootstrapKey("222"));
        withExclusiveStore(db2, [&](const auto & ea) {
            db2.record(ea, rootPath(), string_t{"value-2", {}}, {});
        });
    }

    // Verify isolation: each bootstrap session key sees its own trace result.
    {
        SqliteTraceStorage db1(state.symbols, state.tracingPools(), state.vocabStore(), bootstrapKey("111"));
        auto r1 = test::TraceStorageTestAccess::verify(db1, rootPath(), state);
        ASSERT_TRUE(r1.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r1->value));
        EXPECT_EQ(std::get<string_t>(r1->value).first, "value-1");
    }
    {
        SqliteTraceStorage db2(state.symbols, state.tracingPools(), state.vocabStore(), bootstrapKey("222"));
        auto r2 = test::TraceStorageTestAccess::verify(db2, rootPath(), state);
        ASSERT_TRUE(r2.has_value());
        ASSERT_TRUE(std::holds_alternative<string_t>(r2->value));
        EXPECT_EQ(std::get<string_t>(r2->value).first, "value-2");
    }
}

// ── Parent-child trace chain tests (Adapton: DDG parent-child edges) ──

TEST_F(TraceCacheIntegrationTest, Integration_ParentChild_AttrChain)
{
    auto db = makeDbBackend();

    auto childPathId = vpath({"child"});
    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea,
            rootPath(), attrs_t{{CachedAttrEntry{createSymbol("child")}}},
            {});

        db->record(ea,
            childPathId, int_t{NixInt{42}},
            {});
    });

    // Verification for child should work (BSàlC: verify child trace with parent hint)
    auto result = test::TraceStorageTestAccess::verify(*db, childPathId, state);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<int_t>(result->value));
    EXPECT_EQ(std::get<int_t>(result->value).x.value, 42);
}

TEST_F(TraceCacheIntegrationTest, Integration_ParentChild_ValidationCascade)
{
    ScopedEnvVar env("NIX_INT_TEST_VAR", "valid");

    auto db = makeDbBackend();

    // Parent trace with a valid dep (BSàlC: verifiable trace)
    auto & p = state.tracingPools();
    auto dep = makeEnvVarDep(p, "NIX_INT_TEST_VAR", "valid");
    auto childResult = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea,
            rootPath(), makeNull(), {dep});

        // Child trace with no deps — always valid. Each trace records only
        // its own deps; the parent's EnvVar dep is not inherited by children.
        return db->record(ea,
            vpath({"child"}), int_t{NixInt{1}}, {});
    });

    EXPECT_TRUE(test::TraceStorageTestAccess::verifyTrace(*db, childResult.traceId, state));
}

TEST_F(TraceCacheIntegrationTest, Recovery_ChildSurvivesParentInvalidation_CacheHit)
{
    ScopedEnvVar env("NIX_INT_CASCADE", "current");

    auto db = makeDbBackend();

    // Parent trace with stale dep (hash doesn't match current oracle state)
    auto & p = state.tracingPools();
    auto staleDep = makeEnvVarDep(p, "NIX_INT_CASCADE", "old_value");
    auto childResult = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), makeNull(), {staleDep});

        // Child with no direct deps
        return db->record(ea,
            vpath({"child"}), int_t{NixInt{1}}, {});
    });

    // Each trace records only its own deps (no parent inheritance).
    // Child has no deps → always valid. Parent's stale EnvVar dep
    // lives in the parent's trace only, not the child's.
    db.reset();
    db = makeDbBackend();
    EXPECT_TRUE(test::TraceStorageTestAccess::verifyTrace(*db, childResult.traceId, state));
}

// ── Lock-file structural coverage: per-file scope ────────────────────
//
// When a file's coarse Content dep matches, deferred structural deps on the
// same file are covered. Structural deps on another file from the same source
// still have to verify normally.

TEST_F(TraceCacheIntegrationTest, Integration_LockFileSubsumption_PerFileScope)
{
    TempDir root;
    root.addFile("flake.lock", "{}");
    root.addFile("flake.nix", "{}");

    auto db = makeDbBackend();
    auto & p = state.tracingPools();
    auto source = DepSource::fromNodeKey("root");
    auto lockHash = depHash(std::string("{}"));
    auto bogusHash = depHash(std::string("bogus"));

    auto sameFile = withExclusiveStore(*db, [&](const auto & ea) {
        auto sameFileResult = db->record(
            ea,
            vpath({"same-file"}),
            makeNull(),
            {
                makeSimpleRecordedDep(p, CanonicalQueryKind::FileBytes, source, "/flake.lock", DepHashValue(lockHash)),
                makeStructuredDepForTest(
                    p, CanonicalQueryKind::StructuredProjection, source, "/flake.lock",
                    StructuredFormat::Json, {}, DepHashValue(bogusHash)),
            });
        auto otherFileResult = db->record(
            ea,
            vpath({"other-file"}),
            makeNull(),
            {
                makeSimpleRecordedDep(p, CanonicalQueryKind::FileBytes, source, "/flake.lock", DepHashValue(lockHash)),
                makeStructuredDepForTest(
                    p, CanonicalQueryKind::StructuredProjection, source, "/flake.nix",
                    StructuredFormat::Nix, {}, DepHashValue(bogusHash)),
            });
        return std::pair{sameFileResult, otherFileResult};
    });

    boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> locked;
    locked.emplace(source, SourcePath(getFSSourceAccessor(), CanonPath(root.path().string())));
    SemanticRegistry registry(std::move(locked));
    VerificationSession session;

    EXPECT_TRUE(test::TraceStorageTestAccess::verifyTrace(*db, sameFile.first.traceId, registry, state, session))
        << "same-file structural dep should be covered by the matching content dep";
    EXPECT_FALSE(test::TraceStorageTestAccess::verifyTrace(*db, sameFile.second.traceId, registry, state, session))
        << "structural dep on a different file must not be covered";
}

// ── Soundness gap: ParentSlot does not capture attrset key set ───────
//
// Known limitation: ParentSlot deps capture the active eval-trace digest of
// the parent's dep hashes, which reflects the parent's computation inputs
// (files read), NOT the parent attrset's output structure (key set). When a
// sibling attribute is removed (parent's output changes but dep inputs don't),
// the ParentSlot dep still matches, and verification falsely passes.
//
// This test documents the gap. When a fix is implemented (e.g., an
// "attrset key set" dep), this test should be updated to assert that
// verification FAILS for the removed child.

TEST_F(TraceCacheIntegrationTest, Integration_ParentSlot_DoesNotCaptureKeySetRemoval)
{
    auto db = makeDbBackend();

    auto parentPath = vpath({"parent"});
    auto childAPath = vpath({"parent", "childA"});
    auto childBPath = vpath({"parent", "childB"});

    // Session 1: parent has two children {childA, childB}.
    // Parent has no file deps (its output is purely structural).
    auto parentTraceHash = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, parentPath,
            attrs_t{{CachedAttrEntry{createSymbol("childA")},
                     CachedAttrEntry{createSymbol("childB")}}},
            {});

        return db->getCurrentTraceHash(ea, parentPath);
    });
    ASSERT_TRUE(parentTraceHash.has_value());

    // Record childA and childB, each with a ParentSlot dep on the parent.
    Dep parentSlotDep = Dep::makeParentSlot(
        ParentSlot{parentPath},
        DepHashValue(DepHash{parentTraceHash->value}));
    auto childBResult = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, childAPath, string_t{"a-value", {}}, {});
        return db->record(ea, childBPath,
            string_t{"b-value", {}}, {parentSlotDep});
    });

    // Session 2: parent is re-recorded with only {childA}.
    // childB is removed. But the parent's dep set is empty (same as before),
    // so the parent's trace hash is unchanged.
    db.reset();
    db = makeDbBackend();
    auto parentTraceHashV2 = withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, parentPath,
            attrs_t{{CachedAttrEntry{createSymbol("childA")}}},
            {});

        return db->getCurrentTraceHash(ea, parentPath);
    });
    ASSERT_TRUE(parentTraceHashV2.has_value());

    // KNOWN GAP: parent trace hash is the same because deps are the same
    // (both sessions recorded the parent with no deps). The ParentSlot
    // dep for childB still matches.
    EXPECT_EQ(parentTraceHash->value, parentTraceHashV2->value)
        << "Parent trace hash should be unchanged (same deps, different keys — "
           "this is the soundness gap)";

    // childB's trace still verifies because its ParentSlot dep matches.
    // This is the false positive — childB was removed but verification passes.
    // TODO: When attrset key set deps are added, change this to EXPECT_FALSE.
    EXPECT_TRUE(test::TraceStorageTestAccess::verifyTrace(
        *db, childBResult.traceId, state))
        << "KNOWN GAP: ParentSlot does not capture key set changes. "
           "childB verifies despite being removed from parent.";
}

// ── Full TraceSession + DepRecordingContext flow (BSàlC: end-to-end trace pipeline) ──

TEST_F(TraceCacheIntegrationTest, Integration_FullFlow_ScalarRoot)
{
    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache("\"hello world\"");
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsStringEq("hello world"));
    }
    // Verification (BSàlC: verify trace and serve cached result)
    {
        int loaderCalls = 0;
        auto cache = makeCache("\"hello world\"", &loaderCalls);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsStringEq("hello world"));
        EXPECT_EQ(loaderCalls, 0);
    }
}

TEST_F(TraceCacheIntegrationTest, Integration_FullFlow_NestedAttrAccess)
{
    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache("{ x = { y = 42; }; }");
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsAttrsOfSize(1));
        auto * x = v->attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        auto * y = x->value->attrs()->get(createSymbol("y"));
        ASSERT_NE(y, nullptr);
        state.forceValue(*y->value, noPos);
        EXPECT_THAT(*y->value, IsIntEq(42));
    }
    // Verification (BSàlC: verify trace and serve cached result)
    {
        int loaderCalls = 0;
        auto cache = makeCache("{ x = { y = 42; }; }", &loaderCalls);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * x = v->attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        auto * y = x->value->attrs()->get(createSymbol("y"));
        ASSERT_NE(y, nullptr);
        state.forceValue(*y->value, noPos);
        EXPECT_THAT(*y->value, IsIntEq(42));
        EXPECT_EQ(loaderCalls, 0);
    }
}

TEST_F(TraceCacheIntegrationTest, Integration_FullFlow_TwoIndependentAttrs)
{
    // Fresh evaluation (BSàlC: trace recording)
    {
        auto cache = makeCache("{ a = 1; b = 2; }");
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * a = v->attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsIntEq(1));
        auto * b = v->attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsIntEq(2));
    }
    // Verification (BSàlC: verify trace and serve cached result)
    {
        int loaderCalls = 0;
        auto cache = makeCache("{ a = 1; b = 2; }", &loaderCalls);
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        auto * a = v->attrs()->get(createSymbol("a"));
        ASSERT_NE(a, nullptr);
        state.forceValue(*a->value, noPos);
        EXPECT_THAT(*a->value, IsIntEq(1));
        auto * b = v->attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);
        EXPECT_THAT(*b->value, IsIntEq(2));
        EXPECT_EQ(loaderCalls, 0);
    }
}

TEST_F(TraceCacheIntegrationTest, Integration_FullFlow_ParsedExpr)
{
    // Use state.parseExprFromString directly as the rootLoader
    int loaderCalls = 0;
    auto loader = [this, &loaderCalls]() -> Value * {
        ++loaderCalls;
        auto * e = state.parseExprFromString(
            "{ message = \"parsed\"; count = 3; }",
            state.rootPath(CanonPath::root));
        auto * v = state.allocValue();
        state.eval(e, *v);
        return v;
    };

    // Fresh evaluation (BSàlC: trace recording)
    {
        releaseActiveSession();
        auto cache = make_ref<TraceSession>(
            std::optional<TraceSession::BackendParams>{
                TraceSession::BackendParams{testFingerprint, std::nullopt}},
            state, loader);
        activeSession_ = cache.get_ptr();
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsAttrsOfSize(2));
    }
    EXPECT_EQ(loaderCalls, 1);
    // Verification (BSàlC: verify trace and serve cached result)
    {
        releaseActiveSession();
        auto cache = make_ref<TraceSession>(
            std::optional<TraceSession::BackendParams>{
                TraceSession::BackendParams{testFingerprint, std::nullopt}},
            state, loader);
        activeSession_ = cache.get_ptr();
        auto * v = cache->getRootValue();
        state.forceValue(*v, noPos);
        EXPECT_THAT(*v, IsAttrsOfSize(2));
    }
    EXPECT_EQ(loaderCalls, 1) << "warm verify must not re-run the loader";
}

// ── Session memoization integration (Salsa: memoized verification) ───

TEST_F(TraceCacheIntegrationTest, Integration_SessionCache_TraceVerification_SkipsRevalidation)
{
    ScopedEnvVar env("NIX_EVAL_SESSION", "ok");

    auto db = makeDbBackend();
    auto & p = state.tracingPools();
    auto dep = makeEnvVarDep(p, "NIX_EVAL_SESSION", "ok");
    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea,
            rootPath(), makeNull(), {dep});
    });

    VerificationSession session;
    db.reset();
    db = makeDbBackend();
    EXPECT_TRUE(test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state, session));
    EXPECT_TRUE(session.verifiedTraceIds.count(result.traceId));

    // Second call should be memoized in the shared session (Salsa: green query)
    EXPECT_TRUE(test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state, session));
}

TEST_F(TraceCacheIntegrationTest, Integration_SessionCache_VolatileDep_NotCached)
{
    auto db = makeDbBackend();
    auto & p = state.tracingPools();
    auto dep = makeCurrentTimeDep(p);
    auto result = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea,
            rootPath(), makeNull(), {dep});
    });

    VerificationSession session;
    EXPECT_FALSE(test::TraceStorageTestAccess::verifyTrace(*db, result.traceId, state, session));
    EXPECT_FALSE(session.verifiedTraceIds.count(result.traceId));
}

// ── Constructive recovery flow integration (BSàlC: constructive traces) ──

TEST_F(TraceCacheIntegrationTest, Recovery_DepChange_RecoversToPriorTrace)
{
    // Step 1: Record trace with env var = "v1"
    // Step 2: Change env var to "v2", old trace invalid
    // Step 3: Change back to "v1" -> constructive recovery should find v1's trace

    auto db = makeDbBackend();

    auto & p = state.tracingPools();

    // Step 1: Record trace with v1
    SqliteTraceStorage::RecordResult v1Result;
    {
        ScopedEnvVar env("NIX_RECOVERY_TEST", "v1");
        auto dep = makeEnvVarDep(p, "NIX_RECOVERY_TEST", "v1");
        v1Result = withExclusiveStore(*db, [&](const auto & ea) {
            return db->record(ea, rootPath(), string_t{"result-v1", {}}, {dep});
        });
    }

    // Step 2: Record trace with v2
    {
        ScopedEnvVar env("NIX_RECOVERY_TEST", "v2");
        auto dep = makeEnvVarDep(p, "NIX_RECOVERY_TEST", "v2");
        withExclusiveStore(*db, [&](const auto & ea) {
            db->record(ea, rootPath(), string_t{"result-v2", {}}, {dep});
        });
    }

    // Step 3: Revert to v1 — trigger constructive recovery
    {
        ScopedEnvVar env("NIX_RECOVERY_TEST", "v1");

        // Clear session memos to force re-verification
        db.reset();
        db = makeDbBackend();

        // Primary CurrentNode points to v2's trace (it was recorded
        // second and overwrote v1's slot). Verifying that fails
        // because the env var is now v1. Constructive recovery must
        // traverse History and find v1's trace, whose env-var dep
        // matches the current oracle state.
        auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
        ASSERT_TRUE(result.has_value())
            << "constructive recovery must find v1's trace in History "
               "(v1's env-var dep matches the current oracle state)";
        ASSERT_TRUE(std::holds_alternative<string_t>(result->value));
        EXPECT_EQ(std::get<string_t>(result->value).first, "result-v1")
            << "recovery must return v1's stored value, not v2's stale one";
    }
}

TEST_F(TraceCacheIntegrationTest, Recovery_VolatileDep_Fails)
{
    auto db = makeDbBackend();

    // Record trace with volatile dep (Shake: always-dirty rule)
    auto & p = state.tracingPools();
    auto dep = makeCurrentTimeDep(p);
    auto storeResult = withExclusiveStore(*db, [&](const auto & ea) {
        return db->record(ea,
            rootPath(), makeNull(), {dep});
    });

    // Destroy old store (flushes to DB), then create fresh one
    db.reset();
    db = makeDbBackend();

    // Constructive recovery should fail for volatile deps (Shake: always-dirty, no recovery)
    auto result = test::TraceStorageTestAccess::recovery(*db, storeResult.traceId, rootPath(), state);
    EXPECT_FALSE(result.has_value());
}

// ── E-4: mapAttrs / filter structural override warm path ─────────────
//
// These tests document the intended precision boundary for structural override:
// changing an attr key that is NOT accessed by mapAttrs should ideally not
// invalidate the trace. Exact behavior depends on structural dep granularity;
// the test uses EXPECT_NO_THROW to document intent without asserting
// a specific hit/miss outcome until the precision is guaranteed.
//
// E-5 is co-located here (see note below).

TEST_F(TraceCacheIntegrationTest, MapAttrs_ValueChange_CacheMiss)
{
    // Two-key attrset imported via `import` and piped through `mapAttrs`.
    // The fixture never emits a Nix-level ExprSelect on the imported
    // attrset (mapAttrs iterates via the C++ Bindings API), so no
    // per-binding StructuredProjection covers the file.  Post OR-2 fix,
    // ExprParseFile::eval records a FileBytes backstop on the imported
    // file, so a value change to any binding invalidates the trace and
    // the loader re-runs.
    //
    // The previous version of this test asserted `calls == 0` on this
    // same mutation.  That was a false positive: before the OR-2 fix the
    // trace had zero deps on the imported file, so the content edit was
    // invisible and the stale cached result (v + 1 computed against
    // b = 2) was served.  A future structural override extending to Nix
    // AST key sets would recover the precision (comment-only edit → hit),
    // but a value-only change would still correctly invalidate.
    TempTestFile f("{ a = 1; b = 2; }");
    auto exprBase = "builtins.mapAttrs (k: v: v + 1) (import " + f.path.string() + ")";

    // Cold eval — records FileBytes backstop on the imported file.
    {
        auto cache = makeCache(exprBase);
        (void) forceRoot(*cache);
    }

    // Warm hit precondition: unchanged file → cache hit.
    {
        int calls = 0;
        auto cache = makeCache(exprBase, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "precision pre-condition: unchanged file must hit cache";
    }

    // Value change to b (no binding access → no SP coverage).
    f.modify("{ a = 1; b = 99; }");
    invalidateFileCache(f.path);

    int calls = 0;
    auto cache = makeCache(exprBase, &calls);
    (void) forceRoot(*cache);
    EXPECT_EQ(calls, 1)
        << "OR-2 soundness: FileBytes backstop invalidates on any content change "
           "when no per-binding SP dep fires (mapAttrs iterates via C++ API, not ExprSelect)";
}

TEST_F(TraceCacheIntegrationTest, Filter_StructuralOverride_WarmHit)
{
    // builtins.filter on a list imported from a file. Stable content → warm hit.
    TempTestFile f("[1 2 3]");
    auto expr = "builtins.filter (x: x > 1) (import " + f.path.string() + ")";

    // Cold eval
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Warm eval — file unchanged → must hit (Content dep still valid)
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: file content unchanged";
    }
}

// ── E-5: Import cycle loop prevention via verifiedTraceIds ────────────
//
// NOTE: Do NOT use actual cyclic Nix import files. Those throw
// InfiniteRecursionError before the cache layer is reached, so they cannot
// test the TraceValueContext cycle-break logic at all.
//
// Instead, this test uses synthetic DB deps forming a TraceValueContext
// cycle — the same approach as A-6 TraceValueContext_HardcodedHashCycle_MissOnMismatch
// (see store/trace-value-context.cc). The test lives here (integration.cc)
// because it exercises the end-to-end verify path, matching the other
// integration-level cycle tests.
//
// The TraceStoreTest fixture is inherited (TraceStoreFixture), not
// TraceCacheIntegrationTest, because cycle tests operate at the raw
// SqliteTraceStorage level (no TraceSession needed).

TEST_F(TraceCacheIntegrationTest, ImportCycle_TraceValueContext_LoopPrevention)
{
    // Use the SqliteTraceStorage backend directly to set up a synthetic cycle.
    // makeDbBackend() returns a fresh SqliteTraceStorage against the fixture's DB.
    // Mirrors A-6 TraceValueContext_HardcodedHashCycle_MissOnMismatch from
    // store/trace-value-context.cc — same pattern, different fixture.
    auto db = makeDbBackend();

    // Hardcoded stubs: hash mismatch guarantees a miss without filesystem
    // access.  The verifiedTraceIds cycle-break path does NOT fire under
    // these stubs — the hash-mismatch path short-circuits verification
    // first.  Strengthened from EXPECT_NO_THROW to EXPECT_FALSE on the
    // miss outcome (§N.6).
    auto s1 = DepHashValue(depHash("s1"));
    auto s2 = DepHashValue(depHash("s2"));

    withExclusiveStore(*db, [&](const auto & ea) {
        // A→B and B→A via TraceValueContext deps (Dep::makeValueContext is the
        // correct API — TraceValueContext is not a CanonicalQueryKind handled
        // by makeSimpleRecordedDep; it uses a trace-context key format).
        db->record(ea, vpath({"A"}), string_t{"av", {}},
            {Dep::makeValueContext(vpath({"B"}), s1)});
        db->record(ea, vpath({"B"}), string_t{"bv", {}},
            {Dep::makeValueContext(vpath({"A"}), s2)});
    });

    auto result = test::TraceStorageTestAccess::verify(*db, vpath({"A"}), state);
    EXPECT_FALSE(result.has_value())
        << "hardcoded-hash cycle in a cross-import shape: hash mismatch "
           "must miss without stack-overflow or exception";
}

// ── E-6: builtins.readDir — listing change invalidates, content only does not
//
// readDir records a DirectoryEntries dep (listing hash, not file content hash).
// Adding a file changes the listing → miss.
// Changing a file's content WITHOUT adding/removing → hit.

TEST_F(TraceCacheIntegrationTest, ReadDir_ListingChange_Invalidates)
{
    TempDir td;
    td.addFile("existing.txt", "v");
    auto expr = "builtins.readDir \"" + td.path().string() + "\"";

    // Cold eval
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Confirm warm hit (precision pre-condition)
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0);
    }

    // Add a new file — changes directory listing (DirectoryEntries dep changes)
    td.addFile("newfile.txt", "n");
    INVALIDATE_DIR(td);

    // Must re-evaluate (soundness: listing changed)
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "soundness: directory listing change must invalidate readDir trace";
    }
}

// ── §N.7: OR-5 unit-level reproducer via orchestrator scanHistory ─────
//
// Plan reference: `test-drift-fix-plan.md` §N.7 fix. Exercises the
// `scanHistory(stableRecoveryKey, pathId)` fallback at
// `verification-orchestrator.cc` through the real async orchestrator
// path — not `TraceStorageTestAccess`, which bypasses the orchestrator
// entirely.
//
// Why scanHistory fires in this shape (v2 after adversarial review):
//
//   Two sessions with DIFFERENT `semanticSessionKey` but the SAME
//   `stableRecoveryKey`. Session A records a trace under its own
//   `semanticSessionKey.digest`, which also writes a History row under
//   `stableRecoveryKey`. Session B looks up `CurrentTraces` keyed on
//   its own (distinct) `semanticSessionKey.digest` — primary misses.
//   The orchestrator falls through to `scanHistory(pathId)` keyed on
//   `stableRecoveryKey`, finds A's History row, bootstraps from it,
//   and runs `verifyTrace` on the discovered trace.
//
//   Production analogue: two `nix eval` invocations with different
//   `--allowed-uris` values (different policy digest → different
//   `semanticSessionKey`) evaluating the same source file (same
//   source-identity → same `stableRecoveryKey`). OR-5's historical
//   bug was exactly this: a missing `policyDigest` field in the
//   stable recovery key made B bootstrap from A's row even when
//   policy differed, letting policy-sensitive traces leak across
//   sessions. The fix (commit after 90e908ad6) added `policyDigest`
//   to the stable recovery key; this test pins the scanHistory path
//   is still reachable for the LEGITIMATE same-policy different-
//   session-key case.
//
//   The first version of this test (committed then flagged by
//   adversarial review) used the fixture's 2-arg `makeCache` twice
//   with the same expression. Both calls derived the same
//   `perExprFp` (§N.1 mixing is deterministic over `nixExpr`), so
//   `currentSemanticSessionKey()` was identical across calls, primary
//   `lookupCurrentNode` hit directly, and scanHistory was never
//   reached. This version uses explicit `SessionConfig::forTest`
//   calls with distinct `policyDigest` values but the same
//   `stableRecoveryKey` string, producing the bootstrap-required
//   shape.
TEST_F(TraceCacheIntegrationTest, Integration_OR5Reproducer_ScanHistoryServesCrossSession)
{
    TempTextFile f("stable-content-for-or5");
    const std::string expr = "builtins.readFile " + f.path.string();

    constexpr std::string_view kSharedStableRecoveryKey = "or5-repro-stable";
    const auto sessionAPolicy = depHash("or5-policy-A").value;
    const auto sessionBPolicy = depHash("or5-policy-B").value;
    // Bootstrap key seed for the backend's initial `semanticSessionKey_`
    // BEFORE setSessionConfig runs. Does NOT select the SQLite file --
    // SqliteTraceStorage chooses the versioned, hash-algorithm-suffixed cache file
    // under ScopedCacheDir. Both sessions share the SQLite file regardless
    // of this value; session-key differentiation is handled entirely by
    // setSessionConfig.
    const Hash bootstrapSeed =
        hashString(HashAlgorithm::SHA256, "or5-repro-bootstrap-seed");

    // Session A: record a trace with policyA + shared stableRecoveryKey.
    int coldLoaderCalls = 0;
    {
        auto session = makeCacheWithSessionConfig(
            expr, bootstrapSeed,
            SessionConfig::forTest(sessionAPolicy, kSharedStableRecoveryKey),
            &coldLoaderCalls);
        (void) forceRoot(*session);
    }
    ASSERT_EQ(coldLoaderCalls, 1) << "cold eval must invoke the loader";

    // Flush session A to SQLite and drop in-process state.
    simulateWarmRestart();

    // Session B: different semanticSessionKey (policyB), same
    // stableRecoveryKey. The primary `lookupCurrentNode` keyed on
    // B's `semanticSessionKey.digest` will MISS (no CurrentTraces row
    // for B's digest), triggering the orchestrator's scanHistory
    // fallback, which finds A's History row (same stableRecoveryKey).
    int warmLoaderCalls = 0;
    {
        PathCountersSnapshot snap;
        auto session = makeCacheWithSessionConfig(
            expr, bootstrapSeed,
            SessionConfig::forTest(sessionBPolicy, kSharedStableRecoveryKey),
            &warmLoaderCalls);
        (void) forceRoot(*session);

        EXPECT_EQ(warmLoaderCalls, 0)
            << "warm eval must not re-invoke the loader; "
               "scanHistory should have recovered session A's trace";

        // Load-bearing assertion: the orchestrator MUST have taken the
        // scanHistory bootstrap branch (primary lookup missed; history
        // consulted). If future code makes the primary lookup succeed
        // across session-key boundaries, this assertion fires and we
        // know the OR-5 pinning weakened.
        EXPECT_GE(snap.deltaHistoryBootstraps(), 1u)
            << "OR-5 reproducer: primary lookup should miss and "
               "scanHistory should bootstrap from A's History row";

        // Primary verify then succeeds against the bootstrapped row;
        // the 3-strategy recovery fallback is NOT involved.
        EXPECT_GE(snap.deltaTraceCacheHits(), 1u)
            << "the bootstrapped trace must verify";
        EXPECT_EQ(snap.deltaRecoveryAttempts(), 0u)
            << "scanHistory bootstrap should precede any recovery "
               "fallback; recovery() is only invoked on verifyTrace "
               "failure";
    }
}

// ── §N.4/M-2: demonstrate primaryCacheServedOnly() catches scanHistory ──
//
// Companion to the OR-5 reproducer above. Under the same shape (two
// sessions with distinct semanticSessionKey, same stableRecoveryKey),
// scanHistory serves the warm result. The adversarial review pointed
// out that the 58 migrated §N.4 Case A tests all assert
// `primaryCacheServedOnly()` but none of them actually reach
// scanHistory, because §N.1 per-expression fingerprint mixing gives
// each iteration a distinct default stableRecoveryKey (via the
// default-constructed SessionConfig). So the tightened guard is
// defense-in-depth — but "defense-in-depth" is only meaningful if the
// guard CAN fire. This test proves it can:
//
//   1. Exercise a scanHistory-reaching shape (same as OR-5 repro).
//   2. Assert that `primaryCacheServedOnly()` returns FALSE — the
//      warm result came via history bootstrap, NOT primary cache.
//   3. If B-1 regressed (e.g., counter increment moved to the wrong
//      branch or removed), `deltaHistoryBootstraps` would be 0 and
//      `primaryCacheServedOnly()` would return true — this test fails.
//   4. If a future change made `lookupCurrentNode` accidentally
//      succeed across session-key boundaries (OR-5-class regression),
//      `deltaHistoryBootstraps` would again be 0 — this test fails.
TEST_F(TraceCacheIntegrationTest, PrimaryCacheServedOnly_RejectsHistoryBootstrap)
{
    TempTextFile f("content-for-primary-cache-demo");
    const std::string expr = "builtins.readFile " + f.path.string();
    constexpr std::string_view kShared = "m2-demo-stable";
    const Hash bootstrapSeed =
        hashString(HashAlgorithm::SHA256, "m2-demo-bootstrap");

    // Session A: record under policy A.
    {
        auto session = makeCacheWithSessionConfig(
            expr, bootstrapSeed,
            SessionConfig::forTest(depHash("m2-policy-A").value, kShared));
        (void) forceRoot(*session);
    }
    simulateWarmRestart();

    // Session B: different policy, same stable key.
    {
        PathCountersSnapshot snap;
        auto session = makeCacheWithSessionConfig(
            expr, bootstrapSeed,
            SessionConfig::forTest(depHash("m2-policy-B").value, kShared));
        (void) forceRoot(*session);

        // Positive control: the path we KNOW fired.
        EXPECT_GE(snap.deltaHistoryBootstraps(), 1u)
            << "shape precondition: scanHistory must have bootstrapped";
        EXPECT_GE(snap.deltaTraceCacheHits(), 1u)
            << "shape precondition: bootstrapped trace must verify";

        // Load-bearing negative assertion: primaryCacheServedOnly()
        // MUST return false under a scanHistory-served warm result.
        // If this starts returning true, the guard used by 58
        // §N.4 Case A tests has weakened and no longer discriminates
        // history-bootstrap from primary-cache.
        EXPECT_FALSE(snap.primaryCacheServedOnly())
            << "primaryCacheServedOnly() must reject history-bootstrap-"
               "served results; if it returns true here, the §N.4 Case A "
               "guard is broken and would spuriously pass for "
               "history-bootstrap regressions";

        // Weaker `deltaTraceCacheHits >= 1 && deltaRecoveryAttempts == 0`
        // (the pre-B1 shape) WOULD spuriously pass here — documenting
        // what the B1 tightening catches that the old shape did not.
        EXPECT_TRUE(snap.deltaTraceCacheHits() >= 1
                 && snap.deltaRecoveryAttempts() == 0)
            << "the OLD (pre-§N.4) Case A shape would pass here — "
               "this is the spurious pass that B1 closes";
    }
}

TEST_F(TraceCacheIntegrationTest, ReadDir_ContentOnlyChange_WarmHit)
{
    TempDir td;
    td.addFile("f.txt", "v1");
    auto expr = "builtins.readDir \"" + td.path().string() + "\"";

    // Cold eval
    {
        auto cache = makeCache(expr);
        (void) forceRoot(*cache);
    }

    // Overwrite file content — listing is UNCHANGED (same file name, no add/remove)
    td.addFile("f.txt", "v2");  // addFile overwrites; no listing change
    INVALIDATE_DIR(td);         // clear FS accessor cache

    // Warm eval — DirectoryEntries dep hashes listing only, not content
    // → must still hit (precision: content-only change leaves listing intact)
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "precision: content-only change must not invalidate readDir trace";
    }
}

} // namespace nix::eval_trace
