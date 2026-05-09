#pragma once

#include "nix/expr/eval-environment/observation-types.hh"
#include "nix/expr/eval-environment/session-types.hh"

namespace nix {

class Store;

class EvalEnvironment final
{
    std::shared_ptr<void> pImpl;

public:
    explicit EvalEnvironment(const EvalEnvironmentAuthority & authority);
    ~EvalEnvironment();

    EvalEnvironment(const EvalEnvironment &) = delete;
    EvalEnvironment & operator=(const EvalEnvironment &) = delete;

    DetachedEffectScope openDetachedEffectScope();
    std::optional<BoundEffectScope> tryBindCurrentEvalSession();
    void initializeLookupPathAccessControl(
        DetachedEffectScope &,
        const LookupPath & lookupPath);
    CapturedSessionOpenInputs captureSessionOpenInputs(
        DetachedEffectScope &,
        const LookupPath & lookupPath);

    BoundEffectScope openEvalSession(FlakeEvalSessionOpen sessionOpen);
    BoundEffectScope openEvalSession(FileEvalSessionOpen sessionOpen);
    ref<eval_trace::TraceSession> traceSession(const BoundEffectScope & session) const;

    // ── Auto-dispatch overloads ────────────────────────────────────────
    //
    // These try the current bound session first, then fall back to a
    // detached scope.  Most call sites should use these.

    EnvVarObservation readEnvVar(std::string_view name);
    LookupPathObservation resolveLookupPath(const LookupPathRequest & request);
    UriPolicyObservation authorizeUri(const UriPolicyRequest & request);
    ContextRealisationObservation realiseContext(const RealiseContextRequest & request);
    PathStatusObservation observePath(const PathObservationRequest & request);
    FileReadObservation readFile(const ReadFileRequest & request);
    DirectoryReadObservation readDirectory(const ReadDirectoryRequest & request);
    PublishedStorePathString publishStorePath(const StorePathPublishRequest & request);
    PublishedStorePathString copyPathToStore(const CopyPathToStoreRequest & request);
    StorePathObservation authorizeStorePath(const StorePath & path);
    StoreClosureObservation authorizeStoreClosure(const StorePath & path);
    PublishedStorePathString renderAuthorizedStorePath(
        const AuthorizedStorePathRenderRequest & request);
    SessionSystemObservation observeSessionSystem();

    // ── Observe-only overloads ─────────────────────────────────────────
    //
    // Read-only, no recording.  Used by the verification/dep-resolution
    // path and by doc-string loading.

    EnvVarObservation readEnvVar(ObserveOnlyTag, std::string_view name);
    PathStatusObservation observePath(ObserveOnlyTag, const PathObservationRequest & request);
    FileReadObservation readFile(ObserveOnlyTag, const ReadFileRequest & request);
    DirectoryReadObservation readDirectory(ObserveOnlyTag, const ReadDirectoryRequest & request);
    RuntimeFetchIdentityObservation observeRuntimeFetchIdentity(
        ObserveOnlyTag, const FetchIdentityRequest & request);
    GitIdentityObservation observeGitIdentity(ObserveOnlyTag, const GitIdentityRequest & request);
    DerivedStorePathObservation observeDerivedStorePath(
        ObserveOnlyTag, const DerivedStorePathRequest & request);
    SessionSystemObservation observeSessionSystem(ObserveOnlyTag);

    // ── Explicit detached overloads ────────────────────────────────────
    //
    // For callers that hold a DetachedEffectScope and deliberately skip
    // session binding (flake graph resolution, fetch pipelines).

    EnvVarObservation readEnvVar(DetachedEffectScope &, std::string_view name);
    LookupPathObservation resolveLookupPath(
        DetachedEffectScope &, const LookupPathRequest & request);
    UriPolicyObservation authorizeUri(
        DetachedEffectScope &, const UriPolicyRequest & request);
    ContextRealisationObservation realiseContext(
        DetachedEffectScope &, const RealiseContextRequest & request);
    PathStatusObservation observePath(
        DetachedEffectScope &, const PathObservationRequest & request);
    FileReadObservation readFile(DetachedEffectScope &, const ReadFileRequest & request);
    DirectoryReadObservation readDirectory(
        DetachedEffectScope &, const ReadDirectoryRequest & request);
    PublishedStorePathString publishStorePath(
        DetachedEffectScope &, const StorePathPublishRequest & request);
    PublishedStorePathString copyPathToStore(
        DetachedEffectScope &, const CopyPathToStoreRequest & request);
    StorePathObservation authorizeStorePath(DetachedEffectScope &, const StorePath & path);
    StoreClosureObservation authorizeStoreClosure(DetachedEffectScope &, const StorePath & path);
    PublishedStorePathString renderAuthorizedStorePath(
        DetachedEffectScope &, const AuthorizedStorePathRenderRequest & request);
    FetchIdentityResolution resolveFetchIdentity(
        DetachedEffectScope &, const FetchIdentityRequest & request);
    FetchIdentityResolution resolveFetchIdentity(
        BoundEffectScope &, const FetchIdentityRequest & request);
    FetchedInput materializeFetch(DetachedEffectScope &, ResolvedFetchIdentity && identity);
    FetchedInput materializeFetch(BoundEffectScope &, ResolvedFetchIdentity && identity);
    DetachedMountedStorePath ensureMountedStorePath(
        DetachedEffectScope &, const EnsureMountedStorePathRequest & request);
    DetachedStandaloneMountedInput mountFetchedInput(DetachedEffectScope &, FetchedInput && fetched);
    DetachedGraphMountedInput mountGraphFetchedInput(
        DetachedEffectScope &, FetchedInput && fetched, DepSource promotedGraphSource);
    GraphFetchCompletion completeGraphFetch(
        DetachedEffectScope &, DetachedGraphMountedInput && mountedInput);
    RuntimeFetchResult mountAndCompleteRuntimeFetch(
        BoundEffectScope && session, FetchedInput && fetched);
    GitIdentityObservation observeGitIdentity(
        DetachedEffectScope &, const GitIdentityRequest & request);

private:
    template<typename SessionReuse>
    BoundEffectScope openEvalSessionImpl(
        EvalTraceSessionAuthority authority,
        SessionReuse sessionReuse);
    static PublishedStorePathString makePublishedStorePathString(
        const Store & store,
        StorePath storePath,
        StorePathPublicationMode publicationMode,
        std::optional<PathObject> provenance = std::nullopt);

    // ── Auto-dispatch priority chains ──────────────────────────────────
    //
    // Each auto-dispatch public entry calls the session-bound overload
    // first, then the raw TraceAccess overload, then a fallback. The
    // three chain shapes below factor out the identical boilerplate.
    // Definitions live in eval-environment.cc so TraceAccess's full
    // type need not be visible in this header.
    //
    // dispatchBoundAccessDetached: most observation methods (readFile,
    // readDirectory, etc.) — opens a fresh DetachedEffectScope as the
    // terminal fallback, since the detached overload performs real
    // filesystem I/O.
    //
    // dispatchBoundDetached: authorizeUri — session-bound or detached;
    // there is no separate TraceAccess overload.
    //
    // dispatchBoundAccessObserveOnly: readEnvVar, observeSessionSystem
    // — the fallback is observeOnly (no filesystem), since the values
    // are already in hand by the time auto-dispatch is reached.
    template<typename F>
    decltype(auto) dispatchBoundAccessDetached(F && fn);

    template<typename F>
    decltype(auto) dispatchBoundDetached(F && fn);

    template<typename F>
    decltype(auto) dispatchBoundAccessObserveOnly(F && fn);

    // ── Bound → TraceAccess forwarder ──────────────────────────────────
    //
    // Every Bound overload in this class is structurally identical:
    // `withRecordingAccess(session.session_, [&](access){ return opImpl(access, ...); })`.
    // `boundRecord` factors that boilerplate into one template so the
    // 16 Bound overload bodies each become a single expression.
    // Definition lives in eval-environment.cc.
    template<typename F>
    decltype(auto) boundRecord(BoundEffectScope & session, F && fn);

    // ── Bound overloads (session-recording) ────────────────────────────
    EnvVarObservation readEnvVar(BoundEffectScope &, std::string_view name);
    LookupPathObservation resolveLookupPath(
        BoundEffectScope &, const LookupPathRequest & request);
    UriPolicyObservation authorizeUri(
        BoundEffectScope &, const UriPolicyRequest & request);
    ContextRealisationObservation realiseContext(
        BoundEffectScope &, const RealiseContextRequest & request);
    PathStatusObservation observePath(
        BoundEffectScope &, const PathObservationRequest & request);
    FileReadObservation readFile(BoundEffectScope &, const ReadFileRequest & request);
    DirectoryReadObservation readDirectory(
        BoundEffectScope &, const ReadDirectoryRequest & request);
    PublishedStorePathString publishStorePath(
        BoundEffectScope &, const StorePathPublishRequest & request);
    PublishedStorePathString copyPathToStore(
        BoundEffectScope &, const CopyPathToStoreRequest & request);
    StorePathObservation authorizeStorePath(BoundEffectScope &, const StorePath & path);
    StoreClosureObservation authorizeStoreClosure(BoundEffectScope &, const StorePath & path);
    PublishedStorePathString renderAuthorizedStorePath(
        BoundEffectScope &, const AuthorizedStorePathRenderRequest & request);
    GitIdentityObservation observeGitIdentity(
        BoundEffectScope &, const GitIdentityRequest & request);
    DerivedStorePathObservation observeDerivedStorePath(
        BoundEffectScope &, const DerivedStorePathRequest & request);
    SessionSystemObservation observeSessionSystem(BoundEffectScope &);

    // ── TraceAccess overloads (direct recording) ───────────────────────
    PathStatusObservation observePath(
        const eval_trace::TraceAccess &, const PathObservationRequest & request);
    FileReadObservation readFile(
        const eval_trace::TraceAccess &, const ReadFileRequest & request);
    DirectoryReadObservation readDirectory(
        const eval_trace::TraceAccess &, const ReadDirectoryRequest & request);
    ContextRealisationObservation realiseContext(
        const eval_trace::TraceAccess &, const RealiseContextRequest & request);
    PublishedStorePathString publishStorePath(
        const eval_trace::TraceAccess &, const StorePathPublishRequest & request);
    PublishedStorePathString copyPathToStore(
        const eval_trace::TraceAccess &, const CopyPathToStoreRequest & request);
    SessionSystemObservation observeSessionSystem(const eval_trace::TraceAccess &);
    EnvVarObservation readEnvVar(
        const eval_trace::TraceAccess &, std::string_view name);
    LookupPathObservation resolveLookupPath(
        const eval_trace::TraceAccess &, const LookupPathRequest & request);
    StorePathObservation authorizeStorePath(
        const eval_trace::TraceAccess &, const StorePath & path);
    StoreClosureObservation authorizeStoreClosure(
        const eval_trace::TraceAccess &, const StorePath & path);
    PublishedStorePathString renderAuthorizedStorePath(
        const eval_trace::TraceAccess &, const AuthorizedStorePathRenderRequest & request);

    // ── Runtime fetch completion (private) ─────────────────────────────
    LockedPublishedRuntimeFetch completeLockedRuntimeFetch(
        BoundEffectScope &&,
        BoundLockedMountedInput && mountedInput);
    UnlockedPublishedRuntimeFetch completeUnlockedRuntimeFetch(
        BoundEffectScope &&,
        BoundUnlockedMountedInput && mountedInput,
        OriginalFetchInput originalInput);
};

} // namespace nix
