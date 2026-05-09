/// verifier.cc — Async verification pipeline + SqliteTraceStorage
/// verification/recovery/dep-resolution implementations.
///
/// Rearchitecture-proposal.md §14 step 7. Merged from
/// `sqlite-trace-storage-verify.cc` (1910 lines of phase
/// functions + `SqliteTraceStorage::verifyTrace`/`recovery`/`verify`
/// method bodies) and the previous standalone `verifier.cc` (the
/// `Verifier` async orchestrator). The file-static helpers
/// (`OriginScopeFactory`, `FileStrandGate`, `StorePathBatch`,
/// `VerificationState`, `Pass2Result`, `VerifyImpl`,
/// `RecoveryState<Stage>`) and the `SqliteTraceStorage::verifyTrace`
/// method bodies still live here because they friend
/// `SqliteTraceStorage` for private-helper access; collapsing them
/// onto `Verifier` member helpers requires the §2.1 full virtual
/// surface on `TraceStorage`, which lands in a follow-up when there
/// is a motivated consumer for the second backend's verify path.
///
/// Contents:
///   - resolveCurrentDepHash / resolveTraceContextHash
///   - verifyTrace (two-pass verification with structural override)
///   - recovery (typestate-driven: RecoveryState<Stage>)
///   - Verifier::verifyAttr (async orchestrator with prefetch pool)

#include "verifier.hh"

#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/store/semantic-registry.hh"
#include "nix/expr/eval-trace/store/trace-resolve.hh"
#include "nix/expr/eval-trace/store/trace-result-codec.hh"
#include "nix/expr/eval-trace/store/verification-protocol.hh"
#include "nix/expr/eval-trace/store/verification-session.hh"
#include "../fiber/blocking-scope.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/deps/analysis.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval.hh"
#include "nix/store/store-api.hh"

#include "trace-serialize.hh"
#include "dep-resolution-service.hh"
#include "parse-caches.hh"
#include <nlohmann/json.hpp>

#include "nix/util/gdp/proof.hh"
#include "nix/util/finally.hh"
#include "nix/util/linear.hh"

#include <cstring>
#include <filesystem>
#include <algorithm>
#include <optional>
#include <string>
#include <type_traits>

namespace nix::eval_trace {

static bool containsVolatileDep(const std::vector<Dep::Key> & keys);

static ParseCaches & parseCachesFor(VerificationSession & session)
{
    if (!session.parseCaches)
        session.parseCaches = std::make_shared<ParseCaches>();
    return *session.parseCaches;
}

// ── Origin-scope entry points ────────────────────────────────────────
//
// `OriginScopeFactory` is forward-declared in the header (friended to the
// OriginScope specializations) but DEFINED only in this TU.  Other TUs
// see only the incomplete type and cannot invoke its static members.
// Defining a second `OriginScopeFactory` in `nix::eval_trace` elsewhere
// would be an ODR violation — this is a one-definition-rule barrier,
// not a file-static (anonymous-namespace) barrier.
//
// Production call sites use `OriginScopeFactory::enterCurrentTrace(traceId, ...)`
// or `::enterCandidate(...)` — each a sealed scope-entry function.  A
// caller holding a CurrentTraceScope cannot accidentally spawn a
// CandidateScope (different entry function + scope types are distinct).
struct OriginScopeFactory {
    template<typename F>
    static decltype(auto) enterCurrentTrace(TraceId traceId, F && f)
    {
        CurrentTraceScope scope(traceId);
        return std::forward<F>(f)(std::as_const(scope));
    }

    template<typename F>
    static decltype(auto) enterCandidate(F && f)
    {
        CandidateScope scope;
        return std::forward<F>(f)(std::as_const(scope));
    }
};

// ── Batch StorePathExistence resolution ──────────────────────────────

/// Accumulates StorePathExistence deps and resolves them in a single
/// queryValidPaths daemon call on flush(). Multiple dep sources (traces,
/// candidate key sets) can feed into one batch before flushing.
struct StorePathBatch {
    const InterningPools & pools;
    Store & store;
    VerificationSession & session;

    StorePathSet toCheck;
    std::vector<std::pair<StorePathAvailabilityDepKeyId, StorePath>> keyToStore;

    /// Add a single dep key. Skips non-StorePathExistence types and
    /// keys already resolved in this session.
    void collect(const Dep::Key & key) {
        if (key.kind != CanonicalQueryKind::StorePathAvailability) return;
        auto keyId = key.storePathAvailabilityKeyId();
        if (session.storePathValid.count(keyId)) return;
        try {
            auto decoded = decodeStorePathAvailabilityDepKey(pools, keyId);
            auto sp = decoded.storePath;
            keyToStore.emplace_back(keyId, sp);
            toCheck.insert(std::move(sp));
        } catch (...) {
            session.storePathValid[keyId] = false;
        }
    }

    /// Issue a single queryValidPaths for all collected paths.
    ///
    /// Known side-effect: queryValidPaths delegates to Store::isValidPath for
    /// each path not in the local valid-path set.  When a path is NOT found,
    /// Store::isValidPath writes a negative entry to the NarInfo disk cache
    /// (diskCache->upsertNarInfo).  This is NOT a Nix store write and cannot
    /// produce false cache hits, but it IS a disk write that occurs on the
    /// "read-only" verification path.  It is a minor, unavoidable side-effect
    /// of the underlying store API and is documented here for clarity.
    void flush() {
        if (toCheck.empty())
            return;
        auto valid = store.queryValidPaths(toCheck);
        for (auto & [keyId, sp] : keyToStore)
            session.storePathValid[keyId] = valid.count(sp) > 0;
        toCheck.clear();
        keyToStore.clear();
    }
};

// ── Dep hash resolution pipeline ─────────────────────────────────────
//
// resolveCurrentDepHash is the entry point. Two layers:
//
//   L1  Session cache check         (DepResolution<Unchecked>.checkCache)
//   L3  Compute via resolveDepHash  (DepResolution<CacheMissed>.computeAndCache)
//
// Trace-scoped subsumption (the former L2) is enforced inside resolveDepHash
// (dep-resolution-service.cc). CurrentTrace callers get stored structural
// hashes only when the same trace proved the underlying file content
// unchanged. See VerificationSession for the trace-id keyed state.

/// Attribute dep hash computation time to per-CQK counters.
/// Uses -Wswitch-enum on CanonicalQueryKind to ensure all variants are covered.
static void attributeDepHashTime(CanonicalQueryKind type, uint64_t us)
{
    switch (type) {
    case CanonicalQueryKind::FileBytes:
    case CanonicalQueryKind::RawBytes:
    case CanonicalQueryKind::NarIdentity:
        nrDepHashContentUs += us; break;
    case CanonicalQueryKind::DirectoryEntries:
        nrDepHashDirectoryUs += us; break;
    case CanonicalQueryKind::ExistenceCheck:
        nrDepHashExistenceUs += us; break;
    case CanonicalQueryKind::StorePathAvailability:
        nrDepHashStorePathUs += us; break;
    case CanonicalQueryKind::StructuredProjection:
    case CanonicalQueryKind::ImplicitStructure:
        // Per-format timing (JSON/TOML/Dir/Nix) is tracked inside
        // resolveDepHash. This captures outer overhead only.
        nrDepHashStructuredOuterUs += us;
        nrDepHashStructuredMisses++;
        break;
    case CanonicalQueryKind::GitRevisionIdentity:
        nrDepHashGitIdentityUs += us;
        nrDepHashGitIdentityMisses++;
        break;
    case CanonicalQueryKind::EnvironmentLookup:
    case CanonicalQueryKind::SessionSystemValue:
    case CanonicalQueryKind::RuntimeFetchIdentity:
    case CanonicalQueryKind::DerivedStorePath:
    case CanonicalQueryKind::VolatileExec:
    case CanonicalQueryKind::TraceValueContext:
    case CanonicalQueryKind::TraceParentSlot:
    case CanonicalQueryKind::VolatileTime:
        nrDepHashOtherUs += us;
        break;
    }
}

/// `FileStrandGate::ifPassed` mints a `FileStrandTag` proof from an
/// `ExclusiveTraceStorageAccess` capability — the exclusive-access holder
/// is also the file strand for verification-session parse caches.
class FileStrandGate : private gdp::Certifier<FileStrandTag> {
    friend struct SqliteTraceStorage;

    template<typename F>
    static auto ifPassed(const ExclusiveTraceStorageAccess &, F && f) {
        return Certifier::withProof(std::forward<F>(f));
    }
};

/// External callers use this; the typestate is internal.
template<typename TaggedDepType>
std::optional<DepHashValue> SqliteTraceStorage::resolveCurrentDepHash(
    const ExclusiveTraceStorageAccess & ea,
    const TaggedDepType & dep,
    const SemanticRegistry & registry,
    EvalState & state, VerificationSession & session)
{
    return FileStrandGate::ifPassed(ea, [&](const auto & fileTok) {
        if (auto cached = session.lookupDepHash(dep.value().key)) {
            nrDepHashCacheHits++;
            return *cached;
        }
        nrDepHashCacheMisses++;
        auto hashStart = timerStart();
        auto current = resolveDepHash(state, session, dep, registry, pools, parseCachesFor(session), fileTok);
        attributeDepHashTime(dep.value().key.kind, elapsedUs(hashStart));
        // resolveDepHash caches internally via cacheComputedHash /
        // cacheVerifiedHash — no write needed here.
        return current;
    });
}

// Explicit instantiations — the two live origin tags.
template std::optional<DepHashValue> SqliteTraceStorage::resolveCurrentDepHash<CurrentTraceDep>(
    const ExclusiveTraceStorageAccess &,
    const CurrentTraceDep &, const SemanticRegistry &,
    EvalState &, VerificationSession &);
template std::optional<DepHashValue> SqliteTraceStorage::resolveCurrentDepHash<CandidateDep>(
    const ExclusiveTraceStorageAccess &,
    const CandidateDep &, const SemanticRegistry &,
    EvalState &, VerificationSession &);

std::optional<EvalTraceHash> SqliteTraceStorage::resolveTraceContextHash(
    const ExclusiveTraceStorageAccess & ea,
    const Dep::Key & key,
    const SemanticRegistry & registry,
    EvalState & state,
    VerificationSession & session)
{
    auto parentPathId = key.attrPathId;
    auto parentRow = lookupCurrentNode(ea.blockingProof(), parentPathId);
    if (!parentRow)
        return std::nullopt;

    auto memoIt = session.traceContextMemo.find(parentPathId);
    if (memoIt != session.traceContextMemo.end()
        && memoIt->second.nodeStamp == parentRow->nodeStamp)
        return memoIt->second.traceHash;

    // Recursively verify the parent trace.  The nested verifyTrace enters
    // its own CurrentTraceScope on the parent's fullDeps; L1 writes remain
    // sound because any VerifiedHash produced there is gated by the parent
    // trace id and encodes hash(op(current F)) only after that trace proved
    // current F == its recording of F.  Cycle detection at verifyTrace's
    // entry breaks mutual recursion via `session.inProgressTraceIds`.
    std::optional<EvalTraceHash> resolved;
    if (verifyTrace(ea, parentRow->traceId, registry, state, session)) {
        auto parentTraceHash = getCurrentTraceHash(ea, parentPathId);
        if (parentTraceHash)
            resolved = parentTraceHash->value;
    }

    session.traceContextMemo.insert_or_assign(parentPathId, VerificationSession::TraceContextMemoEntry{
        .nodeStamp = parentRow->nodeStamp,
        .traceHash = resolved,
    });
    return resolved;
}

// ── Trace verification types ─────────────────────────────────────────

/**
 * File identity for coverage set lookups, using interned IDs to avoid
 * string allocation. Stored as (DepSourceId, StringId) where StringId
 * is the canonical interned ID type for file-ish path atoms. FilePathId,
 * SimpleDepKeyId, and StringId share the same StringInternTable, so converting to StringId
 * preserves identity: same interned string ⟹ same StringId value.
 *
 * Overloaded constructors accept SimpleDepKeyId, FilePathId, or StringId
 * directly, performing the tag conversion internally. This ensures
 * Content deps (keyed by SimpleDepKeyId) and StructuredProjection deps (keyed by FilePathId)
 * referring to the same file produce equal FileIdentity values.
 */
struct FileIdentity {
    DepSourceId sourceId;
    StringId pathId;

    FileIdentity(DepSourceId src, SimpleDepKeyId key) : sourceId(src), pathId(StringId(key.value)) {}
    FileIdentity(DepSourceId src, FilePathId path) : sourceId(src), pathId(StringId(path.value)) {}
    FileIdentity(DepSourceId src, StringId id) : sourceId(src), pathId(id) {}

    bool operator==(const FileIdentity &) const = default;

    struct Hash {
        size_t operator()(const FileIdentity & fi) const noexcept {
            return hashValues(fi.sourceId.value, fi.pathId.value);
        }
    };
};

static FileIdentity scFileIdentity(const Dep::Key & key) {
    if (key.dirSetHashId)
        return {key.sourceId, key.dirSetHashId};
    if (key.isStructured())
        return {key.sourceId, key.filePathId};
    assert(repoRootAddressingKind(key.kind) == RepoRootAddressingKind::DirectPath);
    return {key.sourceId, key.simpleKeyId()};
}

static FileIdentity contentFileIdentity(const Dep::Key & key) {
    assert(repoRootAddressingKind(key.kind) == RepoRootAddressingKind::DirectPath);
    return {key.sourceId, key.simpleKeyId()};
}

using FileIdentitySet = boost::unordered_flat_set<FileIdentity, FileIdentity::Hash>;

static bool containsVolatileDep(const std::vector<Dep::Key> & keys)
{
    return std::any_of(keys.begin(), keys.end(), [](const Dep::Key & key) {
        return isVolatile(key.kind);
    });
}

// ── Verified file proof (GDP continuation pattern) ───────────────────
// withVerifiedFile + VerifiedFileTag + prePopulateVerifiedDepHash deleted.
// resolveDepHash owns L1 caching via ProvenancedHash phantom types.
// Subsumption in resolveDepHash handles verified files' StructuredProjection/ImplicitStructure deps.

// VerifyOutcome is defined in verification-protocol.hh

/// What the caller must do after classifyDep().
enum class DepAction {
    Done,               ///< Already classified; no hash resolution needed.
    CheckTraceContext,  ///< Resolve hash, call recordTraceContext(matched).
    CheckNormal,        ///< Resolve hash, call recordNormal(matched, ...).
};

/**
 * Accumulated state from Pass 1 (dep classification) of verifyTrace.
 * Groups the 5 classification flags + 2 deferred index vectors + failed
 * file set into a testable struct. classifyDep() routes deps by kind;
 * record*() methods capture hash comparison results; determineOutcome()
 * produces the final VerifyOutcome from accumulated state.
 */
struct VerificationState {
    bool hasNonContentFailure = false;
    bool hasContentFailure = false;

    /// Indices into fullDeps for deferred StructuredContent deps.
    std::vector<size_t> structuralDepIndices;
    /// Indices into fullDeps for deferred ImplicitShape deps.
    std::vector<size_t> implicitShapeDepIndices;
    /// Content/Directory deps that failed hash comparison.
    FileIdentitySet failedContentFiles;
    /// Content/Directory deps that passed hash comparison.
    FileIdentitySet passedContentFiles;

    bool hasStructuralDeps() const { return !structuralDepIndices.empty(); }
    bool hasImplicitShapeDeps() const { return !implicitShapeDepIndices.empty(); }

    /// Pass 1: Classify a dep by kind. Returns what the caller should do.
    /// For Volatile/Structural/ImplicitShape: updates state directly, returns Done.
    /// For deps needing hash resolution: returns the appropriate check action.
    DepAction classifyDep(size_t index, CanonicalQueryKind type) {
        auto desc = describe(type);
        if (desc.isVolatile) {
            hasNonContentFailure = true;
            return DepAction::Done;
        }
        // GitRevisionIdentity is recorded for session identity and history
        // indexing, but primary verification checks the concrete deps directly.
        // Eagerly resolving the repo identity is expensive and, in large
        // nixpkgs evals, produced no primary-verify skips.
        if (type == CanonicalQueryKind::GitRevisionIdentity)
            return DepAction::Done;
        if (desc.behavior == QueryBehavior::ImplicitStructural) {
            implicitShapeDepIndices.push_back(index);
            return DepAction::Done;
        }
        if (desc.behavior == QueryBehavior::Structural) {
            structuralDepIndices.push_back(index);
            return DepAction::Done;
        }
        if (desc.behavior == QueryBehavior::TraceContext)
            return DepAction::CheckTraceContext;
        return DepAction::CheckNormal;
    }

    /// Record result of trace-context hash check.
    void recordTraceContext(bool matched) {
        if (!matched) hasNonContentFailure = true;
    }

    /// Record result of normal dep hash check.
    void recordNormal(bool matched, bool contentOverrideable, const FileIdentity * fileIdentity) {
        if (!contentOverrideable) {
            if (!matched)
                hasNonContentFailure = true;
            return;
        }

        if (matched) {
            if (fileIdentity) passedContentFiles.insert(*fileIdentity);
        } else {
            hasContentFailure = true;
            if (fileIdentity) failedContentFiles.insert(*fileIdentity);
        }
    }

    /// Determine outcome from accumulated Pass 1 state and Pass 2 verification results.
    /// @param structuralDepsVerified  All deferred StructuredContent deps match current hashes.
    /// @param implicitDepsVerified    All deferred ImplicitShape deps match current hashes.
    /// @param allFailuresCovered      Every failed content file has structural or implicit coverage.
    /// @param hasImplicitOnlyCoverage At least one failed file covered only by implicit (not structural).
    VerifyOutcome determineOutcome(bool structuralDepsVerified, bool implicitDepsVerified,
                                    bool allFailuresCovered, bool hasImplicitOnlyCoverage) const {
        if (hasNonContentFailure)
            return VerifyOutcome::Invalid;
        if (!hasContentFailure) {
            bool depsOk = (!hasStructuralDeps() && !hasImplicitShapeDeps())
                       || (structuralDepsVerified && implicitDepsVerified);
            return depsOk ? VerifyOutcome::Valid : VerifyOutcome::Invalid;
        }
        // hasContentFailure && need structural/implicit coverage
        if (!hasStructuralDeps() && !hasImplicitShapeDeps())
            return VerifyOutcome::Invalid;
        if (!allFailuresCovered || !structuralDepsVerified || !implicitDepsVerified)
            return VerifyOutcome::Invalid;
        return hasImplicitOnlyCoverage
            ? VerifyOutcome::ValidViaImplicitShapeOverride
            : VerifyOutcome::ValidViaStructuralOverride;
    }
};

// ── Verification context (shared parameter bundle) ───────────────────

/// Bundles the parameters threaded through verification/recovery.
/// The SemanticRegistry is immutable and pre-populated at session open.
///
/// `ea` is the `ExclusiveTraceStorageAccess` capability for the enclosing
/// `withExclusiveAccess` scope. Public SqliteTraceStorage methods take `ea`.
/// Private helper methods on SqliteTraceStorage still take `bs`; reach them via
/// `ea.blockingProof()` (aliased as `bs` below for convenience).

/// Mutability of the fields, audited 2026-05:
///
/// Read-only from the verify path:
///   - `registry`  (declared `const`; `SemanticRegistry` is session-
///     immutable post-open.)
///   - `pools`     (mutable type, but every verifier callsite only
///     uses read accessors: `resolve`, `dirSets.find`.  Internal
///     pools auto-grow on new interns from elsewhere; the verify
///     pass doesn't intern.)
///
/// Mutable via the verify path:
///   - `store`     — `patchTraceHashInMemory` writes
///     `traceCache`; every `ensureTraceHeader`/`loadFullTrace` miss
///     also populates caches.
///   - `state`     — `state.store` mutates NarInfo disk cache on
///     missed lookups (documented CLAUDE.md pitfall).
///   - `session`   — primary mutable target; `markFileVerified`,
///     `cacheVerifiedHash`, etc.
///   - `ea` / `bs` — capability tokens; lifetimes bounded by the
///     enclosing `withExclusiveAccess` / `coroBlock` scope.
struct VerifyContext {
    const ExclusiveTraceStorageAccess & ea;
    const gdp::Proof<BlockingTag> & bs;
    SqliteTraceStorage & store;
    InterningPools & pools;
    const SemanticRegistry & registry;
    EvalState & state;
    VerificationSession & session;
};

/// Result from Pass 2: outcome + set of files whose Content/Directory
/// deps passed verification (for Invalid pre-compute optimization).
///
/// `structuralCoveredFiles` / `implicitCoveredFiles` / `uncoveredCount`
/// are diagnostic fields for the pass-2 override coverage summary logged
/// at `verifyTrace` exit.  They record the exact file sets that the
/// override decision consulted, so the log can report accurate counts
/// instead of re-deriving them approximately.  Populated only when
/// `Counter::enabled`; empty sets when stats are off.
struct Pass2Result {
    VerifyOutcome outcome{};
    FileIdentitySet passedContentFiles;
    FileIdentitySet structuralCoveredFiles;
    FileIdentitySet implicitCoveredFiles;
    size_t uncoveredCount = 0; ///< files in failedContentFiles not covered by either set
};

/// Human-readable name for a VerifyOutcome.  Used in debug-gated log
/// lines.  Matches the enum-variant names in verification-protocol.hh
/// so log readers and the enum stay in sync; -Wswitch-enum keeps this
/// exhaustive.
static constexpr const char * verifyOutcomeName(VerifyOutcome outcome)
{
    switch (outcome) {
    case VerifyOutcome::Valid:                          return "Valid";
    case VerifyOutcome::ValidViaStructuralOverride:     return "ValidViaStructuralOverride";
    case VerifyOutcome::ValidViaImplicitShapeOverride:  return "ValidViaImplicitShapeOverride";
    case VerifyOutcome::Invalid:                        return "Invalid";
    }
    return "?";
}

// ── Recovery types ───────────────────────────────────────────────────

// HistoryEntry lives at namespace `eval_trace::` scope (trace-store.hh).

using TraceHashLookup = boost::unordered_flat_map<TraceHash, std::vector<size_t>, TraceHash::Hash>;

/// Result from direct hash recovery: optional result plus the deps resolved
/// while constructing the direct trace hash.
struct DirectHashResult {
    std::optional<SqliteTraceStorage::VerifyResult> result;
    std::vector<Dep> currentTraceHashDeps;
    bool allComputable = false;
};

// ── Phase functions (VerifyImpl) ─────────────────────────────────────
//
// Extracted from verifyTrace and recovery. Each phase function is a
// mechanical cut-paste with parameters made explicit via VerifyContext.
// ── Generic canonical-query verifier ────────────────────────────────
//
// Single verification pipeline, no phases.  Primary verification checks
// concrete deps directly; GitRevisionIdentity deps are retained for session
// identity/history but are no longer used as an eager dep-skip shortcut.
//
// File-static via VerifyImpl struct (friend of VerificationSession).

struct VerifyImpl {
    /// Opaque result from the recovery accept helpers. Private constructor
    /// ensures all recovery paths centralize publish/cache behavior.
    class RecoveryAcceptance {
        SqliteTraceStorage::VerifyResult result_;
        explicit RecoveryAcceptance(SqliteTraceStorage::VerifyResult && r) : result_(std::move(r)) {}
        friend struct VerifyImpl;
    public:
        SqliteTraceStorage::VerifyResult take() && { return std::move(result_); }
    };

    /// Batch all StorePathAvailability deps into a single queryValidPaths RPC.
    static void runStorePathBatch(
        const std::vector<Dep> & fullDeps, VerifyContext & ctx)
    {
        StorePathBatch batch{ctx.pools, *ctx.state.store, ctx.session};
        for (auto & idep : fullDeps)
            batch.collect(idep.key);
        batch.flush();
    }

    /// Classify each dep, resolve hashes, accumulate state.
    static VerificationState runPass1(
        const CurrentTraceScope & scope,
        const std::vector<Dep> & fullDeps,
        VerifyContext & ctx)
    {
        VerificationState vs;

        for (size_t i = 0; i < fullDeps.size(); ++i) {
            auto & idep = fullDeps[i];
            nrDepsChecked++;

            switch (vs.classifyDep(i, idep.key.kind)) {
            case DepAction::Done:
                break;
            case DepAction::CheckTraceContext: {
                auto traceContextHash = ctx.store.resolveTraceContextHash(
                    ctx.ea, idep.key, ctx.registry, ctx.state, ctx.session);
                bool matched = false;
                if (traceContextHash) {
                    auto * expected = std::get_if<DepHash>(&idep.hash);
                    matched = expected && expected->value == *traceContextHash;
                }
                if (!matched) {
                    nrVerificationsFailed++;
                    // Include the expected + current hashes when we have them,
                    // matching the Normal dep FAILED log shape below.  The
                    // expected hash MUST be a DepHash for TraceContext
                    // deps by construction (the recorder only writes a digest
                    // into TraceContext dep hashes).  A non-digest variant
                    // here is a recorder-side bug; warn + assert so it
                    // surfaces instead of silently rendering "(not-digest)".
                    auto * expectedHash = std::get_if<DepHash>(&idep.hash);
                    if (!expectedHash) {
                        warn("eval-trace/verify: TraceContext dep for '%s' has non-digest stored hash "
                             "(recorder-side invariant violation)",
                             resolveDep(ctx.pools, ctx.store.vocab, idep).key);
                        assert(expectedHash && "TraceContext dep.hash must be a digest");
                    }
                    if (verbosity >= lvlDebug) {
                        debug("eval-trace/verify: TraceContext dep FAILED type=%s key='%s' "
                            "expected=%s current=%s",
                            queryKindName(idep.key.kind), resolveDep(ctx.pools, ctx.store.vocab, idep).key,
                            expectedHash ? expectedHash->value.toHex() : std::string("(invalid)"),
                            traceContextHash ? traceContextHash->toHex() : std::string("(null)"));
                    }
                }
                vs.recordTraceContext(matched);
                break;
            }
            case DepAction::CheckNormal: {
                auto current = ctx.store.resolveCurrentDepHash(
                    ctx.ea, scope.tag(idep), ctx.registry, ctx.state, ctx.session);
                bool matched = current && *current == idep.hash;
                bool overrideable = isContentOverrideable(idep.key.kind);
                std::optional<FileIdentity> fileIdentity;
                if (overrideable)
                    fileIdentity = contentFileIdentity(idep.key);
                if (!matched) {
                    nrVerificationsFailed++;
                    if (verbosity >= lvlDebug) {
                        debug("eval-trace/verify: Normal dep FAILED type=%s key='%s' expected=%s current=%s",
                            queryKindName(idep.key.kind), resolveDep(ctx.pools, ctx.store.vocab, idep).key,
                            std::visit([](const auto & h) -> std::string {
                                if constexpr (std::is_same_v<std::decay_t<decltype(h)>, DepHash>)
                                    return h.value.toHex();
                                else return h;
                            }, idep.hash),
                            current ? std::visit([](const auto & h) -> std::string {
                                if constexpr (std::is_same_v<std::decay_t<decltype(h)>, DepHash>)
                                    return h.value.toHex();
                                else return h;
                            }, *current) : std::string("(null)"));
                    }
                    vs.recordNormal(false, overrideable, fileIdentity ? &*fileIdentity : nullptr);
                } else {
                    vs.recordNormal(true, overrideable, fileIdentity ? &*fileIdentity : nullptr);
                }
                break;
            }
            }
        }

        return vs;
    }

    // ── Named dep verifiers for Pass 2 ──────────────────────────────

    /// Verify deps in indices, skipping any dep whose file identity is in
    /// skipFiles. Used when certain files have already passed content-dep
    /// verification and their structural/implicit deps are trivially valid.
    static bool verifyDepsExcluding(
        const CurrentTraceScope & scope,
        const std::vector<size_t> & indices,
        const std::vector<Dep> & fullDeps,
        const FileIdentitySet & skipFiles,
        VerifyContext & ctx)
    {
        for (auto idx : indices) {
            auto & idep = fullDeps[idx];
            if (skipFiles.count(scFileIdentity(idep.key))) continue;
            nrDepsChecked++;
            auto current = ctx.store.resolveCurrentDepHash(
                ctx.ea, scope.tag(idep), ctx.registry, ctx.state, ctx.session);
            if (!current || *current != idep.hash) {
                nrVerificationsFailed++;
                return false;
            }
        }
        return true;
    }

    /// Verify deps in indices that cover a file in onlyFiles, skipping deps
    /// whose file identity is in skipFiles. Used for implicit-shape deps
    /// during the content-failure path: only check deps covering failed files,
    /// and skip files already covered by structural deps.
    static bool verifyDepsOnlyFor(
        const CurrentTraceScope & scope,
        const std::vector<size_t> & indices,
        const std::vector<Dep> & fullDeps,
        const FileIdentitySet & skipFiles,
        const FileIdentitySet & onlyFiles,
        VerifyContext & ctx)
    {
        for (auto idx : indices) {
            auto & idep = fullDeps[idx];
            auto fileKey = scFileIdentity(idep.key);
            if (skipFiles.count(fileKey)) continue;
            if (!onlyFiles.count(fileKey)) continue;
            nrDepsChecked++;
            auto current = ctx.store.resolveCurrentDepHash(
                ctx.ea, scope.tag(idep), ctx.registry, ctx.state, ctx.session);
            if (!current || *current != idep.hash) {
                nrVerificationsFailed++;
                return false;
            }
        }
        return true;
    }

    /// Step 4: Structural/implicit override resolution + coverage analysis.
    static Pass2Result runPass2(
        const CurrentTraceScope & scope,
        const std::vector<Dep> & fullDeps,
        const VerificationState & vs,
        VerifyContext & ctx)
    {
        Pass2Result result;
        bool structuralDepsVerified = true;
        bool implicitDepsVerified = true;
        bool allFailuresCovered = true;
        bool hasImplicitOnlyCoverage = false;

        if (!vs.hasNonContentFailure && !vs.hasContentFailure) {
            if (vs.hasStructuralDeps() || vs.hasImplicitShapeDeps()) {
                // All content deps passed. Skip structural/implicit deps for
                // files whose content dep already verified — they are covered.
                structuralDepsVerified = verifyDepsExcluding(
                    scope, vs.structuralDepIndices, fullDeps, vs.passedContentFiles, ctx);
                implicitDepsVerified = structuralDepsVerified
                    && verifyDepsExcluding(
                        scope, vs.implicitShapeDepIndices, fullDeps, vs.passedContentFiles, ctx);
            }
        } else if (!vs.hasNonContentFailure && vs.hasContentFailure
                   && (vs.hasStructuralDeps() || vs.hasImplicitShapeDeps())) {
            result.passedContentFiles = vs.passedContentFiles;

            FileIdentitySet structuralCoveredFiles;
            for (auto idx : vs.structuralDepIndices) {
                auto & skey = fullDeps[idx].key;
                structuralCoveredFiles.insert(scFileIdentity(skey));
                if (skey.dirSetHashId) {
                    try {
                        auto dsHash = std::string(ctx.pools.resolve(skey.dirSetHashId));
                        auto it = ctx.pools.dirSets.find(dsHash);
                        if (it == ctx.pools.dirSets.end()) continue;
                        for (auto & dir : it->second)
                            structuralCoveredFiles.insert(FileIdentity{dir.sourceId, dir.filePathId});
                    } catch (...) {}
                }
            }

            FileIdentitySet implicitCoveredFiles;
            for (auto idx : vs.implicitShapeDepIndices)
                implicitCoveredFiles.insert(scFileIdentity(fullDeps[idx].key));

            // When stats are enabled, iterate every failed file to count
            // `uncovered` accurately for the pass-2 diagnostic log.
            // When stats are off, short-circuit on the first uncovered
            // file (original behaviour) since the count is unused.
            size_t uncovered = 0;
            for (auto & failedFile : vs.failedContentFiles) {
                bool structural = structuralCoveredFiles.count(failedFile);
                bool implicit = implicitCoveredFiles.count(failedFile);
                if (!structural && !implicit) {
                    allFailuresCovered = false;
                    if (!Counter::enabled)
                        break;
                    uncovered++;
                    continue;
                }
                if (!structural && implicit) {
                    hasImplicitOnlyCoverage = true;
                }
            }

            // Retain the exact coverage sets + `uncovered` count for the
            // diagnostic log at the `verifyTrace` exit.  Persisted only
            // when stats are enabled; no cost otherwise.
            if (Counter::enabled) {
                result.structuralCoveredFiles = structuralCoveredFiles;
                result.implicitCoveredFiles = implicitCoveredFiles;
                result.uncoveredCount = uncovered;
            }

            if (!allFailuresCovered) {
                result.outcome = vs.determineOutcome(
                    structuralDepsVerified, implicitDepsVerified,
                    allFailuresCovered, hasImplicitOnlyCoverage);
                return result;
            }

            // Structural deps: skip files that already passed content-dep
            // verification — they do not need structural coverage.
            structuralDepsVerified = verifyDepsExcluding(
                scope, vs.structuralDepIndices, fullDeps, result.passedContentFiles, ctx);
            // Implicit-shape deps: only check deps covering failed files (the
            // ones that need implicit coverage), skipping those already covered
            // structurally.
            implicitDepsVerified = structuralDepsVerified
                && verifyDepsOnlyFor(
                    scope, vs.implicitShapeDepIndices, fullDeps,
                    structuralCoveredFiles, vs.failedContentFiles, ctx);
        }

        result.outcome = vs.determineOutcome(structuralDepsVerified, implicitDepsVerified,
                                              allFailuresCovered, hasImplicitOnlyCoverage);
        return result;
    }

    /// Apply outcome (patch trace hash, memoize trace validity, etc.)
    static bool applyOutcome(
        TraceId traceId,
        const std::vector<Dep> & fullDeps,
        const VerificationState & vs,
        const Pass2Result & p2,
        VerifyContext & ctx)
    {
        switch (p2.outcome) {
        case VerifyOutcome::Valid:
        case VerifyOutcome::ValidViaStructuralOverride:
            ctx.session.verifiedTraceIds.insert(traceId);
            break;

        case VerifyOutcome::ValidViaImplicitShapeOverride: {
            // Soundness of ValidViaImplicitShapeOverride (argument distributed
            // across 4 files, consolidated here):
            //
            // The trace hash is recomputed from CURRENT dep hashes, not stored ones:
            // 1. Computed deps: L1 contains current hashes (dep-resolution-service.cc)
            // 2. TraceContext deps: freshly resolved during pass 1
            // 3. patchTraceHashInMemory is in-memory only — children see it via
            //    getCurrentTraceHash(); the DB retains the original hash for
            //    cross-session recovery via tryStructuralVariantRecovery
            // 4. Structural/ImplicitStructural deps whose file was skipped by
            //    Pass 2's verifyDepsExcluding because it was in
            //    passedContentFiles: no L1 entry, fallback uses idep (stored
            //    hash).  Sound because passedContentFiles membership means the
            //    file's content dep matched — current F = recording F — so
            //    hash(op(current F)) = hash(op(recording F)) = idep.hash.
            std::vector<Dep> traceHashInputDeps;
            traceHashInputDeps.reserve(fullDeps.size());
            for (auto & idep : fullDeps) {
                if (queryBehavior(idep.key.kind) == QueryBehavior::TraceContext) {
                    auto traceContextHash = ctx.store.resolveTraceContextHash(
                        ctx.ea, idep.key, ctx.registry, ctx.state, ctx.session);
                    traceHashInputDeps.push_back(traceContextHash
                        ? Dep{idep.key, DepHashValue(DepHash{*traceContextHash})}
                        : idep);
                } else {
                    auto cached = ctx.session.lookupDepHash(idep.key);
                    traceHashInputDeps.push_back(cached && *cached
                        ? Dep{idep.key, **cached} : idep);
                }
            }
            auto patchedTraceHash = ctx.store.computePresortedTraceHash(traceHashInputDeps);
            ctx.store.patchTraceHashInMemory(ctx.bs, traceId, patchedTraceHash);
            ctx.session.verifiedTraceIds.insert(traceId);
            break;
        }

        case VerifyOutcome::Invalid:
            // Only the invalid path needs trace-scoped subsumption facts:
            // valid traces return immediately, while direct-hash recovery can
            // reuse stored structural/implicit hashes for files whose content
            // dep matched in THIS trace.  We do not precompute every missing
            // structural dep here: GitIdentity recovery may succeed first, and
            // direct-hash recovery already resolves the remaining deps on
            // demand before structural-variant recovery reuses those L1 hits.
            // Delaying this population removes a per-content-dep hash-set
            // insert from the hot valid path and prevents session-wide file
            // verification from leaking between trace ids.
            for (auto & file : vs.passedContentFiles)
                ctx.session.markFileIdentityVerified(traceId, file.sourceId, file.pathId);
            if (verbosity >= lvlDebug && !vs.passedContentFiles.empty()) {
                debug("eval-trace/verify: traceId=%u marked %zu files for recovery subsumption",
                      traceId.value, vs.passedContentFiles.size());
            }
            break;
        }

        if (p2.outcome != VerifyOutcome::Invalid)
            ctx.session.clearTraceVerifiedFiles(traceId);

        return p2.outcome != VerifyOutcome::Invalid;
    }

    // ── recovery strategies ─────────────────────────────────────────

    /// Load history entries for a given attr path from the History table.
    static std::vector<HistoryEntry> loadHistory(
        AttrPathId pathId, VerifyContext & ctx)
    {
        auto scanStart = timerStart();
        nrRecoveryScanHistoryCount++;
        std::vector<HistoryEntry> entries;
        auto & st = *ctx.store._state;
        auto use(st.scanHistoryForAttr.use());
        bindTaggedEvalTraceHash(use, ctx.store.currentStableRecoveryKey());
        use(static_cast<int64_t>(pathId.value));
        while (use.next()) {
            auto [thData, thSize] = use.getBlob(3);
            entries.push_back({
                DepKeySetId(static_cast<uint32_t>(use.getInt(0))),
                TraceId(static_cast<uint32_t>(use.getInt(1))),
                ResultId(static_cast<uint32_t>(use.getInt(2))),
                evalTraceHashFromBlob<TraceHash>(thData, thSize),
            });
        }
        nrRecoveryScanHistoryRows += entries.size();
        nrRecoveryScanHistoryUs += elapsedUs(scanStart);
        return entries;
    }

    static std::vector<HistoryEntry> loadHistoryByTraceHash(
        AttrPathId pathId,
        const TraceHash & traceHash,
        VerifyContext & ctx)
    {
        auto lookupStart = timerStart();
        nrRecoveryDirectHashLookupCount++;
        std::vector<HistoryEntry> entries;
        auto & st = *ctx.store._state;
        auto use(st.lookupHistoryByTraceHash.use());
        bindTaggedEvalTraceHash(use, ctx.store.currentStableRecoveryKey());
        use(static_cast<int64_t>(pathId.value));
        bindTaggedEvalTraceHash(use, traceHash);
        while (use.next()) {
            entries.push_back({
                DepKeySetId(static_cast<uint32_t>(use.getInt(0))),
                TraceId(static_cast<uint32_t>(use.getInt(1))),
                ResultId(static_cast<uint32_t>(use.getInt(2))),
                traceHash,
            });
        }
        nrRecoveryDirectHashLookupRows += entries.size();
        nrRecoveryDirectHashLookupUs += elapsedUs(lookupStart);
        return entries;
    }

    /// Build in-memory trace_hash -> entry index lookup.
    static TraceHashLookup buildTraceHashLookup(
        const std::vector<HistoryEntry> & history)
    {
        TraceHashLookup lookup;
        for (size_t i = 0; i < history.size(); i++)
            lookup[history[i].traceHash].push_back(i);
        return lookup;
    }

    /// Look up a trace hash in the history.
    static const std::vector<size_t> * lookupCandidates(
        const TraceHash & candidateHash,
        const TraceHashLookup & lookup)
    {
        auto it = lookup.find(candidateHash);
        if (it == lookup.end()) return nullptr;
        return &it->second;
    }

    static bool contributesToTraceHash(const Dep::Key & key)
    {
        return nix::contributesToTraceHash(key.kind);
    }

    static size_t traceHashDepCount(const std::vector<Dep> & deps)
    {
        size_t count = 0;
        for (const auto & dep : deps)
            if (contributesToTraceHash(dep.key))
                count++;
        return count;
    }

    static bool hasImplicitStructureGuard(const std::vector<Dep::Key> & keys)
    {
        return std::any_of(keys.begin(), keys.end(), [](const Dep::Key & key) {
            return key.kind == CanonicalQueryKind::ImplicitStructure;
        });
    }

    // Key-set variants moved to SqliteTraceStorage::extractGoverningRepoId /
    // ::allDepsGitRecoverable (overloaded for both Dep and Dep::Key ranges).

    static bool validateImplicitStructureGuards(
        const std::vector<Dep> & deps,
        VerifyContext & ctx)
    {
        auto guardStart = timerStart();
        Finally guardTimer{[&] {
            nrRecoveryImplicitGuardTimeUs += elapsedUs(guardStart);
        }};
        return OriginScopeFactory::enterCandidate([&](const CandidateScope & scope) {
            for (const auto & dep : deps) {
                if (dep.key.kind != CanonicalQueryKind::ImplicitStructure)
                    continue;
                nrRecoveryImplicitGuardChecks++;
                auto current = ctx.store.resolveCurrentDepHash(
                    ctx.ea, scope.tag(dep), ctx.registry, ctx.state, ctx.session);
                if (!current || *current != dep.hash) {
                    nrRecoveryImplicitGuardFailures++;
                    return false;
                }
            }
            return true;
        });
    }

    /// Accept a recovered trace whose deps were already loaded by the caller.
    /// Still validates implicit structural guards before publish/cache. The
    /// result payload is loaded only after the candidate passes those checks.
    static RecoveryAcceptance acceptRecoveredTraceUnchecked(
        TraceId traceId,
        ResultId resultId,
        AttrPathId pathId,
        VerifyContext & ctx)
    {
        ctx.store.publishStateChange(ctx.bs, pathId, traceId, resultId, /*insertHistory=*/false);
        ctx.session.verifiedTraceIds.insert(traceId);
        return RecoveryAcceptance{SqliteTraceStorage::VerifyResult{
            ctx.store.decodeCachedResult(ctx.bs, resultId),
            traceId}};
    }

    static std::optional<RecoveryAcceptance> acceptRecoveredTraceWithLoadedDeps(
        TraceId traceId, ResultId resultId,
        const std::vector<Dep> & candidateDeps,
        AttrPathId pathId,
        VerifyContext & ctx)
    {
        if (!validateImplicitStructureGuards(candidateDeps, ctx))
            return std::nullopt;

        return acceptRecoveredTraceUnchecked(traceId, resultId, pathId, ctx);
    }

    static std::optional<RecoveryAcceptance> acceptRecoveredTraceWithKeySet(
        TraceId traceId, ResultId resultId,
        const std::vector<Dep::Key> & candidateKeys,
        AttrPathId pathId,
        VerifyContext & ctx)
    {
        if (!hasImplicitStructureGuard(candidateKeys))
            return acceptRecoveredTraceUnchecked(traceId, resultId, pathId, ctx);

        nrRecoveryImplicitGuardCandidates++;
        nrRecoveryImplicitGuardFullTraceLoads++;
        auto candidateDeps = ctx.store.loadFullTrace(ctx.ea, traceId);
        return acceptRecoveredTraceWithLoadedDeps(
            traceId, resultId, *candidateDeps, pathId, ctx);
    }

    /// Accept a recovered trace: check triedTraceIds, publish recovery, cache.
    static std::optional<RecoveryAcceptance> acceptRecoveredTrace(
        const HistoryEntry & entry,
        AttrPathId pathId,
        boost::unordered_flat_set<TraceId, TraceId::Hash> & triedTraceIds,
        VerifyContext & ctx)
    {
        if (triedTraceIds.count(entry.traceId))
            return std::nullopt;
        triedTraceIds.insert(entry.traceId);

        auto candidateKeys = ctx.store.loadKeySet(ctx.ea, entry.depKeySetId);
        if (!candidateKeys)
            return std::nullopt;

        return acceptRecoveredTraceWithKeySet(
            entry.traceId, entry.resultId, *candidateKeys, pathId, ctx);
    }

    /// Strategy 1: indexed lookup via session fingerprint or GitIdentity dep.
    static std::optional<SqliteTraceStorage::VerifyResult> tryGitIdentityRecovery(
        const CurrentTraceScope & scope,
        const std::vector<Dep> & oldDeps,
        AttrPathId pathId,
        VerifyContext & ctx)
    {
        auto gitIdentityStart = timerStart();
        nrRecoveryGitIdentityAttempts++;
        Finally gitIdentityTimer{[&] {
            nrRecoveryGitIdentityTimeUs += elapsedUs(gitIdentityStart);
        }};

        // Extract governing repo id from old deps (stable across git revisions)
        auto repoId = extractGoverningRepoId(oldDeps);
        if (!repoId)
            return std::nullopt;

        // Check all deps are eligible for git identity recovery
        if (!allDepsGitRecoverable(oldDeps, *repoId))
            return std::nullopt;

        // Use CURRENT git identity hash. Primary verification no longer runs
        // an eager GitIdentity pre-pass, so compute lazily only if this
        // recovery strategy is actually eligible.
        std::optional<CurrentGitIdentityHash> currentHash;
        auto currentIt = ctx.session.gitIdentityCache.find(*repoId);
        if (currentIt != ctx.session.gitIdentityCache.end()) {
            currentHash = currentIt->second;
        } else {
            for (auto & dep : oldDeps) {
                if (dep.key.kind != CanonicalQueryKind::GitRevisionIdentity)
                    continue;
                if (dep.key.governingRepoId != *repoId)
                    continue;

                auto current = ctx.store.resolveCurrentDepHash(
                    ctx.ea, scope.tag(dep), ctx.registry, ctx.state, ctx.session);
                if (current) {
                    if (auto * hash = std::get_if<DepHash>(&*current))
                        currentHash = CurrentGitIdentityHash{hash->value};
                }
                break;
            }
        }
        if (!currentHash)
            return std::nullopt;

        {
            struct GitIdentityCandidateRef {
                DepKeySetId depKeySetId;
                TraceId traceId;
                ResultId resultId;
            };
            std::vector<GitIdentityCandidateRef> candidates;
            {
                auto lookupStart = timerStart();
                nrRecoveryGitIdentityLookupCount++;
                auto & st = *ctx.store._state;
                auto use(st.lookupHistoryByGitIdentity.use());
                bindTaggedEvalTraceHash(use, ctx.store.currentStableRecoveryKey());
                use(static_cast<int64_t>(pathId.value));
                bindTaggedEvalTraceHash(use, *currentHash);
                while (use.next()) {
                    candidates.push_back({
                        DepKeySetId(static_cast<uint32_t>(use.getInt(0))),
                        TraceId(static_cast<uint32_t>(use.getInt(1))),
                        ResultId(static_cast<uint32_t>(use.getInt(2))),
                    });
                }
                nrRecoveryGitIdentityLookupRows += candidates.size();
                nrRecoveryGitIdentityLookupUs += elapsedUs(lookupStart);
            }
            nrRecoveryGitIdentityCandidates += candidates.size();

            if (candidates.empty())
                return std::nullopt;

            for (const auto & candidate : candidates) {
                // Verify the candidate trace is also git-recoverable with the same repo.
                auto candidateKeys = ctx.store.loadKeySet(ctx.ea, candidate.depKeySetId);
                if (!candidateKeys) {
                    nrRecoveryGitIdentityRejected++;
                    continue;
                }
                auto candidateRepo = extractGoverningRepoId(*candidateKeys);
                if (!candidateRepo || *candidateRepo != *repoId) {
                    nrRecoveryGitIdentityRejected++;
                    continue;
                }
                if (!allDepsGitRecoverable(*candidateKeys, *candidateRepo)) {
                    nrRecoveryGitIdentityRejected++;
                    continue;
                }

                auto acceptance = acceptRecoveredTraceWithKeySet(
                    candidate.traceId, candidate.resultId, *candidateKeys, pathId, ctx);
                if (!acceptance) {
                    nrRecoveryGitIdentityRejected++;
                    continue;
                }
                nrRecoveryGitIdentityAccepted++;
                nrRecoveryGitIdentityHits++;
                if (verbosity >= lvlDebug) {
                    debug("eval-trace/recovery: GitIdentity-indexed recovery succeeded for '%s'",
                        ctx.store.vocab.displayPath(pathId));
                }
                return std::move(*acceptance).take();
            }
            return std::nullopt;
        }
    }

    /// Strategy 2: Recompute all dep hashes, compute trace_hash, lookup.
    ///
    /// Direct-hash recovery iterates the CURRENT trace's oldDeps, so the
    /// caller hands us a CurrentTraceScope.  We cannot produce a
    /// CandidateDep here — a type error.
    static DirectHashResult tryDirectHashRecovery(
        const CurrentTraceScope & scope,
        const std::vector<Dep> & oldDeps,
        AttrPathId pathId,
        VerifyContext & ctx)
    {
        DirectHashResult dr;
        dr.allComputable = true;

        auto depRecomputeStart = timerStart();
        const size_t hashableDepCount = traceHashDepCount(oldDeps);
        size_t resolvedCount = 0;
        std::vector<std::optional<DepHashValue>> resolved(oldDeps.size());

        if (dr.allComputable) {
            for (size_t i = 0; i < oldDeps.size(); ++i) {
                if (!contributesToTraceHash(oldDeps[i].key))
                    continue;
                auto cached = ctx.session.lookupDepHash(oldDeps[i].key);
                if (!cached)
                    continue;
                if (!cached->has_value()) {
                    dr.allComputable = false;
                    break;
                }
                resolved[i] = **cached;
                resolvedCount++;
            }
        }

        auto resolveIndex = [&](size_t depIdx) -> bool {
            auto & idep = oldDeps[depIdx];
            if (!contributesToTraceHash(idep.key))
                return true;
            if (resolved[depIdx].has_value())
                return true;
            if (queryBehavior(idep.key.kind) == QueryBehavior::TraceContext) {
                auto traceContextHash = ctx.store.resolveTraceContextHash(
                    ctx.ea, idep.key, ctx.registry, ctx.state, ctx.session);
                if (!traceContextHash)
                    return false;
                resolved[depIdx] = DepHashValue(DepHash{*traceContextHash});
                resolvedCount++;
                return true;
            }

            // Tagging via the CurrentTraceScope: subsumption returns dep.hash
            // (which equals current-state resolution, confirmed by the final
            // trace-hash match) and writes VerifiedHash to L1.  Structural
            // variant recovery runs in a separate CandidateScope and tags
            // with CandidateDep — distinct type, distinct L1 write rules.
            auto current = ctx.store.resolveCurrentDepHash(
                ctx.ea, scope.tag(idep), ctx.registry, ctx.state, ctx.session);

            if (!current)
                return false;
            resolved[depIdx] = *current;
            resolvedCount++;
            return true;
        };

        if (dr.allComputable) {
            // Resolve deps most likely to make direct recovery impossible
            // first.  Structural deps are the dominant resolve-failure source
            // in current benchmarks, and TraceContext deps can fail without
            // contributing useful partial direct-hash work.
            for (size_t i = 0; i < oldDeps.size(); ++i) {
                auto behavior = queryBehavior(oldDeps[i].key.kind);
                if (behavior != QueryBehavior::Structural
                    && behavior != QueryBehavior::TraceContext)
                    continue;
                if (!resolveIndex(i)) {
                    dr.allComputable = false;
                    break;
                }
            }
        }

        if (dr.allComputable) {
            for (size_t i = 0; i < oldDeps.size(); ++i) {
                auto behavior = queryBehavior(oldDeps[i].key.kind);
                if (behavior == QueryBehavior::Structural
                    || behavior == QueryBehavior::TraceContext)
                    continue;
                if (!resolveIndex(i)) {
                    dr.allComputable = false;
                    break;
                }
            }
        }

        if (dr.allComputable) {
            dr.currentTraceHashDeps.reserve(hashableDepCount);
            for (size_t i = 0; i < oldDeps.size(); ++i) {
                if (!contributesToTraceHash(oldDeps[i].key))
                    continue;
                assert(resolved[i].has_value());
                dr.currentTraceHashDeps.push_back({oldDeps[i].key, *resolved[i]});
            }
        }

        nrRecoveryDepRecomputeUs += elapsedUs(depRecomputeStart);
        nrRecoveryDepRecomputeCount += resolvedCount;

        if (verbosity >= lvlDebug) {
            debug("eval-trace/recovery: recomputed %zu/%zu dep hashes for '%s'",
                  resolvedCount, hashableDepCount, ctx.store.vocab.displayPath(pathId));
        }

        if (dr.allComputable) {
            auto directHashStart = timerStart();
            auto currentTraceHash = ctx.store.computePresortedTraceHash(dr.currentTraceHashDeps);

            boost::unordered_flat_set<TraceId, TraceId::Hash> triedTraceIds;
            auto candidates = loadHistoryByTraceHash(pathId, currentTraceHash, ctx);
            for (const auto & entry : candidates) {
                if (auto acceptance = acceptRecoveredTrace(entry, pathId, triedTraceIds, ctx)) {
                    if (verbosity >= lvlDebug) {
                        debug("eval-trace/recovery: direct hash recovery succeeded for '%s'",
                            ctx.store.vocab.displayPath(pathId));
                    }
                    nrRecoveryDirectHashHits++;
                    nrRecoveryDirectHashTimeUs += elapsedUs(directHashStart);
                    dr.result = std::move(*acceptance).take();
                    return dr;
                }
            }
            nrRecoveryDirectHashTimeUs += elapsedUs(directHashStart);
            // DirectHash missed — log the unmatched target hash.  The
            // recomputed dep count is already logged one line above
            // ("eval-trace/recovery: recomputed N/M dep hashes for ...").
            if (verbosity >= lvlDebug) {
                debug("eval-trace/recovery: DirectHash miss: target=%s no trace in History matched",
                      currentTraceHash.value.toHex());
            }
        }

        return dr;
    }

    /// Strategy 3: Group candidates by dep key set, batch-resolve, hash each.
    ///
    /// Takes a `CandidateScope` — the only way to tag CandidateDeps.  The
    /// orchestrator enters the scope via OriginScopeFactory::enterCandidate
    /// before calling this function.  Inside this function, tagging a Dep
    /// produces CandidateDep; there is no path to forge a CurrentTraceDep
    /// (the CurrentTraceScope type is not reachable from this scope).
    static std::optional<SqliteTraceStorage::VerifyResult> tryStructuralVariantRecovery(
        const CandidateScope & scope,
        TraceId oldTraceId,
        AttrPathId pathId,
        const std::vector<HistoryEntry> & history,
        const TraceHashLookup & lookup,
        VerifyContext & ctx)
    {
        auto structVariantStart = timerStart();

        if (verbosity >= lvlDebug) {
            debug("eval-trace/recovery: structural variant scan for '%s' -- scanning %zu history entries",
                  ctx.store.vocab.displayPath(pathId), history.size());
        }

        auto * oldHeader = ctx.store.ensureTraceHeader(ctx.bs, oldTraceId);
        if (!oldHeader)
            throw Error("trace %d not found", oldTraceId.value);
        auto oldDepKeySetId = oldHeader->depKeySetId;

        const bool captureMismatchTelemetry =
            ctx.state.settings.useStructuralRecoveryMismatchTelemetry;

        boost::unordered_flat_set<TraceId, TraceId::Hash> triedTraceIds;
        struct StructuralVariantGroup {
            DepKeySetId depKeySetId;
            TraceId repTraceId;
            std::shared_ptr<const std::vector<Dep::Key>> keys;
            std::vector<DepHashValue> historicalValues;
        };
        std::vector<StructuralVariantGroup> structGroups;
        structGroups.reserve(history.size());
        boost::unordered_flat_set<DepKeySetId, DepKeySetId::Hash> seenDepKeySets;
        seenDepKeySets.reserve(history.size());
        for (auto & e : history) {
            if (e.depKeySetId == oldDepKeySetId)
                continue;
            if (!seenDepKeySets.insert(e.depKeySetId).second)
                continue;
            structGroups.push_back({
                .depKeySetId = e.depKeySetId,
                .repTraceId = e.traceId,
            });
        }

        std::vector<Dep> repDeps;
        constexpr size_t normalStructVariantChunkSize = 16;
        const size_t chunkSize = captureMismatchTelemetry ? 1 : normalStructVariantChunkSize;
        for (size_t chunkStart = 0; chunkStart < structGroups.size(); chunkStart += chunkSize) {
            const size_t chunkEnd = std::min(chunkStart + chunkSize, structGroups.size());

            // Load only the candidates we are about to try. A successful
            // early candidate no longer pays to deserialize every later
            // keyset. Telemetry mode uses one candidate per chunk so stored
            // historical values are also loaded only for tried candidates.
            {
                auto preloadStart = timerStart();
                for (size_t i = chunkStart; i < chunkEnd; ++i) {
                    auto & group = structGroups[i];
                    if (captureMismatchTelemetry) {
                        auto fullDeps = ctx.store.loadFullTrace(ctx.ea, group.repTraceId);
                        auto keys = std::make_shared<std::vector<Dep::Key>>();
                        keys->reserve(fullDeps->size());
                        group.historicalValues.reserve(fullDeps->size());
                        for (const auto & dep : *fullDeps) {
                            keys->push_back(dep.key);
                            group.historicalValues.push_back(dep.hash);
                        }
                        group.keys = std::move(keys);
                    } else {
                        group.keys = ctx.store.loadKeySet(ctx.ea, group.depKeySetId);
                    }
                }
                nrStructVariantLoadKeySetUs += elapsedUs(preloadStart);
            }

            // Batch StorePathAvailability for this chunk. This keeps the
            // daemon query amortized across nearby candidates without forcing
            // keyset loads for candidates after an early hit.
            {
                StorePathBatch batch{ctx.pools, *ctx.state.store, ctx.session};
                for (size_t i = chunkStart; i < chunkEnd; ++i) {
                    if (!structGroups[i].keys)
                        continue;
                    for (auto & key : *structGroups[i].keys)
                        batch.collect(key);
                }
                batch.flush();
            }

            // Recovery itself only needs dep keys: every candidate value below
            // is recomputed against current state before hashing. Stored
            // historical values are loaded only for opt-in mismatch telemetry.
            // We iterate with CandidateDep origin so that `resolveDepHash`
            // takes the compute path for every dep — returning
            // `hash(op(current F))` for Structural deps rather than the
            // candidate's recorded value. This keeps
            // `computePresortedTraceHash(repDeps)` honest: it matches a
            // history entry iff the candidate's evaluation would produce the
            // same result against the current filesystem. Historical-hash
            // poisoning of L1 is also unrepresentable — `VerifiedSubsumption`
            // can only be minted from a `CurrentTraceDep`.
            //
            // L1 reads are shared: if the current trace's verifyTrace already
            // resolved a key K (ComputedHash from compute path, or VerifiedHash
            // on the CurrentTrace subsumption path), SV's iteration hits L1 and
            // avoids recomputing — the L1 invariant
            // (`lookup(K) == resolve(K, current_state)`) guarantees soundness.
            for (size_t groupIdx = chunkStart; groupIdx < chunkEnd; ++groupIdx) {
                auto & group = structGroups[groupIdx];
                if (!group.keys)
                    continue;
                nrStructVariantCandidates++;
                auto & repKeys = *group.keys;

                repDeps.clear();
                bool repComputable = true;
            // Capture the first abort cause for diagnostic logging.
            // Populated only on the abort path; the live iteration path
            // never reads these.
            enum class AbortReason { None, Volatile, TraceContextMiss, ResolveFailed };
            AbortReason abortReason = AbortReason::None;
            std::optional<Dep::Key> abortKey;
            // ── SV any-dep early-exit gating signal (measurement-only) ──
            //
            // The proposed optimisation is: break as soon as any resolved
            // `current` hash disagrees with the stored historical hash (for singleton
            // DepKeySetId buckets — 98.3% of the distribution — a single
            // mismatch guarantees `computePresortedTraceHash(repDeps)` will
            // not match, because the canonical hash is sorted over
            // `(key, value)` pairs).  Whether it's worth implementing
            // depends on whether the first hash mismatch typically occurs
            // EARLIER than the first resolveFail, or at the SAME dep (same
            // file changed in both dimensions).  The numbers below resolve
            // that question.
            //
            // IMPORTANT: measurement only.  We do NOT break on mismatch —
            // if we did, we'd observe savings but couldn't distinguish
            // "proposal wins" from "instrumentation made the loop
            // shorter".  Loading historical values is not free, so the
            // comparison runs only when mismatch telemetry is enabled.
            std::optional<uint64_t> firstHashMismatchIdx;
            std::optional<uint64_t> firstResolveFailIdx;
            auto historicalValueAt = [&](uint64_t idx) -> const DepHashValue * {
                if (!captureMismatchTelemetry)
                    return nullptr;
                if (idx >= group.historicalValues.size())
                    return nullptr;
                return &group.historicalValues[idx];
            };
            auto maybeRecordHashMismatch = [&](uint64_t idx, const DepHashValue & current) {
                if (firstHashMismatchIdx.has_value())
                    return;
                auto * historical = historicalValueAt(idx);
                if (historical && current != *historical)
                    firstHashMismatchIdx = idx;
            };
            auto logEarlyExitSignal = [&](const char * outcome, uint64_t totalDeps) {
                if (!captureMismatchTelemetry || verbosity < lvlDebug)
                    return;
                auto fmtOpt = [](const std::optional<uint64_t> & o) {
                    return o.has_value() ? std::to_string(*o)
                                         : std::string("none");
                };
                debug("eval-trace/recovery: SV early-exit signal "
                      "depKeySet=%u traceId=%u firstHashMismatch=%s "
                      "firstResolveFail=%s totalDeps=%zu outcome=%s",
                      group.depKeySetId.value, group.repTraceId.value,
                      fmtOpt(firstHashMismatchIdx),
                      fmtOpt(firstResolveFailIdx),
                      totalDeps, outcome);
            };
            auto depResolveStart = timerStart();
            // Iterate with CandidateDep.  `checkCache` reads L1 freely — sound
            // under the L1 invariant (every entry equals
            // `resolve(K, current_state)`).  When a key was already resolved
            // by the current trace's Pass 1/2 or by an earlier candidate's
            // compute path, SV hits L1 for O(1) cost.  On miss, `resolveDepHash`
            // falls through to the compute path for every kind — CandidateDep
            // does NOT take the subsumption shortcut (that branch would return
            // the candidate's historical `dep.hash`, not `hash(op(current F))`,
            // making recovery tautologically accept stale candidates).
            // VerifiedHash writes are structurally unreachable here.
                for (uint64_t depIdx = 0; depIdx < repKeys.size(); ++depIdx) {
                    const auto & key = repKeys[depIdx];
                    if (!contributesToTraceHash(key))
                        continue;

                    if (isVolatile(key.kind)) {
                        repComputable = false;
                        abortReason = AbortReason::Volatile;
                        abortKey = key;
                        if (!firstResolveFailIdx.has_value())
                            firstResolveFailIdx = depIdx;
                        break;
                    }

                    if (queryBehavior(key.kind) == QueryBehavior::TraceContext) {
                        auto traceContextHash = ctx.store.resolveTraceContextHash(
                            ctx.ea, key, ctx.registry, ctx.state, ctx.session);
                        if (!traceContextHash) {
                            repComputable = false;
                            abortReason = AbortReason::TraceContextMiss;
                            abortKey = key;
                            if (!firstResolveFailIdx.has_value())
                                firstResolveFailIdx = depIdx;
                            break;
                        }
                        DepHashValue resolvedValue{DepHash{*traceContextHash}};
                        maybeRecordHashMismatch(depIdx, resolvedValue);
                        repDeps.push_back({key, std::move(resolvedValue)});
                        continue;
                    }

                    Dep candidateDep{key, DepHash{}};
                    auto current = ctx.store.resolveCurrentDepHash(
                        ctx.ea, scope.tag(std::move(candidateDep)),
                        ctx.registry, ctx.state, ctx.session);

                    if (!current) {
                        repComputable = false;
                        abortReason = AbortReason::ResolveFailed;
                        abortKey = key;
                        if (!firstResolveFailIdx.has_value())
                            firstResolveFailIdx = depIdx;
                        break;
                    }
                    maybeRecordHashMismatch(depIdx, *current);
                    repDeps.push_back({key, *current});
                }
            auto candidateResolveUs = elapsedUs(depResolveStart);
            nrStructVariantDepResolveUs += candidateResolveUs;
            nrStructVariantDepsResolved += repDeps.size();
            // `totalDeps` is the candidate's full dep-list size (what
            // a naive full scan would iterate).  `repDeps.size()` is
            // the count of *successfully resolved* deps, i.e., the
            // number of iterations before the break.  For the
            // abort-early slice the two differ; for the hash-mismatch
            // slice (no break) they are equal.
            const uint64_t totalDeps = repKeys.size();
            if (!repComputable) {
                // Record abort outcome on the process-scoped telemetry
                // map.  SV runs under ExclusiveTraceStorageAccess so no
                // lock is needed.
                recordSVCandidate(
                    group.depKeySetId.value,
                    /*tried*/true, /*succeeded*/false,
                    /*abortedEarly*/true, /*hashMismatch*/false,
                    repDeps.size(), candidateResolveUs,
                    firstHashMismatchIdx, firstResolveFailIdx, totalDeps);
                logEarlyExitSignal("abortedEarly", totalDeps);
                // Log abort reason + the first dep that caused it.  The
                // outer `verbosity >= lvlDebug` guard is load-bearing:
                // it prevents `queryKindName()` and the expensive
                // `resolveDep()` string construction from running when
                // debug is off.  `debug()` itself re-checks verbosity
                // for `fmt()` but its arguments are evaluated eagerly.
                if (verbosity >= lvlDebug && abortKey) {
                    const char * reasonStr =
                        abortReason == AbortReason::Volatile ? "volatile"
                        : abortReason == AbortReason::TraceContextMiss ? "traceContextMiss"
                        : "resolveFailed";
                    Dep abortDep{*abortKey, DepHash{}};
                    debug("eval-trace/recovery: SV candidate depKeySet=%u traceId=%u "
                          "abort reason=%s kind=%s key='%s'",
                          group.depKeySetId.value, group.repTraceId.value, reasonStr,
                          queryKindName(abortKey->kind),
                          resolveDep(ctx.pools, ctx.store.vocab, abortDep).key);
                }
                continue;
            }

            auto hashStart = timerStart();
            auto candidateTraceHash = ctx.store.computePresortedTraceHash(repDeps);
            nrStructVariantHashUs += elapsedUs(hashStart);

            if (auto * indices = lookupCandidates(candidateTraceHash, lookup)) {
                for (auto historyIdx : *indices) {
                    const auto & entry = history[historyIdx];
                    if (auto acceptance = acceptRecoveredTrace(entry, pathId, triedTraceIds, ctx)) {
                        if (verbosity >= lvlDebug) {
                            debug("eval-trace/recovery: structural variant recovery succeeded for '%s'",
                                ctx.store.vocab.displayPath(pathId));
                        }
                        // Record success outcome.
                        recordSVCandidate(
                            group.depKeySetId.value,
                            /*tried*/true, /*succeeded*/true,
                            /*abortedEarly*/false, /*hashMismatch*/false,
                            repDeps.size(), candidateResolveUs,
                            firstHashMismatchIdx, firstResolveFailIdx, totalDeps);
                        logEarlyExitSignal("win", totalDeps);
                        nrRecoveryStructVariantHits++;
                        nrRecoveryStructVariantTimeUs += elapsedUs(structVariantStart);
                        return std::move(*acceptance).take();
                    }
                }
                // `acceptRecoveredTrace` declined (already tried this
                // traceId).  Count as a hash-mismatch outcome: the lookup
                // surfaced no fresh candidate for this bucket.
            }
            // Record hash-mismatch outcome.
            recordSVCandidate(
                group.depKeySetId.value,
                /*tried*/true, /*succeeded*/false,
                /*abortedEarly*/false, /*hashMismatch*/true,
                repDeps.size(), candidateResolveUs,
                firstHashMismatchIdx, firstResolveFailIdx, totalDeps);
            logEarlyExitSignal("hashMismatch", totalDeps);
            // Hash-mismatch log. Candidate finished the loop but no history
            // entry matched its canonical trace hash.
            if (verbosity >= lvlDebug) {
                debug("eval-trace/recovery: SV candidate depKeySet=%u traceId=%u "
                      "hash-mismatch: candidate=%s (%zu deps resolved)",
                      group.depKeySetId.value, group.repTraceId.value,
                      candidateTraceHash.value.toHex(), repDeps.size());
            }
            }
        }
        nrRecoveryStructVariantTimeUs += elapsedUs(structVariantStart);
        return std::nullopt;
    }
};

// ── Recovery typestate ────────────────────────────────────────────────
//
// RecoveryState<Stage> drives the recovery pipeline through three
// strategies in order, following the same pattern as DepResolution.
// See verification-protocol.hh for the recovery stage tags.

template<typename State>
class RecoveryState : public Linear<RecoveryState<State>> {
    TraceId oldTraceId_;
    AttrPathId pathId_;
    VerifyContext & ctx_;
    std::shared_ptr<const std::vector<Dep>> oldDeps_;

    template<typename> friend class RecoveryState;

    RecoveryState(TraceId oldTraceId, AttrPathId pathId, VerifyContext & ctx,
                  std::shared_ptr<const std::vector<Dep>> oldDeps)
        : oldTraceId_(oldTraceId), pathId_(pathId), ctx_(ctx),
          oldDeps_(std::move(oldDeps)) {}

    // Private transition constructor: advance from a previous state.
    template<typename FromState>
    RecoveryState(RecoveryState<FromState> && from)
        : oldTraceId_(from.oldTraceId_), pathId_(from.pathId_), ctx_(from.ctx_),
          oldDeps_(std::move(from.oldDeps_))
    {
        from.markConsumed();
    }

public:
    static constexpr const char * linearName = "RecoveryState";

    /// Factory: create initial Untried state.
    static RecoveryState<RecoveryUntried> begin(
        TraceId oldTraceId, AttrPathId pathId, VerifyContext & ctx)
        requires std::same_as<State, RecoveryUntried>
    {
        auto oldDeps = ctx.store.loadFullTrace(ctx.ea, oldTraceId);
        return RecoveryState<RecoveryUntried>(oldTraceId, pathId, ctx, std::move(oldDeps));
    }

    /// Strategy 1: GitIdentity-indexed recovery.
    /// Also checks for volatile deps — if present, recovery is aborted
    /// (returns nullopt) because DirectHash and StructVariant cannot
    /// recompute volatile dep hashes.
    [[nodiscard]] std::variant<
        std::optional<SqliteTraceStorage::VerifyResult>,  // terminal: hit or abort
        RecoveryState<RecoveryGitMissed>          // continue
    >
    tryGitIdentity() &&
        requires std::same_as<State, RecoveryUntried>
    {
        this->markConsumed();
        auto gitResult = OriginScopeFactory::enterCurrentTrace(oldTraceId_, [&](const CurrentTraceScope & scope) {
            return VerifyImpl::tryGitIdentityRecovery(scope, *oldDeps_, pathId_, ctx_);
        });
        if (gitResult)
            return std::optional<SqliteTraceStorage::VerifyResult>(*gitResult);

        // Volatile deps make all remaining strategies impossible.
        for (auto & idep : *oldDeps_) {
            if (isVolatile(idep.key.kind)) {
                if (verbosity >= lvlDebug) {
                    debug("eval-trace/recovery: aborting for '%s' -- contains volatile dep",
                        ctx_.store.vocab.displayPath(pathId_));
                }
                nrRecoveryFailures++;
                return std::optional<SqliteTraceStorage::VerifyResult>(std::nullopt);
            }
        }

        RecoveryState<RecoveryGitMissed> next(std::move(*this));
        return std::move(next);
    }

    /// Strategy 2: Direct hash recovery (recompute all dep hashes, hash lookup).
    [[nodiscard]] std::variant<
        std::optional<SqliteTraceStorage::VerifyResult>,  // terminal: hit or exhausted
        RecoveryState<RecoveryDirectMissed>       // continue
    >
    tryDirectHash() &&
        requires std::same_as<State, RecoveryGitMissed>
    {
        this->markConsumed();
        // Direct-hash recovery iterates the CURRENT trace's oldDeps.
        auto dr = OriginScopeFactory::enterCurrentTrace(oldTraceId_, [&](const CurrentTraceScope & scope) {
            return VerifyImpl::tryDirectHashRecovery(
                scope, *oldDeps_, pathId_, ctx_);
        });
        if (dr.result)
            return std::optional<SqliteTraceStorage::VerifyResult>(*dr.result);

        RecoveryState<RecoveryDirectMissed> next(std::move(*this));
        return std::move(next);
    }

    /// Strategy 3: Structural variant recovery (scan history, batch resolve).
    /// Final transition — no next state. Returns hit or nullopt.
    [[nodiscard]] std::optional<SqliteTraceStorage::VerifyResult>
    tryStructVariant() &&
        requires std::same_as<State, RecoveryDirectMissed>
    {
        this->markConsumed();
        if (!ctx_.state.settings.useStructuralRecovery) {
            if (verbosity >= lvlDebug) {
                debug("eval-trace/recovery: SV skipped for '%s' -- "
                      "eval-trace-structural-recovery disabled",
                      ctx_.store.vocab.displayPath(pathId_));
            }
            return std::nullopt;
        }
        auto history = VerifyImpl::loadHistory(pathId_, ctx_);
        auto lookup = VerifyImpl::buildTraceHashLookup(history);
        // SV iterates HISTORICAL candidates' fullDeps — CandidateScope.
        return OriginScopeFactory::enterCandidate([&](const CandidateScope & scope) {
            return VerifyImpl::tryStructuralVariantRecovery(
                scope,
                oldTraceId_, pathId_, history, lookup, ctx_);
        });
    }
};

// ── Trace verification (BSàlC VT check) ─────────────────────────────

bool SqliteTraceStorage::verifyTrace(
    const ExclusiveTraceStorageAccess & ea,
    TraceId traceId,
    const SemanticRegistry & registry,
    EvalState & state,
    VerificationSession & session)
{
    auto & bs = ea.blockingProof();
    if (session.verifiedTraceIds.count(traceId))
        return true;

    // Cycle detection: if this traceId is already being verified further up
    // the call stack (via TraceValueContext/TraceParentSlot deps), treat it
    // as unverified to break the recursion. This prevents a stack overflow
    // when two traces point to each other via TraceValueContext deps.
    if (session.inProgressTraceIds.count(traceId))
        return false;
    session.inProgressTraceIds.insert(traceId);

    struct InProgressGuard {
        VerificationSession & session;
        TraceId traceId;
        ~InProgressGuard() { session.inProgressTraceIds.erase(traceId); }
    } guard{session, traceId};

    session.clearTraceVerifiedFiles(traceId);

    auto vtStart = timerStart();
    auto fullDepsPtr = loadFullTrace(ea, traceId);
    const auto & fullDeps = *fullDepsPtr;

    // Generic canonical-query verification: resolve each dep's current hash,
    // compare against stored, determine validity.  No phases — just ordering
    // constraints within a single pipeline:
    //   1. Batch store-path availability (single RPC)
    //   2. Resolve normal deps, classify results
    //   3. Verify deferred structural/implicit deps, determine coverage
    //   4. Apply outcome
    VerifyContext ctx{ea, bs, *this, pools, registry, state, session};

    // All of verifyTrace operates on the CURRENT trace's fullDeps, so we
    // enter a CurrentTraceScope once and thread it through the phases.
    // The scope type only produces CurrentTraceDeps; a phase function that
    // accidentally tried to construct a CandidateDep would be a compile
    // error (no constructor reachable from this scope).
    return OriginScopeFactory::enterCurrentTrace(traceId, [&](const CurrentTraceScope & scope) {
        VerifyImpl::runStorePathBatch(fullDeps, ctx);
        auto vs = VerifyImpl::runPass1(scope, fullDeps, ctx);

        auto p2 = VerifyImpl::runPass2(scope, fullDeps, vs, ctx);
        bool valid = VerifyImpl::applyOutcome(traceId, fullDeps, vs, p2, ctx);
        nrVerifyTraceTimeUs += elapsedUs(vtStart);
        // Stringify the outcome enum rather than emit a bare integer.
        debug("eval-trace/verify: %s traceId=%u outcome=%s nDeps=%zu",
            valid ? "PASS" : "FAIL", traceId.value,
            verifyOutcomeName(p2.outcome), fullDeps.size());
        // Pass-2 override coverage summary.  Emitted only when an
        // override actually fired (outcome != Valid && != Invalid), so
        // the ordinary "Valid" success path stays quiet.  Uses the
        // retained coverage file sets from `runPass2` so the counts are
        // exact, not approximate.
        if (valid && p2.outcome != VerifyOutcome::Valid && verbosity >= lvlDebug) {
            debug("eval-trace/verify: pass2 outcome=%s failedContentFiles=%zu "
                  "structCoverage=%zu implicitCoverage=%zu uncovered=%zu",
                  verifyOutcomeName(p2.outcome),
                  vs.failedContentFiles.size(),
                  p2.structuralCoveredFiles.size(),
                  p2.implicitCoveredFiles.size(),
                  p2.uncoveredCount);
        }
        return valid;
    });
}

// ── Recovery (BSàlC constructive trace recovery) ─────────────────────

std::optional<SqliteTraceStorage::VerifyResult> SqliteTraceStorage::recovery(
    const ExclusiveTraceStorageAccess & ea,
    TraceId oldTraceId,
    AttrPathId pathId,
    const SemanticRegistry & registry,
    EvalState & state,
    VerificationSession & session)
{
    auto & bs = ea.blockingProof();
    auto recoveryStart = timerStart();
    nrRecoveryAttempts++;

    // No Phase 0 — registry is pre-populated at session open.
    VerifyContext ctx{ea, bs, *this, pools, registry, state, session};

    struct TraceVerifiedFilesGuard {
        VerificationSession & session;
        TraceId traceId;
        ~TraceVerifiedFilesGuard() { session.clearTraceVerifiedFiles(traceId); }
    } traceVerifiedFilesGuard{session, oldTraceId};

    // Drive the RecoveryState typestate to completion.
    // Each transition either resolves (returns result) or advances state.
    // The ordering is enforced at compile time: tryDirectHash requires
    // GitMissed (produced only by tryGitIdentity), tryStructVariant
    // requires DirectMissed (produced only by tryDirectHash).

    auto rs = RecoveryState<RecoveryUntried>::begin(oldTraceId, pathId, ctx);

    // L1: Untried → tryGitIdentity → result | GitMissed | abort
    auto l1 = std::move(rs).tryGitIdentity();
    if (auto * terminal = std::get_if<std::optional<SqliteTraceStorage::VerifyResult>>(&l1)) {
        nrRecoveryTimeUs += elapsedUs(recoveryStart);
        return *terminal;
    }

    // L2: GitMissed → tryDirectHash → result | DirectMissed
    auto l2 = std::get<RecoveryState<RecoveryGitMissed>>(std::move(l1)).tryDirectHash();
    if (auto * terminal = std::get_if<std::optional<SqliteTraceStorage::VerifyResult>>(&l2)) {
        nrRecoveryTimeUs += elapsedUs(recoveryStart);
        return *terminal;
    }

    // L3: DirectMissed → tryStructVariant → result | nullopt
    auto result = std::get<RecoveryState<RecoveryDirectMissed>>(std::move(l2)).tryStructVariant();
    if (!result) {
        if (verbosity >= lvlDebug) {
            debug("eval-trace/recovery: all strategies failed for '%s'", vocab.displayPath(pathId));
        }
        nrRecoveryFailures++;
    }
    nrRecoveryTimeUs += elapsedUs(recoveryStart);
    return result;
}

std::optional<SqliteTraceStorage::VerifyResult> SqliteTraceStorage::verify(
    const ExclusiveTraceStorageAccess & ea,
    AttrPathId pathId,
    const SemanticRegistry & registry,
    EvalState & state,
    VerificationSession & session)
{
    auto & bs = ea.blockingProof();
    auto row = lookupCurrentNode(bs, pathId);
    bool bootstrappedFromHistory = false;

    if (!row) {
        row = lookupLatestHistoryForAttr(ea, pathId);
        if (!row)
            return std::nullopt;
        bootstrappedFromHistory = true;
    }

    auto kh = loadTraceKeysAndHeader(ea, row->traceId);
    if (!kh)
        return std::nullopt;
    if (containsVolatileDep(*kh->keys))
        return std::nullopt;

    std::optional<VerifyResult> result;
    if (verifyTrace(ea, row->traceId, registry, state, session)) {
        if (bootstrappedFromHistory) {
            nrHistoryBootstraps++;
            row = publishStateChange(bs, pathId, row->traceId, row->resultId, /*insertHistory=*/false);
        }
        result.emplace(VerifyResult{
            decodeCachedResult(bs, row->resultId), row->traceId});
    } else {
        result = recovery(ea, row->traceId, pathId, registry, state, session);
    }
    return result;
}

// ── Verifier (async orchestrator) ───────────────────────────────


static bool verifierContainsVolatileDep(const std::vector<Dep::Key> & keys)
{
    return std::any_of(keys.begin(), keys.end(), [](const Dep::Key & key) {
        return isVolatile(key.kind);
    });
}

Verifier::Verifier(
    SqliteTraceStorage & store,
    BlockingThreadPool & blockingPool,
    Config config)
    : store_(store)
    , blockingPool_(blockingPool)
    , config_(config)
{
}

Verifier::~Verifier() = default;

// All blocking store access goes through coroBlock(blockingPool_, ...).
// parallel_group captures EvalState& and session_ by reference — safe
// because session caches are concurrent_flat_map and EvalState is thread-safe.
void Verifier::bindSession(
    const SemanticRegistry & registry,
    EvalState & state)
{
    registry_ = &registry;
    state_ = &state;
    resetVerificationState();
    debug("eval-trace/verifier: bindSession (registry %zu entries, %zu mount points)",
          registry.size(), registry.mountPointCount());
}

void Verifier::resetVerificationState()
{
    session_ = VerificationSession{};
    Verifier::withProof([&](const auto & tok) {
        prefetchPool_.access(tok).clear();
    });
    // Per-DepKeySetId SV telemetry is process-scoped by design (the
    // map lives in counters.cc).  Reset on session open so that
    // `NIX_SHOW_STATS` output describes only the just-started session,
    // not whatever prior session ran in this process.
    clearSVCandidateStats();
}

asio::awaitable<std::optional<SqliteTraceStorage::VerifyResult>>
Verifier::verifyAttr(AttrPathId pathId)
{
    co_return co_await verifyAttrImpl(pathId);
}

void Verifier::submitPrefetchHints(const std::vector<AttrPathId> & pathIds)
{
    Verifier::withProof([&](const auto & tok) {
        auto & pool = prefetchPool_.access(tok);
        for (auto pathId : pathIds) {
            if (pool.size() >= config_.maxPrefetchHints)
                return;
            pool.submit(pathId);
        }
    });
}

asio::awaitable<std::optional<SqliteTraceStorage::VerifyResult>>
Verifier::verifyAttrImpl(AttrPathId pathId)
{
    assert(registry_ && state_ && "verifyAttrImpl called before bindSession");
    auto & registry = *registry_;
    auto & state = *state_;
    bool bootstrappedFromHistory = false;

    // 0. Check prefetch pool.
    {
        auto earlyResult = gdp::Certifier<VerificationAccessTag>::withProof(
            [&](const auto & verifyTok) -> std::optional<std::optional<SqliteTraceStorage::VerifyResult>> {
                auto & prefetchPool = prefetchPool_.access(verifyTok);
                if (auto * token = prefetchPool.lookup(pathId)) {
                    if (token->completed) {
                        auto result = std::move(token->result);
                        prefetchPool.remove(pathId);
                        return result;
                    }
                    prefetchPool.remove(pathId);
                }
                return std::nullopt;
            });
        if (earlyResult)
            co_return *earlyResult;
    }

    // 1. Look up current node from the trace store on the blocking pool.
    auto currentNode = co_await coroBlock(blockingPool_, [&](const gdp::Proof<BlockingTag> & bs) {
        return store_.withExclusiveAccess(bs, [&](const auto & ea) {
            return store_.lookupCurrentNode(ea.blockingProof(), pathId);
        });
    });

    // 2. Verification pipeline.
    // Pre-population happens inside verifyTrace, coupled to the correct
    // traceId. No cross-trace L1 interference possible.
    auto verifyStart = timerStart();
    nrTraceVerifications++;

    if (!currentNode) {
        // Semantic flake sessions intentionally split CurrentTraces across
        // lock-graph changes. When that happens, bootstrap verification from
        // the stable recovery namespace instead of treating the attr as
        // entirely uncached.
        auto latestHistory = co_await coroBlock(blockingPool_, [&](const gdp::Proof<BlockingTag> & bs) {
            return store_.withExclusiveAccess(bs, [&](const auto & ea) {
                return store_.lookupLatestHistoryForAttr(ea, pathId);
            });
        });
        if (!latestHistory) {
            debug("eval-trace/verifier: verifyAttr pathId=%u primary miss + no history — fresh eval",
                  pathId.value);
            co_return std::nullopt;
        }
        currentNode = *latestHistory;
        bootstrappedFromHistory = true;
        debug("eval-trace/verifier: verifyAttr pathId=%u primary miss — bootstrapping from history (traceId=%u)",
              pathId.value, currentNode->traceId.value);
        // NB: nrHistoryBootstraps is incremented AFTER the bootstrapped
        // trace successfully verifies (see branch below), not here — so
        // the counter means "history-served result," not merely
        // "history lookup returned a row." If verifyTrace fails, recovery()
        // runs and `nrRecoveryAttempts` is the correct signal, NOT
        // nrHistoryBootstraps.
    }

    if (!currentNode)
        co_return std::nullopt;

    auto traceId = currentNode->traceId;
    auto kh = co_await coroBlock(blockingPool_, [&](const gdp::Proof<BlockingTag> & bs) {
            return store_.withExclusiveAccess(bs, [&](const auto & ea) {
                return store_.loadTraceKeysAndHeader(ea, traceId);
            });
        });
    if (!kh) {
        debug("eval-trace/verifier: verifyAttr pathId=%u traceId=%u missing trace metadata — fresh eval",
              pathId.value, traceId.value);
        co_return std::nullopt;
    }
    if (verifierContainsVolatileDep(*kh->keys)) {
        debug("eval-trace/verifier: verifyAttr pathId=%u traceId=%u has volatile deps — skipping cache",
              pathId.value, traceId.value);
        co_return std::nullopt;
    }

    // 3b. Full verification pipeline on the blocking pool. No direct blocking
    //     store or filesystem work happens on the io_context workers.
    bool verified = co_await coroBlock(blockingPool_, [&](const gdp::Proof<BlockingTag> & bs) {
        return store_.withExclusiveAccess(bs, [&](const auto & ea) {
            return store_.verifyTrace(ea, traceId, registry, state, session_);
        });
    });

    // 3c. Construct result or run recovery.
    std::optional<SqliteTraceStorage::VerifyResult> result;
    if (verified) {
        nrVerificationsPassed++;
        if (bootstrappedFromHistory) {
            nrHistoryBootstraps++;
            currentNode = co_await coroBlock(blockingPool_, [&](const gdp::Proof<BlockingTag> & bs) {
                return store_.withExclusiveAccess(bs, [&](const auto & ea) {
                    return store_.publishStateChange(
                        ea.blockingProof(), pathId, currentNode->traceId, currentNode->resultId,
                        /*insertHistory=*/false);
                });
            });
        }
        auto cachedResult = co_await coroBlock(blockingPool_, [&](const gdp::Proof<BlockingTag> & bs) {
            return store_.withExclusiveAccess(bs, [&](const auto & ea) {
                return store_.decodeCachedResult(ea.blockingProof(), currentNode->resultId);
            });
        });
        result.emplace(SqliteTraceStorage::VerifyResult{std::move(cachedResult), traceId});
    } else {
        result = co_await coroBlock(blockingPool_, [&](const gdp::Proof<BlockingTag> & bs) {
            return store_.withExclusiveAccess(bs, [&](const auto & ea) {
                return store_.recovery(ea, traceId, pathId, registry, state, session_);
            });
        });
    }

    nrVerifyTimeUs += elapsedUs(verifyStart);
    co_return result;
}


} // namespace nix::eval_trace
