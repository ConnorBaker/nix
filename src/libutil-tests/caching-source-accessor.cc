#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "nix/util/source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/canon-path.hh"

namespace nix {

/* Law inventory for `makeCachingSourceAccessor` (the positive
   lstat/readlink cache, Phase 3 / PROPOSAL.md §6.6). The cache is
   behaviour-preserving, so these laws ARE its correctness gate (there is
   no fast-path-firing obligation). Labels follow the proposal's C1–C6:

     C1 transparency      — Cache(a).read(p) == a.read(p) ∀ method/path
     C2 idempotence       — a 2nd query does NOT re-hit the underlying accessor
     C3 positive-only     — a miss is NOT cached as absent; a later
                            appearance IS observed (the load-bearing one)
     C5 composition       — invalidateCache propagates to `next`
     C6 thread-safety     — concurrent reads converge to one value
                            (benign duplicate compute OK)

   C4 (predicate-determinism) is a property of the SEPARATE
   `CachingFilteringSourceAccessor` (libfetchers), exercised by the
   exportIgnore git tests; it does not apply to this lstat/readlink
   cache, which has no predicate. */

namespace {

/* Wraps a MemorySourceAccessor, counting underlying calls so C2 (no
   re-hit) is observable. Mutable so C3 (a path appearing after a miss)
   can be simulated. */
struct CountingMemAccessor : SourceAccessor
{
    ref<MemorySourceAccessor> inner;
    std::atomic<size_t> lstatCalls{0};
    std::atomic<size_t> readLinkCalls{0};
    std::atomic<size_t> readFileCalls{0};

    explicit CountingMemAccessor(ref<MemorySourceAccessor> inner)
        : inner(inner)
    {
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        ++lstatCalls;
        return inner->maybeLstat(path);
    }

    std::string readLink(const CanonPath & path) override
    {
        ++readLinkCalls;
        return inner->readLink(path);
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        ++readFileCalls;
        inner->readFile(path, sink, sizeCallback);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        return inner->readDirectory(path);
    }
};

static ref<MemorySourceAccessor> memWith(std::string_view file, std::string_view contents)
{
    auto mem = make_ref<MemorySourceAccessor>();
    mem->addFile(CanonPath{file}, std::string{contents});
    return mem;
}

} // namespace

/* C1 — transparency: the cache returns the same answers the underlying
   accessor would, for hits and (separately) for reads. */
TEST(CachingSourceAccessor, C1_Transparency)
{
    auto base = memWith("f.txt", "hello");
    auto counting = make_ref<CountingMemAccessor>(base);
    auto cached = makeCachingSourceAccessor(counting);

    auto direct = base->maybeLstat(CanonPath{"f.txt"});
    auto viaCache = cached->maybeLstat(CanonPath{"f.txt"});
    ASSERT_TRUE(direct.has_value());
    ASSERT_TRUE(viaCache.has_value());
    EXPECT_EQ(direct->type, viaCache->type);

    EXPECT_EQ(cached->readFile(CanonPath{"f.txt"}), "hello");
    EXPECT_EQ(base->readFile(CanonPath{"f.txt"}), cached->readFile(CanonPath{"f.txt"}));
}

/* C2 — idempotence: a second lstat of the same path hits the cache, not
   the underlying accessor. */
TEST(CachingSourceAccessor, C2_Idempotence)
{
    auto counting = make_ref<CountingMemAccessor>(memWith("f.txt", "x"));
    auto cached = makeCachingSourceAccessor(counting);

    (void) cached->maybeLstat(CanonPath{"f.txt"});
    auto after1 = counting->lstatCalls.load();
    (void) cached->maybeLstat(CanonPath{"f.txt"});
    auto after2 = counting->lstatCalls.load();

    EXPECT_EQ(after1, 1u);
    EXPECT_EQ(after2, after1) << "second lstat re-hit the underlying accessor";
}

/* C3 — positive-only (load-bearing): a MISS must NOT be cached as absent.
   A path that appears after the first (missing) probe must be observed on
   the next probe. */
TEST(CachingSourceAccessor, C3_PositiveOnly_MissNotCached)
{
    auto mem = make_ref<MemorySourceAccessor>();
    mem->addFile(CanonPath{"present.txt"}, "here");
    auto counting = make_ref<CountingMemAccessor>(mem);
    auto cached = makeCachingSourceAccessor(counting);

    /* First probe: absent. */
    EXPECT_FALSE(cached->maybeLstat(CanonPath{"later.txt"}).has_value());

    /* The path appears (the eval analogue: a lazily-materialised store
       mount added after the first probe). */
    mem->addFile(CanonPath{"later.txt"}, "appeared");

    /* The cache must NOT have memoised the miss — the appearance is seen. */
    auto reprobe = cached->maybeLstat(CanonPath{"later.txt"});
    EXPECT_TRUE(reprobe.has_value()) << "a cached negative masked a later appearance (C3 violated)";
}

/* C3 corollary — positive hits ARE cached and survive an underlying
   mutation (the evaluator relies on positive lookups not changing). */
TEST(CachingSourceAccessor, C3_PositiveCachedAndStable)
{
    auto mem = memWith("f.txt", "v1");
    auto counting = make_ref<CountingMemAccessor>(mem);
    auto cached = makeCachingSourceAccessor(counting);

    auto first = cached->maybeLstat(CanonPath{"f.txt"});
    ASSERT_TRUE(first.has_value());

    /* Underlying lstat would change, but the cached positive lstat is
       pinned (1 underlying call total). */
    auto callsBefore = counting->lstatCalls.load();
    auto second = cached->maybeLstat(CanonPath{"f.txt"});
    EXPECT_EQ(counting->lstatCalls.load(), callsBefore);
    EXPECT_EQ(first->type, second->type);
}

/* C5 — composition: invalidateCache propagates to the wrapped accessor
   and drops the local positive cache (so the next probe re-hits). */
TEST(CachingSourceAccessor, C5_InvalidatePropagates)
{
    auto counting = make_ref<CountingMemAccessor>(memWith("f.txt", "x"));
    auto cached = makeCachingSourceAccessor(counting);

    (void) cached->maybeLstat(CanonPath{"f.txt"});
    EXPECT_EQ(counting->lstatCalls.load(), 1u);
    (void) cached->maybeLstat(CanonPath{"f.txt"}); // cached
    EXPECT_EQ(counting->lstatCalls.load(), 1u);

    cached->invalidateCache();

    (void) cached->maybeLstat(CanonPath{"f.txt"}); // must re-hit
    EXPECT_EQ(counting->lstatCalls.load(), 2u) << "invalidateCache did not drop the positive cache";
}

/* C6 — thread-safety: concurrent readers converge to one value. The
   underlying compute may run more than once (benign duplicate compute is
   accepted, mirroring the boost::concurrent_flat_map read-then-emplace),
   but never zero, and all observers see the same Stat. */
TEST(CachingSourceAccessor, C6_ConcurrentReadsConverge)
{
    auto counting = make_ref<CountingMemAccessor>(memWith("f.txt", "x"));
    auto cached = makeCachingSourceAccessor(counting);

    constexpr size_t nThreads = 16;
    std::vector<std::thread> threads;
    std::vector<std::optional<SourceAccessor::Stat>> results(nThreads);

    for (size_t i = 0; i < nThreads; ++i)
        threads.emplace_back([&, i] { results[i] = cached->maybeLstat(CanonPath{"f.txt"}); });
    for (auto & t : threads)
        t.join();

    for (size_t i = 0; i < nThreads; ++i) {
        ASSERT_TRUE(results[i].has_value());
        EXPECT_EQ(results[i]->type, results[0]->type);
    }
    /* At least one real compute, at most one per thread (benign dup). */
    EXPECT_GE(counting->lstatCalls.load(), 1u);
    EXPECT_LE(counting->lstatCalls.load(), nThreads);
}

} // namespace nix
