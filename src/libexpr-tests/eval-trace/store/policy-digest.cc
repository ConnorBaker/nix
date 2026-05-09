/**
 * Tests for computePolicyDigest — verifies that both NIX_PATH env and
 * settings.nixPath config affect the digest (BUG-11 regression guard),
 * and that nixPath is ignored in pure mode.
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/store/session-policy.hh"
#include "nix/expr/eval-settings.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

// ── computePolicyDigest — nixPath coverage (BUG-11 regression guard) ──
//
// BUG-11: computePolicyDigest included NIX_PATH env var but omitted
// settings.nixPath (the nix-path config key / --nix-path CLI flag).
// Two sessions with the same NIX_PATH but different --nix-path would share
// a policy digest and therefore share a session key, causing stale cached
// results to be served across lookup-path changes.
//
// The fix added settings.nixPath.get() to the digest after the NIX_PATH
// field.  These tests verify that invariant.

namespace {

/**
 * Make a minimal EvalSettings with only the fields that computePolicyDigest
 * reads.  EvalSettings requires a `bool & readOnlyMode` reference; we use a
 * local bool that outlives the settings object (guaranteed by the lambda
 * capture pattern below or by caller lifetime).
 */
struct PolicyDigestFixture
{
    bool readOnlyMode = false;
    EvalSettings settings{readOnlyMode};

    PolicyDigestFixture()
    {
        // Start from a known baseline: impure, unrestricted, empty nix-path.
        settings.pureEval = false;
        settings.restrictEval = false;
        settings.nixPath = {};
    }
};

} // namespace

// ── Test 1: Different nixPath config values produce different digests ──

TEST(PolicyDigestTest, NixPathConfig_Changed_AffectsDigest)
{
    // BUG-11 regression: before the fix, both calls returned the same digest
    // because nixPath was never hashed.
    PolicyDigestFixture f1, f2;
    f1.settings.nixPath = Strings{"nixpkgs=/path/to/nixpkgs-v1"};
    f2.settings.nixPath = Strings{"nixpkgs=/path/to/nixpkgs-v2"};

    auto d1 = computePolicyDigest(f1.settings);
    auto d2 = computePolicyDigest(f2.settings);

    EXPECT_NE(d1, d2)
        << "Different nixPath values must produce different policy digests "
        << "(BUG-11: nixPath config was not included in the digest)";
}

// ── Test 2: NIX_PATH env AND nixPath config both affect the digest ────

TEST(PolicyDigestTest, NixPathEnvAndConfig_Both_AffectDigest)
{
    PolicyDigestFixture base, withEnv, withConfig, withBoth;

    // Baseline: no NIX_PATH env, empty nixPath config.
    // Ensure NIX_PATH is unset for the baseline measurement.
    ScopedEnvVar clearNixPath("NIX_PATH", "");
    auto dBase = computePolicyDigest(base.settings);

    // Changing NIX_PATH env must change the digest (existing behavior).
    {
        ScopedEnvVar env("NIX_PATH", "nixpkgs=/via-env");
        auto dEnv = computePolicyDigest(withEnv.settings);
        EXPECT_NE(dBase, dEnv)
            << "NIX_PATH env change must affect the policy digest";
    }

    // Changing nixPath config must change the digest (BUG-11 fix).
    {
        withConfig.settings.nixPath = Strings{"nixpkgs=/via-config"};
        auto dConfig = computePolicyDigest(withConfig.settings);
        EXPECT_NE(dBase, dConfig)
            << "nixPath config change must affect the policy digest (BUG-11)";
    }

    // Both changes together produce yet another distinct digest.
    {
        ScopedEnvVar env("NIX_PATH", "nixpkgs=/via-env");
        withBoth.settings.nixPath = Strings{"nixpkgs=/via-config"};
        auto dBoth = computePolicyDigest(withBoth.settings);
        // dBoth must differ from dBase, dEnv, and dConfig — three distinct inputs.
        EXPECT_NE(dBase, dBoth);
    }
}

// ── Test 3: In pure mode, nixPath does NOT affect the digest ──────────

TEST(PolicyDigestTest, PureMode_NixPath_Ignored)
{
    // In pure mode (pureEval = true), lookup paths are disabled — nixPath is
    // irrelevant to evaluation results, so it must NOT be hashed.  If it
    // were hashed, two pure-mode sessions with different --nix-path values
    // would get different session keys and fail to share the cache, even
    // though the evaluation is identical.
    PolicyDigestFixture fNoPath, fWithPath;
    fNoPath.settings.pureEval = true;
    fWithPath.settings.pureEval = true;
    fWithPath.settings.nixPath = Strings{"nixpkgs=/some/path"};

    auto dNoPath = computePolicyDigest(fNoPath.settings);
    auto dWithPath = computePolicyDigest(fWithPath.settings);

    EXPECT_EQ(dNoPath, dWithPath)
        << "nixPath must not affect the policy digest in pure mode "
        << "(lookup paths are disabled, so their value cannot influence results)";
}

// ── allowed-uris coverage ─────────────────────────────────────────────
//
// allowed-uris gates which URIs fetch* accepts under restrict-eval. Two
// sessions with the same purity/restriction/NIX_PATH but different
// allowed-uris were sharing a session key, so a cached result recorded
// under a permissive allowlist could be served after the allowlist was
// tightened (and vice versa). The fix added settings.allowedUris.get() to
// the digest unconditionally (regardless of pureEval/restrictEval).

// ── Test 4: Different allowedUris values produce different digests ────

TEST(PolicyDigestTest, AllowedUrisConfig_Changed_AffectsDigest)
{
    PolicyDigestFixture f1, f2;
    f1.settings.restrictEval = true;
    f2.settings.restrictEval = true;
    f1.settings.allowedUris = Strings{"https://example.com/a"};
    f2.settings.allowedUris = Strings{"https://example.com/b"};

    auto d1 = computePolicyDigest(f1.settings);
    auto d2 = computePolicyDigest(f2.settings);

    EXPECT_NE(d1, d2)
        << "Different allowedUris values must produce different policy digests";
}

// ── Test 5: Empty vs populated allowedUris produce different digests ──

TEST(PolicyDigestTest, AllowedUrisEmpty_vs_Populated_AffectsDigest)
{
    PolicyDigestFixture fEmpty, fPopulated;
    fEmpty.settings.restrictEval = true;
    fPopulated.settings.restrictEval = true;
    fPopulated.settings.allowedUris = Strings{"https://example.com"};

    auto dEmpty = computePolicyDigest(fEmpty.settings);
    auto dPopulated = computePolicyDigest(fPopulated.settings);

    EXPECT_NE(dEmpty, dPopulated)
        << "An empty allowedUris list must produce a different digest from "
        << "a populated one, so that restrict-eval sessions cannot collide";
}

// ── Test 6: allowedUris is hashed even in pure/unrestricted mode ──────
//
// Design choice: include allowedUris unconditionally (not gated by
// restrictEval or pureEval). This is conservative: it may cause spurious
// cache misses in modes that don't consult allowed-uris, but it
// future-proofs against adding code paths that read allowed-uris outside
// restrict-eval.

TEST(PolicyDigestTest, AllowedUris_Hashed_EvenInUnrestrictedMode)
{
    PolicyDigestFixture fNoUris, fWithUris;
    fNoUris.settings.restrictEval = false;
    fWithUris.settings.restrictEval = false;
    fWithUris.settings.allowedUris = Strings{"https://example.com"};

    auto dNoUris = computePolicyDigest(fNoUris.settings);
    auto dWithUris = computePolicyDigest(fWithUris.settings);

    EXPECT_NE(dNoUris, dWithUris)
        << "allowedUris must affect the digest even in unrestricted mode "
        << "(conservative inclusion — future-proofing against adding "
        << "allowed-uris consumers outside restrict-eval)";
}

} // namespace nix::eval_trace
