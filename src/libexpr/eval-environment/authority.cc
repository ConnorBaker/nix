#include "nix/expr/eval-environment/authority-internal.hh"

#include "nix/expr/eval-environment/session-types.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval-trace/hash-spec.hh"
#include "nix/expr/eval.hh"
#include "nix/fetchers/input-cache.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <cstring>
#include <concepts>
#include <functional>
#include <mutex>
#include <type_traits>

namespace nix {

namespace {

std::optional<EvalTraceHash> semanticSessionDigest(const std::optional<EvalTraceSessionConfigInput> & config)
{
    if (!config)
        return std::nullopt;
    return toTraceSessionConfig(*config).buildSemanticSessionKey().digest;
}

std::optional<Hash> buildBackendFingerprint(
    const std::optional<EvalTraceHash> & digest)
{
    if (!digest)
        return std::nullopt;

    Hash hash(eval_trace::toHashAlgorithm(eval_trace::getEvalTraceHashAlgorithm()));
    std::memcpy(hash.hash, digest->bytes.data(), digest->bytes.size());
    return hash;
}

std::vector<eval_trace::TraceSession::RootLoadDep> toRootLoadDeps(
    std::vector<RootLoadDepObservation> observations)
{
    std::vector<eval_trace::TraceSession::RootLoadDep> deps;
    deps.reserve(observations.size());
    for (auto & observation : observations) {
        deps.push_back(eval_trace::TraceSession::RootLoadDep{
            .kind = observation.kind,
            .source = std::move(observation.source),
            .key = std::move(observation.key),
            .hash = std::move(observation.hash),
        });
    }
    return deps;
}

ref<eval_trace::TraceSession> makeTraceSession(
    EvalState & state,
    EvalTraceSessionAuthority::OpenPayload && payload,
    const std::optional<EvalTraceHash> & semanticDigest)
{
    boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> inputAccessorMap;
    boost::unordered_flat_map<CanonPath, std::vector<std::pair<DepSource, RegistryMountSubdir>>> mountToInput;

    for (const auto & binding : payload.inputAccessors)
        inputAccessorMap.emplace(binding.source, binding.path);

    for (const auto & binding : payload.mountedInputs)
        mountToInput[binding.mountPoint].push_back({
            binding.source,
            binding.subdir,
        });

    // Fuse fingerprint + session config into a single optional so the
    // illegal combination (`useCache=Some, sessionConfig=None`) is
    // unrepresentable at the type level. In this production path the
    // two inputs are derived from the same semantic digest
    // (`semanticDigest` drives `buildBackendFingerprint`, and
    // `payload.sessionConfig` is the policy half of that same digest),
    // so they are always either both present or both absent. See OR-8
    // closure note in `eval-trace/CLAUDE.md`.
    auto backendFingerprint = buildBackendFingerprint(semanticDigest);
    assert(!backendFingerprint == !payload.sessionConfig
        && "OR-8 invariant: fingerprint and sessionConfig must agree in production");
    std::optional<eval_trace::TraceSession::BackendParams> backendParams;
    if (backendFingerprint && payload.sessionConfig) {
        backendParams = eval_trace::TraceSession::BackendParams{
            *backendFingerprint,
            toTraceSessionConfig(*payload.sessionConfig),
        };
    }

    return make_ref<eval_trace::TraceSession>(
        std::move(backendParams),
        state,
        std::move(payload.rootLoader).intoRootLoader(),
        std::move(inputAccessorMap),
        std::move(mountToInput),
        toRootLoadDeps(std::move(payload.rootLoadDeps)),
        std::move(payload.registrySeed));
}

class EvalStateTraceSessionFactory final : public TraceSessionFactory
{
    struct CacheEntry {
        std::optional<EvalTraceHash> semanticSessionDigest;
        ref<eval_trace::TraceSession> session;
    };

    EvalState & state_;
    boost::unordered_flat_map<TraceSessionReuseSlotKey, CacheEntry, TraceSessionReuseSlotKey::Hash> sessionCache_;

public:
    explicit EvalStateTraceSessionFactory(EvalState & state)
        : state_(state)
    {
    }

    ref<eval_trace::TraceSession> openTraceSession(
        std::optional<TraceSessionReuseSlotKey> reuseKey,
        EvalTraceSessionAuthority authority) override
    {
        auto payload = std::move(authority).consumeForTraceSessionOpen();
        auto newSemanticDigest = semanticSessionDigest(payload.sessionConfig);

        // Backend lifetime tracking applies only to file-eval paths.
        // Flake backend reuse is stateless — keyed entirely by the
        // fingerprint passed to TraceSession, not by in-memory session
        // caching. Reopen-style callers (REPL :reload) need fresh root
        // and graph bindings every time.
        bool trackBackend = reuseKey
            && !std::holds_alternative<FlakeSessionReuseKey>(reuseKey->value);

        // Release the old backend if the semantic namespace changed.
        if (trackBackend) {
            if (auto it = sessionCache_.find(*reuseKey); it != sessionCache_.end()) {
                if (it->second.semanticSessionDigest != newSemanticDigest) {
                    it->second.session->releaseBackend();
                    sessionCache_.erase(it);
                }
            }
        }

        // Always create a fresh TraceSession. In-memory TraceSession objects
        // are never reused — their SemanticRegistry/input bindings are tied
        // to the specific open and would keep relative file deps bound to
        // stale source roots. Backend/session identity reuse happens via the
        // semantic digest fingerprint, not by sharing TraceSession objects.
        auto session = makeTraceSession(
            state_, std::move(payload), newSemanticDigest);

        // Keep the session alive for backend connection reuse.
        if (trackBackend) {
            sessionCache_.insert_or_assign(*reuseKey, CacheEntry{
                .semanticSessionDigest = std::move(newSemanticDigest),
                .session = session,
            });
        }

        return session;
    }
};

std::mutex & sessionFactoryMutex()
{
    static std::mutex mutex;
    return mutex;
}

boost::unordered_flat_map<EvalState *, std::shared_ptr<EvalStateTraceSessionFactory>> & sessionFactories()
{
    static boost::unordered_flat_map<EvalState *, std::shared_ptr<EvalStateTraceSessionFactory>> factories;
    return factories;
}

EvalEnvironmentAuthority makeBaseEvalEnvironmentAuthority(
    EvalState & state,
    const std::shared_ptr<EvalEnvironmentSharedState> & sharedState)
{
    return EvalEnvironmentAuthority{
        .evalState = &state,
        .store = state.store,
        .buildStore = state.buildStore,
        .fetchSettings = state.fetchSettings,
        .evalSettings = state.settings,
        .repair = state.repair,
        .storeFS = state.storeFS,
        .rootFS = state.rootFS,
        .corepkgsFS = state.corepkgsFS,
        .internalFS = state.internalFS,
        .inputCache = sharedState->inputCache,
        .lookupPathHookResolver = [&state](std::string_view scheme, std::string_view rest) -> std::optional<SourcePath> {
            if (auto * hook = get(state.settings.lookupPathHooks, std::string(scheme)))
                return (*hook)(state, rest);
            return std::nullopt;
        },
        .traceSessionFactory = nullptr,
        .sharedState = sharedState,
    };
}

std::shared_ptr<EvalStateTraceSessionFactory> getOrCreateSessionFactory(EvalState & state)
{
    std::lock_guard guard(sessionFactoryMutex());
    auto & factories = sessionFactories();
    if (auto it = factories.find(&state); it != factories.end())
        return it->second;

    auto created = std::make_shared<EvalStateTraceSessionFactory>(state);
    factories.emplace(&state, created);
    return created;
}

} // namespace

EvalEnvironmentAuthority makeDetachedEvalEnvironmentAuthority(EvalState & state)
{
    return makeBaseEvalEnvironmentAuthority(state, state.evalEnvironmentSharedState);
}

EvalEnvironmentAuthority makeSessionEvalEnvironmentAuthority(EvalState & state)
{
    auto authority = makeBaseEvalEnvironmentAuthority(state, state.evalEnvironmentSharedState);
    authority.traceSessionFactory = getOrCreateSessionFactory(state);
    return authority;
}

void releaseSessionEvalEnvironmentState(EvalState & state)
{
    {
        std::lock_guard guard(sessionFactoryMutex());
        sessionFactories().erase(&state);
    }
}

void clearEvalEnvironmentState(EvalState & state)
{
    auto & sharedState = *state.evalEnvironmentSharedState;
    sharedState.lookupPathResolved->clear();
    sharedState.srcToStore->clear();
    sharedState.importResolutionCache->clear();
    sharedState.fileTraceCache->clear();
    sharedState.inputCache->clear();
}

} // namespace nix
