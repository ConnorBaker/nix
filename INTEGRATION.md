# The `integration` branch

This branch is upstream `master` with a set of fix and feature branches
merged into it, one `git merge --no-ff` per branch, in the order below. It
is what we build and run; it is not meant to go upstream as a whole. Each
merged branch is its own unit and goes upstream (or not) on its own.

When a source branch or `master` moves, this branch is rebuilt from scratch
(a fresh `master`, then the same merges with the new heads), not rebased,
and force-pushed. Every conflict a merge resolved is described in that
merge commit's message.

Base `master` commit: `6965392507e19998de706aa15e514f81e9b7ce11`

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
| 10 | `nix-fix/append-plugin-files` | `cd3b47cac839676321f3fede51377cbb61e85317` | Make path list and set settings appendable |  |  |
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
  with merge 25's test, both kept in name order.

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
after merge 26, "tests/functional/dyn-drv: build three-layer paths over the
daemon too": it replaces the local-store-only guard of two of merge 18's tests,
which need merge 19 to run over the daemon. The daemon must also have merge
18's fix (2.35.6 fails them), hence `requireDaemonNewerThan "2.36pre"`. Risk:
merge 18's fix is not upstream (NixOS/nix#16459 is open); once daemonCompat's
daemon is an upstream 2.36 without it, the guard passes and the two tests fail
there, so the guard must then name a version with the fix.
The `integration: describe the merged branches` commits only add or update
this file and `maintainers/integration-status.sh`.

## Known state

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
- Until this integration is pushed together with its branches (rows 24-26
  new, `integ/external-builders` moved to a1567615ff), `integration-status.sh`
  reports rows 24-26 MISSING and row 15 REWRITTEN against the local and
  `origin/` refs. Both are expected.

## How to check staleness

Run `maintainers/integration-status.sh` (no network; fetch first if you
want the remotes' current state). It reads the merged branches and shas from
the merge commits themselves, cross-checks them against the table above,
and for every branch prints the merged sha, the local and `origin/` tips
that exist, and whether each is UP-TO-DATE, BEHIND (n commits), REWRITTEN
(the tip no longer contains the merged sha) or MISSING. It also says how far
the base is behind `master`, `origin/master` and `upstream/master`. It exits
non-zero if anything is not up to date or the table disagrees with the merges.
