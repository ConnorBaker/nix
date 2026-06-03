#include "nix/store/async-path-writer.hh"
#include "nix/store/derivations.hh"
#include "nix/store/local-store.hh"
#include "nix/util/archive.hh"
#include "nix/util/serialise.hh"
#include "nix/util/sync.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/callback.hh"

#include <condition_variable>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace nix {

struct AsyncPathWriterImpl : AsyncPathWriter
{
    ref<Store> store;

    struct Item
    {
        StorePath storePath;
        std::string contents;
        std::string name;
        Hash hash;
        StorePathSet references;
        RepairFlag repair;
    };

    /* Pending (enqueued, not-yet-written) objects, keyed by their
       content-addressed path. Retained until materialised or until the queue
       is destroyed (the call-by-need GC of un-forced thunks = elision). */
    Sync<std::map<StorePath, Item>> pending_;

    /* A deferred *source* copy (Increment 6). The bytes are NOT retained
       (a source tree can be huge); instead we hold its already-known
       `ValidPathInfo` (CA path, narHash, references — enough to answer the
       virtual store's `queryPathInfo`) plus `copy`, an idempotent thunk that
       walks-and-copies the bytes into the store on demand
       (`MaterialisationScheduler::outPathOf`). A source is demanded only as a
       reference of a `.drv` being materialised; an un-demanded source is
       dropped at destruction → source-copy elision (LD-S2 for sources). */
    struct SourceItem
    {
        ValidPathInfo info;
        std::function<void()> copy;
    };

    /* Keyed by content-addressed path, like `pending_`. Disjoint from it
       (sources are not `.drv`s). */
    Sync<std::map<StorePath, SourceItem>> pendingSources_;

    /* Serialises the body of `materialise()` across threads (the eval thread's
       flush-on-read, the bulk barrier, and the optional background drain), so
       two threads never collect+write the same pending entry concurrently. */
    std::mutex writeMutex_;

    /* Optional background-drain thread (`startBackgroundDrain`). All of these
       are guarded by `drainMutex_`. */
    std::thread drainThread_;
    std::mutex drainMutex_;
    std::condition_variable drainCv_;
    bool drainStarted_ = false;
    bool drainStop_ = false;
    bool drainDirty_ = false;          // something was enqueued since the last drain
    std::exception_ptr drainError_;    // latched background-write failure (LD-X1)

    /* Computed once: is the backing store an in-process LocalStore? Decides
       three things, all hinging on whether `isValidPath`/`addToStoreFromDump`
       are cheap local ops or per-`.drv` daemon round-trips:
         - addPath black-holes already-valid paths at ENQUEUE (warm re-eval);
         - writePaths flushes per-item via `addToStoreFromDump` vs the one framed
           `addMultipleToStore`;
         - startBackgroundDrain overlaps only locally (remote would fragment the
           one bulk op into per-wave round-trips). */
    const bool storeIsLocal;

    AsyncPathWriterImpl(ref<Store> store)
        : store(store)
        , storeIsLocal(dynamic_cast<LocalStore *>(&*store) != nullptr)
    {
    }

    ~AsyncPathWriterImpl() override
    {
        /* Stop+join the background thread before any member is destroyed (a
           still-joinable `std::thread` dtor would `terminate`). Whatever
           remains pending is then discarded with the maps — the elision
           semantics are unchanged for the never-drained (build) path. */
        stopDrainThread();
    }

    StorePath
    addPath(std::string contents, std::string name, StorePathSet references, RepairFlag repair) override
    {
        auto hash = hashString(HashAlgorithm::SHA256, contents);
        auto storePath = store->makeFixedOutputPathFromCA(
            name,
            TextInfo{
                .hash = hash,
                .references = references,
            });

        /* Warm-path black-hole at ENQUEUE (LocalStore only): if the `.drv` is
           already valid, don't enqueue it — a warm re-eval then pays nothing
           here and nothing at the flush (no graph walk, no map churn), matching
           eager's inline `isValidPath` bail. Gated to LocalStore because there
           `isValidPath` is a cheap cached lookup (eager does the same); on a
           remote store it would be a per-`.drv` round-trip during eval — exactly
           the IO the deferral exists to avoid — so there we keep deferring the
           check to the one bulk flush (materialise still black-holes).

           NB: when the background drain runs (eval-to-populate), this per-`.drv`
           `isValidPath` read serialises against the drain's concurrent
           `addToStoreFromDump` writes on the shared store connection — ~1.7s of
           contention on a 16k-`.drv` cold eval. Skipping it while draining
           recovers that on cold but re-enqueues every already-valid `.drv` on a
           WARM re-eval (drain no-ops the write, but the enqueue churn made warm
           ~9% SLOWER than eager — violating "lazy ≤ eager"), so we keep the
           black-hole unconditionally. Removing the contention without the warm
           cost needs the drain to write on its own store connection (TODO). */
        if (storeIsLocal && store->isValidPath(storePath))
            return storePath;

        {
            auto pending(pending_.lock());
            /* Dedup by the content-determined path (LD-P6): two enqueues of one
               path collapse to a single retained entry / single write. */
            pending->try_emplace(
                storePath,
                Item{
                    .storePath = storePath,
                    .contents = std::move(contents),
                    .name = std::move(name),
                    .hash = hash,
                    .references = std::move(references),
                    .repair = repair,
                });
        }
        signalDirty(); // wake the background drain, if running
        return storePath;
    }

    std::optional<Pending> lookupPending(const StorePath & path) override
    {
        auto pending(pending_.lock());
        auto i = pending->find(path);
        if (i == pending->end())
            return std::nullopt;
        return Pending{i->second.contents, i->second.references};
    }

    bool isPending(const StorePath & path) override
    {
        return pending_.lock()->contains(path);
    }

    StorePath addSource(
        std::string name,
        Hash narHash,
        ContentAddressMethod method,
        StoreReferences references,
        std::function<void()> copy) override
    {
        /* Derive the same content-addressed path the eager `outPathOf` would
           (makeFixedOutputPathFromCA over the same name/method/narHash/refs),
           so the deferred and the materialised paths are byte-identical
           (LD-P8). `makeFromCA` fills CA + references; the bytes/size come
           later, when `copy` runs. */
        auto info = ValidPathInfo::makeFromCA(
            *store, name, ContentAddressWithReferences::fromParts(method, narHash, std::move(references)), narHash);
        auto storePath = info.path;

        {
            auto sources(pendingSources_.lock());
            /* Dedup by the content-determined path (LD-P6). */
            sources->try_emplace(storePath, SourceItem{.info = std::move(info), .copy = std::move(copy)});
        }
        signalDirty(); // wake the background drain, if running
        return storePath;
    }

    bool isSourcePending(const StorePath & path) override
    {
        return pendingSources_.lock()->contains(path);
    }

    std::optional<ValidPathInfo> lookupSource(const StorePath & path) override
    {
        auto sources(pendingSources_.lock());
        auto i = sources->find(path);
        if (i == sources->end())
            return std::nullopt;
        return i->second.info;
    }

    void waitForPath(const StorePath & path) override
    {
        rethrowDrainError(); // a failed background drain latches to every observer (LD-X1)
        materialise({path});
    }

    void waitForPaths(const StorePathSet & paths) override
    {
        rethrowDrainError();
        /* One batched materialise → one framed `addMultipleToStore` over a
           daemon (vs one round-trip per path if a caller looped `waitForPath`).
           `materialise` already pulls in each root's transitive pending
           references and is serialised against the background drain. */
        materialise(paths);
    }

    void waitForAllPaths() override
    {
        /* Stop+join the background drain (if running) so the final bulk
           materialise below is the sole writer; then flush whatever remains. */
        stopDrainThread();
        rethrowDrainError();
        materialise(collectAllPending());
    }

    /* Every currently-pending path (`.drv`s + deferred sources). */
    StorePathSet collectAllPending()
    {
        StorePathSet all;
        {
            auto pending(pending_.lock());
            for (auto & [p, _] : *pending)
                all.insert(p);
        }
        {
            /* Seed pending sources too. In practice every pending source is an
               `inputSrc` of some pending `.drv` (registered together in
               `derivationStrict`), so the `.drv` roots already reach them
               transitively; seeding them directly makes a bulk-observation
               boundary flush an orphan source as well. */
            auto sources(pendingSources_.lock());
            for (auto & [p, _] : *sources)
                all.insert(p);
        }
        return all;
    }

    /* ---- optional background drain (async overlap, §7) ---- */

    void startBackgroundDrain() override
    {
        /* Overlap only helps a LocalStore, where the flush is per-item
           `addToStoreFromDump` (no wire batch) and draining during eval simply
           interleaves the writes with eval CPU. For a REMOTE store the flush is
           a single framed `addMultipleToStore`; draining during eval fragments
           that into many dependency-ordered waves, each a daemon round-trip —
           MEASURED slower than eager on a deep nixpkgs DAG (15 pkgs over a
           daemon: lazy-noov 1.36x vs lazy+overlap 0.86x). So for a remote store
           the drain is a NO-OP: `waitForAllPaths` does the one bulk flush, which
           is the round-trip-coalescing win. */
        if (!storeIsLocal)
            return;
        std::lock_guard<std::mutex> lk(drainMutex_);
        if (drainStarted_)
            return;
        drainStarted_ = true;
        drainThread_ = std::thread([this] { drainLoop(); });
    }

    void signalDirty()
    {
        {
            std::lock_guard<std::mutex> lk(drainMutex_);
            if (!drainStarted_)
                return;
            drainDirty_ = true;
        }
        drainCv_.notify_one();
    }

    void drainLoop()
    {
        while (true) {
            {
                std::unique_lock<std::mutex> lk(drainMutex_);
                drainCv_.wait(lk, [this] { return drainStop_ || drainDirty_; });
                if (drainStop_)
                    return; // the final flush is done by waitForAllPaths
                drainDirty_ = false;
            }
            try {
                /* Write whatever is pending right now; entries enqueued during
                   the write are picked up by the next iteration (signalled via
                   `drainDirty_`). This is what overlaps the writes with eval. */
                materialise(collectAllPending());
            } catch (...) {
                std::lock_guard<std::mutex> lk(drainMutex_);
                drainError_ = std::current_exception(); // latch (LD-X1/V4)
                drainStop_ = true;
                return;
            }
        }
    }

    void stopDrainThread()
    {
        {
            std::lock_guard<std::mutex> lk(drainMutex_);
            drainStop_ = true;
        }
        drainCv_.notify_one();
        if (drainThread_.joinable())
            drainThread_.join();
    }

    void rethrowDrainError()
    {
        std::lock_guard<std::mutex> lk(drainMutex_);
        if (drainError_)
            std::rethrow_exception(drainError_);
    }

    /* Write `roots` and their transitive *pending* references to the store, so
       registration is referentially closed (a `.drv`'s `inputDrvs` must be
       valid when it registers — confirmed by the export/import referential
       check). On success the written entries leave the queue; on failure they
       stay, so a later demand re-attempts (and re-throws — failure latching,
       LD-X1). */
    void materialise(const StorePathSet & roots)
    {
        /* Serialise writers: the background drain and an eval-thread
           flush-on-read (or the bulk barrier) must not collect+write the same
           pending entry concurrently. */
        std::lock_guard<std::mutex> wlk(writeMutex_);

        std::vector<Item> toWrite;
        /* Deferred sources reached by the walk (storePath + its copy thunk),
           in reference-first order. Copied before the `.drv`s that reference
           them so registration is referentially closed. */
        std::vector<std::pair<StorePath, std::function<void()>>> srcToCopy;
        StorePathSet seen;
        {
            auto pending(pending_.lock());
            auto sources(pendingSources_.lock());
            std::function<void(const StorePath &)> visit = [&](const StorePath & p) {
                if (!seen.insert(p).second)
                    return;
                /* A deferred source (Increment 6): schedule its copy thunk
                   (its own references first), then it leaves the queue. */
                if (auto si = sources->find(p); si != sources->end()) {
                    if (store->isValidPath(p)) { // black-hole (LD-C1/LD-D5)
                        sources->erase(si);
                        return;
                    }
                    for (auto & r : si->second.info.references)
                        visit(r); // references first
                    srcToCopy.emplace_back(p, si->second.copy);
                    return;
                }
                auto i = pending->find(p);
                if (i == pending->end())
                    return; // not pending: already written or an external path
                /* Black-hole BEFORE encode (LD-C1): if the path is already valid
                   in the store — written by a prior flush or a prior process
                   (content-addressing ⇒ same bytes; LD-D5) — drop it from the
                   queue without re-encoding/re-writing it. */
                if (store->isValidPath(p)) {
                    pending->erase(i);
                    return;
                }
                for (auto & r : i->second.references)
                    visit(r); // references first
                toWrite.push_back(i->second); // copy (Items are copyable)
            };
            for (auto & r : roots)
                visit(r);
        }

        if (toWrite.empty() && srcToCopy.empty())
            return;

        /* Copy deferred sources into the store FIRST: they are references of
           the `.drv`s we are about to register, so referential integrity
           requires them valid before `addMultipleToStore`. The thunk
           (`outPathOf`) walks+ingests and is idempotent (skips if already
           valid). Run outside the queue locks — it does real I/O. */
        for (auto & [p, copy] : srcToCopy)
            copy();

        if (!toWrite.empty())
            writePaths(toWrite);

        {
            auto pending(pending_.lock());
            for (auto & item : toWrite)
                pending->erase(item.storePath);
        }
        {
            auto sources(pendingSources_.lock());
            for (auto & [p, _] : srcToCopy)
                sources->erase(p);
        }
    }

    void writePaths(const std::vector<Item> & items)
    {
        /* Pick the flush mechanism by store kind. `items` is reference-ordered
           (materialise pushes references before referents), so both paths
           register a referent after its references (referential integrity).

           A LocalStore (in-process) writes each `.drv` with the SAME cheap flat
           `addToStoreFromDump` the eager `writeDerivation` uses. Routing it
           through `addMultipleToStore` instead pushes every `.drv` through the
           heavyweight NAR-restore + CA-re-verify + `processGraph` path —
           measured ~47% slower per item — for NO benefit: there is no wire to
           batch, so the bulk op only adds overhead (this is what made the local
           eval-store case regress vs eager; PROPOSAL-LAZY-DERIVATIONS §7's
           "neutral" prediction). */
        if (storeIsLocal) {
            for (auto & item : items) {
                StringSource s{item.contents};
                auto p2 = store->addToStoreFromDump(
                    s,
                    item.name,
                    FileSerialisationMethod::Flat,
                    ContentAddressMethod::Raw::Text,
                    HashAlgorithm::SHA256,
                    item.references,
                    item.repair);
                assert(p2 == item.storePath);
            }
            return;
        }

        /* A remote store (daemon / SSH / binary cache) DOES benefit from
           batching: `addMultipleToStore` carries the whole batch in ONE framed
           op (the §4.3 daemon round-trip win) and `processGraph` ingests it
           reference-ordered in parallel. */
        Store::PathsSource sources;
        sources.reserve(items.size());
        RepairFlag repair = NoRepair;

        for (auto & item : items) {
            /* `addMultipleToStore` ingests and re-verifies a NAR, so it needs
               the NAR hash/size. A `.drv` is a flat `text:sha256` object whose
               NAR is `dumpString(contents)`; computing this hash is the *same*
               pass `addToStoreFromDump` already does for a flat object, so it
               is free, not a second hash (PROPOSAL-LAZY-DERIVATIONS.md §4.3). */
            HashSink narSink{HashAlgorithm::SHA256};
            dumpString(item.contents, narSink);
            auto narHash = narSink.finish();

            ValidPathInfo info{item.storePath, UnkeyedValidPathInfo{*store, narHash.hash}};
            info.narSize = narHash.numBytesDigested;
            info.references = item.references;
            info.ca = ContentAddress{
                .method = ContentAddressMethod::Raw::Text,
                .hash = item.hash,
            };
            if (item.repair)
                repair = item.repair;

            store->addTempRoot(item.storePath);

            sources.push_back(
                {std::move(info), sinkToSource([contents = item.contents](Sink & sink) { dumpString(contents, sink); })});
        }

        Activity act(*logger, lvlDebug, actUnknown, fmt("writing %d derivations to the store", items.size()));

        /* One framed `AddMultipleToStore` op carries the whole batch to a
           daemon; `processGraph` ingests it reference-ordered and in parallel
           (N reference-ordered single-path transactions, not one atomic txn —
           Finding C3). */
        store->addMultipleToStore(std::move(sources), act, repair);
    }
};

ref<AsyncPathWriter> AsyncPathWriter::make(ref<Store> store)
{
    return make_ref<AsyncPathWriterImpl>(store);
}

/* ------------------------------------------------------------------------ */

/**
 * Store decorator overlaying an `AsyncPathWriter`'s pending `.drv`s on a
 * backing store: a pending path reads back as valid, with its bytes / info
 * synthesised from the queue, without being written. Everything else delegates
 * to `next`. Used as the build Worker's *eval store*, so a `.drv` can be read
 * (to substitute its output) without being materialised (LD-S6).
 */
struct LazyDrvStore : Store
{
    ref<Store> next;
    ref<AsyncPathWriter> writer;

    LazyDrvStore(ref<Store> next, ref<AsyncPathWriter> writer)
        : Store(next->config)
        , next(next)
        , writer(writer)
    {
    }

    void anchor() override {}

    /* --- the overlay: pending `.drv`s read back as valid --- */

    bool isValidPathUncached(const StorePath & path) override
    {
        return writer->isPending(path) || writer->isSourcePending(path) || next->isValidPath(path);
    }

    void queryPathInfoUncached(
        const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept override
    {
        /* A deferred source (Increment 6): its CA/narHash/references are known
           at registration, so answer without copying — `queryMissing` can
           classify the `.drv` that references it as buildable (LD-S9) and the
           build reads the real bytes from `next` after `materialiseDeferred`
           runs the copy thunk. */
        if (auto info = writer->lookupSource(path)) {
            callback(std::make_shared<ValidPathInfo>(std::move(*info)));
            return;
        }
        if (auto p = writer->lookupPending(path)) {
            try {
                HashSink narSink{HashAlgorithm::SHA256};
                dumpString(p->contents, narSink);
                auto narHash = narSink.finish();
                auto info = std::make_shared<ValidPathInfo>(path, UnkeyedValidPathInfo{*this, narHash.hash});
                info->narSize = narHash.numBytesDigested;
                info->references = p->references;
                info->ca = ContentAddress{
                    .method = ContentAddressMethod::Raw::Text,
                    .hash = hashString(HashAlgorithm::SHA256, p->contents),
                };
                callback(std::move(info));
            } catch (...) {
                callback.rethrow();
            }
            return;
        }
        try {
            callback(std::make_shared<ValidPathInfo>(*next->queryPathInfo(path)));
        } catch (InvalidPath &) {
            callback(nullptr);
        } catch (...) {
            callback.rethrow();
        }
    }

    Derivation readDerivation(const StorePath & drvPath) override
    {
        if (auto p = writer->lookupPending(drvPath))
            return parseDerivation(*this, std::move(p->contents), Derivation::nameFromPath(drvPath));
        return next->readDerivation(drvPath);
    }

    Derivation readInvalidDerivation(const StorePath & drvPath) override
    {
        if (auto p = writer->lookupPending(drvPath))
            return parseDerivation(*this, std::move(p->contents), Derivation::nameFromPath(drvPath));
        return next->readInvalidDerivation(drvPath);
    }

    void narFromPath(const StorePath & path, Sink & sink) override
    {
        /* Serve a pending object's NAR from the queue without touching `next`.
           This is what lets `RemoteStore::copyDrvsFromEvalStore` ship a
           deferred `.drv` *closure* to a daemon before it builds (LD-S6,
           remote-daemon regime): `copyClosure(evalStore, daemon, …)` reads
           each pending `.drv` here. A `.drv` is a flat `text:sha256` object
           whose NAR is exactly `dumpString(contents)` (the same bytes the
           queue holds and that `writePaths` hashes) — so this needs no
           round-trip, which also avoids the connection deadlock the base
           `narFromPath` (→ `requireStoreObjectAccessor` → read through `next`)
           would hit on a path the daemon does not have. */
        if (auto p = writer->lookupPending(path)) {
            dumpString(p->contents, sink);
            return;
        }
        /* A pending deferred source has no bytes in the queue (only a copy
           thunk), so materialise it into the backing store first, then serve
           it from there. */
        if (writer->isSourcePending(path))
            writer->waitForPath(path);
        next->narFromPath(path, sink);
    }

    void addTempRoot(const StorePath & path) override
    {
        if (!writer->isPending(path))
            next->addTempRoot(path);
    }

    void materialiseDeferred(const StorePath & path) override
    {
        /* The build committed to building this derivation, so its `.drv` must
           be a real store object (doc C1); write it + its pending closure. */
        writer->waitForPath(path);
    }

    /* --- everything else delegates to `next` --- */

    void queryRealisationUncached(
        const DrvOutput & id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept override
    {
        next->queryRealisation(id, std::move(callback));
    }

    std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override
    {
        return next->queryPathFromHashPart(hashPart);
    }

    void addToStore(const ValidPathInfo & info, Source & source, RepairFlag repair, CheckSigsFlag checkSigs) override
    {
        next->addToStore(info, source, repair, checkSigs);
    }

    StorePath addToStoreFromDump(
        Source & dump,
        std::string_view name,
        FileSerialisationMethod dumpMethod,
        ContentAddressMethod hashMethod,
        HashAlgorithm hashAlgo,
        const StorePathSet & references,
        RepairFlag repair) override
    {
        return next->addToStoreFromDump(dump, name, dumpMethod, hashMethod, hashAlgo, references, repair);
    }

    void registerDrvOutput(const Realisation & output) override
    {
        next->registerDrvOutput(output);
    }

    ref<SourceAccessor> getFSAccessor(bool requireValidPath) override
    {
        return next->getFSAccessor(requireValidPath);
    }

    std::shared_ptr<SourceAccessor> getFSAccessor(const StorePath & path, bool requireValidPath) override
    {
        return next->getFSAccessor(path, requireValidPath);
    }

    std::optional<TrustedFlag> isTrustedClient() override
    {
        return next->isTrustedClient();
    }
};

ref<Store> makeLazyDrvStore(ref<Store> next, ref<AsyncPathWriter> writer)
{
    return make_ref<LazyDrvStore>(next, writer);
}

} // namespace nix
