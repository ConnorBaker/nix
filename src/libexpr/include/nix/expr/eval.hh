#pragma once
///@file

#include "nix/expr/attr-set.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-profiler.hh"
#include "nix/util/types.hh"
#include "nix/expr/value.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/configuration.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/position.hh"
#include "nix/util/pos-table.hh"
#include "nix/util/source-accessor.hh"
#include "nix/expr/search-path.hh"
#include "nix/expr/repl-exit-status.hh"
#include "nix/expr/eval-trace/ids.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/types.hh"
#include "nix/expr/eval-trace/result.hh"
#include "nix/expr/eval-trace/semantic-objects.hh"
#include "nix/util/ref.hh"
#include "nix/expr/counter.hh"

// For `NIX_USE_BOEHMGC`, and if that's set, `GC_THREADS`
#include "nix/expr/config.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/concurrent_flat_map_fwd.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>
#include <span>
#include <variant>
#include <vector>

namespace nix {

/**
 * We put a limit on primop arity because it lets us use a fixed size array on
 * the stack. 8 is already an impractical number of arguments. Use an attrset
 * argument for such overly complicated functions.
 */
constexpr size_t maxPrimOpArity = 8;

class Store;

namespace fetchers {
struct Settings;
struct InputCache;
struct Input;
} // namespace fetchers
struct EvalSettings;
struct EvalEnvironmentAuthority;
class EvalState;
class StorePath;
struct SingleDerivedPath;
enum RepairFlag : bool;
struct MemorySourceAccessor;
struct MountedSourceAccessor;
struct InterningPools;
struct NixBindingEntry;
struct PublishedMaterializedIdentity;

namespace eval_trace {
class TraceSession;
struct AttrVocabStore;
class TraceBackend;
struct TracedExpr;
template<typename> class EvalContext;
struct Suspendable;
struct Critical;
}

struct TraceRuntime;

struct FileTraceCacheKey
{
    SourcePath path;
    SourcePath displayPath;
    SourcePath basePath;
    bool mustBeTrivial = false;
    std::optional<PathObject> origin;

    bool operator==(const FileTraceCacheKey &) const = default;

    struct Hash {
        size_t operator()(const FileTraceCacheKey & key) const noexcept
        {
            std::size_t hash = hashValues(key.path, key.displayPath, key.basePath, key.mustBeTrivial);
            if (key.origin) {
                boost::hash_combine(hash, true);
                boost::hash_combine(hash, DepSource::Hash{}(key.origin->source));
                boost::hash_combine(hash, key.origin->rootPath);
            } else {
                boost::hash_combine(hash, false);
            }
            return hash;
        }
    };
};

struct SrcToStoreCacheKey
{
    SourcePath path;
    const Store * store = nullptr;

    bool operator==(const SrcToStoreCacheKey &) const = default;

    struct Hash
    {
        size_t operator()(const SrcToStoreCacheKey & key) const
        {
            size_t hash = std::hash<SourcePath>{}(key.path);
            boost::hash_combine(hash, key.store);
            return hash;
        }
    };
};

using SrcToStoreCache = boost::concurrent_flat_map<
    SrcToStoreCacheKey,
    StorePath,
    SrcToStoreCacheKey::Hash>;
using ImportResolutionCache = boost::concurrent_flat_map<SourcePath, SourcePath>;
using FileTraceValueCache = boost::concurrent_flat_map<
    FileTraceCacheKey,
    Value *,
    FileTraceCacheKey::Hash,
    std::equal_to<FileTraceCacheKey>,
    traceable_allocator<std::pair<const FileTraceCacheKey, Value *>>>;

/// Session-lifetime cache mapping a (accessor, path) pair to its
/// eval-trace content hash.
///
/// Eliminates redundant content-hash computation for files read many times
/// during a single eval (e.g., nixpkgs/all-packages.nix accessed per
/// binding).
///
/// Keyed on `SourcePath` (not `CanonPath`) so that two files with the
/// same canonical path but backed by different accessors (e.g., two
/// flake inputs mounted at colliding paths) do not share a cache
/// entry. `SourcePath`'s hash includes `accessor->number`.
///
/// CONTRACT: Files are assumed stable for the lifetime of this cache.
/// If file bytes change after the first access that populates this
/// cache, subsequent readers of this cache will see the first-observed
/// hash (NOT the current bytes' hash). File-change-during-eval is
/// already undefined behavior in Nix's evaluation model; this cache
/// documents and embraces that invariant rather than defending against
/// it per-access.
using FileContentHashCache = boost::concurrent_flat_map<SourcePath, DepHash>;

struct EvalEnvironmentSharedState
{
    const ref<fetchers::InputCache> inputCache;
    const ref<SrcToStoreCache> srcToStore;
    const ref<ImportResolutionCache> importResolutionCache;
    const ref<FileTraceValueCache> fileTraceCache;
    /// Session-wide cache of file content hashes. See FileContentHashCache
    /// docstring for the stability contract.
    const ref<FileContentHashCache> fileContentHashCache;
    /// Cache of lookup-path resolutions keyed by the raw path string.
    /// Concurrent to match master's thread-safety model for EvalState caches.
    const ref<boost::concurrent_flat_map<std::string, std::optional<SourcePath>>> lookupPathResolved;
    std::mutex retainedPathAccessorsMutex;
    std::vector<ref<SourceAccessor>> retainedPathAccessors;

    EvalEnvironmentSharedState();
};

/**
 * Increments a count on construction and decrements on destruction.
 */
class CallDepth
{
    size_t & count;

public:
    CallDepth(size_t & count)
        : count(count)
    {
        ++count;
    }

    ~CallDepth()
    {
        --count;
    }
};

/**
 * Function that implements a primop.
 */
using PrimOpFun = void(EvalState & state, const PosIdx pos, Value ** args, Value & v);

/**
 * Info about a primitive operation, and its implementation
 */
struct PrimOp
{
    /**
     * Name of the primop. `__` prefix is treated specially.
     */
    std::string name;

    /**
     * Names of the parameters of a primop, for primops that take a
     * fixed number of arguments to be substituted for these parameters.
     */
    std::vector<std::string> args;

    /**
     * Aritiy of the primop.
     *
     * If `args` is not empty, this field will be computed from that
     * field instead, so it doesn't need to be manually set.
     */
    size_t arity = 0;

    /**
     * Optional free-form documentation about the primop.
     */
    std::optional<std::string> doc;

    /**
     * Add a trace item, while calling the `<name>` builtin.
     *
     * This is used to remove the redundant item for `builtins.addErrorContext`.
     */
    bool addTrace = true;

    /**
     * Implementation of the primop.
     */
    fun<PrimOpFun> impl;

    /**
     * Optional experimental for this to be gated on.
     */
    std::optional<ExperimentalFeature> experimentalFeature;

    /**
     * If true, this primop is not exposed to the user.
     */
    bool internal = false;

    /**
     * Validity check to be performed by functions that introduce primops,
     * such as RegisterPrimOp() and Value::mkPrimOp().
     */
    void check();
};

std::ostream & operator<<(std::ostream & output, const PrimOp & primOp);

/**
 * Info about a constant
 */
struct Constant
{
    /**
     * Optional type of the constant (known since it is a fixed value).
     *
     * @todo we should use an enum for this.
     */
    ValueType type = nThunk;

    /**
     * Optional free-form documentation about the constant.
     */
    const char * doc = nullptr;

    /**
     * Whether the constant is impure, and not available in pure mode.
     */
    bool impureOnly = false;
};

typedef std::
    map<std::string, Value *, std::less<std::string>, traceable_allocator<std::pair<const std::string, Value *>>>
        ValMap;

typedef boost::unordered_flat_map<PosIdx, DocComment, std::hash<PosIdx>> DocCommentMap;

struct Env
{
    Env * up;
    Value * values[0];
};

void printEnvBindings(const EvalState & es, const Expr & expr, const Env & env);
void printEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env, int lvl = 0);

std::unique_ptr<ValMap> mapStaticEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env);

void copyContext(
    const Value & v,
    NixStringContext & context,
    const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

std::string printValue(EvalState & state, Value & v);
std::ostream & operator<<(std::ostream & os, const ValueType t);

struct RegexCache;

ref<RegexCache> makeRegexCache();

struct DebugTrace
{
    /* WARNING: Converting PosIdx -> Pos should be done with extra care. This is
       due to the fact that operator[] of PosTable is incredibly expensive. */
    std::variant<Pos, PosIdx> pos;
    const Expr & expr;
    const Env & env;
    HintFmt hint;
    bool isError;

    Pos getPos(const PosTable & table) const
    {
        return std::visit(
            overloaded{
                [&](PosIdx idx) {
                    // Prefer direct pos, but if noPos then try the expr.
                    if (!idx)
                        idx = expr.getPos();
                    return table[idx];
                },
                [&](Pos pos) { return pos; },
            },
            pos);
    }
};

struct StaticEvalSymbols
{
    Symbol with, outPath, drvPath, type, meta, name, value, system, overrides, outputs, outputName, ignoreNulls, file,
        line, column, functor, toString, right, wrong, structuredAttrs, json, allowedReferences, allowedRequisites,
        disallowedReferences, disallowedRequisites, maxSize, maxClosureSize, builder, args, contentAddressed, impure,
        outputHash, outputHashAlgo, outputHashMode, recurseForDerivations, description, self, epsilon, startSet,
        operator_, key, path, prefix, outputSpecified;

    Expr::AstSymbols exprSymbols;

    static constexpr auto preallocate()
    {
        StaticSymbolTable alloc;

        StaticEvalSymbols staticSymbols = {
            .with = alloc.create("<with>"),
            .outPath = alloc.create("outPath"),
            .drvPath = alloc.create("drvPath"),
            .type = alloc.create("type"),
            .meta = alloc.create("meta"),
            .name = alloc.create("name"),
            .value = alloc.create("value"),
            .system = alloc.create("system"),
            .overrides = alloc.create("__overrides"),
            .outputs = alloc.create("outputs"),
            .outputName = alloc.create("outputName"),
            .ignoreNulls = alloc.create("__ignoreNulls"),
            .file = alloc.create("file"),
            .line = alloc.create("line"),
            .column = alloc.create("column"),
            .functor = alloc.create("__functor"),
            .toString = alloc.create("__toString"),
            .right = alloc.create("right"),
            .wrong = alloc.create("wrong"),
            .structuredAttrs = alloc.create("__structuredAttrs"),
            .json = alloc.create("__json"),
            .allowedReferences = alloc.create("allowedReferences"),
            .allowedRequisites = alloc.create("allowedRequisites"),
            .disallowedReferences = alloc.create("disallowedReferences"),
            .disallowedRequisites = alloc.create("disallowedRequisites"),
            .maxSize = alloc.create("maxSize"),
            .maxClosureSize = alloc.create("maxClosureSize"),
            .builder = alloc.create("builder"),
            .args = alloc.create("args"),
            .contentAddressed = alloc.create("__contentAddressed"),
            .impure = alloc.create("__impure"),
            .outputHash = alloc.create("outputHash"),
            .outputHashAlgo = alloc.create("outputHashAlgo"),
            .outputHashMode = alloc.create("outputHashMode"),
            .recurseForDerivations = alloc.create("recurseForDerivations"),
            .description = alloc.create("description"),
            .self = alloc.create("self"),
            .epsilon = alloc.create(""),
            .startSet = alloc.create("startSet"),
            .operator_ = alloc.create("operator"),
            .key = alloc.create("key"),
            .path = alloc.create("path"),
            .prefix = alloc.create("prefix"),
            .outputSpecified = alloc.create("outputSpecified"),
            .exprSymbols = {
                .sub = alloc.create("__sub"),
                .lessThan = alloc.create("__lessThan"),
                .mul = alloc.create("__mul"),
                .div = alloc.create("__div"),
                .or_ = alloc.create("or"),
                .findFile = alloc.create("__findFile"),
                .nixPath = alloc.create("__nixPath"),
                .body = alloc.create("body"),
            }};

        return std::pair{staticSymbols, alloc};
    }

    static consteval StaticEvalSymbols create()
    {
        return preallocate().first;
    }

    static constexpr StaticSymbolTable staticSymbolTable()
    {
        return preallocate().second;
    }
};

class EvalMemory
{
public:
    struct Statistics
    {
        Counter nrEnvs;
        Counter nrValuesInEnvs;
        Counter nrValues;
        Counter nrAttrsets;
        Counter nrAttrsInAttrsets;
        Counter nrListElems;
    };

    EvalMemory();

    EvalMemory(const EvalMemory &) = delete;
    EvalMemory(EvalMemory &&) = delete;
    EvalMemory & operator=(const EvalMemory &) = delete;
    EvalMemory & operator=(EvalMemory &&) = delete;

    inline void * allocBytes(size_t n);
    inline Value * allocValue();
    inline Env & allocEnv(size_t size);

    Bindings * allocBindings(
        size_t capacity,
        EmptyBindingsAllocation emptyAllocation = EmptyBindingsAllocation::ReuseSharedEmpty);

    BindingsBuilder buildBindings(
        SymbolTable & symbols,
        size_t capacity,
        EmptyBindingsAllocation emptyAllocation = EmptyBindingsAllocation::ReuseSharedEmpty)
    {
        return BindingsBuilder(*this, symbols, allocBindings(capacity, emptyAllocation), capacity);
    }

    ListBuilder buildList(size_t size, bool forceHeap = false)
    {
        stats.nrListElems += size;
        return ListBuilder(*this, size, forceHeap);
    }

    const Statistics & getStats() const &
    {
        return stats;
    }

    /**
     * Storage for the AST nodes
     */
    Exprs exprs;

private:
    Statistics stats;
};

class EvalState : public std::enable_shared_from_this<EvalState>
{
public:
    static constexpr StaticEvalSymbols s = StaticEvalSymbols::create();

    const fetchers::Settings & fetchSettings;
    const EvalSettings & settings;

    SymbolTable symbols;
    PosTable positions;

    EvalMemory mem;

    /**
     * If set, force copying files to the Nix store even if they
     * already exist there.
     */
    RepairFlag repair;

    /**
     * The accessor corresponding to `store`.
     */
    const ref<MountedSourceAccessor> storeFS;

    /**
     * The accessor for the root filesystem.
     */
    const ref<SourceAccessor> rootFS;

    /**
     * The in-memory filesystem for <nix/...> paths.
     */
    const ref<MemorySourceAccessor> corepkgsFS;

    /**
     * In-memory filesystem for internal, non-user-callable Nix
     * expressions like `derivation.nix`.
     */
    const ref<MemorySourceAccessor> internalFS;

    const SourcePath derivationInternal;

    /**
     * Store used to materialise .drv files.
     */
    const ref<Store> store;

    /**
     * Store used to build stuff.
     */
    const ref<Store> buildStore;

    RootValue vImportedDrvToDerivation = nullptr;

    /**
     * Debugger
     */
    ReplExitStatus (*debugRepl)(ref<EvalState> es, const ValMap & extraEnv);
    bool debugStop;
    bool inDebugger = false;
    int trylevel;
    std::list<DebugTrace> debugTraces;
    boost::unordered_flat_map<const Expr *, const std::shared_ptr<const StaticEnv>> exprEnvs;

    const std::shared_ptr<const StaticEnv> getStaticEnv(const Expr & expr) const
    {
        auto i = exprEnvs.find(&expr);
        if (i != exprEnvs.end())
            return i->second;
        else
            return std::shared_ptr<const StaticEnv>();
        ;
    }

    void retainPathAccessor(ref<SourceAccessor> accessor)
    {
        std::lock_guard lock(evalEnvironmentSharedState->retainedPathAccessorsMutex);
        evalEnvironmentSharedState->retainedPathAccessors.push_back(std::move(accessor));
    }

    /** Whether a debug repl can be started. If `false`, `runDebugRepl(error)` will return without starting a repl. */
    bool canDebug();

    /** Use front of `debugTraces`; see `runDebugRepl(error,env,expr)` */
    void runDebugRepl(const Error * error);

    /**
     * Run a debug repl with the given error, environment and expression.
     * @param error The error to debug, may be nullptr.
     * @param env The environment to debug, matching the expression.
     * @param expr The expression to debug, matching the environment.
     */
    void runDebugRepl(const Error * error, const Env & env, const Expr & expr);

    template<class T, typename... Args>
    [[nodiscard, gnu::noinline]]
    EvalErrorBuilder<T> & error(const Args &... args)
    {
        // `EvalErrorBuilder::debugThrow` performs the corresponding `delete`.
        return *new EvalErrorBuilder<T>(*this, args...);
    }

    /**
     * Trace-aware evaluation context (BSàlC trace store + Adapton DDG).
     * Holds trace runtime state plus the fingerprint-keyed TraceSession registry.
     * Non-null when eval-trace is enabled (the default); nullptr otherwise.
     */
    std::unique_ptr<TraceRuntime> traceCtx;

    /**
     * Non-zero only inside TracedExpr::evaluateFresh(), so forceValue
     * skips replayMemoizedDeps when no DepRecordingContext is active.
     */
    uint32_t traceActiveDepth = 0;

    /**
     * Outlined thunk-forcing path for forceValue (reduces inline code size).
     *
     * Backward-compatible bridge. Checks for an active EvalContext<Suspendable>
     * thread-local. When EvalContext<Mode> propagation is complete (all callers
     * migrated), this bridge is removed and forceValue takes EvalContext<Mode>&.
     */
    [[gnu::noinline]]
    void forceThunkValue(Value & v, PosIdx pos);

    /**
     * Mode-parameterized forceThunkValue. Critical skips TracedExpr verification
     * (direct eval). Suspendable dispatches through TracedExpr (may syncAwait).
     */
    template<typename Mode>
    [[gnu::noinline]]
    void forceThunkValue(eval_trace::EvalContext<Mode> & ctx, Value & v, PosIdx pos);

    /**
     * Outlined app-forcing path for forceValue (reduces inline code size).
     */
    [[gnu::noinline]]
    void forceAppValue(Value & v, PosIdx pos);

    /**
     * Get the mount-to-input mapping. Returns an empty map when tracing is disabled.
     */
    bool hasTraceContext() const { return traceCtx != nullptr; }
    void resetTraceContext();
    InterningPools & tracingPools();
    /// Get the MemoReplayStore's owned epoch log vector.
    /// Used by FiberScheduler::run to bind the fiber's DepRecordingContext.
    std::vector<Dep> & replayEpochLog();
    eval_trace::AttrVocabStore & vocabStore();
    void registerTracedValueIdentity(const Value * key, const eval_trace::TracedExpr & tracedExpr);
    void registerTracedBindingsValueIdentity(
        const Bindings * key,
        const eval_trace::TracedExpr & tracedExpr,
        const Bindings * originalBindings = nullptr);
    void rememberNixBinding(PosIdx pos, const NixBindingEntry & entry);
    const NixBindingEntry * lookupNixBinding(PosIdx pos) const;
    void registerMaterializedValueIdentity(
        const Value * key,
        eval_trace::TraceBackend * traceBackend,
        std::optional<SiblingIdentity> siblingIdentity,
        AttrPathId pathId,
        std::optional<ValueIdentityStamp> valueIdentityStamp = std::nullopt);
    PublishedMaterializedIdentity publishRootMaterializedValueIdentity(
        const Value * key,
        eval_trace::TraceBackend * traceBackend,
        AttrPathId pathId,
        ValueIdentityStamp valueIdentityStamp);
    void rollbackRootMaterializedValueIdentity(
        const PublishedMaterializedIdentity & publication);
    std::optional<ValueIdentityStamp> lookupValueIdentityStamp(const Value & v) const;
    bool hasValueIdentityForTest(const Value * key) const;
    bool hasBindingsValueIdentityForTest(const Bindings * key) const;
    void recordThunkDeps(const Value & v, uint32_t epochStart);
    void rollbackReplayEpoch(uint32_t epochStart);
    void replayMemoizedDeps(const Value & v);
    /// Fast bloom filter test: true if v MIGHT have memoized deps.
    /// False negatives impossible; false positives rare (~0.4%).
    /// Used by forceValue to skip the replayMemoizedDeps function call
    /// for the 99%+ of values that definitely have no memoized deps.
    bool mayHaveMemoizedDeps(const Value & v) const;
    std::unique_ptr<eval_trace::TraceBackend> makeTraceBackend(const Hash & fingerprint);
    bool sameValueIdentity(Value & v1, Value & v2);

private:
    /**
     * A cache that maps paths to "resolved" paths for importing Nix
     * expressions, i.e. `/foo` to `/foo/default.nix`.
     */
    /**
     * Associate source positions of certain AST nodes with their preceding doc comment, if they have one.
     * Grouped by file.
     */
    const ref<boost::concurrent_flat_map<SourcePath, ref<DocCommentMap>>> positionToDocComment;

    LookupPath lookupPath;

    /**
     * Cache used by prim_match().
     */
    const ref<RegexCache> regexCache;

public:

    /**
     * @param lookupPath     Only used during construction.
     * @param store          The store to use for instantiation
     * @param fetchSettings  Must outlive the lifetime of this EvalState!
     * @param settings       Must outlive the lifetime of this EvalState!
     * @param buildStore     The store to use for builds ("import from derivation", C API `nix_string_realise`)
     */
    EvalState(
        const LookupPath & lookupPath,
        ref<Store> store,
        const fetchers::Settings & fetchSettings,
        const EvalSettings & settings,
        std::shared_ptr<Store> buildStore = nullptr);
    ~EvalState();

    /**
     * A wrapper around EvalMemory::allocValue() to avoid code churn when it
     * was introduced.
     */
    inline Value * allocValue()
    {
        return mem.allocValue();
    }

    LookupPath getLookupPath()
    {
        return lookupPath;
    }

    /**
     * Return a `SourcePath` that refers to `path` in the root
     * filesystem.
     */
    SourcePath rootPath(CanonPath path);

    /**
     * Variant which accepts relative paths too.
     */
    SourcePath rootPath(std::string_view path);

    /**
     * Return a `SourcePath` that refers to `path` in the store.
     *
     * For now, this has to also be within the root filesystem for
     * backwards compat, but for Windows and maybe also pure eval, we'll
     * probably want to do something different.
     */
    SourcePath storePath(const StorePath & path);


    /**
     * Parse a Nix expression from the specified file.
     */
    Expr * parseExprFromFile(const SourcePath & path);
    Expr * parseExprFromFile(const SourcePath & path, const std::shared_ptr<StaticEnv> & staticEnv);
    Expr *
    parseExprFromFile(const SourcePath & path, const SourcePath & basePath, const std::shared_ptr<StaticEnv> & staticEnv);
    Expr * parseExprFromFile(const SourcePath & path, const SourcePath & basePath);
    Expr * parseExprFromFile(
        const SourcePath & displayPath,
        const SourcePath & physicalPath,
        const SourcePath & basePath,
        const std::shared_ptr<StaticEnv> & staticEnv);
    Expr * parseExprFromFile(
        const SourcePath & displayPath,
        const SourcePath & physicalPath,
        const SourcePath & basePath);

    /**
     * Parse a Nix expression from the specified string.
     */
    Expr *
    parseExprFromString(std::string s, const SourcePath & basePath, const std::shared_ptr<StaticEnv> & staticEnv);
    Expr * parseExprFromString(std::string s, const SourcePath & basePath);

    /**
     * Parse REPL bindings from the specified string.
     * Returns ExprAttrs with bindings to add to scope.
     */
    ExprAttrs *
    parseReplBindings(std::string s, const SourcePath & basePath, const std::shared_ptr<StaticEnv> & staticEnv);
    ExprAttrs * parseReplBindings(
        std::string s,
        std::string errorSource,
        const SourcePath & basePath,
        const std::shared_ptr<StaticEnv> & staticEnv);

    Expr * parseStdin();

    /**
     * Evaluate an expression read from the given file to normal
     * form. Optionally enforce that the top-level expression is
     * trivial (i.e. doesn't require arbitrary computation).
     */
    void evalFile(
        const SourcePath & path,
        const SourcePath & basePath,
        Value & v,
        bool mustBeTrivial = false,
        const std::optional<PathObject> & origin = std::nullopt);

    void evalFile(
        const SourcePath & displayPath,
        const SourcePath & physicalPath,
        const SourcePath & basePath,
        Value & v,
        bool mustBeTrivial = false,
        const std::optional<PathObject> & origin = std::nullopt);

    void evalFile(
        const SourcePath & path,
        Value & v,
        bool mustBeTrivial = false,
        const std::optional<PathObject> & origin = std::nullopt);

    void resetFileCache();

    /**
     * Clear the cross-session memo tables that a real subprocess
     * boundary would start empty:
     *   - `importResolutionCache` — evalFile resolution memo
     *   - `fileTraceCache` — evalFile value memo
     *   - `inputCache` — fetchers::InputCache cross-fetch memo
     *
     * Narrower than `resetFileCache()`: preserves `positions` (PosTable)
     * and `traceCtx` so this is safe to call mid-evaluation. Used by
     * test fixtures (`simulateWarmRestart` / `simulateColdProcess`) to match a real new-process
     * cache-state boundary.
     */
    void clearCrossSessionCaches();

    /**
     * Evaluate an expression to normal form
     *
     * @param [out] v The resulting is stored here.
     */
    void eval(Expr * e, Value & v);

    /**
     * Evaluation the expression, then verify that it has the expected
     * type.
     */
    inline bool evalBool(Env & env, Expr * e);
    inline bool evalBool(Env & env, Expr * e, const PosIdx pos, std::string_view errorCtx);
    inline void evalAttrs(Env & env, Expr * e, Value & v, const PosIdx pos, std::string_view errorCtx);

    /**
     * If `v` is a thunk, enter it and overwrite `v` with the result
     * of the evaluation of the thunk.  If `v` is a delayed function
     * application, call the function and overwrite `v` with the
     * result.  Otherwise, this is a no-op.
     */
    inline void forceValue(Value & v, const PosIdx pos);

private:

    /**
     * Internal support function for forceValue
     *
     * This code is factored out so that it's not in the heavily inlined hot path.
     */
    void handleEvalExceptionForThunk(Env * env, Expr * expr, Value & v, const PosIdx pos);

    /**
     * Internal support function for forceValue
     *
     * This code is factored out so that it's not in the heavily inlined hot path.
     */
    void handleEvalExceptionForApp(Value & v, const Value & savedApp);

    void handleEvalFailed(Value & v, PosIdx pos);

    void tryFixupBlackHolePos(Value & v, PosIdx pos);

public:

    /**
     * Force a value, then recursively force list elements and
     * attributes.
     */
    void forceValueDeep(Value & v);

    /**
     * Force `v`, and then verify that it has the expected type.
     */
    NixInt forceInt(Value & v, const PosIdx pos, std::string_view errorCtx);
    NixFloat forceFloat(Value & v, const PosIdx pos, std::string_view errorCtx);
    bool forceBool(Value & v, const PosIdx pos, std::string_view errorCtx);

    void forceAttrs(Value & v, const PosIdx pos, std::string_view errorCtx);

    template<typename Callable>
    inline void forceAttrs(Value & v, Callable getPos, std::string_view errorCtx);

    inline void forceList(Value & v, const PosIdx pos, std::string_view errorCtx);

    /**
     * Force a list value and record a #len shape dep if the list has
     * TracedData provenance. Combines forceList + maybeRecordListLenDep
     * so primops don't need to remember the hook.
     */
    void forceListObserved(Value & v, const PosIdx pos, std::string_view errorCtx);

    /**
     * @param v either lambda or primop
     */
    void forceFunction(Value & v, const PosIdx pos, std::string_view errorCtx);
    std::string_view forceString(Value & v, const PosIdx pos, std::string_view errorCtx);
    std::string_view forceString(
        Value & v,
        NixStringContext & context,
        const PosIdx pos,
        std::string_view errorCtx,
        const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);
    std::string_view forceStringNoCtx(Value & v, const PosIdx pos, std::string_view errorCtx);

    /**
     * Get attribute from an attribute set and throw an error if it doesn't exist.
     */
    const Attr * getAttr(Symbol attrSym, const Bindings * attrSet, std::string_view errorCtx);

    template<typename... Args>
    [[gnu::noinline]]
    void addErrorTrace(Error & e, const Args &... formatArgs) const;
    template<typename... Args>
    [[gnu::noinline]]
    void addErrorTrace(Error & e, const PosIdx pos, const Args &... formatArgs) const;

public:
    class CoercedPath {
        SourcePath value_;
        std::optional<PathObject> origin_;

        CoercedPath(SourcePath value, std::optional<PathObject> origin)
            : value_(std::move(value))
            , origin_(std::move(origin))
        {
        }

        friend class EvalState;

    public:
        CoercedPath() = delete;
        CoercedPath(const CoercedPath &) = delete;
        CoercedPath & operator=(const CoercedPath &) = delete;
        CoercedPath(CoercedPath &&) noexcept = default;
        CoercedPath & operator=(CoercedPath &&) noexcept = default;

        const SourcePath & path() const { return value_; }
        const std::optional<PathObject> & origin() const { return origin_; }
    };

    /**
     * @return true iff the value `v` denotes a derivation (i.e. a
     * set with attribute `type = "derivation"`).
     */
    bool isDerivation(Value & v);

    std::optional<std::string> tryAttrsToString(
        const PosIdx pos, Value & v, NixStringContext & context,
        bool coerceMore = false, bool copyToStore = true);

    std::optional<SemanticHandle> lookupSemanticHandle(const Value & v) const;

    /// Low-level write: replace the Value's entire publication slot.
    /// Used during materialization (replay) where the full SemanticHandle
    /// is restored from the trace cache.  Re-creates the Value with the
    /// new publication pointer.
    void setSemanticHandle(Value & v, const std::optional<SemanticHandle> & publication) const;

    /// Merge fields into the Value's existing SemanticHandle.
    /// Copies path/text/identity from `publication` into whatever the Value
    /// already carries, then updates the kind discriminator.  Used during
    /// coercion and primop publication where the Value may already have
    /// partial provenance (e.g. a PathObject from fetchTree that now also
    /// gets a TextObject from readFile).
    void mergeSemanticHandle(Value & v, const std::optional<SemanticHandle> & publication) const;
    ContextObject captureContextObject(
        std::string_view value,
        const Value & source) const;
    CoercedPath captureCoercedPath(
        SourcePath value,
        const Value & source) const;
    CoercedPath capturePathWithObject(
        SourcePath value,
        std::optional<PathObject> origin) const;
    void publishContextObject(
        Value & v,
        ContextObject && coerced,
        NixStringContext context = {});
    void publishCoercedPath(
        Value & v,
        CoercedPath && coerced);

    /// Attach PathObject provenance to v, guarded by traceActiveDepth.
    /// Replaces manual `if (traceActiveDepth) mergeSemanticHandle(v, SemanticHandle::forPath(...))`.
    void publishPathProvenance(Value & v, PathObject obj);

    /// Attach TextObject provenance to v, guarded by traceActiveDepth.
    void publishTextProvenance(Value & v, TextObject obj);

    /// Attach StructuredObject provenance to an attrset Value's Bindings.
    /// Only effective for nAttrs values; silently does nothing for other types.
    ///
    /// Provenance carrier asymmetry across Value types:
    ///
    /// | Type    | Carrier                               | Works on x86-64? | Why |
    /// |---------|---------------------------------------|------------------|-----|
    /// | nString | StringWithContext::Context::publication | Yes | Context is heap-allocated; publication field not in packed payload |
    /// | nPath   | Path::Details::publication            | Yes | Details is heap-allocated; only Details* in packed payload |
    /// | nAttrs  | Bindings::publication_                | Yes | Bindings is heap-allocated; only Bindings* in packed payload |
    /// | nList   | ContainerProvenanceRegistry (side-map) | Yes | Packed payload stores only elems+size; no room for a third pointer |
    ///
    /// The pattern: for types where the extra field lives on a separately
    /// heap-allocated object (string context, path details, bindings), the packed
    /// payload stores only the pointer to that object, and the publication field
    /// survives because it is NOT in the payload. For lists, the only pointer in
    /// the payload IS the elems array itself, with no separately heap-allocated
    /// header — hence the side-map (ContainerProvenanceRegistry in trace-frame.hh).
    void publishStructuredProvenance(Value & v, StructuredObject obj);

    /// mkStorePathString + publishPathProvenance in one call.
    void mkStorePathStringWithProvenance(const StorePath & p, Value & v, PathObject provenance);

    /// mkOutputString + publishPathProvenance in one call.
    void mkOutputStringWithProvenance(
        Value & v,
        const SingleDerivedPath::Built & b,
        std::optional<StorePath> optStaticOutputPath,
        PathObject provenance,
        const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

    /// mkSingleDerivedPathString + publishPathProvenance in one call.
    void mkSingleDerivedPathStringWithProvenance(const SingleDerivedPath & p, Value & v,
        PathObject provenance);

    enum class CopyLazyPaths : bool {
        PreserveLazy = false,
        Copy = true,
    };

    /**
     * For efficiency reasons, some store paths (as seen by the evaluator) in
     * the storeFS at their content-addressed locations don't get copied to the
     * store eagerly. This saves on needless I/O and possibly IPC if all the
     * evaluator does is just evaluate nix expressions from those locations.
     * This function copies such store objects to the store if they aren't already valid.
     */
    void ensureLazyPathCopied(const StorePath & path);

    /**
     * Ensure that all NixStringContextElem::Opaque context elements get fetched
     * to the store.
     */
    void ensureLazyPathsCopied(const NixStringContext & context);

    /**
     * String coercion.
     *
     * Converts strings, paths and derivations to a
     * string.  If `coerceMore` is set, also converts nulls, integers,
     * booleans and lists to a string.  If `copyToStore` is set,
     * referenced paths are copied to the Nix store as a side effect.
     */
    BackedStringView coerceToString(
        const PosIdx pos,
        Value & v,
        NixStringContext & context,
        std::string_view errorCtx,
        bool coerceMore = false,
        bool copyToStore = true,
        bool canonicalizePath = true);
    ContextObject coerceToContextObject(
        const PosIdx pos,
        Value & v,
        NixStringContext & context,
        std::string_view errorCtx,
        bool coerceMore = false,
        bool copyToStore = true,
        bool canonicalizePath = true);
    ContextObject coerceToContextObjectForUnsafeDiscard(
        const PosIdx pos,
        Value & v,
        NixStringContext & context,
        std::string_view errorCtx);

    /**
     * Path coercion.
     *
     * Converts strings, paths and derivations to a
     * path.  The result is guaranteed to be a canonicalised, absolute
     * path.  Nothing is copied to the store.
     */
    SourcePath coerceToPath(
        const PosIdx pos,
        Value & v,
        NixStringContext & context,
        std::string_view errorCtx);
    CoercedPath coerceToCoercedPath(
        const PosIdx pos,
        Value & v,
        NixStringContext & context,
        std::string_view errorCtx);

    /**
     * Like coerceToPath, but the result must be a store path.
     */
    StorePath coerceToStorePath(const PosIdx pos, Value & v, NixStringContext & context, std::string_view errorCtx);

    /**
     * Part of `coerceToSingleDerivedPath()` without any store IO which is exposed for unit testing only.
     */
    std::pair<SingleDerivedPath, std::string_view> coerceToSingleDerivedPathUnchecked(
        const PosIdx pos,
        Value & v,
        std::string_view errorCtx,
        const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

    /**
     * Coerce to `SingleDerivedPath`.
     *
     * Must be a string which is either a literal store path or a
     * "placeholder (see `DownstreamPlaceholder`).
     *
     * Even more importantly, the string context must be exactly one
     * element, which is either a `NixStringContextElem::Opaque` or
     * `NixStringContextElem::Built`. (`NixStringContextEleme::DrvDeep`
     * is not permitted).
     *
     * The string is parsed based on the context --- the context is the
     * source of truth, and ultimately tells us what we want, and then
     * we ensure the string corresponds to it.
     */
    SingleDerivedPath coerceToSingleDerivedPath(const PosIdx pos, Value & v, std::string_view errorCtx);

#if NIX_USE_BOEHMGC
    /** A GC root for the baseEnv reference. */
    const std::shared_ptr<Env *> baseEnvP;
#endif

public:

    /**
     * The base environment, containing the builtin functions and
     * values.
     */
    Env & baseEnv;

    /**
     * The same, but used during parsing to resolve variables.
     */
    const std::shared_ptr<StaticEnv> staticBaseEnv; // !!! should be private

    /**
     * Internal primops not exposed to the user.
     */
    boost::unordered_flat_map<
        std::string,
        Value *,
        StringViewHash,
        std::equal_to<>,
        traceable_allocator<std::pair<const std::string, Value *>>>
        internalPrimOps;

    /**
     * Name and documentation about every constant.
     *
     * Constants from primops are hard to crawl, and their docs will go
     * here too.
     */
    std::vector<std::pair<std::string, Constant>> constantInfos;

private:

    unsigned int baseEnvDispl = 0;

    void createBaseEnv(const EvalSettings & settings);

    Value * addConstant(const std::string & name, Value & v, Constant info);

    void addConstant(const std::string & name, Value * v, Constant info);

    Value * addPrimOp(PrimOp && primOp);

public:

    /**
     * Retrieve a specific builtin, equivalent to evaluating `builtins.${name}`.
     * @param name The attribute name of the builtin to retrieve.
     * @throws EvalError if the builtin does not exist.
     */
    Value & getBuiltin(const std::string & name);

    /**
     * Retrieve the `builtins` attrset, equivalent to evaluating the reference `builtins`.
     * Always returns an attribute set value.
     */
    Value & getBuiltins();

    struct Doc
    {
        Pos pos;
        std::optional<std::string> name;
        size_t arity;
        std::vector<std::string> args;
        /**
         * Unlike the other `doc` fields in this file, this one should never be
         * `null`.
         */
        const char * doc;
    };

    /**
     * Retrieve the documentation for a value. This will evaluate the value if
     * it is a thunk, and it will partially apply __functor if applicable.
     *
     * @param v The value to get the documentation for.
     */
    std::optional<Doc> getDoc(Value & v);

private:

    inline Value * lookupVar(Env * env, const ExprVar & var, bool noEval);

    friend struct ExprVar;
    friend struct ExprAttrs;
    friend struct ExprLet;

    Expr * parse(
        char * text,
        size_t length,
        Pos::Origin origin,
        const SourcePath & basePath,
        const std::shared_ptr<StaticEnv> & staticEnv);

    ExprAttrs * parseReplBindings(
        char * text,
        size_t length,
        Pos::Origin origin,
        const SourcePath & basePath,
        const std::shared_ptr<StaticEnv> & staticEnv);

    /**
     * Current Nix call stack depth, used with `max-call-depth` setting to throw stack overflow hopefully before we run
     * out of system stack.
     */
    size_t callDepth = 0;

public:

    /**
     * Check that the call depth is within limits, and increment it, until the returned object is destroyed.
     */
    inline CallDepth addCallDepth(const PosIdx pos);

    /**
     * Do a deep equality test between two values.  That is, list
     * elements and attributes are compared recursively.
     */
    bool eqValues(Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx);

    /**
     * Like `eqValues`, but throws an `AssertionError` if not equal.
     *
     * WARNING:
     * Callers should call `eqValues` first and report if `assertEqValues` behaves
     * incorrectly. (e.g. if it doesn't throw if eqValues returns false or vice versa)
     */
    void assertEqValues(Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx);

    bool isFunctor(const Value & fun) const;

    void callFunction(Value & fun, std::span<Value *> args, Value & vRes, const PosIdx pos);

    void callFunction(Value & fun, Value & arg, Value & vRes, const PosIdx pos)
    {
        Value * args[] = {&arg};
        callFunction(fun, args, vRes, pos);
    }

    /**
     * Automatically call a function for which each argument has a
     * default value or has a binding in the `args` map.
     */
    void autoCallFunction(const Bindings & args, Value & fun, Value & res);

    BindingsBuilder buildBindings(
        size_t capacity,
        EmptyBindingsAllocation emptyAllocation = EmptyBindingsAllocation::ReuseSharedEmpty)
    {
        return mem.buildBindings(symbols, capacity, emptyAllocation);
    }

    ListBuilder buildList(size_t size, bool forceHeap = false)
    {
        return mem.buildList(size, forceHeap);
    }

    /**
     * Return a boolean `Value *` without allocating.
     */
    Value * getBool(bool b);

    void mkThunk_(Value & v, Expr * expr);
    void mkPos(Value & v, PosIdx pos);

    /**
     * Create a string representing a store path.
     *
     * The string is the printed store path with a context containing a
     * single `NixStringContextElem::Opaque` element of that store path.
     */
    void mkStorePathString(const StorePath & storePath, Value & v);

    /**
     * Create a string representing a `SingleDerivedPath::Built`.
     *
     * The string is the printed store path with a context containing a
     * single `NixStringContextElem::Built` element of the drv path and
     * output name.
     *
     * @param value Value we are settings
     *
     * @param b the drv whose output we are making a string for, and the
     * output
     *
     * @param optStaticOutputPath Optional output path for that string.
     * Must be passed if and only if output store object is
     * input-addressed or fixed output. Will be printed to form string
     * if passed, otherwise a placeholder will be used (see
     * `DownstreamPlaceholder`).
     *
     * @param xpSettings Stop-gap to avoid globals during unit tests.
     */
    void mkOutputString(
        Value & value,
        const SingleDerivedPath::Built & b,
        std::optional<StorePath> optStaticOutputPath,
        const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

    /**
     * Create a string representing a `SingleDerivedPath`.
     *
     * A combination of `mkStorePathString` and `mkOutputString`.
     */
    void mkSingleDerivedPathString(const SingleDerivedPath & p, Value & v);

    /**
     * @brief Concatenate values with an n-ary version of the `++` operator.
     */
    void concatLists(Value & v, std::span<Value * const> lists, const PosIdx pos, std::string_view errorCtx);

    /**
     * Print statistics, if enabled.
     *
     * Performs a full memory GC before printing the statistics, so that the
     * GC statistics are more accurate.
     */
    void maybePrintStats();

    /**
     * Print statistics, unconditionally, cheaply, without performing a GC first.
     */
    void printStatistics();

    /**
     * Perform a full memory garbage collection - not incremental.
     *
     * @return true if Nix was built with GC and a GC was performed, false if not.
     *              The return value is currently not thread safe - just the return value.
     */
    bool fullGC();

    /**
     * Realise the given context
     * @param[in] context the context to realise
     * @param[out] maybePaths if not nullptr, all built or referenced store paths will be added to this set
     * @return a mapping from the placeholders used to construct the associated value to their final store path.
     */
    [[nodiscard]] StringMap
    realiseContext(const NixStringContext & context, StorePathSet * maybePaths = nullptr, bool isIFD = true);

    /**
     * Coerce `v` to a path and realise it, i.e. build anything in the value's string context using `realiseContext()`.
     * @param copyLazyPaths When encountering a lazy path (i.e. a string with Opaque context that's also "mounted" on
     * the storeFS), fetch the store path to the store.
     */
    SourcePath realisePath(
        const PosIdx pos,
        Value & v,
        std::optional<SymlinkResolution> resolveSymlinks = SymlinkResolution::Full,
        CopyLazyPaths copyLazyPaths = CopyLazyPaths::PreserveLazy);

    /**
     * Realise the given string with context, and return the string with outputs instead of downstream output
     * placeholders.
     * @param[in] str the string to realise
     * @param[out] paths all referenced store paths will be added to this set
     * @return the realised string
     * @throw EvalError if the value is not a string, path or derivation (see `coerceToString`)
     */
    std::string
    realiseString(Value & str, StorePathSet * storePathsOutMaybe, bool isIFD = true, const PosIdx pos = noPos);

    /* Call the binary path filter predicate used builtins.path etc. */
    bool callPathFilter(
        Value * filterFun,
        const SourcePath & path,
        PosIdx pos,
        const std::optional<PathObject> & origin = std::nullopt);

    DocComment getDocCommentForPos(PosIdx pos);

private:

    const std::shared_ptr<EvalEnvironmentSharedState> evalEnvironmentSharedState;

    friend EvalEnvironmentAuthority makeDetachedEvalEnvironmentAuthority(EvalState & state);
    friend EvalEnvironmentAuthority makeSessionEvalEnvironmentAuthority(EvalState & state);
    friend void clearEvalEnvironmentState(EvalState & state);
    friend void releaseSessionEvalEnvironmentState(EvalState & state);
    friend DepHash getOrStoreFileContentHash(
        EvalState & state, const SourcePath & path, std::string_view bytes);
    friend DepHash getOrReadFileContentHash(
        EvalState & state, const SourcePath & path);

    /// Internal coercion with provenance out-params — used only by
    /// coerceToContextObject and coerceToCoercedPath.
    BackedStringView coerceToStringWithProvenance(
        const PosIdx pos, Value & v, NixStringContext & context,
        std::string_view errorCtx, bool coerceMore, bool copyToStore,
        bool canonicalizePath,
        std::optional<PathObject> * origin,
        std::optional<TextObject> * textProvenance);
    SourcePath coerceToPathWithProvenance(
        const PosIdx pos, Value & v, NixStringContext & context,
        std::string_view errorCtx,
        std::optional<PathObject> * origin);
    std::optional<std::string> tryAttrsToString(
        const PosIdx pos, Value & v, NixStringContext & context,
        bool coerceMore, bool copyToStore,
        std::optional<PathObject> * origin,
        std::optional<TextObject> * textProvenance);

    /**
     * Like `mkOutputString` but just creates a raw string, not an
     * string Value, which would also have a string context.
     */
    std::string mkOutputStringRaw(
        const SingleDerivedPath::Built & b,
        std::optional<StorePath> optStaticOutputPath,
        const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

    /**
     * Like `mkSingleDerivedPathStringRaw` but just creates a raw string
     * Value, which would also have a string context.
     */
    std::string mkSingleDerivedPathStringRaw(const SingleDerivedPath & p);

    Counter nrLookups;
    Counter nrAvoided;
    Counter nrOpUpdates;
    Counter nrOpUpdateValuesCopied;
    Counter nrListConcats;
    Counter nrPrimOpCalls;
    Counter nrFunctionCalls;

    bool countCalls;

    typedef boost::unordered_flat_map<std::string, size_t, StringViewHash, std::equal_to<>> PrimOpCalls;
    PrimOpCalls primOpCalls;

    typedef boost::unordered_flat_map<ExprLambda *, size_t> FunctionCalls;
    FunctionCalls functionCalls;

    /** Evaluation/call profiler. */
    MultiEvalProfiler profiler;

    void incrFunctionCall(ExprLambda * fun);

    typedef boost::unordered_flat_map<PosIdx, size_t, std::hash<PosIdx>> AttrSelects;
    AttrSelects attrSelects;

    friend struct ExprOpUpdate;
    friend struct ExprOpConcatLists;
    friend struct ExprVar;
    friend struct ExprString;
    friend struct ExprInt;
    friend struct ExprFloat;
    friend struct ExprPath;
    friend struct ExprSelect;
    friend void prim_getAttr(EvalState & state, const PosIdx pos, Value ** args, Value & v);
    friend void prim_match(EvalState & state, const PosIdx pos, Value ** args, Value & v);
    friend void prim_split(EvalState & state, const PosIdx pos, Value ** args, Value & v);

    friend struct Value;
    friend class ListBuilder;
};

struct DebugTraceStacker
{
    DebugTraceStacker(EvalState & evalState, DebugTrace t);

    ~DebugTraceStacker()
    {
        evalState.debugTraces.pop_front();
    }

    EvalState & evalState;
    DebugTrace trace;
};

/**
 * @return A string representing the type of the value `v`.
 *
 * @param withArticle Whether to begin with an english article, e.g. "an
 * integer" vs "integer".
 */
std::string_view showType(ValueType type, bool withArticle = true);
std::string showType(const Value & v);

/// Returns the cached eval-trace content hash for `path`; computes from `bytes`
/// on miss and caches before returning.
///
/// Use this when the file contents are already in hand (e.g., inside
/// readFile's observation flow). For the read-if-miss variant, see
/// getOrReadFileContentHash.
DepHash getOrStoreFileContentHash(
    EvalState & state,
    const SourcePath & path,
    std::string_view bytes);

/// Returns the cached eval-trace content hash for `path`; on miss reads the
/// file via its SourceAccessor, hashes the bytes, and caches before
/// returning.
///
/// Throws whatever SourcePath::readFile would throw on IO error — matches
/// the existing failure mode where callers that used to call
/// environment.readFile() would have propagated the same exception.
DepHash getOrReadFileContentHash(
    EvalState & state,
    const SourcePath & path);

/**
 * If `path` refers to a directory, then append "/default.nix".
 *
 * @param addDefaultNix Whether to append "/default.nix" after resolving symlinks.
 */
SourcePath resolveExprPath(SourcePath path, bool addDefaultNix = true);

/**
 * Whether a URI is allowed, assuming restrictEval is enabled
 */
bool isAllowedURI(std::string_view uri, const Strings & allowedPaths);

} // namespace nix

#include "nix/expr/eval-inline.hh"
