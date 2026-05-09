#include "traced-expr.hh"
// Root/Child headers collapsed into traced-expr.hh in the 2026-05
// devirtualisation pass.

#include "nix/util/finally.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval-trace/counters.hh"
#include "nix/expr/eval-trace/eval-context.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/deps/dep-capture-scope.hh"
#include "nix/expr/eval-trace/deps/trace-activation-scope.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/eval-trace/deps/hash.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/fetchers/fetchers.hh"
#include "../fiber/fiber-scheduler.hh"

#include <algorithm>
#include <cassert>

namespace nix::eval_trace {

namespace {

thread_local TraceSession * currentTraceSession_ = nullptr;

struct TraceSessionActivationScope {
    TraceSession * prev = nullptr;

    explicit TraceSessionActivationScope(TraceSession & session)
        : prev(currentTraceSession_)
    {
        currentTraceSession_ = &session;
    }

    ~TraceSessionActivationScope()
    {
        currentTraceSession_ = prev;
    }
};

struct TracePublishScope {
    TracedExpr & expr;
    TraceBackend & backend;
    EvalContext<Suspendable> & ctx;

    TracePublishScope(TracedExpr & expr, TraceBackend & backend, EvalContext<Suspendable> & ctx)
        : expr(expr)
        , backend(backend)
        , ctx(ctx)
    {
    }

    bool publish(const CachedResult & value, std::vector<Dep> deps)
    {
        try {
            if (auto result = backend.record(ctx, expr.pathId, value, deps)) {
                expr.ensureLazy().traceId = result->traceId;
                return true;
            }
        } catch (std::exception & e) {
            if (verbosity >= lvlDebug) {
                debug("eval-trace/session: trace recording failed for '%s': %s", expr.attrPathStr(), e.what());
            }
        }
        return false;
    }
};

static std::vector<uint32_t> detectSiblingAliases(const std::vector<Value *> & siblingVals)
{
    boost::unordered_flat_map<const Value *, size_t> valueToIdx;
    std::vector<uint32_t> aliasOf(siblingVals.size(), invalidSiblingIndex);
    bool hasAliases = false;
    for (size_t i = 0; i < siblingVals.size(); ++i) {
        auto [it, inserted] = valueToIdx.emplace(siblingVals[i], i);
        if (!inserted) {
            aliasOf[i] = static_cast<uint32_t>(it->second);
            hasAliases = true;
        }
    }
    if (!hasAliases)
        aliasOf.clear();
    return aliasOf;
}

} // namespace

void TracedExpr::evaluateResolvedTarget(EvalContext<Suspendable> & ctx, Value & v, std::function<Value *()> targetProvider)
{
    TraceSessionActivationScope activeSession(*cache);
    DepCaptureScope depCapture(cache->state.tracingPools(), cache->registry_);
    TraceActivationScope traceActivation(cache->state);

    // Lock-file dep precision: flake.lock changes are tracked via per-key
    // StructuredContent deps recorded during callFlake graph traversal.
    // The resolved graph is passed to call-flake.nix as a structured Nix
    // attrset (not as a lock-file string); per-key StructuredProjection deps are recorded at
    // graph-node attribute accesses. Only traces that access a changed lock
    // entry are invalidated.
    TracePublishScope publishTrace(*this, *cache->runtime_, ctx);

    std::optional<SiblingReplayCaptureScope> siblingCapture;
    if (auto slot = parentSlot()) {
        siblingCapture.emplace(
            *slot,
            valueContext(),
            *cache->runtime_,
            ctx);
    }

    Value * target = targetProvider();
    auto & st = cache->state;

    // Cache the value identity for pointer identity restoration in eqValues.
    cacheResolvedTarget(target);

    try {
        st.forceValue(*target, noPos);

        // drvPath-forcing guard -- CRITICAL CORRECTNESS INVARIANT.
        // Force drvPath BEFORE materialization to prevent infinite recursion
        // from the `//` operator sharing Value ptrs.
        if (target->type() == nAttrs && st.isDerivation(*target)) {
            if (auto * dp = target->attrs()->get(st.s.drvPath))
                st.forceValue(*dp->value, noPos);
        }
    } catch (EvalError & e) {
        if (verbosity >= lvlDebug) {
            debug("eval-trace/session: setting '%s' to failed (fresh evaluation)", attrPathStr());
        }
        auto directDeps = depCapture.finalizeAndTakeDeps();
        if (depCapture.isStable())
            publishTrace.publish(failed_t{.errorMessage = e.message()}, std::move(directDeps));
        throw;
    }

    if (target->type() == nAttrs && target->attrs() != &Bindings::emptyBindings) {
        cache->state.registerTracedBindingsValueIdentity(target->attrs(), *this, target->attrs());
    } else if (target->type() == nList) {
        auto slot = parentSlot();
        auto defStamp = definitionStamp();
        auto slotStampValue = slotStamp();
        std::optional<SiblingIdentity> siblingIdentity = std::nullopt;
        if (slot && defStamp && slotStampValue && canonicalSiblingIdx != invalidSiblingIndex) {
            siblingIdentity = SiblingIdentity{
                .parentSlot = *slot,
                .definitionStamp = *defStamp,
                .slotStamp = *slotStampValue,
                .canonicalSiblingIdx = canonicalSiblingIdx,
            };
        }
        cache->state.registerMaterializedValueIdentity(
            target,
            cache->runtime_.get(),
            std::move(siblingIdentity),
            pathId);
    }

    auto directDeps = depCapture.finalizeAndTakeDeps();
    CachedResult attrValue = buildCachedResult(st, *target);

    bool hasBackendRecord = depCapture.isStable()
        && publishTrace.publish(attrValue, std::move(directDeps));

    auto * trivial = std::get_if<trivial_t>(&attrValue);
    bool triggersFreshEval = trivial
        && (trivial->kind == TrivialKind::Misc
         || trivial->kind == TrivialKind::Missing
         || trivial->kind == TrivialKind::Placeholder);
    if (!hasBackendRecord || triggersFreshEval) {
        v = *target;
        return;
    }
    materializeResult(ctx, v, attrValue);
}

TraceSession * currentTraceSession()
{
    return currentTraceSession_;
}

// ── TracedExpr core methods ──────────────────────────────────────────

// Defined out-of-line because it needs TraceSession and AttrVocabStore definitions.
AttrVocabStore & TracedExpr::vocab() const
{
    return cache->state.vocabStore();
}

TraceBackend * TracedExpr::traceBackend() const
{
    return cache->runtime_.get();
}

std::string TracedExpr::attrPathStr() const
{
    return vocab().displayPath(pathId);
}

TracedExpr::TracedExpr(TraceSession & cache, Kind kind)
    : cache(&cache)
    , kind_(kind)
{
}

TracedExpr * TracedExpr::makeRoot(TraceSession & session)
{
    return new TracedExpr(session, Kind::Root);
}

TracedExpr * TracedExpr::makeChild(
    TraceSession & cache,
    Symbol name,
    TracedExpr & parent,
    std::optional<size_t> listIndex)
{
    auto * child = new TracedExpr(cache, Kind::Child);
    child->parentExpr = &parent;
    child->name = name;
    child->listIndex = std::move(listIndex);
    child->parentSlot_ = ParentSlot(parent.pathId);
    child->pathId = child->vocab().extendPath(parent.pathId, name);
    child->definitionStamp_ = cache.definitionStampForChildPath(child->pathId);
    child->slotStamp_ = cache.slotStampForChildPath(child->pathId);
    return child;
}

// `evaluateFresh` and `evaluateDirect` are identical for Root and
// Child — only `navigateToReal` differs.  The kind-switch lives
// inside `navigateToReal` below.
void TracedExpr::evaluateFresh(EvalContext<Suspendable> & ctx, Value & v)
{
    evaluateResolvedTarget(ctx, v, [&] { return navigateToReal(); });
}

void TracedExpr::evaluateDirect(Value & v)
{
    auto * target = navigateToReal();
    cache->state.forceValue(*target, noPos);
    v = *target;
}

// Root's `navigateToReal` just returns the session's real root.
// Child's implementation is in `materialize.cc` alongside the other
// materialization bits (reuses the `traverseRealTree` machinery).
//
// Defined here (not out-of-line in each TU) so the function pointer
// is a single symbol; implementation chooses by `kind_`.
Value * TracedExpr::navigateToReal()
{
    switch (kind_) {
    case Kind::Root:
        return cache->getRealRoot();
    case Kind::Child:
        // Child: walk up the parent chain, registering siblings
        // during the walk, without forcing the final value or
        // caching the target.
        return traverseRealTree(
            /* registerSiblingIdentities */ true,
            /* forceFinalValue */ false,
            /* cacheTarget */ false);
    }
    return nullptr;
}

Value * TracedExpr::getResolvedTarget()
{
    if (kind_ == Kind::Root)
        return nullptr;
    if (auto * target = peekResolvedTarget())
        return target;
    try {
        return traverseRealTree(
            /* registerSiblingIdentities */ false,
            /* forceFinalValue */ true,
            /* cacheTarget */ true);
    } catch (std::exception & e) {
        if (verbosity >= lvlDebug) {
            debug("eval-trace/session: getResolvedTarget failed for '%s': %s", attrPathStr(), e.what());
        }
        return nullptr;
    }
}

NodeLocator TracedExpr::tracePathFromRoot() const
{
    // Walk up the parent chain via `parentExpr` (nullptr terminates
    // at the root).  Safe to call on a Root directly — returns
    // empty.
    std::vector<const TracedExpr *> lineage;
    for (auto * current = this; current && current->kind_ == Kind::Child;
         current = current->parentExpr)
        lineage.push_back(current);

    NodeLocator path;
    path.reserve(lineage.size());
    for (auto it = lineage.rbegin(); it != lineage.rend(); ++it) {
        if ((*it)->listIndex)
            path.push_back(ListSelector{*(*it)->listIndex});
        else
            path.push_back(AttrSelector{(*it)->name});
    }
    return path;
}

std::vector<TracedExpr *> TracedExpr::traceChainFromRoot() const
{
    // Walk parent chain; stops at the Root (whose `parentExpr` is
    // null and whose `kind_` is `Root`).
    std::vector<TracedExpr *> chain;
    auto * current = const_cast<TracedExpr *>(this);
    while (current && current->kind_ == Kind::Child) {
        chain.push_back(current);
        current = current->parentExpr;
    }
    if (current) // the Root itself
        chain.push_back(current);
    std::reverse(chain.begin(), chain.end());
    return chain;
}

/// Install `child` as a TracedExpr thunk on `val` and register it in
/// the value identity map for already-materialized sibling detection.
void TracedExpr::installChildThunk(Value * val, Env * env, TracedExpr * child)
{
    val->mkThunk(env, child);
    cache->state.registerTracedValueIdentity(val, *child);
}

Value * TracedExpr::traverseRealTree(
    bool registerSiblingIdentities,
    bool forceFinalValue,
    bool cacheTarget)
{
    auto path = tracePathFromRoot();
    auto * value = cache->getRealRoot();
    auto storeExprChain = registerSiblingIdentities
        ? traceChainFromRoot()
        : std::vector<TracedExpr *>{};

    size_t pathStep = 0;
    for (auto & step : path) {
        if (auto * list = std::get_if<ListSelector>(&step)) {
            cache->state.forceValue(*value, noPos);
            if (value->type() != nList) {
                if (registerSiblingIdentities)
                    throw Error("expected a list but found %s while navigating to cached list element",
                                showType(*value));
                throw Error("expected a list while resolving target");
            }

            auto index = list->index;
            if (index >= value->listSize()) {
                if (registerSiblingIdentities)
                    throw Error("list index %d out of bounds (size %d) during re-evaluation",
                                index, value->listSize());
                throw Error("list index out of bounds while resolving target");
            }

            value = value->listView()[index];
        } else {
            auto & attrSelector = std::get<AttrSelector>(step);
            cache->state.forceAttrs(
                *value,
                noPos,
                registerSiblingIdentities
                    ? "while navigating to cached attribute"
                    : "while resolving target for pointer identity");

            if (registerSiblingIdentities) {
                auto * parentExpr = storeExprChain[pathStep];

                // Skip if this parent's siblings were already registered
                // by a previous child's traversal through the same path.
                if (!cache->registeredSiblingParents.insert(parentExpr->pathId).second)
                    goto skipSiblingRegistration;

                {
                std::vector<Value *> siblingVals;
                std::vector<Symbol> siblingNames;
                siblingVals.reserve(value->attrs()->size());
                siblingNames.reserve(value->attrs()->size());
                for (auto & attr : *value->attrs()) {
                    siblingVals.push_back(attr.value);
                    siblingNames.push_back(attr.name);
                }

                auto aliasOf = detectSiblingAliases(siblingVals);
                for (size_t i = 0; i < siblingVals.size(); ++i) {
                    auto * attrValue = siblingVals[i];
                    auto attrName = siblingNames[i];

                    auto siblingPathId = vocab().extendPath(parentExpr->pathId, attrName);
                    uint32_t canonicalSiblingIdx = invalidSiblingIndex;
                    if (!aliasOf.empty()) {
                        canonicalSiblingIdx = (aliasOf[i] != invalidSiblingIndex)
                            ? aliasOf[i]
                            : static_cast<uint32_t>(i);
                    }

                    cache->state.registerMaterializedValueIdentity(
                        attrValue,
                        cache->runtime_.get(),
                        std::optional<SiblingIdentity>(SiblingIdentity{
                            .parentSlot = ParentSlot(parentExpr->pathId),
                            .definitionStamp = cache->definitionStampForChildPath(siblingPathId),
                            .slotStamp = cache->slotStampForChildPath(siblingPathId),
                            .canonicalSiblingIdx = canonicalSiblingIdx,
                        }),
                        siblingPathId);
                }
                }
                skipSiblingRegistration:;
            }

            auto * attr = value->attrs()->get(attrSelector.name);
            if (!attr) {
                auto attrName = cache->state.symbols[attrSelector.name];
                if (registerSiblingIdentities)
                    throw Error("attribute '%s' vanished during re-evaluation",
                                attrName);
                throw Error("attribute vanished while resolving target");
            }
            value = attr->value;
        }

        ++pathStep;
    }

    if (forceFinalValue)
        cache->state.forceValue(*value, noPos);
    if (cacheTarget)
        cacheResolvedTarget(value);
    return value;
}

RootHandle::RootHandle(EvalState & state, RootLoader rootLoader)
    : rootLoader(std::move(rootLoader))
{
    (void) state;
}

Value * RootHandle::getRealRoot()
{
    if (!realRoot) {
        debug("eval-trace/session: getting root value via rootLoader");
        realRoot = allocRootValue(rootLoader());
    }
    return *realRoot;
}

void RootHandle::reset(RootLoader newRootLoader)
{
    rootLoader = std::move(newRootLoader);
    realRoot.reset();
}

DefinitionStamp TraceSession::definitionStampForChildPath(AttrPathId pathId)
{
    auto idx = pathId.value;
    if (idx >= childDefinitionStamps.size())
        childDefinitionStamps.resize(idx + 1);
    auto & slot = childDefinitionStamps[idx];
    if (!slot.value) {
        slot = DefinitionStamp(nextDefinitionStamp);
        ++nextDefinitionStamp;
    }
    return slot;
}

SlotStamp TraceSession::slotStampForChildPath(AttrPathId pathId)
{
    auto idx = pathId.value;
    if (idx >= childSlotStamps.size())
        childSlotStamps.resize(idx + 1);
    auto & slot = childSlotStamps[idx];
    if (!slot.value) {
        slot = SlotStamp(nextSlotStamp);
        ++nextSlotStamp;
    }
    return slot;
}

// TracedExpr::eval -- demand-driven dispatch (Adapton)
void TracedExpr::eval(EvalState & state, Env & env, Value & v)
{
    // Ensure the entire evaluation subtree runs inside a single task.
    // The first TracedExpr::eval() creates the task context. All nested
    // eval() calls (from materializeResult → child forcing) find
    // insideTask() == true and skip this check (just a thread_local read).
    if (!FiberScheduler::insideTask()) {
        auto * scheduler = cache->runtime_ ? cache->runtime_->getScheduler() : nullptr;
        if (scheduler) {
            // Thread-affinity check: the eval-trace pipeline requires
            // single-threaded dispatch on the scheduler's owner thread.
            // A worker thread (e.g. a parallel primop) attempting to
            // enter the colored pipeline aborts here with a loud
            // diagnostic rather than silently degrading trace
            // recording on the worker subtree.
            if (!scheduler->onOwnerThread()) [[unlikely]]
                evalContextViolation(
                    "eval-trace/dispatch: TracedExpr evaluated off the "
                    "scheduler's owner thread. The eval-trace pipeline "
                    "requires single-threaded dispatch; this typically "
                    "indicates a parallel primop forcing a traced "
                    "expression on a worker thread.");
            scheduler->run([&] { eval(state, env, v); }, &cache->state.tracingPools(),
                          &cache->state.replayEpochLog());
            return;
        }
    }

    nrTracedExprForced++;

    // ── EvalContext<Suspendable> ──────────────────────────────────
    //
    // SuspendableCtxScope is the only minter of EvalContext<Suspendable>
    // (its ctor is private; the scope is friended).  At the outermost
    // TracedExpr::eval entry (prev_ == nullptr inside the ctor), the
    // scope mints a new ctx via placement-new.  At nested entries it
    // adopts the enclosing scope's ctx after a consistency check.
    //
    // No-backend path: FiberScheduler::current() == nullptr; we
    // skip scope construction entirely, ctx stays null, and fall
    // through to evaluateDirect below.
    auto * sched = FiberScheduler::current();
    std::optional<SuspendableCtxScope> scope;
    if (sched) scope.emplace(state, *sched);
    auto * ctx = scope ? &scope->ctx() : nullptr;

    // Without a Suspendable context, verify() can't be called (it requires
    // EvalContext<Suspendable> &). TraceBackend::verify needs it for
    // syncAwait. If ctx is null, skip verification entirely.
    // This is the no-scheduler path (eval-trace disabled or no AsyncRuntime).

    // Register Bindings* → ValueIdentity on all exit paths (attrset values).
    // ExprOpEq::eval creates stack-local Value copies (via ExprSelect's
    // `v = *vAttrs`), which have different addresses than the originals
    // in valueIdentityMap. But copies preserve the Bindings* pointer,
    // so this secondary map enables sameValueIdentity to find the
    // TracedExpr that produced a copied attrset Value.
    //
    // originalBindings is set from the cached resolved target when available.
    Finally registerBindings{[&]{
        try {
            if (v.type() != nAttrs) return;
            const Bindings * originalBindings = resolvedAttrBindingsHint();
            cache->state.registerTracedBindingsValueIdentity(v.attrs(), *this, originalBindings);
        } catch (...) {}
    }};
    Finally registerListIdentity{[&]{
        try {
            if (v.type() != nList) return;
            std::optional<ValueIdentityStamp> valueIdentityStamp = cache->state.lookupValueIdentityStamp(v);
            if (v.listSize() > 0 && v.listSize() <= 2) {
                auto view = v.listView();
                auto stableList = state.buildList(v.listSize(), true);
                for (size_t i = 0; i < v.listSize(); ++i)
                    stableList[i] = view[i];
                v.mkList(stableList);
            }
            auto slot = parentSlot();
            auto defStamp = definitionStamp();
            auto slotStampValue = slotStamp();
            if (!slot || !defStamp || !slotStampValue || canonicalSiblingIdx == invalidSiblingIndex) return;
            if (!valueIdentityStamp) {
                if (auto * target = peekResolvedTarget(); target && target->type() == nList)
                    valueIdentityStamp = cache->state.lookupValueIdentityStamp(*target);
            }
            cache->state.registerMaterializedValueIdentity(
                &v,
                cache->runtime_.get(),
                std::optional<SiblingIdentity>(SiblingIdentity{
                    .parentSlot = *slot,
                    .definitionStamp = *defStamp,
                    .slotStamp = *slotStampValue,
                    .canonicalSiblingIdx = canonicalSiblingIdx,
                }),
                pathId,
                valueIdentityStamp);
        } catch (...) {}
    }};

    // ── Coloring decision point ────────────────────────────────────
    //
    // EvalContext<Suspendable> cannot be constructed without an executor
    // (requires clause). Its existence IS the proof that strand dispatch
    // is possible. This is not a fallback — it separates two structurally
    // different capabilities at the type level.
    if (!ctx) {
        // Uncolored path: no executor → no strand dispatch → no verify/record.
        // No-backend path. Just force the real target.
        evaluateDirect(v);
        return;
    }

    // Colored path: EvalContext<Suspendable> exists → full pipeline.
    auto warmResult = cache->runtime_->verify(*ctx, pathId);

    if (verbosity >= lvlDebug) {
        debug("eval-trace/session: TracedExpr::eval '%s' verify %s", attrPathStr(),
            warmResult ? "HIT" : "MISS");
    }

    if (warmResult) {
        auto & [cachedValue, cachedTraceId] = *warmResult;
        ensureLazy().traceId = cachedTraceId;

        if (auto * failure = std::get_if<failed_t>(&cachedValue)) {
            if (!failure->errorMessage.empty()) {
                nrTraceCacheHits++;
                if (verbosity >= lvlDebug) {
                    debug("eval-trace/session: trace verify hit (cached error) for '%s'", attrPathStr());
                }
                throw Error("%s", failure->errorMessage);
            }
            evaluateFresh(*ctx, v);
            return;
        }

        if (auto * trivial = std::get_if<trivial_t>(&cachedValue);
            trivial
            && (trivial->kind == TrivialKind::Misc
             || trivial->kind == TrivialKind::Missing
             || trivial->kind == TrivialKind::Placeholder)) {
            evaluateFresh(*ctx, v);
            return;
        }

        nrTraceCacheHits++;
        if (verbosity >= lvlDebug) {
            debug("eval-trace/session: trace verify hit for '%s'", attrPathStr());
        }

        // Warm-hit branch. `materializeResult` populates `v` with
        // the cached value. `replayTrace` appends the cached trace's
        // deps to the global epoch log, but ONLY if a DepCaptureScope
        // is currently active — otherwise the appended deps would be
        // unreachable from any recording scope.
        //
        // Warm-hit asymmetry (OR-3 investigation, 2026-04-30): when
        // no DepCaptureScope is active, replayTrace is skipped.
        // `forceThunkValue`'s `recordThunkDeps(v, epochStart)` then
        // sees `epochStart == epochEnd` (no log growth during the
        // force) and creates no `epochMap` entry. Subsequent
        // `forceValue(v)` calls fall through `replayMemoizedDeps`'s
        // early return — neither replay nor
        // SiblingReplayCaptureScope capture fires. This is BENIGN
        // under the current architecture because cold re-eval uses
        // `navigateToReal`'s realRoot walk (fresh thunks), never the
        // materialized tree. See `source-tree-soundness.cc`'s
        // `WarmHit_Child_NoEpochMapEntry` +
        // `WarmHit_ThenSourceMutation_SiblingsStillInvalidate` tests.
        materializeResult(*ctx, v, cachedValue);
        RecordingScopeGuard::ifActive(
            [&](const EpochLogWriteProof<EpochLogWriteReason::ReplayActive> & proof) {
                replayTrace(*ctx, proof, cachedTraceId);
            });
        return;
    }

    // Verify miss -- fresh evaluation path
    nrTraceCacheMisses++;
    evaluateFresh(*ctx, v);
}

// ── TraceSession public API ───────────────────────────────────────────

TraceSession::TraceSession(
    std::optional<BackendParams> backendParams,
    EvalState & state,
    RootLoader rootLoader,
    boost::unordered_flat_map<DepSource, SourcePath, DepSource::Hash> inputAccessors,
    boost::unordered_flat_map<CanonPath, std::vector<std::pair<DepSource, RegistryMountSubdir>>> lockedMounts,
    std::vector<RootLoadDep> rootLoadDeps,
    SemanticRegistry registry)
    : runtime_()
    , state(state)
    , root(state, std::move(rootLoader))
    , inputAccessors(std::move(inputAccessors))
    , rootLoadDeps_(std::move(rootLoadDeps))
    , registry_(std::move(registry))
{
    // Populate mount points (reverse index) from the resolved flake graph.
    // The forward entries and mount points use the same node-key-based
    // DepSource identity, ensuring that deps recorded via reverseResolve()
    // during cold eval are resolvable by resolve() during warm verification.
    for (auto & [mountPoint, entries] : lockedMounts)
        for (auto & [source, subdir] : entries)
            registry_.addMountPoint(mountPoint, source, subdir);

    if (backendParams) {
        try {
            runtime_ = state.makeTraceBackend(backendParams->fingerprint);

            // RAII guard: reset runtime_ to null if any subsequent step
            // throws. Prevents null boundHandle_ crash (BUG-4). Released
            // only after full setup completes.
            bool setupComplete = false;
            auto resetGuard = std::unique_ptr<void, std::function<void(void*)>>(
                reinterpret_cast<void*>(1),
                [&](void*) {
                    if (!setupComplete)
                        runtime_.reset();
                });

            if (backendParams->sessionConfig)
                runtime_->setSessionConfig(std::move(*backendParams->sessionConfig));
            // Load and verify runtime roots from session metadata.
            // Verified roots are merged into the registry so dep verification
            // can resolve Registered sources for runtime-fetched inputs.
            // Failed roots are NOT registered — traces with deps referencing
            // those sources will fail verification naturally (resolver returns
            // nullopt → hash computation fails → trace falls to cold eval).
            auto runtimeResult = runtime_->loadAndVerifyRuntimeRoots(state);
            if (runtimeResult.verifiedRoots.size() < runtimeResult.expectedCount)
                debug("eval-trace/session: runtime root verification: %zu/%zu roots verified",
                    runtimeResult.verifiedRoots.size(), runtimeResult.expectedCount);
            for (auto & verified : runtimeResult.verifiedRoots)
                registry_.addEntry(verified.record.source, std::move(verified.rootPath));
            // Bind the per-session state to the verifier so it doesn't
            // need per-call reference parameters.
            runtime_->bindSession(registry_, state);
            debug("eval-trace/session: registry has %zu entries, %zu mount points",
                registry_.size(), registry_.mountPointCount());

            setupComplete = true;  // dismiss the guard
        } catch (std::exception & e) {
            nrTraceBackendSetupFailed++;
            ignoreExceptionExceptInterrupt();
        } catch (...) {
            nrTraceBackendSetupFailed++;
            ignoreExceptionExceptInterrupt();
        }
    }
}

void TraceSession::recordRootLoadDeps()
{
    if (rootLoadDeps_.empty())
        return;

    if (auto access = TraceAccess::current()) {
        for (const auto & dep : rootLoadDeps_)
            access->record(dep.kind, dep.source, dep.key, dep.hash);
        return;
    }

    auto * ctx = currentFiberDepCtx();
    if (!ctx)
        ctx = currentStandaloneDepCtx();
    if (!ctx || !ctx->isActive())
        return;

    for (const auto & dep : rootLoadDeps_)
        ctx->record(dep.kind, dep.source, dep.key, dep.hash);
}

Value * TraceSession::getRealRoot()
{
    recordRootLoadDeps();
    return root.getRealRoot();
}

Value * TraceSession::getRootValue()
{
    if (!state.hasTraceContext()) {
        return getRealRoot();
    }
    if (!tracedRoot) {
        auto * v = state.allocValue();
        v->mkThunk(&state.baseEnv, TracedExpr::makeRoot(*this));
        nrTracedExprCreated++;
        tracedRoot = allocRootValue(v);
    }
    return *tracedRoot;
}

void TraceSession::flush()
{
    if (runtime_)
        runtime_->flush();
}

void TraceSession::registerRuntimeRootMount(CanonPath mountPoint, DepSource source, RegistryMountSubdir subdir)
{
    registry_.addMountPoint(std::move(mountPoint), std::move(source), std::move(subdir));
}

void TraceSession::releaseBackend()
{
    if (runtime_)
        runtime_->flush();
    runtime_.reset();
    // Clear the traced root so the next getRootValue() creates a fresh
    // thunk against the new (null) runtime_. Without this, zombie
    // TracedExpr thunks from the old runtime_ would persist.
    tracedRoot.reset();
}

} // namespace nix::eval_trace
