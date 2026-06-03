#include <gtest/gtest.h>

#include "nix/store/async-path-writer.hh"
#include "nix/store/store-open.hh"
#include "nix/store/derivations.hh"
#include "nix/store/tests/libstore.hh"
#include "nix/store/tests/counting-store.hh"
#include "nix/util/file-system.hh"
#include "nix/util/serialise.hh"

#include <chrono>
#include <thread>

/**
 * Behavioural tests for the ported `AsyncPathWriter` (the `.drv` write-queue,
 * PROPOSAL-LAZY-DERIVATIONS.md §3/§4.1). Covers Increment 1's acceptance:
 * round-trip enqueue→drain, the LD-P6 dedup, and LD-X1/LD-V4 failure latching.
 */
namespace nix {

/* A real on-disk chroot store: the dummy store deliberately rejects
   `addToStoreFromDump`, so successful-write tests need a local store. */
class AsyncPathWriterTest : public LibStoreTest
{
protected:
    std::filesystem::path root = createTempDir();
    ref<Store> localStore = openStore(fmt("local?root=%s", root.string()));

    ~AsyncPathWriterTest() override
    {
        try {
            deletePath(root);
        } catch (...) {
        }
    }
};

/* The path is content-addressed and returned synchronously; the worker writes
   it; `waitForAllPaths` is the drain barrier after which it is valid. */
TEST_F(AsyncPathWriterTest, RoundTripWrites)
{
    auto writer = AsyncPathWriter::make(localStore);
    auto p = writer->addPath("hello world", "hello", {});
    writer->waitForAllPaths();
    EXPECT_TRUE(localStore->isValidPath(p));
}

/* LD-P6 — enqueue dedup (correctness half): two enqueues of identical content
   collapse to one content-addressed path and drain cleanly. The falsifiable
   "exactly one write" count is exercised at the functional layer in
   Increment 3 (deferred-write markers), where it is observable end-to-end. */
TEST_F(AsyncPathWriterTest, DuplicateEnqueueSamePath)
{
    auto writer = AsyncPathWriter::make(localStore);
    auto p1 = writer->addPath("dup", "dup", {});
    auto p2 = writer->addPath("dup", "dup", {});
    EXPECT_EQ(p1, p2);
    writer->waitForAllPaths();
    EXPECT_TRUE(localStore->isValidPath(p1));
}

/* LD-X1 / LD-V4 — failure latches to every observer. The dummy store throws
   from `addToStoreFromDump` on a `.drv` name, so the background write fails;
   the exception must propagate (via `set_exception`) to each `waitForPath`
   and to `waitForAllPaths`, not be silently swallowed. `store` here is the
   dummy store from the LibStoreTest fixture. */
TEST_F(LibStoreTest, AsyncWriteFailureLatchesToAllObservers)
{
    auto writer = AsyncPathWriter::make(store);
    auto p = writer->addPath("(content)", "foo.drv", {});
    EXPECT_THROW(writer->waitForPath(p), Error);    // first observer
    EXPECT_THROW(writer->waitForPath(p), Error);    // latched: still throws
    EXPECT_THROW(writer->waitForAllPaths(), Error); // and the bulk drain
}

/* LD-D2 + LD-D4 — the batched flush is a monotone join into the store: two
   independent objects flush to the same content-addressed paths regardless of
   enqueue order (commutativity), and a second flush only *adds* — the first
   path stays valid and unchanged (monotonicity). */
TEST_F(AsyncPathWriterTest, BulkFlushOrderIndependentAndMonotone)
{
    auto w = AsyncPathWriter::make(localStore);
    auto a = w->addPath("content-A", "a", {});
    auto b = w->addPath("content-B", "b", {});
    w->waitForAllPaths();
    EXPECT_TRUE(localStore->isValidPath(a));
    EXPECT_TRUE(localStore->isValidPath(b));

    /* A fresh store, reverse enqueue order: identical content-addressed paths,
       both valid (commutativity). */
    auto root2 = createTempDir();
    auto store2 = openStore(fmt("local?root=%s", root2.string()));
    auto w2 = AsyncPathWriter::make(store2);
    auto b2 = w2->addPath("content-B", "b", {});
    auto a2 = w2->addPath("content-A", "a", {});
    w2->waitForAllPaths();
    EXPECT_EQ(a, a2);
    EXPECT_EQ(b, b2);
    EXPECT_TRUE(store2->isValidPath(a2));
    EXPECT_TRUE(store2->isValidPath(b2));

    /* A later flush only adds; `a` stays valid (monotone domain extension). */
    auto c = w->addPath("content-C", "c", {});
    w->waitForAllPaths();
    EXPECT_TRUE(localStore->isValidPath(a));
    EXPECT_TRUE(localStore->isValidPath(c));

    deletePath(root2);
}

/* LD-D2 (reference fidelity) — `addMultipleToStore` ingests the batch
   reference-ordered (`processGraph`), so an object B that references an
   in-batch object A registers with A wired as a reference, by StorePath (not
   submission index). */
TEST_F(AsyncPathWriterTest, BulkFlushResolvesIntraBatchReferences)
{
    auto w = AsyncPathWriter::make(localStore);
    auto a = w->addPath("ref-target", "a", {});
    auto b = w->addPath("ref-source", "b", {a}); // B references A, same batch
    w->waitForAllPaths();
    EXPECT_TRUE(localStore->isValidPath(a));
    EXPECT_TRUE(localStore->isValidPath(b));
    auto refs = localStore->queryPathInfo(b)->references;
    EXPECT_TRUE(refs.count(a)) << "intra-batch reference A was not wired into B";
}

/* LD-P5 / LD-D1 — idempotent materialisation (join-semilattice): re-flushing a
   valid path is a no-op; store membership grows monotonically. */
TEST_F(AsyncPathWriterTest, ReflushIsNoOp)
{
    auto cs = make_ref<CountingStore>(localStore);
    auto w = AsyncPathWriter::make(cs);
    auto p = w->addPath("p5", "p5", {});
    w->waitForAllPaths();
    EXPECT_TRUE(localStore->isValidPath(p));
    auto writes = cs->pathsWritten.load();
    EXPECT_EQ(writes, 1u);

    w->waitForAllPaths();    // nothing pending → no write
    w->waitForPath(p);       // already written → no write
    EXPECT_EQ(cs->pathsWritten.load(), writes);
    EXPECT_TRUE(localStore->isValidPath(p)); // still valid (monotone)
}

/* LD-P6 — enqueue dedup: two enqueues of one content-addressed path collapse to
   a single write. Contrasted with the control below (two distinct contents ⇒
   two writes), which proves the counter — and that the dedup is what makes it
   one, not the counter undercounting. */
TEST_F(AsyncPathWriterTest, DuplicateEnqueueWritesOnce)
{
    auto cs = make_ref<CountingStore>(localStore);
    auto w = AsyncPathWriter::make(cs);
    auto p1 = w->addPath("dup6", "dup6", {});
    auto p2 = w->addPath("dup6", "dup6", {}); // identical ⇒ same path ⇒ deduped
    EXPECT_EQ(p1, p2);
    w->waitForAllPaths();
    EXPECT_EQ(cs->pathsWritten.load(), 1u) << "duplicate enqueue must write exactly once";
}

TEST_F(AsyncPathWriterTest, DistinctEnqueuesWriteEach) // negative control for LD-P6
{
    auto cs = make_ref<CountingStore>(localStore);
    auto w = AsyncPathWriter::make(cs);
    w->addPath("d6a", "d6a", {});
    w->addPath("d6b", "d6b", {}); // different ⇒ NOT deduped
    w->waitForAllPaths();
    EXPECT_EQ(cs->pathsWritten.load(), 2u);
}

/* LD-D3 — associativity + cost: grouping changes the round-trip COUNT, not the
   final store. {A,B} then {C} = two bulk submissions; {A,B,C} = one; both leave
   all three valid. (Makes §7's perf claim falsifiable, distinct from
   confluence.) */
TEST_F(AsyncPathWriterTest, BatchGroupingAssociativeAndCost)
{
    auto root2 = createTempDir();
    auto store2 = openStore(fmt("local?root=%s", root2.string()));

    auto cs1 = make_ref<CountingStore>(localStore); // grouping {A,B},{C}
    auto w1 = AsyncPathWriter::make(cs1);
    w1->addPath("gA", "gA", {});
    w1->addPath("gB", "gB", {});
    w1->waitForAllPaths();
    auto c1 = w1->addPath("gC", "gC", {});
    w1->waitForAllPaths();

    auto cs2 = make_ref<CountingStore>(store2); // grouping {A,B,C}
    auto w2 = AsyncPathWriter::make(cs2);
    w2->addPath("gA", "gA", {});
    w2->addPath("gB", "gB", {});
    auto c2 = w2->addPath("gC", "gC", {});
    w2->waitForAllPaths();

    EXPECT_EQ(c1, c2);
    EXPECT_TRUE(localStore->isValidPath(c1)); // same result
    EXPECT_TRUE(store2->isValidPath(c2));
    EXPECT_GT(cs1->bulkWrites.load(), cs2->bulkWrites.load()); // but different cost (2 > 1)

    deletePath(root2);
}

/* LD-C1 — black-hole before encode + LD-D5 cross-process re-materialisation is
   free: a path already valid in the store (e.g. written by a prior "process")
   is dropped from the queue without being re-written, even when re-enqueued by
   a fresh writer (content-addressing ⇒ identical path). */
TEST_F(AsyncPathWriterTest, AlreadyValidIsNotRewrittenAcrossWriters)
{
    auto cs = make_ref<CountingStore>(localStore);
    {
        auto w1 = AsyncPathWriter::make(cs);
        auto p = w1->addPath("c1d5", "c1d5", {});
        w1->waitForAllPaths();
        EXPECT_TRUE(localStore->isValidPath(p));
    }
    auto writes = cs->pathsWritten.load();

    auto w2 = AsyncPathWriter::make(cs); // a fresh "process"
    auto p2 = w2->addPath("c1d5", "c1d5", {}); // re-mints the identical path
    w2->waitForAllPaths();                       // already valid ⇒ no re-write
    EXPECT_EQ(cs->pathsWritten.load(), writes);
    EXPECT_TRUE(localStore->isValidPath(p2));
}

/* LD-C3 — hash-consing of independently-equal objects: two *separately*
   constructed equal payloads reach one content-addressed path and then write
   once (maximal sharing). */
TEST_F(AsyncPathWriterTest, StructurallyEqualShareOnePath)
{
    auto cs = make_ref<CountingStore>(localStore);
    auto w = AsyncPathWriter::make(cs);
    std::string a = "c3", b = "c3"; // built independently, structurally equal
    auto pa = w->addPath(std::string(a), "c3", {});
    auto pb = w->addPath(std::string(b), "c3", {});
    EXPECT_EQ(pa, pb);
    w->waitForAllPaths();
    EXPECT_EQ(cs->pathsWritten.load(), 1u);
}

/* LD-P8 — reference-set fidelity through deferral: a deferred `.drv`'s `addPath`
   references are registered as the path's store references (so GC/closure see
   them). The `.drv`-level mapping (references = inputSrcs ∪ inputDrv keys) is
   exercised at the libexpr level (lazy-derivation-defer.cc) over a real
   evaluated derivation; here we verify the queue preserves whatever references
   were enqueued — eager vs deferred agree. */
TEST_F(AsyncPathWriterTest, DeferredReferencesMatchEager)
{
    /* A real, valid content-addressed object to reference. */
    StringSource src{std::string("p8-source-contents")};
    auto refPath = localStore->addToStoreFromDump(
        src, "p8-ref", FileSerialisationMethod::Flat, ContentAddressMethod::Raw::Text, HashAlgorithm::SHA256, {},
        NoRepair);

    auto w = AsyncPathWriter::make(localStore);
    auto p = w->addPath("p8-referrer", "p8-referrer", {refPath});
    EXPECT_FALSE(localStore->isValidPath(p)); // deferred
    w->waitForAllPaths();
    EXPECT_TRUE(localStore->isValidPath(p));

    auto refs = localStore->queryPathInfo(p)->references;
    EXPECT_TRUE(refs.count(refPath)) << "deferred object lost its reference edge (LD-P8)";
}

/* LD-P7 — `computeP` (the ATerm `unparse`) is done ONCE by the caller and the
   queue REUSES those bytes verbatim; only `encodeO` (the store write) is
   deferred — never a recompute. The queue holds no `Derivation` and has no
   `unparse` capability, so it *structurally* cannot re-derive the bytes; this
   characterises that: the held bytes equal what was enqueued, the flushed object
   is content-addressed by exactly their hash (same hash ⇒ same bytes ⇒ no
   divergence at flush), and the bytes are written exactly once. A cost property —
   the proposal notes there is no clean negative control. */
TEST_F(AsyncPathWriterTest, UnparseBytesComputedOnceAndReused)
{
    auto cs = make_ref<CountingStore>(localStore);
    auto w = AsyncPathWriter::make(cs);
    std::string contents = "p7-aterm-bytes-stand-in";
    auto p = w->addPath(contents, "p7", {});

    auto pend = w->lookupPending(p);
    ASSERT_TRUE(pend);
    EXPECT_EQ(pend->contents, contents) << "the queue must hold the once-computed bytes verbatim (no recompute)";

    w->waitForAllPaths();
    EXPECT_TRUE(localStore->isValidPath(p));
    auto info = localStore->queryPathInfo(p);
    ASSERT_TRUE(info->ca);
    EXPECT_EQ(info->ca->hash, hashString(HashAlgorithm::SHA256, contents))
        << "flushed object's content-address must be the hash of the enqueued bytes (encodeO of the once-computed contents)";
    EXPECT_EQ(cs->pathsWritten.load(), 1u) << "the bytes are written exactly once";
}

/* ---------------- Increment 6: deferred sources (source-copy elision) -------- */

/* A deferred source names its content-addressed path up front (from the
   narHash — the eval-time hash walk is unavoidable) and is queryable as pending
   WITHOUT being copied: the queue holds only the copy thunk, not the bytes. The
   named path is exactly the one the eager copy would produce (LD-P8). */
TEST_F(AsyncPathWriterTest, AddSourceNamesPathAndIsPending)
{
    auto w = AsyncPathWriter::make(localStore);
    std::string content = "src-naming";
    auto h = hashString(HashAlgorithm::SHA256, content);
    auto expected = localStore->makeFixedOutputPathFromCA(
        "srcnm", ContentAddressWithReferences::fromParts(ContentAddressMethod::Raw::Text, h, {}));

    bool fired = false;
    auto sp = w->addSource("srcnm", h, ContentAddressMethod::Raw::Text, {}, [&] { fired = true; });

    EXPECT_EQ(sp, expected) << "addSource must name the same CA path the eager copy would";
    EXPECT_TRUE(w->isSourcePending(sp));
    EXPECT_FALSE(localStore->isValidPath(sp)) << "registration must not copy the source";
    EXPECT_FALSE(fired) << "registration must not run the copy thunk";
    auto info = w->lookupSource(sp);
    ASSERT_TRUE(info);
    EXPECT_EQ(info->path, sp);
}

/* Increment 6 (positive) — a deferred source riding a referrer's referential
   closure is copied when that referrer is materialised: the copy thunk fires
   (reference-first), so the referrer registers with the source already valid.
   This is the build-from-source path (the referrer stands in for a `.drv`). */
TEST_F(AsyncPathWriterTest, DeferredSourceCopiedWhenReferrerMaterialised)
{
    auto w = AsyncPathWriter::make(localStore);
    std::string content = "src-copied";
    auto h = hashString(HashAlgorithm::SHA256, content);

    bool fired = false;
    auto sp = w->addSource("srccp", h, ContentAddressMethod::Raw::Text, {}, [&] {
        fired = true;
        StringSource s{content};
        localStore->addToStoreFromDump(
            s, "srccp", FileSerialisationMethod::Flat, ContentAddressMethod::Raw::Text, HashAlgorithm::SHA256, {},
            NoRepair);
    });

    auto referrer = w->addPath("referrer-bytes", "referrer", {sp}); // references the source
    EXPECT_FALSE(localStore->isValidPath(sp));
    EXPECT_FALSE(localStore->isValidPath(referrer));

    w->waitForPath(referrer); // demand the referrer ⇒ materialise its source closure too

    EXPECT_TRUE(fired) << "source copy thunk must fire as part of the referrer's closure";
    EXPECT_TRUE(localStore->isValidPath(sp)) << "source must be copied (referential integrity)";
    EXPECT_TRUE(localStore->isValidPath(referrer));
    EXPECT_FALSE(w->isSourcePending(sp)) << "a materialised source leaves the queue";
    auto refs = localStore->queryPathInfo(referrer)->references;
    EXPECT_TRUE(refs.count(sp)) << "referrer must register with the source as a reference";
}

/* Increment 6 (negative / elision — LD-S2 for sources) — a deferred source
   whose referrer is NEVER demanded is dropped when the queue is destroyed: the
   copy thunk never fires and the source is never written. This is the
   substitutable-target path (the referrer `.drv` is elided, so its source is
   too). */
TEST_F(AsyncPathWriterTest, UndemandedDeferredSourceIsElided)
{
    std::string content = "src-elided";
    auto h = hashString(HashAlgorithm::SHA256, content);
    std::optional<StorePath> sp;
    bool fired = false;
    {
        auto w = AsyncPathWriter::make(localStore);
        sp = w->addSource("srcel", h, ContentAddressMethod::Raw::Text, {}, [&] { fired = true; });
        w->addPath("ref-bytes", "ref-el", {*sp}); // referrer enqueued but never demanded
        // queue destroyed here without any waitForPath / waitForAllPaths
    }
    EXPECT_FALSE(fired) << "an un-demanded source's copy thunk must never run (elision)";
    EXPECT_FALSE(localStore->isValidPath(*sp)) << "an un-demanded source must not be written";
}

/* Increment 6 (black-hole — LD-C1 for sources) — a deferred source already valid
   in the store is dropped without re-running its copy thunk (the
   content-addressing black-hole applies to sources too). */
TEST_F(AsyncPathWriterTest, AlreadyValidDeferredSourceNotReCopied)
{
    std::string content = "src-blackhole";
    /* Pre-populate the store with the source object. */
    StringSource pre{content};
    auto sp0 = localStore->addToStoreFromDump(
        pre, "srcbh", FileSerialisationMethod::Flat, ContentAddressMethod::Raw::Text, HashAlgorithm::SHA256, {},
        NoRepair);
    ASSERT_TRUE(localStore->isValidPath(sp0));
    auto h = hashString(HashAlgorithm::SHA256, content);

    auto w = AsyncPathWriter::make(localStore);
    bool fired = false;
    auto sp = w->addSource("srcbh", h, ContentAddressMethod::Raw::Text, {}, [&] { fired = true; });
    EXPECT_EQ(sp, sp0) << "same content ⇒ same CA path";

    auto referrer = w->addPath("bh-ref", "bh-ref", {sp});
    w->waitForPath(referrer);

    EXPECT_FALSE(fired) << "an already-valid source must be black-holed (copy thunk not re-run)";
    EXPECT_TRUE(localStore->isValidPath(referrer));
}

/* ---------------- async overlap: the background drain (§7) ---------------- */

/* The headline overlap property: with the background drain started, an enqueued
   `.drv` becomes valid in the store WITHOUT any explicit `waitForPath`/
   `waitForAllPaths` barrier — the writes run concurrently with "eval" (here,
   with the test thread). Polls (bounded) for the proactive write. */
TEST_F(AsyncPathWriterTest, BackgroundDrainWritesBeforeBarrier)
{
    auto w = AsyncPathWriter::make(localStore);
    w->startBackgroundDrain();

    auto p = w->addPath("bg-overlap", "bg-overlap", {});
    EXPECT_TRUE(w->isPending(p)); // enqueued

    bool became = false;
    for (int i = 0; i < 500 && !became; ++i) {
        if (localStore->isValidPath(p)) {
            became = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(became) << "background drain did not write the path without an explicit barrier (no overlap)";

    w->waitForAllPaths(); // clean shutdown (stops + joins the drain thread)
}

/* Idempotent with the barrier: starting the drain, enqueuing, then
   `waitForAllPaths` leaves everything valid and the thread stopped — the bulk
   barrier still finalises whatever the drain hadn't reached. */
TEST_F(AsyncPathWriterTest, BackgroundDrainFinalisesAtBarrier)
{
    auto w = AsyncPathWriter::make(localStore);
    w->startBackgroundDrain();
    auto a = w->addPath("bg-a", "bg-a", {});
    auto b = w->addPath("bg-b", "bg-b", {});
    w->waitForAllPaths();
    EXPECT_TRUE(localStore->isValidPath(a));
    EXPECT_TRUE(localStore->isValidPath(b));
    EXPECT_FALSE(w->isPending(a));
    EXPECT_FALSE(w->isPending(b));
}

/* LD-X1/LD-V4 through the background path: a write that fails in the drain
   thread latches and is re-thrown to the barrier observer. `store` is the
   dummy store (rejects `addToStoreFromDump` on a `.drv` name). */
TEST_F(LibStoreTest, BackgroundDrainFailureLatches)
{
    auto writer = AsyncPathWriter::make(store);
    writer->startBackgroundDrain();
    writer->addPath("(bg-content)", "bg-fail.drv", {});
    /* `waitForAllPaths` joins the drain thread (so the failure is latched) and
       re-throws it; a subsequent observer still sees the latched error. */
    EXPECT_THROW(writer->waitForAllPaths(), Error);
    EXPECT_THROW(writer->waitForPath(writer->addPath("(x)", "bg-fail2.drv", {})), Error);
}

} // namespace nix
