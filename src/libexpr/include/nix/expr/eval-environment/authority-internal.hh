#pragma once

#include "nix/expr/eval-environment/capabilities.hh"
#include "nix/expr/eval-environment/observation-types.hh"
#include "nix/expr/eval-environment/session-types.hh"

namespace nix {

class EvalState;
struct EvalEnvironmentSharedState;
struct GitIdentityObservation;

namespace eval_trace {
struct SessionConfig;
class TraceSession;
}

EvalEnvironmentAuthority makeDetachedEvalEnvironmentAuthority(EvalState & state);
EvalEnvironmentAuthority makeSessionEvalEnvironmentAuthority(EvalState & state);
void clearEvalEnvironmentState(EvalState & state);
void releaseSessionEvalEnvironmentState(EvalState & state);

/// Allow read access to @p storePath via the authority's rootFS allowlist.
void allowPath(const EvalEnvironmentAuthority & authority, const StorePath & storePath);
/// Allow read access to @p root and its entire FS closure.
void allowClosure(const EvalEnvironmentAuthority & authority, const StorePath & root);
eval_trace::SessionConfig toTraceSessionConfig(const EvalTraceSessionConfigInput & input);
std::optional<FileEvalGitIdentitySnapshot> buildFileEvalSessionConfigInputs(
    const GitIdentityObservation & observation);
FlakeEvalSessionOpen assembleFlakeTraceSessionOpen(
    CapturedSessionOpenInputs sessionInputs,
    FlakeGraphTraceSessionAuthorityRequest authorityRequest,
    const FlakeTraceSessionConfigRequest & sessionConfigRequest);
FileEvalSessionOpen assembleFileEvalTraceSessionOpen(
    CapturedSessionOpenInputs sessionInputs,
    CommonTraceSessionAuthorityInputs inputs,
    const std::optional<FileEvalGitIdentitySnapshot> & sessionGitIdentity,
    const FileEvalTraceSessionReuseKeyInputs & reuseKeyInputs);

/// Copy a source path to the store with derivation-name validation and
/// the already-in-store fast path.  Merges the resulting string context
/// into @p context.  Used by coercion paths and value-to-json.
PublishedStorePathString copyPathToStoreViaEvalEnvironment(
    EvalState & state,
    NixStringContext & context,
    const SourcePath & path,
    std::optional<PathObject> origin = std::nullopt);

/// Realise a string value: coerce to string, realise its context (building
/// derivations if @p isIFD), rewrite placeholders to store paths.
/// If @p storePathsOutMaybe is non-null, all referenced store paths are
/// added to it.  Used by the C API nix_string_realise.
std::string realiseStringViaEvalEnvironment(
    EvalState & state,
    Value & str,
    StorePathSet * storePathsOutMaybe = nullptr,
    bool isIFD = true,
    const PosIdx pos = noPos);

class TraceSessionFactory
{
public:
    virtual ~TraceSessionFactory() = default;

    virtual ref<eval_trace::TraceSession> openTraceSession(
        std::optional<TraceSessionReuseSlotKey> reuseKey,
        EvalTraceSessionAuthority authority) = 0;
};

} // namespace nix
