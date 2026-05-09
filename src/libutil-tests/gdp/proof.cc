#include <gtest/gtest.h>
#include <type_traits>

#include "nix/util/gdp/proof.hh"

namespace nix::gdp {

struct TagA {};
struct TagB {};

/// Test helper: publicly exposes Certifier<Tag>::withProof / withProofIf.
template<typename Tag>
class TestCertifier : private Certifier<Tag> {
public:
    using Certifier<Tag>::withProof;
    using Certifier<Tag>::withProofIf;
};

// --- static_assert: Proof is not constructible/copyable/movable ---

static_assert(!std::is_default_constructible_v<Proof<TagA>>);
static_assert(!std::is_copy_constructible_v<Proof<TagA>>);
static_assert(!std::is_move_constructible_v<Proof<TagA>>);
static_assert(!std::is_copy_assignable_v<Proof<TagA>>);
static_assert(!std::is_move_assignable_v<Proof<TagA>>);

// --- withProof ---

TEST(GDPProof, WithProofCallsContinuation) {
    bool called = false;
    TestCertifier<TagA>::withProof([&](const Proof<TagA> &) {
        called = true;
    });
    EXPECT_TRUE(called);
}

TEST(GDPProof, WithProofForwardsReturnValue) {
    auto result = TestCertifier<TagA>::withProof([](const Proof<TagA> &) {
        return 42;
    });
    EXPECT_EQ(result, 42);
}

// --- withProofIf (non-void) ---

TEST(GDPProof, WithProofIfTrueReturnsOptional) {
    auto result = TestCertifier<TagA>::withProofIf(true, [](const Proof<TagA> &) {
        return 7;
    });
    static_assert(std::is_same_v<decltype(result), std::optional<int>>);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 7);
}

TEST(GDPProof, WithProofIfFalseReturnsNullopt) {
    bool called = false;
    auto result = TestCertifier<TagA>::withProofIf(false, [&](const Proof<TagA> &) {
        called = true;
        return 7;
    });
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(called);
}

// --- withProofIf (void) ---

TEST(GDPProof, WithProofIfVoidTrueReturnsTrue) {
    bool called = false;
    bool result = TestCertifier<TagA>::withProofIf(true, [&](const Proof<TagA> &) {
        called = true;
    });
    EXPECT_TRUE(result);
    EXPECT_TRUE(called);
}

TEST(GDPProof, WithProofIfVoidFalseReturnsFalse) {
    bool called = false;
    bool result = TestCertifier<TagA>::withProofIf(false, [&](const Proof<TagA> &) {
        called = true;
    });
    EXPECT_FALSE(result);
    EXPECT_FALSE(called);
}

// --- Certifier CRTP: domain-specific wrapper ---

struct CertTag {};

class DomainCertifier : private Certifier<CertTag> {
public:
    template<typename F>
    bool certifyIf(bool ok, F && f) {
        return Certifier::withProofIf(ok, std::forward<F>(f));
    }
};

TEST(GDPProof, CertifierCreatesProof) {
    DomainCertifier cert;
    bool called = false;
    bool result = cert.certifyIf(true, [&](const Proof<CertTag> &) {
        called = true;
    });
    EXPECT_TRUE(result);
    EXPECT_TRUE(called);
}

TEST(GDPProof, CertifierSkipsOnFalse) {
    DomainCertifier cert;
    bool called = false;
    bool result = cert.certifyIf(false, [&](const Proof<CertTag> &) {
        called = true;
    });
    EXPECT_FALSE(result);
    EXPECT_FALSE(called);
}

// --- Tag confusion: Proof<A> and Proof<B> are distinct ---

TEST(GDPProof, DifferentTagsAreDistinctTypes) {
    static_assert(!std::is_same_v<Proof<TagA>, Proof<TagB>>);
}

} // namespace nix::gdp
