#include "nix/expr/eval-trace/drv-sharing-probe.hh"
#include "nix/util/environment-variables.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

namespace nix::eval_trace::drv_sharing_probe {

// MEASUREMENT-ONLY global state. Guarded by `g_mutex` because eval can be
// multi-threaded (parallel-eval); the probe must not introduce a data race
// even though it's diagnostic. Off-path entirely when !enabled().

namespace {

bool initEnabled()
{
    auto v = getEnv("NIX_MEASURE_DRV_SHARING");
    bool on = v && *v == "1";
    if (on)
        // Dump at process exit so the probe is self-contained (does not depend
        // on NIX_SHOW_STATS / printStatistics being reached). `dump()` is the
        // public entry (defined below); it re-checks g_enabled.
        std::atexit([] { ::nix::eval_trace::drv_sharing_probe::dump(); });
    return on;
}

bool g_enabled = initEnabled();

std::mutex g_mutex;

// drvPath -> set of distinct consumer pathIds that forced it.
boost::unordered_flat_map<std::string, boost::unordered_flat_set<uint32_t>> g_sharing;

// Thread-local stack of active consumer pathIds; innermost is .back().
thread_local std::vector<uint32_t> t_consumerStack;

} // namespace

bool enabled()
{
    return g_enabled;
}

ConsumerScope::ConsumerScope(uint32_t pathId)
    : active(g_enabled)
{
    if (active)
        t_consumerStack.push_back(pathId);
}

ConsumerScope::~ConsumerScope()
{
    if (active && !t_consumerStack.empty())
        t_consumerStack.pop_back();
}

void recordProducer(std::string_view drvPath)
{
    if (!g_enabled)
        return;
    if (t_consumerStack.empty())
        return; // produced outside any consumer scope — not a flattening site.
    uint32_t consumer = t_consumerStack.back();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sharing[std::string(drvPath)].insert(consumer);
}

void dump()
{
    if (!g_enabled)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);

    // Build the consumers-per-drvPath histogram.
    std::map<size_t, size_t> histogram; // N consumers -> how many drvPaths
    size_t totalProducers = 0;
    size_t totalEdges = 0; // sum of distinct consumers across producers
    size_t maxN = 0;
    for (auto & [drv, consumers] : g_sharing) {
        size_t n = consumers.size();
        ++histogram[n];
        ++totalProducers;
        totalEdges += n;
        maxN = std::max(maxN, n);
    }

    auto path = getEnv("NIX_MEASURE_DRV_SHARING_PATH").value_or("-");
    FILE * out = (path == "-") ? stderr : std::fopen(path.c_str(), "w");
    if (!out)
        out = stderr;

    std::fprintf(out, "=== drv-sharing probe (RFC §8 step 0) ===\n");
    std::fprintf(out, "distinct producer drvPaths: %zu\n", totalProducers);
    if (totalProducers > 0) {
        double mean = static_cast<double>(totalEdges) / static_cast<double>(totalProducers);
        std::fprintf(out, "mean distinct consumers per producer: %.3f\n", mean);
        std::fprintf(out, "max distinct consumers for one producer: %zu\n", maxN);
        // How many producers are shared (N>1) vs singly-consumed (N==1)?
        size_t singly = histogram.count(1) ? histogram[1] : 0;
        size_t shared = totalProducers - singly;
        std::fprintf(out, "singly-consumed (N==1): %zu (%.1f%%)\n", singly,
            100.0 * static_cast<double>(singly) / static_cast<double>(totalProducers));
        std::fprintf(out, "multiply-consumed (N>1): %zu (%.1f%%)\n", shared,
            100.0 * static_cast<double>(shared) / static_cast<double>(totalProducers));
        std::fprintf(out, "consumers-per-producer histogram (N: count):\n");
        for (auto & [n, count] : histogram)
            std::fprintf(out, "  %zu: %zu\n", n, count);
        std::fprintf(out,
            "VERDICT: mean~1 => producers singly-consumed => RFC direction DEAD (reduces to "
            "§3b net loss). mean>>1 => infra-sharing exists => only hot-path cost (§6) remains.\n");
    }
    if (out != stderr)
        std::fclose(out);
}

} // namespace nix::eval_trace::drv_sharing_probe
