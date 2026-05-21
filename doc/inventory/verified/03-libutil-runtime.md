# Inventory — Shard 03: libutil runtime (verified)

This shard covers libutil runtime utilities: configuration plumbing, error/logging,
argument parsing, process management, current-process introspection, executable path
search, environment variables, threading primitives, signals, users/XDG, experimental
features, Unix-domain sockets, and miscellaneous string/utility helpers.

Each entry below has been verified against the source.

---

## File: src/libutil/configuration.cc

### Namespaces
- `nix` — wraps all definitions.

### Classes / structs / enums
None defined here (out-of-line members for classes declared in the header).

### Functions
- `Config::Config(StringMap initials)` — constructor; forwards to `AbstractConfig`.
- `Config::set(name, value)` — looks up setting (or strips `extra-` prefix for appendable), calls `set(value, append)`, marks `overridden`; returns whether it was known.
- `Config::addSetting(AbstractSetting *)` — registers setting and aliases in `_settings`; if any matching `unknownSettings` entry exists it is consumed (with a warning if both name and an alias were set).
- `AbstractConfig::AbstractConfig(StringMap initials)` — constructor; stores initials in `unknownSettings`.
- `AbstractConfig::warnUnknownSettings()` — emits `warn("unknown setting '%s'", ...)` for each unknown.
- `AbstractConfig::reapplyUnknownSettings()` — moves out the unknown map and replays each via `set`.
- `Config::getSettings(res, overriddenOnly) const` — copies non-alias settings (filtered by `overriddenOnly` and experimental-feature gating) into `res` as `SettingInfo`.
- `parseConfigFiles(contents, path, parsedContents)` (file-static) — line-oriented parser handling `name = value`, `include`, `!include` (with recursive include resolution).
- `AbstractConfig::applyConfig(contents, path)` — parses with `parseConfigFiles`; first applies `experimental-features`/`extra-experimental-features`, then everything else (skipping `nix-path`/`extra-nix-path` if `NIX_PATH` env var is set).
- `Config::resetOverridden()` — clears `overridden` flag on all settings.
- `Config::toJSON()` — produces JSON object of non-alias settings via each setting's `toJSON()`.
- `Config::toKeyValue()` — produces "k = v\n" form for **alias** entries only (non-alias entries handled by `GlobalConfig::toKeyValue`).
- `Config::convertToArgs(Args &, category)` — calls `convertToArg` on each non-alias setting.
- `AbstractSetting::AbstractSetting(name, description, aliases, experimentalFeature)` — base constructor; description is run through `stripIndentation`.
- `AbstractSetting::~AbstractSetting()` — asserts `created == 123` to detect a known gcc miscompilation.
- `AbstractSetting::toJSON()` — wraps `toJSONObject()` in a json object.
- `AbstractSetting::toJSONObject() const` — emits `description`, `aliases`, `experimentalFeature` (or null).
- `AbstractSetting::convertToArg(args, category)` — empty default; overridden in `BaseSetting<T>`.
- `AbstractSetting::isOverridden() const` — returns `overridden`.
- Template specializations of `BaseSetting<T>::parse / to_string / appendOrSet / convertToArg` for:
  - `BaseSetting<std::string>` — `parse` (identity), `to_string`.
  - `BaseSetting<std::optional<std::string>>` — `parse` (empty -> nullopt), `to_string`.
  - `BaseSetting<bool>` — `parse` (true/yes/1 vs false/no/0), `to_string`, `convertToArg` (adds `--name` and `--no-name` flags).
  - `BaseSetting<std::list<std::filesystem::path>>` — `parse`, `appendOrSet`, `to_string`.
  - `BaseSetting<Strings>` — `parse`, `appendOrSet`, `to_string`.
  - `BaseSetting<StringSet>` — `parse`, `appendOrSet`, `to_string`.
  - `BaseSetting<std::set<std::filesystem::path>>` — `parse`, `appendOrSet`, `to_string`.
  - `BaseSetting<std::set<ExperimentalFeature>>` — `parse` (parses tokens, auto-enables `FetchTree` when `Flakes` is set, warns on `no-url-literals` and unknown features), `appendOrSet`, `to_string`.
  - `BaseSetting<StringMap>` — `parse` (key=value tokens), `appendOrSet`, `to_string` (via `transform_reduce`).
  - `BaseSetting<std::filesystem::path>` — `parse` (rejects empty), `to_string`.
  - `BaseSetting<AbsolutePath>` — `parse` (delegates to `parseAbsolutePath`), `to_string`.
  - `BaseSetting<std::optional<AbsolutePath>>` — `parse` (empty -> nullopt), `to_string`.
- `parseAbsolutePath(const AbstractSetting &, const std::string &)` (file-static) — `canonPath` wrapper that throws `UsageError` on empty.
- `ExperimentalFeatureSettings::isEnabled(feature) const` — set membership in `experimentalFeatures`.
- `ExperimentalFeatureSettings::require(feature, reason) const` — throws `MissingExperimentalFeature` if not enabled.
- `ExperimentalFeatureSettings::isEnabled(std::optional<feature> &) const` — true when no feature given.
- `ExperimentalFeatureSettings::require(std::optional<feature> &) const` — no-op when no feature given.

### Type aliases
None.

### Macros / globals
- Explicit template instantiations: `BaseSetting<int>`, `unsigned int`, `long`, `unsigned long`, `long long`, `unsigned long long`, `bool`, `std::string`, `std::list<std::filesystem::path>`, `Strings`, `StringSet`, `StringMap`, `std::set<ExperimentalFeature>`, `std::filesystem::path`, `AbsolutePath`, `std::optional<AbsolutePath>`, `std::optional<std::string>`.

---

## File: src/libutil/include/nix/util/configuration.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `class Args;` (forward decl).
- `class AbstractSetting;` (forward decl).
- `class AbstractConfig`
  - protected `StringMap unknownSettings`.
  - protected ctor `AbstractConfig(StringMap initials = {})`.
  - pure virtual `set(name, value)`.
  - nested `struct SettingInfo { std::string value; std::string description; }`.
  - pure virtual `getSettings(res, overriddenOnly = false) const`.
  - `applyConfig(contents, path = "<unknown>")`.
  - pure virtual `resetOverridden()`.
  - pure virtual `toJSON()` returning `nlohmann::json`.
  - pure virtual `toKeyValue()`.
  - pure virtual `convertToArgs(args, category)`.
  - `warnUnknownSettings()`, `reapplyUnknownSettings()`.
  - virtual default destructor.
- `class Config : public AbstractConfig`
  - friend `class AbstractSetting`.
  - public `struct SettingData { bool isAlias; AbstractSetting * setting; }`.
  - public `using Settings = std::map<std::string, SettingData>`.
  - private `Settings _settings`.
  - ctor `Config(StringMap initials = {})`.
  - overrides `set`, `getSettings`, `resetOverridden`, `toJSON`, `toKeyValue`, `convertToArgs`.
  - `addSetting(AbstractSetting *)`.
- `class AbstractSetting`
  - friend `class Config`.
  - public const `name`, `description`, `aliases`.
  - `int created = 123` (gcc miscompile sentinel).
  - `bool overridden = false`.
  - `std::optional<ExperimentalFeature> experimentalFeature`.
  - `bool isOverridden() const`.
  - protected ctor (with optional `experimentalFeature`).
  - virtual destructor.
  - pure virtual `set(value, append = false)`, `isAppendable()`, `to_string() const`.
  - `toJSON()`, virtual `toJSONObject() const`.
  - virtual `convertToArg(args, category)`.
- `struct AbsolutePath`
  - private `std::filesystem::path _path`.
  - constructors from `path`, `const char *`, `const wchar_t *` (Windows-only) — each throws `Error` if not absolute.
  - accessors: `path()`, `operator const std::filesystem::path &()`, `string()`, `native()`, `c_str()`, `empty()`.
  - `operator/`, three `operator==` overloads (against `AbsolutePath`, `path`, `std::string`), `operator<=>`, friend `operator<<`.
- `template<> struct json_avoids_null<AbsolutePath> : std::true_type` — JSON helper trait.
- `template<typename T> class BaseSetting : public AbstractSetting`
  - protected `value`, `defaultValue`, `documentDefault`.
  - virtual `parse(str) const`, virtual `appendOrSet(newValue, append)`.
  - constructor `(def, documentDefault, name, description, aliases = {}, experimentalFeature = nullopt)`.
  - conversion operators to `const T &` and `T &`.
  - `get()` const + non-const.
  - templated `operator==`, `operator!=`, `operator=` (forwards to `assign`).
  - virtual `assign(v)` (default writes `value`).
  - templated `setDefault(v)` — assigns only if not overridden.
  - final override `set(str, append)`.
  - struct `trait` (template-specialized in `config-impl.hh`).
  - final override `isAppendable()`.
  - virtual `override(v)` — sets overridden flag and value.
  - overrides `to_string`, `convertToArg`, `toJSONObject`.
- `template<typename T> class Setting : public BaseSetting<T>` — registers itself with `Config*` in constructor; `operator=` forwarding to `assign`.
- `template<> class Setting<AbsolutePath> : public BaseSetting<AbsolutePath>` — specialization that pulls in base ctor/assign and adds an explicit ctor + `operator const std::filesystem::path &()` to bridge the two-step user-defined-conversion barrier.
- `struct ExperimentalFeatureSettings : Config`
  - `Setting<std::set<ExperimentalFeature>> experimentalFeatures` (with embedded markdown description).
  - `isEnabled(feature) const`, `require(feature, reason = "") const`, lazy-reason `require<GetReason>(feature, getReason)`, optional overloads for `isEnabled` and `require`.

### Functions
- `template<typename T> std::ostream & operator<<(std::ostream &, const BaseSetting<T> &)` — print value.
- `template<typename T> bool operator==(const T & v1, const BaseSetting<T> & v2)`.
- Four deleted `formatHelper` overloads for `AbsolutePath` and `Setting<AbsolutePath>` (with and without trailing args) — disables `fmt` quoting footguns.
- `template<> void BaseSetting<std::set<std::filesystem::path>>::appendOrSet(...)` — out-of-class declaration.

### Type aliases
- `Config::Settings`.

### Macros / globals
- `#define NIX_DECLARE_CONFIG_SERIALISER(TY)` — declares specializations of `BaseSetting<TY>::parse` and `to_string`.
- `extern ExperimentalFeatureSettings experimentalFeatureSettings;` (with FIXME comment about being global).

---

## File: src/libutil/include/nix/util/config-impl.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- Template specializations of `BaseSetting<T>::trait` for `Strings`, `StringSet`, `StringMap`, `std::set<ExperimentalFeature>` — all set `appendable = true`.
- General `template<typename T> struct BaseSetting<T>::trait` — sets `appendable = false`.

### Functions
- `template<typename T> bool BaseSetting<T>::isAppendable()` — returns `trait::appendable`.
- Forward declarations of specialized `appendOrSet` for the four appendable types.
- `template<typename T> void BaseSetting<T>::appendOrSet(T newValue, bool append)` — default; `static_assert` not appendable, asserts `!append`, moves value.
- `template<typename T> void BaseSetting<T>::set(const std::string & str, bool append)` — gates on experimental feature; if disabled, asserts a feature was set and warns.
- `template<> void BaseSetting<bool>::convertToArg(Args &, category)` — declaration (definition in `.cc`).
- `template<typename T> void BaseSetting<T>::convertToArg(Args &, category)` — adds `--name` (set) and, if appendable, `--extra-name` flags.
- `template<typename T> T BaseSetting<T>::parse(const std::string &) const` — generic integral parser via `string2IntWithUnitPrefix<T>`; `static_assert(std::is_integral<T>::value, "Integer required.")`.
- `template<typename T> std::string BaseSetting<T>::to_string() const` — generic integral via `std::to_string` (`static_assert` integral).

### Type aliases
None.

### Macros / globals
- Multiple invocations of `NIX_DECLARE_CONFIG_SERIALISER(...)` for: `std::string`, `std::optional<std::string>`, `bool`, `Strings`, `StringSet`, `StringMap`, `std::set<ExperimentalFeature>`, `std::filesystem::path`, `AbsolutePath`, `std::set<std::filesystem::path>`, `std::optional<AbsolutePath>`.

---

## File: src/libutil/config-global.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None new (implements `GlobalConfig`).

### Functions
- `GlobalConfig::configRegistrations()` — Meyers singleton returning `static ConfigRegistrations &`.
- `GlobalConfig::set(name, value)` — iterates registered configs; on miss, stashes pair into `unknownSettings` and returns `false`.
- `GlobalConfig::getSettings(res, overriddenOnly) const` — fans out to each registered config's `getSettings`.
- `GlobalConfig::resetOverridden()` — fans out.
- `GlobalConfig::toJSON()` — merges (`update`) every config's JSON.
- `GlobalConfig::toKeyValue()` — gathers via `globalConfig.getSettings` and emits `"k = v\n"` lines (i.e. uses globalConfig directly rather than receiver).
- `GlobalConfig::convertToArgs(args, category)` — fans out.
- `GlobalConfig::Register::Register(Config *)` — appends to registrations.

### Type aliases
None.

### Macros / globals
- `GlobalConfig globalConfig;` — definition.
- `ExperimentalFeatureSettings experimentalFeatureSettings;` — definition (declared `extern` in `configuration.hh`).
- `static GlobalConfig::Register rSettings(&experimentalFeatureSettings);` — registers the experimental-feature config.

---

## File: src/libutil/include/nix/util/config-global.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct GlobalConfig : public AbstractConfig`
  - `typedef std::vector<Config *> ConfigRegistrations`.
  - `static ConfigRegistrations & configRegistrations()`.
  - overrides `set`, `getSettings`, `resetOverridden`, `toJSON`, `toKeyValue`, `convertToArgs`.
  - nested `struct Register { Register(Config *); }`.

### Functions
None standalone.

### Type aliases
- `GlobalConfig::ConfigRegistrations` (typedef).

### Macros / globals
- `extern GlobalConfig globalConfig;`.

---

## File: src/libutil/error.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None defined here (out-of-line members + free functions).

### Functions
- `BaseError::addTrace(std::shared_ptr<const Pos> &&, HintFmt, TracePrint)` — pushes a `Trace` at the front of `err.traces`.
- `throwExceptionSelfCheck()` — throws an `Error` used by `initLibUtil()` to verify exception unwinding.
- `BaseError::recalcWhat() const` — re-renders `what_` via `showErrorInfo` using `loggerSettings.showTrace`.
- `BaseError::calcWhat() const` — caches/returns rendered message.
- `BaseError::hasPos() const` — true iff `err.pos.get() && *err.pos.get()`.
- `ErrorInfo::programName` static member — definition (initialized to `nullopt`).
- `operator<<(std::ostream &, const HintFmt &)`.
- `inline std::strong_ordering operator<=>(const Trace &, const Trace &)` (file-scope) — null-safe pointer comparison, then dereferenced `Pos` compare, then hint-string compare.
- `printCodeLines(out, prefix, errPos, loc)` — renders previous, current (with column arrow), and next lines of code.
- `indent(indentFirst, indentRest, s)` (file-static) — re-indent a multi-line string, chomping trailing whitespace from each line.
- `printPosMaybe(oss, indent, pos)` (file-static) — render position with code lines if known; honors `printUnknownLocations`. Returns whether a position was printed.
- `printTrace(output, indent, count, trace)` (file-static) — render single trace with optional pos.
- `printSkippedTracesMaybe(output, indent, count, skippedTraces, tracesSeen)` — output skipped trace summary, or print them inline if there are five or fewer.
- `showErrorInfo(out, einfo, showTrace)` — main error renderer: dispatches prefix on verbosity, walks traces with deduplication, prints suggestions, optionally truncates after three traces unless `showTrace`.
- `writeErr(buf)` (file-static) — robust loop write to stderr fd, retrying on EINTR; aborts on real failure.
- `panic(msg)` — write panic message and `std::terminate()`.
- `unreachable(loc)` — format `std::source_location` and panic.
- `handleExceptions(programName, body)` — central main-wrapper: constructs `ReceiveInterrupts`, sets `ErrorInfo::programName`, catches `Exit`, `UsageError`, `BaseError`, `std::bad_alloc`, `std::exception` and translates to exit codes.

### Type aliases
None.

### Macros / globals
- `static bool printUnknownLocations = getEnv("_NIX_EVAL_SHOW_UNKNOWN_LOCATIONS").has_value();` — file-scope, controls "UNKNOWN LOCATION" rendering.

---

## File: src/libutil/include/nix/util/error.hh

### Namespaces
- `nix`.
- `nix::windows` (Windows-only block).

### Classes / structs / enums
- `typedef enum { lvlError, lvlWarn, lvlNotice, lvlInfo, lvlTalkative, lvlChatty, lvlDebug, lvlVomit } Verbosity`.
- `struct LinesOfCode { std::optional<std::string> prevLineOfCode, errLineOfCode, nextLineOfCode; }`.
- `struct Pos;` — forward declaration only (full type in `position.hh`).
- `enum struct TracePrint { Default, Always }`.
- `struct Trace { std::shared_ptr<const Pos> pos; HintFmt hint; TracePrint print = Default; }`.
- `struct ErrorInfo` — fields `level`, `msg`, `pos`, `traces`, `isFromExpr = false`, `status = 1`, `suggestions`, static `programName`.
- `class BaseError : public std::exception`
  - protected `mutable ErrorInfo err`, `mutable std::optional<std::string> what_`, `calcWhat() const`.
  - copy ctor + assignment defaulted.
  - templated ctors `(unsigned int status, Args &&...)`, `(const std::string & fs, Args &&...)`, `(const Suggestions &, Args &&...)`.
  - non-template ctors `(HintFmt)`, `(ErrorInfo &&)`, `(const ErrorInfo &)`.
  - `message() const`, `what() const noexcept` (returns `calcWhat().c_str()`), `msg() const`, `info() const`, `withExitStatus(unsigned int)`, `atPos(shared_ptr)`, `hasPos() const`, `pushTrace(Trace)`, two `addTrace` overloads (varargs and `(pos, HintFmt, print)`), `hasTrace() const`, `unsafeInfo()`, `recalcWhat() const`, pure virtual `[[noreturn]] throwClone() const`.
- `template<typename Derived, typename Base> class CloneableError : public Base` — implements `throwClone()` by copying `*this` to `Derived`. Friend-only default ctor; pulls in base ctors via `using Base::Base`.
- `class SystemError : public CloneableError<SystemError, Error>`
  - private `std::error_code errorCode`, `std::string errorDetails`.
  - protected tag types `DisambigHintFmt`, `DisambigVarArgs` for ctor disambiguation.
  - protected `(DisambigHintFmt, error_code, errorDetails, HintFmt)` ctor — formats `"%s: %s"`.
  - protected templated `(DisambigVarArgs, error_code, errorDetails, Args &&...)` ctor — delegates to the HintFmt one.
  - public `(error_code, HintFmt)` ctor and templated `(error_code, Args &&...)` ctor; both forward `errorCode.message()` as the details.
  - `ec() const &`, `is(std::errc) const`.
- `class SysError final : public CloneableError<SysError, SystemError>`
  - public `int errNo`.
  - templated varargs ctor `SysError(int errNo, Args &&...)` — uses `strerror(errNo)`.
  - `(int errNo, const HintFmt &)` ctor.
  - templated varargs ctor reading ambient `errno`.
  - generic `SysError(auto && mkHintFmt) requires std::invocable<...> && std::same_as<...HintFmt>` — captures `errno` first via `captureErrno`.
  - private static `captureErrno(auto && mkHintFmt)`, private pair-forwarder ctor.
- `class windows::WinError : public CloneableError<WinError, SystemError>` (Windows-only) — analogous to `SysError`, fields `DWORD lastError`, uses `GetLastError()` / `FormatMessageA`. Has private static `captureLastError`, `renderError(DWORD)`.

### Functions
- `printCodeLines(out, prefix, errPos, loc)` — declaration.
- `inline std::strong_ordering operator<=>(const Trace &, const Trace &)` — declaration.
- `showErrorInfo(out, einfo, showTrace)` — declaration.
- `throwExceptionSelfCheck()` — declaration.
- `[[noreturn]] panic(std::string_view msg)` — declaration.
- `handleExceptions(programName, body)` — declaration.
- `[[gnu::noinline, gnu::cold, noreturn]] unreachable(loc = std::source_location::current())`.
- `windows::WinError::renderError(DWORD)` (private static, Windows only).

### Type aliases
- `using NativeSysError = windows::WinError;` (Windows) or `= SysError;` (Unix) — single typedef chosen via `#ifdef _WIN32`.

### Macros / globals
- `#define MakeError(newClass, superClass)` — defines `class newClass : public CloneableError<newClass, superClass> { using CloneableError::CloneableError; }`.
- `MakeError(Error, BaseError)`, `MakeError(UsageError, Error)`, `MakeError(UnimplementedError, Error)`.
- `#define nixUnreachableWhenHardened ::nix::unreachable` (when `NIX_UBSAN_ENABLED`) or `std::unreachable` otherwise.

---

## File: src/libutil/exit.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None defined here.

### Functions
- `Exit::~Exit()` — out-of-line destructor (key function for `Exit`'s vtable).

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/include/nix/util/exit.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `class Exit : public std::exception`
  - public `int status`.
  - default ctor (`status = 0`), `explicit Exit(int)` ctor.
  - virtual destructor.

### Functions
None.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/logging.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `class SimpleLogger : public Logger` (file-scope) — fields `bool systemd`, `bool tty`, `bool printBuildLogs`; ctor reads `IN_SYSTEMD` env and `isTTY()`. Overrides `isVerbose`, `log` (with systemd-prefix mapping), `logEI`, `startActivity` (logs "...$s..."), `result` (forwards `resBuildLogLine` and `resPostBuildLogLine` when `printBuildLogs`).
- `struct JSONLogger : Logger`
  - fields `Descriptor fd`, `bool includeNixPrefix`.
  - nested `struct State { bool enabled = true; }`, `Sync<State> _state`.
  - ctor `(Descriptor, bool)`.
  - `isVerbose()` always returns true.
  - `addFields(json, fields)` helper — copies int/string fields into json array.
  - `write(const nlohmann::json &)` — locked write with disable-on-error (logs once via `logger->warn`).
  - overrides `log`, `logEI` (renders via `showErrorInfo` plus structured msg/pos/traces), `startActivity`, `stopActivity`, `result`.
- `struct JSONFileLogger : JSONLogger` (defined inside `makeJSONLogger(path)`) — owns `AutoCloseFD fd`, forwards descriptor to `JSONLogger`.

### Functions
- `getCurActivity()` — returns thread-local `curActivity` id.
- `setCurActivity(id)` — sets thread-local.
- `Logger::warn(const std::string & msg)` — formats `"warning:"` prefix and forwards to `log(lvlWarn, ...)`.
- `Logger::writeToStdout(std::string_view)` — `writeFull` to stdout descriptor + newline.
- `Logger::suspend()` — calls `pause()` and returns RAII guard that calls `resume()` on destruction.
- `Logger::suspendIf(bool)` — returns optional suspend.
- `getPid()` (file-static) — `getpid()` / `GetCurrentProcessId()`.
- `Activity::Activity(...)` — generates `id` from `nextId++ + (pid << 32)`; calls `logger.startActivity`.
- `Activity::~Activity()` — calls `logger.stopActivity(id)` swallowing exceptions via `ignoreExceptionInDestructor`.
- `to_json(nlohmann::json &, std::shared_ptr<const Pos>)` — emits `line/column/file` (or nulls).
- `writeFullLogging(fd, s)` (file-static) — `writeFull` swallowing `SystemError`.
- `writeToStderr(std::string_view)` — wrapper around `writeFullLogging(getStandardError(), ...)`.
- `makeSimpleLogger(printBuildLogs)` — `make_unique<SimpleLogger>`.
- `makeJSONLogger(Descriptor fd, includeNixPrefix)`.
- `makeJSONLogger(const path &, includeNixPrefix)` — opens file (or unix socket via `connect`) and returns `JSONFileLogger`.
- `applyJSONLogger()` — if `loggerSettings.jsonLogPath` set, wraps current `logger` with `makeTeeLogger`; aborts if tee construction fails after the original logger was moved out.
- `getFields(nlohmann::json &)` (file-static) — parse JSON array of unsigned/string fields; throws on other types.
- `parseJSONMessage(msg, source)` — strips `@nix ` prefix and parses JSON; logs and returns nullopt on parse error.
- `handleJSONLogMessage(json, act, activities, source, trusted)` — process `start`/`stop`/`result`/`setPhase`/`msg` actions; trusted gate on `start` (only `actFileTransfer` permitted from untrusted).
- `handleJSONLogMessage(string msg, ...)` — overload that calls `parseJSONMessage` then forwards.

### Type aliases
None.

### Macros / globals
- `LoggerSettings loggerSettings;` — global definition.
- `static GlobalConfig::Register rLoggerSettings(&loggerSettings);` — registers with `globalConfig`.
- `static thread_local ActivityId curActivity = 0;`.
- `std::unique_ptr<Logger> logger = makeSimpleLogger(true);` — global default logger.
- `Verbosity verbosity = lvlInfo;` — global verbosity.
- `std::atomic<uint64_t> nextId{0};` — global activity counter.

---

## File: src/libutil/include/nix/util/logging.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `typedef enum ActivityType` with values `actUnknown=0`, `actCopyPath=100`, `actFileTransfer=101`, `actRealise=102`, `actCopyPaths=103`, `actBuilds=104`, `actBuild=105`, `actOptimiseStore=106`, `actVerifyPaths=107`, `actSubstitute=108`, `actQueryPathInfo=109`, `actPostBuildHook=110`, `actBuildWaiting=111`, `actFetchTree=112`.
- `typedef enum ResultType` with values `resFileLinked=100`, `resBuildLogLine=101`, `resUntrustedPath=102`, `resCorruptedPath=103`, `resSetPhase=104`, `resProgress=105`, `resSetExpected=106`, `resPostBuildLogLine=107`, `resFetchStatus=108`.
- `typedef uint64_t ActivityId`.
- `struct LoggerSettings : Config`
  - `Setting<bool> showTrace{ this, false, "show-trace", ... }`.
  - `Setting<std::optional<AbsolutePath>> jsonLogPath{ this, {}, "json-log-path", ... }`.
- `class Logger`
  - friend `struct Activity`.
  - nested `struct Field { enum { tInt = 0, tString = 1 } type; uint64_t i = 0; std::string s; }` plus three converting ctors (string, const char *, uint64_t).
  - `typedef std::vector<Field> Fields`.
  - virtual destructor, virtual `stop`, virtual `pause`, `resume`, `isVerbose` (default false).
  - nested `struct Suspension { Finally<fun<void()>> _finalize; }`.
  - `Suspension suspend()`, `std::optional<Suspension> suspendIf(bool)`.
  - pure virtual `log(Verbosity, std::string_view) = 0`, plus inline overload `log(string_view)` defaulting to `lvlInfo`.
  - pure virtual `logEI(const ErrorInfo &) = 0`, plus inline `logEI(Verbosity, ErrorInfo)` overriding level.
  - virtual `warn(const std::string &)`.
  - virtual `startActivity(id, lvl, type, s, fields, parent)`, `stopActivity(id)`, `result(id, type, fields)` (default no-ops).
  - virtual `writeToStdout(std::string_view)`.
  - templated `cout(args...)` writing `fmt(args...)` to stdout.
  - virtual `ask(string_view)` (default `{}`).
  - virtual `setPrintBuildLogs(bool)` (default no-op).
- `struct nop` — variadic ctor that does nothing (used to expand parameter packs).
- `struct Activity`
  - `Logger & logger`, `const ActivityId id`.
  - constructor `(logger, lvl, type, s = "", fields = {}, parent = getCurActivity())`.
  - constructor `(logger, type, fields = {}, parent = getCurActivity())` delegating with `lvlError` and empty string.
  - deleted copy ctor.
  - destructor.
  - `progress(done = 0, expected = 0, running = 0, failed = 0) const` — emits `resProgress`.
  - `setExpected(type2, expected) const` — emits `resSetExpected`.
  - templated `result(type, args...) const` packs args via `nop`.
  - `result(type, fields) const` — calls `logger.result`.
  - friend `class Logger`.
- `struct PushActivity` — RAII wrapper that swaps `curActivity` and restores in destructor.

### Functions
- `getCurActivity()`, `setCurActivity(id)` — declarations.
- `makeSimpleLogger(printBuildLogs = true)`.
- `makeTeeLogger(mainLogger, extraLoggers)`.
- Two `makeJSONLogger` overloads (`Descriptor`, `path`).
- `applyJSONLogger()`.
- `parseJSONMessage(msg, source)`.
- Two `handleJSONLogMessage` overloads (`json &`, `msg`).
- `template<typename... Args> inline void warn(const std::string & fs, const Args &... args)` — uses `boost::format` with `formatHelper`.
- `void writeToStderr(std::string_view)`.

### Type aliases
- `Logger::Fields` (typedef inside class).

### Macros / globals
- `extern LoggerSettings loggerSettings;`.
- `extern std::unique_ptr<Logger> logger;`.
- `extern Verbosity verbosity;`.
- `#define logErrorInfo(level, errorInfo...)` — guarded `logger->logEI`.
- `#define logError(...) logErrorInfo(lvlError, ...)`.
- `#define logWarning(...) logErrorInfo(lvlWarn, ...)`.
- `#define printMsgUsing(loggerParam, level, args...)` — formats and dispatches.
- `#define printMsg(level, args...) printMsgUsing(logger, level, args)`.
- `#define printError(args...) printMsg(lvlError, args)`.
- `#define notice(args...) printMsg(lvlNotice, args)`.
- `#define printInfo(args...) printMsg(lvlInfo, args)`.
- `#define printTalkative(args...) printMsg(lvlTalkative, args)`.
- `#define debug(args...) printMsg(lvlDebug, args)`.
- `#define vomit(args...) printMsg(lvlVomit, args)`.
- `#define warnOnce(haveWarned, args...)` — fires `warn` only the first time, sets the boolean flag.

---

## File: src/libutil/tee-logger.cc

### Namespaces
- `nix` (with anonymous namespace inside).

### Classes / structs / enums
- `class TeeLogger final : public Logger` (anonymous-namespace) — owns `std::vector<std::unique_ptr<Logger>> loggers`. Overrides `stop`, `pause`, `resume` (fan-out), `log`, `logEI`, `startActivity`, `stopActivity`, `result` (fan-out), `writeToStdout` (only first logger), `ask` (returns first non-empty result), `setPrintBuildLogs` (fan-out).

### Functions
- `makeTeeLogger(mainLogger, extraLoggers)` — bundles `mainLogger` followed by `extraLoggers` into a `TeeLogger`.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/args.cc

### Namespaces
- `nix` (with anonymous namespace for shebang parser helpers).

### Classes / structs / enums
- `struct Parser` (anonymous ns) — abstract base; field `std::string_view remaining`; pure virtual `operator()(state, r)`; ctor `(string_view)`; virtual destructor.
- `struct ParseQuoted : public Parser` — accumulator string `acc`; overrides `operator()` (handles space, double backtick close, triple+ backtick escape, otherwise verbatim).
- `struct ParseUnquoted : public Parser` — accumulator `acc`; overrides `operator()` with token classification: whitespace splits, double backtick opens quoted state, listed reserved chars throw, leading `#` throws, otherwise accumulates.

### Functions
- `Args::addFlag(Flag &&)` — `make_shared<Flag>`, asserts arity matches labels, registers in `longFlags` (under longName + each alias) and `shortFlags`.
- `Args::removeFlag(longName)` — asserts existence, removes from longFlags + shortFlags.
- `Completions::setType(t)` — stores completion type.
- `Completions::add(completion, description)` — trims/ellipsizes description (truncates at first `.` or `\n`, appending `" [...]"` if truncated mid-string), inserts a `Completion`.
- `Completion::operator<=>(const Completion &) const noexcept` — defaulted (out-of-class).
- `Args::getRoot()` — walks `parent` chain to root and `dynamic_cast`s to `RootArgs &` (asserts).
- `RootArgs::needsCompletion(s)` — returns `s` truncated at the completion marker, or nullopt if no completions are being tracked.
- `parseShebangContent(s)` — trampoline driver running parser state machine starting with `ParseUnquoted`.
- `RootArgs::parseCmdline(cmdline, allowShebang)` — main CLI parser; reads `NIX_GET_COMPLETIONS` env var (sets verbosity to error and inserts completion marker), handles shebang scripts (parses lines starting with `#`/`/`/`\\`/`%`/`@`/`*`/`-`/`(`, picks out `#! nix ...` directives), expands compound short flags (`-qlf` -> `-q -l -f`), handles `--`, dispatches flags via `processFlag`, accumulates positional args via `rewriteArgs`/`processArgs`, finally calls `checkArgs`, `initialFlagsProcessed`, requires accumulated `flagExperimentalFeatures`, and runs `deferredCompletions`.
- `Args::getCommandBaseDir() const` — asserts parent and delegates to `parent->getCommandBaseDir()`.
- `RootArgs::getCommandBaseDir() const` — returns stored `commandBaseDir`.
- `Args::processFlag(pos, end)` — long/short flag dispatch; handles `--` long form (with completion-prefix expansion against `longFlags`), `-c` short form, and bare `-` completion fallback that lists short flags. For each consumed flag it pulls handler arity arguments (with deferred completions when arg matches the completion marker).
- `Args::processArgs(args, finish)` — pop next expected positional, run handler if its arity is satisfied (or `finish` for `ArityAny`), splice front of `expectedArgs` into `processedArgs` to keep iterators alive, and throw `UsageError` if `finish` and a non-optional positional remains.
- `Args::checkArgs()` — verifies required flags were used; throws `UsageError` for first missing.
- `Args::toJSON()` — JSON description: per-long-flag (skipping aliases) records `hiddenCategory`, `shortName`, `description`, `category`, `arity`, `labels`, `experimental-feature`; per-positional records `label`, `optional`, `arity`; top-level `description`, `flags`, `args`, optional `doc`.
- `_completePath(completions, prefix, onlyDirs)` (file-static) — POSIX `glob(3)` with `GLOB_NOESCAPE` (and `GLOB_ONLYDIR` when available); on Windows the body is a no-op.
- `Args::completePath(completions, _, prefix)` / `Args::completeDir(...)` — wrappers around `_completePath`.
- `argvToStrings(argc, argv)` — convert C argv (skipping argv[0]) to `Strings`.
- `Command::experimentalFeature()` — returns `Xp::NixCommand`.
- `MultiCommand::MultiCommand(commandName, commands)` — installs an optional `subcommand` positional (with handler that resolves the command, runs `Suggestions::bestMatches` on miss) and `categories[catDefault] = "Available commands"`.
- `MultiCommand::processFlag(pos, end)` — base then selected subcommand.
- `MultiCommand::processArgs(args, finish)` — delegates to subcommand if selected, otherwise base.
- `MultiCommand::checkArgs()` — base then subcommand.
- `MultiCommand::toJSON()` — base JSON augmented with `commands` map (each command's JSON enriched with `category` containing `id`, `description`, `experimental-feature`).
- `MultiCommand::rewriteArgs(args, pos)` — delegates to subcommand if any, otherwise replaces a top-level alias hit (warns on `Deprecated`); sets `aliasUsed` to single-shot.

### Type aliases
None.

### Macros / globals
- `std::string completionMarker = "___COMPLETE___";` — sentinel substring used by completion-aware flag/positional parsing.

---

## File: src/libutil/include/nix/util/args.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `enum struct HashAlgorithm : char;` (forward decl).
- `enum struct HashFormat : int;` (forward decl).
- `class MultiCommand;` (forward).
- `class RootArgs;` (forward).
- `class AddCompletions;` (forward).
- `class Args`
  - virtual `description()` returning empty string by default.
  - virtual `forceImpureByDefault()` returning false.
  - virtual `doc()` returning empty.
  - virtual `getCommandBaseDir() const` (parent delegation).
  - virtual destructor.
  - protected `static const size_t ArityAny = numeric_limits<size_t>::max()`.
  - protected nested `struct Handler` — many constructors taking different functor/destination types: default; varargs `function<void(vector<string>)>` (`ArityAny`); zero-arg, one-arg, two-arg `function<...>` overloads; `vector<string>*` (ArityAny); `string*`; `optional<string>*`; `path*`; `optional<path>*`; templated `(T*, const T &)` (zero-arity, sets to literal value); templated `(I*)` and `(optional<I>*)` for integer types via `string2IntWithUnitPrefix<I>`. Stores `function<void(vector<string>)> fun` and `size_t arity = 0`.
  - public `using CompleterFun = void(AddCompletions &, size_t, std::string_view)`.
  - public `using CompleterClosure = std::function<CompleterFun>`.
  - public `struct Flag { using ptr = std::shared_ptr<Flag>; std::string longName; StringSet aliases; char shortName = 0; std::string description; std::string category; Strings labels; Handler handler; CompleterClosure completer; bool required = false; std::optional<ExperimentalFeature> experimentalFeature; size_t timesUsed = 0; }`.
  - protected `std::map<string, Flag::ptr> longFlags`, `std::map<char, Flag::ptr> shortFlags`, virtual `processFlag(pos, end)`.
  - public `struct ExpectedArg { std::string label; bool optional = false; Handler handler; CompleterClosure completer; }`.
  - protected `std::list<ExpectedArg> expectedArgs`, `std::list<ExpectedArg> processedArgs`, virtual `processArgs(args, finish)`, virtual `rewriteArgs(args, pos)` (default returns `pos`), `StringSet hiddenCategories`, virtual `checkArgs()`, virtual `initialFlagsProcessed()` (default no-op).
  - public `addFlag(Flag &&)`, `removeFlag(longName)`, `expectArgs(ExpectedArg &&)`, two `expectArg(label, dest, optional = false)` overloads (`string*`, `path*`), `expectArgs(label, vector<string>*)`.
  - public `static CompleterFun completePath`, `completeDir`.
  - virtual `toJSON()`.
  - friend `class MultiCommand`.
  - public `MultiCommand * parent = nullptr`.
  - public `RootArgs & getRoot()`.
- `struct Command : virtual public Args`
  - friend `class MultiCommand`.
  - virtual destructor; pure virtual `run() = 0`.
  - public `using Category = int`; `static constexpr Category catDefault = 0`.
  - virtual `experimentalFeature()`, virtual `category()` returning `catDefault` by default.
- `using Commands = std::map<std::string, fun<ref<Command>()>>`.
- `class MultiCommand : virtual public Args`
  - public `Commands commands`.
  - public `std::map<Command::Category, std::string> categories`.
  - public `std::optional<std::pair<std::string, ref<Command>>> command`.
  - constructor `(string_view commandName, const Commands &)`.
  - overrides `processFlag`, `processArgs`, `toJSON`.
  - public `enum struct AliasStatus { AcceptedShorthand, Deprecated }`.
  - public `struct AliasInfo { AliasStatus status; std::vector<std::string> replacement; }`.
  - public `std::map<std::string, AliasInfo> aliases`.
  - override `rewriteArgs`.
  - protected `commandName`, `aliasUsed = false`, override `checkArgs()`.
- `struct Completion { std::string completion; std::string description; auto operator<=>(const Completion &) const noexcept; }`.
- `class AddCompletions`
  - `enum class Type { Normal, Filenames, Attrs }`.
  - pure virtual `setType(Type)`, `add(completion, description = "")`.
  - virtual destructor (default).

### Functions
- `argvToStrings(argc, argv)`.
- `parseShebangContent(s)`.

### Type aliases
- `Args::CompleterFun` (using), `Args::CompleterClosure` (using), `Args::Flag::ptr` (using), `Command::Category` (using), `Commands` (using).

### Macros / globals
None.

---

## File: src/libutil/include/nix/util/args/root.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct Completions final : AddCompletions`
  - `std::set<Completion> completions`, `Type type = Type::Normal`.
  - overrides `setType`, `add(completion, description = "")`.
- `class RootArgs : virtual public Args`
  - protected `std::filesystem::path commandBaseDir = "."`.
  - public `parseCmdline(cmdline, allowShebang = false)`.
  - public `std::shared_ptr<Completions> completions`.
  - public `getCommandBaseDir() const override`.
  - protected friend `class Args`.
  - protected nested `struct DeferredCompletion { const CompleterClosure & completer; size_t n; std::string prefix; }`.
  - protected `std::vector<DeferredCompletion> deferredCompletions`.
  - protected `std::set<ExperimentalFeature> flagExperimentalFeatures`.
  - private `needsCompletion(string_view)`.

### Functions
None standalone.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/processes.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None new.

### Functions
- `Pid & Pid::operator=(Pid && other) noexcept` — swap-based move assignment using friend `swap`.
- `runProgram(RunOptions &&)` — runs `runProgram2` collecting stdout into a `StringSink`, catches `ExecError` to extract status, returns `pair<status, string>`.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/include/nix/util/processes.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- forward decls `struct Sink;`, `struct Source;`.
- `class Pid`
  - On Unix (private): `pid_t pid = -1`, `bool separatePG = false`, `int killSignal = SIGKILL`, `std::chrono::milliseconds killTimeout`, `std::thread killThread`.
  - On Windows (private): `AutoCloseFD pid = INVALID_DESCRIPTOR`.
  - public default ctor; deleted copy ctor + copy assignment; move ctor + move assignment.
  - Unix-only public ctor `Pid(pid_t)`, `operator=(pid_t)`, `operator pid_t()`; Windows-only `Pid(AutoCloseFD)`, `operator=(AutoCloseFD)`.
  - destructor.
  - `kill(allowInterrupts = true)`, `wait(allowInterrupts = true)`.
  - Unix-only `setSeparatePG(bool)`, `setKillSignal(int)`, `setKillTimeout(milliseconds)`, `release()`.
  - friend `void swap(Pid &, Pid &) noexcept` (inline definition).
- `struct ProcessOptions { std::string errorPrefix = ""; bool dieWithParent = true; bool runExitHandlers = false; bool allowVfork = false; int cloneFlags = 0; }`.
- `struct RunOptions`
  - `std::filesystem::path program`.
  - `bool lookupPath = true`.
  - `OsStrings args`.
  - Unix-only `std::optional<uid_t> uid`, `std::optional<uid_t> gid` (note: `gid` is typed `uid_t`).
  - `std::optional<path> chdir`.
  - `std::optional<OsStringMap> environment`.
  - `Sink * standardOut = nullptr`.
  - `bool mergeStderrToStdout = false`.
  - `bool isInteractive = false`.
- `class ExecError final : public CloneableError<ExecError, Error>` — public `int status`; templated varargs ctor `(status, args...)` forwarding to base.

### Functions
- `void killUser(uid_t)` — Unix only.
- `pid_t startProcess(fun<void()> processMain, const ProcessOptions & options = {})` — Unix only.
- `runProgram(path program, bool lookupPath = false, const OsStrings & args = {}, bool isInteractive = false)` — string-returning shell-backtick analogue.
- `runProgram(RunOptions &&)` — pair<status, string> output.
- `runProgram2(const RunOptions &)` — execute with full options.
- `statusToString(int)` — convert wait status.
- `statusOk(int)`.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/current-process.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `getMaxCPU()` — Linux: read cgroup `cpu.max`, parse `quota period` pair, return `ceil(quota / period)` if quota is not "max"; otherwise/elsewhere returns 0. Catches `Error` and routes through `ignoreExceptionInDestructor(lvlDebug)` (note: not actually a destructor — see observations).
- `ensureStackSizeAtLeast(stackSize)` — Unix only; reads `RLIMIT_STACK`, warns if hard limit is below desired (suppressed by `_NIX_TEST_NO_ENVIRONMENT_WARNINGS=1`), then raises `rlim_cur` up to `min(stackSize, rlim_max)` and stores the previous soft limit in `savedStackSize`; logs error on `setrlimit` failure.
- `restoreProcessContext(restoreMounts)` — Unix: `unix::restoreSignals()`. Linux: `restoreMountNamespace()` if `restoreMounts`. Unix: restores stack rlimit to `savedStackSize` if it was saved.
- `getSelfExe()` — function-local cached lookup; Linux/GNU read `/proc/self/exe`, Apple uses `_NSGetExecutablePath` (1024-byte buffer), FreeBSD uses `sysctl(KERN_PROC_PATHNAME)` (with FreeBSD-specific null-terminator strip). Other platforms return nullopt.

### Type aliases
None.

### Macros / globals
- `size_t savedStackSize = 0;` — file-scope (Unix only).

---

## File: src/libutil/include/nix/util/current-process.hh

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `std::chrono::microseconds getCpuUserTime()` — declaration only.
- `unsigned int getMaxCPU()`.
- `void ensureStackSizeAtLeast(size_t stackSize)` — Unix only (note: not thread-safe; comment recommends `std::call_once`).
- `void restoreProcessContext(bool restoreMounts = true)`.
- `std::optional<std::filesystem::path> getSelfExe()`.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/executable-path.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None new.

### Functions
- `ExecutablePath::load()` — reads `PATH` env via `getEnvOs(OS_STR("PATH"))` (defaulting to empty), parses with `parse`.
- `ExecutablePath::parse(path)` — wraps `parseAppend` over an empty starting path.
- `ExecutablePath::parseAppend(path)` — splits on `path_var_separator` via `basicSplitString`; empty entries map to `"."` (POSIX legacy).
- `ExecutablePath::render() const` — joins entries via `basicConcatStringsSep`, returning an `OsString`.
- `ExecutablePath::findName(exe, isExecutable)` — asserts `exe` contains no path separator; iterates `directories` and returns first `dir / exe` where `isExecutable` returns true (via `lexically_normal()`).
- `ExecutablePath::findPath(exe, isExecutable)` — full POSIX-spec lookup: if `exe.filename() == exe` (no directory part) delegates to `findName` (throws `ExecutableLookupError` on miss), else returns `exe` unchanged.

### Type aliases
None.

### Macros / globals
- `constexpr static const OsStringView path_var_separator{&ExecutablePath::separator, 1};` — file-scope view of the single-char separator.

---

## File: src/libutil/include/nix/util/executable-path.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `MakeError(ExecutableLookupError, Error);` — error class.
- `struct ExecutablePath`
  - `std::vector<std::filesystem::path> directories`.
  - `constexpr static const OsChar separator` (`L';'` on Win32 else `':'`).
  - `static parse(path)`, `parseAppend(path)`, `static load()`, `render()`, `findName(exe, isExecutable = isExecutableFileAmbient)`, `findPath(exe, isExecutable = isExecutableFileAmbient)`, defaulted `operator==`.
  - Comment notes a TODO to rename — the type also serves non-executable path lists.

### Functions
None standalone.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/environment-variables.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `getEnv(const std::string &)` — `getenv(key.c_str())`, returns nullopt for null, otherwise the string.
- `getEnvNonEmpty(const std::string &)` — calls `getEnv`, returns nullopt for `""`.
- `getEnvOsNonEmpty(const OsString &)` — same idea using `getEnvOs` and `OS_STR("")`.
- `clearEnv()` — iterates `getEnvOs()` and unsets each.
- `replaceEnv(const StringMap &)` — `clearEnv` then `setEnv` for each entry.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/include/nix/util/environment-variables.hh

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `std::optional<std::string> getEnv(const std::string & key)`.
- `std::optional<OsString> getEnvOs(const OsString & key)`.
- `OsStringMap getEnvOs()` — full env as map.
- `std::optional<std::string> getEnvNonEmpty(const std::string & key)`.
- `std::optional<OsString> getEnvOsNonEmpty(const OsString & key)`.
- `StringMap getEnv()` — full env as string map.
- `int unsetenv(const char *)` — Windows-only POSIX shim.
- `int setEnv(const char * name, const char * value)` — always overrides.
- `int setEnvOs(const OsString & name, const OsString & value)`.
- `int unsetEnvOs(const OsChar * name)`.
- `void clearEnv()`.
- `void replaceEnv(const StringMap & newEnv)`.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/include/nix/util/exec.hh

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `int execvpe(const OsChar * file0, const OsChar * const argv[], const OsChar * const envp[])` — portable shim (used unconditionally for consistency, replacing the GNU extension).

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/thread-pool.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None new.

### Functions
- `ThreadPool::ThreadPool(_maxThreads)` — defaults to `std::thread::hardware_concurrency()` (or 1 if zero); logs `debug("starting pool of %d threads", maxThreads - 1)`.
- `ThreadPool::~ThreadPool()` — calls `shutdown`.
- `ThreadPool::shutdown()` — sets `quit`, swaps out the workers vector under the lock, notifies all and joins.
- `ThreadPool::enqueue(work_t)` — adds work, possibly spawns a worker thread if `pending > workers + 1` and within `maxThreads`, signals one waiter; throws `ThreadPoolShutDown` if `quit` is set.
- `ThreadPool::process()` — sets `draining`, runs `doWork(true)` on the main thread, asserts `quit`, rethrows captured exception if any; on exception runs `shutdown()` first.
- `ThreadPool::doWork(mainThread)` — worker loop with `ReceiveInterrupts`; for non-main worker on Unix installs `unix::interruptCheck = []() { return (bool) quit; }`. After running each work item, decrements active count and either captures the first exception (signaling all workers via `quit + notify_all`) or silently drops `Interrupted`/`ThreadPoolShutDown` and warns on others. Idle wait exits when `quit` or when no active/pending and `draining`.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/include/nix/util/thread-pool.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `MakeError(ThreadPoolShutDown, Error);`.
- `class ThreadPool`
  - public ctor `ThreadPool(maxThreads = 0)`, destructor.
  - public `typedef fun<void()> work_t`.
  - public `enqueue(work_t)`, `process()`, `shutdown()`.
  - private `size_t maxThreads`.
  - private nested `struct State { std::queue<work_t> pending; size_t active = 0; std::exception_ptr exception; std::vector<std::thread> workers; bool draining = false; }`.
  - private `std::atomic_bool quit{false}`, `Sync<State> state_`, `std::condition_variable work`.
  - private `void doWork(bool mainThread)`.

### Functions
- `template<typename T> void processGraph(const std::set<T> & nodes, fun<std::set<T>(const T &)> getEdges, fun<void(const T &)> processNode)` — parallel processing of a partially ordered graph using a `Sync<Graph>` that holds `left`, `refs`, `rrefs`. Workers compute edge sets on demand; once a node is processed, all dependents whose `refs` become empty are enqueued. After `pool.process()` returns, throws `Error("graph processing incomplete (cyclic reference?)")` if anything is left.

### Type aliases
- `ThreadPool::work_t` (typedef inside class).

### Macros / globals
None.

---

## File: src/libutil/include/nix/util/sync.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `template<class T, class M, class WL, class RL, class CV> class SyncBase`
  - private `M mutex; T data;`.
  - public `using element_type = T`.
  - default ctor; ctor from `const T &`; noexcept ctor from `T &&`; variadic ctor `SyncBase(Ts &&...) requires { T{std::forward<Ts>(args)...} }`.
  - move ctor (noexcept) that locks the source and moves out the data.
  - nested `template<class L> class Lock`
    - protected `SyncBase * s`, `L lk` (acquires `s->mutex` in ctor), friend `SyncBase`.
    - deleted copy/move.
    - destructor.
    - `wait(cv)`, templated `wait(cv, pred)`, templated `wait_for(cv, duration)`, templated `wait_for(cv, duration, pred)`, templated `wait_until(cv, time_point)` — all assert `s` is non-null.
  - nested `struct WriteLock : Lock<WL>` — `using Lock<WL>::Lock`; `T * operator->()`, `T & operator*()`.
  - public `WriteLock lock()`.
  - nested `struct ReadLock : Lock<RL>` — pulls in ctor; `const T * operator->()`, `const T & operator*()`.
  - public `ReadLock readLock() const` (uses `const_cast` to acquire shared/read state).

### Functions
None standalone.

### Type aliases
- `template<class T> using Sync = SyncBase<T, std::mutex, std::unique_lock<std::mutex>, std::unique_lock<std::mutex>, std::condition_variable>;`.
- `template<class T> using SharedSync = SyncBase<T, std::shared_mutex, std::unique_lock<std::shared_mutex>, std::shared_lock<std::shared_mutex>, std::condition_variable>;`.

### Macros / globals
None.

---

## File: src/libutil/include/nix/util/async.hh

### Namespaces
- `nix`.
- `nix::asio = boost::asio` (namespace alias).

### Classes / structs / enums
None.

### Functions
- `template<typename T, std::invocable<Callback<T>> F, typename CompletionToken> auto callbackToAwaitable(F && initiate, CompletionToken && token)` — adapts a callback-based API to Asio completion tokens. Threads cancellation through an associated cancellation slot (which posts an `Interrupted("interrupted by user")` exception via `std::promise`), keeps the io_context alive via `make_work_guard`, uses an atomic flag to ensure single-shot delivery, and posts the resulting `std::future<T>` to the associated executor.
- `template<typename T, std::invocable<Callback<T>> F> asio::awaitable<T> callbackToAwaitable(F && initiate)` — coroutine wrapper using `asio::use_awaitable`; awaits the future and returns its value.
- `template<typename Range, typename F> asio::awaitable<void> forEachAsync(Range && range, const F & f)` — `co_spawn`s a coroutine per element on the current executor, accumulates the first thrown `exception_ptr`, posts handler resumption when all `pending` items have completed. Comments note this assumes single-threaded executor (strand) ownership.

### Type aliases
- `nix::asio` (namespace alias of `boost::asio`).

### Macros / globals
None.

---

## File: src/libutil/include/nix/util/signals.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `MakeError(Interrupted, BaseError);`.
- `struct InterruptCallback { virtual ~InterruptCallback() {}; }`.
- `struct ReceiveInterrupts;` — forward declaration only (definition is in `signals-impl.hh`, included at file end).

### Functions
- `static inline void setInterrupted(bool isInterrupted)` — declaration (no-op on Windows; impl in `signals-impl.hh`).
- `static inline bool getInterrupted()` — declaration.
- `static inline bool isInterrupted()` — declaration.
- `inline void checkInterrupt()` — declaration.
- `std::unique_ptr<InterruptCallback> createInterruptCallback(fun<void()> callback)` — register a callback for SIGINT (no-op on Windows).

### Type aliases
None.

### Macros / globals
- `#define NIX_SIG_MULTI_INT SIGTSTP` (FreeBSD; comment notes SIGUSR1 is used by bdwgc) or `SIGUSR1` (other Unix). Not defined on Windows.
- `#include "nix/util/signals-impl.hh"` at the bottom (provides inline implementations after the class declarations).

---

## File: src/libutil/include/nix/util/muxable-pipe.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct MuxablePipePollState`
  - Unix fields: `std::vector<struct pollfd> pollStatus`, `std::map<int, size_t> fdToPollStatus`.
  - Windows fields: `OVERLAPPED_ENTRY oentries[0x20] = {0}`, `ULONG removed`, `bool gotEOF = false`.
  - `void poll([HANDLE ioport,] std::optional<unsigned int> timeout)` — Windows version takes the I/O Completion Port handle.
  - `using CommChannel = Descriptor` (Unix) or `windows::AsyncPipe *` (Windows).
  - `void iterate(std::set<CommChannel> &, fun<void(Descriptor, string_view)> handleRead, fun<void(Descriptor)> handleEOF)`.

### Functions
None standalone.

### Type aliases
- `using MuxablePipe = Pipe;` (Unix) or `= windows::AsyncPipe;` (Windows).
- `MuxablePipePollState::CommChannel` (member alias).

### Macros / globals
None.

---

## File: src/libutil/users.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `getCacheDir()` — `NIX_CACHE_HOME` env, else Unix `unix::xdg::getCacheHome() / "nix"` / Windows `windows::known_folders::getLocalAppData() / "nix" / "cache"`.
- `getConfigDir()` — `NIX_CONFIG_HOME`, else Unix XDG config home / Windows roaming AppData (`/nix/config`).
- `getConfigDirs()` — list starting with `getConfigDir()`, then on Unix appends each entry of `unix::xdg::getConfigDirs()` joined with `/nix`.
- `getDataDir()` — `NIX_DATA_HOME`, else Unix XDG data home / Windows local AppData (`/nix/data`).
- `getStateDir()` — `NIX_STATE_HOME`, else Unix XDG state home / Windows local AppData (`/nix/state`).
- `createNixStateDir()` — calls `createDirs(getStateDir())` and returns the path.
- `expandTilde(path)` — replaces leading `~/` or `~` with `getHome() / suffix`; otherwise returns unchanged. Comment notes a TODO for `~user` expansion.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/include/nix/util/users.hh

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `std::string getUserName()` (declaration only; implementation lives in platform-specific source).
- `std::filesystem::path getHomeOf(uid_t)` — Unix only.
- `std::filesystem::path getHome()` — `$HOME` or passwd entry.
- `getCacheDir()`, `getConfigDir()`, `getConfigDirs()`, `getDataDir()`, `getStateDir()`.
- `createNixStateDir()`.
- `expandTilde(string_view)`.
- `bool isRootUser()` — true if uid 0 on Unix; comment notes always false on Windows currently.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/experimental-features.cc

### Namespaces
- `nix`.

### Classes / structs / enums
- `struct ExperimentalFeatureDetails { ExperimentalFeature tag; std::string_view name, description, trackingUrl; }`.

### Functions
- `parseExperimentalFeature(name)` — looks up feature by name via a function-local Meyers singleton `unique_ptr<ReverseXpMap>` reverse-built from `xpFeatureDetails`; returns `optional<ExperimentalFeature>`.
- `showExperimentalFeature(tag)` — asserts index in range, returns `name` field.
- `documentExperimentalFeatures()` — generates `nlohmann::json` map of name -> rendered docs (description plus trailing tracking-issue link).
- `parseFeatures(rawFeatures)` — parse strings to set, silently dropping unknowns (note: comment in header says "warning for", but implementation only inserts known ones — no warn here).
- `MissingExperimentalFeature::MissingExperimentalFeature(feature, reason)` — formats `"experimental Nix feature '%1%' is disabled%2%; add '--extra-experimental-features %1%' to enable it"` with `optionalBracket(" (", reason, ")")`, stores feature and reason.
- `operator<<(std::ostream &, const ExperimentalFeature &)` — emits name.
- `to_json(json &, const ExperimentalFeature &)` — sets the JSON value to the feature's name.
- `from_json(const json &, ExperimentalFeature &)` — string lookup; throws `Error` on unknown name.

### Type aliases
- `using ReverseXpMap = std::map<std::string_view, ExperimentalFeature>` (function-local).

### Macros / globals
- `constexpr size_t numXpFeatures = 1 + static_cast<size_t>(Xp::BLAKE3Hashes);` — count derived from last enumerator (with comment about avoiding silent merge conflicts).
- `constexpr std::array<ExperimentalFeatureDetails, numXpFeatures> xpFeatureDetails = {{ ... }};` — central feature table for: `CaDerivations`, `ImpureDerivations`, `Flakes`, `FetchTree`, `NixCommand`, `GitHashing`, `RecursiveNix`, `FetchClosure`, `AutoAllocateUids`, `Cgroups`, `DaemonTrustOverride`, `DynamicDerivations`, `ParseTomlTimestamps`, `ReadOnlyLocalStore`, `LocalOverlayStore`, `ConfigurableImpureEnv`, `MountedSSHStore`, `VerifiedFetches`, `PipeOperators`, `ExternalBuilders`, `BLAKE3Hashes`.
- `static_assert(... index == feature.tag ...)` enforces array order matches enum.

---

## File: src/libutil/include/nix/util/experimental-features.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `enum struct ExperimentalFeature` — values `CaDerivations`, `ImpureDerivations`, `Flakes`, `FetchTree`, `NixCommand`, `GitHashing`, `RecursiveNix`, `FetchClosure`, `AutoAllocateUids`, `Cgroups`, `DaemonTrustOverride`, `DynamicDerivations`, `ParseTomlTimestamps`, `ReadOnlyLocalStore`, `LocalOverlayStore`, `ConfigurableImpureEnv`, `MountedSSHStore`, `VerifiedFetches`, `PipeOperators`, `ExternalBuilders`, `BLAKE3Hashes`.
- `class MissingExperimentalFeature final : public CloneableError<MissingExperimentalFeature, Error>`
  - public `ExperimentalFeature missingFeature`, `std::string reason`.
  - constructor `MissingExperimentalFeature(feature, reason = "")`.
- `template<> struct json_avoids_null<ExperimentalFeature> : std::true_type` — JSON helper.

### Functions
- `parseExperimentalFeature(name)`, `showExperimentalFeature(tag)`.
- `documentExperimentalFeatures()`.
- `operator<<(std::ostream &, const ExperimentalFeature &)`.
- `parseFeatures(StringSet)`.
- `to_json(json &, const ExperimentalFeature &)`, `from_json(const json &, ExperimentalFeature &)`.

### Type aliases
- `using Xp = ExperimentalFeature;`.

### Macros / globals
None.

---

## File: src/libutil/unix-domain-socket.cc

### Namespaces
- `nix`.
- `nix::unix` (Unix-only `sendMessageWithFds`/`receiveMessageWithFds`).

### Classes / structs / enums
None new.

### Functions
- `createUnixDomainSocket()` — creates `PF_UNIX` `SOCK_STREAM` socket, with `SOCK_CLOEXEC` when available; on Unix follows up with `unix::closeOnExec(fdSocket.get())`.
- `createUnixDomainSocket(path, mode)` — creates socket, `bind`s, `chmod`s, then `listen(..., 100)`.
- `bindConnectProcHelper(operationName, operation, fd, path)` (file-static) — fills `sockaddr_un`. If path is too long it forks a helper (Unix) that `chdir`s into the parent directory and uses `baseNameOf` so the relative socket name fits in `sun_path`, communicating errno back through a pipe; on Windows it just throws. Otherwise calls `operation(fd, psaddr, sizeof(addr))` directly.
- `bind(Socket fd, path)` — `tryUnlink(path)` then `bindConnectProcHelper("bind", ::bind, ...)`.
- `connect(Socket fd, path)` — `bindConnectProcHelper("connect", ::connect, ...)`.
- `connect(path)` — convenience that creates socket + connects.
- `unix::sendMessageWithFds(sockfd, data, fds)` — `sendmsg` with `SCM_RIGHTS`. If `data` is empty and `fds` is non-empty asserts that the socket isn't `SOCK_STREAM`. Allocates an aligned cmsg buffer via `operator new(controlSize, cmsghdrAlign)`, fills with file descriptors, then loops `sendmsg` for partial writes (clearing ancillary data after the first successful send). do/while ensures `SOCK_DGRAM` zero-length still triggers `sendmsg`.
- `unix::receiveMessageWithFds(sockfd, data)` — `recvmsg` with `SCM_RIGHTS`. Stack-allocated `controlBuf[CMSG_SPACE(sizeof(int) * maxFds)]` where `maxFds` is 253 on Linux else 512. Uses `MSG_CMSG_CLOEXEC` when available; otherwise calls `closeOnExec` on each received fd. Throws `EndOfFile` only when zero bytes received with no truncation and no ancillary data; asserts no `MSG_CTRUNC` truncation.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/include/nix/util/unix-domain-socket.hh

### Namespaces
- `nix`.
- `nix::unix` (Unix-only block).

### Classes / structs / enums
- `struct unix::ReceivedMessage { size_t bytesReceived; std::vector<AutoCloseFD> fds; }` — returned by `receiveMessageWithFds`.

### Functions
- `AutoCloseFD createUnixDomainSocket()`.
- `AutoCloseFD createUnixDomainSocket(const path &, mode_t)`.
- `void bind(Socket fd, const path &)`.
- `void connect(Socket fd, const path &)`.
- `AutoCloseFD connect(const path &)`.
- `unix::sendMessageWithFds(sockfd, data, fds)` — Unix only.
- `unix::receiveMessageWithFds(sockfd, data)` — Unix only.

### Type aliases
None.

### Macros / globals
None.

---

## File: src/libutil/util.cc

### Namespaces
- `nix`.

### Classes / structs / enums
None.

### Functions
- `initLibUtil()` — runs `throwExceptionSelfCheck()` inside try/catch and asserts the catch fired (verifies linker emitted exception tables); calls `sodium_init()` and throws `Error` on failure.
- `stringsToCharPtrs(const Strings &)` — null-terminated `char*` vector built from contained strings (note: casts away constness from `c_str()`; comment notes lifetime requirement).
- `chomp(string_view)` — trim trailing whitespace (spaces/`\n`/`\r`/`\t`).
- `trim(string_view, whitespace)` — trim leading and trailing.
- `replaceStrings(s, from, to)` — replaces all occurrences in place; no-op for empty `from`.
- `rewriteStrings(s, rewrites)` — apply a `StringMap` of replacements (skipping identity entries).
- `template<class N> string2Int(string_view)` — `boost::lexical_cast<N>(data, size)`; rejects negative for unsigned types; returns nullopt on bad cast.
- Explicit instantiations for `unsigned char`, `unsigned short`, `unsigned int`, `unsigned long`, `unsigned long long`, `signed char`, `signed short`, `signed int`, `signed long`, `signed long long`.
- `template<class N> string2Float(string_view)` — same `boost::lexical_cast` pattern.
- Float instantiations for `double`, `float`.
- `getSizeUnit(int64_t)` — find largest unit such that `abs(value) <= 1024`.
- `getCommonSizeUnit(initializer_list<int64_t>)` — unit only if all values share it; asserts non-empty list.
- `renderSizeWithoutUnit(value, unit, align)` — format number; `power = max(1, to_underlying(unit))`, denominator `pow(1024, power)`, `fmt("%6.1f"|"%.1f", ...)`.
- `getSizeUnitSuffix(unit)` — switch via `NIX_UTIL_SIZE_UNITS` macro returning the suffix char.
- `renderSize(value, align)` — composes `renderSizeWithoutUnit` and `getSizeUnitSuffix` to produce `"%s %ciB"`.
- `hasPrefix(s, prefix)`, `hasSuffix(s, suffix)`.
- `toLower(s)` — in-place per-char `std::tolower`.
- `escapeShellArgAlways(s)` — single-quote shell-quote, escaping interior `'` as `'\''`.
- `ignoreExceptionInDestructor(lvl)` — robust catch-everything wrapper (rethrowing inner exception inside an outer try/catch to avoid leaking from the destructor); prints via `printMsg(lvl, ...)`.
- `ignoreExceptionExceptInterrupt(lvl)` — catch all but rethrows `Interrupted`; prints `Error` and `std::exception` via `printMsg`.
- `stripIndentation(string_view)` — strip common leading whitespace.
- `getLine(string_view)` — split on first newline; trims trailing `\r` on the line if present.

### Type aliases
None.

### Macros / globals
- `static const int64_t conversionNumber = 1024;` — file-scope binary unit base.
- `#ifdef NDEBUG #error "Nix may not be built with assertions disabled (i.e. with -DNDEBUG)." #endif` — compile-time guard.

---

## File: src/libutil/include/nix/util/util.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `MakeError(FormatError, Error);`.
- `enum class SizeUnit { Base, Kilo, Mega, Giga, Tera, Peta, Exa, Zetta, Yotta }` — defined via `NIX_UTIL_SIZE_UNITS` X-macro.
- `template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; }` plus deduction guide `overloaded(Ts...) -> overloaded<Ts...>` — `std::visit` helper.
- `template<typename T> class Callback;` (forward declaration).
- `template<typename T> struct MaintainCount` — RAII counter; fields `T & counter`, `long delta`; constructor adds delta, destructor subtracts; deleted copy/move.

### Functions
- `void initLibUtil()`.
- `std::vector<char *> stringsToCharPtrs(const Strings &)`.
- `template<class... Parts> auto concatStrings(Parts &&...) -> enable_if_t<(... && convertible_to_string_view), string>` — concatenate via `concatStringsSep({}, views[])`.
- `inline std::string quoteString(string_view, char quote = '\'')`.
- `template<class C> Strings quoteStrings(const C &, char quote = '\'')`.
- `inline Strings quoteFSPaths(const std::set<path> &, char quote = '\'')`.
- `chomp(string_view)`, `trim(string_view, whitespace = " \n\r\t")` — declarations.
- `replaceStrings(s, from, to)`, `rewriteStrings(s, rewrites)` — declarations.
- `template<class N> std::optional<N> string2Int(string_view)` (declaration only; defined in `.cc`).
- `template<class N> N string2IntWithUnitPrefix(string_view)` — strips one trailing alpha suffix, recognized values `K/M/G/T` mapping to `1<<10`/`<<20`/`<<30`/`<<40`; throws `UsageError` for unrecognized suffixes or non-integers.
- `getSizeUnit`, `getCommonSizeUnit`, `renderSizeWithoutUnit(value, unit, align = false)`, `getSizeUnitSuffix`, `renderSize(value, align = false)`.
- `template<class N> std::optional<N> string2Float(string_view)`.
- `template<typename T> T readLittleEndian(unsigned char * p)` — `memcpy` into the destination, then byteswap with `__builtin_bswapNN` on big-endian hosts; `static_assert(false)` for non-`uint16/32/64_t` on big-endian.
- `hasPrefix(s, prefix)`, `hasSuffix(s, suffix)`, `toLower(s)`.
- `escapeShellArgAlways(s)`.
- `ignoreExceptionInDestructor(lvl = lvlError)`, `ignoreExceptionExceptInterrupt(lvl = lvlError)`.
- `stripIndentation(s)`, `getLine(s)`.
- `template<class T> const T * get(const std::optional<T> &)`, `T * get(std::optional<T> &)`.
- `template<class T, K> const T::mapped_type * get(const T & map, const K & key)`, non-const overload.
- `template<class T, K> T::mapped_type * get(T && map, const K & key) = delete` — forbid temporaries.
- `template<class T> std::optional<T::mapped_type> getConcurrent(const T & map, const T::key_type & key)` — uses `boost::concurrent_flat_map::cvisit`.
- `template<class T, K> const T::mapped_type & getOr(T & map, const K & key, const T::mapped_type & default)` plus deleted rvalue overload.
- `template<class T> std::optional<T::value_type> remove_begin(T &)` — pop and return the first element of a generic container.
- `template<class T> std::optional<T::value_type> pop(T &)` — pop front of a queue-like container (uses `c.front()` and `c.pop()`).
- `template<class C, T> void append(C &, std::initializer_list<T>)` — TODO comment about replacement by C++23 `append_range`.
- `template<std::ranges::viewable_range R> constexpr auto enumerate(R && range)` — implemented via `std::views::zip(views::iota(size_t{0}), range)`.
- `inline std::string operator+(const std::string &, std::string_view)`.
- `inline std::string operator+(std::string &&, std::string_view)`.
- `inline std::string operator+(std::string_view, const char *)`.

### Type aliases
None additional.

### Macros / globals
- `#define NIX_UTIL_SIZE_UNITS` — X-macro listing size unit name + suffix entries (`Base 'K'`, `Kilo 'K'`, `Mega 'M'`, `Giga 'G'`, `Tera 'T'`, `Peta 'P'`, `Exa 'E'`, `Zetta 'Z'`, `Yotta 'Y'`).
- `#define NIX_UTIL_DEFINE_SIZE_UNIT(name, suffix)` — driven into the `enum class SizeUnit` body, the `sizeUnits` array, and the `getSizeUnitSuffix` switch.
- `constexpr inline auto sizeUnits = std::to_array<SizeUnit>({...});`.
- `constexpr char treeConn[] = "├───";`, `treeLast[] = "└───";`, `treeLine[] = "│   ";`, `treeNull[] = "    ";` — tree drawing constants.

---

## File: src/libutil/include/nix/util/types.hh

### Namespaces
- `nix`.

### Classes / structs / enums
- `template<typename T> struct OnStartup { OnStartup(T && t) { t(); } }` — runs `t()` from constructor for static-init side effects.
- `template<typename T> struct Explicit { T t; bool operator==(...) = default; bool operator<(...) const; }` — wraps `T` to prevent implicit casts (e.g. `char*`-to-`bool`).
- `class BackedStringView`
  - private `std::variant<std::string, std::string_view> data`.
  - private nested `class Ptr` holding a `std::string_view` and exposing `const std::string_view * operator->()` (used so the outer `operator->` can return a pointer into a temporary view).
  - constructors from `std::string &&`, `std::string_view`, and templated `(const char (&lit)[N])`.
  - deleted copy ctor / copy assign; defaulted move ctor + move assign.
  - `isOwned() const`, `toOwned() &&` (move-out variant), `operator*() const` returning `string_view`, `Ptr operator->() const`.

### Functions
None.

### Type aliases
- `typedef std::list<std::string> Strings;`.
- `using StringMap = std::map<std::string, std::string, std::less<>>;`.
- `using StringPairs = StringMap;`.
- `using StringSet = std::set<std::string, std::less<>>;`.
- `typedef std::vector<std::pair<std::string, std::string>> Headers;`.

### Macros / globals
None.

---

## Cross-file observations

- **OS-aware string helpers**: `OS_STR(...)`, `OsString`, `OsStringView`, `OsStringMap`, `OsChar` show up in `executable-path`, `unix-domain-socket`, `environment-variables`, and `users` headers; the `OsString` API parallels the regular `std::string` API. New code touching env vars / paths should reuse these instead of adding new platform branches.
- **Setting<T> serialization scaffolding**: the same template-specialization pattern (`parse`, `to_string`, `appendOrSet`, `convertToArg`) is replicated for many `T` in `configuration.cc`. The list (`std::list<path>`, `Strings`, `StringSet`, `std::set<path>`, `std::set<ExperimentalFeature>`, `StringMap`) plus the `trait::appendable` specializations in `config-impl.hh` plus the `NIX_DECLARE_CONFIG_SERIALISER` macro plus the explicit `template class BaseSetting<...>` instantiations form four near-parallel registers; folding through one X-macro list (similar to `NIX_UTIL_SIZE_UNITS`) would remove drift risk.
- **Logging glue duplication**: `SimpleLogger`, `JSONLogger`, and `TeeLogger` independently implement the same `startActivity / stopActivity / result / log / logEI / writeToStdout / ask / setPrintBuildLogs` shape. The Tee/JSON variants both serialize through `showErrorInfo` + `loggerSettings.showTrace.get()`. A common base or visitor pattern could deduplicate.
- **Error/HintFmt construction**: `BaseError`, `SystemError`, `SysError`, `WinError`, `ExecError`, and `MissingExperimentalFeature` all replicate "varargs ctor that builds `HintFmt` and forwards to base". The `DisambigHintFmt`/`DisambigVarArgs` tag idiom in `SystemError` is replicated in derived classes (`SysError`, `WinError`); `captureErrno` and `captureLastError` are nearly identical. A single helper (`HintFmtCtor`) or a CRTP helper could displace `MakeError`.
- **Cloneable exception macro vs direct CRTP**: the `MakeError(...)` macro appears in `error.hh` (Error, UsageError, UnimplementedError), `thread-pool.hh` (ThreadPoolShutDown), `experimental-features.hh` (none — it actually uses direct `CloneableError<...>` derivation for `MissingExperimentalFeature`), `executable-path.hh` (ExecutableLookupError), `signals.hh` (Interrupted), `util.hh` (FormatError). `ExecError` (`processes.hh`) and `MissingExperimentalFeature` (`experimental-features.hh`) bypass the macro and inherit directly from `CloneableError<...>` because they need extra fields (`status`, `missingFeature`, `reason`); worth checking whether a CRTP base could subsume both styles.
- **`getEnv`/`getEnvOs` parallel API**: every function (`getEnv`, `getEnvNonEmpty`, `setEnv`, `unsetenv`, `clearEnv`, `replaceEnv`) has an `Os` twin (declared in `environment-variables.hh`). `clearEnv()` already iterates the `Os` API and does not need a Unix/Windows split. The same applies to the `XDG`-style directory lookups in `users.cc`, which switch on `_WIN32` per call.
- **`Sync<T>` patterns**: `ThreadPool::state_`, `JSONLogger::_state`, and `processGraph::graph_` all use `Sync<T>` with similar `lock()` / `wait(work)` / `notify_*` idioms. `signals.hh` and `async.hh` both rely on `Interrupted` exceptions and the legacy `ReceiveInterrupts` RAII guard.
- **`fun<...>`** (from `nix/util/fun.hh`) is used pervasively (`thread-pool.hh`, `signals.hh`, `processes.hh`, `logging.hh`, `error.hh`, `muxable-pipe.hh`); newer Asio code in `async.hh` mixes `fun<>` and `std::function`/`std::invocable`. If anything migrates to `std::function` it should converge on one or the other.
- **Configuration / GlobalConfig duplication**: `Config::set`/`getSettings`/`toJSON`/`toKeyValue`/`convertToArgs` and `GlobalConfig::set`/`getSettings`/`toJSON`/`toKeyValue`/`convertToArgs` follow identical loop shapes. A subtle behavior difference: `Config::toKeyValue` only emits **alias** entries (non-aliases), while `GlobalConfig::toKeyValue` calls `globalConfig.getSettings(...)` and emits all entries via the merged setting map. With a CRTP base or explicit composition, the GlobalConfig wrapper could simply iterate registered configs without redefining each method.
- **Position/code-line formatting**: `printCodeLines`, `unreachable`, and the JSON serialization in `to_json(json, shared_ptr<const Pos>)` in `logging.cc` all assemble line/column/file info; format strings differ. Checking whether the JSON form and the textual one in `printCodeLines` could share a normaliser is worthwhile.
- **Args/MultiCommand `processFlag`/`processArgs`/`checkArgs`/`toJSON` overrides** all chain to base + delegate to selected sub-command. A pluggable visitor would also make `MultiCommand` and `Args::Handler` constructor flexibility easier to extend.
- **Threading + signal interplay**: `ReceiveInterrupts` is constructed at the top of `handleExceptions` (error.cc) and inside `ThreadPool::doWork` (thread-pool.cc). The interrupt-aware coroutine path in `async.hh` (`Interrupted("interrupted by user")` injected via the cancellation slot) parallels the legacy thread-pool interrupt path. Future async refactor should consolidate.
- **Path-string round-tripping**: `AbsolutePath`, `BaseSetting<filesystem::path>`, `BaseSetting<AbsolutePath>`, `BaseSetting<list<path>>`, `BaseSetting<set<path>>` each implement nearly identical `parse`/`to_string`. Possible candidate for a generic path-list helper.
- **`current-process.cc` exception handling oddity**: `getMaxCPU` catches `Error` and routes through `ignoreExceptionInDestructor(lvlDebug)`, but it is not actually a destructor. Per the documentation comment on that helper in `util.hh`, callers outside destructors should usually use `ignoreExceptionExceptInterrupt`. Worth double-checking the intent here.
- **Comment and TODO debt**: there is an explicit FIXME against `experimentalFeatureSettings` being a global in `configuration.hh`, a TODO about `ExecutablePath` having a misleading name in `executable-path.hh`, a TODO about renaming `SysError` to `PosixError` in `error.hh`, and a TODO in `users.cc` about `~user` expansion. Worth picking up alongside any refactor.
