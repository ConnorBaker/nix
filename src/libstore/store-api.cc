#include "nix/store/build/worker.hh"
#include "nix/util/logging.hh"
#include "nix/util/signature/local-keys.hh"
#include "nix/util/source-accessor.hh"
#include "nix/store/globals.hh"
#include "nix/store/derived-path.hh"
#include "nix/store/realisation.hh"
#include "nix/store/derivations.hh"
#include "nix/store/derivation/resolution.hh"
#include "nix/store/derivation/aterm.hh"
#include "nix/store/store-api.hh"
#include "nix/store/local-store.hh"
#include "nix/store/build.hh"
#include "nix/store/store-open.hh"
#include "nix/store/outputs-query.hh"
#include "nix/util/util.hh"
#include "nix/store/nar-info-disk-cache.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/archive.hh"
#include "nix/util/callback.hh"
#include "nix/util/object-hash-sink.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/signals.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"

#include "store-config-private.hh"

#include <filesystem>
#include <array>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#  include "nix/util/windows-known-folders.hh"
#endif

using json = nlohmann::json;

namespace nix {

void StoreConfig::anchor() {}

void InvalidPath::anchor() {}

void Unsupported::anchor() {}

void SubstituteGone::anchor() {}

void SubstituterDisabled::anchor() {}

void InvalidStoreReference::anchor() {}

void StoreConfigBase::anchor() {}

void Builder::anchor() {}

static std::string canonStoreDir(std::string path)
{
    if (path.empty() || path[0] != '/')
        throw UsageError("store directory path \"%s\" is not an absolute path", path);
    return CanonPath(std::move(path)).abs();
}

static std::string canonStoreDir(std::filesystem::path path)
{
    if (!path.is_absolute())
        throw UsageError("store directory path %s is not an absolute path", PathFmt(path));
    return canonPath(std::move(path)).string();
}

StoreConfigBase::StoreDirSetting::StoreDirSetting(Config * options, FilePathType pathType)
    : BaseSetting<std::string>(
          [pathType]() -> std::string {
              auto envOverrides = getEnvOsNonEmpty(OS_STR("NIX_STORE_DIR")).or_else([] {
                  return getEnvOsNonEmpty(OS_STR("NIX_STORE"));
              });

              switch (pathType) {
              case FilePathType::Unix:
                  return canonStoreDir(envOverrides.transform([](const auto & s) { return os_string_to_string(s); })
                                           .value_or(NIX_STORE_DIR));

              case FilePathType::Native:
                  return canonStoreDir(envOverrides.transform([](const auto & s) { return std::filesystem::path(s); })
                                           .or_else([]() -> std::optional<std::filesystem::path> {
#ifdef _WIN32
                                               return windows::known_folders::getProgramData() / "nix" / "store";
#else
                                               return std::filesystem::path{NIX_STORE_DIR};
#endif
                                           })
                                           .value());
              }
              assert(false);
          }(),
          true,
          "store",
          R"(
            Logical location of the Nix store, usually `/nix/store`.

            Defaults to [`NIX_STORE_DIR`](@docroot@/command-ref/env-common.md#env-NIX_STORE_DIR) if unset.

            Note that you can only copy store paths between stores if they have the same `store` setting.
          )",
          {})
    , pathType(pathType)
{
    options->addSetting(this);
}

std::string StoreConfigBase::StoreDirSetting::parse(const std::string & str) const
{
    if (str.empty())
        throw UsageError("setting '%s' is a path and paths cannot be empty", name);

    switch (pathType) {
    case FilePathType::Unix:
        return canonStoreDir(str);
    case FilePathType::Native:
        return canonStoreDir(std::filesystem::path(str));
    }
    assert(false);
}

StoreConfigBase::StoreConfigBase(const StoreReference::Params & params, FilePathType pathType)
    : Config(params)
    , storeDir_{this, pathType}
{
}

StoreConfig::StoreConfig(const Params & params, FilePathType pathType)
    : StoreConfigBase(params, pathType)
    , StoreDirConfig{storeDir_}
{
}

bool StoreDirConfig::isInStore(std::string_view path) const
{
    return isInDir(path, storeDir);
}

std::pair<StorePath, CanonPath> StoreDirConfig::toStorePath(std::string_view path) const
{
    if (!isInStore(path))
        throw Error("path '%1%' is not in the Nix store", path);
    auto slash = path.find('/', storeDir.size() + 1);
    if (slash == std::string::npos)
        return {parseStorePath(path), CanonPath::root};
    else
        return {parseStorePath(path.substr(0, slash)), CanonPath{path.substr(slash)}};
}

ref<Builder> Store::getBuilder(std::shared_ptr<Store> evalStore)
{
    auto store = ref<Store>(shared_from_this());
    auto evalStoreRef = evalStore ? ref<Store>(std::move(evalStore)) : store;
    return make_ref<LocalBuilder>(store, evalStoreRef);
}

std::filesystem::path Store::followLinksToStore(std::string_view _path) const
{
    auto path = absPath(std::string(_path));

    // Limit symlink follows to prevent infinite loops
    unsigned int followCount = 0;
    const unsigned int maxFollow = 1024;

    while (!isInStore(path.string())) {
        if (!std::filesystem::is_symlink(path))
            break;

        if (++followCount >= maxFollow)
            throw Error("too many symbolic links encountered while resolving '%s'", _path);

        auto target = readLink(path);
        auto parentPath = path.parent_path();
        path = absPath(target, &parentPath);
    }

    if (!isInStore(path.string()))
        throw BadStorePath("path %1% is not in the Nix store", PathFmt(path));
    return path;
}

StorePath Store::followLinksToStorePath(std::string_view path) const
{
    return toStorePath(followLinksToStore(path).string()).first;
}

StorePath Store::addTextToStore(
    std::string_view name, std::string_view contents, const StorePathSet & references, RepairFlag repair)
{
    auto path = makeFixedOutputPathFromCA(
        name, TextInfo{.hash = hashString(HashAlgorithm::SHA256, contents), .references = references});

    // Root before testing validity: an existing referrer protects its closure.
    addTempRoot(path);
    if (isValidPath(path) && !repair)
        return path;

    // Publication can trigger GC, including in an older remote daemon.
    for (const auto & reference : references)
        addTempRoot(reference);

    StringSource source(contents);
    auto written = addToStoreFromDump(
        source,
        name,
        FileSerialisationMethod::Flat,
        ContentAddressMethod::Raw::Text,
        HashAlgorithm::SHA256,
        references,
        repair);
    assert(written == path);
    return path;
}

StorePath Store::addToStore(
    std::string_view name,
    const SourcePath & path,
    ContentAddressMethod method,
    HashAlgorithm hashAlgo,
    const StorePathSet & references,
    PathFilter & filter,
    RepairFlag repair,
    TreeNamer namer,
    bool callbacksPermitParallelism)
{
    if (method == ContentAddressMethod::Raw::Git && hashAlgo == HashAlgorithm::SHA256 && references.empty())
        if (auto local = dynamic_cast<LocalStore *>(this); local && local->materialisesFromObjects())
            return local->materialise(name, path, filter, repair, std::move(namer), callbacksPermitParallelism);

    auto fsm = method.getFileSerialisationMethod();
    std::optional<StorePath> storePath;
    auto sink = sourceToSink([&](Source & source) {
        LengthSource lengthSource(source);
        storePath = addToStoreFromDump(lengthSource, name, fsm, method, hashAlgo, references, repair);
        if (settings.warnLargePathThreshold && lengthSource.total >= settings.warnLargePathThreshold)
            warn("copied large path '%s' to the store (%s)", path, renderSize(lengthSource.total));
    });
    dumpPath(path, *sink, fsm, filter);
    sink->finish();
    return storePath.value();
}

void Store::addMultipleToStore(
    PathsSource && pathsToCopy,
    Activity & act,
    RepairFlag repair,
    CheckSigsFlag checkSigs,
    const std::function<Hash(const StorePath &, const Source &)> & narHash)
{
    using PathWithInfo = PathsSource::value_type;

    auto copyPath = [&](PathWithInfo & value, auto & nrDone, auto & nrFailed, auto & nrRunning, auto showProgress) {
        checkInterrupt();
        auto info = value.first;
        info.ultimate = false;
        /* A source may own a coroutine stack or connection lock. Release it
           when this action finishes, even if the path is already valid. */
        auto source = std::move(value.second);
        std::function<Hash()> hashForPath;
        if (narHash)
            hashForPath = [&] { return narHash(info.path, *source); };

        bool failed = false;
        if (repair || !isValidPath(info.path)) {
            MaintainCount mc(nrRunning);
            showProgress();
            try {
                addToStore(info, *source, repair, checkSigs, hashForPath);
            } catch (Error & e) {
                nrFailed++;
                if (!settings.getWorkerSettings().keepGoing)
                    throw;
                printMsg(lvlError, "could not copy %s: %s", printStorePath(info.path), e.what());
                failed = true;
            }
        }
        if (!failed)
            nrDone++;
        showProgress();
    };

    /* A singleton has no inter-path order or parallel work. Its counters
       belong to this thread; only the graph needs atomic counters. */
    if (pathsToCopy.size() == 1) {
        auto & value = pathsToCopy.front();
        act.setExpected(actCopyPath, value.first.narSize);
        size_t nrDone = 0, nrFailed = 0;
        uint64_t nrRunning = 0;
        ReceiveInterrupts receiveInterrupts;
        copyPath(value, nrDone, nrFailed, nrRunning, [&] { act.progress(nrDone, 1, nrRunning, nrFailed); });
        return;
    }

    std::atomic<size_t> nrDone{0};
    std::atomic<size_t> nrFailed{0};
    std::atomic<uint64_t> nrRunning{0};

    uint64_t bytesExpected = 0;
    using Transfer = std::pair<StorePath, size_t>;
    std::set<Transfer> transfers;
    for (size_t i = 0; i != pathsToCopy.size(); ++i) {
        bytesExpected += pathsToCopy[i].first.narSize;
        transfers.emplace(pathsToCopy[i].first.path, i);
    }
    act.setExpected(actCopyPath, bytesExpected);
    auto showProgress = [&, nrTotal = pathsToCopy.size()]() { act.progress(nrDone, nrTotal, nrRunning, nrFailed); };

    processGraph<Transfer>(
        transfers,
        [&](const Transfer & transfer) {
            std::set<Transfer> dependencies;
            auto here = transfers.find(transfer);
            if (here != transfers.begin() && std::prev(here)->first == transfer.first)
                dependencies.insert(*std::prev(here));
            auto & info = pathsToCopy[transfer.second].first;
            if (repair || !isValidPath(info.path))
                for (auto & ref : info.references)
                    if (ref != transfer.first) {
                        auto first = transfers.lower_bound({ref, 0});
                        if (first != transfers.end() && first->first == ref && (!repair || !isValidPath(ref)))
                            dependencies.insert(*first);
                    }
            return dependencies;
        },
        [&](const Transfer & transfer) {
            copyPath(pathsToCopy[transfer.second], nrDone, nrFailed, nrRunning, showProgress);
        });
}

/* Compute the object hash and content address in one walk. The address
   hashes the NAR for NAR addressing, regular-file bytes for flat/text,
   or uses the root's object hash for Git. */
ValidPathInfo Store::addToStoreSlow(
    std::string_view name,
    const SourcePath & srcPath,
    ContentAddressMethod method,
    HashAlgorithm hashAlgo,
    const StorePathSet & references,
    std::optional<Hash> expectedCAHash)
{
    checkIngestionAlgorithm(method.getFileIngestionMethod(), hashAlgo);

    bool caFromNar = method == ContentAddressMethod::Raw::NixArchive;
    HashSink caHashSink{hashAlgo};

    /* Note that fileSink and narSink must never both write to caHashSink;
       they do not, since the former is used only in the flat case and the
       latter feeds it only under the NAR method. */
    RegularFileSink fileSink{caHashSink};
    LambdaSink narSink{[&](std::string_view data) {
        if (caFromNar)
            caHashSink(data);
    }};

    /* Functionally, this means that fileSource will yield the content of
       srcPath. The fact that we use scratchpadSink as a temporary buffer here
       is an implementation detail. */
    auto fileSource = sinkToSource([&](Sink & scratchpadSink) { srcPath.dumpPath(scratchpadSink); });

    /* tapped provides the same data as fileSource, but we also write all the
       information to narSink. */
    TeeSource tapped{*fileSource, narSink};

    /* The object hash is computed for every method; the flat method's
       content hash needs the file's bytes as well, through `fileSink`, so
       there the parse is delivered to both. */
    HashingVisitor objectVisitor;
    ObjectHashSink objectSink{objectVisitor};
    TeeFileSystemObjectSink flatTee{fileSink, objectSink};
    auto & parseSink = method.getFileIngestionMethod() == FileIngestionMethod::Flat
                           ? (FileSystemObjectSink &) flatTee
                           : (FileSystemObjectSink &) objectSink; // for recursive or git we do recursive

    /* The information that flows from tapped (besides being replicated in
       narSink), is now put in parseSink. */
    parseDump(parseSink, tapped);

    /* We extract the result of the computation from the sinks by calling
       finish; the NAR size is the object sink's. */
    auto object = objectSink.finish();
    auto objectHash = ObjectHash::of(object.root);
    auto hash = method == ContentAddressMethod::Raw::Git ? objectHash.hash : caHashSink.finish().hash;

    if (expectedCAHash && expectedCAHash != hash)
        throw Error("hash mismatch for '%s'", srcPath);

    auto info = ValidPathInfo::makeFromCA(
        *this,
        name,
        ContentAddressWithReferences::fromParts(
            method,
            hash,
            {
                .others = references,
                .self = false,
            }),
        objectHash);
    info.narSize = object.narSize.value();

    if (!isValidPath(info.path)) {
        /* For a peer speaking only the old forms: the NAR hash computed
           above when it is the address, else one more walk. */
        auto source = sinkToSource([&](Sink & scratchpadSink) { srcPath.dumpPath(scratchpadSink); });
        addToStore(info, *source, NoRepair, CheckSigs, [&]() -> Hash {
            if (caFromNar && hashAlgo == HashAlgorithm::SHA256)
                return hash;
            HashSink sink{HashAlgorithm::SHA256};
            srcPath.dumpPath(sink);
            return sink.finish().hash;
        });
    }

    return info;
}

ObjectHash Store::queryObjectHash(const ValidPathInfo & info)
{
    if (info.objectHash)
        return *info.objectHash;
    /* The peer described the path by its NAR hash alone (the shim): the
       object hash is the walk's, over the object it holds. */
    return ObjectHash::of(objectHashOf(*requireStoreObjectAccessor(info.path), CanonPath::root).root);
}

Hash narHashOf(Store & store, const StorePath & path)
{
    HashSink sink{HashAlgorithm::SHA256};
    store.narFromPath(path, sink);
    return sink.finish().hash;
}

std::optional<std::pair<std::string, std::string>>
Store::contentMismatch(const ValidPathInfo & info, SourceAccessor * accessor)
{
    if (info.objectHash) {
        auto current = ObjectHash::of(
            objectHashOf(
                accessor ? *accessor : *requireStoreObjectAccessor(info.path, /*requireValidPath=*/false),
                CanonPath::root)
                .root);
        if (current == *info.objectHash)
            return std::nullopt;
        return std::pair{info.objectHash->render(), current.render()};
    }
    if (info.assertedNarHash) {
        HashSink sink(info.assertedNarHash->algo);
        narFromPath(info.path, sink);
        auto current = sink.finish().hash;
        if (current == *info.assertedNarHash)
            return std::nullopt;
        return std::pair{
            info.assertedNarHash->to_string(HashFormat::Nix32, true), current.to_string(HashFormat::Nix32, true)};
    }
    throw Error("path '%s' has neither an object hash nor a NAR hash to check", printStorePath(info.path));
}

void Store::narFromPath(const StorePath & path, Sink & sink)
{
    auto accessor = requireStoreObjectAccessor(path);
    SourcePath sourcePath{accessor};
    dumpPath(sourcePath, sink, FileSerialisationMethod::NixArchive);
}

StringSet Store::Config::getDefaultSystemFeatures()
{
    auto res = settings.systemFeatures.get();

    if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations))
        res.insert("ca-derivations");

    if (experimentalFeatureSettings.isEnabled(Xp::RecursiveNix)) {
        res.insert("recursive-nix");
    }

    if (experimentalFeatureSettings.isEnabled(Xp::DynamicDerivations)) {
        res.insert(std::string{drvFeatureBuilderRpcV0});
    }

    return res;
}

Store::Store(const Store::Config & config)
    : StoreDirConfig{config}
    , config{config}
    , pathInfoCache(
          config.pathInfoCacheSize
              ? std::make_shared<decltype(pathInfoCache)::element_type>((size_t) config.pathInfoCacheSize)
              : nullptr)
{
    assertLibStoreInitialized();
}

StoreReference StoreConfig::getReference() const
{
    return {.variant = StoreReference::Auto{}};
}

bool StoreConfig::getReadOnly() const
{
    return settings.readOnlyMode;
}

bool Store::PathInfoCacheValue::isKnownNow(const NarInfoDiskCacheSettings & settings) const
{
    std::chrono::duration ttl =
        didExist() ? std::chrono::seconds(settings.ttlPositive) : std::chrono::seconds(settings.ttlNegative);

    return std::chrono::steady_clock::now() < time_point + ttl;
}

void Store::invalidatePathInfoCacheFor(const StorePath & path)
{
    if (pathInfoCache)
        pathInfoCache->lock()->erase(path);
}

std::map<std::string, std::optional<StorePath>> Store::queryStaticPartialDerivationOutputMap(const StorePath & path)
{
    std::map<std::string, std::optional<StorePath>> outputs;
    auto drv = readInvalidDerivation(path);
    for (auto & [outputName, output] : outputsAndOptPaths(drv, *this)) {
        outputs.emplace(outputName, output.second);
    }
    return outputs;
}

std::optional<StorePath>
Store::queryStaticPartialDerivationOutput(const StorePath & path, const std::string & outputName)
{
    auto drv = readInvalidDerivation(path);
    auto outputs = outputsAndOptPaths(drv, *this);
    auto it = outputs.find(outputName);
    if (it == outputs.end())
        throw Error("derivation '%s' does not have an output named '%s'", printStorePath(path), outputName);
    return it->second.second;
}

std::map<std::string, std::optional<StorePath>>
Store::queryPartialDerivationOutputMap(const StorePath & path, Store * evalStore_)
{
    auto & evalStore = evalStore_ ? *evalStore_ : *this;

    auto outputs = evalStore.queryStaticPartialDerivationOutputMap(path);

    if (!experimentalFeatureSettings.isEnabled(Xp::CaDerivations))
        return outputs;

    auto drv = evalStore.readInvalidDerivation(path);
    queryPartialDerivationOutputMapCA(*this, path, drv, outputs);

    return outputs;
}

OutputPathMap Store::queryDerivationOutputMap(const StorePath & path, Store * evalStore)
{
    auto resp = queryPartialDerivationOutputMap(path, evalStore);
    OutputPathMap result;
    for (auto & [outName, optOutPath] : resp) {
        if (!optOutPath)
            throw MissingRealisation(*this, path, outName);
        result.insert_or_assign(outName, *optOutPath);
    }
    return result;
}

StorePathSet Store::queryDerivationOutputs(const StorePath & path)
{
    auto outputMap = nix::deepQueryDerivationOutputMap(*this, path);
    StorePathSet outputPaths;
    for (auto & i : outputMap) {
        outputPaths.emplace(std::move(i.second));
    }
    return outputPaths;
}

StorePathSet Store::querySubstitutablePaths(const StorePathSet & paths)
{
    if (!settings.getWorkerSettings().useSubstitutes)
        return StorePathSet();

    StorePathSet remaining;
    for (auto & i : paths)
        remaining.insert(i);

    StorePathSet res;

    for (auto & sub : getDefaultSubstituters()) {
        if (remaining.empty())
            break;
        if (sub->storeDir != storeDir)
            continue;
        if (!sub->config.wantMassQuery)
            continue;

        auto valid = sub->queryValidPaths(remaining);

        StorePathSet remaining2;
        for (auto & path : remaining)
            if (valid.count(path))
                res.insert(path);
            else
                remaining2.insert(path);

        std::swap(remaining, remaining2);
    }

    return res;
}

/* A memory-cache description must answer its full path key. The disk cache
   is keyed only by hash; its canonical name still needs admission here. */
static bool goodStorePath(const StorePath & expected, const StorePath & actual)
{
    return expected.hashPart() == actual.hashPart()
           && (expected.name() == Store::MissingName || expected.name() == actual.name());
}

bool Store::isValidPath(const StorePath & storePath)
{
    if (pathInfoCache) {
        auto res = pathInfoCache->lock()->get(storePath);
        if (res && res->isKnownNow(settings.getNarInfoDiskCacheSettings()))
            return res->didExist();
    }

    if (diskCache) {
        auto res = diskCache->lookupNarInfo(
            config.getReference().render(/*FIXME withParams=*/false), std::string(storePath.hashPart()));
        if (res.first != NarInfoDiskCache::oUnknown) {
            bool valid = res.first == NarInfoDiskCache::oValid && goodStorePath(storePath, res.second->path);
            if (pathInfoCache)
                pathInfoCache->lock()->upsert(
                    storePath, valid ? PathInfoCacheValue{.value = res.second} : PathInfoCacheValue{});
            return valid;
        }
    }

    /* A full-path miss need not mean its hash is absent: another name may
       be valid. Hash-indexed backends cache their metadata misses themselves. */
    return isValidPathUncached(storePath);
}

/* Default implementation for stores that only implement
   queryPathInfoUncached(). */
bool Store::isValidPathUncached(const StorePath & path)
{
    try {
        queryPathInfoAfterCacheMiss(path);
        return true;
    } catch (InvalidPath &) {
        return false;
    }
}

ref<const ValidPathInfo> Store::queryPathInfo(const StorePath & storePath)
{
    /* A cached description is already a synchronous result. */
    if (auto info = queryPathInfoFromClientCache(storePath); info.has_value()) {
        if (*info)
            return ref(*info);
        throw InvalidPath("path '%s' is not valid", printStorePath(storePath));
    }

    return queryPathInfoAfterCacheMiss(storePath);
}

ref<const ValidPathInfo> Store::queryPathInfoAfterCacheMiss(const StorePath & storePath)
{
    std::promise<Callback<ref<const ValidPathInfo>>::Result> promise;

    queryPathInfoAfterCacheMiss(storePath, {[&](Callback<ref<const ValidPathInfo>>::Result result) {
                                    promise.set_value(std::move(result));
                                }});

    return promise.get_future().get().value();
}

std::optional<std::shared_ptr<const ValidPathInfo>> Store::queryPathInfoFromClientCache(const StorePath & storePath)
{
    if (pathInfoCache) {
        auto res = pathInfoCache->lock()->get(storePath);
        if (res && res->isKnownNow(settings.getNarInfoDiskCacheSettings()))
            if (auto info = std::get_if<std::shared_ptr<const ValidPathInfo>>(&res->value))
                return *info;
        /* Validity without a description cannot answer this query. */
    }

    if (diskCache) {
        auto hashPart = std::string(storePath.hashPart());
        auto res = diskCache->lookupNarInfo(config.getReference().render(/*FIXME withParams=*/false), hashPart);
        if (res.first != NarInfoDiskCache::oUnknown) {
            bool valid = res.first == NarInfoDiskCache::oValid && goodStorePath(storePath, res.second->path);
            std::shared_ptr<const ValidPathInfo> info = valid ? res.second : nullptr;
            if (pathInfoCache)
                pathInfoCache->lock()->upsert(storePath, PathInfoCacheValue{.value = info});
            return std::make_optional(std::move(info));
        }
    }

    return std::nullopt;
}

void Store::queryPathInfo(const StorePath & storePath, Callback<ref<const ValidPathInfo>> callback) noexcept
{
    try {
        auto r = queryPathInfoFromClientCache(storePath);
        if (r.has_value()) {
            std::shared_ptr<const ValidPathInfo> & info = *r;
            if (info)
                return callback(ref(info));
            else
                throw InvalidPath("path '%s' is not valid", printStorePath(storePath));
        }
    } catch (...) {
        return callback.rethrow();
    }

    queryPathInfoAfterCacheMiss(storePath, std::move(callback));
}

void Store::queryPathInfoAfterCacheMiss(
    const StorePath & storePath, Callback<ref<const ValidPathInfo>> && callback) noexcept
{
    auto callbackPtr = std::make_shared<Callback<ref<const ValidPathInfo>>>(std::move(callback));

    queryPathInfoUncached(
        storePath, {[this, storePath, callbackPtr](Callback<std::shared_ptr<const ValidPathInfo>>::Result fut) {
            try {
                auto info = std::move(fut).value();
                bool valid = info && goodStorePath(storePath, info->path);

                /* A matching hash describes its canonical name even if this
                   request rejected it. A null result only proves this full
                   path absent; hash-indexed backends publish their own misses. */
                if (diskCache && info && storePath.hashPart() == info->path.hashPart())
                    diskCache->upsertNarInfo(
                        config.getReference().render(/*FIXME withParams=*/false),
                        std::string(storePath.hashPart()),
                        info);

                if (!valid)
                    info.reset();
                if (pathInfoCache)
                    pathInfoCache->lock()->upsert(storePath, PathInfoCacheValue{.value = info});

                if (!info)
                    throw InvalidPath("path '%s' is not valid", printStorePath(storePath));

                (*callbackPtr)(ref<const ValidPathInfo>(info));
            } catch (...) {
                callbackPtr->rethrow();
            }
        }});
}

void Store::queryRealisation(
    const DrvOutput & id, Callback<std::shared_ptr<const UnkeyedRealisation>> callback) noexcept
{
    if (!experimentalFeatureSettings.isEnabled(Xp::CaDerivations)) {
        /* then we should not be checking any experimental realisations
           data structures. */
        callback(nullptr);
        return;
    }

    try {
        if (diskCache) {
            auto [cacheOutcome, maybeCachedRealisation] =
                diskCache->lookupRealisation(config.getReference().render(/*FIXME: withParams=*/false), id);
            switch (cacheOutcome) {
            case NarInfoDiskCache::oValid:
                debug("Returning a cached realisation for %s", id.to_string());
                callback(maybeCachedRealisation);
                return;
            case NarInfoDiskCache::oInvalid:
                debug("Returning a cached missing realisation for %s", id.to_string());
                callback(nullptr);
                return;
            case NarInfoDiskCache::oUnknown:
                break;
            }
        }
    } catch (...) {
        return callback.rethrow();
    }

    auto callbackPtr = std::make_shared<decltype(callback)>(std::move(callback));

    queryRealisationUncached(id, {[this, id, callbackPtr](Callback<std::shared_ptr<const UnkeyedRealisation>>::Result fut) {
                                 try {
                                     auto info = std::move(fut).value();

                                     if (diskCache) {
                                         if (info)
                                             diskCache->upsertRealisation(
                                                 config.getReference().render(/*FIXME withParams=*/false), {*info, id});
                                         else
                                             diskCache->upsertAbsentRealisation(
                                                 config.getReference().render(/*FIXME withParams=*/false), id);
                                     }

                                     (*callbackPtr)(std::shared_ptr<const UnkeyedRealisation>(info));

                                 } catch (...) {
                                     callbackPtr->rethrow();
                                 }
                             }});
}

std::shared_ptr<const UnkeyedRealisation> Store::queryRealisation(const DrvOutput & id)
{
    using RealPtr = std::shared_ptr<const UnkeyedRealisation>;
    std::promise<Callback<RealPtr>::Result> promise;

    queryRealisation(id, {[&](Callback<RealPtr>::Result result) { promise.set_value(std::move(result)); }});

    return promise.get_future().get().value();
}

void Store::substitutePaths(const StorePathSet & paths)
{
    std::vector<DerivedPath> paths2;
    for (auto & path : paths)
        if (!path.isDerivation())
            paths2.emplace_back(DerivedPath::Opaque{path});
    auto missing = queryMissing(paths2);

    if (!missing.willSubstitute.empty())
        try {
            std::vector<DerivedPath> subs;
            for (auto & p : missing.willSubstitute)
                subs.emplace_back(DerivedPath::Opaque{p});
            getBuilder()->buildPaths(subs, bmNormal);
        } catch (Error & e) {
            logWarning(e.info());
        }
}

StorePathSet Store::queryValidPaths(const StorePathSet & paths, SubstituteFlag maybeSubstitute)
{
    struct State
    {
        size_t left;
        StorePathSet valid;
        std::exception_ptr exc;
    };

    Sync<State> state_(State{paths.size(), StorePathSet()});

    std::condition_variable wakeup;
    ThreadPool pool;

    auto doQuery = [&](const StorePath & path) {
        checkInterrupt();
        queryPathInfo(path, {[path, &state_, &wakeup](Callback<ref<const ValidPathInfo>>::Result fut) {
                          bool exists = false;
                          std::exception_ptr newExc{};

                          try {
                              auto info = std::move(fut).value();
                              exists = true;
                          } catch (InvalidPath &) {
                          } catch (...) {
                              newExc = std::current_exception();
                          }

                          auto state(state_.lock());

                          if (exists)
                              state->valid.insert(path);

                          if (newExc)
                              state->exc = newExc;

                          assert(state->left);
                          if (!--state->left)
                              wakeup.notify_one();
                      }});
    };

    for (auto & path : paths)
        pool.enqueue(std::bind(doQuery, path));

    pool.process();

    while (true) {
        auto state(state_.lock());
        if (!state->left) {
            if (state->exc)
                std::rethrow_exception(state->exc);
            return std::move(state->valid);
        }
        state.wait(wakeup);
    }
}

/* Return a string accepted by decodeValidPathInfo() that
   registers the specified paths as valid.  Note: it's the
   responsibility of the caller to provide a closure. */
std::string Store::makeValidityRegistration(const StorePathSet & paths, bool showDerivers, bool showHash)
{
    std::string s = "";

    for (auto & i : paths) {
        s += printStorePath(i) + "\n";

        auto info = queryPathInfo(i);

        if (showHash) {
            /* The object hash's rendering; for a description without one
               (an old peer's), the NAR hash it asserted. */
            s += (info->objectHash ? info->objectHash->render()
                  : info->assertedNarHash
                      ? info->assertedNarHash->to_string(HashFormat::Base16, false)
                      : throw Error("path '%s' has neither an object hash nor a NAR hash", printStorePath(i)))
                 + "\n";
            s += fmt("%1%\n", info->narSize);
        }

        auto deriver = showDerivers && info->deriver ? printStorePath(*info->deriver) : "";
        s += deriver + "\n";

        s += fmt("%1%\n", info->references.size());

        for (auto & j : info->references)
            s += printStorePath(j) + "\n";
    }

    return s;
}

StorePathSet Store::exportReferences(const StorePathSet & storePaths, const StorePathSet & inputPaths)
{
    StorePathSet paths;

    for (auto & storePath : storePaths) {
        if (!inputPaths.count(storePath))
            throw BuildError(
                BuildResult::Failure::InputRejected,
                "cannot export references of path '%s' because it is not in the input closure of the derivation",
                printStorePath(storePath));

        computeFSClosure({storePath}, paths);
    }

    /* If there are derivations in the graph, then include their
       outputs as well.  This is useful if you want to do things
       like passing all build-time dependencies of some path to a
       derivation that builds a NixOS DVD image. */
    auto paths2 = paths;

    for (auto & j : paths2) {
        if (j.isDerivation()) {
            Derivation drv = derivationFromPath(j);
            for (auto & k : outputsAndOptPaths(drv, *this)) {
                if (!k.second.second)
                    /* FIXME: I am confused why we are calling
                       `computeFSClosure` on the output path, rather than
                       derivation itself. That doesn't seem right to me, so I
                       won't try to implemented this for CA derivations. */
                    throw UnimplementedError("exportReferences on CA derivations is not yet implemented");
                computeFSClosure(*k.second.second, paths);
            }
        }
    }

    return paths;
}

static std::string
makeCopyPathMessage(const StoreConfig & srcCfg, const StoreConfig & dstCfg, std::string_view storePath)
{
    auto src = srcCfg.getReference();
    auto dst = dstCfg.getReference();

    auto isShorthand = [](const StoreReference & ref) {
        /* At this point StoreReference **must** be resolved. */
        const auto & specified = std::visit(
            overloaded{
                [](const StoreReference::Auto &) -> const StoreReference::Specified & { unreachable(); },
                [](const StoreReference::Specified & specified) -> const StoreReference::Specified & {
                    return specified;
                }},
            ref.variant);
        const auto & scheme = specified.scheme;
        return (scheme == "local" || scheme == "unix") && specified.authority.empty();
    };

    if (isShorthand(src))
        return fmt("copying path '%s' to '%s'", storePath, dstCfg.getHumanReadableURI());

    if (isShorthand(dst))
        return fmt("copying path '%s' from '%s'", storePath, srcCfg.getHumanReadableURI());

    return fmt(
        "copying path '%s' from '%s' to '%s'", storePath, srcCfg.getHumanReadableURI(), dstCfg.getHumanReadableURI());
}

void copyStorePath(
    Store & srcStore, Store & dstStore, const StorePath & storePath, RepairFlag repair, CheckSigsFlag checkSigs)
{
    /* Bail out early (before starting a download from srcStore) if
       dstStore already has this path. */
    if (!repair && dstStore.isValidPath(storePath))
        return;

    const auto & srcCfg = srcStore.config;
    const auto & dstCfg = dstStore.config;
    auto storePathS = srcStore.printStorePath(storePath);
    Activity act(
        *logger,
        lvlInfo,
        actCopyPath,
        makeCopyPathMessage(srcCfg, dstCfg, storePathS),
        std::to_array<Logger::Field>({storePathS, srcCfg.getHumanReadableURI(), dstCfg.getHumanReadableURI()}));
    PushActivity pact(act.id);

    auto info = srcStore.queryPathInfo(storePath);

    uint64_t total = 0;

    // Prepare the destination description once, retaining the source info otherwise.
    if ((info->ca && info->references.empty()) || info->ultimate) {
        auto info2 = make_ref<ValidPathInfo>(*info);
        if (info->ca && info->references.empty()) {
            info2->path =
                dstStore.makeFixedOutputPathFromCA(info->path.name(), info->contentAddressWithReferences().value());
            if (dstStore.storeDir == srcStore.storeDir)
                assert(info->path == info2->path);
        }
        info2->ultimate = false;
        info = info2;
    }

    if (getEnv("_NIX_TEST_CONCURRENT_SUBSTITUTION"))
        std::this_thread::sleep_for(std::chrono::seconds(1));

    auto source = sinkToSource(
        [&](Sink & sink) {
            LambdaSink progressSink([&](std::string_view data) {
                total += data.size();
                act.progress(total, info->narSize);
            });
            TeeSink tee{sink, progressSink};
            srcStore.narFromPath(storePath, tee);
        },
        [&]() {
            throw EndOfFile(
                "NAR for '%s' fetched from '%s' is incomplete",
                srcStore.printStorePath(storePath),
                srcStore.config.getHumanReadableURI());
        });

    dstStore.addToStore(*info, *source, repair, checkSigs, [&] { return narHashOf(srcStore, storePath); });
}

std::map<StorePath, StorePath> copyPaths(
    Store & srcStore,
    Store & dstStore,
    const RealisedPath::Set & paths,
    RepairFlag repair,
    CheckSigsFlag checkSigs,
    SubstituteFlag substitute)
{
    StorePathSet storePaths;
    std::vector<const Realisation *> realisations;
    for (auto & path : paths) {
        std::visit(
            overloaded{
                [&](const Realisation & realisation) {
                    experimentalFeatureSettings.require(Xp::CaDerivations);
                    realisations.push_back(&realisation);
                    storePaths.insert(realisation.outPath);
                },
                [&](const OpaquePath & op) { storePaths.insert(op.path); },
            },
            path.raw);
    }

    auto pathsMap = copyPaths(srcStore, dstStore, storePaths, repair, checkSigs, substitute);

    try {
        // Copy the realisations. TODO batch this
        for (const auto * realisation : realisations)
            dstStore.registerDrvOutput(*realisation, checkSigs);
    } catch (MissingExperimentalFeature & e) {
        // Don't fail if the remote doesn't support CA derivations is it might
        // not be within our control to change that, and we might still want
        // to at least copy the output paths.
        if (e.missingFeature == Xp::CaDerivations)
            ignoreExceptionExceptInterrupt();
        else
            throw;
    }

    return pathsMap;
}

std::map<StorePath, StorePath> copyPaths(
    Store & srcStore,
    Store & dstStore,
    const StorePathSet & storePaths,
    RepairFlag repair,
    CheckSigsFlag checkSigs,
    SubstituteFlag substitute)
{
    auto valid = dstStore.queryValidPaths(storePaths, repair ? NoSubstitute : substitute);

    StorePathSet missing;
    for (auto & path : storePaths)
        if (repair || !valid.count(path))
            missing.insert(path);

    /* Don't start an activity if there's no work to do. */
    if (missing.empty())
        return {};

    Activity act(*logger, lvlInfo, actCopyPaths, fmt("copying %d paths", missing.size()));

    // In the general case, `addMultipleToStore` requires a sorted list of
    // store paths to add, so sort them right now
    auto sortedMissing = repair ? srcStore.topoSortPathsBy(missing, [&](const StorePath & path) {
        auto references = srcStore.queryPathInfo(path)->references;
        std::erase_if(references, [&](const StorePath & ref) { return valid.contains(ref); });
        return references;
    }) : srcStore.topoSortPaths(missing);

    std::map<StorePath, StorePath> pathsMap;
    for (auto & path : storePaths)
        pathsMap.insert_or_assign(path, path);

    Store::PathsSource pathsToCopy;
    std::map<const Source *, StorePath> sourcePaths;

    auto computeStorePathForDst = [&](const ValidPathInfo & currentPathInfo) -> StorePath {
        auto storePathForSrc = currentPathInfo.path;
        auto storePathForDst = storePathForSrc;
        if (currentPathInfo.ca && currentPathInfo.references.empty()) {
            storePathForDst = dstStore.makeFixedOutputPathFromCA(
                currentPathInfo.path.name(), currentPathInfo.contentAddressWithReferences().value());
            if (dstStore.storeDir == srcStore.storeDir)
                assert(storePathForDst == storePathForSrc);
            if (storePathForDst != storePathForSrc)
                debug(
                    "replaced path '%s' to '%s' for substituter '%s'",
                    srcStore.printStorePath(storePathForSrc),
                    dstStore.printStorePath(storePathForDst),
                    dstStore.config.getHumanReadableURI());
        }
        return storePathForDst;
    };

    for (auto & missingPath : sortedMissing | std::views::reverse) {
        auto info = srcStore.queryPathInfo(missingPath);

        auto storePathForDst = computeStorePathForDst(*info);
        pathsMap.insert_or_assign(missingPath, storePathForDst);

        ValidPathInfo infoForDst = *info;
        infoForDst.path = storePathForDst;

        auto source = sinkToSource([&, narSize = info->narSize](Sink & sink) {
            // We can reasonably assume that the copy will happen whenever we
            // read the path, so log something about that at that point
            uint64_t total = 0;
            const auto & srcCfg = srcStore.config;
            const auto & dstCfg = dstStore.config;
            auto storePathS = srcStore.printStorePath(missingPath);
            Activity act(
                *logger,
                lvlInfo,
                actCopyPath,
                makeCopyPathMessage(srcCfg, dstCfg, storePathS),
                std::to_array<Logger::Field>({storePathS, srcCfg.getHumanReadableURI(), dstCfg.getHumanReadableURI()}));
            PushActivity pact(act.id);

            LambdaSink progressSink([&](std::string_view data) {
                total += data.size();
                act.progress(total, narSize);
            });
            TeeSink tee{sink, progressSink};

            srcStore.narFromPath(missingPath, tee);
        });
        if (storePathForDst != missingPath)
            sourcePaths.emplace(source.get(), missingPath);
        pathsToCopy.emplace_back(std::move(infoForDst), std::move(source));
    }

    dstStore.addMultipleToStore(
        std::move(pathsToCopy), act, repair, checkSigs, [&](const StorePath & path, const Source & source) {
            auto original = sourcePaths.find(&source);
            return narHashOf(srcStore, original == sourcePaths.end() ? path : original->second);
        });

    return pathsMap;
}

void copyClosure(
    Store & srcStore,
    Store & dstStore,
    const RealisedPath::Set & paths,
    RepairFlag repair,
    CheckSigsFlag checkSigs,
    SubstituteFlag substitute,
    bool includeOutputs)
{
    if (&srcStore == &dstStore)
        return;

    StorePathSet closure0;
    for (auto & path : paths) {
        closure0.insert(path.path());
    }

    StorePathSet closure1;
    srcStore.computeFSClosure(closure0, closure1, false, includeOutputs);

    RealisedPath::Set closure = paths;
    for (auto && path : closure1)
        closure.insert({std::move(path)});

    copyPaths(srcStore, dstStore, closure, repair, checkSigs, substitute);
}

void copyClosure(
    Store & srcStore,
    Store & dstStore,
    const StorePathSet & storePaths,
    RepairFlag repair,
    CheckSigsFlag checkSigs,
    SubstituteFlag substitute,
    bool includeOutputs)
{
    if (&srcStore == &dstStore)
        return;

    StorePathSet closure;
    srcStore.computeFSClosure(storePaths, closure, false, includeOutputs);
    copyPaths(srcStore, dstStore, closure, repair, checkSigs, substitute);
}

std::optional<ValidPathInfo>
decodeValidPathInfo(const Store & store, std::istream & str, std::optional<HashResult> hashGiven)
{
    std::string path;
    getline(str, path);
    if (str.eof()) {
        return {};
    }
    /* The registration's hash line is the object hash's rendering when a
       Nix of this version wrote it, and a NAR hash when an older one did:
       the first is the row's value, the second an assertion the caller's
       walk verifies while computing the object hash. */
    std::optional<ObjectHash> objectHash;
    std::optional<Hash> assertedNarHash;
    uint64_t narSize;
    if (!hashGiven) {
        std::string s;
        getline(str, s);
        if (auto oh = ObjectHash::parse(s))
            objectHash = *oh;
        else
            assertedNarHash = Hash::parseAny(s, HashAlgorithm::SHA256);
        getline(str, s);
        auto size = string2Int<uint64_t>(s);
        if (!size)
            throw Error("number expected");
        narSize = *size;
    } else {
        assertedNarHash = hashGiven->hash;
        narSize = hashGiven->numBytesDigested;
    }
    ValidPathInfo info(store.parseStorePath(path), {store, objectHash});
    info.assertedNarHash = assertedNarHash;
    info.narSize = narSize;
    std::string deriver;
    getline(str, deriver);
    if (deriver != "")
        info.deriver = store.parseStorePath(deriver);
    std::string s;
    getline(str, s);
    auto n = string2Int<int>(s);
    if (!n)
        throw Error("number expected");
    while ((*n)--) {
        getline(str, s);
        info.references.insert(store.parseStorePath(s));
    }
    if (!str || str.eof())
        throw Error("missing input");
    return std::optional<ValidPathInfo>(std::move(info));
}

Derivation Store::derivationFromPath(const StorePath & drvPath)
{
    getBuilder()->ensurePath(drvPath);
    return readDerivation(drvPath);
}

static Derivation readDerivationCommon(Store & store, const StorePath & drvPath, bool requireValidPath)
{
    auto accessor = store.requireStoreObjectAccessor(drvPath, requireValidPath);
    auto contents = accessor->readFile(CanonPath::root);

    try {
        /* Special case for an empty file to show the user a better message */
        if (contents.empty())
            throw FormatError("file is empty (possible filesystem corruption)");

        return derivation::parse(store, std::move(contents), Derivation::nameFromPath(drvPath));
    } catch (FormatError & e) {
        throw Error("error parsing derivation '%s': %s", store.printStorePath(drvPath), e.message());
    }
}

std::optional<StorePath> Store::getBuildDerivationPath(const StorePath & path)
{

    if (!path.isDerivation()) {
        try {
            auto info = queryPathInfo(path);
            if (!info->deriver)
                return std::nullopt;
            return *info->deriver;
        } catch (InvalidPath &) {
            return std::nullopt;
        }
    }

    if (!experimentalFeatureSettings.isEnabled(Xp::CaDerivations) || !isValidPath(path))
        return path;

    auto drv = readDerivation(path);
    if (!type(drv).hasKnownOutputPaths()) {
        // The build log is actually attached to the corresponding
        // resolved derivation, so we need to get it first
        auto resolvedDrv = tryResolve(drv, *this);
        if (resolvedDrv)
            return nix::computeStorePath(*this, unresolve(*resolvedDrv));
    }

    return path;
}

Derivation Store::readDerivation(const StorePath & drvPath)
{
    return readDerivationCommon(*this, drvPath, true);
}

Derivation Store::readInvalidDerivation(const StorePath & drvPath)
{
    return readDerivationCommon(*this, drvPath, false);
}

void Store::signPathInfo(ValidPathInfo & info)
{
    // FIXME: keep secret keys in memory.

    auto secretKeyFiles = settings.secretKeyFiles;

    for (auto & secretKeyFile : secretKeyFiles.get()) {
        SecretKey secretKey(readFile(secretKeyFile));
        LocalSigner signer(std::move(secretKey));
        info.sign(*this, signer);
    }
}

void Store::signRealisation(Realisation & realisation)
{
    // FIXME: keep secret keys in memory.

    auto secretKeyFiles = settings.secretKeyFiles;

    for (auto & secretKeyFile : secretKeyFiles.get()) {
        SecretKey secretKey(readFile(secretKeyFile));
        LocalSigner signer(std::move(secretKey));
        realisation.sign(realisation.id, signer);
    }
}

const std::filesystem::path & StoreConfig::getStateDir() const
{
    return settings.nixStateDir;
}

const std::filesystem::path & StoreConfig::getLogDir() const
{
    static std::filesystem::path logDir = [] {
        return getEnvOsNonEmpty(OS_STR("NIX_LOG_DIR"))
            .transform([](auto && s) { return std::filesystem::path(s); })
            .or_else([]() -> std::optional<std::filesystem::path> {
#ifdef _WIN32
#  ifdef NIX_LOG_DIR
#    error "NIX_LOG_DIR should not be defined on Windows"
#  endif
                return windows::known_folders::getProgramData() / "nix" / "log";
#else
                return NIX_LOG_DIR;
#endif
            })
            .transform([](auto && s) { return canonPath(s); })
            .value();
    }();
    return logDir;
}

} // namespace nix
