# shellcheck shell=bash
#
# Differential harness, shared part: one snapshot of the observable store state.
#
# Sourced by run-one.sh (final snapshot after a test) and by the patched
# common/functions.sh of the prepared test tree (snapshot before each
# clearStore, so a test that clears the store several times yields several
# comparison points).  It uses only the environment the test itself has:
# TEST_ROOT, NIX_STORE_DIR, NIX_STATE_DIR, NIX_REMOTE, PATH.
#
# Rules the snapshot keeps:
# - it never creates state: a store whose database does not exist is reported
#   as such and not opened, because opening it would create the database and
#   the next run would inherit it;
# - it does not depend on the test's configuration: the new CLI is invoked with
#   the `nix-command` feature enabled explicitly;
# - a failure inside a snapshot is recorded *in* the snapshot file rather than
#   swallowed, so that a snapshot failing on one side and succeeding on the
#   other shows up as a difference;
# - besides the main store it snapshots every chroot store a test created under
#   TEST_ROOT (`--store <dir>` gives `<dir>/nix/store` and `<dir>/nix/var/nix`),
#   e.g. the eval store of eval-store.sh.

# Two views are written: `snap-<label>.txt` (loose: build outputs compared by
# path, not by contents) decides the verdict; `snap-<label>.strict` records the
# contents too and differences in it are reported as `content-differs`.  A
# third, `snap-<label>.<store>.renamed`, sees the store up to a name-preserving
# renaming of its paths (for a run whose naming function differs); it is
# compared by renamed-content.py and is not part of the verdict.  Its strict
# form, `.renamed-strict`, digests build outputs' bytes too.
harness_snapshot() {
    # Tests run under `set -x`; the trace would otherwise land in the snapshot
    # (stderr is captured below so that snapshot failures are recorded).
    local xtrace=0
    [[ $- == *x* ]] && xtrace=1
    set +x
    local label=$1
    local out=${HARNESS_OUT:?}
    local f=$out/snap-$label
    mkdir -p "$out"
    local view
    for view in loose strict; do
        {
            echo "## label: $label ($view view)"
            echo "## NIX_REMOTE: ${NIX_REMOTE-}"
            if [[ -e ${NIX_STATE_DIR:?}/db/db.sqlite ]]; then
                harness_snapshot_store "$view" main "" "${NIX_STORE_DIR:?}" "$NIX_STORE_DIR" "$NIX_STATE_DIR" "$f.main"
            else
                echo "## [main] no database: the main store was never opened"
            fi
            local db prefix logical
            while IFS= read -r db; do
                prefix=${db%/nix/var/nix/db/db.sqlite}
                [[ $prefix == "$db" ]] && continue
                # The chroot store's logical directory is whatever the test used when it wrote it.
                logical=$(python3 "${HARNESS_DIR:?}/snapshot.py" storedir "$db" 2> /dev/null || true)
                [[ -n $logical ]] || logical=${NIX_STORE_DIR:-/nix/store}
                harness_snapshot_store "$view" "chroot:${prefix#"${TEST_ROOT:?}"/}" "$prefix" "$logical" "$prefix/nix/store" "$prefix/nix/var/nix" \
                    "$f.chroot-$(echo "${prefix#"$TEST_ROOT"/}" | tr '/' '_')"
            done < <(find "${TEST_ROOT:?}" -path '*/nix/var/nix/db/db.sqlite' -not -path "${NIX_STATE_DIR}/*" 2> /dev/null | LC_ALL=C sort)
        } > "$f.$([[ $view == loose ]] && echo txt || echo strict)" 2>&1
    done
    [[ $xtrace == 1 ]] && set -x
    return 0
}

# harness_snapshot_store <view> <name> <store uri or empty for the ambient store> <logical store dir> <real store dir> <state dir> <file prefix>
# The raw observations (path-info JSON, roots) are taken once, on the loose pass, and re-rendered on the strict pass.
harness_snapshot_store() {
    local view=$1 name=$2 uri=$3 logical=$4 real=$5 statedir=$6 f=$7
    local storearg=() flag=()
    [[ -n $uri ]] && storearg=(--store "$uri")
    [[ $view == loose ]] && flag=(--loose)
    echo "## [$name] pathinfo (registrationTime removed, store dir blanked, signatures reduced to key names${flag[*]+, build outputs masked})"
    if [[ $view == loose ]]; then
        if ! NIX_STORE_DIR=$logical nix --extra-experimental-features nix-command ${storearg[@]+"${storearg[@]}"} path-info --all --json --json-format 2 > "$f.pathinfo.json" 2> "$f.pathinfo.err"; then
            rm -f "$f.pathinfo.json"
        fi
    fi
    if [[ $view == loose ]]; then
        # The renaming view (snapshot.py renamed): a side file, compared by renamed-content.py, not part of the verdict.
        NIX_STORE_DIR=$logical python3 "${HARNESS_DIR:?}/snapshot.py" renamed "$statedir/db/db.sqlite" "$real" > "$f.renamed" 2>&1 \
            || echo "SNAPSHOT-ERROR: renamed view failed" >> "$f.renamed"
        NIX_STORE_DIR=$logical python3 "${HARNESS_DIR:?}/snapshot.py" --strict renamed "$statedir/db/db.sqlite" "$real" > "$f.renamed-strict" 2>&1 \
            || echo "SNAPSHOT-ERROR: strict renamed view failed" >> "$f.renamed-strict"
    fi
    if [[ -e $f.pathinfo.json ]]; then
        NIX_STORE_DIR=$logical python3 "${HARNESS_DIR:?}/snapshot.py" ${flag[@]+"${flag[@]}"} pathinfo "$f.pathinfo.json" \
            || echo "SNAPSHOT-ERROR: pathinfo normalisation failed"
    else
        echo "SNAPSHOT-ERROR: nix path-info --all failed:"
        cat "$f.pathinfo.err" 2> /dev/null
    fi
    echo "## [$name] db (ValidPaths minus registrationTime, Refs, DerivationOutputs, BuildTraceV3)"
    NIX_STORE_DIR=$logical python3 "${HARNESS_DIR:?}/snapshot.py" ${flag[@]+"${flag[@]}"} db "$statedir/db/db.sqlite" \
        || echo "SNAPSHOT-ERROR: database dump failed"
    echo "## [$name] roots (nix-store --gc --print-roots, sorted)"
    if [[ $view == loose ]]; then
        if ! NIX_STORE_DIR=$logical nix-store ${storearg[@]+"${storearg[@]}"} --gc --print-roots > "$f.roots.raw" 2> "$f.roots.err"; then
            rm -f "$f.roots.raw"
        fi
    fi
    if [[ -e $f.roots.raw ]]; then
        LC_ALL=C sort "$f.roots.raw"
    else
        echo "SNAPSHOT-ERROR: print-roots failed:"
        cat "$f.roots.err" 2> /dev/null
    fi
    echo "## [$name] storedir (sorted listing; unregistered entries${flag[*]+ with the hash part masked})"
    python3 "${HARNESS_DIR:?}/snapshot.py" ${flag[@]+"${flag[@]}"} listing "$statedir/db/db.sqlite" "$real" \
        || echo "SNAPSHOT-ERROR: store directory listing failed"
}
