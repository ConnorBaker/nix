#include <gtest/gtest.h>
#include <string>

#include "nix/util/gdp/proof-guarded.hh"

namespace nix::gdp {

struct GuardTag {};

/// Test helper: publicly exposes Certifier<GuardTag>::withProof.
class GuardCertifier : private Certifier<GuardTag> {
public:
    using Certifier<GuardTag>::withProof;
};

TEST(GDPProofGuarded, AccessWithMatchingProof) {
    ProofGuarded<int, GuardTag> g(42);
    GuardCertifier::withProof([&](const Proof<GuardTag> & proof) {
        EXPECT_EQ(g.access(proof), 42);
        g.access(proof) = 100;
        EXPECT_EQ(g.access(proof), 100);
    });
}

TEST(GDPProofGuarded, ConstAccess) {
    const ProofGuarded<int, GuardTag> g(42);
    GuardCertifier::withProof([&](const Proof<GuardTag> & proof) {
        EXPECT_EQ(g.access(proof), 42);
        static_assert(std::is_same_v<decltype(g.access(proof)), const int &>);
    });
}

TEST(GDPProofGuarded, VariadicConstruction) {
    ProofGuarded<std::string, GuardTag> g("hello", 3);
    GuardCertifier::withProof([&](const Proof<GuardTag> & proof) {
        EXPECT_EQ(g.access(proof), "hel");
    });
}

TEST(GDPProofGuarded, DefaultConstruction) {
    ProofGuarded<int, GuardTag> g;
    GuardCertifier::withProof([&](const Proof<GuardTag> & proof) {
        g.access(proof) = 5;
        EXPECT_EQ(g.access(proof), 5);
    });
}

} // namespace nix::gdp
