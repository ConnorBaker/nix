# Dead or stale code

Candidates 48-60. All thirteen VALID. The validation pass surfaced four
**latent bugs** in this group (#53, #54, #58, #59 — see notes). High
concentration of trivial-effort wins.

| # | Verdict | Effort |
| - | ------- | ------ |
| 48 | VALID | trivial |
| 49 | VALID | trivial |
| 50 | VALID (resolved-by-design) | none |
| 51 | VALID | small |
| 52 | VALID | trivial |
| 53 | VALID | small (latent bug) |
| 54 | VALID | trivial (latent bug) |
| 55 | VALID | trivial |
| 56 | VALID | trivial |
| 57 | VALID | trivial |
| 58 | VALID | trivial |
| 59 | VALID | small (latent bug) |
| 60 | VALID | trivial |

---

48. **`void check();` in `lockfile.cc` is an unused forward declaration.** Inside `nix::flake` after `LockFile::check`'s definition; appears to be dead code.
    - ../verified/15-libflake-libmain.md
    - **Validation:** VALID. Whole-tree grep confirms zero references. Pure deletion.
    - **Branch:** `vibe-coding/cleanup/libflake`

49. **`blockInt` and similar dead identifiers.** Worth a sweep through the verified shards for any declaration without matching uses.
    - **Validation:** VALID. `blockInt` in `shared.hh` has zero references in `src/`. Pure deletion.
    - **Branch:** `vibe-coding/cleanup/libmain`

50. **`nix_value_incref`/`_decref` are pure forwarders to the generic `nix_gc_*` helpers.** Documented as the preferred typed API, but currently identical to the generic ones. The header comment notes a migration intent.
    - ../verified/19-c-bindings-misc.md
    - **Validation:** VALID — but **the original framing has the migration direction backwards.** The header in `src/libexpr-c/nix_api_expr.h` already marks `nix_gc_decref` itself `@deprecated` and explicitly names `nix_value_decref` as the preferred replacement (with a TODO above `nix_gc_incref` proposing the same). The typed forwarders are the migration *target*, not the debt; deprecating or deleting them would be a regression in API direction, and removing them would break the C ABI. **Recommendation:** no code change. Resolved-by-design; close the inventory entry. Effort: none.
    - **Branch:** none — resolved-by-design.

51. **`#if 0` blocks in `github.cc`.** The treeHash-mismatch warning inside `downloadArchive` and the treeHash output attribute inside `getAccessor` are commented out, hinting at unfinished tree-hash propagation.
    - ../verified/14-libfetchers.md
    - **Validation:** VALID. The `#if 0` blocks suggest an abandoned tree-hash propagation effort. The matching `treeHash` slot in `GitArchiveInputScheme::allowedAttrs` is wired up but unreferenced elsewhere — a delete-and-trim cleanup should also drop that slot (recheck). Either commit to the feature or delete the code. Effort: small (delete-and-trim) or medium (resurrect with the missing `upstreamTreeHash` lookup).
    - **Branch:** `vibe-coding/cleanup/libfetchers` (delete-and-trim; the trim went past the two `#if 0` blocks to also drop `RefInfo::treeHash`, the `upstreamTreeHash` helper, and the `treeHash` slot in `GitArchiveInputScheme::allowedAttrs`, none of which had any live readers after the `#if 0` blocks were removed; the now-single-field `RefInfo` struct itself is also unwrapped — the abstract `getRevFromRef` and the three concrete overrides (GitHub/GitLab/SourceHut) return `Hash` directly, since the type was a private nested struct of a private base class with no external consumers).

52. **`CurlInputScheme::specialParams` is declared but never defined or referenced.** Compiles only because nothing odr-uses it.
    - ../verified/14-libfetchers.md
    - **Validation:** VALID. Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libfetchers`

53. **`getCustomRegistry` only honours the first call's path.** Memoises in a function-local static; later changes to the registry path are silently ignored.
    - ../verified/14-libfetchers.md
    - **Validation:** VALID — and a **latent bug**. Currently masked because the sole caller invokes it once. Embedders calling with different paths would silently get the first path's result. Effort: small.
    - **Branch:** `vibe-coding/cleanup/libfetchers` (one-line removal of the `static` keyword on the function-local `customRegistry`. The body now re-runs `Registry::read(SourcePath{getFSSourceAccessor(), CanonPath{p.string()}}.resolveSymlinks())` on every call. The sole in-tree caller `RegistryCommand::getRegistry` already memoises per-instance, so per-call cost is paid at most once per `nix registry <subcmd>` process invocation. **Adjacent latent-bug shape (out of scope here, file as new candidate):** `getUserRegistry`, `getSystemRegistry`, `getGlobalRegistry` in the same file have the same `static auto x = Registry::read(settings, ...)` pattern and *are* on the hot path (called per `lookupInRegistries`). They silently capture the first-call `Settings &`/`Store &` and ignore subsequent ones.)

54. **`SQLiteSettings::useWAL` is declared without a default initialiser.** Each constructor of `SQLite` reads it; a constructor that doesn't set it is undefined behaviour.
    - ../verified/07-libstore-local.md
    - **Validation:** VALID — and a **latent bug**. Currently masked because every caller uses designated initialisers. A positional construction is UB. **Default to `true`** to match the runtime default of `Setting<bool> useSQLiteWAL{this, !isWSL1(), ...}` in `globals.hh`; `false` would silently change behaviour on every non-WSL1 platform if a future caller ever omits the field. Every existing caller (`local-store.cc`, `nar-info-disk-cache.cc`, `eval-cache.cc`, `libfetchers/cache.cc`, `http-binary-cache-store.cc`, `libstore-tests/nar-info-disk-cache.cc`) threads `settings.useSQLiteWAL` explicitly, so the chosen default only matters for hypothetical positional/zero-init constructions. Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libstore`

55. **Stale `IndexReferrer` index dropped at runtime.** The `20260309-drop-redundant-indexreferrer` migration in `LocalStore::upgradeDBSchema` cleans up a previous-version index. The matching `create index` is no longer in `schema.sql`. The migration drop is harmless but stale once all stores have run it.
    - ../verified/07-libstore-local.md
    - **Validation:** VALID. Drop the migration once enough time has passed for in-the-wild stores to have run it. Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libstore` (one-line deletion in `LocalStore::upgradeDBSchema`. The migration was a no-op on already-upgraded stores; older stores that never ran it still hold the obsolete `IndexReferrer` index but the schema is otherwise valid. The migration shipped 2026-03-09; ~2.5 months in-the-wild is structural-reasoning, not measurement, but the drop is harmless either way.)

56. **`BaseSetting<PathsInChroot>::trait` lived in a different file from `BaseSetting<SandboxMode>::trait` — a real ODR violation.** The `PathsInChroot` trait was in `local-settings.hh`; `SandboxMode` was only in `globals.cc`. Different TUs saw different definitions of the explicit specialisation; the values coincidentally matched `appendable=false` from the primary template, so behaviour was preserved but the program was technically ill-formed. Originally framed as cosmetic-style placement; the deeper observation is that this fits "Latent bugs hiding inside duplication" / "ODR fix" rather than pure dead/stale code. Cross-referenced here because the branch landed in this category's cleanup branch.
    - ../verified/07-libstore-local.md
    - **Validation:** VALID. The adversarial review pass found this is a real ODR fix (different TUs previously saw different definitions of the explicit specialisation; the values coincidentally matched `appendable=false` from the primary template, so behaviour was preserved but the program was technically ill-formed). **See also:** N44 (`BaseSetting<T>::trait` specialisation pattern is one-of and ad-hoc; the layered `parse`/`to_string`/`appendable`/macro split is the underlying debt class). Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libstore`

57. **Macro hygiene caveats.** All three `*_USE_LENGTH_PREFIX_SERIALISER_COMMA` helpers (`WORKER_USE_LENGTH_PREFIX_SERIALISER_COMMA`, `SERVE_USE_LENGTH_PREFIX_SERIALISER_COMMA`) are `#define`d but never `#undef`'d in their respective impl headers, leaking into translation units. There is also a stray bare `#undef COMMA_` at the end of `common-protocol.hh` with no matching `#define` in scope.
    - ../verified/09-libstore-protocol.md
    - **Validation:** VALID. The implementing PR caught a third issue too: `LENGTH_PREFIXED_PROTO_HELPER_X` in `length-prefixed-protocol-helper.hh` was `#define`d but never `#undef`'d; landed PR normalises all three protocol headers onto a single prefixed-macro convention scoped per-`std::set`/`std::map` pair. Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libstore`

58. **`getMaxCPU` catches `Error` and routes through `ignoreExceptionInDestructor`, but it is not actually a destructor.** Should use `ignoreExceptionExceptInterrupt` per the `util.hh` comment.
    - ../verified/03-libutil-runtime.md
    - **Validation:** VALID. **Correction:** the function lives in `current-process.cc`, not `processes.cc`. Effort: trivial.
    - **Branch:** `vibe-coding/cleanup/libutil`

59. **`useBuildUsers` returns a function-local `static bool`.** Computed once and cached for the process lifetime; changes to `localSettings` after first call are not observed.
    - ../verified/07-libstore-local.md
    - **Validation:** VALID — same shape of latent bug as #53. Effort: small.
    - **Branch:** `vibe-coding/cleanup/libstore` (`static` keyword removed in both `#ifdef` branches of `unix/user-lock.cc::useBuildUsers`; the body now re-evaluates `localSettings.buildUsersGroup != ""` and `isRootUser()` on every call. Sole caller is `derivation-builder.cc::prepareUser`, called once per derivation build, so the per-call cost is dwarfed by the surrounding fork/exec/sandbox setup.)

60. **`SQLiteStmt::create` is called on `purgeCache` but the statement is never used.** In `NarInfoDiskCacheImpl::State`; the periodic purge runs ad-hoc inside the constructor against `LastPurge` rather than via the prepared statement.
    - ../verified/08-libstore-remote.md
    - **Validation:** VALID, with a sharper finding: the candidate text overstates use — `purgeCache` is the only `SQLiteStmt` member that the constructor never even `.create()`s. Pure deletion. Stretch: extract the inline purge into a `State::purgeIfNeeded(time_t)` method with a properly cached `SQLiteStmt`. Effort: trivial (delete) to small (extract method).
    - **Branch:** `vibe-coding/cleanup/libstore` (deletion only; the `purgeIfNeeded` extract was deliberately deferred)

225. **`getUserRegistry`/`getSystemRegistry`/`getGlobalRegistry` share #53's first-call-wins memoisation pattern, with worse blast radius.** All three use the same `static auto X = Registry::read(settings, ...)` shape that #53 just removed from `getCustomRegistry`, but unlike `getCustomRegistry` (called once per `nix registry <subcmd>`) these are reached on every `lookupInRegistries()` — i.e. on every flake-resolve, lockfile-load, and `fetchTree` callsite. Worse, they capture `settings` from the **first** caller and ignore subsequent `Settings &` arguments entirely. `getGlobalRegistry` additionally captures `Store & store` from the first call. Long-lived processes (daemons, the repl, test harnesses doing multiple resolves) silently get the first call's view. Pre-existing oddity surfaced by the Pass-B review of #53. Same fix shape: drop the `static` and let each call re-load (since these are called on the lookup hot path, this may need a finer-grained cache — e.g. mtime-keyed — rather than naive removal).
    - ../verified/14-libfetchers.md
    - **Validation:** VALID — and a **latent bug** family. Same shape as #53. **Pitfall:** unlike #53, these three are on the hot path (`lookupInRegistries` is called per `fetchTree`/per-flake-resolve), so naive `static` removal trades correctness for re-parsing the registry JSON on every call. The right fix probably involves an mtime-keyed cache rather than a static-init memo. Effort: small per function; medium across all three because of the cache-invalidation policy decision.
