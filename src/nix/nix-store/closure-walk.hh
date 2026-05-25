#pragma once
///@file

#include "nix/store/store-api.hh"
#include "nix/store/path.hh"

namespace nix {

/**
 * BFS over the closure of `roots` via `references`. For each
 * not-yet-seen path, query its `ValidPathInfo` once and call
 * `onPath(path, *info)`; for each non-self reference, call
 * `onEdge(path, reference)` and enqueue the reference. Self-references
 * are skipped. The visit order is whatever `StorePathSet::extract`
 * returns from its underlying ordered container; both consumers
 * (`dotgraph.cc` and `graphml.cc`) write their output to a single
 * `std::ostream` and don't depend on the order beyond determinism.
 *
 * Pulled out of `dotgraph.cc` and `graphml.cc` where the same scaffold
 * was duplicated. Kept in `nix-store/` (rather than promoted to
 * `libstore/include/nix/store/store-api.hh` per #76's original
 * sketch) so the change stays single-shard.
 */
template<class OnPath, class OnEdge>
void walkClosure(ref<Store> store, StorePathSet && roots, OnPath && onPath, OnEdge && onEdge)
{
    StorePathSet workList(std::move(roots));
    StorePathSet doneSet;
    while (!workList.empty()) {
        auto path = std::move(workList.extract(workList.begin()).value());
        if (!doneSet.insert(path).second)
            continue;
        auto info = store->queryPathInfo(path);
        onPath(path, *info);
        for (auto & p : info->references) {
            if (p != path) {
                workList.insert(p);
                onEdge(path, p);
            }
        }
    }
}

} // namespace nix
