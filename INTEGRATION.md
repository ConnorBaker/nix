# The `integration` branch

This branch is upstream `master` with a set of fix and feature branches
merged into it, one `git merge --no-ff` per branch, in the order below. It
is what we build and run; it is not meant to go upstream as a whole. Each
merged branch is its own unit and goes upstream (or not) on its own.

Until 2026-11-21, when a source branch or `master` moved, this branch was
rebuilt from scratch (a fresh `master`, then the same merges with the new
heads) and force-pushed. Since 2026-11-23 the existing merges are kept,
because deployed and pinned revisions contain them: a newer `master` comes
in as a "Merge upstream master <sha>" merge, and a new branch, or a newer
head of a merged branch, as one more `--no-ff` merge after it, so the
branch only moves forward. Every conflict a merge resolved is described in
that merge commit's message.

Base `master` commit: `6965392507e19998de706aa15e514f81e9b7ce11`

Merged `master` commit: `6fa1defb4d5869a3e8c8a1ad8e25b0cab59f596d`

## Merged branches

| Merge order | Branch | Head sha merged | Description | Upstream PR/issue | Stacked on |
| --- | --- | --- | --- | --- | --- |
| 1 | `nix-fix/ldso-self-exe` | `7aac5e3f3c2ec55b8e7145b4ec7898d1c1c9a34e` | Find and re-run the program, not the dynamic loader, when Nix is started through `ld.so` |  |  |
| 2 | `nix-fix/fork-safety` | `ca173f4d611e98f3053a55289d69da0f4ba88082` | Start build children, the external builder, the pager and the recursive-Nix bind helper without allocating after `fork` (`spawnProgram`, fresh exec of Nix); Linux build children are forked by one fresh exec of Nix (the former `nix-fix/fork-safety-server`, folded in). Also: `spawnProgram` sets supplementary groups before the gid, sets `PR_SET_PDEATHSIG` after the uid/gid change and exits if the parent already died; `killUser`'s child reports failure with `_exit(errno)`; killing a build child also kills its pid; the build spec reaches the child over a socket only once Nix holds its pid, and a child the server started but did not report sends its pid back and is reaped; only EPIPE/ECONNRESET count as the server going away (once retried); the server's stdout is `/dev/null`; interrupts stop the reply wait and the reap; the external builder gets only stdin, stdout and stderr (other fds are closed); with `$PAGER` set only `sh -c $PAGER` is tried; tests: `build-child-faults.sh` (LD_PRELOAD shim), unit tests, VM test `spawn-program-root`; release note `build-child.md` |  | `nix-fix/ldso-self-exe` |
| 3 | `nix-fix/builddrv-repair` | `8c7aa713d4dcb6c95af9261a5227b57fb4c1bd9e` | Daemon: disallow unprivileged repair in `BuildDerivation` |  |  |
| 4 | `nix-fix/h1-build-traces` | `4b4aa8bcc25ed147ba43e7bf7ebc657dda27bae5` | Register the build traces of a whole build together, a rebuild's replacing the recorded ones; build-remote copies back the wanted outputs and registers all of them. Also: a remote result not known to be one whole build is registered as `TracesOf::MaybeMixed` (refused when it would mix builds); a consumer realises each input derivation's wanted outputs with one goal; `--check` of builder-rpc-v0 submitted outputs compares with the recorded trace instead of registering; the local-overlay store copies a lower trace up only when the upper store has none; the delete hint names a user that may write the store | Fixes NixOS/nix#15649; an alternative to NixOS/nix#16476 and NixOS/nix#16477 (same issue; those replace stale traces one at a time) | `nix-fix/builddrv-repair` |
| 5 | `nix-fix/replace-valid-path` | `2cae7e43048e3e87d9191c61181fc43fd375e1c4` | Put `replaceValidPath()`'s temporary path next to the path (also in h1-build-traces; adds `tests/functional/ca/repair.sh`) |  |  |
| 6 | `nix-fix/overlay-store-reference` | `4b81373c8eb490ecb49ccfdf78370f3bf7d939cd` | Keep the settings in a local-overlay store's reference (also in h1-build-traces) |  |  |
| 7 | `nix-fix/bt-star` | `f7346c2ce116396881e1e4a8ca8005957f7976ae` | `nix store build-trace delete` accepts `<drv>^<output>,<output>` |  |  |
| 8 | `nix-fix/extbuilder` | `e16c42b44c2dd43548a69818741115ace889dbfc` | Reject invalid submitted output paths; check a fixed output's content-addressing method |  |  |
| 9 | `nix-fix/u98-storepath` | `20102598ebe4eba95c93ec2c8beb51a92e21074b` | Copy store paths the eval store lacks from the build store; tests that a substituter is tried first, an unsigned copy is refused, and an input-addressed path's closure is copied |  |  |
| 10 | `nix-fix/append-plugin-files` | `cd3b47cac839676321f3fede51377cbb61e85317` | Make path list and set settings appendable; since upstream a098d2f678 (in the merged `master` a347df065a) upstream declares these specialisations itself, so the branch's declarations of them are superseded (see the merge of that `master` below) |  |  |
| 11 | `nix-fix/kv-newline` | `002134995e1eb0b98f8b64ca2d7f986860d02ee4` | Write the `builders` setting on one line; report a setting value with a newline instead of asserting | Fixes a regression from NixOS/nix#16659 |  |
| 12 | `nix-fix/findfile-restricted` | `2e5224e855da3a223299ace73a1fba8aa7143171` | Check `findFile` URLs before downloading; restrict `flake:` lookup path entries from Nix code |  |  |
| 13 | `integ/eval-store` | `6b7a52a4d932507c64002568114eb138cbbd59ce` | Separate evaluation store: `eval-store` setting; read derivations from it; build from it (repl, bundle, flake check, build hook) | Build hook part fixes NixOS/nix#13179 | `nix-fix/u98-storepath` |
| 14 | `integ/build-remote-closure` | `b14e11c99a7a4d9c8afe16e65e09108f9395f6a4` | build-remote copies output closures back, for recursive-Nix builds |  |  |
| 15 | `integ/external-builders` | `a1567615fff688c2ac43bf7006b83bfd260fb58b` | External builders (for Tribuchet): decline fallback and `canBuildLocally`, uid-range, recursive Nix, timeouts; a `build-client` setting, written into an external build's `build.json` as `client` (set by a trusted daemon client; an untrusted client's override is refused) |  |  |
| 16 | `nix-fix/build-remote-upload-lock` | `326f70eaa9448df61704ad1dccd5afd62c133dba` | build-remote locks uploads per (machine, path) instead of per machine; the validity query (and substitution on the remote) runs unlocked, so hooks uploading different paths, or nothing, do not wait for each other |  |  |
| 17 | `nix-fix/query-missing-eval-store` | `d448fa946657980ca8207ee53fa300df6042a383` | queryMissing asks about validity in batches, one request per level against a daemon without CA derivations; printMissing sorts in the evaluation store when that has all the derivations |  | `integ/eval-store` |
| 18 | `nix-fix/dyn-drv-ia-nested` | `c5d2375a8c9051630e6ab118df8c7bb49283b8ea` | resolveDerivedPath reports a derivation that is itself an output and not valid as missing before reading it (an input-addressed producer not built yet, or a .drv collected while its realisation was kept); with the unit test of the upstream fix (df5461aa17) and functional tests for the input-addressed case and for collected .drv files (two and three layers, a CA consumer, getContext) | Same bug as NixOS/nix#16459 (open), whose check on the resolved output breaks getContext after collection |  |
| 19 | `derived-path-legacy-separators` | `4aa6ddd2defe315f92329f1cbb1cb6bebf653d40` | Print every layer of a derived path's legacy (worker protocol) form with `!`, and also parse the older form with `^` after the innermost derivation, so paths with two or more dynamic layers reach the daemon and come back, also with older daemons; separately, `build-built-drv.sh` compares real output paths | Draft NixOS/nix#16856; re-enables the property test disabled in NixOS/nix#12619 |  |
| 20 | `nix-fix/output-name-check` | `ec7add763abf7c1a4e1cb0690383670103af2437` | Check output names as store path names in derivations, the inner layers of deriving paths, `derivationStrict`, `builtins.outputOf` and `builtins.appendContext` (on bada5b254b) |  |  |
| 21 | `nix-fix/extbuilder-drvpath` | `ff7f82428a81bf606feb60126be24438a1e64642` | External builders: `drvPath` in `build.json`, naming the derivation being built (restored; see below) |  |  |
| 22 | `nix-fix/ca-drv-foreign-path` | `78e934a36b2f5c43c0070af5852438f7f905967a` | Build a derivation read from a foreign path under its own path, after obtaining its inputs (on bada5b254b) |  |  |
| 23 | `inputs-from-after-all-flags` | `6710deaf2651b9e351bd875af4d0424782d3270f` | `--inputs-from` takes effect after all flags are parsed and the plugins are loaded, so flags after it (such as `--eval-store`) apply and a plugin's store types can be used | Draft NixOS/nix#16857 |  |
| 24 | `nix-fix/release-evaluator` | `636063591c3a24cc127f47aa3b74b7d4ec25d0df` | `nix build` lets the evaluator go before it waits on builds: it drops its installables and `EvalState`, zeroes 64 KiB of dead stack below the caller, then collects and unmaps free GC pages until two collections in a row change neither count; tests `build-release-evaluator.sh` and unit tests (`collectUntilStable`, stale stack pointers) |  |  |
| 25 | `nix-fix/build-remote-substitutes` | `de424e280b47900ae2458d9cb7682cfa89acb61c` | Test only: with `builders-use-substitutes` a machine substitutes a build's inputs from its own substituters, over ssh:// and ssh-ng://, whether or not its daemon trusts the machine's user; an upload of the input is made to fail, so substitution is what passes |  |  |
| 26 | `nix-fix/client-store-setting` | `4cf9353caeedce82d6fa666c6583951da786f611` | A client does not send its `store` setting to the daemon, whose build hook otherwise opened it and, with `--store daemon`, waited forever for the output lock its own build held; test `build-remote-store-setting.sh` |  |  |
| 27 | `nix-fix/client-store-setting` | `121bf01f5c54b4f2ff678b6a34c74af10e433eb2` | The branch's next commit (its parent is 4cf9353cae, merged as row 26; in this branch's history it is merged after the merge of upstream `master` 96084b82ab): the daemon ignores a `store` setting sent by any client (older clients still send it), as it ignores a client's experimental features; the test speaks the protocol as such a client |  |  |
| 28 | `nix-fix/release-evaluator` | `77253d340c5cc13927962fd932fa67e224ed4f8e` | The branch's next commit (its parent is 636063591c, merged as row 24): `build-release-evaluator.sh` is skipped on NixOS (`TODO_NixOS`), where its `clearStore` aborts and the test daemon's sandbox hides the `$TEST_ROOT` files its builder signals through |  |  |
| 29 | `nix-fix/builddrv-repair` | `82ce79f89f3fd780a4ee5342d986083fd56986fd` | The branch's next commit (its parent is 8c7aa713d4, merged as row 3): its `daemon.cc` test names gtest's matchers `::testing::…`, since a test including `nix/util/tests/gmock-matchers.hh` (which declares `nix::testing`) ahead of it in libstore-tests' unity translation unit, as upstream's `buildenv.cc` does from a347df065a, makes `testing::` name that namespace |  |  |
| 30 | `builder-exit-ends-build` | `f840f97c988c079e307afee8190d1f7f052fa2ed` | Wait for the builder to exit instead of killing it when its log reaches EOF (a builder that closes stdout/stderr and then exits, e.g. a script ending in a coreutils program, no longer fails with signal 9); Linux only, through a pidfd the worker polls with the log (`ChildExited`); without `pidfd_open()` the build still ends at log EOF; `nix -v --version` lists `wait-for-builder-exit`; tests `builder-exit.sh`, `external-builders.sh` (an external builder that closes its output and keeps working), `timeout.sh` (`closeLog` now times out under `--max-silent-time`), `nix-functional-tests-run-without-pidfd` (`builder-exit.sh`, `external-builders.sh` and `timeout.sh` without `pidfd_open()`), `openExitDescriptor` unit tests; release note `builder-exit.md`. Based on upstream `master` a347df065a, hence the merge of that `master` before it | Upstream issue 2176 of NixOS/nix (open); PR not yet opened |  |
| 31 | `nix-fix/mimalloc-thp-advice` | `36b7c88f026378ac0f0dfdf494b218be8c5cfbbe` | Build mimalloc with the C define `MI_NO_THP`, so it no longer advises its arenas with `madvise(MADV_HUGEPAGE)`, whose page faults compact and reclaim memory synchronously where `transparent_hugepage/defrag` is `madvise`; `allow_thp` keeps its default, so no `prctl(PR_SET_THP_DISABLE)` reaches the processes Nix starts; NixOS test `mimalloc-thp-advice` (a guest with `transparent_hugepage=madvise`) |  |  |
| 32 | `nix-fix/fod-own-output-lock` | `b331d8f1531f0f74533ef46b8c85ebd28bde7a6d` | A fixed-output derivation whose builder adds its own output path through recursive Nix or builder-rpc-v0 no longer waits forever for the output lock its build goal holds: the builder records the outputs the goal locked, the restricted store passes them to `LocalStore::addToStore`, `addToStoreFromDump` and the scanning add, and for those paths `LocalStore` takes a mutex of that build instead of the file lock (`HeldLocks`; the build hook's `locksHeld` uses the same mechanism). This removes the hang only: under build users such an output is still rejected for its ownership, and in a Linux sandbox without them the build fails with a misleading hash mismatch. Test `fixed-add-own-output.sh` (on 3705f35ddc) |  |  |
| 33 | `eval-store-copy-from-build-store` | `2e385bdf570fe903b6e293334b1c4edd3683e119` | The newer head of the change row 9 merged (`nix-fix/u98-storepath` 20102598eb), rewritten on upstream 4e458b037a as the head of its upstream pull request: `EvalState::ensurePath` registers the temporary root in the build store and checks that the build store has the path whenever substituting into the eval store failed (no early return when the eval store is the build store), and throws a failing copy's error instead of adding it to the substitution error as a trace; `eval-store.sh` also covers `builtins.fetchTarball`, checks that a path neither store has fails without an attempt to copy it, and that the refused unsigned copy's error is the one shown below several stack frames without `--show-trace`; release note `eval-store-copy-from-build-store.md` | Draft pull request 16855 of NixOS/nix |  |

Resolutions, each described in its merge commit's message:
- merge 5 (replace-valid-path): no conflict that git flags, but h1-build-traces
  carries the same `--repair` check in `tests/functional/ca/build-trace-delete.sh`
  ("A repair rebuilds an output whose path is already valid…") that
  replace-valid-path has as `tests/functional/ca/repair.sh`; the merge drops
  h1's copy so the test runs once (as 7669042e91 did);
- merge 7 (bt-star): conflict in `build-trace-delete.sh` with h1-build-traces,
  both blocks kept (as 8bd323ab1e did);
- merge 14 (integ/build-remote-closure): no conflict, but checked together with
  h1-build-traces, since both change what build-remote copies back;
- merge 15 (integ/external-builders): conflict in `derivation-builder.hh` with
  fork-safety, both declarations kept (as 682a2ad75c did);
- merge 18 (dyn-drv-ia-nested): conflict in `tests/functional/dyn-drv/meson.build`
  with h1-build-traces, both kept;
- merge 19 (derived-path-legacy-separators): conflicts with dyn-drv-ia-nested in
  `tests/functional/dyn-drv/meson.build` and `text-hashed-output.nix`, both
  sides kept, merge 18's first;
- merge 20 (output-name-check): conflict in `src/libstore-tests/derived-path.cc`
  with derived-path-legacy, both tests kept, the branch's disabled-test TODO
  dropped;
- merge 21 (extbuilder-drvpath): no conflict;
- merge 22 (ca-drv-foreign-path): conflicts in `tests/functional/dyn-drv/meson.build`
  and `text-hashed-output.nix` with merges 18 and 19, all sides kept. It is
  merged last so that its reviewed new tip can replace it without redoing
  other merges;
- merge 23 (inputs-from-after-all-flags): conflicts in `tests/functional/eval-store.sh`
  with integ/eval-store, both kept, eval-store's tests first and this branch's
  block after its own store reset; and in `tests/functional/suites/plugins/meson.build`
  and `src/plugintest.cc` with fork-safety's builtin-builder test, both kept.
- merge 24 (release-evaluator): conflict in `tests/functional/meson.build`
  with h1-build-traces' `build-remote-ca-rebuild-after-gc.sh`, both kept in
  name order (the file as reviewed on the branch's reviewed base);
- merge 25 (build-remote-substitutes): no conflict;
- merge 26 (client-store-setting): conflict in `tests/functional/meson.build`
  with merge 25's test, both kept in name order;
- "Merge upstream master 96084b82ab" (after merge 26, the integration-only
  commit and the 2026-11-21 describe commit): no conflict. Upstream and the
  merged branches both changed `src/libexpr/eval.cc`,
  `src/libexpr-tests/eval.cc`, `src/libstore/build/derivation-builder-impl.cc`,
  `src/libstore/local-store.cc` and `tests/functional/flakes/flakes.sh`, in
  hunks git merged by itself, none within 40 lines of the other side's
  except in `src/libexpr-tests/eval.cc`, where the branches add tests after
  the `resetFileCacheReleasesValues` test that upstream changed. The functions
  and types whose declarations upstream changed (`restorePath`,
  `copyRecursive`, the `FileSystemObjectSink`, `MemorySourceAccessor` and
  `MemorySink` interfaces, `OsFilename`) occur in no line the merged branches
  add;
- merge 27 (client-store-setting at 121bf01f5c): no conflict; its change is
  exactly the branch's commit since merge 26.
- merge 28 (release-evaluator at 77253d340c): no conflict; its change is
  exactly the branch's commit since merge 24.
- merge 29 (builddrv-repair at 82ce79f89f): no conflict; its change is
  exactly the branch's commit since merge 3.
- "Merge upstream master a347df065a" (after merge 29): one conflict, in `src/libutil/include/nix/util/config-impl.hh`,
  with merge 10 (append-plugin-files): upstream now declares the appendable
  settings, the list and set of filesystem paths among them, with
  `NIX_DECLARE_APPENDABLE_SETTING`, covering what the branch declared by
  hand; both hunks take upstream's side, and the branch's
  `NIX_DECLARE_CONFIG_SERIALISER(std::list<std::filesystem::path>)` stays.
  Upstream renamed `isDerivation` (file names) to `isDerivationFileName`,
  `derivationOptionsFromStructuredAttrs` to
  `derivation::optionsFromStructuredAttrs`, moved `derivation-options.hh`
  and `parsed-derivations.hh` under `derivation/`, and removed
  `getFSSourceAccessor`; no line a merged branch adds uses an old name.
  Of the 16 files both upstream and the merged branches changed, the other
  hunks within 40 lines of each other are in `src/libstore-tests/meson.build`,
  `src/libstore/misc.cc`, `src/libexpr/primops.cc`,
  `src/libexpr/primops/context.cc`, `src/libstore/derivations.cc`,
  `derivation-builder.hh` and `src/libcmd/installables.cc`, all renames or
  include paths on upstream's side, which git merged by itself;
- merge 30 (builder-exit-ends-build): conflicts in
  `src/libstore/build/derivation-building-goal.cc` with merge 15
  (external-builders), whose `childStarted` call passes
  `!localBuildCap.externalBuilder` for `respectTimeouts`: both kept, with
  this branch's `exitChannel`, and this branch's comment that
  `max-silent-time` and `timeout` keep applying while the worker waits
  now says that an external builder applies them itself; and in
  `tests/functional/meson.build` with merge 2's `build-child.sh`: both
  kept, in name order.
- merge 31 (mimalloc-thp-advice): no conflict; nothing merged since
  6bc0ca807c touches `packaging/dependencies.nix` or `tests/nixos`.
- merge 32 (fod-own-output-lock): conflict in
  `src/nix/build-remote/build-remote.cc` with merge 14
  (build-remote-closure), where the hook copies the outputs back: this
  branch's `locksHeld.try_emplace(...)` line and, after it, merge 14's
  comment and `copyClosure(...)`, both kept. Merge 16
  (build-remote-upload-lock)'s per-(machine, path) upload locks are in
  another hunk of that file, which git merged by itself and which is
  unchanged. In the other nine files the change is the branch's commit as
  it is.
- merge 33 (eval-store-copy-from-build-store at 2e385bdf57): conflicts in
  `src/libexpr/primops.cc` (two hunks in `EvalState::ensurePath`) and
  `tests/functional/eval-store.sh` (three hunks in the change's test
  block) with merge 9, the older revision of the same change; every hunk
  takes this branch's side, so the function and the test block are the
  pull request's, line for line. Merge 13 (integ/eval-store)'s tests
  before and after that block and merge 23's block at the end are as they
  were.
- "Merge upstream master 6fa1defb4d" (after merge 33 and the describe
  commit that follows it): one conflict, in
  `src/libstore-c/nix_api_store.cc`, with merge 2 (fork-safety), which
  defines `nix_build_child_main` just before `nix_libstore_init`, whose
  definition upstream now marks `NIXC_NOEXCEPT`: both kept, the branch's
  function unmarked as it was. Of the 16 files both upstream and the
  merged branches changed, the other hunks within 40 lines of each other
  are in `src/libstore-c/nix_api_store.h`, `src/libexpr/primops.cc`,
  `src/libstore/include/nix/store/worker-settings.hh`,
  `src/libutil/include/nix/util/configuration.hh`,
  `src/libutil-tests/package.nix`, `tests/functional/package.nix` and
  `tests/functional/suites/plugins/meson.build`, which git merged by
  itself; in each of the 16 files the merge's change is upstream's own.
  Upstream gave `CanonPath` and `SourcePath` (`operator/`, `push`,
  `baseName`, iteration) the new `FilenameView` in place of a string
  view, keyed directory entries by `Filename`, changed the signatures of
  `createChild`, `insertChild` and the virtual `dumpPath`, made
  `StorePath::to_string()` of a temporary return a string, made the C
  API's functions and `nix_get_string_callback` `noexcept`, and renamed
  `plugintest_dlsym.cc`, `canon-path-algebra.cc` and the manual's
  `installing-binary.md`; no line a merged branch adds uses any of these.

Rows marked PENDING REVIEW or PENDING RE-REVIEW are merged for the gate;
their reviews are not yet done, and this candidate is not pushed until
they are.

NixOS/nix#16459 is not merged: its check,
made on the resolved output rather than on the derivation about to be read,
breaks `builtins.getContext` of a `producer^out^out` string once the inner
.drv is collected (`gc-built-drv-eval.sh`); merge 18 keeps its unit test.

Folded: `nix-fix/fork-safety-server` is now part of `nix-fix/fork-safety` (it could not land alone).

Restored: `nix-fix/extbuilder-drvpath` (merge 21) was dropped on 2026-11-18 on the
assumption that nothing reads `drvPath`. It was put back because two recorded
needs exist: a user decision on 2026-11-03 to log the derivation path (built
with the output path instead, a substitution nobody recorded the user
accepting), and a 2026-11-19 request to join a hub id to its `.drv`, which
failed for want of the field. The pushed `integration` already carries it.

Dropped: `nix-fix/legacy-derived-path` (692868cfd4), replaced by
`derived-path-legacy-separators` (merge 19; the same tree as
`nix-fix/derived-path-legacy` 862152a09e), which has the same printer fix plus
the parser change and its tests. `nix-fix/darwin-spawn-gid` and `nix-fix/clang-vtables` (upstream in
bada5b254b: a09b085f67, 95b9270ced), `nix-fix/empty-flat` (product fix
upstream in d21034932d; dropped by decision), `nix-fix/empty-flat-fork`
(local only). `nix-fix/conf-hash` is shelved and not merged.

The `integ/*` branches hold this fork's own commits that are not (yet) a
`nix-fix/*` branch of their own.

Integration-only commits (adaptations that belong to no source branch): one,
between merge 26 and the merge of `master` 96084b82ab, "tests/functional/dyn-drv: build three-layer paths over the
daemon too": it replaces the local-store-only guard of two of merge 18's tests,
which need merge 19 to run over the daemon. The daemon must also have merge
18's fix (2.35.6 fails them), hence `requireDaemonNewerThan "2.36pre"`. Risk:
merge 18's fix is not upstream (NixOS/nix#16459 is open); once daemonCompat's
daemon is an upstream 2.36 without it, the guard passes and the two tests fail
there, so the guard must then name a version with the fix.
The same risk holds for merge 22's `tests/functional/dyn-drv/build-built-drv-foreign-path.sh`
and `build-built-drv-foreign-path-substitute.sh`, which are guarded by
`requireDaemonNewerThan "2.37pre"`. Their fix (in `DerivationTrampolineGoal`)
exists only in this fork, but an upstream `master` daemon built after "Bump
version" (3705f35ddc, 2.37.0pre) passes that guard without it, so the two tests
would fail against such a daemon. The same bump makes this branch's own daemon
pass the guard: since the merge of 96084b82ab, `daemonCompat`'s run against
this build's daemon runs `build-built-drv-foreign-path.sh`, which it skipped
before (`build-built-drv-foreign-path-substitute.sh` is still skipped there).
The `integration: describe the merged branches` commits only add or update
this file and `maintainers/integration-status.sh`.

## Known state

- Since the merge of upstream `master` 6fa1defb4d: `initPlugins()` throws
  on a second call, where it used to fail an assertion; merge 2 passes it
  to `buildChildMain`, whose build child calls it at most once, when a
  builtin builder is not registered (`builtin-builder.sh`). Upstream's
  clang-tidy configuration rejects `std::filesystem::read_symlink` in
  favour of `nix::readLink`; merge 1 (ldso-self-exe) calls it, with an
  error code, in `src/libutil/current-process.cc`. clang-tidy is not
  among this flake's checks, so nothing here fails on it.
  `nix_build_child_main` (merge 2) is not marked `NIXC_NOEXCEPT` as
  upstream's C API functions now are.

- Merge 30 and external builders: the worker does not apply `timeout` or
  `max-silent-time` to an external builder (merge 15). An external builder
  that closes its output and keeps running is now waited for rather than
  killed, so only its own timeouts bound it (`external-builders.sh`
  tests the wait). That test's `--timeout 120` therefore doesn't bound
  its builder here: a regression that never ends such a build would hang
  the functional tests rather than fail them. Merge 2's build child server
  forks Linux builders with `CLONE_PARENT`, so the builder stays this
  process's unreaped child, which the pidfd requires.

- Rows 24 and 25 are ports onto this base of their reviewed commits, which
  were made on a candidate with a newer `master`: `nix-fix/release-evaluator`
  636063591c of 84018b2acf (the same added and removed lines; only the
  context of its `tests/functional/meson.build` hunk differs, and the result
  of merge 24 is that file as reviewed), and `nix-fix/build-remote-substitutes`
  de424e280b of 658c2974f8 (the same patch-id; its trailer reads
  Assisted-by instead of Co-Authored-By, the rest of its message is the
  reviewed one).
- `nix-fix/client-store-setting` 4cf9353cae has the tree of its reviewed
  commit b5d0df27cd; only its message was reworded.
- The previous integration,
  `ba52427a18d81bae36ce3efc24de3a0fc6c96dff`, merged `nix-fix/build-remote-upload-lock`
  at af1d009c12 while the branch was at 326f70eaa9 (a rewrite, not a
  descendant, differing only in `tests/functional/build-remote-upload-lock.sh`),
  so `integration-status.sh` reported row 16 REWRITTEN for it. Row 16 now
  merges 326f70eaa9.
- Rows 26 and 27 merge the same branch, `nix-fix/client-store-setting`, at
  4cf9353cae and at its next commit 121bf01f5c, the branch's tip;
  `integration-status.sh` compares only the newer merge with the branch's
  tips and reports a tip still at 4cf9353cae as OLDER. Rows 24 and 28
  (`nix-fix/release-evaluator`) and rows 3 and 29
  (`nix-fix/builddrv-repair`) are the same kind of pair.
- Rows 9 and 33 are two revisions of one change under two branch names:
  `nix-fix/u98-storepath` stays at 20102598eb, and
  `eval-store-copy-from-build-store` (2e385bdf57), a rewrite of that
  commit on a newer upstream `master` and not a descendant of it, is the
  head of the change's upstream pull request. After merge 33 the change's
  own lines are that head's. `integration-status.sh` knows a branch by
  its name, so it checks the two rows separately, each against its own
  branch's tips, and does not report row 9 as superseded as it does rows
  3, 24 and 26; it would report `nix-fix/u98-storepath` only if that
  branch moved.
- Every merged branch exists in this fork, and this branch merges its tip:
  the branches are what integration is made of, and each is the source of
  its upstream PR.

## How to check staleness

Run `maintainers/integration-status.sh` (no network; fetch first if you
want the remotes' current state). It reads the merged branches and shas from
the merge commits themselves, cross-checks them against the table above,
and for every branch prints the merged sha, the local and `origin/` tips
that exist, and whether each is UP-TO-DATE, BEHIND (n commits), REWRITTEN
(the tip no longer contains the merged sha) or MISSING. It also says how far
the merged `master` (the newest "Merge upstream master" merge's, else the
base) is behind `master`, `origin/master` and `upstream/master`. It exits
non-zero if anything is not up to date or the table, or the "Merged
`master` commit" line, disagrees with the merges.
