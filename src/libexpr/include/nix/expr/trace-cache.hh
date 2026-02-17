#pragma once
///@file

#include "nix/util/sync.hh"
#include "nix/util/hash.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/dep-tracker.hh"

#include <functional>
#include <map>
#include <variant>

namespace nix::eval_trace {

// ── Shared types (used by TraceStore, TracedExpr, serialization) ────

enum ResultKind {
    Placeholder = 0,
    FullAttrs = 1,
    String = 2,
    Missing = 3,
    Misc = 4,
    Failed = 5,
    Bool = 6,
    ListOfStrings = 7,
    Int = 8,
    Path = 9,
    Null = 10,
    Float = 11,
    List = 12,
};

struct placeholder_t {};
struct missing_t {};
struct misc_t {};
struct failed_t {};

struct int_t { NixInt x; };
struct path_t { std::string path; };
struct null_t {};
struct float_t { double x; };
struct list_t { size_t size; };

typedef uint64_t AttrId;
typedef std::pair<AttrId, Symbol> AttrKey;
typedef std::pair<std::string, NixStringContext> string_t;

typedef std::variant<
    std::vector<Symbol>,
    string_t,
    placeholder_t,
    missing_t,
    misc_t,
    failed_t,
    bool,
    int_t,
    std::vector<std::string>,
    path_t,
    null_t,
    float_t,
    list_t>
    CachedResult;

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
    std::map<std::string, SourcePath> inputAccessors;

public:

    TraceCache(
        std::optional<std::reference_wrapper<const Hash>> useCache,
        EvalState & state,
        RootLoader rootLoader,
        std::map<std::string, SourcePath> inputAccessors = {});

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

/**
 * Eval trace performance counters, active when NIX_SHOW_STATS is set.
 */
extern Counter nrTraceCacheHits;
extern Counter nrTraceCacheMisses;
extern Counter nrTraceVerifications;
extern Counter nrVerificationsPassed;
extern Counter nrVerificationsFailed;
extern Counter nrDepsChecked;
extern Counter nrRecoveryFailures;

} // namespace nix::eval_trace
