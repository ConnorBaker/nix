/**
 * Tests for CanonicalQueryKind (CQK) lattice predicates — verifies that
 * queryBehavior, isVolatile, isCoveredBySessionFingerprint,
 * repoRootAddressingKind, isFileContentDep, and describe() are mutually
 * consistent across all known query kinds.
 */
#include "nix/expr/eval-trace/store/session-policy.hh"

#include <gtest/gtest.h>

namespace nix::eval_trace {

// ── CQK lattice consistency ─────────────────────────────────────────

static constexpr CanonicalQueryKind allCQKs[] = {
    CanonicalQueryKind::FileBytes,
    CanonicalQueryKind::DirectoryEntries,
    CanonicalQueryKind::ExistenceCheck,
    CanonicalQueryKind::EnvironmentLookup,
    CanonicalQueryKind::SessionSystemValue,
    CanonicalQueryKind::RuntimeFetchIdentity,
    CanonicalQueryKind::DerivedStorePath,
    CanonicalQueryKind::VolatileExec,
    CanonicalQueryKind::NarIdentity,
    CanonicalQueryKind::StructuredProjection,
    CanonicalQueryKind::ImplicitStructure,
    CanonicalQueryKind::RawBytes,
    CanonicalQueryKind::StorePathAvailability,
    CanonicalQueryKind::GitRevisionIdentity,
    CanonicalQueryKind::TraceValueContext,
    CanonicalQueryKind::TraceParentSlot,
    CanonicalQueryKind::VolatileTime,
};

TEST(CQKLatticeTest, Volatile_Behavior_ImpliesIsVolatile)
{
    for (auto kind : allCQKs) {
        if (queryBehavior(kind) == QueryBehavior::Volatile) {
            EXPECT_TRUE(isVolatile(kind))
                << "CQK " << queryKindName(kind)
                << " has Volatile behavior but isVolatile returns false";
        }
    }
}

TEST(CQKLatticeTest, IsVolatile_Predicate_ImpliesBehavior)
{
    for (auto kind : allCQKs) {
        if (isVolatile(kind)) {
            EXPECT_EQ(queryBehavior(kind), QueryBehavior::Volatile)
                << "CQK " << queryKindName(kind)
                << " isVolatile but behavior is not Volatile";
        }
    }
}

TEST(CQKLatticeTest, TraceContext_Deps_NotCoveredByFingerprint)
{
    // Trace-context deps depend on upstream traces which may have
    // non-file deps — they must not be covered by session fingerprint.
    EXPECT_FALSE(isCoveredBySessionFingerprint(CanonicalQueryKind::TraceValueContext));
    EXPECT_FALSE(isCoveredBySessionFingerprint(CanonicalQueryKind::TraceParentSlot));
}

TEST(CQKLatticeTest, EveryKind_Domains_HasObservedDomains)
{
    for (auto kind : allCQKs) {
        EXPECT_NE(describe(kind).observedDomains, 0u)
            << "CQK " << queryKindName(kind)
            << " must observe at least one semantic domain";
    }
}

TEST(CQKLatticeTest, RuntimeFetch_Domains_ObservesAvailabilityAndResolution)
{
    auto kind = CanonicalQueryKind::RuntimeFetchIdentity;
    EXPECT_TRUE(queryDomainContains(describe(kind).observedDomains, QueryDomain::Availability));
    EXPECT_TRUE(queryDomainContains(describe(kind).observedDomains, QueryDomain::Resolution));
}

TEST(CQKLatticeTest, IdentityDomain_Families_MatchesExpected)
{
    for (auto kind : {CanonicalQueryKind::GitRevisionIdentity, CanonicalQueryKind::TraceValueContext, CanonicalQueryKind::TraceParentSlot}) {
        EXPECT_TRUE(queryDomainContains(describe(kind).observedDomains, QueryDomain::Identity))
            << "CQK " << queryKindName(kind)
            << " should observe the identity domain";
    }
}

TEST(CQKLatticeTest, RepoRoot_Addressing_MatchesFileContentDeps)
{
    for (auto kind : allCQKs) {
        auto addressing = repoRootAddressingKind(kind);
        if (addressing == RepoRootAddressingKind::None)
            continue;

        EXPECT_TRUE(isFileContentDep(kind))
            << "CQK " << queryKindName(kind)
            << " is repo-root addressable but not a file content dep";
    }

    EXPECT_EQ(repoRootAddressingKind(CanonicalQueryKind::FileBytes), RepoRootAddressingKind::DirectPath);
    EXPECT_EQ(repoRootAddressingKind(CanonicalQueryKind::DirectoryEntries), RepoRootAddressingKind::DirectPath);
    EXPECT_EQ(repoRootAddressingKind(CanonicalQueryKind::ExistenceCheck), RepoRootAddressingKind::DirectPath);
    EXPECT_EQ(repoRootAddressingKind(CanonicalQueryKind::RawBytes), RepoRootAddressingKind::DirectPath);
    EXPECT_EQ(repoRootAddressingKind(CanonicalQueryKind::NarIdentity), RepoRootAddressingKind::DirectPath);
    EXPECT_EQ(repoRootAddressingKind(CanonicalQueryKind::StructuredProjection), RepoRootAddressingKind::StructuredPath);
    EXPECT_EQ(repoRootAddressingKind(CanonicalQueryKind::ImplicitStructure), RepoRootAddressingKind::StructuredPath);
}

} // namespace nix::eval_trace
