#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/deps/dep-recording-context.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/nix-binding.hh"
#include "nix/expr/eval-trace/deps/trace-frame.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/eval-context.hh"
#include "nix/expr/attr-set.hh"
#include "nix/expr/value.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "../fiber/fiber-scheduler.hh"

#include <filesystem>

namespace nix {

// ═══════════════════════════════════════════════════════════════════════
// Structured path helpers
// ═══════════════════════════════════════════════════════════════════════

RepoRootId InterningPools::internGoverningRepo(std::string_view absPath)
{
    if (absPath.empty() || absPath[0] != '/')
        return RepoRootId{};

    // Canonicalize once up front so `/tmp/...` and `/private/tmp/...`
    // (macOS) collapse to the same cache entry.  Fall back to the raw
    // string on failure (e.g. path doesn't exist yet, permission) —
    // preserves the old behavior for the not-yet-extant dep case.
    std::error_code ec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(
        std::filesystem::path{std::string(absPath)}, ec);
    std::string canonStr = ec ? std::string(absPath) : canon.string();

    if (auto it = governingRepoCache_.find(canonStr);
        it != governingRepoCache_.end())
        return it->second;

    // Start from the DIRECTORY that could host `.git`.  For an extant
    // directory, that's `canonStr` itself; for files (or not-yet-existing
    // paths), walk up one.
    std::filesystem::path dir;
    {
        std::filesystem::path probe{canonStr};
        auto stat = std::filesystem::status(probe, ec);
        dir = (!ec && std::filesystem::is_directory(stat))
            ? probe
            : probe.parent_path();
    }

    // Walk ancestors searching for `.git`.  Record the chain so we can
    // populate the cache for every ancestor in one pass (amortized O(1)
    // per unique ancestor — siblings hit the first common ancestor in
    // cache rather than re-walking).
    std::vector<std::filesystem::path> chain;
    RepoRootId repoId{};
    while (true) {
        // Fast cache hit: an ancestor we've already resolved.  Copy its
        // answer and stop walking.
        if (auto it = governingRepoCache_.find(dir.string());
            it != governingRepoCache_.end()) {
            repoId = it->second;
            break;
        }
        chain.push_back(dir);
        if (std::filesystem::exists(dir / ".git", ec)) {
            repoId = strings_.intern<RepoRootId>(dir.string());
            break;
        }
        auto parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = std::move(parent);
    }

    // Populate cache for every ancestor we visited (including the
    // original canonStr path) so future siblings amortize to O(1).
    for (auto & ancestor : chain)
        governingRepoCache_.emplace(ancestor.string(), repoId);
    governingRepoCache_.emplace(std::move(canonStr), repoId);
    return repoId;
}

StructuredPath resolveStructuredPath(const InterningPools & pools, DataPathId nodeId)
{
    StructuredPath path;
    for (auto & node : pools.dataPathPool.collectPath(nodeId)) {
        if (node.arrayIndex >= 0)
            path.push_back(StructuredPathComponent::makeIndex(node.arrayIndex));
        else
            path.push_back(StructuredPathComponent::makeKey(node.component));
    }
    return path;
}

DataPathId internStructuredPath(InterningPools & pools, const StructuredPath & path)
{
    auto id = pools.dataPathPool.root();
    for (auto & component : path) {
        if (component.isIndex())
            id = pools.dataPathPool.internArrayChild(id, component.index);
        else
            id = pools.dataPathPool.internChild(id, component.key);
    }
    return id;
}

Pos::ProvenanceRef allocateProvenanceRef(
    InterningPools & pools, DepSourceId srcId, FilePathId fpId, DataPathId dpId, StructuredFormat format)
{
    return Pos::ProvenanceRef{pools.provenanceTable.allocate(srcId, fpId, dpId, format)};
}

const ProvenanceRecord & resolveProvenanceRef(InterningPools & pools, const Pos::ProvenanceRef & ref)
{
    return pools.provenanceTable.resolve(ref.id);
}

// ═══════════════════════════════════════════════════════════════════════
// TraceFrame / TraceAccess
// ═══════════════════════════════════════════════════════════════════════

static thread_local TraceFrame * currentTraceFrame = nullptr;

TraceFrame * TraceFrame::swapCurrent(TraceFrame * next)
{
    auto * previous = currentTraceFrame;
    currentTraceFrame = next;
    return previous;
}

TraceFrame * TraceFrame::currentForAccess()
{
    return currentTraceFrame;
}

namespace eval_trace {

std::optional<TraceAccess> TraceAccess::current()
{
    // Fiber-local context (production eval path).
    if (auto * fiberCtx = currentFiberDepCtx()) {
        if (fiberCtx->isActive())
            return TraceAccess(&fiberCtx->pools, TraceFrame::currentForAccess(), fiberCtx);
    }

    // Standalone context (tests, non-fiber paths).
    if (auto * standaloneCtx = currentStandaloneDepCtx()) {
        if (standaloneCtx->isActive())
            return TraceAccess(&standaloneCtx->pools, TraceFrame::currentForAccess(), standaloneCtx);
    }

    return std::nullopt;
}

} // namespace eval_trace

// ═══════════════════════════════════════════════════════════════════════
// TraceFrame registry implementations
// ═══════════════════════════════════════════════════════════════════════

ProvenanceRef TraceFrame::allocateProvenance(
    DepSourceId sourceId, FilePathId filePathId, DataPathId dataPathId, StructuredFormat format)
{
    return containerProvenanceRegistry.allocate(sourceId, filePathId, dataPathId, format);
}

ProvenanceRef TraceFrame::ContainerProvenanceRegistry::allocate(
    DepSourceId sourceId, FilePathId filePathId, DataPathId dataPathId, StructuredFormat format)
{
    provenancePool.emplace_back(TracedContainerProvenance{sourceId, filePathId, dataPathId, format});
    return &provenancePool.back();
}

std::optional<TraceFrame::ContainerRef> TraceFrame::ContainerProvenanceRegistry::makeRef(const Value * key)
{
    if (!key || !key->isValid())
        return std::nullopt;

    if (key->type() == nAttrs) {
        auto * attrs = key->attrs();
        if (attrs == &Bindings::emptyBindings)
            return std::nullopt;
        return TraceFrame::ContainerRef{
            .kind = TraceFrame::ContainerRefKind::Attrs,
            .value = attrs,
        };
    }

    if (key->type() != nList)
        return std::nullopt;

    auto * storage = key->listStorageIdentity();
    if (!storage)
        return std::nullopt;

    return TraceFrame::ContainerRef{
        .kind = TraceFrame::ContainerRefKind::List,
        .value = storage,
    };
}

void TraceFrame::ContainerProvenanceRegistry::registerContainer(
    const Value * key, const TracedContainerProvenance * prov)
{
    auto ref = makeRef(key);
    if (!ref) {
        if (!key || !key->isValid())
            throw Error("internal error: cannot register eval-trace container provenance for an invalid value");
        if (key->type() == nAttrs && key->attrs() == &Bindings::emptyBindings)
            throw Error(
                "internal error: cannot register eval-trace attrset provenance on shared emptyBindings; "
                "allocate fresh empty bindings for traced empty attrsets");
        if (key->type() == nList && !key->listStorageIdentity())
            throw Error(
                "internal error: cannot register eval-trace list provenance without heap-backed list storage");
        throw Error("internal error: cannot register eval-trace container provenance for a non-container value");
    }
    provenanceByContainer.insert_or_assign(std::move(*ref), prov);
}

void TraceFrame::ContainerProvenanceRegistry::unregisterContainer(const Value * key)
{
    auto ref = makeRef(key);
    if (ref)
        provenanceByContainer.erase(*ref);
}

const TracedContainerProvenance * TraceFrame::ContainerProvenanceRegistry::lookupContainer(
    const Value * key) const
{
    auto ref = makeRef(key);
    if (!ref)
        return nullptr;
    auto it = provenanceByContainer.find(*ref);
    return it != provenanceByContainer.end() ? it->second : nullptr;
}

bool TraceFrame::ScannedBindingsRegistry::markScanned(const Bindings * bindings)
{
    return scanned.insert(BindingsRef{bindings}).second;
}

bool TraceFrame::ScannedBindingsRegistry::contains(const Bindings * bindings) const
{
    return scanned.find(BindingsRef{bindings}) != scanned.end();
}

void TraceFrame::PrecomputedKeysRegistry::registerKeys(uint32_t originOffset, PrecomputedKeysInfo info)
{
    byOriginOffset.insert_or_assign(originOffset, std::move(info));
}

void TraceFrame::PrecomputedKeysRegistry::eraseKeys(uint32_t originOffset)
{
    byOriginOffset.erase(originOffset);
}

const PrecomputedKeysInfo * TraceFrame::PrecomputedKeysRegistry::lookup(uint32_t originOffset) const
{
    auto it = byOriginOffset.find(originOffset);
    return it != byOriginOffset.end() ? &it->second : nullptr;
}

const std::vector<IntersectOriginInfo> * TraceFrame::IntersectOriginsRegistry::lookup(
    const Bindings * bindings) const
{
    auto it = byBindings.find(BindingsRef{bindings});
    return it != byBindings.end() ? &it->second : nullptr;
}

const std::vector<IntersectOriginInfo> * TraceFrame::IntersectOriginsRegistry::cache(
    const Bindings * bindings, std::vector<IntersectOriginInfo> origins)
{
    auto [it, inserted] = byBindings.try_emplace(BindingsRef{bindings}, std::move(origins));
    assert(inserted);
    return &it->second;
}

size_t TraceFrame::IntersectOriginsRegistry::size() const { return byBindings.size(); }

std::optional<std::string_view> TraceFrame::DirSetHashRegistry::lookup(const DirSetKey & key) const
{
    auto it = byDirSet.find(key);
    return it != byDirSet.end() ? std::optional<std::string_view>(it->second) : std::nullopt;
}

void TraceFrame::DirSetHashRegistry::cache(DirSetKey key, std::string hash)
{
    byDirSet.emplace(std::move(key), std::move(hash));
}

size_t TraceFrame::DirSetHashRegistry::size() const { return byDirSet.size(); }

void TraceFrame::registerPrecomputedKeys(uint32_t originOffset, PrecomputedKeysInfo info) { precomputedKeysRegistry.registerKeys(originOffset, std::move(info)); }
void TraceFrame::erasePrecomputedKeys(uint32_t originOffset) { precomputedKeysRegistry.eraseKeys(originOffset); }
void TraceFrame::registerTracedContainer(const Value * key, const TracedContainerProvenance * prov) { containerProvenanceRegistry.registerContainer(key, prov); }
void TraceFrame::unregisterTracedContainer(const Value * key) { containerProvenanceRegistry.unregisterContainer(key); }
const TracedContainerProvenance * TraceFrame::lookupTracedContainer(const Value * key) const { return containerProvenanceRegistry.lookupContainer(key); }
bool TraceFrame::markBindingsScanned(const Bindings * bindings) { return scannedBindingsRegistry.markScanned(bindings); }
bool TraceFrame::isBindingsScannedForTest(const Bindings * bindings) const { return scannedBindingsRegistry.contains(bindings); }
const PrecomputedKeysInfo * TraceFrame::lookupPrecomputedKeys(uint32_t originOffset) const { return precomputedKeysRegistry.lookup(originOffset); }
const std::vector<IntersectOriginInfo> * TraceFrame::lookupIntersectOrigins(const Bindings * bindings) const { return intersectOriginsRegistry.lookup(bindings); }
const std::vector<IntersectOriginInfo> * TraceFrame::cacheIntersectOriginsIfScoped(const Bindings * bindings, std::vector<IntersectOriginInfo> origins) { return intersectOriginsRegistry.cache(bindings, std::move(origins)); }
std::optional<std::string_view> TraceFrame::lookupDirSetHash(const DirSetKey & key) const { return dirSetHashRegistry.lookup(key); }
void TraceFrame::cacheDirSetHash(DirSetKey key, std::string hash) { dirSetHashRegistry.cache(std::move(key), std::move(hash)); }
size_t TraceFrame::dirSetHashCacheSizeForTest() const { return dirSetHashRegistry.size(); }
size_t TraceFrame::intersectOriginsCacheSizeForTest() const { return intersectOriginsRegistry.size(); }

// ═══════════════════════════════════════════════════════════════════════
// TraceAccess forwarding methods
// ═══════════════════════════════════════════════════════════════════════

namespace eval_trace {

void TraceAccess::registerPrecomputedKeys(uint32_t originOffset, PrecomputedKeysInfo info) const { if (scope_) scope_->registerPrecomputedKeys(originOffset, std::move(info)); }
void TraceAccess::erasePrecomputedKeys(uint32_t originOffset) const { if (scope_) scope_->erasePrecomputedKeys(originOffset); }
ProvenanceRef TraceAccess::allocateProvenance(DepSourceId sourceId, FilePathId filePathId, DataPathId dataPathId, StructuredFormat format) const { return scope_ ? scope_->allocateProvenance(sourceId, filePathId, dataPathId, format) : nullptr; }
void TraceAccess::registerTracedContainer(const ::nix::Value * key, const TracedContainerProvenance * prov) const { if (scope_) scope_->registerTracedContainer(key, prov); }
void TraceAccess::unregisterTracedContainer(const ::nix::Value * key) const { if (scope_) scope_->unregisterTracedContainer(key); }
const TracedContainerProvenance * TraceAccess::lookupTracedContainer(const ::nix::Value * key) const { return scope_ ? scope_->lookupTracedContainer(key) : nullptr; }
bool TraceAccess::markBindingsScanned(const Bindings * bindings) const { return scope_ ? scope_->markBindingsScanned(bindings) : false; }
bool TraceAccess::isBindingsScannedForTest(const Bindings * bindings) const { return scope_ && scope_->isBindingsScannedForTest(bindings); }
const PrecomputedKeysInfo * TraceAccess::lookupPrecomputedKeys(uint32_t originOffset) const { return scope_ ? scope_->lookupPrecomputedKeys(originOffset) : nullptr; }
const std::vector<IntersectOriginInfo> * TraceAccess::lookupIntersectOrigins(const Bindings * bindings) const { return scope_ ? scope_->lookupIntersectOrigins(bindings) : nullptr; }
const std::vector<IntersectOriginInfo> * TraceAccess::cacheIntersectOriginsIfScoped(const Bindings * bindings, std::vector<IntersectOriginInfo> origins) const { return scope_ ? scope_->cacheIntersectOriginsIfScoped(bindings, std::move(origins)) : nullptr; }
std::optional<std::string_view> TraceAccess::lookupDirSetHash(const DirSetKey & key) const { return scope_ ? scope_->lookupDirSetHash(key) : std::nullopt; }
void TraceAccess::cacheDirSetHash(DirSetKey key, std::string hash) const { if (scope_) scope_->cacheDirSetHash(std::move(key), std::move(hash)); }
size_t TraceAccess::dirSetHashCacheSizeForTest() const { return scope_ ? scope_->dirSetHashCacheSizeForTest() : 0; }
size_t TraceAccess::intersectOriginsCacheSizeForTest() const { return scope_ ? scope_->intersectOriginsCacheSizeForTest() : 0; }

} // namespace eval_trace

// ═══════════════════════════════════════════════════════════════════════
// TraceFrameScope — RAII guard for TraceFrame thread_local
// ═══════════════════════════════════════════════════════════════════════

struct TraceFrameScope {
    TraceFrame * previous;
    TraceFrame rootScope;

    TraceFrameScope()
        : previous(TraceFrame::swapCurrent(&rootScope))
    {
    }

    ~TraceFrameScope()
    {
        TraceFrame::swapCurrent(previous);
    }

    TraceFrameScope(const TraceFrameScope &) = delete;
    TraceFrameScope & operator=(const TraceFrameScope &) = delete;
};

// ═══════════════════════════════════════════════════════════════════════
// recordStructuredDep
// ═══════════════════════════════════════════════════════════════════════

[[gnu::cold]] bool recordStructuredDep(
    InterningPools & pools,
    const CompactDepComponents & c,
    const DepHashValue & hash,
    CanonicalQueryKind depType)
{
    if (c.format == StructuredFormat::Directory && c.suffix == ShapeSuffix::Len)
        throw Error(
            "internal error: eval-trace attempted to record a directory #len dependency; "
            "directory shape must be represented with #keys or #has dependencies");

    auto key = Dep::Key::makeStructured(
        depType,
        c.sourceId, c.filePathId, c.format,
        c.dataPathId, c.suffix, c.hasKeyId, c.dirSetHashId);
    // Governing-repo attachment for structured file-content deps.
    // filePathId is a file path atom; DirSet-aggregated deps
    // (dirSetHashId != 0) span multiple paths and are not covered by
    // git-identity skip, so leave governingRepoId == 0.
    if (c.dirSetHashId.value == 0)
        key.governingRepoId = pools.internGoverningRepo(pools.resolve(c.filePathId));
    Dep dep{
        std::move(key),
        hash,
    };

    // Fiber context (production eval path).
    if (auto * fiberCtx = eval_trace::currentFiberDepCtx()) {
        fiberCtx->record(dep);
        return true;
    }

    // Standalone context (tests, non-fiber evaluation).
    if (auto * standaloneCtx = eval_trace::currentStandaloneDepCtx()) {
        standaloneCtx->record(dep);
        return true;
    }

    // No recording context active. This is a legitimate path: eval-trace
    // is enabled but the current eval didn't enter through TracedExpr::eval
    // (e.g., manual generation, internal evals). Builtins fire dep recording
    // hooks but no DepRecordingContext exists to receive them. Drop the dep —
    // no consumer exists.
    //
    // Non-zero under normal operation — see counter doc in counters.hh.
    eval_trace::nrDepRecordNoActiveContext++;
    return false;
}

// ═══════════════════════════════════════════════════════════════════════
// DepCaptureScope — out-of-line (needs TraceFrameScope)
// ═══════════════════════════════════════════════════════════════════════

static thread_local std::optional<TraceFrameScope> standaloneTraceFrame;

eval_trace::DepCaptureScope::DepCaptureScope(InterningPools & pools, const eval_trace::SemanticRegistry & reg)
    : registry(&reg)
{
    ctx = eval_trace::currentFiberDepCtx();
    if (!ctx) {
        ctx = eval_trace::currentStandaloneDepCtx();
        if (!ctx) {
            ownedCtx = std::make_unique<DepRecordingContext>(pools, fallbackEpochLog_);
            ctx = ownedCtx.get();
            standaloneGuard.emplace(*ownedCtx);
            isRoot = true;
            if (!TraceFrame::currentForAccess()) {
                standaloneTraceFrame.emplace();
                ownsTraceFrame = true;
            }
        }
    }
    Certifier::withProof([&](const auto & proof) { ctx->pushScope(proof); });
    // Set the registry on the scope so recordDep can access it.
    if (auto * scope = ctx->currentScope())
        scope->registry = registry;
}

eval_trace::DepCaptureScope::DepCaptureScope(InterningPools & pools)
{
    ctx = eval_trace::currentFiberDepCtx();
    if (!ctx) {
        ctx = eval_trace::currentStandaloneDepCtx();
        if (!ctx) {
            ownedCtx = std::make_unique<DepRecordingContext>(pools, fallbackEpochLog_);
            ctx = ownedCtx.get();
            standaloneGuard.emplace(*ownedCtx);
            isRoot = true;
            if (!TraceFrame::currentForAccess()) {
                standaloneTraceFrame.emplace();
                ownsTraceFrame = true;
            }
        }
    }
    Certifier::withProof([&](const auto & proof) { ctx->pushScope(proof); });
}

eval_trace::DepCaptureScope::~DepCaptureScope()
{
    if (!scopePopped) {
        // Exception path: the epoch log retains partial entries, which
        // inflates epochLogStartIndex for subsequent scopes and causes replay
        // false negatives (missed memoization, BUG-8). This is a performance
        // issue, not a correctness issue.
        //
        // A correct truncation would also need to scrub MemoReplayStore::
        // epochMap entries whose DepRange points past the new log end — a
        // dangling range would cause an out-of-bounds access in
        // replayMemoizedDeps. MemoReplayStore::rollbackEpoch()
        // (memo-replay-store.hh) already implements that scrub; the reason
        // we don't call it here is API boundary, not correctness:
        // DepCaptureScope doesn't hold a MemoReplayStore reference. Plumbing
        // one through (or routing truncation via the recording context) is
        // ~30–50 LoC; the benchmark motivating it hasn't been produced.
        Certifier::withProof([&](const auto & proof) { ctx->popScope(proof); });
    }
    if (isRoot && ownsTraceFrame)
        standaloneTraceFrame.reset();
    // standaloneGuard destructs automatically, restoring the prior
    // thread-local. ownedCtx destructs after it, as required.
}

} // namespace nix
