#pragma once
/// store/trace-storage.hh — Non-polymorphic state scaffolding shared by
/// every concrete trace-storage backend.
///
/// Rearchitecture-proposal.md §14 step 1 / §2.1 originally made this a
/// virtual abstract base.  That was wrong for three reasons:
///
///   1. Every production call site holds a concrete `SqliteTraceStorage &`
///      — there is no polymorphic `TraceStorage*` dispatch site
///      anywhere in production code.  The 23 virtuals only existed for
///      `InMemoryTraceRows`, which is used by four tests in
///      `store/in-memory-rows-backend.cc`.  A vtable and vptr per
///      instance were paid for zero dispatch calls.
///
///   2. Introducing a vptr shifted every field offset in
///      `SqliteTraceStorage`, which measurably perturbed register
///      allocation in the hot `verifyTrace` loop (see benchmark
///      `cold/2` vs `cold/1` pairwise analysis).
///
///   3. The "abstract storage API" concept that tests and tools
///      depended on is still preserved — see the `TraceStorageLike`
///      C++20 concept below.  Any class shaped like a storage backend
///      satisfies it and can be passed to `template<TraceStorageLike
///      S>` code without inheritance.
///
/// What the base now does:
///
///   * Owns `storeMutex_` (non-recursive `std::mutex`), the session
///     identity slots (`semanticSessionKey_`, `stableRecoveryKey_`),
///     the `SessionConfig` SetOnce slot, and the `NodeStamp` counter.
///
///   * Provides `withExclusiveAccess(bs, f)` — the sole minter of
///     `ExclusiveTraceStorageAccess`.
///
///   * Provides `allocateNodeStamp`, `currentSemanticSessionKey`,
///     `currentStableRecoveryKey`, `hasSessionConfig` — all
///     non-virtual inline accessors.
///
///   * Provides `raiseNextNodeStamp` + `storeSessionConfig` for
///     concrete backends to use when they bulk-load.
///
/// Concrete backends (`SqliteTraceStorage`, `InMemoryTraceRows`)
/// publicly inherit and add their own methods with identical names to
/// what the old virtuals had.  Call sites that hold the concrete type
/// get direct calls; the `TraceStorageLike` concept validates that a
/// template parameter has the expected shape.
///
/// `TraceObserver` remains a genuine virtual interface: `Recorder`
/// takes an `observer` pointer that may be null or point at a concrete
/// observer (`Verifier` implements the interface).  That runtime
/// polymorphism is a different axis from storage-backend substitution
/// and is kept.

#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/eval-trace/store/session-identity.hh"
#include "nix/expr/eval-trace/store/trace-value-types.hh"
#include "nix/util/gdp/proof.hh"

#include <atomic>
#include <concepts>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace nix {
class Store;
}

namespace nix::eval_trace {

struct BlockingTag;
class TraceStorageBase;

/// Capability token: "I have exclusive access to THIS storage's
/// mutable state." Minted only by
/// `TraceStorageBase::withExclusiveAccess(bs, f)`.
///
/// Lifetime-bound to the minting callback; copying and moving are
/// deleted so the capability cannot escape.
class ExclusiveTraceStorageAccess {
    const gdp::Proof<BlockingTag> & bs_;

    explicit ExclusiveTraceStorageAccess(const gdp::Proof<BlockingTag> & bs)
        : bs_(bs) {}

    friend class TraceStorageBase;

public:
    ExclusiveTraceStorageAccess(const ExclusiveTraceStorageAccess &) = delete;
    ExclusiveTraceStorageAccess(ExclusiveTraceStorageAccess &&) = delete;
    ExclusiveTraceStorageAccess & operator=(const ExclusiveTraceStorageAccess &) = delete;
    ExclusiveTraceStorageAccess & operator=(ExclusiveTraceStorageAccess &&) = delete;

    /// `ea` implies `bs`: the mutex-holding thread is also the blocking
    /// thread. Callers pass `blockingProof()` to non-storage blocking
    /// operations (filesystem, daemon IPC, git).
    ///
    /// MUST NOT be used to call `storage.withExclusiveAccess(bs, ...)`
    /// on the same storage — the non-recursive mutex deadlocks.
    const gdp::Proof<BlockingTag> & blockingProof() const noexcept { return bs_; }
};

/// Observer interface for trace-recording callbacks.  Legitimately
/// virtual: `Recorder` accepts an optional `observer` pointer and
/// `Verifier` implements it to update session-scoped caches when the
/// recording pipeline publishes.  A typical session may have zero or
/// one live observer; the virtual call per recording event is not in a
/// verifier hot loop.
struct TraceObserver {
    virtual ~TraceObserver() = default;
    virtual void onNewTrace(
        TraceId,
        const TraceHeader &,
        const std::vector<Dep> & fullDeps,
        DepKeySetId,
        const std::vector<Dep::Key> & keys) = 0;
    virtual void onPublishCurrent(AttrPathId, const CurrentNodeRef &) = 0;
};

/// Non-polymorphic state base shared by every trace-storage backend.
/// Concrete backends inherit and add their data-path methods directly
/// (no virtuals).  Instances of concrete backends get to keep their
/// member layout tight — no vptr, no vtable, no virtual dispatch.
class TraceStorageBase {
public:
    TraceStorageBase(const TraceStorageBase &) = delete;
    TraceStorageBase & operator=(const TraceStorageBase &) = delete;

    // Non-polymorphic: the dtor is explicitly non-virtual and
    // protected so nobody can ever delete through a base pointer.
    // Base pointers are taken only by the `thread_local` re-entrancy
    // detector (which never owns them) and by test helpers that never
    // delete through them.

    // ── Session identity ────────────────────────────────────────────

    const SemanticSessionKey & currentSemanticSessionKey() const noexcept
    { return semanticSessionKey_; }

    const SessionRecoveryKey & currentStableRecoveryKey() const noexcept
    { return stableRecoveryKey_; }

    /// Allocate a fresh NodeStamp. Monotonic per-process-lifetime;
    /// collisions across process boundaries are avoided by the
    /// concrete backend's bulk-load path raising the floor to
    /// `max(node_stamp) + 1`.
    NodeStamp allocateNodeStamp() noexcept
    { return NodeStamp(nextNodeStamp_.fetch_add(1, std::memory_order_relaxed)); }

    /// True once `setSessionConfig` has been called successfully.
    bool hasSessionConfig() const noexcept { return sessionConfig_.has_value(); }

    // ── Capability minting ──────────────────────────────────────────

    /// Run `f` under exclusive access to this storage's mutable state.
    ///
    /// Non-recursive — re-entry deadlocks in release builds; debug
    /// builds assert first via a `thread_local` detector defined in the
    /// concrete backend's TU (sqlite-trace-storage-lifecycle.cc).
    template<typename F>
    auto withExclusiveAccess(const gdp::Proof<BlockingTag> & bs, F && f)
        -> std::invoke_result_t<F, const ExclusiveTraceStorageAccess &>
    {
        reentrancyCheckEnter();
        struct Guard {
            TraceStorageBase & self;
            ~Guard() { self.reentrancyCheckExit(); }
        } guard{*this};
        std::lock_guard<std::mutex> lock(storeMutex_);
        ExclusiveTraceStorageAccess ea(bs);
        return std::forward<F>(f)(ea);
    }

protected:
    explicit TraceStorageBase(SemanticSessionKey initialKey)
        : semanticSessionKey_(std::move(initialKey))
        , stableRecoveryKey_(SessionRecoveryKey{semanticSessionKey_.digest})
    {}

    // Protected, non-virtual. A derived class is responsible for its
    // own cleanup; base teardown is trivial.
    ~TraceStorageBase() = default;

    /// Advance the NodeStamp counter's floor. Called from the concrete
    /// backend's bulk-load path so `allocateNodeStamp()` never returns
    /// a stamp that collides with a pre-loaded Sessions row.
    void raiseNextNodeStamp(uint32_t floor) noexcept
    {
        auto cur = nextNodeStamp_.load(std::memory_order_relaxed);
        while (cur < floor
            && !nextNodeStamp_.compare_exchange_weak(cur, floor, std::memory_order_relaxed)) {}
    }

    /// Called by the concrete backend's `setSessionConfig`. The base
    /// updates its cached session key + stable recovery key and
    /// enforces the SetOnce invariant; the concrete backend supplies
    /// the algorithm (SqliteTraceStorage reads the process-global; an
    /// in-memory test backend can pin one).
    void storeSessionConfig(SessionConfig config, EvalTraceHashAlgorithm algorithm)
    {
        semanticSessionKey_ = config.buildSemanticSessionKey(algorithm);
        stableRecoveryKey_ = config.stableRecoveryKey;
        sessionConfig_.set(std::move(config));
    }

    const SetOnce<SessionConfig> & sessionConfigSlot() const noexcept
    { return sessionConfig_; }

    /// Protected mutex so the concrete backend's ctor / dtor /
    /// self-mint-bs lifecycle paths can still lock directly.
    mutable std::mutex storeMutex_;

private:
    /// Re-entrancy detection. Debug: assert. Release: no-op. Defined
    /// in `sqlite-trace-storage-lifecycle.cc` — the `thread_local` map
    /// keys on the base-class pointer so all concrete backends share
    /// one detector.
    void reentrancyCheckEnter();
    void reentrancyCheckExit() noexcept;

    SemanticSessionKey semanticSessionKey_;
    SessionRecoveryKey stableRecoveryKey_;
    SetOnce<SessionConfig> sessionConfig_;
    std::atomic<uint32_t> nextNodeStamp_{1};
};

/// C++20 concept describing "the notion of TraceStorage as an API".
///
/// The user directive was: "I don't want you to remove the notion of
/// TraceStorage — it is an API — but I do want zero runtime cost."
/// Inheritance-based abstraction cost a vtable and perturbed layout;
/// a concept gives every consumer template-based polymorphism with
/// zero runtime overhead.  Call sites that want to take any storage
/// backend spell `template<TraceStorageLike S>` instead of `TraceStorage
/// &`.  Concrete-type call sites (the overwhelming majority) go
/// unchanged.
///
/// The concept checks for the public data-path surface plus the
/// base-owned helpers every storage must provide.  A class that
/// inherits `TraceStorageBase` automatically satisfies the base-owned
/// portion.
///
/// **Policy**: template over `TraceStorageLike` ONLY when a callsite
/// genuinely needs to work against both backends (currently only a
/// few test paths that compare `SqliteTraceStorage` against
/// `InMemoryTraceRows`).  Production callsites hold a concrete
/// `SqliteTraceStorage &` directly.  The concept is a boundary tool,
/// not a coding convention — spraying it across the call graph would
/// re-introduce the very binary-size bloat the devirtualisation
/// removed, via one template instantiation per backend per function.
///
/// Today production has zero `template<TraceStorageLike S>` call
/// sites; that is the correct baseline.  Audit `grep -rn
/// 'template<.*TraceStorageLike'` if you suspect drift.
template<typename S>
concept TraceStorageLike =
    std::derived_from<S, TraceStorageBase>
    && requires(
        S & s,
        const gdp::Proof<BlockingTag> & bs,
        const ExclusiveTraceStorageAccess & ea,
        SessionConfig cfg,
        AttrPathId pathId,
        TraceId traceId,
        DepKeySetId keySetId,
        ResultId resultId,
        ResultHash rh,
        DepKeySetHash dkh,
        FullTraceHash fth,
        TraceHash th,
        CurrentGitIdentityHash gih,
        TraceHeader header,
        EncodedResultPayload payload,
        std::vector<uint8_t> blob,
        std::optional<EvalTraceHash> optHash,
        RuntimeRootRecord rr,
        Store & store
    ) {
    { s.setSessionConfig(std::move(cfg)) } -> std::same_as<void>;
    { s.insertResult(ea, rh, std::move(payload)) } -> std::same_as<ResultId>;
    { s.insertDepKeySet(ea, dkh, std::move(blob)) } -> std::same_as<DepKeySetId>;
    { s.insertTrace(ea, fth, std::move(header), std::move(blob)) } -> std::same_as<TraceId>;
    { s.loadTraceHeader(ea, traceId) } -> std::same_as<std::optional<TraceHeader>>;
    { s.loadTraceBlobs(ea, traceId) } -> std::same_as<std::optional<TraceBlobs>>;
    { s.loadKeysBlob(ea, keySetId) } -> std::same_as<std::optional<std::vector<uint8_t>>>;
    { s.loadResultPayload(ea, resultId) } -> std::same_as<std::optional<ResultPayload>>;
    { s.lookupCurrent(ea, pathId) } -> std::same_as<std::optional<CurrentNodeRef>>;
    { s.lookupLatestHistory(ea, pathId) } -> std::same_as<std::optional<HistoryNodeRef>>;
    { s.queryAllHistory(ea, pathId) } -> std::same_as<std::vector<HistoryEntry>>;
    { s.queryHistoryByTraceHash(ea, pathId, th) } -> std::same_as<std::vector<HistoryEntry>>;
    { s.queryHistoryByGitIdentity(ea, pathId, gih) } -> std::same_as<std::vector<HistoryEntry>>;
    { s.publishFreshRecord(ea, pathId, traceId, resultId, optHash) } -> std::same_as<CurrentNodeRef>;
    { s.publishHistoryBootstrap(ea, pathId, traceId, resultId) } -> std::same_as<CurrentNodeRef>;
    { s.recordRuntimeRoot(ea, rr, store) } -> std::same_as<void>;
    { s.loadRuntimeRoots(ea, store) } -> std::same_as<RuntimeRootLoadResult>;
    { s.flush(ea) } -> std::same_as<void>;
};

// Back-compat alias.  Historical code spells `TraceStorage` when it
// means "the base class that owns session identity + mutex".  Keep the
// alias so callers don't churn; the name no longer implies an abstract
// interface.
using TraceStorage = TraceStorageBase;

} // namespace nix::eval_trace
