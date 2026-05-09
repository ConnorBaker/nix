#include "eval-trace/helpers.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

class TraceCacheTest : public TraceCacheFixture
{
public:
    TraceCacheTest()
    {
        testFingerprint = hashString(HashAlgorithm::SHA256, "test-fingerprint");
    }

protected:
    /**
     * Create a TraceSession with a specific fingerprint (or nullopt).
     * The fingerprint serves as the trace context key (BSàlC: task identity).
     */
    ref<TraceSession> makeCache(
        const std::string & nixExpr,
        std::optional<std::reference_wrapper<const Hash>> fingerprint,
        int * loaderCallCount = nullptr)
    {
        releaseActiveSession();
        auto loader = [this, nixExpr, loaderCallCount]() -> Value * {
            if (loaderCallCount) (*loaderCallCount)++;
            Value v = eval(nixExpr);
            auto * result = state.allocValue();
            *result = v;
            return result;
        };
        std::optional<TraceSession::BackendParams> backendParams;
        if (fingerprint)
            backendParams = TraceSession::BackendParams{*fingerprint, std::nullopt};
        auto session = make_ref<TraceSession>(
            std::move(backendParams), state, std::move(loader));
        activeSession_ = session.get_ptr();
        return session;
    }

    using TraceCacheFixture::makeCache;
};

// ── Construction tests ───────────────────────────────────────────────

TEST_F(TraceCacheTest, Session_Construction_WithFingerprintSucceeds)
{
    auto cache = makeCache("\"hello\"", testFingerprint);
    // ref<TraceSession> is non-nullable by construction.
    (void) cache;
}

TEST_F(TraceCacheTest, Session_Construction_NoFingerprintSucceeds)
{
    auto cache = makeCache("\"hello\"", std::nullopt);
    (void) cache;
}

// ── Root value tests ─────────────────────────────────────────────────

TEST_F(TraceCacheTest, Session_GetRootValue_ReturnsThunk)
{
    auto cache = makeCache("42");
    auto * v = cache->getRootValue();
    ASSERT_NE(v, nullptr);
    // Before forcing, it should be a thunk (Adapton: unevaluated node in DDG)
    EXPECT_TRUE(v->isThunk());
}

TEST_F(TraceCacheTest, Session_GetRootValue_Idempotent)
{
    auto cache = makeCache("42");
    auto * v1 = cache->getRootValue();
    auto * v2 = cache->getRootValue();
    EXPECT_EQ(v1, v2);
}

TEST_F(TraceCacheTest, Session_GetRealRoot_CallsLoader)
{
    int callCount = 0;
    auto cache = makeCache("42", testFingerprint, &callCount);
    cache->getRealRoot();
    EXPECT_EQ(callCount, 1);
}

TEST_F(TraceCacheTest, Session_GetRealRoot_CachedAfterFirst)
{
    int callCount = 0;
    auto cache = makeCache("42", testFingerprint, &callCount);
    auto * v1 = cache->getRealRoot();
    auto * v2 = cache->getRealRoot();
    EXPECT_EQ(v1, v2);
    EXPECT_EQ(callCount, 1); // rootLoader called once (Salsa: query executed)
}

// ── Record -> Verify roundtrip tests (BSàlC: trace recording then verification) ──

/**
 * Helper: evaluate expression through trace cache (fresh evaluation / recording),
 * then create new cache with same fingerprint (verification), and return the
 * verified value. (BSàlC: record trace, then verify trace in new session.)
 */
// COLD_WARM_TEST macro.
//
// Post-§N.1 semantics (2026-04-20): BOTH the cold and warm invocations
// inside this macro use the 2-arg `TraceCacheFixture::makeCache`
// overload. That overload derives the session fingerprint from
// `SHA256(testFingerprint || ":" || nixExpr)` (§N.1 mixing). Using the
// 3-arg overload for the warm call would land the warm session at a
// different slot than the cold session.
//
// Consequence: the macro IGNORES any subclass-provided `testFingerprint`
// beyond using it as the mixing seed. Tests that need explicit-Hash
// session-key control (e.g. `Session_DifferentFingerprints_Isolated`)
// must call `TraceCacheTest::makeCache(expr, fp, &loaderCalls)`
// directly rather than through this macro.
#define COLD_WARM_TEST(test_name, nix_expr, assertion)           \
    TEST_F(TraceCacheTest, ColdWarm_##test_name)                  \
    {                                                            \
        /* Fresh evaluation: record trace */                     \
        {                                                        \
            auto cache = makeCache(nix_expr);                    \
            (void) forceRoot(*cache);                            \
        }                                                        \
        /* Verification: verify trace and serve cached result. */\
        /* Uses the 2-arg fixture overload so the per-expression */\
        /* fingerprint mixing (§N.1) matches the cold session.  */\
        {                                                        \
            int loaderCalls = 0;                                 \
            auto cache = TraceCacheFixture::makeCache(           \
                nix_expr, &loaderCalls);                         \
            auto v = forceRoot(*cache);                          \
            assertion;                                           \
            EXPECT_EQ(loaderCalls, 0)                            \
                << "warm verify must serve cached result without re-running loader"; \
        }                                                        \
    }

COLD_WARM_TEST(String, "\"hello\"",
    EXPECT_THAT(v, IsStringEq("hello")))

COLD_WARM_TEST(Int, "42",
    EXPECT_THAT(v, IsIntEq(42)))

COLD_WARM_TEST(Bool_True, "true",
    EXPECT_THAT(v, IsTrue()))

COLD_WARM_TEST(Bool_False, "false",
    EXPECT_THAT(v, IsFalse()))

COLD_WARM_TEST(Null, "null",
    EXPECT_THAT(v, IsNull()))

COLD_WARM_TEST(Float, "3.14",
    EXPECT_THAT(v, IsFloatEq(3.14)))

COLD_WARM_TEST(Path, "/tmp",
    EXPECT_THAT(v, IsPathEq("/tmp")))

#undef COLD_WARM_TEST

TEST_F(TraceCacheTest, ColdWarm_Attrset_VerifyHit)
{
    // Fresh evaluation: record trace
    {
        auto cache = makeCache("{ a = 1; b = 2; }");
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsAttrsOfSize(2));
    }
    // Verification: verify trace and serve cached result
    {
        int loaderCalls = 0;
        auto cache = makeCache("{ a = 1; b = 2; }", &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsAttrsOfSize(2));
        EXPECT_EQ(loaderCalls, 0);
    }
}

TEST_F(TraceCacheTest, ColdWarm_SameFingerprint_ReusesTracedResult)
{
    auto expr = "1 + 2";

    int coldLoaderCalls = 0;
    {
        auto cache = makeCache(expr, testFingerprint, &coldLoaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }
    EXPECT_EQ(coldLoaderCalls, 1);

    int warmLoaderCalls = 0;
    {
        auto cache = makeCache(expr, testFingerprint, &warmLoaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsIntEq(3));
    }
    EXPECT_EQ(warmLoaderCalls, 0)
        << "same fingerprint should replay traced result without re-running loader";
}

TEST_F(TraceCacheTest, ColdWarm_NestedAttrset_VerifyHit)
{
    // Fresh evaluation: record traces for nested structure
    {
        auto cache = makeCache("{ x = { y = 42; }; }");
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsAttrsOfSize(1));
        auto * x = v.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsAttrsOfSize(1));
        auto * y = x->value->attrs()->get(createSymbol("y"));
        ASSERT_NE(y, nullptr);
        state.forceValue(*y->value, noPos);
        EXPECT_THAT(*y->value, IsIntEq(42));
    }
    // Verification: verify traces and serve cached results
    {
        int loaderCalls = 0;
        auto cache = makeCache("{ x = { y = 42; }; }", &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsAttrsOfSize(1));
        auto * x = v.attrs()->get(createSymbol("x"));
        ASSERT_NE(x, nullptr);
        state.forceValue(*x->value, noPos);
        EXPECT_THAT(*x->value, IsAttrsOfSize(1));
        auto * y = x->value->attrs()->get(createSymbol("y"));
        ASSERT_NE(y, nullptr);
        state.forceValue(*y->value, noPos);
        EXPECT_THAT(*y->value, IsIntEq(42));
        EXPECT_EQ(loaderCalls, 0);
    }
}

TEST_F(TraceCacheTest, ColdWarm_StringList_CanonicalList)
{
    {
        auto cache = makeCache("[\"a\" \"b\" \"c\"]");
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsListOfSize(3));
    }
    {
        int loaderCalls = 0;
        auto cache = makeCache("[\"a\" \"b\" \"c\"]", &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsListOfSize(3));
        EXPECT_EQ(loaderCalls, 0);
    }
}

TEST_F(TraceCacheTest, ColdWarm_List_VerifyHit)
{
    {
        auto cache = makeCache("[1 2 3]");
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsListOfSize(3));
    }
    {
        int loaderCalls = 0;
        auto cache = makeCache("[1 2 3]", &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsListOfSize(3));
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── Error trace tests ────────────────────────────────────────────────

TEST_F(TraceCacheTest, ColdWarm_Failed_ReEvaluates)
{
    // Fresh evaluation — force error, record failed trace
    {
        auto cache = makeCache("throw \"test error\"");
        EXPECT_THROW(forceRoot(*cache), ThrownError);
    }
    // Verification — failed_t triggers fresh re-evaluation (BSàlC: no caching of failures)
    {
        auto cache = makeCache("throw \"test error\"");
        EXPECT_THROW(forceRoot(*cache), ThrownError);
    }
}

// ── No-backend mode (no trace store — direct evaluation only) ────────

TEST_F(TraceCacheTest, Session_NoBackend_EvalsDirect)
{
    int callCount = 0;
    auto cache = makeCache("42", std::nullopt, &callCount);
    auto v = forceRoot(*cache);
    EXPECT_THAT(v, IsIntEq(42));
    EXPECT_EQ(callCount, 1); // rootLoader called (no trace store -> always fresh evaluation)
}

TEST_F(TraceCacheTest, Session_NoBackend_AttrsetAccess)
{
    auto cache = makeCache("{ a = 1; b = 2; }", std::nullopt);
    auto v = forceRoot(*cache);
    EXPECT_THAT(v, IsAttrsOfSize(2));
}

TEST_F(TraceCacheTest, Session_NoBackend_NoCacheReuseAcrossSessions)
{
    int loaderCalls = 0;

    {
        auto cache1 = makeCache("1 + 2", std::nullopt, &loaderCalls);
        auto v1 = forceRoot(*cache1);
        EXPECT_THAT(v1, IsIntEq(3));
    }

    {
        auto cache2 = makeCache("1 + 2", std::nullopt, &loaderCalls);
        auto v2 = forceRoot(*cache2);
        EXPECT_THAT(v2, IsIntEq(3));
    }

    EXPECT_EQ(loaderCalls, 2)
        << "no-backend mode should rerun loader per session";
}

TEST_F(TraceCacheTest, Session_NoBackend_SingleSessionCachesRoot)
{
    int loaderCalls = 0;

    {
        ScopedEnvVar env("NIX_NO_BACKEND_SINGLE_SESSION", "cold");
        auto cache = makeCache("builtins.getEnv \"NIX_NO_BACKEND_SINGLE_SESSION\"", std::nullopt, &loaderCalls);

        auto v1 = forceRoot(*cache);
        EXPECT_THAT(v1, IsStringEq("cold"));

        {
            ScopedEnvVar env2("NIX_NO_BACKEND_SINGLE_SESSION", "warm");
            auto v2 = forceRoot(*cache);
            EXPECT_THAT(v2, IsStringEq("cold"));
        }

        auto v2 = forceRoot(*cache);
        EXPECT_THAT(v2, IsStringEq("cold"));
    }

    EXPECT_EQ(loaderCalls, 1)
        << "A single no-backend session should evaluate root thunk once";
}

TEST_F(TraceCacheTest, Session_NoBackend_RootThunkAndRealRootShareCache)
{
    int loaderCalls = 0;

    {
        ScopedEnvVar env("NIX_NO_BACKEND_SINGLE_SESSION", "cold");
        auto cache = makeCache("builtins.getEnv \"NIX_NO_BACKEND_SINGLE_SESSION\"", std::nullopt, &loaderCalls);

        auto * thunk1 = cache->getRootValue();
        auto * root1 = cache->getRealRoot();
        auto * thunk2 = cache->getRootValue();
        auto * root2 = cache->getRealRoot();

        EXPECT_TRUE(thunk1->isThunk() || thunk1 == thunk2);
        EXPECT_EQ(thunk1, thunk2);
        EXPECT_EQ(root1, root2);
        EXPECT_THAT(*root1, IsStringEq("cold"));
        EXPECT_THAT(*root2, IsStringEq("cold"));

        {
            ScopedEnvVar env2("NIX_NO_BACKEND_SINGLE_SESSION", "warm");
            auto * root3 = cache->getRealRoot();
            EXPECT_EQ(root2, root3);
            EXPECT_THAT(*root3, IsStringEq("cold"));
        }
    }

    EXPECT_EQ(loaderCalls, 1)
        << "A single no-backend session should evaluate root thunk exactly once";
}

TEST_F(TraceCacheTest, Session_NoBackend_EnvVarChange_RespectsCurrentState)
{
    int loaderCalls = 0;

    {
        ScopedEnvVar env("NIX_NO_BACKEND_EVAL", "cold");
        auto cache = makeCache("builtins.getEnv \"NIX_NO_BACKEND_EVAL\"", std::nullopt, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("cold"));
    }

    {
        ScopedEnvVar env("NIX_NO_BACKEND_EVAL", "warm");
        auto cache = makeCache("builtins.getEnv \"NIX_NO_BACKEND_EVAL\"", std::nullopt, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("warm"));
    }

    EXPECT_EQ(loaderCalls, 2)
        << "no-backend mode should re-evaluate each session with current env state";
}

// ── Multiple trace caches with different fingerprints (BSàlC: isolated task traces) ──

TEST_F(TraceCacheTest, Session_DifferentFingerprints_Isolated)
{
    auto fp1 = hashString(HashAlgorithm::SHA256, "fingerprint-1");
    auto fp2 = hashString(HashAlgorithm::SHA256, "fingerprint-2");

    // Record trace with fp1
    {
        auto cache = makeCache("\"value-1\"", fp1);
        forceRoot(*cache);
    }
    // Record trace with fp2
    {
        auto cache = makeCache("\"value-2\"", fp2);
        forceRoot(*cache);
    }
    // Verify with fp1 should serve value-1 from its trace
    {
        int loaderCalls = 0;
        auto cache = makeCache("\"value-1\"", fp1, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("value-1"));
        EXPECT_EQ(loaderCalls, 0);
    }
    // Verify with fp2 should serve value-2 from its trace
    {
        int loaderCalls = 0;
        auto cache = makeCache("\"value-2\"", fp2, &loaderCalls);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("value-2"));
        EXPECT_EQ(loaderCalls, 0);
    }
}

// ── Fixed-point cycle regression tests ───────────────────────────────

/**
 * Regression test for infinite recursion caused by eval trace's eager
 * drvPath forcing during fresh evaluation for origExpr wrappers.
 *
 * Pattern: fixed-point package set with buildPackages = self (native build).
 * Dependency chain (Adapton DDG cycle): blas -> perl -> libxcrypt -> buildPackages.perl -> cycle.
 * Entry point: opencv references ${blas.name} (simple attr access).
 *
 * Without the fix: fresh evaluation for blas (origExpr wrapper, wrapped by
 * navigateToReal) eagerly forces drvPath -> derivationStrict for blas ->
 * chain through perl -> libxcrypt -> buildPackages.perl -> self.perl ->
 * drvPath blackholed -> infinite recursion.
 *
 * With the fix: origExpr wrappers skip drvPath forcing during fresh evaluation,
 * matching normal evaluation order where derivationStrict only runs on string coercion.
 */
TEST_F(TraceCacheTest, Session_FixedPointCycle_NoInfiniteRecursion)
{
    auto nixExpr = R"(
        let
          fix = f: let self = f self; in self;
          pkgs = fix (self: {
            blas = derivation {
              name = "blas";
              builder = "/bin/sh";
              system = builtins.currentSystem;
              perlDep = self.perl;
            };
            perl = derivation {
              name = "perl";
              builder = "/bin/sh";
              system = builtins.currentSystem;
              libxcryptDep = self.libxcrypt;
            };
            libxcrypt = derivation {
              name = "libxcrypt";
              builder = "/bin/sh";
              system = builtins.currentSystem;
              perlBuild = self.buildPackages.perl;
            };
            opencv = derivation {
              name = "opencv-with-${self.blas.name}";
              builder = "/bin/sh";
              system = builtins.currentSystem;
            };
            buildPackages = self;
          });
        in pkgs
    )";

    // Fresh evaluation (BSàlC: trace recording): navigate to opencv.drvPath.
    // Without the fix, forcing drvPath here would trigger infinite recursion
    // because navigateToReal wraps blas with TracedExpr origExpr, and
    // fresh evaluation would eagerly force blas.drvPath -> derivationStrict ->
    // dependency chain through perl -> libxcrypt -> buildPackages.perl -> cycle.
    {
        auto cache = makeCache(nixExpr);
        auto root = forceRoot(*cache);
        EXPECT_THAT(root, IsAttrsOfSize(5));

        auto * opencv = root.attrs()->get(createSymbol("opencv"));
        ASSERT_NE(opencv, nullptr);
        state.forceValue(*opencv->value, noPos);
        EXPECT_TRUE(state.isDerivation(*opencv->value));

        auto * drvPath = opencv->value->attrs()->get(state.s.drvPath);
        ASSERT_NE(drvPath, nullptr);
        state.forceValue(*drvPath->value, noPos);
        EXPECT_THAT(*drvPath->value, IsString());
    }

    // Verification: same access should work from verified trace
    {
        auto cache = makeCache(nixExpr);
        auto root = forceRoot(*cache);

        auto * opencv = root.attrs()->get(createSymbol("opencv"));
        ASSERT_NE(opencv, nullptr);
        state.forceValue(*opencv->value, noPos);

        auto * drvPath = opencv->value->attrs()->get(state.s.drvPath);
        ASSERT_NE(drvPath, nullptr);
        state.forceValue(*drvPath->value, noPos);
        EXPECT_THAT(*drvPath->value, IsString());
    }
}

/**
 * Early blackhole exit: verify that origExpr wrappers properly exit the
 * blackhole on the Value* before derivationStrict runs during fresh evaluation,
 * so that other attributes referencing the same Value* through the fixed-point
 * (Adapton: shared node in DDG) don't see a blackhole.
 */
TEST_F(TraceCacheTest, Session_FixedPointCycle_EarlyBlackholeExit)
{
    auto nixExpr = R"(
        let
          fix = f: let self = f self; in self;
          pkgs = fix (self: {
            a = derivation {
              name = "a";
              builder = "/bin/sh";
              system = builtins.currentSystem;
            };
            b = derivation {
              name = "b-uses-${self.a.name}";
              builder = "/bin/sh";
              system = builtins.currentSystem;
            };
          });
        in pkgs
    )";

    // Access b.drvPath — b's name references a.name, which requires
    // forcing sibling a through its TracedExpr origExpr wrapper during fresh evaluation.
    // The early blackhole exit (v = *target) ensures a is available (Adapton: force completes).
    {
        auto cache = makeCache(nixExpr);
        auto root = forceRoot(*cache);

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);

        auto * drvPath = b->value->attrs()->get(state.s.drvPath);
        ASSERT_NE(drvPath, nullptr);
        state.forceValue(*drvPath->value, noPos);
        EXPECT_THAT(*drvPath->value, IsString());
    }

    // Verification path (BSàlC: verify trace and serve cached result)
    {
        auto cache = makeCache(nixExpr);
        auto root = forceRoot(*cache);

        auto * b = root.attrs()->get(createSymbol("b"));
        ASSERT_NE(b, nullptr);
        state.forceValue(*b->value, noPos);

        auto * drvPath = b->value->attrs()->get(state.s.drvPath);
        ASSERT_NE(drvPath, nullptr);
        state.forceValue(*drvPath->value, noPos);
        EXPECT_THAT(*drvPath->value, IsString());
    }
}

} // namespace nix::eval_trace
