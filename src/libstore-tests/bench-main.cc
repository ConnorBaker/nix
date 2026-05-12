#include <benchmark/benchmark.h>
#include "nix/store/globals.hh"
#include "nix/util/logging.hh"

#include <csignal>
#include <cstdlib>

#ifdef __linux__
#  include <pthread.h>
#endif

// Custom main to initialize Nix before running benchmarks
int main(int argc, char ** argv)
{
    // Benchmarks that spawn workers and redirect output can have
    // bursty writes to stdout/stderr; a closed pipe would otherwise
    // SIGPIPE and terminate the process mid-run. Ignoring SIGPIPE
    // turns such writes into EPIPE errors we can observe without
    // crashing the whole bench.
    std::signal(SIGPIPE, SIG_IGN);

    // Set a stable, short main-thread name so bpftrace probes in the
    // nixos test harness (see tests/nixos/nix-store-bench/mk-test.nix)
    // can filter by `comm == "nix-bench"` without relying on Linux's
    // 15-char truncation of the binary name `nix-store-benchmarks`
    // (which truncates to `nix-store-bench`). The truncation made the
    // filter fragile: renaming the binary would silently break the
    // probe and zero out all the bpftrace counters. Setting the comm
    // explicitly makes the contract one-sided — the binary name can
    // change freely, the bpftrace filter just looks for `nix-bench`.
    //
    // 16-byte cap including NUL, per Linux PR_SET_NAME / pthread.
#ifdef __linux__
    pthread_setname_np(pthread_self(), "nix-bench");
#endif

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
