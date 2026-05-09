#include <gtest/gtest.h>

#include "nix/expr/counter.hh"
#include "nix/store/tests/test-main.hh"
#include "nix/util/configuration.hh"

int main(int argc, char ** argv)
{
    auto res = nix::testMainForBuidingPre(argc, argv);
    if (res)
        return res;

    // For pipe operator tests in trivial.cc
    nix::experimentalFeatureSettings.set("experimental-features", "pipe-operators");

    // Enable Counter increments globally in the test harness. Production
    // defaults to `Counter::enabled = false` so non-NIX_SHOW_STATS
    // evaluations don't pay the atomic-counter cost; tests want
    // counter observability by default. Individual tests that want a
    // baseline snapshot still construct `PathCountersSnapshot` (which
    // captures baselines AND keeps Counter::enabled on for its scope).
    nix::Counter::enabled = true;

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
