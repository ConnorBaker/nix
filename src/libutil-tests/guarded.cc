#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

#include "nix/util/guarded.hh"

namespace nix {

TEST(Guarded, WithLockBasicMutate) {
    Guarded<int> g(0);
    g.withLock([](int & val) {
        val = 42;
    });
    auto result = g.withLock([](int & val) { return val; });
    EXPECT_EQ(result, 42);
}

TEST(Guarded, WithLockReturnValue) {
    Guarded<std::string> g("hello");
    auto len = g.withLock([](std::string & s) { return s.size(); });
    EXPECT_EQ(len, 5u);
}

TEST(Guarded, WithLockConst) {
    const Guarded<int> g(42);
    auto result = g.withLock([](const int & val) { return val; });
    EXPECT_EQ(result, 42);
}

TEST(Guarded, ThreadSafety) {
    Guarded<int> g(0);
    constexpr int kThreads = 8;
    constexpr int kIncr = 1000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < kIncr; ++j) {
                g.withLock([](int & val) { ++val; });
            }
        });
    }
    for (auto & t : threads)
        t.join();

    auto result = g.withLock([](int & val) { return val; });
    EXPECT_EQ(result, kThreads * kIncr);
}

struct NonDefaultConstructible {
    int val;
    explicit NonDefaultConstructible(int v) : val(v) {}
};

TEST(Guarded, NonDefaultConstructibleT) {
    Guarded<NonDefaultConstructible> g(42);
    auto result = g.withLock([](NonDefaultConstructible & obj) { return obj.val; });
    EXPECT_EQ(result, 42);
}

} // namespace nix
