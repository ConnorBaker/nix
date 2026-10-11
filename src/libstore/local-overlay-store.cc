#include <regex>

#include "nix/store/local-overlay-store.hh"
#include "nix/util/callback.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/os-string.hh"
#include "nix/store/realisation.hh"
#include "nix/util/processes.hh"
#include "nix/store/store-open.hh"
#include "nix/store/store-registration.hh"

namespace nix {

void LocalOverlayStoreConfig::anchor() {}

void LocalOverlayStore::anchor() {}

std::string LocalOverlayStoreConfig::doc()
{
    return
#include "local-overlay-store.md"
        ;
}

ref<Store> LocalOverlayStoreConfig::openStore() const
{
    return ref{std::shared_ptr<LocalOverlayStore>{
        new LocalOverlayStore(ref{std::dynamic_pointer_cast<const LocalOverlayStoreConfig>(shared_from_this())}),
        LocalStore::Deleter{}}};
}

StoreReference LocalOverlayStoreConfig::getReference() const
{
    return {
        .variant =
            StoreReference::Specified{
                .scheme = *uriSchemes().begin(),
            },
    };
}

std::filesystem::path LocalOverlayStoreConfig::toUpperPath(const StorePath & path) const
{
    return upperLayer.get() / path.to_string();
}

LocalOverlayStore::LocalOverlayStore(ref<const Config> config)
    : Store{*config}
    , LocalFSStore{*config}
    , LocalStore{static_cast<ref<const LocalStore::Config>>(config)}
    , config{config}
    , lowerStore(openStore(config->lowerStoreUri.get()).dynamic_pointer_cast<LocalFSStore>())
{
    if (!config->upperLayer.isOverridden())
        throw Error("overlay store at %s requires the 'upper-layer' setting", PathFmt(config->realStoreDir.get()));

    if (config->checkMount.get()) {
        std::smatch match;
        std::string mountInfo;
        auto mounts = readFile(std::filesystem::path{"/proc/self/mounts"});
        auto regex = std::regex(R"((^|\n)overlay )" + config->realStoreDir.get().string() + R"( .*(\n|$))");

        // Mount points can be stacked, so there might be multiple matching entries.
        // Loop until the last match, which will be the current state of the mount point.
        while (std::regex_search(mounts, match, regex)) {
            mountInfo = match.str();
            mounts = match.suffix();
        }

        auto checkOption = [&](std::string_view option, const std::filesystem::path & value) {
            return std::regex_search(mountInfo, std::regex("\\b" + option + "=" + value.string() + "( |,)"));
        };

        auto expectedLowerDir = lowerStore->config.realStoreDir.get();
        if (!checkOption("lowerdir", expectedLowerDir) || !checkOption("upperdir", config->upperLayer.get())) {
            debug("expected lowerdir: %s", PathFmt(lowerStore->config.realStoreDir.get()));
            debug("expected upperdir: %s", PathFmt(config->upperLayer.get()));
            debug("actual mount: %s", mountInfo);
            throw Error("overlay filesystem %s mounted incorrectly", PathFmt(config->realStoreDir.get()));
        }
    }
}

void LocalOverlayStore::registerDrvOutputUnchecked(const Realisation & info)
{
    // First do queryRealisation on lower layer to populate DB
    auto res = lowerStore->queryRealisation(info.id);
    if (res)
        LocalStore::registerDrvOutputUnchecked({*res, info.id});

    LocalStore::registerDrvOutputUnchecked(info);
}

void LocalOverlayStore::queryPathInfoUncached(
    const StorePath & path, Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept
{
    auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

    LocalStore::queryPathInfoUncached(
        path, {[this, path, callbackPtr](Callback<std::shared_ptr<const ValidPathInfo>>::Result && fut) {
            try {
                auto info = std::move(fut).value();
                if (info)
                    return (*callbackPtr)(std::move(info));
            } catch (...) {
                return callbackPtr->rethrow();
            }
            // If we don't have it, check lower store.  Its info arrives with
            // its object hash: the lower store migrates an old row on read,
            // in memory when it is read-only.
            lowerStore->queryPathInfo(path, {[path, callbackPtr](Callback<ref<const ValidPathInfo>>::Result && fut) {
                                          try {
                                              (*callbackPtr)(std::move(fut).value().get_ptr());
                                          } catch (...) {
                                              return callbackPtr->rethrow();
                                          }
                                      }});
        }});
}

void LocalOverlayStore::queryRealisationUncached(
    const DrvOutput & drvOutput, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept
{
    auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

    LocalStore::queryRealisationUncached(
        drvOutput, {[this, drvOutput, callbackPtr](Callback<std::shared_ptr<const UnkeyedRealisation>>::Result && fut) {
            try {
                auto info = std::move(fut).value();
                if (info)
                    return (*callbackPtr)(std::move(info));
            } catch (...) {
                return callbackPtr->rethrow();
            }
            // If we don't have it, check lower store
            lowerStore->queryRealisation(drvOutput, std::move(*callbackPtr));
        }});
}

/**
 * The lower store's description of `path` with its object hash, as
 * `registerValidPath` requires.  A local lower store has it (migrated on
 * read); a lower store that is a daemon without
 * `WorkerProto::featureObjectHash` describes the path by its NAR hash
 * alone, so the object hash is computed by a walk of its object then.
 */
static ValidPathInfo lowerPathInfo(Store & lower, const StorePath & path)
{
    auto info = lower.queryPathInfo(path);
    if (info->objectHash)
        return *info;
    ValidPathInfo res{*info};
    auto object = objectHashOf(*lower.requireStoreObjectAccessor(path), CanonPath::root);
    res.objectHash = ObjectHash::of(object.root);
    if (!res.narSize)
        res.narSize = object.narSize.value();
    return res;
}

StorePathSet LocalOverlayStore::queryVisibleValidPaths()
{
    auto paths = LocalStore::queryAllValidPaths();
    paths.merge(lowerStore->queryVisibleValidPaths());
    return paths;
}

bool LocalOverlayStore::mayHaveValidPaths()
{
    return LocalStore::mayHaveValidPaths() || lowerStore->mayHaveValidPaths();
}

bool LocalOverlayStore::isValidPathUncached(const StorePath & path)
{
    auto res = LocalStore::isValidPathUncached(path);
    if (res)
        return res;
    res = lowerStore->isValidPath(path);
    if (res) {
        // Get path info from lower store so upper DB genuinely has it.
        auto p = lowerPathInfo(*lowerStore, path);
        // recur on references, syncing entire closure.
        for (auto & r : p.references)
            if (r != path)
                isValidPath(r);
        LocalStore::registerValidPath(p);
    }
    return res;
}

void LocalOverlayStore::queryReferrers(const StorePath & path, StorePathSet & referrers)
{
    LocalStore::queryReferrers(path, referrers);
    lowerStore->queryReferrers(path, referrers);
}

void LocalOverlayStore::queryGCReferrers(const StorePath & path, StorePathSet & referrers)
{
    LocalStore::queryReferrers(path, referrers);
}

StorePathSet LocalOverlayStore::queryValidDerivers(const StorePath & path)
{
    auto res = LocalStore::queryValidDerivers(path);
    for (const auto & p : lowerStore->queryValidDerivers(path))
        res.insert(p);
    return res;
}

std::optional<StorePath> LocalOverlayStore::queryPathFromHashPart(const std::string & hashPart)
{
    auto res = LocalStore::queryPathFromHashPart(hashPart);
    if (res)
        return res;
    else
        return lowerStore->queryPathFromHashPart(hashPart);
}

void LocalOverlayStore::registerValidPaths(const ValidPathInfos & infos)
{
    registerValidPaths(infos, nullptr);
}

void LocalOverlayStore::registerValidPaths(const ValidPathInfos & infos, const GitObjectStore::Hold * hold)
{
    validatePathInfoKeys(infos);

    // First, get any from lower store so we merge
    {
        StorePathSet notInUpper;
        for (auto & [p, _] : infos)
            if (!LocalStore::isValidPathUncached(p)) // avoid divergence
                notInUpper.insert(p);
        auto pathsInLower = lowerStore->queryValidPaths(notInUpper);
        ValidPathInfos inLower;
        for (auto & p : pathsInLower)
            inLower.insert_or_assign(p, lowerPathInfo(*lowerStore, p));
        LocalStore::registerValidPaths(inLower, nullptr);
    }
    // Then do original request
    LocalStore::registerValidPaths(infos, hold);
}

void LocalOverlayStore::collectGarbage(const GCOptions & options, GCResults & results)
{
    LocalStore::collectGarbage(options, results);

    remountIfNecessary();
}

void LocalOverlayStore::sweepObjectStore()
{
    /* Reap only upper-layer leases; the merged mount also shows the lower
       store's namespace, which this cleanup does not own. */
    GitObjectStore::Owned owned{
        .dir = config->upperLayer.get() / ".objects",
        /* The cleaner removes through descriptors opened in the upper
           layer. Refresh the merged view after any actual removal. */
        .removed = [&] { _remountRequired = true; },
    };

    objects.sweep(&owned);
}

void LocalOverlayStore::deleteStorePath(const std::filesystem::path & path, uint64_t & bytesFreed, bool isKnownPath)
{
    bytesFreed = 0;
    if (path.parent_path() != config->realStoreDir.get()) {
        warn("local-overlay: unexpected gc path %s", PathFmt(path));
        return;
    }

    auto name = CanonPath::fromFilename(path.filename().string());
    auto upperPath = config->upperLayer.get() / name.rel();

    if (pathExists(upperPath)) {
        _remountRequired = true;
        LocalStore::deleteStorePath(upperPath, bytesFreed, isKnownPath);
    }
}

void LocalOverlayStore::optimiseStore()
{
    Activity act(*logger, actOptimiseStore);

    // Note for LocalOverlayStore, queryAllValidPaths only returns paths in upper layer
    auto paths = queryAllValidPaths();

    act.progress(0, paths.size());

    uint64_t done = 0;

    for (auto & path : paths) {
        if (lowerStore->isValidPath(path)) {
            uint64_t bytesFreed = 0;
            // Deduplicate store path
            deleteStorePath(toRealPath(path), bytesFreed, true);
        }
        done++;
        act.progress(done, paths.size());
    }

    remountIfNecessary();
}

LocalStore::VerificationResult LocalOverlayStore::verifyAllValidPaths(RepairFlag repair)
{
    StorePathSet done;

    auto existsInStoreDir = [&](const StorePath & storePath) {
        return pathExists(config->realStoreDir.get() / storePath.to_string());
    };

    bool errors = false;
    StorePathSet validPaths;

    for (auto & i : queryAllValidPaths())
        verifyPath(i, existsInStoreDir, done, validPaths, repair, errors);

    return {
        .errors = errors,
        .validPaths = validPaths,
    };
}

void LocalOverlayStore::remountIfNecessary()
{
    if (!_remountRequired)
        return;

    if (auto & hook = config->remountHook.get()) {
        runProgram(*hook, false, {config->realStoreDir.get().native()});
    } else {
        warn("%s needs remounting, set remount-hook to do this automatically", PathFmt(config->realStoreDir.get()));
    }

    _remountRequired = false;
}

static RegisterStoreImplementation<LocalOverlayStore::Config> regLocalOverlayStore;

} // namespace nix
