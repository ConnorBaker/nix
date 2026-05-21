# Inventory — Shard 06: libstore derivations

## File: src/libstore/derivations.cc

### Namespaces
- `nix` — wraps everything except the JSON serialisers.
- anonymous namespace inside `nix` — holds `StringViewStream` and `Escapes`.
- `nlohmann` — at file end, holds `adl_serializer` template specialisations for `DerivationOutput`, `BasicDerivation`, `Derivation`.

### Classes / structs / enums
- `StringViewStream` — struct, kind: helper. Lightweight `std::istream`-style cursor over a `std::string_view`; field `remaining` (`std::string_view`); methods `peek() const` returning `int` (EOF if empty) and `get()` consuming one character.
- `Escapes` — struct, kind: compile-time table. Field `char map[256]`; `constexpr` default constructor populates entries (identity by default; overrides for `'n'`, `'r'`, `'t'`); provides `char operator[](char) const`. A single `constexpr` instance `escapes` lives in the same anonymous namespace.
- `DerivationATermVersion` — `enum struct`. Values: `Traditional`, `DynamicDerivations`. Used by parser to distinguish ATerm format versions.

### Functions
- `DerivationOutput::path(StoreDirConfig &, std::string_view drvName, OutputNameView outputName) const` — member. Returns `std::optional<StorePath>` for the output, dispatching across the variant via `std::visit`/`overloaded` (returns nullopt for `CAFloating`/`Deferred`/`Impure`).
- `DerivationOutput::CAFixed::path(StoreDirConfig &, std::string_view drvName, OutputNameView outputName) const` — member. Computes the fixed output store path via `store.makeFixedOutputPathFromCA(outputPathName(...), ContentAddressWithReferences::withoutRefs(ca))`.
- `DerivationType::isCA() const` — member. True iff `ContentAddressed` or `Impure` variant.
- `DerivationType::isFixed() const` — member. True iff `ContentAddressed{.fixed = true}`.
- `DerivationType::hasKnownOutputPaths() const` — member. True for non-deferred input-addressed and fixed CA; false for floating CA / impure.
- `DerivationType::isSandboxed() const` — member. True for input-addressed; depends on `sandboxed` for CA; false for impure.
- `DerivationType::isImpure() const` — member. True iff `Impure` variant.
- `BasicDerivation::isBuiltin() const` — member. Tests whether `builder` starts with `"builtin:"` (compares first 8 chars).
- `infoForDerivation(const StoreDirConfig &, const Derivation &)` — file-static (using auto-deduced return). Computes a `(suffix, contents, references, path)` tuple needed to write a derivation; references include `inputDrvs.map` keys plus `inputSrcs`; the path is computed via `store.makeFixedOutputPathFromCA` over a `TextInfo`.
- `computeStorePath(const StoreDirConfig &, const Derivation &)` — free function. Returns the would-be store path of a `Derivation` without writing it (delegates to `infoForDerivation`).
- `Store::writeDerivation(const Derivation &, RepairFlag)` — member of `Store` defined here. Adds a temp root, returns existing path if already valid (and not repairing), else writes the derivation contents via `addToStoreFromDump` with `FileSerialisationMethod::Flat` / `ContentAddressMethod::Raw::Text`.
- `expect(StringViewStream &, std::string_view)` — file-static. Asserts upcoming literal and consumes it; throws `FormatError`.
- `expect(StringViewStream &, char)` — file-static overload of the above.
- `parseString(StringViewStream &)` — file-static. Reads a backslash-escaped C-style string into a `BackedStringView`, decoding escapes via the `escapes` table.
- `validatePath(std::string_view)` — file-static. Throws `FormatError` if the path is empty or doesn't start with `/`.
- `parsePath(StringViewStream &)` — file-static. Parses a string then validates as a path.
- `endOfList(StringViewStream &)` — file-static. Returns true on `]`, false on `,`, leaves position after delimiter.
- `parseStrings(StringViewStream &, bool arePaths)` — file-static. Parses a `[ ... ]` of strings/paths into a `StringSet`.
- `parseDerivationOutput(StoreDirConfig &, std::string_view pathS, std::string_view hashAlgoStr, std::string_view hashS, const ExperimentalFeatureSettings &)` — file-static. Builds a `DerivationOutput` variant from already-parsed strings; enforces experimental-feature gating for text-hashed/CA/impure outputs.
- `parseDerivationOutput(StoreDirConfig &, StringViewStream &, const ExperimentalFeatureSettings &)` — file-static overload. Reads `,path,algo,hash)` tuple from stream and dispatches to the above.
- `parseDerivedPathMapNode(StoreDirConfig &, StringViewStream &, DerivationATermVersion)` — file-static. Recursively parses a `DerivedPathMap<StringSet>::ChildNode` according to the ATerm version.
- `parseDerivation(const StoreDirConfig &, std::string &&, std::string_view, const ExperimentalFeatureSettings &)` — free function. Top-level parser; matches `Derive(` or `DrvWithVersion(`, then outputs / input derivs / input srcs / platform / builder / args / env, returning a `Derivation`. Extracts `__json` env var as `StructuredAttrs`.
- `printString(std::string &, std::string_view)` — file-static. Quoted/escaped string emitter (chunked through a buffer of size `2 * chunkSize + 2` with `chunkSize = 1024`).
- `printUnquotedString(std::string &, std::string_view)` — file-static. Emits `"..."` without escaping.
- `printStrings<ForwardIterator>(std::string &, ForwardIterator, ForwardIterator)` — file-static template. Emits `[ ... ]` of escaped strings.
- `printUnquotedStrings<ForwardIterator>(std::string &, ForwardIterator, ForwardIterator)` — file-static template. Same but without per-string escaping.
- `unparseDerivedPathMapNode(const StoreDirConfig &, std::string &, const DerivedPathMap<StringSet>::ChildNode &)` — file-static. Renders an inputDrvs ChildNode (handling dynamic outputs).
- `hasDynamicDrvDep(const Derivation &)` — file-static. True iff any inputDrvs ChildNode has a non-empty `childMap`.
- `Derivation::unparse(const StoreDirConfig &, bool maskOutputs, DerivedPathMap<StringSet>::ChildNode::Map * actualInputs) const` — member. Renders a derivation to its ATerm string; chooses `Derive(` vs `DrvWithVersion("xp-dyn-drv",...)` form; can mask outputs and substitute `actualInputs` for `inputDrvs`.
- `isDerivation(std::string_view)` — free function. Checks for `drvExtension` (`.drv`) suffix.
- `outputPathName(std::string_view drvName, OutputNameView outputName)` — free function. Returns `drvName-outputName`, or just `drvName` when output is `out`.
- `BasicDerivation::type() const` — member. Determines the `DerivationType` by reducing per-output kinds; throws on inconsistent mixes; throws if no outputs; rejects multiple `CAFixed` outputs and rejects fixed-output named anything other than `"out"`.
- `pathDerivationModulo(Store &, const StorePath &)` — file-static. Memoised lookup in `drvHashes` of `hashDerivationModulo` for a stored derivation; uses `cvisit` for the lookup and `insert_or_assign` to cache.
- `hashDerivationModulo(Store &, const Derivation &, bool maskOutputs)` — free function. Returns the `DrvHashModulo` (per-output for fixed CA, single hash for resolvable input-addr, deferred otherwise); recursively replaces input drv paths with their modulo hashes; throws if a CA output's hash for a referenced output name is missing.
- `readDerivationOutput(Source &, const StoreDirConfig &)` — file-static. Reads a `DerivationOutput` from a `Source`.
- `BasicDerivation::outputNames() const` — member. Returns `StringSet` of output names.
- `BasicDerivation::outputsAndOptPaths(const StoreDirConfig &) const` — member. Returns the `DerivationOutputsAndOptPaths` map keyed on output name.
- `BasicDerivation::nameFromPath(const StorePath &)` — static member. Strips `.drv` suffix from a derivation `StorePath`'s name; calls `requireDerivation()` first.
- `readDerivation(Source &, const StoreDirConfig &, BasicDerivation &, std::string_view)` — free function. Reads a serialised `BasicDerivation` from a `Source`; returns the `Source &`. Tries to extract structuredAttrs from env via `StructuredAttrs::tryExtract`.
- `writeDerivation(Sink &, const StoreDirConfig &, const BasicDerivation &)` — free function. Writes the binary serialisation; calls `StructuredAttrs::checkKeyNotInUse(drv.env)` first.
- `hashPlaceholder(OutputNameView)` — free function. Returns `/<base32 sha256 of "nix-output:" + name>` placeholder string (Nix32-encoded SHA-256, no SRI prefix).
- `BasicDerivation::applyRewrites(const StringMap &)` — member. Applies a `StringMap` rewrite to `builder`, `args`, `env` (rewriting both keys and values), and (round-trip JSON parse) `structuredAttrs`.
- `Derivation::shouldResolve() const` — member. Decides whether the derivation needs resolution before building (deferred IA, floating CA, fixed CA when `Xp::CaDerivations` is enabled, impure, or any inputs from dynamic derivations). Returns false immediately if `inputDrvs.map` is empty.
- `Derivation::tryResolve(Store &, Store * evalStore) const` — member. Convenience overload using `resolveDerivedPath` plus a try/catch returning `std::nullopt` on `Error`.
- `tryResolveInput(const StoreDirConfig &, StorePathSet &, StringMap &, const DownstreamPlaceholder *, ref<const SingleDerivedPath>, const DerivedPathMap<StringSet>::ChildNode &, fun<...>)` — file-static. Recursively rewrites a single `inputDrvs` entry into source paths and placeholder rewrites; uses `DownstreamPlaceholder::unknownDerivation` / `unknownCaOutput` for placeholders.
- `Derivation::tryResolve(Store &, fun<std::optional<StorePath>(ref<const SingleDerivedPath>, const std::string &)>) const` — member. Main resolver: walks `inputDrvs.map`, applies `tryResolveInput`, then `applyRewrites` and `fillInOutputPaths`.
- `processDerivationOutputPaths<bool fillIn>(Store &, auto && drv, std::string_view drvName)` — file-static template. Either validates output paths/env vars (when `fillIn` is false) or fills them in based on `hashDerivationModulo` (when `fillIn` is true). Calls `drv.type()` at the end as an invariant assertion.
- `Derivation::checkInvariants(Store &, const StorePath &) const` — member. Asserts `drvPath.isDerivation()`, verifies `drvName` matches path, then delegates with extra error trace.
- `Derivation::checkInvariants(Store &) const` — member. Calls `processDerivationOutputPaths<false>(store, *this, name)`.
- `Derivation::fillInOutputPaths(Store &)` — member. Calls `processDerivationOutputPaths<true>(store, *this, name)`.
- `Derivation::parseJsonAndValidate(Store &, const nlohmann::json &)` — static member. Casts JSON to `Derivation`, fills paths, checks invariants, adds trace on error.

### nlohmann adl_serializer specialisations
- `adl_serializer<DerivationOutput>::to_json(json &, const DerivationOutput &)` — visits the variant: InputAddressed emits `path`; CAFixed assigns the `ContentAddress` directly; CAFloating emits `method`+`hashAlgo`; Deferred emits empty object; Impure emits `method`+`hashAlgo`+`impure: true`.
- `adl_serializer<DerivationOutput>::from_json(const json &, const ExperimentalFeatureSettings &)` — per-keys-set dispatch to the variant arms; gates Text on `Xp::DynamicDerivations`, CAFloating on `Xp::CaDerivations`, Impure on `Xp::ImpureDerivations`.
- `inputSrcsToJson(json &, const StorePathSet &)` — file-static. Encodes a `StorePathSet` as a JSON array of path strings.
- `basicDerivationToJson(json &, const BasicDerivation &)` — file-static. Common JSON skeleton for `BasicDerivation`/`Derivation`: `name`, `version`, `outputs`, `system`, `builder`, `args`, `env`, optional `structuredAttrs`.
- `adl_serializer<BasicDerivation>::to_json(json &, const BasicDerivation &)` — calls `basicDerivationToJson`, then writes `inputs` as a flat JSON array of `inputSrcs`.
- `adl_serializer<BasicDerivation>::from_json(const json &, const ExperimentalFeatureSettings &)` — calls `basicDerivationFromJson`, then `inputSrcsFromJson(json["inputs"], ...)`.
- `adl_serializer<Derivation>::to_json(json &, const Derivation &)` — calls `basicDerivationToJson`, then writes structured `inputs.{srcs,drvs}`; recurses through `inputDrvs.map` emitting `outputs` plus `dynamicOutputs` per node.
- `adl_serializer<Derivation>::from_json(const json &, const ExperimentalFeatureSettings &)` — mirrors `to_json`; recursively reads `dynamicOutputs` requiring `Xp::DynamicDerivations` for each entry.
- `inputSrcsFromJson(const json &, StorePathSet &)` — file-static. Inverse of `inputSrcsToJson`.
- `basicDerivationFromJson(const json::object_t &, BasicDerivation &, const ExperimentalFeatureSettings &)` — file-static. Reads name, validates `version` against `expectedJsonVersionDerivation`, reads outputs, system, builder, args, env, optional `structuredAttrs`.

### Type aliases
- (none introduced in this `.cc`; uses aliases declared in headers.)

### Macros / globals
- `escapes` — file-scope `constexpr Escapes` instance (anonymous namespace).
- `drvHashes` — global `DrvHashes` (boost concurrent flat map) defined here, `extern`-declared in the header.
- `impureOutputHash` — `const Hash` global; `hashString(HashAlgorithm::SHA256, "impure")`.

---

## File: src/libstore/include/nix/store/derivations.hh

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- `DerivationOutput` — struct; the variant of the five output kinds.
  - Nested `InputAddressed` — struct; field `StorePath path`; defaulted `==` and `<=>`.
  - Nested `CAFixed` — struct; field `ContentAddress ca`; member `path(const StoreDirConfig &, std::string_view drvName, OutputNameView outputName) const`; defaulted `==` and `<=>`.
  - Nested `CAFloating` — struct; fields `ContentAddressMethod method`, `HashAlgorithm hashAlgo`; defaulted `==`/`<=>`.
  - Nested `Deferred` — empty struct, defaulted `==`/`<=>`.
  - Nested `Impure` — struct; fields `ContentAddressMethod method`, `HashAlgorithm hashAlgo`; defaulted `==`/`<=>`.
  - Type alias `Raw = std::variant<InputAddressed, CAFixed, CAFloating, Deferred, Impure>` (declared via `typedef`).
  - Field `Raw raw`.
  - Defaulted `operator==` and `operator<=>` on `DerivationOutput` itself.
  - `MAKE_WRAPPER_CONSTRUCTOR(DerivationOutput)` (variant-wrapping ctor).
  - Deleted default ctor (`DerivationOutput() = delete;`).
  - Member `path(const StoreDirConfig &, std::string_view drvName, OutputNameView outputName) const` returning `std::optional<StorePath>`.
- `DerivationType` — struct; the variant of derivation kinds.
  - Nested `InputAddressed` — struct; `bool deferred` field; defaulted `==`/`<=>`.
  - Nested `ContentAddressed` — struct; fields `bool sandboxed`, `bool fixed`; defaulted `==`/`<=>`.
  - Nested `Impure` — empty struct; defaulted `==`/`<=>`.
  - Type alias `Raw = std::variant<InputAddressed, ContentAddressed, Impure>`.
  - Field `Raw raw`.
  - Defaulted `operator==` and `operator<=>` on `DerivationType`.
  - `MAKE_WRAPPER_CONSTRUCTOR(DerivationType)`; deleted default ctor.
  - Methods: `isCA() const`, `isFixed() const`, `isSandboxed() const`, `isImpure() const`, `hasKnownOutputPaths() const`.
- `BasicDerivation` — struct.
  - Fields: `DerivationOutputs outputs`, `StorePathSet inputSrcs`, `std::string platform`, `std::string builder`, `Strings args`, `StringPairs env`, `std::optional<StructuredAttrs> structuredAttrs`, `std::string name`.
  - Special members: defaulted default/move/copy constructors and copy/move `operator=`; defined-but-empty virtual destructor `virtual ~BasicDerivation() {}`.
  - Methods: `isBuiltin() const`, `type() const`, `outputNames() const`, `outputsAndOptPaths(const StoreDirConfig &) const`, static `nameFromPath(const StorePath &)`, `applyRewrites(const StringMap &)`, defaulted `operator==`. (`<=>` deliberately omitted; comment notes libc++ 16 bug.)
- `Derivation` — struct, derives from `BasicDerivation`.
  - Field `DerivedPathMap<std::set<OutputName, std::less<>>> inputDrvs`.
  - Methods: `unparse(const StoreDirConfig &, bool maskOutputs, DerivedPathMap<StringSet>::ChildNode::Map * actualInputs = nullptr) const`, `shouldResolve() const`, two-overload `tryResolve` (one taking `Store * evalStore = nullptr`, one taking the resolution callback), two-overload `checkInvariants` (one with just `Store &`, one with `Store &` + `const StorePath &`), `fillInOutputPaths(Store &)`.
  - Constructors: defaulted `Derivation()`, `Derivation(const BasicDerivation &)`, `Derivation(BasicDerivation &&)`.
  - Static: `parseJsonAndValidate(Store &, const nlohmann::json &)`.
  - Defaulted `operator==`. (`<=>` deliberately omitted; same libc++ 16 comment.)
- `DrvHashModulo` — struct; variant of the three hash result kinds.
  - Type aliases (using-declarations) `DrvHash = Hash`, `CaOutputHashes = std::map<std::string, Hash>`, `DeferredDrv = std::monostate`.
  - Type alias `Raw = std::variant<DrvHash, CaOutputHashes, DeferredDrv>`.
  - Field `Raw raw`.
  - Defaulted `operator==`. (`<=>` commented-out.)
  - `MAKE_WRAPPER_CONSTRUCTOR(DrvHashModulo)`.
- `DrvHashFct` — struct; functor used as boost concurrent_flat_map hash.
  - Type alias `is_avalanching = std::true_type`.
  - `std::size_t operator()(const StorePath &) const noexcept` — uses `std::hash<std::string_view>{}(path.to_string())`.

### Functions
- `computeStorePath(const StoreDirConfig &, const Derivation &)` — declared.
- `parseDerivation(const StoreDirConfig &, std::string &&, std::string_view name, const ExperimentalFeatureSettings & = experimentalFeatureSettings)` — declared.
- `isDerivation(std::string_view fileName)` — declared.
- `outputPathName(std::string_view drvName, OutputNameView outputName)` — declared.
- `hashDerivationModulo(Store &, const Derivation &, bool maskOutputs)` — declared.
- `resolveInputAddressed(Store &, Derivation &)` — declared (no definition in this shard).
- `readDerivation(Source &, const StoreDirConfig &, BasicDerivation &, std::string_view)` — declared.
- `writeDerivation(Sink &, const StoreDirConfig &, const BasicDerivation &)` — declared.
- `hashPlaceholder(const OutputNameView)` — declared.

### Type aliases
- `DerivationOutputs = std::map<std::string, DerivationOutput>` (typedef).
- `DerivationOutputsAndOptPaths = std::map<std::string, std::pair<DerivationOutput, std::optional<StorePath>>>` (typedef).
- `DerivationInputs = std::map<StorePath, StringSet>` (typedef).
- `DrvHashes = boost::concurrent_flat_map<StorePath, DrvHashModulo, DrvHashFct>` (typedef).

### Macros / globals
- `extern DrvHashes drvHashes;` — declaration of the global memoisation map.
- `constexpr unsigned expectedJsonVersionDerivation = 4;` — JSON version constant.
- Forward declarations: `struct StoreDirConfig;`, `class Store;`, `struct Source;`, `struct Sink;`.
- `JSON_IMPL_WITH_XP_FEATURES(nix::DerivationOutput)` — declares JSON adl_serializer (outside `nix` namespace).
- `JSON_IMPL_WITH_XP_FEATURES(nix::BasicDerivation)`.
- `JSON_IMPL_WITH_XP_FEATURES(nix::Derivation)`.

---

## File: src/libstore/derivation-options.cc

### Namespaces
- `nix` — primary namespace.
- `nlohmann` — JSON serialiser specialisations.

### Classes / structs / enums
- (no new types; consumes `DerivationOptions<...>::OutputChecks` from the header.)

### Functions
- `getStringAttr(const StringMap & env, const StructuredAttrs * parsed, const std::string & name)` — file-static. Reads either a structuredAttrs string or a flat env value as `std::optional<std::string>`; rethrows with trace on conversion error.
- `getBoolAttr(const StringMap & env, const StructuredAttrs * parsed, const std::string & name, bool def)` — file-static. Same pattern for `bool`; flat env uses `*i == "1"` truthiness.
- `getStringSetAttr(const StringMap & env, const StructuredAttrs * parsed, const std::string & name)` — file-static. Same pattern returning `std::optional<StringSet>`; flat env uses `tokenizeString<StringSet>`.
- `derivationOptionsFromStructuredAttrs(const StoreDirConfig &, const StringMap &, const StructuredAttrs *, bool, const ExperimentalFeatureSettings &)` — `DerivationOptions<StorePath>` overload. Delegates to the `SingleDerivedPath` version with empty `inputDrvs`, then asserts-resolves (callback `assert(false)` since there is nothing to resolve).
- `flatten(const nlohmann::json &, StringSet &)` — file-static. Recursively flattens an array of strings (or single string) into a `StringSet`; throws otherwise.
- `derivationOptionsFromStructuredAttrs(const StoreDirConfig &, const DerivedPathMap<StringSet> & inputDrvs, const StringMap &, const StructuredAttrs *, bool, const ExperimentalFeatureSettings &)` — main parser; builds:
  - placeholder map from input drvs (only when `Xp::CaDerivations` enabled),
  - lambdas `findPlaceholder`, `parseSingleDerivedPath`, `parseRef`,
  - warns when `structuredAttrs` hides certain top-level attrs (`allowedReferences`, `allowedRequisites`, `disallowedRequisites`, `disallowedReferences`, `maxSize`, `maxClosureSize`, `passAsFile`),
  - populates all `DerivationOptions<SingleDerivedPath>` fields including `outputChecks`, `unsafeDiscardReferences`, `passAsFile`, `exportReferencesGraph`, `additionalSandboxProfile`, `noChroot`, `impureHostDeps`, `impureEnvVars`, `allowLocalNetworking`, `requiredSystemFeatures`, `preferLocalBuild`, `allowSubstitutes`.
- `DerivationOptions<Input>::getRequiredSystemFeatures(const BasicDerivation &) const` — template member. Builds the result set; auto-adds `"ca-derivations"` if `drv.type().hasKnownOutputPaths()` is false.
- `DerivationOptions<Input>::substitutesAllowed(const WorkerSettings &) const` — template member. `workerSettings.alwaysAllowSubstitutes ? true : allowSubstitutes`.
- `DerivationOptions<Input>::useUidRange(const BasicDerivation &) const` — template member. True iff feature `"uid-range"` is in `getRequiredSystemFeatures`.
- `tryResolve(const DerivationOptions<SingleDerivedPath> &, fun<std::optional<StorePath>(ref<const SingleDerivedPath>, const std::string &)>)` — free function. Resolves a `DerivationOptions<SingleDerivedPath>` to `DerivationOptions<StorePath>`. Internal helpers `tryResolvePath`, `tryResolveRef`, `tryResolveRefSet`, `tryResolveOutputChecks`, `tryResolveExportReferencesGraph`. Walks the `OutputChecks` variant via `std::visit`.

### nlohmann adl_serializer specialisations
- `derivationOptionsFromJson<Inputs>(const nlohmann::json &)` — file-static template. Converts a JSON object into `DerivationOptions<Inputs>` (handles `forAllOutputs` vs `perOutput`; throws if both or neither present).
- `derivationOptionsToJson<Inputs>(nlohmann::json &, const DerivationOptions<Inputs> &)` — file-static template. Inverse.
- `outputChecksFromJson<Inputs>(const nlohmann::json &)` / `outputChecksToJson<Inputs>(nlohmann::json &, const OutputChecks<Inputs> &)` — file-static templates for `OutputChecks<Inputs>`.
- `adl_serializer<DerivationOptions<SingleDerivedPath>>::from_json(const json &)` / `to_json(json &, ...)`.
- `adl_serializer<DerivationOptions<StorePath>>::from_json` / `to_json`.
- `adl_serializer<OutputChecks<SingleDerivedPath>>::from_json` / `to_json`.
- `adl_serializer<OutputChecks<StorePath>>::from_json` / `to_json`.

### Type aliases
- `OutputChecks<Inputs> = DerivationOptions<Inputs>::OutputChecks` (file-local using-template).
- `OutputChecksVariant<Inputs> = std::variant<OutputChecks<Inputs>, std::map<std::string, OutputChecks<Inputs>, std::less<>>>` (file-local using-template).

### Macros / globals
- `template struct DerivationOptions<StorePath>;` and `template struct DerivationOptions<SingleDerivedPath>;` — explicit instantiations.

---

## File: src/libstore/include/nix/store/derivation-options.hh

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- `DerivationOptions<Input>` — class template (struct).
  - Nested `OutputChecks` struct:
    - Field `bool ignoreSelfRefs = false`.
    - Field `std::optional<uint64_t> maxSize, maxClosureSize` (declared together).
    - Type alias `using DrvRef = nix::DrvRef<Input>;` (member-typedef).
    - Field `std::optional<std::set<DrvRef>> allowedReferences`.
    - Field `std::set<DrvRef> disallowedReferences`.
    - Field `std::optional<std::set<DrvRef>> allowedRequisites`.
    - Field `std::set<DrvRef> disallowedRequisites`.
    - Defaulted `operator==`.
  - Field `outputChecks` (`std::variant<OutputChecks, std::map<std::string, OutputChecks, std::less<>>>`, default `OutputChecks{}`).
  - Field `unsafeDiscardReferences` (`std::map<std::string, bool, std::less<>>`).
  - Field `passAsFile` (`StringSet`).
  - Field `exportReferencesGraph` (`std::map<std::string, std::set<Input>, std::less<>>`).
  - Field `additionalSandboxProfile` (`std::string`, default `""`).
  - Field `noChroot` (`bool`, default `false`).
  - Field `impureHostDeps` (`StringSet`, default `{}`).
  - Field `impureEnvVars` (`StringSet`, default `{}`).
  - Field `allowLocalNetworking` (`bool`, default `false`).
  - Field `requiredSystemFeatures` (`StringSet`, default `{}`).
  - Field `preferLocalBuild` (`bool`, default `false`).
  - Field `allowSubstitutes` (`bool`, default `true`).
  - Defaulted `operator==`.
  - Methods: `getRequiredSystemFeatures(const BasicDerivation &) const`, `substitutesAllowed(const WorkerSettings &) const`, `useUidRange(const BasicDerivation &) const`.

### Functions
- `derivationOptionsFromStructuredAttrs(const StoreDirConfig &, const DerivedPathMap<StringSet> & inputDrvs, const StringMap & env, const StructuredAttrs * parsed, bool shouldWarn = true, const ExperimentalFeatureSettings & = experimentalFeatureSettings)` — declared, returns `DerivationOptions<SingleDerivedPath>`.
- `derivationOptionsFromStructuredAttrs(const StoreDirConfig &, const StringMap & env, const StructuredAttrs * parsed, bool shouldWarn = true, const ExperimentalFeatureSettings & = experimentalFeatureSettings)` — overload declared, returns `DerivationOptions<StorePath>`.
- `tryResolve(const DerivationOptions<SingleDerivedPath> &, fun<std::optional<StorePath>(ref<const SingleDerivedPath>, const std::string &)>)` — declared.

### Type aliases
- (template-internal `DrvRef` aliasing `nix::DrvRef<Input>`.)

### Macros / globals
- Forward declarations: `struct StoreDirConfig;`, `struct BasicDerivation;`, `struct StructuredAttrs;`, `template<typename V> struct DerivedPathMap;`, `struct DerivationOutput;` (after the template).
- `extern template struct DerivationOptions<StorePath>;` / `<SingleDerivedPath>;` — extern instantiations.
- `JSON_IMPL(nix::DerivationOptions<nix::StorePath>);` / `<nix::SingleDerivedPath>;` / `<...>::OutputChecks` (both flavours) — JSON adl_serializer declarations outside `nix`.

---

## File: src/libstore/parsed-derivations.cc

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- (no new types defined; provides member implementations for `StructuredAttrs`.)

### Functions
- `StructuredAttrs::parse(std::string_view encoded)` — static. Parses a JSON string into a `StructuredAttrs`; on `std::exception` rethrows as `Error` referencing `envVarName` ("__json").
- `StructuredAttrs::tryExtract(StringPairs & env)` — static. If env contains `__json`, removes the entry and parses it; returns `std::optional<StructuredAttrs>`.
- `StructuredAttrs::unparse() const` — member. Returns `(envVarName, json.dump())` pair.
- `StructuredAttrs::checkKeyNotInUse(const StringPairs & env)` — static. Throws if env already contains `__json`.
- `pathInfoToJSON(Store &, const StorePathSet &)` — file-static. Emits a JSON array of `{narHash, narSize, references, ca?, path, valid, closureSize}` objects.
- `StructuredAttrs::prepareStructuredAttrs(Store &, const DerivationOptions<StorePath> &, const StorePathSet & inputPaths, const DerivationOutputs & outputs) const` — member. Returns a JSON object copy with `outputs` (placeholders) and any `exportReferencesGraph` entries injected (resolved via `store.exportReferences`).
- `StructuredAttrs::writeShell(const nlohmann::json::object_t &)` — static. Emits a bash declarations script (handles strings, numbers, nulls, booleans, arrays of simple, objects of simple); skips keys not matching `shVarName`.

### Type aliases
- (none new.)

### Macros / globals
- `static std::regex shVarName("[A-Za-z_][A-Za-z0-9_]*");` — file-static regex for valid shell variable names.

---

## File: src/libstore/include/nix/store/parsed-derivations.hh

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- `StructuredAttrs` — struct.
  - Static constexpr `std::string_view envVarName{"__json"}`.
  - Field `nlohmann::json::object_t structuredAttrs`.
  - Defaulted `operator==`.
  - Static `parse(std::string_view encoded)`, static `tryExtract(StringPairs & env)`, member `unparse() const`, static `checkKeyNotInUse(const StringPairs &)`, member `prepareStructuredAttrs(Store &, const DerivationOptions<StorePath> &, const StorePathSet &, const DerivationOutputs &) const`, static `writeShell(const nlohmann::json::object_t &)`.

### Functions
- (no free functions.)

### Type aliases
- `DerivationOutputs = std::map<std::string, DerivationOutput>` (typedef; duplicated with the same definition in `derivations.hh`).

### Macros / globals
- Forward declarations: `class Store;`, `template<typename Input> struct DerivationOptions;`, `struct DerivationOutput;`.

---

## File: src/libstore/names.cc

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- `Regex` — struct (defined here, declared as a forward-decl in `names.hh`); single field `std::regex regex`. Used as opaque pimpl held by `DrvName::regex` pointer.

### Functions
- `DrvName::DrvName()` — default constructor; sets `name = ""`.
- `DrvName::DrvName(std::string_view s)` — primary constructor; initialises `hits(0)`, sets `name = fullName = std::string(s)`, then splits on the first `'-'` not followed by an alpha into `name`/`version`.
- `DrvName::~DrvName()` — destructor (defined out-of-line, empty body) so the `unique_ptr<Regex>` can use the pimpl pattern.
- `DrvName::matches(const DrvName & n)` — member. If `name != "*"`, lazily compiles `name` as a POSIX-extended regex (caching it in the pimpl) and matches against `n.name`; if `version` is non-empty, also requires equality with `n.version`.
- `nextComponent(std::string_view::const_iterator & p, const std::string_view::const_iterator end)` — free function. Skips leading `.`/`-` separators, then consumes a maximal run of digits or non-digit-non-separators and returns the slice as `std::string_view`.
- `componentsLT(std::string_view c1, std::string_view c2)` — file-static. Performs the per-component comparison rule: numeric < numeric by integer value; `"" < numeric`; `"pre"` precedes anything else; "alpha vs digits" prefers the digit side; falls back to lexicographic.
- `compareVersions(std::string_view v1, std::string_view v2)` — free function. Returns `std::strong_ordering` derived from per-component comparisons via `nextComponent` + `componentsLT`.
- `drvNamesFromArgs(const Strings & opArgs)` — free function. Maps a list of argument strings into a `DrvNames` list (`emplace_back`).

### Type aliases
- (none new.)

### Macros / globals
- (none.)

---

## File: src/libstore/include/nix/store/names.hh

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- Forward declaration `struct Regex;`.
- `DrvName` — struct.
  - Fields: `std::string fullName`, `std::string name`, `std::string version`, `unsigned int hits`.
  - Constructors: default `DrvName()`, `DrvName(std::string_view s)`; destructor `~DrvName()`.
  - Public method `matches(const DrvName &)`.
  - Private member `std::unique_ptr<Regex> regex` (pimpl).

### Functions
- `nextComponent(std::string_view::const_iterator &, const std::string_view::const_iterator)` — declared.
- `compareVersions(std::string_view, std::string_view)` — declared.
- `drvNamesFromArgs(const Strings &)` — declared.

### Type aliases
- `DrvNames = std::list<DrvName>` (typedef).

### Macros / globals
- (none.)

---

## File: src/libstore/profiles.cc

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- (no new types; uses `Generation`, `Generations`, `GenerationNumber`, `ProfileDirsOptions` from header.)

### Functions
- `parseName(const std::string & profileName, const std::string & name)` — file-static. Parses `<profileName>-<num>-link` into a `std::optional<GenerationNumber>`.
- `findGenerations(std::filesystem::path profile)` — free function. Iterates the parent directory, collecting matching generations sorted ascending; the second element of the pair is the current generation number resolved from the profile symlink (or `nullopt` if profile doesn't exist).
- `makeName(const std::filesystem::path & profile, GenerationNumber num)` — file-static. Builds `<profile>-<num>-link` filesystem path via `fmt`.
- `createGeneration(LocalFSStore & store, std::filesystem::path profile, StorePath outPath)` — free function. Creates a new generation symlink (via `addPermRoot`) unless the previous generation already points at `outPath`; returns the generation path.
- `deleteGeneration(const std::filesystem::path & profile, GenerationNumber gen)` — free function. Unlinks the generation file if present.
- `deleteGeneration2(const std::filesystem::path & profile, GenerationNumber gen, bool dryRun)` — file-static. Logs and conditionally deletes.
- `deleteGenerations(const std::filesystem::path &, const std::set<GenerationNumber> &, bool dryRun)` — free function. Locks the profile, refuses to delete the active gen, deletes the requested set.
- `iterDropUntil(Generations & gens, auto && i, auto && cond)` — file-static `inline`. Advances the iterator until predicate holds.
- `deleteGenerationsGreaterThan(const std::filesystem::path &, GenerationNumber max, bool dryRun)` — free function. Throws if `max == 0`. Keeps the most recent `max` generations (after the current one) including the current one.
- `deleteOldGenerations(const std::filesystem::path &, bool dryRun)` — free function. Deletes everything except the current.
- `deleteGenerationsOlderThan(const std::filesystem::path &, time_t t, bool dryRun)` — free function. Keeps the newest gen older than `t` (so rollback remains possible) and deletes the rest.
- `parseOlderThanTimeSpec(std::string_view timeSpec)` — free function. Parses `"Nd"` into a `time_t` cutoff (`now - N*24*3600`); throws `UsageError` on bad input.
- `switchLink(std::filesystem::path link, std::filesystem::path target)` — free function. Replaces a symlink, using a relative target if same parent dir.
- `switchGeneration(const std::filesystem::path &, std::optional<GenerationNumber> dstGen, bool dryRun)` — free function. Picks the requested gen (or most recent older when `dstGen` is null), `notice`s, and replaces the symlink unless `dryRun`.
- `lockProfile(PathLocks & lock, const std::filesystem::path & profile)` — free function. Calls `lock.lockPaths({profile}, ...)` and `lock.setDeletion(true)`.
- `optimisticLockProfile(const std::filesystem::path & profile)` — free function. Returns the current symlink target as `std::string` (or `""`).
- `profilesDir(ProfileDirsOptions settings)` — free function. Resolves and ensures the user's profile directory exists (returns `rootProfilesDir` for root, else `<state>/profiles`).
- `rootProfilesDir(ProfileDirsOptions settings)` — free function. `<nixStateDir>/profiles/per-user/root`.
- `getDefaultProfile(ProfileDirsOptions settings)` — free function. Returns `~/.nix-profile` (or `<state>/profile` under XDG), creating symlinks as needed (root also gets `<state>/profiles/default`).
- `defaultChannelsDir(ProfileDirsOptions settings)` — free function. `profilesDir/channels`.
- `rootChannelsDir(ProfileDirsOptions settings)` — free function. `rootProfilesDir/channels`.

### Type aliases
- (none new.)

### Macros / globals
- (none.)

---

## File: src/libstore/include/nix/store/profiles.hh

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- `Generation` — struct.
  - Fields: `GenerationNumber number`, `std::filesystem::path path`, `time_t creationTime`.
- `ProfileDirsOptions` — struct.
  - Fields: `const std::filesystem::path & nixStateDir`, `bool useXDGBaseDirectories`.

### Functions (all declared)
- `findGenerations(std::filesystem::path)`.
- `createGeneration(LocalFSStore &, std::filesystem::path, StorePath)`.
- `deleteGeneration(const std::filesystem::path &, GenerationNumber)`.
- `deleteGenerations(const std::filesystem::path &, const std::set<GenerationNumber> &, bool)`.
- `deleteGenerationsGreaterThan(const std::filesystem::path &, GenerationNumber, bool)`.
- `deleteOldGenerations(const std::filesystem::path &, bool)`.
- `deleteGenerationsOlderThan(const std::filesystem::path &, time_t, bool)`.
- `parseOlderThanTimeSpec(std::string_view)`.
- `switchLink(std::filesystem::path, std::filesystem::path)`.
- `switchGeneration(const std::filesystem::path &, std::optional<GenerationNumber>, bool)`.
- `lockProfile(PathLocks &, const std::filesystem::path &)`.
- `optimisticLockProfile(const std::filesystem::path &)`.
- `profilesDir(ProfileDirsOptions)`.
- `rootProfilesDir(ProfileDirsOptions)`.
- `defaultChannelsDir(ProfileDirsOptions)`.
- `rootChannelsDir(ProfileDirsOptions)`.
- `getDefaultProfile(ProfileDirsOptions)`.

### Type aliases
- `GenerationNumber = uint64_t` (typedef).
- `Generations = std::list<Generation>` (typedef).

### Macros / globals
- Forward declarations: `class StorePath;`, `struct LocalFSStore;`.

---

## File: src/libstore/keys.cc

### Namespaces
- `nix` — main namespace.

### Functions
- `getDefaultPublicKeys()` — free function. Builds a `PublicKeys` map from `settings.trustedPublicKeys.get()` and from any readable `settings.secretKeyFiles.get()`; ignores `SystemError` on unreadable secret keys (multi-user installs).

### Type aliases / Macros / globals
- (none new.)

---

## File: src/libstore/include/nix/store/keys.hh

### Namespaces
- `nix` — main namespace.

### Functions
- `getDefaultPublicKeys()` — declared, returns `PublicKeys`.

### Type aliases / Macros / globals
- (none. Header is essentially a re-export of `nix/util/signature/local-keys.hh`.)

---

## File: src/libstore/log-store.cc

### Namespaces
- `nix` — main namespace.

### Functions
- `LogStore::anchor()` — empty private virtual override.
- `LogStore::getBuildLog(const StorePath & path)` — member. Looks up `getBuildDerivationPath(path)` and dispatches to `getBuildLogExact`; returns `std::nullopt` if no build derivation path.

### Type aliases / Macros / globals
- (none.)

---

## File: src/libstore/include/nix/store/log-store.hh

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- `LogStore` — struct, `public virtual` base of `Store`.
  - `private: void anchor() override;` — RTTI anchor.
  - `inline static std::string operationName = "Build log storage and retrieval";`.
  - `std::optional<std::string> getBuildLog(const StorePath &)` (member; non-virtual convenience wrapper).
  - `virtual std::optional<std::string> getBuildLogExact(const StorePath &) = 0;`.
  - `virtual void addBuildLog(const StorePath &, std::string_view) = 0;`.
  - `static LogStore & require(Store &)` — declared.

### Type aliases / Macros / globals
- (none.)

---

## File: src/libstore/misc.cc

### Namespaces
- `nix` — main namespace.
- `nlohmann` — for `TrustedFlag` JSON serialiser.

### Functions
- `Store::computeFSClosure(const StorePathSet & startPaths, StorePathSet & paths_, bool flipDirection, bool includeOutputs, bool includeDerivers)` — member. Builds an asio coroutine `queryDeps` (forward or reverse) and feeds `computeClosure<StorePath>`. Forward edges include path's `references`, plus its outputs (if `includeOutputs && isDerivation`) and `deriver` (if `includeDerivers`). Reverse direction uses `queryReferrers` plus `queryValidDerivers`/`queryPartialDerivationOutputMap` based on flags.
- `Store::computeFSClosure(const StorePath & startPath, StorePathSet &, bool, bool, bool)` — overload that wraps a single path into a set and delegates.
- `getDerivationCA(const BasicDerivation & drv)` — free function. Returns pointer to `ContentAddress` if `outputs["out"]` is `CAFixed`, else `nullptr`.
- `querySubstitutablePathInfosAsync(Store & store, const StorePathCAMap & paths, SubstitutablePathInfos & infos)` — file-static `asio::awaitable<void>`. Runs `forEachAsync` over substituters per path; respects `useSubstitutes`, `tryFallback`, and store-dir compatibility (recomputing fixed CA paths across stores).
- `Store::querySubstitutablePathInfos(const StorePathCAMap &, SubstitutablePathInfos &)` — member. Bridges asio io_context to a synchronous interface, rethrowing exceptions stored in the captured `std::exception_ptr`.
- `collectDerivedPaths(std::set<DerivedPath> & out, ref<SingleDerivedPath> inputDrv, const DerivedPathMap<StringSet>::ChildNode & node)` — file-static. Recursively flattens a `DerivedPathMap<StringSet>::ChildNode` into `DerivedPath::Built` edges.
- `Store::queryMissing(const std::vector<DerivedPath> & targets)` — member. Walks each `DerivedPath`, builds `MissingPaths { unknown, willBuild, willSubstitute, downloadSize, narSize }` using `computeClosure<DerivedPath>` and substituter queries. Honours CA derivations / dynamic-derivation caveats; warns on dynamic derivations not yet implemented; uses `derivationOptionsFromStructuredAttrs` to compute `substitutesAllowed`.
- `Store::topoSortPaths(const StorePathSet & paths)` — member. Topo-sorts paths by their `references`; throws `BuildError(BuildResult::Failure::OutputRejected, ...)` on cycle.
- `resolveDerivedPath(Store & store, const DerivedPath::Built & bfd, Store * evalStore_)` — free function. Resolves a `DerivedPath::Built` to an `OutputPathMap` (filtered by `OutputsSpec::All` or `OutputsSpec::Names`); throws `MissingRealisation` if any selected output has no path.
- `resolveDerivedPath(Store & store, const SingleDerivedPath & req, Store * evalStore_)` — free function. Resolves a `SingleDerivedPath`; for `Built` calls `deepQueryPartialDerivationOutput` and throws `MissingRealisation` on miss.
- `resolveDerivedPath(Store & store, const DerivedPath::Built & bfd)` — free function. Like the first overload, but without an `evalStore` parameter; uses `deepQueryDerivationOutputMap` and validates that all requested output names are present in the result map (throws if any are missing).

### nlohmann adl_serializer specialisations
- `adl_serializer<TrustedFlag>::from_json(const json &)` — converts boolean JSON to `TrustedFlag::Trusted`/`NotTrusted`.
- `adl_serializer<TrustedFlag>::to_json(json &, const TrustedFlag &)` — assigns the flag's `bool` representation.

### Type aliases / Macros / globals
- (none new.)

---

## File: src/libstore/include/nix/store/global-paths.hh

### Namespaces
- `nix` — main namespace.

### Functions
- `nixConfDir()` — free function declaration. Returns `const std::filesystem::path &` for the system config directory.
- `nixConfFile()` — `static inline` free function defined here. Returns `nixConfDir() / "nix.conf"`.
- `nixUserConfFiles()` — declaration. Returns `const std::vector<std::filesystem::path> &` listing user config files to load.

### Type aliases / Classes / globals
- (none.)

---

## File: src/libstore/optimise-store.cc

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- `MakeReadOnly` — struct. RAII wrapper that, on destruction, calls `canonicaliseTimestampAndPermissions(path.string())` to make the directory read-only again. Single field `std::filesystem::path path`; constructor `MakeReadOnly(std::filesystem::path)` (move-stores); destructor swallows exceptions via `ignoreExceptionInDestructor()` and skips canonicalisation if `path.empty()`.

### Functions
- `makeWritable(const std::filesystem::path & path)` — file-static. Adds `S_IWUSR` to the mode of `path` via `chmod`.
- `LocalStore::loadInodeHash()` — member. Walks `linksDir` via `opendir`/`readdir`, returning a `LocalStore::InodeHash` (set of inode numbers found there).
- `LocalStore::readDirectoryIgnoringInodes(const std::filesystem::path & path, const InodeHash & inodeHash)` — member. Returns `Strings` listing directory entries skipping known inodes and `.`/`..`.
- `LocalStore::optimisePath_(Activity * act, OptimiseStats & stats, const std::filesystem::path & path, InodeHash & inodeHash, RepairFlag repair)` — member. Recursively dedupes a tree by hard-linking equal regular files (and symlinks where `CAN_LINK_SYMLINK`) into `linksDir`. Skips `.app/Contents/...` on macOS, writable files (warning on suspicious writable file), files already linked, and tolerates `std::errc::no_space_on_device` (treats ext4 directory-index full as soft skip) and `std::errc::too_many_links` on the `link()` and `rename()` paths. Atomically renames a temp link in place. Updates `stats.filesLinked`/`bytesFreed`. Reports `resFileLinked` activity (with `st_blocks` on non-Windows). Hashes via NAR serialisation under SHA-256.
- `LocalStore::optimiseStore(OptimiseStats & stats)` — member. Iterates all valid paths via `queryAllValidPaths`, takes temp roots, optimises each one via `optimisePath_`. Drives an `Activity` progress meter.
- `LocalStore::optimiseStore()` — member overload. Runs the above with a fresh `OptimiseStats`, prints summary line `<bytes> freed by hard-linking <N> files`.
- `LocalStore::optimisePath(const std::filesystem::path & path, RepairFlag repair)` — member. If `config->getLocalSettings().autoOptimiseStore` is set, run a single-path optimisation.

### Type aliases / Macros / globals
- (none new; defined inline in `LocalStore`.)

---

## File: src/libstore/pathlocks.cc

### Namespaces
- `nix` — main namespace.

### Functions
- `PathLocks::PathLocks()` — default constructor; initialises `deletePaths(false)`.
- `PathLocks::PathLocks(const std::set<std::filesystem::path> & paths, const std::string & waitMsg)` — constructor; initialises `deletePaths(false)` and immediately calls `lockPaths(paths, waitMsg)` (uses default `wait = true`).
- `PathLocks::~PathLocks()` — destructor. Calls `unlock()` inside try/catch, swallowing exceptions via `ignoreExceptionInDestructor()`.
- `PathLocks::setDeletion(bool deletePaths)` — member; toggles the `deletePaths` flag.

### Type aliases / Classes / globals
- (none new; the cross-platform pieces are in unix/windows .cc files.)

---

## File: src/libstore/include/nix/store/pathlocks.hh

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- `enum LockType { ltRead, ltWrite, ltNone };` — bare (untyped) enum.
- `PathLocks` — class.
  - Private type alias `FDPair = std::pair<Descriptor, std::filesystem::path>` (typedef).
  - Private fields `std::list<FDPair> fds`, `bool deletePaths`.
  - Public constructors: default `PathLocks()`, `PathLocks(const std::set<std::filesystem::path> &, const std::string & waitMsg = "")`.
  - Inline-defined move constructor `PathLocks(PathLocks &&) noexcept` (uses `std::exchange`).
  - Inline-defined move assignment `operator=(PathLocks &&) noexcept`.
  - Deleted copy constructor `PathLocks(const PathLocks &) = delete;` and copy assignment `operator=(const PathLocks &) = delete;`.
  - Public methods: `lockPaths(const std::set<std::filesystem::path> &, const std::string & waitMsg = "", bool wait = true)` returning `bool`, destructor `~PathLocks()`, `unlock()`, `setDeletion(bool)`.
- `FdLock` — struct.
  - Fields `Descriptor desc`, `bool acquired = false`.
  - Constructor `FdLock(Descriptor desc, LockType lockType, bool wait, std::string_view waitMsg)`.
  - Deleted copy/move constructors and assignment operators.
  - Inline destructor: if `acquired`, calls `lockFile(desc, ltNone, false)`.

### Functions
- `openLockFile(const std::filesystem::path & path, bool create)` — declared, returns `AutoCloseFD`.
- `deleteLockFile(const std::filesystem::path &, Descriptor)` — declared.
- `lockFile(Descriptor desc, LockType lockType, bool wait)` — declared, returns `bool`.

### Type aliases / Macros / globals
- (none.)

---

## File: src/libstore/unix/pathlocks.cc

### Namespaces
- `nix` — main namespace.

### Functions
- `openLockFile(const std::filesystem::path & path, bool create)` — opens lock file with `O_CLOEXEC | O_RDWR | (create ? O_CREAT : 0)` mode `0600`. Throws `SysError` on failure unless `create` is false and the file simply doesn't exist (`ENOENT`).
- `deleteLockFile(const std::filesystem::path & path, Descriptor desc)` — `tryUnlink`s the path, then writes `"d"` to `desc` to signal "stale" to other waiters.
- `lockFile(Descriptor desc, LockType lockType, bool wait)` — wraps `flock` with `LOCK_SH`/`LOCK_EX`/`LOCK_UN`. When `wait` is true, retries until success (raising on non-`EINTR` errors); when false, uses `LOCK_NB` and returns false on `EWOULDBLOCK`. Calls `checkInterrupt()` between retries.
- `PathLocks::lockPaths(const std::set<std::filesystem::path> & paths, const std::string & waitMsg, bool wait)` — member. Asserts `fds.empty()`, then acquires per-path `<path>.lock` files in sorted order; tries non-blocking lock first, falls back to blocking after printing `waitMsg` if non-empty; retries when the held lock turns out to be stale (file size != 0). Returns `false` if a non-waiting lock fails. Pushes successfully acquired `(fd, lockPath)` onto `fds` (`fd.release()`).
- `PathLocks::unlock()` — member. For each held FD: optionally delete the lock file, then `close()` it (logs error on failure); clears `fds`.
- `FdLock::FdLock(Descriptor desc, LockType lockType, bool wait, std::string_view waitMsg)` — when `wait` is true, tries non-blocking lock first; on failure prints `waitMsg` via `printInfo` and falls back to blocking. When `wait` is false, just attempts a non-blocking lock.

### Type aliases / Classes / globals
- (none.)

---

## File: src/libstore/windows/pathlocks.cc

### Namespaces
- `nix` — main namespace.

### Functions
- `deleteLockFile(const std::filesystem::path & path, Descriptor desc)` — calls `DeleteFileW`; warns on failure (with `GetLastError()` rendered).
- `PathLocks::unlock()` — member. For each held FD: optionally delete the lock file, then `CloseHandle()` it (logs error on failure); clears `fds`.
- `openLockFile(const std::filesystem::path & path, bool create)` — `CreateFileW` with `GENERIC_READ|GENERIC_WRITE`, `FILE_SHARE_READ|FILE_SHARE_WRITE`, `OPEN_ALWAYS` or `OPEN_EXISTING`, `FILE_ATTRIBUTE_NORMAL | FILE_FLAG_POSIX_SEMANTICS`. Warns on `INVALID_HANDLE_VALUE`.
- `warnOrThrowWine<Args...>(DWORD lastError, const std::string & fs, const Args &... args)` — file-static template. Under Wine (`nix::windows::isWine()`), warns and returns true; otherwise throws `WinError`.
- `lockFile(Descriptor desc, LockType lockType, bool wait)` — switches on `LockType`. For `ltNone`, `UnlockFileEx` over `(0, 2)`. For `ltRead`, `LockFileEx` over `(0, 1)` with optional `LOCKFILE_FAIL_IMMEDIATELY`, then `UnlockFileEx` at offset 1 (range 1) ignoring `ERROR_NOT_LOCKED`. For `ltWrite`, `LockFileEx` with `LOCKFILE_EXCLUSIVE_LOCK` at offset 1, then `UnlockFileEx` at offset 0 ignoring `ERROR_NOT_LOCKED`. Returns false on `ERROR_LOCK_VIOLATION` when non-blocking; warns/throws on other errors via `warnOrThrowWine`.
- `PathLocks::lockPaths(const std::set<std::filesystem::path> & paths, const std::string & waitMsg, bool wait)` — member. Same protocol as the Unix version: open `<path>.lock`, exclusive lock, retry when stale (file size != 0 via `getFileSize`). Asserts `fds.empty()` first.
- `FdLock::FdLock(Descriptor desc, LockType lockType, bool wait, std::string_view waitMsg)` — same try/wait/printInfo pattern as the Unix version.

### Type aliases / Classes / globals
- (none.)

---

## File: src/libstore/unix/user-lock.cc

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- `SimpleUserLock` — struct, derives from `UserLock`.
  - Fields: `AutoCloseFD fdUserLock`, `uid_t uid`, `gid_t gid`, `std::vector<gid_t> supplementaryGIDs`.
  - Override methods: `getUID()` (asserts `uid` non-zero, returns it), `getUIDCount()` (always returns 1), `getGID()` (asserts `gid` non-zero, returns it), `getSupplementaryGIDs()` (returns the field).
  - Static `acquire(const std::filesystem::path & userPoolDir, const std::string & buildUsersGroup)` returning `std::unique_ptr<UserLock>`. Asserts `buildUsersGroup` non-empty; iterates members of `getgrnam(buildUsersGroup)`; opens per-uid lock file `userPoolDir / uid` with `O_RDWR|O_CREAT|O_CLOEXEC` mode 0600; attempts `lockFile(ltWrite, false)`; on Linux only populates supplementary GIDs (excluding the primary `gid`); asserts the chosen uid is not the current/effective user; returns `nullptr` if all users are taken. Throws if the group does not exist or has no members.
- `AutoUserLock` — struct, derives from `UserLock`.
  - Fields: `AutoCloseFD fdUserLock`, `uid_t firstUid = 0`, `gid_t firstGid = 0`, `uid_t nrIds = 1`.
  - Override methods: `getUID()` (asserts `firstUid` non-zero, returns it), `getUIDCount()` (returns `nrIds`), `getGID()` (asserts `firstGid` non-zero, returns it), `getSupplementaryGIDs()` (empty vector).
  - Static `acquire(const std::filesystem::path & userPoolDir, const std::string & buildUsersGroup, uid_t nrIds, bool useUserNamespace, const AutoAllocateUidSettings & uidSettings)`. Forces `useUserNamespace = false` on non-Linux. Requires `Xp::AutoAllocateUids`; asserts on settings invariants; iterates `slot-N` files; picks uid range `startId + i*maxIdsPerBuild`; throws if the uid clashes with an existing account; uses `firstGid = firstUid` when `useUserNamespace`, else looks up `buildUsersGroup` for `firstGid`. Returns `nullptr` if all slots are taken.

### Functions
- `get_group_list(const char * username, gid_t group_id)` — file-static (Linux only, `__linux__`). Wraps `getgrouplist` with retry-on-undersized-buffer using a heuristic (initial size 32, retry once; throws if the second attempt also fails).
- `acquireUserLock(const std::filesystem::path & stateDir, const LocalSettings & localSettings, uid_t nrIds, bool useUserNamespace)` — free function. If `localSettings.getAutoAllocateUidSettings()` returns non-null, uses `AutoUserLock::acquire` under `userpool2/`; else uses `SimpleUserLock::acquire` under `userpool/`. `createDirs` first.
- `useBuildUsers(const LocalSettings & localSettings)` — free function. Linux: true if (`buildUsersGroup` set or `autoAllocateUids`) and `isRootUser()`. Apple/FreeBSD: requires `buildUsersGroup` set and `isRootUser()`. Other: false. Result is cached in a function-local `static bool`.

### Type aliases / Macros / globals
- (none new.)

---

## File: src/libstore/unix/include/nix/store/user-lock.hh

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- `UserLock` — abstract struct.
  - Defined-but-empty virtual destructor `virtual ~UserLock() {}`.
  - Method `getUIDRange()` (non-virtual). Returns `std::pair<uid_t, uid_t> {first, first + count - 1}`.
  - Pure virtual: `getUID()`, `getUIDCount()` (declared returning `uid_t`), `getGID()`, `getSupplementaryGIDs()`.

### Functions
- `acquireUserLock(const std::filesystem::path & stateDir, const LocalSettings &, uid_t nrIds, bool useUserNamespace)` — declared.
- `useBuildUsers(const LocalSettings &)` — declared.

### Type aliases / Macros / globals
- Forward declaration `struct LocalSettings;`.

---

## File: src/libstore/machines.cc

### Namespaces
- `nix` — main namespace.

### Functions
- `Machine::Machine(const std::string & storeUri, decltype(systemTypes), decltype(sshKey), decltype(maxJobs), decltype(speedFactor), decltype(supportedFeatures), decltype(mandatoryFeatures), decltype(sshPublicHostKey))` — explicit constructor. Adds `ssh://` prefix to bare URIs unless they look like a known scheme/path/special token (`auto`, `daemon`, `local`, or any of those followed by `?` query), or already have `://` or any `/`. Clamps `speedFactor == 0.0f` to `1.0f`; throws `UsageError` on negative speed factor.
- `Machine::systemSupported(const std::string & system) const` — member. Returns true for `"builtin"` or any system in `systemTypes`.
- `Machine::allSupported(const StringSet & features) const` — member. True iff every requested feature is in `supportedFeatures` or `mandatoryFeatures`.
- `Machine::mandatoryMet(const StringSet & features) const` — member. True iff every `mandatoryFeatures` entry is also in the input set.
- `Machine::completeStoreReference() const` — member. Augments the parsed `StoreReference`: when scheme is `ssh`, adds `max-connections=1` and `log-fd=4`; when scheme is `ssh` or `ssh-ng`, adds `ssh-key` (if `sshKey` set) and `base64-ssh-public-host-key` (if `sshPublicHostKey` non-empty); always concatenates `supportedFeatures` and `mandatoryFeatures` (space-separated) into `system-features`.
- `Machine::openStore() const` — member. Returns `nix::openStore(completeStoreReference())`.
- `expandBuilderLines(const std::string & builders)` — file-static. Tokenises on `\n`, strips comments after `#`, then splits each remaining line on `;`, trims each entry; recursively expands `@file` references via `readFile` (skipping with debug message if file is `ENOENT`); returns `std::vector<std::string>`.
- `parseBuilderLine(const StringSet & defaultSystems, const std::string & line)` — file-static. Splits a line and returns a `Machine` populated from up to eight columns (`storeUri`, `systemTypes`, `sshKey`, `maxJobs`, `speedFactor`, `supportedFeatures`, `mandatoryFeatures`, `sshPublicHostKey`). Treats `""` and `"-"` as "unset". Throws `FormatError` on unparseable numbers; ensures column 7 (host key) is base64-decodable.
- `parseBuilderLines(const StringSet & defaultSystems, const std::vector<std::string> & builders)` — file-static. Maps `parseBuilderLine` over all expanded lines into a `Machines`.
- `Machine::parseConfig(const StringSet & defaultSystems, const std::string & s)` — static member. Composes `expandBuilderLines` and `parseBuilderLines`.

### Type aliases / Macros / globals
- (none new.)

---

## File: src/libstore/include/nix/store/machines.hh

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- Forward declarations: `class Store;`, `struct Machine;`.
- `Machine` — struct.
  - Const fields: `storeUri` (`StoreReference`), `systemTypes` (`StringSet`), `sshKey` (`std::optional<std::filesystem::path>`), `maxJobs` (`unsigned int`), `speedFactor` (`float`), `supportedFeatures` (`StringSet`), `mandatoryFeatures` (`StringSet`), `sshPublicHostKey` (`std::string`).
  - Mutable field: `bool enabled = true`.
  - Methods: `systemSupported(const std::string &) const`, `allSupported(const StringSet &) const`, `mandatoryMet(const StringSet &) const`, eight-arg constructor (using `decltype(field)` for each parameter type), `completeStoreReference() const`, `openStore() const` (returning `ref<Store>`), static `parseConfig(const StringSet &, const std::string &)`.

### Type aliases
- `Machines = std::vector<Machine>` (typedef).

### Macros / globals
- (none.)

---

## File: src/libstore/include/nix/store/length-prefixed-protocol-helper.hh

### Namespaces
- `nix` — main namespace.

### Classes / structs / enums
- `LengthPrefixedProtoHelper<Inner, T>` — primary template (forward declaration only; no body).
- Specialisations (created via the `LENGTH_PREFIXED_PROTO_HELPER` macro):
  - `LengthPrefixedProtoHelper<Inner, std::vector<T>>`.
  - `LengthPrefixedProtoHelper<Inner, std::set<T, Compare>>` (uses the `COMMA_` macro to embed the second template arg).
  - `LengthPrefixedProtoHelper<Inner, std::tuple<Ts...>>`.
  - `LengthPrefixedProtoHelper<Inner, std::map<K, V, Compare>>` (uses `LENGTH_PREFIXED_PROTO_HELPER_X` and `COMMA_`).
- Each specialisation contains:
  - Public static `T read(const StoreDirConfig &, typename Inner::ReadConn)`.
  - Public static `void write(const StoreDirConfig &, typename Inner::WriteConn, const T & str)`.
  - Private using-template `template<typename U> using S = typename Inner::template Serialise<U>;`.
- Inline (template) implementations of `read`/`write` for each container type:
  - `vector`: prefix with size; `S<T>::read`/`write` per element using `push_back`.
  - `set`: same shape, inserted with `set.insert`.
  - `map`: `read` calls `S<K>::read`, `S<V>::read` and uses `insert_or_assign`.
  - `tuple`: `read` brace-initialises a tuple of `S<Ts>::read(...)` results in pack-expansion order; `write` uses `std::apply` with a generic lambda.

### Functions
- (Member `read`/`write` on the partial specialisations; no free functions.)

### Type aliases / Macros / globals
- Forward declaration `struct StoreDirConfig;`.
- `#define LENGTH_PREFIXED_PROTO_HELPER(Inner, T) ...` — emits a struct specialisation declaration with `read`/`write` plus the private `S` alias template.
- `#define COMMA_ ,` — workaround so a comma can appear inside a macro arg list (used to pass `std::set<T, Compare>` and `std::map<K, V, Compare>` types). `#undef COMMA_` after the map specialisation.
- `#define LENGTH_PREFIXED_PROTO_HELPER_X std::map<K, V, Compare>` — local macro used only for the map specialisation; not undefined (a minor leak).

---

## Cross-file observations

- **Two `DerivationOutputs` typedefs**: `typedef std::map<std::string, DerivationOutput> DerivationOutputs;` is defined identically in both `nix/store/derivations.hh` and `nix/store/parsed-derivations.hh`. The duplication is structural (the latter forward-declares `DerivationOutput` and is included by the former), but could be hoisted to a single header.
- **Repeated `std::visit + overloaded` dispatch tables**: the `DerivationOutput` and `DerivationType` variants are visited in many functions in `derivations.cc` (`path`, `unparse`, `type`, `processDerivationOutputPaths`, JSON `to_json`, `writeDerivation`, `BasicDerivation::type`). All carry the same five-or-three branch structure; an iteration utility or strategy table could centralise this.
- **PathLocks has near-identical Unix/Windows implementations**: `lockPaths` (sorted iteration, retry on stale lock detected by `<file>.lock` not being empty) and `FdLock`'s constructor (try non-blocking, then blocking with `printInfo(waitMsg)`) are duplicated almost verbatim between `unix/pathlocks.cc` and `windows/pathlocks.cc`. The differences are confined to the actual `flock` vs `LockFileEx` calls and the test for "lock file became stale" (`fstat().st_size` vs `getFileSize`). Could share a generic skeleton with platform shims for `openLockFile`/`lockFile`/`isStale`. Note the Windows `unlock` uses `CloseHandle` and the `lockFile` implementation models shared/exclusive locks via two adjacent OVERLAPPED ranges (offsets 0 and 1) rather than a single byte-range mode.
- **Generation deletion helpers in profiles.cc** all share a `lockProfile` + `findGenerations` + per-gen iteration prologue (`deleteGenerations`, `deleteGenerationsGreaterThan`, `deleteOldGenerations`, `deleteGenerationsOlderThan`, `switchGeneration`). A common driver pattern parametrised on a predicate (and possibly using `iterDropUntil`) would remove the boilerplate.
- **JSON helpers are duplicated** between `derivation-options.cc` (templated `outputChecksFromJson`/`outputChecksToJson`, `derivationOptionsFromJson`/`derivationOptionsToJson`) and the four near-identical `adl_serializer` specialisations (one pair per `<SingleDerivedPath>`/`<StorePath>` × `OutputChecks`/`DerivationOptions`) that simply delegate to those templates. The boilerplate could collapse to one specialisation behind a macro.
- **`getStringAttr`/`getBoolAttr`/`getStringSetAttr` in derivation-options.cc** all share the same "structured first, fall back to env" structure. A single template parameterised on the JSON converter and the env fallback parser would dedupe these.
- **Two `acquire` implementations in user-lock.cc** (`SimpleUserLock`, `AutoUserLock`) both: open a per-slot lockfile, try non-blocking exclusive lock via `lockFile(ltWrite, false)`, populate the lock object on success. The lock-acquisition skeleton could be shared.
- **JSON adl_serializer specialisations for `Derivation` and `BasicDerivation`** call `basicDerivationToJson` then add an `inputs` key in slightly different shapes (flat array of `inputSrcs` for `BasicDerivation`; structured `{srcs, drvs}` object with recursive `outputs`/`dynamicOutputs` per-node for `Derivation`). The reverse side shares `basicDerivationFromJson`; the diverging input encoding is concentrated only in those two `to_json`/`from_json` overloads.
- **Variants with `MAKE_WRAPPER_CONSTRUCTOR`**: `DerivationOutput`, `DerivationType`, `DrvHashModulo` all use the same wrapper-constructor + `Raw` typedef + `raw` field idiom. `DerivationOutput` and `DerivationType` additionally delete the default constructor; `DrvHashModulo` does not. Worth noting because shared helpers (visiting, comparison, JSON) could exploit the common shape.
- **`drvHashes` is a global** memoisation map (`boost::concurrent_flat_map`); `getBuildLog` uses a virtual two-step (`getBuildDerivationPath` + `getBuildLogExact`) that mirrors several other store-side caches.
- **`length-prefixed-protocol-helper.hh`** uses the `LENGTH_PREFIXED_PROTO_HELPER` macro + `COMMA_` workaround to support `set<T, Compare>` and `map<K, V, Compare>` inside macro arguments. `COMMA_` is `#undef`-ed after the map specialisation; `LENGTH_PREFIXED_PROTO_HELPER_X` is left defined after this file, which is a minor leak.
- **`DerivationOptions<Input>::OutputChecks` parametrisation**: the `DrvRef` member alias is `nix::DrvRef<Input>`, which is `std::variant<OutputName, Input>` (judging by the consumers in `derivation-options.cc`). The "ignoreSelfRefs" field defaults to `false` in the JSON path but is set to `true` in the legacy non-structured-attributes branch of `derivationOptionsFromStructuredAttrs`, an asymmetry worth documenting.
- **`Derivation::checkInvariants` and `fillInOutputPaths` share `processDerivationOutputPaths<bool>`**: the same template body handles both validation and fill-in via the `fillIn` non-type template parameter (with `if constexpr` branches). After processing, both paths call `drv.type()` purely as an invariant assertion.
- **`useBuildUsers` returns a function-local `static bool`** — its result is computed once and cached for the process lifetime. This is fine for single-store processes but means changes to `localSettings` after first call are not observed.
