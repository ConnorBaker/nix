# Inventory — Shard 15: libflake + libmain

## File: src/libflake/config.cc

### Namespaces
- `nix::flake` — namespace, file path `src/libflake/config.cc`, scope for the `ConfigFile::apply` implementation and the trusted-list helpers.

### Classes / structs / enums
(none defined; this file only implements `ConfigFile::apply` declared in `flake.hh`)

### Functions
- `trustedListPath` — free function (static), file path `src/libflake/config.cc`, returns `getDataDir() / "trusted-settings.json"`.
- `readTrustedList` — free function (static), file path `src/libflake/config.cc`, reads and JSON-parses the trusted-settings file, returning empty `TrustedList` if the file is missing.
- `writeTrustedList` — free function (static), file path `src/libflake/config.cc`, ensures the parent directory exists then writes the JSON-serialized trust map.
- `ConfigFile::apply` — member function, file path `src/libflake/config.cc`, applies a flake's `nixConfig` to the global `Config` subject to a hardcoded whitelist (`bash-prompt`, `bash-prompt-prefix`, `bash-prompt-suffix`, `flake-registry`, `commit-lock-file-summary`, `commit-lockfile-summary`); for non-whitelisted keys when `acceptFlakeConfig` is false, prompts via `logger->ask`, persists user choices via `writeTrustedList`, and warns/skips untrusted entries.

### Type aliases
- `TrustedList` — alias (`typedef`), file path `src/libflake/config.cc`, `std::map<std::string, std::map<std::string, bool>>`; setting name -> value -> allow/ignore.

### Macros / globals
(none)

---

## File: src/libflake/flake-primops.cc

### Namespaces
- `nix::flake::primops` — namespace, file path `src/libflake/flake-primops.cc`, holds the flake-related primops.

### Classes / structs / enums
(none)

### Functions
- `primops::getFlake` — free function, file path `src/libflake/flake-primops.cc`, returns a `PrimOp` named `__getFlake` whose closure captures `Settings`, sets up `LockFlags`, and dispatches on `nPath` vs string argument; for path-style store-resident inputs, looks them up in the `storeFS` mount before falling back to `lockFlake` + `callFlake`; rejects unlocked refs in pure eval.
- `prim_parseFlakeRef` — free function (static), file path `src/libflake/flake-primops.cc`, body of the `__parseFlakeRef` primop; parses a flakeref string via `nix::parseFlakeRef`, then translates the resulting attrs into a Nix attrset (string/uint64/Explicit<bool>).
- `prim_flakeRefToString` — free function (static), file path `src/libflake/flake-primops.cc`, body of the `__flakeRefToString` primop; converts a Nix attrset into `fetchers::Attrs` with `nInt`/`nBool`/`nString` dispatch (rejecting negatives), then calls `FlakeRef::fromAttrs(...).to_string()`.

### Type aliases
(none)

### Macros / globals
- `primops::parseFlakeRef` — global `nix::PrimOp`, file path `src/libflake/flake-primops.cc`, registers `__parseFlakeRef` with documentation and `Xp::Flakes`.
- `primops::flakeRefToString` — global `nix::PrimOp`, file path `src/libflake/flake-primops.cc`, registers `__flakeRefToString` with documentation and `Xp::Flakes`.

---

## File: src/libflake/flake.cc

### Namespaces
- `nix` — namespace, file path `src/libflake/flake.cc`.
- `nix::flake` — namespace, file path `src/libflake/flake.cc`, scope for flake-evaluation helpers.

### Classes / structs / enums
- `OverrideTarget` — struct (local to `lockFlake`), file path `src/libflake/flake.cc`, fields: `FlakeInput input`, `SourcePath sourcePath`, `std::optional<InputAttrPath> parentInputAttrPath`. Records an override candidate while the lock-file resolver walks the dependency tree.

### Functions
- `forceTrivialValue` — free function (static), file path `src/libflake/flake.cc`, forces a thunk if it is trivial.
- `expectType` — free function (static), file path `src/libflake/flake.cc`, throws if a forced value has the wrong `ValueType`.
- `parseFlakeInputs` — free function (static, forward + definition), file path `src/libflake/flake.cc`, walks an `inputs` attrset and produces a `(std::map<FlakeId, FlakeInput>, fetchers::Attrs)` pair (`selfAttrs` only when `allowSelf` is true; rejects `self` otherwise).
- `parseFlakeInputAttr` — free function (static), file path `src/libflake/flake.cc`, copies one attribute (string/bool/non-negative int, or `publicKeys` JSON-stringified under `Xp::VerifiedFetches`) from a Nix attr into a fetcher `Attrs`.
- `parseFlakeInput` — free function (static), file path `src/libflake/flake.cc`, parses a single flake input attrset producing a `FlakeInput` with `ref`, `isFlake`, `follows`, `overrides`; supports `url`, `flake`, `inputs`, `follows`, plus generic attrs forwarded through `parseFlakeInputAttr`; converts path-typed `url` values relative to `flakeDir`.
- `readFlake` — free function (static), file path `src/libflake/flake.cc`, evaluates `flake.nix`, extracts `description`, `inputs`/`selfAttrs`, `outputs` formals, `nixConfig` settings (string/path/int/bool/list-of-string), and rejects unsupported attrs.
- `applySelfAttrs` — free function (static), file path `src/libflake/flake.cc`, applies whitelisted `self` attrs (`submodules`, `lfs`) onto a `FlakeRef` clone.
- `getFlake` (static four-arg) — free function (static), file path `src/libflake/flake.cc`, fetches a lazy tree via `state.inputCache->getAccessor`, parses `flake.nix`, refetches when `selfAttrs` change inputs (clearing `narHash`), and re-parses from the mounted store path.
- `getFlake` (public three-arg) — free function, file path `src/libflake/flake.cc`, public wrapper calling the static four-arg form with empty `lockRootAttrPath`.
- `readLockFile` — free function (static), file path `src/libflake/flake.cc`, reads a lock file from a `SourcePath` returning empty `LockFile` if missing.
- `lockFlake` (five-arg core, takes `Flake` by value) — free function, file path `src/libflake/flake.cc`, requires `Xp::Flakes`; declares the local `OverrideTarget` struct and a self-recursive `computeLocks` lambda; resolves `inputOverrides`, `inputUpdates`, `follows`, and circular-import guards; emits warnings for unused `--override-input`/`--update-input` flags; validates `follows` via `LockFile::check`; writes/commits the lock file when configured and re-fetches the top flake when committing.
- `lockFlake(const Settings&, EvalState&, const FlakeRef&, const LockFlags&)` — free function, file path `src/libflake/flake.cc`, FlakeRef overload that fetches the flake first via `getFlake` then dispatches to the core overload.
- `lockFlake(const Settings&, EvalState&, const SourcePath&, const LockFlags&)` — free function, file path `src/libflake/flake.cc`, SourcePath overload that builds a fake `flake:get-flake` ref then calls `readFlake` and dispatches to the core overload.
- `makeInternalFS` — free function (static), file path `src/libflake/flake.cc`, builds an in-memory `MemorySourceAccessor` containing the embedded `call-flake.nix` (from `call-flake.nix.gen.hh`) with display name `«flakes-internal»`.
- `requireInternalFile` — free function (static), file path `src/libflake/flake.cc`, evaluates a file from `internalFS` using `state.evalFile`, returning the value pointer.
- `callFlake` — free function, file path `src/libflake/flake.cc`, requires `Xp::Flakes`; serializes the lock file, builds an overrides bindings keyed by `keyMap` with `sourceInfo`+`dir`, then invokes `call-flake.nix` with `(locks, overrides, fetchFinalTree)` to populate `vRes`.
- `LockedFlake::getFingerprint` — member function, file path `src/libflake/flake.cc`, hashes the locked input fingerprint together with subdir, lock-file string, and (for `revCount`) either `;hasRevCount` for lazy values or `;revCount=N` for concrete values, plus `;lastModified=N`, producing a SHA-256 fingerprint for caching.
- `Flake::~Flake` — destructor (default body), file path `src/libflake/flake.cc`.
- `openEvalCache` — free function, file path `src/libflake/flake.cc`, returns or creates a cached `eval_cache::EvalCache` keyed by the locked-flake fingerprint (only when `useEvalCache && pureEval`); the lazy `rootLoader` calls `callFlake` and forces `outputs`.

### Type aliases
(none new — uses `FlakeInputs`, `InputAttrPath`, etc. declared elsewhere)

### Macros / globals
- `internalFS` — file-scope static `ref<SourceAccessor>`, file path `src/libflake/flake.cc`, single shared in-memory accessor created via `makeInternalFS`.

---

## File: src/libflake/flakeref.cc

### Namespaces
- `nix` — namespace, file path `src/libflake/flakeref.cc`, scope of FlakeRef parsing free functions and the `flakeIdRegex` global.

### Classes / structs / enums
(none defined; this file implements members of `FlakeRef` declared in `flakeref.hh`)

### Functions
- `FlakeRef::to_string` — member function, file path `src/libflake/flakeref.cc`, serializes input as a URL adding `dir=<subdir>` query when non-empty.
- `FlakeRef::toAttrs` — member function, file path `src/libflake/flakeref.cc`, returns `input.toAttrs()` with `dir` added when `subdir` is non-empty.
- `operator<<(std::ostream&, const FlakeRef&)` — free function, file path `src/libflake/flakeref.cc`, streams `to_string()`.
- `FlakeRef::resolve` — member function, file path `src/libflake/flakeref.cc`, looks up the input via `lookupInRegistries` and returns a new `FlakeRef` (preferring the registry's `dir` extra-attr, falling back to the current `subdir`).
- `parseFlakeRef` — free function, file path `src/libflake/flakeref.cc`, calls `parseFlakeRefWithFragment` and rejects a non-empty fragment.
- `fromParsedURL` — free function (static), file path `src/libflake/flakeref.cc`, extracts `dir` and fragment from a `ParsedURL`, then constructs a `FlakeRef` via `fetchers::Input::fromURL`; returns `(FlakeRef, fragment)`.
- `parsePathFlakeRefWithFragment` — free function, file path `src/libflake/flakeref.cc`, regex-splits a path-style URL (`pathFlakeRegex`); decodes query/fragment; absolutizes against `baseDir`; corrects accidental `flake.nix` paths to their parent; searches upward for `flake.nix`/`.git` (rejecting filesystem-boundary crossings); produces either a `git+file:` (with `shallow=1` if `.git/shallow` exists) or `path:` `FlakeRef` plus its fragment.
- `parseFlakeIdRef` — free function (static), file path `src/libflake/flakeref.cc`, recognizes `<flake-id>(/(<ref>(/<rev>)?))?(#<fragment>)?` short form via `flakeRegex` built from `flakeIdRegexS`, `refAndOrRevRegex`, `fragmentRegex`.
- `parseURLFlakeRef` — free function, file path `src/libflake/flakeref.cc`, attempts `parseURL` (lenient) and absolutizes `path:`/`git+file:` URLs relative to `baseDir`; returns nullopt on `BadURL`.
- `parseFlakeRefWithFragment` — free function, file path `src/libflake/flakeref.cc`, dispatcher that tries `parseFlakeIdRef`, then `parseURLFlakeRef`, falling back to `parsePathFlakeRefWithFragment`.
- `FlakeRef::fromAttrs` — static member function, file path `src/libflake/flakeref.cc`, removes `dir` from a copy of attrs, builds the `Input`, and re-attaches `dir` as `subdir`.
- `FlakeRef::lazyFetch` — member function, file path `src/libflake/flakeref.cc`, returns the `SourceAccessor` and a re-locked `FlakeRef` (subdir preserved).
- `FlakeRef::canonicalize` — member function, file path `src/libflake/flakeref.cc`, drops a redundant `dir=` query parameter from the `url` attr so old/new lock-file `original` fields match.
- `parseFlakeRefWithFragmentAndExtendedOutputsSpec` — free function, file path `src/libflake/flakeref.cc`, splits `^outputs` from the URL via `ExtendedOutputsSpec::parse` then defers to `parseFlakeRefWithFragment`.

### Type aliases
(none)

### Macros / globals
- `subDirElemRegex` — file-scope `const static std::string` (inside `#if 0`), file path `src/libflake/flakeref.cc`, dormant subdir element regex.
- `subDirRegex` — file-scope `const static std::string` (inside `#if 0`), file path `src/libflake/flakeref.cc`, dormant subdir regex.
- `flakeIdRegex` — global `std::regex`, file path `src/libflake/flakeref.cc`, compiled from `flakeIdRegexS` declared in the header.

---

## File: src/libflake/include/nix/flake/flake-primops.hh

### Namespaces
- `nix::flake::primops` — namespace, file path `src/libflake/include/nix/flake/flake-primops.hh`, public surface of flake primops.

### Classes / structs / enums
(none)

### Functions
- `primops::getFlake` — free function declaration, file path `src/libflake/include/nix/flake/flake-primops.hh`, returns a `__getFlake` PrimOp bound to the supplied `Settings`.

### Type aliases
(none)

### Macros / globals
- `primops::parseFlakeRef` — extern global `nix::PrimOp`, file path `src/libflake/include/nix/flake/flake-primops.hh`, definition lives in `flake-primops.cc`.
- `primops::flakeRefToString` — extern global `nix::PrimOp`, file path `src/libflake/include/nix/flake/flake-primops.hh`, definition lives in `flake-primops.cc`.

---

## File: src/libflake/include/nix/flake/flake.hh

### Namespaces
- `nix` — namespace, file path `src/libflake/include/nix/flake/flake.hh`.
- `nix::flake` — namespace, file path `src/libflake/include/nix/flake/flake.hh`.

### Classes / structs / enums
- `FlakeInput` — struct, file path `src/libflake/include/nix/flake/flake.hh`, fields: `std::optional<FlakeRef> ref`, `bool isFlake = true` (true = process flake to get outputs; false = static source path), `std::optional<InputAttrPath> follows`, `FlakeInputs overrides`.
- `ConfigFile` — struct, file path `src/libflake/include/nix/flake/flake.hh`, holds parsed `nixConfig`; nested `using ConfigValue = std::variant<std::string, int64_t, Explicit<bool>, std::vector<std::string>>`; field `std::map<std::string, ConfigValue> settings`; method `apply(const Settings&)`.
- `Flake` — struct, file path `src/libflake/include/nix/flake/flake.hh`, fields: `FlakeRef originalRef`, `FlakeRef resolvedRef`, `FlakeRef lockedRef`, `SourcePath path`, `bool forceDirty = false`, `std::optional<std::string> description`, `FlakeInputs inputs`, `fetchers::Attrs selfAttrs`, `ConfigFile config`; declared destructor `~Flake()` and inline method `lockFilePath()` returning `path.parent()/"flake.lock"`.
- `LockedFlake` — struct, file path `src/libflake/include/nix/flake/flake.hh`, fields: `Flake flake`, `LockFile lockFile`, `std::map<ref<Node>, SourcePath> nodePaths`; method `getFingerprint(Store&, const fetchers::Settings&) const -> std::optional<Fingerprint>`.
- `LockFlags` — struct, file path `src/libflake/include/nix/flake/flake.hh`, flags driving `lockFlake`. Fields: `bool recreateLockFile = false`, `bool updateLockFile = true`, `bool writeLockFile = true`, `bool failOnUnlocked = false`, `std::optional<bool> useRegistries = std::nullopt`, `bool applyNixConfig = false`, `bool allowUnlocked = true`, `bool commitLockFile = false`, `std::optional<SourcePath> referenceLockFilePath`, `std::optional<std::filesystem::path> outputLockFilePath`, `std::map<NonEmptyInputAttrPath, FlakeRef> inputOverrides`, `std::set<NonEmptyInputAttrPath> inputUpdates`.

### Functions
- `getFlake(EvalState&, const FlakeRef&, fetchers::UseRegistries)` — free function, file path `src/libflake/include/nix/flake/flake.hh`, fetches and parses a flake.
- `lockFlake(const Settings&, EvalState&, const FlakeRef&, const LockFlags&)` — free function, file path `src/libflake/include/nix/flake/flake.hh`, computes the in-memory lock file (and optionally writes it) starting from a flake reference.
- `lockFlake(const Settings&, EvalState&, const SourcePath&, const LockFlags&)` — free function, file path `src/libflake/include/nix/flake/flake.hh`, alternate entry point keyed off a flake directory rather than a flake-ref.
- `callFlake(EvalState&, const LockedFlake&, Value&)` — free function, file path `src/libflake/include/nix/flake/flake.hh`, invokes `call-flake.nix` to materialize the flake outputs.
- `openEvalCache(EvalState&, ref<const LockedFlake>)` — free function, file path `src/libflake/include/nix/flake/flake.hh`, returns/creates the per-flake `eval_cache::EvalCache`.
- `prim_fetchFinalTree(EvalState&, const PosIdx, Value**, Value&)` — free function (declared in `nix` namespace, outside `flake`), file path `src/libflake/include/nix/flake/flake.hh`, fetchTree-like primop that treats the input as final.

### Type aliases
- `FlakeInputs` — alias (`typedef`), file path `src/libflake/include/nix/flake/flake.hh`, `std::map<FlakeId, FlakeInput>`.
- `Fingerprint` — alias (`typedef`), file path `src/libflake/include/nix/flake/flake.hh`, `Hash`; cache key for a locked flake.
- `ConfigFile::ConfigValue` — alias (`using`, member of `ConfigFile`), file path `src/libflake/include/nix/flake/flake.hh`, `std::variant<std::string, int64_t, Explicit<bool>, std::vector<std::string>>`.

### Macros / globals
(none)

---

## File: src/libflake/include/nix/flake/flakeref.hh

### Namespaces
- `nix` — namespace, file path `src/libflake/include/nix/flake/flakeref.hh`.
- `nix::fetchers` — namespace, file path `src/libflake/include/nix/flake/flakeref.hh`, forward declares `struct Settings`.

### Classes / structs / enums
- `FlakeRef` — struct, file path `src/libflake/include/nix/flake/flakeref.hh`, fields: `fetchers::Input input`, `std::string subdir`. Defaulted `operator==`; `operator<` lexicographic on `(input, subdir)`. Constructor: `FlakeRef(fetchers::Input&& input, const std::string& subdir)`. Methods: `to_string() const`, `toAttrs() const`, `resolve(fetchSettings, store, useRegistries=All) const`, static `fromAttrs(fetchSettings, attrs)`, `lazyFetch(fetchSettings, store) const`, `canonicalize() const`.

### Functions
- `operator<<(std::ostream&, const FlakeRef&)` — free function declaration, file path `src/libflake/include/nix/flake/flakeref.hh`.
- `parseFlakeRef` — free function, file path `src/libflake/include/nix/flake/flakeref.hh`, signature `(fetchSettings, url, baseDir={}, allowMissing=false, isFlake=true, preserveRelativePaths=false)`.
- `parseFlakeRefWithFragment` — free function, file path `src/libflake/include/nix/flake/flakeref.hh`, returns `(FlakeRef, std::string)` (the fragment); same defaults as `parseFlakeRef`.
- `parseFlakeRefWithFragmentAndExtendedOutputsSpec` — free function, file path `src/libflake/include/nix/flake/flakeref.hh`, returns `(FlakeRef, std::string, ExtendedOutputsSpec)`; signature `(fetchSettings, url, baseDir={}, allowMissing=false, isFlake=true)`.

### Type aliases
- `FlakeId` — alias (`typedef`), file path `src/libflake/include/nix/flake/flakeref.hh`, `std::string`.

### Macros / globals
- `flakeIdRegexS` — `const static std::string`, file path `src/libflake/include/nix/flake/flakeref.hh`, regex pattern `[a-zA-Z][a-zA-Z0-9_-]*`.
- `flakeIdRegex` — extern `std::regex` (definition in `flakeref.cc`), file path `src/libflake/include/nix/flake/flakeref.hh`.

---

## File: src/libflake/include/nix/flake/lockfile.hh

### Namespaces
- `nix` — namespace, file path `src/libflake/include/nix/flake/lockfile.hh`, forward declares `class Store` and `class StorePath`.
- `nix::flake` — namespace, file path `src/libflake/include/nix/flake/lockfile.hh`.

### Classes / structs / enums
- `NonEmptyInputAttrPath` — class, file path `src/libflake/include/nix/flake/lockfile.hh`, wraps a non-empty `InputAttrPath`. Private field `InputAttrPath path`; explicit private constructor `NonEmptyInputAttrPath(InputAttrPath&&)` (asserts non-empty). Public statics: `parse(string_view) -> std::optional<NonEmptyInputAttrPath>`, `make(InputAttrPath) -> std::optional<NonEmptyInputAttrPath>`, `append(prefix, element) -> NonEmptyInputAttrPath`. Public members: `get() const -> const InputAttrPath&`, implicit conversion `operator const InputAttrPath&() const`, `inputName() const` (last component), `parent() const` (path without last), defaulted `operator<=>`.
- `Node` — struct (publicly inherits `std::enable_shared_from_this<Node>`), file path `src/libflake/include/nix/flake/lockfile.hh`, lock-file root node; nested `typedef std::variant<ref<LockedNode>, InputAttrPath> Edge`; field `std::map<FlakeId, Edge> inputs`; `virtual ~Node() {}`.
- `LockedNode` — struct (extends `Node`), file path `src/libflake/include/nix/flake/lockfile.hh`, fields: `FlakeRef lockedRef, originalRef`, `bool isFlake = true`, `std::optional<InputAttrPath> parentInputAttrPath`. Constructors: `LockedNode(const FlakeRef& lockedRef, const FlakeRef& originalRef, bool isFlake=true, std::optional<InputAttrPath> parentInputAttrPath={})` and `LockedNode(const fetchers::Settings&, const nlohmann::json&)`. Method: `computeStorePath(Store&) const`.
- `LockFile` — struct, file path `src/libflake/include/nix/flake/lockfile.hh`, field `ref<Node> root = make_ref<Node>()`. Default constructor and `LockFile(const fetchers::Settings&, std::string_view contents, std::string_view path)`. Nested `typedef std::map<ref<const Node>, std::string> KeyMap`. Methods: `toJSON() const -> std::pair<nlohmann::json, KeyMap>`, `to_string() const -> std::pair<std::string, KeyMap>`, `isUnlocked(fetchSettings) const -> std::optional<FlakeRef>`, `operator==`, `findInput(path) -> std::shared_ptr<Node>`, `getAllInputs() const -> std::map<InputAttrPath, Node::Edge>`, static `diff(oldLocks, newLocks) -> std::string`, `check()`.

### Functions
- `operator<<(std::ostream&, const LockFile&)` — free function declaration, file path `src/libflake/include/nix/flake/lockfile.hh`.
- `parseInputAttrPath(std::string_view)` — free function declaration, file path `src/libflake/include/nix/flake/lockfile.hh`, parses a `/`-separated path of flake-ids.
- `printInputAttrPath(const InputAttrPath&)` — free function declaration, file path `src/libflake/include/nix/flake/lockfile.hh`, joins with `/`.

### Type aliases
- `InputAttrPath` — alias (`typedef`), file path `src/libflake/include/nix/flake/lockfile.hh`, `std::vector<FlakeId>`.
- `Node::Edge` — alias (`typedef`, member of `Node`), file path `src/libflake/include/nix/flake/lockfile.hh`, `std::variant<ref<LockedNode>, InputAttrPath>`.
- `LockFile::KeyMap` — alias (`typedef`, member of `LockFile`), file path `src/libflake/include/nix/flake/lockfile.hh`, `std::map<ref<const Node>, std::string>`.

### Macros / globals
(none)

---

## File: src/libflake/include/nix/flake/settings.hh

### Namespaces
- `nix` — namespace, file path `src/libflake/include/nix/flake/settings.hh`, forward declares `struct EvalSettings`.
- `nix::flake` — namespace, file path `src/libflake/include/nix/flake/settings.hh`.

### Classes / structs / enums
- `Settings` — struct (extends `Config`), file path `src/libflake/include/nix/flake/settings.hh`, default constructor; method `configureEvalSettings(nix::EvalSettings&) const`. Setting members: `Setting<bool> useRegistries{this, true, "use-registries", ..., {}, true, Xp::Flakes}`; `Setting<bool> acceptFlakeConfig{this, false, "accept-flake-config", ..., {}, true, Xp::Flakes}`; `Setting<std::string> commitLockFileSummary{this, "", "commit-lock-file-summary", ..., {"commit-lockfile-summary"}, true, Xp::Flakes}`.

### Functions
(only the constructor and `configureEvalSettings` declared; defined in `settings.cc`)

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libflake/include/nix/flake/url-name.hh

### Namespaces
- `nix` — namespace, file path `src/libflake/include/nix/flake/url-name.hh`, forward declares `struct ParsedURL`.

### Classes / structs / enums
(none)

### Functions
- `getNameFromURL(const ParsedURL&)` — free function declaration, file path `src/libflake/include/nix/flake/url-name.hh`, heuristic that derives a human-readable name from a parsed flake URL; returns `nullopt` if the URL is not informative (empty or `default`).

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libflake/lockfile.cc

### Namespaces
- `nix` — namespace, file path `src/libflake/lockfile.cc`, forward declares `class Store`.
- `nix::flake` — namespace, file path `src/libflake/lockfile.cc`, scope of `LockFile` and `LockedNode` definitions.

### Classes / structs / enums
(none defined in this file beyond implementing those declared in `lockfile.hh`)

### Functions
- `getFlakeRef` — free function (static), file path `src/libflake/lockfile.cc`, reads `attr` and an optional legacy `info` key from a JSON node, merges them into one `Attrs`, and constructs a `FlakeRef`; throws if `attr` is missing.
- `LockedNode::LockedNode(const fetchers::Settings&, const nlohmann::json&)` — constructor, file path `src/libflake/lockfile.cc`, builds locked/original `FlakeRef`s from JSON (with legacy `info` merge for `locked`), reads `flake` and `parent`, validates lockedness or warns/throws based on NAR-hash trust, asserts `__final` is not present then stamps `__final = true`.
- `LockedNode::computeStorePath` — member function, file path `src/libflake/lockfile.cc`, delegates to `lockedRef.input.computeStorePath`.
- `doFind` — free function (static), file path `src/libflake/lockfile.cc`, recursive helper that walks `Node` edges following `follows` indirections while detecting cycles via a `visited` vector.
- `LockFile::findInput` — member function, file path `src/libflake/lockfile.cc`, public entry to `doFind` with a fresh `visited` vector.
- `LockFile::LockFile(const fetchers::Settings&, std::string_view contents, std::string_view path)` — constructor, file path `src/libflake/lockfile.cc`, parses JSON (annotates parse errors with the file path), validates `version` is in `[5, 7]`, and builds the node graph using a self-recursive lambda; tolerates legacy array-form follows; throws on cycles back to root in old versions.
- `LockFile::toJSON` — member function, file path `src/libflake/lockfile.cc`, serializes the graph; allocates fresh keys with `_2`, `_3`, ... suffixes on collision; emits `version=7`; strips `__final` from `locked`; asserts the locked input is `isFinal()` or `isRelative()`.
- `LockFile::to_string` — member function, file path `src/libflake/lockfile.cc`, returns `(toJSON().first.dump(2), keyMap)`.
- `operator<<(std::ostream&, const LockFile&)` — free function, file path `src/libflake/lockfile.cc`, streams `toJSON().first.dump(2)`.
- `LockFile::isUnlocked` — member function, file path `src/libflake/lockfile.cc`, DFS-collects all reachable nodes then returns the first non-locked, non-final, non-relative `LockedNode::lockedRef`; honors `allowDirtyLocks` for NAR-hashed inputs.
- `LockFile::operator==` — member function, file path `src/libflake/lockfile.cc`, equality via JSON comparison (marked FIXME: slow).
- `parseInputAttrPath` — free function, file path `src/libflake/lockfile.cc`, splits on `/` and validates each token against `flakeIdRegex`; throws `UsageError` on invalid element.
- `NonEmptyInputAttrPath::parse` — static member function, file path `src/libflake/lockfile.cc`, calls `parseInputAttrPath` then `make`.
- `NonEmptyInputAttrPath::make` — static member function, file path `src/libflake/lockfile.cc`, returns `nullopt` for empty paths.
- `LockFile::getAllInputs` — member function, file path `src/libflake/lockfile.cc`, DFS-collects every `(InputAttrPath, Edge)` reachable from root, deduplicating nodes via a `done` set.
- `describe` — free function (static), file path `src/libflake/lockfile.cc`, formats a `FlakeRef` quoted with optional `(YYYY-MM-DD)` last-modified suffix.
- `operator<<(std::ostream&, const Node::Edge&)` — free function, file path `src/libflake/lockfile.cc`, prints either the locked-node description or `follows '<path>'`.
- `equals` — free function (static), file path `src/libflake/lockfile.cc`, semantic equality on `Node::Edge` (compares `lockedRef` for nodes, or follows path).
- `LockFile::diff` — static member function, file path `src/libflake/lockfile.cc`, merge-walks two flattened input maps and produces an ANSI-colored Added/Removed/Updated summary string.
- `LockFile::check` — member function, file path `src/libflake/lockfile.cc`, asserts every non-empty `follows` target resolves via `findInput` (throws `Error` otherwise).
- `void check();` — orphan free-function forward declaration at file scope inside `nix::flake`, file path `src/libflake/lockfile.cc`, no definition; appears unreferenced and is dead code.
- `printInputAttrPath` — free function, file path `src/libflake/lockfile.cc`, joins path components with `/` via `concatStringsSep`.

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libflake/settings.cc

### Namespaces
- `nix::flake` — namespace, file path `src/libflake/settings.cc`.

### Classes / structs / enums
(none)

### Functions
- `Settings::Settings()` — constructor, file path `src/libflake/settings.cc`, empty body (default-constructs `Config` and the inline `Setting<...>` members).
- `Settings::configureEvalSettings` — member function, file path `src/libflake/settings.cc`, registers `primops::getFlake(*this)`, `primops::parseFlakeRef`, and `primops::flakeRefToString` into `EvalSettings::extraPrimOps`.

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libflake/url-name.cc

### Namespaces
- `nix` — namespace, file path `src/libflake/url-name.cc`.

### Classes / structs / enums
(none)

### Functions
- `getNameFromURL(const ParsedURL&)` — free function, file path `src/libflake/url-name.cc`, derives a name in priority order: `dir=` query value; last attribute element of the fragment when the prefix is not `defaultPackage.` and the last element is not `default`; second path segment when the scheme matches `github|gitlab|sourcehut`; last path segment for `git`/`git+*` schemes; last path segment as fallback; returns `nullopt` if no rule applies.

### Type aliases
(none)

### Macros / globals
- `attributeNamePattern` — file-scope `static const std::string`, file path `src/libflake/url-name.cc`, `[a-zA-Z0-9_-]+`.
- `lastAttributeRegex` — file-scope `static const std::regex`, file path `src/libflake/url-name.cc`, captures the (prefix, last-element, optional `^...`) of a Nix attr-path-style fragment.
- `pathSegmentPattern` — file-scope `static const std::string`, file path `src/libflake/url-name.cc`, `[a-zA-Z0-9_-]+`.
- `lastPathSegmentRegex` — file-scope `static const std::regex`, file path `src/libflake/url-name.cc`, captures the trailing path segment.
- `secondPathSegmentRegex` — file-scope `static const std::regex`, file path `src/libflake/url-name.cc`, captures the second `/`-separated path segment.
- `gitProviderRegex` — file-scope `static const std::regex`, file path `src/libflake/url-name.cc`, matches `github|gitlab|sourcehut`.
- `gitSchemeRegex` — file-scope `static const std::regex`, file path `src/libflake/url-name.cc`, matches `git` and `git+*` scheme prefixes.

---

## File: src/libmain/common-args.cc

### Namespaces
- `nix` — namespace, file path `src/libmain/common-args.cc`.

### Classes / structs / enums
(none defined; this file implements methods of classes declared in `common-args.hh`)

### Functions
- `MixCommonArgs::MixCommonArgs(const std::string& programName)` — constructor, file path `src/libmain/common-args.cc`, registers `--verbose`/`-v`, `--quiet`, `--debug`, `--option` (with completion against `globalConfig.getSettings`), `--log-format`, `--max-jobs`/`-j`; folds all `globalConfig` settings into command-line flags via `convertToArgs` under category `"Options to override configuration settings"`; erases the `system` flag when `programName == "nix-env"`; hides the configuration-overrides category.
- `MixCommonArgs::initialFlagsProcessed` — member function (override), file path `src/libmain/common-args.cc`, calls `initPlugins()` then the protected hook `pluginsInited()`.
- `MixPrintJSON::printJSON<T>` — template member function definition (with explicit instantiation for `nlohmann::json`), file path `src/libmain/common-args.cc`, suspends the logger and writes JSON either pretty (`dump(2)`) or compact (`dump()`) based on `outputPretty`.

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libmain/include/nix/main/common-args.hh

### Namespaces
- `nix` — namespace, file path `src/libmain/include/nix/main/common-args.hh`.

### Classes / structs / enums
- `MixCommonArgs` — class (`virtual` extends `Args`), file path `src/libmain/include/nix/main/common-args.hh`. Public field `std::string programName`. Public constructor from `programName`. Private override `initialFlagsProcessed()`. Protected hook `virtual void pluginsInited() {}`.
- `MixDryRun` — struct (extends `virtual Args`), file path `src/libmain/include/nix/main/common-args.hh`, field `bool dryRun = false`; constructor adds `--dry-run`.
- `MixPrintJSON` — struct (extends `virtual Args`), file path `src/libmain/include/nix/main/common-args.hh`, field `bool outputPretty = isatty(STDOUT_FILENO)`; constructor adds `--pretty`/`--no-pretty`; templated method `printJSON<T>(const T&)` with `std::enable_if_t<std::is_same_v<T, nlohmann::json>>` to forbid implicit string-to-json coercion.
- `MixJSON` — struct (extends `virtual Args, virtual MixPrintJSON`), file path `src/libmain/include/nix/main/common-args.hh`, field `bool json = false`; constructor adds `--json`.
- `MixRepair` — struct (extends `virtual Args`), file path `src/libmain/include/nix/main/common-args.hh`, field `RepairFlag repair = NoRepair`; constructor adds `--repair`.

### Functions
(only constructors, the override on `MixCommonArgs`, and the `printJSON` template above)

### Type aliases
(none)

### Macros / globals
- `loggingCategory` — `static constexpr auto`, file path `src/libmain/include/nix/main/common-args.hh`, value `"Logging-related options"`.
- `miscCategory` — `static constexpr auto`, file path `src/libmain/include/nix/main/common-args.hh`, value `"Miscellaneous global options"`.

---

## File: src/libmain/include/nix/main/loggers.hh

### Namespaces
- `nix` — namespace, file path `src/libmain/include/nix/main/loggers.hh`.

### Classes / structs / enums
- `LogFormat` — `enum class`, file path `src/libmain/include/nix/main/loggers.hh`, enumerators: `raw`, `rawWithLogs`, `internalJSON`, `bar`, `barWithLogs`.

### Functions
- `setLogFormat(const std::string&)` — free function declaration, file path `src/libmain/include/nix/main/loggers.hh`.
- `setLogFormat(const LogFormat&)` — free function declaration, file path `src/libmain/include/nix/main/loggers.hh`.

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libmain/include/nix/main/plugin.hh

### Namespaces
- `nix` — namespace, file path `src/libmain/include/nix/main/plugin.hh`.

### Classes / structs / enums
(none)

### Functions
- `initPlugins()` — free function declaration, file path `src/libmain/include/nix/main/plugin.hh`, called after settings init to load configured plugins.

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libmain/include/nix/main/progress-bar.hh

### Namespaces
- `nix` — namespace, file path `src/libmain/include/nix/main/progress-bar.hh`.

### Classes / structs / enums
(none)

### Functions
- `makeProgressBar()` — free function declaration, file path `src/libmain/include/nix/main/progress-bar.hh`, returns a `std::unique_ptr<Logger>`.

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libmain/include/nix/main/shared.hh

### Namespaces
- `nix` — namespace, file path `src/libmain/include/nix/main/shared.hh`, also forward declares `class Store` and `struct MissingPaths` and `struct GCResults`.

### Classes / structs / enums
- `LegacyArgs` — class (extends `MixCommonArgs, RootArgs`), file path `src/libmain/include/nix/main/shared.hh`, public field `fun<bool(Strings::iterator& arg, const Strings::iterator& end)> parseArg`; constructor `LegacyArgs(programName, parseArg)`; overrides `processFlag(Strings::iterator& pos, Strings::iterator end)` and `processArgs(const Strings& args, bool finish)`.
- `RunPager` — class, file path `src/libmain/include/nix/main/shared.hh`, public default constructor (forks a pager when stdout is a TTY) and destructor (waits the pager); private fields `Pid pid` (non-Windows) and `Descriptor std_out`.

### Functions
- `initNix(bool loadConfig=true)` — free function declaration, file path `src/libmain/include/nix/main/shared.hh`.
- `parseCmdLine(int argc, char** argv, fun<...> parseArg)` — free function declaration, file path `src/libmain/include/nix/main/shared.hh`.
- `parseCmdLine(const std::string& programName, const Strings& args, fun<...> parseArg)` — free function declaration, file path `src/libmain/include/nix/main/shared.hh`.
- `printVersion(const std::string& programName)` — free function declaration, file path `src/libmain/include/nix/main/shared.hh`.
- `printGCWarning()` — free function declaration, file path `src/libmain/include/nix/main/shared.hh`.
- `printMissing(ref<Store>, const std::vector<DerivedPath>&, Verbosity=lvlInfo)` — free function declaration, file path `src/libmain/include/nix/main/shared.hh`.
- `printMissing(ref<Store>, const MissingPaths&, Verbosity=lvlInfo)` — free function declaration, file path `src/libmain/include/nix/main/shared.hh`.
- `getArg(const std::string& opt, Strings::iterator& i, const Strings::iterator& end)` — free function declaration, file path `src/libmain/include/nix/main/shared.hh`.
- `getIntArg<N>(opt, i, end, allowUnit)` — template free function definition, file path `src/libmain/include/nix/main/shared.hh`, advances the iterator and parses an integer with `string2IntWithUnitPrefix<N>`; throws `UsageError` if missing argument. (Note: the `allowUnit` parameter is accepted but not consulted in the body.)
- `printFreed(bool dryRun, const GCResults& results)` — free function declaration, file path `src/libmain/include/nix/main/shared.hh`.
- `detectStackOverflow()` — free function declaration (non-Windows only), file path `src/libmain/include/nix/main/shared.hh`.
- `defaultStackOverflowHandler(siginfo_t*, void*)` — free function declaration (non-Windows only), file path `src/libmain/include/nix/main/shared.hh`.

### Type aliases
(none)

### Macros / globals
- `blockInt` — `extern volatile ::sig_atomic_t`, file path `src/libmain/include/nix/main/shared.hh`, declared but no matching definition or use is present in `src/`; appears to be dead code.
- `stackOverflowHandler` — `extern fun<void(siginfo_t* info, void* ctx)>` (non-Windows only), file path `src/libmain/include/nix/main/shared.hh`, defined in `unix/stack.cc`.

---

## File: src/libmain/loggers.cc

### Namespaces
- `nix` — namespace, file path `src/libmain/loggers.cc`.

### Classes / structs / enums
(none)

### Functions
- `parseLogFormat(const std::string&)` — free function, file path `src/libmain/loggers.cc`, maps strings (`raw`, `raw-with-logs`, `internal-json`, `bar`, `bar-with-logs`) to a `LogFormat`; the `NIX_GET_COMPLETIONS` env var (or string `"raw"`) forces `LogFormat::raw`; throws `Error` on unknown value.
- `makeDefaultLogger()` — free function, file path `src/libmain/loggers.cc`, dispatches on `defaultLogFormat` to `makeSimpleLogger(false)` for `raw`, `makeSimpleLogger(true)` for `rawWithLogs`, `makeJSONLogger(getStandardError())` for `internalJSON`, `makeProgressBar()` for `bar`, or progress bar with `setPrintBuildLogs(true)` for `barWithLogs`; calls `unreachable()` otherwise.
- `setLogFormat(const std::string&)` — free function, file path `src/libmain/loggers.cc`, parses the string and forwards to the enum overload.
- `setLogFormat(const LogFormat&)` — free function, file path `src/libmain/loggers.cc`, updates `defaultLogFormat` and re-creates the global `logger` via `makeDefaultLogger`.

### Type aliases
(none)

### Macros / globals
- `defaultLogFormat` — global `LogFormat`, file path `src/libmain/loggers.cc`, initialized to `LogFormat::raw`.

---

## File: src/libmain/plugin.cc

### Namespaces
- `nix` — namespace, file path `src/libmain/plugin.cc`.

### Classes / structs / enums
- `PluginFilesSetting` — struct (extends `BaseSetting<std::list<std::filesystem::path>>`), file path `src/libmain/plugin.cc`, mutable field `bool pluginsLoaded = false`; constructor takes `Config* options`, default value, name, description, optional aliases and registers itself via `options->addSetting(this)`; `parse(const std::string&) const override` rejects late changes.
- `PluginSettings` — struct (extends `Config`), file path `src/libmain/plugin.cc`, holds the inline-initialized `PluginFilesSetting pluginFiles{this, {}, "plugin-files", ...}` with its full docstring.

### Functions
- `PluginFilesSetting::parse(const std::string&) const` — member function (override), file path `src/libmain/plugin.cc`, throws `UsageError` if plugins were already loaded; otherwise delegates to `BaseSetting::parse`.
- `initPlugins()` — free function, file path `src/libmain/plugin.cc`, asserts `!pluginsLoaded`, expands directories into file lists via `DirectoryIterator` (catches `SystemError` for non-directory paths), `dlopen`s each plugin (`RTLD_LAZY|RTLD_LOCAL`), invokes optional `nix_plugin_entry()` via `dlsym`, then calls `globalConfig.reapplyUnknownSettings()` and `warnUnknownSettings()`, finally sets `pluginsLoaded = true`. On Windows throws an `Error` for each plugin.

### Type aliases
(none)

### Macros / globals
- `pluginSettings` — file-scope static `PluginSettings`, file path `src/libmain/plugin.cc`.
- `rPluginSettings` — file-scope static `GlobalConfig::Register`, file path `src/libmain/plugin.cc`, RAII-registers `pluginSettings` with the global config registry.

---

## File: src/libmain/progress-bar.cc

### Namespaces
- `nix` — namespace, file path `src/libmain/progress-bar.cc`.
- `nix::{anonymous}` — anonymous namespace, file path `src/libmain/progress-bar.cc`, hides the progress-bar internals (helper functions and the `ProgressBar` class).

### Classes / structs / enums
- `ProgressBar` — class (`final`, extends `Logger`), file path `src/libmain/progress-bar.cc` (in the anonymous namespace). Nested types:
  - `ProgressBar::ActInfo` — struct (private), file path `src/libmain/progress-bar.cc`, fields: `std::string s, lastLine, phase`, `ActivityType type = actUnknown`, `uint64_t done = 0, expected = 0, running = 0, failed = 0`, `std::map<ActivityType, uint64_t> expectedByType`, `bool visible = true`, `ActivityId parent`, `std::optional<std::string> name`, `std::chrono::time_point<std::chrono::steady_clock> startTime`.
  - `ProgressBar::ActivitiesByType` — struct (private), file path `src/libmain/progress-bar.cc`, fields: `std::map<ActivityId, std::list<ActInfo>::iterator> its`, `uint64_t done = 0, expected = 0, failed = 0`.
  - `ProgressBar::State` — struct (private), file path `src/libmain/progress-bar.cc`, fields: `std::list<ActInfo> activities`, `std::map<ActivityId, std::list<ActInfo>::iterator> its`, `std::map<ActivityType, ActivitiesByType> activitiesByType`, `uint64_t filesLinked = 0, bytesLinked = 0`, `uint64_t corruptedPaths = 0, untrustedPaths = 0`, `bool active = true`, `size_t suspensions = 0`, `bool haveUpdate = true`; method `bool isPaused() const`.
  - Private fields on `ProgressBar`: `Sync<std::string> lastOutput_`, `Sync<State> state_`, `std::thread updateThread`, `std::condition_variable quitCV, updateCV`, `bool printBuildLogs = false`, `bool isTTY`, `std::unique_ptr<InterruptCallback> interruptCallback`.
  - Private methods: `hideCursorIfNeeded() const`, `unhideCursorIfNeeded() const`.
  - Public constructor `ProgressBar(bool isTTY)` (registers an interrupt callback that pauses+`shutting down`; spawns `updateThread`).
  - Public destructor `~ProgressBar()` calling `stop()`.
  - Public overrides (`override final` for `stop`, `override` for the rest): `stop()`, `pause()`, `resume()`, `bool isVerbose()`, `log(Verbosity, std::string_view)`, `logEI(const ErrorInfo&)`, `startActivity(ActivityId, Verbosity, ActivityType, const std::string&, const Fields&, ActivityId)`, `stopActivity(ActivityId)`, `result(ActivityId, ResultType, const std::vector<Field>&)`, `writeToStdout(std::string_view)`, `std::optional<char> ask(std::string_view)`, `setPrintBuildLogs(bool)`.
  - Public non-override methods: `void log(State&, Verbosity, std::string_view)` (private overload taking the locked state), `bool hasAncestor(State&, ActivityType, ActivityId)`, `void update(State&)`, `void redraw(std::string)`, `void invalidateRedrawCache()`, `void clearProgressDisplay()`, `std::chrono::milliseconds draw(State&)`, `std::string getStatus(State&)`.

### Functions
- `getS(const std::vector<Logger::Field>&, size_t)` — free function (anonymous namespace, static), file path `src/libmain/progress-bar.cc`, returns the string field at index `n`; throws `Error` if missing or wrong type.
- `getI(const std::vector<Logger::Field>&, size_t)` — free function (anonymous namespace, static), file path `src/libmain/progress-bar.cc`, returns the int field at index `n`; throws `Error` if missing or wrong type.
- `storePathToName(std::string_view path)` — free function (anonymous namespace, static), file path `src/libmain/progress-bar.cc`, returns the substring after the first `-` of the basename (or empty if no `-`).
- `storePathToNameWithoutDrvSuffix(std::string_view path)` — free function (anonymous namespace, static), file path `src/libmain/progress-bar.cc`, like `storePathToName` but strips a trailing `drvExtension` (`.drv`).
- `makeProgressBar()` — free function, file path `src/libmain/progress-bar.cc`, factory creating `std::make_unique<ProgressBar>(isTTY())`.
- (Inline lambdas inside `ProgressBar::getStatus`: `renderActivity`, `renderSizeActivity`, `maybeAppendToResult`, `showActivity` — local helpers, not file-scope.)

### Type aliases
(none)

### Macros / globals
(none)

---

## File: src/libmain/shared.cc

### Namespaces
- `nix` — namespace, file path `src/libmain/shared.cc`.

### Classes / structs / enums
(implements classes from `shared.hh`)

### Functions
- `printGCWarning()` — free function, file path `src/libmain/shared.cc`, emits a one-shot warning that no `--add-root` was given (gated by `gcWarning` and `warnOnce`'s static `haveWarned`).
- `printMissing(ref<Store>, const std::vector<DerivedPath>&, Verbosity)` — free function, file path `src/libmain/shared.cc`, defers to `store->queryMissing` and the `MissingPaths` overload.
- `printMissing(ref<Store>, const MissingPaths&, Verbosity)` — free function, file path `src/libmain/shared.cc`, prints paths to be built (topo-sorted, reversed), substituted (sorted by name then store-path), and unknown; mentions read-only-mode warning when `settings.readOnlyMode`.
- `getArg(const std::string&, Strings::iterator&, const Strings::iterator&)` — free function, file path `src/libmain/shared.cc`, advances iterator or throws `UsageError`.
- `sigHandler(int signo)` — free function (static, non-Windows), file path `src/libmain/shared.cc`, no-op handler used for `NIX_SIG_MULTI_INT`, `SIGWINCH`, etc.
- `bumpFileLimit()` — free function, file path `src/libmain/shared.cc`, raises `RLIMIT_NOFILE` to the hard limit (capped to `kern.maxfilesperproc` on Apple via `sysctlbyname`) on a best-effort basis; no-op on Windows.
- `initNix(bool loadConfig)` — free function, file path `src/libmain/shared.cc`, performs full library bootstrap: `pubsetbuf` for cerr (when `HAVE_PUBSETBUF`), `initLibStore`, `unix::startSignalHandlerThread`, `SIG_DFL` for `SIGCHLD`, dummy handler for `NIX_SIG_MULTI_INT`, Apple-specific signal taming (`SIGWINCH` dummy plus `SIG_DFL` for `SIGINT`/`SIGTERM`/`SIGHUP`/`SIGPIPE`/`SIGQUIT`/`SIGTRAP` to disable `SA_RESTART`), `detectStackOverflow`, `umask(0022)`, `bumpFileLimit`.
- `LegacyArgs::LegacyArgs(programName, parseArg)` — constructor, file path `src/libmain/shared.cc`, registers `--no-build-output`/`-Q`, `--keep-failed`/`-K`, `--keep-going`/`-k`, `--fallback`, integer-with-unit aliases via the local `intSettingAlias` lambda for `--cores`, `--max-silent-time`, `--timeout`, plus `--readonly-mode`, `--no-gc-warning`, `--store`.
- `LegacyArgs::processFlag(Strings::iterator& pos, Strings::iterator end)` — member function (override), file path `src/libmain/shared.cc`, lets `MixCommonArgs::processFlag` handle first; otherwise delegates to `parseArg` and increments `pos` on success.
- `LegacyArgs::processArgs(const Strings& args, bool finish)` — member function (override), file path `src/libmain/shared.cc`, asserts a single positional argument and calls `parseArg`, throwing `UsageError` on rejection; returns true on empty args.
- `parseCmdLine(int, char**, parseArg)` — free function, file path `src/libmain/shared.cc`, builds string args from `argv` via `argvToStrings` and forwards.
- `parseCmdLine(const std::string&, const Strings&, parseArg)` — free function, file path `src/libmain/shared.cc`, instantiates `LegacyArgs` and calls `parseCmdline`.
- `printVersion(const std::string&)` — free function, file path `src/libmain/shared.cc`, prints the program name + `nixVersion`; with `verbosity > lvlInfo` also prints system type, extra platforms, features (`gc` only when `NIX_USE_BOEHMGC`, plus `signed-caches`), config file paths, store/state directories; throws `Exit` to terminate.
- `RunPager::RunPager()` — constructor, file path `src/libmain/shared.cc`, no-ops if stdout isn't a TTY or `NIX_PAGER`/`PAGER` is empty/`cat`; otherwise stops the logger, forks a child via `startProcess` that sets `LESS=FRSXMK` if unset, `dup2`s the read end onto stdin and `execl`s `/bin/sh -c $pager` (falling back to `pager`/`less`/`more`); sets `SIGINT` kill-signal and dups stdout to the pipe (Windows throws an explicit `Error`).
- `RunPager::~RunPager()` — destructor, file path `src/libmain/shared.cc`, restores stdout, waits the pager pid, swallows exceptions via `ignoreExceptionInDestructor`.
- `printFreed(bool dryRun, const GCResults&)` — free function, file path `src/libmain/shared.cc`, prints the GC summary (paths and bytes freed; in dry-run mode prints only the path count, since `bytesFreed` cannot be reliably computed without deletion due to hardlinking).

### Type aliases
(none)

### Macros / globals
- `savedArgv` — global `char**`, file path `src/libmain/shared.cc`, definition (declared `extern` in `src/libcmd/include/nix/cmd/command.hh`); raw saved argv pointer set by `nix/main.cc` and read by `nix/unix/daemon.cc`.
- `gcWarning` — file-scope static `bool` (default `true`), file path `src/libmain/shared.cc`, gate for `printGCWarning` toggled by `--no-gc-warning`.

---

## File: src/libmain/unix/stack.cc

### Namespaces
- `nix` — namespace, file path `src/libmain/unix/stack.cc`.

### Classes / structs / enums
(none)

### Functions
- `sigsegvHandler(int, siginfo_t*, void*)` — free function (static), file path `src/libmain/unix/stack.cc`, attempts to read the stack pointer via architecture-specific `REG_RSP` (x86_64) or `REG_ESP` and calls `nix::stackOverflowHandler` if `si_addr` is within 4096 bytes of `sp`; then restores default `SIGSEGV` behavior via `sigaction(SIGSEGV, SIG_DFL)` and returns (so the kernel can dump core); calls `abort()` on `sigaction` failure.
- `detectStackOverflow()` — free function, file path `src/libmain/unix/stack.cc`, only does real work when `SA_SIGINFO && SA_ONSTACK` are defined; allocates a heap-backed alt-stack (`4096*4 + MINSIGSTKSZ` bytes) once via `static auto stackBuf = std::make_unique<std::vector<char>>(...)`; calls `sigaltstack`; installs `sigsegvHandler` with `SA_SIGINFO|SA_ONSTACK`.
- `defaultStackOverflowHandler(siginfo_t*, void*)` — free function, file path `src/libmain/unix/stack.cc`, writes a static "stack overflow (possible infinite recursion)" message via `::write(2, ...)` then `_exit(1)` (with FIXME comment).

### Type aliases
(none)

### Macros / globals
- `stackOverflowHandler` — global `fun<void(siginfo_t* info, void* ctx)>`, file path `src/libmain/unix/stack.cc`, definition initialized to `defaultStackOverflowHandler`; declared `extern` in `shared.hh`.
- `stackBuf` — function-local static `std::unique_ptr<std::vector<char>>` inside `detectStackOverflow`, file path `src/libmain/unix/stack.cc`, owns the alt-signal-stack bytes for process lifetime.

---

## Cross-file observations

- **Two flake-ref attribute parsers, one direction.** `prim_flakeRefToString` and `prim_parseFlakeRef` (`flake-primops.cc`) re-implement the attr/string round-trip exposed by `FlakeRef::fromAttrs`/`FlakeRef::toAttrs` (`flakeref.cc`, `flakeref.hh`). The primop versions add Nix-value coercion (string/bool/int + `publicKeys` JSON) but otherwise duplicate logic that also appears in `parseFlakeInputAttr` (`flake.cc`). Three different sites coerce a Nix attribute value into a `fetchers::Attrs` entry with near-identical type-dispatch on `nString`/`nBool`/`nInt`.
- **Whitelist of trusted `nixConfig` keys is duplicated.** The hardcoded `whitelist` in `ConfigFile::apply` (`config.cc`) — `bash-prompt`, `bash-prompt-prefix`, `bash-prompt-suffix`, `flake-registry`, `commit-lock-file-summary`, `commit-lockfile-summary` — overlaps with the alias list of `commitLockFileSummary` (`{"commit-lockfile-summary"}` aliasing `commit-lock-file-summary`) declared in `flake::Settings` (`settings.hh`). Future work could move the whitelist beside the settings declarations.
- **URL-name extraction overlaps with FlakeRef parsing.** `getNameFromURL` (`url-name.cc`) inspects schemes (`github|gitlab|sourcehut`, `git`, `git+...`) and path/fragment shapes that are also probed by the flakeref parser (`fromParsedURL`, `parsePathFlakeRefWithFragment`, `parseFlakeIdRef` in `flakeref.cc`). Both modules use independent regexes for the same flake URL grammar; the github/gitlab/sourcehut list is also hard-coded in fetchers (outside this shard).
- **Lock-file walk repeated.** `LockFile::isUnlocked`, `LockFile::getAllInputs`, and `doFind` in `lockfile.cc` each implement a custom DFS over `Node::inputs` with their own visited-set; only `getAllInputs` is reused (by `diff` and `check`). A shared `forEachNode`/`forEachReachableEdge` helper would simplify all three.
- **Config-error reporting duplicated.** `parseFlakeInputAttr` (`flake.cc`) and `prim_flakeRefToString` (`flake-primops.cc`) both emit nearly identical "negative value given for ..." errors and "flake reference attribute sets may only contain integers, Booleans, and strings" type errors (with the latter using "flake input attribute" wording). The wording could come from a shared helper.
- **Logger lifecycle tied to `LogFormat` enum** (`loggers.hh`/`loggers.cc`) drives both progress-bar and JSON loggers. `parseLogFormat` (`loggers.cc`) accepts `raw`, `raw-with-logs`, `internal-json`, `bar`, `bar-with-logs`, while `MixCommonArgs` documents only `raw`, `internal-json`, `bar`, `bar-with-logs` (omitting `raw-with-logs`); the two strings should be derived from a single source of truth.
- **`ProgressBar::ActInfo`/`ActivitiesByType`/`State`** (`progress-bar.cc`) own per-type progress aggregations (`done`, `expected`, `failed`, plus `running` only on `ActInfo`) and `getStatus` re-aggregates them via two near-identical lambdas (`renderActivity`, `renderSizeActivity`). The two lambdas mostly differ in unit rendering and could share a helper.
- **Stack-overflow plumbing split across headers.** `shared.hh` exposes `stackOverflowHandler`/`detectStackOverflow`/`defaultStackOverflowHandler` whose definitions live in `unix/stack.cc`; the global `stackOverflowHandler` is declared `extern` in `shared.hh` and defined in `unix/stack.cc`. This is the only Unix-specific piece in the otherwise platform-neutral `shared.hh` API.
- **Stray `void check();`** in `lockfile.cc` inside `nix::flake` after `LockFile::check`'s definition is an unused forward declaration with no definition; appears to be dead code.
- **`MixCommonArgs` and `LegacyArgs`** both register `--max-jobs`/`-j`-style flags by binding to `settings.set`; the int-with-unit-prefix parsing pattern (`string2IntWithUnitPrefix`) is reproduced in three sites: `getIntArg` (`shared.hh`) and the `intSettingAlias` lambda in `LegacyArgs`'s constructor (`shared.cc`), plus `MixCommonArgs`'s `--max-jobs` lambda (`common-args.cc`, where the value is passed through as a string). Note that `getIntArg`'s `allowUnit` parameter is currently unused.
- **Trusted-list persistence** (`readTrustedList`/`writeTrustedList`/`trustedListPath` in `config.cc`) is a small ad-hoc JSON-backed store of `name -> value -> bool` rooted at `getDataDir()/trusted-settings.json`. It is independent of the `Config`/`Setting` machinery used elsewhere in this shard, despite addressing the same configuration-acceptance question that `acceptFlakeConfig` covers.
- **`callFlake`'s `keyMap` mapping** (built by `LockFile::toJSON`) is the canonical source of truth for stable lock-file keys; `lockfile.cc::toJSON` allocates `_2`/`_3`/... suffixes on collision, and `flake.cc::callFlake` consumes the resulting `KeyMap` to install per-node `sourceInfo`/`dir` overrides. The key generation logic is local to `toJSON` and not reused, so any other consumer that needs node identifiers must round-trip through JSON.
- **Settings registration vs. plugin loading order.** `MixCommonArgs::initialFlagsProcessed` calls `initPlugins()` followed by the protected hook `pluginsInited()`; `initPlugins` (`plugin.cc`) then `reapplyUnknownSettings`/`warnUnknownSettings`. Setting a `plugin-files` value after this point throws `UsageError` (`PluginFilesSetting::parse`); subcommand authors must register the flag before the subcommand argument.
