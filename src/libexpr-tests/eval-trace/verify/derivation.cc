// E-2: Derivation evaluation — drvPath access, storePath, toFile.
//
// Tests the eval-trace cache warm/invalidation cycle for derivation-related
// builtins:
//   - drvPath access via import of a .nix file (E-2a)
//   - builtins.toFile recording a RawBytes dep (E-2d)
//   - builtins.storePath (pre-populated via state.store->addToStore — E-2c)
//
// E-2b (IFD: builtins.readFile on a store path) requires a real derivation
// build, which is architecturally impossible in the unit-test fixture
// environment (the fixture uses dummy://).  Functional tests cover IFD
// end-to-end; `DerivedStorePath_TabDelimitedKey_WarmHit` in
// `store/derived-store-path.cc` covers the dep record-verify round-trip.
// A stub is included below with GTEST_SKIP() to preserve intent.
//
// NOTE: TempTestFile uses a .nix extension which is required for `import`
// to recognise the file as a Nix source file.

#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class DerivationIntegrationTest : public TraceCacheFixture
{
public:
    DerivationIntegrationTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "derivation-integration-test");
    }
};

// ── E-2d: builtins.toFile — warm hit and content change ──────────────
//
// builtins.toFile does NOT record any dep in the eval-trace system.
// prim_toFile calls mkStorePathString / allowAndSetStorePathString but never
// calls maybeRecordRawContentDep or any other dep-recording primitive.
// A trace with zero deps always verifies as Valid, so every warm verify
// is a cache hit regardless of what the expression evaluates to.
//
// The result IS deterministic: same name+content → same store path.
// Warm eval must hit; changing the content argument in the expression text
// is irrelevant to the cache because the stored trace has zero deps.

TEST_F(DerivationIntegrationTest, BuiltinsToFile_ContentUnchanged_WarmHit)
{
    auto expr = R"(builtins.toFile "f.txt" "content-v1")";

    // Cold eval — records trace with zero deps
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsString());
    }

    // Warm eval — zero deps → always a cache hit
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: toFile content unchanged";
        EXPECT_THAT(v, IsString());
    }
}

TEST_F(DerivationIntegrationTest, BuiltinsToFile_ContentChange_Invalidation)
{
    // Regression/precision boundary: after the §N.1 fixture fix, distinct
    // Nix expressions land at distinct `(session_key, AttrPathId(0))` slots
    // via fingerprint mixing in `TraceCacheFixture::makeCache`, mirroring
    // production's per-expression `FileEvalExpressionHash` session-key
    // derivation. v2's session key differs from v1's, so v2's cold eval
    // naturally runs — no stale v1 serve from the fixture artifact.
    //
    // The underlying OR-1 precision gap (prim_toFile records no RawBytes
    // dep on its content argument) remains open in production, but is NOT
    // reachable through this fixture anymore. See `src/libexpr/eval-trace/
    // CLAUDE.md` §OR-1 for the reachability analysis; a production repro
    // would require a distinct containing scope that shares one session
    // key but routes two toFile literals through it.

    auto exprV1 = R"(builtins.toFile "f.txt" "content-v1")";
    auto exprV2 = R"(builtins.toFile "f.txt" "content-v2-different")";

    // Cold eval for v1 — records trace with zero deps
    {
        auto cache = makeCache(exprV1);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsString());
    }

    // Warm eval for v1 — zero deps → cache hit (precision pre-condition)
    {
        int calls = 0;
        auto cache = makeCache(exprV1, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: toFile content unchanged";
        EXPECT_THAT(v, IsString());
    }

    // v2 expression: distinct session key (per-expression fingerprint
    // mixing) → cold eval runs → calls == 1.
    {
        int calls = 0;
        auto cache = makeCache(exprV2, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1)
            << "per-expression fingerprint mixing: v2 lands at a different "
               "slot than v1, so v2's cold eval runs";
        EXPECT_THAT(v, IsString());
    }
}

// ── E-2a: import .nix file — warm hit and content change ─────────────
//
// import of a .nix file records a FileBytes dep on the file path.
// TempTestFile uses a .nix extension so import recognises the file.
// When the file content changes, the trace must be invalidated.

TEST_F(DerivationIntegrationTest, DrvPath_ImportNix_WarmHit)
{
    TempTestFile src("42");
    auto expr = "import " + src.path.string();

    // Cold eval (BSàlC: trace recording)
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(42));
    }

    // Warm eval — cache hit
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0);
        EXPECT_THAT(v, IsIntEq(42));
    }
}

TEST_F(DerivationIntegrationTest, DrvPath_ReadFile_InputChange_Invalidation)
{
    // FileBytes dep invalidation: change file content, assert cache miss.
    //
    // Uses builtins.readFile (not import) because EvalState::fileTraceCache
    // memoizes import results within a single process. The test fixture
    // reuses one EvalState across makeCache calls, so import would serve
    // the stale parsed value even after invalidateFileCache clears the FS
    // accessor. readFile goes through the FS accessor
    // directly, which invalidateFileCache does clear.
    TempTextFile src("hello");
    auto expr = "builtins.readFile " + src.path.string();

    // Cold eval
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("hello"));
    }

    // Confirm warm hit (precision pre-condition)
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        (void) forceRoot(*cache);
        EXPECT_EQ(calls, 0);
    }

    // Modify the source — FileBytes dep hash changes.
    src.modify("world");
    invalidateFileCache(src.path);

    // Must re-evaluate (soundness)
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 1) << "soundness: file change must invalidate readFile trace";
        EXPECT_THAT(v, IsStringEq("world"));
    }
}

// ── E-2b stub: IFD — requires a real build store ──────────────────────
//
// IFD (import-from-derivation) requires building a derivation and reading
// the output store path.  The unit-test fixture uses a dummy:// store that
// does not support builds, and functional tests already cover the IFD path
// end-to-end.  The DerivedStorePath record-verify round-trip is covered by
// `DerivedStorePath_TabDelimitedKey_WarmHit` in `store/derived-store-path.cc`.
//
// Kept as a stub rather than enabled with a synthetic-output drv: a
// synthetic variant would add structural precision ("goes through
// realiseCoercedPath") without adding behavioral coverage that functional
// tests don't already provide.  See §8 "Blocked tests" for the rationale.

TEST_F(DerivationIntegrationTest, IFD_WithTracing_WarmHit)
{
    GTEST_SKIP() << "IFD requires a real build store; unit-test fixture uses "
                    "dummy://. Covered by functional tests.";
}

// ── E-2c: builtins.storePath — warm hit via pre-populated store ──────
//
// builtins.storePath takes a string that must be an existing store path.
// We pre-populate the write-enabled dummy:// store by adding a real file
// via state.store->addToStore; the resulting store-path string is then
// usable as an argument to builtins.storePath.  Matches the pattern in
// store/store-path-availability.cc.

TEST_F(DerivationIntegrationTest, BuiltinsStorePath_ContentUnchanged_WarmHit)
{
    TempTextFile f("store-path-v1");
    // Canonicalize to avoid /tmp → /private/tmp symlink failures on macOS.
    auto canonical = std::filesystem::canonical(f.path).string();
    auto storePath = state.store->addToStore(
        std::string(*CanonPath(canonical).baseName()),
        SourcePath(getFSSourceAccessor(), CanonPath(canonical)));
    auto storePathStr = state.store->printStorePath(storePath);

    auto expr = std::format("builtins.storePath \"{}\"", storePathStr);

    // Cold eval — records a StorePathAvailability dep on the store path.
    {
        auto cache = makeCache(expr);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsString());
    }

    // Warm eval — store still has the path → isValidPath true → dep
    // resolves to "valid" → trace verifies → cache hit.
    {
        int calls = 0;
        auto cache = makeCache(expr, &calls);
        auto v = forceRoot(*cache);
        EXPECT_EQ(calls, 0) << "warm hit expected: store path still valid";
        EXPECT_THAT(v, IsString());
    }
}

} // namespace nix::eval_trace
