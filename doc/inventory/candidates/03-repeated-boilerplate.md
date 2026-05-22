# Repeated boilerplate

Candidates 17-25. All nine VALID. Note that #17 (`anchor()` vtable-pinning) is
boilerplate that **cannot be removed** — the comment on `Store::anchor()`
documents that it exists "to avoid weak linkage of the vtable - it breaks
dynamic_cast across shared libraries on Darwin", and modern lld on Darwin
still requires an out-of-line virtual to anchor weak vtables across `.dylib`
boundaries. The refactor (CRTP/macro) consolidates the boilerplate without
removing the mechanism.

| # | Verdict | Effort |
| - | ------- | ------ |
| 17 | VALID | trivial (consolidation only — mechanism is load-bearing) |
| 18 | VALID | small |
| 19 | VALID | medium |
| 20 | VALID | medium |
| 21 | VALID | small |
| 22 | VALID | small (in isolation) — subsumed by N8 if asio migration lands first |
| 23 | VALID | large |
| 24 | VALID | trivial |
| 25 | VALID | trivial |

---

17. **`anchor()` vtable-pinning override appears in twelve+ classes.** Every class with virtual functions in the libstore shards defines a private out-of-line `void anchor() override {}` whose only purpose is to pin the vtable. Used in `StoreConfig`, `Store`, `LocalStoreConfig`, `LocalBuildStoreConfig`, `LocalStore`, `LocalFSStoreConfig`, `LocalFSStore`, `LocalOverlayStoreConfig`, `LocalOverlayStore`, `IndirectRootStore`, `GcStore`, `LogStore`, `DummyStoreConfig`, `DummyStore`, `DummyStoreImpl`, `RestrictedStore`, `RemoteStoreConfig`, `RemoteStore`, `UDSRemoteStoreConfig`, `UDSRemoteStore`, `BinaryCacheStoreConfig`, `BinaryCacheStore`, `LocalBinaryCacheStoreConfig`, `LocalBinaryCacheStore`, `HttpBinaryCacheStoreConfig`, `HttpBinaryCacheStore`, `SSHStoreConfig`, `SSHStore`, `MountedSSHStoreConfig`, `MountedSSHStore`, `LegacySSHStoreConfig`, `LegacySSHStore`, `CommonSSHStoreConfig`, `S3BinaryCacheStore`. A macro or CRTP could eliminate this entirely.
    - ../verified/05-libstore-core.md, ../verified/07-libstore-local.md, ../verified/08-libstore-remote.md
    - **Validation:** VALID — but the mechanism is load-bearing on Darwin (weak-vtable dynamic_cast across dylibs). Refactor must preserve the out-of-line virtual; only the boilerplate (~31 explicit overrides) can be folded into a `NIX_VTABLE_ANCHOR()` macro or CRTP. Effort: trivial.

18. **`MakeError(name, parent)` macro coexists with hand-written `CloneableError<Derived,Parent>` derivations.** Most error types use the macro; a handful (`ExecError`, `MissingExperimentalFeature`, `BuildError`, `BuilderFailureError`, `MissingRealisation`, `AwsAuthError`, `FileTransferError`, `InvalidSSHAuthority`, `NotDeterministic`, `BuildEnvFileConflictError`, `TimedOut`, `curlMultiError`, `SymlinkNotAllowed`, `SQLiteError`) bypass the macro to add fields. A CRTP base could subsume both styles. The `SystemError`/`SysError`/`WinError` triad's `DisambigHintFmt`/`DisambigVarArgs` tag idiom is similarly replicated.
    - ../verified/03-libutil-runtime.md, ../verified/04-libutil-misc.md
    - **Validation:** VALID — body rewritten per B3's source-level verification. **Count corrected:** the candidate body lists 14 hand-written `CloneableError` derivations; B3 found **17 visible via `grep -rEn '(class|struct) \w+( final)?\s*:\s*(public\s+)?CloneableError' src/`** (the `MakeError` macro template line is excluded; the included subset spans `EvalBaseError`, `StackOverflowError`, `InvalidPathError`, `BadNixStringContextElem`, `GitError`, `ExecError`, `MissingExperimentalFeature`, `SymlinkNotAllowed`, `SystemError`, `SysError`, `WinError`, `InvalidSSHAuthority`, `FileTransferError`, `AwsAuthError`, `BuildError`, `MissingRealisation`, `BuildEnvFileConflictError`). The original "14" was undercounted — six libexpr/libfetchers cases were missed (`EvalBaseError`, `StackOverflowError`, `CachedEvalError`, `InvalidPathError`, `BadNixStringContextElem`, `GitError`). **18a is not a real refactor target:** B3 verified that `MakeError` *already is* CRTP sugar (the macro emits `class name final : public CloneableError<name, parent>`). The catalog's framing "the macro could be subsumed by a CRTP base" was mistaken; the only refactor that resembles 18a is migrating the no-field hand-written cases (`InvalidSSHAuthority`, `NotDeterministic`, `GitError`, `MissingRealisation`, `StackOverflowError`) to `MakeError`, which is properly part of 18b. **18b is per-class:** most of the hand-written derivations carry intrinsic payload data (`errorCode`, `status`, `missingFeature`, `path`, etc.) that cannot be subsumed by a generic CRTP without reflection. The realistic refactor is consolidate the ~5 no-fields cases plus audit ctor surfaces — not "subsume both styles into one CRTP". **18c is too small for top-level status:** the `SystemError`/`SysError`/`WinError` triad's `DisambigHintFmt`/`DisambigVarArgs` tag idiom is local to three classes; keep as a sub-bullet of 18b. **Cross-link to flag:** `MakeError(SQLiteBusy, SQLiteError)` is the *only* `MakeError` whose parent is a hand-written `CloneableError` — the protected-ctor inheritance through `using CloneableError::CloneableError` is subtle and load-bearing; document. **Fragility to flag:** `BadNixStringContextElem` and `GitError` both mutate `err.msg` after `CloneableError("")` — depends on `BaseError::err` being `protected mutable`; surface this and consider replacing with a factory or named-ctor. **Decision:** keep #18 umbrella; do not renumber to 18a/18b/18c (the split would create three IDs for what's essentially one and a half real refactors, and breaks every existing `#18` cross-reference). **See also:** B3, N22 (macros emitting struct declarations — `MakeError`, `MakeBinOp`, `MAKE_WRAPPER_CONSTRUCTOR` share fate), R9. Effort: small (no-field migration) / medium (per-class field-bearing audit) / trivial (`Disambig*` tag idiom).

19. **`Setting<T>{this, default, name, R"(doc)", aliases, xpFeature}` initialisers dominate `*Config` boilerplate.** Every store config and settings struct uses this same shape; explicit instantiations for each `T` are scattered across many files. A small flag-builder DSL or constexpr table would shrink the source dramatically, especially in `LocalSettings`, `Settings`, `RemoteStoreConfig`, `BinaryCacheStoreConfig`, `HttpBinaryCacheStoreConfig`, `S3BinaryCacheStoreConfig`, `WorkerSettings`, `flake::Settings`, the `Mix*` command tower.
    - ../verified/03-libutil-runtime.md, ../verified/07-libstore-local.md, ../verified/08-libstore-remote.md, ../verified/09-libstore-protocol.md, ../verified/16-libcmd.md
    - **Validation:** VALID. Any DSL must respect data-member declaration order — some defaults read earlier settings (e.g. `LocalFSStoreConfig::stateDir` reads `rootDir.get()`). **Blocked by #136:** the X-macro driver doesn't work without #136 because the constructor self-registration prevents declaring settings as plain members. **Count correction:** N4's verified count is 21-22 settings structs (catalog body's "19+" is a mild undercount). **See also:** N4 (X-macro `*Settings` struct driver — alternative keystone), N44 (per-T `BaseSetting<T>::trait` specialisation pattern). Effort: medium.

20. **`MAKE_WRAPPER_CONSTRUCTOR(T)` + `Raw raw` member + visit-with-overloaded idiom appears for every variant-shaped type.** `ContentAddressMethod`, `ContentAddressWithReferences`, `OutputsSpec`, `ExtendedOutputsSpec`, `SingleDerivedPath`/`DerivedPath`, `RealisedPath`, `StorePathWithOutputs::ParseResult`, `StoreReference::Variant`, `BuildResult::inner`, `DrvRef<Item>`, `DerivationOutput::Raw`, `DerivationType::Raw`, `DrvHashModulo::Raw`. Each pair re-defines `==`, `to_string`, and `parse` symmetrically. A tagged-union helper would consolidate.
    - ../verified/05-libstore-core.md, ../verified/06-libstore-derivations.md
    - **Validation:** VALID. C++23 deducing-this on a `TaggedUnion<Tag, Variants...>` template is the cleanest factoring; `boost::hana::overloaded` does not improve over the local `overloaded` already in `util.hh`. Effort: medium.

21. **`Setting<T>` BaseSetting specialisations have a four-times-cut-and-pasted shape.** Each specialisation declares `BaseSetting<T>::trait`, defines `parse`/`to_string`, and (for collections) `appendOrSet`. Repeated for `SandboxMode`, `PathsInChroot`, `LocalSettings::ExternalBuilders`, `StoreReference`, `std::vector<StoreReference>`, `std::set<StoreReference>`, `CompressionAlgo`, `std::optional<CompressionAlgo>`, `S3AddressingStyle`, `Diagnose`. There is no shared template for "tokenise-list setting" or "JSON-roundtripping setting".
    - ../verified/02-libutil-data.md, ../verified/07-libstore-local.md, ../verified/08-libstore-remote.md, ../verified/11-libexpr-eval.md
    - **Validation:** VALID. Effort: small.

22. **Async/sync pair pattern.** `Store::queryPathInfo` and `Store::queryRealisation` each have a synchronous `promise/future` wrapper around a `Callback`-based async variant; the wrapper code is structurally identical and could be factored into a helper template that turns any async callback into a blocking call.
    - ../verified/05-libstore-core.md
    - **Validation:** VALID. `HttpBinaryCacheStore::topoSortPaths` already uses `callbackToAwaitable`, so the coroutine helper exists and could be repurposed. Pitfall: `Callback::rethrow` semantics (per-callback exception capture) must be preserved to keep `noexcept` correct (non-trivial reasoning across each call site; trivial undersells it). **Sequencing:** small *if done in isolation*; **subsumed** by N8 if asio migration lands first (the codebase has three async idioms — `Callback<T>`, `awaitable<T>`, `Goal::Co` — and the right move is to converge on `awaitable<T>` rather than invest in a tactical sync wrapper that gets undone by #160). Effort: small (in isolation).

23. **`unsupported(...)` is used as a default for many virtuals.** `Store::queryAllValidPaths`, `Store::queryReferrers`, `Store::addSignatures`, plus the `*::repairPath` and most overrides in `RestrictedStore` and `LegacySSHStore`. This is a workaround in lieu of pure-virtual + capability traits; explicit feature negotiation would be cleaner.
    - ../verified/05-libstore-core.md
    - **Validation:** VALID. Properly fixing this means moving methods into capability traits (`GcStore`, `LogStore`, `IndirectRootStore`) and threading `requires<...>` everywhere. Pitfall: callers do `auto & gcStore = require<GcStore>(*store)` — this idiom must be uniformly applied at every site, not opportunistically. Effort: large (structural).

24. **`registerCommand<...>` boilerplate is uniform across 30+ command files.** Every modern `nix` command file ends with `static auto rXxx = registerCommand<CmdXxx>("xxx")`, with naming irregular (`rCmdXxx`, `r2`, `rFormatterRun`, `rShowConfig`, etc.). The legacy bridges follow the same template via `RegisterLegacyCommand`. A macro encoding name + class + category could shrink each file by several lines.
    - ../verified/17-nix-modern-1.md, ../verified/18-nix-modern-2-legacy.md
    - **Validation:** VALID. Effort: trivial.

25. **`doc()` overrides include a per-command `*.md` (or `*.md.gen.hh`) via `#include`.** Every command does `return ` `#include "name.md"` `;`. Stable but heavy boilerplate.
    - ../verified/17-nix-modern-1.md, ../verified/18-nix-modern-2-legacy.md
    - **Validation:** VALID. A `NIX_COMMAND_DOC(name)` macro (ideally folded into the #24 registration macro so name and doc filename share one source of truth) replaces the body. Pitfall: Meson's `*.md.gen.hh` generation expects the include path to match the source file, so any macro-stringified parameter has to match what the build system already recognises. Effort: trivial.
