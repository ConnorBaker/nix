#include "nix/expr/eval-trace/drv-benefit-probe.hh"
#include "nix/util/environment-variables.hh"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace nix::eval_trace::drv_benefit_probe {

namespace {

// Number of CanonicalQueryKind variants (VolatileTime is the last, value 16).
constexpr size_t kNumKinds = 17;

std::mutex g_mutex;

// Per-kind total dep count across all derivationStrict ranges (the flattened
// deps an edge would replace).
std::array<uint64_t, kNumKinds> g_kindCounts{};
uint64_t g_totalDeps = 0;
uint64_t g_numDerivations = 0;
uint64_t g_maxRange = 0;

// Consumer-trace side (benefit denominator).
std::array<uint64_t, kNumKinds> g_consumerKindCounts{};
uint64_t g_consumerTotalDeps = 0;
uint64_t g_numConsumerTraces = 0;
uint64_t g_consumerMax = 0;

void dumpImpl();

bool initEnabled()
{
    auto v = getEnv("NIX_MEASURE_DRV_BENEFIT");
    bool on = v && *v == "1";
    if (on)
        std::atexit([] { dumpImpl(); });
    return on;
}

bool g_enabled = initEnabled();

void dumpImpl()
{
    if (!g_enabled)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);

    auto path = getEnv("NIX_MEASURE_DRV_BENEFIT_PATH").value_or("-");
    FILE * out = (path == "-") ? stderr : std::fopen(path.c_str(), "w");
    if (!out)
        out = stderr;

    std::fprintf(out, "=== drv-benefit probe (RFC §8 step 3 — benefit magnitude) ===\n");
    std::fprintf(out, "derivationStrict calls measured: %llu\n",
        static_cast<unsigned long long>(g_numDerivations));
    std::fprintf(out, "total flattened deps across all derivation ranges: %llu\n",
        static_cast<unsigned long long>(g_totalDeps));
    if (g_numDerivations > 0)
        std::fprintf(out, "mean flattened deps per derivation: %.1f\n",
            static_cast<double>(g_totalDeps) / static_cast<double>(g_numDerivations));
    std::fprintf(out, "max range for one derivation: %llu\n",
        static_cast<unsigned long long>(g_maxRange));
    std::fprintf(out, "per-kind breakdown (kind: count, %% of total):\n");
    if (g_totalDeps > 0) {
        for (size_t k = 0; k < kNumKinds; ++k) {
            if (g_kindCounts[k] == 0)
                continue;
            auto kind = static_cast<CanonicalQueryKind>(k);
            std::fprintf(out, "  %-22s %12llu  %5.1f%%\n",
                std::string(queryKindName(kind)).c_str(),
                static_cast<unsigned long long>(g_kindCounts[k]),
                100.0 * static_cast<double>(g_kindCounts[k]) / static_cast<double>(g_totalDeps));
        }
    }
    std::fprintf(out, "\n--- CONSUMER traces (the benefit denominator) ---\n");
    std::fprintf(out, "consumer traces measured: %llu\n",
        static_cast<unsigned long long>(g_numConsumerTraces));
    std::fprintf(out, "total consumer-trace deps: %llu\n",
        static_cast<unsigned long long>(g_consumerTotalDeps));
    if (g_numConsumerTraces > 0)
        std::fprintf(out, "mean deps per consumer trace: %.1f (max %llu)\n",
            static_cast<double>(g_consumerTotalDeps) / static_cast<double>(g_numConsumerTraces),
            static_cast<unsigned long long>(g_consumerMax));
    if (g_consumerTotalDeps > 0) {
        std::fprintf(out, "consumer per-kind breakdown (kind: count, %% of consumer total):\n");
        for (size_t k = 0; k < kNumKinds; ++k) {
            if (g_consumerKindCounts[k] == 0)
                continue;
            auto kind = static_cast<CanonicalQueryKind>(k);
            std::fprintf(out, "  %-22s %12llu  %5.1f%%\n",
                std::string(queryKindName(kind)).c_str(),
                static_cast<unsigned long long>(g_consumerKindCounts[k]),
                100.0 * static_cast<double>(g_consumerKindCounts[k]) / static_cast<double>(g_consumerTotalDeps));
        }
        // Edge-removable estimate: SPA deps in consumer traces are 1:1 with the
        // derivations a consumer references; FileBytes/DerivedStorePath/StructProj/
        // ImplicitStruct in a consumer MAY be derivation-closure (flattened in) or the
        // consumer's own reads — the probe cannot tell them apart from the stored set
        // alone (the honest limit, see follow-up #12 part 2). So we report BOUNDS:
        //   lower = SPA fraction (definitely-derivation: the .drv deps themselves)
        //   the rest is the ambiguous middle that an edge removes ONLY if it was
        //   derivation-closure-flattened.
        uint64_t spa = g_consumerKindCounts[static_cast<size_t>(CanonicalQueryKind::StorePathAvailability)];
        std::fprintf(out,
            "\nBENEFIT BOUND: consumer SPA deps = %.1f%% of consumer flattening — these are the\n"
            "  .drv references, definitely edge-collapsible. The remaining content/struct deps are\n"
            "  edge-removable ONLY insofar as they were derivation-closure-flattened (vs the\n"
            "  consumer's own reads); the stored set cannot distinguish them, so true benefit is\n"
            "  between 'SPA-only' and 'SPA + all content/struct'. Compare the consumer breakdown\n"
            "  here against the derivation-range breakdown above to judge where in that band it sits.\n",
            100.0 * static_cast<double>(spa) / static_cast<double>(g_consumerTotalDeps));
    }
    if (out != stderr)
        std::fclose(out);
}

} // namespace

bool enabled()
{
    return g_enabled;
}

void recordDerivationRange(const std::vector<Dep> & deps)
{
    if (!g_enabled)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_numDerivations;
    g_totalDeps += deps.size();
    if (deps.size() > g_maxRange)
        g_maxRange = deps.size();
    for (const auto & dep : deps) {
        auto k = static_cast<size_t>(dep.key.kind);
        if (k < kNumKinds)
            ++g_kindCounts[k];
    }
}

void recordConsumerTrace(const std::vector<Dep> & deps)
{
    if (!g_enabled)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_numConsumerTraces;
    g_consumerTotalDeps += deps.size();
    if (deps.size() > g_consumerMax)
        g_consumerMax = deps.size();
    for (const auto & dep : deps) {
        auto k = static_cast<size_t>(dep.key.kind);
        if (k < kNumKinds)
            ++g_consumerKindCounts[k];
    }
}

void dump()
{
    dumpImpl();
}

} // namespace nix::eval_trace::drv_benefit_probe
