#pragma once
///@file

#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/util/pointer-bloom-filter.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <cassert>
#include <optional>
#include <vector>

namespace nix {

namespace eval_trace {

struct MemoReplayStore {
    /// Owned epoch log vector. All fibers' DepRecordingContexts reference
    /// this vector (per-EvalState, long-lived). DepRange structs point to
    /// &epochLog_. With cooperative scheduling on one carrier thread, no
    /// concurrent writes.
    std::vector<Dep> epochLog_;
    /// Value* keys point into the GC heap. The replay map is long-lived for
    /// the EvalState trace runtime because later traced child evaluations may
    /// need deps from thunks forced by earlier parents. Invisible keys could
    /// let Boehm reuse a Value address and make getReplayRange() return an
    /// unrelated stale DepRange. The traceable allocator intentionally keeps
    /// keyed Values live until TraceRuntime::reset()/MemoReplayStore::clear().
    /// Do not replace it with an untraced allocator for speed; shorten the
    /// replay-store lifetime only with a proof that no later trace can need
    /// those memoized deps.
    boost::unordered_flat_map<
        const Value *,
        DepRange,
        boost::hash<const Value *>,
        std::equal_to<const Value *>,
        traceable_allocator<std::pair<const Value * const, DepRange>>>
        epochMap;
    PointerBloomFilter<1 << 23, 16> replayBloom;

    /// Producer side-table: maps a forced derivation result's `Bindings *`
    /// to its CA producer trace identity. Populated when a producer-trace
    /// boundary finalizes (RFC §3b); consulted in `replayMemoizedDeps` to
    /// route between flatten-replay and edge-emission.
    ///
    /// **Why `Bindings*` and not `Value*`** — primops are dispatched as
    /// `fn->impl(state, pos, args, vCur)` where `vCur` is `callFunction`'s
    /// stack-local Value. After `callFunction` returns, `vRes = vCur`
    /// COPIES the value into the caller's slot (eval.cc:2530); `&vCur`
    /// becomes stale and is reused by the next callFunction invocation.
    /// Subsequent `forceValue(vRes)` uses `&vRes` — a different pointer.
    /// Keying by `&v` would make every gate lookup miss.
    ///
    /// `Bindings *` is stable across `Value` copies: `Value::mkAttrs(b)`
    /// stores `b` in the Value's payload, so copies share the same
    /// `Bindings *`. For derivation results (always attrsets), the
    /// `Bindings *` is the stable content-identity across the Value's
    /// lifetime in the eval graph.
    ///
    /// Same lifetime + traceable_allocator concerns as `epochMap`:
    /// keyed `Bindings *`s must stay live across GC. Independent of
    /// `epochMap`.
    struct ProducerEntry {
        AttrPathId caKey;
        DepHash traceHash;
    };
    boost::unordered_flat_map<
        const Bindings *,
        ProducerEntry,
        boost::hash<const Bindings *>,
        std::equal_to<const Bindings *>,
        traceable_allocator<std::pair<const Bindings * const, ProducerEntry>>>
        producerMap;

    /// Pointer bloom for fast-rejecting `lookupProducer` calls on the hot
    /// `replayMemoizedDeps` path. Sized for ~thousands of producers (one
    /// per `derivationStrict` call). Template params are
    /// `<Bits, PointerAlignment>`: 65536 bits ≈ 8 KB; alignment 16 matches
    /// Boehm-GC Value allocation (mirrors `replayBloom`'s alignment, since
    /// both filters key on the SAME GC-allocated `Value*` keys). The
    /// filter is k=2 (hard-coded in PointerBloomFilter); FPR at n=6,419
    /// producers in m=65,536 bits is ≈ 3% — net win on the ~22M-call hot
    /// path because the bloom-then-find combo skips the
    /// `unordered_flat_map::find` for ~97% of non-producer Values, and
    /// `find` would otherwise hash + probe a group on every miss.
    PointerBloomFilter<1 << 16, 16> producerBloom;

    /// Reset the per-replay state (epoch index + bloom). Called from
    /// `rollbackEpoch` when the epoch log is fully unwound, and from the
    /// `_ForTest` access shim. Deliberately does NOT touch `producerMap`:
    /// producer bindings (RFC §3b) are scoped to the trace-runtime's
    /// lifetime, not to a single replay window. Clearing them on a
    /// rollback-empty path would erase legitimate producer registrations
    /// from earlier successful `derivationStrict` calls. Tests that need
    /// producerMap reset call `clearProducerMap` explicitly.
    void clearReplayIndex()
    {
        epochMap.clear();
        epochMap.rehash(0);
        replayBloom.reset();
    }

    /// Reset the producer side-table. Called from `clear()` (full lifecycle
    /// reset) and from test fixtures that need a fresh slate per-test.
    /// NOT called from `clearReplayIndex` — see that method's comment.
    void clearProducerMap()
    {
        producerMap.clear();
        producerMap.rehash(0);
        producerBloom.reset();
    }

    void clear()
    {
        epochLog_.clear();
        clearReplayIndex();
        clearProducerMap();
    }

    MemoReplayStore() = default;

    uint32_t epochSize() const
    {
        return epochLog_.size();
    }

    const std::vector<Dep> & epochEntriesForTest() const
    {
        return epochLog_;
    }

    /// Record the epoch-log range grown during a thunk force.
    ///
    /// Called from `ReplayPublishScope` in `forceThunkValue`
    /// (eval.cc:1693). When `epochStart == epochEnd`, no entry is
    /// created — nothing grew the log during the force.
    ///
    /// Warm-hit asymmetry (OR-3 investigation, 2026-04-30): warm-hit
    /// of a TracedExpr thunk through `TracedExpr::eval` →
    /// `materializeResult` does not grow the log (replayTrace is
    /// gated on an active DepCaptureScope; materialization itself
    /// doesn't append). So warm-hit forces of TracedExpr thunks
    /// produce NO epochMap entry for the Value slot. Downstream:
    /// `replayMemoizedDeps(v)` at context.cc:821 returns early when
    /// `replayBloom.test(&v)` is false, skipping both replay copy AND
    /// `SiblingReplayCaptureScope::maybeCapture`.
    ///
    /// This asymmetry is benign in the current architecture because
    /// cold re-eval traverses `realRoot`'s fresh thunks via
    /// `navigateToReal`; warm-materialized Values are never on that
    /// walk. If a refactor makes cold re-eval reach materialized
    /// Values, the asymmetry becomes a soundness hole (missing deps
    /// or missing TraceValueContext in the enclosing trace). See
    /// `src/libexpr-tests/eval-trace/dep/source-tree-soundness.cc`'s
    /// `WarmHit_Child_NoEpochMapEntry` +
    /// `WarmHit_ThenSourceMutation_SiblingsStillInvalidate` for
    /// regression guards.
    void recordThunkDeps(const Value & v, uint32_t epochStart)
    {
        uint32_t epochEnd = epochLog_.size();
        if (epochStart < epochEnd) {
            // insert_or_assign, not emplace: if GC recycled this Value*
            // address, the stale epochMap entry from the old Value must be
            // overwritten with the new (correct) DepRange. emplace is a
            // no-op on duplicate keys, silently retaining the stale entry
            // and losing the new one (BUG-7: GC address reuse hazard).
            epochMap.insert_or_assign(&v, DepRange{&epochLog_, epochStart, epochEnd});
            replayBloom.set(&v);
        }
    }

    /// Rollback the epoch log to `epochStart`, dropping any `epochMap` entries
    /// whose ranges fall in (or cross) the rolled-back region. Called from
    /// exception-unwind paths (e.g., `tryEval`-swallowed errors).
    ///
    /// Intentional asymmetry vs `producerMap`: rollback does NOT scrub
    /// producer bindings. The §3b producer-trace hook in
    /// `prim_derivationStrict` registers a producer only AFTER
    /// `derivationStrictInternal` returns successfully (the catch block
    /// re-throws without registering). So a rolled-back force never had a
    /// producer registration to scrub, and the rollback path can't reach
    /// stale bindings. If a future caller registers a producer BEFORE its
    /// success-or-throw decision is final, this asymmetry must be revisited
    /// — the rollback would need to scrub `producerMap` entries whose
    /// epoch-range falls in the rolled-back region (analogous to BUG-8 for
    /// the epoch log itself).
    void rollbackEpoch(uint32_t epochStart)
    {
        if (epochStart >= epochLog_.size())
            return;

        epochLog_.erase(epochLog_.begin() + epochStart, epochLog_.end());

        for (auto it = epochMap.begin(); it != epochMap.end();) {
            if (it->second.start >= epochStart || it->second.end > epochStart)
                it = epochMap.erase(it);
            else
                ++it;
        }
        if (epochMap.empty())
            clearReplayIndex();
    }

    std::optional<DepRange> getReplayRange(const Value & v) const
    {
        if (!replayBloom.test(&v))
            return {};
        auto it = epochMap.find(&v);
        if (it == epochMap.end())
            return {};
        return it->second;
    }

    /// Bind a forced derivation result's `Bindings *` to its CA producer
    /// trace identity. Called from the producer-trace boundary finalization
    /// (RFC §3b, `prim_derivationStrict`) after `Recorder::record` publishes
    /// the producer trace. Subsequent `lookupProducer(b)` calls return the
    /// binding; `replayMemoizedDeps` will emit a single edge dep targeting
    /// the CA key instead of copying the producer's flattened deps.
    ///
    /// Keyed by `Bindings *` (not `Value *`) for stability across the
    /// `vRes = vCur` copy in `callFunction` — see `producerMap` doc above.
    ///
    /// Uses `insert_or_assign` for the BUG-7 GC-address-reuse hazard: if a
    /// Bindings* is reclaimed and reused, the stale binding must be
    /// overwritten by the new one.
    void registerProducer(const Bindings * b, AttrPathId caKey, DepHash traceHash)
    {
        producerMap.insert_or_assign(b, ProducerEntry{caKey, traceHash});
        producerBloom.set(b);
    }

    /// Hot-path call site: `replayMemoizedDeps` invokes `lookupProducer`
    /// before its bloom-gated `getReplayRange`. closures.gnome fires
    /// `replayMemoizedDeps` ~22M times across ~thousands of registered
    /// producers. The bloom rejects ~97% of non-attrset/non-producer Values
    /// without paying the `unordered_flat_map::find` cost.
    ///
    /// The caller passes `v.attrs()` for attrset Values (the only kind a
    /// derivation result has); other Values short-circuit to nullopt before
    /// this is called.
    std::optional<ProducerEntry> lookupProducer(const Bindings * b) const
    {
        if (!producerBloom.test(b)) [[likely]]
            return {};
        auto it = producerMap.find(b);
        if (it == producerMap.end())
            return {};
        return it->second;
    }

    size_t producerMapSize() const { return producerMap.size(); }
};

} // namespace eval_trace

} // namespace nix
