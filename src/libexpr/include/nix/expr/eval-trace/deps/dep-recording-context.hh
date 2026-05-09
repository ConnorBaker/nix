#pragma once
/// dep-recording-context.hh — Per-fiber dependency recording context.
///
/// Each evaluation fiber owns one DepRecordingContext. Nested scopes
/// (child attr eval within the same fiber) are a stack within the
/// context. The epoch log is per-context; in production it wraps
/// MemoReplayStore::epochLog_ and in test/fallback paths it wraps
/// DepCaptureScope's own fallbackEpochLog_ member vector.

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/util/gdp/proof.hh"
#include "nix/util/singleton/dispatch.hh"

#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

namespace nix {

struct Value;
struct SiblingForceScope;
struct PublicationWarmupScope;
namespace eval_trace { class SemanticRegistry; }

namespace eval_trace {
struct DepCaptureScope;
class RecordingScopeGuard;
namespace test { struct TestScopeAccess; }
}

/// Reason gating an EpochLog write. Both reasons authorise a push_back on
/// `MemoReplayStore::epochLog_`; the singleton::Tag specialisations below
/// make the two proofs non-interconvertible in the type system.
///
///   DedupPassed   — recording path: the scope dedup check ran and the
///                   dep must be appended.
///   ReplayActive  — replay path: a recording scope is active, and
///                   replayed deps populate the log for thunk memoization
///                   indexing (no dedup — replayed deps are stored trace
///                   deps).
enum class EpochLogWriteReason : uint8_t {
    DedupPassed,
    ReplayActive,
};

template<EpochLogWriteReason R>
using EpochLogWriteProof = gdp::Proof<singleton::Tag<R>>;

/// Unified certifier for epoch log write proofs. Only exposes the two
/// conditional (ifPassed/ifActive) forms — unconditional minting is
/// deliberately NOT exposed. This prevents creating a proof without its
/// precondition, which is the invariant violation that caused the session
/// 91 cold regression.
///
/// Each continuation receives its typed proof and is ONLY called when the
/// condition is true. Both epochLog and ownDeps writes must be inside the
/// continuation, making them structurally inseparable from the decision.
class EpochLogWriteCertifier
    : private gdp::Certifier<singleton::Tag<EpochLogWriteReason::DedupPassed>>
    , private gdp::Certifier<singleton::Tag<EpochLogWriteReason::ReplayActive>>
{
    friend struct DepRecordingContext;
    friend class eval_trace::RecordingScopeGuard;

    /// Gate an epoch log write on a dedup condition. Called only when
    /// `dedupPassed` is true. Both epochLog and ownDeps writes belong
    /// inside the continuation — they can't be reordered outside it
    /// because the proof only exists within the continuation.
    ///
    /// Friend-only (DepRecordingContext): the recording write path is
    /// narrowly accessed because creating a DedupPassed proof without
    /// running the dedup check is the invariant violation we defend.
    template<typename F>
    static auto ifDedupPassed(bool dedupPassed, F && f) {
        using DedupCert = gdp::Certifier<singleton::Tag<EpochLogWriteReason::DedupPassed>>;
        return DedupCert::withProofIf(dedupPassed, std::forward<F>(f));
    }

    /// Gate a replay write on a recording scope being active. Called only
    /// when `replayActive` is true.
    ///
    /// Friend-only (RecordingScopeGuard): callers go through
    /// RecordingScopeGuard::ifActive which checks the fiber-local state
    /// and calls this with the right condition.
    template<typename F>
    static auto ifReplayActive(bool replayActive, F && f) {
        using ReplayCert = gdp::Certifier<singleton::Tag<EpochLogWriteReason::ReplayActive>>;
        return ReplayCert::withProofIf(replayActive, std::forward<F>(f));
    }
};

/// Wrapper around the epoch log vector reference. The underlying vector is
/// private with NO friend declarations. Two GDP-guarded write paths, each
/// taking a distinct proof type:
///
///   append(EpochLogWriteProof<DedupPassed>, dep)  — recording (dedup enforced)
///   appendReplayed(EpochLogWriteProof<ReplayActive>, dep) — replay (active scope required)
///
/// MemoReplayStore::epochLog_ is the authoritative owner of the underlying
/// vector. All writes go through this wrapper — no raw push_back available.
struct EpochLogRef {
private:
    std::vector<Dep> & log_;

public:
    explicit EpochLogRef(std::vector<Dep> & log) : log_(log) {}

    uint32_t size() const { return static_cast<uint32_t>(log_.size()); }

    /// Read-only access for external consumers (test assertions, epoch
    /// size queries). Does NOT allow mutation.
    const std::vector<Dep> & storage() const { return log_; }

    void truncate(uint32_t size) {
        if (size >= log_.size())
            return;
        log_.erase(log_.begin() + size, log_.end());
    }

    /// Recording write path: requires proof of scope dedup check.
    void append(const EpochLogWriteProof<EpochLogWriteReason::DedupPassed> &, const Dep & dep) {
        log_.push_back(dep);
    }

    /// Replay write path: requires proof that a recording scope is active.
    /// Used by replayTrace to populate epoch log for thunk memoization
    /// indexing. No dedup — replayed deps are stored trace deps.
    void appendReplayed(const EpochLogWriteProof<EpochLogWriteReason::ReplayActive> &, const Dep & dep) {
        log_.push_back(dep);
    }
};

/// Per-task dependency recording context.
///
/// Each evaluation task owns one context for its root scope. Nested
/// scopes (child attr eval within the same task) are a stack within
/// the context.
///
/// The epochLog member wraps a reference to an externally-owned vector.
/// In production (fiber path), this references MemoReplayStore::epochLog_
/// (per-EvalState, long-lived). In tests and fallback paths, it references
/// DepCaptureScope's own member vector (fallbackEpochLog_).
///
/// Epoch log writes are GDP-guarded via EpochLogWriteCertifier::ifDedupPassed
/// (conditional). Both epochLog and ownDeps writes are inside the proof
/// continuation, structurally tied to the dedup decision. The certifier only
/// exposes conditional minters — unconditional proof creation is not available.
struct DepRecordingContext {
    InterningPools & pools;

    /// Per-scope state. Each scope has its own dep vector (structural
    /// isolation, same as DepRecordingContext). pushScope() on nested attr
    /// eval, popScope() on return.
    struct Scope {
        /// GDP-guarded mount resolution for dep provenance.
        /// Set by DepCaptureScope when pushing the scope.
        /// Null for test scopes without mount resolution.
        const eval_trace::SemanticRegistry * registry = nullptr;

        /// Per-scope dep vector: structurally isolated from other scopes.
        std::vector<Dep> ownDeps;

        /// Deduplication map: prevents adding the same dep key twice
        /// within a scope. Conflicting hash values mark the scope unstable.
        boost::unordered_flat_map<
            Dep::Key, DepHashValue, Dep::Key::Hash> seenDeps;

        /// Deduplication set: prevents replaying the same memoized dep
        /// range twice when a Value is re-forced.
        ///
        /// Value* points into the GC heap. Keep the table storage traceable so
        /// a replayed value cannot be reclaimed and its address reused during
        /// the same recording scope, which would incorrectly suppress replay
        /// for a different value.
        boost::unordered_flat_set<
            const Value *,
            boost::hash<const Value *>,
            std::equal_to<const Value *>,
            traceable_allocator<const Value *>>
            replayedValues;

        /// Index into epochLog at scope construction time.
        uint32_t epochLogStartIndex = 0;

        /// Set when conflicting hash values are observed for the same
        /// dep key within this scope.
        bool unstable = false;
    };

    /// Stack of scopes. The bottom element is the root scope.
    std::vector<Scope> scopeStack;

    /// GDP-guarded epoch log wrapper. For production paths, wraps
    /// MemoReplayStore::epochLog_ (per-EvalState). Writes require an
    /// EpochLogWriteProof<DedupPassed> (recording) or
    /// EpochLogWriteProof<ReplayActive> (replay), each created only
    /// after its precondition is checked.
    EpochLogRef epochLog;

    // ── Construction / destruction ───────────────────────────────────

    /// GDP tag: proof that a DepCaptureScope is being constructed.
    /// Only DepCaptureScope (which privately inherits Certifier<DepCaptureScopeTag>)
    /// can create this proof. Prevents anyone from pushing scopes outside
    /// DepCaptureScope — including the constructor, which cannot push a
    /// "root scope" that changes dep recording behavior.
    struct DepCaptureScopeTag {};

    /// Construct with an explicit epoch log reference.
    /// NO pushScope — context starts with empty scopeStack.
    /// Between DepCaptureScope activations, currentScope() returns null
    /// and deps go to epochLog only (matching old DepRecordingScope behavior).
    DepRecordingContext(InterningPools & pools, std::vector<Dep> & epochLog)
        : pools(pools)
        , epochLog(epochLog)
    {}

    ~DepRecordingContext() = default;

    DepRecordingContext(const DepRecordingContext &) = delete;
    DepRecordingContext & operator=(const DepRecordingContext &) = delete;
    DepRecordingContext(DepRecordingContext &&) = delete;
    DepRecordingContext & operator=(DepRecordingContext &&) = delete;

    // ── Read-only scope queries (public) ─────────────────────────────

    Scope * currentScope()
    {
        if (scopeStack.empty())
            return nullptr;
        return &scopeStack.back();
    }

    const Scope * currentScope() const
    {
        if (scopeStack.empty())
            return nullptr;
        return &scopeStack.back();
    }

    bool isActive() const { return !scopeStack.empty(); }

    uint32_t depth() const { return static_cast<uint32_t>(scopeStack.size()); }

    /// Returns true if the current scope's seenDeps already contains
    /// `key`. Callers use this to skip expensive per-access work (file
    /// hash computation, registry lookup) when the dep would be
    /// deduplicated anyway.
    ///
    /// With an empty scope stack (between DepCaptureScope activations),
    /// returns false so callers fall through to the normal recording
    /// path.
    bool scopeContainsDepKey(const Dep::Key & key) const
    {
        const auto * scope = currentScope();
        return scope && scope->seenDeps.contains(key);
    }

private:
    void recordInterned(
        CanonicalQueryKind type,
        DepSourceId sourceId,
        SimpleDepKeyId keyId,
        DepHashValue hash,
        RepoRootId governingRepoId);

public:
    // ── Recording (public — deps go to epochLog AND ownDeps together,
    //    or neither, based on scope dedup) ────────────────────────────

    void record(CanonicalQueryKind type, const DepSource & source,
                const SimpleDepKeyAtom & key, DepHashValue hash,
                RepoRootId governingRepoId = {});
    void record(const DepSource & source, const DerivedStorePathDepKey & key, DepHashValue hash,
                RepoRootId governingRepoId = {});
    void record(const DepSource & source, const StorePathAvailabilityDepKey & key, DepHashValue hash);
    void record(const DepSource & source, const RuntimeFetchIdentityDepKey & key, DepHashValue hash);

    void record(const Dep & dep);

    // ── Replay (public — memoization needs this from any call site) ──

    bool replayMemoizedRange(const Value & value, const DepRange & range)
    {
        auto * scope = currentScope();
        if (!scope)
            return false;
        if (range.start >= scope->epochLogStartIndex) {
            // Count NarIdentity in blocked range for diagnosis
            size_t narCount = 0;
            for (uint32_t i = range.start; i < range.end; ++i)
                if ((*range.deps)[i].key.kind == CanonicalQueryKind::NarIdentity)
                    ++narCount;
            if (narCount > 100)
                debug("eval-trace/deps: replay-guard blocked range [%d,%d) (%zu NarIdentity), scope.epochLogStartIndex=%d, Value@%p",
                    range.start, range.end, narCount, scope->epochLogStartIndex, &value);
            return false;
        }
        if (!scope->replayedValues.insert(&value).second)
            return false;
        scope->ownDeps.insert(
            scope->ownDeps.end(),
            range.deps->begin() + range.start,
            range.deps->begin() + range.end);
        return true;
    }

    bool isStable() const
    {
        auto * scope = currentScope();
        return scope ? !scope->unstable : true;
    }

private:
    static bool observeRecordedDep(Scope & scope, const Dep & dep);

    // ── Scope management (private + GDP-guarded) ─────────────────────
    //
    // Private: external code can't call these (compile error).
    // GDP proof: even friends need the proof — the constructor has none,
    // preventing the "root scope in constructor" regression.
    // Only DepCaptureScope and SiblingForceScope can call them.

    friend struct eval_trace::DepCaptureScope;
    friend struct SiblingForceScope;
    friend struct PublicationWarmupScope;
    // Test-only access via TestScopeAccess in helpers.hh.
    friend struct eval_trace::test::TestScopeAccess;

    void pushScope(const gdp::Proof<DepCaptureScopeTag> &)
    {
        Scope scope;
        scope.epochLogStartIndex = static_cast<uint32_t>(epochLog.size());
        // Inherit mount gate from parent scope so nested scopes
        // (SiblingReplayCaptureScope, SiblingForceScope) can resolve provenance.
        if (!scopeStack.empty())
            scope.registry = scopeStack.back().registry;
        scopeStack.push_back(std::move(scope));
        eval_trace::nrDepContextScopes++;
    }

    void popScope(const gdp::Proof<DepCaptureScopeTag> &)
    {
        assert(!scopeStack.empty());
        scopeStack.pop_back();
    }

    // Ordering invariant: materializeResult must complete before replayTrace.
    // materializeResult constructs Value thunks; replayTrace populates the
    // epoch log for those thunks. If materializeResult forced thunks (it
    // currently does not), their deps would be recorded before replay,
    // corrupting the epoch log.
    std::vector<Dep> takeDeps(const gdp::Proof<DepCaptureScopeTag> &)
    {
        auto * scope = currentScope();
        assert(scope && "takeDeps called with no active scope");
        auto deps = std::move(scope->ownDeps);
        if (Counter::enabled) {
            auto sz = static_cast<uint64_t>(deps.size());
            eval_trace::nrOwnDepsTotal += sz;
            auto cur = eval_trace::nrOwnDepsMax.load();
            while (sz > cur && !eval_trace::nrOwnDepsMax.inner.compare_exchange_weak(cur, sz))
                ;
        }
        return deps;
    }
};

} // namespace nix
