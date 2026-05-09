#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval-trace/cache/trace-backend.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"
#include "nix/expr/value.hh"
#include "nix/expr/attr-set.hh"
#include "nix/util/logging.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/store/store-api.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-environment/authority-internal.hh"
#include "nix/expr/eval-environment/environment.hh"
#include "cache/traced-expr.hh"
#include "store/verifier.hh"
#include "fiber/fiber-scheduler.hh"
#include "fiber/blocking-scope.hh"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <thread>
#include "nix/expr/eval-trace/eval-context.hh"

#include <cassert>
#include <cstring>
#include <filesystem>

namespace nix {

thread_local eval_trace::SiblingReplayCaptureScope *
    eval_trace::SiblingReplayCaptureScope::current_ = nullptr;

namespace eval_trace {

SiblingReplayCaptureScope::SiblingReplayCaptureScope(
    ParentSlot parentSlot,
    ValueContext selfContext,
    TraceBackend & backend,
    EvalContext<Suspendable> & ctx)
    : parentSlot(parentSlot)
    , parentTraceHash_(backend.getCurrentTraceHash(ctx, parentSlot.value))
    , ctx_(ctx)
    , selfContext(selfContext)
    , previous(current_)
{
    current_ = this;
}

SiblingReplayCaptureScope::~SiblingReplayCaptureScope()
{
    current_ = previous;
}

void SiblingReplayCaptureScope::recordAccess(
    ValueContext pathContext,
    const TraceHash & traceHash)
{
    if (seen.insert(pathContext).second)
        accesses.push_back(CapturedAccess {
            .pathContext = pathContext,
            .traceHash = traceHash,
        });
}

void SiblingReplayCaptureScope::appendRecordedValueContextDeps(std::vector<Dep> & deps) const
{
    for (auto & access : accesses) {
        deps.push_back(Dep::makeValueContext(
            access.pathContext.value,
            DepHashValue(DepHash{access.traceHash.value})));
    }
}

void SiblingReplayCaptureScope::appendDeps(std::vector<Dep> & deps) const
{
    // Emit both the coarse parent slot and fine-grained per-sibling deps.
    // This does NOT defeat selective invalidation: the parent's trace hash
    // only covers attrset shape (attribute names, aliases, metadata), not
    // children's Content/Directory deps. Changing a sibling's file changes
    // that sibling's trace, not the parent's. So ParentSlot remains
    // valid, and only the ValueContext deps for *accessed* siblings
    // determine whether this child is invalidated.
    if (parentTraceHash_) {
        deps.push_back(Dep::makeParentSlot(
            parentSlot, DepHashValue(DepHash{parentTraceHash_->value})));
    }

    appendRecordedValueContextDeps(deps);
}

void appendActiveReplayCaptureDeps(std::vector<Dep> & deps)
{
    if (auto * capture = SiblingReplayCaptureScope::innermost())
        capture->appendDeps(deps);
}

bool SiblingReplayCaptureScope::maybeCapture(
    ParentSlot parentSlot,
    ValueContext pathContext,
    std::optional<TraceHash> traceHash)
{
    if (!shouldCapture(parentSlot, pathContext)) return false;
    try {
        if (!traceHash) return false;
        current_->recordAccess(pathContext, *traceHash);
        return true;
    } catch (...) {
        return false;
    }
}

bool SiblingReplayCaptureScope::shouldCapture(
    ParentSlot parentSlot,
    ValueContext pathContext)
{
    if (!current_) return false;
    if (parentSlot.value != current_->parentSlot.value) return false;
    if (pathContext == current_->selfContext) return false;
    return true;
}

SiblingReplayCaptureScope * snapshotReplayCaptureScope()
{
    return SiblingReplayCaptureScope::snapshotPointer();
}

void restoreReplayCaptureScope(SiblingReplayCaptureScope * scope)
{
    SiblingReplayCaptureScope::restorePointer(scope);
}

} // namespace eval_trace

TraceRuntime::TraceRuntime() = default;

std::optional<DepHash> TraceRuntime::lookupFileContentHash(const SourcePath & path) const
{
    auto it = fileContentHashes.find(path);
    if (it == fileContentHashes.end())
        return std::nullopt;
    return it->second;
}

void TraceRuntime::cacheFileContentHash(const SourcePath & path, DepHash hash)
{
    fileContentHashes.emplace(path, std::move(hash));
}


std::optional<DepRange> TraceRuntime::lookupReplayRange_ForTest(const Value & v) const
{
    return replayStore.getReplayRange(v);
}

bool TraceRuntime::hasReplayEntries_ForTest() const
{
    return !replayStore.epochMap.empty();
}

void TraceRuntime::clearReplayEntries_ForTest()
{
    replayStore.clearReplayIndex();
}

std::unique_ptr<eval_trace::TraceBackend> TraceRuntime::makeTraceBackend(
    const Hash & fingerprint, SymbolTable & syms)
{
    auto bootstrapKey = eval_trace::SemanticSessionKey::fromSerialized(
        "bootstrap:"
        + fingerprint.to_string(HashFormat::Base16, false));
    return std::make_unique<eval_trace::TraceBackend>(
        std::make_shared<eval_trace::SqliteTraceStorage>(
            syms,
            tracingPools(),
            getVocabStore(syms),
            std::move(bootstrapKey)),
        tracingPools());
}

// ── TraceBackend out-of-line methods ─────────────────────────────────

namespace eval_trace {

namespace {

/// RAII wrapper for io_context worker threads. Stops the io_context
/// and joins all workers on destruction.
struct IocWorkerPool {
    IocWorkerPool(
        boost::asio::io_context & ioc,
        boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type> & workGuard,
        uint32_t numThreads)
        : ioc_(ioc)
        , workGuard_(workGuard)
    {
        for (uint32_t i = 0; i < numThreads; ++i)
            workers_.emplace_back([&ioc] { ioc.run(); });
    }

    void shutdown()
    {
        workGuard_.reset();
        ioc_.stop();
        for (auto & t : workers_)
            if (t.joinable()) t.join();
        workers_.clear();
    }

    ~IocWorkerPool() { shutdown(); }

    IocWorkerPool(const IocWorkerPool &) = delete;
    IocWorkerPool & operator=(const IocWorkerPool &) = delete;

private:
    boost::asio::io_context & ioc_;
    boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type> & workGuard_;
    std::vector<std::thread> workers_;
};

} // namespace

/// Flat-owned async + verifier infrastructure for `TraceBackend`.
///
/// The struct is defined in this TU (not a private header) so the
/// public `trace-backend.hh` header doesn't leak the asio + fiber
/// + blocking-pool includes into every caller.
///
/// Member declaration order = reverse destruction order. `blockingPool`
/// MUST be destroyed before `iocWorkers_` stops the io_context: coroBlock
/// posts timer cancellation to the io_context when blocking work
/// finishes; if the io_context is already stopped, the post is dropped
/// and the coroutine never resumes (deadlock).
///
/// Correct teardown: destructor stops `blockingPool` first (joins
/// blocking threads → they post timer cancellations), then
/// `iocWorkers_.shutdown()` drains the io_context and joins workers,
/// then `verifier`/`ioc_` destructors run in reverse declaration order.
struct BackendAsyncInfra {
    static constexpr uint32_t kIocThreads = 2;
    static constexpr uint32_t kBlockingThreads = 2;

    boost::asio::io_context ioc_;
    FiberScheduler scheduler;
    boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type> workGuard_;
    IocWorkerPool iocWorkers_;
    BlockingThreadPool blockingPool;
    std::unique_ptr<Verifier> verifier;

    BackendAsyncInfra(SqliteTraceStorage & store, InterningPools & /*pools*/)
        : scheduler(ioc_)
        , workGuard_(ioc_.get_executor())
        , iocWorkers_(ioc_, workGuard_, kIocThreads)
        , blockingPool(ioc_, kBlockingThreads)
    {
        verifier = std::make_unique<Verifier>(store, blockingPool);
    }

    ~BackendAsyncInfra()
    {
        // 1. Stop blocking pool FIRST.
        blockingPool.stop();
        // 2. Drain ioc — workers process the timer cancellations posted
        //    by blocking threads, resuming suspended coroutines.
        iocWorkers_.shutdown();
        // 3. All threads stopped. Safe to destroy verifier + remaining
        //    members.
        verifier.reset();
    }

    BackendAsyncInfra(const BackendAsyncInfra &) = delete;
    BackendAsyncInfra & operator=(const BackendAsyncInfra &) = delete;
};

} // namespace eval_trace

eval_trace::TraceBackend::TraceBackend(
    std::shared_ptr<eval_trace::SqliteTraceStorage> storage,
    InterningPools & pools)
    : store(std::move(storage))
    , infra_(std::make_unique<eval_trace::BackendAsyncInfra>(*this->store, pools))
{
}

eval_trace::TraceBackend::~TraceBackend() = default;

void eval_trace::TraceBackend::setSessionConfig(eval_trace::SessionConfig config)
{
    store->setSessionConfig(std::move(config));
}

std::optional<eval_trace::SqliteTraceStorage::VerifyResult>
eval_trace::TraceBackend::verify(
    eval_trace::EvalContext<eval_trace::Suspendable> & ctx, AttrPathId pathId)
{
    // The RAII guard in trace-session.cc (which calls bindSession
    // before any verify()) prevents unbound verify() in practice;
    // the assert catches violations in debug builds if that guard is
    // ever removed or bypassed.
    assert(sessionBound_ && "verify() called before bindSession() — backend not bound");
    return ctx.syncAwait(infra_->verifier->verifyAttr(pathId));
}

eval_trace::FiberScheduler * eval_trace::TraceBackend::getScheduler()
{
    return infra_ ? &infra_->scheduler : nullptr;
}

void eval_trace::TraceBackend::submitPrefetchHints(
    const std::vector<AttrPathId> & pathIds)
{
    if (sessionBound_ && infra_ && infra_->verifier)
        infra_->verifier->submitPrefetchHints(pathIds);
}

void eval_trace::TraceBackend::bindSession(
    const SemanticRegistry & registry,
    EvalState & state)
{
    if (infra_ && infra_->verifier) {
        infra_->verifier->bindSession(registry, state);
        sessionBound_ = true;
    }
}

std::optional<eval_trace::SqliteTraceStorage::RecordResult>
eval_trace::TraceBackend::record(
    eval_trace::EvalContext<eval_trace::Suspendable> & ctx,
    AttrPathId pathId,
    const CachedResult & value,
    const std::vector<Dep> & allDeps)
{
    return ctx.syncAwait(coroBlock(infra_->blockingPool, [&](const gdp::Proof<BlockingTag> & bs) {
        return store->withExclusiveAccess(bs, [&](const auto & ea) {
            return store->record(ea, pathId, value, allDeps);
        });
    }));
}

std::shared_ptr<const std::vector<Dep>>
eval_trace::TraceBackend::loadFullTrace(
    eval_trace::EvalContext<eval_trace::Suspendable> & ctx, TraceId traceId)
{
    return ctx.syncAwait(coroBlock(infra_->blockingPool, [&](const gdp::Proof<BlockingTag> & bs) {
        return store->withExclusiveAccess(bs, [&](const auto & ea) {
            return store->loadFullTrace(ea, traceId);
        });
    }));
}

std::optional<TraceHash>
eval_trace::TraceBackend::getCurrentTraceHash(
    eval_trace::EvalContext<eval_trace::Suspendable> & ctx, AttrPathId pathId)
{
    return ctx.syncAwait(coroBlock(infra_->blockingPool, [&](const gdp::Proof<BlockingTag> & bs) {
        return store->withExclusiveAccess(bs, [&](const auto & ea) {
            return store->getCurrentTraceHash(ea, pathId);
        });
    }));
}

void eval_trace::TraceBackend::flush()
{
    // flush() is a shutdown operation — called after all eval work
    // completes (no concurrent verify/record/loadFullTrace in flight).
    // Direct store access is safe: no strand dispatch needed, no
    // future.get() blocking. Strand dispatch here would introduce a
    // liveness dependency on the async runtime (workers must be alive
    // to process the posted work), which isn't guaranteed at shutdown
    // and causes deadlocks in sandbox environments.
    gdp::Certifier<BlockingTag>::withProof([&](const auto & bs) {
        store->withExclusiveAccess(bs, [&](const auto & ea) {
            store->flush(ea);
        });
    });
}

void eval_trace::TraceBackend::recordRuntimeRoot(
    const SqliteTraceStorage::RuntimeRootRecord & record,
    Store & storeDirConfig)
{
    // recordRuntimeRoot is called during cold eval from fetchTree.
    // Direct store access is safe: this is a simple DB insert that
    // doesn't conflict with verification (which only runs on hot path).
    gdp::Certifier<BlockingTag>::withProof([&](const auto & bs) {
        store->withExclusiveAccess(bs, [&](const auto & ea) {
            store->recordRuntimeRoot(ea, record, storeDirConfig);
        });
    });
}

const eval_trace::SemanticSessionKey &
eval_trace::TraceBackend::currentSemanticSessionKey() const noexcept
{
    return store->currentSemanticSessionKey();
}

std::optional<eval_trace::SqliteTraceStorage::EvalInfoRecord>
eval_trace::TraceBackend::queryEvalInfo(
    AttrPathId pathId, bool allowHistoryFallback)
{
    return store->queryEvalInfoExclusive(pathId, allowHistoryFallback);
}

eval_trace::SqliteTraceStorage::RuntimeRootLoadResult
eval_trace::TraceBackend::loadRuntimeRoots(Store & storeDirConfig)
{
    SqliteTraceStorage::RuntimeRootLoadResult result;
    gdp::Certifier<BlockingTag>::withProof([&](const auto & bs) {
        store->withExclusiveAccess(bs, [&](const auto & ea) {
            result = store->loadRuntimeRoots(ea, storeDirConfig);
        });
    });
    return result;
}

eval_trace::TraceBackend::RuntimeRootResult
eval_trace::TraceBackend::loadAndVerifyRuntimeRoots(EvalState & state)
{
    RuntimeRootResult result;
    {
        auto loaded = loadRuntimeRoots(*state.store);
        result.expectedCount = loaded.storedCount;
        result.rejectedCount = loaded.rejectedCount;
        if (loaded.rejectedCount != 0)
            warn("eval-trace/context: ignored %d malformed runtime-root row(s) for this session",
                static_cast<uint64_t>(loaded.rejectedCount));
        for (auto & entry : loaded.entries) {
            auto runtimeIdentityDisplay = renderRuntimeFetchIdentityDisplay(entry.fetchIdentity);
            try {
                // Reopen the input read-only from the store path recorded during
                // cold evaluation. Verification must not trust fetcher-cache
                // accessors here: a cached accessor can outlive the current
                // source state and falsely validate stale runtime roots.

                auto rootPath = [&]() -> std::optional<SourcePath> {
                    // Use the store path recorded during cold eval.
                    // The SessionRuntimeRoots table stores the exact store path
                    // that the fetcher produced. No need to recompute it from
                    // narHash (which can produce different paths depending on
                    // the fetcher's naming convention). Read-only: just checks
                    // isValidPath + verifies narHash via queryPathInfo.
                    //
                    // Known side-effect: isValidPath writes a negative entry to
                    // the NarInfo disk cache (diskCache->upsertNarInfo) when the
                    // path is NOT present in the store.  This is not a Nix store
                    // write and cannot produce false cache hits, but it is a minor
                    // disk side-effect of this otherwise read-only verification path.
                    try {
                        if (state.store->isValidPath(entry.storePath.value)) {
                            auto actualHash = state.store->queryPathInfo(entry.storePath.value)->narHash;
                            auto expectedHash = entry.narHash.value;
                            if (actualHash != expectedHash) {
                                warn("eval-trace/context: runtime root store-path verification failed for %s: expected %s, got %s",
                                    runtimeIdentityDisplay,
                                    entry.narHash.value.to_string(HashFormat::SRI, true),
                                    actualHash.to_string(HashFormat::SRI, true));
                                return std::nullopt;
                            }
                            EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
                            (void) environment.authorizeStorePath(entry.storePath.value);
                            return state.storePath(entry.storePath.value);
                        }
                    } catch (std::exception & e) {
                        warn("eval-trace/context: runtime root store-path lookup failed for %s: %s",
                            state.store->printStorePath(entry.storePath.value), e.what());
                    }

                    warn("eval-trace/context: runtime root not available for %s — skipping (store path unavailable)", runtimeIdentityDisplay);
                    return std::nullopt;
                }();

                if (!rootPath)
                    continue;

                result.verifiedRoots.push_back(SqliteTraceStorage::VerifiedRuntimeRootRecord{
                    .record = entry,
                    .rootPath = *rootPath,
                });
            } catch (std::exception & e) {
                warn("eval-trace/context: runtime root load failed for %s: %s", runtimeIdentityDisplay, e.what());
            } catch (...) {
                warn("eval-trace/context: runtime root load failed for %s: unknown exception", runtimeIdentityDisplay);
            }
        }
    }
    return result;
}

void TraceRuntime::registerTracedValueIdentity(const Value * key, const eval_trace::TracedExpr & tracedExpr)
{
    auto siblingIdentity = makeSiblingIdentity(
        tracedExpr.parentSlot(),
        tracedExpr.definitionStamp(),
        tracedExpr.slotStamp(),
        tracedExpr.canonicalSiblingIdx);

    valueIdentityMap.withLock([&](auto & map) {
        map.emplace(key, makeValueIdentity(
            true,
            nullptr,
            siblingIdentity,
            tracedExpr.valueContext()));
    });
}

void TraceRuntime::registerMaterializedValueIdentity(
    const Value * key,
    eval_trace::TraceBackend * traceBackend,
    std::optional<SiblingIdentity> siblingIdentity,
    AttrPathId pathId,
    std::optional<ValueIdentityStamp> valueIdentityStamp)
{
    auto listIdentityStamp = makeListIdentityStamp(
        siblingIdentity ? std::optional<ParentSlot>(siblingIdentity->parentSlot) : std::nullopt,
        siblingIdentity ? siblingIdentity->canonicalSiblingIdx : invalidSiblingIndex);
    if (!valueIdentityStamp && listIdentityStamp)
        valueIdentityStamp = lookupOrCreateListValueIdentityStamp(*listIdentityStamp);
    if (!valueIdentityStamp && key && key->isValid() && key->type() == nList && key->listSize() > 0) {
        auto listView = key->listView();
        auto & lbmap = listBackingValueIdentityStampMap.readMap();
        auto lbit = lbmap.find(listView.data());
        if (lbit != lbmap.end())
            valueIdentityStamp = lbit->second;
        else
            valueIdentityStamp = ValueIdentityStamp(nextValueIdentityStamp++);
    }

    valueIdentityMap.withLock([&](auto & map) {
        map.emplace(key, makeValueIdentity(
            false,
            traceBackend,
            siblingIdentity,
            ValueContext(pathId),
            valueIdentityStamp));
    });

    if (valueIdentityStamp && key && key->isValid() && key->type() == nAttrs
        && key->attrs() != &Bindings::emptyBindings) {
        const_cast<Bindings *>(key->attrs())->setValueIdentityStamp(*valueIdentityStamp);
        {
            auto idx = valueIdentityStamp->value;
            if (idx >= stampedValueIdentityVec.size())
                stampedValueIdentityVec.resize(idx + 1);
            stampedValueIdentityVec[idx] = makeValueIdentity(
                false,
                traceBackend,
                std::move(siblingIdentity),
                ValueContext(pathId),
                valueIdentityStamp);
        }
    }

    if (valueIdentityStamp && key && key->isValid() && key->type() == nList
        && key->listSize() > 0) {
        auto listView = key->listView();
        listBackingValueIdentityStampMap.withLock([&](auto & map) {
            map.insert_or_assign(listView.data(), *valueIdentityStamp);
        });
    }
}

PublishedMaterializedIdentity TraceRuntime::publishRootMaterializedValueIdentity(
    const Value * key,
    eval_trace::TraceBackend * traceBackend,
    AttrPathId pathId,
    ValueIdentityStamp valueIdentityStamp)
{
    assert(key);

    ::nix::PublishedMaterializedIdentity publication{
        .key = key,
        .stamp = valueIdentityStamp,
    };

    valueIdentityMap.withLock([&](auto & map) {
        auto it = map.find(key);
        if (it != map.end()) {
            publication.previousValueIdentity = PublishedMaterializedIdentity::ValueIdentitySnapshot{
                .isTracedProducer = it->second.isTracedProducer,
                .siblingIdentity = it->second.siblingIdentity,
                .valueContext = it->second.valueContext,
                .valueIdentityStamp = it->second.valueIdentityStamp,
                .traceBackend = it->second.traceBackend,
            };
        }
        map.insert_or_assign(key, makeValueIdentity(
            false,
            traceBackend,
            std::nullopt,
            ValueContext(pathId),
            valueIdentityStamp));
    });

    if (key->isValid() && key->type() == nAttrs && key->attrs() != &Bindings::emptyBindings) {
        publication.previousBindingsStamp = key->attrs()->getValueIdentityStamp();
        {
            auto idx = valueIdentityStamp.value;
            if (idx < stampedValueIdentityVec.size() && stampedValueIdentityVec[idx].has_value()) {
                auto & prev = *stampedValueIdentityVec[idx];
                publication.previousStampedIdentity = PublishedMaterializedIdentity::ValueIdentitySnapshot{
                    .isTracedProducer = prev.isTracedProducer,
                    .siblingIdentity = prev.siblingIdentity,
                    .valueContext = prev.valueContext,
                    .valueIdentityStamp = prev.valueIdentityStamp,
                    .traceBackend = prev.traceBackend,
                };
            }
            const_cast<Bindings *>(key->attrs())->setValueIdentityStamp(valueIdentityStamp);
            if (idx >= stampedValueIdentityVec.size())
                stampedValueIdentityVec.resize(idx + 1);
            stampedValueIdentityVec[idx] = makeValueIdentity(
                false,
                traceBackend,
                std::nullopt,
                ValueContext(pathId),
                valueIdentityStamp);
        }
        publication.bindings = key->attrs();
    }

    if (key->isValid() && key->type() == nList && key->listSize() > 0) {
        auto listView = key->listView();
        listBackingValueIdentityStampMap.withLock([&](auto & map) {
            auto it = map.find(listView.data());
            if (it != map.end())
                publication.previousListBackingStamp = it->second;
            map.insert_or_assign(listView.data(), valueIdentityStamp);
        });
        publication.listBacking = listView.data();
    }

    return publication;
}

void TraceRuntime::rollbackRootMaterializedValueIdentity(
    const ::nix::PublishedMaterializedIdentity & publication)
{
    if (publication.listBacking) {
        listBackingValueIdentityStampMap.withLock([&](auto & map) {
            if (publication.previousListBackingStamp)
                map.insert_or_assign(
                    publication.listBacking, *publication.previousListBackingStamp);
            else
                map.erase(publication.listBacking);
        });
    }

    if (publication.stamp) {
        auto idx = publication.stamp->value;
        if (publication.previousStampedIdentity) {
            auto & snapshot = *publication.previousStampedIdentity;
            if (idx >= stampedValueIdentityVec.size())
                stampedValueIdentityVec.resize(idx + 1);
            stampedValueIdentityVec[idx] = makeValueIdentity(
                snapshot.isTracedProducer,
                snapshot.traceBackend,
                snapshot.siblingIdentity,
                snapshot.valueContext,
                snapshot.valueIdentityStamp);
        } else {
            if (idx < stampedValueIdentityVec.size())
                stampedValueIdentityVec[idx].reset();
        }
    }

    if (publication.bindings) {
        if (publication.previousBindingsStamp)
            const_cast<Bindings *>(publication.bindings)->setValueIdentityStamp(
                *publication.previousBindingsStamp);
        else
            const_cast<Bindings *>(publication.bindings)->clearValueIdentityStamp();
    }

    if (publication.key) {
        valueIdentityMap.withLock([&](auto & map) {
            if (publication.previousValueIdentity) {
                auto & snapshot = *publication.previousValueIdentity;
                map.insert_or_assign(
                    publication.key,
                    makeValueIdentity(
                        snapshot.isTracedProducer,
                        snapshot.traceBackend,
                        snapshot.siblingIdentity,
                        snapshot.valueContext,
                        snapshot.valueIdentityStamp));
            } else {
                map.erase(publication.key);
            }
        });
    }
}

void TraceRuntime::registerValueIdentity_ForTest(const Value * key, AttrPathId pathId)
{
    valueIdentityMap.withLock([&](auto & map) {
        map.emplace(key, makeValueIdentity(
            false, nullptr, std::nullopt, ValueContext(pathId)));
    });
}

bool TraceRuntime::hasValueIdentity_ForTest(const Value * key) const
{
    return valueIdentityMap.readMap().find(key) != valueIdentityMap.readMap().end();
}

bool TraceRuntime::hasBindingsValueIdentity_ForTest(const Bindings * key) const
{
    return key && key->getValueIdentityStamp().has_value();
}

void TraceRuntime::registerTracedBindingsValueIdentity(
    const Bindings * key,
    const eval_trace::TracedExpr & tracedExpr,
    const Bindings * originalBindings)
{
    auto siblingIdentity = makeSiblingIdentity(
        tracedExpr.parentSlot(),
        tracedExpr.definitionStamp(),
        tracedExpr.slotStamp(),
        tracedExpr.canonicalSiblingIdx);

    std::optional<ValueIdentityStamp> valueIdentityStamp = std::nullopt;
    const Bindings * stampedSource = originalBindings ? originalBindings : key;
    if (stampedSource && stampedSource != &Bindings::emptyBindings) {
        if (auto stamp = stampedSource->getValueIdentityStamp())
            valueIdentityStamp = *stamp;
    }
    if (!valueIdentityStamp && siblingIdentity) {
        if (auto aliasStamp = makeListIdentityStamp(
                std::optional<ParentSlot>(siblingIdentity->parentSlot),
                siblingIdentity->canonicalSiblingIdx)) {
            valueIdentityStamp = lookupOrCreateListValueIdentityStamp(*aliasStamp);
        }
    }
    if (!valueIdentityStamp && stampedSource)
        valueIdentityStamp = lookupOrCreateBindingsIdentityStamp(stampedSource);
    if (!valueIdentityStamp)
        valueIdentityStamp = lookupOrCreateBindingsIdentityStamp(key);

    if (key && key != &Bindings::emptyBindings)
        const_cast<Bindings *>(key)->setValueIdentityStamp(*valueIdentityStamp);
    if (originalBindings && originalBindings != &Bindings::emptyBindings)
        const_cast<Bindings *>(originalBindings)->setValueIdentityStamp(*valueIdentityStamp);
    {
        auto idx = valueIdentityStamp->value;
        if (idx >= stampedValueIdentityVec.size())
            stampedValueIdentityVec.resize(idx + 1);
        stampedValueIdentityVec[idx] = makeValueIdentity(
            true,
            nullptr,
            siblingIdentity,
            tracedExpr.valueContext(),
            valueIdentityStamp);
    }

}

void TraceRuntime::rememberNixBinding(PosIdx pos, const NixBindingEntry & entry)
{
    semanticAnalyzer.rememberBinding(pos, entry);
}

const NixBindingEntry * TraceRuntime::lookupNixBinding(PosIdx pos) const
{
    return semanticAnalyzer.lookupBinding(pos);
}

TraceRuntime::ValueIdentity::ValueIdentity(
    bool isTracedProducer,
    eval_trace::TraceBackend * traceBackend,
    std::optional<SiblingIdentity> siblingIdentity,
    ValueContext valueContext,
    std::optional<ValueIdentityStamp> valueIdentityStamp)
    : isTracedProducer(isTracedProducer)
    , traceBackend(traceBackend)
    , siblingIdentity(std::move(siblingIdentity))
    , valueContext(valueContext)
    , valueIdentityStamp(valueIdentityStamp)
{
}

std::optional<SiblingIdentity> TraceRuntime::makeSiblingIdentity(
    std::optional<ParentSlot> parentSlot,
    std::optional<DefinitionStamp> definitionStamp,
    std::optional<SlotStamp> slotStamp,
    uint32_t canonicalSiblingIdx)
{
    if (!parentSlot || !definitionStamp || !slotStamp)
        return std::nullopt;
    return SiblingIdentity{
        .parentSlot = *parentSlot,
        .definitionStamp = *definitionStamp,
        .slotStamp = *slotStamp,
        .canonicalSiblingIdx = canonicalSiblingIdx,
    };
}

std::optional<TraceRuntime::ListIdentityStamp> TraceRuntime::makeListIdentityStamp(
    std::optional<ParentSlot> parentSlot,
    uint32_t canonicalSiblingIdx)
{
    if (!parentSlot || canonicalSiblingIdx == invalidSiblingIndex)
        return std::nullopt;

    return ListIdentityStamp{
        .parentSlot = *parentSlot,
        .canonicalSiblingIdx = canonicalSiblingIdx,
    };
}

TraceRuntime::ValueIdentity TraceRuntime::makeValueIdentity(
    bool isTracedProducer,
    eval_trace::TraceBackend * traceBackend,
    std::optional<SiblingIdentity> siblingIdentity,
    ValueContext valueContext,
    std::optional<ValueIdentityStamp> valueIdentityStamp)
{
    return ValueIdentity(
        isTracedProducer, traceBackend, std::move(siblingIdentity), valueContext, valueIdentityStamp);
}

/// Lock-free read: find() doesn't allocate, no GC reentrancy risk.
std::optional<TraceRuntime::ValueIdentity>
TraceRuntime::lookupCapturedValueIdentity(const Value & v) const
{
    auto & map = valueIdentityMap.readMap();
    auto it = map.find(&v);
    return it != map.end() ? std::optional{it->second} : std::nullopt;
}

std::optional<TraceRuntime::ValueIdentity>
TraceRuntime::lookupValueIdentity(Value & v) const
{
    if (v.type() == nAttrs) {
        if (auto stamp = v.attrs()->getValueIdentityStamp()) {
            if (stamp->value < stampedValueIdentityVec.size()
                && stampedValueIdentityVec[stamp->value].has_value())
                return *stampedValueIdentityVec[stamp->value];
        }
    }

    auto & map = valueIdentityMap.readMap();
    auto it = map.find(&v);
    return it != map.end() ? std::optional{it->second} : std::nullopt;
}

std::optional<ValueIdentityStamp> TraceRuntime::lookupValueIdentityStamp(const Value & v) const
{
    if (v.type() == nAttrs) {
        if (auto stamp = v.attrs()->getValueIdentityStamp())
            return stamp;
    }

    if (v.type() == nList && v.listSize() > 0) {
        auto listView = v.listView();
        auto & lmap = listBackingValueIdentityStampMap.readMap();
        auto lit = lmap.find(listView.data());
        if (lit != lmap.end())
            return lit->second;
    }

    auto identity = lookupValueIdentity(const_cast<Value &>(v));
    if (!identity)
        return std::nullopt;
    return identity->valueIdentityStamp;
}

ValueIdentityStamp TraceRuntime::lookupOrCreateBindingsIdentityStamp(const Bindings * key)
{
    if (key) {
        if (auto stamp = key->getValueIdentityStamp())
            return *stamp;
    }

    auto stamp = ValueIdentityStamp(nextValueIdentityStamp++);
    if (key && key != &Bindings::emptyBindings)
        const_cast<Bindings *>(key)->setValueIdentityStamp(stamp);
    return stamp;
}

ValueIdentityStamp TraceRuntime::lookupOrCreateListValueIdentityStamp(const ListIdentityStamp & stamp)
{
    // Read-first (lock-free), write only on miss (locked).
    auto & rmap = listValueIdentityStampMap.readMap();
    auto it = rmap.find(stamp);
    if (it != rmap.end())
        return it->second;
    auto result = ValueIdentityStamp(nextValueIdentityStamp++);
    listValueIdentityStampMap.withLock([&](auto & map) {
        map.emplace(stamp, result);
    });
    return result;
}

void TraceRuntime::recordThunkDeps(const Value & v, uint32_t epochStart)
{
    replayStore.recordThunkDeps(v, epochStart);
}

void TraceRuntime::rollbackReplayEpoch(uint32_t epochStart)
{
    replayStore.rollbackEpoch(epochStart);
}

bool TraceRuntime::shouldIsolateSiblingForce(const Value & v) const
{
    // Fast reject: no capture scope active → sibling isolation impossible.
    // SiblingReplayCaptureScope::innermost() is thread_local, set only
    // during evaluateResolvedTarget for child TracedExprs with a parent
    // slot. Only ~6 exist per evaluation vs ~12M thunk forces — this
    // eliminates the lock + map lookup for 99.99%+ of calls.
    if (!eval_trace::SiblingReplayCaptureScope::innermost())
        return false;
    auto sibling = lookupCapturedValueIdentity(v);
    return sibling
        && sibling->siblingIdentity
        && sibling->isTracedProducer
        && eval_trace::SiblingReplayCaptureScope::shouldCapture(
            sibling->siblingIdentity->parentSlot,
            sibling->valueContext);
}

// ── replayMemoizedDeps sibling capture ───────────────────────────────
//
// For already-forced siblings, replayMemoizedDeps can collapse the replay
// range directly to a compact ValueContext dep. First-touch sibling
// forcing now routes through the same capture path in forceThunkValue by
// publishing the sibling's replay range immediately after evaluation and
// replaying it under the active capture scope.
void TraceRuntime::replayMemoizedDeps(const Value & v)
{
    eval_trace::nrReplayTotalCalls++;
    auto rangeOpt = replayStore.getReplayRange(v);
    if (!rangeOpt) return;
    eval_trace::nrReplayBloomHits++;
    eval_trace::nrReplayEpochHits++;

    // Sibling detection: if this value is a registered sibling TracedExpr
    // and a replay capture scope is active, record the sibling access and
    // skip dep copy (the child gets a ValueContext dep instead).
    // On failure (parent mismatch, missing trace hash, or no active
    // capture), fall through to normal dep replay.
    //
    // Fast reject: skip identity lookup + lock when no capture scope is
    // active. The bloom filter above eliminates Values without memoized
    // deps; this eliminates the lock for the remaining Values when no
    // sibling capture is in progress.
    if (auto * captureScope = eval_trace::SiblingReplayCaptureScope::innermost()) {
    if (auto sibling = lookupCapturedValueIdentity(v)) {
        if (sibling->siblingIdentity) {
            // ctx is carried by the active SiblingReplayCaptureScope —
            // a scoped RAII guard constructed in evaluateResolvedTarget
            // with ctx. Same pattern as DepRecordingContext / TraceFrame.
            auto & ctx = captureScope->ctx();
            auto traceHash = sibling->traceBackend
                ? sibling->traceBackend->getCurrentTraceHash(ctx, sibling->valueContext.value)
                : std::optional<TraceHash>{};
            if (eval_trace::SiblingReplayCaptureScope::maybeCapture(
                    sibling->siblingIdentity->parentSlot,
                    sibling->valueContext,
                    traceHash)) {
                return;
            }
        }
    }
    }

    auto & range = *rangeOpt;

    auto access = eval_trace::TraceAccess::current();
    if (!access) return;
    if (access->replayMemoizedRange(v, range)) {
        eval_trace::nrReplayAdded++;
    }
}

bool TraceRuntime::sameValueIdentity(Value & v1, Value & v2)
{
    // String/path values: check IdentityObject on SemanticHandle.
    // Identity stamps are carried on the publication slot, avoiding
    // dependence on the replay-time valueIdentityMap.
    if ((v1.type() == nString || v1.type() == nPath)
        && v1.type() == v2.type()) {
        auto * pub1 = v1.publication();
        auto * pub2 = v2.publication();
        if (pub1 && pub2 && pub1->hasIdentity() && pub2->hasIdentity()) {
            return pub1->identity == pub2->identity;
        }
        return false;
    }

    if (v1.type() == nList && v2.type() == nList) {
        if (v1.listSize() != v2.listSize())
            return false;

        auto list1 = v1.listView();
        auto list2 = v2.listView();
        if (list1.data() == list2.data())
            return true;

        if (auto valueStamp1 = lookupValueIdentityStamp(v1)) {
            if (auto valueStamp2 = lookupValueIdentityStamp(v2)) {
                if (*valueStamp1 == *valueStamp2)
                    return true;
            }
        }
        return false;
    }

    if (v1.type() == nAttrs && v2.type() == nAttrs) {
        if (auto stamp1 = v1.attrs()->getValueIdentityStamp()) {
            if (auto stamp2 = v2.attrs()->getValueIdentityStamp()) {
                if (*stamp1 == *stamp2)
                    return true;
            }
        }
        return false;
    }

    auto si1 = lookupValueIdentity(v1);
    if (!si1)
        return false;
    auto si2 = lookupValueIdentity(v2);
    if (!si2)
        return false;

    // Tier 1: same parent slot + stamped sibling identity. SlotStamp is
    // decisive when present on both sides; otherwise DefinitionStamp is
    // decisive. canonicalSiblingIdx is only a compatibility fallback when
    // stamped identity is absent on one or both sides.
    if (si1->siblingIdentity && si2->siblingIdentity
        && si1->siblingIdentity->parentSlot == si2->siblingIdentity->parentSlot) {
        auto & sibling1 = *si1->siblingIdentity;
        auto & sibling2 = *si2->siblingIdentity;

        if (sibling1.slotStamp && sibling2.slotStamp) {
            if (sibling1.slotStamp == sibling2.slotStamp)
                return true;
        } else if (sibling1.definitionStamp && sibling2.definitionStamp) {
            if (sibling1.definitionStamp == sibling2.definitionStamp)
                return true;
        } else if (sibling1.canonicalSiblingIdx != invalidSiblingIndex
            && sibling1.canonicalSiblingIdx == sibling2.canonicalSiblingIdx) {
            return true;
        }
    }

    // Tier 2: same ValueIdentityStamp.
    if (si1->valueIdentityStamp && si2->valueIdentityStamp
        && si1->valueIdentityStamp == si2->valueIdentityStamp)
        return true;

    return false;
}


void TraceRuntime::reset()
{
    fileContentHashes.clear();
    semanticAnalyzer.clear();
    replayStore.clear();
    valueIdentityMap.clear();
    stampedValueIdentityVec.clear();
    listValueIdentityStampMap.clear();
    listBackingValueIdentityStampMap.clear();
    nextValueIdentityStamp = 1;
}

} // namespace nix
