#pragma once
///@file

#include "nix/util/sync.hh"
#include "nix/util/hash.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/eval-trace/result.hh"

#include <boost/unordered/unordered_flat_map.hpp>

#include <functional>

namespace nix::eval_trace {

// ── TraceCache ────────────────────────────────────────────────────────

struct TraceStore;
struct TracedExpr;

class TraceCache : public std::enable_shared_from_this<TraceCache>
{
    friend struct TracedExpr;

    /**
     * SQLite-based trace store (BSàlC constructive trace store).
     */
    std::shared_ptr<TraceStore> dbBackend;

    EvalState & state;
    typedef std::function<Value *()> RootLoader;
    RootLoader rootLoader;
    RootValue value;
    RootValue realRoot;

    /**
     * Maps flake input names to their source paths (accessor + base path).
     * Used during trace verification (BSàlC verifying trace check) to validate dep hashes
     * against current file content.
     */
    boost::unordered_flat_map<std::string, SourcePath> inputAccessors;

public:

    TraceCache(
        std::optional<std::reference_wrapper<const Hash>> useCache,
        EvalState & state,
        RootLoader rootLoader,
        boost::unordered_flat_map<std::string, SourcePath> inputAccessors = {});

    /**
     * Get the real root value via rootLoader, bypassing the trace system.
     * Used to regenerate GC'd .drv files by forcing fresh evaluation
     * (Adapton demand-driven recomputation).
     */
    Value * getOrEvaluateRoot();

    /**
     * Get the root value backed by TracedExpr thunks (Adapton articulation points).
     * On verify hit: returns a value materialized from the trace store.
     * On verify miss: returns a thunk that evaluates freshly via rootLoader.
     */
    Value * getRootValue();

};

} // namespace nix::eval_trace

// Backward-compatible: all counter declarations accessible via this header.
#include "nix/expr/eval-trace/counters.hh"
