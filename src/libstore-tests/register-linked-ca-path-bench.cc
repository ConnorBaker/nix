#include <benchmark/benchmark.h>

#include "nix/store/local-store.hh"
#include "nix/store/store-open.hh"
#include "nix/store/content-address.hh"
#include "nix/store/source-content-id.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/serialise.hh"

#ifndef _WIN32

#  include <filesystem>

namespace nix {

/* Copy-once-link-N (PROPOSAL.md §4 / §2.O — the headline cargo-workspace win).
 *
 * A cargo workspace is N packages sharing ONE source content but distinct
 * `name`s, so they resolve to N distinct (name-stamped) CA store paths with
 * byte-identical NARs. Eager Nix copies the bytes N times; our scheduler copies
 * once (`addToStoreFromDump`) and hardlinks the remaining N-1 into place
 * (`registerLinkedCAPath`). These two benchmarks measure exactly that contrast
 * at the store primitive level — the same calls `materialiseGroup` makes — so
 * the saving is attributable, not buried in an end-to-end eval.
 *
 * Single-tree regression guard; the cross-tree copy-count contrast is the
 * `cargo` workload in benchmarks/.
 */

namespace {

/* A `bytes`-sized file, NAR-serialised once into a string so both strategies
   ingest byte-identical input (a fresh `StringSource` per sibling). */
std::string makeNar(uint64_t bytes)
{
    auto mem = make_ref<MemorySourceAccessor>();
    mem->addFile(CanonPath{"data"}, std::string(bytes, 'x'));
    StringSink sink;
    mem->dumpPath(CanonPath::root, sink);
    return std::move(sink.s);
}

std::shared_ptr<LocalStore> freshStore(const std::filesystem::path & root)
{
    createDirs(root);
    std::shared_ptr<Store> store = openStore(fmt("local?root=%s", root.string()));
    auto local = std::dynamic_pointer_cast<LocalStore>(store);
    if (!local)
        throw Error("expected a LocalStore");
    return local;
}

} // namespace

/* COPY-N: ingest N identical-content siblings, each via an independent
   `addToStoreFromDump` (what eager Nix does — N full byte copies). */
static void BM_CopyN_Independent(benchmark::State & state)
{
    const size_t n = (size_t) state.range(0);
    const uint64_t fileBytes = 64 * 1024;

    for (auto _ : state) {
        state.PauseTiming();
        auto root = createTempDir();
        auto store = freshStore(root / "nix/store");
        auto nar = makeNar(fileBytes);
        state.ResumeTiming();

        for (size_t i = 0; i < n; ++i) {
            StringSource src{nar};
            auto path = store->addToStoreFromDump(
                src,
                fmt("pkg-%d", i),
                FileSerialisationMethod::NixArchive,
                ContentAddressMethod::Raw::NixArchive,
                HashAlgorithm::SHA256,
                {},
                NoRepair);
            benchmark::DoNotOptimize(path);
        }

        state.PauseTiming();
        store.reset();
        deletePath(root);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * (int64_t) n);
}
BENCHMARK(BM_CopyN_Independent)->Arg(4)->Arg(16)->Arg(64);

/* COPY-ONCE-LINK-N: copy the first sibling, then hardlink the remaining N-1
   into their name-stamped CA paths via `registerLinkedCAPath` (what our
   scheduler does). Mirrors materialisation-scheduler.cc::materialiseGroup. */
static void BM_CopyOnceLinkN(benchmark::State & state)
{
    const size_t n = (size_t) state.range(0);
    const uint64_t fileBytes = 64 * 1024;

    for (auto _ : state) {
        state.PauseTiming();
        auto root = createTempDir();
        auto store = freshStore(root / "nix/store");
        auto nar = makeNar(fileBytes);
        state.ResumeTiming();

        /* First sibling: a real copy (also yields the narHash for the rest). */
        StorePath firstPath = ([&] {
            StringSource src{nar};
            return store->addToStoreFromDump(
                src,
                "pkg-0",
                FileSerialisationMethod::NixArchive,
                ContentAddressMethod::Raw::NixArchive,
                HashAlgorithm::SHA256,
                {},
                NoRepair);
        })();
        auto firstInfo = store->queryPathInfo(firstPath);
        Hash narHash = firstInfo->narHash;

        /* The remaining N-1: hardlink, no re-copy. */
        for (size_t i = 1; i < n; ++i) {
            auto desc = ContentAddressWithReferences::fromParts(
                ContentAddressMethod::Raw::NixArchive, narHash, StoreReferences{});
            auto info = ValidPathInfo::makeFromCA(*store, fmt("pkg-%d", i), std::move(desc), narHash);
            info.narSize = firstInfo->narSize;
            store->registerLinkedCAPath(firstPath, info);
            benchmark::DoNotOptimize(info.path);
        }

        state.PauseTiming();
        store.reset();
        deletePath(root);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * (int64_t) n);
}
BENCHMARK(BM_CopyOnceLinkN)->Arg(4)->Arg(16)->Arg(64);

/* SourceContentId::compute — the content-determined key every virtualised
   source hashes to (PROPOSAL.md §1.1). Runs once per addPath/registerView, so
   its cost is on the eval hot path for monorepo-scale workloads. */
static void BM_SourceContentIdCompute(benchmark::State & state)
{
    auto shape = hashString(HashAlgorithm::SHA256, "bench-shape");
    auto method = ContentAddressMethod::Raw::NixArchive;
    StoreReferences refs{};
    for (auto _ : state) {
        auto id = SourceContentId::compute("git:0123456789abcdef0123456789abcdef01234567", shape, method, refs);
        benchmark::DoNotOptimize(id);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SourceContentIdCompute);

} // namespace nix

#endif
