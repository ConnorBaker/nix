#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/eval-environment/authority-internal.hh"
#include "nix/expr/eval-environment/environment.hh"
#include "eval-environment/private-errors.hh"
#include "nix/expr/eval-environment/request-types.hh"
#include "nix/expr/eval-trace/context.hh"
#include "nix/expr/eval-trace/cache/derived-container-builder.hh"
#include "nix/expr/eval-trace/cache/trace-session.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval-trace/deps/recording.hh"
#include "nix/expr/eval-trace/deps/trace-access.hh"
#include "nix/expr/eval-trace/deps/replay-publish-scope.hh"
#include "nix/expr/eval-trace/deps/sibling-force-scope.hh"
#include "nix/expr/eval-trace/deps/input-resolution.hh"
#include "nix/expr/eval-trace/deps/shape-recording.hh"
#include "nix/expr/eval-trace/deps/nix-binding.hh"
#include "nix/expr/eval-trace/deps/dep-hash-fns.hh"
#include "nix/expr/eval-trace/hash-spec.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/print-options.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/util/exit.hh"
#include "nix/util/types.hh"
#include "nix/util/util.hh"
#include "nix/util/environment-variables.hh"
#include "nix/store/store-api.hh"
#include "nix/store/derivations.hh"
#include "nix/store/downstream-placeholder.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/store/filetransfer.hh"
#include "nix/expr/function-trace.hh"
#include "nix/store/profiles.hh"
#include "nix/expr/print.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/expr/gc-small-vector.hh"
#include "nix/util/url.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/fetchers/tarball.hh"
#include "nix/fetchers/input-cache.hh"
#include "nix/util/current-process.hh"

#include "parser-tab.hh"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <cstring>
#include <optional>
#include <unistd.h>
#include <sys/time.h>
#include <fstream>
#include <functional>
#include <ranges>
#include <mutex>

#include <nlohmann/json.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/unordered/concurrent_flat_map.hpp>

#include "nix/util/strings-inline.hh"

using json = nlohmann::json;

namespace nix {

EvalEnvironmentSharedState::EvalEnvironmentSharedState()
    : inputCache(fetchers::InputCache::create())
    , srcToStore(make_ref<SrcToStoreCache>())
    , importResolutionCache(make_ref<ImportResolutionCache>())
    , fileTraceCache(make_ref<FileTraceValueCache>())
    , fileContentHashCache(make_ref<FileContentHashCache>())
    , lookupPathResolved(make_ref<boost::concurrent_flat_map<std::string, std::optional<SourcePath>>>())
{
}

DepHash getOrStoreFileContentHash(
    EvalState & state,
    const SourcePath & path,
    std::string_view bytes)
{
    auto & cache = *state.evalEnvironmentSharedState->fileContentHashCache;
    if (auto cached = getConcurrent(cache, path))
        return *cached;
    auto hash = depHash(bytes);
    cache.try_emplace(path, hash);
    return hash;
}

DepHash getOrReadFileContentHash(
    EvalState & state,
    const SourcePath & path)
{
    auto & cache = *state.evalEnvironmentSharedState->fileContentHashCache;
    if (auto cached = getConcurrent(cache, path))
        return *cached;
    auto bytes = path.readFile();
    auto hash = depHash(bytes);
    cache.try_emplace(path, hash);
    return hash;
}

static void maybeRecordImportedFileContent(
    EvalState & state,
    const SourcePath & path,
    const std::optional<PathObject> & origin);

/// Copy a path to the store and return the published store path string.
///
/// The `context` parameter is an OUTPUT accumulator — the caller's growing
/// set of string context entries (e.g., from derivationStrict's attribute
/// iteration).  This function ADDS its result's context to the accumulator
/// but does NOT pass the accumulator as input to the copy/publish operation.
///
/// This separation is critical: the copy operation needs only the context
/// of the specific path being copied (empty for a local file, one Opaque
/// entry for a store path root).  Passing the outer accumulator would cause
/// realiseContextImpl to process Built entries from unrelated derivation
/// attributes, triggering spurious IFD errors.
PublishedStorePathString copyPathToStoreViaEvalEnvironment(
    EvalState & state,
    NixStringContext & context,
    const SourcePath & path,
    std::optional<PathObject> origin)
{
    if (nix::isDerivation(path.path.abs()))
        state.error<EvalError>("file names are not allowed to end in '%1%'", drvExtension).debugThrow();

    EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
    auto storePathAndSubpath = [&]() -> std::optional<std::pair<StorePath, CanonPath>> {
        try {
            return state.store->toStorePath(path.path.abs());
        } catch (Error &) {
            return std::nullopt;
        }
    }();
    // Fast-path: if the path IS a store path root (not a file within
    // a store tree), publish it directly without copying.  The subpath
    // must be root — for files within a store tree (e.g., ./builder.sh
    // inside a flake source), we must fall through to the slow path
    // which independently content-addresses the file.  Otherwise the
    // string context would reference the containing tree root instead
    // of the individual file, producing wrong derivation hashes.
    //
    // Master's EvalState::storePath() routes through `rootFS` (a union
    // accessor that contains `storeFS`), so we accept either accessor:
    // both point at the same underlying store tree.
    if (storePathAndSubpath
        && storePathAndSubpath->second.isRoot()
        && state.store->isValidPath(storePathAndSubpath->first)
        && (path.accessor == state.rootFS
            || path.accessor == state.storeFS.cast<SourceAccessor>()))
    {
        auto [storePath, subpath] = *storePathAndSubpath;
        // Fresh context for this specific publish operation — NOT the
        // outer accumulator.  The store path root has no placeholder
        // rewrites to resolve.
        NixStringContext publishContext;
        auto request = StorePathPublishRequest{
            .coercedPath = CoercedPathRequest{
                .path = state.storePath(storePath),
                .context = publishContext,
            },
            .pos = noPos,
        };

        auto published = environment.publishStorePath(request);

        context.insert(published.context().begin(), published.context().end());
        return published;
    }

    // Fresh context for this specific copy operation — NOT the outer
    // accumulator.  The file being copied has its own identity; any
    // CA derivation placeholder rewrites in its path string (if any)
    // would come from the path's own provenance, not from unrelated
    // derivation attributes that happen to have been coerced earlier.
    NixStringContext copyContext;
    auto request = CopyPathToStoreRequest{
        .name = std::string(path.baseName()),
        .path = path,
        .origin = std::move(origin),
        .filterEvaluator = {},
        .method = ContentAddressMethod::Raw::NixArchive,
        .expectedHash = std::nullopt,
        .context = copyContext,
        .pos = noPos,
    };

    auto published = environment.copyPathToStore(request);

    printMsg(lvlChatty, "copied source '%1%' -> '%2%'", path, published.renderedPath());
    context.insert(published.context().begin(), published.context().end());
    return published;
}

std::string realiseStringViaEvalEnvironment(
    EvalState & state,
    Value & str,
    StorePathSet * storePathsOutMaybe,
    bool isIFD,
    const PosIdx pos)
{
    NixStringContext stringContext;
    auto rawStr = state.coerceToString(pos, str, stringContext, "while realising a string").toOwned();
    EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
    auto observation = environment.realiseContext(
        RealiseContextRequest::make(
            stringContext,
            isIFD ? StringRealisationMode::ImportFromDerivation
                  : StringRealisationMode::NonImportFromDerivation));
    if (storePathsOutMaybe) {
        for (const auto & path : observation.referencedStorePaths)
            storePathsOutMaybe->emplace(path);
    }
    StringMap rewrites;
    for (const auto & rewrite : observation.rewrites)
        rewrites.insert_or_assign(rewrite.placeholder, state.store->printStorePath(rewrite.storePath));
    return rewriteStrings(rawStr, rewrites);
}

static std::string readFileViaEvalEnvironment(
    EvalState & state,
    const SourcePath & path)
{
    EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
    ReadFileRequest request{
        .coercedPath = CoercedPathRequest{
            .path = path,
        },
    };

    return environment.readFile(observeOnly, request).bytes;
}

/**
 * Just for doc strings. Not for regular string values.
 */
static char * allocString(size_t size)
{
    char * t;
    t = (char *) GC_MALLOC_ATOMIC(size);
    if (!t)
        throw std::bad_alloc();
    return t;
}

// When there's no need to write to the string, we can optimize away empty
// string allocations.
// This function handles makeImmutableString(std::string_view()) by returning
// the empty string.
/**
 * Just for doc strings. Not for regular string values.
 */
static const char * makeImmutableString(std::string_view s)
{
    const size_t size = s.size();
    if (size == 0)
        return "";
    auto t = allocString(size + 1);
    memcpy(t, s.data(), size);
    t[size] = '\0';
    return t;
}

StringData & StringData::alloc(EvalMemory & mem, size_t size)
{
    void * t = mem.allocBytes(sizeof(StringData) + size + 1);
    if (!t)
        throw std::bad_alloc();
    auto res = new (t) StringData(size);
    return *res;
}

const StringData & StringData::make(EvalMemory & mem, std::string_view s)
{
    if (s.empty())
        return ""_sds;
    auto & res = alloc(mem, s.size());
    std::memcpy(&res.data_, s.data(), s.size());
    res.data_[s.size()] = '\0';
    return res;
}

RootValue allocRootValue(Value * v)
{
    return std::allocate_shared<Value *>(traceable_allocator<Value *>(), v);
}

// Pretty print types for assertion errors
std::ostream & operator<<(std::ostream & os, const ValueType t)
{
    os << showType(t);
    return os;
}

std::string printValue(EvalState & state, Value & v)
{
    std::ostringstream out;
    v.print(state, out);
    return out.str();
}

Value * Value::toPtr(SymbolStr str) noexcept
{
    return const_cast<Value *>(str.valuePtr());
}

void Value::print(EvalState & state, std::ostream & str, PrintOptions options)
{
    printValue(state, str, *this, options);
}

std::string_view showType(ValueType type, bool withArticle)
{
#define WA(a, w) withArticle ? a " " w : w
    switch (type) {
    case nInt:
        return WA("an", "integer");
    case nBool:
        return WA("a", "Boolean");
    case nString:
        return WA("a", "string");
    case nPath:
        return WA("a", "path");
    case nNull:
        return "null";
    case nAttrs:
        return WA("a", "set");
    case nList:
        return WA("a", "list");
    case nFunction:
        return WA("a", "function");
    case nExternal:
        return WA("an", "external value");
    case nFloat:
        return WA("a", "float");
    case nThunk:
        return WA("a", "thunk");
    case nFailed:
        return WA("an", "error");
    }
    unreachable();
}

std::string showType(const Value & v)
{
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (v.getInternalType()) {
    case tString:
        return v.context() ? "a string with context" : "a string";
    case tPrimOp:
        return fmt("the built-in function '%s'", std::string(v.primOp()->name));
    case tPrimOpApp:
        return fmt("the partially applied built-in function '%s'", v.primOpAppPrimOp()->name);
    case tExternal:
        return v.external()->showType();
    case tThunk:
        return v.isBlackhole() ? "a black hole" : "a thunk";
    case tApp:
        return "a function application";
    default:
        return std::string(showType(v.type()));
    }
#pragma GCC diagnostic pop
}

PosIdx Value::determinePos(const PosIdx pos) const
{
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    switch (getInternalType()) {
    case tAttrs:
        return attrs()->pos;
    case tLambda:
        return lambda().fun->pos;
    case tApp:
        return app().left->determinePos(pos);
    default:
        return pos;
    }
#pragma GCC diagnostic pop
}

bool Value::isTrivial() const
{
    return !isa<tApp, tPrimOpApp>()
           && (!isa<tThunk>()
               || (dynamic_cast<ExprAttrs *>(thunk().expr) && ((ExprAttrs *) thunk().expr)->dynamicAttrs->empty())
               || dynamic_cast<ExprLambda *>(thunk().expr) || dynamic_cast<ExprList *>(thunk().expr));
}

static Symbol getName(const AttrName & name, EvalState & state, Env & env)
{
    if (name.symbol) {
        return name.symbol;
    } else {
        Value nameValue;
        name.expr->eval(state, env, nameValue);
        state.forceStringNoCtx(nameValue, name.expr->getPos(), "while evaluating an attribute name");
        return state.symbols.create(nameValue.string_view());
    }
}

static constexpr size_t BASE_ENV_SIZE = 128;

EvalMemory::EvalMemory()
{
    assertGCInitialized();
}

EvalState::EvalState(
    const LookupPath & lookupPathFromArguments,
    ref<Store> store,
    const fetchers::Settings & fetchSettings,
    const EvalSettings & settings,
    std::shared_ptr<Store> buildStore)
    : fetchSettings{fetchSettings}
    , settings{settings}
    , symbols(StaticEvalSymbols::staticSymbolTable())
    , repair(NoRepair)
    , storeFS(makeMountedSourceAccessor({
          {CanonPath::root, makeEmptySourceAccessor()},
          /* In the pure eval case, we can simply require
             valid paths. However, in the *impure* eval
             case this gets in the way of the union
             mechanism, because an invalid access in the
             upper layer will *not* be caught by the union
             source accessor, but instead abort the entire
             lookup.

             This happens when the store dir in the
             ambient file system has a path (e.g. because
             another Nix store there), but the relocated
             store does not.

             TODO make the various source accessors doing
             access control all throw the same type of
             exception, and make union source accessor
             catch it, so we don't need to do this hack.
           */
          {CanonPath(store->storeDir), store->getFSAccessor(settings.pureEval)},
      }))
    , rootFS([&] {
        /* In pure eval mode, we provide a filesystem that only
           contains the Nix store.

           Otherwise, use a union accessor to make the augmented store
           available at its logical location while still having the
           underlying directory available. This is necessary for
           instance if we're evaluating a file from the physical
           /nix/store while using a chroot store, and also for lazy
           mounted fetchTree. */
        auto accessor = settings.pureEval ? storeFS.cast<SourceAccessor>()
                                          : makeUnionSourceAccessor({getFSSourceAccessor(), storeFS});
        /* Cache positive lstat/readlink results to speed up resolveSymlinks. */
        accessor = makeCachingSourceAccessor(accessor);

        /* Apply access control if needed. */
        if (settings.restrictEval || settings.pureEval)
            accessor = AllowListSourceAccessor::create(
                accessor, {}, {}, [&settings](const CanonPath & path) -> RestrictedPathError {
                    auto modeInformation = settings.pureEval ? "in pure evaluation mode (use '--impure' to override)"
                                                             : "in restricted mode";
                    throw RestrictedPathError("access to absolute path '%1%' is forbidden %2%", path, modeInformation);
                });

        return accessor;
    }())
    , corepkgsFS(make_ref<MemorySourceAccessor>())
    , internalFS(make_ref<MemorySourceAccessor>())
    , derivationInternal{internalFS->addFile(
          CanonPath("derivation-internal.nix"),
#include "primops/derivation.nix.gen.hh"
          )}
    , store(store)
    , buildStore(buildStore ? buildStore : store)
    , debugRepl(nullptr)
    , debugStop(false)
    , trylevel(0)
    , positionToDocComment(make_ref<decltype(positionToDocComment)::element_type>())
    , regexCache(makeRegexCache())
#if NIX_USE_BOEHMGC
    , baseEnvP(std::allocate_shared<Env *>(traceable_allocator<Env *>(), &mem.allocEnv(BASE_ENV_SIZE)))
    , baseEnv(**baseEnvP)
#else
    , baseEnv(mem.allocEnv(BASE_ENV_SIZE))
#endif
    , staticBaseEnv{std::make_shared<StaticEnv>(nullptr, nullptr)}
    , evalEnvironmentSharedState(std::make_shared<EvalEnvironmentSharedState>())
{
#ifndef _WIN32
    static std::once_flag stackSizeBumped;
    std::call_once(stackSizeBumped, []() {
        // Increase the default stack size for the evaluator and for
        // libstdc++'s std::regex.
        // This used to be 64 MiB, but macOS as deployed on GitHub Actions has a
        // hard limit slightly under that, so we round it down a bit.
        nix::ensureStackSizeAtLeast(60 * 1024 * 1024);
    });
#endif

    corepkgsFS->setPathDisplay("<nix", ">");
    internalFS->setPathDisplay("«nix-internal»", "");

    countCalls = getEnv("NIX_COUNT_CALLS").value_or("0") != "0";

    static_assert(sizeof(Env) <= 16, "environment must be <= 16 bytes");

    /* Construct the Nix expression search path. */
    assert(lookupPath.elements.empty());
    if (!settings.pureEval) {
        for (auto & i : lookupPathFromArguments.elements) {
            lookupPath.elements.emplace_back(LookupPath::Elem{i});
        }
        /* $NIX_PATH overriding regular settings is implemented as a hack in `initGC()` */
        for (auto & i : settings.nixPath.get()) {
            lookupPath.elements.emplace_back(LookupPath::Elem::parse(i));
        }
        if (!settings.restrictEval) {
            for (auto & i : EvalSettings::getDefaultNixPath()) {
                lookupPath.elements.emplace_back(LookupPath::Elem::parse(i));
            }
        }
    }

    /* Allow access to all paths in the search path. */
    if (rootFS.dynamic_pointer_cast<AllowListSourceAccessor>()) {
        EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(*this));
        auto detached = environment.openDetachedEffectScope();
        environment.initializeLookupPathAccessControl(detached, lookupPath);
    }

    corepkgsFS->addFile(
        CanonPath("fetchurl.nix"),
#include "fetchurl.nix.gen.hh"
    );

    eval_trace::setEvalTraceHashAlgorithm(settings.evalTraceHashAlgorithm);

    createBaseEnv(settings);

    if (settings.useTraceCache)
        traceCtx = std::make_unique<TraceRuntime>();

    /* Register function call tracer. */
    if (settings.traceFunctionCalls)
        profiler.addProfiler(make_ref<FunctionCallTrace>());

    switch (settings.evalProfilerMode) {
    case EvalProfilerMode::flamegraph:
        profiler.addProfiler(
            makeSampleStackProfiler(*this, settings.evalProfileFile.get(), settings.evalProfilerFrequency));
        break;
    case EvalProfilerMode::disabled:
        break;
    }
}

EvalState::~EvalState()
{
    releaseSessionEvalEnvironmentState(*this);
}


Value * EvalState::addConstant(const std::string & name, Value & v, Constant info)
{
    Value * v2 = allocValue();
    *v2 = v;
    addConstant(name, v2, info);
    return v2;
}

void EvalState::addConstant(const std::string & name, Value * v, Constant info)
{
    auto name2 = name.substr(0, 2) == "__" ? name.substr(2) : name;

    constantInfos.push_back({name2, info});

    if (!(settings.pureEval && info.impureOnly)) {
        /* Check the type, if possible.

           We might know the type of a thunk in advance, so be allowed
           to just write it down in that case. */
        if (auto gotType = v->type</*invalidIsThunk=*/true>(); gotType != nThunk)
            assert(info.type == gotType);

        /* Install value the base environment. */
        staticBaseEnv->vars.emplace_back(symbols.create(name), baseEnvDispl);
        baseEnv.values[baseEnvDispl++] = v;
        const_cast<Bindings *>(getBuiltins().attrs())->push_back(Attr(symbols.create(name2), v));
    }
}

void PrimOp::check()
{
    if (arity > maxPrimOpArity) {
        throw Error("primop arity must not exceed %1%", maxPrimOpArity);
    }
}

std::ostream & operator<<(std::ostream & output, const PrimOp & primOp)
{
    output << "primop " << primOp.name;
    return output;
}

const PrimOp * Value::primOpAppPrimOp() const
{
    Value * left = primOpApp().left;
    while (left && !left->isPrimOp()) {
        left = left->primOpApp().left;
    }

    if (!left)
        return nullptr;

    assert(left->isPrimOp());
    return left->primOp();
}

void Value::mkPrimOp(PrimOp * p)
{
    p->check();
    setStorage(p);
}

Value * EvalState::addPrimOp(PrimOp && primOp)
{
    /* Hack to make constants lazy: turn them into a application of
       the primop to a dummy value. */
    if (primOp.arity == 0) {
        primOp.arity = 1;
        auto vPrimOp = allocValue();
        vPrimOp->mkPrimOp(new PrimOp(std::move(primOp)));
        Value v;
        v.mkApp(vPrimOp, vPrimOp);
        auto & primOp1 = *vPrimOp->primOp();
        return addConstant(
            primOp1.name,
            v,
            {
                .type = nThunk, // FIXME
                .doc = primOp1.doc ? primOp1.doc->c_str() : nullptr,
            });
    }

    auto envName = symbols.create(primOp.name);
    if (hasPrefix(primOp.name, "__"))
        primOp.name = primOp.name.substr(2);

    Value * v = allocValue();
    v->mkPrimOp(new PrimOp(primOp));

    if (primOp.internal)
        internalPrimOps.emplace(primOp.name, v);
    else {
        staticBaseEnv->vars.emplace_back(envName, baseEnvDispl);
        baseEnv.values[baseEnvDispl++] = v;
        const_cast<Bindings *>(getBuiltins().attrs())->push_back(Attr(symbols.create(primOp.name), v));
    }

    return v;
}

Value & EvalState::getBuiltins()
{
    return *baseEnv.values[0];
}

Value & EvalState::getBuiltin(const std::string & name)
{
    auto it = getBuiltins().attrs()->get(symbols.create(name));
    if (it)
        return *it->value;
    else
        error<EvalError>("builtin '%1%' not found", name).debugThrow();
}

std::optional<EvalState::Doc> EvalState::getDoc(Value & v)
{
    if (v.isPrimOp()) {
        auto v2 = &v;
        auto & primOp = *v2->primOp();
        if (primOp.doc)
            return Doc{
                .pos = {},
                .name = primOp.name,
                .arity = primOp.arity,
                .args = primOp.args,
                .doc = primOp.doc->c_str(),
            };
    }
    if (v.isLambda()) {
        auto exprLambda = v.lambda().fun;

        std::ostringstream s;
        std::string name;
        auto pos = positions[exprLambda->getPos()];
        std::string docStr;

        if (exprLambda->name) {
            name = symbols[exprLambda->name];
        }

        if (exprLambda->docComment) {
            docStr = exprLambda->docComment.getInnerText(positions);
        }

        if (name.empty()) {
            s << "Function ";
        } else {
            s << "Function `" << name << "`";
            if (pos)
                s << "\\\n  … ";
            else
                s << "\\\n";
        }
        if (pos) {
            s << "defined at " << pos;
        }
        if (!docStr.empty()) {
            s << "\n\n";
        }

        s << docStr;

        return Doc{
            .pos = pos,
            .name = name,
            .arity = 0, // FIXME: figure out how deep by syntax only? It's not semantically useful though...
            .args = {},
            /* N.B. Can't use StringData here, because that would lead to an interior pointer.
               NOTE: memory leak when compiled without GC. */
            .doc = makeImmutableString(s.view()),
        };
    }
    if (isFunctor(v)) {
        try {
            Value & functor = *v.attrs()->get(s.functor)->value;
            Value * vp[] = {&v};
            Value partiallyApplied;
            // The first parameter is not user-provided, and may be
            // handled by code that is opaque to the user, like lib.const = x: y: y;
            // So preferably we show docs that are relevant to the
            // "partially applied" function returned by e.g. `const`.
            // We apply the first argument:
            callFunction(functor, vp, partiallyApplied, noPos);
            auto _level = addCallDepth(noPos);
            return getDoc(partiallyApplied);
        } catch (Error & e) {
            e.addTrace(nullptr, "while partially calling '%1%' to retrieve documentation", "__functor");
            throw;
        }
    }
    return {};
}

// just for the current level of StaticEnv, not the whole chain.
void printStaticEnvBindings(const SymbolTable & st, const StaticEnv & se)
{
    std::cout << ANSI_MAGENTA;
    for (auto & i : se.vars)
        std::cout << st[i.first] << " ";
    std::cout << ANSI_NORMAL;
    std::cout << std::endl;
}

// just for the current level of Env, not the whole chain.
void printWithBindings(const SymbolTable & st, const Env & env)
{
    if (!env.values[0]->isThunk()) {
        std::cout << "with: ";
        std::cout << ANSI_MAGENTA;
        auto j = env.values[0]->attrs()->begin();
        while (j != env.values[0]->attrs()->end()) {
            std::cout << st[j->name] << " ";
            ++j;
        }
        std::cout << ANSI_NORMAL;
        std::cout << std::endl;
    }
}

void printEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env, int lvl)
{
    std::cout << "Env level " << lvl << std::endl;

    if (se.up && env.up) {
        std::cout << "static: ";
        printStaticEnvBindings(st, se);
        if (se.isWith)
            printWithBindings(st, env);
        std::cout << std::endl;
        printEnvBindings(st, *se.up, *env.up, ++lvl);
    } else {
        std::cout << ANSI_MAGENTA;
        // for the top level, don't print the double underscore ones;
        // they are in builtins.
        for (auto & i : se.vars)
            if (!hasPrefix(st[i.first], "__"))
                std::cout << st[i.first] << " ";
        std::cout << ANSI_NORMAL;
        std::cout << std::endl;
        if (se.isWith)
            printWithBindings(st, env); // probably nothing there for the top level.
        std::cout << std::endl;
    }
}

void printEnvBindings(const EvalState & es, const Expr & expr, const Env & env)
{
    // just print the names for now
    auto se = es.getStaticEnv(expr);
    if (se)
        printEnvBindings(es.symbols, *se, env, 0);
}

void mapStaticEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env, ValMap & vm)
{
    // add bindings for the next level up first, so that the bindings for this level
    // override the higher levels.
    // The top level bindings (builtins) are skipped since they are added for us by initEnv()
    if (env.up && se.up) {
        mapStaticEnvBindings(st, *se.up, *env.up, vm);

        if (se.isWith && !env.values[0]->isThunk()) {
            // add 'with' bindings.
            for (auto & j : *env.values[0]->attrs())
                vm.insert_or_assign(std::string(st[j.name]), j.value);
        } else {
            // iterate through staticenv bindings and add them.
            for (auto & i : se.vars)
                vm.insert_or_assign(std::string(st[i.first]), env.values[i.second]);
        }
    }
}

std::unique_ptr<ValMap> mapStaticEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env)
{
    auto vm = std::make_unique<ValMap>();
    mapStaticEnvBindings(st, se, env, *vm);
    return vm;
}

/**
 * Sets `inDebugger` to true on construction and false on destruction.
 */
class DebuggerGuard
{
    bool & inDebugger;
public:
    DebuggerGuard(bool & inDebugger)
        : inDebugger(inDebugger)
    {
        inDebugger = true;
    }

    DebuggerGuard(DebuggerGuard &&) = delete;
    DebuggerGuard(const DebuggerGuard &) = delete;
    DebuggerGuard & operator=(DebuggerGuard &&) = delete;
    DebuggerGuard & operator=(const DebuggerGuard &) = delete;

    ~DebuggerGuard()
    {
        inDebugger = false;
    }
};

bool EvalState::canDebug()
{
    return debugRepl && !debugTraces.empty();
}

void EvalState::runDebugRepl(const Error * error)
{
    if (!canDebug())
        return;

    assert(!debugTraces.empty());
    const DebugTrace & last = debugTraces.front();
    const Env & env = last.env;
    const Expr & expr = last.expr;

    runDebugRepl(error, env, expr);
}

void EvalState::runDebugRepl(const Error * error, const Env & env, const Expr & expr)
{
    // Make sure we have a debugger to run and we're not already in a debugger.
    if (!debugRepl || inDebugger)
        return;

    auto dts = [&]() -> std::unique_ptr<DebugTraceStacker> {
        if (error && expr.getPos()) {
            auto trace = DebugTrace{
                .pos = [&]() -> std::variant<Pos, PosIdx> {
                    if (error->info().pos) {
                        if (auto * pos = error->info().pos.get())
                            return *pos;
                        return noPos;
                    }
                    return expr.getPos();
                }(),
                .expr = expr,
                .env = env,
                .hint = error->info().msg,
                .isError = true};

            return std::make_unique<DebugTraceStacker>(*this, std::move(trace));
        }
        return nullptr;
    }();

    if (error) {
        printError("%s\n", error->what());

        if (trylevel > 0 && error->info().level != lvlInfo)
            printError(
                "This exception occurred in a 'tryEval' call. Use " ANSI_GREEN "--ignore-try" ANSI_NORMAL
                " to skip these.\n");
    }

    auto se = getStaticEnv(expr);
    if (se) {
        auto vm = mapStaticEnvBindings(symbols, *se.get(), env);
        DebuggerGuard _guard(inDebugger);
        auto exitStatus = (debugRepl) (ref<EvalState>(shared_from_this()), *vm);
        switch (exitStatus) {
        case ReplExitStatus::QuitAll:
            if (error)
                throw *error;
            throw Exit(0);
        case ReplExitStatus::Continue:
            break;
        default:
            unreachable();
        }
    }
}

template<typename... Args>
void EvalState::addErrorTrace(Error & e, const Args &... formatArgs) const
{
    e.addTrace(nullptr, HintFmt(formatArgs...));
}

template<typename... Args>
void EvalState::addErrorTrace(Error & e, const PosIdx pos, const Args &... formatArgs) const
{
    e.addTrace(positions[pos], HintFmt(formatArgs...));
}

template<typename... Args>
static std::unique_ptr<DebugTraceStacker> makeDebugTraceStacker(
    EvalState & state, Expr & expr, Env & env, std::variant<Pos, PosIdx> pos, const Args &... formatArgs)
{
    return std::make_unique<DebugTraceStacker>(
        state,
        DebugTrace{.pos = std::move(pos), .expr = expr, .env = env, .hint = HintFmt(formatArgs...), .isError = false});
}

DebugTraceStacker::DebugTraceStacker(EvalState & evalState, DebugTrace t)
    : evalState(evalState)
    , trace(std::move(t))
{
    evalState.debugTraces.push_front(trace);
    if (evalState.debugStop && evalState.debugRepl)
        evalState.runDebugRepl(nullptr, trace.env, trace.expr);
}

struct PublicationWarmupScope
    : private gdp::Certifier<DepRecordingContext::DepCaptureScopeTag>
{
    EvalState & state;
    DepRecordingContext * ctx = nullptr;
    std::optional<DepRecordingContext::Scope> stashed;
    uint32_t epochStart = 0;
    bool closed = false;
    bool finalized = false;

    PublicationWarmupScope(EvalState & state, DepRecordingContext * ctx)
        : state(state)
        , ctx(ctx)
        , epochStart(ctx ? ctx->epochLog.size() : 0)
    {
        if (ctx)
            Certifier::withProof([&](const auto & proof) { ctx->pushScope(proof); });
    }

    void close()
    {
        if (!ctx || closed)
            return;
        assert(!ctx->scopeStack.empty());
        stashed = std::move(ctx->scopeStack.back());
        Certifier::withProof([&](const auto & proof) { ctx->popScope(proof); });
        closed = true;
    }

    void mergeIntoParent()
    {
        close();
        finalized = true;
        if (!ctx || !stashed)
            return;
        auto * parent = ctx->currentScope();
        if (!parent) {
            discard();
            stashed.reset();
            return;
        }

        // Re-record deps to the parent scope. Do NOT rollback the epoch log
        // or epochMap — rollbackReplayEpoch erases epochMap entries for
        // sub-thunks forced during the warmup scope. If those thunks are
        // later re-accessed from a different scope, replayMemoizedDeps finds
        // no entry and silently skips their deps (BUG-3: stale dep replay).
        //
        // The epoch log retains the warmup scope's contributions at their
        // original positions. The epochMap entries remain valid because they
        // point to these unchanged positions. The re-record loop below adds
        // the deps to the parent's ownDeps and dedup state without affecting
        // the epoch log positions that epochMap references.
        for (auto & dep : stashed->ownDeps)
            ctx->record(dep);

        parent->replayedValues.insert(
            std::make_move_iterator(stashed->replayedValues.begin()),
            std::make_move_iterator(stashed->replayedValues.end()));
        parent->unstable = parent->unstable || stashed->unstable;
        stashed.reset();
    }

    void discard()
    {
        close();
        finalized = true;
        if (ctx) {
            state.rollbackReplayEpoch(epochStart);
            ctx->epochLog.truncate(epochStart);
        }
        stashed.reset();
    }

    ~PublicationWarmupScope()
    {
        if (!ctx || finalized)
            return;
        discard();
    }
};

void Value::mkString(std::string_view s, EvalMemory & mem, const SemanticHandle * publication)
{
    mkStringNoCopy(StringData::make(mem, s), Value::StringWithContext::Context::fromBuilder({}, mem, publication));
}

Value::StringWithContext::Context *
Value::StringWithContext::Context::fromBuilder(
    const NixStringContext & context,
    EvalMemory & mem,
    const SemanticHandle * publication)
{
    if (context.empty() && !publication)
        return nullptr;

    auto ctx = new (mem.allocBytes(sizeof(Context) + context.size() * sizeof(value_type))) Context(context.size(), publication);
    std::ranges::transform(
        context, ctx->elems, [&](const NixStringContextElem & elt) { return &StringData::make(mem, elt.to_string()); });
    return ctx;
}

void Value::mkString(
    std::string_view s,
    const NixStringContext & context,
    EvalMemory & mem,
    const SemanticHandle * publication)
{
    mkStringNoCopy(StringData::make(mem, s), Value::StringWithContext::Context::fromBuilder(context, mem, publication));
}

void Value::mkStringMove(
    const StringData & s,
    const NixStringContext & context,
    EvalMemory & mem,
    const SemanticHandle * publication)
{
    mkStringNoCopy(s, Value::StringWithContext::Context::fromBuilder(context, mem, publication));
}

const Value::Path::Details *
Value::Path::Details::make(EvalMemory & mem, SourceAccessor * accessor, const SemanticHandle * publication)
{
    return new (mem.allocBytes(sizeof(Details))) Details{
        .accessor = accessor,
        .publication = publication,
    };
}

void Value::mkPath(const SourcePath & path, EvalMemory & mem, const SemanticHandle * publication)
{
    mkPath(&*path.accessor, StringData::make(mem, path.path.abs()), mem, publication);
}

[[gnu::always_inline]] inline Value * EvalState::lookupVar(Env * env, const ExprVar & var, bool noEval)
{
    for (auto l = var.level; l; --l, env = env->up)
        ;

    if (!var.fromWith)
        return env->values[var.displ];

    // This early exit defeats the `maybeThunk` optimization for variables from `with`,
    // The added complexity of handling this appears to be similarly in cost, or
    // the cases where applicable were insignificant in the first place.
    if (noEval)
        return nullptr;

    auto * fromWith = var.fromWith;
    while (1) {
        forceAttrs(*env->values[0], fromWith->pos, "while evaluating the first subexpression of a with expression");
        if (auto j = env->values[0]->attrs()->get(var.name)) {
            if (countCalls) [[unlikely]]
                attrSelects[j->pos]++;
            return j->value;
        }
        if (!fromWith->parentWith) [[unlikely]]
            error<UndefinedVarError>("undefined variable '%1%'", symbols[var.name])
                .atPos(var.pos)
                .withFrame(*env, var)
                .debugThrow();
        for (size_t l = fromWith->prevWith; l; --l, env = env->up)
            ;
        fromWith = fromWith->parentWith;
    }
}

ListBuilder::ListBuilder(EvalMemory & mem, size_t size, bool forceHeap)
    : size(size)
    , forceHeap(forceHeap)
    , elems(
        this->forceHeap || size > 2
            ? (Value **) mem.allocBytes((size == 0 ? 1 : size) * sizeof(Value *))
            : inlineElems)
{
    if (this->forceHeap && size == 0)
        elems[0] = nullptr;
}

Value * EvalState::getBool(bool b)
{
    return b ? &Value::vTrue : &Value::vFalse;
}

static Counter nrThunks;

static inline void mkThunk(Value & v, Env & env, Expr * expr)
{
    v.mkThunk(&env, expr);
    nrThunks++;
}

void EvalState::mkThunk_(Value & v, Expr * expr)
{
    mkThunk(v, baseEnv, expr);
}

void EvalState::mkPos(Value & v, PosIdx p)
{
    auto origin = positions.originOf(p);
    if (auto path = std::get_if<SourcePath>(&origin)) {
        auto attrs = buildBindings(3);
        attrs.alloc(s.file).mkString(path->path.abs(), mem);
        makePositionThunks(*this, p, attrs.alloc(s.line), attrs.alloc(s.column));
        v.mkAttrs(attrs);
    } else
        v.mkNull();
}

void EvalState::mkStorePathString(const StorePath & p, Value & v)
{
    v.mkString(
        store->printStorePath(p),
        NixStringContext{
            NixStringContextElem::Opaque{.path = p},
        },
        mem);
}

std::string EvalState::mkOutputStringRaw(
    const SingleDerivedPath::Built & b,
    std::optional<StorePath> optStaticOutputPath,
    const ExperimentalFeatureSettings & xpSettings)
{
    /* In practice, this is testing for the case of CA derivations, or
       dynamic derivations. */
    return optStaticOutputPath ? store->printStorePath(std::move(*optStaticOutputPath))
                               /* Downstream we would substitute this for an actual path once
                                  we build the floating CA derivation */
                               : DownstreamPlaceholder::fromSingleDerivedPathBuilt(b, xpSettings).render();
}

void EvalState::mkOutputString(
    Value & value,
    const SingleDerivedPath::Built & b,
    std::optional<StorePath> optStaticOutputPath,
    const ExperimentalFeatureSettings & xpSettings)
{
    value.mkString(mkOutputStringRaw(b, optStaticOutputPath, xpSettings), NixStringContext{b}, mem);
}

std::string EvalState::mkSingleDerivedPathStringRaw(const SingleDerivedPath & p)
{
    return std::visit(
        overloaded{
            [&](const SingleDerivedPath::Opaque & o) { return store->printStorePath(o.path); },
            [&](const SingleDerivedPath::Built & b) {
                auto optStaticOutputPath = std::visit(
                    overloaded{
                        [&](const SingleDerivedPath::Opaque & o) {
                            auto drv = store->readDerivation(o.path);
                            auto i = drv.outputs.find(b.output);
                            if (i == drv.outputs.end())
                                throw Error(
                                    "derivation '%s' does not have output '%s'",
                                    b.drvPath->to_string(*store),
                                    b.output);
                            return i->second.path(*store, drv.name, b.output);
                        },
                        [&](const SingleDerivedPath::Built & o) -> std::optional<StorePath> { return std::nullopt; },
                    },
                    b.drvPath->raw());
                return mkOutputStringRaw(b, optStaticOutputPath);
            }},
        p.raw());
}

void EvalState::mkSingleDerivedPathString(const SingleDerivedPath & p, Value & v)
{
    v.mkString(
        mkSingleDerivedPathStringRaw(p),
        NixStringContext{
            std::visit([](auto && v) -> NixStringContextElem { return v; }, p),
        },
        mem);
}

void EvalState::publishPathProvenance(Value & v, PathObject obj)
{
    if (traceActiveDepth) [[unlikely]]
        mergeSemanticHandle(v, SemanticHandle::forPath(std::move(obj)));
}

void EvalState::publishTextProvenance(Value & v, TextObject obj)
{
    if (traceActiveDepth) [[unlikely]]
        mergeSemanticHandle(v, SemanticHandle::forText(std::move(obj)));
}

void EvalState::publishStructuredProvenance(Value & v, StructuredObject obj)
{
    if (!traceActiveDepth) [[likely]]
        return;

    // NOTE: No list branch. On x86-64 (useBitPackedValueStorage == true),
    // the packed setStorage(List) stores only elems and size; any additional
    // field on the List struct would be silently dropped by the packed
    // storage round-trip. List provenance is carried entirely via
    // ContainerProvenanceRegistry (trace-frame.hh), keyed by heap-backed list
    // storage identity. Both buildCachedResult and materialization's stageContainerProvenance
    // path use lookupTracedContainer, not publication(). See value.hh, List struct
    // comment for the full explanation of the asymmetry.
    //
    // The attrset branch below works because Bindings is heap-allocated; the
    // packed Value payload stores only a Bindings*, and publication_ lives on
    // the heap-allocated Bindings object, not in the payload. The shared
    // emptyBindings singleton is replaced before publication because it must
        // never carry per-container provenance.
    if (v.type() == nAttrs) {
        if (v.attrs() == &Bindings::emptyBindings)
            v.mkAttrs(mem.allocBindings(0, EmptyBindingsAllocation::AllocateFresh));

        auto handle = SemanticHandle::forStructured(std::move(obj));
        auto * allocated = new (mem.allocBytes(sizeof(SemanticHandle)))
            SemanticHandle(std::move(handle));
        const_cast<Bindings *>(v.attrs())->setPublication(allocated);
    }
}

void EvalState::mkStorePathStringWithProvenance(const StorePath & p, Value & v, PathObject provenance)
{
    mkStorePathString(p, v);
    publishPathProvenance(v, std::move(provenance));
}

void EvalState::mkOutputStringWithProvenance(
    Value & v,
    const SingleDerivedPath::Built & b,
    std::optional<StorePath> optStaticOutputPath,
    PathObject provenance,
    const ExperimentalFeatureSettings & xpSettings)
{
    mkOutputString(v, b, std::move(optStaticOutputPath), xpSettings);
    publishPathProvenance(v, std::move(provenance));
}

void EvalState::mkSingleDerivedPathStringWithProvenance(const SingleDerivedPath & p, Value & v,
    PathObject provenance)
{
    mkSingleDerivedPathString(p, v);
    publishPathProvenance(v, std::move(provenance));
}

Value * Expr::maybeThunk(EvalState & state, Env & env)
{
    Value * v = state.allocValue();
    mkThunk(*v, env, this);
    return v;
}

Value * ExprVar::maybeThunk(EvalState & state, Env & env)
{
    Value * v = state.lookupVar(&env, *this, true);
    /* The value might not be initialised in the environment yet.
       In that case, ignore it. */
    if (v) {
        state.nrAvoided++;
        return v;
    }
    return Expr::maybeThunk(state, env);
}

Value * ExprString::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return &v;
}

Value * ExprInt::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return &v;
}

Value * ExprFloat::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return &v;
}

Value * ExprPath::maybeThunk(EvalState & state, Env & env)
{
    state.nrAvoided++;
    return &v;
}

/**
 * A helper `Expr` class to lets us parse and evaluate Nix expressions
 * from a thunk, ensuring that every file is parsed/evaluated only
 * once (via the thunk stored in `EvalEnvironmentSharedState::fileTraceCache`).
 */
struct ExprParseFile : Expr, gc
{
    SourcePath displayPath;
    SourcePath physicalPath;
    SourcePath basePath;
    bool mustBeTrivial;
    std::optional<PathObject> origin;

    ExprParseFile(
        SourcePath displayPath,
        SourcePath physicalPath,
        SourcePath basePath,
        bool mustBeTrivial,
        std::optional<PathObject> origin)
        : displayPath(std::move(displayPath))
        , physicalPath(std::move(physicalPath))
        , basePath(std::move(basePath))
        , mustBeTrivial(mustBeTrivial)
        , origin(std::move(origin))
    {
    }

    void showForHash(const SymbolTable &, std::ostream &, const CanonPath &) const override {}

    void eval(EvalState & state, Env & env, Value & v) override
    {
        printTalkative("evaluating file '%s'", displayPath);

        auto e = state.parseExprFromFile(displayPath, physicalPath, basePath);

        // Two-layer dep emission for traced imports (see OR-2 in
        // src/libexpr/eval-trace/CLAUDE.md):
        //
        //   1. `registerNixBindings` runs whenever `hasTraceContext()` is
        //      true — including during lockFlake, before any trace scope
        //      starts.  It populates the NixSemanticAnalyzer registry so
        //      later access-time `maybeRecordNixBindingDep` calls (fired
        //      from ExprSelect / ExprOpHasAttr / prim_getAttr) can emit
        //      per-binding StructuredProjection deps.  Provenance
        //      resolution (source, key) is deferred to access time, when
        //      the mount table is populated.
        //
        //   2. `maybeRecordImportedFileContent` runs only inside a live
        //      trace scope (`traceActiveDepth`).  It is the FileBytes
        //      backstop: a coarse content dep that covers bare imports
        //      whose consumer never fires a Nix-level ExprSelect —
        //      deepSeq / attrNames / formals / findAlongAttrPath / C++
        //      attrs()->get paths.  Access-time emission dedups against
        //      this one via `scopeContainsDepKey`; pass-2 override
        //      subsumes it when a per-binding SP covers the file.
        if (state.hasTraceContext()) [[unlikely]] {
            auto [exprAttrs, scopeExprs] = findNonRecExprAttrs(e);
            if (exprAttrs) {
                auto resolved = physicalPath.resolveSymlinks(SymlinkResolution::Ancestors);
                auto parentDir = resolved.path.parent().value_or(CanonPath::root);
                auto scopeHash = computeNixScopeHash(scopeExprs, state.symbols, &parentDir);
                registerNixBindings(
                    state, exprAttrs, scopeHash, state.symbols,
                    state.tracingPools(), resolved, origin);
            }
            if (state.traceActiveDepth) [[unlikely]] {
                maybeRecordImportedFileContent(state, physicalPath, origin);
            }
        }

        try {
            auto dts =
                state.debugRepl
                    ? makeDebugTraceStacker(
                          state,
                          *e,
                          state.baseEnv,
                          e->getPos(),
                          "while evaluating the file '%s':",
                          displayPath.to_string())
                    : nullptr;

            // Enforce that 'flake.nix' is a direct attrset, not a
            // computation.
            if (mustBeTrivial && !(dynamic_cast<ExprAttrs *>(e)))
                state.error<EvalError>("file '%s' must be an attribute set", displayPath).debugThrow();

            state.eval(e, v);
        } catch (Error & e) {
            state.addErrorTrace(e, "while evaluating the file '%s':", displayPath.to_string());
            throw;
        }
    }
};

static void evalFileImpl(
    EvalState & state,
    EvalEnvironmentSharedState & envSharedState,
    const SourcePath & displayPath,
    const SourcePath & physicalPath,
    const SourcePath & basePath,
    Value & v,
    bool mustBeTrivial,
    const std::optional<PathObject> & origin)
{
    auto & importResolutionCache = *envSharedState.importResolutionCache;
    auto & fileTraceCache = *envSharedState.fileTraceCache;
    auto resolvedPath = getConcurrent(importResolutionCache, physicalPath);

    if (!resolvedPath) {
        resolvedPath = resolveExprPath(physicalPath);
        importResolutionCache.emplace(physicalPath, *resolvedPath);
    }

    FileTraceCacheKey cacheKey{
        .path = *resolvedPath,
        .displayPath = displayPath,
        .basePath = basePath,
        .mustBeTrivial = mustBeTrivial,
        .origin = origin,
    };

    // Content dep recording moved into ExprParseFile::eval (cache-miss only).
    // NOT recording on cache hits prevents orphaned Content deps in child
    // scopes. The Content dep covers the whole file. For .nix code consumed
    // via import/evalFile, NixBinding (format 'n') provides per-binding
    // StructuredContent override for eligible non-recursive attrsets. For data
    // files consumed by fromJSON/fromTOML, TextObject enables
    // StructuredContent two-level override. See design.md §4.6 (Dependency
    // Over-Approximation). For NixBinding files, the Content dep comes from
    // maybeRecordNixBindingDep (co-located with the SC dep).

    if (auto v2 = getConcurrent(fileTraceCache, cacheKey)) {
        state.forceValue(**v2, noPos);
        v = **v2;
        return;
    }

    Value * vExpr;
    // FIXME: put ExprParseFile on the stack instead of the heap once
    // https://github.com/NixOS/nix/pull/13930 is merged. That will ensure
    // the post-condition that `expr` is unreachable after
    // `forceValue()` returns.
    auto expr = new ExprParseFile{displayPath, *resolvedPath, basePath, mustBeTrivial, origin};

    fileTraceCache.try_emplace_and_cvisit(
        std::move(cacheKey),
        nullptr,
        [&](auto & i) {
            vExpr = state.allocValue();
            vExpr->mkThunk(&state.baseEnv, expr);
            i.second = vExpr;
        },
        [&](auto & i) { vExpr = i.second; });

    state.forceValue(*vExpr, noPos);

    v = *vExpr;
}

void EvalState::evalFile(
    const SourcePath & path,
    const SourcePath & basePath,
    Value & v,
    bool mustBeTrivial,
    const std::optional<PathObject> & origin)
{
    evalFileImpl(*this, *evalEnvironmentSharedState, path, path, basePath, v, mustBeTrivial, origin);
}

void EvalState::evalFile(
    const SourcePath & displayPath,
    const SourcePath & physicalPath,
    const SourcePath & basePath,
    Value & v,
    bool mustBeTrivial,
    const std::optional<PathObject> & origin)
{
    evalFileImpl(*this, *evalEnvironmentSharedState, displayPath, physicalPath, basePath, v, mustBeTrivial, origin);
}

void EvalState::evalFile(
    const SourcePath & path,
    Value & v,
    bool mustBeTrivial,
    const std::optional<PathObject> & origin)
{
    auto resolvedPath = resolveExprPath(path);
    auto basePath = resolvedPath.parent();
    evalFileImpl(*this, *evalEnvironmentSharedState, path, resolvedPath, basePath, v, mustBeTrivial, origin);
}

static void maybeRecordImportedFileContent(
    EvalState & state,
    const SourcePath & path,
    const std::optional<PathObject> & origin)
{
    EvalEnvironment environment(makeDetachedEvalEnvironmentAuthority(state));
    // Only record when a trace session is active — without one the read
    // is pure overhead (the result is discarded).  The auto-dispatch
    // readFile(request) fallback would perform real I/O via the detached
    // path, which the old code intentionally skipped.
    if (!environment.tryBindCurrentEvalSession())
        return;
    (void) environment.readFile(ReadFileRequest{
        .coercedPath = CoercedPathRequest{
            .path = path,
            .origin = origin,
        },
    });
}

void EvalState::resetFileCache()
{
    evalEnvironmentSharedState->importResolutionCache->clear();
    evalEnvironmentSharedState->fileTraceCache->clear();
    evalEnvironmentSharedState->inputCache->clear();
    evalEnvironmentSharedState->fileContentHashCache->clear();
    /* `clearEvalEnvironmentState` clears `sharedState->lookupPathResolved`
       along with `srcToStore`, so no direct clear here. */
    clearEvalEnvironmentState(*this);
    positions.clear();
    rootFS->invalidateCache();
    resetTraceContext();
}

void EvalState::clearCrossSessionCaches()
{
    evalEnvironmentSharedState->importResolutionCache->clear();
    evalEnvironmentSharedState->fileTraceCache->clear();
    evalEnvironmentSharedState->inputCache->clear();
    evalEnvironmentSharedState->fileContentHashCache->clear();
    /* Without this, session-boundary callers that skip `resetFileCache`
       leak lookup-path resolutions across sessions (AD-6 rule 6). */
    evalEnvironmentSharedState->lookupPathResolved->clear();
    rootFS->invalidateCache();
}

// ── First-touch sibling forcing ──────────────────────────────────────
//
// Under an active SiblingReplayCaptureScope, first-touch sibling thunks are
// evaluated with dep tracking temporarily suspended, then their replay range
// is published immediately and re-consumed through replayMemoizedDeps(). This
// gives first-touch and replay-hit siblings the same ValueContext recording
// path while keeping non-sibling thunk forcing on the ordinary direct-record
// path.

SiblingForceScope::SiblingForceScope(
    EvalState & state,
    const Value & value,
    uint32_t epochStart)
    : state(state)
    , value(value)
    , epochStart(epochStart)
{
    fiberCtx = eval_trace::currentFiberDepCtx();
    if (!fiberCtx)
        fiberCtx = eval_trace::currentStandaloneDepCtx();
    if (fiberCtx)
        Certifier::withProof([&](const auto & proof) { fiberCtx->pushScope(proof); });
}

void SiblingForceScope::commit()
{
    assert(!committed);
    // 1. Pop isolated scope — deps from eval are NOT in outer ownDeps.
    if (fiberCtx)
        Certifier::withProof([&](const auto & proof) { fiberCtx->popScope(proof); });
    // 2. Publish epoch range [epochStart, epochLog.size()) to replayStore.
    state.recordThunkDeps(value, epochStart);
    // 3. Replay through SiblingReplayCaptureScope → ValueContext dep.
    state.replayMemoizedDeps(value);
    committed = true;
}

// ── Mode-parameterized forceThunkValue ────────────────────────────────
//
// Critical: skip TracedExpr verification. Direct eval only.
// Used inside lock scopes (EvalContext<Critical>) where syncAwait
// is structurally forbidden.
template<>
[[gnu::noinline]]
void EvalState::forceThunkValue(eval_trace::EvalContext<eval_trace::Critical> &,
                                 Value & v, const PosIdx pos)
{
    Env * env = v.thunk().env;
    assert(env || v.isBlackhole());
    Expr * expr = v.thunk().expr;

    try {
        v.mkBlackhole();
        if (env) [[likely]]
            expr->eval(*this, *env, v);
        else
            ExprBlackHole::throwInfiniteRecursionError(*this, v);
    } catch (...) {
        handleEvalExceptionForThunk(env, expr, v, pos);
        throw;
    }
}

// Suspendable: may verify via TracedExpr.
//
// No SuspendableCtxScope needed here: receiving ctx by reference
// implies a live ancestor scope on this thread's stack (Ref-implies-
// scope invariant, enforced by EvalContext<Suspendable>'s private
// ctor + SuspendableCtxScope friend).  The thread-local already
// points at the innermost scope's ctx.
template<>
[[gnu::noinline]]
void EvalState::forceThunkValue(eval_trace::EvalContext<eval_trace::Suspendable> & ctx,
                                 Value & v, const PosIdx pos)
{
    Env * env = v.thunk().env;
    assert(env || v.isBlackhole());
    Expr * expr = v.thunk().expr;
    const uint32_t epochStart = traceCtx ? traceCtx->currentReplayEpochSize() : 0;

    try {
        v.mkBlackhole();
        if (env) [[likely]] {
            if (traceCtx && traceCtx->shouldIsolateSiblingForce(v)) {
                SiblingForceScope siblingScope(*this, v, epochStart);
                expr->eval(*this, *env, v);
                siblingScope.commit();
            } else {
                ReplayPublishScope replayScope([this, &v, epochStart]{
                    recordThunkDeps(v, epochStart);
                });
                expr->eval(*this, *env, v);
                replayScope.commit();
            }
        } else
            ExprBlackHole::throwInfiniteRecursionError(*this, v);
    } catch (...) {
        handleEvalExceptionForThunk(env, expr, v, pos);
        throw;
    }
}

// Backward-compatible bridge (removed when all callers are migrated).
// Preserves the original body verbatim — does NOT delegate to the
// template specializations. The Critical specialization intentionally
// skips dep recording (it's for lock scopes), but uncolored callers
// that go through this bridge still need dep recording for correctness.
[[gnu::noinline]]
void EvalState::forceThunkValue(Value & v, const PosIdx pos)
{
    Env * env = v.thunk().env;
    assert(env || v.isBlackhole());
    Expr * expr = v.thunk().expr;
    const uint32_t epochStart = traceCtx ? traceCtx->currentReplayEpochSize() : 0;

    try {
        v.mkBlackhole();
        if (env) [[likely]] {
            if (traceCtx && traceCtx->shouldIsolateSiblingForce(v)) {
                SiblingForceScope siblingScope(*this, v, epochStart);
                expr->eval(*this, *env, v);
                siblingScope.commit();
            } else {
                ReplayPublishScope replayScope([this, &v, epochStart]{
                    recordThunkDeps(v, epochStart);
                });
                expr->eval(*this, *env, v);
                replayScope.commit();
            }
        } else
            ExprBlackHole::throwInfiniteRecursionError(*this, v);
    } catch (...) {
        handleEvalExceptionForThunk(env, expr, v, pos);
        throw;
    }
}

[[gnu::noinline]]
void EvalState::forceAppValue(Value & v, const PosIdx pos)
{
    const uint32_t epochStart = traceCtx ? traceCtx->currentReplayEpochSize() : 0;
    ReplayPublishScope replayScope([this, &v, epochStart]{
        recordThunkDeps(v, epochStart);
    });
    Value savedApp = v;
    try {
        callFunction(*v.app().left, *v.app().right, v, pos);
        replayScope.commit();
    } catch (...) {
        handleEvalExceptionForApp(v, savedApp);
        throw;
    }
}

void EvalState::resetTraceContext()
{
    if (traceCtx)
        traceCtx->reset();
}

std::unique_ptr<eval_trace::TraceBackend> EvalState::makeTraceBackend(const Hash & fingerprint)
{
    assert(traceCtx);
    return traceCtx->makeTraceBackend(fingerprint, symbols);
}

InterningPools & EvalState::tracingPools()
{
    assert(traceCtx);
    return traceCtx->tracingPools();
}

std::vector<Dep> & EvalState::replayEpochLog()
{
    assert(traceCtx);
    return traceCtx->replayStore.epochLog_;
}

eval_trace::AttrVocabStore & EvalState::vocabStore()
{
    assert(traceCtx);
    return traceCtx->getVocabStore(symbols);
}

void EvalState::registerTracedValueIdentity(const Value * key, const eval_trace::TracedExpr & tracedExpr)
{
    if (traceCtx)
        traceCtx->registerTracedValueIdentity(key, tracedExpr);
}

void EvalState::registerTracedBindingsValueIdentity(
    const Bindings * key,
    const eval_trace::TracedExpr & tracedExpr,
    const Bindings * originalBindings)
{
    if (traceCtx)
        traceCtx->registerTracedBindingsValueIdentity(key, tracedExpr, originalBindings);
}

void EvalState::rememberNixBinding(PosIdx pos, const NixBindingEntry & entry)
{
    if (traceCtx)
        traceCtx->rememberNixBinding(pos, entry);
}

const NixBindingEntry * EvalState::lookupNixBinding(PosIdx pos) const
{
    return traceCtx ? traceCtx->lookupNixBinding(pos) : nullptr;
}

void EvalState::registerMaterializedValueIdentity(
    const Value * key,
    eval_trace::TraceBackend * traceBackend,
    std::optional<SiblingIdentity> siblingIdentity,
    AttrPathId pathId,
    std::optional<ValueIdentityStamp> valueIdentityStamp)
{
    if (traceCtx)
        traceCtx->registerMaterializedValueIdentity(
            key, traceBackend, std::move(siblingIdentity), pathId, valueIdentityStamp);
}

PublishedMaterializedIdentity EvalState::publishRootMaterializedValueIdentity(
    const Value * key,
    eval_trace::TraceBackend * traceBackend,
    AttrPathId pathId,
    ValueIdentityStamp valueIdentityStamp)
{
    if (!traceCtx)
        return {};
    return traceCtx->publishRootMaterializedValueIdentity(
        key, traceBackend, pathId, valueIdentityStamp);
}

void EvalState::rollbackRootMaterializedValueIdentity(
    const PublishedMaterializedIdentity & publication)
{
    if (traceCtx)
        traceCtx->rollbackRootMaterializedValueIdentity(publication);
}

std::optional<ValueIdentityStamp> EvalState::lookupValueIdentityStamp(const Value & v) const
{
    if (traceCtx)
        return traceCtx->lookupValueIdentityStamp(v);
    return std::nullopt;
}

bool EvalState::hasValueIdentityForTest(const Value * key) const
{
    return traceCtx && traceCtx->hasValueIdentity_ForTest(key);
}

bool EvalState::hasBindingsValueIdentityForTest(const Bindings * key) const
{
    return traceCtx && traceCtx->hasBindingsValueIdentity_ForTest(key);
}

void EvalState::recordThunkDeps(const Value & v, uint32_t epochStart)
{
    if (traceCtx)
        traceCtx->recordThunkDeps(v, epochStart);
}

void EvalState::rollbackReplayEpoch(uint32_t epochStart)
{
    if (traceCtx)
        traceCtx->rollbackReplayEpoch(epochStart);
}

void EvalState::replayMemoizedDeps(const Value & v)
{
    if (traceCtx)
        traceCtx->replayMemoizedDeps(v);
}

bool EvalState::mayHaveMemoizedDeps(const Value & v) const
{
    return traceCtx && traceCtx->replayStore.replayBloom.test(&v);
}

bool EvalState::sameValueIdentity(Value & v1, Value & v2)
{
    return traceCtx && traceCtx->sameValueIdentity(v1, v2);
}

void EvalState::eval(Expr * e, Value & v)
{
    e->eval(*this, baseEnv, v);
}

inline bool EvalState::evalBool(Env & env, Expr * e, const PosIdx pos, std::string_view errorCtx)
{
    try {
        Value v;
        e->eval(*this, env, v);
        if (v.type() != nBool)
            error<TypeError>(
                "expected a Boolean but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .withFrame(env, *e)
                .debugThrow();
        return v.boolean();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

inline void EvalState::evalAttrs(Env & env, Expr * e, Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        e->eval(*this, env, v);
        if (v.type() != nAttrs)
            error<TypeError>(
                "expected a set but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .withFrame(env, *e)
                .debugThrow();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

void Expr::eval(EvalState & state, Env & env, Value & v)
{
    unreachable();
}

void ExprInt::eval(EvalState & state, Env & env, Value & v)
{
    v = this->v;
}

void ExprFloat::eval(EvalState & state, Env & env, Value & v)
{
    v = this->v;
}

void ExprString::eval(EvalState & state, Env & env, Value & v)
{
    v = this->v;
}

void ExprPath::eval(EvalState & state, Env & env, Value & v)
{
    v = this->v;
}

Env * ExprAttrs::buildInheritFromEnv(EvalState & state, Env & up)
{
    Env & inheritEnv = state.mem.allocEnv(inheritFromExprs->size());
    inheritEnv.up = &up;

    Displacement displ = 0;
    for (auto from : *inheritFromExprs)
        inheritEnv.values[displ++] = from->maybeThunk(state, up);

    return &inheritEnv;
}

void ExprAttrs::eval(EvalState & state, Env & env, Value & v)
{
    auto bindings = state.buildBindings(attrs->size() + dynamicAttrs->size());
    auto dynamicEnv = &env;
    bool sort = false;

    if (recursive) {
        /* Create a new environment that contains the attributes in
           this `rec'. */
        Env & env2(state.mem.allocEnv(attrs->size()));
        env2.up = &env;
        dynamicEnv = &env2;
        Env * inheritEnv = inheritFromExprs ? buildInheritFromEnv(state, env2) : nullptr;

        AttrDefs::iterator overrides = attrs->find(state.s.overrides);
        bool hasOverrides = overrides != attrs->end();

        /* The recursive attributes are evaluated in the new
           environment, while the inherited attributes are evaluated
           in the original environment. */
        Displacement displ = 0;
        for (auto & i : *attrs) {
            Value * vAttr;
            if (hasOverrides && i.second.kind != AttrDef::Kind::Inherited) {
                vAttr = state.allocValue();
                mkThunk(*vAttr, *i.second.chooseByKind(&env2, &env, inheritEnv), i.second.e);
            } else
                vAttr = i.second.e->maybeThunk(state, *i.second.chooseByKind(&env2, &env, inheritEnv));
            env2.values[displ++] = vAttr;
            bindings.insert(i.first, vAttr, i.second.pos);
        }

        /* If the rec contains an attribute called `__overrides', then
           evaluate it, and add the attributes in that set to the rec.
           This allows overriding of recursive attributes, which is
           otherwise not possible.  (You can use the // operator to
           replace an attribute, but other attributes in the rec will
           still reference the original value, because that value has
           been substituted into the bodies of the other attributes.
           Hence we need __overrides.) */
        if (hasOverrides) {
            Value * vOverrides = (*bindings.bindings)[overrides->second.displ].value;
            state.forceAttrs(
                *vOverrides,
                [&]() { return vOverrides->determinePos(noPos); },
                "while evaluating the `__overrides` attribute");
            bindings.grow(state.buildBindings(bindings.capacity() + vOverrides->attrs()->size()));
            for (auto & i : *vOverrides->attrs()) {
                AttrDefs::iterator j = attrs->find(i.name);
                if (j != attrs->end()) {
                    (*bindings.bindings)[j->second.displ] = i;
                    env2.values[j->second.displ] = i.value;
                } else
                    bindings.push_back(i);
            }
            sort = true;
        }
    }

    else {
        Env * inheritEnv = inheritFromExprs ? buildInheritFromEnv(state, env) : nullptr;
        for (auto & i : *attrs)
            bindings.insert(
                i.first, i.second.e->maybeThunk(state, *i.second.chooseByKind(&env, &env, inheritEnv)), i.second.pos);
    }

    /* Dynamic attrs apply *after* rec and __overrides. */
    for (auto & i : *dynamicAttrs) {
        Value nameVal;
        i.nameExpr->eval(state, *dynamicEnv, nameVal);
        state.forceValue(nameVal, i.pos);
        if (nameVal.type() == nNull)
            continue;
        state.forceStringNoCtx(nameVal, i.pos, "while evaluating the name of a dynamic attribute");
        auto nameSym = state.symbols.create(nameVal.string_view());
        if (sort)
            // FIXME: inefficient
            bindings.bindings->sort();
        if (auto j = bindings.bindings->get(nameSym))
            state
                .error<EvalError>(
                    "dynamic attribute '%1%' already defined at %2%", state.symbols[nameSym], state.positions[j->pos])
                .atPos(i.pos)
                .withFrame(env, *this)
                .debugThrow();

        i.valueExpr->setName(nameSym);
        /* Keep sorted order so find can catch duplicates */
        bindings.insert(nameSym, i.valueExpr->maybeThunk(state, *dynamicEnv), i.pos);
        sort = true;
    }

    bindings.bindings->pos = pos;

    v.mkAttrs(sort ? bindings.finish() : bindings.alreadySorted());
}

void ExprLet::eval(EvalState & state, Env & env, Value & v)
{
    /* Create a new environment that contains the attributes in this
       `let'. */
    Env & env2(state.mem.allocEnv(attrs->attrs->size()));
    env2.up = &env;

    Env * inheritEnv = attrs->inheritFromExprs ? attrs->buildInheritFromEnv(state, env2) : nullptr;

    /* The recursive attributes are evaluated in the new environment,
       while the inherited attributes are evaluated in the original
       environment. */
    Displacement displ = 0;
    for (auto & i : *attrs->attrs) {
        env2.values[displ++] = i.second.e->maybeThunk(state, *i.second.chooseByKind(&env2, &env, inheritEnv));
    }

    auto dts = state.debugRepl
                   ? makeDebugTraceStacker(state, *this, env2, getPos(), "while evaluating a '%1%' expression", "let")
                   : nullptr;

    body->eval(state, env2, v);
}

void ExprList::eval(EvalState & state, Env & env, Value & v)
{
    auto list = state.buildList(elems.size());
    for (const auto & [n, v2] : enumerate(list))
        v2 = elems[n]->maybeThunk(state, env);
    v.mkList(list);
}

Value * ExprList::maybeThunk(EvalState & state, Env & env)
{
    if (elems.empty()) {
        return &Value::vEmptyList;
    }
    return Expr::maybeThunk(state, env);
}

void ExprVar::eval(EvalState & state, Env & env, Value & v)
{
    Value * v2 = state.lookupVar(&env, *this, false);
    state.forceValue(*v2, pos);
    v = *v2;
}

static std::string showAttrSelectionPath(EvalState & state, Env & env, std::span<const AttrName> attrPath)
{
    std::ostringstream out;
    bool first = true;
    for (auto & i : attrPath) {
        if (!first)
            out << '.';
        else
            first = false;
        try {
            out << state.symbols[getName(i, state, env)];
        } catch (Error & e) {
            assert(!i.symbol);
            out << "\"${";
            i.expr->show(state.symbols, out);
            out << "}\"";
        }
    }
    return out.str();
}

void ExprSelect::eval(EvalState & state, Env & env, Value & v)
{
    Value vTmp;
    PosIdx pos2;
    Value * vAttrs = &vTmp;

    e->eval(state, env, vTmp);

    try {
        auto dts = state.debugRepl ? makeDebugTraceStacker(
                                         state,
                                         *this,
                                         env,
                                         getPos(),
                                         "while evaluating the attribute '%1%'",
                                         showAttrSelectionPath(state, env, getAttrPath()))
                                   : nullptr;

        for (auto & i : getAttrPath()) {
            state.nrLookups++;
            const Attr * j;
            auto name = getName(i, state, env);
            if (def) {
                state.forceValue(*vAttrs, pos);
                if (vAttrs->type() != nAttrs || !(j = vAttrs->attrs()->get(name))) {
                    // Record #has:key for traced attrset where key was NOT found
                    if (state.traceActiveDepth && vAttrs->type() == nAttrs) [[unlikely]]
                        maybeRecordHasKeyDep(state.positions, state.symbols, *vAttrs, name, false);
                    def->eval(state, env, v);
                    return;
                }
            } else {
                state.forceAttrs(*vAttrs, pos, "while selecting an attribute");
                if (!(j = vAttrs->attrs()->get(name))) {
                    StringSet allAttrNames;
                    for (auto & attr : *vAttrs->attrs())
                        allAttrNames.insert(std::string(state.symbols[attr.name]));
                    auto suggestions = Suggestions::bestMatches(allAttrNames, state.symbols[name]);
                    state.error<EvalError>("attribute '%1%' missing", state.symbols[name])
                        .atPos(pos)
                        .withSuggestions(suggestions)
                        .withFrame(env, *this)
                        .debugThrow();
                }
            }
            vAttrs = j->value;
            pos2 = j->pos;
            // Record per-binding NixBinding dep if this attr was defined
            // in an eligible .nix file (PosIdx registered at parse time).
            if (state.traceActiveDepth) [[unlikely]] {
                if (auto access = eval_trace::TraceAccess::current())
                    maybeRecordNixBindingDep(*access, state, j->pos);
            }
            if (state.countCalls)
                state.attrSelects[pos2]++;
        }

        state.forceValue(*vAttrs, (pos2 ? pos2 : this->pos));

    } catch (Error & e) {
        if (pos2) {
            auto pos2r = state.positions[pos2];
            auto origin = std::get_if<SourcePath>(&pos2r.origin);
            if (!(origin && *origin == state.derivationInternal))
                state.addErrorTrace(
                    e, pos2, "while evaluating the attribute '%1%'", showAttrSelectionPath(state, env, getAttrPath()));
        }
        throw;
    }

    // Copy the forced Value into the caller's output. This is a struct copy:
    // the caller gets the same data (Bindings*, list pointers, etc.) but at
    // a different Value* address. Code that relies on Value* identity — such
    // as TraceRuntime::valueIdentityMap — cannot find these copies.
    // Copied attrsets recover eval-trace pointer equality through the owned
    // Bindings ValueIdentityStamp instead of a secondary Bindings*-keyed map.
    v = *vAttrs;
}

Symbol ExprSelect::evalExceptFinalSelect(EvalState & state, Env & env, Value & attrs)
{
    Value vTmp;
    Symbol name = getName(attrPathStart[nAttrPath - 1], state, env);

    if (nAttrPath == 1) {
        e->eval(state, env, vTmp);
    } else {
        ExprSelect init(*this);
        init.nAttrPath--;
        init.eval(state, env, vTmp);
    }
    attrs = vTmp;
    return name;
}

void ExprOpHasAttr::eval(EvalState & state, Env & env, Value & v)
{
    // Eval-trace soundness for multi-segment ? (e.g., `data ? x.y`):
    //
    // Only the FINAL segment records a #has:key dep. Intermediate segments
    // are navigated without recording #has deps. This is sound because:
    //
    // 1. The SC dep key encodes the full path (e.g., `j:x#has:y`).
    //    During verification, the system navigates the JSON DOM through
    //    all intermediate segments before checking the leaf. If any
    //    intermediate key is removed, navigation fails → dep fails →
    //    re-evaluation.
    //
    // 2. For multi-provenance attrsets (via //), ImplicitShape #keys deps
    //    recorded at creation time serve as a fallback. If a source's key
    //    set changes (e.g., another source adds a shadowing key), IS #keys
    //    detects the change → re-evaluation.
    //
    // Recording #has:key for intermediate segments would be redundant:
    // property (1) already catches removal, and property (2) catches
    // multi-source shadowing.

    Value vTmp;
    Value * vAttrs = &vTmp;
    Value * lastAttrset = nullptr;
    Symbol lastKeyName;

    e->eval(state, env, vTmp);

    for (auto & i : attrPath) {
        state.forceValue(*vAttrs, getPos());
        const Attr * j;
        auto name = getName(i, state, env);
        if (vAttrs->type() == nAttrs && (j = vAttrs->attrs()->get(name))) {
            lastAttrset = vAttrs;
            lastKeyName = name;
            // Record per-binding NixBinding dep (same as ExprSelect).
            if (state.traceActiveDepth) [[unlikely]] {
                if (auto access = eval_trace::TraceAccess::current())
                    maybeRecordNixBindingDep(*access, state, j->pos);
            }
            vAttrs = j->value;
        } else {
            // Record #has:key for the attrset where the key was NOT found.
            // Only the failure point gets a dep — intermediate successes
            // are covered by SC dep path navigation during verification.
            if (state.traceActiveDepth && vAttrs->type() == nAttrs) [[unlikely]]
                maybeRecordHasKeyDep(state.positions, state.symbols, *vAttrs, name, false);
            v.mkBool(false);
            return;
        }
    }

    // Record #has:key for the last attrset where the key was found.
    // Only the leaf segment gets a dep — see soundness comment above.
    if (state.traceActiveDepth && lastAttrset) [[unlikely]]
        maybeRecordHasKeyDep(state.positions, state.symbols, *lastAttrset, lastKeyName, true);
    v.mkBool(true);
}

void ExprLambda::eval(EvalState & state, Env & env, Value & v)
{
    v.mkLambda(&env, this);
}

void EvalState::callFunction(Value & fun, std::span<Value *> args, Value & vRes, const PosIdx pos)
{
    auto _level = addCallDepth(pos);

    auto neededHooks = profiler.getNeededHooks();
    if (neededHooks.test(EvalProfiler::preFunctionCall)) [[unlikely]]
        profiler.preFunctionCallHook(*this, fun, args, pos);

    Finally traceExit_{[&]() {
        if (profiler.getNeededHooks().test(EvalProfiler::postFunctionCall)) [[unlikely]]
            profiler.postFunctionCallHook(*this, fun, args, pos);
    }};

    forceValue(fun, pos);

    Value vCur(fun);

    auto makeAppChain = [&]() {
        for (auto arg : args) {
            auto fun2 = allocValue();
            *fun2 = vCur;
            vCur.mkPrimOpApp(fun2, arg);
        }
        vRes = vCur;
    };

    const Attr * functor;

    while (args.size() > 0) {

        if (vCur.isLambda()) {

            ExprLambda & lambda(*vCur.lambda().fun);

            auto size = (!lambda.arg ? 0 : 1) + (lambda.getFormals() ? lambda.getFormals()->formals.size() : 0);
            Env & env2(mem.allocEnv(size));
            env2.up = vCur.lambda().env;

            Displacement displ = 0;

            if (auto formals = lambda.getFormals()) {
                try {
                    forceAttrs(*args[0], lambda.pos, "while evaluating the value passed for the lambda argument");
                } catch (Error & e) {
                    if (pos)
                        e.addTrace(positions[pos], "from call site");
                    throw;
                }

                if (lambda.arg)
                    env2.values[displ++] = args[0];

                /* For each formal argument, get the actual argument.  If
                   there is no matching actual argument but the formal
                   argument has a default, use the default. */
                size_t attrsUsed = 0;
                for (auto & i : formals->formals) {
                    auto j = args[0]->attrs()->get(i.name);
                    if (!j) {
                        if (!i.def) {
                            error<TypeError>(
                                "function '%1%' called without required argument '%2%'",
                                (lambda.name ? std::string(symbols[lambda.name]) : "anonymous lambda"),
                                symbols[i.name])
                                .atPos(lambda.pos)
                                .withTrace(pos, "from call site")
                                .withFrame(*vCur.lambda().env, lambda)
                                .debugThrow();
                        }
                        env2.values[displ++] = i.def->maybeThunk(*this, env2);
                    } else {
                        attrsUsed++;
                        env2.values[displ++] = j->value;
                    }
                }

                /* Check that each actual argument is listed as a formal
                   argument (unless the attribute match specifies a `...'). */
                if (traceActiveDepth && !formals->ellipsis) [[unlikely]] maybeRecordAttrKeysDep(positions, symbols, *args[0]);
                if (!formals->ellipsis && attrsUsed != args[0]->attrs()->size()) {
                    /* Nope, so show the first unexpected argument to the
                       user. */
                    for (auto & i : *args[0]->attrs())
                        if (!formals->has(i.name)) {
                            StringSet formalNames;
                            for (auto & formal : formals->formals)
                                formalNames.insert(std::string(symbols[formal.name]));
                            auto suggestions = Suggestions::bestMatches(formalNames, symbols[i.name]);
                            error<TypeError>(
                                "function '%1%' called with unexpected argument '%2%'",
                                (lambda.name ? std::string(symbols[lambda.name]) : "anonymous lambda"),
                                symbols[i.name])
                                .atPos(lambda.pos)
                                .withTrace(pos, "from call site")
                                .withSuggestions(suggestions)
                                .withFrame(*vCur.lambda().env, lambda)
                                .debugThrow();
                        }
                    unreachable();
                }
            } else {
                env2.values[displ++] = args[0];
            }

            nrFunctionCalls++;
            if (countCalls)
                incrFunctionCall(&lambda);

            /* Evaluate the body. */
            try {
                auto dts = debugRepl
                               ? makeDebugTraceStacker(
                                     *this,
                                     *lambda.body,
                                     env2,
                                     lambda.pos,
                                     "while calling %s",
                                     lambda.name ? concatStrings("'", symbols[lambda.name], "'") : "anonymous lambda")
                               : nullptr;

                lambda.body->eval(*this, env2, vCur);
            } catch (Error & e) {
                if (loggerSettings.showTrace.get()) {
                    addErrorTrace(
                        e,
                        lambda.pos,
                        "while calling %s",
                        lambda.name ? concatStrings("'", symbols[lambda.name], "'") : "anonymous lambda");
                    if (pos)
                        addErrorTrace(e, pos, "from call site");
                }
                throw;
            }

            args = args.subspan(1);
        }

        else if (vCur.isPrimOp()) {

            size_t argsLeft = vCur.primOp()->arity;

            if (args.size() < argsLeft) {
                /* We don't have enough arguments, so create a tPrimOpApp chain. */
                makeAppChain();
                return;
            } else {
                /* We have all the arguments, so call the primop. */
                auto * fn = vCur.primOp();

                nrPrimOpCalls++;
                if (countCalls)
                    primOpCalls[fn->name]++;

                try {
                    fn->impl(*this, vCur.determinePos(noPos), args.data(), vCur);
                } catch (Error & e) {
                    if (fn->addTrace)
                        addErrorTrace(e, pos, "while calling the '%1%' builtin", fn->name);
                    throw;
                }

                args = args.subspan(argsLeft);
            }
        }

        else if (vCur.isPrimOpApp()) {
            /* Figure out the number of arguments still needed. */
            size_t argsDone = 0;
            Value * primOp = &vCur;
            while (primOp->isPrimOpApp()) {
                argsDone++;
                primOp = primOp->primOpApp().left;
            }
            assert(primOp->isPrimOp());
            auto arity = primOp->primOp()->arity;
            auto argsLeft = arity - argsDone;

            if (args.size() < argsLeft) {
                /* We still don't have enough arguments, so extend the tPrimOpApp chain. */
                makeAppChain();
                return;
            } else {
                /* We have all the arguments, so call the primop with
                   the previous and new arguments. */

                Value * vArgs[maxPrimOpArity];
                auto n = argsDone;
                for (Value * arg = &vCur; arg->isPrimOpApp(); arg = arg->primOpApp().left)
                    vArgs[--n] = arg->primOpApp().right;

                for (size_t i = 0; i < argsLeft; ++i)
                    vArgs[argsDone + i] = args[i];

                auto fn = primOp->primOp();
                nrPrimOpCalls++;
                if (countCalls)
                    primOpCalls[fn->name]++;

                try {
                    // TODO:
                    // 1. Unify this and above code. Heavily redundant.
                    // 2. Create a fake env (arg1, arg2, etc.) and a fake expr (arg1: arg2: etc: builtins.name arg1 arg2
                    // etc)
                    //    so the debugger allows to inspect the wrong parameters passed to the builtin.
                    fn->impl(*this, vCur.determinePos(noPos), vArgs, vCur);
                } catch (Error & e) {
                    if (fn->addTrace)
                        addErrorTrace(e, pos, "while calling the '%1%' builtin", fn->name);
                    throw;
                }

                args = args.subspan(argsLeft);
            }
        }

        else if (vCur.type() == nAttrs && (functor = vCur.attrs()->get(s.functor))) {
            /* 'vCur' may be allocated on the stack of the calling
               function, but for functors we may keep a reference, so
               heap-allocate a copy and use that instead. */
            Value * args2[] = {allocValue(), args[0]};
            *args2[0] = vCur;
            try {
                callFunction(*functor->value, args2, vCur, functor->pos);
            } catch (Error & e) {
                e.addTrace(positions[pos], "while calling a functor (an attribute set with a '__functor' attribute)");
                throw;
            }
            args = args.subspan(1);
        }

        else
            error<TypeError>(
                "attempt to call something which is not a function but %1%: %2%",
                showType(vCur),
                ValuePrinter(*this, vCur, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
    }

    vRes = vCur;
}

void ExprCall::eval(EvalState & state, Env & env, Value & v)
{
    auto dts =
        state.debugRepl ? makeDebugTraceStacker(state, *this, env, getPos(), "while calling a function") : nullptr;

    Value vFun;
    fun->eval(state, env, vFun);

    // Empirical arity of Nixpkgs lambdas by regex e.g. ([a-zA-Z]+:(\s|(/\*.*\/)|(#.*\n))*){5}
    // 2: over 4000
    // 3: about 300
    // 4: about 60
    // 5: under 10
    // This excluded attrset lambdas (`{...}:`). Contributions of mixed lambdas appears insignificant at ~150 total.
    SmallValueVector<4> vArgs(args->size());
    for (size_t i = 0; i < args->size(); ++i)
        vArgs[i] = (*args)[i]->maybeThunk(state, env);

    state.callFunction(vFun, vArgs, v, pos);
}

// Lifted out of callFunction() because it creates a temporary that
// prevents tail-call optimisation.
void EvalState::incrFunctionCall(ExprLambda * fun)
{
    functionCalls[fun]++;
}

void EvalState::autoCallFunction(const Bindings & args, Value & fun, Value & res)
{
    auto pos = fun.determinePos(noPos);

    forceValue(fun, pos);

    if (fun.type() == nAttrs) {
        auto found = fun.attrs()->get(s.functor);
        if (found) {
            Value * v = allocValue();
            callFunction(*found->value, fun, *v, pos);
            forceValue(*v, pos);
            return autoCallFunction(args, *v, res);
        }
    }

    if (!fun.isLambda() || !fun.lambda().fun->getFormals()) {
        res = fun;
        return;
    }
    auto formals = fun.lambda().fun->getFormals();

    auto attrs = buildBindings(std::max(static_cast<uint32_t>(formals->formals.size()), args.size()));

    if (formals->ellipsis) {
        // If the formals have an ellipsis (eg the function accepts extra args) pass
        // all available automatic arguments (which includes arguments specified on
        // the command line via --arg/--argstr)
        for (auto & v : args)
            attrs.insert(v);
    } else {
        // Otherwise, only pass the arguments that the function accepts
        for (auto & i : formals->formals) {
            auto j = args.get(i.name);
            if (j) {
                attrs.insert(*j);
            } else if (!i.def) {
                error<MissingArgumentError>(
                    R"(cannot evaluate a function that has an argument without a value ('%1%')
Nix attempted to evaluate a function as a top level expression; in
this case it must have its arguments supplied either by default
values, or passed explicitly with '--arg' or '--argstr'. See
https://nix.dev/manual/nix/stable/language/syntax.html#functions.)",
                    symbols[i.name])
                    .atPos(i.pos)
                    .withFrame(*fun.lambda().env, *fun.lambda().fun)
                    .debugThrow();
            }
        }
    }

    callFunction(fun, allocValue()->mkAttrs(attrs), res, pos);
}

void ExprWith::eval(EvalState & state, Env & env, Value & v)
{
    Env & env2(state.mem.allocEnv(1));
    env2.up = &env;
    env2.values[0] = attrs->maybeThunk(state, env);

    body->eval(state, env2, v);
}

void ExprIf::eval(EvalState & state, Env & env, Value & v)
{
    // We cheat in the parser, and pass the position of the condition as the position of the if itself.
    (state.evalBool(env, cond, pos, "while evaluating a branch condition") ? then : else_)->eval(state, env, v);
}

void ExprAssert::eval(EvalState & state, Env & env, Value & v)
{
    if (!state.evalBool(env, cond, pos, "in the condition of the assert statement")) {
        std::ostringstream out;
        cond->show(state.symbols, out);
        auto exprStr = out.view();

        if (auto eq = dynamic_cast<ExprOpEq *>(cond)) {
            try {
                Value v1;
                eq->e1->eval(state, env, v1);
                Value v2;
                eq->e2->eval(state, env, v2);
                state.assertEqValues(v1, v2, eq->pos, "in an equality assertion");
            } catch (AssertionError & e) {
                e.addTrace(state.positions[pos], "while evaluating the condition of the assertion '%s'", exprStr);
                throw;
            }
        }

        state.error<AssertionError>("assertion '%1%' failed", exprStr).atPos(pos).withFrame(env, *this).debugThrow();
    }
    body->eval(state, env, v);
}

void ExprOpNot::eval(EvalState & state, Env & env, Value & v)
{
    v.mkBool(!state.evalBool(env, e, getPos(), "in the argument of the not operator")); // XXX: FIXME: !
}

void ExprOpEq::eval(EvalState & state, Env & env, Value & v)
{
    // v1 and v2 are stack-local Values. When e1/e2 are ExprSelect, the result
    // is a struct copy of the Bindings Value (see ExprSelect::eval). These
    // copies have different Value* addresses than the originals, which matters
    // for eval-trace's valueIdentityMap (keyed by Value*). Attrset copies now
    // recover identity through the copied value's Bindings-local stamp.
    Value v1;
    e1->eval(state, env, v1);
    Value v2;
    e2->eval(state, env, v2);
    v.mkBool(state.eqValues(v1, v2, pos, "while testing two values for equality"));
}

void ExprOpNEq::eval(EvalState & state, Env & env, Value & v)
{
    Value v1;
    e1->eval(state, env, v1);
    Value v2;
    e2->eval(state, env, v2);
    v.mkBool(!state.eqValues(v1, v2, pos, "while testing two values for inequality"));
}

void ExprOpAnd::eval(EvalState & state, Env & env, Value & v)
{
    v.mkBool(
        state.evalBool(env, e1, pos, "in the left operand of the AND (&&) operator")
        && state.evalBool(env, e2, pos, "in the right operand of the AND (&&) operator"));
}

void ExprOpOr::eval(EvalState & state, Env & env, Value & v)
{
    v.mkBool(
        state.evalBool(env, e1, pos, "in the left operand of the OR (||) operator")
        || state.evalBool(env, e2, pos, "in the right operand of the OR (||) operator"));
}

void ExprOpImpl::eval(EvalState & state, Env & env, Value & v)
{
    v.mkBool(
        !state.evalBool(env, e1, pos, "in the left operand of the IMPL (->) operator")
        || state.evalBool(env, e2, pos, "in the right operand of the IMPL (->) operator"));
}

void ExprOpUpdate::eval(EvalState & state, Value & v, Value & v1, Value & v2)
{
    state.nrOpUpdates++;

    // ExprOpUpdate (//) is a set UNION — the output can contain keys from
    // both operands, making it potentially larger than either source.  This
    // violates the shape-preserving requirement (output.keys ⊆ source.keys)
    // of DerivedContainerBuilder.  Two disjoint subsets of the same traced
    // container (e.g. removeAttrs x ks1 // removeAttrs x ks2) would reunite
    // into an output larger than either, triggering the finishAttrs assertion.
    //
    // DerivedContainerBuilder is therefore only used for the empty-operand
    // alias paths where output == one input (trivially shape-preserving).  The
    // builder is fed only the operand actually returned; the other operand may
    // have been observed to decide the branch, but it is not the output's
    // container identity.  For actual merge paths, we call mkAttrs directly.
    // Per-key attribute-level deps still flow through Attr::pos on those paths.
    const Bindings & bindings1 = *v1.attrs();
    if (bindings1.empty()) {
        v = v2;
        eval_trace::DerivedAttrsBuilder aliasBuilder;
        aliasBuilder.addShapePreservingSource(v2);
        aliasBuilder.registerContainer(v);
        return;
    }

    const Bindings & bindings2 = *v2.attrs();
    if (bindings2.empty()) {
        v = v1;
        eval_trace::DerivedAttrsBuilder aliasBuilder;
        aliasBuilder.addShapePreservingSource(v1);
        aliasBuilder.registerContainer(v);
        return;
    }

    /* Simple heuristic for determining whether attrs2 should be "layered" on top of
       attrs1 instead of copying to a new Bindings. */
    bool shouldLayer = [&]() -> bool {
        if (bindings1.isLayerListFull())
            return false;

        if (bindings2.size() > state.settings.bindingsUpdateLayerRhsSizeThreshold)
            return false;

        return true;
    }();

    if (shouldLayer) {
        auto attrs = state.buildBindings(bindings2.size());
        attrs.layerOnTopOf(bindings1);

        std::ranges::copy(bindings2, std::back_inserter(attrs));
        v.mkAttrs(attrs.alreadySorted());

        state.nrOpUpdateValuesCopied += bindings2.size();
        return;
    }

    auto attrs = state.buildBindings(bindings1.size() + bindings2.size());

    /* Merge the sets, preferring values from the second set.  Make
       sure to keep the resulting vector in sorted order. */
    auto i = bindings1.begin();
    auto j = bindings2.begin();

    while (i != bindings1.end() && j != bindings2.end()) {
        if (i->name == j->name) {
            attrs.insert(*j);
            ++i;
            ++j;
        } else if (i->name < j->name) {
            attrs.insert(*i);
            ++i;
        } else {
            attrs.insert(*j);
            ++j;
        }
    }

    while (i != bindings1.end()) {
        attrs.insert(*i);
        ++i;
    }

    while (j != bindings2.end()) {
        attrs.insert(*j);
        ++j;
    }

    v.mkAttrs(attrs.alreadySorted());

    state.nrOpUpdateValuesCopied += v.attrs()->size();
}

void ExprOpUpdate::eval(EvalState & state, Env & env, Value & v)
{
    UpdateQueue q;
    evalForUpdate(state, env, q);

    Value vTmp;
    vTmp.mkAttrs(&Bindings::emptyBindings);

    for (auto & rhs : std::views::reverse(q)) {
        /* Remember that queue is sorted rightmost attrset first. */
        eval(state, /*v=*/vTmp, /*v1=*/vTmp, /*v2=*/rhs);
    }

    v = vTmp;
}

void Expr::evalForUpdate(EvalState & state, Env & env, UpdateQueue & q, std::string_view errorCtx)
{
    Value v;
    state.evalAttrs(env, this, v, getPos(), errorCtx);
    q.push_back(v);
}

void ExprOpUpdate::evalForUpdate(EvalState & state, Env & env, UpdateQueue & q)
{
    /* Output rightmost attrset first to the merge queue as the one
       with the most priority. */
    e2->evalForUpdate(state, env, q, "in the right operand of the update (//) operator");
    e1->evalForUpdate(state, env, q, "in the left operand of the update (//) operator");
}

void ExprOpUpdate::evalForUpdate(EvalState & state, Env & env, UpdateQueue & q, std::string_view errorCtx)
{
    evalForUpdate(state, env, q);
}

void ExprOpConcatLists::eval(EvalState & state, Env & env, Value & v)
{
    Value v1;
    e1->eval(state, env, v1);
    Value v2;
    e2->eval(state, env, v2);
    Value * lists[2] = {&v1, &v2};
    state.concatLists(v, lists, pos, "while evaluating one of the elements to concatenate");
}

/// Concatenate multiple lists into one.
///
/// NOTE: Does NOT use DerivedContainerBuilder.  The output list's length
/// is the sum of all input lengths — not a shape property of any single
/// source.  DerivedContainerBuilder would propagate provenance from one
/// source, but then maybeRecordListLenDep would record #len = (sum of
/// all lengths) keyed to that source's identity.  Verification would
/// re-read that single source and compute its length (not the sum),
/// producing a hash mismatch.  DerivedContainerBuilder is only correct
/// for shape-preserving operations where the output's shape is a subset
/// or reordering of a single source's shape (filter, sort, removeAttrs).
///
/// The alias path (single non-empty list, v = *nonEmpty) copies the
/// Value bitwise, preserving the same heap-backed list-storage pointer.
///
/// Container provenance lookup for lists is storage-identity-based:
/// ContainerRef (trace-frame.hh) uses the heap-backed list-storage pointer as
/// the hash/eq key, NOT the enclosing Value* address.  Because the
/// bitwise copy preserves the same storage pointer, lookupTracedContainer
/// on the alias returns the source's TracedContainerProvenance without
/// any new registerTracedContainer call.  buildCachedResult therefore
/// serialises TracedContainerMeta correctly for the alias.
///
/// The #len dep is recorded on the source lists (maybeRecordListLenDep
/// in the loop below) before the alias branch; the dep identity is correct because it uses the source
/// provenance, which is the same provenance the alias resolves to.
///
/// INVARIANT: this alias is correct only because v = *nonEmpty is a
/// bitwise copy that preserves list storage identity.  Do NOT replace
/// with a manual rebuild (buildList + memcpy) — that would create a new
/// elems array with different pointer identity, breaking the storage-keyed
/// ContainerProvenanceRegistry lookup.  Use the bitwise copy.
void EvalState::concatLists(Value & v, std::span<Value * const> lists, const PosIdx pos, std::string_view errorCtx)
{
    nrListConcats++;

    Value * nonEmpty = nullptr;
    size_t len = 0;
    for (auto * list : lists) {
        forceList(*list, pos, errorCtx);
        if (traceActiveDepth) [[unlikely]] maybeRecordListLenDep(*list);
        auto l = list->listSize();
        len += l;
        if (l)
            nonEmpty = list;
    }

    if (nonEmpty && len == nonEmpty->listSize()) {
        v = *nonEmpty;
        return;
    }

    auto list = buildList(len);
    auto out = list.elems;
    size_t pos2 = 0;
    for (auto * l : lists) {
        auto listView = l->listView();
        auto n = listView.size();
        if (n)
            memcpy(out + pos2, listView.data(), n * sizeof(Value *));
        pos2 += n;
    }
    v.mkList(list);
}

void ExprConcatStrings::eval(EvalState & state, Env & env, Value & v)
{
    NixStringContext context;
    std::vector<BackedStringView> strings;
    size_t sSize = 0;
    NixInt n{0};
    NixFloat nf = 0;

    bool first = !forceString;
    ValueType firstType = nString;

    /* Accessor of the first element, captured when it's a path, so the
       result inherits handle identity rather than being rebuilt onto
       `state.rootFS`. */
    SourceAccessor * firstPathAccessor = nullptr;

    // List of returned strings. References to these Values must NOT be persisted.
    SmallTemporaryValueVector<conservativeStackReservation> values(es.size());
    Value * vTmpP = values.data();

    for (auto & [i_pos, i] : es) {
        Value & vTmp = *vTmpP++;
        i->eval(state, env, vTmp);

        /* If the first element is a path, then the result will also
           be a path, we don't copy anything (yet - that's done later,
           since paths are copied when they are used in a derivation),
           and none of the strings are allowed to have contexts. */
        if (first) {
            firstType = vTmp.type();
            if (firstType == nPath)
                firstPathAccessor = vTmp.pathAccessor();
        }

        if (firstType == nInt) {
            if (vTmp.type() == nInt) {
                auto newN = n + vTmp.integer();
                if (auto checked = newN.valueChecked(); checked.has_value()) {
                    n = NixInt(*checked);
                } else {
                    state.error<EvalError>("integer overflow in adding %1% + %2%", n, vTmp.integer())
                        .atPos(i_pos)
                        .debugThrow();
                }
            } else if (vTmp.type() == nFloat) {
                // Upgrade the type from int to float;
                firstType = nFloat;
                nf = n.value;
                nf += vTmp.fpoint();
            } else
                state.error<EvalError>("cannot add %1% to an integer", showType(vTmp))
                    .atPos(i_pos)
                    .withFrame(env, *this)
                    .debugThrow();
        } else if (firstType == nFloat) {
            if (vTmp.type() == nInt) {
                nf += vTmp.integer().value;
            } else if (vTmp.type() == nFloat) {
                nf += vTmp.fpoint();
            } else
                state.error<EvalError>("cannot add %1% to a float", showType(vTmp))
                    .atPos(i_pos)
                    .withFrame(env, *this)
                    .debugThrow();
        } else {
            if (strings.empty())
                strings.reserve(es.size());
            /* skip canonization of first path, which would only be not
            canonized in the first place if it's coming from a ./${foo} type
            path */
            auto part = state.coerceToString(
                i_pos, vTmp, context, "while evaluating a path segment", false, firstType == nString, !first);
            sSize += part->size();
            strings.emplace_back(std::move(part));
        }

        first = false;
    }

    if (firstType == nInt) {
        v.mkInt(n);
    } else if (firstType == nFloat) {
        v.mkFloat(nf);
    } else if (firstType == nPath) {
        if (!context.empty())
            state.error<EvalError>("a string that refers to a store path cannot be appended to a path")
                .atPos(pos)
                .withFrame(env, *this)
                .debugThrow();
        std::string resultStr;
        resultStr.reserve(sSize);
        for (const auto & part : strings) {
            resultStr += *part;
        }
        assert(firstPathAccessor && "firstType==nPath implies firstPathAccessor was set");
        v.mkPath(
            SourcePath{
                ref(firstPathAccessor->shared_from_this()),
                CanonPath(resultStr),
            },
            state.mem);
    } else {
        auto & resultStr = StringData::alloc(state.mem, sSize);
        auto * tmp = resultStr.data();
        for (const auto & part : strings) {
            std::memcpy(tmp, part->data(), part->size());
            tmp += part->size();
        }
        *tmp = '\0';
        v.mkStringMove(resultStr, context, state.mem);
    }

    if (firstType == nPath || firstType == nString) {
        std::optional<PathObject> propagated;
        bool ambiguous = false;
        for (const auto & value : values) {
            auto pub = state.lookupSemanticHandle(value);
            auto found = pub ? pub->path : std::optional<PathObject>{};
            if (!found)
                continue;
            if (!propagated) {
                propagated = *found;
                continue;
            }
            if (propagated->source != found->source || propagated->rootPath != found->rootPath) {
                ambiguous = true;
                break;
            }
        }

        if (!ambiguous && propagated)
            state.publishPathProvenance(v, *propagated);
    }
}

void ExprPos::eval(EvalState & state, Env & env, Value & v)
{
    state.mkPos(v, pos);
}

void ExprBlackHole::eval(EvalState & state, [[maybe_unused]] Env & env, Value & v)
{
    throwInfiniteRecursionError(state, v);
}

[[gnu::noinline]] [[noreturn]] void ExprBlackHole::throwInfiniteRecursionError(EvalState & state, Value & v)
{
    state.error<InfiniteRecursionError>("infinite recursion encountered").atPos(v.determinePos(noPos)).debugThrow();
}

// always force this to be separate, otherwise forceValue may inline it and take
// a massive perf hit
[[gnu::noinline]]
void EvalState::handleEvalExceptionForThunk(Env * env, Expr * expr, Value & v, const PosIdx pos)
{
    if (!env)
        tryFixupBlackHolePos(v, pos);

    auto e = std::current_exception();
    Value * recovery = nullptr;
    try {
        std::rethrow_exception(e);
    } catch (const RecoverableEvalError & e) {
        recovery = allocValue();
    } catch (...) {
    }
    if (recovery) {
        recovery->mkThunk(env, expr);
    }
    v.mkFailed(e, recovery);
}

[[gnu::noinline]]
void EvalState::handleEvalExceptionForApp(Value & v, const Value & savedApp)
{
    auto e = std::current_exception();
    Value * recovery = nullptr;
    try {
        std::rethrow_exception(e);
    } catch (const RecoverableEvalError & e) {
        recovery = allocValue();
    } catch (...) {
    }
    if (recovery) {
        *recovery = savedApp;
    }
    v.mkFailed(e, recovery);
}

[[gnu::noinline]]
void EvalState::handleEvalFailed(Value & v, const PosIdx pos)
{
    assert(v.isFailed());
    if (auto recoveryValue = v.failed().recoveryValue) {
        v = *recoveryValue;
        forceValue(v, pos);
    } else {
        v.failed().rethrow();
    }
}

void EvalState::tryFixupBlackHolePos(Value & v, PosIdx pos)
{
    if (!v.isBlackhole())
        return;
    auto e = std::current_exception();
    try {
        std::rethrow_exception(e);
    } catch (InfiniteRecursionError & e) {
        if (!e.hasPos())
            e.atPos(positions[pos]);
    } catch (...) {
    }
}

void EvalState::forceValueDeep(Value & v)
{
    std::set<const Value *> seen;

    [&, &state(*this)](this const auto & recurse, Value & v) {
        auto _level = state.addCallDepth(v.determinePos(noPos));

        if (!seen.insert(&v).second)
            return;

        state.forceValue(v, v.determinePos(noPos));

        if (v.type() == nAttrs) {
            for (auto & i : *v.attrs())
                try {
                    // If the value is a thunk, we're evaling. Otherwise no trace necessary.
                    auto dts = state.debugRepl && i.value->isThunk() ? makeDebugTraceStacker(
                                                                           state,
                                                                           *i.value->thunk().expr,
                                                                           *i.value->thunk().env,
                                                                           i.pos,
                                                                           "while evaluating the attribute '%1%'",
                                                                           state.symbols[i.name])
                                                                     : nullptr;

                    recurse(*i.value);
                } catch (Error & e) {
                    state.addErrorTrace(e, i.pos, "while evaluating the attribute '%1%'", state.symbols[i.name]);
                    throw;
                }
        }

        else if (v.isList()) {
            size_t index = 0;
            for (auto v2 : v.listView())
                try {
                    recurse(*v2);
                    index++;
                } catch (Error & e) {
                    state.addErrorTrace(e, "while evaluating list element at index %1%", index);
                    throw;
                }
        }
    }(v);
}

NixInt EvalState::forceInt(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type() != nInt)
            error<TypeError>(
                "expected an integer but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.integer();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }

    return v.integer();
}

NixFloat EvalState::forceFloat(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type() == nInt)
            return v.integer().value;
        else if (v.type() != nFloat)
            error<TypeError>(
                "expected a float but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.fpoint();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

bool EvalState::forceBool(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type() != nBool)
            error<TypeError>(
                "expected a Boolean but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.boolean();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }

    return v.boolean();
}

const Attr * EvalState::getAttr(Symbol attrSym, const Bindings * attrSet, std::string_view errorCtx)
{
    auto value = attrSet->get(attrSym);
    if (!value) {
        error<TypeError>("attribute '%s' missing", symbols[attrSym]).withTrace(noPos, errorCtx).debugThrow();
    }
    return value;
}

bool EvalState::isFunctor(const Value & fun) const
{
    return fun.type() == nAttrs && fun.attrs()->get(s.functor);
}

void EvalState::forceListObserved(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    forceList(v, pos, errorCtx);
    if (traceActiveDepth) [[unlikely]] maybeRecordListLenDep(v);
}

void EvalState::forceFunction(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type() != nFunction && !isFunctor(v))
            error<TypeError>(
                "expected a function but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

std::string_view EvalState::forceString(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    try {
        forceValue(v, pos);
        if (v.type() != nString)
            error<TypeError>(
                "expected a string but found %1%: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .atPos(pos)
                .debugThrow();
        return v.string_view();
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }
}

void copyContext(const Value & v, NixStringContext & context, const ExperimentalFeatureSettings & xpSettings)
{
    if (auto * ctx = v.context())
        for (auto * elem : *ctx)
            context.insert(NixStringContextElem::parse(elem->view(), xpSettings));
}

static const SemanticHandle * allocateSemanticHandle(
    EvalMemory & mem,
    const std::optional<SemanticHandle> & publication)
{
    if (!publication || publication->empty())
        return nullptr;
    return new (mem.allocBytes(sizeof(SemanticHandle))) SemanticHandle(*publication);
}

std::optional<SemanticHandle> EvalState::lookupSemanticHandle(const Value & v) const
{
    if (auto * publication = v.publication()) {
        if (!publication->empty())
            return *publication;
    }

    return std::nullopt;
}

std::string_view ContextObject::view() const
{
    return std::visit(
        [](const auto & inner) -> std::string_view {
            return *inner.value;
        },
        inner_);
}

void EvalState::setSemanticHandle(Value & v, const std::optional<SemanticHandle> & publication) const
{
    if (!publication || publication->empty())
        return;

    auto & memRef = const_cast<EvalMemory &>(mem);
    auto carriedPublication = allocateSemanticHandle(memRef, publication);
    if (!carriedPublication)
        return;

    if (v.type() == nString) {
        auto text = std::string(v.string_view());
        NixStringContext copied;
        if (auto * ctx = v.context())
            for (auto * elem : *ctx)
                copied.insert(NixStringContextElem::parse(elem->view()));
        if (copied.empty())
            v.mkString(text, memRef, carriedPublication);
        else
            v.mkString(text, copied, memRef, carriedPublication);
    } else if (v.type() == nPath) {
        v.mkPath(v.path(), memRef, carriedPublication);
    }
}

void EvalState::mergeSemanticHandle(Value & v, const std::optional<SemanticHandle> & publication) const
{
    if (!publication || publication->empty())
        return;

    auto merged = lookupSemanticHandle(v).value_or(SemanticHandle{});
    if (publication->path)
        merged.path = publication->path;
    if (publication->text)
        merged.text = publication->text;
    if (publication->identity)
        merged.identity = publication->identity;

    // Update the kind discriminator to match the merged fields.
    if (merged.path && merged.text)
        merged.kind = SemanticKind::PathText;
    else if (merged.path)
        merged.kind = SemanticKind::Path;
    else if (merged.text)
        merged.kind = SemanticKind::Text;

    if (v.type() != nString && v.type() != nPath) {
        return;
    }
    setSemanticHandle(v, std::optional<SemanticHandle>(merged));
}

ContextObject EvalState::captureContextObject(
    std::string_view value,
    const Value & source) const
{
    return ContextObject{
        ContextObject::PreservedString{
            .value = BackedStringView(value),
            .publication = lookupSemanticHandle(source).value_or(SemanticHandle{}),
        }};
}

EvalState::CoercedPath EvalState::captureCoercedPath(
    SourcePath value,
    const Value & source) const
{
    auto handle = lookupSemanticHandle(source);
    return capturePathWithObject(std::move(value), handle ? handle->path : std::nullopt);
}

EvalState::CoercedPath EvalState::capturePathWithObject(
    SourcePath value,
    std::optional<PathObject> origin) const
{
    return CoercedPath(std::move(value), std::move(origin));
}

void EvalState::publishContextObject(
    Value & v,
    ContextObject && coerced,
    NixStringContext context)
{
    std::visit(
        [&](const auto & inner) {
            if (context.empty())
                v.mkString(*inner.value, mem);
            else
                v.mkString(*inner.value, context, mem);

            using Inner = std::decay_t<decltype(inner)>;
            if constexpr (std::is_same_v<Inner, ContextObject::PreservedString>) {
                mergeSemanticHandle(v, inner.publication.empty() ? std::nullopt : std::optional<SemanticHandle>(inner.publication));
            }
        },
        coerced.inner_);
}

void EvalState::publishCoercedPath(
    Value & v,
    CoercedPath && coerced)
{
    v.mkPath(coerced.value_, mem);
    if (coerced.origin_)
        mergeSemanticHandle(v, SemanticHandle::forPath(*coerced.origin_));
}

bool ContextObject::isDetached() const
{
    return std::holds_alternative<DetachedStorePathString>(inner_);
}

std::string_view EvalState::forceString(
    Value & v,
    NixStringContext & context,
    const PosIdx pos,
    std::string_view errorCtx,
    const ExperimentalFeatureSettings & xpSettings)
{
    auto s = forceString(v, pos, errorCtx);
    copyContext(v, context, xpSettings);
    return s;
}

std::string_view EvalState::forceStringNoCtx(Value & v, const PosIdx pos, std::string_view errorCtx)
{
    auto s = forceString(v, pos, errorCtx);
    if (v.context()) {
        auto ctxElem = NixStringContextElem::parse((*v.context()->begin())->view());
        error<EvalError>(
            "the string '%1%' is not allowed to refer to a store path (such as '%2%')",
            v.string_view(),
            ctxElem.display(*store))
            .withTrace(pos, errorCtx)
            .debugThrow();
    }
    return s;
}

bool EvalState::isDerivation(Value & v)
{
    if (v.type() != nAttrs)
        return false;
    auto i = v.attrs()->get(s.type);
    if (!i)
        return false;
    forceValue(*i->value, i->pos);
    if (i->value->type() != nString)
        return false;
    return i->value->string_view().compare("derivation") == 0;
}

std::optional<std::string>
EvalState::tryAttrsToString(
    const PosIdx pos,
    Value & v,
    NixStringContext & context,
    bool coerceMore,
    bool copyToStore)
{
    return tryAttrsToString(pos, v, context, coerceMore, copyToStore, nullptr, nullptr);
}

std::optional<std::string>
EvalState::tryAttrsToString(
    const PosIdx pos,
    Value & v,
    NixStringContext & context,
    bool coerceMore,
    bool copyToStore,
    std::optional<PathObject> * origin,
    std::optional<TextObject> * readFileProvenance)
{
    auto i = v.attrs()->get(s.toString);
    if (i) {
        Value v1;
        callFunction(*i->value, v, v1, pos);
        return coerceToStringWithProvenance(
                   pos,
                   v1,
                   context,
                   "while evaluating the result of the `__toString` attribute",
                   coerceMore,
                   copyToStore,
                   true,
                   origin,
                   readFileProvenance)
            .toOwned();
    }

    return {};
}

BackedStringView EvalState::coerceToStringWithProvenance(
    const PosIdx pos,
    Value & v,
    NixStringContext & context,
    std::string_view errorCtx,
    bool coerceMore,
    bool copyToStore,
    bool canonicalizePath,
    std::optional<PathObject> * origin,
    std::optional<TextObject> * readFileProvenance)
{
    auto _level = addCallDepth(pos);

    forceValue(v, pos);

    if (origin)
        origin->reset();
    if (readFileProvenance)
        readFileProvenance->reset();

    if (v.type() == nString) {
        copyContext(v, context);
        if (origin || readFileProvenance) {
            auto handle = lookupSemanticHandle(v);
            if (origin)
                *origin = handle ? handle->path : std::nullopt;
            if (readFileProvenance)
                *readFileProvenance = handle ? handle->text : std::nullopt;
        }
        return v.string_view();
    }

    if (v.type() == nPath) {
        if (origin) {
            auto handle = lookupSemanticHandle(v);
            *origin = handle ? handle->path : std::nullopt;
        }
        if (!canonicalizePath && !copyToStore) {
            // FIXME: hack to preserve path literals that end in a
            // slash, as in /foo/${x}.
            return v.pathStrView();
        } else if (copyToStore) {
            auto handle = lookupSemanticHandle(v);
            return BackedStringView(
                std::string(copyPathToStoreViaEvalEnvironment(
                    *this,
                    context,
                    v.path(),
                    handle ? handle->path : std::nullopt).renderedPath()));
        } else {
            return std::string{v.path().path.abs()};
        }
    }

    if (v.type() == nAttrs) {
        auto maybeString = tryAttrsToString(
            pos,
            v,
            context,
            coerceMore,
            copyToStore,
            origin,
            readFileProvenance);
        if (maybeString)
            return std::move(*maybeString);
        auto i = v.attrs()->get(s.outPath);
        if (!i) {
            error<TypeError>(
                "cannot coerce %1% to a string: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
                .withTrace(pos, errorCtx)
                .debugThrow();
        }
        return coerceToStringWithProvenance(
            pos,
            *i->value,
            context,
            errorCtx,
            coerceMore,
            copyToStore,
            canonicalizePath,
            origin,
            readFileProvenance);
    }

    if (v.type() == nExternal) {
        try {
            return v.external()->coerceToString(*this, pos, context, coerceMore, copyToStore);
        } catch (Error & e) {
            e.addTrace(nullptr, errorCtx);
            throw;
        }
    }

    if (coerceMore) {
        /* Note that `false' is represented as an empty string for
           shell scripting convenience, just like `null'. */
        if (v.type() == nBool && v.boolean())
            return "1";
        if (v.type() == nBool && !v.boolean())
            return "";
        if (v.type() == nInt)
            return std::to_string(v.integer().value);
        if (v.type() == nFloat)
            return std::to_string(v.fpoint());
        if (v.type() == nNull)
            return "";

        if (v.isList()) {
            if (traceActiveDepth) [[unlikely]] maybeRecordListLenDep(v);
            std::string result;
            auto listView = v.listView();
            for (auto [n, v2] : enumerate(listView)) {
                try {
                    result += *coerceToString(
                        pos,
                        *v2,
                        context,
                        "while evaluating one element of the list",
                        coerceMore,
                        copyToStore,
                        canonicalizePath);
                } catch (Error & e) {
                    e.addTrace(positions[pos], errorCtx);
                    throw;
                }
                if (n < v.listSize() - 1
                    /* !!! not quite correct */
                    && (!v2->isList() || v2->listSize() != 0))
                    result += " ";
            }
            return result;
        }
    }

    error<TypeError>("cannot coerce %1% to a string: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
        .withTrace(pos, errorCtx)
        .debugThrow();
}

BackedStringView EvalState::coerceToString(
    const PosIdx pos,
    Value & v,
    NixStringContext & context,
    std::string_view errorCtx,
    bool coerceMore,
    bool copyToStore,
    bool canonicalizePath)
{
    return coerceToStringWithProvenance(pos, v, context, errorCtx,
        coerceMore, copyToStore, canonicalizePath, nullptr, nullptr);
}

ContextObject EvalState::coerceToContextObject(
    const PosIdx pos,
    Value & v,
    NixStringContext & context,
    std::string_view errorCtx,
    bool coerceMore,
    bool copyToStore,
    bool canonicalizePath)
{
    auto _level = addCallDepth(pos);

    forceValue(v, pos);

    if (v.type() == nString) {
        copyContext(v, context);
        return ContextObject{
            ContextObject::PreservedString{
                .value = BackedStringView(v.string_view()),
                .publication = lookupSemanticHandle(v).value_or(SemanticHandle{}),
            }};
    }

    if (v.type() == nPath) {
        if (!canonicalizePath && !copyToStore) {
            return ContextObject{
                ContextObject::PreservedString{
                    .value = BackedStringView(v.pathStrView()),
                    .publication = lookupSemanticHandle(v).value_or(SemanticHandle{}),
                }};
        } else if (copyToStore) {
            auto handle = lookupSemanticHandle(v);
            auto published = copyPathToStoreViaEvalEnvironment(
                *this,
                context,
                v.path(),
                handle ? handle->path : std::nullopt);
            return ContextObject{
                ContextObject::DetachedStorePathString{
                    .value = BackedStringView(std::string(published.renderedPath())),
                }};
        } else {
            return ContextObject{
                ContextObject::PreservedString{
                    .value = BackedStringView(std::string{v.path().path.abs()}),
                    .publication = lookupSemanticHandle(v).value_or(SemanticHandle{}),
                }};
        }
    }

    if (v.type() == nAttrs) {
        auto i = v.attrs()->get(s.toString);
        if (i) {
            Value v1;
            callFunction(*i->value, v, v1, pos);
            return coerceToContextObject(
                pos,
                v1,
                context,
                "while evaluating the result of the `__toString` attribute",
                coerceMore,
                copyToStore,
                true);
        }

        auto outPath = v.attrs()->get(s.outPath);
        if (!outPath) {
            error<TypeError>(
                "cannot coerce %1% to a string: %2%",
                showType(v),
                ValuePrinter(*this, v, errorPrintOptions))
                .withTrace(pos, errorCtx)
                .debugThrow();
        }

        return coerceToContextObject(
            pos,
            *outPath->value,
            context,
            errorCtx,
            coerceMore,
            copyToStore,
            canonicalizePath);
    }

    if (v.type() == nExternal) {
        try {
            return ContextObject{
                ContextObject::PlainString{
                    .value = BackedStringView(
                        v.external()->coerceToString(*this, pos, context, coerceMore, copyToStore)),
                }};
        } catch (Error & e) {
            e.addTrace(nullptr, errorCtx);
            throw;
        }
    }

    if (coerceMore) {
        if (v.type() == nBool && v.boolean())
            return ContextObject{
                ContextObject::PlainString{.value = BackedStringView("1")}};
        if (v.type() == nBool && !v.boolean())
            return ContextObject{
                ContextObject::PlainString{.value = BackedStringView("")}};
        if (v.type() == nInt)
            return ContextObject{
                ContextObject::PlainString{
                    .value = BackedStringView(std::to_string(v.integer().value))}};
        if (v.type() == nFloat)
            return ContextObject{
                ContextObject::PlainString{
                    .value = BackedStringView(std::to_string(v.fpoint()))}};
        if (v.type() == nNull)
            return ContextObject{
                ContextObject::PlainString{.value = BackedStringView("")}};

        if (v.isList()) {
            if (traceActiveDepth) [[unlikely]] maybeRecordListLenDep(v);
            std::string result;
            auto listView = v.listView();
            for (auto [n, v2] : enumerate(listView)) {
                try {
                    auto part = coerceToContextObject(
                        pos,
                        *v2,
                        context,
                        "while evaluating one element of the list",
                        coerceMore,
                        copyToStore,
                        canonicalizePath);
                    result += part.view();
                } catch (Error & e) {
                    e.addTrace(positions[pos], errorCtx);
                    throw;
                }
                if (n < v.listSize() - 1
                    && (!v2->isList() || v2->listSize() != 0))
                    result += " ";
            }
            return ContextObject{
                ContextObject::PlainString{
                    .value = BackedStringView(std::move(result))}};
        }
    }

    error<TypeError>("cannot coerce %1% to a string: %2%", showType(v), ValuePrinter(*this, v, errorPrintOptions))
        .withTrace(pos, errorCtx)
        .debugThrow();
}

ContextObject EvalState::coerceToContextObjectForUnsafeDiscard(
    const PosIdx pos,
    Value & v,
    NixStringContext & context,
    std::string_view errorCtx)
{
    auto * depCtx = eval_trace::currentFiberDepCtx();
    if (!depCtx)
        depCtx = eval_trace::currentStandaloneDepCtx();

    if (!(traceActiveDepth && depCtx && depCtx->isActive()))
        return coerceToContextObject(pos, v, context, errorCtx, false, true, true);

    PublicationWarmupScope warmup(*this, depCtx);
    auto coerced = coerceToContextObject(pos, v, context, errorCtx, false, true, true);
    if (!coerced.isDetached())
        warmup.mergeIntoParent();
    else
        warmup.discard();
    return coerced;
}



SourcePath EvalState::coerceToPathWithProvenance(
    const PosIdx pos,
    Value & v,
    NixStringContext & context,
    std::string_view errorCtx,
    std::optional<PathObject> * origin)
{
    try {
        forceValue(v, pos);
    } catch (Error & e) {
        e.addTrace(positions[pos], errorCtx);
        throw;
    }

    if (origin)
        origin->reset();

    /* Handle path values directly, without coercing to a string. */
    if (v.type() == nPath) {
        if (origin) {
            auto handle = lookupSemanticHandle(v);
            *origin = handle ? handle->path : std::nullopt;
        }
        return v.path();
    }

    /* Similarly, handle __toString where the result may be a path
       value. */
    if (v.type() == nAttrs) {
        auto i = v.attrs()->get(s.toString);
        if (i) {
            Value v1;
            callFunction(*i->value, v, v1, pos);
            return coerceToPathWithProvenance(pos, v1, context, errorCtx, origin);
        }
    }

    /* Any other value should be coercible to a string, interpreted
       relative to the root filesystem. */
    auto path = coerceToStringWithProvenance(pos, v, context, errorCtx, false, false, true, origin, nullptr).toOwned();
    if (path == "" || path[0] != '/')
        error<EvalError>("string '%1%' doesn't represent an absolute path", path).withTrace(pos, errorCtx).debugThrow();
    return rootPath(CanonPath(path));
}

SourcePath EvalState::coerceToPath(
    const PosIdx pos,
    Value & v,
    NixStringContext & context,
    std::string_view errorCtx)
{
    return coerceToPathWithProvenance(pos, v, context, errorCtx, nullptr);
}

EvalState::CoercedPath EvalState::coerceToCoercedPath(
    const PosIdx pos,
    Value & v,
    NixStringContext & context,
    std::string_view errorCtx)
{
    std::optional<PathObject> origin;
    auto path = coerceToPathWithProvenance(pos, v, context, errorCtx, &origin);
    return capturePathWithObject(std::move(path), std::move(origin));
}

StorePath
EvalState::coerceToStorePath(const PosIdx pos, Value & v, NixStringContext & context, std::string_view errorCtx)
{
    auto path = coerceToString(pos, v, context, errorCtx, false, false, true).toOwned();
    if (auto storePath = store->maybeParseStorePath(path))
        return *storePath;
    error<EvalError>("path '%1%' is not in the Nix store", path).withTrace(pos, errorCtx).debugThrow();
}

std::pair<SingleDerivedPath, std::string_view> EvalState::coerceToSingleDerivedPathUnchecked(
    const PosIdx pos, Value & v, std::string_view errorCtx, const ExperimentalFeatureSettings & xpSettings)
{
    NixStringContext context;
    auto s = forceString(v, context, pos, errorCtx, xpSettings);
    auto csize = context.size();
    if (csize != 1)
        error<EvalError>("string '%s' has %d entries in its context. It should only have exactly one entry", s, csize)
            .withTrace(pos, errorCtx)
            .debugThrow();
    auto derivedPath = std::visit(
        overloaded{
            [&](NixStringContextElem::Opaque && o) -> SingleDerivedPath { return std::move(o); },
            [&](NixStringContextElem::DrvDeep &&) -> SingleDerivedPath {
                error<EvalError>(
                    "string '%s' has a context which refers to a complete source and binary closure. This is not supported at this time",
                    s)
                    .withTrace(pos, errorCtx)
                    .debugThrow();
            },
            [&](NixStringContextElem::Built && b) -> SingleDerivedPath { return std::move(b); },
        },
        ((NixStringContextElem &&) *context.begin()).raw);
    return {
        std::move(derivedPath),
        std::move(s),
    };
}

SingleDerivedPath EvalState::coerceToSingleDerivedPath(const PosIdx pos, Value & v, std::string_view errorCtx)
{
    auto [derivedPath, s_] = coerceToSingleDerivedPathUnchecked(pos, v, errorCtx);
    auto s = s_;
    auto sExpected = mkSingleDerivedPathStringRaw(derivedPath);
    if (s != sExpected) {
        /* `std::visit` is used here just to provide a more precise
           error message. */
        std::visit(
            overloaded{
                [&](const SingleDerivedPath::Opaque & o) {
                    error<EvalError>("path string '%s' has context with the different path '%s'", s, sExpected)
                        .withTrace(pos, errorCtx)
                        .debugThrow();
                },
                [&](const SingleDerivedPath::Built & b) {
                    error<EvalError>(
                        "string '%s' has context with the output '%s' from derivation '%s', but the string is not the right placeholder for this derivation output. It should be '%s'",
                        s,
                        b.output,
                        b.drvPath->to_string(*store),
                        sExpected)
                        .withTrace(pos, errorCtx)
                        .debugThrow();
                }},
            derivedPath.raw());
    }
    return derivedPath;
}

// NOTE: This implementation must match eqValues!
// We accept this burden because informative error messages for
// `assert a == b; x` are critical for our users' testing UX.
void EvalState::assertEqValues(Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx)
{
    auto _level = addCallDepth(pos);

    // This implementation must match eqValues.
    forceValue(v1, pos);
    forceValue(v2, pos);

    if (&v1 == &v2)
        return;

    // Special case type-compatibility between float and int
    if ((v1.type() == nInt || v1.type() == nFloat) && (v2.type() == nInt || v2.type() == nFloat)) {
        if (eqValues(v1, v2, pos, errorCtx)) {
            return;
        } else {
            error<AssertionError>(
                "%s with value '%s' is not equal to %s with value '%s'",
                showType(v1),
                ValuePrinter(*this, v1, errorPrintOptions),
                showType(v2),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
    }

    if (v1.type() != v2.type()) {
        error<AssertionError>(
            "%s of value '%s' is not equal to %s of value '%s'",
            showType(v1),
            ValuePrinter(*this, v1, errorPrintOptions),
            showType(v2),
            ValuePrinter(*this, v2, errorPrintOptions))
            .debugThrow();
    }

    switch (v1.type()) {
    case nInt:
        if (v1.integer() != v2.integer()) {
            error<AssertionError>("integer '%d' is not equal to integer '%d'", v1.integer(), v2.integer()).debugThrow();
        }
        return;

    case nBool:
        if (v1.boolean() != v2.boolean()) {
            error<AssertionError>(
                "boolean '%s' is not equal to boolean '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        return;

    case nString:
        if (v1.string_view() != v2.string_view()) {
            error<AssertionError>(
                "string '%s' is not equal to string '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        return;

    case nPath:
        if (v1.pathAccessor() != v2.pathAccessor()) {
            error<AssertionError>(
                "path '%s' is not equal to path '%s' because their accessors are different",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        if (v1.pathStrView() != v2.pathStrView()) {
            error<AssertionError>(
                "path '%s' is not equal to path '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        return;

    case nNull:
        return;

    case nList:
        if (v1.listSize() != v2.listSize()) {
            error<AssertionError>(
                "list of size '%d' is not equal to list of size '%d', left hand side is '%s', right hand side is '%s'",
                v1.listSize(),
                v2.listSize(),
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        for (size_t n = 0; n < v1.listSize(); ++n) {
            try {
                assertEqValues(*v1.listView()[n], *v2.listView()[n], pos, errorCtx);
            } catch (Error & e) {
                e.addTrace(positions[pos], "while comparing list element %d", n);
                throw;
            }
        }
        return;

    case nAttrs: {
        // See comment in eqValues nAttrs case.
        if (v1.attrs() == v2.attrs())
            return;
        if (isDerivation(v1) && isDerivation(v2)) {
            auto i = v1.attrs()->get(s.outPath);
            auto j = v2.attrs()->get(s.outPath);
            if (i && j) {
                try {
                    assertEqValues(*i->value, *j->value, pos, errorCtx);
                    return;
                } catch (Error & e) {
                    e.addTrace(positions[pos], "while comparing a derivation by its '%s' attribute", "outPath");
                    throw;
                }
                assert(false);
            }
        }

        if (v1.attrs()->size() != v2.attrs()->size()) {
            error<AssertionError>(
                "attribute names of attribute set '%s' differs from attribute set '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }

        // Like normal comparison, we compare the attributes in non-deterministic Symbol index order.
        // This function is called when eqValues has found a difference, so to reliably
        // report about its result, we should follow in its literal footsteps and not
        // try anything fancy that could lead to an error.
        Bindings::const_iterator i, j;
        for (i = v1.attrs()->begin(), j = v2.attrs()->begin(); i != v1.attrs()->end(); ++i, ++j) {
            if (i->name != j->name) {
                // A difference in a sorted list means that one attribute is not contained in the other, but we don't
                // know which. Let's find out. Could use <, but this is more clear.
                if (!v2.attrs()->get(i->name)) {
                    error<AssertionError>(
                        "attribute name '%s' is contained in '%s', but not in '%s'",
                        symbols[i->name],
                        ValuePrinter(*this, v1, errorPrintOptions),
                        ValuePrinter(*this, v2, errorPrintOptions))
                        .debugThrow();
                }
                if (!v1.attrs()->get(j->name)) {
                    error<AssertionError>(
                        "attribute name '%s' is missing in '%s', but is contained in '%s'",
                        symbols[j->name],
                        ValuePrinter(*this, v1, errorPrintOptions),
                        ValuePrinter(*this, v2, errorPrintOptions))
                        .debugThrow();
                }
                assert(false);
            }
            try {
                assertEqValues(*i->value, *j->value, pos, errorCtx);
            } catch (Error & e) {
                // The order of traces is reversed, so this presents as
                //  where left hand side is
                //    at <pos>
                //  where right hand side is
                //    at <pos>
                //  while comparing attribute '<name>'
                if (j->pos != noPos)
                    e.addTrace(positions[j->pos], "where right hand side is");
                if (i->pos != noPos)
                    e.addTrace(positions[i->pos], "where left hand side is");
                e.addTrace(positions[pos], "while comparing attribute '%s'", symbols[i->name]);
                throw;
            }
        }
        return;
    }

    case nFunction:
        error<AssertionError>("distinct functions and immediate comparisons of identical functions compare as unequal")
            .debugThrow();

    case nExternal:
        if (!(*v1.external() == *v2.external())) {
            error<AssertionError>(
                "external value '%s' is not equal to external value '%s'",
                ValuePrinter(*this, v1, errorPrintOptions),
                ValuePrinter(*this, v2, errorPrintOptions))
                .debugThrow();
        }
        return;

    case nFloat:
        // !!!
        if (!(v1.fpoint() == v2.fpoint())) {
            error<AssertionError>("float '%f' is not equal to float '%f'", v1.fpoint(), v2.fpoint()).debugThrow();
        }
        return;

    // Cannot be returned by forceValue().
    case nThunk:
    case nFailed:
        unreachable();

    default: // Note that we pass compiler flags that should make `default:` unreachable.
        // Also note that this probably ran after `eqValues`, which implements
        // the same logic more efficiently (without having to unwind stacks),
        // so maybe `assertEqValues` and `eqValues` are out of sync. Check it for solutions.
        error<EvalError>("assertEqValues: cannot compare %1% with %2%", showType(v1), showType(v2))
            .withTrace(pos, errorCtx)
            .panic();
    }
}

// This implementation must match assertEqValues
bool EvalState::eqValues(Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx)
{
    auto _level = addCallDepth(pos);

    forceValue(v1, pos);
    forceValue(v2, pos);

    /* !!! Hack to support some old broken code that relies on pointer
       equality tests between sets.  (Specifically, builderDefs calls
       uniqList on a list of sets.)  Will remove this eventually. */
    if (&v1 == &v2)
        return true;

    // Special case type-compatibility between float and int
    if (v1.type() == nInt && v2.type() == nFloat)
        return v1.integer().value == v2.fpoint();
    if (v1.type() == nFloat && v2.type() == nInt)
        return v1.fpoint() == v2.integer().value;

    // All other types are not compatible with each other.
    if (v1.type() != v2.type())
        return false;

    switch (v1.type()) {
    case nInt:
        return v1.integer() == v2.integer();

    case nBool:
        return v1.boolean() == v2.boolean();

    case nString:
        if (traceActiveDepth) [[unlikely]] {
            if (auto access = eval_trace::TraceAccess::current()) {
                maybeRecordRawContentDep(*access, v1);
                maybeRecordRawContentDep(*access, v2);
            }
        }
        if (sameValueIdentity(v1, v2))
            return true;
        return v1.string_view() == v2.string_view();

    case nPath:
        if (sameValueIdentity(v1, v2))
            return true;
        return
            // FIXME: compare accessors by their fingerprint.
            v1.pathAccessor() == v2.pathAccessor() && v1.pathStrView() == v2.pathStrView();

    case nNull:
        return true;

    case nList:
        if (traceActiveDepth) [[unlikely]] {
            maybeRecordListLenDep(v1);
            maybeRecordListLenDep(v2);
        }
        if (v1.listSize() != v2.listSize())
            return false;
        // Eval-trace materialization allocates fresh list backing arrays,
        // breaking Value pointer identity. Check whether both values were
        // produced by TracedExpr thunks that navigate to the same real value.
        if (sameValueIdentity(v1, v2))
            return true;
        for (size_t n = 0; n < v1.listSize(); ++n)
            if (!eqValues(*v1.listView()[n], *v2.listView()[n], pos, errorCtx))
                return false;
        return true;

    case nAttrs: {
        if (traceActiveDepth) [[unlikely]] {
            maybeRecordAttrKeysDep(positions, symbols, v1);
            maybeRecordAttrKeysDep(positions, symbols, v2);
        }
        if (v1.attrs() == v2.attrs())
            return true;
        // Eval-trace materialization allocates fresh Bindings, breaking
        // the pointer check above. Check whether both values were produced
        // by TracedExpr thunks that navigate to the same real value.
        if (sameValueIdentity(v1, v2))
            return true;
        /* If both sets denote a derivation (type = "derivation"),
           then compare their outPaths. */
        if (isDerivation(v1) && isDerivation(v2)) {
            auto i = v1.attrs()->get(s.outPath);
            auto j = v2.attrs()->get(s.outPath);
            if (i && j)
                return eqValues(*i->value, *j->value, pos, errorCtx);
        }

        if (v1.attrs()->size() != v2.attrs()->size())
            return false;

        /* Otherwise, compare the attributes one by one. */
        Bindings::const_iterator i, j;
        for (i = v1.attrs()->begin(), j = v2.attrs()->begin(); i != v1.attrs()->end(); ++i, ++j)
            if (i->name != j->name)
                return false;
            else if (!eqValues(*i->value, *j->value, pos, errorCtx))
                return false;

        return true;
    }

    /* Functions are incomparable. */
    case nFunction:
        if (sameValueIdentity(v1, v2))
            return true;
        return false;

    case nExternal:
        return *v1.external() == *v2.external();

    case nFloat:
        // !!!
        return v1.fpoint() == v2.fpoint();

    // Cannot be returned by forceValue().
    case nThunk:
    case nFailed:
        unreachable();

    default: // Note that we pass compiler flags that should make `default:` unreachable.
        error<EvalError>("eqValues: cannot compare %1% with %2%", showType(v1), showType(v2))
            .withTrace(pos, errorCtx)
            .panic();
    }
}

bool EvalState::fullGC()
{
#if NIX_USE_BOEHMGC
    GC_gcollect();
    // Check that it ran. We might replace this with a version that uses more
    // of the boehm API to get this reliably, at a maintenance cost.
    // We use a 1K margin because technically this has a race condition, but we
    // probably won't encounter it in practice, because the CLI isn't concurrent
    // like that.
    return GC_get_bytes_since_gc() < 1024;
#else
    return false;
#endif
}

bool Counter::enabled = getEnv("NIX_SHOW_STATS").value_or("0") != "0";

void EvalState::maybePrintStats()
{
    if (Counter::enabled) {
        // Make the final heap size more deterministic.
#if NIX_USE_BOEHMGC
        if (!fullGC()) {
            warn("failed to perform a full GC before reporting stats");
        }
#endif
        printStatistics();
    }
}

void EvalState::printStatistics()
{
    std::chrono::microseconds cpuTimeDuration = getCpuUserTime();
    float cpuTime = std::chrono::duration_cast<std::chrono::duration<float>>(cpuTimeDuration).count();

    auto & memstats = mem.getStats();

    uint64_t bEnvs = memstats.nrEnvs * sizeof(Env) + memstats.nrValuesInEnvs * sizeof(Value *);
    uint64_t bLists = memstats.nrListElems * sizeof(Value *);
    uint64_t bValues = memstats.nrValues * sizeof(Value);
    uint64_t bAttrsets = memstats.nrAttrsets * sizeof(Bindings) + memstats.nrAttrsInAttrsets * sizeof(Attr);

#if NIX_USE_BOEHMGC
    GC_word heapSize, totalBytes;
    GC_get_heap_usage_safe(&heapSize, 0, 0, 0, &totalBytes);
    double gcFullOnlyTime = ({
        auto ms = GC_get_full_gc_total_time();
        ms * 0.001;
    });
    auto gcCycles = getGCCycles();
#endif

    auto outPath = getEnv("NIX_SHOW_STATS_PATH").value_or("-");
    std::fstream fs;
    if (outPath != "-")
        fs.open(outPath, std::fstream::out);
    json topObj = json::object();
    topObj["cpuTime"] = cpuTime;
    topObj["time"] = {
        {"cpu", cpuTime},
#if NIX_USE_BOEHMGC
        {GC_is_incremental_mode() ? "gcNonIncremental" : "gc", gcFullOnlyTime},
        {GC_is_incremental_mode() ? "gcNonIncrementalFraction" : "gcFraction", gcFullOnlyTime / cpuTime},
#endif
    };
    topObj["envs"] = {
        {"number", memstats.nrEnvs.load()},
        {"elements", memstats.nrValuesInEnvs.load()},
        {"bytes", bEnvs},
    };
    topObj["nrExprs"] = Expr::nrExprs.load();
    topObj["list"] = {
        {"elements", memstats.nrListElems.load()},
        {"bytes", bLists},
        {"concats", nrListConcats.load()},
    };
    topObj["values"] = {
        {"number", memstats.nrValues.load()},
        {"bytes", bValues},
    };
    topObj["symbols"] = {
        {"number", symbols.size()},
        {"bytes", symbols.totalSize()},
    };
    topObj["sets"] = {
        {"number", memstats.nrAttrsets.load()},
        {"bytes", bAttrsets},
        {"elements", memstats.nrAttrsInAttrsets.load()},
    };
    topObj["sizes"] = {
        {"Env", sizeof(Env)},
        {"Value", sizeof(Value)},
        {"Bindings", sizeof(Bindings)},
        {"Attr", sizeof(Attr)},
    };
    topObj["nrOpUpdates"] = nrOpUpdates.load();
    topObj["nrOpUpdateValuesCopied"] = nrOpUpdateValuesCopied.load();
    topObj["nrThunks"] = nrThunks.load();
    topObj["nrAvoided"] = nrAvoided.load();
    topObj["nrLookups"] = nrLookups.load();
    topObj["nrPrimOpCalls"] = nrPrimOpCalls.load();
    topObj["nrFunctionCalls"] = nrFunctionCalls.load();
    topObj["evalTrace"] = {
        {"db", {
            {"closeTimeUs", eval_trace::nrDbCloseTimeUs.load()},
            {"initTimeUs", eval_trace::nrDbInitTimeUs.load()},
        }},
        {"hits", eval_trace::nrTraceCacheHits.load()},
        {"loadTrace", {
            {"count", eval_trace::nrLoadTraces.load()},
            {"timeUs", eval_trace::nrLoadTraceTimeUs.load()},
        }},
        {"loadKeySet", {
            {"count", eval_trace::nrLoadKeySets.load()},
            {"cacheHits", eval_trace::nrLoadKeySetCacheHits.load()},
            {"cacheMisses", eval_trace::nrLoadKeySetCacheMisses.load()},
            {"timeUs", eval_trace::nrLoadKeySetTimeUs.load()},
        }},
        {"misses", eval_trace::nrTraceCacheMisses.load()},
        {"record", {
            {"count", eval_trace::nrRecords.load()},
            {"flushUs", eval_trace::nrRecordFlushUs.load()},
            {"hashUs", eval_trace::nrRecordHashUs.load()},
            {"serializeKeysUs", eval_trace::nrRecordSerializeKeysUs.load()},
            {"serializeValuesUs", eval_trace::nrRecordSerializeValuesUs.load()},
            {"timeUs", eval_trace::nrRecordTimeUs.load()},
        }},
        {"recovery", {
            {"attempts", eval_trace::nrRecoveryAttempts.load()},
            {"acceptance", {
                {"implicitGuardCandidates", eval_trace::nrRecoveryImplicitGuardCandidates.load()},
                {"implicitGuardChecks", eval_trace::nrRecoveryImplicitGuardChecks.load()},
                {"implicitGuardFailures", eval_trace::nrRecoveryImplicitGuardFailures.load()},
                {"implicitGuardFullTraceLoads", eval_trace::nrRecoveryImplicitGuardFullTraceLoads.load()},
                {"implicitGuardTimeUs", eval_trace::nrRecoveryImplicitGuardTimeUs.load()},
            }},
            {"directHash", {
                {"hits", eval_trace::nrRecoveryDirectHashHits.load()},
                {"timeUs", eval_trace::nrRecoveryDirectHashTimeUs.load()},
            }},
            {"failures", eval_trace::nrRecoveryFailures.load()},
            {"gitIdentity", {
                {"accepted", eval_trace::nrRecoveryGitIdentityAccepted.load()},
                {"attempts", eval_trace::nrRecoveryGitIdentityAttempts.load()},
                {"candidates", eval_trace::nrRecoveryGitIdentityCandidates.load()},
                {"rejected", eval_trace::nrRecoveryGitIdentityRejected.load()},
                {"timeUs", eval_trace::nrRecoveryGitIdentityTimeUs.load()},
            }},
            {"gitIdentityHits", eval_trace::nrRecoveryGitIdentityHits.load()},
            // History-based bootstrap: orchestrator hit scanHistory
            // after primary lookupCurrentNode missed. Distinct from
            // attempts/hits, which track the 3-strategy fallback on
            // verifyTrace failure. See OR-5 closure note.
            {"historyBootstraps", eval_trace::nrHistoryBootstraps.load()},
            {"structVariant", {
                {"hits", eval_trace::nrRecoveryStructVariantHits.load()},
                {"timeUs", eval_trace::nrRecoveryStructVariantTimeUs.load()},
            }},
            {"lookups", {
                {"directHash", {
                    {"count", eval_trace::nrRecoveryDirectHashLookupCount.load()},
                    {"rows", eval_trace::nrRecoveryDirectHashLookupRows.load()},
                    {"timeUs", eval_trace::nrRecoveryDirectHashLookupUs.load()},
                }},
                {"gitIdentity", {
                    {"count", eval_trace::nrRecoveryGitIdentityLookupCount.load()},
                    {"rows", eval_trace::nrRecoveryGitIdentityLookupRows.load()},
                    {"timeUs", eval_trace::nrRecoveryGitIdentityLookupUs.load()},
                }},
                {"latestHistory", {
                    {"count", eval_trace::nrRecoveryLatestHistoryLookupCount.load()},
                    {"timeUs", eval_trace::nrRecoveryLatestHistoryLookupUs.load()},
                }},
                {"scanHistory", {
                    {"count", eval_trace::nrRecoveryScanHistoryCount.load()},
                    {"rows", eval_trace::nrRecoveryScanHistoryRows.load()},
                    {"timeUs", eval_trace::nrRecoveryScanHistoryUs.load()},
                }},
            }},
            {"timeUs", eval_trace::nrRecoveryTimeUs.load()},
        }},
        {"verify", {
            {"count", eval_trace::nrTraceVerifications.load()},
            {"depsChecked", eval_trace::nrDepsChecked.load()},
            {"failed", eval_trace::nrVerificationsFailed.load()},
            {"passed", eval_trace::nrVerificationsPassed.load()},
            {"timeUs", eval_trace::nrVerifyTimeUs.load()},
        }},
        {"verifyTrace", {
            {"timeUs", eval_trace::nrVerifyTraceTimeUs.load()},
        }},
        {"thunks", {
            {"created", eval_trace::nrTracedExprCreated.load()},
            {"fromMaterialize", eval_trace::nrTracedExprFromMaterialize.load()},
            {"fromDataFile", eval_trace::nrTracedExprFromDataFile.load()},
            {"forced", eval_trace::nrTracedExprForced.load()},
            {"lazyStateAllocated", eval_trace::nrLazyStateAllocated.load()},
        }},
        {"dataFile", {
            {"scalarChildren", eval_trace::nrDataFileScalarChildren.load()},
            {"containerChildren", eval_trace::nrDataFileContainerChildren.load()},
        }},
        {"depTracker", {
            {"scopes", eval_trace::nrDepContextScopes.load()},
            {"ownDepsTotal", eval_trace::nrOwnDepsTotal.load()},
            {"ownDepsMax", eval_trace::nrOwnDepsMax.load()},
        }},
        {"replay", {
            {"totalCalls", eval_trace::nrReplayTotalCalls.load()},
            {"bloomHits", eval_trace::nrReplayBloomHits.load()},
            {"epochHits", eval_trace::nrReplayEpochHits.load()},
            {"added", eval_trace::nrReplayAdded.load()},
        }},
        {"depHash", {
            {"cacheHits", eval_trace::nrDepHashCacheHits.load()},
            {"cacheMisses", eval_trace::nrDepHashCacheMisses.load()},
            {"structuredMisses", eval_trace::nrDepHashStructuredMisses.load()},
            {"contentSubsumptionSkips", eval_trace::nrContentSubsumptionSkips.load()},
            {"contentUs", eval_trace::nrDepHashContentUs.load()},
            {"directoryUs", eval_trace::nrDepHashDirectoryUs.load()},
            {"existenceUs", eval_trace::nrDepHashExistenceUs.load()},
            {"storePathUs", eval_trace::nrDepHashStorePathUs.load()},
            {"structuredJsonUs", eval_trace::nrDepHashStructuredJsonUs.load()},
            {"structuredTomlUs", eval_trace::nrDepHashStructuredTomlUs.load()},
            {"structuredDirUs", eval_trace::nrDepHashStructuredDirUs.load()},
            {"structuredNixUs", eval_trace::nrDepHashStructuredNixUs.load()},
            {"structuredOuterUs", eval_trace::nrDepHashStructuredOuterUs.load()},
            {"scDirSetMisses", eval_trace::nrDepHashScDirSetMisses.load()},
            {"scJsonParseUs", eval_trace::nrDepHashScJsonParseUs.load()},
            {"gitIdentityUs", eval_trace::nrDepHashGitIdentityUs.load()},
            {"gitIdentityMisses", eval_trace::nrDepHashGitIdentityMisses.load()},
            {"otherUs", eval_trace::nrDepHashOtherUs.load()},
            {"recoveryRecomputeUs", eval_trace::nrRecoveryDepRecomputeUs.load()},
            {"recoveryRecomputeCount", eval_trace::nrRecoveryDepRecomputeCount.load()},
            {"structVariantCandidates", eval_trace::nrStructVariantCandidates.load()},
            {"structVariantDepsResolved", eval_trace::nrStructVariantDepsResolved.load()},
            {"structVariantLoadKeySetUs", eval_trace::nrStructVariantLoadKeySetUs.load()},
            {"structVariantHashUs", eval_trace::nrStructVariantHashUs.load()},
            {"structVariantDepResolveUs", eval_trace::nrStructVariantDepResolveUs.load()},
            {"backendSetupFailed", eval_trace::nrTraceBackendSetupFailed.load()},
            {"resolveViaRegistry", eval_trace::nrResolveViaRegistry.load()},
            {"resolveViaPathObject", eval_trace::nrResolveViaPathObject.load()},
            {"resolveViaAbsolute", eval_trace::nrResolveViaAbsolute.load()},
            {"depRecordNoActiveContext", eval_trace::nrDepRecordNoActiveContext.load()},
        }},
    };

    // Per-DepKeySetId SV candidate telemetry.  Emitted as an array
    // (not an object) so the sort-by-`tried`-desc order survives JSON
    // serialization.  `nlohmann::json::object_t` is `std::map` which
    // sorts keys alphabetically at emit time, which would destroy the
    // intended ordering ("10" would sort before "2"); an array pins
    // the order the analysis scripts consume.
    //
    // Emitted only when the map is non-empty — most runs never enter
    // structural-variant recovery, so this block is absent for them
    // and the stats schema stays tidy.
    {
        auto svMap = eval_trace::snapshotSVCandidateStats();
        if (!svMap.empty()) {
            topObj["evalTrace"]["structVariant"] = {
                {"byDepKeySet", eval_trace::renderSVCandidateStatsJson(svMap)},
            };
        }
    }

#if NIX_USE_BOEHMGC
    topObj["gc"] = {
        {"heapSize", heapSize},
        {"totalBytes", totalBytes},
        {"cycles", gcCycles},
    };
#endif

    if (countCalls) {
        topObj["primops"] = primOpCalls;
        {
            auto & list = topObj["functions"];
            list = json::array();
            for (auto & [fun, count] : functionCalls) {
                json obj = json::object();
                if (fun->name)
                    obj["name"] = (std::string_view) symbols[fun->name];
                else
                    obj["name"] = nullptr;
                if (auto pos = positions[fun->pos]) {
                    if (auto path = std::get_if<SourcePath>(&pos.origin))
                        obj["file"] = path->to_string();
                    obj["line"] = pos.line;
                    obj["column"] = pos.column;
                }
                obj["count"] = count;
                list.push_back(obj);
            }
        }
        {
            auto list = topObj["attributes"];
            list = json::array();
            for (auto & i : attrSelects) {
                json obj = json::object();
                if (auto pos = positions[i.first]) {
                    if (auto path = std::get_if<SourcePath>(&pos.origin))
                        obj["file"] = path->to_string();
                    obj["line"] = pos.line;
                    obj["column"] = pos.column;
                }
                obj["count"] = i.second;
                list.push_back(obj);
            }
        }
    }

    if (getEnv("NIX_SHOW_SYMBOLS").value_or("0") != "0") {
        // XXX: overrides earlier assignment
        topObj["symbols"] = json::array();
        auto & list = topObj["symbols"];
        symbols.dump([&](std::string_view s) { list.emplace_back(s); });
    }
    if (outPath == "-") {
        std::cerr << topObj.dump(2) << std::endl;
    } else {
        fs << topObj.dump(2) << std::endl;
    }
}

SourcePath resolveExprPath(SourcePath path, bool addDefaultNix)
{
    unsigned int followCount = 0, maxFollow = 1024;

    /* If `path' is a symlink, follow it.  This is so that relative
       path references work. */
    while (!path.path.isRoot()) {
        // Basic cycle/depth limit to avoid infinite loops.
        if (++followCount >= maxFollow)
            throw Error("too many symbolic links encountered while traversing the path '%s'", path);
        auto p = path.parent().resolveSymlinks() / path.baseName();
        if (p.path != path.path) {
            path = p;
            continue;
        }
        if (p.lstat().type != SourceAccessor::tSymlink)
            break;
        path = {path.accessor, CanonPath(p.readLink(), path.path.parent().value_or(CanonPath::root))};
    }

    /* If `path' refers to a directory, append `/default.nix' and continue
       resolving symlinks from that file path. Otherwise `<nixpkgs>` and
       `<foo/dependencies.nix>` can observe different identities when
       `default.nix` is itself a symlink. */
    if (addDefaultNix && path.resolveSymlinks().lstat().type == SourceAccessor::tDirectory)
        return resolveExprPath(path / "default.nix", false);

    return path;
}

Expr * EvalState::parseExprFromFile(const SourcePath & path)
{
    return parseExprFromFile(path, resolveExprPath(path).parent(), staticBaseEnv);
}

Expr * EvalState::parseExprFromFile(const SourcePath & path, const std::shared_ptr<StaticEnv> & staticEnv)
{
    return parseExprFromFile(path, resolveExprPath(path).parent(), staticEnv);
}

Expr * EvalState::parseExprFromFile(
    const SourcePath & path,
    const SourcePath & basePath,
    const std::shared_ptr<StaticEnv> & staticEnv)
{
    auto resolvedPath = resolveExprPath(path);
    auto buffer = readFileViaEvalEnvironment(*this, resolvedPath);

    // readFile hopefully have left some extra space for terminators
    buffer.append("\0\0", 2);
    return parse(buffer.data(), buffer.size(), Pos::Origin(path), basePath, staticEnv);
}

Expr * EvalState::parseExprFromFile(
    const SourcePath & displayPath,
    const SourcePath & physicalPath,
    const SourcePath & basePath,
    const std::shared_ptr<StaticEnv> & staticEnv)
{
    auto resolvedPath = resolveExprPath(physicalPath);
    auto buffer = readFileViaEvalEnvironment(*this, resolvedPath);

    // readFile hopefully have left some extra space for terminators
    buffer.append("\0\0", 2);
    return parse(buffer.data(), buffer.size(), Pos::Origin(displayPath), basePath, staticEnv);
}

Expr * EvalState::parseExprFromFile(const SourcePath & path, const SourcePath & basePath)
{
    return parseExprFromFile(path, basePath, staticBaseEnv);
}

Expr * EvalState::parseExprFromFile(
    const SourcePath & displayPath,
    const SourcePath & physicalPath,
    const SourcePath & basePath)
{
    return parseExprFromFile(displayPath, physicalPath, basePath, staticBaseEnv);
}

Expr * EvalState::parseExprFromString(
    std::string s_, const SourcePath & basePath, const std::shared_ptr<StaticEnv> & staticEnv)
{
    // NOTE this method (and parseStdin) must take care to *fully copy* their input
    // into their respective Pos::Origin until the parser stops overwriting its input
    // data.
    auto s = make_ref<std::string>(s_);
    s_.append("\0\0", 2);
    return parse(s_.data(), s_.size(), Pos::String{.source = s}, basePath, staticEnv);
}

Expr * EvalState::parseExprFromString(std::string s, const SourcePath & basePath)
{
    return parseExprFromString(std::move(s), basePath, staticBaseEnv);
}

ExprAttrs *
EvalState::parseReplBindings(std::string s_, const SourcePath & basePath, const std::shared_ptr<StaticEnv> & staticEnv)
{
    return parseReplBindings(s_, s_, basePath, staticEnv);
}

ExprAttrs * EvalState::parseReplBindings(
    std::string s_, std::string errorSource, const SourcePath & basePath, const std::shared_ptr<StaticEnv> & staticEnv)
{
    auto s = make_ref<std::string>(std::move(errorSource));
    // flex requires two NUL terminators for yy_scan_buffer
    s_.append("\0\0", 2);
    return parseReplBindings(s_.data(), s_.size(), Pos::String{.source = s}, basePath, staticEnv);
}

Expr * EvalState::parseStdin()
{
    // NOTE this method (and parseExprFromString) must take care to *fully copy* their
    // input into their respective Pos::Origin until the parser stops overwriting its
    // input data.
    // Activity act(*logger, lvlTalkative, "parsing standard input");
    auto buffer = drainFD(0);
    // drainFD should have left some extra space for terminators
    buffer.append("\0\0", 2);
    auto s = make_ref<std::string>(buffer);
    return parse(buffer.data(), buffer.size(), Pos::Stdin{.source = s}, rootPath("."), staticBaseEnv);
}

Expr * EvalState::parse(
    char * text,
    size_t length,
    Pos::Origin origin,
    const SourcePath & basePath,
    const std::shared_ptr<StaticEnv> & staticEnv)
{
    auto tmpDocComments = make_ref<DocCommentMap>();

    auto result = parseExprFromBuf(
        text, length, origin, basePath, mem.exprs, symbols, settings, positions, *tmpDocComments, rootFS);

    result->bindVars(*this, staticEnv);

    if (auto sourcePath = std::get_if<SourcePath>(&origin))
        /* A single file might appear multiple times in PosTable if it's
           parsed by scopedImport. If we are the first then emplace into the map, otherwise
           copy our positions into the existing map. */
        positionToDocComment->emplace_or_visit(*sourcePath, tmpDocComments, [&tmpDocComments](auto & kv) {
            kv.second->insert(tmpDocComments->begin(), tmpDocComments->end());
        });

    return result;
}

ExprAttrs * EvalState::parseReplBindings(
    char * text,
    size_t length,
    Pos::Origin origin,
    const SourcePath & basePath,
    const std::shared_ptr<StaticEnv> & staticEnv)
{
    auto tmpDocComments = make_ref<DocCommentMap>();

    auto bindings = parseReplBindingsFromBuf(
        text, length, origin, basePath, mem.exprs, symbols, settings, positions, *tmpDocComments, rootFS);
    assert(bindings);

    bindings->bindVars(*this, staticEnv);

    if (auto sourcePath = std::get_if<SourcePath>(&origin))
        /* A single file might appear multiple times in PosTable if it's
           parsed by scopedImport. If we are the first then emplace into the map, otherwise
           copy our positions into the existing map. */
        positionToDocComment->emplace_or_visit(*sourcePath, tmpDocComments, [&tmpDocComments](auto & kv) {
            kv.second->insert(tmpDocComments->begin(), tmpDocComments->end());
        });

    return bindings;
}

DocComment EvalState::getDocCommentForPos(PosIdx pos)
{
    auto pos2 = positions[pos];
    auto path = pos2.getSourcePath();
    if (!path)
        return {};

    DocComment result;
    positionToDocComment->visit(*path, [&](const auto & kv) {
        if (auto it = kv.second->find(pos); it != kv.second->end())
            result = it->second;
    });
    return result;
}

std::string ExternalValueBase::coerceToString(
    EvalState & state, const PosIdx & pos, NixStringContext & context, bool copyMore, bool copyToStore) const
{
    state.error<TypeError>("cannot coerce %1% to a string: %2%", showType(), *this).atPos(pos).debugThrow();
}

bool ExternalValueBase::operator==(const ExternalValueBase & b) const noexcept
{
    return false;
}

std::ostream & operator<<(std::ostream & str, const ExternalValueBase & v)
{
    return v.print(str);
}

void forceNoNullByte(std::string_view s, std::function<Pos()> pos)
{
    if (s.find('\0') != s.npos) {
        using namespace std::string_view_literals;
        auto str = replaceStrings(std::string(s), "\0"sv, "␀"sv);
        Error error("input string '%s' cannot be represented as Nix string because it contains null bytes", str);
        if (pos) {
            error.atPos(pos());
        }
        throw std::move(error);
    }
}

} // namespace nix
