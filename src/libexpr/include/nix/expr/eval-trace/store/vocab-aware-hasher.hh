#pragma once
/// store/vocab-aware-hasher.hh — EBO-safe mixin supplying vocab-aware
/// dep-key hashing via `feedKey`.
///
/// `feedKey(builder, key)` is a pure dep-key hash feeder that closes
/// over an `InterningPools` and an `AttrVocabStore` — the recording
/// path (Recorder) and the verification path (Verifier, via
/// `computePresortedTraceHash`) both need it. Rather than have each
/// class store its own pair of references and hand-roll `feedKey`, they
/// privately inherit this mixin and get `feedKey` for free.
///
/// Used today by `TraceStore` (via private inheritance); when the
/// rearch lands the Recorder and Verifier will use the same mixin.
///
/// Step 5 of rearchitecture-proposal.md §14 — §2.3.1 "VocabAwareHasher
/// mixin" describes the intended shape.

#include "nix/expr/eval-trace/canonical-hash.hh"
#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/store/attr-vocab-store.hh"

namespace nix::eval_trace {

/// EBO-safe mixin: takes two references at construction and exposes a
/// `feedKey` hook that dispatches on whether the dep key is a
/// trace-context (attr-path) key or a structured/simple key.
/// Concrete users privately inherit and get `feedKey` as a protected
/// inline member. Zero-cost under EBO — the stored refs are the only
/// runtime cost, and the shared implementation has one definition.
class VocabAwareHasher {
    InterningPools & pools_;
    AttrVocabStore & vocab_;

protected:
    VocabAwareHasher(InterningPools & pools, AttrVocabStore & vocab) noexcept
        : pools_(pools), vocab_(vocab) {}

    InterningPools & hasherPools() noexcept { return pools_; }
    const InterningPools & hasherPools() const noexcept { return pools_; }
    AttrVocabStore & hasherVocab() const noexcept { return vocab_; }

    void feedKey(CanonicalHashBuilder & builder, const Dep::Key & key) const
    {
        if (key.isTraceContext()) {
            builder.field("dep.key.kind", key.kind);
            vocab_.feedPath(builder, key.attrPathId);
            return;
        }
        feedCanonicalDepKeyMaterial(builder, pools_, key);
    }
};

} // namespace nix::eval_trace
