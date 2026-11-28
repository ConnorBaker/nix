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
| 34 | `nix-fix/query-missing-realisations` | `e19cec45d0275ad8ceca1f0e7852b08e0e293799` | `queryMissing` asks the substituters for the realisation of an output whose path is not known in advance (e.g. of a floating content-addressed derivation) with the callback form of `queryRealisation`, awaited, instead of the blocking one, so the lookups of different derivations overlap instead of running one after the other; a derivation wanted with two sets of outputs now sends two requests for the same realisation, and when several lookups fail the error is that of the first to fail. Test `query-missing-realisations.cc` in libstore-tests, a network test: an HTTP binary cache that answers no lookup until four wait at once. On upstream `master` 6fa1defb4d |  |  |
| 35 | `nix-fix/locks-after-build-slot` | `3a048b37c9df19d8098a5ef1ae5b5e59babc8610` | A build goal takes its output locks only once a build slot is free: `tryHookLoop` returns at once when the hook can only decline and a local build is possible, `tryBuildLocally` waits for a slot before it locks unless `max-jobs` is 0, and `buildLocally` no longer yields on entry. Outcomes are unchanged; three derivations built one at a time take their locks 4 times instead of 12. Tests in `build.sh` and `external-builders.sh` count the "trying to build" rounds. On upstream `master` 6fa1defb4d |  |  |
| 36 | `nix-fix/builder-setup-in-worker-loop` | `f015f7b15f4331cd374c15cf1dbf49e12e279995` | The worker no longer blocks while a builder sets up its environment (e.g. the sandbox): `startBuild` returns at once and the build goal passes the builder's output to `processSetupOutput` until the `\2` line, so other goals run meanwhile; the build gets its timeouts and is shown as started only once the setup is done, and a failed sandbox helper's error is read from the builder's output. Tests in `external-builders.sh`: two builders that each finish starting only when the other has started, a builder that cannot be started, a setup error longer than one read, a builder that exits before `\2`. On upstream `master` 6fa1defb4d |  |  |
| 37 | `nix-fix/fork-safety` | `48c26b6fbf2431d40cb9f30673ff3c57938cd818` | The branch's next two commits (their parent is ca173f4d61, merged as row 2): `spawnBuildChild` checks for an interrupt once the callback that ends its wait for the build child server's reply is registered, where an interrupt taken between the request and the registration was lost (a fault in the test's shim puts one there); and `build-child-faults.sh` loses its `timeout`s and poll counts, starts nix-build through `setpriv --pdeathsig KILL`, so that a Nix that hangs dies with the test's shell, and requires an interrupted Nix to exit with status 1 |  |  |
| 38 | `nix-fix/build-hook-handshake` | `70390d8cbe857f8c76e00ca047d72fcf2c95239f` | Nix no longer waits, on the one thread that runs every goal, while a build hook connects to its machine: `nix __build-remote` writes the log message `@nix {"action":"connecting"}` once it holds a build slot and before it connects; on that message the goal takes the hook from the worker, which starts the next goal's hook, and waits for the rest of the answer as child events (`tryBuildHook` is a coroutine and hands the hook, the machine's name and what it read past the name to `buildWithHook` in an `AcceptedHook`). A hook that then cannot connect goes back to the worker, so the next goal asks a hook that knows the machine. The hook does not say it when a machine it may connect to for the derivation is an ssh one (`ssh://`, `ssh-ng://`, `mounted-ssh-ng://`, a bare host name), because sshd refuses connections while too many are unauthenticated (`MaxStartups`): for such machines, and for a hook that never says it (an older or a custom one), the goal blocks up to the answer and the machine's name as before. A goal of an older Nix ignores the message. And: a goal whose hook has finished a build on another machine, or whose accepted hook is destroyed, wakes one of the goals that wait for a while (the first in the set's order), so a goal the hook postponed asks again when a slot frees up and not only after `build-poll-interval`. Test `build-hook-waiting.sh`. On upstream `master` 6fa1defb4d | Upstream issue 3651 of NixOS/nix (closed as stale; its reporter's machine is an ssh one, a kind this change leaves as it was); no pull request |  |

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
- merge 34 (query-missing-realisations): conflict in
  `src/libstore-tests/meson.build` with merge 17
  (query-missing-eval-store): both add a source after `pathlocks.cc`;
  both kept, in name order. In `src/libstore/misc.cc` merges 13, 17 and
  18 changed `queryMissing` above this branch's hunk, the nearest change
  23 lines before it; git merged the file by itself, and the branch's
  lines replace the same lines as upstream.
- merge 35 (locks-after-build-slot): conflicts in
  `src/libstore/build/derivation-building-goal.cc`, at the start of
  `buildLocally`, with merge 15 (external-builders): the branch's removal
  of the yield and merge 15's `canBuildHere`, which is now the function's
  first statement, both kept; and in
  `tests/functional/external-builders.sh`, where merge 30's block and
  this branch's lock rounds both follow upstream's last line: both kept,
  merge 30's first. When an external builder declines, the stock
  builder's slot check now runs without a yield before it, so no other
  goal can take the slot the external builder gave up first.
  If the stock builder then has to wait for a build user, another goal
  can still take the slot, and the derivation goes back through
  `tryToBuild` and so through the external builder again, as before
  this merge.
- merge 36 (builder-setup-in-worker-loop): conflicts in
  `src/libstore/unix/build/unix-derivation-builder.cc` with merge 30
  (`killChild`: merge 30's `builderExit.close()` stays in the block on
  the pid, the branch's unregistration follows the block; end of
  `startBuild`: merge 30's `openExitDescriptor` stays, the blocking wait
  for the setup goes); in `derivation-building-goal.cc` with merges 15
  and 30, where `buildLocally` registers the builder: one lambda,
  `registerChild`, makes the registration during the setup (no timeouts,
  no exit channel) and the one after it (timeouts unless the builder is
  external, and the exit channel), also in place of the
  `childStarted(..., true, true)` that git merges into the branch's
  `processSetupOutput` lambda without a conflict; and in
  `tests/functional/external-builders.sh`, where this branch's block
  goes before merge 30's. The two source files are resolved as in the
  branch's own trial merge (the same patch-id in each). The test's block
  is the trial merge's with one line more, and differs from the branch's
  in two places.
  The branch has two checks of a builder that cannot be executed, a
  missing program and a name too long. Both expect the failed exec to
  arrive through the builder's output. Merge 2 starts an external
  builder with `spawnProgram`, which reports the failure to Nix itself
  (`could not start program`), so here the test fails at the first of
  them. What they pinned that still holds here stays pinned, in two
  parts. That such a build fails with an error that names the program:
  the first check is kept, with `spawnProgram`'s message (this is the
  line the trial merge does not have); a name too long fails the same
  way here, so the second check would add nothing to it. That an error
  which arrives through the builder's output in more than one read is
  passed on whole, under "while setting up the build environment": a
  builder that sends such an error in the build child's wire format
  takes the second check's place (the trial merge's replacement).
  The branch's last check, that a cancelled build's child is killed when
  it has no process group of its own yet, is left out: here it fails
  although the kill works. Its builder waits until Nix is asleep or
  gone, on the premise that Nix does not sleep before the kill. But
  `Worker::run` (merge 2) stops the build child server when it returns,
  before the cancelled goal is destroyed and its child killed, and Nix
  sleeps while it waits for the server to exit. Measured on this tree
  with the check put back, on a 20-CPU machine busy with other work: a
  builder survived in 3 of 200 tries with nothing added beside the
  test, and in 51 of 200 with the test and four busy loops held to one
  CPU; where the builder recorded Nix's children, the server was still
  one of them in 18 of 18 records. A form of the check that waits for
  that as well, until the builder is Nix's only child, left no survivor
  in 1000 tries. But with `kill(pid)` removed from `killChild`, the
  line the check is for, it left one in only 34 of 1000: here the child
  has usually called `setsid()` by the time the server has exited, so
  the kill of its process group reaches it. A check that the broken
  kill fails so seldom is not added. With that line removed,
  `build-child-faults.sh` (merge 2) was ended by its runner's time
  limit in 3 of 3 runs, at its build child that a fault stops before
  `setsid()`: Nix waits for a child it did not kill.
- merge 37 (fork-safety at 48c26b6fbf): no conflict. The merge base is
  ca173f4d61, merge 2's head, so the merge brings the branch's two newer
  commits only; the line they add to
  `src/libstore/linux/build/linux-derivation-builder.cc` is in
  `spawnBuildChild`, far from merge 36's change to `startChild`.
- merge 38 (build-hook-handshake): the merge commit is the branch's own
  trial merge on ea80f0ec99, taken after the same merge was made a
  second time without looking at it: the source files and the test list
  came out the same. Conflicts in
  `src/libstore/build/derivation-building-goal.cc`, at the start of
  `tryHookLoop`, with merge 35 (locks-after-build-slot): its early return
  when the hook can only decline and the branch's `AcceptedHook
  accepted;`, both kept, the early return first; at the start and at the
  end of `tryBuildHook` with merge 13 (integ/eval-store): its
  `drvInBuildStore` test and its copy of the derivation from the
  evaluation store before the accept stay, their `return`s being
  `co_return`s in what is now a coroutine; in
  `derivation-building-goal.hh` with merge 15 (external-builders):
  `canBuildLocally`'s declaration stays, the branch's declaration of
  `tryBuildHook` follows it; and in `tests/functional/meson.build`,
  adjacent entries of the list, all kept. Outside the test's two files
  the merge adds and removes the 169 lines of the branch's earlier trial
  merge on integration 78e3105524; the conflict with merge 35 is new
  since then. The merge's message lists what git merged by itself next
  to other merges' changes; `buildWithHook` is changed by this branch
  only, `buildLocally` by merges 15, 30, 35 and 36 only.
  What the merges mean for each other, by the branch's notes. Merge 35:
  a goal that waits for a connecting hook holds the output locks it took
  at the start of that round of `tryHookLoop`, as it did through the
  blocking read before, and takes no other; a goal the hook postpones
  lets them go; merge 35's slot wait is on the local path, where a goal
  holds no locks of a hook round. Merge 36: there are now two waits for
  a child that is not yet a build, the builder's setup (in a build slot)
  and the connecting hook (in none). A goal has one of them at a time:
  `Worker::childTerminated` finds a goal's child by the goal alone, and
  each wait is ended before the goal can start the other. Neither end
  wakes a goal that waits for a while; only the end of a remote build
  does. Merge 2: the hook is not a build child and is not started
  through the build child server; a goal destroyed after `Worker::run`
  has returned kills its connecting hook as it kills an accepted one
  (SIGTERM, then SIGKILL after `build-hook-kill-timeout`), after the
  server was stopped; new is only that several hooks can be connecting
  then. Merge 15: an external builder that declines is followed by the
  stock builder in the same step, under the slot count that connecting
  hooks do not add to; what Nix tells a hook about its own willingness
  to build counts starting builders (merge 36) and not connecting hooks.
  Merge 30: the wait for a hook has no exit channel, so no `ChildExited`
  reaches the reader of the hook's lines.
  One change is the merge's own, in `build-hook-waiting.sh` and its
  `.nix`. The branch's check that a goal waiting for its hook holds no
  build slot expects the `building '` line of a derivation built here
  before that of the one the hook accepts. With merge 36 a local build
  is shown as started only once its builder is set up, a child event
  like the hook's answer, which is already there: with the branch's
  files the test fails here at that line, the hook's build first
  (measured). The merge's test has a second derivation that prefers a
  local build, runs Nix with `-vvv` and `--max-jobs 1`, and wants
  exactly one goal to say "wait for build slot" (the trace line merge
  36's test in `external-builders.sh` matches): one of the two local
  builds waits for the other, and both would if the goal that waits for
  its hook held the slot. It pins what the branch's check pinned, the
  slot: with the waiting goal put into a build slot it finds 2 and fails
  (measured), where the branch's check fails on the branch. It does not
  show by itself that the local build starts before the hook has
  accepted; by the branch's mutants a Nix that ignores the hook's
  message fails an earlier case of the test, on this tree too. The
  test's other lines are the branch's.

Rows marked PENDING REVIEW or PENDING RE-REVIEW are merged for the gate;
their reviews are not yet done, and this candidate is not pushed until
they are.
Row 33's head 2e385bdf57 was reviewed with the candidate that merged it
(78e3105524): those reviews covered its difference from row 9's revision.
The trial merges that the messages of merges 35, 36 and 38 name by
their commit ids are ancestors of no branch. In the repository where
these merges were made they are kept under `refs/archive/2026-11-28/`;
they are not in this fork.

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
The same risk holds for merge 32's `tests/functional/fixed-add-own-output.sh`,
guarded by `requireDaemonNewerThan "2.37.0pre20261120"`, and for merge 35's
count of lock rounds in `tests/functional/build.sh`, guarded by
`isDaemonNewer "2.37pre"`: both fixes exist only in this fork, and
`daemonCompat` also runs against Nixpkgs' `nixVersions.latest` and `stable`
(`packaging/hydra.nix`). Both are older than 2.37 today, so the two checks are
skipped there; once one of them is a 2.37 release, it passes both guards
without the fixes, and the checks fail against it.
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

- Tests without a time limit of their own. Since merge 37
  `build-child-faults.sh` has none: a Nix that hangs in it keeps the test
  waiting until the test's runner ends it. `meson test` does, after 300 s
  per functional test. A package build does not: Nixpkgs' meson hook runs
  the check phase with `--timeout-multiplier=0`, so there the hang lasts
  until the `timeout` or `max-silent-time` of the Nix that runs the build,
  if it sets one, and under `checkPhase` in the development shell until
  the developer ends it. The same holds for merge 36's two builders in
  `external-builders.sh` that wait for each other. Merge 34's unit test
  sets no limit either: a regression leaves it waiting for a lookup that
  the test's cache holds, which `meson test` ends after 30 s and, in a
  package build, Nix's `stalled-download-timeout` after 300 s.
  The first case of merge 38's `build-hook-waiting.sh` has no limit
  either: without the wake-up when a remote build ends, or if a remote
  build's output were no longer relayed, it waits for a line that never
  comes. A tree that lacks that wake-up must not be given to a package
  gate.
- Rows 2 and 37 merge `nix-fix/fork-safety` at ca173f4d61 and at
  48c26b6fbf, two commits later; `integration-status.sh` compares only
  row 37 with the branch's tips, as for the pairs below. When this was
  written both tips, the local one and this fork's, were still at
  ca173f4d61, which the script reports as OLDER; 48c26b6fbf becomes the
  fork's tip in the push that carries this merge.
- Merge 37 (from the pointed review of ea80f0ec99; read in the code,
  not reproduced): the interrupt check it adds to `spawnBuildChild`
  throws after the request for a build child was sent. The build child
  server still starts that child, and nothing reaps it, because the
  exception passes the code that waits for a child the server did not
  report. It stays a zombie until Nix exits, which matters only for a
  process that goes on after an interrupt.
- Merge 38, before it is run for real: nobody has measured many hooks
  connecting at the same time on the gRPC path. A hook whose connection
  fails does not try again: it prints `cannot build on`, gives the
  machine up and, with no other machine, declines, and with
  `--max-jobs 0` the build then fails. Upstream, hooks connected one at
  a time; with merge 38 as many as the machine has free slots connect at
  once (64 to the farm's hub). Before an integration build with this
  merge becomes the farm's daemon or a client's Nix, 64 or more new
  derivations go through the hub with a check that no `cannot build on`
  line appears. The rig runs behind the branch's figures used a
  `unix://` daemon and an earlier form of the change; nothing has been
  timed with 70390d8cbe.
- Merge 38, behaviour that no test pins (by the branch's notes). The
  `!hook &&` in `tryBuildHook`, which makes a goal ignore a second
  "connecting" from the hook it already holds: `nix __build-remote` says
  it once per machine it tries, so with two machines that are not ssh
  ones and the first not reachable, one hook says it twice for one
  request. With the test's unreachable machine doubled the builds are
  done here after four `cannot build on` lines; without the guard Nix's
  output ends after the first, with no error message, and the command
  fails. A second such machine in the test's `--builders` and 4 expected
  lines would pin it. And the wake-up when a hook that had accepted is
  destroyed (a log limit, a timeout, a cancelled goal): every remote
  build in the test ends the ordinary way. Also not pinned: that a goal
  destroyed while its hook connects is unregistered.
- Merge 38, slots that become free without a wake-up, each bounded by
  `build-poll-interval` and none worse than upstream: when the first
  goal of the waiting set cannot use the slot, it takes every wake-up
  while it is first; when a hook that said "connecting" gives up, its
  slot wakes nobody; when a build of another Nix process ends.
- Merge 38's test skips itself on NixOS and behind a daemon, so
  `daemonCompat` and the VM functional tests do not run it, and the VM
  remote-build tests use ssh machines, for which the hook stays silent:
  only the plain functional suite makes a hook say "connecting".

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
