#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/hash.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── CopiedPath dep key format: canonical typed encoding ─────────────
//
// computeCurrentHash extracts the store name from the typed dep key, never
// reading expectedHash. This is critical because structural variant
// recovery constructs deps with a placeholder expectedHash (EvalTraceHash{}).

/// Compute the CopiedPath store path for a test file using its baseName.
static std::string computeTestStorePath(EvalState & state, const std::filesystem::path & filePath)
{
    auto baseName = std::string(*CanonPath(filePath.string()).baseName());
    return state.store->printStorePath(
        std::get<0>(state.store->computeStorePath(
            baseName,
            SourcePath(getFSSourceAccessor(), CanonPath(filePath.string()))
                .resolveSymlinks(SymlinkResolution::Ancestors),
            ContentAddressMethod::Raw::NixArchive,
            HashAlgorithm::SHA256, {})));
}

// ── Verification (verifyTrace) ──────────────────────────────────────

TEST_F(TraceStoreTest, CopiedPath_FileUnchanged_VerifyPasses)
{
    TempTestFile file("copiedpath_verify_content");

    auto db = makeDb();
    auto storePath = computeTestStorePath(state, file.path);
    auto baseName = std::string(*CanonPath(file.path.string()).baseName());

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result", {}}, {
            makeCopiedPathDep(pools(), file.path.string(), baseName, storePath),
        });
    });

    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, CopiedPath_FileChanges_VerifyFails)
{
    TempTestFile file("copiedpath_verify_original");

    auto db = makeDb();
    auto storePath = computeTestStorePath(state, file.path);
    auto baseName = std::string(*CanonPath(file.path.string()).baseName());

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result", {}}, {
            makeCopiedPathDep(pools(), file.path.string(), baseName, storePath),
        });
    });

    // Modify file → store path changes → verification fails
    file.modify("copiedpath_verify_modified_content");
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TraceStoreTest, CopiedPath_FileDeleted_VerifyFails)
{
    TempTestFile file("copiedpath_verify_deleted");

    auto db = makeDb();
    auto storePath = computeTestStorePath(state, file.path);
    auto baseName = std::string(*CanonPath(file.path.string()).baseName());

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result", {}}, {
            makeCopiedPathDep(pools(), file.path.string(), baseName, storePath),
        });
    });

    std::filesystem::remove(file.path);
    getFSSourceAccessor()->invalidateCache();
    recreateDb(db);

    // Missing file → sentinel hash ≠ stored store path → fails
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value());
}

// ── Direct hash recovery ────────────────────────────────────────────

TEST_F(TraceStoreTest, CopiedPath_DirectHash_RecoverySucceeds)
{
    // T1 and T2 share the same dep key set. Revert env → direct hash
    // matches T1. CopiedPath dep is computed from oldDeps (real expectedHash).
    ScopedEnvVar envA("NIX_CPDH_A", "a1");
    TempTestFile file("copiedpath_directhash_content");

    auto db = makeDb();
    auto storePath = computeTestStorePath(state, file.path);
    auto baseName = std::string(*CanonPath(file.path.string()).baseName());

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result_a1", {}}, {
            makeEnvVarDep(pools(), "NIX_CPDH_A", "a1"),
            makeCopiedPathDep(pools(), file.path.string(), baseName, storePath),
        });

        setenv("NIX_CPDH_A", "a2", 1);
        db->record(ea, rootPath(), string_t{"result_a2", {}}, {
            makeEnvVarDep(pools(), "NIX_CPDH_A", "a2"),
            makeCopiedPathDep(pools(), file.path.string(), baseName, storePath),
        });
    });

    setenv("NIX_CPDH_A", "a1", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result_a1", {}}, result->value, state.symbols);
}

// ── Structural variant recovery ─────────────────────────────────────

TEST_F(TraceStoreTest, CopiedPath_StructVariant_RecoverySucceeds)
{
    // CopiedPath dep in a structural variant NOT in the old trace's key set.
    // Store name is encoded in the canonical dep key → computeCurrentHash
    // derives it without reading expectedHash → structural variant recovery
    // succeeds.
    ScopedEnvVar envA("NIX_CPSV_A", "aval");
    ScopedEnvVar envB("NIX_CPSV_B", "bval");
    TempTestFile file("copiedpath_structvariant_content");

    auto db = makeDb();
    auto storePath = computeTestStorePath(state, file.path);
    auto baseName = std::string(*CanonPath(file.path.string()).baseName());

    withExclusiveStore(*db, [&](const auto & ea) {
        // T1: EnvVar(A) + CopiedPath(file) — the variant to recover
        db->record(ea, rootPath(), string_t{"result_with_copied", {}}, {
            makeEnvVarDep(pools(), "NIX_CPSV_A", "aval"),
            makeCopiedPathDep(pools(), file.path.string(), baseName, storePath),
        });

        // T2: EnvVar(A) + EnvVar(B) — different structure, no CopiedPath
        db->record(ea, rootPath(), string_t{"result_envonly", {}}, {
            makeEnvVarDep(pools(), "NIX_CPSV_A", "aval"),
            makeEnvVarDep(pools(), "NIX_CPSV_B", "bval"),
        });
    });

    setenv("NIX_CPSV_B", "bval_new", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result_with_copied", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, CopiedPath_StructVariantWithSharedKey_RecoverySucceeds)
{
    // CopiedPath dep in BOTH old trace and structural variant (same key).
    // Direct hash path caches the value → variant reuses cache → succeeds.
    ScopedEnvVar envB("NIX_CPSK_B", "bval");
    ScopedEnvVar envC("NIX_CPSK_C", "cval");
    TempTestFile file("copiedpath_sharedkey_content");

    auto db = makeDb();
    auto storePath = computeTestStorePath(state, file.path);
    auto baseName = std::string(*CanonPath(file.path.string()).baseName());

    withExclusiveStore(*db, [&](const auto & ea) {
        db->record(ea, rootPath(), string_t{"result_C", {}}, {
            makeCopiedPathDep(pools(), file.path.string(), baseName, storePath),
            makeEnvVarDep(pools(), "NIX_CPSK_C", "cval"),
        });

        db->record(ea, rootPath(), string_t{"result_B", {}}, {
            makeCopiedPathDep(pools(), file.path.string(), baseName, storePath),
            makeEnvVarDep(pools(), "NIX_CPSK_B", "bval"),
        });
    });

    setenv("NIX_CPSK_B", "bval_new", 1);
    recreateDb(db);

    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    ASSERT_TRUE(result.has_value());
    assertCachedResultEquals(string_t{"result_C", {}}, result->value, state.symbols);
}

TEST_F(TraceStoreTest, CopiedPath_StructVariantMissingFile_RecoveryFails)
{
    // Structural variant has CopiedPath dep on a deleted file.
    // computeCurrentHash returns sentinel → recovery computes all deps
    // (allComputable=true) but sentinel won't match stored store path →
    // no historical trace matches → recovery fails gracefully.
    ScopedEnvVar envB("NIX_CPMF_B", "bval");
    TempTestFile file("copiedpath_missing_file_content");

    auto db = makeDb();
    auto storePath = computeTestStorePath(state, file.path);
    auto baseName = std::string(*CanonPath(file.path.string()).baseName());

    withExclusiveStore(*db, [&](const auto & ea) {
        // T1: CopiedPath(file) — variant
        db->record(ea, rootPath(), string_t{"result_cp", {}}, {
            makeCopiedPathDep(pools(), file.path.string(), baseName, storePath),
        });

        // T2: EnvVar(B) — current trace
        db->record(ea, rootPath(), string_t{"result_env", {}}, {
            makeEnvVarDep(pools(), "NIX_CPMF_B", "bval"),
        });
    });

    // Delete file AND invalidate B → both traces fail verification
    std::filesystem::remove(file.path);
    getFSSourceAccessor()->invalidateCache();
    setenv("NIX_CPMF_B", "bval_new", 1);
    recreateDb(db);

    // T2 fails (B changed). Structural variant T1: CopiedPath returns
    // sentinel (file gone), sentinel ≠ stored store path → no match.
    auto result = test::TraceStorageTestAccess::verify(*db, rootPath(), state);
    EXPECT_FALSE(result.has_value());
}

} // namespace nix::eval_trace
