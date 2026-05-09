#include <gtest/gtest.h>
#include <type_traits>

#include "nix/util/session-types/dual.hh"

namespace nix::session {

TEST(SessionDual, Involution) {
    static_assert(std::is_same_v<Dual_t<Dual_t<Done>>, Done>);
    static_assert(std::is_same_v<Dual_t<Dual_t<Send<int, Done>>>, Send<int, Done>>);
    static_assert(std::is_same_v<Dual_t<Dual_t<Recv<int, Done>>>, Recv<int, Done>>);
    static_assert(std::is_same_v<Dual_t<Dual_t<Continue<3>>>, Continue<3>>);
    static_assert(std::is_same_v<
        Dual_t<Dual_t<Send<int, Recv<float, Done>>>>,
        Send<int, Recv<float, Done>>>);
}

TEST(SessionDual, EachConstructor) {
    static_assert(std::is_same_v<Dual_t<Send<int, Done>>, Recv<int, Done>>);
    static_assert(std::is_same_v<Dual_t<Recv<int, Done>>, Send<int, Done>>);
    static_assert(std::is_same_v<Dual_t<Done>, Done>);
    static_assert(std::is_same_v<Dual_t<Continue<0>>, Continue<0>>);
    static_assert(std::is_same_v<
        Dual_t<Loop<Send<int, Continue<0>>>>,
        Loop<Recv<int, Continue<0>>>>);
}

TEST(SessionDual, ChooseOffer) {
    static_assert(std::is_same_v<
        Dual_t<Choose<Send<int, Done>, Recv<float, Done>>>,
        Offer<Recv<int, Done>, Send<float, Done>>>);
    static_assert(std::is_same_v<
        Dual_t<Offer<Send<int, Done>, Done>>,
        Choose<Recv<int, Done>, Done>>);
    // Involution over Choose
    static_assert(std::is_same_v<
        Dual_t<Dual_t<Choose<Send<int, Done>, Recv<float, Done>>>>,
        Choose<Send<int, Done>, Recv<float, Done>>>);
}

TEST(SessionDual, Split) {
    // Split: P and Q flip
    static_assert(std::is_same_v<
        Dual_t<Split<Send<int, Done>, Recv<float, Done>, Done>>,
        Split<Send<float, Done>, Recv<int, Done>, Done>>);
    static_assert(std::is_same_v<
        Dual_t<Dual_t<Split<Send<int, Done>, Recv<float, Done>, Send<bool, Done>>>>,
        Split<Send<int, Done>, Recv<float, Done>, Send<bool, Done>>>);
}

TEST(SessionDual, Call) {
    static_assert(std::is_same_v<
        Dual_t<Call<Send<int, Done>, Recv<float, Done>>>,
        Call<Recv<int, Done>, Send<float, Done>>>);
}

TEST(SessionDual, ComplexNested) {
    static_assert(std::is_same_v<
        Dual_t<Dual_t<Loop<Choose<Send<int, Continue<0>>, Done>>>>,
        Loop<Choose<Send<int, Continue<0>>, Done>>>);
}

// -- Typed test suite: Dual involution across a bank of protocol types --------

template<typename P>
struct DualInvolutionTest : ::testing::Test {};

using InvolutionBank = ::testing::Types<
    Done,
    Send<int, Done>,
    Recv<int, Done>,
    Continue<0>,
    Continue<3>,
    Loop<Send<int, Continue<0>>>,
    Loop<Recv<float, Continue<0>>>,
    Choose<Send<int, Done>, Recv<float, Done>>,
    Offer<Done, Done, Done>,
    Choose<Send<int, Done>, Recv<float, Done>, Done>,
    Call<Send<int, Done>, Recv<float, Done>>,
    Split<Send<int, Done>, Recv<float, Done>, Done>,
    Send<int, Recv<float, Done>>,
    Loop<Choose<Send<int, Continue<0>>, Done>>,
    Split<Send<int, Done>, Recv<float, Done>, Send<bool, Done>>,
    Choose<Done, Choose<Done, Loop<Choose<
        Call<Loop<Choose<Send<int, Continue<0>>, Done>>, Choose<Done, Continue<0>>>,
        Done
    >>>>
>;

TYPED_TEST_SUITE(DualInvolutionTest, InvolutionBank);

TYPED_TEST(DualInvolutionTest, DualOfDualIsIdentity) {
    static_assert(std::is_same_v<Dual_t<Dual_t<TypeParam>>, TypeParam>);
}

} // namespace nix::session
