#include <gtest/gtest.h>
#include <type_traits>

#include "nix/util/session-types/select.hh"

namespace nix::session {

TEST(SessionSelect, ChooseBranch) {
    static_assert(std::is_same_v<SelectBranch_t<Choose<Send<int, Done>, Done>, 0>, Send<int, Done>>);
    static_assert(std::is_same_v<SelectBranch_t<Choose<Send<int, Done>, Done>, 1>, Done>);
}

TEST(SessionSelect, OfferBranch) {
    static_assert(std::is_same_v<SelectBranch_t<Offer<Done, Recv<int, Done>>, 0>, Done>);
    static_assert(std::is_same_v<SelectBranch_t<Offer<Done, Recv<int, Done>>, 1>, Recv<int, Done>>);
}

TEST(SessionSelect, RemoveAt) {
    static_assert(std::is_same_v<
        RemoveAt_t<TypeList<int, float, double>, 0>,
        TypeList<float, double>>);
    static_assert(std::is_same_v<
        RemoveAt_t<TypeList<int, float, double>, 1>,
        TypeList<int, double>>);
    static_assert(std::is_same_v<
        RemoveAt_t<TypeList<int, float, double>, 2>,
        TypeList<int, float>>);
    static_assert(std::is_same_v<
        RemoveAt_t<TypeList<int>, 0>,
        TypeList<>>);
}

TEST(SessionSelect, ThreeChoices) {
    using C3 = Choose<Send<int, Done>, Recv<float, Done>, Done>;
    static_assert(std::is_same_v<SelectBranch_t<C3, 0>, Send<int, Done>>);
    static_assert(std::is_same_v<SelectBranch_t<C3, 1>, Recv<float, Done>>);
    static_assert(std::is_same_v<SelectBranch_t<C3, 2>, Done>);
}

} // namespace nix::session
