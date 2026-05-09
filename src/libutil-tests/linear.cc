#include <gtest/gtest.h>
#include <utility>

#include "nix/util/linear.hh"

namespace nix {

// --- Basic test class ---

class Token : public Linear<Token> {
public:
    void consume() { markConsumed(); }
};

TEST(Linear, ConsumedDestructionOk) {
    Token t;
    t.consume();
    // destructor runs without abort
}

TEST(Linear, UnconsumedDestructionAborts) {
    EXPECT_DEATH({
        Token t;
        // destructor fires without consume()
    }, "linearity");
}

// --- Move semantics ---

TEST(Linear, MoveFromUnconsumed) {
    Token a;
    Token b = std::move(a);
    // a is now consumed (moved-from)
    // b is not consumed
    b.consume();
}

// --- Custom linearName ---

class NamedToken : public Linear<NamedToken> {
public:
    static constexpr const char * linearName = "NamedToken";
    void consume() { markConsumed(); }
};

TEST(Linear, CustomNameInDeathMessage) {
    EXPECT_DEATH({
        NamedToken t;
    }, "NamedToken");
}

// --- Two-state pipeline ---

struct StateA {};
struct StateB {};

template<typename State>
class Pipeline : public Linear<Pipeline<State>> {
    int val_;

    template<typename> friend class Pipeline;

public:
    static constexpr const char * linearName = "Pipeline";

    explicit Pipeline(int v) : val_(v) {}

    [[nodiscard]] Pipeline<StateB> advance() &&
        requires std::same_as<State, StateA>
    {
        this->markConsumed();
        return Pipeline<StateB>{val_ + 1};
    }

    int finish() &&
        requires std::same_as<State, StateB>
    {
        this->markConsumed();
        return val_;
    }
};

TEST(Linear, TwoStatePipeline) {
    Pipeline<StateA> a(10);
    Pipeline<StateB> b = std::move(a).advance();
    int result = std::move(b).finish();
    EXPECT_EQ(result, 11);
}

TEST(Linear, SkippingStateAborts) {
    EXPECT_DEATH({
        Pipeline<StateA> a(10);
        // dropping without calling advance() aborts
    }, "Pipeline");
}

} // namespace nix
