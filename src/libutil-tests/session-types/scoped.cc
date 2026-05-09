#include <gtest/gtest.h>

#include "nix/util/session-types/scoped.hh"

namespace nix::session {

TEST(SessionScoped, DoneAlwaysScoped) {
    static_assert(Scoped<Done>);
    static_assert(Scoped<Done, 0>);
    static_assert(Scoped<Done, 5>);
}

TEST(SessionScoped, LinearProtocols) {
    static_assert(Scoped<Send<int, Done>>);
    static_assert(Scoped<Recv<int, Send<float, Done>>>);
}

TEST(SessionScoped, ContinueValid) {
    static_assert(Scoped<Loop<Continue<0>>>);              // 0 < 1
    static_assert(Scoped<Loop<Send<int, Continue<0>>>>);   // 0 < 1
    static_assert(Scoped<Loop<Loop<Continue<0>>>>);        // 0 < 2 (inner)
    static_assert(Scoped<Loop<Loop<Continue<1>>>>);        // 1 < 2 (outer)
}

TEST(SessionScoped, ContinueInvalid) {
    static_assert(!Scoped<Continue<0>>);                    // 0 < 0 fails
    static_assert(!Scoped<Continue<1>>);                    // no loops
    static_assert(!Scoped<Loop<Continue<1>>>);              // 1 < 1 fails
    static_assert(!Scoped<Loop<Loop<Continue<2>>>>);        // 2 < 2 fails
    static_assert(!Scoped<Loop<Continue<5>>>);              // 5 < 1 fails
}

TEST(SessionScoped, ChooseOffer) {
    static_assert(Scoped<Choose<Send<int, Done>, Recv<float, Done>>>);
    static_assert(Scoped<Offer<Done, Done, Done>>);
    static_assert(!Scoped<Choose<Send<int, Done>, Continue<0>>>);
    static_assert(!Scoped<Offer<Continue<0>, Done>>);
    static_assert(Scoped<Loop<Choose<Continue<0>, Done>>>);
}

TEST(SessionScoped, CallSplit) {
    static_assert(Scoped<Call<Send<int, Done>, Recv<float, Done>>>);
    static_assert(!Scoped<Call<Continue<0>, Done>>);
    static_assert(!Scoped<Call<Done, Continue<0>>>);
    static_assert(Scoped<Split<Send<int, Done>, Recv<float, Done>, Done>>);
    static_assert(!Scoped<Split<Continue<0>, Done, Done>>);
}

TEST(SessionScoped, DeepNesting) {
    static_assert(Scoped<Loop<Loop<Loop<Continue<2>>>>>);   // 2 < 3
    static_assert(!Scoped<Loop<Loop<Loop<Continue<3>>>>>);  // 3 < 3 fails
}

TEST(SessionScoped, MixedValidity) {
    static_assert(Scoped<Loop<Choose<
        Send<int, Continue<0>>,
        Loop<Continue<1>>                   // 1 < 2 (refers to outer)
    >>>);
    static_assert(!Scoped<Loop<Choose<
        Send<int, Continue<0>>,
        Loop<Continue<2>>                   // 2 < 2 fails
    >>>);
}

} // namespace nix::session
