#include <gtest/gtest.h>
#include <string>
#include <type_traits>

#include "nix/util/session-types/then.hh"

namespace nix::session {

TEST(SessionThen, BasicSequencing) {
    static_assert(std::is_same_v<Then_t<Done, Recv<int, Done>>, Recv<int, Done>>);
    static_assert(std::is_same_v<
        Then_t<Send<int, Done>, Recv<float, Done>>,
        Send<int, Recv<float, Done>>>);
}

TEST(SessionThen, Chaining) {
    static_assert(std::is_same_v<
        Then_t<Send<int, Done>, Then_t<Recv<float, Done>, Send<bool, Done>>>,
        Send<int, Recv<float, Send<bool, Done>>>>);
}

TEST(SessionThen, ContinueIsInert) {
    static_assert(std::is_same_v<Then_t<Continue<0>, Recv<int, Done>>, Continue<0>>);
}

TEST(SessionThen, CallOnlyQGetsContinuation) {
    static_assert(std::is_same_v<
        Then_t<Call<Send<int, Done>, Done>, Recv<float, Done>>,
        Call<Send<int, Done>, Recv<float, Done>>>);
}

TEST(SessionThen, SplitOnlyRGetsContinuation) {
    static_assert(std::is_same_v<
        Then_t<Split<Send<int, Done>, Recv<float, Done>, Done>, Send<bool, Done>>,
        Split<Send<int, Done>, Recv<float, Done>, Send<bool, Done>>>);
}

TEST(SessionThen, ChooseOfferBranches) {
    static_assert(std::is_same_v<
        Then_t<Choose<Send<int, Done>, Done>, Recv<float, Done>>,
        Choose<Send<int, Recv<float, Done>>, Recv<float, Done>>>);
}

TEST(SessionThen, LoopDepthAdjustment) {
    static_assert(std::is_same_v<
        Then_t<Loop<Done>, Recv<int, Done>>,
        Loop<Recv<int, Done>>>);
    // When continuation P has a Continue<0>, it gets lifted
    static_assert(std::is_same_v<
        Then_t<Loop<Done>, Continue<0>>,
        Loop<Continue<1>>>);
}

} // namespace nix::session
