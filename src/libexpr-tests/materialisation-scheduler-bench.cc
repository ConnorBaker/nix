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

/* Parse cache: upsert a parsed Value then look it up (the persistent SQLite
   behind fromJSON-over-readFile). To actually exercise the SQLite — not a no-op
   — point XDG_CACHE_HOME at a temp dir BEFORE the first getParseCache() (the
   cache is a singleton that resolves its dir on first open) and assert the cache
   is `functional`; if the dir were unwritable the cache silently no-ops, which
   would make this measure `encode` + an early return rather than a round-trip. */
static void BM_ParseCacheRoundTrip(benchmark::State & state)
{
    auto root = createTempDir();
    setenv("XDG_CACHE_HOME", (root / "cache").string().c_str(), /*overwrite=*/1);
    createDirs(root / "cache");
    auto st = makeEvalState(root / "store");
    auto cache = getParseCache();

    /* A small attrset Value (closer to a real fromJSON result than a scalar). */
    Value a, b, c;
    a.mkInt(1);
    b.mkString("two", st->mem);
    c.mkBool(true);
    auto builder = st->buildBindings(3);
    builder.insert(st->symbols.create("alpha"), &a);
    builder.insert(st->symbols.create("beta"), &b);
    builder.insert(st->symbols.create("gamma"), &c);
    Value v;
    v.mkAttrs(builder.finish());

    /* Fail loudly rather than silently measure a no-op: prove the SQLite round
       trip actually happens once before timing. */
    if (!cache->upsert("bench:probe", "json", "json-v1", CanonPath::root, *st, v))
        throw Error("parse cache is not functional (XDG_CACHE_HOME unwritable?) — benchmark would be a no-op");

    size_t i = 0;
    for (auto _ : state) {
        auto fp = fmt("bench:%d", i++);
        bool up = cache->upsert(fp, "json", "json-v1", CanonPath::root, *st, v);
        Value out;
        bool hit = cache->lookup(fp, "json", "json-v1", CanonPath::root, *st, out);
        benchmark::DoNotOptimize(up);
        benchmark::DoNotOptimize(hit);
    }
    state.SetItemsProcessed(state.iterations());

    st.reset();
    deletePath(root);
}
BENCHMARK(BM_ParseCacheRoundTrip);

} // namespace nix
