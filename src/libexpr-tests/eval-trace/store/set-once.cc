/**
 * Tests for SetOnce<T> — the write-once wrapper that enforces at runtime that
 * a value is set at most once, throwing nix::Error on a second set attempt.
 */
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"

#include <gtest/gtest.h>
#include <string>

namespace nix::eval_trace {

// ── SetOnce<T> ──────────────────────────────────────────────────────

TEST(SetOnceTest, Initial_State_IsEmpty)
{
    SetOnce<int> s;
    EXPECT_FALSE(s.has_value());
    EXPECT_FALSE(static_cast<bool>(s));
}

TEST(SetOnceTest, FirstSet_Value_Succeeds)
{
    SetOnce<int> s;
    s.set(42);
    EXPECT_TRUE(s.has_value());
    EXPECT_EQ(*s, 42);
}

TEST(SetOnceTest, SecondSet_Value_Throws)
{
    SetOnce<int> s;
    s.set(1);
    EXPECT_THROW(s.set(2), Error);
    // Value unchanged after failed second set.
    EXPECT_EQ(*s, 1);
}

TEST(SetOnceTest, Arrow_Operator_AccessesValue)
{
    SetOnce<std::string> s;
    s.set("hello");
    EXPECT_EQ(s->size(), 5u);
}

} // namespace nix::eval_trace
