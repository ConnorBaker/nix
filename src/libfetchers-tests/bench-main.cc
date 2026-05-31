#include <benchmark/benchmark.h>

#include "nix/store/globals.hh"

/* Custom entry point for the libfetchers microbenchmarks. Mirrors
   libstore-tests/bench-main.cc: initialise libstore (which the fetcher
   cache, store paths, and content-address machinery depend on) before
   handing control to Google Benchmark. No initGC() — these benchmarks
   exercise fetcher/source-accessor mechanisms, not the evaluator. */
int main(int argc, char ** argv)
{
    nix::initLibStore(false);

    ::benchmark::Initialize(&argc, argv);
    ::benchmark::RunSpecifiedBenchmarks();
    return 0;
}
