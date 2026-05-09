#include <gtest/gtest.h>
#include <string>
#include <type_traits>

#include "nix/util/session-types/protocol.hh"

namespace nix::session {

TEST(SessionProtocol, TypeListBasics) {
    static_assert(TypeList<int, float, double>::size == 3);
    static_assert(TypeList<>::size == 0);
    static_assert(TypeList<int>::size == 1);
    static_assert(std::is_same_v<Head_t<TypeList<int, float>>, int>);
    static_assert(std::is_same_v<Tail_t<TypeList<int, float>>, TypeList<float>>);
    static_assert(std::is_same_v<Tail_t<TypeList<int>>, TypeList<>>);
    static_assert(std::is_same_v<At_t<TypeList<int, float, double>, 0>, int>);
    static_assert(std::is_same_v<At_t<TypeList<int, float, double>, 1>, float>);
    static_assert(std::is_same_v<At_t<TypeList<int, float, double>, 2>, double>);
}

TEST(SessionProtocol, BranchCount) {
    static_assert(BranchCount<Choose<Done, Done, Done>>::value == 3);
    static_assert(BranchCount<Offer<Done>>::value == 1);
    static_assert(BranchCount<Choose<Done>>::value == 1);
    static_assert(BranchCount<Offer<Done, Done>>::value == 2);
}

TEST(SessionProtocol, IsChooseIsOffer) {
    static_assert(IsChoose<Choose<Done>>::value);
    static_assert(!IsChoose<Offer<Done>>::value);
    static_assert(!IsChoose<Done>::value);
    static_assert(!IsChoose<Send<int, Done>>::value);

    static_assert(IsOffer<Offer<Done>>::value);
    static_assert(!IsOffer<Choose<Done>>::value);
    static_assert(!IsOffer<Done>::value);
    static_assert(!IsOffer<Recv<int, Done>>::value);
}

template<typename T> struct WrapInSend { using type = Send<T, Done>; };

TEST(SessionProtocol, Map) {
    static_assert(std::is_same_v<
        Map_t<WrapInSend, TypeList<int, float>>,
        TypeList<Send<int, Done>, Send<float, Done>>>);
    static_assert(std::is_same_v<
        Map_t<WrapInSend, TypeList<>>,
        TypeList<>>);
}

template<typename T> struct IsNotContinue : std::true_type {};
template<unsigned I> struct IsNotContinue<Continue<I>> : std::false_type {};

TEST(SessionProtocol, All) {
    static_assert(All<IsNotContinue, TypeList<Done, Send<int, Done>>>::value);
    static_assert(!All<IsNotContinue, TypeList<Done, Continue<0>>>::value);
    static_assert(All<IsNotContinue, TypeList<>>::value); // vacuously true
}

TEST(SessionProtocol, ToTypeList) {
    static_assert(std::is_same_v<
        ToTypeList_t<Choose<Send<int, Done>, Done>>,
        TypeList<Send<int, Done>, Done>>);
    static_assert(std::is_same_v<
        ToTypeList_t<Offer<Done>>,
        TypeList<Done>>);
}

TEST(SessionProtocol, IsSendIsRecvIsDoneIsCall) {
    static_assert(IsSend<Send<int, Done>>::value);
    static_assert(!IsSend<Recv<int, Done>>::value);
    static_assert(!IsSend<Done>::value);
    static_assert(!IsSend<Call<Done, Done>>::value);

    static_assert(IsRecv<Recv<int, Done>>::value);
    static_assert(!IsRecv<Send<int, Done>>::value);
    static_assert(!IsRecv<Done>::value);

    static_assert(IsDone<Done>::value);
    static_assert(!IsDone<Send<int, Done>>::value);
    static_assert(!IsDone<Recv<int, Done>>::value);

    static_assert(IsCall<Call<Done, Done>>::value);
    static_assert(IsCall<Call<Send<int, Done>, Recv<float, Done>>>::value);
    static_assert(!IsCall<Send<int, Done>>::value);
    static_assert(!IsCall<Done>::value);
}

TEST(SessionProtocol, PayloadAndContinuation) {
    static_assert(std::is_same_v<Payload_t<Send<int, Done>>, int>);
    static_assert(std::is_same_v<Payload_t<Recv<float, Done>>, float>);
    static_assert(std::is_same_v<Payload_t<Send<std::string, Recv<int, Done>>>, std::string>);

    static_assert(std::is_same_v<Continuation_t<Send<int, Done>>, Done>);
    static_assert(std::is_same_v<Continuation_t<Recv<float, Done>>, Done>);
    static_assert(std::is_same_v<Continuation_t<Send<int, Recv<float, Done>>>, Recv<float, Done>>);
    static_assert(std::is_same_v<Continuation_t<Recv<int, Send<float, Done>>>, Send<float, Done>>);
}

TEST(SessionProtocol, CallTargetAndCallCont) {
    static_assert(std::is_same_v<
        CallTarget_t<Call<Send<int, Done>, Recv<float, Done>>>,
        Send<int, Done>>);
    static_assert(std::is_same_v<
        CallCont_t<Call<Send<int, Done>, Recv<float, Done>>>,
        Recv<float, Done>>);

    // Nested Call
    using Inner = Call<Done, Done>;
    using Outer = Call<Inner, Send<int, Done>>;
    static_assert(std::is_same_v<CallTarget_t<Outer>, Inner>);
    static_assert(std::is_same_v<CallCont_t<Outer>, Send<int, Done>>);
}

TEST(SessionProtocol, ChoiceStruct) {
    Choice c{3};
    EXPECT_EQ(c.index, 3u);

    Choice c2{0};
    EXPECT_EQ(c2.index, 0u);
}

// Decompose trait defaults: verify that non-matching types get the sentinel
// defaults (DecomposeDefault for Payload, Done for the rest). These defaults
// exist so Chan method declarations parse during class template instantiation
// even for protocol types the method doesn't apply to.
TEST(SessionProtocol, DecomposeDefaults) {
    // Payload defaults to DecomposeDefault for non-Send/Recv types.
    static_assert(std::is_same_v<Payload_t<Done>, DecomposeDefault>);
    static_assert(std::is_same_v<Payload_t<Choose<Done>>, DecomposeDefault>);
    static_assert(std::is_same_v<Payload_t<Call<Done, Done>>, DecomposeDefault>);

    // Continuation defaults to Done for non-Send/Recv types.
    static_assert(std::is_same_v<Continuation_t<Done>, Done>);
    static_assert(std::is_same_v<Continuation_t<Choose<Done>>, Done>);

    // CallTarget defaults to Done for non-Call types.
    static_assert(std::is_same_v<CallTarget_t<Done>, Done>);
    static_assert(std::is_same_v<CallTarget_t<Send<int, Done>>, Done>);

    // CallCont defaults to Done for non-Call types.
    static_assert(std::is_same_v<CallCont_t<Done>, Done>);
    static_assert(std::is_same_v<CallCont_t<Recv<int, Done>>, Done>);
}

} // namespace nix::session
