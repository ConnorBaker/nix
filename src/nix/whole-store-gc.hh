#pragma once
///@file

#include "nix/store/gc-store.hh"
#include "nix/store/store-cast.hh"
#include "nix/util/finally.hh"

namespace nix {

/**
 * Run a whole-store garbage collection on `store`'s `GcStore` capability.
 *
 * Centralises the dispatch shared by the three legacy and modern GC
 * entry points (`nix-store --gc`, `nix-collect-garbage`,
 * `nix store gc`):
 *
 *   - require a `GcStore` capability;
 *   - set `options.pathsToDelete = GCOptions::WholeStore{}` (the
 *     precondition all three callers share);
 *   - wrap `collectGarbage` in a `Finally` whose body calls
 *     `printResults(results)` regardless of how `collectGarbage`
 *     returns.
 *
 * `options.action` and any other tunables (e.g. `maxFreed`) must be
 * configured by the caller before this returns. The printer callback
 * runs even if `collectGarbage` throws, matching the existing
 * behaviour at every call site. Templated on `Printer` so the printer
 * lambda is captured inline rather than through `std::function`'s
 * type-erasure indirection.
 */
template<class Printer>
inline void runWholeStoreGC(Store & store, GCOptions & options, const Printer & printResults)
{
    auto & gcStore = require<GcStore>(store);
    options.pathsToDelete = GCOptions::WholeStore{};
    GCResults results;
    Finally printer([&] { printResults(results); });
    gcStore.collectGarbage(options, results);
}

} // namespace nix
