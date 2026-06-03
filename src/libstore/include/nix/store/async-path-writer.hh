#pragma once
///@file

#include "nix/store/store-api.hh"

#include <functional>
#include <optional>

namespace nix {

/**
 * A lazy write-back queue for small content-addressed store objects (`.drv`
 * files). It is the "thunk heap" of the lazy-derivations design
 * (PROPOSAL-LAZY-DERIVATIONS.md §0.5/§4): `addPath` computes the resulting
 * `StorePath` synchronously (a cheap hash over the in-memory bytes) and
 * *retains* the bytes, but does **not** write them. A path is written only
 * when it is *demanded* — `waitForPath` (a build/read-back) or
 * `waitForAllPaths` (a bulk observation boundary) — and any path that is never
 * demanded is dropped when the queue is destroyed (the call-by-need GC of
 * un-forced thunks → the substitutable-subtree elision, LD-S2).
 *
 * The virtual store (`makeLazyDrvStore`) reads a path's *bytes* from this
 * queue without forcing a write, so the build can inspect a `.drv` (e.g. to
 * substitute its output) without materialising it.
 *
 * Ported in spirit from DeterminateSystems/nix; here it holds-not-writes
 * (lazy), deduplicates by the content-determined path (LD-P6), and
 * `waitForPath` transitively materialises a path's pending references first so
 * registration never sees a dangling reference.
 */
struct AsyncPathWriter
{
    virtual ~AsyncPathWriter() = default;

    /**
     * Enqueue `contents` as a `text:sha256` (flat) object named `name` with
     * `references`. Returns the content-addressed `StorePath` immediately; the
     * bytes are retained and written only on demand. Re-enqueuing the same
     * resulting path is a no-op (LD-P6).
     */
    virtual StorePath
    addPath(std::string contents, std::string name, StorePathSet references, RepairFlag repair = NoRepair) = 0;

    /**
     * The retained contents + references of a still-pending (not-yet-written)
     * path, or `nullopt` if it is not pending (already written, or never
     * enqueued). Used by the virtual store to serve `.drv` bytes.
     */
    struct Pending
    {
        std::string contents;
        StorePathSet references;
    };

    virtual std::optional<Pending> lookupPending(const StorePath & path) = 0;

    /** Is `path` enqueued and not yet written? */
    virtual bool isPending(const StorePath & path) = 0;

    /**
     * Enqueue a *source* tree as a deferred copy (Increment 6). Unlike
     * `addPath`, the bytes are NOT held in memory: the content-addressed
     * `StorePath` is computed now from `narHash` (the eval-time hash walk is
     * unavoidable — computing a derivation's `drvPath` eagerly needs its
     * `inputSrcs` named), but the actual walk-and-copy is deferred to `copy`, a
     * thunk (typically `MaterialisationScheduler::outPathOf`) retained and run
     * only when the source is *demanded* — i.e. it is a reference of a `.drv`
     * being materialised. A source that is never demanded is dropped at
     * destruction → the substitutable-target source copy is elided too (LD-S2,
     * extended from `.drv`s to their sources). The thunk must be idempotent
     * (it copies into the same content-addressed path, skipping if already
     * valid). Returns the content-addressed `StorePath`. Re-registering the
     * same resulting path is a no-op (LD-P6).
     */
    virtual StorePath addSource(
        std::string name,
        Hash narHash,
        ContentAddressMethod method,
        StoreReferences references,
        std::function<void()> copy) = 0;

    /** Is `path` a pending (registered, not-yet-copied) deferred source? */
    virtual bool isSourcePending(const StorePath & path) = 0;

    /**
     * The synthesised `ValidPathInfo` of a pending deferred source — its CA,
     * `narHash` and references are all known at registration, so the virtual
     * store can answer `queryPathInfo` for it without copying. `nullopt` if
     * `path` is not a pending source.
     */
    virtual std::optional<ValidPathInfo> lookupSource(const StorePath & path) = 0;

    /**
     * Materialise `path` (and, first, its transitive *pending* references, so
     * registration is referentially closed) to the store, removing them from
     * the queue. A no-op if `path` is not pending.
     */
    virtual void waitForPath(const StorePath & path) = 0;

    /**
     * Materialise a *set* of demanded paths (and their transitive pending
     * references) in ONE batched flush. Equivalent in effect to calling
     * `waitForPath` for each, but the writes are coalesced into a single
     * `materialise()` — over a daemon/remote store that is one framed
     * `addMultipleToStore` instead of one round-trip per path, which is the
     * difference that matters when a value-output boundary demands many `.drv`s
     * at once (e.g. `nix eval --json` over a remote eval-store). Unlike
     * `waitForAllPaths` this flushes only the demanded set, so un-demanded
     * pending entries stay deferred (elision is preserved).
     */
    virtual void waitForPaths(const StorePathSet & paths) = 0;

    /** Materialise every currently-pending path (bulk, reference-ordered). */
    virtual void waitForAllPaths() = 0;

    /**
     * Opt-in: start a background thread that materialises pending entries as
     * they accumulate, so the store writes *overlap* continued evaluation
     * (PROPOSAL-LAZY-DERIVATIONS.md §7, the "async overlap" property). This is
     * for the **write-everything** workloads — eval-store mass instantiation
     * (`nix-instantiate` of a jobset), where every `.drv` will be written and
     * there is nothing to elide. The **build** path deliberately does NOT call
     * this: it stays lazy so a substitutable subtree's `.drv`s are never
     * written (selective elision, LD-S2). Idempotent; the thread is stopped and
     * joined at `waitForAllPaths` and at destruction. A failure in the
     * background write latches and is re-thrown to every observer (LD-X1).
     */
    virtual void startBackgroundDrain() = 0;

    static ref<AsyncPathWriter> make(ref<Store> store);
};

/**
 * A store decorator that overlays `writer`'s pending `.drv`s on top of `next`:
 * a pending path reads back as valid (its bytes served from the queue) without
 * being written. This is what lets the build Worker inspect a deferred `.drv`
 * (and substitute its output) without materialising it (LD-S6). Writes and all
 * other operations delegate to `next`.
 */
ref<Store> makeLazyDrvStore(ref<Store> next, ref<AsyncPathWriter> writer);

} // namespace nix
