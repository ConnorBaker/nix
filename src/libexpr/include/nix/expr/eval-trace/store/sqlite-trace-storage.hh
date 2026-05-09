#pragma once

#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/eval-trace/deps/analysis.hh"
#include "nix/expr/eval-trace/deps/authority.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/store/attr-vocab-store.hh"
#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/expr/eval-trace/store/session-identity.hh"
#include "nix/expr/eval-trace/store/trace-resolve.hh"
#include "nix/expr/eval-trace/store/trace-result-codec.hh"
#include "nix/expr/eval-trace/store/trace-storage.hh"
#include "nix/expr/eval-trace/store/trace-value-types.hh"
#include "nix/expr/eval-trace/store/vocab-aware-hasher.hh"
#include "nix/store/sqlite.hh"
#include "nix/store/path.hh"
#include "nix/util/canon-path.hh"
#include "nix/util/source-path.hh"
#include "nix/util/gdp/proof.hh"
#include "nix/util/hash.hh"
#include "nix/expr/eval-trace/ids.hh"
#include "nix/expr/eval-trace/strand-local.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <concepts>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace nix {
class EvalState;
class Store;
struct InterningPools;
}

namespace nix::eval_trace {
struct BlockingTag;
class CanonicalHashBuilder;
}

namespace nix::eval_trace {

struct VerificationSession;
class Verifier;
class Recorder;

struct SqliteTraceStorage;

// TraceId, ResultId, DepKeySetId, and the namespace-scope value types
// (CurrentNodeRef, ResultPayload, TraceHeader, HistoryEntry,
// VerifyResult, RecordResult, RuntimeRootRecord,
// VerifiedRuntimeRootRecord, RuntimeRootLoadResult, EvalInfoRecord,
// ResolvedDep) are declared in `store/trace-value-types.hh`
// (included above). Lightweight consumers (the codec free fns,
// `nix eval-info`, upcoming Recorder/Verifier/TraceBackend) can pull
// that header without the SQLite surface.
//
// Session identity types (SemanticSessionKey, SessionConfig, session
// keys, kSchemaEpoch, kProviderEpoch, SetOnce) live in
// `store/session-identity.hh`.
//
// TraceStorageTestAccess is used elsewhere in this namespace (for
// SqliteTraceStorage method access) but is NOT friended to the origin-typing
// machinery — tests must enter scopes through the same named factories
// as production.
namespace test { struct TraceStorageTestAccess; }

// ── Dep origin tags ──────────────────────────────────────────────────
//
// An origin classifies the CALLING CONTEXT of a dep, not the dep itself.
// Pass 1/2 iterate the current trace's fullDeps (CurrentTrace). Structural
// variant recovery iterates a historical candidate's fullDeps
// (HistoricalCandidate). Both carry `Dep::hash` values, but they have
// different trust levels:
//
//   CurrentTrace: `dep.hash` is the stored hash of the trace being
//     verified. If trace-scoped subsumption fires (Pass 1 matched this
//     trace's file content dep), the stored hash equals the current-state
//     resolution and may be written to L1 as VerifiedHash.
//
//   HistoricalCandidate: `dep.hash` is the stored hash of a historical
//     recording.  It may not equal current-state resolution (candidate was
//     recorded against different content), so it MUST NOT be written to L1
//     as VerifiedHash.
//
// Origin-SCOPED minting:
// ---------------------
// Each origin has a scope type defined below.  The scope type's constructor
// is private and friended to narrow trusted call sites (named factory fns
// that document each entry point).  A CurrentTrace scope carries the current
// TraceId so subsumption facts are scoped to the trace being verified instead
// of leaking across a whole verification session. Passing a scope by
// const-reference between helper functions is sound.
//
// To TAG a Dep, call `scope.tag(dep)` → returns `OriginDep<Origin>`.  A
// `CurrentTraceScope` cannot produce a `CandidateDep` (different types).
// A phase function that takes only `const CandidateScope &` cannot forge
// CurrentTraceDep even by accident — its argument has no such capability.

template<typename Origin> class OriginScope;

namespace dep_origin {
    struct CurrentTrace {
    private:
        friend class OriginScope<CurrentTrace>;
    };
    struct HistoricalCandidate {
    private:
        friend class OriginScope<HistoricalCandidate>;
    };
}

/// Dep tagged with the origin of its recorded hash.
///
/// Sealed constructor: only `OriginScope<Origin>::tag` can mint.
template<typename Origin>
class OriginDep {
    Dep value_;
    std::optional<TraceId> traceId_;
    explicit OriginDep(Dep d, std::optional<TraceId> traceId = std::nullopt) noexcept
        : value_(std::move(d)), traceId_(traceId) {}

    // ONLY friend: the matching OriginScope specialization.  Tests that need
    // an origin-tagged Dep must go through OriginScopeFactory::enterCurrentTrace
    // or ::enterCandidate, same as production code.  There is no test-only
    // escape hatch.
    friend class OriginScope<Origin>;

public:
    using OriginTag = Origin;

    OriginDep(const OriginDep &) = delete;
    OriginDep(OriginDep &&) noexcept = default;
    OriginDep & operator=(const OriginDep &) = delete;
    OriginDep & operator=(OriginDep &&) = delete;

    const Dep & value() const noexcept { return value_; }

    TraceId traceId() const noexcept
        requires std::same_as<Origin, dep_origin::CurrentTrace>
    {
        assert(traceId_);
        return *traceId_;
    }
};

using CurrentTraceDep = OriginDep<dep_origin::CurrentTrace>;
using CandidateDep    = OriginDep<dep_origin::HistoricalCandidate>;

/// Origin-scoped capability for tagging Deps.  Primary template left
/// incomplete; ONLY the two authorized specializations exist.
template<typename Origin> class OriginScope;

// The scope entry points are members of a "gatekeeper" class forward-
// declared here but DEFINED in exactly one TU (verifier.cc).
// Other TUs see only an incomplete type, so they cannot invoke its static
// members.  Defining a second `struct OriginScopeFactory` in this namespace
// elsewhere would be an ODR violation — this is a one-definition-rule
// barrier, not a file-static (anonymous-namespace) barrier.  An anonymous-
// namespace type cannot be the target of a namespace-scope friend
// declaration, so the ODR approach is the only workable design.
struct OriginScopeFactory;

/// CurrentTrace scope.  Constructible ONLY by `OriginScopeFactory`, which
/// is defined in verifier.cc.
template<>
class OriginScope<dep_origin::CurrentTrace> {
    TraceId traceId_;

    explicit OriginScope(TraceId traceId) : traceId_(traceId) {}

    friend struct OriginScopeFactory;

public:
    OriginScope(const OriginScope &) = delete;
    OriginScope(OriginScope &&) = default;
    OriginScope & operator=(const OriginScope &) = delete;
    OriginScope & operator=(OriginScope &&) = delete;

    OriginDep<dep_origin::CurrentTrace> tag(Dep d) const noexcept
    {
        return OriginDep<dep_origin::CurrentTrace>{std::move(d), traceId_};
    }
};

/// HistoricalCandidate scope.  Same sealing story — only
/// OriginScopeFactory (defined in verifier.cc) can construct.
template<>
class OriginScope<dep_origin::HistoricalCandidate> {
    OriginScope() = default;

    friend struct OriginScopeFactory;

public:
    OriginScope(const OriginScope &) = delete;
    OriginScope(OriginScope &&) = default;
    OriginScope & operator=(const OriginScope &) = delete;
    OriginScope & operator=(OriginScope &&) = delete;

    OriginDep<dep_origin::HistoricalCandidate> tag(Dep d) const noexcept
    {
        return OriginDep<dep_origin::HistoricalCandidate>{std::move(d)};
    }
};

using CurrentTraceScope = OriginScope<dep_origin::CurrentTrace>;
using CandidateScope    = OriginScope<dep_origin::HistoricalCandidate>;

/// Sealed capability: authorizes writing `VerifiedHash{dep.hash}` to L1.
///
/// The write is sound only when `dep.hash` equals the current-state
/// resolution of `dep.key`.  This precondition holds by construction for
/// CurrentTrace-origin deps whose file was verified unchanged for the same
/// TraceId in Pass 1 (isFileVerified(traceId, key) is true).  It does NOT
/// hold for HistoricalCandidate deps — their stored hash may reflect
/// different recording content.
///
/// Three composed gates enforce the barrier:
///
///   (a) grantVerifiedSubsumption<O> has a `requires
///       std::same_as<O, CurrentTrace>` constraint — passing a
///       CandidateDep fails SFINAE cleanly (probed by `requires{}` in
///       tests).  The constraint sits on a free function at namespace
///       scope so the failure is visible as a clean SFINAE, not an
///       in-class access violation.
///
///   (b) OriginDep<O>'s constructor is private per-specialization.  A
///       CurrentTraceDep can only come from OriginScope<CurrentTrace>::tag,
///       and OriginScope<CurrentTrace> is constructible only by the sealed
///       factory OriginScopeFactory::enterCurrentTrace.
///
///   (c) VerifiedSubsumption's default constructor is private; the only
///       friend is grantVerifiedSubsumption.  Even if gate (a) were
///       somehow bypassed, constructing the capability directly from any
///       other scope is a compile error.
///
/// The composition: a VerifiedSubsumption can only exist inside a call
/// from the CurrentTrace subsumption branch of resolveDepHash — and that
/// branch is only reached when `session.isFileVerified(traceId, dep.key)` AND
/// the caller passed a CurrentTraceDep (which by (b) can only have come
/// from OriginScopeFactory::enterCurrentTrace(traceId)). Historical-hash
/// poisoning of L1 is unrepresentable.
///
/// This is the sole authorization for VerificationSession::cacheVerifiedHash.
class VerifiedSubsumption {
    VerifiedSubsumption() = default;

    // Only grant() may construct.
    template<typename O>
        requires std::same_as<O, dep_origin::CurrentTrace>
    friend VerifiedSubsumption grantVerifiedSubsumption(const OriginDep<O> &) noexcept;
};

/// Sealed minter for VerifiedSubsumption.  The `requires` clause is the
/// soundness barrier; it is directly probeable from any scope via a
/// requires-expression, because SFINAE failure at this free-function level
/// is a clean constraint failure (no nested template dispatch).
template<typename O>
    requires std::same_as<O, dep_origin::CurrentTrace>
inline VerifiedSubsumption grantVerifiedSubsumption(const OriginDep<O> &) noexcept
{
    return VerifiedSubsumption{};
}

// AuthorityGate / AuthorityState moved to deps/authority.hh.
// kSchemaEpoch, kProviderEpoch, SessionSourceDigest, SessionRecoveryKey,
// SessionExternalRoot, SessionConfig, SetOnce moved to
// store/session-identity.hh (included above).

// ID declarations moved above the namespace-scope value types (they
// need ResultId / DepKeySetId). Kept as forward references here for
// any reader skimming top-to-bottom — see line ~54 above.

struct SqliteTraceStorage;

// `ExclusiveTraceStorageAccess` is defined in `trace-storage.hh` (base
// class). It's the same token regardless of which concrete backend
// minted it.

/** SQLite-backed trace store for the eval trace system.
 *
 * Concurrency: primary public data-path methods require
 * `const ExclusiveTraceStorageAccess &` and trust the caller already holds
 * `storeMutex_` (via `withExclusiveAccess`, the sole minter of the
 * capability). Some private/friend helper methods still take only
 * `Proof<BlockingTag>` but are always reached from inside a
 * `withExclusiveAccess` scope (the orchestrator's `coroBlock` paths
 * wrap in `withExclusiveAccess`; verify/recovery phase functions use
 * `ctx.bs` which was minted inside the orchestrator's `withExclusiveAccess`).
 *
 * Re-entrancy: `storeMutex_` is non-recursive. Every internal caller
 * of the private helpers (`ensureTraceHeader`, `lookupCurrentNode`,
 * `loadResultPayload`) is reached only from inside a
 * `withExclusiveAccess` scope that already holds the mutex — the
 * helpers no longer acquire their own `lock_guard`. Re-entry would
 * deadlock, which is strictly better than the old `recursive_mutex`'s
 * silent recursion that masked bugs. The `thread_local` re-entrancy
 * detector in `withExclusiveAccessReentrancyCheckEnter` still asserts
 * in debug builds against accidental re-entry via the public
 * `withExclusiveAccess` entry point.
 *
 * The inner `BlockingSync<State>` that used to re-lock on every SQLite
 * access was removed per P30.  `storeMutex_` alone now serialises all
 * access to `_state`.
 */
struct SqliteTraceStorage
    : public TraceStorage
    , private gdp::Certifier<BlockingTag>
    , private VocabAwareHasher {
    /// Friend access for file-static VerifyImpl and recovery functions
    /// in verifier.cc. These need resolveCurrentDepHash,
    /// resolveTraceContextHash, publishRecovery, patchTraceHashInMemory, etc.
    friend struct VerifyImpl;

    /// Friend access for the async verification pipeline (Verifier).
    /// Needs decodeCachedResult, loadResultPayload for result serving.
    friend class Verifier;

    /// Friend access for the record pipeline (Recorder). Needs
    /// getOrCreateDepKeySet, doInternResult, getOrCreateTrace,
    /// publishRecord, flush — all the private helpers that used to be
    /// reached from the inlined `SqliteTraceStorage::record` body.
    friend class Recorder;
    struct State {
        SQLite db;

        // Strings — read-all for bulk load, explicit-ID insert for flush
        SQLiteStmt getAllStrings;
        SQLiteStmt insertStringWithId;

        // DataPaths — read-all for bulk load, explicit-ID insert for flush
        SQLiteStmt getAllDataPaths;
        SQLiteStmt insertDataPathWithId;

        // Results — read-all for bulk load, explicit-ID insert for flush
        SQLiteStmt getAllResults;
        SQLiteStmt insertResultWithId;
        SQLiteStmt getResult;

        // DepKeySets (content-addressed exact dep key storage)
        SQLiteStmt getAllDepKeySets;
        SQLiteStmt insertDepKeySetWithId;
        SQLiteStmt getDepKeySet;

        // Traces (references DepKeySets via dep_key_set_id FK, stores values_blob)
        SQLiteStmt getAllTraces;
        SQLiteStmt insertTraceWithId;
        SQLiteStmt lookupTraceByFullHash;
        SQLiteStmt getTraceInfo;

        // Sessions (current verified state per attribute — renamed from CurrentTraces)
        SQLiteStmt lookupAttr;
        SQLiteStmt upsertAttr;

        // History (all historical traces for constructive recovery — renamed from TraceHistory)
        SQLiteStmt insertHistory;
        SQLiteStmt lookupLatestHistoryForAttr;
        SQLiteStmt lookupHistoryByTraceHash;
        SQLiteStmt scanHistoryForAttr;
        SQLiteStmt lookupHistoryByGitIdentity;

        // SessionRuntimeRoots (session metadata for runtime-fetched inputs)
        SQLiteStmt insertRuntimeRoot;
        SQLiteStmt loadRuntimeRoots;

        // DirSets (normalized dir-set definitions for aggregated DirSet deps)
        SQLiteStmt insertDirSet;
        SQLiteStmt getAllDirSets;

        // Vocab (on vocab.* schema, ATTACH'd from attr-vocab.sqlite)
        SQLiteStmt insertVocabName;
        SQLiteStmt insertVocabPath;

    };

    // `storeMutex_`, `semanticSessionKey_`, `stableRecoveryKey_`,
    // `nextNodeStamp_`, `sessionConfig_` are owned by the `TraceStorage`
    // base class. See trace-storage.hh.

    /// SQLite connection + prepared-statement storage. Protected by
    /// the base class's `storeMutex_` (acquired by
    /// `withExclusiveAccess`); every caller of a `State`-touching
    /// member holds the mutex either directly via `ea` or transitively
    /// through a `coroBlock` path that wraps in `withExclusiveAccess`.
    std::unique_ptr<State> _state;
    SymbolTable & symbols;
    InterningPools & pools;
    AttrVocabStore & vocab;

    // ── Back-compat type aliases ─────────────────────────────────────
    //
    // The actual types live at namespace scope in
    // `store/trace-value-types.hh`. These aliases let existing callers
    // that spell `SqliteTraceStorage::X` keep compiling. Single grep target for
    // cleanup when the SqliteTraceStorage split lands.
    using CurrentNodeRef = eval_trace::CurrentNodeRef;
    using ResultPayload = eval_trace::ResultPayload;
    using TraceHeader = eval_trace::TraceHeader;
    using HistoryEntry = eval_trace::HistoryEntry;
    using VerifyResult = eval_trace::VerifyResult;
    using RecordResult = eval_trace::RecordResult;
    using RuntimeRootRecord = eval_trace::RuntimeRootRecord;
    using VerifiedRuntimeRootRecord = eval_trace::VerifiedRuntimeRootRecord;
    using RuntimeRootLoadResult = eval_trace::RuntimeRootLoadResult;
    using EvalInfoRecord = eval_trace::EvalInfoRecord;
    using ResolvedDep = eval_trace::ResolvedDep;

    // ── Session caches (boost flat containers for cache locality) ─────

    /// Content-addressed dedup maps (in-memory, flushed to DB periodically)
    boost::unordered_flat_map<ResultHash, ResultId, ResultHash::Hash> resultByHash;
    boost::unordered_flat_map<DepKeySetHash, DepKeySetId, DepKeySetHash::Hash> depKeySetByHash;
    boost::unordered_flat_map<FullTraceHash, TraceId, FullTraceHash::Hash> traceByFullHash;

    /// Fused per-trace cache, keyed by TraceId.  Carries both the
    /// lightweight header and an optional full dep-vector.
    ///
    /// - `header` is populated by `ensureTraceHeader` and by every
    ///   path that observes the trace's scalar metadata.
    /// - `full` is set non-null only after `loadFullTrace` deserialises
    ///   the values blob.  Holds a shared_ptr so verify/recovery can
    ///   retain immutable dep vectors across recursive trace loads
    ///   without copying or depending on unordered_flat_map reference
    ///   stability.
    ///
    /// Fusion consolidates two previously-separate TraceId-keyed
    /// maps (`traceHeaderCache` + `traceFullCache`).  Every code
    /// path that populated both together (the `loadFullTrace` cold
    /// path, `publishRecord`, `loadTraceKeysAndHeader`) now does one
    /// `insert_or_assign` instead of two; the hot `ensureTraceHeader`
    /// path returns `&entry.header` after a single probe.
    struct TraceCacheEntry {
        TraceHeader header{};
        std::shared_ptr<const std::vector<Dep>> full;
    };
    boost::unordered_flat_map<TraceId, TraceCacheEntry, TraceId::Hash> traceCache;

    /// Current-node index: AttrPathId → lightweight CurrentNodeRef snapshot.
    /// Avoids repeated Sessions DB lookups for the same attr path.
    /// Replaced wholesale when current state changes for a path.
    boost::unordered_flat_map<AttrPathId, CurrentNodeRef, AttrPathId::Hash> currentNodeIndex;

    /// On-demand result payload cache: ResultId → decoded result fields.
    /// Checked by loadResultPayload before DB query.
    boost::unordered_flat_map<ResultId, ResultPayload, ResultId::Hash> resultPayloadCache;

    /// DepKeySet session cache: maps DepKeySetId → resolved dep keys.
    /// Keyed by integer ID (not hash) because we look up by ID after DB queries.
    /// Avoids re-decompressing keys_blob when multiple traces share a key set.
    boost::unordered_flat_map<
        DepKeySetId,
        std::shared_ptr<const std::vector<Dep::Key>>,
        DepKeySetId::Hash> depKeySetCache;

    /// Pre-fetched but undeserialized values blob, stashed during keys-only loading.
    struct DeferredTraceBlob {
        DepKeySetId depKeySetId;
        std::vector<uint8_t> valuesBlob;
    };
    boost::unordered_flat_map<TraceId, DeferredTraceBlob, TraceId::Hash> deferredTraceBlobs;

    // ── In-memory ID counters (next ID to assign = max(DB IDs) + 1) ──
    //
    // NodeStamp counter is base-owned (see TraceStorage::allocateNodeStamp).
    //
    // Explicit `{}` is load-bearing: the constructor's bulk-load path
    // reads these (`if (id > nextResultId) nextResultId = id;`) before
    // any write.  An empty DB runs the loop zero times, so
    // `nextResultId` stays at its default value.  `Tagged<>` no longer
    // zero-inits scalar payloads (see `tagged.hh`); the
    // `cppcoreguidelines-pro-type-member-init` clang-tidy check flags
    // uninitialized scalar fields in classes with user-provided ctors.

    ResultId nextResultId{};
    DepKeySetId nextDepKeySetId{};
    TraceId nextTraceId{};

    /// High-water mark: highest string ID flushed to DB.
    /// Strings with ID > flushedStringHighWaterMark are pending write.
    uint32_t flushedStringHighWaterMark = 0;
    uint32_t flushedDataPathHighWaterMark = 0;

    // ── Pending writes (deferred to flush()) ─────────────────────────

    // Pending-write entry structs.  Scalar Tagged fields get `{}` so
    // cppcoreguidelines-pro-type-member-init doesn't flag them — these
    // are value types that are always brace-initialized at their
    // push-back sites, but the check looks at the type's default ctor,
    // not the call sites.

    struct PendingResult {
        ResultId id{};
        EncodedResultPayload payload;
        ResultHash hash{};
    };
    std::vector<PendingResult> pendingResults;

    struct PendingDepKeySet {
        DepKeySetId id{};
        DepKeySetHash keySetHash{};
        std::vector<uint8_t> keysBlob;
    };
    std::vector<PendingDepKeySet> pendingDepKeySets;

    struct PendingTrace {
        TraceId id{};
        TraceHash traceHash{};
        FullTraceHash fullHash{};
        DepKeySetId depKeySetId{};
        std::vector<uint8_t> valuesBlob;
    };
    std::vector<PendingTrace> pendingTraces;

    // ── Capability minting ────────────────────────────────────────────

    // `withExclusiveAccess`, `currentSemanticSessionKey`,
    // `currentStableRecoveryKey`, `allocateNodeStamp` are inherited
    // from the `TraceStorage` base.

    // ── Lifecycle ────────────────────────────────────────────────────

    /// Flush without requiring an external blocking proof. Uses Certifier
    /// authority — safe because the only caller is the destructor, which
    /// runs single-threaded after all eval work completes.  External code
    /// goes through `TraceBackend::flush()` which self-mints `bs` and
    /// acquires `ea` via `withExclusiveAccess` before calling `flush(ea)`.
    void flushExclusive();

    SqliteTraceStorage(
        SymbolTable & symbols,
        InterningPools & pools,
        AttrVocabStore & vocab,
        SemanticSessionKey initialSessionKey);
    ~SqliteTraceStorage();

    /// Set session configuration. Throws if already set (SetOnce invariant).
    /// Called once at session creation when a fingerprint is available.
    void setSessionConfig(SessionConfig config);

    /** Flush all pending writes to SQLite in dependency order
     *  (Strings → Results → DepKeySets → Traces).
     *  Called from record() before upsertAttr/insertHistory, and
     *  from the destructor before committing the transaction. */
    void flush(const ExclusiveTraceStorageAccess &);

    /// Persist a runtime root during cold evaluation.
    /// Called from fetchTree when a locked input is fetched.
    void recordRuntimeRoot(
        const ExclusiveTraceStorageAccess &,
        const RuntimeRootRecord & record,
        Store & store);

    /// Load all runtime roots for the current session.
    /// Returns entries from SessionRuntimeRoots table.
    RuntimeRootLoadResult loadRuntimeRoots(
        const ExclusiveTraceStorageAccess &,
        Store & store);

    // ── TraceStorage data-path surface (§2.1) ────────────────────────
    //
    // Thin adapters over the existing content-addressed helpers
    // (`doInternResult`, `getOrCreateDepKeySet`, `getOrCreateTrace`,
    // `ensureTraceHeader`, `lookupCurrentNode`, etc.). The old-name
    // helpers still exist as private members because internal callers
    // (recorder.cc, verifier.cc phase functions) reach them directly
    // across friendship. These methods are the abstract API that
    // `TraceBackend` and `TraceStorageLike`-templated code dispatches
    // through — direct calls, no virtual dispatch.

    ResultId insertResult(
        const ExclusiveTraceStorageAccess &,
        ResultHash, EncodedResultPayload &&);

    DepKeySetId insertDepKeySet(
        const ExclusiveTraceStorageAccess &,
        DepKeySetHash, std::vector<uint8_t> && keysBlob);

    TraceId insertTrace(
        const ExclusiveTraceStorageAccess &,
        FullTraceHash, TraceHeader,
        std::vector<uint8_t> && valuesBlob);

    std::optional<TraceHeader> loadTraceHeader(
        const ExclusiveTraceStorageAccess &, TraceId);

    std::optional<TraceBlobs> loadTraceBlobs(
        const ExclusiveTraceStorageAccess &, TraceId);

    std::optional<std::vector<uint8_t>> loadKeysBlob(
        const ExclusiveTraceStorageAccess &, DepKeySetId);

    std::optional<ResultPayload> loadResultPayload(
        const ExclusiveTraceStorageAccess &, ResultId);

    std::optional<CurrentNodeRef> lookupCurrent(
        const ExclusiveTraceStorageAccess &, AttrPathId);

    std::optional<HistoryNodeRef> lookupLatestHistory(
        const ExclusiveTraceStorageAccess &, AttrPathId);

    std::vector<HistoryEntry> queryAllHistory(
        const ExclusiveTraceStorageAccess &, AttrPathId);

    std::vector<HistoryEntry> queryHistoryByTraceHash(
        const ExclusiveTraceStorageAccess &, AttrPathId, TraceHash);

    std::vector<HistoryEntry> queryHistoryByGitIdentity(
        const ExclusiveTraceStorageAccess &, AttrPathId,
        CurrentGitIdentityHash);

    CurrentNodeRef publishFreshRecord(
        const ExclusiveTraceStorageAccess &,
        AttrPathId, TraceId, ResultId,
        std::optional<EvalTraceHash> gitIdentityHash);

    CurrentNodeRef publishHistoryBootstrap(
        const ExclusiveTraceStorageAccess &,
        AttrPathId, TraceId, ResultId);

    /// Query the cached evaluation record for an attr path. Read-only: never
    /// writes to the trace DB, never triggers eval/verify/recovery.
    ///
    /// When `allowHistoryFallback` is true and the current session key has no
    /// row, returns the most recent History row under the stable recovery key
    /// with `source == History`. Otherwise returns `std::nullopt` on miss.
    ///
    /// Mints its own blocking proof internally — safe because the whole path
    /// is synchronous and does not participate in the verification io_context.
    std::optional<EvalInfoRecord> queryEvalInfoExclusive(
        AttrPathId pathId, bool allowHistoryFallback);

    // (publishRecord / publishRecovery / publishStateChange are private)

    /**
     * Record a fresh evaluation result with its dependencies (BSàlC constructive
     * trace recording).
     *
     * Stores (attrPath, result, deps) as a constructive trace: the full result
     * value is persisted alongside dep hashes, enabling future recovery without
     * re-evaluation. Each trace stores only its own deps — no parent dep
     * inheritance. If a trace with the same trace_hash already exists (dedup),
     * reuses it. Dep keys are content-addressed via DepKeySets table; hash
     * values are stored per-trace in values_blob.
     *
     * Also inserts into History for constructive recovery: historical traces
     * can be recovered when dep hashes match a previously-seen state (e.g., after
     * a file revert).
     */
    RecordResult record(
        const ExclusiveTraceStorageAccess &,
        AttrPathId pathId,
        const CachedResult & value,
        const std::vector<Dep> & allDeps);


    /**
     * Load the full dependency set for a trace.
     *
     * Reads the keys_blob from DepKeySets and values_blob from Traces via JOIN.
     * Zips the key set with positional hash values to reconstruct full deps.
     * Single DB round-trip + two zstd decompressions. O(D) in dep count.
     *
     * The returned vector is immutable and NOT sorted. Callers that need
     * canonical ordering must call sortAndDedupInterned() on their own copy.
     */
    std::shared_ptr<const std::vector<Dep>> loadFullTrace(
        const ExclusiveTraceStorageAccess &, TraceId traceId);

    /**
     * Load just the dep key set for a DepKeySets row (no hash values).
     *
     * Returns Dep::Key entries (type + source/key IDs). Session-cached:
     * subsequent calls for the same depKeySetId return from depKeySetCache.
     * Used by structural variant recovery to avoid decompressing
     * values_blob when only dep keys are needed for hash recomputation.
     * Returns null only if the DepKeySets row is missing.
     */
    std::shared_ptr<const std::vector<Dep::Key>> loadKeySet(
        const ExclusiveTraceStorageAccess &, DepKeySetId depKeySetId);

    /// Scan history entries for this attr path in the current stable recovery namespace.
    std::vector<HistoryEntry> scanHistory(const ExclusiveTraceStorageAccess &, AttrPathId pathId);

    /// Keys + header loaded from a trace, without deserializing values.
    struct TraceKeysAndHeader {
        TraceHeader header;
        std::shared_ptr<const std::vector<Dep::Key>> keys;
    };

    /**
     * Load trace keys and header without deserializing values_blob.
     *
     * Two-phase trace loading: keys are cheap (used for hash recomputation),
     * values decompression is deferred until loadFullTrace() needs them.
     * The raw values_blob is stashed in deferredTraceBlobs for later use.
     */
    std::optional<TraceKeysAndHeader> loadTraceKeysAndHeader(const ExclusiveTraceStorageAccess &, TraceId traceId);

    bool attrExists(const ExclusiveTraceStorageAccess &, AttrPathId pathId);

    /** Get the current canonical trace hash for an attr path.
     *  Used by trace-context dep verification and recovery. Unlike a result hash
     *  (which for attrsets only captures attribute names), this hash captures
     *  trace-hash-contributing deps while excluding implicit structural guards. */
    std::optional<TraceHash> getCurrentTraceHash(const ExclusiveTraceStorageAccess &, AttrPathId pathId);

    // `allocateNodeStamp` / `setSessionConfig` inherited from TraceStorage.
    // (SqliteTraceStorage::setSessionConfig override declared above.)

    // Back-compat shims forwarding to the free fns in deps/analysis.hh.
    // Defined here as inline one-liners so the forward is folded at
    // the call site. New code should call the free fns directly.
    [[nodiscard]] std::optional<RepoRootId> extractGoverningRepoId(const std::vector<Dep> & deps) const
    { return eval_trace::extractGoverningRepoId(deps); }
    [[nodiscard]] std::optional<RepoRootId> extractGoverningRepoId(const std::vector<Dep::Key> & keys) const
    { return eval_trace::extractGoverningRepoId(keys); }
    [[nodiscard]] bool allDepsGitRecoverable(const std::vector<Dep> & deps, RepoRootId targetRepoId) const
    { return eval_trace::allDepsGitRecoverable(deps, targetRepoId); }
    [[nodiscard]] bool allDepsGitRecoverable(const std::vector<Dep::Key> & keys, RepoRootId targetRepoId) const
    { return eval_trace::allDepsGitRecoverable(keys, targetRepoId); }
    [[nodiscard]] std::optional<StoredGitIdentityHash> extractGitIdentityHash(const std::vector<Dep> & deps) const
    { return eval_trace::extractGitIdentityHash(deps); }

    // ── BLOB serialization ───────────────────────────────────────────

    /// Serialize dep key sets to a packed kind-tagged binary format, zstd
    /// compressed. Stored in DepKeySets and shared across traces with the same
    /// exact dep-key set.
    static std::vector<uint8_t> serializeKeys(const std::vector<Dep::Key> & keys);
    std::vector<Dep::Key> deserializeKeys(const void * blob, size_t size);

    /// Serialize dep hash values to a self-describing schema-grouped format:
    ///   Header: [numEntries:u32] [entryTypes: numEntries bytes]
    ///   Digest block: [numDigests:u32] [data: numDigests*32 bytes]
    ///   String block: [numStrings:u32] [foreach: [len:u8][data:len bytes]]
    /// Entire blob zstd-compressed. Self-describing: no keys parameter needed
    /// for deserialization (entry types embedded in header).
    static std::vector<uint8_t> serializeValues(const std::vector<Dep> & deps);
    static std::vector<DepHashValue> deserializeValues(
        const void * blob, size_t size);

    // Back-compat shims forwarding to the free fns. Prefer the free
    // fns directly (store/trace-resolve.hh, store/trace-result-codec.hh).
    [[nodiscard]] ResolvedDep resolveDep(const Dep & dep)
    { return eval_trace::resolveDep(pools, vocab, dep); }

    [[nodiscard]] EncodedResultPayload encodeCachedResult(const CachedResult & value)
    { return eval_trace::encodeCachedResult(value, vocab); }

    [[nodiscard]] CachedResult decodeCachedResult(const ResultPayload & payload)
    { return eval_trace::decodeCachedResult(payload, vocab, symbols); }

    bool verifyTrace(
        const ExclusiveTraceStorageAccess &,
        TraceId traceId,
        const SemanticRegistry & registry,
        EvalState & state,
        VerificationSession & session);

    std::optional<VerifyResult> recovery(
        const ExclusiveTraceStorageAccess &,
        TraceId oldTraceId,
        AttrPathId pathId,
        const SemanticRegistry & registry,
        EvalState & state,
        VerificationSession & session);

    std::optional<VerifyResult> verify(
        const ExclusiveTraceStorageAccess &,
        AttrPathId pathId,
        const SemanticRegistry & registry,
        EvalState & state,
        VerificationSession & session);

    // feedKey inherited from the VocabAwareHasher mixin (protected).

private:
    // ── Verification pipeline (private — only orchestrator calls these) ──

    void patchTraceHashInMemory(const gdp::Proof<BlockingTag> & bs, TraceId traceId, TraceHash newTraceHash);

    void bulkLoadAllLocked(State & st);

    std::optional<CurrentNodeRef> lookupCurrentNode(const gdp::Proof<BlockingTag> &, AttrPathId pathId);

    std::optional<CurrentNodeRef> lookupLatestHistoryForAttr(
        const ExclusiveTraceStorageAccess &, AttrPathId pathId);

    /// Load result payload from DB by ResultId (cached in resultPayloadCache).
    /// Throws on missing result (DB corruption).
    /// Internal helper — uses `bs` directly and returns a reference into
    /// the cache. The public virtual `loadResultPayload(ea, id)` wraps
    /// this and copies into an `std::optional<ResultPayload>`.
    const ResultPayload & loadResultPayloadCached(const gdp::Proof<BlockingTag> &, ResultId resultId);

    /** Ensure traceCache has traceHash + keySetHash for the given traceId.
     *  Queries getTraceInfo on cache miss. Returns null if trace not found. */
    TraceHeader * ensureTraceHeader(const gdp::Proof<BlockingTag> & bs, TraceId traceId);

    ResultId doInternResult(const EncodedResultPayload & payload, const ResultHash & resultHash);

    TraceId getOrCreateTrace(
        const TraceHash & traceHash,
        const FullTraceHash & fullHash,
        DepKeySetId depKeySetId,
        const std::vector<uint8_t> & valuesBlob);

    DepKeySetId getOrCreateDepKeySet(
        const DepKeySetHash & keySetHash,
        const std::vector<uint8_t> & keysBlob);

    /// Atomic record publication: DB writes + all session cache updates.
    /// Called only from record(). All parameters mandatory.
    CurrentNodeRef publishRecord(
        const gdp::Proof<BlockingTag> &,
        AttrPathId pathId, TraceId traceId, ResultId resultId,
        TraceHeader header, std::vector<Dep> fullDeps,
        DepKeySetId depKeySetId, std::vector<Dep::Key> keys);

    /// Low-level DB operation. Allocates NodeStamp, writes Sessions,
    /// optionally writes History.  Also used directly as the "recovery
    /// publication" path: call with `insertHistory=false` to update the
    /// current-node pointer without inserting a new History row
    /// (publishRecord inserts; constructive recovery reuses an existing
    /// History row).
    CurrentNodeRef publishStateChange(
        const gdp::Proof<BlockingTag> &,
        AttrPathId pathId, TraceId traceId, ResultId resultId,
        bool insertHistory,
        std::optional<EvalTraceHash> gitIdentityHash = std::nullopt);

    /// Load payload from DB by ResultId, then decode.
    CachedResult decodeCachedResult(const gdp::Proof<BlockingTag> &, ResultId resultId);

    /// Sort+dedup deps in-place and compute trace hash.
    TraceHash computeSortedTraceHash(std::vector<Dep> & deps) const;

    /// Compute trace hash for already-sorted, deduplicated deps (skips sort+dedup).
    TraceHash computePresortedTraceHash(const std::vector<Dep> & deps) const;

    // ── Typestate pipeline: DepResolution ────────────────────────────
    //
    // Two-state machine: Unchecked → checkCache → CacheMissed → computeAndCache.
    // Subsumption is enforced inside resolveDepHash (dep-resolution-service.cc),
    // not inside the typestate. This ensures all callers — typestate, batch
    // pre-population, any future path — get subsumption automatically.
    //
    //   DepResolution<Unchecked>
    //       |
    //       v checkCache() &&          [requires Unchecked]
    //       +-- resolved (cache hit)
    //       '-- DepResolution<CacheMissed>
    //               |
    //               v computeAndCache() &&   [requires CacheMissed]
    //               '-- resolved (resolveDepHash checks subsumption inside)

    /// Cache-or-compute a dep's current hash value.
    ///
    /// Internally drives the DepResolution typestate pipeline:
    ///   L1 (session cache check) → L3 (resolveDepHash with subsumption).
    /// Subsumption is enforced inside resolveDepHash, not the typestate.
    ///
    /// TaggedDepType: CurrentTraceDep or CandidateDep.  The two share the same
    /// pipeline; subsumption-write and -read behavior is origin-dispatched
    /// inside resolveDepHash.
    template<typename TaggedDepType>
    [[nodiscard]] std::optional<DepHashValue> resolveCurrentDepHash(
        const ExclusiveTraceStorageAccess &,
        const TaggedDepType & dep,
        const SemanticRegistry & registry,
        EvalState & state, VerificationSession & session);

    /// Resolve a trace-context dep and return its current trace hash as EvalTraceHash.
    [[nodiscard]] std::optional<EvalTraceHash> resolveTraceContextHash(
        const ExclusiveTraceStorageAccess &,
        const Dep::Key & key,
        const SemanticRegistry & registry,
        EvalState & state,
        VerificationSession & session);
};

} // namespace nix::eval_trace
