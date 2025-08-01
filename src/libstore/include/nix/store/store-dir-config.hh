#pragma once

#include "nix/store/path.hh"
#include "nix/util/hash.hh"
#include "nix/store/content-address.hh"
#include "nix/store/globals.hh"
#include "nix/util/configuration.hh"

#include <map>
#include <string>
#include <variant>

namespace nix {

struct SourcePath;

MakeError(BadStorePath, Error);
MakeError(BadStorePathName, BadStorePath);

/**
 * @todo This should just be part of `StoreDirConfig`. However, it would
 * be a huge amount of churn if `Store` didn't have these methods
 * anymore, forcing a bunch of code to go from `store.method(...)` to
 * `store.config.method(...)`.
 *
 * So we instead pull out the methods into their own mix-in, so can put
 * them directly on the Store too.
 */
struct MixStoreDirMethods
{
    const Path & storeDir;

    // pure methods

    StorePath parseStorePath(std::string_view path) const;

    std::optional<StorePath> maybeParseStorePath(std::string_view path) const;

    std::string printStorePath(const StorePath & path) const;

    /**
     * Deprecated
     *
     * \todo remove
     */
    StorePathSet parseStorePathSet(const PathSet & paths) const;

    PathSet printStorePathSet(const StorePathSet & path) const;

    /**
     * Display a set of paths in human-readable form (i.e., between quotes
     * and separated by commas).
     */
    std::string showPaths(const StorePathSet & paths) const;

    /**
     * @return true if *path* is in the Nix store (but not the Nix
     * store itself).
     */
    bool isInStore(PathView path) const;

    /**
     * @return true if *path* is a store path, i.e. a direct child of the
     * Nix store.
     */
    bool isStorePath(std::string_view path) const;

    /**
     * Split a path like `/nix/store/<hash>-<name>/<bla>` into
     * `/nix/store/<hash>-<name>` and `/<bla>`.
     */
    std::pair<StorePath, Path> toStorePath(PathView path) const;

    /**
     * Constructs a unique store path name.
     */
    StorePath makeStorePath(std::string_view type, std::string_view hash, std::string_view name) const;
    StorePath makeStorePath(std::string_view type, const Hash & hash, std::string_view name) const;

    StorePath makeOutputPath(std::string_view id, const Hash & hash, std::string_view name) const;

    StorePath makeFixedOutputPath(std::string_view name, const FixedOutputInfo & info) const;

    StorePath makeFixedOutputPathFromCA(std::string_view name, const ContentAddressWithReferences & ca) const;

    /**
     * Read-only variant of addToStore(). It returns the store
     * path for the given file system object.
     */
    std::pair<StorePath, Hash> computeStorePath(
        std::string_view name,
        const SourcePath & path,
        ContentAddressMethod method = FileIngestionMethod::NixArchive,
        HashAlgorithm hashAlgo = HashAlgorithm::SHA256,
        const StorePathSet & references = {},
        PathFilter & filter = defaultPathFilter) const;
};

/**
 * Need to make this a separate class so I can get the right
 * initialization order in the constructor for `StoreDirConfig`.
 */
struct StoreDirConfigBase : Config
{
    using Config::Config;

    const PathSetting storeDir_{
        this,
        settings.nixStore,
        "store",
        R"(
          Logical location of the Nix store, usually
          `/nix/store`. Note that you can only copy store paths
          between stores if they have the same `store` setting.
        )"};
};

/**
 * The order of `StoreDirConfigBase` and then `MixStoreDirMethods` is
 * very important. This ensures that `StoreDirConfigBase::storeDir_`
 * is initialized before we have our one chance (because references are
 * immutable) to initialize `MixStoreDirMethods::storeDir`.
 */
struct StoreDirConfig : StoreDirConfigBase, MixStoreDirMethods
{
    using Params = StringMap;

    StoreDirConfig(const Params & params);

    StoreDirConfig() = delete;

    virtual ~StoreDirConfig() = default;
};

} // namespace nix
