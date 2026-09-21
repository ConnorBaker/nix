# Status: the single entry point

Read this first. It says where the work stands, how to verify that claim, what is decided, what is open and on what evidence, what is in flight and exactly how to resume it, and how to operate the machinery. The other documents hold the meaning, the laws, the implementation and the measurements; this one holds the state. Every number here is read from a log or a document section named beside it; a number computed from those is marked `[derived]`.

## 1. Where the branch stands

The branch is five commits above master, rebuilt from the whole diff by directory (packaging; src; tests; doc/manual; doc/lazy-store), so the history is the logical structure and not the diary. The diary is kept under local refs `refs/archive/lazy-store-rewrite-diary-*` and is not needed to resume anything.

What the tree does, in one paragraph: a source and a store object have one address, the git tree id under SHA-256; the local store keeps an object store under `.objects/{blobs,blobs-x,trees,tmp}` sharded 256 ways, holding the trees and the symlink-target blobs, while a regular file's blob is a store path's own file — a *holder* — which the store indexes in its own database, so two paths holding the same file hold one inode; a source is named by one walk with a per-node memo, and copied from the store's own objects where the store holds them, sibling directories on a bounded pool; a git checkout's clean files are read from the object database and only its modified files from the working directory; the evaluator defers its store writes into a byte-bounded buffer and writes them in one batch at the doors; an ingestion holds its objects until it has registered, and a whole-store collection sweeps the object store and then the index's stale rows, the second half being one no store can override. `01-specification.md` gives the meaning and the decisions, `04-derivation.md` the components, `08-store-model.md` the invariants.

**Verify the claim before trusting it.** Every gate the branch is held to, one suite per command line, is in `05-validation.md` section 2 (the suites and their command lines), section 4 (the mutation table, run by `runs/run-mutation.sh` over `runs/mutations.py`), and section 5 (both platforms, with the counts the binaries printed at this tree and the two gates the sandbox cannot give: a per-file gcc build and ThreadSanitizer). A resumption starts by running section 2's commands and comparing with section 5's counts; a count that moved with no test added is a defect. The counts in section 5 were printed at a tree that differs from this one only under `doc/`; the documentation commit that followed it was not re-gated, since no gate reads `doc/`.

## 2. Decided, with the reason

Every decision is in `01-specification.md` section 10 with its reason and consequence. The ones a resuming agent most needs to know were decisions and not accidents:

- Sources are named under the git method only; sources with references keep the NAR name; the SHA-1 git form is not written (readers tolerate it).
- No settings and one code path: deferral is unconditional and byte-bounded; the copy's pool size is fixed; the export-ignore filter is the identity when no source names the attribute.
- Older peers need `git-hashing` to read git-named sources; accepted as the release's blast radius (mixed fleets set the feature on the older side).
- The object store's layout is 256-way sharded; a store an earlier build of this branch wrote flat is tolerated, not migrated.
- **A regular-file blob's canonical file is a store path's own file** -- a *holder* -- and the store indexes (blob id, mode) to it, so the first entry writes each file once and pays no `linkat` a file; `.objects/blobs` keeps the symlink-target blobs alone. Taken on the condition the user set, which is met: the parallel first entry of a 40 000-file tree is **0.840 s against 0.840 s** for the route it replaces, twenty trials a label in each of two orders on a builder at load 0.54-2.10, with the ablation that writes no rows also at 0.840 (`06` section 6). The index's writer holds a database connection of its own, which is what meets it; the per-directory batched probe is deliberately not built, per-file probing costing nothing measurable.
- Physical accounting is kept; the sweep runs on whole-store collections only; an ingestion's hold is a locked directory, empty until locked.
- The evaluator's memory bound is on bytes, not a count; the fetcher cache's rows are a compact codec, not JSON.

## 3. Open, with the evidence

- **The working-directory accessor's attribute sources** (`01` section 13): for a dirty checkout the export-ignore decision still reads the index's `.gitattributes` and not an uncommitted one below the root. Consistent with what the branch does for names; changing it alters what a dirty-checkout fetch exports. The user's call.
- **The local overlay store** (`01` section 13, `05` section 7): the upper-layer sweep is built and pinned by unit tests; the functional suite needs an overlayfs mount inside a user namespace, which the build sandbox forbids, and has not run on this branch. `runs/overlay-suite-on-host.sh` runs it on a Linux host that allows unprivileged user namespaces; the verifier's object checks and the materialising copy remain gated off for the overlay until it does.
- **Windows**: by inspection only.
- **Evaluation memory**: deferral holds each object's bytes until the flush, so `nix eval --json` over many independent derivations peaks above master (`05` section 6, the write-buffer table). By design; whether the trade is acceptable is the user's.
- **The write path's retained memory**: about a kilobyte per object written beyond the buffer (`04` section 2); attributed in part to the NAR-hash thunk, the rest not attributed.

## 4. Experiments and their verdicts

The design space was explored by measurement; what was built and not kept is recorded so that no one rebuilds it without a new reason. Each verdict's numbers are in the section named.

| experiment | verdict | evidence |
|---|---|---|
| batching the memo's row writes in one transaction (before the codec change) | no measurable gain; the cost was the JSON encoding | `05` §6, the memo's writes |
| the fetcher cache's rows encoded without JSON | kept | `05` §6 |
| the memo's rows batched, in key order, with a page cache (after the codec change) | kept | `05` §6, the first entry taken apart |
| export-ignore filter as the identity when no source names it | kept | `05` §6, the filter's lookups |
| a commit's attributes read from the commit's tree, not the working directory | kept | `05` §6, the export-ignore paragraphs |
| sharding the object directories alone | neutral by itself; kept as the precondition of the parallel copy | `05` §6 |
| the copy's sibling directories on a bounded pool | kept: first entry under master with sharing off | `05` §6 |
| a blob's link on a second thread, unsharded | slower; reverted | `05` §6 |
| a clean file's bytes read from the object database | kept: the macOS first entry from minutes to seconds | `05` §6, where a first entry's bytes come from |
| carrying the naming walk's bytes to the copy walk | no wall time once the object-database read is on; not kept (branch `vibe-coding/lazy-store-source-carry`) | `05` §6 |
| the flush's coroutine, one hold per batch, the registered NAR hash as the store's walk | kept, with one correction: one hold *directory* per batch, one *ingestion* per path (`08` K16) | `05` §6, the write's wall time; `07`, the batch's row machinery |
| the holder index with a continuous writer on the store's connection | worse: writer and probe on one lock (branch `vibe-coding/lazy-store-holder-index-continuous-writer`) | `06` §5 |
| the holder index with the writer on its own connection | **kept**: meets the gate, 0.840 against 0.840 at twenty trials a label | `06` §6 |
| a per-directory batched index probe | unnecessary: per-file probing costs the copy nothing measurable | `06` §5 |

## 5. In flight, and how to resume it exactly

**The holder index**, branch `vibe-coding/lazy-store-holder-index` (a local branch; its tip is the worker's last commit). The design is taken and its gate is met (section 2); what is in flight is the merge quality that follows. Landed: one code path, the probe variants that were not shipped removed and the ablation kept as `_NIX_TEST_HOLDER_NO_ROWS` because it is what separates the writer's cost from everything else; two deterministic tests of the stale-row rule, each with a mutation; the two repair tests rewritten to pin what repair does to a holder; the schema convention checked and applied; and the documents — `08`'s invariants and durability points (K1–K4, K8, K9, K11, K13, K16, K17, sections 1.3, 1.4, 1.5, X2), `01` section 9.10's laws and *Where the objects live* and *One route*, section 10's decision entry, `04` section 1.7 and its verifier paragraph, `07`'s four rows, `05` sections 2, 4, 5, 6 and 7, and the manual's release note and `nix-store --optimise` page.

Landed with the tests: the unit tests and the **four** functional scripts that pinned the old representation, each rewritten with the same discriminating power and a forced defect quoted, none deleted without its replacement named and one renamed where its rule's domain narrowed; `--optimise`'s re-entry of a forgotten holder, which needed no new code, only its test; and two defects the reading found and fixed, a batch's paths sharing one hold's row machinery (which terminated `nix copy` of a closure) and the stale-row sweep sitting inside the overridable half of a collection. The gates are green and their counts are `05` section 5's, read from the runs' own logs.

Still owed, and it is now one item. The non-unity build of the five test packages and the sanitised store and fetchers runs (the parallel copy, the materialise and restore routes, the index's writer thread, the memo behind its lock) pass at this tree (`05` section 5). *The overlay run*: `runs/overlay-suite-on-host.sh` on a Linux host, which is the only thing that can say what the representation does to a merged mount; what is by reading there is now one limit, no cross-layer sharing of regular-file blobs, the stale rows having been moved out of the overridable half and pinned (`08` K13). `06-holder-index.md` section 7 lists the same items and shrinks as they land. When they have landed: cherry-pick the branch onto the rewrite branch's tip, run the gates, re-slice into the five commits (the slice rule is the directory: `packaging/`; `src/` and `maintainers/`; `tests/`; `doc/manual/`; `doc/lazy-store/`), and move the branch.

**The overlay suite** has run, on a Linux host with unprivileged user namespaces, at this tree: ten of its twelve tests pass and two fail, `local-overlay-store/gc-objects` (the upper-layer sweep, new on this branch) and `local-overlay-store/verify`. Those two are the next defects to fix; their output is in the host's `~/nix-lazy/build/meson-logs/testlog.txt` (the sections headed by the test's name), and nothing else is owed. How the run is made, since it took several attempts: `runs/overlay-suite-on-host.sh <host> <revision>` ships the revision and starts the build and the suite detached, logging to `~/nix-lazy/overlay-run.log`. The host needs `sysctl kernel.apparmor_restrict_unprivileged_userns=0` (Ubuntu; root; until reboot). The suite's own probe, `unshare --user --mount-proc true` in `tests/functional/common/vars.sh`, fails on a host whose `/proc` the kernel does not consider fully visible, for root as well, and then every test skips with exit 77; the tests themselves need only `unshare --mount --map-root-user` and an overlay mount, which work there, so the driver asserts `_canUseSandbox=1` after checking that mount. A run whose twelve tests all skip in a tenth of a second is the probe, not a result; a run whose tests fail with exit 127 is a build that did not finish (read `build exit=` in the log first). If an agent is denied `ssh` by policy, the user runs the driver and pastes the log's tail; `nix build --system aarch64-linux` through the remote builder is not affected by that policy.

**The user-visible decisions** in section 3 above wait for the user; nothing is blocked on them.

## 6. How to operate

- **Build** in a worktree: `nix develop -c sh -c 'meson setup build -Dpkg_config_path="$PKG_CONFIG_PATH"'`, then `nix develop -c ninja -C build`. Every `ninja`/`meson` invocation runs under `nix develop -c`; outside it they exit 127 and stale binaries lie.
- **Gates**: `05` section 2's command lines, one suite per line, the exit code captured before anything reads the output. The named functional list needs `NIX_REFERENCE_BIN` pointing at a Nix built from master. The unit binaries need `_NIX_TEST_UNIT_DATA`.
- **The second platform**: `nix build --system aarch64-linux .#nix-<lib>-tests.tests.run` for the five libraries and `.#nix-functional-tests`, through a remote builder; a derivation whose inputs did not change is already valid and prints nothing, so confirm identity with `nix path-info --derivation` before counting it as run. The per-file gcc build and ThreadSanitizer run are `05` section 5's two extra gates. A builder that has run out of disk fails builds at once with Nix's note about free space, and a test that died by a signal right after such an episode is the builder's state until a re-run says otherwise; free the disk (`nix-collect-garbage -d` there) and re-run before reading anything into it. A gate's exit code is read before anything acts on it; a branch move is never chained behind a gate.
- **Measurements**: `runs/` holds every driver `05` section 6 and `06` cite, parametrised by repository path, revisions and size; constants are the Linux builder's, macOS timings are directions only, and single 40 000-file runs on a machine with an endpoint-security agent are not comparable to each other (`05` section 6 says why).
- **Mutation table**: `LAZY_STORE_TREE=<worktree> runs/run-mutation.sh <rows>` inside `nix develop`; `runs/mutations.py check` reports anchors that no longer match the tree.
- **Rules for changing this code**: `05` section 8. The ones most often broken: a fix carries a test that failed first; a number is quoted from an artefact or marked; the documents carry no history, dates or commit hashes; report the whole diff, not a slice.
