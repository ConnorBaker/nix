#include <gtest/gtest.h>
#include <string>
#include <type_traits>

#include "nix/util/session-types/session.hh"

namespace nix::session {

TEST(SessionConcept, ValidSessions) {
    static_assert(Session<Done>);
    static_assert(Session<Send<int, Done>>);
    static_assert(Session<Recv<int, Done>>);
    static_assert(Session<Send<int, Recv<float, Done>>>);
    static_assert(Session<Choose<Send<int, Done>, Recv<float, Done>>>);
    static_assert(Session<Offer<Done, Done, Done>>);
    static_assert(Session<Loop<Send<int, Continue<0>>>>);
    static_assert(Session<Loop<Choose<Send<int, Continue<0>>, Done>>>);
    static_assert(Session<Call<Send<int, Done>, Recv<float, Done>>>);
    static_assert(Session<Split<Send<int, Done>, Recv<float, Done>, Done>>);
}

TEST(SessionConcept, InvalidSessions) {
    static_assert(!Session<Continue<0>>);                        // unscoped
    static_assert(!Session<Loop<Continue<1>>>);                  // index too high
    static_assert(!Session<Choose<Done, Continue<0>>>);          // unscoped branch
}

TEST(SessionConcept, DualOfSessionIsSession) {
    static_assert(Session<Dual_t<Send<int, Done>>>);
    static_assert(Session<Dual_t<Loop<Send<int, Continue<0>>>>>);
    static_assert(Session<Dual_t<Choose<Send<int, Done>, Done>>>);
    static_assert(Session<Dual_t<Split<Send<int, Done>, Recv<float, Done>, Done>>>);
}

TEST(SessionConcept, RealisticProtocol) {
    // Simplified verification chain
    using DepLoop = Loop<Choose<
        Send<int, Continue<0>>,     // process next dep
        Done                        // all deps processed
    >>;
    static_assert(Session<DepLoop>);

    using RecoveryChain = Choose<
        Done,                                   // GitIdentity hit
        Choose<
            Done,                               // DirectHash hit
            Loop<Choose<                        // StructVariant scan
                Call<DepLoop, Choose<Done, Continue<0>>>,  // try variant, maybe loop
                Done                            // no more variants
            >>
        >
    >;
    static_assert(Session<RecoveryChain>);

    using VerificationChain = Choose<
        Done,                                   // fingerprint hit
        Call<DepLoop, Choose<
            Done,                               // verified
            Call<RecoveryChain, Done>            // recovery needed
        >>
    >;
    static_assert(Session<VerificationChain>);

    // Dual involution for realistic protocols
    static_assert(std::is_same_v<Dual_t<Dual_t<VerificationChain>>, VerificationChain>);
    static_assert(Session<Dual_t<VerificationChain>>);
}

TEST(SessionConcept, ThenComposesSessionsIntoSessions) {
    using Handshake = Send<int, Recv<int, Done>>;
    using DataTransfer = Loop<Choose<Send<std::string, Continue<0>>, Done>>;
    using FullProtocol = Then_t<Handshake, DataTransfer>;
    static_assert(Session<FullProtocol>);
    static_assert(Session<Dual_t<FullProtocol>>);
}

} // namespace nix::session
