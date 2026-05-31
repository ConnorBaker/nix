/* Microbenchmarks for the runtime materialisation machinery (PROPOSAL.md §2.O).
 *
 * These drive the actual scheduler the cargo-workspace win runs through —
 * `registerView` + `outPathsOf` — not just the pure key helpers. Two shapes:
 *
 *   BM_OutPathsOf_SharedContent: N placeholders sharing ONE SourceContentId
 *     (distinct names) → one narHash walk + copy-once-link-N. This is the
 *     cargo-workspace path end to end through the scheduler.
 *   BM_OutPathsOf_Warm: a second outPathsOf over already-materialised
 *     placeholders → the peek-hit (no walk, no copy) steady state.
 *
 * Plus the parse-cache round-trip (PROPOSAL.md §2.E), the persistent
 * content-keyed Value cache behind `builtins.fromJSON (readFile X)`.
 *
 * Single-tree regression guard; the cross-tree story is benchmarks/.
 */

#include <benchmark/benchmark.h>

#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/materialisation-scheduler.hh"
#include "nix/expr/parse-cache.hh"
#include "nix/expr/value.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/content-address.hh"
#include "nix/store/source-content-id.hh"
#include "nix/store/source-placeholder.hh"
#include "nix/store/store-open.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-view.hh"

namespace nix {

namespace {

/* An EvalState backed by a real local store (materialisation calls
   addToStoreFromDump, which dummy:// lacks). Mirrors the construction in
   materialisation-scheduler.cc's test fixture, minus gtest. */
std::shared_ptr<EvalState> makeEvalState(const std::filesystem::path & root)
{
    createDirs(root);
    auto store = openStore("local?root=" + root.string());
    static fetchers::Settings fetchSettings{};
    bool readOnlyMode = false;
    EvalSettings evalSettings{readOnlyMode};
    evalSettings.nixPath = {};
    return std::make_shared<EvalState>(LookupPath{}, store, fetchSettings, evalSettings, nullptr);
}

/* Register `n` views sharing one contentId but distinct names — the
   cargo-workspace shape (one source+filter, N packages). */
std::vector<SourcePlaceholder> registerSharedContent(EvalState & state, size_t n)
{
    auto mem = make_ref<MemorySourceAccessor>();
    mem->addFile(CanonPath{"file.txt"}, "shared cargo-workspace content");
    auto view = sourceViewRoot(mem);
    auto shapeHash = hashString(HashAlgorithm::SHA256, "bench-shape");
    auto contentId = SourceContentId::compute(
        "bench-fixture", shapeHash, ContentAddressMethod::Raw::NixArchive, StoreReferences{});

    std::vector<SourcePlaceholder> placeholders;
    for (size_t i = 0; i < n; ++i)
        placeholders.push_back(state.materialisationScheduler->registerView(
            MaterialisationScheduler::Registration{
                .contentId = contentId,
                .name = fmt("pkg-%d", i),
                .method = ContentAddressMethod::Raw::NixArchive,
                .refs = StoreReferences{},
                .view = view,
            }));
    return placeholders;
}

} // namespace

/* COLD: N cargo siblings resolved in one batch — one walk + copy-once-link-N. */
static void BM_OutPathsOf_SharedContent(benchmark::State & state)
{
    const size_t n = (size_t) state.range(0);
    for (auto _ : state) {
        state.PauseTiming();
        auto root = createTempDir();
        auto st = makeEvalState(root / "store");
        auto placeholders = registerSharedContent(*st, n);
        state.ResumeTiming();

        auto resolved = st->materialisationScheduler->outPathsOf(placeholders);
        benchmark::DoNotOptimize(resolved);

        state.PauseTiming();
        st.reset();
        deletePath(root);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * (int64_t) n);
}
BENCHMARK(BM_OutPathsOf_SharedContent)->Arg(4)->Arg(16)->Arg(64);

/* WARM: a second outPathsOf over already-materialised placeholders — the
   peek-hit steady state (no walk, no copy). */
static void BM_OutPathsOf_Warm(benchmark::State & state)
{
    const size_t n = (size_t) state.range(0);
    auto root = createTempDir();
    auto st = makeEvalState(root / "store");
    auto placeholders = registerSharedContent(*st, n);
    st->materialisationScheduler->outPathsOf(placeholders); // prime

    for (auto _ : state) {
        auto resolved = st->materialisationScheduler->outPathsOf(placeholders);
        benchmark::DoNotOptimize(resolved);
    }
    state.SetItemsProcessed(state.iterations() * (int64_t) n);

    st.reset();
    deletePath(root);
}
BENCHMARK(BM_OutPathsOf_Warm)->Arg(4)->Arg(16)->Arg(64);

/* Parse cache: upsert a Value tree then look it up (the fromJSON-over-readFile
   warm path). Uses a private HOME so the persistent SQLite is isolated. */
static void BM_ParseCacheRoundTrip(benchmark::State & state)
{
    auto root = createTempDir();
    auto st = makeEvalState(root / "store");
    auto cache = getParseCache();

    /* A modest attrset Value to round-trip. */
    Value v;
    v.mkInt(424242);

    size_t i = 0;
    for (auto _ : state) {
        auto fp = fmt("bench:%d", i++);
        cache->upsert(fp, "json", "json-v1", CanonPath::root, *st, v);
        Value out;
        bool hit = cache->lookup(fp, "json", "json-v1", CanonPath::root, *st, out);
        benchmark::DoNotOptimize(hit);
    }
    state.SetItemsProcessed(state.iterations());

    st.reset();
    deletePath(root);
}
BENCHMARK(BM_ParseCacheRoundTrip);

} // namespace nix
