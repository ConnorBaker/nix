#include "nix/expr/input-materialisation.hh"
#include "nix/util/error.hh"
#include "nix/util/memo.hh"
#include "nix/util/source-path.hh"
#include "nix/store/store-api.hh"
#include "nix/fetchers/fetch-to-store.hh"

#include <limits>

namespace nix {

InputMaterialisation::InputMaterialisation(
    const fetchers::Settings & fetchSettings,
    ref<Store> store,
    ref<SourceAccessor> accessor,
    std::string name,
    std::optional<Hash> expectedNarHash)
    : fetchSettings(fetchSettings)
    , store(store)
    , accessor(accessor)
    , name(std::move(name))
    , expectedNarHash(expectedNarHash)
{
}

std::pair<StorePath, Hash> InputMaterialisation::force()
{
    /* Fast path: already done or already failed. */
    {
        auto st = state_.readLock();
        if (st->phase == Phase::Done)
            return {*st->storePath, *st->narHash};
        if (st->phase == Phase::Failed)
            std::rethrow_exception(st->error);
    }

    /* Slow path: become the winner or wait on a winner's future. */
    std::shared_future<Hash> waitFuture;
    bool weAreTheWinner = false;
    std::promise<Hash> ours;
    {
        auto st = state_.lock();
        if (st->phase == Phase::Done)
            return {*st->storePath, *st->narHash};
        if (st->phase == Phase::Failed)
            std::rethrow_exception(st->error);
        if (st->phase == Phase::Pending) {
            weAreTheWinner = true;
            st->phase = Phase::Running;
            st->future = ours.get_future().share();
            waitFuture = st->future;
        } else {
            /* Running — there's an in-flight walk; wait on it. */
            waitFuture = st->future;
        }
    }

    if (!weAreTheWinner) {
        (void) waitFuture.get(); // throws if winner failed
        auto st = state_.readLock();
        if (st->phase == Phase::Done)
            return {*st->storePath, *st->narHash};
        std::rethrow_exception(st->error);
    }

    /* Winner runs the walk without holding the state lock. */
    try {
        /* The narHash walk below (`fetchToStore2`) dumps the WHOLE
           tree, reading every blob. On a partial clone that means
           every blob must be backfilled; without a bulk prefetch each
           one would fault in singly via `GitSourceAccessor::readBlob`'s
           per-blob fallback — one HTTPS round-trip per file. So prime
           the whole subtree in ONE coalesced request first. This is
           the same `prefetchSubtree` seam the `prim_readFile` /
           `prim_readDir` eval triggers use, here applied at the root
           with unbounded depth because the walk is exhaustive.

           No-op unless the accessor is a partial-clone `GitSourceAccessor`
           with a provider attached (the default `prefetchSubtree` does
           nothing); harmless otherwise. This is what makes lazy-FETCH
           and the eventual narHash WALK compose efficiently: laziness
           saves the blobs nobody reads, and when the hash IS demanded
           the unavoidable full read is one request, not N. */
        accessor->prefetchSubtree(CanonPath::root, std::numeric_limits<unsigned>::max());

        auto [storePath, narHash] = fetchToStore2(fetchSettings, *store, SourcePath{accessor}, FetchMode::DryRun, name);

        if (expectedNarHash && narHash != *expectedNarHash)
            throw Error(
                (unsigned int) 102,
                "NAR hash mismatch in input '%s', expected '%s' but got '%s'",
                name,
                expectedNarHash->to_string(HashFormat::SRI, true),
                narHash.to_string(HashFormat::SRI, true));

        {
            auto st = state_.lock();
            st->phase = Phase::Done;
            st->storePath = storePath;
            st->narHash = narHash;
        }
        ours.set_value(narHash);
        return {storePath, narHash};
    } catch (...) {
        auto eptr = std::current_exception();
        {
            auto st = state_.lock();
            st->phase = Phase::Failed;
            st->error = eptr;
        }
        ours.set_exception(eptr);
        std::rethrow_exception(eptr);
    }
}

std::optional<Hash> InputMaterialisation::peekNarHash() const
{
    auto st = state_.readLock();
    switch (st->phase) {
    case Phase::Pending:
    case Phase::Running:
        return std::nullopt;
    case Phase::Done:
        return *st->narHash;
    case Phase::Failed:
        std::rethrow_exception(st->error);
    }
    /* All Phase values are handled above; this is unreachable. */
    return std::nullopt;
}

fetchers::LazyAttr makeVirtualNarHashAttr(ref<InputMaterialisation> mat)
{
    /* Hold `mat` by `ref<>` in the closure so its lifetime extends
       past `mountInput`'s stack frame. `EvalState::inputMaterialisations_`
       also retains it for `peekNarHash()` / debugging access.

       `memo<>` mirrors `git.cc::makeLazyAttr` — at the LazyAttr layer,
       the result of the first call is memoised so subsequent forces
       don't re-enter `mat->force()`'s lock acquisition on the hot
       path. (Once Done, `mat->force()` itself short-circuits via the
       readLock fast path, but the memo skips even that.) */
    return make_ref<fetchers::LazyAttrComputation>(fetchers::LazyAttrComputation{
        .compute = memo<fetchers::ResolvedAttr>(fun<fetchers::ResolvedAttr()>([mat]() -> fetchers::ResolvedAttr {
            auto pair = mat->force();
            return pair.second.to_string(HashFormat::SRI, true);
        })),
    });
}

} // namespace nix
