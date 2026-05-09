#include <gtest/gtest.h>
#include <type_traits>

#include "nix/util/session-types/actionable.hh"

namespace nix::session {

TEST(SessionActionable, ConcreteActionsAreSelf) {
    static_assert(std::is_same_v<Action_t<Done>, Done>);
    static_assert(std::is_same_v<Action_t<Send<int, Done>>, Send<int, Done>>);
    static_assert(std::is_same_v<Action_t<Recv<int, Done>>, Recv<int, Done>>);
    static_assert(std::is_same_v<Action_t<Choose<Done, Done>>, Choose<Done, Done>>);
    static_assert(std::is_same_v<Action_t<Offer<Done, Done>>, Offer<Done, Done>>);
    static_assert(std::is_same_v<Action_t<Call<Done, Done>>, Call<Done, Done>>);
    static_assert(std::is_same_v<Action_t<Split<Done, Done, Done>>, Split<Done, Done, Done>>);
}

TEST(SessionActionable, LoopUnrolling) {
    // Loop<Send<int, Continue<0>>>
    // -> Subst(Send<int, Continue<0>>, Loop<Send<int, Continue<0>>>, 0)
    // -> Send<int, Loop<Send<int, Continue<0>>>>
    // -> Action(Send<...>) = Send<...>
    static_assert(std::is_same_v<
        Action_t<Loop<Send<int, Continue<0>>>>,
        Send<int, Loop<Send<int, Continue<0>>>>>);
}

TEST(SessionActionable, LoopWithChoose) {
    static_assert(std::is_same_v<
        Action_t<Loop<Choose<Send<int, Continue<0>>, Done>>>,
        Choose<Send<int, Loop<Choose<Send<int, Continue<0>>, Done>>>, Done>>);
}

TEST(SessionActionable, Idempotency) {
    static_assert(std::is_same_v<
        Action_t<Action_t<Loop<Send<int, Continue<0>>>>>,
        Action_t<Loop<Send<int, Continue<0>>>>>);
}

TEST(SessionActionable, ActionableConcept) {
    static_assert(Actionable<Done>);
    static_assert(Actionable<Send<int, Done>>);
    static_assert(Actionable<Loop<Send<int, Continue<0>>>>);
}

} // namespace nix::session
