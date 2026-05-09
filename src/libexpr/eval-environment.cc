#include "nix/expr/eval-environment/environment.hh"
#include "eval-environment/private-errors.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-environment/authority-internal.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "eval-trace/fiber/fiber-scheduler.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/input-cache.hh"
#include "nix/fetchers/registry.hh"
#include "nix/fetchers/tarball.hh"
#include "nix/store/derivations.hh"
#include "nix/store/downstream-placeholder.hh"
#include "nix/store/realisation.hh"
#include "nix/store/store-api.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/error.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/util/strings-inline.hh"
#include "nix/util/url.hh"
#include "nix/util/util.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <filesystem>

namespace nix {

// ── Auto-dispatch priority chain template definitions ─────────────────
//
// Defined here (not in the header) so eval_trace::TraceAccess's full
// type stays out of environment.hh's public dependency closure. Only
// this TU uses them.

template<typename F>
decltype(auto) EvalEnvironment::dispatchBoundAccessDetached(F && fn)
{
    if (auto session = tryBindCurrentEvalSession())
        return std::forward<F>(fn)(*session);
    if (auto access = eval_trace::TraceAccess::current())
        return std::forward<F>(fn)(*access);
    auto detached = openDetachedEffectScope();
    return std::forward<F>(fn)(detached);
}

template<typename F>
decltype(auto) EvalEnvironment::dispatchBoundDetached(F && fn)
{
    if (auto session = tryBindCurrentEvalSession())
        return std::forward<F>(fn)(*session);
    auto detached = openDetachedEffectScope();
    return std::forward<F>(fn)(detached);
}

template<typename F>
decltype(auto) EvalEnvironment::dispatchBoundAccessObserveOnly(F && fn)
{
    if (auto session = tryBindCurrentEvalSession())
        return std::forward<F>(fn)(*session);
    if (auto access = eval_trace::TraceAccess::current())
        return std::forward<F>(fn)(*access);
    return std::forward<F>(fn)(observeOnly);
}

namespace {

struct EvalEnvironmentState {
    EvalEnvironmentAuthority authority;

    explicit EvalEnvironmentState(EvalEnvironmentAuthority authority)
        : authority(std::move(authority))
    {
    }
};

EvalEnvironmentState & requireState(const std::shared_ptr<void> & state)
{
    return *std::static_pointer_cast<EvalEnvironmentState>(state);
}

std::optional<SourcePath> tryAnchorAccessorAtPhysicalRoot(
    const ref<SourceAccessor> & accessor,
    std::optional<std::filesystem::path> physicalPath)
{
    if (!physicalPath || !physicalPath->is_absolute())
        return std::nullopt;

    auto mountPoint = CanonPath(physicalPath->string());
    std::map<CanonPath, ref<SourceAccessor>> mounts;
    mounts.emplace(CanonPath::root, makeEmptySourceAccessor());
    mounts.emplace(mountPoint, accessor);

    auto mountedAccessor = makeMountedSourceAccessor(std::move(mounts));
    mountedAccessor->fingerprint = accessor->fingerprint;
    return SourcePath{
        mountedAccessor,
        std::move(mountPoint),
    };
}

SourcePath computePhase1FetchRoot(
    const EvalEnvironmentAuthority & authority,
    const fetchers::Input & requestedInput,
    const fetchers::Input & resolvedInput,
    const ref<SourceAccessor> & accessor)
{
    (void) resolvedInput;
    if (requestedInput.getSourcePath()
        && !requestedInput.isLocked(authority.fetchSettings)
        && !requestedInput.getRef()
        && !requestedInput.getRev())
        if (auto anchored = tryAnchorAccessorAtPhysicalRoot(accessor, requestedInput.getSourcePath()))
            return *anchored;
    return SourcePath(accessor);
}

template<typename F>
decltype(auto) withRecordingAccess(const ref<eval_trace::TraceSession> & session, F && f)
{
    if (auto * currentSession = eval_trace::currentTraceSession();
        currentSession == &*session)
        if (auto currentAccess = eval_trace::TraceAccess::current())
            return std::forward<F>(f)(*currentAccess);

    return session->withDepCaptureScope([&](const eval_trace::TraceAccess & access) {
        return std::forward<F>(f)(access);
    });
}

NixStringContext makePublishedStorePathContext(const StorePath & storePath)
{
    NixStringContext context;
    context.insert(NixStringContextElem::Opaque{.path = storePath});
    return context;
}

} // namespace (anon)

template<typename F>
decltype(auto) EvalEnvironment::boundRecord(BoundEffectScope & session, F && fn)
{
    return withRecordingAccess(session.session_, std::forward<F>(fn));
}

void allowPath(const EvalEnvironmentAuthority & authority, const StorePath & storePath)
{
    if (auto rootFS = authority.rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
        rootFS->allowPrefix(CanonPath(authority.store->printStorePath(storePath)));
}

void allowClosure(const EvalEnvironmentAuthority & authority, const StorePath & root)
{
    auto allowList = authority.rootFS.dynamic_pointer_cast<AllowListSourceAccessor>();
    if (!allowList)
        return;

    StorePathSet closure;
    authority.store->computeFSClosure(root, closure);
    for (const auto & path : closure)
        allowList->allowPrefix(CanonPath(authority.store->printStorePath(path)));
}

bool isAllowedURI(std::string_view uri, const Strings & allowedUris)
{
    auto isJustSchemePrefix = [](std::string_view prefix) {
        return !prefix.empty() && prefix[prefix.size() - 1] == ':'
            && isValidSchemeName(prefix.substr(0, prefix.size() - 1));
    };
    for (const auto & prefix : allowedUris) {
        if (uri == prefix
            || (uri.size() > prefix.size()
                && !prefix.empty()
                && hasPrefix(uri, prefix)
                && (prefix[prefix.size() - 1] == '/'
                    || uri[prefix.size()] == '/'
                    || isJustSchemePrefix(prefix))))
            return true;
    }

    return false;
}

namespace {

void allowPathLegacy(const EvalEnvironmentAuthority & authority, const std::string & path)
{
    if (auto rootFS = authority.rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
        rootFS->allowPrefix(CanonPath(path));
}

void checkURI(const EvalEnvironmentAuthority & authority, std::string_view uri0)
{
    if (!authority.evalSettings.restrictEval)
        return;

    if (isAllowedURI(uri0, authority.evalSettings.allowedUris.get()))
        return;

    std::filesystem::path path{std::string(uri0)};
    if (path.is_absolute()) {
        if (auto rootFS = authority.rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
            rootFS->checkAccess(CanonPath(path.string()));
        return;
    }

    ParsedURL uri = parseURL(uri0);
    if (uri.scheme == "file") {
        if (auto rootFS = authority.rootFS.dynamic_pointer_cast<AllowListSourceAccessor>())
            rootFS->checkAccess(CanonPath(urlPathToPath(uri.path).string()));
        return;
    }

    throw RestrictedPathError("access to URI '%1%' is forbidden in restricted mode", uri0);
}

struct MountedStoreResult
{
    StorePath storePath;
    RuntimeRootNarHash narHash;
    FinalizedLockedInput finalizedInput;
};

struct DeferredRecordedDep
{
    SourcePath path;
    DepHashValue hash;
    CanonicalQueryKind kind;
    std::optional<PathObject> origin;
    std::string storeName;
};

struct CopyPathToStoreOutcome
{
    StorePath storePath;
    std::vector<DeferredRecordedDep> deferredDeps;
};

MountedStoreResult mountInput(
    const EvalEnvironmentAuthority & authority,
    const ResolvedLockedInput & lockedInput,
    const fetchers::Input & originalInput,
    ref<SourceAccessor> accessor,
    std::optional<std::string> materializationFingerprint = std::nullopt)
{
    /* Use `FetchMode::Copy` here, NOT `FetchMode::DryRun` despite AD-2
       rule 3's suggestion to flip. Master's "Don't copy flakes to the
       store unnecessarily" optimisation (lazy mount + `ensureLazyPathCopied`
       on demand) requires every downstream path that reads flake bytes to
       call `ensureLazyPathCopied` before using the store path.  Our flake
       and fetcher test suites assume eager materialisation via this
       mountInput free function; flipping to DryRun breaks 12 tests
       (fetchGit*, fetchTree-file, eval-trace-recovery, flake inputs,
       packed-refs-no-cache, etc.).  Keeping `Copy` here is the
       conservative choice: correctness > the performance win from laziness,
       until our fetcher-path call sites are audited to invoke
       `ensureLazyPathCopied` where master does. */
    auto [storePath, narHash] =
        fetchToStore2(
            authority.fetchSettings,
            *authority.store,
            accessor,
            FetchMode::Copy,
            lockedInput.value->getName(),
            ContentAddressMethod::Raw::NixArchive,
            nullptr,
            NoRepair,
            std::move(materializationFingerprint));

    allowPath(authority, storePath);
    authority.storeFS->mount(CanonPath(authority.store->printStorePath(storePath)), accessor);

    if (originalInput.getNarHash() && narHash != *originalInput.getNarHash()) {
        throw Error(
            (unsigned int) 102,
            "NAR hash mismatch in input '%s', expected '%s' but got '%s'",
            originalInput.to_string(),
            narHash.to_string(HashFormat::SRI, true),
            originalInput.getNarHash()->to_string(HashFormat::SRI, true));
    }

    // Pure derivation: produce a finalized input with narHash + __final.
    // Copy the resolved input, add finalization attrs, clear stale cache.
    auto finalized = *lockedInput.value;
    finalized.attrs.insert_or_assign("narHash", narHash.to_string(HashFormat::SRI, true));
    finalized.attrs.insert_or_assign("__final", Explicit<bool>(true));
    finalized.cachedFingerprint.reset();

    return MountedStoreResult{
        .storePath = std::move(storePath),
        .narHash = RuntimeRootNarHash{narHash},
        .finalizedInput = FinalizedLockedInput{
            std::make_shared<const fetchers::Input>(std::move(finalized))},
    };
}

SourcePath rootPath(const EvalEnvironmentAuthority & authority, std::string_view path)
{
    return {authority.rootFS, CanonPath(absPath(path).string())};
}

SourcePath storePathSource(const EvalEnvironmentAuthority & authority, const StorePath & path)
{
    return {authority.storeFS.cast<SourceAccessor>(), CanonPath(authority.store->printStorePath(path))};
}

bool sameLogicalStore(const EvalEnvironmentAuthority & authority)
{
    return authority.store->storeDir == authority.buildStore->storeDir;
}

std::optional<TraceSessionReuseSlotKey> sessionReuseKey(const FlakeSessionReuseDecision & reuse)
{
    return std::visit([](const auto & value) -> std::optional<TraceSessionReuseSlotKey> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<T, NoSessionReuseRequested>)
            return std::nullopt;
        else
            return TraceSessionReuseSlotKey{.value = value};
    }, reuse);
}

std::optional<TraceSessionReuseSlotKey> sessionReuseKey(const FileEvalSessionReuseDecision & reuse)
{
    return std::visit([](const auto & value) -> std::optional<TraceSessionReuseSlotKey> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<T, NoSessionReuseRequested> || std::same_as<T, FileEvalReuseKeyUncacheableReason>)
            return std::nullopt;
        else
            return TraceSessionReuseSlotKey{.value = value.value};
    }, reuse);
}

void recordStorePathAvailability(const eval_trace::TraceAccess & access, const StorePath & storePath)
{
    access.record(
        DepSource::makeAbsolute(),
        StorePathAvailabilityDepKey{
            .storePath = storePath,
        },
        DepHashValue(std::string("valid")));
}

RuntimeFetchIdentityDepKey computeRuntimeFetchIdentityKey(
    const fetchers::Input & input)
{
    return RuntimeFetchIdentityDepKey{
        .inputAttrs = input.toAttrs(),
    };
}

void recordRuntimeFetchIdentity(
    const eval_trace::TraceAccess & access,
    const RuntimeFetchIdentityDepKey & key,
    std::string_view storePath)
{
    access.record(
        DepSource::makeAbsolute(),
        key,
        DepHashValue(std::string(storePath)));
}

void recordEnvVarObservation(const eval_trace::TraceAccess & access, const EnvVarObservation & observation)
{
    auto hash = depHash(observation.value);
    access.record(
        CanonicalQueryKind::EnvironmentLookup,
        DepSource::makeAbsolute(),
        SimpleDepKeyAtom{observation.name},
        hash);
}

void recordSessionSystemObservation(
    const eval_trace::TraceAccess & access,
    const SessionSystemObservation & observation)
{
    auto hash = depHash(observation.currentSystem.value);
    access.record(
        CanonicalQueryKind::SessionSystemValue,
        DepSource::makeAbsolute(),
        SimpleDepKeyAtom{"currentSystem"},
        hash);
}

void recordLookupPathObservation(
    const eval_trace::TraceAccess & access,
    const LookupPathObservation & observation)
{
    for (const auto & envLookup : observation.envLookups)
        recordEnvVarObservation(access, envLookup);
}

void recordContextRealisationObservation(
    const eval_trace::TraceAccess & access,
    const ContextRealisationObservation & observation)
{
    for (const auto & path : observation.referencedStorePaths)
        recordStorePathAvailability(access, path);
}


void recordPathStatusObservation(
    const eval_trace::TraceAccess & access,
    const PathStatusObservation & observation)
{
    DepHashValue hashValue = observation.stat
        ? DepHashValue(fmt("type:%d", static_cast<int>(observation.stat->type)))
        : DepHashValue(std::string("missing"));
    recordDep(
        access,
        observation.observedPath,
        hashValue,
        CanonicalQueryKind::ExistenceCheck,
        observation.request.coercedPath.origin);
}

void recordFileReadObservation(
    const eval_trace::TraceAccess & access,
    const FileReadObservation & observation,
    const DepHash & contentHash)
{
    recordDep(
        access,
        observation.observedPath,
        DepHashValue(contentHash),
        CanonicalQueryKind::FileBytes,
        observation.request.coercedPath.origin);
}

void recordDirectoryReadObservation(
    const eval_trace::TraceAccess & access,
    const DirectoryReadObservation & observation)
{
    recordDep(
        access,
        observation.observedPath,
        depHashDirListing(observation.entries),
        CanonicalQueryKind::DirectoryEntries,
        observation.request.coercedPath.origin);
}

void recordPublishedStorePathString(
    const eval_trace::TraceAccess & access,
    const PublishedStorePathString & published)
{
    recordStorePathAvailability(access, published.storePath());
}

void recordDerivedStorePathObservation(
    const eval_trace::TraceAccess & access,
    const DerivedStorePathObservation & observation,
    const Store * store)
{
    if (!observation.storePath)
        return;
    recordDep(
        access,
        observation.observedSourcePath,
        DepHashValue(store->printStorePath(*observation.storePath)),
        CanonicalQueryKind::DerivedStorePath,
        observation.request.sourcePath.origin,
        SimpleDepKeyAtom{observation.request.storeName});
}

void recordDeferredPathDeps(
    const eval_trace::TraceAccess & access,
    const std::vector<DeferredRecordedDep> & deps)
{
    if (!deps.empty()) {
        size_t narCount = 0;
        for (const auto & dep : deps)
            if (dep.kind == CanonicalQueryKind::NarIdentity)
                narCount++;
        if (narCount > 100) {
            auto * ctx = eval_trace::currentFiberDepCtx();
            if (!ctx) ctx = eval_trace::currentStandaloneDepCtx();
            uint32_t epochPos = ctx ? ctx->epochLog.size() : 0;
            // Include the path being copied for identification
            auto pathStr = deps[0].path.to_string();
            debug("eval-env/deps: recording %zu deferred deps (%zu NarIdentity) starting at epoch ~%u for path '%s'",
                deps.size(), narCount, epochPos, pathStr);
        }
    }
    for (const auto & dep : deps) {
        recordDep(
            access,
            dep.path,
            dep.hash,
            dep.kind,
            dep.origin,
            SimpleDepKeyAtom{dep.storeName});
    }
}

void recordGitIdentityObservation(
    const eval_trace::TraceAccess & access,
    const GitIdentityObservation & observation)
{
    if (!observation.hash)
        return;
    // GitRevisionIdentity's governing repo IS itself — the key identifies
    // the repo root.  Intern it so the verifier can cross-reference
    // FileBytes/etc. deps sharing the same governingRepoId.
    auto repoRootAbs = observation.observedRepoRoot.path.abs();
    auto governingRepoId = access.tracingPools().internGoverningRepo(repoRootAbs);
    access.record(
        CanonicalQueryKind::GitRevisionIdentity,
        DepSource::makeAbsolute(),
        SimpleDepKeyAtom{repoRootAbs},
        DepHashValue(DepHash{observation.hash->value}),
        governingRepoId);
}

UnrealizedFullLookupPathEntry makeLookupPathEntryForIdentity(
    const std::optional<LookupPathPrefix> & prefix,
    std::string rawValue)
{
    return buildLookupPathEntrySpec(
        {},
        prefix,
        LookupPathRawValue{.value = std::move(rawValue)});
}

std::vector<UnrealizedFullLookupPathEntry> buildLookupPathEntries(const LookupPath & lookupPath)
{
    std::vector<UnrealizedFullLookupPathEntry> entries;
    entries.reserve(lookupPath.elements.size());

    for (const auto & elem : lookupPath.elements) {
        entries.push_back(buildLookupPathEntrySpec(
            {{}, elem.path.origin},
            elem.prefix.s.empty()
                ? std::nullopt
                : std::make_optional(LookupPathPrefix{.value = elem.prefix.s}),
            LookupPathRawValue{.value = elem.path.s}));
    }

    return entries;
}

StringMap realiseContextImpl(
    const EvalEnvironmentAuthority & authority,
    const NixStringContext & context,
    StorePathSet * maybePathsOut,
    bool isIFD)
{
    std::vector<DerivedPath::Built> drvs;
    StringMap rewrites;
    StorePathSet pathsToCopyAndAllow;
    const bool sameStore = sameLogicalStore(authority);

    for (const auto & c : context) {
        auto ensureValid = [&](const StorePath & path) {
            if (authority.store->isValidPath(path))
                return;
            if (!sameStore && path.isDerivation()) {
                pathsToCopyAndAllow.insert(path);
                return;
            }
            if (path.isDerivation()) {
                try {
                    (void) authority.store->readInvalidDerivation(path);
                    return;
                } catch (Error &) {
                }
            }
            if (!sameStore && authority.buildStore->isValidPath(path)) {
                pathsToCopyAndAllow.insert(path);
                return;
            }
            throw Error("path '%s' is not valid", path.to_string());
        };

        std::visit(overloaded{
            [&](const NixStringContextElem::Built & built) {
                drvs.push_back(DerivedPath::Built{
                    .drvPath = built.drvPath,
                    .outputs = OutputsSpec::Names{built.output},
                });
            },
            [&](const NixStringContextElem::Opaque & opaque) {
                ensureValid(opaque.path);
                if (maybePathsOut)
                    maybePathsOut->emplace(opaque.path);
            },
            [&](const NixStringContextElem::DrvDeep & deep) {
                ensureValid(deep.drvPath);
                if (maybePathsOut)
                    maybePathsOut->emplace(deep.drvPath);
            },
        }, c.raw);
    }

    if (drvs.empty())
        return rewrites;

    if (isIFD) {
        if (!authority.evalSettings.enableImportFromDerivation)
            throw IFDError(
                *authority.evalState,
                "cannot build '%s' during evaluation because the option 'allow-import-from-derivation' is disabled",
                drvs.begin()->to_string(*authority.store));

        if (authority.evalSettings.traceImportFromDerivation)
            warn("built '%1%' during evaluation due to an import from derivation", drvs.begin()->to_string(*authority.store));
    }

    std::vector<DerivedPath> buildReqs;
    buildReqs.reserve(drvs.size());
    for (auto & drv : drvs)
        buildReqs.emplace_back(DerivedPath{drv});
    authority.buildStore->buildPaths(buildReqs, bmNormal, authority.store);

    StorePathSet outputsToCopyAndAllow;
    for (auto & drv : drvs) {
        /* In eval-store mode the drv path itself may live only in the eval
           store, while the realised output map must still be queried from the
           build store. Use the standard helper for output-map resolution so we
           don't mix eval-store and build-store identities by hand. */
        auto outputs = std::visit(
            overloaded{
                [&](const OutputsSpec::All &) {
                    return resolveDerivedPath(*authority.buildStore, drv, &*authority.store);
                },
                [&](const OutputsSpec::Names & names) {
                    OutputPathMap selected;
                    auto resolved = resolveDerivedPath(*authority.buildStore, drv, &*authority.store);
                    for (auto & output : names) {
                        auto * outputPath = get(resolved, output);
                        if (!outputPath)
                            throw Error(
                                "the derivation '%s' doesn't have an output named '%s'",
                                drv.drvPath->to_string(*authority.store),
                                output);
                        selected.insert_or_assign(output, *outputPath);
                    }
                    return selected;
                },
            },
            drv.outputs.raw);

        for (auto & [outputName, outputPath] : outputs) {
            outputsToCopyAndAllow.insert(outputPath);
            pathsToCopyAndAllow.insert(outputPath);
            if (maybePathsOut)
                maybePathsOut->emplace(outputPath);

            if (experimentalFeatureSettings.isEnabled(Xp::CaDerivations)) {
                rewrites.insert_or_assign(
                    DownstreamPlaceholder::fromSingleDerivedPathBuilt(
                        SingleDerivedPath::Built{
                            .drvPath = drv.drvPath,
                            .output = outputName,
                        })
                        .render(),
                    authority.buildStore->printStorePath(outputPath));
            }
        }
    }

    if (!sameStore)
        copyClosure(*authority.buildStore, *authority.store, pathsToCopyAndAllow);

    if (isIFD) {
        for (const auto & path : pathsToCopyAndAllow)
            allowPath(authority, path);
        for (const auto & path : outputsToCopyAndAllow)
            allowClosure(authority, path);
    }

    return rewrites;
}

SourcePath realiseCoercedPath(
    const EvalEnvironmentAuthority & authority,
    const CoercedPathRequest & request,
    std::optional<SymlinkResolution> symlinkResolution = std::nullopt)
{
    auto path = request.path;

    if (!request.context.empty() && path.accessor == authority.rootFS) {
        auto rewrites = realiseContextImpl(
            authority,
            request.context,
            nullptr,
            /* isIFD */ true);
        path = {path.accessor, CanonPath(rewriteStrings(path.path.abs(), rewrites))};
    }

    return symlinkResolution
        ? path.resolveSymlinks(*symlinkResolution)
        : path;
}

std::optional<SourcePath> resolveLookupPathEntry(
    EvalEnvironmentState & state,
    std::string_view rawValue,
    bool initAccessControl,
    LookupPathResolvedEntry * observation,
    RealizedLookupPathIdentity identity)
{
    auto & authority = state.authority;
    auto & cache = *authority.sharedState->lookupPathResolved;
    if (auto cached = getConcurrent(cache, std::string(rawValue))) {
        if (*cached && initAccessControl) {
            allowPathLegacy(authority, (*cached)->path.abs());
            if (authority.store->isInStore((*cached)->path.abs())) {
                auto [storePath, subPath] = authority.store->toStorePath((*cached)->path.abs());
                allowClosure(authority, storePath);
            }
        }
        if (*cached && observation) {
            std::optional<SourcePath> allowlistedPath;
            std::optional<StorePath> allowlistedClosureRoot;
            if (initAccessControl) {
                allowlistedPath = **cached;
                if (authority.store->isInStore((*cached)->path.abs())) {
                    auto [storePath, subPath] = authority.store->toStorePath((*cached)->path.abs());
                    allowlistedClosureRoot = storePath;
                }
            }
            *observation = ExistingLookupPathResolution{
                {**cached, std::move(allowlistedPath), std::move(allowlistedClosureRoot)},
                {},
                std::move(identity),
            };
        }
        return *cached;
    }

    auto finish = [&](std::optional<SourcePath> result) -> std::optional<SourcePath> {
        cache.insert_or_assign(std::string(rawValue), result);
        return result;
    };

    if (EvalSettings::isPseudoUrl(rawValue)) {
        try {
            auto accessor = fetchers::downloadTarball(
                *authority.store,
                authority.fetchSettings,
                EvalSettings::resolvePseudoUrl(rawValue));
            auto materializedStorePath = fetchToStore(
                authority.fetchSettings,
                *authority.store,
                SourcePath(accessor),
                FetchMode::Copy);
            auto resolved = storePathSource(authority, materializedStorePath);
            if (observation) {
                *observation = DownloadedLookupPathResolution{
                    {resolved, std::nullopt, std::nullopt},
                    {materializedStorePath},
                    std::move(identity),
                };
            }
            return finish(std::move(resolved));
        } catch (Error & e) {
            debug("eval-env/lookup-path: pseudo-url '%s' failed: %s", rawValue, e.what());
        }
    }

    if (auto colPos = rawValue.find(':'); colPos != rawValue.npos) {
        auto scheme = rawValue.substr(0, colPos);
        auto rest = rawValue.substr(colPos + 1);
        if (authority.lookupPathHookResolver) {
            if (auto resolved = authority.lookupPathHookResolver(scheme, rest)) {
                if (observation) {
                    *observation = HookResolvedLookupPathResolution{
                        {*resolved, std::nullopt, std::nullopt},
                        {},
                        std::move(identity),
                    };
                }
                return finish(std::move(*resolved));
            }
        }
    }

    auto path = rootPath(authority, rawValue);
    auto resolvedPath = path.resolveSymlinks(SymlinkResolution::Full);

    std::optional<SourcePath> allowlistedPath;
    std::optional<StorePath> allowlistedClosureRoot;

    if (initAccessControl) {
        allowPathLegacy(authority, path.path.abs());
        allowlistedPath = path;
        if (authority.store->isInStore(resolvedPath.path.abs())) {
            auto [storePath, subPath] = authority.store->toStorePath(resolvedPath.path.abs());
            allowClosure(authority, storePath);
            allowlistedClosureRoot = storePath;
        }
    }

    if (!resolvedPath.pathExists()) {
        if (auto accessor = path.accessor.dynamic_pointer_cast<FilteringSourceAccessor>())
            accessor->checkAccess(path.path);
        if (observation) {
            *observation = MissingLookupPathResolution{{}, {}, std::move(identity)};
        }
        return finish(std::nullopt);
    }

    if (observation) {
        *observation = ExistingLookupPathResolution{
            {resolvedPath, std::move(allowlistedPath), std::move(allowlistedClosureRoot)},
            {},
            std::move(identity),
        };
    }
    return finish(std::move(resolvedPath));
}

}

PublishedStorePathString EvalEnvironment::makePublishedStorePathString(
    const Store & store,
    StorePath storePath,
    StorePathPublicationMode publicationMode,
    std::optional<PathObject> provenance)
{
    auto renderedPath = store.printStorePath(storePath);
    auto context = makePublishedStorePathContext(storePath);

    switch (publicationMode) {
    case StorePathPublicationMode::Preserve:
        if (!provenance)
            throw Error("internal error: preserved store-path publication requires provenance");
        return PublishedStorePathString::preserve(
            std::move(storePath),
            std::move(renderedPath),
            std::move(context),
            std::move(*provenance));
    case StorePathPublicationMode::Detach:
        return PublishedStorePathString::detach(
            std::move(storePath),
            std::move(renderedPath),
            std::move(context));
    case StorePathPublicationMode::Plain:
        return PublishedStorePathString::plain(
            std::move(storePath),
            std::move(renderedPath),
            std::move(context));
    }
    unreachable();
}

EvalEnvironment::EvalEnvironment(const EvalEnvironmentAuthority & authority)
    : pImpl(std::make_shared<EvalEnvironmentState>(authority))
{
}

EvalEnvironment::~EvalEnvironment() = default;

DetachedEffectScope EvalEnvironment::openDetachedEffectScope()
{
    return DetachedEffectScope(pImpl);
}

std::optional<BoundEffectScope> EvalEnvironment::tryBindCurrentEvalSession()
{
    if (auto * session = eval_trace::currentTraceSession())
        return BoundEffectScope(pImpl, ref<eval_trace::TraceSession>(session->shared_from_this()));
    return std::nullopt;
}

void EvalEnvironment::initializeLookupPathAccessControl(
    DetachedEffectScope &,
    const LookupPath & lookupPath)
{
    auto & state = requireState(pImpl);
    for (const auto & elem : lookupPath.elements)
        (void) resolveLookupPathEntry(state, elem.path.s, true, nullptr,
            makeLookupPathEntryForIdentity(
                elem.prefix.s.empty()
                    ? std::nullopt
                    : std::make_optional(LookupPathPrefix{.value = elem.prefix.s}),
                elem.path.s).realize().toIdentity());
}

CapturedSessionOpenInputs EvalEnvironment::captureSessionOpenInputs(
    DetachedEffectScope &,
    const LookupPath & lookupPath)
{
    auto & state = requireState(pImpl);
    return CapturedSessionOpenInputs(
        EvalPolicySnapshot(
            /* useTraceCache */ state.authority.evalSettings.useTraceCache,
            /* purityMode */ state.authority.evalSettings.pureEval ? EvalPurityMode::Pure : EvalPurityMode::Impure,
            /* restrictionMode */ state.authority.evalSettings.restrictEval ? EvalRestrictionMode::Restricted : EvalRestrictionMode::Unrestricted,
            /* ifdMode */ state.authority.evalSettings.enableImportFromDerivation ? ImportFromDerivationMode::Enabled : ImportFromDerivationMode::Disabled,
            /* currentSystem */ SessionCurrentSystem{state.authority.evalSettings.getCurrentSystem()},
            /* nixPathEnv */ getEnv("NIX_PATH").value_or(""),
            /* nixPathSetting */ std::vector<std::string>(
                state.authority.evalSettings.nixPath.get().begin(),
                state.authority.evalSettings.nixPath.get().end()),
            /* allowedUris */ std::vector<std::string>(
                state.authority.evalSettings.allowedUris.get().begin(),
                state.authority.evalSettings.allowedUris.get().end())),
        buildLookupPathEntries(lookupPath));
}

BoundEffectScope EvalEnvironment::openEvalSession(FlakeEvalSessionOpen sessionOpen)
{
    auto [authority, sessionReuse] = std::move(sessionOpen);
    return openEvalSessionImpl(std::move(authority), std::move(sessionReuse));
}

BoundEffectScope EvalEnvironment::openEvalSession(FileEvalSessionOpen sessionOpen)
{
    auto [authority, sessionReuse] = std::move(sessionOpen);
    return openEvalSessionImpl(std::move(authority), std::move(sessionReuse));
}

template<typename SessionReuse>
BoundEffectScope EvalEnvironment::openEvalSessionImpl(
    EvalTraceSessionAuthority authority,
    SessionReuse sessionReuse)
{
    auto & state = requireState(pImpl);
    auto reuseKey = sessionReuseKey(sessionReuse);

    if (!state.authority.traceSessionFactory)
        throw UnimplementedError("EvalEnvironment requires a trace session factory to open sessions");

    auto session = state.authority.traceSessionFactory->openTraceSession(
        reuseKey,
        std::move(authority));
    return BoundEffectScope(pImpl, std::move(session));
}

ref<eval_trace::TraceSession> EvalEnvironment::traceSession(const BoundEffectScope & session) const
{
    return session.session_;
}

EnvVarObservation EvalEnvironment::readEnvVar(ObserveOnlyTag, std::string_view name)
{
    auto key = std::string(name);
    auto & state = requireState(pImpl);
    return EnvVarObservation{
        .name = key,
        .value = (state.authority.evalSettings.restrictEval || state.authority.evalSettings.pureEval)
            ? std::string("")
            : getEnv(key).value_or(""),
    };
}

EnvVarObservation EvalEnvironment::readEnvVar(DetachedEffectScope &, std::string_view name)
{
    return readEnvVar(observeOnly, name);
}

EnvVarObservation EvalEnvironment::readEnvVar(const eval_trace::TraceAccess & access, std::string_view name)
{
    auto observation = readEnvVar(observeOnly, name);
    recordEnvVarObservation(access, observation);
    return observation;
}

EnvVarObservation EvalEnvironment::readEnvVar(BoundEffectScope & session, std::string_view name)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return readEnvVar(access, name);
    });
}

LookupPathObservation EvalEnvironment::resolveLookupPath(DetachedEffectScope &, const LookupPathRequest & request)
{
    auto & state = requireState(pImpl);
    LookupPathObservation observation{
        .request = request,
        .envLookups = {},
        .resolvedEntries = {},
        .resolution = LookupPathObservation::CorepkgsFallbackResolution{},
        .resolvedPath = rootPath(state.authority, "/"),
    };

    if (!state.authority.evalSettings.pureEval) {
        observation.envLookups.push_back(EnvVarObservation{
            .name = "NIX_PATH",
            .value = getEnv("NIX_PATH").value_or(""),
        });
    }

    for (size_t idx = 0; idx < request.lookupPathEntries.size(); ++idx) {
        const auto & entry = request.lookupPathEntries[idx];
        auto rawValue = entry.rawValue.value;
        if (!entry.context.empty()) {
            auto rewrites = realiseContextImpl(
                state.authority,
                entry.context,
                nullptr,
                /* isIFD */ true);
            rawValue = rewriteStrings(std::move(rawValue), rewrites);
        }

        LookupPath::Prefix prefix{.s = entry.prefix ? entry.prefix->value : ""};
        auto suffixOpt = prefix.suffixIfPotentialMatch(request.logicalPath);
        auto identity = entry.realize(LookupPathRawValue{.value = rawValue}).toIdentity();

        if (!suffixOpt) {
            observation.resolvedEntries.push_back(
                MissingLookupPathResolution{{}, {}, std::move(identity)});
            continue;
        }

        LookupPathResolvedEntry resolvedEntry = MissingLookupPathResolution{{}, {}, identity};
        auto resolvedRoot = resolveLookupPathEntry(
            state,
            identity.rawValue.value,
            request.accessControlMode == LookupPathAccessControlMode::Initialize,
            &resolvedEntry,
            identity);
        observation.resolvedEntries.push_back(resolvedEntry);

        if (!resolvedRoot)
            continue;

        auto matchedPath = *resolvedRoot / CanonPath(*suffixOpt);
        auto resolvedPath = matchedPath.resolveSymlinks();
        if (auto accessor = matchedPath.accessor.dynamic_pointer_cast<FilteringSourceAccessor>())
            accessor->checkAccess(matchedPath.path);
        if (auto accessor = resolvedPath.accessor.dynamic_pointer_cast<FilteringSourceAccessor>())
            accessor->checkAccess(resolvedPath.path);
        if (resolvedPath.pathExists()) {
            observation.resolvedPath = resolvedPath;
            std::optional<PathObject> matchedOrigin;
            if (entry.origin
                && entry.origin->source.kind() == DepSourceKind::Registered
                && (resolvedPath.path == entry.origin->rootPath
                    || resolvedPath.path.isWithin(entry.origin->rootPath)))
            {
                matchedOrigin = entry.origin;
            }
            observation.resolution = LookupPathObservation::MatchedResolution{
                .matchedEntryIndex = static_cast<uint32_t>(idx),
                .resolvedEntryRoot = *resolvedRoot,
                .matchedOrigin = std::move(matchedOrigin),
            };
            return observation;
        }
    }

    if (hasPrefix(request.logicalPath, "nix/")) {
        std::shared_ptr<SourceAccessor> corepkgsAccessor =
            std::static_pointer_cast<SourceAccessor>(state.authority.corepkgsFS.get_ptr());
        observation.resolvedPath = SourcePath{
            ref<SourceAccessor>(std::move(corepkgsAccessor)),
            CanonPath(request.logicalPath.substr(3)),
        };
        observation.resolution = LookupPathObservation::CorepkgsFallbackResolution{};
        return observation;
    }

    throw LookupPathMissError(
        state.authority.evalSettings.pureEval
            ? "cannot look up '<%s>' in pure evaluation mode (use '--impure' to override)"
            : "file '%s' was not found in the Nix search path (add it using $NIX_PATH or -I)",
        request.logicalPath);
}

LookupPathObservation EvalEnvironment::resolveLookupPath(const eval_trace::TraceAccess & access, const LookupPathRequest & request)
{
    DetachedEffectScope detached(pImpl);
    auto observation = resolveLookupPath(detached, request);
    recordLookupPathObservation(access, observation);
    return observation;
}

LookupPathObservation EvalEnvironment::resolveLookupPath(BoundEffectScope & session, const LookupPathRequest & request)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return resolveLookupPath(access, request);
    });
}

UriPolicyObservation EvalEnvironment::authorizeUri(
    DetachedEffectScope &,
    const UriPolicyRequest & request)
{
    auto & state = requireState(pImpl);
    checkURI(state.authority, request.uri);
    return UriPolicyObservation{.request = request};
}

UriPolicyObservation EvalEnvironment::authorizeUri(
    BoundEffectScope &,
    const UriPolicyRequest & request)
{
    auto & state = requireState(pImpl);
    checkURI(state.authority, request.uri);
    return UriPolicyObservation{.request = request};
}

ContextRealisationObservation EvalEnvironment::realiseContext(
    const eval_trace::TraceAccess & access,
    const RealiseContextRequest & request)
{
    DetachedEffectScope detached(pImpl);
    auto observation = realiseContext(detached, request);
    recordContextRealisationObservation(access, observation);
    return observation;
}

ContextRealisationObservation EvalEnvironment::realiseContext(DetachedEffectScope &, const RealiseContextRequest & request)
{
    auto & state = requireState(pImpl);
    StorePathSet referencedStorePaths;
    auto rewrites = realiseContextImpl(
        state.authority,
        request.context,
        &referencedStorePaths,
        request.mode == StringRealisationMode::ImportFromDerivation);

    std::vector<PlaceholderRewrite> placeholderRewrites;
    placeholderRewrites.reserve(rewrites.size());
    for (auto & [placeholder, storePathString] : rewrites) {
        auto storePath = state.authority.store->parseStorePath(storePathString);
        placeholderRewrites.push_back(PlaceholderRewrite{
            .placeholder = std::move(placeholder),
            .storePath = std::move(storePath),
        });
    }

    std::vector<StorePath> referencedPaths;
    referencedPaths.reserve(referencedStorePaths.size());
    for (const auto & path : referencedStorePaths)
        referencedPaths.push_back(path);

    return ContextRealisationObservation{
        .request = request,
        .rewrites = std::move(placeholderRewrites),
        .referencedStorePaths = std::move(referencedPaths),
    };
}

ContextRealisationObservation EvalEnvironment::realiseContext(BoundEffectScope & session, const RealiseContextRequest & request)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return realiseContext(access, request);
    });
}


PathStatusObservation EvalEnvironment::observePath(ObserveOnlyTag, const PathObservationRequest & request)
{
    auto & state = requireState(pImpl);
    auto observedPath = realiseCoercedPath(
        state.authority,
        request.coercedPath,
        request.symlinkResolution);
    auto stat = observedPath.maybeLstat();
    auto exists = stat
        && (request.mode != PathObservationMode::MustBeDirectory
            || stat->type == SourceAccessor::tDirectory);
    return PathStatusObservation{
        .request = request,
        .observedPath = std::move(observedPath),
        .stat = stat,
        .exists = exists,
    };
}

PathStatusObservation EvalEnvironment::observePath(
    const eval_trace::TraceAccess & access,
    const PathObservationRequest & request)
{
    auto observation = observePath(observeOnly, request);
    recordPathStatusObservation(access, observation);
    return observation;
}

PathStatusObservation EvalEnvironment::observePath(DetachedEffectScope &, const PathObservationRequest & request)
{
    return observePath(observeOnly, request);
}

PathStatusObservation EvalEnvironment::observePath(BoundEffectScope & session, const PathObservationRequest & request)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return observePath(access, request);
    });
}

FileReadObservation EvalEnvironment::readFile(ObserveOnlyTag, const ReadFileRequest & request)
{
    auto & state = requireState(pImpl);
    auto observedPath = realiseCoercedPath(
        state.authority,
        request.coercedPath,
        SymlinkResolution::Full);
    auto bytes = observedPath.readFile();

    return FileReadObservation{
        .request = request,
        .observedPath = std::move(observedPath),
        .bytes = std::move(bytes),
    };
}

FileReadObservation EvalEnvironment::readFile(
    const eval_trace::TraceAccess & access,
    const ReadFileRequest & request)
{
    auto observation = readFile(observeOnly, request);
    auto & state = requireState(pImpl);

    auto hash = getOrStoreFileContentHash(
        *state.authority.evalState, observation.observedPath, observation.bytes);

    if (auto resolved = resolveProvenanceViaRegistry(
            access,
            observation.observedPath,
            request.coercedPath.origin))
    {
        observation.textObject = TextObject{
            std::move(resolved->source),
            std::move(resolved->key),
            hash,
        };
    }

    // governingRepoId is excluded from Dep::Key identity (see Key::Hash in
    // types.hh), so the dedup probe need only match kind + sourceId + keyId.
    if (observation.textObject) {
        auto & pools = access.tracingPools();
        auto sourceId = pools.intern<DepSourceId>(observation.textObject->source);
        auto keyId = pools.intern(SimpleDepKeyAtom{observation.textObject->key});
        auto depKey = Dep::Key::makeSimple(CanonicalQueryKind::FileBytes, sourceId, keyId);
        if (access.scopeContainsDepKey(depKey))
            return observation;
    }

    recordFileReadObservation(access, observation, hash);
    return observation;
}

FileReadObservation EvalEnvironment::readFile(DetachedEffectScope &, const ReadFileRequest & request)
{
    return readFile(observeOnly, request);
}

FileReadObservation EvalEnvironment::readFile(BoundEffectScope & session, const ReadFileRequest & request)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return readFile(access, request);
    });
}

DirectoryReadObservation EvalEnvironment::readDirectory(ObserveOnlyTag, const ReadDirectoryRequest & request)
{
    auto & state = requireState(pImpl);
    auto observedPath = realiseCoercedPath(
        state.authority,
        request.coercedPath,
        SymlinkResolution::Full);
    return DirectoryReadObservation{
        .request = request,
        .observedPath = observedPath,
        .entries = observedPath.readDirectory(),
        .structuredObject = std::nullopt,
    };
}

DirectoryReadObservation EvalEnvironment::readDirectory(
    const eval_trace::TraceAccess & access,
    const ReadDirectoryRequest & request)
{
    auto observation = readDirectory(observeOnly, request);
    recordDirectoryReadObservation(access, observation);
    return observation;
}

DirectoryReadObservation EvalEnvironment::readDirectory(DetachedEffectScope &, const ReadDirectoryRequest & request)
{
    return readDirectory(observeOnly, request);
}

DirectoryReadObservation EvalEnvironment::readDirectory(BoundEffectScope & session, const ReadDirectoryRequest & request)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return readDirectory(access, request);
    });
}

PublishedStorePathString EvalEnvironment::publishStorePath(
    const eval_trace::TraceAccess & access,
    const StorePathPublishRequest & request)
{
    auto detached = openDetachedEffectScope();
    auto published = publishStorePath(detached, request);
    recordPublishedStorePathString(access, published);
    return published;
}

PublishedStorePathString EvalEnvironment::publishStorePath(
    DetachedEffectScope &,
    const StorePathPublishRequest & request)
{
    auto & state = requireState(pImpl);
    auto path = realiseCoercedPath(
        state.authority,
        request.coercedPath,
        SymlinkResolution::Full);

    StorePath storePath = [&]() {
        if (sameLogicalStore(state.authority)) {
            if (auto parsed = state.authority.store->maybeParseStorePath(path.path.abs());
                parsed && state.authority.store->isValidPath(*parsed))
            {
                if (path.accessor == state.authority.storeFS.cast<SourceAccessor>())
                    return *parsed;
                if (path.accessor == state.authority.rootFS
                    && state.authority.store->isInStore(path.path.abs()))
                    return *parsed;
            }
        }
        return fetchToStore(
            state.authority.fetchSettings,
            *state.authority.store,
            path.resolveSymlinks(SymlinkResolution::Ancestors),
            state.authority.evalSettings.isReadOnly() ? FetchMode::DryRun : FetchMode::Copy,
            path.baseName());
    }();

    allowPath(state.authority, storePath);

    if (request.coercedPath.origin) {
        return makePublishedStorePathString(
            *state.authority.store,
            std::move(storePath),
            StorePathPublicationMode::Preserve,
            *request.coercedPath.origin);
    }

    return makePublishedStorePathString(
        *state.authority.store,
        std::move(storePath),
        StorePathPublicationMode::Detach);
}

PublishedStorePathString EvalEnvironment::publishStorePath(
    BoundEffectScope & session,
    const StorePathPublishRequest & request)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return publishStorePath(access, request);
    });
}

static CopyPathToStoreOutcome copyPathToStoreImpl(
    EvalEnvironmentState & state,
    const CopyPathToStoreRequest & request)
{
    auto path = request.path;
    auto cacheKey = SrcToStoreCacheKey{
        .path = path,
        .store = &*state.authority.store,
    };
    StorePathSet refs;
    std::vector<DeferredRecordedDep> deferredDeps;

    if (path.accessor == state.authority.rootFS
        && state.authority.store->isInStore(path.path.abs())
        && !request.context.empty())
    {
        auto rewrites = realiseContextImpl(
            state.authority,
            request.context,
            nullptr,
            /* isIFD */ true);
        path = {path.accessor, CanonPath(rewriteStrings(path.path.abs(), rewrites))};
        auto [storePath, subPath] = state.authority.store->toStorePath(path.path.abs());
        try {
            refs = state.authority.store->queryPathInfo(storePath)->references;
        } catch (Error &) {
        }
    }

    std::unique_ptr<PathFilter> filter;
    bool collectFilterDeps = false;
    if (request.hasFilter()) {
        collectFilterDeps = !state.authority.store->isInStore(path.path.abs());

        filter = std::make_unique<PathFilter>([&](const std::string & p) {
            auto p2 = CanonPath(p);
            SourcePath sp{path.accessor, p2};
            bool include = request.filterEvaluator(sp);

            if (include && collectFilterDeps) {
                auto st = sp.lstat();
                if (st.type == SourceAccessor::tRegular) {
                    deferredDeps.push_back(DeferredRecordedDep{
                        .path = sp,
                        .hash = depHashPath(sp),
                        .kind = CanonicalQueryKind::NarIdentity,
                        .origin = request.origin,
                        .storeName = {},
                    });
                } else if (st.type == SourceAccessor::tDirectory) {
                    deferredDeps.push_back(DeferredRecordedDep{
                        .path = sp,
                        .hash = depHashDirListing(sp.readDirectory()),
                        .kind = CanonicalQueryKind::DirectoryEntries,
                        .origin = request.origin,
                        .storeName = {},
                    });
                }
                return include;
            }

            return include;
        });

        if (collectFilterDeps) {
            auto rootType = path.lstat().type;
            if (rootType == SourceAccessor::tDirectory) {
                deferredDeps.push_back(DeferredRecordedDep{
                    .path = path,
                    .hash = depHashDirListing(path.readDirectory()),
                    .kind = CanonicalQueryKind::DirectoryEntries,
                    .origin = request.origin,
                    .storeName = {},
                });
            } else if (rootType == SourceAccessor::tRegular) {
                deferredDeps.push_back(DeferredRecordedDep{
                    .path = path,
                    .hash = depHashPath(path),
                    .kind = CanonicalQueryKind::NarIdentity,
                    .origin = request.origin,
                    .storeName = {},
                });
            }
        }
    }

    std::optional<StorePath> expectedStorePath;
    if (request.expectedHash) {
        expectedStorePath = state.authority.store->makeFixedOutputPathFromCA(
            request.name,
            ContentAddressWithReferences::fromParts(request.method, *request.expectedHash, {refs}));
    }

    // The `srcToStore` cache keys by `{path, store}` only — no content
    // fingerprint.  Caching is only sound when the accessor serves a
    // content-addressed source (git object database, tarball, fetched
    // archive), identified by a non-null `fingerprint` on the accessor.
    // Dirty working directories (PosixSourceAccessor) return no
    // fingerprint; caching their store paths leaks stale results across
    // mutations — see SourceTreeSoundnessTest.ParentChild_PerLeafLazyVerification
    // (which verifies the per-leaf re-evaluation contract when this
    // fingerprint guard correctly forces a re-copy on mutation).
    bool cacheable = !request.hasFilter()
        && refs.empty()
        && !expectedStorePath
        && state.authority.sharedState
        && path.accessor->getFingerprint(path.path).second.has_value();

    StorePath resultStorePath = [&]() -> StorePath {
        if (cacheable) {
            if (auto cached = getConcurrent(*state.authority.sharedState->srcToStore, cacheKey))
                return *cached;
        }

        if (!expectedStorePath || !state.authority.store->isValidPath(*expectedStorePath)) {
            auto dstPath = refs.empty()
            ? fetchToStore(
                state.authority.fetchSettings,
                *state.authority.store,
                path.resolveSymlinks(SymlinkResolution::Ancestors),
                state.authority.evalSettings.isReadOnly() ? FetchMode::DryRun : FetchMode::Copy,
                request.name,
                request.method,
                filter.get(),
                state.authority.repair)
            : state.authority.store->addToStore(
                request.name,
                path.resolveSymlinks(SymlinkResolution::Ancestors),
                request.method,
                HashAlgorithm::SHA256,
                refs,
                filter ? *filter.get() : defaultPathFilter,
                state.authority.repair);
            if (expectedStorePath && expectedStorePath != dstPath) {
                throw Error(
                    "store path mismatch in (possibly filtered) path added from '%s'",
                    path);
            }
            if (cacheable)
                state.authority.sharedState->srcToStore->try_emplace(cacheKey, dstPath);
            return dstPath;
        }
        return *expectedStorePath;
    }();

    allowPath(state.authority, resultStorePath);

    if (!request.hasFilter()
        && refs.empty()
        && !state.authority.store->isInStore(path.path.abs()))
    {
        deferredDeps.push_back(DeferredRecordedDep{
            .path = path,
            .hash = DepHashValue(state.authority.store->printStorePath(resultStorePath)),
            .kind = CanonicalQueryKind::DerivedStorePath,
            .origin = request.origin,
            .storeName = request.name,
        });
    }

    return CopyPathToStoreOutcome{
        .storePath = resultStorePath,
        .deferredDeps = std::move(deferredDeps),
    };
}

PublishedStorePathString EvalEnvironment::copyPathToStore(
    const eval_trace::TraceAccess & access,
    const CopyPathToStoreRequest & request)
{
    auto outcome = copyPathToStoreImpl(requireState(pImpl), request);
    recordDeferredPathDeps(access, outcome.deferredDeps);
    auto & state = requireState(pImpl);
    auto published = makePublishedStorePathString(
        *state.authority.store,
        std::move(outcome.storePath),
        StorePathPublicationMode::Detach);
    recordPublishedStorePathString(access, published);
    return published;
}

PublishedStorePathString EvalEnvironment::copyPathToStore(
    DetachedEffectScope &,
    const CopyPathToStoreRequest & request)
{
    auto outcome = copyPathToStoreImpl(requireState(pImpl), request);
    auto & state = requireState(pImpl);
    return makePublishedStorePathString(
        *state.authority.store,
        std::move(outcome.storePath),
        StorePathPublicationMode::Detach);
}

PublishedStorePathString EvalEnvironment::copyPathToStore(
    BoundEffectScope & session,
    const CopyPathToStoreRequest & request)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return copyPathToStore(access, request);
    });
}

StorePathObservation EvalEnvironment::authorizeStorePath(DetachedEffectScope &, const StorePath & path)
{
    auto & state = requireState(pImpl);
    allowPath(state.authority, path);
    return StorePathObservation{.storePath = path};
}

StorePathObservation EvalEnvironment::authorizeStorePath(const eval_trace::TraceAccess & access, const StorePath & path)
{
    auto & state = requireState(pImpl);
    allowPath(state.authority, path);
    recordStorePathAvailability(access, path);
    return StorePathObservation{.storePath = path};
}

StorePathObservation EvalEnvironment::authorizeStorePath(BoundEffectScope & session, const StorePath & path)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return authorizeStorePath(access, path);
    });
}

StoreClosureObservation EvalEnvironment::authorizeStoreClosure(DetachedEffectScope &, const StorePath & path)
{
    auto & state = requireState(pImpl);
    allowClosure(state.authority, path);
    return StoreClosureObservation{.root = path};
}

StoreClosureObservation EvalEnvironment::authorizeStoreClosure(const eval_trace::TraceAccess & access, const StorePath & path)
{
    auto & state = requireState(pImpl);
    allowClosure(state.authority, path);
    recordStorePathAvailability(access, path);
    return StoreClosureObservation{.root = path};
}

StoreClosureObservation EvalEnvironment::authorizeStoreClosure(BoundEffectScope & session, const StorePath & path)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return authorizeStoreClosure(access, path);
    });
}

PublishedStorePathString EvalEnvironment::renderAuthorizedStorePath(
    DetachedEffectScope &,
    const AuthorizedStorePathRenderRequest & request)
{
    auto & state = requireState(pImpl);
    return makePublishedStorePathString(
        *state.authority.store,
        request.storePath,
        request.publicationMode,
        request.provenance);
}

PublishedStorePathString EvalEnvironment::renderAuthorizedStorePath(
    const eval_trace::TraceAccess & access,
    const AuthorizedStorePathRenderRequest & request)
{
    DetachedEffectScope detached(pImpl);
    auto published = renderAuthorizedStorePath(detached, request);
    recordPublishedStorePathString(access, published);
    return published;
}

PublishedStorePathString EvalEnvironment::renderAuthorizedStorePath(
    BoundEffectScope & session,
    const AuthorizedStorePathRenderRequest & request)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return renderAuthorizedStorePath(access, request);
    });
}

FetchIdentityResolution EvalEnvironment::resolveFetchIdentity(
    DetachedEffectScope &,
    const FetchIdentityRequest & request)
{
    auto & state = requireState(pImpl);
    auto & originalInput = *request.input.value;
    auto cached = state.authority.inputCache->getAccessor(
        state.authority.fetchSettings,
        *state.authority.store,
        originalInput,
        request.useRegistries);

    auto materializationFingerprint = [&]() -> std::optional<std::string> {
        auto fingerprint = [&]() -> std::optional<std::string> {
            if (originalInput.getSourcePath()
                && !originalInput.isLocked(state.authority.fetchSettings)
                && !originalInput.getRef()
                && !originalInput.getRev())
                return originalInput.getFingerprint(*state.authority.store);
            return cached.lockedInput.getFingerprint(*state.authority.store);
        }();
        if (!fingerprint)
            fingerprint = cached.accessor->getFingerprint(CanonPath::root).second;
        if (!fingerprint && originalInput.getSourcePath())
            fingerprint = originalInput.getFingerprint(*state.authority.store);
        return fingerprint;
    }();

    auto resolvedInput = std::move(cached.resolvedInput);
    auto extraAttrs = std::move(cached.extraAttrs);
    auto phase1Root = computePhase1FetchRoot(
        state.authority,
        originalInput,
        resolvedInput,
        cached.accessor);
    return FetchIdentityResolution{
        .resolvedInput = std::move(resolvedInput),
        .extraAttrs = extraAttrs,
        .phase1Root = std::move(phase1Root),
        .identity = ResolvedFetchIdentity(ResolvedFetchIdentity::MaterializationPayload{
            .request = request,
            .lockedInput = ResolvedLockedInput{std::make_shared<const fetchers::Input>(std::move(cached.lockedInput))},
            .accessor = cached.accessor,
            .materializationFingerprint = std::move(materializationFingerprint),
        }),
    };
}

FetchIdentityResolution EvalEnvironment::resolveFetchIdentity(
    BoundEffectScope &,
    const FetchIdentityRequest & request)
{
    DetachedEffectScope detached(pImpl);
    return resolveFetchIdentity(detached, request);
}

FetchedInput EvalEnvironment::materializeFetch(DetachedEffectScope &, ResolvedFetchIdentity && identity)
{
    auto payload = std::move(identity).consumeForMaterialization();
    return FetchedInput(FetchedInput::MountPayload{
        .originalInput = payload.request.input,
        .lockedInput = std::move(payload.lockedInput),
        .accessor = std::move(payload.accessor),
        .materializationFingerprint = std::move(payload.materializationFingerprint),
    });
}

FetchedInput EvalEnvironment::materializeFetch(BoundEffectScope &, ResolvedFetchIdentity && identity)
{
    DetachedEffectScope detached(pImpl);
    return materializeFetch(detached, std::move(identity));
}

DetachedMountedStorePath EvalEnvironment::ensureMountedStorePath(
    DetachedEffectScope &,
    const EnsureMountedStorePathRequest & request)
{
    auto & state = requireState(pImpl);
    auto cached = state.authority.inputCache->getAccessor(
        state.authority.fetchSettings,
        *state.authority.store,
        *request.lockedInput,
        fetchers::UseRegistries::No);
    allowPath(state.authority, request.storePath);
    state.authority.storeFS->mount(
        CanonPath(state.authority.store->printStorePath(request.storePath)),
        cached.accessor);
    return DetachedMountedStorePath{
        .storePath = request.storePath,
        .provenance = request.provenance,
    };
}

DetachedStandaloneMountedInput EvalEnvironment::mountFetchedInput(
    DetachedEffectScope &,
    FetchedInput && fetched)
{
    auto payload = std::move(fetched).consumeForMount();
    auto & state = requireState(pImpl);
    auto mounted = mountInput(
        state.authority,
        payload.lockedInput,
        *payload.originalInput.value,
        payload.accessor,
        std::move(payload.materializationFingerprint));
    return DetachedStandaloneMountedInput(
        MountedStorePath{std::move(mounted.storePath), std::nullopt},
        std::move(mounted.finalizedInput));
}

DetachedGraphMountedInput EvalEnvironment::mountGraphFetchedInput(
    DetachedEffectScope &,
    FetchedInput && fetched,
    DepSource promotedGraphSource)
{
    auto payload = std::move(fetched).consumeForMount();
    auto & state = requireState(pImpl);
    auto mounted = mountInput(
        state.authority,
        payload.lockedInput,
        *payload.originalInput.value,
        payload.accessor,
        std::move(payload.materializationFingerprint));
    return DetachedGraphMountedInput(
        MountedStorePath{std::move(mounted.storePath), std::nullopt},
        MountedInputGraphField{std::move(promotedGraphSource)});
}

RuntimeFetchResult EvalEnvironment::mountAndCompleteRuntimeFetch(
    BoundEffectScope && session, FetchedInput && fetched)
{
    auto payload = std::move(fetched).consumeForMount();
    auto & state = requireState(pImpl);
    auto mounted = mountInput(
        state.authority,
        payload.lockedInput,
        *payload.originalInput.value,
        payload.accessor,
        std::move(payload.materializationFingerprint));

    // Preserve the caller-visible semantics of the original fetch request.
    // A local git worktree without explicit ref/rev is semantically unlocked
    // even if getAccessor() resolves it to a clean locked revision today.
    if (payload.originalInput.value->isLocked(state.authority.fetchSettings)) {
        auto mountedInput = BoundLockedMountedInput(
            MountedStorePath{std::move(mounted.storePath), std::nullopt},
            std::move(mounted.finalizedInput),
            MountedInputNarHashField{std::move(mounted.narHash)});
        return completeLockedRuntimeFetch(std::move(session), std::move(mountedInput));
    }

    auto mountedInput = BoundUnlockedMountedInput(
        MountedStorePath{std::move(mounted.storePath), std::nullopt},
        std::move(mounted.finalizedInput));
    return completeUnlockedRuntimeFetch(
        std::move(session), std::move(mountedInput), std::move(payload.originalInput));
}

GraphFetchCompletion EvalEnvironment::completeGraphFetch(
    DetachedEffectScope &,
    DetachedGraphMountedInput && mountedInput)
{
    return std::move(mountedInput).consumeForGraphCompletion();
}

/// Register a runtime root mount in the in-memory SemanticRegistry AND
/// persist it to the SessionRuntimeRoots DB table.  Both steps are
/// required for warm-verify: the in-memory mount enables dep resolution
/// within this process, and the DB row enables a future warm-verify
/// process to reconstruct the same registry entry.
///
/// This helper pairs the two calls so that omitting one is a code-change
/// in a single function rather than an independent oversight in each
/// completion path.
static void registerAndPersistRuntimeRoot(
    eval_trace::TraceSession & traceSession,
    Store & store,
    const DepSource & runtimeSource,
    const RuntimeFetchIdentityDepKey & fetchIdentity,
    const RuntimeRootNarHash & narHash,
    const StorePath & storePath,
    const std::string & storePathPrinted)
{
    traceSession.registerRuntimeRootMount(
        CanonPath(storePathPrinted),
        runtimeSource,
        RegistryMountSubdir{CanonPath::root});

    // If the session has no backend (useTraceCache disabled, or backend setup
    // failed), skip persistence. The in-memory mount registration above is
    // enough for this process; no warm-verify will ever try to reconstruct
    // the registry from the (nonexistent) DB row.
    if (auto * backend = traceSession.traceBackend()) {
        backend->recordRuntimeRoot(
            eval_trace::SqliteTraceStorage::RuntimeRootRecord{
                .source = runtimeSource,
                .fetchIdentity = fetchIdentity,
                .narHash = narHash,
                .storePath = RuntimeRootStorePath{storePath},
            },
            store);
    }
}

LockedPublishedRuntimeFetch EvalEnvironment::completeLockedRuntimeFetch(
    BoundEffectScope && session,
    BoundLockedMountedInput && mountedInput)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) mutable {
        auto payload = std::move(mountedInput).consumeForRuntimeCompletion();
        auto runtimeFetchIdentity = computeRuntimeFetchIdentityKey(*payload.lockedInput.value);
        auto runtimeSource = DepSource::fromRuntimeRoot(makeRuntimeRootSourceKey(runtimeFetchIdentity));
        auto & state = requireState(pImpl);
        auto storePathPrinted = state.authority.store->printStorePath(payload.storePath);

        registerAndPersistRuntimeRoot(
            *session.session_, *state.authority.store,
            runtimeSource, runtimeFetchIdentity,
            payload.narHash, payload.storePath, storePathPrinted);

        recordStorePathAvailability(access, payload.storePath);

        auto candidate = RuntimeRootCandidate{
            .source = runtimeSource,
            .fetchIdentity = std::move(runtimeFetchIdentity),
            .narHash = payload.narHash,
            .storePath = RuntimeRootStorePath{payload.storePath},
        };

        return LockedPublishedRuntimeFetch(
            MountedStorePath{std::move(payload.storePath), std::move(payload.provenance)},
            std::move(payload.lockedInput),
            std::move(session),
            LockedRuntimeFetchField{std::move(candidate)});
    });
}

UnlockedPublishedRuntimeFetch EvalEnvironment::completeUnlockedRuntimeFetch(
    BoundEffectScope && session,
    BoundUnlockedMountedInput && mountedInput,
    OriginalFetchInput originalInput)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) mutable {
        auto payload = std::move(mountedInput).consumeForRuntimeCompletion();
        auto runtimeKey = computeRuntimeFetchIdentityKey(*originalInput.value);
        auto runtimeSource = DepSource::fromRuntimeRoot(makeRuntimeRootSourceKey(runtimeKey));
        auto & state = requireState(pImpl);
        auto storePathPrinted = state.authority.store->printStorePath(payload.storePath);

        auto narHash = RuntimeRootNarHash{
            state.authority.store->queryPathInfo(payload.storePath)->narHash};
        registerAndPersistRuntimeRoot(
            *session.session_, *state.authority.store,
            runtimeSource, runtimeKey,
            narHash, payload.storePath, storePathPrinted);

        recordRuntimeFetchIdentity(access, runtimeKey, storePathPrinted);

        return UnlockedPublishedRuntimeFetch(
            MountedStorePath{std::move(payload.storePath), std::move(payload.provenance)},
            std::move(payload.lockedInput),
            std::move(session),
            UnlockedRuntimeFetchField{runtimeSource});
    });
}

RuntimeFetchIdentityObservation EvalEnvironment::observeRuntimeFetchIdentity(
    ObserveOnlyTag,
    const FetchIdentityRequest & request)
{
    auto & state = requireState(pImpl);

    auto computeFromLockedInput = [&](const fetchers::Input & lockedInput, ref<SourceAccessor> accessor)
        -> StorePath
    {
        auto narHash = lockedInput.getNarHash().value_or(
            accessor->hashPath(
                CanonPath::root,
                defaultPathFilter,
                HashAlgorithm::SHA256));
        return state.authority.store->makeFixedOutputPath(
            lockedInput.getName(),
            FixedOutputInfo{
                .method = FileIngestionMethod::NixArchive,
                .hash = narHash,
                .references = {},
            });
    };

    std::optional<StorePath> storePath;
    try {
        const auto & input = *request.input.value;
        if (input.getSourcePath()) {
            auto [accessor, lockedInput] = input.getAccessor(state.authority.fetchSettings, *state.authority.store);
            storePath = computeFromLockedInput(lockedInput, accessor);
        } else {
            storePath = input.computeStorePath(*state.authority.store);
        }
    } catch (std::exception &) {
        storePath = std::nullopt;
    }

    return RuntimeFetchIdentityObservation{
        .request = request,
        .storePath = std::move(storePath),
    };
}

GitIdentityObservation EvalEnvironment::observeGitIdentity(
    ObserveOnlyTag,
    const GitIdentityRequest & request)
{
    auto & state = requireState(pImpl);
    auto observedRepoRoot = realiseCoercedPath(
        state.authority,
        request.repoRoot,
        SymlinkResolution::Full);
    auto physicalPath = observedRepoRoot.getPhysicalPath();
    auto repoRoot = physicalPath ? *physicalPath : std::filesystem::path(observedRepoRoot.path.abs());
    return GitIdentityObservation{
        .request = request,
        .observedRepoRoot = std::move(observedRepoRoot),
        .hash = computeGitIdentityHash(repoRoot),
    };
}

GitIdentityObservation EvalEnvironment::observeGitIdentity(
    DetachedEffectScope &,
    const GitIdentityRequest & request)
{
    return observeGitIdentity(observeOnly, request);
}

GitIdentityObservation EvalEnvironment::observeGitIdentity(
    BoundEffectScope & session,
    const GitIdentityRequest & request)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        auto observation = observeGitIdentity(observeOnly, request);
        recordGitIdentityObservation(access, observation);
        return observation;
    });
}

DerivedStorePathObservation EvalEnvironment::observeDerivedStorePath(
    ObserveOnlyTag,
    const DerivedStorePathRequest & request)
{
    auto & state = requireState(pImpl);
    auto observedSourcePath = realiseCoercedPath(
        state.authority,
        request.sourcePath,
        SymlinkResolution::Ancestors);
    return DerivedStorePathObservation{
        .request = request,
        .observedSourcePath = observedSourcePath,
        .storePath = fetchToStore(
            state.authority.fetchSettings,
            *state.authority.store,
            observedSourcePath,
            FetchMode::DryRun,
            request.storeName),
    };
}

DerivedStorePathObservation EvalEnvironment::observeDerivedStorePath(
    BoundEffectScope & session,
    const DerivedStorePathRequest & request)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        auto observation = observeDerivedStorePath(observeOnly, request);
        recordDerivedStorePathObservation(access, observation, &*requireState(pImpl).authority.store);
        return observation;
    });
}

SessionSystemObservation EvalEnvironment::observeSessionSystem(ObserveOnlyTag)
{
    return SessionSystemObservation{
        .currentSystem = SessionCurrentSystem{.value = requireState(pImpl).authority.evalSettings.getCurrentSystem()},
    };
}

SessionSystemObservation EvalEnvironment::observeSessionSystem(const eval_trace::TraceAccess & access)
{
    auto observation = observeSessionSystem(observeOnly);
    recordSessionSystemObservation(access, observation);
    return observation;
}

SessionSystemObservation EvalEnvironment::observeSessionSystem(BoundEffectScope & session)
{
    return boundRecord(session, [&](const eval_trace::TraceAccess & access) {
        return observeSessionSystem(access);
    });
}

// ═══════════════════════════════════════════════════════════════════════
// Auto-dispatch overloads
// ═══════════════════════════════════════════════════════════════════════
//
// Priority: bound TraceSession > standalone TraceAccess > detached/observeOnly.
// The standalone TraceAccess path covers non-session recording contexts
// such as DepCaptureScope, matching how shape deps in primops.cc already
// record via TraceAccess::current().

EnvVarObservation EvalEnvironment::readEnvVar(std::string_view name)
{
    return dispatchBoundAccessObserveOnly([&](auto & target) { return readEnvVar(target, name); });
}

LookupPathObservation EvalEnvironment::resolveLookupPath(const LookupPathRequest & request)
{
    return dispatchBoundAccessDetached([&](auto & target) { return resolveLookupPath(target, request); });
}

UriPolicyObservation EvalEnvironment::authorizeUri(const UriPolicyRequest & request)
{
    return dispatchBoundDetached([&](auto & target) { return authorizeUri(target, request); });
}

ContextRealisationObservation EvalEnvironment::realiseContext(const RealiseContextRequest & request)
{
    return dispatchBoundAccessDetached([&](auto & target) { return realiseContext(target, request); });
}

PathStatusObservation EvalEnvironment::observePath(const PathObservationRequest & request)
{
    return dispatchBoundAccessDetached([&](auto & target) { return observePath(target, request); });
}

FileReadObservation EvalEnvironment::readFile(const ReadFileRequest & request)
{
    return dispatchBoundAccessDetached([&](auto & target) { return readFile(target, request); });
}

DirectoryReadObservation EvalEnvironment::readDirectory(const ReadDirectoryRequest & request)
{
    return dispatchBoundAccessDetached([&](auto & target) { return readDirectory(target, request); });
}

PublishedStorePathString EvalEnvironment::publishStorePath(const StorePathPublishRequest & request)
{
    return dispatchBoundAccessDetached([&](auto & target) { return publishStorePath(target, request); });
}

PublishedStorePathString EvalEnvironment::copyPathToStore(const CopyPathToStoreRequest & request)
{
    return dispatchBoundAccessDetached([&](auto & target) { return copyPathToStore(target, request); });
}

StorePathObservation EvalEnvironment::authorizeStorePath(const StorePath & path)
{
    return dispatchBoundAccessDetached([&](auto & target) { return authorizeStorePath(target, path); });
}

StoreClosureObservation EvalEnvironment::authorizeStoreClosure(const StorePath & path)
{
    return dispatchBoundAccessDetached([&](auto & target) { return authorizeStoreClosure(target, path); });
}

PublishedStorePathString EvalEnvironment::renderAuthorizedStorePath(
    const AuthorizedStorePathRenderRequest & request)
{
    return dispatchBoundAccessDetached([&](auto & target) { return renderAuthorizedStorePath(target, request); });
}

SessionSystemObservation EvalEnvironment::observeSessionSystem()
{
    return dispatchBoundAccessObserveOnly([&](auto & target) { return observeSessionSystem(target); });
}

} // namespace nix
