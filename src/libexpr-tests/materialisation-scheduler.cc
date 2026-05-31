#include <gtest/gtest.h>

#include <set>
#include <thread>
#include <vector>

#include "nix/expr/eval.hh"
#include "nix/expr/materialisation-scheduler.hh"
#include "nix/expr/tests/libexpr.hh"
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

ref<SourceViewAccessor> sourceViewRoot(ref<SourceAccessor> base);

/**
 * Fixture for `MaterialisationScheduler::outPathsOf`. Uses a
 * per-process temp `local?root=...` store (not `dummy://`) because
 * materialisation calls `addToStoreFromDump`, which `dummy://` does
 * not support — same reason as `BoundaryAuditTest`.
 */
class MaterialisationSchedulerTest : public LibExprTest
{
public:
    MaterialisationSchedulerTest()
        : LibExprTest(openStore("local?root=" + (createTempDir() / "store").string()), [](bool & readOnlyMode) {
            EvalSettings settings{readOnlyMode};
            settings.nixPath = {};
            return settings;
        })
    {
    }

protected:
    /**
     * Register `names.size()` views that all share ONE
     * `SourceContentId` (the cargo-workspace shape: many packages, one
     * source+filter) but differ by `name`. Returns the placeholders in
     * `names` order. Each placeholder differs because
     * `SourcePlaceholder::make` folds in the name; all share the
     * contentId, so `outPathsOf` does one narHash walk and N per-name
     * Copies.
     */
    std::vector<SourcePlaceholder> registerSharedContent(const std::vector<std::string> & names)
    {
        auto mem = make_ref<MemorySourceAccessor>();
        mem->addFile(CanonPath{"file.txt"}, "shared cargo-workspace content");
        auto view = sourceViewRoot(mem);

        /* contentId is content-determined and name-independent. */
        auto shapeHash = hashString(HashAlgorithm::SHA256, "scheduler-test-shape");
        auto contentId = SourceContentId::compute(
            "scheduler-test-fixture", shapeHash, ContentAddressMethod::Raw::NixArchive, StoreReferences{});

        std::vector<SourcePlaceholder> placeholders;
        for (auto & name : names)
            placeholders.push_back(state.materialisationScheduler->registerView(
                MaterialisationScheduler::Registration{
                    .contentId = contentId,
                    .name = name,
                    .method = ContentAddressMethod::Raw::NixArchive,
                    .refs = StoreReferences{},
                    .view = view,
                }));
        return placeholders;
    }
};

/* The parallel Copy loop in `outPathsOf`: N placeholders sharing one
   contentId (distinct names) all materialise to valid, distinct store
   paths. The narHash prelude walks once; the per-name Copies run on a
   ThreadPool. This gates correctness-under-parallelism (no write-write
   contention, no lost results), not a fast-path firing — so it can't
   be "reverted to fail" the way a walk-count test can. */
TEST_F(MaterialisationSchedulerTest, OutPathsOfSharedContentAllValid)
{
    auto placeholders = registerSharedContent({"pkg-a", "pkg-b", "pkg-c", "pkg-d"});

    auto resolved = state.materialisationScheduler->outPathsOf(placeholders);

    ASSERT_EQ(resolved.size(), placeholders.size());
    std::set<StorePath> distinct;
    for (auto & p : placeholders) {
        auto it = resolved.find(p);
        ASSERT_NE(it, resolved.end()) << "placeholder " << p.render() << " missing from result";
        EXPECT_TRUE(state.store->isValidPath(it->second))
            << "store path " << state.store->printStorePath(it->second) << " is not valid";
        distinct.insert(it->second);
    }
    /* Distinct names ⇒ distinct CA store paths (same narHash, different
       name component). */
    EXPECT_EQ(distinct.size(), placeholders.size());
}

/* Idempotence: a second `outPathsOf` over the same placeholders
   returns the same store paths (all already valid, peek-hit path). */
TEST_F(MaterialisationSchedulerTest, OutPathsOfIsIdempotent)
{
    auto placeholders = registerSharedContent({"pkg-a", "pkg-b", "pkg-c", "pkg-d"});

    auto first = state.materialisationScheduler->outPathsOf(placeholders);
    auto second = state.materialisationScheduler->outPathsOf(placeholders);

    ASSERT_EQ(first.size(), second.size());
    for (auto & p : placeholders)
        EXPECT_EQ(first.at(p), second.at(p));
}

/* The single-placeholder short-circuit (load-bearing for
   `materialiseFused`) still yields a valid path. */
TEST_F(MaterialisationSchedulerTest, OutPathsOfSinglePlaceholder)
{
    auto placeholders = registerSharedContent({"solo"});

    auto resolved = state.materialisationScheduler->outPathsOf(placeholders);

    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_TRUE(state.store->isValidPath(resolved.at(placeholders[0])));
}

/* ---------- negative / concurrency invariants ---------- */

namespace {

/* A leaf whose reads throw — used to make the scheduler's narHash walk
   fail, exercising the exception path. Inherits MemorySourceAccessor so
   path resolution works up to the point bytes are demanded. */
struct ThrowingLeaf : MemorySourceAccessor
{
    void readFile(const CanonPath &, Sink &, fun<void(uint64_t)>) override
    {
        throw Error("simulated walk failure");
    }
};

} // namespace

/* PF-5 + winner-election: N placeholders share ONE contentId (so
   `regsByContent_` has N entries for it). `narHashOf` must still resolve
   *a* registration via the index and walk exactly once; a second call
   peek-hits. This pins the contentId→registration index (replacing the
   old linear scan) on the multi-registration case. */
TEST_F(MaterialisationSchedulerTest, NarHashOfResolvesContentIdWithManyRegistrations)
{
    auto placeholders = registerSharedContent({"pkg-a", "pkg-b", "pkg-c"});
    auto reg = state.materialisationScheduler->lookup(placeholders.front());
    ASSERT_NE(reg, nullptr);

    /* First narHashOf computes; second is a cache hit. Both must return
       the same hash, and the lookup must succeed despite N registrations
       sharing the contentId. */
    auto h1 = state.materialisationScheduler->narHashOf(reg->contentId);
    auto h2 = state.materialisationScheduler->narHashOf(reg->contentId);
    EXPECT_EQ(h1, h2);
}

/* A thrown walk must NOT wedge `inFlight_`: after a failed narHashOf, a
   later demand of the SAME contentId must be retryable (here, succeed
   once the throwing view is replaced), not hang or stay poisoned. We
   approximate "retryable" by asserting the throw propagates AND a fresh
   contentId still works (the scheduler isn't globally broken). */
TEST_F(MaterialisationSchedulerTest, ThrownWalkPropagatesAndDoesNotWedgeScheduler)
{
    auto thrower = make_ref<ThrowingLeaf>();
    thrower->root = MemorySourceAccessor::File::Directory{};
    thrower->addFile(CanonPath{"file.txt"}, "x"); // present in structure; readFile throws
    auto view = sourceViewRoot(thrower);
    auto shapeHash = hashString(HashAlgorithm::SHA256, "throwing-shape");
    auto cid = SourceContentId::compute(
        "throwing-fixture", shapeHash, ContentAddressMethod::Raw::NixArchive, StoreReferences{});
    state.materialisationScheduler->registerView(MaterialisationScheduler::Registration{
        .contentId = cid,
        .name = "thrower",
        .method = ContentAddressMethod::Raw::NixArchive,
        .refs = StoreReferences{},
        .view = view,
    });

    /* The walk throws — and re-demanding throws again (latched/retried,
       not hung). */
    EXPECT_THROW(state.materialisationScheduler->narHashOf(cid), Error);
    EXPECT_THROW(state.materialisationScheduler->narHashOf(cid), Error);

    /* The scheduler is not globally wedged: an UNRELATED contentId still
       materialises fine. */
    auto ok = registerSharedContent({"healthy"});
    EXPECT_TRUE(state.store->isValidPath(state.materialisationScheduler->outPathsOf(ok).at(ok[0])));
}

/* Concurrent narHashOf for one contentId coalesces to a single walk
   (the scheduler analogue of InputMaterialisation's
   ConcurrentForceObservesOneWalk). All threads observe the same hash. */
TEST_F(MaterialisationSchedulerTest, ConcurrentNarHashOfObservesOneHash)
{
    auto placeholders = registerSharedContent({"solo"});
    auto cid = state.materialisationScheduler->lookup(placeholders.front())->contentId;

    constexpr int N = 16;
    std::vector<std::thread> threads;
    std::vector<Hash> results(N, Hash(HashAlgorithm::SHA256));
    for (int i = 0; i < N; ++i)
        threads.emplace_back([&, i] { results[i] = state.materialisationScheduler->narHashOf(cid); });
    for (auto & t : threads)
        t.join();

    /* Every thread saw the same (single, coalesced) result. */
    for (int i = 1; i < N; ++i)
        EXPECT_EQ(results[i], results[0]);
}

/* The multi-DISTINCT-contentId branch of outPathsOf (the parallel
   prelude + per-group ThreadPool + shard merge) — previously 0% covered
   (every fixture used one shared contentId). Register two groups with
   DIFFERENT contentIds and assert all paths resolve, valid and distinct. */
TEST_F(MaterialisationSchedulerTest, OutPathsOfMultipleDistinctContentIds)
{
    auto groupA = registerSharedContent({"a1", "a2"});

    /* A second group with different content ⇒ a different contentId. */
    auto mem = make_ref<MemorySourceAccessor>();
    mem->addFile(CanonPath{"other.txt"}, "DIFFERENT content, distinct contentId");
    auto view = sourceViewRoot(mem);
    auto shapeHash = hashString(HashAlgorithm::SHA256, "scheduler-test-shape-B");
    auto cidB = SourceContentId::compute(
        "scheduler-test-fixture-B", shapeHash, ContentAddressMethod::Raw::NixArchive, StoreReferences{});
    std::vector<SourcePlaceholder> all = groupA;
    for (auto & name : {"b1", "b2"})
        all.push_back(state.materialisationScheduler->registerView(MaterialisationScheduler::Registration{
            .contentId = cidB,
            .name = name,
            .method = ContentAddressMethod::Raw::NixArchive,
            .refs = StoreReferences{},
            .view = view,
        }));

    auto resolved = state.materialisationScheduler->outPathsOf(all);
    ASSERT_EQ(resolved.size(), all.size());
    std::set<StorePath> distinct;
    for (auto & p : all) {
        auto it = resolved.find(p);
        ASSERT_NE(it, resolved.end());
        EXPECT_TRUE(state.store->isValidPath(it->second));
        distinct.insert(it->second);
    }
    EXPECT_EQ(distinct.size(), all.size()); // 4 distinct names ⇒ 4 distinct paths
}

/* E-4 regression: `coerceToSingleDerivedPath` on a SourceVirtual string
   must RESOLVE (not throw). The SourceVirtual arm resolves the path to
   Opaque{realPath} but the string body stays the placeholder render, so
   the checked variant's `s != sExpected` guard used to throw "has context
   with the different path". The fix accepts a body that is exactly a
   SourcePlaceholder render. */
TEST_F(MaterialisationSchedulerTest, CoerceToSingleDerivedPathResolvesSourceVirtual)
{
    auto placeholders = registerSharedContent({"derived-path-src"});
    auto & placeholder = placeholders.front();

    /* Build a string Value: body = placeholder render, context = a
       SourceVirtual elem for that placeholder (what addPath emits). */
    Value v;
    NixStringContext context;
    context.insert(NixStringContextElem{NixStringContextElem::SourceVirtual{
        .placeholder = placeholder,
        .name = "derived-path-src",
    }});
    v.mkString(placeholder.render(), context, state.mem);

    SingleDerivedPath d = state.coerceToSingleDerivedPath(noPos, v, "while testing E-4");

    /* It must resolve to the Opaque real store path the scheduler
       materialises for this placeholder — NOT throw, and NOT the
       placeholder body. */
    auto * opaque = std::get_if<SingleDerivedPath::Opaque>(&d.raw());
    ASSERT_NE(opaque, nullptr);
    auto expected = state.materialisationScheduler->outPathOf(placeholder);
    EXPECT_EQ(opaque->path, expected);
    EXPECT_TRUE(state.store->isValidPath(opaque->path));
}

/* A MANGLED body (placeholder render with a suffix, e.g. "${src}/bin")
   must still be REJECTED — the E-4 fix only whitelists a body that is
   EXACTLY a placeholder render (SourcePlaceholder::tryParse is exact). */
TEST_F(MaterialisationSchedulerTest, CoerceToSingleDerivedPathRejectsMangledSourceVirtual)
{
    auto placeholders = registerSharedContent({"mangled-src"});
    auto & placeholder = placeholders.front();

    Value v;
    NixStringContext context;
    context.insert(NixStringContextElem{NixStringContextElem::SourceVirtual{
        .placeholder = placeholder,
        .name = "mangled-src",
    }});
    /* body = render + "/bin" — NOT a bare placeholder render. */
    v.mkString(placeholder.render() + "/bin", context, state.mem);

    EXPECT_THROW(state.coerceToSingleDerivedPath(noPos, v, "while testing E-4 mangled"), Error);
}

} // namespace nix
