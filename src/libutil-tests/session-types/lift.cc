#include <gtest/gtest.h>
#include <type_traits>

#include "nix/util/session-types/lift.hh"

namespace nix::session {

TEST(SessionLift, FreeVariableShifted) {
    static_assert(std::is_same_v<Lift_t<Continue<0>, 1>, Continue<1>>);
    static_assert(std::is_same_v<Lift_t<Continue<0>, 3>, Continue<3>>);
    static_assert(std::is_same_v<Lift_t<Continue<2>, 5>, Continue<7>>);
}

TEST(SessionLift, BoundVariableNotShifted) {
    // Inside Loop, Continue<0> refers to the Loop -> bound, no shift
    static_assert(std::is_same_v<Lift_t<Loop<Continue<0>>, 1>, Loop<Continue<0>>>);
}

TEST(SessionLift, FreeVariableInsideLoop) {
    // Continue<1> refers outside the Loop -> free, shifted
    static_assert(std::is_same_v<Lift_t<Loop<Continue<1>>, 1>, Loop<Continue<2>>>);
    static_assert(std::is_same_v<Lift_t<Loop<Continue<1>>, 3>, Loop<Continue<4>>>);
}

TEST(SessionLift, DeeplyNested) {
    // Continue<0> inside Loop<Loop<...>> at Level=2: 0 < 2 -> bound
    static_assert(std::is_same_v<Lift_t<Loop<Loop<Continue<0>>>, 1>, Loop<Loop<Continue<0>>>>);
    // Continue<2> inside Loop<Loop<...>> at Level=2: 2 >= 2 -> free, shifted
    static_assert(std::is_same_v<Lift_t<Loop<Loop<Continue<2>>>, 1>, Loop<Loop<Continue<3>>>>);
}

TEST(SessionLift, DonePassthrough) {
    static_assert(std::is_same_v<Lift_t<Done, 5>, Done>);
}

TEST(SessionLift, SendRecvTailShifted) {
    static_assert(std::is_same_v<
        Lift_t<Send<int, Continue<0>>, 2>,
        Send<int, Continue<2>>>);
    static_assert(std::is_same_v<
        Lift_t<Recv<float, Continue<0>>, 1>,
        Recv<float, Continue<1>>>);
}

TEST(SessionLift, ChooseOfferBranchesShifted) {
    static_assert(std::is_same_v<
        Lift_t<Choose<Continue<0>, Continue<1>>, 1>,
        Choose<Continue<1>, Continue<2>>>);
    static_assert(std::is_same_v<
        Lift_t<Offer<Continue<0>, Done>, 2>,
        Offer<Continue<2>, Done>>);
}

TEST(SessionLift, ZeroShiftIsIdentity) {
    static_assert(std::is_same_v<Lift_t<Continue<0>, 0>, Continue<0>>);
    static_assert(std::is_same_v<Lift_t<Send<int, Done>, 0>, Send<int, Done>>);
    static_assert(std::is_same_v<Lift_t<Loop<Continue<0>>, 0>, Loop<Continue<0>>>);
}

} // namespace nix::session
