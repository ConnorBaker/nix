#pragma once
///@file
///
/// `MaterialisationScheduler` — eval-side floating-CA realisation
/// registry.
///
/// `addPath` (and similar source-injection sites) register a
/// `SourceView` here against a content-determined `SourceContentId`
/// and get a `SourcePlaceholder` back. **No walk happens at
/// registration.**
///
/// When a consumer demands a real storePath (derivation as input,
/// CLI print, lock-file write), it calls `outPathOf(placeholder)`
/// or `outPathsOf(many)`. The scheduler:
///
///   1. Looks up the contentId in the in-process realisation map.
///   2. Misses fall through to the persistent
///      `filteredSourcePathToHash` projection (Track D).
///   3. Persistent misses trigger the walk via `fetchToStore` —
///      coalesced via `shared_future` so concurrent demanders share
///      the work.
///   4. Batch demands (`outPathsOf(span<placeholders>)`) dispatch
///      independent walks in parallel through `ThreadPool`.
///
/// The cargo-workspace property: 200 placeholders sharing one
/// contentId trigger one walk and 200 cheap storePath computations.

#include "nix/store/source-content-id.hh"
#include "nix/store/source-placeholder.hh"
#include "nix/util/sync.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <future>
#include <memory>
#include <span>
#include <unordered_map>

namespace nix {

class Store;
struct EvalState;
struct SourceViewAccessor;
struct ContentAddressMethod;
struct StoreReferences;

struct MaterialisationScheduler
{
    EvalState & state;

    explicit MaterialisationScheduler(EvalState & state)
        : state(state)
    {
    }

    /** A pending or completed registration. The contentId is what
     *  the realisation cache keys on; the placeholder is what
     *  consumers reference. */
    struct Registration
    {
        SourceContentId contentId;
        std::string name;
        ContentAddressMethod method;
        StoreReferences refs;
        ref<SourceViewAccessor> view;
    };

    /** Register a SourceView. Returns a placeholder. No walk. */
    SourcePlaceholder registerView(Registration);

    /** Look up the registration for a placeholder. nullopt if the
     *  placeholder isn't ours (or we forgot it). */
    std::shared_ptr<const Registration> lookup(const SourcePlaceholder &) const;

    /** Demand realisation. Idempotent. Coalesces concurrent
     *  demands for the same contentId. */
    StorePath outPathOf(const SourcePlaceholder &);

    /** Batch demand. Independent contentIds dispatch in parallel
     *  via ThreadPool; demands sharing a contentId coalesce.
     *  Returns a map placeholder → storePath. */
    std::unordered_map<SourcePlaceholder, StorePath> outPathsOf(std::span<const SourcePlaceholder>);

    /** Look up the narHash for a contentId, computing+persisting
     *  if missing. Used directly by callers that want the NAR hash
     *  but not a storePath. */
    Hash narHashOf(const SourceContentId &);

private:
    /* These three are read-mostly (written once at registerView / once per
       cold walk, READ on every demand — including concurrently by the parallel
       `outPathsOf` group threads). `SharedSync` (not `Sync`) so `.readLock()` is
       a genuine shared lock: under `Sync`, `readLock()` is an *exclusive*
       std::mutex acquire, serialising the per-group lookups. Write sites use
       `.lock()` (exclusive) under both, so the change is API-compatible. */

    /* contentId → narHash. Cached in-process; backed by Track D's
       persistent filteredSourcePathToHash projection so cross-
       process sharing is automatic. */
    SharedSync<std::unordered_map<SourceContentId, Hash>> narHashByContent_;

    /* placeholder → registration. Constructed at registerView,
       consulted at outPathOf. */
    SharedSync<std::unordered_map<SourcePlaceholder, std::shared_ptr<Registration>>> registrations_;

    /* contentId → registrations sharing it. A secondary index built
       alongside `registrations_` so `narHashOf`'s walk-winner can find
       *a* registration for a contentId in O(1) average instead of
       linear-scanning `registrations_` (PF-5). Many placeholders can
       share one contentId (the cargo-workspace shape), hence multimap;
       any of them suffices as the walk source (same contentId ⇒ same
       narHash). */
    SharedSync<std::unordered_multimap<SourceContentId, std::shared_ptr<Registration>>> regsByContent_;

    /* In-flight walks: shared_future for coalescing. Keyed on
       contentId — concurrent demands for the same content share
       one walk. */
    boost::concurrent_flat_map<SourceContentId, std::shared_future<Hash>> inFlight_;

    /** Walk and hash. Internal; called from narHashOf on a miss. */
    Hash computeNarHash(const Registration &);

    /** Before a whole-tree walk (DryRun or Copy) of `reg`'s view,
     *  prime every reachable blob in ONE coalesced request if the
     *  underlying accessor is a partial-clone git source. No-op
     *  otherwise (the base `prefetchSubtree` does nothing). This is the
     *  scheduler-side analogue of the prefetch in
     *  `InputMaterialisation::force` — without it a partial-clone
     *  `builtins.path` would fault blobs one-per-round-trip during the
     *  scheduler's walk (PF-2). */
    void prefetchForWalk(const Registration &);

    /** The synthetic-subset-tree OID for `reg`'s view (Track Z.gap5),
     *  or nullopt for non-git / non-Subset / non-NixArchive. Computed
     *  ONCE per cold materialisation and threaded into both the peek and
     *  the writeback (PF-4) rather than re-synthesised in each. */
    std::optional<Hash> synthesiseViewTreeOidFor(const Registration &);

    /** Track Z.gap5 writeback: key the freshly-walked `narHash` on the
     *  precomputed synthetic subset tree OID (`synthOid`, from
     *  `synthesiseViewTreeOidFor`) so a different base / pipeline
     *  filtering to the same subset hits the `treeHashToNarHash` bridge
     *  next time. No-op when `synthOid` is nullopt. Shared by
     *  `computeNarHash` and `materialiseFused` so the cargo fused path
     *  (PF-1) keeps the cross-base normalisation the DryRun prelude used
     *  to provide. */
    void writeSynthesisedNarHash(const Registration &, const std::optional<Hash> & synthOid, const Hash & narHash);

    /** Peek the narHash caches (in-process map, then persistent
     *  `sourceContentToNarHash` projection) WITHOUT walking or
     *  coalescing. Returns nullopt on a genuine miss. Shared by
     *  `narHashOf` and the single-observer fused path. */
    std::optional<Hash> peekNarHash(const SourceContentId &);

    /** Populate both narHash caches (in-process + persistent).
     *  Idempotent. */
    void cacheNarHash(const SourceContentId &, const Hash &);

    /** Single-walk materialisation for the sole-observer cache-miss
     *  case: one `fetchToStore2(Copy)` yields BOTH the storePath and
     *  the narHash (for NixArchive the copy's hash IS the narHash), so
     *  the separate `narHashOf` DryRun walk is elided. Caches the hash
     *  and returns the storePath. Only sound when there is exactly one
     *  observer for the contentId (no shared walk to coalesce). */
    StorePath materialiseFused(const Registration &);

    /** Materialise one contentId group (≥2 placeholders sharing a
     *  contentId, narHash already cached). On a `LocalStore`, Copy the
     *  first sibling's bytes once and HARDLINK the rest into their
     *  name-stamped CA paths (copy-once-link-N, Perf #1); siblings with
     *  a self-reference, or any non-`LocalStore`, fall back to
     *  independent `outPathOf` copies. Writes results into `out` under
     *  each placeholder's key. */
    void materialiseGroup(
        const std::vector<SourcePlaceholder> & group,
        const std::unordered_map<SourcePlaceholder, std::shared_ptr<const Registration>> & regs,
        std::unordered_map<SourcePlaceholder, StorePath> & out);
};

} // namespace nix
