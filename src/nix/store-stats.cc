#include "nix/cmd/command.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>

namespace nix {

static std::string renderPow2(uint64_t v)
{
    static constexpr std::array units = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    size_t u = 0;
    while (v >= 1024 && u + 1 < units.size()) {
        v >>= 10;
        ++u;
    }
    return fmt("%d %s", v, units[u]);
}

static std::pair<uint64_t, uint64_t> bucketRange(uint8_t bucket)
{
    return {bucket == 0 ? 0 : uint64_t{1} << bucket, uint64_t{1} << (bucket + 1)};
}

static nlohmann::json histogramToJson(const Store::ContentStats::Histogram & hist)
{
    auto out = nlohmann::json::array();
    for (auto & [bucket, count] : hist) {
        auto [low, high] = bucketRange(bucket);
        out.push_back({{"bucket", bucket}, {"low", low}, {"high", high}, {"count", count}});
    }
    return out;
}

static void printHistogram(const std::string & title, const Store::ContentStats::Histogram & hist)
{
    if (hist.empty()) {
        notice("%s: (empty)", title);
        return;
    }
    notice("%s:", title);
    uint64_t total = 0;
    for (auto & [_, count] : hist)
        total += count;
    for (auto & [bucket, count] : hist) {
        auto [low, high] = bucketRange(bucket);
        double pct = total > 0 ? (100.0 * double(count)) / double(total) : 0.0;
        notice("  [%10s, %10s)  %10d  (%5.1f%%)", renderPow2(low), renderPow2(high), count, pct);
    }
}

struct CmdStatsStore : StoreCommand, MixJSON
{
    Store::ContentStatsOptions opts;

    CmdStatsStore()
    {
        addFlag({
            .longName = "detailed",
            .description = "Walk every store path's contents for dedup metrics and on-disk totals.",
            .handler = {&opts.detailed, true},
        });
        addFlag({
            .longName = "histograms",
            .description = "Add NAR-size and (with --detailed) .links size histograms.",
            .handler = {&opts.histograms, true},
        });
    }

    std::string description() override
    {
        return "show summary statistics about a Nix store";
    }

    std::string doc() override
    {
        return
#include "store-stats.md"
            ;
    }

    void run(ref<Store> store) override
    {
        auto maybeStats = store->queryStoreStats(opts);

        if (!json) {
            notice("Store URL: %s", store->config.getReference().render(/*withParams=*/true));
            if (!maybeStats) {
                notice("Statistics are not available for this store.");
                return;
            }
            auto & s = *maybeStats;
            notice("Valid paths:             %d", s.pathCount);
            notice("Total NAR size:          %s", renderSize(int64_t(s.totalNarSize)));

            if (s.dedup) {
                auto & d = *s.dedup;
                notice("");
                notice("From .links walk:");
                notice("  Unique files:          %d", d.linksFileCount);
                notice("  Deduplicated files:    %d", d.dedupedFileCount);
                notice("  Inodes saved by dedup: %d", d.inodesSaved);
                notice("  Unique bytes:          %s", renderSize(int64_t(d.uniqueBytes)));
                notice("  Unique disk bytes:     %s", renderSize(int64_t(d.uniqueDiskBytes)));
                notice("  Bytes saved by dedup:  %s", renderSize(int64_t(d.dedupBytes)));
                notice("  Disk bytes saved:      %s", renderSize(int64_t(d.dedupDiskBytes)));
            } else if (opts.detailed) {
                notice("Detailed deduplication stats are not available for this store.");
            }

            if (s.fullWalk) {
                auto & w = *s.fullWalk;
                notice("");
                notice("From full store walk:");
                notice("  Total disk bytes:      %s", renderSize(int64_t(w.totalDiskBytes)));
                notice("  Total inodes:          %d", w.totalInodes());
                notice("    files:               %d", w.fileInodes);
                notice("    directories:         %d", w.dirInodes);
                notice("    symlinks:            %d", w.symlinkInodes);
            }

            if (opts.histograms) {
                notice("");
                printHistogram("NAR size distribution", s.narSizeHistogram);
                if (s.dedup) {
                    notice("");
                    printHistogram(".links size distribution", s.dedup->sizeHistogram);
                }
            }
            return;
        }

        nlohmann::json res;
        res["url"] = store->config.getReference().render(/*withParams=*/true);
        res["available"] = bool(maybeStats);
        if (maybeStats) {
            auto & s = *maybeStats;
            res["pathCount"] = s.pathCount;
            res["totalNarSize"] = s.totalNarSize;
            if (opts.histograms)
                res["narSizeHistogram"] = histogramToJson(s.narSizeHistogram);
            if (s.dedup) {
                auto & d = *s.dedup;
                auto & dj = res["dedup"] = {
                    {"linksFileCount", d.linksFileCount},
                    {"dedupedFileCount", d.dedupedFileCount},
                    {"inodesSaved", d.inodesSaved},
                    {"uniqueBytes", d.uniqueBytes},
                    {"uniqueDiskBytes", d.uniqueDiskBytes},
                    {"dedupBytes", d.dedupBytes},
                    {"dedupDiskBytes", d.dedupDiskBytes},
                };
                if (opts.histograms)
                    dj["sizeHistogram"] = histogramToJson(d.sizeHistogram);
            }
            if (s.fullWalk) {
                auto & w = *s.fullWalk;
                res["fullWalk"] = {
                    {"totalDiskBytes", w.totalDiskBytes},
                    {"fileInodes", w.fileInodes},
                    {"dirInodes", w.dirInodes},
                    {"symlinkInodes", w.symlinkInodes},
                    {"totalInodes", w.totalInodes()},
                };
            }
        }
        printJSON(res);
    }
};

static auto rCmdStatsStore = registerCommand2<CmdStatsStore>({"store", "stats"});

} // namespace nix
