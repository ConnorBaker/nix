#include <gtest/gtest.h>
#include <concepts>
#include <type_traits>

#include "nix/util/singleton/dispatch.hh"

namespace nix::singleton {

// --- static_assert: value and value_type ---

static_assert(Tag<42>::value == 42);
static_assert(std::same_as<Tag<42>::value_type, int>);

static_assert(Tag<'x'>::value == 'x');
static_assert(std::same_as<Tag<'x'>::value_type, char>);

// --- enum class ---

enum class Color { Red, Green, Blue };

static_assert(Tag<Color::Red>::value == Color::Red);
static_assert(std::same_as<Tag<Color::Red>::value_type, Color>);

// --- implicit conversion ---

TEST(SingletonDispatch, ImplicitConversion) {
    Color c = Tag<Color::Green>{};
    EXPECT_EQ(c, Color::Green);
}

TEST(SingletonDispatch, IntConversion) {
    int v = Tag<42>{};
    EXPECT_EQ(v, 42);
}

// --- Full dispatch pattern ---

enum class Op { Add, Mul };

template<typename F>
decltype(auto) dispatchOp(Op op, F && f)
{
    switch (op) {
    case Op::Add: return f(Tag<Op::Add>{});
    case Op::Mul: return f(Tag<Op::Mul>{});
    }
    __builtin_unreachable();
}

template<Op> int compute(int a, int b) = delete;
template<> int compute<Op::Add>(int a, int b) { return a + b; }
template<> int compute<Op::Mul>(int a, int b) { return a * b; }

TEST(SingletonDispatch, FullDispatchPattern) {
    auto result = dispatchOp(Op::Add, [](auto tag) {
        return compute<decltype(tag)::value>(3, 4);
    });
    EXPECT_EQ(result, 7);

    result = dispatchOp(Op::Mul, [](auto tag) {
        return compute<decltype(tag)::value>(3, 4);
    });
    EXPECT_EQ(result, 12);
}

// --- Different NTTP types in same test ---

TEST(SingletonDispatch, DifferentNTTPTypes) {
    static_assert(Tag<true>::value == true);
    static_assert(std::same_as<Tag<true>::value_type, bool>);

    static_assert(Tag<100u>::value == 100u);
    static_assert(std::same_as<Tag<100u>::value_type, unsigned int>);
}

} // namespace nix::singleton
