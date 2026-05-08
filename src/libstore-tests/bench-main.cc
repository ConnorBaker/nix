#include <benchmark/benchmark.h>
#include "nix/store/globals.hh"
#include "nix/util/logging.hh"

#include <csignal>
#include <cstdlib>

// Custom main to initialize Nix before running benchmarks
int main(int argc, char ** argv)
{
    // Benchmarks that spawn workers and redirect output can have
    // bursty writes to stdout/stderr; a closed pipe would otherwise
    // SIGPIPE and terminate the process mid-run. Ignoring SIGPIPE
    // turns such writes into EPIPE errors we can observe without
    // crashing the whole bench.
    std::signal(SIGPIPE, SIG_IGN);

    // Silence Nix's `printInfo` / `printMsg(lvlTalkative|…)` output so
    // the benchmark table isn't interleaved with "finding garbage
    // collector roots..." and similar. Errors and warnings still
    // print. Set `NIX_BENCH_VERBOSITY=4` (or higher) to restore the
    // normal lvlInfo behaviour for debugging.
    if (auto * env = std::getenv("NIX_BENCH_VERBOSITY")) {
        nix::verbosity = static_cast<nix::Verbosity>(std::atoi(env));
    } else {
        nix::verbosity = nix::lvlError;
    }

    // Initialize libstore
    nix::initLibStore(false);

    // Initialize and run benchmarks
    ::benchmark::Initialize(&argc, argv);
    ::benchmark::RunSpecifiedBenchmarks();
    return 0;
}
