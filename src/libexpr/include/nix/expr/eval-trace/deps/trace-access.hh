#pragma once

#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/trace-frame.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace nix {

struct Value;
struct DerivedStorePathDepKey;
struct StorePathAvailabilityDepKey;
struct RuntimeFetchIdentityDepKey;

namespace eval_trace {

struct TraceAccess
{
private:
    InterningPools * pools_ = nullptr;
    TraceFrame * scope_ = nullptr;
    /// Non-null when recording goes through the fiber's DepRecordingContext.
    DepRecordingContext * depCtx_ = nullptr;

    TraceAccess(InterningPools * pools, TraceFrame * scope, DepRecordingContext * depCtx = nullptr)
        : pools_(pools)
        , scope_(scope)
        , depCtx_(depCtx)
    {
    }

public:

    static std::optional<TraceAccess> current();
    static TraceAccess forRecording(InterningPools & pools, DepRecordingContext & depCtx)
    {
        return TraceAccess(&pools, nullptr, &depCtx);
    }

    bool isActive() const { return pools_ != nullptr; }
    explicit operator bool() const { return isActive(); }

    InterningPools & tracingPools() const { return *pools_; }
    DepRecordingContext & depRecordingContext() const
    {
        assert(depCtx_ && "TraceAccess without DepRecordingContext");
        return *depCtx_;
    }

    void record(CanonicalQueryKind type, const DepSource & source, const SimpleDepKeyAtom & key, const DepHashValue & hash, RepoRootId governingRepoId = {}) const
    {
        assert(depCtx_ && "TraceAccess::record without DepRecordingContext");
        depCtx_->record(type, source, key, hash, governingRepoId);
    }
    void record(const DepSource & source, const DerivedStorePathDepKey & key, const DepHashValue & hash, RepoRootId governingRepoId = {}) const
    {
        assert(depCtx_ && "TraceAccess::record without DepRecordingContext");
        depCtx_->record(source, key, hash, governingRepoId);
    }
    void record(const DepSource & source, const StorePathAvailabilityDepKey & key, const DepHashValue & hash) const
    {
        assert(depCtx_ && "TraceAccess::record without DepRecordingContext");
        depCtx_->record(source, key, hash);
    }
    void record(const DepSource & source, const RuntimeFetchIdentityDepKey & key, const DepHashValue & hash) const
    {
        assert(depCtx_ && "TraceAccess::record without DepRecordingContext");
        depCtx_->record(source, key, hash);
    }
    void record(const Dep & dep) const
    {
        assert(depCtx_ && "TraceAccess::record without DepRecordingContext");
        depCtx_->record(dep);
    }
    void recordStructured(
        const CompactDepComponents & c,
        const DepHashValue & hash,
        CanonicalQueryKind depType = CanonicalQueryKind::StructuredProjection) const
    {
        recordStructuredDep(*pools_, c, hash, depType);
    }

    /// Returns true if the current scope's `seenDeps` already contains
    /// `key`. Allows callers to skip expensive per-access work (file
    /// hash computation, registry lookup) when the dep would be
    /// deduplicated anyway. Returns false when no DepRecordingContext
    /// is active.
    bool scopeContainsDepKey(const Dep::Key & key) const
    {
        return depCtx_ ? depCtx_->scopeContainsDepKey(key) : false;
    }

    bool replayMemoizedRange(const ::nix::Value & value, const DepRange & range) const
    {
        // Replay into the current scope of the DepRecordingContext.
        // MemoReplayStore ranges index into the context's epoch log, and
        // the scope epochLogStartIndex must match — only deps recorded
        // before the scope opened are eligible for replay into ownDeps.
        assert(depCtx_ && "TraceAccess::replayMemoizedRange without DepRecordingContext");
        auto * scope = depCtx_->currentScope();
        if (!scope)
            return false;
        if (range.start >= scope->epochLogStartIndex)
            return false;
        if (!scope->replayedValues.insert(&value).second)
            return false;
        scope->ownDeps.insert(
            scope->ownDeps.end(),
            range.deps->begin() + range.start,
            range.deps->begin() + range.end);
        return true;
    }

    void registerPrecomputedKeys(uint32_t originOffset, PrecomputedKeysInfo info) const;
    void erasePrecomputedKeys(uint32_t originOffset) const;

    ProvenanceRef allocateProvenance(
        DepSourceId sourceId,
        FilePathId filePathId,
        DataPathId dataPathId,
        StructuredFormat format) const;

    void registerTracedContainer(const ::nix::Value * key, const TracedContainerProvenance * prov) const;
    void unregisterTracedContainer(const ::nix::Value * key) const;

    const TracedContainerProvenance * lookupTracedContainer(const ::nix::Value * key) const;

    bool markBindingsScanned(const Bindings * bindings) const;

    bool isBindingsScannedForTest(const Bindings * bindings) const;

    const PrecomputedKeysInfo * lookupPrecomputedKeys(uint32_t originOffset) const;

    const std::vector<IntersectOriginInfo> * lookupIntersectOrigins(const Bindings * bindings) const;

    const std::vector<IntersectOriginInfo> * cacheIntersectOriginsIfScoped(
        const Bindings * bindings,
        std::vector<IntersectOriginInfo> origins) const;

    std::optional<std::string_view> lookupDirSetHash(const DirSetKey & key) const;

    void cacheDirSetHash(DirSetKey key, std::string hash) const;

    size_t dirSetHashCacheSizeForTest() const;

    size_t intersectOriginsCacheSizeForTest() const;
};

} // namespace eval_trace
} // namespace nix
