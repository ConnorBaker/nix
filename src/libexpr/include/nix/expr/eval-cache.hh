#pragma once
///@file

#include "nix/util/sync.hh"
#include "nix/util/hash.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/file-load-tracker.hh"

#include <functional>
#include <map>
#include <variant>

namespace nix::eval_cache {

// ── Shared types (used by EvalCacheStore, ExprCached, serialization) ──

enum AttrType {
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
    AttrValue;

// ── EvalCache ────────────────────────────────────────────────────────

struct EvalCacheStore;
struct ExprCached;

class EvalCache : public std::enable_shared_from_this<EvalCache>
{
    friend struct ExprCached;

    /**
     * Store-based eval cache backend.
     */
    std::shared_ptr<EvalCacheStore> storeBackend;

    EvalState & state;
    typedef std::function<Value *()> RootLoader;
    RootLoader rootLoader;
    RootValue value;
    RootValue realRoot;

    /**
     * Maps flake input names to their source paths (accessor + base path).
     * Used for validating cached dep hashes against current file content.
     */
    std::map<std::string, SourcePath> inputAccessors;

public:

    EvalCache(
        std::optional<std::reference_wrapper<const Hash>> useCache,
        EvalState & state,
        RootLoader rootLoader,
        std::map<std::string, SourcePath> inputAccessors = {});

    /**
     * Get the real root value via rootLoader, bypassing the cache.
     * Used to regenerate GC'd .drv files by forcing fresh evaluation.
     */
    Value * getOrEvaluateRoot();

    /**
     * Get the root value backed by ExprCached thunks.
     * On warm cache: returns a value materialized from the store.
     * On cold cache: returns a thunk that evaluates via rootLoader.
     */
    Value * getRootValue();
};

/**
 * Eval cache performance counters, active when NIX_SHOW_STATS is set.
 */
extern Counter nrEvalCacheHits;
extern Counter nrEvalCacheMisses;
extern Counter nrDepValidations;
extern Counter nrDepValidationsPassed;
extern Counter nrDepValidationsFailed;
extern Counter nrDepsChecked;
extern Counter nrCacheVerificationFailures;

} // namespace nix::eval_cache
