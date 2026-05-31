#pragma once

#include "nix/util/source-path.hh"
#include "nix/store/store-api.hh"
#include "nix/util/file-system.hh"
#include "nix/util/repair-flag.hh"
#include "nix/util/file-content-address.hh"
#include "nix/fetchers/cache.hh"

namespace nix {

enum struct FetchMode { DryRun, Copy };

/**
 * Copy the `path` to the Nix store.
 *
 * `refs` is the set of additional store paths the result references
 * (e.g. when `path` is a subpath of an existing store path that itself
 * contains store-path references). When non-empty, both cache-hit
 * store-path reconstruction and the miss-path `addToStore` call thread
 * `refs` through; the cache key is unchanged because refs are
 * content-derived from `path`.
 */
StorePath fetchToStore(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & path,
    FetchMode mode,
    std::string_view name = "source",
    ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive,
    PathFilter * filter = nullptr,
    RepairFlag repair = NoRepair,
    const StorePathSet & refs = {});

std::pair<StorePath, Hash> fetchToStore2(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & path,
    FetchMode mode,
    std::string_view name = "source",
    ContentAddressMethod method = ContentAddressMethod::Raw::NixArchive,
    PathFilter * filter = nullptr,
    RepairFlag repair = NoRepair,
    const StorePathSet & refs = {});

fetchers::Cache::Key
makeSourcePathToHashCacheKey(std::string_view fingerprint, ContentAddressMethod method, const CanonPath & path);

} // namespace nix
