#pragma once

#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/eval-trace/strand-local.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <memory>
#include <optional>

namespace nix::eval_trace {

class SemanticRegistry;
struct ParseCaches;

struct VerificationSession {
    /// Friend access for file-static VerifyImpl in
    /// verifier.cc. Needs access to trace-scoped
    /// subsumption marks.
    friend struct VerifyImpl;
    friend struct SqliteTraceStorage;

    using VerifiedTraceIds = boost::unordered_flat_set<TraceId, TraceId::Hash>;
    using InProgressTraceIds = boost::unordered_flat_set<TraceId, TraceId::Hash>;
    using CurrentDepHashes = boost::unordered_flat_map<Dep::Key, std::optional<DepHashValue>, Dep::Key::Hash>;
    struct TraceContextMemoEntry {
        NodeStamp nodeStamp{};
        std::optional<EvalTraceHash> traceHash;
    };
    using TraceContextMemo =
        boost::unordered_flat_map<AttrPathId, TraceContextMemoEntry, AttrPathId::Hash>;

    VerificationSession() = default;
    VerificationSession(VerificationSession &&) noexcept = default;
    VerificationSession & operator=(VerificationSession &&) noexcept = default;
    ~VerificationSession() = default;

    VerificationSession(const VerificationSession &) = delete;
    VerificationSession & operator=(const VerificationSession &) = delete;

    // ── L1 dep-hash cache (encapsulated) ─────────────────────────────
    //
    // L1 contains dep hashes from two provenances:
    //   - ComputedHash: freshly computed from current filesystem state.
    //     Pure function of current state; any caller may write these.
    //   - VerifiedHash: trace-scoped subsumption shortcut — this trace's
    //     Content dep for the file verified unchanged, so this trace's stored
    //     structural hash equals the current-state resolution. Writing this
    //     is sealed behind VerifiedSubsumption, which is only mintable inside
    //     resolveDepHash's CurrentTrace branch.
    //
    // Invariant: for every key K present in L1, the value equals
    //   resolve(K, current_state).
    //
    // This holds because:
    //   - ComputedHash writes are computed directly from state.
    //   - VerifiedHash writes require the VerifiedSubsumption capability,
    //     whose factory is SFINAE-restricted to CurrentTrace origin; the
    //     subsumption precondition (isFileVerified(traceId, F)) implies this
    //     trace's stored hash for any key on F equals resolve(K, current).
    //
    // HistoricalCandidate origins cannot mint VerifiedSubsumption, so SV
    // recovery cannot inject historical hashes into L1.
    //
    // Reads are unconditional — the invariant makes every L1 entry safe to
    // return to any caller.

    /// Read: lookup a cached dep hash. Returns null if not in cache.
    /// The pointed-to value may itself be nullopt (dep was computed
    /// but resolveDepHash returned nullopt, e.g. volatile deps).
    const std::optional<DepHashValue> * lookupDepHash(const Dep::Key & key) const
    {
        auto it = currentDepHashes_.find(key);
        return it != currentDepHashes_.end() ? &it->second : nullptr;
    }

    /// Size of the L1 cache (for test assertions only).
    size_t depHashCount() const { return currentDepHashes_.size(); }

    VerifiedTraceIds verifiedTraceIds;
    /// TraceIds currently being verified. Used to detect and break cycles
    /// introduced by TraceValueContext/TraceParentSlot deps that point back
    /// to a trace already in the verification call stack. When a cycle is
    /// detected in verifyTrace, the recursive call returns false immediately
    /// rather than recursing until stack overflow.
    InProgressTraceIds inProgressTraceIds;
    TraceContextMemo traceContextMemo;
    std::shared_ptr<ParseCaches> parseCaches;

    /// Batched StorePathExistence results: keyId → valid.
    /// Access is sequential: orchestrator coroutine accesses before/after
    /// coroBlock calls, never concurrently with blocking threads.
    boost::unordered_flat_map<StorePathAvailabilityDepKeyId, bool, StorePathAvailabilityDepKeyId::Hash> storePathValid;

    /// Cached GitIdentity hash per repo root id.
    /// Value is CurrentGitIdentityHash (phantom-typed) to prevent BUG-1:
    /// passing a cached current hash where a stored hash is expected is a
    /// compile error. StoredGitIdentityHash is the counterpart type for
    /// hashes extracted from trace deps at recording time.  This cache is
    /// repo-state caching — purely a function of the current filesystem
    /// state, independent of any trace.  Shared across all traces in the
    /// session.  Keyed by `RepoRootId` (not `GitRepoRoot` / `CanonPath`)
    /// to avoid per-lookup string allocation; `resolveRepoRoot(id)` is
    /// invoked only once per repo when computing the git hash from disk.
    boost::unordered_flat_map<RepoRootId, std::optional<CurrentGitIdentityHash>, RepoRootId::Hash> gitIdentityCache;

    // ── Subsumption ──────────────────────────────────────────────────
    //
    // Content/Directory deps are coarse "root" deps. When their hash
    // matches the recorded value for a trace, finer-grained structural deps
    // for the same file in that same trace are subsumed — their stored hash
    // is the current hash.  The trace id is part of the subsumption key; a
    // pass in trace A never authorizes returning a stored hash from trace B.
    //
    // These methods are the ONLY interface to the subsumption state.
    // resolveDepHash checks isFileVerified(traceId, key) before computing;
    // callers that resolve CurrentTrace deps by any path get subsumption
    // automatically.

    /// Check if a file's Content dep was verified unchanged.  Returns true
    /// only for subsumable structured keys (StructuredProjection,
    /// ImplicitStructure on a single source file).  The sole caller is
    /// `canSubsumeShortcut` in `dep-resolution-service.cc`, which already
    /// gates on `queryBehavior(kind) ∈ {Structural, ImplicitStructural}` —
    /// every such kind is structured.  DirSet-aggregated structured deps
    /// span multiple directories and are not subsumable.
    bool isFileVerified(TraceId traceId, const Dep::Key & key) const
    {
        if (!key.isStructured())
            return false;
        if (key.dirSetHashId)
            return false;
        auto traceIt = verifiedContentFilesByTrace_.find(traceId);
        if (traceIt == verifiedContentFilesByTrace_.end())
            return false;
        return traceIt->second.count(contentFileKey(key));
    }

private:
    // resolveDepHash is the ONLY function that writes to L1.
    // It creates ComputedHash on the compute path and VerifiedHash on
    // the subsumption path (CurrentTrace origin only).
    template<typename T>
    friend std::optional<DepHashValue> resolveDepHash(
        EvalState &, VerificationSession &, const T &,
        const SemanticRegistry &,
        const InterningPools &, ParseCaches &,
        const StrandToken<FileStrandTag> &);

    /// Write a freshly-computed hash to L1.  Any caller may write these —
    /// ComputedHash is a pure function of current filesystem state.  A
    /// nullopt payload caches a negative result (e.g. volatile kinds).
    void cacheComputedHash(const Dep::Key & key, std::optional<ComputedHash> hash)
    {
        currentDepHashes_[key] = hash ? std::optional{hash->value} : std::nullopt;
    }

    /// Write a subsumed hash to L1.  Requires the VerifiedSubsumption
    /// capability — only mintable from a CurrentTraceDep in resolveDepHash's
    /// subsumption branch.  HistoricalCandidate origins cannot mint the
    /// capability, making historical-hash poisoning a compile error.
    void cacheVerifiedHash(const Dep::Key & key, VerifiedHash hash,
                           VerifiedSubsumption)
    {
        currentDepHashes_[key] = std::optional{hash.value};
    }

    CurrentDepHashes currentDepHashes_;

    using VerifiedContentFiles = boost::unordered_flat_set<uint64_t>;

    /// Files whose Content/Directory dep hash matched the recorded value,
    /// keyed by trace id.  This avoids the old session-wide file bit that
    /// could let a verified file in one trace subsume a stale structural dep
    /// from another trace.
    boost::unordered_flat_map<TraceId, VerifiedContentFiles, TraceId::Hash> verifiedContentFilesByTrace_;

    void clearTraceVerifiedFiles(TraceId traceId)
    {
        verifiedContentFilesByTrace_.erase(traceId);
    }

    void markFileIdentityVerified(TraceId traceId, DepSourceId sourceId, StringId pathId)
    {
        verifiedContentFilesByTrace_[traceId].insert(contentFileKey(sourceId, pathId));
    }

    /// Pack (sourceId, file-path-id) into a single uint64_t key for
    /// verifiedContentFilesByTrace_. For structured deps, the file path is in
    /// filePathId. For non-structured deps, we explicitly erase the typed
    /// dep-key ID only for file-content kinds. Both ID types share the same
    /// StringInternTable, so the underlying values match for the same path
    /// string.
    static uint64_t contentFileKey(const Dep::Key & key)
    {
        assert(!key.isTraceContext());
        assert(key.isStructured()
            || key.kind == CanonicalQueryKind::FileBytes
            || key.kind == CanonicalQueryKind::DirectoryEntries
            || key.kind == CanonicalQueryKind::ExistenceCheck
            || key.kind == CanonicalQueryKind::RawBytes
            || key.kind == CanonicalQueryKind::NarIdentity);
        uint32_t pathValue = key.isStructured()
            ? key.filePathId.value
            : key.simpleKeyId().value;
        return contentFileKey(key.sourceId, StringId(pathValue));
    }

    static uint64_t contentFileKey(DepSourceId sourceId, StringId pathId)
    {
        return (uint64_t(sourceId.value) << 32) | pathId.value;
    }
};

} // namespace nix::eval_trace
