/**
 * Tests for SessionConfig construction and session key determinism:
 *   - SessionConfigTest  — SqliteTraceStorage::setSessionConfig (SetOnce enforcement)
 *                          and path-value coercion provenance
 *   - SessionKeyDeterminismTest — buildSemanticSessionKey stability
 *   - InputCopyIdentityTest     — fetchers::Input copy preserves all attrs
 */
#include "eval-trace/helpers.hh"
#include "nix/expr/eval-trace/hash-spec.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/session-policy.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/fetchers/fetchers.hh"

#include <gtest/gtest.h>
#include <type_traits>

namespace nix::eval_trace {

using namespace nix::eval_trace::test;

namespace {

std::vector<SessionExternalRoot> sessionRoots(std::initializer_list<std::string_view> roots)
{
    std::vector<SessionExternalRoot> typedRoots;
    typedRoots.reserve(roots.size());
    for (auto root : roots)
        typedRoots.emplace_back(CanonPath(std::string(root)));
    return typedRoots;
}

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

class SessionConfigTest : public TraceStoreFixture {};

template<typename T>
concept HasTypedResolve = requires(const T & t, DepSource source, std::string key) {
    t.resolve(source, key);
};

static_assert(HasTypedResolve<SemanticRegistry>);

TEST_F(SessionConfigTest, CoerceToString_CopyToStore_ReportsPathObjectForPathValues)
{
    TempDir runtimeDir;
    runtimeDir.addFile("value.txt", "shared");
    ScopedExtraExperimentalFeatures flakes("flakes");

    auto input = fetchers::Input::fromURL(
        state.fetchSettings,
        "path:" + runtimeDir.path().string());
    auto [storePath, lockedInput] = input.fetchToStore(state.fetchSettings, *state.store);

    eval_trace::SemanticRegistry testRegistry;
    DepCaptureScope depCapture(pools(), testRegistry);
    TraceActivationScope traceActivation(state);

    Value value;
    value.mkPath(state.storePath(storePath), state.mem);

    auto origin = PathObject{
        .source = DepSource::fromRuntimeRoot(
            makeRuntimeRootSourceKey(RuntimeFetchIdentityDepKey{.inputAttrs = lockedInput.toAttrs()})),
        .rootPath = CanonPath(state.store->printStorePath(storePath)),
    };

    state.mergeSemanticHandle(value,
        std::optional<SemanticHandle>(SemanticHandle::forPath(origin)));

    NixStringContext context;
    auto coerced = state.coerceToContextObject(
        noPos,
        value,
        context,
        "while testing path coercion provenance",
        false,
        true,
        true);

    EXPECT_FALSE(coerced.view().empty());
    auto handle = state.lookupSemanticHandle(value);
    ASSERT_TRUE(handle);
    ASSERT_TRUE(handle->hasPath());
    EXPECT_EQ(*handle->path, origin);
}

// ── SetOnce<SessionConfig> on SqliteTraceStorage ────────────────────────────

TEST_F(SessionConfigTest, TraceStore_SetSessionConfig_Once_Succeeds)
{
    auto db = makeDb();
    db->setSessionConfig(SessionConfig::forTest(depHash("fp").value));
    EXPECT_TRUE(db->hasSessionConfig());
}

TEST_F(SessionConfigTest, TraceStore_SetSessionConfig_Twice_Throws)
{
    auto db = makeDb();
    db->setSessionConfig(SessionConfig::forTest(depHash("fp").value));
    EXPECT_THROW(
        db->setSessionConfig(SessionConfig::forTest(depHash("fp2").value)),
        Error);
}

TEST_F(SessionConfigTest, TraceStore_RuntimeRootPersistence_RoundTripsTypedRows)
{
    TempDir runtimeDir;
    runtimeDir.addFile("value.txt", "shared");
    ScopedExtraExperimentalFeatures flakes("flakes");

    auto input = fetchers::Input::fromURL(
        state.fetchSettings,
        "path:" + runtimeDir.path().string());
    auto [storePath, lockedInput] = input.fetchToStore(state.fetchSettings, *state.store);

    auto db = makeDb();
    auto fetchIdentity = RuntimeFetchIdentityDepKey{.inputAttrs = lockedInput.toAttrs()};
    auto narHash = RuntimeRootNarHash{state.store->queryPathInfo(storePath)->narHash};
    auto source = DepSource::fromRuntimeRoot(makeRuntimeRootSourceKey(fetchIdentity));

    withExclusiveStore(*db, [&](const auto & ea) {
        db->recordRuntimeRoot(ea,
            SqliteTraceStorage::RuntimeRootRecord{
                .source = source,
                .fetchIdentity = fetchIdentity,
                .narHash = narHash,
                .storePath = RuntimeRootStorePath{storePath},
            },
            *state.store);
    });

    auto loaded = withExclusiveStore(*db, [&](const auto & ea) {
        return db->loadRuntimeRoots(ea, *state.store);
    });
    EXPECT_EQ(loaded.storedCount, 1u);
    EXPECT_EQ(loaded.rejectedCount, 0u);
    ASSERT_EQ(loaded.entries.size(), 1u);

    auto & entry = loaded.entries.front();
    EXPECT_EQ(entry.source, source);
    EXPECT_EQ(entry.fetchIdentity.inputAttrs, fetchIdentity.inputAttrs);
    EXPECT_EQ(entry.narHash, narHash);
    EXPECT_EQ(entry.storePath.value, storePath);
}

// ── Invariant 1: lockedInput copy preserves Input identity ──────────

TEST(InputCopyIdentityTest, InputCopy_PreservesAttrsAndURL_Identical)
{
    ScopedExtraExperimentalFeatures flakes("flakes");
    fetchers::Settings fetchSettings;

    // Construct a locked git Input with typical attributes.
    auto original = fetchers::Input::fromURL(fetchSettings,
        "git+file:///tmp/test-repo?ref=refs/heads/main&rev=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&narHash=sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA%3D");

    // Copy it (same operation as storing on ResolvedFlakeNode).
    auto copied = original;

    // The copy must be identical: same attrs, same URL serialization,
    // same rev/narHash/type accessors.
    EXPECT_EQ(original, copied);
    EXPECT_EQ(original.to_string(), copied.to_string());
    EXPECT_EQ(original.getType(), copied.getType());
    EXPECT_EQ(original.getRev(), copied.getRev());
    EXPECT_EQ(original.getNarHash(), copied.getNarHash());
    EXPECT_EQ(original.getLastModified(), copied.getLastModified());
    EXPECT_EQ(original.getRevCount(), copied.getRevCount());
}

// ── Invariant 2: Session key determinism ────────────────────────────

TEST(SessionKeyDeterminismTest, Session_SameComponents_ProduceSameKey)
{
    auto policyDigest = depHash("test-policy").value;
    auto graphDigest = depHash("test-graph").value;

    SessionConfig cfg1{
        .policyDigest = policyDigest,
        .graphDigest = graphDigest,
        .sourceIdentity = SessionSourceDigest{depHash("git+file:///tmp/test-flake").value},
        .externalRoots = sessionRoots({"/tmp/root1", "/tmp/root2"}),
        .stableRecoveryKey = SessionRecoveryKey{depHash("recovery-key").value},
    };

    SessionConfig cfg2{
        .policyDigest = policyDigest,
        .graphDigest = graphDigest,
        .sourceIdentity = SessionSourceDigest{depHash("git+file:///tmp/test-flake").value},
        .externalRoots = sessionRoots({"/tmp/root1", "/tmp/root2"}),
        .stableRecoveryKey = SessionRecoveryKey{depHash("recovery-key").value},
    };

    auto key1 = cfg1.buildSemanticSessionKey();
    auto key2 = cfg2.buildSemanticSessionKey();

    EXPECT_EQ(key1.digest, key2.digest);
}

TEST(SessionKeyDeterminismTest, Session_DifferentComponents_ProduceDifferentKeys)
{
    auto policyDigest = depHash("test-policy").value;

    SessionConfig withGraph{
        .policyDigest = policyDigest,
        .graphDigest = depHash("graph-v1").value,
        .sourceIdentity = SessionSourceDigest{depHash("test-flake").value},
    };

    SessionConfig withoutGraph{
        .policyDigest = policyDigest,
        .sourceIdentity = SessionSourceDigest{depHash("test-flake").value},
    };

    SessionConfig differentSource{
        .policyDigest = policyDigest,
        .graphDigest = depHash("graph-v1").value,
        .sourceIdentity = SessionSourceDigest{depHash("other-flake").value},
    };

    EXPECT_NE(withGraph.buildSemanticSessionKey().digest,
              withoutGraph.buildSemanticSessionKey().digest);
    EXPECT_NE(withGraph.buildSemanticSessionKey().digest,
              differentSource.buildSemanticSessionKey().digest);
}

// ── D-2: Session key 5-component isolation ───────────────────────────
//
// Each of the three previously-untested SessionConfig components must
// produce a different session key when changed.
//
// Note: kProviderEpoch is a global compile-time constant, not a SessionConfig
// field. It is encoded unconditionally into every session key via
// buildSemanticSessionKey(). There is no per-config providerEpoch parameter
// to vary in tests; its contribution is tested implicitly by
// Session_SameComponents_ProduceSameKey.

// externalRoots change must produce a different session key.
TEST(SessionKeyDeterminismTest, SessionKey_ExternalRoots_Isolates)
{
    SessionConfig base{
        .policyDigest = depHash("policy").value,
        .graphDigest = depHash("graph").value,
        .sourceIdentity = SessionSourceDigest{depHash("git+file:///repo").value},
    };
    auto withRoots = base;
    withRoots.externalRoots = sessionRoots({"/nix/store/abc"});
    EXPECT_NE(base.buildSemanticSessionKey().digest,
              withRoots.buildSemanticSessionKey().digest);
}

// sourceIdentity change must produce a different session key.
TEST(SessionKeyDeterminismTest, SessionKey_SourceIdentity_Isolates)
{
    SessionConfig cfg1{
        .policyDigest = depHash("policy").value,
        .sourceIdentity = SessionSourceDigest{depHash("git+file:///repo-A").value},
    };
    auto cfg2 = cfg1;
    cfg2.sourceIdentity = SessionSourceDigest{depHash("git+file:///repo-B").value};
    EXPECT_NE(cfg1.buildSemanticSessionKey().digest,
              cfg2.buildSemanticSessionKey().digest);
}

// stableRecoveryKey change must produce a different session key.
// stableRecoveryKey is NOT included in buildSemanticSessionKey() — it is a
// cross-session identity used for history bootstrap, not a session namespace
// component. This test documents that the field does NOT affect the semantic
// digest, which is the correct behavior (changing the recovery key must not
// invalidate existing session caches).
//
// If the design changes so that stableRecoveryKey IS included in the key,
// this test should be updated to EXPECT_NE.
TEST(SessionKeyDeterminismTest, SessionKey_StableRecoveryKey_DoesNotAffectDigest)
{
    SessionConfig cfg1{
        .policyDigest = depHash("policy").value,
        .stableRecoveryKey = SessionRecoveryKey{depHash("key-A").value},
    };
    auto cfg2 = cfg1;
    cfg2.stableRecoveryKey = SessionRecoveryKey{depHash("key-B").value};
    // stableRecoveryKey is not part of the semantic session key serialization.
    EXPECT_EQ(cfg1.buildSemanticSessionKey().digest,
              cfg2.buildSemanticSessionKey().digest);
}

// Hash-domain baseline regression guard (ref adversarial review #13).
//
// Any refactor that renames a `hash_domain::*` tag used by
// `SessionConfig::buildSemanticSessionKey` (or reorders the fields fed
// into the canonical-hash builder) silently changes every user's
// session digest. There is no compile-time guard against this —
// `makeDomainBuilder<hash_domain::Foo>` takes the tag as a template
// parameter, so a rename compiles cleanly. This test fails loudly
// instead.
//
// If this test fails after a deliberate hash-domain change, you MUST
// bump `kSchemaEpoch` in `session-identity.hh` (otherwise existing
// cache files under `eval-trace-v<epoch>-<algo>.sqlite` will be read
// as valid but produce mismatched session keys) AND update the
// expected digest below.
//
// Uses the explicit-algorithm `buildSemanticSessionKey(algorithm)`
// overload so the test is insensitive to process-global algorithm
// state (ref #8).
TEST(SessionKeyDeterminismTest, SessionKey_PinnedDigest_RegressionGuard)
{
    SessionConfig cfg{
        .policyDigest = depHash("hash-domain-baseline-policy").value,
        .graphDigest = std::nullopt,
        .sourceIdentity = SessionSourceDigest{depHash("hash-domain-baseline-source").value},
        .externalRoots = {},
        .stableRecoveryKey = SessionRecoveryKey{depHash("hash-domain-baseline-recovery").value},
    };

    // Expected digest pinned at branch-creation time against:
    //   hash-algorithm = blake3
    //   kSchemaEpoch = 25
    //   kProviderEpoch = 1
    //
    // Rebuild instructions if this assertion fires intentionally:
    //   1. Bump kSchemaEpoch in session-identity.hh.
    //   2. Run this test; use the "actual" hex in the gtest output
    //      as the new expected value.
    //   3. Commit both changes together.
    const std::string_view expectedHex =
        "a4084e6e4b290491f4c6169fdbabe4a2df603ee7d6446db960a01150c670141a";
    auto key = cfg.buildSemanticSessionKey(EvalTraceHashAlgorithm::Blake3);
    EXPECT_EQ(key.digest.toHex(), expectedHex)
        << "SessionConfig::buildSemanticSessionKey digest changed. "
           "If intentional, bump kSchemaEpoch and update expectedHex.";
}

} // namespace nix::eval_trace
