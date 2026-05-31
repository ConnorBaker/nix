#pragma once
/// store/recorder.hh — Constructive-trace recording pipeline
/// (rearchitecture-proposal.md §14 step 7 + §2.3).
///
/// Drives the sort+dedup → hash → serialise → intern → publish
/// sequence for a single `(pathId, CachedResult, deps)` tuple. Privately
/// inherits `VocabAwareHasher` (EBO-safe) so `feedKey` dispatch is
/// shared with `Verifier` without duplicating the pools/vocab
/// references. The remaining state is a `SqliteTraceStorage &`; the
/// backend befriends `Recorder` so the body can reach private helpers
/// (`getOrCreateDepKeySet`, `doInternResult`, `getOrCreateTrace`,
/// `publishRecord`, `flush`) directly.
///
/// Supports an optional `TraceObserver` that receives
/// `onNewTrace` + `onPublishCurrent` after publish returns; this is the
/// hook verification-owned caches will consume in a later phase.

#include "nix/expr/eval-trace/deps/interning-pools.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/store/attr-vocab-store.hh"
#include "nix/expr/eval-trace/store/trace-storage.hh"
#include "nix/expr/eval-trace/store/trace-value-types.hh"
#include "nix/expr/eval-trace/store/vocab-aware-hasher.hh"

#include <vector>

namespace nix::eval_trace {

struct SqliteTraceStorage;

/// Orchestrates the record pipeline. Instances are cheap — one
/// stack-allocated `Recorder` per `record()` call.
class Recorder : private VocabAwareHasher {
public:
    Recorder(SqliteTraceStorage & storage, InterningPools & pools,
             AttrVocabStore & vocab) noexcept;

    Recorder(const Recorder &) = delete;
    Recorder & operator=(const Recorder &) = delete;

    /// Record a fresh evaluation result with its dependencies.
    ///
    /// Drives the full BSàlC constructive-trace recording pipeline:
    /// sort+dedup → hash → serialise → intern → publish. If `observer`
    /// is non-null, fires `onNewTrace` and `onPublishCurrent` after
    /// the backend publish returns so downstream caches can mirror the
    /// backend's state atomically.
    /// `deferFlush` (async-producer-recording-plan Layer 1): when true, SKIP the
    /// per-record `storage_.flush(ea)` (step 7). Entities still BUFFER in `pending*`
    /// and are drained by the next non-deferred flush or the destructor's
    /// `flushExclusive()`; in-memory caches (which within-session verify +
    /// `getCurrentTraceHash` read) are still updated synchronously via
    /// `publishRecord`. Used by `recordSync` for producer traces to avoid N
    /// checkpoints+COMMITs (one per producer). Env-gated at the call site.
    RecordResult record(const ExclusiveTraceStorageAccess & ea,
                        AttrPathId pathId,
                        const CachedResult & value,
                        const std::vector<Dep> & allDeps,
                        TraceObserver * observer = nullptr,
                        bool deferFlush = false);

private:
    SqliteTraceStorage & storage_;
    // pools_ + vocab_ live in VocabAwareHasher (accessed via
    // `hasherPools()` / `hasherVocab()` / `feedKey`).
};

} // namespace nix::eval_trace
