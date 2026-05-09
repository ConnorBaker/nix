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

    void clearReplayIndex()
    {
        epochMap.clear();
        epochMap.rehash(0);
        replayBloom.reset();
    }

    void clear()
    {
        epochLog_.clear();
        clearReplayIndex();
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
};

} // namespace eval_trace

} // namespace nix
