#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <future>
#include <thread>

#include "nix/expr/input-materialisation.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/hash.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/tests/source-accessor.hh"
#include "nix/store/path.hh"

namespace nix {

/**
 * Counting wrapper around `MemorySourceAccessor` that increments
 * `readCalls` on every `readFile`/`readDirectory`/`maybeLstat`. Used
 * to assert "exactly N walks observed" in the InputMaterialisation
 * tests.
 */
struct CountingSourceAccessor : SourceAccessor
{
    ref<MemorySourceAccessor> inner;
    std::atomic<size_t> readCalls{0};

    explicit CountingSourceAccessor(ref<MemorySourceAccessor> inner)
        : inner(inner)
    {
    }

    /* Override only the leaf entry points; `pathExists` deliberately
       routes through `this->maybeLstat` per the wrapper-accessor
       contract in `source-accessor.hh`, so we don't need a separate
       override and we still increment `readCalls` for it. */
    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        ++readCalls;
        inner->readFile(path, sink, sizeCallback);
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        ++readCalls;
        return inner->maybeLstat(path);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        ++readCalls;
        return inner->readDirectory(path);
    }

    std::string readLink(const CanonPath & path) override
    {
        ++readCalls;
        return inner->readLink(path);
    }
};

class InputMaterialisationTest : public LibExprTest
{
protected:
    /** Build a tiny in-memory accessor with one file, wrapped in a
     *  counting accessor for walk-count assertions. */
    ref<CountingSourceAccessor> makeCountingAccessor()
    {
        auto mem = make_ref<MemorySourceAccessor>();
        mem->addFile(CanonPath{"file.txt"}, "hello world");
        return make_ref<CountingSourceAccessor>(mem);
    }
};

TEST_F(InputMaterialisationTest, RegisterDoesNotForce)
{
    auto accessor = makeCountingAccessor();
    InputMaterialisation mat(state.fetchSettings, state.store, accessor, "test", std::nullopt);
    EXPECT_EQ(accessor->readCalls.load(), 0u);
    /* peek before any force returns nullopt without walking. */
    EXPECT_EQ(mat.peekNarHash(), std::nullopt);
    EXPECT_EQ(accessor->readCalls.load(), 0u);
}

TEST_F(InputMaterialisationTest, ForceComputesNarHash)
{
    auto accessor = makeCountingAccessor();
    InputMaterialisation mat(state.fetchSettings, state.store, accessor, "test", std::nullopt);
    auto [storePath, narHash] = mat.force();
    EXPECT_GT(accessor->readCalls.load(), 0u);
    /* The walk produced a SHA-256 NAR hash. */
    EXPECT_EQ(narHash.algo, HashAlgorithm::SHA256);
    /* peek after force returns the same hash. */
    auto peeked = mat.peekNarHash();
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(*peeked, narHash);
    /* The storePath is name-bound. */
    EXPECT_EQ(storePath.name(), "test");
}

TEST_F(InputMaterialisationTest, ForceIsIdempotent)
{
    auto accessor = makeCountingAccessor();
    InputMaterialisation mat(state.fetchSettings, state.store, accessor, "test", std::nullopt);
    auto first = mat.force();
    auto callsAfterFirst = accessor->readCalls.load();
    auto second = mat.force();
    auto callsAfterSecond = accessor->readCalls.load();
    EXPECT_EQ(first, second);
    /* Second force must not re-walk: counts unchanged. */
    EXPECT_EQ(callsAfterFirst, callsAfterSecond);
}

TEST_F(InputMaterialisationTest, PeekReturnsNulloptBeforeForce)
{
    auto accessor = makeCountingAccessor();
    InputMaterialisation mat(state.fetchSettings, state.store, accessor, "test", std::nullopt);
    EXPECT_FALSE(mat.peekNarHash().has_value());
    EXPECT_EQ(accessor->readCalls.load(), 0u);
}

TEST_F(InputMaterialisationTest, PeekReturnsHashAfterForce)
{
    auto accessor = makeCountingAccessor();
    InputMaterialisation mat(state.fetchSettings, state.store, accessor, "test", std::nullopt);
    auto [_, narHash] = mat.force();
    auto peeked = mat.peekNarHash();
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(*peeked, narHash);
}

TEST_F(InputMaterialisationTest, MismatchAgainstExpectedThrows)
{
    auto accessor = makeCountingAccessor();
    /* Construct an "expected" hash that is definitely not what the
       walk will produce. The ergonomic way: a SHA-256 of a different
       input. */
    auto wrong = hashString(HashAlgorithm::SHA256, "definitely-not-the-narhash-of-the-test-tree");
    InputMaterialisation mat(state.fetchSettings, state.store, accessor, "test", wrong);
    EXPECT_THROW(mat.force(), Error);
    /* The mismatch error has errno 102 and the standard message. */
    try {
        mat.force();
        FAIL() << "expected throw";
    } catch (Error & e) {
        EXPECT_NE(std::string(e.what()).find("NAR hash mismatch in input"), std::string::npos);
    }
}

TEST_F(InputMaterialisationTest, FailedForceStaysFailed)
{
    auto accessor = makeCountingAccessor();
    auto wrong = hashString(HashAlgorithm::SHA256, "different-content");
    InputMaterialisation mat(state.fetchSettings, state.store, accessor, "test", wrong);

    std::string firstMessage;
    try {
        mat.force();
        FAIL() << "expected first force to throw";
    } catch (Error & e) {
        firstMessage = e.what();
    }
    auto callsAfterFirst = accessor->readCalls.load();

    std::string secondMessage;
    try {
        mat.force();
        FAIL() << "expected second force to throw";
    } catch (Error & e) {
        secondMessage = e.what();
    }
    auto callsAfterSecond = accessor->readCalls.load();

    /* The same latched error is rethrown — same message text. */
    EXPECT_EQ(firstMessage, secondMessage);
    /* The walk did not re-run on the second force. */
    EXPECT_EQ(callsAfterFirst, callsAfterSecond);
}

TEST_F(InputMaterialisationTest, ConcurrentForceObservesOneWalk)
{
    auto accessor = makeCountingAccessor();
    /* Establish a baseline: how many readCalls does one walk do? */
    auto baselineAccessor = makeCountingAccessor();
    InputMaterialisation baselineMat(state.fetchSettings, state.store, baselineAccessor, "test", std::nullopt);
    (void) baselineMat.force();
    auto baselineWalks = baselineAccessor->readCalls.load();

    /* Now spawn many concurrent forcers. The Sync-based lock plus
       shared_future should ensure only one of them walks. */
    auto mat = make_ref<InputMaterialisation>(state.fetchSettings, state.store, accessor, "test", std::nullopt);

    constexpr size_t nThreads = 16;
    std::vector<std::thread> threads;
    std::vector<std::pair<StorePath, Hash>> results(nThreads, {StorePath::dummy, Hash(HashAlgorithm::SHA256)});
    std::promise<void> startBarrier;
    auto startFuture = startBarrier.get_future().share();

    for (size_t i = 0; i < nThreads; ++i) {
        threads.emplace_back([&, i] {
            startFuture.wait();
            results[i] = mat->force();
        });
    }
    startBarrier.set_value();
    for (auto & t : threads)
        t.join();

    /* All threads observed the same result. */
    for (size_t i = 1; i < nThreads; ++i)
        EXPECT_EQ(results[i], results[0]);

    /* Exactly one walk total. The accessor's readCalls counter
       should match the baseline (within ±0; the Sync-based winner
       election is deterministic in terms of how many walks happen,
       just not which thread wins). */
    EXPECT_EQ(accessor->readCalls.load(), baselineWalks);
}

/* ---- Property tests ---- */

/* FIXME: RC_GTEST_FIXTURE_PROP doesn't call SetUpTestSuite; this
   dummy TEST_F forces the fixture to initialise before the PROPs. */
TEST_F(InputMaterialisationTest, _RapidCheckInit) {}

#ifndef COVERAGE

/* Helper: ensure an accessor always has a root (required by dumpPath).
   The Arbitrary<MemorySourceAccessor> generator can produce an accessor
   with no root if all entries were dropped due to path conflicts or the
   sample had zero entries. InputMaterialisation::force() calls
   fetchToStore2 → dumpPath(/) → lstat(/), which throws if root is
   absent. Inserting root here is the right fix: the generator
   producing an empty-root accessor is not invalid for most tests, but
   for this specific consumer we need a walkable tree. */
static ref<MemorySourceAccessor> ensureRoot(const MemorySourceAccessor & tree)
{
    auto m = make_ref<MemorySourceAccessor>(tree);
    if (!m->maybeLstat(CanonPath::root))
        m->open(CanonPath::root, MemorySourceAccessor::File{MemorySourceAccessor::File::Directory{}});
    return m;
}

/* PROP: for any MemorySourceAccessor tree, force() produces a
   deterministic (storePath, narHash) pair and exactly one walk.
   Subsequent forces return the same pair without additional reads. */
RC_GTEST_FIXTURE_PROP(
    InputMaterialisationTest, ForceIsIdempotentForArbitraryTree, (const MemorySourceAccessor & tree))
{
    auto inner = ensureRoot(tree);
    auto accessor = make_ref<CountingSourceAccessor>(inner);

    InputMaterialisation mat(state.fetchSettings, state.store, accessor, "prop-test", std::nullopt);

    /* First force: walk happens, result recorded. */
    auto first = mat.force();
    auto readsAfterFirst = accessor->readCalls.load();

    /* Second force: no additional walk. */
    auto second = mat.force();
    RC_ASSERT(first == second);
    RC_ASSERT(accessor->readCalls.load() == readsAfterFirst);
}

/* PROP: peekNarHash() before force returns nullopt; after force
   returns the same hash that force() returned. */
RC_GTEST_FIXTURE_PROP(InputMaterialisationTest, PeekBehaviourForArbitraryTree, (const MemorySourceAccessor & tree))
{
    auto accessor = make_ref<CountingSourceAccessor>(ensureRoot(tree));

    InputMaterialisation mat(state.fetchSettings, state.store, accessor, "prop-peek", std::nullopt);

    /* Before force: peek returns nullopt, no reads triggered. */
    RC_ASSERT(!mat.peekNarHash().has_value());
    RC_ASSERT(accessor->readCalls.load() == 0u);

    /* After force: peek returns the same hash. */
    auto [_, narHash] = mat.force();
    auto peeked = mat.peekNarHash();
    RC_ASSERT(peeked.has_value());
    RC_ASSERT(*peeked == narHash);
}

/* PROP: a mat constructed with a wrong expectedNarHash always fails.
   Any accessor content whose actual hash ≠ wrong gives an error. */
RC_GTEST_FIXTURE_PROP(InputMaterialisationTest, WrongExpectedHashAlwaysFails, (const MemorySourceAccessor & tree))
{
    auto accessor = make_ref<CountingSourceAccessor>(ensureRoot(tree));

    /* Compute the real hash first so we can pick a definitely-wrong one. */
    auto realAccessor = make_ref<CountingSourceAccessor>(ensureRoot(tree));
    InputMaterialisation realMat(state.fetchSettings, state.store, realAccessor, "x", std::nullopt);
    auto [_, realHash] = realMat.force();

    /* Produce a guaranteed mismatch. */
    auto wrong = hashString(HashAlgorithm::SHA256, "definitely-not-" + realHash.to_string(HashFormat::Base16, false));
    RC_PRE(wrong != realHash);

    InputMaterialisation mat(state.fetchSettings, state.store, accessor, "prop-wrong", wrong);
    bool threw = false;
    try {
        mat.force();
    } catch (Error &) {
        threw = true;
    }
    RC_ASSERT(threw);

    /* Second call re-throws the latched error, no re-walk. */
    auto readsAfterFirst = accessor->readCalls.load();
    bool threw2 = false;
    try {
        mat.force();
    } catch (Error &) {
        threw2 = true;
    }
    RC_ASSERT(threw2);
    RC_ASSERT(accessor->readCalls.load() == readsAfterFirst);
}

#endif

} // namespace nix
