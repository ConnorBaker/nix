# Multi-implementation patterns that could share a base

Candidates 36-47. Mostly VALID; #38, #40, #42, #43, #45 PARTIALLY VALID; #44
OBSOLETE (the `BuildLog`/`LogSink` overlap is genuine but only ~5 lines).

| # | Verdict | Effort |
| - | ------- | ------ |
| 36 | VALID | medium |
| 37 | VALID | small |
| 38 | PARTIALLY VALID | small |
| 39 | VALID | medium |
| 40 | PARTIALLY VALID | small |
| 41 | VALID | medium |
| 42 | PARTIALLY VALID | medium |
| 43 | PARTIALLY VALID | medium |
| 44 | OBSOLETE | — |
| 45 | PARTIALLY VALID | small |
| 46 | VALID | small |
| 47 | VALID | trivial |

---

36. **Five parallel value renderers.** `Printer::print` (`print.cc`), `printAmbiguous` (`print-ambiguous.cc`), `printValueAsJSON` (`value-to-json.cc`), `printValueAsXML` (`value-to-xml.cc`), plus the AST-side `Expr::show` (`nixexpr.cc`). All five share: `state.forceValue` (or check `force`/`strict` option), `state.settings.maxCallDepth` / `addCallDepth(...)` recursion guard, recursion with a per-printer `seen`/`drvsSeen`/`Done` set, derivation special-casing (`state.s.drvPath` / `state.isDerivation`), per-`nValueType` switch. A shared visitor scaffold could leave each printer implementing only per-type emission.
    - ../verified/12-libexpr-parse.md
    - **Validation:** VALID. Dispatch is over `v.type() == nValueType`, so CRTP with deducing-this fits better than `std::variant` + `std::visit`. Cross-references #110 (the four parallel "seen" sets). **See also:** N20 (the `nValueType` table-driven dispatch — five renderers here, plus `force*` from #211, plus `prim_is*` from #66, all consume one constexpr `valueTypeTraits[]` table; this candidate is one of three slices). Effort: medium.

37. **NAR-tree walkers in three places.** `NarAccessorImpl::find/get` (in `nar-accessor.cc`), `NarIndexer::createMember` (in `nar-listing.cc`), `MemorySourceAccessor::open` (in `memory-source-accessor.cc`). All walk a path, descend into the `Directory` variant, and either find or insert. A generic helper over `fso::VariantT` could replace all three.
    - ../verified/01-libutil-io.md
    - **Validation:** VALID. Effort: small.

38. **Source/Sink wrapper hierarchy with many parallel one-shot adapters.** `TeeSink`/`TeeSource`, `LengthSink`/`LengthSource`, `LambdaSink`/`LambdaSource`, `SizedSource`, `EnsureRead`, `ChainSource`. Written one-by-one; a base or generator could reduce repetition.
    - ../verified/01-libutil-io.md
    - **Validation:** PARTIALLY VALID. Several of these adapters have non-trivial state (chunking, length tracking, error injection); a unifying base would only deduplicate the constructor/forwarding shape, not the semantics. Pitfall: the named structs have stable types that callers store as members (e.g. `LengthSink` inside `HashedSink`-style users); switching to factory-returning-`unique_ptr` breaks ABI. **Forward-looking:** new sites continue to grow new single-purpose subclasses (`LogSink`, the Brotli sinks); a `makeObserverSink(innerSink, observer)` factory would prevent that growth without renaming existing types. **See also:** N6 (`WrappingSink<Inner, Observer>` template + `splitOnDelimiter` adapter — `LogSink` and `BuildLog` are two slices of the same line-buffer family), N36 (the `make*Sink` factory pattern is parallel — construction is hand-rolled differently per site). Recommendation: leave as-is unless touching. Effort: small for the boilerplate consolidation only.

39. **`MemorySink`/`RestoreSink` parallel `FileSystemObjectSink` impls.** Both implement essentially the same `FileSystemObjectSink` interface plus their own `CreateRegularFileSink` subclass for byte streams (`RestoreRegularFile` and `CreateMemoryRegularFile`). The bytes-callback boilerplate could be factored.
    - ../verified/01-libutil-io.md
    - **Validation:** VALID. Each `CreateRegularFileSink` subclass is a bytes-callback wrapper threading state through the sink; genuine factoring requires a virtual-or-callback design choice that affects every NAR consumer. **See also:** N6 (the `WrappingSink<Inner, Observer>` template + `splitOnDelimiter` adapter — this is one slice of the broader Source/Sink wrapping pattern). Effort: medium.

40. **Wrapping source accessors clear `displayPrefix` and chain `showPath`/`getPhysicalPath`/`invalidateCache` near-identically.** `UnionSourceAccessor`, `MountedSourceAccessorImpl`, `CachingSourceAccessor` all redo this in their constructors. Their `getFingerprint` implementations diverge: caching is a passthrough; mounted/union apply "own fingerprint else delegate". A `WrappingSourceAccessor` base could absorb all three.
    - ../verified/01-libutil-io.md
    - **Validation:** PARTIALLY VALID. The constructor boilerplate genuinely shares; `getFingerprint` divergence is real and would need a small policy hook. Effort: small.

41. **Logger fan-out duplication.** `SimpleLogger`, `JSONLogger`, `TeeLogger` independently implement `startActivity`/`stopActivity`/`result`/`log`/`logEI`/`writeToStdout`/`ask`/`setPrintBuildLogs`. A common base or visitor would deduplicate.
    - ../verified/03-libutil-runtime.md
    - **Validation:** VALID. The natural fix is a `LoggerVisitor` pattern dispatching on `std::variant<LogEvent::StartActivity, …>` so `TeeLogger` becomes a single `void emit(LogEvent)` that fans out. Pitfall: external loggers (e.g. `progress-bar.cc`) inherit `Logger` and override these methods — collapsing the virtuals into a variant is a public ABI change. Effort: medium.

42. **`Config`/`GlobalConfig` plumbing duplication.** `Config::set`/`getSettings`/`toJSON`/`toKeyValue`/`convertToArgs` and `GlobalConfig::set`/`getSettings`/`toJSON`/`toKeyValue`/`convertToArgs` follow identical loop shapes (one subtle behaviour difference in `toKeyValue`: `Config` only emits aliases; `GlobalConfig` calls `globalConfig.getSettings(...)` and emits all entries). A CRTP base or composition would let `GlobalConfig` simply iterate registered configs.
    - ../verified/03-libutil-runtime.md
    - **Validation:** PARTIALLY VALID. The "subtle behaviour difference" is load-bearing — `GlobalConfig::toKeyValue` is the entrypoint for `nix show-config`, which expects all entries. Refactor must preserve. Compounds with #136. Effort: medium.

43. **X-macro setting list.** The `BaseSetting<T>` template-specialisation pattern (`parse`, `to_string`, `appendOrSet`, `convertToArg`, `trait::appendable`, `NIX_DECLARE_CONFIG_SERIALISER`, explicit `template class BaseSetting<...>` instantiations) is replicated for many `T` in `configuration.cc`. The list (`std::list<path>`, `Strings`, `StringSet`, `std::set<path>`, `std::set<ExperimentalFeature>`, `StringMap`, `AbsolutePath`, plus the per-store specialisations) plus the trait specialisations plus the macro plus the explicit instantiations form four near-parallel registers. A single X-macro list would collapse them.
    - ../verified/03-libutil-runtime.md, ../verified/07-libstore-local.md
    - **Validation:** PARTIALLY VALID. Several specialisations have non-trivial parse semantics (path-list resolution, alias handling) that don't reduce to a single X-macro. Realistic consolidation is to a tag-dispatched template family with X-macro instantiation only for the simple cases. **The simple-case half is blocked by #136** (the X-macro driver doesn't work without #136 because the constructor self-registration prevents declaring settings as plain members). **See also:** N4 (X-macro `*Settings` struct driver), N22 (macros emitting struct declarations — plus `NIX_DECLARE_CONFIG_SERIALISER` is one of the broader "9 X-macro families" group of N35), N44 (per-T `BaseSetting<T>::trait` ad-hoc specialisation pattern). Effort: medium.

44. **`BuildLog` and `LogSink` are similar line-buffering sinks.** `BuildLog` (in `build-log.{cc,hh}`) and `LogSink` (in `derivation-building-goal.cc`) both implement `Sink::operator()` as a line buffer that splits on `\n`. `BuildLog` adds JSON parsing, tail tracking, and `\r` carriage-return handling; `LogSink` is simpler. They could share a base.
    - ../verified/10-libstore-build.md
    - **Validation:** OBSOLETE. The shared scaffold is only ~5 lines. Worth noting in the inventory, not actionable. Compounds with #157.

45. **Three NAR magic-prefixed switches.** `archive.cc` and `nar-listing.cc` independently implement near-identical switches over file-system-object types (regular/directory/symlink/...); `tarfile.cc` does the same over `archive_entry_filetype`. A unifying visitor over the tri-typed FSO tag would reduce repetition.
    - ../verified/01-libutil-io.md
    - **Validation:** PARTIALLY VALID. The `archive_entry_filetype` taxonomy is not isomorphic to the NAR FSO taxonomy (libarchive distinguishes seven values: regular, link, symlink, socket, character device, block device, directory, fifo). Realistic factoring is a NAR-side `visitFSO<RC>` helper for `archive.cc`/`nar-listing.cc` plus a one-liner libarchive adapter `fromArchiveEntry(...) -> std::optional<fsoType>`. Effort: small.

46. **`builtinBuilders` family share a `RegisterBuiltinBuilder` registration pattern.** All three (`buildenv`, `fetchurl`, `unpack-channel`) follow the same shape: static function `void(const BuiltinBuilderContext &)` plus a file-scope `static RegisterBuiltinBuilder` instance. Each uses a local `getAttr` lambda for env-attribute lookup. The local lambda is a candidate for a shared helper.
    - ../verified/10-libstore-build.md
    - **Validation:** VALID. The `getAttr` lambda is genuinely repeated, but each builtin's body has irreducibly per-builtin behaviour; the trivial label assumes the helper extraction is one-line per site, which is true but the helper itself needs design. Effort: small.

47. **`DerivationGoal`/`DerivationBuildingGoal::doneSuccess`/`doneFailure` are paired overrides with mostly-shared bodies.** Both override the protected `doneSuccess`/`doneFailure` with the same shape: drop a `MaintainCount` handle, increment `worker.doneBuilds++` / `failedBuilds++`, call `worker.updateProgress`, then forward to `Goal::doneSuccess`/`doneFailure`. The `doneFailure` bodies are byte-identical except for the handle name (`mcExpectedBuilds.reset()` vs `mcRunningBuilds.reset()`); the `doneSuccess` bodies diverge only in how `builtOutputs` is shaped (one wraps a single output keyed by `wantedOutput`, the other forwards a multi-output map). The other four concrete `Goal` subclasses (`DerivationTrampolineGoal`, `DerivationResolutionGoal`, `PathSubstitutionGoal`, `DrvOutputSubstitutionGoal`) use the base `Goal::doneSuccess`/`doneFailure` directly.
    - ../verified/10-libstore-build.md
    - **Validation:** VALID. Cross-references #33. Effort: trivial.
