# Inheritance chains worth flattening

Candidates 30-35. Mixed verdicts: #30, #32-34 VALID; #31 and #35 PARTIALLY
VALID — those have additional concerns called out in the validation
paragraphs.

| # | Verdict | Effort |
| - | ------- | ------ |
| 30 | VALID | trivial |
| 31 | PARTIALLY VALID | medium |
| 32 | VALID | large |
| 33 | VALID | structural |
| 34 | VALID | structural |
| 35 | PARTIALLY VALID | structural |

---

30. **`Installable` chain (Installable → InstallableValue → InstallableAttrPath / InstallableFlake; InstallableDerivedPath sibling).** The two leaves' `toDerivedPaths` perform the same `ExtendedOutputsSpec` visit (`Default` synthesises `outputsToInstall` then defaults to `{"out"}`; `Explicit` returns the spec verbatim) and the same `trySinglePathToDerivedPaths` short-circuit. Extracting an `outputsSpecFromExtended` helper plus a templated `InstallableValue` method would deduplicate.
    - ../verified/16-libcmd.md
    - **Validation:** VALID. Effort: trivial.

31. **Command `run` chain (StoreConfigCommand → StoreCommand → BuiltPathsCommand → StorePathsCommand → StorePathCommand).** Each step does nothing more than narrow the type via virtual `run` overloads. The chain is fragile to GCC's `-Woverloaded-virtual` warning (suppressed by a pragma in `command.hh`). A templated CRTP-style narrowing would flatten it.
    - ../verified/16-libcmd.md
    - **Validation:** PARTIALLY VALID. The chain is not pure narrowing — each layer adds real construction concerns (config lookup, eval state, source-expr handling) beyond the type-narrow. The `-Woverloaded-virtual` suppression is the smoking gun and removable; the layers themselves are not all collapsible. Effort: medium for the parts that actually narrow only.

32. **`InputScheme` hierarchy with nine schemes and significant per-scheme rewrite.** Every scheme registers itself at startup via `static auto rXxx = OnStartup([] { registerInputScheme(make_unique<XxxInputScheme>()); });` (the same idiom verbatim across all nine concrete schemes: `IndirectInputScheme`, `PathInputScheme`, `GitInputScheme`, `MercurialInputScheme`, `GitHubInputScheme`, `GitLabInputScheme`, `SourceHutInputScheme`, `FileInputScheme`, `TarballInputScheme`; the abstract `GitArchiveInputScheme` and `CurlInputScheme` bases factor out forge-tarball and curl-download logic respectively). Every scheme reimplements URL parsing, attribute strip+re-emit, cache-key construction, fingerprint computation. A `BaseInputScheme<T>` CRTP with `urlGrammar`, `cacheKeyDomain`, etc. would replace much of the duplication.
    - ../verified/14-libfetchers.md
    - **Validation:** VALID. Pitfall: nine schemes with subtle URL-parsing divergence; a botched refactor risks silent behavioural regressions in flake fetching. Split into per-scheme PRs. Effort: large rather than medium when accounting for that staging.

33. **`Goal` hierarchy (six concrete goal types).** Common shape across all six: `init`/`gaveUpOnSubstitution`/`tryToBuild`/etc. coroutines, `key` override, `jobCategory` override, `MaintainCount<uint64_t>` counter handles. Only `DerivationGoal` and `DerivationBuildingGoal` override the protected `doneSuccess`/`doneFailure` (the other four goals use the base `Goal::doneSuccess`/`doneFailure` directly); their `doneFailure` bodies are byte-identical except for the `MaintainCount` handle name (`mcExpectedBuilds` vs `mcRunningBuilds`), and their `doneSuccess` bodies share the same reset → conditional `worker.doneBuilds++` → `worker.updateProgress()` → forward shape but differ in how they construct `builtOutputs` (single output keyed by `wantedOutput` vs multi-output map already supplied).
    - ../verified/10-libstore-build.md
    - **Validation:** VALID. The bespoke `Goal::Co` coroutine machinery (`promise_type`, `Awaiter`, `await_transform(AsyncCallback<T>)`) is already a hand-rolled `boost::asio::awaitable<T>` look-alike; migrating to `asio::awaitable` plus `asio::co_spawn`/`asio::any_io_executor` would let `Worker::waitForCompletion`, the per-goal `ChildEvents` queue, and the worker's `Waker` self-pipe (cross-ref #152) collapse. Effort: structural (central scheduling primitive; cannot be done in one shot). Phases: extract `MaintainCount` book-keeping into a shared mixin (trivial; cross-refs #47); move `Awaiter` to a thin asio adapter (medium); migrate `Worker` loop to asio executors (large).

34. **`DerivationBuilder` diamond (see #28).** A more disciplined strategy/policy split (cgroup strategy vs jail strategy vs sandbox-init strategy, plus a separate "needs hash rewrite" trait) would let a single `UnixDerivationBuilder` be parameterised on the platform pieces.
    - ../verified/10-libstore-build.md
    - **Validation:** VALID. Effort: structural. The strategy split is also relevant for mock builds in tests (#155).

35. **`Store` hierarchy with eleven concrete stores.** `LocalStore`, `LocalOverlayStore`, `DummyStoreImpl` (the concrete subclass behind `DummyStore`), `RestrictedStore`, `UDSRemoteStore`, `LocalBinaryCacheStore`, `HttpBinaryCacheStore`, `S3BinaryCacheStore`, `SSHStore`, `MountedSSHStore`, `LegacySSHStore`. (`RemoteStore`, `BinaryCacheStore` are abstract bases.) Many concrete stores are "thin override + delegate" or "diamond merge" classes. There is no shared "DelegatingStore" or "FSAccessorStore" base. See #11, #12, #14, #15.
    - ../verified/05-libstore-core.md, ../verified/07-libstore-local.md, ../verified/08-libstore-remote.md
    - **Validation:** PARTIALLY VALID. The hierarchy is real but several leaves (`LegacySSHStore`, `S3BinaryCacheStore`, `LocalOverlayStore`) have genuinely distinct behaviour that resists collapsing. Best treated as a meta-candidate for the union of #11, #12, #14, #15 rather than a single refactor. Effort: structural.
