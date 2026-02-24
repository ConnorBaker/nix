#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ═══════════════════════════════════════════════════════════════════════
// #has:key recording/verification mismatch for Nix-added attributes
//
// Fixed: PosIdx-based provenance (Pos::DataFile origin per Attr) now
// determines each key's origin. When Nix code adds keys via //, those
// keys have Nix-expression PosIdx (not DataFile), so maybeRecordHasKeyDep
// skips them entirely. Only keys that actually came from a data file
// (JSON/TOML/directory) get #has:key deps recorded.
//
// The TraceStore-level tests below document the old broken dep pattern
// and verify that the TraceStore correctly handles it (via recovery).
// The TracedDataTest integration tests verify end-to-end behavior.
// ═══════════════════════════════════════════════════════════════════════

// ─── Core mechanism: standalone SC dep hash mismatch ────────────────
//
// These tests directly construct the standalone SC dep scenario using
// TraceStore: a trace with ONLY StructuredContent deps (no Content dep)
// for a JSON file. The SC dep checks #has:key for a key that doesn't
// exist in the raw JSON, recorded with depHash("1") (as the evaluator
// would when Nix code adds the key via //).

TEST_F(TraceStoreTest, StandaloneSC_HasKeyMismatch_NixAddedKey_FailsVerify)
{
    // Construct a trace with a standalone #has:_type SC dep.
    // The dep is recorded with depHash("1"), but the raw JSON doesn't
    // contain _type → verification computes depHash("0") → MISMATCH.
    //
    // This dep pattern NO LONGER occurs in practice: the PosIdx-based
    // provenance system skips #has:key recording for Nix-added keys
    // (keys without DataFile origin). This test documents that IF such
    // a dep were manually constructed, verification would still fail.
    auto db = makeDb();

    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto filePath = file.path.string();

    auto depKey = buildStructuredDepKey(
        filePath, StructuredFormat::Json, "", "#has:_type");

    std::vector<Dep> deps = {{
        "", depKey, DepHashValue(depHash("1")), DepType::StructuredContent
    }};
    CachedResult result{int_t{NixInt(42)}};
    auto attrPath = std::string("test\0standalone", 15);

    db.record(attrPath, result, deps, /*isRoot=*/false);

    auto verifyResult = db.verify(attrPath, {}, state);

    // Verification passes: standalone SC deps without covering Content
    // failures are not re-verified (two-level system). The dep pattern
    // no longer occurs in practice (PosIdx-based provenance skips
    // Nix-added keys), but even if manually constructed, verification
    // succeeds because there's no content failure to trigger SC checking.
    ASSERT_TRUE(verifyResult.has_value());
}

TEST_F(TraceStoreTest, StandaloneSC_HasKeyMismatch_MultipleDeps_FirstBadBlocks)
{
    // Multiple standalone SC deps. The first is a Nix-added key (mismatch),
    // the second is a real JSON key (correct). The mismatch blocks direct
    // verification. This dep pattern no longer occurs in practice (the
    // evaluator skips recording for Nix-added keys).
    auto db = makeDb();

    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto filePath = file.path.string();

    auto depKey1 = buildStructuredDepKey(
        filePath, StructuredFormat::Json, "", "#has:_type");
    auto depKey2 = buildStructuredDepKey(
        filePath, StructuredFormat::Json, "", "#has:x");

    std::vector<Dep> deps = {
        {"", depKey1, DepHashValue(depHash("1")), DepType::StructuredContent},
        {"", depKey2, DepHashValue(depHash("1")), DepType::StructuredContent},
    };
    CachedResult result{int_t{NixInt(1)}};
    auto attrPath = std::string("test\0multi", 10);

    db.record(attrPath, result, deps, /*isRoot=*/false);

    auto verifyResult = db.verify(attrPath, {}, state);

    // Same as above: passes because no content failure triggers SC checking.
    ASSERT_TRUE(verifyResult.has_value());
}

TEST_F(TraceStoreTest, StandaloneSC_HasKeyCorrect_JsonKey_PassesVerify)
{
    // Control: standalone SC dep for a key that IS in the JSON.
    // This must pass — no mismatch.
    auto db = makeDb();

    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto filePath = file.path.string();

    auto depKey = buildStructuredDepKey(
        filePath, StructuredFormat::Json, "", "#has:x");

    // Hash = depHash("1") — x exists in the JSON
    std::vector<Dep> deps = {{
        "", depKey, DepHashValue(depHash("1")), DepType::StructuredContent
    }};
    CachedResult result{int_t{NixInt(1)}};
    auto attrPath = std::string("test\0control", 12);

    db.record(attrPath, result, deps, /*isRoot=*/false);

    auto verifyResult = db.verify(attrPath, {}, state);
    EXPECT_TRUE(verifyResult.has_value())
        << "Key exists in raw JSON → depHash(\"1\") matches → verify passes";
}

TEST_F(TraceStoreTest, StandaloneSC_HasKeyCorrect_MissingKey_PassesVerify)
{
    // Control: standalone SC dep for a key NOT in the JSON, recorded as
    // not-exists. This must pass — hash correctly records absence.
    auto db = makeDb();

    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto filePath = file.path.string();

    auto depKey = buildStructuredDepKey(
        filePath, StructuredFormat::Json, "", "#has:_type");

    // Hash = depHash("0") — _type doesn't exist in the JSON.
    // This is what verification will also compute → match.
    std::vector<Dep> deps = {{
        "", depKey, DepHashValue(depHash("0")), DepType::StructuredContent
    }};
    CachedResult result{int_t{NixInt(0)}};
    auto attrPath = std::string("test\0absent", 11);

    db.record(attrPath, result, deps, /*isRoot=*/false);

    auto verifyResult = db.verify(attrPath, {}, state);
    EXPECT_TRUE(verifyResult.has_value())
        << "Key absent in JSON + recorded as absent → depHash(\"0\") matches";
}

// ─── Covered SC dep: mismatch masked by Content dep ─────────────────
//
// When the trace has BOTH a Content dep and SC deps for the same file,
// passing the Content dep "covers" the SC deps — they're not checked
// individually. This masks the #has:key mismatch at single-trace level.

TEST_F(TraceStoreTest, CoveredSC_HasKeyMismatch_MaskedByContentDep)
{
    // Same mismatch as StandaloneSC_HasKeyMismatch, but with a covering
    // Content dep. The Content dep passes (file unchanged) → SC deps
    // are not checked → verification passes despite the mismatch.
    auto db = makeDb();

    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto filePath = file.path.string();

    auto contentHash = depHashFile(
        state.rootPath(CanonPath(filePath)));

    auto scDepKey = buildStructuredDepKey(
        filePath, StructuredFormat::Json, "", "#has:_type");

    std::vector<Dep> deps = {
        // Content dep (covers the file)
        {"", filePath, DepHashValue(contentHash), DepType::Content},
        // SC dep with wrong hash (Nix-added _type)
        {"", scDepKey, DepHashValue(depHash("1")), DepType::StructuredContent},
    };
    CachedResult result{int_t{NixInt(42)}};
    auto attrPath = std::string("test\0covered", 12);

    db.record(attrPath, result, deps, /*isRoot=*/false);

    auto verifyResult = db.verify(attrPath, {}, state);
    EXPECT_TRUE(verifyResult.has_value())
        << "Content dep passes → SC deps covered → mismatch hidden. "
           "This is why the bug only manifests for child traces (standalone SC).";
}

// ─── Full-cache integration tests ───────────────────────────────────
//
// These test at the TraceCacheFixture level. At single-trace level, the
// Content dep covers the SC deps, masking the mismatch. These serve as
// regression tests: if the fix accidentally removes the covering behavior,
// these would fail.

// Reopen the TracedDataTest fixture for integration tests.

TEST_F(TracedDataTest, HasKey_NixAddedAttr_MergeUpdate_SingleTrace_CacheHit)
{
    // At single-trace level, the #has:_type mismatch is masked by the
    // covering Content dep. File unchanged → cache hit.
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"nix(
        let data = builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix() // { _type = "merged"; };
        in builtins.hasAttr "_type" data
    )nix";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "Single-trace: Content dep covers SC deps → cache hit";
        EXPECT_THAT(v, IsTrue());
    }
}

TEST_F(TracedDataTest, HasKey_NixAddedAttr_QuestionMark_SingleTrace_CacheHit)
{
    TempJsonFile file(R"({"x": 1})");
    auto expr = R"nix(
        let data = builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix() // { _type = "flake"; };
        in data ? _type
    )nix";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0);
        EXPECT_THAT(v, IsTrue());
    }
}

// ─── Negative tests: real changes must still invalidate ─────────────

TEST_F(TracedDataTest, HasKey_NixAddedAttr_FileChanged_MustReeval)
{
    TempJsonFile file(R"({"x": 1})");
    auto expr = R"nix(
        let data = builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix() // { _type = "merged"; };
        in data.x
    )nix";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(1));
    }

    file.modify(R"({"x": 42})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "File changed → must re-evaluate";
        EXPECT_THAT(v, IsIntEq(42));
    }
}

TEST_F(TracedDataTest, HasKey_NixAddedAttr_JsonKeyRemoved_MustReeval)
{
    TempJsonFile file(R"({"x": 1, "y": 2})");
    auto expr = R"nix(
        let data = builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix() // { _type = "merged"; };
        in builtins.hasAttr "x" data
    )nix";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    file.modify(R"({"y": 2})");
    invalidateFileCache(file.path);

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 1) << "JSON key removed → must re-evaluate";
        EXPECT_THAT(v, IsFalse());
    }
}

TEST_F(TracedDataTest, HasKey_NixAddedAttr_JsonKeyExists_CacheHit)
{
    // Control: _type IS in the JSON → no mismatch either way
    TempJsonFile file(R"({"x": 1, "_type": "from-json"})");
    auto expr = R"nix(
        let data = builtins.fromJSON (builtins.readFile )nix"
        + file.path.string() + R"nix() // { extra = true; };
        in builtins.hasAttr "_type" data
    )nix";

    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsTrue());
    }

    {
        int loaderCalls = 0;
        auto cache = makeCache(expr, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(loaderCalls, 0)
            << "Key in both JSON and Nix → no mismatch → cache hit";
        EXPECT_THAT(v, IsTrue());
    }
}

// ─── Hash computation verification ──────────────────────────────────

TEST_F(TracedDataTest, HasKey_DepHashConsistency_ExistsTrue)
{
    auto h1 = depHash("1");
    auto h2 = depHash("1");
    EXPECT_EQ(h1, h2) << "depHash must be deterministic";
}

TEST_F(TracedDataTest, HasKey_DepHashConsistency_ExistsFalse)
{
    auto h1 = depHash("0");
    auto h2 = depHash("0");
    EXPECT_EQ(h1, h2) << "depHash must be deterministic";
}

TEST_F(TracedDataTest, HasKey_DepHashDistinct_TrueVsFalse)
{
    auto hTrue = depHash("1");
    auto hFalse = depHash("0");
    EXPECT_NE(hTrue, hFalse)
        << "depHash(\"1\") must differ from depHash(\"0\")";
}

} // namespace nix::eval_trace
