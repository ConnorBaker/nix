#pragma once

#include "nix/util/source-path.hh"
#include "nix/store/store-api.hh"
#include "nix/util/file-system.hh"
#include "nix/util/repair-flag.hh"
#include "nix/util/file-content-address.hh"
#include "nix/util/merkle-files.hh"
#include "nix/fetchers/cache.hh"

namespace nix {

enum struct FetchMode { DryRun, Copy };

/**
 * Copy the `path` to the Nix store.  Without a `method`, the source is named
 * by its git tree hash, the store's object hash
 * (doc/lazy-store/01-specification.md, section 9.11).
 */
StorePath fetchToStore(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & path,
    FetchMode mode,
    std::string_view name = "source",
    std::optional<ContentAddressMethod> method = std::nullopt,
    PathFilter * filter = nullptr,
    RepairFlag repair = NoRepair);

std::pair<StorePath, Hash> fetchToStore2(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & path,
    FetchMode mode,
    std::string_view name = "source",
    std::optional<ContentAddressMethod> method = std::nullopt,
    PathFilter * filter = nullptr,
    RepairFlag repair = NoRepair);

/**
 * The key of a named tree's row in the `sourcePathToHash` memo.  Under
 * the NAR method the row is `{hash}`; under the git method it is the
 * node's entry, `{hash, mode}` (01 §9.9, §9.11), and the key carries
 * `version = 2` so that the hash-only rows of the earlier form are misses.
 */
fetchers::Cache::Key
makeSourcePathToHashCacheKey(std::string_view fingerprint, ContentAddressMethod method, const CanonPath & path);

/**
 * The store path of a tree named by its git tree hash, the store's object
 * hash (doc/lazy-store/01-specification.md, section 9.11).
 */
StorePath gitTreePath(const StoreDirConfig & store, std::string_view name, const Hash & treeHash);

/**
 * Seed the git memo row of the named tree at `root` from a store object's
 * hash: for a directory or a plain file the object hash is the root's
 * entry hash, and the row is written; for a bare executable or symlink it
 * is the synthetic tree's id, which does not give the blob's back, so
 * nothing is written and the next naming hashes the one file.  A memo: an
 * unnamed root or a cache that cannot be opened records nothing.
 */
void recordRootEntry(const fetchers::Settings & settings, const SourcePath & root, const Hash & objectHash);

/**
 * The fetcher cache when it can be opened, else null: every memo is an
 * optimisation, so a cache that cannot be created (a read-only home) is a
 * miss, not an error.  The naming memos go through this rather than
 * `settings.getCache()`, which opens the cache eagerly.
 */
std::shared_ptr<fetchers::Cache> cacheIfAvailable(const fetchers::Settings & settings);

/**
 * The NAR hash of `tree`, a tree named by `treeHash` (the store's object
 * hash): one memoised dry run under `NixArchive`, and for a named tree the
 * pair is recorded in the `treeAddress` memo
 * (doc/lazy-store/01-specification.md, section 9.11).
 */
Hash narHashOf(const fetchers::Settings & settings, Store & store, const SourcePath & tree, const Hash & treeHash);

/**
 * The one rendering of a failed input-level assertion, exit status 102:
 * `<kind> mismatch in input '<input>', expected '<expected>' but got
 * '<got>'`, the hashes as SRI.
 */
Error inputHashMismatch(std::string_view kind, std::string_view input, const Hash & expected, const Hash & got);

/**
 * Verify an asserted NAR hash (a version-7 lock's `narHash`, a `?narHash=`,
 * a `sha256` on `builtins.path`) against `tree`, named by `treeHash`.  The
 * `treeAddress` memo answers first when it already pairs the two; else the
 * walk of `narHashOf`.
 *
 * @throws `inputHashMismatch("NAR hash", …)`.
 */
void assertNarHash(
    const fetchers::Settings & settings,
    Store & store,
    const SourcePath & tree,
    const Hash & treeHash,
    const Hash & asserted,
    std::string_view inputDescription);

/**
 * The memo from a tree's NAR hash to its git tree hash
 * (doc/lazy-store/01-specification.md, sections 2.4 and 9.11): recorded
 * wherever both are computed for a named tree, read where something names a
 * tree by its NAR hash while the store names it by the tree hash.
 */
std::optional<Hash> lookupTreeAddress(const fetchers::Settings & settings, const Hash & narHash);

void recordTreeAddress(const fetchers::Settings & settings, const Hash & narHash, const Hash & treeHash);

/**
 * The tree `filter` admits of `tree`, as one fixed-set accessor
 * (`makeFixedSetFilteringSourceAccessor` over `filteredPaths`): the filter
 * is evaluated once, here, and the result is named by the inner tree's
 * name and the admitted set.  Dumping it without a filter gives the NAR
 * that dumping `tree` with `filter` would.
 */
SourcePath filteredTree(const SourcePath & tree, PathFilter & filter);

} // namespace nix
