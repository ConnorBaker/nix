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
#include "nix/util/ref.hh"
#include "nix/expr/counter.hh"

// For `NIX_USE_BOEHMGC`, and if that's set, `GC_THREADS`
#include "nix/expr/config.hh"

#include <unordered_map>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/concurrent_flat_map_fwd.hpp>

#include <map>
#include <set>
#include <optional>
#include <functional>
#include <span>

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
class EvalState;
struct Executor;
class StorePath;
struct SingleDerivedPath;

/**
 * inc2b-1 (parallel eval): per-eval-thread identifier, handed out lazily (the
 * main thread is 1, each worker gets the next value). Packed into a pending
 * value's tag word (above the discriminator bits) so `EvalState::waitOnThunk`
 * can tell a cross-thread wait from a same-thread self-cycle (reported as
 * infinite recursion rather than deadlocking). Declared here (rather than in
 * parallel-eval.hh) so the inlined force site can read it without pulling the
 * executor/boost headers into every translation unit. Defined in
 * parallel-eval.cc.
 */
[[gnu::tls_model("initial-exec")]] extern thread_local uint32_t myEvalThreadId;
enum RepairFlag : bool;
struct MemorySourceAccessor;
struct MountedSourceAccessor;

namespace eval_cache {
class EvalCache;
}

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
 * A position at which a primop is invoked (`noPos` or presumably an `ExprCall` location).
 *
 * If you're about to pass your primop call position to a different function that
 * runs in your primop implementation, then stop, because that position has
 * already been printed in the trace. Just don't pass a pos, or pass `noPos`.
 *
 * Nonetheless, we pass the call site to the primop so that it can be used
 * in other contexts than the usual `try`/`catch`/`addTrace` flow, and
 * specifically as extra context in delayed computations that may fail.
 *
 * Alternatively, a primop registration can set `PrimOp::addTrace = false`, so
 * that the otherwise redundant trace item is suppressed, and the primop becomes
 * responsible for printing its call site location, allowing for some
 * customization of the trace.
 */
struct CallSite
{
    /** An already printed pos! Don't make it noisy. Read the `CallSite` comment. */
    PosIdx pos;
};

/**
 * Function that implements a primop.
 */
using PrimOpFun = void(EvalState & state, CallSite callSite, Value * const * args, Value & v);

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

/**
 * Trace-message stage marker for "we tried to reach a usable value
 * from `v` but failed".
 * Renders as `using` when it has evaluated to a proper value, and
 * `evaluating` otherwise.
 *
 * Use in a `%s` in `addTrace`, e.g.:
 * `"while %s the result of the %s attribute"`.
 */
struct WhileTryingToUse
{
    const Value & v;
};

std::ostream & operator<<(std::ostream & os, WhileTryingToUse w);

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
        operator_, key, path, prefix, outputSpecified, requiredSystemFeatures;

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
            .requiredSystemFeatures = alloc.create("requiredSystemFeatures"),
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

/**
 * A bump region holding what one job (an outermost
 * unforce scope) allocates, released wholesale when the job ends. Every
 * evaluator object lives here while the region is active, and each `Env`
 * carries a size header so it can be copied out.
 * Single-threaded evaluation only.
 */
void evalRegionNoteBigEnv(const void * env, size_t size);

struct EvalRegion
{
    char * base = nullptr;
    char * cur = nullptr;
    char * end = nullptr;
    /** Allocate elsewhere for now: the data outlives the job. */
    bool paused = false;
    /** Bytes of string data allocated here, which the producer closure
        never walks: they do not count towards its step budget. */
    size_t stringBytes = 0;
    /** Bump backend: the size of each Env allocated here, one byte per
        16-byte granule of the region's mapping from `envSizesBase` (255:
        see `evalRegionNoteBigEnv`). Copying an Env out of the region needs
        its size, which the Env does not hold; a side table, released at
        the job's end, keeps the Envs themselves as small as without
        unforcing, so a job that keeps its memory keeps no size prefixes. */
    uint8_t * envSizes = nullptr;
    const char * envSizesBase = nullptr;
    /** PROTOTYPE (parallel-eval increment 0). The striped reserved arena this
        region occupies: the id of its worker's stripe. Single-threaded
        evaluation runs one worker in one active stripe (0). This is the HIGH
        level of the two-level owner-timeline clock `(arena_id, offset)`; it is
        constant across a single-threaded job (every cell is in the one active
        stripe), so `ownerOf` needs only the low level (the within-stripe
        offset, `(p - base) >> 4`). Set when the region opens. A cold field: it
        must stay after the hot bump fields so their offsets are unchanged. */
    uint32_t arenaId = 0;

    bool active() const noexcept
    {
        return cur && !paused;
    }

    [[gnu::always_inline]] void noteEnvSize(const void * env, size_t size)
    {
        auto & s = envSizes[((const char *) env - envSizesBase) >> 4];
        if (size < 255) [[likely]]
            s = size;
        else {
            s = 255;
            evalRegionNoteBigEnv(env, size);
        }
    }

    /** Whether `p` was allocated in the innermost scope. */
    bool contains(const void * p) const noexcept
    {
        return (const char *) p >= base && (const char *) p < cur;
    }

    [[gnu::always_inline]] void * alloc(size_t n)
    {
        return bump(n);
    }

    /** A `Value` cell. In the bump region, addresses are allocation order. */
    [[gnu::always_inline]] void * allocCell()
    {
        return bump(16);
    }

    [[gnu::always_inline]] void * bump(size_t n)
    {
        n = (n + 15) & ~size_t(15);
        if (size_t(end - cur) < n) [[unlikely]]
            throw std::bad_alloc();
        auto p = cur;
        cur += n;
        return p;
    }
};

/* inc2c-1: per-worker. Each worker bumps within its own stripe, giving it an
   independent address clock (the two-level owner-timeline clock's low level).
   initial-exec so the inlined alloc path (allocBytes/allocValue/allocEnv) pays
   one TP-relative load, not a general-dynamic __tls_get_addr call. All member
   initializers are constant, so this is constant-initialized (no TLS-init
   guard on the hot access). At eval-cores == 1 there is one worker, so this is
   a pure relocation of where the region lives. */
[[gnu::tls_model("initial-exec")]] extern thread_local EvalRegion evalRegion;

#if !NIX_USE_BOEHMGC
/** Zeroed evaluator memory outside unforce scopes, without Boehm
    GC: from mimalloc's default heap when available. Never freed. */
void * evalBaseAlloc(size_t n);
#endif

/**
 * A never-freed bump arena for evaluator objects outside job
 * regions, used instead of `calloc` when Boehm GC is disabled and
 * `NIX_NOGC_BUMP` is set. Memory is zero because it comes fresh from mmap.
 */
struct BumpArena
{
    char * cur = nullptr;
    char * end = nullptr;

    [[gnu::always_inline]] void * alloc(size_t n)
    {
        n = (n + 15) & ~size_t(15);
        if (size_t(end - cur) >= n) [[likely]] {
            auto p = cur;
            cur += n;
            return p;
        }
        return refill(n);
    }

    void * refill(size_t n);

    /** Every mapping handed out, for heap verification: [start, end). */
    std::vector<std::pair<char *, char *>> chunks;
};

extern BumpArena evalBaseArena;

extern const bool evalUseBaseArena;

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

    /**
     * Allocates @p count * sizeof(T) + N contiguous bytes.
     * @throws bad_alloc if the computation would overflow.
     */
    template<typename T, std::size_t N = 0>
    inline void * allocBytes(size_t count);

    inline Value * allocValue();

    inline Env & allocEnv(size_t size);

    Bindings * allocBindings(size_t capacity);

    BindingsBuilder buildBindings(SymbolTable & symbols, size_t capacity)
    {
        return BindingsBuilder(*this, symbols, allocBindings(capacity), capacity);
    }

    ListBuilder buildList(size_t size)
    {
        stats.nrListElems += size;
        return ListBuilder(*this, size);
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
     * inc2b-1 (parallel eval): the worker-thread pool. Always constructed; at
     * `eval-cores = 1` (the default) it holds a single idle worker and
     * `executor->enabled` is false, so `builtins.__parallel` never dispatches
     * work and evaluation stays serial. Enabling multiple workers is inc2b-2.
     */
    std::unique_ptr<Executor> executor;

    /**
     * inc2b-1: statistics for the blackhole wait/notify path. All are only
     * touched when a thread actually blocks on a concurrently-forced thunk,
     * i.e. never at `eval-cores = 1`.
     */
    std::atomic<uint64_t> nrThunksAwaited{0};
    std::atomic<uint64_t> nrThunksAwaitedSlow{0};
    std::atomic<uint64_t> currentlyWaiting{0};
    std::atomic<uint64_t> maxWaiting{0};
    std::atomic<uint64_t> microsecondsWaiting{0};
    std::atomic<uint64_t> nrSpuriousWakeups{0};

    /**
     * inc2b-1: block until another thread finishes forcing `v`. `expectedP0` is
     * the pending word 0 observed by the caller (its thread-id field selects
     * self-cycle vs. cross-thread wait). Defined in parallel-eval.cc.
     */
    void waitOnThunk(Value & v, uint64_t expectedP0);

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
    const SourcePath importedDrvToDerivation;

    /**
     * Store used to materialise .drv files.
     */
    const ref<Store> store;

    /**
     * Store used to build stuff.
     */
    const ref<Store> buildStore;

    const ref<fetchers::InputCache> inputCache;

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
     * A cache for evaluation caches, so as to reuse the same root value if possible
     */
    std::map<const Hash, ref<eval_cache::EvalCache>> evalCaches;

private:

    /**
     * A cache that maps paths to "resolved" paths for importing Nix
     * expressions, i.e. `/foo` to `/foo/default.nix`.
     */
    const ref<boost::concurrent_flat_map<SourcePath, SourcePath>> importResolutionCache;

    /**
     * A cache from resolved paths to values.
     */
    const ref<boost::concurrent_flat_map<
        SourcePath,
        Value *,
        std::hash<SourcePath>,
        std::equal_to<SourcePath>,
        gc_root_allocator<std::pair<const SourcePath, Value *>>>>
        fileEvalCache;

    /**
     * Associate source positions of certain AST nodes with their preceding doc comment, if they have one.
     * Grouped by file.
     */
    const ref<boost::concurrent_flat_map<SourcePath, ref<DocCommentMap>>> positionToDocComment;

    LookupPath lookupPath;

    struct LookupPathResolvedState
    {
        SourcePath path;
        const ref<boost::concurrent_flat_map<CanonPath, std::optional<SourcePath>>> resolvedPaths;
    };

    const ref<
        boost::
            concurrent_flat_map<std::string, std::shared_ptr<LookupPathResolvedState>, StringViewHash, std::equal_to<>>>
        lookupPathResolved;

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
     * Allow access to a path.
     *
     * Only for restrict eval: pure eval just whitelist store paths,
     * never arbitrary paths.
     */
    void allowPathLegacy(const std::string & path);

    /**
     * Allow access to a store path. Note that this gets remapped to
     * the real store path if `store` is a chroot store.
     */
    void allowPath(const StorePath & storePath);

    /**
     * Allow access to the closure of a store path.
     */
    void allowClosure(const StorePath & storePath);

    /**
     * Allow access to a store path and return it as a string.
     */
    void allowAndSetStorePathString(const StorePath & storePath, Value & v);

    void checkURI(const std::string & uri);

    /**
     * Mount an input on the Nix store.
     */
    StorePath mountInput(fetchers::Input & input, const fetchers::Input & originalInput, ref<SourceAccessor> accessor);

    /**
     * Parse a Nix expression from the specified file.
     */
    Expr * parseExprFromFile(const SourcePath & path);
    Expr * parseExprFromFile(const SourcePath & path, const std::shared_ptr<StaticEnv> & staticEnv);

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
    void evalFile(const SourcePath & path, Value & v, bool mustBeTrivial = false);

    void resetFileCache();

    /**
     * Look up a file in the search path.
     */
    SourcePath findFile(const std::string_view path);
    SourcePath findFile(const LookupPath & lookupPath, const std::string_view path, const PosIdx pos = noPos);

    /**
     * Try to resolve a search path value (not the optional key part).
     *
     * If the specified search path element is a URI, download it.
     *
     * If it is not found, return `nullptr`.
     */
    std::shared_ptr<LookupPathResolvedState>
    resolveLookupPathPath(const LookupPath::Path & elem, bool initAccessControl = false);

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
    inline bool evalBool(Env & env, Expr * e, std::string_view errorCtx);
    inline void evalAttrs(Env & env, Expr * e, Value & v, std::string_view errorCtx);

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

    /**
     * inc2b-1: slow path of forceValue for a cell already being forced
     * (pdPending). Either this thread is re-entering its own in-progress force
     * (a cycle, reported as infinite recursion byte-identically to inc2a/base),
     * or -- only with eval-cores > 1 -- another worker is forcing it, in which
     * case we block in `waitOnThunk` until it finishes (rethrowing if it
     * failed). `w0` is the pending word observed by the caller.
     *
     * Factored out of the inlined hot path. Takes only `v` (re-reading its tag
     * word itself) so the inlined caller need not keep extra values live across
     * the cold call -- this preserves inc2a's force-site frame size and thus the
     * stack-parity edge.
     */
    void forceValuePending(Value & v);

    /**
     * inc2b-1/2b-2: forceValue's pending pre-check (word 0 observed pdPending,
     * value `expected`) lands here. Self-cycle (executor disabled, or the pending
     * word's thread id is ours) -> reproduces inc2a's env==null arm byte-for-byte
     * (infinite-recursion throw, position fix-up via `pos`, mkFailed, rethrow).
     * Otherwise -> waits for the forcing worker. Out of line so the inlined force
     * site keeps inc2a's frame on every recursion path.
     */
    void forceValueOnPending(Value & v, const PosIdx pos, uint64_t expected);

    /**
     * inc2b-2: elect the unique forcer of a thunk/app cell, out of line -- read
     * word0, pre-check pending (self-cycle throw / cross-thread wait), capture
     * env/expr (resp. left/right) BEFORE the CAS, then CAS word0 to a pending
     * word carrying this thread's id. Returns the captured pointers on a win;
     * returns {null,null} when already handled here (waited). Out of line so the
     * hot force site pays no `myEvalThreadId` TLS read and holds nothing across a
     * call -- both would grow register-tight recursion drivers past base.
     */
    Value::ClosureThunk electThunk(Value & v, const PosIdx pos);
    Value::FunctionApplicationThunk electApp(Value & v, const PosIdx pos);

    void tryFixupBlackHolePos(Value & v, PosIdx pos);

public:

    /**
     * Force a value, then recursively force list elements and
     * attributes.
     */
    void forceValueDeep(Value & v);

    /**
     * The same, skipping cells already in `seen`
     * and adding the cells it walks: several calls with one `seen` walk
     * what one call on a value containing all of them walks. The cells are
     * reported with `unforceNoteSeen`, so an `UnforceSeen` for `seen` keeps
     * them forced and their addresses valid across unforce jobs.
     */
    void forceValueDeep(Value & v, std::set<const void *> & seen);

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
    /**
     * @return true iff the value `v` denotes a derivation (i.e. a
     * set with attribute `type = "derivation"`).
     */
    bool isDerivation(Value & v);

    /**
     * Force `v` and peel through `__toString` and `outPath` attributes
     * repeatedly until reaching a terminal value: either a non-attrset,
     * or an attrset that has neither attribute. Invoke
     * `cb(peeled, cameThroughToString)` on that terminal value.
     *
     * `cb` runs while the peel's call stack is still live, so errors it
     * raises carry a trace reflecting which `__toString` and `outPath`
     * attributes were traversed to reach `peeled`. Relying on the stack
     * to preserve that context avoids the cost and complexity of
     * tracking provenance in the hot path at runtime.
     *
     * `__toString` takes precedence over a sibling `outPath` at each
     * step, matching string-interpolation coercion in the language.
     *
     * `peeled` is always a valid, forced Value; in the terminal-attrset
     * case it is that attrset.
     *
     * `cameThroughToString` is true iff the peel invoked at least one
     * `__toString`.
     *
     * With `checkToStringReturn`, if any `__toString` was traversed the
     * terminal value must be a string, path, or external value;
     * otherwise a `TypeError` is thrown before `cb` runs. Pure `outPath`
     * peels are exempt.
     */
    template<typename Cb>
    auto peelToStringOutPath(const PosIdx pos, Value & v, bool checkToStringReturn, Cb && cb)
        -> std::invoke_result_t<Cb, Value *, bool>;

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

    StorePath copyPathToStore(NixStringContext & context, const SourcePath & path);

    /**
     * Path coercion.
     *
     * Converts strings, paths and derivations to a
     * path.  The result is guaranteed to be a canonicalised, absolute
     * path.  Nothing is copied to the store.
     */
    SourcePath coerceToPath(const PosIdx pos, Value & v, NixStringContext & context, std::string_view errorCtx);

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
    /* inc2b-1: per-thread (DetSys commit 9b814c454). Each worker has its own
       native stack, so the max-call-depth guard must track depth per worker; a
       shared counter races (non-atomic ++/--), drifts upward from lost
       decrements, and eventually throws a spurious "max-call-depth exceeded".
       initial-exec TLS keeps the access to a direct offset. */
    [[gnu::tls_model("initial-exec")]] static thread_local size_t callDepth;

public:

    /**
     * While `isolationDepth > 0`, every thunk
     * update records the thunk's pre-image here, so that the unforce scope's
     * end can restore it. Traceable: the log keeps each target alive, so a
     * restore never writes into reused memory.
     */
    struct UndoEntry
    {
        Value * target;
        Value preImage;
        /* Vestigial. The interval "force marker" algorithm that used these
           24 bytes (per-force start/end allocation clock and parent log
           index) is gone; they are reserved only so the undo log keeps its
           per-entry footprint, and thus `UnforceBatch::full`'s batch-size
           heuristic -- and every NIX_SHOW_STATS owner counter -- stays
           identical. Dropping them is byte-identical in output but shifts
           `ownerConsumerNominated` by one (a batch-packing micro-shift);
           deferred as a separate decision. */
        uint64_t reserved_[3] = {};
    };

    /* inc2c-1: the undo buffer is per-worker (a thread_local global,
       `nix::undoLog` declared after this class), not an EvalState member: the
       DetSys model shares one EvalState across workers, so a single member
       would be a cross-worker contention point and would re-entangle the
       revert proof across threads. Appended only on the warm `logPreImageSlow`
       path (winner-only since inc2c-1), never on the inlined hot path, so the
       (dynamic-init) TLS access never reaches a stack-parity-critical frame.
       At eval-cores == 1 there is one worker, so this is a pure relocation. */

    /* inc2c-1: per-worker force-nesting depth. Checked on the warm hooks
       (logPreImage/endLoggedForce inline guards, forceValueDeep) and in
       begin/endUnforceScope. A single shared member would race across workers;
       per-worker is correct and, with winner-only logPreImage, the only inlined
       read on the force site is endLoggedForce's guard. initial-exec +
       constant-initialized: a single TP-relative load, frame-neutral. */
    [[gnu::tls_model("initial-exec")]] static thread_local unsigned isolationDepth;

    /** Policy only: in how many jobs each old cell was forced. */
    std::unordered_map<const Value *, uint32_t> forceCounts;

    /**
     * Open an unforce scope: thunks updated until the matching
     * `endUnforceScope` are recorded. Returns the log mark to pass back.
     * `stackMark` bounds the frames that the scope outlives (see
     * `unforceIsScopeStackSlot`): the stack pointer of the function whose
     * callees run inside the scope. By default that is the caller, hence not
     * inlined.
     */
    [[gnu::noinline]] size_t beginUnforceScope(const void * stackMark = nullptr);

    /**
     * Record `v`'s current contents before it is overwritten, if
     * an unforce scope must restore it: `v` is older than the scope, and not
     * a stack slot of a frame that will have returned when the scope ends.
     *
     * Shaped for the evaluator's stack frames: one
     * test, one out-of-line call, and nothing returned and kept across the
     * evaluation. The start of the force goes on a thread-local stack, and
     * `endLoggedForce` matches it by `&v`.
     */
    [[gnu::always_inline]] void logPreImage(Value & v)
    {
        if (isolationDepth) [[unlikely]]
            logPreImageSlow(v);
    }

    /**
     * inc2c-1: winner-only variant. The force-site election
     * (`electThunk`/`electApp`) overwrites word 0 with the pending word before
     * the force runs, so the pre-image can no longer be re-read from `v`; the
     * election captures the original Value and passes it here, on the CAS win
     * only. (The force site reads `isolationDepth` itself and calls the slow
     * path directly, so there is no inline variant of this on the hot path.)
     */
    [[gnu::noinline, gnu::cold]] void logPreImageSlow(Value & v, const Value & preImage);

    /**
     * The out-of-line part of `logPreImage`: log `v` with the
     * start marker of its force. Records `v`'s current contents as the
     * pre-image (the single-argument callers overwrite `v` only after the
     * force, e.g. `handleEvalFailed`).
     */
    [[gnu::noinline, gnu::cold]] void logPreImageSlow(Value & v);

    /**
     * After a force of `v`, however it ended: record the end
     * marker of its logged start, if the innermost open one is `v`'s.
     * `isolationDepth` may have changed during the force; a start that was
     * not logged, or that a job's end dropped, is simply not found.
     */
    [[gnu::always_inline]] void endLoggedForce(Value & v)
    {
        if (isolationDepth) [[unlikely]]
            endLoggedForceSlow(v);
    }

    [[gnu::noinline, gnu::cold]] void endLoggedForceSlow(Value & v);

    /**
     * Close an unforce scope and unforce every thunk updated
     * inside it (except pinned ones). Only call it when nothing forced in
     * the scope is still referenced by C++ code that expects it forced.
     */
    void endUnforceScope(size_t mark);

    /**
     * inc2c-2: close a per-worker unforce scope, retaining the whole stripe
     * (retain-only; revert stays OFF until Inc3). Opened from `Executor::worker`
     * for a worker that forces under unforcing. INERT until 2c-3 (the
     * eval-cores > 1 + unforce ctor guard forbids the only path that reaches it).
     */
    void endUnforceScopeRetaining();

    /**
     * inc2c-3: reserve the striped unforce arena up front, on the main thread,
     * before the Executor spawns its worker threads. A worker opens its unforce
     * scope at entry (`Executor::worker`), so without this the first few workers
     * would race the lazy `stripesBase` reservation (and `nStripes` binding).
     * `evalCores` is passed in because the Executor's own back-pointer
     * (`EvalState::executor`) is not yet assigned while this runs (it is called
     * from inside the Executor constructor). No-op unless unforcing is enabled;
     * the caller only invokes it when eval-cores > 1, so the single-stripe
     * eval-cores == 1 path keeps reserving lazily (byte-identical).
     */
    void reserveUnforceStripes(size_t evalCores);

    /**
     * Check that the call depth is within limits, and increment it, until the returned object is destroyed.
     */
    /* inc2b-1: out of line so the thread_local `callDepth` TLS access (read +
       limit check + increment) lives in this frame, not in hot recursion
       drivers like callFunction. That keeps their inlined frames -- and the
       stack-parity edges -- byte-identical to inc2a, while the returned
       CallDepth's inlined destructor decrements through the already-resolved
       pointer (no TLS access on the hot path). */
    CallDepth addCallDepth(const PosIdx pos);

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

    void callFunction(Value & fun, std::span<Value * const> args, Value & vRes, const PosIdx pos);

    void callFunction(Value & fun, Value & arg, Value & vRes, const PosIdx pos)
    {
        callFunction(fun, std::to_array({&arg}), vRes, pos);
    }

    /**
     * Automatically call a function for which each argument has a
     * default value or has a binding in the `args` map.
     */
    void autoCallFunction(const Bindings & args, Value & fun, Value & res);

    BindingsBuilder buildBindings(size_t capacity)
    {
        return mem.buildBindings(symbols, capacity);
    }

    ListBuilder buildList(size_t size)
    {
        return mem.buildList(size);
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
     * @param[out] storePathsOutMaybe all referenced store paths will be added to this set if it's not nullptr
     * @return the realised string
     * @throw EvalError if the value is not a string, path or derivation (see `coerceToString`)
     */
    std::string
    realiseString(Value & str, StorePathSet * storePathsOutMaybe, bool isIFD = true, const PosIdx pos = noPos);

    /* Call the binary path filter predicate used builtins.path etc. */
    bool callPathFilter(Value * filterFun, const SourcePath & path, PosIdx pos);

    DocComment getDocCommentForPos(PosIdx pos);

private:

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

    const bool countCalls;

    typedef boost::concurrent_flat_map<std::string, size_t, StringViewHash, std::equal_to<>> PrimOpCalls;
    const ref<PrimOpCalls> primOpCalls;

    typedef boost::concurrent_flat_map<ExprLambda *, size_t> FunctionCalls;
    const ref<FunctionCalls> functionCalls;

    /** Evaluation/call profiler. */
    MultiEvalProfiler profiler;

    void incrFunctionCall(ExprLambda * fun);

    typedef boost::concurrent_flat_map<PosIdx, size_t, std::hash<PosIdx>> AttrSelects;
    const ref<AttrSelects> attrSelects;

    friend struct ExprOpUpdate;
    friend struct ExprOpConcatLists;
    friend struct ExprVar;
    friend struct ExprString;
    friend struct ExprInt;
    friend struct ExprFloat;
    friend struct ExprPath;
    friend struct ExprSelect;
    friend void prim_getAttr(EvalState & state, CallSite callSite, Value * const * args, Value & v);
    friend void prim_match(EvalState & state, CallSite callSite, Value * const * args, Value & v);
    friend void prim_split(EvalState & state, CallSite callSite, Value * const * args, Value & v);

    friend struct Value;
    friend class ListBuilder;
};

/* inc2c-1: the per-worker undo buffer (see EvalState::isolationDepth / the
   `undoLog` note inside EvalState). thread_local so each worker appends its own
   force log; recorded by `logPreImageSlow`, consumed at the scope's end on the
   same worker. initial-exec keeps the (warm-path) access to a TP-relative load.
   Named `undoLog` so the former `EvalState::undoLog` spellings resolve to it
   unchanged inside the evaluator's methods and free functions. */
using EvalUndoLog = std::vector<EvalState::UndoEntry, traceable_allocator<EvalState::UndoEntry>>;
[[gnu::tls_model("initial-exec")]] extern thread_local EvalUndoLog undoLog;

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
