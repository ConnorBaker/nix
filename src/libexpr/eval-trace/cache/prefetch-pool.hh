#pragma once
/// prefetch-pool.hh — Speculative prefetch pool for verification hints.
///
/// When an eval coroutine materializes an attrset (wrapping children as
/// TracedExpr thunks), it knows all sibling attr paths. Before any child
/// is accessed, it can submit prefetch hints to the Verification
/// Orchestrator.
///
/// The pool manages in-flight prefetch futures. When an eval coroutine
/// actually requests verification, the pool checks whether a prefetch is
/// already in-flight and returns the existing future instead of starting a
/// new verification.
///
/// Bounded: max outstanding prefetches per pool (configurable).
/// Overflow is silently dropped (falls back to on-demand verification).

#include "nix/expr/eval-trace/ids.hh"
#include "nix/expr/eval-trace/store/sqlite-trace-storage.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace nix::eval_trace {

/// Token representing an in-flight or completed prefetch.
struct PrefetchToken {
    AttrPathId pathId;
    bool completed = false;
    std::optional<SqliteTraceStorage::VerifyResult> result;
};

/// Pool of speculative prefetch operations.
///
/// Thread-safety: The pool is accessed only from the verify_strand
/// (owned by Verifier). No synchronization needed.
class PrefetchPool {
public:
    explicit PrefetchPool(uint32_t maxOutstanding = 16)
        : maxOutstanding_(maxOutstanding)
    {
    }

    /// Check whether a prefetch is in-flight or completed for this path.
    PrefetchToken * lookup(AttrPathId pathId)
    {
        auto it = tokens_.find(pathId);
        return it != tokens_.end() ? &it->second : nullptr;
    }

    /// Submit a new prefetch hint. Returns false if the pool is full
    /// (caller should fall back to on-demand verification).
    bool submit(AttrPathId pathId)
    {
        if (tokens_.size() >= maxOutstanding_)
            return false;
        if (tokens_.count(pathId))
            return true; // already in-flight
        tokens_.emplace(pathId, PrefetchToken{pathId});
        return true;
    }

    /// Mark a prefetch as completed with a result.
    void complete(AttrPathId pathId, std::optional<SqliteTraceStorage::VerifyResult> result)
    {
        auto it = tokens_.find(pathId);
        if (it != tokens_.end()) {
            it->second.completed = true;
            it->second.result = std::move(result);
        }
    }

    /// Remove a completed prefetch from the pool.
    void remove(AttrPathId pathId)
    {
        tokens_.erase(pathId);
    }

    /// Clear all prefetches (e.g., on session reset).
    void clear()
    {
        tokens_.clear();
    }

    uint32_t size() const { return static_cast<uint32_t>(tokens_.size()); }
    uint32_t maxOutstanding() const { return maxOutstanding_; }

private:
    uint32_t maxOutstanding_;
    boost::unordered_flat_map<AttrPathId, PrefetchToken, AttrPathId::Hash> tokens_;
};

} // namespace nix::eval_trace
