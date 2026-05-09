#pragma once
///@file

#include "nix/util/sync.hh"
#include "nix/util/hash.hh"
#include "nix/expr/eval-trace/ids.hh"
#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/expr/eval-trace/cache/trace-backend.hh"
#include "nix/expr/eval-trace/cache/root-handle.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/eval-trace/result.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <vector>

#include <memory>
#include <type_traits>
#include <utility>

namespace nix::eval_trace {

// ── TraceSession ──────────────────────────────────────────────────────

struct TracedExpr;

class TraceSession : public std::enable_shared_from_this<TraceSession>
{
    friend struct TracedExpr;

public:
    struct RootLoadDep {
        CanonicalQueryKind kind;
        DepSource source;
        SimpleDepKeyAtom key;
        DepHashValue hash;
    };

    /// Parameters that only make sense together when the session uses a
    /// trace backend. Fusing them prevents the illegal-state combination
    /// (`useCache=None, sessionConfig=Some`) at the type level — without
    /// this, a caller could pass a `SessionConfig` that would be silently
    /// ignored by the ctor. Test fixtures that construct a
    /// `TraceSession` with a fingerprint but no `SessionConfig`
    /// deliberately exercise the bootstrap path, so `sessionConfig` here
    /// remains optional. See OR-8 in `eval-trace/CLAUDE.md`.
    struct BackendParams {
        std::reference_wrapper<const Hash> fingerprint;
        std::optional<SessionConfig> sessionConfig;
    };

private:

    /**
     * Async-enabled trace backend — owns the SQLite storage plus the
     * io_context / blocking pool / verifier that drive it.
     */
    std::unique_ptr<TraceBackend> runtime_;

    EvalState & state;
    RootHandle root;
    RootValue tracedRoot;

    /**
     * Maps typed flake input sources to their source paths (accessor + base
     * path). Used during trace verification to validate dep hashes against
     * current file content.
     */
    boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> inputAccessors;
    std::vector<RootLoadDep> rootLoadDeps_;
    /// Immutable semantic registry for dep-source-to-SourcePath resolution.
    /// Constructed at session open from resolved graph entries and (future)
    /// session runtime roots. Replaces FrozenInputResolver for all new code paths.
    SemanticRegistry registry_;
    /// Per-child AttrPathId stamp tables for sibling identity tracking.
    /// Dense vectors indexed by AttrPathId::value (monotonic uint32 from
    /// AttrVocabStore). O(1) indexed access vs hash probe.
    /// DefinitionStamp(0) / SlotStamp(0) are sentinels for "unset" since
    /// valid stamps start at 1 (nextDefinitionStamp/nextSlotStamp = 1).
    std::vector<DefinitionStamp> childDefinitionStamps;
    std::vector<SlotStamp> childSlotStamps;
    uint32_t nextDefinitionStamp = 1;
    uint32_t nextSlotStamp = 1;

    /// Parents whose siblings have already been registered during
    /// traverseRealTree. Prevents re-scanning all siblings at a shared
    /// path prefix when multiple children traverse through the same parent.
    /// Without this, N children sharing a parent with M siblings costs
    /// O(N*M) stamp lookups; with it, O(M) total (once per parent).
    boost::unordered_flat_set<AttrPathId, AttrPathId::Hash> registeredSiblingParents;

    DefinitionStamp definitionStampForChildPath(AttrPathId pathId);
    SlotStamp slotStampForChildPath(AttrPathId pathId);
    void recordRootLoadDeps();

public:

    TraceSession(
        std::optional<BackendParams> backendParams,
        EvalState & state,
        RootLoader rootLoader,
        boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> inputAccessors = {},
        boost::unordered_flat_map<CanonPath, std::vector<std::pair<DepSource, RegistryMountSubdir>>> lockedMounts = {},
        std::vector<RootLoadDep> rootLoadDeps = {},
        SemanticRegistry registry = {});

    /// Non-owning pointer to the session's trace backend.
    TraceBackend * traceBackend() const { return runtime_.get(); }
    SemanticRegistry & registry() { return registry_; }
    const SemanticRegistry & registry() const { return registry_; }
    void registerRuntimeRootMount(CanonPath mountPoint, DepSource source, RegistryMountSubdir subdir);

    template<typename F>
    decltype(auto) withDepCaptureScope(F && f)
    {
        DepCaptureScope scope(state.tracingPools(), registry_);
        auto access = TraceAccess::forRecording(state.tracingPools(), *scope.ctx);
        if constexpr (std::is_invocable_v<F, const TraceAccess &>)
            return std::forward<F>(f)(access);
        else
            return std::forward<F>(f)();
    }

    /**
     * Get the real root value via rootLoader, bypassing the trace system.
     * Used to regenerate GC'd .drv files by forcing fresh evaluation
     * (Adapton demand-driven recomputation).
     */
    Value * getRealRoot();

    /**
     * Get the root value. With active trace context, this returns the traced root thunk.
     * On verify hit, that thunk materializes from trace data; on verify miss,
     * it re-evaluates via rootLoader. When the `EvalState` has no trace runtime, it
     * returns the direct real root value to avoid constructing traced thunks.
     */
    Value * getRootValue();

    /**
     * Flush backend/session state for this TraceSession.
     * Called explicitly before exec()/command boundary.
     */
    void flush();

    /**
     * Flush and release the trace backend, leaving this session in a
     * no-backend state. After this call, any TracedExpr thunks still
     * referencing this session will use evaluateDirect() (no trace cache,
     * no SQLite access).
     *
     * This releases the SQLite connection so a new TraceSession can open
     * the same database file. Necessary because ref<TraceSession> in
     * GC-allocated TracedExpr thunks keeps the session alive beyond the
     * caller's scope — without releaseBackend(), the old TraceStore would
     * hold the DB file open indefinitely.
     */
    void releaseBackend();

};

TraceSession * currentTraceSession();

} // namespace nix::eval_trace
