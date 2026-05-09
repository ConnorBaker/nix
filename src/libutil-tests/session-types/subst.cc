#include <gtest/gtest.h>
#include <type_traits>

#include "nix/util/session-types/subst.hh"

namespace nix::session {

TEST(SessionSubst, DirectSubstitution) {
    static_assert(std::is_same_v<Subst_t<Continue<0>, Done>, Done>);
    static_assert(std::is_same_v<Subst_t<Continue<0>, Recv<int, Done>>, Recv<int, Done>>);
}

TEST(SessionSubst, NoOpWrongDepth) {
    static_assert(std::is_same_v<Subst_t<Continue<1>, Done>, Continue<1>>);
    static_assert(std::is_same_v<Subst_t<Continue<0>, Done, 1>, Continue<0>>);
}

TEST(SessionSubst, DonePassthrough) {
    static_assert(std::is_same_v<Subst_t<Done, Recv<int, Done>>, Done>);
}

TEST(SessionSubst, SendRecvTail) {
    static_assert(std::is_same_v<
        Subst_t<Send<int, Continue<0>>, Done>,
        Send<int, Done>>);
    static_assert(std::is_same_v<
        Subst_t<Recv<float, Continue<0>>, Send<int, Done>>,
        Recv<float, Send<int, Done>>>);
}

TEST(SessionSubst, DeBruijnLoopDepth) {
    // Continue<0> inside Loop refers to the Loop itself: N=0 -> enters Loop -> N=1, 0 != 1
    static_assert(std::is_same_v<
        Subst_t<Loop<Continue<0>>, Done>,
        Loop<Continue<0>>>);

    // Continue<1> inside Loop: N=0 -> enters Loop -> N=1, 1 == 1 -> substituted
    static_assert(std::is_same_v<
        Subst_t<Loop<Continue<1>>, Done>,
        Loop<Done>>);

    // Continue<0> inside Loop<Loop<...>>: N=0 -> N=1 -> N=2, 0 != 2
    static_assert(std::is_same_v<
        Subst_t<Loop<Loop<Continue<0>>>, Done>,
        Loop<Loop<Continue<0>>>>);
}

TEST(SessionSubst, ChooseOffer) {
    static_assert(std::is_same_v<
        Subst_t<Choose<Continue<0>, Send<int, Continue<0>>>, Done>,
        Choose<Done, Send<int, Done>>>);

    static_assert(std::is_same_v<
        Subst_t<Offer<Continue<0>, Done>, Send<int, Done>>,
        Offer<Send<int, Done>, Done>>);
}

TEST(SessionSubst, CallSplit) {
    static_assert(std::is_same_v<
        Subst_t<Call<Continue<0>, Continue<0>>, Done>,
        Call<Done, Done>>);
    static_assert(std::is_same_v<
        Subst_t<Split<Continue<0>, Done, Continue<0>>, Send<int, Done>>,
        Split<Send<int, Done>, Done, Send<int, Done>>>);
}

} // namespace nix::session
