# Differential harness for the lazy-store rewrite

The external check of `doc/lazy-store/04-derivation.md` section 0.2. It runs master's functional test
scripts, unchanged, under two modes and compares the store afterwards, which is
the check the specification's (C1) asks for. Nothing in Nix is modified; the
only change to the test tree is a hook in `doClearStore` that snapshots the
store before each clear, so tests that clear several times give several
comparison points. What it compares and what it has shown are in `doc/lazy-store/05-validation.md`.

Files:

- `prepare-tree.sh`: copies a `tests/functional` tree in repository shape
  (`<dest>/tests/functional`, `<dest>/scripts/nix-profile.sh.in`), generates
  what meson would (`common/subst-vars.sh`, `config.nix`), installs the hook.
- `run-one.sh`: runs one script exactly as `tests/functional/meson.build`
  does (`bash -x -e -u -o pipefail`, same environment variables), from a wiped
  test root, then takes the final snapshot. It defines what stdenv would
  (`NIX_STORE`, `shell`), pins the dates git records so commit hashes are
  functions of content, and re-executes itself under the mode's bash because
  macOS ships bash 3.2.
- `lib.sh`, `snapshot.py`: one snapshot = normalised `nix path-info --all
  --json --json-format 2` (registrationTime removed, store dir blanked,
  signatures reduced to key names), the store database (ValidPaths, Refs,
  DerivationOutputs, BuildTraceV3), the permanent roots, and the store
  directory listing; for the main store and for every chroot store found under
  the test root (`<dir>/nix/store`, `<dir>/nix/var/nix`), whose logical store
  directory is read from its own database. Two views: the loose view
  (`snap-*.txt`) compares build outputs, that is objects without a content
  address, by path and metadata but not by contents, and masks the hash of
  unregistered store-directory entries; the strict view (`snap-*.strict`)
  keeps everything. A snapshot never opens a store that has no database yet,
  never depends on the test's configuration, and records its own failures
  inside the snapshot. Since the one-address step (`01-specification.md`
  section 9.11) both views differ on every row of a store the branch wrote:
  the snapshot dumps `ValidPaths.hash`, which the branch renders
  `git:sha256:<hex>` and the reference `sha256:<hex>`, and every source path
  is renamed besides. The verdict for a branch-against-reference run is
  therefore the renaming views below; the loose and strict views remain
  the verdict for reference-against-reference runs.
- `differential.sh`: runs A then B per test; the verdict (SAME-PASS,
  SAME-SKIP, SAME-FAIL, DIFF) comes from the loose view, `content-differs`
  from the strict view, `stdout-differs` is informational; unified diffs are
  left beside the B-side snapshots.
- `summarize.sh`: counts per verdict from a summary TSV, last row per test.
- `name-multiset.py`: a third, hash-independent view over two runs of a
  suite: the multiset of object names with the hash prefix stripped. Two runs
  agree here exactly when they registered the same number of objects of each
  name, whatever the hashes. It cannot see content or references, so it
  strengthens a by-construction exclusion (a DIFF whose only cause is
  nondeterministic inputs should still be NAMES-EQUAL); it does not replace
  the snapshot views.
- `renamed-content.py`: a fourth view, for a run whose naming function differs
  from the reference's (the git naming of sources, `01-specification.md`
  sections 9.9 and 9.11; every source is git-named and no setting selects
  it), where every source path changes and the snapshot views cannot
  read anything. `lib.sh` writes `snap-*.<store>.renamed` beside each snapshot
  (`snapshot.py renamed`): one line per valid object with its name, the digest
  of its tree after the hash part of every embedded store path is masked,
  whether it is content-addressed, and its references' and deriver's names;
  input-addressed objects' bytes are masked as in the loose view. Two runs are
  RENAMED-EQUAL when every store snapshot has the same multiset of lines: the
  same objects with the same bytes up to a name-preserving renaming, in the
  same reference graph by name. Stronger than `name-multiset.py`, weaker than
  SAME. It is the verdict for a branch-against-reference run (above); for a
  reference-against-reference run the loose view is the verdict and this view
  is informational. Its strict form (`snapshot.py --strict
  renamed`, `snap-*.renamed-strict`, `renamed-content.py --strict`) digests
  build outputs' bytes too, so it also reads whether the builders wrote the
  same bytes up to the renaming.
- `c2-observe.py`: the (C2) observer for a single command; a second process
  checks each printed store path as the line arrives: registered in the
  store's database when `NIX_STATE_DIR` names one, else present on the
  filesystem, and the report says which check was used.
- `c2/run-c2.sh`, `c2/many-drvs.nix`: a driver for the observer. Runs one
  command from a fresh private store under `$C2_ROOT` (required), piping the
  stream that carries store paths (stdout, or stderr for `trace` and `throw`)
  through the observer while the command runs, and prints the present/absent
  tally; its arguments are a label, the nix package to run and the mode. The
  300-derivation list makes any serialisation of it longer than a pipe's stdio
  buffer, so bytes leave before the command ends. The `lazyraw`, `lazyeval`
  and `lazyjson` modes build a 4000-file git repository and print its
  `fetchGit` outPath, a mounted input the reference copies only after
  printing. Run it for the reference and the implementation; the
  implementation must never be absent where the reference is present, and
  may be present where the reference is absent.
- `modes/*.env.in`: the mode templates. A mode file sets `HARNESS_TREE`, the
  prepared tree; `HARNESS_NIX_PACKAGE`, the nix package whose `bin/` the tree's
  `bindir` points at; `HARNESS_BASH`, the bash that runs a script (5.x; macOS
  ships 3.2, so the scripts re-execute under it); `HARNESS_TOOLS_PATH`, a PATH
  prefix starting with that bash and then GNU coreutils, sed, grep, findutils,
  diffutils, gawk, tar, gzip, bzip2, xz and jq; `HARNESS_NIX_CONFIG`, an
  optional settings fragment applied through `NIX_CONFIG` after the files;
  `HARNESS_CLIENT_PACKAGE`, an optional nix package put first on PATH; and,
  for a daemon run, `HARNESS_DAEMON=1` with `HARNESS_DAEMON_PACKAGE` naming
  the daemon's package (the reference's when unset). `prepare-tree.sh` fills
  `@tree@`, `@nix@`, `@bash@` and `@tools@` from its arguments and writes one
  file per template into `<dest>/modes/`:
  - `reference.env`: the reference binary alone.
  - `reference-githash.env`: the reference with `extra-experimental-features =
    git-hashing`, the reference side of a branch-against-reference run
    (master parses `fixed:git:` content addresses only under that feature,
    and every source the branch adds is named that way; the branch answers
    the line with a one-line "has been stabilized" warning).
  - `branch.env`: the reference tree and bindir with the branch's `nix` first
    on PATH through `HARNESS_CLIENT_PACKAGE`, so the two sides differ only in
    the binary. `@branchNix@` is filled from `BRANCH_NIX` when it is set
    (`BRANCH_NIX=$(nix build .#nix-cli --print-out-paths)`) and otherwise
    left for one `sed` over the generated file.

  A daemon mode is one of these with `HARNESS_DAEMON=1` appended and, for the
  branch, `HARNESS_DAEMON_PACKAGE=<the branch package>`: the client forwards
  its settings to the daemon, and a daemon that does not know a setting warns
  on the client's stderr, which the characterisation tests compare. There is
  no mode for the write buffer: deferral is unconditional
  (`doc/lazy-store/01-specification.md` section 10), so the branch binary is
  the deferred side and the reference binary the eager one.

Typical use (`<tmp>` a resolved, short directory: the daemon socket path
must stay under the 104-byte limit on macOS, and `/tmp` there resolves to
`/private/tmp`, which is what the tests see, since the characterisation
tests normalise the working directory):

```
BRANCH_NIX=$(nix build .#nix-cli --print-out-paths) \
  prepare-tree.sh <repo>/tests/functional <tmp>/tree <reference nix>/bin <coreutils>/bin <bash> 2.36.0 aarch64-darwin
export HARNESS_TMPDIR=<tmp>
differential.sh <tmp>/tree/modes/reference.env refA <tmp>/tree/modes/reference.env refB main simple.sh dependencies.sh
differential.sh <tmp>/tree/modes/reference-githash.env reference <tmp>/tree/modes/branch.env branch main simple.sh
summarize.sh <tmp>/out/summary-refA-vs-refB-main.tsv
name-multiset.py <tmp>/out refA refB main
renamed-content.py <tmp>/out reference branch main
C2_ROOT=<tmp>/c2 c2/run-c2.sh reference <reference nix package> trace
C2_ROOT=<tmp>/c2 c2/run-c2.sh branch <branch nix package> trace
```

`differential.sh` and `run-one.sh` refuse to run without `HARNESS_TMPDIR`;
`c2/run-c2.sh` without `C2_ROOT`; the measurement scripts without
`MEASURE_ROOT`.
