#include "helpers.hh"
#include "nix/expr/eval-cache-db.hh"

#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/util/hash.hh"

namespace nix::eval_cache {

using namespace nix::eval_cache::test;

class EvalCacheTest : public LibExprTest
{
public:
    EvalCacheTest()
        : LibExprTest(openStore("dummy://", {{"read-only", "false"}}),
            [](bool & readOnlyMode) {
                readOnlyMode = false;
                EvalSettings s{readOnlyMode};
                s.nixPath = {};
                return s;
            })
    {}

protected:
    // Writable cache dir for EvalCacheDb SQLite (sandbox has no writable $HOME)
    ScopedCacheDir cacheDir;

    Hash testFingerprint = hashString(HashAlgorithm::SHA256, "test-fingerprint");

    /**
     * Create an EvalCache with a rootLoader that evaluates a Nix expression string.
     */
    std::unique_ptr<EvalCache> makeCache(
        const std::string & nixExpr,
        std::optional<std::reference_wrapper<const Hash>> fingerprint,
        int * loaderCallCount = nullptr)
    {
        auto loader = [this, nixExpr, loaderCallCount]() -> Value * {
            if (loaderCallCount) (*loaderCallCount)++;
            Value v = eval(nixExpr);
            auto * result = state.allocValue();
            *result = v;
            return result;
        };
        return std::make_unique<EvalCache>(fingerprint, state, std::move(loader));
    }

    /**
     * Create a cache with default fingerprint.
     */
    std::unique_ptr<EvalCache> makeCache(const std::string & nixExpr)
    {
        return makeCache(nixExpr, testFingerprint);
    }

    /**
     * Force a cache's root value to completion.
     */
    Value forceRoot(EvalCache & cache)
    {
        auto * v = cache.getRootValue();
        state.forceValue(*v, noPos);
        return *v;
    }
};

// ── Construction tests ───────────────────────────────────────────────

TEST_F(EvalCacheTest, Construction_WithFingerprint)
{
    auto cache = makeCache("\"hello\"", testFingerprint);
    EXPECT_NE(cache, nullptr);
}

TEST_F(EvalCacheTest, Construction_NoFingerprint)
{
    auto cache = makeCache("\"hello\"", std::nullopt);
    EXPECT_NE(cache, nullptr);
}

// ── Root value tests ─────────────────────────────────────────────────

TEST_F(EvalCacheTest, GetRootValue_ReturnsThunk)
{
    auto cache = makeCache("42");
    auto * v = cache->getRootValue();
    ASSERT_NE(v, nullptr);
    // Before forcing, it should be a thunk
    EXPECT_TRUE(v->isThunk());
}

TEST_F(EvalCacheTest, GetRootValue_Idempotent)
{
    auto cache = makeCache("42");
    auto * v1 = cache->getRootValue();
    auto * v2 = cache->getRootValue();
    EXPECT_EQ(v1, v2);
}

TEST_F(EvalCacheTest, GetOrEvaluateRoot_CallsLoader)
{
    int callCount = 0;
    auto cache = makeCache("42", testFingerprint, &callCount);
    cache->getOrEvaluateRoot();
    EXPECT_EQ(callCount, 1);
}

TEST_F(EvalCacheTest, GetOrEvaluateRoot_CachedAfterFirst)
{
    int callCount = 0;
    auto cache = makeCache("42", testFingerprint, &callCount);
    auto * v1 = cache->getOrEvaluateRoot();
    auto * v2 = cache->getOrEvaluateRoot();
    EXPECT_EQ(v1, v2);
    EXPECT_EQ(callCount, 1); // only called once
}

// ── Cold → Warm roundtrip tests ──────────────────────────────────────

/**
 * Helper: evaluate expression through cache (cold), then create new cache
 * with same fingerprint (warm), and return the warm-served value.
 */
#define COLD_WARM_TEST(test_name, nix_expr, assertion)           \
    TEST_F(EvalCacheTest, ColdWarm_##test_name)                  \
    {                                                            \
        /* Cold eval */                                          \
        {                                                        \
            auto cache = makeCache(nix_expr);                    \
            (void) forceRoot(*cache);                            \
        }                                                        \
        /* Warm eval */                                          \
        {                                                        \
            int loaderCalls = 0;                                 \
            auto cache = makeCache(nix_expr, testFingerprint,    \
                                    &loaderCalls);               \
            auto v = forceRoot(*cache);                          \
            assertion;                                           \
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

TEST_F(EvalCacheTest, ColdWarm_Attrset)
{
    // Cold eval
    {
        auto cache = makeCache("{ a = 1; b = 2; }");
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsAttrsOfSize(2));
    }
    // Warm eval
    {
        auto cache = makeCache("{ a = 1; b = 2; }");
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsAttrsOfSize(2));
    }
}

TEST_F(EvalCacheTest, ColdWarm_NestedAttrset)
{
    // Cold eval
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
    // Warm eval
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
}

TEST_F(EvalCacheTest, ColdWarm_ListOfStrings)
{
    {
        auto cache = makeCache("[\"a\" \"b\" \"c\"]");
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsListOfSize(3));
    }
    {
        auto cache = makeCache("[\"a\" \"b\" \"c\"]");
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsListOfSize(3));
    }
}

TEST_F(EvalCacheTest, ColdWarm_List)
{
    {
        auto cache = makeCache("[1 2 3]");
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsListOfSize(3));
    }
    {
        auto cache = makeCache("[1 2 3]");
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsListOfSize(3));
    }
}

// ── Error caching tests ──────────────────────────────────────────────

TEST_F(EvalCacheTest, ColdWarm_Failed)
{
    // Cold eval — force error
    {
        auto cache = makeCache("throw \"test error\"");
        EXPECT_THROW(forceRoot(*cache), ThrownError);
    }
    // Warm eval — should re-throw (failed_t triggers cold re-eval)
    {
        auto cache = makeCache("throw \"test error\"");
        EXPECT_THROW(forceRoot(*cache), ThrownError);
    }
}

// ── No-backend mode ──────────────────────────────────────────────────

TEST_F(EvalCacheTest, NoBackend_EvalsDirect)
{
    int callCount = 0;
    auto cache = makeCache("42", std::nullopt, &callCount);
    auto v = forceRoot(*cache);
    EXPECT_THAT(v, IsIntEq(42));
    EXPECT_EQ(callCount, 1); // rootLoader called
}

TEST_F(EvalCacheTest, NoBackend_AttrsetAccess)
{
    auto cache = makeCache("{ a = 1; b = 2; }", std::nullopt);
    auto v = forceRoot(*cache);
    EXPECT_THAT(v, IsAttrsOfSize(2));
}

// ── Multiple caches with different fingerprints ──────────────────────

TEST_F(EvalCacheTest, DifferentFingerprints_Isolated)
{
    auto fp1 = hashString(HashAlgorithm::SHA256, "fingerprint-1");
    auto fp2 = hashString(HashAlgorithm::SHA256, "fingerprint-2");

    // Cold store with fp1
    {
        auto cache = makeCache("\"value-1\"", fp1);
        forceRoot(*cache);
    }
    // Cold store with fp2
    {
        auto cache = makeCache("\"value-2\"", fp2);
        forceRoot(*cache);
    }
    // Warm with fp1 should get value-1
    {
        auto cache = makeCache("\"value-1\"", fp1);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("value-1"));
    }
    // Warm with fp2 should get value-2
    {
        auto cache = makeCache("\"value-2\"", fp2);
        auto v = forceRoot(*cache);
        EXPECT_THAT(v, IsStringEq("value-2"));
    }
}

// ── Fixed-point cycle regression tests ───────────────────────────────

/**
 * Regression test for infinite recursion caused by eval cache's eager
 * drvPath forcing in evaluateCold for origExpr wrappers.
 *
 * Pattern: fixed-point package set with buildPackages = self (native build).
 * Dependency chain: blas → perl → libxcrypt → buildPackages.perl → cycle.
 * Entry point: opencv references ${blas.name} (simple attr access).
 *
 * Without the fix: evaluateCold for blas (origExpr wrapper, wrapped by
 * navigateToReal) eagerly forces drvPath → derivationStrict for blas →
 * chain through perl → libxcrypt → buildPackages.perl → self.perl →
 * drvPath blackholed → infinite recursion.
 *
 * With the fix: origExpr wrappers skip drvPath forcing, matching normal
 * evaluation order where derivationStrict only runs on string coercion.
 */
TEST_F(EvalCacheTest, FixedPointCycle_NoInfiniteRecursion)
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

    // Cold: navigate to opencv.drvPath through the eval cache.
    // Without the fix, forcing drvPath here would trigger infinite recursion
    // because navigateToReal wraps blas with ExprCached origExpr, and
    // evaluateCold would eagerly force blas.drvPath → derivationStrict →
    // dependency chain through perl → libxcrypt → buildPackages.perl → cycle.
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

    // Warm: same access should work from cache
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
 * blackhole on the Value* before derivationStrict runs, so that other
 * attributes referencing the same Value* through the fixed-point don't
 * see a blackhole.
 */
TEST_F(EvalCacheTest, FixedPointCycle_EarlyBlackholeExit)
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
    // forcing sibling a through its ExprCached origExpr wrapper.
    // The early blackhole exit (v = *target) ensures a is available.
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

    // Warm path
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

} // namespace nix::eval_cache
