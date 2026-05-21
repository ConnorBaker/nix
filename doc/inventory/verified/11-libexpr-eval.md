# Inventory — Shard 11: libexpr eval

Topic: libexpr evaluation runtime — `EvalState`, `Value` representation, attribute sets/bindings, attr-path traversal, evaluation cache, evaluation errors, evaluator GC, profiler, settings, function-trace, diagnose, symbol-table, GC small vectors, value contexts.

## File: src/libexpr/include/nix/expr/counter.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `struct Counter` — cache-line-aligned (`alignas(std::hardware_destructive_interference_size)`) atomic counter; arithmetic ops short-circuit to 0 unless `enabled` is set, to avoid contention under multi-threaded evaluation
  - `using value_type = uint64_t`
  - `std::atomic<value_type> inner{0}` — backing storage
  - `static bool enabled` — global enable flag (defined in eval.cc; reads `NIX_SHOW_STATS`)
  - `Counter()` — default ctor
  - `operator value_type() const noexcept` — implicit load
  - `void operator=(value_type) noexcept` — store
  - `value_type load() const noexcept` — explicit load
  - `value_type operator++() noexcept`, `value_type operator++(int) noexcept`
  - `value_type operator--() noexcept`, `value_type operator--(int) noexcept`
  - `value_type operator+=(value_type) noexcept`, `value_type operator-=(value_type) noexcept`

## File: src/libexpr/include/nix/expr/static-string-data.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `template<size_t N> struct StringData::Static` — compile-time literal layout-compatible with `StringData`; layout: size first then `data[N]` so cast to `StringData` works
  - `const size_t size = N - 1` — first member to match StringData layout
  - `char data[N]` — character storage
  - `consteval Static(const char (&str)[N])` — verifies trailing NUL (`throw` aborts compile if missing) and copies via `std::copy_n`
  - `operator const StringData &() const &` — `reinterpret_cast` to runtime StringData (gray-area cast for FAM compatibility, with size/alignment static_asserts)

### Functions
- `template<StringData::Static S> const StringData & operator""_sds()` — user-defined literal returning a static `StringData &`

## File: src/libexpr/include/nix/expr/gc-small-vector.hh

### Namespaces
- `nix`

### Type aliases
- `template<typename T, size_t nItems> using SmallVector = boost::container::small_vector<T, nItems, traceable_allocator<T>>` — boost small_vector with GC-traceable allocator
- `template<size_t nItems> using SmallValueVector = SmallVector<Value *, nItems>` — small vector of value pointers
- `template<size_t nItems> using SmallTemporaryValueVector = SmallVector<Value, nItems>` — small vector of inline values; comment notes that values must not be referenced after the vector is destroyed

### Macros / globals
- `constexpr size_t nonRecursiveStackReservation = 128` — default reservation size for non-recursive callers
- `constexpr size_t conservativeStackReservation = 16` — smaller reservation for self-similar/recursive callers

## File: src/libexpr/include/nix/expr/diagnose.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `enum struct Diagnose { Ignore, Warn, Fatal }` — diagnostic level for deprecated/non-portable language features

### Functions
- `template<typename F> void diagnose(const Setting<Diagnose> & setting, const F & mkError)` — switch on the setting; `mkError(bool fatal)` returns `std::optional<error>`; `Ignore` returns immediately, `Warn` calls `logWarning(error.info())`, `Fatal` rethrows; appends ANSI-bold setting name to the error message via `recalcWhat`

### Macros / globals
- `NIX_DECLARE_CONFIG_SERIALISER(Diagnose)` — declare config serialiser specialisation for `Diagnose`

## File: src/libexpr/diagnose.cc

### Namespaces
- `nix`

### Functions
- `template<> Diagnose BaseSetting<Diagnose>::parse(const std::string &) const` — parses "ignore"/"warn"/"fatal"; throws `UsageError` otherwise
- `template<> std::string BaseSetting<Diagnose>::to_string() const` — round-trip Diagnose to string (uses `unreachable()` on default)

### Classes / structs / enums
- `template<> struct BaseSetting<Diagnose>::trait` — sets `static constexpr bool appendable = false`

### Macros / globals
- `NLOHMANN_JSON_SERIALIZE_ENUM(Diagnose, ...)` — JSON enum serialisation
- `template class BaseSetting<Diagnose>` — explicit instantiation

## File: src/libexpr/include/nix/expr/function-trace.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `class FunctionCallTrace : public EvalProfiler` — profiler that prints entry/exit messages for each function call
  - `Hooks getNeededHooksImpl() const override` — returns `Hooks().set(preFunctionCall).set(postFunctionCall)` (private)
  - `FunctionCallTrace() = default`
  - `[[gnu::noinline]] void preFunctionCallHook(EvalState &, const Value &, std::span<Value *>, const PosIdx) override`
  - `[[gnu::noinline]] void postFunctionCallHook(EvalState &, const Value &, std::span<Value *>, const PosIdx) override`

## File: src/libexpr/function-trace.cc

### Namespaces
- `nix`

### Functions
- `void FunctionCallTrace::preFunctionCallHook(EvalState &, const Value &, std::span<Value *>, const PosIdx)` — logs `lvlInfo` "function-trace entered ... at <ns>" using high-resolution clock
- `void FunctionCallTrace::postFunctionCallHook(EvalState &, const Value &, std::span<Value *>, const PosIdx)` — logs `lvlInfo` "function-trace exited ..."

## File: src/libexpr/include/nix/expr/value/context.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `class BadNixStringContextElem final : public CloneableError<BadNixStringContextElem, Error>` — exception type for malformed string context elements
  - `std::string_view raw` — public; the offending input
  - `template<typename... Args> BadNixStringContextElem(std::string_view raw_, const Args &... args)` — formats "Bad String Context element: `<msg>`: `<raw>`"
- `struct NixStringContextElem` — variant identifying string-context elements; comment says it should be renamed `StringContextBuilderElem`
  - `using Opaque = SingleDerivedPath::Opaque` — plain store-object reference (`<path>`)
  - `struct DrvDeep { StorePath drvPath; GENERATE_CMP(DrvDeep, me->drvPath); }` — deep derivation closure (`=<drvPath>`)
  - `using Built = SingleDerivedPath::Built` — derivation output (`!<output>!<drvPath>`)
  - `using Raw = std::variant<Opaque, DrvDeep, Built>`
  - `Raw raw` — payload
  - `GENERATE_CMP(NixStringContextElem, me->raw)` — comparison ops
  - `MAKE_WRAPPER_CONSTRUCTOR(NixStringContextElem)` — variant-wrapping ctors
  - `static NixStringContextElem parse(std::string_view, const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings)` — decode from internal encoding
  - `std::string to_string() const` — encode back to internal form
  - `std::string display(const StoreDirConfig &) const` — human-readable form using `DerivedPath` syntax

### Type aliases
- `typedef std::set<NixStringContextElem> NixStringContext` — collection alias; comment notes pending rename to `StringContextBuilder`

## File: src/libexpr/value/context.cc

### Namespaces
- `nix`

### Functions
- `NixStringContextElem NixStringContextElem::parse(std::string_view, const ExperimentalFeatureSettings &)` — recursive deducing-this lambda parses optional trailing `!<drv>` chain; switches on first byte: `!` (Built), `=` (DrvDeep), default (Opaque or chain); throws `BadNixStringContextElem` on empty, missing second `!`, or extraneous `!`
- `std::string NixStringContextElem::to_string() const` — uses `overloaded` visitor; prepends `!` for Built and `=` for DrvDeep; nested `SingleDerivedPath` walked recursively
- `std::string NixStringContextElem::display(const StoreDirConfig &) const` — Opaque/Built render via `SingleDerivedPath::to_string`; DrvDeep appends " (deep)"

## File: src/libexpr/include/nix/expr/attr-path.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `MakeError(AttrPathNotFound, Error)` — error for missing attribute in selection path
- `MakeError(NoPositionInfo, Error)` — error when value has no source location
- `struct AttrPath : std::vector<Symbol>` — path of symbols, inherits ctors via `using std::vector<Symbol>::vector`
  - `static AttrPath parse(EvalState &, std::string_view)` — parse dotted (with quoted segments) attr path
  - `std::string to_string(EvalState &) const` — render with `.` separator
  - `std::vector<SymbolStr> resolve(EvalState &) const` — resolve to string views

### Functions
- `std::pair<Value *, PosIdx> findAlongAttrPath(EvalState &, const std::string & attrPath, Bindings & autoArgs, Value & vIn)` — descend a value following an attr path, applying `autoCallFunction` at each step
- `std::pair<SourcePath, uint32_t> findPackageFilename(EvalState &, Value &, std::string what)` — heuristic: looks at `meta.position`, coerces to path, splits on last `:` for line number

## File: src/libexpr/attr-path.cc

### Namespaces
- `nix`

### Functions
- `static Strings parseAttrPath(std::string_view)` — splits path on `.` honouring `"`-quoted segments; throws `ParseError` on missing closing quote
- `AttrPath AttrPath::parse(EvalState &, std::string_view)` — internalises tokens through symbol table
- `std::string AttrPath::to_string(EvalState &) const` — uses `dropEmptyInitThenConcatStringsSep`
- `std::vector<SymbolStr> AttrPath::resolve(EvalState &) const`
- `std::pair<Value *, PosIdx> findAlongAttrPath(EvalState &, const std::string &, Bindings &, Value &)` — token loop; at each step `autoCallFunction(autoArgs, *v, *vNew)` then `forceValue(*v, noPos)`; integer index for lists, symbol lookup for attrsets; emits suggestion-rich `AttrPathNotFound`
- `std::pair<SourcePath, uint32_t> findPackageFilename(EvalState &, Value &, std::string)` — calls `findAlongAttrPath(state, "meta.position", emptyBindings, v)`, throws `NoPositionInfo` on miss; coerces to path and splits on last `:`; throws `ParseError` if line not parseable

## File: src/libexpr/include/nix/expr/symbol-table.hh

### Namespaces
- `nix`
- `std` (specialisation of `hash<nix::Symbol>`)

### Classes / structs / enums
- `class SymbolValue : protected Value` — interned-string-typed `Value` with extra `uint32_t idx`; befriended by `SymbolStr` and `SymbolTable`; private default ctor; converts implicitly to `string_view`
- `class StaticSymbolTable` — forward declaration here
- `class Symbol` — opaque 32-bit handle (`id == 0` reserved); private explicit ctor `Symbol(uint32_t)`; befriends `SymbolStr`, `SymbolTable`, `StaticSymbolTable`
  - `constexpr Symbol() noexcept` — id-0 sentinel
  - `constexpr explicit operator bool() const noexcept` — `id > 0`
  - `constexpr uint32_t getId() const noexcept` — exposed for `switch`-case dispatch
  - `constexpr auto operator<=>(const Symbol &) const noexcept = default`
  - `friend class std::hash<Symbol>`
- `class SymbolStr` — wrapper combining a pointer to a `SymbolValue`
  - `static constexpr size_t chunkSize{65536}`
  - `using SymbolValueStore = ChunkedVector<SymbolValue, chunkSize, max-id/chunkSize>` (`MaxChunks = numeric_limits<uint32_t>::max() / chunkSize`)
  - private `const SymbolValue * s`
  - `struct Key { using HashType = boost::hash<std::string_view>; SymbolValueStore & store; std::string_view s; std::size_t hash; std::pmr::memory_resource & resource; Key(SymbolValueStore &, std::string_view, std::pmr::memory_resource & stringMemory); }`
  - `SymbolStr(const SymbolValue &) noexcept` — wrap existing value
  - `SymbolStr(const Key &)` — append-on-miss insertion (asserts `size < numeric_limits<uint32_t>::max()`); `mkStringNoCopy("`""_sds`", nullptr)` for empty, otherwise allocates via `StringData::make`
  - `bool operator==(std::string_view) const noexcept`
  - `const StringData & string_data() const noexcept`
  - `const char * c_str() const noexcept`
  - `operator std::string_view() const noexcept`
  - `friend std::ostream & operator<<(std::ostream &, const SymbolStr &)` (definition in eval.cc)
  - `bool empty() const noexcept` — fast-path on `""_sds` sentinel
  - `size_t size() const noexcept`
  - `const Value * valuePtr() const noexcept`
  - `explicit operator Symbol() const noexcept` — `Symbol{s->idx + 1}`
  - `struct Hash` — transparent boost-hash functor for SymbolStr & Key (`is_avalanching = std::true_type`)
  - `struct Equal` — transparent equality (pointer compare for SymbolStr×SymbolStr; string compare across SymbolStr×Key)
- `class StaticSymbolTable` — compile-time-fillable map (`maxSize = 1024`)
  - `struct StaticSymbolInfo { std::string_view str; Symbol sym; }`
  - private `std::array<StaticSymbolInfo, maxSize> symbols`, `std::size_t size = 0`
  - `constexpr StaticSymbolTable() = default`
  - `constexpr Symbol create(std::string_view)` — allocate next id (`size + 1`, 1-based)
  - `void copyIntoSymbolTable(SymbolTable &) const` — replays inserts into runtime table
- `class SymbolTable` — main runtime symbol table
  - private `std::pmr::synchronized_pool_resource fallbackResource{}`
  - private `BumpMemoryResource buffer{BumpMemoryResource::defaultReserveSize, &fallbackResource}` — primary monotonic allocator
  - private `SymbolStr::SymbolValueStore store` — append-only chunked vector of values
  - private `boost::concurrent_flat_set<SymbolStr, SymbolStr::Hash, SymbolStr::Equal> symbols{SymbolStr::chunkSize}` — interning set
  - `SymbolTable(const StaticSymbolTable &)` — initialises from static table via `copyIntoSymbolTable`
  - `Symbol create(std::string_view)` — uses `insert_and_cvisit` to either insert or look up
  - `std::vector<SymbolStr> resolve(const std::span<const Symbol> &) const`
  - `SymbolStr operator[](Symbol) const` — `s.id - 1` lookup, `unreachable()` on out-of-bounds; comment about happens-before guarantees
  - `size_t size() const noexcept` — count
  - `size_t totalSize() const` — declared (defined in eval.cc)
  - `template<typename T> void dump(T) const` — iterate via `store.forEach`
- `template<> struct std::hash<nix::Symbol>` — id-based hash specialisation

### Functions
- `inline void StaticSymbolTable::copyIntoSymbolTable(SymbolTable &) const` — sanity-checks `sym != staticSym` via `unreachable()`

## File: src/libexpr/include/nix/expr/eval-gc.hh

### Namespaces
- `nix`

### Classes / structs / enums (only when `NIX_USE_BOEHMGC` is false)
- `template<typename T> using traceable_allocator = std::allocator<T>` — dummy alias (in global namespace)
- `template<typename T> using gc_allocator = std::allocator<T>` — dummy alias
- `struct gc {}`, `struct gc_cleanup {}` — empty stand-ins for Boehm `gc` base classes (in global namespace)

### Functions
- `void initGC()` — initialise Boehm GC (no-op when disabled)
- `void assertGCInitialized()` — assert `initGC` was called
- `size_t getGCCycles()` — count of GC cycles since init (only when `NIX_USE_BOEHMGC`)

### Macros / globals
- `GC_INCLUDE_NEW`, `GC_THREADS 1` — pre-include defines for libgc (when Boehm enabled)
- `GC_MALLOC_ATOMIC std::malloc` — fallback when Boehm absent

## File: src/libexpr/eval-gc.cc

### Namespaces
- `nix`

### Functions (under `NIX_USE_BOEHMGC`)
- `static void * oomHandler(size_t requested)` — converts Boehm OOM into `std::bad_alloc`
- `static inline void initGCReal()` — disables interior pointers (`GC_set_all_interior_pointers(0)`) and DLS scanning (`GC_set_no_dls(1)`); enables perf measurement; `GC_INIT()`; `GC_allow_register_threads()`; if bit-packed value storage, `GC_register_displacement(i)` for `i ∈ [1, sizeof(uintptr_t))`; `GC_set_oom_fn(oomHandler)`; sizes initial heap to 25% of RAM (capped at 384 MiB) via `sysconf(_SC_PAGESIZE)/_SC_PHYS_PAGES` and `GC_expand_hp`, unless `GC_INITIAL_HEAP_SIZE` env override
- `size_t getGCCycles()` — `GC_get_gc_no()` minus `gcCyclesAfterInit`

### Functions (always defined)
- `void initGC()` — guards against double init via `gcInitialised`; calls `initGCReal()`; honours `NIX_PATH` env, overriding `nix-path` setting via `globalConfig.set`
- `void assertGCInitialized()` — `assert(gcInitialised)`

### Macros / globals
- `static_assert(sizeof(void *) * 2 == GC_GRANULE_BYTES, ...)` — enforces Boehm 2-word granule (Boehm-only)
- `static size_t gcCyclesAfterInit = 0` (Boehm-only)
- `static bool gcInitialised = false`

## File: src/libexpr/include/nix/expr/eval-error.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `class EvalBaseError : public CloneableError<EvalBaseError, Error>` — base; holds `EvalState & state`; `template<class T> friend class EvalErrorBuilder`
  - `EvalState & state` — public
  - `EvalBaseError(EvalState &, ErrorInfo &&)`
  - `template<typename... Args> explicit EvalBaseError(EvalState &, const std::string & formatString, const Args &... formatArgs)`
- `MakeError(EvalError, EvalBaseError)` — pure-mode-cacheable evaluation error
- `MakeError(ParseError, Error)` — parser error (not an EvalError; caching distinct)
- `MakeError(AssertionError, EvalError)` — `assert` failure
- `MakeError(ThrownError, AssertionError)` — `throw`/`builtins.throw`
- `MakeError(Abort, EvalError)` — `builtins.abort`
- `MakeError(TypeError, EvalError)`
- `MakeError(UndefinedVarError, EvalError)`
- `MakeError(MissingArgumentError, EvalError)` — missing function argument
- `MakeError(InfiniteRecursionError, EvalError)` — infinite recursion detected
- `struct StackOverflowError : public CloneableError<StackOverflowError, EvalBaseError>` — `max-call-depth` exhaustion; deliberately not cached
  - `StackOverflowError(EvalState & state)` — formats "stack overflow; max-call-depth exceeded"
- `MakeError(IFDError, EvalBaseError)` — import-from-derivation error
- `MakeError(RecoverableEvalError, EvalBaseError)` — error meant to be retried; not cached
- `struct InvalidPathError : public CloneableError<InvalidPathError, EvalError>` — path not in store
  - `StorePath path` — public
  - `InvalidPathError(EvalState &, const StorePath &)`
- `template<class T> class EvalErrorBuilder final` — fluent builder; private templated ctor `EvalErrorBuilder(EvalState &, const Args &...)` constructs `T error(state, args...)`; `friend class EvalState`
  - `T error` — public
  - `[[nodiscard, gnu::noinline]] EvalErrorBuilder<T> & withExitStatus(unsigned int)`
  - `[[nodiscard, gnu::noinline]] EvalErrorBuilder<T> & atPos(PosIdx)`
  - `[[nodiscard, gnu::noinline]] EvalErrorBuilder<T> & atPos(Value &, PosIdx fallback = noPos)`
  - `[[nodiscard, gnu::noinline]] EvalErrorBuilder<T> & withTrace(PosIdx, const std::string_view)`
  - `[[nodiscard, gnu::noinline]] EvalErrorBuilder<T> & withFrameTrace(PosIdx, const std::string_view)` — declared only, no definition
  - `[[nodiscard, gnu::noinline]] EvalErrorBuilder<T> & withSuggestions(Suggestions &)`
  - `[[nodiscard, gnu::noinline]] EvalErrorBuilder<T> & withFrame(const Env &, const Expr &)`
  - `[[nodiscard, gnu::noinline]] EvalErrorBuilder<T> & addTrace(PosIdx, HintFmt)`
  - `[[nodiscard, gnu::noinline]] EvalErrorBuilder<T> & setIsFromExpr()`
  - `template<typename... Args> [[nodiscard, gnu::noinline]] EvalErrorBuilder<T> & addTrace(PosIdx, std::string_view formatString, const Args &...)`
  - `[[gnu::noinline, gnu::noreturn]] void debugThrow()` — runs debug REPL, deletes builder, throws
  - `[[gnu::noinline, gnu::noreturn]] void panic()` — log + abort for fatal/programmer errors

## File: src/libexpr/eval-error.cc

### Namespaces
- `nix`

### Functions
- `InvalidPathError::InvalidPathError(EvalState &, const StorePath &)` — formats "path '%s' is not valid" via `state.store->printStorePath`
- `template<class T> EvalErrorBuilder<T> & EvalErrorBuilder<T>::withExitStatus(unsigned)` — delegates to error
- `template<class T> EvalErrorBuilder<T> & EvalErrorBuilder<T>::atPos(PosIdx)` — sets `err.pos` from `state.positions[pos]`
- `template<class T> EvalErrorBuilder<T> & EvalErrorBuilder<T>::atPos(Value &, PosIdx fallback)` — uses `value.determinePos(fallback)`
- `template<class T> EvalErrorBuilder<T> & EvalErrorBuilder<T>::withTrace(PosIdx, const std::string_view)` — adds trace via `error.addTrace(state.positions[pos], text)`
- `template<class T> EvalErrorBuilder<T> & EvalErrorBuilder<T>::withSuggestions(Suggestions &)`
- `template<class T> EvalErrorBuilder<T> & EvalErrorBuilder<T>::withFrame(const Env &, const Expr &)` — pushes a fake `DebugTrace` frame onto `state.debugTraces` with `isError = true`
- `template<class T> EvalErrorBuilder<T> & EvalErrorBuilder<T>::addTrace(PosIdx, HintFmt)`
- `template<class T> template<typename... Args> EvalErrorBuilder<T> & EvalErrorBuilder<T>::addTrace(PosIdx, std::string_view formatString, const Args &...)` — formats `HintFmt(string(formatString), formatArgs...)`
- `template<class T> EvalErrorBuilder<T> & EvalErrorBuilder<T>::setIsFromExpr()` — sets `err.isFromExpr = true`
- `template<class T> void EvalErrorBuilder<T>::debugThrow()` — runs `state.runDebugRepl(&error)`, moves error out, `delete this`, then `throw std::move(error)`
- `template<class T> void EvalErrorBuilder<T>::panic()` — `logError`, `printError` reproducible-example message, then `abort()`

### Macros / globals
- Explicit instantiations: `EvalErrorBuilder<EvalBaseError>`, `EvalErrorBuilder<EvalError>`, `EvalErrorBuilder<AssertionError>`, `EvalErrorBuilder<ThrownError>`, `EvalErrorBuilder<Abort>`, `EvalErrorBuilder<TypeError>`, `EvalErrorBuilder<UndefinedVarError>`, `EvalErrorBuilder<MissingArgumentError>`, `EvalErrorBuilder<InfiniteRecursionError>`, `EvalErrorBuilder<StackOverflowError>`, `EvalErrorBuilder<InvalidPathError>`, `EvalErrorBuilder<IFDError>`, `EvalErrorBuilder<RecoverableEvalError>`

## File: src/libexpr/include/nix/expr/eval-profiler-settings.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `enum struct EvalProfilerMode { disabled, flamegraph }` — profiler mode

### Macros / globals
- `NIX_DECLARE_CONFIG_SERIALISER(EvalProfilerMode)`

## File: src/libexpr/eval-profiler-settings.cc

### Namespaces
- `nix`

### Functions
- `template<> EvalProfilerMode BaseSetting<EvalProfilerMode>::parse(const std::string &) const` — string→enum; throws `UsageError` on invalid value
- `template<> std::string BaseSetting<EvalProfilerMode>::to_string() const` — enum→string ("disabled"/"flamegraph"); `unreachable()` otherwise

### Classes / structs / enums
- `template<> struct BaseSetting<EvalProfilerMode>::trait` — sets `static constexpr bool appendable = false`

### Macros / globals
- `NLOHMANN_JSON_SERIALIZE_ENUM(EvalProfilerMode, ...)`
- `template class BaseSetting<EvalProfilerMode>` — explicit instantiation

## File: src/libexpr/include/nix/expr/eval-profiler.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `class EvalProfiler` — base class
  - `enum Hook { preFunctionCall, postFunctionCall }`
  - `static constexpr std::size_t numHooks = Hook::postFunctionCall + 1`
  - `using Hooks = std::bitset<numHooks>`
  - private `std::optional<Hooks> neededHooks`
  - protected `void invalidateNeededHooks()` — drop cache (sets to nullopt)
  - protected `virtual Hooks getNeededHooksImpl() const` — default returns empty `Hooks{}`; subclasses override
  - public `virtual void preFunctionCallHook(EvalState &, const Value &, std::span<Value *>, const PosIdx)` — default no-op (defined in eval-profiler.cc)
  - public `virtual void postFunctionCallHook(EvalState &, const Value &, std::span<Value *>, const PosIdx)` — default no-op
  - `virtual ~EvalProfiler() = default`
  - `Hooks getNeededHooks()` — NVI cached accessor (caches `getNeededHooksImpl()` result)
- `class MultiEvalProfiler : public EvalProfiler` — fan-out wrapper
  - private `std::vector<ref<EvalProfiler>> profilers`
  - private `[[gnu::noinline]] Hooks getNeededHooksImpl() const override`
  - `MultiEvalProfiler() = default`
  - `void addProfiler(ref<EvalProfiler>)` — push back + `invalidateNeededHooks()`
  - `[[gnu::noinline]] void preFunctionCallHook(EvalState &, const Value &, std::span<Value *>, const PosIdx) override`
  - `[[gnu::noinline]] void postFunctionCallHook(EvalState &, const Value &, std::span<Value *>, const PosIdx) override`

### Functions
- `ref<EvalProfiler> makeSampleStackProfiler(EvalState &, std::filesystem::path profileFile, uint64_t frequency)` — factory for stack-sampling profiler

## File: src/libexpr/eval-profiler.cc

### Namespaces
- `nix`
- anonymous namespace inside `nix`

### Classes / structs / enums (anonymous)
- `class PosCache : private LRUCache<PosIdx, Pos>` — caches resolved positions; size 524288 (~40MiB)
  - `const EvalState & state`
  - `PosCache(const EvalState &)` — base ctor `LRUCache(524288)`
  - `Pos lookup(PosIdx)` — upserts on miss using `state.positions[posIdx]`
- `struct LambdaFrameInfo` — `ExprLambda * expr`, `PosIdx callPos = noPos`
  - `std::ostream & symbolize(const EvalState &, std::ostream &, PosCache &) const`
  - `auto operator<=>(const LambdaFrameInfo &) const = default`
- `struct PrimOpFrameInfo` — `const PrimOp * expr`, `PosIdx callPos = noPos`
  - `std::ostream & symbolize(const EvalState &, std::ostream &, PosCache &) const`
  - `auto operator<=>(const PrimOpFrameInfo &) const = default`
- `struct FunctorFrameInfo` — `PosIdx pos`
  - `std::ostream & symbolize(const EvalState &, std::ostream &, PosCache &) const`
  - `auto operator<=>(const FunctorFrameInfo &) const = default`
- `struct DerivationStrictFrameInfo` — `PosIdx callPos = noPos`, `std::string drvName`
  - `std::ostream & symbolize(const EvalState &, std::ostream &, PosCache &) const`
  - `auto operator<=>(const DerivationStrictFrameInfo &) const = default`
- `struct GenericFrameInfo` — `PosIdx pos`; fallback frame info
  - `std::ostream & symbolize(const EvalState &, std::ostream &, PosCache &) const`
  - `auto operator<=>(const GenericFrameInfo &) const = default`
- `using FrameInfo = std::variant<LambdaFrameInfo, PrimOpFrameInfo, FunctorFrameInfo, DerivationStrictFrameInfo, GenericFrameInfo>`
- `using FrameStack = std::vector<FrameInfo>`
- `class SampleStack : public EvalProfiler` — flamegraph stack-sampler profiler
  - `static constexpr std::chrono::microseconds profileDumpInterval = std::chrono::milliseconds(2000)`
  - private `Hooks getNeededHooksImpl() const override` — returns `Hooks().set(preFunctionCall).set(postFunctionCall)`
  - private `FrameInfo getPrimOpFrameInfo(const PrimOp &, std::span<Value *>, PosIdx)`
  - `SampleStack(EvalState &, const std::filesystem::path & profileFile, std::chrono::nanoseconds period)` — opens profile file with `truncateExisting + followSymlinksOnTruncate`; throws `SysError` if `openNewFileForWrite` fails
  - `[[gnu::noinline]] void preFunctionCallHook(...) override`
  - `[[gnu::noinline]] void postFunctionCallHook(...) override`
  - `void maybeSaveProfile(std::chrono::time_point<std::chrono::high_resolution_clock>)`
  - `void saveProfile()`
  - `FrameInfo getFrameInfoFromValueAndPos(const Value &, std::span<Value *>, PosIdx)`
  - move-constructible only (`SampleStack(SampleStack &&) = default`); copy and move-assignment deleted
  - `~SampleStack()` — saves profile, with `ignoreExceptionInDestructor` guard
  - private `EvalState & state`, `std::chrono::nanoseconds sampleInterval`, `AutoCloseFD profileFd`, `FrameStack stack`, `std::map<FrameStack, uint32_t> callCount`, `lastStackSample`, `lastDump`, `PosCache posCache`

### Functions
- `void EvalProfiler::preFunctionCallHook(...)` — empty default
- `void EvalProfiler::postFunctionCallHook(...)` — empty default
- `void MultiEvalProfiler::preFunctionCallHook(...)` — for each child, call hook only if it opted in
- `void MultiEvalProfiler::postFunctionCallHook(...)` — same
- `EvalProfiler::Hooks MultiEvalProfiler::getNeededHooksImpl() const` — OR all children's hooks
- `void MultiEvalProfiler::addProfiler(ref<EvalProfiler>)` — push back + invalidate cache
- `FrameInfo SampleStack::getPrimOpFrameInfo(const PrimOp &, std::span<Value *>, PosIdx)` — special-cases `derivationStrict`: forces args[0] attrs, reads `name`, captures `drvName`; falls back to `PrimOpFrameInfo`
- `FrameInfo SampleStack::getFrameInfoFromValueAndPos(const Value &, std::span<Value *>, PosIdx)` — branches on `isLambda` / `isPrimOp` / `isPrimOpApp` / `isFunctor` (functor uses `state.s.functor` attr position when callsite unresolved) / `GenericFrameInfo` fallback
- `[[gnu::noinline]] void SampleStack::preFunctionCallHook(...)` — push frame, sample if interval elapsed (`callCount[stack] += 1`), `maybeSaveProfile`
- `[[gnu::noinline]] void SampleStack::postFunctionCallHook(...)` — pops front of stack if non-empty
- `std::ostream & LambdaFrameInfo::symbolize(...)` — falls back to lambda location when callsite origin is `monostate`; appends `:<name>` if available
- `std::ostream & GenericFrameInfo::symbolize(...)`
- `std::ostream & FunctorFrameInfo::symbolize(...)` — appends `:functor`
- `std::ostream & PrimOpFrameInfo::symbolize(...)` — emits position then `*expr`
- `std::ostream & DerivationStrictFrameInfo::symbolize(...)` — emits "primop derivationStrict:<drvName>"
- `void SampleStack::maybeSaveProfile(...)` — if interval elapsed, save profile, update `lastDump`, clear `callCount`
- `void SampleStack::saveProfile()` — folded format (one line per stack with semicolons + count); writes via `writeLine` to `profileFd`
- `SampleStack::~SampleStack()` — guarded `saveProfile`
- `ref<EvalProfiler> makeSampleStackProfiler(EvalState &, std::filesystem::path, uint64_t frequency)` — `frequency == 0` → period 0 (sample every call); else `period = ns/frequency`

## File: src/libexpr/include/nix/expr/eval-settings.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `class DeprecatedWarnSetting : public BaseSetting<bool>` — bool setting that, on assignment, redirects to a target `Setting<Diagnose>` and warns
  - private `Setting<Diagnose> & target`, `const char * targetName`
  - `DeprecatedWarnSetting(Config *, Setting<Diagnose> & target, const char * targetName, const std::string & name, const std::string & description, const StringSet & aliases = {})` — registers via `options->addSetting(this)`
  - `void assign(const bool &) override`
  - `void appendOrSet(bool, bool) override`
  - `void override(const bool &) override`
- `struct EvalSettings : Config` — main evaluator configuration container
  - `using LookupPathHook = std::optional<SourcePath>(EvalState &, std::string_view)` — scheme handler signature
  - `using LookupPathHooks = std::map<std::string, fun<LookupPathHook>>` — scheme→hook
  - `EvalSettings(bool & readOnlyMode, LookupPathHooks lookupPathHooks = {})` — ctor
  - `bool * readOnlyMode = nullptr` — pointer to outer `readOnlyMode`
  - `bool isReadOnly() const` — `assert(readOnlyMode); return *readOnlyMode`
  - `static Strings getDefaultNixPath()` — built-in defaults (channels, root nixpkgs)
  - `static bool isPseudoUrl(std::string_view)` — recognises `channel:` prefix and common URI schemes
  - `static Strings parseNixPath(const std::string &)` — colon-aware splitter that respects URLs
  - `static std::string resolvePseudoUrl(std::string_view)` — `channel:foo` → channels.nixos.org URL
  - `LookupPathHooks lookupPathHooks` — runtime registry
  - `std::vector<PrimOp> extraPrimOps` — additional primops
  - `Setting<bool> enableNativeCode` (`allow-unsafe-native-code-during-evaluation`, default false) — gates `builtins.importNative`/`builtins.exec`
  - `Setting<Strings> nixPath` (`nix-path`, default `{}`, deprecated=false aliased)
  - `Setting<std::string> currentSystem` (`eval-system`, default empty) — overrides `builtins.currentSystem`
  - `const std::string & getCurrentSystem() const` — `eval-system` → `settings.thisSystem` fallback
  - `Setting<bool> restrictEval` (`restrict-eval`, default false)
  - `Setting<bool> pureEval` (`pure-eval`, default false)
  - `Setting<bool> traceImportFromDerivation` (`trace-import-from-derivation`, default false)
  - `Setting<bool> enableImportFromDerivation` (`allow-import-from-derivation`, default true)
  - `Setting<Strings> allowedUris` (`allowed-uris`, default `{}`)
  - `Setting<bool> traceFunctionCalls` (`trace-function-calls`, default false)
  - `Setting<EvalProfilerMode> evalProfilerMode` (`eval-profiler`, default `disabled`)
  - `Setting<std::filesystem::path> evalProfileFile` (`eval-profile-file`, default `"nix.profile"`)
  - `Setting<uint32_t> evalProfilerFrequency` (`eval-profiler-frequency`, default 99 Hz)
  - `Setting<bool> useEvalCache` (`eval-cache`, default true)
  - `Setting<bool> ignoreExceptionsDuringTry` (`ignore-try`, default false)
  - `Setting<bool> traceVerbose` (`trace-verbose`, default false)
  - `Setting<unsigned int> maxCallDepth` (`max-call-depth`, default 10000)
  - `Setting<bool> builtinsTraceDebugger` (`debugger-on-trace`, default false)
  - `Setting<bool> builtinsDebuggerOnWarn` (`debugger-on-warn`, default false)
  - `Setting<bool> builtinsAbortOnWarn` (`abort-on-warn`, default false; ctor reads `NIX_ABORT_ON_WARN` env var)
  - `Setting<Diagnose> lintShortPathLiterals` (`lint-short-path-literals`, default `Diagnose::Ignore`)
  - `DeprecatedWarnSetting warnShortPathLiterals` — alias setting with name `lint-short-path-literals`, deprecation alias `warn-short-path-literals`, redirecting to `lintShortPathLiterals`
  - `Setting<Diagnose> lintAbsolutePathLiterals` (`lint-absolute-path-literals`, default `Diagnose::Ignore`)
  - `Setting<Diagnose> lintUrlLiterals` (`lint-url-literals`, default `Diagnose::Ignore`)
  - `Setting<unsigned> bindingsUpdateLayerRhsSizeThreshold` (`eval-attrset-update-layer-rhs-threshold`, default 8192 on 32-bit, 16 on 64-bit; `0` disables)

### Functions
- `std::filesystem::path getNixDefExpr()` — `~/.nix-defexpr` or `getStateDir()/defexpr` depending on `useXDGBaseDirectories`

## File: src/libexpr/eval-settings.cc

### Namespaces
- `nix`

### Functions
- `void DeprecatedWarnSetting::assign(const bool & v)` — store value, emit `warn(...)` deprecation message, propagate to `target` Diagnose unless `target.overridden`
- `void DeprecatedWarnSetting::appendOrSet(bool, bool append)` — `assert(!append)`, delegates to `assign`
- `void DeprecatedWarnSetting::override(const bool &)` — sets `overridden = true`, then `assign`
- `Strings EvalSettings::parseNixPath(const std::string &)` — handcrafted colon-splitting parser; preserves URLs / pseudo-URLs / `flake:` entries
- `EvalSettings::EvalSettings(bool &, LookupPathHooks)` — ctor; sets `readOnlyMode` pointer; checks `NIX_ABORT_ON_WARN` env (`"1"`/`"yes"`/`"true"`) and sets `builtinsAbortOnWarn`
- `Strings EvalSettings::getDefaultNixPath()` — adds (only if path exists): `~/.nix-defexpr/channels`, `<root profiles>/channels/nixpkgs` aliased as `nixpkgs`, `<root profiles>/channels`
- `bool EvalSettings::isPseudoUrl(std::string_view)` — recognises `channel:` prefix and `://` schemes (`http`/`https`/`file`/`channel`/`git`/`s3`/`ssh`)
- `std::string EvalSettings::resolvePseudoUrl(std::string_view)` — expands `channel:<name>` to `https://channels.nixos.org/<name>/nixexprs.tar.xz`; otherwise returns the URL unchanged
- `const std::string & EvalSettings::getCurrentSystem() const` — `eval-system` setting if non-empty, else `settings.thisSystem.get()`
- `std::filesystem::path getNixDefExpr()` — `useXDGBaseDirectories ? getStateDir()/"defexpr" : getHome()/".nix-defexpr"`

## File: src/libexpr/include/nix/expr/eval-cache.hh

### Namespaces
- `nix::eval_cache`

### Classes / structs / enums
- `struct AttrDb` — forward declaration (defined in .cc)
- `class AttrCursor` — forward declaration
- `struct CachedEvalError : CloneableError<CachedEvalError, EvalError>` — represents a cached failure
  - `const ref<AttrCursor> cursor` — public
  - `const Symbol attr` — public
  - `CachedEvalError(ref<AttrCursor>, Symbol)`
  - `[[noreturn]] void force()` — re-evaluates the underlying value to surface the original error
- `class EvalCache : public std::enable_shared_from_this<EvalCache>` — top-level cache holder
  - `friend class AttrCursor`, `friend struct CachedEvalError`
  - private `std::shared_ptr<AttrDb> db`, `EvalState & state`, `RootLoader rootLoader`, `RootValue value`
  - private `Value * getRootValue()`
  - `typedef fun<Value *()> RootLoader` — lazy root value supplier
  - `EvalCache(std::optional<std::reference_wrapper<const Hash>> useCache, EvalState &, RootLoader)`
  - `ref<AttrCursor> getRoot()` — returns root cursor
- `enum AttrType { Placeholder = 0, FullAttrs = 1, String = 2, Missing = 3, Misc = 4, Failed = 5, Bool = 6, ListOfStrings = 7, Int = 8 }`
- `struct placeholder_t {}`, `struct missing_t {}`, `struct misc_t {}`, `struct failed_t {}` — empty tag types
- `struct int_t { NixInt x; }` — wrapper for int values
- `class AttrCursor : public std::enable_shared_from_this<AttrCursor>` — attribute traversal node combining a cached row and live `Value *`
  - `friend class EvalCache`, `friend struct CachedEvalError`
  - private `ref<EvalCache> root`, `using Parent = std::optional<std::pair<ref<AttrCursor>, Symbol>>`, `Parent parent`, `RootValue _value`, `std::optional<std::pair<AttrId, AttrValue>> cachedValue`
  - private `AttrKey getKey()`
  - private `Value & getValue()`
  - private `void fetchCachedValue()` — populates from DB; throws `CachedEvalError` on `failed_t`
  - `AttrCursor(ref<EvalCache>, Parent, Value * = nullptr, std::optional<std::pair<AttrId, AttrValue>> && = {})`
  - `AttrPath getAttrPath() const`
  - `AttrPath getAttrPath(Symbol) const`
  - `std::string getAttrPathStr() const`
  - `std::string getAttrPathStr(Symbol) const`
  - `Suggestions getSuggestionsForAttr(Symbol)` — Levenshtein over attr names
  - `std::shared_ptr<AttrCursor> maybeGetAttr(Symbol)` — null on absence
  - `std::shared_ptr<AttrCursor> maybeGetAttr(std::string_view)`
  - `ref<AttrCursor> getAttr(Symbol)` — throws on absence
  - `ref<AttrCursor> getAttr(std::string_view)`
  - `OrSuggestions<ref<AttrCursor>> findAlongAttrPath(const AttrPath &)` — multi-step traversal, no functor auto-call
  - `std::string getString()`
  - `string_t getStringWithContext()`
  - `bool getBool()`
  - `NixInt getInt()`
  - `std::vector<std::string> getListOfStrings()`
  - `std::vector<Symbol> getAttrs()`
  - `bool isDerivation()` — checks `type == "derivation"`
  - `Value & forceValue()` — force underlying value; on `EvalError` records `failed_t` and rethrows; persists String/Path/Bool/Int/Misc on success
  - `StorePath forceDerivation()` — ensures `.drv` exists, regenerating if GC'd

### Type aliases
- `typedef uint64_t AttrId` — DB row id
- `typedef std::pair<AttrId, Symbol> AttrKey` — composite cache key
- `typedef std::pair<std::string, NixStringContext> string_t` — string + context
- `typedef std::variant<std::vector<Symbol>, string_t, placeholder_t, missing_t, misc_t, failed_t, bool, int_t, std::vector<std::string>> AttrValue` — cached payload variant

## File: src/libexpr/eval-cache.cc

### Namespaces
- `nix::eval_cache`

### Classes / structs / enums
- `struct AttrDb` — SQLite-backed cache
  - `std::atomic_bool failed{false}` — sticky failure flag
  - `const StoreDirConfig & cfg`
  - `struct State { SQLite db; SQLiteStmt insertAttribute; SQLiteStmt insertAttributeWithContext; SQLiteStmt queryAttribute; SQLiteStmt queryAttributes; std::unique_ptr<SQLiteTxn> txn; }`
  - `std::unique_ptr<Sync<State>> _state` — locked state
  - `SymbolTable & symbols`
  - `AttrDb(const StoreDirConfig &, const Hash & fingerprint, SymbolTable &)` — opens `<cache>/eval-cache-v6/<fingerprint>.sqlite`, creates schema, prepares statements (`insert or replace`, `select`), opens `SQLiteTxn`
  - `~AttrDb()` — commits transaction if not failed; uses `ignoreExceptionInDestructor`
  - `template<typename F> AttrId doSQLite(const F &)` — guards body, sets `failed = true` on `SQLiteError`; returns 0 if already failed
  - `AttrId setAttrs(AttrKey, const std::vector<Symbol> &)` — writes parent `FullAttrs` row + child `Placeholder` rows
  - `AttrId setString(AttrKey, std::string_view, const Value::StringWithContext::Context * = nullptr)` — writes string with optional space-joined context
  - `AttrId setBool(AttrKey, bool)`
  - `AttrId setInt(AttrKey, int)`
  - `AttrId setListOfStrings(AttrKey, const std::vector<std::string> &)` — joins with `\t`
  - `AttrId setPlaceholder(AttrKey)`
  - `AttrId setMissing(AttrKey)`
  - `AttrId setMisc(AttrKey)`
  - `AttrId setFailed(AttrKey)`
  - `std::optional<std::pair<AttrId, AttrValue>> getAttr(AttrKey)` — query row, switch on `AttrType`, decode into variant; throws `Error` on unexpected type

### Functions
- `CachedEvalError::CachedEvalError(ref<AttrCursor>, Symbol)` — formats "cached failure of attribute '%s'" using `cursor->getAttrPathStr(attr)`
- `void CachedEvalError::force()` — re-forces parent value; if attrs, looks up and forces the child to surface real error; throws `EvalError` if it succeeds unexpectedly
- `static std::shared_ptr<AttrDb> makeAttrDb(const StoreDirConfig &, const Hash &, SymbolTable &)` — try-build, returns `nullptr` on `SQLiteError`
- `EvalCache::EvalCache(std::optional<std::reference_wrapper<const Hash>>, EvalState &, RootLoader)` — sets up `db` if cache requested
- `Value * EvalCache::getRootValue()` — lazily allocates root via `rootLoader`
- `ref<AttrCursor> EvalCache::getRoot()` — wraps shared `EvalCache`
- `AttrCursor::AttrCursor(ref<EvalCache>, Parent, Value *, std::optional<std::pair<AttrId, AttrValue>> &&)` — root-allocs supplied value via `allocRootValue`
- `AttrKey AttrCursor::getKey()` — recursively forces parent's cached value to obtain DB row id; uses `s.epsilon` for root
- `Value & AttrCursor::getValue()` — derives from parent's `attrs()->get(parent->second)`; allocates root value if no parent
- `void AttrCursor::fetchCachedValue()` — populates from DB; throws `CachedEvalError(parent->first, parent->second)` if cached as `failed_t` and has parent
- `AttrPath AttrCursor::getAttrPath() const`
- `AttrPath AttrCursor::getAttrPath(Symbol) const`
- `std::string AttrCursor::getAttrPathStr() const`
- `std::string AttrCursor::getAttrPathStr(Symbol) const`
- `Value & AttrCursor::forceValue()` — force underlying value; on `EvalError` record `Failed` and rethrow; on success persist String (with context)/Path-as-string/Bool/Int/Misc; `nAttrs` deliberately not persisted here
- `Suggestions AttrCursor::getSuggestionsForAttr(Symbol)` — Levenshtein from `getAttrs`
- `std::shared_ptr<AttrCursor> AttrCursor::maybeGetAttr(Symbol)` — multi-tier lookup: cached attrs list; else `placeholder_t` triggers DB lookup with `missing_t`/`failed_t`/result handling; falls through to fresh evaluation (records `Missing`/`Placeholder` as needed)
- `std::shared_ptr<AttrCursor> AttrCursor::maybeGetAttr(std::string_view)` — interns symbol then dispatches
- `ref<AttrCursor> AttrCursor::getAttr(Symbol)` — throws `Error("attribute '%s' does not exist", ...)` on absence
- `ref<AttrCursor> AttrCursor::getAttr(std::string_view)`
- `OrSuggestions<ref<AttrCursor>> AttrCursor::findAlongAttrPath(const AttrPath &)` — descent loop with suggestion-rich failure
- `std::string AttrCursor::getString()` — cached string read; for `nPath` returns `path().to_string()`
- `string_t AttrCursor::getStringWithContext()` — re-validates store paths in cached context (visits `DrvDeep`/`Built`/`Opaque`); falls back to fresh evaluation if any path invalid
- `bool AttrCursor::getBool()`
- `NixInt AttrCursor::getInt()`
- `std::vector<std::string> AttrCursor::getListOfStrings()`
- `std::vector<Symbol> AttrCursor::getAttrs()` — sorted by string view of symbol name
- `bool AttrCursor::isDerivation()` — checks `type == "derivation"`
- `StorePath AttrCursor::forceDerivation()` — derives drv path; calls `addTempRoot`; if path is GC'd, re-forces `aDrvPath` and verifies; throws `Error` if recreation fails

### Macros / globals
- `static const char * schema = R"sql(create table if not exists Attributes (parent integer not null, name text, type integer not null, value text, context text, primary key (parent, name));)sql"` — eval-cache table

## File: src/libexpr/include/nix/expr/value.hh

### Namespaces
- `nix`
- `nix::detail`

### Classes / structs / enums
- `enum InternalType` — fine-grained discriminator (ordering load-bearing for bit-packed storage)
  - `tUninitialized = 0`
  - Single/zero-field payload: `tInt = 1`, `tBool`, `tNull`, `tFloat`, `tFailed`, `tExternal`, `tPrimOp`, `tAttrs`
  - Pair-of-pointers payload: `tFirstPairOfPointers`, `tListSmall = tFirstPairOfPointers`, `tPrimOpApp`, `tApp`, `tThunk`, `tLambda`, `tLastPairOfPointers = tLambda`
  - Single-untaggable payload: `tFirstSingleUntaggable`, `tListN = tFirstSingleUntaggable`, `tString`, `tPath`
  - sentinel `tNumberOfInternalTypes`
- `typedef enum { nThunk, nFailed, nInt, nFloat, nBool, nString, nPath, nNull, nAttrs, nList, nFunction, nExternal } ValueType` — coarse user-visible types
- `class ExternalValueBase` — abstract base for plug-in external values
  - `friend std::ostream & operator<<(std::ostream &, const ExternalValueBase &)`
  - `friend class Printer`
  - protected `virtual std::ostream & print(std::ostream &) const = 0`
  - public `virtual std::string showType() const = 0`
  - public `virtual std::string typeOf() const = 0`
  - `virtual std::string coerceToString(EvalState &, const PosIdx &, NixStringContext &, bool copyMore, bool copyToStore) const`
  - `virtual bool operator==(const ExternalValueBase &) const noexcept`
  - `virtual nlohmann::json printValueAsJSON(EvalState &, bool strict, NixStringContext &, bool copyToStore = true) const`
  - `virtual void printValueAsXML(EvalState &, bool strict, bool location, XMLWriter &, NixStringContext &, StringSet &, const PosIdx) const`
  - `virtual ~ExternalValueBase() {}`
- `class ListBuilder` — short-lived builder used while constructing list values
  - private `const size_t size`
  - private `Value * inlineElems[2] = {nullptr, nullptr}`
  - public `Value ** elems`
  - `ListBuilder(EvalMemory &, size_t)` (defined in eval.cc)
  - `ListBuilder(ListBuilder &&) noexcept` — moves inline elems and pointer
  - copy ctor and copy/move assignment deleted
  - `~ListBuilder() = default`
  - `Value *& operator[](size_t)`
  - `typedef Value ** iterator`
  - `iterator begin()`, `iterator end()`
  - `friend struct Value`
- `class StringData` — flexible-array-member backed string with explicit size; non-copyable/non-movable
  - `using size_type = std::size_t`
  - `size_type size_; char data_[]` — public members (FAM)
  - copy/move ctor/assignment all deleted
  - `~StringData() = default`
  - private default ctor `StringData()` deleted
  - private explicit `StringData(size_type)`
  - `static const StringData & make(EvalMemory &, std::string_view)` — GC-allocated copy (defined in eval.cc)
  - `static StringData & alloc(EvalMemory &, size_t size)` — uninitialized GC-allocated
  - `size_t size() const`, `char * data() noexcept`, `const char * data() const noexcept`, `const char * c_str() const noexcept`, `constexpr std::string_view view() const noexcept`
  - `template<size_t N> struct Static` — declared here, defined in static-string-data.hh
  - `static StringData & make(std::pmr::memory_resource &, std::string_view)` — pmr-allocated copy (inline)

### `namespace nix::detail`
- `struct ValueBase` — mixin providing the payload field types used by `ValueStorage`
  - `struct StringWithContext`
    - `const StringData * str`
    - `struct Context` — flexible-array of `const StringData *` pointers (sorted)
      - `using value_type = const StringData *`, `using size_type = std::size_t`, `using iterator = const value_type *`
      - `Context(size_type size)` — explicit ctor stores size
      - private `size_type size_`, `value_type elems[]`
      - `iterator begin() const`, `iterator end() const`, `size_type size() const`
      - `static Context * fromBuilder(const NixStringContext &, EvalMemory &)` — null when context is empty
    - `const Context * context` — may be null for context-free strings
  - `struct Path { SourceAccessor * accessor; const StringData * path; }`
  - `struct Null {}`
  - `struct ClosureThunk { Env * env; Expr * expr; }`
  - `struct FunctionApplicationThunk { Value *left, *right; }`
  - `struct PrimOpApplicationThunk { Value *left, *right; }` — distinct type for overload resolution
  - `struct Lambda { Env * env; ExprLambda * fun; }`
  - `using SmallList = std::array<Value *, 2>` — inline list of <=2 elements
  - `struct List { size_t size; Value * const * elems; }` — for `tListN`
  - `struct Failed : gc_cleanup`
    - `std::exception_ptr ex`
    - `Value * recoveryValue` — must be set iff `ex` is `RecoverableEvalError`
    - `Failed(std::exception_ptr, Value *)` — asserts `ex` is non-null
    - `[[noreturn]] void rethrow() const` — rethrows clone via `BaseError::throwClone`
- `template<typename T> struct PayloadTypeToInternalType` — empty primary; specialised by `NIX_VALUE_PAYLOAD_TYPE`
- `template<typename T> inline constexpr InternalType payloadTypeToInternalType` — alias to `PayloadTypeToInternalType<T>::value`
- `template<std::size_t ptrSize> inline constexpr bool useBitPackedValueStorage = (ptrSize == 8) && (__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= 16)` — controls choice of storage specialisation

### Storage templates
- `template<std::size_t ptrSize, typename Enable = void> class ValueStorage : public detail::ValueBase` — generic discriminator+union implementation
  - protected `using Payload = union { ... NIX_VALUE_STORAGE_FOR_EACH_FIELD ... }`
  - private `InternalType internalType = tUninitialized`, `Payload payload`
  - protected `getStorage(K &) const noexcept` / `setStorage(K) noexcept` overloads (one pair per payload type via `NIX_VALUE_STORAGE_GET_IMPL` / `NIX_VALUE_STORAGE_SET_IMPL`)
  - protected `InternalType getInternalType() const noexcept`
  - protected `static bool isAtomic()` — returns false in this specialisation
- `template<std::size_t ptrSize> class alignas(16) ValueStorage<ptrSize, std::enable_if_t<detail::useBitPackedValueStorage<ptrSize>>>` — 64-bit bit-packing specialisation (inherits `detail::ValueBase`)
  - inner `template<size_t> struct PackedPointerTypeStruct { using type = std::uint64_t; }`
  - `using PackedPointer = typename PackedPointerTypeStruct<ptrSize>::type`, `using Payload = std::array<PackedPointer, 2>`
  - storage member: `__m128i payloadWords` (when `__x86_64__ && __SSE2__`) else `Payload payloadWords = {}`
  - `static constexpr int discriminatorBits = 3`, `static constexpr PackedPointer discriminatorMask = (PackedPointer(1) << discriminatorBits) - 1`
  - inner `enum PrimaryDiscriminator : int { pdUninitialized = 0, pdSingleDWord, pdListN, pdString, pdPath, pdPairOfPointers }`
  - private `[[gnu::always_inline]] void updatePayload(Payload) noexcept` — atomic 16-byte stores via `_mm_store_si128` (MOVAPS) on x86_64+SSE2; plain assignment otherwise
  - private `[[gnu::always_inline]] Payload loadPayload() const noexcept` — `_mm_load_si128` (MOVDQA) on x86_64+SSE2
  - private `template<typename T> requires std::is_pointer_v<T> static T untagPointer(PackedPointer) noexcept`
  - private `PrimaryDiscriminator getPrimaryDiscriminator(PackedPointer) const noexcept`
  - private `static void assertAligned(PackedPointer) noexcept`
  - private `template<InternalType type> void setSingleDWordPayload(PackedPointer untaggedVal) noexcept`
  - private `template<PrimaryDiscriminator discriminator, typename T, typename U> void setUntaggablePayload(T * firstPtrField, U untaggableField) noexcept`
  - private `template<InternalType type, typename T, typename U> void setPairOfPointersPayload(T * firstPtrField, U * secondPtrField) noexcept`
  - private `template<typename T, typename U> requires std::is_pointer_v<T> && std::is_pointer_v<U> void getPairOfPointersPayload(T &, U &) const noexcept`
  - public `ValueStorage()` — zero-init
  - public copy ctor, copy assignment, move ctor (`noexcept`), move assignment (`noexcept`) — copy/move semantics via `loadPayload`/`updatePayload`; move zero-out RHS
  - public `~ValueStorage() noexcept {}`
  - protected `static bool isAtomic()` — defined in value.cc; checks AVX via libcpuid
  - protected `InternalType getInternalType() const noexcept` — switch on PrimaryDiscriminator; uses `nixUnreachableWhenHardened` on default
  - per-type protected `getStorage`/`setStorage` overloads via `NIX_VALUE_STORAGE_DEF_PAIR_OF_PTRS` (for `SmallList[0/1]`, `PrimOpApplicationThunk.left/.right`, `FunctionApplicationThunk.left/.right`, `ClosureThunk.env/.expr`, `Lambda.env/.fun`) plus explicit `getStorage`/`setStorage` for `NixInt`, `bool`, `Null`, `NixFloat`, `ExternalValueBase *`, `PrimOp *`, `Bindings *`, `List`, `StringWithContext`, `Path`, `Failed *`
- `class ListView` — read-only view bridging `SmallList` and `List` via `std::variant`
  - private `using SpanType = std::span<Value * const>`, `using SmallList = detail::ValueBase::SmallList`, `using List = detail::ValueBase::List`
  - private `std::variant<SmallList, List> raw`
  - `ListView(SmallList)`, `ListView(List)`
  - `Value * const * data() const & noexcept`
  - `std::size_t size() const noexcept` — for SmallList, `back() == nullptr ? 1 : 2`
  - `Value * operator[](std::size_t) const noexcept`
  - `SpanType span() const &`
  - `SpanType span() && = delete`, `Value * const * data() && = delete`
  - inner `class iterator` — random-access iterator over `Value * const *`
    - `using value_type = Value *`, `pointer`, `reference`, `difference_type`, `iterator_category = std::random_access_iterator_tag`
    - private `pointer ptr = nullptr`, private explicit ctor
    - default ctor; full set of `++`/`--`/`+=`/`-=`/`+`/`-`/`<=>` operators, `operator*`, `operator->`, `operator[]`
  - `using const_iterator = iterator`
  - `iterator begin() const &`, `iterator end() const &`
  - `iterator begin() && = delete`, `iterator end() && = delete`
- `static_assert(std::random_access_iterator<ListView::iterator>)`
- `struct Value : public ValueStorage<sizeof(void *)>` — central evaluator value type
  - `friend std::string showType(const Value &)`
  - static well-known values (not singletons, comments warn pointer equality is _not_ sufficient): `Value vEmptyList`, `Value vNull`, `Value vTrue`, `Value vFalse`
  - private `template<InternalType... discriminator> bool isa() const noexcept`
  - private `template<typename T> T getStorage() const noexcept` — uses `nixUnreachableWhenHardened` on type mismatch
  - `static Value * toPtr(SymbolStr) noexcept` — exposes the embedded `Value` of an interned symbol
  - `void print(EvalState &, std::ostream &, PrintOptions = PrintOptions{})`
  - predicates: `inline bool isThunk() const`, `isApp()`, `isBlackhole()`, `isLambda()`, `isPrimOp()`, `isPrimOpApp()`, `isFailed()`, `isList()`, `isValid()`
  - `template<bool invalidIsThunk = false> inline ValueType type() const` — lookup-table mapping internal→user type; `tUninitialized` and out-of-range handled per template flag (returns `nThunk` if `invalidIsThunk`, else `nixUnreachableWhenHardened`)
  - mutators (`mk*`):
    - `inline void mkInt(NixInt::Inner) noexcept`, `inline void mkInt(NixInt) noexcept`
    - `inline void mkBool(bool) noexcept`
    - `void mkStringNoCopy(const StringData &, const Value::StringWithContext::Context * = nullptr) noexcept`
    - `void mkString(std::string_view, EvalMemory &)`
    - `void mkString(std::string_view, const NixStringContext &, EvalMemory &)`
    - `void mkStringMove(const StringData &, const NixStringContext &, EvalMemory &)`
    - `void mkPath(const SourcePath &, EvalMemory &)`
    - `inline void mkPath(SourceAccessor *, const StringData &) noexcept`
    - `inline void mkNull() noexcept`
    - `inline void mkAttrs(Bindings *) noexcept`
    - `Value & mkAttrs(BindingsBuilder &)` — calls `bindings.finish()` then `mkAttrs(Bindings *)`
    - `void mkList(const ListBuilder &) noexcept` — switch on size: 0 → empty `List`, 1 → SmallList of `[elem, nullptr]`, 2 → SmallList, default → `List{size, elems}`
    - `inline void mkThunk(Env *, Expr *) noexcept`
    - `inline void mkApp(Value *, Value *) noexcept`
    - `inline void mkLambda(Env *, ExprLambda *) noexcept`
    - `inline void mkBlackhole()` — uses global `eBlackHole`
    - `void mkPrimOp(PrimOp *)` — defined in eval.cc; runs `PrimOp::check`
    - `inline void mkPrimOpApp(Value *, Value *) noexcept`
    - `const PrimOp * primOpAppPrimOp() const` — defined in eval.cc; resolves chain
    - `inline void mkExternal(ExternalValueBase *) noexcept`
    - `inline void mkFloat(NixFloat) noexcept`
    - `inline void mkFailed(std::exception_ptr, Value * recovery) noexcept` — `new Value::Failed(...)`
  - list accessors: `ListView listView() const noexcept`, `size_t listSize() const noexcept`
  - `PosIdx determinePos(const PosIdx) const`
  - `bool isTrivial() const`
  - path accessors: `SourcePath path() const`, `const char * pathStr() const noexcept`, `std::string_view pathStrView() const noexcept`, `SourceAccessor * pathAccessor() const noexcept`
  - string accessors: `const StringData & string_data() const noexcept`, `const char * c_str() const noexcept`, `std::string_view string_view() const noexcept`, `const Value::StringWithContext::Context * context() const noexcept`
  - other accessors: `ExternalValueBase * external() const noexcept`, `const Bindings * attrs() const noexcept`, `const PrimOp * primOp() const noexcept`, `bool boolean() const noexcept`, `NixInt integer() const noexcept`, `NixFloat fpoint() const noexcept`, `Lambda lambda() const noexcept`, `ClosureThunk thunk() const noexcept`, `PrimOpApplicationThunk primOpApp() const noexcept`, `FunctionApplicationThunk app() const noexcept`, `Failed & failed() const noexcept`

### Type aliases
- `using NixInt = checked::Checked<int64_t>` — overflow-checked integer
- `using NixFloat = double`
- `typedef std::vector<Value *, traceable_allocator<Value *>> ValueVector`
- `typedef boost::unordered_flat_map<Symbol, Value *, std::hash<Symbol>, std::equal_to<Symbol>, traceable_allocator<std::pair<const Symbol, Value *>>> ValueMap`
- `typedef std::map<Symbol, ValueVector, std::less<Symbol>, traceable_allocator<std::pair<const Symbol, ValueVector>>> ValueVectorMap`
- `typedef std::shared_ptr<Value *> RootValue` — value held in traceable memory

### Functions
- `std::ostream & operator<<(std::ostream &, const ExternalValueBase &)` — declared
- `inline bool Value::isBlackhole() const` — `isThunk() && thunk().expr == (Expr *) &eBlackHole`
- `inline void Value::mkBlackhole()` — `mkThunk(nullptr, (Expr *) &eBlackHole)`
- `RootValue allocRootValue(Value *)` — declared
- `void forceNoNullByte(std::string_view, std::function<Pos()> = nullptr)` — declared

### Macros / globals
- `extern ExprBlackHole eBlackHole` — sentinel used for blackhole thunks
- `NIX_VALUE_STORAGE_FOR_EACH_FIELD(MACRO)` — table of (T, FIELD_NAME, DISCRIMINATOR) triples driving payload definitions, getters, setters, and internal-type mapping; entries: `NixInt integer tInt`, `bool boolean tBool`, `ValueBase::StringWithContext string tString`, `ValueBase::Path path tPath`, `ValueBase::Null null_ tNull`, `Bindings * attrs tAttrs`, `ValueBase::List bigList tListN`, `ValueBase::SmallList smallList tListSmall`, `ValueBase::ClosureThunk thunk tThunk`, `ValueBase::FunctionApplicationThunk app tApp`, `ValueBase::Lambda lambda tLambda`, `PrimOp * primOp tPrimOp`, `ValueBase::PrimOpApplicationThunk primOpApp tPrimOpApp`, `ExternalValueBase * external tExternal`, `ValueBase::Failed * failed tFailed`, `NixFloat fpoint tFloat`
- `NIX_VALUE_PAYLOAD_TYPE` (macro, undef'd after use) — specialises `PayloadTypeToInternalType<T>::value`
- `NIX_VALUE_STORAGE_DEFINE_FIELD` (used inside generic union, undef'd after use)
- `NIX_VALUE_STORAGE_GET_IMPL`, `NIX_VALUE_STORAGE_SET_IMPL` — generate generic getters/setters (undef'd after use)
- `NIX_VALUE_STORAGE_DEF_PAIR_OF_PTRS(TYPE, MEMBER_A, MEMBER_B)` — generates pair-of-pointers get/set in the bit-packed specialisation

## File: src/libexpr/value.cc

### Namespaces
- `nix`

### Functions
- `Value Value::vEmptyList = ...` — IIFE constructing a `List{.size = 0, .elems = nullptr}`
- `Value Value::vNull = ...` — IIFE setting `Null`
- `Value Value::vTrue` — IIFE setting `bool true`
- `Value Value::vFalse` — IIFE setting `bool false`
- `template<> bool ValueStorage<8>::isAtomic()` — runtime AVX detection via libcpuid; returns false when libcpuid is unavailable

### Macros / globals
- `#if HAVE_LIBCPUID` includes `<libcpuid/libcpuid.h>`

## File: src/libexpr/include/nix/expr/attr-set.hh

### Namespaces
- `nix`

### Classes / structs / enums
- `struct Attr` — single attribute entry
  - `Symbol name` — public; uint32 wrapper
  - `PosIdx pos` — public; uint32 wrapper, packed adjacent to `name` so `Attr` keeps a tight layout
  - `Value * value = nullptr` — public
  - `Attr(Symbol, Value *, PosIdx = noPos)`, default `Attr() {}`
  - `auto operator<=>(const Attr &) const` — compares by `name`
- `static_assert(sizeof(Attr) == 2 * sizeof(uint32_t) + sizeof(Value *), ...)` — performance contract: no padding on 64-bit
- `class Bindings` — attribute set with optional intrusive linked-list "layer" chain for cheap `//` updates
  - `using size_type = uint32_t`
  - `PosIdx pos` — public position
  - `static Bindings emptyBindings` — read-only zero-attr instance
  - private `size_type numAttrs = 0`, `size_type numAttrsInChain = 0`, `uint32_t numLayers = 1`, `const Bindings * baseLayer = nullptr`, `Attr attrs[0]` (FAM)
  - private default ctor + dtor (`= default`); copy/move ctors/assignments deleted
  - `friend class BindingsBuilder`, `friend class EvalMemory`
  - `static constexpr unsigned maxLayers = 8`
  - `size_type size() const` — returns `numAttrsInChain`
  - `bool empty() const`
  - `class iterator` — forward iterator implementing on-the-fly k-way merge across layers
    - `using value_type = Attr`, `pointer = const value_type *`, `reference = const value_type &`, `difference_type = std::ptrdiff_t`, `iterator_category = std::forward_iterator_tag`
    - `friend class Bindings`
    - inner private `struct BindingsCursor { pointer current; pointer end; uint32_t priority; pointer operator->() const noexcept; reference get() const noexcept; bool empty() const noexcept; void increment() noexcept; void consume(Symbol) noexcept; GENERATE_CMP(BindingsCursor, me->current->name, me->priority); }` — heap node
    - `using QueueStorageType = boost::container::static_vector<BindingsCursor, maxLayers>`
    - `static constexpr auto comp = std::greater<BindingsCursor>()`
    - private `QueueStorageType cursorHeap`, `pointer current = nullptr`, `bool doMerge = true`
    - private helpers `void push(BindingsCursor) noexcept`, `[[nodiscard]] BindingsCursor pop() noexcept`, `iterator & finished() noexcept`, `void next(BindingsCursor) noexcept`, `std::optional<BindingsCursor> consumeAllUntilCurrentName() noexcept`
    - private explicit `iterator(const Bindings &) noexcept` — pushes per-layer cursors and primes heap
    - public default `iterator() = default`
    - `reference operator*() const noexcept`, `pointer operator->() const noexcept`
    - `iterator & operator++() noexcept` — branches on `doMerge`
    - `iterator operator++(int) noexcept`
    - `bool operator==(const iterator &) const noexcept`
  - `using const_iterator = iterator`
  - `void push_back(const Attr &)` — append; sets `numAttrsInChain = numAttrs`
  - `const Attr * get(Symbol) const noexcept` — binary-search in each layer
  - `bool isLayerListFull() const noexcept`, `bool isLayered() const noexcept`
  - `const_iterator begin() const`, `const_iterator end() const`
  - `Attr & operator[](size_type)` and const overload — `unreachable()` when layered
  - `void sort()` — sorts the FAM in place (defined in attr-set.cc)
  - `std::vector<const Attr *> lexicographicOrder(const SymbolTable &) const`
- `static_assert(std::forward_iterator<Bindings::iterator>)`
- `static_assert(std::ranges::forward_range<Bindings>)`
- `class BindingsBuilder final` — wrapper that ensures the underlying Bindings is sorted at finish
  - `using value_type = Attr`, `using size_type = Bindings::size_type` (for `std::back_inserter`)
  - private `Bindings * bindings`, `Bindings::size_type capacity_`
  - `friend class EvalMemory`
  - private `BindingsBuilder(EvalMemory &, SymbolTable &, Bindings *, size_type)`
  - private `bool hasBaseLayer() const noexcept`
  - private `void finishSizeIfNecessary()` — recomputes `numAttrsInChain` after layering using `set_intersection` (when `attrs.size() > base.size()`) or per-elem `base.get(...)` (when smaller)
  - public `std::reference_wrapper<EvalMemory> mem`, `std::reference_wrapper<SymbolTable> symbols`
  - `void insert(Symbol, Value *, PosIdx = noPos)` — wraps `insert(Attr(...))`
  - `void insert(const Attr &)` — calls `push_back`
  - `void push_back(const Attr &)` — asserts `bindings->numAttrs < capacity_`
  - `void layerOnTopOf(const Bindings &) noexcept` — sets `baseLayer`, increments `numLayers`
  - `Value & alloc(Symbol, PosIdx = noPos)`
  - `Value & alloc(std::string_view, PosIdx = noPos)`
  - `Bindings * finish()` — `sort()` + `finishSizeIfNecessary()`
  - `Bindings * alreadySorted()` — skip sort; only calls `finishSizeIfNecessary`
  - `size_t capacity() const noexcept`
  - `void grow(BindingsBuilder newBindings)` — copy attrs into new builder, swap
  - `friend struct ExprAttrs`

## File: src/libexpr/attr-set.cc

### Namespaces
- `nix`

### Functions
- `Bindings * EvalMemory::allocBindings(size_t capacity)` — returns `&Bindings::emptyBindings` for zero capacity; throws `Error("attribute set of size %d is too big", capacity)` when capacity exceeds `size_type`; otherwise placement-new at `allocBytes(sizeof(Bindings) + sizeof(Attr)*capacity)`; bumps `nrAttrsets` and `nrAttrsInAttrsets`
- `Value & BindingsBuilder::alloc(Symbol, PosIdx)` — allocates fresh value via `mem.get().allocValue()` and `push_back`s an `Attr`
- `Value & BindingsBuilder::alloc(std::string_view, PosIdx)` — interns name via `symbols.get().create(...)` then delegates
- `void Bindings::sort()` — `std::sort(attrs, attrs + numAttrs)`
- `Value & Value::mkAttrs(BindingsBuilder & bindings)` — calls `mkAttrs(bindings.finish())` then returns `*this`

### Macros / globals
- `Bindings Bindings::emptyBindings` — empty singleton

## File: src/libexpr/include/nix/expr/eval-inline.hh

### Namespaces
- `nix`

### Functions
- `[[gnu::always_inline]] inline void * EvalMemory::allocBytes(size_t)` — `GC_MALLOC` (or `calloc(n, 1)` zero-init when GC disabled); throws `std::bad_alloc` on failure
- `[[gnu::always_inline]] Value * EvalMemory::allocValue()` — uses Boehm `GC_malloc_many` thread-local cache for batch Value allocations; clears next-pointer; bumps `stats.nrValues`
- `[[gnu::always_inline]] Env & EvalMemory::allocEnv(size_t size)` — special-cases `size == 1` with thread-local `GC_malloc_many` cache; otherwise generic `allocBytes(sizeof(Env) + size * sizeof(Value *))`; bumps `nrEnvs`/`nrValuesInEnvs`
- `[[gnu::always_inline]] void EvalState::forceValue(Value &, const PosIdx)` — handles Thunk (sets to blackhole, then `expr->eval`; if env is nullptr → `ExprBlackHole::throwInfiniteRecursionError`), App (saves the App for recovery, calls `callFunction`), Failed (delegates to `handleEvalFailed`); on exception calls `handleEvalExceptionForThunk`/`...App` and rethrows
- `[[gnu::always_inline]] inline void EvalState::forceAttrs(Value &, const PosIdx, std::string_view)` — wraps templated overload via lambda returning the pos
- `template<typename Callable> [[gnu::always_inline]] inline void EvalState::forceAttrs(Value &, Callable getPos, std::string_view)` — forces value, type-checks `nAttrs`; throws `TypeError` with `withTrace` and `debugThrow`
- `[[gnu::always_inline]] inline void EvalState::forceList(Value &, const PosIdx, std::string_view)` — forces value, type-checks `isList()`; throws `TypeError` similarly
- `[[gnu::always_inline]] inline CallDepth EvalState::addCallDepth(const PosIdx)` — depth check vs `settings.maxCallDepth`, throws `StackOverflowError`; returns RAII `CallDepth(callDepth)`

## File: src/libexpr/include/nix/expr/eval.hh

### Namespaces
- `nix`
- `nix::fetchers` (forward decls of `Settings`, `InputCache`, `Input`)
- `nix::eval_cache` (forward decl of `EvalCache`)

### Classes / structs / enums
- `class CallDepth` — RAII counter; ctor takes `size_t & count` reference, increments on construction, decrements on destruction
- `using PrimOpFun = void(EvalState &, const PosIdx, Value ** args, Value & v)` — primop signature
- `struct PrimOp` — primop descriptor
  - `std::string name` (with special `__` prefix handling)
  - `std::vector<std::string> args` — declared parameters
  - `size_t arity = 0` — derived from `args` if non-empty
  - `std::optional<std::string> doc`
  - `bool addTrace = true`
  - `fun<PrimOpFun> impl`
  - `std::optional<ExperimentalFeature> experimentalFeature`
  - `bool internal = false` — hidden from user
  - `void check()` — validity check called from registration
- `struct Constant` — builtin constant descriptor
  - `ValueType type = nThunk` (TODO: dedicated enum)
  - `const char * doc = nullptr`
  - `bool impureOnly = false`
- `struct Env` — runtime env: `Env * up; Value * values[0]` (FAM)
- `struct DebugTrace` — debugger frame
  - `std::variant<Pos, PosIdx> pos` — optimisation: avoid `PosTable::operator[]` until needed
  - `const Expr & expr`
  - `const Env & env`
  - `HintFmt hint`
  - `bool isError`
  - `Pos getPos(const PosTable &) const` — resolves PosIdx (falling back to `expr.getPos()` when unset)
- `struct StaticEvalSymbols` — compile-time-resolved well-known attribute names
  - 44 named `Symbol` fields: `with, outPath, drvPath, type, meta, name, value, system, overrides, outputs, outputName, ignoreNulls, file, line, column, functor, toString, right, wrong, structuredAttrs, json, allowedReferences, allowedRequisites, disallowedReferences, disallowedRequisites, maxSize, maxClosureSize, builder, args, contentAddressed, impure, outputHash, outputHashAlgo, outputHashMode, recurseForDerivations, description, self, epsilon, startSet, operator_, key, path, prefix, outputSpecified`
  - `Expr::AstSymbols exprSymbols` — additional symbols `sub, lessThan, mul, div, or_, findFile, nixPath, body`
  - `static constexpr auto preallocate()` — fills a `StaticSymbolTable` and returns `pair<StaticEvalSymbols, StaticSymbolTable>`
  - `static consteval StaticEvalSymbols create()` — returns `preallocate().first`
  - `static constexpr StaticSymbolTable staticSymbolTable()` — returns `preallocate().second`
- `class EvalMemory` — owns the value/env/binding allocators and AST storage
  - `struct Statistics { Counter nrEnvs, nrValuesInEnvs, nrValues, nrAttrsets, nrAttrsInAttrsets, nrListElems; }` — public
  - `EvalMemory()` — asserts GC initialised
  - copy/move ctor/assignment all deleted
  - `inline void * allocBytes(size_t)` (defined in eval-inline.hh)
  - `inline Value * allocValue()` (defined in eval-inline.hh)
  - `inline Env & allocEnv(size_t)` (defined in eval-inline.hh)
  - `Bindings * allocBindings(size_t)` (defined in attr-set.cc)
  - `BindingsBuilder buildBindings(SymbolTable &, size_t)` — inline
  - `ListBuilder buildList(size_t)` — inline; bumps `nrListElems`
  - `const Statistics & getStats() const &`
  - `Exprs exprs` — public; AST node storage
  - private `Statistics stats`
- `class EvalState : public std::enable_shared_from_this<EvalState>` — top-level evaluator
  - `static constexpr StaticEvalSymbols s = StaticEvalSymbols::create()`
  - `const fetchers::Settings & fetchSettings`
  - `const EvalSettings & settings`
  - `SymbolTable symbols`
  - `PosTable positions`
  - `EvalMemory mem`
  - `RepairFlag repair`
  - `const ref<MountedSourceAccessor> storeFS`
  - `const ref<SourceAccessor> rootFS`
  - `const ref<MemorySourceAccessor> corepkgsFS`
  - `const ref<MemorySourceAccessor> internalFS`
  - `const SourcePath derivationInternal`
  - `const SourcePath importedDrvToDerivation`
  - `const ref<Store> store`
  - `const ref<Store> buildStore`
  - `const ref<fetchers::InputCache> inputCache`
  - debug state: `ReplExitStatus (*debugRepl)(ref<EvalState>, const ValMap & extraEnv)`, `bool debugStop`, `bool inDebugger = false`, `int trylevel`, `std::list<DebugTrace> debugTraces`, `boost::unordered_flat_map<const Expr *, const std::shared_ptr<const StaticEnv>> exprEnvs`
  - `const std::shared_ptr<const StaticEnv> getStaticEnv(const Expr &) const` — returns null if not present
  - `bool canDebug()` — debugger availability
  - `void runDebugRepl(const Error *)` — uses front of `debugTraces`
  - `void runDebugRepl(const Error *, const Env &, const Expr &)` — explicit env+expr
  - `template<class T, typename... Args> [[nodiscard, gnu::noinline]] EvalErrorBuilder<T> & error(const Args &...)` — heap-allocates builder; freed by `debugThrow`
  - `std::map<const Hash, ref<eval_cache::EvalCache>> evalCaches` — cache reuse
  - private `const ref<boost::concurrent_flat_map<SourcePath, StorePath>> srcToStore`
  - private `const ref<boost::concurrent_flat_map<SourcePath, SourcePath>> importResolutionCache`
  - private `const ref<boost::concurrent_flat_map<SourcePath, Value *, std::hash<SourcePath>, std::equal_to<SourcePath>, traceable_allocator<...>>> fileEvalCache`
  - private `const ref<boost::concurrent_flat_map<SourcePath, ref<DocCommentMap>>> positionToDocComment`
  - private `LookupPath lookupPath`
  - private `struct LookupPathResolvedState { SourcePath path; const ref<boost::concurrent_flat_map<CanonPath, std::optional<SourcePath>>> resolvedPaths; }`
  - private `const ref<boost::concurrent_flat_map<std::string, std::shared_ptr<LookupPathResolvedState>, StringViewHash, std::equal_to<>>> lookupPathResolved`
  - private `const ref<RegexCache> regexCache`
  - constructor `EvalState(const LookupPath &, ref<Store>, const fetchers::Settings &, const EvalSettings &, std::shared_ptr<Store> buildStore = nullptr)` and dtor
  - `inline Value * allocValue()` — delegates to `mem.allocValue()`
  - `LookupPath getLookupPath()`
  - path mounting: `SourcePath rootPath(CanonPath)`, `SourcePath rootPath(std::string_view)`, `SourcePath storePath(const StorePath &)`
  - access control: `void allowPathLegacy(const std::string &)`, `void allowPath(const StorePath &)`, `void allowClosure(const StorePath &)`, `void allowAndSetStorePathString(const StorePath &, Value &)`, `void checkURI(const std::string &)`
  - `StorePath mountInput(fetchers::Input &, const fetchers::Input & originalInput, ref<SourceAccessor>)`
  - parser entry points: `Expr * parseExprFromFile(const SourcePath &)` and overload `(const SourcePath &, const std::shared_ptr<StaticEnv> &)`
  - `Expr * parseExprFromString(std::string, const SourcePath &, const std::shared_ptr<StaticEnv> &)` and overload `(std::string, const SourcePath &)`
  - `ExprAttrs * parseReplBindings(std::string, const SourcePath &, const std::shared_ptr<StaticEnv> &)` and overload `(std::string, std::string errorSource, const SourcePath &, const std::shared_ptr<StaticEnv> &)`
  - `Expr * parseStdin()`
  - `void evalFile(const SourcePath &, Value &, bool mustBeTrivial = false)`
  - `void resetFileCache()`
  - search-path lookup: `SourcePath findFile(const std::string_view)`, `SourcePath findFile(const LookupPath &, const std::string_view, const PosIdx = noPos)`, `std::shared_ptr<LookupPathResolvedState> resolveLookupPathPath(const LookupPath::Path &, bool initAccessControl = false)`
  - core evaluation: `void eval(Expr *, Value &)`
  - typed evals: `inline bool evalBool(Env &, Expr *)` (declared, but no definition found in eval.cc); `inline bool evalBool(Env &, Expr *, const PosIdx, std::string_view)`; `inline void evalAttrs(Env &, Expr *, Value &, const PosIdx, std::string_view)`
  - `inline void forceValue(Value &, const PosIdx)` (defined in eval-inline.hh)
  - private support: `void handleEvalExceptionForThunk(Env *, Expr *, Value &, const PosIdx)`, `void handleEvalExceptionForApp(Value &, const Value & savedApp)`, `void handleEvalFailed(Value &, PosIdx)`, `void tryFixupBlackHolePos(Value &, PosIdx)`
  - `void forceValueDeep(Value &)` — recursive force
  - typed forcers: `NixInt forceInt(Value &, const PosIdx, std::string_view)`, `NixFloat forceFloat(...)`, `bool forceBool(...)`
  - `void forceAttrs(Value &, const PosIdx, std::string_view)` (and templated `Callable getPos` overload, both inline in eval-inline.hh)
  - `inline void forceList(Value &, const PosIdx, std::string_view)`
  - `void forceFunction(Value &, const PosIdx, std::string_view)`
  - string forcers: `std::string_view forceString(Value &, const PosIdx, std::string_view)`, overload taking `NixStringContext &` and `const ExperimentalFeatureSettings & = experimentalFeatureSettings`, `std::string_view forceStringNoCtx(...)`
  - attr lookup: `const Attr * getAttr(Symbol, const Bindings *, std::string_view errorCtx)`
  - error tracing: `template<typename... Args> [[gnu::noinline]] void addErrorTrace(Error &, const Args &...) const` and overload with `const PosIdx`
  - `bool isDerivation(Value &)` — checks `type = "derivation"`
  - `std::optional<std::string> tryAttrsToString(const PosIdx, Value &, NixStringContext &, bool coerceMore = false, bool copyToStore = true)`
  - `enum class CopyLazyPaths : bool { PreserveLazy = false, Copy = true }`
  - `void ensureLazyPathCopied(const StorePath &)`, `void ensureLazyPathsCopied(const NixStringContext &)`
  - coercion: `BackedStringView coerceToString(const PosIdx, Value &, NixStringContext &, std::string_view, bool coerceMore = false, bool copyToStore = true, bool canonicalizePath = true)`
  - `StorePath copyPathToStore(NixStringContext &, const SourcePath &)`
  - `SourcePath coerceToPath(const PosIdx, Value &, NixStringContext &, std::string_view)`
  - `StorePath coerceToStorePath(const PosIdx, Value &, NixStringContext &, std::string_view)`
  - `std::pair<SingleDerivedPath, std::string_view> coerceToSingleDerivedPathUnchecked(const PosIdx, Value &, std::string_view, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`
  - `SingleDerivedPath coerceToSingleDerivedPath(const PosIdx, Value &, std::string_view)`
  - `#if NIX_USE_BOEHMGC const std::shared_ptr<Env *> baseEnvP` — GC root for baseEnv
  - `Env & baseEnv` — runtime base env
  - `const std::shared_ptr<StaticEnv> staticBaseEnv` — parser-time base env (comment "should be private")
  - `boost::unordered_flat_map<std::string, Value *, StringViewHash, std::equal_to<>, traceable_allocator<std::pair<const std::string, Value *>>> internalPrimOps` — hidden builtins
  - `std::vector<std::pair<std::string, Constant>> constantInfos`
  - private base-env helpers: `unsigned int baseEnvDispl = 0`, `void createBaseEnv(const EvalSettings &)`, `Value * addConstant(const std::string &, Value &, Constant)`, `void addConstant(const std::string &, Value *, Constant)`, `Value * addPrimOp(PrimOp &&)`
  - public builtins access: `Value & getBuiltin(const std::string &)`, `Value & getBuiltins()`
  - inner `struct Doc { Pos pos; std::optional<std::string> name; size_t arity; std::vector<std::string> args; const char * doc; }` (doc never null)
  - `std::optional<Doc> getDoc(Value &)` — resolves docs for a value
  - private `inline Value * lookupVar(Env *, const ExprVar &, bool noEval)` — `[[gnu::always_inline]]`
  - friends: `ExprVar`, `ExprAttrs`, `ExprLet`
  - parser internals: `Expr * parse(char *, size_t, Pos::Origin, const SourcePath &, const std::shared_ptr<StaticEnv> &)`, `ExprAttrs * parseReplBindings(char *, size_t, Pos::Origin, const SourcePath &, const std::shared_ptr<StaticEnv> &)` (private overloads)
  - private `size_t callDepth = 0`
  - public `inline CallDepth addCallDepth(const PosIdx)` (defined inline)
  - `bool eqValues(Value &, Value &, const PosIdx, std::string_view)` — deep equality
  - `void assertEqValues(Value &, Value &, const PosIdx, std::string_view)` — deep equality, throws `AssertionError` on mismatch (with WARNING comment about staying in sync with `eqValues`)
  - `bool isFunctor(const Value &) const`
  - `void callFunction(Value & fun, std::span<Value *> args, Value & vRes, const PosIdx)`
  - `void callFunction(Value & fun, Value & arg, Value & vRes, const PosIdx)` — single-arg overload, wraps array of one
  - `void autoCallFunction(const Bindings & args, Value & fun, Value & res)` — fills defaults
  - convenience builders: `BindingsBuilder buildBindings(size_t)`, `ListBuilder buildList(size_t)`
  - `Value * getBool(bool)` — pre-allocated true/false
  - mkers: `void mkThunk_(Value &, Expr *)`, `void mkPos(Value &, PosIdx)`, `void mkStorePathString(const StorePath &, Value &)`, `void mkOutputString(Value &, const SingleDerivedPath::Built &, std::optional<StorePath>, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`, `void mkSingleDerivedPathString(const SingleDerivedPath &, Value &)`
  - `void concatLists(Value &, std::span<Value * const>, const PosIdx, std::string_view)` — n-ary `++`
  - stats/GC: `void maybePrintStats()`, `void printStatistics()`, `bool fullGC()`
  - `[[nodiscard]] StringMap realiseContext(const NixStringContext &, StorePathSet * maybePaths = nullptr, bool isIFD = true)`
  - `SourcePath realisePath(const PosIdx, Value &, std::optional<SymlinkResolution> resolveSymlinks = SymlinkResolution::Full, CopyLazyPaths copyLazyPaths = CopyLazyPaths::PreserveLazy)`
  - `std::string realiseString(Value & str, StorePathSet * storePathsOutMaybe, bool isIFD = true, const PosIdx = noPos)`
  - `bool callPathFilter(Value * filterFun, const SourcePath &, PosIdx)`
  - `DocComment getDocCommentForPos(PosIdx)`
  - private raw-string helpers: `std::string mkOutputStringRaw(const SingleDerivedPath::Built &, std::optional<StorePath>, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`, `std::string mkSingleDerivedPathStringRaw(const SingleDerivedPath &)`
  - private counters: `Counter nrLookups`, `nrAvoided`, `nrOpUpdates`, `nrOpUpdateValuesCopied`, `nrListConcats`, `nrPrimOpCalls`, `nrFunctionCalls`
  - private `bool countCalls`
  - private `typedef boost::unordered_flat_map<std::string, size_t, StringViewHash, std::equal_to<>> PrimOpCalls; PrimOpCalls primOpCalls;`
  - private `typedef boost::unordered_flat_map<ExprLambda *, size_t> FunctionCalls; FunctionCalls functionCalls;`
  - private `MultiEvalProfiler profiler`
  - private `void incrFunctionCall(ExprLambda *)`
  - private `typedef boost::unordered_flat_map<PosIdx, size_t, std::hash<PosIdx>> AttrSelects; AttrSelects attrSelects;`
  - friend declarations: `ExprOpUpdate`, `ExprOpConcatLists`, `ExprVar`, `ExprString`, `ExprInt`, `ExprFloat`, `ExprPath`, `ExprSelect`, `prim_getAttr`, `prim_match`, `prim_split`, `Value`, `ListBuilder`
- `struct DebugTraceStacker` — RAII; ctor pushes `DebugTrace` onto front of `evalState.debugTraces`; dtor pops front
  - `EvalState & evalState`, `DebugTrace trace` — public
  - `DebugTraceStacker(EvalState &, DebugTrace)`
  - `~DebugTraceStacker()`

### Type aliases
- `typedef std::map<std::string, Value *, std::less<std::string>, traceable_allocator<std::pair<const std::string, Value *>>> ValMap`
- `typedef boost::unordered_flat_map<PosIdx, DocComment, std::hash<PosIdx>> DocCommentMap`

### Functions
- `std::ostream & operator<<(std::ostream &, const PrimOp &)` — declared
- `void printEnvBindings(const EvalState &, const Expr &, const Env &)`
- `void printEnvBindings(const SymbolTable &, const StaticEnv &, const Env &, int lvl = 0)`
- `std::unique_ptr<ValMap> mapStaticEnvBindings(const SymbolTable &, const StaticEnv &, const Env &)`
- `void copyContext(const Value &, NixStringContext &, const ExperimentalFeatureSettings & = experimentalFeatureSettings)`
- `std::string printValue(EvalState &, Value &)`
- `std::ostream & operator<<(std::ostream &, const ValueType)`
- `ref<RegexCache> makeRegexCache()`
- `std::string_view showType(ValueType, bool withArticle = true)`
- `std::string showType(const Value &)`
- `SourcePath resolveExprPath(SourcePath, bool addDefaultNix = true)` — appends `/default.nix` for directories
- `bool isAllowedURI(std::string_view, const Strings & allowedPaths)`

### Macros / globals
- `constexpr size_t maxPrimOpArity = 8` — maximum primop arity
- `struct RegexCache` — forward declared

## File: src/libexpr/eval.cc

### Namespaces
- `nix`

### Classes / structs / enums
- `class DebuggerGuard` — RAII flag toggle setting `inDebugger = true` on construction, false on destruction; non-copyable / non-movable
- `struct ExprParseFile : Expr, gc` — synthetic Expr that lazily parses + evaluates a source file inside a thunk
  - `SourcePath path`
  - `bool mustBeTrivial`
  - `ExprParseFile(SourcePath &, bool)`
  - `void eval(EvalState &, Env &, Value &) override` — parse and eval, optionally enforcing top-level attrset, with debug trace stacker

### Functions
- `static char * allocString(size_t)` — atomic Boehm allocation (`GC_MALLOC_ATOMIC`) for doc strings
- `static const char * makeImmutableString(std::string_view)` — copy or empty-string short-circuit
- `StringData & StringData::alloc(EvalMemory &, size_t)` — placement-new with FAM payload
- `const StringData & StringData::make(EvalMemory &, std::string_view)` — copies + NUL-terminates; returns `""_sds` for empty
- `RootValue allocRootValue(Value *)` — `allocate_shared<Value *>` with traceable allocator
- `std::ostream & operator<<(std::ostream &, const ValueType)` — delegates to `showType`
- `std::string printValue(EvalState &, Value &)` — wraps `Value::print` into `ostringstream`
- `Value * Value::toPtr(SymbolStr)` — `const_cast<Value *>(str.valuePtr())`
- `void Value::print(EvalState &, std::ostream &, PrintOptions)` — calls free `printValue(state, str, *this, options)`
- `std::string_view showType(ValueType, bool withArticle)` — switch with `WA` macro; `unreachable` on default
- `std::string showType(const Value &)` — branches on internal type (string-with-context, primops, externals, blackhole, app), defaults to `showType(v.type())`
- `PosIdx Value::determinePos(const PosIdx) const` — cascades through `tAttrs` (returns `attrs()->pos`), `tLambda` (`lambda().fun->pos`), `tApp` (`app().left->determinePos(pos)`)
- `bool Value::isTrivial() const` — false for App/PrimOpApp; for thunks accepts `ExprAttrs` with empty `dynamicAttrs`, `ExprLambda`, `ExprList`
- `static Symbol getName(const AttrName &, EvalState &, Env &)` — for dynamic attr names, evaluates expression and forces `forceStringNoCtx`
- `EvalMemory::EvalMemory()` — asserts GC initialised
- `EvalState::EvalState(...)` — initialises `storeFS` with `MountedSourceAccessor` (using `getFSAccessor(settings.pureEval)`); `rootFS` with optional `AllowListSourceAccessor`+caching union; internal/corepkgsFS; registers `derivation-internal.nix`, `imported-drv-to-derivation.nix`, `fetchurl.nix` source files; allocates `baseEnv` of size `BASE_ENV_SIZE` (with `baseEnvP` GC root if Boehm); bumps stack to 60 MiB once via `std::call_once`; sets `corepkgsFS->setPathDisplay("<nix", ">")`, `internalFS->setPathDisplay("«nix-internal»", "")`; reads `NIX_COUNT_CALLS`; `static_assert(sizeof(Env) <= 16)`; parses lookup path entries (excluding pure-eval), creates base env, registers `FunctionCallTrace` and flamegraph profiler if enabled
- `EvalState::~EvalState()` — defaulted body
- `void EvalState::allowPathLegacy(const std::string &)` — calls `rootFS->allowPrefix` if `AllowListSourceAccessor`
- `void EvalState::allowPath(const StorePath &)`
- `void EvalState::allowClosure(const StorePath &)` — computes FS closure, allows each
- `void EvalState::allowAndSetStorePathString(const StorePath &, Value &)`
- `static inline bool isJustSchemePrefix(std::string_view)` — `https:` style scheme detector
- `bool isAllowedURI(std::string_view, const Strings &)` — prefix matching with subdir/scheme allowance
- `void EvalState::checkURI(const std::string &)` — pure/restricted URI gating with file-scheme handling and `RestrictedPathError` on miss
- `Value * EvalState::addConstant(const std::string &, Value &, Constant)` — heap-copies value, then calls overload
- `void EvalState::addConstant(const std::string &, Value *, Constant)` — strips `__` prefix into `name2`; pushes `(name2, info)` into `constantInfos`; if `!(pureEval && info.impureOnly)` then checks `v->type<invalidIsThunk=true>()` against `info.type`, installs `(symbols.create(name), baseEnvDispl)` into `staticBaseEnv->vars`, stores `v` at `baseEnv.values[baseEnvDispl++]`, and pushes `Attr(symbols.create(name2), v)` into `getBuiltins().attrs()`
- `void PrimOp::check()` — validates `arity <= maxPrimOpArity`
- `std::ostream & operator<<(std::ostream &, const PrimOp &)` — `"primop <name>"`
- `const PrimOp * Value::primOpAppPrimOp() const` — walks left chain of `primOpApp`, returns underlying primop
- `void Value::mkPrimOp(PrimOp *)` — runs `check`, then `setStorage(p)`
- `Value * EvalState::addPrimOp(PrimOp &&)` — special-cases `arity == 0` by re-routing through `addConstant` with `Value` set to `App(vPrimOp, vPrimOp)` (lazy constant trick, primop applied to itself, with `Constant{nThunk, doc}`); otherwise creates `envName = symbols.create(name)` *before* stripping `__` prefix from `primOp.name`; installs primop value in `internalPrimOps` (when `internal`) or in `staticBaseEnv->vars[envName] = baseEnvDispl++`, `baseEnv.values[displ] = v`, and pushes `Attr(symbols.create(stripped name), v)` into `getBuiltins().attrs()`
- `Value & EvalState::getBuiltins()` — returns `*baseEnv.values[0]`
- `Value & EvalState::getBuiltin(const std::string &)` — fetch by symbol; throws `EvalError` on miss
- `std::optional<EvalState::Doc> EvalState::getDoc(Value &)` — primOp doc; lambda + position + docComment fallback (uses `makeImmutableString` for assembled doc); functor partial application (catches errors with trace)
- `void printStaticEnvBindings(const SymbolTable &, const StaticEnv &)` — single-level dump
- `void printWithBindings(const SymbolTable &, const Env &)` — single-level "with" dump
- `void printEnvBindings(const SymbolTable &, const StaticEnv &, const Env &, int lvl)` — recurse from top, suppressing `__` builtins at top level
- `void printEnvBindings(const EvalState &, const Expr &, const Env &)` — entry point using `getStaticEnv`
- `void mapStaticEnvBindings(const SymbolTable &, const StaticEnv &, const Env &, ValMap &)` — recursive (private 4-arg form)
- `std::unique_ptr<ValMap> mapStaticEnvBindings(const SymbolTable &, const StaticEnv &, const Env &)` — wrapper
- `bool EvalState::canDebug()` — returns `debugRepl && !debugTraces.empty()`
- `void EvalState::runDebugRepl(const Error *)` — uses front of debugTraces
- `void EvalState::runDebugRepl(const Error *, const Env &, const Expr &)` — pushes synthetic `DebugTraceStacker` for the error; calls `debugRepl` and handles `ReplExitStatus::QuitAll`/`Continue`
- `template<typename... Args> void EvalState::addErrorTrace(Error &, const Args &...) const` — `e.addTrace(nullptr, HintFmt(...))`
- `template<typename... Args> void EvalState::addErrorTrace(Error &, const PosIdx, const Args &...) const` — uses `positions[pos]`
- `template<typename... Args> static std::unique_ptr<DebugTraceStacker> makeDebugTraceStacker(EvalState &, Expr &, Env &, std::variant<Pos, PosIdx>, const Args &...)` — convenience factory; isError = false
- `DebugTraceStacker::DebugTraceStacker(EvalState &, DebugTrace)` — pushes front, optionally enters debug REPL when `debugStop` is true
- `DebugTraceStacker::~DebugTraceStacker()` — pops front (defined inline in header)
- `void Value::mkString(std::string_view, EvalMemory &)` — allocates via `StringData::make`
- `Value::StringWithContext::Context * Value::StringWithContext::Context::fromBuilder(const NixStringContext &, EvalMemory &)` — returns null on empty; else placement-new of `Context` with array, populated via `std::ranges::transform` calling `StringData::make(mem, elt.to_string())`
- `void Value::mkString(std::string_view, const NixStringContext &, EvalMemory &)`
- `void Value::mkStringMove(const StringData &, const NixStringContext &, EvalMemory &)`
- `void Value::mkPath(const SourcePath &, EvalMemory &)` — `mkPath(&*path.accessor, StringData::make(mem, path.path.abs()))`
- `[[gnu::always_inline]] inline Value * EvalState::lookupVar(Env *, const ExprVar &, bool noEval)` — climbs levels; for `with`-introduced names repeatedly forces the with's attrs and walks parent withs; throws `UndefinedVarError` on miss
- `ListBuilder::ListBuilder(EvalMemory &, size_t)` — picks inline storage if `size <= 2`, else heap via `mem.allocBytes`
- `Value * EvalState::getBool(bool)` — returns pre-allocated `vTrue` / `vFalse`
- `static inline void mkThunk(Value &, Env &, Expr *)` — bumps file-local `nrThunks` counter
- `void EvalState::mkThunk_(Value &, Expr *)` — wraps `mkThunk` with `baseEnv`
- `void EvalState::mkPos(Value &, PosIdx)` — produces `{file, line, column}` attrs (or `mkNull()` if non-`SourcePath` origin); uses `makePositionThunks`
- `void EvalState::mkStorePathString(const StorePath &, Value &)` — string with single `Opaque` context element
- `std::string EvalState::mkOutputStringRaw(const SingleDerivedPath::Built &, std::optional<StorePath>, const ExperimentalFeatureSettings &)` — picks static path or `DownstreamPlaceholder::fromSingleDerivedPathBuilt(...).render()`
- `void EvalState::mkOutputString(Value &, const SingleDerivedPath::Built &, std::optional<StorePath>, const ExperimentalFeatureSettings &)` — sets value with `Built` context
- `std::string EvalState::mkSingleDerivedPathStringRaw(const SingleDerivedPath &)` — visitor over `Opaque`/`Built` (Built reads derivation outputs to compute static output path)
- `void EvalState::mkSingleDerivedPathString(const SingleDerivedPath &, Value &)`
- `Value * Expr::maybeThunk(EvalState &, Env &)` — wraps Expr in thunk; default
- specialised `Expr::maybeThunk` overrides on `ExprVar` (calls `lookupVar(..., true)` and bumps `nrAvoided`), `ExprString`, `ExprInt`, `ExprFloat`, `ExprPath` (each returns pre-existing value)
- `void EvalState::evalFile(const SourcePath &, Value &, bool mustBeTrivial)` — caches resolved path in `importResolutionCache` and thunk in `fileEvalCache`; uses `ExprParseFile` (currently heap-allocated; FIXME comment about future stack allocation)
- `void EvalState::resetFileCache()` — clears resolution/file/input/lookup-path caches; clears `positions`; invalidates rootFS cache
- `void EvalState::eval(Expr *, Value &)` — `e->eval(*this, baseEnv, v)`
- `inline bool EvalState::evalBool(Env &, Expr *, const PosIdx, std::string_view)` — eval to bool; throws `TypeError` with frame trace and adds errorCtx trace on rethrow
- `inline void EvalState::evalAttrs(Env &, Expr *, Value &, const PosIdx, std::string_view)`
- per-Expr `eval` definitions: `Expr::eval` (`unreachable`), `ExprInt::eval`, `ExprFloat::eval`, `ExprString::eval`, `ExprPath::eval`, `ExprAttrs::buildInheritFromEnv`, `ExprAttrs::eval`, `ExprLet::eval`, `ExprList::eval`, `ExprList::maybeThunk` (returns `&Value::vEmptyList` if elems empty), `ExprVar::eval`, `ExprSelect::eval`, `ExprSelect::evalExceptFinalSelect`, `ExprOpHasAttr::eval`, `ExprLambda::eval`, `EvalState::callFunction`, `ExprCall::eval`, `EvalState::incrFunctionCall`, `EvalState::autoCallFunction`, `ExprWith::eval`, `ExprIf::eval`, `ExprAssert::eval` (special-cases `ExprOpEq` to call `assertEqValues`), `ExprOpNot::eval`, `ExprOpEq::eval`, `ExprOpNEq::eval`, `ExprOpAnd::eval`, `ExprOpOr::eval`, `ExprOpImpl::eval`, `ExprOpUpdate::eval(EvalState &, Value &, Value &, Value &)` (helper performing the merge with `shouldLayer` heuristic over `bindingsUpdateLayerRhsSizeThreshold` + linked-list cap), `ExprOpUpdate::eval(EvalState &, Env &, Value &)` (entry calling `evalForUpdate` then folding right-to-left), `Expr::evalForUpdate(EvalState &, Env &, UpdateQueue &, std::string_view)`, `ExprOpUpdate::evalForUpdate(EvalState &, Env &, UpdateQueue &)`, `ExprOpUpdate::evalForUpdate(EvalState &, Env &, UpdateQueue &, std::string_view)` overload, `ExprOpConcatLists::eval`, `ExprConcatStrings::eval`, `ExprPos::eval`, `ExprBlackHole::eval`, `[[gnu::noinline]] [[noreturn]] ExprBlackHole::throwInfiniteRecursionError`
- `static std::string showAttrSelectionPath(EvalState &, Env &, std::span<const AttrName>)` — pretty-print attr-selection path with `${expr}` fallback when symbol is dynamic
- `void EvalState::callFunction(Value &, std::span<Value *>, Value &, const PosIdx)` — main lambda/primop/primopApp/functor dispatch loop; integrates profiler hooks (pre/post via `Finally`); for lambdas matches formals with suggestion-rich `TypeError`; for primops collects args into `vArgs[maxPrimOpArity]` and dispatches; for functors heap-copies `vCur` and recurses; uses `addCallDepth` for guard
- `void ExprCall::eval(EvalState &, Env &, Value &)` — wraps call with debug trace stacker; uses `SmallValueVector<4>` for args (chosen empirically)
- `void EvalState::incrFunctionCall(ExprLambda *)` — `functionCalls[fun]++` (extracted to enable TCO of caller)
- `void EvalState::autoCallFunction(const Bindings &, Value &, Value &)` — recurses through functors; for lambdas with formals, builds attrs from defaults and `args`; when no ellipsis, only passes accepted formal names; emits `MissingArgumentError` for required-but-absent
- `void EvalState::concatLists(Value &, std::span<Value * const>, const PosIdx, std::string_view)` — `nrListConcats++`; fast path for single non-empty source; otherwise `memcpy` into new list
- `[[gnu::noinline]] void EvalState::handleEvalExceptionForThunk(Env *, Expr *, Value &, const PosIdx)` — captures `current_exception`; for `RecoverableEvalError` allocates recovery thunk; sets value to `tFailed`; calls `tryFixupBlackHolePos` when env is null
- `[[gnu::noinline]] void EvalState::handleEvalExceptionForApp(Value &, const Value & savedApp)` — same but recovery is the saved App
- `[[gnu::noinline]] void EvalState::handleEvalFailed(Value &, const PosIdx)` — replays `recoveryValue` (re-forces) or rethrows
- `void EvalState::tryFixupBlackHolePos(Value &, PosIdx)` — annotates `InfiniteRecursionError` with position when missing
- `void EvalState::forceValueDeep(Value &)` — recursive force using deducing-this lambda; tracks seen values; force attrs/lists with debug-trace stacker and call-depth guard
- `NixInt EvalState::forceInt(Value &, const PosIdx, std::string_view)`
- `NixFloat EvalState::forceFloat(...)` — accepts `nInt` (returning float) or `nFloat`
- `bool EvalState::forceBool(...)`
- `const Attr * EvalState::getAttr(Symbol, const Bindings *, std::string_view)` — throws `TypeError` ("attribute '%s' missing") on miss
- `bool EvalState::isFunctor(const Value &) const` — checks `nAttrs` with `__functor` attr
- `void EvalState::forceFunction(Value &, const PosIdx, std::string_view)` — force, ensure `nFunction` or functor
- `std::string_view EvalState::forceString(Value &, const PosIdx, std::string_view)` — typed force
- `void copyContext(const Value &, NixStringContext &, const ExperimentalFeatureSettings &)` — ingests context elements via `NixStringContextElem::parse`
- `std::string_view EvalState::forceString(Value &, NixStringContext &, const PosIdx, std::string_view, const ExperimentalFeatureSettings &)` — wraps + `copyContext`
- `std::string_view EvalState::forceStringNoCtx(Value &, const PosIdx, std::string_view)` — rejects strings with context, throws `EvalError`
- `bool EvalState::isDerivation(Value &)` — forces and checks `type = "derivation"`
- `std::optional<std::string> EvalState::tryAttrsToString(...)` — invokes `__toString` and `coerceToString`
- `BackedStringView EvalState::coerceToString(const PosIdx, Value &, NixStringContext &, std::string_view, bool, bool, bool)` — handles `nString`/`nPath`/`nAttrs` (with `__toString`/`outPath`)/`nExternal`/`coerceMore` (`nBool` true→"1", false→"", `nInt`/`nFloat`/`nNull`/`nList`)
- `StorePath EvalState::copyPathToStore(NixStringContext &, const SourcePath &)` — refuses `.drv` paths; caches via `srcToStore`; calls `fetchToStore`; allows path; inserts `Opaque` element into context
- `SourcePath EvalState::coerceToPath(const PosIdx, Value &, NixStringContext &, std::string_view)` — handles `nPath` directly, `nAttrs`/`__toString` recursively, otherwise coerces to string + canonicalise
- `StorePath EvalState::coerceToStorePath(const PosIdx, Value &, NixStringContext &, std::string_view)` — coerces to string then `maybeParseStorePath`
- `std::pair<SingleDerivedPath, std::string_view> EvalState::coerceToSingleDerivedPathUnchecked(const PosIdx, Value &, std::string_view, const ExperimentalFeatureSettings &)` — context size 1 enforced; errors on `DrvDeep`
- `SingleDerivedPath EvalState::coerceToSingleDerivedPath(const PosIdx, Value &, std::string_view)` — additionally verifies the string matches the expected representation
- `void EvalState::assertEqValues(Value &, Value &, const PosIdx, std::string_view)` — deep equality with informative errors; mirrors `eqValues`; `panic()` on default
- `bool EvalState::eqValues(Value &, Value &, const PosIdx, std::string_view)` — deep equality (int↔float compatibility, derivation outPath shortcut, attribute-by-attribute compare)
- `bool EvalState::fullGC()` — `GC_gcollect()` if Boehm; checks `GC_get_bytes_since_gc() < 1024`; returns false otherwise
- `void EvalState::maybePrintStats()` — runs `fullGC` (warns if it didn't) then `printStatistics` if `Counter::enabled`
- `void EvalState::printStatistics()` — emits JSON with `cpuTime`, `time`, env/list/value/attrset memory, sizes, counters, GC heap size + cycles + GC time, primOp/function call counts; honours `NIX_SHOW_STATS_PATH` and `NIX_SHOW_SYMBOLS`
- `SourcePath resolveExprPath(SourcePath, bool addDefaultNix)` — symlink walking with `maxFollow=1024` (throws `Error` on cycle); for directories appends `/default.nix` after `resolveSymlinks`
- `Expr * EvalState::parseExprFromFile(const SourcePath &)` (delegates to overload using `staticBaseEnv`) and overload with explicit `staticEnv` — read and parse, double-NUL terminator
- `Expr * EvalState::parseExprFromString(std::string, const SourcePath &, const std::shared_ptr<StaticEnv> &)` — string source via `Pos::String{.source = s}` (copies input for parser-overwrite safety)
- `Expr * EvalState::parseExprFromString(std::string, const SourcePath &)` — uses `staticBaseEnv`
- `ExprAttrs * EvalState::parseReplBindings(std::string, const SourcePath &, const std::shared_ptr<StaticEnv> &)` — wraps overload with same string as `errorSource`
- `ExprAttrs * EvalState::parseReplBindings(std::string, std::string errorSource, const SourcePath &, const std::shared_ptr<StaticEnv> &)` — flex two-NUL terminator, uses `Pos::String`
- `Expr * EvalState::parseStdin()` — parses stdin via `drainFD(0)`, marks `Pos::Stdin`
- `SourcePath EvalState::findFile(const std::string_view)` — uses internal `lookupPath`
- `SourcePath EvalState::findFile(const LookupPath &, const std::string_view, const PosIdx)` — search lookup path; caches positive/negative results in resolvedPaths; `nix/...` falls back to `corepkgsFS`; throws `ThrownError` on miss
- `std::shared_ptr<EvalState::LookupPathResolvedState> EvalState::resolveLookupPathPath(const LookupPath::Path &, bool initAccessControl)` — pseudo-URLs (downloads tarball + fetchToStore); scheme→hook lookup; plain path resolution with allowlisting; warns on missing/failed entries
- `Expr * EvalState::parse(char *, size_t, Pos::Origin, const SourcePath &, const std::shared_ptr<StaticEnv> &)` — invokes `parseExprFromBuf`, runs `bindVars`, persists doc comments per source via `positionToDocComment` (emplace-or-merge)
- `ExprAttrs * EvalState::parseReplBindings(char *, size_t, Pos::Origin, const SourcePath &, const std::shared_ptr<StaticEnv> &)` — analogous for REPL bindings via `parseReplBindingsFromBuf`
- `DocComment EvalState::getDocCommentForPos(PosIdx)` — looks up via `positionToDocComment`
- `std::string ExternalValueBase::coerceToString(EvalState &, const PosIdx &, NixStringContext &, bool, bool) const` — default throws `TypeError`
- `bool ExternalValueBase::operator==(const ExternalValueBase &) const noexcept` — default returns false
- `std::ostream & operator<<(std::ostream &, const ExternalValueBase &)` — delegates to `print`
- `void forceNoNullByte(std::string_view, std::function<Pos()>)` — substitutes NULs with `␀` and throws `Error` (with `atPos` if `pos` callable supplied)

### Macros / globals
- `static constexpr size_t BASE_ENV_SIZE = 128`
- `static Counter nrThunks` — file-local counter incremented by inline `mkThunk`
- `bool Counter::enabled = getEnv("NIX_SHOW_STATS").value_or("0") != "0"` — definition of the `Counter::enabled` flag
- `using json = nlohmann::json` — alias used inside the file
- `static_assert(sizeof(Env) <= 16, ...)` — env size invariant (in EvalState ctor)
- `static std::once_flag stackSizeBumped` — limited to one stack-size bump per process (in EvalState ctor, non-Windows)
- includes generated headers for primop nix files: `primops/derivation.nix.gen.hh`, `imported-drv-to-derivation.nix.gen.hh`, `fetchurl.nix.gen.hh`

## Cross-file observations

- **Single source of truth for the Value layout.** `value.hh` drives the discriminated-union Value through the `NIX_VALUE_STORAGE_FOR_EACH_FIELD` X-macro, which lists all 16 (T, FIELD_NAME, DISCRIMINATOR) tuples. The same table generates: union variants in the generic `ValueStorage<ptrSize>`; the `PayloadTypeToInternalType<T>` traits; and the `getStorage`/`setStorage` overloads in both the generic and bit-packed specialisation. The bit-packed specialisation (`useBitPackedValueStorage<8>`) hand-codes only the per-layout helpers (`setSingleDWordPayload`, `setUntaggablePayload`, `setPairOfPointersPayload`, `getPairOfPointersPayload`) and uses x86_64 SSE2 MOVAPS/MOVDQA via `_mm_store_si128`/`_mm_load_si128` for atomic 16-byte loads/stores; a fallback uses plain assignment.

- **Value layout invariants encoded as `static_assert`s.** `attr-set.hh` enforces `sizeof(Attr) == 2 * sizeof(uint32_t) + sizeof(Value *)` (no padding around `name`/`pos`); `eval.cc`'s `EvalState` ctor enforces `sizeof(Env) <= 16`; `eval-gc.cc` enforces `sizeof(void *) * 2 == GC_GRANULE_BYTES` (Boehm's 2-word granule).

- **Bidirectional GC integration.** When `NIX_USE_BOEHMGC` is enabled, `eval-gc.cc` registers displacements for the bit-packed Value's tagged pointers (`GC_register_displacement(i)` for `i ∈ [1, sizeof(uintptr_t))`) so the collector treats tagged pointers as roots; `eval-inline.hh::EvalMemory::allocValue` uses thread-local `GC_malloc_many` caches (one for `Value`, one for `Env` of size 1) to amortise allocation; `eval-gc.hh` provides `gc`/`gc_cleanup` stand-ins (and `traceable_allocator`/`gc_allocator` aliases of `std::allocator`) when GC is disabled.

- **Symbol table is append-only and concurrent.** `SymbolStr::SymbolValueStore` is a `ChunkedVector` (chunks of 65536 entries, capped at `numeric_limits<uint32_t>::max() / chunkSize`); the front-side `boost::concurrent_flat_set<SymbolStr, ...>` provides interning with transparent lookup via `Hash`/`Equal` functors that compare strings between `SymbolStr` and `Key`. `SymbolValue` itself is a protected-inherited `Value` (`mkStringNoCopy`-initialised), so `Value::toPtr(SymbolStr)` exposes an embedded `Value *` for symbol values without an extra allocation. `Symbol{0}` is reserved as the "unset" sentinel; ids start at 1.

- **Static (compile-time) symbol bootstrap.** `StaticEvalSymbols::preallocate()` + `StaticEvalSymbols::create()` populate a `consteval` `StaticSymbolTable` with the well-known names (`with`, `outPath`, …), and `EvalState::s` is a `static constexpr StaticEvalSymbols`. The runtime `SymbolTable` ctor takes the `StaticSymbolTable` and replays inserts via `copyIntoSymbolTable`, asserting that runtime ids match the compile-time ones. This means `EvalState::s.<name>` is usable without ever allocating a Symbol at run time.

- **Dual error hierarchies for caching.** `eval-error.hh` distinguishes `EvalError` (cacheable in pure mode) from `EvalBaseError` (not cacheable). `RecoverableEvalError` and `StackOverflowError` deliberately inherit from `EvalBaseError` and not `EvalError` so that the eval cache won't persist them. `EvalErrorBuilder<T>` is heap-allocated by `EvalState::error<T>` and self-deletes only on `debugThrow()` — there is no public way to construct one without `EvalState`.

- **Exception-driven `Failed` value type.** `Value::Failed` (under `detail::ValueBase`) is GC-aware (`gc_cleanup`) and stores an `exception_ptr` plus a `Value * recoveryValue` for `RecoverableEvalError`. `eval-inline.hh::forceValue` calls `handleEvalExceptionForThunk`/`...App` on any thrown exception, materialising a `tFailed` value; subsequent `forceValue`s of the same value call `handleEvalFailed`, which either replays the recovery value or re-throws via `Failed::rethrow()` (which uses `BaseError::throwClone()` to give each call a fresh copy of the exception).

- **Layered Bindings ("//" optimisation).** `Bindings` carries a `numLayers`/`baseLayer` linked-list chain (capped at `maxLayers = 8`). `Bindings::iterator` performs an on-the-fly k-way merge across layers using `boost::container::static_vector<BindingsCursor, maxLayers>` as a heap; lookups (`Bindings::get`) walk the chain doing per-layer binary search. `ExprOpUpdate::eval` decides whether to layer or memcpy-merge based on `bindingsUpdateLayerRhsSizeThreshold` (default 16 on 64-bit) and whether the chain is full. When the chain grows, `BindingsBuilder::finishSizeIfNecessary` recomputes the deduplicated `numAttrsInChain` using `set_intersection` or per-elem binary search, depending on relative sizes.

- **Thunk / app / failed recovery is hot-path-inlined in `eval-inline.hh`.** `forceValue`, `forceAttrs`, `forceList`, `addCallDepth` are all `[[gnu::always_inline]]`; the slow-path exception handlers (`handleEvalException*`, `handleEvalFailed`, `tryFixupBlackHolePos`) are `[[gnu::noinline]]` to avoid bloating the inlined caller. Similarly, `EvalErrorBuilder<T>`'s methods are all `[[gnu::noinline]]` because they're typically called in the throw-trace path.

- **Profiler hook caching.** `EvalProfiler` exposes a non-virtual `getNeededHooks()` that caches the result of the virtual `getNeededHooksImpl()` in a `std::optional<Hooks>`; subclasses (and the `MultiEvalProfiler` aggregator) call `invalidateNeededHooks()` when the hook set changes (e.g. on `addProfiler`). `EvalState::callFunction` checks the cached bitset (with `[[unlikely]]`) before invoking pre/post hooks and uses `Finally` to ensure post-hook always runs.

- **String context encoding doubles as cache encoding.** The on-disk format used by `eval-cache.cc::AttrDb::setString` is the same colon-free space-joined `NixStringContextElem::to_string()` form (`!<output>!<drvPath>`, `=<drvPath>`, `<path>`) that `value/context.cc` produces from `NixStringContextElem::parse`. The cache also re-validates each context entry by checking `store->isValidPath` for the underlying `StorePath`s; if any are invalid the cache falls back to fresh evaluation.

- **`Counter::enabled` gates all per-eval counters in one go.** Counters are cache-line-aligned (`alignas(std::hardware_destructive_interference_size)`) atomics whose arithmetic is gated by a single global `bool` (defined in `eval.cc` as `getEnv("NIX_SHOW_STATS").value_or("0") != "0"`). When disabled, every increment/decrement short-circuits to `return 0`, avoiding the atomic instruction. `EvalState::printStatistics` emits a JSON document with these counters; `EvalState::maybePrintStats` is the user-facing entry point that first runs `fullGC` for a more deterministic heap size.

- **Lazy file cache + lazy parser via `ExprParseFile`.** `evalFile` populates `importResolutionCache` (resolved path) and `fileEvalCache` (a `Value *` pointing to a thunk wrapping a heap-allocated `ExprParseFile`). The thunk's `eval` method runs `parseExprFromFile` + `state.eval` only when first forced. The FIXME notes intent to put `ExprParseFile` on the stack once a separate PR (#13930) merges.

- **`Value` static "constants" are not singletons.** `vEmptyList`/`vNull`/`vTrue`/`vFalse` are file-level statics initialised via IIFEs in `value.cc`; the comments explicitly note that pointer equality is _not_ sufficient because the eval cache and other consumers may construct fresh copies. `EvalState::getBool` returns `&Value::vTrue`/`&Value::vFalse` for the "no allocation needed" use case.

- **Bit-packed atomic 16-byte stores rely on AVX-detected at runtime.** `ValueStorage<8>::isAtomic()` lives in `value.cc` and uses `libcpuid` (when `HAVE_LIBCPUID`) to detect `CPU_FEATURE_AVX`. Without AVX or libcpuid the answer is `false`. Note that the actual MOVAPS/MOVDQA instructions are SSE2 and unconditional in the bit-packed specialisation; the `isAtomic()` helper exists so callers can decide whether to rely on the atomicity guarantee that AVX-aware SSE2 implementations provide on Intel.
